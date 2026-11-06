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
    uint64_t render_targets = 0, textures = 0, pipelines = 0, shaders = 0; // textures: loads
    // The texture cache: the textures it holds now and the GPU memory their
    // images take (the most it took), those destroyed by the memory limits
    // (Options::texture_memory_*) and to make room in a full bindless table,
    // and the loads refused for want of room there.
    uint64_t textures_held = 0, texture_bytes = 0, texture_bytes_peak = 0;
    uint64_t textures_evicted = 0, textures_evicted_for_room = 0, textures_refused = 0;
    uint64_t occlusion_queries = 0, zpd_reports = 0; // host queries begun, reports answered with their counts
    double seconds = 0;                          // spent in the renderer's calls, of which:
    double submit_seconds = 0;                   //   submitting work and waiting for the GPU, of which:
    double wait_seconds = 0;                     //     waiting for the GPU to finish work
    double check_seconds = 0;                    //   checking textures against the memory under them
    double decode_seconds = 0;                   //   decoding textures on the CPU and recording their uploads
    double vertex_seconds = 0;                   //   comparing vertex data with the mirror's, uploading what differs
    double resolve_seconds = 0;                  //   recording resolves (their waits counted in submit_seconds too)
    double writeback_seconds = 0;                //   copying what resolves wrote into guest memory, once done
    double pipeline_seconds = 0;                 //   making pipelines (the driver compiling the shaders)
    uint64_t pipeline_cache_read = 0;            // bytes of pipeline cache read from disk at the start
    uint64_t pipeline_cache_written = 0;         // bytes of it written back, the last time
    uint64_t vertex_bytes = 0;                   // the vertex data uploaded (the pages that differed)
    uint64_t writeback_bytes = 0;                // what resolves wrote, copied into guest memory
    uint64_t submissions = 0, waits = 0;         // command buffers submitted; waits for the GPU
    std::map<std::string, uint64_t> skipped;     // draws not done, by reason
    std::map<std::string, std::vector<uint64_t>> skipped_at; // the first swaps (by number) each reason came in
    std::map<std::string, uint64_t> approximate; // done, with something not exact yet, by what
    // What the draws and packets asked for, done or not, by what: the
    // features the corpus and the games use, to order the work by (user clip
    // planes, line widths, gamma targets, screen-extent and VIZ queries,
    // immediate indices)
    std::map<std::string, uint64_t> used;
    // The GPU's time by kind of work (Options::gpu_timing): timestamps
    // written where the kind changes, each interval counted to the kind that
    // began it; the submissions' first to last timestamps in all
    struct GpuTime {
        double seconds = 0;
        uint64_t intervals = 0;
    };
    std::map<std::string, GpuTime> gpu_time;
    double gpu_seconds = 0;
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
    // The display's gamma (display_gamma below): the front buffer's bits a
    // component the display reads (8, or 10 for 2_10_10_10), DC_LUTA_CONTROL
    // at the swap, the 256-entry ramp (10 bits a component, red highest) and
    // the piecewise linear one (128 entries a component, red, green, blue;
    // base in bits 0-15, delta in 16-31)
    uint32_t bits = 8;
    uint32_t luta_control = 0;
    std::array<uint32_t, 256> gamma{};
    std::array<uint32_t, 128 * 3> pwl{};
};

// A component through the display's look-up table A by DC_LUTA_CONTROL (the
// register libxenon and Direct3D program, 0x1930), as AMD's RV630 Register
// Reference Guide gives its increment exponent: 0 the 256-entry table,
// indexed by the component's 8 bits (a 10-bit component's top 8, which no
// reference states), else the piecewise linear ramp of 128 entries, indexed
// by bits INC+6 to INC, the bits below it times the entry's delta over 2^INC
// added to its base. The RV630 has an increment per component (blue bits
// 0-3, green 8-11, red 16-19), but Direct3D writes 3 for a 10-bit front
// buffer, which Xenia (and the console's picture as Xenia draws it) takes as
// the ramp for all three: the blue field's increment applies to every
// component here. Xenia picks the table or the ramp by the front buffer's
// format and always steps by 8 (DC_LUTA_CONTROL 0 and 3, what Direct3D
// writes for 8- and 10-bit fronts); both agree there. value: the component in `bits` bits; channel: 0 red, 1
// green, 2 blue. The result: 10 bits.
uint32_t display_gamma(uint32_t value, uint32_t bits, uint32_t channel, uint32_t luta_control,
                       const std::array<uint32_t, 256> &table, const std::array<uint32_t, 128 * 3> &pwl);

