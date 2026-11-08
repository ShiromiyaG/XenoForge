// The player's settings, kept in config.toml (after the 3DS runtime's
// frontend/settings.h): the window, the sound, the controllers' deadzones and
// the keyboard's bindings. The overlay changes what can change live and the
// file is saved when it closes; a missing file is written with the defaults,
// so there is one to edit.
//
// A game may have settings of its own: a [games."<title ID>"] table in the
// same file holding the video, audio, input and keyboard sections, read over
// the shared ones for that game. The launcher makes and edits them; the
// player reads them for the game it runs and saves its overlay's changes
// back where they came from. The launcher's own section (the games folder, its
// look, its window and table) is shared only.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
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
    // the port's options (run.h's PortOption) by key, in config.toml's [port]
    std::map<std::string, std::variant<bool, int64_t, double>> port;
    // each binding's keys, space-separated (BINDINGS' names)
    std::map<std::string, std::string> keys;
    // where F12 and the overlay's button put screenshots ("" for the
    // screenshots folder beside the config file)
    std::string screenshots;
    // the launcher's own: shared only, never a game's
    struct Launcher {
        std::string games;           // the folder the launcher lists the games of
        std::string accent;          // the launcher's accent colour, "#RRGGBB" ("" for its own)
        std::string theme = "system"; // "system" (the desktop's light or dark), "light" or "dark"
        std::string columns;         // the games table's column widths, "title=320 status=140" (dp)
        std::string sort = "title";  // the column the table is sorted by, "-" before it for descending
        // the main window where the user left it (pixels; width 0: never saved)
        int x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
    } launcher;
    // the title ID ("4D5307E6") whose own settings these are, or "" for the
    // shared ones: where save() writes them
    std::string game;

    Settings(); // Xenia's default bindings
};

// SDL's preference folder's xenoforge\config.toml
std::filesystem::path default_path();
// A missing file gives the defaults; false and why for one that does not
// parse. With a title ID, that game's own settings over the shared ones
// (settings.game is the title ID when the game has a table, else "").
bool load(const std::filesystem::path &path, Settings &settings, std::string &error, const std::string &title = {});
// Written as the shared settings, or as settings.game's own when it is set;
// the rest of the file (the other games' tables, the shared settings when a
// game's are written) is kept.
bool save(const std::filesystem::path &path, const Settings &settings, std::string &error);
// A game's own settings dropped: it goes back to the shared ones.
bool forget_game(const std::filesystem::path &path, const std::string &title, std::string &error);

} // namespace xf::frontend
