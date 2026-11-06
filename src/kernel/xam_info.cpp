/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's system queries (plan step A4; docs/runtime.md, "XAM"): the
// dashboard's context and app information, what a title ID is, the system's
// version and features, the console's settings as XAM reads them (audio
// flags, IPTV), the Live hive and XAuth offline, network storage,
// notifications broadcast and queued for the guide's UI, and the stubs Xenia
// keeps for devices that are never there (video capabilities, the
// microphone array, the media objects, the media verification's close and
// inject). Translated from Xenia (BSD; see LICENSE-xenia): xam_info.cc,
// title_id_utils.h, xam_notify.cc, xam_ui.cc (XNotifyQueueUI), xam_video.cc,
// xam_voice.cc, xam_media.cc, xam_enum.cc, xam_net.cc (XAuth), xam_user.cc
// (network storage), xam_state.cc, kernel_state.h; checked against them.
// Where it differs, and why:
//   - XamGetCachedTitleName gives an empty name: no title name is read from
//     the executable's SPA here (Xenia takes it from its game database);
//   - XamIptvGetServiceName's buffer is from the system heap, made at the
//     first call (Xenia allocates it at 0x80D20000 as XAM starts);
//   - XamFeatureEnabled reads feature n at bit n - 1 of the table, as the
//     console's XAM does (xfxam, 2.0.16202: 1-32 and 39 on, 0 off); Xenia
//     tests bit n of the table's low 36 bits (0-31 on, 39 off) and throws
//     past them;
//   - XamNetworkStorageHasUserEnabledStorage reads no user preference (the
//     kernel keeps none), which leaves the privilege check, always denied
//     offline: never enabled, as Xenia answers for a profile that never set it;
//   - XNotifyQueueUI logs the text (there is no notification UI to show it).

#include "xam.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace xf::kernel {

// ---- the console's settings, through ExGetXConfigSetting ----

std::vector<uint8_t> config_setting(Kernel &k, uint16_t category, uint16_t setting) {
    static const ExportFn get = find_export_fn("xboxkrnl", "ExGetXConfigSetting");
    if (!get) return {};
    constexpr uint32_t MAX = 0x200; // the largest setting is 256 bytes
    uint32_t scratch = k.memory().system_alloc(MAX + 4);
    if (!scratch) return {};
    ppc::Context x{};
    k.attach(x);
    x.r[3] = category, x.r[4] = setting, x.r[5] = scratch, x.r[6] = MAX, x.r[7] = scratch + MAX;
    get(k, x);
    std::vector<uint8_t> out;
    if (uint32_t(x.r[3]) == X_STATUS_SUCCESS) {
        uint16_t size = std::min<uint16_t>(k.load16(scratch + MAX), MAX);
        out.assign(k.host(scratch), k.host(scratch) + size);
    }
    k.memory().system_free(scratch);
    return out;
}

uint32_t config_setting32(Kernel &k, uint16_t category, uint16_t setting, uint32_t fallback) {
    std::vector<uint8_t> v = config_setting(k, category, setting);
    if (v.size() != 4) return fallback;
    return uint32_t(v[0]) << 24 | uint32_t(v[1]) << 16 | uint32_t(v[2]) << 8 | v[3];
}

