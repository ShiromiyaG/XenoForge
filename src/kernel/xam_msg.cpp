/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's messages to its apps (XMsg*), XamGetOverlappedResult, and the
// parties (never in one). The apps answer as Xenia's do offline: XAM's own
// (0xFE), XLiveBase (0xFC), XGI (0xFB: contexts, properties, achievements
// and sessions taken and kept nowhere), the media player (0xFA: idle, the
// title plays its music), Messenger (0xF7). Translated from Xenia (BSD; see
// LICENSE-xenia): xam_msg.cc, app_manager.cc, apps/xam_app.cc,
// xlivebase_app.cc, xgi_app.cc, xmp_app.cc, messenger_app.cc, xam_party.cc;
// checked against them. The messages Xenia answers from state this kernel
// does not keep (the user tracker's contexts and properties, the media
// player's playlists and songs, Kinect's identities (its title
// initialization is kept: NuiState, xam.h), the cross-title enumeration) answer
// X_E_FAIL here and are logged once each.

#include "xam.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace xf::kernel {
namespace {

constexpr uint32_t X_ONLINE_S_LOGON_DISCONNECTED = 0x001510F1;
constexpr uint32_t X_ONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;
constexpr uint32_t X_ONLINE_E_SESSION_NOT_LOGGED_ON = 0x80155209;

uint32_t unanswered(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard<std::mutex> l(m);
    if (seen.insert({app, message}).second)
        std::fprintf(stderr, "xam: app 0x%02X does not answer message 0x%08X (0x%08X, 0x%X)\n", app, message, buffer,
                     length);
    return X_E_FAIL;
}

// XamApp::DispatchMessageSync
uint32_t xam_app(Kernel &k, uint32_t message, uint32_t buffer, uint32_t length) {
    switch (message) {
    case 0x00020021: { // XContentQueryVolumeDeviceType(root_name[64], ...): packages only
        std::string root(reinterpret_cast<const char *>(k.host(buffer)),
                         strnlen(reinterpret_cast<const char *>(k.host(buffer)), 64));
        std::string target;
        if (!file_system(k).find_link(root + ":", target)) return X_E_INVALIDARG;
        if (!target.starts_with("\\Device\\Package_")) return X_E_INVALIDARG;
        uint32_t device_type_ptr = k.load32(buffer + 68);
        uint32_t deployment = file_system(k).deployment;
        k.store32(device_type_ptr, deployment == 0 ? 4 /* ODD */ : deployment <= 2 ? 1 /* HDD */ : 0);
        return X_E_SUCCESS;
    }
    case 0x00021012: return X_E_SUCCESS; // XEnableGuestSignin
    case 0x00022005:                     // XTitleGetDeploymentType(type_ptr, overlapped)
        k.store32(k.load32(buffer), file_system(k).deployment);
        return X_E_SUCCESS;
    case 0x0002B001: return X_E_SUCCESS; // XNuiDelayUI
    case 0x0002B003: {                   // XNuiTitleInitialize(xam_version (64), xtl_version (64), flags)
        uint64_t xam_version = k.load64(buffer), xtl_version = k.load64(buffer + 8);
        uint32_t flags = k.load32(buffer + 0x10);
        uint32_t r = X_E_FAIL;
        if (xam_version == 5) {
            NuiState &n = k.state<NuiState>();
            std::lock_guard<std::mutex> l(n.mutex);
            n.versions[0] = xam_version, n.versions[1] = xtl_version;
            n.init_flags = flags;
            if (!(flags & 0x80000000)) r = X_E_SUCCESS;
        }
        return r;
    }
    case 0x0002B004: { // XNuiTitleShutdown
        NuiState &n = k.state<NuiState>();
        std::lock_guard<std::mutex> l(n.mutex);
        n.versions[0] = n.versions[1] = 0;
        n.init_flags = 0;
        n.tilt_callback = 0;
        return X_E_SUCCESS;
    }
    case 0x0002C009: return buffer ? X_E_SUCCESS : X_E_INVALIDARG;
    case 0x8000000D: return X_E_SUCCESS; // the language or region changed; no parental control
    default: return unanswered(0xFE, message, buffer, length);
    }
}

// XLiveBaseApp::DispatchMessageSync: not logged on
uint32_t xlivebase_app(Kernel &k, uint32_t message, uint32_t buffer, uint32_t length) {
    switch (message) {
    case 0x00058003: return X_ONLINE_S_LOGON_DISCONNECTED; // XLiveBaseLogonGetHR
    case 0x00058004:                                       // XLiveBaseGetLogonId
        if (buffer) k.store32(buffer, 1);
        return X_E_SUCCESS;
    case 0x00058006:                                        // XLiveBaseGetNatType
    case 0x00058007: return X_ONLINE_E_LOGON_NOT_LOGGED_ON; // XOnlineGetServiceInfo
    case 0x00058037:                                        // XPresenceInitialize
    case 0x00058046: return X_E_SUCCESS;                    // the signed-in profile's check
    case 0x0005008C:
    case 0x00050094:
    case 0x00058020:
    case 0x00058023: // dashboards', friends, invites
        return X_E_FAIL;
    default: return unanswered(0xFC, message, buffer, length);
    }
}

// XgiApp::DispatchMessageSync
uint32_t xgi_app(Kernel &k, uint32_t message, uint32_t buffer, uint32_t length) {
    switch (message) {
    case 0x000B0006: // XGIUserSetContext
    case 0x000B0007: // XGIUserSetPropertyEx
    case 0x000B0008: // XGIUserWriteAchievements
    case 0x000B0011: // XGISessionDelete
    case 0x000B0012: // XGISessionJoinLocal
    case 0x000B0014: // XSessionStart
    case 0x000B0015: // XSessionEnd
        return X_E_SUCCESS;
    case 0x000B0010: { // XSessionCreate: offline sessions only
        uint32_t flags = k.load32(buffer + 4);
        constexpr uint32_t LIVE_FEATURES = 0x02 | 0x04 | 0x08 | 0x10; // presence, stats, matchmaking, arbitration
        return (flags && (flags & LIVE_FEATURES)) ? X_ONLINE_E_SESSION_NOT_LOGGED_ON : X_E_SUCCESS;
    }
    case 0x000B0021: return X_ONLINE_E_LOGON_NOT_LOGGED_ON; // XUserReadStats
    case 0x000B0036:                                        // XInvalidateGamerTileCache
    case 0x000B003D: return X_E_FAIL;                       // XUserGetANID
    default: return unanswered(0xFB, message, buffer, length);
    }
}

// XmpApp::DispatchMessageSync: the player idle, its volume full, the title
// the controller
uint32_t xmp_app(Kernel &k, uint32_t message, uint32_t buffer, uint32_t length) {
    switch (message) {
    case 0x00070003: // XMPContinue
    case 0x00070004: // XMPStop
    case 0x00070005: // XMPPause
    case 0x00070006: // XMPNext
    case 0x00070007: // XMPPrevious
    case 0x00070008: // XMPSetPlaybackBehavior
    case 0x0007000C: // XMPSetVolume
    case 0x0007001A: // XMPSetPlaybackController
        return X_E_SUCCESS;
    case 0x00070009: { // XMPGetStatus(client, state_ptr)
        uint32_t state_ptr = k.load32(buffer + 4);
        if (!state_ptr) return X_E_INVALIDARG;
        k.store32(state_ptr, 0); // idle
        return X_E_SUCCESS;
    }
    case 0x0007000B: { // XMPGetVolume(client, volume_ptr)
        uint32_t volume_ptr = k.load32(buffer + 4);
        if (!volume_ptr) return X_E_INVALIDARG;
        k.store32(volume_ptr, 0x3F800000); // 1.0f
        return X_E_SUCCESS;
    }
    case 0x0007001B: {                      // XMPGetPlaybackController(client, controller_ptr, locked_ptr)
        k.store32(k.load32(buffer + 4), 0); // the game
        k.store32(k.load32(buffer + 8), 0); // no override
        return X_E_SUCCESS;
    }
    default: return unanswered(0xFA, message, buffer, length);
    }
}

// AppManager::DispatchMessageSync (the async form answers the same)
uint32_t dispatch(Kernel &k, uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
    switch (app) {
    case 0xFE: return xam_app(k, message, buffer, length);
    case 0xFC: return xlivebase_app(k, message, buffer, length);
    case 0xFB: return xgi_app(k, message, buffer, length);
    case 0xFA: return xmp_app(k, message, buffer, length);
    case 0xF7: unanswered(0xF7, message, buffer, length); return X_STATUS_UNSUCCESSFUL; // Messenger
    default: std::fprintf(stderr, "xam: no app 0x%08X (message 0x%08X)\n", app, message); return X_E_NOTFOUND;
    }
}

// xeXMsgStartIORequestEx
uint32_t start_io_request(Kernel &k, uint32_t app, uint32_t message, uint32_t ov, uint32_t buffer, uint32_t length) {
    uint32_t r = dispatch(k, app, message, buffer, length);
    if (r == X_E_NOTFOUND) {
        r = X_E_INVALIDARG;
        set_last_error(k, X_ERROR_NOT_FOUND);
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, r);
        r = X_ERROR_IO_PENDING;
    }
    if (r == X_ERROR_SUCCESS || r == X_ERROR_IO_PENDING) set_last_error(k, 0);
    return r;
}

} // namespace

