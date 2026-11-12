// A windowless Vulkan 1.3 device that runs one compute dispatch at a time
// over host-visible storage buffers. Slow and simple on purpose: it checks
// shaders, it does not draw frames.

#include "xenoforge/vk_compute.h"
#include "xenoforge/vk_device.h"

#include <volk.h> // Vulkan without prototypes, its functions loaded at run time (cmake/vulkan.cmake)

#include <algorithm>
#include <cstring>

namespace xf::vk {

struct Compute::State {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
};

namespace {

std::string failed(const char *what, VkResult result) { return std::string(what) + " failed (" + std::to_string(result) + ")"; }

// Everything one run makes, destroyed in reverse when it ends.
struct Run {
    VkDevice device;
    std::vector<VkBuffer> buffers;
    std::vector<VkDeviceMemory> memories;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Run() {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, commands, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        for (VkBuffer b : buffers) vkDestroyBuffer(device, b, nullptr);
        for (VkDeviceMemory m : memories) vkFreeMemory(device, m, nullptr);
    }
};

} // namespace

std::unique_ptr<Compute> Compute::open(std::string &error) {
    if (!load_vulkan(error)) return nullptr;
    std::unique_ptr<Compute> c(new Compute());
    c->s_ = std::make_unique<State>();
    State &s = *c->s_;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xenoforge";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &s.instance); r != VK_SUCCESS) {
        error = failed("vkCreateInstance", r);
        return nullptr;
    }
    load_instance(s.instance);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(s.instance, &count, devices.data());
    int best = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &families, props.data());
        for (uint32_t f = 0; f < families; f++) {
            if (!(props[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score;
                s.physical = d;
                s.family = f;
                c->device_name_ = p.deviceName;
            }
            break;
        }
    }
    if (!s.physical) {
        error = "no Vulkan 1.3 device with a compute queue";
        return nullptr;
    }
    vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = s.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (VkResult r = vkCreateDevice(s.physical, &device_info, nullptr, &s.device); r != VK_SUCCESS) {
        error = failed("vkCreateDevice", r);
        return nullptr;
    }
    device_created(s.device);
    vkGetDeviceQueue(s.device, s.family, 0, &s.queue);
    return c;
}

Compute::~Compute() {
    if (!s_) return;
    if (s_->device) vkDestroyDevice(s_->device, nullptr), device_destroyed(s_->device);
    if (s_->instance) vkDestroyInstance(s_->instance, nullptr);
}

bool Compute::run(std::span<const uint32_t> spirv, std::span<std::vector<uint8_t> *const> buffers, uint32_t groups_x,
                  std::string &error) {
    State &s = *s_;
    Run run{s.device};
    VkResult r;

    // the buffers, host visible and coherent, filled with the caller's bytes
    for (std::vector<uint8_t> *data : buffers) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = std::max<size_t>(data->size(), 4);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer buffer;
        if ((r = vkCreateBuffer(s.device, &info, nullptr, &buffer)) != VK_SUCCESS) return error = failed("vkCreateBuffer", r), false;
        run.buffers.push_back(buffer);
        VkMemoryRequirements need;
        vkGetBufferMemoryRequirements(s.device, buffer, &need);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < s.memory.memoryTypeCount; i++)
            if ((need.memoryTypeBits & (1u << i)) && (s.memory.memoryTypes[i].propertyFlags & want) == want) {
                type = i;
                break;
            }
        if (type == UINT32_MAX) return error = "no host-visible memory for a buffer", false;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = need.size;
        alloc.memoryTypeIndex = type;
        VkDeviceMemory memory;
        if ((r = vkAllocateMemory(s.device, &alloc, nullptr, &memory)) != VK_SUCCESS) return error = failed("vkAllocateMemory", r), false;
        run.memories.push_back(memory);
        vkBindBufferMemory(s.device, buffer, memory, 0);
        void *mapped;
        vkMapMemory(s.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        std::memset(mapped, 0, info.size);
        std::memcpy(mapped, data->data(), data->size());
        vkUnmapMemory(s.device, memory);
    }

    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = spirv.size() * 4;
    module_info.pCode = spirv.data();
    if ((r = vkCreateShaderModule(s.device, &module_info, nullptr, &run.module)) != VK_SUCCESS)
        return error = failed("vkCreateShaderModule", r), false;

    std::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
    for (uint32_t i = 0; i < bindings.size(); i++)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = uint32_t(bindings.size());
    set_info.pBindings = bindings.data();
    if ((r = vkCreateDescriptorSetLayout(s.device, &set_info, nullptr, &run.set_layout)) != VK_SUCCESS)
        return error = failed("vkCreateDescriptorSetLayout", r), false;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &run.set_layout;
    if ((r = vkCreatePipelineLayout(s.device, &layout_info, nullptr, &run.layout)) != VK_SUCCESS)
        return error = failed("vkCreatePipelineLayout", r), false;

    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = run.module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = run.layout;
    if ((r = vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &run.pipeline)) != VK_SUCCESS)
        return error = failed("vkCreateComputePipelines", r), false;

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(std::max<size_t>(buffers.size(), 1))};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if ((r = vkCreateDescriptorPool(s.device, &pool_info, nullptr, &run.pool)) != VK_SUCCESS)
        return error = failed("vkCreateDescriptorPool", r), false;
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = run.pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &run.set_layout;
    VkDescriptorSet set;
    if ((r = vkAllocateDescriptorSets(s.device, &set_alloc, &set)) != VK_SUCCESS)
        return error = failed("vkAllocateDescriptorSets", r), false;
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (uint32_t i = 0; i < buffers.size(); i++) {
        infos[i] = {run.buffers[i], 0, VK_WHOLE_SIZE};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(s.device, uint32_t(writes.size()), writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo pool_cmd{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_cmd.queueFamilyIndex = s.family;
    if ((r = vkCreateCommandPool(s.device, &pool_cmd, nullptr, &run.commands)) != VK_SUCCESS)
        return error = failed("vkCreateCommandPool", r), false;
    VkCommandBufferAllocateInfo cmd_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_alloc.commandPool = run.commands;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(s.device, &cmd_alloc, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, run.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, run.layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, groups_x, 1, 1);
    // the shader's writes made visible to the host
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(s.device, &fence_info, nullptr, &run.fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if ((r = vkQueueSubmit(s.queue, 1, &submit, run.fence)) != VK_SUCCESS) return error = failed("vkQueueSubmit", r), false;
    if ((r = vkWaitForFences(s.device, 1, &run.fence, VK_TRUE, 10'000'000'000ull)) != VK_SUCCESS)
        return error = failed("vkWaitForFences", r), false;

    for (size_t i = 0; i < buffers.size(); i++) {
        void *mapped;
        vkMapMemory(s.device, run.memories[i], 0, VK_WHOLE_SIZE, 0, &mapped);
        std::memcpy(buffers[i]->data(), mapped, buffers[i]->size());
        vkUnmapMemory(s.device, run.memories[i]);
    }
    return true;
}

} // namespace xf::vk
