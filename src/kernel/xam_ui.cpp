/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's system UI (the guide's dialogs: message boxes, the keyboard, the
// storage device selector, sign-in, the marketplace...) for a system that
// draws none: each answers as Xenia does headless, with the dialog's
// default (the focused button, the default text, the hard drive, the user
// already signed in), its overlapped completed and the XN_SYS_UI
// notifications around it. Translated from Xenia (BSD; see LICENSE-xenia):
// xam_ui.cc (the headless paths: xeXamDispatchHeadless, ...HeadlessAsync,
// and the exports) and nui.cc (NuiHudCheck); checked against them.
//
// Where it differs from Xenia, and why:
//   - XN_SYS_UI goes out on, then off, together around the answer (Xenia
//     sleeps 25 ms after "on" and sends "off" 200 ms later from another
//     thread); an overlapped request completes before the export returns
//     X_ERROR_IO_PENDING (xam.h, run_or_defer);
//   - the exports Xenia answers with a dialog even headless (the
//     achievements, the gamer card, the profile editor, the marketplace
//     outside headless) answer as the dialog closed at once: XN_SYS_UI on
//     and off, X_ERROR_SUCCESS;
//   - XamShowAchievementsUI shows the running title's (title_id 0 or its
//     own) and finds no record of any other (X_ERROR_NO_SUCH_USER), as
//     Xenia's user tracker finds none for a title the profile never played.
//
// A host that draws the dialogs sets a UiHandler (kernel.h, set_ui_handler); present() and present_async() below
// hand each request to it and complete the title's overlapped when it answers (docs/runtime.md, "XAM").
//
// What the console's XAM answers with no user (the oracle, xfxam, the system update's own XAM): ERROR_ACCESS_DENIED
// from the friends, players, messages, quick chat, game invite, friend request, player review and message compose
// pages; 0x65B (ERROR_FUNCTION_FAILED) from the gamer card, the gamer card for an XUID and the achievements;
// ERROR_INVALID_PARAMETER from the custom message and player list pages given null arguments. Xenia implements none
// of the first group and answers the gamer card ERROR_ACCESS_DENIED and the achievements NO_SUCH_USER.
// Kinect (NUI) is never there: its UI answers X_ERROR_ACCESS_DENIED, as
// Xenia's NuiHudCheck does with no sensor initialized.

#include "xam.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>

