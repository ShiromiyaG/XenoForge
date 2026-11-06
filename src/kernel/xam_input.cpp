/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's input (xenoforge/input.h; docs/input.md): the XamInput* exports and
// the input system behind them, asking its drivers in order. Translated from
// Xenia (BSD; see LICENSE-xenia): kernel/xam/xam_input.cc (the exports),
// hid/input_system.cc (the drivers asked in order, the connected slots and
// their notification, the deadzones), hid/sdl/sdl_input_driver.cc
// (GetKeystroke's buttons-to-keys, AnalogToKeyfield, the capabilities);
// checked against them. Where it differs:
//
// - The deadzones scale by the sticks' full range, 32767 (Xenia scales by
//   the capabilities' thumb values, which its drivers report as -1, so a
//   deadzone it is given mostly does nothing).
// - The UI being up does not discard the state (Xenia's IsUIActive): the
//   guide's UI here is answered at once, never up while the game polls.
// - A state asked for with no buffer is read and dropped (Xenia hands its
//   drivers the null pointer).
// - The drivers get the scheduler's clock (the keystrokes' repeats; Xenia's
//   guest uptime, the same clock).

#include "xam.h"
#include "xenoforge/input.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace xf::input {

Capabilities gamepad_capabilities() {
    Capabilities c;
    c.type = 1;                         // XINPUT_DEVTYPE_GAMEPAD
    c.sub_type = 1;                     // XINPUT_DEVSUBTYPE_GAMEPAD
    c.flags = 0;                        // wired, with the navigation buttons
    c.gamepad.buttons = 0xF3FF | GUIDE; // Xenia's guide_button on by default
    c.gamepad.left_trigger = c.gamepad.right_trigger = 0xFF;
    c.gamepad.thumb_lx = c.gamepad.thumb_ly = c.gamepad.thumb_rx = c.gamepad.thumb_ry = -1;
    c.vibration.left_motor_speed = c.vibration.right_motor_speed = 0xFFFF;
    return c;
}

Gamepad combine(const Gamepad &a, const Gamepad &b) {
    auto further = [](int16_t x, int16_t y) { return std::abs(int(y)) > std::abs(int(x)) ? y : x; };
    Gamepad g;
    g.buttons = a.buttons | b.buttons;
    g.left_trigger = std::max(a.left_trigger, b.left_trigger);
    g.right_trigger = std::max(a.right_trigger, b.right_trigger);
    g.thumb_lx = further(a.thumb_lx, b.thumb_lx), g.thumb_ly = further(a.thumb_ly, b.thumb_ly);
    g.thumb_rx = further(a.thumb_rx, b.thumb_rx), g.thumb_ry = further(a.thumb_ry, b.thumb_ry);
    return g;
}