namespace {

constexpr uint32_t DASHBOARD_ID = 0xFFFE07D1; // kDashboardID
constexpr uint32_t BASE_KERNEL_BUILD = 1888;  // kBaseKernelBuildVersion
constexpr uint32_t XN_SYS_DASHCONTEXTCHANGED = 0x8000000C;
constexpr uint32_t HR_PENDING = 0x8000000A;             // X_E_PENDING
constexpr uint32_t HR_NOT_IMPLEMENTED = 0x80004001;     // X_E_NOT_IMPLEMENTED
constexpr uint32_t HR_LOGON_NOT_LOGGED_ON = 0x80151802; // X_ONLINE_E_LOGON_NOT_LOGGED_ON
// XConfig: the user category's audio and retail flags, the IPTV provider
constexpr uint16_t CONFIG_USER = 3, CONFIG_USER_AUDIO_FLAGS = 0x0B, CONFIG_USER_RETAIL_FLAGS = 0x0C;
constexpr uint16_t CONFIG_IPTV = 9, CONFIG_IPTV_SERVICE_PROVIDER_NAME = 1;
constexpr uint32_t RETAIL_IPTV_ENABLED = 0x00001000;

// what XamState and KernelState keep for these exports
struct InfoState {
    std::mutex mutex;
    uint32_t dash_context = 0;
    uint8_t dash_app_info[0xC] = {};   // X_DASH_APP_INFO
    uint32_t dash_backstack_count = 0; // nothing ever pushes one (Xenia's never does)
    uint32_t launch_callback = 0;
    uint32_t online_schema = 0, iptv_name = 0;
};

InfoState &info(Kernel &k) { return k.state<InfoState>(); }

// ---- what a title ID is (title_id_utils.h) ----

constexpr bool valid_game_id(uint32_t title) { return (title >> 16) != 0xFFFE; }
constexpr char publisher_first(uint32_t title) { return char(title >> 24); }
constexpr char publisher_second(uint32_t title) { return char((title >> 16) & 0xFF); }

constexpr bool system_experience_title(uint32_t title) {
    bool app = publisher_first(title) == 'X' && (publisher_second(title) == 'H' || publisher_second(title) == 'J');
    return app || title == 0x584E07D1 || title == 0x584E07D2; // the apps, kXN_2001 and kXN_2002
}

constexpr bool system_title(uint32_t title) {
    if (!title) return true;
    if (publisher_first(title) != 'X') return !valid_game_id(title);
    if (publisher_second(title) == 'A') return !valid_game_id(title); // Xbox Live Arcade
    return true;
}

constexpr bool original_xbox_title(uint32_t title) {
    if (!valid_game_id(title)) return true;
    if (title >> 24 == 0xFF) return false;
    return (title & 0x7FFF) < 0x7D0;
}

static_assert(system_title(DASHBOARD_ID) && !system_title(0x4D5308BC) && !system_title(0x5841089A));
static_assert(original_xbox_title(0x41430006) && !original_xbox_title(0x4D5308BC));

// the calling thread's process type (KeGetCurrentProcessType)
uint8_t process_type(Kernel &k) {
    Thread *t = current_thread();
    return t ? k.host(t->kthread() + kthread::PROCESS_TYPE)[0] : kprocess::TITLE;
}

// XamQueryLiveHive(A/W): not logged on
uint32_t query_live_hive(Kernel &k, uint32_t name, uint32_t value, uint32_t size, uint32_t ov) {
    uint32_t extended_error = HR_LOGON_NOT_LOGGED_ON;
    if (!name || !value || !size) extended_error = X_E_INVALIDARG;
    if (ov) {
        complete_overlapped_immediate_ex(k, ov, X_STATUS_SUCCESS, extended_error, 0);
        return HR_PENDING;
    }
    return extended_error;
}

// XamGetLiveHiveValue(A/W): not logged on; the dashboard's system process
// asking with -1 queries the hive
uint32_t get_live_hive_value(Kernel &k, uint32_t name, uint32_t value, uint32_t size, uint32_t unk, uint32_t ov) {
    uint32_t r = HR_LOGON_NOT_LOGGED_ON;
    if (!name || !value || !size) r = X_E_INVALIDARG;
    if (unk == 0xFFFFFFFF && title_id(k) == DASHBOARD_ID && process_type(k) == kprocess::SYSTEM)
        return query_live_hive(k, name, value, size, ov);
    if (ov) {
        complete_overlapped_immediate(k, ov, r);
        return X_ERROR_IO_PENDING;
    }
    return r;
}

} // namespace

// ---- features and versions ----

// (feature): Xenia's table, 0x40FFFFFFFF, with feature n at bit n - 1 as the
// console's XAM reads it (xfxam: 1-32 and 39 on, 0 and 33-38 off; Xenia tests
// bit n of the table's low 36 bits)
XF_EXPORT(xam, XamFeatureEnabled) {
    uint32_t bit = arg(k, c, 0) - 1;
    result(c, bit < 64 && ((0x40FFFFFFFFull >> bit) & 1) ? 1 : 0);
}

XF_EXPORT(xam, XamGetStagingMode) { result(c, 0); } // Xenia's staging_mode, off
XF_EXPORT(xam, XamUpdateGetBaseSystemVersion) { result(c, BASE_KERNEL_BUILD); }
XF_EXPORT(xam, XamUpdateGetCurrentSystemVersion) { result(c, BASE_KERNEL_BUILD); }
XF_EXPORT(xam, XamDoesOmniNeedConfiguration) { result(c, 0); }
XF_EXPORT(xam, XamFirstRunExperienceShouldRun) { result(c, 0); }
// (live, local, adult, child profiles): nothing to count them for
XF_EXPORT(xam, XampWebInstrumentationSetProfileCounts) {}
// (unknown, unknown_ptr): XStudio's request (xstudio.xex's ordinal 1), absent
XF_EXPORT(xam, XamXStudioRequest) { result(c, X_E_FAIL); }

// (error_code, unknown): the two localization errors XAM logs succeed
XF_EXPORT(xam, XamLogLocalizationEtx) {
    uint32_t code = arg(k, c, 0);
    result(c, code == 0x80300034 || code == 0x80300035 ? X_ERROR_SUCCESS : HR_NOT_IMPLEMENTED);
}

