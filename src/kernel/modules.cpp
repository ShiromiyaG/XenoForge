/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The modules family (plan step A4; docs/runtime.md): the kernel modules
// (xboxkrnl.exe, xam.xex) and the game's executable with their loader entries
// (X_LDR_DATA_TABLE_ENTRY, the HMODULE), the executable's XEX header in guest
// memory, the kernel's variables at Xenia's addresses, the kernel's guest
// globals (the object types, the processes), and the Xex* exports. Translated
// from Xenia (BSD; see LICENSE-xenia): xboxkrnl_module.cc (the variables),
// xboxkrnl_modules.cc (the exports), xboxkrnl_video.cc (the Vd* variables),
// kernel_state.cc (the globals, the executable's setup, KeTimeStampBundle),
// xmodule.cc, kernel_module.cc and user_module.cc (the modules); checked
// against them. Leaner than Xenia: a module is no kernel object here (its
// loader entry's checksum, Xenia's handle, stays 0), and loading another
// executable from the disc waits for the files family.
//
// The guest globals (Xenia's KernelGuestGlobals) are at
// k.variable("ExThreadObjectType"), the first field:
//   0x000  10 object types (X_OBJECT_TYPE, 0x1C each): ExThread, ExEvent,
//          ExMutant, ExSemaphore, ExTimer, IoCompletion, IoDevice, IoFile,
//          ObDirectory, ObSymbolicLink
//   0x118  XboxKernelDefaultObject (X_DISPATCH_HEADER)
//   0x128  the idle process, 0x188 the title process, 0x1E8 the system
//          process (X_KPROCESS, 0x60 each)
//   0x248  the dispatcher lock, 0x24C the Ob lock, 0x250 the TLS lock
//   0x254  UsbdBootEnumerationDoneEvent (X_KEVENT)

#include "xenoforge/exports.h"
#include "xenoforge/kernel.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t X_STATUS_NOT_FOUND = 0xC0000225;
constexpr uint32_t X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND = 0xC0000263;
constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x00000057;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x00000490;

// X_LDR_DATA_TABLE_ENTRY (the HMODULE points at it)
constexpr uint32_t LDR_SIZE = 0x64;
constexpr uint32_t LDR_DLL_BASE = 0x18;
constexpr uint32_t LDR_IMAGE_BASE = 0x1C;
constexpr uint32_t LDR_FULL_IMAGE_SIZE = 0x38;
constexpr uint32_t LDR_ENTRY_POINT = 0x3C;
constexpr uint32_t LDR_LOAD_COUNT = 0x40; // 16 bits
constexpr uint32_t LDR_XEX_HEADER_BASE = 0x58;

// the kernel's variables, where Xenia puts them (most at the real kernel's
// addresses)
constexpr uint32_t XboxKrnlVersion = 0x8004045C;
constexpr uint32_t XboxHardwareInfo = 0x801D0030;
constexpr uint32_t VdGpuClockInMHz = 0x801D0E94;
constexpr uint32_t VdHSIOCalibrationLock = 0x801D1210;
constexpr uint32_t VdGlobalDevice = 0x801E6FC4;
constexpr uint32_t VdGlobalXamDevice = 0x801E6FC8;
constexpr uint32_t ExConsoleGameRegion = 0x80205150;
constexpr uint32_t KeCertMonitorData = 0x80207A60;
constexpr uint32_t KeDebugMonitorData = 0x80207A64;
constexpr uint32_t ExLoadedCommandLine = 0x80207C60;
constexpr uint32_t ExLoadedImageName = 0x80207E60;
constexpr uint32_t XexExecutableModuleHandle = 0x802080D4;
constexpr uint32_t KeTimeStampBundle = 0x80240EE0;
constexpr uint32_t ExLoadedCommandLineSize = 1024;
constexpr uint32_t ExLoadedImageNameSize = 256;