// When the CPU sees what a resolve wrote (Xenia's readback_resolve option):
// the GPU's copy of guest memory always has it, and textures read it there.
enum class ReadbackResolve {
    none, // never: guest memory keeps its bytes (Xenia's default since December 2025)
    fast, // a frame late: each resolve gives guest memory the bytes the same resolve wrote the frame before
    full, // at once: each resolve waits for the GPU and gives guest memory its bytes
};

struct Options {
    bool verbose = false; // a line per draw and resolve on stderr
    ReadbackResolve readback_resolve = ReadbackResolve::none;
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
    // Graphics pipelines made off the renderer's thread, as Xenia's pipeline
    // creation threads with its async_shader_compilation (on by default
    // there; its trace dump turns it off): a draw whose pipeline is not made
    // yet queues it for worker threads and is skipped meanwhile, counted as
    // "pipeline not ready (being made)". Off: each pipeline is made when its
    // first draw needs it, on the renderer's thread, as a replay must.
    // Pipelines of draws without a pixel shader are made at once either way
    // (Xenia's too: they are quick). The threads: -1 three quarters of the
    // logical cores (Xenia's d3d12_pipeline_creation_threads default).
    bool async_pipelines = false;
    int pipeline_threads = -1;
    // what replay() has its processor write in occlusion query reports
    // (live, the caller sets its processor's: tools/game.cpp)
    gpu::OcclusionQueries occlusion_queries = gpu::OcclusionQueries::Fake;
    // GPU timestamps around each kind of work (draws by render pass, EDRAM
    // loads and stores, resolves, texture loads, the swap's read-back), summed
    // into Report::gpu_time as submissions finish
    bool gpu_timing = false;
    // with it, each draw timed on its own, counted to its primitive and
    // shaders ("draw: ..."), the time between draws apart
    bool gpu_timing_draws = false;
    // Rectangle and point lists expanded in the vertex shader, as on a device
    // without geometry shaders, rather than by geometry shaders as Xenia's
    // D3D12 backend draws them (XF_VK_EXPAND_IN_VS=1 sets it too)
    bool expand_in_vertex_shader = false;
    // D24FS8 depth to 20e4 rounded to the nearest even rather than towards
    // zero, where the host's 32-bit depth goes into EDRAM's 24 bits (Xenia's
    // depth_float24_round, off by default as there: truncation keeps a
    // "greater or equal" test passing on a second pass over depth that lost
    // the host's precision)
    bool depth_float24_round = false;
    // 8_8_8_8_GAMMA color targets kept linear as RGBA16 unorm, so that they
    // blend in linear as the console's do, their gamma made and undone where
    // they move through EDRAM (Xenia's gamma_render_target_as_unorm16, on by
    // default as there); off, or on a device that cannot blend that format,
    // the pixel shader writes the gamma curve and the blend works on it
    bool gamma_render_target_as_unorm16 = true;
    // The depth of draws into a D24FS8 target with the depth test on written
    // by the pixel shader rounded to 20e4 (towards zero, or to the nearest
    // even with depth_float24_round), so the host's depth holds only what
    // EDRAM's 24 bits can, and a pass over depth that went through EDRAM
    // compares equal; the shader runs per sample under MSAA. Xenia's
    // depth_float24_convert_in_pixel_shader, off by default as there (the
    // host's depth is kept beside EDRAM instead, renderer.md).
    bool depth_float24_convert_in_pixel_shader = false;
    // The texture cache's memory limits, in MB of the GPU memory its images
    // hold, as Xenia's texture_cache_memory_limit_* (its defaults): above the
    // soft limit, textures unused for soft_lifetime seconds are destroyed,
    // the least recently used first; above the hard limit, any whose last
    // use the GPU is done with. render_to_texture is the part of both that
    // a draw resolution scale would multiply (none here: it adds nothing).
    uint32_t texture_memory_soft_mb = 384;
    uint32_t texture_memory_hard_mb = 768;
    uint32_t texture_memory_soft_lifetime_s = 30;
    uint32_t texture_memory_render_to_texture_mb = 24;
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
// player leaves it. With repeat above 1 the trace plays that many times
// through the same renderer, as a game draws the same frame again (memory
// the trace gives again unchanged is not written again), and the report's
// GPU time is that of the fastest play after the first (each finished on
// the GPU before the next: other work sharing the GPU only adds to a play's).
bool replay(const GpuTrace &trace, vk::Device &device, const Options &options, Image &frame, Report &report,
            std::string &error, bool restore_edram = true, uint32_t repeat = 1);

} // namespace xf::render
