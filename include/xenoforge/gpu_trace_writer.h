// Xenia's GPU traces (.xtr) written from a live run (docs/renderer.md, "How
// it is checked"): the frames asked for, each a file Xenia's trace player
// and XenoForge's replay both read, so a scene any run reaches has Xenia's
// own picture of the same commands (xenia-gpu-vulkan-trace-dump) as its
// reference. Xenia's TraceWriter (src/xenia/gpu/trace_writer.cc,
// trace_protocol.h; BSD, see LICENSE-xenia) translated.
//
// The writer stands between the command processor and its Sink (the
// renderer): it records what the processor does, packet by packet, and
// hands every call on. A trace begins after a swap, as Xenia's: the
// register file, the gamma ramps and EDRAM's contents first, then the
// packets of one frame, each with the memory it read, up to and with the
// next swap. Frames are numbered as Xenia's --trace_gpu_frames numbers them,
// and as boot numbers its shots: frame N is what the swap after the first N
// shows, written as <title id>_<N>.xtr.
//
// The memory a frame reads is what the processor reads (constants, shaders,
// polled words) and what the backend reads to draw (vertex data, indices,
// textures), which it reports through MemoryTrace, as Xenia's shared memory
// reports its uploads. Each range is recorded where the memory differs from
// what the trace has already given (as Xenia's shared memory uploads again
// only what the CPU wrote since), never what the GPU itself wrote during the
// frame: a replay's own resolves make that, as on Xenia.

#pragma once

#include "xenoforge/command_processor.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xf::gpu {

// What a backend tells a trace writer about guest memory while a trace is
// open (physical addresses; from the processor's thread, or from whichever
// thread finishes the backend's work).
class MemoryTrace {
public:
    virtual ~MemoryTrace() = default;
    // memory the backend read where it lies, for a draw, a resolve or a swap
    virtual void read(uint32_t address, uint32_t bytes) = 0;
    // memory the GPU wrote (a resolve's bytes) that the backend has just put
    // into guest memory: a replay makes it with its own resolves
    virtual void gpu_wrote(uint32_t address, uint32_t bytes) = 0;
};

// What a trace asks of the backend as it begins.
class TraceSource {
public:
    virtual ~TraceSource() = default;
    // where the backend reports the memory it reads; null: nowhere
    virtual void set_memory_trace(MemoryTrace *trace) = 0;
    // everything given to the GPU done, so guest memory holds what it wrote
    virtual void settle() = 0;
    // EDRAM as it stands, 10 MB in Xenia's layout (its snapshots'); false
    // when the backend keeps none
    virtual bool snapshot_edram(std::vector<uint8_t> &out) = 0;
    // memory the GPU wrote that guest memory does not hold (memory exports,
    // which stay on the GPU as Xenia's do): addresses and bytes, recorded
    // as the trace begins, as Xenia's trace downloads what its GPU wrote
    virtual void gpu_memory(std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &out) { out.clear(); }
};

struct TraceOptions {
    std::filesystem::path directory;
    std::set<uint64_t> frames; // the frames to record
    uint32_t title_id = 0;
    bool compress = true; // snappy for what is longer than 1 KB, as Xenia's writer
};

class TraceWriter final : public Sink, public MemoryTrace {
public:
    // Hands every call on to next (may be null); source (may be null) gives
    // EDRAM and reports what the backend reads.
    TraceWriter(CommandProcessor &processor, Sink *next, TraceSource *source, TraceOptions options);
    ~TraceWriter() override; // a trace still open is closed as it stands

    // the frame after the one under way recorded too (the player's F4, as
    // Xenia's); from any thread
    void trace_next();
    // the traces finished so far, and the errors met (a file not opened)
    std::vector<std::filesystem::path> finished() const;
    std::vector<std::string> errors() const;

