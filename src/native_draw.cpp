// A port's renderer drawing a game's draws from their register state
// (xenoforge/native_draw.h): the Drawer, built on the port API alone
// (native_render.h's Frame, native_shaders.h's ShaderCache, native_draw.h's
// draw_setup), so it is what a port would write, once.
//
// Per frame: begin() resets a descriptor pool; draws and clears are prepared
// as they come (their targets made, textures loaded, constants and indices
// uploaded: Frame's helpers may record and begin a new command buffer, so
// nothing of the frame's own is recorded yet) and recorded together at the
// next resolve, present or end(), one dynamic rendering for each run of ops
// with the same attachments.
//
// What a draw costs is kept small (docs/sdk.md, "The Drawer's per-draw
// cost"): set 0 written once a frame with dynamic offsets (the stages' and
// the system constants' uniform buffers in the frame's arena, guest memory or
// the arena whole as binding 1, copied vertices addressed by their place in
// the arena), each block of constants uploaded only when it differs from the
// stage's last, set 1 (the textures) reused while the draws' textures stay
// the same, pipelines by a 64-bit key through a VkPipelineCache kept beside
// the shaders, bindings and dynamic state set only when they change.

#include "xenoforge/native_draw.h"

#include "xenoforge/vk_device.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace xf::native {

namespace {

const uint32_t FULLSCREEN_UV_VERT[] = {
#include "native_fullscreen.vert.inc"
};
const uint32_t PRESENT_FRAG[] = {
#include "native_present.frag.inc"
};
const uint32_t PACK7E3_FRAG[] = {
#include "native_pack7e3.frag.inc"
};
const uint32_t DOWNSAMPLE_FRAG[] = {
#include "native_downsample.frag.inc"
};

constexpr uint32_t SLOTS = 32;
// each image table (set 1's bindings 0-2: 2D arrays, 3D, cubes): a stand-in at 0 (the translated shaders read
// each table's 0 for the kinds a slot is not; unbound, the device is lost), slot k's texture at 1 + k, its
// signed view at 1 + 32 + k; the samplers (binding 3): the stand-in's at 0, slot k's at 1 + k
constexpr uint32_t IMAGES = 1 + 2 * SLOTS, SAMPLERS = 1 + SLOTS;
constexpr int POOLS = 8;          // reset round robin: one a frame (the frames the GPU may still be on)
constexpr uint32_t SETS = 4096;   // a pool's sets (a frame's draws with new textures)

constexpr VkImageAspectFlags DEPTH_STENCIL = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull + (v >> 29); }

struct Target {
    bool depth = false;
    uint32_t base = 0, pitch = 0, format = 0;
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE;   // the attachment's (depth: both aspects)
    VkImageView sampled = VK_NULL_HANDLE; // color: 2D
    VkImageView array = VK_NULL_HANDLE;   // color: a 2D array (present_target)
    uint32_t width = 0, height = 0;
    uint64_t written = 0;
};

struct Attachments {
    Target *color[4] = {};
    Target *depth = nullptr;
    bool operator==(const Attachments &o) const {
        return std::equal(color, color + 4, o.color) && depth == o.depth;
    }
};

// a draw or a clear, ready to record
struct Op {
    bool clear = false;
    Attachments attachments;
    // a draw's
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {};
    uint32_t offsets[3] = {}; // set 0's dynamic offsets: bindings 0, 2, 3
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend_constants[4] = {};
    uint32_t stencil_reference[2] = {}, stencil_compare_mask[2] = {}, stencil_write_mask[2] = {};
    float line_width = 1.0f, bias_constant = 0.0f, bias_slope = 0.0f;
    uint8_t bias_representation = 0;
    bool depth_target_float = false;
    VkBuffer indices = VK_NULL_HANDLE, attributes = VK_NULL_HANDLE;
    VkDeviceSize indices_at = 0, attributes_at = 0;
    uint32_t count = 0, first = 0;
    // a clear's
    VkClearAttachment values[5] = {};
    uint32_t value_count = 0;
    VkClearRect rect{};
};

// the last values set in the command buffer (a draw sets only what changed)
struct Bound {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {};
    uint32_t offsets[3] = {};
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {}, line = -1, bias[2] = {NAN, NAN};
    uint8_t bias_representation = 255;
    uint32_t stencil[3][2] = {{~0u, ~0u}, {~0u, ~0u}, {~0u, ~0u}};
    VkBuffer indices = VK_NULL_HANDLE, attributes = VK_NULL_HANDLE;
    VkDeviceSize indices_at = ~0ull, attributes_at = ~0ull;
};

} // namespace

struct Drawer::Impl {
    std::filesystem::path shader_dir;
    std::unique_ptr<ShaderCache> shaders;
    bool made = false, failed = false;
    std::string failed_why;
    // the run's
    vk::Device *device = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    VkDescriptorSetLayout layouts[2] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout helper_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout helper_layout = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    vk::Image standin_images[3];
    VkImageView standins[3] = {};
    VkDescriptorPool pools[POOLS] = {};
    VkPipelineCache cache = VK_NULL_HANDLE;
    uint64_t pipelines_unsaved = 0;
    std::unordered_map<uint64_t, VkShaderModule> modules;  // by hash, stage and variant
    std::unordered_map<uint64_t, std::string> module_why;
    std::unordered_map<uint64_t, VkPipeline> pipelines;    // by the draw's key
    std::map<std::tuple<const uint32_t *, int>, VkPipeline> helpers;
    std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t>, Target> targets; // (depth, base, pitch, format)
    std::map<std::tuple<int, uint32_t, uint32_t>, Target> scratch;              // (format, width, height)
    std::vector<std::pair<uint64_t, Target>> retired; // targets made again taller: destroyed POOLS frames later
    uint64_t written = 0;
    // the frame's
    const OutputContext *ctx = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<Op> ops;
    std::map<std::pair<VkBuffer, VkBuffer>, VkDescriptorSet> set0; // by binding 1's buffer and the arena's
    struct Uploaded {
        bool valid = false;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize at = 0;
    };
    ShaderConstants last[2]{}; // the stages' last blocks (the floats their shaders read, the rest)
    uint64_t last_used[2][4] = {};
    Uploaded last_at[2];
    SystemConstants last_system{};
    Uploaded last_system_at;
    // the last textures' set: its key (per slot: the view, signed view, sampler and kind) and the set
    std::vector<uint64_t> textures_key;
    VkDescriptorSet textures_set = VK_NULL_HANDLE;
    std::vector<uint32_t> index_words, attribute_words, fetch_words;
    Stats stats;
    mutable std::mutex stats_mutex;
    // XF_DRAWER_LOG=<swap>: that frame's draws listed, numbered from 0 in the order the port gave them;
    // XF_DRAWER_SKIP=<swap>:<first>-<last>: those draws of that frame left out (finding the one a wrong
    // picture comes from, beside the shadow mode's emulated picture of the same frame)
    struct Debug {
        uint64_t log_swap = UINT64_MAX, skip_swap = UINT64_MAX;
        uint32_t skip_first = 0, skip_last = 0;
        Debug() {
            if (const char *v = std::getenv("XF_DRAWER_LOG"); v && *v) log_swap = std::strtoull(v, nullptr, 10);
            if (const char *v = std::getenv("XF_DRAWER_SKIP"); v && *v) {
                unsigned long long swap = 0;
                unsigned first = 0, last = 0;
                const int n = std::sscanf(v, "%llu:%u-%u", &swap, &first, &last);
                if (n >= 2) skip_swap = swap, skip_first = first, skip_last = n == 3 ? last : first;
            }
        }
    } debug;
    uint32_t frame_draws = 0;