namespace {

// HID_SDL_THUMB_THRES, HID_SDL_TRIGG_THRES, HID_SDL_REPEAT_DELAY and _RATE
constexpr int16_t THUMB_THRESHOLD = 0x4E00;
constexpr uint8_t TRIGGER_THRESHOLD = 0x1F;
constexpr uint64_t REPEAT_DELAY_MS = 400, REPEAT_RATE_MS = 100;

// the keys of the bits of buttons | analog_bits, in the order they are sent
// when several change at once (kVkLookup); 0 for the button with no key
constexpr std::array<uint16_t, 34> KEYS = {
    // the buttons' bits
    vk::DPAD_UP, vk::DPAD_DOWN, vk::DPAD_LEFT, vk::DPAD_RIGHT, vk::START, vk::BACK, vk::LTHUMB_PRESS, vk::RTHUMB_PRESS,
    vk::LSHOULDER, vk::RSHOULDER, vk::GUIDE, 0, vk::A, vk::B, vk::X, vk::Y,
    // 16: the triggers past their threshold
    vk::LTRIGGER, vk::RTRIGGER,
    // 18: the left stick's directions, then 26: the right one's
    vk::LTHUMB_UP, vk::LTHUMB_DOWN, vk::LTHUMB_RIGHT, vk::LTHUMB_LEFT, vk::LTHUMB_UPLEFT, vk::LTHUMB_UPRIGHT,
    vk::LTHUMB_DOWNRIGHT, vk::LTHUMB_DOWNLEFT, vk::RTHUMB_UP, vk::RTHUMB_DOWN, vk::RTHUMB_RIGHT, vk::RTHUMB_LEFT,
    vk::RTHUMB_UPLEFT, vk::RTHUMB_UPRIGHT, vk::RTHUMB_DOWNRIGHT, vk::RTHUMB_DOWNLEFT};

// AnalogToKeyfield: the triggers and the sticks' eight directions as bits
uint64_t analog_bits(const Gamepad &g) {
    uint64_t f = 0;
    f |= uint64_t(g.left_trigger > TRIGGER_THRESHOLD) << 16;
    f |= uint64_t(g.right_trigger > TRIGGER_THRESHOLD) << 17;
    int16_t x = g.thumb_lx, y = g.thumb_ly;
    for (int i = 0; i <= 8; i += 8) {
        uint64_t u = y > THUMB_THRESHOLD, d = y < ~THUMB_THRESHOLD;
        uint64_t r = x > THUMB_THRESHOLD, l = x < ~THUMB_THRESHOLD;
        if (u && l) u = l = 0, f |= uint64_t(1) << (22 + i);
        if (u && r) u = r = 0, f |= uint64_t(1) << (23 + i);
        if (d && r) d = r = 0, f |= uint64_t(1) << (24 + i);
        if (d && l) d = l = 0, f |= uint64_t(1) << (25 + i);
        f |= u << (18 + i) | d << (19 + i) | r << (20 + i) | l << (21 + i);
        x = g.thumb_rx, y = g.thumb_ry;
    }
    return f;
}

} // namespace

uint32_t PadKeystrokes::next(const Gamepad &pad, uint32_t user, uint64_t now_ms, Keystroke &out) {
    const uint64_t now_buttons = pad.buttons | analog_bits(pad);
    out = Keystroke{};
    out.user_index = uint8_t(user);
    // a key held: repeated after the delay, then at the rate
    if (repeat_ == Repeat::Waiting && repeat_time_ + REPEAT_DELAY_MS < now_ms) repeat_ = Repeat::Repeating;
    if (repeat_ == Repeat::Repeating && repeat_time_ + REPEAT_RATE_MS < now_ms) {
        repeat_time_ = now_ms;
        out.virtual_key = KEYS[repeat_index_];
        out.flags = KEYSTROKE_KEYDOWN | KEYSTROKE_REPEAT;
        return SUCCESS;
    }
    const uint64_t changed = now_buttons ^ buttons_;
    if (!changed) return EMPTY;
    // the ups first (a stick going from up-left to left lets up-left go
    // before left goes down, as XInput does), then the downs
    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < KEYS.size(); i++) {
            const uint64_t bit = uint64_t(1) << i;
            if (!(changed & bit) || !KEYS[i]) continue;
            const bool pressed = now_buttons & bit;
            out.virtual_key = KEYS[i];
            if (pass == 0 && !pressed) {
                out.flags = KEYSTROKE_KEYUP;
                buttons_ &= ~bit;
                repeat_ = Repeat::Idle;
                return SUCCESS;
            }
            if (pass == 1 && pressed) {
                out.flags = KEYSTROKE_KEYDOWN;
                buttons_ |= bit;
                repeat_ = Repeat::Waiting;
                repeat_index_ = i;
                repeat_time_ = now_ms;
                return SUCCESS;
            }
        }
    }
    out = Keystroke{};
    return EMPTY;
}

namespace {

// boot's controller: user 0, nothing pressed but the overlay's
class IdlePad final : public Driver {
  public:
    explicit IdlePad(Overlay overlay) : overlay_(std::move(overlay)) {}
    uint32_t capabilities(uint32_t user, uint32_t, Capabilities &out) override {
        if (user != 0) return DEVICE_NOT_CONNECTED;
        out = gamepad_capabilities();
        return SUCCESS;
    }
    uint32_t state(uint32_t user, uint64_t now_ms, State &out) override {
        if (user != 0) return DEVICE_NOT_CONNECTED;
        out.gamepad = pad(now_ms);
        out.packet_number = packets_.next(out.gamepad);
        return SUCCESS;
    }
    uint32_t vibrate(uint32_t user, const Vibration &) override { return user == 0 ? SUCCESS : DEVICE_NOT_CONNECTED; }
    uint32_t keystroke(uint32_t user, uint32_t, uint64_t now_ms, Keystroke &out) override {
        if (user != 0) return DEVICE_NOT_CONNECTED;
        return keys_.next(pad(now_ms), user, now_ms, out);
    }

