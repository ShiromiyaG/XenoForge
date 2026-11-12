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
//   - XamTaskSchedule queues to a pool of worker threads and hands out the
//     task object's address with the low bit set, as the console's XAM does
//     (Xenia: a thread a task and the handle 12345, which XamTaskCloseHandle
//     then fails to close); see "tasks" below.

#include "xam.h"

#include "xenoforge/xex.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <regex>
#include <set>

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

void begin_overlapped(Kernel &k, uint32_t ov) {
    k.store32(ov + xoverlapped::RESULT, X_ERROR_IO_PENDING);
    k.store32(ov + xoverlapped::CONTEXT, current_thread_handle());
    Objects &o = objects(k);
    if (uint32_t event_handle = k.load32(ov + xoverlapped::EVENT)) {
        if (Ref<Event> e = o.lookup<Event>(event_handle)) {
            std::lock_guard<std::mutex> l(o.lock);
            e->set_signal_state(0);
        }
    }
}

uint32_t complete_overlapped_deferred(Kernel &k, uint32_t ov, const OverlappedRun &run) {
    begin_overlapped(k, ov);
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

// (a gamertag: 15 characters at most, as the console's accounts have)
void set_gamertag(Kernel &k, const std::string &name) {
    if (!name.empty()) xam(k).user.name = name.substr(0, 15);
}

void set_user(Kernel &k, uint64_t xuid, const std::string &name) {
    if (xuid) xam(k).user.xuid = xuid;
    set_gamertag(k, name);
}

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
    k.stop(why);
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

void set_launch_data(Kernel &k, const std::vector<uint8_t> &data) {
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.launch_data = data;
    if (s.launch_data.size() > MAX_LAUNCH_DATA_SIZE) s.launch_data.resize(MAX_LAUNCH_DATA_SIZE);
}

std::vector<uint8_t> launch_data(Kernel &k) {
    XamState &s = xam(k);
    std::lock_guard<std::mutex> l(s.mutex);
    return s.launch_data;
}

// The title ends: the terminate notifications run, then the exit is
// recorded (Kernel::title_exit) and the run stops. Xenia asks the user to
// restart with the launch data; here the host starts the next title.
void exit_title(Kernel &k, bool launch, const std::string &path, uint32_t flags, uint32_t exit_code,
                const std::string &why) {
    if (ppc::Context *c = Kernel::current_context()) run_title_terminate_notifications(k, *c);
    Kernel::TitleExit &e = k.title_exit;
    e.launch = launch, e.path = path, e.flags = flags, e.exit_code = exit_code, e.launch_data = launch_data(k);
    end_title(k, why);
}

// (name, flags): another executable, or the dashboard with no name
XF_EXPORT(xam, XamLoaderLaunchTitle) {
    uint32_t name = arg(k, c, 0), flags = arg(k, c, 1);
    std::string path = name ? reinterpret_cast<const char *>(k.host(name)) : "";
    exit_title(k, true, path, flags, 0,
               path.empty() ? "the title exits to the dashboard (XamLoaderLaunchTitle)"
                            : "the title launches " + path + " (XamLoaderLaunchTitle)");
}

XF_EXPORT(xam, XamLoaderTerminateTitle) {
    exit_title(k, false, "", 0, 0, "the title exits to the dashboard (XamLoaderTerminateTitle)");
}

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
    if (writer) return writer(k, buffer, buffer_size, written);
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
//
// The console's XAM does not make a thread for a task (xfxam): the call
// enters a critical region, makes a task object in XAM's heap, puts it on a
// work queue under a lock, resets the task's event and releases a semaphore
// that wakes one of the worker threads XAM's start made (eight threads, six of
// them the workers that take tasks, each with a 64 KB stack), and answers 0;
// the handle it writes is the task object's address with the low bit set. The
// worker takes the task, runs callback(message) with its thread's base
// priority lowered, sets the task's event, and the task's reference count (a
// word at +4 of the object) is what XamTaskCloseHandle decrements: the call
// answers the count it leaves, and the object goes with the last reference.
// XamTaskShouldExit takes no argument (the worker finds its task through
// its thread's TLS) and faults when the caller is no worker.
//
// Here: the same pool, six workers with the console's stack size, started
// with the first task, kernel threads in the system process whose body is host
// code calling the guest callback; a queue of tasks, a semaphore a task each,
// no separate dispatcher thread (the console's thread that hands tasks to
// idle workers) and no affinities or priorities. The task object is a
// 0x50-byte block in the system heap, the count at +4 as the console's;
// the other fields (state, callback, message) are ours. A caller that is
// no worker gets 0 from XamTaskShouldExit, not a fault. Xenia runs each
// task on a thread of its own and writes a fixed handle (12345).

namespace {

class XamTasks {
  public:
    static constexpr unsigned WORKERS = 6;
    static constexpr uint32_t STACK = 0x10000, TASK_SIZE = 0x50;
    // the task object's words: +4 is the console's reference count
    static constexpr uint32_t STATE = 0x00, REFS = 0x04, CALLBACK = 0x08, MESSAGE = 0x0C, ARGS = 0x10;
    enum : uint32_t { QUEUED, RUNNING, DONE };

    // the task, queued: its handle (address | 1) for the caller; 0 when it could not be
    uint32_t schedule(Kernel &kernel, uint32_t callback, uint32_t message, uint32_t args) {
        start(kernel);
        if (!running.load()) return 0;
        uint32_t task = k->memory().system_alloc(TASK_SIZE);
        if (!task) return 0;
        std::memset(k->host(task), 0, TASK_SIZE);
        k->store32(task + REFS, 2); // the caller's handle and the queue's
        k->store32(task + CALLBACK, callback);
        k->store32(task + MESSAGE, message);
        k->store32(task + ARGS, args);
        {
            std::lock_guard<std::mutex> l(mutex);
            tasks.insert(task);
            queue.push_back(task);
        }
        release_work(1);
        return task | 1;
    }

    // XamTaskCloseHandle: the count the task is left with, or -1 for a handle that is not one's
    // (the console reads whatever the address holds)
    uint32_t close(uint32_t handle) {
        uint32_t task = handle & ~1u;
        std::lock_guard<std::mutex> l(mutex);
        if (!(handle & 1) || !tasks.count(task)) return 0xFFFFFFFF;
        return unref(task);
    }

    // XamTaskShouldExit: the calling worker's task is asked to end (the kernel is ending)
    bool should_exit() { return !running.load(); }

    // the kernel ends: the workers stop after the task they run, and are joined if they do
    void stop() {
        if (!k) return;
        Objects &o = objects(*k);
        if (!running.exchange(false)) return;
        release_work(WORKERS);
        // a worker in a callback that never returns cannot be joined (the host scheduler's
        // threads are detached): give them a moment, and leave those
        {
            std::unique_lock<std::mutex> l(mutex);
            exited.wait_for(l, std::chrono::seconds(2), [&] { return finished == started_count; });
        }
        bool all_back = finished == started_count;
        if (all_back) {
            for (size_t i = 0; i < threads.size(); i++)
                if (started_flags[i]) o.scheduler->join(*threads[i]);
        } else {
            std::fprintf(stderr, "xam: %u task workers still run a task as the kernel ends\n",
                         started_count - finished);
        }
        {
            std::lock_guard<std::mutex> l(o.lock);
            for (auto &t : threads)
                if (t && t->handle()) o.release_handle(t->handle());
            if (work && work->handle()) o.release_handle(work->handle());
        }
        // after the lock (the last reference deletes); a worker that never came back keeps its
        // object, which is leaked as the thread is
        if (all_back) {
            threads.clear();
            work = {};
        } else {
            new std::vector<Ref<Thread>>(std::move(threads));
            new Ref<Semaphore>(std::move(work));
        }
    }

  private:
    Kernel *k = nullptr;
    std::once_flag once;
    std::mutex mutex; // the queue and the task set; never held around the objects' lock
    std::condition_variable exited;
    std::set<uint32_t> tasks;
    std::deque<uint32_t> queue;
    Ref<Semaphore> work;
    std::vector<Ref<Thread>> threads;
    std::vector<bool> started_flags;
    unsigned started_count = 0, finished = 0;
    std::atomic<bool> running{false};

    // the count after one reference less; the object goes with the last (mutex held)
    uint32_t unref(uint32_t task) {
        uint32_t refs = k->load32(task + REFS) - 1;
        k->store32(task + REFS, refs);
        if (!refs) {
            tasks.erase(task);
            k->memory().system_free(task);
        }
        return refs;
    }

    void release_work(int32_t count) {
        Objects &o = objects(*k);
        std::lock_guard<std::mutex> l(o.lock);
        work->set_signal_state(work->signal_state() + count);
        o.signaled(*work);
    }

    void start(Kernel &kernel) {
        std::call_once(once, [&] {
            k = &kernel;
            Objects &o = objects(kernel);
            work = o.create<Semaphore>(GuestType::Semaphore, ksemaphore::SIZE);
            if (!work) return;
            work->set_signal_state(0);
            k->store32(work->guest + ksemaphore::LIMIT, 0x7FFFFFFF);
            uint32_t status = X_STATUS_NO_MEMORY;
            for (unsigned i = 0; i < WORKERS; i++) {
                Thread::Params p;
                p.stack_size = STACK;
                p.process = o.system_process;
                p.host_arg = this;
                p.host = [](Thread &self, void *v) { static_cast<XamTasks *>(v)->loop(self); };
                p.name = "XAM task worker";
                Ref<Thread> t = o.create_thread(p, status);
                if (!t) {
                    std::fprintf(stderr, "xam: a task worker was not made (0x%08X)\n", status);
                    break;
                }
                threads.push_back(t);
                started_flags.push_back(false);
            }
            if (threads.empty()) return;
            running = true;
            for (size_t i = 0; i < threads.size(); i++) {
                {
                    std::lock_guard<std::mutex> l(mutex);
                    started_count++; // counted before it runs, so a stop waits for it
                }
                started_flags[i] = o.scheduler->start(*threads[i]);
                if (!started_flags[i]) {
                    std::lock_guard<std::mutex> l(mutex);
                    started_count--;
                    std::fprintf(stderr, "xam: no host thread for a task worker\n");
                }
            }
        });
    }

    void loop(Thread &self) {
        Objects &o = objects(*k);
        Dispatcher *const works[] = {work.get()};
        while (running.load()) {
            o.wait(self, works, false, false, Scheduler::NEVER);
            uint32_t task = 0;
            {
                std::lock_guard<std::mutex> l(mutex);
                if (!queue.empty()) task = queue.front(), queue.pop_front();
            }
            if (!task) continue;
            k->store32(task + STATE, RUNNING);
            try {
                o.call(*self.context, k->load32(task + CALLBACK), {k->load32(task + MESSAGE)});
            } catch (const ThreadExit &) {
                // an ExitThread in a task ends the task, not the worker
                self.context->r[1] = self.stack_base - (64 + 112);
            }
            k->store32(task + STATE, DONE);
            std::lock_guard<std::mutex> l(mutex);
            unref(task); // the queue's reference
        }
        std::lock_guard<std::mutex> l(mutex);
        finished++;
        exited.notify_all();
    }
};

} // namespace

void shutdown_xam_tasks(Kernel &k) { k.state<XamTasks>().stop(); }

// (callback, message, task_args, handle_ptr): callback(message) on a worker
// thread, the task's handle through the pointer; 0 when it is queued
XF_EXPORT(xam, XamTaskSchedule) {
    uint32_t handle = k.state<XamTasks>().schedule(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2));
    if (!handle) return result(c, X_STATUS_NO_MEMORY);
    if (uint32_t handle_ptr = arg(k, c, 3)) k.store32(handle_ptr, handle);
    result(c, X_STATUS_SUCCESS);
}

// (handle): one reference of the task less: the count it leaves; a handle
// that is no task's is ERROR_INVALID_HANDLE as the thread's last error
XF_EXPORT(xam, XamTaskCloseHandle) {
    uint32_t refs = k.state<XamTasks>().close(arg(k, c, 0));
    if (refs == 0xFFFFFFFF) set_last_error(k, X_ERROR_INVALID_HANDLE);
    result(c, refs);
}

// (): whether the task this worker runs should end
XF_EXPORT(xam, XamTaskShouldExit) { result(c, k.state<XamTasks>().should_exit() ? 1 : 0); }

// (the voice calls are in xam_voice.cpp)

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
