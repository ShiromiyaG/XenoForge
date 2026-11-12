/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// What the XAM exports share (plan step A4; docs/runtime.md, "XAM"): the
// overlapped requests' completion, the notifications, the signed-in user,
// the enumerators, the content root. Translated from Xenia (BSD; see
// LICENSE-xenia): kernel_state.cc (CompleteOverlapped*, the notification
// listeners), xnotifylistener.cc, xenumerator.cc, xam_state.cc and
// profile_manager.cc (the user), xbox.h (the codes); checked against them.

#pragma once

#include "files.h"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xf::kernel {

// ---- Win32 errors and HRESULTs (Xenia's xbox.h) ----

constexpr uint32_t X_ERROR_SUCCESS = 0x0;
constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 0x2;
constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 0x3;
constexpr uint32_t X_ERROR_ACCESS_DENIED = 0x5;
constexpr uint32_t X_ERROR_INVALID_HANDLE = 0x6;
constexpr uint32_t X_ERROR_NOT_ENOUGH_MEMORY = 0x8;
constexpr uint32_t X_ERROR_NO_MORE_FILES = 0x12;
constexpr uint32_t X_ERROR_NOT_SUPPORTED = 0x32;
constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
constexpr uint32_t X_ERROR_INVALID_NAME = 0x7B;
constexpr uint32_t X_ERROR_BAD_ARGUMENTS = 0xA0;
constexpr uint32_t X_ERROR_ALREADY_EXISTS = 0xB7;
constexpr uint32_t X_ERROR_IO_INCOMPLETE = 0x3E4;
constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
constexpr uint32_t X_ERROR_CANCELLED = 0x4C7;
constexpr uint32_t X_ERROR_NOT_LOGGED_ON = 0x4DD;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
constexpr uint32_t X_ERROR_FUNCTION_FAILED = 0x65B;
constexpr uint32_t X_ERROR_EMPTY = 0x10D2;

constexpr uint32_t hresult_from_win32(uint32_t x) {
    return int32_t(x) <= 0 ? x : ((x & 0xFFFF) | (7u << 16) | 0x80000000u);
}
constexpr uint32_t X_E_SUCCESS = 0;
constexpr uint32_t X_E_FAIL = 0x80004005;
constexpr uint32_t X_E_INVALIDARG = hresult_from_win32(X_ERROR_INVALID_PARAMETER);
constexpr uint32_t X_E_NOTFOUND = hresult_from_win32(X_ERROR_NOT_FOUND);
constexpr uint32_t X_E_NO_SUCH_USER = hresult_from_win32(X_ERROR_NO_SUCH_USER);
constexpr uint32_t X_E_ACCESS_DENIED = hresult_from_win32(X_ERROR_ACCESS_DENIED);
constexpr uint32_t X_E_DEVICE_NOT_CONNECTED = hresult_from_win32(X_ERROR_DEVICE_NOT_CONNECTED);

// ---- overlapped requests ----

// XAM_OVERLAPPED (0x1C)
namespace xoverlapped {
constexpr uint32_t RESULT = 0x0, LENGTH = 0x4, CONTEXT = 0x8, EVENT = 0xC, COMPLETION_ROUTINE = 0x10;
constexpr uint32_t COMPLETION_CONTEXT = 0x14, EXTENDED_ERROR = 0x18;
} // namespace xoverlapped

// KernelState::CompleteOverlappedEx: the result, extended error and length
// written, then the event set or, with a completion routine, the routine
// queued as a user APC to the thread named in the context
void complete_overlapped(Kernel &k, uint32_t overlapped, uint32_t result, uint32_t extended_error, uint32_t length);
// CompleteOverlappedImmediate(Ex): the current thread in the context first
// (Immediate: the length 0 on success, -1 on failure)
void complete_overlapped_immediate(Kernel &k, uint32_t overlapped, uint32_t result);
void complete_overlapped_immediate_ex(Kernel &k, uint32_t overlapped, uint32_t result, uint32_t extended_error,
                                      uint32_t length);
