// The Xenos renderer, first form (renderer.h), the command processor's Sink.
// The viewport and scissor follow Xenia's src/xenia/gpu/draw_util.cc
// (GetHostViewportInfo, GetScissor), the register fields registers.h, the
// clear values vulkan/vulkan_render_target_cache.cc, the vertex data a draw
// needs vulkan/vulkan_command_processor.cc (IssueDraw's shared memory
// requests), the point sizes its UpdateSystemConstantValues, and the swap
// vulkan/vulkan_texture_cache.cc
// (RequestSwapTexture); all BSD, see LICENSE-xenia. Written anew, plainly:
// one submission per draw, waited for.
//
// Guest memory is read where it lies, with nothing told when the CPU writes
// it, so what is copied from it is checked or copied again at each use:
// - the vertex data: the ranges the shaders' vertex fetch constants give,
//   copied to the GPU's copy of guest memory before each draw (Xenia
//   requests the same ranges and uploads the pages its write watches saw
//   change);
// - textures: kept with a hash of the memory they were decoded from, and
//   decoded again when it differs at a use (Xenia drops them when a write
//   watch on their pages fires); the processor's own writes
//   (memory_written) mark the textures under them at once;
// - indices: read at each draw.

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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

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

constexpr uint32_t DESCRIPTORS = 64;     // per image kind and for samplers, per draw
constexpr uint32_t EDRAM_TILES = 2048;   // 80x16 samples of 32 bits each
constexpr VkDeviceSize GUEST_MEMORY = 512u << 20;
constexpr VkDeviceSize EDRAM_BYTES = VkDeviceSize(EDRAM_TILES) * 80 * 16 * 4;
constexpr VkDeviceSize STAGING = 64u << 20;

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

// the hash of guest memory spans, each wrapping at the end of physical memory
uint64_t hash_spans(const GuestMemory &memory, std::span<const std::pair<uint32_t, uint32_t>> spans) {
    uint64_t h = 0;
    for (auto [address, bytes] : spans) {
        uint32_t at = address & GuestMemory::PHYSICAL_MASK, first = std::min(bytes, GuestMemory::SIZE - at);
        h = hash_bytes(memory.data() + at, first, h);
        if (bytes > first) h = hash_bytes(memory.data(), bytes - first, h);
    }
    return h;
}

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

// xenos::Float32To20e4, rounded towards zero (Xenia's default)
uint32_t float_to_float24(float f) {
    if (!(f > 0.0f)) return 0;
    uint32_t u = std::bit_cast<uint32_t>(f);
    if (u >= 0x3FFFFFF8) return 0xFFFFFF;
    if (u < 0x38800000) u = (0x800000 | (u & 0x7FFFFF)) >> std::min(113 - (u >> 23), 24u);
    else u += 0xC8000000u;
    return (u >> 3) & 0xFFFFFF;
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
    uint32_t width = 0, height = 0;
    // in EDRAM: tiles per row (at its bits per sample), a tile's size in pixels
    uint32_t pitch_tiles = 1, tile_width = 80, tile_height = 16;
    bool wide = false; // 64 bits per sample
};

// edram.glsl's push constants
struct EdramTarget {
    uint32_t base_tiles, pitch_tiles, msaa, format, width, height, is_depth, ints_log2, stencil_bit;
};

struct Texture {
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE; // its own: made and destroyed with it
    uint32_t dimension = 1;
    std::vector<std::pair<uint32_t, uint32_t>> footprint; // the guest memory it was decoded from
    uint64_t hash = 0;                                    // of that memory then
    uint32_t low = 0, high = 0;                           // the footprint's bounds
    bool stale = false;                                   // the processor wrote memory under it
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
    // the textures over them are decoded again at their next use
    void memory_written(uint32_t address, uint32_t bytes) override {
        Call call(*this, nullptr);
        uint64_t end = uint64_t(address) + bytes;
        for (auto &[key, t] : textures_)
            if (address < t->high && end > t->low) t->stale = true;
    }