// KernelGuestGlobals
constexpr uint32_t GLOBALS_SIZE = 0x264;
constexpr uint32_t OBJECT_TYPE_SIZE = 0x1C;
enum ObjectType : uint32_t {
    ExThreadObjectType,
    ExEventObjectType,
    ExMutantObjectType,
    ExSemaphoreObjectType,
    ExTimerObjectType,
    IoCompletionObjectType,
    IoDeviceObjectType,
    IoFileObjectType,
    ObDirectoryObjectType,
    ObSymbolicLinkObjectType,
};
constexpr const char *object_type_names[] = {
    "ExThreadObjectType",    "ExEventObjectType",        "ExMutantObjectType", "ExSemaphoreObjectType",
    "ExTimerObjectType",     "IoCompletionObjectType",   "IoDeviceObjectType", "IoFileObjectType",
    "ObDirectoryObjectType", "ObSymbolicLinkObjectType",
};
constexpr uint32_t DEFAULT_OBJECT = 0x118;
constexpr uint32_t IDLE_PROCESS = 0x128, TITLE_PROCESS = 0x188, SYSTEM_PROCESS = 0x1E8;
constexpr uint32_t USBD_BOOT_ENUMERATION_DONE_EVENT = 0x254;

// Xenia's kTrampolineSize: room for 400 GetProcAddress trampolines a module
constexpr uint32_t TRAMPOLINE_SIZE = 400 * 8;

struct Module {
    std::string path;    // "xe:\\xboxkrnl.exe", "\\Device\\Cdrom0\\default.xex"
    std::string name;    // "xboxkrnl", "default"
    std::string library; // a kernel module's export table ("xboxkrnl"); empty for a user module
    uint32_t hmodule = 0;
    // a user module's
    uint32_t xex_header = 0, base_address = 0;
    // a kernel module's GetProcAddress trampolines
    uint32_t trampoline = 0, trampoline_next = 0;
    std::map<uint16_t, uint32_t> trampolines;

    bool is_kernel() const { return !library.empty(); }
};

struct Modules {
    Kernel *k = nullptr;
    std::mutex mutex;
    std::vector<std::unique_ptr<Module>> kernel_modules, user_modules;
    Module *executable = nullptr;
    uint32_t globals = 0;
    std::vector<std::pair<uint32_t, uint32_t>> terminate_notifications; // routine, priority
    std::thread timer;
    std::atomic<bool> running{false};

    ~Modules() {
        running = false;
        if (timer.joinable()) timer.join();
    }
};

bool equal_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

// xe::utf8::find_name_from_guest_path: the last part of a path
std::string_view name_from_path(std::string_view path) {
    size_t slash = path.find_last_of('\\');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// XModule::Matches
bool matches(const Module &m, std::string_view name) {
    return equal_case(name_from_path(m.path), name) || equal_case(m.name, name) || equal_case(m.path, name);
}

std::string read_string(Kernel &k, uint32_t address) {
    if (!address) return {};
    const char *s = reinterpret_cast<const char *>(k.host(address));
    return std::string(s, strnlen(s, 0x1000));
}

// UserModule::GetOptHeader on a header in guest memory: a value stored in
// the header (key's low byte 0), the guest address of the value (1), or of
// the data at its offset; false when the header has no such key
bool opt_header(Kernel &k, uint32_t header, uint32_t key, uint32_t &out) {
    out = 0;
    uint32_t count = k.load32(header + 0x14);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = header + 0x18 + 8 * i;
        if (k.load32(entry) != key) continue;
        switch (key & 0xFF) {
        case 0x00: out = k.load32(entry + 4); break;
        case 0x01: out = entry + 4; break;
        default: out = header + k.load32(entry + 4); break;
        }
        return true;
    }
    return false;
}

// KernelState::GetModule: the executable for no name, the kernel modules,
// then the user modules (by the path the name resolves to: "game:" and "d:"
// are the disc, as Xenia's symbolic links make them)
Module *find_module(Modules &s, std::string_view name) {
    if (name.empty()) return s.executable;
    if (equal_case(name, "kernel32.dll")) return nullptr; // some games ask for it
    for (auto &m : s.kernel_modules)
        if (matches(*m, name)) return m.get();
    std::string path(name);
    for (std::string_view device : {"game:", "d:"}) {
        if (path.size() >= device.size() && equal_case(std::string_view(path).substr(0, device.size()), device)) {
            path = "\\Device\\Cdrom0" + path.substr(device.size());
            break;
        }
    }
    for (auto &m : s.user_modules)
        if (matches(*m, path)) return m.get();
    return nullptr;
}