namespace xf::kernel {
namespace {

bool signed_in(Kernel &k, uint32_t index) { return index < 4 && user_at(k, index) != nullptr; }

struct UiState {
    std::mutex mutex;
    UiHandler handler;
    std::atomic<int> active{0}; // dialogs a host has taken and not yet answered
};
UiState &ui(Kernel &k) { return k.state<UiState>(); }

UiHandler handler_of(Kernel &k) {
    UiState &s = ui(k);
    std::lock_guard<std::mutex> l(s.mutex);
    return s.handler;
}

// The request given to the host. true: it took it, and `finish` runs once with its answer, from whichever thread
// that is, before the dialog counts as closed (XN_SYS_UI off).
bool hand_over(Kernel &k, const UiRequest &request, std::function<void(const UiResult &)> finish) {
    UiHandler handler = handler_of(k);
    if (!handler) return false;
    UiState &s = ui(k);
    s.active++;
    auto once = std::make_shared<std::atomic<bool>>(false);
    Kernel *kernel = &k;
    UiDone done = [kernel, once, finish = std::move(finish)](const UiResult &result) {
        if (once->exchange(true)) return;
        finish(result);
        ui(*kernel).active--;
        broadcast(*kernel, XN_SYS_UI, 0);
    };
    if (handler(request, done)) return true;
    once->store(true);
    s.active--;
    return false;
}

// writes a dialog's answer into the title's memory and says the status
using UiApply = std::function<uint32_t(const UiResult &)>;

constexpr uint32_t ERROR_CANCELLED = 0x4C7;

// What the UI does with a dialog: shows it (XN_SYS_UI on), makes its answer,
// hides it (XN_SYS_UI off). With no host handler the answer is the dialog's
// default, made at once. With an overlapped, the answer completes it and the
// export returns X_ERROR_IO_PENDING (xeXamDispatchHeadless); a host's answer
// may come later, from another thread, and a cancelled dialog completes it
// with ERROR_CANCELLED. Without an overlapped a host's answer is waited for.
uint32_t present(Kernel &k, uint32_t ov, const UiRequest &request, const UiResult &defaults, UiApply apply) {
    broadcast(k, XN_SYS_UI, 1);
    auto status_of = [apply](const UiResult &r) { return r.cancelled ? ERROR_CANCELLED : apply(r); };
    if (ov) {
        begin_overlapped(k, ov);
        Kernel *kernel = &k;
        if (hand_over(k, request, [kernel, ov, status_of](const UiResult &r) {
                uint32_t status = status_of(r);
                complete_overlapped(*kernel, ov, status, status, 0);
            }))
            return X_ERROR_IO_PENDING;
    } else {
        auto answer = std::make_shared<std::promise<uint32_t>>();
        std::future<uint32_t> future = answer->get_future();
        if (hand_over(k, request, [answer, status_of](const UiResult &r) { answer->set_value(status_of(r)); }))
            return future.get();
    }
    // no host: the default, at once
    uint32_t a = apply(defaults);
    if (ov) complete_overlapped(k, ov, a, a, 0);
    broadcast(k, XN_SYS_UI, 0);
    return ov ? X_ERROR_IO_PENDING : a;
}

// a dialog the export does not wait for (xeXamDispatchHeadlessAsync and
// xeXamDispatchDialogAsync): X_ERROR_SUCCESS, `closed` run as it closes
// (not when a host cancelled it)
uint32_t present_async(Kernel &k, const UiRequest &request, std::function<void()> closed = {}) {
    broadcast(k, XN_SYS_UI, 1);
    if (hand_over(k, request, [closed](const UiResult &r) {
            if (closed && !r.cancelled) closed();
        }))
        return X_ERROR_SUCCESS;
    if (closed) closed();
    broadcast(k, XN_SYS_UI, 0);
    return X_ERROR_SUCCESS;
}

UiRequest request_of(UiKind kind, uint32_t user = 0xFF) {
    UiRequest r;
    r.kind = kind;
    r.user = user;
    return r;
}

// NUIController::NuiHudCheck: no Kinect sensor initialized
uint32_t nui_hud_check() { return X_ERROR_ACCESS_DENIED; }

// a message box (XamShowMessageBoxUi): the focused button
// The console's XAM refuses (ERROR_INVALID_PARAMETER, xfxam) a box with no
// overlapped (a box is never synchronous there), a user that is neither 0-3
// nor "any" (0xFF), a button count of 0 or past 3, an active button past the
// count, no result, and flags outside the icon (0-4), bit 8 and bits 12-21.
// Xenia takes them all and answers the default button at once. With a valid
// request and no UI to show it the console's XAM answers
// ERROR_FUNCTION_FAILED; here the box is answered headless, as Xenia does.
uint32_t message_box(Kernel &k, uint32_t user, uint32_t title, uint32_t text, uint32_t button_count, uint32_t buttons,
                     uint32_t active_button, uint32_t flags, uint32_t result_ptr, uint32_t ov) {
    constexpr uint32_t FLAGS_TAKEN = 0x003FF107;
    if (!ov || (user >= 4 && user != USER_INDEX_ANY) || button_count < 1 || button_count > 3 ||
        active_button >= button_count || !result_ptr || (flags & ~FLAGS_TAKEN))
        return X_ERROR_INVALID_PARAMETER;
    UiRequest request = request_of(UiKind::MessageBox, user);
    if (title) request.title = guest_u16(k, title);
    if (text) request.text = guest_u16(k, text);
    for (uint32_t i = 0; buttons && i < button_count; i++)
        if (uint32_t label = k.load32(buttons + 4 * i)) request.buttons.push_back(guest_u16(k, label));
    request.default_button = active_button;
    request.flags = flags;
    UiResult defaults;
    defaults.button = active_button;
    return present(k, ov, request, defaults, [&k, result_ptr](const UiResult &r) {
        k.store32(result_ptr, r.button); // MESSAGEBOX_RESULT::ButtonPressed
        return X_ERROR_SUCCESS;
    });
}

// X_MARKETPLACE_ENTRYPOINT
constexpr uint32_t MARKETPLACE_FORCED_NAME_CHANGE_V2 = 8, MARKETPLACE_MAX_ENTRYPOINT = 25;

// XamShowMarketplaceUIEx's checks, then a page shown and closed
uint32_t marketplace(Kernel &k, uint32_t user_index, uint32_t ui_type) {
    if (user_index >= 4 || ui_type >= MARKETPLACE_MAX_ENTRYPOINT) return X_ERROR_INVALID_PARAMETER;
    if (!signed_in(k, user_index)) return X_ERROR_NO_SUCH_USER;
    return present_async(k, request_of(UiKind::Marketplace, user_index));
}

// xeXamShowSigninUI: the profiles needed signed in (the one user is, in
// slot 0), told with XN_SYS_SIGNINCHANGED and the slots' mask
uint32_t signin(Kernel &k, uint32_t users_needed) {
    if (users_needed != 1 && users_needed != 2 && users_needed != 4) return X_ERROR_INVALID_PARAMETER;
    UiRequest request = request_of(UiKind::SignIn);
    request.users_needed = users_needed;
    return present_async(k, request, [&k, users_needed] {
        uint32_t slots = 0;
        for (uint32_t i = 0, found = 0; i < 4 && found < users_needed; i++)
            if (user_at(k, i)) slots |= 1u << i, found++;
        broadcast(k, XN_SYS_SIGNINCHANGED, slots);
    });
}

} // namespace

XF_EXPORT(xam, XamIsUIActive) { result(c, ui(k).active.load() > 0); } // a dialog a host has taken and not answered

// ---- message boxes ----

// (user_index, title, text, button_count, buttons, active_button, flags,
// result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUI) {
    result(c, message_box(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5),
                          arg(k, c, 6), arg(k, c, 7), arg(k, c, 8)));
}

