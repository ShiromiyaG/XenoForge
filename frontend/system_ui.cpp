// The model of the system's dialogs (system_ui.h): each request becomes a Dialog with the text the offline console
// shows, the navigation moves its selection, and the answer goes back through the request's UiDone.
//
// What an offline console shows for each page, and how this differs from Xenia: Xenia draws a message box, the
// keyboard, the device selector (its ImGui dialogs) and the sign-in and cancels or stubs the rest (headless it
// answers the defaults); here every page the kernel asks for is shown: the gamer card and the achievements from
// the profile's GPD, and the Xbox Live pages (friends, players, messages, quick chat, invites, requests, reviews,
// compose, the marketplace) as the console with no Live account shows them, with nothing to list and no way to send.

#include "system_ui.h"

#include <algorithm>
#include <cstdio>

namespace xf::frontend {

namespace {

constexpr uint64_t REPEAT_DELAY_MS = 400, REPEAT_EVERY_MS = 100;
constexpr int16_t STICK_THRESHOLD = 16000; // about half of the stick's range

// the navigation a pad's buttons give: directions (the D-pad, the left stick) repeat, the rest press once
uint16_t nav_bits(const input::Gamepad &p) {
    uint16_t b = p.buttons;
    if (p.thumb_ly > STICK_THRESHOLD) b |= input::DPAD_UP;
    if (p.thumb_ly < -STICK_THRESHOLD) b |= input::DPAD_DOWN;
    if (p.thumb_lx < -STICK_THRESHOLD) b |= input::DPAD_LEFT;
    if (p.thumb_lx > STICK_THRESHOLD) b |= input::DPAD_RIGHT;
    return b;
}

std::string hex_xuid(uint64_t xuid) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)xuid);
    return text;
}

std::string gamerscore_text(uint32_t earned, uint32_t total) {
    return std::to_string(earned) + " / " + std::to_string(total) + " G";
}

} // namespace

