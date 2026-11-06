// XAM's system UI with no dialog drawn (src/kernel/xam_ui.cpp), driven
// through its exports as a game calls them: each answers with the dialog's
// default (the focused button, the default text, the hard drive, the user
// signed in), completes its overlapped (the result, the extended error, the
// event set) and sends XN_SYS_UI on and off around it; the checks Xenia
// makes first; Kinect's UI refused; the Live pages that fail.

#include "kernel_harness.h"

#include "objects.h"
#include "xam.h"

using namespace xf;
using namespace xf::kernel;

namespace {

KernelHarness h;
uint32_t listener = 0, id_ptr = 0, param_ptr = 0;

uint32_t xam(const char *name, std::initializer_list<uint64_t> args = {}) { return h.call32("xam", name, args); }

// the next notification queued, else id 0
std::pair<uint32_t, uint32_t> next() {
    if (!xam("XNotifyGetNext", {listener, 0, id_ptr, param_ptr})) return {0, 0};
    return {h.k->load32(id_ptr), h.k->load32(param_ptr)};
}

// XN_SYS_UI on, then off: a dialog shown and closed
bool shown_and_closed() {
    auto on = next(), off = next();
    return on == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 1} && off == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 0};
}
bool nothing_queued() { return next().first == 0; }

// a XAM_OVERLAPPED with an event, cleared
struct Overlapped {
    uint32_t address = 0;
    Ref<Event> event;
};
Overlapped overlapped() {
    Overlapped ov;
    ov.address = h.alloc(0x1C);
    std::memset(h.k->host(ov.address), 0xEE, 0x1C);
    ov.event = objects(*h.k).create<Event>(GuestType::EventNotification, header::SIZE);
    h.k->store32(ov.address + xoverlapped::EVENT, ov.event->handle());
    h.k->store32(ov.address + xoverlapped::COMPLETION_ROUTINE, 0);
    return ov;
}
bool completed(const Overlapped &ov, uint32_t result, uint32_t extended_error) {
    return h.k->load32(ov.address + xoverlapped::RESULT) == result &&
           h.k->load32(ov.address + xoverlapped::EXTENDED_ERROR) == extended_error && ov.event->signal_state() == 1;
}

void message_boxes() {
    CHECK(xam("XamIsUIActive") == 0);
    // (user_index, title, text, button_count, buttons, active_button, flags,
    // result, overlapped): the focused button
    uint32_t box = h.alloc(4);
    h.k->store32(box, 0xEEEEEEEE);
    CHECK(xam("XamShowMessageBoxUI", {0, h.wide(u"Title"), h.wide(u"Text"), 0, 0, 2, 0, box, 0}) == X_ERROR_SUCCESS);
    CHECK(h.k->load32(box) == 2 && shown_and_closed());
    // the Ex form, its overlapped tenth (on the stack)
    Overlapped ov = overlapped();
    CHECK(xam("XamShowMessageBoxUIEx", {0, 0, 0, 0, 0, 1, 0, 0, box, ov.address}) == X_ERROR_IO_PENDING);
    CHECK(h.k->load32(box) == 1 && completed(ov, X_ERROR_SUCCESS, X_ERROR_SUCCESS) &&
          h.k->load32(ov.address + xoverlapped::LENGTH) == 0);
    CHECK(shown_and_closed());
    // Kinect's: no sensor
    CHECK(xam("XamShowNuiMessageBoxUI", {1, 0, 0, 0, 0, 0, 0, 0, box, 0}) == X_ERROR_ACCESS_DENIED);
    CHECK(nothing_queued());
}

void keyboard() {
    uint32_t buffer = h.alloc(64);
    std::memset(h.k->host(buffer), 0xEE, 64);
    Overlapped ov = overlapped();
    // (user_index, flags, default_text, title, description, buffer, length,
    // overlapped): the default text, truncated with its terminator
    CHECK(xam("XamShowKeyboardUI", {0, 0, h.wide(u"Marcus"), 0, 0, buffer, 4, ov.address}) == X_ERROR_IO_PENDING);
    CHECK(h.k->load16(buffer) == 'M' && h.k->load16(buffer + 4) == 'r' && h.k->load16(buffer + 6) == 0);
    CHECK(completed(ov, X_ERROR_SUCCESS, X_ERROR_SUCCESS) && shown_and_closed());
    // none: an empty answer
    CHECK(xam("XamShowKeyboardUI", {0, 0, 0, 0, 0, buffer, 4, 0}) == X_ERROR_SUCCESS);
    CHECK(h.k->load32(buffer) == 0 && h.k->load32(buffer + 4) == 0 && shown_and_closed());
    CHECK(xam("XamShowKeyboardUI", {0, 0, 0, 0, 0, 0, 4, 0}) == X_ERROR_INVALID_PARAMETER && nothing_queued());
}