// XModule::GetFromHModule
Module *module_from_hmodule(Modules &s, uint32_t hmodule) {
    if (!hmodule) return nullptr;
    for (auto &m : s.kernel_modules)
        if (m->hmodule == hmodule) return m.get();
    for (auto &m : s.user_modules)
        if (m->hmodule == hmodule) return m.get();
    return nullptr;
}

// KernelModule's constructor: the loader entry and the trampolines' room
Module &add_kernel_module(Kernel &k, Modules &s, const char *path, const char *library) {
    auto m = std::make_unique<Module>();
    m->path = path;
    m->library = library;
    m->name = std::string(name_from_path(m->path));
    m->name = m->name.substr(0, m->name.find('.'));
    m->hmodule = k.memory().system_alloc(LDR_SIZE);
    k.memory()
        .heap_for(0x80040000)
        ->alloc_range(0x80040000, 0x801C0000, TRAMPOLINE_SIZE, 16, ALLOCATE_COMMIT, PROTECT_READ_WRITE, false,
                      &m->trampoline);
    s.kernel_modules.push_back(std::move(m));
    return *s.kernel_modules.back();
}

// KernelModule::GetProcAddressByOrdinal: a variable's address; a trampoline
// for a function with an implementation (0 for one without, as in Xenia)
uint32_t kernel_proc_address(Kernel &k, Module &m, uint16_t ordinal) {
    const Export *e = find_export(m.library, ordinal);
    if (!e) return 0;
    if (e->kind == ExportKind::Variable) return k.variable(e->name);
    auto it = m.trampolines.find(ordinal);
    if (it != m.trampolines.end()) return it->second;
    if (!Kernel::implemented(m.library, e->name)) {
        std::fprintf(stderr, "kernel: XexGetProcedureAddress: %s!%s has no implementation\n", m.library.c_str(),
                     e->name);
        return 0;
    }
    if (!m.trampoline || (m.trampoline_next + 1) * 8 > TRAMPOLINE_SIZE) {
        std::fprintf(stderr, "kernel: %s's trampolines are used up\n", m.name.c_str());
        return 0;
    }
    uint32_t address = m.trampoline + 8 * m.trampoline_next++;
    k.add_thunk(address, m.library, e->name);
    m.trampolines[ordinal] = address;
    return address;
}

