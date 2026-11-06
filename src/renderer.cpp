// The Xenos renderer, the command processor's Sink (renderer.h). The
// viewport and scissor follow Xenia's src/xenia/gpu/draw_util.cc
// (GetHostViewportInfo, GetScissor), the register fields registers.h, the
// clear values vulkan/vulkan_render_target_cache.cc, the vertex data a draw
// needs vulkan/vulkan_command_processor.cc (IssueDraw's shared memory
// requests), the point sizes its UpdateSystemConstantValues, the rows a
// draw takes over in EDRAM draw_extent_estimator.cc, and the swap
// vulkan/vulkan_texture_cache.cc (RequestSwapTexture); all BSD, see
// LICENSE-xenia.
//
// Batched (plan B4): what the processor sends is recorded into the command
// buffer of a submission slot, render passes kept open from draw to draw
// while the targets stay the same, and the slot is submitted at each swap or
// when its share of the upload arena fills. A few slots are in flight; the
// renderer waits for the GPU only to reuse a slot, or when the CPU needs
// what the GPU wrote (a front buffer read back for --shots, a texture over a
// resolve's output in a format only the CPU decodes). A draw's constants
// and indices are copied into its slot's share of the arena; its vertex data
// is read from the mirror (below); textures are bindless, each image at an
// index of one descriptor set written as they are made.
//
// Guest memory is read where it lies, and the CPU's writes to it are seen by
// the memory's write watches (memory.h; Xenia's physical memory access
// callbacks): a page is watched before its bytes are copied, and the number
// of its last write, above the count taken then, says the copy is old.
// Indices are copied at each draw. The mirror, a copy of guest memory on the
// GPU as Xenia's shared memory, is given the pages a draw's vertex data or a
// texture load needs that are not current there (Xenia's RequestRange), and
// textures are kept until a write to their pages (Xenia's texture cache
// watches); the processor's own writes (memory_written) are told to the
// watches.
//
// Resolves and memory exports run on the GPU, into the mirror, whose pages
// then hold what the GPU wrote and guest memory does not (Xenia's
// RangeWrittenByGpu) until the CPU writes them. Textures over them are
// loaded from the mirror on the GPU (as Xenia's texture cache loads all of
// its textures). Guest memory gets a resolve's bytes as Xenia's
// readback_resolve option says (Options::readback_resolve): never, as its
// default; a frame late, without waiting; or at once, waiting for the GPU.

#include "xenoforge/renderer.h"

#include "xenoforge/guest_memory.h"
#include "xenoforge/resolve.h"
#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/shader_translator.h"
#include "xenoforge/texture.h"
#include "xenoforge/ucode.h"
#include "xenoforge/ucode_parse.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace xf::render {
namespace {

using namespace xenos;

const uint32_t RECTANGLE_LIST_GS[] = {
#include "rectangle_list.geom.inc"
};
const uint32_t POINT_LIST_GS[] = {
#include "point_list.geom.inc"
};
const uint32_t FULLSCREEN_VS[] = {
#include "fullscreen.vert.inc"
};
const uint32_t EDRAM_STORE[] = {
#include "edram_store.comp.inc"
};
const uint32_t EDRAM_STORE_MS[] = {
#include "edram_store_ms.comp.inc"
};
const uint32_t EDRAM_LOAD[] = {
#include "edram_load.frag.inc"
};
const uint32_t EDRAM_LOAD_DEPTH[] = {
#include "edram_load_depth.frag.inc"
};
const uint32_t EDRAM_LOAD_STENCIL[] = {
#include "edram_load_stencil.frag.inc"
};
const uint32_t RESOLVE[] = {
#include "resolve.comp.inc"
};
const uint32_t TEXTURE_LOAD[] = {
#include "texture_load.comp.inc"
};

// registers xenos.h does not name
namespace r {
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100, VGT_MIN_VTX_INDX = 0x2101, VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t RB_BLEND_RED = 0x2105; // then green, blue, alpha
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F; // then XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
constexpr uint32_t RB_BLENDCONTROL[4] = {0x2201, 0x2209, 0x220A, 0x220B};
constexpr uint32_t RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281;
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284, VGT_HOS_CNTL = 0x2285;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380; // then FRONT_OFFSET, BACK_SCALE, BACK_OFFSET
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F;
constexpr uint32_t COLOR_INFO[4] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO};
} // namespace r

constexpr uint32_t EDRAM_TILES = 2048; // 80x16 samples of 32 bits each
constexpr VkDeviceSize GUEST_MEMORY = 512u << 20;
constexpr VkDeviceSize EDRAM_BYTES = VkDeviceSize(EDRAM_TILES) * 80 * 16 * 4;
// submissions in flight, and each one's share of the upload arena
constexpr uint32_t SLOTS = 3;
constexpr VkDeviceSize ARENA_SLOT = 64u << 20;
constexpr VkDeviceSize READBACK_CHUNK = 32u << 20;
// GPU timestamps a submission holds at most (Options::gpu_timing)
constexpr uint32_t TIMESTAMPS = 8192;
constexpr uint32_t GPU_END = UINT32_MAX; // the kind of a submission's last timestamp
constexpr VkDeviceSize NONE = ~VkDeviceSize(0);
// the bindless tables: images by kind (2D arrays, 3D, cube; the slot word
// holds 14 bits of index) and samplers
constexpr uint32_t IMAGES[3] = {16384, 4096, 4096};
constexpr uint32_t SAMPLERS = 1024;

float as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
int32_t sign_extend(uint32_t value, uint32_t bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }

uint64_t hash_words(uint32_t seed, std::span<const uint32_t> words) {
    uint64_t h = 0xcbf29ce484222325ull ^ seed; // FNV-1a
    for (uint32_t d : words)
        for (int i = 0; i < 4; i++) h = (h ^ ((d >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
}

// Physical memory as [first, second) ranges that do not wrap.
using Range = std::pair<uint32_t, uint32_t>;

// a span of guest memory as ranges, split where it wraps past the end of
// physical memory, as the address mask wraps it
void add_span(std::vector<Range> &out, uint32_t address, uint64_t bytes) {
    address &= GuestMemory::PHYSICAL_MASK;
    bytes = std::min<uint64_t>(bytes, GuestMemory::SIZE);
    if (!bytes) return;
    uint64_t end = uint64_t(address) + bytes;
    if (end <= GuestMemory::SIZE) {
        out.push_back({address, uint32_t(end)});
        return;
    }
    out.push_back({address, GuestMemory::SIZE});
    out.push_back({0, uint32_t(end - GuestMemory::SIZE)});
}

std::vector<Range> ranges_of(const std::vector<std::pair<uint32_t, uint32_t>> &footprint) {
    std::vector<Range> out;
    for (auto [address, bytes] : footprint) add_span(out, address, bytes);
    return out;
}

bool overlaps(const Range &a, const Range &b) { return a.first < b.second && b.first < a.second; }

float half_to_float(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(float(mantissa), -24);
    if (exponent == 31) return as_float(sign | 0x7F800000 | (mantissa << 13));
    return as_float(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

// xenos::Float20e4To32
float float24_to_float(uint32_t f24) {
    f24 &= 0xFFFFFF;
    if (!f24) return 0.0f;
    uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
    if (!exponent) {
        // normalized in the result
        uint32_t lzcnt = 0;
        while (!(mantissa & (0x100000u >> lzcnt))) lzcnt++;
        exponent = uint32_t(1 - int32_t(lzcnt));
        mantissa = (mantissa << lzcnt) & 0xFFFFF;
    }
    return as_float(((exponent + 112) << 23) | (mantissa << 3));
}

// xenos::Float7e3To32
float float7e3_to_float(uint32_t f10) {
    f10 &= 0x3FF;
    if (!f10) return 0.0f;
    uint32_t mantissa = f10 & 0x7F, exponent = f10 >> 7;
    if (!exponent) {
        uint32_t lzcnt = uint32_t(std::countl_zero(mantissa)) - 24;
        exponent = uint32_t(1 - int32_t(lzcnt));
        mantissa = (mantissa << lzcnt) & 0x7F;
    }
    return as_float(((exponent + 124) << 23) | (mantissa << 16));
}

VkFormat vk_format_of(texture::HostFormat f) {
    using H = texture::HostFormat;
    switch (f) {
    case H::R8: return VK_FORMAT_R8_UNORM;
    case H::RG8: return VK_FORMAT_R8G8_UNORM;
    case H::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    // the packed formats Vulkan requires every device to sample, as Xenia's
    // Vulkan texture cache takes them (texture.h: the loads rearrange the
    // guest's bits for them)
    case H::R5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case H::A1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case H::B4G4R4A4: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case H::RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case H::R16: return VK_FORMAT_R16_UNORM;
    case H::RG16: return VK_FORMAT_R16G16_UNORM;
    case H::RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
    case H::R16F: return VK_FORMAT_R16_SFLOAT;
    case H::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case H::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case H::R32: return VK_FORMAT_R32_UINT;
    case H::RG32: return VK_FORMAT_R32G32_UINT;
    case H::RGBA32: return VK_FORMAT_R32G32B32A32_UINT;
    case H::R32F: return VK_FORMAT_R32_SFLOAT;
    case H::RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case H::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case H::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case H::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case H::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case H::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case H::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// A component of a swizzle composed with the host format's
// (texture::compose_swizzle): 0-3 a host component, 5 one, the rest zero.
VkComponentSwizzle swizzle_of(uint32_t composed) {
    if (composed < 4) return VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + composed);
    return composed == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
}

// What the device samples, filters and has the snorm of, per host format
// (texture::Support), from its format properties. XF_TEXTURE_FALLBACKS takes
// away what a device may lack, so the fallbacks run on a device that has it
// all: 1 (or "all") everything below, or a list of bc (the BC formats),
// norm16 (16-bit unorm), packed (the packed 16-bit formats), snorm (the
// snorm views Vulkan does not require: 16-bit, 2_10_10_10, BC4 and BC5) and
// linear32 (linear filtering of 32-bit floats).
VkFormat snorm_of(VkFormat f);
texture::Support texture_support(const vk::Device &d) {
    using H = texture::HostFormat;
    std::string force = std::getenv("XF_TEXTURE_FALLBACKS") ? std::getenv("XF_TEXTURE_FALLBACKS") : "";
    bool all = force == "1" || force == "all";
    auto forced = [&](const char *name) {
        return all || ("," + force + ",").find(std::string(",") + name + ",") != std::string::npos;
    };
    bool bc = forced("bc"), norm16 = forced("norm16"), packed = forced("packed"), snorms = forced("snorm");
    bool linear32 = forced("linear32");
    texture::Support s{0, 0, 0};
    for (uint32_t i = 1; i < texture::HOST_FORMAT_COUNT; i++) {
        H h = H(i);
        VkFormat f = vk_format_of(h), snorm = snorm_of(f);
        bool is_bc = h == H::BC1 || h == H::BC2 || h == H::BC3 || h == H::BC4 || h == H::BC5;
        bool is_norm16 = h == H::R16 || h == H::RG16 || h == H::RGBA16;
        if (f == VK_FORMAT_UNDEFINED || (bc && is_bc) || (norm16 && is_norm16) ||
            (packed && (h == H::R5G6B5 || h == H::A1R5G5B5 || h == H::B4G4R4A4)))
            continue;
        if (d.format_supported(f, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) s.sampled |= uint64_t(1) << i;
        bool float32 = h == H::R32F || h == H::RG32F || h == H::RGBA32F;
        if (d.format_supported(f, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && !(linear32 && float32))
            s.linear |= uint64_t(1) << i;
        bool required = h == H::R8 || h == H::RG8 || h == H::RGBA8; // their snorm Vulkan requires
        if (snorm != VK_FORMAT_UNDEFINED && (required || !snorms) &&
            d.format_supported(snorm,
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            s.snorm |= uint64_t(1) << i;
    }
    return s;
}

VkCompareOp compare_op(uint32_t f) {
    static const VkCompareOp ops[] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,          VK_COMPARE_OP_EQUAL,
                                      VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                      VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return ops[f & 7];
}
VkStencilOp stencil_op(uint32_t o) {
    static const VkStencilOp ops[] = {VK_STENCIL_OP_KEEP,      VK_STENCIL_OP_ZERO,
                                      VK_STENCIL_OP_REPLACE,   VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                      VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                      VK_STENCIL_OP_INCREMENT_AND_WRAP,  VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return ops[o & 7];
}
VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
    }
}
VkBlendOp blend_op(uint32_t o) {
    static const VkBlendOp ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                    VK_BLEND_OP_REVERSE_SUBTRACT};
    return o < 5 ? ops[o] : VK_BLEND_OP_ADD;
}

struct Shader {
    xf::ucode::ParsedShader parsed;
    xf::ucode::ShaderSummary summary;
    bool translated = false;
    VkShaderModule module = VK_NULL_HANDLE;
    std::string unsupported; // why it cannot be drawn with
    uint32_t colors_written = 0; // oC0-oC3, a bit each
    bool writes_depth = false;
    // for running it on the CPU (draw extents): whether it uses what a
    // texture fetch returns, and which of oPos's y and w, oPts's size and
    // vertex kill it writes
    bool fetches_textures = false;
    bool writes_position_y = false, writes_position_w = false, writes_point_size = false, writes_vertex_kill = false;
    // memexport: whether it writes any eM#, and the float constants its eA
    // writes take the stream from (Shader::memexport_stream_constants)
    bool exports = false;
    std::set<uint32_t> stream_constants;
    // per texture slot, the first fetch's filter overrides: mag, min, mip, aniso (3: the constant's)
    std::map<uint32_t, std::array<uint32_t, 4>> filters;
};

struct RenderTarget {
    bool depth = false;
    uint32_t base = 0, pitch = 0, msaa = 0, format = 0;
    texture::HostFormat host = texture::HostFormat::Unsupported; // color
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sampled = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE; // color or depth alone, stencil alone
    VkDescriptorSet store_set = VK_NULL_HANDLE;                     // its EDRAM store's
    uint32_t width = 0, height = 0;
    // in EDRAM: tiles per row (at its bits per sample), a tile's size in pixels
    uint32_t pitch_tiles = 1, tile_width = 80, tile_height = 16;
    bool wide = false; // 64 bits per sample
};

// edram.glsl's push constants
struct EdramTarget {
    uint32_t base_tiles, pitch_tiles, msaa, format, width, height, is_depth, ints_log2, stencil_bit;
};

// resolve.comp's push constants
struct ResolveConstants {
    uint32_t width, height, src_words, source_format, depth, depth_mode, stencil_word, raw;
    uint32_t dest_base, dest_pitch, dest_height, dest_x, dest_y, dest_slice, dest_array;
    uint32_t dest_format, dest_log2_bytes, dest_endian, dest_number, dest_swap;
    float exp_factor;
    // a multisampled source read from EDRAM
    uint32_t edram, edram_base, edram_pitch, msaa, ints_log2, src_x, src_y, first_sample, samples;
};
static_assert(sizeof(ResolveConstants) <= 128, "the push constants every device has room for");

// texture_load.comp's push constants
using LoadConstants = texture::GpuLevel;

struct Texture {
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE; // its own: made and destroyed with it
    uint32_t dimension = 1;
    texture::HostFormat host = texture::HostFormat::Unsupported; // what its image holds
    uint32_t host_swizzle = texture::SWIZZLE_RGBA;               // what each guest component reads of it
    uint32_t kind = 0, index = 0;      // its bindless table (2D arrays, 3D, cube) and place in it
    // for a constant with signed components: a view of the image as snorm,
    // or of an image of its own where the signed bits differ (signed_image),
    // at its own place in the same table (0: none)
    vk::Image signed_image;
    VkImageView signed_view = VK_NULL_HANDLE;
    uint32_t signed_index = 0;
    std::vector<Range> ranges;         // the guest memory it was loaded from
    uint32_t checked = 0;              // the write count its memory was last known unwritten at (Memory's)
    uint32_t low = 0, high = 0;        // the ranges' bounds
    bool stale = false;                // a resolve or a memory export wrote memory under it
    bool gpu = false;                  // loaded from what the GPU wrote (the mirror's, or read back from it)
};

// the translated shaders' and the geometry shaders' system constants
struct System {
    float ndc_scale[4];
    float ndc_offset[4];
    uint32_t slots[32]; // image index (bits 0-13), its kind (14-15: 2D array, 3D, cube), sampler (16-31)
    uint32_t flags[4];  // alpha test, the position's formats, gamma targets, D24FS8 depth
    float values[4];    // alpha reference, the point diameter's bounds (PA_SU_POINT_MINMAX)
    float color_scales[4];
    float point[4]; // the constant diameter (PA_SU_POINT_SIZE), guest pixel diameter to NDC radius
    // per slot, its texture's signs on the result components (bits 0-7, two
    // each: unsigned, signed, biased, gamma; Xenia's SwizzleSigns) and the
    // width less one of each one's component (bits 8-23, four each; 0
    // unknown), for biased components
    uint32_t signs[32];
    uint32_t signed_slots[32]; // per slot, its texture's signed view as slots has it; 0 none
};
static_assert(sizeof(System) == 480);

// The bits of each component of a fixed-point guest texture format, in the
// texel's order (Xenia's texture_info_formats.inl, its fixed formats); zeros
// for the others.
std::array<uint32_t, 4> component_bits(uint32_t format) {
    switch (format) {
    case 2: case 8: case 9: case 58: case 59: return {8, 0, 0, 0};       // 8, 8_A, 8_B, DXT3A, DXT5A
    case 3: return {5, 5, 5, 1};                                         // 1_5_5_5
    case 4: return {5, 6, 5, 0};                                         // 5_6_5
    case 5: return {5, 5, 6, 0};                                         // 6_5_5
    case 6: case 14: case 50: case 62: return {8, 8, 8, 8};              // 8_8_8_8 and its kin
    case 7: case 54: return {10, 10, 10, 2};                             // 2_10_10_10
    case 10: case 49: case 60: return {8, 8, 0, 0};                      // 8_8, DXN, CTX1
    case 13: case 25: return {16, 16, 0, 0};                             // 16_16
    case 15: return {4, 4, 4, 4};                                        // 4_4_4_4
    case 16: case 55: return {11, 11, 10, 0};                            // 10_11_11
    case 17: case 56: return {10, 11, 11, 0};                            // 11_11_10
    case 21: case 26: return {16, 16, 16, 16};                           // 16_16_16_16
    case 24: return {16, 0, 0, 0};                                       // 16
    default: return {0, 0, 0, 0};
    }
}

// A slot's signs word (System::signs): the fetch constant's signs through its
// swizzle onto the result components, as Xenia's SwizzleSigns (constant
// components unsigned, or signed when everything else is), and the biased
// ones' component widths as its GetIntegerScaleBits gives them.
uint32_t swizzled_signs(const texture::Info &info) {
    uint32_t signs = 0, constants = 0;
    bool any_signed = false, any_not_signed = false;
    std::array<uint32_t, 4> bits = component_bits(info.format);
    uint32_t last_stored = 0;
    for (uint32_t c = 1; c < 4; c++)
        if (bits[c]) last_stored = c;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t source = (info.swizzle >> (3 * k)) & 7;
        if (source & 4) {
            constants |= 1u << (2 * k);
            continue;
        }
        uint32_t sign = info.signs[source];
        signs |= sign << (2 * k);
        (sign == 1 ? any_signed : any_not_signed) = true;
        uint32_t width = bits[std::min(source, last_stored)];
        if (sign == 2 && width && width <= 16) signs |= (width - 1) << (8 + 4 * k);
    }
    bool constants_signed = constants == 0x55 ? info.signs[0] == 1 && info.signs[1] == 1 && info.signs[2] == 1 &&
                                                    info.signs[3] == 1
                                              : any_signed && !any_not_signed;
    if (constants_signed) signs |= constants; // 1 for each constant component
    return signs;
}

// The snorm format of a unorm one's bits, where the host has one (a signed
// view of the same image); undefined for the rest.
VkFormat snorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case VK_FORMAT_R16_UNORM: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_BC4_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_BC5_SNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// A resolve's pair of read-back buffers for readback_resolve=fast (Xenia's
// ReadbackBuffer, keyed by the resolve's address and length): one written
// this frame, the other the frame before's, which guest memory gets.
struct ResolveReadback {
    vk::Buffer buffers[2];
    uint64_t serials[2] = {}; // the submission that last wrote each
    uint32_t current = 0;     // the one this frame writes
    uint64_t last_used = 0;   // the swap it was last used at
};

// A submission slot: its command buffer and fence, its share of the arena,
// its read-back buffers, and what waits for the GPU to be done with it.
struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t serial = 0; // the submission it holds, recording or in flight; 0 when free
    bool recording = false;
    VkDeviceSize used = 0;
    std::vector<std::function<void()>> after;
    std::vector<vk::Buffer> readbacks;
    size_t readback = 0;
    VkDeviceSize readback_used = 0;
    // GPU timing (Options::gpu_timing): its timestamps, and the kind of work
    // each one begins
    VkQueryPool queries = VK_NULL_HANDLE;
    std::vector<uint32_t> marks;
    // occlusion queries (Impl's "occlusion queries"): the pool, reset as the
    // slot begins recording, and the report each query counts for
    VkQueryPool occlusion = VK_NULL_HANDLE;
    uint32_t occlusion_used = 0; // the queries to reset before the next use
    std::vector<uint64_t> occlusion_reports;
};

// A swap's front buffer being read back: where its texels wait for the GPU
// to be done, and how they are shown (the fetch constant's swizzle and
// format, the gamma ramp at the swap).
struct FrontRead {
    uint64_t number = 0, serial = 0;
    const uint8_t *data = nullptr;
    VkDeviceSize bytes = 0;
    texture::HostFormat host = texture::HostFormat::Unsupported;
    uint32_t width = 0, height = 0, swizzle = 0, format = 0;
    std::array<uint32_t, 256> gamma{};
    std::array<uint32_t, 128 * 3> pwl{};
};

// The attachments of the render pass under way.
struct Pass {
    VkImageView colors[4] = {};
    VkImageView depth = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool operator==(const Pass &) const = default;
};

// ---- pipelines made on worker threads ----
//
// As Xenia's pipeline cache's creation threads (d3d12/pipeline_cache.cc): a
// draw's graphics pipeline that is not made yet is queued with a copy of its
// description, and worker threads make it while the renderer goes on; the
// draw is skipped meanwhile (D3D12CommandProcessor::IssueDraw's "pipeline not
// ready"), and the draws after it have it once it is made. The pipeline
// cache (Vulkan's, internally synchronized) is shared by all of them.

// A graphics pipeline's description, owning what its pointers reach (the
// renderer's draws give stages, the fixed states, blend attachments, dynamic
// states and the dynamic rendering formats; nothing else is chained).
struct PipelineDesc {
    VkPipelineShaderStageCreateInfo stages[5]{};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineTessellationStateCreateInfo tessellation{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    VkPipelineColorBlendAttachmentState blends[4]{};
    VkPipelineColorBlendStateCreateInfo blend{};
    VkDynamicState dynamic_states[32]{};
    VkPipelineDynamicStateCreateInfo dynamic{};
    VkFormat color_formats[4]{};
    VkPipelineRenderingCreateInfo rendering{};
    VkGraphicsPipelineCreateInfo info{};

    explicit PipelineDesc(const VkGraphicsPipelineCreateInfo &from) : info(from) {
        info.stageCount = std::min<uint32_t>(from.stageCount, uint32_t(std::size(stages)));
        std::copy_n(from.pStages, info.stageCount, stages);
        info.pStages = stages;
        if (from.pNext) {
            rendering = *static_cast<const VkPipelineRenderingCreateInfo *>(from.pNext);
            rendering.colorAttachmentCount = std::min<uint32_t>(rendering.colorAttachmentCount, 4);
            std::copy_n(rendering.pColorAttachmentFormats, rendering.colorAttachmentCount, color_formats);
            rendering.pColorAttachmentFormats = color_formats;
            info.pNext = &rendering;
        }
        own(info.pVertexInputState, vertex_input);
        own(info.pInputAssemblyState, assembly);
        own(info.pTessellationState, tessellation);
        own(info.pViewportState, viewport);
        own(info.pRasterizationState, raster);
        own(info.pMultisampleState, multisample);
        own(info.pDepthStencilState, depth);
        if (own(info.pColorBlendState, blend) && blend.pAttachments) {
            blend.attachmentCount = std::min<uint32_t>(blend.attachmentCount, 4);
            std::copy_n(blend.pAttachments, blend.attachmentCount, blends);
            blend.pAttachments = blends;
        }
        if (own(info.pDynamicState, dynamic) && dynamic.pDynamicStates) {
            dynamic.dynamicStateCount = std::min<uint32_t>(dynamic.dynamicStateCount, uint32_t(std::size(dynamic_states)));
            std::copy_n(dynamic.pDynamicStates, dynamic.dynamicStateCount, dynamic_states);
            dynamic.pDynamicStates = dynamic_states;
        }
    }
    PipelineDesc(const PipelineDesc &) = delete;
    PipelineDesc &operator=(const PipelineDesc &) = delete;

private:
    template <typename T> static bool own(const T *&pointer, T &store) {
        if (!pointer) return false;
        store = *pointer;
        pointer = &store;
        return true;
    }
};

// A pipeline the renderer asked for: made (state 1, the pipeline null when
// the driver failed) or being made (0).
struct PipelineEntry {
    std::atomic<int> state{0};
    VkPipeline pipeline = VK_NULL_HANDLE;
};

class PipelineWorkers {
public:
    PipelineWorkers(VkDevice device, VkPipelineCache cache, uint32_t threads) : dev_(device), cache_(cache) {
        for (uint32_t i = 0; i < threads; i++) threads_.emplace_back([this] { run(); });
    }
    // stops: what is queued is left unmade, what is being made is waited for
    ~PipelineWorkers() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread &t : threads_) t.join();
    }
    size_t threads() const { return threads_.size(); }
    void queue(PipelineEntry *entry, std::unique_ptr<PipelineDesc> desc) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.emplace_back(entry, std::move(desc));
        }
        work_.notify_one();
    }
    // until the entry is made
    void wait(PipelineEntry *entry) {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return entry->state.load(std::memory_order_acquire) != 0; });
    }
    // until everything queued is made
    void wait_all() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return queue_.empty() && busy_ == 0; });
    }
    // the time the workers spent making pipelines, and how many they made
    double seconds() const { return double(nanoseconds_.load()) * 1e-9; }
    uint64_t made() const { return made_.load(); }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            work_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            auto [entry, desc] = std::move(queue_.front());
            queue_.pop_front();
            busy_++;
            lock.unlock();
            auto start = std::chrono::steady_clock::now();
            VkPipeline p = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(dev_, cache_, 1, &desc->info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
            nanoseconds_ += uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            made_++;
            desc.reset();
            lock.lock();
            entry->pipeline = p;
            entry->state.store(1, std::memory_order_release);
            busy_--;
            done_.notify_all();
        }
    }

    VkDevice dev_;
    VkPipelineCache cache_;
    std::mutex mutex_;
    std::condition_variable work_, done_;
    std::deque<std::pair<PipelineEntry *, std::unique_ptr<PipelineDesc>>> queue_;
    uint32_t busy_ = 0;
    bool stop_ = false;
    std::atomic<uint64_t> nanoseconds_{0}, made_{0};
    std::vector<std::thread> threads_;
};

