// The GPU's command processor, live (plan step A4, "GPU live";
// docs/runtime.md, "The GPU"): the ring buffer the CPU fills and the
// indirect buffers it points at, executed packet by packet with their
// effects on the GPU's registers and on guest memory, as Xenia's
// CommandProcessor executes them (src/xenia/gpu/command_processor.cc,
// pm4_command_processor_implement.h, graphics_system.cc's registers; BSD,
// see LICENSE-xenia): register writes and the scratch write-back, fences
// (MEM_WRITE, EVENT_WRITE_SHD), waits on memory or a register, interrupts,
// the display's gamma ramp, occlusion query reports.
//
// It draws nothing. Draws, resolves, swaps and shader loads go to a Sink
// with the register file standing as they happen: the renderer
// (renderer.h), or nothing (the headless backend is Xenia's null command
// processor: every draw and copy succeeds and leaves no picture). A trace
// is replayed through it too, packet by packet, as Xenia's trace player
// feeds its command processor (render::replay).
//
// No kernel here: guest memory is Memory's physical view, and what the
// processor needs from the system (an interrupt on a CPU, a wait between
// polls) comes through Host. One thread executes packets; the register
// file may be read and written from others (the CPU's MMIO), as on the
// console.

#pragma once

#include "xenoforge/memory.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace xf::gpu {

// How the GPU swaps a word it reads or writes in memory: the low 2 bits of
// the address a packet gives (Xenia's xenos::Endian and GpuSwap).
enum class Endian : uint32_t { None = 0, In16 = 1, In32 = 2, Half = 3 };
constexpr uint32_t gpu_swap(uint32_t v, Endian e) {
    switch (e) {
    case Endian::In16: return (v << 8 & 0xFF00FF00) | (v >> 8 & 0x00FF00FF);
    case Endian::In32: return __builtin_bswap32(v);
    case Endian::Half: return v >> 16 | v << 16;
    default: return v;
    }
}

