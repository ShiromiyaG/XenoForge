/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's shared parts (xam.h) and its small exports: the notifications, the
// loader, the title's information, XamAlloc, the enumerators, the tasks,
// voice, sessions and the media verification; the user signed in or out,
// and the kernel exports XAM's own code calls. Translated from Xenia (BSD;
// see LICENSE-xenia): kernel_state.cc, xnotifylistener.cc, xenumerator.cc,
// xam_state.cc, profile_manager.cc (FindProfiles), xam_notify.cc,
// xam_info.cc, xam_module.cc, xam_enum.cc, xam_task.cc, xam_voice.cc,
// xam_user.cc (sessions) and xam_media.cc; checked against them.
//
// Where it differs from Xenia, and why:
//   - deferred overlapped requests run at once, on the calling thread
//     (Xenia runs them 25 ms later on a dispatch thread): the result is in
//     the XAM_OVERLAPPED, its event set or its routine queued, before the
//     export returns X_ERROR_IO_PENDING;
//   - XamLoaderLaunchTitle and XamLoaderTerminateTitle stop the run with
//     the reason (Xenia asks to be restarted with the launch data), ending
//     the calling thread;
//   - XamTaskSchedule hands out the task thread's handle (Xenia: 12345,
//     which XamTaskCloseHandle then fails to close).

#include "xam.h"

#include "xenoforge/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <regex>

namespace xf::kernel {
namespace {

struct XamState {
    std::mutex mutex;
    std::filesystem::path content_root;
    User user;
    std::atomic<bool> signed_out{false}; // XamUserLogon took the user out
    // under the objects' lock; each retained for good, as Xenia's (and so
    // never released after the objects are gone)
    std::vector<NotifyListener *> listeners;
    bool notified_startup = false, notified_live = false, notified_xmp = false;
    std::vector<uint8_t> launch_data;
};

XamState &xam(Kernel &k) { return k.state<XamState>(); }

} // namespace

// ---- overlapped requests ----

void complete_overlapped(Kernel &k, uint32_t ov, uint32_t result, uint32_t extended_error, uint32_t length) {
    k.store32(ov + xoverlapped::RESULT, result);
    k.store32(ov + xoverlapped::EXTENDED_ERROR, extended_error);
    k.store32(ov + xoverlapped::LENGTH, length);
    Objects &o = objects(k);
    uint32_t routine = k.load32(ov + xoverlapped::COMPLETION_ROUTINE);
    uint32_t event_handle = k.load32(ov + xoverlapped::EVENT);
    // the event stays unsignaled when there is a completion routine
    if (event_handle && !routine) {
        if (Ref<Event> e = o.lookup<Event>(event_handle)) {
            std::lock_guard<std::mutex> l(o.lock);
            e->set_signal_state(1);
            o.signaled(*e);
        } else {
            std::fprintf(stderr, "xam: an overlapped request's event 0x%08X is no event\n", event_handle);
        }
    }
    if (routine) {
        // queued to the thread that asked: routine(result, length, overlapped)
        if (Ref<Thread> t = o.lookup<Thread>(k.load32(ov + xoverlapped::CONTEXT))) {
            std::lock_guard<std::mutex> l(o.lock);
            o.queue_user_apc(*t, routine, result, length, ov);
        }
    }
}

namespace {
uint32_t current_thread_handle() {
    Thread *t = current_thread();
    return t ? t->handle() : 0;
}
} // namespace

void complete_overlapped_immediate_ex(Kernel &k, uint32_t ov, uint32_t result, uint32_t extended_error,
                                      uint32_t length) {
    k.store32(ov + xoverlapped::CONTEXT, current_thread_handle());
    complete_overlapped(k, ov, result, extended_error, length);
}

void complete_overlapped_immediate(Kernel &k, uint32_t ov, uint32_t result) {
    // some games read the length as the success: -1 when it failed
    complete_overlapped_immediate_ex(k, ov, result, result, result ? 0xFFFFFFFF : 0);
}

uint32_t complete_overlapped_deferred(Kernel &k, uint32_t ov, const OverlappedRun &run) {
    k.store32(ov + xoverlapped::RESULT, X_ERROR_IO_PENDING);
    k.store32(ov + xoverlapped::CONTEXT, current_thread_handle());
    Objects &o = objects(k);
    if (uint32_t event_handle = k.load32(ov + xoverlapped::EVENT)) {
        if (Ref<Event> e = o.lookup<Event>(event_handle)) {
            std::lock_guard<std::mutex> l(o.lock);
            e->set_signal_state(0);
        }
    }
    uint32_t extended_error = 0, length = 0;
    uint32_t result = run(extended_error, length);
    complete_overlapped(k, ov, result, extended_error, length);
    return X_ERROR_IO_PENDING;
}

uint32_t run_or_defer(Kernel &k, uint32_t ov, const OverlappedRun &run) {
    if (ov) return complete_overlapped_deferred(k, ov, run);
    uint32_t extended_error = 0, length = 0;
    return run(extended_error, length);
}

void set_last_error(Kernel &k, uint32_t error) {
    if (Thread *t = current_thread()) k.store32(t->kthread() + kthread::LAST_ERROR, error);
}

uint32_t last_error(Kernel &k) {
    Thread *t = current_thread();
    return t ? k.load32(t->kthread() + kthread::LAST_ERROR) : 0;
}

uint32_t call_export(Kernel &k, ppc::Context &c, const char *library, const char *name,
                     std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::fprintf(stderr, "xam: %s!%s, which XAM calls, is not implemented\n", library, name);
        return X_STATUS_NOT_IMPLEMENTED;
    }
    auto saved = std::make_unique<ppc::Context>(c);
    c.r[1] -= SCRATCH_SIZE + 112; // past the scratch, a minimal frame
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    fn(k, c);
    uint32_t r = uint32_t(c.r[3]);
    c = *saved;
    return r;
}

