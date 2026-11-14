// The GPU's work split in two threads: see deferred_sink.h.
#include "gpu/deferred_sink.h"

#include "xenoforge/bytes.h"
#include "xenoforge/resolve.h"

#include "draw_sections.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace xf::gpu {

namespace {

// set for good on the back end's thread, and around a call passed to the sink on another
thread_local int passing_depth = 0;
thread_local bool back_end_thread = false;

struct Passing {
    Passing() { passing_depth++; }
    ~Passing() { passing_depth--; }
};

inline void pause() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

constexpr uint32_t ALIGN = 8;
constexpr uint32_t aligned(uint32_t n) { return (n + ALIGN - 1) & ~(ALIGN - 1); }
// The front end waiting for the back end spins about this many pauses before it sleeps: the
// back end's catching up usually comes sooner than a sleep and a wake-up take.
constexpr uint32_t SPINS = 4096;
// The back end with nothing to do sleeps at once, by default: on a trace's replay, where the
// records come between the walk's own work, 4,096 pauses first cost all threads a sixth more
// processor time (399 against 345 ms a play) for nothing on the command thread within the
// noise (9.5 against 10.1 ms). XF_GPU_BACK_END_SPINS=<n> spins first, for timing live.
uint32_t back_spins() {
    static const uint32_t n = [] {
        const char *v = std::getenv("XF_GPU_BACK_END_SPINS");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    return n;
}

} // namespace

bool DeferredSink::passing() { return passing_depth > 0; }
bool DeferredSink::on_back_end() { return back_end_thread; }

DeferredSink::DeferredSink(Sink &back, RegisterFile &front, Memory &memory)
    : back_(back), front_(front), memory_(memory), view_(memory.translate_physical(0)), ring_(new uint8_t[RING]) {
    front_.track_blocks(true);
    thread_ = std::thread([this] { run(); });
}

DeferredSink::~DeferredSink() {
    finish();
    // (a record of nothing handed over after the stop: the back end, awake or asleep, sees it)
    stop_.store(true, std::memory_order_seq_cst);
    reserve(Kind::Wrap, 0);
    commit();
    thread_.join();
    front_.track_blocks(false);
}

DeferredSink::Stats DeferredSink::stats() const {
    Stats s = stats_;
    s.finishes = finishes_.load(std::memory_order_relaxed);
    return s;
}

// ---- the front end ----

uint8_t *DeferredSink::reserve(Kind kind, uint32_t bytes) {
    const uint32_t size = aligned(sizeof(Header) + bytes);
    uint64_t at = written_.load(std::memory_order_relaxed);
    // (a record never runs past the ring's end: the rest skipped with a Wrap where it would)
    const size_t offset = at & (RING - 1);
    uint64_t need = size;
    if (offset + size > RING) need += RING - offset;
    // (done_ read again only where the last value read leaves no room: its line stays the back
    // end's, which stores it at every record)
    if (at + need - done_seen_ > RING && at + need - (done_seen_ = done_.load(std::memory_order_acquire)) > RING) {
        stats_.full++;
        wait_done(at + need - RING, nullptr, WAIT_FULL);
    }
    if (offset + size > RING) {
        Header wrap{Kind::Wrap, uint32_t(RING - offset - sizeof(Header))};
        std::memcpy(&ring_[offset], &wrap, sizeof(wrap));
        at += RING - offset;
    }
    Header h{kind, bytes};
    std::memcpy(&ring_[at & (RING - 1)], &h, sizeof(h));
    record_at_ = at;
    reserved_ = at + size;
    stats_.records++, stats_.bytes += size;
    return &ring_[(at & (RING - 1)) + sizeof(Header)];
}

void DeferredSink::commit() {
    written_.store(reserved_, std::memory_order_seq_cst);
    // (the back end announces its sleep before it looks at written_ again: one of the two sees the other)
    if (back_asleep_.load(std::memory_order_seq_cst)) written_.notify_one();
}

void DeferredSink::push(Kind kind, const void *payload, uint32_t bytes) {
    push_registers();
    uint8_t *p = reserve(kind, bytes);
    if (bytes) std::memcpy(p, payload, bytes);
    commit();
}

void DeferredSink::push_registers() {
    // the blocks written since the last record (all of them after a time synchronous); a block's
    // registers as they stand now, which is what the processor's calls would have seen
    // A block goes as its number, a mask of the registers in it sent (bit k: its register k) and
    // their values in order: the registers the processor changed since the last record, all of
    // them for a block another thread wrote (or after a time synchronous). Game G's town changes
    // about 4 of a block's 32 registers a record: whole blocks were six times the bytes, copied
    // by the front end and compared again by the back end. XF_SPARSE_BLOCKS=0: whole blocks.
    std::vector<BlockBits> &blocks = blocks_;
    blocks.clear();
    uint32_t words = 0;
    if (resync_) {
        front_.take_blocks([](uint32_t, uint32_t) {});
        for (uint32_t b = 0; b < (RegisterFile::COUNT + RegisterFile::BLOCK - 1) / RegisterFile::BLOCK; b++)
            blocks.push_back({b, RegisterFile::block_bits(b)});
        resync_ = false;
    } else {
        front_.take_blocks([&](uint32_t b, uint32_t bits) { blocks.push_back({b, sparse_blocks_ ? bits : ~0u}); });
    }
    for (BlockBits &b : blocks) words += uint32_t(__builtin_popcount(b.bits &= RegisterFile::block_bits(b.block)));
    const uint64_t foreign = front_.foreign_writes();
    if (blocks.empty() && foreign == foreign_sent_) return;
    foreign_sent_ = foreign;
    uint8_t *p = reserve(Kind::Blocks, 8 + uint32_t(blocks.size()) * 8 + words * 4);
    std::memcpy(p, &foreign, 8);
    p += 8;
    const uint32_t *values = front_.data();
    for (const BlockBits &b : blocks) {
        const uint32_t *from = values + b.block * RegisterFile::BLOCK;
        std::memcpy(p, &b, 8);
        p += 8;
        // (relaxed loads in effect: the CPU's MMIO may write one meanwhile, and its mark brings it again)
        if (b.bits == ~0u) {
            std::memcpy(p, from, RegisterFile::BLOCK * 4);
            p += RegisterFile::BLOCK * 4;
        } else {
            for (uint32_t m = b.bits; m; m &= m - 1, p += 4) {
                const uint32_t v = __atomic_load_n(from + __builtin_ctz(m), __ATOMIC_RELAXED);
                std::memcpy(p, &v, 4);
            }
        }
    }
    stats_.blocks += blocks.size();
    commit();
}

void DeferredSink::push_draw(Kind kind, const Draw &d) {
    push_registers();
    const uint32_t n = uint32_t(d.immediate.size());
    uint8_t *p = reserve(kind, sizeof(DrawRecord) + n * 4);
    DrawRecord r{d.initiator,
                 d.primitive,
                 d.source,
                 d.count,
                 d.viz_query,
                 d.index_base,
                 d.index_bytes,
                 n,
                 uint8_t(d.major_mode_explicit),
                 uint8_t(d.indexed),
                 uint8_t(d.index32),
                 uint8_t(d.index_endian)};
    std::memcpy(p, &r, sizeof(r));
    if (n) std::memcpy(p + sizeof(r), d.immediate.data(), n * 4);
    commit();
}

void DeferredSink::wait_done(uint64_t position, uint64_t *counter, size_t why) {
    if (done_.load(std::memory_order_acquire) >= position) return;
    if (counter) ++*counter;
    stats_.waits++;
    const auto began = std::chrono::steady_clock::now();
    for (uint32_t spins = 0;; spins++) {
        uint64_t done = done_.load(std::memory_order_seq_cst);
        if (done >= position) break;
        if (spins < SPINS) {
            pause();
            continue;
        }
        stats_.sleeps++;
        sleep_until(position);
    }
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    stats_.wait_seconds += waited;
    stats_.wait_by[why] += waited;
}

void DeferredSink::finish() {
    if (passing()) return; // (the back end itself, or a call it passed on: everything before it is done)
    const uint64_t position = written_.load(std::memory_order_acquire);
    if (done_.load(std::memory_order_acquire) >= position) return;
    finishes_.fetch_add(1, std::memory_order_relaxed);
    // (from another thread than the processor's the stats' time is not added: wait_done's is the front end's)
    for (uint32_t spins = 0;; spins++) {
        uint64_t done = done_.load(std::memory_order_seq_cst);
        if (done >= position) return;
        if (spins < SPINS) {
            pause();
            continue;
        }
        sleep_until(position);
    }
}

// Asleep until the back end has done everything before position. The sleeper puts the position
// it waits for in wanted_ (the lowest of any two sleepers' stays), and the back end wakes the
// sleepers once it gets there: woken at every record it did, a front end waiting 9 ms of a frame
// for the back end made a system call and a wake-up a record, and as much processor time as the
// wait itself (game G's town, the player). A sleeper woken short of its position (another's was
// reached) puts its own back and sleeps again.
void DeferredSink::sleep_until(uint64_t position) {
    for (;;) {
        uint64_t want = wanted_.load(std::memory_order_seq_cst);
        while (position < want && !wanted_.compare_exchange_weak(want, position, std::memory_order_seq_cst)) {
        }
        // (after the position is put: the back end's done_ then wanted_, this one's wanted_ then done_)
        const uint64_t done = done_.load(std::memory_order_seq_cst);
        if (done >= position) return;
        done_.wait(done, std::memory_order_seq_cst);
        if (done_.load(std::memory_order_seq_cst) >= position) return;
    }
}

void DeferredSink::set_synchronous(bool on) {
    if (on == synchronous_) return;
    if (on) {
        wait_done(written_.load(std::memory_order_relaxed), nullptr, WAIT_FULL);
    } else {
        resync_ = true; // the back end's copy of the registers brought up to date in full
    }
    synchronous_ = on;
}

void DeferredSink::sync(SyncFor why, uint32_t address, uint32_t bytes) {
    const size_t w = size_t(why);
    stats_.syncs[w]++;
    if (synchronous_) return;
    if (why != SyncFor::MemoryRead) return wait_done(written_.load(std::memory_order_relaxed), &stats_.waited[w], w);
    // A memory read waits only for the work yet to be done that writes where it reads: a
    // resolve's destination, a fence written later. What else that work may write (memexport)
    // the console's command processor would not wait for either without a fence or a wait
    // before the read, which sync.
    const uint64_t done = done_.load(std::memory_order_acquire);
    uint64_t until = 0;
    const uint64_t end = uint64_t(address) + bytes;
    size_t kept = 0;
    for (const Pending &p : pending_) {
        if (p.position <= done) continue;
        if (address < p.end && end > p.start) until = std::max(until, p.position);
        pending_[kept++] = p;
    }
    pending_.resize(kept);
    if (until) wait_done(until, &stats_.waited[w], w);
}

void DeferredSink::add_pending(uint32_t start, uint32_t end) {
    // (written_ is the record's end once it is committed: the caller's record is the last)
    if (pending_.size() >= 4096) {
        const uint64_t done = done_.load(std::memory_order_acquire);
        std::erase_if(pending_, [&](const Pending &p) { return p.position <= done; });
    }
    pending_.push_back({start, end, written_.load(std::memory_order_relaxed)});
}

bool DeferredSink::write_later(const LateWrite &write) {
    if (synchronous_) return false;
    push(Kind::LateWrite, &write, sizeof(write));
    add_pending(write.address, write.address + 4);
    stats_.late_writes++;
    return true;
}

void DeferredSink::memory_written(uint32_t address, uint32_t bytes) {
    if (synchronous_) {
        Passing p;
        return back_.memory_written(address, bytes);
    }
    const uint32_t v[2] = {address, bytes};
    push(Kind::MemoryWritten, v, sizeof(v));
}

void DeferredSink::shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) {
    if (synchronous_) {
        Passing p;
        return back_.shader(type, address, code);
    }
    push_registers();
    const uint32_t n = uint32_t(code.size());
    uint8_t *p = reserve(Kind::Shader, 12 + n * 4);
    const uint32_t head[3] = {type, address, n};
    std::memcpy(p, head, 12);
    if (n) std::memcpy(p + 12, code.data(), n * 4);
    commit();
}

