// The Xenos renderer, the command processor's Sink (renderer.h). The
// viewport and scissor follow Xenia's src/xenia/gpu/draw_util.cc
// (GetHostViewportInfo, GetScissor), the register fields registers.h, the
// clear values vulkan/vulkan_render_target_cache.cc, the vertex data a draw
// needs vulkan/vulkan_command_processor.cc (IssueDraw's shared memory
// requests), the point sizes its UpdateSystemConstantValues, and the swap
// vulkan/vulkan_texture_cache.cc (RequestSwapTexture); all BSD, see
// LICENSE-xenia.
//
// Batched (plan B4): what the processor sends is recorded into the command
// buffer of a submission slot, render passes kept open from draw to draw
// while the targets stay the same, and the slot is submitted at each swap or
// when its share of the upload arena fills. A few slots are in flight; the
// renderer waits for the GPU only to reuse a slot, or when the CPU needs
// what the GPU wrote (a front buffer read back for --shots, a texture over a
// resolve's output in a format only the CPU decodes). A draw's constants,
// indices and vertex data are copied into its slot's share of the arena, the
// vertex fetch constants pointed at the copies; textures are bindless, each
// image at an index of one descriptor set written as they are made.
//
// Guest memory is read where it lies, with nothing told when the CPU writes
// it, so what is copied from it is checked or copied again at each use:
// vertex data and indices are copied at each draw (Xenia requests the same
// ranges from its shared memory and uploads the pages its write watches saw
// change); textures are kept with a hash of the memory they were decoded
// from and decoded again when it differs at a use (Xenia drops them when a
// write watch on their pages fires); the processor's own writes
// (memory_written) mark the textures under them at once.
//
// Resolves run on the GPU, into the mirror: a copy of guest memory on the
// GPU, as Xenia's shared memory, holding what resolves wrote and what loads
// from it need around that. Textures over what a resolve wrote are loaded
// from the mirror on the GPU (as Xenia's texture cache loads all of its
// textures), and guest memory gets the resolve's bytes once the GPU has
// written them, without waiting for it (docs/renderer.md, "Decisions").

#include "xenoforge/renderer.h"

#include "xenoforge/guest_memory.h"
#include "xenoforge/resolve.h"
#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_translator.h"
#include "xenoforge/texture.h"
#include "xenoforge/ucode.h"
#include "xenoforge/ucode_parse.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t RB_BLEND_RED = 0x2105; // then green, blue, alpha
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F; // then XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET
constexpr uint32_t RB_BLENDCONTROL[4] = {0x2201, 0x2209, 0x220A, 0x220B};
constexpr uint32_t RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
constexpr uint32_t RB_COPY_CONTROL = 0x2318;
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

// A hash of bytes, fast enough to run over a texture's memory at each use:
// four lanes, each word xored in, rotated and multiplied (a bijection of the
// lane for a given word, so one changed word always changes its lane), then
// the lanes and the tail mixed.
uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t seed) {
    constexpr uint64_t K1 = 0x9E3779B97F4A7C15ull, K2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t lanes[4] = {seed ^ K1, seed + K2, seed * K1 + 1, ~seed};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * l, 8);
            lanes[l] = std::rotl(lanes[l] ^ w, 31) * K1;
        }
    uint64_t out = n;
    for (; i < n; i++) out = (out ^ p[i]) * K2;
    for (uint64_t l : lanes) out = std::rotl(out ^ (l * K2), 27) * K1;
    return out ^ (out >> 29);
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
    case H::B5G6R5: return VK_FORMAT_B5G6R5_UNORM_PACK16;     // R in the low bits, as texture.h keeps it
    case H::A4R4G4B4: return VK_FORMAT_A4B4G4R4_UNORM_PACK16; // likewise; 1_5_5_5 goes through to_rgba8
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

// Xenia's host swizzles: one-component formats read as RRRR, two as RGGG
uint32_t host_components(texture::HostFormat f) {
    using H = texture::HostFormat;
    switch (f) {
    case H::R8: case H::R16: case H::R16F: case H::R32: case H::R32F: case H::BC4: return 1;
    case H::RG8: case H::RG16: case H::RG16F: case H::RG32: case H::RG32F: case H::BC5: return 2;
    default: return 4;
    }
}

VkComponentSwizzle swizzle_of(uint32_t guest, uint32_t components) {
    switch (guest) {
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    case 5: return VK_COMPONENT_SWIZZLE_ONE;
    case 6: case 7: return VK_COMPONENT_SWIZZLE_ZERO;
    }
    uint32_t c = std::min(guest, components - 1);
    return VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + c);
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
VkSamplerAddressMode address_mode(uint32_t clamp) {
    switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // the halfway and mirrored-once clamps approximated
    }
}

struct Shader {
    xf::ucode::ParsedShader parsed;
    xf::ucode::ShaderSummary summary;
    bool translated = false;
    VkShaderModule module = VK_NULL_HANDLE;
    std::string unsupported; // why it cannot be drawn with
    uint32_t colors_written = 0; // oC0-oC3, a bit each
    bool writes_depth = false;
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
};

// texture_load.comp's push constants
struct LoadConstants {
    uint32_t base, slice_stride, slices, layered, pitch_blocks, z_rows, ox, oy, oz, tiled, is_3d, log2_bytes, endian;
    uint32_t blocks_x, blocks_y, out_word, words, load, row_words;
};

struct Texture {
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE; // its own: made and destroyed with it
    uint32_t dimension = 1;
    texture::HostFormat host = texture::HostFormat::Unsupported; // what its image holds (RGBA8 when converted)
    uint32_t kind = 0, index = 0;      // its bindless table (2D arrays, 3D, cube) and place in it
    std::vector<Range> ranges;         // the guest memory it was loaded from
    uint64_t hash = 0;                 // of that memory then
    uint32_t low = 0, high = 0;        // the ranges' bounds
    bool stale = false;                // the processor or a resolve wrote memory under it
    bool pending = false;              // loaded from the mirror where guest memory does not have the bytes yet
    bool rehash = false;               // they are there now: hash them at the next use
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
};
static_assert(sizeof(System) == 224);

