// A port's own renderer in the console's place (docs/sdk.md, "Native
// rendering"; plan E4): the port draws a game's scene itself on XenoForge's
// Vulkan device, from what its hooks capture of the game's engine, and the
// emulated GPU skips the draws and resolves the port draws instead.
//
// At each swap, on the renderer's thread, the output renderer the port
// registered is called with the device, a command buffer recording into the
// frame's work, an output image to draw the frame into and the guest's front
// buffer as the swap gives it. It returns whether it drew the frame: true, and
// the window shows the output image; false, and the emulated picture is shown
// (a menu, a loading screen, any frame the port yields). While it draws, the
// frames after it skip the draws and resolves the port's pass filter selects
// (all of them without a filter); everything the CPU can observe still runs:
// the command processor's packets, register writes, fences and EVENT_WRITE,
// interrupts, the swaps and their timing, and draws exporting memory
// (memexport), which are never skipped. Occlusion query reports with a
// skipped draw in them are answered with set_occlusion_samples' count, as the
// rexglue fork's native renderer answers them with Xenia's fake count.
//
// The switch from the port's frames to emulated ones is a frame late: the
// frame after one the port drew had its passes skipped, so when the port
// yields it, the window keeps the port's last picture (counted as held), and
// the frame after is the emulated one in full.
//
// Without an output renderer registered, or with the toggle off (enabled()),
// the renderer does what it always does: no call, nothing skipped.
//
//   #include <xenoforge/native_render.h>
//   xf::native::set_output_renderer([](const xf::native::OutputContext &ctx) {
//       if (!scene_ready) return false;                 // the emulated picture
//       VkCommandBuffer cmd = ctx.frame->commands();
//       // ... barriers, vkCmdBeginRendering into ctx.output_view, the scene ...
//       return true;
//   });
//   xf::native::set_pass_filter([](const xf::native::PassInfo &pass) {
//       return pass.surface_pitch >= 1280;              // skip the main scene's passes only
//   });
//
// A port's shaders are SPIR-V it brings itself (compiled by its own CMake,
// glslc from the Vulkan SDK; sdk/examples/native_port).

#pragma once

#include "xenoforge/vk_device.h"

#include <volk.h> // Vulkan without prototypes, its functions loaded at run time (cmake/vulkan.cmake)

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace xf::native {

// ---- what the output renderer gets ----

// The guest's front buffer at the swap: texture fetch constant 0, as the
// swap shows it (address 0 when the constant holds no 2D texture).
struct GuestFrontBuffer {
    uint32_t address = 0;           // physical
    uint32_t width = 0, height = 0; // texels
    uint32_t pitch = 0;             // texels a row
    uint32_t format = 0;            // the guest format (texture.h's format_name)
    bool tiled = false;
    uint32_t endian = 0;
    const uint32_t *fetch = nullptr; // the constant's six words (Frame::texture takes them)
};

// A guest texture as the renderer's texture cache holds it (Frame::texture):
// its image in VK_IMAGE_LAYOUT_GENERAL, usable as a sampled image, and a view
// of it with the fetch constant's swizzle applied: a 2D array (one layer for a
// 2D texture; 1D and stacked textures too), a 3D view or a cube view, as
// `kind` says (the translated shaders' image table: set 1's binding 0, 1 or
// 2). Valid until the frame's work is done on the GPU: ask for it again each
// frame.
//
// What a translated shader's sampling of it takes besides (native_shaders.h's
// SystemConstants, per slot): its components' modes after the swizzle (signs:
// two bits each, 0 unsigned, 1 signed, 2 unsigned biased to -1..1, 3 gamma),
// the integer number format's scales (integer_scale), and, with a signed
// component the format has a signed version for, a view of it in that
// version (signed_view, else null), which the slot's signed_slots names.
struct TextureImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_2D_ARRAY, _3D or _CUBE (kind)
    uint32_t kind = 0;                 // 0 2D array, 1 3D, 2 cube
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    uint32_t signs = 0, integer_scale = 0;
    VkImageView signed_view = VK_NULL_HANDLE;
};