    void log_draw(uint32_t index, const DrawCall &call, const Op &op) {
        const uint32_t *r = call.registers;
        std::printf("drawer: swap %llu draw %u: primitive %u count %u, shaders %016llX %016llX%s",
                    (unsigned long long)ctx->swap, index, call.primitive, call.count,
                    (unsigned long long)call.vs_hash, (unsigned long long)call.ps_hash,
                    op.pipeline ? "" : ", nothing drawn");
        for (int i = 0; i < 4; i++)
            if (const Target *t = op.attachments.color[i])
                std::printf(", color %d at %X format %u blend %08X mask %X", i, t->base, t->format,
                            r[i == 0 ? 0x2201 : 0x2208 + i], (r[0x2104] >> (4 * i)) & 0xF);
        if (const Target *t = op.attachments.depth)
            std::printf(", depth at %X format %u control %08X stencil %08X/%08X", t->base, t->format, r[0x2200],
                        r[0x210D], r[0x210C]);
        std::printf(", color control %08X, mode %08X, scissor %d,%d %ux%u, bias %g %g\n", r[0x2202], r[0x2205],
                    op.scissor.offset.x, op.scissor.offset.y, op.scissor.extent.width, op.scissor.extent.height,
                    op.bias_constant, op.bias_slope);
    }