// The first half of a deferred completion: the request pending (result X_ERROR_IO_PENDING, the calling thread in
// the context, the event cleared). complete_overlapped finishes it, from any thread, whenever the work is done.
void begin_overlapped(Kernel &k, uint32_t overlapped);
// CompleteOverlappedDeferredEx, run at once here (Xenia runs it 25 ms later
// on its dispatch thread): returns X_ERROR_IO_PENDING
using OverlappedRun = std::function<uint32_t(uint32_t &extended_error, uint32_t &length)>;
uint32_t complete_overlapped_deferred(Kernel &k, uint32_t overlapped, const OverlappedRun &run);
// what a XAM export that takes an overlapped does with its work: at once
// without one, deferred with one
uint32_t run_or_defer(Kernel &k, uint32_t overlapped, const OverlappedRun &run);

// the calling thread's last error (X_KTHREAD's), as XThread::SetLastError
void set_last_error(Kernel &k, uint32_t error);
uint32_t last_error(Kernel &k);

// Another export run from a XAM export, as XAM's own code calls the kernel:
// on the caller's context, its stack lowered past a scratch area (the
// export's own frame, SCRATCH_SIZE bytes at export_scratch(c)) the
// arguments may point into, so nothing the callee runs (a user APC)
// reaches it; the registers restored after. Its r3
// (X_STATUS_NOT_IMPLEMENTED, logged, when there is no such export).
constexpr uint32_t SCRATCH_SIZE = 0x80;
inline uint32_t export_scratch(ppc::Context &c) { return uint32_t(c.r[1]) - SCRATCH_SIZE; }
uint32_t call_export(Kernel &k, ppc::Context &c, const char *library, const char *name,
                     std::initializer_list<uint64_t> args);

// ---- notifications (Xenia's XNotifyListener) ----

constexpr uint32_t XN_SYS_UI = 0x00000009;
constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
constexpr uint32_t XN_SYS_STORAGEDEVICESCHANGED = 0x0000000B;
constexpr uint32_t XN_SYS_PROFILESETTINGCHANGED = 0x0000000E;
constexpr uint32_t XN_LIVE_CONNECTIONCHANGED = 0x02000001;
constexpr uint32_t XN_LIVE_LINK_STATE_CHANGED = 0x02000003;
constexpr uint32_t XN_XMP_PLAYBACKCONTROLLERCHANGED = 0x0A000003;

// A listener: the notifications its mask and version take, under the
// objects' lock; a manual-reset event, signaled while any wait.
class NotifyListener : public Dispatcher {
  public:
    explicit NotifyListener(Objects &owner) : Dispatcher(owner, Type::NotifyListener) {}
    uint64_t mask = 0;
    uint32_t max_version = 0;
    std::vector<std::pair<uint32_t, uint32_t>> queued; // id, data
    // (lock held)
    void enqueue(uint32_t id, uint32_t data);
    bool dequeue(uint32_t match_id, uint32_t &id, uint32_t &data);
};

// to every listener (KernelState::BroadcastNotification)
void broadcast(Kernel &k, uint32_t id, uint32_t data);

// ---- the user ----

constexpr uint64_t DEFAULT_XUID = 0xB13EBABEBABEBABEull; // Xenia's default profile's
constexpr uint32_t USER_INDEX_LATEST = 0xFD, USER_INDEX_NONE = 0xFE, USER_INDEX_ANY = 0xFF;

struct User {
    uint64_t xuid = DEFAULT_XUID;
    std::string name = "User";
    uint32_t signin_state = 1; // signed in locally (offline)
    // its X_XAMACCOUNTINFO's flags, as Xenia's new account has them (none):
    // reserved (live enabled 0x20000000), cached (country 0xFF00,
    // subscription tier 0xF00000, language 0x3E000000)
    uint32_t reserved_flags = 0, cached_flags = 0;
};
// XamState::GetUserProfile(user_index): slot 0 holds the one user; "any"
// and "latest" are it; 4-0xFC and "none" are nobody; nobody while the user
// is signed out
const User *user_at(Kernel &k, uint32_t user_index);
const User *user_by_xuid(Kernel &k, uint64_t xuid);
// the user's profile, signed in or not (its account)
const User &user_profile(Kernel &k);
// ProfileManager::Login and Logout of the one user (slot 0): whether it
// changed
bool set_signed_in(Kernel &k, bool signed_in);

// the console's user settings XAM reads (xam_locale.cpp), through
// ExGetXConfigSetting: the country (XCONFIG_USER_COUNTRY) and language
// (XCONFIG_USER_LANGUAGE)
uint8_t config_country(Kernel &k);
uint32_t config_language(Kernel &k);
// An SPA (an XDBF file in guest memory) and from it the string table of its
// default language (XSTC, else English), by string id (xam_locale.cpp)
std::map<uint16_t, std::u16string> spa_strings(Kernel &k, uint32_t base, uint32_t size);