// A part of what a resolve wrote that guest memory does not have yet, and
// where its bytes wait for the GPU to be done (a read-back buffer).
struct Piece {
    uint32_t start, end;
    const uint8_t *from;
};
struct WriteBack {
    uint64_t serial = 0; // the submission that writes it
    std::vector<Piece> pieces;
};

// takes [start, end) out of the pieces
void cut(std::vector<Piece> &pieces, uint32_t start, uint32_t end) {
    bool any = false;
    for (const Piece &p : pieces) any |= start < p.end && p.start < end;
    if (!any) return;
    std::vector<Piece> out;
    for (const Piece &p : pieces) {
        if (end <= p.start || start >= p.end) {
            out.push_back(p);
            continue;
        }
        if (p.start < start) out.push_back({p.start, start, p.from});
        if (end < p.end) out.push_back({end, p.end, p.from + (end - p.start)});
    }
    pieces = std::move(out);
}

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
};

// The attachments of the render pass under way.
struct Pass {
    VkImageView colors[4] = {};
    VkImageView depth = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool operator==(const Pass &) const = default;
};

class Impl final : public Renderer {
public:
    Impl(vk::Device &device, gpu::CommandProcessor &processor, const Options &options)
        : d_(device), dev_(device.device()), processor_(processor), options_(options),
          memory_(processor.memory().translate_physical(0)) {}
    ~Impl() override;

    bool init(std::string &error);

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
    // the processor's own writes (fences, reports), and a trace's memory:
    // the textures over them are loaded again at their next use, and what a
    // resolve wrote there and guest memory does not have yet stays the GPU's
    void memory_written(uint32_t address, uint32_t bytes) override {
        Call call(*this, nullptr);
        uint64_t end = uint64_t(address) + bytes;
        for (auto &[key, t] : textures_)
            if (address < t->high && end > t->low) t->stale = true;
        std::vector<Range> written;
        add_span(written, address, bytes);
        for (WriteBack &w : writebacks_)
            for (auto [start, stop] : written) cut(w.pieces, start, stop);
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
        vkCmdCopyBuffer(outside(), from, edram_.buffer, 1, &copy);
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
    }

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
    void resolve();
    void gpu_resolve(const resolve::Info &info, RenderTarget &source);
    void swap();
    void read_front_buffer(uint64_t number);
    void deliver(const FrontRead &read);
    void skip(const std::string &reason) {
        report_.skipped[reason]++;
        if (options_.verbose) std::fprintf(stderr, "  draw %llu skipped: %s\n", (unsigned long long)report_.draws, reason.c_str());
    }
    void approximate(const std::string &what) { report_.approximate[what]++; }