  private:
    Gamepad pad(uint64_t now_ms) const { return overlay_ ? overlay_(0, now_ms) : Gamepad{}; }
    Overlay overlay_;
    Packets packets_;
    PadKeystrokes keys_;
};

} // namespace

std::unique_ptr<Driver> make_idle_pad(Overlay overlay) { return std::make_unique<IdlePad>(std::move(overlay)); }

} // namespace xf::input

namespace xf::kernel {
namespace {

using namespace input;

constexpr uint32_t XN_SYS_INPUTDEVICESCHANGED = 0x00000012;

// Xenia's InputSystem: the drivers in order, the users seen connected, the
// deadzones, what was answered
struct InputSystem {
    std::mutex mutex;
    std::vector<std::unique_ptr<Driver>> drivers;
    std::bitset<USERS> connected;
    double deadzone[2] = {0, 0}; // left, right
    uint64_t last_activity = 0; // XamResetInactivity's: the interrupt time then
    uint64_t states[USERS] = {}, keystrokes[USERS] = {}, vibrations = 0, changes = 0, not_connected = 0;

    std::vector<Driver *> filter(uint32_t flags) {
        std::vector<Driver *> out;
        for (auto &d : drivers)
            if (d->type() != Type::None && (flags & uint32_t(d->type()))) out.push_back(d.get());
        return out;
    }

    // UpdateUsedSlot: true when the user's connection changed (the
    // notification goes out once the lock is let go); the keyboard's
    // passthrough is not a controller
    bool update_slot(const Driver *d, uint32_t user, bool now_connected) {
        if (user >= USERS || (d && d->type() == Type::Keyboard)) return false;
        if (connected.test(user) == now_connected) return false;
        connected.flip(user);
        changes++;
        std::fprintf(stderr, "input: controller %u %s\n", user, now_connected ? "connected" : "disconnected");
        return true;
    }

    // AdjustDeadzoneLevels: an axis inside the deadzone, scaled along the
    // stick's angle, reads 0
    void apply_deadzones(Gamepad &g) const {
        auto stick = [](double fraction, int16_t &x, int16_t &y) {
            if (fraction <= 0 || fraction >= 1) return;
            const double zone = 32767.0 * fraction, theta = std::atan2(double(y), double(x));
            const double zone_y = std::abs(std::sin(theta) * zone), zone_x = std::abs(std::cos(theta) * zone);
            if (y > -zone_y && y < zone_y) y = 0;
            if (x > -zone_x && x < zone_x) x = 0;
        };
        stick(deadzone[0], g.thumb_lx, g.thumb_ly);
        stick(deadzone[1], g.thumb_rx, g.thumb_ry);
    }

    uint32_t capabilities(uint32_t user, uint32_t flags, Capabilities &out) {
        for (Driver *d : filter(flags))
            if (d->capabilities(user, flags, out) == SUCCESS) return SUCCESS;
        return DEVICE_NOT_CONNECTED;
    }

    uint32_t state(uint32_t user, uint32_t flags, uint64_t now_ms, State &out, bool &changed) {
        std::vector<Driver *> list = filter(flags);
        if (list.empty()) {
            not_connected++;
            return DEVICE_NOT_CONNECTED;
        }
        for (Driver *d : list) {
            if (d->state(user, now_ms, out) != SUCCESS) continue;
            changed = update_slot(d, user, true);
            apply_deadzones(out.gamepad);
            if (user < USERS) states[user]++;
            return SUCCESS;
        }
        changed = update_slot(nullptr, user, false);
        not_connected++;
        return DEVICE_NOT_CONNECTED;
    }

    uint32_t vibrate(uint32_t user, const Vibration &v) {
        vibrations++;
        for (auto &d : drivers)
            if (d->vibrate(user, v) == SUCCESS) return SUCCESS;
        return DEVICE_NOT_CONNECTED;
    }