// (app, message, buffer, length)
XF_EXPORT(xam, XMsgInProcessCall) { result(c, dispatch(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3))); }
XF_EXPORT(xam, XMsgSystemProcessCall) {
    result(c, dispatch(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3)));
}

// (app, message, overlapped, buffer, length[, task_args])
XF_EXPORT(xam, XMsgStartIORequest) {
    result(c, start_io_request(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)));
}
XF_EXPORT(xam, XMsgStartIORequestEx) {
    result(c, start_io_request(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)));
}

// (overlapped, wait): requests complete at once, so one waits for nothing
XF_EXPORT(xam, XMsgCancelIORequest) { result(c, 0); }

// (overlapped, result, extended_error, length)
XF_EXPORT(xam, XMsgCompleteIORequest) {
    complete_overlapped_immediate_ex(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3));
    result(c, X_ERROR_SUCCESS);
}

// ---- parties (xam_party.cc) and their UI (xam_ui.cc): never in one ----
// Games ask for these by ordinal (XexGetProcedureAddress).

namespace {
constexpr uint32_t X_PARTY_E_NOT_IN_PARTY = 0x807D0003;
constexpr uint32_t X_E_NOT_IMPLEMENTED = 0x80004001;
} // namespace

// (caller, party_list)
XF_EXPORT(xam, XamPartyGetUserList) { result(c, arg(k, c, 0) != 1 ? X_E_NOT_IMPLEMENTED : X_PARTY_E_NOT_IN_PARTY); }

