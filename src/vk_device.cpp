// The renderer's windowless Vulkan 1.3 device.

#include "xenoforge/vk_device.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace xf::vk {

namespace {
std::string failed(const char *what, VkResult r) { return std::string(what) + " failed (" + std::to_string(r) + ")"; }
} // namespace

std::unique_ptr<Device> Device::open(std::string &error) {
    std::unique_ptr<Device> d(new Device());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xenoforge";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    const char *env = std::getenv("XF_VK_VALIDATION");
    if (env && env[0] == '1') {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &validation;
    }
    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &d->instance_); r != VK_SUCCESS) {
        error = failed("vkCreateInstance", r);
        return nullptr;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d->instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d->instance_, &count, devices.data());
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
            if (!(fp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score, d->physical_ = p, d->family_ = f, d->name_ = props.deviceName;
            }
            break;
        }
    }
    if (!d->physical_) {
        error = "no Vulkan 1.3 device with a graphics queue";
        return nullptr;
    }
    vkGetPhysicalDeviceMemoryProperties(d->physical_, &d->memory_);

    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &v13;
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v12;
    features.features.geometryShader = VK_TRUE;
    features.features.depthClamp = VK_TRUE;
    features.features.independentBlend = VK_TRUE;
    features.features.textureCompressionBC = VK_TRUE;
    features.features.fragmentStoresAndAtomics = VK_TRUE;
    features.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = d->family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (VkResult r = vkCreateDevice(d->physical_, &device_info, nullptr, &d->device_); r != VK_SUCCESS) {
        error = failed("vkCreateDevice", r);
        return nullptr;
    }
    vkGetDeviceQueue(d->device_, d->family_, 0, &d->queue_);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = d->family_;
    vkCreateCommandPool(d->device_, &pool, nullptr, &d->pool_);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(d->device_, &fence, nullptr, &d->fence_);
    return d;
}

Device::~Device() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyFence(device_, fence_, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

uint32_t Device::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

Buffer Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    if (vkCreateBuffer(device_, &info, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements need;
    vkGetBufferMemoryRequirements(device_, b.buffer, &need);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                          : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, b.buffer, nullptr);
        return {};
    }
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    if (host_visible) vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}

void Device::destroy(Buffer &b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

Image Device::create_image(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t levels, uint32_t layers,
                           VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    Image im;
    im.format = format, im.extent = extent, im.levels = levels, im.layers = layers, im.samples = samples;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = type;
    info.format = format;
    info.extent = extent;
    info.mipLevels = levels;
    info.arrayLayers = layers;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &info, nullptr, &im.image) != VK_SUCCESS) return {};
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(device_, im.image, &need);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &alloc, nullptr, &im.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, im.image, nullptr);
        return {};
    }
    vkBindImageMemory(device_, im.image, im.memory, 0);
    return im;
}

void Device::destroy(Image &im) {
    if (im.image) vkDestroyImage(device_, im.image, nullptr);
    if (im.memory) vkFreeMemory(device_, im.memory, nullptr);
    im = {};
}

bool Device::format_supported(VkFormat format, VkFormatFeatureFlags features) const {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical_, format, &p);
    return (p.optimalTilingFeatures & features) == features;
}

bool Device::run(const std::function<void(VkCommandBuffer)> &record, std::string &error) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (VkResult r = vkAllocateCommandBuffers(device_, &alloc, &cmd); r != VK_SUCCESS) return error = failed("vkAllocateCommandBuffers", r), false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkResetFences(device_, 1, &fence_);
    VkResult r = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (r == VK_SUCCESS) r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 20'000'000'000ull);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    if (r != VK_SUCCESS) return error = failed("a submission", r), false;
    return true;
}

} // namespace xf::vk