std::string utf8_of(const std::u16string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t c = text[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string utf16_of(const std::string &text, size_t max_chars) {
    std::u16string out;
    size_t chars = 0;
    for (size_t i = 0; i < text.size() && chars < max_chars; chars++) {
        unsigned char b = text[i];
        uint32_t c = b;
        size_t n = 1;
        if (b >= 0xF0 && b < 0xF8)
            c = b & 7, n = 4;
        else if (b >= 0xE0)
            c = b & 15, n = 3;
        else if (b >= 0xC0)
            c = b & 31, n = 2;
        for (size_t j = 1; j < n; j++)
            c = (c << 6) | (i + j < text.size() ? (unsigned char)text[i + j] & 0x3F : 0);
        i += n;
        if (c >= 0x10000)
            out += char16_t(0xD800 + ((c - 0x10000) >> 10)), out += char16_t(0xDC00 + ((c - 0x10000) & 0x3FF));
        else
            out += char16_t(c);
    }
    return out;
}

// ---- the pad ----

std::vector<Nav> PadNavigator::feed(const input::Gamepad &pad, uint64_t now_ms) {
    struct Map {
        uint16_t bit;
        Nav nav;
    };
    static const Map map[] = {
        {input::DPAD_UP, Nav::Up},   {input::DPAD_DOWN, Nav::Down},
        {input::DPAD_LEFT, Nav::Left}, {input::DPAD_RIGHT, Nav::Right},
        {input::A, Nav::Accept},     {input::B, Nav::Cancel},
        {input::LEFT_SHOULDER, Nav::PageUp}, {input::RIGHT_SHOULDER, Nav::PageDown},
    };
    const uint16_t now = nav_bits(pad);
    std::vector<Nav> out;
    int direction = 0;
    for (const Map &m : map) {
        const bool down = now & m.bit, was = held_ & m.bit;
        const bool is_direction = m.nav <= Nav::Right;
        if (down && !was) {
            out.push_back(m.nav);
            if (is_direction) {
                direction_ = int(m.nav) + 1;
                next_repeat_ = now_ms + REPEAT_DELAY_MS;
            }
        }
        if (down && is_direction && direction_ == int(m.nav) + 1) direction = direction_;
    }
    if (!direction) {
        direction_ = 0;
    } else if (now_ms >= next_repeat_) {
        out.push_back(Nav(direction - 1));
        next_repeat_ = now_ms + REPEAT_EVERY_MS;
    }
    held_ = now;
    return out;
}

void PadNavigator::reset(const input::Gamepad &pad, uint64_t) {
    held_ = nav_bits(pad);
    direction_ = 0;
}

// ---- the dialogs ----

SystemUi::SystemUi(ProfileSource profile) : profile_(std::move(profile)) {}

std::unique_ptr<Dialog> SystemUi::make(const kernel::UiRequest &r) {
    using kernel::UiKind;
    auto d = std::make_unique<Dialog>();
    d->kind = r.kind, d->user = r.user, d->request = r;
    auto profile = [&] { return profile_ ? profile_() : kernel::ProfileSummary{}; };
    switch (r.kind) {
    case UiKind::MessageBox:
        d->title = r.title.empty() ? "Message" : utf8_of(r.title);
        d->icon = int(r.flags & 0xF) <= 3 ? int(r.flags & 0xF) : 3;
        d->lines.push_back(utf8_of(r.text));
        for (const auto &b : r.buttons) d->choices.push_back(utf8_of(b));
        if (d->choices.empty()) d->choices.push_back("OK");
        d->selected = std::min<size_t>(r.default_button, d->choices.size() - 1);
        break;
    case UiKind::Keyboard:
        d->title = r.title.empty() ? "Keyboard" : utf8_of(r.title);
        if (!r.text.empty()) d->lines.push_back(utf8_of(r.text));
        d->text_entry = true;
        d->text = utf8_of(r.default_text);
        d->max_length = r.max_length;
        d->choices = {"OK", "Cancel"};
        break;
    case UiKind::DeviceSelector: {
        d->title = "Select a storage device";
        if (r.bytes_required)
            d->lines.push_back("Space needed: " + std::to_string((r.bytes_required + (1u << 20) - 1) >> 20) + " MB");
        d->lines.push_back("The console's hard drive is the only device here.");
        d->choices = {"Hard Drive", "Cancel"};
        break;
    }
    case UiKind::SignIn: {
        kernel::ProfileSummary p = profile();
        d->title = "Sign in";
        d->lines.push_back("Profile: " + p.gamertag + " (offline, on this console)");
        if (r.users_needed > 1)
            d->lines.push_back("The title wants " + std::to_string(r.users_needed) +
                               " profiles; this console has one.");
        d->choices = {"Sign in as " + p.gamertag, "Cancel"};
        break;
    }
    case UiKind::GamerCard: {
        kernel::ProfileSummary p = profile();
        d->title = "Gamer card";
        if (r.xuid && r.xuid != p.xuid) {
            d->lines.push_back("Player " + hex_xuid(r.xuid));
            d->lines.push_back("Not available offline: this player's gamer card is kept on Xbox Live.");
        } else {
            d->lines.push_back("Gamertag: " + p.gamertag);
            d->lines.push_back("XUID: " + hex_xuid(p.xuid));
            if (p.title_id) {
                char id[16];
                std::snprintf(id, sizeof(id), "%08X", p.title_id);
                d->lines.push_back(std::string("Gamerscore in this title (") + id +
                                   "): " + gamerscore_text(p.gamerscore, p.gamerscore_total));
            }
            d->lines.push_back("Offline: reputation, zone and recent players are not available.");
        }
        d->choices = {"Close"};
        break;
    }
    case UiKind::Achievements: {
        d->profile = profile();
        d->achievements = true;
        d->title = "Achievements";
        d->lines.push_back(d->profile.gamertag + ": " +
                           gamerscore_text(d->profile.gamerscore, d->profile.gamerscore_total));
        d->choices = {"Close"};
        break;
    }
    case UiKind::EditProfile:
        d->title = "Edit profile";
        d->lines.push_back("The profile is kept on this console: its gamertag is config.toml's [profile].");
        d->choices = {"Close"};
        break;
    case UiKind::Marketplace:
        d->title = "Xbox Live Marketplace";
        d->lines.push_back("Not available: this console is offline and not signed in to Xbox Live.");
        d->choices = {"Close"};
        break;
    case UiKind::Friends:
        d->title = "Friends";
        d->lines.push_back("You are not signed in to Xbox Live: no friends are listed.");
        d->choices = {"Close"};
        break;
    case UiKind::Messages:
        d->title = "Messages";
        d->lines.push_back("You are not signed in to Xbox Live: no messages.");
        d->choices = {"Close"};
        break;
    case UiKind::Players:
        d->title = "Players";
        d->lines.push_back("You are not signed in to Xbox Live: no recent players.");
        d->choices = {"Close"};
        break;
    case UiKind::QuickChat:
        d->title = "Quick chat";
        d->lines.push_back("Not available offline.");
        d->choices = {"Close"};
        break;
    case UiKind::GameInvite:
        d->title = "Game invite";
        d->lines.push_back("You are not signed in to Xbox Live: invites cannot be sent.");
        d->choices = {"Close"};
        break;
    case UiKind::FriendRequest:
        d->title = "Friend request";
        d->lines.push_back("You are not signed in to Xbox Live: " + (r.xuid ? hex_xuid(r.xuid) : std::string("the player")) +
                           " cannot be sent a friend request.");
        d->choices = {"Close"};
        break;
    case UiKind::PlayerReview:
        d->title = "Player review";
        d->lines.push_back("You are not signed in to Xbox Live: " + (r.xuid ? hex_xuid(r.xuid) : std::string("the player")) +
                           " cannot be reviewed.");
        d->choices = {"Close"};
        break;
    case UiKind::MessageCompose:
    case UiKind::CustomMessageCompose:
        d->title = "New message";
        for (uint64_t x : r.xuids) d->lines.push_back("To: " + hex_xuid(x));
        if (!r.text.empty()) d->lines.push_back(utf8_of(r.text));
        d->lines.push_back("You are not signed in to Xbox Live: messages cannot be sent.");
        d->choices = {"Close"};
        break;
    case UiKind::CustomPlayerList:
        d->title = "Players";
        for (uint64_t x : r.xuids) d->lines.push_back("Player " + hex_xuid(x));
        d->lines.push_back("Names are not available offline.");
        d->choices = {"Close"};
        break;
    case UiKind::Other:
        return nullptr;
    }
    return d;
}

bool SystemUi::handle(const kernel::UiRequest &request, kernel::UiDone done) {
    std::unique_ptr<Dialog> d = make(request);
    if (!d) return false;
    {
        std::lock_guard<std::mutex> l(mutex_);
        queue_.push_back({std::move(d), std::move(done)});
        count_++;
    }
    taken_++;
    return true;
}

void SystemUi::toast(const kernel::Toast &toast) {
    std::lock_guard<std::mutex> l(mutex_);
    toasts_.push_back({toast, -1});
}

Dialog *SystemUi::front() {
    std::lock_guard<std::mutex> l(mutex_);
    return queue_.empty() ? nullptr : queue_.front().dialog.get();
}

uint32_t SystemUi::user() const {
    auto *self = const_cast<SystemUi *>(this);
    std::lock_guard<std::mutex> l(self->mutex_);
    return queue_.empty() || queue_.front().dialog->user >= input::USERS ? 0 : queue_.front().dialog->user;
}

void SystemUi::answer(bool cancelled) {
    Entry e;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (queue_.empty()) return;
        e = std::move(queue_.front());
        queue_.pop_front();
    }
    const Dialog &d = *e.dialog;
    kernel::UiResult r;
    r.cancelled = cancelled;
    if (!cancelled) {
        switch (d.kind) {
        case kernel::UiKind::MessageBox:
            r.button = uint32_t(d.selected);
            break;
        case kernel::UiKind::Keyboard:
            r.text = utf16_of(d.text, d.max_length ? d.max_length : SIZE_MAX);
            break;
        case kernel::UiKind::DeviceSelector:
            r.device_id = 1; // the hard drive
            break;
        default:
            break;
        }
    }
    answered_++;
    // answered with no lock held: the kernel completes the title's overlapped from here
    if (e.done) e.done(r);
    count_--;
}

void SystemUi::navigate(Nav nav) {
    Dialog *d = front();
    if (!d) return;
    if (nav == Nav::Cancel) return answer(true);
    if (d->achievements) {
        const size_t rows = d->profile.achievements.size();
        auto move = [&](int delta) {
            if (!rows) return;
            long at = long(d->first_row) + delta;
            d->first_row = size_t(std::clamp<long>(at, 0, long(rows) - 1));
        };
        switch (nav) {
        case Nav::Up: return move(-1);
        case Nav::Down: return move(1);
        case Nav::PageUp: return move(-6);
        case Nav::PageDown: return move(6);
        case Nav::Accept: return answer(false);
        default: return;
        }
    }
    switch (nav) {
    case Nav::Up:
    case Nav::Left:
        if (d->selected > 0) d->selected--;
        break;
    case Nav::Down:
    case Nav::Right:
        if (d->selected + 1 < d->choices.size()) d->selected++;
        break;
    case Nav::Accept:
        // the keyboard's and the device selector's second choice, the sign-in's second: Cancel
        if ((d->kind == kernel::UiKind::Keyboard || d->kind == kernel::UiKind::DeviceSelector ||
             d->kind == kernel::UiKind::SignIn) &&
            d->selected == 1)
            return answer(true);
        return answer(false);
    default:
        break;
    }
}

void SystemUi::confirm_text() {
    Dialog *d = front();
    if (d && d->text_entry) answer(false);
}

void SystemUi::cancel_all() {
    while (front()) answer(true);
}

bool SystemUi::has_toasts() {
    std::lock_guard<std::mutex> l(mutex_);
    return !toasts_.empty();
}

std::vector<ToastItem> SystemUi::toasts(double now, double lifetime) {
    std::lock_guard<std::mutex> l(mutex_);
    for (ToastItem &t : toasts_)
        if (t.born < 0) t.born = now;
    std::erase_if(toasts_, [&](const ToastItem &t) { return now - t.born >= lifetime; });
    return toasts_;
}

} // namespace xf::frontend