class Impl final : public Renderer {
public:
    Impl(vk::Device &device, gpu::CommandProcessor &processor, const Options &options)
        : d_(device), dev_(device.device()), processor_(processor), options_(options),
          memory_(processor.memory().translate_physical(0)), mem_(processor.memory()),
          stamps_(mem_.write_stamps()), page_shift_(mem_.watch_page_shift()),
          mirror_at_(GuestMemory::SIZE >> page_shift_, 0),
          gpu_pages_(std::make_unique<std::atomic<uint64_t>[]>(((GuestMemory::SIZE >> page_shift_) + 63) / 64)) {
        for (size_t i = 0; i < ((GuestMemory::SIZE >> page_shift_) + 63) / 64; i++) gpu_pages_[i].store(0);
        watcher_ = mem_.add_watcher(watch_bounds, this);
    }
    ~Impl() override;

    bool init(std::string &error);
    // Vulkan's pipeline cache, on disk where the options say (see
    // open_pipeline_cache)
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    uint64_t pipelines_saved_ = 0, last_pipeline_swap_ = 0;
    void open_pipeline_cache();
    void save_pipeline_cache();

    // ---- the Sink: the processor's calls, on its thread ----
    void shader(uint32_t type, uint32_t, std::span<const uint32_t> code) override {
        Call call(*this, nullptr);
        if (call) load_shader(type, code);
    }
    void draw(const gpu::RegisterFile &registers, const gpu::Draw &d) override {
        Call call(*this, &registers);
        if (call) draw(d);
    }
    void resolve(const gpu::RegisterFile &registers, const gpu::Draw &) override {
        Call call(*this, &registers);
        Timer timer(report_.resolve_seconds);
        if (call) resolve();
    }
    void swap(const gpu::RegisterFile &registers, const gpu::Swap &) override {
        Call call(*this, &registers);
        if (call) swap();
    }
    // occlusion queries (gpu::OcclusionQueries::Fast; "occlusion queries" below)
    bool counts_samples() override { return true; }
    void zpd_report(uint64_t ended, uint64_t next) override {
        Call call(*this, nullptr);
        if (call) zpd_report_ended(ended, next);
    }
    void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) override {
        std::lock_guard<std::mutex> lock(mutex_);
        resolved.insert(resolved.end(), zpd_resolved_.begin(), zpd_resolved_.end());
        zpd_resolved_.clear();
    }
    // the processor's own writes (fences, reports), and a trace's memory
    // (Xenia's TracePlaybackWroteMemory): written through the physical view,
    // which no watch sees, so the watches are told; the mirror's pages and
    // the textures there are loaded again at their next use
    void memory_written(uint32_t address, uint32_t bytes) override {
        Call call(*this, nullptr);
        std::vector<Range> written;
        add_span(written, address, bytes);
        for (auto [start, stop] : written) mem_.physical_written(start, stop - start);
    }

    // EDRAM as a trace began, in Xenia's layout (edram.glsl's)
    void restore_edram(std::span<const uint8_t> bytes) override {
        Call call(*this, nullptr);
        if (bytes.size() != EDRAM_BYTES || !call) return;
        std::fill(owners_.begin(), owners_.end(), nullptr);
        VkBuffer from;
        VkDeviceSize at;
        uint8_t *data = staging(EDRAM_BYTES, from, at);
        if (!data) return;
        std::memcpy(data, bytes.data(), bytes.size());
        VkBufferCopy copy{at, 0, EDRAM_BYTES};
        VkCommandBuffer c = outside();
        gpu_mark("EDRAM: a trace's snapshot restored");
        vkCmdCopyBuffer(c, from, edram_.buffer, 1, &copy);
    }
    Report report() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return report_;
    }
    bool try_report(Report &out) const override {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock) return false;
        out = report_;
        return true;
    }
    bool failed(std::string &error) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fatal_.empty()) return false;
        error = fatal_;
        return true;
    }
    void drain() override {
        Call call(*this, nullptr);
        if (call) sync();
        if (pipeline_workers_) pipeline_workers_->wait_all();
        save_pipeline_cache();
    }

    // ---- TraceSource: what a trace writer asks ----
    void set_memory_trace(gpu::MemoryTrace *trace) override {
        std::lock_guard<std::mutex> lock(mutex_);
        trace_ = trace;
    }
    void settle() override { drain(); }
    bool snapshot_edram(std::vector<uint8_t> &out) override;
    void gpu_memory(std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &out) override;

