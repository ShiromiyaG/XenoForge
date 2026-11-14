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

#include "draw_sections.h"
#include "fast_clock.h"
#include "thread_cpu.h"
#include "native_internal.h"

#include "xenoforge/guest_memory.h"
#include "xenoforge/memory.h"
#include "xenoforge/native_shaders.h"
#include "xenoforge/render_scale.h"
#include "xenoforge/resolve.h"
#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/index_convert.h"
#include "xenoforge/shader_program.h"
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
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// (last: its macros stand for the driver's vkCmd* functions in what follows)
#include "gpu/deferred_sink.h"
#include "record_ring.h"

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
// edram_transfer.frag: [single-sampled source, multisampled][color, UINT color, depth, depth and stencil]
const uint32_t EDRAM_TRANSFER[] = {
#include "edram_transfer.frag.inc"
};
const uint32_t EDRAM_TRANSFER_UINT[] = {
#include "edram_transfer_uint.frag.inc"
};
const uint32_t EDRAM_TRANSFER_DEPTH[] = {
#include "edram_transfer_depth.frag.inc"
};
const uint32_t EDRAM_TRANSFER_STENCIL[] = {
#include "edram_transfer_stencil.frag.inc"
};
const uint32_t EDRAM_TRANSFER_STENCIL_COPY[] = {
#include "edram_transfer_stencil_copy.frag.inc"
};
const uint32_t EDRAM_TRANSFER_MS_STENCIL_COPY[] = {
#include "edram_transfer_ms_stencil_copy.frag.inc"
};
const uint32_t EDRAM_TRANSFER_MS[] = {
#include "edram_transfer_ms.frag.inc"
};
const uint32_t EDRAM_TRANSFER_MS_UINT[] = {
#include "edram_transfer_ms_uint.frag.inc"
};
const uint32_t EDRAM_TRANSFER_MS_DEPTH[] = {
#include "edram_transfer_ms_depth.frag.inc"
};
const uint32_t EDRAM_TRANSFER_MS_STENCIL[] = {
#include "edram_transfer_ms_stencil.frag.inc"
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
// (the host render target path's, reading the source target's image)
const uint32_t RESOLVE_IMAGE[] = {
#include "resolve_image.comp.inc"
};
const uint32_t RESOLVE_IMAGE_MS[] = {
#include "resolve_image_ms.comp.inc"
};
const uint32_t TEXTURE_LOAD[] = {
#include "texture_load.comp.inc"
};
// the interlock path's (edram_fsi.frag): the output merger the translated
// pixel shaders take in, the depth-only fragment shaders, and EDRAM's clears
const uint32_t EDRAM_FSI_LIBRARY[] = {
#include "edram_fsi_library.frag.inc"
};
// the generic shaders (generic.glsl): the vertex shader, and the pixel
// shader on host render targets and on the interlock path (pixel and sample
// interlock), each with point coordinates or not and writing depth or not
const uint32_t GENERIC_VS[] = {
#include "generic.vert.inc"
};
const uint32_t GENERIC_CLIP_VS[] = {
#include "generic_clip.vert.inc"
};
const uint32_t GENERIC_CULL_VS[] = {
#include "generic_cull.vert.inc"
};
const uint32_t GENERIC_EXTENTS_VS[] = {
#include "generic_extents.vert.inc"
};
const uint32_t GENERIC_EXTENTS_CLIP_VS[] = {
#include "generic_extents_clip.vert.inc"
};
const uint32_t GENERIC_EXTENTS_CULL_VS[] = {
#include "generic_extents_cull.vert.inc"
};
const uint32_t GENERIC_FS[] = {
#include "generic.frag.inc"
};
const uint32_t GENERIC_DEPTH_FS[] = {
#include "generic_depth.frag.inc"
};
const uint32_t GENERIC_POINTS_FS[] = {
#include "generic_points.frag.inc"
};
const uint32_t GENERIC_POINTS_DEPTH_FS[] = {
#include "generic_points_depth.frag.inc"
};
const uint32_t GENERIC_FSI_FS[] = {
#include "generic_fsi.frag.inc"
};
const uint32_t GENERIC_FSI_DEPTH_FS[] = {
#include "generic_fsi_depth.frag.inc"
};
const uint32_t GENERIC_FSI_POINTS_FS[] = {
#include "generic_fsi_points.frag.inc"
};
const uint32_t GENERIC_FSI_POINTS_DEPTH_FS[] = {
#include "generic_fsi_points_depth.frag.inc"
};
const uint32_t GENERIC_FSI_SAMPLE_FS[] = {
#include "generic_fsi_sample.frag.inc"
};
const uint32_t GENERIC_FSI_SAMPLE_DEPTH_FS[] = {
#include "generic_fsi_sample_depth.frag.inc"
};
const uint32_t GENERIC_FSI_SAMPLE_POINTS_FS[] = {
#include "generic_fsi_sample_points.frag.inc"
};
const uint32_t GENERIC_FSI_SAMPLE_POINTS_DEPTH_FS[] = {
#include "generic_fsi_sample_points_depth.frag.inc"
};
// the program buffer's size: a program is 16 bytes per control flow
// instruction and 64 per ALU or fetch one, a few KB a shader
constexpr VkDeviceSize PROGRAM_BUFFER_BYTES = 32ull << 20;
const uint32_t EDRAM_FSI_DEPTH_PIXEL[] = {
#include "edram_fsi_depth_pixel.frag.inc"
};
const uint32_t EDRAM_FSI_DEPTH_SAMPLE[] = {
#include "edram_fsi_depth_sample.frag.inc"
};
const uint32_t EDRAM_FSI_DEPTH_UNORDERED[] = {
#include "edram_fsi_depth_unordered.frag.inc"
};
const uint32_t EDRAM_CLEAR[] = {
#include "edram_clear.comp.inc"
};
const uint32_t EDRAM_RESCALE[] = {
#include "edram_rescale.comp.inc"
};
const uint32_t SCALED_FILL[] = {
#include "scaled_fill.comp.inc"
};

// registers xenos.h does not name
namespace r {
constexpr uint32_t RB_HIZCONTROL = 0x2203;
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
// A slot's share of the draw-data arena (the draws' constants in video
// memory, Impl::draw_data_take): about 1,500 draws that change both stages'
// constants, after which the submission is flushed as for the arena
constexpr VkDeviceSize DRAW_DATA_SLOT = 16u << 20;
constexpr VkDeviceSize READBACK_CHUNK = 32u << 20;
// GPU timestamps a submission holds at most (Options::gpu_timing)
constexpr uint32_t TIMESTAMPS = 8192;
// generic shaders' draws timed a submission at most (a pair of timestamps each; Impl::generic_allowed)
constexpr uint32_t GENERIC_TIMED = 512;
constexpr uint32_t GPU_END = UINT32_MAX; // the kind of a submission's last timestamp
constexpr VkDeviceSize NONE = ~VkDeviceSize(0);
// the bindless tables: images by kind (2D arrays, 3D, cube; the slot word
// holds 14 bits of index) and samplers
constexpr uint32_t IMAGES[3] = {16384, 4096, 4096};
constexpr uint32_t SAMPLERS = 1024;
// the textures resolves write as well, of 32- and of 64-bit texels each
// (Impl::resolve_target)
// a texel size's places in the resolves' table (resolve.comp's targets32 and
// targets64): 127, all that a place's 7 bits in ResolveConstants::target name
constexpr uint32_t RESOLVE_TARGETS = 127;

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

// Each host sample at the position of the guest sample it stands for
// (draw_util.cc's GetHostSampleXenosPositions; positions in sixteenths from
// the pixel's center, draw_util.h's kXenosSamplePositions2x and 4x): 4x in
// the guest's order on host render targets and 0, 3, 2, 1 on the interlock
// path, native 2x host samples 1 and 0, the interlock path's 2x as 4x
// 0, 1, 0, 1. msaa: 1 for the guest's 2x, 2 for its 4x.
void xenos_sample_locations(uint32_t msaa, VkSampleCountFlagBits samples, bool interlock, VkSampleLocationEXT *positions,
                            VkPipelineSampleLocationsStateCreateInfoEXT &state) {
    static const int8_t positions_2x[2][2] = {{-4, 4}, {4, -4}};
    static const int8_t positions_4x[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};
    static const uint32_t remap_4x_interlock[4] = {0, 3, 2, 1};
    uint32_t count = uint32_t(samples);
    for (uint32_t k = 0; k < count; k++) {
        const int8_t *p = msaa == 2 ? positions_4x[interlock ? remap_4x_interlock[k] : k]
                                    : positions_2x[count == 2 ? k ^ 1 : k & 1];
        positions[k] = {0.5f + float(p[0]) / 16.0f, 0.5f + float(p[1]) / 16.0f};
    }
    state.sampleLocationsEnable = VK_TRUE;
    state.sampleLocationsInfo = {VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, nullptr, samples, {1, 1}, count, positions};
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

// RB_BLENDCONTROL for the host's blending. The console applies the factors
// before a minimum or maximum (Xenia's interlock paths do: "unlike Vulkan/D3D12
// fixed-function blend which ignores factors for MIN/MAX, the Xbox 360 applies
// blend factors before min/max", dxbc_shader_translator_om.cc); the host takes
// the minimum or maximum of the source and the destination as they are. Both
// factors one need nothing. With factors of zero and one the console's result
// is a plain copy, where values are never negative (the target's format):
// max(0, d) is d, max(s, 0) is s, a minimum with zero is zero; those become
// additions the host does. A source factor that depends on the source and the
// blend constant alone, against a destination factor of zero or one, is
// multiplied in by the pixel shader (shader::PixelOptions::blend_premultiply,
// with `shader` set: the draw has a pixel shader on host render targets), which
// also takes the minimum or maximum with zero for a destination factor of
// zero; `premultiply` gets the target's word of the system constants then
// (shader_translator.cpp), and the host blends the product with factors of
// one. Xenia's Direct3D 12 pipelines do that for the source's alpha against a
// destination factor of one alone. What is left as the host blends, `ignored`
// set for the count: a factor that needs the destination (either side), a
// destination factor other than zero and one, a 16-bit fixed-point target
// (kept scaled on the host), and an alpha multiplied here while the color's
// blend reads the source's alpha.
uint32_t host_blend_control(uint32_t blend, uint32_t format, bool shader, uint32_t &premultiply, bool &ignored) {
    const bool non_negative = format <= 3 || format == 10 || format == 12; // unorm and 7e3 targets
    const bool scaled = format == 4 || format == 5;                        // 16_16 and 16_16_16_16
    // (edram_fsi.frag's fixed_color and fixed_alpha)
    const bool fixed_color = format <= 2 || format == 10, fixed_alpha = fixed_color || format == 3 || format == 12;
    const uint32_t guest = blend;
    premultiply = 0;
    for (uint32_t shift : {0u, 16u}) { // color, alpha
        const uint32_t half = (guest >> shift) & 0x1FFF;
        const uint32_t source = half & 0x1F, op = (half >> 5) & 7, destination = (half >> 8) & 0x1F;
        if (op != 2 && op != 3) continue;              // not MIN, MAX
        if (source == 1 && destination == 1) continue; // the host's own
        uint32_t replaced = 0;
        if (source <= 1 && destination <= 1 && non_negative) {
            // (ADD, with the factors that leave the console's result: ZERO, ADD, ZERO for a minimum)
            if (op == 3) replaced = source | destination << 8;
        } else {
            // ZERO, ONE, the source's color and alpha, the constant's, and one minus those
            const bool source_only = source <= 1 || (source >= 4 && source <= 7) || (source >= 12 && source <= 15);
            if (!shader || scaled || !source_only || destination > 1) {
                ignored = true;
                continue;
            }
            uint32_t word = source | 0x80;
            if (destination == 1) {
                replaced = 1 | op << 5 | 1 << 8; // ONE, the minimum or maximum, ONE
            } else {
                word |= (op == 2 ? 1u : 2u) << 5; // with zero, in the shader
                replaced = 1;                     // ONE, ADD, ZERO
            }
            premultiply |= word << (shift / 2);
        }
        blend = (blend & ~(0x1FFFu << shift)) | replaced << shift;
    }
    if (premultiply & 0x8000) {
        // the alpha the shader writes is no longer the source's: a color
        // blend that reads it (SRC_ALPHA, ONE_MINUS_SRC_ALPHA, SRC_ALPHA_SATURATE) keeps the guest's alpha blend
        const uint32_t color_source = blend & 0x1F, color_destination = (blend >> 8) & 0x1F;
        auto reads_alpha = [](uint32_t factor) { return factor == 6 || factor == 7 || factor == 16; };
        if (reads_alpha(color_source) || reads_alpha(color_destination)) {
            blend = (blend & 0x0000FFFF) | (guest & 0xFFFF0000);
            premultiply &= 0xFF;
            ignored = true;
        }
    }
    if (premultiply) premultiply |= (fixed_color ? 1u << 16 : 0u) | (fixed_alpha ? 1u << 17 : 0u);
    return blend;
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
    uint32_t writes_position_xz = 0; // bit 0: x, bit 2: z (Impl::depth_overwrite)
    // memexport: whether it writes any eM#, and the float constants its eA
    // writes take the stream from (Shader::memexport_stream_constants)
    bool exports = false;
    std::set<uint32_t> stream_constants;
    // per texture slot, the first fetch's filter overrides: mag, min, mip, aniso (3: the constant's)
    std::map<uint32_t, std::array<uint32_t, 4>> filters;
    // what a draw asks of the summary's sets, made once (load_shader): the texture slots and the
    // vertex fetch slots as masks, and the blocks of 32 fetch constant words they lie in
    uint64_t texture_mask = 0, vertex_mask[2] = {};
    uint32_t fetch_blocks = 0;
    // the float constants its instructions read, a bit per block of 8 (c0-c7, c8-c15, ...): all
    // of them when one is read relative to a0 or aL. A draw uploads those blocks alone.
    uint32_t float_blocks = 0xFFFFFFFFu;
    uint32_t filter_mask = 0; // the slots (below 32) filters has
    std::array<uint32_t, 4> slot_filters[32] = {}; // filters' for those slots, without the map's search
    // its program for the generic shaders (Impl::program): encoded once, at
    // `program` in the program buffer (16-byte units; UINT32_MAX: not there,
    // with why in program_unsupported), its flags (shader::PROGRAM_*)
    bool encoded = false;
    uint32_t program = UINT32_MAX, program_flags = 0;
    std::string program_unsupported;
    bool analyzed = false;            // Impl::analyze
    std::set<uint32_t> translating; // forms on the translation workers (Impl::form_module)
};

// A rectangle of a render target's EDRAM tiles: columns [c0, c1) of its tile
// rows [r0, r1), as the target lays its tiles out (pitch_tiles a row).
struct TileRect {
    uint32_t c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    bool empty() const { return c1 <= c0 || r1 <= r0; }
    bool within(const TileRect &o) const { return c0 >= o.c0 && r0 >= o.r0 && c1 <= o.c1 && r1 <= o.r1; }
    uint32_t area() const { return empty() ? 0 : (c1 - c0) * (r1 - r0); }
    // a memo of tiles known to be some way, after `more` are found to be so
    // too: both when they make one rectangle, else the larger
    void remember(const TileRect &more) {
        if (more.within(*this)) return;
        if (empty() || within(more)) {
            *this = more;
        } else if (more.c0 == c0 && more.c1 == c1 && more.r0 <= r1 && more.r1 >= r0) {
            r0 = std::min(r0, more.r0), r1 = std::max(r1, more.r1);
        } else if (more.r0 == r0 && more.r1 == r1 && more.c0 <= c1 && more.c1 >= c0) {
            c0 = std::min(c0, more.c0), c1 = std::max(c1, more.c1);
        } else if (more.area() > area()) {
            *this = more;
        }
    }
};

struct RenderTarget {
    bool depth = false;
    uint32_t base = 0, pitch = 0, msaa = 0, format = 0;
    texture::HostFormat host = texture::HostFormat::Unsupported; // color
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sampled = VK_NULL_HANDLE, stencil = VK_NULL_HANDLE; // color or depth alone, stencil alone
    VkDescriptorSet store_set = VK_NULL_HANDLE;                     // its EDRAM store's
    // a depth image drawn into since a shader last read it (Impl::settle_depth_samples)
    bool samples_unsettled = false;
    // resolves' clears kept for its next render pass (Impl::flush_clears)
    std::vector<std::pair<VkClearAttachment, VkClearRect>> pending_clears;
    uint32_t width = 0, height = 0; // in the console's pixels
    // What a draw's bookkeeping found last (Impl::acquire, tiles_drawn), so
    // the next draw into the same target does not walk its tiles again: it
    // held the tiles of `owned` as of owners_epoch_ == owned_epoch, and none
    // of the tiles of `drawn` held a clear's value as of cleared_epoch_ ==
    // drawn_epoch.
    uint64_t owned_epoch = 0, drawn_epoch = 0;
    TileRect owned, drawn;
    // (and none of the tiles of stencil_written had a known stencil as of
    // stencil_epoch_ == stencil_epoch: Impl::tile_stencil_)
    uint64_t stencil_epoch = 0;
    TileRect stencil_written;
    // its image's size: width and height at the resolution scale (Impl::scale_)
    uint32_t host_width = 0, host_height = 0;
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
    // a depth target's stencil as an R8_UINT image of its size, which EDRAM
    // transfers into it write and copy into its stencil aspect where the
    // device has no stencil export (Impl::edram_transfer); made at the first
    vk::Image stencil_copy;
    VkImageView stencil_copy_view = VK_NULL_HANDLE;
};

// edram.glsl's push constants (width, height, x and y in the scaled grid's pixels)
struct EdramTarget {
    uint32_t base_tiles, pitch_tiles, msaa, format, width, height, is_depth, ints_log2, stencil_bit, flags;
    uint32_t x, y, clear_lo, clear_hi; // edram_clear.comp's
    uint32_t scale;                    // the resolution scale in quarters
};
// edram_transfer.frag's push constants: the destination's and the source's
// EdramTarget, up to the flags (edram.glsl's EdramParams)
struct EdramTransfer {
    uint32_t dest[11], source[11]; // (the 11th: the resolution scale)
};
static_assert(sizeof(EdramTransfer) <= 128);

// edram_fsi.frag's uniform block (set 0 binding 4): the interlock path's
// render target state
// a draw's viewport on the host (host_viewport)
struct Viewport {
    VkViewport viewport{};
    float ndc_scale[3] = {1, 1, 1}, ndc_offset[3] = {};
    bool empty = false;
};

struct FsiConstants {
    uint32_t flags, depth_base, pitch, zpd_index;
    uint32_t rt[4], blend[4], rt_base[4], stencil[4];
    float poly_offset[4], blend_constant[4], exp_bias[4];
    // the resolution scale in quarters and a tile's dwords at it (render_scale.h)
    uint32_t scale[4];
    bool operator==(const FsiConstants &) const = default;
};
static_assert(sizeof(FsiConstants) == 144);
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
    // bit 0: D24FS8 from the host's float, 20e4 rounded to the nearest even;
    // the resolution scales (resolve.comp's p.flags)
    uint32_t flags;
    uint32_t target; // the texture written too (resolve.comp's p.target), 0 none
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
    uint64_t loads = 0;         // how many times it was loaded (what a draw kept of it is as of one: SlotMemo)
    bool gpu = false;           // loaded from what the GPU wrote (the mirror's, or read back from it)
    // why its next load comes (Report's texture_loads_*)
    LoadWhy load_why = LoadWhy::New;
    bool exported = false; // of GpuWritten: a memory export's write, not a resolve's
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
    // what the resolves feeding it write: 0 the guest's bits, 1 and 2 a
    // depth texture's float from them (its load's DepthUnormToFloat and
    // DepthFloatToFloat; resolve.comp's write_targets)
    uint32_t target_convert = 0;
    // its image at the resolution scale (Impl::scale_texture), the resolves
    // feeding it writing its scaled texels; a load makes it the console's
    // size again
    bool scaled = false;
    // a stale target's 32x32 tiles the resolves feeding it wrote since it
    // went stale, a bit each by rows of tiles, and how many (Impl::cover):
    // once all are, its image is what memory holds again. Empty while none.
    std::vector<uint64_t> covered;
    uint32_t covered_count = 0;
    // XF_RESOLVE_TARGET_DETAIL: the resolve that last made it stale (its
    // destination's format, size, pitch, base and corner), for the reload's count
    struct StaleBy {
        uint32_t format = 0, width = 0, height = 0, pitch = 0, base = 0, x = 0, y = 0;
    } stale_by;
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
    // per slot, its texture's signs on the result components (two bits
    // each: unsigned, signed, biased, gamma; texture::swizzled_signs)
    uint32_t signs[32];
    uint32_t signed_slots[32]; // per slot, its texture's signed view as slots has it; 0 none
    float clip_planes[6][4];   // the user clip planes enabled, packed (shader::VertexOptions)
    // screen extents (shader::VertexOptions::extents): the host's NDC to
    // console pixels (x scale, x offset, y scale, y offset), and the
    // report's first word in the extents buffer
    float extent_map[4];
    uint32_t extent_report[4];
    // per slot, how its result goes through the constant's number format
    // (texture::integer_scale_bits, Xenia's GetIntegerScaleBits)
    uint32_t integer_scale[32];
    // per color target, the factors of a minimum or maximum blend the pixel
    // shader multiplies its color by (host_blend_control,
    // shader::PixelOptions::blend_premultiply), and the blend constant for them
    uint32_t blend_premultiply[4];
    float blend_constant[4];
    // the generic shaders' programs (generic.glsl): the vertex shader's and
    // the pixel shader's, in 16-byte units into the program buffer (binding 8)
    uint32_t generic[4];
};
static_assert(sizeof(System) == 784);
// a port's view of the block (native_shaders.h) is this one
static_assert(sizeof(System) == sizeof(native::SystemConstants));
static_assert(offsetof(System, slots) == offsetof(native::SystemConstants, slots));
static_assert(offsetof(System, flags) == offsetof(native::SystemConstants, flags));
static_assert(offsetof(System, values) == offsetof(native::SystemConstants, values));
static_assert(offsetof(System, signs) == offsetof(native::SystemConstants, signs));
static_assert(offsetof(System, signed_slots) == offsetof(native::SystemConstants, signed_slots));
static_assert(offsetof(System, clip_planes) == offsetof(native::SystemConstants, clip_planes));
static_assert(offsetof(System, extent_map) == offsetof(native::SystemConstants, extent_map));
static_assert(offsetof(System, integer_scale) == offsetof(native::SystemConstants, integer_scale));
static_assert(offsetof(System, blend_premultiply) == offsetof(native::SystemConstants, blend_premultiply));
static_assert(offsetof(System, generic) == offsetof(native::SystemConstants, generic));

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
    // uploads into the mirror run ahead of cmd, in the same submission
    // (Impl::request: pages nothing in the submission has used yet)
    VkCommandBuffer pre = VK_NULL_HANDLE;
    bool pre_recording = false;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t serial = 0; // the submission it holds, recording or in flight; 0 when free
    bool recording = false;
    VkDeviceSize used = 0;
    VkDeviceSize draw_used = 0; // of its share of the draw-data arena
    std::vector<std::function<void()>> after;
    std::vector<vk::Buffer> readbacks;
    size_t readback = 0;
    VkDeviceSize readback_used = 0;
    // GPU timing (Options::gpu_timing): its timestamps, and the kind of work
    // each one begins
    VkQueryPool queries = VK_NULL_HANDLE;
    std::vector<uint32_t> marks;
    // the generic shaders' draws timed (Impl::generic_allowed): a pair of timestamps each, and
    // each one's pixel shader; the pool reset as the slot begins recording
    VkQueryPool generic_timed = VK_NULL_HANDLE;
    std::vector<uint64_t> generic_shaders;
    bool generic_reset = false; // its queries to reset before the next use
    // occlusion queries (Impl's "occlusion queries"): the pool, reset as the
    // slot begins recording, and the report each query counts for
    VkQueryPool occlusion = VK_NULL_HANDLE;
    uint32_t occlusion_used = 0; // the queries to reset before the next use
    std::vector<uint64_t> occlusion_reports;
    // screen extents (Impl's "screen extents"): per report bounded in the
    // slot's share of the extents buffer, its handle and the pixels added
    // around its bounds (wide lines)
    struct ExtentReport {
        uint64_t handle = 0;
        uint32_t margin = 0;
    };
    std::vector<ExtentReport> extent_reports;
    uint32_t extents_used = 0; // the reports to zero before the next use
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
// The dynamic state a draw sets (Impl::draw_state_: what the recording
// command buffer holds since the last draw), compared bytewise
struct DrawState {
    VkViewport viewport;
    VkRect2D scissor;
    float blend_constants[4];
    uint32_t stencil_reference[2], stencil_compare_mask[2], stencil_write_mask[2]; // front, back
    VkStencilOp stencil_op[2][4];                                                    // fail, pass, depth fail, compare
    VkPrimitiveTopology topology;
    VkBool32 restart;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_compare;
    VkBool32 stencil_test, depth_bias_enable;
    VkBool32 blend_enables[4];
    VkColorBlendEquationEXT blend_equations[4];
    VkColorComponentFlags write_masks[4];
    float line_width;
    float depth_bias[3]; // the constant factor, the slope factor, the representation (VK_EXT_depth_bias_control)
};

struct Pass {
    VkImageView colors[4] = {};
    VkImageView depth = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    // the interlock path's passes have no attachments: their draws' samples
    // apart (all pipelines in such a pass rasterize at one count)
    uint32_t samples = 0;
    // an unordered depth pass of the interlock path (1 minimum, 2 maximum;
    // Impl::fsi_depth_run_), a pass apart from the interlock's draws
    uint32_t unordered = 0;
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
    // the generic stand-in (docs/renderer.md, "The generic shaders"): linked
    // from the vertex input and fragment output libraries and the generic
    // shaders' pre-rasterization and fragment shader libraries, drawing
    // until the pipeline or its exact linked stand-in is made
    VkPipeline generic = VK_NULL_HANDLE;
    PipelineEntry *generic_libraries[4] = {};
    bool generic_failed = false;
};

// Shaders translated off the renderer's thread (docs/renderer.md, "The
// translation off the renderer's thread"): a form's translation to SPIR-V,
// which reads only the shader's parse (fixed once loaded; shaders are never
// dropped), done by a few threads; the renderer's thread takes what they
// finished (take) and makes the modules. Stopping waits for the one under
// way and leaves the queue.
class TranslationWorkers {
public:
    struct Job {
        Shader *shader;
        uint32_t key; // Impl::form_module's
        bool pixel;
        shader::VertexOptions vertex;
        shader::PixelOptions pixel_options;
    };
    struct Done {
        Shader *shader;
        uint32_t key;
        shader::Translation translation;
        double seconds;
    };
    explicit TranslationWorkers(uint32_t threads) {
        for (uint32_t i = 0; i < threads; i++) threads_.emplace_back([this] { run(); });
    }
    ~TranslationWorkers() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread &t : threads_) t.join();
    }
    void queue(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }
    // what is finished, into done (a check of an atomic when nothing is)
    void take(std::vector<Done> &done) {
        if (!finished_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(done_);
        finished_.store(0, std::memory_order_release);
    }

private:
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            const auto start = std::chrono::steady_clock::now();
            Done d{job.shader, job.key,
                   shader::translate(job.shader->parsed, job.pixel ? shader::Stage::Pixel : shader::Stage::Vertex,
                                     job.vertex, job.pixel_options),
                   0.0};
            d.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard<std::mutex> lock(mutex_);
            done_.push_back(std::move(d));
            finished_.store(1, std::memory_order_release);
        }
    }
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    std::atomic<int> finished_{0};
    bool stop_ = false;
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

// The renderer's own lock, held through each of the processor's calls: a word taken by one
// compare-and-swap and given back by one exchange where no other thread wants it (nearly
// always: the others only read the report now and then), waiting on the word otherwise (0 free,
// 1 held, 2 held and wanted). std::mutex's lock and unlock (with the thread's id taken and an
// SRW lock behind them on Windows) were 2% of the command thread at several calls a draw.
// XF_RENDERER_STD_MUTEX=1: std::mutex, as before.
class CallLock {
public:
    // Who holds it, as the back end's waits for it are counted (XF_SLOW_FRAMES's back end line):
    // the back end itself, a call the front end passed (DeferredSink::passing on its thread), or
    // another thread (the player's report, a drain). Only the threads other than the back end
    // write it, so the back end's own taking stays one compare-and-swap.
    enum Holder : uint32_t { BACK_END, FRONT_END, OTHER, HOLDERS };
    void lock() {
        if (std_) {
            if (!gpu::DeferredSink::on_back_end()) {
                std_mutex_.lock();
                return mark_holder();
            }
            if (std_mutex_.try_lock()) return;
            const uint64_t t0 = FastClock::now();
            const uint32_t by = holder_.load(std::memory_order_relaxed);
            std_mutex_.lock();
            waited_[by].fetch_add(FastClock::now() - t0, std::memory_order_relaxed);
            return;
        }
        uint32_t free = 0;
        if (word_.compare_exchange_strong(free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            // (the holder named where it is not the back end, whose fast path this is)
            if (!gpu::DeferredSink::on_back_end()) mark_holder();
            return;
        }
        // (the back end's waits timed, by who held it as it began to wait)
        const bool back_end = gpu::DeferredSink::on_back_end();
        const uint64_t t0 = back_end ? FastClock::now() : 0;
        const uint32_t by = holder_.load(std::memory_order_relaxed);
        while (word_.exchange(2, std::memory_order_acquire) != 0) word_.wait(2, std::memory_order_relaxed);
        if (back_end) {
            waited_[by].fetch_add(FastClock::now() - t0, std::memory_order_relaxed);
        } else {
            mark_holder();
        }
    }
    bool try_lock() {
        if (std_) {
            if (!std_mutex_.try_lock()) return false;
            if (!gpu::DeferredSink::on_back_end()) mark_holder();
            return true;
        }
        uint32_t free = 0;
        if (!word_.compare_exchange_strong(free, 1, std::memory_order_acquire, std::memory_order_relaxed)) return false;
        if (!gpu::DeferredSink::on_back_end()) mark_holder();
        return true;
    }
    // the back end's time waiting for it so far, in FastClock's ticks, by holder
    uint64_t waited(Holder by) const { return waited_[by].load(std::memory_order_relaxed); }
    void unlock() {
        if (std_) return std_mutex_.unlock();
        if (word_.exchange(0, std::memory_order_release) == 2) word_.notify_one();
    }

private:
    void mark_holder() {
        holder_.store(gpu::DeferredSink::passing() ? FRONT_END : OTHER, std::memory_order_relaxed);
    }
    std::atomic<uint32_t> word_{0};
    std::atomic<uint32_t> holder_{OTHER};
    std::atomic<uint64_t> waited_[HOLDERS]{};
    std::mutex std_mutex_;
    const bool std_ = [] {
        const char *v = std::getenv("XF_RENDERER_STD_MUTEX");
        return v && v[0] == '1';
    }();
};

class Impl final : public Renderer {
public:
    Impl(vk::Device &device, gpu::CommandProcessor &processor, const Options &options)
        : d_(device), dev_(device.device()), processor_(processor), options_(options),
          memory_(processor.memory().translate_physical(0)), mem_(processor.memory()),
          stamps_(mem_.write_stamps()), block_stamps_(mem_.write_block_stamps()), page_shift_(mem_.watch_page_shift()),
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
    // (background: on its own thread, while playing; else here, a write under way waited for)
    void save_pipeline_cache(bool background = false);
    uint64_t write_pipeline_cache() const;
    std::thread cache_saver_;
    std::atomic<bool> cache_saver_done_{false};
    std::atomic<uint64_t> cache_saver_bytes_{0};

    // ---- the Sink: the processor's calls, on its thread ----
    void shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) override {
        if (deferring()) return deferred_->shader(type, address, code);
        Call call(*this, nullptr);
        if (!call) return;
        load_shader(type, code);
        // the IM_LOAD shader_again just let through: kept by where it came from
        if (shader_at_open_ && shader_at_key_ == shader_at(type, address, uint32_t(code.size())))
            shaders_at_[shader_at_key_] = {type == 0 ? vertex_hash_ : pixel_hash_, shader_at_count_,
                                           type == 0 ? vertex_shader_ : pixel_shader_};
        shader_at_open_ = false;
    }
    // IM_LOAD's shaders by where they were loaded from (beyond Xenia, which
    // copies and hashes the microcode at every load: a game loading its
    // shaders at each draw spent 15% of the command thread there): the hash
    // a load from the same words found, again while their pages are
    // unwritten (memory.h's watches: watched, the count taken, then the
    // copy). Never while a trace is recorded (it wants each load's words).
    bool shader_again(uint32_t type, uint32_t address, uint32_t dwords) override {
        if (deferring()) return deferred_->shader_again(type, address, dwords);
        Call call(*this, nullptr, true);
        shader_at_open_ = false;
        if (!call || trace_ || type > 1 || !dwords || address + uint64_t(dwords) * 4 > GuestMemory::SIZE) return false;
        const uint64_t key = shader_at(type, address, dwords);
        const uint32_t count = mem_.write_count();
        auto it = shaders_at_.find(key);
        if (it != shaders_at_.end()) {
            bool same = it->second.checked == count;
            if (!same) {
                same = pages_unwritten(address >> page_shift_, (address + dwords * 4 - 1) >> page_shift_,
                                       it->second.checked);
            }
            if (same && it->second.shader) {
                it->second.checked = count;
                (type == 0 ? vertex_hash_ : pixel_hash_) = it->second.hash;
                (type == 0 ? have_vertex_ : have_pixel_) = true;
                (type == 0 ? vertex_shader_ : pixel_shader_) = it->second.shader;
                report_.shader_loads_again++;
                return true;
            }
        }
        // the processor copies it next
        mem_.watch(address, dwords * 4);
        shader_at_key_ = key, shader_at_count_ = mem_.write_count(), shader_at_open_ = true;
        return false;
    }
    void draw(const gpu::RegisterFile &registers, const gpu::Draw &d) override {
        if (deferring()) return deferred_->draw(registers, d);
        Call call(*this, &registers);
        extent_drawn_ = false;
        uint32_t survey = viz_survey_begin();
        if (call) draw(d);
        // a draw in a screen extent report that did not bound it
        if (extent_open_ && !extent_drawn_) extent_whole_ = true;
        // a survey draw not measured: its query stays visible
        if (survey != UINT32_MAX && !viz_measured_) viz_[survey].fallback = true;
        viz_survey_ = viz_predicate_ = UINT32_MAX;
    }
    void viz_query(uint32_t id, bool end) override {
        if (deferring()) return deferred_->viz_query(id, end);
        Call call(*this, nullptr);
        if (call) viz_query_event(id & 63, end);
    }
    void resolve(const gpu::RegisterFile &registers, const gpu::Draw &d) override {
        if (deferring()) return deferred_->resolve(registers, d);
        Call call(*this, &registers);
        Timer timer(report_.resolve_seconds);
        if (call) resolve();
    }
    void swap(const gpu::RegisterFile &registers, const gpu::Swap &s) override {
        if (deferring()) return deferred_->swap(registers, s);
        Call call(*this, &registers);
        if (call) swap();
    }
    // the back end's (XF_GPU_BACK_END=1): the processor caught up with where the guest may look
    void sync(gpu::SyncFor why, uint32_t address, uint32_t bytes) override {
        if (deferring()) deferred_->sync(why, address, bytes);
    }
    bool write_later(const LateWrite &write) override { return deferring() && deferred_->write_later(write); }
    // occlusion queries (gpu::OcclusionQueries::Fast; "occlusion queries" below)
    bool counts_samples() override { return true; }
    void zpd_report(uint64_t ended, uint64_t next) override {
        if (deferring()) return deferred_->zpd_report(ended, next);
        Call call(*this, nullptr);
        if (call) zpd_report_ended(ended, next);
    }
    void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) override {
        std::lock_guard<CallLock> lock(mutex_);
        resolved.insert(resolved.end(), zpd_resolved_.begin(), zpd_resolved_.end());
        zpd_resolved_.clear();
    }
    // screen extents (gpu::ScreenExtents::Measured; "screen extents" below)
    bool measures_extents() override { return extents_.buffer != VK_NULL_HANDLE; }
    Work work() override {
        if (deferring()) return deferred_->work(); // (with the threads' figures, then back here)
        std::lock_guard<CallLock> lock(mutex_);
        Work w;
        w.gpu_wait_seconds = report_.wait_seconds;
        w.texture_loads = report_.textures, w.submissions = report_.submissions, w.pipelines = report_.pipelines;
        w.texture_loads_new = report_.texture_loads_new, w.texture_loads_cpu = report_.texture_loads_cpu_written;
        w.texture_loads_resolved = report_.texture_loads_gpu_written - report_.texture_loads_exported;
        w.texture_loads_exported = report_.texture_loads_exported;
        w.extent_wait_seconds = extent_wait_seconds_, w.extent_fence_seconds = extent_fence_seconds_;
        w.back_end_gpu_wait_seconds = back_end_gpu_wait_seconds_;
        w.gpu_wait_cpu_seconds = double(gpu_wait_cpu_ticks_.load(std::memory_order_relaxed)) / cpu_ticks_a_second();
        if (ring_) w.record_wait_seconds = FastClock::seconds(ring_->wait_ticks(rec::Ring::Fence));
        w.lock_front_seconds = FastClock::seconds(mutex_.waited(CallLock::FRONT_END));
        w.lock_other_seconds = FastClock::seconds(mutex_.waited(CallLock::OTHER));
        w.pipeline_seconds = report_.pipeline_seconds, w.translate_seconds = report_.translate_seconds;
        w.texture_seconds = report_.texture_load_seconds;
        w.drain_seconds = drain_seconds_, w.drains = drains_, w.cache_save_seconds = cache_save_seconds_;
        w.stand_in_draws = report_.stand_in_draws, w.generic_draws = report_.generic_draws;
        for (const auto &[why, n] : report_.skipped) w.skipped_draws += n;
        w.gpu_seconds = report_.gpu_seconds, w.gpu_generic_seconds = gpu_generic_seconds_;
        return w;
    }
    void extent_init() override {
        if (deferring()) return deferred_->extent_init();
        Call call(*this, nullptr, true);
        // Between two reports, the draws recorded so far handed to the GPU
        // once there are enough of them: the processor waits for the GPU
        // where the guest may read the extents (extent_wait), and a pass of
        // thousands of draws submitted only then left the GPU idle while it
        // was recorded and the processor idle while it was drawn. (Not
        // inside a report: one a submission begins in is the whole screen.)
        if (call && extent_flush_draws_ && slots_[current_].recording &&
            report_.drawn - drawn_at_submission_ >= extent_flush_draws_)
            flush();
        extent_open_ = true, extent_whole_ = false, extent_index_ = UINT32_MAX, extent_margin_ = 0;
    }
    uint64_t extent_report() override {
        if (deferring()) return deferred_->extent_report();
        Call call(*this, nullptr, true);
        return extent_reported();
    }
    void extent_wait() override {
        if (deferring()) return deferred_->extent_wait();
        Call call(*this, nullptr);
        if (!call) return;
        uint64_t serial = 0;
        for (const Slot &s : slots_)
            for (const Slot::ExtentReport &e : s.extent_reports)
                if (e.handle && s.serial) serial = std::max(serial, s.serial);
        if (!serial) return;
        // (timed for XF_SLOW_FRAMES: the whole wait, and of it the fence's)
        const uint64_t t0 = FastClock::now();
        const double fence0 = report_.wait_seconds;
        wait_serial(serial);
        extent_wait_seconds_ += FastClock::seconds(FastClock::now() - t0);
        extent_fence_seconds_ += report_.wait_seconds - fence0;
    }
    void extent_poll(std::vector<gpu::Extent> &resolved) override {
        if (deferring()) return deferred_->extent_poll(resolved);
        std::lock_guard<CallLock> lock(mutex_);
        resolved.insert(resolved.end(), extent_resolved_.begin(), extent_resolved_.end());
        extent_resolved_.clear();
    }
    // the processor's own writes (fences, reports), and a trace's memory
    // (Xenia's TracePlaybackWroteMemory): written through the physical view,
    // which no watch sees, so the watches are told; the mirror's pages and
    // the textures there are loaded again at their next use
    void memory_written(uint32_t address, uint32_t bytes) override {
        if (deferring()) return deferred_->memory_written(address, bytes);
        Call call(*this, nullptr, true);
        std::vector<Range> &written = draw_lists_.written; // (kept for its room)
        written.clear();
        add_span(written, address, bytes);
        for (auto [start, stop] : written) mem_.physical_written(start, stop - start);
    }

    // EDRAM as a trace began, in Xenia's layout (edram.glsl's)
    void restore_edram(std::span<const uint8_t> bytes) override {
        if (deferring()) deferred_->finish(); // (in order with the draws handed over)
        Call call(*this, nullptr);
        if (bytes.size() != EDRAM_BYTES || !call) return;
        std::fill(owners_.begin(), owners_.end(), nullptr);
        owners_epoch_++;
        VkBuffer from;
        VkDeviceSize at;
        uint8_t *data = staging(EDRAM_BYTES, from, at);
        if (!data) return;
        std::memcpy(data, bytes.data(), bytes.size());
        VkBufferCopy copy{at, 0, EDRAM_BYTES};
        VkCommandBuffer c = outside();
        gpu_mark("EDRAM: a trace's snapshot restored");
        // (at a resolution scale, through EDRAM at the console's size)
        vkCmdCopyBuffer(c, from, scale_.one() ? edram_.buffer : edram_1x_.buffer, 1, &copy);
        if (!scale_.one()) rescale_edram(true);
    }
    Report report() const override {
        std::lock_guard<CallLock> lock(mutex_);
        return with_processor_counts(report_);
    }
    bool try_report(Report &out) const override {
        std::unique_lock<CallLock> lock(mutex_, std::try_to_lock);
        if (!lock) return false;
        out = with_processor_counts(report_);
        return true;
    }
    bool failed(std::string &error) const override {
        std::lock_guard<CallLock> lock(mutex_);
        if (fatal_.empty()) return false;
        error = fatal_;
        return true;
    }
    void drain() override {
        if (deferred_) deferred_->finish();
        {
            Call call(*this, nullptr);
            if (call) sync();
        }
        // The workers' last pipelines and the cache's file waited for outside the renderer's
        // lock: they take a while (the whole pipelines queued last, a file of 100 MB and more),
        // and the back end's thread, which may go on with the processor's calls meanwhile,
        // waited for the lock all that time (a player's last frame, 160 ms).
        if (pipeline_workers_) pipeline_workers_->wait_all();
        bool writing;
        {
            std::lock_guard<CallLock> lock(mutex_);
            save_pipeline_cache(true);
            writing = cache_saver_.joinable();
        }
        while (writing && !cache_saver_done_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard<CallLock> lock(mutex_);
        save_pipeline_cache(); // (the write's thread joined, its size counted)
    }

    // ---- TraceSource: what a trace writer asks ----
    void set_memory_trace(gpu::MemoryTrace *trace) override {
        // (what the renderer reads recorded in order with the packets: no back end while it is)
        if (deferred_) deferred_->set_synchronous(trace != nullptr);
        std::lock_guard<CallLock> lock(mutex_);
        trace_ = trace;
    }
    void settle() override { drain(); }
    void set_texture_filtering(uint32_t anisotropy, uint32_t filter) override {
        anisotropy_.store(anisotropy, std::memory_order_relaxed);
        texture_filter_.store(filter, std::memory_order_relaxed);
    }
    uint32_t resolution_scale() const override { return scale_.q4; }
    std::string scale_notice() const override { return scale_notice_; }
    bool snapshot_edram(std::vector<uint8_t> &out) override;
    void gpu_memory(std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &out) override;

private:
    // adds the time from its making to its end to a count of seconds
    class Timer {
    public:
        // (on false: nothing timed, no clock read)
        explicit Timer(double &into, bool on = true) : into_(into), on_(on) {
            if (on_) start_ = now();
        }
        ~Timer() {
            if (on_) into_ += steady() ? double(now() - start_) * STEADY_TICK : FastClock::seconds(now() - start_);
        }

    private:
        // the cheap clock (fast_clock.h), or steady_clock with XF_RENDERER_STEADY_CLOCK=1 (as
        // before; the replay's own timing keeps the cheap one)
        static bool steady() { return steady_; }
        static inline const bool steady_ = [] {
            const char *v = std::getenv("XF_RENDERER_STEADY_CLOCK");
            return v && v[0] == '1';
        }();
        static constexpr double STEADY_TICK =
            double(std::chrono::steady_clock::period::num) / std::chrono::steady_clock::period::den;
        static uint64_t now() {
            return steady() ? uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) : FastClock::now();
        }
        double &into_;
        bool on_;
        uint64_t start_ = 0;
    };
    // a translation and its module's making, timed into the report
    // (Report::translate_seconds)
    class TranslateTimer {
    public:
        explicit TranslateTimer(Report &report) : report_(report), start_(std::chrono::steady_clock::now()) {}
        ~TranslateTimer() {
            const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
            report_.translate_seconds += spent;
            report_.translate_max = std::max(report_.translate_max, spent);
            report_.translations++;
        }

    private:
        Report &report_;
        std::chrono::steady_clock::time_point start_;
    };
    // Options::cpu_times (or XF_RENDERER_CPU_TIMES=1): the clock read around the work done at
    // every draw, texture and shader load too. Off, Report::check_seconds and vertex_seconds stay
    // 0 and Report::seconds leaves out the calls that neither draw, resolve nor swap: two clock
    // reads around each were 4% of the command thread in a scene of 3,000 draws.
    bool cpu_times_ = false; // (set in init)

    // One of the processor's calls: the renderer's state is its alone while
    // it runs (report() waits), the registers are the processor's as they
    // stand, and its time is counted. False once the device has failed.
    // XF_GPU_BACK_END=1: the processor's calls handed to the back end's thread, which makes them
    // again here (DeferredSink::passing on it)
    std::unique_ptr<gpu::DeferredSink> deferred_;
    bool deferring() const { return deferred_ && !gpu::DeferredSink::passing(); }
    static void print_back_end(const gpu::DeferredSink::Stats &s) {
        static const char *const why[] = {"fence", "interrupt", "wait", "gamma", "memory read"};
        std::string line;
        char part[96];
        for (size_t i = 0; i < size_t(gpu::SyncFor::COUNT); i++) {
            std::snprintf(part, sizeof(part), "%s%s %llu (%llu waited)", i ? ", " : "", why[i],
                          (unsigned long long)s.syncs[i], (unsigned long long)s.waited[i]);
            line += part;
        }
        std::fprintf(stderr,
                     "gpu back end: %llu records, %.1f MB, %llu register blocks, %llu fences written by it; syncs: "
                     "%s; %llu finishes, %llu waits for room; the front end waited %.1f ms\n",
                     (unsigned long long)s.records, double(s.bytes) / 1048576.0, (unsigned long long)s.blocks,
                     (unsigned long long)s.late_writes, line.c_str(), (unsigned long long)s.finishes,
                     (unsigned long long)s.full, s.wait_seconds * 1e3);
    }

    class Call {
    public:
        // (small: one of the many short calls, timed only with cpu_times_. With the work split in
        // two none is timed but with cpu_times_: the back end's busy time is Report::seconds
        // then, and two clock reads a call were 4% of its time)
        Call(Impl &r, const gpu::RegisterFile *registers, bool small = false)
            : r_(r), lock_(r.mutex_), timer_(r.report_.seconds, r.cpu_times_ || (!small && !r.deferred_)) {
            if (registers) r.regs_ = registers->data(), r.file_ = registers;
        }
        explicit operator bool() const { return r_.fatal_.empty(); }

    private:
        Impl &r_;
        std::lock_guard<CallLock> lock_;
        Timer timer_;
    };

    void load_shader(uint32_t type, std::span<const uint32_t> code);

    // draws
    void draw(const gpu::Draw &d);
    // a vertex on the screen as the vertex shader run on the CPU places it
    // (the viewport's scale and offset applied; not the window offset nor
    // the pixel centers' half)
    struct ScreenVertex {
        uint32_t index;
        float x, y;
    };
    uint32_t draw_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                        uint32_t primitive, bool tessellated, std::vector<ScreenVertex> *screen = nullptr) const;
    uint32_t vertex_max_y(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                          uint32_t primitive, std::vector<ScreenVertex> *screen = nullptr) const;
    // the console's pixels a draw can reach, [x0, x1) x [y0, y1) each
    struct Reach {
        uint32_t x0, y0, x1, y1;
    };
    void draw_reach(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                    uint32_t primitive, bool tessellated, const VkViewport &viewport, const VkRect2D &scissor,
                    std::vector<Reach> &reach) const;
    mutable std::vector<ScreenVertex> reach_vertices_; // (draw_reach's own, kept for its room)
    std::vector<Reach> reach_;                         // (the draw's)
    std::vector<TileRect> reach_rects_;
    // What a draw provably writes into its depth target whatever the target
    // held (Impl::depth_overwrite): every sample of the console's pixels
    // [x0, x1) x [y0, y1), and whether with one known word (the guest's
    // depth and stencil, D24S8 or D24FS8 as the target's format).
    // whole: the depth and the stencil; depth_whole: the depth, the stencil
    // replaced (whole) or kept as it was; known_depth: the depth written is
    // the word's (and the stencil, when whole); constant: whole with a known
    // depth.
    struct DepthOverwrite {
        bool whole = false, depth_whole = false, known_depth = false;
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool constant = false;
        uint32_t word = 0;
    };
    DepthOverwrite depth_overwrite(const gpu::Draw &d, const Shader &vs, const Shader *ps,
                                   const harness::Constants &constants, const std::vector<uint32_t> &indices,
                                   uint32_t depth_control, const RenderTarget &depth, const VkViewport &viewport,
                                   const float *ndc_scale, const float *ndc_offset, const VkRect2D &scissor,
                                   const char *&why) const;
    // a depth target's clear value of the guest's word, as the target keeps it
    // (xenos::UNorm24To32, or D24FS8 halved)
    static VkClearValue depth_clear_value(uint32_t format, uint32_t word) {
        VkClearValue v{};
        uint32_t d24 = word >> 8;
        v.depthStencil.depth =
            format == 0 ? float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f) : float24_to_float(d24) * 0.5f;
        v.depthStencil.stencil = word & 0xFF;
        return v;
    }
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
    void approximate(const std::string &what) {
        report_.approximate[what]++, approximations_++;
        front_keepable_ = pipeline_keepable_ = false; // (a count by a made string is not kept: draw_front)
    }
    void use(const std::string &what) {
        report_.used[what]++;
        front_keepable_ = pipeline_keepable_ = false;
    }
    // A literal's counts by its address: no string made and no map of
    // strings searched at each draw (1.8% of the command thread and the
    // allocations with it in a heavy scene), folded into the report's maps
    // as it is taken (with_processor_counts).
    std::unordered_map<const char *, uint64_t> used_by_literal_, approximate_by_literal_;
    struct UseSlot {
        const char *what = nullptr;
        uint64_t *count = nullptr;
    };
    UseSlot use_slots_[8]; // use()'s, never emptied (used_by_literal_ never is)
    // XF_RESOLVE_TARGET_DETAIL=1: each refused resolve target counted with
    // its format, size and pitch beside the resolve's (resolve_targets)
    bool refusal_detail_ = [] {
        const char *v = std::getenv("XF_RESOLVE_TARGET_DETAIL");
        return v && v[0] == '1';
    }();
    Texture::StaleBy stale_by_; // the resolve under way, for textures_written_by_gpu
    bool exporting_ = false;    // textures_written_by_gpu called for a memory export
    // depth resolves feeding textures, and depth textures as targets
    // (resolve_targets; XF_RESOLVE_TARGETS_DEPTH=0 turns both off)
    bool depth_targets_ = [] {
        const char *v = std::getenv("XF_RESOLVE_TARGETS_DEPTH");
        return !(v && v[0] == '0');
    }();
    // a resolve feeding a texture whose memory holds its destination a whole
    // number of 32x32 tiles in, across as well as down (one part of an
    // atlas; resolve_targets; XF_RESOLVE_TARGET_TILES=0: whole tile rows only)
    bool target_tiles_ = [] {
        const char *v = std::getenv("XF_RESOLVE_TARGET_TILES");
        return !(v && v[0] == '0');
    }();
    // a stale target fed by resolves that each write a part of it, current
    // again once together they wrote all of it (Texture::covered;
    // XF_RESOLVE_TARGET_COVER=0: only a resolve writing all of it at once)
    bool target_cover_ = [] {
        const char *v = std::getenv("XF_RESOLVE_TARGET_COVER");
        return !(v && v[0] == '0');
    }();
    // and a stale one at the resolution scale too, its scaled texels written
    // by the scaled pass (XF_SCALED_COVER=0: those loaded again; game G's
    // shadow atlas at 200%, 0.83 ms a play)
    bool scaled_cover_ = [] {
        const char *v = std::getenv("XF_SCALED_COVER");
        return !(v && v[0] == '0');
    }();
    // At a resolution scale, a resolve leaves its destination out of resolved
    // memory at the scale where no texture load looked there since resolves
    // of two frames wrote it (its pages' tags cleared: the textures it feeds
    // take what it wrote). A texture loaded there next is the console's size
    // that once, and its pages are written by every resolve from then on.
    // XF_SCALED_MEMORY_UNREAD=1: every resolve writes it (Impl::ScaledRegion).
    bool scaled_skip_unread_ = [] {
        const char *v = std::getenv("XF_SCALED_MEMORY_UNREAD");
        return !(v && v[0] == '1');
    }();
    // A texture over resolved memory a part of which is kept at the scale
    // loads at the scale, the rest of it made from guest memory first (as a
    // resolve makes the units it writes a part of: scaled_fill.comp), as
    // Xenia scales a texture any of whose pages a scaled resolve wrote. A
    // screen resolved in two parts and read between them (the second part's
    // memory then holds the last frame's) was otherwise loaded, and fed by
    // the second resolve, at the console's size. XF_SCALED_FILL_LOAD=0: such
    // a texture loads at the console's size.
    bool scaled_fill_load_ = [] {
        const char *v = std::getenv("XF_SCALED_FILL_LOAD");
        return !(v && v[0] == '0');
    }();
    // Mipmapped textures over resolved memory loaded at the scale level by
    // level, each level from its own scaled tiles (a mip chain resolved a
    // level at a time), where every level's size at the scale is the image's
    // level size (always at whole scales of power-of-two sizes).
    // XF_SCALED_MIPS=0: those load at the console's size.
    bool scaled_mips_ = [] {
        const char *v = std::getenv("XF_SCALED_MIPS");
        return !(v && v[0] == '0');
    }();
    // a page's resolved memory at the scale (0: never written): bits 0-4 the
    // frames whose resolves wrote it since a texture load last looked there,
    // bit 5 left out since, bit 6 looked at since the last write, bit 7
    // always written (a load found it left out); and the swap of its last
    // write counted
    std::vector<uint8_t> scaled_reads_;
    std::vector<uint32_t> scaled_write_swaps_;
    void approximate(const char *what) {
        approximate_by_literal_[what]++, approximations_++;
        if (front_recording_) front_counts_.push_back({what, true});
        if (pipeline_recording_) pipeline_counts_.push_back({what, true});
    }
    void use(const char *what) {
        // (a literal's count found again by its address in a few kept slots, before the map: a
        // draw counts the same few things, and the map's lookup was a seventieth of the back end;
        // the map's values stay where they are as it grows)
        UseSlot &slot = use_slots_[(reinterpret_cast<uintptr_t>(what) >> 3) & 7];
        if (slot.what != what) slot.what = what, slot.count = &used_by_literal_[what];
        ++*slot.count;
        if (front_recording_) front_counts_.push_back({what, false});
        if (pipeline_recording_) pipeline_counts_.push_back({what, false});
    }
    // the report with what the processor counted of the packets the renderer
    // does not see (Report::used)
    Report with_processor_counts(Report r) const {
        for (const auto &[what, count] : used_by_literal_) r.used[what] += count;
        for (const auto &[what, count] : approximate_by_literal_) r.approximate[what] += count;
        const gpu::Counts &n = processor_.counts();
        auto put = [&](const char *what, uint64_t count) {
            if (count) r.used[what] = count;
        };
        put("EVENT_WRITE_EXT packets (screen extent reports)", n.opcodes[pm4::EVENT_WRITE_EXT].load());
        put("VIZ_QUERY packets", n.opcodes[pm4::VIZ_QUERY].load());
        put("draws with no valid index source (not drawn)", n.failed_draws.load());
        if (deferred_) r.back_end_cpu_ticks = deferred_->back_cpu_ticks();
        // (the calls are untimed with the work split: the back end's busy time is the renderer's)
        if (deferred_ && !cpu_times_) r.seconds = deferred_->back_busy_seconds();
        if (ring_) r.record_cpu_ticks = ring_->cpu_ticks();
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
    // after a lost device: when (the swaps and draws so far) and what the
    // driver says went wrong (vk::Device::fault_report); empty otherwise
    std::string lost_device_report(VkResult r) {
        if (r != VK_ERROR_DEVICE_LOST) return {};
        std::string out = ", the device lost after " + std::to_string(report_.swaps) + " swaps and " +
                          std::to_string(report_.draws) + " draws";
        std::string fault = d_.fault_report();
        return (fault.empty() ? out : out + ": " + fault) + "; " + d_.heap_report();
    }
    void gpu_collect(Slot &s);
    std::string target_name(const RenderTarget &t) const;

    // resources
    Shader *shader(uint64_t hash);
    // whether the shader can be drawn (its first form translated); with
    // `pending`, the translation may go to the workers, pending set meanwhile
    bool translate(Shader &s, std::string &why, bool *pending = nullptr);
    // a translated vertex shader's module in a host form; null when the
    // translator cannot make it, or (pending set) while the workers do
    VkShaderModule vertex_module(Shader &s, const shader::VertexOptions &options, std::string &why,
                                 bool *pending = nullptr);
    VkShaderModule pixel_module(Shader &s, const shader::PixelOptions &options, std::string &why,
                                bool *pending = nullptr);
    void analyze(Shader &s);
    VkShaderModule form_module(Shader &s, uint32_t key, const shader::VertexOptions &vertex,
                               const shader::PixelOptions &pixel, bool *pending);
    VkShaderModule finish_form(Shader &s, uint32_t key, const shader::Translation &t);
    bool translate_later(Shader &s, uint32_t key);
    void collect_translations();
    // the translation workers (docs/renderer.md, "The translation off the
    // renderer's thread"): with async pipelines, the generic shaders and
    // pipeline libraries
    std::unique_ptr<TranslationWorkers> translation_workers_;
    // the depth-only draws' pixel shaders that convert D24FS8 depth
    // (Options::depth_float24_convert_in_pixel_shader): towards zero, nearest even
    VkShaderModule depth_float24_fs_[2] = {};
    // the pixel shader of the placeholder pipelines (Options::pipeline_stand_in)
    VkShaderModule placeholder_fs_ = VK_NULL_HANDLE;
    // the generic shaders (init_generic): on (Options::generic_shaders, not
    // XF_GENERIC_SHADERS=0), the program buffer and what of it is written
    // (16-byte units), the modules ([point coordinates][writes depth])
    bool generic_on_ = false;
    // (Options::pipeline_cache_first and translate_here_ms, as init_generic takes them)
    bool cache_first_ = false;
    double translate_here_seconds_ = 0;
    // (Options::generic_gpu_ms_a_frame) the budget, the GPU's time a generic draw of each pixel
    // shader took the last time one was timed, what the frame's generic draws are counted to
    // take so far, and the frame's draws of a shader not timed yet
    double generic_budget_seconds_ = 0, generic_spent_frame_ = 0;
    uint32_t generic_unknown_a_frame_ = 0, generic_unknown_frame_ = 0;
    std::unordered_map<uint64_t, double> generic_cost_;
    bool generic_allowed();
    void generic_collect(Slot &s);
    bool generic_timing_ = false; // (timestamps on the queue)
    vk::Buffer programs_;
    uint32_t programs_used_ = 0;
    // (the vertex shader's: [screen extents][none, clip, cull distances])
    VkShaderModule generic_vs_[2][3] = {}, generic_fs_[2][2] = {};
    bool init_generic(std::string &error);
    bool program(Shader &s, std::string &why);
    // a draw's generic pipeline: its info with the generic shaders in place
    // of the guest's (false, with why, where they cannot draw it), and the
    // stand-in linked from its libraries (made on the workers the first time)
    // (the specialization: generic.glsl's POINT_SNAP, generic.vert's CLIP_PLANES)
    struct GenericStages {
        VkPipelineShaderStageCreateInfo stages[5];
        VkSpecializationMapEntry entries[2] = {{21, 0, 4}, {22, 4, 4}};
        uint32_t vertex_data[2] = {0, 0}, pixel_data = 1;
        VkSpecializationInfo vertex_spec{2, entries, 8, vertex_data}, pixel_spec{1, entries, 4, &pixel_data};
        VkGraphicsPipelineCreateInfo info{};
    };
    bool generic_info(const VkGraphicsPipelineCreateInfo &info, Shader &vs, Shader *ps, bool points, uint32_t vertex_form,
                      uint32_t pixel_form, GenericStages &out, std::string &why);
    VkPipeline generic_stand_in(PipelineEntry &e, const VkGraphicsPipelineCreateInfo &generic, bool async);
    void retire_generic(PipelineEntry &e);
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
    void release_index(uint32_t kind, uint32_t index, bool owner_reloaded = false);
    // A new place in the resolves' table for a texel size (none given back
    // to take): a place never taken, else the place of the least recently
    // used texture holding one that nothing recorded or in flight uses (it
    // is then no resolve target: a resolve over it makes it stale, and its
    // next use loads it as any texture), else none (UINT32_MAX). A game
    // with many render-to-texture surfaces had more than the table holds,
    // and the ones made last were never fed, loaded again after each
    // resolve instead (renderer.md, "Resolve targets beyond the table").
    uint32_t take_target(uint32_t wide) {
        if (next_target_[wide] < RESOLVE_TARGETS) return next_target_[wide]++;
        for (Texture *t = lru_oldest_; t; t = t->newer) {
            // (the list runs oldest use first: past one in use, all are)
            if (t->used_serial > done_serial_) break;
            if (!(t->target & 0x7F) || ((t->target >> 7) & 1) != wide) continue;
            const uint32_t slot = (t->target & 0x7F) - 1;
            if (t->target_view) vkDestroyImageView(dev_, t->target_view, nullptr);
            t->target = 0, t->target_view = VK_NULL_HANDLE;
            report_.resolve_targets_reclaimed++;
            return slot;
        }
        use("resolve targets without a place in the resolves' table (all in use)");
        return UINT32_MAX;
    }
    // a resolve target's slot given back and its view destroyed (the GPU done with them)
    void release_target(uint32_t target, VkImageView view) {
        if (view) vkDestroyImageView(dev_, view, nullptr);
        if (target & 0x7F) free_targets_[(target >> 7) & 1].push_back((target & 0x7F) - 1);
    }
    // the textures (up to two) a resolve to this destination feeds, written
    // by it as well (resolve targets not loaded again since): how many
    // (and the columns and rows of texels, whole tiles, the destination starts
    // at in them, the same in both)
    uint32_t resolve_targets(const resolve::Info &info, Texture *targets[2], uint32_t &extra_columns,
                             uint32_t &extra_rows);
    uint32_t resolve_targets_by_tiles(const resolve::Info &info, Texture *targets[2], uint32_t &extra_columns,
                                      uint32_t &extra_rows, uint32_t bucket, uint32_t base, uint32_t bytes,
                                      uint32_t wide);
    // A stale target's tiles a feeding resolve wrote (texels [x0, x1) x
    // [y0, y1)): true once all of its tiles are, its image then what memory
    // holds (Texture::covered). uncover forgets them.
    static bool cover(Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
    static void uncover(Texture &t) {
        if (t.covered_count) t.covered.clear(), t.covered_count = 0;
    }
    void scale_texture(Texture &t, uint32_t width, uint32_t height);
    // A scaled resolve no texture was over yet (docs/renderer.md,
    // "Resolution scale"): its texels at the resolution scale, kept in an
    // image of its own for the texture a later draw makes over the
    // destination (texture() copies it in and makes that texture scaled),
    // instead of the console's size its memory holds. By the destination's
    // base; what the resolve wrote, for dropping it when anything else writes
    // there; the swap it was made at, dropped two swaps on.
    struct ScaledResolve {
        vk::Image image;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t target = 0; // its place in the resolves' table plus one, bit 7 64-bit
        uint32_t pitch = 0, endian = 0, width = 0, height = 0;
        uint32_t low = 0, high = 0;
        uint64_t swap = 0;
    };
    std::map<uint32_t, ScaledResolve> scaled_resolves_;
    ScaledResolve *scaled_resolve(const resolve::Info &info, const std::vector<Range> &written);
    void drop_scaled_resolves(const std::vector<Range> &written, bool all = false);
    // Resolved memory at the resolution scale (docs/renderer.md, "Resolution
    // scale"; Xenia's scaled resolve buffer): what a resolve writes, also kept
    // at the scale in a heap on the GPU, for every texture over it to load at
    // the scale (texture_load.comp from it, by tiling.glsl's
    // scaled_offset_2d), whatever its format, size, layers or offset; guest
    // memory and the mirror keep the console's size. Regions of guest memory
    // (64 KB aligned, merged with those within 256 KB) each own a run of the heap, the
    // region's bytes times the scale's area. A 4 KB page's tag says what its
    // scaled bytes hold: 0 nothing current, else the bytes per block's log2
    // plus one and, for 64- and 128-bit blocks (whose tiles span two and four
    // pages), the page's place in its tile from bit 4. Cleared by what else
    // writes there: the CPU (request), exports, resolves not kept at the
    // scale. XF_SCALED_MEMORY=0: none (the textures no resolve feeds whole
    // stay the console's size, as before).
    struct ScaledRegion {
        uint32_t end = 0;
        VkDeviceSize heap = 0;
    };
    bool scaled_memory_ = false;
    vk::Buffer scaled_heap_;
    VkDeviceSize scaled_heap_used_ = 0;
    std::map<uint32_t, ScaledRegion> scaled_regions_;
    std::vector<uint8_t> scaled_tags_;
    // gpu_resolve's units of the destination at a scale (an address, its tiles written whole), kept
    std::vector<std::pair<uint32_t, uint32_t>> resolve_units_;
    VkDescriptorSet scaled_resolve_set_ = VK_NULL_HANDLE, scaled_load_set_ = VK_NULL_HANDLE,
                    scaled_fill_set_ = VK_NULL_HANDLE;
    VkPipeline scaled_fill_pipeline_ = VK_NULL_HANDLE;
    const std::pair<const uint32_t, ScaledRegion> *scaled_region(uint32_t low, uint32_t high) const;
    const std::pair<const uint32_t, ScaledRegion> *make_scaled_region(uint32_t low, uint32_t high);
    bool repack_scaled_heap(const std::vector<std::pair<uint32_t, ScaledRegion>> &merged, uint32_t start,
                            ScaledRegion &region);
    // An address of a region as the shaders take it (tiling.glsl's
    // scaled_offset_2d): in guest bytes from the heap's start, its region's
    // run of the heap (a multiple of 4096 times the scale's area: regions
    // are 64 KB multiples) counted as the guest bytes it holds
    uint32_t scaled_rel(const std::pair<const uint32_t, ScaledRegion> &region, uint32_t address) const {
        return address - region.first + uint32_t(region.second.heap / (scale_.q4 * scale_.q4) * 16);
    }
    void write_scaled_sets();
    void clear_scaled_tags(uint32_t start, uint32_t end) {
        if (scaled_tags_.empty() || end <= start) return;
        std::fill(scaled_tags_.begin() + (start >> 12), scaled_tags_.begin() + ((end + 4095) >> 12), uint8_t(0));
    }
    void clear_scaled_tags(const std::vector<Range> &ranges) {
        for (auto [start, end] : ranges) clear_scaled_tags(start, end);
    }
    // the tag a page holds when a 2D surface at base with blocks of 1 <<
    // log2_bytes is kept at the scale there
    static uint8_t scaled_tag(uint32_t page_address, uint32_t base, uint32_t log2_bytes) {
        const uint32_t pages = log2_bytes >= 3 ? 1u << (log2_bytes - 2) : 1u;
        return uint8_t((log2_bytes + 1) | ((((page_address - (base & ~4095u)) >> 12) & (pages - 1)) << 4));
    }
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
    bool gpu_load(Texture &t, const vk::Image &image, const texture::Plan &plan, bool fresh, bool upload,
                  const std::vector<uint32_t> *scaled_bases = nullptr);
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
    // Each module's SPIR-V by its hash. A description names its modules (the
    // guest shader and form), which stay the same when the translator
    // changes what it writes for them; the build step's record of what it
    // made keys the modules' code too (built_key), so a run after such a
    // change does not take a library for made when the pipeline cache holds
    // only an older code's.
    std::unordered_map<VkShaderModule, uint64_t> module_content_;
    // a description's or a library key's hash with the code of its modules
    // (which: a library's part, or 4 for a whole pipeline's stages)
    uint64_t built_key(uint32_t seed, const std::vector<uint32_t> &words, const VkGraphicsPipelineCreateInfo &info,
                       uint32_t which) const;
    // a pipeline or library the build step made, looked up in the pipeline
    // cache alone (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
    // where the device has it): false when the driver would have to compile
    // it, which is then left to the workers rather than done on the
    // renderer's thread
    bool from_cache(const VkGraphicsPipelineCreateInfo &info, VkPipeline &out);
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
    double pipeline_seconds_at_swap_ = 0, translate_seconds_at_swap_ = 0;
    std::chrono::steady_clock::time_point last_swap_time_{};
    std::chrono::steady_clock::time_point first_swap_time_{}; // XF_GPU_TIME_LOG's
    std::map<std::string, double> gpu_time_logged_;            // its totals at the last line

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
    void native_pass(native::PassInfo &pass);
    VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                          VkComponentMapping components = {}, uint32_t levels = 1, uint32_t layers = 1,
                          bool keep = true);
    VkShaderModule module(const uint32_t *words, size_t bytes) {
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = bytes;
        m.pCode = words;
        VkShaderModule out = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &m, nullptr, &out);
        if (out) module_content_[out] = ShaderStorage::hash(0, {words, bytes / 4});
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
    // every way the renderer's commands write memory, and read it (full_barrier):
    // shaders, attachments, transfers, uniform, index and vertex buffers. No
    // draw or dispatch is indirect; the conditional rendering's predicate
    // has its own barrier where it is written (viz_end).
    static constexpr VkAccessFlags2 BARRIER_WRITES =
        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    static constexpr VkAccessFlags2 BARRIER_READS =
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
        VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    static inline const bool barrier_generic_ = [] {
        const char *v = std::getenv("XF_BARRIER_ACCESSES");
        return v && std::string(v) == "generic";
    }();
    static void full_barrier(VkCommandBuffer cmd) {
        // Everything the GPU wrote, seen by everything the GPU does next. The
        // accesses are named one by one: the generic MEMORY_READ covers reads
        // nothing here makes after most barriers (indirect commands, the
        // conditional rendering's predicate, the host's), and a barrier whose
        // destination has the first two makes the NVIDIA driver stall the
        // GPU: 2 to 7 us each against under 1 us for these (docs/renderer.md,
        // "The barriers' accesses"). What the host reads is covered once, at
        // the submission's end (flush). XF_BARRIER_ACCESSES=generic: as before.
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (barrier_generic_) {
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        } else {
            b.srcAccessMask = BARRIER_WRITES;
            b.dstAccessMask = BARRIER_WRITES | BARRIER_READS;
        }
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
        const uint64_t t0 = FastClock::now();
        flush();
        while (fatal_.empty() && finish_oldest(true)) {}
        drain_seconds_ += FastClock::seconds(FastClock::now() - t0);
        drains_++;
    }
    // the render pass ended, if one is under way
    void end_pass() {
        end_query(); // a query does not outlive its render pass
        if (in_pass_) {
            vkCmdEndRendering(slots_[current_].cmd);
        }
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
    // ---- VIZ queries ----
    // VIZ_QUERY (Xenia's occlusion_query_viz, off by default there; beyond
    // its default here, where the device has conditional rendering): the
    // survey draws of a query (PA_SC_VIZ_QUERY's viz_query_ena with
    // kill_pix_post_hi_z, killed after hi-Z on the console) counted by a host
    // occlusion query, or the interlock path's counter, of their own (never
    // for an occlusion report, as Xenia's); at the query's end that count
    // copied to the ID's word in viz_predicates_, and the draws predicated
    // on the ID after it (DRAW_INDX's VIZ token, bit 8) drawn under
    // conditional rendering on that word, so the GPU skips them where the
    // survey passed no sample. A query whose survey draws were not all
    // measured, in one count, is visible (its draws drawn, as Xenia's
    // fallback); one with no survey draw at all is not (its draws dropped, as
    // Xenia's). A survey is measured only as the console would see it: no
    // color, depth or stencil writes, hi-Z on (Xenia tests depth then; with
    // hi-Z off nothing rejects a survey, and our depth test could). Draws
    // exporting memory are never predicated or dropped.
    struct VizQuery {
        bool active = false, ended = false;
        bool surveyed = false; // a survey draw came
        bool fallback = false; // one was not measured: visible
        bool armed = false;    // its count copied to its word
        uint32_t segments = 0; // the counts its surveys went into
        uint64_t serial = 0;   // the submission of its count
        uint32_t index = 0;    // its query (host) or counter (interlock) in that submission
    };
    vk::Buffer viz_predicates_; // a word an ID (the device has conditional rendering)
    VizQuery viz_[64];
    uint32_t viz_open_ = UINT32_MAX;      // the ID whose host query is open in the pass
    uint32_t viz_survey_ = UINT32_MAX;    // the draw being made is a survey measured for this ID
    uint32_t viz_predicate_ = UINT32_MAX; // the draw being made is predicated on this ID's word
    bool viz_measured_ = false;           // the survey being made was counted
    // the draw about to be made: a survey of an active query (its ID), and
    // whether it can be measured (viz_survey_)
    uint32_t viz_survey_begin() {
        viz_survey_ = viz_predicate_ = UINT32_MAX, viz_measured_ = false;
        uint32_t viz = regs_[reg::PA_SC_VIZ_QUERY];
        if (!viz_predicates_.buffer || !(viz & 1)) return UINT32_MAX;
        uint32_t id = (viz >> 1) & 63;
        VizQuery &q = viz_[id];
        if (!q.active) return UINT32_MAX;
        // Xenia's OnVIZSurveyDraw: any draw with viz_query_ena counts for its
        // active query, a survey (kill_pix_post_hi_z: counted and killed after
        // hi-Z) or a draw as any other, which is drawn under its own state and
        // not measured here: its query stays visible (the caller's fallback)
        q.surveyed = true;
        if (!(viz & 0x80)) return id;
        uint32_t mode = regs_[reg::RB_MODECONTROL] & 7, depth = regs_[reg::RB_DEPTHCONTROL];
        bool color = EdramMode(mode) == EdramMode::ColorDepth && (regs_[reg::RB_COLOR_MASK] & 0xFFFF);
        bool hiz = (regs_[r::RB_HIZCONTROL] >> 1) & 1;
        if (!color && !(depth & 5) && hiz) viz_survey_ = id;
        return id;
    }
    void viz_end_host() {
        if (viz_open_ == UINT32_MAX) return;
        vkCmdEndQuery(slots_[current_].cmd, slots_[current_].occlusion, viz_[viz_open_].index);
        viz_open_ = UINT32_MAX;
    }
    // a survey's count in this submission: its own, or the one its query's
    // earlier surveys opened (a second makes the query visible)
    bool viz_count(VizQuery &q) {
        Slot &s = slots_[current_];
        if (q.segments && q.serial == s.serial && (fsi_ || viz_open_ == viz_survey_)) return true;
        if (s.occlusion_reports.size() >= OCCLUSION_QUERIES || (!fsi_ && !s.occlusion)) return false;
        q.index = uint32_t(s.occlusion_reports.size());
        s.occlusion_reports.push_back(0); // no report's
        s.occlusion_used = uint32_t(s.occlusion_reports.size());
        q.serial = s.serial;
        q.segments++;
        return true;
    }
    // the host path: the survey's draw counted in its query's host query
    void viz_draw(VkCommandBuffer c) {
        VizQuery &q = viz_[viz_survey_];
        if (viz_open_ != viz_survey_) {
            end_query(); // the report's and another ID's: one occlusion query at a time
            if (!viz_count(q)) return;
            vkCmdBeginQuery(c, slots_[current_].occlusion, q.index, 0);
            viz_open_ = viz_survey_;
        }
        viz_measured_ = true;
    }
    // the interlock path: the survey's counter
    uint32_t viz_fsi_index() {
        VizQuery &q = viz_[viz_survey_];
        if (!viz_count(q)) return UINT32_MAX;
        viz_measured_ = true;
        return current_ * OCCLUSION_QUERIES + q.index;
    }
    void viz_query_event(uint32_t id, bool end) {
        if (!viz_predicates_.buffer) return;
        VizQuery &q = viz_[id];
        if (viz_open_ == id) viz_end_host();
        if (!end) {
            q = {};
            q.active = true;
            return;
        }
        if (!q.active) return;
        q.active = false, q.ended = true;
        report_.viz_queries++;
        if (!q.surveyed) return;
        Slot &s = slots_[current_];
        if (q.fallback || q.segments != 1 || !s.recording || q.serial != s.serial) {
            q.fallback = true;
            return;
        }
        // the count to the ID's word, outside the render pass
        end_pass();
        VkCommandBuffer c = cmd();
        full_barrier(c);
        if (fsi_) {
            VkBufferCopy copy{VkDeviceSize(current_ * OCCLUSION_QUERIES + q.index) * 4, VkDeviceSize(id) * 4, 4};
            vkCmdCopyBuffer(c, zpd_counters_.buffer, viz_predicates_.buffer, 1, &copy);
        } else {
            vkCmdCopyQueryPoolResults(c, s.occlusion, q.index, 1, viz_predicates_.buffer, VkDeviceSize(id) * 4, 4,
                                      VK_QUERY_RESULT_WAIT_BIT);
        }
        full_barrier(c);
        if (!barrier_generic_) {
            // the predicate's word for the draws predicated on it: the one
            // read full_barrier's accesses leave out (a barrier that names it
            // makes the driver wait for the GPU, so only here, where it is written)
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
            b.dstAccessMask = VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(c, &dep);
        }
        q.armed = true;
        report_.viz_armed++;
    }
    // ---- screen extents ----
    // EVENT_WRITE_EXT (gpu::ScreenExtents::Measured, beyond Xenia, which
    // answers the whole screen): the draws between a SCREEN_EXT_INIT and the
    // SCREEN_EXT_RPT after it bound their report on the GPU, each final
    // vertex position taking the report's rectangle out to it
    // (shader::VertexOptions::extents) in the slot's share of extents_
    // (device-local: atomics over the bus cost a UE3 game's frame ten times
    // its GPU time), zeroed as the slot begins recording, copied to
    // extents_readback_ as it ends and read as its submission is done. A report the renderer cannot bound whole
    // (a draw there skipped, drawn some other way, or after a submission
    // began; more reports than a slot holds) is answered as the whole
    // screen. Wide lines add half their width around.
    static constexpr uint32_t EXTENT_REPORTS = 4096; // a slot's
    vk::Buffer extents_, extents_readback_;
    bool extent_open_ = false;                 // between an init and its report
    bool extent_whole_ = false;                // a draw there not bounded
    uint32_t extent_index_ = UINT32_MAX;       // its report in the slot, once a draw bounds it
    uint64_t extent_serial_ = 0;               // the submission that report is in
    uint32_t extent_margin_ = 0;               // pixels around the bounds
    uint64_t extent_next_ = 1;                 // the next report's handle
    bool extent_drawn_ = false;                // the draw being made bounded its report
    std::vector<gpu::Extent> extent_resolved_; // for extent_poll
    // extent_init's submissions: after this many draws (XF_EXTENT_FLUSH_DRAWS, 0 for none), and the
    // draws drawn when the submission being recorded began
    uint64_t extent_flush_draws_ = [] {
        const char *v = std::getenv("XF_EXTENT_FLUSH_DRAWS");
        return v ? uint64_t(std::strtoull(v, nullptr, 10)) : uint64_t(512);
    }();
    uint64_t drawn_at_submission_ = 0;
    double extent_wait_seconds_ = 0, extent_fence_seconds_ = 0; // extent_wait's, all and the fences'
    double back_end_gpu_wait_seconds_ = 0;                      // the fences waited for on the back end's thread
    // the processor time spent inside vkWaitForFences, every thread's (a driver may spin there);
    // timed only with XF_SLOW_FRAMES set
    std::atomic<uint64_t> gpu_wait_cpu_ticks_{0};
    const bool time_gpu_wait_cpu_ = std::getenv("XF_SLOW_FRAMES") != nullptr;
    // (XF_SLOW_FRAMES's back end line, Sink::Work) the GPU drained by sync() (its time and count),
    // the time the renderer's thread spent writing the pipeline cache's file (or handing it to
    // its thread), the GPU's time in the generic shaders' draws (with the GPU timed)
    double drain_seconds_ = 0, cache_save_seconds_ = 0, gpu_generic_seconds_ = 0;
    uint64_t drains_ = 0;
    uint32_t gpu_generic_kind_ = UINT32_MAX, gpu_pass_kind_ = UINT32_MAX;
    // the draw being made bounds the open report (the index of its words in
    // extents_), or UINT32_MAX: it cannot, and the report is the whole screen
    uint32_t extent_draw_words() {
        if (!extent_open_ || extent_whole_) return UINT32_MAX;
        begin_recording();
        Slot &s = slots_[current_];
        if (extent_index_ == UINT32_MAX) {
            if (s.extent_reports.size() >= EXTENT_REPORTS) {
                extent_whole_ = true;
                return UINT32_MAX;
            }
            extent_index_ = uint32_t(s.extent_reports.size());
            extent_serial_ = s.serial;
            s.extent_reports.push_back({});
            s.extents_used = uint32_t(s.extent_reports.size());
        } else if (extent_serial_ != s.serial) {
            extent_whole_ = true; // a submission began inside the report
            return UINT32_MAX;
        }
        return (current_ * EXTENT_REPORTS + extent_index_) * 4;
    }
    uint64_t extent_reported() {
        bool bounded = extent_open_ && !extent_whole_ && extent_index_ != UINT32_MAX && slots_[current_].recording &&
                       extent_serial_ == slots_[current_].serial;
        extent_open_ = false;
        report_.extent_reports++;
        if (!bounded) return 0;
        report_.extent_bounded++;
        Slot::ExtentReport &e = slots_[current_].extent_reports[extent_index_];
        e.handle = extent_next_++, e.margin = extent_margin_;
        return e.handle;
    }
    // the slot's reports bounded, as its submission is done: 8192 less the
    // left and top edges, the right and bottom ones plus one (0: no position
    // reached it, the whole screen), widened by the margin and a pixel
    void extent_collect(Slot &s, uint32_t slot) {
        if (s.extent_reports.empty()) return;
        const uint32_t *words =
            reinterpret_cast<const uint32_t *>(extents_readback_.mapped) + slot * EXTENT_REPORTS * 4;
        for (size_t i = 0; i < s.extent_reports.size(); i++) {
            const Slot::ExtentReport &e = s.extent_reports[i];
            if (!e.handle) continue;
            const uint32_t *w = words + i * 4;
            gpu::Extent x;
            x.handle = e.handle;
            x.whole = !w[0] || !w[1] || !w[2] || !w[3];
            if (!x.whole) {
                int32_t margin = int32_t(e.margin) + 1;
                x.left = uint32_t(std::max(0, int32_t(8192 - std::min(w[0], 8192u)) - margin));
                x.top = uint32_t(std::max(0, int32_t(8192 - std::min(w[1], 8192u)) - margin));
                x.right = std::min(8192u, w[2] - 1 + uint32_t(margin));
                x.bottom = std::min(8192u, w[3] - 1 + uint32_t(margin));
            }
            extent_resolved_.push_back(x);
        }
        s.extent_reports.clear();
    }
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
        viz_end_host();
        if (zpd_active_ == UINT32_MAX) return;
        // (the interlock path's counters are no host queries)
        if (!fsi_) {
            vkCmdEndQuery(slots_[current_].cmd, slots_[current_].occlusion, zpd_active_);
        }
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
        viz_end_host(); // a survey's query: one occlusion query at a time
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
            it->second.samples += samples[i] * 16 / (scale_.q4 * scale_.q4); // (the console's samples, at a resolution scale)
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
    // the draws' constants (both stages', the system's, the interlock's):
    // the draw-data arena in video memory where there is one, the arena
    // otherwise. Offsets are into draw_buffer().
    VkDeviceSize draw_data_take(VkDeviceSize bytes, VkDeviceSize align);
    // bytes into the draw data or the arena: a copy the compiler cannot fold
    // with one around it (the destination may be write-combined device
    // memory, hundreds of times slower to read than to write). In 16-byte
    // stores, in order, the rest by words (XF_UPLOAD_STORES=0: memcpy's): the
    // C runtime's memcpy took half again as long into the write-combined draw
    // data (game G's frame by sections: the constants' upload 1,230 to 870
    // ticks a draw, the system constants' 510 to 360), by its wider or string
    // stores, which that memory takes worse than plain ones filling each line
    // in order.
    static inline const bool upload_stores_ = [] {
        const char *v = std::getenv("XF_UPLOAD_STORES");
        return !(v && v[0] == '0');
    }();
    [[gnu::noinline]] static void upload(void *to, const void *from, size_t bytes) {
#if defined(__SSE2__)
        if (upload_stores_) {
            uint8_t *d = static_cast<uint8_t *>(to);
            const uint8_t *f = static_cast<const uint8_t *>(from);
            size_t k = 0;
            for (; k + 16 <= bytes; k += 16) {
                const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(f + k));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(d + k), v);
            }
            for (; k + 4 <= bytes; k += 4) {
                uint32_t w;
                std::memcpy(&w, f + k, 4);
                std::memcpy(d + k, &w, 4);
            }
            if (k < bytes) std::memcpy(d + k, f + k, bytes - k);
            return;
        }
#endif
        std::memcpy(to, from, bytes);
    }
    uint8_t *draw_data(VkDeviceSize at) {
        return static_cast<uint8_t *>((draw_data_.buffer ? draw_data_ : arena_).mapped) + at;
    }
    VkBuffer draw_buffer() const { return draw_data_.buffer ? draw_data_.buffer : arena_.buffer; }
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
        uint64_t used_serial = 0; // the submission its pages were last marked used in
    };
    VkDeviceSize request(const std::vector<Range> &ranges, RequestMemo *memo = nullptr);
    RequestMemo vertex_memo_;
    // A draw's vertex ranges, each as it was last found in the mirror: the
    // write count before that look. A range no page of which was written
    // since (by blocks of pages: pages_unwritten) and under which the mirror
    // lost nothing (mirror_epoch_) is in the mirror still, so a draw asks
    // only for the others: a scene's draws read whole vertex buffers of
    // hundreds of pages, each page looked at twice a draw otherwise.
    // XF_VERTEX_RANGE_MEMO=0: every range asked for at every draw, as before.
    struct RangeMemo {
        Range range{0, 0};
        uint32_t count = 0;
        uint64_t epoch = 0, used_serial = 0;
    };
    static constexpr size_t RANGE_MEMOS = 1u << 12;
    std::vector<RangeMemo> range_memos_ = std::vector<RangeMemo>(RANGE_MEMOS);
    static size_t range_memo_index(const Range &r) {
        const uint64_t h = (uint64_t(r.first) << 32 | r.second) * 0x9E3779B97F4A7C15ull;
        return size_t(h >> 52) & (RANGE_MEMOS - 1);
    }
    bool range_memos_on_ = [] {
        const char *v = std::getenv("XF_VERTEX_RANGE_MEMO");
        return !(v && v[0] == '0');
    }();
    uint64_t mirror_epoch_ = 1; // advanced where pages of the mirror stop being current but for a write
    void gpu_written(const std::vector<Range> &ranges);
    // The submission that last used each page of the mirror (read or wrote
    // it on the GPU, or uploaded it): an upload of pages the recording
    // submission has not used yet goes into its slot's pre command buffer,
    // ahead of everything in the submission, rather than between its draws,
    // which would end the render pass and drain the GPU (beyond Xenia, whose
    // uploads go between its draws). XF_HOISTED_UPLOADS=0 turns it off.
    std::vector<uint64_t> page_used_;
    bool hoisted_uploads_ = [] {
        const char *v = std::getenv("XF_HOISTED_UPLOADS");
        return !(v && std::string(v) == "0");
    }();
    void mark_used(const std::vector<Range> &ranges);
    VkCommandBuffer pre_cmd();
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
    // the texture filtering forced (Options::anisotropy, texture_filter; set_texture_filtering)
    std::atomic<uint32_t> anisotropy_{0}, texture_filter_{0};
    // The resolution scale everything drawn into EDRAM is drawn at (docs/
    // renderer.md, "Resolution scale"), the minimum scalable dimension, and
    // why the scale is not the one asked for. EDRAM's tiles are scale_'s
    // size: every render target is drawn at it (they share EDRAM's tiles
    // whatever their sizes, as the console's do); a resolve or a texture
    // smaller than min_scalable_ leaves EDRAM at the console's size.
    Scale scale_{};
    uint32_t min_scalable_ = 16;
    // At a resolution scale, a draw whose target is at most this many console
    // pixels across or down (the surface's pitch, the window scissor) reads
    // its interpolators at the console pixel's center (shader_translator.cpp,
    // bit 16 of the system flags' y): small targets are tables and
    // reductions, whose shaders index them by position and expect the
    // console's centers. XF_CONSOLE_CENTERS=<pixels> sets it, 0 turns it off.
    uint32_t console_centers_ = [] {
        const char *v = std::getenv("XF_CONSOLE_CENTERS");
        return v && *v ? uint32_t(std::atoi(v)) : 16u;
    }();
    // A texture loaded at the scale (from a scaled resolve target or scaled
    // memory): no smaller than the minimum scalable dimension, and, with
    // console_centers_, wider and taller than it: a table such draws made is
    // read at the console's size, from the pixels a resolve takes (the
    // console pixels' centers). Its other host pixels hold the same values
    // only to the interpolators' rounding, which at a scale that is not a
    // power of two can step a position-indexed table across an index.
    bool texture_scalable(uint32_t width, uint32_t height) const {
        return scalable(width, height, min_scalable_) &&
               !(console_centers_ && (width <= console_centers_ || height <= console_centers_));
    }
    // 64-bit resolve targets made the scale's too (XF_SCALE_WIDE=0: those stay
    // the console's size)
    bool scale_wide_ = [] {
        const char *v = std::getenv("XF_SCALE_WIDE");
        return !(v && v[0] == '0');
    }();
    std::string scale_notice_;
    // EDRAM's tiles and dwords at scale_
    uint32_t tile_dwords_ = 1280;
    uint32_t max_image_ = 16384; // the device's largest 2D image
    VkDeviceSize edram_bytes_ = 0;
    mutable CallLock mutex_; // held through each of the processor's calls
    Report report_;
    std::string fatal_;
    const uint32_t *regs_ = nullptr; // the processor's register file, during a call
    const gpu::RegisterFile *file_ = nullptr; // the same, for what changed in it (RegisterFile's stamps)
    GuestMemory memory_;             // the processor's physical memory
    Memory &mem_;                    // the same memory, with its write watches
    const std::atomic<uint32_t> *stamps_ = nullptr, *block_stamps_ = nullptr;
    // whether no page of [first, last] was written above since: by blocks
    // of pages first (memory.h's write_block_stamps), the pages of a block only
    // where the block was
    bool pages_unwritten(uint32_t first, uint32_t last, uint32_t since) const {
        constexpr uint32_t shift = Memory::WRITE_BLOCK_SHIFT;
        for (uint32_t p = first; p <= last;) {
            const uint32_t block = p >> shift, block_end = std::min(last, ((block + 1) << shift) - 1);
            if (block_stamps_[block].load(std::memory_order_acquire) > since)
                for (uint32_t q = p; q <= block_end; q++)
                    if (stamps_[q].load(std::memory_order_acquire) > since) return false;
            p = block_end + 1;
        }
        return true;
    }
    uint32_t page_shift_ = 12; // the watch pages'
    std::vector<uint32_t> mirror_at_;
    std::unique_ptr<std::atomic<uint64_t>[]> gpu_pages_;
    void *watcher_ = nullptr;
    gpu::MemoryTrace *trace_ = nullptr; // a trace writer's, told what is read
    uint64_t vertex_hash_ = 0, pixel_hash_ = 0;
    bool have_vertex_ = false, have_pixel_ = false;
    // the loaded shaders themselves (shaders_ never drops one), so a draw looks neither up by hash
    Shader *vertex_shader_ = nullptr, *pixel_shader_ = nullptr;
    float max_viewport_[2] = {};
    VkDeviceSize ubo_align_ = 256, ssbo_align_ = 64;

    std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
    // IM_LOAD's shaders by type, size and address (shader_again): the
    // shader's hash and the write count its words were last found unwritten at
    struct ShaderAt {
        uint64_t hash = 0;
        uint32_t checked = 0;
        Shader *shader = nullptr;
    };
    std::unordered_map<uint64_t, ShaderAt> shaders_at_;
    static uint64_t shader_at(uint32_t type, uint32_t address, uint32_t dwords) {
        return uint64_t(type) << 63 | uint64_t(dwords & 0xFFFF) << 32 | address;
    }
    // The draw's lists, kept from draw to draw for their room: a heavy
    // scene's 3,000 draws a frame made and freed each of them (a tenth of
    // the command thread in the heap). Each is cleared where draw() starts
    // filling it; the swaps (not moves) between them keep every buffer.
    struct DrawScratch {
        std::vector<Range> exported, vertex_ranges, sorted, written;
        std::vector<uint32_t> indices, triangles, converted, guest_indices, strips, key, color_key;
        std::vector<uint8_t> raw_indices;
    } draw_lists_;
    uint64_t shader_at_key_ = 0;  // the load shader_again let through, for shader()
    uint32_t shader_at_count_ = 0; // the write count taken before its copy
    bool shader_at_open_ = false;
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
    SamplerEntry *last_sampler_ = nullptr; // the one sampler() just returned (null: the stand-in's)
    // What a texture slot gave the last draw that read it (Impl::draw): the fetch constant's
    // words and the filters it was made from, and the system constants' words made of them. Good
    // while texture_epoch_ stands: it moves when a texture is loaded or loaded again, a place in
    // the bindless tables is given back (a view or a texture destroyed) or a sampler is destroyed.
    // XF_TEXTURE_SLOT_MEMO=0: every slot looked up at every draw, as before.
    struct SlotMemo {
        uint64_t epoch = 0; // 0: none
        uint32_t fetch[6] = {};
        std::array<uint32_t, 4> overrides = {};
        uint32_t forced_filter = 0, forced_aniso = 0;
        Texture *texture = nullptr;
        uint64_t loads = 0; // the texture's, when this was kept
        SamplerEntry *sampler = nullptr;
        uint32_t slot_word = 0, signs = 0, integer_scale = 0, signed_word = 0;
        uint32_t uses = 0; // the Report::used counts a draw adds for it (bit 0, bit 1)
        uint64_t used = 0; // the draw that last found it (the two ways' replacement)
    };
    // by the constant's content rather than the slot: a table any slot finds what any slot made
    // of the same constant and filters in (a game binding other textures at every draw rarely
    // gives a slot the same constant twice running). 16,384 entries, two ways a set, the one a
    // draw found less lately replaced, the hash's bits mixed: direct-mapped, a scene's thousand
    // constants in 4,096 entries collided often enough that one slot in fifty was looked up the
    // long way, each a few microseconds (XF_SLOT_MEMO_WAYS=1: 4,096 direct-mapped, the hash as
    // before).
    static constexpr uint32_t FETCH_MEMOS = 16384;
    std::unique_ptr<SlotMemo[]> fetch_memos_ = std::make_unique<SlotMemo[]>(FETCH_MEMOS);
    bool slot_memo_two_ways_ = [] {
        const char *v = std::getenv("XF_SLOT_MEMO_WAYS");
        return !(v && v[0] == '1');
    }();
    uint32_t fetch_memo_index(const uint32_t *fetch, const std::array<uint32_t, 4> &overrides) const {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (uint32_t k = 0; k < 6; k++) h = (h ^ fetch[k]) * 0x100000001B3ull;
        for (uint32_t o : overrides) h = (h ^ o) * 0x100000001B3ull;
        if (!slot_memo_two_ways_) return uint32_t(h >> 40) & 4095; // (the table as it was: 4,096 entries)
        h ^= h >> 31, h *= 0xBF58476D1CE4E5B9ull, h ^= h >> 29;
        return uint32_t(h >> 32) & (FETCH_MEMOS - 1);
    }
    // the entry of a slot's constant and filters: the way that holds them, else the one to
    // replace (one kept by an older epoch, else the one found less lately)
    SlotMemo *fetch_memo(const uint32_t *fetch, const std::array<uint32_t, 4> &overrides) {
        return fetch_memo(fetch, overrides, fetch_memo_index(fetch, overrides));
    }
    SlotMemo *fetch_memo(const uint32_t *fetch, const std::array<uint32_t, 4> &overrides, uint32_t index) {
        SlotMemo *a = &fetch_memos_[index];
        if (!slot_memo_two_ways_) return a;
        SlotMemo *b = &fetch_memos_[index ^ 1];
        auto holds = [&](const SlotMemo &m) {
            return m.epoch == texture_epoch_ && std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0 &&
                   m.overrides == overrides;
        };
        if (holds(*a)) return a;
        if (holds(*b)) return b;
        if (a->epoch != texture_epoch_) return a;
        if (b->epoch != texture_epoch_) return b;
        return a->used <= b->used ? a : b;
    }
    // The entries a draw's slot memos and vertex range memos will be looked up in, prefetched as
    // the draw begins (XF_DRAW_PREFETCH=0 off): each is in a table of its own too large for the
    // processor's nearer caches, and a draw found them one after another, each a miss.
    void prefetch_draw(const gpu::Draw &d, const Shader *vs, const Shader *ps) {
        if (index_cache_on_ && d.indexed && d.immediate.empty()) {
            // the index cache's entry, by the key the indices' section makes (a strip's reset
            // index as for any draw that is not tessellated: a mismatch only a prefetch lost)
            const uint32_t primitive = d.primitive;
            const bool strip = primitive == 3 || primitive == 5 || primitive == 6 || primitive == 12 ||
                               primitive == 14 || primitive == 15 || primitive == 0x15 || primitive == 0x16;
            const bool restart = strip && (regs_[r::PA_SU_SC_MODE_CNTL] >> 21) & 1;
            IndexKey key;
            key.base = d.index_base & GuestMemory::PHYSICAL_MASK, key.count = d.count;
            const uint32_t converted = primitive >= 12 && primitive <= 15 ? primitive : 0;
            key.form = uint32_t(d.index32) | uint32_t(d.index_endian) << 1 | uint32_t(restart) << 4 | converted << 8;
            key.reset = restart ? regs_[r::VGT_MULTI_PRIM_IB_RESET_INDX] : 0;
            key.offset = regs_[r::VGT_INDX_OFFSET];
            key.min = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, key.max = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
            if (const IndexEntry *e = index_front_[IndexKeyHash()(key) & (INDEX_FRONT - 1)]) __builtin_prefetch(e);
        }
        if (slot_memos_on_ && !trace_) {
            const uint64_t slots = vs->texture_mask | (ps ? ps->texture_mask : 0);
            for (uint64_t left = slots; left; left &= left - 1) {
                const uint32_t slot = uint32_t(std::countr_zero(left));
                const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS + slot * 6];
                std::array<uint32_t, 4> overrides = {3, 3, 3, 7};
                for (const Shader *s : {vs, ps})
                    if (s && slot < 32 && (s->filter_mask & (1u << slot))) {
                        overrides = s->slot_filters[slot];
                        break;
                    }
                const uint32_t index = fetch_memo_index(fetch, overrides);
                __builtin_prefetch(&fetch_memos_[index]);
                __builtin_prefetch(&fetch_memos_[index ^ 1]);
                prefetched_slots_[slot] = {overrides, index};
            }
            prefetched_mask_ = slots, prefetched_vs_ = vs, prefetched_ps_ = ps;
        }
        if (range_memos_on_ && !trace_) {
            const uint64_t fetch_slots[2] = {vs->vertex_mask[0] | (ps ? ps->vertex_mask[0] : 0),
                                             vs->vertex_mask[1] | (ps ? ps->vertex_mask[1] : 0)};
            for (uint32_t half = 0; half < 2; half++)
                for (uint64_t left = fetch_slots[half]; left; left &= left - 1) {
                    const uint32_t slot = half * 64 + uint32_t(std::countr_zero(left));
                    const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 2];
                    const uint32_t start = f[0] & 0x1FFFFFFC, words = (f[1] >> 2) & 0xFFFFFF;
                    const uint32_t end =
                        uint32_t(std::min<uint64_t>(uint64_t(start) + uint64_t(words) * 4, GuestMemory::SIZE));
                    if (end > start) __builtin_prefetch(&range_memos_[range_memo_index({start, end})]);
                }
        }
    }
    // the slots' filters and memo entries the prefetch found, for the textures' section (the
    // registers do not change within a draw)
    struct PrefetchedSlot {
        std::array<uint32_t, 4> overrides;
        uint32_t index;
    };
    PrefetchedSlot prefetched_slots_[64] = {};
    uint64_t prefetched_mask_ = 0;
    const Shader *prefetched_vs_ = nullptr, *prefetched_ps_ = nullptr; // (the shaders the filters are of)
    bool draw_prefetch_ = [] {
        const char *v = std::getenv("XF_DRAW_PREFETCH");
        return !(v && v[0] == '0');
    }();
    uint64_t texture_epoch_ = 1;
    uint64_t approximations_ = 0; // approximate()'s calls
    bool slot_memos_on_ = [] {
        const char *v = std::getenv("XF_TEXTURE_SLOT_MEMO");
        return !(v && v[0] == '0');
    }();
    std::vector<uint32_t> free_samplers_; // slots of samplers destroyed, to reuse
    std::map<std::vector<uint32_t>, std::unique_ptr<PipelineEntry>> pipelines_;
    // the generic pipelines of draws whose shaders are being translated, by
    // the pipeline's key (dropped when the draw next goes the usual way)
    std::map<std::vector<uint32_t>, std::unique_ptr<PipelineEntry>> translating_pipelines_;
    // the entries pipeline() found last, by their keys' hashes, each with its key
    struct PipelineFront {
        std::vector<uint32_t> key;
        PipelineEntry *entry = nullptr;
    };
    static constexpr size_t PIPELINE_FRONT = 1u << 10;
    std::vector<PipelineFront> pipeline_front_ = std::vector<PipelineFront>(PIPELINE_FRONT);

    // submissions
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Slot slots_[SLOTS];
    uint32_t current_ = 0;
    uint64_t next_serial_ = 1, done_serial_ = 0;
    bool in_pass_ = false;
    Pass pass_;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    // the draws' dynamic state the recording command buffer holds (invalid
    // after a new command buffer or another pipeline of the renderer's;
    // XF_VK_DRAW_STATE_ALWAYS=1 sets all of it at every draw)
    DrawState draw_state_{};
    bool draw_state_valid_ = false, draw_state_elision_ = true;
    // the last constants and system constants put in the arena, and where
    harness::Constants last_constants_[2];
    // The constants kept from draw to draw: last_constants_ are the registers' as of the register
    // file's serial constants_serial_ (and its count of other threads' writes), so a draw copies
    // only the blocks written to another value since, and uploads a stage's again only then. The
    // fetch constants count for a stage only in the blocks its shader's slots lie in (a draw's new
    // vertex buffers leave the pixel shader's constants as they were). XF_CONSTANT_STAMPS=0: copied
    // and compared whole at every draw, as before.
    const gpu::RegisterFile *constants_file_[2] = {};
    uint64_t constants_serial_[2] = {}, constants_foreign_[2] = {};
    uint32_t constants_fetch_blocks_[2] = {}; // the fetch constants' blocks current as of the serial
    bool constant_stamps_ = [] {
        const char *v = std::getenv("XF_CONSTANT_STAMPS");
        return !(v && v[0] == '0');
    }();
    System last_system_{};
    VkDeviceSize last_constants_at_[2] = {NONE, NONE}, last_system_at_ = NONE;
    uint64_t constants_dirty_[2] = {}; // (XF_DRAW_SECTIONS: the blocks written since a stage's last upload)
    // the blocks uploaded there: the float constants' in bits 0-31 (Shader::float_blocks), the
    // fetch constants' in bits 32-37 (Shader::fetch_blocks)
    uint64_t last_constants_blocks_[2] = {};
    uint32_t last_system_slots_ = 0;         // how many slots of the system constants' tables are uploaded there
    bool constant_blocks_ = [] {
        const char *v = std::getenv("XF_CONSTANT_BLOCKS");
        return !(v && v[0] == '0');
    }();
    // and of the fetch constants only the blocks the stage's shader fetches from: 768 of a
    // stage's 928 bytes past the float constants went up at every upload for the one or two
    // blocks a shader reads (XF_FETCH_BLOCKS=0: all six)
    bool fetch_blocks_ = [] {
        const char *v = std::getenv("XF_FETCH_BLOCKS");
        return !(v && v[0] == '0');
    }();

    // ---- a draw's front part ----
    // What a draw works out from the registers of the draws' state alone
    // (RegisterFile::state_serial), its shaders and its kind of primitive:
    // the targets, the viewport and scissor, how the primitive is drawn, the
    // interlock path's constants. A draw that comes with all of those as the
    // draw before had them takes that draw's (Cemu's continued draws, RPCS3's
    // and Dolphin's dirty state: a scene's draws mostly change constants,
    // buffers and textures between them). XF_DRAW_FRONT_MEMO=0: worked out at
    // every draw, as before.
    struct DrawFront {
        // what it is worked out from, beside the registers
        uint32_t primitive = 0, count = 0, extent_words = UINT32_MAX;
        bool index32 = false, color = false, writes_targets = false;
        // what the draw takes from it
        bool tessellated{};
        uint32_t tess_mode{};
        shader::VertexOptions host_vertex{};
        uint32_t patch_points{};
        uint32_t hull{};
        std::string draw_name{};
        bool rectangles{};
        bool points{};
        uint32_t ucp{};
        bool expanded{};
        bool polygonal{};
        VkPrimitiveTopology topology{};
        bool strip{};
        Shader *vs = nullptr;
        Shader *ps = nullptr;
        std::string why{};
        bool translating{};
        bool first_forms_translating{};
        uint32_t pitch{};
        uint32_t msaa{};
        bool export_only{};
        uint32_t gamma_targets{};
        uint32_t depth_control{};
        RenderTarget *depth = nullptr;
        uint32_t width{};
        uint32_t height{};
        uint32_t host_width{};
        uint32_t host_height{};
        bool depth_float24{};
        bool float24_convert{};
        Viewport viewport{};
        VkRect2D scissor{};
        FsiConstants fsi_constants{};
        RenderTarget *colors[4] = {};
        uint32_t color_masks[4] = {};
        float color_scales[4] = {};
    };
    DrawFront front_;
    // the draw the kept one was worked out for
    struct DrawFrontKey {
        uint64_t state_serial = 0, foreign_writes = 0;
        const Shader *vertex = nullptr, *pixel = nullptr;
        uint32_t primitive = 0, flags = 0;
        bool operator==(const DrawFrontKey &) const = default;
    } front_key_;
    bool front_valid_ = false;
    // what the front part counted (use, approximate): counted again by a draw that takes it
    std::vector<std::pair<const char *, bool>> front_counts_;
    bool front_recording_ = false, front_keepable_ = false;
    bool front_memo_ = [] {
        const char *v = std::getenv("XF_DRAW_FRONT_MEMO");
        return !(v && v[0] == '0');
    }();
    // false: the draw ends there (skipped, or it writes nothing)
    bool draw_front(const gpu::Draw &d, DrawFront &f);
    // the front part worked out again (each a new one: what was kept with the last stands no more)
    uint64_t front_generation_ = 0;
    // The pipeline and what the draw takes of its description, kept for the
    // next draw whose front part is the same one (the key, the description
    // and the lookup worked out from the registers the front part's draw
    // state covers, its shaders and its primitive); XF_PIPELINE_MEMO=0 off.
    struct PipelineMemo {
        bool valid = false;
        uint64_t front_generation = 0;
        std::vector<std::pair<const char *, bool>> counts; // (use, approximate), counted again
        VkPipelineColorBlendAttachmentState blends[4] = {};
        VkPipelineRasterizationStateCreateInfo raster{};
        VkPipelineDepthStencilStateCreateInfo depth_state{};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkShaderModule fragment = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        bool premultiplied = false;
        uint32_t premultiply[4] = {};
        uint32_t fsi_unordered = 0;
    } pipeline_memo_;
    std::vector<std::pair<const char *, bool>> pipeline_counts_;
    bool pipeline_recording_ = false, pipeline_keepable_ = false;
    // the last pipeline() found its pipeline made and final (its front table's entry)
    bool pipeline_final_ = false;
    bool pipeline_memo_on_ = [] {
        const char *v = std::getenv("XF_PIPELINE_MEMO");
        return !(v && v[0] == '0');
    }();

    // ---- the index cache ----
    // A draw's converted indices kept by where the guest's are and how they
    // were converted (Xenia's primitive processor keeps its converted index
    // buffers the same way), in buffers of their own: static geometry's are
    // converted and uploaded once, not at every draw. The guest's pages are
    // watched as a texture's are; pages written since make the draw convert
    // again, and the entry stays where the words came out the same (a write
    // elsewhere in its pages). Indices written again and again (dynamic
    // geometry) leave the cache for a while, and are converted into the
    // arena as before. XF_INDEX_CACHE=0: every draw's converted, as before.
    struct IndexKey {
        uint32_t base = 0, count = 0, form = 0, reset = 0, offset = 0, min = 0, max = 0;
        bool operator==(const IndexKey &) const = default;
    };
    struct IndexKeyHash {
        size_t operator()(const IndexKey &k) const {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (uint32_t w : {k.base, k.count, k.form, k.reset, k.offset, k.min, k.max}) h = (h ^ w) * 0x100000001B3ull;
            return size_t(h ^ (h >> 29));
        }
    };
    struct IndexEntry {
        std::vector<uint32_t> words; // as the draw binds them (after the primitive's conversion)
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize at = 0;
        uint32_t checked = 0;       // the write count the guest's pages were last known unwritten at
        uint32_t first = 0, last = 0; // its watch pages
        uint64_t seen_swap = 0;     // the swap it was last drawn in, plus 1
        uint64_t written_swap = 0;  // the swap its pages were last found written in
        uint32_t streak = 0;        // how many times in a row within a few swaps
        uint64_t dynamic_until = 0; // not kept before this swap
        IndexKey key;
    };
    // the entries looked up last, by their keys' hashes: a draw finds its
    // own without walking the map (a node of it in memory the cache has not)
    static constexpr size_t INDEX_FRONT = 1u << 12;
    std::vector<IndexEntry *> index_front_ = std::vector<IndexEntry *>(INDEX_FRONT, nullptr);
    struct IndexChunk {
        vk::Buffer buffer;
        VkDeviceSize used = 0;
    };
    static constexpr VkDeviceSize INDEX_CHUNK = 8u << 20, INDEX_CACHE_BUDGET = 128u << 20;
    static constexpr size_t INDEX_CACHE_ENTRIES = 1u << 17;
    std::unordered_map<IndexKey, IndexEntry, IndexKeyHash> index_cache_;
    std::vector<IndexChunk> index_chunks_;
    VkDeviceSize index_cache_words_ = 0; // the entries' host copies, in bytes
    bool index_cache_on_ = [] {
        const char *v = std::getenv("XF_INDEX_CACHE");
        return !(v && v[0] == '0');
    }();
    // room for an entry's words in the cache's buffers; false when none can be made
    bool index_cache_place(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &at);
    // everything dropped: the buffers go once the submission being recorded is done
    void index_cache_drop();
    // the index buffer the recording command buffer has bound at offset 0
    VkBuffer bound_index_buffer_ = VK_NULL_HANDLE;
    uint64_t draw_skip_[2] = {UINT64_MAX, 0};
    bool draw_skip_read_ = [this] {
        const char *v = std::getenv("XF_DRAW_SKIP");
        unsigned long long first = 0, last = 0;
        if (v && std::sscanf(v, "%llu-%llu", &first, &last) == 2) draw_skip_[0] = first, draw_skip_[1] = last;
        return true;
    }();
    uint64_t draw_limit_ = [] {
        const char *v = std::getenv("XF_DRAW_LIMIT");
        return v ? uint64_t(std::strtoull(v, nullptr, 10)) : uint64_t(0);
    }();
    float rounded_scale_in_ = 0.0f, rounded_scale_out_ = 0.0f; // the polygon offset scale last rounded
    // whether it has the draws' two sets bound, and the first's dynamic
    // offsets (XF_SET_ELISION=0: both bound at every draw, as before)
    bool sets_bound_ = false;
    uint32_t bound_offsets_[4] = {};
    bool set_elision_ = [] {
        const char *v = std::getenv("XF_SET_ELISION");
        return !(v && v[0] == '0');
    }();

    // ---- the draws' commands, and the recording thread ----
    // A draw's commands (its binds, the dynamic state it changes, the draw)
    // are put as words: a command's number, then what it takes (run_recorded
    // records them). Recorded at once, or, with the recording thread
    // (XF_RECORD_RING=1, record_ring.h), put whole into its ring, where
    // everything else the renderer records goes too: the command buffers the
    // renderer records through are then the ring's handles (rec::Fake), and
    // the thread that makes the commands never calls the driver to record.
    // (XF_RECORD_THREAD, a thread for the draws' commands alone, waited for
    // the ring to empty at every other command: about 100 waits a frame, and
    // it lost; docs/renderer.md, "Game G's command thread".)
    struct Recorded {
        enum Op : uint32_t {
            BindPipeline, BindSets, BindIndexBuffer, BindVertexBuffer, Viewport, Scissor, BlendConstants,
            StencilReference, StencilCompareMask, StencilWriteMask, Topology, RestartEnable, CullMode, FrontFace,
            DepthTest, DepthWrite, DepthCompare, StencilTest, StencilOp, DepthBiasEnable, BlendEnables, BlendEquations,
            WriteMasks, LineWidth, DepthBias, DepthBiasExact, BeginConditional, EndConditional, Draw, DrawIndexed,
        };
        struct Sets {
            uint32_t count;
            uint32_t offsets[4];
            VkDescriptorSet sets[2];
        };
        struct Bias {
            uint32_t host_float;
            float constant, slope;
        };
        struct Place {
            VkBuffer buffer;
            VkDeviceSize at;
        };
    };
    static constexpr uint32_t RECORDED_MOST = 32; // words of a command, with its number
    uint32_t recorded_direct_[RECORDED_MOST] = {};
    VkCommandBuffer record_direct_cmd_ = VK_NULL_HANDLE;
    std::unique_ptr<rec::Ring> ring_; // the recording thread's ring (XF_RECORD_RING=0 off)
    bool ring_on_ = [] {
        const char *v = std::getenv("XF_RECORD_RING");
        return !(v && v[0] == '0');
    }();
    // the serial of the last submission the recording thread made, and its error
    std::atomic<uint64_t> ring_submitted_{0};
    std::atomic<int32_t> ring_submit_error_{VK_SUCCESS};
    uint64_t flushed_serial_ = 0; // the last submission's serial
    // a command put in the ring: the renderer and the command's words
    static void run_recorded_entry(const void *payload, VkCommandBuffer real) {
        Impl *self;
        std::memcpy(&self, payload, sizeof(self));
        self->run_recorded(real, reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(payload) + 8));
    }
    // the next command's words to fill, then recorded(): into the ring, or recorded at once
    uint32_t *record(VkCommandBuffer c, Recorded::Op op, uint32_t words) {
        if (rec::Fake *f = rec::fake_of(c); f && !f->ring->direct()) {
            uint8_t *p = static_cast<uint8_t *>(f->ring->reserve(f->target, 8 + 4 * (words + 1), &run_recorded_entry));
            Impl *self = this;
            std::memcpy(p, &self, sizeof(self));
            uint32_t *r = reinterpret_cast<uint32_t *>(p + 8);
            r[0] = op;
            record_direct_cmd_ = VK_NULL_HANDLE;
            return r + 1;
        } else if (f) {
            c = f->ring->real(f->target);
        }
        recorded_direct_[0] = op;
        record_direct_cmd_ = c;
        return &recorded_direct_[1];
    }
    void recorded() {
        if (!record_direct_cmd_) {
            ring_->commit();
            return;
        }
        run_recorded(record_direct_cmd_, recorded_direct_);
    }
    // the commands put so far handed to the recording thread, by batches (Ring::publish_some)
    void record_publish() {
        if (ring_) ring_->publish_some();
    }
    template <class T> void record_value(VkCommandBuffer c, Recorded::Op op, const T &value) {
        static_assert(sizeof(T) / 4 + 2 <= RECORDED_MOST);
        std::memcpy(record(c, op, uint32_t(sizeof(T) + 3) / 4), &value, sizeof(T));
        recorded();
    }
    template <class... A> void record_words(VkCommandBuffer c, Recorded::Op op, A... a) {
        const uint32_t words[sizeof...(a) + 1] = {uint32_t(a)..., 0u};
        std::memcpy(record(c, op, uint32_t(sizeof...(a))), words, sizeof...(a) * 4);
        recorded();
    }
    // a command's words (after its number) into the command buffer; how many it took
    uint32_t run_recorded(VkCommandBuffer c, const uint32_t *r);
    // a command buffer being recorded, begun if none is
    void begin_recording();

    vk::Buffer mirror_, arena_, scratch_, load_scratch_;
    // the draw-data arena (draw_data_take): a share a slot, in device-local
    // host-visible memory; none without such memory or with
    // XF_VK_NO_DEVICE_LOCAL_DRAW_DATA=1
    vk::Buffer draw_data_;
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
    // the host path's resolves from the source target's image (resolve.comp
    // with IMAGE, the target's store set as set 2), 1x and multisampled;
    // XF_RESOLVE_THROUGH_EDRAM=1: stored into EDRAM and read from there, as before
    VkPipelineLayout resolve_image_layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_image_pipelines_[2] = {};
    bool resolve_through_edram_ = [] {
        const char *v = std::getenv("XF_RESOLVE_THROUGH_EDRAM");
        return v && v[0] == '1';
    }();
    // resolves' clears kept for the target's next render pass
    // (RenderTarget::pending_clears); XF_DEFER_CLEARS=0: drawn at once
    bool defer_clears_ = [] {
        const char *v = std::getenv("XF_DEFER_CLEARS");
        return !(v && v[0] == '0');
    }();
    void flush_clears(RenderTarget *t);
    // Xenia's texture_point_snap, off as there (shader::VertexOptions::
    // texture_point_snap): XF_TEXTURE_POINT_SNAP=1
    bool point_snap_ = [] {
        const char *v = std::getenv("XF_TEXTURE_POINT_SNAP");
        return v && v[0] == '1';
    }();
    // the console's vertex snapping in the vertex shaders (Options::vertex_snap, or XF_VERTEX_SNAP=1)
    bool vertex_snap_ = [this] {
        const char *v = std::getenv("XF_VERTEX_SNAP");
        return options_.vertex_snap || (v && v[0] == '1');
    }();
    // the screen extents' maxima a subgroup at a time (shader::VertexOptions::extents_subgroup)
    // where the device has subgroup arithmetic in vertex shaders; XF_EXTENTS_SUBGROUP=0 off
    bool extents_subgroup_ = [] {
        const char *v = std::getenv("XF_EXTENTS_SUBGROUP");
        return !(v && v[0] == '0');
    }();
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
    void acquire(RenderTarget *t, uint32_t rows, const DepthOverwrite *overwritten = nullptr);
    void acquire(RenderTarget *t, std::span<const TileRect> rects, const DepthOverwrite *overwritten = nullptr);
    std::vector<uint32_t> acquire_tiles_; // (acquire's own, kept for its room)
    std::vector<uint32_t> acquire_needed_, acquire_unheld_;
    // (request's own, kept for their room)
    std::vector<std::pair<uint32_t, uint32_t>> request_runs_;
    std::vector<VkBufferCopy> request_copies_, request_ahead_;
    bool transfer_cleared_only_ = false;  // the last edram_transfer drew nothing: clears alone (the verbose line)
    bool resolve_stored_ = false;         // the resolve being made reads tiles their holders stored into EDRAM
    bool edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles);
    bool edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows);
    // tiles drawn straight from their holders into the target, one pass
    // (edram_transfer.frag, Xenia's ownership transfers)
    bool edram_transfer(RenderTarget *t, const std::map<RenderTarget *, std::vector<uint32_t>> &held);
    VkPipeline transfer_pipeline(const RenderTarget &t, bool source_ms);
    // Direct transfers, the default; XF_EDRAM_TRANSFERS=store moves the
    // tiles through the EDRAM buffer, a store by the holder and a load by the
    // new target, as before
    bool direct_transfers_ = [] {
        const char *v = std::getenv("XF_EDRAM_TRANSFERS");
        return !(v && std::string(v) == "store");
    }();
    // [single-sampled source, multisampled][color, UINT color, depth, depth
    // and stencil exported, depth and the stencil into an R8_UINT image]
    VkShaderModule transfer_fs_[2][5] = {};
    // the stencil of transfers into depth targets copied from an R8_UINT
    // image (no stencil export, VK_KHR_maintenance8)
    bool stencil_copies_ = false;
    std::map<std::tuple<VkFormat, uint32_t, bool>, VkPipeline> transfer_pipelines_;
    VkDeviceSize edram_mask(const std::vector<uint32_t> &tiles);
    EdramTarget edram_target(const RenderTarget &t, uint32_t height) const {
        // (20e4 rounded only where the host's depth is not already 20e4, as
        // Xenia's dump of float24 depth)
        bool round = options_.depth_float24_round && !options_.depth_float24_convert_in_pixel_shader;
        uint32_t flags = t.depth ? (host_depth_kept(t.format) ? 1u : 0u) | (round ? 2u : 0u)
                                 : (t.gamma_linear ? 4u : 0u) | (t.raw ? 8u : 0u);
        return {t.base,  t.pitch_tiles,     t.msaa,           t.format, t.host_width,
                height,  t.depth ? 1u : 0u, t.wide ? 1u : 0u, 8u,       flags,
                0,       0,                 0,                0,        scale_.q4};
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
    VkPipeline edram_pipeline(const RenderTarget &t, VkShaderModule fs, VkPipelineLayout layout, bool stencil_copy);
    VkDescriptorSet edram_set(uint32_t which);
    std::vector<RenderTarget *> owners_ = std::vector<RenderTarget *>(EDRAM_TILES, nullptr); // null: EDRAM holds it
    // Beyond Xenia: a tile whose holder's pixels are all one clear's value
    // (a resolve's clear covered the whole tile, and no draw has reached it
    // since). A target of the same format taking it over is cleared to that
    // value there instead of drawn from the holder (edram_transfer): the same
    // pixels, without reading the holder. XF_TRANSFER_CLEARS=0: always drawn.
    std::vector<uint8_t> tile_cleared_ = std::vector<uint8_t>(EDRAM_TILES, 0);
    std::vector<VkClearValue> tile_clear_value_ = std::vector<VkClearValue>(EDRAM_TILES);
    bool transfer_clears_ = [] {
        const char *v = std::getenv("XF_TRANSFER_CLEARS");
        return !(v && std::string(v) == "0");
    }();
    // Beyond Xenia too, for the console's own clears (a rectangle drawn over
    // a depth target with the depth test always passing and written), which
    // games also make through another view of the same tiles (a 4x target
    // cleared as a single-sampled one twice as wide and tall):
    // - a target taking tiles for a draw that provably writes every sample
    //   of them whatever they held (depth_overwrite) takes them without the
    //   transfer from their holder, or the load from EDRAM, the draw would
    //   only overwrite. XF_OVERWRITE_TRANSFERS=0: transferred, as Xenia does.
    // - where such a draw writes one known word, the tiles it covers whole
    //   hold a clear's value as after a resolve's clear, and a depth target of
    //   the other depth format taking a cleared tile is cleared to the same
    //   guest word in its own format (tile_clear_word_). XF_DRAW_CLEARS=0:
    //   a draw never makes a tile a clear's, and the other format is drawn.
    std::vector<uint32_t> tile_clear_word_ = std::vector<uint32_t>(EDRAM_TILES, 0); // depth tiles: the guest's word
    bool overwrite_transfers_ = [] {
        const char *v = std::getenv("XF_OVERWRITE_TRANSFERS");
        return !(v && std::string(v) == "0");
    }();
    bool draw_clears_ = [] {
        const char *v = std::getenv("XF_DRAW_CLEARS");
        return !(v && std::string(v) == "0");
    }();
    // whether all of a tile's pixels, as the target lays them out, are in
    // the target's image and among those the draw overwrites
    static bool tile_within(const RenderTarget &t, uint32_t tile, const DepthOverwrite &o) {
        const uint32_t k = (tile + EDRAM_TILES - t.base % EDRAM_TILES) % EDRAM_TILES;
        const uint32_t x = (k % t.pitch_tiles) * t.tile_width, y = (k / t.pitch_tiles) * t.tile_height;
        const uint32_t x1 = x + t.tile_width, y1 = y + t.tile_height;
        return o.depth_whole && x1 <= t.width && y1 <= t.height && x >= o.x0 && y >= o.y0 && x1 <= o.x1 && y1 <= o.y1;
    }
    // A depth target's tile whose samples all have one stencil (-1: not
    // known to): set by what makes a tile a clear's and by a draw that
    // replaces the stencil of all of it, kept by draws with the stencil test
    // off and by transfers between depth targets (the guest word's low
    // byte whatever the depth format and the sample count), dropped by
    // anything else that writes the tile. With it a clear of the depth alone
    // (the stencil test off: the console's depth-only clears) makes a tile a
    // clear's again: the depth's word with the stencil it has. Part of
    // XF_DRAW_CLEARS.
    std::vector<int16_t> tile_stencil_ = std::vector<int16_t>(EDRAM_TILES, -1);
    uint64_t stencil_epoch_ = 1; // counted up as a tile's stencil becomes known (RenderTarget::stencil_written's memo)
    // a draw with the stencil test on reached these tiles of t
    void tiles_stencil_written(RenderTarget &t, const TileRect &tiles) {
        if (tiles.empty()) return;
        const bool known = t.stencil_epoch == stencil_epoch_;
        if (known && tiles.within(t.stencil_written)) return;
        for (uint32_t row = tiles.r0; row < tiles.r1; row++)
            for (uint32_t column = tiles.c0; column < tiles.c1; column++)
                tile_stencil_[(t.base + row * t.pitch_tiles + column) % EDRAM_TILES] = -1;
        if (!known) t.stencil_epoch = stencil_epoch_, t.stencil_written = {};
        t.stencil_written.remember(tiles);
    }
    // the tiles of t in its pixel rows [top, bottom), every column: a draw
    // may have written them, so they no longer hold a clear's value
    // (A draw after another into the same rows, no tile made a clear's
    // since, has nothing to do: a game's hundred thousand draws a second
    // walked 720 tiles each.)
    void tiles_drawn(RenderTarget &t, const TileRect &tiles) {
        if (tiles.empty()) return;
        const bool known = t.drawn_epoch == cleared_epoch_;
        if (known && tiles.within(t.drawn)) return;
        for (uint32_t row = tiles.r0; row < tiles.r1; row++)
            for (uint32_t column = tiles.c0; column < tiles.c1; column++)
                tile_cleared_[(t.base + row * t.pitch_tiles + column) % EDRAM_TILES] = 0;
        if (!known) t.drawn_epoch = cleared_epoch_, t.drawn = {};
        t.drawn.remember(tiles);
    }
    // The tiles of a target that hold the console's pixels [x0, x1) x [y0, y1)
    // (within its pitch and its height).
    static TileRect tiles_of(const RenderTarget &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        TileRect r;
        r.c0 = x0 / t.tile_width, r.c1 = std::min((x1 + t.tile_width - 1) / t.tile_width, t.pitch_tiles);
        r.r0 = y0 / t.tile_height, r.r1 = std::min((y1 + t.tile_height - 1) / t.tile_height, t.height / t.tile_height);
        return r;
    }
    // Beyond Xenia, which takes whole rows of tiles from the first down to
    // the draw's lowest: a draw's targets take over only the tiles the draw
    // can reach (Impl::draw_reach: the scissor, and the viewport with
    // clipping on or the vertices' extent without), so a target drawn to in
    // part while another holds the rest of the same rows (a shadow atlas
    // cleared a region at a time, through views of two sample counts) moves
    // that part alone. XF_DRAW_REACH=rows: whole rows from the first, as
    // Xenia.
    bool reach_tiles_ = [] {
        const char *v = std::getenv("XF_DRAW_REACH");
        return !(v && std::string(v) == "rows");
    }();
    // counted up as a tile changes hands (owners_) and as one is made a
    // clear's (tile_cleared_ set): what the targets' memos are as of
    uint64_t owners_epoch_ = 1, cleared_epoch_ = 1;
    // EDRAM, and beside it the host's 32-bit depth of the last D24FS8 target
    // (and D24S8 where the host keeps it as a float) to store each sample,
    // in the same layout (edram.glsl's host_depth_offset)
    vk::Buffer edram_, host_depth_;
    VkDescriptorSetLayout edram_set_layouts_[2] = {}; // store (and transfer), load
    VkPipelineLayout edram_layouts_[3] = {};          // store, load, transfer
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
    // host render targets rasterized at the Xenos's sample positions
    // (Options::xenos_sample_positions, XF_XENOS_SAMPLE_POSITIONS)
    bool xenos_positions_ = false;
    bool xenos_at(uint32_t msaa) const { return xenos_positions_ && msaa != 0; }
    // A multisampled depth image rasterized at the Xenos's positions, before
    // a shader reads it as a texture (an EDRAM store, a transfer out of it, a
    // resolve): taken to the attachment layout and back with its sample
    // locations named (VkSampleLocationsInfoEXT on the barriers, what
    // VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT asks for at
    // each transition). A device that keeps depth as planes per block of
    // pixels works the samples out when the image is read, and without the
    // locations it does so at its standard ones: each sample then reads the
    // surface's depth a part of a pixel away, and a pass testing for equal
    // depth after a transfer out and back fails over whole surfaces. Only
    // once after the image was last an attachment (RenderTarget::samples_unsettled).
    void settle_depth_samples(VkCommandBuffer c, RenderTarget &t) {
        if (!t.samples_unsettled) return;
        t.samples_unsettled = false;
        if (!t.depth || !xenos_at(t.msaa)) return;
        VkSampleLocationEXT positions[4];
        VkPipelineSampleLocationsStateCreateInfoEXT state{};
        xenos_sample_locations(t.msaa, t.image.samples, false, positions, state);
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.pNext = &state.sampleLocationsInfo;
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.image = t.image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(c, &dep);
        b.srcAccessMask = b.dstAccessMask;
        b.oldLayout = b.newLayout, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(c, &dep);
    }
    // polygon offsets on float depth in the format's own units even with
    // VK_EXT_depth_bias_control (XF_DEPTH_BIAS_FLOAT_UNITS=1)
    bool depth_bias_float_units_ = false;

    // ---- the interlock path (Options::render_target_path fsi) ----
    // 0: host render targets; 1 under pixel interlock, 2 under sample
    // interlock. The pixel shaders read and write edram_ themselves (set 0
    // binding 5), with the state of FsiConstants (binding 4) or of the
    // pipeline's specialization; no render targets are made, resolves read
    // EDRAM, clears write it, and occlusion counts are the shaders' (binding
    // 6, a slot's share each, read back at its end).
    uint32_t fsi_ = 0;
    bool fsi_2x_as_4x_ = false;         // the guest's 2x rasterized as 4x (no 2x without attachments)
    // The early rejection's knowledge of the render pass under way
    // (edram_fsi.glsl's xf_fsi_rejects; reset as a pass begins, after its
    // barrier made every earlier write seen): whether a draw in it may have
    // raised or lowered depth in EDRAM, and the tiles color targets wrote.
    // XF_FSI_EARLY_REJECT=0 turns the rejection off.
    // A set of EDRAM's tiles, a bit each, filled a run of tiles at a time.
    struct TileSet {
        std::array<uint64_t, EDRAM_TILES / 64> words{};
        void set_run(uint32_t first, uint32_t count) { // (wrapping at EDRAM's end)
            first %= EDRAM_TILES;
            while (count) {
                const uint32_t bit = first & 63, n = std::min(count, 64 - bit);
                words[first >> 6] |= (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit;
                count -= n, first = (first + n) % EDRAM_TILES;
            }
        }
        void set_all() { words.fill(~0ull); }
        void add(const TileSet &o) {
            for (size_t i = 0; i < words.size(); i++) words[i] |= o.words[i];
        }
        bool meets(const TileSet &o) const {
            for (size_t i = 0; i < words.size(); i++)
                if (words[i] & o.words[i]) return true;
            return false;
        }
    };
    struct FsiPassDepth {
        bool raised = false, lowered = false;
        TileSet color;
    } fsi_pass_;
    bool fsi_early_reject_ = [] {
        const char *v = std::getenv("XF_FSI_EARLY_REJECT");
        return !(v && v[0] == '0');
    }();
    // the EDRAM tiles of a target (its base and a row's tiles) under the
    // draw's reach (reach_), at tile_width x tile_height console pixels each
    void fsi_reach_tiles(TileSet &tiles, uint32_t base, uint32_t pitch_tiles, uint32_t tile_width,
                         uint32_t tile_height) const {
        for (const Reach &r : reach_) {
            if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
            const uint32_t c0 = r.x0 / tile_width, c1 = std::min((r.x1 + tile_width - 1) / tile_width, pitch_tiles);
            const uint32_t r0 = r.y0 / tile_height, r1 = (r.y1 + tile_height - 1) / tile_height;
            if (uint64_t(r1 - r0) * pitch_tiles >= EDRAM_TILES) return tiles.set_all();
            if (c1 <= c0) continue;
            for (uint32_t row = r0; row < r1; row++) tiles.set_run(base + row * pitch_tiles + c0, c1 - c0);
        }
    }
    // the tiles a draw's color targets write (FsiConstants' rt: written, a
    // write mask) and those its depth test reads
    void fsi_color_tiles(const FsiConstants &f, TileSet &tiles) const;
    void fsi_depth_tiles(const FsiConstants &f, TileSet &tiles) const;
    // A draw's own tiles, its color targets' and its depth test's, kept for
    // the next draw with the same reach and targets (most of a pass's draws:
    // each walk of the tiles' rows was an eighth of the back end's time on
    // game G's town). XF_FSI_TILES_MEMO=0 walks them at every draw.
    struct FsiTiles {
        bool valid = false;
        std::vector<Reach> reach;
        uint32_t msaa = 0, pitch = 0, depth_base = 0, tile_dwords = 0, rt[4] = {}, rt_base[4] = {};
        TileSet drawn, read;
    } fsi_tiles_;
    bool fsi_tiles_memo_ = [] {
        const char *v = std::getenv("XF_FSI_TILES_MEMO");
        return !(v && v[0] == '0');
    }();
    const FsiTiles &fsi_draw_tiles(const FsiConstants &f) {
        FsiTiles &m = fsi_tiles_;
        uint32_t rt[4];
        for (uint32_t i = 0; i < 4; i++) rt[i] = f.rt[i] & 0x1FF; // written, write mask, format
        if (fsi_tiles_memo_ && m.valid && m.msaa == (f.flags & 3) && m.pitch == f.pitch &&
            m.depth_base == f.depth_base && m.tile_dwords == tile_dwords_ && !std::memcmp(m.rt, rt, sizeof(rt)) &&
            !std::memcmp(m.rt_base, f.rt_base, sizeof(m.rt_base)) && m.reach.size() == reach_.size() &&
            (reach_.empty() || !std::memcmp(m.reach.data(), reach_.data(), reach_.size() * sizeof(Reach))))
            return m;
        m.valid = true, m.msaa = f.flags & 3, m.pitch = f.pitch, m.depth_base = f.depth_base;
        m.tile_dwords = tile_dwords_;
        std::memcpy(m.rt, rt, sizeof(rt)), std::memcpy(m.rt_base, f.rt_base, sizeof(m.rt_base));
        m.reach.assign(reach_.begin(), reach_.end());
        m.drawn = {}, m.read = {};
        fsi_color_tiles(f, m.drawn);
        fsi_depth_tiles(f, m.read);
        return m;
    }
    VkShaderModule fsi_depth_fs_ = VK_NULL_HANDLE; // depth and stencil alone (no pixel shader)
    // Beyond Xenia: depth alone without the interlock (edram_fsi.glsl's
    // xf_fsi_depth_unordered), for runs of draws without a pixel shader that
    // write depth one way only, without the stencil or an occlusion query: a
    // render pass of their own (Pass::unordered; its barriers order them
    // against the interlock's draws) once a run is fsi_depth_run_min_ long.
    // XF_FSI_UNORDERED_DEPTH=0 turns it off, =<n> sets the run's length.
    VkShaderModule fsi_depth_unordered_fs_ = VK_NULL_HANDLE;
    struct {
        uint32_t direction = 0, count = 0; // the run of such draws so far
    } fsi_depth_run_;
    uint32_t fsi_depth_run_min_ = [] {
        const char *v = std::getenv("XF_FSI_UNORDERED_DEPTH");
        return v && *v ? uint32_t(std::atoi(v)) : 3u;
    }();
    VkPipeline clear_pipeline_ = VK_NULL_HANDLE;
    // EDRAM between the console's size and scale_'s (edram_rescale.comp), for
    // traces' snapshots: through edram_1x_, EDRAM at the console's size; the
    // sets from it into EDRAM and back
    VkPipeline rescale_pipeline_ = VK_NULL_HANDLE;
    vk::Buffer edram_1x_;
    VkDescriptorSet rescale_sets_[2] = {};
    void rescale_edram(bool to_scaled);
    vk::Buffer zpd_counters_, zpd_readback_;
    FsiConstants last_fsi_{};
    VkDeviceSize last_fsi_at_ = NONE;
    // a target's place in EDRAM, as a resolve reads it: a RenderTarget
    // without an image
    RenderTarget edram_view(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format) const;
    void fsi_clear(const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t lo, uint32_t hi);
    void edram_clear_rows(VkCommandBuffer c, const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t lo, uint32_t hi);
    uint32_t fsi_zpd_index(); // the counter the draw counts in, ~0 none
};

Impl::~Impl() {
    if (deferred_) {
        // (what the back end has yet to do done, its thread stopped, before anything it uses goes)
        deferred_->finish();
        const gpu::DeferredSink::Stats s = deferred_->stats();
        deferred_.reset();
        print_back_end(s);
    }
    if (ring_) {
        // (what the thread has yet to record recorded, the thread stopped)
        ring_->stop();
        ring_->print("record ring");
    }
    translation_workers_.reset(); // before the shaders they read
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
        if (s.generic_timed) vkDestroyQueryPool(dev_, s.generic_timed, nullptr);
    }
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    for (auto &[k, s] : shaders_) {
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
        for (auto &[v, m] : s->variants)
            if (m) vkDestroyShaderModule(dev_, m, nullptr);
    }
    for (auto *table : {&pipelines_, &libraries_, &translating_pipelines_})
        for (auto &[k, p] : *table)
            for (VkPipeline pipe : {p->pipeline, p->stand_in, p->generic})
                if (pipe) vkDestroyPipeline(dev_, pipe, nullptr);
    for (auto &modules : generic_vs_)
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto &modules : generic_fs_)
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(dev_, m, nullptr);
    if (programs_.buffer) d_.destroy(programs_);
    for (auto &[k, s] : samplers_) vkDestroySampler(dev_, s.sampler, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (auto &[k, t] : targets_) {
        d_.destroy(t->image);
        if (t->stencil_copy.image) d_.destroy(t->stencil_copy);
    }
    for (auto &[k, t] : textures_) {
        for (const TextureView &v : t->views) vkDestroyImageView(dev_, v.view, nullptr);
        if (t->target_view) vkDestroyImageView(dev_, t->target_view, nullptr);
        d_.destroy(t->image);
        if (t->signed_image.image) d_.destroy(t->signed_image);
    }
    for (auto &[base, kept] : scaled_resolves_) {
        if (kept.view) vkDestroyImageView(dev_, kept.view, nullptr);
        d_.destroy(kept.image);
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
    for (auto &[k, p] : transfer_pipelines_)
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto &modules : transfer_fs_)
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (VkPipeline p : {store_pipelines_[0], store_pipelines_[1], resolve_pipeline_, resolve_image_pipelines_[0],
                         resolve_image_pipelines_[1], load_pipeline_, clear_pipeline_, rescale_pipeline_,
                         scaled_fill_pipeline_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : {fullscreen_vs_, load_fs_[0], load_fs_[1], load_fs_[2], load_uint_fs_, depth_float24_fs_[0],
                             depth_float24_fs_[1], placeholder_fs_, fsi_depth_fs_, fsi_depth_unordered_fs_})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (VkDescriptorPool p : edram_pools_) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkDescriptorPool p : {pool0_, bindless_pool_, compute_pool_, target_pool_})
        if (p) vkDestroyDescriptorPool(dev_, p, nullptr);
    for (VkPipelineLayout l : {edram_layouts_[0], edram_layouts_[1], edram_layouts_[2], layout_, resolve_layout_,
                               resolve_image_layout_, load_layout_})
        if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (VkDescriptorSetLayout l : {edram_set_layouts_[0], edram_set_layouts_[1], set_layouts_[0], set_layouts_[1],
                                    compute_set_layout_, target_set_layout_})
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    for (vk::Buffer *b : {&edram_, &edram_1x_, &host_depth_, &mirror_, &arena_, &draw_data_, &scratch_, &load_scratch_,
                          &zpd_counters_, &zpd_readback_, &extents_, &extents_readback_, &viz_predicates_,
                          &scaled_heap_})
        d_.destroy(*b);
    for (IndexChunk &chunk : index_chunks_) d_.destroy(chunk.buffer);
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
    module_content_.erase(m);
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
//
// While playing (background), on a thread of its own: a game's cache grows past 100 MB, and
// reading it out of the driver and writing it took the renderer's thread 70-120 ms at a swap,
// a frame missed every time new pipelines had settled. VkPipelineCache is synchronized by the
// driver (no VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT here), so the workers and the
// renderer's thread go on making pipelines with it meanwhile. XF_PIPELINE_CACHE_SAVE_THREAD=0
// writes it on the renderer's thread, as before.
void Impl::save_pipeline_cache(bool background) {
    if (!pipeline_cache_ || options_.pipeline_cache.empty()) return;
    Timer timer(cache_save_seconds_);
    if (cache_saver_.joinable()) {
        // one write at a time: a write under way is left to finish (the next swap asks again)
        if (!cache_saver_done_.load(std::memory_order_acquire) && background) return;
        cache_saver_.join();
        const uint64_t bytes = cache_saver_bytes_.load(std::memory_order_relaxed);
        if (bytes)
            report_.pipeline_cache_written = bytes;
        else
            pipelines_saved_ = 0; // failed: written again later
    }
    if (pipelines_saved_ == report_.pipelines) return;
    static const bool thread = [] {
        const char *v = std::getenv("XF_PIPELINE_CACHE_SAVE_THREAD");
        return !(v && v[0] == '0');
    }();
    pipelines_saved_ = report_.pipelines;
    if (background && thread) {
        cache_saver_done_.store(false, std::memory_order_relaxed);
        cache_saver_ = std::thread([this] {
            cache_saver_bytes_.store(write_pipeline_cache(), std::memory_order_relaxed);
            cache_saver_done_.store(true, std::memory_order_release);
        });
        return;
    }
    const uint64_t bytes = write_pipeline_cache();
    if (bytes)
        report_.pipeline_cache_written = bytes;
    else
        pipelines_saved_ = 0;
}

// The cache's data written to its file through a temporary one renamed over it: its bytes, 0
// when it could not be. (Reads nothing of the renderer's but the cache and its path.)
uint64_t Impl::write_pipeline_cache() const {
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return 0;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return 0;
    std::error_code ec;
    std::filesystem::create_directories(options_.pipeline_cache.parent_path(), ec);
    std::filesystem::path temporary = options_.pipeline_cache;
    temporary += ".new";
    FILE *f = std::fopen(temporary.string().c_str(), "wb");
    if (!f) return 0;
    const bool written = std::fwrite(data.data(), 1, size, f) == size;
    std::fclose(f);
    if (!written) return 0;
    std::filesystem::rename(temporary, options_.pipeline_cache, ec);
    return ec ? 0 : size;
}

bool Impl::init(std::string &error) {
    // (XF_RENDERER_CPU_TIMES=1 turns them on, =0 off even where the options ask: a replay timed
    // as live play runs)
    cpu_times_ = options_.cpu_times;
    if (const char *v = std::getenv("XF_RENDERER_CPU_TIMES"); v && *v) cpu_times_ = v[0] == '1';
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
    // The draws' constants in video memory the host writes through its
    // mapping (beyond Xenia, whose upload buffers are system memory): the
    // shaders read them without crossing the bus each draw. Without
    // resizable BAR that memory is a window of about 256 MB, so only where
    // its heap is at least four times the 48 MB taken; the arena otherwise.
    if (const char *off = std::getenv("XF_VK_NO_DEVICE_LOCAL_DRAW_DATA"); !off || !off[0] || off[0] == '0')
        draw_data_ = d_.create_device_local_mapped_buffer(VkDeviceSize(SLOTS) * DRAW_DATA_SLOT,
                                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                          4 * VkDeviceSize(SLOTS) * DRAW_DATA_SLOT);
    if (const char *always = std::getenv("XF_VK_DRAW_STATE_ALWAYS"); always && always[0] && always[0] != '0')
        draw_state_elision_ = false;
    if (options_.verbose)
        std::fprintf(stderr, "renderer: the draws' constants in %s\n",
                     draw_data_.buffer ? "video memory (device-local, host-visible)" : "the arena (host memory)");

    // The render target path (Options::render_target_path): the interlock
    // one where asked for (XF_RENDER_TARGET_PATH=fsi or host forces either)
    // and the device has fragment shader interlock, host render targets
    // otherwise; automatic is host (docs/renderer.md, "Exact EDRAM through
    // fragment shader interlock", has the measurements). The guest's 2x is
    // rasterized as 4x where a render pass without attachments takes no 2x.
    {
        if (render_target_path(d_, options_) == Options::RenderTargetPath::fsi) fsi_ = d_.fragment_shader_interlock();
        fsi_2x_as_4x_ = fsi_ && !(d_.no_attachments_samples() & VK_SAMPLE_COUNT_2_BIT);
        std::fprintf(stderr, "renderer: render targets: %s\n",
                     fsi_ ? "exact EDRAM (fragment shader interlock)" : "the host's");
        if (const char *e = std::getenv("XF_FSI_DYNAMIC"); e && *e && *e != '0') options_.fsi_specialized = false;
    }
    // D24S8 as a 32-bit float depth where the device has no 24-bit unorm
    // depth (AMD), or where XF_DEPTH_FALLBACK=1 asks for it, to test that path
    const char *depth_fallback = std::getenv("XF_DEPTH_FALLBACK");
    if (!d_.format_supported(d24s8_, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (depth_fallback && depth_fallback[0] == '1'))
        d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (const char *e = std::getenv("XF_DEPTH_BIAS_FLOAT_UNITS")) depth_bias_float_units_ = e[0] && e[0] != '0';
    // Host render targets at the Xenos's sample positions (Xenia's
    // xenos_sample_positions, off by default there, on here): needs
    // VK_EXT_sample_locations for both 2x and 4x
    {
        const char *e = std::getenv("XF_XENOS_SAMPLE_POSITIONS");
        const bool wanted = e && e[0] ? e[0] != '0' : options_.xenos_sample_positions;
        const VkSampleCountFlags both = VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
        xenos_positions_ = wanted && !fsi_ && (d_.sample_locations() & both) == both;
        if (wanted && !fsi_)
            std::fprintf(stderr, "renderer: 2x and 4x host render targets at %s sample positions\n",
                         xenos_positions_ ? "the Xenos's" : "the host's (no VK_EXT_sample_locations for 2x and 4x)");
    }
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
            vkAllocateCommandBuffers(dev_, &alloc, &s.pre) != VK_SUCCESS ||
            vkCreateFence(dev_, &fence, nullptr, &s.fence) != VK_SUCCESS)
            return error = "could not make the submission slots", false;
    }
    if (ring_on_) {
        // the recording thread's: the slots' command buffers named by the ring's handles
        ring_ = std::make_unique<rec::Ring>();
        for (uint32_t i = 0; i < SLOTS; i++) {
            Slot &s = slots_[i];
            s.cmd = ring_->fake(2 * i, s.cmd), s.pre = ring_->fake(2 * i + 1, s.pre);
            if (!s.cmd || !s.pre) return error = "no room for the recording ring's handles", false;
        }
    }
    // timestamps where the queue has them (none: no timing): the GPU timing's, and the generic
    // shaders' draws' (generic_allowed)
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d_.physical(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(d_.physical(), &families, family.data());
    const uint32_t bits = d_.queue_family() < families ? family[d_.queue_family()].timestampValidBits : 0;
    timestamp_period_ = limits.timestampPeriod;
    timestamp_mask_ = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    generic_timing_ = bits != 0;
    if (options_.gpu_timing) {
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
    // counters; bound in either path, read only by its pixel shaders; 7 the
    // screen extents, written by the vertex shaders that bound them)
    const VkShaderStageFlags vertex_stages =
        stages & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    VkDescriptorSetLayoutBinding b0[9] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vertex_stages, nullptr},
        // 8 the generic shaders' programs (init_generic)
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
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
    set_info.bindingCount = 9;
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
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10}};
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
            // (the constants by dynamic offsets from draw_data_take)
            VkDescriptorBufferInfo buffers[4] = {{draw_buffer(), 0, sizeof(harness::Constants)},
                                                 {k ? mirror_.buffer : arena_.buffer, 0, VK_WHOLE_SIZE},
                                                 {draw_buffer(), 0, sizeof(System)},
                                                 {draw_buffer(), 0, sizeof(harness::Constants)}};
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
    // At the resolution scale (Options::resolution_scale, or XF_RESOLUTION_SCALE
    // in percent): EDRAM's tiles that many times wider and taller. Where the
    // device cannot hold it, the next smaller scale that it can, said in the
    // notice (the player shows it).
    {
        Scale wanted{std::clamp(options_.resolution_scale, Scale::MIN_Q4, Scale::MAX_Q4)};
        if (const char *percent = std::getenv("XF_RESOLUTION_SCALE"); percent && *percent)
            wanted = Scale::from_percent(std::atoi(percent));
        // The interlock path addresses a pixel's samples from its first one
        // (edram_fsi.frag's fsi_offsets and sample_offset), which keeps them
        // in one half of a tile only where the half (10 samples a quarter of
        // scale) is a whole number of 4-sample groups: even quarters (50%,
        // 100%, 150%, ...). An odd quarter is drawn at the even one below it,
        // said in the notice; the host render target path takes every quarter.
        Scale asked = wanted;
        if (fsi_ && (wanted.q4 & 1)) wanted.q4 = std::max(wanted.q4 - 1, Scale::MIN_Q4);
        min_scalable_ = options_.min_scalable;
        if (const char *min = std::getenv("XF_MIN_SCALABLE"); min && *min) min_scalable_ = uint32_t(std::atoi(min));
        for (scale_ = wanted;; scale_.q4 -= fsi_ ? 2 : 1) {
            tile_dwords_ = scale_.tile_dwords();
            edram_bytes_ = VkDeviceSize(EDRAM_TILES) * tile_dwords_ * 4;
            edram_ = d_.create_buffer(edram_bytes_,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      false);
            // the host's depth beside it: D24FS8's, then D24S8's where the
            // host keeps that as a float too
            if (edram_.buffer)
                host_depth_ =
                    d_.create_buffer(edram_bytes_ * (host_depth_kept(0) ? 2 : 1),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            if (edram_.buffer && host_depth_.buffer) break;
            d_.destroy(edram_), d_.destroy(host_depth_);
            if (scale_.q4 <= Scale::MIN_Q4 || (scale_.q4 <= 4 && wanted.q4 > 4))
                return error = "could not make the EDRAM buffer", false;
        }
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(d_.physical(), &properties);
        max_image_ = std::min(properties.limits.maxImageDimension2D,
                              std::min(properties.limits.maxFramebufferWidth, properties.limits.maxFramebufferHeight));
        anisotropy_ = options_.anisotropy, texture_filter_ = options_.texture_filter;
        if (scale_.q4 != wanted.q4)
            scale_notice_ = "drawn at " + std::to_string(scale_.percent()) + "%: the GPU could not hold EDRAM at " +
                            std::to_string(wanted.percent()) + "%";
        else if (scale_.q4 != asked.q4)
            scale_notice_ = "drawn at " + std::to_string(scale_.percent()) + "%: the exact EDRAM path (interlock) takes " +
                            "multiples of 50%, not " + std::to_string(asked.percent()) + "%";
    }
    // the interlock path's occlusion counters: a slot's share each, zeroed as
    // it begins recording, copied for the CPU as it ends
    zpd_counters_ = d_.create_buffer(VkDeviceSize(SLOTS) * OCCLUSION_QUERIES * 4,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     false);
    zpd_readback_ = d_.create_buffer(VkDeviceSize(SLOTS) * OCCLUSION_QUERIES * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     true, true);
    if (!zpd_counters_.buffer || !zpd_readback_.buffer) return error = "could not make the occlusion counters", false;
    // the screen extents' reports ("screen extents"), zeroed by the first
    // command buffer
    extents_ = d_.create_buffer(VkDeviceSize(SLOTS) * EXTENT_REPORTS * 16,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false);
    extents_readback_ =
        d_.create_buffer(VkDeviceSize(SLOTS) * EXTENT_REPORTS * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
    if (!extents_.buffer || !extents_readback_.buffer)
        return error = "could not make the screen extents' buffers", false;
    for (Slot &s : slots_) s.extents_used = EXTENT_REPORTS;
    // the VIZ queries' predicates ("VIZ queries"), where the device has
    // conditional rendering
    if (d_.conditional_rendering()) {
        viz_predicates_ = d_.create_buffer(
            64 * 4, VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        if (!viz_predicates_.buffer) return error = "could not make the VIZ queries' predicates", false;
    }
    for (int k = 0; k < 2; k++) {
        VkDescriptorBufferInfo buffers[4] = {{draw_buffer(), 0, sizeof(FsiConstants)},
                                             {edram_.buffer, 0, VK_WHOLE_SIZE},
                                             {zpd_counters_.buffer, 0, VK_WHOLE_SIZE},
                                             {extents_.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4] = {};
        for (uint32_t b = 0; b < 4; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = draw_sets_[k];
            writes[b].dstBinding = 4 + b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
    }
    if (fsi_) {
        fsi_depth_fs_ = fsi_ == 2 ? module(EDRAM_FSI_DEPTH_SAMPLE, sizeof(EDRAM_FSI_DEPTH_SAMPLE))
                                  : module(EDRAM_FSI_DEPTH_PIXEL, sizeof(EDRAM_FSI_DEPTH_PIXEL));
        if (!fsi_depth_fs_) return error = "could not make the interlock path's depth shader", false;
        fsi_depth_unordered_fs_ = module(EDRAM_FSI_DEPTH_UNORDERED, sizeof(EDRAM_FSI_DEPTH_UNORDERED));
        if (!fsi_depth_unordered_fs_) return error = "could not make the interlock path's depth shader", false;
    }
    {
        constexpr VkShaderStageFlags store_stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding store[5] = {
            // (the fragment stage's too: a transfer from the target reads it, edram_transfer.frag)
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, store_stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, store_stages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, store_stages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, store_stages, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, store_stages, nullptr},
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
        {
            // transfers: the source's store set, both targets in the push constants
            VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EdramTransfer)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &edram_set_layouts_[0];
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(dev_, &layout, nullptr, &edram_layouts_[2]) != VK_SUCCESS)
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
        transfer_fs_[0][0] = module(EDRAM_TRANSFER, sizeof(EDRAM_TRANSFER));
        transfer_fs_[0][1] = module(EDRAM_TRANSFER_UINT, sizeof(EDRAM_TRANSFER_UINT));
        transfer_fs_[0][2] = module(EDRAM_TRANSFER_DEPTH, sizeof(EDRAM_TRANSFER_DEPTH));
        transfer_fs_[1][0] = module(EDRAM_TRANSFER_MS, sizeof(EDRAM_TRANSFER_MS));
        transfer_fs_[1][1] = module(EDRAM_TRANSFER_MS_UINT, sizeof(EDRAM_TRANSFER_MS_UINT));
        transfer_fs_[1][2] = module(EDRAM_TRANSFER_MS_DEPTH, sizeof(EDRAM_TRANSFER_MS_DEPTH));
        if (d_.stencil_export()) {
            transfer_fs_[0][3] = module(EDRAM_TRANSFER_STENCIL, sizeof(EDRAM_TRANSFER_STENCIL));
            transfer_fs_[1][3] = module(EDRAM_TRANSFER_MS_STENCIL, sizeof(EDRAM_TRANSFER_MS_STENCIL));
        } else if (d_.depth_stencil_color_copies() &&
                   d_.format_supported(VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            transfer_fs_[0][4] = module(EDRAM_TRANSFER_STENCIL_COPY, sizeof(EDRAM_TRANSFER_STENCIL_COPY));
            transfer_fs_[1][4] = module(EDRAM_TRANSFER_MS_STENCIL_COPY, sizeof(EDRAM_TRANSFER_MS_STENCIL_COPY));
            stencil_copies_ = transfer_fs_[0][4] && transfer_fs_[1][4];
        }
        for (auto &modules : transfer_fs_)
            for (int k = 0; k < 3; k++)
                if (!modules[k]) return error = "could not make the EDRAM transfer shaders", false;
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
        if (!fsi_) {
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolveConstants)};
            const VkDescriptorSetLayout set_layouts[3] = {compute_set_layout_, target_set_layout_, edram_set_layouts_[0]};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.setLayoutCount = 3;
            layout.pSetLayouts = set_layouts;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &range;
            if (vkCreatePipelineLayout(dev_, &layout, nullptr, &resolve_image_layout_) != VK_SUCCESS)
                return error = "vkCreatePipelineLayout failed", false;
            resolve_image_pipelines_[0] = compute_pipeline(RESOLVE_IMAGE, sizeof(RESOLVE_IMAGE), resolve_image_layout_);
            resolve_image_pipelines_[1] = compute_pipeline(RESOLVE_IMAGE_MS, sizeof(RESOLVE_IMAGE_MS), resolve_image_layout_);
            if (!resolve_image_pipelines_[0] || !resolve_image_pipelines_[1])
                return error = "could not make the image resolve pipelines", false;
        }
        // the interlock path's clears into EDRAM: the resolves' layout and
        // their EDRAM set (EDRAM at binding 0), edram.glsl's push constants
        static_assert(sizeof(EdramTarget) <= sizeof(ResolveConstants));
        if (fsi_ && !(clear_pipeline_ = compute_pipeline(EDRAM_CLEAR, sizeof(EDRAM_CLEAR), resolve_layout_)))
            return error = "could not make the EDRAM clear pipeline", false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 8;
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
        // at a resolution scale, EDRAM at the console's size beside it, and
        // the sets that move a snapshot between the two
        if (!scale_.one()) {
            edram_1x_ = d_.create_buffer(EDRAM_BYTES,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         false);
            rescale_pipeline_ = compute_pipeline(EDRAM_RESCALE, sizeof(EDRAM_RESCALE), load_layout_);
            VkDescriptorSetLayout twice[2] = {compute_set_layout_, compute_set_layout_};
            alloc.descriptorSetCount = 2;
            alloc.pSetLayouts = twice;
            if (!edram_1x_.buffer || !rescale_pipeline_ ||
                vkAllocateDescriptorSets(dev_, &alloc, rescale_sets_) != VK_SUCCESS)
                return error = "could not make the EDRAM rescale", false;
            VkDescriptorBufferInfo ends[4] = {{edram_1x_.buffer, 0, VK_WHOLE_SIZE}, {edram_.buffer, 0, VK_WHOLE_SIZE},
                                              {edram_.buffer, 0, VK_WHOLE_SIZE}, {edram_1x_.buffer, 0, VK_WHOLE_SIZE}};
            for (uint32_t k = 0; k < 4; k++) {
                writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[k].dstSet = rescale_sets_[k >> 1];
                writes[k].dstBinding = k & 1;
                writes[k].descriptorCount = 1;
                writes[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[k].pBufferInfo = &ends[k];
            }
            vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
            // resolved memory at the scale (Impl::ScaledRegion): its heap, the
            // sets that write it (a resolve, from EDRAM; a fill, from the
            // mirror) and read it (a texture load), the fill's pipeline
            const char *scaled_memory = std::getenv("XF_SCALED_MEMORY");
            if (!(scaled_memory && scaled_memory[0] == '0')) {
                VkDescriptorSetLayout sets3[3] = {compute_set_layout_, compute_set_layout_, compute_set_layout_};
                VkDescriptorSet made[3];
                alloc.descriptorSetCount = 3;
                alloc.pSetLayouts = sets3;
                scaled_heap_ = d_.create_buffer(VkDeviceSize(16) << 20,
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                false);
                scaled_fill_pipeline_ = compute_pipeline(SCALED_FILL, sizeof(SCALED_FILL), load_layout_);
                if (scaled_heap_.buffer && scaled_fill_pipeline_ &&
                    vkAllocateDescriptorSets(dev_, &alloc, made) == VK_SUCCESS) {
                    scaled_resolve_set_ = made[0], scaled_load_set_ = made[1], scaled_fill_set_ = made[2];
                    write_scaled_sets();
                    scaled_tags_.assign(GUEST_MEMORY >> 12, 0);
                    scaled_reads_.assign(GUEST_MEMORY >> 12, 0);
                    scaled_write_swaps_.assign(GUEST_MEMORY >> 12, UINT32_MAX);
                    scaled_memory_ = true;
                    report_.scaled_heap_bytes = scaled_heap_.size;
                }
            }
        }
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
                                  fsi_depth_fs_,       depth_float24_fs_[0], depth_float24_fs_[1], placeholder_fs_,
                                  fsi_depth_unordered_fs_};
    for (uint32_t k = 0; k < std::size(own); k++)
        if (own[k]) module_names_[own[k]] = {2, own[k] == fsi_depth_fs_ ? fsi_ : 0u, k + 1};
    if (!init_generic(error)) return false;
    open_storage();
    // The work of the processor's calls on a thread of its own, the command processor's thread
    // left with the packets (gpu/deferred_sink.h; XF_GPU_BACK_END=0 off)
    if (const char *v = std::getenv("XF_GPU_BACK_END"); !(v && v[0] == '0'))
        deferred_ = std::make_unique<gpu::DeferredSink>(*this, processor_.registers(), mem_);
    return true;
}

// The generic shaders (docs/renderer.md, "The generic shaders"): the program
// buffer (set 0, binding 8; host-visible, written once per shader and never
// again, so nothing on the GPU races the writes), the vertex shader and the
// pixel shader's four forms for this renderer's render target path, named
// as the renderer's own modules (kind 2, from 100) with their code's hash.
bool Impl::init_generic(std::string &error) {
    static const bool off = [] {
        const char *v = std::getenv("XF_GENERIC_SHADERS");
        return v && v[0] == '0';
    }();
    generic_on_ = (options_.generic_shaders || options_.generic_only) && !off;
    // the new shaders' libraries asked of the pipeline cache first (a lookup: only with the
    // device's cache control), and the new forms translated here within a frame's time
    // (Options::pipeline_cache_first, translate_here_ms; XF_PIPELINE_CACHE_FIRST=0,
    // XF_TRANSLATE_HERE_MS=<ms>)
    {
        const char *first = std::getenv("XF_PIPELINE_CACHE_FIRST");
        cache_first_ = options_.pipeline_cache_first && !(first && first[0] == '0') && d_.cache_control();
        const char *here = std::getenv("XF_TRANSLATE_HERE_MS");
        translate_here_seconds_ =
            std::max(0.0, here ? std::strtod(here, nullptr) : options_.translate_here_ms) / 1000.0;
        const char *budget = std::getenv("XF_GENERIC_GPU_MS");
        generic_budget_seconds_ =
            std::max(0.0, budget ? std::strtod(budget, nullptr) : options_.generic_gpu_ms_a_frame) / 1000.0;
        const char *unknown = std::getenv("XF_GENERIC_UNKNOWN_A_FRAME");
        generic_unknown_a_frame_ =
            unknown ? uint32_t(std::strtoul(unknown, nullptr, 10)) : options_.generic_unknown_a_frame;
    }
    // (in video memory where the host can write it, as the interpreter reads
    // it at every instruction; else host memory)
    programs_ = d_.create_device_local_mapped_buffer(PROGRAM_BUFFER_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     16 * PROGRAM_BUFFER_BYTES);
    if (!programs_.buffer) programs_ = d_.create_buffer(PROGRAM_BUFFER_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!programs_.buffer || !programs_.mapped) return error = "could not make the generic shaders' program buffer", false;
    // (offset 0: an empty program, what the draws that run no generic shader point at)
    const uint32_t empty[4] = {0, 1, 0, 0};
    std::memcpy(programs_.mapped, empty, sizeof(empty));
    programs_used_ = 1;
    for (VkDescriptorSet set : draw_sets_) {
        VkDescriptorBufferInfo buffer{programs_.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 8;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    }
    if (!generic_on_) return true;
    auto make = [&](std::span<const uint32_t> words, uint32_t id) {
        VkShaderModule m = module(words.data(), words.size_bytes());
        if (m) {
            module_names_[m] = {2, fsi_, id};
            module_content_[m] = ShaderStorage::hash(0, std::vector<uint32_t>(words.begin(), words.end()));
        }
        return m;
    };
    const std::span<const uint32_t> vertex[2][3] = {{GENERIC_VS, GENERIC_CLIP_VS, GENERIC_CULL_VS},
                                                     {GENERIC_EXTENTS_VS, GENERIC_EXTENTS_CLIP_VS, GENERIC_EXTENTS_CULL_VS}};
    for (uint32_t e = 0; e < 2; e++)
        for (uint32_t c = 0; c < 3; c++) {
            generic_vs_[e][c] = make(vertex[e][c], e || c ? 110 + e * 3 + c : 100);
            if (!generic_vs_[e][c]) return error = "could not make the generic shaders", false;
        }
    // [point coordinates][writes depth]
    const std::span<const uint32_t> host[2][2] = {{GENERIC_FS, GENERIC_DEPTH_FS},
                                                  {GENERIC_POINTS_FS, GENERIC_POINTS_DEPTH_FS}};
    const std::span<const uint32_t> pixel[2][2] = {{GENERIC_FSI_FS, GENERIC_FSI_DEPTH_FS},
                                                   {GENERIC_FSI_POINTS_FS, GENERIC_FSI_POINTS_DEPTH_FS}};
    const std::span<const uint32_t> sample[2][2] = {{GENERIC_FSI_SAMPLE_FS, GENERIC_FSI_SAMPLE_DEPTH_FS},
                                                    {GENERIC_FSI_SAMPLE_POINTS_FS, GENERIC_FSI_SAMPLE_POINTS_DEPTH_FS}};
    for (uint32_t p = 0; p < 2; p++)
        for (uint32_t d = 0; d < 2; d++)
            generic_fs_[p][d] = make(fsi_ == 2 ? sample[p][d] : fsi_ ? pixel[p][d] : host[p][d], 101 + p * 2 + d);
    if (!generic_fs_[0][0] || !generic_fs_[0][1] || !generic_fs_[1][0] || !generic_fs_[1][1])
        return error = "could not make the generic shaders", false;
    // the translation off the renderer's thread, where the generic shaders
    // draw meanwhile (XF_TRANSLATE_ASYNC=0 keeps it here)
    static const bool translate_here = [] {
        const char *v = std::getenv("XF_TRANSLATE_ASYNC");
        return v && v[0] == '0';
    }();
    if (options_.async_pipelines && pipeline_workers_ && use_libraries_ && !options_.generic_only &&
        !options_.linked_pipelines_only && stand_in_ == Options::PipelineStandIn::library && !translate_here)
        translation_workers_ = std::make_unique<TranslationWorkers>(2);
    return true;
}

// A shader's program in the program buffer, encoded and written the first
// time (Shader::program); false with why when the generic shaders cannot run it.
bool Impl::program(Shader &s, std::string &why) {
    if (!s.encoded) {
        s.encoded = true;
        shader::Program p = shader::encode_program(s.parsed);
        s.program_flags = p.flags;
        const uint32_t units = uint32_t(p.words.size() / 4);
        if (!p.ok()) {
            s.program_unsupported = p.unsupported;
        } else if (VkDeviceSize(programs_used_ + units) * 16 > programs_.size) {
            s.program_unsupported = "the program buffer is full";
        } else {
            std::memcpy(static_cast<uint8_t *>(programs_.mapped) + VkDeviceSize(programs_used_) * 16, p.words.data(),
                        p.words.size() * 4);
            s.program = programs_used_;
            programs_used_ += units;
            report_.generic_programs++;
        }
    }
    if (s.program == UINT32_MAX) why = s.program_unsupported;
    return s.program != UINT32_MAX;
}

// ---- submissions ----

VkCommandBuffer Impl::cmd() {
    begin_recording();
    return slots_[current_].cmd;
}

uint32_t Impl::run_recorded(VkCommandBuffer c, const uint32_t *r) {
    const uint32_t *a = r + 1;
    auto value = [a]<class T>(T &out) {
        std::memcpy(&out, a, sizeof(T));
        return uint32_t(sizeof(T) + 3) / 4;
    };
    switch (Recorded::Op(r[0])) {
    case Recorded::BindPipeline: {
        VkPipeline pipeline;
        const uint32_t words = value(pipeline);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        return words;
    }
    case Recorded::BindSets: {
        Recorded::Sets sets;
        const uint32_t words = value(sets);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, sets.count, sets.sets, 4, sets.offsets);
        return words;
    }
    case Recorded::BindIndexBuffer: {
        VkBuffer buffer;
        const uint32_t words = value(buffer);
        vkCmdBindIndexBuffer(c, buffer, 0, VK_INDEX_TYPE_UINT32);
        return words;
    }
    case Recorded::BindVertexBuffer: {
        Recorded::Place place;
        const uint32_t words = value(place);
        vkCmdBindVertexBuffers(c, 0, 1, &place.buffer, &place.at);
        return words;
    }
    case Recorded::Viewport: {
        VkViewport viewport;
        const uint32_t words = value(viewport);
        vkCmdSetViewport(c, 0, 1, &viewport);
        return words;
    }
    case Recorded::Scissor: {
        VkRect2D scissor;
        const uint32_t words = value(scissor);
        vkCmdSetScissor(c, 0, 1, &scissor);
        return words;
    }
    case Recorded::BlendConstants: {
        float constants[4];
        const uint32_t words = value(constants);
        vkCmdSetBlendConstants(c, constants);
        return words;
    }
    case Recorded::StencilReference: vkCmdSetStencilReference(c, a[0], a[1]); return 2;
    case Recorded::StencilCompareMask: vkCmdSetStencilCompareMask(c, a[0], a[1]); return 2;
    case Recorded::StencilWriteMask: vkCmdSetStencilWriteMask(c, a[0], a[1]); return 2;
    case Recorded::Topology: vkCmdSetPrimitiveTopology(c, VkPrimitiveTopology(a[0])); return 1;
    case Recorded::RestartEnable: vkCmdSetPrimitiveRestartEnable(c, a[0]); return 1;
    case Recorded::CullMode: vkCmdSetCullMode(c, a[0]); return 1;
    case Recorded::FrontFace: vkCmdSetFrontFace(c, VkFrontFace(a[0])); return 1;
    case Recorded::DepthTest: vkCmdSetDepthTestEnable(c, a[0]); return 1;
    case Recorded::DepthWrite: vkCmdSetDepthWriteEnable(c, a[0]); return 1;
    case Recorded::DepthCompare: vkCmdSetDepthCompareOp(c, VkCompareOp(a[0])); return 1;
    case Recorded::StencilTest: vkCmdSetStencilTestEnable(c, a[0]); return 1;
    case Recorded::StencilOp:
        vkCmdSetStencilOp(c, a[0], VkStencilOp(a[1]), VkStencilOp(a[2]), VkStencilOp(a[3]), VkCompareOp(a[4]));
        return 5;
    case Recorded::DepthBiasEnable: vkCmdSetDepthBiasEnable(c, a[0]); return 1;
    case Recorded::BlendEnables: {
        VkBool32 enables[4];
        const uint32_t words = value(enables);
        set_blend_enable_(c, 0, 4, enables);
        return words;
    }
    case Recorded::BlendEquations: {
        VkColorBlendEquationEXT equations[4];
        const uint32_t words = value(equations);
        set_blend_equation_(c, 0, 4, equations);
        return words;
    }
    case Recorded::WriteMasks: {
        VkColorComponentFlags masks[4];
        const uint32_t words = value(masks);
        set_write_mask_(c, 0, 4, masks);
        return words;
    }
    case Recorded::LineWidth: {
        float width;
        const uint32_t words = value(width);
        vkCmdSetLineWidth(c, width);
        return words;
    }
    case Recorded::DepthBias: {
        Recorded::Bias bias;
        const uint32_t words = value(bias);
        vkCmdSetDepthBias(c, bias.constant, 0.0f, bias.slope);
        return words;
    }
    case Recorded::DepthBiasExact: {
        // (a float depth format's unit is forced to a 24-bit unorm one's)
        Recorded::Bias from;
        const uint32_t words = value(from);
        VkDepthBiasRepresentationInfoEXT representation{VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
        representation.depthBiasRepresentation =
            from.host_float ? VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT
                            : VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;
        representation.depthBiasExact = VK_TRUE;
        VkDepthBiasInfoEXT bias{VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &representation};
        bias.depthBiasConstantFactor = from.constant;
        bias.depthBiasSlopeFactor = from.slope;
        vkCmdSetDepthBias2EXT(c, &bias);
        return words;
    }
    case Recorded::BeginConditional: {
        Recorded::Place place;
        const uint32_t words = value(place);
        VkConditionalRenderingBeginInfoEXT conditional{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
        conditional.buffer = place.buffer;
        conditional.offset = place.at;
        vkCmdBeginConditionalRenderingEXT(c, &conditional);
        return words;
    }
    case Recorded::EndConditional: vkCmdEndConditionalRenderingEXT(c); return 0;
    case Recorded::Draw: vkCmdDraw(c, a[0], 1, 0, 0); return 1;
    case Recorded::DrawIndexed: vkCmdDrawIndexed(c, a[0], 1, a[1], 0, 0); return 2;
    }
    return 0;
}

void Impl::begin_recording() {
    Slot &s = slots_[current_];
    if (!s.recording) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmd, &begin);
        s.recording = true;
        drawn_at_submission_ = report_.drawn;
        s.serial = next_serial_++;
        s.used = 0, s.draw_used = 0;
        s.readback = 0, s.readback_used = 0;
        in_pass_ = false;
        bound_pipeline_ = VK_NULL_HANDLE;
        draw_state_valid_ = false;
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
        // the generic draws' timestamps, where the last submission used them (made the first
        // time a recording begins with the generic shaders on)
        if (!s.generic_timed && generic_timing_ && generic_on_ && generic_budget_seconds_ > 0) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2 * GENERIC_TIMED;
            if (vkCreateQueryPool(dev_, &info, nullptr, &s.generic_timed) != VK_SUCCESS)
                s.generic_timed = VK_NULL_HANDLE;
            s.generic_reset = true;
        }
        if (s.generic_timed && s.generic_reset) vkCmdResetQueryPool(s.cmd, s.generic_timed, 0, 2 * GENERIC_TIMED);
        s.generic_reset = false;
        s.generic_shaders.clear();
        // the interlock path's counters, the slot's share zeroed
        if (fsi_ && s.occlusion_used) {
            vkCmdFillBuffer(s.cmd, zpd_counters_.buffer, VkDeviceSize(current_) * OCCLUSION_QUERIES * 4,
                            VkDeviceSize(s.occlusion_used) * 4, 0);
            full_barrier(s.cmd);
        }
        s.occlusion_used = 0;
        // the screen extents' share its last submission used zeroed
        if (s.extents_used) {
            vkCmdFillBuffer(s.cmd, extents_.buffer, VkDeviceSize(current_) * EXTENT_REPORTS * 16,
                            VkDeviceSize(s.extents_used) * 16, 0);
            full_barrier(s.cmd);
        }
        s.extents_used = 0;
        s.extent_reports.clear();
    }
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
        if (s.marks[i] == gpu_generic_kind_) gpu_generic_seconds_ += seconds;
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
    if (!s.extent_reports.empty()) {
        // the screen extents' atomics, for extent_collect
        full_barrier(s.cmd);
        VkDeviceSize at = VkDeviceSize(current_) * EXTENT_REPORTS * 16;
        VkBufferCopy copy{at, at, VkDeviceSize(s.extent_reports.size()) * 16};
        vkCmdCopyBuffer(s.cmd, extents_.buffer, extents_readback_.buffer, 1, &copy);
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
    if (!barrier_generic_) {
        // what the submission wrote, for the host's reads once it is done (the
        // read-backs' buffers): full_barrier's accesses leave the host's out
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = BARRIER_WRITES;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(s.cmd, &dep);
    }
    vkEndCommandBuffer(s.cmd);
    s.recording = false;
    VkCommandBuffer buffers[2] = {s.cmd, VK_NULL_HANDLE};
    uint32_t buffer_count = 1;
    if (s.pre_recording) {
        // the uploads seen by everything after them
        full_barrier(s.pre);
        vkEndCommandBuffer(s.pre);
        s.pre_recording = false;
        buffers[0] = s.pre, buffers[1] = s.cmd, buffer_count = 2;
    }
    if (ring_ && !ring_->direct()) {
        // submitted by the recording thread once it has recorded the slot's
        // command buffers, the fence reset here first (its last submission
        // is done: finish_oldest saw it signaled): a wait for it before the
        // thread submits is a wait for the GPU still (finish_oldest)
        vkResetFences(dev_, 1, &s.fence);
        struct Submit {
            Impl *self;
            uint32_t count, targets[2];
            VkFence fence;
            uint64_t serial;
            void operator()(VkCommandBuffer) const {
                VkCommandBuffer real[2] = {self->ring_->real(targets[0]), self->ring_->real(targets[1])};
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = count;
                submit.pCommandBuffers = real;
                if (VkResult r = self->d_.submit(1, &submit, fence); r != VK_SUCCESS)
                    self->ring_submit_error_.store(int32_t(r), std::memory_order_relaxed);
                self->ring_submitted_.store(serial, std::memory_order_release);
            }
        };
        const uint32_t pre = rec::fake_of(s.pre)->target, main = rec::fake_of(s.cmd)->target;
        Submit submit{this, buffer_count, {buffer_count == 2 ? pre : main, main}, s.fence, s.serial};
        ring_->put(rec::Ring::NONE, submit);
        ring_->publish();
    } else {
        Timer timer(report_.submit_seconds);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = buffer_count;
        submit.pCommandBuffers = buffers;
        if (ring_) {
            // (a port's frame: the ring's handles stand for the real ones, the ring recorded)
            for (uint32_t i = 0; i < buffer_count; i++) buffers[i] = ring_->real(rec::fake_of(buffers[i])->target);
            ring_submitted_.store(s.serial, std::memory_order_release);
        }
        vkResetFences(dev_, 1, &s.fence);
        if (VkResult r = d_.submit(1, &submit, s.fence); r != VK_SUCCESS)
            fatal_ = "a submission failed (" + std::to_string(r) + ")" + lost_device_report(r);
    }
    report_.submissions++;
    flushed_serial_ = s.serial;
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
    if (ring_) {
        // the recording thread's submission of it made first (a wait counted by the ring)
        if (ring_submitted_.load(std::memory_order_acquire) < oldest->serial) {
            if (!wait) return false;
            ring_->wait_for(ring_submitted_, oldest->serial, rec::Ring::Fence);
        }
        if (const int32_t r = ring_submit_error_.load(std::memory_order_relaxed); r != VK_SUCCESS) {
            fatal_ = "a submission failed (" + std::to_string(r) + ")" + lost_device_report(VkResult(r));
            return false;
        }
    }
    VkResult status = vkGetFenceStatus(dev_, oldest->fence);
    if (status == VK_NOT_READY) {
        if (!wait) return false;
        Timer submit(report_.submit_seconds), waiting(report_.wait_seconds);
        report_.waits++;
        const uint64_t t0 = gpu::DeferredSink::on_back_end() ? FastClock::now() : 0;
        const uint64_t c0 = time_gpu_wait_cpu_ ? current_thread_cpu_ticks() : 0;
        status = vkWaitForFences(dev_, 1, &oldest->fence, VK_TRUE, 20'000'000'000ull);
        if (time_gpu_wait_cpu_) gpu_wait_cpu_ticks_.fetch_add(current_thread_cpu_ticks() - c0, std::memory_order_relaxed);
        if (t0) back_end_gpu_wait_seconds_ += FastClock::seconds(FastClock::now() - t0);
    }
    if (status != VK_SUCCESS) {
        fatal_ = "waiting for the GPU failed (" + std::to_string(status) + ")" + lost_device_report(status);
        return false;
    }
    finish(*oldest);
    return true;
}

// The GPU is done with the slot's submission: what waited for that goes
// (images and buffers no longer used).
void Impl::finish(Slot &s) {
    gpu_collect(s);
    generic_collect(s);
    zpd_collect(s);
    extent_collect(s, uint32_t(&s - slots_));
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
    // (the pass under way: the draw's commands go to the recording thread, nothing waited for)
    if (in_pass_ && pass_ == pass) return slots_[current_].cmd;
    fsi_pass_ = {};
    VkCommandBuffer c = cmd();
    end_pass();
    // a target's kept clears (pending_clears) beyond this pass's area drawn
    // before it, the rest in it once it begins
    for (RenderTarget *t : {colors[0], colors[1], colors[2], colors[3], depth}) {
        if (!t) continue;
        for (const auto &[clear, rect] : t->pending_clears)
            if (rect.rect.offset.x < 0 || rect.rect.offset.y < 0 ||
                uint64_t(rect.rect.offset.x) + rect.rect.extent.width > pass.width ||
                uint64_t(rect.rect.offset.y) + rect.rect.extent.height > pass.height) {
                flush_clears(t);
                break;
            }
    }
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
        gpu_pass_kind_ = gpu_kind_ids_[kind]; // (the pass's again after a generic draw)
    }
    // a kept clear over the whole render area as the attachment's load
    // operation (the driver's fast clear, as Xenia's ClearRenderTargetView
    // gets): the last such one, the kept clears before it dropped (it
    // covers them), those after drawn in the pass
    static const bool clear_load_op = [] {
        const char *v = std::getenv("XF_CLEAR_LOAD_OP");
        return !(v && v[0] == '0');
    }();
    for (uint32_t i = 0; i < 5 && clear_load_op; i++) {
        RenderTarget *t = i < 4 ? colors[i] : depth;
        if (!t || t->pending_clears.empty()) continue;
        for (size_t j = t->pending_clears.size(); j-- > 0;) {
            const auto &[clear, rect] = t->pending_clears[j];
            if (rect.rect.offset.x || rect.rect.offset.y || rect.rect.extent.width != pass.width ||
                rect.rect.extent.height != pass.height)
                continue;
            VkRenderingAttachmentInfo &a = i < 4 ? attachments[i] : depth_attachment;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.clearValue = clear.clearValue;
            t->pending_clears.erase(t->pending_clears.begin(), t->pending_clears.begin() + ptrdiff_t(j) + 1);
            break;
        }
    }
    vkCmdBeginRendering(c, &rendering_info);
    if (depth) depth->samples_unsettled = true;
    in_pass_ = true;
    pass_ = pass;
    for (uint32_t i = 0; i < 5; i++) {
        RenderTarget *t = i < 4 ? colors[i] : depth;
        if (!t || t->pending_clears.empty()) continue;
        // (timed with the pass's draws)
        for (auto [clear, rect] : t->pending_clears) {
            clear.colorAttachment = i < 4 ? i : 0;
            vkCmdClearAttachments(c, 1, &clear, 1, &rect);
        }
        t->pending_clears.clear();
    }
    return c;
}

// ---- the arena ----

VkDeviceSize Impl::arena_take(VkDeviceSize bytes, VkDeviceSize align) {
    begin_recording();
    Slot &s = slots_[current_];
    VkDeviceSize at = (s.used + align - 1) / align * align;
    if (at + bytes > ARENA_SLOT) return NONE;
    s.used = at + bytes;
    return VkDeviceSize(current_) * ARENA_SLOT + at;
}

bool Impl::index_cache_place(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &at) {
    if (index_chunks_.empty() || index_chunks_.back().used + bytes > INDEX_CHUNK) {
        IndexChunk chunk;
        chunk.buffer = d_.create_buffer(INDEX_CHUNK, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        if (!chunk.buffer.buffer) return false;
        index_chunks_.push_back(chunk);
    }
    IndexChunk &chunk = index_chunks_.back();
    buffer = chunk.buffer.buffer, at = chunk.used;
    chunk.used += (bytes + 3) & ~VkDeviceSize(3);
    return true;
}

void Impl::index_cache_drop() {
    // (draws recorded so far read the buffers: they outlive the submission)
    begin_recording();
    for (IndexChunk &chunk : index_chunks_)
        slots_[current_].after.push_back([this, buffer = chunk.buffer]() mutable { d_.destroy(buffer); });
    index_chunks_.clear();
    index_cache_.clear();
    std::fill(index_front_.begin(), index_front_.end(), nullptr);
    index_cache_words_ = 0;
}

VkDeviceSize Impl::draw_data_take(VkDeviceSize bytes, VkDeviceSize align) {
    if (!draw_data_.buffer) return arena_take(bytes, align);
    begin_recording();
    Slot &s = slots_[current_];
    VkDeviceSize at = (s.draw_used + align - 1) / align * align;
    if (at + bytes > DRAW_DATA_SLOT) return NONE;
    s.draw_used = at + bytes;
    return VkDeviceSize(current_) * DRAW_DATA_SLOT + at;
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
    begin_recording();
    slots_[current_].after.push_back([this, own]() mutable { d_.destroy(own); });
    buffer = own.buffer, offset = 0;
    return static_cast<uint8_t *>(own.mapped);
}

uint8_t *Impl::readback(VkDeviceSize bytes, VkBuffer &buffer, VkDeviceSize &offset) {
    begin_recording();
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
    if (memo && memo->epoch == mirror_epoch_ && memo->count == count_before && memo->ranges == ranges) {
        if (hoisted_uploads_ && memo->used_serial != submission_serial()) {
            mark_used(ranges);
            memo->used_serial = submission_serial();
        }
        return 0;
    }
    // runs of pages to give, [first, end); a page taken is marked current
    // (UINT32_MAX) until the count is known, so ranges sharing it give it once
    std::vector<std::pair<uint32_t, uint32_t>> &runs = request_runs_; // (kept for its room)
    runs.clear();
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
        mark_used(ranges);
        if (memo) *memo = {ranges, count_before, mirror_epoch_, submission_serial()};
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
    // runs of pages the submission has not used go ahead of it (pre_cmd)
    std::vector<VkBufferCopy> &copies = request_copies_, &ahead = request_ahead_;
    copies.clear(), ahead.clear();
    const uint64_t serial = submission_serial();
    VkDeviceSize offset = 0;
    for (auto [first, end] : runs) {
        uint32_t start = first << page_shift_;
        VkDeviceSize bytes = VkDeviceSize(end - first) << page_shift_;
        std::memcpy(data + offset, memory_.data() + start, bytes);
        bool unused = hoisted_uploads_;
        for (uint32_t p = first; p < end && unused; p++) unused = p >= page_used_.size() || page_used_[p] != serial;
        (unused ? ahead : copies).push_back({at + offset, start, bytes});
        offset += bytes;
        for (uint32_t p = first; p < end; p++) {
            mirror_at_[p] = count;
            set_gpu_page(p, false);
        }
        // (newer than what resolves kept at the resolution scale there)
        clear_scaled_tags(first << page_shift_, end << page_shift_);
    }
    if (!ahead.empty()) vkCmdCopyBuffer(pre_cmd(), from, mirror_.buffer, uint32_t(ahead.size()), ahead.data());
    if (!copies.empty()) {
        VkCommandBuffer c = outside();
        gpu_mark("other (barriers, uploads into the mirror, clears of new targets)");
        vkCmdCopyBuffer(c, from, mirror_.buffer, uint32_t(copies.size()), copies.data());
    }
    mark_used(ranges);
    // (every page of the ranges current as of count, while nothing is
    // written after it: those just given, and the rest found current above
    // if nothing was written since that check either)
    if (memo && count == count_before) *memo = {ranges, count, mirror_epoch_, serial};
    return total;
}

void Impl::mark_used(const std::vector<Range> &ranges) {
    if (!hoisted_uploads_) return;
    const uint64_t serial = submission_serial();
    if (page_used_.size() < mirror_at_.size()) page_used_.resize(mirror_at_.size(), 0);
    for (const Range &r : ranges) {
        if (r.second <= r.first) continue;
        for (uint32_t p = first_page(r); p <= last_page(r) && p < page_used_.size(); p++) page_used_[p] = serial;
    }
}

// The slot's command buffer that runs ahead of its submission, begun with a
// barrier after everything submitted before (whose draws may still read
// what it overwrites)
VkCommandBuffer Impl::pre_cmd() {
    cmd(); // (the same pool as the command buffer the recording thread records: waited for)
    Slot &s = slots_[current_];
    if (!s.pre_recording) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.pre, &begin);
        s.pre_recording = true;
        full_barrier(s.pre);
    }
    return s.pre;
}

// The GPU wrote the ranges in the mirror (a resolve, a memory export; Xenia's
// RangeWrittenByGpu): their pages hold what guest memory does not, current
// until the CPU writes them, so they are watched. What else those pages hold
// was made current before (request).
void Impl::gpu_written(const std::vector<Range> &ranges) {
    mark_used(ranges);
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
    XF_DRAW_TALLY(UnwrittenScans);
    for (const Range &r : t.ranges) {
        if (r.second <= r.first) continue;
        if (!pages_unwritten(first_page(r), last_page(r), t.checked)) return false;
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
    drop_scaled_resolves(pages);
    clear_scaled_tags(pages);
    gpu_written(pages);
    exporting_ = true;
    textures_written_by_gpu(pages);
    exporting_ = false;
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
            // (a stale one again only where resolves were covering it: what
            // they wrote of it is no longer all memory holds there)
            if (t->high <= low || t->low >= high ||
                (t->stale && t->load_why == LoadWhy::GpuWritten && !t->covered_count))
                continue;
            bool hit = false;
            for (const Range &r : written) {
                for (const Range &tr : t->ranges)
                    if (overlaps(r, tr)) {
                        hit = true;
                        break;
                    }
                if (hit) break;
            }
            if (hit) {
                t->stale = true, t->load_why = LoadWhy::GpuWritten, t->exported = exporting_;
                uncover(*t);
                if (refusal_detail_) t->stale_by = stale_by_;
            }
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
        for (uint32_t slot : s->summary.texture_slots)
            if (slot < 64) {
                s->texture_mask |= 1ull << slot;
                if (slot < 32) s->fetch_blocks |= 1u << (slot * 6 / 32) | 1u << ((slot * 6 + 5) / 32);
            }
        for (uint32_t slot : s->summary.vertex_slots)
            if (slot < 128) {
                s->vertex_mask[slot >> 6] |= 1ull << (slot & 63);
                if (slot < 96) s->fetch_blocks |= 1u << (slot * 2 / 32);
            }
        // the float constants read (every ALU operand of every exec, reached or not)
        s->float_blocks = 0;
        for (const xf::ucode::ControlFlow &cf : s->parsed.control_flow) {
            const auto *exec = std::get_if<xf::ucode::Exec>(&cf.instruction);
            if (!exec) continue;
            for (const xf::ucode::ExecSlot &slot : exec->slots) {
                const auto *alu = std::get_if<xf::ucode::AluInstruction>(&slot.instruction);
                if (!alu) continue;
                auto read = [&](const xf::ucode::Operand &o) {
                    if (o.source != xf::ucode::StorageSource::ConstantFloat) return;
                    if (o.addressing != xf::ucode::StorageAddressing::Absolute)
                        s->float_blocks = 0xFFFFFFFFu;
                    else if (o.index < 256)
                        s->float_blocks |= 1u << (o.index / 8);
                };
                for (uint32_t k = 0; k < alu->vector_operand_count && k < 3; k++) read(alu->vector_operands[k]);
                for (uint32_t k = 0; k < alu->scalar_operand_count && k < 2; k++) read(alu->scalar_operands[k]);
            }
        }
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
    (type == 0 ? vertex_shader_ : pixel_shader_) = s.get();
}

Shader *Impl::shader(uint64_t hash) {
    auto it = shaders_.find(hash);
    return it == shaders_.end() ? nullptr : it->second.get();
}

// what the draws read of a shader's microcode, once
void Impl::analyze(Shader &s) {
    if (!s.analyzed) {
        s.analyzed = true;
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
                            s.writes_position_xz |= mask & 5;
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
                    if (t->opcode == xf::ucode::FetchOpcode::TextureFetch && !s.filters.count(t->operands[1].index)) {
                        s.filters[t->operands[1].index] = {uint32_t(t->mag_filter), uint32_t(t->min_filter),
                                                           uint32_t(t->mip_filter), uint32_t(t->aniso_filter)};
                        if (t->operands[1].index < 32) {
                            s.filter_mask |= 1u << t->operands[1].index;
                            s.slot_filters[t->operands[1].index] = s.filters[t->operands[1].index];
                        }
                    }
                }
            }
        }
    }
}

// A shader's form (0 its first; a vertex form's key, or a pixel form's key
// with bit 31), translated here, or with `pending` given and a form the
// generic shaders draw (Impl::translate_later) on the translation workers,
// pending set while it is (docs/renderer.md, "The translation off the
// renderer's thread"). Null when it cannot be made (or is pending).
VkShaderModule Impl::form_module(Shader &s, uint32_t key, const shader::VertexOptions &vertex,
                                 const shader::PixelOptions &pixel, bool *pending) {
    if (!key && s.translated) return s.module;
    if (key) {
        auto it = s.variants.find(key);
        if (it != s.variants.end()) return it->second;
    }
    const bool is_pixel = s.parsed.type == xf::ucode::ShaderType::Pixel;
    shader::VertexOptions v = vertex;
    shader::PixelOptions p = pixel;
    v.texture_point_snap = p.texture_point_snap = point_snap_;
    p.console_centers = console_centers_ && !scale_.one();
    v.vertex_snap = vertex_snap_;
    v.extents_subgroup = v.extents && extents_subgroup_ && d_.vertex_subgroup_arithmetic();
    // (Options::translate_here_ms) translated here while the frame's time for it lasts and no
    // worker has it already: the draw may then have its exact pipeline at once (from the
    // pipeline cache), where the generic shaders drawing it meanwhile cost the GPU many times
    // the translated shader's time
    bool later = pending && translate_later(s, key);
    if (later && !s.translating.count(key) &&
        report_.translate_seconds - translate_seconds_at_swap_ < translate_here_seconds_) {
        later = false;
        report_.translations_in_budget++;
    }
    if (later) {
        if (s.translating.insert(key).second)
            translation_workers_->queue({&s, key, is_pixel, v, p});
        *pending = true;
        return VK_NULL_HANDLE;
    }
    shader::Translation t;
    {
        TranslateTimer timer(report_);
        t = shader::translate(s.parsed, is_pixel ? shader::Stage::Pixel : shader::Stage::Vertex, v, p);
    }
    return finish_form(s, key, t);
}

// A form's translation made into its module and kept (here or from the
// translation workers)
VkShaderModule Impl::finish_form(Shader &s, uint32_t key, const shader::Translation &t) {
    s.translating.erase(key);
    VkShaderModule m = VK_NULL_HANDLE;
    std::string failure = t.unsupported.empty() ? "the translator wrote nothing" : t.unsupported.front();
    if (t.ok()) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = t.spirv.size() * 4;
        info.pCode = t.spirv.data();
        if (vkCreateShaderModule(dev_, &info, nullptr, &m) != VK_SUCCESS) {
            m = VK_NULL_HANDLE;
            failure = "vkCreateShaderModule failed";
        }
        if (m) module_names_[m] = {1, key, s.hash}, module_content_[m] = ShaderStorage::hash(0, t.spirv);
    }
    if (!key) {
        s.translated = true;
        s.module = m;
        s.approximate = t.approximate;
        if (!m) s.unsupported = failure;
    } else {
        s.variants[key] = m;
    }
    return m;
}

// whether a form is translated on the workers: with them (async pipelines
// with the generic shaders and pipeline libraries), for a form the generic
// shaders draw (a vertex shader's plain host form; a pixel shader's form
// without the float24 conversion), of a shader they run
bool Impl::translate_later(Shader &s, uint32_t key) {
    if (!translation_workers_) return false;
    if (s.parsed.type == xf::ucode::ShaderType::Pixel ? (key & 3) != 0 : (key & 0xFF) != 0) return false;
    std::string why;
    return program(s, why);
}

// the translations the workers finished, kept
void Impl::collect_translations() {
    if (!translation_workers_) return;
    std::vector<TranslationWorkers::Done> done;
    translation_workers_->take(done);
    for (TranslationWorkers::Done &d : done) {
        report_.translations_async++;
        report_.translate_async_seconds += d.seconds;
        // (made meanwhile here: kept as it is)
        if (d.key ? d.shader->variants.count(d.key) != 0 : d.shader->translated) {
            d.shader->translating.erase(d.key);
            continue;
        }
        finish_form(*d.shader, d.key, d.translation);
    }
}

bool Impl::translate(Shader &s, std::string &why, bool *pending) {
    analyze(s);
    if (!s.translated) form_module(s, 0, {}, {}, pending);
    if (!s.unsupported.empty()) why = s.unsupported;
    return s.unsupported.empty();
}

// a pixel shader's other forms (shader::PixelOptions), made as draws ask for
// them, kept among its variants above the vertex forms' keys
VkShaderModule Impl::pixel_module(Shader &s, const shader::PixelOptions &options, std::string &why, bool *pending) {
    const uint32_t key = options.key() ? options.key() | 1u << 31 : 0;
    VkShaderModule m = form_module(s, key, {}, options, pending);
    if (!m && !(pending && *pending)) why = "its float24 depth, centroid or interlock form could not be made";
    return m;
}

VkShaderModule Impl::vertex_module(Shader &s, const shader::VertexOptions &options, std::string &why, bool *pending) {
    VkShaderModule m = form_module(s, options.key(), options, {}, pending);
    if (!m && !(pending && *pending)) why = "its host form could not be made";
    return m;
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
    t->host_width = std::min(scale_.up(t->width), max_image_);
    t->host_height = std::min(scale_.up(t->height), max_image_);
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
    // (a multisampled depth target drawn at sample positions of our own must say so)
    VkImageCreateFlags image_flags = raw_format != VK_FORMAT_UNDEFINED ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    if (depth && xenos_at(msaa)) image_flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
    t->image = d_.create_image(VK_IMAGE_TYPE_2D, vk_format, {t->host_width, t->host_height, 1}, 1, 1, samples, usage,
                               image_flags);
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
                     base, pitch, msaa, format, t->host_width, t->host_height);
    return (targets_[key] = std::move(t)).get();
}

// ---- EDRAM: tiles change hands between render targets ----

// The target takes over the tiles of its first `rows` pixel rows: those
// another target holds are drawn into it from their holder (edram_transfer),
// and those no target holds are loaded from EDRAM, as Xenia's ownership
// transfers do. (XF_EDRAM_TRANSFERS=store: the holders store theirs into
// EDRAM first, then all are loaded from there.)
void Impl::acquire(RenderTarget *t, uint32_t rows, const DepthOverwrite *overwritten) {
    const TileRect whole_rows = tiles_of(*t, 0, 0, t->pitch_tiles * t->tile_width, rows);
    acquire(t, std::span<const TileRect>(&whole_rows, 1), overwritten);
}

// The same for the tiles of these rectangles of the target's tiles (a draw's
// reach, a resolve's rectangle). `overwritten`: the draw this is for writes
// the depth of every sample of the pixels it names (Impl::depth_overwrite),
// so the tiles wholly inside them need not be read from their holders.
void Impl::acquire(RenderTarget *t, std::span<const TileRect> rects, const DepthOverwrite *overwritten) {
    // (it held them at its last draw and no tile changed hands since)
    const bool known = t->owned_epoch == owners_epoch_;
    if (known && std::all_of(rects.begin(), rects.end(), [&](const TileRect &r) { return r.empty() || r.within(t->owned); }))
        return;
    // the tiles, as the target numbers them from its base, in order (once
    // each: a draw's rectangles may share some), within EDRAM's 2048
    std::vector<uint32_t> &wanted = acquire_tiles_;
    wanted.clear();
    uint32_t bottom_row = 0;
    for (const TileRect &r : rects) {
        if (r.empty()) continue;
        bottom_row = std::max(bottom_row, r.r1);
        for (uint32_t row = r.r0; row < r.r1; row++)
            for (uint32_t column = r.c0; column < r.c1; column++)
                if (row * t->pitch_tiles + column < EDRAM_TILES) wanted.push_back(row * t->pitch_tiles + column);
    }
    if (rects.size() > 1) {
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    }
    // (kept for their room: thousands of calls a frame)
    std::vector<uint32_t> &needed = acquire_needed_, &unheld = acquire_unheld_;
    needed.clear(), unheld.clear();
    std::map<RenderTarget *, std::vector<uint32_t>> held;
    uint32_t taken_unread = 0;
    for (uint32_t k : wanted) {
        uint32_t tile = (t->base + k) % EDRAM_TILES;
        if (owners_[tile] == t) continue;
        needed.push_back(tile);
        // (the draw this is for writes every sample of the tile whatever it
        // holds: taken as it is, nothing read from its holder or from EDRAM)
        if (overwritten && tile_within(*t, tile, *overwritten)) {
            if (overwritten->whole && overwrite_transfers_) {
                tile_cleared_[tile] = 0, tile_stencil_[tile] = -1;
                taken_unread++;
                continue;
            }
            // (the draw writes the depth of every sample of the tile, and
            // all of them have one stencil: any depth with that stencil
            // will do, so the tile is taken as a clear to the draw's own)
            if (!overwritten->whole && overwritten->known_depth && draw_clears_ && transfer_clears_ &&
                direct_transfers_ && owners_[tile] && owners_[tile]->depth && tile_stencil_[tile] >= 0) {
                const uint32_t word = (overwritten->word & ~0xFFu) | uint32_t(tile_stencil_[tile]);
                tile_cleared_[tile] = 1, tile_clear_word_[tile] = word;
                tile_clear_value_[tile] = depth_clear_value(t->format, word);
                cleared_epoch_++;
            }
        }
        if (owners_[tile])
            held[owners_[tile]].push_back(tile);
        else
            unheld.push_back(tile);
    }
    // what is known to be the target's now: these rectangles, with what was
    // known before where no tile changes hands
    auto remember = [&](bool keep) {
        if (!keep) t->owned = {};
        for (const TileRect &r : rects)
            if (!r.empty()) t->owned.remember(r);
    };
    if (needed.empty()) {
        t->owned_epoch = owners_epoch_;
        remember(known);
        return;
    }
    const uint32_t tile_rows = bottom_row;
    if (direct_transfers_) {
        // (edram_transfer keeps the clears it moves as clears, and drops the rest)
        if (!held.empty() && !edram_transfer(t, held)) return;
        for (uint32_t tile : unheld) tile_cleared_[tile] = 0, tile_stencil_[tile] = -1;
        if (!unheld.empty() && !edram_load(t, unheld, tile_rows * t->tile_height)) return;
    } else {
        for (uint32_t tile : needed) tile_cleared_[tile] = 0, tile_stencil_[tile] = -1;
        for (auto &[holder, tiles] : held)
            if (!edram_store(holder, tiles)) return;
        std::vector<uint32_t> loaded; // (needed, without the tiles taken unread)
        for (uint32_t tile : needed)
            if (!overwritten || !tile_within(*t, tile, *overwritten)) loaded.push_back(tile);
        if (!loaded.empty() && !edram_load(t, loaded, tile_rows * t->tile_height)) return;
    }
    for (uint32_t tile : needed) owners_[tile] = t;
    if (!t->depth)
        for (uint32_t tile : needed) tile_stencil_[tile] = -1;
    t->owned_epoch = ++owners_epoch_;
    remember(known); // (it lost none of what it was known to hold)
    if (options_.verbose) {
        std::string from;
        for (auto &[holder, tiles] : held) from += " [" + std::to_string(tiles.size()) + " of " + target_name(*holder) + "]";
        std::fprintf(stderr, "  EDRAM: %zu tiles to %s (from %zu targets%s, %u taken unread)%s\n", needed.size(),
                     target_name(*t).c_str(), held.size(),
                     direct_transfers_ && !held.empty() && transfer_cleared_only_ ? " as clears" : "", taken_unread,
                     from.c_str());
    }
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
    flush_clears(t);
    uint32_t last_row = 0;
    for (uint32_t tile : tiles) last_row = std::max(last_row, ((tile + EDRAM_TILES - t->base) % EDRAM_TILES) / t->pitch_tiles);
    uint32_t height = std::min(scale_.up((last_row + 1) * t->tile_height), t->host_height);
    VkDeviceSize mask = edram_mask(tiles);
    if (mask == NONE) return false;
    EdramTarget push = edram_target(*t, height);
    uint32_t offset = uint32_t(mask);
    VkCommandBuffer c = outside();
    if (options_.gpu_timing) gpu_mark("EDRAM store: " + target_name(*t));
    settle_depth_samples(c, *t);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipelines_[t->msaa ? 1 : 0]);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, edram_layouts_[0], 0, 1, &t->store_set, 1, &offset);
    vkCmdPushConstants(c, edram_layouts_[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(c, (t->host_width + 7) / 8, (height + 7) / 8, 1);
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
    t.host_width = scale_.up(t.width), t.host_height = scale_.up(t.height);
    return t;
}

void Impl::fsi_color_tiles(const FsiConstants &f, TileSet &tiles) const {
    const uint32_t msaa = f.flags & 3, pitch_tiles = f.pitch / tile_dwords_;
    for (uint32_t i = 0; i < 4; i++) {
        if (!(f.rt[i] & 0x100) || !((f.rt[i] >> 4) & 0xF)) continue;
        const uint32_t format = f.rt[i] & 0xF;
        const bool wide = format == 5 || format == 7 || format == 15;
        fsi_reach_tiles(tiles, f.rt_base[i] / tile_dwords_, pitch_tiles * (wide ? 2 : 1),
                        (msaa >= 2 ? 40 : 80) / (wide ? 2 : 1), msaa ? 8 : 16);
    }
}
void Impl::fsi_depth_tiles(const FsiConstants &f, TileSet &tiles) const {
    const uint32_t msaa = f.flags & 3;
    fsi_reach_tiles(tiles, f.depth_base / tile_dwords_, f.pitch / tile_dwords_, msaa >= 2 ? 40 : 80, msaa ? 8 : 16);
}

// A resolve's clear into EDRAM on the interlock path (edram_clear.comp): the
// rectangle's samples get the clear's words
void Impl::fsi_clear(const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t lo, uint32_t hi) {
    VkCommandBuffer c = outside();
    gpu_mark("resolves: clears");
    // Whole rows of tiles the rectangle covers, all of their samples one
    // word (where the pitch is a whole number of tiles and a 64-bit clear's
    // two words are the same), are a run of EDRAM filled as it is, without
    // the shader's scattered writes; the rows above and below go through it.
    if ((!t.wide || lo == hi) && x == 0 && w == t.pitch_tiles * t.tile_width) {
        uint32_t first_row = (y + t.tile_height - 1) / t.tile_height, end_row = (y + h) / t.tile_height;
        if (end_row > first_row) {
            uint32_t first = (t.base + first_row * t.pitch_tiles) % EDRAM_TILES;
            uint32_t count = std::min((end_row - first_row) * t.pitch_tiles, EDRAM_TILES);
            uint32_t before_wrap = std::min(count, EDRAM_TILES - first);
            const VkDeviceSize tile_bytes = VkDeviceSize(tile_dwords_) * 4; // (at the resolution scale)
            vkCmdFillBuffer(c, edram_.buffer, first * tile_bytes, before_wrap * tile_bytes, lo);
            if (count > before_wrap) vkCmdFillBuffer(c, edram_.buffer, 0, (count - before_wrap) * tile_bytes, lo);
            // the rows above and below the filled ones
            uint32_t top = first_row * t.tile_height - y, bottom_y = end_row * t.tile_height;
            uint32_t bottom = y + h - bottom_y;
            if (top) edram_clear_rows(c, t, x, y, w, top, lo, hi);
            if (bottom) edram_clear_rows(c, t, x, bottom_y, w, bottom, lo, hi);
            return;
        }
    }
    edram_clear_rows(c, t, x, y, w, h, lo, hi);
}

void Impl::edram_clear_rows(VkCommandBuffer c, const RenderTarget &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t lo, uint32_t hi) {
    EdramTarget p{};
    p.base_tiles = t.base, p.pitch_tiles = t.pitch_tiles, p.msaa = t.msaa, p.format = t.format;
    // (the rectangle's pixels at the resolution scale: x, y, w and h are
    // multiples of 8, whole at every quarter)
    p.width = scale_.down(w), p.height = scale_.down(h), p.is_depth = t.depth, p.ints_log2 = t.wide ? 1 : 0;
    p.x = scale_.down(x), p.y = scale_.down(y), p.clear_lo = lo, p.clear_hi = hi;
    p.scale = scale_.q4;
    w = p.width, h = p.height;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, clear_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 1, &resolve_edram_set_, 0, nullptr);
    vkCmdPushConstants(c, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
}

// EDRAM between the console's size (edram_1x_) and scale_'s, tile by tile
// (edram_rescale.comp), between full barriers
void Impl::rescale_edram(bool to_scaled) {
    VkCommandBuffer c = outside();
    full_barrier(c);
    uint32_t push[2] = {to_scaled ? 4u : scale_.q4, to_scaled ? scale_.q4 : 4u};
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, rescale_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1, &rescale_sets_[to_scaled ? 0 : 1], 0,
                            nullptr);
    vkCmdPushConstants(c, load_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(c, (Scale{push[1]}.tile_dwords() + 63) / 64, EDRAM_TILES, 1);
    full_barrier(c);
}

// The interlock path's occlusion counter for the draw being recorded (the
// report open, a counter per report in the slot's share, as zpd_draw opens
// a host query; ~0 when none counts)
uint32_t Impl::fsi_zpd_index() {
    if (!zpd_current_) return UINT32_MAX;
    begin_recording();
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
    if (deferred_) deferred_->finish();
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
    // (at a resolution scale, EDRAM given back at the console's size)
    if (!scale_.one()) rescale_edram(false);
    vkCmdCopyBuffer(outside(), scale_.one() ? edram_.buffer : edram_1x_.buffer, back, 1, &copy);
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
    if (deferred_) deferred_->finish();
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
    return load_pipelines_[key] = edram_pipeline(t, fs, edram_layouts_[1], false);
}

VkPipeline Impl::transfer_pipeline(const RenderTarget &t, bool source_ms) {
    VkFormat color_format = t.raw ? t.raw_format : t.image.format;
    auto key = std::make_tuple(color_format, t.msaa, source_ms);
    auto it = transfer_pipelines_.find(key);
    if (it != transfer_pipelines_.end()) return it->second;
    const VkShaderModule *fs = transfer_fs_[source_ms ? 1 : 0];
    VkShaderModule module = !t.depth ? fs[t.raw ? 1 : 0] : fs[3] ? fs[3] : stencil_copies_ ? fs[4] : fs[2];
    return transfer_pipelines_[key] = edram_pipeline(t, module, edram_layouts_[2], t.depth && stencil_copies_);
}

// A pipeline drawing a whole target's samples from a fragment shader (the
// fullscreen triangle, the scissor picking the region): EDRAM loads and
// transfers. Depth targets write depth and stencil always (the stencil a bit
// per pass without stencil export: its write mask and reference dynamic).
// With stencil_copy, a depth target's pipeline writes depth alone and the
// stencil into an R8_UINT color attachment.
VkPipeline Impl::edram_pipeline(const RenderTarget &t, VkShaderModule fs, VkPipelineLayout layout, bool stencil_copy) {
    VkFormat color_format = t.raw ? t.raw_format : t.image.format;
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
    // (at the positions the target's draws rasterize at: a depth image keeps
    // one set of sample positions)
    VkSampleLocationEXT sample_positions[4];
    VkPipelineSampleLocationsStateCreateInfoEXT sample_locations{
        VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
    if (xenos_at(t.msaa)) {
        xenos_sample_locations(t.msaa, t.image.samples, false, sample_positions, sample_locations);
        multisample.pNext = &sample_locations;
    }
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (t.depth) {
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        // (the stencil left alone where it is copied in after the pass)
        depth.stencilTestEnable = stencil_copy ? VK_FALSE : VK_TRUE;
        depth.front = depth.back = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                                    VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = stencil_copy ? VK_COLOR_COMPONENT_R_BIT : 0xF;
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = t.depth && !stencil_copy ? 0 : 1;
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
        if (stencil_copy) color_format = VK_FORMAT_R8_UINT;
    }
    if (!t.depth || stencil_copy) {
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
    info.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}

bool Impl::edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows) {
    flush_clears(t);
    VkPipeline pipeline = load_pipeline(*t);
    if (!pipeline) return fatal_ = "could not make an EDRAM load pipeline", false;
    VkDeviceSize mask = edram_mask(tiles);
    if (mask == NONE) return false;
    uint32_t height = std::min(scale_.up(rows), t->host_height), offset = uint32_t(mask);
    EdramTarget push = edram_target(*t, height);
    VkCommandBuffer c = outside();
    if (options_.gpu_timing) gpu_mark("EDRAM load: " + target_name(*t));
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = t->raw ? t->raw : t->view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {t->host_width, height}};
    rendering.layerCount = 1;
    if (t->depth) {
        rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    }
    vkCmdBeginRendering(c, &rendering);
    t->samples_unsettled = t->depth;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline_ = VK_NULL_HANDLE; // the draws' pipeline and sets are bound again after it
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, edram_layouts_[1], 0, 1, &edram_load_set_, 1, &offset);
    vkCmdPushConstants(c, edram_layouts_[1], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkViewport viewport{0, 0, float(t->host_width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {t->host_width, height}};
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

// The tiles each holder has of what the target takes over, drawn straight
// from the holder's image into the target (edram_transfer.frag), in one
// render pass: a draw per rectangle of the target's tiles a holder has
// (Xenia's Transfer::GetRangeRectangles: a run of tiles is at most a part of
// a row, whole rows, a part of a row), a pass per stencil bit more for depth
// without stencil export.
bool Impl::edram_transfer(RenderTarget *t, const std::map<RenderTarget *, std::vector<uint32_t>> &held) {
    flush_clears(t);
    for (const auto &[holder, tiles] : held) flush_clears(holder);
    struct Draw {
        RenderTarget *source;
        std::vector<VkRect2D> rects;
        bool clear = false; // the tiles hold one clear's value: cleared to it, not drawn
        VkClearValue value{};
    };
    std::vector<Draw> draws;
    uint32_t height = 0;
    // the holder's tiles that hold a clear's value, by value, where the
    // target has the holder's format (the same bits: the same clear value
    // whatever the sample count); the rest drawn
    struct Group {
        RenderTarget *holder;
        std::vector<uint32_t> tiles;
        bool clear;
        VkClearValue value;
    };
    std::vector<Group> groups;
    for (auto &[holder, tiles] : held) {
        const bool same = transfer_clears_ && holder->depth == t->depth && holder->format == t->format &&
                          holder->host == t->host && holder->gamma_linear == t->gamma_linear &&
                          (holder->raw != VK_NULL_HANDLE) == (t->raw != VK_NULL_HANDLE);
        // (and a depth target of the other depth format: the same guest word
        // as its own format keeps it, which is what the transfer would draw.
        // Where the host keeps the target's depth as a float beside EDRAM,
        // the transfer would rather take a float left there that still
        // rounds to the word, if one happened to: the same guest bits either
        // way.)
        const bool other_depth = transfer_clears_ && draw_clears_ && holder->depth && t->depth && holder->format != t->format;
        Group drawn{holder, {}, false, {}};
        std::vector<Group> cleared;
        for (uint32_t tile : tiles) {
            if ((!same && !other_depth) || !tile_cleared_[tile]) {
                drawn.tiles.push_back(tile);
                continue;
            }
            const VkClearValue v = same ? tile_clear_value_[tile] : depth_clear_value(t->format, tile_clear_word_[tile]);
            auto g = std::find_if(cleared.begin(), cleared.end(),
                                  [&](const Group &c) { return std::memcmp(&c.value, &v, sizeof(v)) == 0; });
            if (g == cleared.end()) cleared.push_back({holder, {tile}, true, v});
            else g->tiles.push_back(tile);
        }
        if (!drawn.tiles.empty()) groups.push_back(std::move(drawn));
        for (Group &g : cleared) groups.push_back(std::move(g));
    }
    for (const Group &g : groups) {
        RenderTarget *holder = g.holder;
        const std::vector<uint32_t> &tiles = g.tiles;
        // (the tiles drawn hold what the holder drew; those cleared keep their value)
        for (uint32_t tile : tiles) {
            tile_cleared_[tile] = g.clear;
            if (g.clear) tile_clear_value_[tile] = g.value; // (as the target keeps it)
            // (the stencil: the clear's; kept from one depth target to another; else not known)
            if (g.clear && t->depth) tile_stencil_[tile] = int16_t(g.value.depthStencil.stencil & 0xFF);
            else if (!holder->depth || !t->depth) tile_stencil_[tile] = -1;
        }
        if (g.clear) cleared_epoch_++, stencil_epoch_++;
        if (!g.clear && !transfer_pipeline(*t, holder->msaa != 0))
            return fatal_ = "could not make an EDRAM transfer pipeline", false;
        Draw d{holder, {}, g.clear, g.value};
        // the tiles as the target's, in runs
        std::vector<uint32_t> at;
        at.reserve(tiles.size());
        for (uint32_t tile : tiles) at.push_back((tile + EDRAM_TILES - t->base) % EDRAM_TILES);
        std::sort(at.begin(), at.end());
        auto rect = [&](uint32_t row, uint32_t first, uint32_t columns, uint32_t rows) {
            // (in host pixels at the resolution scale: whole tiles, whole at
            // every quarter)
            uint32_t x = scale_.down(first * t->tile_width), y = scale_.down(row * t->tile_height);
            if (x >= t->host_width || y >= t->host_height) return;
            VkRect2D r{{int32_t(x), int32_t(y)},
                       {std::min(scale_.down(columns * t->tile_width), t->host_width - x),
                        std::min(scale_.down(rows * t->tile_height), t->host_height - y)}};
            d.rects.push_back(r);
            height = std::max(height, y + r.extent.height);
        };
        // (the runs of each row, a run that is the same columns as one of the
        // row above joined to it: whole rows are one rectangle, and so is a
        // column of tiles a draw reached)
        struct Open {
            uint32_t row, first, columns, rows;
        };
        std::vector<Open> open, next;
        for (size_t i = 0; i < at.size();) {
            const uint32_t pitch = t->pitch_tiles, row = at[i] / pitch;
            next.clear();
            while (i < at.size() && at[i] / pitch == row) {
                size_t j = i + 1;
                while (j < at.size() && at[j] == at[j - 1] + 1 && at[j] / pitch == row) j++;
                const uint32_t first = at[i] % pitch, columns = uint32_t(j - i);
                auto above = std::find_if(open.begin(), open.end(), [&](const Open &o) {
                    return o.row + o.rows == row && o.first == first && o.columns == columns;
                });
                if (above != open.end()) {
                    next.push_back({above->row, first, columns, above->rows + 1});
                    open.erase(above);
                } else {
                    next.push_back({row, first, columns, 1});
                }
                i = j;
            }
            for (const Open &o : open) rect(o.row, o.first, o.columns, o.rows);
            open.swap(next);
        }
        for (const Open &o : open) rect(o.row, o.first, o.columns, o.rows);
        if (!d.rects.empty()) draws.push_back(std::move(d));
    }
    transfer_cleared_only_ = false;
    if (draws.empty()) return true;
    // (the stencil's copy is for the tiles drawn: a clear writes the
    // stencil aspect itself)
    const bool any_drawn = std::any_of(draws.begin(), draws.end(), [](const Draw &d) { return !d.clear; });
    transfer_cleared_only_ = !any_drawn;
    if (!any_drawn && defer_clears_ && !t->raw) {
        // clears alone: kept for the target's next pass, as a resolve's
        // (flush_clears), with no pass of their own (a pass open on the
        // target ends here: the clears are drawn as the next begins)
        outside();
        for (const Draw &d : draws) {
            VkClearAttachment clear{};
            clear.aspectMask = t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            clear.clearValue = d.value;
            for (const VkRect2D &r : d.rects) t->pending_clears.push_back({clear, {r, 0, 1}});
        }
        return true;
    }
    bool stencil_copy = t->depth && stencil_copies_ && any_drawn, fresh = false;
    if (stencil_copy && !t->stencil_copy.image) {
        t->stencil_copy = d_.create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8_UINT, {t->host_width, t->host_height, 1}, 1, 1,
                                          t->image.samples,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
        if (!t->stencil_copy.image) return fatal_ = "could not make a depth target's stencil image", false;
        t->stencil_copy_view = make_view(t->stencil_copy.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UINT,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        fresh = true;
    }
    VkCommandBuffer c = outside();
    if (fresh) begin_image(c, t->stencil_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    if (options_.gpu_timing) gpu_mark("EDRAM transfer: " + target_name(*t));
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = t->raw ? t->raw : t->view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {t->host_width, height}};
    rendering.layerCount = 1;
    VkRenderingAttachmentInfo stencil_attachment = attachment;
    stencil_attachment.imageView = t->stencil_copy_view;
    stencil_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (t->depth) {
        rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
        if (stencil_copy) rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &stencil_attachment;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    }
    // (the sources' depth at the positions they were rasterized at)
    for (const Draw &d : draws)
        if (!d.clear) settle_depth_samples(c, *d.source);
    vkCmdBeginRendering(c, &rendering);
    t->samples_unsettled = t->depth;
    bound_pipeline_ = VK_NULL_HANDLE; // the draws' pipeline and sets are bound again after it
    VkViewport viewport{0, 0, float(t->host_width), float(t->host_height), 0, 1};
    vkCmdSetViewport(c, 0, 1, &viewport);
    EdramTarget dest = edram_target(*t, t->host_height);
    bool stencil_passes = t->depth && !transfer_fs_[0][3] && !stencil_copy;
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Draw &d : draws) {
        if (d.clear) {
            // the clear's value over the rectangles (depth and stencil
            // both; the copy after the pass leaves these rectangles out)
            std::vector<VkClearRect> rects;
            for (const VkRect2D &r : d.rects) rects.push_back({r, 0, 1});
            VkClearAttachment clears[1] = {};
            uint32_t count = 1;
            if (t->depth) {
                clears[0].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                clears[0].clearValue = d.value;
            } else {
                clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clears[0].colorAttachment = 0;
                clears[0].clearValue = d.value;
            }
            vkCmdClearAttachments(c, count, clears, uint32_t(rects.size()), rects.data());
            continue;
        }
        VkPipeline pipeline = transfer_pipeline(*t, d.source->msaa != 0);
        if (pipeline != bound) vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
        uint32_t offset = 0;
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, edram_layouts_[2], 0, 1, &d.source->store_set, 1,
                                &offset);
        EdramTarget source = edram_target(*d.source, d.source->host_height);
        EdramTransfer push{};
        std::memcpy(push.dest, &dest, 10 * sizeof(uint32_t));
        std::memcpy(push.source, &source, 10 * sizeof(uint32_t));
        push.dest[10] = push.source[10] = scale_.q4;
        vkCmdPushConstants(c, edram_layouts_[2], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        if (t->depth) {
            // the depth and the stencil (exported, or cleared to 0 for the
            // bit passes that follow)
            vkCmdSetDepthWriteEnable(c, VK_TRUE);
            vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        }
        for (const VkRect2D &r : d.rects) {
            vkCmdSetScissor(c, 0, 1, &r);
            vkCmdDraw(c, 3, 1, 0, 0);
        }
        if (!stencil_passes) continue;
        vkCmdSetDepthWriteEnable(c, VK_FALSE);
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        for (uint32_t bit = 0; bit < 8; bit++) {
            EdramTransfer pass = push;
            pass.dest[8] = bit; // stencil_bit
            vkCmdPushConstants(c, edram_layouts_[2], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass), &pass);
            vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
            for (const VkRect2D &r : d.rects) {
                vkCmdSetScissor(c, 0, 1, &r);
                vkCmdDraw(c, 3, 1, 0, 0);
            }
        }
    }
    vkCmdEndRendering(c);
    if (stencil_copy) {
        // the stencil the pass wrote into the R8_UINT image, into the
        // target's stencil aspect, rectangle by rectangle
        full_barrier(c);
        std::vector<VkImageCopy> copies;
        for (const Draw &d : draws)
            for (const VkRect2D &r : d.rects) {
                if (d.clear) continue;
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
                copy.srcOffset = copy.dstOffset = {r.offset.x, r.offset.y, 0};
                copy.extent = {r.extent.width, r.extent.height, 1};
                copies.push_back(copy);
            }
        vkCmdCopyImage(c, t->stencil_copy.image, VK_IMAGE_LAYOUT_GENERAL, t->image.image, VK_IMAGE_LAYOUT_GENERAL,
                       uint32_t(copies.size()), copies.data());
    }
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
    texture::Levels levels = texture::levels(parsed);
    // The player's filtering beyond the game's (Options::anisotropy and
    // texture_filter): nearest everywhere; or, on mipmapped textures (not a
    // base map alone) the game filters linearly, linear between mips and the
    // anisotropy forced, as Xenia's anisotropic_override picks them
    // (vulkan_texture_cache.cc's GetSamplerParameters).
    const uint32_t forced_filter = texture_filter_.load(std::memory_order_relaxed);
    const uint32_t forced_aniso = anisotropy_.load(std::memory_order_relaxed);
    if (forced_filter == 2) {
        mag = min = 0, mip = mip == 2 ? 2 : 0, aniso = 0;
    } else if (levels.mip_max > levels.mip_min && mip != 2 && mag == 1 && min == 1) {
        if (forced_filter == 1) mip = 1;
        // 1x, 2x, 4x, 8x, 16x as Xenos' AnisoFilter 1-5
        if (forced_aniso) aniso = uint32_t(std::bit_width(std::min(forced_aniso, 16u)));
    }
    // points where the host does not filter the formats it reads (Xenia's
    // GetSamplerParameters; anisotropy filters linearly too)
    if ((mag == 1 || min == 1 || mip == 1 || (aniso && aniso < 6)) && !texture::filterable(parsed, support_))
        mag = min = 0, mip = mip == 2 ? 2 : 0, aniso = 0;
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
    last_sampler_ = nullptr;
    auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        it->second.used_serial = submission_serial();
        last_sampler_ = &it->second;
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
        texture_epoch_++;
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
    last_sampler_ = &(samplers_[key] = {s, index, submission_serial()});
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
    texture_epoch_++; // (what the draws kept of it: SlotMemo)
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
// descriptor ever names a destroyed view) and free to take again. What the
// draws kept of the slots that read through it goes (SlotMemo): all of it,
// unless the caller says the view's texture was loaded again since (its
// loads moved: what was kept of its own slots alone is gone, and only those
// name its views). A texture made again at another size once a frame
// otherwise sent every slot of every draw the long way once a frame
// (XF_SLOT_MEMO_KEEP=0: all of it, always).
void Impl::release_index(uint32_t kind, uint32_t index, bool owner_reloaded) {
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
    static const bool keep = [] {
        const char *v = std::getenv("XF_SLOT_MEMO_KEEP");
        return !(v && v[0] == '0');
    }();
    if (!owner_reloaded || !keep) texture_epoch_++; // (what read through it: SlotMemo)
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
            Timer timer(report_.check_seconds, cpu_times_);
            same = unwritten(*t);
        }
        if (same) {
            // a trace gets what guest memory gave it (a replay makes the
            // GPU's parts with its own resolves and exports); only while one
            // is recorded (cpu_parts makes a list and walks the pages: 13% of
            // the command thread in a heavy scene when it ran for every draw)
            if (trace_) traced(t->gpu ? cpu_parts(t->ranges) : t->ranges);
            return t;
        }
        t->load_why = LoadWhy::CpuWritten;
    }
    if (t) t->stale = true, uncover(*t); // until it is loaded again
    Timer loading(report_.texture_load_seconds);
    // a load: what the draws kept of this texture's slots goes (SlotMemo), not
    // of every texture's (a scene reloading fifty textures a frame after its
    // resolves otherwise took every slot of every draw the long way again)
    if (t) t->loads++;

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
    // A resolve target (the resolves write it as well: below), and one a
    // scaled resolve kept the scaled texels of: made at the resolution scale,
    // those copied in rather than loaded (docs/renderer.md, "Resolution scale")
    const uint32_t texel_bytes = texture::format_block(info.format).bytes;
    // (a depth texture's float made from the depth bits by the resolve as its
    // load makes it: beyond the copies, 32-bit texels)
    const uint32_t convert = !depth_targets_                                 ? 0u
                             : plan.load == texture::Load::DepthUnormToFloat ? 1u
                             : plan.load == texture::Load::DepthFloatToFloat ? 2u
                                                                             : 0u;
    const bool target = resolve_targets_ && mirror && !separate && !three && !cube && layers == 1 &&
                        level_count == 1 && plan.slices.size() == 1 && plan.slices[0].level == 0 &&
                        plan.slices[0].tiled && (plan.load == texture::Load::Copy || (convert && texel_bytes == 4)) &&
                        plan.half == texture::Half::None && plan.block_width == 1 && plan.block_height == 1 &&
                        (texel_bytes == 4 || texel_bytes == 8) && plan.bytes_per_block == texel_bytes;
    // A load over what a resolve wrote, counted by format and size with the
    // first reason it is no resolve target (the loads a resolve's own write
    // into the texture could spare; "uses" in the report)
    if (t && t->load_why == LoadWhy::GpuWritten) {
        const char *reason = !resolve_targets_                                        ? "resolve targets off"
                             : !mirror                                                ? "not over the GPU's pages"
                             : separate                                               ? "a separate signed image"
                             : three || cube || layers != 1                           ? "not one 2D layer"
                             : level_count != 1                                       ? "mipmapped"
                             : plan.slices.size() != 1 || plan.slices[0].level != 0   ? "more than one slice"
                             : !plan.slices[0].tiled                                  ? "linear"
                             : plan.load != texture::Load::Copy && !convert           ? "converted as it loads"
                             : plan.half != texture::Half::None                       ? "half-size texels"
                             : plan.block_width != 1 || plan.block_height != 1        ? "block-compressed"
                             : texel_bytes != 4 && texel_bytes != 8                   ? "texels neither 32 nor 64 bits"
                             : plan.bytes_per_block != texel_bytes                    ? "host texels of another size"
                             : !t->target ? "a target with no place in the resolves' table"
                                          : "a target the resolve did not feed";
        char line[320];
        std::snprintf(line, sizeof(line), "texture loads after a resolve: %s %ux%u%s, %s", format_name(info.format),
                      plan.width, plan.height, level_count > 1 ? " mipmapped" : "", reason);
        if (refusal_detail_ && target) {
            // its own base and pitch, and the resolve that made it stale
            const Texture::StaleBy &s = t->stale_by;
            char more[160];
            std::snprintf(more, sizeof(more), " (texture at %08X pitch %u; stale by %s %ux%u pitch %u at %08X +%u,%u)",
                          plan.slices[0].base & 0x1FFFFFFF, plan.slices[0].pitch_blocks,
                          s.width ? format_name(s.format) : "a memory export", s.width, s.height, s.pitch, s.base, s.x,
                          s.y);
            std::strncat(line, more, sizeof(line) - std::strlen(line) - 1);
        }
        use(std::string(line));
    }
    ScaledResolve *kept = nullptr;
    if (target && !convert && !scale_.one() && texture_scalable(plan.width, plan.height) &&
        (texel_bytes == 4 || scale_wide_)) {
        auto found = scaled_resolves_.find(plan.slices[0].base & 0x1FFFFFFF);
        if (found != scaled_resolves_.end() && found->second.pitch == plan.slices[0].pitch_blocks &&
            found->second.endian == info.endian && ((found->second.target >> 7) & 1) == (texel_bytes == 8 ? 1u : 0u) &&
            found->second.width >= plan.width && found->second.height >= plan.height) {
            kept = &found->second;
            extent = {scale_.up(plan.width), scale_.up(plan.height), 1};
        }
    }
    // Over resolved memory kept at the resolution scale (Impl::ScaledRegion):
    // loaded from there at the scale, as Xenia loads its textures from its
    // scaled resolve buffer: one level of 2D slices (layers, cube faces),
    // tiled, of blocks of one texel, no smaller than the minimum scalable
    // dimension, every page of it in one region and holding this layout's
    // scaled bytes (made current first: what the CPU wrote since clears that).
    // What is not is counted by why ("uses" in the report).
    //
    // Mipmapped ones level by level (scaled_mips_), each level's slices in
    // one region, where every level's size at the scale is the image's level
    // size. One part of which is kept at the scale, the rest made from memory
    // first (scaled_fill_load_).
    std::vector<uint32_t> scaled_bases; // each level's first slice, in the heap (scaled_rel); empty: not scaled
    const char *scaled_why = nullptr;   // (for the verbose line)
    if (scaled_memory_ && mirror && !kept && !ranges.empty()) {
        const texture::SliceLoad *first = plan.slices.empty() ? nullptr : &plan.slices[0];
        bool layout = first != nullptr, levels_kept = true;
        for (size_t i = 0; i < plan.slices.size(); i++) {
            const texture::SliceLoad &s = plan.slices[i];
            layout &= s.tiled && !s.is_3d && s.log2_bytes_per_block == first->log2_bytes_per_block;
            // (a level's slices of one pitch; the first level's from its corner)
            if (i && s.level == plan.slices[i - 1].level) layout &= s.pitch_blocks == plan.slices[i - 1].pitch_blocks;
            if (s.level == 0) layout &= !s.ox && !s.oy;
            const uint32_t w = std::max(plan.width >> s.level, 1u), h = std::max(plan.height >> s.level, 1u);
            levels_kept &= std::max(scale_.up(plan.width) >> s.level, 1u) == scale_.up(w) &&
                           std::max(scale_.up(plan.height) >> s.level, 1u) == scale_.up(h) &&
                           scale_.up(s.ox) * 4 == s.ox * scale_.q4 && scale_.up(s.oy) * 4 == s.oy * scale_.q4;
        }
        const bool small = !texture_scalable(plan.width, plan.height);
        const char *reason = three                                             ? "3D"
                             : level_count != 1 && !scaled_mips_               ? "mipmapped"
                             : !layout                                         ? "not tiled 2D slices"
                             : plan.block_width != 1 || plan.block_height != 1 ? "block-compressed"
                             : small                            ? "smaller than the minimum scalable dimension"
                             : level_count != 1 && !levels_kept ? "mipmapped, its levels' sizes not the scale's"
                                                                : nullptr;
        if (!reason) {
            request(ranges);
            // each slice's pages, with the base its tiles count from (for
            // 64- and 128-bit blocks, whose tiles span pages)
            const uint32_t log2_bytes = first->log2_bytes_per_block;
            auto slice_pages = [&](const texture::SliceLoad &s, auto &&f) {
                const uint32_t base = s.base & 0x1FFFFFFF;
                for (uint64_t page = (base + s.low) & ~uint64_t(4095); page < base + s.high; page += 4096)
                    f(uint32_t(page), base);
            };
            bool all = true, any = false;
            for (const texture::SliceLoad &s : plan.slices)
                slice_pages(s, [&](uint32_t page, uint32_t base) {
                    const bool kept_here = scaled_tags_[page >> 12] == scaled_tag(page, base, log2_bytes);
                    all &= kept_here, any |= kept_here;
                    // (looked at: scaled_skip_unread_; always written if left out)
                    uint8_t &reads = scaled_reads_[page >> 12];
                    if (reads) reads |= (reads & 0x20) ? 0xC0 : 0x40;
                });
            // each level's span of guest memory, page aligned (a level's
            // slices follow one another: texture::gpu_levels makes a load of
            // each run of them)
            std::vector<std::pair<uint32_t, uint32_t>> spans;
            std::vector<uint32_t> span_of(plan.slices.size());
            for (size_t i = 0; i < plan.slices.size(); i++) {
                const texture::SliceLoad &s = plan.slices[i];
                const uint32_t base = s.base & 0x1FFFFFFF;
                const uint32_t lo = uint32_t((base + s.low) & ~uint64_t(4095)),
                               hi =
                                   uint32_t(std::min<uint64_t>((base + s.high + 4095) & ~uint64_t(4095), GUEST_MEMORY));
                if (!i || s.level != plan.slices[i - 1].level) spans.push_back({lo, hi});
                span_of[i] = uint32_t(spans.size() - 1);
                spans.back().first = std::min(spans.back().first, lo);
                spans.back().second = std::max(spans.back().second, hi);
            }
            if (!all && any && scaled_fill_load_) {
                // the regions first (one may merge and move another), then
                // the units not holding this layout made from memory
                bool regions = true;
                for (auto [lo, hi] : spans) regions &= make_scaled_region(lo, hi) != nullptr;
                if (regions) {
                    struct Fill {
                        uint32_t guest, at, log2_bytes, q, tiles, words;
                    };
                    std::vector<Fill> fills;
                    const uint32_t tile_bytes = 1024u << log2_bytes, unit_bytes = std::max(tile_bytes, 4096u),
                                   q = scale_.q4;
                    for (const texture::SliceLoad &s : plan.slices) {
                        const uint32_t base = s.base & 0x1FFFFFFF;
                        const uint64_t from = tile_bytes < 4096 ? (base + s.low) & ~uint64_t(4095)
                                                                : base + s.low / tile_bytes * tile_bytes;
                        for (uint64_t unit = from; unit < base + s.high && unit + unit_bytes <= GUEST_MEMORY;
                             unit += unit_bytes) {
                            bool current = true;
                            for (uint64_t page = unit; page < unit + unit_bytes; page += 4096)
                                current &= scaled_tags_[page >> 12] == scaled_tag(uint32_t(page), base, log2_bytes);
                            if (current) continue;
                            const auto *region = scaled_region(uint32_t(unit), uint32_t(unit + unit_bytes));
                            if (!region) continue;
                            fills.push_back({uint32_t(unit),
                                             uint32_t(region->second.heap +
                                                      VkDeviceSize((uint32_t(unit) - region->first) >> 4) * q * q),
                                             log2_bytes, q, unit_bytes / tile_bytes,
                                             uint32_t(((unit_bytes >> 4) * q * q) >> 2)});
                            for (uint64_t page = unit; page < unit + unit_bytes; page += 4096)
                                scaled_tags_[page >> 12] = scaled_tag(uint32_t(page), base, log2_bytes);
                        }
                    }
                    if (!fills.empty()) {
                        VkCommandBuffer c = outside();
                        if (options_.gpu_timing) gpu_mark("textures: scaled memory made from memory for a load");
                        full_barrier(c);
                        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, scaled_fill_pipeline_);
                        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1,
                                                &scaled_fill_set_, 0, nullptr);
                        for (const Fill &f : fills) {
                            vkCmdPushConstants(c, load_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(f), &f);
                            vkCmdDispatch(c, (f.words + 63) / 64, 1, 1);
                        }
                        full_barrier(c);
                        report_.scaled_fills += fills.size();
                        use("textures over resolved memory at the scale: a part made from memory first");
                    }
                    all = true;
                    for (const texture::SliceLoad &s : plan.slices)
                        slice_pages(s, [&](uint32_t page, uint32_t base) {
                            all &= scaled_tags_[page >> 12] == scaled_tag(page, base, log2_bytes);
                        });
                }
            }
            if (all) {
                for (size_t i = 0; i < plan.slices.size(); i++) {
                    if (scaled_bases.size() > span_of[i]) continue;
                    const auto [lo, hi] = spans[span_of[i]];
                    const auto *region = scaled_region(lo, hi);
                    if (!region) break;
                    scaled_bases.push_back(scaled_rel(*region, plan.slices[i].base & 0x1FFFFFFF));
                }
                if (scaled_bases.size() != spans.size()) scaled_bases.clear();
            }
            if (scaled_bases.empty()) reason = "not all of it kept at the scale";
        }
        if (!scaled_bases.empty())
            extent = {scale_.up(plan.width), scale_.up(plan.height), 1};
        else
            use(std::string("textures over resolved memory at the console's size: ") + reason);
        scaled_why = reason;
    }
    bool fresh = !t || t->image.format != format || t->image.extent.width != extent.width ||
                 t->image.extent.height != extent.height || t->image.extent.depth != extent.depth ||
                 t->image.levels != level_count || t->image.layers != layers ||
                 separate != (t->signed_image.image != VK_NULL_HANDLE) || signed_format != t->signed_format;
    // One made before a resolve wrote its memory (no resolve target then:
    // its image has no storage view) and now over what one wrote, made again
    // as one while the table has a place for it: else the loads after every
    // resolve go on (game D's 320x192 and 32x32 surfaces, 16,000 loads a run)
    if (!fresh && target && !t->target) {
        const uint32_t wide = texel_bytes == 8 ? 1 : 0;
        fresh = next_target_[wide] < RESOLVE_TARGETS || !free_targets_[wide].empty();
    }
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
            begin_recording();
            slots_[current_].after.push_back([this, old, old_signed_image, old_views, old_kind, old_target,
                                              old_target_view]() mutable {
                for (const TextureView &v : old_views) {
                    release_index(old_kind, v.index, true); // (t->loads moved above)
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
            } else {
                slot = take_target(wide);
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
                t->target_convert = convert;
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
    t->scaled = kept != nullptr || !scaled_bases.empty();
    if (kept) {
        // the scaled resolve's texels, of the same size (the texture's format
        // holds the guest's bits as they are, as the resolve wrote them)
        VkCommandBuffer c = outside();
        if (fresh) begin_image(c, t->image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        full_barrier(c);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = extent;
        vkCmdCopyImage(c, kept->image.image, VK_IMAGE_LAYOUT_GENERAL, t->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        full_barrier(c);
        std::vector<Range> its{{kept->low, kept->high}};
        drop_scaled_resolves(its);
        report_.textures_scaled++;
        if (options_.verbose)
            std::fprintf(stderr, "  texture %08X: a scaled resolve's texels copied in\n", info.base_address);
    } else if (gpu) {
        if (!gpu_load(*t, t->image, plan, fresh, true, scaled_bases.empty() ? nullptr : &scaled_bases)) return nullptr;
        if (separate &&
            !gpu_load(*t, t->signed_image, signed_plan, fresh, false, scaled_bases.empty() ? nullptr : &scaled_bases))
            return nullptr;
        if (!scaled_bases.empty()) report_.textures_scaled++, report_.textures_scaled_loads++;
    } else {
        if (!cpu_upload(t->image, image, fresh)) return nullptr;
        if (separate && !cpu_upload(t->signed_image, signed_image, fresh)) return nullptr;
    }
    t->checked = checked;
    t->gpu = mirror;
    t->stale = false;
    uncover(*t);
    report_.textures++;
    (t->load_why == LoadWhy::New          ? report_.texture_loads_new
     : t->load_why == LoadWhy::CpuWritten ? report_.texture_loads_cpu_written
                                          : report_.texture_loads_gpu_written)++;
    if (t->load_why == LoadWhy::GpuWritten && t->exported) report_.texture_loads_exported++;
    t->load_why = LoadWhy::CpuWritten; // the next one, unless a resolve or an export marks it
    if (options_.verbose)
        std::fprintf(stderr, "  texture %08X: format %s %ux%u%s, signs %u%u%u%u, exponent %d, from %s as %s%s%s%s\n",
                     info.base_address, format_name(info.format), plan.width, plan.height,
                     level_count > 1 ? " mipmapped" : "", info.signs[0], info.signs[1], info.signs[2], info.signs[3],
                     info.exp_adjust,
                     mirror ? "the mirror"
                     : gpu  ? "guest memory on the GPU"
                            : "guest memory",
                     texture::host_format_name(plan.format), separate ? " and a signed image" : "",
                     t->scaled    ? ", at the scale"
                     : scaled_why ? ", at the console's size: "
                                  : "",
                     !t->scaled && scaled_why ? scaled_why : "");
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
bool Impl::gpu_load(Texture &t, const vk::Image &image, const texture::Plan &plan, bool fresh, bool upload,
                    const std::vector<uint32_t> *scaled_bases) {
    std::vector<texture::GpuLevel> loads;
    uint64_t total = 0;
    texture::gpu_levels(plan, loads, total);
    if (scaled_bases && scaled_bases->size() == loads.size()) {
        // from resolved memory at the scale (texture() checked the layout and
        // that each level's size at the scale is the image's), the image's
        // texels at the scale, level by level from each level's place in the
        // heap (a packed tail's offsets at the scale too)
        total = 0;
        for (size_t i = 0; i < loads.size(); i++) {
            texture::GpuLevel &g = loads[i];
            g.base = (*scaled_bases)[i];
            g.blocks_x = g.out_width = scale_.up(g.out_width), g.blocks_y = g.out_height = scale_.up(g.out_height);
            g.ox = scale_.up(g.ox), g.oy = scale_.up(g.oy);
            const uint64_t bytes = uint64_t(g.blocks_x) * g.blocks_y * texture::bytes_per_block(plan.format) * g.slices;
            g.out_word = uint32_t(total / 4);
            g.words = uint32_t((bytes + 3) / 4);
            const uint32_t groups = (g.words + 63) / 64, across = std::max(std::min(groups, 32768u), 1u);
            g.row_words = across * 64;
            g.scale_q4 = scale_.q4;
            total += (bytes + 15) & ~uint64_t(15);
        }
    }
    std::vector<uint32_t> level_of; // each load's level
    for (size_t i = 0; i < plan.slices.size(); i++)
        if (!i || plan.slices[i].level != plan.slices[i - 1].level) level_of.push_back(plan.slices[i].level);
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkBuffer scratch_before = load_scratch_.buffer;
    if (!grow(load_scratch_, std::max<VkDeviceSize>(total, 16), usage, load_set_, 1)) return false;
    if (load_scratch_.buffer != scratch_before && scaled_load_set_) write_scaled_sets();
    if (upload)
        request(t.ranges);
    else
        mark_used(t.ranges); // read from the mirror in this submission
    VkCommandBuffer c = outside();
    // (by why, format and size: which loads a frame's GPU time goes to)
    if (options_.gpu_timing) {
        char kind[160];
        std::snprintf(kind, sizeof(kind), "textures: loads on the GPU (texture_load): %s, %s %ux%u%s",
                      t.load_why == LoadWhy::New          ? "new"
                      : t.load_why == LoadWhy::CpuWritten ? "after the CPU's writes"
                      : t.exported                        ? "after an export"
                                                          : "after a resolve",
                      format_name(t.key[1] & 0x3F), (t.key[2] & 0x1FFF) + 1, ((t.key[2] >> 13) & 0x1FFF) + 1,
                      loads.size() > 1 ? " mipmapped" : "");
        gpu_mark(std::string(kind));
    }
    if (fresh) begin_image(c, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.levels, image.layers);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1,
                            scaled_bases ? &scaled_load_set_ : &load_set_, 0, nullptr);
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
    // A pipeline found before again, by its key's hash (a scene's draws go back and forth
    // between a few hundred): one comparison of the key instead of the map's search, a compare
    // of two keys at each of its levels. Entries are never dropped.
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (uint32_t word : key) hash = (hash ^ word) * 0x100000001B3ull;
    PipelineFront &front = pipeline_front_[size_t(hash >> 32) & (PIPELINE_FRONT - 1)];
    if (front.entry && front.entry->pipeline && !front.entry->stand_in && !front.entry->generic && key == front.key &&
        front.entry->asked == std::chrono::steady_clock::time_point{} &&
        front.entry->state.load(std::memory_order_acquire)) {
        pipeline_final_ = true;
        return front.entry->pipeline;
    }
    PipelineEntry *e = nullptr;
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        e = it->second.get();
        front.entry = e, front.key = key;
        if (e->state.load(std::memory_order_acquire)) {
            if (e->stand_in) retire_stand_in(*e);
            if (e->generic) retire_generic(*e);
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
            if (e->generic) retire_generic(*e);
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
                built = !built_pipelines_.empty() && built_pipelines_.count(built_key(0, description, info, 4)) != 0;
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
        // made now: waited for (a draw without a pixel shader, as Xenia), or
        // (built, and not linked from libraries that are) found in the
        // pipeline cache, unless the driver would compile it there
        if (!async) {
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
        if (built && !linked) {
            VkPipeline p = VK_NULL_HANDLE;
            if (from_cache(info, p)) {
                e->pipeline = p;
                e->state.store(1, std::memory_order_release);
                return p;
            }
            report_.pipelines_prebuilt_missed++; // made on the workers below, as any other
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
        if (e->generic) retire_generic(*e);
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
    const bool built =
        !built_libraries_.empty() && built_libraries_.count(built_key(part, key, desc.info, part)) != 0;
    if (built) report_.pipeline_libraries_prebuilt++;
    if (async && (part == 1 || part == 2)) {
        if (built) {
            // a lookup in the pipeline cache, unless the driver would have to
            // compile it after all
            VkPipeline p = VK_NULL_HANDLE;
            if (from_cache(desc.info, p)) {
                slot->pipeline = p;
                slot->state.store(1, std::memory_order_release);
                return slot.get();
            }
            report_.pipeline_libraries_prebuilt_missed++;
        } else if (cache_first_ && pipeline_cache_) {
            // (Options::pipeline_cache_first) the pipeline cache asked alone, whatever the build
            // step recorded: an earlier run of this build met the shader, and its library is a
            // lookup there, so the draw links its exact pipeline now rather than being drawn by
            // the generic shaders until a worker has looked it up. Not there: a miss costs a
            // hash and a search, and the workers compile it.
            VkPipeline p = VK_NULL_HANDLE;
            if (from_cache(desc.info, p)) {
                report_.pipeline_libraries_cached++;
                slot->pipeline = p;
                slot->state.store(1, std::memory_order_release);
                return slot.get();
            }
            report_.pipeline_libraries_cache_missed++;
        }
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

uint64_t Impl::built_key(uint32_t seed, const std::vector<uint32_t> &words, const VkGraphicsPipelineCreateInfo &info,
                         uint32_t which) const {
    // the stages a part holds, as LibraryDesc splits them: the pre-
    // rasterization part all but the fragment shader, the fragment shader's
    // part that one; the vertex input and fragment output none
    if (which == 0 || which == 3) return ShaderStorage::hash(seed, words);
    std::vector<uint32_t> keyed = words;
    for (uint32_t i = 0; i < info.stageCount; i++) {
        const bool fragment = info.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        if ((which == 1 && fragment) || (which == 2 && !fragment)) continue;
        auto it = module_content_.find(info.pStages[i].module);
        const uint64_t code = it == module_content_.end() ? 0 : it->second;
        keyed.insert(keyed.end(), {uint32_t(code), uint32_t(code >> 32)});
    }
    return ShaderStorage::hash(seed, keyed);
}

bool Impl::from_cache(const VkGraphicsPipelineCreateInfo &info, VkPipeline &out) {
    out = VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo asked = info;
    // without the device's cache control the record is trusted, as before
    if (d_.cache_control() && pipeline_cache_) asked.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkResult r;
    {
        Timer timer(report_.pipeline_seconds);
        r = vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &asked, nullptr, &out);
    }
    if (r == VK_SUCCESS) return true;
    out = VK_NULL_HANDLE;
    // VK_PIPELINE_COMPILE_REQUIRED: not in the cache; any other result a
    // failure, which the workers would meet too
    return r != VK_PIPELINE_COMPILE_REQUIRED;
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

// The draw's pipeline with the generic shaders in place of the guest's: the
// vertex shader in its plain host form (no expansion in the vertex shader,
// no user clip planes, no screen extents; not tessellated), the pixel
// shader's form without the float24 conversion (its centroids read at the
// center, counted), the interlock path's state from the draw's uniform block
// rather than specialized. The geometry shaders and everything else as the
// draw's.
bool Impl::generic_info(const VkGraphicsPipelineCreateInfo &info, Shader &vs, Shader *ps, bool points,
                        uint32_t vertex_form, uint32_t pixel_form, GenericStages &out, std::string &why) {
    if (!generic_on_) return why = "off", false;
    if (!options_.generic_only && !use_libraries_) return why = "no pipeline libraries on the device", false;
    if (info.stageCount > 5) return why = "more stages than the renderer makes", false;
    // the vertex host form (shader::VertexOptions::key's host 0; ps_ucp_mode
    // applies to points alone), with its screen extents and user clip planes
    if (vertex_form & 0xFF) return why = "the vertex shader expanding points or rectangles", false;
    if (pixel_form & 3) return why = "the pixel shader's float24 depth form", false;
    std::string reason;
    if (!program(vs, reason)) return why = "vertex shader: " + reason, false;
    if (ps && !program(*ps, reason)) return why = "pixel shader: " + reason, false;
    out.info = info;
    out.info.pStages = out.stages;
    out.vertex_data[0] = out.pixel_data = 1;
    const uint32_t planes = std::min<uint32_t>((vertex_form >> 9) & 7, 6), cull = (vertex_form >> 12) & 1,
                   extents = (vertex_form >> 15) & 1;
    for (uint32_t i = 0; i < info.stageCount; i++) {
        VkPipelineShaderStageCreateInfo &st = out.stages[i];
        st = info.pStages[i];
        switch (st.stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            st.module = generic_vs_[extents][planes ? 1 + cull : 0];
            out.vertex_data[0] = point_snap_ ? 1 : 0;
            out.vertex_data[1] = planes;
            st.pSpecializationInfo = point_snap_ || planes ? &out.vertex_spec : nullptr;
            break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return why = "a tessellated draw", false;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            if (!ps) break; // the renderer's own (depth alone)
            if ((pixel_form >> 8) & 0xFFFF)
                approximate("generic shaders: interpolators at the pixel's center, not its centroid");
            // (oDepth as the translator takes it: any result to it)
            st.module = generic_fs_[points ? 1 : 0][ps->program_flags & shader::PROGRAM_WRITES_DEPTH ? 1 : 0];
            st.pSpecializationInfo = point_snap_ ? &out.pixel_spec : nullptr;
            break;
        default: break; // the geometry shaders
        }
    }
    return true;
}

VkPipeline Impl::generic_stand_in(PipelineEntry &e, const VkGraphicsPipelineCreateInfo &generic, bool async) {
    if (e.generic || e.generic_failed) return e.generic;
    if (!e.generic_libraries[0] && !e.generic_libraries[1]) {
        PipelineParts parts;
        library_keys(generic, parts);
        for (uint32_t part = 0; part < 4; part++) e.generic_libraries[part] = library(part, parts.keys[part], generic, async);
    }
    VkPipeline libraries[4];
    uint32_t count = 0;
    for (PipelineEntry *l : e.generic_libraries) {
        if (!l) continue;
        if (!l->state.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
        if (!l->pipeline) {
            approximate("generic shaders: a library the driver failed");
            e.generic_failed = true;
            return VK_NULL_HANDLE;
        }
        libraries[count++] = l->pipeline;
    }
    VkPipelineLibraryCreateInfoKHR list{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    list.libraryCount = count;
    list.pLibraries = libraries;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &list;
    info.layout = layout_;
    {
        Timer timer(report_.pipeline_seconds);
        if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &info, nullptr, &e.generic) != VK_SUCCESS)
            e.generic = VK_NULL_HANDLE;
    }
    if (e.generic) {
        report_.generic_pipelines++;
    } else {
        approximate("generic shaders: linking failed");
        e.generic_failed = true;
    }
    return e.generic;
}

// Whether a draw the generic shaders would draw is drawn (Options::generic_gpu_ms_a_frame): its
// pixel shader's cost on the GPU the last time one of its generic draws was timed counted
// against the frame's budget; a shader not timed yet drawn while the frame's share of those is
// left, to learn its cost. The rest are skipped as without the generic shaders: the GPU's time
// for a frame of their draws ran to hundreds of milliseconds, a frame's draws held up three
// frames behind them (a wave of new shaders in a game's play).
bool Impl::generic_allowed() {
    if (generic_budget_seconds_ <= 0) return true;
    auto it = generic_cost_.find(pixel_hash_);
    if (it == generic_cost_.end()) {
        if (generic_unknown_frame_ >= generic_unknown_a_frame_) return false;
        generic_unknown_frame_++;
        return true;
    }
    if (generic_spent_frame_ + it->second > generic_budget_seconds_) return false;
    generic_spent_frame_ += it->second;
    return true;
}

// The finished submission's generic draws' times, kept by pixel shader
void Impl::generic_collect(Slot &s) {
    if (!s.generic_timed || s.generic_shaders.empty()) return;
    const size_t n = s.generic_shaders.size();
    std::vector<uint64_t> ticks(2 * n);
    if (vkGetQueryPoolResults(dev_, s.generic_timed, 0, uint32_t(2 * n), ticks.size() * 8, ticks.data(), 8,
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const double scale = timestamp_period_ * 1e-9;
        for (size_t i = 0; i < n; i++) {
            const double seconds = double((ticks[2 * i + 1] - ticks[2 * i]) & timestamp_mask_) * scale;
            generic_cost_[s.generic_shaders[i]] = seconds;
            // (the GPU timing's kind counts them where it is on)
            if (!options_.gpu_timing) gpu_generic_seconds_ += seconds;
        }
    }
    s.generic_shaders.clear();
}

void Impl::retire_generic(PipelineEntry &e) {
    VkPipeline old = e.generic;
    e.generic = VK_NULL_HANDLE;
    if (!old) return;
    begin_recording();
    slots_[current_].after.push_back([this, old] { vkDestroyPipeline(dev_, old, nullptr); });
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
    begin_recording();
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
                o.depth_float24 = uint8_t(form & 3);
                o.early_depth_stencil = (form >> 2) & 1;
                o.blend_premultiply = (form >> 3) & 1;
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
                o.extents = (form >> 15) & 1;
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
    // the extra shaders no run met, translated: made into libraries in the
    // descriptions' commonest states below
    std::vector<std::pair<uint32_t, uint64_t>> unmet;
    std::unordered_set<uint64_t> unmet_seen, met_extra;
    for (const auto &[type, code] : o.extra_shaders) {
        if (type > 1) continue;
        const uint64_t hash = hash_words(type, code);
        out.extra_shaders++;
        const bool met = storage.has_shader(hash);
        if (!met) out.extra_new++;
        load_shader(type, code);
        Shader *sh = shader(hash);
        // a vertex shader whose fetches Direct3D fills in from the vertex
        // declaration at bind time: the copy a draw runs is another shader
        if (sh && xf::ucode::awaits_vertex_declaration(sh->parsed)) {
            out.extra_undeclared++;
            continue;
        }
        if (!sh || !translate(*sh, why))
            out.extra_failed++;
        else if (met)
            met_extra.insert(hash);
        else if (unmet_seen.insert(hash).second)
            unmet.emplace_back(type, hash);
    }
    if (cancelled()) return error = "cancelled", false;

    // the descriptions' libraries and whole pipelines, queued for the
    // workers, each library once
    struct VariantState {
        uint64_t uses = 0;
        std::vector<uint32_t> words; // a description in that state
        size_t at = 0;               // where its guest shader's id lies in it
    };
    // a description's guest vertex and pixel shaders: the state of each one's
    // part (its key with the shader's id left out, its form kept), counted
    // (and each shader's part, by the shader and that key, in `parts_seen`)
    std::set<std::tuple<uint32_t, uint64_t, std::vector<uint32_t>>> parts_seen;
    auto note_variant_state = [&parts_seen](std::span<const uint32_t> words, const PipelineParts &parts,
                                            std::map<std::vector<uint32_t>, VariantState> (&states)[2]) {
        if (words.size() < 3) return;
        size_t at = 3; // (describe: per stage, the stage, the module's kind, form and id, the specialization)
        for (uint32_t i = 0; i < words[2]; i++) {
            if (at + 6 > words.size()) return;
            const uint32_t stage = words[at], kind = words[at + 1];
            const size_t id_at = at + 3;
            at += 5;
            if (const uint32_t entries = words[at++]; entries != UINT32_MAX) {
                at += size_t(entries) * 3;
                if (at >= words.size()) return;
                at += (size_t(words[at]) + 3) / 4 + 1;
            }
            if (kind != 1 || (stage != VK_SHADER_STAGE_VERTEX_BIT && stage != VK_SHADER_STAGE_FRAGMENT_BIT)) continue;
            const uint32_t type = stage == VK_SHADER_STAGE_FRAGMENT_BIT;
            std::vector<uint32_t> key = parts.keys[type ? 2 : 1];
            if (key.empty()) continue;
            const uint32_t lo = words[id_at], hi = words[id_at + 1];
            for (size_t k = 3; k + 1 < key.size(); k++)
                if (key[k - 2] == 1 && key[k] == lo && key[k + 1] == hi) key[k] = key[k + 1] = 0;
            VariantState &s = states[type][key];
            if (!s.uses) s.words.assign(words.begin(), words.end()), s.at = id_at;
            s.uses++;
            parts_seen.emplace(type, lo | uint64_t(hi) << 32, std::move(key));
        }
    };
    struct Made {
        uint64_t hash = 0;
        bool library = false;
        std::unique_ptr<PipelineEntry> entry;
        bool extra = false; // an unmet shader's library
        bool generic = false; // a generic shaders' library
    };
    std::vector<Made> made;
    std::unordered_set<uint64_t> queued_libraries;
    // the states the descriptions give the vertex (0) and pixel (1) shaders'
    // parts, by the part's key without the shader
    std::map<std::vector<uint32_t>, VariantState> variant_states[2];
    ShaderStorage::Built record = ids;
    const bool libraries = d_.pipeline_library();
    for (const std::vector<uint32_t> &words : storage.pipelines()) {
        out.descriptions++;
        // made back first: what an earlier build step made is known by the
        // description with its modules' code (built_key), which a newer
        // translator changes
        std::unique_ptr<PipelineDesc> desc = undescribe(words, why);
        if (!desc) {
            out.descriptions_failed++;
            out.descriptions_failed_why[why]++;
            if (options_.verbose)
                std::fprintf(stderr, "  description %016llX: %s\n", (unsigned long long)ShaderStorage::hash(0, words),
                             why.c_str());
            continue;
        }
        const uint64_t hash = built_key(0, words, desc->info, 4);
        PipelineParts parts;
        if (libraries) {
            library_keys(desc->info, parts);
            if (!unmet.empty()) note_variant_state(words, parts, variant_states);
            // the generic shaders' libraries in this description's state, so
            // that a run draws a shader it never met in it at once
            // (Options::generic_shaders), each once
            Shader *vs = nullptr, *ps = nullptr;
            bool points = false;
            uint32_t vertex_form = 0, pixel_form = 0;
            for (uint32_t i = 0; i < desc->info.stageCount; i++) {
                auto name = module_names_.find(desc->info.pStages[i].module);
                if (name == module_names_.end() || name->second.kind != 1) continue;
                if (desc->info.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    ps = shader(name->second.id), points = (name->second.form >> 30) & 1,
                    pixel_form = name->second.form & ~(1u << 31);
                else
                    vs = shader(name->second.id), vertex_form = name->second.form;
            }
            // (the exact EDRAM's pipelines have no attachments)
            const auto *rendering = static_cast<const VkPipelineRenderingCreateInfo *>(desc->info.pNext);
            const bool attachments = rendering && (rendering->colorAttachmentCount ||
                                                   rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
                                                   rendering->stencilAttachmentFormat != VK_FORMAT_UNDEFINED);
            const bool this_path = attachments == !fsi_;
            if (!this_path) out.descriptions_other_path++;
            GenericStages generic;
            if (generic_on_ && this_path && vs && ps && generic_info(desc->info, *vs, ps, points, vertex_form, pixel_form, generic, why)) {
                PipelineParts generic_parts;
                library_keys(generic.info, generic_parts);
                for (uint32_t part : {1u, 2u}) {
                    if (generic_parts.keys[part].empty()) continue;
                    LibraryDesc ld(generic.info, part);
                    const uint64_t lh = built_key(part, generic_parts.keys[part], ld.info, part);
                    if (made_libraries.count(lh) || !queued_libraries.insert(lh).second) continue;
                    Made m{lh, true, std::make_unique<PipelineEntry>()};
                    m.generic = true;
                    pipeline_workers_->queue(m.entry.get(), std::make_unique<PipelineDesc>(ld.info),
                                             PipelinePriority::normal);
                    made.push_back(std::move(m));
                }
            }
        }
        if (made_pipelines.count(hash)) {
            out.descriptions_done_before++;
            continue;
        }
        if (libraries) {
            for (uint32_t part = 0; part < 4; part++) {
                if (parts.keys[part].empty()) continue;
                LibraryDesc ld(desc->info, part);
                const uint64_t lh = built_key(part, parts.keys[part], ld.info, part);
                if (made_libraries.count(lh) || !queued_libraries.insert(lh).second) continue;
                Made m{lh, true, std::make_unique<PipelineEntry>()};
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
    // the unmet shaders' libraries, in the commonest states of their part
    if (libraries && o.extra_variants && !unmet.empty()) {
        out.variant_states_seen = variant_states[0].size() + variant_states[1].size();
        // a state's uses: the shaders runs met in it (a library each)
        for (auto &states : variant_states)
            for (auto &[key, state] : states) state.uses = 0;
        for (const auto &[type, id, key] : parts_seen) variant_states[type][key].uses++;
        for (uint32_t type = 0; type < 2; type++) {
            std::vector<std::pair<const std::vector<uint32_t> *, const VariantState *>> states;
            for (const auto &[key, state] : variant_states[type]) states.emplace_back(&key, &state);
            // the commonest first; ties by the key, so a step chooses the same
            std::sort(states.begin(), states.end(), [](const auto &a, const auto &b) {
                return a.second->uses != b.second->uses ? a.second->uses > b.second->uses : *a.first < *b.first;
            });
            if (states.size() > o.extra_variants) states.resize(o.extra_variants);
            out.variant_states += states.size();
            // how well those states cover the libraries runs made of the
            // disc's shaders they met (a library of each in each state taken)
            for (const auto &[seen_type, id, key] : parts_seen) {
                if (seen_type != type || !met_extra.count(id)) continue;
                out.variant_check_libraries++;
                for (const auto &[state_key, state] : states)
                    if (*state_key == key) {
                        out.variant_check_covered++;
                        break;
                    }
            }
            for (const auto &[shader_type, shader_hash] : unmet) {
                if (shader_type != type) continue;
                for (const auto &[state_key, state] : states) {
                    std::vector<uint32_t> words = state->words;
                    words[state->at] = uint32_t(shader_hash), words[state->at + 1] = uint32_t(shader_hash >> 32);
                    std::unique_ptr<PipelineDesc> desc = undescribe(words, why);
                    if (!desc) {
                        out.extra_libraries_unmade++;
                        continue;
                    }
                    PipelineParts parts;
                    library_keys(desc->info, parts);
                    const uint32_t part = type == 0 ? 1 : 2;
                    if (parts.keys[part].empty()) continue;
                    LibraryDesc ld(desc->info, part);
                    const uint64_t lh = built_key(part, parts.keys[part], ld.info, part);
                    if (made_libraries.count(lh)) {
                        out.extra_libraries_done_before++;
                        continue;
                    }
                    if (!queued_libraries.insert(lh).second) continue;
                    Made m{lh, true, std::make_unique<PipelineEntry>(), true};
                    pipeline_workers_->queue(m.entry.get(), std::make_unique<PipelineDesc>(ld.info),
                                             PipelinePriority::normal);
                    made.push_back(std::move(m));
                }
            }
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
        if (m.extra) (ok ? out.extra_libraries : out.extra_libraries_failed)++;
        if (m.generic) (ok ? out.generic_libraries : out.generic_libraries_failed)++;
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
                          uint32_t primitive, bool tessellated, std::vector<ScreenVertex> *screen) const {
    if (screen) screen->clear();
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
            max_y = std::min(max_y, vertex_max_y(vs, constants, indices, primitive, screen));
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
                            uint32_t primitive, std::vector<ScreenVertex> *screen) const {
    // (screen: where each vertex that is not killed lands, for draw_reach;
    // left empty when the shader cannot be run for all of them)
    if (screen) screen->clear();
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
    float x_scale = vte & 1 ? vport[0] : 1.0f, x_offset = vte & 2 ? vport[1] : 0.0f;
    if (screen && !(vs.writes_position_xz & 1)) screen = nullptr;
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
        if (!shader::interpret(vs.parsed, input, constants, memory, output, unsupported)) {
            if (screen) screen->clear();
            return 8192;
        }
        if (vs.writes_vertex_kill && (std::bit_cast<uint32_t>(output.point_size[2]) & ~(1u << 31))) continue;
        float y = output.position[1];
        if (divide_by_w) y /= output.position[3];
        y = y * y_scale + y_offset;
        if (screen) {
            float x = output.position[0];
            // (behind the eye, a polygon is not within its vertices' box)
            if (divide_by_w) x = output.position[3] > 0.0f ? x / output.position[3] : NAN;
            screen->push_back({vertex, x * x_scale + x_offset, y});
        }
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

// Beyond Xenia (whose estimate is the lowest row alone, draw_max_y): the
// pixels of its render targets a draw can reach, as rectangles, for the
// targets to take over the tiles under them and no others (Impl::draw's
// acquire). Never less than the draw can write:
// - the scissor, down to draw_max_y's row, always;
// - with clipping on, polygons are clipped to the host's viewport: its
//   rectangle;
// - with clipping off, where the vertex shader runs on the CPU for the
//   draw's vertices (draw_max_y's own run when there is no scissor, else for
//   draws of at most twelve vertices: the clears and the screen-space
//   rectangles), the box of a polygon draw's vertices, none of them behind
//   the eye; for a rectangle list of up to four rectangles a box each, with
//   the corner the rasterizer adds, whichever of the three it completes.
// Points and lines (sprites and widths reach past their vertices) and
// tessellated draws keep the scissor's rectangle.
void Impl::draw_reach(const Shader &vs, const harness::Constants &constants, const std::vector<uint32_t> &indices,
                      uint32_t primitive, bool tessellated, const VkViewport &viewport, const VkRect2D &scissor,
                      std::vector<Reach> &reach) const {
    reach.clear();
    std::vector<ScreenVertex> &screen = reach_vertices_;
    const uint32_t left = uint32_t(scissor.offset.x), top = uint32_t(scissor.offset.y);
    const uint32_t right = left + scissor.extent.width;
    const uint32_t bottom =
        std::min(top + scissor.extent.height, draw_max_y(vs, constants, indices, primitive, tessellated, &screen));
    const Reach all{left, top, right, bottom};
    const bool polygons = (primitive >= 4 && primitive <= 8) || (primitive >= 13 && primitive <= 15);
    if (!reach_tiles_ || !polygons || bottom <= top || right <= left) return reach.push_back(all);
    // A box of screen positions (in the console's pixels, the window offset
    // and the pixel centers' half added) as the pixels with a sample inside
    // it, within the scissor; nothing when the box is not finite. A host
    // pixel's samples are at least a sixteenth of it from its edges (the
    // standard positions and the Xenos's), and the host's own vertices are
    // taken to be within a 512th of a host pixel (the rasterizer's snapping)
    // and a 1024th of a console pixel (the arithmetic) of these: so a pixel
    // the box enters by less than `inside` has no sample in it, and an edge
    // on a pixel boundary (a clear's, a viewport's) takes no pixel beyond.
    // Polygons drawn as lines or points (the polygon mode) reach a pixel
    // past their edges.
    const double inside = (1.0 / 16.0 - 1.0 / 512.0) / double(scale_.factor()) - 1.0 / 1024.0;
    const double past = primitive != 8 && ((regs_[r::PA_SU_SC_MODE_CNTL] >> 3) & 3) == 1 ? 1.0 : 0.0;
    auto add = [&](double x0, double y0, double x1, double y1) {
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) || !(inside > 0.0))
            return false;
        Reach r = all;
        r.x0 = uint32_t(std::clamp(std::floor(x0 + inside - past), double(left), double(right)));
        r.y0 = uint32_t(std::clamp(std::floor(y0 + inside - past), double(top), double(bottom)));
        r.x1 = uint32_t(std::clamp(std::ceil(x1 - inside + past), double(left), double(right)));
        r.y1 = uint32_t(std::clamp(std::ceil(y1 - inside + past), double(top), double(bottom)));
        reach.push_back(r);
        return true;
    };
    if (!((regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1)) {
        if (!add(viewport.x, viewport.y, double(viewport.x) + viewport.width, double(viewport.y) + viewport.height))
            reach.push_back(all);
        return;
    }
    if (tessellated) return reach.push_back(all);
    // (draw_max_y ran the shader only where there is no scissor)
    if (screen.empty() && indices.size() <= 12) vertex_max_y(vs, constants, indices, primitive, &screen);
    if (screen.empty()) return reach.push_back(all);
    double add_x = 0.0, add_y = 0.0;
    if (!(regs_[r::PA_SU_VTX_CNTL] & 1)) add_x += 0.5, add_y += 0.5;
    if ((regs_[r::PA_SU_SC_MODE_CNTL] >> 16) & 1) {
        const uint32_t window = regs_[r::PA_SC_WINDOW_OFFSET];
        add_x += sign_extend(window & 0x7FFF, 15), add_y += sign_extend((window >> 16) & 0x7FFF, 15);
    }
    if (primitive == 8 && indices.size() <= 12 && indices.size() % 3 == 0) {
        // a box a rectangle (vertex_max_y's vertices are the indices
        // clamped as the draw's, sorted)
        const uint32_t min_index = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
        bool each = true;
        for (size_t k = 0; k < indices.size() && each; k += 3) {
            double x[3], y[3];
            for (size_t i = 0; i < 3 && each; i++) {
                const uint32_t index = std::min(max_index, std::max(min_index, indices[k + i] & 0xFFFFFF));
                auto at = std::lower_bound(screen.begin(), screen.end(), index,
                                           [](const ScreenVertex &v, uint32_t value) { return v.index < value; });
                if (indices[k + i] == UINT32_MAX || at == screen.end() || at->index != index) each = false;
                else x[i] = at->x + add_x, y[i] = at->y + add_y;
            }
            if (!each) break;
            // The fourth corner mirrors the vertex opposite the longest edge
            // (rectangle_list.geom). Three corners of an axis-aligned box:
            // the diagonal is the longest however the axes are scaled, and
            // the fourth is the box's. Else opposite any of the three.
            const double bx0 = std::min({x[0], x[1], x[2]}), bx1 = std::max({x[0], x[1], x[2]});
            const double by0 = std::min({y[0], y[1], y[2]}), by1 = std::max({y[0], y[1], y[2]});
            uint32_t corners = 0;
            for (int i = 0; i < 3; i++)
                if ((x[i] == bx0 || x[i] == bx1) && (y[i] == by0 || y[i] == by1))
                    corners |= 1u << ((x[i] == bx1 ? 1 : 0) | (y[i] == by1 ? 2 : 0));
            if (std::popcount(corners) == 3 && bx1 > bx0 && by1 > by0) {
                each = add(bx0, by0, bx1, by1);
                continue;
            }
            const double fx[3] = {x[1] + x[2] - x[0], x[0] + x[2] - x[1], x[0] + x[1] - x[2]};
            const double fy[3] = {y[1] + y[2] - y[0], y[0] + y[2] - y[1], y[0] + y[1] - y[2]};
            each = add(std::min({bx0, fx[0], fx[1], fx[2]}), std::min({by0, fy[0], fy[1], fy[2]}),
                       std::max({bx1, fx[0], fx[1], fx[2]}), std::max({by1, fy[0], fy[1], fy[2]}));
        }
        if (each) return;
        reach.clear();
        // (a killed vertex or a position that is not finite: the scissor's)
        return reach.push_back(all);
    }
    if (primitive == 8) return reach.push_back(all);
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const ScreenVertex &v : screen) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) return reach.push_back(all);
        x0 = std::min(x0, double(v.x)), x1 = std::max(x1, double(v.x));
        y0 = std::min(y0, double(v.y)), y1 = std::max(y1, double(v.y));
    }
    if (!add(x0 + add_x, y0 + add_y, x1 + add_x, y1 + add_y)) reach.push_back(all);
}

// Beyond Xenia: whether a draw is the console's clear of its depth target,
// one rectangle that writes the depth and the stencil of every sample it
// covers whatever they held, and which pixels and word then. Only what is
// certain: anything else answers no (`why`), and the draw's tiles are
// transferred as always.
// - one rectangle (a rectangle list of three vertices), not tessellated, not
//   predicated on a VIZ query nor a survey draw, no user clip planes, no
//   vertex kill;
// - the depth test on, always passing and written; the stencil test on,
//   always passing, replacing (or zeroing) with the whole write mask, so
//   that no bit of the old word is left;
// - nothing in the pixel shader that drops a sample: no kill, no oDepth, no
//   alpha test other than always, no alpha to coverage;
// - the vertex shader run on the CPU (as draw_max_y's) gives three corners
//   of an axis-aligned rectangle with W 1 and one Z; the pixels are those
//   wholly inside it, the scissor and the viewport (a thirty-second of a
//   pixel of slack: sample positions are no nearer than a sixteenth to a
//   pixel's edge), so every sample of them at any sample count and
//   resolution scale;
// - the word is known where the host's depth is one value whatever the
//   order and the contraction of the operations that make it (each
//   intermediate exact as a float: the clears' 0 and 1 are), with no polygon
//   offset, and it is the target's own encoding of a guest word (a 24-bit
//   unorm host depth rounds without doubt; a float one is exactly the
//   word's value).
Impl::DepthOverwrite Impl::depth_overwrite(const gpu::Draw &d, const Shader &vs, const Shader *ps,
                                           const harness::Constants &constants, const std::vector<uint32_t> &indices,
                                           uint32_t depth_control, const RenderTarget &depth, const VkViewport &viewport,
                                           const float *ndc_scale, const float *ndc_offset, const VkRect2D &scissor,
                                           const char *&why) const {
    DepthOverwrite o;
    auto no = [&](const char *reason) { return why = reason, o; };
    if (indices.size() != 3) return no("not one rectangle");
    if ((d.viz_query & 0x100) || viz_predicate_ != UINT32_MAX || (regs_[reg::PA_SC_VIZ_QUERY] & 1))
        return no("a VIZ query");
    const uint32_t clip = regs_[r::PA_CL_CLIP_CNTL], vte = regs_[r::PA_CL_VTE_CNTL];
    const bool clipping = !((clip >> 16) & 1);
    if (clipping && (clip & 0x3F)) return no("user clip planes");
    if ((depth_control & 0x76) != 0x76) return no("depth not always written");
    // stencil: on, always, the pass operation replace (2) or zero (1), every bit written
    const uint32_t stencil_state = regs_[r::RB_STENCILREFMASK];
    const uint32_t pass = (depth_control >> 14) & 7;
    // (the stencil test off: the stencil is kept, the depth alone overwritten)
    const bool stencil_kept = !(depth_control & 1);
    if (!stencil_kept) {
        if (((depth_control >> 8) & 7) != 7 || (pass != 1 && pass != 2) || ((stencil_state >> 16) & 0xFF) != 0xFF)
            return no("stencil not replaced");
        // (the back face's own state, which the rectangle's triangles may take: the same)
        if (((depth_control >> 7) & 1) && (((depth_control >> 20) & 0xFFF) != ((depth_control >> 8) & 0xFFF) ||
                                           ((regs_[r::RB_STENCILREFMASK_BF] ^ stencil_state) & 0xFFFFFF)))
            return no("stencil not replaced");
    }
    const uint32_t stencil = !stencil_kept && pass == 2 ? stencil_state & 0xFF : 0;
    if (ps) {
        const uint32_t color_control = regs_[r::RB_COLORCONTROL];
        if (ps->summary.kills || ps->writes_depth) return no("the pixel shader kills or writes depth");
        if (((color_control & 8) && (color_control & 7) != 7) || (color_control & 16)) return no("alpha test or alpha to coverage");
    }
    if (depth.format == 1 && options_.depth_float24_convert_in_pixel_shader) return no("depth converted in the pixel shader");
    if (vs.fetches_textures || vs.writes_vertex_kill || !vs.writes_position_y || !vs.writes_position_w ||
        vs.writes_position_xz != 5)
        return no("the vertex shader");
    // the three corners on the screen, in the console's pixels, as the
    // host's vertex shader and viewport place them
    const uint32_t min_index = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
    std::span<const uint8_t> memory(memory_.data(), GuestMemory::SIZE);
    harness::CaseInput input{};
    harness::CaseOutput output;
    std::string unsupported;
    double x[3], y[3];
    float z = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (indices[i] == UINT32_MAX) return no("not one rectangle");
        input.registers[0][0] = float(std::min(max_index, std::max(min_index, indices[i] & 0xFFFFFF)));
        if (!shader::interpret(vs.parsed, input, constants, memory, output, unsupported)) return no("the vertex shader");
        // (W 1, given as W or as 1/W: the position's formats change nothing then)
        if (output.position[3] != 1.0f) return no("W is not 1");
        if (i && std::bit_cast<uint32_t>(output.position[2]) != std::bit_cast<uint32_t>(z)) return no("Z is not one value");
        z = output.position[2];
        const double ndc_x = double(output.position[0]) * ndc_scale[0] + ndc_offset[0];
        const double ndc_y = double(output.position[1]) * ndc_scale[1] + ndc_offset[1];
        x[i] = viewport.x + (ndc_x + 1.0) * 0.5 * viewport.width;
        y[i] = viewport.y + (ndc_y + 1.0) * 0.5 * viewport.height;
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) return no("the vertex shader");
    }
    (void)vte;
    double x0 = std::min({x[0], x[1], x[2]}), x1 = std::max({x[0], x[1], x[2]});
    double y0 = std::min({y[0], y[1], y[2]}), y1 = std::max({y[0], y[1], y[2]});
    // three different corners of that box: the fourth is the rectangle's
    uint32_t corners = 0;
    for (int i = 0; i < 3; i++) {
        if ((x[i] != x0 && x[i] != x1) || (y[i] != y0 && y[i] != y1)) return no("not axis-aligned");
        corners |= 1u << ((x[i] == x1 ? 1 : 0) | (y[i] == y1 ? 2 : 0));
    }
    if (std::popcount(corners) != 3 || !(x1 > x0) || !(y1 > y0)) return no("not axis-aligned");
    if (clipping) {
        // (clipped to the viewport)
        x0 = std::max(x0, double(viewport.x)), x1 = std::min(x1, double(viewport.x) + viewport.width);
        y0 = std::max(y0, double(viewport.y)), y1 = std::min(y1, double(viewport.y) + viewport.height);
    }
    const double slack = 1.0 / 32.0;
    const double left = std::max(std::ceil(x0 - slack), double(scissor.offset.x));
    const double top = std::max(std::ceil(y0 - slack), double(scissor.offset.y));
    const double right = std::min(std::floor(x1 + slack), double(scissor.offset.x) + scissor.extent.width);
    const double bottom = std::min(std::floor(y1 + slack), double(scissor.offset.y) + scissor.extent.height);
    if (!(left >= 0.0 && top >= 0.0 && right > left && bottom > top && right <= 16384.0 && bottom <= 16384.0))
        return no("covers nothing");
    // the host's depth: z * scale + offset, then the viewport's range, each
    // step exact (so the same however it is computed)
    auto exact = [](double v) { return double(float(v)) == v; };
    const double z_scaled = double(z) * ndc_scale[2], z_ndc = z_scaled + ndc_offset[2];
    const double range = double(viewport.maxDepth) - viewport.minDepth, z_ranged = z_ndc * range;
    double host = z_ranged + viewport.minDepth;
    const bool known = exact(z_scaled) && exact(z_ndc) && exact(range) && exact(z_ranged) && exact(host);
    const double low = std::min(viewport.minDepth, viewport.maxDepth), high = std::max(viewport.minDepth, viewport.maxDepth);
    if (clipping) {
        // inside the clip volume, or the rectangle is clipped away (or its
        // depth is not known to be inside)
        if (!known || !(z_ndc >= 0.0 && z_ndc <= 1.0)) return no("the depth may be clipped");
    } else {
        host = std::min(std::max(host, low), high); // (the depth clamp)
    }
    o.depth_whole = true, o.whole = !stencil_kept;
    o.word = stencil; // (the stencil written, when whole; the depth's bits below, once known)
    o.x0 = uint32_t(left), o.y0 = uint32_t(top), o.x1 = uint32_t(right), o.y1 = uint32_t(bottom);
    why = "the depth is not one known word";
    if (!known || !(host >= 0.0 && host <= 1.0)) return o;
    // no polygon offset (a rectangle's is the front's, with the offset of
    // points and lines enabled)
    if ((regs_[r::PA_SU_SC_MODE_CNTL] >> 13) & 1) {
        const float *offsets = reinterpret_cast<const float *>(&regs_[r::PA_SU_POLY_OFFSET_FRONT_SCALE]);
        if (offsets[0] != 0.0f || offsets[1] != 0.0f) return o;
    }
    uint32_t d24;
    if (depth.format == 0) {
        const double scaled = host * 16777215.0;
        d24 = uint32_t(std::llround(scaled));
        if (std::fabs(scaled - double(d24)) > 1.0 / 64.0) return o;
    } else {
        d24 = float_to_float24(float(host * 2.0), options_.depth_float24_round);
    }
    const uint32_t word = d24 << 8 | stencil;
    // (a float host depth holds the draw's value itself, which transfers keep)
    if (depth.image.format != VK_FORMAT_D24_UNORM_S8_UINT && double(depth_clear_value(depth.format, word).depthStencil.depth) != host)
        return o;
    o.known_depth = true, o.constant = o.whole, o.word = word;
    why = stencil_kept ? "the stencil kept" : nullptr;
    return o;
}

// A draw outside copy mode, as the processor gives it (immediate indices in
// the draw, which Xenia does not draw).
// A draw's front part (DrawFront): false when the draw ends there.
bool Impl::draw_front(const gpu::Draw &d, DrawFront &f) {
    const uint32_t primitive = f.primitive, count = f.count;
    const bool index32 = f.index32, color = f.color, writes_targets = f.writes_targets;
    (void)count, (void)index32;
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
                // (by its text: the literals' count is kept by address, and this one is on the stack)
                use(std::string(what));
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
        if (host_vertex.host == HV::Vertex) return skip("tessellation: " + what), false;
        // the device's tessellation shaders are optional (most mobile GPUs
        // lack them)
        if (!d_.tessellation_shader()) return skip("tessellation without tessellation shaders: " + what), false;
        // adaptive tessellation's edge factors are 32-bit floats in an index
        // buffer (Xenia fails the draw otherwise)
        if (tess_mode == 2 && !(d.indexed && d.index32))
            return skip("adaptive tessellation without 32-bit edge factors"), false;
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
            return skip("user clip planes without clip or cull distances on the device"), false;
        if (points && host_vertex.point_ucp_mode == 2 && !host_vertex.clip_planes_cull && host_vertex.clip_planes > 4)
            approximate("points with ps_ucp_mode 2 and more than 4 user clip planes (clipped by the corners only)");
    }
    if ((expand_in_vs_ || ucp) && (rectangles || points))
        host_vertex.host =
            points ? shader::HostVertex::PointListAsTriangleStrip : shader::HostVertex::RectangleListAsTriangleStrip;
    bool expanded = host_vertex.host == shader::HostVertex::PointListAsTriangleStrip ||
                    host_vertex.host == shader::HostVertex::RectangleListAsTriangleStrip;
    // in a screen extent report, the vertex shader bounding it where its
    // positions are the final ones (not points or rectangles a geometry
    // shader expands: a sprite's corners, a rectangle's fourth corner)
    const uint32_t extent_words = (!points && !rectangles) || expanded ? extent_draw_words() : UINT32_MAX;
    host_vertex.extents = extent_words != UINT32_MAX;
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
    default: return skip(std::string("primitive ") + primitive_name(primitive)), false;
    }
    // the types the reset index cuts (Xenia's ProcessIndexBuffer list, by the
    // guest's type: tessellated strips and fans too, patches not, adaptive
    // tessellation's edge factors not)
    bool strip = (primitive == 3 || primitive == 5 || primitive == 6 || primitive == 12 || primitive == 14 ||
                  primitive == 15 || primitive == 0x15 || primitive == 0x16) &&
                 !(tessellated && tess_mode == 2);

    // shaders
    Shader *vs = have_vertex_ ? vertex_shader_ : nullptr;
    if (!vs) return skip("no vertex shader"), false;
    Shader *ps = color && have_pixel_ ? pixel_shader_ : nullptr;
    std::string why;
    // a shader's forms the generic shaders draw may be translated on the
    // workers, the draw drawn by them meanwhile (translating)
    // (translating: a form the draw uses is; the first forms may be
    // translating for another draw's sake)
    bool translating = false, first_forms_translating = false;
    if (!translate(*vs, why, &first_forms_translating)) return skip("vertex shader: " + why), false;
    if (ps && !translate(*ps, why, &first_forms_translating)) return skip("pixel shader: " + why), false;
    for (const Shader *s : {vs, ps})
        if (s)
            for (const std::string &a : s->approximate) report_.approximate[a]++, approximations_++;

    // render targets
    uint32_t surface = regs_[reg::RB_SURFACE_INFO], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
    if (!pitch) return skip("surface pitch 0"), false; // Xenia draws nothing then, exports neither
    if (msaa == 3) return skip("MSAA mode 3"), false;
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
            return false;
        }
    }
    RenderTarget *colors[4] = {};
    uint32_t color_masks[4] = {};
    float color_scales[4] = {1, 1, 1, 1};
    uint32_t gamma_targets = 0;
    bool any_gamma = false;
    uint32_t mask = regs_[reg::RB_COLOR_MASK];
    // the interlock path makes no render targets: their places in EDRAM
    // (edram_view), for the draw's extent; only what the draw reads of each
    // (a RenderTarget each, made and destroyed at every draw, was a tenth of
    // the draw's own time)
    struct View {
        uint32_t width, height, format;
    };
    View views[5];
    bool viewed[5] = {};
    auto view_of = [&](bool is_depth, uint32_t base, uint32_t format) {
        const RenderTarget t = edram_view(is_depth, base, pitch, msaa, format);
        return View{t.width, t.height, t.format};
    };
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
                return skip(std::string("color target format ") + color_target_format_name(format)), false;
            views[i] = view_of(false, info & 0xFFF, format);
            viewed[i] = true;
            continue;
        }
        // the exponent bias, and 16-bit fixed (-32..32, 10 fraction bits) kept as snorm
        color_scales[i] = std::ldexp(1.0f, sign_extend((info >> 20) & 0x3F, 6));
        if (format == 4 || format == 5) color_scales[i] *= 1024.0f / 32767.0f;
        // (16_16 keeps no alpha: the shader's is the blend's factor only, and
        // the range's scale would make a blend by it 32 times too weak, as
        // Xenia's host path does; a negative scale tells the shader to leave
        // the alpha out of it. 16_16_16_16 stores its alpha, scaled: a blend
        // by the source's alpha there is still off, and counted)
        if (format == 4) color_scales[i] = -color_scales[i];
        colors[i] = render_target(false, info & 0xFFF, pitch, msaa, format);
        if (!colors[i]) {
            if (fatal_.empty()) skip(std::string("color target format ") + color_target_format_name(format));
            return false;
        }
        // 8_8_8_8_GAMMA: the shader writes linear into a target kept linear,
        // else the gamma curve (blended in gamma then, unlike the console)
        if (format == 1 && !colors[i]->gamma_linear) gamma_targets |= 1u << i;
        // 2_10_10_10_FLOAT: its alpha is two fixed bits, so the console takes
        // the source's alpha to 0..1 before it blends by it and stores it; the
        // host keeps the target as float, whose blend takes any alpha (as
        // Xenia's host path): the shader clamps it (the word's bits 4-7)
        if (format == 3 || format == 12) gamma_targets |= 16u << i;
    }
    if (any_gamma) use("draws into 8_8_8_8_GAMMA color targets");
    // (a pixel shader's export runs for each host pixel: as many times more
    // as the scale's area, to the same addresses; Xenia's the same)
    if (!scale_.one() && ps && ps->exports)
        approximate("pixel shader memory export at a resolution scale (run once a host pixel)");
    if (gamma_targets & 0xF)
        approximate("8_8_8_8_GAMMA color targets blended in gamma (no RGBA16 unorm attachments)");
    uint32_t depth_control = export_only ? 0 : regs_[reg::RB_DEPTHCONTROL];
    RenderTarget *depth = nullptr;
    if (depth_control & 3) {
        uint32_t info = regs_[reg::RB_DEPTH_INFO], format = (info >> 16) & 1;
        if (fsi_) {
            views[4] = view_of(true, info & 0xFFF, format);
            viewed[4] = true;
        } else {
            if (format == 1 && !options_.depth_float24_convert_in_pixel_shader)
                approximate("D24FS8 depth as 32-bit float");
            depth = render_target(true, info & 0xFFF, pitch, msaa, format);
            if (!depth) return false;
        }
    } else {
        depth_control = 0;
    }
    uint32_t width = 8192, height = 8192;
    // (and the pass's, at the resolution scale: the targets' images)
    uint32_t host_width = max_image_, host_height = max_image_;
    for (RenderTarget *t : colors)
        if (t) {
            width = std::min(width, t->width), height = std::min(height, t->height);
            host_width = std::min(host_width, t->host_width), host_height = std::min(host_height, t->host_height);
        }
    if (depth) {
        width = std::min(width, depth->width), height = std::min(height, depth->height);
        host_width = std::min(host_width, depth->host_width), host_height = std::min(host_height, depth->host_height);
    }
    for (uint32_t i = 0; i < 5; i++)
        if (viewed[i]) width = std::min(width, views[i].width), height = std::min(height, views[i].height);
    if (width == 8192 && height == 8192 && !export_only && !rasterizes_nothing()) {
        // no target the pixel shader writes and no depth: nothing it could change
        if (options_.verbose)
            std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
        return false;
    }

    // viewport and scissor
    bool depth_float24 = depth && depth->format == 1;
    // D24FS8 depth converted to 20e4 in the pixel shader (Options), for draws
    // with the depth test on, as Xenia's depth_float24_convert_in_pixel_shader
    bool float24_convert = options_.depth_float24_convert_in_pixel_shader && depth_float24 && (depth_control & 2);
    Viewport viewport = host_viewport(regs_, max_viewport_[0] / scale_.factor(), max_viewport_[1] / scale_.factor(), ps && ps->writes_depth,
                                      depth_float24 && (depth_control & 2), float24_convert);
    VkRect2D scissor = host_scissor(regs_, pitch);
    scissor.extent.width = std::min<uint32_t>(scissor.extent.width, width - std::min<uint32_t>(scissor.offset.x, width));
    scissor.extent.height = std::min<uint32_t>(scissor.extent.height, height - std::min<uint32_t>(scissor.offset.y, height));
    if (!export_only && (viewport.empty || !scissor.extent.width || !scissor.extent.height) && !rasterizes_nothing()) return false;
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
            f.rt_base[i] = ((info & 0xFFF) % EDRAM_TILES) * tile_dwords_;
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
        f.depth_base = (depth_base % EDRAM_TILES) * tile_dwords_;
        f.pitch = std::max(1u, (pitch * (msaa >= 2 ? 2 : 1) + 79) / 80) * tile_dwords_;
        f.scale[0] = scale_.q4, f.scale[1] = tile_dwords_;
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
        // (the slope's per host pixel: the depth changes 1/scale as much
        // from one to the next, Xenia's poly_offset_scale_factor)
        f.poly_offset[0] *= scale_.factor() / 16.0f, f.poly_offset[2] *= scale_.factor() / 16.0f;
        std::memcpy(f.blend_constant, &regs_[r::RB_BLEND_RED], 16);
    }
    // (the targets take over their EDRAM tiles below, once the draw's lowest
    // row is known: draw_max_y)

    f.tessellated = tessellated;
    f.tess_mode = tess_mode;
    f.host_vertex = host_vertex;
    f.patch_points = patch_points;
    f.hull = hull;
    f.draw_name = draw_name;
    f.rectangles = rectangles;
    f.points = points;
    f.ucp = ucp;
    f.expanded = expanded;
    f.polygonal = polygonal;
    f.topology = topology;
    f.strip = strip;
    f.vs = vs;
    f.ps = ps;
    f.why = why;
    f.translating = translating;
    f.first_forms_translating = first_forms_translating;
    f.pitch = pitch;
    f.msaa = msaa;
    f.export_only = export_only;
    f.gamma_targets = gamma_targets;
    f.depth_control = depth_control;
    f.depth = depth;
    f.width = width;
    f.height = height;
    f.host_width = host_width;
    f.host_height = host_height;
    f.depth_float24 = depth_float24;
    f.float24_convert = float24_convert;
    f.viewport = viewport;
    f.scissor = scissor;
    f.fsi_constants = fsi_constants;
    std::copy(colors, colors + 4, f.colors);
    std::copy(color_masks, color_masks + 4, f.color_masks);
    std::copy(color_scales, color_scales + 4, f.color_scales);
    f.extent_words = extent_words;
    return true;
}

void Impl::draw(const gpu::Draw &d) {
    report_.draws++;
    // (XF_DRAW_SECTIONS=1: the draw's time by section, draw_sections.h; the last closed on the way out)
    struct SectionsEnd {
        ~SectionsEnd() { XF_DRAW_SECTION(Record); }
    };
    std::optional<SectionsEnd> sections_end;
    if (DrawSections::on()) DrawSections::get().begin(), DrawSections::get().draw(), sections_end.emplace();
    // (XF_DRAW_LIMIT=N: the draws after the Nth left out; XF_DRAW_SKIP=A-B: the draws from the Ath to
    // the Bth left out: for finding the one a wrong picture comes from)
    if (draw_limit_ && report_.draws > draw_limit_) return;
    if (report_.draws >= draw_skip_[0] && report_.draws <= draw_skip_[1]) return;
    // VIZ queries ("VIZ queries" above), counted: draws predicated on one
    // (DRAW_INDX's VIZ token, bit 8), and survey draws (PA_SC_VIZ_QUERY's
    // viz_query_ena with kill_pix_post_hi_z), drawn as they are; without
    // conditional rendering, every predicated draw is drawn (Xenia's default)
    if (d.viz_query & 0x100)
        use(viz_predicates_.buffer ? "draws predicated on a VIZ query"
                                   : "draws predicated on a VIZ query (drawn, as Xenia's default)");
    if ((regs_[reg::PA_SC_VIZ_QUERY] & 0x81) == 0x81) use("VIZ survey draws");
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
        const Shader *v = have_vertex_ ? vertex_shader_ : nullptr;
        const Shader *p = have_pixel_ ? pixel_shader_ : nullptr;
        bool exports = (v && v->summary.memexport) || (color && p && p->summary.memexport);
        if (!writes_targets && !exports) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
        // predicated on a VIZ query that ended: dropped when it had no
        // survey, else on the GPU by its word when that holds its count
        if ((d.viz_query & 0x100) && viz_predicates_.buffer && !exports) {
            const VizQuery &q = viz_[d.viz_query & 63];
            if (q.ended && !q.surveyed) {
                use("draws predicated on a VIZ query without survey draws (dropped)");
                return;
            }
            if (q.ended && q.armed) viz_predicate_ = d.viz_query & 63;
        }
        // the port's observer sees every draw; the port draws this frame
        // (native_render.h): the draws its filter selects skipped, never one
        // exporting memory (the CPU may read it)
        if (native_suppress_ || native_settings_.observer) {
            native::PassInfo pass;
            pass.color = color, pass.has_pixel_shader = p != nullptr;
            pass.primitive = primitive, pass.count = count;
            pass.vertex_shader = have_vertex_ ? vertex_hash_ : 0, pass.pixel_shader = have_pixel_ ? pixel_hash_ : 0;
            native_pass(pass);
            if (native_settings_.observer) native_settings_.observer(pass);
            if (native_suppress_) {
                if (exports) {
                    native::detail::bump(native::detail::atomic_counters().memexport_kept);
                } else if (!native_keep(pass)) {
                    if (zpd_current_) zpd_counts_[zpd_current_].suppressed = true;
                    return;
                }
            }
        }
    }
    // what the textures' and vertex data's sections look up first, asked of the memory now
    prefetched_mask_ = 0;
    if (draw_prefetch_ && have_vertex_ && vertex_shader_)
        prefetch_draw(d, vertex_shader_, have_pixel_ ? pixel_shader_ : nullptr);
    // the front part: the draw before's where this one comes with the same
    // state, shaders and kind of primitive, else worked out
    collect_translations();
    DrawFront &f = front_;
    bool front_hit = false; // (the front part the draw before's: the pipeline's memo may stand)
    {
        DrawFrontKey key;
        key.state_serial = file_ ? file_->state_serial() : 0, key.foreign_writes = file_ ? file_->foreign_writes() : 0;
        key.vertex = have_vertex_ ? vertex_shader_ : nullptr, key.pixel = have_pixel_ ? pixel_shader_ : nullptr;
        key.primitive = primitive;
        key.flags = uint32_t(d.major_mode_explicit) | uint32_t(d.indexed) << 1 | uint32_t(index32) << 2 |
                    uint32_t(color) << 3 | uint32_t(writes_targets) << 4;
        // (the screen extent report the draw bounds is the draw's own)
        const bool was_bounded = f.host_vertex.extents;
        bool hit = front_memo_ && front_valid_ && file_ && key == front_key_ && !native_suppress_ && !options_.verbose;
        if (hit) {
            f.extent_words = (!f.points && !f.rectangles) || f.expanded ? extent_draw_words() : UINT32_MAX;
            if ((f.extent_words != UINT32_MAX) != was_bounded) hit = false;
        }
        front_hit = hit;
        if (hit) {
            XF_DRAW_TALLY(FrontHits);
            for (const auto &[what, approximation] : front_counts_) approximation ? approximate(what) : use(what);
            for (const Shader *s : {front_key_.vertex, f.color ? front_key_.pixel : nullptr})
                if (s)
                    for (const std::string &a : s->approximate) report_.approximate[a]++, approximations_++;
        } else {
            front_valid_ = false;
            front_generation_++;
            front_counts_.clear();
            front_recording_ = front_keepable_ = true;
            f.primitive = primitive, f.count = count, f.index32 = index32, f.color = color;
            f.writes_targets = writes_targets;
            const bool goes_on = draw_front(d, f);
            front_recording_ = false;
            if (!goes_on) return;
            // kept for the next draw: not one whose shaders are still being
            // translated, nor one that exports alone
            front_valid_ = front_memo_ && front_keepable_ && file_ && !f.translating && !f.first_forms_translating &&
                           !f.export_only && !native_suppress_ && !options_.verbose;
            front_key_ = key;
        }
    }
    XF_DRAW_SECTION(Front);
    bool tessellated = f.tessellated;
    uint32_t tess_mode = f.tess_mode;
    shader::VertexOptions host_vertex = f.host_vertex;
    uint32_t patch_points = f.patch_points;
    uint32_t hull = f.hull;
    std::string draw_name = f.draw_name;
    bool rectangles = f.rectangles;
    bool points = f.points;
    uint32_t ucp = f.ucp;
    bool expanded = f.expanded;
    bool polygonal = f.polygonal;
    VkPrimitiveTopology topology = f.topology;
    bool strip = f.strip;
    Shader *vs = f.vs;
    Shader *ps = f.ps;
    std::string why = f.why;
    bool translating = f.translating;
    uint32_t msaa = f.msaa;
    bool export_only = f.export_only;
    uint32_t gamma_targets = f.gamma_targets;
    uint32_t depth_control = f.depth_control;
    RenderTarget *depth = f.depth;
    uint32_t host_width = f.host_width;
    uint32_t host_height = f.host_height;
    bool depth_float24 = f.depth_float24;
    bool float24_convert = f.float24_convert;
    const Viewport &viewport = f.viewport;
    VkRect2D scissor = f.scissor;
    FsiConstants fsi_constants = f.fsi_constants;
    const auto &colors = f.colors;
    const auto &color_masks = f.color_masks;
    const auto &color_scales = f.color_scales;
    const uint32_t extent_words = f.host_vertex.extents ? f.extent_words : UINT32_MAX;
    host_vertex.extents = extent_words != UINT32_MAX;
    // the pages of the streams the shaders export to (the pixel shader's only
    // when it runs)
    std::vector<Range> &exported = draw_lists_.exported;
    exported.clear();
    export_ranges(*vs, 0, exported);
    if (ps) export_ranges(*ps, 1, exported);
    bool exports = vs->exports || (ps && ps->exports);

    // indices: the guest's, swapped, plus the index offset, cut to 24 bits
    // and clamped to VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX (as Xenia's vertex
    // shaders do it: RemapAndConvertVertexIndices), reset indices kept as the
    // host's restart index; adaptive tessellation's edge factors only swapped
    uint32_t offset = regs_[r::VGT_INDX_OFFSET];
    bool restart = strip && (regs_[r::PA_SU_SC_MODE_CNTL] >> 21) & 1;
    uint32_t reset_index = regs_[r::VGT_MULTI_PRIM_IB_RESET_INDX];
    const uint32_t min_index = regs_[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs_[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
    auto remap = [&](uint32_t index) { return std::min(max_index, std::max(min_index, (index + offset) & 0xFFFFFF)); };
    const bool factors = tessellated && tess_mode == 2;
    auto vertex = [&](uint32_t index) { return factors ? index : remap(index); };
    // An index buffer's converted before and unwritten since (the index
    // cache): its words as they are, in the cache's buffer. Not a draw whose
    // list is made here (exports alone, patches, expanded points and
    // rectangles) nor indices that wrap past the end of memory.
    IndexEntry *index_entry = nullptr;
    bool index_hit = false, index_keep = false;
    uint32_t index_checked = 0;
    {
        const uint32_t base = d.index_base & GuestMemory::PHYSICAL_MASK;
        const uint64_t bytes = uint64_t(count) * (index32 ? 4 : 2);
        if (index_cache_on_ && d.indexed && d.immediate.empty() && !export_only && !tessellated && !expanded &&
            base + bytes <= GuestMemory::SIZE && uint64_t(count) * 6 <= INDEX_CHUNK / 4) {
            IndexKey key;
            key.base = base, key.count = count;
            const uint32_t converted = primitive >= 12 && primitive <= 15 ? primitive : 0;
            key.form = uint32_t(index32) | uint32_t(d.index_endian) << 1 | uint32_t(restart) << 4 | converted << 8;
            key.reset = restart ? reset_index : 0, key.offset = offset, key.min = min_index, key.max = max_index;
            // (over its budget: everything dropped, to be kept again as it is drawn)
            if (index_chunks_.size() * INDEX_CHUNK + index_cache_words_ > INDEX_CACHE_BUDGET ||
                index_cache_.size() > INDEX_CACHE_ENTRIES)
                index_cache_drop();
            IndexEntry *&front = index_front_[IndexKeyHash()(key) & (INDEX_FRONT - 1)];
            if (!front || !(front->key == key)) {
                auto found = index_cache_.find(key);
                if (found == index_cache_.end()) found = index_cache_.emplace(key, IndexEntry{}).first, found->second.key = key;
                front = &found->second;
            }
            index_entry = front;
            IndexEntry &e = *index_entry;
            const uint64_t swap = report_.swaps;
            if (e.buffer) {
                const uint32_t now = mem_.write_count();
                if (now == e.checked || pages_unwritten(e.first, e.last, e.checked)) {
                    e.checked = now;
                    index_hit = true;
                } else {
                    e.streak = swap - e.written_swap <= 4 ? e.streak + 1 : 1;
                    e.written_swap = swap;
                    if (e.streak >= 3) {
                        // written again and again: not kept for a while
                        index_cache_words_ -= e.words.size() * 4;
                        e.words = {};
                        e.buffer = VK_NULL_HANDLE;
                        e.dynamic_until = swap + 300;
                        e.streak = 0;
                    }
                }
            }
            // kept from the second swap it is drawn in: indices made anew
            // each frame somewhere else (a ring of dynamic geometry) are
            // never watched, a watch costing the game a fault at its next write
            const bool seen_before = e.seen_swap && e.seen_swap != swap + 1;
            e.seen_swap = swap + 1;
            if (!index_hit && seen_before && swap >= e.dynamic_until) {
                // watched, then the count, then the words read (memory.h)
                e.first = base >> page_shift_, e.last = uint32_t((base + bytes - 1) >> page_shift_);
                mem_.watch(base, uint32_t(bytes));
                index_checked = mem_.write_count();
                index_keep = true;
            }
        }
    }
    std::vector<uint32_t> &indices = index_hit ? index_entry->words : draw_lists_.indices;
    if (index_hit) {
        traced(d.index_base, count * (index32 ? 4 : 2));
    } else if (d.indexed && !d.immediate.empty()) {
        indices.clear();
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
        traced(d.index_base, count * size);
        // Each step a loop of its own over the whole list, its choices made
        // outside it (the index buffer of a heavy scene's draws was a fifth
        // of the command thread, a push_back and every branch per index):
        // the guest's words in host order as the fetch's endianness says,
        // then the reset indices kept and the rest offset and clamped.
        // (one pass, index_convert.h; read where guest memory holds them,
        // copied first only when they wrap past its end)
        indices.resize(count);
        IndexConversion conversion;
        conversion.index32 = index32;
        conversion.endian = uint32_t(endian);
        conversion.restart = restart;
        conversion.reset = index32 ? reset_index : reset_index & 0xFFFF;
        conversion.remap = !factors;
        conversion.offset = offset, conversion.min = min_index, conversion.max = max_index;
        const size_t bytes = size_t(count) * size;
        const uint32_t base = d.index_base & GuestMemory::PHYSICAL_MASK;
        const uint8_t *in = memory_.data() + base;
        if (base + bytes > GuestMemory::SIZE) {
            std::vector<uint8_t> &raw_indices = draw_lists_.raw_indices;
            raw_indices.resize(bytes);
            memory_.read(d.index_base, raw_indices);
            in = raw_indices.data();
        }
        convert_indices(in, indices.data(), count, conversion);
    } else {
        indices.resize(count);
        uint32_t *out = indices.data();
        if (factors)
            for (uint32_t i = 0; i < count; i++) out[i] = i;
        else
            for (uint32_t i = 0; i < count; i++) out[i] = remap(i);
    }
    if (index_hit) {
        // (converted when the entry was made)
    } else if (export_only) {
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
        // Quads to triangle pairs split along the 1-3 diagonal, as Xenia's
        // geometry shaders make them (both backends' kQuadList: a strip of
        // vertices 0, 1, 3, 2, as GL_QUAD_STRIP; the console's own split is
        // not known): (0, 1, 3) and (1, 2, 3), each in the quad's winding,
        // their first vertices the strip's provoking ones. A quad whose
        // attributes are not affine over it (a billboard in perspective)
        // interpolates differently on either side of the diagonal, so the
        // other split moves alpha tested and alpha to coverage edges.
        std::vector<uint32_t> &triangles = draw_lists_.triangles;
        triangles.resize(indices.size() / 4 * 6);
        const uint32_t *quad = indices.data();
        uint32_t *triangle = triangles.data();
        for (size_t q = 0; q + 3 < indices.size(); q += 4, quad += 4, triangle += 6) {
            triangle[0] = quad[0], triangle[1] = quad[1], triangle[2] = quad[3];
            triangle[3] = quad[1], triangle[4] = quad[2], triangle[5] = quad[3];
        }
        indices.swap(triangles);
        if (indices.empty()) return;
    } else if (primitive == 12 || primitive == 14 || primitive == 15) {
        // each primitive between reset indices on its own: a line loop closed
        // by its first index (2 vertices give two lines, 1 nothing: Xenia's
        // GetLineLoopStripIndexCount); a quad strip's whole quads; a polygon
        // of at least a triangle
        std::vector<uint32_t> &converted = draw_lists_.converted;
        converted.clear();
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
        indices.swap(converted);
        if (indices.empty()) return;
    }
    if (index_keep) {
        // into the cache: the words and their place in its buffers, or the
        // entry's as they were where they came out the same
        IndexEntry &e = *index_entry;
        if (!e.buffer || e.words != indices) {
            index_cache_words_ += indices.size() * 4;
            index_cache_words_ -= e.words.size() * 4;
            e.words = indices;
            if (index_cache_place(indices.size() * 4, e.buffer, e.at))
                upload(static_cast<uint8_t *>(index_chunks_.back().buffer.mapped) + e.at, indices.data(),
                            indices.size() * 4);
            else
                e.buffer = VK_NULL_HANDLE;
        }
        e.checked = index_checked;
        if (!e.buffer) index_entry = nullptr;
    } else if (!index_hit) {
        index_entry = nullptr;
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

    XF_DRAW_SECTION(Indices);
    // the textures and samplers the prefetched slot memos name (in the caches by now), prefetched
    // in turn
    for (uint64_t left = prefetched_mask_; left; left &= left - 1) {
        const uint32_t index = prefetched_slots_[std::countr_zero(left)].index;
        for (const SlotMemo *m : {&fetch_memos_[index], &fetch_memos_[index ^ 1]})
            if (m->texture) __builtin_prefetch(m->texture), __builtin_prefetch(m->sampler);
    }
    // constants
    for (int stage = 0; stage < 2; stage++) {
        harness::Constants &c = last_constants_[stage];
        const Shader *s = stage == 0 ? vs : ps;
        if (!constant_stamps_ || !file_) {
            // copied whole, uploaded again where they differ from the last upload
            harness::Constants now;
            std::memcpy(now.float_constants, &regs_[reg::ALU_CONSTANTS + stage * 1024], 4096);
            std::memcpy(now.bool_constants, &regs_[reg::BOOL_CONSTANTS], sizeof(now.bool_constants));
            std::memcpy(now.loop_constants, &regs_[reg::LOOP_CONSTANTS], sizeof(now.loop_constants));
            std::memcpy(now.fetch_constants, &regs_[reg::FETCH_CONSTANTS], sizeof(now.fetch_constants));
            if (std::memcmp(&c, &now, sizeof(now)) != 0) c = now, last_constants_at_[stage] = NONE;
            constants_file_[stage] = nullptr;
            continue;
        }
        // Only the blocks of registers written to another value since the kept copy was made
        // (the 5 KB of each stage copied, compared and copied again at every draw were a sixth
        // of the command thread)
        using File = gpu::RegisterFile;
        const File &file = *file_;
        const uint64_t foreign = file.foreign_writes();
        const bool kept = constants_file_[stage] == file_ && constants_foreign_[stage] == foreign;
        const uint64_t since = constants_serial_[stage], serial = file.serial();
        bool changed = false;
        const uint32_t alu = reg::ALU_CONSTANTS + stage * 1024;
        // (a stage none of whose float constants was written since: one comparison, not its 32 blocks')
        const bool floats_written = !kept || file.float_constants_serial(stage) > since;
        for (uint32_t first = 0; floats_written && first < 1024; first += File::BLOCK)
            if (!kept || file.changed_since(alu + first, File::BLOCK, since)) {
                std::memcpy(&c.float_constants[first / 4], &regs_[alu + first], File::BLOCK * 4);
                changed = true;
                constants_dirty_[stage] |= 1ull << (first / File::BLOCK);
            }
        if (!kept || file.changed_since(reg::BOOL_CONSTANTS, 8 + 32, since)) {
            std::memcpy(c.bool_constants, &regs_[reg::BOOL_CONSTANTS], sizeof(c.bool_constants));
            std::memcpy(c.loop_constants, &regs_[reg::LOOP_CONSTANTS], sizeof(c.loop_constants));
            changed = true;
            constants_dirty_[stage] |= 1ull << 63;
        }
        // the fetch constants: the blocks the stage's shader reads brought up to date; the others
        // are no longer current once written
        static_assert(reg::FETCH_CONSTANTS % File::BLOCK == 0 && (32 * 6) % File::BLOCK == 0);
        uint32_t current = kept ? constants_fetch_blocks_[stage] : 0u;
        const uint32_t wanted = s ? s->fetch_blocks : 0u;
        for (uint32_t b = 0; b < 32 * 6 / File::BLOCK; b++) {
            const uint32_t first = b * File::BLOCK;
            if ((current & (1u << b)) && file.changed_since(reg::FETCH_CONSTANTS + first, File::BLOCK, since))
                current &= ~(1u << b);
            if ((wanted & (1u << b)) && !(current & (1u << b))) {
                std::memcpy(&c.fetch_constants[first], &regs_[reg::FETCH_CONSTANTS + first], File::BLOCK * 4);
                current |= 1u << b;
                changed = true;
                constants_dirty_[stage] |= 1ull << (32 + b);
            }
        }
        if (changed) last_constants_at_[stage] = NONE;
        constants_file_[stage] = file_, constants_foreign_[stage] = foreign;
        constants_serial_[stage] = serial, constants_fetch_blocks_[stage] = current;
    }

    XF_DRAW_SECTION(Constants);
    // the targets take over the EDRAM tiles down to the lowest row the draw
    // can reach (draw_max_y), within the scissor
    {
        // (the tiles under the pixels the draw can reach, draw_reach; with
        // XF_DRAW_REACH=rows every tile of the rows from the first down to
        // the lowest, as Xenia takes them)
        draw_reach(*vs, last_constants_[0], indices, primitive, tessellated, viewport.viewport, scissor, reach_);
        auto take = [&](RenderTarget *t, const DepthOverwrite *overwritten) {
            std::vector<TileRect> &rects = reach_rects_;
            rects.clear();
            for (const Reach &r : reach_) {
                TileRect tiles = tiles_of(*t, r.x0, r.y0, r.x1, r.y1);
                if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
                if (!reach_tiles_) tiles.c0 = 0, tiles.c1 = t->pitch_tiles, tiles.r0 = 0;
                rects.push_back(tiles);
            }
            acquire(t, std::span<const TileRect>(rects), overwritten);
            for (TileRect tiles : rects) {
                if (!reach_tiles_) tiles.r0 = uint32_t(scissor.offset.y) / t->tile_height;
                tiles_drawn(*t, tiles);
            }
        };
        for (RenderTarget *t : colors)
            if (t) take(t, nullptr);
        if (depth) {
            // the console's clear of the depth target (depth_overwrite): the
            // tiles it overwrites are taken without their transfer, and hold
            // a clear's value after it where it writes one word
            DepthOverwrite over;
            if (rectangles && !export_only && (overwrite_transfers_ || draw_clears_) && (depth_control & 0x76) == 0x76) {
                const char *why = nullptr;
                over = depth_overwrite(d, *vs, ps, last_constants_[0], indices, depth_control, *depth, viewport.viewport,
                                       viewport.ndc_scale, viewport.ndc_offset, scissor, why);
                if (options_.verbose)
                    std::fprintf(stderr, "  draw %llu: a depth rectangle, %s%s (%u, %u)-(%u, %u) word %08X\n",
                                 (unsigned long long)report_.draws, over.whole ? "overwrites" : "not an overwrite: ",
                                 why ? why : "", over.x0, over.y0, over.x1, over.y1, over.word);
            }
            take(depth, over.depth_whole ? &over : nullptr);
            // the stencil of the tiles reached: not known any more with the
            // stencil test on, but the one written where the draw replaces
            // all of a tile's (tile_stencil_)
            if (depth_control & 1)
                for (const TileRect &tiles : reach_rects_) tiles_stencil_written(*depth, tiles);
            if (over.depth_whole && fatal_.empty()) {
                const uint32_t tiles = std::min(depth->height / depth->tile_height * depth->pitch_tiles, EDRAM_TILES);
                bool cleared = false, stencil = false;
                for (uint32_t k = 0; k < tiles; k++) {
                    const uint32_t tile = (depth->base + k) % EDRAM_TILES;
                    if (owners_[tile] != depth || !tile_within(*depth, tile, over)) continue;
                    if (over.whole) tile_stencil_[tile] = int16_t(over.word & 0xFF), stencil = true;
                    // (one depth and one stencil: a clear's, as after a resolve's clear)
                    if (!draw_clears_ || !over.known_depth || tile_stencil_[tile] < 0) continue;
                    const uint32_t word = (over.word & ~0xFFu) | uint32_t(tile_stencil_[tile]);
                    tile_cleared_[tile] = 1, tile_clear_word_[tile] = word;
                    tile_clear_value_[tile] = depth_clear_value(depth->format, word);
                    cleared = true;
                }
                if (cleared) cleared_epoch_++;
                if (stencil) stencil_epoch_++;
            }
        }
        if (!fatal_.empty()) return;
    }
    System sys{};
    for (int i = 0; i < 3; i++) sys.ndc_scale[i] = viewport.ndc_scale[i], sys.ndc_offset[i] = viewport.ndc_offset[i];
    sys.ndc_scale[3] = 1.0f;
    if (host_vertex.extents || vertex_snap_) {
        // the host's NDC to the viewport's console pixels (for the extents and the vertex snapping)
        const VkViewport &v = viewport.viewport;
        sys.extent_map[0] = v.width * 0.5f, sys.extent_map[1] = v.x + v.width * 0.5f;
        sys.extent_map[2] = v.height * 0.5f, sys.extent_map[3] = v.y + v.height * 0.5f;
        if (host_vertex.extents) sys.extent_report[0] = extent_words;
    }
    uint32_t color_control = regs_[r::RB_COLORCONTROL];
    if (color_control & 8) sys.flags[0] = 8 | (color_control & 7);
    sys.values[0] = as_float(regs_[r::RB_ALPHA_REF]);
    // the host pixel's size in console pixels: the pixel parameters' position
    // is the console's pixel (shader_translator.cpp's param_gen)
    sys.values[3] = 1.0f / scale_.factor();
    uint32_t vte = regs_[r::PA_CL_VTE_CNTL];
    sys.flags[1] = ((vte >> 10) & 1) | ((vte >> 8) & 1) << 1 | ((vte >> 9) & 1) << 2;
    // a small target at a resolution scale: the interpolators at the console
    // pixel's center (console_centers_)
    if (console_centers_ && !scale_.one() && ps) {
        const uint32_t pitch = regs_[reg::RB_SURFACE_INFO] & 0x3FFF;
        const uint32_t tl = regs_[r::PA_SC_WINDOW_SCISSOR_TL], br = regs_[r::PA_SC_WINDOW_SCISSOR_BR];
        const uint32_t width = (br & 0x7FFF) - std::min(br & 0x7FFF, tl & 0x7FFF);
        const uint32_t height = ((br >> 16) & 0x7FFF) - std::min((br >> 16) & 0x7FFF, (tl >> 16) & 0x7FFF);
        if ((pitch && pitch <= console_centers_) || width <= console_centers_ || height <= console_centers_) {
            sys.flags[1] |= 1u << 16;
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: interpolators at the console's centers (pitch %u, scissor %ux%u)\n",
                             (unsigned long long)report_.draws, pitch, width, height);
        }
    }
    // the vertex snapping's mode (shader::VertexOptions::vertex_snap): PA_SU_VTX_CNTL's round_mode, quant_mode
    if (vertex_snap_)
        sys.flags[1] |= ((regs_[r::PA_SU_VTX_CNTL] >> 1) & 3) << 24 | ((regs_[r::PA_SU_VTX_CNTL] >> 3) & 7) << 26;
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

    XF_DRAW_SECTION(Targets);
    // textures: per slot the shaders read, an image's index in the table of
    // its dimension and a sampler's
    // (a mask of the 32 texture fetch constants, in order)
    uint64_t texture_slots = vs->texture_mask | (ps ? ps->texture_mask : 0);
    if (prefetched_vs_ != vs || prefetched_ps_ != ps) prefetched_mask_ = 0;
    for (uint64_t left = texture_slots; left; left &= left - 1) {
        const uint32_t slot = uint32_t(std::countr_zero(left));
        const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS + slot * 6];
        uint64_t miss_t0 = 0;
        if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture) {
            approximate("a texture slot without a texture constant");
            continue;
        }
        // the first fetch's filters over the constant's, the vertex shader's first
        std::array<uint32_t, 4> overrides = {3, 3, 3, 7};
        for (Shader *s : {vs, ps})
            if (s && slot < 32 && (s->filter_mask & (1u << slot))) {
                overrides = s->slot_filters[slot];
                break;
            }
        // (the prefetch's entry where it hashed the same filters: a shader's are known once it
        // is analyzed, which a new shader is only within the draw)
        const bool prefetched = ((prefetched_mask_ >> slot) & 1) && prefetched_slots_[slot].overrides == overrides;
        // What the slot gave the last draw that read it, again: the same constant's words and
        // filters, no texture, view or sampler destroyed or loaded since (texture_epoch_), and
        // the texture's memory unwritten. The constant is not parsed, nor the texture, its view
        // and its sampler looked up (three maps, 7% of the command thread).
        const uint32_t forced_filter = texture_filter_.load(std::memory_order_relaxed);
        const uint32_t forced_aniso = anisotropy_.load(std::memory_order_relaxed);
        SlotMemo *memo = !slot_memos_on_ || trace_ ? nullptr
                         : prefetched              ? fetch_memo(fetch, overrides, prefetched_slots_[slot].index)
                                                   : fetch_memo(fetch, overrides);
        if (memo) {
            if (done_serial_ != limits_applied_at_) apply_texture_limits(); // (as texture() begins)
            SlotMemo &m = *memo;
            if (m.epoch == texture_epoch_ && std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0 &&
                m.overrides == overrides && m.forced_filter == forced_filter && m.forced_aniso == forced_aniso &&
                m.loads == m.texture->loads && !m.texture->stale && unwritten(*m.texture)) {
                XF_DRAW_TALLY(SlotHits);
                m.used = report_.draws;
                use(*m.texture, submission_serial() + 1);
                m.sampler->used_serial = submission_serial();
                sys.slots[slot] = m.slot_word, sys.signs[slot] = m.signs;
                sys.integer_scale[slot] = m.integer_scale, sys.signed_slots[slot] = m.signed_word;
                if (m.uses & 1) use("textures read with the integer number format (scaled to the guest's integers)");
                if (m.uses & 2)
                    use("4-6 bit unsigned components of point-filtered textures (rebuilt as the console expands them)");
                continue;
            }
            XF_DRAW_TALLY(SlotMisses);
            if (DrawSections::on()) {
                // (why: a new constant or one replaced, another in its entry, its texture loaded again
                // or stale, its memory written)
                miss_t0 = FastClock::now();
                const bool same = std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0 && m.overrides == overrides;
                DrawSections::get().tally(m.epoch != texture_epoch_       ? DrawSections::MissEpoch
                                          : !same                         ? DrawSections::MissFetch
                                          : m.loads != m.texture->loads   ? DrawSections::MissLoads
                                          : m.texture->stale              ? DrawSections::MissStale
                                                                          : DrawSections::MissWritten);
            }
            m.epoch = 0; // (kept again below, when the slot comes out whole)
        }
        const uint64_t approximations = approximations_;
        uint32_t uses = 0;
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
        uint32_t sampler_bits = sampler(fetch, overrides) << 16;
        SamplerEntry *const sampler_entry = last_sampler_;
        sys.slots[slot] = index | t->kind << 14 | sampler_bits;
        sys.signs[slot] = texture::swizzled_signs(info);
        // (XF_TEXTURE_NUMBER_FORMAT=0: none of it, to measure what it costs)
        static const bool number_format = [] {
            const char *v = std::getenv("XF_TEXTURE_NUMBER_FORMAT");
            return !(v && v[0] == '0');
        }();
        sys.integer_scale[slot] = number_format ? texture::integer_scale_bits(info, sys.signs[slot]) : 0u;
        if (sys.integer_scale[slot] & ((1u << 24) - 1)) {
            if (!(sys.integer_scale[slot] & (1u << 24))) {
                use("textures read with the integer number format (scaled to the guest's integers)");
                uses |= 1;
            } else {
                for (uint32_t k = 0; k < 4; k++)
                    if (((sys.integer_scale[slot] >> (6 * k)) & 0x3F) - 1 < 15) {
                        use("4-6 bit unsigned components of point-filtered textures (rebuilt as the console expands them)");
                        uses |= 2;
                        break;
                    }
            }
        }
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
        if (miss_t0) DrawSections::get().tally(DrawSections::MissTicks, FastClock::now() - miss_t0);
        // kept for the next draw, when nothing about it was counted as approximate (those counts
        // are per draw) and its sampler is one of the table's
        if (memo && approximations == approximations_ && sampler_entry && !t->stale) {
            SlotMemo &m = *memo;
            m.epoch = texture_epoch_;
            std::memcpy(m.fetch, fetch, sizeof(m.fetch));
            m.overrides = overrides, m.forced_filter = forced_filter, m.forced_aniso = forced_aniso;
            m.texture = t, m.loads = t->loads, m.sampler = sampler_entry;
            m.slot_word = sys.slots[slot], m.signs = sys.signs[slot];
            m.integer_scale = sys.integer_scale[slot], m.signed_word = sys.signed_slots[slot];
            m.uses = uses, m.used = report_.draws;
        }
    }

    // Expanded in the vertex shader (shader::HostVertex): a strip of two
    // triangles a point or rectangle, cut by the restart index, as Xenia's
    // built-in index buffer of two-triangle strips. A point's four host
    // vertices are its index shifted left by 2 plus the corner; a
    // rectangle's are numbered from 4 times its number, each with the
    // rectangle's three guest indices as its attribute (guest_indices).
    XF_DRAW_SECTION(Textures);
    std::vector<uint32_t> &guest_indices = draw_lists_.guest_indices;
    guest_indices.clear();
    if (expanded) {
        std::vector<uint32_t> &strips = draw_lists_.strips;
        strips.clear();
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
        indices.swap(strips);
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        restart = true;
    }

    // the pipeline
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
    VkFormat color_formats[4] = {};
    VkPipelineColorBlendAttachmentState blends[4] = {};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool generic = false;
    uint32_t centroid = 0;
    const uint32_t sc_mode = regs_[r::PA_SU_SC_MODE_CNTL];
    // The draw before's pipeline and what the draw takes of its description,
    // again: its front part was the one this draw found (DrawFront: the same
    // draw state, shaders and kind of primitive), so everything the key, the
    // description and the lookup are worked out from is as it was then
    // (XF_PIPELINE_MEMO=0: worked out at every draw).
    // The interlock path's draws without a pixel shader that write depth one
    // way alone, without the stencil and outside occlusion queries, in a run:
    // unordered, in a pass of their own (fsi_depth_unordered_fs_) once the
    // run is long enough, or where the pass under way is such a one already.
    uint32_t fsi_unordered = 0;
    if (fsi_ && !export_only) {
        const uint32_t flags = fsi_constants.flags, fn = (flags / FSI_DEPTH_LESS) & 7;
        uint32_t direction = 0;
        if (!ps && (flags & FSI_DEPTH_STENCIL) && (flags & FSI_DEPTH_WRITE) && !(flags & FSI_STENCIL) &&
            !zpd_current_ && viz_survey_ == UINT32_MAX)
            direction = fn == 1 || fn == 3 ? 1 : fn == 4 || fn == 6 ? 2 : 0;
        if (direction && direction == fsi_depth_run_.direction)
            fsi_depth_run_.count++;
        else
            fsi_depth_run_ = {direction, direction ? 1u : 0u};
        if (direction && fsi_depth_run_min_ &&
            (fsi_depth_run_.count >= fsi_depth_run_min_ || (in_pass_ && pass_.unordered == direction))) {
            fsi_unordered = direction;
            use("interlock path's depth draws without the interlock");
        }
    }
    PipelineMemo &pm = pipeline_memo_;
    if (pipeline_memo_on_ && front_hit && pm.valid && pm.front_generation == front_generation_ &&
        pm.fsi_unordered == fsi_unordered) {
        for (const auto &[what, approximation] : pm.counts) approximation ? approximate(what) : use(what);
        std::copy(pm.blends, pm.blends + 4, blends);
        raster = pm.raster, depth_state = pm.depth_state;
        multisample.rasterizationSamples = pm.samples;
        fragment = pm.fragment, pipe = pm.pipe;
        if (pm.premultiplied) {
            std::copy(pm.premultiply, pm.premultiply + 4, sys.blend_premultiply);
            std::memcpy(sys.blend_constant, &regs_[r::RB_BLEND_RED], 16);
        }
        XF_DRAW_TALLY(PipelineMemoHits);
    } else {
        pm.valid = false;
        pipeline_counts_.clear();
        pipeline_recording_ = pipeline_keepable_ = true;
        pipeline_final_ = false;
        struct RecordingEnd {
            bool &recording;
            ~RecordingEnd() { recording = false; }
        } recording_end{pipeline_recording_};
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
        std::vector<uint32_t> &key = draw_lists_.key;
        key.assign({uint32_t(vertex_hash_), uint32_t(vertex_hash_ >> 32), ps ? uint32_t(pixel_hash_) : 0,
                    ps ? uint32_t(pixel_hash_ >> 32) : 0, topology_class, uint32_t(rectangles), uint32_t(msaa),
                    depth ? uint32_t(depth->image.format) : 0u, regs_[r::PA_CL_CLIP_CNTL] & (1u << 16), host_key});
        std::vector<uint32_t> &color_key = draw_lists_.color_key; // the targets' part of the key
        color_key.clear();
        // (the factors of a minimum or maximum the pixel shader multiplies in: host_blend_control)
        uint32_t premultiply[4] = {};
        bool premultiplied = false;
        for (uint32_t i = 0; i < 4; i++) {
            if (!colors[i]) continue;
            color_formats[i] = colors[i]->image.format;
            // (the factors of a minimum or maximum, where the host can: host_blend_control)
            bool factors_ignored = false;
            uint32_t blend = host_blend_control(regs_[r::RB_BLENDCONTROL[i]], colors[i]->format,
                                                ps && !fsi_ && !export_only, premultiply[i], factors_ignored);
            if (factors_ignored) approximate("minimum and maximum blending with factors (the host ignores them)");
            if (premultiply[i]) {
                premultiplied = true;
                use("minimum and maximum blending with factors multiplied in by the pixel shader");
            }
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
        // A 16_16_16_16 target blended by the source's alpha ("A 16_16 target's
        // alpha", renderer.md): its alpha is stored, so it is scaled by the fixed
        // range, and so is the blend's factor, which should not be: counted (a
        // second source for the blend would carry the alpha unscaled)
        for (uint32_t i = 0; i < 4; i++) {
            if (!colors[i] || colors[i]->format != 5 || !blends[i].blendEnable) continue;
            auto by_alpha = [](VkBlendFactor f) {
                return f == VK_BLEND_FACTOR_SRC_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            };
            const VkPipelineColorBlendAttachmentState &b = blends[i];
            if (by_alpha(b.srcColorBlendFactor) || by_alpha(b.dstColorBlendFactor) || by_alpha(b.srcAlphaBlendFactor) ||
                by_alpha(b.dstAlphaBlendFactor))
                approximate("a 16_16_16_16 target blended by the source's alpha (scaled with the target's range)");
        }
        key.insert(key.end(), color_key.begin(), color_key.end());
        if (premultiplied) {
            std::copy(premultiply, premultiply + 4, sys.blend_premultiply);
            std::memcpy(sys.blend_constant, &regs_[r::RB_BLEND_RED], 16);
        }
        // (a form being translated on the workers stands as the generic shader's
        // module until the pipeline, which the generic shaders' then is)
        VkShaderModule guest_vs = vertex_module(*vs, host_vertex, why, &translating);
        if (!guest_vs && translating) guest_vs = generic_vs_[0][0];
        if (!guest_vs) return skip("vertex shader: " + why);
        // The pixel shader: the guest's, its float24 depth conversion form with
        // float24_convert; a depth-only draw that tests or writes depth then gets
        // one that converts the rasterized depth (Xenia's float24_truncate and
        // _round fragment shaders). The conversion runs per sample under MSAA, as
        // Xenia's (unless the shader writes oDepth, which is per pixel).
        fragment = ps ? (ps->translated ? ps->module : generic_fs_[0][0]) : VK_NULL_HANDLE;
        bool fragment_pending = ps && !ps->translated; // its first form, unless another is taken below
        uint32_t pixel_form = 0;                       // its form's key (shader::PixelOptions), for the generic shaders
        uint32_t float24_mode = float24_convert ? (options_.depth_float24_round ? 2u : 1u) : 0u;
        // The interpolators read at the centroid under MSAA, as xenos.h's
        // GetInterpolatorSamplingPattern takes SQ_CONTEXT_MISC's sc_sample_cntl (bits 2-3:
        // 0 centroids only, 1 centers only, 2 both, by SQ_INTERPOLATOR_CNTL's
        // sampling_pattern: a bit each, 1 the center). Xenia keeps only the
        // interpolators the pixel shader reads; here all 16, as the shader
        // loads them all.
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
        // Xenia's DepthStencilMode::kEarlyHint (pipeline_cache.cc): on the host
        // path, without a float24 conversion, a shader that may write depth early
        // (Shader::implicit_early_z_write_allowed: no kill, no oDepth, no memory
        // export) is given its early form (PixelOptions::early_depth_stencil)
        // where the coverage does not depend on its alpha (draw_util's
        // DoesCoverageDependOnAlpha: an alpha test other than always, or alpha to
        // coverage), or it does not write oC0. XF_EARLY_DEPTH_STENCIL=0: never.
        static const bool early_allowed = [] {
            const char *v = std::getenv("XF_EARLY_DEPTH_STENCIL");
            return !(v && v[0] == '0');
        }();
        const uint32_t color_control_now = regs_[r::RB_COLORCONTROL];
        const bool early = early_allowed && ps && !fsi_ && !export_only && !float24_convert && !ps->summary.kills &&
                           !ps->writes_depth && !ps->exports &&
                           (!(ps->colors_written & 1) ||
                            !(((color_control_now & 8) && (color_control_now & 7) != 7) || (color_control_now & 16)));
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
                pixel_form = options.key();
                fragment_pending = false;
                fragment = pixel_module(*ps, options, why, &translating);
                if (!fragment && translating) fragment = generic_fs_[0][0];
                if (!fragment) return skip("pixel shader: " + why);
            } else {
                fragment = fsi_constants.flags & FSI_DEPTH_STENCIL
                               ? (fsi_unordered ? fsi_depth_unordered_fs_ : fsi_depth_fs_)
                               : VK_NULL_HANDLE;
            }
            if (!fragment) {
                if (options_.verbose)
                    std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
                return;
            }
        } else if (ps && (float24_convert || centroid || point_coordinates || early || premultiplied)) {
            shader::PixelOptions options;
            options.depth_float24 = uint8_t(float24_mode);
            options.early_depth_stencil = early;
            options.blend_premultiply = premultiplied;
            options.centroid = uint16_t(centroid);
            options.point_coordinates = point_coordinates;
            pixel_form = options.key();
            fragment_pending = false;
            fragment = pixel_module(*ps, options, why, &translating);
            if (!fragment && translating) fragment = generic_fs_[0][0];
            if (!fragment) return skip("pixel shader: " + why);
        }
        if (fragment_pending) translating = true;
        // the interlock path's state as the pipeline's specialization
        // (Options::fsi_specialized; edram_fsi.frag's constant_id 0-9)
        uint32_t spec_data[10] = {};
        VkSpecializationMapEntry spec_entries[10];
        VkSpecializationInfo spec{};
        if (fsi_ && fragment) {
            key.push_back(0xF5000000u | fsi_ | (options_.fsi_specialized ? 0x100u : 0u) | fsi_unordered << 9);
            if (options_.fsi_specialized) {
                spec_data[0] = 1, spec_data[1] = fsi_constants.flags;
                for (uint32_t i = 0; i < 4; i++)
                    spec_data[2 + i] = fsi_constants.rt[i], spec_data[6 + i] = fsi_constants.blend[i];
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
                      (point_coordinates ? 16u : 0u) | (early ? 32u : 0u) | (premultiplied ? 64u : 0u) |
                      centroid << 16);
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
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1.0f;
        bool triangles = primitive >= 4 && !rectangles;
        if (triangles) {
            raster.cullMode = (sc_mode & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (sc_mode & 2 ? VK_CULL_MODE_BACK_BIT : 0);
            raster.frontFace = sc_mode & 4 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
        raster.polygonMode = polygon_mode; // (above, with the key)
        raster.depthClampEnable = (regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1;
        raster.depthBiasEnable = depth ? VK_TRUE : VK_FALSE; // the polygon offset, set at the draw
        multisample.rasterizationSamples = msaa == 0   ? VK_SAMPLE_COUNT_1_BIT
                                           : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT
                                                       : VK_SAMPLE_COUNT_4_BIT;
        // (the interlock path's guest 2x as 4x where passes without attachments
        // take no 2x: edram_fsi.frag's F_2X_AS_4X)
        if (fsi_ && msaa == 1 && fsi_2x_as_4x_) multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        // The interlock path rasterizes at the Xenos's sample positions where the
        // device places samples (VK_EXT_sample_locations), as Xenia's interlock
        // paths always do; host render targets do with
        // Options::xenos_sample_positions (Xenia's option of that name, off by
        // default there, on here): xenos_sample_locations.
        VkSampleLocationEXT sample_positions[4];
        VkPipelineSampleLocationsStateCreateInfoEXT sample_locations{
            VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
        if (msaa && (fsi_ ? (d_.sample_locations() & multisample.rasterizationSamples) != 0 : xenos_at(msaa))) {
            xenos_sample_locations(msaa, multisample.rasterizationSamples, fsi_, sample_positions, sample_locations);
            multisample.pNext = &sample_locations;
        }
        if (sample_shading) multisample.sampleShadingEnable = VK_TRUE, multisample.minSampleShading = 1.0f;
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
            // (the back face's own only for polygons, as Xenia's: a point, line or rectangle has no
            // facing on the console and takes the front's, whichever way the host's triangles of it
            // face: a rectangle list clearing depth where the stencil says so otherwise passed
            // everywhere by the back face's test)
            depth_state.back = polygonal && ((depth_control >> 7) & 1) ? face(20) : depth_state.front;
        }
        VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend_state.attachmentCount = 4;
        blend_state.pAttachments = blends;
        const VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS,
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
        GenericStages generic_stages;
        std::string generic_why;
        const bool generic_wanted =
            options_.generic_only && ps; // (draws without a pixel shader are made at once anyway)
        if (generic_wanted &&
            !generic_info(info, *vs, ps, point_coordinates, host_vertex.key(), pixel_form, generic_stages, generic_why))
            report_.generic_refused[generic_why]++;
        if (generic_wanted && generic_why.empty()) {
            // (Options::generic_only: whole pipelines of the generic shaders, made at once)
            key.push_back(0x6E0E0000u);
            pipe = pipeline(key, generic_stages.info, false, pending, stand_in);
            generic = true;
        } else if (translating) {
            // a shader's form on the translation workers: the generic shaders'
            // pipeline alone, linked once their libraries are made
            if (generic_info(info, *vs, ps, point_coordinates, host_vertex.key(), pixel_form, generic_stages,
                             generic_why)) {
                std::unique_ptr<PipelineEntry> &slot = translating_pipelines_[key];
                if (!slot) slot = std::make_unique<PipelineEntry>();
                pipe = generic_stand_in(*slot, generic_stages.info, true);
                generic = pipe != VK_NULL_HANDLE;
            } else {
                report_.generic_refused[generic_why]++;
            }
            if (!pipe) return skip("pipeline not ready (shaders being translated)");
        } else {
            // the generic pipeline drawn while its shaders were translated
            if (!translating_pipelines_.empty()) {
                auto it = translating_pipelines_.find(key);
                if (it != translating_pipelines_.end()) {
                    retire_generic(*it->second);
                    translating_pipelines_.erase(it);
                }
            }
            pipe = pipeline(key, info, options_.async_pipelines && ps, pending, stand_in);
            // waiting for the pipeline and its exact stand-in: the generic one
            if (pending && generic_on_ && !options_.generic_only) {
                if (generic_info(info, *vs, ps, point_coordinates, host_vertex.key(), pixel_form, generic_stages,
                                 generic_why)) {
                    auto entry = pipelines_.find(key);
                    if (entry != pipelines_.end()) pipe = generic_stand_in(*entry->second, generic_stages.info, true);
                    if (pipe) pending = false, generic = true;
                } else {
                    report_.generic_refused[generic_why]++;
                }
            }
        }
        if (pending) return skip("pipeline not ready (being made)");
        if (!pipe) return skip("vkCreateGraphicsPipelines failed");
        // (Options::generic_draws_a_frame) past the frame's share of the generic shaders' draws,
        // skipped as without them: their interpreter costs the GPU many times the translated
        // shaders' time (a frame of a scene's new shaders drawn by it alone, 300 ms of the GPU's
        // against 30 ms), and a wave of new shaders drawn by it held the GPU up for frames
        if (generic && !options_.generic_only && !generic_allowed())
            return skip("pipeline not ready (the generic shaders' GPU time for the frame spent)");
        if (stand_in) report_.stand_in_draws++;
        if (generic) {
            report_.generic_draws++;
            sys.generic[0] = vs->program;
            sys.generic[1] = ps ? ps->program : 0;
        }
        // kept for the next draw that finds this one's front part: a pipeline
        // found made, the shaders' own forms, nothing counted by a made string
        if (pipeline_memo_on_ && front_valid_ && pipeline_final_ && pipeline_keepable_ && !generic && !translating &&
            !stand_in && !pending) {
            pm.valid = true;
            pm.front_generation = front_generation_;
            pm.counts = pipeline_counts_;
            std::copy(blends, blends + 4, pm.blends);
            pm.raster = raster, pm.depth_state = depth_state;
            pm.raster.pNext = nullptr, pm.depth_state.pNext = nullptr;
            pm.samples = multisample.rasterizationSamples;
            pm.fragment = fragment, pm.pipe = pipe, pm.fsi_unordered = fsi_unordered;
            pm.premultiplied = premultiplied;
            std::copy(premultiply, premultiply + 4, pm.premultiply);
        }
    }

    XF_DRAW_SECTION(Pipeline);
    // the vertex data: the ranges of the vertex fetch constants the shaders
    // use (address and size, as Xenia's IssueDraw requests them), merged,
    // read from the mirror in device memory as Xenia's shaders read its
    // shared memory, guest memory's bytes put there where the CPU wrote them
    // since (request) and where a resolve or an export has not written newer
    // ones. (Read from host memory, the arena's copies, they made the
    // scene's vertex shaders wait on the bus: 6.7 ms of a UE3 game's race
    // frame.)
    std::vector<Range> &vertex_ranges = draw_lists_.vertex_ranges;
    vertex_ranges.clear();
    {
        // the slots both shaders fetch, in order (a mask: 96 vertex fetch
        // constants, three to a texture constant's six words)
        std::vector<Range> &sorted = draw_lists_.sorted;
        sorted.clear();
        const uint64_t fetch_slots[2] = {vs->vertex_mask[0] | (ps ? ps->vertex_mask[0] : 0),
                                         vs->vertex_mask[1] | (ps ? ps->vertex_mask[1] : 0)};
        // (the slots fetched, by their bits: a loop over all 128 was 1.6% of the command thread)
        for (uint32_t half = 0; half < 2; half++)
            for (uint64_t left = fetch_slots[half]; left; left &= left - 1) {
                const uint32_t slot = half * 64 + uint32_t(std::countr_zero(left));
                const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 2];
                // Xenia draws with any type here (its gpu_allow_invalid_fetch_constants
                // is on by default), warning about texture constants
                FetchConstantType type = FetchConstantType(f[0] & 3);
                if (type == FetchConstantType::Texture || type == FetchConstantType::InvalidTexture)
                    approximate("a vertex fetch from a texture fetch constant");
                uint32_t start = f[0] & 0x1FFFFFFC, words = (f[1] >> 2) & 0xFFFFFF;
                uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(start) + uint64_t(words) * 4, GuestMemory::SIZE));
                if (end > start) sorted.push_back({start, end});
            }
        std::sort(sorted.begin(), sorted.end());
        for (const Range &r : sorted) {
            if (!vertex_ranges.empty() && r.first <= vertex_ranges.back().second)
                vertex_ranges.back().second = std::max(vertex_ranges.back().second, r.second);
            else
                vertex_ranges.push_back(r);
        }
    }
    {
        Timer timer(report_.vertex_seconds, cpu_times_);
        if (range_memos_on_ && !trace_) {
            // the ranges found in the mirror before and unwritten since left out
            const uint32_t count = mem_.write_count();
            const uint64_t serial = submission_serial();
            std::vector<Range> &asked = draw_lists_.sorted; // (free again: the merge above is done)
            asked.clear();
            for (const Range &r : vertex_ranges) {
                RangeMemo &m = range_memos_[range_memo_index(r)];
                if (m.range == r && m.epoch == mirror_epoch_ &&
                    (m.count == count || pages_unwritten(first_page(r), last_page(r), m.count))) {
                    m.count = count;
                    if (hoisted_uploads_ && m.used_serial != serial) {
                        std::vector<Range> &one = draw_lists_.written;
                        one.assign(1, r);
                        mark_used(one);
                        m.used_serial = serial;
                    }
                    continue;
                }
                asked.push_back(r);
            }
            if (!asked.empty()) {
                const uint64_t epoch = mirror_epoch_;
                report_.vertex_bytes += request(asked);
                // (as of the count read before the look: a write since numbers a page above it;
                // not when the mirror could not take them)
                if (fatal_.empty() && epoch == mirror_epoch_)
                    for (const Range &r : asked) range_memos_[range_memo_index(r)] = {r, count, mirror_epoch_, submission_serial()};
            }
        } else
            report_.vertex_bytes += request(vertex_ranges, &vertex_memo_);
        // guest memory's bytes under the streams' pages, where the mirror has
        // nothing newer (Xenia's RequestRange of each stream)
        report_.vertex_bytes += request(exported);
    }
    if (!fatal_.empty()) return;

    XF_DRAW_SECTION(Vertex);
    // everything else the draw takes from the arena, in one submission: the
    // indices (none for a tessellated draw), the vertex attributes (its
    // control points, or an expanded rectangle's guest indices)
    if (tessellated) indices.clear();
    VkDeviceSize index_bytes = indices.size() * 4;
    const void *attributes = tessellated ? static_cast<const void *>(control_points.data()) : guest_indices.data();
    VkDeviceSize attribute_bytes = (tessellated ? control_points.size() : guest_indices.size()) * 4;
    VkDeviceSize constants_size = (sizeof(harness::Constants) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    VkDeviceSize system_size = (sizeof(System) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    // (the index cache's are in its buffers already)
    const bool cached_indices = index_entry && !tessellated;
    bool own_indices = index_bytes > ARENA_SLOT / 4, own_attributes = attribute_bytes > ARENA_SLOT / 4;
    // (the constants in the draw-data arena where there is one, the streams
    // in the arena)
    VkDeviceSize draw_need = 2 * constants_size + system_size + 2 * sizeof(FsiConstants) + 6 * ubo_align_;
    VkDeviceSize need = (draw_data_.buffer ? 0 : draw_need) + (own_indices || cached_indices ? 0 : index_bytes + 4) +
                        (own_attributes ? 0 : attribute_bytes + 4);
    bool fits = arena_take(need, 1) != NONE;
    if (fits) slots_[current_].used -= need; // only a check that the draw fits
    if (fits && draw_data_.buffer) {
        fits = draw_data_take(draw_need, 1) != NONE;
        if (fits) slots_[current_].draw_used -= draw_need;
    }
    if (!fits) {
        flush();
        // the draw reads its streams in the new submission
        mark_used(vertex_ranges);
        mark_used(exported);
    }
    if (!fatal_.empty()) return;
    // The submission the draw is in begun now: a flush above leaves the next
    // one unbegun, and the constants' offsets kept from the last draw
    // (last_*_at_, cleared as a submission begins) would otherwise still be
    // the flushed one's. A draw reusing them read that slot's share, which
    // the host writes again as soon as the slot records again, while this
    // submission may still be on the GPU: garbage constants (a lost device
    // with the constants in their own buffer, which fills a slot mid-frame).
    begin_recording();
    XF_DRAW_SECTION(Fit);

    // the interlock path's constants, with the occlusion counter the draw
    // counts in (the slot's, so once the flush above is past)
    // (the render pass the draw goes in, begun below)
    Pass pass;
    for (uint32_t i = 0; i < 4; i++) pass.colors[i] = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
    pass.depth = depth ? depth->view : VK_NULL_HANDLE;
    pass.width = export_only ? 1 : host_width, pass.height = export_only ? 1 : host_height;
    if (fsi_ && !export_only)
        pass.width = pass.height = std::min(scale_.up(8192), max_image_),
        pass.samples = uint32_t(multisample.rasterizationSamples), pass.unordered = fsi_unordered;
    if (fsi_ && fragment) {
        fsi_constants.zpd_index = viz_survey_ != UINT32_MAX ? viz_fsi_index() : fsi_zpd_index();
        // The early rejection (edram_fsi.glsl's xf_fsi_rejects): on where the
        // depth EDRAM holds under the draw can only have gone one way in its
        // render pass so far (the pass's draws' tests and color targets, this
        // one's own included; a new pass starts from what its barrier made
        // seen), the draw's test passes nothing that way, a fragment that
        // fails it changes nothing (the stencil's fail and depth fail
        // operations keep) and the shader neither writes oDepth nor exports.
        fsi_constants.scale[2] = 0;
        const uint32_t fn = (fsi_constants.flags / FSI_DEPTH_LESS) & 7;
        if (fsi_early_reject_ && (fsi_constants.flags & FSI_DEPTH_STENCIL) && fn != 7 &&
            (!ps || (!ps->writes_depth && !ps->exports))) {
            bool keeps = true;
            if (fsi_constants.flags & FSI_STENCIL)
                for (uint32_t face : {fsi_constants.stencil[1], fsi_constants.stencil[3]})
                    keeps &= ((face >> 3) & 7) == 0 && ((face >> 9) & 7) == 0;
            const bool same_pass = in_pass_ && !exports && pass_ == pass;
            const bool raised = same_pass && fsi_pass_.raised, lowered = same_pass && fsi_pass_.lowered;
            const uint32_t mode = !(fn & 4) && !raised ? 1 : !(fn & 1) && !lowered ? 2 : 0;
            if (keeps && mode) {
                const FsiTiles &tiles = fsi_draw_tiles(fsi_constants);
                if (!tiles.read.meets(tiles.drawn) && !(same_pass && tiles.read.meets(fsi_pass_.color))) {
                    fsi_constants.scale[2] = mode;
                    use("interlock draws rejecting before the shader");
                }
            }
        }
        if (last_fsi_at_ == NONE || std::memcmp(&last_fsi_, &fsi_constants, sizeof(FsiConstants)) != 0) {
            last_fsi_at_ = draw_data_take(sizeof(FsiConstants), ubo_align_);
            // (the kept copy first, the upload from it: the draw data is in
            // write-combined memory, which nothing must read back, and a
            // compiler free to copy the kept one from either did)
            last_fsi_ = fsi_constants;
            upload(draw_data(last_fsi_at_), &last_fsi_, sizeof(FsiConstants));
        }
    }
    uint32_t dynamic_offsets[4] = {0, 0, 0, last_fsi_at_ == NONE ? 0u : uint32_t(last_fsi_at_)};
    for (int stage = 0; stage < 2; stage++) {
        // The float constants the stage's shader reads, by blocks of 8 (Shader::float_blocks; Cemu
        // and RPCS3 upload a shader's own constants too): 4 KB of them went up at nearly every
        // draw for the few a shader reads. All of them for the generic shaders, which read by
        // the microcode's index. XF_CONSTANT_BLOCKS=0: all, always.
        // The fetch constants likewise, the blocks of 32 words its fetches' slots lie in
        // (Shader::fetch_blocks; the others are not kept current for it either).
        const Shader *reader = stage == 0 ? vs : ps;
        constexpr uint64_t ALL_FLOATS = 0xFFFFFFFFull, ALL_FETCH = 0x3Full << 32, ALL = ALL_FLOATS | ALL_FETCH;
        const uint64_t fetch = !fetch_blocks_ ? ALL_FETCH : reader ? uint64_t(reader->fetch_blocks) << 32 : 0;
        const uint64_t blocks = !constant_blocks_ || generic ? ALL : (reader ? reader->float_blocks : 0) | fetch;
        // (uploaded again when a draw found them changed, in each new submission, and where the
        // blocks there are not all this draw's)
        if (last_constants_at_[stage] == NONE || (blocks & ~last_constants_blocks_[stage])) {
            XF_DRAW_TALLY(ConstantUploadCount);
            if (DrawSections::on()) {
                const uint64_t dirty = constants_dirty_[stage];
                const uint64_t read = blocks | 1ull << 63;
                DrawSections::get().tally(DrawSections::UploadBlocks, std::popcount(read));
                DrawSections::get().tally(DrawSections::UploadDirtyBlocks, std::popcount(read & dirty));
                if (!(dirty & read & 0xFFFFFFFFull)) DrawSections::get().tally(DrawSections::UploadNoFloats);
                if (stage == 1) DrawSections::get().tally(DrawSections::UploadPixel);
            }
            constants_dirty_[stage] = 0;
            last_constants_at_[stage] = draw_data_take(sizeof(harness::Constants), ubo_align_);
            uint8_t *to = draw_data(last_constants_at_[stage]);
            const uint8_t *from = reinterpret_cast<const uint8_t *>(&last_constants_[stage]);
            if (blocks == ALL) {
                upload(to, from, sizeof(harness::Constants));
            } else {
                // each run of neighboring blocks in one copy
                constexpr size_t BLOCK = 8 * 16, FLOATS = sizeof(harness::Constants::float_constants);
                constexpr size_t FETCH = offsetof(harness::Constants, fetch_constants);
                static_assert(FLOATS == 32 * BLOCK && offsetof(harness::Constants, float_constants) == 0);
                static_assert(offsetof(harness::Constants, bool_constants) == FLOATS &&
                              sizeof(harness::Constants) == FETCH + 6 * BLOCK);
                auto runs = [](uint32_t mask, auto copy) {
                    while (mask) {
                        const uint32_t first = uint32_t(std::countr_zero(mask));
                        const uint32_t count = uint32_t(std::countr_one(mask >> first));
                        copy(first, count);
                        mask = count + first >= 32 ? 0u : mask & ~(((1u << count) - 1) << first);
                    }
                };
                runs(uint32_t(blocks), [&](uint32_t first, uint32_t count) {
                    upload(to + first * BLOCK, from + first * BLOCK, count * BLOCK);
                });
                // the bools and loops (unit 0), then the fetch blocks (units 1-6)
                runs(1u | uint32_t(blocks >> 32) << 1, [&](uint32_t first, uint32_t count) {
                    const size_t start = first ? FETCH + (first - 1) * BLOCK : FLOATS;
                    const size_t end = FETCH + (first + count - 1) * BLOCK;
                    upload(to + start, from + start, end - start);
                });
            }
            last_constants_blocks_[stage] = blocks;
        }
    }
    XF_DRAW_SECTION(ConstantUploads);
    // (all of the tables for the generic shaders, and with XF_CONSTANT_BLOCKS=0)
    const uint32_t system_slots = !constant_blocks_ || generic
                                      ? 32u
                                      : uint32_t(std::min(64 - std::countl_zero(texture_slots | 1), 32));
    if (last_system_at_ == NONE || system_slots > last_system_slots_ ||
        std::memcmp(&last_system_, &sys, sizeof(System)) != 0) {
        XF_DRAW_TALLY(SystemUploadCount);
        last_system_at_ = draw_data_take(sizeof(System), ubo_align_);
        last_system_ = sys;
        last_system_slots_ = system_slots;
        // Two draws in three upload them, most for the textures of their slots, and 512 of
        // the 784 bytes are the four tables of 32 slots, of which a draw's shaders read the
        // first few: each table goes up to the last slot the shaders fetch from, with what
        // lies before it (those there are a draw's whose shaders fetch no further).
        const uint32_t slots = system_slots;
        uint8_t *to = draw_data(last_system_at_);
        const uint8_t *from = reinterpret_cast<const uint8_t *>(&last_system_);
        if (slots == 32) {
            upload(to, from, sizeof(System));
        } else {
            auto piece = [&](size_t first, size_t end) { upload(to + first, from + first, end - first); };
            constexpr size_t SLOTS = offsetof(System, slots), FLAGS = offsetof(System, flags);
            constexpr size_t SIGNS = offsetof(System, signs), SIGNED = offsetof(System, signed_slots);
            constexpr size_t CLIP = offsetof(System, clip_planes), SCALE = offsetof(System, integer_scale);
            constexpr size_t REST = offsetof(System, blend_premultiply);
            static_assert(SLOTS < FLAGS && FLAGS < SIGNS && SIGNS < SIGNED && SIGNED < CLIP && CLIP < SCALE && SCALE < REST);
            piece(0, SLOTS + 4 * slots);          // the NDC transform, the slots
            piece(FLAGS, SIGNS + 4 * slots);      // flags, values, scales, the point, the signs
            piece(SIGNED, SIGNED + 4 * slots);    // the signed views
            piece(CLIP, SCALE + 4 * slots);       // clip planes, the extent's, the integer scales
            piece(REST, sizeof(System));          // the blend's, the generic shaders'
        }
    }
    // Every block the draw reads lies in its own submission's share: one in
    // another's is the host's to write again while this one runs (the lost
    // device above). A renderer bug, stopped here rather than on the GPU.
    {
        const VkDeviceSize share = draw_data_.buffer ? DRAW_DATA_SLOT : ARENA_SLOT;
        const VkDeviceSize fsi_at = fsi_ && fragment ? last_fsi_at_ : NONE;
        for (VkDeviceSize at : {last_constants_at_[0], last_constants_at_[1], last_system_at_, fsi_at})
            if (at != NONE && at / share != current_) {
                fatal_ = "a draw's constants lie outside its submission's share of the arena";
                return;
            }
    }
    XF_DRAW_SECTION(SystemUploads);
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
            upload(own.mapped, data, bytes);
            slots_[current_].after.push_back([this, own]() mutable { d_.destroy(own); });
            buffer = own.buffer, at = 0;
        } else {
            at = arena_take(bytes, 4);
            upload(arena_data(at), data, bytes);
            buffer = arena_.buffer;
        }
        return true;
    };
    VkBuffer index_buffer = VK_NULL_HANDLE, attribute_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_at = 0, attribute_at = 0;
    if (cached_indices)
        index_buffer = index_entry->buffer, index_at = index_entry->at;
    else if (index_bytes && !put_words(indices.data(), index_bytes, own_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       index_buffer, index_at))
        return;
    if (attribute_bytes && !put_words(attributes, attribute_bytes, own_attributes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      attribute_buffer, attribute_at))
        return;

    XF_DRAW_SECTION(Uploads);
    // record, in the render pass of these targets (none for exports alone);
    // a draw that exports begins its own, after a barrier, as what came
    // before in the pass may read the mirror where it writes
    if (exports) end_pass();
    VkCommandBuffer c = begin_pass(pass, colors, depth);
    if (fsi_ && fragment && !export_only) {
        // what the draw may do to depth and colors in EDRAM, for the early
        // rejection of the pass's later draws
        const uint32_t fn = (fsi_constants.flags / FSI_DEPTH_LESS) & 7;
        if ((fsi_constants.flags & FSI_DEPTH_STENCIL) && (fsi_constants.flags & FSI_DEPTH_WRITE))
            fsi_pass_.raised |= (fn & 4) != 0, fsi_pass_.lowered |= (fn & 1) != 0;
        fsi_pass_.color.add(fsi_draw_tiles(fsi_constants).drawn);
    }
    XF_DRAW_SECTION(Pass);
    if (pipe != bound_pipeline_) {
        // (after another pipeline of the renderer's own, a transfer's or a
        // port's, whose state is not the draws': every state set again)
        if (!bound_pipeline_) draw_state_valid_ = false, bound_index_buffer_ = VK_NULL_HANDLE, sets_bound_ = false;
        record_value(c, Recorded::BindPipeline, pipe);
        bound_pipeline_ = pipe;
    }
    // an export's pipeline has no dynamic state, so binding it leaves the
    // draws' undefined
    if (export_only) draw_state_valid_ = false;
    // The draws' sets: both after anything else's, then the first alone (the
    // constants', whose offsets are the only thing a draw changes) and only
    // where an offset moved; the textures' set stays bound.
    const bool both_sets = !sets_bound_ || !set_elision_;
    if (both_sets || std::memcmp(bound_offsets_, dynamic_offsets, sizeof(bound_offsets_)) != 0) {
        Recorded::Sets sets;
        sets.count = both_sets ? 2 : 1;
        sets.sets[0] = draw_sets_[1], sets.sets[1] = bindless_;
        std::memcpy(sets.offsets, dynamic_offsets, sizeof(sets.offsets));
        record_value(c, Recorded::BindSets, sets);
        sets_bound_ = true;
    }
    std::memcpy(bound_offsets_, dynamic_offsets, sizeof(bound_offsets_));
    // the index buffer bound whole, once: the draw's first index is where its words are
    if (index_buffer && index_buffer != bound_index_buffer_) {
        record_value(c, Recorded::BindIndexBuffer, bound_index_buffer_ = index_buffer);
    }
    const uint32_t first_index = uint32_t(index_at / 4);
    if (attribute_buffer) record_value(c, Recorded::BindVertexBuffer, Recorded::Place{attribute_buffer, attribute_at});
    if (export_only) {
        record_words(c, Recorded::DrawIndexed, uint32_t(indices.size()), first_index);
        record_publish();
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
    if (!polygonal || !((depth_control >> 7) & 1)) stencil_bf = stencil;
    const float *blend_constants = reinterpret_cast<const float *>(&regs_[r::RB_BLEND_RED]);
    // at the resolution scale (Xenia's GetHostViewportInfo and GetScissor
    // with draw_resolution_scale): the viewport's bounds and the scissor's
    // edges in host pixels, the NDC transform the same
    VkViewport host_viewport_rect = viewport.viewport;
    VkRect2D host_scissor_rect = scissor;
    if (!scale_.one()) {
        const float f = scale_.factor();
        host_viewport_rect.x *= f, host_viewport_rect.y *= f;
        host_viewport_rect.width *= f, host_viewport_rect.height *= f;
        const uint32_t x0 = scale_.down(uint32_t(scissor.offset.x)), y0 = scale_.down(uint32_t(scissor.offset.y));
        host_scissor_rect.offset = {int32_t(x0), int32_t(y0)};
        host_scissor_rect.extent = {scale_.down(uint32_t(scissor.offset.x) + scissor.extent.width) - x0,
                                    scale_.down(uint32_t(scissor.offset.y) + scissor.extent.height) - y0};
    }
    // The draws' dynamic state, each set only where it differs from what the
    // command buffer holds (draw_state_; Xenia's D3D12 command processor sets
    // its viewport, scissor, blend factor and stencil reference the same way,
    // the rest is in its pipelines): most draws change a few of the 25.
    DrawState n;
    std::memset(&n, 0, sizeof(n)); // (compared bytewise, padding included)
    n.viewport = host_viewport_rect;
    n.scissor = host_scissor_rect;
    std::memcpy(n.blend_constants, blend_constants, sizeof(n.blend_constants));
    for (uint32_t face = 0; face < 2; face++) {
        const uint32_t s = face ? stencil_bf : stencil;
        n.stencil_reference[face] = s & 0xFF;
        n.stencil_compare_mask[face] = (s >> 8) & 0xFF;
        n.stencil_write_mask[face] = (s >> 16) & 0xFF;
        const VkStencilOpState &op = face ? depth_state.back : depth_state.front;
        n.stencil_op[face][0] = op.failOp, n.stencil_op[face][1] = op.passOp;
        n.stencil_op[face][2] = op.depthFailOp, n.stencil_op[face][3] = VkStencilOp(op.compareOp);
    }
    n.topology = topology;
    n.restart = restart;
    n.cull_mode = raster.cullMode;
    n.front_face = raster.frontFace;
    n.depth_test = depth_state.depthTestEnable;
    n.depth_write = depth_state.depthWriteEnable;
    n.depth_compare = depth_state.depthCompareOp;
    n.stencil_test = depth_state.stencilTestEnable;
    n.depth_bias_enable = raster.depthBiasEnable;
    const bool blend_dynamic = d_.dynamic_blend() && !fsi_;
    if (blend_dynamic)
        for (uint32_t i = 0; i < 4; i++) {
            const VkPipelineColorBlendAttachmentState &b = blends[i];
            n.blend_enables[i] = b.blendEnable;
            n.blend_equations[i] = {b.srcColorBlendFactor, b.dstColorBlendFactor, b.colorBlendOp,
                                    b.srcAlphaBlendFactor, b.dstAlphaBlendFactor, b.alphaBlendOp};
            n.write_masks[i] = b.colorWriteMask;
        }
    const bool all = !draw_state_valid_ || !draw_state_elision_;
    const DrawState &o = draw_state_;
    // (whether any of it did: the kept copy is written again only then)
    bool state_changed = all;
    auto differs = [all, &state_changed](const auto &a, const auto &b) {
        if (!all && std::memcmp(&a, &b, sizeof(a)) == 0) return false;
        state_changed = true;
        return true;
    };
    if (differs(n.viewport, o.viewport)) record_value(c, Recorded::Viewport, n.viewport);
    if (differs(n.scissor, o.scissor)) record_value(c, Recorded::Scissor, n.scissor);
    if (differs(n.blend_constants, o.blend_constants)) record_value(c, Recorded::BlendConstants, n.blend_constants);
    static constexpr VkStencilFaceFlags FACES[2] = {VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_FACE_BACK_BIT};
    for (uint32_t face = 0; face < 2; face++) {
        if (differs(n.stencil_reference[face], o.stencil_reference[face]))
            record_words(c, Recorded::StencilReference, FACES[face], n.stencil_reference[face]);
        if (differs(n.stencil_compare_mask[face], o.stencil_compare_mask[face]))
            record_words(c, Recorded::StencilCompareMask, FACES[face], n.stencil_compare_mask[face]);
        if (differs(n.stencil_write_mask[face], o.stencil_write_mask[face]))
            record_words(c, Recorded::StencilWriteMask, FACES[face], n.stencil_write_mask[face]);
    }
    // the extended dynamic state, as the pipeline's description has it
    if (differs(n.topology, o.topology)) record_words(c, Recorded::Topology, uint32_t(n.topology));
    if (differs(n.restart, o.restart)) record_words(c, Recorded::RestartEnable, uint32_t(n.restart));
    if (differs(n.cull_mode, o.cull_mode)) record_words(c, Recorded::CullMode, uint32_t(n.cull_mode));
    if (differs(n.front_face, o.front_face)) record_words(c, Recorded::FrontFace, uint32_t(n.front_face));
    if (differs(n.depth_test, o.depth_test)) record_words(c, Recorded::DepthTest, uint32_t(n.depth_test));
    if (differs(n.depth_write, o.depth_write)) record_words(c, Recorded::DepthWrite, uint32_t(n.depth_write));
    if (differs(n.depth_compare, o.depth_compare)) record_words(c, Recorded::DepthCompare, uint32_t(n.depth_compare));
    if (differs(n.stencil_test, o.stencil_test)) record_words(c, Recorded::StencilTest, uint32_t(n.stencil_test));
    for (uint32_t face = 0; face < 2; face++)
        if (differs(n.stencil_op[face], o.stencil_op[face]))
            record_words(c, Recorded::StencilOp, FACES[face], uint32_t(n.stencil_op[face][0]),
                         uint32_t(n.stencil_op[face][1]), uint32_t(n.stencil_op[face][2]),
                         uint32_t(n.stencil_op[face][3]));
    if (differs(n.depth_bias_enable, o.depth_bias_enable)) record_words(c, Recorded::DepthBiasEnable, uint32_t(n.depth_bias_enable));
    if (blend_dynamic) {
        // extended dynamic state 3: the blend and write masks of the four
        // attachments the pipelines have (those without a target unused)
        if (differs(n.blend_enables, o.blend_enables)) record_value(c, Recorded::BlendEnables, n.blend_enables);
        if (differs(n.blend_equations, o.blend_equations)) record_value(c, Recorded::BlendEquations, n.blend_equations);
        if (differs(n.write_masks, o.write_masks)) record_value(c, Recorded::WriteMasks, n.write_masks);
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
        n.line_width = width;
        if (differs(n.line_width, o.line_width)) record_value(c, Recorded::LineWidth, width);
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
                // (the last scale's kept: a scene draws with a few, and the
                // logarithm and power at every draw were 4% of the command thread)
                if (scale != 0.0f && scale == rounded_scale_in_) {
                    scale = rounded_scale_out_;
                } else if (scale != 0.0f) {
                    rounded_scale_in_ = scale;
                    double order = std::pow(10.0, std::floor(std::log10(std::fabs(double(scale)))));
                    scale = rounded_scale_out_ = float(std::round(double(scale) / order) * order);
                }
            }
            if ((sc_mode >> 12) & 1 && !(sc_mode & 2) && scale == 0.0f && offset == 0.0f)
                scale = offsets[2], offset = offsets[3];
        } else if ((sc_mode >> 13) & 1) {
            scale = offsets[0], offset = offsets[1];
        }
        // The constant, in the host's units (Xenia's draw_util.h). Direct3D's
        // bias is a whole number of units, rounded up (GetD3D10IntegerPolygonOffset:
        // more offset is better than none), of 2^-24 on 24-bit unorm depth and of
        // the primitive's own float unit on float depth, where a D24FS8 unit
        // is 8 of the host's (and the target's halved range halves the unit
        // by itself).
        // With VK_EXT_depth_bias_control the unit is exactly 2^-24 on both:
        // without it a device may take up to twice that on unorm depth (the
        // specification's minimum resolvable difference; NVIDIA does, which
        // doubled every constant offset there), and a float format's unit
        // shrinks with the depth, losing small offsets far from 1 (decals
        // with reversed depth). Float depth then takes the guest's offset as
        // it is, as the console and the exact path do and as Xenia's Vulkan
        // backend (vulkan_depth_bias_force_unorm), halved with D24FS8's range.
        // XF_DEPTH_BIAS_FLOAT_UNITS=1 keeps a float format's own units, as
        // Xenia's Direct3D 12 backend draws.
        const bool float24 = depth && depth->format == 1;
        const bool host_float = depth && depth->image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const bool exact_units = d_.depth_bias_control() && !(host_float && depth_bias_float_units_);
        float constant = 0.0f;
        if (offset != 0.0f) {
            if (host_float && exact_units)
                constant = offset * (float24 ? float(1u << 24) * 0.5f : float((1u << 24) - 1));
            else if (float24)
                constant = std::copysign(std::ceil(std::fabs(offset) * float(1u << 21)) * 8.0f, offset);
            else
                constant = std::copysign(std::ceil(std::fabs(offset) * float((1u << 24) - 1)), offset);
            if (!exact_units)
                approximate(host_float ? "polygon offsets in a float depth format's units (no VK_EXT_depth_bias_control)"
                                       : "polygon offsets in the device's depth units (no VK_EXT_depth_bias_control)");
        }
        // (the representation is part of the state: 1 the format's, 2 unorm forced)
        n.depth_bias[0] = constant, n.depth_bias[1] = scale * (scale_.factor() / 16.0f);
        n.depth_bias[2] = !exact_units ? 0.0f : host_float ? 2.0f : 1.0f;
        if (differs(n.depth_bias, o.depth_bias)) {
            record_value(c, exact_units ? Recorded::DepthBiasExact : Recorded::DepthBias,
                         Recorded::Bias{host_float, n.depth_bias[0], n.depth_bias[1]});
        }
    }
    XF_DRAW_SECTION(State);
    if (state_changed) draw_state_ = n;
    draw_state_valid_ = true;
    if (options_.gpu_timing_draws) {
        // each draw timed on its own, counted to its shaders
        char kind[96];
        std::snprintf(kind, sizeof(kind), "draw: %s, shaders %016llX %016llX", draw_name.c_str(),
                      (unsigned long long)vertex_hash_, ps ? (unsigned long long)pixel_hash_ : 0ull);
        gpu_mark(kind);
    } else if (options_.gpu_timing && generic) {
        // the generic shaders' draws timed apart from their pass (the GPU's cost of a stand-in)
        if (gpu_generic_kind_ == UINT32_MAX) {
            gpu_mark("draws: by the generic shaders");
            gpu_generic_kind_ = gpu_kind_ids_["draws: by the generic shaders"];
        } else {
            gpu_mark(gpu_generic_kind_);
        }
    }
    // a generic draw timed on the GPU, its cost kept by its pixel shader (generic_allowed)
    uint32_t generic_timed = UINT32_MAX;
    if (generic && slots_[current_].generic_timed && slots_[current_].generic_shaders.size() < GENERIC_TIMED) {
        Slot &s = slots_[current_];
        generic_timed = uint32_t(s.generic_shaders.size());
        s.generic_shaders.push_back(pixel_hash_);
        s.generic_reset = true;
        vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.generic_timed, 2 * generic_timed);
    }
    if (viz_survey_ == UINT32_MAX)
        zpd_draw(c);
    else if (!fsi_)
        viz_draw(c);
    if (viz_predicate_ != UINT32_MAX) {
        // drawn where the query's survey passed samples
        record_value(c, Recorded::BeginConditional,
                     Recorded::Place{viz_predicates_.buffer, VkDeviceSize(viz_predicate_) * 4});
        report_.viz_predicated++;
    }
    if (host_vertex.extents && extent_serial_ == slots_[current_].serial) {
        // bounded (lines half their width wider: PA_SU_LINE_CNTL's width is
        // half the width in 12.4 fixed point)
        extent_drawn_ = true;
        if (topology_class == 1)
            extent_margin_ = std::max(extent_margin_, ((regs_[r::PA_SU_LINE_CNTL] & 0xFFFF) + 15) / 16);
    }
    if (tessellated)
        record_words(c, Recorded::Draw, uint32_t(control_points.size() / 2));
    else
        record_words(c, Recorded::DrawIndexed, uint32_t(indices.size()), first_index);
    if (viz_predicate_ != UINT32_MAX) record_words(c, Recorded::EndConditional);
    if (generic_timed != UINT32_MAX)
        vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slots_[current_].generic_timed,
                             2 * generic_timed + 1);
    record_publish();
    if (options_.gpu_timing_draws)
        gpu_mark("draws: between draws");
    else if (options_.gpu_timing && generic && gpu_pass_kind_ != UINT32_MAX)
        gpu_mark(gpu_pass_kind_);
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
    if (native_suppress_ || native_settings_.observer) {
        native::PassInfo pass;
        pass.resolve = true;
        pass.copy_dest = regs_[reg::RB_COPY_DEST_BASE];
        native_pass(pass);
        if (native_settings_.observer) native_settings_.observer(pass);
        if (native_suppress_ && !native_keep(pass)) return;
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
    // (the tiles under the rectangle: with XF_DRAW_REACH=rows, every tile of
    // the rows down to its last, as Xenia takes them)
    auto take = [&](RenderTarget *t) {
        if (!reach_tiles_ || !w || !h) return acquire(t, y + h);
        const TileRect tiles = tiles_of(*t, x, y, x + w, y + h);
        acquire(t, std::span<const TileRect>(&tiles, 1));
    };
    // Beyond Xenia: a copy alone from a source that does not hold every tile
    // of the rectangle (a view of tiles another target draws to: a 4x
    // shadow atlas resolved as a single-sampled surface twice as wide and
    // tall) leaves the tiles with their holders, which store them into
    // EDRAM, and reads them there: a store, where taking them was a transfer
    // into the view and another back at the holder's next draw. Off unless
    // XF_RESOLVE_FROM_HOLDERS=1 (the source takes them, as Xenia's): in the
    // one frame measured the transfers it replaces were clears already, and
    // its two stores cost 0.02 ms more (docs/renderer.md).
    static const bool from_holders = [] {
        const char *v = std::getenv("XF_RESOLVE_FROM_HOLDERS"), *image = std::getenv("XF_RESOLVE_FROM_IMAGE");
        return v && v[0] == '1' && !(image && std::string(image) == "1");
    }();
    resolve_stored_ = false;
    if (!fsi_ && from_holders && reach_tiles_ && info.copy && !info.clear_color && !info.clear_depth && w && h) {
        const TileRect rect = tiles_of(*source, x, y, x + w, y + h);
        std::map<RenderTarget *, std::vector<uint32_t>> held;
        bool others = false;
        for (uint32_t row = rect.r0; row < rect.r1; row++)
            for (uint32_t column = rect.c0; column < rect.c1; column++) {
                if (row * source->pitch_tiles + column >= EDRAM_TILES) continue;
                const uint32_t tile = (source->base + row * source->pitch_tiles + column) % EDRAM_TILES;
                if (!owners_[tile]) continue; // (EDRAM has it)
                held[owners_[tile]].push_back(tile);
                others |= owners_[tile] != source;
            }
        if (others) {
            for (auto &[holder, tiles] : held)
                if (!edram_store(holder, tiles)) return;
            resolve_stored_ = true;
        }
    }
    if (!fsi_ && !resolve_stored_) take(source);
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
            take(t);
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
            // (at the resolution scale: x, y, w and h are multiples of 8)
            const uint32_t cw = std::min(w, t->width - std::min(x, t->width)), ch = std::min(h, t->height - std::min(y, t->height));
            VkClearRect rect{{{int32_t(scale_.down(x)), int32_t(scale_.down(y))},
                              {std::min(scale_.down(cw), t->host_width - std::min(scale_.down(x), t->host_width)),
                               std::min(scale_.down(ch), t->host_height - std::min(scale_.down(y), t->host_height))}},
                             0, 1};
            if (!rect.rect.extent.width || !rect.rect.extent.height) continue;
            // the target's tiles the clear covers whole now hold its value;
            // those it covers in part, not one value any more
            {
                const uint32_t x1 = std::min(x, t->width) + cw, y1 = std::min(y, t->height) + ch;
                const uint32_t rows = t->height / t->tile_height;
                for (uint32_t row = y / t->tile_height; row < std::min(rows, (y1 + t->tile_height - 1) / t->tile_height); row++)
                    for (uint32_t column = x / t->tile_width;
                         column < std::min(t->pitch_tiles, (x1 + t->tile_width - 1) / t->tile_width); column++) {
                        const uint32_t tile = (t->base + row * t->pitch_tiles + column) % EDRAM_TILES;
                        if (owners_[tile] != t) continue;
                        const bool whole = column * t->tile_width >= x && (column + 1) * t->tile_width <= x1 &&
                                           row * t->tile_height >= y && (row + 1) * t->tile_height <= y1;
                        tile_cleared_[tile] = whole;
                        if (whole) tile_clear_value_[tile] = clear.clearValue, cleared_epoch_++;
                        if (whole && t->depth) tile_clear_word_[tile] = regs_[r::RB_DEPTH_CLEAR];
                        if (t->depth) {
                            tile_stencil_[tile] = whole ? int16_t(regs_[r::RB_DEPTH_CLEAR] & 0xFF) : int16_t(-1);
                            if (whole) stencil_epoch_++;
                        }
                    }
            }
            // kept for the target's next render pass, whose draws need it
            // first, and drawn there (begin_pass), without a barrier and a
            // rendering pass of its own; anything else that reads or writes
            // the image first has it drawn before (flush_clears). Not for a
            // target cleared through its UINT view (begin_pass draws through
            // the other), nor with XF_DEFER_CLEARS=0.
            t->pending_clears.push_back({clear, rect});
            if (t->raw || !defer_clears_) flush_clears(t);
        }
    }
}

// A target's kept clears drawn now, in a rendering pass of their own (as
// Xenia's resolves clear), before something other than its next render pass
// takes its image
void Impl::flush_clears(RenderTarget *t) {
    if (!t || t->pending_clears.empty()) return;
    VkCommandBuffer c = outside();
    gpu_mark("resolves: clears");
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = t->raw ? t->raw : t->view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {t->host_width, t->host_height}};
    rendering.layerCount = 1;
    if (t->depth) {
        rendering.pDepthAttachment = rendering.pStencilAttachment = &attachment;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    }
    vkCmdBeginRendering(c, &rendering);
    t->samples_unsettled = t->depth;
    for (const auto &[clear, rect] : t->pending_clears) vkCmdClearAttachments(c, 1, &clear, 1, &rect);
    vkCmdEndRendering(c);
    t->pending_clears.clear();
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
uint32_t Impl::resolve_targets(const resolve::Info &info, Texture *targets[2], uint32_t &extra_columns,
                               uint32_t &extra_rows) {
    targets[0] = targets[1] = nullptr;
    extra_columns = extra_rows = 0;
    // (depth resolves too: they copy the guest's depth words as they are,
    // which a texture reading the bits takes, and a depth texture takes as
    // the float its load would make; Texture::target_convert;
    // XF_RESOLVE_TARGETS_DEPTH=0 leaves them to memory alone)
    if (!resolve_targets_ || info.dest_array || !info.texture_base || (info.depth && !depth_targets_)) return 0;
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
    // (x, y + rows). Within a row of macro tiles, the 32x32 tiles follow one
    // another across, each 32 * 32 texels: a destination a whole number of
    // tiles in is fed that many columns across as well (XF_RESOLVE_TARGET_TILES;
    // GetTiledOffset2D of (x + 32k, y + 32m) is that of (x, y) plus
    // (m * pitch / 32 + k) tiles, the bits of x and y below 32 it mixes
    // being the same).
    //
    // A stale one the resolve writes a part of is fed too (XF_RESOLVE_TARGET_
    // COVER), its tiles written counted until all are (a shadow atlas drawn
    // and resolved a quarter at a time, over memory another surface wrote
    // since its last use), after those it keeps current: these are taken
    // first.
    if (target_tiles_ || target_cover_)
        return resolve_targets_by_tiles(info, targets, extra_columns, extra_rows, bucket, base, bytes, wide);
    extra_columns = 0;
    uint32_t count = 0;
    for (Texture *t : texture_buckets_[bucket]) {
        // (each target over the destination's base refused, counted by why:
        // "uses" in the report)
        if (!t->target) continue;
        if (base < t->target_base || base >= t->high) continue;
        if (((t->target >> 7) & 1) != wide || t->target_pitch != info.dest_pitch) {
            if (refusal_detail_) {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "a resolve target refused: texture %s %ux%u pitch %u, resolve %s %ux%u pitch %u",
                              format_name(t->key[1] & 0x3F), (t->key[2] & 0x1FFF) + 1, ((t->key[2] >> 13) & 0x1FFF) + 1,
                              t->target_pitch, format_name(info.dest_format), info.width, info.height, info.dest_pitch);
                use(std::string(line));
            } else {
                use(((t->target >> 7) & 1) != wide ? "a resolve target refused: another texel size"
                                                     : "a resolve target refused: another pitch");
            }
            continue;
        }
        if (t->target_endian != info.dest_endian) {
            use("a resolve target refused: another endianness");
            continue;
        }
        const uint32_t offset = base - t->target_base, macro_row = t->target_pitch * bytes * 32;
        if (!macro_row || offset % macro_row) {
            use("a resolve target refused: not a whole number of macro tile rows in");
            continue;
        }
        const uint32_t rows = offset / macro_row * 32;
        if ((info.texel_y0 + rows) >> 5 > 0xFF || (count && rows != extra_rows)) {
            use("a resolve target refused: rows the push constants cannot hold");
            continue;
        }
        if (t->stale && (rows || !covers(*t))) {
            use("a resolve target refused: stale, and the resolve does not write all of it");
            continue;
        }
        if (count == 2) {
            use("a resolve target refused: a third over the destination");
            continue;
        }
        extra_rows = rows;
        targets[count++] = t;
    }
    return count;
}

// resolve_targets with targets a whole number of tiles in across
// (target_tiles_) and stale ones fed in parts (target_cover_).
uint32_t Impl::resolve_targets_by_tiles(const resolve::Info &info, Texture *targets[2], uint32_t &extra_columns,
                                        uint32_t &extra_rows, uint32_t bucket, uint32_t base, uint32_t bytes,
                                        uint32_t wide) {
    const uint32_t x0 = info.texel_x0 + info.dest_x, y0 = info.texel_y0 + info.dest_y;
    struct Candidate {
        Texture *t;
        uint32_t columns, rows;
        bool part; // stale, and the resolve writes a part of it
    };
    Candidate found[8];
    uint32_t candidates = 0;
    for (Texture *t : texture_buckets_[bucket]) {
        if (!t->target) continue;
        if (base < t->target_base || base >= t->high) continue;
        if (((t->target >> 7) & 1) != wide || t->target_pitch != info.dest_pitch) {
            if (refusal_detail_) {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "a resolve target refused: texture %s %ux%u pitch %u, resolve %s %ux%u pitch %u",
                              format_name(t->key[1] & 0x3F), (t->key[2] & 0x1FFF) + 1, ((t->key[2] >> 13) & 0x1FFF) + 1,
                              t->target_pitch, format_name(info.dest_format), info.width, info.height, info.dest_pitch);
                use(std::string(line));
            } else {
                use(((t->target >> 7) & 1) != wide ? "a resolve target refused: another texel size"
                                                   : "a resolve target refused: another pitch");
            }
            continue;
        }
        if (t->target_endian != info.dest_endian) {
            use("a resolve target refused: another endianness");
            continue;
        }
        const uint32_t offset = base - t->target_base, macro_row = t->target_pitch * bytes * 32, tile = bytes * 32 * 32;
        if (!macro_row || offset % (target_tiles_ ? tile : macro_row)) {
            use(target_tiles_ ? "a resolve target refused: not a whole number of tiles in"
                              : "a resolve target refused: not a whole number of macro tile rows in");
            continue;
        }
        const uint32_t rows = offset / macro_row * 32, columns = offset % macro_row / tile * 32;
        // (a destination running past the pitch writes memory the texture
        // holds a row of tiles further down, which the texel's bounds check
        // would leave out of its image)
        if (columns && columns + x0 + info.width > t->target_pitch) {
            use("a resolve target refused: a part past its pitch");
            continue;
        }
        if ((info.texel_y0 + rows) >> 5 > 0xFF || (info.texel_x0 + columns) >> 5 > 0xFF) {
            use("a resolve target refused: rows the push constants cannot hold");
            continue;
        }
        const uint32_t width = (t->key[2] & 0x1FFF) + 1, height = ((t->key[2] >> 13) & 0x1FFF) + 1;
        const bool whole = !rows && !columns && x0 == 0 && y0 == 0 && info.width >= width && info.height >= height;
        const bool part = t->stale && !whole;
        if (part && (!target_cover_ || (t->scaled && !scaled_cover_))) {
            use("a resolve target refused: stale, and the resolve does not write all of it");
            continue;
        }
        if (candidates == std::size(found)) {
            use("a resolve target refused: a third over the destination");
            continue;
        }
        found[candidates++] = {t, columns, rows, part};
    }
    // the targets kept current first, then stale ones written in part
    uint32_t count = 0;
    for (uint32_t pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < candidates; i++) {
            const Candidate &c = found[i];
            if (c.part != (pass == 1)) continue;
            if (count && (c.rows != extra_rows || c.columns != extra_columns)) {
                use("a resolve target refused: rows the push constants cannot hold");
                continue;
            }
            if (count == 2) {
                use("a resolve target refused: a third over the destination");
                continue;
            }
            extra_rows = c.rows, extra_columns = c.columns;
            targets[count++] = c.t;
        }
    return count;
}

bool Impl::cover(Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t width = (t.key[2] & 0x1FFF) + 1, height = ((t.key[2] >> 13) & 0x1FFF) + 1;
    const uint32_t across = (width + 31) / 32, down = (height + 31) / 32, words = (across + 63) / 64;
    x1 = std::min(x1, width), y1 = std::min(y1, height);
    if (t.covered.size() != size_t(words) * down) t.covered.assign(size_t(words) * down, 0), t.covered_count = 0;
    // the tiles wholly inside, a tile at the texture's edge ending there
    for (uint32_t ty = (y0 + 31) / 32; ty < down && std::min(ty * 32 + 32, height) <= y1; ty++)
        for (uint32_t tx = (x0 + 31) / 32; tx < across && std::min(tx * 32 + 32, width) <= x1; tx++) {
            uint64_t &word = t.covered[size_t(ty) * words + tx / 64];
            const uint64_t bit = 1ull << (tx % 64);
            if (!(word & bit)) word |= bit, t.covered_count++;
        }
    return t.covered_count == across * down;
}

// ---- resolved memory at the resolution scale (Impl::ScaledRegion) ----

void Impl::write_scaled_sets() {
    const VkDescriptorBufferInfo infos[6] = {
        {edram_.buffer, 0, VK_WHOLE_SIZE},       {scaled_heap_.buffer, 0, VK_WHOLE_SIZE},
        {scaled_heap_.buffer, 0, VK_WHOLE_SIZE}, {load_scratch_.buffer, 0, VK_WHOLE_SIZE},
        {mirror_.buffer, 0, VK_WHOLE_SIZE},      {scaled_heap_.buffer, 0, VK_WHOLE_SIZE}};
    const VkDescriptorSet sets[3] = {scaled_resolve_set_, scaled_load_set_, scaled_fill_set_};
    VkWriteDescriptorSet writes[6];
    for (uint32_t k = 0; k < 6; k++) {
        writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[k].dstSet = sets[k >> 1];
        writes[k].dstBinding = k & 1;
        writes[k].descriptorCount = 1;
        writes[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[k].pBufferInfo = &infos[k];
    }
    vkUpdateDescriptorSets(dev_, 6, writes, 0, nullptr);
}

// The region holding [low, high), or null
const std::pair<const uint32_t, Impl::ScaledRegion> *Impl::scaled_region(uint32_t low, uint32_t high) const {
    auto it = scaled_regions_.upper_bound(low);
    if (it == scaled_regions_.begin()) return nullptr;
    --it;
    return high <= it->second.end ? &*it : nullptr;
}

// A heap a quarter larger than the regions need (the room the merges' moves
// take until the next repacking), the other regions' scaled bytes moved into
// it packed, and after them the new region's run with the bytes of those it
// merged. Waits for the GPU: rare, as a game's resolves settle on their
// surfaces. False where the heap cannot be so large.
bool Impl::repack_scaled_heap(const std::vector<std::pair<uint32_t, ScaledRegion>> &merged, uint32_t start,
                              ScaledRegion &region) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(d_.physical(), &properties);
    const VkDeviceSize q2 = scale_.q4 * scale_.q4;
    // (within a storage buffer's range, and a 32-bit guest-byte count of it: scaled_rel)
    const VkDeviceSize most = std::min({VkDeviceSize(properties.limits.maxStorageBufferRange),
                                        VkDeviceSize(3) << 30, (VkDeviceSize(1) << 28) * q2});
    const VkDeviceSize length = VkDeviceSize((region.end - start) >> 4) * q2;
    VkDeviceSize live = length;
    for (const auto &[first, other] : scaled_regions_) live += VkDeviceSize((other.end - first) >> 4) * q2;
    VkDeviceSize size = std::max<VkDeviceSize>(live / 4 * 5, VkDeviceSize(16) << 20);
    size = std::min((size + 0xFFFFF) & ~VkDeviceSize(0xFFFFF), most);
    if (live > size) return false;
    sync();
    vk::Buffer heap = d_.create_buffer(
        size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        false);
    if (!heap.buffer) return false;
    std::vector<VkBufferCopy> copies;
    VkDeviceSize at = 0;
    for (auto &[first, other] : scaled_regions_) {
        const VkDeviceSize bytes = VkDeviceSize((other.end - first) >> 4) * q2;
        copies.push_back({other.heap, at, bytes});
        other.heap = at;
        at += bytes;
    }
    region.heap = at;
    for (const auto &[first, old] : merged)
        copies.push_back(
            {old.heap, at + VkDeviceSize((first - start) >> 4) * q2, VkDeviceSize((old.end - first) >> 4) * q2});
    at += length;
    if (!copies.empty()) {
        VkCommandBuffer c = outside();
        vkCmdCopyBuffer(c, scaled_heap_.buffer, heap.buffer, uint32_t(copies.size()), copies.data());
        full_barrier(c);
        sync();
    }
    d_.destroy(scaled_heap_);
    scaled_heap_ = heap;
    scaled_heap_used_ = at;
    write_scaled_sets();
    report_.scaled_heap_bytes = size;
    report_.scaled_heap_repacks++;
    if (options_.verbose)
        std::fprintf(stderr, "  resolved memory at the scale: a heap of %.1f MB for %zu regions (%.1f MB live)\n",
                     double(size) / 1048576.0, scaled_regions_.size() + 1, double(at) / 1048576.0);
    return true;
}

// The region holding [low, high), made (or made larger, merging the regions
// it meets, their scaled bytes moved into its new run of the heap) where
// none does. Null where the heap cannot grow to it: then nothing there is
// kept at the scale.
const std::pair<const uint32_t, Impl::ScaledRegion> *Impl::make_scaled_region(uint32_t low, uint32_t high) {
    if (const auto *found = scaled_region(low, high)) return found;
    constexpr uint32_t ALIGN = 0x10000;
    uint32_t start = low & ~(ALIGN - 1),
             end = uint32_t(std::min<uint64_t>((uint64_t(high) + ALIGN - 1) & ~uint64_t(ALIGN - 1), GUEST_MEMORY));
    // the regions it meets, or that lie within 256 KB of it, are merged into
    // it (the gaps between a frame's surfaces taken in once, rather than a
    // region moved again as each surface beside it is resolved)
    constexpr uint32_t SLACK = 0x40000;
    std::vector<std::pair<uint32_t, ScaledRegion>> merged;
    for (auto at = scaled_regions_.begin(); at != scaled_regions_.end();) {
        if (at->first <= end + SLACK && start <= at->second.end + SLACK) {
            start = std::min(start, at->first), end = std::max(end, at->second.end);
            merged.push_back(*at);
            scaled_regions_.erase(at);
            at = scaled_regions_.begin(); // (a wider region may meet more)
            continue;
        }
        ++at;
    }
    const VkDeviceSize q2 = scale_.q4 * scale_.q4;
    const VkDeviceSize length = VkDeviceSize((end - start) >> 4) * q2;
    ScaledRegion region{end, 0};
    if (scaled_heap_used_ + length <= scaled_heap_.size) {
        region.heap = scaled_heap_used_;
        scaled_heap_used_ += length;
        if (!merged.empty()) {
            // the merged regions' scaled bytes into the new run (beyond every
            // live one: the copies do not overlap)
            VkCommandBuffer c = outside();
            full_barrier(c);
            std::vector<VkBufferCopy> copies;
            for (const auto &[first, old] : merged)
                copies.push_back({old.heap, region.heap + VkDeviceSize((first - start) >> 4) * q2,
                                  VkDeviceSize((old.end - first) >> 4) * q2});
            vkCmdCopyBuffer(c, scaled_heap_.buffer, scaled_heap_.buffer, uint32_t(copies.size()), copies.data());
            full_barrier(c);
        }
    } else if (!repack_scaled_heap(merged, start, region)) {
        // no room: those it meets are given up, their pages' tags cleared
        for (const auto &[first, old] : merged) clear_scaled_tags(first, old.end);
        report_.scaled_heap_full++;
        return nullptr;
    }
    return &*scaled_regions_.emplace(start, region).first;
}

// The scaled resolve kept for a resolve no scaled texture is over (Impl::
// ScaledResolve): one of the resolves' targets, 32- or 64-bit texels, from the
// texture's corner, no smaller than the minimum scalable dimension; at most
// eight kept. Null when the resolve is not one.
Impl::ScaledResolve *Impl::scaled_resolve(const resolve::Info &info, const std::vector<Range> &written) {
    const uint32_t bytes = texture::format_block(info.dest_format).bytes;
    // (64-bit texels with XF_SCALE_WIDE, as scale_texture's callers; never kept where resolved memory is
    // kept at the scale: its caller)
    if (info.depth || info.dest_array || !info.texture_base || (bytes != 4 && !(bytes == 8 && scale_wide_)) ||
        info.texel_x0 + info.dest_x || info.texel_y0 + info.dest_y || !scalable(info.width, info.height, min_scalable_) ||
        written.empty() || scaled_resolves_.size() >= 8)
        return nullptr;
    const uint32_t wide = bytes == 8 ? 1 : 0;
    const VkFormat format = wide ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32_UINT;
    ScaledResolve kept;
    kept.image = d_.create_image(VK_IMAGE_TYPE_2D, format, {scale_.up(info.width), scale_.up(info.height), 1}, 1, 1,
                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!kept.image.image) return nullptr;
    uint32_t slot = UINT32_MAX;
    if (!free_targets_[wide].empty()) {
        slot = free_targets_[wide].back();
        free_targets_[wide].pop_back();
    } else {
        slot = take_target(wide);
    }
    kept.view = slot == UINT32_MAX ? VK_NULL_HANDLE
                                   : make_view(kept.image.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, {}, 1, 1, false);
    if (!kept.view) {
        if (slot != UINT32_MAX) free_targets_[wide].push_back(slot);
        d_.destroy(kept.image);
        return nullptr;
    }
    VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, kept.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = target_set_;
    write.dstBinding = wide;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    begin_image(outside(), kept.image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    kept.target = (slot + 1) | wide << 7;
    kept.pitch = info.dest_pitch, kept.endian = info.dest_endian;
    kept.width = info.width, kept.height = info.height;
    kept.low = UINT32_MAX, kept.high = 0;
    for (auto [start, end] : written) kept.low = std::min(kept.low, start), kept.high = std::max(kept.high, end);
    kept.swap = report_.swaps;
    return &(scaled_resolves_[info.texture_base & 0x1FFFFFFF] = kept);
}

// The scaled resolves kept over memory something else writes, gone once the
// work recorded so far is done
void Impl::drop_scaled_resolves(const std::vector<Range> &written, bool all) {
    for (auto it = scaled_resolves_.begin(); it != scaled_resolves_.end();) {
        bool hit = all;
        for (auto [start, end] : written) hit |= start < it->second.high && it->second.low < end;
        if (!hit) {
            ++it;
            continue;
        }
        ScaledResolve old = it->second;
        it = scaled_resolves_.erase(it);
        begin_recording();
        slots_[current_].after.push_back([this, old]() mutable {
            release_target(old.target, old.view);
            d_.destroy(old.image);
        });
    }
}

// A resolve target made the resolution scale's (docs/renderer.md,
// "Resolution scale"): a new image of the same format width x height console
// texels at the scale, with its own place in the resolves' table; the old
// one, its views and its place go once the submission being recorded is
// done, as texture() replaces an image. Its views are made again as draws
// ask. The resolve that calls it writes every texel.
void Impl::scale_texture(Texture &t, uint32_t width, uint32_t height) {
    const uint32_t wide = (t.target >> 7) & 1;
    const VkFormat target_format = wide ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32_UINT;
    vk::Image image = d_.create_image(
        VK_IMAGE_TYPE_2D, t.image.format, {scale_.up(width), scale_.up(height), 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, t.signed_format, target_format);
    if (!image.image) return;
    uint32_t slot = UINT32_MAX;
    if (!free_targets_[wide].empty()) {
        slot = free_targets_[wide].back();
        free_targets_[wide].pop_back();
    } else {
        slot = take_target(wide);
    }
    VkImageView target_view = VK_NULL_HANDLE;
    if (slot != UINT32_MAX) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = target_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageViewUsageCreateInfo view_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        view_usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        view.pNext = &view_usage;
        if (vkCreateImageView(dev_, &view, nullptr, &target_view) != VK_SUCCESS) target_view = VK_NULL_HANDLE;
    }
    if (!target_view) {
        if (slot != UINT32_MAX) free_targets_[wide].push_back(slot);
        d_.destroy(image);
        return;
    }
    VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, target_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = target_set_;
    write.dstBinding = wide;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    // the old image, views and place, once the work recorded so far is done
    vk::Image old = t.image;
    std::vector<TextureView> old_views = std::move(t.views);
    const uint32_t old_kind = t.kind, old_target = t.target;
    const VkImageView old_target_view = t.target_view;
    report_.texture_views -= old_views.size();
    begin_recording();
    slots_[current_].after.push_back([this, old, old_views, old_kind, old_target, old_target_view]() mutable {
        for (const TextureView &v : old_views) {
            release_index(old_kind, v.index, true); // (t.loads moved below)
            vkDestroyImageView(dev_, v.view, nullptr);
        }
        release_target(old_target, old_target_view);
        d_.destroy(old);
    });
    t.views.clear();
    t.loads++; // (what the draws kept of its slots: SlotMemo)
    const VkDeviceSize bytes = image_bytes(image);
    count_texture_bytes(bytes, t.bytes);
    t.bytes = bytes;
    t.image = image;
    t.target = (slot + 1) | wide << 7, t.target_view = target_view;
    t.scaled = true;
    begin_image(outside(), image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    report_.textures_scaled++;
    if (options_.verbose)
        std::fprintf(stderr, "  texture %08X made the resolution scale's (%ux%u)\n", t.target_base, width, height);
}

// The copy on the GPU: the rectangle (resolved to one sample per pixel when
// multisampled) copied into a scratch buffer as the host keeps it, then
// written into the mirror as the destination says (resolve.comp, resolve.cpp's
// write); what of its pages it does not write keeps guest memory's bytes
// (made current in the mirror first, Xenia's RequestRange of the extent).
// Its pages are then the GPU's (gpu_written); textures over it are loaded
// again; guest memory gets the bytes as readback_resolve says.
void Impl::gpu_resolve(const resolve::Info &info, RenderTarget &source) {
    flush_clears(&source);
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
    // The source is read as EDRAM's samples, as the guest keeps them, as
    // Xenia's resolve shaders read them (the sample or the average
    // RB_COPY_CONTROL selects; depth one sample): on the interlock path from
    // EDRAM, which is the targets; on the host path from the target's image,
    // each sample packed as the EDRAM store would pack it (from_target_image
    // below). Single-sampled ones too: that costs less than copying the image
    // into a buffer first (game B's final 1280x720 resolve, 0.064 against
    // 0.099 ms with the store; docs/renderer.md, "Direct ownership
    // transfers"). XF_RESOLVE_FROM_IMAGE=1 copies a
    // single-sampled target's image, as before (not an 8_8_8_8_GAMMA one
    // kept linear, whose gamma is made in EDRAM's bits).
    static const bool from_image = [] {
        const char *v = std::getenv("XF_RESOLVE_FROM_IMAGE");
        return v && std::string(v) == "1";
    }();
    // (and at a resolution scale, always: the resolve reads EDRAM's scaled
    // samples)
    const bool from_edram =
        !from_image || fsi_ || info.msaa != 0 || (!info.depth && source.gamma_linear) || !scale_.one();
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!from_edram && !grow(scratch_, pixel_bytes + stencil_bytes, usage, resolve_set_, 0)) return;
    request(keep);

    // Resolved memory at the resolution scale (Impl::ScaledRegion), every
    // resolve's (a texture smaller than the minimum scalable dimension loads
    // from memory at the console's size whatever is kept): the destination's
    // units (a 32x32 tile of 32-bit or wider blocks, else the 4
    // KB page holding two or four tiles) in a region; those the rectangle
    // writes a part of, made from memory first unless their scaled bytes are
    // already this layout's; every unit's pages tagged after.
    struct ScaledFill {
        uint32_t guest, at, log2_bytes, q, tiles, words;
    };
    std::vector<ScaledFill> fills;
    std::vector<std::pair<uint32_t, uint8_t>> scaled_pages; // a page, the tag the resolve leaves it
    const std::pair<const uint32_t, ScaledRegion> *region = nullptr;
    const uint32_t dest_base = info.dest_base & 0x1FFFFFFF;
    if (scaled_memory_ && from_edram && !info.dest_array && !(info.dest_pitch & 31) && !(dest_base & 1023)) {
        const uint32_t log2_bytes = uint32_t(std::countr_zero(texture::format_block(info.dest_format).bytes));
        const uint32_t tile_bytes = 1024u << log2_bytes, unit_bytes = std::max(tile_bytes, 4096u);
        const uint32_t x0 = info.dest_x, y0 = info.dest_y, x1 = x0 + w, y1 = y0 + h;
        // the unit's first byte, and its tiles the rectangle writes whole (a
        // 64- or 128-bit tile is its own unit, wherever it starts; smaller
        // ones share the 4 KB page they lie in)
        // (by address, in a kept vector: the tiles come in address order but where a rectangle is
        // wider than its pitch, merged where they share a unit; a map's node a tile was a
        // tenth of a resolve's time at a scale)
        std::vector<std::pair<uint32_t, uint32_t>> &units = resolve_units_;
        units.clear();
        uint64_t high = 0;
        for (uint32_t ty = y0 / 32; ty <= (y1 - 1) / 32; ty++)
            for (uint32_t tx = x0 / 32; tx <= (x1 - 1) / 32; tx++) {
                uint64_t a = uint64_t(dest_base) + uint64_t(tx + ty * (info.dest_pitch >> 5)) * tile_bytes;
                if (tile_bytes < 4096) a &= ~uint64_t(4095);
                const bool whole = x0 <= tx * 32 && x1 >= tx * 32 + 32 && y0 <= ty * 32 && y1 >= ty * 32 + 32;
                if (!units.empty() && units.back().first == uint32_t(a))
                    units.back().second += whole ? 1 : 0;
                else
                    units.push_back({uint32_t(a), whole ? 1 : 0});
                high = std::max(high, a + unit_bytes);
            }
        auto by_address = [](const auto &a, const auto &b) { return a.first < b.first; };
        if (!std::is_sorted(units.begin(), units.end(), by_address)) {
            std::stable_sort(units.begin(), units.end(), by_address);
            size_t kept = 0;
            for (size_t i = 0; i < units.size(); i++) {
                if (kept && units[kept - 1].first == units[i].first)
                    units[kept - 1].second += units[i].second;
                else
                    units[kept++] = units[i];
            }
            units.resize(kept);
        }
        if (high <= GUEST_MEMORY) region = make_scaled_region(units.begin()->first, uint32_t(high));
        if (region) {
            const uint32_t tiles_per_unit = unit_bytes / tile_bytes, q = scale_.q4;
            std::vector<Range> fill_ranges;
            for (auto [unit, whole] : units) {
                bool current = true;
                for (uint32_t page = unit; page < unit + unit_bytes; page += 4096) {
                    const uint8_t tag = scaled_tag(page, dest_base, log2_bytes);
                    current &= scaled_tags_[page >> 12] == tag;
                    scaled_pages.push_back({page >> 12, tag});
                }
                if (whole == tiles_per_unit || current) continue;
                const uint32_t at = uint32_t(VkDeviceSize((unit - region->first) >> 4) * q * q);
                fills.push_back({unit, uint32_t(region->second.heap) + at, log2_bytes, q, tiles_per_unit,
                                 uint32_t(((unit_bytes >> 4) * q * q) >> 2)});
                fill_ranges.push_back({unit, unit + unit_bytes});
            }
            request(fill_ranges);
        }
    }

    VkCommandBuffer c;
    // On the host path the guest samples come from the source's image itself,
    // packed as the EDRAM store packs them (resolve.comp's IMAGE), with no
    // store and no barrier between the two: the source holds every tile of
    // the rectangle (acquire), and keeps holding them, so EDRAM's copy of
    // them is read by nothing (XF_RESOLVE_THROUGH_EDRAM=1: as before).
    // (Not where the source does not hold them: their holders have stored
    // them into EDRAM, Impl::resolve.)
    const bool from_target_image = from_edram && !fsi_ && !resolve_through_edram_ && !resolve_stored_;
    if (from_target_image) {
        c = outside();
        if (options_.gpu_timing) gpu_mark("resolves: copies from the target: " + target_name(source));
    } else if (from_edram) {
        std::vector<uint32_t> tiles;
        for (uint32_t row = info.y / source.tile_height; row <= (info.y + h - 1) / source.tile_height; row++)
            for (uint32_t column = info.x / source.tile_width; column <= (info.x + w - 1) / source.tile_width; column++)
                tiles.push_back((source.base + row * source.pitch_tiles + column) % EDRAM_TILES);
        if (!fsi_ && !resolve_stored_ && !edram_store(&source, tiles)) return;
        c = outside();
        if (options_.gpu_timing) gpu_mark("resolves: copies from EDRAM: " + target_name(source));
    } else {
        c = outside();
        if (options_.gpu_timing) gpu_mark("resolves: copies from the image: " + target_name(source));
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
    p.flags = options_.depth_float24_round && !options_.depth_float24_convert_in_pixel_shader ? 1u : 0u;
    // EDRAM read at the resolution scale, over the console's pixels
    p.flags |= scale_.q4 << 8 | 4u << 16;
    // Direct3D 9's pixel centers move a draw half a console pixel, which at a
    // scale leaves the first host pixels on a rectangle's left and top
    // uncovered: they take the first covered one, as Xenia's
    // resolve_resolution_scale_fill_half_pixel_offset (resolve.comp's
    // edram_pixel; XF_RESOLVE_FILL_HALF_PIXEL=0 off)
    static const bool fill_half_pixel = [] {
        const char *v = std::getenv("XF_RESOLVE_FILL_HALF_PIXEL");
        return !(v && v[0] == '0');
    }();
    if (fill_half_pixel && !scale_.one() && !(regs_[r::PA_SU_VTX_CNTL] & 1)) p.flags |= 4u;
    p.raw = resolve::raw_copy(info);
    p.dest_base = info.dest_base, p.dest_pitch = info.dest_pitch, p.dest_height = info.dest_height;
    p.dest_x = info.dest_x, p.dest_y = info.dest_y, p.dest_slice = info.dest_slice, p.dest_array = info.dest_array;
    p.dest_format = info.dest_format;
    p.dest_log2_bytes = uint32_t(std::countr_zero(texture::format_block(info.dest_format).bytes));
    p.dest_endian = info.dest_endian, p.dest_number = info.dest_number, p.dest_swap = info.dest_swap;
    p.exp_factor = std::ldexp(1.0f, info.dest_exp_bias);
    Texture *targets[2];
    uint32_t extra_columns = 0, extra_rows = 0;
    // the textures fed at the console's size (written by the pass over its
    // pixels) and at the resolution scale (by a pass of their own after it)
    uint32_t target_1x = 0, target_scaled = 0, convert_1x = 0, convert_scaled = 0;
    if (uint32_t count = resolve_targets(info, targets, extra_columns, extra_rows)) {
        const uint32_t corner =
            ((info.texel_x0 + extra_columns) >> 5) << 16 | ((info.texel_y0 + extra_rows) >> 5) << 24;
        uint32_t places[2] = {0, 0};
        // both in use before either is made the resolution scale's (whose new
        // place in the table could otherwise be the other one's: take_target)
        for (uint32_t k = 0; k < count; k++) use(*targets[k], submission_serial() + 1);
        for (uint32_t k = 0; k < count; k++) {
            Texture &t = *targets[k];
            // Made the resolution scale's when this resolve writes all of it
            // and both are no smaller than the minimum scalable dimension
            // (RPCS3's rule): the resolve then writes every scaled texel (a
            // depth texture's float too, made from them as at the console's
            // size: convert_scaled). 64-bit texels too (XF_SCALE_WIDE=0: not):
            // the wrong colors their scaled feed showed in game F's bloom were
            // the half pixel offset's uncovered host pixels (edram_pixel).
            const uint32_t tw = (t.key[2] & 0x1FFF) + 1, th = ((t.key[2] >> 13) & 0x1FFF) + 1;
            if (!scale_.one() && !t.scaled && !extra_rows && !extra_columns &&
                info.texel_x0 + info.dest_x == 0 && info.texel_y0 + info.dest_y == 0 && info.width >= tw &&
                info.height >= th && scalable(tw, th, min_scalable_) &&
                scalable(info.width, info.height, min_scalable_) && (scale_wide_ || !(t.target & 0x80)))
                scale_texture(t, tw, th);
            // current again if it was stale: the resolve wrote all of it, or
            // with the parts the resolves before it wrote since it went stale
            // (Impl::cover; resolve_targets feeds a stale one in part only so)
            if (t.stale) {
                const uint32_t x = info.texel_x0 + info.dest_x + extra_columns,
                               y = info.texel_y0 + info.dest_y + extra_rows;
                if (cover(t, x, y, x + info.width, y + info.height))
                    t.stale = false, t.load_why = LoadWhy::CpuWritten, uncover(t), report_.resolve_targets_covered++;
            }
            uint32_t &into = t.scaled ? target_scaled : target_1x;
            // what each place at the console's size takes (flags bits 24-25
            // and 26-27: resolve.comp's write_targets)
            (t.scaled ? convert_scaled : convert_1x) |= t.target_convert << (24 + 2 * places[t.scaled]);
            into |= (t.target & (places[t.scaled] ? 0x7Fu : 0xFFu)) << (8 * places[t.scaled]++);
        }
        if (target_1x) target_1x |= corner;
        if (target_scaled) target_scaled |= corner | (targets[0]->target & 0x80u);
        report_.resolve_targets_written += count;
    }
    // at a resolution scale, a resolve no scaled texture is over keeps its
    // scaled texels for the texture a later draw makes there
    drop_scaled_resolves(written);
    if (!scale_.one() && !target_scaled && resolve_targets_ && !region)
        if (ScaledResolve *kept = scaled_resolve(info, written))
            target_scaled = (kept->target & 0xFFu) | (info.texel_x0 >> 5) << 16 | (info.texel_y0 >> 5) << 24;
    // (left out where nothing read it since the last resolve wrote it:
    // scaled_skip_unread_)
    if (region && scaled_skip_unread_) {
        bool unread = true;
        for (auto [page, tag] : scaled_pages)
            unread &= !(scaled_reads_[page] & 0xC0) && (scaled_reads_[page] & 31) >= 2;
        if (unread) {
            for (auto [page, tag] : scaled_pages) scaled_reads_[page] |= 0x20;
            region = nullptr, fills.clear(), scaled_pages.clear();
            report_.scaled_resolves_unread++;
        }
    }
    p.target = target_1x;
    p.flags |= convert_1x;
    if (from_edram) {
        p.edram = 1;
        p.edram_base = source.base, p.edram_pitch = source.pitch_tiles, p.msaa = source.msaa;
        p.ints_log2 = source.wide ? 1 : 0;
        p.src_x = info.x, p.src_y = info.y;
        p.first_sample = resolve::first_sample(info.sample_select);
        p.samples = resolve::sample_count(info.sample_select);
    }
    // the store's packing for the target: its flags (bits 4-7) and format (28-31)
    if (from_target_image) p.flags |= (edram_target(source, 0).flags & 0xFu) << 4 | (source.format & 0xFu) << 28;
    const VkPipelineLayout layout = from_target_image ? resolve_image_layout_ : resolve_layout_;
    if (!fills.empty()) {
        // what of the scaled units the rectangle leaves, from memory
        if (options_.gpu_timing) gpu_mark("resolves: scaled memory made from memory");
        full_barrier(c);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, scaled_fill_pipeline_);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1, &scaled_fill_set_, 0, nullptr);
        for (const ScaledFill &f : fills) {
            vkCmdPushConstants(c, load_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(f), &f);
            vkCmdDispatch(c, (f.words + 63) / 64, 1, 1);
        }
        full_barrier(c);
        report_.scaled_fills += fills.size();
    }
    if (from_target_image) settle_depth_samples(c, source);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                      from_target_image ? resolve_image_pipelines_[source.msaa ? 1 : 0] : resolve_pipeline_);
    const VkDescriptorSet resolve_sets[3] = {from_edram ? resolve_edram_set_ : resolve_set_, target_set_,
                                             source.store_set};
    const uint32_t mask_offset = 0; // (the store set's tile mask, which the resolve does not read)
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, from_target_image ? 3 : 2, resolve_sets,
                            from_target_image ? 1 : 0, &mask_offset);
    vkCmdPushConstants(c, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    if (target_scaled || region) {
        // the scaled textures, and resolved memory at the scale, over their
        // texels at the resolution scale
        if (options_.gpu_timing)
            gpu_mark(region ? "resolves: the pass at the scale (textures and memory)"
                            : "resolves: the pass at the scale (textures)");
        ResolveConstants scaled = p;
        scaled.target = target_scaled;
        scaled.flags = (p.flags & (1u | 4u | 0xF0u | 0xF0000000u)) | 2u | scale_.q4 << 8 | scale_.q4 << 16 |
                       convert_scaled;
        scaled.width = scale_.down(w), scaled.height = scale_.down(h);
        if (region) {
            // (the destination's place in the heap, in guest bytes: scaled_rel)
            scaled.flags |= 8u;
            scaled.dest_base = scaled_rel(*region, dest_base);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &scaled_resolve_set_, 0, nullptr);
        }
        vkCmdPushConstants(c, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(scaled), &scaled);
        vkCmdDispatch(c, (scaled.width + 7) / 8, (scaled.height + 7) / 8, 1);
    }
    full_barrier(c);
    // the pages' tags: what the scaled bytes now hold, or nothing current
    if (region) {
        const uint32_t swap = uint32_t(report_.swaps);
        for (auto [page, tag] : scaled_pages) {
            uint8_t &reads = scaled_reads_[page];
            const uint32_t frames = (reads & 0x40) || !reads                ? 1
                                    : scaled_write_swaps_[page] == swap ? reads & 31
                                                                        : std::min((reads & 31) + 1, 31);
            reads = uint8_t((reads & 0x80) | frames);
            scaled_write_swaps_[page] = swap;
            scaled_tags_[page] = tag;
        }
    } else if (scaled_memory_) {
        clear_scaled_tags(written);
        clear_scaled_tags(partial);
    }

    gpu_written(written);
    if (refusal_detail_)
        stale_by_ = {info.dest_format, info.width,           info.height, info.dest_pitch, info.texture_base & 0x1FFFFFFF,
                     info.texel_x0 + info.dest_x, info.texel_y0 + info.dest_y};
    textures_written_by_gpu(written, targets);
    if (refusal_detail_) stale_by_ = {};
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
                begin_recording();
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
    // XF_DUMP_RESOLVES=<directory> (with the full read-back, Options::readback_resolve): what each
    // resolve wrote, a file each (<resolve's number>_<address>.bin, the ranges one after the other),
    // so that two runs of a frame (the two render target paths, two builds) are compared resolve by
    // resolve and the first that differs names where a wrong picture begins
    static const char *const dump = std::getenv("XF_DUMP_RESOLVES");
    if (dump && *dump && !written.empty()) {
        char name[64];
        std::snprintf(name, sizeof(name), "%05llu_%08X.bin", (unsigned long long)report_.resolves, written.front().first);
        if (std::FILE *f = std::fopen((std::filesystem::path(dump) / name).string().c_str(), "wb")) {
            for (auto [start, end] : written) std::fwrite(memory_.data() + start, 1, end - start, f);
            std::fclose(f);
        }
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
            begin_recording();
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
        VkCommandBuffer c = r_.outside();
        // With the recording thread, the port records into the real command buffer itself: the
        // ring recorded first, and the renderer's own commands recorded at once until the frame
        // ends (Ring::direct; native_frame), so the two keep their order.
        if (r_.ring_) {
            if (!r_.ring_->direct()) r_.ring_->set_direct(true);
            c = r_.ring_->real(rec::fake_of(c)->target);
        }
        return c;
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
    VkDeviceSize guest_memory(uint32_t address, uint32_t bytes, VkBuffer &buffer, VkDeviceSize &offset) override {
        buffer = r_.mirror_.buffer, offset = address & GuestMemory::PHYSICAL_MASK;
        if (!bytes || !r_.fatal_.empty()) return 0;
        std::vector<Range> ranges;
        add_span(ranges, address, bytes);
        const VkDeviceSize given = r_.request(ranges);
        r_.report_.vertex_bytes += given;
        return given;
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
        begin_recording();
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
            // (the recording thread's ring again, where the port recorded itself: NativeFrame::commands)
            if (ring_ && ring_->direct()) ring_->set_direct(false);
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
// what the register file says of a draw or resolve, for the filter and the observer
void Impl::native_pass(native::PassInfo &pass) {
    pass.surface_pitch = regs_[reg::RB_SURFACE_INFO] & 0x3FFF;
    pass.color_base = regs_[reg::RB_COLOR_INFO] & 0xFFF;
    pass.color_format = (regs_[reg::RB_COLOR_INFO] >> 16) & 0xF;
    pass.registers = regs_, pass.swap = report_.swaps;
}

bool Impl::native_keep(native::PassInfo &pass) {
    native::detail::AtomicCounters &counts = native::detail::atomic_counters();
    bool skip = true;
    if (native_settings_.filter) skip = native_settings_.filter(pass);
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
    // (what the draws keep of one another is a frame's: a setting changed between two, as the
    // resolution scale, is in none of their keys)
    front_valid_ = false;
    // the draws after a swap count for no report until the next one
    // (Xenia's EndZPDFrame)
    end_query();
    zpd_current_ = 0;
    uint64_t number = report_.swaps++;
    if (number == 0 && DrawSections::on()) DrawSections::get().reset(); // (the first play left out)
    // the scaled resolves no texture took within two swaps go
    std::vector<Range> unused;
    for (const auto &[base, kept] : scaled_resolves_)
        if (kept.swap + 2 < report_.swaps) unused.push_back({kept.low, kept.high});
    if (!unused.empty()) drop_scaled_resolves(unused);
    {
        // hitches: the frame's time making pipelines on this thread, and the
        // time since the last swap
        double spent = report_.pipeline_seconds - pipeline_seconds_at_swap_;
        pipeline_seconds_at_swap_ = report_.pipeline_seconds;
        if (spent > 0.004) report_.pipeline_hitches++;
        report_.pipeline_frame_max = std::max(report_.pipeline_frame_max, spent);
        report_.translate_frame_max =
            std::max(report_.translate_frame_max, report_.translate_seconds - translate_seconds_at_swap_);
        translate_seconds_at_swap_ = report_.translate_seconds;
        generic_spent_frame_ = 0, generic_unknown_frame_ = 0;
        auto now = std::chrono::steady_clock::now();
        if (number) {
            double gap = std::chrono::duration<double>(now - last_swap_time_).count();
            if (gap > 0.05) report_.swap_gaps_50ms++;
            if (gap > 0.1) report_.swap_gaps_100ms++;
            report_.swap_gap_max = std::max(report_.swap_gap_max, gap);
        }
        last_swap_time_ = now;
        // XF_GPU_TIME_LOG=1 (with the GPU timing on): the GPU time a frame by
        // kind of work over every 300 swaps, with the seconds since the
        // first swap, on stderr (the line Xenia's local measuring build
        // logs, docs/renderer.md "Xenia's passes timed", for a window of a
        // played run)
        static const bool log_gpu_time = [] {
            const char *v = std::getenv("XF_GPU_TIME_LOG");
            return v && v[0] == '1';
        }();
        if (log_gpu_time && options_.gpu_timing) {
            if (!number) first_swap_time_ = now;
            if (report_.swaps % 300 == 0) {
                std::string line;
                double total = 0.0;
                for (const auto &[kind, time] : report_.gpu_time) {
                    double seconds = time.seconds - gpu_time_logged_[kind];
                    gpu_time_logged_[kind] = time.seconds;
                    total += seconds;
                    char part[256];
                    // (kinds' names hold "; " themselves)
                    std::snprintf(part, sizeof(part), " | %s %.3f", kind.c_str(), seconds * 1000.0 / 300.0);
                    if (seconds > 0.0) line += part;
                }
                std::fprintf(stderr, "xf-gpu-timing swap %llu at %.1f s total %.3f ms/frame%s\n",
                             (unsigned long long)report_.swaps,
                             std::chrono::duration<double>(now - first_swap_time_).count(), total * 1000.0 / 300.0,
                             line.c_str());
            }
        }
    }
    {
        // the display's look-up table mode (display_gamma), counted
        char what[96];
        std::snprintf(what, sizeof(what), "swaps with DC_LUTA_CONTROL 0x%06X", regs_[r::DC_LUTA_CONTROL]);
        // (by its text: the literals' count is kept by address, and this one is on the stack)
        use(std::string(what));
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
    if (front.image && fatal_.empty()) {
        // (what the hook submits after the frame's submission: that made by the recording thread first)
        if (ring_) ring_->wait_for(ring_submitted_, flushed_serial_, rec::Ring::Present);
        options_.on_swap(front);
    }
    poll();
    next_readback_frame();
    // the pipeline cache written once new pipelines have settled
    if (pipelines_saved_ != report_.pipelines && report_.swaps - last_pipeline_swap_ >= 300)
        save_pipeline_cache(true);
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
