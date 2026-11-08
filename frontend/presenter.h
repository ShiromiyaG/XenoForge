// The window's pictures (docs/runtime.md, "The player"): the guest's front
// buffers shown in an SDL3 window through a Vulkan swapchain on the
// renderer's own device, scaled to fit and letterboxed, with Dear ImGui's
// overlay drawn over them. Nothing leaves the GPU.
//
// Two threads, one clock. At each swap, on the renderer's thread (its
// on_swap hook), take() makes the front buffer the window's next picture
// with a compute shader (the fetch constant's swizzle, the display's gamma
// ramp, as the read-back makes it on the CPU) into one of three pictures of
// its own, submitted after the swap's work on the renderer's queue, and
// returns: it never waits for the GPU or the window. The window's thread
// shows the newest picture whenever the display lets it (present()), on a
// queue of its own where the device has a second one, so a present waiting
// for the display never holds the renderer. Timeline semaphores order the
// two: a picture is shown once its conversion is done, and written again
// only once the window's last use of it is done (else the swap is not
// shown, and counted).

#pragma once

#include "xenoforge/renderer.h"
#include "xenoforge/vk_device.h"

#include <volk.h> // Vulkan without prototypes, its functions loaded at run time (cmake/vulkan.cmake)

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xf::frontend {

// Where the picture goes in the window: its shape kept and fitted (letterboxed or pillarboxed), the largest whole
// multiple of its size that fits (integer scaling; fitted when not even 1x fits), or stretched over the window.
enum class Fit { aspect, integer, stretch };
struct Rect {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};
Rect fit_picture(uint32_t window_width, uint32_t window_height, uint32_t width, uint32_t height, Fit fit);
// The sizes FSR's EASU upscales through, as Xenia's presenter chains its passes: at most twice the size a pass along
// each axis, the last one the destination's, up to max_passes (the last then takes the rest). Empty when the
// destination is no larger than the source along both axes (no upscale: CAS resamples instead).
std::vector<std::pair<uint32_t, uint32_t>> easu_passes(uint32_t source_width, uint32_t source_height,
                                                      uint32_t dest_width, uint32_t dest_height,
                                                      uint32_t max_passes = 4);
// CasSetup's sharpness constant: -1 / lerp(8, 5, sharpness) for sharpness 0-1
float cas_sharpness_constant(float sharpness);

class Presenter {
  public:
    struct View {
        uint32_t width = 0, height = 0; // the window's, in pixels
        bool vsync = true, smooth = true;
        Fit fit = Fit::aspect;
        // the output scaling (settings.h's upscaler): 0 a blit, 1 FSR 1, 2 CAS; and their sharpness
        uint32_t upscaler = 0;
        float fsr_sharpness_reduction = 0.2f, cas_sharpness = 0.0f;
        uint32_t antialiasing = 0; // 0 none, 1 FXAA, 2 FXAA extreme
    };

    // The pictures and the window's swapchain on the device, which was opened
    // with the window (vk::Device::Window); false, and why.
    bool open(vk::Device &device, std::string &error);
    ~Presenter();

    // On the renderer's thread, at a swap (render::Options::on_swap): the
    // front buffer made the window's next picture on the GPU. False when no
    // picture was free (the window still shows or reads all three), so this
    // swap is not shown.
    bool take(const render::FrontBuffer &front);

    // The newest picture taken (or the last one again) shown in the window,
    // with the overlay recorded into the command buffer inside the rendering
    // of the swapchain image; true when it is one not shown before, with its
    // swap in *swap.
    bool present(const View &view, const std::function<void(VkCommandBuffer)> &overlay, uint64_t *swap = nullptr);
    // a picture taken and not shown yet
    bool fresh() {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_ >= 0;
    }
    // The next picture taken is also copied back to the CPU (a screenshot
    // of what the window shows); captured() hands it over once the GPU has
    // made it, on any thread, without waiting.
    void capture_next() { capture_ = true; }
    bool capturing() const { return capture_; } // asked, and no picture taken since
    bool captured(render::Image &picture, uint64_t &swap);
    // The window's next present is copied back too, with the overlay and the dialogs drawn over the picture (what a
    // person sees; --window-shots): the window's thread waits for that one present to finish. window_captured()
    // hands the copy over (RGBA) once, on the window's thread.
    void capture_window_next() { window_capture_ = true; }
    bool window_captured(render::Image &out);
    // the window's work on the GPU done (the renderer's goes on)
    void idle();

    // for Dear ImGui's Vulkan backend (the window's queue)
    VkInstance instance() const { return device_->instance(); }
    VkPhysicalDevice physical() const { return device_->physical(); }
    VkDevice device() const { return dev_; }
    VkQueue queue() const { return device_->present_queue(); }
    std::mutex &queue_mutex() { return device_->present_mutex(); }
    uint32_t family() const { return device_->queue_family(); }
    VkFormat format() const { return format_; }
    VkDescriptorPool overlay_pool() const { return overlay_pool_; }
    bool own_queue() const { return device_->present_queue() != device_->queue(); }

    // front buffers taken and not (no picture free); the window thread's
    // waits, in seconds: for its frame in flight, the swapchain's image, the
    // present
    std::atomic<uint64_t> taken{0}, skipped{0};
    double fence_seconds = 0, acquire_seconds = 0, present_seconds = 0;

  private:
    static constexpr uint32_t PICTURES = 3;
    // a picture: the front buffer as the window shows it (8-bit RGBA), its
    // conversion's command buffer and descriptors, and where it stands
    struct Picture {
        vk::Image image;
        VkImageView storage = VK_NULL_HANDLE; // the conversion's output
        VkImageView front = VK_NULL_HANDLE;   // the renderer's image, until the conversion is done
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vk::Buffer ramp;      // the gamma ramps: the table (256 words), then the piecewise linear one (384)
        vk::Buffer copy;      // the picture copied back, when captured
        bool copying = false; // a capture not handed over yet (the picture is kept until it is)
        uint64_t swap = 0;
        uint64_t converted = 0; // converted_'s value once its conversion is done
        uint64_t read = 0;      // shown_'s value once the window's last use of it is done
    };

    bool make_swapchain(const View &view);
    // the effects (present_effects.comp): their pipelines, made on the first present that asks
    bool make_effects();
    struct EffectImage;
    // the picture through the view's effects (the antialiasing at its size, then the output scaling to the
    // destination's): the last pass's image, in VK_IMAGE_LAYOUT_GENERAL, and its size; null when none applies (a
    // blit does it all) or they could not be made
    EffectImage *run_effects(VkCommandBuffer cmd, const View &view, VkImageView picture_view, uint32_t width,
                             uint32_t height, uint32_t dest_width, uint32_t dest_height, uint32_t &out_width,
                             uint32_t &out_height);
    void drop_swapchain();
    bool make_pipeline(std::string &error);
    uint64_t counter(VkSemaphore semaphore) const;

    vk::Device *device_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;

    // the conversion (the renderer's thread)
    VkCommandPool convert_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool convert_pool_sets_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSemaphore converted_ = VK_NULL_HANDLE, shown_ = VK_NULL_HANDLE; // timelines
    uint64_t next_converted_ = 0;                                     // the renderer's thread's
    uint64_t next_shown_ = 0;                                         // the window's thread's
    Picture pictures_[PICTURES];
    std::mutex mutex_; // latest_, showing_ and each picture's read and copying, between the threads
    int latest_ = -1, showing_ = -1;
    std::atomic<bool> capture_{false}, window_capture_{false};
    vk::Buffer window_copy_;
    bool window_ready_ = false;
    uint32_t window_w_ = 0, window_h_ = 0;

    // the effects (the window's thread): a pipeline each (EASU, RCAS, CAS, FXAA, FXAA extreme), the images between
    // passes (kept while their sizes hold), a descriptor set per pass and frame in flight
    enum Effect { EASU, RCAS, CAS, FXAA, FXAA_EXTREME, EFFECTS };
    static constexpr uint32_t EFFECT_PASSES = 8;
    struct EffectImage {
        vk::Image image;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool effects_tried_ = false, effects_ok_ = false, effects_failure_told_ = false;
    VkDescriptorSetLayout effect_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout effect_layout_ = VK_NULL_HANDLE;
    VkPipeline effect_pipelines_[EFFECTS] = {};
    VkSampler linear_ = VK_NULL_HANDLE;
    VkDescriptorPool effect_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet effect_sets_[2][EFFECT_PASSES] = {};
    EffectImage effect_images_[EFFECT_PASSES]; // the passes' outputs, by the pass's place in the chain
    EffectImage *effect_image(uint32_t slot, uint32_t width, uint32_t height);

    // the window (its thread)
    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkDescriptorPool overlay_pool_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    bool vsync_ = true;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> finished_; // one per swapchain image
    struct InFlight {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    } flight_[2];
    uint32_t at_ = 0;
};

} // namespace xf::frontend
