// The renderer's Vulkan 1.3 device, windowless or presenting to a window (vk_device.h).

#include "xenoforge/vk_device.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace xf::vk {

namespace {
std::string failed(const char *what, VkResult r) { return std::string(what) + " failed (" + std::to_string(r) + ")"; }
} // namespace

bool load_vulkan(std::string &error) {
    static std::once_flag once;
    static VkResult result = VK_SUCCESS;
    std::call_once(once, [] { result = volkInitialize(); });
    if (result != VK_SUCCESS) {
        error = "no Vulkan loader on this system (vulkan-1.dll / libvulkan.so.1): install a GPU driver with Vulkan";
        return false;
    }
    return true;
}

void load_instance(VkInstance instance) {
    static std::mutex m;
    std::lock_guard lock(m);
    volkLoadInstance(instance);
}

// A block of the image pool: device memory of one type, its free ranges by
// offset (merged with their neighbors as they come back) and by size (for
// the best fit).
struct Device::Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize size = 0, used = 0;
    uint64_t images = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;         // offset: size
    std::multimap<VkDeviceSize, VkDeviceSize> by_size; // size: offset

    void add_free(VkDeviceSize offset, VkDeviceSize bytes) {
        if (!bytes) return;
        // merged with the free ranges just before and just after it
        auto next = free.lower_bound(offset);
        if (next != free.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                offset = previous->first, bytes += previous->second;
                drop_size(previous->second, previous->first);
                free.erase(previous);
            }
        }
        if (next != free.end() && offset + bytes == next->first) {
            bytes += next->second;
            drop_size(next->second, next->first);
            free.erase(next);
        }
        free[offset] = bytes;
        by_size.insert({bytes, offset});
    }
    void drop_size(VkDeviceSize bytes, VkDeviceSize offset) {
        auto [from, to] = by_size.equal_range(bytes);
        for (auto it = from; it != to; ++it)
            if (it->second == offset) {
                by_size.erase(it);
                return;
            }
    }
    // the smallest free range that holds `bytes` at `alignment`: its
    // offset and size, and where the image would start; false if none
    bool fit(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &range, VkDeviceSize &range_bytes,
             VkDeviceSize &at) const {
        for (auto it = by_size.lower_bound(bytes); it != by_size.end(); ++it) {
            VkDeviceSize aligned = (it->second + alignment - 1) / alignment * alignment;
            if (aligned + bytes <= it->second + it->first) {
                range = it->second, range_bytes = it->first, at = aligned;
                return true;
            }
        }
        return false;
    }
};

