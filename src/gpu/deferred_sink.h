// The GPU's work split in two threads (on by default, XF_GPU_BACK_END=0 off; docs/renderer.md,
// "The command thread split in two"): the command processor's thread (the
// front end) parses the packets, keeps the register file and does everything
// the guest can observe at once (its register and memory writes, fences,
// interrupts, waits, the read pointer), while the sink's work for draws,
// resolves, swaps and shader loads (the renderer's) runs on a thread of its
// own (the back end), in the same order, from a ring of records.
//
// A record carries what its call needs as the processor stood then: the
// draw, the shader's words, and the register file's blocks written since the
// record before (RegisterFile::track_blocks), which the back end writes into
// a copy of the file of its own before it calls the sink. The sink then sees
// the same calls with the same registers as without the split, later.
//
// Where the guest could see how far the GPU has come, the processor asks for
// a sync (Sink::sync): the front end waits there until the back end has done
// everything handed over. Between two syncs the back end reads guest memory
// (vertices, indices, textures, shaders) after the front end has moved on,
// as the console's GPU reads it after its command processor has; a game
// reuses that memory only after a fence or a wait, which sync.
//
// Calls that answer at once are passed to the sink from the calling thread:
// whether it counts samples or measures extents, its occlusion counts and
// screen extents in (the extents' handles made here and matched to the
// sink's), its work so far. The sink is called from the back end's thread
// and, for those, the front end's: it must take its own lock (the renderer's
// Call does). Only the processor's thread calls the Sink entry points here.
#pragma once

#include "xenoforge/command_processor.h"
#include "xenoforge/guest_memory.h"

#include "thread_cpu.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xf::gpu {

class DeferredSink final : public Sink {
  public:
    // back: the sink doing the work; front: the processor's register file
    // (its blocks tracked from here on); memory: where shaders are read
    DeferredSink(Sink &back, RegisterFile &front, Memory &memory);
    ~DeferredSink() override; // everything handed over done, the thread stopped

    // True on the back end's thread, and while a call is passed from here to
    // the sink on another: a sink that sends its own entry points here (the
    // renderer) does its work then.
    static bool passing();
    // True on the back end's own thread only.
    static bool on_back_end();
    // Everything handed over so far done; from any thread (at once on the
    // back end's own).
    void finish();
    // Synchronous: every call passed to the sink at once with the
    // processor's register file, as without the split (while a trace records
    // what the sink reads, in order with the packets). On the processor's
    // thread.
    void set_synchronous(bool on);

    // What it did, for its line at the end (read once the processor has stopped).
    struct Stats {
        uint64_t records = 0, bytes = 0, blocks = 0;
        uint64_t syncs[size_t(SyncFor::COUNT)] = {};  // asked for
        uint64_t waited[size_t(SyncFor::COUNT)] = {}; // of them, with work still to do
        uint64_t finishes = 0, full = 0;              // other waits: finish() from outside, the ring full
        uint64_t late_writes = 0;                     // fences written by the back end
        double wait_seconds = 0;                      // the front end's time waiting for the back end
        // of it, by why (Work::back_end_wait_seconds's order: SyncFor's, the extents', the ring full)
        double wait_by[Work::BACK_END_WAITS] = {};
    };
    Stats stats() const;
    // the back end's processor time so far (thread_cpu.h's ticks), from any thread
    uint64_t back_cpu_ticks() const { return back_cpu_.ticks(); }

