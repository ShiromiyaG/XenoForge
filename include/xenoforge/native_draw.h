// A port's renderer drawing a game's draws from their register state
// (docs/sdk.md, "Drawing the guest's draws"): what a port that replaces the
// guest's Direct3D needs between the draws it captures and Vulkan, so it
// writes none of it again.
//
// Two levels:
// - draw_setup / place_draw: a draw's registers (the render state at
//   0x2000-0x23FF, as the GPU would get them) and its shaders' uses
//   (native_shaders.h) as Vulkan's state, the way the emulated path takes the
//   same registers: the targets the draw writes, the topology and how its
//   indices are converted, the shaders' forms (ShaderVariant), the pipeline's
//   fixed function (cull, polygon modes, depth and stencil, blending with the
//   console's minimum and maximum factors, the samples), the dynamic state
//   (viewport, scissor, stencil masks and references, blend constants, the
//   polygon offset in the target's units) and the system constants (NDC,
//   alpha test, alpha to coverage, the position's formats, the pixel
//   parameters, point sizes, user clip planes, the targets' scales). No GPU
//   work: a test or a tool can call them.
// - Drawer: draws a frame's draws, clears and resolves with them into the
//   renderer's own render targets (Frame::target: the console's EDRAM rules
//   as the emulated path keeps them, formats and pitches sharing tiles see
//   one another's bits) and resolves them as the emulated path does
//   (Frame::resolve_target); pipelines, samplers with the shaders' filter
//   overrides, descriptor sets with the stand-in images the translated
//   shaders need at each image table's 0, and the guest's front buffer
//   presented, all in the output renderer (native_render.h). Draws exporting
//   memory are left to the emulated path.
//
//   xf::native::Drawer drawer(shader_directory);           // the port's, one for the run
//   xf::native::set_output_renderer([&](const xf::native::OutputContext &ctx) {
//       std::string why;
//       if (!drawer.begin(ctx, why)) return false;
//       for (const Op &op : frame.ops) {                     // what the port captured
//           bool ok = op.kind == Op::Draw    ? drawer.draw(op.draw, why)
//                     : op.kind == Op::Clear ? drawer.clear(op.clear, why)
//                                            : drawer.resolve(op.resolve, why);
//           if (!ok) return drawer.cancel(), false;          // the emulated picture
//       }
//       return drawer.present(ctx.guest.fetch, why) && drawer.end();
//   });

#pragma once

#include "xenoforge/native_render.h"
#include "xenoforge/native_shaders.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xf::native {

// ---- a draw's registers as Vulkan's state ----

// One EDRAM target a draw writes or tests.
struct DrawTarget {
    bool used = false;
    uint32_t edram_base = 0; // tiles
    uint32_t format = 0;     // RB_COLOR_INFO's color format (0-15), or RB_DEPTH_INFO's (0 D24S8, 1 D24FS8)
};

// How a draw's indices become the host's (convert_indices).
enum class IndexForm : uint8_t {
    AsGiven,    // as they are (a restart index where the guest's resets)
    Quads,      // quad lists: two triangles a quad, (0, 1, 3) and (1, 2, 3)
    Primitives, // line loops closed, quad strips' whole quads, polygons of 3 or more: each between resets
    Rectangles, // a rectangle list: each rectangle's three guest indices, four times (VertexForm::RectangleList)
    Points,     // a point list: four host indices a point, its index shifted left by 2 and the corner (PointList)
};

struct DrawSetup {
    std::string why;  // not empty: the draw cannot be drawn (what is missing)
    bool empty = false; // it writes nothing (no target written, or an empty viewport or scissor)
    std::vector<const char *> approximate; // what it does as Xenia does, not as the console (for a port's count)

    // the targets (RB_SURFACE_INFO, RB_COLOR_INFO 0-3, RB_DEPTH_INFO)
    uint32_t pitch = 0, msaa = 0; // pixels; 0 1x, 1 2x, 2 4x
    DrawTarget color[4], depth;
    bool pixel_shader = true; // the draw runs its pixel shader (else depth alone)
    bool pixel_writes_depth = false; // its pixel shader writes oDepth (the viewport's depth range then 0-1)

    // the primitive
    uint32_t primitive = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool restart = false;     // the pipeline's primitive restart (UINT32_MAX in the host's indices)
    bool reset = false;       // the guest's reset index cuts its strips (PA_SU_SC_MODE_CNTL's multi_prim_ib_ena)
    uint32_t reset_index = 0; // VGT_MULTI_PRIM_IB_RESET_INDX
    IndexForm indices = IndexForm::AsGiven;