// IM_LOAD's words are the back end's to read (it asks the sink whether it has them, and reads
// them from memory if not): the front end never copies them.
bool DeferredSink::shader_again(uint32_t type, uint32_t address, uint32_t dwords) {
    if (synchronous_) {
        Passing p;
        return back_.shader_again(type, address, dwords);
    }
    const uint32_t v[3] = {type, address, dwords};
    push(Kind::ImLoad, v, sizeof(v));
    return true;
}

void DeferredSink::draw(const RegisterFile &registers, const Draw &d) {
    if (synchronous_) {
        Passing p;
        return back_.draw(registers, d);
    }
    push_draw(Kind::Draw, d);
}

void DeferredSink::resolve(const RegisterFile &registers, const Draw &d) {
    if (synchronous_) {
        Passing p;
        return back_.resolve(registers, d);
    }
    push_draw(Kind::Resolve, d);
    // where it writes guest memory, worked out from the registers as the back end will (all of
    // memory where that cannot be told; nothing for a resolve the back end will skip, or a clear)
    resolve::Info info;
    std::string why;
    if (!resolve::get_info({front_.data(), RegisterFile::COUNT}, view_, info, why) || !info.copy) return;
    written_ranges_.clear(), partial_ranges_.clear();
    if (!resolve::destination_ranges(info, written_ranges_, partial_ranges_) || written_ranges_.empty())
        return add_pending(0, UINT32_MAX);
    add_pending(written_ranges_.front().first, written_ranges_.back().second);
}

