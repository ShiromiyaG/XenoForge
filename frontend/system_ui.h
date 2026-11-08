// The console's system dialogs on the player's window, the logic apart from the view (docs/runtime.md, "The
// player's system UI"). The kernel hands every dialog a title opens to a UiHandler (kernel.h); SystemUi is that
// handler's model: it queues the requests, makes each a Dialog (what the view draws: its title, text, choices, the
// selection), moves the selection with the pad's and the keyboard's navigation and, when the user answers, calls the
// request's UiDone with the answer in the kernel's result shapes (the button, the text, the device, or cancelled).
// system_ui_view.cpp draws a Dialog with Dear ImGui; nothing here includes it, so the tests drive the model alone.
//
// Threads: handle() and toast() are called from the guest's threads; navigate() and the rest from the window's
// thread, the one that draws. The dialog on top is changed only by the window's thread, so the view may edit it.
// The handler answers every dialog it is given (an unknown one is declined, so the kernel's headless default
// applies), and cancel_all() answers whatever is still up when the player closes.

#pragma once

#include "xenoforge/input.h"
#include "xenoforge/kernel.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xf::frontend {

// what moves the selection: the pad's D-pad (and stick), A, B, the shoulders; the keyboard's arrows, Enter, Escape
enum class Nav { Up, Down, Left, Right, Accept, Cancel, PageUp, PageDown };

// The pad's buttons as navigation, with the repeat a held direction has on the console's guide (after 400 ms, every
// 100 ms): one press gives one Nav, a direction held gives more.
class PadNavigator {
  public:
    // the user's controller state at `now_ms` (any monotonic clock); the navigation it gives, in order
    std::vector<Nav> feed(const input::Gamepad &pad, uint64_t now_ms);
    // forget what was held (a dialog opens: a button already down is not an answer)
    void reset(const input::Gamepad &pad, uint64_t now_ms);

  private:
    uint16_t held_ = 0;
    int direction_ = 0; // 0 none, else the Nav + 1 repeating
    uint64_t next_repeat_ = 0;
};

// One dialog on the screen. `lines` are its paragraphs; `choices` its buttons (a message box's, the device
// selector's items, "Close"), `selected` the focused one.
struct Dialog {
    kernel::UiKind kind = kernel::UiKind::Other;
    uint32_t user = 0xFF;
    std::string title;
    std::vector<std::string> lines;
    std::vector<std::string> choices;
    size_t selected = 0;
    int icon = 0; // a message box's: 0 none, 1 error, 2 warning, 3 information
    // a keyboard's: the text being edited (UTF-8, by the view) and its room in characters
    bool text_entry = false;
    std::string text;
    uint32_t max_length = 0;
    // the achievements page: the profile as it was when the dialog opened, and the row on top
    kernel::ProfileSummary profile;
    bool achievements = false;
    size_t first_row = 0;
    // how it ends: Accept on choice i gives this answer; Cancel gives `cancelled`
    kernel::UiRequest request;
    // the view's: when it was first drawn (its entrance), in the view's seconds; -1 before
    double shown_at = -1;
};

// a toast the window shows: an unlocked achievement or a notice, for a few seconds from the first time it is seen
struct ToastItem {
    kernel::Toast toast;
    double born = -1; // in the caller's seconds; set the first time toasts() sees it
};

class SystemUi {
  public:
    // the profile the achievements and gamer card pages show, read when one is requested (on the guest's thread:
    // reading the SPA needs a guest context)
    using ProfileSource = std::function<kernel::ProfileSummary()>;
    explicit SystemUi(ProfileSource profile);

    // The kernel's UiHandler: true when the request is taken (the answer comes through `done`, from navigate()
    // or cancel_all()), false for a request this does not know (the kernel answers it by default).
    bool handle(const kernel::UiRequest &request, kernel::UiDone done);
    // A toast to show (the kernel's ToastHandler)
    void toast(const kernel::Toast &toast);

    // whether a dialog is up: the game's input is not delivered meanwhile, the pad and keyboard go to the dialog
    bool active() const { return count_.load() > 0; }
    // the dialog on top (null when none); its pointer is good until the window's thread answers it
    Dialog *front();
    // the user whose controller drives the dialog: the user the title asked for, else 0
    uint32_t user() const;

    // The window's thread: one navigation step on the top dialog. Accept and Cancel answer it.
    void navigate(Nav nav);
    // the top dialog's text entered (the keyboard's: the view edited Dialog::text) and confirmed
    void confirm_text();
    // every dialog answered cancelled (the player is closing)
    void cancel_all();

    // the toasts to show now (the oldest first), expired ones dropped; `now` in seconds
    std::vector<ToastItem> toasts(double now, double lifetime = 5.0);

    // how many dialogs were taken and answered: for the player's report
    uint64_t taken() const { return taken_.load(); }
    uint64_t answered() const { return answered_.load(); }
    // whether a toast is waiting or showing
    bool has_toasts();

  private:
    struct Entry {
        std::unique_ptr<Dialog> dialog;
        kernel::UiDone done;
    };
    std::unique_ptr<Dialog> make(const kernel::UiRequest &request);
    // the answer for the top dialog; pops it first, then calls its done with no lock held
    void answer(bool cancelled);

    ProfileSource profile_;
    std::mutex mutex_;
    std::deque<Entry> queue_;
    std::vector<ToastItem> toasts_;
    std::atomic<int> count_{0};
    std::atomic<uint64_t> taken_{0}, answered_{0};
};

// text as the dialogs carry it: the guest's UTF-16 as UTF-8, and back (the keyboard's answer, cut to a room in
// characters)
std::string utf8_of(const std::u16string &text);
std::u16string utf16_of(const std::string &text, size_t max_chars = SIZE_MAX);

} // namespace xf::frontend