    uint32_t keystroke(uint32_t user, uint32_t flags, uint64_t now_ms, Keystroke &out) {
        bool any_connected = false;
        for (Driver *d : filter(flags)) {
            uint32_t r = d->keystroke(user, flags, now_ms, out);
            if (r == X_ERROR_INVALID_PARAMETER || r == DEVICE_NOT_CONNECTED) continue;
            any_connected = true;
            if (r == SUCCESS) {
                if (user < USERS) keystrokes[user]++;
                return SUCCESS;
            }
        }
        return any_connected ? EMPTY : DEVICE_NOT_CONNECTED;
    }
};

InputSystem &inputs(Kernel &k) { return k.state<InputSystem>(); }

// the guest's structures, big-endian
void write_gamepad(Kernel &k, uint32_t at, const Gamepad &g) {
    k.store16(at + 0, g.buttons);
    k.host(at + 2)[0] = g.left_trigger;
    k.host(at + 3)[0] = g.right_trigger;
    k.store16(at + 4, uint16_t(g.thumb_lx));
    k.store16(at + 6, uint16_t(g.thumb_ly));
    k.store16(at + 8, uint16_t(g.thumb_rx));
    k.store16(at + 10, uint16_t(g.thumb_ry));
}
void write_capabilities(Kernel &k, uint32_t at, const Capabilities &c) {
    k.host(at)[0] = c.type;
    k.host(at + 1)[0] = c.sub_type;
    k.store16(at + 2, c.flags);
    write_gamepad(k, at + 4, c.gamepad);
    k.store16(at + 16, c.vibration.left_motor_speed);
    k.store16(at + 18, c.vibration.right_motor_speed);
}
void write_keystroke(Kernel &k, uint32_t at, const Keystroke &s) {
    k.store16(at, s.virtual_key);
    k.store16(at + 2, s.unicode);
    k.store16(at + 4, s.flags);
    k.host(at + 6)[0] = s.user_index;
    k.host(at + 7)[0] = s.hid_code;
}

// XamInputGetCapabilitiesEx's work: any user is user 0; no flags, any device
uint32_t get_capabilities(Kernel &k, uint32_t user, uint32_t flags, Capabilities &out) {
    if ((user & USER_INDEX_ANY) == USER_INDEX_ANY || (flags & FLAG_ANY_USER)) user = 0;
    if (!flags) flags = FLAG_GAMEPAD | FLAG_KEYBOARD;
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.capabilities(user, flags, out);
}

// XamInputGetKeystrokeEx's work, the user's index read and written back
uint32_t get_keystroke(Kernel &k, uint32_t &user, uint32_t flags, Keystroke &out) {
    InputSystem &s = inputs(k);
    const uint64_t now = k.uptime_ms();
    std::lock_guard<std::mutex> lock(s.mutex);
    if ((user & USER_INDEX_ANY) == USER_INDEX_ANY) user = 0;
    if (flags & FLAG_ANY_USER) {
        // the first user with a key waiting; else, as Xenia answers, not
        // connected
        for (uint32_t i = 0; i < USERS; i++) {
            if (s.keystroke(i, flags, now, out) == SUCCESS) {
                user = out.user_index;
                return SUCCESS;
            }
        }
        return DEVICE_NOT_CONNECTED;
    }
    uint32_t r = s.keystroke(user, flags, now, out);
    if (r == SUCCESS) user = out.user_index;
    return r;
}

} // namespace

void add_input_driver(Kernel &k, std::unique_ptr<input::Driver> driver) {
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.drivers.push_back(std::move(driver));
}

void set_input_deadzones(Kernel &k, double left, double right) {
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.deadzone[0] = left, s.deadzone[1] = right;
}

uint64_t input_states_delivered(Kernel &k) {
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    uint64_t n = 0;
    for (uint64_t v : s.states) n += v;
    return n;
}

std::string input_report(Kernel &k) {
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    char line[320];
    std::snprintf(line, sizeof(line),
                  "input: %zu drivers; states delivered %llu %llu %llu %llu (users 0-3), %llu answered not "
                  "connected; keystrokes %llu %llu %llu %llu; %llu vibrations; %llu connection changes\n",
                  s.drivers.size(), (unsigned long long)s.states[0], (unsigned long long)s.states[1],
                  (unsigned long long)s.states[2], (unsigned long long)s.states[3], (unsigned long long)s.not_connected,
                  (unsigned long long)s.keystrokes[0], (unsigned long long)s.keystrokes[1],
                  (unsigned long long)s.keystrokes[2], (unsigned long long)s.keystrokes[3],
                  (unsigned long long)s.vibrations, (unsigned long long)s.changes);
    return line;
}

// ---- the exports (xam_input.cc) ----

XF_EXPORT(xam, XamEnableInactivityProcessing) { result(c, X_ERROR_SUCCESS); }

// The console's XAM reads the kernel's interrupt time and keeps it as the
// last activity (xfxam); Xenia's stub does nothing. Nothing here times a user
// out, so the stamp only waits for whoever asks.
XF_EXPORT(xam, XamResetInactivity) {
    inputs(k).last_activity = k.load64(k.variable("KeTimeStampBundle"));
}

// (unknown, user_index, flags, X_INPUT_CAPABILITIES_EX *)
XF_EXPORT(xam, XamInputGetCapabilitiesEx) {
    if (arg(k, c, 0) > 1) return result(c, X_ERROR_NOT_SUPPORTED);
    uint32_t caps = arg(k, c, 3);
    if (!caps) return result(c, X_ERROR_BAD_ARGUMENTS);
    std::memset(k.host(caps), 0, 20 + 12);
    Capabilities out;
    uint32_t r = get_capabilities(k, arg(k, c, 1), arg(k, c, 2), out);
    if (r == SUCCESS) write_capabilities(k, caps, out);
    result(c, r);
}

// (user_index, ...): the kernel's HID layer, which the console's XAM asks
// before it answers XamInputGetCapabilities (xfxam: HidGetCapabilities(user,
// ...) on the way, and it goes on when the answer is success). The kernel has
// no device of its own: it answers from the drivers XAM's input uses, so the
// two agree (ours answered "not connected" from XAM's state while the kernel
// said success). A pad that is not there is 0xC000009D, the code games check
// from the HID layer (HidReadKeys' note). Xenia has no HidGetCapabilities.
XF_EXPORT(xboxkrnl, HidGetCapabilities) {
    uint32_t user = arg(k, c, 0);
    Capabilities out;
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    bool there = user < USERS && s.capabilities(user, FLAG_GAMEPAD, out) == SUCCESS;
    result(c, there ? 0 : 0xC000009D);
}

// (user_index, flags, X_INPUT_CAPABILITIES *)
XF_EXPORT(xam, XamInputGetCapabilities) {
    uint32_t caps = arg(k, c, 2);
    if (!caps) return result(c, X_ERROR_BAD_ARGUMENTS);
    std::memset(k.host(caps), 0, 20);
    Capabilities out;
    uint32_t r = get_capabilities(k, arg(k, c, 0), arg(k, c, 1), out);
    if (r == SUCCESS) write_capabilities(k, caps, out);
    result(c, r);
}

// (user_index, flags, X_INPUT_STATE *): no flags, the gamepad. Xenia pins
// "any user" to user 0 and then asks for the index as given; so here.
XF_EXPORT(xam, XamInputGetState) {
    uint32_t user = arg(k, c, 0), flags = arg(k, c, 1), at = arg(k, c, 2);
    if (at) std::memset(k.host(at), 0, 16);
    if (user >= USERS) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    InputSystem &s = inputs(k);
    const uint64_t now = k.uptime_ms();
    State state;
    bool changed = false;
    uint32_t r;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        r = s.state(user, flags ? flags : FLAG_GAMEPAD, now, state, changed);
    }
    if (changed) broadcast(k, XN_SYS_INPUTDEVICESCHANGED, 0);
    if (r == SUCCESS && at) {
        k.store32(at, state.packet_number);
        write_gamepad(k, at + 4, state.gamepad);
    }
    result(c, r);
}

