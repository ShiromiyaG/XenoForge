// The player's settings, kept in config.toml (after the 3DS runtime's
// frontend/settings.h): the window, the sound, the controllers' deadzones and
// the keyboard's bindings. The overlay changes what can change live and the
// file is saved when it closes; a missing file is written with the defaults,
// so there is one to edit.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace xf::frontend {

// The pad's controls the keyboard can press, in Xenia's binding table's
// order (hid/keyboard/keyboard_binding_table.inc), by its cvars' names
// without keybind_.
struct Binding {
    const char *name;
    uint16_t button;  // X_INPUT_GAMEPAD's bit, or 0 for an axis
    int axis;         // 0: none; 1 left trigger, 2 right trigger; 3-6 left stick left, right, down, up; 7-10 right
    const char *keys; // Xenia's default, as SDL key names: ^ with Shift (or Caps Lock), _ without
};
extern const Binding BINDINGS[25];

struct Settings {
    struct {
        bool fullscreen = false;
        bool vsync = true;           // the window waits for the display (the guest never does)
        bool smooth = true;          // scaled with linear filtering, else nearest
        std::string aspect = "auto"; // "auto" (the front buffer's own shape, letterboxed) or "stretch"
    } video;
    struct {
        int volume = 100; // percent
        bool mute = false;
    } audio;
    struct {
        // the keyboard as a controller (Xenia's keyboard_mode 1) for one
        // user (keyboard_user_index), asked after the gamepads as Xenia asks
        // its drivers; Xenia's default is off, the player's on, so a game
        // can be played without a pad
        bool keyboard = true;
        int keyboard_user = 0;
        double left_deadzone = 0, right_deadzone = 0; // 0-1 of the stick's range (Xenia's *_deadzone_percentage)
        bool vibration = true;
    } input;
    // each binding's keys, space-separated (BINDINGS' names)
    std::map<std::string, std::string> keys;
    // where F12 and the overlay's button put screenshots ("" for the
    // screenshots folder beside the config file)
    std::string screenshots;

    Settings(); // Xenia's default bindings
};

// SDL's preference folder's xenoforge\config.toml
std::filesystem::path default_path();
// a missing file gives the defaults; false and why for one that does not parse
bool load(const std::filesystem::path &path, Settings &settings, std::string &error);
bool save(const std::filesystem::path &path, const Settings &settings, std::string &error);

} // namespace xf::frontend
