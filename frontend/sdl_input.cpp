/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The player's input drivers (sdl_input.h). Translated from Xenia (BSD; see
// LICENSE-xenia): hid/sdl/sdl_input_driver.cc (the slots, the axes and
// buttons, the capabilities, the rumble) and hid/keyboard/
// keyboard_input_driver.cc (the bindings, the state, the capabilities), on
// SDL3 where Xenia's is on SDL2; checked against them. Where it differs:
//
// - The keyboard's state is read from SDL's key state each turn of the
//   window's loop, a binding held while its key is down with Shift or Caps
//   Lock as its case asks (Xenia follows key events and can leave a binding
//   held when Shift changes under a held key).
// - The keyboard's keystrokes are the pad's (the bindings' buttons and axes
//   through Xenia's SDL driver's GetKeystroke: downs, ups and repeats);
//   Xenia's keyboard driver, in controller mode, sends a down for each key
//   event of a bound key and nothing on the way up.
// - Guitars and their whammy options are left out (no game here asks).
// - The overlay open takes the keyboard from the game (the gamepads stay).

#include "sdl_input.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace xf::frontend {

using namespace input;

// ---- the gamepads (SDLInputDriver) ----

class PadDriver final : public Driver {
  public:
    explicit PadDriver(Controllers &c) : c_(c) {}
    uint32_t capabilities(uint32_t user, uint32_t, Capabilities &out) override {
        if (user >= USERS) return BAD_ARGUMENTS;
        std::lock_guard<std::mutex> lock(c_.mutex_);
        Controllers::Pad &p = c_.pads_[user];
        if (!p.sdl) return DEVICE_NOT_CONNECTED;
        c_.capabilities(p); // drivers learn some of it late (the battery), so each time, as Xenia
        out = p.caps;
        return SUCCESS;
    }
    uint32_t state(uint32_t user, uint64_t now_ms, State &out) override {
        if (user >= USERS) return BAD_ARGUMENTS;
        std::lock_guard<std::mutex> lock(c_.mutex_);
        Controllers::Pad &p = c_.pads_[user];
        if (!p.sdl) return DEVICE_NOT_CONNECTED;
        Gamepad pad = combine(p.state, c_.overlay(user, now_ms));
        // the packet moves when the state does (a scripted press too)
        if (p.changed || !(pad == last_[user])) p.packet++, p.changed = false;
        last_[user] = pad;
        out.packet_number = p.packet;
        out.gamepad = pad;
        return SUCCESS;
    }
    uint32_t vibrate(uint32_t user, const Vibration &v) override {
        if (user >= USERS) return BAD_ARGUMENTS;
        std::lock_guard<std::mutex> lock(c_.mutex_);
        Controllers::Pad &p = c_.pads_[user];
        if (!p.sdl) return DEVICE_NOT_CONNECTED;
        // vibration off: the motors stopped (InputSystem::ModifyVibrationLevel)
        uint16_t low = c_.vibration_ ? v.left_motor_speed : 0, high = c_.vibration_ ? v.right_motor_speed : 0;
        return SDL_RumbleGamepad(p.sdl, low, high, 0) ? SUCCESS : FUNCTION_FAILED;
    }
    uint32_t keystroke(uint32_t user, uint32_t, uint64_t now_ms, Keystroke &out) override {
        if (user >= USERS) return BAD_ARGUMENTS;
        std::lock_guard<std::mutex> lock(c_.mutex_);
        Controllers::Pad &p = c_.pads_[user];
        if (!p.sdl) return DEVICE_NOT_CONNECTED;
        return p.keys.next(combine(p.state, c_.overlay(user, now_ms)), user, now_ms, out);
    }

  private:
    Controllers &c_;
    Gamepad last_[USERS];
};

// ---- the keyboard (KeyboardInputDriver, mode 1: a controller) ----

class KeyboardDriver final : public Driver {
  public:
    explicit KeyboardDriver(Controllers &c) : c_(c) {}
    Type type() const override { return c_.keyboard_on_ ? Type::Controller : Type::None; }
    uint32_t capabilities(uint32_t user, uint32_t, Capabilities &out) override {
        std::lock_guard<std::mutex> lock(c_.mutex_);
        if (!mine(user)) return DEVICE_NOT_CONNECTED;
        out = Capabilities{};
        out.type = 1, out.sub_type = 1;
        out.gamepad.buttons = 0xFFFF;
        out.gamepad.left_trigger = out.gamepad.right_trigger = 0xFF;
        out.gamepad.thumb_lx = out.gamepad.thumb_ly = out.gamepad.thumb_rx = out.gamepad.thumb_ry = -1;
        return SUCCESS;
    }
    uint32_t state(uint32_t user, uint64_t now_ms, State &out) override {
        std::lock_guard<std::mutex> lock(c_.mutex_);
        if (!mine(user)) return DEVICE_NOT_CONNECTED;
        // a packet each call, as Xenia's keyboard driver counts them
        out.packet_number = ++c_.keyboard_packet_;
        out.gamepad = combine(c_.keyboard_state_, c_.overlay(user, now_ms));
        return SUCCESS;
    }
    uint32_t vibrate(uint32_t user, const Vibration &) override {
        std::lock_guard<std::mutex> lock(c_.mutex_);
        return mine(user) ? SUCCESS : DEVICE_NOT_CONNECTED;
    }
    uint32_t keystroke(uint32_t user, uint32_t, uint64_t now_ms, Keystroke &out) override {
        std::lock_guard<std::mutex> lock(c_.mutex_);
        if (!mine(user)) return DEVICE_NOT_CONNECTED;
        return c_.keyboard_keys_.next(combine(c_.keyboard_state_, c_.overlay(user, now_ms)), user, now_ms, out);
    }