// A resolve as the renderer reads it from the GPU's registers at the copy
// (resolve.h's Info, xenos.h's numbering): PassInfo::resolve_info gives it, so
// a port drawing into render targets of its own does each resolve itself
// (Frame::resolve) where the GPU would have.
struct ResolveInfo {
    // the source, in EDRAM
    bool depth = false;         // the depth buffer, else color target color_target
    uint32_t color_target = 0;  // 0-3
    uint32_t edram_base = 0;    // in tiles, of the source surface
    uint32_t surface_pitch = 0; // in pixels
    uint32_t msaa = 0;          // 0 1x, 1 2x, 2 4x
    uint32_t sample_select = 0;
    uint32_t source_format = 0;                   // a color target format, or the depth format
    uint32_t x = 0, y = 0, width = 0, height = 0; // the rectangle, in surface pixels
    // the destination, in guest memory (as a texture's fetch constant names it)
    uint32_t dest_base = 0, dest_pitch = 0, dest_height = 0;
    uint32_t dest_format = 0, dest_endian = 0;
    int32_t dest_exp_bias = 0;
    bool dest_swap = false;
    uint32_t dest_x = 0, dest_y = 0, dest_slice = 0, dest_number = 0;
    bool dest_array = false;
    uint32_t texture_base = 0, texel_x0 = 0, texel_y0 = 0;
    bool copy = true; // false: a clear alone
    // the clears after the copy, over the same rectangle of the targets
    // RB_DEPTH_INFO and the color target's RB_COLOR_INFO name: the guest's
    // words (RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO; RB_DEPTH_CLEAR)
    bool clear_color = false, clear_depth = false;
    uint32_t clear_color_value[2] = {};
    uint32_t clear_depth_value = 0;
    // the targets those clears hit: RB_COLOR_INFO's of color_target and
    // RB_DEPTH_INFO's EDRAM base (tiles) and format, at surface_pitch
    uint32_t clear_color_base = 0, clear_color_format = 0;
    uint32_t clear_depth_base = 0, clear_depth_format = 0;
    // When the GPU met it (a count of the guest's memory writes the renderer
    // watches; 0 unknown): a port's resolve done later (Frame::resolve_target)
    // leaves the destination to the CPU where the CPU wrote it since, as the
    // emulated resolve, done first, would have been overwritten.
    uint32_t cpu_epoch = 0;
};

// A render target of the port's own as a resolve's source (Frame::resolve):
// single-sampled, at the console's pixels, made in Frame::target_format's
// format for its guest format with VK_IMAGE_USAGE_TRANSFER_SRC_BIT, in
// VK_IMAGE_LAYOUT_GENERAL.
struct TargetImage {
    VkImage image = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};

// One of the renderer's own render targets for a port's draws
// (Frame::target): its cache by EDRAM place, pitch, samples and format, which
// keeps the console's rules (the formats and pitches sharing EDRAM's tiles
// see one another's bits, clears, the resolves read what was written last).
struct TargetRequest {
    bool depth = false;
    uint32_t edram_base = 0, pitch = 0, msaa = 0, format = 0; // RB_SURFACE_INFO's pitch and samples, the info's base and format
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;                  // the pixels the draws may write (the scissor)
};
struct RendererTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // the attachment's (depth: both aspects)
    VkImageView sampled = VK_NULL_HANDLE; // the color, or depth alone
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 0, height = 0; // the image's
    // an 8_8_8_8_GAMMA target kept linear (as RGBA16 unorm): the pixel shader
    // writes linear into it, so SystemConstants::flags[2]'s gamma bit is off
    bool gamma_linear = false;
    uint64_t id = 0; // the same target, frame after frame
};

