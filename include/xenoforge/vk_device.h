// A windowless Vulkan 1.3 device for the renderer: one graphics queue, the
// features the translated shaders and the renderer ask for (descriptor
// indexing with sampled images updated after binding, dynamic rendering,
// demote to helper, geometry shaders for rectangle lists, BC textures,
// timeline semaphores), and small helpers for buffers, images and one-off
// command buffers. Opened with a window, it presents to it too (the player
// shows the renderer's front buffers without leaving the GPU).
// XF_VK_VALIDATION=1 turns the validation layer on.

#pragma once

#include <volk.h> // Vulkan without prototypes, its functions loaded at run time (cmake/vulkan.cmake)

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xf::vk {

// Finds the system's Vulkan loader (vulkan-1.dll, libvulkan.so.1) and loads
// the global functions through volk, once per process; false with the reason
// where there is none. Device::open and Compute::open call it first.
bool load_vulkan(std::string &error);
// Loads the instance's and its devices' functions (volkLoadInstance: the
// loader's trampolines, valid for every instance and device of the process).
void load_instance(VkInstance instance);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // host-visible buffers stay mapped
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // its own, or the pool block's it lies in
    VkDeviceSize offset = 0, bytes = 0;     // its range of that memory
    uint32_t block = 0;                     // its pool block's number (1 and up); 0: memory of its own
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    uint32_t levels = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// What the device's memory holds (Device::memory_counts).
struct MemoryCounts {
    uint64_t allocations = 0, allocations_peak = 0; // vkAllocateMemory's live now, and the most at once
    uint64_t allocations_made = 0;                  // vkAllocateMemory calls so far
    uint64_t blocks = 0, block_bytes = 0;           // the image pool's blocks, and the memory they hold
    uint64_t pooled_images = 0, pooled_bytes = 0;   // images in them, and the bytes those take
    uint64_t own_images = 0, own_bytes = 0;         // images with memory of their own
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
    // anisotropic filtering (its largest ratio, 0 without it) and the mirror
    // clamp to edge address mode, enabled when the device has them
    float max_anisotropy() const { return max_anisotropy_; }
    bool mirror_clamp_to_edge() const { return mirror_clamp_to_edge_; }
    // occlusion queries that count samples (otherwise only zero or not)
    bool precise_occlusion() const { return precise_occlusion_; }
    // geometry shaders (not on many mobile GPUs)
    bool geometry_shader() const { return geometry_shader_; }
    // tessellation control and evaluation shaders (the guest's tessellator)
    bool tessellation_shader() const { return tessellation_shader_; }
    // clip and cull distances in the shaders (the guest's user clip planes)
    bool clip_distance() const { return clip_distance_; }
    bool cull_distance() const { return cull_distance_; }
    // lines wider than a pixel, and the widths the device draws
    bool wide_lines() const { return wide_lines_; }
    float min_line_width() const { return wide_lines_ ? line_width_range_[0] : 1.0f; }
    float max_line_width() const { return wide_lines_ ? line_width_range_[1] : 1.0f; }
    // polygons drawn as lines or points (the guest's polygon modes)
    bool fill_mode_non_solid() const { return fill_mode_non_solid_; }
    // samplers with a border color of their own (VK_EXT_custom_border_color
    // without a format), for the textures' YCbCr border colors
    bool custom_border_color() const { return custom_border_color_; }
    // copies between a depth/stencil aspect and a color image of the same
    // texel size (VK_KHR_maintenance8): the stencil of an EDRAM transfer
    // copied from an R8_UINT image where the device has no stencil export
    bool depth_stencil_color_copies() const { return maintenance8_; }
    // storage image descriptors updated after binding (the resolve targets)
    bool storage_image_update_after_bind() const { return storage_image_update_after_bind_; }
    // VK_EXT_extended_dynamic_state3's blend enables, blend equations and
    // color write masks set at the draw (all three, or none of them)
    bool dynamic_blend() const { return dynamic_blend_; }
    // VK_EXT_graphics_pipeline_library with fast linking: pipelines linked
    // from libraries of their four parts in a fraction of a millisecond
    bool pipeline_library() const { return pipeline_library_; }
    // VK_EXT_fragment_shader_interlock (0 none, 1 pixel interlock, 2 pixel
    // and sample interlock), and the sample counts a render pass without
    // attachments takes (framebufferNoAttachmentsSampleCounts)
    uint32_t fragment_shader_interlock() const { return fragment_shader_interlock_; }
    VkSampleCountFlags no_attachments_samples() const { return no_attachments_samples_; }
    // VK_EXT_sample_locations's sample counts (0 without it; enabled only
    // with fragment shader interlock, which uses it)
    VkSampleCountFlags sample_locations() const { return sample_locations_; }

    // host-visible and coherent (mapped), or device-local; host_reads: the
    // host reads it back, so cached memory where the device has it
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, bool host_reads = false);
    void destroy(Buffer &buffer);
    // view_format: a second format views of it read it as (a mutable format
    // with the two listed, as Xenia's textures with a signed view)
    Image create_image(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t levels, uint32_t layers,
                       VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags = 0,
                       VkFormat view_format = VK_FORMAT_UNDEFINED, VkFormat view_format2 = VK_FORMAT_UNDEFINED);
    void destroy(Image &image);
    // The image pool (Xenia's Vulkan backend has VMA place its images in
    // larger blocks of memory): images lie in blocks of device memory shared
    // by many, of BLOCK_BYTES (or an eighth of a smaller heap); one larger
    // than half a block, or whose driver asks for memory of its own, gets its
    // own. XF_VK_DEDICATED_IMAGES=1 gives every image its own, as before.
    static constexpr VkDeviceSize BLOCK_BYTES = VkDeviceSize(64) << 20;
    bool pooled_images() const { return pooled_images_; }
    MemoryCounts memory_counts();
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
    // vkAllocateMemory and vkFreeMemory, counted
    VkResult allocate(const VkMemoryAllocateInfo &info, VkDeviceMemory &out);
    void free_memory(VkDeviceMemory memory);
    // a range of a pool block for an image's requirements (false: none, the
    // image gets memory of its own)
    struct Block;
    bool pool_take(const VkMemoryRequirements &need, uint32_t type, Image &im);
    void pool_give(Image &im);

    bool pooled_images_ = true;
    std::vector<std::unique_ptr<Block>> blocks_; // by number less 1; null once freed
    std::mutex memory_mutex_;                    // the pool and the counts (images come from more than one thread)
    MemoryCounts counts_;

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
    float max_anisotropy_ = 0.0f;
    bool mirror_clamp_to_edge_ = false;
    bool precise_occlusion_ = false;
    bool geometry_shader_ = false;
    bool tessellation_shader_ = false;
    bool clip_distance_ = false, cull_distance_ = false, wide_lines_ = false, fill_mode_non_solid_ = false;
    bool maintenance8_ = false;
    bool custom_border_color_ = false, storage_image_update_after_bind_ = false;
    float line_width_range_[2] = {1.0f, 1.0f};
    bool dynamic_blend_ = false, pipeline_library_ = false;
    uint32_t fragment_shader_interlock_ = 0;
    VkSampleCountFlags no_attachments_samples_ = 0, sample_locations_ = 0;
    std::mutex queue_mutex_;
};

} // namespace xf::vk
