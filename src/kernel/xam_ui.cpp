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
// A player overlay would take over in present() below (docs/runtime.md, "XAM").
// Kinect (NUI) is never there: its UI answers X_ERROR_ACCESS_DENIED, as
// Xenia's NuiHudCheck does with no sensor initialized.

#include "xam.h"

#include <cstdio>
#include <cstring>

namespace xf::kernel {
namespace {

bool signed_in(Kernel &k, uint32_t index) { return index < 4 && user_at(k, index) != nullptr; }

// What the UI does with a dialog: shows it (XN_SYS_UI on), makes its answer,
// hides it (XN_SYS_UI off). Headless, the answer is the dialog's default,
// made at once. With an overlapped, the answer completes it and the export
// returns X_ERROR_IO_PENDING (xeXamDispatchHeadless).
uint32_t present(Kernel &k, uint32_t ov, const std::function<uint32_t()> &answer) {
    broadcast(k, XN_SYS_UI, 1);
    uint32_t r = run_or_defer(k, ov, [&](uint32_t &extended_error, uint32_t &length) {
        uint32_t a = answer();
        extended_error = a, length = 0;
        return a;
    });
    broadcast(k, XN_SYS_UI, 0);
    return r;
}

// a dialog the export does not wait for (xeXamDispatchHeadlessAsync and
// xeXamDispatchDialogAsync): X_ERROR_SUCCESS, the answer made as it closes
uint32_t present_async(Kernel &k, const std::function<void()> &answer) {
    broadcast(k, XN_SYS_UI, 1);
    answer();
    broadcast(k, XN_SYS_UI, 0);
    return X_ERROR_SUCCESS;
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
uint32_t message_box(Kernel &k, uint32_t user, uint32_t button_count, uint32_t active_button, uint32_t flags,
                     uint32_t result_ptr, uint32_t ov) {
    constexpr uint32_t FLAGS_TAKEN = 0x003FF107;
    if (!ov || (user >= 4 && user != USER_INDEX_ANY) || button_count < 1 || button_count > 3 ||
        active_button >= button_count || !result_ptr || (flags & ~FLAGS_TAKEN))
        return X_ERROR_INVALID_PARAMETER;
    return present(k, ov, [&] {
        if (result_ptr) k.store32(result_ptr, active_button); // MESSAGEBOX_RESULT::ButtonPressed
        return X_ERROR_SUCCESS;
    });
}

// X_MARKETPLACE_ENTRYPOINT
constexpr uint32_t MARKETPLACE_FORCED_NAME_CHANGE_V2 = 8, MARKETPLACE_MAX_ENTRYPOINT = 25;

// XamShowMarketplaceUIEx's checks, then a page shown and closed
uint32_t marketplace(Kernel &k, uint32_t user_index, uint32_t ui_type) {
    if (user_index >= 4 || ui_type >= MARKETPLACE_MAX_ENTRYPOINT) return X_ERROR_INVALID_PARAMETER;
    if (!signed_in(k, user_index)) return X_ERROR_NO_SUCH_USER;
    return present_async(k, [] {});
}

// xeXamShowSigninUI: the profiles needed signed in (the one user is, in
// slot 0), told with XN_SYS_SIGNINCHANGED and the slots' mask
uint32_t signin(Kernel &k, uint32_t users_needed) {
    if (users_needed != 1 && users_needed != 2 && users_needed != 4) return X_ERROR_INVALID_PARAMETER;
    return present_async(k, [&] {
        uint32_t slots = 0;
        for (uint32_t i = 0, found = 0; i < 4 && found < users_needed; i++)
            if (user_at(k, i)) slots |= 1u << i, found++;
        broadcast(k, XN_SYS_SIGNINCHANGED, slots);
    });
}

} // namespace

XF_EXPORT(xam, XamIsUIActive) { result(c, 0); } // no dialog is ever up

// ---- message boxes ----

// (user_index, title, text, button_count, buttons, active_button, flags,
// result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUI) {
    result(c, message_box(k, arg(k, c, 0), arg(k, c, 3), arg(k, c, 5), arg(k, c, 6), arg(k, c, 7), arg(k, c, 8)));
}

// (user_index, title, text, button_count, buttons, active_button, flags,
// unused, result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUIEx) {
    result(c, message_box(k, arg(k, c, 0), arg(k, c, 3), arg(k, c, 5), arg(k, c, 6), arg(k, c, 8), arg(k, c, 9)));
}

// (tracking_id, user_index, title, text, ...): Kinect's
XF_EXPORT(xam, XamShowNuiMessageBoxUI) { result(c, nui_hud_check()); }

// (XNotifyQueueUI, in Xenia's xam_ui.cc, is with the notifications in
// xam_info.cpp)

// ---- the keyboard ----

// (user_index, flags, default_text, title, description, buffer, length,
// overlapped): the default text is the answer
XF_EXPORT(xam, XamShowKeyboardUI) {
    uint32_t default_text = arg(k, c, 2), buffer = arg(k, c, 5), length = arg(k, c, 6), ov = arg(k, c, 7);
    if (!buffer) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, present(k, ov, [&] {
               if (!default_text)
                   std::memset(k.host(buffer), 0, size_t(length) * 2);
               else
                   put_u16(k, buffer, guest_u16(k, default_text), length);
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
    result(c, present(k, ov, [&] {
               k.store32(device_ptr, 1); // the HDD (xam_content.cpp's devices)
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
    result(c, present(k, ov, [&] {
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
    if (!user_at(k, index)) return result(c, X_ERROR_NO_SUCH_USER);
    if (title && title != title_id(k)) return result(c, X_ERROR_NO_SUCH_USER);
    result(c, present_async(k, [] {}));
}

// (user_index): the user's gamer card, and the profile editor
XF_EXPORT(xam, XamShowGamerCardUI) {
    if (!user_at(k, arg(k, c, 0))) return result(c, X_ERROR_ACCESS_DENIED);
    result(c, present_async(k, [] {}));
}

XF_EXPORT(xam, XamShowEditProfileUI) {
    if (!user_at(k, arg(k, c, 0))) return result(c, X_ERROR_ACCESS_DENIED);
    result(c, present_async(k, [] {}));
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
