/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The Win32-style functions XAM exports for titles (its kernel32 part):
// threads, sleeps and waits, handles, the last error, the clock, module
// handles, RtlRandom, lstrlenW, and XamAllocEx. Each is the kernel export
// it wraps, called as XAM's code calls it (call_export, xam.h), with the
// NTSTATUS turned into the thread's last error. Translated from Xenia (BSD;
// see LICENSE-xenia): xam_info.cc; checked against it.
//
// Where it differs from Xenia, and why:
//   - WaitForSingleObject(Ex) answers as kernel32 does: the wait's status
//     (WAIT_OBJECT_0, WAIT_TIMEOUT, WAIT_IO_COMPLETION...), or WAIT_FAILED
//     (-1) with the last error set when the wait fails; Xenia answers -1
//     whatever happened;
//   - XapiFormatTimeOut answers the guest address of the timeout it wrote
//     (Xenia hands back the host's);
//   - RtlRandom's table is the kernel's, under a lock (Xenia's is one global
//     table, updated racily as XAM's is).

#include "xam.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_ALERTED = 0x00000101;
constexpr uint32_t USER_MODE = 1; // MODE::UserMode

bool failed(uint32_t status) { return int32_t(status) < 0; } // XFAILED

// RtlSetLastNTError: the status as a Win32 error, the thread's last
uint32_t set_last_nt_error(Kernel &k, ppc::Context &c, uint32_t status) {
    uint32_t error = call_export(k, c, "xboxkrnl", "RtlNtStatusToDosError", {status});
    set_last_error(k, error);
    return error;
}