    // EDRAM as a trace began, in Xenia's layout (edram.glsl's)
    void restore_edram(std::span<const uint8_t> bytes) override {
        Call call(*this, nullptr);
        if (bytes.size() != EDRAM_BYTES || !call) return;
        std::fill(owners_.begin(), owners_.end(), nullptr);
        std::memcpy(staging_.mapped, bytes.data(), bytes.size());
        run([&](VkCommandBuffer cmd) {
            full_barrier(cmd);
            VkBufferCopy copy{0, 0, EDRAM_BYTES};
            vkCmdCopyBuffer(cmd, staging_.buffer, edram_.buffer, 1, &copy);
        });
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
    void swap();
    bool front_buffer(Image &frame);
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
    void destroy(Texture &t);
    VkSampler sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides);
    VkPipeline pipeline(const std::vector<uint32_t> &key, const VkGraphicsPipelineCreateInfo &info);
    std::vector<std::pair<uint32_t, uint32_t>> vertex_ranges(const Shader *vs, const Shader *ps);
    VkDeviceSize stage(const std::vector<std::pair<uint32_t, uint32_t>> &ranges, std::vector<VkBufferCopy> &copies);
    bool upload_vertices(const std::vector<std::pair<uint32_t, uint32_t>> &ranges, std::vector<VkBufferCopy> &copies);
    VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                          VkComponentMapping components = {}, uint32_t levels = 1, uint32_t layers = 1,
                          bool keep = true);
    bool run(const std::function<void(VkCommandBuffer)> &record) {
        std::string error;
        Timer timer(report_.submit_seconds);
        if (d_.run(record, error)) return true;
        fatal_ = error;
        return false;
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

    std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
    std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, std::unique_ptr<RenderTarget>> targets_;
    std::map<std::array<uint32_t, 6>, std::unique_ptr<Texture>> textures_;
    std::map<std::array<uint32_t, 8>, VkSampler> samplers_;
    std::map<std::vector<uint32_t>, VkPipeline> pipelines_;

    vk::Buffer guest_, staging_, constants_[2], system_, indices_, readback_;
    VkDescriptorSetLayout set_layouts_[2] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkShaderModule rectangle_gs_ = VK_NULL_HANDLE, point_gs_ = VK_NULL_HANDLE;
    vk::Image dummy_[3]; // 2D, 3D, cube
    VkImageView dummy_views_[3] = {};
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;
    std::vector<VkImageView> views_; // all made but the textures', destroyed at the end

    // EDRAM: the render targets' contents move through it when another
    // target takes over its tiles (Xenia's ownership transfers); it starts
    // as zeros, or as a trace's snapshot
    void acquire(RenderTarget *t, uint32_t rows);
    bool edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles);
    bool edram_load(RenderTarget *t, const std::vector<uint32_t> &tiles, uint32_t rows);
    EdramTarget edram_target(const RenderTarget &t, uint32_t height) const {
        return {t.base, t.pitch_tiles, t.msaa, t.format, t.width, height, t.depth ? 1u : 0u, t.wide ? 1u : 0u, 8u};
    }
    VkPipeline load_pipeline(const RenderTarget &t);
    std::vector<RenderTarget *> owners_ = std::vector<RenderTarget *>(EDRAM_TILES, nullptr); // null: EDRAM holds it
    vk::Buffer edram_, mask_;
    VkDescriptorSetLayout edram_set_layouts_[2] = {}; // store, load
    VkPipelineLayout edram_layouts_[2] = {};
    VkDescriptorPool edram_pool_ = VK_NULL_HANDLE;
    VkPipeline store_pipelines_[2] = {}; // 1x, multisampled
    VkShaderModule fullscreen_vs_ = VK_NULL_HANDLE, load_fs_[3] = {}; // color, depth, depth and stencil
    std::map<std::pair<VkFormat, uint32_t>, VkPipeline> load_pipelines_;
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
};