// UserModule::GetProcAddressByOrdinal (XexModule::GetProcAddress): the XEX
// export table, else the PE export directory
uint32_t user_proc_address(Kernel &k, Module &m, uint16_t ordinal) {
    uint32_t security = m.xex_header + k.load32(m.xex_header + 0x10);
    if (uint32_t table = k.load32(security + 0x160)) {
        ordinal = uint16_t(ordinal - k.load32(table + 0x28)); // base
        if (ordinal >= k.load32(table + 0x24)) {              // count
            std::fprintf(stderr, "kernel: GetProcAddress(%03X): ordinal out of bounds\n", ordinal);
            return 0;
        }
        return k.load32(table + 0x2C + 4 * ordinal) + (k.load32(table + 0x20) << 16);
    }
    uint32_t directory;
    if (!opt_header(k, m.xex_header, 0x00E10402, directory)) return 0; // XEX_HEADER_EXPORTS_BY_NAME
    // a PE export directory: little-endian
    auto le32 = [&](uint32_t address) {
        const uint8_t *p = k.host(address);
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    uint32_t e = m.base_address + k.load32(directory);
    if (ordinal < le32(e + 0x14)) return m.base_address + le32(e + le32(e + 0x1C) + 4 * ordinal);
    return 0;
}

// XexModule::GetProcAddress(name)
uint32_t user_proc_address(Kernel &k, Module &m, std::string_view name) {
    uint32_t directory;
    if (!opt_header(k, m.xex_header, 0x00E10402, directory)) return 0;
    auto le32 = [&](uint32_t address) {
        const uint8_t *p = k.host(address);
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    uint32_t e = m.base_address + k.load32(directory);
    uint32_t functions = e + le32(e + 0x1C), names = e + le32(e + 0x20), ordinals = e + le32(e + 0x24);
    for (uint32_t i = 0; i < le32(e + 0x18); i++) {
        uint32_t fn_name = e + le32(names + 4 * i);
        uint16_t ordinal = uint16_t(k.host(ordinals + 2 * i)[0] | k.host(ordinals + 2 * i)[1] << 8);
        if (name == read_string(k, fn_name)) return m.base_address + le32(functions + 4 * ordinal);
    }
    return 0;
}

// KernelState::InitializeProcess
void initialize_process(Kernel &k, uint32_t process, uint8_t type, uint8_t priority_class, uint8_t default_priority,
                        uint8_t max_dynamic_priority) {
    uint8_t *p = k.host(process);
    p[0x18] = priority_class;
    p[0x19] = default_priority;
    p[0x1A] = max_dynamic_priority;
    k.store32(process + 0x04, process + 0x04); // the thread list, empty
    k.store32(process + 0x08, process + 0x04);
    k.store32(process + 0x0C, 60);        // quantum
    k.store32(process + 0x10, 0);         // the CLRDATAA pointer
    k.store32(process + 0x14, 0);         // thread count
    p[0x1B] = 0x06;                       // disable quantum decay
    k.store32(process + 0x1C, 16 * 1024); // kernel stack size
    k.store16(process + 0x2C, 0x80);      // TLS slot size
    p[0x2F] = type;
    k.store32(process + 0x54, process + 0x54); // a list, empty
    k.store32(process + 0x58, process + 0x54);
}

// KernelState::SetProcessTLSVars: the slots in the bitmap (1 free)
void set_process_tls(Kernel &k, uint32_t process, uint32_t slots, uint32_t data_size, uint32_t static_data) {
    uint32_t padded = (slots + 3) & 0xFFFFFFFC;
    k.store32(process + 0x24, data_size);
    k.store32(process + 0x28, data_size);
    k.store32(process + 0x20, static_data);
    k.store16(process + 0x2C, uint16_t(4 * padded));
    uint32_t words = padded / 32;
    for (uint32_t i = 0; i < words; i++) k.store32(process + 0x30 + 4 * i, 0xFFFFFFFF);
    if (((slots + 3) & 0x1C) != 0) k.store32(process + 0x30 + 4 * words, 0xFFFFFFFFu << (32 - ((slots + 3) & 0x1C)));
}

// KernelState::InitializeKernelGuestGlobals
uint32_t initialize_globals(Kernel &k) {
    uint32_t g = k.memory().system_alloc(GLOBALS_SIZE);
    initialize_process(k, g + IDLE_PROCESS, 0, 0, 0, 0);
    k.store32(g + IDLE_PROCESS + 0x0C, 0x7F); // quantum
    initialize_process(k, g + SYSTEM_PROCESS, 2, 2, 5, 9);
    set_process_tls(k, g + SYSTEM_PROCESS, 32, 0, 0);

    // the default object some types point at: a signalled synchronization
    // event, its wait list empty
    uint32_t object = g + DEFAULT_OBJECT;
    k.host(object)[0] = 1; // EventSynchronizationObject
    k.store32(object + 4, 1);
    k.store32(object + 8, object + 8);
    k.store32(object + 12, object + 8);

    // the object types' procedures: the pool's, and the thread's own (the
    // ones Xenia leaves to fatal errors or other families are 0 here: the
    // mutant's and timer's delete, the I/O completion's, device's, file's and
    // symbolic link's procedures)
    uint32_t procs = k.memory().system_alloc(4 * 8);
    uint32_t allocate_pool = procs, free_pool = procs + 8, allocate_thread = procs + 16, free_thread = procs + 24;
    k.add_thunk(allocate_pool, "kernel", "SimpleForwardAllocatePoolTypeWithTag");
    k.add_thunk(free_pool, "kernel", "SimpleForwardFreePool");
    k.add_thunk(allocate_thread, "kernel", "AllocateThread");
    k.add_thunk(free_thread, "kernel", "FreeThread");
    auto type = [&](ObjectType t, uint32_t tag, uint32_t allocate, uint32_t free, uint32_t size_or_object = 0) {
        uint32_t at = g + OBJECT_TYPE_SIZE * t;
        k.store32(at + 0x00, allocate);
        k.store32(at + 0x04, free);
        k.store32(at + 0x14, size_or_object);
        k.store32(at + 0x18, tag);
    };
    type(ExThreadObjectType, 0x65726854, allocate_thread, free_thread);           // 'Thre'
    type(ExEventObjectType, 0x76657645, allocate_pool, free_pool);                // 'Eve'
    type(ExMutantObjectType, 0x6174754D, allocate_pool, free_pool);               // 'Muta'
    type(ExSemaphoreObjectType, 0x616D6553, allocate_pool, free_pool);            // 'Sema'
    type(ExTimerObjectType, 0x656D6954, allocate_pool, free_pool);                // 'Time'
    type(IoCompletionObjectType, 0x706D6F43, allocate_pool, free_pool, object);   // 'Comp'
    type(IoDeviceObjectType, 0x69766544, allocate_pool, free_pool, object);       // 'Devi'
    type(IoFileObjectType, 0x656C6946, allocate_pool, free_pool, 0x38);           // 'File', the file object's size
    type(ObDirectoryObjectType, 0x65726944, allocate_pool, free_pool, object);    // 'Dire'
    type(ObSymbolicLinkObjectType, 0x626D7953, allocate_pool, free_pool, object); // 'Symb'

    // KeSetEvent on the zeroed event: signalled
    k.store32(g + USBD_BOOT_ENUMERATION_DONE_EVENT + 4, 1);
    return g;
}

// KernelState::UpdateKeTimestampBundle (Xenia's timer runs every millisecond)
void update_timestamp_bundle(Kernel &k) {
    k.store64(KeTimeStampBundle + 0x00, k.interrupt_time());
    k.store64(KeTimeStampBundle + 0x08, k.system_time());
    k.store32(KeTimeStampBundle + 0x10, k.uptime_ms());
}

void start_timer(Modules &s) {
    s.running = true;
    s.timer = std::thread([&s] {
#ifdef _WIN32
        // a millisecond, which Sleep does not give
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
#endif
        while (s.running) {
            update_timestamp_bundle(*s.k);
#ifdef _WIN32
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -10000; // 1 ms, relative
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, INFINITE);
                    continue;
                }
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#ifdef _WIN32
        if (timer) CloseHandle(timer);
#endif
    });
}

void copy_string(Kernel &k, uint32_t address, const std::string &s, uint32_t capacity) {
    size_t n = std::min<size_t>(s.size(), capacity - 1);
    std::memcpy(k.host(address), s.data(), n);
    k.host(address)[n] = 0;
}

} // namespace