// (user_index, flags, X_INPUT_VIBRATION *)
XF_EXPORT(xam, XamInputSetState) {
    uint32_t user = arg(k, c, 0), at = arg(k, c, 2);
    // The console's XAM answers a user past the slots, and any pad that is not
    // there, with the Win32 ERROR_DEVICE_NOT_CONNECTED whether or not a
    // vibration is given (xfxam); the pointer is asked for after the pad. Xenia
    // answers the HRESULT for a user past the slots and asks for the pointer
    // first.
    if (user >= USERS) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    Vibration v{at ? k.load16(at) : uint16_t(0), at ? k.load16(at + 2) : uint16_t(0)};
    InputSystem &s = inputs(k);
    std::lock_guard<std::mutex> lock(s.mutex);
    uint32_t r = s.vibrate(user, v);
    result(c, r == SUCCESS && !at ? X_ERROR_BAD_ARGUMENTS : r);
}

// (user_index_ptr, flags, X_INPUT_KEYSTROKE *)
XF_EXPORT(xam, XamInputGetKeystrokeEx) {
    uint32_t user_ptr = arg(k, c, 0), flags = arg(k, c, 1), at = arg(k, c, 2);
    if (!at) return result(c, X_ERROR_BAD_ARGUMENTS);
    std::memset(k.host(at), 0, 8);
    uint32_t user = user_ptr ? k.load32(user_ptr) : 0;
    Keystroke out;
    uint32_t r = get_keystroke(k, user, flags, out);
    if (r == SUCCESS) {
        write_keystroke(k, at, out);
        if (user_ptr) k.store32(user_ptr, user);
    }
    result(c, r);
}

