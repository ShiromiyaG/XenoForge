// A windowless Vulkan 1.3 device for the renderer: one graphics queue, the
// features the translated shaders and the renderer ask for (descriptor
// indexing with sampled images updated after binding, dynamic rendering,
// demote to helper, geometry shaders for rectangle lists, BC textures,
// timeline semaphores), and small helpers for buffers, images and one-off
// command buffers. Opened with a window, it presents to it too (the player
// shows the renderer's front buffers without leaving the GPU).
// XF_VK_VALIDATION=1 turns the validation layer on.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xf::vk {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // host-visible buffers stay mapped
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    uint32_t levels = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

class Device {
public:
    // A device that also shows pictures in a window (the player's): the
    // instance gets the window system's extensions, the surface is made on it,
    // and the device chosen has a graphics queue that presents to it, with
    // VK_KHR_swapchain.
    struct Window {
        std::vector<const char *> instance_extensions;
        std::function<VkSurfaceKHR(VkInstance)> make_surface; // VK_NULL_HANDLE when it fails
    };

    static std::unique_ptr<Device> open(std::string &error, const Window *window = nullptr);
    ~Device();

    VkInstance instance() const { return instance_; }
    VkDevice device() const { return device_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return family_; }
    // the window's surface (opened with one), else VK_NULL_HANDLE
    VkSurfaceKHR surface() const { return surface_; }
    // The queue a window's pictures are submitted and presented on: a second
    // queue of the family where it has one, so a present that waits for the
    // display never holds the renderer's submissions; else the renderer's,
    // shared through queue_mutex (present_mutex is then the same mutex).
    VkQueue present_queue() const { return present_queue_; }
    std::mutex &present_mutex() { return present_queue_ == queue_ ? queue_mutex_ : present_mutex_; }
    const std::string &name() const { return name_; }
    // VK_EXT_shader_stencil_export, enabled when the device has it
    bool stencil_export() const { return stencil_export_; }

    // host-visible and coherent (mapped), or device-local; host_reads: the
    // host reads it back, so cached memory where the device has it
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, bool host_reads = false);
    void destroy(Buffer &buffer);
    Image create_image(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t levels, uint32_t layers,
                       VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags = 0);
    void destroy(Image &image);
    bool format_supported(VkFormat format, VkFormatFeatureFlags features) const;

    // Records with record(), submits, waits.
    bool run(const std::function<void(VkCommandBuffer)> &record, std::string &error);
    // vkQueueSubmit on the queue, which threads share through this (the
    // renderer's thread and a presenter's): Vulkan wants a queue's
    // submissions made one at a time.
    VkResult submit(uint32_t count, const VkSubmitInfo *submits, VkFence fence);
    std::mutex &queue_mutex() { return queue_mutex_; }

private:
    Device() = default;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE, present_queue_ = VK_NULL_HANDLE;
    std::mutex present_mutex_;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    std::string name_;
    bool stencil_export_ = false;
    std::mutex queue_mutex_;
};

} // namespace xf::vk