void setup_modules(Kernel &k, const Xex &xex) {
    Memory &memory = k.memory();
    Modules &s = k.state<Modules>();
    s.k = &k;

    // KernelState(): the guest globals; a page Xenia keeps committed at
    // 0x8E030000
    s.globals = initialize_globals(k);
    for (uint32_t t = 0; t <= ObSymbolicLinkObjectType; t++)
        k.set_variable(object_type_names[t], s.globals + OBJECT_TYPE_SIZE * t);
    memory.heap_for(0x8E030000)
        ->alloc_fixed(0x8E030000, 0x10000, 0, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE);

    // xboxkrnl.exe: its trampolines, then the kernel's memory for the
    // variables (0x80070000-0x8026FFFF)
    add_kernel_module(k, s, "xe:\\xboxkrnl.exe", "xboxkrnl");
    uint32_t kernel_range;
    memory.heap_for(0x80070000)
        ->alloc_range(0x80070000, 0x80280000, 0x200000, 0x1000, ALLOCATE_COMMIT, PROTECT_READ_WRITE, false,
                      &kernel_range);

    // the video module's variables (RegisterVideoExports): the global
    // device's pointer at a physical block, the GPU's clock, the HSIO
    // calibration lock (a critical section, its spin count 10000)
    uint32_t video = memory.system_alloc(40, 32, true);
    k.store32(VdGlobalDevice, video);
    k.store32(VdGlobalXamDevice, 0);
    k.store32(VdGpuClockInMHz, 500);
    k.host(VdHSIOCalibrationLock)[0] = 1;                  // EventSynchronizationObject
    k.host(VdHSIOCalibrationLock)[1] = (10000 + 255) >> 8; // the spin count / 256
    k.store32(VdHSIOCalibrationLock + 0x04, 0);            // signal state
    k.store32(VdHSIOCalibrationLock + 0x10, 0xFFFFFFFF);   // lock count
    k.store32(VdHSIOCalibrationLock + 0x14, 0);            // recursion count
    k.store32(VdHSIOCalibrationLock + 0x18, 0);            // owning thread
    k.set_variable("VdGlobalDevice", VdGlobalDevice);
    k.set_variable("VdGlobalXamDevice", VdGlobalXamDevice);
    k.set_variable("VdGpuClockInMHz", VdGpuClockInMHz);
    k.set_variable("VdHSIOCalibrationLock", VdHSIOCalibrationLock);

    // no debug or certificate monitor
    k.store32(KeDebugMonitorData, 0);
    k.set_variable("KeDebugMonitorData", KeDebugMonitorData);
    k.store32(KeCertMonitorData, 0);
    k.set_variable("KeCertMonitorData", KeCertMonitorData);

    // XboxHardwareInfo: flags (0x20, bit 5: set once the storage is up),
    // then the CPU count
    k.store32(XboxHardwareInfo + 0, 0x20);
    k.host(XboxHardwareInfo + 4)[0] = 6;
    k.set_variable("XboxHardwareInfo", XboxHardwareInfo);

    // every region
    k.store16(ExConsoleGameRegion, 0xFFFF);
    k.set_variable("ExConsoleGameRegion", ExConsoleGameRegion);

    k.set_variable("XexExecutableModuleHandle", XexExecutableModuleHandle);
    k.set_variable("ExLoadedImageName", ExLoadedImageName);
    std::memset(k.host(ExLoadedCommandLine), 0, ExLoadedCommandLineSize);
    copy_string(k, ExLoadedCommandLine, "\"default.xex\"", ExLoadedCommandLineSize);
    k.set_variable("ExLoadedCommandLine", ExLoadedCommandLine);

    // XboxKrnlVersion: 2.0.1888.0 (Xenia's default build)
    k.store16(XboxKrnlVersion + 0, 2);
    k.store16(XboxKrnlVersion + 2, 0);
    k.store16(XboxKrnlVersion + 4, 1888);
    k.store16(XboxKrnlVersion + 6, 0);
    k.set_variable("XboxKrnlVersion", XboxKrnlVersion);

    // KeTimeStampBundle: interrupt time, system time, tick count, kept
    // current by a timer
    update_timestamp_bundle(k);
    k.store32(KeTimeStampBundle + 0x14, 0);
    k.set_variable("KeTimeStampBundle", KeTimeStampBundle);
    start_timer(s);

    // xam.xex
    add_kernel_module(k, s, "xe:\\xam.xex", "xam");

    // The executable (UserModule::LoadContinue): its header copied into
    // guest memory, its loader entry
    auto exe = std::make_unique<Module>();
    exe->path = "\\Device\\Cdrom0\\default.xex";
    exe->name = "default";
    exe->hmodule = memory.system_alloc(LDR_SIZE);
    exe->base_address = xex.base_address;
    exe->xex_header = memory.system_alloc(uint32_t(std::max<size_t>(xex.header.size(), 0x18)));
    if (!xex.header.empty()) std::memcpy(k.host(exe->xex_header), xex.header.data(), xex.header.size());
    k.store32(exe->hmodule + LDR_DLL_BASE, 0); // GetProcAddress reads this
    k.store32(exe->hmodule + LDR_XEX_HEADER_BASE, exe->xex_header);
    k.store32(exe->hmodule + LDR_FULL_IMAGE_SIZE, xex.image_size);
    k.store32(exe->hmodule + LDR_IMAGE_BASE, xex.base_address);
    k.store32(exe->hmodule + LDR_ENTRY_POINT, xex.entry_point);
    s.executable = exe.get();
    s.user_modules.push_back(std::move(exe));
    Module &m = *s.executable;

    // KernelState::SetExecutableModule: the title process (its TLS from the
    // header, its kernel stacks' size), the handle, the image's name and
    // the command line
    uint32_t title = s.globals + TITLE_PROCESS;
    initialize_process(k, title, 1, 10, 13, 17);
    uint32_t tls;
    if (opt_header(k, m.xex_header, xex_key::TLS_INFO, tls) && tls) {
        uint32_t slots = k.load32(tls), raw_data = k.load32(tls + 4), data_size = k.load32(tls + 8);
        k.store32(title + 0x20, raw_data);
        k.store32(title + 0x24, data_size);
        k.store32(title + 0x28, k.load32(tls + 12));
        k.store16(title + 0x2C, uint16_t(slots * 4));
        set_process_tls(k, title, slots, data_size, raw_data);
    }
    uint32_t stack_size = 0;
    opt_header(k, m.xex_header, xex_key::DEFAULT_STACK_SIZE, stack_size);
    if (stack_size) k.store32(title + 0x1C, std::max((stack_size + 4095) & 0xFFFFF000, 0x4000u));
    k.store32(XexExecutableModuleHandle, m.hmodule);
    copy_string(k, ExLoadedImageName, m.path, ExLoadedImageNameSize);
    copy_string(k, ExLoadedCommandLine, "\"" + m.name + ".xex\"", ExLoadedCommandLineSize);
}

