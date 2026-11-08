// The Xenos renderer, the command processor's Sink (renderer.h). The
// viewport and scissor follow Xenia's src/xenia/gpu/draw_util.cc
// (GetHostViewportInfo, GetScissor), the register fields registers.h, the
// clear values vulkan/vulkan_render_target_cache.cc, the vertex data a draw
// needs vulkan/vulkan_command_processor.cc (IssueDraw's shared memory
// requests), the point sizes its UpdateSystemConstantValues, the rows a
// draw takes over in EDRAM draw_extent_estimator.cc, the tessellated draws
// and the indices primitive_processor.cc and d3d12/pipeline_cache.cc (the
// hull shaders by mode and domain), the swap
// vulkan/vulkan_texture_cache.cc (RequestSwapTexture), and the textures'
// lifetime texture_cache.cc (MarkAsUsed, CompletedSubmissionUpdated, the
// texture_cache_memory_limit_* options) with d3d12/d3d12_texture_cache.cc's
// descriptors given back with their texture; all BSD, see LICENSE-xenia.
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

#include "native_internal.h"

#include "xenoforge/guest_memory.h"
#include "xenoforge/memory.h"
#include "xenoforge/resolve.h"
#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/shader_storage.h"
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
// tessellation: the host vertex shader, and the hull shaders by input patch
// (1, 3 and 4 control points; adaptive triangles and quads)
const uint32_t TESSELLATION_VS[] = {
#include "tessellation.vert.inc"
};
const uint32_t TESSELLATION_1CP[] = {
#include "tessellation_1cp.tesc.inc"
};
const uint32_t TESSELLATION_3CP[] = {
#include "tessellation_3cp.tesc.inc"
};
const uint32_t TESSELLATION_4CP[] = {
#include "tessellation_4cp.tesc.inc"
};
const uint32_t TESSELLATION_ADAPTIVE_TRIANGLE[] = {
#include "tessellation_adaptive_triangle.tesc.inc"
};
const uint32_t TESSELLATION_ADAPTIVE_QUAD[] = {
#include "tessellation_adaptive_quad.tesc.inc"
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
const uint32_t EDRAM_LOAD_UINT[] = {
#include "edram_load_uint.frag.inc"
};
const uint32_t EDRAM_LOAD_DEPTH[] = {
#include "edram_load_depth.frag.inc"
};
const uint32_t EDRAM_LOAD_STENCIL[] = {
#include "edram_load_stencil.frag.inc"
};
const uint32_t DEPTH_FLOAT24[] = {
#include "depth_float24.frag.inc"
};
const uint32_t DEPTH_FLOAT24_ROUND[] = {
#include "depth_float24_round.frag.inc"
};
const uint32_t PLACEHOLDER[] = {
#include "placeholder.frag.inc"
};
const uint32_t RESOLVE[] = {
#include "resolve.comp.inc"
};
const uint32_t TEXTURE_LOAD[] = {
#include "texture_load.comp.inc"
};
// the interlock path's (edram_fsi.frag): the output merger the translated
// pixel shaders take in, the depth-only fragment shaders, and EDRAM's clears
const uint32_t EDRAM_FSI_LIBRARY[] = {
#include "edram_fsi_library.frag.inc"
};
const uint32_t EDRAM_FSI_DEPTH_PIXEL[] = {
#include "edram_fsi_depth_pixel.frag.inc"
};
const uint32_t EDRAM_FSI_DEPTH_SAMPLE[] = {
#include "edram_fsi_depth_sample.frag.inc"
};
const uint32_t EDRAM_CLEAR[] = {
#include "edram_clear.comp.inc"
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
constexpr uint32_t PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281, PA_SU_LINE_CNTL = 0x2282;
constexpr uint32_t PA_CL_UCP_0_X = 0x2388; // six planes of X, Y, Z, W
constexpr uint32_t DC_LUTA_CONTROL = 0x1930;
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284, VGT_HOS_CNTL = 0x2285;
constexpr uint32_t VGT_HOS_MAX_TESS_LEVEL = 0x2286, VGT_HOS_MIN_TESS_LEVEL = 0x2287;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181, SQ_INTERPOLATOR_CNTL = 0x2182;
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
// the textures resolves write as well, of 32- and of 64-bit texels each
// (Impl::resolve_target)
constexpr uint32_t RESOLVE_TARGETS = 64;

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
// xenos.cc's PWLGammaToLinear: a gamma value (0-1, saturated) to linear
float pwl_gamma_to_linear(float gamma) {
    gamma = std::isnan(gamma) ? 0.0f : std::clamp(gamma, 0.0f, 1.0f);
    float scale, offset;
    if (gamma >= 96.0f / 255.0f) {
        if (gamma >= 192.0f / 255.0f)
            scale = 8.0f / 1024.0f, offset = -1024.0f;
        else
            scale = 4.0f / 1024.0f, offset = -256.0f;
    } else {
        if (gamma >= 64.0f / 255.0f)
            scale = 2.0f / 1024.0f, offset = -64.0f;
        else
            scale = 1.0f / 1024.0f, offset = 0.0f;
    }
    float linear = gamma * ((255.0f * 1024.0f) * scale) + offset;
    linear += std::trunc(linear * scale);
    return linear * (1.0f / 1023.0f);
}

// xenos::Float32To20e4: towards zero, or to the nearest even
uint32_t float_to_float24(float f, bool round_to_nearest_even) {
    if (!(f > 0.0f)) return 0;
    uint32_t u = std::bit_cast<uint32_t>(f);
    if (u >= 0x3FFFFFF8) return 0xFFFFFF;
    if (u < 0x38800000)
        u = (0x800000 | (u & 0x7FFFFF)) >> std::min(113u - (u >> 23), 24u);
    else
        u += 0xC8000000u;
    if (round_to_nearest_even) u += 3 + ((u >> 3) & 1);
    return (u >> 3) & 0xFFFFFF;
}

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
    uint64_t hash = 0; // hash_words(type, microcode), its name in the shader storage
    xf::ucode::ParsedShader parsed;
    xf::ucode::ShaderSummary summary;
    bool translated = false;
    VkShaderModule module = VK_NULL_HANDLE;
    // a vertex shader's other host forms (shader::HostVertex, and the
    // spacing in bit 8 for the domain ones), made as draws ask for them
    std::map<uint32_t, VkShaderModule> variants;
    std::string unsupported; // why it cannot be drawn with
    std::vector<std::string> approximate; // shader::Translation's, counted at each draw
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
    // an 8_8_8_8_GAMMA target kept linear, as RGBA16 unorm, so that blending
    // is done in linear as the console's (Xenia's
    // gamma_render_target_as_unorm16); its gamma is made in EDRAM's bits
    bool gamma_linear = false;
    // a float target moved through EDRAM by its bits (NaN payloads,
    // denormals), through a UINT view of its image: the view, its format
    VkImageView raw = VK_NULL_HANDLE;
    VkFormat raw_format = VK_FORMAT_UNDEFINED;
};

// edram.glsl's push constants
struct EdramTarget {
    uint32_t base_tiles, pitch_tiles, msaa, format, width, height, is_depth, ints_log2, stencil_bit, flags;
    uint32_t x, y, clear_lo, clear_hi; // edram_clear.comp's
};

// edram_fsi.frag's uniform block (set 0 binding 4): the interlock path's
// render target state
struct FsiConstants {
    uint32_t flags, depth_base, pitch, zpd_index;
    uint32_t rt[4], blend[4], rt_base[4], stencil[4];
    float poly_offset[4], blend_constant[4], exp_bias[4];
    bool operator==(const FsiConstants &) const = default;
};
static_assert(sizeof(FsiConstants) == 128);
// its flags (edram_fsi.frag's F_ bits; the MSAA mode in bits 0-1)
constexpr uint32_t FSI_2X_AS_4X = 4, FSI_DEPTH_STENCIL = 8, FSI_DEPTH_LESS = 16, FSI_DEPTH_WRITE = 128,
                   FSI_STENCIL = 256, FSI_FLOAT24 = 512;

// resolve.comp's push constants
struct ResolveConstants {
    uint32_t width, height, src_words, source_format, depth, depth_mode, stencil_word, raw;
    uint32_t dest_base, dest_pitch, dest_height, dest_x, dest_y, dest_slice, dest_array;
    uint32_t dest_format, dest_log2_bytes, dest_endian, dest_number, dest_swap;
    float exp_factor;
    // a multisampled source read from EDRAM
    uint32_t edram, edram_base, edram_pitch, msaa, ints_log2, src_x, src_y, first_sample, samples;
    uint32_t depth_round; // D24FS8 from the host's float: 20e4 rounded to the nearest even
    uint32_t target;      // the texture written too (resolve.comp's p.target), 0 none
};
static_assert(sizeof(ResolveConstants) <= 128, "the push constants every device has room for");

// texture_load.comp's push constants
using LoadConstants = texture::GpuLevel;

// What makes a texture's image, out of its fetch constant (Xenia's
// TextureKey, as BindingInfoFromFetchConstant fills it): the pitch and
// tiling, the format, endianness and stacking, the size, the levels the
// constant gives (texture::levels: the base and mip pages within physical
// memory, so the windows' aliases of a page are one, and the last level),
// the dimension and packed mips, and whether it is the separate signed
// version. Not the swizzle, signs, exponent, filters, clamps, border, LOD
// bias nor the smallest level, which views and samplers take.
using TextureKey = std::array<uint32_t, 6>;
TextureKey texture_key(const texture::Info &info, const uint32_t fetch[6], bool signed_separate) {
    texture::Levels lv = texture::levels(info);
    TextureKey key;
    key[0] = fetch[0] & 0xFFC00000u; // pitch, tiled
    key[1] = fetch[1] & 0x000004FFu; // format, endianness, stacked
    key[2] = fetch[2];               // the size
    key[3] = lv.base ? lv.base_address : 0;
    key[4] = lv.mips ? lv.mip_address : 0;
    key[5] = (fetch[5] & 0x00000E00u) | lv.mip_max << 12 | (signed_separate ? 1u << 16 : 0u); // dimension, packed
    return key;
}

// why a texture is loaded: not held, its memory written by the CPU (seen by
// its watches), or by a resolve or a memory export (render to texture)
enum class LoadWhy : uint8_t { New, CpuWritten, GpuWritten };

// A view of a texture's image as fetch constants read it, at its place in the
// texture's bindless table (Xenia's SRV descriptors by SRVDescriptorKey): by
// the swizzle through the host format's, and whether it reads the signed
// components.
struct TextureView {
    uint32_t key = 0; // the swizzle (bits 0-11), signed (bit 12)
    VkImageView view = VK_NULL_HANDLE;
    uint32_t index = 0;
};

// An image of guest memory, by what makes the image (Xenia's TextureKey: the
// memory, format, size and layout, and whether it is a separate signed
// version); constants over it that differ in their swizzle and signs read it
// through views of their own, and in their filters and clamps through
// samplers of their own.
struct Texture {
    vk::Image image;
    uint32_t dimension = 1;
    texture::HostFormat host = texture::HostFormat::Unsupported; // what its image holds
    uint32_t host_swizzle = texture::SWIZZLE_RGBA;               // what each guest component reads of it
    uint32_t kind = 0;                                           // its bindless table (2D arrays, 3D, cube)
    // what signed components read: a snorm view of the image (made mutable
    // for it), or an image of their own where the signed bits differ
    // (signed_image, in a texture keyed as signed separate); undefined: none
    // (the float formats are signed either way)
    texture::Signed signed_read = texture::Signed::None;
    VkFormat signed_format = VK_FORMAT_UNDEFINED;
    vk::Image signed_image;
    std::vector<TextureView> views;
    std::vector<Range> ranges;  // the guest memory it was loaded from
    uint32_t checked = 0;       // the write count its memory was last known unwritten at (Memory's)
    uint32_t low = 0, high = 0; // the ranges' bounds
    bool stale = false;         // a resolve or a memory export wrote memory under it
    bool gpu = false;           // loaded from what the GPU wrote (the mirror's, or read back from it)
    // why its next load comes (Report's texture_loads_*)
    LoadWhy load_why = LoadWhy::New;
    // the buckets of memory it is listed in (Impl::texture_buckets_; none
    // while first > last)
    uint32_t bucket_first = 1, bucket_last = 0;
    // its lifetime (Xenia's TextureCache::Texture): its key in the cache, the
    // GPU memory its images hold (host_memory_usage), the last submission
    // that used it and the host's clock then (last_usage_submission_index,
    // last_usage_time), and its place in the list by last use (used_previous,
    // used_next)
    TextureKey key{};
    VkDeviceSize bytes = 0;
    uint64_t used_serial = 0, used_ms = 0;
    Texture *older = nullptr, *newer = nullptr;
    // a resolve target (Impl::resolve_target): its slot in the resolves'
    // target table plus one (0: not one) and bit 7 for a 64-bit texel; its
    // unsigned view there; and what a resolve must write to feed it (its
    // base, pitch in texels, guest format and endianness)
    uint32_t target = 0;
    VkImageView target_view = VK_NULL_HANDLE;
    uint32_t target_base = 0, target_pitch = 0, target_endian = 0;
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
    float clip_planes[6][4];   // the user clip planes enabled, packed (shader::VertexOptions)
};
static_assert(sizeof(System) == 576);

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
    uint32_t luta_control = 0; // DC_LUTA_CONTROL at the swap
};

// The attachments of the render pass under way.
struct Pass {
    VkImageView colors[4] = {};
    VkImageView depth = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    // the interlock path's passes have no attachments: their draws' samples
    // apart (all pipelines in such a pass rasterize at one count)
    uint32_t samples = 0;
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
// states, and chained the dynamic rendering formats and a pipeline library's
// part and libraries).
struct PipelineDesc {
    VkPipelineShaderStageCreateInfo stages[5]{};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    VkVertexInputBindingDescription vertex_bindings[1]{};
    VkVertexInputAttributeDescription vertex_attributes[1]{};
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
    VkGraphicsPipelineLibraryCreateInfoEXT library_part{};
    VkPipeline libraries[4]{};
    VkPipelineLibraryCreateInfoKHR library_list{};
    VkGraphicsPipelineCreateInfo info{};
    // a stage's specialization (the interlock path's fragment shader)
    VkSpecializationInfo spec{};
    VkSpecializationMapEntry spec_entries[16]{};
    uint32_t spec_data[16]{};
    // the multisample state's sample locations (the interlock path's)
    VkPipelineSampleLocationsStateCreateInfoEXT sample_locations{};
    VkSampleLocationEXT locations[4]{};

    explicit PipelineDesc(const VkGraphicsPipelineCreateInfo &from) : info(from) {
        info.stageCount = std::min<uint32_t>(from.stageCount, uint32_t(std::size(stages)));
        std::copy_n(from.pStages, info.stageCount, stages);
        info.pStages = stages;
        for (uint32_t i = 0; i < info.stageCount; i++) {
            const VkSpecializationInfo *s = stages[i].pSpecializationInfo;
            if (!s) continue;
            spec = *s;
            spec.mapEntryCount = std::min<uint32_t>(spec.mapEntryCount, 16);
            spec.dataSize = std::min<size_t>(spec.dataSize, sizeof(spec_data));
            std::copy_n(s->pMapEntries, spec.mapEntryCount, spec_entries);
            std::memcpy(spec_data, s->pData, spec.dataSize);
            spec.pMapEntries = spec_entries, spec.pData = spec_data;
            stages[i].pSpecializationInfo = &spec;
        }
        // the chain, each structure copied and linked again in the same order
        const void **tail = &info.pNext;
        for (auto *in = static_cast<const VkBaseInStructure *>(from.pNext); in; in = in->pNext) {
            switch (in->sType) {
            case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
                rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo *>(in);
                rendering.colorAttachmentCount = std::min<uint32_t>(rendering.colorAttachmentCount, 4);
                std::copy_n(rendering.pColorAttachmentFormats, rendering.colorAttachmentCount, color_formats);
                rendering.pColorAttachmentFormats = color_formats;
                *tail = &rendering, tail = &rendering.pNext;
                break;
            case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
                library_part = *reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT *>(in);
                *tail = &library_part, tail = &library_part.pNext;
                break;
            case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
                library_list = *reinterpret_cast<const VkPipelineLibraryCreateInfoKHR *>(in);
                library_list.libraryCount = std::min<uint32_t>(library_list.libraryCount, 4);
                std::copy_n(library_list.pLibraries, library_list.libraryCount, libraries);
                library_list.pLibraries = libraries;
                *tail = &library_list, tail = &library_list.pNext;
                break;
            default: break; // nothing else is chained
            }
        }
        *tail = nullptr;
        if (own(info.pVertexInputState, vertex_input)) {
            // one binding and one attribute at most (tessellation's control
            // points, the rectangles' guest indices)
            vertex_input.vertexBindingDescriptionCount =
                std::min<uint32_t>(vertex_input.vertexBindingDescriptionCount, 1);
            vertex_input.vertexAttributeDescriptionCount =
                std::min<uint32_t>(vertex_input.vertexAttributeDescriptionCount, 1);
            std::copy_n(vertex_input.pVertexBindingDescriptions, vertex_input.vertexBindingDescriptionCount,
                        vertex_bindings);
            std::copy_n(vertex_input.pVertexAttributeDescriptions, vertex_input.vertexAttributeDescriptionCount,
                        vertex_attributes);
            vertex_input.pVertexBindingDescriptions = vertex_bindings;
            vertex_input.pVertexAttributeDescriptions = vertex_attributes;
        }
        own(info.pInputAssemblyState, assembly);
        own(info.pTessellationState, tessellation);
        own(info.pViewportState, viewport);
        own(info.pRasterizationState, raster);
        if (own(info.pMultisampleState, multisample) && multisample.pNext) {
            const auto *in = static_cast<const VkBaseInStructure *>(multisample.pNext);
            multisample.pNext = nullptr;
            if (in->sType == VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT) {
                sample_locations = *reinterpret_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(in);
                VkSampleLocationsInfoEXT &l = sample_locations.sampleLocationsInfo;
                l.sampleLocationsCount = std::min<uint32_t>(l.sampleLocationsCount, 4);
                std::copy_n(l.pSampleLocations, l.sampleLocationsCount, locations);
                l.pSampleLocations = locations;
                sample_locations.pNext = nullptr;
                multisample.pNext = &sample_locations;
            }
        }
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

// A pipeline (or a pipeline library) the renderer asked for: made (state 1,
// the pipeline null when the driver failed) or being made (0). While a
// pipeline is made on the workers, its draws may be drawn by a stand-in
// (Options::pipeline_stand_in): a placeholder or a pipeline linked from the
// libraries of its parts, which the renderer's thread owns.
struct PipelineEntry {
    std::atomic<int> state{0};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline stand_in = VK_NULL_HANDLE;
    bool stand_in_failed = false; // the driver failed it: not tried again
    bool queued = false;          // the whole pipeline given to the workers
    // when a draw first asked for it, until one is first drawn with it or
    // its stand-in (Report::pipeline_wait_*); zero once counted
    std::chrono::steady_clock::time_point asked{};
    // when a worker finished making it (zero: not made on a worker)
    std::chrono::steady_clock::time_point made{};
    // the libraries it is linked from (vertex input, pre-rasterization,
    // fragment shader, fragment output; the last two null when rasterization
    // is discarded), once asked for
    PipelineEntry *libraries[4] = {};
};

// What a pipeline made on the workers is to the draws waiting for it.
enum class PipelinePriority {
    first,  // a library a stand-in waits for: ahead of everything queued
    normal, // a whole pipeline its draws wait for
    later,  // a whole pipeline that will replace a linked stand-in drawing as
            // it does: only when nothing else waits, on half the workers at
            // most, so the libraries are never kept waiting by it
};

class PipelineWorkers {
public:
  PipelineWorkers(VkDevice device, VkPipelineCache cache, uint32_t threads)
      : dev_(device), cache_(cache), max_later_(std::max(threads / 2, 1u)) {
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
    void queue(PipelineEntry *entry, std::unique_ptr<PipelineDesc> desc, PipelinePriority priority) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (priority == PipelinePriority::first)
                queue_.emplace_front(entry, std::move(desc));
            else if (priority == PipelinePriority::normal)
                queue_.emplace_back(entry, std::move(desc));
            else
                later_.emplace_back(entry, std::move(desc));
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
        done_.wait(lock, [&] { return queue_.empty() && later_.empty() && busy_ == 0; });
    }
    // the time the workers spent making pipelines, and how many they made
    double seconds() const { return double(nanoseconds_.load()) * 1e-9; }
    uint64_t made() const { return made_.load(); }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            work_.wait(lock, [&] { return stop_ || !queue_.empty() || (!later_.empty() && busy_later_ < max_later_); });
            if (stop_) return;
            const bool late = queue_.empty();
            auto &from = late ? later_ : queue_;
            auto [entry, desc] = std::move(from.front());
            from.pop_front();
            busy_++;
            if (late) busy_later_++;
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
            entry->made = std::chrono::steady_clock::now();
            entry->state.store(1, std::memory_order_release);
            busy_--;
            if (late) {
                busy_later_--;
                work_.notify_one(); // another one may be taken now
            }
            done_.notify_all();
        }
    }

    VkDevice dev_;
    VkPipelineCache cache_;
    std::mutex mutex_;
    std::condition_variable work_, done_;
    std::deque<std::pair<PipelineEntry *, std::unique_ptr<PipelineDesc>>> queue_, later_;
    uint32_t busy_ = 0, busy_later_ = 0;
    const uint32_t max_later_;
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
        return with_processor_counts(report_);
    }
    bool try_report(Report &out) const override {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock) return false;
        out = with_processor_counts(report_);
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
                        uint32_t primitive, bool tessellated) const;
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
    void use(const std::string &what) { report_.used[what]++; }
    // the report with what the processor counted of the packets the renderer
    // does not see (Report::used)
    Report with_processor_counts(Report r) const {
        const gpu::Counts &n = processor_.counts();
        auto put = [&](const char *what, uint64_t count) {
            if (count) r.used[what] = count;
        };
        put("EVENT_WRITE_EXT packets (screen extent reports)", n.opcodes[pm4::EVENT_WRITE_EXT].load());
        put("VIZ_QUERY packets", n.opcodes[pm4::VIZ_QUERY].load());
        put("draws with no valid index source (not drawn)", n.failed_draws.load());
        return r;
    }

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
    // a translated vertex shader's module in a host form; null when the
    // translator cannot make it
    VkShaderModule vertex_module(Shader &s, const shader::VertexOptions &options, std::string &why);
    VkShaderModule pixel_module(Shader &s, const shader::PixelOptions &options, std::string &why);
    // the depth-only draws' pixel shaders that convert D24FS8 depth
    // (Options::depth_float24_convert_in_pixel_shader): towards zero, nearest even
    VkShaderModule depth_float24_fs_[2] = {};
    // the pixel shader of the placeholder pipelines (Options::pipeline_stand_in)
    VkShaderModule placeholder_fs_ = VK_NULL_HANDLE;
    RenderTarget *render_target(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format);
    VkFormat depth_vk_format(uint32_t format) const;
    Texture *texture(const uint32_t fetch[6], std::string &why);
    // the place in its table of the view a constant reads the texture
    // through (made the first time), with its signed components or not; 0
    // when it cannot be made (why says)
    uint32_t texture_view(Texture &t, uint32_t guest_swizzle, bool is_signed, std::string &why);
    // The texture cache's lifetime (Xenia's texture_cache.cc): textures by
    // last use, the least recent first; destroyed by the memory limits once
    // the GPU is done with their last use, their places in the tables given
    // back then and only then.
    uint64_t submission_serial() const { return slots_[current_].recording ? slots_[current_].serial : next_serial_; }
    void use(Texture &t, uint64_t serial);
    void unlink(Texture &t);
    void apply_texture_limits();
    bool make_room(uint32_t kind, uint32_t needed);
    void destroy_texture(Texture *t);
    void release_index(uint32_t kind, uint32_t index);
    // a resolve target's slot given back and its view destroyed (the GPU done with them)
    void release_target(uint32_t target, VkImageView view) {
        if (view) vkDestroyImageView(dev_, view, nullptr);
        if (target & 0x7F) free_targets_[(target >> 7) & 1].push_back((target & 0x7F) - 1);
    }
    // the textures (up to two) a resolve to this destination feeds, written
    // by it as well (resolve targets not loaded again since): how many
    // (and the rows of texels the destination starts at in them, the same in both)
    uint32_t resolve_targets(const resolve::Info &info, Texture *targets[2], uint32_t &extra_rows);
    uint32_t take_index(uint32_t kind);
    uint32_t room(uint32_t kind) const {
        return uint32_t(free_indices_[kind].size()) + (capacity_[kind] - next_index_[kind]);
    }
    void count_texture_bytes(VkDeviceSize add, VkDeviceSize remove);
    VkDeviceSize image_bytes(const vk::Image &image) const;
    // Textures by the 4 MB of physical memory their ranges span (Xenia's
    // shared memory watch buckets, kWatchBucketSizeLog2), so that a resolve
    // or a memory export finds the textures it wrote without looking at
    // every one.
    static constexpr uint32_t TEXTURE_BUCKET_SHIFT = 22;
    std::vector<Texture *> texture_buckets_[GuestMemory::SIZE >> TEXTURE_BUCKET_SHIFT];
    void index_texture(Texture &t);
    void unindex_texture(Texture &t);
    // the textures over what the GPU wrote, loaded again (from the mirror) at
    // their next use (Xenia's FireWatches with invalidated_by_gpu); but for
    // written_too, resolve targets the GPU wrote as well (null ones none)
    void textures_written_by_gpu(const std::vector<Range> &written, Texture *const written_too[2] = nullptr);
    Texture *lru_oldest_ = nullptr, *lru_newest_ = nullptr;
    VkDeviceSize texture_bytes_ = 0;
    std::vector<uint32_t> free_indices_[3]; // each table's places given back
    uint64_t limits_applied_at_ = 0;        // done_serial_ when the limits were last applied
    bool gpu_load(Texture &t, const vk::Image &image, const texture::Plan &plan, bool fresh, bool upload);
    bool cpu_upload(const vk::Image &target, const texture::Image &image, bool fresh);
    texture::Support support_; // what the device samples (texture_support)
    uint32_t sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides);
    // The keys of a pipeline's four parts, for its libraries (vertex input,
    // pre-rasterization, fragment shader, fragment output); a part the
    // pipeline has not (rasterization discarded) has none.
    struct PipelineParts {
        std::vector<uint32_t> keys[4];
    };
    // the pipeline of a key, made now or found made; null when the driver
    // failed, or (pending) when worker threads are making it and the draw may
    // not wait (async); or its stand-in (Options::pipeline_stand_in) while
    // they make it
    VkPipeline pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info, bool async,
                        bool &pending, bool &stand_in);
    // ---- the shader and pipeline storage (Options::shader_storage) ----
    std::unique_ptr<ShaderStorage> storage_;
    // The modules' names in the storage's descriptions and the libraries'
    // keys, the same from run to run: a guest shader's hash and host form
    // (Shader::variants' key, 0 for Shader::module), or one of the
    // renderer's own (OwnModule)
    struct ModuleName {
        uint32_t kind = 0; // 1 a guest shader's, 2 the renderer's own
        uint32_t form = 0;
        uint64_t id = 0;
    };
    std::unordered_map<VkShaderModule, ModuleName> module_names_;
    // the libraries' and pipelines' keys the build step made into the
    // pipeline cache for this device and driver (hashes)
    std::unordered_set<uint64_t> built_libraries_, built_pipelines_;
    void open_storage();
    // a pipeline's description in the storage, empty when one of its modules
    // has no name; and back, null (why) when it cannot be made here
    std::vector<uint32_t> describe(const VkGraphicsPipelineCreateInfo &info) const;
    std::unique_ptr<PipelineDesc> undescribe(std::span<const uint32_t> words, std::string &why);
    void module_words(VkShaderModule m, std::vector<uint32_t> &out) const;
    // a pipeline's description as it is made: the values of its dynamic
    // states set to one each (canonicalize)
    static std::unique_ptr<PipelineDesc> canonical(const VkGraphicsPipelineCreateInfo &info);
    // its libraries' keys
    void library_keys(const VkGraphicsPipelineCreateInfo &info, PipelineParts &parts) const;