// The GPU's registers (Xenia's RegisterFile), by index (xenos.h's reg). Each
// access is a relaxed atomic one: the CPU reads and writes some of them
// through MMIO while the command processor runs.
//
// What changed, for a Sink that keeps what it made of the registers (beyond
// Xenia, whose backends compare the values again at each draw): the
// processor's own writes go through write and write_big_endian, which stamp
// the register's block of BLOCK registers with a serial when the value
// differs from the one held; a sink that remembers the serial as of its last
// look finds with changed_since whether a range was written to another value
// since. Writes from other threads (set: the CPU's MMIO) stamp nothing and
// count in foreign_writes instead, which the sink compares too.
class RegisterFile {
public:
    static constexpr uint32_t COUNT = xenos::REGISTER_COUNT;
    static constexpr uint32_t BLOCK_SHIFT = 5, BLOCK = 1u << BLOCK_SHIFT;
    RegisterFile() : values_(COUNT, 0), stamps_((COUNT + BLOCK - 1) >> BLOCK_SHIFT, 0) {}
    RegisterFile(const RegisterFile &) = delete;
    RegisterFile &operator=(const RegisterFile &) = delete;
    uint32_t operator[](uint32_t index) const { return index < COUNT ? get(index) : 0; }
    uint32_t get(uint32_t index) const {
        return std::atomic_ref<uint32_t>(const_cast<uint32_t &>(values_[index])).load(std::memory_order_relaxed);
    }
    // from any thread
    void set(uint32_t index, uint32_t value) {
        std::atomic_ref<uint32_t>(values_[index]).store(value, std::memory_order_relaxed);
        // (marked after the store: whoever takes the mark then reads this value or a later one)
        if (tracking_) foreign_dirty_[index >> (BLOCK_SHIFT + 6)].fetch_or(1ull << ((index >> BLOCK_SHIFT) & 63));
        foreign_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    // on the command processor's thread
    void write(uint32_t index, uint32_t value) {
        if (get(index) == value) return;
        std::atomic_ref<uint32_t>(values_[index]).store(value, std::memory_order_relaxed);
        stamps_[index >> BLOCK_SHIFT] = ++serial_;
        if (tracking_) mark(index >> BLOCK_SHIFT);
        if (draw_state(index)) state_serial_ = serial_;
        if (index >= 0x4000 && index < 0x4800) float_serial_[(index - 0x4000) >> 10] = serial_;
    }
    // count registers from first, from big-endian words (a packet's, guest
    // memory's); first + count within COUNT. On the command processor's thread.
    void write_big_endian(uint32_t first, const uint8_t *words, uint32_t count) {
        write_run<true>(first, words, count);
    }
    // the same from words in host order (a copy of another file's blocks)
    void write_host(uint32_t first, const uint32_t *words, uint32_t count) {
        write_run<false>(first, reinterpret_cast<const uint8_t *>(words), count);
    }

    // The blocks written since they were last taken, for a copy of the file
    // kept on another thread (gpu/deferred_sink.h): with tracking on, the
    // processor's writes that changed a value list their block, and the
    // writes from other threads (set) mark theirs. Set before the processor
    // runs.
    void track_blocks(bool on) {
        tracking_ = on;
        dirty_.clear();
        std::fill(dirty_mark_.begin(), dirty_mark_.end(), uint8_t(0));
        for (auto &w : foreign_dirty_) w.store(0, std::memory_order_relaxed);
    }
    bool tracking_blocks() const { return tracking_; }
    // each block listed since the last take, once (and any marked by other
    // threads, perhaps again), the lists emptied; on the processor's thread
    template <class F> void take_blocks(F &&block) {
        for (uint32_t b : dirty_) dirty_mark_[b] = 0, block(b);
        dirty_.clear();
        for (uint32_t w = 0; w < FOREIGN_WORDS; w++) {
            if (!foreign_dirty_[w].load(std::memory_order_relaxed)) continue;
            for (uint64_t bits = foreign_dirty_[w].exchange(0, std::memory_order_acquire); bits; bits &= bits - 1)
                block(w * 64 + uint32_t(__builtin_ctzll(bits)));
        }
    }
    // the copy's count of the writes from other threads, as the original's
    // stood (what a sink compares, see foreign_writes)
    void set_foreign_writes(uint64_t n) { foreign_writes_.store(n, std::memory_order_relaxed); }
    // The serial of the processor's last write that changed a value; whether
    // any of count registers from first changed after a serial (by blocks: a
    // neighbour in the same block counts); the writes from other threads.
    uint64_t serial() const { return serial_; }
    // The serial of the last such write to a register of the draws' state:
    // one that is neither a shader constant (float, fetch, boolean or loop:
    // a draw takes those as they stand) nor what a draw packet itself sets
    // (the index buffer's place, the draw initiator, the index offset and
    // bounds, the event initiator), nor below the registers a draw reads.
    // A draw made after another with this unchanged draws with the same
    // pipeline, targets, viewport and dynamic state.
    uint64_t state_serial() const { return state_serial_; }
    // and to a float constant of a stage's (0 the vertex shader's, 1 the pixel shader's)
    uint64_t float_constants_serial(int stage) const { return float_serial_[stage]; }
    static bool draw_state(uint32_t index) {
        if (index < 0x2000) return false;
        if (index >= 0x4000 && index < 0x4910) return false; // ALU, fetch, boolean and loop constants
        if (index >= 0x2100 && index <= 0x2103) return false; // VGT_MAX_VTX_INDX to VGT_MULTI_PRIM_IB_RESET_INDX
        if (index >= 0x21F9 && index <= 0x21FC) return false; // VGT_EVENT_INITIATOR to VGT_DRAW_INITIATOR
        return true;
    }
    bool changed_since(uint32_t first, uint32_t count, uint64_t serial) const {
        for (uint32_t b = first >> BLOCK_SHIFT, last = (first + count - 1) >> BLOCK_SHIFT; b <= last; b++)
            if (stamps_[b] > serial) return true;
        return false;
    }
    uint64_t foreign_writes() const { return foreign_writes_.load(std::memory_order_relaxed); }

    // all of them, for whoever runs on the command processor's thread (a
    // Sink's callbacks): constants copied in bulk
    const uint32_t *data() const { return values_.data(); }

private:
    static constexpr uint32_t BLOCKS = (COUNT + BLOCK - 1) >> BLOCK_SHIFT, FOREIGN_WORDS = (BLOCKS + 63) / 64;
    template <bool BigEndian> void write_run(uint32_t first, const uint8_t *words, uint32_t count) {
        while (count) {
            const uint32_t n = std::min(count, BLOCK - (first & (BLOCK - 1)));
            // The block's words in host order (a host-order run's own: they come as whole words)
            uint32_t swapped[BLOCK];
            const uint32_t *host = reinterpret_cast<const uint32_t *>(words);
            if constexpr (BigEndian) {
                std::memcpy(swapped, words, 4 * n);
                for (uint32_t k = 0; k < n; k++) swapped[k] = __builtin_bswap32(swapped[k]);
                host = swapped;
            }
            uint32_t *const held = values_.data() + first;
            if (run_masks()) {
                // Which registers changed, a bit each, by vector compares; then those alone
                // stored, by their bits, with a plain relaxed store. A register at a time, with a
                // branch each (taken at random in a block of constants) and std::atomic_ref's
                // constructor and store as calls (not inlined), was most of what a block cost the
                // GPU back end, which writes every block the processor changed into its own copy.
                // XF_REGISTER_RUN_MASKS=0: a register at a time, as before.
                const uint32_t changed = changed_mask(held, host, n);
                if (changed) {
                    ++serial_;
                    for (uint32_t m = changed; m; m &= m - 1) {
                        const uint32_t k = uint32_t(__builtin_ctz(m));
                        __atomic_store_n(&held[k], host[k], __ATOMIC_RELAXED);
                    }
                    stamped(first, ((changed << (first & (BLOCK - 1))) & state_mask(first & ~(BLOCK - 1))) != 0);
                }
            } else {
                // compared with those held in one pass, and stored only where they differ
                uint32_t differs = 0;
                for (uint32_t k = 0; k < n; k++) differs |= held[k] ^ host[k];
                if (differs) {
                    // (the shader constants' blocks hold no register of the draws' state)
                    bool state = false;
                    ++serial_;
                    const bool constants = first >= 0x4000 && first + n <= 0x4910;
                    for (uint32_t k = 0; k < n; k++) {
                        if (held[k] == host[k]) continue;
                        if (!constants && draw_state(first + k)) state = true;
                        std::atomic_ref<uint32_t>(held[k]).store(host[k], std::memory_order_relaxed);
                    }
                    stamped(first, state);
                }
            }
            first += n, words += 4 * n, count -= n;
        }
    }
    // a block changed at serial_ (whether a register of the draws' state did)
    void stamped(uint32_t first, bool state) {
        stamps_[first >> BLOCK_SHIFT] = serial_;
        if (tracking_) mark(first >> BLOCK_SHIFT);
        if (state) state_serial_ = serial_;
        if (first >= 0x4000 && first < 0x4800) float_serial_[(first - 0x4000) >> 10] = serial_;
    }
    // bit k: held[k] differs from host[k] (n within a block)
    static uint32_t changed_mask(const uint32_t *held, const uint32_t *host, uint32_t n) {
#if defined(__SSE2__)
        if (n == BLOCK) {
            uint32_t same = 0;
            for (uint32_t k = 0; k < BLOCK; k += 4) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(held + k));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(host + k));
                same |= uint32_t(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, b)))) << k;
            }
            return ~same;
        }