private:
    // adds the time from its making to its end to a count of seconds
    class Timer {
    public:
        explicit Timer(double &into) : into_(into), start_(std::chrono::steady_clock::now()) {}
        ~Timer() { into_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }

    private:
        double &into_;
        std::chrono::steady_clock::time_point start_;
    };

    // One of the processor's calls: the renderer's state is its alone while
    // it runs (report() waits), the registers are the processor's as they
    // stand, and its time is counted. False once the device has failed.
    class Call {
    public:
        Call(Impl &r, const gpu::RegisterFile *registers) : r_(r), lock_(r.mutex_), timer_(r.report_.seconds) {
            if (registers) r.regs_ = registers->data();
        }
        explicit operator bool() const { return r_.fatal_.empty(); }

    private:
        Impl &r_;
        std::lock_guard<std::mutex> lock_;
        Timer timer_;
    };

    void load_shader(uint32_t type, std::span<const uint32_t> code);

    // draws
    void draw(const gpu::Draw &d);
    uint32_t draw_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                        uint32_t primitive) const;
    uint32_t vertex_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                          uint32_t primitive) const;
    void resolve();
    void gpu_resolve(const resolve::Info &info, RenderTarget &source);
    void swap();
    Texture *front_texture(texture::Info &info);
    void read_front_buffer(uint64_t number, Texture &t, const texture::Info &info);
    void deliver(const FrontRead &read);
    void skip(const std::string &reason) {
        report_.skipped[reason]++;
        // the frames to trace for it: the swap this draw's frame ends with
        std::vector<uint64_t> &at = report_.skipped_at[reason];
        if (at.size() < 8 && (at.empty() || at.back() != report_.swaps)) at.push_back(report_.swaps);
        if (options_.verbose)
            std::fprintf(stderr, "  draw %llu skipped: %s (shaders %016llX %016llX)\n", (unsigned long long)report_.draws,
                         reason.c_str(), (unsigned long long)vertex_hash_, (unsigned long long)pixel_hash_);
    }
    void approximate(const std::string &what) { report_.approximate[what]++; }

    // GPU timing (Options::gpu_timing): a timestamp where the kind of work
    // being recorded changes; finish() sums the intervals by kind
    std::vector<std::string> gpu_kinds_;
    std::map<std::string, uint32_t> gpu_kind_ids_;
    double timestamp_period_ = 1.0; // nanoseconds a tick
    uint64_t timestamp_mask_ = ~0ull;
    void gpu_mark(const std::string &kind) {
        if (!options_.gpu_timing) return;
        auto [it, made] = gpu_kind_ids_.try_emplace(kind, uint32_t(gpu_kinds_.size()));
        if (made) gpu_kinds_.push_back(kind);
        gpu_mark(it->second);
    }
    void gpu_mark(uint32_t kind);
    void gpu_collect(Slot &s);
    std::string target_name(const RenderTarget &t) const;

    // resources
    Shader *shader(uint64_t hash);
    bool translate(Shader &s, std::string &why);
    RenderTarget *render_target(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format);
    VkFormat depth_vk_format(uint32_t format) const;
    Texture *texture(const uint32_t fetch[6], std::string &why);
    bool gpu_load(Texture &t, const vk::Image &image, const texture::Plan &plan, bool fresh, bool upload);
    bool cpu_upload(const vk::Image &target, const texture::Image &image, bool fresh);
    texture::Support support_; // what the device samples (texture_support)
    uint32_t sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides);
    // the pipeline of a key, made now or found made; null when the driver
    // failed, or (pending) when worker threads are making it and the draw may
    // not wait (async)
    VkPipeline pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info, bool async,
                        bool &pending);
    std::unique_ptr<PipelineWorkers> pipeline_workers_; // with Options::async_pipelines
    VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                          VkComponentMapping components = {}, uint32_t levels = 1, uint32_t layers = 1,
                          bool keep = true);
    VkShaderModule module(const uint32_t *words, size_t bytes) {
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = bytes;
        m.pCode = words;
        VkShaderModule out = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &m, nullptr, &out);
        return out;
    }
    VkPipeline compute_pipeline(const uint32_t *words, size_t bytes, VkPipelineLayout layout);
    // guest memory read where it lies, for a trace being written
    void traced(uint32_t address, uint32_t bytes) {
        if (trace_) trace_->read(address, bytes);
    }
    void traced(const std::vector<Range> &ranges) {
        if (trace_)
            for (auto [start, end] : ranges) trace_->read(start, end - start);
    }
    static void full_barrier(VkCommandBuffer cmd) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    // an image's first use: from nothing to the layout it keeps
    static void begin_image(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t levels,
                            uint32_t layers) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = image;
        b.subresourceRange = {aspect, 0, levels, 0, layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ---- submissions ----
    // the command buffer being recorded, begun if none is
    VkCommandBuffer cmd();
    // the recording submitted (the next slot reclaimed: waited for if its
    // last submission is still running)
    void flush();
    // the oldest submission in flight finished (waited for, or only if done)
    bool finish_oldest(bool wait);
    void finish(Slot &s);
    void poll() {
        while (fatal_.empty() && finish_oldest(false)) {}
    }
    void wait_serial(uint64_t serial);
    void sync() {
        flush();
        while (fatal_.empty() && finish_oldest(true)) {}
    }
    // the render pass ended, if one is under way
    void end_pass() {
        end_query(); // a query does not outlive its render pass
        if (in_pass_) vkCmdEndRendering(slots_[current_].cmd);
        in_pass_ = false;
    }
    // ---- occlusion queries ----
    //
    // As Xenia's query segments (command_processor.cc's OpenQuerySegment and
    // CloseQuerySegment, the backends' OpenQuery and CloseQuery): the draws
    // between two EVENT_WRITE_ZPD count for one report through host occlusion
    // queries, one per render pass they fall in (a query cannot cross one),
    // opened at the first draw that counts; each query's samples are read
    // when its submission is done, and a report is answered (zpd_poll) once
    // all of its queries are in. Draws after a swap count for no report until
    // the next event (Xenia's EndZPDFrame). A draw that finds no query free
    // makes its report at least one sample, as Xenia's fast mode does.
    struct ZpdCount {
        uint64_t samples = 0;
        uint32_t open = 0; // queries not read yet
        bool ended = false, uncounted = false;
    };
    static constexpr uint32_t OCCLUSION_QUERIES = 4096; // a slot's
    uint64_t zpd_current_ = 0;                          // the report draws count for (0: none)
    uint32_t zpd_active_ = UINT32_MAX;                  // the query open in the render pass
    std::map<uint64_t, ZpdCount> zpd_counts_;
    std::vector<std::pair<uint64_t, uint64_t>> zpd_resolved_; // for zpd_poll
    void zpd_answer(uint64_t handle, ZpdCount &count) {
        report_.zpd_reports++;
        zpd_resolved_.emplace_back(handle, count.uncounted ? std::max<uint64_t>(count.samples, 1) : count.samples);
        zpd_counts_.erase(handle);
    }
    void end_query() {
        if (zpd_active_ == UINT32_MAX) return;
        vkCmdEndQuery(slots_[current_].cmd, slots_[current_].occlusion, zpd_active_);
        zpd_active_ = UINT32_MAX;
    }
    void zpd_report_ended(uint64_t ended, uint64_t next) {
        end_query();
        if (ended) {
            ZpdCount &count = zpd_counts_[ended];
            count.ended = true;
            if (!count.open) zpd_answer(ended, count);
        }
        zpd_current_ = next;
    }
    // before a draw that rasterizes, in its render pass: the query its report counts it with
    void zpd_draw(VkCommandBuffer c) {
        if (!zpd_current_ || zpd_active_ != UINT32_MAX) return;
        Slot &s = slots_[current_];
        if (!s.occlusion || s.occlusion_reports.size() >= OCCLUSION_QUERIES) {
            zpd_counts_[zpd_current_].uncounted = true;
            return;
        }
        zpd_active_ = uint32_t(s.occlusion_reports.size());
        s.occlusion_reports.push_back(zpd_current_);
        s.occlusion_used = uint32_t(s.occlusion_reports.size());
        zpd_counts_[zpd_current_].open++;
        report_.occlusion_queries++;
        vkCmdBeginQuery(c, s.occlusion, zpd_active_, d_.precise_occlusion() ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    }
    // the slot's submission is done: its queries' samples to their reports
    void zpd_collect(Slot &s) {
        if (s.occlusion_reports.empty()) return;
        std::vector<uint64_t> samples(s.occlusion_reports.size());
        vkGetQueryPoolResults(dev_, s.occlusion, 0, uint32_t(samples.size()), samples.size() * 8, samples.data(), 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        for (size_t i = 0; i < samples.size(); i++) {
            auto it = zpd_counts_.find(s.occlusion_reports[i]);
            if (it == zpd_counts_.end()) continue;
            it->second.samples += samples[i];
            if (--it->second.open == 0 && it->second.ended) zpd_answer(it->first, it->second);
        }
        s.occlusion_reports.clear();
    }
    // for a transfer, compute or clear: no render pass, what came before done
    VkCommandBuffer outside() {
        VkCommandBuffer c = cmd();
        end_pass();
        full_barrier(c);
        return c;
    }
    // the draw's render pass: the one under way, or a new one after what came before
    VkCommandBuffer begin_pass(const Pass &pass, RenderTarget *const colors[4], RenderTarget *depth);

    // ---- the arena: host memory each submission's uploads come from ----
    VkDeviceSize arena_take(VkDeviceSize bytes, VkDeviceSize align);
    // the same, a new submission begun when they do not fit the current
    // one's share; NONE when they would not fit any
    VkDeviceSize arena(VkDeviceSize bytes, VkDeviceSize align) {
        VkDeviceSize at = arena_take(bytes, align);
        if (at != NONE || bytes + align > ARENA_SLOT) return at;
        flush();
        return arena_take(bytes, align);
    }
    uint8_t *arena_data(VkDeviceSize at) { return static_cast<uint8_t *>(arena_.mapped) + at; }
    // room for bytes to copy from: the arena, or a buffer of their own that
    // goes when the submission is done
    uint8_t *staging(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &offset);
    // room in the submission's read-back buffers
    uint8_t *readback(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &offset);
    // a device buffer grown to bytes (everything in flight finished first),
    // its descriptor rewritten
    bool grow(vk::Buffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, VkDescriptorSet set, uint32_t binding);

    // ---- the mirror: guest memory on the GPU (Xenia's shared memory) ----
    // By the memory's watch pages: the write count the mirror's copy of each
    // is as of (0: never given), and whether it holds what the GPU wrote
    // there (a resolve, a memory export) and guest memory does not. A page is
    // current while no write numbered above its count came; a page the GPU
    // holds is current until the CPU writes it.
    bool current(uint32_t page) const {
        uint32_t at = mirror_at_[page];
        return at && stamps_[page].load(std::memory_order_acquire) <= at;
    }
    bool gpu_page(uint32_t page) const {
        return (gpu_pages_[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1;
    }
    void set_gpu_page(uint32_t page, bool on) {
        if (on)
            gpu_pages_[page >> 6].fetch_or(1ull << (page & 63), std::memory_order_relaxed);
        else if (gpu_page(page))
            gpu_pages_[page >> 6].fetch_and(~(1ull << (page & 63)), std::memory_order_relaxed);
    }
    bool gpu_held(uint32_t page) const { return gpu_page(page) && current(page); }
    uint32_t first_page(const Range &r) const { return r.first >> page_shift_; }
    uint32_t last_page(const Range &r) const { return (r.second - 1) >> page_shift_; }
    bool gpu_held(const std::vector<Range> &ranges) const;
    std::vector<Range> cpu_parts(const std::vector<Range> &ranges) const;
    VkDeviceSize request(const std::vector<Range> &ranges);
    void gpu_written(const std::vector<Range> &ranges);
    bool unwritten(Texture &t);
    static std::pair<uint32_t, uint32_t> watch_bounds(void *self, uint32_t first, uint32_t last);
    // guest memory given a resolve's bytes, as readback_resolve says
    void read_back_resolve(const std::vector<Range> &written);
    std::unordered_map<uint64_t, ResolveReadback> resolve_readbacks_;
    void next_readback_frame();

    // ---- memexport: shaders writing guest memory, into the mirror ----
    // The streams' pages are the GPU's after the draw (gpu_written), as
    // Xenia keeps them on the GPU, until the CPU writes them.
    static constexpr uint32_t PAGE_SHIFT = 12;
    void export_ranges(const Shader &s, uint32_t stage, std::vector<Range> &ranges);
    void end_export(const std::vector<Range> &pages);

    vk::Device &d_;
    VkDevice dev_;
    gpu::CommandProcessor &processor_;
    Options options_;
    mutable std::mutex mutex_; // held through each of the processor's calls
    Report report_;
    std::string fatal_;
    const uint32_t *regs_ = nullptr; // the processor's register file, during a call
    GuestMemory memory_;             // the processor's physical memory
    Memory &mem_;                    // the same memory, with its write watches
    const std::atomic<uint32_t> *stamps_ = nullptr;
    uint32_t page_shift_ = 12; // the watch pages'
    std::vector<uint32_t> mirror_at_;
    std::unique_ptr<std::atomic<uint64_t>[]> gpu_pages_;
    void *watcher_ = nullptr;
    gpu::MemoryTrace *trace_ = nullptr; // a trace writer's, told what is read
    uint64_t vertex_hash_ = 0, pixel_hash_ = 0;
    bool have_vertex_ = false, have_pixel_ = false;
    float max_viewport_[2] = {};
    VkDeviceSize ubo_align_ = 256, ssbo_align_ = 64;

    std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
    std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, std::unique_ptr<RenderTarget>> targets_;
    std::map<std::array<uint32_t, 6>, std::unique_ptr<Texture>> textures_;
    std::map<std::array<uint32_t, 8>, std::pair<VkSampler, uint32_t>> samplers_;
    std::map<std::vector<uint32_t>, std::unique_ptr<PipelineEntry>> pipelines_;

    // submissions
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Slot slots_[SLOTS];
    uint32_t current_ = 0;
    uint64_t next_serial_ = 1, done_serial_ = 0;
    bool in_pass_ = false;
    Pass pass_;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    // the last constants and system constants put in the arena, and where
    harness::Constants last_constants_[2];
    System last_system_{};
    VkDeviceSize last_constants_at_[2] = {NONE, NONE}, last_system_at_ = NONE;

    vk::Buffer mirror_, arena_, scratch_, load_scratch_;
    std::deque<FrontRead> front_reads_; // oldest first

    // draws: set 0 (constants, system constants, vertex memory: the mirror),
    // set 1 bindless (images and samplers)
    VkDescriptorSetLayout set_layouts_[2] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool0_ = VK_NULL_HANDLE, bindless_pool_ = VK_NULL_HANDLE;
    // vertex memory: the arena (no longer drawn with: its host memory made
    // the vertex shaders wait on the bus), the mirror
    VkDescriptorSet draw_sets_[2] = {};
    VkDescriptorSet bindless_ = VK_NULL_HANDLE;
    uint32_t capacity_[3] = {}, next_index_[3] = {1, 1, 1}, sampler_capacity_ = 0, next_sampler_ = 1;
    VkShaderModule rectangle_gs_ = VK_NULL_HANDLE, point_gs_ = VK_NULL_HANDLE;
    vk::Image dummy_[3]; // 2D, 3D, cube
    VkImageView dummy_views_[3] = {};
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;
    std::vector<VkImageView> views_; // all made but the textures', destroyed at the end

    // resolves and texture loads (compute): set 0 binds a source and a destination
    VkDescriptorSetLayout compute_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout resolve_layout_ = VK_NULL_HANDLE, load_layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline_ = VK_NULL_HANDLE, load_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool compute_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE, load_set_ = VK_NULL_HANDLE, resolve_edram_set_ = VK_NULL_HANDLE;

    // EDRAM: the render targets' contents move through it when another
    // target takes over its tiles (Xenia's ownership transfers); it starts
    // as zeros, or as a trace's snapshot
    void acquire(RenderTarget *t, uint32_t rows);
    bool edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles);
    bool edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows);
    VkDeviceSize edram_mask(const std::vector<uint32_t> &tiles);
    EdramTarget edram_target(const RenderTarget &t, uint32_t height) const {
        return {t.base, t.pitch_tiles, t.msaa, t.format, t.width, height, t.depth ? 1u : 0u, t.wide ? 1u : 0u, 8u};
    }
    VkPipeline load_pipeline(const RenderTarget &t);
    VkDescriptorSet edram_set(uint32_t which);
    std::vector<RenderTarget *> owners_ = std::vector<RenderTarget *>(EDRAM_TILES, nullptr); // null: EDRAM holds it
    vk::Buffer edram_;
    VkDescriptorSetLayout edram_set_layouts_[2] = {}; // store, load
    VkPipelineLayout edram_layouts_[2] = {};
    std::vector<VkDescriptorPool> edram_pools_;
    VkDescriptorSet edram_load_set_ = VK_NULL_HANDLE;
    VkPipeline store_pipelines_[2] = {}; // 1x, multisampled
    VkShaderModule fullscreen_vs_ = VK_NULL_HANDLE, load_fs_[3] = {}; // color, depth, depth and stencil
    std::map<std::pair<VkFormat, uint32_t>, VkPipeline> load_pipelines_;
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
};

Impl::~Impl() {
    mem_.remove_watcher(watcher_);
    vkDeviceWaitIdle(dev_);
    pipeline_workers_.reset(); // before the shader modules and the cache they use
    save_pipeline_cache();
    for (auto &[key, rb] : resolve_readbacks_)
        for (vk::Buffer &b : rb.buffers) d_.destroy(b);
    if (pipeline_cache_) vkDestroyPipelineCache(dev_, pipeline_cache_, nullptr);
    for (Slot &s : slots_) {
        for (auto &f : s.after) f();
        for (vk::Buffer &b : s.readbacks) d_.destroy(b);
        if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
        if (s.queries) vkDestroyQueryPool(dev_, s.queries, nullptr);
        if (s.occlusion) vkDestroyQueryPool(dev_, s.occlusion, nullptr);
    }
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    for (auto &[k, s] : shaders_)
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
    for (auto &[k, p] : pipelines_)
        if (p->pipeline) vkDestroyPipeline(dev_, p->pipeline, nullptr);
    for (auto &[k, s] : samplers_) vkDestroySampler(dev_, s.first, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (auto &[k, t] : targets_) d_.destroy(t->image);
    for (auto &[k, t] : textures_) {
        if (t->view) vkDestroyImageView(dev_, t->view, nullptr);
        if (t->signed_view) vkDestroyImageView(dev_, t->signed_view, nullptr);
        d_.destroy(t->image);
        if (t->signed_image.image) d_.destroy(t->signed_image);
    }
    for (auto &im : dummy_) d_.destroy(im);
    if (dummy_sampler_) vkDestroySampler(dev_, dummy_sampler_, nullptr);
    for (VkShaderModule m : {rectangle_gs_, point_gs_})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto &[k, p] : load_pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (VkPipeline p : {store_pipelines_[0], store_pipelines_[1], resolve_pipeline_, load_pipeline_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : {fullscreen_vs_, load_fs_[0], load_fs_[1], load_fs_[2]})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (VkDescriptorPool p : edram_pools_) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkDescriptorPool p : {pool0_, bindless_pool_, compute_pool_})
        if (p) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkPipelineLayout l : {edram_layouts_[0], edram_layouts_[1], layout_, resolve_layout_, load_layout_})
        if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (VkDescriptorSetLayout l : {edram_set_layouts_[0], edram_set_layouts_[1], set_layouts_[0], set_layouts_[1],
                                    compute_set_layout_})
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    for (vk::Buffer *b : {&edram_, &mirror_, &arena_, &scratch_, &load_scratch_}) d_.destroy(*b);
}

// keep: destroyed with the renderer, else by the caller
VkImageView Impl::make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                            VkComponentMapping components, uint32_t levels, uint32_t layers, bool keep) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = type;
    info.format = format;
    info.components = components;
    info.subresourceRange = {aspect, 0, levels, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev_, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (keep) views_.push_back(view);
    return view;
}

VkPipeline Impl::compute_pipeline(const uint32_t *words, size_t bytes, VkPipelineLayout layout) {
    VkShaderModule m = module(words, bytes);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo c{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    c.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    c.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev_, pipeline_cache_, 1, &c, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev_, m, nullptr);
    return p;
}

// ---- the pipeline cache ----

// Vulkan's pipeline cache, made from the file the options name when it is
// there (Vulkan checks the data's header against the device and the driver
// and starts empty on a mismatch), and given to every pipeline made: a
// game's pipelines are compiled by the driver once, not at each run (El Chavo
// Kart's title screen made 24 in its first frame, 2.5 s on this thread).
void Impl::open_pipeline_cache() {
    std::vector<char> data;
    if (!options_.pipeline_cache.empty()) {
        if (FILE *f = std::fopen(options_.pipeline_cache.string().c_str(), "rb")) {
            char chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) data.insert(data.end(), chunk, chunk + n);
            std::fclose(f);
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        info.initialDataSize = 0, info.pInitialData = nullptr; // unreadable data: an empty cache
        if (vkCreatePipelineCache(dev_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) pipeline_cache_ = VK_NULL_HANDLE;
        data.clear();
    }
    report_.pipeline_cache_read = data.size();
}

// The cache written to its file (through a temporary file renamed over it,
// so a run cut short leaves the last whole one) when pipelines were made
// since the last time
void Impl::save_pipeline_cache() {
    if (!pipeline_cache_ || options_.pipeline_cache.empty() || pipelines_saved_ == report_.pipelines) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(options_.pipeline_cache.parent_path(), ec);
    std::filesystem::path temporary = options_.pipeline_cache;
    temporary += ".new";
    FILE *f = std::fopen(temporary.string().c_str(), "wb");
    if (!f) return;
    const bool written = std::fwrite(data.data(), 1, size, f) == size;
    std::fclose(f);
    if (!written) return;
    std::filesystem::rename(temporary, options_.pipeline_cache, ec);
    if (ec) return;
    pipelines_saved_ = report_.pipelines;
    report_.pipeline_cache_written = size;
}

bool Impl::init(std::string &error) {
    open_pipeline_cache();
    support_ = texture_support(d_);
    if (options_.async_pipelines) {
        // Xenia's d3d12_pipeline_creation_threads: -1 three quarters of the
        // logical cores, else as many as asked up to their number
        uint32_t cores = std::max(std::thread::hardware_concurrency(), 1u);
        uint32_t threads = options_.pipeline_threads < 0 ? std::max(cores * 3 / 4, 1u)
                                                        : std::min(uint32_t(options_.pipeline_threads), cores);
        if (threads) pipeline_workers_ = std::make_unique<PipelineWorkers>(dev_, pipeline_cache_, threads);
    }
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &p12;
    vkGetPhysicalDeviceProperties2(d_.physical(), &props);
    const VkPhysicalDeviceLimits &limits = props.properties.limits;
    max_viewport_[0] = float(limits.maxViewportDimensions[0]);
    max_viewport_[1] = float(limits.maxViewportDimensions[1]);
    ubo_align_ = std::max<VkDeviceSize>(limits.minUniformBufferOffsetAlignment, 16);
    ssbo_align_ = std::max<VkDeviceSize>(limits.minStorageBufferOffsetAlignment, 16);
    // the bindless tables within what the device binds after binding
    uint64_t images = uint64_t(IMAGES[0]) + IMAGES[1] + IMAGES[2];
    uint64_t image_limit = std::min(p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                    p12.maxDescriptorSetUpdateAfterBindSampledImages);
    for (int k = 0; k < 3; k++)
        capacity_[k] = images <= image_limit ? IMAGES[k] : uint32_t(uint64_t(IMAGES[k]) * image_limit / images);
    sampler_capacity_ = std::min({SAMPLERS, p12.maxPerStageDescriptorUpdateAfterBindSamplers,
                                  p12.maxDescriptorSetUpdateAfterBindSamplers});
    if (capacity_[0] < 256 || capacity_[1] < 16 || capacity_[2] < 16 || sampler_capacity_ < 64)
        return error = "the device binds too few images after binding for the renderer's tables", false;

    mirror_ = d_.create_buffer(GUEST_MEMORY, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               false);
    arena_ = d_.create_buffer(VkDeviceSize(SLOTS) * ARENA_SLOT,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              true);
    const VkBufferUsageFlags scratch_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    scratch_ = d_.create_buffer(16u << 20, scratch_usage, false);
    load_scratch_ = d_.create_buffer(16u << 20, scratch_usage, false);
    if (!mirror_.buffer || !arena_.buffer || !scratch_.buffer || !load_scratch_.buffer)
        return error = "could not make the renderer's buffers (512 MB of device memory for guest memory's mirror)", false;

    if (!d_.format_supported(d24s8_, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;

    // submissions
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = d_.queue_family();
    if (vkCreateCommandPool(dev_, &pool_info, nullptr, &pool_) != VK_SUCCESS) return error = "vkCreateCommandPool failed", false;
    for (Slot &s : slots_) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev_, &alloc, &s.cmd) != VK_SUCCESS ||
            vkCreateFence(dev_, &fence, nullptr, &s.fence) != VK_SUCCESS)
            return error = "could not make the submission slots", false;
    }
    if (options_.gpu_timing) {
        // timestamps where the queue has them (none: no timing)
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d_.physical(), &families, nullptr);
        std::vector<VkQueueFamilyProperties> family(families);
        vkGetPhysicalDeviceQueueFamilyProperties(d_.physical(), &families, family.data());
        uint32_t bits = d_.queue_family() < families ? family[d_.queue_family()].timestampValidBits : 0;
        timestamp_period_ = limits.timestampPeriod;
        timestamp_mask_ = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        for (Slot &s : slots_) {
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = TIMESTAMPS;
            if (bits && vkCreateQueryPool(dev_, &q, nullptr, &s.queries) != VK_SUCCESS) s.queries = VK_NULL_HANDLE;
        }
    }

    // draws: set 0, three uniform buffers in the arena at offsets of their
    // own per draw, and vertex memory; set 1, the bindless tables
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b0[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
    };
    VkDescriptorSetLayoutBinding b1[4] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_[0], stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_[1], stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_[2], stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity_, stages, nullptr},
    };
    const VkDescriptorBindingFlags bindless_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags partly[4] = {bindless_flags, bindless_flags, bindless_flags, bindless_flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 4;
    flags.pBindingFlags = partly;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 4;
    set_info.pBindings = b0;
    if (vkCreateDescriptorSetLayout(dev_, &set_info, nullptr, &set_layouts_[0]) != VK_SUCCESS)
        return error = "vkCreateDescriptorSetLayout failed", false;
    set_info.pNext = &flags;
    set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    set_info.pBindings = b1;
    if (vkCreateDescriptorSetLayout(dev_, &set_info, nullptr, &set_layouts_[1]) != VK_SUCCESS)
        return error = "vkCreateDescriptorSetLayout failed", false;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = set_layouts_;
    if (vkCreatePipelineLayout(dev_, &layout_info, nullptr, &layout_) != VK_SUCCESS)
        return error = "vkCreatePipelineLayout failed", false;
    {
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(dev_, &pool, nullptr, &pool0_) != VK_SUCCESS)
            return error = "vkCreateDescriptorPool failed", false;
        VkDescriptorPoolSize bindless_sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_[0] + capacity_[1] + capacity_[2]},
            {VK_DESCRIPTOR_TYPE_SAMPLER, sampler_capacity_}};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.pPoolSizes = bindless_sizes;
        if (vkCreateDescriptorPool(dev_, &pool, nullptr, &bindless_pool_) != VK_SUCCESS)
            return error = "vkCreateDescriptorPool failed", false;
        VkDescriptorSetLayout twice[2] = {set_layouts_[0], set_layouts_[0]};
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool0_;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = twice;
        if (vkAllocateDescriptorSets(dev_, &alloc, draw_sets_) != VK_SUCCESS)
            return error = "vkAllocateDescriptorSets failed", false;
        alloc.descriptorPool = bindless_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layouts_[1];
        if (vkAllocateDescriptorSets(dev_, &alloc, &bindless_) != VK_SUCCESS)
            return error = "vkAllocateDescriptorSets failed", false;
        for (int k = 0; k < 2; k++) {
            VkDescriptorBufferInfo buffers[4] = {{arena_.buffer, 0, sizeof(harness::Constants)},
                                                 {k ? mirror_.buffer : arena_.buffer, 0, VK_WHOLE_SIZE},
                                                 {arena_.buffer, 0, sizeof(System)},
                                                 {arena_.buffer, 0, sizeof(harness::Constants)}};
            VkWriteDescriptorSet writes[4] = {};
            for (uint32_t b = 0; b < 4; b++) {
                writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[b].dstSet = draw_sets_[k];
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[b].pBufferInfo = &buffers[b];
            }
            vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
        }
    }

    if (d_.geometry_shader()) {
        rectangle_gs_ = module(RECTANGLE_LIST_GS, sizeof(RECTANGLE_LIST_GS));
        point_gs_ = module(POINT_LIST_GS, sizeof(POINT_LIST_GS));
        if (!rectangle_gs_ || !point_gs_) return error = "could not make the geometry shaders", false;
    }

    // EDRAM: the buffer, store (compute) and load (draw) pipelines; the tile
    // masks come from the arena
    edram_ = d_.create_buffer(EDRAM_BYTES,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              false);
    if (!edram_.buffer) return error = "could not make the EDRAM buffer", false;
    {
        VkDescriptorSetLayoutBinding store[4] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorBindingFlags store_flags[4] = {0, 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo store_binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        store_binding_flags.bindingCount = 4;
        store_binding_flags.pBindingFlags = store_flags;
        VkDescriptorSetLayoutBinding load[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.pNext = &store_binding_flags;
        info.bindingCount = 4;
        info.pBindings = store;
        vkCreateDescriptorSetLayout(dev_, &info, nullptr, &edram_set_layouts_[0]);
        info.pNext = nullptr;
        info.bindingCount = 2;
        info.pBindings = load;
        vkCreateDescriptorSetLayout(dev_, &info, nullptr, &edram_set_layouts_[1]);
        for (int k = 0; k < 2; k++) {
            VkPushConstantRange range{VkShaderStageFlags(k == 0 ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT), 0,
                                      sizeof(EdramTarget)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &edram_set_layouts_[k];
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(dev_, &layout, nullptr, &edram_layouts_[k]) != VK_SUCCESS)
                return error = "could not make the EDRAM pipeline layouts", false;
        }
        store_pipelines_[0] = compute_pipeline(EDRAM_STORE, sizeof(EDRAM_STORE), edram_layouts_[0]);
        store_pipelines_[1] = compute_pipeline(EDRAM_STORE_MS, sizeof(EDRAM_STORE_MS), edram_layouts_[0]);
        if (!store_pipelines_[0] || !store_pipelines_[1]) return error = "could not make the EDRAM store pipelines", false;
        fullscreen_vs_ = module(FULLSCREEN_VS, sizeof(FULLSCREEN_VS));
        load_fs_[0] = module(EDRAM_LOAD, sizeof(EDRAM_LOAD));
        load_fs_[1] = module(EDRAM_LOAD_DEPTH, sizeof(EDRAM_LOAD_DEPTH));
        if (d_.stencil_export()) load_fs_[2] = module(EDRAM_LOAD_STENCIL, sizeof(EDRAM_LOAD_STENCIL));
        edram_load_set_ = edram_set(1);
        if (!edram_load_set_) return error = "could not make the EDRAM load's descriptors", false;
        VkDescriptorBufferInfo buffers[2] = {{edram_.buffer, 0, VK_WHOLE_SIZE}, {arena_.buffer, 0, EDRAM_TILES * 4}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t b = 0; b < 2; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = edram_load_set_;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(dev_, 2, writes, 0, nullptr);
    }

    // resolves and texture loads: a source and a destination buffer
    {
        VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 2;
        info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev_, &info, nullptr, &compute_set_layout_) != VK_SUCCESS)
            return error = "vkCreateDescriptorSetLayout failed", false;
        uint32_t push_sizes[2] = {sizeof(ResolveConstants), sizeof(LoadConstants)};
        VkPipelineLayout *layouts[2] = {&resolve_layout_, &load_layout_};
        for (int k = 0; k < 2; k++) {
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_sizes[k]};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &compute_set_layout_;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(dev_, &layout, nullptr, layouts[k]) != VK_SUCCESS)
                return error = "vkCreatePipelineLayout failed", false;
        }
        resolve_pipeline_ = compute_pipeline(RESOLVE, sizeof(RESOLVE), resolve_layout_);
        load_pipeline_ = compute_pipeline(TEXTURE_LOAD, sizeof(TEXTURE_LOAD), load_layout_);
        if (!resolve_pipeline_ || !load_pipeline_) return error = "could not make the resolve and load pipelines", false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 3;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(dev_, &pool, nullptr, &compute_pool_) != VK_SUCCESS)
            return error = "vkCreateDescriptorPool failed", false;
        VkDescriptorSetLayout thrice[3] = {compute_set_layout_, compute_set_layout_, compute_set_layout_};
        VkDescriptorSet sets[3];
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = compute_pool_;
        alloc.descriptorSetCount = 3;
        alloc.pSetLayouts = thrice;
        if (vkAllocateDescriptorSets(dev_, &alloc, sets) != VK_SUCCESS) return error = "vkAllocateDescriptorSets failed", false;
        // the resolves' from the scratch buffer and from EDRAM (multisampled
        // sources), and the texture loads'
        resolve_set_ = sets[0], load_set_ = sets[1], resolve_edram_set_ = sets[2];
        VkDescriptorBufferInfo buffers[6] = {{scratch_.buffer, 0, VK_WHOLE_SIZE},      {mirror_.buffer, 0, VK_WHOLE_SIZE},
                                             {mirror_.buffer, 0, VK_WHOLE_SIZE},       {load_scratch_.buffer, 0, VK_WHOLE_SIZE},
                                             {edram_.buffer, 0, VK_WHOLE_SIZE}, {mirror_.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[6] = {};
        for (uint32_t k = 0; k < 6; k++) {
            writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[k].dstSet = k < 2 ? resolve_set_ : k < 4 ? load_set_ : resolve_edram_set_;
            writes[k].dstBinding = k & 1;
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[k].pBufferInfo = &buffers[k];
        }
        vkUpdateDescriptorSets(dev_, 6, writes, 0, nullptr);
    }

    // the stand-ins for slots without a texture, index 0 of each table: 1x1
    // transparent black (2D textures are sampled as 2D arrays)
    const VkImageType types[] = {VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D, VK_IMAGE_TYPE_2D};
    const VkImageViewType view_types[] = {VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE};
    for (int k = 0; k < 3; k++) {
        dummy_[k] = d_.create_image(types[k], VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, k == 2 ? 6 : 1,
                                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    k == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
        dummy_views_[k] = make_view(dummy_[k].image, view_types[k], VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, {},
                                    1, k == 2 ? 6 : 1);
        if (!dummy_views_[k]) return error = "could not make the stand-in images", false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    vkCreateSampler(dev_, &si, nullptr, &dummy_sampler_);
    {
        VkDescriptorImageInfo images[4] = {{VK_NULL_HANDLE, dummy_views_[0], VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, dummy_views_[1], VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, dummy_views_[2], VK_IMAGE_LAYOUT_GENERAL},
                                           {dummy_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
        VkWriteDescriptorSet writes[4] = {};
        for (uint32_t b = 0; b < 4; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = bindless_;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[b].pImageInfo = &images[b];
        }
        vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
    }
    VkCommandBuffer c = cmd();
    vkCmdFillBuffer(c, edram_.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(c, mirror_.buffer, 0, VK_WHOLE_SIZE, 0);
    for (auto &im : dummy_) {
        begin_image(c, im.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, im.layers);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im.layers};
        vkCmdClearColorImage(c, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    sync();
    if (!fatal_.empty()) return error = fatal_, false;
    return true;
}

// ---- submissions ----

VkCommandBuffer Impl::cmd() {
    Slot &s = slots_[current_];
    if (!s.recording) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmd, &begin);
        s.recording = true;
        s.serial = next_serial_++;
        s.used = 0;
        s.readback = 0, s.readback_used = 0;
        in_pass_ = false;
        bound_pipeline_ = VK_NULL_HANDLE;
        last_constants_at_[0] = last_constants_at_[1] = last_system_at_ = NONE;
        if (s.queries) {
            vkCmdResetQueryPool(s.cmd, s.queries, 0, TIMESTAMPS);
            s.marks.clear();
            gpu_mark("other (barriers, uploads into the mirror, clears of new targets)");
        }
        // the occlusion queries' pool, reset where its last submission used it
        if (!s.occlusion) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_OCCLUSION;
            info.queryCount = OCCLUSION_QUERIES;
            if (vkCreateQueryPool(dev_, &info, nullptr, &s.occlusion) != VK_SUCCESS) s.occlusion = VK_NULL_HANDLE;
            s.occlusion_used = OCCLUSION_QUERIES;
        }
        if (s.occlusion && s.occlusion_used) vkCmdResetQueryPool(s.cmd, s.occlusion, 0, s.occlusion_used);
        s.occlusion_used = 0;
    }
    return s.cmd;
}

void Impl::gpu_mark(uint32_t kind) {
    VkCommandBuffer c = cmd();
    Slot &s = slots_[current_];
    if (!s.queries || (!s.marks.empty() && s.marks.back() == kind)) return;
    // one kept for the submission's end
    if (s.marks.size() + 1 >= TIMESTAMPS) return;
    vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, uint32_t(s.marks.size()));
    s.marks.push_back(kind);
}

// The finished submission's timestamps, each interval to the kind of work
// that began it
void Impl::gpu_collect(Slot &s) {
    if (!s.queries || s.marks.size() < 2) return;
    std::vector<uint64_t> ticks(s.marks.size());
    if (vkGetQueryPoolResults(dev_, s.queries, 0, uint32_t(ticks.size()), ticks.size() * 8, ticks.data(), 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    double scale = timestamp_period_ * 1e-9;
    for (size_t i = 0; i + 1 < ticks.size(); i++) {
        double seconds = double((ticks[i + 1] - ticks[i]) & timestamp_mask_) * scale;
        Report::GpuTime &t = report_.gpu_time[gpu_kinds_[s.marks[i]]];
        t.seconds += seconds, t.intervals++;
        report_.gpu_seconds += seconds;
    }
    s.marks.clear();
}

// a render target as the GPU timing names it
std::string Impl::target_name(const RenderTarget &t) const {
    char name[96];
    std::snprintf(name, sizeof(name), "%s %s, pitch %u, %ux MSAA, base %u", t.depth ? "depth" : "color",
                  t.depth ? (t.format ? "D24FS8" : "D24S8") : color_target_format_name(t.format), t.pitch, 1u << t.msaa,
                  t.base);
    return name;
}

void Impl::flush() {
    Slot &s = slots_[current_];
    if (!s.recording) return;
    end_pass();
    if (s.queries && !s.marks.empty()) {
        vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, uint32_t(s.marks.size()));
        s.marks.push_back(GPU_END);
    }
    vkEndCommandBuffer(s.cmd);
    s.recording = false;
    {
        Timer timer(report_.submit_seconds);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &s.cmd;
        vkResetFences(dev_, 1, &s.fence);
        if (VkResult r = d_.submit(1, &submit, s.fence); r != VK_SUCCESS)
            fatal_ = "a submission failed (" + std::to_string(r) + ")";
    }
    report_.submissions++;
    current_ = (current_ + 1) % SLOTS;
    // the next slot's last submission done before it records again
    Slot &next = slots_[current_];
    while (fatal_.empty() && next.serial && done_serial_ < next.serial && finish_oldest(true)) {}
}

bool Impl::finish_oldest(bool wait) {
    Slot *oldest = nullptr;
    for (Slot &s : slots_)
        if (s.serial && !s.recording && (!oldest || s.serial < oldest->serial)) oldest = &s;
    if (!oldest) return false;
    VkResult status = vkGetFenceStatus(dev_, oldest->fence);
    if (status == VK_NOT_READY) {
        if (!wait) return false;
        Timer submit(report_.submit_seconds), waiting(report_.wait_seconds);
        report_.waits++;
        status = vkWaitForFences(dev_, 1, &oldest->fence, VK_TRUE, 20'000'000'000ull);
    }
    if (status != VK_SUCCESS) {
        fatal_ = "waiting for the GPU failed (" + std::to_string(status) + ")";
        return false;
    }
    finish(*oldest);
    return true;
}

// The GPU is done with the slot's submission: what waited for that goes
// (images and buffers no longer used).
void Impl::finish(Slot &s) {
    gpu_collect(s);
    zpd_collect(s);
    for (auto &f : s.after) f();
    s.after.clear();
    done_serial_ = s.serial;
    // the front buffers it read back, to on_frame
    while (!front_reads_.empty() && front_reads_.front().serial <= s.serial) {
        FrontRead read = std::move(front_reads_.front());
        front_reads_.pop_front();
        deliver(read);
    }
    s.serial = 0;
}

void Impl::wait_serial(uint64_t serial) {
    if (slots_[current_].recording && slots_[current_].serial <= serial) flush();
    while (fatal_.empty() && done_serial_ < serial && finish_oldest(true)) {}
}

VkCommandBuffer Impl::begin_pass(const Pass &pass, RenderTarget *const colors[4], RenderTarget *depth) {
    VkCommandBuffer c = cmd();
    if (in_pass_ && pass_ == pass) return c;
    end_pass();
    full_barrier(c);
    VkRenderingAttachmentInfo attachments[4] = {};
    for (uint32_t i = 0; i < 4; i++) {
        attachments[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachments[i].imageView = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
        attachments[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_attachment.imageView = depth ? depth->view : VK_NULL_HANDLE;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering_info.renderArea = {{0, 0}, {pass.width, pass.height}};
    rendering_info.layerCount = 1;
    // none at all for memory exports alone (their pipelines discard rasterization)
    bool attachments_used = depth != nullptr;
    for (uint32_t i = 0; i < 4; i++) attachments_used |= colors[i] != nullptr;
    rendering_info.colorAttachmentCount = attachments_used ? 4 : 0;
    rendering_info.pColorAttachments = attachments;
    if (depth) rendering_info.pDepthAttachment = rendering_info.pStencilAttachment = &depth_attachment;
    if (options_.gpu_timing) {
        std::string kind = "draws:";
        for (uint32_t i = 0; i < 4; i++)
            if (colors[i]) kind += " " + target_name(*colors[i]) + ";";
        if (depth) kind += " " + target_name(*depth);
        gpu_mark(kind);
    }
    vkCmdBeginRendering(c, &rendering_info);
    in_pass_ = true;
    pass_ = pass;
    return c;
}

// ---- the arena ----

VkDeviceSize Impl::arena_take(VkDeviceSize bytes, VkDeviceSize align) {
    cmd();
    Slot &s = slots_[current_];
    VkDeviceSize at = (s.used + align - 1) / align * align;
    if (at + bytes > ARENA_SLOT) return NONE;
    s.used = at + bytes;
    return VkDeviceSize(current_) * ARENA_SLOT + at;
}

uint8_t *Impl::staging(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &offset) {
    VkDeviceSize at = arena(bytes, 16);
    if (at != NONE) {
        buffer = arena_.buffer, offset = at;
        return arena_data(at);
    }
    vk::Buffer own = d_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!own.buffer) {
        fatal_ = "could not make a staging buffer";
        return nullptr;
    }
    cmd();
    slots_[current_].after.push_back([this, own]() mutable { d_.destroy(own); });
    buffer = own.buffer, offset = 0;
    return static_cast<uint8_t *>(own.mapped);
}

uint8_t *Impl::readback(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &offset) {
    cmd();
    Slot &s = slots_[current_];
    bytes = (bytes + 15) & ~VkDeviceSize(15);
    for (; s.readback < s.readbacks.size(); s.readback++, s.readback_used = 0) {
        vk::Buffer &b = s.readbacks[s.readback];
        if (s.readback_used + bytes > b.size) continue;
        buffer = b.buffer, offset = s.readback_used;
        s.readback_used += bytes;
        return static_cast<uint8_t *>(b.mapped) + offset;
    }
    // cached memory, read by the CPU
    vk::Buffer b = d_.create_buffer(std::max(bytes, READBACK_CHUNK), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
    if (!b.buffer) {
        fatal_ = "could not make a read-back buffer";
        return nullptr;
    }
    s.readbacks.push_back(b);
    s.readback = s.readbacks.size() - 1, s.readback_used = bytes;
    buffer = b.buffer, offset = 0;
    return static_cast<uint8_t *>(b.mapped);
}

bool Impl::grow(vk::Buffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, VkDescriptorSet set,
                uint32_t binding) {
    if (buffer.size >= bytes) return true;
    sync();
    d_.destroy(buffer);
    buffer = d_.create_buffer(std::max(bytes, buffer.size * 2), usage, false);
    if (!buffer.buffer) {
        fatal_ = "could not grow a device buffer";
        return false;
    }
    VkDescriptorBufferInfo info{buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    return true;
}

// ---- the mirror ----

// whether the GPU holds any page of the ranges
bool Impl::gpu_held(const std::vector<Range> &ranges) const {
    for (const Range &r : ranges)
        if (r.second > r.first)
            for (uint32_t p = first_page(r); p <= last_page(r); p++)
                if (gpu_held(p)) return true;
    return false;
}

// the ranges less the pages the GPU holds: what of them guest memory has
std::vector<Range> Impl::cpu_parts(const std::vector<Range> &ranges) const {
    std::vector<Range> out;
    for (const Range &r : ranges) {
        if (r.second <= r.first) continue;
        uint32_t start = r.first;
        for (uint32_t p = first_page(r); p <= last_page(r); p++) {
            if (!gpu_held(p)) continue;
            uint32_t page_start = p << page_shift_, page_end = page_start + (1u << page_shift_);
            if (start < page_start) out.push_back({start, page_start});
            start = std::min(page_end, r.second);
        }
        if (start < r.second) out.push_back({start, r.second});
    }
    return out;
}

// Guest memory's bytes into the mirror, before what reads them there
// (Xenia's RequestRange): the pages of the ranges that are not current,
// watched first, then copied whole, the write count taken between the two (a
// write after the watch numbers its page above it, so the copy is seen to be
// old). Pages the GPU holds are current and keep its bytes. The bytes
// uploaded.
VkDeviceSize Impl::request(const std::vector<Range> &ranges) {
    if (ranges.empty()) return 0;
    traced(cpu_parts(ranges));
    // runs of pages to give, [first, end); a page taken is marked current
    // (UINT32_MAX) until the count is known, so ranges sharing it give it once
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (const Range &r : ranges) {
        if (r.second <= r.first) continue;
        for (uint32_t p = first_page(r); p <= last_page(r); p++) {
            if (current(p)) continue;
            mirror_at_[p] = UINT32_MAX;
            if (!runs.empty() && runs.back().second == p)
                runs.back().second++;
            else
                runs.push_back({p, p + 1});
        }
    }
    if (runs.empty()) return 0;
    for (auto [first, end] : runs) mem_.watch(first << page_shift_, (end - first) << page_shift_);
    uint32_t count = mem_.write_count();
    VkDeviceSize total = 0;
    for (auto [first, end] : runs) total += VkDeviceSize(end - first) << page_shift_;
    VkBuffer from;
    VkDeviceSize at;
    uint8_t *data = staging(total, from, at);
    if (!data) {
        for (auto [first, end] : runs) std::fill(mirror_at_.begin() + first, mirror_at_.begin() + end, 0u);
        return 0;
    }
    std::vector<VkBufferCopy> copies;
    VkDeviceSize offset = 0;
    for (auto [first, end] : runs) {
        uint32_t start = first << page_shift_;
        VkDeviceSize bytes = VkDeviceSize(end - first) << page_shift_;
        std::memcpy(data + offset, memory_.data() + start, bytes);
        copies.push_back({at + offset, start, bytes});
        offset += bytes;
        for (uint32_t p = first; p < end; p++) {
            mirror_at_[p] = count;
            set_gpu_page(p, false);
        }
    }
    VkCommandBuffer c = outside();
    gpu_mark("other (barriers, uploads into the mirror, clears of new targets)");
    vkCmdCopyBuffer(c, from, mirror_.buffer, uint32_t(copies.size()), copies.data());
    return total;
}

// The GPU wrote the ranges in the mirror (a resolve, a memory export; Xenia's
// RangeWrittenByGpu): their pages hold what guest memory does not, current
// until the CPU writes them, so they are watched. What else those pages hold
// was made current before (request).
void Impl::gpu_written(const std::vector<Range> &ranges) {
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (const Range &r : ranges) {
        if (r.second <= r.first) continue;
        uint32_t first = first_page(r), end = last_page(r) + 1;
        if (!runs.empty() && runs.back().second >= first)
            runs.back().second = std::max(runs.back().second, end);
        else
            runs.push_back({first, end});
    }
    for (auto [first, end] : runs) mem_.watch(first << page_shift_, (end - first) << page_shift_);
    uint32_t count = mem_.write_count();
    for (auto [first, end] : runs)
        for (uint32_t p = first; p < end; p++) {
            mirror_at_[p] = count;
            set_gpu_page(p, true);
        }
}

// Whether a texture's memory is unwritten since it was last known to be: no
// page of it numbered above its count (one read of the count when nothing
// was written anywhere since).
bool Impl::unwritten(Texture &t) {
    uint32_t count = mem_.write_count();
    if (count == t.checked) return true;
    for (const Range &r : t.ranges) {
        if (r.second <= r.first) continue;
        for (uint32_t p = first_page(r); p <= last_page(r); p++)
            if (stamps_[p].load(std::memory_order_acquire) > t.checked) return false;
    }
    t.checked = count;
    return true;
}

// How far around a written page the watches may go (Xenia's
// SharedMemory::MemoryInvalidationCallback, not exact): within the 64 pages
// around it, short of the nearest page the GPU holds, which a write would
// otherwise take back from the mirror. Called in the fault handler, on the
// writing thread: it reads the GPU's pages without the renderer's lock.
std::pair<uint32_t, uint32_t> Impl::watch_bounds(void *self, uint32_t first, uint32_t last) {
    const Impl &r = *static_cast<const Impl *>(self);
    if (first & 63) {
        uint64_t below = r.gpu_pages_[first >> 6].load(std::memory_order_relaxed) & ((1ull << (first & 63)) - 1);
        first = (first & ~63u) + uint32_t(64 - std::countl_zero(below));
    }
    if ((last & 63) != 63) {
        uint64_t above = r.gpu_pages_[last >> 6].load(std::memory_order_relaxed) & ~((2ull << (last & 63)) - 1);
        last = (last & ~63u) + uint32_t(std::max(std::countr_zero(above), 1) - 1);
    }
    return {first, last};
}

// ---- memexport ----
//
// As Xenia's IssueDraw does it: the streams a draw's shaders export to are
// found from their float constants; guest memory's bytes under their pages
// go into the mirror first (where it has nothing newer: Xenia's
// RequestRange), the draw's shaders write the mirror, and the pages are then
// the GPU's (Xenia's RangeWrittenByGpu): textures over them are loaded
// again, from the mirror, and vertex data there is read from the mirror,
// until the CPU writes them. Guest memory never gets the exported bytes, as
// with Xenia's default (readback_memexport off): copied back late they
// would overwrite what the CPU wrote there after its fence, which the
// processor writes before the GPU has run the export.

// a format's bits per element, for the formats exports take (Xenia's
// kResolvable formats); 0 for the others
static uint32_t export_format_bits(uint32_t format) {
    switch (format) {
    case 2: case 8: case 9: return 8;                                       // 8, 8_A, 8_B
    case 3: case 4: case 5: case 10: case 15: case 24: case 30: return 16; // 1_5_5_5 ... 16, 16_FLOAT
    case 6: case 7: case 14: case 16: case 17: case 25: case 31: case 36: // 8_8_8_8 ... 32_FLOAT
    case 50: case 54: case 55: case 56: return 32;                         // the AS_16_16_16_16 ones
    case 26: case 32: case 37: return 64;                                  // 16_16_16_16 (float), 32_32_FLOAT
    case 38: return 128;                                                   // 32_32_32_32_FLOAT
    default: return 0;
    }
}

// The pages of the streams the shader exports to (draw_util.cc's
// AddMemExportRanges): per float constant its eA takes a stream from, the
// stream's base and count, when the bits the sequencer checks are there, in
// a format exports take; whole pages, merged.
void Impl::export_ranges(const Shader &s, uint32_t stage, std::vector<Range> &ranges) {
    if (!s.exports) return;
    for (uint32_t c : s.stream_constants) {
        if (c > 255) continue;
        const uint32_t *k = &regs_[reg::ALU_CONSTANTS + stage * 1024 + c * 4];
        uint32_t count = k[3] & 0x7FFFFF;
        if (k[0] >> 30 != 1 || k[2] >> 20 != 0x4B0 || k[3] >> 23 != 0x96 || !count) continue;
        uint32_t format = (k[2] >> 8) & 0x3F, bits = export_format_bits(format);
        if (!bits) {
            approximate("memory export to format " + std::to_string(format) + " (not written)");
            continue;
        }
        uint64_t start = uint64_t(k[0] & 0x3FFFFFFF) << 2, end = start + uint64_t(count) * (bits / 8);
        if (start >= GUEST_MEMORY) continue;
        const uint64_t page = uint64_t(1) << PAGE_SHIFT;
        start &= ~(page - 1);
        end = std::min<uint64_t>((end + page - 1) & ~(page - 1), GUEST_MEMORY);
        ranges.push_back({uint32_t(start), uint32_t(end)});
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> merged;
    for (const Range &r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    ranges = std::move(merged);
}

// After the draw: the streams' pages are the GPU's until the CPU writes
// them; the textures over them loaded again at their next use. The pass
// ends, so what reads the mirror next comes after a barrier.
void Impl::end_export(const std::vector<Range> &pages) {
    if (pages.empty()) return;
    end_pass();
    gpu_written(pages);
    for (auto &[key, t] : textures_) {
        if (t->high <= pages.front().first || t->low >= pages.back().second) continue;
        for (const Range &r : pages)
            for (const Range &tr : t->ranges)
                if (overlaps(r, tr)) t->stale = true;
    }
}

// ---- shaders: what IM_LOAD and IM_LOAD_IMMEDIATE load, kept by hash ----

void Impl::load_shader(uint32_t type, std::span<const uint32_t> code) {
    if (type > 1) return;
    uint64_t hash = hash_words(type, code);
    auto &s = shaders_[hash];
    if (!s) {
        s = std::make_unique<Shader>();
        s->parsed = xf::ucode::parse_shader(type == 0 ? xf::ucode::ShaderType::Vertex : xf::ucode::ShaderType::Pixel, code);
        s->summary = xf::ucode::analyze(code);
        report_.shaders++;
        if (!options_.dump_shaders.empty()) {
            char name[64];
            std::snprintf(name, sizeof(name), "/shader_%016llX.ucode.bin.%s", (unsigned long long)hash,
                          type == 0 ? "vert" : "frag");
            if (std::FILE *f = std::fopen((options_.dump_shaders + name).c_str(), "wb")) {
                std::fwrite(code.data(), 4, code.size(), f);
                std::fclose(f);
            }
        }
    }
    (type == 0 ? vertex_hash_ : pixel_hash_) = hash;
    (type == 0 ? have_vertex_ : have_pixel_) = true;
}

Shader *Impl::shader(uint64_t hash) {
    auto it = shaders_.find(hash);
    return it == shaders_.end() ? nullptr : it->second.get();
}

bool Impl::translate(Shader &s, std::string &why) {
    if (!s.translated) {
        s.translated = true;
        bool pixel = s.parsed.type == xf::ucode::ShaderType::Pixel;
        for (const xf::ucode::ControlFlow &cf : s.parsed.control_flow) {
            const auto *e = std::get_if<xf::ucode::Exec>(&cf.instruction);
            if (!e) continue;
            for (const xf::ucode::ExecSlot &slot : e->slots) {
                if (const auto *a = std::get_if<xf::ucode::AluInstruction>(&slot.instruction)) {
                    for (const xf::ucode::Result *r : {&a->vector_and_constant_result, &a->scalar_result}) {
                        uint32_t mask = r->used_write_mask();
                        if (r->target == xf::ucode::StorageTarget::Color && mask)
                            s.colors_written |= 1u << (r->index & 3);
                        if (r->target == xf::ucode::StorageTarget::Depth && mask) s.writes_depth = true;
                        if (r->target == xf::ucode::StorageTarget::Position) {
                            s.writes_position_y |= (mask & 2) != 0;
                            s.writes_position_w |= (mask & 8) != 0;
                        }
                        if (r->target == xf::ucode::StorageTarget::PointSizeEdgeFlagKillVertex) {
                            s.writes_point_size |= (mask & 1) != 0;
                            s.writes_vertex_kill |= (mask & 4) != 0;
                        }
                        if (r->target == xf::ucode::StorageTarget::ExportData && r->index < 5 && mask) s.exports = true;
                    }
                    if (uint32_t c = a->memexport_stream_constant(); c != UINT32_MAX) s.stream_constants.insert(c);
                } else if (const auto *t = std::get_if<xf::ucode::TextureFetchInstruction>(&slot.instruction)) {
                    if (t->has_result()) s.fetches_textures = true;
                    if (t->opcode == xf::ucode::FetchOpcode::TextureFetch && !s.filters.count(t->operands[1].index))
                        s.filters[t->operands[1].index] = {uint32_t(t->mag_filter), uint32_t(t->min_filter),
                                                           uint32_t(t->mip_filter), uint32_t(t->aniso_filter)};
                }
            }
        }
        shader::Translation t = shader::translate(s.parsed, pixel ? shader::Stage::Pixel : shader::Stage::Vertex);
        if (!t.ok()) {
            s.unsupported = t.unsupported.empty() ? "the translator wrote nothing" : t.unsupported.front();
        } else {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = t.spirv.size() * 4;
            info.pCode = t.spirv.data();
            if (vkCreateShaderModule(dev_, &info, nullptr, &s.module) != VK_SUCCESS) s.unsupported = "vkCreateShaderModule failed";
        }
    }
    if (!s.unsupported.empty()) why = s.unsupported;
    return s.unsupported.empty();
}

// ---- render targets: EDRAM as host images, one per base, pitch, samples and format ----

VkFormat Impl::depth_vk_format(uint32_t format) const {
    return format == 0 ? d24s8_ : VK_FORMAT_D32_SFLOAT_S8_UINT; // D24S8, D24FS8
}

// a descriptor set of the EDRAM store's (0) or load's (1) layout, from a pool
// that grows as render targets are made
VkDescriptorSet Impl::edram_set(uint32_t which) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &edram_set_layouts_[which];
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!edram_pools_.empty()) {
        alloc.descriptorPool = edram_pools_.back();
        if (vkAllocateDescriptorSets(dev_, &alloc, &set) == VK_SUCCESS) return set;
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 64},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 64;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev_, &pool, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    edram_pools_.push_back(p);
    alloc.descriptorPool = p;
    if (vkAllocateDescriptorSets(dev_, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    return set;
}

RenderTarget *Impl::render_target(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format) {
    // by the format EDRAM stores (xenos.h's GetStorageColorFormat, as
    // Xenia keys its render targets): 2_10_10_10_AS_10_10_10_10 is
    // 2_10_10_10 and 2_10_10_10_FLOAT_AS_16_16_16_16 is 2_10_10_10_FLOAT,
    // the same target, so nothing moves through EDRAM's bits between them
    if (!depth && format == 10) format = 2;
    if (!depth && format == 12) format = 3;
    auto key = std::make_tuple(depth, base, pitch, msaa, format);
    auto it = targets_.find(key);
    if (it != targets_.end()) return it->second.get();

    auto t = std::make_unique<RenderTarget>();
    t->depth = depth, t->base = base, t->pitch = pitch, t->msaa = msaa, t->format = format;
    VkFormat vk_format;
    uint32_t bytes = 4;
    if (depth) {
        vk_format = depth_vk_format(format);
    } else {
        t->host = resolve::host_format_of_color_target(format);
        // 16-bit fixed targets as snorm: the shader scales -32..32 into it,
        // so the bits are the guest's (resolve.h reads them so)
        vk_format = t->host == texture::HostFormat::RG16     ? VK_FORMAT_R16G16_SNORM
                    : t->host == texture::HostFormat::RGBA16 ? VK_FORMAT_R16G16B16A16_SNORM
                                                             : vk_format_of(t->host);
        if (vk_format == VK_FORMAT_UNDEFINED) return nullptr;
        // the guest's bits per sample (xenos.h's IsColorRenderTargetFormat64bpp),
        // not the host's: 2_10_10_10_FLOAT is 32 bits in EDRAM, kept as
        // 16-bit floats here
        bytes = format == 5 || format == 7 || format == 15 ? 8 : 4;
    }
    // as many rows as fit in EDRAM from the base: a tile is 80x16 samples of
    // 32 bits (40 wide at 64 bits), 2x MSAA doubles the samples down, 4x
    // across too
    uint32_t samples_x = msaa >= 2 ? 2 : 1, samples_y = msaa >= 1 ? 2 : 1;
    uint32_t tile_width = (bytes == 8 ? 40 : 80) / samples_x, tile_height = 16 / samples_y;
    uint32_t pitch_tiles = std::max(1u, (pitch + tile_width - 1) / tile_width);
    uint32_t rows = std::max(1u, (EDRAM_TILES - std::min(base, EDRAM_TILES - 1)) / pitch_tiles);
    t->width = std::clamp(pitch, 1u, 8192u);
    t->height = std::clamp(rows * tile_height, 1u, 8192u);
    t->wide = bytes == 8;
    t->tile_width = tile_width, t->tile_height = tile_height;
    t->pitch_tiles = pitch_tiles;
    VkSampleCountFlagBits samples = msaa == 0 ? VK_SAMPLE_COUNT_1_BIT : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_4_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    t->image = d_.create_image(VK_IMAGE_TYPE_2D, vk_format, {t->width, t->height, 1}, 1, 1, samples, usage);
    if (!t->image.image) {
        fatal_ = "could not make a render target";
        return nullptr;
    }
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    t->view = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, aspect);
    t->sampled = depth ? make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_DEPTH_BIT) : t->view;
    if (depth) t->stencil = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_STENCIL_BIT);
    // its EDRAM store's descriptors: EDRAM, the tile mask (in the arena), its views
    t->store_set = edram_set(0);
    if (!t->store_set) {
        fatal_ = "could not make a render target's EDRAM descriptors";
        return nullptr;
    }
    VkDescriptorBufferInfo buffers[2] = {{edram_.buffer, 0, VK_WHOLE_SIZE}, {arena_.buffer, 0, EDRAM_TILES * 4}};
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, t->sampled, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, t->stencil, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t b = 0; b < 4; b++) {
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = t->store_set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b == 0   ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                   : b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                            : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (b < 2) writes[b].pBufferInfo = &buffers[b];
        else writes[b].pImageInfo = &images[b - 2];
    }
    vkUpdateDescriptorSets(dev_, t->stencil ? 4 : 3, writes, 0, nullptr);
    // cleared: its tiles come from EDRAM when it takes them over (acquire)
    VkCommandBuffer c = outside();
    gpu_mark("other (barriers, uploads into the mirror, clears of new targets)");
    begin_image(c, t->image.image, aspect, 1, 1);
    VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue zero{};
        vkCmdClearDepthStencilImage(c, t->image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    } else {
        VkClearColorValue zero{};
        vkCmdClearColorImage(c, t->image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    report_.render_targets++;
    if (options_.verbose)
        std::fprintf(stderr, "  render target: %s base %u pitch %u msaa %u format %u -> %ux%u\n", depth ? "depth" : "color",
                     base, pitch, msaa, format, t->width, t->height);
    return (targets_[key] = std::move(t)).get();
}

// ---- EDRAM: tiles change hands between render targets ----

// The target takes over the tiles of its first `rows` pixel rows: those
// another target holds are stored into EDRAM by their holder, then all it
// did not hold are loaded from EDRAM, as Xenia's ownership transfers do.
void Impl::acquire(RenderTarget *t, uint32_t rows) {
    uint32_t tile_rows = std::min((rows + t->tile_height - 1) / t->tile_height, t->height / t->tile_height);
    uint32_t count = std::min(tile_rows * t->pitch_tiles, EDRAM_TILES);
    std::vector<uint32_t> needed;
    std::map<RenderTarget *, std::vector<uint32_t>> held;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t tile = (t->base + k) % EDRAM_TILES;
        if (owners_[tile] == t) continue;
        needed.push_back(tile);
        if (owners_[tile]) held[owners_[tile]].push_back(tile);
    }
    if (needed.empty()) return;
    for (auto &[holder, tiles] : held)
        if (!edram_store(holder, tiles)) return;
    if (!edram_load(t, needed, tile_rows * t->tile_height)) return;
    for (uint32_t tile : needed) owners_[tile] = t;
    if (options_.verbose)
        std::fprintf(stderr, "  EDRAM: %zu tiles to %s base %u (from %zu targets)\n", needed.size(), t->depth ? "depth" : "color",
                     t->base, held.size());
}

// the tiles a transfer moves, a word each, in the arena
VkDeviceSize Impl::edram_mask(const std::vector<uint32_t> &tiles) {
    VkDeviceSize at = arena(EDRAM_TILES * 4, ssbo_align_);
    if (at == NONE) {
        fatal_ = "no room in the arena for an EDRAM transfer";
        return NONE;
    }
    uint32_t *mask = reinterpret_cast<uint32_t *>(arena_data(at));
    std::fill(mask, mask + EDRAM_TILES, 0u);
    for (uint32_t tile : tiles) mask[tile] = 1;
    return at;
}

bool Impl::edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles) {
    uint32_t last_row = 0;
    for (uint32_t tile : tiles) last_row = std::max(last_row, ((tile + EDRAM_TILES - t->base) % EDRAM_TILES) / t->pitch_tiles);
    uint32_t height = std::min((last_row + 1) * t->tile_height, t->height);
    VkDeviceSize mask = edram_mask(tiles);
    if (mask == NONE) return false;
    EdramTarget push = edram_target(*t, height);
    uint32_t offset = uint32_t(mask);
    VkCommandBuffer c = outside();
    if (options_.gpu_timing) gpu_mark("EDRAM store: " + target_name(*t));
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipelines_[t->msaa ? 1 : 0]);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, edram_layouts_[0], 0, 1, &t->store_set, 1, &offset);
    vkCmdPushConstants(c, edram_layouts_[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(c, (t->width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

// EDRAM as it stands, for a trace's start: every tile a render target holds
// stored into EDRAM by its holder (which keeps holding it), then the whole
// buffer read back, waited for.
bool Impl::snapshot_edram(std::vector<uint8_t> &out) {
    Call call(*this, nullptr);
    if (!call) return false;
    std::map<RenderTarget *, std::vector<uint32_t>> held;
    for (uint32_t tile = 0; tile < EDRAM_TILES; tile++)
        if (owners_[tile]) held[owners_[tile]].push_back(tile);
    for (auto &[holder, tiles] : held)
        if (!edram_store(holder, tiles)) return false;
    VkBuffer back;
    VkDeviceSize at;
    uint8_t *data = readback(EDRAM_BYTES, back, at);
    if (!data) return false;
    VkBufferCopy copy{0, at, EDRAM_BYTES};
    vkCmdCopyBuffer(outside(), edram_.buffer, back, 1, &copy);
    sync();
    if (!fatal_.empty()) return false;
    out.assign(data, data + EDRAM_BYTES);
    return true;
}

// The pages the GPU holds (what resolves and memory exports wrote and the
// CPU has not written since), read back from the mirror, runs of pages
// together: what a trace records as it begins (Xenia's trace downloads the
// ranges its GPU wrote, PrepareForTraceDownload)
void Impl::gpu_memory(std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &out) {
    out.clear();
    Call call(*this, nullptr);
    if (!call) return;
    std::vector<Range> runs;
    for (uint32_t page = 0; page < uint32_t(mirror_at_.size()); page++) {
        if ((page & 63) == 0 && !gpu_pages_[page >> 6].load(std::memory_order_relaxed)) {
            page += 63;
            continue;
        }
        if (!gpu_held(page)) continue;
        uint32_t start = page << page_shift_, end = (page + 1) << page_shift_;
        if (!runs.empty() && runs.back().second == start)
            runs.back().second = end;
        else
            runs.push_back({start, end});
    }
    VkDeviceSize total = 0;
    for (auto [start, end] : runs) total += end - start;
    if (!total) return;
    VkBuffer back;
    VkDeviceSize at;
    uint8_t *data = readback(total, back, at);
    if (!data) return;
    std::vector<VkBufferCopy> copies;
    VkDeviceSize offset = 0;
    for (auto [start, end] : runs) {
        copies.push_back({start, at + offset, end - start});
        offset += end - start;
    }
    vkCmdCopyBuffer(outside(), mirror_.buffer, back, uint32_t(copies.size()), copies.data());
    sync();
    if (!fatal_.empty()) return;
    offset = 0;
    for (auto [start, end] : runs) {
        out.push_back({start, std::vector<uint8_t>(data + offset, data + offset + (end - start))});
        offset += end - start;
    }
}

VkPipeline Impl::load_pipeline(const RenderTarget &t) {
    auto key = std::make_pair(t.image.format, t.msaa);
    auto it = load_pipelines_.find(key);
    if (it != load_pipelines_.end()) return it->second;
    VkShaderModule fs = !t.depth ? load_fs_[0] : load_fs_[2] ? load_fs_[2] : load_fs_[1];
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, fullscreen_vs_, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = t.image.samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (t.depth) {
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depth.stencilTestEnable = VK_TRUE;
        depth.front = depth.back = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                                    VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = t.depth ? 0 : 1;
    blend_state.pAttachments = &blend;
    // depth: the stencil a bit per pass without stencil export
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = t.depth ? 5 : 2;
    dynamic.pDynamicStates = dynamic_states;
    VkFormat color_format = t.image.format;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (t.depth) {
        rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = t.image.format;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &color_format;
    }
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = edram_layouts_[1];
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return load_pipelines_[key] = p;
}

bool Impl::edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows) {
    VkPipeline pipeline = load_pipeline(*t);
    if (!pipeline) return fatal_ = "could not make an EDRAM load pipeline", false;
    VkDeviceSize mask = edram_mask(tiles);
    if (mask == NONE) return false;
    uint32_t height = std::min(rows, t->height), offset = uint32_t(mask);
    EdramTarget push = edram_target(*t, height);
    VkCommandBuffer c = outside();
    if (options_.gpu_timing) gpu_mark("EDRAM load: " + target_name(*t));
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = t->view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {t->width, height}};
    rendering.layerCount = 1;
    if (t->depth) {
        rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    }
    vkCmdBeginRendering(c, &rendering);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = VK_NULL_HANDLE; // the draws' pipeline and sets are bound again after it
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, edram_layouts_[1], 0, 1, &edram_load_set_, 1, &offset);
    vkCmdPushConstants(c, edram_layouts_[1], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkViewport viewport{0, 0, float(t->width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {t->width, height}};
    vkCmdSetViewport(c, 0, 1, &viewport);
    vkCmdSetScissor(c, 0, 1, &scissor);
    if (!t->depth) {
        vkCmdDraw(c, 3, 1, 0, 0);
    } else {
        // the depth and the stencil (exported, or cleared to 0 for the
        // bit passes that follow)
        vkCmdSetDepthWriteEnable(c, VK_TRUE);
        vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdDraw(c, 3, 1, 0, 0);
        if (!load_fs_[2]) {
            vkCmdSetDepthWriteEnable(c, VK_FALSE);
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            for (uint32_t bit = 0; bit < 8; bit++) {
                EdramTarget pass = push;
                pass.stencil_bit = bit;
                vkCmdPushConstants(c, edram_layouts_[1], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass), &pass);
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                vkCmdDraw(c, 3, 1, 0, 0);
            }
        }
    }
    vkCmdEndRendering(c);
    return true;
}

// ---- textures ----

// A sampler's place in the bindless table, made once per what it is made
// of, as Xenia's Vulkan texture cache makes them (GetSamplerParameters,
// UseSampler, NormalizeClampMode): the clamps (the halfway one as to the
// edge, the mirrored clamps all as mirror clamp to edge, or mirrored repeat
// without it); the border color when a clamp uses it (white, else
// transparent black); the filters, the instruction's over the constant's;
// anisotropy up to the device's; the constant's lowest mip as the lowest
// LOD (level 0 for a base map with a base), a base map clamped there. LOD
// biases are the shader's.
uint32_t Impl::sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides) {
    uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3, aniso = (fetch[3] >> 25) & 7;
    if (overrides[0] != 3) mag = overrides[0];
    if (overrides[1] != 3) min = overrides[1];
    if (overrides[2] != 3) mip = overrides[2];
    if (overrides[3] != 7) aniso = overrides[3];
    texture::Info parsed = texture::parse_fetch_constant(fetch);
    // points where the host does not filter the formats it reads (Xenia's
    // GetSamplerParameters; anisotropy filters linearly too)
    if ((mag == 1 || min == 1 || mip == 1 || (aniso && aniso < 6)) && !texture::filterable(parsed, support_))
        mag = min = 0, mip = mip == 2 ? 2 : 0, aniso = 0;
    texture::Levels levels = texture::levels(parsed);
    uint32_t mip_min = mip == 2 && levels.base ? 0 : levels.mip_min;
    uint32_t clamp[3];
    bool border = false;
    for (uint32_t k = 0; k < 3; k++) {
        clamp[k] = (fetch[0] >> (10 + 3 * k)) & 7;
        if (clamp[k] == 4) clamp[k] = 2;
        if (clamp[k] == 3 || clamp[k] == 5 || clamp[k] == 7) clamp[k] = d_.mirror_clamp_to_edge() ? 3 : 1;
        border |= clamp[k] == 6;
    }
    uint32_t border_color = border ? fetch[5] & 3 : 0;
    if (border_color >= 2) approximate("YCbCr border colors (transparent black)");
    if (d_.max_anisotropy() < 1.0f) aniso = 0;
    std::array<uint32_t, 8> key = {clamp[0] | clamp[1] << 3 | clamp[2] << 6 | border_color << 9,
                                   mag, min, mip, aniso, mip_min, 0, 0};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second.second;
    if (next_sampler_ >= sampler_capacity_) {
        approximate("samplers past the bindless table's size (the stand-in's used)");
        return 0;
    }
    static const VkSamplerAddressMode modes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.addressModeU = modes[clamp[0]];
    info.addressModeV = modes[clamp[1]];
    info.addressModeW = modes[clamp[2]];
    info.magFilter = mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (aniso && aniso < 6) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(float(1u << (aniso - 1)), d_.max_anisotropy());
    }
    info.minLod = float(mip_min);
    info.maxLod = mip == 2 ? info.minLod + 0.25f : VK_LOD_CLAMP_NONE; // 2: the base map alone
    info.borderColor = border_color == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &info, nullptr, &s);
    uint32_t index = next_sampler_++;
    VkDescriptorImageInfo image{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_;
    write.dstBinding = 3;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    samplers_[key] = {s, index};
    return index;
}

// A texture by its fetch constant, loaded again when the memory it came from
// was written since (its pages' watches, as Xenia's texture cache drops a
// texture when a watch on its pages fires; resolves and memory exports over
// it mark it at once): from guest memory on the CPU, or from the mirror on
// the GPU when the GPU holds some of it. Loaded again, it keeps its image and
// its place in the table.
Texture *Impl::texture(const uint32_t fetch[6], std::string &why) {
    std::array<uint32_t, 6> key;
    std::copy(fetch, fetch + 6, key.begin());
    auto it = textures_.find(key);
    Texture *t = it == textures_.end() ? nullptr : it->second.get();
    if (t && !t->stale) {
        bool same;
        {
            Timer timer(report_.check_seconds);
            same = unwritten(*t);
        }
        if (same) {
            // a trace gets what guest memory gave it (a replay makes the
            // GPU's parts with its own resolves and exports)
            traced(t->gpu ? cpu_parts(t->ranges) : t->ranges);
            return t;
        }
    }
    if (t) t->stale = true; // until it is loaded again

    texture::Info info = texture::parse_fetch_constant(fetch);
    texture::Plan plan, signed_plan;
    if (!texture::plan(info, plan, why, support_)) return nullptr;
    // signed components: read through a snorm view of the same bits (Xenia's
    // signed host format; float formats are signed either way), or an image
    // of their own where the signed bits differ (10_11_11 and 11_11_10, 16-bit
    // snorm as floats); biased and gamma components, and the exponent adjust,
    // are the shader's (texture_signs)
    bool any_signed = info.signs[0] == 1 || info.signs[1] == 1 || info.signs[2] == 1 || info.signs[3] == 1;
    bool separate = any_signed && plan.signed_read == texture::Signed::Separate;
    if (separate && !texture::plan(info, signed_plan, why, support_, true)) return nullptr;
    if (!plan.approximate.empty()) approximate("a texture format held with fewer bits: " + plan.approximate);
    if (separate && !signed_plan.approximate.empty())
        approximate("a texture format held with fewer bits: " + signed_plan.approximate);
    std::vector<Range> ranges = ranges_of(plan.footprint);
    VkFormat format = vk_format_of(plan.format); // support_ has it sampled
    auto is_float = [](VkFormat f) {
        return f == VK_FORMAT_R16_SFLOAT || f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R16G16B16A16_SFLOAT ||
               f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT;
    };
    VkFormat signed_format = VK_FORMAT_UNDEFINED;
    if (any_signed && plan.signed_read == texture::Signed::None)
        approximate("signed texture components read as unsigned (no signed version of the format)");
    else if (any_signed && plan.signed_read == texture::Signed::View && !is_float(format))
        signed_format = snorm_of(format);
    else if (separate)
        signed_format =
            signed_plan.snorm ? snorm_of(vk_format_of(signed_plan.format)) : vk_format_of(signed_plan.format);
    // over pages the GPU holds (a resolve's, a memory export's): loaded on the
    // GPU from the mirror; the DXT formats a host without BC holds
    // decompressed are decompressed on the GPU too, from guest memory put into
    // the mirror
    bool mirror = gpu_held(ranges);
    bool gpu = mirror || (texture::is_compressed(info.host) && !texture::is_compressed(plan.format));
    // the write count before anything is watched or read: a write after it
    // that the load may have missed numbers a page above it
    uint32_t checked = mem_.write_count();

    texture::Image image, signed_image;
    if (!gpu) {
        Timer decoding(report_.decode_seconds);
        // decoded from guest memory, watched first
        for (auto [start, end] : ranges) mem_.watch(start, end - start);
        traced(ranges);
        if (!texture::decode(info, memory_, image, why, support_)) return nullptr;
        if (separate && !texture::decode(info, memory_, signed_image, why, support_, true)) return nullptr;
    } else if (!mirror) {
        traced(ranges);
    }

    // 1D, 2D and stacked as 2D arrays (stacked: a layer a slice), as Xenia
    // binds them
    bool cube = info.dimension == 3, three = info.dimension == 2;
    uint32_t layers = three ? 1 : std::max(1u, plan.depth);
    VkExtent3D extent{plan.width, plan.height, three ? plan.depth : 1};
    uint32_t level_count = plan.level_count;
    bool fresh = !t || t->image.format != format || t->image.extent.width != extent.width ||
                 t->image.extent.height != extent.height || t->image.extent.depth != extent.depth ||
                 t->image.levels != level_count || t->image.layers != layers ||
                 separate != (t->signed_image.image != VK_NULL_HANDLE) ||
                 (signed_format != VK_FORMAT_UNDEFINED) != (t->signed_index != 0);
    if (fresh) {
        uint32_t kind = three ? 1 : cube ? 2 : 0;
        uint32_t needed = signed_format != VK_FORMAT_UNDEFINED ? 2 : 1;
        if (next_index_[kind] + needed > capacity_[kind]) {
            why = "the bindless table is full";
            return nullptr;
        }
        uint32_t index = next_index_[kind]++;
        uint32_t signed_index = needed == 2 ? next_index_[kind]++ : 0;
        if (t) {
            // the same constant made another image (not with the same format
            // decisions; kept safe): the old one goes once unused, its place
            // in the table with it (work under way may still read it there)
            vk::Image old = t->image, old_signed_image = t->signed_image;
            VkImageView old_view = t->view, old_signed = t->signed_view;
            cmd();
            slots_[current_].after.push_back([this, old, old_signed_image, old_view, old_signed]() mutable {
                vkDestroyImageView(dev_, old_view, nullptr);
                if (old_signed) vkDestroyImageView(dev_, old_signed, nullptr);
                d_.destroy(old);
                if (old_signed_image.image) d_.destroy(old_signed_image);
            });
            t->image = {}, t->signed_image = {}, t->view = VK_NULL_HANDLE, t->signed_view = VK_NULL_HANDLE;
            t->signed_index = 0;
        } else {
            auto owned = std::make_unique<Texture>();
            t = owned.get();
            t->stale = true;
            textures_[key] = std::move(owned);
        }
        t->dimension = info.dimension, t->kind = kind, t->index = index;
        t->host = plan.format, t->host_swizzle = plan.host_swizzle;
        // a source of copies too: a front buffer read back, or copied by a presenter
        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const VkImageCreateFlags flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        t->image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, format, extent, level_count, layers,
                                   VK_SAMPLE_COUNT_1_BIT, usage,
                                   flags | (signed_index && !separate ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0));
        if (separate)
            t->signed_image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, signed_format, extent,
                                              level_count, layers, VK_SAMPLE_COUNT_1_BIT, usage, flags);
        if (!t->image.image || (separate && !t->signed_image.image)) {
            why = "could not make the image";
            return nullptr;
        }
        // the guest's swizzle over the host format's
        uint32_t swizzle = texture::compose_swizzle(info.swizzle, plan.host_swizzle);
        VkComponentMapping mapping{swizzle_of(swizzle & 7), swizzle_of((swizzle >> 3) & 7),
                                   swizzle_of((swizzle >> 6) & 7), swizzle_of((swizzle >> 9) & 7)};
        VkImageViewType view_type = cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                    : three ? VK_IMAGE_VIEW_TYPE_3D
                                            : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        t->view = make_view(t->image.image, view_type, format, VK_IMAGE_ASPECT_COLOR_BIT, mapping, level_count, layers,
                            false);
        if (!t->view) {
            why = "could not make the image's view";
            return nullptr;
        }
        if (signed_index) {
            t->signed_view = make_view(separate ? t->signed_image.image : t->image.image, view_type, signed_format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, mapping, level_count, layers, false);
            if (!t->signed_view) {
                why = "could not make the image's signed view";
                return nullptr;
            }
            t->signed_index = signed_index;
        }
        VkDescriptorImageInfo descriptors[2] = {{VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL},
                                                {VK_NULL_HANDLE, t->signed_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t w = 0; w < 2; w++) {
            writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[w].dstSet = bindless_;
            writes[w].dstBinding = kind;
            writes[w].dstArrayElement = w ? signed_index : index;
            writes[w].descriptorCount = 1;
            writes[w].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[w].pImageInfo = &descriptors[w];
        }
        vkUpdateDescriptorSets(dev_, signed_index ? 2 : 1, writes, 0, nullptr);
    }

    // what it was loaded from, for seeing when that changes
    t->ranges = std::move(ranges);
    t->low = UINT32_MAX, t->high = 0;
    for (auto [start, end] : t->ranges) t->low = std::min(t->low, start), t->high = std::max(t->high, end);
    if (gpu) {
        if (!gpu_load(*t, t->image, plan, fresh, true)) return nullptr;
        if (separate && !gpu_load(*t, t->signed_image, signed_plan, fresh, false)) return nullptr;
    } else {
        if (!cpu_upload(t->image, image, fresh)) return nullptr;
        if (separate && !cpu_upload(t->signed_image, signed_image, fresh)) return nullptr;
    }
    t->checked = checked;
    t->gpu = mirror;
    t->stale = false;
    report_.textures++;
    if (options_.verbose)
        std::fprintf(stderr, "  texture %08X: format %s %ux%u, signs %u%u%u%u, exponent %d, from %s as %s%s\n",
                     info.base_address, format_name(info.format), plan.width, plan.height, info.signs[0],
                     info.signs[1], info.signs[2], info.signs[3], info.exp_adjust,
                     mirror ? "the mirror" : gpu ? "guest memory on the GPU" : "guest memory",
                     texture::host_format_name(plan.format), separate ? " and a signed image" : "");
    return t;
}

// The decoded levels into an image, through the arena, as Xenia uploads
// them: each level's slices tightly packed, at 16-byte aligned offsets.
bool Impl::cpu_upload(const vk::Image &target, const texture::Image &image, bool fresh) {
    Timer decoding(report_.decode_seconds);
    VkExtent3D extent = target.extent;
    uint32_t layers = target.layers, level_count = target.levels;
    bool compressed = texture::is_compressed(image.format);
    uint32_t block_size = texture::bytes_per_block(image.format);
    const std::vector<std::vector<uint8_t>> &levels = image.levels;
    std::vector<VkBufferImageCopy> copies;
    std::vector<std::pair<uint32_t, VkDeviceSize>> sources; // level, bytes
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < level_count && l < levels.size(); l++) {
        VkExtent3D e{std::max(1u, extent.width >> l), std::max(1u, extent.height >> l), std::max(1u, extent.depth >> l)};
        uint64_t need = compressed ? uint64_t((e.width + 3) / 4) * ((e.height + 3) / 4) : uint64_t(e.width) * e.height;
        need *= uint64_t(block_size) * e.depth * layers;
        if (levels[l].size() < need) {
            // no base level (mips alone), or less than the level holds: left as it was
            if (!levels[l].empty()) approximate("texture levels shorter than their size");
            continue;
        }
        VkBufferImageCopy c{};
        c.bufferOffset = total;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers};
        c.imageExtent = e;
        copies.push_back(c);
        sources.push_back({l, need});
        total += (need + 15) & ~uint64_t(15);
    }
    VkBuffer from = VK_NULL_HANDLE;
    VkDeviceSize at = 0;
    uint8_t *data = total ? staging(total, from, at) : nullptr;
    if (total && !data) return false;
    for (size_t i = 0; i < copies.size(); i++) {
        std::memcpy(data + copies[i].bufferOffset, levels[sources[i].first].data(), sources[i].second);
        copies[i].bufferOffset += at;
    }
    VkCommandBuffer c = outside();
    gpu_mark("textures: uploads of CPU decodes");
    if (fresh) begin_image(c, target.image, VK_IMAGE_ASPECT_COLOR_BIT, level_count, layers);
    if (!copies.empty())
        vkCmdCopyBufferToImage(c, from, target.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()), copies.data());
    return true;
}

// A texture's version loaded on the GPU (texture_load.comp) from the mirror,
// level by level as the plan lays it out, then copied into the image as
// cpu_upload copies: what of its memory guest memory holds the newest of goes
// into the mirror first (upload: once for its versions).
bool Impl::gpu_load(Texture &t, const vk::Image &image, const texture::Plan &plan, bool fresh, bool upload) {
    std::vector<texture::GpuLevel> loads;
    uint64_t total = 0;
    texture::gpu_levels(plan, loads, total);
    std::vector<uint32_t> level_of; // each load's level
    for (size_t i = 0; i < plan.slices.size(); i++)
        if (!i || plan.slices[i].level != plan.slices[i - 1].level) level_of.push_back(plan.slices[i].level);
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!grow(load_scratch_, std::max<VkDeviceSize>(total, 16), usage, load_set_, 1)) return false;
    if (upload) request(t.ranges);
    VkCommandBuffer c = outside();
    gpu_mark("textures: loads on the GPU (texture_load)");
    if (fresh) begin_image(c, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.levels, image.layers);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1, &load_set_, 0, nullptr);
    std::vector<VkBufferImageCopy> copies;
    for (size_t i = 0; i < loads.size(); i++) {
        const LoadConstants &p = loads[i];
        if (!p.words) continue;
        uint32_t groups = (p.words + 63) / 64, across = p.row_words / 64;
        vkCmdPushConstants(c, load_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(c, across, (groups + across - 1) / across, 1);
        VkBufferImageCopy copy{};
        copy.bufferOffset = VkDeviceSize(p.out_word) * 4;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level_of[i], 0, image.layers};
        copy.imageExtent = {std::max(1u, image.extent.width >> level_of[i]),
                            std::max(1u, image.extent.height >> level_of[i]),
                            std::max(1u, image.extent.depth >> level_of[i])};
        copies.push_back(copy);
    }
    full_barrier(c);
    if (!copies.empty())
        vkCmdCopyBufferToImage(c, load_scratch_.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()),
                               copies.data());
    if (options_.verbose)
        std::fprintf(stderr, "  texture at %08X loaded on the GPU (%u levels)\n",
                     plan.slices.empty() ? 0 : plan.slices[0].base, uint32_t(loads.size()));
    return true;
}

