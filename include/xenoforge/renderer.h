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
#include <atomic>
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
    // replay only, with repeat: the time each play after the first spent in the command processor
    // and what it calls (the renderer's CPU side), without the trace's walk and the memory it
    // restores: the work of a game's command thread for the frame
    std::vector<double> command_seconds;
    uint64_t draws = 0, drawn = 0;    // outside copy mode; of them, done
    uint64_t resolves = 0, swaps = 0; // draws in copy mode; XE_SWAP packets
    uint64_t frames = 0;              // front buffers read back (the swaps asked for)
    uint64_t render_targets = 0, textures = 0, pipelines = 0, shaders = 0; // textures: loads
    uint64_t samplers_reused = 0; // samplers destroyed to free their slot in the full bindless table
    // The texture cache: the textures it holds now and the GPU memory their
    // images take (the most it took), those destroyed by the memory limits
    // (Options::texture_memory_*) and to make room in a full bindless table,
    // and the loads refused for want of room there.
    uint64_t textures_held = 0, texture_bytes = 0, texture_bytes_peak = 0;
    uint64_t texture_views = 0; // the views of them held (one a swizzle and sign constants read them by)
    // the loads (textures) by why: a texture not held, one whose memory the
    // CPU wrote (its watches), one whose memory a resolve or a memory export
    // wrote (render to texture, loaded again from the mirror on the GPU)
    uint64_t texture_loads_new = 0, texture_loads_cpu_written = 0, texture_loads_gpu_written = 0;
    uint64_t resolve_targets_written = 0; // resolves that wrote the texture they feed too (not loaded again)
    uint64_t resolve_targets_reclaimed = 0; // places in the resolves' table taken from a texture unused since
    uint64_t textures_scaled = 0;         // textures made the resolution scale's (Options::resolution_scale)
    uint64_t textures_evicted = 0, textures_evicted_for_room = 0, textures_refused = 0;
    uint64_t occlusion_queries = 0, zpd_reports = 0; // host queries begun, reports answered with their counts
    // screen extent reports (EVENT_WRITE_EXT) the sink was asked for, and
    // those it bounded (the others the whole screen)
    uint64_t extent_reports = 0, extent_bounded = 0;
    // VIZ queries ended, those whose count predicates their draws (the
    // device's conditional rendering), and the draws drawn so predicated
    uint64_t viz_queries = 0, viz_armed = 0, viz_predicated = 0;
    double seconds = 0;                          // spent in the renderer's calls, of which:
    double submit_seconds = 0;                   //   submitting work and waiting for the GPU, of which:
    double wait_seconds = 0;                     //     waiting for the GPU to finish work
    double check_seconds = 0;                    //   checking textures against the memory under them
    double decode_seconds = 0;                   //   decoding textures on the CPU and recording their uploads
    double vertex_seconds = 0;                   //   comparing vertex data with the mirror's, uploading what differs
    double resolve_seconds = 0;                  //   recording resolves (their waits counted in submit_seconds too)
    double writeback_seconds = 0;                //   copying what resolves wrote into guest memory, once done
    double pipeline_seconds = 0;                 //   making pipelines (the driver compiling the shaders)
    // translating shaders to SPIR-V and making their modules on the
    // renderer's thread (a shader's first form and its other forms), the
    // modules so made, the longest one, and the most in one frame
    double translate_seconds = 0, translate_max = 0, translate_frame_max = 0;
    uint64_t translations = 0;
    uint64_t pipeline_cache_read = 0;            // bytes of pipeline cache read from disk at the start
    uint64_t pipeline_cache_written = 0;         // bytes of it written back, the last time
    uint64_t vertex_bytes = 0;                   // the vertex data uploaded (the pages that differed)
    uint64_t writeback_bytes = 0;                // what resolves wrote, copied into guest memory
    uint64_t submissions = 0, waits = 0;         // command buffers submitted; waits for the GPU
    uint64_t shader_loads_again = 0;             // IM_LOADs of a shader from its unwritten words (none copied)
    // Pipelines (Options::pipeline_stand_in): besides the whole ones counted
    // in `pipelines` above, the libraries of their parts made, the pipelines
    // linked from them and the placeholders made; the draws drawn by a
    // stand-in (a placeholder or a linked pipeline) while their own pipeline
    // was made
    uint64_t pipeline_libraries = 0, pipelines_linked = 0, pipeline_placeholders = 0, stand_in_draws = 0;
    // The generic shaders (Options::generic_shaders): the shaders' programs
    // written to their buffer, the generic stand-ins linked, the draws they
    // drew; and why a draw waiting for its pipeline could not be drawn by
    // them, by reason
    uint64_t generic_programs = 0, generic_pipelines = 0, generic_draws = 0;
    std::map<std::string, uint64_t> generic_refused;
    // The shader and pipeline storage (Options::shader_storage): the shaders
    // and pipeline descriptions this run added to it; the pipelines and
    // libraries the launcher's build step had made into the pipeline cache
    // for this device and driver, made at once from it (render::precompile)
    uint64_t shaders_stored = 0, pipelines_stored = 0;
    uint64_t pipelines_prebuilt = 0, pipeline_libraries_prebuilt = 0;
    // of those, the ones the pipeline cache did not hold after all (the
    // driver would have compiled them: a cache another build of the driver
    // or of the translator wrote), left to the workers instead
    uint64_t pipelines_prebuilt_missed = 0, pipeline_libraries_prebuilt_missed = 0;
    // The pipelines made on the workers, and how long their draws went
    // without one (from the first draw asking to the first drawn, by the
    // pipeline or its stand-in): in all and at most
    uint64_t pipeline_waits = 0;
    double pipeline_wait_seconds = 0, pipeline_wait_max = 0;
    // Hitches: the frames in which the renderer's thread spent more than 4 ms
    // making pipelines (and the most it spent in one), and the intervals
    // between swaps longer than 50 and 100 ms, whatever their cause
    uint64_t pipeline_hitches = 0;
    double pipeline_frame_max = 0;
    uint64_t swap_gaps_50ms = 0, swap_gaps_100ms = 0;
    double swap_gap_max = 0;
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
    // The clock read around the work done at every draw, texture and shader load too: without it
    // Report::check_seconds and vertex_seconds stay 0 and Report::seconds leaves out the calls that
    // neither draw, resolve nor swap. For the tools that print them (XF_RENDERER_CPU_TIMES=1 anywhere).
    bool cpu_times = false;
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
    // The title's shader and pipeline storage (shader_storage.h; docs/
    // renderer.md, "Shaders and pipelines at the launcher's build step"):
    // every shader met and every pipeline's description appended to it, for
    // the build step (render::precompile), whose records there name the
    // pipelines and libraries it made into the pipeline cache; those are
    // made at once on the renderer's thread (a lookup in the cache) rather
    // than on the workers. Empty: none.
    std::filesystem::path shader_storage;
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
    // What a draw is drawn with while its pipeline is made on the workers
    // (async_pipelines; docs/renderer.md, "Pipelines while they are made"):
    enum class PipelineStandIn {
        // nothing: the draw is skipped, as Xenia's D3D12 backend skips it
        skip,
        // a placeholder pipeline made at once on the renderer's thread, the
        // guest's vertex shader with a pixel shader writing transparent
        // black, as Xenia's Vulkan backend draws (its pipeline hot-swap)
        placeholder,
        // a pipeline linked at once from libraries of its four parts
        // (VK_EXT_graphics_pipeline_library's fast linking), the libraries of
        // shaders made on the workers too and the draw skipped until they
        // are; the whole pipeline made after them, when nothing else waits.
        // Where the device has no such libraries, skip. The default: it
        // measured best (docs/renderer.md)
        library,
    };
    PipelineStandIn pipeline_stand_in = PipelineStandIn::library;
    // The generic shaders (docs/renderer.md, "The generic shaders"): with
    // library stand-ins, a draw whose pipeline and exact stand-in are not
    // made yet is drawn by a pipeline linked from the libraries of an
    // interpreter of the microcode (generic.glsl) that runs its shaders from
    // a buffer; those libraries are few (by state, not by shader), made once
    // on the workers and kept in the pipeline cache. XF_GENERIC_SHADERS=0
    // turns it off.
    bool generic_shaders = true;
    // Every draw the generic shaders can draw drawn by them, as whole
    // pipelines made at once (xfreplay --generic): to check what they draw
    // against the translated shaders' pictures
    bool generic_only = false;
    // Every draw drawn by a pipeline linked from libraries, made at once,
    // never by the whole pipeline: to check that linked pipelines draw what
    // whole ones do (xfreplay --linked-pipelines); needs the device's
    // libraries
    bool linked_pipelines_only = false;
    // Resolves write the texture a later draw reads over their destination
    // as well as memory, instead of the texture being loaded again from
    // memory (beyond Xenia; docs/renderer.md, "Resolve targets"). On where
    // the device updates storage image descriptors after binding;
    // true (or XF_RESOLVE_TARGETS=0) turns it off: Xenia's reload.
    bool no_resolve_targets = false;
    // what replay() has its processor write in occlusion query reports
    // (live, the caller sets its processor's: tools/game.cpp)
    gpu::OcclusionQueries occlusion_queries = gpu::OcclusionQueries::Fake;
    // and in screen extent reports (live, the caller's: XF_SCREEN_EXTENTS)
    gpu::ScreenExtents screen_extents = gpu::ScreenExtents::Measured;
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
    // 2x and 4x host render targets rasterized at the Xenos's sample positions
    // rather than the host's standard ones, where the device places samples
    // (VK_EXT_sample_locations): edges, alpha to coverage and
    // the depth a resolve takes from a sample then lie where the console's
    // do, as on the interlock path, which always takes them (Xenia's
    // xenos_sample_positions, off by default there; on here since 2026-11-09:
    // the host path's edges come within a few pixels of the exact path's
    // for a fiftieth more GPU time; XF_XENOS_SAMPLE_POSITIONS=1 or 0
    // overrides)
    bool xenos_sample_positions = true;
    // The depth of draws into a D24FS8 target with the depth test on written
    // by the pixel shader rounded to 20e4 (towards zero, or to the nearest
    // even with depth_float24_round), so the host's depth holds only what
    // EDRAM's 24 bits can, and a pass over depth that went through EDRAM
    // compares equal; the shader runs per sample under MSAA. Xenia's
    // depth_float24_convert_in_pixel_shader, off by default as there (the
    // host's depth is kept beside EDRAM instead, renderer.md).
    bool depth_float24_convert_in_pixel_shader = false;
    // How EDRAM is drawn into (docs/renderer.md, "Exact EDRAM through fragment
    // shader interlock"): host render targets (images per EDRAM range, their
    // contents moved through the EDRAM buffer when another takes their
    // tiles: Xenia's render_target_path_vulkan=fbo, its default), or the
    // pixel shader reading and writing the EDRAM buffer itself under fragment
    // shader interlock (Xenia's fsi and its Direct3D 12 backend's rov:
    // blending, the float formats, 20e4 depth and the stencil exact). fsi
    // needs VK_EXT_fragment_shader_interlock, host where the device lacks it;
    // automatic is the default the renderer chooses (docs/renderer.md says
    // which and why). XF_RENDER_TARGET_PATH=fsi or host forces one.
    enum class RenderTargetPath { automatic, host, fsi };
    RenderTargetPath render_target_path = RenderTargetPath::automatic;
    // The interlock path's render target state (formats, write masks,
    // blending, the depth and stencil tests) as specialization constants of
    // the pipeline, so the driver drops what does not apply, rather than read
    // from the draw's constants (Xenia's, all dynamic; fewer pipelines).
    // XF_FSI_DYNAMIC=1 turns it off.
    bool fsi_specialized = true;
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
    // The resolution scale (docs/renderer.md, "Resolution scale"; render_scale.h), in quarters: 4 draws at the
    // console's size, 2-32 at 50-800% of it, as RPCS3's resolution scale and Xenia's draw_resolution_scale. Render
    // targets and EDRAM drawn at it, resolves read it and write the textures they feed at it; guest memory, and so
    // everything the CPU reads (read-backs, memory exports, the front buffer a game copies) at the console's size.
    // XF_RESOLUTION_SCALE=<percent> sets it too.
    uint32_t resolution_scale = 4;
    // RPCS3's minimum scalable dimension, in console pixels: a resolve or a texture smaller than it along either
    // axis stays at the console's size
    uint32_t min_scalable = 16;
    // Texture filtering beyond the game's (set_texture_filtering changes them live): anisotropy forced (0: the
    // game's; else 1, 2, 4, 8 or 16) on mipmapped textures the game filters linearly, as Xenia's
    // anisotropic_override; and the filters: 0 the game's, 1 trilinear (linear between mips where the game filters
    // each linearly), 2 nearest everywhere
    uint32_t anisotropy = 0;
    uint32_t texture_filter = 0;
};