void DeferredSink::swap(const RegisterFile &registers, const Swap &s) {
    if (synchronous_) {
        Passing p;
        return back_.swap(registers, s);
    }
    push(Kind::Swap, &s, sizeof(s));
}

bool DeferredSink::counts_samples() {
    Passing p;
    return back_.counts_samples();
}

void DeferredSink::zpd_report(uint64_t ended, uint64_t next) {
    if (synchronous_) {
        Passing p;
        return back_.zpd_report(ended, next);
    }
    const uint64_t v[2] = {ended, next};
    push(Kind::ZpdReport, v, sizeof(v));
}

// (the counts in so far: those of reports the back end has not reached yet come at a later poll)
void DeferredSink::zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) {
    Passing p;
    back_.zpd_poll(resolved);
}

bool DeferredSink::measures_extents() {
    Passing p;
    return back_.measures_extents();
}

void DeferredSink::extent_init() {
    if (synchronous_) {
        Passing p;
        return back_.extent_init();
    }
    push(Kind::ExtentInit, nullptr, 0);
}

// A handle of ours at once; the back end matches the sink's to it as the sink reports, and one
// the sink does not bound comes back as the whole screen.
uint64_t DeferredSink::extent_report() {
    if (synchronous_) {
        Passing p;
        return back_.extent_report();
    }
    const uint64_t handle = extent_next_++;
    push(Kind::ExtentReport, &handle, sizeof(handle));
    return handle;
}

