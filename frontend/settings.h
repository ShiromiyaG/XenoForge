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
#include <memory>
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
        // "auto" (the front buffer's own shape, letterboxed), "integer" (the largest whole multiple of it that fits,
        // centered) or "stretch"
        std::string aspect = "auto";
        // The picture options beyond the console (docs/renderer.md, "Resolution scale"; docs/runtime.md, "Picture
        // options"). They never change what the game computes: what the CPU reads back stays at the console's size.
        // The internal resolution in percent of the console's, 50-800 in steps of 25 (RPCS3's resolution scale);
        // taken when the title starts.
        int resolution_scale = 100;
        // render targets, resolves and textures narrower or shorter than this many console pixels stay at the
        // console's size (RPCS3's minimum scalable dimension); taken when the title starts
        int min_scalable = 16;
        // The window's output scaling, apart from the internal resolution (RPCS3's "output scaling"): "none" (a
        // blit, linear or nearest by `smooth`), "fsr" (AMD FSR 1: EASU, then RCAS) or "cas" (AMD CAS, sharpening
        // and resampling), as Xenia's presenter offers them; live.
        std::string upscaler = "none";
        double fsr_sharpness = 0.2; // RCAS's sharpness reduction in stops, 0-2 (Xenia's default)
        double cas_sharpness = 0.0; // CAS's additional sharpness, 0-1 (Xenia's default)
        // "none", "fxaa", "fxaa_extreme" (Xenia's two FXAA presets) or "smaa_medium", "smaa_high", "smaa_ultra" (SMAA
        // 1x at its presets), on the picture before scaling; live
        std::string antialiasing = "none";
        // The output resampling where neither FSR nor CAS is asked for and `smooth` is on (Dolphin's choices):
        // "auto" (area where the picture is larger than the window's rectangle along either axis, else the bilinear
        // blit), "bilinear", "area", "bspline", "mitchell", "catmull_rom" or "sharp_bilinear"; live
        std::string resampling = "auto";
        // Texture filtering, live: anisotropic filtering forced (0 the game's, else 1, 2, 4, 8 or 16) on mipmapped
        // textures the game filters linearly (Xenia's anisotropic_override rule), and "game", "trilinear" (linear
        // between mips where the game filters each one linearly) or "nearest" (everything point sampled)
        int anisotropy = 0;
        std::string texture_filter = "game";
        // How the console's render target memory (EDRAM) is drawn into, as the game starts: "fast" (host render
        // targets, Xenia's default: less GPU time a frame) or "exact" (the pixel shaders read and write EDRAM
        // themselves: blending, float formats, depth and stencil as the console's, where the device has fragment
        // shader interlock; host render targets where it has not)
        std::string edram = "fast";
        bool frame_graph = false; // the frame-time graph in the F1 overlay
        // the performance display over the picture (F3 switches it): the game's frames a second, the frame
        // times with their graph, the command processor's work a frame and the draws
        bool performance = false;
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
    // the console's own settings the games read, in config.toml's [system]: the
    // user's language (XLanguage: 1 English, 2 Japanese, 3 German, 4 French,
    // 5 Spanish, 6 Italian, ...) and country code, the screen size, widescreen
    // and the refresh rate (50 or 60); 0, -1 and "" leave the console's defaults
    struct System {
        int language = 0;
        int country = -1;
        int width = 0, height = 0;
        int widescreen = -1;
        int refresh = 0;
    } system;
    // how new saves are kept, in [saves]: "stfs" (the console's own packages, the default) or "directory" (Xenia's
    // layout: a folder of files)
    struct Saves {
        std::string format = "stfs";
        std::string root; // the storage root (content/, cache*/): "" for storage/ beside config.toml
    } saves;
    // the profiles, in [profile]: the one the games use (its XUID as 16 hex digits, "" for the first the storage has)
    // and its gamertag ("" for the default), and every profile's gamertag by XUID in [profile.names]
    struct Profile {
        std::string gamertag, xuid;
        std::map<std::string, std::string> names;
    } profile;
    // [player]: when a title launches another (XamLoaderLaunchTitle), the player restarts on the executable it
    // launched, from the same disc, as boot's --follow-launches does
    bool follow_launches = true;
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
        std::string sort = "title";  // the column the table is sorted by, "-" before it for descending
        std::string backdrop = "animated"; // the window's ribbons: "animated", "still" or "off"
        bool sounds = true;          // the window's own sounds (ui_sound.h): a tick on moving, a swish on changing page
        bool smooth_edges = true;    // 4x multisampling for the window's drawing (ui::Renderer::set_msaa)
        // the main window where the user left it (pixels; width 0: never saved)
        int x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
    } launcher;
    // the title ID ("5846000A") whose own settings these are, or "" for the
    // shared ones: where save() writes them
    std::string game;
    // The settings as load() read them (or save() last wrote them): save() writes into the file only the fields
    // that differ from this, over the file as it is then, so what another process (the launcher, a player) saved
    // meanwhile survives. Null for settings never loaded: all of them are written.
    mutable std::shared_ptr<const Settings> loaded;

    Settings(); // Xenia's default bindings
};

// A resolution scale in percent made one the renderer draws at: the nearest step of 25 within 50-800.
int clamp_resolution_scale(int percent);
// The video section's values made valid (choices not known go back to their defaults, numbers into their ranges).
void clamp_video(Settings &settings);

// The video section's choices, shared by the launcher's Video tab and the player's overlay and guide: for a key
// ("video.resolution_scale", "video.min_scalable", "video.upscaler", "video.antialiasing", "video.resampling",
// "video.anisotropy",
// "video.texture_filter", "video.aspect") its values (as text) and their labels, the current value as text, and a
// value set (false for a key or value not known). Empty for other keys.
struct VideoChoice {
    std::string value, label;
};
std::vector<VideoChoice> video_choices(const std::string &key);
std::string video_value(const Settings &settings, const std::string &key);
bool set_video_value(Settings &settings, const std::string &key, const std::string &value);
// the video keys a running title takes only when it starts again
bool video_needs_restart(const std::string &key);

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
