// The window's pictures (presenter.h). The swapchain's making and the
// frames in flight follow the 3DS runtime's src/vk/present.cpp; the front
// buffer is uploaded and blitted (scaled, filtered) into the swapchain's
// image, which needs no pipeline of its own, and the overlay drawn over it.

#include "presenter.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace xf::frontend {

namespace {

// one barrier, everything before to everything after (a few per frame)
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
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

std::string failed(const char *what, VkResult r) { return std::string(what) + " failed (" + std::to_string(r) + ")"; }

} // namespace

bool Presenter::open(SDL_Window *window, std::string &error) {
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) return error = std::string("SDL: no Vulkan: ") + SDL_GetError(), false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xenoforge player";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = count;
    ii.ppEnabledExtensionNames = names;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (const char *env = std::getenv("XF_VK_VALIDATION"); env && env[0] == '1') {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &validation;
    }
    if (VkResult r = vkCreateInstance(&ii, nullptr, &instance_); r != VK_SUCCESS)
        return error = failed("vkCreateInstance", r), false;
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_))
        return error = std::string("SDL: no surface: ") + SDL_GetError(), false;

    // a Vulkan 1.3 device with a queue that draws and presents to the
    // window, a discrete one first
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    int best = -1;
    for (VkPhysicalDevice p : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fp(families);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &families, fp.data());
        for (uint32_t f = 0; f < families; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(p, f, surface_, &present);
            if (!(fp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) best = score, physical_ = p, family_ = f;
            break;
        }
    }
    if (!physical_) return error = "no Vulkan 1.3 device that presents to the window", false;
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v13;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &features;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &swapchain;
    if (VkResult r = vkCreateDevice(physical_, &di, nullptr, &device_); r != VK_SUCCESS)
        return error = failed("vkCreateDevice", r), false;
    vkGetDeviceQueue(device_, family_, 0, &queue_);

    // plain 8-bit BGRA or RGBA, the front buffer's values as they are (the
    // guest's gamma already applied), that a blit can write
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fc, formats.data());
    bool found = false;
    for (const VkSurfaceFormatKHR &f : formats) {
        if (f.format != VK_FORMAT_B8G8R8A8_UNORM && f.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_, f.format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;
        format_ = f.format, found = true;
        break;
    }
    if (!found) return error = "the window takes no 8-bit RGBA or BGRA format a blit can write", false;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    vkCreateCommandPool(device_, &pci, nullptr, &commands_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = commands_;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (InFlight &f : flight_) {
        vkAllocateCommandBuffers(device_, &cai, &f.cmd);
        vkCreateFence(device_, &fci, nullptr, &f.fence);
        vkCreateSemaphore(device_, &sci, nullptr, &f.acquired);
    }
    // the overlay's textures (Dear ImGui's font)
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 16;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    vkCreateDescriptorPool(device_, &dpi, nullptr, &pool_);
    return true;
}

Presenter::~Presenter() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        drop_swapchain();
        for (InFlight &f : flight_) {
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroySemaphore(device_, f.acquired, nullptr);
            if (f.staging) vkDestroyBuffer(device_, f.staging, nullptr);
            if (f.staging_memory) vkFreeMemory(device_, f.staging_memory, nullptr);
        }
        if (frame_) vkDestroyImage(device_, frame_, nullptr);
        if (frame_memory_) vkFreeMemory(device_, frame_memory_, nullptr);
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        vkDestroyCommandPool(device_, commands_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void Presenter::idle() {
    if (device_) vkDeviceWaitIdle(device_);
}

uint32_t Presenter::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void Presenter::drop_swapchain() {
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : finished_) vkDestroySemaphore(device_, s, nullptr);
    views_.clear(), finished_.clear(), images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Presenter::make_swapchain(const View &view) {
    idle();
    drop_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::clamp(view.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(view.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false; // minimized
    // with vsync the display's refresh paces this thread; without, the
    // newest picture each refresh (mailbox) or at once (immediate)
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!view.vsync)
        mode = has(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
               : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                    : VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        return false;
    }
    extent_ = extent;
    vsync_ = view.vsync;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        vkCreateImageView(device_, &vi, nullptr, &v);
        views_.push_back(v);
        VkSemaphore s;
        vkCreateSemaphore(device_, &sci, nullptr, &s);
        finished_.push_back(s);
    }
    return true;
}

bool Presenter::make_frame_image(uint32_t width, uint32_t height) {
    idle();
    if (frame_) vkDestroyImage(device_, frame_, nullptr);
    if (frame_memory_) vkFreeMemory(device_, frame_memory_, nullptr);
    frame_ = VK_NULL_HANDLE, frame_memory_ = VK_NULL_HANDLE, frame_ready_ = false;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device_, &info, nullptr, &frame_) != VK_SUCCESS) return frame_ = VK_NULL_HANDLE, false;
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(device_, frame_, &need);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &frame_memory_)) {
        vkDestroyImage(device_, frame_, nullptr);
        frame_ = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(device_, frame_, frame_memory_, 0);
    frame_width_ = width, frame_height_ = height;
    return true;
}

