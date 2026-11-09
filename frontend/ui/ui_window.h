// The launcher's window on Vulkan: a vk::Device opened with the SDL3 window,
// its swapchain, and the frames the render interface records (ui_renderer.h)
// shown in it. Two frames in flight. A frame can also be drawn at a size of
// its own without the window (the screenshots at 4K on a 1080p desktop) and
// read back to the CPU.
//
// The launcher has no game to keep time for, so this thread may wait for
// vsync as it likes; nothing else depends on it.

#pragma once

#include "ui_renderer.h"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

namespace xf::ui {

class Window {
  public:
    ~Window();
    // the device with the window's surface, the renderer on it
    bool open(SDL_Window *window, std::string &error, const std::filesystem::path &pipeline_cache = {});
    // The swapchain at that size, made ahead of the first frame (creating it is the slowest step of the start, and
    // can run on another thread while the interface loads; nothing else may use the window until it returns)
    bool prepare_swapchain(uint32_t width, uint32_t height) { return swapchain_ || make_swapchain(width, height); }
    Renderer &renderer() { return renderer_; }

    // One frame: `draw` records the UI (the context's Render()) between the
    // renderer's begin and end. width and height of 0: the window's size in
    // pixels, shown in it (false when there is nothing to show: minimized,
    // or the swapchain made again). Otherwise the frame is drawn at that
    // size and only kept for take_capture().
    bool frame(const std::function<void()> &draw, uint32_t width = 0, uint32_t height = 0);

    // The next frame is also read back (RGBA, opaque); take_capture hands
    // the last one over.
    void capture_next() { capture_ = true; }
    bool take_capture(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);

    // the window's size in pixels
    uint32_t pixel_width() const;
    uint32_t pixel_height() const;
    uint32_t draws() const { return renderer_.draws(); }
    std::string device_name() const { return device_ ? device_->name() : std::string(); }

  private:
    static constexpr uint32_t FRAMES = 2;
    bool make_swapchain(uint32_t width, uint32_t height);
    void drop_swapchain();

    SDL_Window *window_ = nullptr;
    std::unique_ptr<vk::Device> device_;
    Renderer renderer_;
    VkDevice dev_ = VK_NULL_HANDLE;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    struct InFlight {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        uint64_t serial = 0; // the frame it carries, 0 when none
    } flight_[FRAMES];
    uint64_t serial_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> finished_;

    uint64_t completed_ = 0; // the newest frame known to be done
    bool capture_ = false;
    std::vector<uint8_t> capture_rgba_;
    uint32_t capture_w_ = 0, capture_h_ = 0;
};

} // namespace xf::ui