    // the shaders' forms (ShaderCache::get's variant)
    ShaderVariant vertex, pixel;

    // the pipeline's fixed function
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depth_clamp = false;
    bool depth_test = false, depth_write = false, stencil_test = false;
    VkCompareOp depth_compare = VK_COMPARE_OP_ALWAYS;
    VkStencilOpState stencil_front{}, stencil_back{};
    VkPipelineColorBlendAttachmentState blend[4] = {};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    // the dynamic state (place_draw fills the viewport, the scissor and the depth bias)
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend_constants[4] = {};
    uint32_t stencil_reference[2] = {}, stencil_compare_mask[2] = {}, stencil_write_mask[2] = {}; // front, back
    float line_width = 1.0f;
    float depth_bias_constant = 0.0f, depth_bias_slope = 0.0f; // the target's units (place_draw)
    // how they are set: 0 vkCmdSetDepthBias; with VK_EXT_depth_bias_control, vkCmdSetDepthBias2EXT exact,
    // 1 in the format's units, 2 a float format's forced to 24-bit unorm ones
    uint8_t depth_bias_representation = 0;
    float poly_offset[2] = {}; // the guest's: PA_SU_POLY_OFFSET_*_SCALE (per 1/16 pixel) and _OFFSET

    // the system constants: all but the textures' (Drawer, or the port: slots, signs, signed_slots,
    // integer_scale), with NDC, point sizes and the targets' scales from place_draw
    SystemConstants system{};

    // the textures the shaders sample (a bit per fetch slot) and the filters their first fetches give
    // over the fetch constants' (ShaderUses::slot_filters' form: 3 and 7 the constant's)
    uint32_t texture_slots = 0;
    uint8_t slot_filters[32][4] = {};

    // a key of everything above a pipeline is made from (with the shaders' hashes and the targets'
    // host formats, which the port adds)
    uint64_t pipeline_key = 0;
};

// The device's features a draw's state takes (vk::Device's; all on when null).
// registers: the GPU's register file indexed by register number (xenos.h's),
// covering 0x2000-0x23FF. primitive: VGT_DRAW_INITIATOR's type. ps: null for a
// draw with no pixel shader bound.
DrawSetup draw_setup(const uint32_t *registers, uint32_t primitive, const ShaderUses &vs, const ShaderUses *ps,
                     const vk::Device *device = nullptr);

// The rest of the setup once the draw's targets are known: the viewport and
// scissor on targets of width by height pixels, the NDC scale and offset,
// point sizes, the targets' scales (target_constants), and the polygon offset
// in the units of the depth target's host format (Frame::target_format's;
// UNDEFINED with no depth target), as the emulated path sets them.
void place_draw(DrawSetup &setup, const uint32_t *registers, uint32_t width, uint32_t height, VkFormat depth_format,
                const vk::Device *device = nullptr);

// A draw's host indices (32-bit, UINT32_MAX restarts) from the guest's, as
// setup.indices says: `guest` in host order (null: a draw of `count`
// vertices from `first`, as non-indexed). Rectangle lists come out as three
// indices a corner (a uvec3 vertex attribute, rectangles * 4 of them).
// Empty when it draws nothing.
void convert_indices(const DrawSetup &setup, const uint32_t *guest, uint32_t count, uint32_t first,
                     std::vector<uint32_t> &out);

// The value a clear of a target writes, from the guest's words (RB_COLOR_CLEAR
// and RB_COLOR_CLEAR_LO; RB_DEPTH_CLEAR), in the host format a port's target of
// that guest format takes (Frame::target_format's): 7e3 as floats, 16-bit
// fixed formats as snorm, D24FS8 halved.
VkClearValue clear_value(bool depth, uint32_t format, const uint32_t words[2]);
// The same from Direct3D's floats (a clear's color, its depth and stencil):
// scaled as the target keeps them.
VkClearValue clear_value(bool depth, uint32_t format, const float color[4], float z, uint32_t stencil);

// ---- the Drawer: whole frames drawn into the port's own targets ----

