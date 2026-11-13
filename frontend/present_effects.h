// The window's picture effects (docs/runtime.md, "Picture options"), apart
// from the window so that they run on any image (the presenter's pictures; the
// tests' synthetic ones): antialiasing at the picture's size (FXAA 3.11 or
// SMAA 1x), then the output scaling to the destination's size (AMD FSR 1, AMD
// CAS, or the output resampling: area, bicubic, sharp bilinear), each pass a
// compute shader (present_effects.comp) writing an RGBA8 image. What is left
// to a blit (a linear or nearest scale, or a copy at the same size) the caller
// does.

#pragma once

#include "xenoforge/vk_device.h"

#include <volk.h>

#include <cstdint>
#include <functional>
#include <string>

namespace xf::frontend {

enum class Antialiasing : uint32_t { none, fxaa, fxaa_extreme, smaa_medium, smaa_high, smaa_ultra };
// The output resampling (Dolphin's choices): automatic (area where the picture is larger than the destination,
// else the linear blit), the linear blit, area sampling, the cubics (B-spline, Mitchell-Netravali, Catmull-Rom) and
// sharp bilinear
enum class Resampling : uint32_t { automatic, bilinear, area, bspline, mitchell, catmull_rom, sharp_bilinear };
// settings.h's values ("fxaa", "smaa_high", "area", ...; unknown ones are none and automatic)
Antialiasing antialiasing_from(const std::string &value);
Resampling resampling_from(const std::string &value);

struct EffectOptions {
    bool smooth = true; // false: nearest, whatever the resampling
    // the output scaling (settings.h's upscaler): 0 the resampling, 1 FSR 1, 2 CAS; and their sharpness
    uint32_t upscaler = 0;
    float fsr_sharpness_reduction = 0.2f, cas_sharpness = 0.0f;
    Antialiasing antialiasing = Antialiasing::none;
    Resampling resampling = Resampling::automatic;
};

// The resampling filter a picture of width x height takes into dest_width x dest_height: bilinear where the linear
// blit does it (or nothing is left to resample: the same size, nearest, FSR or CAS asked for), else the filter that
// runs as a pass.
Resampling resampling_pass(const EffectOptions &options, uint32_t width, uint32_t height, uint32_t dest_width,
                           uint32_t dest_height);
// whether any pass runs (else the blit does it all)
bool effects_apply(const EffectOptions &options, uint32_t width, uint32_t height, uint32_t dest_width,
                   uint32_t dest_height);

class PictureEffects {
  public:
    // The pipelines, the passes' descriptor sets and SMAA's lookup textures (their upload recorded with the first
    // passes). idle: waits until the GPU no longer reads the passes' images (called before one is made again at
    // another size).
    bool open(vk::Device &device, std::function<void()> idle, std::string &error);
    ~PictureEffects();

    // The passes the options ask for, from source (a view of an image in VK_IMAGE_LAYOUT_GENERAL, sampled) of width
    // x height to dest_width x dest_height, recorded into cmd with the descriptor sets of `frame` (0 or 1, one per
    // frame in flight). The last pass's image, in VK_IMAGE_LAYOUT_GENERAL with its writes made visible, and its size;
    // null when no pass applies or one could not be made.
    struct Output {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };
    bool record(VkCommandBuffer cmd, uint32_t frame, const EffectOptions &options, VkImageView source, uint32_t width,
                uint32_t height, uint32_t dest_width, uint32_t dest_height, Output &out);

    // For the tests' timing: a timestamp written into `pool` before the first pass (query 0) and after each (query
    // n), at most `count` (the queries reset in the command buffer); passes() is how many the last record() ran.
    void timestamps(VkQueryPool pool, uint32_t count) { timestamps_ = pool, timestamp_count_ = count; }
    uint32_t passes() const { return passes_; }

  private:
    enum Effect {
        EASU,
        RCAS,
        CAS,
        FXAA,
        FXAA_EXTREME,
        AREA,
        BICUBIC,
        SHARP_BILINEAR,
        SMAA_EDGES,
        SMAA_WEIGHTS_MEDIUM,
        SMAA_WEIGHTS_HIGH,
        SMAA_WEIGHTS_ULTRA,
        SMAA_BLEND,
        EFFECTS
    };
    // SMAA's three, then FSR's EASU passes (at most 4) and RCAS
    static constexpr uint32_t PASSES = 8;
    struct Image {
        vk::Image image;
        VkImageView view = VK_NULL_HANDLE;
    };
    Image *image(uint32_t slot, uint32_t width, uint32_t height);
    bool make_lookup(Image &image, VkFormat format, uint32_t width, uint32_t height);

    vk::Device *device_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    std::function<void()> idle_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[EFFECTS] = {};
    VkSampler linear_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2][PASSES] = {};
    Image images_[PASSES]; // the passes' outputs, by the pass's place in the chain
    // SMAA's precomputed textures (third_party/smaa: AreaTex.h, RG8 160x560; SearchTex.h, R8 64x16), uploaded from
    // `staging_` by the first record()
    Image area_, search_;
    vk::Buffer staging_;
    bool uploaded_ = false;
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    uint32_t timestamp_count_ = 0, passes_ = 0;
};

} // namespace xf::frontend