  private:
    bool mine(uint32_t user) const { return c_.keyboard_on_ && user == c_.keyboard_user_; }
    Controllers &c_;
};

Controllers::Controllers(const Settings &settings, Overlay presses) : presses_(std::move(presses)) { apply(settings); }

Controllers::~Controllers() {
    for (Pad &p : pads_)
        if (p.sdl) SDL_CloseGamepad(p.sdl);
}

std::unique_ptr<Driver> Controllers::gamepads() { return std::make_unique<PadDriver>(*this); }
std::unique_ptr<Driver> Controllers::keyboard() { return std::make_unique<KeyboardDriver>(*this); }

void Controllers::apply(const Settings &s) {
    std::vector<Key> keys;
    for (size_t b = 0; b < std::size(BINDINGS); b++) {
        auto it = s.keys.find(BINDINGS[b].name);
        if (it == s.keys.end()) continue;
        // space-separated names, each with its case: ^ Shift, _ no Shift
        const std::string &list = it->second;
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find(' ', at);
            std::string token = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? list.size() : end + 1;
            if (token.empty()) continue;
            int letter_case = 0;
            if (token.size() > 1 && token[0] == '^')
                letter_case = 1, token.erase(0, 1);
            else if (token.size() > 1 && token[0] == '_')
                letter_case = -1, token.erase(0, 1);
            SDL_Scancode code = SDL_GetScancodeFromName(token.c_str());
            if (code == SDL_SCANCODE_UNKNOWN) {
                std::fprintf(stderr, "keyboard: no key \"%s\" for %s\n", token.c_str(), BINDINGS[b].name);
                continue;
            }
            keys.push_back({code, letter_case, b});
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    keys_ = std::move(keys);
    keyboard_on_ = s.input.keyboard;
    keyboard_user_ = uint32_t(s.input.keyboard_user);
    vibration_ = s.input.vibration;
}

void Controllers::read_keyboard(bool focused, bool taken) {
    const bool *down = SDL_GetKeyboardState(nullptr);
    const SDL_Keymod mods = SDL_GetModState();
    const bool upper = (mods & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS)) != 0;
    Gamepad g;
    if (focused && !taken) {
        // which bindings are held, then what they press (Xenia's GetState:
        // the opposite directions of a stick add up)
        bool held[std::size(BINDINGS)] = {};
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Key &k : keys_) {
            if (!down[k.code]) continue;
            if ((k.letter_case > 0 && !upper) || (k.letter_case < 0 && upper)) continue;
            held[k.binding] = true;
        }
        int lx = 0, ly = 0, rx = 0, ry = 0;
        for (size_t b = 0; b < std::size(BINDINGS); b++) {
            if (!held[b]) continue;
            g.buttons |= BINDINGS[b].button;
            switch (BINDINGS[b].axis) {
            case 1: g.left_trigger = 0xFF; break;
            case 2: g.right_trigger = 0xFF; break;
            case 3: lx += SHRT_MIN; break;
            case 4: lx += SHRT_MAX; break;
            case 5: ly += SHRT_MIN; break;
            case 6: ly += SHRT_MAX; break;
            case 7: rx += SHRT_MIN; break;
            case 8: rx += SHRT_MAX; break;
            case 9: ry += SHRT_MIN; break;
            case 10: ry += SHRT_MAX; break;
            default: break;
            }
        }
        g.thumb_lx = int16_t(lx), g.thumb_ly = int16_t(ly), g.thumb_rx = int16_t(rx), g.thumb_ry = int16_t(ry);
        keyboard_state_ = g;
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    keyboard_state_ = g;
}

int Controllers::slot_of(SDL_JoystickID id) const {
    for (size_t i = 0; i < pads_.size(); i++)
        if (pads_[i].sdl && SDL_GetGamepadID(pads_[i].sdl) == id) return int(i);
    return -1;
}