    void approximate(const char *what) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        stats.approximate[what]++;
    }

    VkShaderModule make_module(const uint32_t *words, size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes, info.pCode = words;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &info, nullptr, &m);
        return m;
    }

    VkSampler make_sampler(VkFilter filter) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler s = VK_NULL_HANDLE;
        vkCreateSampler(dev, &info, nullptr, &s);
        return s;
    }

    VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect,
                          uint32_t layers = 1) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image, view.viewType = type, view.format = format;
        view.subresourceRange = {aspect, 0, 1, 0, layers};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev, &view, nullptr, &v);
        return v;
    }

    // an image in the general layout, cleared, its layout change and clear recorded now
    void initialize(VkImage image, bool depth, uint32_t layers = 1) {
        VkCommandBuffer cmd = ctx->frame->commands();
        const VkImageSubresourceRange range{depth ? DEPTH_STENCIL : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0,
                                            1, 0, layers};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image, b.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        if (depth) {
            VkClearDepthStencilValue zero{};
            vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        } else {
            VkClearColorValue zero{};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0,
                             nullptr, 0, nullptr);
    }

    bool make_objects(std::string &why) {
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // set 0 as native_shaders.h gives it, the uniform buffers with dynamic offsets
        const VkDescriptorSetLayoutBinding b0[4] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr},
        };
        const VkDescriptorSetLayoutBinding b1[4] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMAGES, stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMAGES, stages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMAGES, stages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS, stages, nullptr},
        };
        const VkDescriptorBindingFlags partly[4] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flags.bindingCount = 4, flags.pBindingFlags = partly;
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 4, set.pBindings = b0;
        if (vkCreateDescriptorSetLayout(dev, &set, nullptr, &layouts[0]) != VK_SUCCESS)
            return why = "the descriptor set layouts were not made", false;
        set.pNext = &flags, set.pBindings = b1;
        if (vkCreateDescriptorSetLayout(dev, &set, nullptr, &layouts[1]) != VK_SUCCESS)
            return why = "the descriptor set layouts were not made", false;
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2, pl.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(dev, &pl, nullptr, &layout) != VK_SUCCESS)
            return why = "the pipeline layout was not made", false;
        // the helper passes: one combined image sampler, 16 bytes of push constants
        const VkDescriptorSetLayoutBinding hb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo hset{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        hset.bindingCount = 1, hset.pBindings = &hb;
        if (vkCreateDescriptorSetLayout(dev, &hset, nullptr, &helper_set_layout) != VK_SUCCESS)
            return why = "the helper layout was not made", false;
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo hl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        hl.setLayoutCount = 1, hl.pSetLayouts = &helper_set_layout;
        hl.pushConstantRangeCount = 1, hl.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(dev, &hl, nullptr, &helper_layout) != VK_SUCCESS)
            return why = "the helper layout was not made", false;
        nearest = make_sampler(VK_FILTER_NEAREST), linear = make_sampler(VK_FILTER_LINEAR);
        if (!nearest || !linear) return why = "the samplers were not made", false;
        // the stand-ins: 1x1 images of zeros as a 2D array, a 3D image and a cube
        for (int k = 0; k < 3; k++) {
            standin_images[k] = device->create_image(
                k == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, k == 2 ? 6 : 1,
                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                k == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
            if (!standin_images[k].image) return why = "the stand-in images were not made", false;
            standins[k] = make_view(standin_images[k].image,
                                    k == 0   ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                    : k == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                             : VK_IMAGE_VIEW_TYPE_CUBE,
                                    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, k == 2 ? 6 : 1);
            if (!standins[k]) return why = "the stand-in images were not made", false;
            initialize(standin_images[k].image, false, k == 2 ? 6 : 1);
        }
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 64},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (2 * SLOTS + 3) * SETS},
            {VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS * SETS},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256}};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = SETS + 64 + 256, pool_info.poolSizeCount = uint32_t(std::size(sizes));
        pool_info.pPoolSizes = sizes;
        for (VkDescriptorPool &p : pools)
            if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &p) != VK_SUCCESS)
                return why = "the descriptor pools were not made", false;
        // the pipelines kept beside the shaders (the driver checks the data's device)
        std::vector<char> data;
        if (!shader_dir.empty())
            if (std::ifstream f(shader_dir / "pipelines.bin", std::ios::binary); f)
                data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size(), ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(dev, &ci, nullptr, &cache) != VK_SUCCESS) {
            ci.initialDataSize = 0, ci.pInitialData = nullptr;
            vkCreatePipelineCache(dev, &ci, nullptr, &cache);
        }
        return true;
    }

    void save_pipelines() {
        if (!cache || shader_dir.empty() || !pipelines_unsaved) return;
        size_t size = 0;
        if (vkGetPipelineCacheData(dev, cache, &size, nullptr) != VK_SUCCESS || !size) return;
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(dev, cache, &size, data.data()) != VK_SUCCESS) return;
        std::error_code ec;
        std::filesystem::create_directories(shader_dir, ec);
        const std::filesystem::path file = shader_dir / "pipelines.bin", temp = file.string() + ".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f.write(data.data(), std::streamsize(size));
            if (!f) return;
        }
        std::filesystem::rename(temp, file, ec);
        if (ec) std::filesystem::remove(temp, ec);
        pipelines_unsaved = 0;
    }

    // ---- targets ----

    // The port's target for an EDRAM place and format: made the first time at the pitch, as tall as asked
    // (a multiple of 16, at most 2048), again when taller is asked (the old contents lost, counted).
    Target *target(bool depth, uint32_t base, uint32_t pitch, uint32_t format, uint32_t height_wanted,
                   std::string &why) {
        if (!depth && format == 10) format = 2; // (2_10_10_10_AS_10_10_10_10, 2_10_10_10_FLOAT_AS_16_16_16_16)
        if (!depth && format == 12) format = 3;
        if (depth) format &= 1;
        height_wanted = std::clamp(height_wanted, 16u, 2048u);
        const auto key = std::make_tuple(depth, base, pitch, format);
        auto it = targets.find(key);
        if (it != targets.end() && it->second.height >= height_wanted) return &it->second;
        if (!pitch) return why = "a target of pitch 0", nullptr;
        const VkFormat vk_format = ctx->frame->target_format(depth, format);
        if (vk_format == VK_FORMAT_UNDEFINED) return why = "a target format the renderer keeps none of", nullptr;
        if (it != targets.end()) {
            approximate("a target made taller (its contents lost)");
            flush(); // (the frame's draws into it recorded first)
            retired.emplace_back(stats.frames, it->second);
            targets.erase(it);
        }
        Target t;
        t.depth = depth, t.base = base, t.pitch = pitch, t.format = format;
        t.width = pitch, t.height = (height_wanted + 15) & ~15u;
        t.image = device->create_image(
            VK_IMAGE_TYPE_2D, vk_format, {t.width, t.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
        if (!t.image.image) return why = "a target image not made", nullptr;
        t.view = make_view(t.image.image, VK_IMAGE_VIEW_TYPE_2D, vk_format,
                           depth ? DEPTH_STENCIL : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT));
        if (!depth) {
            t.sampled = t.view;
            t.array = make_view(t.image.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vk_format, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (!t.view || (!depth && !t.array)) return destroy(t), why = "a target view not made", nullptr;
        initialize(t.image.image, depth);
        return &(targets[key] = t);
    }

    void destroy(Target &t) {
        // (an image the GPU may still use: destroyed after the device is idle, at the Drawer's end, or
        // now when made again taller, which a draw of this frame cannot have used yet)
        if (t.array) vkDestroyImageView(dev, t.array, nullptr);
        if (t.view) vkDestroyImageView(dev, t.view, nullptr);
        if (t.image.image) device->destroy(t.image);
        t = {};
    }

    // a scratch image of this format and size (sampled, a color attachment, general layout)
    Target *scratch_image(VkFormat format, uint32_t width, uint32_t height) {
        Target &t = scratch[{int(format), width, height}];
        if (t.image.image) return &t;
        t.width = width, t.height = height;
        t.image = device->create_image(VK_IMAGE_TYPE_2D, format, {width, height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!t.image.image) return nullptr;
        t.view = t.sampled = make_view(t.image.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!t.view) return nullptr;
        initialize(t.image.image, false);
        return &t;
    }

    // ---- shaders and pipelines ----

    VkShaderModule module_of(const uint32_t *words, size_t count, uint64_t hash, bool vertex,
                             const ShaderVariant &variant, std::string &why) {
        const uint64_t key = mix(mix(hash, variant.key(vertex)), vertex ? 1 : 2);
        if (auto it = modules.find(key); it != modules.end()) {
            if (!it->second) why = module_why[key];
            return it->second;
        }
        const TranslatedShader &t =
            shaders->get(words, count, vertex ? ShaderStage::Vertex : ShaderStage::Pixel, variant);
        VkShaderModule m = t.ok() ? make_module(t.spirv.data(), t.spirv.size() * 4) : VK_NULL_HANDLE;
        if (!m) why = module_why[key] = "a shader the translator does not do: " + (t.ok() ? "no module" : t.why);
        for (const std::string &a : t.approximate) {
            std::lock_guard<std::mutex> lock(stats_mutex);
            stats.approximate[a]++;
        }
        return modules[key] = m;
    }

    VkPipeline pipeline_of(const DrawSetup &s, VkShaderModule vs, VkShaderModule ps, const Attachments &a,
                           uint64_t key, std::string &why) {
        if (auto it = pipelines.find(key); it != pipelines.end()) {
            if (!it->second) why = "no pipeline for the draw (vkCreateGraphicsPipelines failed)";
            return it->second;
        }
        std::lock_guard<std::mutex> lock(stats_mutex);
        stats.pipeline_misses++;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = ps, stages[1].pName = "main";
        // a rectangle's three guest indices, the same for its four vertices
        const VkVertexInputBindingDescription binding{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_UINT, 0};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (s.indices == IndexForm::Rectangles) {
            input.vertexBindingDescriptionCount = input.vertexAttributeDescriptionCount = 1;
            input.pVertexBindingDescriptions = &binding, input.pVertexAttributeDescriptions = &attribute;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = s.topology, assembly.primitiveRestartEnable = s.restart;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = s.polygon_mode, raster.cullMode = s.cull, raster.frontFace = s.front_face;
        raster.depthClampEnable = s.depth_clamp, raster.depthBiasEnable = a.depth != nullptr;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (a.depth) {
            depth.depthTestEnable = s.depth_test, depth.depthWriteEnable = s.depth_write;
            depth.depthCompareOp = s.depth_compare, depth.stencilTestEnable = s.stencil_test;
            depth.front = s.stencil_front, depth.back = s.stencil_back;
        }
        VkPipelineColorBlendAttachmentState blends[4];
        VkFormat formats[4];
        for (int i = 0; i < 4; i++) {
            blends[i] = a.color[i] ? s.blend[i] : VkPipelineColorBlendAttachmentState{};
            formats[i] = a.color[i] ? a.color[i]->image.format : VK_FORMAT_UNDEFINED;
        }
        VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colors.attachmentCount = 4, colors.pAttachments = blends;
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR,
                                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                 VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                                 VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                 VK_DYNAMIC_STATE_DEPTH_BIAS,
                                                 VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                                 VK_DYNAMIC_STATE_LINE_WIDTH};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states)), dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 4, rendering.pColorAttachmentFormats = formats;
        rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat =
            a.depth ? a.depth->image.format : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = ps ? 2 : 1, info.pStages = stages;
        info.pVertexInputState = &input, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
        info.pRasterizationState = &raster, info.pMultisampleState = &samples, info.pColorBlendState = &colors;
        info.pDepthStencilState = &depth, info.pDynamicState = &dynamic, info.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(dev, cache, 1, &info, nullptr, &p) != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
            why = "no pipeline for the draw (vkCreateGraphicsPipelines failed)";
        } else {
            stats.pipelines++, pipelines_unsaved++;
        }
        return pipelines[key] = p;
    }

    // a fullscreen pass's pipeline: the triangle and a fragment shader reading one combined image sampler
    VkPipeline helper(const uint32_t *fragment, size_t bytes, VkFormat format) {
        VkPipeline &p = helpers[{fragment, int(format)}];
        if (p) return p;
        VkShaderModule vs = make_module(FULLSCREEN_UV_VERT, sizeof(FULLSCREEN_UV_VERT));
        VkShaderModule fs = make_module(fragment, bytes);
        if (vs && fs) {
            VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL, raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            colors.attachmentCount = 1, colors.pAttachments = &blend;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2, dynamic.pDynamicStates = dynamic_states;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &format;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.pNext = &rendering, info.stageCount = 2, info.pStages = stages;
            info.pVertexInputState = &input, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
            info.pRasterizationState = &raster, info.pMultisampleState = &samples, info.pColorBlendState = &colors;
            info.pDepthStencilState = &depth, info.pDynamicState = &dynamic, info.layout = helper_layout;
            if (vkCreateGraphicsPipelines(dev, cache, 1, &info, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
        }
        if (vs) vkDestroyShaderModule(dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(dev, fs, nullptr);
        return p;
    }

    // a fullscreen pass of a helper pipeline reading `view` into `target` (width by height)
    bool helper_pass(VkPipeline pipeline, VkImageView view, VkSampler sampler, VkImageView target, uint32_t width,
                     uint32_t height, const int32_t *push = nullptr) {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool, alloc.descriptorSetCount = 1, alloc.pSetLayouts = &helper_set_layout;
        if (vkAllocateDescriptorSets(dev, &alloc, &set) != VK_SUCCESS) return false;
        const VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = &image;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        VkCommandBuffer cmd = ctx->frame->commands();
        barrier(cmd);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target, color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1, rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
        const VkRect2D all{{0, 0}, {width, height}};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, helper_layout, 0, 1, &set, 0, nullptr);
        if (push) vkCmdPushConstants(cmd, helper_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, push);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &all);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrier(cmd);
        return true;
    }

    static void barrier(VkCommandBuffer cmd) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0,
                             nullptr, 0, nullptr);
    }

    // ---- uploads ----

    bool upload(const void *data, size_t bytes, size_t alignment, VkBuffer &buffer, VkDeviceSize &at) {
        return ctx->frame->upload(data, bytes, alignment, buffer, at);
    }

    // a stage's constants: written when they differ from its last block (the floats its shader reads, the
    // bools, loops and fetch constants), else the last block's place. A block holds only what the shader
    // reads: the room of a whole ShaderConstants, the other floats left as the arena had them.
    bool stage_constants(int stage, const uint32_t *floats, const uint64_t used[4], const uint32_t *fetch,
                         const uint32_t *bools, const uint32_t *loops, VkBuffer &buffer, VkDeviceSize &at) {
        static const uint32_t zeros[40] = {};
        const uint32_t *b = bools ? bools : zeros, *l = loops ? loops : zeros;
        ShaderConstants &last_block = last[stage];
        Uploaded &u = last_at[stage];
        bool same = u.valid && std::memcmp(last_block.bool_constants, b, sizeof(last_block.bool_constants)) == 0 &&
                    std::memcmp(last_block.loop_constants, l, sizeof(last_block.loop_constants)) == 0 &&
                    std::memcmp(last_block.fetch_constants, fetch, sizeof(last_block.fetch_constants)) == 0 &&
                    std::memcmp(last_used[stage], used, 32) == 0;
        for (uint32_t block = 0; block < 4 && same; block++)
            for (uint64_t left = used[block]; left && same; left &= left - 1) {
                const uint32_t k = block * 64 + uint32_t(std::countr_zero(left));
                same = std::memcmp(last_block.float_constants[k], floats + 4 * k, 16) == 0;
            }
        if (same) {
            buffer = u.buffer, at = u.at;
            std::lock_guard<std::mutex> lock(stats_mutex);
            stats.constants_reused++;
            return true;
        }
        auto *out = static_cast<ShaderConstants *>(ctx->frame->allocate(sizeof(ShaderConstants), 256, buffer, at));
        if (!out) return false;
        for (uint32_t block = 0; block < 4; block++)
            for (uint64_t left = used[block]; left; left &= left - 1) {
                const uint32_t k = block * 64 + uint32_t(std::countr_zero(left));
                std::memcpy(out->float_constants[k], floats + 4 * k, 16);
                std::memcpy(last_block.float_constants[k], floats + 4 * k, 16);
            }
        std::memcpy(out->bool_constants, b, sizeof(out->bool_constants));
        std::memcpy(out->loop_constants, l, sizeof(out->loop_constants));
        std::memcpy(out->fetch_constants, fetch, sizeof(out->fetch_constants));
        std::memcpy(last_block.bool_constants, b, sizeof(last_block.bool_constants));
        std::memcpy(last_block.loop_constants, l, sizeof(last_block.loop_constants));
        std::memcpy(last_block.fetch_constants, fetch, sizeof(last_block.fetch_constants));
        std::memcpy(last_used[stage], used, 32);
        u = {true, buffer, at};
        std::lock_guard<std::mutex> lock(stats_mutex);
        stats.constant_uploads++;
        return true;
    }

    // a texture and its sampler, met once a stretch between resolves (a resolve may write any texture's memory)
    struct TextureKey {
        uint32_t words[7];
        bool operator==(const TextureKey &) const = default;
    };
    struct TextureKeyHash {
        size_t operator()(const TextureKey &k) const {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t w : k.words) h = (h ^ w) * 0x100000001B3ull;
            return size_t(h);
        }
    };
    struct TextureMet {
        bool ok = false;
        TextureImage image;
        VkSampler sampler = VK_NULL_HANDLE;
        std::string why;
    };
    std::unordered_map<TextureKey, TextureMet, TextureKeyHash> textures_met;

    const TextureMet &texture_of(const uint32_t fetch[6], const uint8_t filters[4]) {
        TextureKey key;
        std::memcpy(key.words, fetch, 24);
        std::memcpy(&key.words[6], filters, 4);
        auto [it, made] = textures_met.try_emplace(key);
        if (made) {
            TextureMet &m = it->second;
            m.ok = ctx->frame->texture(fetch, m.image, m.why);
            if (m.ok) m.sampler = ctx->frame->sampler(fetch, filters);
        }
        return it->second;
    }

    // set 0 for binding 1's buffer (guest memory's mirror, or the arena) and the arena (the uniform blocks):
    // written once a frame for each
    VkDescriptorSet constants_set(VkBuffer storage, VkDeviceSize storage_range, VkBuffer arena) {
        VkDescriptorSet &set = set0[{storage, arena}];
        if (set) return set;
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool, alloc.descriptorSetCount = 1, alloc.pSetLayouts = &layouts[0];
        if (vkAllocateDescriptorSets(dev, &alloc, &set) != VK_SUCCESS) return set = VK_NULL_HANDLE;
        const VkDescriptorBufferInfo buffers[4] = {{arena, 0, sizeof(ShaderConstants)},
                                                   {storage, 0, storage_range},
                                                   {arena, 0, sizeof(SystemConstants)},
                                                   {arena, 0, sizeof(ShaderConstants)}};
        VkWriteDescriptorSet writes[4];
        for (uint32_t b = 0; b < 4; b++) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = set, writes[b].dstBinding = b, writes[b].descriptorCount = 1;
            writes[b].descriptorType =
                b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
        return set;
    }

    // ---- a draw ----

    bool prepare(const DrawCall &call, Op &op, std::string &why) {
        Frame &frame = *ctx->frame;
        if (!call.registers || !call.vs || !call.vs_words || !call.vs_uses || !call.vs_floats || !call.fetch)
            return why = "a draw without its registers, vertex shader or constants", false;
        const bool has_ps = call.ps && call.ps_words && call.ps_uses && call.ps_floats;
        DrawSetup s = draw_setup(call.registers, call.primitive, *call.vs_uses, has_ps ? call.ps_uses : nullptr,
                                 device);
        if (!s.why.empty()) return why = s.why, false;
        for (const char *a : s.approximate) approximate(a);
        if (s.empty) return true;
        if (s.msaa) return why = "a multisampled target", false;
        // the targets, as tall as the scissor reaches
        const VkRect2D reach = render_scissor(call.registers, s.pitch);
        const uint32_t bottom = uint32_t(reach.offset.y) + reach.extent.height;
        for (uint32_t i = 0; i < 4; i++)
            if (s.color[i].used) {
                op.attachments.color[i] = target(false, s.color[i].edram_base, s.pitch, s.color[i].format, bottom, why);
                if (!op.attachments.color[i]) return false;
            }
        if (s.depth.used) {
            op.attachments.depth = target(true, s.depth.edram_base, s.pitch, s.depth.format, bottom, why);
            if (!op.attachments.depth) return false;
        }
        uint32_t width = UINT32_MAX, height = UINT32_MAX;
        for (Target *t : op.attachments.color)
            if (t) width = std::min(width, t->width), height = std::min(height, t->height);
        if (Target *t = op.attachments.depth) width = std::min(width, t->width), height = std::min(height, t->height);
        place_draw(s, call.registers, width, height,
                   op.attachments.depth ? op.attachments.depth->image.format : VK_FORMAT_UNDEFINED, device);
        for (const char *a : s.approximate) approximate(a);
        if (s.empty) return op.pipeline = VK_NULL_HANDLE, true;
        // the shaders and the pipeline
        VkShaderModule vs = module_of(call.vs, call.vs_words, call.vs_hash, true, s.vertex, why);
        if (!vs) return false;
        VkShaderModule ps = VK_NULL_HANDLE;
        if (s.pixel_shader && has_ps) {
            ps = module_of(call.ps, call.ps_words, call.ps_hash, false, s.pixel, why);
            if (!ps) return false;
        }
        uint64_t key = mix(mix(s.pipeline_key, call.vs_hash), ps ? call.ps_hash : 0);
        for (Target *t : op.attachments.color) key = mix(key, t ? uint64_t(t->image.format) : 0);
        key = mix(key, op.attachments.depth ? uint64_t(op.attachments.depth->image.format) : 0);
        op.pipeline = pipeline_of(s, vs, ps, op.attachments, key, why);
        if (!op.pipeline) return false;

        // the vertices: binding 1 is guest memory's mirror, or the arena with the fetch constants moved there
        const uint32_t *fetch = call.fetch;
        VkBuffer storage = VK_NULL_HANDLE;
        VkDeviceSize storage_range = VK_WHOLE_SIZE;
        if (!call.vertex_data) {
            VkDeviceSize at;
            frame.guest_memory(0, 0, storage, at);
            for (size_t k = 0; k < call.range_count; k++) frame.guest_memory(call.ranges[k][0], call.ranges[k][1], storage, at);
        } else {
            VkDeviceSize at = 0;
            static const uint32_t none[4] = {};
            const bool nothing = !call.vertex_bytes;
            if (!upload(nothing ? static_cast<const void *>(none) : call.vertex_data,
                        nothing ? sizeof(none) : call.vertex_bytes, 256, storage, at))
                return why = "the vertices did not fit the frame's upload space", false;
            fetch_words.assign(call.fetch, call.fetch + 192);
            for (uint32_t f = 0; f < 96; f++)
                if ((call.vs_uses->vertex_fetches[f / 32] >> (f % 32)) & 1)
                    fetch_words[f * 2] = (fetch_words[f * 2] & 3) | ((fetch_words[f * 2] & ~3u) + uint32_t(at));
            fetch = fetch_words.data();
        }
        // the textures
        SystemConstants &sys = s.system;
        VkDescriptorImageInfo images[3][IMAGES] = {}, samplers[SAMPLERS] = {};
        std::vector<uint64_t> textures;
        textures.reserve(64);
        for (uint32_t slot = 0; slot < SLOTS; slot++) {
            if (!((s.texture_slots >> slot) & 1)) continue;
            const uint32_t *f = &fetch[slot * 6];
            if ((f[0] & 3) != 2) {
                approximate("a texture slot without a texture constant");
                continue;
            }
            const TextureMet &met = texture_of(f, s.slot_filters[slot]);
            if (!met.ok) return why = "a texture: " + met.why, false;
            const TextureImage &t = met.image;
            const VkSampler sampler = met.sampler;
            const uint32_t kind = std::min(t.kind, 2u);
            images[kind][1 + slot] = {VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL};
            samplers[1 + slot] = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            sys.slots[slot] = (1 + slot) | kind << 14 | (1 + slot) << 16;
            sys.signs[slot] = t.signs, sys.integer_scale[slot] = t.integer_scale;
            if (t.signed_view) {
                images[kind][1 + SLOTS + slot] = {VK_NULL_HANDLE, t.signed_view, VK_IMAGE_LAYOUT_GENERAL};
                sys.signed_slots[slot] = (1 + SLOTS + slot) | kind << 14 | (1 + slot) << 16;
            } else {
                for (uint32_t k = 0; k < 4; k++)
                    if (((t.signs >> (2 * k)) & 3) == 1) {
                        approximate("signed texture components read as unsigned");
                        break;
                    }
            }
            textures.insert(textures.end(), {uint64_t(slot) | uint64_t(kind) << 8, uint64_t(t.view),
                                             uint64_t(t.signed_view), uint64_t(sampler)});
        }
        // the constants (the stages', the system's), each uploaded when it changed
        VkBuffer arena = VK_NULL_HANDLE, buffer;
        VkDeviceSize at;
        if (!stage_constants(0, call.vs_floats, call.vs_uses->float_constants, fetch, call.bools, call.loops, buffer, at))
            return why = "the constants did not fit the frame's upload space", false;
        arena = buffer, op.offsets[0] = uint32_t(at);
        if (ps) {
            if (!stage_constants(1, call.ps_floats, call.ps_uses->float_constants, fetch, call.bools, call.loops,
                                 buffer, at))
                return why = "the constants did not fit the frame's upload space", false;
            if (buffer != arena) return why = "the frame's upload space changed buffers", false;
            op.offsets[2] = uint32_t(at);
        } else {
            op.offsets[2] = op.offsets[0];
        }
        if (last_system_at.valid && std::memcmp(&sys, &last_system, sizeof(sys)) == 0) {
            buffer = last_system_at.buffer, at = last_system_at.at;
        } else {
            if (!upload(&sys, sizeof(sys), 256, buffer, at))
                return why = "the constants did not fit the frame's upload space", false;
            last_system = sys, last_system_at = {true, buffer, at};
        }
        if (buffer != arena) return why = "the frame's upload space changed buffers", false;
        op.offsets[1] = uint32_t(at);
        if (call.vertex_data) storage_range = VK_WHOLE_SIZE; // (the arena whole: the fetch constants address it)
        op.sets[0] = constants_set(storage, storage_range, arena);
        if (!op.sets[0]) return why = "out of descriptor sets this frame", false;
        // set 1: the last draw's when its textures are the same
        if (textures_set && textures == textures_key) {
            op.sets[1] = textures_set;
        } else {
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = pool, alloc.descriptorSetCount = 1, alloc.pSetLayouts = &layouts[1];
            if (vkAllocateDescriptorSets(dev, &alloc, &op.sets[1]) != VK_SUCCESS)
                return why = "out of descriptor sets this frame", false;
            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(8 + 3 * SLOTS);
            VkDescriptorImageInfo standin[3], standin_sampler{nearest, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            for (uint32_t k = 0; k < 3; k++) {
                standin[k] = {VK_NULL_HANDLE, standins[k], VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = op.sets[1], w.dstBinding = k, w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w.pImageInfo = &standin[k];
                writes.push_back(w);
            }
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = op.sets[1], ws.dstBinding = 3, ws.descriptorCount = 1;
            ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, ws.pImageInfo = &standin_sampler;
            writes.push_back(ws);
            for (uint32_t slot = 0; slot < SLOTS; slot++) {
                if (!samplers[1 + slot].sampler) continue;
                ws.dstArrayElement = 1 + slot, ws.pImageInfo = &samplers[1 + slot];
                writes.push_back(ws);
                for (uint32_t k = 0; k < 3; k++)
                    for (uint32_t e : {1 + slot, 1 + SLOTS + slot}) {
                        if (!images[k][e].imageView) continue;
                        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        w.dstSet = op.sets[1], w.dstBinding = k, w.dstArrayElement = e, w.descriptorCount = 1;
                        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w.pImageInfo = &images[k][e];
                        writes.push_back(w);
                    }
            }
            vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
            textures_set = op.sets[1], textures_key = std::move(textures);
        }
        // the indices, as the host draws them
        const bool plain = !call.indices && s.indices == IndexForm::AsGiven;
        if (plain) {
            op.count = call.count, op.first = uint32_t(int32_t(call.first) + call.base);
        } else {
            convert_indices(s, call.indices, call.count, call.first, index_words);
            if (call.base && call.indices)
                for (uint32_t &v : index_words)
                    if (v != UINT32_MAX && s.indices != IndexForm::Points) v = uint32_t(int32_t(v) + call.base);
            if (s.indices == IndexForm::Points && call.base) {
                for (uint32_t &v : index_words)
                    if (v != UINT32_MAX) v = uint32_t(int32_t(v >> 2) + call.base) << 2 | (v & 3);
            }
            if (s.indices == IndexForm::Rectangles) {
                // the corners' guest indices as the vertex attribute, the strips 4k..4k+3 between restarts
                attribute_words.swap(index_words);
                const uint32_t rects = uint32_t(attribute_words.size() / 12);
                if (!rects) return op.pipeline = VK_NULL_HANDLE, true;
                if (!upload(attribute_words.data(), attribute_words.size() * 4, 16, op.attributes, op.attributes_at))
                    return why = "the rectangles did not fit the frame's upload space", false;
                index_words.clear();
                index_words.reserve(size_t(rects) * 5);
                for (uint32_t r = 0; r < rects; r++) {
                    if (r) index_words.push_back(UINT32_MAX);
                    for (uint32_t c = 0; c < 4; c++) index_words.push_back(4 * r + c);
                }
            }
            if (index_words.empty()) return op.pipeline = VK_NULL_HANDLE, true;
            if (!upload(index_words.data(), index_words.size() * 4, 4, op.indices, op.indices_at))
                return why = "the indices did not fit the frame's upload space", false;
            op.count = uint32_t(index_words.size());
        }
        // the dynamic state
        op.viewport = s.viewport, op.scissor = s.scissor;
        std::memcpy(op.blend_constants, s.blend_constants, 16);
        for (int f = 0; f < 2; f++)
            op.stencil_reference[f] = s.stencil_reference[f], op.stencil_compare_mask[f] = s.stencil_compare_mask[f],
            op.stencil_write_mask[f] = s.stencil_write_mask[f];
        op.line_width = s.line_width;
        op.bias_constant = s.depth_bias_constant, op.bias_slope = s.depth_bias_slope;
        op.bias_representation = s.depth_bias_representation;
        return true;
    }

    // the scissor's reach (how tall a target the draw needs), before the target's size is known
    static VkRect2D render_scissor(const uint32_t *registers, uint32_t pitch) { return host_scissor(registers, pitch); }

    // ---- recording ----

    void record(VkCommandBuffer cmd, const Op &op, Bound &b) {
        if (op.clear) {
            vkCmdClearAttachments(cmd, op.value_count, op.values, 1, &op.rect);
            return;
        }
        if (op.pipeline != b.pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.pipeline = op.pipeline);
        if (op.sets[0] != b.sets[0] || op.sets[1] != b.sets[1] || std::memcmp(op.offsets, b.offsets, 12) != 0) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, op.sets, 3, op.offsets);
            b.sets[0] = op.sets[0], b.sets[1] = op.sets[1];
            std::memcpy(b.offsets, op.offsets, 12);
        }
        if (std::memcmp(&op.viewport, &b.viewport, sizeof(VkViewport)) != 0)
            vkCmdSetViewport(cmd, 0, 1, &op.viewport), b.viewport = op.viewport;
        if (std::memcmp(&op.scissor, &b.scissor, sizeof(VkRect2D)) != 0)
            vkCmdSetScissor(cmd, 0, 1, &op.scissor), b.scissor = op.scissor;
        if (std::memcmp(op.blend_constants, b.blend, 16) != 0)
            vkCmdSetBlendConstants(cmd, op.blend_constants), std::memcpy(b.blend, op.blend_constants, 16);
        if (op.line_width != b.line) vkCmdSetLineWidth(cmd, b.line = op.line_width);
        if (op.bias_constant != b.bias[0] || op.bias_slope != b.bias[1] ||
            op.bias_representation != b.bias_representation) {
            if (op.bias_representation && vkCmdSetDepthBias2EXT) {
                VkDepthBiasRepresentationInfoEXT representation{
                    VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
                representation.depthBiasRepresentation =
                    op.bias_representation == 2
                        ? VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT
                        : VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;
                representation.depthBiasExact = VK_TRUE;
                VkDepthBiasInfoEXT bias{VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &representation};
                bias.depthBiasConstantFactor = op.bias_constant, bias.depthBiasSlopeFactor = op.bias_slope;
                vkCmdSetDepthBias2EXT(cmd, &bias);
            } else {
                vkCmdSetDepthBias(cmd, op.bias_constant, 0.0f, op.bias_slope);
            }
            b.bias[0] = op.bias_constant, b.bias[1] = op.bias_slope, b.bias_representation = op.bias_representation;
        }
        const uint32_t *stencil[3] = {op.stencil_reference, op.stencil_compare_mask, op.stencil_write_mask};
        for (int kind = 0; kind < 3; kind++)
            for (int face = 0; face < 2; face++) {
                if (stencil[kind][face] == b.stencil[kind][face]) continue;
                const VkStencilFaceFlags which = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
                if (kind == 0) vkCmdSetStencilReference(cmd, which, stencil[kind][face]);
                if (kind == 1) vkCmdSetStencilCompareMask(cmd, which, stencil[kind][face]);
                if (kind == 2) vkCmdSetStencilWriteMask(cmd, which, stencil[kind][face]);
                b.stencil[kind][face] = stencil[kind][face];
            }
        if (op.attributes && (op.attributes != b.attributes || op.attributes_at != b.attributes_at))
            vkCmdBindVertexBuffers(cmd, 0, 1, &op.attributes, &op.attributes_at), b.attributes = op.attributes,
                                                                                     b.attributes_at = op.attributes_at;
        if (op.indices) {
            if (op.indices != b.indices || op.indices_at != b.indices_at)
                vkCmdBindIndexBuffer(cmd, op.indices, op.indices_at, VK_INDEX_TYPE_UINT32), b.indices = op.indices,
                                                                                           b.indices_at = op.indices_at;
            vkCmdDrawIndexed(cmd, op.count, 1, 0, 0, 0);
        } else {
            vkCmdDraw(cmd, op.count, 1, op.first, 0);
        }
    }

    // the frame's prepared ops recorded: one rendering a run of the same attachments
    void flush() {
        if (ops.empty()) return;
        VkCommandBuffer cmd = ctx->frame->commands();
        bool open = false;
        Attachments bound;
        Bound b;
        for (const Op &op : ops) {
            if (!op.clear && !op.pipeline) continue;
            if (!open || !(op.attachments == bound)) {
                if (open) vkCmdEndRendering(cmd);
                // (what the renderings before wrote, done: the same images are attached or read again)
                barrier(cmd);
                VkRenderingAttachmentInfo colors[4];
                VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                uint32_t width = UINT32_MAX, height = UINT32_MAX;
                for (int i = 0; i < 4; i++) {
                    colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    if (Target *t = op.attachments.color[i]) {
                        colors[i].imageView = t->view, colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                        width = std::min(width, t->width), height = std::min(height, t->height);
                    }
                }
                rendering.colorAttachmentCount = 4, rendering.pColorAttachments = colors;
                if (Target *t = op.attachments.depth) {
                    depth.imageView = t->view, depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    rendering.pDepthAttachment = rendering.pStencilAttachment = &depth;
                    width = std::min(width, t->width), height = std::min(height, t->height);
                }
                rendering.renderArea = {{0, 0}, {width, height}};
                rendering.layerCount = 1;
                vkCmdBeginRendering(cmd, &rendering);
                open = true, bound = op.attachments;
                b = {};
            }
            record(cmd, op, b);
            for (Target *t : op.attachments.color)
                if (t) t->written = ++written;
            if (op.attachments.depth) op.attachments.depth->written = ++written;
        }
        if (open) vkCmdEndRendering(cmd);
        barrier(cmd);
        ops.clear();
    }

    // ---- a resolve ----

    bool do_resolve(const ResolveInfo &info, std::string &why) {
        flush();
        textures_met.clear(); // (the resolve may write a texture's memory: loaded again after it)
        auto fail = [&](const char *what) {
            char place[112];
            std::snprintf(place, sizeof(place), " (%s base %X pitch %u msaa %u format %u)",
                          info.depth ? "depth" : "color", info.edram_base, info.surface_pitch, info.msaa,
                          info.source_format);
            why = std::string(what) + place;
            return false;
        };
        // the view's samples on the single-sampled tiles: 2x two rows a row, 4x a 2x2 block a pixel
        const uint32_t sx = info.msaa == 2 ? 2 : 1, sy = info.msaa >= 1 ? 2 : 1;
        const uint32_t pitch = info.surface_pitch * sx;
        if (info.copy) {
            uint32_t format = info.source_format;
            if (!info.depth && format == 10) format = 2;
            if (!info.depth && format == 12) format = 3;
            if (info.depth) format &= 1;
            // the target written last at the place (the formats there share its bits)
            const Target *source = nullptr;
            for (const auto &[key, t] : targets)
                if (t.depth == info.depth && t.base == info.edram_base && t.pitch == pitch && t.written &&
                    (!source || t.written > source->written))
                    source = &t;
            if (!source) return fail("a resolve from a target the port has not drawn");
            if (info.depth && info.msaa) return fail("a depth resolve of a multisampled view");
            VkImageView view = source->sampled;
            VkFormat vk_format = source->image.format;
            uint32_t source_format = format;
            if (source->format != format) {
                if (!info.depth && format == 2 && source->format == 3) {
                    // a 7e3 target's bits read as 2_10_10_10
                    Target *packed = scratch_image(VK_FORMAT_A2B10G10R10_UNORM_PACK32, source->width, source->height);
                    VkPipeline pack = helper(PACK7E3_FRAG, sizeof(PACK7E3_FRAG), VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                    if (!packed || !pack) return fail("the packing pass not made");
                    helper_pass(pack, view, nearest, packed->view, source->width, source->height);
                    source = packed, view = packed->sampled, vk_format = packed->image.format, source_format = 2;
                } else {
                    approximate("resolves of a place another format wrote last");
                    auto it = targets.find(std::make_tuple(info.depth, info.edram_base, pitch, format));
                    if (it == targets.end()) return fail("a resolve of a place another format wrote");
                    source = &it->second, view = source->sampled, vk_format = source->image.format;
                }
            }
            ResolveInfo described = info;
            if (!info.depth) described.source_format = source_format;
            if (info.msaa) {
                // the view's samples as the resolve selects them (resolve.h's sample selections), averaged
                const uint32_t select = info.sample_select;
                const int32_t push[4] = {int32_t(sx), int32_t(sy),
                                         int32_t(select <= 3 ? select : select == 5 ? 2 : 0),
                                         int32_t(select <= 3 ? 1 : select == 6 ? 4 : 2)};
                const uint32_t w = source->width / sx, h = source->height / sy;
                Target *samples = scratch_image(vk_format, w, h);
                VkPipeline down = helper(DOWNSAMPLE_FRAG, sizeof(DOWNSAMPLE_FRAG), vk_format);
                if (!samples || !down) return fail("the downsampling pass not made");
                helper_pass(down, view, nearest, samples->view, w, h, push);
                source = samples;
                described.msaa = 0, described.sample_select = 0;
            }
            TargetImage image;
            image.image = source->image.image, image.width = source->width, image.height = source->height;
            if (!ctx->frame->resolve(described, image, why)) return false;
        }
        // its clears, over the rectangle of the targets they hit (the view's pixels on the single-sampled tiles)
        if ((info.clear_color || info.clear_depth) && info.width && info.height) {
            Attachments a;
            Op op;
            op.clear = true;
            const uint32_t bottom = (info.y + info.height) * sy;
            if (info.clear_color) {
                a.color[0] = target(false, info.clear_color_base, pitch, info.clear_color_format, bottom, why);
                if (!a.color[0]) return false;
                VkClearAttachment &v = op.values[op.value_count++];
                v.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, v.colorAttachment = 0;
                v.clearValue = clear_value(false, a.color[0]->format, info.clear_color_value);
            }
            if (info.clear_depth) {
                a.depth = target(true, info.clear_depth_base, pitch, info.clear_depth_format, bottom, why);
                if (!a.depth) return false;
                VkClearAttachment &v = op.values[op.value_count++];
                v.aspectMask = DEPTH_STENCIL;
                const uint32_t words[2] = {info.clear_depth_value, 0};
                v.clearValue = clear_value(true, a.depth->format, words);
            }
            const Target *any = a.color[0] ? a.color[0] : a.depth;
            const uint32_t x = std::min(info.x * sx, any->width), y = std::min(info.y * sy, any->height);
            const uint32_t w = std::min(info.width * sx, any->width - x), h = std::min(info.height * sy, any->height - y);
            if (w && h) {
                op.rect = {{{int32_t(x), int32_t(y)}, {w, h}}, 0, 1};
                op.attachments = a;
                ops.push_back(op);
                flush();
            }
        }
        std::lock_guard<std::mutex> lock(stats_mutex);
        stats.resolves++;
        return true;
    }
};

Drawer::Drawer(std::filesystem::path shaders) : impl_(std::make_unique<Impl>()) {
    impl_->shader_dir = shaders;
    impl_->shaders = std::make_unique<ShaderCache>(std::move(shaders));
}

Drawer::~Drawer() {
    Impl &d = *impl_;
    if (!d.dev) return;
    d.save_pipelines();
    // (the device: idle by now when the run ends, as the renderer's own objects go)
    vkDeviceWaitIdle(d.dev);
    for (auto &[k, t] : d.targets) d.destroy(t);
    for (auto &[k, t] : d.scratch) d.destroy(t);
    for (auto &[k, t] : d.retired) d.destroy(t);
    for (auto &[k, p] : d.pipelines)
        if (p) vkDestroyPipeline(d.dev, p, nullptr);
    for (auto &[k, p] : d.helpers)
        if (p) vkDestroyPipeline(d.dev, p, nullptr);
    for (auto &[k, m] : d.modules)
        if (m) vkDestroyShaderModule(d.dev, m, nullptr);
    for (VkDescriptorPool p : d.pools)
        if (p) vkDestroyDescriptorPool(d.dev, p, nullptr);
    for (int k = 0; k < 3; k++) {
        if (d.standins[k]) vkDestroyImageView(d.dev, d.standins[k], nullptr);
        if (d.standin_images[k].image) d.device->destroy(d.standin_images[k]);
    }
    for (VkSampler s : {d.nearest, d.linear})
        if (s) vkDestroySampler(d.dev, s, nullptr);
    if (d.cache) vkDestroyPipelineCache(d.dev, d.cache, nullptr);
    if (d.layout) vkDestroyPipelineLayout(d.dev, d.layout, nullptr);
    if (d.helper_layout) vkDestroyPipelineLayout(d.dev, d.helper_layout, nullptr);
    for (VkDescriptorSetLayout l : {d.layouts[0], d.layouts[1], d.helper_set_layout})
        if (l) vkDestroyDescriptorSetLayout(d.dev, l, nullptr);
}

bool Drawer::begin(const OutputContext &ctx, std::string &why) {
    Impl &d = *impl_;
    d.ctx = &ctx;
    if (!d.made) {
        d.made = true;
        d.device = ctx.device, d.dev = ctx.vk_device;
        if (!d.device) d.failed = true, d.failed_why = "no device";
        else d.failed = !d.make_objects(d.failed_why);
    }
    if (d.failed) return why = "the Drawer's Vulkan objects were not made: " + d.failed_why, false;
    d.pool = d.pools[d.stats.frames % POOLS];
    while (!d.retired.empty() && d.retired.front().first + POOLS <= d.stats.frames)
        d.destroy(d.retired.front().second), d.retired.erase(d.retired.begin());
    vkResetDescriptorPool(d.dev, d.pool, 0);
    d.ops.clear();
    d.set0.clear();
    d.last_at[0] = d.last_at[1] = {}, d.last_system_at = {};
    d.textures_set = VK_NULL_HANDLE, d.textures_key.clear();
    d.textures_met.clear();
    d.frame_draws = 0;
    return true;
}

bool Drawer::draw(const DrawCall &call, std::string &why) {
    Impl &d = *impl_;
    const uint32_t index = d.frame_draws++;
    if (d.debug.skip_swap == d.ctx->swap && index >= d.debug.skip_first && index <= d.debug.skip_last) return true;
    Op op;
    if (!d.prepare(call, op, why)) return false;
    if (d.debug.log_swap == d.ctx->swap) d.log_draw(index, call, op);
    if (op.pipeline) {
        d.ops.push_back(op);
        std::lock_guard<std::mutex> lock(d.stats_mutex);
        d.stats.draws++;
    }
    return true;
}

bool Drawer::clear(const ClearCall &call, std::string &why) {
    Impl &d = *impl_;
    const uint32_t pitch = call.surface_info & 0x3FFF;
    if ((call.surface_info >> 16) & 3) return why = "a clear of a multisampled target", false;
    const uint32_t bottom = uint32_t(std::max(call.area[3], 0));
    Op op;
    op.clear = true;
    for (uint32_t i = 0; i < 4; i++) {
        if (!((call.targets >> i) & 1)) continue;
        const uint32_t info = call.color_info[i];
        Target *t = d.target(false, info & 0xFFF, pitch, (info >> 16) & 0xF, bottom, why);
        if (!t) return false;
        op.attachments.color[i] = t;
        VkClearAttachment &v = op.values[op.value_count++];
        v.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, v.colorAttachment = i;
        v.clearValue = clear_value(false, t->format, call.color, 0.0f, 0);
    }
    if (call.depth || call.stencil) {
        Target *t = d.target(true, call.depth_info & 0xFFF, pitch, (call.depth_info >> 16) & 1, bottom, why);
        if (!t) return false;
        op.attachments.depth = t;
        VkClearAttachment &v = op.values[op.value_count++];
        v.aspectMask = (call.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (call.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        v.clearValue = clear_value(true, t->format, nullptr, call.z, call.stencil_value);
    }
    if (!op.value_count) return true;
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    for (Target *t : op.attachments.color)
        if (t) width = std::min(width, t->width), height = std::min(height, t->height);
    if (Target *t = op.attachments.depth) width = std::min(width, t->width), height = std::min(height, t->height);
    const int32_t x1 = std::max(call.area[0], 0), y1 = std::max(call.area[1], 0);
    const int32_t x2 = std::min(call.area[2], int32_t(width)), y2 = std::min(call.area[3], int32_t(height));
    if (x1 >= x2 || y1 >= y2) return true;
    op.rect = {{{x1, y1}, {uint32_t(x2 - x1), uint32_t(y2 - y1)}}, 0, 1};
    d.ops.push_back(op);
    std::lock_guard<std::mutex> lock(d.stats_mutex);
    d.stats.clears++;
    return true;
}

bool Drawer::resolve(const ResolveInfo &info, std::string &why) { return impl_->do_resolve(info, why); }

bool Drawer::present(const uint32_t fetch[6], std::string &why) {
    Impl &d = *impl_;
    d.flush();
    if (!fetch) return why = "no front buffer", false;
    TextureImage front;
    if (!d.ctx->frame->texture(fetch, front, why)) return why = "the front buffer: " + why, false;
    if (front.kind != 0) return why = "a front buffer that is not a 2D texture", false;
    VkPipeline p = d.helper(PRESENT_FRAG, sizeof(PRESENT_FRAG), d.ctx->output_format);
    if (!p) return why = "the present pass was not made", false;
    return d.helper_pass(p, front.view, d.linear, d.ctx->output_view, d.ctx->width, d.ctx->height) ||
           (why = "out of descriptor sets this frame", false);
}

bool Drawer::present_target(uint32_t edram_base, uint32_t pitch, uint32_t format, std::string &why) {
    Impl &d = *impl_;
    d.flush();
    const Target *best = nullptr;
    for (const auto &[k, t] : d.targets)
        if (!t.depth && t.base == edram_base && t.pitch == pitch && (format == UINT32_MAX || t.format == format) &&
            (!best || t.written > best->written))
            best = &t;
    if (!best) return why = "no such target", false;
    VkPipeline p = d.helper(PRESENT_FRAG, sizeof(PRESENT_FRAG), d.ctx->output_format);
    if (!p) return why = "the present pass was not made", false;
    return d.helper_pass(p, best->array, d.linear, d.ctx->output_view, d.ctx->width, d.ctx->height) ||
           (why = "out of descriptor sets this frame", false);
}

bool Drawer::end() {
    Impl &d = *impl_;
    d.flush();
    {
        std::lock_guard<std::mutex> lock(d.stats_mutex);
        d.stats.frames++;
    }
    // the pipelines made kept on disk now and then (a run may end without the Drawer's destructor)
    if (d.pipelines_unsaved && d.stats.frames % 600 == 0) d.save_pipelines();
    d.ctx = nullptr;
    return true;
}

void Drawer::cancel() {
    Impl &d = *impl_;
    d.ops.clear();
    std::lock_guard<std::mutex> lock(d.stats_mutex);
    d.stats.frames++;
    d.ctx = nullptr;
}

std::vector<Drawer::TargetInfo> Drawer::targets() const {
    std::vector<TargetInfo> out;
    for (const auto &[k, t] : impl_->targets)
        out.push_back({t.depth, t.base, t.pitch, t.format, t.width, t.height, t.image.format, t.written});
    return out;
}

Drawer::Stats Drawer::stats() const {
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    return impl_->stats;
}

} // namespace xf::native