// (caller, user_index, unknown, overlapped)
XF_EXPORT(xam, XamPartySendGameInvites) {
    result(c, run_or_defer(k, arg(k, c, 3), [](uint32_t &extended_error, uint32_t &length) {
               extended_error = 0, length = 0;
               return X_ERROR_SUCCESS;
           }));
}

// (caller, user_index, custom_data)
XF_EXPORT(xam, XamPartySetCustomData) {
    if (arg(k, c, 0) != 1) return result(c, X_E_NOT_IMPLEMENTED);
    result(c, arg(k, c, 1) >= 4 ? 0x807D0002 : X_PARTY_E_NOT_IN_PARTY);
}

// (bandwidth_type, statistic_ptr)
XF_EXPORT(xam, XamPartyGetBandwidth) {
    if (uint32_t p = arg(k, c, 1)) k.store64(p, 0);
    result(c, X_ERROR_SUCCESS);
}

// (XamShowPartyUI and XamShowCommunitySessionsUI: xam_ui.cpp)

// (overlapped, length_ptr, wait): its result, waiting on its event when
// asked and still pending
XF_EXPORT(xam, XamGetOverlappedResult) {
    uint32_t ov = arg(k, c, 0), length_ptr = arg(k, c, 1), wait = arg(k, c, 2);
    uint32_t r = k.load32(ov + xoverlapped::RESULT);
    if (r == X_ERROR_IO_PENDING) {
        uint32_t event_handle = k.load32(ov + xoverlapped::EVENT);
        Objects &o = objects(k);
        Ref<Event> e = wait && event_handle ? o.lookup<Event>(event_handle) : Ref<Event>();
        Thread *t = current_thread();
        if (e && t) {
            Dispatcher *list[] = {e.get()};
            r = o.wait(*t, list, false, false, Scheduler::NEVER);
        } else {
            r = X_STATUS_TIMEOUT;
        }
    }
    if (r == X_STATUS_TIMEOUT) return result(c, X_ERROR_IO_INCOMPLETE);
    if (r & 0xC0000000) return result(c, last_error(k));
    if (length_ptr) k.store32(length_ptr, k.load32(ov + xoverlapped::LENGTH));
    result(c, r);
}

} // namespace xf::kernel