// OnControllerDeviceAdded: the slot of its player index if free, else the
// first free one (and the index set to it); none free, it is closed
void Controllers::added(SDL_JoystickID id) {
    if (slot_of(id) >= 0) return;
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) return;
    int user = SDL_GetGamepadPlayerIndex(pad);
    if (user < 0 || user >= int(USERS) || pads_[user].sdl) {
        user = -1;
        for (size_t i = 0; i < pads_.size(); i++)
            if (!pads_[i].sdl) {
                user = int(i);
                SDL_SetGamepadPlayerIndex(pad, user);
                break;
            }
    }
    if (user < 0) {
        SDL_CloseGamepad(pad);
        std::fprintf(stderr, "input: %s ignored, every slot taken\n", SDL_GetGamepadName(pad));
        return;
    }
    Pad &p = pads_[user];
    p = Pad{};
    p.sdl = pad;
    const char *name = SDL_GetGamepadName(pad);
    p.name = name ? name : "gamepad";
    p.changed = true; // XInput starts at packet 1
    capabilities(p);
    std::fprintf(stderr, "input: %s in slot %d\n", p.name.c_str(), user);
}

// UpdateXCapabilities
void Controllers::capabilities(Pad &p) {
    Capabilities c = gamepad_capabilities();
    if (SDL_GetGamepadConnectionState(p.sdl) == SDL_JOYSTICK_CONNECTION_WIRELESS) c.flags |= 0x0002; // WIRELESS
    for (SDL_GamepadButton b :
         {SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
          SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT})
        if (!SDL_GamepadHasButton(p.sdl, b)) {
            c.flags |= 0x0010; // NO_NAVIGATION
            break;
        }
    // SDL's joystick types are the XInput subtypes up to the guitar
    switch (SDL_GetJoystickType(SDL_GetGamepadJoystick(p.sdl))) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: c.sub_type = 0x01; break;
    case SDL_JOYSTICK_TYPE_WHEEL: c.sub_type = 0x02; break;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: c.sub_type = 0x03; break;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: c.sub_type = 0x04; break;
    case SDL_JOYSTICK_TYPE_DANCE_PAD: c.sub_type = 0x05; break;
    case SDL_JOYSTICK_TYPE_GUITAR: c.sub_type = 0x06; break;
    case SDL_JOYSTICK_TYPE_DRUM_KIT: c.sub_type = 0x08; break;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: c.sub_type = 0x13; break;
    default: c.sub_type = 0x00; break; // XINPUT_DEVSUBTYPE_UNKNOWN, as Xenia
    }
    p.caps = c;
}

void Controllers::event(const SDL_Event &e) {
    // SDL3's gamepad buttons in their order (SDL2's, which Xenia's table
    // follows): the paddles as Y, B, X, A; misc and the touchpad as guide
    static constexpr uint16_t BUTTONS[] = {
        A,
        B,
        X,
        Y,
        BACK,
        GUIDE,
        START,
        LEFT_THUMB,
        RIGHT_THUMB,
        LEFT_SHOULDER,
        RIGHT_SHOULDER,
        DPAD_UP,
        DPAD_DOWN,
        DPAD_LEFT,
        DPAD_RIGHT,
        GUIDE,
        Y,
        B,
        X,
        A,
        GUIDE,
    };
    std::lock_guard<std::mutex> lock(mutex_);
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: added(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int user = slot_of(e.gdevice.which);
        if (user < 0) break;
        SDL_CloseGamepad(pads_[user].sdl);
        pads_[user] = Pad{};
        std::fprintf(stderr, "input: the gamepad in slot %d removed\n", user);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int user = slot_of(e.gaxis.which);
        if (user < 0) break;
        Gamepad &g = pads_[user].state;
        const int16_t v = e.gaxis.value;
        switch (e.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: g.thumb_lx = v; break;
        case SDL_GAMEPAD_AXIS_LEFTY: g.thumb_ly = int16_t(~v); break; // SDL's down is positive
        case SDL_GAMEPAD_AXIS_RIGHTX: g.thumb_rx = v; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: g.thumb_ry = int16_t(~v); break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: g.left_trigger = uint8_t(std::max<int>(v, 0) >> 7); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: g.right_trigger = uint8_t(std::max<int>(v, 0) >> 7); break;
        default: break;
        }
        pads_[user].changed = true;
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int user = slot_of(e.gbutton.which);
        if (user < 0 || e.gbutton.button >= std::size(BUTTONS)) break;
        uint16_t bit = BUTTONS[e.gbutton.button];
        Gamepad &g = pads_[user].state;
        if (e.gbutton.down)
            g.buttons |= bit;
        else
            g.buttons &= ~bit;
        pads_[user].changed = true;
        break;
    }
    default: break;
    }
}

std::array<Controllers::View, USERS> Controllers::view(uint64_t now_ms) {
    std::array<View, USERS> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t u = 0; u < USERS; u++) {
        if (pads_[u].sdl) {
            out[u].source = pads_[u].name;
            out[u].pad = combine(pads_[u].state, overlay(u, now_ms));
        } else if (keyboard_on_ && u == keyboard_user_) {
            out[u].source = "keyboard";
            out[u].pad = combine(keyboard_state_, overlay(u, now_ms));
        }
    }
    return out;
}

} // namespace xf::frontend
