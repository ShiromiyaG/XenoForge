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
    if (at + need - done_.load(std::memory_order_acquire) > RING) {
        stats_.full++;
        wait_done(at + need - RING, nullptr);
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
    std::vector<uint32_t> &blocks = blocks_;
    blocks.clear();
    if (resync_) {
        front_.take_blocks([](uint32_t) {});
        for (uint32_t b = 0; b < (RegisterFile::COUNT + RegisterFile::BLOCK - 1) / RegisterFile::BLOCK; b++)
            blocks.push_back(b);
        resync_ = false;
    } else {
        front_.take_blocks([&](uint32_t b) { blocks.push_back(b); });
    }
    const uint64_t foreign = front_.foreign_writes();
    if (blocks.empty() && foreign == foreign_sent_) return;
    foreign_sent_ = foreign;
    const uint32_t each = 4 + RegisterFile::BLOCK * 4;
    uint8_t *p = reserve(Kind::Blocks, 8 + uint32_t(blocks.size()) * each);
    std::memcpy(p, &foreign, 8);
    p += 8;
    const uint32_t *values = front_.data();
    for (uint32_t b : blocks) {
        const uint32_t first = b * RegisterFile::BLOCK;
        const uint32_t n = std::min(RegisterFile::BLOCK, RegisterFile::COUNT - first);
        std::memcpy(p, &b, 4);
        // (relaxed loads in effect: the CPU's MMIO may write one meanwhile, and its mark brings it again)
        std::memcpy(p + 4, values + first, n * 4);
        p += each;
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

void DeferredSink::wait_done(uint64_t position, uint64_t *counter) {
    if (done_.load(std::memory_order_acquire) >= position) return;
    if (counter) ++*counter;
    const auto began = std::chrono::steady_clock::now();
    for (uint32_t spins = 0;; spins++) {
        uint64_t done = done_.load(std::memory_order_seq_cst);
        if (done >= position) break;
        if (spins < SPINS) {
            pause();
            continue;
        }
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        done = done_.load(std::memory_order_seq_cst);
        if (done < position) done_.wait(done, std::memory_order_seq_cst);
        waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
    stats_.wait_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
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
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        done = done_.load(std::memory_order_seq_cst);
        if (done < position) done_.wait(done, std::memory_order_seq_cst);
        waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void DeferredSink::set_synchronous(bool on) {
    if (on == synchronous_) return;
    if (on) {
        wait_done(written_.load(std::memory_order_relaxed), nullptr);
    } else {
        resync_ = true; // the back end's copy of the registers brought up to date in full
    }
    synchronous_ = on;
}

void DeferredSink::sync(SyncFor why, uint32_t address, uint32_t bytes) {
    const size_t w = size_t(why);
    stats_.syncs[w]++;
    if (synchronous_) return;
    if (why != SyncFor::MemoryRead) return wait_done(written_.load(std::memory_order_relaxed), &stats_.waited[w]);
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
    if (until) wait_done(until, &stats_.waited[w]);
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
    if (!synchronous_) wait_done(written_.load(std::memory_order_relaxed), nullptr);
    Passing p;
    back_.extent_wait();
}

void DeferredSink::extent_poll(std::vector<Extent> &resolved) {
    Passing p;
    if (synchronous_ && extent_handles_.empty() && extent_whole_.empty()) return back_.extent_poll(resolved);
    std::lock_guard<std::mutex> lock(extents_mutex_);
    extent_scratch_.clear();
    back_.extent_poll(extent_scratch_);
    for (Extent e : extent_scratch_) {
        auto it = extent_handles_.find(e.handle);
        if (it == extent_handles_.end()) {
            // (one reported while synchronous, under the sink's own handle)
            if (synchronous_) resolved.push_back(e);
            continue;
        }
        e.handle = it->second;
        extent_handles_.erase(it);
        resolved.push_back(e);
    }
    resolved.insert(resolved.end(), extent_whole_.begin(), extent_whole_.end());
    extent_whole_.clear();
}

Sink::Work DeferredSink::work() {
    Passing p;
    return back_.work();
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
    back_cpu_.bind_current();
    gpu_back_end_os_thread.store(back_cpu_.os_id());
    uint64_t at = 0;
    for (;;) {
        uint64_t written = written_.load(std::memory_order_acquire);
        if (at == written) {
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
            done_.store(at, std::memory_order_seq_cst);
            if (waiters_.load(std::memory_order_seq_cst)) done_.notify_all();
        }
    }
}

void DeferredSink::handle(Kind kind, const uint8_t *p, uint32_t bytes) {
    switch (kind) {
    case Kind::Wrap: break;
    case Kind::Blocks: {
        uint64_t foreign;
        std::memcpy(&foreign, p, 8);
        const uint32_t each = 4 + RegisterFile::BLOCK * 4;
        for (const uint8_t *q = p + 8, *end = p + bytes; q < end; q += each) {
            uint32_t b;
            std::memcpy(&b, q, 4);
            const uint32_t first = b * RegisterFile::BLOCK;
            // (the words read where the ring holds them: a record's payload, and so each block's
            // words, lies on a word's boundary)
            copy_.write_host(first, reinterpret_cast<const uint32_t *>(q + 4),
                             std::min(RegisterFile::BLOCK, RegisterFile::COUNT - first));
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
            extent_handles_[theirs] = ours;
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
