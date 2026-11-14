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
// of it as a 2D array (one layer for a 2D texture) with the fetch constant's
// swizzle applied. Valid until the frame's work is done on the GPU: ask for
// it again each frame.
struct TextureImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_2D_ARRAY
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
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
    const uint32_t *registers = nullptr;
    uint64_t swap = 0;
};

// True: skip it. Without a filter every draw and resolve is skipped while
// the port's frames are shown. From any thread (taken at the next swap).
using PassFilter = std::function<bool(const PassInfo &pass)>;
void set_pass_filter(PassFilter filter);

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
