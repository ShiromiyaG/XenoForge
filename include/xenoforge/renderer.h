// The Xenos renderer (plans B3 and B4): what the GPU's command processor
// executes, drawn on Vulkan draw by draw, as the console's GPU would have
// drawn it. The renderer is the processor's Sink (command_processor.h): the
// processor runs the packets and keeps the registers and constants; the
// renderer takes the shaders it loads, and its draws, resolves and swaps
// with the register file as it stands. A draw becomes a Vulkan draw with the
// translated shaders (shader_translator.h) into host render targets that
// stand for EDRAM; a resolve copies a render target into guest memory
// (resolve.h) where later draws read it as a texture (texture.h); a swap
// shows texture fetch constant 0 through the display's gamma ramp, as
// Xenia does. Guest memory is read where it lies: the processor's physical
// memory. The work is recorded and submitted a frame at a time, and waited
// for only when the CPU needs what the GPU wrote.
//
// Live, the processor runs on the "GPU commands" kernel thread and the
// renderer's Sink calls arrive there; a trace is replayed through a
// processor the same way (replay).
//
// What a draw needs and the renderer does not do yet is counted by reason
// in the report, and the draw is skipped, never guessed at.

#pragma once

#include "xenoforge/command_processor.h"
#include "xenoforge/gpu_trace.h"
#include "xenoforge/vk_device.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace xf::render {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // 8 bits per component, rows top to bottom
};

struct Report {
    uint64_t packets = 0;             // the processor's (replay only)
    uint64_t draws = 0, drawn = 0;    // outside copy mode; of them, done
    uint64_t resolves = 0, swaps = 0; // draws in copy mode; XE_SWAP packets
    uint64_t frames = 0;              // front buffers read back (the swaps asked for)
    uint64_t render_targets = 0, textures = 0, pipelines = 0, shaders = 0;
    double seconds = 0;                          // spent in the renderer's calls, of which:
    double submit_seconds = 0;                   //   submitting work and waiting for the GPU, of which:
    double wait_seconds = 0;                     //     waiting for the GPU to finish work
    double check_seconds = 0;                    //   checking textures against the memory under them
    double decode_seconds = 0;                   //   decoding textures on the CPU and recording their uploads
    double vertex_seconds = 0;                   //   copying vertex data out of guest memory
    double resolve_seconds = 0;                  //   recording resolves (their waits counted in submit_seconds too)
    double writeback_seconds = 0;                //   copying what resolves wrote into guest memory, once done
    uint64_t vertex_bytes = 0;                   // the vertex data copied
    uint64_t writeback_bytes = 0;                // what resolves wrote, copied into guest memory
    uint64_t submissions = 0, waits = 0;         // command buffers submitted; waits for the GPU
    std::map<std::string, uint64_t> skipped;     // draws not done, by reason
    std::map<std::string, uint64_t> approximate; // done, with something not exact yet, by what
};

struct Options {
    bool verbose = false; // a line per draw and resolve on stderr
    // Which swaps' front buffers to read back, by the swap's number from 0
    // (the number Xenia's --trace_gpu_frames gives the frame a trace holds:
    // frame N is what the swap after the first N shows); none when empty.
    std::function<bool(uint64_t swap)> frame_wanted;
    // Each front buffer read back, once the GPU has drawn it: at a later
    // call of the processor's, on its thread, or at drain() on the caller's.
    // This is the presentation hook too: the player's window takes the
    // frame and returns at once (frame_wanted says no while the window has
    // not shown the last one), so the guest never waits for a display.
    std::function<void(uint64_t swap, const Image &frame)> on_frame;
};

class Renderer : public gpu::Sink {
public:
    // A renderer for the processor's draws on the device, reading the
    // processor's physical memory. Install it with processor.set_sink.
    static std::unique_ptr<Renderer> create(vk::Device &device, gpu::CommandProcessor &processor,
                                            const Options &options, std::string &error);
    ~Renderer() override = default;

    // EDRAM's contents (a trace's snapshot, in Xenia's layout)
    virtual void restore_edram(std::span<const uint8_t> bytes) = 0;
    // what it has done so far; from any thread (it waits for a call under
    // way to end)
    virtual Report report() const = 0;
    // the same without waiting: false while a call is under way
    virtual bool try_report(Report &out) const = 0;
    // true once the device has failed, with why (the calls after it do nothing)
    virtual bool failed(std::string &error) const = 0;
    // everything given to the GPU submitted and waited for, so the front
    // buffers asked for so far have gone to on_frame (on this thread) and
    // guest memory holds what resolves wrote; from any thread
    virtual void drain() = 0;
};

// Replays the trace through a command processor of its own, as Xenia's trace
// player does, into a renderer; the image is what the last swap showed
// (empty without a swap). False only when the trace or the device fails,
// not for draws that were skipped.
bool replay(const GpuTrace &trace, vk::Device &device, const Options &options, Image &frame, Report &report,
            std::string &error);

} // namespace xf::render