    // resources
    Shader *shader(uint64_t hash);
    bool translate(Shader &s, std::string &why);
    RenderTarget *render_target(bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format);
    VkFormat depth_vk_format(uint32_t format) const;
    Texture *texture(const uint32_t fetch[6], std::string &why);
    bool gpu_load(Texture &t, const texture::Plan &plan, bool fresh);
    bool cpu_upload(Texture &t, const texture::Image &image, const std::vector<std::vector<uint8_t>> &levels,
                    VkFormat format, bool fresh);
    uint32_t sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides);
    VkPipeline pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info);
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
    uint64_t hash_ranges(const std::vector<Range> &ranges) const {
        uint64_t h = 0;
        for (auto [start, end] : ranges) h = hash_bytes(memory_.data() + start, end - start, h);
        return h;
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
        if (in_pass_) vkCmdEndRendering(slots_[current_].cmd);
        in_pass_ = false;
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

    // ---- the mirror: what resolves wrote, on the GPU ----
    bool pending(const std::vector<Range> &ranges) const;
    std::vector<Range> not_pending(const std::vector<Range> &ranges) const;
    void wait_for(const std::vector<Range> &ranges);
    void upload_to_mirror(const std::vector<Range> &ranges);
    void finish_writebacks(uint64_t serial);

    vk::Device &d_;
    VkDevice dev_;
    gpu::CommandProcessor &processor_;
    Options options_;
    mutable std::mutex mutex_; // held through each of the processor's calls
    Report report_;
    std::string fatal_;
    const uint32_t *regs_ = nullptr; // the processor's register file, during a call
    GuestMemory memory_;             // the processor's physical memory
    uint64_t vertex_hash_ = 0, pixel_hash_ = 0;
    bool have_vertex_ = false, have_pixel_ = false;
    float max_viewport_[2] = {};
    VkDeviceSize ubo_align_ = 256, ssbo_align_ = 64;

    std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
    std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, std::unique_ptr<RenderTarget>> targets_;
    std::map<std::array<uint32_t, 6>, std::unique_ptr<Texture>> textures_;
    std::unordered_set<Texture *> pending_textures_;
    std::map<std::array<uint32_t, 8>, std::pair<VkSampler, uint32_t>> samplers_;
    std::map<std::vector<uint32_t>, VkPipeline> pipelines_;
    std::map<std::tuple<VkFormat, uint32_t, uint32_t>, vk::Image> msaa_temps_; // resolves' single-sample copies

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
    std::deque<WriteBack> writebacks_; // oldest first
    std::deque<FrontRead> front_reads_; // oldest first

    // draws: set 0 (constants, system constants, vertex memory: the arena or
    // the mirror), set 1 bindless (images and samplers)
    VkDescriptorSetLayout set_layouts_[2] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool0_ = VK_NULL_HANDLE, bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet draw_sets_[2] = {}; // vertex memory: the arena, the mirror
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
    VkDescriptorSet resolve_set_ = VK_NULL_HANDLE, load_set_ = VK_NULL_HANDLE;

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
    vkDeviceWaitIdle(dev_);
    for (Slot &s : slots_) {
        for (auto &f : s.after) f();
        for (vk::Buffer &b : s.readbacks) d_.destroy(b);
        if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
    }
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    for (auto &[k, s] : shaders_)
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
    for (auto &[k, p] : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (auto &[k, s] : samplers_) vkDestroySampler(dev_, s.first, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (auto &[k, t] : targets_) d_.destroy(t->image);
    for (auto &[k, t] : textures_) {
        if (t->view) vkDestroyImageView(dev_, t->view, nullptr);
        d_.destroy(t->image);
    }
    for (auto &[k, im] : msaa_temps_) d_.destroy(im);
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
    if (vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &c, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev_, m, nullptr);
    return p;
}

bool Impl::init(std::string &error) {
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

    rectangle_gs_ = module(RECTANGLE_LIST_GS, sizeof(RECTANGLE_LIST_GS));
    point_gs_ = module(POINT_LIST_GS, sizeof(POINT_LIST_GS));
    if (!rectangle_gs_ || !point_gs_) return error = "could not make the geometry shaders", false;

    // EDRAM: the buffer, store (compute) and load (draw) pipelines; the tile
    // masks come from the arena
    edram_ = d_.create_buffer(EDRAM_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
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
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(dev_, &pool, nullptr, &compute_pool_) != VK_SUCCESS)
            return error = "vkCreateDescriptorPool failed", false;
        VkDescriptorSetLayout twice[2] = {compute_set_layout_, compute_set_layout_};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = compute_pool_;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = twice;
        if (vkAllocateDescriptorSets(dev_, &alloc, sets) != VK_SUCCESS) return error = "vkAllocateDescriptorSets failed", false;
        resolve_set_ = sets[0], load_set_ = sets[1];
        VkDescriptorBufferInfo buffers[4] = {{scratch_.buffer, 0, VK_WHOLE_SIZE},
                                             {mirror_.buffer, 0, VK_WHOLE_SIZE},
                                             {mirror_.buffer, 0, VK_WHOLE_SIZE},
                                             {load_scratch_.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4] = {};
        for (uint32_t k = 0; k < 4; k++) {
            writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[k].dstSet = k < 2 ? resolve_set_ : load_set_;
            writes[k].dstBinding = k & 1;
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[k].pBufferInfo = &buffers[k];
        }
        vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
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
    }
    return s.cmd;
}

void Impl::flush() {
    Slot &s = slots_[current_];
    if (!s.recording) return;
    end_pass();
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
// (images and buffers no longer used), and what its resolves wrote is
// copied into guest memory.
void Impl::finish(Slot &s) {
    for (auto &f : s.after) f();
    s.after.clear();
    finish_writebacks(s.serial);
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
    rendering_info.colorAttachmentCount = 4;
    rendering_info.pColorAttachments = attachments;
    if (depth) rendering_info.pDepthAttachment = rendering_info.pStencilAttachment = &depth_attachment;
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

// whether a resolve wrote any of the ranges and guest memory does not have it yet
bool Impl::pending(const std::vector<Range> &ranges) const {
    for (const WriteBack &w : writebacks_)
        for (const Piece &p : w.pieces)
            for (const Range &r : ranges)
                if (overlaps({p.start, p.end}, r)) return true;
    return false;
}

std::vector<Range> Impl::not_pending(const std::vector<Range> &ranges) const {
    std::vector<Range> out = ranges;
    for (const WriteBack &w : writebacks_)
        for (const Piece &p : w.pieces) {
            std::vector<Range> rest;
            for (const Range &r : out) {
                if (!overlaps({p.start, p.end}, r)) {
                    rest.push_back(r);
                    continue;
                }
                if (r.first < p.start) rest.push_back({r.first, p.start});
                if (p.end < r.second) rest.push_back({p.end, r.second});
            }
            out = std::move(rest);
        }
    return out;
}

// until guest memory has what resolves wrote over the ranges
void Impl::wait_for(const std::vector<Range> &ranges) {
    uint64_t serial = 0;
    for (const WriteBack &w : writebacks_)
        for (const Piece &p : w.pieces)
            for (const Range &r : ranges)
                if (overlaps({p.start, p.end}, r)) serial = std::max(serial, w.serial);
    if (serial) wait_serial(serial);
}

// guest memory's bytes into the mirror, before what reads them there
void Impl::upload_to_mirror(const std::vector<Range> &ranges) {
    VkDeviceSize total = 0;
    for (auto [start, end] : ranges) total += end - start;
    if (!total) return;
    VkBuffer from;
    VkDeviceSize at;
    uint8_t *data;
    data = staging(total, from, at);
    if (!data) return;
    std::vector<VkBufferCopy> copies;
    VkDeviceSize offset = 0;
    for (auto [start, end] : ranges) {
        std::memcpy(data + offset, memory_.data() + start, end - start);
        copies.push_back({at + offset, start, end - start});
        offset += end - start;
    }
    vkCmdCopyBuffer(outside(), from, mirror_.buffer, uint32_t(copies.size()), copies.data());
}

// The submissions up to serial are done: what their resolves wrote goes into
// guest memory, and the textures loaded from it are checked by hash again.
void Impl::finish_writebacks(uint64_t serial) {
    if (writebacks_.empty() || writebacks_.front().serial > serial) return;
    Timer timer(report_.writeback_seconds);
    while (!writebacks_.empty() && writebacks_.front().serial <= serial) {
        WriteBack w = std::move(writebacks_.front());
        writebacks_.pop_front();
        for (const Piece &p : w.pieces) {
            std::memcpy(memory_.data() + p.start, p.from, p.end - p.start);
            report_.writeback_bytes += p.end - p.start;
        }
    }
    for (auto it = pending_textures_.begin(); it != pending_textures_.end();) {
        Texture *t = *it;
        if (pending(t->ranges)) {
            ++it;
            continue;
        }
        t->pending = false, t->rehash = true;
        it = pending_textures_.erase(it);
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
                        if (r->target == xf::ucode::StorageTarget::Color && r->used_write_mask()) s.colors_written |= 1u << (r->index & 3);
                        if (r->target == xf::ucode::StorageTarget::Depth && r->used_write_mask()) s.writes_depth = true;
                    }
                } else if (const auto *t = std::get_if<xf::ucode::TextureFetchInstruction>(&slot.instruction)) {
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
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipelines_[t->msaa ? 1 : 0]);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, edram_layouts_[0], 0, 1, &t->store_set, 1, &offset);
    vkCmdPushConstants(c, edram_layouts_[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(c, (t->width + 7) / 8, (height + 7) / 8, 1);
    return true;
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
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
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

// A sampler's place in the bindless table, made once per address modes and
// filters.
uint32_t Impl::sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides) {
    uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3, aniso = (fetch[3] >> 25) & 7;
    if (overrides[0] != 3) mag = overrides[0];
    if (overrides[1] != 3) min = overrides[1];
    if (overrides[2] != 3) mip = overrides[2];
    if (overrides[3] != 7) aniso = overrides[3];
    std::array<uint32_t, 8> key = {(fetch[0] >> 10) & 7, (fetch[0] >> 13) & 7, (fetch[0] >> 16) & 7, mag, min, mip, aniso, 0};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second.second;
    if (next_sampler_ >= sampler_capacity_) {
        approximate("samplers past the bindless table's size (the stand-in's used)");
        return 0;
    }
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.addressModeU = address_mode(key[0]);
    info.addressModeV = address_mode(key[1]);
    info.addressModeW = address_mode(key[2]);
    info.magFilter = mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.maxLod = mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE; // 2: the base map alone
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
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
// is not what it was: from guest memory on the CPU, or from the mirror on
// the GPU when a resolve wrote some of it and guest memory does not have the
// bytes yet. Loaded again, it keeps its image and its place in the table.
Texture *Impl::texture(const uint32_t fetch[6], std::string &why) {
    std::array<uint32_t, 6> key;
    std::copy(fetch, fetch + 6, key.begin());
    auto it = textures_.find(key);
    Texture *t = it == textures_.end() ? nullptr : it->second.get();
    if (t && !t->stale) {
        bool same = t->pending;
        if (!same) {
            Timer timer(report_.check_seconds);
            uint64_t hash = hash_ranges(t->ranges);
            if (t->rehash) t->hash = hash, t->rehash = false;
            same = hash == t->hash;
        }
        if (same) return t;
    }
    if (t) t->stale = true; // until it is loaded again

    texture::Info info = texture::parse_fetch_constant(fetch);
    texture::Plan plan;
    if (!texture::plan(info, plan, why)) return nullptr;
    std::vector<Range> ranges = ranges_of(plan.footprint);
    VkFormat format = vk_format_of(plan.format);
    bool direct = format != VK_FORMAT_UNDEFINED && d_.format_supported(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    bool gpu = pending(ranges);
    if (gpu && (!direct || plan.load == texture::Load::Dxt3aToR8 || plan.load == texture::Load::Ctx1ToRg8)) {
        // a format the GPU's loads do not make: decoded from guest memory
        // once the resolve's bytes are there
        wait_for(ranges);
        gpu = false;
    }

    texture::Image image;
    std::vector<std::vector<uint8_t>> levels;
    if (!gpu) {
        Timer decoding(report_.decode_seconds);
        if (!texture::decode(info, memory_, image, why)) return nullptr;
        if (direct) {
            levels = std::move(image.levels);
        } else {
            // through 8 bits per component
            format = VK_FORMAT_R8G8B8A8_UNORM;
            levels.resize(image.levels.size());
            for (uint32_t l = 0; l < levels.size(); l++) levels[l] = texture::to_rgba8(image, l);
            approximate("a texture format sampled as 8 bits per component");
        }
    }
    // signed components: float formats are signed either way (Xenia's signed
    // and unsigned views of them are the same format)
    bool float_format = format == VK_FORMAT_R16_SFLOAT || format == VK_FORMAT_R16G16_SFLOAT ||
                        format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32_SFLOAT ||
                        format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT;
    if (!float_format && (info.signs[0] == 1 || info.signs[1] == 1 || info.signs[2] == 1 || info.signs[3] == 1))
        approximate("signed texture components");
    if (info.signs[0] == 2 || info.signs[1] == 2 || info.signs[2] == 2 || info.signs[3] == 2)
        approximate("biased texture components");
    if (info.gamma) approximate("gamma textures read as linear");
    if (info.exp_adjust) approximate("texture exponent adjust");

    // 1D, 2D and stacked as 2D arrays (stacked: a layer a slice), as Xenia
    // binds them
    bool cube = info.dimension == 3, three = info.dimension == 2;
    uint32_t layers = three ? 1 : std::max(1u, plan.depth);
    VkExtent3D extent{plan.width, plan.height, three ? plan.depth : 1};
    uint32_t level_count = plan.level_count;
    bool fresh = !t || t->image.format != format || t->image.extent.width != extent.width ||
                 t->image.extent.height != extent.height || t->image.extent.depth != extent.depth ||
                 t->image.levels != level_count || t->image.layers != layers;
    if (fresh) {
        uint32_t kind = three ? 1 : cube ? 2 : 0;
        if (next_index_[kind] >= capacity_[kind]) {
            why = "the bindless table is full";
            return nullptr;
        }
        uint32_t index = next_index_[kind]++;
        if (t) {
            // the same constant made another image (not with the same format
            // decisions; kept safe): the old one goes once unused, its place
            // in the table with it (work under way may still read it there)
            vk::Image old = t->image;
            VkImageView old_view = t->view;
            cmd();
            slots_[current_].after.push_back([this, old, old_view]() mutable {
                vkDestroyImageView(dev_, old_view, nullptr);
                d_.destroy(old);
            });
            t->image = {}, t->view = VK_NULL_HANDLE;
        } else {
            auto owned = std::make_unique<Texture>();
            t = owned.get();
            t->stale = true;
            textures_[key] = std::move(owned);
        }
        t->dimension = info.dimension, t->kind = kind, t->index = index;
        t->host = format == VK_FORMAT_R8G8B8A8_UNORM && !direct ? texture::HostFormat::RGBA8 : plan.format;
        // a source of copies too: a front buffer read back, or copied by a presenter
        t->image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, format, extent, level_count, layers,
                                   VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
        if (!t->image.image) {
            why = "could not make the image";
            return nullptr;
        }
        // the guest's swizzle over the host format's
        uint32_t components = format == VK_FORMAT_R8G8B8A8_UNORM && plan.format != texture::HostFormat::RGBA8
                                  ? 4
                                  : host_components(plan.format);
        VkComponentMapping mapping{swizzle_of(info.swizzle & 7, components), swizzle_of((info.swizzle >> 3) & 7, components),
                                   swizzle_of((info.swizzle >> 6) & 7, components),
                                   swizzle_of((info.swizzle >> 9) & 7, components)};
        VkImageViewType view_type = cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                    : three ? VK_IMAGE_VIEW_TYPE_3D
                                            : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        t->view = make_view(t->image.image, view_type, format, VK_IMAGE_ASPECT_COLOR_BIT, mapping, level_count, layers,
                            false);
        if (!t->view) {
            why = "could not make the image's view";
            return nullptr;
        }
        VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = bindless_;
        write.dstBinding = kind;
        write.dstArrayElement = index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &descriptor;
        vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);
    }

    // what it was loaded from, for seeing when that changes
    t->ranges = std::move(ranges);
    t->low = UINT32_MAX, t->high = 0;
    for (auto [start, end] : t->ranges) t->low = std::min(t->low, start), t->high = std::max(t->high, end);
    t->rehash = false;
    if (gpu) {
        if (!gpu_load(*t, plan, fresh)) return nullptr;
        t->pending = true, t->hash = 0;
        pending_textures_.insert(t);
    } else {
        if (!cpu_upload(*t, image, levels, format, fresh)) return nullptr;
        if (t->pending) pending_textures_.erase(t);
        t->pending = false;
        t->hash = hash_ranges(t->ranges);
    }
    t->stale = false;
    report_.textures++;
    return t;
}

// The decoded levels into the image, through the arena, as Xenia uploads
// them: each level's slices tightly packed, at 16-byte aligned offsets.
bool Impl::cpu_upload(Texture &t, const texture::Image &image, const std::vector<std::vector<uint8_t>> &levels,
                      VkFormat format, bool fresh) {
    Timer decoding(report_.decode_seconds);
    VkExtent3D extent = t.image.extent;
    uint32_t layers = t.image.layers, level_count = t.image.levels;
    bool compressed = format != VK_FORMAT_R8G8B8A8_UNORM && texture::is_compressed(image.format);
    uint32_t block_size = format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : texture::bytes_per_block(image.format);
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
    if (fresh) begin_image(c, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, level_count, layers);
    if (!copies.empty())
        vkCmdCopyBufferToImage(c, from, t.image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()), copies.data());
    return true;
}

// The texture loaded from the mirror on the GPU (texture_load.comp), level by
// level as the plan lays it out, then copied into the image as cpu_upload
// copies: what of its memory guest memory holds the newest of goes into the
// mirror first.
bool Impl::gpu_load(Texture &t, const texture::Plan &plan, bool fresh) {
    struct Run {
        size_t first, count;
        VkDeviceSize offset, bytes;
    };
    std::vector<Run> runs;
    VkDeviceSize total = 0;
    for (size_t i = 0; i < plan.slices.size();) {
        size_t j = i;
        while (j < plan.slices.size() && plan.slices[j].level == plan.slices[i].level) j++;
        const texture::SliceLoad &s = plan.slices[i];
        VkDeviceSize bytes = VkDeviceSize(s.blocks_x) * s.blocks_y * plan.bytes_per_block * (j - i);
        runs.push_back({i, j - i, total, bytes});
        total += (bytes + 15) & ~VkDeviceSize(15);
        i = j;
    }
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!grow(load_scratch_, std::max<VkDeviceSize>(total, 16), usage, load_set_, 1)) return false;
    upload_to_mirror(not_pending(t.ranges));
    VkCommandBuffer c = outside();
    if (fresh) begin_image(c, t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, t.image.levels, t.image.layers);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout_, 0, 1, &load_set_, 0, nullptr);
    std::vector<VkBufferImageCopy> copies;
    for (const Run &run : runs) {
        const texture::SliceLoad &s = plan.slices[run.first];
        LoadConstants p{};
        p.base = s.base;
        p.slice_stride = run.count > 1 && !s.is_3d ? plan.slices[run.first + 1].base - s.base : 0;
        p.slices = uint32_t(run.count);
        p.layered = s.is_3d ? 0 : 1;
        p.pitch_blocks = s.pitch_blocks, p.z_rows = s.z_rows;
        p.ox = s.ox, p.oy = s.oy, p.oz = s.oz;
        p.tiled = s.tiled, p.is_3d = s.is_3d;
        p.log2_bytes = s.log2_bytes_per_block;
        p.endian = plan.endian;
        p.blocks_x = s.blocks_x, p.blocks_y = s.blocks_y;
        p.out_word = uint32_t(run.offset / 4);
        p.words = uint32_t((run.bytes + 3) / 4);
        p.load = plan.load == texture::Load::DepthUnormToFloat ? 1 : plan.load == texture::Load::DepthFloatToFloat ? 2 : 0;
        uint32_t groups = (p.words + 63) / 64, across = std::min(groups, 32768u);
        p.row_words = across * 64;
        vkCmdPushConstants(c, load_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(c, across, (groups + across - 1) / across, 1);
        VkBufferImageCopy copy{};
        copy.bufferOffset = run.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, 0, t.image.layers};
        copy.imageExtent = {std::max(1u, t.image.extent.width >> s.level), std::max(1u, t.image.extent.height >> s.level),
                            std::max(1u, t.image.extent.depth >> s.level)};
        copies.push_back(copy);
    }
    full_barrier(c);
    vkCmdCopyBufferToImage(c, load_scratch_.buffer, t.image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()),
                           copies.data());
    if (options_.verbose)
        std::fprintf(stderr, "  texture at %08X loaded from the mirror (%u levels)\n", plan.slices.empty() ? 0 : plan.slices[0].base,
                     uint32_t(runs.size()));
    return true;
}

// ---- pipelines ----

VkPipeline Impl::pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info) {
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    report_.pipelines++;
    return pipelines_[key] = p;
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
    {
        // A draw that can write nothing (no color channel enabled, depth and
        // stencil off, no memory export) leaves EDRAM and memory as they
        // were, and the occlusion queries' counts are the processor's fake
        // ones (EVENT_WRITE_ZPD, as Xenia's): nothing to draw, whatever its
        // primitive. El Chavo Kart issues hundreds of such point lists a
        // frame.
        const Shader *v = have_vertex_ ? shader(vertex_hash_) : nullptr;
        const Shader *p = have_pixel_ ? shader(pixel_hash_) : nullptr;
        bool colors = color && (regs_[reg::RB_COLOR_MASK] & 0xFFFF) != 0;
        bool depth_stencil = (regs_[reg::RB_DEPTHCONTROL] & 3) != 0;
        bool exports = (v && v->summary.memexport) || (color && p && p->summary.memexport);
        if (!colors && !depth_stencil && !exports) {
            if (options_.verbose)
                std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
            return;
        }
    }
    bool rectangles = primitive == 8, points = primitive == 1;
    VkPrimitiveTopology topology;
    switch (primitive) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break; // sprites made by point_list.geom
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: case 8: case 13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: return skip(std::string("primitive ") + primitive_name(primitive));
    }
    bool strip = primitive == 3 || primitive == 5 || primitive == 6;

    // shaders
    Shader *vs = have_vertex_ ? shader(vertex_hash_) : nullptr;
    if (!vs) return skip("no vertex shader");
    Shader *ps = color && have_pixel_ ? shader(pixel_hash_) : nullptr;
    std::string why;
    if (!translate(*vs, why)) return skip("vertex shader: " + why);
    if (ps && !translate(*ps, why)) return skip("pixel shader: " + why);
    if (vs->summary.memexport || (ps && ps->summary.memexport)) return skip("memory export");
    if (ps && (regs_[reg::SQ_PROGRAM_CNTL] >> 18) & 1) approximate("pixel parameter generation");

    // render targets
    uint32_t surface = regs_[reg::RB_SURFACE_INFO], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
    if (!pitch) return skip("surface pitch 0");
    if (msaa == 3) return skip("MSAA mode 3");
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
    uint32_t depth_control = regs_[reg::RB_DEPTHCONTROL];
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
    if (width == 8192 && height == 8192) {
        // no target the pixel shader writes and no depth: nothing it could change
        if (options_.verbose)
            std::fprintf(stderr, "  draw %llu: writes nothing\n", (unsigned long long)report_.draws);
        return;
    }

    // viewport and scissor
    bool depth_float24 = depth && depth->format == 1;
    Viewport viewport = host_viewport(regs_, max_viewport_[0], max_viewport_[1], ps && ps->writes_depth,
                                      depth_float24 && (depth_control & 2));
    if (viewport.empty) return;
    VkRect2D scissor = host_scissor(regs_, pitch);
    scissor.extent.width = std::min<uint32_t>(scissor.extent.width, width - std::min<uint32_t>(scissor.offset.x, width));
    scissor.extent.height = std::min<uint32_t>(scissor.extent.height, height - std::min<uint32_t>(scissor.offset.y, height));
    if (!scissor.extent.width || !scissor.extent.height) return;
    // the targets take over the EDRAM tiles down to the scissor's bottom
    for (RenderTarget *t : colors)
        if (t) acquire(t, scissor.offset.y + scissor.extent.height);
    if (depth) acquire(depth, scissor.offset.y + scissor.extent.height);
    if (!fatal_.empty()) return;

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
    if (primitive == 13) {
        // quads to triangle pairs
        std::vector<uint32_t> triangles;
        for (uint32_t q = 0; q + 3 < indices.size(); q += 4)
            for (uint32_t k : {0, 1, 2, 0, 2, 3}) triangles.push_back(indices[q + k]);
        indices = std::move(triangles);
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
    System sys{};
    for (int i = 0; i < 3; i++) sys.ndc_scale[i] = viewport.ndc_scale[i], sys.ndc_offset[i] = viewport.ndc_offset[i];
    sys.ndc_scale[3] = 1.0f;
    uint32_t color_control = regs_[r::RB_COLORCONTROL];
    if (color_control & 8) sys.flags[0] = 8 | (color_control & 7);
    sys.values[0] = as_float(regs_[r::RB_ALPHA_REF]);
    uint32_t vte = regs_[r::PA_CL_VTE_CNTL];
    sys.flags[1] = ((vte >> 10) & 1) | ((vte >> 8) & 1) << 1 | ((vte >> 9) & 1) << 2;
    if (color_control & 16) approximate("alpha to coverage");
    sys.flags[2] = gamma_targets;
    sys.flags[3] = depth_float24 ? 1u : 0u; // oDepth halved, as the target keeps D24FS8's 0-2
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
        sys.slots[slot] = t->index | t->kind << 14 | sampler(fetch, overrides) << 16;
    }

    // the pipeline
    VkFormat color_formats[4] = {};
    VkPipelineColorBlendAttachmentState blends[4] = {};
    std::vector<uint32_t> key = {uint32_t(vertex_hash_), uint32_t(vertex_hash_ >> 32),
                                 ps ? uint32_t(pixel_hash_) : 0, ps ? uint32_t(pixel_hash_ >> 32) : 0,
                                 uint32_t(topology), rectangles, restart, msaa, depth ? depth->image.format : 0u,
                                 depth_control, regs_[r::PA_SU_SC_MODE_CNTL] & 7, regs_[r::PA_CL_CLIP_CNTL] & (1u << 16)};
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
        if ((sc_mode >> 11) & 3) approximate("polygon offset");
    }
    raster.depthClampEnable = (regs_[r::PA_CL_CLIP_CNTL] >> 16) & 1;
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
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 6;
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
    VkPipeline pipe = pipeline(key, info);
    if (!pipe) return skip("vkCreateGraphicsPipelines failed");

    // the vertex data: the ranges of the vertex fetch constants the shaders
    // use (address and size, as Xenia's IssueDraw requests them), merged.
    // Copied into the arena, the constants pointed at the copies; read from
    // the mirror instead, with guest memory's bytes put there, when a
    // resolve wrote some of them and guest memory does not have them yet
    // (or when they are too many for the arena).
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
    VkDeviceSize vertex_bytes = 0;
    for (auto [start, end] : vertex_ranges) vertex_bytes += end - start;
    bool from_mirror = vertex_bytes > ARENA_SLOT / 2 || pending(vertex_ranges);
    if (from_mirror) {
        Timer timer(report_.vertex_seconds);
        upload_to_mirror(not_pending(vertex_ranges));
    }
    report_.vertex_bytes += vertex_bytes;

    // everything else the draw takes from the arena, in one submission
    VkDeviceSize index_bytes = indices.size() * 4;
    VkDeviceSize constants_size = (sizeof(harness::Constants) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    VkDeviceSize system_size = (sizeof(System) + ubo_align_ - 1) / ubo_align_ * ubo_align_;
    bool own_indices = index_bytes > ARENA_SLOT / 4;
    VkDeviceSize need = 2 * constants_size + system_size + 4 * ubo_align_ + (own_indices ? 0 : index_bytes + 4) +
                        (from_mirror ? 0 : vertex_bytes + 16 * vertex_ranges.size());
    if (arena_take(need, 1) == NONE) flush();
    else slots_[current_].used -= need; // only a check that the draw fits
    if (!fatal_.empty()) return;

    if (!from_mirror) {
        Timer timer(report_.vertex_seconds);
        std::vector<VkDeviceSize> at(vertex_ranges.size());
        for (size_t k = 0; k < vertex_ranges.size(); k++) {
            auto [start, end] = vertex_ranges[k];
            at[k] = arena_take(end - start, 16);
            std::memcpy(arena_data(at[k]), memory_.data() + start, end - start);
        }
        for (const VertexSlot &v : vertex_slots) {
            size_t k = size_t(std::upper_bound(vertex_ranges.begin(), vertex_ranges.end(), Range{v.start, UINT32_MAX}) -
                              vertex_ranges.begin()) - 1;
            uint32_t address = uint32_t(at[k] + (v.start - vertex_ranges[k].first));
            for (harness::Constants &c : constants)
                c.fetch_constants[v.slot * 2] = (c.fetch_constants[v.slot * 2] & ~0x1FFFFFFCu) | address;
        }
    }
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

    // record, in the render pass of these targets
    Pass pass;
    for (uint32_t i = 0; i < 4; i++) pass.colors[i] = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
    pass.depth = depth ? depth->view : VK_NULL_HANDLE;
    pass.width = width, pass.height = height;
    VkCommandBuffer c = begin_pass(pass, colors, depth);
    if (pipe != bound_pipeline_) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipeline_ = pipe;
    }
    VkDescriptorSet sets[2] = {draw_sets_[from_mirror ? 1 : 0], bindless_};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 3, dynamic_offsets);
    vkCmdBindIndexBuffer(c, index_buffer, index_at, VK_INDEX_TYPE_UINT32);
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
    vkCmdDrawIndexed(c, uint32_t(indices.size()), 1, 0, 0, 0);
    report_.drawn++;
    if (options_.verbose)
        std::fprintf(stderr, "  draw %llu: %s, %u indices, %s%s%s\n", (unsigned long long)report_.draws, primitive_name(primitive),
                     count, ps ? "color and depth" : "depth only", depth ? ", depth on" : "",
                     from_mirror ? ", vertices from the mirror" : "");
}

