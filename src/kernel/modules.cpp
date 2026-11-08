/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The modules family (plan step A4; docs/runtime.md): the kernel modules
// (xboxkrnl.exe, xam.xex) and the game's executables with their loader
// entries (X_LDR_DATA_TABLE_ENTRY, the HMODULE), their XEX headers in guest
// memory, the kernel's variables at Xenia's addresses, the kernel's guest
// globals (the object types, the processes), and the Xex* exports: the
// executable, and the DLLs the game loads from its disc (XexLoadImage) or
// imports from. Translated from Xenia (BSD; see LICENSE-xenia):
// xboxkrnl_module.cc (the variables), xboxkrnl_modules.cc (the exports),
// xboxkrnl_video.cc (the Vd* variables), kernel_state.cc (the globals, the
// executable's setup, KeTimeStampBundle, LoadUserModule,
// FinishLoadingUserModule, UnloadUserModule, OnThreadExecute and
// OnThreadExit), xmodule.cc, kernel_module.cc and user_module.cc (the
// modules), cpu/xex_module.cc (Load, LoadContinue, Unload, GetProcAddress);
// checked against them.
//
// Where it differs from Xenia, and why:
//   - a module is no kernel object (its loader entry's checksum, Xenia's
//     handle, stays 0), and a DLL's entry point gets its HMODULE (the loader
//     entry, as the console's loader passes it; Xenia passes its handle);
//   - the libraries a DLL XexLoadImage loads imports from, loaded with it,
//     have their entry points called with DLL_PROCESS_ATTACH on the calling
//     thread before its own, as NT's loader does (Xenia calls none for them
//     and leaves the attach to the next thread that starts); those the
//     executable imports from are attached as the main thread starts, as in
//     Xenia;
//   - a thread's DLL_THREAD_ATTACH and DETACH go to guest threads only (Xenia
//     also calls them on its host threads, the GPU's; on the console the
//     kernel's own threads get no DLL notifications);
//   - every DllMain call runs under the loader lock (NT's LdrpLoaderLock;
//     Modules::loader), recursive and owned by the calling thread: held
//     across XexLoadImage's lookup, load and DLL_PROCESS_ATTACH,
//     XexUnloadImage's DLL_PROCESS_DETACH and the removal from the list, a
//     thread's DLL_THREAD_ATTACH and DETACH, and XexGetModuleHandle's lookup.
//     A load waits for an unload in progress and then loads a fresh copy; no
//     DllMain gets the HMODULE of a module unloaded meanwhile. Xenia holds its
//     global critical region (also recursive) in OnThreadExecute,
//     OnThreadExit, UnloadUserModule and GetModule, but calls
//     FinishLoadingUserModule's DLL_PROCESS_ATTACH with it released. As on the
//     console, a DllMain that waits for another thread to start or end, or to
//     load or unload a module, waits forever: that thread waits for the lock;
//   - a thread's DLL_THREAD_DETACH runs before the thread is marked
//     terminated and taken off its process's list, as NT's LdrShutdownThread
//     runs before the thread's end (Xenia's XThread::Exit calls OnThreadExit
//     last). A DllMain that calls ExitThread in its DLL_THREAD_DETACH ends
//     its own call: the other DLLs still get theirs, and the thread's exit
//     code stays the one it ended with; one that does so in its
//     DLL_THREAD_ATTACH ends the thread there (the DLLs after it get no
//     THREAD_ATTACH, every DLL gets its THREAD_DETACH);
//   - XexUnloadImage never unloads the executable (Xenia unloads any user
//     module whose count reaches 0, the executable included, after which
//     XexGetModuleHandle("default.xex") fails); the imports from xbdm.xex are
//     bound as the other system libraries' (kernel.cpp; Xenia loads no xbdm
//     module unless its console_type is set, -1 by default, and leaves them
//     unresolved);
//   - the image's pages are read-write throughout (as the executable's), the
//     heap is not reset before a load (Xenia's ReadImage resets the page
//     table of the heap the image goes in, the executable's too), and an
//     unloaded module's header is freed (Xenia keeps it);
//   - no title update is applied to a module (none is to the executable
//     either);
//   - an ordinal whose slot in a module's export table is empty is not
//     exported (Xenia's GetProcAddress answers the image's base, base << 16,
//     for it): the console's XAM leaves 1,320 of its 2,890 slots empty;
//   - a system module (load_module: the console's own XAM, which a tool
//     reads from the player's disc) gets no DLL_THREAD_ATTACH or DETACH and
//     its entry is its loader's to run (Xenia loads none).
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

#include "files.h"
#include "objects.h"
#include "xenoforge/exports.h"
#include "xenoforge/kernel.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_NOT_FOUND = 0xC0000225;
constexpr uint32_t X_STATUS_INVALID_IMAGE_FORMAT = 0xC000007B;
constexpr uint32_t X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND = 0xC0000263;
constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x00000057;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x00000490;