namespace {

XF_EXPORT(xboxkrnl, XexCheckExecutablePrivilege) {
    // the privilege is a bit of the system flags (6: insecure sockets)
    uint32_t mask = 1u << (arg(k, c, 0) & 31);
    Modules &s = k.state<Modules>();
    if (!s.executable) return result(c, 0);
    uint32_t flags = 0;
    opt_header(k, s.executable->xex_header, xex_key::SYSTEM_FLAGS, flags);
    result(c, (flags & mask) ? 1 : 0);
}

XF_EXPORT(xboxkrnl, XexGetModuleHandle) {
    // (module_name, hmodule_ptr): no name is the executable; the handle is
    // not retained
    std::string name = read_string(k, arg(k, c, 0));
    uint32_t hmodule_ptr = arg(k, c, 1);
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = find_module(s, name);
    if (hmodule_ptr) k.store32(hmodule_ptr, m ? m->hmodule : 0);
    result(c, m ? 0 : X_ERROR_NOT_FOUND);
}

XF_EXPORT(xboxkrnl, XexGetModuleSection) {
    // (hmodule, name, data_ptr, size_ptr): a user module's resource
    uint32_t hmodule = arg(k, c, 0), data_ptr = arg(k, c, 2), size_ptr = arg(k, c, 3);
    std::string name = read_string(k, arg(k, c, 1));
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = module_from_hmodule(s, hmodule);
    if (!m) return result(c, X_STATUS_INVALID_HANDLE);
    if (m->is_kernel()) return result(c, X_STATUS_UNSUCCESSFUL);
    uint32_t resources;
    if (!opt_header(k, m->xex_header, xex_key::RESOURCE_INFO, resources)) return result(c, X_STATUS_NOT_FOUND);
    uint32_t count = (k.load32(resources) - 4) / 16;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t resource = resources + 4 + 16 * i;
        const char *res_name = reinterpret_cast<const char *>(k.host(resource));
        // the name up to its terminator, at most 8 characters
        if (name == std::string_view(res_name, strnlen(res_name, 8))) {
            k.store32(data_ptr, k.load32(resource + 8));
            k.store32(size_ptr, k.load32(resource + 12));
            return result(c, X_STATUS_SUCCESS);
        }
    }
    result(c, X_STATUS_NOT_FOUND);
}

