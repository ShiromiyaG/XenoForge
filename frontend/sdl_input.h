// The player's input drivers (xenoforge/input.h; docs/input.md): SDL3's
// gamepads, each in a user's slot as Xenia's SDL driver puts them, and the
// keyboard as a controller for one user with Xenia's bindings (its keyboard
// driver in mode 1). The window's thread feeds them (events, the keyboard
// read each turn of its loop); the guest's threads ask them through XAM.

#pragma once

#include "settings.h"

#include "xenoforge/input.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xf::frontend {

class Controllers {
  public:
    Controllers(const Settings &settings, input::Overlay presses);
    ~Controllers();

    // the window's thread: SDL's gamepad events; the keyboard read (focused:
    // the window has it; taken: the overlay takes it, so the game gets none)
    void event(const SDL_Event &event);
    void read_keyboard(bool focused, bool taken);
    // the settings changed (the bindings, the keyboard's user, vibration)
    void apply(const Settings &settings);

    // the two drivers, in Xenia's order (the gamepads, then the keyboard),
    // for kernel::add_input_driver; they refer to this, which outlives them
    std::unique_ptr<input::Driver> gamepads();
    std::unique_ptr<input::Driver> keyboard();

    // for the overlay: each user's controller, what drives it and its state
    struct View {
        std::string source; // "" when nobody answers for the user
        input::Gamepad pad;
    };
    std::array<View, input::USERS> view(uint64_t now_ms);

  private:
    friend class PadDriver;
    friend class KeyboardDriver;
    struct Pad {
        SDL_Gamepad *sdl = nullptr;
        std::string name;
        input::Gamepad state;
        bool changed = false;
        uint32_t packet = 0;
        input::Capabilities caps;
        input::PadKeystrokes keys;
    };
    struct Key {
        SDL_Scancode code;
        int letter_case; // 0 any, 1 with Shift or Caps Lock (^), -1 without (_)
        size_t binding;  // into BINDINGS
    };
    int slot_of(SDL_JoystickID id) const;
    void added(SDL_JoystickID id);
    void capabilities(Pad &pad);
    input::Gamepad overlay(uint32_t user, uint64_t now_ms) const {
        return presses_ ? presses_(user, now_ms) : input::Gamepad{};
    }

    mutable std::mutex mutex_;
    std::array<Pad, input::USERS> pads_;
    input::Overlay presses_;
    // the keyboard: its bindings, its state as the last read left it
    std::vector<Key> keys_;
    std::atomic<bool> keyboard_on_{true}, vibration_{true};
    std::atomic<uint32_t> keyboard_user_{0};
    input::Gamepad keyboard_state_;
    uint32_t keyboard_packet_ = 0;
    input::PadKeystrokes keyboard_keys_;
};

} // namespace xf::frontend