// XEX_MODULE_DLL_MODULE, in the main header's module flags
constexpr uint32_t MODULE_DLL = 0x00000008;
// DllMain's reasons
constexpr uint32_t DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1, DLL_THREAD_ATTACH = 2, DLL_THREAD_DETACH = 3;

// X_LDR_DATA_TABLE_ENTRY (the HMODULE points at it)
constexpr uint32_t LDR_SIZE = 0x64;
constexpr uint32_t LDR_DLL_BASE = 0x18;
constexpr uint32_t LDR_IMAGE_BASE = 0x1C;
constexpr uint32_t LDR_FULL_IMAGE_SIZE = 0x38;
constexpr uint32_t LDR_ENTRY_POINT = 0x3C;
constexpr uint32_t LDR_LOAD_COUNT = 0x40; // 16 bits
constexpr uint32_t LDR_TIME_DATE_STAMP = 0x50;
constexpr uint32_t LDR_XEX_HEADER_BASE = 0x58;

// XEX_HEADER_CHECKSUM_TIMESTAMP: the checksum, then the time stamp
constexpr uint32_t XEX_HEADER_CHECKSUM_TIMESTAMP = 0x00018002;

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
    // which load this is: a module unloaded and another loaded in its place
    // (the same HMODULE, base or entry point) has another
    uint64_t serial = 0;
    // a user module's
    uint32_t xex_header = 0, base_address = 0, image_size = 0, entry_point = 0;
    bool dll = false;         // XEX_MODULE_DLL_MODULE: its entry point is a DllMain
    bool attached = false;    // its DllMain had DLL_PROCESS_ATTACH
    std::string code;         // its generated code, or why it has none (Kernel::add_module_code)
    std::unique_ptr<Xex> xex; // one the kernel loaded (the executable's is the caller's)
    // a system module load_module loaded (the console's own): its entry is
    // its loader's to run, and threads starting and ending do not call it
    bool system = false;
    // a kernel module's GetProcAddress trampolines
    uint32_t trampoline = 0, trampoline_next = 0;
    std::map<uint16_t, uint32_t> trampolines;

    bool is_kernel() const { return !library.empty(); }
};

// The loader lock (NT's LdrpLoaderLock, a critical section): recursive,
// owned by a host thread (a guest thread's, or a host caller's). Held across
// guest code (the DllMains), so a guest thread waiting for it waits on the
// host, outside the guest's waits; the report names its owner.
class LoaderLock {
  public:
    void lock() {
        std::unique_lock l(mutex_);
        std::thread::id self = std::this_thread::get_id();
        if (depth_ && owner_ == self) {
            depth_++;
            return;
        }
        waiting_++;
        released_.wait(l, [&] { return depth_ == 0; });
        waiting_--;
        owner_ = self, depth_ = 1;
        Thread *t = current_thread();
        owner_thread_ = t ? int64_t(t->id) : -1;
    }
    void unlock() {
        std::lock_guard l(mutex_);
        if (--depth_ == 0) {
            owner_ = {};
            released_.notify_one();
        }
    }
    // who holds it (a guest thread's id, -1 a host thread) and how many
    // wait; false when it is free
    bool held(int64_t &owner, uint32_t &waiting) {
        std::lock_guard l(mutex_);
        owner = owner_thread_, waiting = waiting_;
        return depth_ != 0;
    }

  private:
    std::mutex mutex_;
    std::condition_variable released_;
    std::thread::id owner_;
    uint32_t depth_ = 0, waiting_ = 0;
    int64_t owner_thread_ = -1;
};

struct Modules {
    Kernel *k = nullptr;
    // the loader lock: every DllMain call, the loads and unloads (see the
    // top); taken before `mutex`, never while it is held
    LoaderLock loader;
    // the lists' (short, never held while guest code runs); recursive: a
    // module's imports load the libraries it imports from while it loads
    std::recursive_mutex mutex;
    uint64_t next_serial = 1;
    std::vector<std::unique_ptr<Module>> kernel_modules, user_modules;
    Module *executable = nullptr;
    std::string executable_path = "game:\\default.xex";
    // modules loaded and not attached yet, in the order their loads ended (a
    // library before the one that imports from it)
    std::vector<Module *> loaded;
    std::vector<std::string> unloaded; // their paths, for the report
    uint32_t globals = 0;
    std::vector<std::pair<uint32_t, uint32_t>> terminate_notifications; // routine, priority
};

bool equal_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