XF_EXPORT(xboxkrnl, XexLoadImage) {
    // (module_name, module_flags, min_version, hmodule_ptr)
    std::string name = read_string(k, arg(k, c, 0));
    uint32_t hmodule_ptr = arg(k, c, 3);
    if (!hmodule_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    uint32_t status = X_STATUS_NO_SUCH_FILE, hmodule = 0;
    if (Module *m = find_module(s, name)) {
        hmodule = m->hmodule;
        status = X_STATUS_SUCCESS;
    } else {
        // a user module from the disc: the files family's to read
        std::fprintf(stderr, "kernel: XexLoadImage(%s): loading an executable is not implemented\n", name.c_str());
    }
    if (hmodule) k.store16(hmodule + LDR_LOAD_COUNT, uint16_t(k.load16(hmodule + LDR_LOAD_COUNT) + 1));
    k.store32(hmodule_ptr, hmodule);
    result(c, status);
}

XF_EXPORT(xboxkrnl, XexUnloadImage) {
    uint32_t hmodule = arg(k, c, 0);
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = module_from_hmodule(s, hmodule);
    if (!m) return result(c, X_STATUS_INVALID_HANDLE);
    // kernel modules stay
    if (!m->is_kernel()) {
        uint16_t count = uint16_t(k.load16(hmodule + LDR_LOAD_COUNT) - 1);
        k.store16(hmodule + LDR_LOAD_COUNT, count);
        // the last reference: unloaded (the kernel keeps the executable)
        if (count == 0 && m != s.executable) {
            k.memory().system_free(m->xex_header);
            k.memory().system_free(m->hmodule);
            std::erase_if(s.user_modules, [&](const std::unique_ptr<Module> &u) { return u.get() == m; });
        }
    }
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XexGetProcedureAddress) {
    // (hmodule, ordinal or name, out_function_ptr): no module is the
    // executable
    uint32_t hmodule = arg(k, c, 0), ordinal = arg(k, c, 1), out_ptr = arg(k, c, 2);
    bool by_name = (ordinal & 0xFFFF0000) != 0;
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = hmodule ? module_from_hmodule(s, hmodule) : s.executable;
    if (!m) return result(c, X_STATUS_INVALID_HANDLE);
    uint32_t address;
    std::string name = by_name ? read_string(k, ordinal) : std::string();
    if (by_name) {
        if (m->is_kernel()) {
            std::fprintf(stderr, "kernel: GetProcAddressByName is not implemented for kernel modules\n");
            address = 0;
        } else {
            address = user_proc_address(k, *m, name);
        }
    } else {
        address = m->is_kernel() ? kernel_proc_address(k, *m, uint16_t(ordinal))
                                 : user_proc_address(k, *m, uint16_t(ordinal));
    }
    if (out_ptr) k.store32(out_ptr, address);
    if (address) return result(c, X_STATUS_SUCCESS);
    if (by_name)
        std::fprintf(stderr, "kernel: XexGetProcedureAddress: export '%s' in '%s' not found\n", name.c_str(),
                     m->name.c_str());
    else
        std::fprintf(stderr, "kernel: XexGetProcedureAddress: ordinal %u (0x%X) in '%s' not found\n", ordinal, ordinal,
                     m->name.c_str());
    result(c, X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND);
}

XF_EXPORT(xboxkrnl, ExRegisterTitleTerminateNotification) {
    // (X_EX_TITLE_TERMINATE_REGISTRATION *: routine, priority; create)
    uint32_t reg = arg(k, c, 0), create = arg(k, c, 1);
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    uint32_t routine = k.load32(reg);
    if (create) {
        s.terminate_notifications.emplace_back(routine, k.load32(reg + 4));
    } else {
        auto it = std::find_if(s.terminate_notifications.begin(), s.terminate_notifications.end(),
                               [&](const auto &n) { return n.first == routine; });
        if (it != s.terminate_notifications.end()) s.terminate_notifications.erase(it);
    }
}

XF_EXPORT(xboxkrnl, RtlImageXexHeaderField) {
    // (xex_header, field): the field's value or guest address, 0 if absent
    uint32_t header = arg(k, c, 0), field = arg(k, c, 1);
    uint32_t value = 0;
    if (header) opt_header(k, header, field, value);
    result(c, value);
}

} // namespace
} // namespace xf::kernel