void storage() {
    uint32_t device = h.alloc(4);
    h.k->store32(device, 0);
    // (user_index, content_type, content_flags, total_requested, device_id_ptr,
    // overlapped): the hard drive
    Overlapped ov = overlapped();
    CHECK(xam("XamShowDeviceSelectorUI", {0xFF, 1, 0, 0x10000, device, ov.address}) == X_ERROR_IO_PENDING);
    CHECK(h.k->load32(device) == 1 && completed(ov, X_ERROR_SUCCESS, X_ERROR_SUCCESS) && shown_and_closed());
    // an overlapped is required; bad flags fail with the extended error set
    CHECK(xam("XamShowDeviceSelectorUI", {0, 1, 0, 0, device, 0}) == X_ERROR_INVALID_PARAMETER);
    Overlapped bad = overlapped();
    CHECK(xam("XamShowDeviceSelectorUI", {0, 1, 8, 0, device, bad.address}) == X_ERROR_INVALID_PARAMETER &&
          h.k->load32(bad.address + xoverlapped::EXTENDED_ERROR) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowDeviceSelectorUI", {5, 1, 0, 0, device, bad.address}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowNuiDeviceSelectorUI", {1, 0, 1, 0, 0, device, ov.address}) == X_ERROR_ACCESS_DENIED);
    CHECK(xam("XamShowDirtyDiscErrorUI", {0}) == X_ERROR_SUCCESS);
    CHECK(xam("XamShowNuiDirtyDiscErrorUI", {1}) == X_ERROR_ACCESS_DENIED);
    CHECK(nothing_queued());
}

