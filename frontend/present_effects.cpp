// The window's picture effects (present_effects.h). The chain after Xenia's
// presenter (presenter.cc's PaintAndPresentImpl) for FXAA, FSR and CAS; SMAA
// 1x's three passes (Jimenez et al., third_party/smaa) and the output
// resampling beside them.

#include "present_effects.h"

#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

#include "presenter.h" // easu_passes, cas_sharpness_constant

#include <cmath>
#include <cstring>
#include <utility>

namespace xf::frontend {

namespace {

// present_effects.comp's variants, in PictureEffects::Effect's order
const uint32_t EFFECT_EASU[] = {
#include "present_EASU.comp.inc"
};
const uint32_t EFFECT_RCAS[] = {
#include "present_RCAS.comp.inc"
};
const uint32_t EFFECT_CAS[] = {
#include "present_CAS.comp.inc"
};
const uint32_t EFFECT_FXAA[] = {
#include "present_FXAA.comp.inc"
};
const uint32_t EFFECT_FXAA_EXTREME[] = {
#include "present_FXAA_EXTREME.comp.inc"
};
const uint32_t EFFECT_AREA[] = {
#include "present_AREA.comp.inc"
};
const uint32_t EFFECT_BICUBIC[] = {
#include "present_BICUBIC.comp.inc"
};
const uint32_t EFFECT_SHARP_BILINEAR[] = {
#include "present_SHARP_BILINEAR.comp.inc"
};
const uint32_t EFFECT_SMAA_EDGES[] = {
#include "present_SMAA_EDGES.comp.inc"
};
const uint32_t EFFECT_SMAA_WEIGHTS_MEDIUM[] = {
#include "present_SMAA_WEIGHTS_MEDIUM.comp.inc"
};
const uint32_t EFFECT_SMAA_WEIGHTS_HIGH[] = {
#include "present_SMAA_WEIGHTS_HIGH.comp.inc"
};
const uint32_t EFFECT_SMAA_WEIGHTS_ULTRA[] = {
#include "present_SMAA_WEIGHTS_ULTRA.comp.inc"
};
const uint32_t EFFECT_SMAA_BLEND[] = {
#include "present_SMAA_BLEND.comp.inc"
};

// present_effects.comp's push constants
struct EffectPush {
    uint32_t dest_width, dest_height;
    float source_width, source_height, dest_width_f, dest_height_f;
    float sharpness;
    uint32_t scaling;
    float param[2];
};

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void memory_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

} // namespace

Antialiasing antialiasing_from(const std::string &value) {
    if (value == "fxaa") return Antialiasing::fxaa;
    if (value == "fxaa_extreme") return Antialiasing::fxaa_extreme;
    if (value == "smaa_medium") return Antialiasing::smaa_medium;
    if (value == "smaa_high") return Antialiasing::smaa_high;
    if (value == "smaa_ultra") return Antialiasing::smaa_ultra;
    return Antialiasing::none;
}

Resampling resampling_from(const std::string &value) {
    if (value == "bilinear") return Resampling::bilinear;
    if (value == "area") return Resampling::area;
    if (value == "bspline") return Resampling::bspline;
    if (value == "mitchell") return Resampling::mitchell;
    if (value == "catmull_rom") return Resampling::catmull_rom;
    if (value == "sharp_bilinear") return Resampling::sharp_bilinear;
    return Resampling::automatic;
}

Resampling resampling_pass(const EffectOptions &options, uint32_t width, uint32_t height, uint32_t dest_width,
                           uint32_t dest_height) {
    if (!options.smooth || options.upscaler || (width == dest_width && height == dest_height))
        return Resampling::bilinear;
    if (options.resampling == Resampling::automatic)
        return dest_width < width || dest_height < height ? Resampling::area : Resampling::bilinear;
    return options.resampling;
}

bool effects_apply(const EffectOptions &options, uint32_t width, uint32_t height, uint32_t dest_width,
                   uint32_t dest_height) {
    return options.upscaler || options.antialiasing != Antialiasing::none ||
           resampling_pass(options, width, height, dest_width, dest_height) != Resampling::bilinear;
}

bool PictureEffects::open(vk::Device &device, std::function<void()> idle, std::string &error) {
    device_ = &device, dev_ = device.device(), idle_ = std::move(idle);
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 4;
    sli.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev_, &sli, nullptr, &set_layout_) != VK_SUCCESS)
        return error = "the effects' descriptor set layout", false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EffectPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_) != VK_SUCCESS)
        return error = "the effects' pipeline layout", false;
    const std::pair<const uint32_t *, size_t> code[EFFECTS] = {
        {EFFECT_EASU, sizeof(EFFECT_EASU)},
        {EFFECT_RCAS, sizeof(EFFECT_RCAS)},
        {EFFECT_CAS, sizeof(EFFECT_CAS)},
        {EFFECT_FXAA, sizeof(EFFECT_FXAA)},
        {EFFECT_FXAA_EXTREME, sizeof(EFFECT_FXAA_EXTREME)},
        {EFFECT_AREA, sizeof(EFFECT_AREA)},
        {EFFECT_BICUBIC, sizeof(EFFECT_BICUBIC)},
        {EFFECT_SHARP_BILINEAR, sizeof(EFFECT_SHARP_BILINEAR)},
        {EFFECT_SMAA_EDGES, sizeof(EFFECT_SMAA_EDGES)},
        {EFFECT_SMAA_WEIGHTS_MEDIUM, sizeof(EFFECT_SMAA_WEIGHTS_MEDIUM)},
        {EFFECT_SMAA_WEIGHTS_HIGH, sizeof(EFFECT_SMAA_WEIGHTS_HIGH)},
        {EFFECT_SMAA_WEIGHTS_ULTRA, sizeof(EFFECT_SMAA_WEIGHTS_ULTRA)},
        {EFFECT_SMAA_BLEND, sizeof(EFFECT_SMAA_BLEND)}};
    for (uint32_t e = 0; e < EFFECTS; e++) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = code[e].second;
        smi.pCode = code[e].first;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev_, &smi, nullptr, &module) != VK_SUCCESS)
            return error = "an effect's shader module", false;
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = layout_;
        VkResult made = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipelines_[e]);
        vkDestroyShaderModule(dev_, module, nullptr);
        if (made != VK_SUCCESS) return error = "an effect's pipeline (" + std::to_string(e) + ")", false;
    }
    // SMAA wants every texture sampled linearly and clamped (SMAA.hlsl's usage notes)
    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev_, &smp, nullptr, &linear_) != VK_SUCCESS) return error = "the effects' sampler", false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * PASSES * 3},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * PASSES}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2 * PASSES;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev_, &dpi, nullptr, &pool_) != VK_SUCCESS) return error = "the effects' pool", false;
    for (auto &frame : sets_)
        for (VkDescriptorSet &set : frame) {
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = pool_;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &set_layout_;
            if (vkAllocateDescriptorSets(dev_, &dai, &set) != VK_SUCCESS) return error = "the effects' sets", false;
        }
    // SMAA's lookup textures, staged here and copied by the first record() (no wait on any queue for them)
    static_assert(sizeof(areaTexBytes) == AREATEX_SIZE && sizeof(searchTexBytes) == SEARCHTEX_SIZE);
    staging_ = device_->create_buffer(AREATEX_SIZE + SEARCHTEX_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!staging_.mapped) return error = "the SMAA textures' staging buffer", false;
    std::memcpy(staging_.mapped, areaTexBytes, AREATEX_SIZE);
    std::memcpy(static_cast<uint8_t *>(staging_.mapped) + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
    if (!make_lookup(area_, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT) ||
        !make_lookup(search_, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT))
        return error = "the SMAA textures", false;
    return true;
}