// ---- pipelines ----

VkPipeline Impl::pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info, bool async,
                          bool &pending) {
    pending = false;
    async = async && pipeline_workers_;
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        PipelineEntry &e = *it->second;
        if (e.state.load(std::memory_order_acquire)) return e.pipeline;
        // being made on a worker: waited for, unless the draw may go without it
        if (async) return pending = true, VK_NULL_HANDLE;
        Timer timer(report_.pipeline_seconds);
        pipeline_workers_->wait(&e);
        return e.pipeline;
    }
    PipelineEntry &e = *(pipelines_[key] = std::make_unique<PipelineEntry>());
    report_.pipelines++;
    last_pipeline_swap_ = report_.swaps;
    if (async) {
        pipeline_workers_->queue(&e, std::make_unique<PipelineDesc>(info));
        return pending = true, VK_NULL_HANDLE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    {
        Timer timer(report_.pipeline_seconds);
        if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    e.pipeline = p;
    e.state.store(1, std::memory_order_release);
    return p;
}

// ---- a draw ----

struct Viewport {
    VkViewport viewport{};
    float ndc_scale[3] = {1, 1, 1}, ndc_offset[3] = {};
    bool empty = false;
};

// draw_util.cc's GetHostViewportInfo, at resolution scale 1, for Vulkan
// (origin at the top left, reverse Z allowed, float24 depth halved);
// depth_float24: a D24FS8 target with the depth test on
Viewport host_viewport(const uint32_t *regs, float max_x, float max_y, bool pixel_writes_depth, bool depth_float24) {
    Viewport v;
    uint32_t clip = regs[r::PA_CL_CLIP_CNTL], vte = regs[r::PA_CL_VTE_CNTL], mode = regs[r::PA_SU_SC_MODE_CNTL];
    uint32_t vtx = regs[r::PA_SU_VTX_CNTL];
    const float *vport = reinterpret_cast<const float *>(&regs[r::PA_CL_VPORT_XSCALE]);
    float scale_xy[2] = {vte & 1 ? vport[0] : 1.0f, vte & 4 ? vport[2] : 1.0f};
    float scale_z = vte & 16 ? vport[4] : 1.0f;
    float offset_base_xy[2] = {vte & 2 ? vport[1] : 0.0f, vte & 8 ? vport[3] : 0.0f};
    float offset_z = vte & 32 ? vport[5] : 0.0f;
    float offset_add_xy[2] = {};
    if (mode & (1u << 16)) {
        uint32_t window = regs[r::PA_SC_WINDOW_OFFSET];
        offset_add_xy[0] += float(sign_extend(window & 0x7FFF, 15));
        offset_add_xy[1] += float(sign_extend((window >> 16) & 0x7FFF, 15));
    }
    if (!(vtx & 1)) offset_add_xy[0] += 0.5f, offset_add_xy[1] += 0.5f; // Direct3D 9's pixel centers
    float xy_max[2] = {max_x, max_y};
    float xy_offset[2], xy_extent[2], z_min, z_max;
    if (clip & (1u << 16)) {
        // clipping off: a huge viewport, the offsets in the vertex shader
        for (int i = 0; i < 2; i++) {
            float extent = std::min(8192.0f, xy_max[i]);
            xy_offset[i] = 0;
            xy_extent[i] = extent;
            float to_ndc = 2.0f / extent;
            v.ndc_scale[i] = scale_xy[i] * to_ndc;
            v.ndc_offset[i] = (offset_base_xy[i] - extent * 0.5f + offset_add_xy[i]) * to_ndc;
        }
        z_min = 0.0f, z_max = 1.0f;
        v.ndc_scale[2] = scale_z, v.ndc_offset[2] = offset_z;
    } else {
        for (int i = 0; i < 2; i++) {
            float offset = offset_base_xy[i] + offset_add_xy[i], scale_abs = std::abs(scale_xy[i]);
            int32_t axis_1 = int32_t(std::floor(std::clamp(offset + scale_abs, 0.0f, xy_max[i])));
            int32_t axis_0 = int32_t(std::floor(std::clamp(offset - scale_abs, float(axis_1) - xy_max[i], xy_max[i])));
            uint32_t extent = axis_1 > 0 ? uint32_t(axis_1 - axis_0) : 0;
            xy_offset[i] = float(axis_0);
            xy_extent[i] = float(extent);
            if (extent) {
                float inv = 1.0f / float(extent);
                v.ndc_scale[i] = scale_xy[i] * 2.0f * inv;
                v.ndc_offset[i] = (offset - (float(axis_0) + float(extent) * 0.5f)) * 2.0f * inv;
            } else {
                v.empty = true;
            }
        }
        float host_offset_z, host_scale_z;
        if (clip & (1u << 19)) {
            host_offset_z = offset_z, host_scale_z = scale_z;
            v.ndc_scale[2] = 1.0f, v.ndc_offset[2] = 0.0f;
        } else {
            host_offset_z = offset_z - scale_z, host_scale_z = scale_z * 2.0f;
            v.ndc_scale[2] = 0.5f, v.ndc_offset[2] = 0.5f;
        }
        if (pixel_writes_depth) {
            z_min = 0.0f, z_max = 1.0f;
        } else {
            z_min = std::clamp(host_offset_z, 0.0f, 1.0f);
            z_max = std::clamp(host_offset_z + host_scale_z, 0.0f, 1.0f);
        }
    }
    if (depth_float24) {
        // D24FS8's 0-2 kept as 0-1 (Xenia's full_float24_in_0_to_1)
        z_min *= 0.5f;
        z_max *= 0.5f;
    }
    v.viewport = {xy_offset[0], xy_offset[1], xy_extent[0], xy_extent[1], z_min, z_max};
    return v;
}

// draw_util.cc's GetScissor, clamped to the surface pitch, the window offset not in EDRAM
VkRect2D host_scissor(const uint32_t *regs, uint32_t pitch) {
    uint32_t tl = regs[r::PA_SC_WINDOW_SCISSOR_TL], br = regs[r::PA_SC_WINDOW_SCISSOR_BR];
    int32_t tl_x = tl & 0x3FFF, tl_y = (tl >> 16) & 0x3FFF, br_x = br & 0x3FFF, br_y = (br >> 16) & 0x3FFF;
    if (!(tl >> 31)) {
        uint32_t window = regs[r::PA_SC_WINDOW_OFFSET];
        int32_t wx = sign_extend(window & 0x7FFF, 15), wy = sign_extend((window >> 16) & 0x7FFF, 15);
        tl_x += wx, tl_y += wy, br_x += wx, br_y += wy;
    }
    uint32_t stl = regs[r::PA_SC_SCREEN_SCISSOR_TL], sbr = regs[r::PA_SC_SCREEN_SCISSOR_BR];
    tl_x = std::max(tl_x, sign_extend(stl & 0x7FFF, 15));
    tl_y = std::max(tl_y, sign_extend((stl >> 16) & 0x7FFF, 15));
    br_x = std::min(br_x, sign_extend(sbr & 0x7FFF, 15));
    br_y = std::min(br_y, sign_extend((sbr >> 16) & 0x7FFF, 15));
    tl_x = std::min(tl_x, int32_t(pitch));
    br_x = std::min(br_x, int32_t(pitch));
    tl_x = std::max(tl_x, 0), tl_y = std::max(tl_y, 0);
    br_x = std::max(br_x, tl_x), br_y = std::max(br_y, tl_y);
    return {{tl_x, tl_y}, {uint32_t(br_x - tl_x), uint32_t(br_y - tl_y)}};
}

// ui/graphics_util.cc's FloatToD3D11Fixed16p8: a float as 16.8 fixed point,
// rounded to nearest even, as Direct3D 11 converts it
int32_t float_to_fixed_16p8(float f32) {
    if (!(std::abs(f32) >= 1.0f / 512.0f)) return 0;
    if (f32 >= 32768.0f - 1.0f / 256.0f) return (1 << 23) - 1;
    if (f32 <= -32768.0f) return -32768 * 256;
    uint32_t bits = std::bit_cast<uint32_t>(f32);
    int32_t result = int32_t((bits & ((1u << 23) - 1)) | (1u << 23));
    if (bits >> 31) result = -result;
    uint32_t extra_bits = uint32_t(15 - (int32_t((bits >> 23) & 255) - 127));
    if (extra_bits) {
        uint32_t u = uint32_t(result);
        u += (1u << (extra_bits - 1)) - 1 + ((u >> extra_bits) & 1);
        result = int32_t(u) >> extra_bits;
    }
    return result;
}

// draw_extent_estimator.cc's EstimateMaxY (the window offset not in EDRAM's
// bases): the rows of the render targets a draw can reach, which it takes
// over from the targets holding them. The scissor's bottom, then the
// viewport's with clipping on; with clipping off and no scissor (8192 both
// ways, as Direct3D 9 leaves it for clears and other screen-space
// rectangles), the lowest row the vertices reach, the vertex shader run on
// the CPU. Without it a 1280-wide target took over all of EDRAM at such a
// draw, every target there stored and loaded again (El Chavo Kart's race:
// its depth target took 2048 tiles three times a frame).
uint32_t Impl::draw_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                          uint32_t primitive) const {
    uint32_t window = regs_[r::PA_SC_WINDOW_OFFSET];
    int32_t window_x = sign_extend(window & 0x7FFF, 15), window_y = sign_extend((window >> 16) & 0x7FFF, 15);
    uint32_t tl = regs_[r::PA_SC_WINDOW_SCISSOR_TL], br = regs_[r::PA_SC_WINDOW_SCISSOR_BR];
    uint32_t screen_br = regs_[r::PA_SC_SCREEN_SCISSOR_BR];
    int32_t scissor_bottom = int32_t((br >> 16) & 0x3FFF), scissor_right = int32_t(br & 0x3FFF);
    int32_t screen_bottom = sign_extend((screen_br >> 16) & 0x7FFF, 15);
    int32_t screen_right = sign_extend(screen_br & 0x7FFF, 15);
    bool scissor_window_offset = !(tl >> 31);
    if (scissor_window_offset) scissor_bottom += window_y, scissor_right += window_x;
    scissor_bottom = std::min(scissor_bottom, screen_bottom);
    scissor_right = std::min(scissor_right, screen_right);
    uint32_t max_y = uint32_t(std::max(scissor_bottom, 0));
    if ((regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1) {
        // the vertices' extent, where there is no scissor (Xenia's
        // execute_unclipped_draw_vs_on_cpu, without _with_scissor)
        if (scissor_bottom >= 8192 && scissor_right >= 8192)
            max_y = std::min(max_y, vertex_max_y(vs, constants, indices, primitive));
    } else {
        // the viewport's bottom, truncated
        uint32_t vte = regs_[r::PA_CL_VTE_CNTL];
        const float *vport = reinterpret_cast<const float *>(&regs_[r::PA_CL_VPORT_XSCALE]);
        float bottom = 0.0f;
        if ((regs_[r::PA_SU_SC_MODE_CNTL] >> 16) & 1) bottom += float(window_y);
        if (!(regs_[r::PA_SU_VTX_CNTL] & 1)) bottom += 0.5f;
        if (vte & 8) bottom += vport[3];
        bottom += vte & 4 ? std::abs(vport[2]) : 1.0f;
        max_y = uint32_t(std::min(float(max_y), std::max(0.0f, bottom)));
    }
    return max_y;
}

// draw_extent_estimator.cc's EstimateVertexMaxY: the vertex shader run on the
// CPU (shader_interpreter.h) for each of the draw's vertices, the lowest row
// one reaches (a point's with its radius); 8192 when it cannot tell (a
// shader using what a texture fetch returns, which the interpreter does not
// sample, as Xenia's CanInterpretShader). Unlike Xenia, a draw of more than
// 1024 vertices is not run (8192): the CPU's time, for what a scissor
// usually bounds anyway.
uint32_t Impl::vertex_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                            uint32_t primitive) const {
    if (indices.empty()) return 0;
    if (vs.fetches_textures || !vs.writes_position_y) return 8192;
    uint32_t min_index = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
    std::vector<uint32_t> vertices;
    for (uint32_t index : indices)
        if (index != UINT32_MAX) vertices.push_back(std::min(max_index, std::max(min_index, index & 0xFFFFFF)));
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    if (vertices.size() > 1024) return 8192;
    uint32_t vte = regs_[r::PA_CL_VTE_CNTL];
    const float *vport = reinterpret_cast<const float *>(&regs_[r::PA_CL_VPORT_XSCALE]);
    float y_scale = vte & 4 ? vport[2] : 1.0f, y_offset = vte & 8 ? vport[3] : 0.0f;
    bool divide_by_w = !((vte >> 8) & 1);
    if (divide_by_w && !vs.writes_position_w) return 8192;
    int32_t point_min_diameter = 0, point_max_diameter = 0;
    float point_constant_radius_y = 0.0f;
    bool points = primitive == 1;
    if (points) {
        uint32_t minmax = regs_[r::PA_SU_POINT_MINMAX];
        point_min_diameter = std::bit_cast<int32_t>(float(minmax & 0xFFFF) * (2.0f / 16.0f));
        point_max_diameter = std::bit_cast<int32_t>(float(minmax >> 16) * (2.0f / 16.0f));
        point_constant_radius_y = float(regs_[r::PA_SU_POINT_SIZE] & 0xFFFF) * (1.0f / 16.0f);
    }
    std::span<const uint8_t> memory(memory_.data(), GuestMemory::SIZE);
    float max_y = -FLT_MAX;
    harness::CaseInput input{};
    harness::CaseOutput output;
    std::string unsupported;
    for (uint32_t vertex : vertices) {
        input.registers[0][0] = float(vertex);
        if (!shader::interpret(vs.parsed, input, constants, memory, output, unsupported)) return 8192;
        if (vs.writes_vertex_kill && (std::bit_cast<uint32_t>(output.point_size[2]) & ~(1u << 31))) continue;
        float y = output.position[1];
        if (divide_by_w) y /= output.position[3];
        y = y * y_scale + y_offset;
        if (points) {
            float radius = point_constant_radius_y;
            if (vs.writes_point_size)
                radius = 0.5f * std::bit_cast<float>(std::min(
                                    point_max_diameter,
                                    std::max(point_min_diameter, std::bit_cast<int32_t>(output.point_size[0]))));
            y += radius;
        }
        max_y = std::max(max_y, y); // NaN keeps max_y, as Xenia's std::max
    }
    int32_t max_y_24p8 = float_to_fixed_16p8(max_y);
    if (!(regs_[r::PA_SU_VTX_CNTL] & 1)) max_y_24p8 += 128; // Direct3D 9's pixel centers
    if ((regs_[r::PA_SU_SC_MODE_CNTL] >> 16) & 1)
        max_y_24p8 += sign_extend((regs_[r::PA_SC_WINDOW_OFFSET] >> 16) & 0x7FFF, 15) * 256;
    // the top-left rule: .5 exclusive without MSAA, 1. with
    uint32_t msaa = (regs_[reg::RB_SURFACE_INFO] >> 16) & 3;
    return (uint32_t(std::max(0, max_y_24p8)) + (msaa == 0 ? 127u : 255u)) >> 8;
}

