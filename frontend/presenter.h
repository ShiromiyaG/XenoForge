// The window's pictures (docs/runtime.md, "The player"): the guest's front
// buffers, as the renderer reads them back at each swap, shown in an SDL3
// window through a Vulkan swapchain, scaled to fit and letterboxed, with
// Dear ImGui's overlay drawn over them. A Vulkan device of its own (the
// renderer's has no window), so nothing here waits for the renderer, and a
// slow present (a display's refresh, a window being moved) waits on this
// thread alone: the guest's threads never see it.

#pragma once

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xf::frontend {

class Presenter {
  public:
    struct View {
        uint32_t width = 0, height = 0; // the window's, in pixels
        bool vsync = true, smooth = true, stretch = false;
    };

    // the instance (with SDL's surface extensions), the window's surface, a
    // device whose queue presents to it; false, and why
    bool open(SDL_Window *window, std::string &error);
    ~Presenter();

    // A front buffer (8-bit RGBA, rows top down), kept until the next one;
    // nullptr shows the last again. The overlay records its draws into the
    // command buffer, inside the rendering of the swapchain image.
    void present(const uint8_t *rgba, uint32_t width, uint32_t height, const View &view,
                 const std::function<void(VkCommandBuffer)> &overlay);
    // waits for the device to finish
    void idle();

    // for Dear ImGui's Vulkan backend
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t family() const { return family_; }
    VkFormat format() const { return format_; }
    VkDescriptorPool overlay_pool() const { return pool_; }

  private:
    bool make_swapchain(const View &view);
    void drop_swapchain();
    bool make_frame_image(uint32_t width, uint32_t height);
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkCommandPool commands_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    bool vsync_ = true;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> finished_; // one per swapchain image

    // two frames in flight, each with its own staging buffer
    struct InFlight {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkDeviceSize staging_size = 0;
        void *mapped = nullptr;
    } flight_[2];
    uint32_t at_ = 0;

    // the front buffer on the GPU
    VkImage frame_ = VK_NULL_HANDLE;
    VkDeviceMemory frame_memory_ = VK_NULL_HANDLE;
    uint32_t frame_width_ = 0, frame_height_ = 0;
    bool frame_ready_ = false;
    VkPhysicalDeviceMemoryProperties memory_{};
};

} // namespace xf::frontend