// ---- notifications ----

// XNotificationKey: the local id (16 bits), the version (9), the mask's
// bit (6)
void NotifyListener::enqueue(uint32_t id, uint32_t data) {
    uint32_t version = (id >> 16) & 0x1FF, mask_index = (id >> 25) & 0x3F;
    if (!(mask & (1ull << mask_index)) || version > max_version) return;
    queued.push_back({id, data});
    set_signal_state(1);
    owner.signaled(*this);
}

bool NotifyListener::dequeue(uint32_t match_id, uint32_t &id, uint32_t &data) {
    for (auto it = queued.begin(); it != queued.end(); ++it) {
        if (match_id && it->first != match_id) continue;
        id = it->first, data = it->second;
        queued.erase(it);
        if (queued.empty()) set_signal_state(0);
        return true;
    }
    return false;
}

void broadcast(Kernel &k, uint32_t id, uint32_t data) {
    Objects &o = objects(k);
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(o.lock);
    for (NotifyListener *listener : s.listeners) listener->enqueue(id, data);
}

namespace {

constexpr uint64_t NOTIFY_SYSTEM = 0x1, NOTIFY_LIVE = 0x2, NOTIFY_XMP = 0x20;

// xeXamNotifyCreateListener and KernelState::RegisterNotifyListener: the
// first listeners of each kind get what games expect at start
uint32_t create_listener(Kernel &k, uint64_t mask, uint32_t max_version) {
    Objects &o = objects(k);
    Ref<NotifyListener> l = o.create<NotifyListener>(GuestType::EventNotification, header::SIZE);
    if (!l) return 0;
    l->mask = mask;
    l->max_version = std::min<uint32_t>(max_version, 10);
    XamState &s = xam(k);
    std::lock_guard<std::mutex> lock(o.lock);
    l->retain();
    s.listeners.push_back(l.get());
    if (!s.notified_startup && (mask & NOTIFY_SYSTEM)) {
        s.notified_startup = true;
        l->enqueue(XN_SYS_UI, 0);
        l->enqueue(XN_SYS_SIGNINCHANGED, 1);
    }
    if (!s.notified_live && (mask & NOTIFY_LIVE)) {
        s.notified_live = true;
        l->enqueue(XN_LIVE_CONNECTIONCHANGED, 0x001510F1); // X_ONLINE_S_LOGON_DISCONNECTED
        l->enqueue(XN_LIVE_LINK_STATE_CHANGED, 0);
    }
    if (!s.notified_xmp && (mask & NOTIFY_XMP)) {
        s.notified_xmp = true;
        l->enqueue(XN_XMP_PLAYBACKCONTROLLERCHANGED, 1); // the title plays its own music
    }
    return l->handle();
}

} // namespace

// (mask (64 bits), max_version)
// The console's XAM makes the listener, finds the mask empty, drops it and
// answers 0 (xfxam); Xenia hands out a listener that no notification reaches.
XF_EXPORT(xam, XamNotifyCreateListener) {
    uint64_t mask = arg64(k, c, 0);
    result(c, mask ? create_listener(k, mask, arg(k, c, 1)) : 0);
}