// The online schema Xenia hands out: an empty one, behind a pointer and
// size pair, made once in the system heap.
XF_EXPORT(xam, XamGetOnlineSchema) {
    static constexpr uint8_t schema[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
                                         0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2C, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18};
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    if (!s.online_schema) {
        uint32_t p = k.memory().system_alloc(8 + sizeof(schema));
        if (p) {
            std::memcpy(k.host(p + 8), schema, sizeof(schema));
            k.store32(p, p + 8);
            k.store32(p + 4, sizeof(schema));
            s.online_schema = p;
        }
    }
    result(c, s.online_schema);
}

// ---- the Live hive: not logged on ----

// (feature_name, value, value_buffer_size, overlapped)
XF_EXPORT(xam, XamQueryLiveHiveA) {
    result(c, query_live_hive(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3)));
}
XF_EXPORT(xam, XamQueryLiveHiveW) {
    result(c, query_live_hive(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3)));
}

// (feature_name, value, value_buffer_size, unknown, overlapped)
XF_EXPORT(xam, XamGetLiveHiveValueA) {
    result(c, get_live_hive_value(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)));
}
XF_EXPORT(xam, XamGetLiveHiveValueW) {
    result(c, get_live_hive_value(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)));
}

// ---- titles ----

// (title_id, name_buffer, length_ptr): the name with its terminator and its
// length counting it; no title's name is known here
XF_EXPORT(xam, XamGetCachedTitleName) {
    uint32_t title = arg(k, c, 0), name = arg(k, c, 1), length_ptr = arg(k, c, 2);
    if (name) k.store16(name, 0);
    if (length_ptr) k.store32(length_ptr, 1);
    result(c, title ? X_ERROR_SUCCESS : X_ERROR_INVALID_PARAMETER);
}

XF_EXPORT(xam, XamIsCurrentTitleDash) { result(c, title_id(k) == DASHBOARD_ID ? 1 : 0); }
XF_EXPORT(xam, XamIsSystemTitleId) { result(c, system_title(arg(k, c, 0)) ? 1 : 0); }
XF_EXPORT(xam, XamIsSystemExperienceTitleId) { result(c, system_experience_title(arg(k, c, 0)) ? 1 : 0); }
XF_EXPORT(xam, XamIsXbox1TitleId) { result(c, original_xbox_title(arg(k, c, 0)) ? 1 : 0); }

// no signed-in user is a child's account here
XF_EXPORT(xam, XamIsChildAccountSignedIn) { result(c, 0); }

// (callback): kept, as Xenia keeps it; nothing launches a title to call it
XF_EXPORT(xam, XamLoaderRegisterLaunchRequestCallback) {
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.launch_callback = arg(k, c, 0);
}

// ---- the dashboard ----

// (value): a change is broadcast (XN_SYS_DASHCONTEXTCHANGED)
XF_EXPORT(xam, XamSetDashContext) {
    uint32_t value = arg(k, c, 0);
    bool changed;
    {
        InfoState &s = info(k);
        std::lock_guard<std::mutex> l(s.mutex);
        changed = value != s.dash_context;
        s.dash_context = value;
    }
    if (changed) broadcast(k, XN_SYS_DASHCONTEXTCHANGED, 0);
    result(c, 0);
}

XF_EXPORT(xam, XamGetDashContext) {
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    result(c, s.dash_context);
}

// (X_DASH_APP_INFO *): kept as given (0xC bytes), cleared with none
XF_EXPORT(xam, XamSetActiveDashAppInfo) {
    uint32_t p = arg(k, c, 0);
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    if (!p)
        std::memset(s.dash_app_info, 0, sizeof(s.dash_app_info));
    else
        std::memcpy(s.dash_app_info, k.host(p), sizeof(s.dash_app_info));
}

XF_EXPORT(xam, XamGetActiveDashAppInfo) {
    uint32_t p = arg(k, c, 0);
    if (!p) return;
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    std::memcpy(k.host(p), s.dash_app_info, sizeof(s.dash_app_info));
}

// (X_DASH_BACKSTACK_DATA *, count_ptr): the backstack is always empty
XF_EXPORT(xam, XamGetDashBackstackData) { result(c, X_ERROR_SUCCESS); }

// ---- the console's settings ----

// the audio flags (XCONFIG_USER_AUDIO_FLAGS), Dolby Digital and Pro Logic
// without any; Xenia's AV pack is HDMI, never SCART (2)
XF_EXPORT(xam, XGetAudioFlags) {
    uint32_t flags = config_setting32(k, CONFIG_USER, CONFIG_USER_AUDIO_FLAGS, 0);
    result(c, flags ? flags : 0x00010001);
}

// no stereoscopic modes (Xenia's enable_3d_mode, off)
XF_EXPORT(xam, XGetVideoCapabilities) { result(c, 0); }