bool PictureEffects::make_lookup(Image &image, VkFormat format, uint32_t width, uint32_t height) {
    image.image = device_->create_image(VK_IMAGE_TYPE_2D, format, {width, height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!image.image.image) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(dev_, &vi, nullptr, &image.view) == VK_SUCCESS;
}

PictureEffects::~PictureEffects() {
    if (!dev_) return;
    for (Image *e : {&area_, &search_}) {
        if (e->view) vkDestroyImageView(dev_, e->view, nullptr);
        device_->destroy(e->image);
    }
    for (Image &e : images_) {
        if (e.view) vkDestroyImageView(dev_, e.view, nullptr);
        device_->destroy(e.image);
    }
    device_->destroy(staging_);
    for (VkPipeline p : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    vkDestroyPipelineLayout(dev_, layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    vkDestroyDescriptorPool(dev_, pool_, nullptr);
    vkDestroySampler(dev_, linear_, nullptr);
}

// A pass's output image, made again when its size changes (once the GPU no
// longer reads the old one: an earlier frame may still)
PictureEffects::Image *PictureEffects::image(uint32_t slot, uint32_t width, uint32_t height) {
    Image &e = images_[slot];
    if (e.image.image && e.image.extent.width == width && e.image.extent.height == height) return &e;
    if (e.image.image) {
        if (idle_) idle_();
        vkDestroyImageView(dev_, e.view, nullptr);
        device_->destroy(e.image);
        e = {};
    }
    e.image = device_->create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {width, height, 1}, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!e.image.image) return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = e.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev_, &vi, nullptr, &e.view) != VK_SUCCESS) return nullptr;
    return &e;
}