std::unique_ptr<Device> Device::open(std::string &error, const Window *window) {
    if (!load_vulkan(error)) return nullptr;
    std::unique_ptr<Device> d(new Device());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xenoforge";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    if (window) {
        instance_info.enabledExtensionCount = uint32_t(window->instance_extensions.size());
        instance_info.ppEnabledExtensionNames = window->instance_extensions.data();
    }
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
    load_instance(d->instance_);
    if (window) {
        d->surface_ = window->make_surface ? window->make_surface(d->instance_) : VK_NULL_HANDLE;
        if (!d->surface_) {
            error = "no surface for the window";
            return nullptr;
        }
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(d->instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(d->instance_, &count, devices.data());
    int best = -1;
    uint32_t queues = 1; // in the family chosen
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
            if (d->surface_) {
                VkBool32 presents = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(p, f, d->surface_, &presents);
                if (!presents) continue;
            }
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score, d->physical_ = p, d->family_ = f, d->name_ = props.deviceName;
                queues = fp[f].queueCount;
            }
            break;
        }
    }
    if (!d->physical_) {
        error = d->surface_ ? "no Vulkan 1.3 device with a graphics queue that presents to the window"
                            : "no Vulkan 1.3 device with a graphics queue";
        return nullptr;
    }
    vkGetPhysicalDeviceMemoryProperties(d->physical_, &d->memory_);

    // the renderer's bindless textures: sampled images and samplers written
    // into a bound set while submissions that do not use them run
    VkPhysicalDeviceVulkan13Features has13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features has12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    has12.pNext = &has13;
    VkPhysicalDeviceFeatures2 has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    has.pNext = &has12;
    vkGetPhysicalDeviceFeatures2(d->physical_, &has);
    if (!has12.descriptorBindingSampledImageUpdateAfterBind || !has12.descriptorBindingUpdateUnusedWhilePending ||
        !has12.descriptorBindingPartiallyBound) {
        error = std::string(d->name_) + " cannot update sampled image descriptors after binding (descriptor indexing)";
        return nullptr;
    }
    if (!has12.timelineSemaphore) {
        error = std::string(d->name_) + " has no timeline semaphores";
        return nullptr;
    }

    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;
    // pipelines asked for from the pipeline cache alone
    // (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT): what the
    // renderer's thread expects to find made is looked up there, and left to
    // the workers when the driver would have to compile it. Required by
    // Vulkan 1.3, queried all the same; XF_VK_NO_CACHE_CONTROL=1 turns it off.
    {
        const char *v = std::getenv("XF_VK_NO_CACHE_CONTROL");
        if (has13.pipelineCreationCacheControl && !(v && v[0] && v[0] != '0'))
            v13.pipelineCreationCacheControl = d->cache_control_ = true;
    }
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &v13;
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v12;
    // optional: storage images updated after binding, for the textures a
    // resolve writes as well as memory (the renderer's resolve targets;
    // XF_VK_NO_RESOLVE_TARGETS=1 turns them off)
    const char *no_targets = std::getenv("XF_VK_NO_RESOLVE_TARGETS");
    if (has12.descriptorBindingStorageImageUpdateAfterBind && has.features.shaderStorageImageArrayDynamicIndexing &&
        !(no_targets && no_targets[0] == '1')) {
        v12.descriptorBindingStorageImageUpdateAfterBind = d->storage_image_update_after_bind_ = true;
        features.features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    }
    // optional: geometry shaders (rectangle and point lists; without them
    // those draws are skipped and counted). XF_VK_NO_GEOMETRY_SHADER=1 turns
    // them off where the device has them, to check the path without.
    const char *no_gs = std::getenv("XF_VK_NO_GEOMETRY_SHADER");
    if (has.features.geometryShader && !(no_gs && no_gs[0] && no_gs[0] != '0'))
        features.features.geometryShader = d->geometry_shader_ = true;
    // optional: tessellation shaders (the guest's tessellated draws; without
    // them those are skipped and counted); XF_VK_NO_TESSELLATION=1 turns them
    // off the same way
    const char *no_tess = std::getenv("XF_VK_NO_TESSELLATION");
    if (has.features.tessellationShader && !(no_tess && no_tess[0] && no_tess[0] != '0'))
        features.features.tessellationShader = d->tessellation_shader_ = true;
    // optional: clip and cull distances (the guest's user clip planes;
    // without them those draws are skipped and counted), and wide lines
    // (PA_SU_LINE_CNTL; without them lines are drawn one pixel wide, counted);
    // XF_VK_NO_CLIP_DISTANCE=1 and XF_VK_NO_WIDE_LINES=1 turn them off
    auto turned_off = [](const char *name) {
        const char *v = std::getenv(name);
        return v && v[0] && v[0] != '0';
    };
    if (!turned_off("XF_VK_NO_CLIP_DISTANCE")) {
        if (has.features.shaderClipDistance) features.features.shaderClipDistance = d->clip_distance_ = true;
        if (has.features.shaderCullDistance) features.features.shaderCullDistance = d->cull_distance_ = true;
    }
    if (has.features.wideLines && !turned_off("XF_VK_NO_WIDE_LINES")) {
        features.features.wideLines = d->wide_lines_ = true;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d->physical_, &props);
        d->line_width_range_[0] = props.limits.lineWidthRange[0];
        d->line_width_range_[1] = props.limits.lineWidthRange[1];
    }
    // polygons as lines or points (PA_SU_SC_MODE_CNTL's polygon modes;
    // without it they are filled, counted); XF_VK_NO_FILL_MODES=1 turns it off
    if (has.features.fillModeNonSolid && !turned_off("XF_VK_NO_FILL_MODES"))
        features.features.fillModeNonSolid = d->fill_mode_non_solid_ = true;
    features.features.depthClamp = VK_TRUE;
    features.features.sampleRateShading = VK_TRUE;
    features.features.independentBlend = VK_TRUE;
    features.features.textureCompressionBC = VK_TRUE;
    features.features.fragmentStoresAndAtomics = VK_TRUE;
    features.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    // optional: the samplers' anisotropy and mirror clamp to edge
    if (has.features.samplerAnisotropy) {
        features.features.samplerAnisotropy = VK_TRUE;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d->physical_, &props);
        d->max_anisotropy_ = props.limits.maxSamplerAnisotropy;
    }
    if (has12.samplerMirrorClampToEdge) v12.samplerMirrorClampToEdge = d->mirror_clamp_to_edge_ = true;
    // optional: occlusion queries counting samples (the guest's are counts)
    if (has.features.occlusionQueryPrecise) features.features.occlusionQueryPrecise = d->precise_occlusion_ = true;
    // images in the pool's blocks, or each with memory of its own
    d->pooled_images_ = !turned_off("XF_VK_DEDICATED_IMAGES");
    // the renderer's queue, and with a window a second one to present on
    // where the family has it
    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = d->family_;
    queue_info.queueCount = d->surface_ && queues >= 2 ? 2 : 1;
    queue_info.pQueuePriorities = priorities;
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(d->physical_, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(d->physical_, nullptr, &extension_count, extensions.data());
    std::vector<const char *> enabled;
    auto has_extension = [&](const char *name) {
        for (const VkExtensionProperties &e : extensions)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    d->stencil_export_ = has_extension(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    if (d->stencil_export_) enabled.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    // optional, beyond Vulkan 1.3 (docs/renderer.md, "Pipelines while they
    // are made"): extended dynamic state 3's
    // blend enables, equations and write masks (XF_VK_NO_EDS3=1 turns them
    // off), and graphics pipeline libraries with fast linking (XF_VK_NO_GPL=1)
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    {
        const bool has_eds3 =
            has_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && !turned_off("XF_VK_NO_EDS3");
        const bool has_gpl = has_extension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                             has_extension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && !turned_off("XF_VK_NO_GPL");
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT has_eds3_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT has_gpl_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        has_eds3_features.pNext = &has_gpl_features;
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &has_eds3_features;
        vkGetPhysicalDeviceFeatures2(d->physical_, &query);
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &gpl_props;
        vkGetPhysicalDeviceProperties2(d->physical_, &props);
        void **tail = &v13.pNext;
        if (has_eds3 && has_eds3_features.extendedDynamicState3ColorBlendEnable &&
            has_eds3_features.extendedDynamicState3ColorBlendEquation &&
            has_eds3_features.extendedDynamicState3ColorWriteMask) {
            eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
            eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
            eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
            *tail = &eds3, tail = &eds3.pNext;
            enabled.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            d->dynamic_blend_ = true;
        }
        // Fragment shader interlock (VK_EXT_fragment_shader_interlock), for
        // the renderer's exact EDRAM (render::Options::render_target_path):
        // pixel interlock at least, sample interlock where the device has it
        // too (Xenia's choice); XF_VK_NO_FSI=1 turns it off. The sample
        // counts a render pass without attachments takes decide whether the
        // guest's 2x is rasterized as 2x or as 4x.
        if (has_extension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) && !turned_off("XF_VK_NO_FSI")) {
            VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT has_fsi{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 query_fsi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query_fsi.pNext = &has_fsi;
            vkGetPhysicalDeviceFeatures2(d->physical_, &query_fsi);
            if (has_fsi.fragmentShaderPixelInterlock) {
                fsi.fragmentShaderPixelInterlock = VK_TRUE;
                fsi.fragmentShaderSampleInterlock = has_fsi.fragmentShaderSampleInterlock;
                *tail = &fsi, tail = &fsi.pNext;
                enabled.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
                d->fragment_shader_interlock_ = has_fsi.fragmentShaderSampleInterlock ? 2 : 1;
                d->no_attachments_samples_ = props.properties.limits.framebufferNoAttachmentsSampleCounts;
            }
        }
        // VK_EXT_sample_locations: the interlock path rasterizes at the
        // Xenos's sample positions, as Xenia's interlock paths always do where
        // the host can (XF_VK_NO_SAMPLE_LOCATIONS=1 turns it off)
        if (d->fragment_shader_interlock_ && has_extension(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) &&
            !turned_off("XF_VK_NO_SAMPLE_LOCATIONS")) {
            VkPhysicalDeviceSampleLocationsPropertiesEXT locations{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 query_locations{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            query_locations.pNext = &locations;
            vkGetPhysicalDeviceProperties2(d->physical_, &query_locations);
            d->sample_locations_ = locations.sampleLocationSampleCounts;
            if (d->sample_locations_) enabled.push_back(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
        }
        // VK_EXT_custom_border_color without a format: the textures' two
        // YCbCr border colors, as Xenia's Vulkan texture cache takes them
        // (XF_VK_NO_CUSTOM_BORDER=1 turns it off: transparent black, counted)
        if (has_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) && !turned_off("XF_VK_NO_CUSTOM_BORDER")) {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT has_border{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 query_border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query_border.pNext = &has_border;
            vkGetPhysicalDeviceFeatures2(d->physical_, &query_border);
            if (has_border.customBorderColors && has_border.customBorderColorWithoutFormat) {
                custom_border.customBorderColors = VK_TRUE;
                custom_border.customBorderColorWithoutFormat = VK_TRUE;
                *tail = &custom_border, tail = &custom_border.pNext;
                enabled.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
                d->custom_border_color_ = true;
            }
        }
        // VK_KHR_maintenance8: depth/stencil aspects copied to and from color
        // images, so that an EDRAM transfer into a depth target without stencil
        // export writes its stencil with one copy rather than a pass a bit
        // (beyond Xenia; XF_VK_NO_MAINTENANCE8=1 turns it off: a pass a bit)
        if (has_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) && !turned_off("XF_VK_NO_MAINTENANCE8")) {
            VkPhysicalDeviceMaintenance8FeaturesKHR has_m8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 query_m8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query_m8.pNext = &has_m8;
            vkGetPhysicalDeviceFeatures2(d->physical_, &query_m8);
            if (has_m8.maintenance8) {
                maintenance8.maintenance8 = VK_TRUE;
                *tail = &maintenance8, tail = &maintenance8.pNext;
                enabled.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
                d->maintenance8_ = true;
            }
        }
        // (without fast linking a linked pipeline may cost as much as a whole
        // one, so the libraries would gain nothing)
        if (has_gpl && has_gpl_features.graphicsPipelineLibrary && gpl_props.graphicsPipelineLibraryFastLinking) {
            gpl.graphicsPipelineLibrary = VK_TRUE;
            *tail = &gpl, tail = &gpl.pNext;
            enabled.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
            enabled.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
            d->pipeline_library_ = true;
        }
    }
    if (d->surface_) enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.enabledExtensionCount = uint32_t(enabled.size());
    device_info.ppEnabledExtensionNames = enabled.data();
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (VkResult r = vkCreateDevice(d->physical_, &device_info, nullptr, &d->device_); r != VK_SUCCESS) {
        error = failed("vkCreateDevice", r);
        return nullptr;
    }
    vkGetDeviceQueue(d->device_, d->family_, 0, &d->queue_);
    d->present_queue_ = d->queue_;
    if (queue_info.queueCount == 2) vkGetDeviceQueue(d->device_, d->family_, 1, &d->present_queue_);
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
        for (auto &block : blocks_)
            if (block) vkFreeMemory(device_, block->memory, nullptr);
        blocks_.clear();
        vkDestroyFence(device_, fence_, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

uint32_t Device::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

Buffer Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, bool host_reads) {
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
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    alloc.memoryTypeIndex =
        memory_type(need.memoryTypeBits, host_visible ? visible : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // what the host reads back: cached memory where there is some (the first
    // coherent type is often write-combined, slow to read)
    if (host_visible && host_reads) {
        uint32_t cached = memory_type(need.memoryTypeBits, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (cached != UINT32_MAX) alloc.memoryTypeIndex = cached;
    }
    if (alloc.memoryTypeIndex == UINT32_MAX || allocate(alloc, b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, b.buffer, nullptr);
        return {};
    }
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    if (host_visible) vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}

void Device::destroy(Buffer &b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) free_memory(b.memory);
    b = {};
}

Image Device::create_image(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t levels, uint32_t layers,
                           VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags,
                           VkFormat view_format, VkFormat view_format2) {
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
    // read through views of other formats too (one or two): mutable, the
    // formats listed (Vulkan 1.2's image format list, which lets a driver
    // keep the image's compression where it can), as Xenia's Vulkan textures
    VkFormat formats[3] = {format};
    uint32_t format_count = 1;
    for (VkFormat f : {view_format, view_format2})
        if (f != VK_FORMAT_UNDEFINED && f != format) formats[format_count++] = f;
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (format_count > 1) {
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        list.viewFormatCount = format_count;
        list.pViewFormats = formats;
        info.pNext = &list;
    }
    if (vkCreateImage(device_, &info, nullptr, &im.image) != VK_SUCCESS) return {};
    // its needs, and whether the driver wants it in memory of its own
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 need2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    need2.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 which{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    which.image = im.image;
    vkGetImageMemoryRequirements2(device_, &which, &need2);
    const VkMemoryRequirements &need = need2.memoryRequirements;
    const uint32_t type_index = memory_type(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index == UINT32_MAX) {
        vkDestroyImage(device_, im.image, nullptr);
        return {};
    }
    const bool wants_own = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation;
    if (pooled_images_ && !wants_own && pool_take(need, type_index, im)) {
        if (vkBindImageMemory(device_, im.image, im.memory, im.offset) == VK_SUCCESS) return im;
        pool_give(im);
        vkDestroyImage(device_, im.image, nullptr);
        return {};
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = need.size;
    alloc.memoryTypeIndex = type_index;
    VkMemoryDedicatedAllocateInfo dedicate{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    if (wants_own) {
        dedicate.image = im.image;
        alloc.pNext = &dedicate;
    }
    if (allocate(alloc, im.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, im.image, nullptr);
        return {};
    }
    im.offset = 0, im.bytes = need.size, im.block = 0;
    {
        std::lock_guard lock(memory_mutex_);
        counts_.own_images++, counts_.own_bytes += need.size;
    }
    vkBindImageMemory(device_, im.image, im.memory, 0);
    return im;
}

void Device::destroy(Image &im) {
    if (im.image) vkDestroyImage(device_, im.image, nullptr);
    if (im.block) {
        pool_give(im);
    } else if (im.memory) {
        free_memory(im.memory);
        std::lock_guard lock(memory_mutex_);
        counts_.own_images--, counts_.own_bytes -= im.bytes;
    }
    im = {};
}

VkResult Device::allocate(const VkMemoryAllocateInfo &info, VkDeviceMemory &out) {
    VkResult r = vkAllocateMemory(device_, &info, nullptr, &out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard lock(memory_mutex_);
    counts_.allocations++, counts_.allocations_made++;
    counts_.allocations_peak = std::max(counts_.allocations_peak, counts_.allocations);
    return r;
}

void Device::free_memory(VkDeviceMemory memory) {
    vkFreeMemory(device_, memory, nullptr);
    std::lock_guard lock(memory_mutex_);
    counts_.allocations--;
}

// The best fit among the blocks of the type, or a new block when none has
// room. Too large for a block (more than half of one): none, the image gets
// memory of its own, as VMA gives such allocations theirs.
bool Device::pool_take(const VkMemoryRequirements &need, uint32_t type, Image &im) {
    const VkDeviceSize heap = memory_.memoryHeaps[memory_.memoryTypes[type].heapIndex].size;
    const VkDeviceSize block_bytes = std::min(BLOCK_BYTES, std::max<VkDeviceSize>(heap / 8, VkDeviceSize(1) << 20));
    if (need.size > block_bytes / 2) return false;
    const VkDeviceSize alignment = std::max<VkDeviceSize>(need.alignment, 1);
    std::unique_lock lock(memory_mutex_);
    Block *best = nullptr;
    size_t best_number = 0;
    VkDeviceSize best_range = 0, best_range_bytes = 0, best_at = 0;
    for (size_t i = 0; i < blocks_.size(); i++) {
        Block *b = blocks_[i].get();
        if (!b || b->type != type || b->size - b->used < need.size) continue;
        VkDeviceSize range, range_bytes, at;
        if (b->fit(need.size, alignment, range, range_bytes, at) && (!best || range_bytes < best_range_bytes))
            best = b, best_number = i + 1, best_range = range, best_range_bytes = range_bytes, best_at = at;
    }
    if (!best) {
        // a new block (allocated outside the lock: it may take a while)
        lock.unlock();
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = block_bytes;
        alloc.memoryTypeIndex = type;
        VkDeviceMemory memory;
        if (allocate(alloc, memory) != VK_SUCCESS) return false;
        lock.lock();
        auto block = std::make_unique<Block>();
        block->memory = memory, block->type = type, block->size = block_bytes;
        block->add_free(0, block_bytes);
        best = block.get();
        auto empty = std::find(blocks_.begin(), blocks_.end(), nullptr);
        if (empty != blocks_.end()) {
            *empty = std::move(block);
            best_number = size_t(empty - blocks_.begin()) + 1;
        } else {
            blocks_.push_back(std::move(block));
            best_number = blocks_.size();
        }
        counts_.blocks++, counts_.block_bytes += block_bytes;
        best->fit(need.size, alignment, best_range, best_range_bytes, best_at);
    }
    // the range taken out, what is left before and after it given back
    best->drop_size(best_range_bytes, best_range);
    best->free.erase(best_range);
    best->add_free(best_range, best_at - best_range);
    best->add_free(best_at + need.size, best_range + best_range_bytes - (best_at + need.size));
    best->used += need.size;
    best->images++;
    counts_.pooled_images++, counts_.pooled_bytes += need.size;
    im.memory = best->memory, im.offset = best_at, im.bytes = need.size, im.block = uint32_t(best_number);
    return true;
}

// An image's range back to its block. A block left empty is freed while
// another empty one of its type is kept (so a frame that frees and makes the
// same images does not allocate again).
void Device::pool_give(Image &im) {
    VkDeviceMemory freed = VK_NULL_HANDLE;
    {
        std::lock_guard lock(memory_mutex_);
        Block *b = im.block <= blocks_.size() ? blocks_[im.block - 1].get() : nullptr;
        if (!b) return;
        b->add_free(im.offset, im.bytes);
        b->used -= im.bytes;
        b->images--;
        counts_.pooled_images--, counts_.pooled_bytes -= im.bytes;
        if (!b->images) {
            bool other_empty = false;
            for (auto &o : blocks_)
                if (o && o.get() != b && o->type == b->type && !o->images) other_empty = true;
            if (other_empty) {
                freed = b->memory;
                counts_.blocks--, counts_.block_bytes -= b->size;
                blocks_[im.block - 1].reset();
            }
        }
    }
    if (freed) free_memory(freed);
}

MemoryCounts Device::memory_counts() {
    std::lock_guard lock(memory_mutex_);
    return counts_;
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
    VkResult r = this->submit(1, &submit, fence_);
    if (r == VK_SUCCESS) r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 20'000'000'000ull);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    if (r != VK_SUCCESS) return error = failed("a submission", r), false;
    return true;
}

VkResult Device::submit(uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return vkQueueSubmit(queue_, count, submits, fence);
}

} // namespace xf::vk