// A draw outside copy mode, as the processor gives it (immediate indices,
// which Xenia does not draw either, never come: the processor counts them).
void Impl::draw(const gpu::Draw &d) {
    report_.draws++;
    uint32_t mode = regs_[reg::RB_MODECONTROL] & 7;
    if (EdramMode(mode) != EdramMode::ColorDepth && EdramMode(mode) != EdramMode::DepthOnly) return skip("EDRAM mode " + std::to_string(mode));

    uint32_t primitive = d.primitive, count = d.count;
    bool index32 = d.index32;
    if (count == 0) return;
    bool color = EdramMode(mode) == EdramMode::ColorDepth;
    // whether the draw can write any target (a color channel enabled, or depth or stencil on)
    bool writes_targets = (color && (regs_[reg::RB_COLOR_MASK] & 0xFFFF) != 0) || (regs_[reg::RB_DEPTHCONTROL] & 3) != 0;
    {
        // A draw that can write nothing (no color channel enabled, depth and
        // stencil off, no memory export) leaves EDRAM and memory as they
        // were, and the occlusion queries' counts are the processor's fake
        // ones (EVENT_WRITE_ZPD, as Xenia's): nothing to draw, whatever its
        // primitive. El Chavo Kart issues hundreds of such point lists a
        // frame.
        const Shader *v = have_vertex_ ? shader(vertex_hash_) : nullptr;
        const Shader *p = have_pixel_ ? shader(pixel_hash_) : nullptr;
        bool exports = (v && v->summary.memexport) || (color && p && p->summary.memexport);
        if (!writes_targets && !exports) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
    }
    if ((regs_[r::VGT_OUTPUT_PATH_CNTL] & 3) == 1) {
        // VGT_OUTPUT_PATH_CNTL's path_select: the tessellator, whose vertex
        // shader runs as a domain shader on the patches' points (Xenia's
        // HostVertexShaderType domain types); primitive types 16 to 18 are
        // then line, triangle and quad patches. Not done: the hull and domain
        // stages are not ported.
        static const char *const modes[] = {"discrete", "continuous", "adaptive", "mode 3"};
        static const char *const patches[] = {"line patch", "triangle patch", "quad patch"};
        const char *kind = primitive >= 16 && primitive <= 18 ? patches[primitive - 16] : primitive_name(primitive);
        return skip(std::string("tessellation: ") + modes[regs_[r::VGT_HOS_CNTL] & 3] + " " + kind);
    }
    bool rectangles = primitive == 8, points = primitive == 1;
    // rectangle and point lists are expanded by geometry shaders, which the
    // device may lack (the vertex shader expansion Xenia's D3D12 backend does
    // is not here yet)
    if ((rectangles || points) && !d_.geometry_shader())
        return skip(std::string(primitive_name(primitive)) + " without geometry shaders");
    // triangles, quads and polygons (draw_util.h's IsPrimitivePolygonal): faces,
    // culling, the front and back polygon offsets
    bool polygonal = (primitive >= 4 && primitive <= 7) || (primitive >= 13 && primitive <= 15);
    VkPrimitiveTopology topology;
    switch (primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break; // sprites made by point_list.geom
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: case 8: case 13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    // Line loops as strips closed by their first index (Xenia's
    // LineLoopToStrip). Quad strips and polygons, which Xenia does not draw,
    // are OpenGL's (Mesa's r600 and freedreno's a2xx give GL_QUAD_STRIP and
    // GL_POLYGON to these primitive types): a quad strip covers what the
    // triangle strip of its vertices covers, a whole number of quads, and a
    // convex polygon what the fan of its vertices does.
    case 12: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 14: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 15: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    // Not drawn, as Xenia's primitive processor does not: the triangle list
    // with wireframe flags (7), whose flags' place no reference gives, and
    // the 2D engine's copy and fill rectangles and strips (16 to 22).
    default: return skip(std::string("primitive ") + primitive_name(primitive));
    }
    // the types the reset index cuts (Xenia's ProcessIndexBuffer list)
    bool strip = primitive == 3 || primitive == 5 || primitive == 6 || (primitive >= 12 && primitive != 13);

    // shaders
    Shader *vs = have_vertex_ ? shader(vertex_hash_) : nullptr;
    if (!vs) return skip("no vertex shader");
    Shader *ps = color && have_pixel_ ? shader(pixel_hash_) : nullptr;
    std::string why;
    if (!translate(*vs, why)) return skip("vertex shader: " + why);
    if (ps && !translate(*ps, why)) return skip("pixel shader: " + why);

    // render targets
    uint32_t surface = regs_[reg::RB_SURFACE_INFO], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
    if (!pitch) return skip("surface pitch 0"); // Xenia draws nothing then, exports neither
    if (msaa == 3) return skip("MSAA mode 3");
    // Memory exports. A draw that rasterizes nothing (it writes no target,
    // or the vertex shader only exports: SQ_PROGRAM_CNTL's vs_export_mode
    // multipass; or no target, viewport or scissor is left) still runs its
    // vertex shader for them, as Xenia's does: alone, with rasterization
    // discarded, each index once (a point list).
    bool export_only = false;
    auto rasterizes_nothing = [&]() {
        if (ps && ps->exports) approximate("pixel shader memory export in a draw that rasterizes nothing (not done)");
        if (!vs->exports) return false;
        export_only = true;
        ps = nullptr;
        return true;
    };
    if (!writes_targets || ((regs_[reg::SQ_PROGRAM_CNTL] >> 24) & 7) == 7) {
        if (!rasterizes_nothing()) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
    }
    RenderTarget *colors[4] = {};
    uint32_t color_masks[4] = {};
    float color_scales[4] = {1, 1, 1, 1};
    uint32_t gamma_targets = 0;
    uint32_t mask = regs_[reg::RB_COLOR_MASK];
    for (uint32_t i = 0; i < 4 && ps; i++) {
        color_masks[i] = (mask >> (4 * i)) & 0xF;
        if (!color_masks[i] || !(ps->colors_written & (1u << i))) {
            color_masks[i] = 0;
            continue;
        }
        uint32_t info = regs_[r::COLOR_INFO[i]], format = (info >> 16) & 0xF;
        // the exponent bias, and 16-bit fixed (-32..32, 10 fraction bits) kept as snorm
        color_scales[i] = std::ldexp(1.0f, sign_extend((info >> 20) & 0x3F, 6));
        if (format == 4 || format == 5) color_scales[i] *= 1024.0f / 32767.0f;
        if (format == 1) gamma_targets |= 1u << i;
        colors[i] = render_target(false, info & 0xFFF, pitch, msaa, format);
        if (!colors[i]) return fatal_.empty() ? skip(std::string("color target format ") + color_target_format_name(format)) : void();
    }
    uint32_t depth_control = export_only ? 0 : regs_[reg::RB_DEPTHCONTROL];
    RenderTarget *depth = nullptr;
    if (depth_control & 3) {
        uint32_t info = regs_[reg::RB_DEPTH_INFO], format = (info >> 16) & 1;
        if (format == 1) approximate("D24FS8 depth as 32-bit float");
        depth = render_target(true, info & 0xFFF, pitch, msaa, format);
        if (!depth) return;
    } else {
        depth_control = 0;
    }
    uint32_t width = 8192, height = 8192;
    for (RenderTarget *t : colors)
        if (t) width = std::min(width, t->width), height = std::min(height, t->height);
    if (depth) width = std::min(width, depth->width), height = std::min(height, depth->height);
    if (width == 8192 && height == 8192 && !export_only && !rasterizes_nothing()) {
        // no target the pixel shader writes and no depth: nothing it could change
        if (options_.verbose)
            std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
        return;
    }

    // viewport and scissor
    bool depth_float24 = depth && depth->format == 1;
    Viewport viewport = host_viewport(regs_, max_viewport_[0], max_viewport_[1], ps && ps->writes_depth,
                                      depth_float24 && (depth_control & 2));
    VkRect2D scissor = host_scissor(regs_, pitch);
    scissor.extent.width = std::min<uint32_t>(scissor.extent.width, width - std::min<uint32_t>(scissor.offset.x, width));
    scissor.extent.height = std::min<uint32_t>(scissor.extent.height, height - std::min<uint32_t>(scissor.offset.y, height));
    if (!export_only && (viewport.empty || !scissor.extent.width || !scissor.extent.height) && !rasterizes_nothing()) return;
    if (export_only) {
        std::fill(std::begin(colors), std::end(colors), nullptr);
        depth = nullptr, depth_control = 0;
    }
    // (the targets take over their EDRAM tiles below, once the draw's lowest
    // row is known: draw_max_y)

    // the pages of the streams the shaders export to (the pixel shader's only
    // when it runs)
    std::vector<Range> exported;
    export_ranges(*vs, 0, exported);
    if (ps) export_ranges(*ps, 1, exported);
    bool exports = vs->exports || (ps && ps->exports);

    // indices: the guest's, swapped, plus the index offset (as Xenia's
    // vertex shader adds it), reset indices kept as the host's restart index
    std::vector<uint32_t> indices;
    indices.reserve(count);
    uint32_t offset = regs_[r::VGT_INDX_OFFSET];
    bool restart = strip && (regs_[r::PA_SU_SC_MODE_CNTL] >> 21) & 1;
    uint32_t reset_index = regs_[r::VGT_MULTI_PRIM_IB_RESET_INDX];
    auto vertex = [&](uint32_t index) { return index + offset; };
    if (d.indexed) {
        uint32_t size = index32 ? 4 : 2;
        gpu::Endian endian = d.index_endian;
        std::vector<uint8_t> raw_indices(size_t(count) * size);
        traced(d.index_base, uint32_t(raw_indices.size()));
        memory_.read(d.index_base, raw_indices);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *b = raw_indices.data() + size_t(i) * size;
            uint32_t raw = index32 ? uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24
                                   : uint32_t(b[0]) | uint32_t(b[1]) << 8;
            uint32_t index = index32 ? gpu::gpu_swap(raw, endian)
                                     : (endian == gpu::Endian::In16 ? ((raw >> 8) | (raw << 8)) & 0xFFFF : raw);
            if (!index32) index &= 0xFFFF;
            if (restart && index == (index32 ? reset_index : reset_index & 0xFFFF)) indices.push_back(UINT32_MAX);
            else indices.push_back(vertex(index));
        }
    } else {
        for (uint32_t i = 0; i < count; i++) indices.push_back(vertex(i));
    }
    if (export_only) {
        // each index a point, the reset indices left out
        std::erase(indices, UINT32_MAX);
        if (indices.empty()) return;
        topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        restart = rectangles = points = false;
    } else if (primitive == 13) {
        // quads to triangle pairs
        std::vector<uint32_t> triangles;
        for (uint32_t q = 0; q + 3 < indices.size(); q += 4)
            for (uint32_t k : {0, 1, 2, 0, 2, 3}) triangles.push_back(indices[q + k]);
        indices = std::move(triangles);
        if (indices.empty()) return;
    } else if (primitive == 12 || primitive == 14 || primitive == 15) {
        // each primitive between reset indices on its own: a line loop closed
        // by its first index (2 vertices give two lines, 1 nothing: Xenia's
        // GetLineLoopStripIndexCount); a quad strip's whole quads; a polygon
        // of at least a triangle
        std::vector<uint32_t> converted;
        converted.reserve(indices.size() + indices.size() / 2 + 1);
        for (size_t first = 0; first < indices.size();) {
            size_t end = first;
            while (end < indices.size() && indices[end] != UINT32_MAX) end++;
            size_t n = end - first;
            if (primitive == 14) n = n >= 4 ? n & ~size_t(1) : 0;
            if (primitive == 15 && n < 3) n = 0;
            if (primitive == 12 && n < 2) n = 0;
            if (n) {
                if (!converted.empty()) converted.push_back(UINT32_MAX);
                converted.insert(converted.end(), indices.begin() + first, indices.begin() + first + n);
                if (primitive == 12) converted.push_back(indices[first]);
            }
            first = end + 1;
        }
        indices = std::move(converted);
        if (indices.empty()) return;
    }

    // constants
    harness::Constants constants[2];
    for (int stage = 0; stage < 2; stage++) {
        harness::Constants &c = constants[stage];
        std::memcpy(c.float_constants, &regs_[reg::ALU_CONSTANTS + stage * 1024], 4096);
        std::memcpy(c.bool_constants, &regs_[reg::BOOL_CONSTANTS], sizeof(c.bool_constants));
        std::memcpy(c.loop_constants, &regs_[reg::LOOP_CONSTANTS], sizeof(c.loop_constants));
        std::memcpy(c.fetch_constants, &regs_[reg::FETCH_CONSTANTS], sizeof(c.fetch_constants));
    }

    // the targets take over the EDRAM tiles down to the lowest row the draw
    // can reach (draw_max_y), within the scissor
    {
        uint32_t rows =
            std::min(scissor.offset.y + scissor.extent.height, draw_max_y(*vs, constants[0], indices, primitive));
        for (RenderTarget *t : colors)
            if (t) acquire(t, rows);
        if (depth) acquire(depth, rows);
        if (!fatal_.empty()) return;
    }
    System sys{};
    for (int i = 0; i < 3; i++) sys.ndc_scale[i] = viewport.ndc_scale[i], sys.ndc_offset[i] = viewport.ndc_offset[i];
    sys.ndc_scale[3] = 1.0f;
    uint32_t color_control = regs_[r::RB_COLORCONTROL];
    if (color_control & 8) sys.flags[0] = 8 | (color_control & 7);
    sys.values[0] = as_float(regs_[r::RB_ALPHA_REF]);
    uint32_t vte = regs_[r::PA_CL_VTE_CNTL];
    sys.flags[1] = ((vte >> 10) & 1) | ((vte >> 8) & 1) << 1 | ((vte >> 9) & 1) << 2;
    // the pixel parameters (SQ_PROGRAM_CNTL's param_gen) into the
    // interpolator SQ_CONTEXT_MISC's param_gen_pos names, as Xenia's
    // GetInterpolatorInputMask allows them (within the 16 interpolators),
    // with the kind of primitive
    if (ps && (regs_[reg::SQ_PROGRAM_CNTL] >> 18) & 1) {
        uint32_t position = (regs_[r::SQ_CONTEXT_MISC] >> 8) & 0xFF;
        if (position < 16) {
            sys.flags[1] |= 1u << 8 | position << 9;
            if (points) {
                sys.flags[1] |= 1u << 13;
                approximate("point sprite coordinates in the pixel parameters (0)");
            } else if (polygonal) {
                sys.flags[1] |= 1u << 15;
            } else if (primitive == 2 || primitive == 3 || primitive == 12) {
                sys.flags[1] |= 1u << 14;
            }
        }
    }
    // alpha to coverage (RB_COLORCONTROL bit 4) with its dither offsets
    // (bits 24-31), as Xenia's system constant alpha_to_mask
    if (color_control & 16) sys.flags[0] |= (color_control >> 24) << 8 | 1u << 16;
    sys.flags[2] = gamma_targets;
    // oDepth halved, as the target keeps D24FS8's 0-2 (bit 0); the samples
    // (bits 1-2: 1x, 2x, 4x), for alpha to coverage
    sys.flags[3] = (depth_float24 ? 1u : 0u) | msaa << 1;
    std::copy(color_scales, color_scales + 4, sys.color_scales);
    if (points) {
        // as Xenia's UpdateSystemConstantValues: the registers' 12.4 radii
        // as diameters, and the viewport's pixels to NDC (2 / extent, halved
        // for the radius)
        uint32_t minmax = regs_[r::PA_SU_POINT_MINMAX], point_size = regs_[r::PA_SU_POINT_SIZE];
        sys.values[1] = float(minmax & 0xFFFF) * (2.0f / 16.0f);
        sys.values[2] = float(minmax >> 16) * (2.0f / 16.0f);
        sys.point[0] = float(point_size >> 16) * (2.0f / 16.0f);
        sys.point[1] = float(point_size & 0xFFFF) * (2.0f / 16.0f);
        sys.point[2] = 1.0f / float(std::max(uint32_t(viewport.viewport.width), 1u));
        sys.point[3] = 1.0f / float(std::max(uint32_t(viewport.viewport.height), 1u));
    }

    // textures: per slot the shaders read, an image's index in the table of
    // its dimension and a sampler's
    std::set<uint32_t> slots(vs->summary.texture_slots.begin(), vs->summary.texture_slots.end());
    if (ps) slots.insert(ps->summary.texture_slots.begin(), ps->summary.texture_slots.end());
    for (uint32_t slot : slots) {
        const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS + slot * 6];
        if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture) {
            approximate("a texture slot without a texture constant");
            continue;
        }
        std::string reason;
        Texture *t = texture(fetch, reason);
        if (!fatal_.empty()) return;
        if (!t) {
            approximate("texture not decoded: " + reason);
            continue;
        }
        std::array<uint32_t, 4> overrides = {3, 3, 3, 7};
        for (Shader *s : {vs, ps})
            if (s && s->filters.count(slot)) {
                overrides = s->filters[slot];
                break;
            }
        uint32_t sampler_bits = sampler(fetch, overrides) << 16;
        sys.slots[slot] = t->index | t->kind << 14 | sampler_bits;
        sys.signs[slot] = swizzled_signs(texture::parse_fetch_constant(fetch));
        if (t->signed_index) sys.signed_slots[slot] = t->signed_index | t->kind << 14 | sampler_bits;
    }

    // the pipeline
    VkFormat color_formats[4] = {};
    VkPipelineColorBlendAttachmentState blends[4] = {};
    // The key: what the pipeline cannot take at the draw. Vulkan 1.3's
    // extended dynamic state (1 and 2, core) gives the draw the topology
    // within its class (points, lines, triangles), primitive restart,
    // culling, the front face, the depth and stencil tests and ops and the
    // depth bias's enable, so those make no pipelines (Xenia's Vulkan backend
    // keys them all: vulkan_pipeline_cache.cc, a TODO in its command
    // processor).
    uint32_t topology_class = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0
                              : topology <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1
                                                                              : 2;
    std::vector<uint32_t> key = {uint32_t(vertex_hash_), uint32_t(vertex_hash_ >> 32),
                                 ps ? uint32_t(pixel_hash_) : 0, ps ? uint32_t(pixel_hash_ >> 32) : 0,
                                 topology_class, rectangles, msaa, depth ? depth->image.format : 0u,
                                 regs_[r::PA_CL_CLIP_CNTL] & (1u << 16)};
    for (uint32_t i = 0; i < 4; i++) {
        if (!colors[i]) continue;
        color_formats[i] = colors[i]->image.format;
        uint32_t blend = regs_[r::RB_BLENDCONTROL[i]];
        VkPipelineColorBlendAttachmentState &b = blends[i];
        b.colorWriteMask = color_masks[i];
        b.srcColorBlendFactor = blend_factor(blend & 0x1F);
        b.colorBlendOp = blend_op((blend >> 5) & 7);
        b.dstColorBlendFactor = blend_factor((blend >> 8) & 0x1F);
        b.srcAlphaBlendFactor = blend_factor((blend >> 16) & 0x1F);
        b.alphaBlendOp = blend_op((blend >> 21) & 7);
        b.dstAlphaBlendFactor = blend_factor((blend >> 24) & 0x1F);
        b.blendEnable = (blend & 0x1FFF1FFF) != 0x00010001; // not ONE, ADD, ZERO for both
        key.insert(key.end(), {i, uint32_t(color_formats[i]), color_masks[i], blend});
    }
    VkPipelineShaderStageCreateInfo stages[3] = {};
    uint32_t stage_count = 0;
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                             vs->module, "main", nullptr};
    if (rectangles || points)
        stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                 VK_SHADER_STAGE_GEOMETRY_BIT, points ? point_gs_ : rectangle_gs_, "main", nullptr};
    if (ps)
        stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                 VK_SHADER_STAGE_FRAGMENT_BIT, ps->module, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    assembly.primitiveRestartEnable = restart;
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    uint32_t sc_mode = regs_[r::PA_SU_SC_MODE_CNTL];
    bool triangles = primitive >= 4 && !rectangles;
    if (triangles) {
        raster.cullMode = (sc_mode & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (sc_mode & 2 ? VK_CULL_MODE_BACK_BIT : 0);
        raster.frontFace = sc_mode & 4 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        if ((sc_mode >> 3) & 3) approximate("polygon modes other than fill");
    }
    raster.depthClampEnable = (regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1;
    raster.depthBiasEnable = depth ? VK_TRUE : VK_FALSE; // the polygon offset, set at the draw
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = msaa == 0 ? VK_SAMPLE_COUNT_1_BIT : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_4_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (depth) {
        depth_state.depthTestEnable = (depth_control >> 1) & 1;
        depth_state.depthWriteEnable = (depth_control >> 2) & 1;
        depth_state.depthCompareOp = compare_op(depth_control >> 4);
        depth_state.stencilTestEnable = depth_control & 1;
        auto face = [&](uint32_t shift) {
            VkStencilOpState s{};
            s.compareOp = compare_op(depth_control >> shift);
            s.failOp = stencil_op(depth_control >> (shift + 3));
            s.passOp = stencil_op(depth_control >> (shift + 6));
            s.depthFailOp = stencil_op(depth_control >> (shift + 9));
            return s;
        };
        depth_state.front = face(8);
        depth_state.back = (depth_control >> 7) & 1 ? face(20) : depth_state.front;
    }
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 4;
    blend_state.pAttachments = blends;
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        // extended dynamic state 1 and 2 (Vulkan 1.3), set at each draw
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = depth ? depth->image.format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_state;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    if (export_only) {
        // the vertex shader alone, rasterization discarded: no viewport,
        // multisample, depth, blend or dynamic state; a render pass without
        // attachments
        key = {uint32_t(vertex_hash_), uint32_t(vertex_hash_ >> 32), 0xE0u};
        raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.rasterizerDiscardEnable = VK_TRUE;
        raster.lineWidth = 1.0f;
        info.stageCount = 1;
        info.pViewportState = nullptr;
        info.pMultisampleState = nullptr;
        info.pDepthStencilState = nullptr;
        info.pColorBlendState = nullptr;
        info.pDynamicState = nullptr;
        rendering.colorAttachmentCount = 0;
    }
    // made on a worker while the draw goes without it when there is a pixel
    // shader (Xenia's D3D12 pipeline cache: draws without one get theirs at once)
    bool pending = false;
    VkPipeline pipe = pipeline(key, info, options_.async_pipelines && ps, pending);
    if (pending) return skip("pipeline not ready (being made)");
    if (!pipe) return skip("vkCreateGraphicsPipelines failed");

    // the vertex data: the ranges of the vertex fetch constants the shaders
    // use (address and size, as Xenia's IssueDraw requests them), merged,
    // read from the mirror in device memory as Xenia's shaders read its
    // shared memory, guest memory's bytes put there where the CPU wrote them
    // since (request) and where a resolve or an export has not written newer
    // ones. (Read from host memory, the arena's copies, they made the
    // scene's vertex shaders wait on the bus: 6.7 ms of El Chavo Kart's race
    // frame.)
    struct VertexSlot {
        uint32_t slot, start, end;
    };
    std::vector<VertexSlot> vertex_slots;
    std::vector<Range> vertex_ranges;
    {
        std::set<uint32_t> fetch_slots;
        for (const Shader *s : {vs, ps})
            if (s) fetch_slots.insert(s->summary.vertex_slots.begin(), s->summary.vertex_slots.end());
        for (uint32_t slot : fetch_slots) {
            const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 2];
            // Xenia draws with any type here (its gpu_allow_invalid_fetch_constants
            // is on by default), warning about texture constants
            FetchConstantType type = FetchConstantType(f[0] & 3);
            if (type == FetchConstantType::Texture || type == FetchConstantType::InvalidTexture)
                approximate("a vertex fetch from a texture fetch constant");
            uint32_t start = f[0] & 0x1FFFFFFC, words = (f[1] >> 2) & 0xFFFFFF;
            uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(start) + uint64_t(words) * 4, GuestMemory::SIZE));
            if (end > start) vertex_slots.push_back({slot, start, end});
        }
        std::vector<Range> sorted;
        for (const VertexSlot &v : vertex_slots) sorted.push_back({v.start, v.end});
        std::sort(sorted.begin(), sorted.end());
        for (const Range &r : sorted) {
            if (!vertex_ranges.empty() && r.first <= vertex_ranges.back().second)
                vertex_ranges.back().second = std::max(vertex_ranges.back().second, r.second);
            else
                vertex_ranges.push_back(r);
        }
    }
    {
        Timer timer(report_.vertex_seconds);
        report_.vertex_bytes += request(vertex_ranges);
        // guest memory's bytes under the streams' pages, where the mirror has
        // nothing newer (Xenia's RequestRange of each stream)
        report_.vertex_bytes += request(exported);
    }
    if (!fatal_.empty()) return;

    // everything else the draw takes from the arena, in one submission
    VkDeviceSize index_bytes = indices.size() * 4;
    VkDeviceSize constants_size = (sizeof(harness::Constants) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    VkDeviceSize system_size = (sizeof(System) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    bool own_indices = index_bytes > ARENA_SLOT / 4;
    VkDeviceSize need = 2 * constants_size + system_size + 4 * ubo_align_ + (own_indices ? 0 : index_bytes + 4);
    if (arena_take(need, 1) == NONE) flush();
    else slots_[current_].used -= need; // only a check that the draw fits
    if (!fatal_.empty()) return;

    uint32_t dynamic_offsets[3];
    for (int stage = 0; stage < 2; stage++) {
        if (last_constants_at_[stage] == NONE ||
            std::memcmp(&last_constants_[stage], &constants[stage], sizeof(harness::Constants)) != 0) {
            last_constants_at_[stage] = arena_take(sizeof(harness::Constants), ubo_align_);
            std::memcpy(arena_data(last_constants_at_[stage]), &constants[stage], sizeof(harness::Constants));
            last_constants_[stage] = constants[stage];
        }
    }
    if (last_system_at_ == NONE || std::memcmp(&last_system_, &sys, sizeof(System)) != 0) {
        last_system_at_ = arena_take(sizeof(System), ubo_align_);
        std::memcpy(arena_data(last_system_at_), &sys, sizeof(System));
        last_system_ = sys;
    }
    dynamic_offsets[0] = uint32_t(last_constants_at_[0]);
    dynamic_offsets[1] = uint32_t(last_system_at_);
    dynamic_offsets[2] = uint32_t(last_constants_at_[1]);
    VkBuffer index_buffer;
    VkDeviceSize index_at;
    if (own_indices) {
        vk::Buffer own = d_.create_buffer(index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        if (!own.buffer) {
            fatal_ = "could not make an index buffer";
            return;
        }
        std::memcpy(own.mapped, indices.data(), index_bytes);
        slots_[current_].after.push_back([this, own]() mutable { d_.destroy(own); });
        index_buffer = own.buffer, index_at = 0;
    } else {
        index_at = arena_take(index_bytes, 4);
        std::memcpy(arena_data(index_at), indices.data(), index_bytes);
        index_buffer = arena_.buffer;
    }

    // record, in the render pass of these targets (none for exports alone);
    // a draw that exports begins its own, after a barrier, as what came
    // before in the pass may read the mirror where it writes
    if (exports) end_pass();
    Pass pass;
    for (uint32_t i = 0; i < 4; i++) pass.colors[i] = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
    pass.depth = depth ? depth->view : VK_NULL_HANDLE;
    pass.width = export_only ? 1 : width, pass.height = export_only ? 1 : height;
    VkCommandBuffer c = begin_pass(pass, colors, depth);
    if (pipe != bound_pipeline_) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipeline_ = pipe;
    }
    VkDescriptorSet sets[2] = {draw_sets_[1], bindless_};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 3, dynamic_offsets);
    vkCmdBindIndexBuffer(c, index_buffer, index_at, VK_INDEX_TYPE_UINT32);
    if (export_only) {
        vkCmdDrawIndexed(c, uint32_t(indices.size()), 1, 0, 0, 0);
        report_.drawn++;
        end_export(exported);
        if (options_.verbose) {
            std::fprintf(stderr, "  draw %llu: %s, %u indices, memory export only, shader %016llX, streams",
                         (unsigned long long)report_.draws, primitive_name(primitive), count,
                         (unsigned long long)vertex_hash_);
            for (auto [start, end] : exported) std::fprintf(stderr, " %08X-%08X", start, end);
            std::fprintf(stderr, "\n");
        }
        return;
    }
    uint32_t stencil = regs_[r::RB_STENCILREFMASK], stencil_bf = regs_[r::RB_STENCILREFMASK_BF];
    if (!((depth_control >> 7) & 1)) stencil_bf = stencil;
    const float *blend_constants = reinterpret_cast<const float *>(&regs_[r::RB_BLEND_RED]);
    vkCmdSetViewport(c, 0, 1, &viewport.viewport);
    vkCmdSetScissor(c, 0, 1, &scissor);
    vkCmdSetBlendConstants(c, blend_constants);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, stencil & 0xFF);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, stencil_bf & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (stencil >> 8) & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (stencil_bf >> 8) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (stencil >> 16) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (stencil_bf >> 16) & 0xFF);
    // the extended dynamic state, as the pipeline's description has it
    vkCmdSetPrimitiveTopology(c, topology);
    vkCmdSetPrimitiveRestartEnable(c, restart);
    vkCmdSetCullMode(c, raster.cullMode);
    vkCmdSetFrontFace(c, raster.frontFace);
    vkCmdSetDepthTestEnable(c, depth_state.depthTestEnable);
    vkCmdSetDepthWriteEnable(c, depth_state.depthWriteEnable);
    vkCmdSetDepthCompareOp(c, depth_state.depthCompareOp);
    vkCmdSetStencilTestEnable(c, depth_state.stencilTestEnable);
    const VkStencilOpState &front = depth_state.front, &back = depth_state.back;
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, front.failOp, front.passOp, front.depthFailOp, front.compareOp);
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, back.failOp, back.passOp, back.depthFailOp, back.compareOp);
    vkCmdSetDepthBiasEnable(c, raster.depthBiasEnable);
    {
        // the polygon offset as Xenia's Vulkan backend sets it
        // (draw_util.cc's GetPreferredFacePolygonOffset, then the guest's
        // units in host ones: kD3D10PolygonOffsetFactorFloat24 or Unorm24,
        // the slope in 1/16 subpixels)
        const float *offsets = reinterpret_cast<const float *>(&regs_[r::PA_SU_POLY_OFFSET_FRONT_SCALE]);
        float scale = 0.0f, offset = 0.0f;
        if (polygonal) {
            if ((sc_mode >> 11) & 1 && !(sc_mode & 1)) {
                scale = offsets[0], offset = offsets[1];
                // base/math.h's roundToNearestOrderOfMagnitude
                if (scale != 0.0f) {
                    double order = std::pow(10.0, std::floor(std::log10(std::fabs(double(scale)))));
                    scale = float(std::round(double(scale) / order) * order);
                }
            }
            if ((sc_mode >> 12) & 1 && !(sc_mode & 2) && scale == 0.0f && offset == 0.0f)
                scale = offsets[2], offset = offsets[3];
        } else if ((sc_mode >> 13) & 1) {
            scale = offsets[0], offset = offsets[1];
        }
        float constant = offset * (depth && depth->format == 1 ? float(1u << 24) : float((1u << 24) - 1));
        vkCmdSetDepthBias(c, constant, 0.0f, scale * (1.0f / 16.0f));
    }
    if (options_.gpu_timing_draws) {
        // each draw timed on its own, counted to its shaders
        char kind[96];
        std::snprintf(kind, sizeof(kind), "draw: %s, shaders %016llX %016llX", primitive_name(primitive),
                      (unsigned long long)vertex_hash_, ps ? (unsigned long long)pixel_hash_ : 0ull);
        gpu_mark(kind);
    }
    zpd_draw(c);
    vkCmdDrawIndexed(c, uint32_t(indices.size()), 1, 0, 0, 0);
    if (options_.gpu_timing_draws) gpu_mark("draws: between draws");
    report_.drawn++;
    end_export(exported);
    if (options_.verbose) {
        std::fprintf(stderr, "  draw %llu: %s, %u indices, %s%s%s, shaders %016llX %016llX, blend %08X, mask %04X, vertices",
                     (unsigned long long)report_.draws, primitive_name(primitive), count,
                     ps ? "color and depth" : "depth only", depth ? ", depth on" : "", exports ? ", memory export" : "",
                     (unsigned long long)vertex_hash_, ps ? (unsigned long long)pixel_hash_ : 0ull,
                     regs_[r::RB_BLENDCONTROL[0]], regs_[reg::RB_COLOR_MASK] & 0xFFFF);
        for (auto [start, end] : vertex_ranges) std::fprintf(stderr, " %08X-%08X", start, end);
        std::fprintf(stderr, "\n");
    }
}