// RtlSleep: KeDelayExecutionThread for ms milliseconds (-1: forever), on
// through the alerts of an alertable one
uint32_t rtl_sleep(Kernel &k, ppc::Context &c, uint32_t ms, uint32_t alertable) {
    uint32_t interval = export_scratch(c);
    k.store64(interval, ms == 0xFFFFFFFF ? uint64_t(INT64_MAX) : uint64_t(int64_t(-10'000) * ms));
    uint32_t status;
    do {
        status = call_export(k, c, "xboxkrnl", "KeDelayExecutionThread", {USER_MODE, alertable, interval});
    } while (alertable && status == X_STATUS_ALERTED);
    return status == X_STATUS_SUCCESS ? X_STATUS_SUCCESS : X_STATUS_USER_APC;
}

// XapiFormatTimeOut: an NT relative timeout for ms milliseconds in the
// guest's 64 bits, 0 (forever) for -1
uint32_t format_timeout(Kernel &k, uint32_t result_ptr, uint32_t ms) {
    if (ms == 0xFFFFFFFF) return 0;
    k.store64(result_ptr, uint64_t(int64_t(-10'000) * ms));
    return result_ptr;
}

// XapipCreateThread: ExCreateThread with the start address as the startup
// routine, so it gets the parameter; CREATE_SUSPENDED (4) and the
// processor (-1 any) as ExCreateThread's flags
uint32_t create_thread(Kernel &k, ppc::Context &c, uint32_t stack_size, uint32_t start_address, uint32_t parameter,
                       uint32_t creation_flags, uint32_t processor, uint32_t thread_id_ptr) {
    uint32_t flags = (creation_flags >> 2) & 1;
    if (processor != 0xFFFFFFFF && processor < 8) flags |= (1u << processor) << 24;
    uint32_t handle_ptr = export_scratch(c);
    k.store32(handle_ptr, 0);
    uint32_t status = call_export(k, c, "xboxkrnl", "ExCreateThread",
                                  {handle_ptr, stack_size, thread_id_ptr, start_address, parameter, 0, flags});
    if (failed(status)) {
        set_last_nt_error(k, c, status);
        return 0;
    }
    return k.load32(handle_ptr);
}

// XAM's RtlRandom table, as xam.xex starts with it
struct RandomState {
    std::mutex mutex;
    int32_t table[128] = {
        1284227242, 1275210071, 573735546,  790525478,  2139871995, 1547161642, 179605362,  789336058,  688789844,
        1801674531, 1563985344, 1957994488, 1364589140, 1645522239, 287218729,  606747145,  1972579041, 1085031214,
        1425521274, 1482476501, 1844823847, 57989841,   1897051121, 1935655697, 1078145449, 1960408961, 1682526488,
        842925246,  1500820517, 1214440339, 1647877149, 682003330,  261478967,  2052111302, 162531612,  583907252,
        1336601894, 1715567821, 413027322,  484763869,  1383384898, 1004336348, 764733703,  854245398,  651377827,
        1614895754, 838170752,  1757981266, 602609370,  1644491937, 926492609,  220523388,  115176313,  725345543,
        261903793,  746137067,  920026266,  1123561945, 1580818891, 1708537768, 616249376,  1292428093, 562591055,
        343818398,  1788223648, 1659004503, 2077806209, 299502267,  1604221776, 602162358,  630328440,  1606980848,
        1580436667, 1078081533, 492894223,  839522115,  1979792683, 117609710,  1767777339, 1454471165, 1965331169,
        1844237615, 308236825,  329068152,  412668190,  796845957,  1303643608, 436374069,  1677128483, 527237240,
        813497703,  1060284298, 1770027372, 1177238915, 884357618,  1409082233, 1958367476, 448539723,  1592454029,
        861567501,  963894560,  73586283,   362288127,  507921405,  113007714,  823518204,  152049171,  1202660629,
        1326574676, 2025429265, 1035525444, 515967899,  1532298954, 2000478354, 1450960922, 1417001333, 2049760794,
        1229272821, 879983540,  1993962763, 706699826,  776561741,  2111687655, 1343024091, 1637723038, 1220945662,
        484061587,  1390067357,
    };
};

// RtlUniform's step, in 32 bits as XAM computes it
uint32_t uniform(uint32_t seed) { return (0x7FFFFFEDu * seed + 0x7FFFFFC3u) % 0x7FFFFFFFu; }

} // namespace

// ---- sleeps ----

// (milliseconds, alertable): STATUS_SUCCESS, or STATUS_USER_APC after APCs
XF_EXPORT(xam, RtlSleep) { result(c, rtl_sleep(k, c, arg(k, c, 0), arg(k, c, 1))); }
XF_EXPORT(xam, SleepEx) { result(c, rtl_sleep(k, c, arg(k, c, 0), arg(k, c, 1))); }
XF_EXPORT(xam, Sleep) { rtl_sleep(k, c, arg(k, c, 0), 0); }

// ---- waits ----

// (result_ptr, milliseconds): its 64 bits
XF_EXPORT(xam, XapiFormatTimeOut) { c.r[3] = format_timeout(k, arg(k, c, 0), arg(k, c, 1)); }

// (handle, milliseconds, alertable)
XF_EXPORT(xam, WaitForSingleObjectEx) {
    uint32_t handle = arg(k, c, 0), ms = arg(k, c, 1), alertable = arg(k, c, 2);
    uint32_t timeout = format_timeout(k, export_scratch(c), ms);
    uint32_t status;
    do {
        status = call_export(k, c, "xboxkrnl", "NtWaitForSingleObjectEx", {handle, USER_MODE, alertable, timeout});
    } while (alertable && status == X_STATUS_ALERTED);
    if (failed(status)) {
        set_last_nt_error(k, c, status);
        return result(c, 0xFFFFFFFF); // WAIT_FAILED
    }
    result(c, status);
}

XF_EXPORT(xam, WaitForSingleObject) {
    static const ExportFn ex = find_export_fn("xam", "WaitForSingleObjectEx");
    c.r[5] = 0;
    ex(k, c);
}

// ---- threads ----

// (thread_attributes, stack_size, start_address, parameter, creation_flags,
// processor, thread_id_ptr): the handle, 0 on failure
XF_EXPORT(xam, XapipCreateThread) {
    result(c, create_thread(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5), arg(k, c, 6)));
}

// (thread_attributes, stack_size, start_address, parameter, creation_flags,
// thread_id_ptr): on any processor
XF_EXPORT(xam, CreateThread) {
    result(c, create_thread(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), 0xFFFFFFFF, arg(k, c, 5)));
}

// (handle): the previous suspend count, -1 on failure
XF_EXPORT(xam, ResumeThread) {
    uint32_t count_ptr = export_scratch(c);
    k.store32(count_ptr, 0);
    uint32_t status = call_export(k, c, "xboxkrnl", "NtResumeThread", {arg(k, c, 0), count_ptr});
    if (failed(status)) {
        set_last_nt_error(k, c, status);
        return result(c, 0xFFFFFFFF);
    }
    result(c, k.load32(count_ptr));
}

// (exit_code): the thread ends
XF_EXPORT(xam, ExitThread) { call_export(k, c, "xboxkrnl", "ExTerminateThread", {arg(k, c, 0)}); }

XF_EXPORT(xam, GetCurrentThreadId) {
    Thread *t = current_thread();
    result(c, t ? t->id : 0);
}

// ---- handles and errors ----

// (handle): TRUE, or FALSE with the last error
XF_EXPORT(xam, CloseHandle) {
    uint32_t status = call_export(k, c, "xboxkrnl", "NtClose", {arg(k, c, 0)});
    if (failed(status)) {
        set_last_nt_error(k, c, status);
        return result(c, 0);
    }
    result(c, 1);
}

// (status): its Win32 error, now the thread's last
XF_EXPORT(xam, RtlSetLastNTError) { result(c, set_last_nt_error(k, c, arg(k, c, 0))); }
XF_EXPORT(xam, RtlGetLastError) { result(c, last_error(k)); }
XF_EXPORT(xam, GetLastError) { result(c, last_error(k)); }

// (overlapped): its extended error, ERROR_IO_INCOMPLETE while it is
// pending, the thread's last error with none
XF_EXPORT(xam, XGetOverlappedExtendedError) {
    uint32_t ov = arg(k, c, 0);
    if (!ov) return result(c, last_error(k));
    if (k.load32(ov + xoverlapped::RESULT) == X_ERROR_IO_PENDING) return result(c, X_ERROR_IO_INCOMPLETE);
    result(c, k.load32(ov + xoverlapped::EXTENDED_ERROR));
}

// (module_name): its handle (none: the executable's), 0 when it is not
// loaded
XF_EXPORT(xam, GetModuleHandleA) {
    uint32_t hmodule_ptr = export_scratch(c);
    k.store32(hmodule_ptr, 0);
    uint32_t status = call_export(k, c, "xboxkrnl", "XexGetModuleHandle", {arg(k, c, 0), hmodule_ptr});
    if (failed(status)) {
        set_last_nt_error(k, c, status);
        return result(c, 0);
    }
    result(c, k.load32(hmodule_ptr));
}

// ---- the clock ----

XF_EXPORT(xam, GetTickCount) { result(c, k.uptime_ms()); }

// (time_ptr): KeQuerySystemTime's, KeTimeStampBundle with it
XF_EXPORT(xam, GetSystemTimeAsFileTime) {
    if (uint32_t time_ptr = arg(k, c, 0)) call_export(k, c, "xboxkrnl", "KeQuerySystemTime", {time_ptr});
}

// (frequency_ptr): the time base's (its low 32 bits, as Xenia stores it)
XF_EXPORT(xam, QueryPerformanceFrequency) {
    uint32_t frequency = call_export(k, c, "xboxkrnl", "KeQueryPerformanceFrequency", {});
    if (uint32_t p = arg(k, c, 0)) k.store64(p, frequency);
    result(c, 1);
}

// ---- the rest ----

// (seed_ptr): two steps of the seed; the table's entry the second picks,
// replaced by the first
XF_EXPORT(xam, RtlRandom) {
    uint32_t seed_ptr = arg(k, c, 0);
    RandomState &s = k.state<RandomState>();
    std::lock_guard<std::mutex> l(s.mutex);
    uint32_t first = uniform(k.load32(seed_ptr));
    uint32_t second = uniform(first);
    k.store32(seed_ptr, second);
    int32_t r = s.table[second & 0x7F];
    s.table[second & 0x7F] = int32_t(first);
    result(c, uint32_t(r));
}

// (no arguments known): success, as Xenia's stub (xam_info.cc, Refresh)
XF_EXPORT(xam, Refresh) { result(c, X_ERROR_SUCCESS); }

// (string): its length in characters
XF_EXPORT(xam, lstrlenW) {
    uint32_t s = arg(k, c, 0);
    result(c, s ? uint32_t(guest_u16(k, s).size()) : 0);
}

// (physical_flags, flags, size, out_ptr): from the system heap, or with
// 0x40000000 in the flags physical memory: the protection by bits 28-29 of
// the physical flags, the alignment by bits 24-27 (4 KB by default),
// zeroed with their 0x40000000
XF_EXPORT(xam, XamAllocEx) {
    uint32_t physical_flags = arg(k, c, 0), flags = arg(k, c, 1), size = arg(k, c, 2), out_ptr = arg(k, c, 3);
    if (!(flags & 0x40000000)) {
        uint32_t p = k.memory().system_alloc(size);
        if (out_ptr) k.store32(out_ptr, p);
        return result(c, X_ERROR_SUCCESS);
    }
    // X_PAGE_READONLY, READWRITE | NOCACHE, READWRITE, WRITECOMBINE | READWRITE
    static constexpr uint32_t protections[4] = {0x02, 0x04 | 0x200, 0x04, 0x400 | 0x04};
    uint32_t remapped = physical_flags;
    if (!(physical_flags & 0x0F000000)) remapped = 0x0C000000 | (physical_flags & 0xF0FFFFFF);
    uint32_t p =
        call_export(k, c, "xboxkrnl", "MmAllocatePhysicalMemoryEx",
                    {2, size, protections[(remapped >> 28) & 3], 0, 0xFFFFFFFF, 1u << ((remapped >> 24) & 0xF)});
    // (read-only pages cannot be written from here: those stay as the heap gives them)
    if (p && (remapped & 0x40000000) && ((remapped >> 28) & 3) != 0) std::memset(k.host(p), 0, size);
    if (out_ptr) k.store32(out_ptr, p);
    result(c, p ? 0 : 0x8007000E); // E_OUTOFMEMORY
}

} // namespace xf::kernel