// IPTV is enabled by the retail flags (off by default): X_E_FAIL when not
XF_EXPORT(xam, XamIsIptvEnabled) {
    bool enabled = config_setting32(k, CONFIG_USER, CONFIG_USER_RETAIL_FLAGS, 0) & RETAIL_IPTV_ENABLED;
    result(c, enabled ? X_ERROR_SUCCESS : X_E_FAIL);
}

// (name_ptr): the address of the provider's name (60 wide characters, from
// the settings, "Xenia TV" when empty)
XF_EXPORT(xam, XamIptvGetServiceName) {
    uint32_t out = arg(k, c, 0);
    InfoState &s = info(k);
    std::lock_guard<std::mutex> l(s.mutex);
    if (!s.iptv_name) s.iptv_name = k.memory().system_alloc(0x78);
    if (!s.iptv_name) return result(c, X_E_FAIL);
    std::memset(k.host(s.iptv_name), 0, 0x78);
    std::vector<uint8_t> name = config_setting(k, CONFIG_IPTV, CONFIG_IPTV_SERVICE_PROVIDER_NAME);
    std::memcpy(k.host(s.iptv_name), name.data(), std::min<size_t>(name.size(), 0x78));
    if (!k.load16(s.iptv_name)) put_u16(k, s.iptv_name, u"Xenia TV", 9);
    if (out) k.store32(out, s.iptv_name);
    result(c, X_ERROR_SUCCESS);
}

// (dvr_storage_ptr, used_dvr_storage_ptr, hdd_unused_space_ptr): none
XF_EXPORT(xam, XamGetDvrStorage) {
    if (uint32_t p = arg(k, c, 0)) k.store32(p, 0);
    if (uint32_t p = arg(k, c, 1)) k.store32(p, 0);
    result(c, X_ERROR_SUCCESS);
}

// (size, overlapped): accepted, the overlapped untouched (as Xenia's)
XF_EXPORT(xam, XamSetDvrStorage) { result(c, X_ERROR_SUCCESS); }

// ---- XAuth and network storage: offline ----

// (XAUTH_SETTINGS *): its size must be 8; then XAuth is unavailable
XF_EXPORT(xam, XampXAuthStartup) {
    uint32_t settings = arg(k, c, 0);
    result(c, settings && k.load32(settings) == 8 ? 0x80158406 : 0x80158401);
}

// (count_ptr): none left
XF_EXPORT(xam, XampXAuthShutdown) {
    if (uint32_t p = arg(k, c, 0)) k.store32(p, 0);
}

// no title buffer (non-zero crashes the titles, Xenia notes)
XF_EXPORT(xam, XampXAuthGetTitleBuffer) { result(c, 0); }

// (user_index; any: each user): XamUserCheckPrivilege(user, 0xD1), which
// denies every privilege offline, signed in or not
XF_EXPORT(xam, XamNetworkStorageUserHasPrivilege) { result(c, 0); }

// (user_index): a signed-in user with the preference set (none is kept), or
// with the privilege (denied)
XF_EXPORT(xam, XamNetworkStorageHasUserEnabledStorage) { result(c, 0); }

// ---- notifications ----

// (id, data): to every listener
XF_EXPORT(xam, XNotifyBroadcast) {
    broadcast(k, arg(k, c, 0), arg(k, c, 1));
    result(c, X_ERROR_SUCCESS);
}

// (type, user_index, areas (64 bits), text, context): logged, no UI to show
XF_EXPORT(xam, XNotifyQueueUI) {
    std::u16string text = guest_u16(k, arg(k, c, 3), 0x400);
    std::string utf8;
    for (char16_t ch : text) utf8 += ch < 0x80 ? char(ch) : '?';
    std::fprintf(stderr, "xam: XNotifyQueueUI: %s\n", utf8.c_str());
    result(c, X_ERROR_SUCCESS);
}

// ---- devices that are never there ----

// the microphone array: not connected
XF_EXPORT(xam, XamVoiceGetMicArrayStatus) { result(c, 0); }

// the media player's objects (Xenia's stubs)
XF_EXPORT(xam, EnumerateMediaObjects) { result(c, HR_NOT_IMPLEMENTED); }
XF_EXPORT(xam, EnumerateMediaObjects_) { result(c, HR_NOT_IMPLEMENTED); }
XF_EXPORT(xam, EnumerateMediaObjects_0) { result(c, HR_NOT_IMPLEMENTED); }
XF_EXPORT(xam, EnumerateMediaObjects_1) { result(c, HR_NOT_IMPLEMENTED); }

// the disc's media verification (xam.cpp has the rest): nothing to close or
// inject
XF_EXPORT(xam, XamMediaVerificationClose) { result(c, X_ERROR_SUCCESS); }
XF_EXPORT(xam, XamMediaVerificationInject) { result(c, X_ERROR_SUCCESS); }

} // namespace xf::kernel