    // Sink: recorded, then handed on
    void buffer_start(bool indirect, uint32_t address, uint32_t dwords) override;
    void buffer_end(bool indirect) override;
    void packet(uint32_t address, std::span<const uint32_t> words) override;
    void packet_end() override;
    // (only while a frame is recorded or about to be: set_packets)
    bool wants_packets() override { return packets_; }
    void memory_read(uint32_t address, uint32_t bytes) override;
    void memory_written(uint32_t address, uint32_t bytes) override;
    void shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) override;
    void draw(const RegisterFile &registers, const Draw &draw) override;
    void resolve(const RegisterFile &registers, const Draw &draw) override;
    void swap(const RegisterFile &registers, const Swap &swap) override;
    void sync(SyncFor why, uint32_t address, uint32_t bytes) override {
        if (next_) next_->sync(why, address, bytes);
    }
    // (a backend writing a fence later does so only while no trace records: it then does its work
    // at once, and the fence is written by the processor and recorded here)
    bool write_later(const LateWrite &write) override { return next_ && next_->write_later(write); }
    // What the backend has done, for XF_SLOW_FRAMES (the player keeps a writer standing by for F4:
    // without this its lines counted no texture loads, submissions or pipelines).
    Work work() override { return next_ ? next_->work() : Work{}; }
    // An IM_LOAD of a shader the backend has from unwritten memory, not copied: the backend's
    // answer while no trace is being written (one being written gets every shader's words).
    bool shader_again(uint32_t type, uint32_t address, uint32_t dwords) override;
    // occlusion queries: the backend's (counts_samples false without one)
    bool counts_samples() override { return next_ && next_->counts_samples(); }
    void zpd_report(uint64_t ended, uint64_t next) override {
        if (next_) next_->zpd_report(ended, next);
    }
    void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) override {
        if (next_) next_->zpd_poll(resolved);
    }
    // Screen extents and VIZ queries: the backend's (measures_extents false without one). The player
    // keeps a writer standing by for F4; without these the processor wrote every extent report as the
    // whole screen there, so a game's predicated tiling never skipped a bin, as it does in a replay.
    bool measures_extents() override { return next_ && next_->measures_extents(); }
    void extent_init() override {
        if (next_) next_->extent_init();
    }
    uint64_t extent_report() override { return next_ ? next_->extent_report() : 0; }
    void extent_wait() override {
        if (next_) next_->extent_wait();
    }
    void extent_poll(std::vector<Extent> &resolved) override {
        if (next_) next_->extent_poll(resolved);
    }
    void viz_query(uint32_t id, bool end) override {
        if (next_) next_->viz_query(id, end);
    }

    // MemoryTrace: what the backend read and what the GPU wrote
    void read(uint32_t address, uint32_t bytes) override;
    void gpu_wrote(uint32_t address, uint32_t bytes) override;

private:
    // what the trace has given of a 4 KB page: its bytes, and which are known
    struct Page {
        uint8_t bytes[4096];
        uint64_t known[64];
    };

    void open(uint64_t frame);
    void close();
    void put(const void *data, size_t bytes);
    void put_u32(uint32_t value) { put(&value, 4); }
    // a command with an encoding and an encoded length, then its data
    // (Xenia's MemoryCommand, EdramSnapshotCommand, RegistersCommand,
    // GammaRampCommand share the shape)
    void put_encoded(std::span<const uint8_t> head, size_t length_at, std::span<const uint8_t> data);
    // guest memory's bytes at the address, or the given ones
    void put_memory(uint32_t command, uint32_t address, uint32_t bytes, const uint8_t *data = nullptr);
    // the range as the trace gives it from now on (guest memory's bytes, or
    // the given ones)
    void learn(uint32_t address, uint32_t bytes, const uint8_t *data = nullptr);
    // the parts of the range that differ from what the trace gave, recorded
    void read_locked(uint32_t address, uint32_t bytes);
    // the processor's packets asked for, or no longer (its thread)
    void set_packets(bool on);

    CommandProcessor &processor_;
    Sink *next_;
    TraceSource *source_;
    TraceOptions options_;
    const uint8_t *memory_; // physical memory
    mutable std::mutex mutex_;
    std::FILE *file_ = nullptr;
    std::filesystem::path path_;
    std::vector<std::filesystem::path> finished_;
    std::vector<std::string> errors_;
    uint64_t swaps_ = 0;       // swaps executed, the number of the frame after them
    bool started_ = false;     // the first packet seen (frame 0 opens there)
    bool swapped_ = false;     // the packet under way swapped
    bool packets_ = false;     // the processor reports its packets (a frame recorded or about to be)
    std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
};

} // namespace xf::gpu