#endif
        uint32_t changed = 0;
        for (uint32_t k = 0; k < n; k++) changed |= uint32_t(held[k] != host[k]) << k;
        return changed;
    }
    void mark(uint32_t block) {
        if (!dirty_mark_[block]) dirty_mark_[block] = 1, dirty_.push_back(block);
    }
    static bool run_masks() {
        static const bool on = [] {
            const char *v = std::getenv("XF_REGISTER_RUN_MASKS");
            return !(v && v[0] == '0');
        }();
        return on;
    }
    // the registers of the draws' state (draw_state) in the block from base, a bit each
    static uint32_t state_mask(uint32_t base) {
        if (base < 0x2000 || (base >= 0x4000 && base + BLOCK <= 0x4910)) return 0;
        // (the blocks the exceptions lie in)
        if (base == 0x2100 || base == 0x21E0 || base == 0x4900) {
            uint32_t mask = 0;
            for (uint32_t k = 0; k < BLOCK; k++) mask |= uint32_t(draw_state(base + k)) << k;
            return mask;
        }
        return ~0u;
    }

    std::vector<uint32_t> values_;
    std::vector<uint64_t> stamps_; // per block, the serial of the last write that changed a value there
    uint64_t serial_ = 0, state_serial_ = 0, float_serial_[2] = {};
    std::atomic<uint64_t> foreign_writes_{0};
    // track_blocks: the processor's blocks changed since the last take (listed once each), and the
    // blocks other threads wrote (a bit each)
    bool tracking_ = false;
    std::vector<uint32_t> dirty_;
    std::vector<uint8_t> dirty_mark_ = std::vector<uint8_t>(BLOCKS, 0);
    std::array<std::atomic<uint64_t>, FOREIGN_WORDS> foreign_dirty_{};
};

// A draw as DRAW_INDX and DRAW_INDX_2 give it (VGT_DRAW_INITIATOR's fields);
// the rest of what it needs is in the registers.
struct Draw {
    uint32_t initiator = 0; // VGT_DRAW_INITIATOR
    uint32_t primitive = 0; // its prim_type
    uint32_t source = 0;    // its source_select: 0 DMA (indexed), 2 auto
    uint32_t count = 0;     // num_indices
    bool major_mode_explicit = false;
    uint32_t viz_query = 0; // DRAW_INDX's VIZ token (bit 8: predicated on query bits 0-5)
    // indexed: the index buffer (Xenia's IndexBufferInfo), physical
    bool indexed = false;
    bool index32 = false;
    uint32_t index_base = 0, index_bytes = 0;
    Endian index_endian = Endian::None;
    // immediate indices (VGT_DRAW_INITIATOR's source_select 1): the packet's
    // words after the initiator, as AMD's DRAW_INDEX_IMMD has them (R6xx 3D
    // Register Reference Guide): one 32-bit index a word, or two 16-bit ones,
    // the first in the low half; indexed is set, with no index buffer
    std::vector<uint32_t> immediate;
};

// XE_SWAP, the packet VdSwap writes: the front buffer, physical
struct Swap {
    uint32_t frontbuffer = 0, width = 0, height = 0;
};