Impl::~Impl() {
    vkDeviceWaitIdle(dev_);
    for (auto &[k, s] : shaders_)
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
    for (auto &[k, p] : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (auto &[k, s] : samplers_) vkDestroySampler(dev_, s, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (auto &[k, t] : targets_) d_.destroy(t->image);
    for (auto &[k, t] : textures_) destroy(*t);
    for (auto &im : dummy_) d_.destroy(im);
    if (dummy_sampler_) vkDestroySampler(dev_, dummy_sampler_, nullptr);
    for (VkShaderModule m : {rectangle_gs_, point_gs_})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto &[k, p] : load_pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (VkPipeline p : store_pipelines_)
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : {fullscreen_vs_, load_fs_[0], load_fs_[1], load_fs_[2]})
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    if (edram_pool_) vkDestroyDescriptorPool(dev_, edram_pool_, nullptr);
    for (int k = 0; k < 2; k++) {
        if (edram_layouts_[k]) vkDestroyPipelineLayout(dev_, edram_layouts_[k], nullptr);
        if (edram_set_layouts_[k]) vkDestroyDescriptorSetLayout(dev_, edram_set_layouts_[k], nullptr);
    }
    d_.destroy(edram_);
    d_.destroy(mask_);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    for (VkDescriptorSetLayout l : set_layouts_)
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    for (vk::Buffer *b : {&guest_, &staging_, &constants_[0], &constants_[1], &system_, &indices_, &readback_})
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

bool Impl::init(std::string &error) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d_.physical(), &props);
    max_viewport_[0] = float(props.limits.maxViewportDimensions[0]);
    max_viewport_[1] = float(props.limits.maxViewportDimensions[1]);
    guest_ = d_.create_buffer(GUEST_MEMORY, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    staging_ = d_.create_buffer(STAGING, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (auto &c : constants_) c = d_.create_buffer(sizeof(harness::Constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    system_ = d_.create_buffer(sizeof(System), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    if (!guest_.buffer || !staging_.buffer || !constants_[0].buffer || !constants_[1].buffer || !system_.buffer)
        return error = "could not make the renderer's buffers (512 MB of device memory for guest memory)", false;

    if (!d_.format_supported(d24s8_, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;

    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b0[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
    };
    VkDescriptorSetLayoutBinding b1[4] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DESCRIPTORS, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DESCRIPTORS, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DESCRIPTORS, stages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER, DESCRIPTORS, stages, nullptr},
    };
    VkDescriptorBindingFlags partly[4] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                                          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 4;
    flags.pBindingFlags = partly;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 4;
    set_info.pBindings = b0;
    if (vkCreateDescriptorSetLayout(dev_, &set_info, nullptr, &set_layouts_[0]) != VK_SUCCESS)
        return error = "vkCreateDescriptorSetLayout failed", false;
    set_info.pNext = &flags;
    set_info.pBindings = b1;
    if (vkCreateDescriptorSetLayout(dev_, &set_info, nullptr, &set_layouts_[1]) != VK_SUCCESS)
        return error = "vkCreateDescriptorSetLayout failed", false;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = set_layouts_;
    if (vkCreatePipelineLayout(dev_, &layout_info, nullptr, &layout_) != VK_SUCCESS)
        return error = "vkCreatePipelineLayout failed", false;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * DESCRIPTORS},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, DESCRIPTORS}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev_, &pool_info, nullptr, &pool_) != VK_SUCCESS)
        return error = "vkCreateDescriptorPool failed", false;

    VkShaderModuleCreateInfo gs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    gs.codeSize = sizeof(RECTANGLE_LIST_GS);
    gs.pCode = RECTANGLE_LIST_GS;
    if (vkCreateShaderModule(dev_, &gs, nullptr, &rectangle_gs_) != VK_SUCCESS)
        return error = "could not make the rectangle list geometry shader", false;
    gs.codeSize = sizeof(POINT_LIST_GS);
    gs.pCode = POINT_LIST_GS;
    if (vkCreateShaderModule(dev_, &gs, nullptr, &point_gs_) != VK_SUCCESS)
        return error = "could not make the point list geometry shader", false;

    // EDRAM: the buffer, the tile mask, store (compute) and load (draw) pipelines
    edram_ = d_.create_buffer(EDRAM_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    mask_ = d_.create_buffer(EDRAM_TILES * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!edram_.buffer || !mask_.buffer) return error = "could not make the EDRAM buffers", false;
    {
        VkDescriptorSetLayoutBinding store[4] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorBindingFlags store_flags[4] = {0, 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo store_binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        store_binding_flags.bindingCount = 4;
        store_binding_flags.pBindingFlags = store_flags;
        VkDescriptorSetLayoutBinding load[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
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
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        vkCreateDescriptorPool(dev_, &pool, nullptr, &edram_pool_);
        auto module = [&](const uint32_t *words, size_t bytes) {
            VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            m.codeSize = bytes;
            m.pCode = words;
            VkShaderModule out = VK_NULL_HANDLE;
            vkCreateShaderModule(dev_, &m, nullptr, &out);
            return out;
        };
        const uint32_t *stores[2] = {EDRAM_STORE, EDRAM_STORE_MS};
        const size_t store_sizes[2] = {sizeof(EDRAM_STORE), sizeof(EDRAM_STORE_MS)};
        for (int k = 0; k < 2; k++) {
            VkShaderModule m = module(stores[k], store_sizes[k]);
            VkComputePipelineCreateInfo c{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            c.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
            c.layout = edram_layouts_[0];
            VkResult r = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &c, nullptr, &store_pipelines_[k]);
            vkDestroyShaderModule(dev_, m, nullptr);
            if (r != VK_SUCCESS) return error = "could not make the EDRAM store pipelines", false;
        }
        fullscreen_vs_ = module(FULLSCREEN_VS, sizeof(FULLSCREEN_VS));
        load_fs_[0] = module(EDRAM_LOAD, sizeof(EDRAM_LOAD));
        load_fs_[1] = module(EDRAM_LOAD_DEPTH, sizeof(EDRAM_LOAD_DEPTH));
        if (d_.stencil_export()) load_fs_[2] = module(EDRAM_LOAD_STENCIL, sizeof(EDRAM_LOAD_STENCIL));
    }

    // the stand-ins for slots without a texture: 1x1 transparent black (2D
    // textures are sampled as 2D arrays)
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
    return run([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, edram_.buffer, 0, VK_WHOLE_SIZE, 0);
        for (auto &im : dummy_) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = im.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im.layers};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im.layers};
            vkCmdClearColorImage(cmd, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
    }) || (error = fatal_, false);
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
    t->view = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format,
                        depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    t->sampled = depth ? make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_DEPTH_BIT) : t->view;
    if (depth) t->stencil = make_view(t->image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format, VK_IMAGE_ASPECT_STENCIL_BIT);
    // cleared: its tiles come from EDRAM when it takes them over (acquire)
    VkImage image = t->image.image;
    if (!run([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = image;
            b.subresourceRange = {aspect, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
            if (depth) {
                VkClearDepthStencilValue zero{};
                vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            } else {
                VkClearColorValue zero{};
                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            }
        }))
        return nullptr;
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

bool Impl::edram_store(RenderTarget *t, const std::vector<uint32_t> &tiles) {
    uint32_t *mask = static_cast<uint32_t *>(mask_.mapped);
    std::fill(mask, mask + EDRAM_TILES, 0u);
    uint32_t last_row = 0;
    for (uint32_t tile : tiles) {
        mask[tile] = 1;
        last_row = std::max(last_row, ((tile + EDRAM_TILES - t->base) % EDRAM_TILES) / t->pitch_tiles);
    }
    uint32_t height = std::min((last_row + 1) * t->tile_height, t->height);
    vkResetDescriptorPool(dev_, edram_pool_, 0);
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = edram_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &edram_set_layouts_[0];
    if (vkAllocateDescriptorSets(dev_, &alloc, &set) != VK_SUCCESS) return fatal_ = "vkAllocateDescriptorSets failed", false;
    VkDescriptorBufferInfo buffers[2] = {{edram_.buffer, 0, VK_WHOLE_SIZE}, {mask_.buffer, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, t->sampled, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, t->stencil, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t b = 0; b < 4; b++) {
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if (b < 2) writes[b].pBufferInfo = &buffers[b];
        else writes[b].pImageInfo = &images[b - 2];
    }
    vkUpdateDescriptorSets(dev_, t->stencil ? 4 : 3, writes, 0, nullptr);
    EdramTarget push = edram_target(*t, height);
    VkPipeline pipeline = store_pipelines_[t->msaa ? 1 : 0];
    return run([&](VkCommandBuffer cmd) {
        full_barrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, edram_layouts_[0], 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, edram_layouts_[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (t->width + 7) / 8, (height + 7) / 8, 1);
    });
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
    uint32_t *mask = static_cast<uint32_t *>(mask_.mapped);
    std::fill(mask, mask + EDRAM_TILES, 0u);
    for (uint32_t tile : tiles) mask[tile] = 1;
    vkResetDescriptorPool(dev_, edram_pool_, 0);
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = edram_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &edram_set_layouts_[1];
    if (vkAllocateDescriptorSets(dev_, &alloc, &set) != VK_SUCCESS) return fatal_ = "vkAllocateDescriptorSets failed", false;
    VkDescriptorBufferInfo buffers[2] = {{edram_.buffer, 0, VK_WHOLE_SIZE}, {mask_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t b = 0; b < 2; b++) {
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffers[b];
    }
    vkUpdateDescriptorSets(dev_, 2, writes, 0, nullptr);
    uint32_t height = std::min(rows, t->height);
    EdramTarget push = edram_target(*t, height);
    return run([&](VkCommandBuffer cmd) {
        full_barrier(cmd);
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
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, edram_layouts_[1], 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, edram_layouts_[1], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        VkViewport viewport{0, 0, float(t->width), float(height), 0, 1};
        VkRect2D scissor{{0, 0}, {t->width, height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (!t->depth) {
            vkCmdDraw(cmd, 3, 1, 0, 0);
        } else {
            // the depth and the stencil (exported, or cleared to 0 for the
            // bit passes that follow)
            vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            if (!load_fs_[2]) {
                vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
                vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
                for (uint32_t bit = 0; bit < 8; bit++) {
                    EdramTarget pass = push;
                    pass.stencil_bit = bit;
                    vkCmdPushConstants(cmd, edram_layouts_[1], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass), &pass);
                    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                }
            }
        }
        vkCmdEndRendering(cmd);
    });
}

// ---- guest memory on the GPU: the vertex data a draw reads, copied over before it ----

// The ranges the shaders' vertex fetches can read: each vertex fetch
// constant they use, its address and size in words (a fetch reads nothing
// past them), as Xenia's IssueDraw requests them from its shared memory;
// sorted and merged.
std::vector<std::pair<uint32_t, uint32_t>> Impl::vertex_ranges(const Shader *vs, const Shader *ps) {
    std::set<uint32_t> slots;
    for (const Shader *s : {vs, ps})
        if (s) slots.insert(s->summary.vertex_slots.begin(), s->summary.vertex_slots.end());
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // [start, end)
    for (uint32_t slot : slots) {
        const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 2];
        // Xenia draws with any type here (its gpu_allow_invalid_fetch_constants
        // is on by default), warning about texture constants
        FetchConstantType type = FetchConstantType(f[0] & 3);
        if (type == FetchConstantType::Texture || type == FetchConstantType::InvalidTexture)
            approximate("a vertex fetch from a texture fetch constant");
        uint32_t start = f[0] & 0x1FFFFFFC, words = (f[1] >> 2) & 0xFFFFFF;
        uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(start) + uint64_t(words) * 4, GuestMemory::SIZE));
        if (end > start) ranges.push_back({start, end});
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (auto r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    return merged;
}

// Copies what of the ranges fits into the staging buffer and adds the
// copies into guest memory's buffer; returns the bytes staged.
VkDeviceSize Impl::stage(const std::vector<std::pair<uint32_t, uint32_t>> &ranges, std::vector<VkBufferCopy> &copies) {
    Timer timer(report_.vertex_seconds);
    VkDeviceSize at = 0;
    for (auto [start, end] : ranges) {
        VkDeviceSize n = end - start;
        std::memcpy(static_cast<uint8_t *>(staging_.mapped) + at, memory_.data() + start, n);
        copies.push_back({at, start, n});
        at += n;
    }
    report_.vertex_bytes += at;
    return at;
}

// The ranges into guest memory's buffer: copies for the draw's own
// submission when they fit the staging buffer, else in turns of it before
// the draw (no copies left).
bool Impl::upload_vertices(const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
                           std::vector<VkBufferCopy> &copies) {
    copies.clear();
    VkDeviceSize total = 0;
    for (auto [start, end] : ranges) total += end - start;
    if (total <= STAGING) {
        stage(ranges, copies);
        return true;
    }
    std::vector<std::pair<uint32_t, uint32_t>> turn;
    VkDeviceSize in_turn = 0;
    auto flush = [&] {
        std::vector<VkBufferCopy> c;
        stage(turn, c);
        turn.clear(), in_turn = 0;
        return c.empty() || run([&](VkCommandBuffer cmd) {
                   vkCmdCopyBuffer(cmd, staging_.buffer, guest_.buffer, uint32_t(c.size()), c.data());
               });
    };
    for (auto [start, end] : ranges)
        for (uint32_t at = start; at < end;) {
            uint32_t n = uint32_t(std::min<VkDeviceSize>(end - at, STAGING - in_turn));
            turn.push_back({at, at + n});
            in_turn += n, at += n;
            if (in_turn == STAGING && !flush()) return false;
        }
    return flush();
}

// ---- textures ----

VkSampler Impl::sampler(const uint32_t fetch[6], const std::array<uint32_t, 4> &overrides) {
    uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3, aniso = (fetch[3] >> 25) & 7;
    if (overrides[0] != 3) mag = overrides[0];
    if (overrides[1] != 3) min = overrides[1];
    if (overrides[2] != 3) mip = overrides[2];
    if (overrides[3] != 7) aniso = overrides[3];
    std::array<uint32_t, 8> key = {(fetch[0] >> 10) & 7, (fetch[0] >> 13) & 7, (fetch[0] >> 16) & 7, mag, min, mip, aniso, 0};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
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
    return samplers_[key] = s;
}

void Impl::destroy(Texture &t) {
    if (t.view) vkDestroyImageView(dev_, t.view, nullptr);
    d_.destroy(t.image);
    t.view = VK_NULL_HANDLE;
}

// A texture by its fetch constant, decoded again when the memory it came
// from is not what it was.
Texture *Impl::texture(const uint32_t fetch[6], std::string &why) {
    std::array<uint32_t, 6> key;
    std::copy(fetch, fetch + 6, key.begin());
    texture::Info info = texture::parse_fetch_constant(fetch);
    auto it = textures_.find(key);
    if (it != textures_.end()) {
        Texture &old = *it->second;
        bool same;
        {
            Timer timer(report_.check_seconds);
            same = !old.stale && hash_spans(memory_, old.footprint) == old.hash;
        }
        if (same) return &old;
        destroy(old);
        textures_.erase(it);
    }

    Timer decoding(report_.decode_seconds);
    texture::Image image;
    if (!texture::decode(info, memory_, image, why)) return nullptr;
    std::vector<std::vector<uint8_t>> levels = image.levels;
    VkFormat format = vk_format_of(image.format);
    if (format == VK_FORMAT_UNDEFINED || !d_.format_supported(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        // through 8 bits per component
        format = VK_FORMAT_R8G8B8A8_UNORM;
        for (uint32_t l = 0; l < levels.size(); l++) levels[l] = texture::to_rgba8(image, l);
        approximate("a texture format sampled as 8 bits per component");
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

    auto t = std::make_unique<Texture>();
    t->dimension = info.dimension;
    // 1D, 2D and stacked as 2D arrays (stacked: a layer a slice), as Xenia
    // binds them
    bool cube = info.dimension == 3, three = info.dimension == 2;
    uint32_t layers = three ? 1 : std::max(1u, image.depth);
    VkExtent3D extent{image.width, image.height, three ? image.depth : 1};
    uint32_t level_count = uint32_t(levels.size());
    t->image = d_.create_image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, format, extent, level_count, layers,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
    if (!t->image.image) {
        why = "could not make the image";
        return nullptr;
    }
    // the guest's swizzle over the host format's
    uint32_t components = format == VK_FORMAT_R8G8B8A8_UNORM && image.format != texture::HostFormat::RGBA8
                              ? 4
                              : host_components(image.format);
    VkComponentMapping mapping{swizzle_of(info.swizzle & 7, components), swizzle_of((info.swizzle >> 3) & 7, components),
                               swizzle_of((info.swizzle >> 6) & 7, components), swizzle_of((info.swizzle >> 9) & 7, components)};
    VkImageViewType view_type = cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                : three ? VK_IMAGE_VIEW_TYPE_3D
                                        : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    t->view = make_view(t->image.image, view_type, format, VK_IMAGE_ASPECT_COLOR_BIT, mapping, level_count, layers,
                        false);
    if (!t->view) {
        destroy(*t);
        why = "could not make the image's view";
        return nullptr;
    }

    // upload, level by level
    VkDeviceSize total = 0;
    for (const auto &l : levels) total += (l.size() + 15) & ~VkDeviceSize(15);
    vk::Buffer staging = d_.create_buffer(std::max<VkDeviceSize>(total, 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize at = 0;
    bool compressed = format != VK_FORMAT_R8G8B8A8_UNORM && texture::is_compressed(image.format);
    uint32_t block_size = format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : texture::bytes_per_block(image.format);
    for (uint32_t l = 0; l < level_count; l++) {
        VkExtent3D e{std::max(1u, extent.width >> l), std::max(1u, extent.height >> l), std::max(1u, extent.depth >> l)};
        uint64_t need = compressed ? uint64_t((e.width + 3) / 4) * ((e.height + 3) / 4) : uint64_t(e.width) * e.height;
        need *= uint64_t(block_size) * e.depth * layers;
        if (levels[l].size() < need) {
            // no base level (mips alone), or less than the level holds: left black
            if (!levels[l].empty()) approximate("texture levels shorter than their size");
            continue;
        }
        std::memcpy(static_cast<uint8_t *>(staging.mapped) + at, levels[l].data(), need);
        VkBufferImageCopy c{};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers};
        c.imageExtent = e;
        copies.push_back(c);
        at += (need + 15) & ~uint64_t(15);
    }
    VkImage vk_image = t->image.image;
    bool ok = run([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = vk_image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, level_count, 0, layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        if (!copies.empty())
            vkCmdCopyBufferToImage(cmd, staging.buffer, vk_image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copies.size()), copies.data());
    });
    d_.destroy(staging);
    if (!ok) {
        destroy(*t);
        return nullptr;
    }

    // what it was decoded from, for seeing when that changes
    t->footprint = std::move(image.footprint);
    t->hash = hash_spans(memory_, t->footprint);
    t->low = UINT32_MAX;
    for (auto [address, bytes] : t->footprint) {
        t->low = std::min(t->low, address);
        uint64_t end = std::min<uint64_t>(uint64_t(address) + bytes, GuestMemory::SIZE);
        t->high = uint32_t(std::max<uint64_t>(t->high, end));
    }
    report_.textures++;
    return (textures_[key] = std::move(t)).get();
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
    VkDeviceSize index_bytes = indices.size() * 4;
    if (indices_.size < index_bytes) {
        d_.destroy(indices_);
        indices_ = d_.create_buffer(std::max<VkDeviceSize>(index_bytes, 1 << 20), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    }
    std::memcpy(indices_.mapped, indices.data(), index_bytes);

    // constants
    for (int stage = 0; stage < 2; stage++) {
        auto *c = static_cast<harness::Constants *>(constants_[stage].mapped);
        std::memcpy(c->float_constants, &regs_[reg::ALU_CONSTANTS + stage * 1024], 4096);
        std::memcpy(c->bool_constants, &regs_[reg::BOOL_CONSTANTS], sizeof(c->bool_constants));
        std::memcpy(c->loop_constants, &regs_[reg::LOOP_CONSTANTS], sizeof(c->loop_constants));
        std::memcpy(c->fetch_constants, &regs_[reg::FETCH_CONSTANTS], sizeof(c->fetch_constants));
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

    // textures: per slot the shaders read, an image in the array of its
    // dimension and a sampler
    std::vector<VkDescriptorImageInfo> images[3], samplers;
    std::set<uint32_t> slots(vs->summary.texture_slots.begin(), vs->summary.texture_slots.end());
    if (ps) slots.insert(ps->summary.texture_slots.begin(), ps->summary.texture_slots.end());
    for (int k = 0; k < 3; k++) images[k].push_back({VK_NULL_HANDLE, dummy_views_[k], VK_IMAGE_LAYOUT_GENERAL});
    samplers.push_back({dummy_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    for (uint32_t slot = 0; slot < 32; slot++) sys.slots[slot] = 0; // the stand-ins
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
        uint32_t kind = t->dimension == 2 ? 1 : t->dimension == 3 ? 2 : 0;
        std::array<uint32_t, 4> overrides = {3, 3, 3, 7};
        for (Shader *s : {vs, ps})
            if (s && s->filters.count(slot)) {
                overrides = s->filters[slot];
                break;
            }
        if (images[kind].size() >= DESCRIPTORS || samplers.size() >= DESCRIPTORS) continue;
        uint32_t image_index = uint32_t(images[kind].size()), sampler_index = uint32_t(samplers.size());
        images[kind].push_back({VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL});
        samplers.push_back({sampler(fetch, overrides), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        sys.slots[slot] = image_index | kind << 14 | sampler_index << 16;
    }
    std::memcpy(system_.mapped, &sys, sizeof(sys));

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

    // the vertex data, from guest memory as it is now (staged last: nothing
    // else uses the staging buffer before the draw's submission)
    std::vector<VkBufferCopy> vertex_copies;
    if (!upload_vertices(vertex_ranges(vs, ps), vertex_copies)) return;

    // descriptors
    vkResetDescriptorPool(dev_, pool_, 0);
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = set_layouts_;
    if (vkAllocateDescriptorSets(dev_, &alloc, sets) != VK_SUCCESS) {
        fatal_ = "vkAllocateDescriptorSets failed";
        return;
    }
    VkDescriptorBufferInfo buffers[4] = {{constants_[0].buffer, 0, VK_WHOLE_SIZE},
                                         {guest_.buffer, 0, VK_WHOLE_SIZE},
                                         {system_.buffer, 0, VK_WHOLE_SIZE},
                                         {constants_[1].buffer, 0, VK_WHOLE_SIZE}};
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t b = 0; b < 4; b++) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[0];
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &buffers[b];
        writes.push_back(w);
    }
    for (uint32_t b = 0; b < 4; b++) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[1];
        w.dstBinding = b;
        w.descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        const auto &list = b == 3 ? samplers : images[b];
        w.descriptorCount = uint32_t(list.size());
        w.pImageInfo = list.data();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(dev_, uint32_t(writes.size()), writes.data(), 0, nullptr);

    // record, submit, wait
    uint32_t stencil = regs_[r::RB_STENCILREFMASK], stencil_bf = regs_[r::RB_STENCILREFMASK_BF];
    if (!((depth_control >> 7) & 1)) stencil_bf = stencil;
    const float *blend_constants = reinterpret_cast<const float *>(&regs_[r::RB_BLEND_RED]);
    uint32_t index_count = uint32_t(indices.size());
    run([&](VkCommandBuffer cmd) {
        if (!vertex_copies.empty())
            vkCmdCopyBuffer(cmd, staging_.buffer, guest_.buffer, uint32_t(vertex_copies.size()), vertex_copies.data());
        full_barrier(cmd);
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
        rendering_info.renderArea = {{0, 0}, {width, height}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 4;
        rendering_info.pColorAttachments = attachments;
        if (depth) rendering_info.pDepthAttachment = rendering_info.pStencilAttachment = &depth_attachment;
        vkCmdBeginRendering(cmd, &rendering_info);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
        vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdSetViewport(cmd, 0, 1, &viewport.viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetBlendConstants(cmd, blend_constants);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil & 0xFF);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencil_bf & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (stencil >> 8) & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (stencil_bf >> 8) & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (stencil >> 16) & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (stencil_bf >> 16) & 0xFF);
        vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
    });
    report_.drawn++;
    if (options_.verbose)
        std::fprintf(stderr, "  draw %llu: %s, %u indices, %s%s\n", (unsigned long long)report_.draws, primitive_name(primitive),
                     count, ps ? "color and depth" : "depth only", depth ? ", depth on" : "");
}

// ---- resolves ----

void Impl::resolve() {
    report_.resolves++;
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
            uint32_t texel = info.depth ? 4 : texture::bytes_per_block(source->host);
            VkDeviceSize bytes = VkDeviceSize(w) * h * texel;
            // the read-back buffer, kept and grown: cached memory, read by the CPU
            if (readback_.size < bytes + VkDeviceSize(w) * h) {
                d_.destroy(readback_);
                readback_ = d_.create_buffer(std::max<VkDeviceSize>(bytes + VkDeviceSize(w) * h, 16u << 20),
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
                if (!readback_.buffer) {
                    fatal_ = "could not make the resolves' read-back buffer";
                    return;
                }
            }
            const vk::Buffer &readback = readback_;
            vk::Image resolved;
            if (!info.depth && info.msaa) {
                resolved = d_.create_image(VK_IMAGE_TYPE_2D, source->image.format, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                uint32_t select = (regs_[r::RB_COPY_CONTROL] >> 4) & 7;
                if (select != 6 && !(select == 4 && info.msaa == 1)) approximate("resolves of one MSAA sample, averaged");
            }
            bool ok = run([&](VkCommandBuffer cmd) {
                full_barrier(cmd);
                VkImage from = source->image.image;
                VkOffset3D at{int32_t(x), int32_t(y), 0};
                if (resolved.image) {
                    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    b.image = resolved.image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dep.imageMemoryBarrierCount = 1;
                    dep.pImageMemoryBarriers = &b;
                    vkCmdPipelineBarrier2(cmd, &dep);
                    VkImageResolve region{};
                    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.srcOffset = at;
                    region.extent = {w, h, 1};
                    vkCmdResolveImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL, resolved.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                    full_barrier(cmd);
                    from = resolved.image;
                    at = {0, 0, 0};
                }
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VkImageAspectFlags(info.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
                copy.imageOffset = at;
                copy.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(cmd, from, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
                if (info.depth) {
                    copy.bufferOffset = bytes;
                    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                    vkCmdCopyImageToBuffer(cmd, from, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
                }
            });
            if (ok) {
                const uint8_t *data = static_cast<const uint8_t *>(readback.mapped);
                std::vector<uint8_t> pixels(data, data + bytes);
                if (info.depth) {
                    // as the guest's words: depth in the high 24 bits, D24S8's
                    // unorm or D24FS8's 20e4 (kept halved), as the EDRAM
                    // store writes them
                    bool float_depth = source->image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
                    for (uint32_t i = 0; i < w * h; i++) {
                        uint32_t d;
                        std::memcpy(&d, data + 4 * i, 4);
                        float depth = as_float(d);
                        uint32_t d24 = !float_depth          ? d & 0xFFFFFF
                                       : source->format == 1 ? float_to_float24(depth * 2.0f)
                                                             : uint32_t(std::nearbyint(std::clamp(depth, 0.0f, 1.0f) *
                                                                                       16777215.0f));
                        uint32_t word = d24 << 8 | data[bytes + i];
                        std::memcpy(&pixels[4 * i], &word, 4);
                    }
                }
                resolve::Info clipped = info;
                clipped.x = x, clipped.y = y, clipped.width = w, clipped.height = h;
                if (!resolve::write(clipped, pixels, memory_, why)) approximate("resolve not written: " + why);
            }
            if (resolved.image) d_.destroy(resolved);
            if (!ok) return;
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
                auto &c = clear.clearValue.color;
                using H = texture::HostFormat;
                if (t->format == 3 || t->format == 12) {
                    // 2_10_10_10_FLOAT: 7e3 floats kept as 16-bit floats
                    for (int j = 0; j < 3; j++) c.float32[j] = float7e3_to_float(uint32_t(value >> (10 * j)));
                    c.float32[3] = float((value >> 30) & 3) / 3.0f;
                } else switch (t->host) {
                case H::RGBA8:
                    for (int j = 0; j < 4; j++) c.float32[j] = float((value >> (8 * j)) & 0xFF) / 255.0f;
                    break;
                case H::RGB10A2:
                    for (int j = 0; j < 3; j++) c.float32[j] = float((value >> (10 * j)) & 0x3FF) / 1023.0f;
                    c.float32[3] = float((value >> 30) & 3) / 3.0f;
                    break;
                case H::RG16F: case H::RGBA16F:
                    for (int j = 0; j < 4; j++) c.float32[j] = half_to_float(uint16_t(value >> (16 * j)));
                    break;
                case H::R32F: case H::RG32F:
                    c.float32[0] = as_float(uint32_t(value >> (t->host == H::RG32F ? 32 : 0)));
                    c.float32[1] = as_float(uint32_t(value));
                    break;
                case H::RG16: case H::RGBA16: // snorm: the guest's bits
                    for (int j = 0; j < 4; j++)
                        c.float32[j] = std::max(-1.0f, float(int16_t(value >> (16 * j))) / 32767.0f);
                    break;
                default:
                    approximate("clears of this target format");
                    break;
                }
            }
            VkClearRect rect{{{int32_t(x), int32_t(y)}, {std::min(w, t->width - std::min(x, t->width)), std::min(h, t->height - std::min(y, t->height))}}, 0, 1};
            if (!rect.rect.extent.width || !rect.rect.extent.height) continue;
            run([&](VkCommandBuffer cmd) {
                full_barrier(cmd);
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
                vkCmdBeginRendering(cmd, &rendering);
                vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
                vkCmdEndRendering(cmd);
            });
        }
    }
}

// ---- the swap: texture fetch constant 0, as Xenia's RequestSwapTexture ----

// Swaps are numbered from 0; the front buffer is read back for the ones the
// options ask for.
void Impl::swap() {
    uint64_t number = report_.swaps++;
    if (!options_.frame_wanted || !options_.frame_wanted(number)) return;
    Image frame;
    if (!front_buffer(frame)) return;
    report_.frames++;
    if (options_.on_frame) options_.on_frame(number, frame);
}

bool Impl::front_buffer(Image &frame) {
    const uint32_t *fetch = &regs_[reg::FETCH_CONSTANTS];
    texture::Info info = texture::parse_fetch_constant(fetch);
    if (FetchConstantType(fetch[0] & 3) != FetchConstantType::Texture || info.dimension != 1 || !info.base_address) {
        approximate("a swap without a 2D texture in fetch constant 0");
        return false;
    }
    texture::Image image;
    std::string why;
    if (!texture::decode(info, memory_, image, why)) {
        approximate("swap texture not decoded: " + why);
        return false;
    }
    std::vector<uint8_t> rgba = texture::to_rgba8(image, 0);
    if (rgba.size() < size_t(image.width) * image.height * 4) {
        approximate("swap texture not decoded: no base level");
        return false;
    }
    uint32_t components = image.format == texture::HostFormat::RGBA8 ? 4 : host_components(image.format);
    frame.width = image.width, frame.height = image.height;
    frame.rgba.resize(size_t(image.width) * image.height * 4);
    for (size_t i = 0; i < size_t(image.width) * image.height; i++)
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t guest = (info.swizzle >> (3 * c)) & 7;
            uint8_t value = guest == 5 ? 255 : guest >= 4 ? 0 : rgba[i * 4 + std::min(guest, components - 1)];
            frame.rgba[i * 4 + c] = c == 3 ? 255 : value; // shown opaque, as the presenter does
        }
    // the display's gamma, the processor's ramps: as Xenia's presenter, the
    // piecewise linear ramp for 2_10_10_10 front buffers (not yet), the
    // 256-entry table for the rest (apply_gamma_table.xesli), the 10 bits it
    // gives back to 8
    if (info.format == 7 || info.format == 54) {
        approximate("the display's piecewise linear gamma ramp");
        return true;
    }
    const std::array<uint32_t, 256> &table = processor_.gamma_ramp_table();
    for (size_t i = 0; i < size_t(image.width) * image.height; i++)
        for (uint32_t c = 0; c < 3; c++) {
            uint32_t entry = table[frame.rgba[i * 4 + c]];
            uint32_t ten = (entry >> (c == 0 ? 20 : c == 1 ? 10 : 0)) & 0x3FF;
            frame.rgba[i * 4 + c] = uint8_t((ten * 255 + 511) / 1023);
        }
    return true;
}

} // namespace

std::unique_ptr<Renderer> Renderer::create(vk::Device &device, gpu::CommandProcessor &processor,
                                           const Options &options, std::string &error) {
    auto renderer = std::make_unique<Impl>(device, processor, options);
    if (!renderer->init(error)) return nullptr;
    return renderer;
}

} // namespace xf::render