// The frame's work, while the output renderer runs (and only then).
class Frame {
  public:
    virtual ~Frame() = default;
    // The command buffer to record into now: outside any render pass, with
    // everything the renderer recorded before it done (a full barrier). Ask
    // again after any other call of Frame's: a helper may submit the
    // renderer's work and begin a new command buffer (the queue keeps the
    // order). Leave it outside a render pass. The port's bindings do not
    // outlive the callback (the renderer binds its own again).
    virtual VkCommandBuffer commands() = 0;
    // A guest texture by its fetch constant (six words, host order, as the
    // registers hold them), loaded and detiled by the renderer's texture
    // cache as a draw's would be, its upload recorded before what the port
    // records next. False, and why, when the renderer cannot load it.
    virtual bool texture(const uint32_t fetch[6], TextureImage &out, std::string &why) = 0;
    // Bytes copied into host-visible memory the GPU reads until the frame's
    // work is done: usable as a vertex, index, uniform or storage buffer at
    // (buffer, offset), offset a multiple of alignment (a power of two; the
    // device's uniform alignment at least). False when it does not fit (a
    // few MB at most).
    virtual bool upload(const void *data, size_t bytes, size_t alignment, VkBuffer &buffer, VkDeviceSize &offset) = 0;
    // The same room, not copied into: the bytes to write (host-visible, the
    // GPU reads them as upload's), null when it does not fit. For a block the
    // port fills in part (a stage's constants: the ones its shader reads).
    virtual void *allocate(size_t bytes, size_t alignment, VkBuffer &buffer, VkDeviceSize &offset) = 0;
    // Guest memory where it lies, for the GPU, without a copy of the port's:
    // the renderer's mirror of physical memory (one storage buffer of all of
    // it, the emulated path's shared memory), its pages under [address,
    // address + bytes) made current (what the CPU wrote since given to it, as
    // a draw's vertex data is) before what the port records next. buffer is
    // the whole mirror; offset the physical address (address & 0x1FFFFFFF),
    // so a shader indexes it by guest address (native_shaders.h's binding
    // 1). The bytes are as the guest stores them: big-endian. Returns the
    // bytes given to the GPU for it (0 when it was current already).
    virtual VkDeviceSize guest_memory(uint32_t address, uint32_t bytes, VkBuffer &buffer, VkDeviceSize &offset) = 0;
    // The host format the renderer keeps a render target of this guest
    // format in (RB_COLOR_INFO's or RB_DEPTH_INFO's format), and so the one a
    // port's own targets take for the translated shaders and Frame::resolve
    // to read them as the emulated path's: 2_10_10_10_FLOAT (7e3) as 16-bit
    // floats, 16_16 and 16_16_16_16 (-32..32) as snorm holding the guest's
    // bits (target_constants' scales), 8_8_8_8_GAMMA as RGBA8 holding the
    // gamma-encoded bits; D24S8 as the device's 24-bit unorm depth (else a
    // 32-bit float), D24FS8 as a 32-bit float holding half the depth (0 to 2
    // halved: its clears too). VK_FORMAT_UNDEFINED when the renderer has none.
    virtual VkFormat target_format(bool depth, uint32_t format) = 0;
    // A resolve the port does from a target of its own: the rectangle of
    // `source` copied into guest memory as the destination says, as the
    // emulated path's resolve writes it (format conversion, exponent bias,
    // endianness, tiling), so the textures the guest makes there next
    // (Frame::texture) are the resolve's. The clears after the copy are the
    // port's own. Recorded after what the port recorded so far (outside a
    // render pass: ask for commands() again after it). False, and why, when
    // it cannot (a resolution scale on, a format the resolves do not write).
    virtual bool resolve(const ResolveInfo &info, const TargetImage &source, std::string &why) = 0;
    // The microcode (host order) of a shader the GPU was given, by its hash
    // (PassInfo's vertex_shader, pixel_shader): what a draw ran, where the
    // guest's Direct3D loads another program than its shader object's (a
    // vertex shader whose fetches it fills in from the vertex declaration,
    // or patches for the pixel shader bound). False when none was met.
    virtual bool shader_words(uint64_t hash, std::vector<uint32_t> &words) = 0;
    // The sampler the emulated path samples a texture with: the fetch
    // constant's addressing, border and filters, with a shader's first
    // fetch's filters over them (filters: mag, min, mip, 3 the constant's;
    // anisotropy, 7 the constant's; null: the constant's own;
    // ShaderUses::slot_filters), and the player's filtering settings. Valid
    // until the frame's work is done on the GPU.
    virtual VkSampler sampler(const uint32_t fetch[6], const uint8_t filters[4]) = 0;
    // The renderer's render target for a request, its tiles under the
    // rectangle made current (another format's or pitch's bits moved into
    // it, the clears it was owed drawn), recorded before what the port
    // records next (outside a render pass: ask for commands() again after).
    // In VK_IMAGE_LAYOUT_GENERAL; the port draws into it and the renderer
    // knows those tiles as its. False and why when it has none.
    virtual bool target(const TargetRequest &request, RendererTarget &out, std::string &why) = 0;
    // A resolve as the emulated path does it, from the renderer's own targets
    // (those target() gave the port): the copy, then its clears. Recorded
    // after what the port recorded so far (outside a render pass).
    virtual bool resolve_target(const ResolveInfo &info, std::string &why) = 0;
    // Whether the GPU holds any of these bytes newer than guest memory (a
    // resolve or a memory export wrote them since the CPU last did): a copy a
    // port made of them from guest memory is not what the GPU reads there
    // (Frame::guest_memory gives the GPU's).
    virtual bool gpu_wrote(uint32_t address, uint32_t bytes) = 0;
};