// ---- resolves ----

void Impl::resolve() {
    report_.resolves++;
    poll();
    resolve::Info info;
    std::string why;
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
        if (info.depth && info.msaa) {
            approximate("MSAA depth resolves (not done)");
        } else {
            resolve::Info clipped = info;
            clipped.x = x, clipped.y = y, clipped.width = w, clipped.height = h;
            gpu_resolve(clipped, *source);
            if (!fatal_.empty()) return;
        }
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
// write); the partly written tiles at its edges keep guest memory's bytes
// (put into the mirror first, unless the mirror holds newer ones). What it
// wrote is copied into a read-back buffer for guest memory, which gets it
// once the GPU is done (finish_writebacks); textures over it are loaded again.
void Impl::gpu_resolve(const resolve::Info &info, RenderTarget &source) {
    std::vector<Range> written, partial;
    if (!resolve::destination_ranges(info, written, partial)) {
        approximate(std::string("resolve not written: resolve to ") + format_name(info.dest_format));
        return;
    }
    if (written.empty()) return;
    uint32_t w = info.width, h = info.height;
    uint32_t texel = info.depth ? 4 : texture::bytes_per_block(source.host);
    if (!texel) {
        approximate(std::string("resolve not written: resolve from ") + color_target_format_name(info.source_format));
        return;
    }
    VkDeviceSize pixel_bytes = VkDeviceSize(w) * h * texel;
    VkDeviceSize stencil_bytes = info.depth ? (VkDeviceSize(w) * h + 3) & ~VkDeviceSize(3) : 0;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!grow(scratch_, pixel_bytes + stencil_bytes, usage, resolve_set_, 0)) return;
    upload_to_mirror(not_pending(partial));
    VkDeviceSize back_bytes = 0;
    for (auto [start, end] : written) back_bytes += end - start;
    VkBuffer back;
    VkDeviceSize back_at;
    uint8_t *back_data = readback(back_bytes, back, back_at);
    if (!back_data) return;

    VkCommandBuffer c = outside();
    VkImage from = source.image.image;
    VkOffset3D at{int32_t(info.x), int32_t(info.y), 0};
    if (!info.depth && info.msaa) {
        // a single-sample copy of the rectangle, kept for the next resolves
        // of its size and format
        auto key = std::make_tuple(source.image.format, w, h);
        vk::Image &temp = msaa_temps_[key];
        if (!temp.image) {
            temp = d_.create_image(VK_IMAGE_TYPE_2D, source.image.format, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (!temp.image) {
                fatal_ = "could not make a resolve's single-sample image";
                return;
            }
            begin_image(c, temp.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        }
        uint32_t select = (regs_[r::RB_COPY_CONTROL] >> 4) & 7;
        if (select != 6 && !(select == 4 && info.msaa == 1)) approximate("resolves of one MSAA sample, averaged");
        VkImageResolve region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = at;
        region.extent = {w, h, 1};
        vkCmdResolveImage(c, from, VK_IMAGE_LAYOUT_GENERAL, temp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        full_barrier(c);
        from = temp.image;
        at = {0, 0, 0};
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VkImageAspectFlags(info.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    copy.imageOffset = at;
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(c, from, VK_IMAGE_LAYOUT_GENERAL, scratch_.buffer, 1, &copy);
    if (info.depth) {
        copy.bufferOffset = pixel_bytes;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdCopyImageToBuffer(c, from, VK_IMAGE_LAYOUT_GENERAL, scratch_.buffer, 1, &copy);
    }
    full_barrier(c);

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
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0, 1, &resolve_set_, 0, nullptr);
    vkCmdPushConstants(c, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(c);

    // for guest memory, once the GPU is done
    std::vector<VkBufferCopy> copies;
    WriteBack wb;
    wb.serial = slots_[current_].serial;
    VkDeviceSize offset = 0;
    for (auto [start, end] : written) {
        copies.push_back({start, back_at + offset, end - start});
        wb.pieces.push_back({start, end, back_data + offset});
        offset += end - start;
    }
    vkCmdCopyBuffer(c, mirror_.buffer, back, uint32_t(copies.size()), copies.data());
    // what earlier resolves wrote there will not reach guest memory: this
    // one's bytes will
    for (WriteBack &older : writebacks_)
        for (auto [start, end] : written) cut(older.pieces, start, end);
    writebacks_.push_back(std::move(wb));
    for (auto &[key, t] : textures_) {
        if (t->high <= written.front().first || t->low >= written.back().second) continue;
        for (const Range &r : written)
            for (const Range &tr : t->ranges)
                if (overlaps(r, tr)) t->stale = true;
    }
}

// ---- the swap: texture fetch constant 0, as Xenia's RequestSwapTexture ----

// Swaps are numbered from 0; the front buffer is read back for the ones the
// options ask for. The frame's work goes to the GPU.
//
// A presenter on the renderer's device could take the front buffer without
// the read-back: texture(fetch constant 0) gives its image (in the general
// layout, its view with the guest's swizzle), loaded on the GPU from the
// mirror when a resolve wrote it; recorded before the flush, a presenter's
// copy submitted after it on the device's queue (vk::Device::submit) comes
// after it, and the renderer's next writes to the image come after that.
void Impl::swap() {
    uint64_t number = report_.swaps++;
    if (options_.frame_wanted && options_.frame_wanted(number)) read_front_buffer(number);
    flush();
    poll();
}

// The front buffer's level 0 copied into a read-back buffer on the GPU,
// handed to on_frame once the GPU is done (deliver), without waiting for it.
void Impl::read_front_buffer(uint64_t number) {
    const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS];
    texture::Info info = texture::parse_fetch_constant(fetch);
    if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture || info.dimension != 1 || !info.base_address) {
        approximate("a swap without a 2D texture in fetch constant 0");
        return;
    }
    texture::Plan plan;
    std::string why;
    if (!texture::plan(info, plan, why)) {
        approximate("swap texture not decoded: " + why);
        return;
    }
    if (plan.slices.empty() || plan.slices[0].level != 0) {
        approximate("swap texture not decoded: no base level");
        return;
    }
    Texture *t = texture(fetch, why);
    if (!t) {
        if (fatal_.empty()) approximate("swap texture not decoded: " + why);
        return;
    }
    FrontRead read;
    read.number = number;
    read.host = t->host;
    read.width = t->image.extent.width, read.height = t->image.extent.height;
    read.swizzle = info.swizzle, read.format = info.format;
    read.gamma = processor_.gamma_ramp_table();
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
    vkCmdCopyImageToBuffer(outside(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, back, 1, &copy);
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
    uint32_t components = read.host == texture::HostFormat::RGBA8 ? 4 : host_components(read.host);
    Image frame;
    frame.width = read.width, frame.height = read.height;
    frame.rgba.resize(pixels * 4);
    for (size_t i = 0; i < pixels; i++)
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t guest = (read.swizzle >> (3 * c)) & 7;
            uint8_t value = guest == 5 ? 255 : guest >= 4 ? 0 : rgba[i * 4 + std::min(guest, components - 1)];
            frame.rgba[i * 4 + c] = c == 3 ? 255 : value; // shown opaque, as the presenter does
        }
    // the display's gamma, the processor's ramps: as Xenia's presenter, the
    // piecewise linear ramp for 2_10_10_10 front buffers (not yet), the
    // 256-entry table for the rest (apply_gamma_table.xesli), the 10 bits it
    // gives back to 8
    if (read.format == 7 || read.format == 54) {
        approximate("the display's piecewise linear gamma ramp");
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