// xe::utf8::find_name_from_guest_path: the last part of a path (either
// separator: boot's paths may come with the host's)
std::string_view name_from_path(std::string_view path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// find_base_guest_path: the directory part
std::string_view base_of_path(std::string_view path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

// find_base_name_from_guest_path: the name without its extension
std::string base_name(std::string_view path) {
    std::string_view name = name_from_path(path);
    return std::string(name.substr(0, name.find_last_of('.')));
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
// then the user modules, by the path the name resolves to (game: and d: are
// the game's device, as the file system's links make them; a bare name
// matches a module's file name or name)
Module *find_module(Kernel &k, Modules &s, std::string_view name) {
    if (name.empty()) return s.executable;
    if (equal_case(name, "kernel32.dll")) return nullptr; // some games ask for it
    for (auto &m : s.kernel_modules)
        if (matches(*m, name)) return m.get();
    std::string path(name);
    if (path.find(':') != std::string::npos || path.starts_with('\\')) path = file_system(k).resolve_links(path);
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
        // an empty slot exports nothing (Xenia's answer is the image's base)
        uint32_t offset = k.load32(table + 0x2C + 4 * ordinal);
        return offset ? offset + (k.load32(table + 0x20) << 16) : 0;
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

// KernelState::InitializeKernelGuestGlobals
uint32_t initialize_globals(Kernel &k) {
    uint32_t g = k.memory().system_alloc(GLOBALS_SIZE);
    initialize_process(k, g + IDLE_PROCESS, 0, 0, 0, 0);
    k.store32(g + IDLE_PROCESS + 0x0C, 0x7F); // quantum
    initialize_process(k, g + TITLE_PROCESS, 1, 10, 13, 17);
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

void copy_string(Kernel &k, uint32_t address, const std::string &s, uint32_t capacity) {
    size_t n = std::min<size_t>(s.size(), capacity - 1);
    std::memcpy(k.host(address), s.data(), n);
    k.host(address)[n] = 0;
}

// UserModule::LoadContinue: the header copied into guest memory, the loader
// entry (XModule's constructor allocates it; UserModule::Dump writes its time
// stamp)
std::unique_ptr<Module> make_user_module(Kernel &k, Modules &s, const std::string &path, const Xex &xex) {
    Memory &memory = k.memory();
    auto m = std::make_unique<Module>();
    m->serial = s.next_serial++;
    m->path = path;
    m->name = base_name(path);
    m->hmodule = memory.system_alloc(LDR_SIZE);
    m->base_address = xex.base_address;
    m->image_size = uint32_t(xex.image.size());
    m->entry_point = xex.entry_point;
    m->dll = (xex.module_flags & MODULE_DLL) != 0;
    m->xex_header = memory.system_alloc(uint32_t(std::max<size_t>(xex.header.size(), 0x18)));
    if (!xex.header.empty()) std::memcpy(k.host(m->xex_header), xex.header.data(), xex.header.size());
    k.store32(m->hmodule + LDR_DLL_BASE, 0); // GetProcAddress reads this
    k.store32(m->hmodule + LDR_XEX_HEADER_BASE, m->xex_header);
    k.store32(m->hmodule + LDR_FULL_IMAGE_SIZE, xex.image_size);
    k.store32(m->hmodule + LDR_IMAGE_BASE, xex.base_address);
    k.store32(m->hmodule + LDR_ENTRY_POINT, xex.entry_point);
    // the time stamp only: the checksum's field is Xenia's handle (0 here)
    uint32_t stamp;
    if (opt_header(k, m->xex_header, XEX_HEADER_CHECKSUM_TIMESTAMP, stamp))
        k.store32(m->hmodule + LDR_TIME_DATE_STAMP, k.load32(stamp + 4));
    return m;
}

// KernelState::LoadUserModule, UserModule::LoadFromFile and LoadContinue,
// XexModule::Load and LoadContinue: the file read through the file system,
// its image placed at its base, its module listed, its imports bound (the
// libraries it imports from loaded first, as Xenia's LoadContinue loads
// them). Its entry point is its caller's to run. nullptr, the reason logged,
// when it cannot be loaded (XexLoadImage answers X_STATUS_NO_SUCH_FILE for
// any reason, as Xenia's).
Module *load_user_module_data(Kernel &k, Modules &s, const std::string &path, const std::string &name,
                              const std::vector<uint8_t> &data);
Module *load_user_module(Kernel &k, Modules &s, std::string_view raw_name) {
    // a bare name is relative to the executable's directory
    std::string path(raw_name);
    if (name_from_path(raw_name) == raw_name && s.executable)
        path = std::string(base_of_path(s.executable->path)) + "\\" + path;
    for (auto &m : s.user_modules)
        if (matches(*m, path)) return m.get();

    vfs::FileSystem::Opened file;
    vfs::Action action;
    uint32_t status = file_system(k).open(nullptr, nullptr, path, vfs::Disposition::Open, vfs::GENERIC_READ, false,
                                          true, file, action);
    if (int32_t(status) < 0 || !file.stream) {
        std::fprintf(stderr, "kernel: XexLoadImage: no file %s (0x%08X)\n", path.c_str(), status);
        return nullptr;
    }
    std::vector<uint8_t> data(size_t(file.entry->size));
    size_t done = 0;
    status = file.stream->read(0, data.data(), data.size(), done);
    data.resize(done);
    std::string absolute = file.entry->device->mount_path + "\\" + file.entry->path; // Entry::absolute_path
    if (int32_t(status) < 0) {
        std::fprintf(stderr, "kernel: XexLoadImage: %s: read status 0x%08X\n", absolute.c_str(), status);
        return nullptr;
    }
    return load_user_module_data(k, s, absolute, base_name(absolute), data);
}

// UserModule::LoadFromMemory and LoadContinue, XexModule::Load and
// LoadContinue on an executable's file in data: its module made as
// load_user_module's, with that path and name
Module *load_user_module_data(Kernel &k, Modules &s, const std::string &path, const std::string &name,
                              const std::vector<uint8_t> &data) {
    auto xex = std::make_unique<Xex>();
    std::string error;
    if (!parse_xex(data, *xex, error) || !load_xex_image(data, *xex, error) || !k.map_image(*xex, error)) {
        std::fprintf(stderr, "kernel: XexLoadImage: %s: %s\n", path.empty() ? name.c_str() : path.c_str(),
                     error.c_str());
        return nullptr;
    }
    std::unique_ptr<Module> made = make_user_module(k, s, path, *xex);
    made->name = name;
    Module &m = *made;
    m.xex = std::move(xex);
    // its generated code, if the game library has the table made from this
    // image, before any of its code runs
    m.code = k.add_module_code(*m.xex);
    // listed before its imports are bound, as Xenia's: a library importing
    // back from it finds it
    s.user_modules.push_back(std::move(made));
    k.bind_imports(*m.xex);
    s.loaded.push_back(&m);
    return &m;
}

// KernelState::UnloadUserModule after the DllMain, UserModule::Unload: the
// module off the list, its thunks dropped, its pages released
// (XexModule::Unload), its loader entry and header freed
void unload_user_module(Kernel &k, Modules &s, Module &m) {
    if (m.xex) {
        k.remove_module_code(m.base_address);
        k.remove_thunks(m.base_address, m.base_address + m.image_size);
        if (Heap *heap = k.memory().heap_for(m.base_address)) heap->release(m.base_address);
    }
    k.memory().system_free(m.xex_header);
    k.memory().system_free(m.hmodule);
    s.unloaded.push_back(m.path.empty() ? m.name : m.path); // (one loaded from memory has a name only)
    std::erase(s.loaded, &m);
    std::erase_if(s.user_modules, [&](const std::unique_ptr<Module> &u) { return u.get() == &m; });
}

// DllMain(hinstance, reason, reserved) on the calling thread (Xenia's
// Processor::Execute), the loader lock held and `mutex` released: each one
// whose module is still listed (a DllMain before it, on this thread, may have
// unloaded it). A ThreadExit (ExitThread in a DllMain) leaves through here.
struct EntryCall {
    uint64_t serial;
    uint32_t entry, hmodule, reason;
};
EntryCall entry_call(const Module &m, uint32_t reason) { return {m.serial, m.entry_point, m.hmodule, reason}; }
Module *listed(Modules &s, uint64_t serial) {
    for (auto &m : s.user_modules)
        if (m->serial == serial) return m.get();
    return nullptr;
}
void run_entry(Kernel &k, Modules &s, ppc::Context &c, const EntryCall &e) {
    {
        std::lock_guard lock(s.mutex);
        if (!listed(s, e.serial)) return;
    }
    objects(k).call(c, e.entry, {e.hmodule, e.reason, 0});
}
void run_entries(Kernel &k, Modules &s, ppc::Context &c, const std::vector<EntryCall> &calls) {
    for (const EntryCall &e : calls) run_entry(k, s, c, e);
}

} // namespace

void setup_kernel(Kernel &k) {
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

    // KeTimeStampBundle: interrupt time, system time, tick count; the
    // scheduler's clock keeps it current (objects.cpp, every millisecond)
    k.store32(KeTimeStampBundle + 0x14, 0);
    k.set_variable("KeTimeStampBundle", KeTimeStampBundle);

    // xam.xex
    add_kernel_module(k, s, "xe:\\xam.xex", "xam");
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

uint32_t title_process(Kernel &k) { return k.state<Modules>().globals + TITLE_PROCESS; }
uint32_t system_process(Kernel &k) { return k.state<Modules>().globals + SYSTEM_PROCESS; }

void set_executable_path(Kernel &k, const std::string &path) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    s.executable_path = path;
}

void set_executable_module(Kernel &k, Modules &s, Module &m, bool reset);

void setup_modules(Kernel &k, const Xex &xex) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);

    // The executable (UserModule::LoadContinue): its path the file's
    // absolute one, as Xenia launches "game:\default.xex" (the disc's
    // \Device\Cdrom0 when nothing is mounted)
    vfs::FileSystem &fs = file_system(k);
    std::string path, target;
    if (!fs.find_link("game:", target)) {
        path = "\\Device\\Cdrom0\\" + std::string(name_from_path(s.executable_path));
    } else if (vfs::Entry *e = fs.resolve(s.executable_path)) {
        path = e->device->mount_path + "\\" + e->path;
    } else {
        path = fs.resolve_links(s.executable_path);
    }
    std::unique_ptr<Module> exe = make_user_module(k, s, path, xex);
    s.executable = exe.get();
    s.user_modules.push_back(std::move(exe));
    Module &m = *s.executable;
    m.code = k.add_module_code(xex);
    set_executable_module(k, s, m, true);
}

// KernelState::SetExecutableModule: the title process (its TLS from the
// header, its kernel stacks' size), the handle, the image's name and the
// command line. reset: the process made anew (the first title; a title
// started later, XexStartExecutable, leaves it as it is).
void set_executable_module(Kernel &k, Modules &s, Module &m, bool reset) {
    uint32_t title = s.globals + TITLE_PROCESS;
    if (reset) initialize_process(k, title, 1, 10, 13, 17);
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
    // (Xenia's GetModule takes its global lock: a module being unloaded is
    // waited for)
    std::lock_guard loader(s.loader);
    std::lock_guard lock(s.mutex);
    Module *m = find_module(k, s, name);
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

namespace {

// xeXexLoadImage (module_name, module_flags, min_version, hmodule_ptr): a
// module loaded already, or one load makes (from its file, or from memory);
// the load counted in its loader entry. From memory, a module found by the
// name already is a collision (no handle).
uint32_t xex_load_image(Kernel &k, ppc::Context &c, const std::string &name, uint32_t flags, uint32_t hmodule_ptr,
                        const std::function<Module *(Modules &)> &load, bool from_memory) {
    if (!hmodule_ptr) return X_ERROR_INVALID_PARAMETER;
    Modules &s = k.state<Modules>();
    // the lookup, the load, the DllMains and the count under the loader lock:
    // a module being unloaded is gone when it is looked up (a fresh copy is
    // loaded), and no thread starts or ends meanwhile
    std::lock_guard loader(s.loader);
    std::unique_lock lock(s.mutex);
    uint32_t status = X_STATUS_NO_SUCH_FILE, hmodule = 0;
    std::vector<EntryCall> attach;
    if (Module *m = find_module(k, s, name)) {
        if (from_memory) {
            k.store32(hmodule_ptr, 0);
            return X_STATUS_OBJECT_NAME_COLLISION;
        }
        hmodule = m->hmodule;
        status = X_STATUS_SUCCESS;
    } else {
        size_t first = s.loaded.size();
        if (Module *loaded = load(s)) {
            hmodule = loaded->hmodule;
            status = X_STATUS_SUCCESS;
            std::fprintf(stderr, "kernel: XexLoadImage%s(%s, flags 0x%X): %s at 0x%08X-0x%08X, entry 0x%08X%s\n",
                         from_memory ? "FromMemory" : "", name.c_str(), flags,
                         loaded->path.empty() ? loaded->name.c_str() : loaded->path.c_str(), loaded->base_address,
                         loaded->base_address + loaded->image_size, loaded->entry_point, loaded->dll ? " (a DLL)" : "");
        }
        // FinishLoadingUserModule's DllMain(DLL_PROCESS_ATTACH), on this
        // thread: the libraries it brought first (see the top)
        for (Module *m : std::vector<Module *>(s.loaded.begin() + first, s.loaded.end()))
            if (m->dll && m->entry_point && !m->attached) {
                m->attached = true;
                attach.push_back(entry_call(*m, DLL_PROCESS_ATTACH));
            }
        s.loaded.resize(first);
    }
    lock.unlock();
    run_entries(k, s, c, attach);
    lock.lock();
    if (hmodule) k.store16(hmodule + LDR_LOAD_COUNT, uint16_t(k.load16(hmodule + LDR_LOAD_COUNT) + 1));
    k.store32(hmodule_ptr, hmodule);
    return status;
}

// (module_name, module_flags, min_version, hmodule_ptr)
void xex_load_image_file(Kernel &k, ppc::Context &c) {
    std::string name = read_string(k, arg(k, c, 0));
    result(c, xex_load_image(
                  k, c, name, arg(k, c, 1), arg(k, c, 3), [&](Modules &s) { return load_user_module(k, s, name); },
                  false));
}

} // namespace

XF_EXPORT(xboxkrnl, XexLoadImage) { xex_load_image_file(k, c); }

// (name, hmodule_ptr, flags, min_version): the console's order, from its XAM's
// own launcher (XamLoaderLaunchTitle's thread calls it with the handle's
// pointer second and the version last). Xenia declares it as XexLoadImage
// (name, flags, min_version, hmodule_ptr), a sketchy alias that makes the
// console's call write through the version; the console wins.
XF_EXPORT(xboxkrnl, XexLoadExecutable) {
    std::string name = read_string(k, arg(k, c, 0));
    result(c, xex_load_image(
                  k, c, name, arg(k, c, 2), arg(k, c, 1), [&](Modules &s) { return load_user_module(k, s, name); },
                  false));
}

// (hmodule): the module XexLoadExecutable loaded, started as a title: its
// entry point on a thread of the title process with the executable's default
// stack, as Kernel::launch does the first one. Xenia has none (it starts the
// title itself); the console's XAM calls it when it launches a title, and the
// state it keeps for a running title is made after this.
XF_EXPORT(xboxkrnl, XexStartExecutable) {
    uint32_t hmodule = arg(k, c, 0);
    uint32_t entry = 0, stack = 0;
    {
        Modules &s = k.state<Modules>();
        std::lock_guard lock(s.mutex);
        Module *m = module_from_hmodule(s, hmodule);
        if (!m || m->is_kernel() || !m->entry_point) return result(c, X_STATUS_INVALID_HANDLE);
        entry = m->entry_point;
        stack = m->xex ? m->xex->stack_size : 0;
        // it is the executable from now on (XexUnloadTitleModules left none)
        if (s.executable != m) {
            s.executable = m;
            set_executable_module(k, s, *m, false);
        }
    }
    Objects &o = objects(k);
    Thread::Params p;
    p.stack_size = stack;
    p.start_address = entry;
    p.process = o.title_process;
    uint32_t status = X_STATUS_SUCCESS;
    Ref<Thread> t = o.create_thread(p, status);
    if (!t) return result(c, status);
    if (!o.scheduler->start(*t)) return result(c, X_STATUS_NO_MEMORY);
    result(c, X_STATUS_SUCCESS);
}

// (buffer, size, module_name, module_flags, min_version, hmodule_ptr): the
// executable's file in memory, its module named by the name's base (no
// directory, no extension; Xenia's LoadUserModuleFromMemory). A module of
// that base name loaded already is the one returned, counted once more (as
// Xenia's lookup finds it; Xenia also runs its DllMain's DLL_PROCESS_ATTACH
// again, which is not done here).
XF_EXPORT(xboxkrnl, XexLoadImageFromMemory) {
    uint32_t buffer = arg(k, c, 0), size = arg(k, c, 1);
    std::string name = read_string(k, arg(k, c, 2));
    std::string base = base_name(name);
    auto load = [&](Modules &s) -> Module * {
        for (auto &m : s.user_modules)
            if (matches(*m, base)) return m.get();
        std::vector<uint8_t> data(k.host(buffer), k.host(buffer) + size);
        return load_user_module_data(k, s, "", base, data);
    };
    result(c, xex_load_image(k, c, name, arg(k, c, 3), arg(k, c, 5), load, true));
}

XF_EXPORT(xboxkrnl, XexUnloadImage) {
    uint32_t hmodule = arg(k, c, 0);
    Modules &s = k.state<Modules>();
    // the count, the DllMain and the removal under the loader lock: no other
    // thread finds the module, loads it again or notifies it meanwhile
    // (Xenia's UnloadUserModule holds its global lock)
    std::lock_guard loader(s.loader);
    std::unique_lock lock(s.mutex);
    Module *m = module_from_hmodule(s, hmodule);
    if (!m) return result(c, X_STATUS_INVALID_HANDLE);
    // kernel modules stay
    if (!m->is_kernel()) {
        uint16_t count = uint16_t(k.load16(hmodule + LDR_LOAD_COUNT) - 1);
        k.store16(hmodule + LDR_LOAD_COUNT, count);
        // the last reference: DllMain(DLL_PROCESS_DETACH) on this thread,
        // then unloaded (the kernel keeps the executable; see the top)
        if (count == 0 && m != s.executable) {
            uint64_t serial = m->serial;
            std::vector<EntryCall> detach;
            if (m->dll && m->entry_point) detach.push_back(entry_call(*m, DLL_PROCESS_DETACH));
            lock.unlock();
            run_entries(k, s, c, detach);
            lock.lock();
            // (unless its DllMain, on this thread, loaded it again or
            // unloaded it itself)
            if ((m = listed(s, serial)) && k.load16(hmodule + LDR_LOAD_COUNT) == 0) {
                std::fprintf(stderr, "kernel: XexUnloadImage: %s unloaded\n", m->path.c_str());
                unload_user_module(k, s, *m);
            }
        }
    }
    result(c, X_STATUS_SUCCESS);
}

// (flags): every module the title holds unloaded, the last loaded first:
// DLL_PROCESS_DETACH to those that attached, then their pages, thunks and
// loader entries released whatever their load counts, and the executable's
// too (none is the executable until XexStartExecutable names the next).
// What stays is the system's: the kernel's modules and the ones a tool loaded
// as the console's own (load_module). Xenia has none (it ends the title by
// tearing the emulator down and starting again); the console's XAM calls it
// before it loads the next title. What the flags select is not known (the
// console's XAM passes them; they are not read here), and the title's threads
// are not ended: the caller's job, as XAM's (its launch ends them first).
XF_EXPORT(xboxkrnl, XexUnloadTitleModules) {
    Modules &s = k.state<Modules>();
    std::lock_guard loader(s.loader);
    std::unique_lock lock(s.mutex);
    std::vector<uint64_t> serials;
    std::vector<EntryCall> detach;
    for (auto it = s.user_modules.rbegin(); it != s.user_modules.rend(); ++it) {
        Module &m = **it;
        if (m.system) continue;
        serials.push_back(m.serial);
        if (m.dll && m.entry_point && m.attached) detach.push_back(entry_call(m, DLL_PROCESS_DETACH));
    }
    lock.unlock();
    run_entries(k, s, c, detach);
    lock.lock();
    for (uint64_t serial : serials) {
        Module *m = listed(s, serial);
        if (!m) continue; // a DllMain unloaded it
        std::fprintf(stderr, "kernel: XexUnloadTitleModules: %s unloaded\n", m->path.c_str());
        if (m == s.executable) {
            s.executable = nullptr;
            k.store32(XexExecutableModuleHandle, 0);
        }
        unload_user_module(k, s, *m);
    }
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XexLoadImageHeaders) {
    // (path: X_ANSI_STRING *, header, buffer_size): an executable's headers
    // read from its file, without loading it
    uint32_t header = arg(k, c, 1), buffer_size = arg(k, c, 2);
    if (buffer_size < 0x800) return result(c, X_STATUS_BUFFER_TOO_SMALL);
    // TranslateAnsiPath: trimmed
    std::string path = ansi_string(k, arg(k, c, 0));
    size_t first = path.find_first_not_of(" \t\r\n"), last = path.find_last_not_of(" \t\r\n");
    path = first == std::string::npos ? std::string() : path.substr(first, last - first + 1);
    vfs::FileSystem::Opened file;
    vfs::Action action;
    uint32_t status = file_system(k).open(nullptr, nullptr, path, vfs::Disposition::Open, vfs::GENERIC_READ, false,
                                          true, file, action);
    if (!file.stream) return result(c, status);
    size_t done = 0;
    // through the view no write watch protects (files.cpp's read_locked)
    status = file.stream->read(0, k.memory().translate_for_host_write(header), 0x800, done);
    k.memory().written(header, uint32_t(done));
    if (int32_t(status) < 0) return result(c, status);
    if (k.load32(header) != 0x58455832) return result(c, X_STATUS_INVALID_IMAGE_FORMAT); // 'XEX2'
    uint32_t header_size = k.load32(header + 8);
    if (header_size < 0x800 || header_size > 0x10000 || (header_size & 0x7FF) != 0) {
        status = X_STATUS_INVALID_IMAGE_FORMAT;
    } else if (header_size > buffer_size) {
        status = X_STATUS_BUFFER_TOO_SMALL;
    } else if (header_size > 0x800) {
        status =
            file.stream->read(0x800, k.memory().translate_for_host_write(header + 0x800), header_size - 0x800, done);
        k.memory().written(header + 0x800, uint32_t(done));
        if (int32_t(status) >= 0) status = X_STATUS_SUCCESS;
    } else {
        status = X_STATUS_SUCCESS;
    }
    result(c, status);
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

// XexModule::LoadContinue: a library not loaded yet is loaded (its entry
// point not run: XexLoadImage runs it, or the next thread to start). Its
// callers hold the loader lock (XexLoadImage) or run before any thread
// (Kernel::load).
uint32_t import_module(Kernel &k, const std::string &library) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = find_module(k, s, library);
    if (!m) m = load_user_module(k, s, library);
    if (!m) {
        std::fprintf(stderr, "kernel: %s is imported from and cannot be loaded\n", library.c_str());
        return 0;
    }
    return m->hmodule;
}

uint32_t module_export(Kernel &k, uint32_t hmodule, uint16_t ordinal) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    Module *m = module_from_hmodule(s, hmodule);
    if (!m) return 0;
    return m->is_kernel() ? kernel_proc_address(k, *m, ordinal) : user_proc_address(k, *m, ordinal);
}

uint32_t load_module(Kernel &k, const std::string &path, const std::vector<uint8_t> &data, std::string &error) {
    Modules &s = k.state<Modules>();
    std::lock_guard loader(s.loader);
    std::lock_guard lock(s.mutex);
    for (auto &m : s.user_modules)
        if (equal_case(m->path, path)) return error = path + " is loaded already", 0;
    size_t first = s.loaded.size();
    Module *m = load_user_module_data(k, s, path, base_name(path), data);
    if (!m) return error = "it cannot be loaded (the kernel's log says why)", 0;
    m->system = true;
    // (the list XexLoadImage attaches what its own load brought from)
    s.loaded.resize(first);
    k.store16(m->hmodule + LDR_LOAD_COUNT, 1);
    std::fprintf(stderr, "kernel: %s at 0x%08X-0x%08X, entry 0x%08X (a system module)\n", path.c_str(), m->base_address,
                 m->base_address + m->image_size, m->entry_point);
    return m->hmodule;
}

// OnThreadExecute: each DLL with an entry point, DLL_PROCESS_ATTACH the
// first time (the libraries the executable imports from, as the main thread
// starts), DLL_THREAD_ATTACH after; under the loader lock, as Xenia's under
// its global lock. A DllMain's ExitThread ends the thread here (body()).
void modules_thread_start(Kernel &k, ppc::Context &c) {
    Modules &s = k.state<Modules>();
    std::lock_guard loader(s.loader);
    std::vector<EntryCall> calls;
    {
        std::lock_guard lock(s.mutex);
        for (auto &m : s.user_modules) {
            if (!m->dll || !m->entry_point || m.get() == s.executable || m->system) continue;
            calls.push_back(entry_call(*m, m->attached ? DLL_THREAD_ATTACH : DLL_PROCESS_ATTACH));
            m->attached = true;
        }
        s.loaded.clear();
    }
    run_entries(k, s, c, calls);
}

// OnThreadExit: each DLL with an entry point, DLL_THREAD_DETACH, under the
// loader lock. A DllMain that calls ExitThread ends its own call; the rest
// still run, on the thread's context as it was (see the top).
void modules_thread_end(Kernel &k, ppc::Context &c) {
    Modules &s = k.state<Modules>();
    std::lock_guard loader(s.loader);
    std::vector<EntryCall> calls;
    {
        std::lock_guard lock(s.mutex);
        for (auto &m : s.user_modules)
            if (m->dll && m->entry_point && m.get() != s.executable && !m->system)
                calls.push_back(entry_call(*m, DLL_THREAD_DETACH));
    }
    if (calls.empty()) return;
    auto saved = std::make_unique<ppc::Context>(c);
    for (const EntryCall &e : calls) {
        try {
            run_entry(k, s, c, e);
        } catch (const ThreadExit &) {
            c = *saved; // Objects::call restores it only when the call returns
        }
    }
}

std::string modules_report(Kernel &k) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    std::string out = "modules:\n";
    for (auto &m : s.kernel_modules) {
        uint16_t count = k.load16(m->hmodule + LDR_LOAD_COUNT);
        if (count) out += "  " + m->path + ": the kernel's, loaded " + std::to_string(count) + " time(s)\n";
    }
    for (auto &m : s.user_modules) {
        char line[512];
        std::snprintf(line, sizeof(line), "  %s: 0x%08X-0x%08X, entry 0x%08X, %s, loaded %u time(s)%s%s\n",
                      (m->path.empty() ? m->name : m->path).c_str(), m->base_address, m->base_address + m->image_size,
                      m->entry_point,
                      m.get() == s.executable ? "the executable"
                      : m->dll                ? (m->attached ? "a DLL, attached" : "a DLL, not attached")
                                              : "loaded",
                      m->hmodule ? k.load16(m->hmodule + LDR_LOAD_COUNT) : 0, m->code.empty() ? "" : "; ",
                      m->code.c_str());
        out += line;
    }
    for (const std::string &path : s.unloaded) out += "  " + path + ": unloaded\n";
    // a DllMain still running, or one that waits forever (see the top)
    int64_t owner;
    uint32_t waiting;
    if (s.loader.held(owner, waiting)) {
        char line[128];
        std::string holder = owner >= 0 ? "thread " + std::to_string(owner) : "a host thread";
        std::snprintf(line, sizeof(line), "  the loader lock: held by %s, %u waiting for it\n", holder.c_str(),
                      waiting);
        out += line;
    }
    return out;
}

std::string module_at(Kernel &k, uint32_t address) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    for (auto &m : s.user_modules)
        if (address - m->base_address < m->image_size)
            return m->path.empty() ? m->name : std::string(name_from_path(m->path));
    return {};
}

bool module_at_try(Kernel &k, uint32_t address, std::string &name, uint32_t &base) {
    Modules &s = k.state<Modules>();
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock) return false;
    for (auto &m : s.user_modules)
        if (address - m->base_address < m->image_size) {
            name = m->path.empty() ? m->name : std::string(name_from_path(m->path));
            base = m->base_address;
            return true;
        }
    return false;
}

bool module_range(Kernel &k, uint32_t address, uint32_t &base, uint32_t &size) {
    Modules &s = k.state<Modules>();
    std::lock_guard lock(s.mutex);
    for (auto &m : s.user_modules)
        if (address - m->base_address < m->image_size) {
            base = m->base_address, size = m->image_size;
            return true;
        }
    return false;
}

} // namespace xf::kernel
