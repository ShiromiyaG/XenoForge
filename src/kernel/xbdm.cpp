/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The debug monitor's exports (xbdm.xex): what a game built against the
// development kit's libraries calls, answered mostly as a retail console
// without a debugger would. Translated from Xenia (BSD; see LICENSE-xenia):
// xbdm/xbdm_misc.cc and xbdm_misc.h (the structures and the XBDM_* results),
// kernel_state.cc's InitializeXbdmCpuCounters; checked against them.
//
// The imports from xbdm.xex are bound as the other system libraries' are
// (kernel.cpp, XF_EXPORT(xbdm, ...) here), but no xbdm.xex module is listed:
// XexGetModuleHandle("xbdm.xex") fails, as on a retail console and in Xenia
// with its default console type (-1, retail; Xenia loads the module only for
// a development or test kit, and leaves the imports unresolved otherwise).
//
// Where it differs from Xenia:
//   - DmMapDevkitDrive mounts \DEVKIT at "devkit" under the storage root (the
//     cache partitions' and content's root), where Xenia takes "devkit" in
//     its working directory; without a storage root it fails;
//   - DmSuspendThread and DmResumeThread are NtSuspendThread's and
//     NtResumeThread's suspension (a thread stops at its next kernel call),
//     where Xenia suspends the host thread.

#include "xam.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace xf::kernel {
namespace {

constexpr uint32_t XBDM_SUCCESSFUL = 0x02DA0000;
constexpr uint32_t XBDM_UNSUCCESSFUL = 0x82DA0000;
constexpr uint32_t XBDM_ENDOFLIST = 0x82DA0104;
constexpr uint32_t XBDM_BUFFER_TOO_SMALL = 0x82DA0105;
constexpr uint32_t MAX_FRAMES_TO_CAPTURE = 256;
constexpr uint32_t X_FACILITY_NT_BIT = 0x10000000;

// Xenia's DmXboxName ("no idea what a real valid value is")
constexpr char XBOX_NAME[] = "Xbox360Name";
// Xenia's console_type default: retail
constexpr uint32_t CONSOLE_TYPE = 0xFFFFFFFF;
// XboxKrnlVersion's build at the kernel's start (kBaseKernelBuildVersion)
constexpr uint16_t BASE_KERNEL_BUILD = 1888;

// the structures' sizes (xbdm_misc.h)
constexpr uint32_t DM_XBE_SIZE = 276, DMN_MODULE_LOAD_SIZE = 292, DM_PDB_SIGNATURE_SIZE = 280;
constexpr uint32_t DM_THREAD_INFO_EX_SIZE = 52, DM_MEMORY_STATISTICS_SIZE = 48, MM_STATISTICS_SIZE = 104;

struct Xbdm {
    std::mutex mutex;
    // the performance counters' names (InitializeXbdmCpuCounters), once
    // DmPMCInstallAndStart made them
    uint32_t counters[0x11] = {};
};

// An xboxkrnl export run with these arguments on the calling thread's
// context, its registers given back after: its result.
uint32_t call_kernel(Kernel &k, ppc::Context &c, const char *name, std::initializer_list<uint32_t> args) {
    ExportFn fn = find_export_fn("xboxkrnl", name);
    if (!fn) return X_STATUS_NOT_IMPLEMENTED;
    uint64_t saved[8];
    std::copy(c.r + 3, c.r + 11, saved);
    int n = 3;
    for (uint32_t a : args) c.r[n++] = a;
    fn(k, c);
    uint32_t r = uint32_t(c.r[3]);
    std::copy(saved, saved + 8, c.r + 3);
    return r;
}

// BaseHeap::QueryRangeAccess: whether every page of the range is committed
// and readable and writable
bool read_write(Kernel &k, uint32_t address, uint32_t size) {
    uint64_t end = uint64_t(address) + size;
    for (uint64_t at = address; at < end;) {
        Heap *heap = k.memory().heap_for(uint32_t(at));
        RegionInfo region;
        if (!heap || !heap->query(uint32_t(at), region) || !region.region_size) return false;
        if (!(region.state & ALLOCATE_COMMIT) || (region.protect & PROTECT_READ_WRITE) != PROTECT_READ_WRITE)
            return false;
        at = uint64_t(region.base_address) + region.region_size;
    }
    return true;
}

// the guest thread with this id (lock held; not retained)
Thread *thread_by_id(Objects &o, uint32_t id) {
    for (Thread *t : o.threads)
        if (t->id == id) return t;
    return nullptr;
}

} // namespace

