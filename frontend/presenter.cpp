// The window's pictures (presenter.h). The swapchain's making and the frames
// in flight follow the 3DS runtime's src/vk/present.cpp; the picture is
// blitted (scaled, filtered) into the swapchain's image, which needs no
// pipeline of its own, and the overlay drawn over it. The front buffer
// becomes a picture by front_buffer.comp, on the renderer's queue.

#include "presenter.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace xf::frontend {

namespace {

const uint32_t FRONT_BUFFER[] = {
#include "front_buffer.comp.inc"
};

// front_buffer.comp's push constants
struct Push {
    uint32_t width, height, swizzle, components, bits, luta;
};

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

// all memory, everything before to everything after (in submission order:
// the work submitted before and after it on the queue too)
void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

std::string failed(const char *what, VkResult r) { return std::string(what) + " failed (" + std::to_string(r) + ")"; }

VkSemaphore timeline(VkDevice dev) {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &info, nullptr, &s);
    return s;
}

} // namespace

bool Presenter::open(vk::Device &device, std::string &error) {
    device_ = &device;
    dev_ = device.device();
    VkPhysicalDevice physical = device.physical();
    VkSurfaceKHR surface = device.surface();
    if (!surface) return error = "the device was opened without the window", false;

    // plain 8-bit BGRA or RGBA, the picture's values as they are (the
    // guest's gamma already applied), that a blit can write
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fc, formats.data());
    bool found = false;
    for (const VkSurfaceFormatKHR &f : formats) {
        if (f.format != VK_FORMAT_B8G8R8A8_UNORM && f.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
        if (!device.format_supported(f.format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;
        format_ = f.format, found = true;
        break;
    }
    if (!found) return error = "the window takes no 8-bit RGBA or BGRA format a blit can write", false;
    if (!device.format_supported(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        return error = "8-bit RGBA is not a storage image a blit can read here", false;

    converted_ = timeline(dev_), shown_ = timeline(dev_);
    if (!converted_ || !shown_) return error = "no timeline semaphores", false;

    // the window's frames in flight
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = device.queue_family();
    vkCreateCommandPool(dev_, &pci, nullptr, &commands_);
    vkCreateCommandPool(dev_, &pci, nullptr, &convert_pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = commands_;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (InFlight &f : flight_) {
        vkAllocateCommandBuffers(dev_, &cai, &f.cmd);
        vkCreateFence(dev_, &fci, nullptr, &f.fence);
        vkCreateSemaphore(dev_, &sci, nullptr, &f.acquired);
    }
    // the overlay's textures: Dear ImGui's font and the achievements' icons
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 64;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    vkCreateDescriptorPool(dev_, &dpi, nullptr, &overlay_pool_);
    return make_pipeline(error);
}

// front_buffer.comp, and each picture's command buffer, descriptor set and
// gamma ramp
bool Presenter::make_pipeline(std::string &error) {
    VkDescriptorSetLayoutBinding bindings[3] = {};
    const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    for (uint32_t i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 3;
    sli.pBindings = bindings;
    if (VkResult r = vkCreateDescriptorSetLayout(dev_, &sli, nullptr, &set_layout_); r != VK_SUCCESS)
        return error = failed("vkCreateDescriptorSetLayout", r), false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (VkResult r = vkCreatePipelineLayout(dev_, &pli, nullptr, &pipeline_layout_); r != VK_SUCCESS)
        return error = failed("vkCreatePipelineLayout", r), false;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(FRONT_BUFFER);
    smi.pCode = FRONT_BUFFER;
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(dev_, &smi, nullptr, &module); r != VK_SUCCESS)
        return error = failed("vkCreateShaderModule", r), false;
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = pipeline_layout_;
    VkResult made = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline_);
    vkDestroyShaderModule(dev_, module, nullptr);
    if (made != VK_SUCCESS) return error = failed("vkCreateComputePipelines", made), false;

    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_NEAREST;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &smp, nullptr, &sampler_);
    VkDescriptorPoolSize sizes[3] = {{types[0], PICTURES}, {types[1], PICTURES}, {types[2], PICTURES}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = PICTURES;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    vkCreateDescriptorPool(dev_, &dpi, nullptr, &convert_pool_sets_);
    for (Picture &p : pictures_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = convert_pool_;
        cai.commandBufferCount = 1;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = convert_pool_sets_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set_layout_;
        p.ramp = device_->create_buffer((256 + 128 * 3) * sizeof(uint32_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        if (vkAllocateCommandBuffers(dev_, &cai, &p.cmd) != VK_SUCCESS ||
            vkAllocateDescriptorSets(dev_, &dai, &p.set) != VK_SUCCESS || !p.ramp.mapped)
            return error = "the window's pictures could not be made", false;
    }
    return true;
}

Presenter::~Presenter() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_); // (the renderer is gone by then: the player exits without destructors)
    drop_swapchain();
    for (InFlight &f : flight_) {
        vkDestroyFence(dev_, f.fence, nullptr);
        vkDestroySemaphore(dev_, f.acquired, nullptr);
    }
    for (Picture &p : pictures_) {
        if (p.front) vkDestroyImageView(dev_, p.front, nullptr);
        if (p.storage) vkDestroyImageView(dev_, p.storage, nullptr);
        device_->destroy(p.image);
        device_->destroy(p.ramp);
        device_->destroy(p.copy);
    }
    vkDestroySemaphore(dev_, converted_, nullptr);
    vkDestroySemaphore(dev_, shown_, nullptr);
    vkDestroySampler(dev_, sampler_, nullptr);
    vkDestroyDescriptorPool(dev_, convert_pool_sets_, nullptr);
    vkDestroyPipeline(dev_, pipeline_, nullptr);
    vkDestroyPipelineLayout(dev_, pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    vkDestroyDescriptorPool(dev_, overlay_pool_, nullptr);
    vkDestroyCommandPool(dev_, commands_, nullptr);
    vkDestroyCommandPool(dev_, convert_pool_, nullptr);
}

uint64_t Presenter::counter(VkSemaphore semaphore) const {
    uint64_t value = 0;
    vkGetSemaphoreCounterValue(dev_, semaphore, &value);
    return value;
}

// The window's queue idle: its frames and presents done. Where the queue is
// the renderer's too, this waits for the renderer's work as well (a resize,
// a change of vsync, the end: never while playing on).
void Presenter::idle() {
    if (!dev_) return;
    std::lock_guard<std::mutex> lock(queue_mutex());
    vkQueueWaitIdle(queue());
}

// ---- the renderer's thread ----

bool Presenter::take(const render::FrontBuffer &front) {
    // a picture neither shown nor waiting to be, whose last conversion and
    // last use by the window are done: without waiting for either
    const uint64_t converted = counter(converted_), shown = counter(shown_);
    int at = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < int(PICTURES) && at < 0; i++)
            if (i != latest_ && i != showing_ && !pictures_[i].copying && pictures_[i].converted <= converted &&
                pictures_[i].read <= shown)
                at = i;
    }
    if (at < 0) {
        skipped++;
        return false;
    }
    Picture &p = pictures_[at];
    if (p.front) vkDestroyImageView(dev_, p.front, nullptr), p.front = VK_NULL_HANDLE;
    if (p.image.extent.width != front.width || p.image.extent.height != front.height) {
        if (p.storage) vkDestroyImageView(dev_, p.storage, nullptr), p.storage = VK_NULL_HANDLE;
        device_->destroy(p.image);
        p.image =
            device_->create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {front.width, front.height, 1}, 1, 1,
                                  VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!p.image.image) return skipped++, false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = p.image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev_, &vi, nullptr, &p.storage) != VK_SUCCESS) return skipped++, false;
    }
    // the renderer's image as it holds it (the swizzle is the shader's)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = front.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = front.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev_, &vi, nullptr, &p.front) != VK_SUCCESS) return skipped++, false;
    std::memcpy(p.ramp.mapped, front.gamma.data(), sizeof(front.gamma));
    std::memcpy(static_cast<uint8_t *>(p.ramp.mapped) + sizeof(front.gamma), front.pwl.data(), sizeof(front.pwl));

    VkDescriptorImageInfo source{sampler_, p.front, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo target{VK_NULL_HANDLE, p.storage, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo ramp{p.ramp.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = p.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[0].pImageInfo = &source;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, writes[1].pImageInfo = &target;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[2].pBufferInfo = &ramp;
    vkUpdateDescriptorSets(dev_, 3, writes, 0, nullptr);

    // after the swap's work (the barrier's first half takes in what was
    // submitted before on this queue), and before the renderer's next (its
    // second half takes in what comes after)
    VkCommandBuffer cmd = p.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    full_barrier(cmd);
    barrier(cmd, p.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &p.set, 0, nullptr);
    Push push{front.width, front.height, front.swizzle, std::max(front.components, 1u), front.bits, front.luta_control};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (front.width + 7) / 8, (front.height + 7) / 8, 1);
    // a screenshot of the picture: copied back too
    const VkDeviceSize bytes = VkDeviceSize(front.width) * front.height * 4;
    bool copying = capture_.exchange(false);
    if (copying && p.copy.size < bytes) {
        device_->destroy(p.copy);
        p.copy = device_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        copying = p.copy.mapped != nullptr;
    }
    if (copying) {
        full_barrier(cmd);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {front.width, front.height, 1};
        vkCmdCopyImageToBuffer(cmd, p.image.image, VK_IMAGE_LAYOUT_GENERAL, p.copy.buffer, 1, &region);
    }
    full_barrier(cmd);
    vkEndCommandBuffer(cmd);

    const uint64_t value = ++next_converted_;
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &value;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &values;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &converted_;
    if (device_->submit(1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        --next_converted_;
        return skipped++, false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    p.converted = value;
    p.swap = front.swap;
    p.copying = copying;
    latest_ = at; // a picture taken before and not shown is free again
    taken++;
    return true;
}

bool Presenter::captured(render::Image &picture, uint64_t &swap) {
    const uint64_t converted = counter(converted_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (Picture &p : pictures_) {
        if (!p.copying || p.converted > converted) continue;
        picture.width = p.image.extent.width, picture.height = p.image.extent.height;
        const uint8_t *bytes = static_cast<const uint8_t *>(p.copy.mapped);
        picture.rgba.assign(bytes, bytes + size_t(picture.width) * picture.height * 4);
        swap = p.swap;
        p.copying = false;
        return true;
    }
    return false;
}

bool Presenter::window_captured(render::Image &out) {
    if (!window_ready_) return false;
    window_ready_ = false;
    out.width = window_w_, out.height = window_h_;
    const uint8_t *bytes = static_cast<const uint8_t *>(window_copy_.mapped);
    out.rgba.assign(bytes, bytes + size_t(out.width) * out.height * 4);
    if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB)
        for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) std::swap(out.rgba[i], out.rgba[i + 2]);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    return true;
}

// ---- the window's thread ----

void Presenter::drop_swapchain() {
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    for (VkSemaphore s : finished_) vkDestroySemaphore(dev_, s, nullptr);
    views_.clear(), finished_.clear(), images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Presenter::make_swapchain(const View &view) {
    idle();
    drop_swapchain();
    VkPhysicalDevice physical = device_->physical();
    VkSurfaceKHR surface = device_->surface();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::clamp(view.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(view.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false; // minimized
    // with vsync the display's refresh paces this thread; without, the
    // newest picture each refresh (mailbox) or at once (immediate)
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!view.vsync)
        mode = has(VK_PRESENT_MODE_MAILBOX_KHR)     ? VK_PRESENT_MODE_MAILBOX_KHR
               : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                    : VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        return false;
    }
    extent_ = extent;
    vsync_ = view.vsync;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &count, images_.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        views_.push_back(v);
        VkSemaphore s;
        vkCreateSemaphore(dev_, &sci, nullptr, &s);
        finished_.push_back(s);
    }
    return true;
}

bool Presenter::present(const View &view, const std::function<void(VkCommandBuffer)> &overlay, uint64_t *swap) {
    if (!dev_ || !view.width || !view.height) return false;
    if (!swapchain_ || view.width != extent_.width || view.height != extent_.height || view.vsync != vsync_)
        if (!make_swapchain(view)) return false;
    InFlight &f = flight_[at_];
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    const Clock::time_point t1 = Clock::now();
    uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    fence_seconds += std::chrono::duration<double>(t1 - t0).count();
    acquire_seconds += std::chrono::duration<double>(Clock::now() - t1).count();
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        extent_ = {}; // made again next time
        return false;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;

    // the newest picture, or the one shown before
    bool fresh = false;
    int shown = -1;
    uint64_t converted = 0, shown_swap = 0;
    uint32_t width = 0, height = 0;
    VkImage picture = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ >= 0) showing_ = latest_, latest_ = -1, fresh = true;
        shown = showing_;
        if (shown >= 0) {
            const Picture &p = pictures_[shown];
            converted = p.converted, shown_swap = p.swap;
            picture = p.image.image, width = p.image.extent.width, height = p.image.extent.height;
        }
    }
    vkResetFences(dev_, 1, &f.fence);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImage target = images_[index];
    barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    if (picture) {
        // the front buffer's shape kept (letterboxed or pillarboxed), or
        // stretched over the window
        int32_t x0 = 0, y0 = 0, x1 = int32_t(extent_.width), y1 = int32_t(extent_.height);
        if (!view.stretch) {
            double scale = std::min(double(extent_.width) / width, double(extent_.height) / height);
            int32_t w = int32_t(width * scale + 0.5), h = int32_t(height * scale + 0.5);
            x0 = (int32_t(extent_.width) - w) / 2, y0 = (int32_t(extent_.height) - h) / 2;
            x1 = x0 + w, y1 = y0 + h;
        }
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cmd, picture, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       view.smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
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
    // a screenshot of the window itself: the finished image copied back (the overlay on it)
    bool grab = window_capture_.exchange(false);
    if (grab) {
        const VkDeviceSize need = VkDeviceSize(extent_.width) * extent_.height * 4;
        if (window_copy_.size < need) {
            device_->destroy(window_copy_);
            window_copy_ = device_->create_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        }
        grab = window_copy_.mapped != nullptr;
    }
    if (grab) {
        barrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, window_copy_.buffer, 1, &region);
        barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        barrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cmd);

    // after the swapchain's image is free and the picture's conversion done;
    // shown_ says when this use of the picture is done
    const uint64_t done = ++next_shown_;
    const VkSemaphore waits[2] = {f.acquired, converted_};
    const uint64_t wait_values[2] = {0, converted};
    const VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const VkSemaphore signals[2] = {finished_[index], shown_};
    const uint64_t signal_values[2] = {0, done};
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.waitSemaphoreValueCount = 2;
    values.pWaitSemaphoreValues = wait_values;
    values.signalSemaphoreValueCount = 2;
    values.pSignalSemaphoreValues = signal_values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &values;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = signals;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &finished_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult r;
    const Clock::time_point t2 = Clock::now();
    {
        std::lock_guard<std::mutex> lock(queue_mutex());
        if (vkQueueSubmit(queue(), 1, &si, f.fence) != VK_SUCCESS) {
            // nothing shown: the acquire's semaphore waited on and the fence
            // signaled all the same, so the next use of this slot goes on
            --next_shown_;
            VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            empty.waitSemaphoreCount = 1;
            empty.pWaitSemaphores = &f.acquired;
            empty.pWaitDstStageMask = stages;
            vkQueueSubmit(queue(), 1, &empty, f.fence);
            return false;
        }
        r = vkQueuePresentKHR(queue(), &pi);
    }
    present_seconds += std::chrono::duration<double>(Clock::now() - t2).count();
    if (grab) {
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX); // (a check's screenshot: this one present is waited for)
        window_w_ = extent_.width, window_h_ = extent_.height, window_ready_ = true;
    }
    if (shown >= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        pictures_[shown].read = done;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) extent_ = {};
    at_ ^= 1;
    if (fresh && swap) *swap = shown_swap;
    return fresh;
}

} // namespace xf::frontend