// One draw as the port captured it.
struct DrawCall {
    const uint32_t *registers = nullptr; // render state by register number (0x2000-0x23FF)
    uint32_t primitive = 0;
    // the shaders: the microcode the GPU would run (host order), its hash
    // (shader_hash) and uses (shader_uses); a null pixel shader for none
    const uint32_t *vs = nullptr, *ps = nullptr;
    size_t vs_words = 0, ps_words = 0;
    uint64_t vs_hash = 0, ps_hash = 0;
    const ShaderUses *vs_uses = nullptr, *ps_uses = nullptr;
    // the constants as the GPU would hold them: float constants c0-c255 of
    // each stage (1024 words each, host order), the 32 fetch constants (192
    // words: the textures' and the vertex fetches'), bool and loop constants
    // (8 and 32 words; null: zero)
    const uint32_t *vs_floats = nullptr, *ps_floats = nullptr, *fetch = nullptr;
    const uint32_t *bools = nullptr, *loops = nullptr;
    // The vertices. In place (vertex_data null): the vertex fetch constants
    // address guest memory, and each of `ranges` ({physical address, bytes})
    // is made current on the GPU first (Frame::guest_memory). Copied: the
    // fetch constants address vertex_data (an offset into it as the
    // physical address), which the GPU reads as guest memory.
    const void *vertex_data = nullptr;
    size_t vertex_bytes = 0;
    const uint32_t (*ranges)[2] = nullptr;
    size_t range_count = 0;
    // The indices (host order), or null for count vertices from first; base
    // is added to each (the base vertex).
    const uint32_t *indices = nullptr;
    uint32_t count = 0, first = 0;
    int32_t base = 0;
};

// A clear as the guest's Direct3D does it (D3DDevice_Clear): the targets
// bound, the area, the values as Direct3D's floats.
struct ClearCall {
    uint32_t surface_info = 0;     // RB_SURFACE_INFO
    uint32_t color_info[4] = {};   // RB_COLOR_INFO, RB_COLOR1-3_INFO
    uint32_t depth_info = 0;       // RB_DEPTH_INFO
    uint32_t targets = 0;          // a bit per color target cleared (0-3)
    bool depth = false, stencil = false;
    float color[4] = {}, z = 0.0f;
    uint32_t stencil_value = 0;
    int32_t area[4] = {}; // x1, y1, x2, y2 in the surface's pixels
};

class Drawer {
  public:
    // shaders: the port's ShaderCache directory (the bake's; empty: in memory)
    explicit Drawer(std::filesystem::path shaders = {});
    ~Drawer();
    Drawer(const Drawer &) = delete;
    Drawer &operator=(const Drawer &) = delete;

    // At the output renderer's call: the frame's work begins (the Vulkan
    // objects made the first time). False and why when they cannot be.
    bool begin(const OutputContext &ctx, std::string &why);
    // In the guest's order. False and why when the port cannot draw it: the
    // frame is then the emulated path's (cancel, and return false).
    bool draw(const DrawCall &call, std::string &why);
    bool clear(const ClearCall &call, std::string &why);
    // A resolve from the target written last at its EDRAM place (a 7e3 one
    // read as 2_10_10_10 packed first; a 2x or 4x view of single-sampled tiles
    // taken as the console's downsample), through Frame::resolve, then its
    // clears.
    bool resolve(const ResolveInfo &info, std::string &why);
    // A texture (the guest's front buffer: OutputContext::guest's fetch
    // constant) drawn over the output image.
    bool present(const uint32_t fetch[6], std::string &why);
    // One of the port's color targets over the output image instead (a
    // target viewer): the one written last at that place.
    bool present_target(uint32_t edram_base, uint32_t pitch, uint32_t format, std::string &why);
    // The frame recorded: true. cancel: the frame's draws dropped (the
    // targets keep what earlier frames' work wrote).
    bool end();
    void cancel();

    // The port's targets (a viewer's list).
    struct TargetInfo {
        bool depth = false;
        uint32_t edram_base = 0, pitch = 0, format = 0, width = 0, height = 0;
        VkFormat vk_format = VK_FORMAT_UNDEFINED;
        uint64_t written = 0; // when a draw or clear last wrote it (an increasing count)
    };
    std::vector<TargetInfo> targets() const;

    struct Stats {
        uint64_t frames = 0, draws = 0, clears = 0, resolves = 0;
        uint64_t pipelines = 0, pipeline_misses = 0; // made; looked up and not found
        uint64_t constant_uploads = 0, constants_reused = 0; // a stage's block uploaded, or the last one's taken
        std::map<std::string, uint64_t> approximate;
    };
    Stats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xf::native