// (mask (64 bits), is_system, max_version): the same listener (Xenia keeps
// is_system and never reads it)
XF_EXPORT(xam, XamNotifyCreateListenerInternal) {
    uint64_t mask = arg64(k, c, 0);
    result(c, mask ? create_listener(k, mask, arg(k, c, 2)) : 0); // an empty mask: 0, as above
}

// (listener, match_id, id_ptr, param_ptr): 1 and the next (or the matching)
// notification, else 0
XF_EXPORT(xam, XNotifyGetNext) {
    uint32_t handle = arg(k, c, 0), match_id = arg(k, c, 1), id_ptr = arg(k, c, 2), param_ptr = arg(k, c, 3);
    if (param_ptr) k.store32(param_ptr, 0);
    if (!id_ptr) return result(c, 0);
    k.store32(id_ptr, 0);
    Objects &o = objects(k);
    Ref<NotifyListener> l = o.lookup<NotifyListener>(handle);
    if (!l) return result(c, 0);
    uint32_t id = 0, data = 0;
    bool dequeued;
    {
        std::lock_guard<std::mutex> lock(o.lock);
        dequeued = l->dequeue(match_id, id, data);
    }
    k.store32(id_ptr, dequeued ? id : 0);
    if (param_ptr) k.store32(param_ptr, dequeued ? data : 0);
    result(c, dequeued ? 1 : 0);
}

XF_EXPORT(xam, XNotifyDelayUI) { result(c, 0); }
XF_EXPORT(xam, XNotifyPositionUI) {} // no notification UI to place

// ---- the user and the title ----

const User *user_at(Kernel &k, uint32_t index) {
    if (index >= 4 && index < USER_INDEX_LATEST) return nullptr;
    uint8_t slot = uint8_t(index);
    if (slot == USER_INDEX_NONE) return nullptr;
    if (slot == USER_INDEX_LATEST || slot == USER_INDEX_ANY) slot = 0;
    XamState &s = xam(k);
    return slot == 0 && !s.signed_out ? &s.user : nullptr;
}

const User *user_by_xuid(Kernel &k, uint64_t xuid) {
    XamState &s = xam(k);
    return xuid == s.user.xuid && !s.signed_out ? &s.user : nullptr;
}

const User &user_profile(Kernel &k) { return xam(k).user; }

bool set_signed_in(Kernel &k, bool signed_in) { return xam(k).signed_out.exchange(!signed_in) == signed_in; }

uint32_t title_id(Kernel &k) { return k.xex() ? k.xex()->execution.title_id : 0; }

std::filesystem::path content_root(Kernel &k) {
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    return s.content_root;
}