struct OutputContext {
    uint64_t swap = 0; // numbered from 0, as the renderer's swaps are
    // the device: the renderer's, shared with the window. The queue is not
    // the port's to submit on; record into frame->commands() instead.
    vk::Device *device = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    Frame *frame = nullptr;
    // The output image: R8G8B8A8_UNORM (UNORM, shown as it is, without the
    // display's gamma ramp), usable as a color attachment, a storage image, a
    // sampled image and a transfer source and destination. In
    // VK_IMAGE_LAYOUT_GENERAL at the call and to be left in it; its contents
    // are the port's last frame (undefined the first time and after a
    // resize). Sized as set_output_size says, else as the guest's front buffer.
    VkImage output = VK_NULL_HANDLE;
    VkImageView output_view = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_2D
    VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 0, height = 0;
    GuestFrontBuffer guest;
    // the GPU's registers at the swap (xenos.h's indices; the constants
    // below read them) and guest memory's physical view (address 0)
    const uint32_t *registers = nullptr;
    uint8_t *physical_memory = nullptr;
    // this frame's passes were skipped (the port drew the frame before)
    bool suppressed = false;
};

// True: the port drew the frame into ctx.output.
using OutputRenderer = std::function<bool(const OutputContext &ctx)>;

// The output renderer, from any thread (taken at the next swap); an empty
// function removes it.
void set_output_renderer(OutputRenderer renderer);
bool has_output_renderer();

// The output image's size; 0 by 0 (the default): the guest's front buffer's,
// else 1280 by 720.
void set_output_size(uint32_t width, uint32_t height);

// Whether the output image goes through the display's gamma ramp the guest
// set (DC_LUTA_CONTROL's table or piecewise linear ramp), as the guest's front
// buffer does when the window shows it and shots read it: on by default, for
// a port that draws the guest's frame (its front buffer into the output);
// off for one that makes its own final picture.
void set_output_display_gamma(bool on);

// ---- what is skipped ----

// A draw or a resolve, as the pass filter sees it (on the renderer's
// thread, for each one while the port's frames are shown: keep it cheap).
struct PassInfo {
    bool resolve = false;       // a resolve (EDRAM to memory) rather than a draw
    uint32_t surface_pitch = 0; // RB_SURFACE_INFO's, in pixels: a full-screen pass or an off-screen one
    uint32_t color_base = 0;    // RB_COLOR_INFO's EDRAM base (tiles) and format
    uint32_t color_format = 0;
    bool color = true; // a draw writing color (else depth only)
    bool has_pixel_shader = true;
    uint32_t primitive = 0, count = 0;            // a draw's
    uint64_t vertex_shader = 0, pixel_shader = 0; // the shaders' hashes (as verbose draws print them)
    uint32_t copy_dest = 0;                       // a resolve's RB_COPY_DEST_BASE (physical)
    // a resolve's, as the renderer reads its registers (null when it cannot:
    // a resolve the emulated path skips too); valid during the call
    const ResolveInfo *resolve_info = nullptr;
    const uint32_t *registers = nullptr;
    uint64_t swap = 0;
};