// What the command processor reports, on its thread, in order. The defaults
// do nothing. Xenia's backend calls (IssueDraw, IssueCopy, IssueSwap,
// LoadShader) and its trace writer's (packets, buffers, memory read and
// written), so a renderer or a trace writer can stand behind it.
// A screen extent report's answer (Sink::extent_poll): the console pixels
// its draws can have rasterized, left and top inclusive, right and bottom
// exclusive; whole when the sink cannot tell (the whole screen).
struct Extent {
    uint64_t handle = 0;
    bool whole = true;
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

// Why the processor asks a sink to have caught up (Sink::sync).
enum class SyncFor : uint32_t {
    Fence,      // a write the CPU learns the GPU's progress by: MEM_WRITE, REG_TO_MEM, COND_WRITE's, and
                // EVENT_WRITE_SHD's and a scratch register's write-back where the sink does not write them later
    Interrupt,  // INTERRUPT, the guest's callback run next
    Wait,       // a WAIT_REG_MEM that did not match at once
    Gamma,      // the display's gamma ramp about to change (the sink reads it at its swaps)
    MemoryRead, // the processor about to read guest memory the sink's work may write (constants, polls)
    COUNT
};

class Sink {
public:
    virtual ~Sink() = default;
    // The processor is about to do something after which the guest can see
    // how far the GPU has come, or to read guest memory (MemoryRead: bytes at
    // a physical address) the sink's work may write: a sink that does its
    // work later than it is told (gpu/deferred_sink.h) finishes what it was
    // told so far first (for a read, what writes there). Others do nothing.
    virtual void sync(SyncFor /*why*/, uint32_t /*address*/, uint32_t /*bytes*/) {}
    // A fence's word for guest memory (EVENT_WRITE_SHD's, a scratch
    // register's write-back), the GPU's progress as the CPU learns it: a sink
    // doing its work later writes it once everything told before it is done,
    // as the console's GPU writes such an event at the end of its pipeline,
    // and returns true; false (the default), the processor writes it now.
    struct LateWrite {
        uint8_t *where = nullptr;                 // in guest memory
        uint32_t word = 0;                        // its four bytes as they are stored
        uint32_t address = 0;                     // physical, for memory_written
        void (*written)(void *context) = nullptr; // called after the write: what polls the write-backs woken
        void *context = nullptr;
    };
    virtual bool write_later(const LateWrite & /*write*/) { return false; }
    // a primary (indirect false) or indirect buffer begins or ends
    virtual void buffer_start(bool /*indirect*/, uint32_t /*address*/, uint32_t /*dwords*/) {}
    virtual void buffer_end(bool /*indirect*/) {}
    // a packet, before it runs: its address (physical) and its words, host
    // order; then its end, once it has run (an indirect buffer's packet ends
    // after the buffer's own packets). Only packets that run are reported:
    // one longer than what is left of its buffer is not.
    virtual void packet(uint32_t /*address*/, std::span<const uint32_t> /*words*/) {}
    virtual void packet_end() {}
    // whether packet() is wanted: a sink that does not take the packets
    // (the renderer) spares the processor copying each one's words (asked
    // once, as the sink is set)
    virtual bool wants_packets() { return false; }
    // memory the processor read (constants, shaders, polled words) or wrote
    // (fences, write-backs, reports), physical
    virtual void memory_read(uint32_t /*address*/, uint32_t /*bytes*/) {}
    virtual void memory_written(uint32_t /*address*/, uint32_t /*bytes*/) {}
    // a shader loaded (IM_LOAD, IM_LOAD_IMMEDIATE): 0 vertex, 1 pixel
    virtual void shader(uint32_t /*type*/, uint32_t /*address*/, std::span<const uint32_t> /*code*/) {}
    // IM_LOAD of `dwords` words at a physical address: true when the sink
    // has the shader it last loaded from there, that memory unwritten since,
    // and takes it again (the processor copies nothing and calls no
    // shader()); false, the processor copies the words and calls shader()
    virtual bool shader_again(uint32_t /*type*/, uint32_t /*address*/, uint32_t /*dwords*/) { return false; }
    // a draw outside copy mode, and a resolve (a draw with RB_MODECONTROL in
    // copy mode: EDRAM to memory, RB_COPY_*)
    virtual void draw(const RegisterFile &, const Draw &) {}
    virtual void resolve(const RegisterFile &, const Draw &) {}
    virtual void swap(const RegisterFile &, const Swap &) {}
    // Occlusion queries with real counts (OcclusionQueries::Fast). Whether
    // the sink counts the samples its draws pass; if not, the processor
    // writes its fake reports.
    virtual bool counts_samples() { return false; }
    // An EVENT_WRITE_ZPD: the draws since the last one counted for report
    // `ended` (0: none were counted), the draws from here count for `next`.
    // Each report ended is answered once, through zpd_poll.
    virtual void zpd_report(uint64_t /*ended*/, uint64_t /*next*/) {}
    // The samples of the reports whose counts are in since the last poll.
    virtual void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> & /*resolved*/) {}
    // Screen extents (EVENT_WRITE_EXT; ScreenExtents::Measured). Whether the
    // sink bounds what its draws rasterize; if not, every report is the
    // whole screen, as Xenia writes.
    virtual bool measures_extents() { return false; }
    // SCREEN_EXT_INIT: the draws from here count for the next report.
    virtual void extent_init() {}
    // SCREEN_EXT_RPT: the draws since the last init end a report, answered
    // once through extent_poll; 0: none (the whole screen).
    virtual uint64_t extent_report() { return 0; }
    // The reports so far answered, waiting for the GPU if it must (before an
    // interrupt or a wait, after which the CPU may read them).
    virtual void extent_wait() {}
    // What the sink has done so far, for XF_SLOW_FRAMES: the seconds it
    // waited for the GPU, its texture loads, its submissions, the pipelines
    // it has made. Called on the processor's thread.
    // With the work split in two threads (gpu/deferred_sink.h), also where
    // each thread's time went, every figure a running total in seconds:
    struct Work {
        double gpu_wait_seconds = 0;
        uint64_t texture_loads = 0, submissions = 0, pipelines = 0;
        // the texture loads by why: a texture not held, its memory written
        // by the CPU, by a resolve, by a memory export
        uint64_t texture_loads_new = 0, texture_loads_cpu = 0, texture_loads_resolved = 0, texture_loads_exported = 0;
        // the front end's waits for the back end, by why: SyncFor's reasons,
        // then the extents' wait (extent_wait), the ring full
        static constexpr size_t BACK_END_WAITS = size_t(SyncFor::COUNT) + 2;
        double back_end_wait_seconds[BACK_END_WAITS] = {};
        // the sink's waits where the guest may read the extents (extent_wait:
        // the submission holding them recorded, submitted and done), and of
        // them, the GPU's fence (the rest: the recording thread's submission)
        double extent_wait_seconds = 0, extent_fence_seconds = 0;
        // every wait for the recording thread to submit (the extents' among them)
        double record_wait_seconds = 0;
        // of the GPU waits (gpu_wait_seconds), those on the back end's thread: a submission's
        // slot free again (the rest are the front end's: the extents', a drain)
        double back_end_gpu_wait_seconds = 0;
        // the back end's wall time with records to do, and its processor time
        double back_end_busy_seconds = 0, back_end_cpu_seconds = 0;
        // Where the sink's thread (the back end's, with the work split) was held up outside its
        // records' plain work: waiting for the sink's own lock, by who held it (the front end's
        // passed calls, another thread such as a report's reader); making pipelines and
        // libraries itself (the driver's lookups and links among them); translating shaders
        // itself; loading textures (their images made, their data staged and their loads
        // recorded); draining the GPU (a buffer grown, a heap made again: of the GPU waits
        // above); writing the pipeline cache's file
        double lock_front_seconds = 0, lock_other_seconds = 0;
        double pipeline_seconds = 0, translate_seconds = 0, texture_seconds = 0;
        double drain_seconds = 0, cache_save_seconds = 0;
        uint64_t drains = 0;
        // the draws drawn by a stand-in while their pipeline is made: a linked pipeline, the
        // generic shaders (an interpreter, many times the GPU's time); and those skipped
        uint64_t stand_in_draws = 0, generic_draws = 0, skipped_draws = 0;
        // with the GPU timed (XF_GPU_TIME): its time in the submissions finished so far, and of
        // it the generic shaders' draws
        double gpu_seconds = 0, gpu_generic_seconds = 0;
    };
    virtual Work work() { return {}; }
    virtual void extent_poll(std::vector<Extent> & /*resolved*/) {}
    // VIZ_QUERY: query id (0-63) begins, or ends
    virtual void viz_query(uint32_t /*id*/, bool /*end*/) {}
};