// Xenia's presenter's chain for FXAA, FSR and CAS: the antialiasing at the
// picture's size; then FSR 1 when upscaling (EASU in passes of at most 2x,
// RCAS with the sharpness reduction in stops at the end) or CAS (sharpening,
// resampling when the sizes differ; FSR asked for without an upscale takes
// CAS too, with no additional sharpness, as Xenia's does). Without either, the
// resampling filter, where one other than the blit applies.
bool PictureEffects::record(VkCommandBuffer cmd, uint32_t frame, const EffectOptions &options, VkImageView source,
                            uint32_t width, uint32_t height, uint32_t dest_width, uint32_t dest_height, Output &out) {
    passes_ = 0;
    if (!dev_ || !source || !width || !height || !dest_width || !dest_height) return false;
    if (!uploaded_) {
        uploaded_ = true;
        image_barrier(cmd, area_.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        image_barrier(cmd, search_.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy area{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};
        VkBufferImageCopy search{AREATEX_SIZE, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {},
                                 {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}};
        vkCmdCopyBufferToImage(cmd, staging_.buffer, area_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &area);
        vkCmdCopyBufferToImage(cmd, staging_.buffer, search_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &search);
        image_barrier(cmd, area_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        image_barrier(cmd, search_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (timestamps_ && timestamp_count_) {
        vkCmdResetQueryPool(cmd, timestamps_, 0, timestamp_count_);
        memory_barrier(cmd); // what came before done, so that the first timestamp starts the passes alone
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, 0);
    }
    VkImageView input = source;
    uint32_t sw = width, sh = height, slot = 0;
    Image *last = nullptr;
    // one pass from `from` (and `extra`, binding 2) into the chain's next image of w x h
    auto pass = [&](Effect effect, VkImageView from, VkImageView extra, uint32_t w, uint32_t h, float sharpness,
                    uint32_t scaling, float param0 = 0.0f, float param1 = 0.0f) -> Image * {
        if (slot >= PASSES) return nullptr;
        Image *img = image(slot, w, h);
        if (!img) return nullptr;
        VkDescriptorSet set = sets_[frame & 1][slot++];
        VkDescriptorImageInfo in{linear_, from, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo to{VK_NULL_HANDLE, img->view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo aux{linear_, extra ? extra : area_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo look{linear_, search_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[4] = {};
        const VkDescriptorImageInfo *infos[4] = {&in, &to, &aux, &look};
        for (uint32_t i = 0; i < 4; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = infos[i];
        }
        vkUpdateDescriptorSets(dev_, 4, writes, 0, nullptr);
        // what came before (the picture's making, the pass before, an earlier
        // frame reading this image) done first
        memory_barrier(cmd);
        image_barrier(cmd, img->image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[effect]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        const uint32_t fw = from == input ? sw : w, fh = from == input ? sh : h; // (only the chain's input differs)
        EffectPush p{w, h, float(fw), float(fh), float(w), float(h), sharpness, scaling, {param0, param1}};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        passes_++;
        if (timestamps_ && passes_ < timestamp_count_)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, passes_);
        last = img;
        return img;
    };
    // the next pass in the chain, from the last one's output
    auto step = [&](Effect effect, uint32_t w, uint32_t h, float sharpness, uint32_t scaling, float param0 = 0.0f,
                    float param1 = 0.0f) {
        Image *img = pass(effect, input, VK_NULL_HANDLE, w, h, sharpness, scaling, param0, param1);
        if (!img) return false;
        input = img->view, sw = w, sh = h;
        return true;
    };
    bool ok = true;
    switch (options.antialiasing) {
    case Antialiasing::none: break;
    case Antialiasing::fxaa: ok = step(FXAA, sw, sh, 0.0f, 0); break;
    case Antialiasing::fxaa_extreme: ok = step(FXAA_EXTREME, sw, sh, 0.0f, 0); break;
    case Antialiasing::smaa_medium:
    case Antialiasing::smaa_high:
    case Antialiasing::smaa_ultra: {
        // the presets' thresholds and searches (SMAA.hlsl's SMAA_PRESET_*): the edges from the picture, the blending
        // weights from the edges, the picture blended with its neighbours by them
        const bool ultra = options.antialiasing == Antialiasing::smaa_ultra;
        const Effect weights = ultra ? SMAA_WEIGHTS_ULTRA
                               : options.antialiasing == Antialiasing::smaa_high ? SMAA_WEIGHTS_HIGH
                                                                                 : SMAA_WEIGHTS_MEDIUM;
        Image *edges = pass(SMAA_EDGES, input, VK_NULL_HANDLE, sw, sh, 0.0f, 0, ultra ? 0.05f : 0.1f);
        Image *blend = edges ? pass(weights, edges->view, area_.view, sw, sh, 0.0f, 0) : nullptr;
        Image *done = blend ? pass(SMAA_BLEND, input, blend->view, sw, sh, 0.0f, 0) : nullptr;
        if (!done) ok = false;
        else input = done->view;
        break;
    }
    }
    if (ok && options.upscaler == 1) {
        const auto steps = easu_passes(sw, sh, dest_width, dest_height);
        if (!steps.empty()) {
            for (auto [w, h] : steps) ok = ok && step(EASU, w, h, 0.0f, 0);
            ok = ok && step(RCAS, dest_width, dest_height, std::exp2(-options.fsr_sharpness_reduction), 0);
        } else {
            ok = step(CAS, dest_width, dest_height, cas_sharpness_constant(0.0f),
                      sw != dest_width || sh != dest_height);
        }
    } else if (ok && options.upscaler == 2) {
        ok = step(CAS, dest_width, dest_height, cas_sharpness_constant(options.cas_sharpness),
                  sw != dest_width || sh != dest_height);
    } else if (ok) {
        switch (resampling_pass(options, sw, sh, dest_width, dest_height)) {
        case Resampling::area: ok = step(AREA, dest_width, dest_height, 0.0f, 0); break;
        case Resampling::bspline: ok = step(BICUBIC, dest_width, dest_height, 0.0f, 0, 1.0f, 0.0f); break;
        case Resampling::mitchell:
            ok = step(BICUBIC, dest_width, dest_height, 0.0f, 0, 1.0f / 3.0f, 1.0f / 3.0f);
            break;
        case Resampling::catmull_rom: ok = step(BICUBIC, dest_width, dest_height, 0.0f, 0, 0.0f, 0.5f); break;
        case Resampling::sharp_bilinear: ok = step(SHARP_BILINEAR, dest_width, dest_height, 0.0f, 0); break;
        default: break; // the blit
        }
    }
    if (!ok || !last) return false;
    memory_barrier(cmd); // the last pass's writes before whatever reads them
    out = {last->image.image, last->view, sw, sh};
    return true;
}

} // namespace xf::frontend