void Presenter::present(const uint8_t *rgba, uint32_t width, uint32_t height, const View &view,
                        const std::function<void(VkCommandBuffer)> &overlay) {
    if (!device_ || !view.width || !view.height) return;
    if (!swapchain_ || view.width != extent_.width || view.height != extent_.height || view.vsync != vsync_)
        if (!make_swapchain(view)) return;
    InFlight &f = flight_[at_];
    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        extent_ = {}; // made again next time
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;

    // the new front buffer into this frame's staging buffer (the fence
    // waited for says the GPU is done with it)
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    bool upload = rgba && width && height;
    if (upload && (width != frame_width_ || height != frame_height_ || !frame_))
        upload = make_frame_image(width, height);
    if (upload && f.staging_size < bytes) {
        if (f.staging) vkDestroyBuffer(device_, f.staging, nullptr), f.staging = VK_NULL_HANDLE;
        if (f.staging_memory) vkFreeMemory(device_, f.staging_memory, nullptr), f.staging_memory = VK_NULL_HANDLE;
        f.staging_size = 0;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(device_, &bi, nullptr, &f.staging);
        VkMemoryRequirements need;
        vkGetBufferMemoryRequirements(device_, f.staging, &need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = memory_type(need.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex != UINT32_MAX &&
            vkAllocateMemory(device_, &alloc, nullptr, &f.staging_memory) == VK_SUCCESS) {
            vkBindBufferMemory(device_, f.staging, f.staging_memory, 0);
            vkMapMemory(device_, f.staging_memory, 0, VK_WHOLE_SIZE, 0, &f.mapped);
            f.staging_size = bytes;
        } else {
            upload = false;
        }
    }
    if (upload) std::memcpy(f.mapped, rgba, size_t(bytes));
    vkResetFences(device_, 1, &f.fence);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    if (upload) {
        barrier(cmd, frame_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, f.staging, frame_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, frame_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        frame_ready_ = true;
    }
    VkImage target = images_[index];
    barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    if (frame_ready_) {
        // the front buffer's shape kept (letterboxed or pillarboxed), or
        // stretched over the window
        int32_t x0 = 0, y0 = 0, x1 = int32_t(extent_.width), y1 = int32_t(extent_.height);
        if (!view.stretch) {
            double scale = std::min(double(extent_.width) / frame_width_, double(extent_.height) / frame_height_);
            int32_t w = int32_t(frame_width_ * scale + 0.5), h = int32_t(frame_height_ * scale + 0.5);
            x0 = (int32_t(extent_.width) - w) / 2, y0 = (int32_t(extent_.height) - h) / 2;
            x1 = x0 + w, y1 = y0 + h;
        }
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(frame_width_), int32_t(frame_height_), 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cmd, frame_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, view.smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    if (overlay) overlay(cmd);
    vkCmdEndRendering(cmd);
    barrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &finished_[index];
    vkQueueSubmit(queue_, 1, &si, f.fence);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &finished_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) extent_ = {};
    at_ ^= 1;
}

} // namespace xf::frontend