// What EVENT_WRITE_ZPD's occlusion query reports hold (Xenia's
// occlusion_query option).
enum class OcclusionQueries {
    // a made-up count, running down from 100 to 80 and around (Xenia's
    // "fake"): what the processor writes with no sink counting samples
    Fake,
    // the sink's counts of the samples draws pass, the report written at
    // once with a guess (the last count at that address, or one sample) and
    // again when the counts are in (Xenia's "fast", its default)
    Fast,
};

// What EVENT_WRITE_EXT's screen extent reports hold.
enum class ScreenExtents {
    // the whole of the largest texture, 8192 pixels square (Xenia's answer)
    Whole,
    // that at once, then the bounds of what the report's draws rasterized
    // once the sink has measured them, where it measures (beyond Xenia):
    // ready before the next interrupt or wait the CPU could read them after
    Measured,
};

// What the command processor counts, read from any thread.
struct Counts {
    std::atomic<uint64_t> packets{0};         // of every type, executed or skipped
    std::atomic<uint64_t> primary_buffers{0}; // primary buffer runs
    std::atomic<uint64_t> indirect_buffers{0};
    std::atomic<uint64_t> draws{0}, resolves{0}, swaps{0};
    // the host's time, in nanoseconds, the processor's thread spent running packets (live: the
    // kernel's video thread adds it) and, of that, in WAIT_REG_MEM's waits
    mutable std::atomic<uint64_t> run_ns{0}, wait_ns{0};
    std::atomic<uint64_t> failed_draws{0};                    // no valid index source (or a packet too short)
    std::atomic<uint64_t> immediate_draws{0};                 // with immediate indices (Xenia fails them)
    std::atomic<uint64_t> interrupts{0};                      // INTERRUPT's, one per CPU in its mask
    std::atomic<uint64_t> waits{0};                           // WAIT_REG_MEM polls that did not match
    // of them: on memory (the rest on a register), and with a sleep (a wait field of 0x100 or
    // more: the rest yield); the WAIT_REG_MEM packets that did not match at once, and the last
    // such packet's poll address (bit 31: memory), reference, mask and wait field
    std::atomic<uint64_t> waits_on_memory{0}, waits_asleep{0}, waiting_packets{0};
    std::atomic<uint32_t> last_wait_poll{0}, last_wait_ref{0}, last_wait_mask{0}, last_wait_field{0};
    std::atomic<uint64_t> bad_packets{0};                     // ones that stopped their buffer
    std::array<std::atomic<uint64_t>, 128> opcodes{};         // type-3 packets by opcode
    std::array<std::atomic<uint64_t>, 128> unknown_opcodes{}; // the ones it has no handler for
};