// The storage root (kernel.h): the cache partitions, the content root, and
// the user from it (ProfileManager::FindProfiles).
bool mount_storage(Kernel &k, const std::filesystem::path &root, std::string &error) {
    if (!mount_cache(k, root, error)) return false;
    std::filesystem::path content = root / "content";
    std::error_code ec;
    std::filesystem::create_directories(content, ec);
    if (ec) return error = "cannot create " + content.string() + ": " + ec.message(), false;
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.content_root = content;
    // a profile's directory is its XUID, with its package at
    // FFFE07D1/00010000/<XUID>
    static const std::regex xuid_name("[0-9A-F]{16}");
    std::vector<std::string> found;
    for (std::filesystem::directory_iterator it(content, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = vfs::guest_name(it->path().filename());
        if (!it->is_directory() || !std::regex_match(name, xuid_name) || name == "0000000000000000") continue;
        if (std::filesystem::exists(it->path() / "FFFE07D1" / "00010000" / name)) found.push_back(name);
    }
    std::sort(found.begin(), found.end());
    if (!found.empty()) s.user.xuid = std::stoull(found.front(), nullptr, 16);
    return true;
}

// ---- the loader (xam_info.cc, xam_module.cc) ----

namespace {
constexpr uint32_t MAX_LAUNCH_DATA_SIZE = 0x3FC;
} // namespace

void end_title(Kernel &k, const std::string &why) {
    std::fprintf(stderr, "xam: %s\n", why.c_str());
    if (!k.stopped.exchange(true)) k.stop_reason = why;
    throw ThreadExit{0};
}

XF_EXPORT(xam, XamLoaderGetLaunchDataSize) {
    uint32_t size_ptr = arg(k, c, 0);
    if (!size_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    k.store32(size_ptr, uint32_t(s.launch_data.size()));
    result(c, s.launch_data.empty() ? X_ERROR_NOT_FOUND : X_ERROR_SUCCESS);
}

XF_EXPORT(xam, XamLoaderGetLaunchData) {
    uint32_t buffer = arg(k, c, 0), size = arg(k, c, 1);
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    if (s.launch_data.empty()) return result(c, X_ERROR_NOT_FOUND);
    std::memcpy(k.host(buffer), s.launch_data.data(), std::min<size_t>(s.launch_data.size(), size));
    result(c, X_ERROR_SUCCESS);
}

XF_EXPORT(xam, XamLoaderSetLaunchData) {
    uint32_t data = arg(k, c, 0), size = std::min(arg(k, c, 1), MAX_LAUNCH_DATA_SIZE);
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.launch_data.assign(k.host(data), k.host(data) + size);
    result(c, 0);
}

// (name, flags): another executable, or the dashboard with no name
XF_EXPORT(xam, XamLoaderLaunchTitle) {
    uint32_t name = arg(k, c, 0);
    std::string path = name ? reinterpret_cast<const char *>(k.host(name)) : "";
    end_title(k, path.empty() ? "the title exits to the dashboard (XamLoaderLaunchTitle)"
                              : "the title launches " + path + " (XamLoaderLaunchTitle)");
}

XF_EXPORT(xam, XamLoaderTerminateTitle) { end_title(k, "the title exits to the dashboard (XamLoaderTerminateTitle)"); }

// ---- the title's information (xam_info.cc) ----

// (info_ptr): the executable's execution info, in its header in guest memory
XF_EXPORT(xam, XamGetExecutionId) {
    uint32_t info_ptr = arg(k, c, 0);
    uint32_t module = k.load32(k.variable("XexExecutableModuleHandle"));
    uint32_t header = module ? k.load32(module + 0x58) : 0;
    if (header) {
        uint32_t count = k.load32(header + 0x14);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t entry = header + 0x18 + 8 * i;
            if (k.load32(entry) != xex_key::EXECUTION_INFO) continue;
            if (info_ptr) k.store32(info_ptr, header + k.load32(entry + 4));
            return result(c, X_STATUS_SUCCESS);
        }
    }
    result(c, 0xC0000225); // X_STATUS_NOT_FOUND
}

// The console's XAM answers a constant, 0x20076000 (xfxam); Xenia answers 0
// on purpose, so that games take the oldest system's paths (its comment: a
// version too high may ask for what is not there, too low may break new
// games). Here the console's answer: a game written for the console reads it.
XF_EXPORT(xam, XamGetSystemVersion) { result(c, 0x20076000); }
XF_EXPORT(xam, XamGetCurrentTitleId) { result(c, title_id(k)); }

// the guide's custom actions: nothing to register with no guide (Xenia's
// stub, which returns nothing)
XF_EXPORT(xam, XCustomRegisterDynamicActions) {}

// ---- the disc's media verification (xam_media.cc): nothing ever fails ----

// (creation_flags)
XF_EXPORT(xam, XamMediaVerificationCreate) { result(c, X_ERROR_SUCCESS); }

// (r3, overlapped, callback): the callback queued to the calling thread as
// a user APC with no arguments (the overlapped untouched, as Xenia's)
XF_EXPORT(xam, XamMediaVerificationVerify) {
    uint32_t callback = arg(k, c, 2) & ~1u;
    if (callback) {
        if (Thread *t = current_thread()) {
            Objects &o = objects(k);
            std::lock_guard<std::mutex> l(o.lock);
            o.queue_user_apc(*t, callback, 0, 0, 0);
        }
    }
    result(c, X_ERROR_SUCCESS);
}

// (X_SECURITY_FAILURE_INFORMATION *: size, failed reads, failed hashes,
// blocks checked, total blocks, complete): none failed, complete
XF_EXPORT(xam, XamMediaVerificationFailedBlocks) {
    if (uint32_t info = arg(k, c, 0)) {
        if (k.load32(info) != 0x18) return result(c, X_ERROR_NOT_ENOUGH_MEMORY);
        for (uint32_t offset = 4; offset < 0x14; offset += 4) k.store32(info + offset, 0);
        k.store32(info + 0x14, 1);
    }
    result(c, X_ERROR_SUCCESS);
}

// (flags, size, out_ptr): from the system heap, as Xenia's
XF_EXPORT(xam, XamAlloc) {
    uint32_t size = arg(k, c, 1), out_ptr = arg(k, c, 2);
    uint32_t p = k.memory().system_alloc(size);
    if (out_ptr) k.store32(out_ptr, p);
    result(c, X_ERROR_SUCCESS);
}

XF_EXPORT(xam, XamFree) {
    if (uint32_t p = arg(k, c, 0)) k.memory().system_free(p);
    result(c, X_ERROR_SUCCESS);
}

// ---- enumerators (xam_enum.cc, xenumerator.cc) ----

Enumerator::~Enumerator() = default;

uint32_t Enumerator::write_items(Kernel &k, uint32_t buffer, uint32_t buffer_size, uint32_t &written) {
    size_t n = std::min<size_t>(count() - current, items_per_enumerate);
    if (!n) return X_ERROR_NO_MORE_FILES;
    if (n * item_size > buffer_size)
        std::fprintf(stderr, "xam: XamEnumerate: %zu items of %u bytes in a %u-byte buffer\n", n, item_size,
                     buffer_size);
    std::memcpy(k.host(buffer), items.data() + current * item_size, n * item_size);
    current += n;
    written = uint32_t(n);
    return X_ERROR_SUCCESS;
}

Ref<Enumerator> make_enumerator(Kernel &k, uint32_t user_index, uint32_t app_id, uint32_t open_message,
                                uint32_t close_message, uint32_t flags, uint32_t items_per_enumerate,
                                uint32_t item_size, uint32_t extra_size) {
    Objects &o = objects(k);
    uint32_t size = object_header::SIZE + 0x18 + extra_size;
    uint32_t memory = k.memory().system_alloc(size);
    if (!memory) return {};
    std::memset(k.host(memory), 0, size);
    Ref<Enumerator> e(new Enumerator(o));
    e->guest = memory + object_header::SIZE;
    e->allocated = true;
    e->items_per_enumerate = items_per_enumerate;
    e->item_size = item_size;
    // X_KENUMERATOR
    k.store32(e->guest + 0x00, app_id);
    k.store32(e->guest + 0x04, open_message);
    k.store32(e->guest + 0x08, close_message);
    k.store32(e->guest + 0x0C, user_index);
    k.store32(e->guest + 0x10, items_per_enumerate);
    k.store32(e->guest + 0x14, flags);
    std::lock_guard<std::mutex> l(o.lock);
    o.add_handle(e.get());
    return e;
}

// (handle, flags, buffer, buffer_length, items_returned, overlapped)
XF_EXPORT(xam, XamEnumerate) {
    uint32_t handle = arg(k, c, 0), buffer = arg(k, c, 2), size = arg(k, c, 3), returned_ptr = arg(k, c, 4);
    uint32_t ov = arg(k, c, 5);
    Ref<Enumerator> e = objects(k).lookup<Enumerator>(handle);
    if (!e) return result(c, X_ERROR_INVALID_HANDLE);
    auto run = [&](uint32_t &extended_error, uint32_t &length) {
        uint32_t count = 0;
        uint32_t r = buffer ? e->write_items(k, buffer, size, count) : X_ERROR_INVALID_PARAMETER;
        extended_error = hresult_from_win32(r);
        length = count;
        if (r && ov) r = X_ERROR_FUNCTION_FAILED;
        return r;
    };
    if (ov) return result(c, complete_overlapped_deferred(k, ov, run));
    uint32_t extended_error, count;
    uint32_t r = run(extended_error, count);
    if (returned_ptr) k.store32(returned_ptr, r == X_ERROR_SUCCESS ? count : 0);
    result(c, r);
}

// (user_index, app_id, open_message, close_message, extra_size,
// item_count, flags, out_handle): an enumerator of extra_size-byte items,
// none in it
XF_EXPORT(xam, XamCreateEnumeratorHandle) {
    uint32_t extra_size = arg(k, c, 4), out_handle = arg(k, c, 7);
    Ref<Enumerator> e = make_enumerator(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 6),
                                        arg(k, c, 5), extra_size, extra_size);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    if (out_handle) k.store32(out_handle, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// (handle, out_ptr): its X_KENUMERATOR, the reference the caller's until
// ObDereferenceObject
XF_EXPORT(xam, XamGetPrivateEnumStructureFromHandle) {
    uint32_t handle = arg(k, c, 0), out_ptr = arg(k, c, 1);
    Objects &o = objects(k);
    Ref<Enumerator> e = o.lookup<Enumerator>(handle);
    // a handle that is not an enumerator's: the console's XAM answers the Win32
    // ERROR_INVALID_HANDLE (it makes ObReferenceObjectByHandle's status one
    // with RtlNtStatusToDosError; xfxam); Xenia the NT status
    if (!e) return result(c, X_ERROR_INVALID_HANDLE);
    {
        std::lock_guard<std::mutex> l(o.lock);
        o.retain_handle(handle);
    }
    if (out_ptr) k.store32(out_ptr, e->guest);
    result(c, X_STATUS_SUCCESS);
}

// ---- tasks (xam_task.cc) ----

// (callback, message, task_args, handle_ptr): callback(message) on a new
// thread of the system process
XF_EXPORT(xam, XamTaskSchedule) {
    uint32_t callback = arg(k, c, 0), message = arg(k, c, 1), handle_ptr = arg(k, c, 3);
    Objects &o = objects(k);
    Thread::Params p;
    p.start_address = callback;
    p.start_context = message;
    p.process = o.system_process;
    uint32_t stack = k.xex() ? k.xex()->stack_size : 0;
    p.stack_size = std::max(0x4000u, (stack + 0xFFF) & 0xFFFFF000);
    uint32_t status;
    Ref<Thread> t = o.create_thread(p, status);
    if (!t) {
        std::fprintf(stderr, "xam: XamTaskSchedule: no thread (0x%08X)\n", status);
        return result(c, status);
    }
    if (handle_ptr) k.store32(handle_ptr, t->handle());
    if (!o.scheduler->start(*t)) std::fprintf(stderr, "xam: no host thread for task thread %u\n", t->id);
    result(c, X_STATUS_SUCCESS);
}

// (handle): NtClose, the error as the thread's last
XF_EXPORT(xam, XamTaskCloseHandle) {
    Objects &o = objects(k);
    bool closed;
    {
        std::lock_guard<std::mutex> l(o.lock);
        closed = o.release_handle(arg(k, c, 0));
    }
    if (!closed) set_last_error(k, X_ERROR_INVALID_HANDLE);
    result(c, closed ? 1 : 0);
}

XF_EXPORT(xam, XamTaskShouldExit) { result(c, 0); }

// ---- voice (xam_voice.cc): no headset ----

XF_EXPORT(xam, XamVoiceIsActiveProcess) { result(c, 0); }
XF_EXPORT(xam, XamVoiceCreate) {
    if (uint32_t out = arg(k, c, 2)) k.store32(out, 0);
    result(c, X_ERROR_ACCESS_DENIED);
}
XF_EXPORT(xam, XamVoiceClose) { result(c, 0); }
XF_EXPORT(xam, XamVoiceHeadsetPresent) { result(c, 0); }
XF_EXPORT(xam, XamVoiceSubmitPacket) { result(c, 0x800700AA); }

// ---- sessions (xam_user.cc): one stand-in, as Xenia's ----

constexpr uint32_t SESSION_HANDLE = 0xCAFEDEAD;

XF_EXPORT(xam, XamSessionCreateHandle) {
    if (uint32_t p = arg(k, c, 0)) k.store32(p, SESSION_HANDLE);
    result(c, X_ERROR_SUCCESS);
}

// A handle that is not the stand-in's is the console's ERROR_INVALID_HANDLE
// with the output cleared (xfxam: it asks ObReferenceObjectByHandle and maps
// its status); Xenia takes any handle.
XF_EXPORT(xam, XamSessionRefObjByHandle) {
    uint32_t handle = arg(k, c, 0), p = arg(k, c, 1);
    if (handle != SESSION_HANDLE) {
        if (p) k.store32(p, 0);
        return result(c, X_ERROR_INVALID_HANDLE);
    }
    if (p) k.store32(p, 0xDEADF00D); // what ObDereferenceObject ignores
    result(c, X_ERROR_SUCCESS);
}

// ---- strings ----

std::u16string guest_u16(Kernel &k, uint32_t address, size_t max) {
    std::u16string s;
    if (!address) return s;
    for (size_t i = 0; i < max; i++) {
        char16_t ch = char16_t(k.load16(address + uint32_t(2 * i)));
        if (!ch) break;
        s += ch;
    }
    return s;
}

void put_u16(Kernel &k, uint32_t address, std::u16string_view s, size_t count) {
    if (!count) return;
    size_t n = std::min(s.size(), count - 1);
    for (size_t i = 0; i < n; i++) k.store16(address + uint32_t(2 * i), uint16_t(s[i]));
    k.store16(address + uint32_t(2 * n), 0);
}

} // namespace xf::kernel
