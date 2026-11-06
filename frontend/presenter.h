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

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace xf::frontend {

class Presenter {
  public:
    struct View {
        uint32_t width = 0, height = 0; // the window's, in pixels
        bool vsync = true, smooth = true, stretch = false;
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
    std::atomic<bool> capture_{false};

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