class CommandProcessor {
public:
    // What the processor asks of the system.
    class Host {
    public:
        virtual ~Host() = default;
        // PM4_INTERRUPT for one CPU: the guest's graphics interrupt callback
        // with source 1 (Xenia's DispatchInterruptCallback(1, cpu)), run
        // before the next packet
        virtual void interrupt(uint32_t cpu) = 0;
        // A WAIT_REG_MEM that did not match: wait before polling again (the
        // packet's wait field: wait / 0x100 ms from 0x100 up, else no more
        // than a yield). False: stop executing (the GPU is shutting down).
        virtual bool wait(uint32_t wait) = 0;
        // The processor wrote something the CPU reads back to guest memory
        // (the read pointer's copy, a scratch register's, an event's value,
        // screen extents, a query's report): what the CPU's code polls for
        // while it waits for the GPU.
        virtual void wrote_back() {}
    };

    CommandProcessor(Memory &memory, Host &host);

    // Set before the processor's thread runs, or on it.
    void set_sink(Sink *sink) {
        sink_ = sink;
        packets_wanted_ = sink && sink->wants_packets();
    }
    Memory &memory() const { return memory_; }
    RegisterFile &registers() { return regs_; }
    const Counts &counts() const { return counts_; }

    // The ring buffer (VdInitializeRingBuffer): physical, 2^(size_log2 + 3)
    // bytes, zeroed; the read pointer back at its start.
    void initialize_ring_buffer(uint32_t physical, uint32_t size_log2);
    // Where the read pointer is written back after each run
    // (VdEnableRingBufferRPtrWriteBack), physical; 0 none.
    void enable_read_pointer_write_back(uint32_t physical, uint32_t block_size_log2);
    uint32_t read_pointer_write_back() const { return read_ptr_writeback_; }
    uint32_t ring_buffer() const { return primary_ptr_; }
    uint32_t ring_buffer_bytes() const { return primary_size_; }
    // where the processor has read the ring buffer to, in dwords
    uint32_t read_pointer() const { return read_ptr_; }

    // Executes the ring buffer from the read pointer to write_index (the
    // CPU's CP_RB_WPTR, in dwords), then writes the read pointer back (the
    // body of Xenia's WorkerThreadMain).
    void run(uint32_t write_index);
    // Executes the ring buffer from read_index to write_index (in dwords,
    // wrapping) and returns the new read index (Xenia's
    // ExecutePrimaryBuffer). A packet that cannot run ends the run, as in
    // Xenia.
    uint32_t execute_primary_buffer(uint32_t read_index, uint32_t write_index);
    // Executes count dwords of packets at a physical address (an indirect
    // buffer, a test's packets).
    void execute_indirect_buffer(uint32_t physical, uint32_t count);

    // What a trace player does (Xenia's TracePlayer): a recorded packet run
    // at its address on its own, outside any buffer (Xenia's
    // ExecutePacket(ptr, count)); the register file restored as recorded,
    // without the side effects of writing the registers (RestoreRegisters
    // without callbacks, as Xenia records them); the gamma ramps restored
    // (RestoreGammaRamp).
    void execute_packets(uint32_t physical, uint32_t count);
    void restore_registers(uint32_t first, std::span<const uint32_t> values);
    void restore_gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t> pwl);

    // A register written by a packet: stored, with the side effects some
    // have (the scratch registers' write-back, COHER_STATUS_HOST, the gamma
    // ramp's). The CPU's MMIO writes store without them, as Xenia's.
    void write_register(uint32_t index, uint32_t value);

    // The counter EVENT_WRITE_SHD writes when asked (bit 31 of its
    // initiator): every vblank and every swap move it (Xenia's counter_).
    uint32_t counter() const { return counter_.load(std::memory_order_relaxed); }
    void increment_counter() { counter_.fetch_add(1, std::memory_order_relaxed); }

    // The display's gamma ramps, as DC_LUT_* leave them: 256 DC_LUT_30_COLOR
    // entries (blue, green, red in 10 bits each from bit 0) and 128
    // DC_LUT_PWL_DATA entries of red, green and blue ([entry * 3 + component];
    // base in bits 0-15, delta in 16-31); the version changes with them.
    const std::array<uint32_t, 256> &gamma_ramp_table() const { return gamma_table_; }
    const std::array<uint32_t, 128 * 3> &gamma_ramp_pwl() const { return gamma_pwl_; }
    uint64_t gamma_ramp_version() const { return gamma_version_; }
    // the component (0 red, 1 green, 2 blue) the next sequential write goes to
    uint32_t gamma_ramp_rw_component() const { return gamma_component_; }
    // What occlusion query reports hold; set before the processor runs.
    void set_occlusion_queries(OcclusionQueries mode) { occlusion_ = mode; }
    OcclusionQueries occlusion_queries() const { return occlusion_; }
    // What screen extent reports hold; set before the processor runs.
    void set_screen_extents(ScreenExtents mode) { extents_ = mode, extents_by_gain_ = false; }
    // Measured or whole by what measuring gains: measured for a while, then
    // kept only where predicated tiling skipped enough of the draws for it
    // (the bins the guest narrowed), else the whole screen for a longer
    // while before it is tried again. Measuring makes the processor wait
    // for the GPU where the guest may read the extents; it pays in a scene
    // whose second tile then leaves out a good part of its draws, and costs
    // where the draws cover both tiles anyway (docs/renderer.md, "Screen
    // extents by their gain").
    void set_screen_extents_by_gain() {
        extents_ = ScreenExtents::Measured, extents_by_gain_ = true;
        extents_window_ = EXTENTS_TRIAL, extents_swaps_ = 0, extents_draws_ = extents_skipped_ = 0;
    }
    ScreenExtents screen_extents() const { return extents_; }

