// xfbench's present hook (docs/renderer.md, "The standing benchmark"): a DLL
// that records the time of every present of the process it is loaded into,
// so that two programs on two graphics APIs are timed by the same code.
//
// - As a Vulkan layer (VK_LAYER_XF_bench, its manifest beside the DLL), it
//   wraps vkQueuePresentKHR.
// - Injected into a Direct3D 12 program by xfbench (before its main thread
//   runs), xfbench_hook_dxgi() patches DXGI's factory so that every swap
//   chain it makes has its Present and Present1 wrapped.
//
// Each present appends its QueryPerformanceCounter value (8 bytes) to the
// file named by XF_BENCH_LOG; xfbench reads it after the run. Windows only
// (the standing benchmark runs against Xenia, a Windows program).

#define VK_NO_PROTOTYPES
#include <windows.h>

#include <dxgi1_2.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

// vk_layer.h declares the loader's negotiation entry, which this DLL exports
#ifdef __clang__
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
#endif

namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;
std::once_flag g_log_once;

void record_present() {
    std::call_once(g_log_once, [] {
        wchar_t path[1024];
        if (GetEnvironmentVariableW(L"XF_BENCH_LOG", path, 1024) > 0)
            g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    });
    if (g_log == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    DWORD written = 0;
    WriteFile(g_log, &t.QuadPart, sizeof(t.QuadPart), &written, nullptr);
}

// ---- the Vulkan layer ----

// dispatchable handles of one instance or device share the loader's dispatch
// table, whose address is their first word
void *key(const void *handle) { return *(void *const *)handle; }

struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
};
std::mutex g_mutex;
std::unordered_map<void *, PFN_vkGetInstanceProcAddr> g_instances;
std::unordered_map<void *, DeviceData> g_devices;

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc,
                                              VkInstance *instance) {
    auto *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(info, alloc, instance);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[key(*instance)] = gipa;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                            const VkAllocationCallbacks *alloc, VkDevice *device) {
    auto *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = create(physical, info, alloc, device);
    if (r == VK_SUCCESS) {
        DeviceData d;
        d.gdpa = gdpa;
        d.present = (PFN_vkQueuePresentKHR)gdpa(*device, "vkQueuePresentKHR");
        d.destroy = (PFN_vkDestroyDevice)gdpa(*device, "vkDestroyDevice");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices[key(*device)] = d;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    DeviceData d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = g_devices[key(device)];
        g_devices.erase(key(device));
    }
    if (d.destroy) d.destroy(device, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    PFN_vkQueuePresentKHR next;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        next = g_devices[key(queue)].present;
    }
    VkResult r = next(queue, info);
    record_present();
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)GetInstanceProcAddr;
    if (!std::strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
    if (!std::strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr next;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        next = g_instances[key(instance)];
    }
    return next ? next(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!std::strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)QueuePresentKHR;
    if (!std::strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    PFN_vkGetDeviceProcAddr next;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        next = g_devices[key(device)].gdpa;
    }
    return next ? next(device, name) : nullptr;
}

// ---- DXGI ----

// vtable slots (dxgi.h and dxgi1_2.h, in declaration order after IUnknown's 3
// and IDXGIObject's 4): IDXGIFactory2::CreateSwapChainForHwnd 15,
// IDXGISwapChain::Present 8, IDXGISwapChain1::Present1 22
constexpr int kCreateSwapChainForHwnd = 15, kPresent = 8, kPresent1 = 22;

using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                     IDXGISwapChain1 **);
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
CreateForHwndFn g_create_for_hwnd = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;

void patch(void **vtable, int slot, void *fn, void **original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return;
    if (original && !*original) *original = vtable[slot];
    vtable[slot] = fn;
    VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain *chain, UINT interval, UINT flags) {
    HRESULT r = g_present(chain, interval, flags);
    if (!(flags & DXGI_PRESENT_TEST)) record_present();
    return r;
}

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1 *chain, UINT interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS *params) {
    HRESULT r = g_present1(chain, interval, flags, params);
    if (!(flags & DXGI_PRESENT_TEST)) record_present();
    return r;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *device, HWND window,
                                                 const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
                                                 IDXGIOutput *output, IDXGISwapChain1 **chain) {
    HRESULT r = g_create_for_hwnd(factory, device, window, desc, fullscreen, output, chain);
    if (SUCCEEDED(r) && chain && *chain) {
        void **vtable = *(void ***)*chain;
        if (vtable[kPresent] != (void *)Present) patch(vtable, kPresent, (void *)Present, (void **)&g_present);
        if (vtable[kPresent1] != (void *)Present1) patch(vtable, kPresent1, (void *)Present1, (void **)&g_present1);
    }
    return r;
}

} // namespace

extern "C" {

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return GetInstanceProcAddr(instance, name);
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return GetDeviceProcAddr(device, name);
}

// Run by xfbench in the target process (a thread of its own, before the
// target's main thread starts): DXGI's factory class patched so that the
// target's swap chains are wrapped. Returns 0 on success.
__declspec(dllexport) DWORD WINAPI xfbench_hook_dxgi(void *) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return 1;
    using CreateFactory2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
    auto create = (CreateFactory2)GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (!create) return 2;
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(create(0, __uuidof(IDXGIFactory2), (void **)&factory)) || !factory) return 3;
    patch(*(void ***)factory, kCreateSwapChainForHwnd, (void *)CreateSwapChainForHwnd, (void **)&g_create_for_hwnd);
    // the patched vtable is the class's: the factory itself may go
    factory->Release();
    return 0;
}

} // extern "C"