void live() {
    CHECK(xam("XamShowPartyUI", {0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam("XamShowCommunitySessionsUI", {0, 0}) == X_ERROR_FUNCTION_FAILED);
    // (user_index, ui_type, offer_id, offer_type, category, title_id)
    CHECK(xam("XamShowMarketplaceUI", {0, 1, 0x4D5307D100000001ull, 0xFFFFFFFF, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(shown_and_closed());
    CHECK(xam("XamShowMarketplaceUI", {4, 0, 0, 0, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowMarketplaceUI", {0, 25, 0, 0, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowMarketplaceUI", {1, 0, 0, 0, 0, 0}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam("XamShowMarketplaceUIEx", {0, 0, 0, 0xFFFFFFFF, 0, 0, 0, 0}) == X_ERROR_SUCCESS && shown_and_closed());
    // (user_index, entry_point, offers, offer_count, hresult_ptr, overlapped)
    uint32_t offers = h.alloc(8), hresult = h.alloc(4);
    h.k->store32(hresult, 0xEEEEEEEE);
    Overlapped ov = overlapped();
    CHECK(xam("XamShowMarketplaceDownloadItemsUI", {0, 0, offers, 1, hresult, ov.address}) == X_ERROR_IO_PENDING);
    CHECK(h.k->load32(hresult) == X_E_SUCCESS && completed(ov, X_ERROR_SUCCESS, X_ERROR_SUCCESS) && shown_and_closed());
    CHECK(xam("XamShowMarketplaceDownloadItemsUI", {0, 0, offers, 7, hresult, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowMarketplaceDownloadItemsUI", {0, 0, 0, 1, hresult, 0}) == X_ERROR_INVALID_PARAMETER);
    // nobody in slot 1: completed at once with the error
    Overlapped nobody = overlapped();
    CHECK(xam("XamShowMarketplaceDownloadItemsUI", {1, 0, offers, 1, hresult, nobody.address}) == X_ERROR_IO_PENDING);
    CHECK(completed(nobody, X_ERROR_NO_SUCH_USER, X_ERROR_NO_SUCH_USER) &&
          h.k->load32(nobody.address + xoverlapped::LENGTH) == 0xFFFFFFFF);
    CHECK(xam("XamShowMarketplaceDownloadItemsUI", {1, 0, offers, 1, hresult, 0}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam("XamShowGoldUpgradeUI", {0, 0, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam("XamShowMultiplayerUpgradeUI", {0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam("XamShowForcedNameChangeUI", {0}) == X_ERROR_SUCCESS && shown_and_closed());
    CHECK(xam("XamShowForcedNameChangeUI", {1}) == X_ERROR_NO_SUCH_USER);
    CHECK(nothing_queued());
}

void profiles() {
    // (users_needed, flags): the user in slot 0, told
    CHECK(xam("XamShowSigninUI", {3, 0}) == X_ERROR_INVALID_PARAMETER && nothing_queued());
    CHECK(xam("XamShowSigninUI", {1, 0}) == X_ERROR_SUCCESS);
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 1}));
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_SIGNINCHANGED, 1}));
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 0}));
    // only one user to find for two
    CHECK(xam("XamShowSigninUIp", {0, 2, 0}) == X_ERROR_SUCCESS);
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 1}));
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_SIGNINCHANGED, 1}));
    CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 0}));
    // the Ex form completes its overlapped with the answer
    Overlapped ov = overlapped();
    CHECK(xam("XamShowSigninUIEx", {1, 0, ov.address}) == X_ERROR_IO_PENDING);
    CHECK(completed(ov, X_ERROR_SUCCESS, X_ERROR_SUCCESS) && h.k->load32(ov.address + xoverlapped::LENGTH) == 0);
    CHECK(next().first == XN_SYS_UI && next().first == XN_SYS_SIGNINCHANGED && next().first == XN_SYS_UI);
    Overlapped bad = overlapped();
    CHECK(xam("XamShowSigninUIEx", {3, 0, bad.address}) == X_ERROR_IO_PENDING);
    CHECK(completed(bad, X_ERROR_INVALID_PARAMETER, X_ERROR_INVALID_PARAMETER));
    CHECK(xam("XamShowNuiSigninUI", {1, 0, 0}) == X_ERROR_ACCESS_DENIED);
    // headless: no dialog, no notification
    CHECK(xam("XamShowCreateProfileUI", {0, 0}) == X_ERROR_SUCCESS);
    CHECK(xam("XamShowCreateProfileUIEx", {0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(nothing_queued());

    // the running title's achievements; a title never played has no record
    uint32_t title = xam("XamGetCurrentTitleId");
    CHECK(xam("XamShowAchievementsUI", {0, 0}) == X_ERROR_SUCCESS && shown_and_closed());
    if (title) CHECK(xam("XamShowAchievementsUI", {0, title}) == X_ERROR_SUCCESS && shown_and_closed());
    CHECK(xam("XamShowAchievementsUI", {0, title ^ 0x12345678}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam("XamShowAchievementsUI", {1, 0}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam("XamShowGamerCardUI", {0}) == X_ERROR_SUCCESS && shown_and_closed());
    CHECK(xam("XamShowGamerCardUI", {1}) == X_ERROR_ACCESS_DENIED);
    CHECK(xam("XamShowEditProfileUI", {0}) == X_ERROR_SUCCESS && shown_and_closed());
    CHECK(xam("XamShowEditProfileUI", {2}) == X_ERROR_ACCESS_DENIED);
    CHECK(nothing_queued());
}

void kinect() {
    CHECK(xam("XamShowNuiTroubleshooterUI", {0xFF, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(xam("XamShowNuiHardwareRequiredUI", {0}) == X_ERROR_SUCCESS);
    CHECK(xam("XamShowNuiHardwareRequiredUI", {1}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam("XamShowNuiGuideUI", {1, 0}) == X_ERROR_ACCESS_DENIED);
    CHECK(nothing_queued());
}

} // namespace

int main() {
    if (!h.init()) return 1;
    {
        id_ptr = h.alloc(4), param_ptr = h.alloc(4);
        // a listener for the system's notifications; the ones every first
        // listener gets at start drained
        listener = xam("XamNotifyCreateListener", {1, 1});
        CHECK(listener != 0);
        CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_UI, 0}));
        CHECK((next() == std::pair<uint32_t, uint32_t>{XN_SYS_SIGNINCHANGED, 1}));
        CHECK(nothing_queued());

        message_boxes();
        keyboard();
        storage();
        live();
        profiles();
        kinect();
    }
    return finish("kernel_ui");
}
