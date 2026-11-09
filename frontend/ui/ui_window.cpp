// The launcher's window on Vulkan (ui_window.h).

#include "ui_window.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xf::ui {

namespace {

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

Window::~Window() {
    if (!device_) return;
    vkDeviceWaitIdle(dev_);
    drop_swapchain();
    for (InFlight &f : flight_) {
        if (f.fence) vkDestroyFence(dev_, f.fence, nullptr);
        if (f.acquired) vkDestroySemaphore(dev_, f.acquired, nullptr);
    }
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
}

uint32_t Window::pixel_width() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return uint32_t(std::max(w, 0));
}

uint32_t Window::pixel_height() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return uint32_t(std::max(h, 0));
}

bool Window::open(SDL_Window *window, std::string &error, const std::filesystem::path &pipeline_cache) {
    window_ = window;
    vk::Device::Window surface;
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) return error = std::string("SDL: no Vulkan: ") + SDL_GetError(), false;
    surface.instance_extensions.assign(names, names + count);
    surface.make_surface = [window](VkInstance instance) {
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &s))
            std::fprintf(stderr, "SDL: no surface: %s\n", SDL_GetError());
        return s;
    };
    device_ = vk::Device::open(error, &surface);
    if (!device_) return false;
    dev_ = device_->device();
    renderer_.set_pipeline_cache_file(pipeline_cache);
    if (!renderer_.open(*device_, error)) return false;

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = device_->queue_family();
    vkCreateCommandPool(dev_, &pi, nullptr, &pool_);
    for (InFlight &f : flight_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev_, &ai, &f.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev_, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev_, &si, nullptr, &f.acquired);
    }
    return true;
}

void Window::drop_swapchain() {
    if (!swapchain_) return;
    for (VkSemaphore s : finished_) vkDestroySemaphore(dev_, s, nullptr);
    finished_.clear();
    images_.clear();
    vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Window::make_swapchain(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(dev_);
    drop_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), device_->surface(), &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = device_->surface();
    ci.minImageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &swapchain_) != VK_SUCCESS) return swapchain_ = VK_NULL_HANDLE, false;
    extent_ = extent;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; i++) {
        VkSemaphore s;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        finished_.push_back(s);
    }
    return true;
}

bool Window::frame(const std::function<void()> &draw, uint32_t width, uint32_t height) {
    const bool offscreen = width && height;
    const uint32_t pw = pixel_width(), ph = pixel_height();
    if (!offscreen) {
        if (!pw || !ph) return false;
        width = pw, height = ph;
    }
    const uint32_t slot = uint32_t(serial_ % FRAMES);
    InFlight &f = flight_[slot];
    vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    completed_ = std::max(completed_, f.serial);

    uint32_t index = 0;
    if (!offscreen) {
        if (!swapchain_ || extent_.width != pw || extent_.height != ph)
            if (!make_swapchain(pw, ph)) return false;
        const VkResult acquired = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            extent_ = {};
            return false;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;
    }
    vkResetFences(dev_, 1, &f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &begin);
    f.serial = ++serial_;
    renderer_.begin_frame(f.cmd, slot, width, height, f.serial, completed_);
    draw();
    renderer_.end_frame();

    // the picture, read back when asked
    vk::Buffer readback;
    const bool capturing = capture_;
    if (capturing) {
        capture_ = false;
        readback = device_->create_buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(f.cmd, renderer_.output(), VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        m.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(f.cmd, &dep);
    }
    if (!offscreen) {
        const VkImage target = images_[index];
        image_barrier(f.cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
        blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
        vkCmdBlitImage(f.cmd, renderer_.output(), VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_NEAREST);
        image_barrier(f.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(f.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (!offscreen) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &finished_[index];
    }
    VkResult presented = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> lock(device_->present_mutex());
        if (vkQueueSubmit(device_->present_queue(), 1, &si, f.fence) != VK_SUCCESS) return false;
        if (!offscreen) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &finished_[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &index;
            presented = vkQueuePresentKHR(device_->present_queue(), &pi);
        }
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) extent_ = {};
    if (capturing) {
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        capture_rgba_.assign(static_cast<uint8_t *>(readback.mapped),
                             static_cast<uint8_t *>(readback.mapped) + size_t(width) * height * 4);
        for (size_t i = 3; i < capture_rgba_.size(); i += 4) capture_rgba_[i] = 255;
        capture_w_ = width, capture_h_ = height;
        device_->destroy(readback);
    }
    return true;
}

bool Window::take_capture(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) {
    if (capture_rgba_.empty()) return false;
    rgba = std::move(capture_rgba_);
    capture_rgba_.clear();
    width = capture_w_, height = capture_h_;
    return true;
}

} // namespace xf::ui