// (user_index, title, text, button_count, buttons, active_button, flags,
// unused, result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUIEx) {
    result(c, message_box(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5),
                          arg(k, c, 6), arg(k, c, 8), arg(k, c, 9)));
}

// (tracking_id, user_index, title, text, ...): Kinect's
XF_EXPORT(xam, XamShowNuiMessageBoxUI) { result(c, nui_hud_check()); }

// (XNotifyQueueUI, in Xenia's xam_ui.cc, is with the notifications in
// xam_info.cpp)

// ---- the keyboard ----

// (user_index, flags, default_text, title, description, buffer, length,
// overlapped): the default text is the answer
XF_EXPORT(xam, XamShowKeyboardUI) {
    uint32_t user = arg(k, c, 0), default_text = arg(k, c, 2), title = arg(k, c, 3), description = arg(k, c, 4);
    uint32_t buffer = arg(k, c, 5), length = arg(k, c, 6), ov = arg(k, c, 7);
    if (!buffer) return result(c, X_ERROR_INVALID_PARAMETER);
    UiRequest request = request_of(UiKind::Keyboard, user);
    request.flags = arg(k, c, 1);
    if (default_text) request.default_text = guest_u16(k, default_text);
    if (title) request.title = guest_u16(k, title);
    if (description) request.text = guest_u16(k, description);
    request.max_length = length;
    UiResult defaults;
    defaults.text = request.default_text;
    result(c, present(k, ov, request, defaults, [&k, buffer, length](const UiResult &r) {
               if (r.text.empty())
                   std::memset(k.host(buffer), 0, size_t(length) * 2);
               else
                   put_u16(k, buffer, r.text, length);
               return X_ERROR_SUCCESS;
           }));
}

// ---- storage ----