// ---- memory ----

XF_EXPORT(xbdm, DmAllocatePool) { result(c, k.memory().system_alloc(arg(k, c, 0))); }

XF_EXPORT(xbdm, DmFreePool) {
    k.memory().system_free(arg(k, c, 0));
    result(c, XBDM_SUCCESSFUL);
}

// (source, size, destination, bytes written): a copy, the destination's
// pages read-write (Xenia checks the destination's only)
namespace {
void copy_memory(Kernel &k, ppc::Context &c, uint32_t from, uint32_t size, uint32_t to, uint32_t written_ptr,
                 const char *name) {
    if (!to || !from || !size) return result(c, X_E_INVALIDARG);
    if (written_ptr) k.store32(written_ptr, 0);
    if (!k.memory().heap_for(to)) return result(c, XBDM_UNSUCCESSFUL);
    if (!read_write(k, to, size)) {
        std::fprintf(stderr, "xbdm: %s: 0x%08X is not read-write\n", name, to);
        return result(c, XBDM_UNSUCCESSFUL);
    }
    std::memmove(k.host(to), k.host(from), size);
    if (written_ptr) k.store32(written_ptr, size);
    result(c, XBDM_SUCCESSFUL);
}
} // namespace

// (destination, size, source, bytes written)
XF_EXPORT(xbdm, DmSetMemory) {
    copy_memory(k, c, arg(k, c, 2), arg(k, c, 1), arg(k, c, 0), arg(k, c, 3), "DmSetMemory");
}

// (source, size, destination, bytes written)
XF_EXPORT(xbdm, DmGetMemory) {
    copy_memory(k, c, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), "DmGetMemory");
}

// (statistics: X_DM_QUERY_MEMORY_STATISTICS_RESULT, its size first): the
// title's pages, from MmQueryStatistics
XF_EXPORT(xbdm, DmQueryTitleMemoryStatistics) {
    uint32_t stats = arg(k, c, 0);
    if (k.load32(stats) != DM_MEMORY_STATISTICS_SIZE) return result(c, X_E_INVALIDARG);
    uint32_t mm = k.memory().system_alloc(MM_STATISTICS_SIZE);
    k.store32(mm, MM_STATISTICS_SIZE);
    uint32_t status = call_kernel(k, c, "MmQueryStatistics", {mm});
    if (int32_t(status) < 0) {
        k.memory().system_free(mm);
        return result(c, status | X_FACILITY_NT_BIT);
    }
    // X_MM_QUERY_STATISTICS_SECTION, the title's
    uint32_t title = mm + 0x0C;
    uint32_t available = k.load32(title + 0x00), physical = k.load32(title + 0x0C), pool = k.load32(title + 0x10);
    uint32_t stack = k.load32(title + 0x14), image = k.load32(title + 0x18), heap = k.load32(title + 0x1C);
    uint32_t virt = k.load32(title + 0x20), page_table = k.load32(title + 0x24), cache = k.load32(title + 0x28);
    k.memory().system_free(mm);
    k.store32(stats + 0x08, available);
    k.store32(stats + 0x0C, stack);
    k.store32(stats + 0x10, page_table);
    k.store32(stats + 0x14, 0); // system page table pages
    k.store32(stats + 0x18, pool);
    k.store32(stats + 0x1C, heap + virt); // virtual mapped pages
    k.store32(stats + 0x20, image);
    k.store32(stats + 0x24, cache);
    k.store32(stats + 0x28, physical); // contiguous pages
    k.store32(stats + 0x2C, 0);        // debugger pages
    k.store32(stats + 0x04, available + stack + page_table + pool + heap + virt + image + cache + physical);
    result(c, XBDM_SUCCESSFUL);
}

// (memory configuration out): no additional memory
XF_EXPORT(xbdm, DmGetConsoleDebugMemoryStatus) {
    uint32_t out = arg(k, c, 0);
    if (!out) return result(c, X_E_INVALIDARG);
    k.store32(out, 0); // DM_CONSOLEMEMCONFIG_NOADDITIONALMEM
    result(c, XBDM_SUCCESSFUL);
}