// True: skip it. Without a filter every draw and resolve is skipped while
// the port's frames are shown. From any thread (taken at the next swap).
using PassFilter = std::function<bool(const PassInfo &pass)>;
void set_pass_filter(PassFilter filter);

// ---- the GPU's state as Vulkan's: the renderer's own conversions ----
//
// For a port that draws a game's draws itself from their register state
// (native_shaders.h's translated shaders), what the emulated path does with
// the same registers, so a port does not write it again. `registers` is a
// register file indexed by the GPU's register number (xenos.h), covering at
// least 0x2000-0x23FF.

VkCompareOp vk_compare_op(uint32_t function);   // RB_DEPTHCONTROL's depth and stencil functions (3 bits)
VkStencilOp vk_stencil_op(uint32_t op);         // its stencil operations (3 bits)
VkBlendFactor vk_blend_factor(uint32_t factor); // RB_BLENDCONTROL's factors (5 bits)
VkBlendOp vk_blend_op(uint32_t op);             // its operations (3 bits)

// The draw's viewport on a host target of max_x by max_y pixels (Xenia's
// draw_util GetHostViewportInfo at resolution scale 1: PA_CL_CLIP_CNTL,
// PA_CL_VTE_CNTL, PA_SU_SC_MODE_CNTL, PA_SU_VTX_CNTL, PA_CL_VPORT_*,
// PA_SC_WINDOW_OFFSET), and the NDC scale and offset the translated vertex
// shaders take (SystemConstants' ndc_scale, ndc_offset). Empty: nothing drawn.
struct HostViewport {
    VkViewport viewport{};
    float ndc_scale[3] = {1, 1, 1}, ndc_offset[3] = {};
    bool empty = false;
};
HostViewport host_viewport(const uint32_t *registers, float max_x, float max_y);
// The same into a port's own targets (Frame::target_format's): with the
// pixel shader writing depth (oDepth), and with a D24FS8 depth target (kept
// halved).
HostViewport host_viewport(const uint32_t *registers, float max_x, float max_y, bool pixel_writes_depth,
                           bool depth_float24);
// The draw's scissor (PA_SC_WINDOW_SCISSOR_*, PA_SC_SCREEN_SCISSOR_*, the
// window offset), clamped to a target `pitch` pixels wide.
VkRect2D host_scissor(const uint32_t *registers, uint32_t pitch);

// What a draw's targets ask of the translated pixel shader, into its system
// constants (native_shaders.h), as the emulated path's host render targets
// take them: each color target's scale (2 to RB_COLOR_INFO's exponent bias,
// times 1024/32767 for the 16-bit fixed formats kept as snorm, negative for
// 16_16, whose alpha is left out) for the targets `colors_written` names (a
// bit each, the shader's), the gamma targets and the 7e3 ones whose alpha is
// clamped (flags[2]), and a D24FS8 depth target the draw tests or writes
// (flags[3] bit 0: its depth written halved).
struct SystemConstants;
void target_constants(const uint32_t *registers, uint32_t colors_written, SystemConstants &sys);

// ---- every draw, watched: a port's check against the emulated path ----

// Called for every draw and resolve the command processor issues, whether
// the port draws the frame or not (on the renderer's thread, in the order the
// GPU gets them), with the PassInfo the pass filter would get: a port that
// captures the guest's draws itself (replacing its Direct3D) compares what it
// captured with what the GPU got, draw by draw. registers is the GPU's
// register file at that draw (xenos.h's indices: fetch constants from
// 0x4800, float constants from 0x4000, the render state from 0x2000), swap
// the swaps before it. Unset (the default), a draw pays one branch. From any
// thread; taken at the next swap. Keep it cheap: it runs on every draw.
using DrawObserver = std::function<void(const PassInfo &pass)>;
void set_draw_observer(DrawObserver observer);

// What an occlusion query report holds when a skipped draw counted for it
// (default 1000, Xenia's query_occlusion_fake_sample_count).
void set_occlusion_samples(uint32_t samples);