// (user_index, content_type, content_flags, total_requested (64 bits),
// device_id_ptr, overlapped): the hard drive, the first storage device
XF_EXPORT(xam, XamShowDeviceSelectorUI) {
    uint32_t index = arg(k, c, 0), flags = arg(k, c, 2), device_ptr = arg(k, c, 4), ov = arg(k, c, 5);
    if (!ov) return result(c, X_ERROR_INVALID_PARAMETER);
    if ((index >= 4 && index != USER_INDEX_ANY) || (flags & 0x83F00008) || !device_ptr) {
        k.store32(ov + xoverlapped::EXTENDED_ERROR, X_ERROR_INVALID_PARAMETER);
        return result(c, X_ERROR_INVALID_PARAMETER);
    }
    UiRequest request = request_of(UiKind::DeviceSelector, index);
    request.content_type = arg(k, c, 1);
    request.flags = flags;
    request.bytes_required = arg64(k, c, 3);
    UiResult defaults;
    defaults.device_id = 1; // the HDD (xam_content.cpp's devices)
    result(c, present(k, ov, request, defaults, [&k, device_ptr](const UiResult &r) {
               k.store32(device_ptr, r.device_id);
               return X_ERROR_SUCCESS;
           }));
}

// (tracking_id, user_index, ...): Kinect's
XF_EXPORT(xam, XamShowNuiDeviceSelectorUI) { result(c, nui_hud_check()); }

// (user_index): the title could not read its disc; said here, as Xenia's
// dialog says it (headless, Xenia asserts and answers X_ERROR_SUCCESS)
XF_EXPORT(xam, XamShowDirtyDiscErrorUI) {
    std::fprintf(stderr, "xam: the title reports a disc read error (XamShowDirtyDiscErrorUI, from 0x%08X)\n",
                 uint32_t(c.lr));
    result(c, X_ERROR_SUCCESS);
}

// (tracking_id): Kinect's
XF_EXPORT(xam, XamShowNuiDirtyDiscErrorUI) { result(c, nui_hud_check()); }

// ---- Live: parties, the marketplace, upgrades ----

XF_EXPORT(xam, XamShowPartyUI) { result(c, X_ERROR_FUNCTION_FAILED); }
XF_EXPORT(xam, XamShowCommunitySessionsUI) { result(c, X_ERROR_FUNCTION_FAILED); }

// (user_index, ui_type, offer_id (64 bits), offer_type, category, unknown,
// unknown, title_id): nothing to buy
XF_EXPORT(xam, XamShowMarketplaceUIEx) { result(c, marketplace(k, arg(k, c, 0), arg(k, c, 1))); }

// (user_index, ui_type, offer_id (64 bits), offer_type, category, title_id)
XF_EXPORT(xam, XamShowMarketplaceUI) { result(c, marketplace(k, arg(k, c, 0), arg(k, c, 1))); }

// (user_index, entry_point, offers, offer_count, hresult_ptr, overlapped):
// the downloads asked for succeed (Xenia's headless answer)
XF_EXPORT(xam, XamShowMarketplaceDownloadItemsUI) {
    uint32_t index = arg(k, c, 0), offers = arg(k, c, 2), count = arg(k, c, 3), hresult_ptr = arg(k, c, 4);
    uint32_t ov = arg(k, c, 5);
    if (index >= 4 || !offers || count > 6) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!signed_in(k, index)) {
        if (!ov) return result(c, X_ERROR_NO_SUCH_USER);
        complete_overlapped_immediate(k, ov, X_ERROR_NO_SUCH_USER);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, present(k, ov, request_of(UiKind::Marketplace, index), UiResult{},
                      [&k, hresult_ptr](const UiResult &) {
                          if (hresult_ptr) k.store32(hresult_ptr, X_E_SUCCESS);
                          return X_ERROR_SUCCESS;
                      }));
}

// (user_index, unknown, title_id) and (user_index): no Gold to buy
XF_EXPORT(xam, XamShowGoldUpgradeUI) { result(c, X_ERROR_FUNCTION_FAILED); }
XF_EXPORT(xam, XamShowMultiplayerUpgradeUI) { result(c, X_ERROR_FUNCTION_FAILED); }

// (user_index): the marketplace's name change page
XF_EXPORT(xam, XamShowForcedNameChangeUI) {
    result(c, marketplace(k, arg(k, c, 0), MARKETPLACE_FORCED_NAME_CHANGE_V2));
}

// ---- profiles ----

// (users_needed, flags): the games wait for the notification
XF_EXPORT(xam, XamShowSigninUI) { result(c, signin(k, arg(k, c, 0))); }