// ---- the console ----

// (name, size in and out): the console's name, terminated
XF_EXPORT(xbdm, DmGetXboxName) {
    uint32_t name = arg(k, c, 0), size_ptr = arg(k, c, 1);
    if (!name || !size_ptr) return result(c, X_E_INVALIDARG);
    if (sizeof(XBOX_NAME) > k.load32(size_ptr)) return result(c, XBDM_BUFFER_TOO_SMALL);
    std::memcpy(k.host(name), XBOX_NAME, sizeof(XBOX_NAME));
    result(c, XBDM_SUCCESSFUL);
}

// (type out): -1, a retail console (Xenia's default)
XF_EXPORT(xbdm, DmGetConsoleType) {
    uint32_t out = arg(k, c, 0);
    if (!out) return result(c, X_E_INVALIDARG);
    k.store32(out, CONSOLE_TYPE);
    result(c, XBDM_SUCCESSFUL);
}

// no debugger (Xenia's debug setting, off in its releases)
XF_EXPORT(xbdm, DmIsDebuggerPresent) { result(c, 0); }

// (XBDM_SYSTEM_INFO): the base kernel's version and the running one's
// (2.0.build.0); the XDK's and the flags left as they were
XF_EXPORT(xbdm, DmGetSystemInfo) {
    uint32_t info = arg(k, c, 0);
    if (!info) return result(c, X_E_INVALIDARG);
    uint16_t build = k.load16(k.variable("XboxKrnlVersion") + 4);
    for (uint32_t version : {info + 0x04, info + 0x0C}) {
        k.store16(version + 0, 2);
        k.store16(version + 2, 0);
        k.store16(version + 6, 0);
    }
    k.store16(info + 0x04 + 4, BASE_KERNEL_BUILD);
    k.store16(info + 0x0C + 4, build);
    result(c, XBDM_SUCCESSFUL);
}

// (name, DM_XBE out): zeroed (Xenia notes a title that takes success)
XF_EXPORT(xbdm, DmGetXbeInfo) {
    uint32_t info = arg(k, c, 1);
    if (!info) return result(c, X_E_INVALIDARG);
    std::memset(k.host(info), 0, DM_XBE_SIZE);
    result(c, XBDM_SUCCESSFUL);
}

// (base, DM_PDB_SIGNATURE out): zeroed; no signature
XF_EXPORT(xbdm, DmFindPdbSignature) {
    uint32_t base = arg(k, c, 0), signature = arg(k, c, 1);
    if (!base || !signature) return result(c, X_E_INVALIDARG);
    std::memset(k.host(signature), 0, DM_PDB_SIGNATURE_SIZE);
    result(c, X_ERROR_INVALID_PARAMETER);
}

// The development kit's drive: \DEVKIT, a directory under the storage root,
// linked as DEVKIT: and e: (see the top)
XF_EXPORT(xbdm, DmMapDevkitDrive) {
    vfs::FileSystem &fs = file_system(k);
    if (fs.has_link("DEVKIT:")) return result(c, 0);
    std::filesystem::path root = content_root(k);
    if (root.empty()) {
        std::fprintf(stderr, "xbdm: DmMapDevkitDrive: no storage root to put the devkit drive in\n");
        return result(c, XBDM_UNSUCCESSFUL);
    }
    auto device = vfs::make_host_device("\\DEVKIT", root.parent_path() / "devkit", false);
    if (!device) {
        std::fprintf(stderr, "xbdm: DmMapDevkitDrive: unable to scan the devkit path\n");
        return result(c, XBDM_UNSUCCESSFUL);
    }
    fs.add_device(device);
    fs.add_link("DEVKIT:", "\\DEVKIT");
    fs.add_link("e:", "\\DEVKIT");
    result(c, 0);
}

// ---- threads ----

// (thread ids, count in and out): the guest threads' ids
XF_EXPORT(xbdm, DmGetThreadList) {
    uint32_t ids = arg(k, c, 0), count_ptr = arg(k, c, 1);
    if (!ids || !count_ptr) return result(c, X_E_INVALIDARG);
    uint32_t entries = k.load32(count_ptr);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t count = uint32_t(o.threads.size());
    if (count > entries) return result(c, XBDM_BUFFER_TOO_SMALL);
    if (count > MAX_FRAMES_TO_CAPTURE) return result(c, XBDM_UNSUCCESSFUL);
    std::memset(k.host(ids), 0, 4 * size_t(entries));
    for (uint32_t i = 0; i < count; i++) k.store32(ids + 4 * i, o.threads[i]->id);
    k.store32(count_ptr, count);
    result(c, XBDM_SUCCESSFUL);
}