// ---- the toggle: the port's frames or the emulated ones ----

// The switch the player's hotkey (F6) and overlay flip and a port option can
// bind (PortOption::boolean("native_render", "Native renderer",
// xf::native::enabled_flag())); on by default. Read at each swap.
bool &enabled_flag();
inline bool enabled() { return std::atomic_ref<bool>(enabled_flag()).load(std::memory_order_relaxed); }
inline void set_enabled(bool on) { std::atomic_ref<bool>(enabled_flag()).store(on, std::memory_order_relaxed); }
inline void toggle() { set_enabled(!enabled()); }

// The shadow mode: the port draws every frame beside the emulated path,
// for the same frame's two pictures. Nothing is skipped (the pass filter is
// not asked); at each swap the emulated picture is read first (boot --shots:
// <title>_<swap>_emulated.png, renderer Options::on_emulated_frame), then the
// output renderer runs and its picture is the frame's (<title>_<swap>.png,
// what the window shows). The port's resolves write guest memory after the
// emulated path's: the next frame's emulated draws read the port's pictures
// where a frame reads the one before's (temporal effects). Off by default;
// boot --native-shadow turns it on.
void set_shadow(bool on);
bool shadow();

// What it did so far, from any thread.
struct Counters {
    uint64_t frames_native = 0;  // swaps the port drew
    uint64_t frames_yielded = 0; // swaps it was called for and did not draw
    // swaps whose passes were skipped and that the port did not draw (it
    // yielded, or the toggle went off): not shown, the port's last kept
    uint64_t frames_held = 0;
    uint64_t draws_skipped = 0, resolves_skipped = 0;
    uint64_t draws_kept = 0, resolves_kept = 0; // the filter kept them while suppressing
    uint64_t memexport_kept = 0;                // draws exporting memory, never skipped
    uint64_t occlusion_faked = 0;               // reports answered with the fake count
    uint64_t frames_shadowed = 0;               // swaps the port drew in the shadow mode (nothing skipped)
};
Counters counters();
// whether the last swap showed the port's frame
bool active();

// ---- the guest's data, as the GPU reads it ----

// a word as the GPU swaps it (command_processor.h's Endian: 0 none, 1 8-in-16,
// 2 8-in-32, 3 16-in-32)
constexpr uint32_t swap_word(uint32_t v, uint32_t endian) {
    switch (endian & 3) {
    case 1: return (v << 8 & 0xFF00FF00u) | (v >> 8 & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return v >> 16 | v << 16;
    default: return v;
    }
}

// Vertex fetch constant `index` (0-95: two words each, three to a texture
// fetch constant's place): the buffer's physical address and size in bytes,
// and how its words are swapped. valid: the constant's type is a vertex's.
struct VertexFetch {
    bool valid = false;
    uint32_t address = 0, bytes = 0, endian = 0;
};
VertexFetch vertex_fetch(const uint32_t *registers, uint32_t index);
// Texture fetch constant `index` (0-31): its six words, for Frame::texture.
const uint32_t *texture_fetch(const uint32_t *registers, uint32_t index);
// Float constant `index` (0-511: the vertex shader's 0-255, the pixel
// shader's 256-511), boolean constant `index` (0-255) and loop constant
// `index` (0-31), as the shaders read them.
void float_constant(const uint32_t *registers, uint32_t index, float out[4]);
bool bool_constant(const uint32_t *registers, uint32_t index);
uint32_t loop_constant(const uint32_t *registers, uint32_t index);

// count words of guest memory at a physical address, swapped as endian says
// (a vertex buffer's words as its fetch constant gives them: floats are
// std::bit_cast<float>(word))
void read_words(const uint8_t *physical, uint32_t address, uint32_t endian, uint32_t *out, size_t count);
void read_floats(const uint8_t *physical, uint32_t address, uint32_t endian, float *out, size_t count);
// count indices of an index buffer (16 or 32 bits each), swapped as endian says
void read_indices(const uint8_t *physical, uint32_t address, uint32_t endian, bool index32, uint32_t *out,
                  size_t count);

} // namespace xf::native