// ---- resolves ----

void Impl::resolve() {
    report_.resolves++;
    poll();
    resolve::Info info;
    std::string why;
    // the rectangle's three vertices, in vertex fetch constant 0
    traced(regs_[reg::FETCH_CONSTANTS] & 0x1FFFFFFC, 24);
    if (!resolve::get_info({regs_, REGISTER_COUNT}, memory_, info, why)) return skip("resolve: " + why);
    if (!info.copy && !why.empty()) approximate("resolve not copied: " + why);
    uint32_t pitch = regs_[reg::RB_SURFACE_INFO] & 0x3FFF;
    RenderTarget *source = render_target(info.depth, info.edram_base, info.surface_pitch ? info.surface_pitch : pitch,
                                         info.msaa, info.source_format);
    if (!source) return fatal_.empty() ? skip("resolve: source format") : void();
    uint32_t x = std::min(info.x, source->width), y = std::min(info.y, source->height);
    uint32_t w = std::min(info.width, source->width - x), h = std::min(info.height, source->height - y);
    acquire(source, y + h);
    if (!fatal_.empty()) return;
    if (options_.verbose)
        std::fprintf(stderr, "  resolve: %s base %u (%u,%u %ux%u) -> %08X format %u%s%s\n", info.depth ? "depth" : "color",
                     info.edram_base, x, y, w, h, info.dest_base, info.dest_format, info.clear_color ? ", clear color" : "",
                     info.clear_depth ? ", clear depth" : "");

    if (info.copy && w && h) {
        resolve::Info clipped = info;
        clipped.x = x, clipped.y = y, clipped.width = w, clipped.height = h;
        gpu_resolve(clipped, *source);
        if (!fatal_.empty()) return;
    }

    // the clears, after the copy, over the same rectangle
    if ((info.clear_color || info.clear_depth) && w && h) {
        uint32_t msaa = (regs_[reg::RB_SURFACE_INFO] >> 16) & 3;
        RenderTarget *depth_target = nullptr, *color_target = nullptr;
        if (info.clear_depth) {
            uint32_t depth_info = regs_[reg::RB_DEPTH_INFO];
            depth_target = render_target(true, depth_info & 0xFFF, source->pitch, msaa, (depth_info >> 16) & 1);
        }
        if (info.clear_color) {
            uint32_t color_info = regs_[r::COLOR_INFO[info.color_target & 3]];
            color_target = render_target(false, color_info & 0xFFF, source->pitch, msaa, (color_info >> 16) & 0xF);
        }
        for (RenderTarget *t : {depth_target, color_target}) {
            if (!t) continue;
            acquire(t, y + h);
            if (!fatal_.empty()) return;
            VkClearAttachment clear{};
            if (t->depth) {
                uint32_t value = regs_[r::RB_DEPTH_CLEAR];
                clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                // xenos::UNorm24To32, or D24FS8 halved (as the target keeps it)
                uint32_t d24 = value >> 8;
                clear.clearValue.depthStencil.depth = t->format == 0 ? float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f)
                                                                     : float24_to_float(d24) * 0.5f;
                clear.clearValue.depthStencil.stencil = value & 0xFF;
            } else {
                clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                uint64_t value = regs_[r::RB_COLOR_CLEAR];
                if (t->wide) value = value << 32 | regs_[r::RB_COLOR_CLEAR_LO];
                auto &cv = clear.clearValue.color;
                using H = texture::HostFormat;
                if (t->format == 3 || t->format == 12) {
                    // 2_10_10_10_FLOAT: 7e3 floats kept as 16-bit floats
                    for (int j = 0; j < 3; j++) cv.float32[j] = float7e3_to_float(uint32_t(value >> (10 * j)));
                    cv.float32[3] = float((value >> 30) & 3) / 3.0f;
                } else switch (t->host) {
                case H::RGBA8:
                    for (int j = 0; j < 4; j++) cv.float32[j] = float((value >> (8 * j)) & 0xFF) / 255.0f;
                    break;
                case H::RGB10A2:
                    for (int j = 0; j < 3; j++) cv.float32[j] = float((value >> (10 * j)) & 0x3FF) / 1023.0f;
                    cv.float32[3] = float((value >> 30) & 3) / 3.0f;
                    break;
                case H::RG16F: case H::RGBA16F:
                    for (int j = 0; j < 4; j++) cv.float32[j] = half_to_float(uint16_t(value >> (16 * j)));
                    break;
                case H::R32F: case H::RG32F:
                    cv.float32[0] = as_float(uint32_t(value >> (t->host == H::RG32F ? 32 : 0)));
                    cv.float32[1] = as_float(uint32_t(value));
                    break;
                case H::RG16: case H::RGBA16: // snorm: the guest's bits
                    for (int j = 0; j < 4; j++)
                        cv.float32[j] = std::max(-1.0f, float(int16_t(value >> (16 * j))) / 32767.0f);
                    break;
                default:
                    approximate("clears of this target format");
                    break;
                }
            }
            VkClearRect rect{{{int32_t(x), int32_t(y)}, {std::min(w, t->width - std::min(x, t->width)), std::min(h, t->height - std::min(y, t->height))}}, 0, 1};
            if (!rect.rect.extent.width || !rect.rect.extent.height) continue;
            VkCommandBuffer c = outside();
            gpu_mark("resolves: clears");
            VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView = t->view;
            attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, {t->width, t->height}};
            rendering.layerCount = 1;
            if (t->depth) {
                rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
            } else {
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
            }
            vkCmdBeginRendering(c, &rendering);
            vkCmdClearAttachments(c, 1, &clear, 1, &rect);
            vkCmdEndRendering(c);
        }
    }
}