// The running title's achievement the user earns (XGIUserWriteAchievements):
// recorded in the user's GPD for the title, with the SPA's name and score, and
// logged. False when the title has no such achievement; true too when it was
// earned already (xam_user.cpp).
bool earn_achievement(Kernel &k, uint32_t user_index, uint32_t achievement_id);

// A toast for the host's handler (kernel.h, set_toast_handler); false, and nothing done, when there is none
bool toast(Kernel &k, const Toast &t);

// the running title's ID (its execution info)
uint32_t title_id(Kernel &k);

// ---- the content root ----

// The writable root in Xenia's storage layout (content/ and the cache
// partitions beside it); the user is the first Xenia profile found there
// (content/<XUID>/FFFE07D1/00010000/<XUID>), else Xenia's default XUID.
std::filesystem::path content_root(Kernel &k);

// ---- enumerators (Xenia's XEnumerator) ----

// An enumerator: X_KENUMERATOR in guest memory (app, messages, user,
// items per call, flags) and its items, fixed-size records written a batch
// a call by XamEnumerate.
class Enumerator : public Object {
  public:
    explicit Enumerator(Objects &owner) : Object(owner, Type::Other) {}
    ~Enumerator() override;
    uint32_t items_per_enumerate = 1, item_size = 0;
    std::vector<uint8_t> items; // item_size each
    size_t current = 0;
    // XStaticUntypedEnumerator::WriteItems
    // an enumerator whose items are more than fixed-size records (strings
    // behind pointers) writes them itself: from the item at `current`,
    // advancing it; the status, the count written
    std::function<uint32_t(Kernel &, uint32_t buffer, uint32_t buffer_size, uint32_t &count)> writer;
    uint32_t write_items(Kernel &k, uint32_t buffer, uint32_t buffer_size, uint32_t &count);
    size_t count() const { return item_size ? items.size() / item_size : 0; }
    uint8_t *append() {
        items.resize(items.size() + item_size);
        return items.data() + items.size() - item_size;
    }
};
// XEnumerator::Initialize: its X_KENUMERATOR (and extra bytes) allocated,
// and a handle
Ref<Enumerator> make_enumerator(Kernel &k, uint32_t user_index, uint32_t app_id, uint32_t open_message,
                                uint32_t close_message, uint32_t flags, uint32_t items_per_enumerate,
                                uint32_t item_size, uint32_t extra_size);

// a UTF-16 big-endian string from guest memory, and one written there
// (truncated to count characters with its terminator, as
// copy_and_swap_truncating)
std::u16string guest_u16(Kernel &k, uint32_t address, size_t max = 0x10000);
void put_u16(Kernel &k, uint32_t address, std::u16string_view s, size_t count);

// The run ends with the reason (the title exits, launches another, asks
// for another disc), from the calling thread (xam.cpp).
[[noreturn]] void end_title(Kernel &k, const std::string &why);
// The title ends: the terminate notifications run (modules.cpp), the exit
// recorded in Kernel::title_exit (a launch of `path`, "" for the dashboard,
// or a plain end), then end_title.
[[noreturn]] void exit_title(Kernel &k, bool launch, const std::string &path, uint32_t flags, uint32_t exit_code,
                             const std::string &why);
void run_title_terminate_notifications(Kernel &k, ppc::Context &c);
// The launch data XamLoaderSetLaunchData kept (xam.cpp)
std::vector<uint8_t> launch_data(Kernel &k);

// A console setting (ExGetXConfigSetting's, by category and id): its bytes,
// empty when there is none; a 32-bit one, or the fallback (xam_info.cpp).
std::vector<uint8_t> config_setting(Kernel &k, uint16_t category, uint16_t setting);
uint32_t config_setting32(Kernel &k, uint16_t category, uint16_t setting, uint32_t fallback);

// ---- Kinect (Xenia's NUIController, nui.h), with no device ----

struct NuiState {
    std::mutex mutex;
    uint32_t engaged_tracking_id = 0, tilt_callback = 0, init_flags = 0;
    uint64_t versions[2] = {}; // XAM's and the XTL's, as XNuiTitleInitialize gives them
    uint64_t session_id = 0;
};

} // namespace xf::kernel
