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
#include "xenoforge/gpu_trace_writer.h"
#include "xenoforge/vk_device.h"

#include <array>
#include <cstdint>
#include <filesystem>
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
    double pipeline_seconds = 0;                 //   making pipelines (the driver compiling the shaders)
    uint64_t pipeline_cache_read = 0;            // bytes of pipeline cache read from disk at the start
    uint64_t pipeline_cache_written = 0;         // bytes of it written back, the last time
    uint64_t vertex_bytes = 0;                   // the vertex data copied
    uint64_t writeback_bytes = 0;                // what resolves wrote, copied into guest memory
    uint64_t submissions = 0, waits = 0;         // command buffers submitted; waits for the GPU
    std::map<std::string, uint64_t> skipped;     // draws not done, by reason
    std::map<std::string, std::vector<uint64_t>> skipped_at; // the first swaps (by number) each reason came in
    std::map<std::string, uint64_t> approximate; // done, with something not exact yet, by what
};

// A swap's front buffer on the GPU (Options::on_swap): level 0, layer 0 of a
// 2D image on the renderer's device, in VK_IMAGE_LAYOUT_GENERAL, usable as a
// sampled image and a transfer source; and how the console's display shows
// it (as the read-back's on_frame picture is made).
struct FrontBuffer {
    uint64_t swap = 0; // numbered as frame_wanted numbers them
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint32_t swizzle = 0;    // the fetch constant's: 3 bits a component (red first), 0-3 a texel's, 4 zero, 5 one
    uint32_t components = 4; // in the image's format (a swizzle past them takes the last)
    bool gamma_table = true; // the display's 256-entry ramp applies (not the piecewise one of 2_10_10_10)
    std::array<uint32_t, 256> gamma{}; // that ramp at the swap: 10 bits a component, red highest
};

struct Options {
    bool verbose = false; // a line per draw and resolve on stderr
    // where to write each shader loaded, as Xenia's --dump_shaders writes
    // them (shader_<hash>.ucode.bin.vert or .frag, words in host order; the
    // hash is the renderer's, the one verbose draws print); none when empty
    std::string dump_shaders;
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
    // Each swap's front buffer on the GPU, with no read-back: called on the
    // renderer's thread right after the swap's work is submitted on the
    // device's queue. The synchronization is the queue's order: what the hook
    // submits on that queue (vk::Device::submit) before it returns comes after
    // the swap's work (a barrier at its start sees it) and before the
    // renderer's next work (a barrier at its end makes that wait for it). The
    // image is valid only until the hook returns; it must not wait for the GPU.
    std::function<void(const FrontBuffer &front)> on_swap;
    // Vulkan's pipeline cache on disk: read when the renderer is made (data
    // from another device or driver is ignored, Vulkan checks its header),
    // written once new pipelines have settled (300 swaps without one) and at
    // drain(). Empty: a cache in memory only.
    std::filesystem::path pipeline_cache;
};

// A trace writer (gpu_trace_writer.h) stands in front of it as the
// processor's Sink and takes what it reads of guest memory (TraceSource):
// vertex data, indices, textures, a resolve's rectangle and the tiles it
// cuts, each where the renderer reads it on the CPU's side (not what it
// loads from the mirror, which a replay's resolves make), and what resolves
// wrote as it reaches guest memory.
class Renderer : public gpu::Sink, public gpu::TraceSource {
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
// not for draws that were skipped. Without restore_edram the trace's EDRAM
// snapshot is left out, EDRAM starting as zeros, as Xenia's Vulkan trace
// player leaves it.
bool replay(const GpuTrace &trace, vk::Device &device, const Options &options, Image &frame, Report &report,
            std::string &error, bool restore_edram = true);

} // namespace xf::render