void DeferredSink::extent_wait() {
    if (!synchronous_) wait_done(written_.load(std::memory_order_relaxed), nullptr, WAIT_EXTENTS);
    Passing p;
    back_.extent_wait();
}

void DeferredSink::extent_poll(std::vector<Extent> &resolved) {
    Passing p;
    if (synchronous_ && extent_first_ == extent_handles_.size() && extent_whole_.empty())
        return back_.extent_poll(resolved);
    std::lock_guard<std::mutex> lock(extents_mutex_);
    extent_scratch_.clear();
    back_.extent_poll(extent_scratch_);
    for (Extent e : extent_scratch_) {
        // (the sink's answers in its reports' order, mostly: looked for from the one after the
        // last found, then from the first still open)
        const size_t n = extent_handles_.size(), from = std::max(extent_cursor_, extent_first_);
        size_t at = from;
        while (at < n && extent_handles_[at].first != e.handle) at++;
        if (at == n) {
            for (at = extent_first_; at < from && extent_handles_[at].first != e.handle;) at++;
            if (at == from) at = n;
        }
        if (at == n) {
            // (one reported while synchronous, under the sink's own handle)
            if (synchronous_) resolved.push_back(e);
            continue;
        }
        e.handle = extent_handles_[at].second;
        extent_handles_[at].first = 0;
        extent_cursor_ = at + 1;
        while (extent_first_ < n && !extent_handles_[extent_first_].first) extent_first_++;
        if (extent_first_ == n) extent_handles_.clear(), extent_first_ = extent_cursor_ = 0;
        resolved.push_back(e);
    }
    resolved.insert(resolved.end(), extent_whole_.begin(), extent_whole_.end());
    extent_whole_.clear();
}

double DeferredSink::back_busy_seconds() const {
    return FastClock::seconds(back_busy_.load(std::memory_order_relaxed));
}

