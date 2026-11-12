// The four kernel calls XAM's own launch of a title reaches and Xenia does not
// have (its table names them, nothing answers): KeSetPriorityClassThread,
// XexShimLock, ObTranslateSymbolicLink and IoSynchronousDeviceIoControlRequest.
// Each is written from what the console's own kernel does (its code read with
// xfkrnl's image, 2.0.16202; nothing of it is copied), and where the console's
// answer depends on what is not here, the answer is said so below.
//
//   - KeSetPriorityClassThread(thread, boost): the thread's priority class is 5,
//     or 13 for a boost; the difference from the class it had is added to its
//     base priority and the two bytes that follow its class, and a thread below
//     the real-time priorities (18) gets its quantum from the process again and
//     its priority set to the new base. Returns nothing. (Xenia: absent.)
//   - XexShimLock(): the console asks the hypervisor to lock the executable
//     loader's shims (hypervisor call 0x6F, selector 1) and turns its status
//     into the kernel's. Nothing here has a hypervisor to refuse, so it
//     answers STATUS_SUCCESS, the mapping of the hypervisor's 0.
//   - ObTranslateSymbolicLink(object attributes *, ansi string *): walks a name
//     through the object manager, following each symbolic link, and writes the
//     name it comes to (a device and the rest of the path) into the caller's
//     string, whose buffer it fills from length 0. The attributes' root
//     directory is 0 (the name must start with a backslash) or -3 (the
//     process's own device directory); any other root, an empty name or one
//     not rooted is STATUS_INVALID_PARAMETER; a name that leads nowhere is
//     STATUS_OBJECT_NAME_NOT_FOUND; an answer that does not fit the string is
//     STATUS_BUFFER_TOO_SMALL. The console's object manager is a tree of
//     directories; here the names are the VFS's links and mounts and the
//     namespace's flat list of named objects (the devices), which the walk
//     takes a prefix of, a component at a time. A link's target is taken as
//     the VFS keeps it (links over links, as ResolveSymbolicLink does).
//   - IoSynchronousDeviceIoControlRequest(code, device, in, in size, out, out
//     size, returned *): the console builds an I/O request packet for the
//     device's driver, calls it and waits. The devices here have no driver
//     (IoCreateDevice makes the object, nothing answers requests), which is
//     what NT's default handler answers: STATUS_INVALID_DEVICE_REQUEST with
//     nothing returned. What a real drive answers to a request (the console's
//     0x4D014 is a CD-ROM class request) is the driver's, not established. A
//     pointer that is not a device object faults on the console; it is
//     STATUS_INVALID_PARAMETER here.

#include "files.h"
#include "objects.h"

#include <algorithm>
#include <cstring>

namespace xf::kernel {

namespace {

constexpr uint32_t ROOT_DEVICES = 0xFFFFFFFD; // OBJECT_ATTRIBUTES' RootDirectory: -3
constexpr uint16_t IO_TYPE_DEVICE = 3;
constexpr uint32_t QUANTUM_ANOTHER_FLAG = 0xBA; // a thread's byte the class change clears
constexpr uint8_t REALTIME_PRIORITY = 0x12;

// Whether a name, or a prefix of it ending at a backslash, is the object namespace's: a
// named object (the devices), a VFS mount, or one of the VFS's links.
bool name_is_known(Kernel &k, const std::string &name) {
    Objects &o = objects(k);
    for (size_t end = name.size(); end > 0;) {
        std::string prefix = name.substr(0, end);
        {
            std::lock_guard<std::mutex> l(o.lock);
            if (uint32_t handle = o.find_name(prefix)) {
                o.release_handle(handle);
                return true;
            }
        }
        if (file_system(k).has_device(prefix)) return true;
        size_t slash = name.rfind('\\', end - 1);
        if (slash == std::string::npos || slash == 0) break;
        end = slash;
    }
    return false;
}

} // namespace

// (thread *, boost)
XF_EXPORT(xboxkrnl, KeSetPriorityClassThread) {
    uint32_t thread = arg(k, c, 0);
    if (!thread) return;
    uint8_t new_class = (arg(k, c, 1) & 0xFF) ? 13 : 5;
    std::lock_guard<std::mutex> l(objects(k).lock);
    uint8_t *g = k.host(thread);
    uint8_t delta = uint8_t(new_class - g[kthread::BASE_PRIORITY_COPY]);
    if (!delta) return;
    uint8_t priority = g[kthread::PRIORITY];
    g[kthread::BASE_PRIORITY_COPY] = new_class;
    g[kthread::BASE_PRIORITY] += delta;
    g[kthread::PROCESS_PRIORITY_CLASS] += delta;
    g[kthread::MAX_DYNAMIC_PRIORITY] += delta;
    if (priority < REALTIME_PRIORITY) {
        g[QUANTUM_ANOTHER_FLAG] = 0;
        k.store32(thread + kthread::QUANTUM, k.load32(k.load32(thread + kthread::PROCESS) + kprocess::QUANTUM));
        g[kthread::PRIORITY] = g[kthread::BASE_PRIORITY];
    }
}

// (): the hypervisor's lock of the shims, always granted here
XF_EXPORT(xboxkrnl, XexShimLock) { result(c, X_STATUS_SUCCESS); }

// (object attributes *, translated: ANSI_STRING *)
XF_EXPORT(xboxkrnl, ObTranslateSymbolicLink) {
    uint32_t attributes = arg(k, c, 0), out = arg(k, c, 1);
    if (!attributes || !out) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t root = k.load32(attributes + object_attributes::ROOT_DIRECTORY);
    uint32_t name_ptr = k.load32(attributes + object_attributes::NAME);
    k.store16(out, 0);
    if (!name_ptr) return result(c, X_STATUS_INVALID_PARAMETER);
    std::string name = ansi_string(k, name_ptr);
    if (root == 0) {
        if (name.empty() || name[0] != '\\') return result(c, X_STATUS_INVALID_PARAMETER);
    } else if (root != ROOT_DEVICES) {
        return result(c, X_STATUS_INVALID_PARAMETER);
    }

    vfs::FileSystem &fs = file_system(k);
    std::string resolved = fs.resolve_links(name);
    // (a link applied when the walk changed the name)
    bool linked = resolved != vfs::canonical(name);
    if (!linked && !name_is_known(k, resolved)) return result(c, X_STATUS_OBJECT_NAME_NOT_FOUND);
    if (resolved.size() > k.load16(out + 2)) return result(c, X_STATUS_BUFFER_TOO_SMALL);
    if (!resolved.empty()) std::memcpy(k.host(k.load32(out + 4)), resolved.data(), resolved.size());
    k.store16(out, uint16_t(resolved.size()));
    result(c, X_STATUS_SUCCESS);
}

// (control code, device object *, in *, in size, out *, out size, returned *)
XF_EXPORT(xboxkrnl, IoSynchronousDeviceIoControlRequest) {
    uint32_t device = arg(k, c, 1), returned = arg(k, c, 6);
    if (!device || k.load16(device) != IO_TYPE_DEVICE) return result(c, X_STATUS_INVALID_PARAMETER);
    if (returned) k.store32(returned, 0);
    result(c, X_STATUS_INVALID_DEVICE_REQUEST);
}

} // namespace xf::kernel