// (thread id, DM_THREAD_INFO_EX out): the thread's state; its name and four
// words (the PCR, the start address, the stack's base and limit) in a page
// of the system's that the info points into (never freed, as Xenia's)
XF_EXPORT(xbdm, DmGetThreadInfoEx) {
    uint32_t id = arg(k, c, 0), info = arg(k, c, 1);
    if (!info) return result(c, X_E_INVALIDARG);
    std::memset(k.host(info), 0, DM_THREAD_INFO_EX_SIZE);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    Thread *t = thread_by_id(o, id);
    if (!t) return result(c, XBDM_UNSUCCESSFUL);
    // XThread's name: its own, else "XThread<id>", with its handle
    char name[96];
    if (t->params.name)
        std::snprintf(name, sizeof(name), "%s (%08X)", t->params.name, t->self_handle);
    else
        std::snprintf(name, sizeof(name), "XThread%04X (%08X)", t->id, t->self_handle);
    uint32_t name_size = uint32_t(std::strlen(name) + 1);
    uint32_t page = k.memory().system_alloc(0x1000);
    if (!page) return result(c, XBDM_UNSUCCESSFUL);
    std::memcpy(k.host(page), name, name_size);
    uint32_t words = page + name_size;
    k.store32(words + 0, t->pcr);
    k.store32(words + 4, k.load32(t->kthread() + kthread::START_ADDRESS));
    k.store32(words + 8, t->stack_base);
    k.store32(words + 12, t->stack_limit);
    k.store32(info + 0x00, DM_THREAD_INFO_EX_SIZE);
    k.store32(info + 0x04, t->suspend_count);
    k.store32(info + 0x08, k.host(t->kthread())[kthread::PRIORITY]);
    k.store32(info + 0x0C, words + 0);
    k.store32(info + 0x10, words + 4);
    k.store32(info + 0x14, words + 8);
    k.store32(info + 0x18, words + 12);
    k.store64(info + 0x1C, k.load64(t->kthread() + kthread::CREATE_TIME));
    k.store32(info + 0x24, 0); // stack slack space
    k.store32(info + 0x28, page);
    k.store32(info + 0x2C, name_size);
    k.host(info)[0x30] = 0; // current processor
    result(c, XBDM_SUCCESSFUL);
}

namespace {
// a thread suspended or resumed by id, as the Nt exports do it by handle
void suspend_or_resume(Kernel &k, ppc::Context &c, const char *name) {
    uint32_t handle = 0;
    {
        Objects &o = objects(k);
        std::lock_guard<std::mutex> l(o.lock);
        if (Thread *t = thread_by_id(o, arg(k, c, 0))) handle = t->self_handle;
    }
    if (!handle) return result(c, XBDM_UNSUCCESSFUL);
    uint32_t status = call_kernel(k, c, name, {handle, 0});
    result(c, int32_t(status) < 0 ? XBDM_UNSUCCESSFUL : XBDM_SUCCESSFUL);
}
} // namespace

XF_EXPORT(xbdm, DmSuspendThread) { suspend_or_resume(k, c, "NtSuspendThread"); }
XF_EXPORT(xbdm, DmResumeThread) { suspend_or_resume(k, c, "NtResumeThread"); }

// (frames, back trace out): zeroed frames; at most 256
XF_EXPORT(xbdm, DmCaptureStackBackTrace) {
    uint32_t frames = arg(k, c, 0), trace = arg(k, c, 1);
    if (frames > MAX_FRAMES_TO_CAPTURE) return result(c, X_E_INVALIDARG);
    if (frames) std::memset(k.host(trace), 0, 4 * size_t(frames));
    result(c, XBDM_SUCCESSFUL);
}

// ---- modules ----

// (walk, DMN_MODULE_LOAD out): no module to report: zeroed, the end of the
// list (some games loop forever until they get it)
XF_EXPORT(xbdm, DmWalkLoadedModules) {
    uint32_t walk = arg(k, c, 0), module = arg(k, c, 1);
    if (!walk || !module) return result(c, X_E_INVALIDARG);
    std::memset(k.host(module), 0, DMN_MODULE_LOAD_SIZE);
    result(c, XBDM_ENDOFLIST);
}