// The render target path a renderer takes with these options on the device
// (Options::render_target_path, then XF_RENDER_TARGET_PATH): fsi or host,
// never automatic. replay_test picks its pictures by it.
Options::RenderTargetPath render_target_path(const vk::Device &device, const Options &options);

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
    // Options::anisotropy and texture_filter changed while it runs: the draws after it sample with them (their
    // samplers made as they are asked for); from any thread
    virtual void set_texture_filtering(uint32_t anisotropy, uint32_t filter) = 0;
    // the resolution scale it draws at, in quarters (Options::resolution_scale, or less where the device could not
    // hold EDRAM at that scale: the notice says so), and why it differs from the one asked for ("" when it does not)
    virtual uint32_t resolution_scale() const = 0;
    virtual std::string scale_notice() const = 0;
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

// The launcher's build step's part of the renderer (docs/renderer.md,
// "Shaders and pipelines at the launcher's build step"): the shaders a
// title's storage holds (Options::shader_storage), and the extra ones given
// (those found on the disc), translated; every pipeline description there
// made into the pipeline cache (Options::pipeline_cache) as the libraries of
// its four parts (with the device's graphics pipeline libraries) and whole,
// on worker threads; the cache written; and a record of what was made
// appended to the storage for this device and driver, so the renderer makes
// those at once while playing. Descriptions a record already names for the
// device and driver are not made again while the cache is still theirs.
struct PrecompileOptions {
    bool whole_pipelines = true;                 // besides the libraries
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> extra_shaders; // type (0 vertex, 1 pixel), microcode
    // The extra shaders that no run met have no pipeline's description, so
    // no state of their own: each is made into the library of its shader's
    // part (the pre-rasterization shaders' for a vertex shader, the fragment
    // shader's for a pixel shader) in the states the storage's descriptions
    // give that part most often, this many of them, its form and fixed state
    // as theirs with the shader replaced. 0: none.
    uint32_t extra_variants = 4;
    std::function<void(uint64_t done, uint64_t total)> progress;
    const std::atomic<bool> *cancel = nullptr;
};
struct PrecompileReport {
    // the pipeline cache read was this device's and driver's (false: none,
    // or another's, so everything was made again)
    bool cache_matched = false;
    uint64_t cache_read = 0, cache_written = 0; // bytes
    uint64_t shaders = 0, shaders_failed = 0;   // from the storage, translated or not
    uint64_t extra_shaders = 0, extra_failed = 0, extra_new = 0; // the extra ones; not in the storage
    // extra vertex shaders as Direct3D keeps them before binding, their
    // fetches' formats left for the vertex declaration
    // (ucode::awaits_vertex_declaration): not translated, not failures
    uint64_t extra_undeclared = 0;
    uint64_t descriptions = 0, descriptions_failed = 0, descriptions_done_before = 0;
    uint64_t libraries = 0, libraries_failed = 0, pipelines = 0, pipelines_failed = 0;
    // PrecompileOptions::extra_variants: the states taken for them (of the
    // distinct ones the descriptions give the vertex and the pixel shaders'
    // parts), and of the libraries counted above those of extra shaders, made
    // and failed; variants not made here (the form cannot be made for that
    // shader) and those made before
    uint64_t variant_states = 0, variant_states_seen = 0;
    // the check of those states: of the libraries runs made of the extra
    // shaders they did meet, how many lie in the states taken (a guess of how
    // many of an unmet shader's the step makes ahead; those shaders weigh in
    // the choice of the states too)
    uint64_t variant_check_libraries = 0, variant_check_covered = 0;
    uint64_t extra_libraries = 0, extra_libraries_failed = 0, extra_libraries_unmade = 0,
             extra_libraries_done_before = 0;
    double seconds = 0, driver_seconds = 0; // in all; the workers' time making them
};
bool precompile(vk::Device &device, const Options &options, const PrecompileOptions &precompile,
                PrecompileReport &report, std::string &error);

} // namespace xf::render