// The copy on the GPU: the rectangle (resolved to one sample per pixel when
// multisampled) copied into a scratch buffer as the host keeps it, then
// written into the mirror as the destination says (resolve.comp, resolve.cpp's
// write); what of its pages it does not write keeps guest memory's bytes
// (made current in the mirror first, Xenia's RequestRange of the extent).
// Its pages are then the GPU's (gpu_written); textures over it are loaded
// again; guest memory gets the bytes as readback_resolve says.
void Impl::gpu_resolve(const resolve::Info &info, RenderTarget &source) {
    std::vector<Range> written, partial;
    if (!resolve::destination_ranges(info, written, partial)) {
        approximate(std::string("resolve not written: resolve to ") + format_name(info.dest_format));
        return;
    }
    if (written.empty()) return;
    // the pages the copy does not write whole: those with a partial tile, and
    // those a written run starts or ends inside
    std::vector<Range> keep = partial;
    {
        std::vector<Range> runs = written;
        std::sort(runs.begin(), runs.end());
        std::vector<Range> merged;
        for (const Range &r : runs) {
            if (!merged.empty() && r.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, r.second);
            else
                merged.push_back(r);
        }
        uint32_t mask = (1u << page_shift_) - 1;
        for (const Range &r : merged) {
            if (r.first & mask) keep.push_back({r.first, r.first + 1});
            if (r.second & mask) keep.push_back({r.second - 1, r.second});
        }
    }
    uint32_t w = info.width, h = info.height;
    uint32_t texel = info.depth ? 4 : texture::bytes_per_block(source.host);
    if (!texel) {
        approximate(std::string("resolve not written: resolve from ") + color_target_format_name(info.source_format));
        return;
    }
    VkDeviceSize pixel_bytes = VkDeviceSize(w) * h * texel;
    VkDeviceSize stencil_bytes = info.depth ? (VkDeviceSize(w) * h + 3) & ~VkDeviceSize(3) : 0;
    // a multisampled source is read from EDRAM, its samples as the guest
    // keeps them, as Xenia's resolve shaders read them (the sample or the
    // average RB_COPY_CONTROL selects; depth one sample): the rectangle's
    // tiles stored there first by the target, which keeps them
    const bool from_edram = info.msaa != 0;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!from_edram && !grow(scratch_, pixel_bytes + stencil_bytes, usage, resolve_set_, 0)) return;
    request(keep);

    VkCommandBuffer c;
    if (from_edram) {
        std::vector<uint32_t> tiles;
        for (uint32_t row = info.y / source.tile_height; row <= (info.y + h - 1) / source.tile_height; row++)
            for (uint32_t column = info.x / source.tile_width; column <= (info.x + w - 1) / source.tile_width; column++)
                tiles.push_back((source.base + row * source.pitch_tiles + column) % EDRAM_TILES);
        if (!edram_store(&source, tiles)) return;
        c = outside();
        gpu_mark("resolves: copies");
    } else {
        c = outside();
        gpu_mark("resolves: copies");
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VkImageAspectFlags(info.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                 0, 0, 1};
        copy.imageOffset = {int32_t(info.x), int32_t(info.y), 0};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(c, source.image.image, VK_IMAGE_LAYOUT_GENERAL, scratch_.buffer, 1, &copy);
        if (info.depth) {
            copy.bufferOffset = pixel_bytes;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdCopyImageToBuffer(c, source.image.image, VK_IMAGE_LAYOUT_GENERAL, scratch_.buffer, 1, &copy);
        }
        full_barrier(c);
    }

    // the destination's words, as resolve.cpp's write makes them
    ResolveConstants p{};
    p.width = w, p.height = h;
    p.src_words = info.depth ? 1 : texel / 4;
    p.source_format = info.source_format;
    p.depth = info.depth;
    // depth words as the read-back gives them: D24S8's unorm bits, or a
    // float (D24FS8 kept halved, or D24S8 kept as a float where the device
    // has no D24S8)
    p.depth_mode = !info.depth || source.image.format != VK_FORMAT_D32_SFLOAT_S8_UINT ? 0 : source.format == 1 ? 1 : 2;
    p.stencil_word = uint32_t(pixel_bytes / 4);
    p.raw = resolve::raw_copy(info);
    p.dest_base = info.dest_base, p.dest_pitch = info.dest_pitch, p.dest_height = info.dest_height;
    p.dest_x = info.dest_x, p.dest_y = info.dest_y, p.dest_slice = info.dest_slice, p.dest_array = info.dest_array;
    p.dest_format = info.dest_format;
    p.dest_log2_bytes = uint32_t(std::countr_zero(texture::format_block(info.dest_format).bytes));
    p.dest_endian = info.dest_endian, p.dest_number = info.dest_number, p.dest_swap = info.dest_swap;
    p.exp_factor = std::ldexp(1.0f, info.dest_exp_bias);
    if (from_edram) {
        p.edram = 1;
        p.edram_base = source.base, p.edram_pitch = source.pitch_tiles, p.msaa = source.msaa;
        p.ints_log2 = source.wide ? 1 : 0;
        p.src_x = info.x, p.src_y = info.y;
        p.first_sample = resolve::first_sample(info.sample_select);
        p.samples = resolve::sample_count(info.sample_select);
    }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 1,
                            from_edram ? &resolve_edram_set_ : &resolve_set_, 0, nullptr);
    vkCmdPushConstants(c, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(c);

    gpu_written(written);
    for (auto &[key, t] : textures_) {
        if (t->high <= written.front().first || t->low >= written.back().second) continue;
        for (const Range &r : written)
            for (const Range &tr : t->ranges)
                if (overlaps(r, tr)) t->stale = true;
    }
    if (options_.readback_resolve != ReadbackResolve::none) read_back_resolve(written);
}