XF_EXPORT(xbdm, DmCloseLoadedModules) { result(c, XBDM_SUCCESSFUL); }

// ---- notifications and command processors: taken, nothing comes ----

XF_EXPORT(xbdm, DmSendNotificationString) { result(c, arg(k, c, 0) ? XBDM_SUCCESSFUL : X_E_INVALIDARG); }

// (name, handler)
XF_EXPORT(xbdm, DmRegisterCommandProcessor) { result(c, arg(k, c, 0) ? XBDM_SUCCESSFUL : X_E_INVALIDARG); }
// (name, handler, thread)
XF_EXPORT(xbdm, DmRegisterCommandProcessorEx) { result(c, arg(k, c, 0) ? XBDM_SUCCESSFUL : X_E_INVALIDARG); }

// (flags, session out): session 0
XF_EXPORT(xbdm, DmOpenNotificationSession) {
    uint32_t session = arg(k, c, 1);
    if (!session) return result(c, X_E_INVALIDARG);
    k.store32(session, 0);
    result(c, XBDM_SUCCESSFUL);
}

// (session, notification, callback)
XF_EXPORT(xbdm, DmNotify) { result(c, arg(k, c, 0) ? XBDM_SUCCESSFUL : X_E_INVALIDARG); }

// ---- profiling ----

// (log file name, buffer size)
XF_EXPORT(xbdm, DmStartProfiling) { result(c, arg(k, c, 0) ? XBDM_SUCCESSFUL : X_E_INVALIDARG); }
XF_EXPORT(xbdm, DmStopProfiling) { result(c, XBDM_SUCCESSFUL); }
XF_EXPORT(xbdm, DmSetProfilingOptions) { result(c, XBDM_SUCCESSFUL); }
XF_EXPORT(xbdm, DmSetDumpMode) { result(c, XBDM_SUCCESSFUL); }

// fast call-attributed profiling: off
XF_EXPORT(xbdm, DmIsFastCAPEnabled) { result(c, XBDM_UNSUCCESSFUL); }

// (group setup): the counters' names made at 0x91F00000 on the first call
// (InitializeXbdmCpuCounters; Xenia's guesses, the last one the answer to a
// bad number)
XF_EXPORT(xbdm, DmPMCInstallAndStart) {
    Xbdm &x = k.state<Xbdm>();
    std::lock_guard<std::mutex> l(x.mutex);
    if (x.counters[0]) return result(c, XBDM_SUCCESSFUL);
    constexpr uint32_t BASE = 0x91F00000;
    static constexpr const char *names[0x11] = {
        "load-hit-stores (S)",
        "instructions committed",
        "i-cache miss cycles",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "core 0 L2 data misses",
        "Bad counter number - must be 0-15.",
    };
    Heap *heap = k.memory().heap_for(BASE);
    if (!heap || !heap->alloc_fixed(BASE, 0x1000, 0, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE))
        return result(c, XBDM_SUCCESSFUL);
    uint32_t at = BASE;
    for (size_t i = 0; i < std::size(names); i++) {
        x.counters[i] = at;
        size_t n = std::strlen(names[i]);
        std::memcpy(k.host(at), names[i], n + 1);
        at += uint32_t(n) + 1;
    }
    result(c, XBDM_SUCCESSFUL);
}

// (counter): its name's address (0 before DmPMCInstallAndStart); a number
// past 15 gets the bad number's
XF_EXPORT(xbdm, DmPMCGetCounterName) {
    uint32_t counter = arg(k, c, 0);
    Xbdm &x = k.state<Xbdm>();
    std::lock_guard<std::mutex> l(x.mutex);
    result(c, x.counters[counter < 0x10 ? counter : 0x10]);
}

// the instrumentation the compiler's /callcap inserts: nothing
XF_EXPORT(xbdm, __CAP_Start_Profiling) {}
XF_EXPORT(xbdm, __CAP_End_Profiling) {}
XF_EXPORT(xbdm, __CAP_Enter_Function) {}
XF_EXPORT(xbdm, __CAP_Exit_Function) {}

} // namespace xf::kernel