public:
    bool precompile(const PrecompileOptions &options, PrecompileReport &out, std::string &error);
    void set_storage_path(const std::filesystem::path &path) { options_.shader_storage = path; }

private:
    // a part's library, made now (async: those with shaders queued for the
    // workers, ahead of the whole pipelines); null for a part without a key
    PipelineEntry *library(uint32_t part, const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &whole,
                           bool async);
    // the entry's pipeline linked from its libraries (fast linking), null
    // while one of them is not made yet or when one failed (stand_in_failed)
    VkPipeline link(PipelineEntry &e);
    // the placeholder of a whole pipeline: its pixel shader the placeholder's
    VkPipeline placeholder(const VkGraphicsPipelineCreateInfo &info);
    // a stand-in replaced by its whole pipeline, destroyed once the GPU is
    // done with what drew with it
    void retire_stand_in(PipelineEntry &e);
    std::unique_ptr<PipelineWorkers> pipeline_workers_; // with Options::async_pipelines
    // the device's pipeline libraries, used (Options::pipeline_stand_in library
    // with async_pipelines, or linked_pipelines_only)
    bool use_libraries_ = false;
    Options::PipelineStandIn stand_in_ = Options::PipelineStandIn::skip; // the option, as the device allows it
    std::map<std::vector<uint32_t>, std::unique_ptr<PipelineEntry>> libraries_;
    // extended dynamic state 3's blend state at the draw (vk::Device::dynamic_blend)
    PFN_vkCmdSetColorBlendEnableEXT set_blend_enable_ = nullptr;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_equation_ = nullptr;
    PFN_vkCmdSetColorWriteMaskEXT set_write_mask_ = nullptr;
    // hitches (Report::pipeline_hitches, swap_gaps_*): the pipeline time at
    // the last swap, and when it came
    double pipeline_seconds_at_swap_ = 0;
    std::chrono::steady_clock::time_point last_swap_time_{};

    // ---- the port's frames (native_render.h) ----
    // The port's settings as the last swap took them (copied again when
    // their version moves), whether this frame's passes are skipped (the
    // port drew the frame before), and the output image it draws into.
    uint64_t native_version_ = 0;
    native::detail::Settings native_settings_;
    bool native_suppress_ = false;
    vk::Image native_output_;
    VkImageView native_output_view_ = VK_NULL_HANDLE;
    class NativeFrame;
    // at a swap: the port's frame drawn (true), or not; held: this frame's
    // passes were skipped and the port did not draw it, so nothing is shown
    bool native_frame(uint64_t number, bool &held);
    bool native_output(uint32_t width, uint32_t height);
    // a draw or a resolve while suppressing: false when it is skipped
    bool native_keep(native::PassInfo &pass);
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
        bool suppressed = false; // a draw the port's frame skipped counted for it (native_render.h)
    };
    static constexpr uint32_t OCCLUSION_QUERIES = 4096; // a slot's
    uint64_t zpd_current_ = 0;                          // the report draws count for (0: none)
    uint32_t zpd_active_ = UINT32_MAX;                  // the query open in the render pass
    std::map<uint64_t, ZpdCount> zpd_counts_;
    std::vector<std::pair<uint64_t, uint64_t>> zpd_resolved_; // for zpd_poll
    void zpd_answer(uint64_t handle, ZpdCount &count) {
        report_.zpd_reports++;
        uint64_t samples = count.uncounted ? std::max<uint64_t>(count.samples, 1) : count.samples;
        if (count.suppressed) {
            samples = std::max<uint64_t>(samples, native_settings_.occlusion_samples);
            native::detail::bump(native::detail::atomic_counters().occlusion_faked);
        }
        zpd_resolved_.emplace_back(handle, samples);
        zpd_counts_.erase(handle);
    }
    void end_query() {
        if (zpd_active_ == UINT32_MAX) return;
        // (the interlock path's counters are no host queries)
        if (!fsi_) vkCmdEndQuery(slots_[current_].cmd, slots_[current_].occlusion, zpd_active_);
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
        if (fsi_ || !zpd_current_ || zpd_active_ != UINT32_MAX) return;
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
        if (fsi_) {
            // the shaders' counters (FsiConstants::zpd_index), read back as the slot ended
            const uint32_t *counts = reinterpret_cast<const uint32_t *>(zpd_readback_.mapped) +
                                     size_t(&s - slots_) * OCCLUSION_QUERIES;
            std::copy(counts, counts + samples.size(), samples.begin());
        } else {
            vkGetQueryPoolResults(dev_, s.occlusion, 0, uint32_t(samples.size()), samples.size() * 8, samples.data(),
                                  8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        }
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
    // A request's memo (the draws' vertex streams): the ranges last found
    // current in the mirror, at which write count and mirror epoch. While
    // nothing was written since, the same ranges are current still, and a
    // draw with the streams of the draw before it checks no page.
    struct RequestMemo {
        std::vector<Range> ranges;
        uint32_t count = 0;
        uint64_t epoch = 0;
    };
    VkDeviceSize request(const std::vector<Range> &ranges, RequestMemo *memo = nullptr);
    RequestMemo vertex_memo_;
    uint64_t mirror_epoch_ = 1; // advanced where pages of the mirror stop being current but for a write
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
    // the samplers by their parameters: the sampler, its slot in the
    // bindless table, the last submission that used it
    struct SamplerEntry {
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t index = 0;
        uint64_t used_serial = 0;
    };
    std::map<std::array<uint32_t, 8>, SamplerEntry> samplers_;
    std::vector<uint32_t> free_samplers_; // slots of samplers destroyed, to reuse
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
    // rectangle and point lists expanded in the vertex shader rather than by
    // the geometry shaders (without them, or with XF_VK_EXPAND_IN_VS=1)
    bool expand_in_vs_ = false;
    // tessellation, where the device has it: the host vertex shader, and the
    // hull shaders in TESSELLATION_1CP to TESSELLATION_ADAPTIVE_QUAD's order
    VkShaderModule tessellation_vs_ = VK_NULL_HANDLE, tessellation_hs_[5] = {};
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
    // the resolves' set 1: the textures they write as well, through 32- and
    // 64-bit unsigned views (RESOLVE_TARGETS each, updated after binding),
    // their free slots
    VkDescriptorSetLayout target_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool target_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet target_set_ = VK_NULL_HANDLE;
    bool resolve_targets_ = false;
    // Every texture loaded on the GPU from the mirror (texture_load.comp,
    // guest memory's pages put there first), none decoded on the CPU: the
    // renderer's thread spends nothing decoding (docs/renderer.md, "Vertex
    // data and the CPU's decodes"). XF_TEXTURE_LOADS=cpu decodes on the CPU
    // what memory the GPU did not write, as before.
    bool texture_loads_on_gpu_ = [] {
        const char *v = std::getenv("XF_TEXTURE_LOADS");
        return !(v && std::string(v) == "cpu");
    }();
    uint32_t next_target_[2] = {};
    std::vector<uint32_t> free_targets_[2];

    // EDRAM: the render targets' contents move through it when another
    // target takes over its tiles (Xenia's ownership transfers); it starts
    // as zeros, or as a trace's snapshot
    void acquire(RenderTarget *t, uint32_t rows);
    bool edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles);
    bool edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows);
    VkDeviceSize edram_mask(const std::vector<uint32_t> &tiles);
    EdramTarget edram_target(const RenderTarget &t, uint32_t height) const {
        // (20e4 rounded only where the host's depth is not already 20e4, as
        // Xenia's dump of float24 depth)
        bool round = options_.depth_float24_round && !options_.depth_float24_convert_in_pixel_shader;
        uint32_t flags = t.depth ? (host_depth_kept(t.format) ? 1u : 0u) | (round ? 2u : 0u)
                                 : (t.gamma_linear ? 4u : 0u) | (t.raw ? 8u : 0u);
        return {t.base, t.pitch_tiles,     t.msaa,           t.format, t.width,
                height, t.depth ? 1u : 0u, t.wide ? 1u : 0u, 8u,       flags};
    }
    // Whether a depth format is encoded differently on the host, so that its
    // 32-bit depth is kept beside EDRAM across transfers (Xenia's
    // IsHostDepthEncodingDifferent): D24FS8 always (its 20e4 is kept as a
    // 32-bit float), D24S8 where the device has no 24-bit unorm depth.
    // (D24FS8 converted to 20e4 in the pixel shaders keeps nothing the 24
    // bits do not: Xenia's IsHostDepthEncodingDifferent)
    bool host_depth_kept(uint32_t format) const {
        return format == 1 ? !options_.depth_float24_convert_in_pixel_shader : d24s8_ != VK_FORMAT_D24_UNORM_S8_UINT;
    }
    VkPipeline load_pipeline(const RenderTarget &t);
    VkDescriptorSet edram_set(uint32_t which);
    std::vector<RenderTarget *> owners_ = std::vector<RenderTarget *>(EDRAM_TILES, nullptr); // null: EDRAM holds it
    // EDRAM, and beside it the host's 32-bit depth of the last D24FS8 target
    // (and D24S8 where the host keeps it as a float) to store each sample,
    // in the same layout (edram.glsl's host_depth_offset)
    vk::Buffer edram_, host_depth_;
    VkDescriptorSetLayout edram_set_layouts_[2] = {}; // store, load
    VkPipelineLayout edram_layouts_[2] = {};
    std::vector<VkDescriptorPool> edram_pools_;
    VkDescriptorSet edram_load_set_ = VK_NULL_HANDLE;
    VkPipeline store_pipelines_[2] = {}; // 1x, multisampled
    VkShaderModule fullscreen_vs_ = VK_NULL_HANDLE, load_fs_[3] = {}; // color, depth, depth and stencil
    VkShaderModule load_uint_fs_ = VK_NULL_HANDLE;                    // a float target's bits (RenderTarget::raw)
    // float targets moved through EDRAM by their bits where the device
    // samples and renders their UINT views at the sample counts below
    // (XF_VK_NO_RAW_FLOAT_TARGETS=1 turns it off: moved by value then)
    bool raw_float_targets_ = false;
    VkSampleCountFlags integer_samples_ = 0;
    std::map<std::pair<VkFormat, uint32_t>, VkPipeline> load_pipelines_;
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
    // 8_8_8_8_GAMMA targets kept linear as RGBA16 unorm (RenderTarget::gamma_linear)
    bool gamma_unorm16_ = false;

    // ---- the interlock path (Options::render_target_path fsi) ----
    // 0: host render targets; 1 under pixel interlock, 2 under sample
    // interlock. The pixel shaders read and write edram_ themselves (set 0
    // binding 5), with the state of FsiConstants (binding 4) or of the
    // pipeline's specialization; no render targets are made, resolves read
    // EDRAM, clears write it, and occlusion counts are the shaders' (binding
    // 6, a slot's share each, read back at its end).
    uint32_t fsi_ = 0;
    bool fsi_2x_as_4x_ = false;         // the guest's 2x rasterized as 4x (no 2x without attachments)
    VkShaderModule fsi_depth_fs_ = VK_NULL_HANDLE; // depth and stencil alone (no pixel shader)
    VkPipeline clear_pipeline_ = VK_NULL_HANDLE;
    vk::Buffer zpd_counters_, zpd_readback_;
    FsiConstants last_fsi_{};
    VkDeviceSize last_fsi_at_ = NONE;
    // a target's place in EDRAM, as a resolve reads it: a RenderTarget
    // without an image
    RenderTarget edram_view(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format) const;
    void fsi_clear(const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t lo, uint32_t hi);
    uint32_t fsi_zpd_index(); // the counter the draw counts in, ~0 none
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
    for (auto &[k, s] : shaders_) {
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
        for (auto &[v, m] : s->variants)
            if (m) vkDestroyShaderModule(dev_, m, nullptr);
    }
    for (auto *table : {&pipelines_, &libraries_})
        for (auto &[k, p] : *table)
            for (VkPipeline pipe : {p->pipeline, p->stand_in})
                if (pipe) vkDestroyPipeline(dev_, pipe, nullptr);
    for (auto &[k, s] : samplers_) vkDestroySampler(dev_, s.sampler, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (auto &[k, t] : targets_) d_.destroy(t->image);
    for (auto &[k, t] : textures_) {
        for (const TextureView &v : t->views) vkDestroyImageView(dev_, v.view, nullptr);
        if (t->target_view) vkDestroyImageView(dev_, t->target_view, nullptr);
        d_.destroy(t->image);
        if (t->signed_image.image) d_.destroy(t->signed_image);
    }
    for (auto &im : dummy_) d_.destroy(im);
    if (native_output_view_) vkDestroyImageView(dev_, native_output_view_, nullptr);
    if (native_output_.image) d_.destroy(native_output_);
    if (dummy_sampler_) vkDestroySampler(dev_, dummy_sampler_, nullptr);
    for (VkShaderModule m : {rectangle_gs_, point_gs_, tessellation_vs_})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (VkShaderModule m : tessellation_hs_)
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto &[k, p] : load_pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (VkPipeline p : {store_pipelines_[0], store_pipelines_[1], resolve_pipeline_, load_pipeline_, clear_pipeline_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : {fullscreen_vs_, load_fs_[0], load_fs_[1], load_fs_[2], load_uint_fs_, depth_float24_fs_[0],
                             depth_float24_fs_[1], placeholder_fs_, fsi_depth_fs_})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (VkDescriptorPool p : edram_pools_) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkDescriptorPool p : {pool0_, bindless_pool_, compute_pool_, target_pool_})
        if (p) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkPipelineLayout l : {edram_layouts_[0], edram_layouts_[1], layout_, resolve_layout_, load_layout_})
        if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (VkDescriptorSetLayout l : {edram_set_layouts_[0], edram_set_layouts_[1], set_layouts_[0], set_layouts_[1],
                                    compute_set_layout_, target_set_layout_})
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    for (vk::Buffer *b : {&edram_, &host_depth_, &mirror_, &arena_, &scratch_, &load_scratch_, &zpd_counters_, &zpd_readback_})
        d_.destroy(*b);
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
// game's pipelines are compiled by the driver once, not at each run (a UE3
// game's title screen made 24 in its first frame, 2.5 s on this thread).
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
    use_libraries_ = d_.pipeline_library() &&
                     (options_.linked_pipelines_only ||
                      (pipeline_workers_ && options_.pipeline_stand_in == Options::PipelineStandIn::library));
    if (options_.linked_pipelines_only && !d_.pipeline_library())
        return error = "linked pipelines asked for, and the device has no graphics pipeline libraries", false;
    // without the device's libraries the draws are skipped, as Xenia's D3D12
    // backend skips them (the placeholder, an option, measured worse: every
    // new pipeline compiled twice, once on the renderer's thread)
    stand_in_ = options_.pipeline_stand_in;
    if (stand_in_ == Options::PipelineStandIn::library && !d_.pipeline_library())
        stand_in_ = Options::PipelineStandIn::skip;
    if (d_.dynamic_blend()) {
        set_blend_enable_ =
            reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(vkGetDeviceProcAddr(dev_, "vkCmdSetColorBlendEnableEXT"));
        set_blend_equation_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(
            vkGetDeviceProcAddr(dev_, "vkCmdSetColorBlendEquationEXT"));
        set_write_mask_ =
            reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(vkGetDeviceProcAddr(dev_, "vkCmdSetColorWriteMaskEXT"));
        if (!set_blend_enable_ || !set_blend_equation_ || !set_write_mask_)
            return error = "extended dynamic state 3 enabled without its commands", false;
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
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              true);
    const VkBufferUsageFlags scratch_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    scratch_ = d_.create_buffer(16u << 20, scratch_usage, false);
    load_scratch_ = d_.create_buffer(16u << 20, scratch_usage, false);
    if (!mirror_.buffer || !arena_.buffer || !scratch_.buffer || !load_scratch_.buffer)
        return error = "could not make the renderer's buffers (512 MB of device memory for guest memory's mirror)", false;

    // The render target path (Options::render_target_path): the interlock
    // one where asked for (XF_RENDER_TARGET_PATH=fsi or host forces either)
    // and the device has fragment shader interlock, host render targets
    // otherwise; automatic is host (docs/renderer.md, "Exact EDRAM through
    // fragment shader interlock", has the measurements). The guest's 2x is
    // rasterized as 4x where a render pass without attachments takes no 2x.
    {
        if (render_target_path(d_, options_) == Options::RenderTargetPath::fsi) fsi_ = d_.fragment_shader_interlock();
        fsi_2x_as_4x_ = fsi_ && !(d_.no_attachments_samples() & VK_SAMPLE_COUNT_2_BIT);
        if (const char *e = std::getenv("XF_FSI_DYNAMIC"); e && *e && *e != '0') options_.fsi_specialized = false;
    }
    // D24S8 as a 32-bit float depth where the device has no 24-bit unorm
    // depth (AMD), or where XF_DEPTH_FALLBACK=1 asks for it, to test that path
    const char *depth_fallback = std::getenv("XF_DEPTH_FALLBACK");
    if (!d_.format_supported(d24s8_, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (depth_fallback && depth_fallback[0] == '1'))
        d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    // 8_8_8_8_GAMMA targets kept linear as RGBA16 unorm, blended in linear
    // (Xenia's gamma_render_target_as_unorm16, on by default), where the
    // device samples, renders to and blends that format; without it, or with
    // the option off (XF_VK_NO_GAMMA_UNORM16=1 too), the pixel shader writes
    // the gamma curve into an RGBA8 target, as Xenia does then
    {
        const char *off = std::getenv("XF_VK_NO_GAMMA_UNORM16");
        gamma_unorm16_ =
            options_.gamma_render_target_as_unorm16 && !(off && off[0] && off[0] != '0') &&
            d_.format_supported(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    }

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
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // the guest's vertex shader run as a domain shader reads all of it too
    if (d_.tessellation_shader()) stages |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    // (4 to 6 the interlock path's: its constants, EDRAM, the occlusion
    // counters; bound in either path, read only by its pixel shaders)
    VkDescriptorSetLayoutBinding b0[7] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
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
    set_info.bindingCount = 7;
    set_info.pBindings = b0;
    if (vkCreateDescriptorSetLayout(dev_, &set_info, nullptr, &set_layouts_[0]) != VK_SUCCESS)
        return error = "vkCreateDescriptorSetLayout failed", false;
    set_info.bindingCount = 4;
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
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
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
    {
        const char *in_vs = std::getenv("XF_VK_EXPAND_IN_VS");
        expand_in_vs_ =
            !d_.geometry_shader() || options_.expand_in_vertex_shader || (in_vs && in_vs[0] && in_vs[0] != '0');
    }
    if (d_.tessellation_shader()) {
        tessellation_vs_ = module(TESSELLATION_VS, sizeof(TESSELLATION_VS));
        const std::pair<const uint32_t *, size_t> hull[5] = {
            {TESSELLATION_1CP, sizeof(TESSELLATION_1CP)},
            {TESSELLATION_3CP, sizeof(TESSELLATION_3CP)},
            {TESSELLATION_4CP, sizeof(TESSELLATION_4CP)},
            {TESSELLATION_ADAPTIVE_TRIANGLE, sizeof(TESSELLATION_ADAPTIVE_TRIANGLE)},
            {TESSELLATION_ADAPTIVE_QUAD, sizeof(TESSELLATION_ADAPTIVE_QUAD)}};
        for (uint32_t k = 0; k < 5; k++) tessellation_hs_[k] = module(hull[k].first, hull[k].second);
        if (!tessellation_vs_ || std::count(std::begin(tessellation_hs_), std::end(tessellation_hs_), VkShaderModule()))
            return error = "could not make the tessellation shaders", false;
    }

    // EDRAM: the buffer, store (compute) and load (draw) pipelines; the tile
    // masks come from the arena
    edram_ = d_.create_buffer(EDRAM_BYTES,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              false);
    if (!edram_.buffer) return error = "could not make the EDRAM buffer", false;
    // the host's depth beside it: D24FS8's, then D24S8's where the host keeps
    // that as a float too
    host_depth_ = d_.create_buffer(EDRAM_BYTES * (host_depth_kept(0) ? 2 : 1),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    if (!host_depth_.buffer) return error = "could not make the host depth buffer", false;
    // the interlock path's occlusion counters: a slot's share each, zeroed as
    // it begins recording, copied for the CPU as it ends
    zpd_counters_ = d_.create_buffer(VkDeviceSize(SLOTS) * OCCLUSION_QUERIES * 4,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     false);
    zpd_readback_ = d_.create_buffer(VkDeviceSize(SLOTS) * OCCLUSION_QUERIES * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     true, true);
    if (!zpd_counters_.buffer || !zpd_readback_.buffer) return error = "could not make the occlusion counters", false;
    for (int k = 0; k < 2; k++) {
        VkDescriptorBufferInfo buffers[3] = {{arena_.buffer, 0, sizeof(FsiConstants)},
                                             {edram_.buffer, 0, VK_WHOLE_SIZE},
                                             {zpd_counters_.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t b = 0; b < 3; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = draw_sets_[k];
            writes[b].dstBinding = 4 + b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(dev_, 3, writes, 0, nullptr);
    }
    if (fsi_) {
        fsi_depth_fs_ = fsi_ == 2 ? module(EDRAM_FSI_DEPTH_SAMPLE, sizeof(EDRAM_FSI_DEPTH_SAMPLE))
                                  : module(EDRAM_FSI_DEPTH_PIXEL, sizeof(EDRAM_FSI_DEPTH_PIXEL));
        if (!fsi_depth_fs_) return error = "could not make the interlock path's depth shader", false;
    }
    {
        VkDescriptorSetLayoutBinding store[5] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorBindingFlags store_flags[5] = {0, 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0};
        VkDescriptorSetLayoutBindingFlagsCreateInfo store_binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        store_binding_flags.bindingCount = 5;
        store_binding_flags.pBindingFlags = store_flags;
        VkDescriptorSetLayoutBinding load[3] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.pNext = &store_binding_flags;
        info.bindingCount = 5;
        info.pBindings = store;
        vkCreateDescriptorSetLayout(dev_, &info, nullptr, &edram_set_layouts_[0]);
        info.pNext = nullptr;
        info.bindingCount = 3;
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
        load_uint_fs_ = module(EDRAM_LOAD_UINT, sizeof(EDRAM_LOAD_UINT));
        if (!load_uint_fs_) return error = "could not make the EDRAM load shaders", false;
        {
            // integer images at the guest's sample counts: sampled, and
            // rendered to (Vulkan 1.2's framebufferIntegerColorSampleCounts)
            VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p.pNext = &p12;
            vkGetPhysicalDeviceProperties2(d_.physical(), &p);
            integer_samples_ =
                p.properties.limits.sampledImageIntegerSampleCounts & p12.framebufferIntegerColorSampleCounts;
            const char *off = std::getenv("XF_VK_NO_RAW_FLOAT_TARGETS");
            raw_float_targets_ = !(off && off[0] && off[0] != '0');
        }
        depth_float24_fs_[0] = module(DEPTH_FLOAT24, sizeof(DEPTH_FLOAT24));
        depth_float24_fs_[1] = module(DEPTH_FLOAT24_ROUND, sizeof(DEPTH_FLOAT24_ROUND));
        if (!depth_float24_fs_[0] || !depth_float24_fs_[1])
            return error = "could not make the float24 depth shaders", false;
        placeholder_fs_ = module(PLACEHOLDER, sizeof(PLACEHOLDER));
        if (!placeholder_fs_) return error = "could not make the placeholder pixel shader", false;
        edram_load_set_ = edram_set(1);
        if (!edram_load_set_) return error = "could not make the EDRAM load's descriptors", false;
        VkDescriptorBufferInfo buffers[3] = {
            {edram_.buffer, 0, VK_WHOLE_SIZE},
            {arena_.buffer, 0, EDRAM_TILES * 4},
            {host_depth_.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t b = 0; b < 3; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = edram_load_set_;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType =
                b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(dev_, 3, writes, 0, nullptr);
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
        // the resolves' set 1, the textures they write as well (resolve.comp's
        // targets32 and targets64): partially bound, updated after binding
        // where the device can (else never written, the resolves writing
        // memory alone)
        const char *targets_env = std::getenv("XF_RESOLVE_TARGETS");
        resolve_targets_ = d_.storage_image_update_after_bind() && !options_.no_resolve_targets &&
                           !(targets_env && targets_env[0] == '0');
        VkDescriptorSetLayoutBinding target_bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RESOLVE_TARGETS, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RESOLVE_TARGETS, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorBindingFlags target_flags[2];
        for (VkDescriptorBindingFlags &f : target_flags)
            f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                (resolve_targets_ ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                                  : 0);
        VkDescriptorSetLayoutBindingFlagsCreateInfo target_flag_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        target_flag_info.bindingCount = 2;
        target_flag_info.pBindingFlags = target_flags;
        VkDescriptorSetLayoutCreateInfo target_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        target_info.pNext = &target_flag_info;
        target_info.flags = resolve_targets_ ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        target_info.bindingCount = 2;
        target_info.pBindings = target_bindings;
        if (vkCreateDescriptorSetLayout(dev_, &target_info, nullptr, &target_set_layout_) != VK_SUCCESS)
            return error = "vkCreateDescriptorSetLayout failed", false;
        VkDescriptorPoolSize target_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * RESOLVE_TARGETS};
        VkDescriptorPoolCreateInfo target_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        target_pool.flags = resolve_targets_ ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
        target_pool.maxSets = 1;
        target_pool.poolSizeCount = 1;
        target_pool.pPoolSizes = &target_size;
        if (vkCreateDescriptorPool(dev_, &target_pool, nullptr, &target_pool_) != VK_SUCCESS)
            return error = "vkCreateDescriptorPool failed", false;
        VkDescriptorSetAllocateInfo target_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        target_alloc.descriptorPool = target_pool_;
        target_alloc.descriptorSetCount = 1;
        target_alloc.pSetLayouts = &target_set_layout_;
        if (vkAllocateDescriptorSets(dev_, &target_alloc, &target_set_) != VK_SUCCESS)
            return error = "vkAllocateDescriptorSets failed", false;
        uint32_t push_sizes[2] = {sizeof(ResolveConstants), sizeof(LoadConstants)};
        VkPipelineLayout *layouts[2] = {&resolve_layout_, &load_layout_};
        for (int k = 0; k < 2; k++) {
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_sizes[k]};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            const VkDescriptorSetLayout set_layouts[2] = {compute_set_layout_, target_set_layout_};
            layout.setLayoutCount = k == 0 ? 2 : 1; // the resolves' with their targets
            layout.pSetLayouts = set_layouts;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(dev_, &layout, nullptr, layouts[k]) != VK_SUCCESS)
                return error = "vkCreatePipelineLayout failed", false;
        }
        resolve_pipeline_ = compute_pipeline(RESOLVE, sizeof(RESOLVE), resolve_layout_);
        load_pipeline_ = compute_pipeline(TEXTURE_LOAD, sizeof(TEXTURE_LOAD), load_layout_);
        if (!resolve_pipeline_ || !load_pipeline_) return error = "could not make the resolve and load pipelines", false;
        // the interlock path's clears into EDRAM: the resolves' layout and
        // their EDRAM set (EDRAM at binding 0), edram.glsl's push constants
        static_assert(sizeof(EdramTarget) <= sizeof(ResolveConstants));
        if (fsi_ && !(clear_pipeline_ = compute_pipeline(EDRAM_CLEAR, sizeof(EDRAM_CLEAR), resolve_layout_)))
            return error = "could not make the EDRAM clear pipeline", false;
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
        VkDescriptorBufferInfo buffers[6] = {
            {scratch_.buffer, 0, VK_WHOLE_SIZE}, {mirror_.buffer, 0, VK_WHOLE_SIZE},
            {mirror_.buffer, 0, VK_WHOLE_SIZE},  {load_scratch_.buffer, 0, VK_WHOLE_SIZE},
            {edram_.buffer, 0, VK_WHOLE_SIZE},   {mirror_.buffer, 0, VK_WHOLE_SIZE}};
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
    vkCmdFillBuffer(c, host_depth_.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(c, mirror_.buffer, 0, VK_WHOLE_SIZE, 0);
    for (auto &im : dummy_) {
        begin_image(c, im.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, im.layers);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im.layers};
        vkCmdClearColorImage(c, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    sync();
    if (!fatal_.empty()) return error = fatal_, false;
    // the renderer's own modules' names in the storage's descriptions (fixed:
    // a new one goes last)
    const VkShaderModule own[] = {rectangle_gs_,       point_gs_,           tessellation_vs_,    tessellation_hs_[0],
                                  tessellation_hs_[1], tessellation_hs_[2], tessellation_hs_[3], tessellation_hs_[4],
                                  fsi_depth_fs_,       depth_float24_fs_[0], depth_float24_fs_[1], placeholder_fs_};
    for (uint32_t k = 0; k < std::size(own); k++)
        if (own[k]) module_names_[own[k]] = {2, own[k] == fsi_depth_fs_ ? fsi_ : 0u, k + 1};
    open_storage();
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
        last_constants_at_[0] = last_constants_at_[1] = last_system_at_ = last_fsi_at_ = NONE;
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
        // the interlock path's counters, the slot's share zeroed
        if (fsi_ && s.occlusion_used) {
            vkCmdFillBuffer(s.cmd, zpd_counters_.buffer, VkDeviceSize(current_) * OCCLUSION_QUERIES * 4,
                            VkDeviceSize(s.occlusion_used) * 4, 0);
            full_barrier(s.cmd);
        }
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
    if (fsi_ && !s.occlusion_reports.empty()) {
        // the interlock path's occlusion counts, for zpd_collect
        full_barrier(s.cmd);
        VkDeviceSize at = VkDeviceSize(current_) * OCCLUSION_QUERIES * 4;
        VkBufferCopy copy{at, at, VkDeviceSize(s.occlusion_reports.size()) * 4};
        vkCmdCopyBuffer(s.cmd, zpd_counters_.buffer, zpd_readback_.buffer, 1, &copy);
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(s.cmd, &dep);
    }
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
VkDeviceSize Impl::request(const std::vector<Range> &ranges, RequestMemo *memo) {
    if (ranges.empty()) return 0;
    if (trace_) traced(cpu_parts(ranges));
    const uint32_t count_before = mem_.write_count();
    if (memo && memo->epoch == mirror_epoch_ && memo->count == count_before && memo->ranges == ranges) return 0;
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
    if (runs.empty()) {
        if (memo) *memo = {ranges, count_before, mirror_epoch_};
        return 0;
    }
    for (auto [first, end] : runs) mem_.watch(first << page_shift_, (end - first) << page_shift_);
    uint32_t count = mem_.write_count();
    VkDeviceSize total = 0;
    for (auto [first, end] : runs) total += VkDeviceSize(end - first) << page_shift_;
    VkBuffer from;
    VkDeviceSize at;
    uint8_t *data = staging(total, from, at);
    if (!data) {
        for (auto [first, end] : runs) std::fill(mirror_at_.begin() + first, mirror_at_.begin() + end, 0u);
        mirror_epoch_++;
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
    // (every page of the ranges current as of count, while nothing is
    // written after it: those just given, and the rest found current above
    // if nothing was written since that check either)
    if (memo && count == count_before) *memo = {ranges, count, mirror_epoch_};
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
    textures_written_by_gpu(pages);
}

// A texture listed in the buckets its ranges span, once its ranges are
// known (they do not change while its key stays the same).
void Impl::index_texture(Texture &t) {
    uint32_t first = 1, last = 0;
    if (t.high > t.low) first = t.low >> TEXTURE_BUCKET_SHIFT, last = (t.high - 1) >> TEXTURE_BUCKET_SHIFT;
    if (first == t.bucket_first && last == t.bucket_last) return;
    unindex_texture(t);
    for (uint32_t b = first; b <= last; b++) texture_buckets_[b].push_back(&t);
    t.bucket_first = first, t.bucket_last = last;
}

void Impl::unindex_texture(Texture &t) {
    for (uint32_t b = t.bucket_first; b <= t.bucket_last; b++) {
        std::vector<Texture *> &bucket = texture_buckets_[b];
        auto it = std::find(bucket.begin(), bucket.end(), &t);
        if (it == bucket.end()) continue;
        *it = bucket.back();
        bucket.pop_back();
    }
    t.bucket_first = 1, t.bucket_last = 0;
}

void Impl::textures_written_by_gpu(const std::vector<Range> &written, Texture *const written_too[2]) {
    uint32_t low = UINT32_MAX, high = 0;
    for (const Range &r : written)
        if (r.second > r.first) low = std::min(low, r.first), high = std::max(high, r.second);
    if (high <= low) return;
    const uint32_t first = low >> TEXTURE_BUCKET_SHIFT, last = (high - 1) >> TEXTURE_BUCKET_SHIFT;
    for (uint32_t b = first; b <= last; b++)
        for (Texture *t : texture_buckets_[b]) {
            // each texture once: in the first bucket both it and the write span
            if (b != std::max(first, t->bucket_first) ||
                (written_too && (t == written_too[0] || t == written_too[1])))
                continue;
            if (t->high <= low || t->low >= high || (t->stale && t->load_why == LoadWhy::GpuWritten)) continue;
            bool hit = false;
            for (const Range &r : written) {
                for (const Range &tr : t->ranges)
                    if (overlaps(r, tr)) {
                        hit = true;
                        break;
                    }
                if (hit) break;
            }
            if (hit) t->stale = true, t->load_why = LoadWhy::GpuWritten;
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
        s->hash = hash;
        report_.shaders++;
        if (storage_ && storage_->add_shader(type, hash, code)) report_.shaders_stored++;
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
        s.approximate = t.approximate;
        if (!t.ok()) {
            s.unsupported = t.unsupported.empty() ? "the translator wrote nothing" : t.unsupported.front();
        } else {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = t.spirv.size() * 4;
            info.pCode = t.spirv.data();
            if (vkCreateShaderModule(dev_, &info, nullptr, &s.module) != VK_SUCCESS)
                s.unsupported = "vkCreateShaderModule failed";
            else
                module_names_[s.module] = {1, 0, s.hash};
        }
    }
    if (!s.unsupported.empty()) why = s.unsupported;
    return s.unsupported.empty();
}

// a pixel shader's other forms (shader::PixelOptions), made as draws ask for
// them, kept among its variants above the vertex forms' keys
VkShaderModule Impl::pixel_module(Shader &s, const shader::PixelOptions &options, std::string &why) {
    uint32_t key = options.key();
    if (!key) return s.module;
    key |= 1u << 31;
    auto it = s.variants.find(key);
    if (it == s.variants.end()) {
        VkShaderModule m = VK_NULL_HANDLE;
        shader::Translation t = shader::translate(s.parsed, shader::Stage::Pixel, {}, options);
        if (t.ok()) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = t.spirv.size() * 4;
            info.pCode = t.spirv.data();
            if (vkCreateShaderModule(dev_, &info, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
            if (m) module_names_[m] = {1, key, s.hash};
        }
        it = s.variants.emplace(key, m).first;
    }
    if (!it->second) why = "its float24 depth, centroid or interlock form could not be made";
    return it->second;
}

VkShaderModule Impl::vertex_module(Shader &s, const shader::VertexOptions &options, std::string &why) {
    uint32_t key = options.key();
    if (!key) return s.module; // HostVertex::Vertex without clip planes
    auto it = s.variants.find(key);
    if (it == s.variants.end()) {
        VkShaderModule m = VK_NULL_HANDLE;
        shader::Translation t = shader::translate(s.parsed, shader::Stage::Vertex, options);
        if (t.ok()) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = t.spirv.size() * 4;
            info.pCode = t.spirv.data();
            if (vkCreateShaderModule(dev_, &info, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
            if (m) module_names_[m] = {1, key, s.hash};
        }
        it = s.variants.emplace(key, m).first;
    }
    if (!it->second) why = "its host form could not be made";
    return it->second;
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
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
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
        if (format == 1 && gamma_unorm16_) vk_format = VK_FORMAT_R16G16B16A16_UNORM, t->gamma_linear = true;
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
    // (xenos.h's GetSurfacePitchTiles: a 64bpp row of tiles is twice the
    // 32bpp one, rounded at 32bpp)
    uint32_t pitch_tiles = std::max(1u, (pitch * samples_x + 79) / 80) << (bytes == 8 ? 1 : 0);
    uint32_t rows = std::max(1u, (EDRAM_TILES - std::min(base, EDRAM_TILES - 1)) / pitch_tiles);
    t->width = std::clamp(pitch, 1u, 8192u);
    t->height = std::clamp(rows * tile_height, 1u, 8192u);
    t->wide = bytes == 8;
    t->tile_width = tile_width, t->tile_height = tile_height;
    t->pitch_tiles = pitch_tiles;
    VkSampleCountFlagBits samples = msaa == 0 ? VK_SAMPLE_COUNT_1_BIT : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_4_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // A float target (16_16_FLOAT, 16_16_16_16_FLOAT, 32_FLOAT, 32_32_FLOAT)
    // keeps the guest's bits as its host components: its image is made
    // mutable and moved through EDRAM through a UINT view (EDRAM stores
    // sampled as floats lose NaN payloads and denormals; Xenia's D3D12 TODO
    // on D3D11.3's NaN rules for these). 2_10_10_10_FLOAT is 7e3 in EDRAM, not
    // the host's bits.
    VkFormat raw_format = VK_FORMAT_UNDEFINED;
    if (!depth && (format == 6 || format == 7 || format == 14 || format == 15) && raw_float_targets_) {
        raw_format = vk_format == VK_FORMAT_R16G16_SFLOAT         ? VK_FORMAT_R16G16_UINT
                     : vk_format == VK_FORMAT_R16G16B16A16_SFLOAT ? VK_FORMAT_R16G16B16A16_UINT
                     : vk_format == VK_FORMAT_R32_SFLOAT          ? VK_FORMAT_R32_UINT
                     : vk_format == VK_FORMAT_R32G32_SFLOAT       ? VK_FORMAT_R32G32_UINT
                                                                  : VK_FORMAT_UNDEFINED;
        if (raw_format != VK_FORMAT_UNDEFINED &&
            (!d_.format_supported(raw_format,
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
             (msaa && !(integer_samples_ & samples))))
            raw_format = VK_FORMAT_UNDEFINED;
    }
    t->image = d_.create_image(VK_IMAGE_TYPE_2D, vk_format, {t->width, t->height, 1}, 1, 1, samples, usage,
                               raw_format != VK_FORMAT_UNDEFINED ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    if (!t->image.image) {
        fatal_ = "could not make a render target";
        return nullptr;
    }
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    t->view = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, aspect);
    t->sampled = depth ? make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_DEPTH_BIT) : t->view;
    if (depth) t->stencil = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_STENCIL_BIT);
    if (raw_format != VK_FORMAT_UNDEFINED) {
        t->raw = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, raw_format, VK_IMAGE_ASPECT_COLOR_BIT);
        t->raw_format = raw_format;
    }
    // its EDRAM store's descriptors: EDRAM, the tile mask (in the arena), its views
    t->store_set = edram_set(0);
    if (!t->store_set) {
        fatal_ = "could not make a render target's EDRAM descriptors";
        return nullptr;
    }
    // EDRAM, the tile mask, its views (the stencil's only for depth), the host depth beside EDRAM
    VkDescriptorBufferInfo buffers[3] = {
        {edram_.buffer, 0, VK_WHOLE_SIZE},
        {arena_.buffer, 0, EDRAM_TILES * 4},
        {host_depth_.buffer, 0, VK_WHOLE_SIZE}};
    // (binding 3: the stencil of a depth target, or a float target's UINT view)
    VkImageView unsigned_view = depth ? t->stencil : t->raw;
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, t->sampled, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, unsigned_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5] = {};
    uint32_t write_count = 0;
    for (uint32_t b = 0; b < 5; b++) {
        if (b == 3 && !unsigned_view) continue;
        VkWriteDescriptorSet &w = writes[write_count++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = t->store_set;
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = b == 1             ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                           : b == 0 || b == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                              : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (b == 2 || b == 3) w.pImageInfo = &images[b - 2];
        else w.pBufferInfo = &buffers[b == 4 ? 2 : b];
    }
    vkUpdateDescriptorSets(dev_, write_count, writes, 0, nullptr);
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

// The interlock path's view of a target in EDRAM, for resolves and clears:
// what render_target() works out, without an image (the storage format's
// host format for the resolve's texel size).
RenderTarget Impl::edram_view(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format) const {
    if (!depth && format == 10) format = 2;
    if (!depth && format == 12) format = 3;
    RenderTarget t;
    t.depth = depth, t.base = base, t.pitch = pitch, t.msaa = msaa, t.format = format;
    if (!depth) t.host = resolve::host_format_of_color_target(format);
    t.wide = !depth && (format == 5 || format == 7 || format == 15);
    uint32_t samples_x = msaa >= 2 ? 2 : 1, samples_y = msaa >= 1 ? 2 : 1;
    t.tile_width = (t.wide ? 40 : 80) / samples_x, t.tile_height = 16 / samples_y;
    // xenos.h's GetSurfacePitchTiles: 64bpp rows twice the 32bpp ones
    t.pitch_tiles = std::max(1u, (pitch * samples_x + 79) / 80) << (t.wide ? 1 : 0);
    uint32_t rows = std::max(1u, (EDRAM_TILES - std::min(base, EDRAM_TILES - 1)) / t.pitch_tiles);
    t.width = std::clamp(pitch, 1u, 8192u);
    t.height = std::clamp(rows * t.tile_height, 1u, 8192u);
    return t;
}

// A resolve's clear into EDRAM on the interlock path (edram_clear.comp): the
// rectangle's samples get the clear's words
void Impl::fsi_clear(const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t lo, uint32_t hi) {
    EdramTarget p{};
    p.base_tiles = t.base, p.pitch_tiles = t.pitch_tiles, p.msaa = t.msaa, p.format = t.format;
    p.width = w, p.height = h, p.is_depth = t.depth, p.ints_log2 = t.wide ? 1 : 0;
    p.x = x, p.y = y, p.clear_lo = lo, p.clear_hi = hi;
    VkCommandBuffer c = outside();
    gpu_mark("resolves: clears");
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, clear_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 1, &resolve_edram_set_, 0, nullptr);
    vkCmdPushConstants(c, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
}

// The interlock path's occlusion counter for the draw being recorded (the
// report open, a counter per report in the slot's share, as zpd_draw opens
// a host query; ~0 when none counts)
uint32_t Impl::fsi_zpd_index() {
    if (!zpd_current_) return UINT32_MAX;
    cmd();
    Slot &s = slots_[current_];
    if (zpd_active_ == UINT32_MAX) {
        if (s.occlusion_reports.size() >= OCCLUSION_QUERIES) {
            zpd_counts_[zpd_current_].uncounted = true;
            return UINT32_MAX;
        }
        zpd_active_ = uint32_t(s.occlusion_reports.size());
        s.occlusion_reports.push_back(zpd_current_);
        s.occlusion_used = uint32_t(s.occlusion_reports.size());
        zpd_counts_[zpd_current_].open++;
        report_.occlusion_queries++;
    }
    return current_ * OCCLUSION_QUERIES + zpd_active_;
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
    // (a float target moved by its bits renders into its UINT view)
    VkFormat color_format = t.raw ? t.raw_format : t.image.format;
    auto key = std::make_pair(color_format, t.msaa);
    auto it = load_pipelines_.find(key);
    if (it != load_pipelines_.end()) return it->second;
    VkShaderModule fs = !t.depth ? (t.raw ? load_uint_fs_ : load_fs_[0]) : load_fs_[2] ? load_fs_[2] : load_fs_[1];
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
    attachment.imageView = t->raw ? t->raw : t->view;
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
    // the border color (dword 5 bits 0-1: black, white, and the two YCbCr
    // blacks) and force_bc_w_to_max (bit 2: its alpha 1), where a clamp uses
    // the border, as Xenia's texture caches take them; the YCbCr ones as a
    // custom border color where the device has it, as Xenia's Vulkan cache
    uint32_t border_color = border ? fetch[5] & 3 : 0, border_alpha = border ? (fetch[5] >> 2) & 1 : 0;
    if (border_color >= 2 && !d_.custom_border_color())
        approximate("YCbCr border colors (black: no custom border colors on the device)");
    if (d_.max_anisotropy() < 1.0f) aniso = 0;
    std::array<uint32_t, 8> key = {clamp[0] | clamp[1] << 3 | clamp[2] << 6 | border_color << 9 | border_alpha << 11,
                                   mag, min, mip, aniso, mip_min, 0, 0};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        it->second.used_serial = submission_serial();
        return it->second.index;
    }
    // The table full: the sampler least recently used by a submission the
    // GPU has finished is destroyed and its slot taken, as Xenia's texture
    // caches keep their samplers within the device's limit (the binding is
    // updated while unused submissions are pending). Only with every one
    // used by a submission still running is the stand-in's taken, counted.
    if (next_sampler_ >= sampler_capacity_ && free_samplers_.empty()) {
        while (finish_oldest(false)) {}
        auto oldest = samplers_.end();
        for (auto s = samplers_.begin(); s != samplers_.end(); ++s)
            if (s->second.used_serial <= done_serial_ &&
                (oldest == samplers_.end() || s->second.used_serial < oldest->second.used_serial))
                oldest = s;
        if (oldest == samplers_.end()) {
            approximate("samplers past the bindless table's size, all in use (the stand-in's used)");
            return 0;
        }
        vkDestroySampler(dev_, oldest->second.sampler, nullptr);
        free_samplers_.push_back(oldest->second.index);
        samplers_.erase(oldest);
        report_.samplers_reused++;
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
    VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border_color == 1) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (border_color >= 2 && d_.custom_border_color()) {
        // (Cr, Y, Cb) and (Y, Cr, Cb) unsigned, Y 0 and the chroma 0.5
        float *c = custom.customBorderColor.float32;
        c[0] = border_color == 2 ? 0.5f : 0.0f;
        c[1] = border_color == 2 ? 0.0f : 0.5f;
        c[2] = 0.5f;
        c[3] = border_alpha ? 1.0f : 0.0f;
        custom.format = VK_FORMAT_UNDEFINED;
        info.pNext = &custom;
        info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else {
        info.borderColor = border_alpha ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &info, nullptr, &s);
    uint32_t index;
    if (!free_samplers_.empty()) {
        index = free_samplers_.back();
        free_samplers_.pop_back();
    } else {
        index = next_sampler_++;
    }
    VkDescriptorImageInfo image{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_;
    write.dstBinding = 3;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    samplers_[key] = {s, index, submission_serial()};
    return index;
}

// ---- the textures' lifetime ----
//
// As Xenia's texture cache (texture_cache.cc): a texture is used by a
// submission (MarkAsUsed, once a submission) and moves to the end of a list
// by last use; when the GPU has finished submissions (CompletedSubmissionUpdated)
// and the images take more memory than the soft limit, the least recently
// used ones it is done with go, if unused for the soft lifetime, or at once
// above the hard limit. A texture is destroyed only once the GPU is done with
// its last use, so its places in the bindless tables are given back then (as
// D3D12's ~D3D12Texture releases its descriptors), pointed at the stand-in
// before anything takes them again; an image a constant replaces goes with
// its places once the submission being recorded is done.
//
// Beyond Xenia, whose one table holds 262,144 descriptors: ours are smaller
// (the slot word's 14 bits), so a table without room destroys the least
// recently used textures of its kind the GPU is done with, waiting for
// submissions in flight if it must (never the one being recorded); a load is
// refused only when the submission being recorded holds the whole table.

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A texture used by a submission: the newest in the list by last use.
void Impl::use(Texture &t, uint64_t serial) {
    if (t.used_serial >= serial) return;
    t.used_serial = serial;
    t.used_ms = now_ms();
    if (lru_newest_ == &t) return;
    unlink(t);
    t.older = lru_newest_;
    if (lru_newest_)
        lru_newest_->newer = &t;
    else
        lru_oldest_ = &t;
    lru_newest_ = &t;
}

void Impl::unlink(Texture &t) {
    if (t.older)
        t.older->newer = t.newer;
    else if (lru_oldest_ == &t)
        lru_oldest_ = t.newer;
    if (t.newer)
        t.newer->older = t.older;
    else if (lru_newest_ == &t)
        lru_newest_ = t.older;
    t.older = t.newer = nullptr;
}

void Impl::count_texture_bytes(VkDeviceSize add, VkDeviceSize remove) {
    texture_bytes_ = texture_bytes_ + add - remove;
    report_.texture_bytes = texture_bytes_;
    report_.texture_bytes_peak = std::max<uint64_t>(report_.texture_bytes_peak, texture_bytes_);
}

VkDeviceSize Impl::image_bytes(const vk::Image &image) const {
    if (!image.image) return 0;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev_, image.image, &req);
    return req.size;
}

// Xenia's CompletedSubmissionUpdated, with its limits: run when the GPU has
// finished submissions since the last time, before a texture is looked up
// (no texture is held then).
void Impl::apply_texture_limits() {
    limits_applied_at_ = done_serial_;
    // the limits' render-to-texture part times the draw resolution scale's
    // area less one (Xenia's): the renderer draws at scale 1, so none
    constexpr uint64_t resolution_scale_area = 1;
    const uint64_t scaled_add = uint64_t(options_.texture_memory_render_to_texture_mb) * (resolution_scale_area - 1);
    const uint64_t soft_mb = options_.texture_memory_soft_mb + scaled_add;
    const uint64_t hard_mb = options_.texture_memory_hard_mb + scaled_add;
    const uint64_t lifetime_ms = uint64_t(options_.texture_memory_soft_lifetime_s) * 1000;
    const uint64_t now = now_ms();
    while (lru_oldest_) {
        const uint64_t mb = (texture_bytes_ + ((uint64_t(1) << 20) - 1)) >> 20;
        const bool hard_exceeded = mb > hard_mb;
        if (mb <= soft_mb && !hard_exceeded) break;
        Texture *t = lru_oldest_;
        if (t->used_serial > done_serial_) break;
        if (!hard_exceeded && t->used_ms + lifetime_ms > now) break;
        destroy_texture(t);
        report_.textures_evicted++;
    }
}

// Room for `needed` places in a table of `kind`: the least recently used
// textures of that kind the GPU is done with destroyed, the oldest
// submission in flight waited for while that is not enough. The list is in
// the order of the submissions that last used them, so the first one still
// in use ends the walk.
bool Impl::make_room(uint32_t kind, uint32_t needed) {
    for (;;) {
        for (Texture *t = lru_oldest_; t && room(kind) < needed && t->used_serial <= done_serial_;) {
            Texture *newer = t->newer;
            if (t->kind == kind) {
                destroy_texture(t);
                report_.textures_evicted_for_room++;
            }
            t = newer;
        }
        if (room(kind) >= needed) return true;
        if (!fatal_.empty() || !finish_oldest(true)) return false;
    }
}

// A texture the GPU is done with: its places given back, its views and
// images destroyed, and out of the cache.
void Impl::destroy_texture(Texture *t) {
    unlink(*t);
    unindex_texture(*t);
    count_texture_bytes(0, t->bytes);
    for (const TextureView &v : t->views) {
        release_index(t->kind, v.index);
        vkDestroyImageView(dev_, v.view, nullptr);
    }
    report_.texture_views -= t->views.size();
    release_target(t->target, t->target_view);
    if (t->image.image) d_.destroy(t->image);
    if (t->signed_image.image) d_.destroy(t->signed_image);
    textures_.erase(t->key);
    report_.textures_held = textures_.size();
}

// A place in a table the GPU no longer reads: pointed at the stand-in (no
// descriptor ever names a destroyed view) and free to take again.
void Impl::release_index(uint32_t kind, uint32_t index) {
    VkDescriptorImageInfo image{VK_NULL_HANDLE, dummy_views_[kind], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_;
    write.dstBinding = kind;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    free_indices_[kind].push_back(index);
}

// A place in a table with room: one given back, else a new one.
uint32_t Impl::take_index(uint32_t kind) {
    if (!free_indices_[kind].empty()) {
        uint32_t index = free_indices_[kind].back();
        free_indices_[kind].pop_back();
        return index;
    }
    return next_index_[kind]++;
}

// A texture by what makes its image in a fetch constant (TextureKey), loaded
// again when the memory it came from was written since (its pages' watches,
// as Xenia's texture cache drops a texture when a watch on its pages fires;
// resolves and memory exports over it mark it at once): from guest memory on
// the CPU, or from the mirror on the GPU when the GPU holds some of it.
// Loaded again, it keeps its image and its views.
Texture *Impl::texture(const uint32_t fetch[6], std::string &why) {
    if (done_serial_ != limits_applied_at_) apply_texture_limits();
    const texture::Info info = texture::parse_fetch_constant(fetch);
    // signed components: read through a snorm view of the same bits (Xenia's
    // signed host format; float formats are signed either way), or an image
    // of their own where the signed bits differ (10_11_11 and 11_11_10, 16-bit
    // snorm as floats), a texture of its own as Xenia's signed_separate keys
    // it; biased and gamma components, and the exponent adjust, are the
    // shader's (texture_signs)
    const bool any_signed = info.signs[0] == 1 || info.signs[1] == 1 || info.signs[2] == 1 || info.signs[3] == 1;
    const bool separate = any_signed && texture::signed_read(info.format, support_) == texture::Signed::Separate;
    const TextureKey key = texture_key(info, fetch, separate);
    auto it = textures_.find(key);
    Texture *t = it == textures_.end() ? nullptr : it->second.get();
    // used by the submission being recorded, counted as the next one's: the
    // draw that wants it records after its loads, which may submit this one,
    // and a swap's presenter reads the front buffer after it is submitted
    const uint64_t serial = submission_serial() + 1;
    if (t) use(*t, serial);
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
        t->load_why = LoadWhy::CpuWritten;
    }
    if (t) t->stale = true; // until it is loaded again

    texture::Plan plan, signed_plan;
    if (!texture::plan(info, plan, why, support_)) return nullptr;
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
    // what signed views read, made whether or not this constant reads its
    // components signed (another constant over the same image may)
    VkFormat signed_format = VK_FORMAT_UNDEFINED;
    if (plan.signed_read == texture::Signed::View && !is_float(format))
        signed_format = snorm_of(format);
    else if (separate)
        signed_format =
            signed_plan.snorm ? snorm_of(vk_format_of(signed_plan.format)) : vk_format_of(signed_plan.format);
    // over pages the GPU holds (a resolve's, a memory export's): loaded on the
    // GPU from the mirror; the DXT formats a host without BC holds
    // decompressed are decompressed on the GPU too, from guest memory put into
    // the mirror
    bool mirror = gpu_held(ranges);
    bool gpu = mirror || texture_loads_on_gpu_ || (texture::is_compressed(info.host) && !texture::is_compressed(plan.format));
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
                 separate != (t->signed_image.image != VK_NULL_HANDLE) || signed_format != t->signed_format;
    if (fresh) {
        uint32_t kind = three ? 1 : cube ? 2 : 0;
        if (t) {
            // the same key made another image (not with the same format
            // decisions; kept safe): the old one goes once the submission
            // being recorded is done, its views and their places in the table
            // with it (work under way may still read them)
            vk::Image old = t->image, old_signed_image = t->signed_image;
            std::vector<TextureView> old_views = std::move(t->views);
            uint32_t old_kind = t->kind, old_target = t->target;
            VkImageView old_target_view = t->target_view;
            t->target = 0, t->target_view = VK_NULL_HANDLE;
            report_.texture_views -= old_views.size();
            cmd();
            slots_[current_].after.push_back([this, old, old_signed_image, old_views, old_kind, old_target,
                                              old_target_view]() mutable {
                for (const TextureView &v : old_views) {
                    release_index(old_kind, v.index);
                    vkDestroyImageView(dev_, v.view, nullptr);
                }
                release_target(old_target, old_target_view);
                if (old.image) d_.destroy(old);
                if (old_signed_image.image) d_.destroy(old_signed_image);
            });
            count_texture_bytes(0, t->bytes);
            t->bytes = 0;
            t->image = {}, t->signed_image = {}, t->views.clear();
        } else {
            auto owned = std::make_unique<Texture>();
            t = owned.get();
            t->stale = true;
            t->key = key;
            textures_[key] = std::move(owned);
            report_.textures_held = textures_.size();
            use(*t, serial);
        }
        t->dimension = info.dimension, t->kind = kind;
        t->host = plan.format, t->host_swizzle = plan.host_swizzle;
        t->signed_read = plan.signed_read, t->signed_format = signed_format;
        // a source of copies too: a front buffer read back, or copied by a presenter
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImageCreateFlags flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        // A resolve target (resolve_target): a texture over memory a resolve
        // wrote, one 2D level of 32- or 64-bit texels whose image holds the
        // guest's bits as they are (a plain copy), tiled, made storable
        // through an unsigned view of its texel's size, which resolves write.
        const uint32_t texel_bytes = texture::format_block(info.format).bytes;
        const bool target = resolve_targets_ && mirror && !separate && !three && !cube && layers == 1 &&
                            level_count == 1 && plan.slices.size() == 1 && plan.slices[0].level == 0 &&
                            plan.slices[0].tiled && plan.load == texture::Load::Copy &&
                            plan.half == texture::Half::None && plan.block_width == 1 && plan.block_height == 1 &&
                            (texel_bytes == 4 || texel_bytes == 8) && plan.bytes_per_block == texel_bytes;
        const VkFormat target_format = texel_bytes == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32_UINT;
        if (target) usage |= VK_IMAGE_USAGE_STORAGE_BIT, flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        // a signed view of the same bits: the image mutable, with the snorm
        // format listed (Xenia's Vulkan textures with both formats)
        t->image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, format, extent, level_count, layers,
                                   VK_SAMPLE_COUNT_1_BIT, usage, flags, separate ? VK_FORMAT_UNDEFINED : signed_format,
                                   target ? target_format : VK_FORMAT_UNDEFINED);
        if (target && t->image.image) {
            const uint32_t wide = texel_bytes == 8 ? 1 : 0;
            uint32_t slot = UINT32_MAX;
            if (!free_targets_[wide].empty()) {
                slot = free_targets_[wide].back();
                free_targets_[wide].pop_back();
            } else if (next_target_[wide] < RESOLVE_TARGETS) {
                slot = next_target_[wide]++;
            }
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = t->image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = target_format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageViewUsageCreateInfo view_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            view_usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            view.pNext = &view_usage;
            if (slot != UINT32_MAX && vkCreateImageView(dev_, &view, nullptr, &t->target_view) == VK_SUCCESS) {
                VkDescriptorImageInfo image{VK_NULL_HANDLE, t->target_view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = target_set_;
                write.dstBinding = wide;
                write.dstArrayElement = slot;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.pImageInfo = &image;
                vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
                t->target = (slot + 1) | wide << 7;
                t->target_base = plan.slices[0].base & 0x1FFFFFFF, t->target_pitch = plan.slices[0].pitch_blocks;
                t->target_endian = info.endian;
            } else if (slot != UINT32_MAX) {
                free_targets_[wide].push_back(slot);
            }
        }
        if (separate)
            t->signed_image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, signed_format, extent,
                                              level_count, layers, VK_SAMPLE_COUNT_1_BIT, usage, flags);
        t->bytes = image_bytes(t->image) + image_bytes(t->signed_image);
        count_texture_bytes(t->bytes, 0);
        if (!t->image.image || (separate && !t->signed_image.image)) {
            why = "could not make the image";
            return nullptr;
        }
    }

    // what it was loaded from, for seeing when that changes
    t->ranges = std::move(ranges);
    t->low = UINT32_MAX, t->high = 0;
    for (auto [start, end] : t->ranges) t->low = std::min(t->low, start), t->high = std::max(t->high, end);
    index_texture(*t);
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
    (t->load_why == LoadWhy::New          ? report_.texture_loads_new
     : t->load_why == LoadWhy::CpuWritten ? report_.texture_loads_cpu_written
                                          : report_.texture_loads_gpu_written)++;
    t->load_why = LoadWhy::CpuWritten; // the next one, unless a resolve or an export marks it
    if (options_.verbose)
        std::fprintf(stderr, "  texture %08X: format %s %ux%u, signs %u%u%u%u, exponent %d, from %s as %s%s\n",
                     info.base_address, format_name(info.format), plan.width, plan.height, info.signs[0],
                     info.signs[1], info.signs[2], info.signs[3], info.exp_adjust,
                     mirror ? "the mirror" : gpu ? "guest memory on the GPU" : "guest memory",
                     texture::host_format_name(plan.format), separate ? " and a signed image" : "");
    return t;
}

// As Xenia's FindOrCreateTextureDescriptor: the view by the swizzle through
// the host format's and the sign, made the first time a constant reads the
// texture so, at a place of its own in the texture's table (a full table
// makes room as a load did).
uint32_t Impl::texture_view(Texture &t, uint32_t guest_swizzle, bool is_signed, std::string &why) {
    const uint32_t swizzle = texture::compose_swizzle(guest_swizzle, t.host_swizzle);
    const uint32_t key = swizzle | (is_signed ? 1u << 12 : 0u);
    for (const TextureView &v : t.views)
        if (v.key == key) return v.index;
    if (!t.image.image || (is_signed && t.signed_format == VK_FORMAT_UNDEFINED)) {
        why = "no image for the view";
        return 0;
    }
    if (room(t.kind) < 1 && !make_room(t.kind, 1)) {
        if (fatal_.empty()) {
            report_.textures_refused++;
            why = "the bindless table is full";
        }
        return 0;
    }
    const vk::Image &image = is_signed && t.signed_image.image ? t.signed_image : t.image;
    VkComponentMapping mapping{swizzle_of(swizzle & 7), swizzle_of((swizzle >> 3) & 7), swizzle_of((swizzle >> 6) & 7),
                               swizzle_of((swizzle >> 9) & 7)};
    VkImageViewType view_type = t.dimension == 3   ? VK_IMAGE_VIEW_TYPE_CUBE
                                : t.dimension == 2 ? VK_IMAGE_VIEW_TYPE_3D
                                                   : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    VkImageView view = make_view(image.image, view_type, is_signed ? t.signed_format : image.format,
                                 VK_IMAGE_ASPECT_COLOR_BIT, mapping, image.levels, image.layers, false);
    if (!view) {
        why = is_signed ? "could not make the image's signed view" : "could not make the image's view";
        return 0;
    }
    const uint32_t index = take_index(t.kind);
    VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_;
    write.dstBinding = t.kind;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    t.views.push_back({key, view, index});
    report_.texture_views++;
    return index;
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

VkPipeline Impl::pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &drawn, bool async,
                          bool &pending, bool &stand_in) {
    pending = stand_in = false;
    async = async && pipeline_workers_ && !options_.linked_pipelines_only;
    // how long a pipeline made on the workers left its draws without one
    // (until it, or for a linked stand-in its last library, was made: a
    // draw asking again only later, as a scene shown once more a minute
    // after, waited no longer than that)
    auto drawn_with = [this](PipelineEntry &entry) {
        using clock = std::chrono::steady_clock;
        if (entry.asked == clock::time_point{}) return;
        clock::time_point end = clock::now(), ready{};
        if (entry.pipeline) {
            ready = entry.made;
        } else {
            for (PipelineEntry *l : entry.libraries)
                if (l) ready = std::max(ready, l->made);
        }
        if (ready != clock::time_point{} && ready < end) end = std::max(ready, entry.asked);
        double waited = std::chrono::duration<double>(end - entry.asked).count();
        entry.asked = {};
        report_.pipeline_waits++;
        report_.pipeline_wait_seconds += waited;
        report_.pipeline_wait_max = std::max(report_.pipeline_wait_max, waited);
    };
    PipelineEntry *e = nullptr;
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        e = it->second.get();
        if (e->state.load(std::memory_order_acquire)) {
            if (e->stand_in) retire_stand_in(*e);
            if (e->pipeline) drawn_with(*e);
            return e->pipeline;
        }
        if (!async) {
            // being made on a worker, and the draw waits for it
            Timer timer(report_.pipeline_seconds);
            if (!e->queued) {
                pipeline_workers_->queue(e, canonical(drawn), PipelinePriority::normal);
                e->queued = true;
            }
            pipeline_workers_->wait(e);
            if (e->stand_in) retire_stand_in(*e);
            return e->pipeline;
        }
    } else {
        e = (pipelines_[key] = std::make_unique<PipelineEntry>()).get();
        report_.pipelines++;
        last_pipeline_swap_ = report_.swaps;
        if (options_.verbose) {
            std::fprintf(stderr, "  pipeline %llu:", (unsigned long long)report_.pipelines);
            for (uint32_t word : key) std::fprintf(stderr, " %X", word);
            std::fprintf(stderr, "\n");
        }
        // made as described: its dynamic states' values one each, so that
        // the driver's pipeline cache finds it whichever draw asks first
        std::unique_ptr<PipelineDesc> desc = canonical(drawn);
        const VkGraphicsPipelineCreateInfo &info = desc->info;
        // kept in the storage for the build step; made by it already?
        bool built = false;
        if (storage_ || !built_pipelines_.empty()) {
            const std::vector<uint32_t> description = describe(info);
            if (!description.empty()) {
                built = built_pipelines_.count(ShaderStorage::hash(0, description)) != 0;
                if (storage_ && storage_->add_pipeline(description)) report_.pipelines_stored++;
            }
        }
        if (built) report_.pipelines_prebuilt++;
        PipelineParts parts;
        if (use_libraries_) library_keys(info, parts);
        if (options_.linked_pipelines_only) {
            // linked from libraries made now, never made whole
            for (uint32_t part = 0; part < 4; part++) e->libraries[part] = library(part, parts.keys[part], info, false);
            e->pipeline = link(*e);
            e->state.store(1, std::memory_order_release);
            return e->pipeline;
        }
        const bool linked = stand_in_ == Options::PipelineStandIn::library && use_libraries_;
        // made now: waited for, or (built, and not linked from libraries
        // that are) found in the pipeline cache
        if (!async || (built && !linked)) {
            VkPipeline p = VK_NULL_HANDLE;
            {
                Timer timer(report_.pipeline_seconds);
                if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &info, nullptr, &p) != VK_SUCCESS)
                    p = VK_NULL_HANDLE;
            }
            e->pipeline = p;
            e->state.store(1, std::memory_order_release);
            return p;
        }
        // made on the workers: the whole pipeline, or first its libraries
        // (made now, those the build step made)
        e->asked = std::chrono::steady_clock::now();
        if (linked) {
            for (uint32_t part = 0; part < 4; part++) e->libraries[part] = library(part, parts.keys[part], info, true);
        } else {
            if (stand_in_ == Options::PipelineStandIn::placeholder) {
                e->stand_in = placeholder(info);
                if (e->stand_in) report_.pipeline_placeholders++;
                e->stand_in_failed = !e->stand_in;
            }
            pipeline_workers_->queue(e, std::move(desc), PipelinePriority::normal);
            e->queued = true;
        }
    }
    // being made on the workers: its stand-in meanwhile, linked as soon as
    // the libraries it is linked from are made; then the whole pipeline,
    // which replaces the linked one, after everything the draws wait for (or
    // at once when its libraries failed)
    if (!e->stand_in && !e->stand_in_failed && e->libraries[0]) e->stand_in = link(*e);
    if (!e->queued && (e->stand_in || e->stand_in_failed)) {
        pipeline_workers_->queue(e, canonical(drawn), e->stand_in ? PipelinePriority::later : PipelinePriority::normal);
        e->queued = true;
    }
    if (e->stand_in) {
        drawn_with(*e);
        return stand_in = true, e->stand_in;
    }
    return pending = true, VK_NULL_HANDLE;
}

// The part of a whole pipeline's description a library holds, by
// VK_EXT_graphics_pipeline_library's four state subsets: the vertex input and
// input assembly; the vertex, tessellation and geometry shaders with the
// viewport, rasterization and tessellation states; the pixel shader with the
// depth and multisample states; the blend and multisample states and the
// targets' formats. The depth state is all dynamic (extended dynamic state 1
// and 2) but for the depth bounds test, never on, so the fragment shader's
// library takes none of the draw's.
struct LibraryDesc {
    VkGraphicsPipelineLibraryCreateInfoEXT part{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[5]{};
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    LibraryDesc(const VkGraphicsPipelineCreateInfo &whole, uint32_t which) {
        static constexpr VkGraphicsPipelineLibraryFlagsEXT FLAGS[4] = {
            VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
        part.flags = FLAGS[which];
        info.pNext = &part;
        info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        info.pDynamicState = whole.pDynamicState;
        // the targets' formats for the fragment output; the others' view
        // mask (0) alone
        for (auto *in = static_cast<const VkBaseInStructure *>(whole.pNext); in; in = in->pNext)
            if (in->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && which == 3)
                rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo *>(in);
        rendering.pNext = nullptr;
        if (which) part.pNext = &rendering;
        switch (which) {
        case 0:
            info.pVertexInputState = whole.pVertexInputState;
            info.pInputAssemblyState = whole.pInputAssemblyState;
            break;
        case 1:
            for (uint32_t i = 0; i < whole.stageCount; i++)
                if (whole.pStages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                    stages[info.stageCount++] = whole.pStages[i];
            info.pStages = stages;
            info.pViewportState = whole.pViewportState;
            info.pRasterizationState = whole.pRasterizationState;
            info.pTessellationState = whole.pTessellationState;
            info.layout = whole.layout;
            break;
        case 2:
            for (uint32_t i = 0; i < whole.stageCount; i++)
                if (whole.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    stages[info.stageCount++] = whole.pStages[i];
            info.pStages = stages;
            info.pDepthStencilState = &depth;
            info.pMultisampleState = whole.pMultisampleState;
            info.layout = whole.layout;
            break;
        default:
            info.pColorBlendState = whole.pColorBlendState;
            info.pMultisampleState = whole.pMultisampleState;
            break;
        }
    }
};

PipelineEntry *Impl::library(uint32_t part, const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &whole,
                             bool async) {
    if (key.empty()) return nullptr;
    std::unique_ptr<PipelineEntry> &slot = libraries_[key];
    if (slot) return slot.get();
    slot = std::make_unique<PipelineEntry>();
    report_.pipeline_libraries++;
    LibraryDesc desc(whole, part);
    // the parts with the guest's shaders are the slow ones: on the workers,
    // ahead of the whole pipelines; the others are made here, and so are
    // those the build step made into the pipeline cache (a lookup there)
    const bool built = !built_libraries_.empty() && built_libraries_.count(ShaderStorage::hash(part, key)) != 0;
    if (built) report_.pipeline_libraries_prebuilt++;
    if (async && !built && (part == 1 || part == 2)) {
        pipeline_workers_->queue(slot.get(), std::make_unique<PipelineDesc>(desc.info), PipelinePriority::first);
        return slot.get();
    }
    VkPipeline p = VK_NULL_HANDLE;
    {
        Timer timer(report_.pipeline_seconds);
        if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &desc.info, nullptr, &p) != VK_SUCCESS)
            p = VK_NULL_HANDLE;
    }
    slot->pipeline = p;
    slot->state.store(1, std::memory_order_release);
    return slot.get();
}

VkPipeline Impl::link(PipelineEntry &e) {
    VkPipeline libraries[4];
    uint32_t count = 0;
    for (PipelineEntry *l : e.libraries) {
        if (!l) continue;
        if (!l->state.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
        if (!l->pipeline) {
            approximate("pipelines without a stand-in: a library the driver failed");
            return e.stand_in_failed = true, VK_NULL_HANDLE;
        }
        libraries[count++] = l->pipeline;
    }
    // fast linking: no link-time optimization (the whole pipeline the
    // workers make replaces it), nor the cache on disk
    VkPipelineLibraryCreateInfoKHR list{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    list.libraryCount = count;
    list.pLibraries = libraries;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &list;
    info.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    {
        Timer timer(report_.pipeline_seconds);
        if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    if (p) {
        report_.pipelines_linked++;
    } else {
        approximate("pipelines without a stand-in: linking failed");
        e.stand_in_failed = true;
    }
    return p;
}

VkPipeline Impl::placeholder(const VkGraphicsPipelineCreateInfo &info) {
    VkPipelineShaderStageCreateInfo stages[5];
    VkGraphicsPipelineCreateInfo p = info;
    p.stageCount = std::min<uint32_t>(info.stageCount, 5);
    for (uint32_t i = 0; i < p.stageCount; i++) {
        stages[i] = info.pStages[i];
        if (stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) stages[i].module = placeholder_fs_;
    }
    p.pStages = stages;
    VkPipeline made = VK_NULL_HANDLE;
    {
        Timer timer(report_.pipeline_seconds);
        if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &p, nullptr, &made) != VK_SUCCESS)
            made = VK_NULL_HANDLE;
    }
    return made;
}

void Impl::retire_stand_in(PipelineEntry &e) {
    VkPipeline old = e.stand_in;
    e.stand_in = VK_NULL_HANDLE;
    cmd();
    slots_[current_].after.push_back([this, old] { vkDestroyPipeline(dev_, old, nullptr); });
}

// ---- the shader and pipeline storage (Options::shader_storage) ----
//
// docs/renderer.md, "Shaders and pipelines at the launcher's build step". A
// run appends every shader it meets and every pipeline's description to the
// title's storage; the build step (precompile) makes their libraries and
// whole pipelines into the pipeline cache and records which, for the device
// and driver; a later run on them makes those at once from the cache.

namespace {

// the description's format: a new one leaves the older descriptions unmade
constexpr uint32_t DESCRIPTION_VERSION = 1;

// The device and driver a pipeline cache is made for (the cache's header
// names the vendor, the device and the pipelineCacheUUID; the driver's
// version is kept beside them)
ShaderStorage::Built device_ids(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(physical, &p);
    ShaderStorage::Built b;
    b.vendor = p.vendorID, b.device = p.deviceID, b.driver = p.driverVersion;
    std::memcpy(b.uuid.data(), p.pipelineCacheUUID, 16);
    return b;
}

bool same_device(const ShaderStorage::Built &a, const ShaderStorage::Built &b) {
    return a.vendor == b.vendor && a.device == b.device && a.driver == b.driver && a.uuid == b.uuid;
}

// whether a pipeline cache file's header (VkPipelineCacheHeaderVersionOne)
// is the device's
bool cache_is_devices(const std::filesystem::path &file, const ShaderStorage::Built &ids) {
    std::FILE *f = std::fopen(file.string().c_str(), "rb");
    if (!f) return false;
    uint32_t header[8] = {};
    const bool read = std::fread(header, 4, 8, f) == 8;
    std::fclose(f);
    return read && header[0] >= 32 && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header[2] == ids.vendor &&
           header[3] == ids.device && std::memcmp(&header[4], ids.uuid.data(), 16) == 0;
}

} // namespace

void Impl::open_storage() {
    if (options_.shader_storage.empty()) return;
    storage_ = std::make_unique<ShaderStorage>();
    if (!storage_->open(options_.shader_storage)) {
        std::fprintf(stderr, "renderer: cannot open the shader storage %s\n", options_.shader_storage.string().c_str());
        storage_.reset();
        return;
    }
    // what the build step made, while the pipeline cache read is still of
    // this device and driver
    const ShaderStorage::Built ids = device_ids(d_.physical());
    if (!report_.pipeline_cache_read || !cache_is_devices(options_.pipeline_cache, ids)) return;
    for (const ShaderStorage::Built &b : storage_->built()) {
        if (!same_device(b, ids)) continue;
        built_libraries_.insert(b.libraries.begin(), b.libraries.end());
        built_pipelines_.insert(b.pipelines.begin(), b.pipelines.end());
    }
}

void Impl::module_words(VkShaderModule m, std::vector<uint32_t> &out) const {
    auto it = module_names_.find(m);
    if (it == module_names_.end()) {
        out.insert(out.end(), {0u, 0u, uint32_t(uint64_t(m)), uint32_t(uint64_t(m) >> 32)});
        return;
    }
    const ModuleName &n = it->second;
    out.insert(out.end(), {n.kind, n.form, uint32_t(n.id), uint32_t(n.id >> 32)});
}

// Vulkan ignores the values of the states a pipeline sets dynamically; they
// are set to one each (the topology to its class's first), so a pipeline's
// description is the same whichever draw asked for it first.
std::unique_ptr<PipelineDesc> Impl::canonical(const VkGraphicsPipelineCreateInfo &info) {
    auto d = std::make_unique<PipelineDesc>(info);
    if (!d->info.pDynamicState) return d; // (rasterization discarded: all of it fixed)
    const VkDynamicState *first = d->dynamic_states, *last = first + d->dynamic.dynamicStateCount;
    auto dynamic = [&](VkDynamicState s) { return std::find(first, last, s) != last; };
    if (d->info.pInputAssemblyState) {
        VkPrimitiveTopology &t = d->assembly.topology;
        if (dynamic(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY))
            t = t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST          ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : t <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP        ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : t == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST        ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                : t <= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN      ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                : t <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY
                                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        if (dynamic(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)) d->assembly.primitiveRestartEnable = VK_FALSE;
    }
    if (d->info.pRasterizationState) {
        VkPipelineRasterizationStateCreateInfo &r = d->raster;
        if (dynamic(VK_DYNAMIC_STATE_CULL_MODE)) r.cullMode = VK_CULL_MODE_NONE;
        if (dynamic(VK_DYNAMIC_STATE_FRONT_FACE)) r.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        if (dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE)) r.depthBiasEnable = VK_FALSE;
        if (dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS))
            r.depthBiasConstantFactor = r.depthBiasClamp = r.depthBiasSlopeFactor = 0.0f;
        if (dynamic(VK_DYNAMIC_STATE_LINE_WIDTH)) r.lineWidth = 1.0f;
    }
    if (d->info.pDepthStencilState) {
        VkPipelineDepthStencilStateCreateInfo &s = d->depth;
        if (dynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)) s.depthTestEnable = VK_FALSE;
        if (dynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)) s.depthWriteEnable = VK_FALSE;
        if (dynamic(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP)) s.depthCompareOp = VK_COMPARE_OP_NEVER;
        if (dynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)) s.stencilTestEnable = VK_FALSE;
        for (VkStencilOpState *face : {&s.front, &s.back}) {
            if (dynamic(VK_DYNAMIC_STATE_STENCIL_OP))
                face->failOp = face->passOp = face->depthFailOp = VK_STENCIL_OP_KEEP,
                face->compareOp = VK_COMPARE_OP_NEVER;
            if (dynamic(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) face->compareMask = 0;
            if (dynamic(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) face->writeMask = 0;
            if (dynamic(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) face->reference = 0;
        }
    }
    if (d->info.pColorBlendState) {
        VkPipelineColorBlendStateCreateInfo &b = d->blend;
        if (dynamic(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) std::fill_n(b.blendConstants, 4, 0.0f);
        for (uint32_t i = 0; i < b.attachmentCount; i++) {
            VkPipelineColorBlendAttachmentState &a = d->blends[i];
            if (dynamic(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT)) a.blendEnable = VK_FALSE;
            if (dynamic(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT))
                a.srcColorBlendFactor = a.dstColorBlendFactor = a.srcAlphaBlendFactor = a.dstAlphaBlendFactor =
                    VK_BLEND_FACTOR_ZERO,
                a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
            if (dynamic(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT)) a.colorWriteMask = 0;
        }
    }
    return d;
}

namespace {
const VkPipelineRenderingCreateInfo *rendering_of(const VkGraphicsPipelineCreateInfo &info) {
    for (auto *in = static_cast<const VkBaseInStructure *>(info.pNext); in; in = in->pNext)
        if (in->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            return reinterpret_cast<const VkPipelineRenderingCreateInfo *>(in);
    return nullptr;
}
const VkPipelineSampleLocationsStateCreateInfoEXT *locations_of(const VkPipelineMultisampleStateCreateInfo &m) {
    for (auto *in = static_cast<const VkBaseInStructure *>(m.pNext); in; in = in->pNext)
        if (in->sType == VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT)
            return reinterpret_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(in);
    return nullptr;
}
uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }
} // namespace

// The description: each state the pipeline has, its fields in order, and the
// modules by name; what the renderer's draws give and nothing else (one
// vertex binding and attribute, five stages, four targets, sample
// locations, the dynamic rendering formats).
std::vector<uint32_t> Impl::describe(const VkGraphicsPipelineCreateInfo &info) const {
    std::vector<uint32_t> w{DESCRIPTION_VERSION, info.flags, info.stageCount};
    if (info.layout != layout_ || info.stageCount > 5) return {};
    for (uint32_t i = 0; i < info.stageCount; i++) {
        const VkPipelineShaderStageCreateInfo &st = info.pStages[i];
        if (!module_names_.count(st.module) || std::strcmp(st.pName, "main") != 0) return {};
        w.push_back(uint32_t(st.stage));
        module_words(st.module, w);
        const VkSpecializationInfo *sp = st.pSpecializationInfo;
        if (!sp) {
            w.push_back(UINT32_MAX);
            continue;
        }
        if (sp->mapEntryCount > 16 || sp->dataSize > 64) return {};
        w.push_back(sp->mapEntryCount);
        for (uint32_t k = 0; k < sp->mapEntryCount; k++)
            w.insert(w.end(), {sp->pMapEntries[k].constantID, sp->pMapEntries[k].offset,
                               uint32_t(sp->pMapEntries[k].size)});
        w.push_back(uint32_t(sp->dataSize));
        uint32_t data[16] = {};
        std::memcpy(data, sp->pData, sp->dataSize);
        w.insert(w.end(), data, data + (sp->dataSize + 3) / 4);
    }
    const auto *vi = info.pVertexInputState;
    w.push_back(vi != nullptr);
    if (vi) {
        if (vi->vertexBindingDescriptionCount > 1 || vi->vertexAttributeDescriptionCount > 1) return {};
        w.push_back(vi->vertexBindingDescriptionCount);
        for (uint32_t k = 0; k < vi->vertexBindingDescriptionCount; k++) {
            const auto &b = vi->pVertexBindingDescriptions[k];
            w.insert(w.end(), {b.binding, b.stride, uint32_t(b.inputRate)});
        }
        w.push_back(vi->vertexAttributeDescriptionCount);
        for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount; k++) {
            const auto &a = vi->pVertexAttributeDescriptions[k];
            w.insert(w.end(), {a.location, a.binding, uint32_t(a.format), a.offset});
        }
    }
    const auto *ia = info.pInputAssemblyState;
    w.push_back(ia != nullptr);
    if (ia) w.insert(w.end(), {uint32_t(ia->topology), ia->primitiveRestartEnable});
    const auto *ts = info.pTessellationState;
    w.push_back(ts != nullptr);
    if (ts) w.push_back(ts->patchControlPoints);
    const auto *vp = info.pViewportState;
    w.push_back(vp != nullptr);
    if (vp) w.insert(w.end(), {vp->viewportCount, vp->scissorCount});
    const auto *rs = info.pRasterizationState;
    w.push_back(rs != nullptr);
    if (rs)
        w.insert(w.end(), {rs->depthClampEnable, rs->rasterizerDiscardEnable, uint32_t(rs->polygonMode), rs->cullMode,
                           uint32_t(rs->frontFace), rs->depthBiasEnable, bits(rs->depthBiasConstantFactor),
                           bits(rs->depthBiasClamp), bits(rs->depthBiasSlopeFactor), bits(rs->lineWidth)});
    const auto *ms = info.pMultisampleState;
    w.push_back(ms != nullptr);
    if (ms) {
        if (ms->pSampleMask) return {};
        w.insert(w.end(), {uint32_t(ms->rasterizationSamples), ms->sampleShadingEnable, bits(ms->minSampleShading),
                           ms->alphaToCoverageEnable, ms->alphaToOneEnable});
        const auto *sl = locations_of(*ms);
        w.push_back(sl != nullptr);
        if (sl) {
            const VkSampleLocationsInfoEXT &l = sl->sampleLocationsInfo;
            if (l.sampleLocationsCount > 4) return {};
            w.insert(w.end(), {sl->sampleLocationsEnable, uint32_t(l.sampleLocationsPerPixel),
                               l.sampleLocationGridSize.width, l.sampleLocationGridSize.height,
                               l.sampleLocationsCount});
            for (uint32_t k = 0; k < l.sampleLocationsCount; k++)
                w.insert(w.end(), {bits(l.pSampleLocations[k].x), bits(l.pSampleLocations[k].y)});
        }
    }
    const auto *ds = info.pDepthStencilState;
    w.push_back(ds != nullptr);
    if (ds) {
        w.insert(w.end(), {ds->depthTestEnable, ds->depthWriteEnable, uint32_t(ds->depthCompareOp),
                           ds->depthBoundsTestEnable, ds->stencilTestEnable});
        for (const VkStencilOpState *f : {&ds->front, &ds->back})
            w.insert(w.end(), {uint32_t(f->failOp), uint32_t(f->passOp), uint32_t(f->depthFailOp),
                               uint32_t(f->compareOp), f->compareMask, f->writeMask, f->reference});
        w.insert(w.end(), {bits(ds->minDepthBounds), bits(ds->maxDepthBounds)});
    }
    const auto *cb = info.pColorBlendState;
    w.push_back(cb != nullptr);
    if (cb) {
        if (cb->attachmentCount > 4) return {};
        w.insert(w.end(), {cb->logicOpEnable, uint32_t(cb->logicOp), cb->attachmentCount});
        for (uint32_t k = 0; k < cb->attachmentCount; k++) {
            const auto &a = cb->pAttachments[k];
            w.insert(w.end(), {a.blendEnable, uint32_t(a.srcColorBlendFactor), uint32_t(a.dstColorBlendFactor),
                               uint32_t(a.colorBlendOp), uint32_t(a.srcAlphaBlendFactor),
                               uint32_t(a.dstAlphaBlendFactor), uint32_t(a.alphaBlendOp), a.colorWriteMask});
        }
        for (float c : cb->blendConstants) w.push_back(bits(c));
    }
    const auto *dy = info.pDynamicState;
    w.push_back(dy != nullptr);
    if (dy) {
        if (dy->dynamicStateCount > 32) return {};
        w.push_back(dy->dynamicStateCount);
        for (uint32_t k = 0; k < dy->dynamicStateCount; k++) w.push_back(uint32_t(dy->pDynamicStates[k]));
    }
    const auto *rd = rendering_of(info);
    w.push_back(rd != nullptr);
    if (rd) {
        if (rd->colorAttachmentCount > 4) return {};
        w.insert(w.end(), {rd->viewMask, rd->colorAttachmentCount});
        for (uint32_t k = 0; k < rd->colorAttachmentCount; k++) w.push_back(uint32_t(rd->pColorAttachmentFormats[k]));
        w.insert(w.end(), {uint32_t(rd->depthAttachmentFormat), uint32_t(rd->stencilAttachmentFormat)});
    }
    return w;
}

std::unique_ptr<PipelineDesc> Impl::undescribe(std::span<const uint32_t> words, std::string &why) {
    struct Reader {
        std::span<const uint32_t> w;
        size_t at = 0;
        bool ok = true;
        uint32_t u() {
            if (at >= w.size()) return ok = false, 0u;
            return w[at++];
        }
        float f() { return std::bit_cast<float>(u()); }
    } r{words};
    auto fail = [&](const char *reason) {
        why = reason;
        return nullptr;
    };
    if (r.u() != DESCRIPTION_VERSION) return fail("a description of another version");
    VkGraphicsPipelineCreateInfo empty{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    auto d = std::make_unique<PipelineDesc>(empty);
    VkGraphicsPipelineCreateInfo &info = d->info;
    info.flags = r.u();
    const uint32_t stage_count = r.u();
    if (stage_count > 5) return fail("more stages than the renderer makes");
    for (uint32_t i = 0; i < stage_count && r.ok; i++) {
        VkPipelineShaderStageCreateInfo &st = d->stages[i];
        st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage = VkShaderStageFlagBits(r.u());
        st.pName = "main";
        const uint32_t kind = r.u(), form = r.u();
        const uint64_t id = r.u() | uint64_t(r.u()) << 32;
        if (kind == 1) {
            Shader *s = shader(id);
            if (!s) return fail("a shader the storage does not hold");
            std::string reason;
            if (!translate(*s, reason)) return fail("a shader the translator cannot translate");
            if (!form) {
                st.module = s->module;
            } else if (form >> 31) {
                shader::PixelOptions o;
                o.depth_float24 = uint8_t(form & 0xFF);
                o.centroid = uint16_t(form >> 8);
                o.fsi = uint8_t((form >> 24) & 0x3F);
                o.point_coordinates = (form >> 30) & 1;
                if (o.fsi) o.edram_library = std::span<const uint32_t>(EDRAM_FSI_LIBRARY);
                if ((o.key() | 1u << 31) != form) return fail("a pixel shader's form unknown here");
                st.module = pixel_module(*s, o, reason);
            } else {
                shader::VertexOptions o;
                o.host = shader::HostVertex(form & 0xFF);
                o.equal_spacing = (form >> 8) & 1;
                o.clip_planes = uint8_t((form >> 9) & 7);
                o.clip_planes_cull = (form >> 12) & 1;
                o.point_ucp_mode = uint8_t((form >> 13) & 3);
                if (o.key() != form) return fail("a vertex shader's form unknown here");
                st.module = vertex_module(*s, o, reason);
            }
        } else if (kind == 2) {
            for (const auto &[m, n] : module_names_)
                if (n.kind == 2 && n.id == id && n.form == form) st.module = m;
        }
        if (!st.module) return fail("a module the renderer does not make here");
        const uint32_t entries = r.u();
        if (entries == UINT32_MAX) continue;
        if (entries > 16) return fail("a specialization larger than the renderer's");
        for (uint32_t k = 0; k < entries; k++) d->spec_entries[k] = {r.u(), r.u(), size_t(r.u())};
        const uint32_t size = r.u();
        if (size > sizeof(d->spec_data)) return fail("a specialization larger than the renderer's");
        for (uint32_t k = 0; k < (size + 3) / 4; k++) d->spec_data[k] = r.u();
        d->spec = {entries, d->spec_entries, size, d->spec_data};
        st.pSpecializationInfo = &d->spec;
    }
    info.stageCount = stage_count;
    info.pStages = d->stages;
    if (r.u()) {
        VkPipelineVertexInputStateCreateInfo &v = d->vertex_input;
        v = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        v.vertexBindingDescriptionCount = r.u();
        if (v.vertexBindingDescriptionCount > 1) return fail("more vertex bindings than the renderer makes");
        for (uint32_t k = 0; k < v.vertexBindingDescriptionCount; k++)
            d->vertex_bindings[k] = {r.u(), r.u(), VkVertexInputRate(r.u())};
        v.vertexAttributeDescriptionCount = r.u();
        if (v.vertexAttributeDescriptionCount > 1) return fail("more vertex attributes than the renderer makes");
        for (uint32_t k = 0; k < v.vertexAttributeDescriptionCount; k++)
            d->vertex_attributes[k] = {r.u(), r.u(), VkFormat(r.u()), r.u()};
        v.pVertexBindingDescriptions = d->vertex_bindings;
        v.pVertexAttributeDescriptions = d->vertex_attributes;
        info.pVertexInputState = &v;
    }
    if (r.u()) {
        d->assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        d->assembly.topology = VkPrimitiveTopology(r.u());
        d->assembly.primitiveRestartEnable = r.u();
        info.pInputAssemblyState = &d->assembly;
    }
    if (r.u()) {
        d->tessellation = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        d->tessellation.patchControlPoints = r.u();
        info.pTessellationState = &d->tessellation;
    }
    if (r.u()) {
        d->viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        d->viewport.viewportCount = r.u();
        d->viewport.scissorCount = r.u();
        info.pViewportState = &d->viewport;
    }
    if (r.u()) {
        VkPipelineRasterizationStateCreateInfo &s = d->raster;
        s = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        s.depthClampEnable = r.u();
        s.rasterizerDiscardEnable = r.u();
        s.polygonMode = VkPolygonMode(r.u());
        s.cullMode = r.u();
        s.frontFace = VkFrontFace(r.u());
        s.depthBiasEnable = r.u();
        s.depthBiasConstantFactor = r.f();
        s.depthBiasClamp = r.f();
        s.depthBiasSlopeFactor = r.f();
        s.lineWidth = r.f();
        info.pRasterizationState = &s;
    }
    if (r.u()) {
        VkPipelineMultisampleStateCreateInfo &m = d->multisample;
        m = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        m.rasterizationSamples = VkSampleCountFlagBits(r.u());
        m.sampleShadingEnable = r.u();
        m.minSampleShading = r.f();
        m.alphaToCoverageEnable = r.u();
        m.alphaToOneEnable = r.u();
        if (r.u()) {
            VkPipelineSampleLocationsStateCreateInfoEXT &sl = d->sample_locations;
            sl = {VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
            sl.sampleLocationsEnable = r.u();
            VkSampleLocationsInfoEXT &l = sl.sampleLocationsInfo;
            l = {VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT};
            l.sampleLocationsPerPixel = VkSampleCountFlagBits(r.u());
            l.sampleLocationGridSize = {r.u(), r.u()};
            l.sampleLocationsCount = r.u();
            if (l.sampleLocationsCount > 4) return fail("more sample locations than the renderer sets");
            for (uint32_t k = 0; k < l.sampleLocationsCount; k++) d->locations[k] = {r.f(), r.f()};
            l.pSampleLocations = d->locations;
            m.pNext = &sl;
        }
        info.pMultisampleState = &m;
    }
    if (r.u()) {
        VkPipelineDepthStencilStateCreateInfo &s = d->depth;
        s = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        s.depthTestEnable = r.u();
        s.depthWriteEnable = r.u();
        s.depthCompareOp = VkCompareOp(r.u());
        s.depthBoundsTestEnable = r.u();
        s.stencilTestEnable = r.u();
        for (VkStencilOpState *f : {&s.front, &s.back})
            *f = {VkStencilOp(r.u()), VkStencilOp(r.u()), VkStencilOp(r.u()), VkCompareOp(r.u()), r.u(), r.u(), r.u()};
        s.minDepthBounds = r.f();
        s.maxDepthBounds = r.f();
        info.pDepthStencilState = &s;
    }
    if (r.u()) {
        VkPipelineColorBlendStateCreateInfo &b = d->blend;
        b = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        b.logicOpEnable = r.u();
        b.logicOp = VkLogicOp(r.u());
        b.attachmentCount = r.u();
        if (b.attachmentCount > 4) return fail("more targets than the renderer binds");
        for (uint32_t k = 0; k < b.attachmentCount; k++) {
            VkPipelineColorBlendAttachmentState &a = d->blends[k];
            a.blendEnable = r.u();
            a.srcColorBlendFactor = VkBlendFactor(r.u());
            a.dstColorBlendFactor = VkBlendFactor(r.u());
            a.colorBlendOp = VkBlendOp(r.u());
            a.srcAlphaBlendFactor = VkBlendFactor(r.u());
            a.dstAlphaBlendFactor = VkBlendFactor(r.u());
            a.alphaBlendOp = VkBlendOp(r.u());
            a.colorWriteMask = r.u();
        }
        b.pAttachments = d->blends;
        for (float &c : b.blendConstants) c = r.f();
        info.pColorBlendState = &b;
    }
    if (r.u()) {
        VkPipelineDynamicStateCreateInfo &y = d->dynamic;
        y = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        y.dynamicStateCount = r.u();
        if (y.dynamicStateCount > 32) return fail("more dynamic states than the renderer sets");
        for (uint32_t k = 0; k < y.dynamicStateCount; k++) d->dynamic_states[k] = VkDynamicState(r.u());
        y.pDynamicStates = d->dynamic_states;
        info.pDynamicState = &y;
    }
    if (r.u()) {
        VkPipelineRenderingCreateInfo &g = d->rendering;
        g = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        g.viewMask = r.u();
        g.colorAttachmentCount = r.u();
        if (g.colorAttachmentCount > 4) return fail("more targets than the renderer binds");
        for (uint32_t k = 0; k < g.colorAttachmentCount; k++) d->color_formats[k] = VkFormat(r.u());
        g.pColorAttachmentFormats = d->color_formats;
        g.depthAttachmentFormat = VkFormat(r.u());
        g.stencilAttachmentFormat = VkFormat(r.u());
        info.pNext = &g;
    }
    info.layout = layout_;
    if (!r.ok || r.at != words.size()) return fail("a description cut short or too long");
    return d;
}

// The libraries' keys: the vertex input's attribute and topology; the
// pre-rasterization stages' modules with the patch size and the fixed
// rasterization state; the pixel shader's module (and specialization) with
// the samples; the targets' formats and samples (with their blend state where
// it is not dynamic). The modules by name, so the keys are the same from run
// to run, as the build step makes them.
void Impl::library_keys(const VkGraphicsPipelineCreateInfo &info, PipelineParts &parts) const {
    const uint32_t dynamic = info.pDynamicState ? info.pDynamicState->dynamicStateCount : 0;
    const auto *vi = info.pVertexInputState;
    const auto *ia = info.pInputAssemblyState;
    parts.keys[0] = {0, dynamic,
                     vi && vi->vertexAttributeDescriptionCount ? uint32_t(vi->pVertexAttributeDescriptions[0].format) : 0u,
                     vi && vi->vertexBindingDescriptionCount ? vi->pVertexBindingDescriptions[0].stride : 0u,
                     ia ? uint32_t(ia->topology) : 0u, ia ? uint32_t(ia->primitiveRestartEnable) : 0u};
    const auto *rs = info.pRasterizationState;
    parts.keys[1] = {1,
                     dynamic,
                     info.pTessellationState ? info.pTessellationState->patchControlPoints : 0u,
                     info.pViewportState != nullptr,
                     rs ? uint32_t(rs->depthClampEnable) : 0u,
                     rs ? uint32_t(rs->rasterizerDiscardEnable) : 0u,
                     rs ? uint32_t(rs->polygonMode) : 0u,
                     rs ? uint32_t(rs->cullMode) : 0u,
                     rs ? uint32_t(rs->frontFace) : 0u,
                     rs ? uint32_t(rs->depthBiasEnable) : 0u};
    const VkPipelineShaderStageCreateInfo *fragment = nullptr;
    for (uint32_t i = 0; i < info.stageCount; i++) {
        if (info.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            fragment = &info.pStages[i];
            continue;
        }
        parts.keys[1].push_back(info.pStages[i].stage);
        module_words(info.pStages[i].module, parts.keys[1]);
    }
    parts.keys[2].clear(), parts.keys[3].clear();
    const auto *ms = info.pMultisampleState;
    if (!ms) return; // rasterization discarded: no fragment parts
    const auto *sl = locations_of(*ms);
    const uint32_t samples = uint32_t(ms->rasterizationSamples), shading = ms->sampleShadingEnable,
                   located = sl && sl->sampleLocationsEnable;
    parts.keys[2] = {2, dynamic, samples, shading, located};
    if (fragment) {
        module_words(fragment->module, parts.keys[2]);
        if (const VkSpecializationInfo *sp = fragment->pSpecializationInfo) {
            uint32_t data[16] = {};
            std::memcpy(data, sp->pData, std::min<size_t>(sp->dataSize, sizeof(data)));
            parts.keys[2].insert(parts.keys[2].end(), data, data + (std::min<size_t>(sp->dataSize, 64) + 3) / 4);
        }
    }
    const auto *rd = rendering_of(info);
    parts.keys[3] = {3, dynamic, samples, shading, located};
    if (rd) {
        parts.keys[3].insert(parts.keys[3].end(), {uint32_t(rd->depthAttachmentFormat),
                                                   uint32_t(rd->stencilAttachmentFormat), rd->colorAttachmentCount});
        for (uint32_t k = 0; k < rd->colorAttachmentCount; k++)
            parts.keys[3].push_back(uint32_t(rd->pColorAttachmentFormats[k]));
    }
    if (const auto *cb = info.pColorBlendState) {
        parts.keys[3].push_back(cb->attachmentCount);
        for (uint32_t k = 0; k < cb->attachmentCount; k++) {
            const auto &a = cb->pAttachments[k];
            parts.keys[3].insert(parts.keys[3].end(),
                                 {a.blendEnable, uint32_t(a.srcColorBlendFactor), uint32_t(a.dstColorBlendFactor),
                                  uint32_t(a.colorBlendOp), uint32_t(a.srcAlphaBlendFactor),
                                  uint32_t(a.dstAlphaBlendFactor), uint32_t(a.alphaBlendOp), a.colorWriteMask});
        }
    }
}

// The build step's part (render::precompile, on its renderer): the storage's
// shaders and the extra ones translated, its descriptions' libraries and
// whole pipelines made on the workers into the pipeline cache, which is
// written, and what was made recorded in the storage.
bool Impl::precompile(const PrecompileOptions &o, PrecompileReport &out, std::string &error) {
    const auto started = std::chrono::steady_clock::now();
    out = {};
    out.cache_read = report_.pipeline_cache_read;
    ShaderStorage storage;
    if (options_.shader_storage.empty() || !storage.open(options_.shader_storage))
        return error = "cannot open the shader storage " + options_.shader_storage.string(), false;
    const ShaderStorage::Built ids = device_ids(d_.physical());
    out.cache_matched = report_.pipeline_cache_read && cache_is_devices(options_.pipeline_cache, ids);
    // what earlier build steps made into this cache: not made again
    std::unordered_set<uint64_t> made_libraries, made_pipelines;
    if (out.cache_matched)
        for (const ShaderStorage::Built &b : storage.built())
            if (same_device(b, ids)) {
                made_libraries.insert(b.libraries.begin(), b.libraries.end());
                made_pipelines.insert(b.pipelines.begin(), b.pipelines.end());
            }
    auto cancelled = [&] { return o.cancel && o.cancel->load(); };

    // the shaders, translated in their first form (the others as the
    // descriptions ask for them)
    std::string why;
    for (const ShaderStorage::Shader &s : storage.shaders()) {
        load_shader(s.type, s.code);
        Shader *sh = shader(s.hash);
        out.shaders++;
        if (!sh || !translate(*sh, why)) out.shaders_failed++;
    }
    for (const auto &[type, code] : o.extra_shaders) {
        if (type > 1) continue;
        const uint64_t hash = hash_words(type, code);
        out.extra_shaders++;
        if (!storage.has_shader(hash)) out.extra_new++;
        load_shader(type, code);
        Shader *sh = shader(hash);
        // a vertex shader whose fetches Direct3D fills in from the vertex
        // declaration at bind time: the copy a draw runs is another shader
        if (sh && xf::ucode::awaits_vertex_declaration(sh->parsed)) {
            out.extra_undeclared++;
            continue;
        }
        if (!sh || !translate(*sh, why)) out.extra_failed++;
    }
    if (cancelled()) return error = "cancelled", false;

    // the descriptions' libraries and whole pipelines, queued for the
    // workers, each library once
    struct Made {
        uint64_t hash = 0;
        bool library = false;
        std::unique_ptr<PipelineEntry> entry;
    };
    std::vector<Made> made;
    std::unordered_set<uint64_t> queued_libraries;
    ShaderStorage::Built record = ids;
    const bool libraries = d_.pipeline_library();
    for (const std::vector<uint32_t> &words : storage.pipelines()) {
        out.descriptions++;
        const uint64_t hash = ShaderStorage::hash(0, words);
        if (made_pipelines.count(hash)) {
            out.descriptions_done_before++;
            continue;
        }
        std::unique_ptr<PipelineDesc> desc = undescribe(words, why);
        if (!desc) {
            out.descriptions_failed++;
            if (options_.verbose) std::fprintf(stderr, "  description %016llX: %s\n", (unsigned long long)hash, why.c_str());
            continue;
        }
        if (libraries) {
            PipelineParts parts;
            library_keys(desc->info, parts);
            for (uint32_t part = 0; part < 4; part++) {
                if (parts.keys[part].empty()) continue;
                const uint64_t lh = ShaderStorage::hash(part, parts.keys[part]);
                if (made_libraries.count(lh) || !queued_libraries.insert(lh).second) continue;
                Made m{lh, true, std::make_unique<PipelineEntry>()};
                LibraryDesc ld(desc->info, part);
                pipeline_workers_->queue(m.entry.get(), std::make_unique<PipelineDesc>(ld.info),
                                         PipelinePriority::normal);
                made.push_back(std::move(m));
            }
        }
        if (o.whole_pipelines) {
            Made m{hash, false, std::make_unique<PipelineEntry>()};
            pipeline_workers_->queue(m.entry.get(), std::move(desc), PipelinePriority::normal);
            made.push_back(std::move(m));
        }
    }
    // waited for in order, with the progress
    for (size_t i = 0; i < made.size(); i++) {
        if (cancelled()) {
            pipeline_workers_.reset(); // stopped: what is under way finishes, the rest is left
            break;
        }
        pipeline_workers_->wait(made[i].entry.get());
        if (o.progress) o.progress(i + 1, made.size());
    }
    const bool stopped = !pipeline_workers_;
    for (Made &m : made) {
        if (!m.entry->state.load(std::memory_order_acquire)) continue;
        const bool ok = m.entry->pipeline != VK_NULL_HANDLE;
        if (m.library)
            (ok ? out.libraries : out.libraries_failed)++;
        else
            (ok ? out.pipelines : out.pipelines_failed)++;
        if (ok) (m.library ? record.libraries : record.pipelines).push_back(m.hash);
        if (m.entry->pipeline) vkDestroyPipeline(dev_, m.entry->pipeline, nullptr);
        m.entry->pipeline = VK_NULL_HANDLE;
    }
    out.driver_seconds = stopped ? 0.0 : pipeline_workers_->seconds();
    // the cache written when anything was made, and the record appended
    report_.pipelines = out.libraries + out.pipelines;
    pipelines_saved_ = 0;
    if (report_.pipelines) save_pipeline_cache();
    out.cache_written = report_.pipeline_cache_written;
    if (report_.pipelines && !out.cache_written) return error = "cannot write the pipeline cache", false;
    if (!record.libraries.empty() || !record.pipelines.empty()) storage.add_built(record);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (stopped) return error = "cancelled", false;
    return true;
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
Viewport host_viewport(const uint32_t *regs, float max_x, float max_y, bool pixel_writes_depth, bool depth_float24,
                       bool float24_convert) {
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
        // the bounds the depth is clamped to after the pixel shader, as 20e4
        // values when the pixel shader converts it (rounded to the nearest
        // even whatever the depth's rounding, as Xenia's)
        if (float24_convert) {
            z_min = float24_to_float(float_to_float24(z_min, true));
            z_max = float24_to_float(float_to_float24(z_max, true));
        }
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
// draw, every target there stored and loaded again (a racing game's race:
// its depth target took 2048 tiles three times a frame). A tessellated draw's
// vertices are not run (the estimator's "not reproducing tessellation").
uint32_t Impl::draw_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                          uint32_t primitive, bool tessellated) const {
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
        if (scissor_bottom >= 8192 && scissor_right >= 8192 && !tessellated)
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

// A draw outside copy mode, as the processor gives it (immediate indices in
// the draw, which Xenia does not draw).
void Impl::draw(const gpu::Draw &d) {
    report_.draws++;
    // VIZ queries, as Xenia's default (occlusion_query_viz off), counted:
    // draws predicated on one (DRAW_INDX's VIZ token, bit 8) are drawn, and
    // survey draws (PA_SC_VIZ_QUERY's viz_query_ena with kill_pix_post_hi_z)
    // are drawn as they are
    if (d.viz_query & 0x100) use("draws predicated on a VIZ query (drawn, as Xenia's default)");
    if ((regs_[reg::PA_SC_VIZ_QUERY] & 0x81) == 0x81) use("VIZ survey draws (drawn, as Xenia's default)");
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
        // primitive. A UE3 game issues hundreds of such point lists a
        // frame.
        const Shader *v = have_vertex_ ? shader(vertex_hash_) : nullptr;
        const Shader *p = have_pixel_ ? shader(pixel_hash_) : nullptr;
        bool exports = (v && v->summary.memexport) || (color && p && p->summary.memexport);
        if (!writes_targets && !exports) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
        // the port draws this frame (native_render.h): the draws its filter
        // selects skipped, never one exporting memory (the CPU may read it)
        if (native_suppress_) {
            if (exports) {
                native::detail::bump(native::detail::atomic_counters().memexport_kept);
            } else {
                native::PassInfo pass;
                pass.color = color, pass.has_pixel_shader = p != nullptr;
                pass.primitive = primitive, pass.count = count;
                pass.vertex_shader = have_vertex_ ? vertex_hash_ : 0, pass.pixel_shader = have_pixel_ ? pixel_hash_ : 0;
                if (!native_keep(pass)) {
                    if (zpd_current_) zpd_counts_[zpd_current_].suppressed = true;
                    return;
                }
            }
        }
    }
    // What the draw asks for, counted whether it is drawn or not
    // (Report::used): user clip planes (PA_CL_CLIP_CNTL's ucp_ena_0-5 while
    // clipping is on; on points with ps_ucp_mode), and lines of another width
    // than one pixel (PA_SU_LINE_CNTL's WIDTH, half the width in 12.4 fixed
    // point: 8 is Xenia's reset value, one pixel)
    {
        uint32_t clip = regs_[r::PA_CL_CLIP_CNTL];
        if ((clip & 0x3F) && !((clip >> 16) & 1)) {
            use((clip >> 17) & 1 ? "draws with user clip planes, cull only" : "draws with user clip planes");
            if (primitive == 1)
                use("point lists with user clip planes, ps_ucp_mode " + std::to_string((clip >> 14) & 3));
        }
        if (primitive == 2 || primitive == 3 || primitive == 12) {
            uint32_t width = regs_[r::PA_SU_LINE_CNTL] & 0xFFFF;
            if (width != 8) {
                char what[96];
                std::snprintf(what, sizeof(what), "line draws %g pixels wide (PA_SU_LINE_CNTL)", double(width) / 8.0);
                use(what);
            }
        }
    }
    // Tessellation, as Xenia's PrimitiveProcessor::Process takes it:
    // VGT_OUTPUT_PATH_CNTL's path_select in the explicit major mode (games
    // leave the register set after their tessellated draws, for draws in the
    // implicit mode that do not tessellate). The guest's vertex shader runs as
    // the domain shader at the points the tessellator makes, after the host's
    // vertex and hull shaders (tessellation.vert and .tesc), as Xenia's D3D12
    // backend draws it; primitive types 16 to 18 are line, triangle and quad
    // patches then. Only what Xenia draws: triangle lists, fans and strips
    // and quad lists in the discrete and continuous modes (indexed by control
    // point), triangle and quad patches in any of the three (indexed by
    // patch); not line patches (a TODO in Xenia), nor the rest.
    const bool tessellated = d.major_mode_explicit && (regs_[r::VGT_OUTPUT_PATH_CNTL] & 3) == 1;
    const uint32_t tess_mode = regs_[r::VGT_HOS_CNTL] & 3;
    shader::VertexOptions host_vertex;
    uint32_t patch_points = 0, hull = 0; // control points of a host patch; its hull shader (tessellation_hs_)
    std::string draw_name = primitive_name(primitive); // for the verbose lines and the GPU timing
    if (tessellated) {
        static const char *const modes[] = {"discrete", "continuous", "adaptive", "mode 3"};
        static const char *const patches[] = {"line patch", "triangle patch", "quad patch"};
        const char *kind = primitive >= 16 && primitive <= 18 ? patches[primitive - 16] : primitive_name(primitive);
        std::string what = std::string(modes[tess_mode]) + " " + kind;
        draw_name = "tessellated " + what;
        using HV = shader::HostVertex;
        switch (primitive) {
        case 4:
        case 5:
        case 6:
            if (tess_mode <= 1) host_vertex.host = HV::TriangleDomainCPIndexed, patch_points = 3, hull = 1;
            break;
        case 13:
            if (tess_mode <= 1) host_vertex.host = HV::QuadDomainCPIndexed, patch_points = 4, hull = 2;
            break;
        case 17:
            if (tess_mode <= 2)
                host_vertex.host = HV::TriangleDomainPatchIndexed, patch_points = tess_mode == 2 ? 3 : 1,
                hull = tess_mode == 2 ? 3 : 0;
            break;
        case 18:
            if (tess_mode <= 2)
                host_vertex.host = HV::QuadDomainPatchIndexed, patch_points = tess_mode == 2 ? 4 : 1,
                hull = tess_mode == 2 ? 4 : 0;
            break;
        }
        if (host_vertex.host == HV::Vertex) return skip("tessellation: " + what);
        // the device's tessellation shaders are optional (most mobile GPUs
        // lack them)
        if (!d_.tessellation_shader()) return skip("tessellation without tessellation shaders: " + what);
        // adaptive tessellation's edge factors are 32-bit floats in an index
        // buffer (Xenia fails the draw otherwise)
        if (tess_mode == 2 && !(d.indexed && d.index32))
            return skip("adaptive tessellation without 32-bit edge factors");
        host_vertex.equal_spacing = tess_mode == 0;
    }
    bool rectangles = !tessellated && primitive == 8, points = !tessellated && primitive == 1;
    // Rectangle and point lists: expanded by geometry shaders, as Xenia's
    // D3D12 backend does; without them (or with XF_VK_EXPAND_IN_VS=1) in the
    // vertex shader, as Xenia's Vulkan backend expands points without them
    // (its rectangles are a TODO there, done here as its
    // HostVertexShaderType describes them: the three guest vertices in each
    // host invocation).
    // User clip planes (PA_CL_CLIP_CNTL's ucp_ena_0-5, while clipping is on),
    // as Xenia's: their distances in the vertex shader (shader::VertexOptions),
    // the planes packed in the system constants. Points and rectangles are
    // expanded in the vertex shader then, where the corners' distances are
    // made (ps_ucp_mode); without clip or cull distances on the device the
    // draw is skipped.
    const uint32_t clip_cntl = regs_[r::PA_CL_CLIP_CNTL];
    const uint32_t ucp = (clip_cntl >> 16) & 1 ? 0u : clip_cntl & 0x3F;
    if (ucp) {
        host_vertex.clip_planes = uint8_t(std::popcount(ucp));
        host_vertex.clip_planes_cull = (clip_cntl >> 17) & 1;
        if (points) host_vertex.point_ucp_mode = uint8_t((clip_cntl >> 14) & 3);
        bool cull = host_vertex.clip_planes_cull || (points && host_vertex.point_ucp_mode == 2);
        bool clip = !host_vertex.clip_planes_cull;
        if ((cull && !d_.cull_distance()) || (clip && !d_.clip_distance()))
            return skip("user clip planes without clip or cull distances on the device");
        if (points && host_vertex.point_ucp_mode == 2 && !host_vertex.clip_planes_cull && host_vertex.clip_planes > 4)
            approximate("points with ps_ucp_mode 2 and more than 4 user clip planes (clipped by the corners only)");
    }
    if ((expand_in_vs_ || ucp) && (rectangles || points))
        host_vertex.host =
            points ? shader::HostVertex::PointListAsTriangleStrip : shader::HostVertex::RectangleListAsTriangleStrip;
    bool expanded = host_vertex.host == shader::HostVertex::PointListAsTriangleStrip ||
                    host_vertex.host == shader::HostVertex::RectangleListAsTriangleStrip;
    // triangles, quads and polygons (draw_util.h's IsPrimitivePolygonal, the
    // triangle and quad patches with it): faces, culling, the front and back
    // polygon offsets
    bool polygonal = (primitive >= 4 && primitive <= 7) || (primitive >= 13 && primitive <= 15) || tessellated;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    // (a tessellated draw's are patches)
    switch (tessellated ? UINT32_MAX : primitive) {
    case UINT32_MAX: break;
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
    // the types the reset index cuts (Xenia's ProcessIndexBuffer list, by the
    // guest's type: tessellated strips and fans too, patches not, adaptive
    // tessellation's edge factors not)
    bool strip = (primitive == 3 || primitive == 5 || primitive == 6 || primitive == 12 || primitive == 14 ||
                  primitive == 15 || primitive == 0x15 || primitive == 0x16) &&
                 !(tessellated && tess_mode == 2);

    // shaders
    Shader *vs = have_vertex_ ? shader(vertex_hash_) : nullptr;
    if (!vs) return skip("no vertex shader");
    Shader *ps = color && have_pixel_ ? shader(pixel_hash_) : nullptr;
    std::string why;
    if (!translate(*vs, why)) return skip("vertex shader: " + why);
    if (ps && !translate(*ps, why)) return skip("pixel shader: " + why);
    for (const Shader *s : {vs, ps})
        if (s)
            for (const std::string &a : s->approximate) approximate(a);

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
        // a domain shader's exports would need the tessellator without the
        // rasterizer (not done)
        if (tessellated) {
            skip("memory export from a tessellated draw that rasterizes nothing");
            return false;
        }
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
    bool any_gamma = false;
    uint32_t mask = regs_[reg::RB_COLOR_MASK];
    // the interlock path makes no render targets: their places in EDRAM
    // (edram_view), for the draw's extent
    RenderTarget views[5];
    bool viewed[5] = {};
    for (uint32_t i = 0; i < 4 && ps; i++) {
        color_masks[i] = (mask >> (4 * i)) & 0xF;
        if (!color_masks[i] || !(ps->colors_written & (1u << i))) {
            color_masks[i] = 0;
            continue;
        }
        uint32_t info = regs_[r::COLOR_INFO[i]], format = (info >> 16) & 0xF;
        if (format == 1) any_gamma = true;
        if (fsi_) {
            if (format == 8 || format == 9 || format == 11 || format == 13)
                return skip(std::string("color target format ") + color_target_format_name(format));
            views[i] = edram_view(false, info & 0xFFF, pitch, msaa, format);
            viewed[i] = true;
            continue;
        }
        // the exponent bias, and 16-bit fixed (-32..32, 10 fraction bits) kept as snorm
        color_scales[i] = std::ldexp(1.0f, sign_extend((info >> 20) & 0x3F, 6));
        if (format == 4 || format == 5) color_scales[i] *= 1024.0f / 32767.0f;
        colors[i] = render_target(false, info & 0xFFF, pitch, msaa, format);
        if (!colors[i]) return fatal_.empty() ? skip(std::string("color target format ") + color_target_format_name(format)) : void();
        // 8_8_8_8_GAMMA: the shader writes linear into a target kept linear,
        // else the gamma curve (blended in gamma then, unlike the console)
        if (format == 1 && !colors[i]->gamma_linear) gamma_targets |= 1u << i;
    }
    if (any_gamma) use("draws into 8_8_8_8_GAMMA color targets");
    if (gamma_targets) approximate("8_8_8_8_GAMMA color targets blended in gamma (no RGBA16 unorm attachments)");
    uint32_t depth_control = export_only ? 0 : regs_[reg::RB_DEPTHCONTROL];
    RenderTarget *depth = nullptr;
    if (depth_control & 3) {
        uint32_t info = regs_[reg::RB_DEPTH_INFO], format = (info >> 16) & 1;
        if (fsi_) {
            views[4] = edram_view(true, info & 0xFFF, pitch, msaa, format);
            viewed[4] = true;
        } else {
            if (format == 1 && !options_.depth_float24_convert_in_pixel_shader)
                approximate("D24FS8 depth as 32-bit float");
            depth = render_target(true, info & 0xFFF, pitch, msaa, format);
            if (!depth) return;
        }
    } else {
        depth_control = 0;
    }
    uint32_t width = 8192, height = 8192;
    for (RenderTarget *t : colors)
        if (t) width = std::min(width, t->width), height = std::min(height, t->height);
    if (depth) width = std::min(width, depth->width), height = std::min(height, depth->height);
    for (uint32_t i = 0; i < 5; i++)
        if (viewed[i]) width = std::min(width, views[i].width), height = std::min(height, views[i].height);
    if (width == 8192 && height == 8192 && !export_only && !rasterizes_nothing()) {
        // no target the pixel shader writes and no depth: nothing it could change
        if (options_.verbose)
            std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
        return;
    }

    // viewport and scissor
    bool depth_float24 = depth && depth->format == 1;
    // D24FS8 depth converted to 20e4 in the pixel shader (Options), for draws
    // with the depth test on, as Xenia's depth_float24_convert_in_pixel_shader
    bool float24_convert = options_.depth_float24_convert_in_pixel_shader && depth_float24 && (depth_control & 2);
    Viewport viewport = host_viewport(regs_, max_viewport_[0], max_viewport_[1], ps && ps->writes_depth,
                                      depth_float24 && (depth_control & 2), float24_convert);
    VkRect2D scissor = host_scissor(regs_, pitch);
    scissor.extent.width = std::min<uint32_t>(scissor.extent.width, width - std::min<uint32_t>(scissor.offset.x, width));
    scissor.extent.height = std::min<uint32_t>(scissor.extent.height, height - std::min<uint32_t>(scissor.offset.y, height));
    if (!export_only && (viewport.empty || !scissor.extent.width || !scissor.extent.height) && !rasterizes_nothing()) return;
    if (export_only) {
        std::fill(std::begin(colors), std::end(colors), nullptr);
        std::fill(std::begin(viewed), std::end(viewed), false);
        depth = nullptr, depth_control = 0, float24_convert = false;
    }
    // The interlock path's render target state (edram_fsi.frag's Fsi block),
    // as Xenia's UpdateSystemConstantValues makes it: per color target its
    // format, write mask and base (the window offset's tiles not added, as
    // the host path's), its blending and exponent bias; the depth and
    // stencil tests (draw_util.cc's GetNormalizedDepthControl: a depth test
    // that neither writes nor rejects is off), off where a color target over
    // the same base writes bits they use (ColorOverlapsDepthStencil); the
    // polygon offset in depth units (the slope's in 1/16 pixels) and the
    // stencil's faces (the back's only for polygons with backface_enable).
    FsiConstants fsi_constants{};
    if (fsi_ && !export_only) {
        FsiConstants &f = fsi_constants;
        uint32_t flags = msaa | (msaa == 1 && fsi_2x_as_4x_ ? FSI_2X_AS_4X : 0u);
        uint32_t dc = viewed[4] ? depth_control : 0;
        if ((dc & 2) && !(dc & 4) && ((dc >> 4) & 7) == 7) dc &= ~2u;
        bool depth_stencil = (dc & 3) != 0;
        uint32_t depth_base = regs_[reg::RB_DEPTH_INFO] & 0xFFF;
        for (uint32_t i = 0; i < 4; i++) {
            if (!viewed[i]) continue;
            uint32_t info = regs_[r::COLOR_INFO[i]], format = (info >> 16) & 0xF;
            f.rt[i] = format | color_masks[i] << 4 | 0x100u;
            f.blend[i] = regs_[r::RB_BLENDCONTROL[i]] & 0x1FFF1FFF;
            f.rt_base[i] = ((info & 0xFFF) % EDRAM_TILES) * 1280;
            f.exp_bias[i] = std::ldexp(1.0f, sign_extend((info >> 20) & 0x3F, 6));
            if (depth_stencil && (info & 0xFFF) % EDRAM_TILES == depth_base % EDRAM_TILES) {
                // the bits the color target writes (render_target_cache.cc's
                // GetPSIColorFormatInfo keep masks), against those depth (bits
                // 8-31) and stencil (0-7) use
                uint32_t m = color_masks[i], written_bits = 0;
                switch (format) {
                case 0: case 1:
                    for (uint32_t k = 0; k < 4; k++)
                        if (m & (1u << k)) written_bits |= 0xFFu << (8 * k);
                    break;
                case 2: case 3: case 10: case 12:
                    for (uint32_t k = 0; k < 3; k++)
                        if (m & (1u << k)) written_bits |= 0x3FFu << (10 * k);
                    if (m & 8) written_bits |= 3u << 30;
                    break;
                case 4: case 6: written_bits = (m & 1 ? 0xFFFFu : 0u) | (m & 2 ? 0xFFFF0000u : 0u); break;
                case 5: case 7: // either half, conservatively
                    written_bits = (m & 5 ? 0xFFFFu : 0u) | (m & 10 ? 0xFFFF0000u : 0u);
                    break;
                case 14: written_bits = m & 1 ? ~0u : 0u; break;
                case 15: written_bits = m & 3 ? ~0u : 0u; break;
                }
                uint32_t used = (dc & 2 ? 0xFFFFFF00u : 0u) | (dc & 1 ? 0xFFu : 0u);
                if (written_bits & used) depth_stencil = false;
            }
        }
        if (depth_stencil) {
            flags |= FSI_DEPTH_STENCIL;
            if (dc & 2) {
                flags |= ((dc >> 4) & 7) * FSI_DEPTH_LESS;
                if (dc & 4) flags |= FSI_DEPTH_WRITE;
            } else {
                flags |= 7 * FSI_DEPTH_LESS; // stencil alone: depth always passes, kept
            }
            if (dc & 1) flags |= FSI_STENCIL;
        }
        if (viewed[4] && views[4].format == 1) flags |= FSI_FLOAT24;
        f.flags = flags;
        f.depth_base = (depth_base % EDRAM_TILES) * 1280;
        f.pitch = std::max(1u, (pitch * (msaa >= 2 ? 2 : 1) + 79) / 80) * 1280;
        f.zpd_index = UINT32_MAX; // (set as the draw is recorded)
        uint32_t stencil = regs_[r::RB_STENCILREFMASK], stencil_bf = regs_[r::RB_STENCILREFMASK_BF];
        f.stencil[0] = f.stencil[2] = stencil & 0xFFFFFF;
        f.stencil[1] = f.stencil[3] = (dc >> 8) & 0xFFF;
        if (polygonal && (dc >> 7) & 1) f.stencil[2] = stencil_bf & 0xFFFFFF, f.stencil[3] = (dc >> 20) & 0xFFF;
        const float *offsets = reinterpret_cast<const float *>(&regs_[r::PA_SU_POLY_OFFSET_FRONT_SCALE]);
        uint32_t sc_mode = regs_[r::PA_SU_SC_MODE_CNTL];
        if (polygonal) {
            if ((sc_mode >> 11) & 1) f.poly_offset[0] = offsets[0], f.poly_offset[1] = offsets[1];
            if ((sc_mode >> 12) & 1) f.poly_offset[2] = offsets[2], f.poly_offset[3] = offsets[3];
        } else if ((sc_mode >> 13) & 1) {
            f.poly_offset[0] = f.poly_offset[2] = offsets[0];
            f.poly_offset[1] = f.poly_offset[3] = offsets[1];
        }
        f.poly_offset[0] *= 1.0f / 16.0f, f.poly_offset[2] *= 1.0f / 16.0f;
        std::memcpy(f.blend_constant, &regs_[r::RB_BLEND_RED], 16);
    }
    // (the targets take over their EDRAM tiles below, once the draw's lowest
    // row is known: draw_max_y)

    // the pages of the streams the shaders export to (the pixel shader's only
    // when it runs)
    std::vector<Range> exported;
    export_ranges(*vs, 0, exported);
    if (ps) export_ranges(*ps, 1, exported);
    bool exports = vs->exports || (ps && ps->exports);

    // indices: the guest's, swapped, plus the index offset, cut to 24 bits
    // and clamped to VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX (as Xenia's vertex
    // shaders do it: RemapAndConvertVertexIndices), reset indices kept as the
    // host's restart index; adaptive tessellation's edge factors only swapped
    std::vector<uint32_t> indices;
    indices.reserve(count);
    uint32_t offset = regs_[r::VGT_INDX_OFFSET];
    bool restart = strip && (regs_[r::PA_SU_SC_MODE_CNTL] >> 21) & 1;
    uint32_t reset_index = regs_[r::VGT_MULTI_PRIM_IB_RESET_INDX];
    const uint32_t min_index = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
    auto remap = [&](uint32_t index) { return std::min(max_index, std::max(min_index, (index + offset) & 0xFFFFFF)); };
    const bool factors = tessellated && tess_mode == 2;
    auto vertex = [&](uint32_t index) { return factors ? index : remap(index); };
    if (d.indexed && !d.immediate.empty()) {
        // immediate indices, from the packet (gpu::Draw::immediate): no
        // swap, 16-bit ones two a word, the first in the low half (AMD's
        // DRAW_INDEX_IMMD); then as an index buffer's
        use("draws with immediate indices");
        if (!index32) approximate("immediate 16-bit indices (their order in a word unverified)");
        for (uint32_t i = 0; i < count; i++) {
            uint32_t word = d.immediate[index32 ? i : i / 2];
            uint32_t index = index32 ? word : (i & 1 ? word >> 16 : word & 0xFFFF);
            if (restart && index == (index32 ? reset_index : reset_index & 0xFFFF))
                indices.push_back(UINT32_MAX);
            else
                indices.push_back(vertex(index));
        }
    } else if (d.indexed) {
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
        restart = rectangles = points = expanded = false;
        host_vertex = {};
    } else if (tessellated) {
        // the control points of the host's patches (tessellation.vert): the
        // index for the domain shader and the edge factor
        restart = false;
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

    // A tessellated draw's control points: two floats each, the index for the
    // domain shader and the edge factor (tessellation.vert). Control point
    // indexed: the triangles' (strips and fans as lists, each primitive
    // between reset indices on its own, as Xenia's TriangleStripToList and
    // TriangleFanToList order them) or the quads' vertices, a patch each.
    // Patch indexed: each index a patch's (discrete and continuous), or the
    // guest's edge factors, 3 or 4 a patch, with the patch's number plus the
    // offset as its index (adaptive, as Xenia's adaptive hull shaders).
    // Factors are the guest's plus 1 clamped to its range plus 1 (Xenia's
    // tessellation_adaptive.vs: NaN to the lowest, as its HLSL clamp), or that
    // range's top (the discrete and continuous hull shaders).
    std::vector<float> control_points;
    if (tessellated) {
        float max_factor = as_float(regs_[r::VGT_HOS_MAX_TESS_LEVEL]) + 1.0f;
        float min_factor = as_float(regs_[r::VGT_HOS_MIN_TESS_LEVEL]) + 1.0f;
        auto put = [&](float index, float factor) { control_points.insert(control_points.end(), {index, factor}); };
        if (factors) {
            for (size_t k = 0; k + patch_points <= indices.size(); k += patch_points) {
                float patch = float(remap(uint32_t(k / patch_points)));
                for (uint32_t j = 0; j < patch_points; j++)
                    put(patch,
                        std::fmin(std::fmax(std::bit_cast<float>(indices[k + j]) + 1.0f, min_factor), max_factor));
            }
        } else if (primitive == 5 || primitive == 6) {
            for (size_t first = 0; first < indices.size();) {
                size_t end = first;
                while (end < indices.size() && indices[end] != UINT32_MAX) end++;
                for (size_t k = first + 2; k < end; k++) {
                    uint32_t a = indices[k - 2], b = indices[k - 1], c = indices[k];
                    if (primitive == 5)
                        a = indices[k - 1], b = indices[k], c = indices[first];
                    else if ((k - first) & 1)
                        std::swap(a, b);
                    for (uint32_t v : {a, b, c}) put(float(v), max_factor);
                }
                first = end + 1;
            }
        } else {
            for (size_t k = 0; k + patch_points <= indices.size(); k += patch_points)
                for (uint32_t j = 0; j < patch_points; j++) put(float(indices[k + j]), max_factor);
        }
        if (control_points.empty()) return;
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
        uint32_t rows = std::min(scissor.offset.y + scissor.extent.height,
                                 draw_max_y(*vs, constants[0], indices, primitive, tessellated));
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
                sys.flags[1] |= 1u << 13; // the sprite's coordinates: PixelOptions::point_coordinates
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
    for (uint32_t plane = 0, k = 0; plane < 6; plane++)
        if (ucp & (1u << plane)) std::memcpy(sys.clip_planes[k++], &regs_[r::PA_CL_UCP_0_X + plane * 4], 16);
    if (ucp && options_.verbose)
        for (uint32_t k = 0; k < host_vertex.clip_planes; k++)
            std::fprintf(stderr, "  draw %llu: user clip plane %u (%g, %g, %g, %g)%s\n",
                         (unsigned long long)report_.draws, k, sys.clip_planes[k][0], sys.clip_planes[k][1],
                         sys.clip_planes[k][2], sys.clip_planes[k][3],
                         host_vertex.clip_planes_cull ? ", cull only" : "");
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
        // the constant's view of it: its swizzle, and its signed components'
        const texture::Info info = texture::parse_fetch_constant(fetch);
        const uint32_t index = t ? texture_view(*t, info.swizzle, false, reason) : 0;
        if (!fatal_.empty()) return;
        if (!index) {
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
        sys.slots[slot] = index | t->kind << 14 | sampler_bits;
        sys.signs[slot] = swizzled_signs(info);
        if (info.signs[0] == 1 || info.signs[1] == 1 || info.signs[2] == 1 || info.signs[3] == 1) {
            if (t->signed_read == texture::Signed::None) {
                approximate("signed texture components read as unsigned (no signed version of the format)");
            } else if (t->signed_format != VK_FORMAT_UNDEFINED) {
                const uint32_t signed_index = texture_view(*t, info.swizzle, true, reason);
                if (!fatal_.empty()) return;
                if (signed_index)
                    sys.signed_slots[slot] = signed_index | t->kind << 14 | sampler_bits;
                else
                    approximate("signed texture components read as unsigned: " + reason);
            }
        }
    }

    // Expanded in the vertex shader (shader::HostVertex): a strip of two
    // triangles a point or rectangle, cut by the restart index, as Xenia's
    // built-in index buffer of two-triangle strips. A point's four host
    // vertices are its index shifted left by 2 plus the corner; a
    // rectangle's are numbered from 4 times its number, each with the
    // rectangle's three guest indices as its attribute (guest_indices).
    std::vector<uint32_t> guest_indices;
    if (expanded) {
        std::vector<uint32_t> strips;
        uint32_t primitives = uint32_t(points ? indices.size() : indices.size() / 3);
        strips.reserve(size_t(primitives) * 5);
        for (uint32_t k = 0; k < primitives; k++) {
            if (k) strips.push_back(UINT32_MAX);
            for (uint32_t corner = 0; corner < 4; corner++)
                strips.push_back(points ? indices[k] << 2 | corner : k * 4 + corner);
            if (rectangles)
                for (uint32_t corner = 0; corner < 4; corner++)
                    guest_indices.insert(guest_indices.end(), indices.begin() + k * 3, indices.begin() + k * 3 + 3);
        }
        if (strips.empty()) return;
        indices = std::move(strips);
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        restart = true;
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
    uint32_t topology_class = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST   ? 0
                              : topology <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1
                              : topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? 3
                                                                             : 2;
    // the vertex shader's host form, with a tessellated draw's hull shader
    // and patch size
    uint32_t host_key = host_vertex.key() | hull << 16 | patch_points << 20;
    // The polygon modes (PA_SU_SC_MODE_CNTL's poly_mode bits 3-4, dual mode
    // 1; the front's and back's types in bits 5-7 and 8-10: 0 points, 1
    // lines, 2 triangles), as Xenia's Vulkan pipeline cache takes them:
    // Vulkan has one mode for both faces, so the lowest type of the faces
    // not culled (the most deliberate one: points, then lines, then fill).
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    if (primitive >= 4 && !rectangles) {
        const uint32_t sc_mode = regs_[r::PA_SU_SC_MODE_CNTL];
        if (((sc_mode >> 3) & 3) == 1) {
            uint32_t type = 2;
            if (!(sc_mode & 1)) type = std::min(type, (sc_mode >> 5) & 7);
            if (!(sc_mode & 2)) type = std::min(type, (sc_mode >> 8) & 7);
            if (type < 2) {
                if (d_.fill_mode_non_solid())
                    polygon_mode = type == 0 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_LINE;
                else
                    approximate("polygon modes other than fill (the device has no fillModeNonSolid)");
            }
        }
    }
    host_key |= uint32_t(polygon_mode) << 24;
    std::vector<uint32_t> key = {uint32_t(vertex_hash_),
                                 uint32_t(vertex_hash_ >> 32),
                                 ps ? uint32_t(pixel_hash_) : 0,
                                 ps ? uint32_t(pixel_hash_ >> 32) : 0,
                                 topology_class,
                                 rectangles,
                                 msaa,
                                 depth ? depth->image.format : 0u,
                                 regs_[r::PA_CL_CLIP_CNTL] & (1u << 16),
                                 host_key};
    std::vector<uint32_t> color_key; // the targets' part of the key
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
        // with extended dynamic state 3 the blend and the write mask are set
        // at the draw too, so the targets' formats alone make pipelines
        if (d_.dynamic_blend())
            color_key.insert(color_key.end(), {i, uint32_t(color_formats[i])});
        else
            color_key.insert(color_key.end(), {i, uint32_t(color_formats[i]), color_masks[i], blend});
    }
    key.insert(key.end(), color_key.begin(), color_key.end());
    VkShaderModule guest_vs = vertex_module(*vs, host_vertex, why);
    if (!guest_vs) return skip("vertex shader: " + why);
    // The pixel shader: the guest's, its float24 depth conversion form with
    // float24_convert; a depth-only draw that tests or writes depth then gets
    // one that converts the rasterized depth (Xenia's float24_truncate and
    // _round fragment shaders). The conversion runs per sample under MSAA, as
    // Xenia's (unless the shader writes oDepth, which is per pixel).
    VkShaderModule fragment = ps ? ps->module : VK_NULL_HANDLE;
    uint32_t float24_mode = float24_convert ? (options_.depth_float24_round ? 2u : 1u) : 0u;
    // The interpolators read at the centroid under MSAA, as xenos.h's
    // GetInterpolatorSamplingPattern takes SQ_CONTEXT_MISC's sc_sample_cntl (bits 2-3:
    // 0 centroids only, 1 centers only, 2 both, by SQ_INTERPOLATOR_CNTL's
    // sampling_pattern: a bit each, 1 the center). Xenia keeps only the
    // interpolators the pixel shader reads; here all 16, as the shader
    // loads them all.
    uint32_t centroid = 0;
    if (ps && msaa) {
        uint32_t sample_control = (regs_[r::SQ_CONTEXT_MISC] >> 2) & 3;
        if (sample_control == 0)
            centroid = 0xFFFF;
        else if (sample_control == 2)
            centroid = ~(regs_[r::SQ_INTERPOLATOR_CNTL] >> 16) & 0xFFFF;
    }
    // a point list's pixel parameters take the sprite's coordinates from the
    // point expansion (Xenia's param_gen_point)
    const bool point_coordinates =
        ps && points && ((regs_[reg::SQ_PROGRAM_CNTL] >> 18) & 1) && ((regs_[r::SQ_CONTEXT_MISC] >> 8) & 0xFF) < 16;
    bool sample_shading = false;
    if (fsi_ && !export_only) {
        // the interlock path: the guest's shader with the output merger taken
        // in, or for depth and stencil alone edram_fsi.frag's own
        if (ps) {
            shader::PixelOptions options;
            options.centroid = uint16_t(centroid);
            options.fsi = uint8_t(fsi_);
            options.point_coordinates = point_coordinates;
            options.edram_library = std::span<const uint32_t>(EDRAM_FSI_LIBRARY);
            fragment = pixel_module(*ps, options, why);
            if (!fragment) return skip("pixel shader: " + why);
        } else {
            fragment = fsi_constants.flags & FSI_DEPTH_STENCIL ? fsi_depth_fs_ : VK_NULL_HANDLE;
        }
        if (!fragment) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
    } else if (ps && (float24_convert || centroid || point_coordinates)) {
        shader::PixelOptions options;
        options.depth_float24 = uint8_t(float24_mode);
        options.centroid = uint16_t(centroid);
        options.point_coordinates = point_coordinates;
        fragment = pixel_module(*ps, options, why);
        if (!fragment) return skip("pixel shader: " + why);
    }
    // the interlock path's state as the pipeline's specialization
    // (Options::fsi_specialized; edram_fsi.frag's constant_id 0-9)
    uint32_t spec_data[10] = {};
    VkSpecializationMapEntry spec_entries[10];
    VkSpecializationInfo spec{};
    if (fsi_ && fragment) {
        key.push_back(0xF5000000u | fsi_ | (options_.fsi_specialized ? 0x100u : 0u));
        if (options_.fsi_specialized) {
            spec_data[0] = 1, spec_data[1] = fsi_constants.flags;
            for (uint32_t i = 0; i < 4; i++) spec_data[2 + i] = fsi_constants.rt[i], spec_data[6 + i] = fsi_constants.blend[i];
            for (uint32_t k = 0; k < 10; k++) spec_entries[k] = {k, k * 4, 4};
            spec = {10, spec_entries, sizeof(spec_data), spec_data};
            key.insert(key.end(), spec_data + 1, spec_data + 10);
        }
    }
    if (float24_convert) {
        if (!ps && ((depth_control & 4) || ((depth_control >> 4) & 7) != 7))
            fragment = depth_float24_fs_[float24_mode == 2 ? 1 : 0];
        // (the guest's shader runs per sample by its sample-decorated
        // position; the depth-only one by the pipeline's sample shading)
        sample_shading = msaa && fragment && !ps;
    }
    key.push_back(float24_mode | (sample_shading ? 4u : 0u) | (fragment && !ps ? 8u : 0u) |
                  (point_coordinates ? 16u : 0u) | centroid << 16);
    VkPipelineShaderStageCreateInfo stages[4] = {};
    uint32_t stage_count = 0;
    auto stage = [&](VkShaderStageFlagBits bit, VkShaderModule m) {
        stages[stage_count++] = {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, bit, m, "main", nullptr};
    };
    if (tessellated) {
        stage(VK_SHADER_STAGE_VERTEX_BIT, tessellation_vs_);
        stage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tessellation_hs_[hull]);
        stage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, guest_vs);
    } else {
        stage(VK_SHADER_STAGE_VERTEX_BIT, guest_vs);
        if ((rectangles || points) && !expanded)
            stage(VK_SHADER_STAGE_GEOMETRY_BIT, points ? point_gs_ : rectangle_gs_);
    }
    if (fragment) {
        stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment);
        if (spec.mapEntryCount) stages[stage_count - 1].pSpecializationInfo = &spec;
    }
    // the vertex attributes: a tessellated draw's control points (two
    // floats), an expanded rectangle's guest indices (three)
    VkVertexInputBindingDescription vertex_binding{0, tessellated ? 8u : 12u, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertex_attribute{
        0, 0, tessellated ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32_UINT, 0};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (tessellated || host_vertex.host == shader::HostVertex::RectangleListAsTriangleStrip) {
        vertex_input.vertexBindingDescriptionCount = vertex_input.vertexAttributeDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    assembly.primitiveRestartEnable = restart;
    VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    tessellation.patchControlPoints = patch_points;
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
    }
    raster.polygonMode = polygon_mode; // (above, with the key)
    raster.depthClampEnable = (regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1;
    raster.depthBiasEnable = depth ? VK_TRUE : VK_FALSE; // the polygon offset, set at the draw
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = msaa == 0   ? VK_SAMPLE_COUNT_1_BIT
                                       : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT
                                                   : VK_SAMPLE_COUNT_4_BIT;
    // (the interlock path's guest 2x as 4x where passes without attachments
    // take no 2x: edram_fsi.frag's F_2X_AS_4X)
    if (fsi_ && msaa == 1 && fsi_2x_as_4x_) multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    // The interlock path rasterizes at the Xenos's sample positions where the
    // device places samples (VK_EXT_sample_locations), as Xenia's interlock
    // paths always do (its xenos_sample_positions, for host render targets,
    // is off by default; so is ours): each host sample at the position of the
    // guest sample its coverage stands for (draw_util.cc's
    // GetHostSampleXenosPositions with interlock: native 2x host samples 1
    // and 0, 2x as 4x 0, 1, 0, 1, 4x 0, 3, 2, 1; positions in sixteenths
    // from the pixel's center, kXenosSamplePositions2x and 4x).
    VkSampleLocationEXT sample_positions[4];
    VkPipelineSampleLocationsStateCreateInfoEXT sample_locations{
        VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
    if (fsi_ && msaa && (d_.sample_locations() & multisample.rasterizationSamples)) {
        static const int8_t positions_2x[2][2] = {{-4, 4}, {4, -4}};
        static const int8_t positions_4x[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};
        static const uint32_t remap_4x[4] = {0, 3, 2, 1};
        uint32_t count = uint32_t(multisample.rasterizationSamples);
        for (uint32_t k = 0; k < count; k++) {
            const int8_t *p = msaa == 2 ? positions_4x[remap_4x[k]] : positions_2x[count == 2 ? k ^ 1 : k & 1];
            sample_positions[k] = {0.5f + float(p[0]) / 16.0f, 0.5f + float(p[1]) / 16.0f};
        }
        sample_locations.sampleLocationsEnable = VK_TRUE;
        sample_locations.sampleLocationsInfo = {VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
                                                nullptr,
                                                multisample.rasterizationSamples,
                                                {1, 1},
                                                count,
                                                sample_positions};
        multisample.pNext = &sample_locations;
    }
    if (sample_shading) multisample.sampleShadingEnable = VK_TRUE, multisample.minSampleShading = 1.0f;
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
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        // (core) the lines' width, PA_SU_LINE_CNTL's
        VK_DYNAMIC_STATE_LINE_WIDTH,
        // extended dynamic state 3 where the device has it (the last three)
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states)) - (d_.dynamic_blend() && !fsi_ ? 0u : 3u);
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = color_formats;
    // the interlock path's passes have no attachments
    if (fsi_) rendering.colorAttachmentCount = blend_state.attachmentCount = 0;
    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat =
        depth ? depth->image.format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    if (tessellated) info.pTessellationState = &tessellation;
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
        key = {uint32_t(vertex_hash_), uint32_t(vertex_hash_ >> 32), 0xE0u, topology_class, host_key};
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
    // made on a worker while the draw goes without it, or with a stand-in,
    // when there is a pixel shader (Xenia's D3D12 pipeline cache: draws
    // without one get theirs at once)
    bool pending = false, stand_in = false;
    VkPipeline pipe = pipeline(key, info, options_.async_pipelines && ps, pending, stand_in);
    if (pending) return skip("pipeline not ready (being made)");
    if (!pipe) return skip("vkCreateGraphicsPipelines failed");
    if (stand_in) report_.stand_in_draws++;

    // the vertex data: the ranges of the vertex fetch constants the shaders
    // use (address and size, as Xenia's IssueDraw requests them), merged,
    // read from the mirror in device memory as Xenia's shaders read its
    // shared memory, guest memory's bytes put there where the CPU wrote them
    // since (request) and where a resolve or an export has not written newer
    // ones. (Read from host memory, the arena's copies, they made the
    // scene's vertex shaders wait on the bus: 6.7 ms of a UE3 game's race
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
        report_.vertex_bytes += request(vertex_ranges, &vertex_memo_);
        // guest memory's bytes under the streams' pages, where the mirror has
        // nothing newer (Xenia's RequestRange of each stream)
        report_.vertex_bytes += request(exported);
    }
    if (!fatal_.empty()) return;

    // everything else the draw takes from the arena, in one submission: the
    // indices (none for a tessellated draw), the vertex attributes (its
    // control points, or an expanded rectangle's guest indices)
    if (tessellated) indices.clear();
    VkDeviceSize index_bytes = indices.size() * 4;
    const void *attributes = tessellated ? static_cast<const void *>(control_points.data()) : guest_indices.data();
    VkDeviceSize attribute_bytes = (tessellated ? control_points.size() : guest_indices.size()) * 4;
    VkDeviceSize constants_size = (sizeof(harness::Constants) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    VkDeviceSize system_size = (sizeof(System) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    bool own_indices = index_bytes > ARENA_SLOT / 4, own_attributes = attribute_bytes > ARENA_SLOT / 4;
    VkDeviceSize need = 2 * constants_size + system_size + 2 * sizeof(FsiConstants) + 6 * ubo_align_ +
                        (own_indices ? 0 : index_bytes + 4) + (own_attributes ? 0 : attribute_bytes + 4);
    if (arena_take(need, 1) == NONE) flush();
    else slots_[current_].used -= need; // only a check that the draw fits
    if (!fatal_.empty()) return;

    // the interlock path's constants, with the occlusion counter the draw
    // counts in (the slot's, so once the flush above is past)
    if (fsi_ && fragment) {
        fsi_constants.zpd_index = fsi_zpd_index();
        if (last_fsi_at_ == NONE || std::memcmp(&last_fsi_, &fsi_constants, sizeof(FsiConstants)) != 0) {
            last_fsi_at_ = arena_take(sizeof(FsiConstants), ubo_align_);
            std::memcpy(arena_data(last_fsi_at_), &fsi_constants, sizeof(FsiConstants));
            last_fsi_ = fsi_constants;
        }
    }
    uint32_t dynamic_offsets[4] = {0, 0, 0, last_fsi_at_ == NONE ? 0u : uint32_t(last_fsi_at_)};
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
    // words into the arena, or into a buffer of their own (freed with the
    // slot) when they are too many for it
    auto put_words = [&](const void *data, VkDeviceSize bytes, bool own_buffer, VkBufferUsageFlags usage,
                         VkBuffer &buffer, VkDeviceSize &at) {
        if (own_buffer) {
            vk::Buffer own = d_.create_buffer(bytes, usage, true);
            if (!own.buffer) {
                fatal_ = "could not make an index or vertex buffer";
                return false;
            }
            std::memcpy(own.mapped, data, bytes);
            slots_[current_].after.push_back([this, own]() mutable { d_.destroy(own); });
            buffer = own.buffer, at = 0;
        } else {
            at = arena_take(bytes, 4);
            std::memcpy(arena_data(at), data, bytes);
            buffer = arena_.buffer;
        }
        return true;
    };
    VkBuffer index_buffer = VK_NULL_HANDLE, attribute_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_at = 0, attribute_at = 0;
    if (index_bytes &&
        !put_words(indices.data(), index_bytes, own_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_at))
        return;
    if (attribute_bytes && !put_words(attributes, attribute_bytes, own_attributes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      attribute_buffer, attribute_at))
        return;

    // record, in the render pass of these targets (none for exports alone);
    // a draw that exports begins its own, after a barrier, as what came
    // before in the pass may read the mirror where it writes
    if (exports) end_pass();
    Pass pass;
    for (uint32_t i = 0; i < 4; i++) pass.colors[i] = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
    pass.depth = depth ? depth->view : VK_NULL_HANDLE;
    pass.width = export_only ? 1 : width, pass.height = export_only ? 1 : height;
    if (fsi_ && !export_only)
        pass.width = pass.height = 8192, pass.samples = uint32_t(multisample.rasterizationSamples);
    VkCommandBuffer c = begin_pass(pass, colors, depth);
    if (pipe != bound_pipeline_) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipeline_ = pipe;
    }
    VkDescriptorSet sets[2] = {draw_sets_[1], bindless_};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 4, dynamic_offsets);
    if (index_buffer) vkCmdBindIndexBuffer(c, index_buffer, index_at, VK_INDEX_TYPE_UINT32);
    if (attribute_buffer) vkCmdBindVertexBuffers(c, 0, 1, &attribute_buffer, &attribute_at);
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
    if (d_.dynamic_blend() && !fsi_) {
        // extended dynamic state 3: the blend and write masks of the four
        // attachments the pipelines have (those without a target unused)
        VkBool32 enables[4];
        VkColorBlendEquationEXT equations[4];
        VkColorComponentFlags masks[4];
        for (uint32_t i = 0; i < 4; i++) {
            const VkPipelineColorBlendAttachmentState &b = blends[i];
            enables[i] = b.blendEnable;
            equations[i] = {b.srcColorBlendFactor, b.dstColorBlendFactor, b.colorBlendOp,
                            b.srcAlphaBlendFactor, b.dstAlphaBlendFactor, b.alphaBlendOp};
            masks[i] = b.colorWriteMask;
        }
        set_blend_enable_(c, 0, 4, enables);
        set_blend_equation_(c, 0, 4, equations);
        set_write_mask_(c, 0, 4, masks);
    }
    {
        // Lines as wide as PA_SU_LINE_CNTL's WIDTH says (half the width in
        // 12.4 fixed point, AMD's R6xx guide and freedreno's a2xx.xml; 8, one
        // pixel, is Xenia's reset value), within what the device draws; one
        // pixel without wide lines, as Xenia draws all of them (a TODO there)
        float width = 1.0f;
        if (topology_class == 1) {
            float guest = float(regs_[r::PA_SU_LINE_CNTL] & 0xFFFF) * (1.0f / 8.0f);
            width = d_.wide_lines() ? std::clamp(guest, d_.min_line_width(), d_.max_line_width()) : 1.0f;
            if (width != guest)
                approximate(d_.wide_lines() ? "line widths outside the device's range (clamped)"
                                            : "wide lines drawn one pixel wide (no wide lines on the device)");
        }
        vkCmdSetLineWidth(c, width);
    }
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
        std::snprintf(kind, sizeof(kind), "draw: %s, shaders %016llX %016llX", draw_name.c_str(),
                      (unsigned long long)vertex_hash_, ps ? (unsigned long long)pixel_hash_ : 0ull);
        gpu_mark(kind);
    }
    zpd_draw(c);
    if (tessellated)
        vkCmdDraw(c, uint32_t(control_points.size() / 2), 1, 0, 0);
    else
        vkCmdDrawIndexed(c, uint32_t(indices.size()), 1, 0, 0, 0);
    if (options_.gpu_timing_draws) gpu_mark("draws: between draws");
    report_.drawn++;
    end_export(exported);
    if (options_.verbose) {
        std::fprintf(stderr, "  draw %llu: %s, %u indices, %s%s%s, shaders %016llX %016llX, blend %08X, mask %04X, vertices",
                     (unsigned long long)report_.draws, draw_name.c_str(), count,
                     ps ? "color and depth" : "depth only", depth ? ", depth on" : "", exports ? ", memory export" : "",
                     (unsigned long long)vertex_hash_, ps ? (unsigned long long)pixel_hash_ : 0ull,
                     regs_[r::RB_BLENDCONTROL[0]], regs_[reg::RB_COLOR_MASK] & 0xFFFF);
        for (auto [start, end] : vertex_ranges) std::fprintf(stderr, " %08X-%08X", start, end);
        if (msaa)
            std::fprintf(stderr, ", %ux MSAA (sample control %u, pattern %04X)", 1u << msaa,
                         (regs_[r::SQ_CONTEXT_MISC] >> 2) & 3, regs_[r::SQ_INTERPOLATOR_CNTL] >> 16);
        if (centroid) std::fprintf(stderr, ", centroid %04X", centroid);
        if (color_control & 8) std::fprintf(stderr, ", alpha test %u", color_control & 7);
        if (color_control & 16) std::fprintf(stderr, ", alpha to coverage %02X", color_control >> 24);
        std::fprintf(stderr, "\n");
    }
}

// ---- resolves ----

void Impl::resolve() {
    report_.resolves++;
    if (native_suppress_) {
        native::PassInfo pass;
        pass.resolve = true;
        pass.copy_dest = regs_[reg::RB_COPY_DEST_BASE];
        if (!native_keep(pass)) return;
    }
    poll();
    resolve::Info info;
    std::string why;
    // the rectangle's three vertices, in vertex fetch constant 0
    traced(regs_[reg::FETCH_CONSTANTS] & 0x1FFFFFFC, 24);
    if (!resolve::get_info({regs_, REGISTER_COUNT}, memory_, info, why)) return skip("resolve: " + why);
    if (!info.copy && !why.empty()) approximate("resolve not copied: " + why);
    uint32_t pitch = regs_[reg::RB_SURFACE_INFO] & 0x3FFF;
    // (the interlock path reads EDRAM itself: the target's place there)
    RenderTarget view;
    RenderTarget *source = nullptr;
    if (fsi_) {
        view = edram_view(info.depth, info.edram_base, info.surface_pitch ? info.surface_pitch : pitch, info.msaa,
                          info.source_format);
        source = &view;
    } else {
        source = render_target(info.depth, info.edram_base, info.surface_pitch ? info.surface_pitch : pitch, info.msaa,
                               info.source_format);
    }
    if (!source) return fatal_.empty() ? skip("resolve: source format") : void();
    uint32_t x = std::min(info.x, source->width), y = std::min(info.y, source->height);
    uint32_t w = std::min(info.width, source->width - x), h = std::min(info.height, source->height - y);
    if (!fsi_) acquire(source, y + h);
    if (!fatal_.empty()) return;
    if (options_.verbose)
        std::fprintf(stderr,
                     "  resolve: %s base %u (%u,%u %ux%u) -> %08X format %u from %u (msaa %u, samples %u)%s%s\n",
                     info.depth ? "depth" : "color", info.edram_base, x, y, w, h, info.dest_base, info.dest_format,
                     info.source_format, info.msaa, info.sample_select, info.clear_color ? ", clear color" : "",
                     info.clear_depth ? ", clear depth" : "");

    if (info.copy && w && h) {
        resolve::Info clipped = info;
        clipped.x = x, clipped.y = y, clipped.width = w, clipped.height = h;
        gpu_resolve(clipped, *source);
        if (!fatal_.empty()) return;
    }

    // the clears, after the copy, over the same rectangle; on the interlock
    // path written into EDRAM (the targets' words: RB_DEPTH_CLEAR, or
    // RB_COLOR_CLEAR with RB_COLOR_CLEAR_LO first at 64 bits per sample)
    if (fsi_ && (info.clear_color || info.clear_depth) && w && h) {
        uint32_t msaa = (regs_[reg::RB_SURFACE_INFO] >> 16) & 3;
        if (info.clear_depth) {
            uint32_t depth_info = regs_[reg::RB_DEPTH_INFO], value = regs_[r::RB_DEPTH_CLEAR];
            RenderTarget t = edram_view(true, depth_info & 0xFFF, source->pitch, msaa, (depth_info >> 16) & 1);
            uint32_t cw = std::min(w, t.width - std::min(x, t.width)), ch = std::min(h, t.height - std::min(y, t.height));
            if (cw && ch) fsi_clear(t, x, y, cw, ch, value, value);
        }
        if (info.clear_color) {
            uint32_t color_info = regs_[r::COLOR_INFO[info.color_target & 3]];
            RenderTarget t = edram_view(false, color_info & 0xFFF, source->pitch, msaa, (color_info >> 16) & 0xF);
            uint32_t cw = std::min(w, t.width - std::min(x, t.width)), ch = std::min(h, t.height - std::min(y, t.height));
            uint32_t value = regs_[r::RB_COLOR_CLEAR];
            if (cw && ch) fsi_clear(t, x, y, cw, ch, t.wide ? regs_[r::RB_COLOR_CLEAR_LO] : value, value);
        }
        return;
    }
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
                    // a gamma target kept linear holds the clear linear, as Xenia's
                    if (t->gamma_linear)
                        for (int j = 0; j < 3; j++) cv.float32[j] = pwl_gamma_to_linear(cv.float32[j]);
                    break;
                case H::RGB10A2:
                    for (int j = 0; j < 3; j++) cv.float32[j] = float((value >> (10 * j)) & 0x3FF) / 1023.0f;
                    cv.float32[3] = float((value >> 30) & 3) / 3.0f;
                    break;
                case H::RG16F: case H::RGBA16F:
                    // through the UINT view, the bits as they are, where the
                    // target is moved by its bits
                    for (int j = 0; j < 4; j++) {
                        if (t->raw)
                            cv.uint32[j] = uint32_t(value >> (16 * j)) & 0xFFFF;
                        else
                            cv.float32[j] = half_to_float(uint16_t(value >> (16 * j)));
                    }
                    break;
                case H::R32F: case H::RG32F:
                    // (the same bits as floats or through the UINT view)
                    cv.uint32[0] = uint32_t(value >> (t->host == H::RG32F ? 32 : 0));
                    cv.uint32[1] = uint32_t(value);
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
            attachment.imageView = t->raw ? t->raw : t->view;
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

// Beyond Xenia, whose resolves write memory and whose textures over it are
// loaded from there again: the textures a later draw reads over a resolve's
// destination are written by the resolve too, their images kept current, not
// loaded again (renderer.md, "Resolve targets"). Such a texture is one made
// as a resolve target (one tiled 2D level of 32- or 64-bit texels whose
// image holds the guest's bits as they are), current before the resolve
// (not stale), with the destination's base, pitch, texel size and
// endianness, whatever its format (the same bits under another name, as a
// 16_16_FLOAT render target read as 16_16_EXPAND): then the rectangle's
// pixel (x, y) is its texel (texel_x0 + dest_x + x, texel_y0 + dest_y + y),
// and the resolve leaves the rest of its memory as it was. Up to two (the
// push constants' room); a third is loaded again as before.
uint32_t Impl::resolve_targets(const resolve::Info &info, Texture *targets[2], uint32_t &extra_rows) {
    targets[0] = targets[1] = nullptr;
    extra_rows = 0;
    if (!resolve_targets_ || info.depth || info.dest_array || !info.texture_base) return 0;
    const uint32_t bytes = texture::format_block(info.dest_format).bytes;
    if (bytes != 4 && bytes != 8) return 0;
    if ((info.texel_x0 >> 5) > 0xFF || (info.texel_y0 >> 5) > 0xFF) return 0;
    const uint32_t base = info.texture_base & 0x1FFFFFFF, wide = bytes == 8 ? 1 : 0;
    const uint32_t bucket = base >> TEXTURE_BUCKET_SHIFT;
    if (bucket >= std::size(texture_buckets_)) return 0;
    // A stale one (written since its load) is fed too when the resolve
    // writes every texel it has: its image is then what memory holds again.
    const uint32_t x0 = info.texel_x0 + info.dest_x, y0 = info.texel_y0 + info.dest_y;
    auto covers = [&](const Texture &t) {
        const uint32_t width = (t.key[2] & 0x1FFF) + 1, height = ((t.key[2] >> 13) & 0x1FFF) + 1;
        return x0 == 0 && y0 == 0 && info.width >= width && info.height >= height;
    };
    // A texture whose memory holds the destination further down, a whole
    // number of 32-row macro tile rows in (a smaller surface placed inside a
    // larger one), is fed at that many rows more: a tiled 2D texture's row of
    // macro tiles is its pitch times 32 texels, contiguous (Xenia's
    // GetTiledOffset2D), so the destination's texel (x, y) is the texture's
    // (x, y + rows).
    uint32_t count = 0;
    for (Texture *t : texture_buckets_[bucket]) {
        if (!t->target || ((t->target >> 7) & 1) != wide || t->target_pitch != info.dest_pitch ||
            t->target_endian != info.dest_endian || base < t->target_base)
            continue;
        const uint32_t offset = base - t->target_base, macro_row = t->target_pitch * bytes * 32;
        if (!macro_row || offset % macro_row) continue;
        const uint32_t rows = offset / macro_row * 32;
        if ((info.texel_y0 + rows) >> 5 > 0xFF || (count && rows != extra_rows)) continue;
        if (t->stale && (rows || !covers(*t))) continue;
        extra_rows = rows;
        targets[count++] = t;
        if (count == 2) break;
    }
    return count;
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
    // (and an 8_8_8_8_GAMMA target kept linear, its gamma made in EDRAM's
    // bits, which Xenia's resolves always read)
    // (the interlock path: always, EDRAM being the targets)
    const bool from_edram = fsi_ || info.msaa != 0 || (!info.depth && source.gamma_linear);
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
        if (!fsi_ && !edram_store(&source, tiles)) return;
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
    // as EDRAM's bits would be (Xenia resolves from its EDRAM dump, which
    // follows depth_float24_round; a depth converted in the pixel shader is
    // exact either way)
    p.depth_round = options_.depth_float24_round && !options_.depth_float24_convert_in_pixel_shader;
    p.raw = resolve::raw_copy(info);
    p.dest_base = info.dest_base, p.dest_pitch = info.dest_pitch, p.dest_height = info.dest_height;
    p.dest_x = info.dest_x, p.dest_y = info.dest_y, p.dest_slice = info.dest_slice, p.dest_array = info.dest_array;
    p.dest_format = info.dest_format;
    p.dest_log2_bytes = uint32_t(std::countr_zero(texture::format_block(info.dest_format).bytes));
    p.dest_endian = info.dest_endian, p.dest_number = info.dest_number, p.dest_swap = info.dest_swap;
    p.exp_factor = std::ldexp(1.0f, info.dest_exp_bias);
    Texture *targets[2];
    uint32_t extra_rows = 0;
    if (uint32_t count = resolve_targets(info, targets, extra_rows)) {
        p.target = (targets[0]->target & 0xFF) | (count > 1 ? (targets[1]->target & 0x7F) << 8 : 0) |
                   (info.texel_x0 >> 5) << 16 | ((info.texel_y0 + extra_rows) >> 5) << 24;
        for (uint32_t k = 0; k < count; k++) {
            use(*targets[k], submission_serial() + 1);
            // current again if it was stale: the resolve wrote all of it
            if (targets[k]->stale) targets[k]->stale = false, targets[k]->load_why = LoadWhy::CpuWritten;
        }
        report_.resolve_targets_written += count;
    }
    if (from_edram) {
        p.edram = 1;
        p.edram_base = source.base, p.edram_pitch = source.pitch_tiles, p.msaa = source.msaa;
        p.ints_log2 = source.wide ? 1 : 0;
        p.src_x = info.x, p.src_y = info.y;
        p.first_sample = resolve::first_sample(info.sample_select);
        p.samples = resolve::sample_count(info.sample_select);
    }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
    const VkDescriptorSet resolve_sets[2] = {from_edram ? resolve_edram_set_ : resolve_set_, target_set_};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 2, resolve_sets, 0, nullptr);
    vkCmdPushConstants(c, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(c);

    gpu_written(written);
    textures_written_by_gpu(written, targets);
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

// ---- the port's frames (native_render.h; docs/renderer.md, "A port's own
// renderer") ----

// What the port's output renderer records through, while it runs: the
// renderer's command buffer, its texture cache and its arena.
class Impl::NativeFrame final : public native::Frame {
public:
    explicit NativeFrame(Impl &r) : r_(r) {}
    VkCommandBuffer commands() override {
        // the port's bindings are its own: the draws bind theirs again
        r_.bound_pipeline_ = VK_NULL_HANDLE;
        return r_.outside();
    }
    bool texture(const uint32_t fetch[6], native::TextureImage &out, std::string &why) override {
        const texture::Info info = texture::parse_fetch_constant(fetch);
        if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture || info.dimension != 1 || !info.base_address)
            return why = "not a 2D texture's fetch constant", false;
        Texture *t = r_.texture(fetch, why);
        if (!t || !r_.fatal_.empty()) return false;
        const uint32_t index = r_.texture_view(*t, info.swizzle, false, why);
        if (!index && !why.empty()) return false;
        const uint32_t key = texture::compose_swizzle(info.swizzle, t->host_swizzle);
        for (const TextureView &v : t->views)
            if (v.key == key) {
                out.image = t->image.image, out.view = v.view, out.format = t->image.format;
                out.width = t->image.extent.width, out.height = t->image.extent.height;
                out.levels = t->image.levels, out.layers = t->image.layers;
                return true;
            }
        return why = "no view of the texture", false;
    }
    bool upload(const void *data, size_t bytes, size_t alignment, VkBuffer &buffer, VkDeviceSize &offset) override {
        const VkDeviceSize align = std::max<VkDeviceSize>({VkDeviceSize(alignment), r_.ubo_align_, 16});
        if (bytes == 0 || bytes + align > ARENA_SLOT || !r_.fatal_.empty()) return false;
        const VkDeviceSize at = r_.arena(bytes, align);
        if (at == NONE) return false;
        std::memcpy(r_.arena_data(at), data, bytes);
        buffer = r_.arena_.buffer, offset = at;
        return true;
    }

private:
    Impl &r_;
};

// The output image at this size (made again when it changes; the old one
// goes once the GPU is done with it), in the general layout.
bool Impl::native_output(uint32_t width, uint32_t height) {
    if (native_output_.image && native_output_.extent.width == width && native_output_.extent.height == height)
        return true;
    if (native_output_.image) {
        vk::Image old = native_output_;
        VkImageView old_view = native_output_view_;
        cmd();
        slots_[current_].after.push_back([this, old, old_view]() mutable {
            vkDestroyImageView(dev_, old_view, nullptr);
            d_.destroy(old);
        });
        native_output_ = {}, native_output_view_ = VK_NULL_HANDLE;
    }
    native_output_ = d_.create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {width, height, 1}, 1, 1,
                                     VK_SAMPLE_COUNT_1_BIT,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!native_output_.image) return false;
    native_output_view_ = make_view(native_output_.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_ASPECT_COLOR_BIT, {}, 1, 1, false);
    if (!native_output_view_) {
        d_.destroy(native_output_);
        native_output_ = {};
        return false;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = native_output_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1, dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(outside(), &dep);
    return true;
}

// At a swap, before its front buffer is taken: the port's output renderer
// called when there is one and the toggle is on. What it returns decides the
// next frame's passes: skipped after a frame the port drew. A frame whose
// passes were skipped and that the port did not draw has no picture of its
// own; held, and the window keeps the port's last.
bool Impl::native_frame(uint64_t number, bool &held) {
    const uint64_t version = native::detail::version();
    if (version != native_version_) native::detail::settings(native_settings_), native_version_ = version;
    const bool on = native_settings_.renderer && native::enabled();
    const bool was_suppressed = native_suppress_;
    if (!on && !was_suppressed) return false;
    native::detail::AtomicCounters &counts = native::detail::atomic_counters();
    bool drew = false;
    if (on) {
        native::OutputContext ctx;
        ctx.swap = number;
        ctx.device = &d_, ctx.instance = d_.instance(), ctx.physical = d_.physical(), ctx.vk_device = dev_;
        ctx.queue_family = d_.queue_family();
        ctx.registers = regs_, ctx.physical_memory = memory_.data();
        ctx.suppressed = was_suppressed;
        const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS];
        const texture::Info info = texture::parse_fetch_constant(fetch);
        if (FetchConstantType(fetch[0] & 3) == FetchConstantType::Texture && info.dimension == 1 && info.base_address) {
            native::GuestFrontBuffer &g = ctx.guest;
            g.address = info.base_address, g.width = info.width, g.height = info.height, g.pitch = info.pitch;
            g.format = info.format, g.tiled = info.tiled, g.endian = info.endian;
        }
        ctx.guest.fetch = fetch;
        uint32_t width = native_settings_.width, height = native_settings_.height;
        if (!width || !height) {
            width = ctx.guest.width ? ctx.guest.width : 1280;
            height = ctx.guest.width ? ctx.guest.height : 720;
        }
        if (native_output(width, height) && fatal_.empty()) {
            ctx.output = native_output_.image, ctx.output_view = native_output_view_;
            ctx.width = width, ctx.height = height;
            NativeFrame frame(*this);
            ctx.frame = &frame;
            gpu_mark("the port's frame (native rendering)");
            drew = native_settings_.renderer(ctx);
            // whatever the port bound is not the draws'
            bound_pipeline_ = VK_NULL_HANDLE;
        } else {
            approximate("the port's output image could not be made");
        }
    }
    if (drew) {
        native::detail::bump(counts.frames_native);
    } else {
        if (on) native::detail::bump(counts.frames_yielded);
        held = was_suppressed;
        if (held) native::detail::bump(counts.frames_held);
    }
    native_suppress_ = drew;
    native::detail::set_active(drew);
    return drew;
}

// A draw or resolve while the port's frame is shown: the filter's answer
// (none: skip everything), counted.
bool Impl::native_keep(native::PassInfo &pass) {
    native::detail::AtomicCounters &counts = native::detail::atomic_counters();
    bool skip = true;
    if (native_settings_.filter) {
        pass.surface_pitch = regs_[reg::RB_SURFACE_INFO] & 0x3FFF;
        pass.color_base = regs_[reg::RB_COLOR_INFO] & 0xFFF;
        pass.color_format = (regs_[reg::RB_COLOR_INFO] >> 16) & 0xF;
        pass.registers = regs_, pass.swap = report_.swaps;
        skip = native_settings_.filter(pass);
    }
    native::detail::bump(skip ? (pass.resolve ? counts.resolves_skipped : counts.draws_skipped)
                              : (pass.resolve ? counts.resolves_kept : counts.draws_kept));
    return !skip;
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
    {
        // hitches: the frame's time making pipelines on this thread, and the
        // time since the last swap
        double spent = report_.pipeline_seconds - pipeline_seconds_at_swap_;
        pipeline_seconds_at_swap_ = report_.pipeline_seconds;
        if (spent > 0.004) report_.pipeline_hitches++;
        report_.pipeline_frame_max = std::max(report_.pipeline_frame_max, spent);
        auto now = std::chrono::steady_clock::now();
        if (number) {
            double gap = std::chrono::duration<double>(now - last_swap_time_).count();
            if (gap > 0.05) report_.swap_gaps_50ms++;
            if (gap > 0.1) report_.swap_gaps_100ms++;
            report_.swap_gap_max = std::max(report_.swap_gap_max, gap);
        }
        last_swap_time_ = now;
    }
    {
        // the display's look-up table mode (display_gamma), counted
        char what[96];
        std::snprintf(what, sizeof(what), "swaps with DC_LUTA_CONTROL 0x%06X", regs_[r::DC_LUTA_CONTROL]);
        use(what);
    }
    const bool read = options_.frame_wanted && options_.frame_wanted(number);
    // the port's frame (native_render.h), shown in the emulated one's place;
    // held: neither is shown (the port's last stays in the window)
    bool held = false;
    const bool native = native_frame(number, held);
    texture::Info info;
    Texture *t = read || (options_.on_swap && !native && !held) ? front_texture(info) : nullptr;
    if (read && t) read_front_buffer(number, *t, info);
    FrontBuffer front;
    if (native && options_.on_swap && fatal_.empty()) {
        // shown as it is: an identity table for the display's gamma
        front.swap = number;
        front.image = native_output_.image, front.format = native_output_.format;
        front.width = native_output_.extent.width, front.height = native_output_.extent.height;
        front.swizzle = texture::SWIZZLE_RGBA, front.components = 4, front.bits = 8, front.luta_control = 0;
        for (uint32_t i = 0; i < 256; i++) {
            const uint32_t ten = i << 2 | i >> 6;
            front.gamma[i] = ten << 20 | ten << 10 | ten;
        }
    } else if (t && options_.on_swap && !held) {
        front.swap = number;
        front.image = t->image.image, front.format = t->image.format;
        front.width = t->image.extent.width, front.height = t->image.extent.height;
        // the fetch constant's swizzle through the host format's, over the
        // image's four components
        front.swizzle = texture::compose_swizzle(info.swizzle, t->host_swizzle);
        front.components = 4;
        front.bits = info.format == 7 || info.format == 54 ? 10 : 8; // as deliver
        front.luta_control = regs_[r::DC_LUTA_CONTROL];
        front.gamma = processor_.gamma_ramp_table();
        front.pwl = processor_.gamma_ramp_pwl();
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
    read.luta_control = regs_[r::DC_LUTA_CONTROL];
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
    // The display's gamma (display_gamma: DC_LUTA_CONTROL's 256-entry table
    // or piecewise linear ramp per component, Xenia's apply_gamma_table and
    // apply_gamma_pwl where they agree), on the front buffer's 8 bits a
    // component, or 10 for 2_10_10_10; the 10 bits it gives back to 8
    // (presenter.h's Packed10bpcRGBTo8bpcBytes).
    const bool ten_bits = read.format == 7 || read.format == 54;
    if (read.luta_control & 0x303030) approximate("DC_LUTA_CONTROL's signed or floating-point data (read as unsigned)");
    if (read.luta_control & 0xF0F00) approximate("DC_LUTA_CONTROL's green and red increments (the blue one's applies)");
    if (ten_bits && !(read.luta_control & 0xF))
        approximate("the display's 256-entry gamma table on a 10-bit front buffer (its top 8 bits)");
    if (ten_bits && read.host != texture::HostFormat::RGB10A2) {
        approximate("the display's gamma ramp (front buffer not 10 bits a component)");
    } else {
        for (size_t i = 0; i < pixels; i++) {
            uint32_t values[4] = {};
            if (ten_bits) {
                uint32_t word;
                std::memcpy(&word, read.data + 4 * i, 4);
                const uint32_t texel[4] = {word & 0x3FF, (word >> 10) & 0x3FF, (word >> 20) & 0x3FF,
                                           (word >> 30) * 341};
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t guest = (read.swizzle >> (3 * c)) & 7;
                    values[c] = guest == 5 ? 1023 : guest >= 4 ? 0 : texel[guest];
                }
            } else {
                for (uint32_t c = 0; c < 3; c++) values[c] = frame.rgba[i * 4 + c];
            }
            for (uint32_t c = 0; c < 3; c++) {
                uint32_t ten = display_gamma(values[c], ten_bits ? 10 : 8, c, read.luta_control, read.gamma, read.pwl);
                frame.rgba[i * 4 + c] =
                    ten_bits ? uint8_t(float(ten) * (255.0f / 1023.0f) + 0.5f) : uint8_t((ten * 255 + 511) / 1023);
            }
        }
    }
    report_.frames++;
    if (options_.on_frame) options_.on_frame(read.number, frame);
}

} // namespace

uint32_t display_gamma(uint32_t value, uint32_t bits, uint32_t channel, uint32_t luta_control,
                       const std::array<uint32_t, 256> &table, const std::array<uint32_t, 128 * 3> &pwl) {
    uint32_t increment = luta_control & 0xF;
    if (!increment) return (table[(value >> (bits - 8)) & 0xFF] >> (channel == 0 ? 20 : channel == 1 ? 10 : 0)) & 0x3FF;
    increment = std::min(increment, 9u);
    uint32_t entry = pwl[((value >> increment) & 0x7F) * 3 + channel];
    // base and delta in 1/64 of the 10 bits (their low 6 bits are zero)
    float out =
        (float(entry & 0xFFFF) + float((value & ((1u << increment) - 1)) * (entry >> 16)) / float(1u << increment)) *
        (1.0f / (64.0f * 1023.0f));
    return uint32_t(std::clamp(out, 0.0f, 1.0f) * 1023.0f + 0.5f);
}

// automatic: the interlock path where the device has fragment shader
// interlock (docs/renderer.md, "Exact EDRAM through fragment shader
// interlock": it matches Xenia's exact pictures and measured faster than
// host render targets on every trace and live)
Options::RenderTargetPath render_target_path(const vk::Device &device, const Options &options) {
    Options::RenderTargetPath path = options.render_target_path;
    if (const char *e = std::getenv("XF_RENDER_TARGET_PATH"); e && *e) {
        if (std::strcmp(e, "fsi") == 0) path = Options::RenderTargetPath::fsi;
        else if (std::strcmp(e, "host") == 0) path = Options::RenderTargetPath::host;
    }
    if (path != Options::RenderTargetPath::host && !device.fragment_shader_interlock()) {
        if (path == Options::RenderTargetPath::fsi)
            std::fprintf(stderr, "renderer: no fragment shader interlock on the device: host render targets\n");
        path = Options::RenderTargetPath::host;
    }
    return path == Options::RenderTargetPath::host ? path : Options::RenderTargetPath::fsi;
}

std::unique_ptr<Renderer> Renderer::create(vk::Device &device, gpu::CommandProcessor &processor,
                                           const Options &options, std::string &error) {
    auto renderer = std::make_unique<Impl>(device, processor, options);
    if (!renderer->init(error)) return nullptr;
    return renderer;
}

bool precompile(vk::Device &device, const Options &options, const PrecompileOptions &precompile,
                PrecompileReport &report, std::string &error) {
    // a renderer of its own (its layout and its own modules, as a run's),
    // over a processor that runs nothing
    class Idle final : public gpu::CommandProcessor::Host {
    public:
        void interrupt(uint32_t) override {}
        bool wait(uint32_t) override { return false; }
    } host;
    std::unique_ptr<Memory> memory = Memory::create(error);
    if (!memory) return false;
    gpu::CommandProcessor processor(*memory, host);
    Options o = options;
    o.async_pipelines = true;
    o.pipeline_stand_in = Options::PipelineStandIn::library;
    o.linked_pipelines_only = false;
    o.on_frame = {}, o.on_swap = {}, o.frame_wanted = {};
    // (the storage is the build step's to read: the renderer's own is not opened)
    o.shader_storage.clear();
    auto renderer = std::make_unique<Impl>(device, processor, o);
    if (!renderer->init(error)) return false;
    renderer->set_storage_path(options.shader_storage);
    return renderer->precompile(precompile, report, error);
}

} // namespace xf::render