// (users_needed, flags, overlapped): with an overlapped, completed at once
// with the answer
XF_EXPORT(xam, XamShowSigninUIEx) {
    uint32_t r = signin(k, arg(k, c, 0)), ov = arg(k, c, 2);
    if (!ov) return result(c, r);
    complete_overlapped_immediate(k, ov, r);
    result(c, X_ERROR_IO_PENDING);
}

// (user_index, users_needed, flags)
XF_EXPORT(xam, XamShowSigninUIp) { result(c, signin(k, arg(k, c, 1))); }

// (tracking_id, user_index, flags): Kinect's
XF_EXPORT(xam, XamShowNuiSigninUI) { result(c, nui_hud_check()); }

// (user_index, flag, unknown) and (user_index, flag): headless, Xenia
// answers at once with no dialog and no notification
XF_EXPORT(xam, XamShowCreateProfileUIEx) { result(c, X_ERROR_SUCCESS); }
XF_EXPORT(xam, XamShowCreateProfileUI) { result(c, X_ERROR_SUCCESS); }

// (user_index, title_id): the running title's achievements, shown and closed
XF_EXPORT(xam, XamShowAchievementsUI) {
    uint32_t index = arg(k, c, 0), title = arg(k, c, 1);
    if (!user_at(k, index)) return result(c, X_ERROR_FUNCTION_FAILED); // the console's XAM, no user: 0x65B
    if (title && title != title_id(k)) return result(c, X_ERROR_NO_SUCH_USER);
    result(c, present_async(k, request_of(UiKind::Achievements, index)));
}

// (user_index): the user's gamer card, and the profile editor
XF_EXPORT(xam, XamShowGamerCardUI) {
    if (!user_at(k, arg(k, c, 0))) return result(c, X_ERROR_FUNCTION_FAILED);
    result(c, present_async(k, request_of(UiKind::GamerCard, arg(k, c, 0))));
}

XF_EXPORT(xam, XamShowEditProfileUI) {
    if (!user_at(k, arg(k, c, 0))) return result(c, X_ERROR_ACCESS_DENIED);
    result(c, present_async(k, request_of(UiKind::EditProfile, arg(k, c, 0))));
}

// ---- the pages Xenia does not implement ----

namespace {
// a page of Live's: the console's XAM refuses with ERROR_ACCESS_DENIED where no user is signed in (the oracle,
// above); a signed-in user is shown the page, closed at once headless
uint32_t live_page(Kernel &k, UiRequest request) {
    if (!user_at(k, request.user)) return X_ERROR_ACCESS_DENIED;
    return present_async(k, request);
}

// (the XUIDs `count` points to, as a title passes a recipient list)
std::vector<uint64_t> xuid_list(Kernel &k, uint32_t list, uint32_t count) {
    std::vector<uint64_t> out;
    if (list && count <= 0x100)
        for (uint32_t i = 0; i < count; i++) out.push_back(k.load64(list + 8 * i));
    return out;
}

uint32_t gamer_card_for_xuid(Kernel &k, ppc::Context &c) {
    if (!user_at(k, arg(k, c, 0))) return X_ERROR_FUNCTION_FAILED;
    UiRequest request = request_of(UiKind::GamerCard, arg(k, c, 0));
    request.xuid = arg64(k, c, 1);
    return present_async(k, request);
}
} // namespace

// (user_index): the friends list, the recent players, the messages, quick chat
XF_EXPORT(xam, XamShowFriendsUI) { result(c, live_page(k, request_of(UiKind::Friends, arg(k, c, 0)))); }
XF_EXPORT(xam, XamShowFriendsUIp) { result(c, live_page(k, request_of(UiKind::Friends, arg(k, c, 0)))); }
XF_EXPORT(xam, XamShowPlayersUI) { result(c, live_page(k, request_of(UiKind::Players, arg(k, c, 0)))); }
XF_EXPORT(xam, XamShowMessagesUI) { result(c, live_page(k, request_of(UiKind::Messages, arg(k, c, 0)))); }
XF_EXPORT(xam, XamShowQuickChatUI) { result(c, live_page(k, request_of(UiKind::QuickChat, arg(k, c, 0)))); }
XF_EXPORT(xam, XamShowQuickChatUIp) { result(c, live_page(k, request_of(UiKind::QuickChat, arg(k, c, 0)))); }