// The sink's, with the front end's waits for the back end and the back end's own time (on the
// processor's thread: the front end's figures are its own)
Sink::Work DeferredSink::work() {
    Work w;
    {
        Passing p;
        w = back_.work();
    }
    for (size_t i = 0; i < Work::BACK_END_WAITS; i++) w.back_end_wait_seconds[i] = stats_.wait_by[i];
    w.back_end_waits = stats_.waits, w.back_end_sleeps = stats_.sleeps;
    w.back_end_busy_seconds = FastClock::seconds(back_busy_.load(std::memory_order_relaxed));
    w.back_end_cpu_seconds = double(back_cpu_.ticks()) / cpu_ticks_a_second();
    return w;
}

void DeferredSink::viz_query(uint32_t id, bool end) {
    if (synchronous_) {
        Passing p;
        return back_.viz_query(id, end);
    }
    const uint32_t v[2] = {id, end ? 1u : 0u};
    push(Kind::VizQuery, v, sizeof(v));
}

// ---- the back end ----

void DeferredSink::run() {
    passing_depth = 1;
    back_end_thread = true;
    back_cpu_.bind_current();
    gpu_back_end_os_thread.store(back_cpu_.os_id());
    uint64_t at = 0;
    uint32_t fenced = 0, records = 0;
    for (;;) {
        uint64_t written = written_.load(std::memory_order_acquire);
        if (at == written) {
            // (relaxed: a sleeper the records' looks missed is woken before the back end waits)
            if (ring_relaxed_) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (at >= wanted_.load(std::memory_order_seq_cst) &&
                    wanted_.exchange(UINT64_MAX, std::memory_order_seq_cst) != UINT64_MAX)
                    done_.notify_all();
            }
            // nothing to do: a spin, then a sleep until the front end hands more over
            for (uint32_t spins = 0; spins < back_spins() && at == written; spins++) {
                pause();
                written = written_.load(std::memory_order_acquire);
            }
            if (at == written) {
                if (stop_.load(std::memory_order_seq_cst)) return;
                back_asleep_.store(true, std::memory_order_seq_cst);
                written = written_.load(std::memory_order_seq_cst);
                if (at == written && !stop_.load(std::memory_order_seq_cst))
                    written_.wait(written, std::memory_order_seq_cst);
                back_asleep_.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        // (the busy time added at each record, not at the batch's end: a batch can last a frame)
        uint64_t busy_from = FastClock::now();
        while (at < written) {
            Header h;
            std::memcpy(&h, &ring_[at & (RING - 1)], sizeof(h));
            if (h.kind != Kind::Wrap) {
                // (XF_DRAW_SECTIONS=1: the time by the kind of record, beside the draws' sections)
                const uint64_t t0 = DrawSections::on() ? FastClock::now() : 0;
                handle(h.kind, &ring_[(at & (RING - 1)) + sizeof(Header)], h.bytes);
                if (t0) {
                    using S = DrawSections;
                    const S::BackKind k = h.kind == Kind::Blocks    ? S::BackBlocks
                                          : h.kind == Kind::Draw    ? S::BackDraw
                                          : h.kind == Kind::Resolve ? S::BackResolve
                                          : h.kind == Kind::Swap    ? S::BackSwap
                                          : h.kind == Kind::Shader || h.kind == Kind::ImLoad ? S::BackShader
                                                                                              : S::BackOther;
                    DrawSections::get().back(k, FastClock::now() - t0);
                }
            }
            at += aligned(sizeof(Header) + h.bytes);
            // the sleepers woken once the position the lowest of them waits for is done. A full
            // fence between done_'s store and wanted_'s load (a sleeper puts wanted_, then reads
            // done_) at every record cost the back end a twelfth of its time: the fence waits for
            // the draw's write-combined uploads to leave. Relaxed, a sleeper that put wanted_ as
            // done_ was stored is seen at a later record's look, at the latest at the fenced one
            // every 16 records or before the back end sleeps.
            if (ring_relaxed_) {
                done_.store(at, std::memory_order_release);
                if ((++fenced & 15) == 0) std::atomic_thread_fence(std::memory_order_seq_cst);
                if (at >= wanted_.load(std::memory_order_relaxed) &&
                    wanted_.exchange(UINT64_MAX, std::memory_order_seq_cst) != UINT64_MAX)
                    done_.notify_all();
            } else {
                done_.store(at, std::memory_order_seq_cst);
                if (at >= wanted_.load(std::memory_order_seq_cst) &&
                    wanted_.exchange(UINT64_MAX, std::memory_order_seq_cst) != UINT64_MAX)
                    done_.notify_all();
            }
            // (the clock read every 8 records and at the batch's end, not at each)
            if ((++records & 7) == 0 || at == written) {
                const uint64_t now = FastClock::now();
                back_busy_.store(back_busy_.load(std::memory_order_relaxed) + (now - busy_from),
                                 std::memory_order_relaxed);
                busy_from = now;
            }
        }
    }
}

void DeferredSink::handle(Kind kind, const uint8_t *p, uint32_t bytes) {
    switch (kind) {
    case Kind::Wrap: break;
    case Kind::Blocks: {
        uint64_t foreign;
        std::memcpy(&foreign, p, 8);
        for (const uint8_t *q = p + 8, *end = p + bytes; q < end;) {
            BlockBits b;
            std::memcpy(&b, q, 8);
            q += 8;
            // (the words read where the ring holds them: a record's payload, and so each block's
            // words, lies on a word's boundary)
            const uint32_t *values = reinterpret_cast<const uint32_t *>(q);
            if (b.bits == ~0u)
                copy_.write_host(b.block * RegisterFile::BLOCK, values, RegisterFile::BLOCK);
            else
                copy_.write_sparse(b.block, b.bits, values);
            q += 4 * __builtin_popcount(b.bits);
        }
        copy_.set_foreign_writes(foreign);
        break;
    }
    case Kind::Draw:
    case Kind::Resolve: {
        DrawRecord r;
        std::memcpy(&r, p, sizeof(r));
        Draw &d = draw_;
        d.initiator = r.initiator, d.primitive = r.primitive, d.source = r.source, d.count = r.count;
        d.viz_query = r.viz_query, d.index_base = r.index_base, d.index_bytes = r.index_bytes;
        d.major_mode_explicit = r.major_mode_explicit, d.indexed = r.indexed, d.index32 = r.index32;
        d.index_endian = Endian(r.index_endian);
        d.immediate.resize(r.immediate);
        if (r.immediate) std::memcpy(d.immediate.data(), p + sizeof(r), r.immediate * 4);
        if (kind == Kind::Draw)
            back_.draw(copy_, d);
        else
            back_.resolve(copy_, d);
        break;
    }
    case Kind::Swap: {
        Swap s;
        std::memcpy(&s, p, sizeof(s));
        back_.swap(copy_, s);
        break;
    }
    case Kind::Shader: {
        uint32_t head[3];
        std::memcpy(head, p, 12);
        words_.resize(head[2]);
        if (head[2]) std::memcpy(words_.data(), p + 12, head[2] * 4);
        back_.shader(head[0], head[1], words_);
        break;
    }
    case Kind::ImLoad: {
        uint32_t v[3];
        std::memcpy(v, p, sizeof(v));
        if (back_.shader_again(v[0], v[1], v[2])) break;
        // (as the processor's IM_LOAD copies them)
        words_.resize(v[2]);
        for (uint32_t i = 0; i < v[2]; i++) words_[i] = be32(memory_.translate_physical(v[1] + 4 * i));
        back_.shader(v[0], v[1], words_);
        break;
    }
    case Kind::MemoryWritten: {
        uint32_t v[2];
        std::memcpy(v, p, sizeof(v));
        back_.memory_written(v[0], v[1]);
        break;
    }
    case Kind::VizQuery: {
        uint32_t v[2];
        std::memcpy(v, p, sizeof(v));
        back_.viz_query(v[0], v[1] != 0);
        break;
    }
    case Kind::ZpdReport: {
        uint64_t v[2];
        std::memcpy(v, p, sizeof(v));
        back_.zpd_report(v[0], v[1]);
        break;
    }
    case Kind::ExtentInit: back_.extent_init(); break;
    case Kind::ExtentReport: {
        uint64_t ours;
        std::memcpy(&ours, p, sizeof(ours));
        std::lock_guard<std::mutex> lock(extents_mutex_);
        const uint64_t theirs = back_.extent_report();
        if (theirs)
            extent_handles_.push_back({theirs, ours});
        else
            extent_whole_.push_back(Extent{ours, true, 0, 0, 0, 0});
        break;
    }
    case Kind::LateWrite: {
        LateWrite w;
        std::memcpy(&w, p, sizeof(w));
        std::memcpy(w.where, &w.word, 4);
        back_.memory_written(w.address, 4);
        if (w.written) w.written(w.context);
        break;
    }
    }
}

} // namespace xf::gpu