    // ---- the Sink ----
    void sync(SyncFor why, uint32_t address, uint32_t bytes) override;
    bool write_later(const LateWrite &write) override;
    void memory_written(uint32_t address, uint32_t bytes) override;
    void shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) override;
    bool shader_again(uint32_t type, uint32_t address, uint32_t dwords) override;
    void draw(const RegisterFile &registers, const Draw &d) override;
    void resolve(const RegisterFile &registers, const Draw &d) override;
    void swap(const RegisterFile &registers, const Swap &s) override;
    bool counts_samples() override;
    void zpd_report(uint64_t ended, uint64_t next) override;
    void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) override;
    bool measures_extents() override;
    void extent_init() override;
    uint64_t extent_report() override;
    void extent_wait() override;
    void extent_poll(std::vector<Extent> &resolved) override;
    Work work() override;
    void viz_query(uint32_t id, bool end) override;

  private:
    enum class Kind : uint32_t {
        Wrap, // the rest of the ring skipped
        Blocks,
        Draw,
        Resolve,
        Swap,
        Shader,
        ImLoad,
        MemoryWritten,
        VizQuery,
        ZpdReport,
        ExtentInit,
        ExtentReport,
        LateWrite
    };
    struct Header {
        Kind kind;
        uint32_t bytes; // the payload's, after the header
    };
    // a draw's fixed part, its immediate indices after it
    struct DrawRecord {
        uint32_t initiator, primitive, source, count, viz_query, index_base, index_bytes, immediate;
        uint8_t major_mode_explicit, indexed, index32, index_endian;
    };

    // ---- the front end ----
    // room for a record of bytes, its payload's place (waits while the ring is full)
    uint8_t *reserve(Kind kind, uint32_t bytes);
    void commit(); // the record reserved handed over
    void push(Kind kind, const void *payload, uint32_t bytes);
    // the register blocks written since the last record, as one (before any other record)
    void push_registers();
    void push_draw(Kind kind, const Draw &d);
    // (why: the index in Stats::wait_by)
    void wait_done(uint64_t position, uint64_t *counter, size_t why);
    static constexpr size_t WAIT_EXTENTS = size_t(SyncFor::COUNT), WAIT_FULL = size_t(SyncFor::COUNT) + 1;

    // ---- the back end ----
    void run();
    void handle(Kind kind, const uint8_t *payload, uint32_t bytes);

    Sink &back_;
    RegisterFile &front_;
    Memory &memory_;
    GuestMemory view_;  // physical memory, for a resolve's destination
    RegisterFile copy_; // the back end's
    bool synchronous_ = false, resync_ = true;
    uint64_t foreign_sent_ = UINT64_MAX;
    std::vector<uint32_t> blocks_; // push_registers's

    static constexpr size_t RING = size_t(32) << 20; // bytes, a power of two
    std::unique_ptr<uint8_t[]> ring_;
    // byte positions, never wrapped: what the front end has handed over, what the back end has done
    std::atomic<uint64_t> written_{0}, done_{0};
    uint64_t reserved_ = 0, record_at_ = 0; // the front end's: the record being made
    std::atomic<bool> back_asleep_{false}, stop_{false};
    // the lowest position a thread asleep in sleep_until waits for (UINT64_MAX: none); the back end
    // wakes the sleepers once done_ reaches it, not at every record
    std::atomic<uint64_t> wanted_{UINT64_MAX};
    void sleep_until(uint64_t position);
    std::thread thread_;
    ThreadCpu back_cpu_;
    std::atomic<uint64_t> back_busy_{0}; // the back end's wall time with records to do (FastClock's ticks)

    // Where the back end's work yet to be done writes guest memory, physical [start, end), and the
    // position its record ends at: the resolves' destinations and the fences written later. A
    // memory read by the processor waits for those it overlaps.
    struct Pending {
        uint32_t start, end;
        uint64_t position;
    };
    std::vector<Pending> pending_;
    void add_pending(uint32_t start, uint32_t end);
    std::vector<std::pair<uint32_t, uint32_t>> written_ranges_, partial_ranges_; // (resolve's, kept for their room)

    // screen extent reports: the handles made here, the sink's matched to them as it reports
    std::mutex extents_mutex_;
    uint64_t extent_next_ = 1;                              // the front end's
    std::unordered_map<uint64_t, uint64_t> extent_handles_; // the sink's handle to ours
    std::vector<Extent> extent_whole_;                      // ours the sink did not bound (whole)
    std::vector<Extent> extent_scratch_;

    // the back end's reusable draw
    Draw draw_;
    std::vector<uint32_t> words_;

    Stats stats_; // the front end's (read at the end)
    std::atomic<uint64_t> finishes_{0};
};

} // namespace xf::gpu