// (user_index, XUID): a friend request, a player's review, a gamer card by XUID. The gamer card is refused with
// 0x65B with no user, as the gamer card of the user's own is.
XF_EXPORT(xam, XamShowFriendRequestUI) {
    UiRequest request = request_of(UiKind::FriendRequest, arg(k, c, 0));
    request.xuid = arg64(k, c, 1);
    result(c, live_page(k, request));
}
XF_EXPORT(xam, XamShowPlayerReviewUI) {
    UiRequest request = request_of(UiKind::PlayerReview, arg(k, c, 0));
    request.xuid = arg64(k, c, 1);
    result(c, live_page(k, request));
}
XF_EXPORT(xam, XamShowGamerCardUIForXUID) { result(c, gamer_card_for_xuid(k, c)); }
XF_EXPORT(xam, XamShowGamerCardUIForXUIDp) { result(c, gamer_card_for_xuid(k, c)); }

// (user_index, recipients (XUIDs), count, text): a game invite, a message to compose
XF_EXPORT(xam, XamShowGameInviteUI) {
    UiRequest request = request_of(UiKind::GameInvite, arg(k, c, 0));
    if (user_at(k, request.user)) request.xuids = xuid_list(k, arg(k, c, 1), arg(k, c, 2));
    if (uint32_t text = arg(k, c, 3); text && user_at(k, request.user)) request.text = guest_u16(k, text);
    result(c, live_page(k, request));
}
XF_EXPORT(xam, XamShowMessageComposeUI) {
    UiRequest request = request_of(UiKind::MessageCompose, arg(k, c, 0));
    if (user_at(k, request.user)) request.xuids = xuid_list(k, arg(k, c, 1), arg(k, c, 2));
    if (uint32_t text = arg(k, c, 3); text && user_at(k, request.user)) request.text = guest_u16(k, text);
    result(c, live_page(k, request));
}

// (user_index, recipients, count, ...) and (user_index, flags, title, description, image, image_size, players,
// count, ...): the console's XAM answers ERROR_INVALID_PARAMETER to null arguments before it looks for a user
// (the oracle); what else it checks is not known, and the list's arguments are the XDK's documented ones
XF_EXPORT(xam, XamShowCustomMessageComposeUI) {
    if (!arg(k, c, 1) || !arg(k, c, 2)) return result(c, X_ERROR_INVALID_PARAMETER);
    UiRequest request = request_of(UiKind::CustomMessageCompose, arg(k, c, 0));
    if (user_at(k, request.user)) request.xuids = xuid_list(k, arg(k, c, 1), arg(k, c, 2));
    result(c, live_page(k, request));
}
XF_EXPORT(xam, XamShowCustomPlayerListUI) {
    if (!arg(k, c, 6) || !arg(k, c, 7)) return result(c, X_ERROR_INVALID_PARAMETER);
    UiRequest request = request_of(UiKind::CustomPlayerList, arg(k, c, 0));
    request.flags = arg(k, c, 1);
    if (user_at(k, request.user)) {
        if (uint32_t title = arg(k, c, 2)) request.title = guest_u16(k, title);
        if (uint32_t text = arg(k, c, 3)) request.text = guest_u16(k, text);
        request.xuids = xuid_list(k, arg(k, c, 6), arg(k, c, 7));
    }
    result(c, live_page(k, request));
}

void set_ui_handler(Kernel &k, UiHandler handler) {
    UiState &s = ui(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.handler = std::move(handler);
}

// ---- Kinect ----

// (user_index, tracking_id, flags): headless, 0
XF_EXPORT(xam, XamShowNuiTroubleshooterUI) { result(c, X_ERROR_SUCCESS); }

// (unknown): the troubleshooter, for 0 only
XF_EXPORT(xam, XamShowNuiHardwareRequiredUI) {
    result(c, arg(k, c, 0) != 0 ? X_ERROR_INVALID_PARAMETER : X_ERROR_SUCCESS);
}

// (tracking_id, unknown)
XF_EXPORT(xam, XamShowNuiGuideUI) { result(c, nui_hud_check()); }

} // namespace xf::kernel