// Guest memory given what a resolve wrote (Xenia's
// IssueCopy_ReadbackResolvePath, readback_resolve fast or full), through the
// physical view, which no watch sees: the mirror keeps the pages as the GPU's.
// full: copied from the mirror and waited for, at once. fast: copied into
// one of the resolve's two read-back buffers (by address and length, as
// Xenia keys them), and guest memory given the other, which the same
// resolve filled the frame before (the first time, this one's, waited for).
// Where Xenia differs: it copies the extent from the resolve's first byte to
// its last, the tiles between included, where only the written ranges are
// copied here; and fast reads the other buffer without waiting, which may
// still be in flight on the GPU, where it is waited for here (a frame old,
// it is done).
void Impl::read_back_resolve(const std::vector<Range> &written) {
    uint32_t first = written.front().first, last = written.front().second;
    for (const Range &r : written) first = std::min(first, r.first), last = std::max(last, r.second);
    VkDeviceSize extent = last - first;
    VkBuffer into = VK_NULL_HANDLE;
    VkDeviceSize into_at = 0;
    const uint8_t *data = nullptr;
    ResolveReadback *rb = nullptr;
    if (options_.readback_resolve == ReadbackResolve::full) {
        uint8_t *mapped = readback(extent, into, into_at);
        if (!mapped) return;
        data = mapped;
    } else {
        rb = &resolve_readbacks_[uint64_t(first) << 32 | uint32_t(extent)];
        rb->last_used = report_.swaps;
        vk::Buffer &b = rb->buffers[rb->current];
        if (b.size < extent) {
            if (b.buffer) {
                vk::Buffer old = b;
                cmd();
                slots_[current_].after.push_back([this, old]() mutable { d_.destroy(old); });
            }
            b = d_.create_buffer(std::max<VkDeviceSize>(extent, 4096), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
            if (!b.buffer) {
                fatal_ = "could not make a resolve's read-back buffer";
                return;
            }
        }
        into = b.buffer;
    }
    std::vector<VkBufferCopy> copies;
    for (auto [start, end] : written) copies.push_back({start, into_at + (start - first), end - start});
    VkCommandBuffer c = outside();
    gpu_mark("resolves: read-backs for guest memory");
    vkCmdCopyBuffer(c, mirror_.buffer, into, uint32_t(copies.size()), copies.data());
    if (rb) {
        rb->serials[rb->current] = slots_[current_].serial;
        uint32_t other = rb->current ^ 1;
        if (rb->buffers[other].size < extent) {
            // nothing from the frame before: this one's, waited for
            other = rb->current;
            sync();
        } else if (rb->serials[other] > done_serial_) {
            wait_serial(rb->serials[other]);
        }
        data = static_cast<const uint8_t *>(rb->buffers[other].mapped);
    } else {
        sync();
    }
    if (!fatal_.empty() || !data) return;
    Timer timer(report_.writeback_seconds);
    for (auto [start, end] : written) {
        std::memcpy(memory_.data() + start, data + (start - first), end - start);
        report_.writeback_bytes += end - start;
        if (trace_) trace_->gpu_wrote(start, end - start);
    }
}

// A frame begins (a swap): the fast read-backs' buffers trade places, and
// those of resolves not seen for a while go (Xenia's BeginSubmission on
// opening a frame, with its eviction above 64 pairs)
void Impl::next_readback_frame() {
    if (resolve_readbacks_.empty()) return;
    for (auto &[key, rb] : resolve_readbacks_) rb.current ^= 1;
    if (resolve_readbacks_.size() <= 64) return;
    for (auto it = resolve_readbacks_.begin(); it != resolve_readbacks_.end();) {
        if (report_.swaps > 60 && it->second.last_used < report_.swaps - 60) {
            ResolveReadback old = it->second;
            cmd();
            slots_[current_].after.push_back([this, old]() mutable {
                for (vk::Buffer &b : old.buffers) d_.destroy(b);
            });
            it = resolve_readbacks_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- the swap: texture fetch constant 0, as Xenia's RequestSwapTexture ----

// Swaps are numbered from 0; the front buffer is read back for the ones the
// options ask for. The frame's work goes to the GPU, and then the front
// buffer's image to on_swap (a presenter on this device): texture(fetch
// constant 0) gives it (in the general layout), loaded on the GPU from the
// mirror when a resolve wrote it, its loads recorded before the flush; what
// the hook submits on the device's queue after the flush comes after them,
// and before the renderer's next work.
void Impl::swap() {
    // the draws after a swap count for no report until the next one
    // (Xenia's EndZPDFrame)
    end_query();
    zpd_current_ = 0;
    uint64_t number = report_.swaps++;
    const bool read = options_.frame_wanted && options_.frame_wanted(number);
    texture::Info info;
    Texture *t = read || options_.on_swap ? front_texture(info) : nullptr;
    if (read && t) read_front_buffer(number, *t, info);
    FrontBuffer front;
    if (t && options_.on_swap) {
        front.swap = number;
        front.image = t->image.image, front.format = t->image.format;
        front.width = t->image.extent.width, front.height = t->image.extent.height;
        // the fetch constant's swizzle through the host format's, over the
        // image's four components
        front.swizzle = texture::compose_swizzle(info.swizzle, t->host_swizzle);
        front.components = 4;
        front.gamma_table = info.format != 7 && info.format != 54; // as deliver
        front.gamma = processor_.gamma_ramp_table();
    }
    flush();
    if (front.image && fatal_.empty()) options_.on_swap(front);
    poll();
    next_readback_frame();
    // the pipeline cache written once new pipelines have settled
    if (pipelines_saved_ != report_.pipelines && report_.swaps - last_pipeline_swap_ >= 300) save_pipeline_cache();
}

// The texture of fetch constant 0 that a swap shows, its loads recorded;
// nullptr when it is none (counted as approximate)
Texture *Impl::front_texture(texture::Info &info) {
    const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS];
    info = texture::parse_fetch_constant(fetch);
    if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture || info.dimension != 1 || !info.base_address) {
        approximate("a swap without a 2D texture in fetch constant 0");
        return nullptr;
    }
    texture::Plan plan;
    std::string why;
    if (!texture::plan(info, plan, why, support_)) {
        approximate("swap texture not decoded: " + why);
        return nullptr;
    }
    if (plan.slices.empty() || plan.slices[0].level != 0) {
        approximate("swap texture not decoded: no base level");
        return nullptr;
    }
    Texture *t = texture(fetch, why);
    if (!t && fatal_.empty()) approximate("swap texture not decoded: " + why);
    return t;
}

// The front buffer's level 0 copied into a read-back buffer on the GPU,
// handed to on_frame once the GPU is done (deliver), without waiting for it.
void Impl::read_front_buffer(uint64_t number, Texture &t, const texture::Info &info) {
    FrontRead read;
    read.number = number;
    read.host = t.host;
    read.width = t.image.extent.width, read.height = t.image.extent.height;
    read.swizzle = texture::compose_swizzle(info.swizzle, t.host_swizzle), read.format = info.format;
    read.gamma = processor_.gamma_ramp_table();
    read.pwl = processor_.gamma_ramp_pwl();
    bool compressed = texture::is_compressed(read.host);
    read.bytes = compressed ? VkDeviceSize((read.width + 3) / 4) * ((read.height + 3) / 4)
                            : VkDeviceSize(read.width) * read.height;
    read.bytes *= texture::bytes_per_block(read.host);
    VkBuffer back;
    VkDeviceSize at;
    uint8_t *data = readback(read.bytes, back, at);
    if (!data) return;
    read.data = data;
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {read.width, read.height, 1};
    VkCommandBuffer c = outside();
    gpu_mark("swap: the front buffer read back");
    vkCmdCopyImageToBuffer(c, t.image.image, VK_IMAGE_LAYOUT_GENERAL, back, 1, &copy);
    read.serial = slots_[current_].serial;
    front_reads_.push_back(std::move(read));
}

// A front buffer the GPU is done with, as Xenia's presenter shows it: the
// fetch constant's swizzle, opaque, through the display's gamma ramp.
void Impl::deliver(const FrontRead &read) {
    texture::Image image;
    image.format = read.host;
    image.width = read.width, image.height = read.height;
    image.levels.emplace_back(read.data, read.data + read.bytes);
    std::vector<uint8_t> rgba = texture::to_rgba8(image, 0);
    size_t pixels = size_t(read.width) * read.height;
    if (rgba.size() < pixels * 4) {
        approximate("swap texture not decoded: no base level");
        return;
    }
    // (read.swizzle is composed with the host format's: it names the image's components)
    Image frame;
    frame.width = read.width, frame.height = read.height;
    frame.rgba.resize(pixels * 4);
    for (size_t i = 0; i < pixels; i++)
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t guest = (read.swizzle >> (3 * c)) & 7;
            uint8_t value = guest == 5 ? 255 : guest >= 4 ? 0 : rgba[i * 4 + guest];
            frame.rgba[i * 4 + c] = c == 3 ? 255 : value; // shown opaque, as the presenter does
        }
    // the display's gamma, the processor's ramps: as Xenia's presenter, the
    // piecewise linear ramp for 2_10_10_10 front buffers
    // (apply_gamma_pwl.xesli: a component's 10 bits pick an entry by their
    // top 7, base plus delta times the low 3 over 8, in 1/64 of 1023), the
    // 256-entry table for the rest (apply_gamma_table.xesli), the 10 bits
    // they give back to 8 (presenter.h's Packed10bpcRGBTo8bpcBytes)
    if (read.format == 7 || read.format == 54) {
        if (read.host != texture::HostFormat::RGB10A2) {
            approximate("the display's piecewise linear gamma ramp (front buffer not 10 bits a component)");
        } else {
            for (size_t i = 0; i < pixels; i++) {
                uint32_t word;
                std::memcpy(&word, read.data + 4 * i, 4);
                const uint32_t values[4] = {word & 0x3FF, (word >> 10) & 0x3FF, (word >> 20) & 0x3FF,
                                            (word >> 30) * 341};
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t guest = (read.swizzle >> (3 * c)) & 7;
                    uint32_t value = guest == 5 ? 1023 : guest >= 4 ? 0 : values[guest];
                    uint32_t entry = read.pwl[(value >> 3) * 3 + c];
                    float out = (float(entry & 0xFFFF) + float((value & 7) * (entry >> 16)) * (1.0f / 8.0f)) *
                                (1.0f / (64.0f * 1023.0f));
                    uint32_t ten = uint32_t(std::clamp(out, 0.0f, 1.0f) * 1023.0f + 0.5f);
                    frame.rgba[i * 4 + c] = uint8_t(float(ten) * (255.0f / 1023.0f) + 0.5f);
                }
            }
        }
    } else {
        for (size_t i = 0; i < pixels; i++)
            for (uint32_t c = 0; c < 3; c++) {
                uint32_t entry = read.gamma[frame.rgba[i * 4 + c]];
                uint32_t ten = (entry >> (c == 0 ? 20 : c == 1 ? 10 : 0)) & 0x3FF;
                frame.rgba[i * 4 + c] = uint8_t((ten * 255 + 511) / 1023);
            }
    }
    report_.frames++;
    if (options_.on_frame) options_.on_frame(read.number, frame);
}

} // namespace

std::unique_ptr<Renderer> Renderer::create(vk::Device &device, gpu::CommandProcessor &processor,
                                           const Options &options, std::string &error) {
    auto renderer = std::make_unique<Impl>(device, processor, options);
    if (!renderer->init(error)) return nullptr;
    return renderer;
}

} // namespace xf::render