private:
    // a buffer being read: a ring (the primary buffer) or a straight run (an
    // indirect buffer), as Xenia's RingBuffer
    struct Reader {
        uint8_t *base = nullptr;
        uint32_t address = 0;                       // physical
        uint32_t capacity = 0, read = 0, write = 0; // bytes
        uint32_t count() const { return read <= write ? write - read : capacity - read + write; }
        uint32_t next();
        uint32_t peek(uint32_t words_ahead) const;
        void skip(uint32_t bytes) { read = capacity ? uint32_t((uint64_t(read) + bytes) % capacity) : 0; }
    };

    bool execute_packet();
    // the packets of a straight run of count dwords (an indirect buffer, a
    // trace's packet)
    void execute_straight(uint32_t start, uint32_t count);
    bool type0(uint32_t address, uint32_t packet);
    bool type1(uint32_t address, uint32_t packet);
    bool type3(uint32_t address, uint32_t packet);
    bool type3_opcode(uint32_t opcode, uint32_t count);
    // a packet that runs, to the sink: its header and the count words after it
    // (nothing called for a sink that wants no packets)
    void report_packet(uint32_t address, uint32_t packet, uint32_t count) {
        if (packets_wanted_) report_packet_words(address, packet, count);
    }
    void report_packet_words(uint32_t address, uint32_t packet, uint32_t count);
    // (to the sink that was given the packet: one that wants none gets no end either)
    void end_packet() {
        if (packets_wanted_) sink_->packet_end();
    }
    // Registers written as a run: count big-endian words to the registers from base. Where none of
    // them has a side effect (special_register_write's all lie below PLAIN_REGISTERS) they are
    // stored in one loop (RegisterFile::write_big_endian); else one by one through write_register.
    static constexpr uint32_t PLAIN_REGISTERS = 0x2000;
    void write_register_run(uint32_t base, const uint8_t *words, uint32_t count);
    void special_register_write(uint32_t index, uint32_t value);
    void write_registers_from_ring(uint32_t base, uint32_t count);
    void write_registers_from_memory(uint32_t base, uint32_t physical, uint32_t count);
    void make_coherent();
    void sync(SyncFor why, uint32_t address = 0, uint32_t bytes = 0) {
        if (sink_) sink_->sync(why, address & 0x1FFFFFFF, bytes); // (physical)
    }
    // a fence's word: the sink's to write later, or written now (after a sync)
    void fence_write(uint8_t *where, uint32_t word, uint32_t address);
    static void wrote_back(void *self) { static_cast<CommandProcessor *>(self)->host_.wrote_back(); }
    bool wait_reg_mem();
    bool cond_write();
    bool event_write_shd();
    bool event_write_ext();
    bool event_write_zpd();
    bool draw(uint32_t viz_query, uint32_t remaining);
    bool im_load();
    bool im_load_immediate(uint32_t count);
    bool xe_swap(uint32_t count);

    // guest memory, physical: words as the GPU reads and writes them
    uint8_t *physical(uint32_t address) const { return memory_.translate_physical(address); }
    uint32_t load_swapped(uint32_t address, Endian e) const;        // a word, swapped as e says
    void store_swapped(uint32_t address, uint32_t value, Endian e); // the inverse
    void store_be32(uint32_t address, uint32_t value);
    void bump(std::atomic<uint64_t> &n) { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

    Memory &memory_;
    Host &host_;
    Sink *sink_ = nullptr;
    bool packets_wanted_ = false; // sink_->wants_packets(), asked as it was set
    RegisterFile regs_;
    Counts counts_;
    Reader reader_;
    uint32_t depth_ = 0;          // indirect buffers inside indirect buffers
    std::vector<uint32_t> words_; // a packet's words, for the sink

    // set by the CPU's thread (the Vd* exports), read by the processor's
    std::atomic<uint32_t> primary_ptr_{0}, primary_size_{0}, read_ptr_{0};
    std::atomic<uint32_t> read_ptr_writeback_{0}, read_ptr_update_freq_{0};
    std::atomic<uint32_t> counter_{0};
    uint64_t bin_select_ = 0xFFFFFFFFull, bin_mask_ = 0xFFFFFFFFull;
    std::vector<uint32_t> me_bin_; // ME_INIT's micro-engine words

    // EVENT_WRITE_ZPD: Xenia's fake reports (its occlusion_query=fake and
    // the fallback without host queries): a sample count running down from
    // 100 to 80 and around, added to a free-running counter
    uint32_t fake_zpd_samples_ = 0;
    uint64_t zpd_counter_ = 0;
    // With real counts (OcclusionQueries::Fast, Xenia's CommandProcessor
    // ZPD state): the reports written with a guess and waiting for their
    // counts, in order; the draws now counting for zpd_current_
    struct ZpdReport {
        uint32_t address = 0;
        uint64_t handle = 0;
        uint64_t delta = 0;             // the samples its draws passed, once in
        bool pending = true;            // its counts not in yet
        uint64_t speculative_value = 0; // the counter the guess wrote
        uint64_t speculative_delta = 0;
    };
    // Screen extents measured (ScreenExtents::Measured): the reports
    // waiting for the sink's answer, and the latest report at each address
    // (an answer only lands where no later report has written since)
    ScreenExtents extents_ = ScreenExtents::Measured;
    // set_screen_extents_by_gain: the swaps of a trial of measuring and of
    // a stay with the whole screen, and the share of the draw packets the
    // bins must skip for measuring to stay (1 in EXTENTS_GAIN)
    static constexpr uint32_t EXTENTS_TRIAL = 120, EXTENTS_STAY = 600, EXTENTS_GAIN = 7;
    bool extents_by_gain_ = false;
    uint32_t extents_window_ = EXTENTS_TRIAL, extents_swaps_ = 0;
    uint64_t extents_draws_ = 0, extents_skipped_ = 0; // draw packets run and skipped by their bins, this window
    // XF_SLOW_FRAMES: the last swap's time and the counts as of it
    std::chrono::steady_clock::time_point slow_at_{};
    uint64_t slow_swaps_ = 0, slow_waited_ = 0, slow_draws_ = 0, slow_resolves_ = 0, slow_reports_ = 0;
    Sink::Work slow_work_{};
    uint64_t slow_cpu_ = 0; // the processor's thread's processor time (thread_cpu.h's ticks)
    std::vector<std::pair<uint64_t, uint32_t>> extent_pending_; // handle (0: answered) and address, in order
    size_t extent_first_ = 0;                                   // the first of them still waiting
    // The latest report at each address, by address: its handle (0: none).
    // Thousands of reports a frame at the addresses of the frame before's,
    // none ever taken out: an open table (a multiplicative hash, the next
    // slot on a clash, doubled at half full), a tenth of a hash map's cost.
    class ExtentLatest {
    public:
        uint64_t &at(uint32_t address) { // made, with no handle, where there is none
            if (used_ * 2 >= slots_.size()) grow();
            return slot(address).second;
        }
        uint64_t *find(uint32_t address) {
            if (slots_.empty()) return nullptr;
            std::pair<uint32_t, uint64_t> &s = probe(address);
            return s.first == address ? &s.second : nullptr;
        }

    private:
        static constexpr uint32_t EMPTY = UINT32_MAX; // (an address is a multiple of 4)
        std::pair<uint32_t, uint64_t> &probe(uint32_t address) {
            size_t i = size_t((address >> 2) * 0x9E3779B1u) & (slots_.size() - 1);
            while (slots_[i].first != address && slots_[i].first != EMPTY) i = (i + 1) & (slots_.size() - 1);
            return slots_[i];
        }
        std::pair<uint32_t, uint64_t> &slot(uint32_t address) {
            std::pair<uint32_t, uint64_t> &s = probe(address);
            if (s.first == EMPTY) s = {address, 0}, used_++;
            return s;
        }
        void grow() {
            std::vector<std::pair<uint32_t, uint64_t>> old = std::move(slots_);
            slots_.assign(old.empty() ? 1024 : old.size() * 2, {EMPTY, 0});
            used_ = 0;
            for (const auto &[address, handle] : old)
                if (address != EMPTY) slot(address).second = handle;
        }
        std::vector<std::pair<uint32_t, uint64_t>> slots_;
        size_t used_ = 0;
    } extent_latest_;
    std::vector<Extent> extent_resolved_;
    void write_extent(uint32_t address, const uint16_t (&extents)[6], bool told = true);
    void poll_extents(bool wait);
    OcclusionQueries occlusion_ = OcclusionQueries::Fake;
    std::deque<ZpdReport> zpd_reports_;
    uint64_t zpd_current_ = 0, zpd_next_ = 1;
    uint64_t zpd_speculative_counter_ = 0;
    std::unordered_map<uint32_t, uint64_t> zpd_cached_deltas_; // the last count of each report address
    void write_zpd(uint32_t address, uint64_t z_pass);
    void queue_zpd(uint32_t address);
    void poll_zpd();
    void retire_zpd();

    std::array<uint32_t, 256> gamma_table_{};
    std::array<uint32_t, 128 * 3> gamma_pwl_{};
    uint32_t gamma_component_ = 0; // the next of red, green, blue DC_LUT_SEQ_COLOR and DC_LUT_PWL_DATA write
    uint64_t gamma_version_ = 0;
};

} // namespace xf::gpu