// (user_index, flags, X_INPUT_KEYSTROKE *)
XF_EXPORT(xam, XamInputGetKeystroke) {
    uint32_t flags = arg(k, c, 1), at = arg(k, c, 2);
    if (!at) return result(c, X_ERROR_BAD_ARGUMENTS);
    std::memset(k.host(at), 0, 8);
    uint32_t user = arg(k, c, 0);
    Keystroke out;
    uint32_t r = get_keystroke(k, user, flags, out);
    if (r == SUCCESS) write_keystroke(k, at, out);
    result(c, r);
}

// (user_index): nothing is bound to undo, so 0 for any user (the console's XAM
// answers 0 for each of them, xfxam; this answered 1 for the four slots)
XF_EXPORT(xam, XAutomationpUnbindController) { result(c, 0); }

// ---- the non-controller devices (the toy portals, devices 5 and 6) ----
// Xenia reads and writes a portal through its USB passthrough; there is
// none here, so a request that passes Xenia's checks finds no device.

namespace {
constexpr uint32_t PORTAL_BUFFER_SIZE = 0x20; // hid::kPortalBufferSize

// (device_id, buffer, buffer_length_ptr, state_ptr)
uint32_t non_controller_get_raw(Kernel &k, uint32_t device, uint32_t buffer, uint32_t length_ptr, uint32_t state_ptr) {
    if (device != 5 && device != 6) return X_ERROR_INVALID_PARAMETER;
    if (!state_ptr || !length_ptr || !buffer) return X_ERROR_INVALID_PARAMETER;
    uint32_t length = k.load32(length_ptr);
    if (!length || length > PORTAL_BUFFER_SIZE) return X_ERROR_INVALID_PARAMETER;
    return X_ERROR_DEVICE_NOT_CONNECTED;
}

// (device_id, buffer, buffer_length)
uint32_t non_controller_set_raw(uint32_t device, uint32_t buffer, uint32_t length) {
    if (device != 5 && device != 6) return X_ERROR_INVALID_PARAMETER;
    if (!buffer || !length || length > PORTAL_BUFFER_SIZE) return X_ERROR_INVALID_PARAMETER;
    return X_ERROR_DEVICE_NOT_CONNECTED;
}
} // namespace

XF_EXPORT(xam, XamInputNonControllerGetRawEx) {
    result(c, non_controller_get_raw(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3)));
}

// (state_ptr, buffer_length_ptr, buffer): device 5
XF_EXPORT(xam, XamInputNonControllerGetRaw) {
    result(c, non_controller_get_raw(k, 5, arg(k, c, 2), arg(k, c, 1), arg(k, c, 0)));
}

XF_EXPORT(xam, XamInputNonControllerSetRawEx) {
    result(c, non_controller_set_raw(arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)));
}

// (buffer_length, buffer): device 5
XF_EXPORT(xam, XamInputNonControllerSetRaw) { result(c, non_controller_set_raw(5, arg(k, c, 1), arg(k, c, 0))); }

} // namespace xf::kernel
