// The player's settings in config.toml (settings.h), read and written with
// toml++ as the 3DS runtime's frontend/settings.cpp does.

#include "settings.h"

#include "xenoforge/input.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>

namespace xf::frontend {

// Xenia's keyboard_binding_table.inc, its Windows virtual keys named as SDL
// names them: 0x08 Backspace, 0x25-0x28 the arrows, 0xBA ";", 0xDE "'"
const Binding BINDINGS[25] = {
    {"guide", input::GUIDE, 0, "Backspace"},
    {"dpad_left", input::DPAD_LEFT, 0, "^A"},
    {"dpad_right", input::DPAD_RIGHT, 0, "^D"},
    {"dpad_down", input::DPAD_DOWN, 0, "^S"},
    {"dpad_up", input::DPAD_UP, 0, "^W"},
    {"left_thumb_left", 0, 3, "_A"},
    {"left_thumb_right", 0, 4, "_D"},
    {"left_thumb_down", 0, 5, "_S"},
    {"left_thumb_up", 0, 6, "_W"},
    {"left_thumb", input::LEFT_THUMB, 0, "F"},
    {"right_thumb_up", 0, 10, "Up"},
    {"right_thumb_down", 0, 9, "Down"},
    {"right_thumb_right", 0, 8, "Right"},
    {"right_thumb_left", 0, 7, "Left"},
    {"right_thumb", input::RIGHT_THUMB, 0, "K"},
    {"x", input::X, 0, "L"},
    {"b", input::B, 0, "'"},
    {"a", input::A, 0, ";"},
    {"y", input::Y, 0, "P"},
    {"left_trigger", 0, 1, "Q I"},
    {"right_trigger", 0, 2, "E O"},
    {"back", input::BACK, 0, "Z"},
    {"start", input::START, 0, "X"},
    {"left_shoulder", input::LEFT_SHOULDER, 0, "1"},
    {"right_shoulder", input::RIGHT_SHOULDER, 0, "3"},
};

Settings::Settings() {
    for (const Binding &b : BINDINGS) keys[b.name] = b.keys;
}

std::filesystem::path default_path() {
    std::filesystem::path dir;
    if (char *pref = SDL_GetPrefPath(nullptr, "xenoforge")) {
        dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(pref)));
        SDL_free(pref);
    }
    return dir / "config.toml";
}

namespace {
template <typename T> void get(const toml::table &t, const char *section, const char *key, T &into) {
    if (auto v = t[section][key].value<T>()) into = *v;
}
} // namespace

bool load(const std::filesystem::path &path, Settings &s, std::string &error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    toml::table t;
    try {
        t = toml::parse_file(path.u8string());
    } catch (const toml::parse_error &e) {
        error = std::string(e.description());
        return false;
    }
    get(t, "video", "fullscreen", s.video.fullscreen);
    get(t, "video", "vsync", s.video.vsync);
    get(t, "video", "smooth", s.video.smooth);
    get(t, "video", "aspect", s.video.aspect);
    int64_t volume = s.audio.volume;
    get(t, "audio", "volume", volume);
    s.audio.volume = int(std::clamp<int64_t>(volume, 0, 100));
    get(t, "audio", "mute", s.audio.mute);
    get(t, "input", "keyboard", s.input.keyboard);
    int64_t user = s.input.keyboard_user;
    get(t, "input", "keyboard_user", user);
    s.input.keyboard_user = int(std::clamp<int64_t>(user, 0, 3));
    get(t, "input", "left_deadzone", s.input.left_deadzone);
    get(t, "input", "right_deadzone", s.input.right_deadzone);
    s.input.left_deadzone = std::clamp(s.input.left_deadzone, 0.0, 1.0);
    s.input.right_deadzone = std::clamp(s.input.right_deadzone, 0.0, 1.0);
    get(t, "input", "vibration", s.input.vibration);
    if (auto *table = t["keyboard"].as_table())
        for (auto &[key, value] : *table)
            if (auto v = value.value<std::string>(); v && s.keys.count(std::string(key.str())))
                s.keys[std::string(key.str())] = *v;
    get(t, "player", "screenshots", s.screenshots);
    return true;
}

bool save(const std::filesystem::path &path, const Settings &s, std::string &error) {
    toml::table keys;
    for (const Binding &b : BINDINGS) keys.insert(b.name, s.keys.at(b.name));
    toml::table t{
        {"video", toml::table{{"fullscreen", s.video.fullscreen},
                              {"vsync", s.video.vsync},
                              {"smooth", s.video.smooth},
                              {"aspect", s.video.aspect}}},
        {"audio", toml::table{{"volume", s.audio.volume}, {"mute", s.audio.mute}}},
        {"input", toml::table{{"keyboard", s.input.keyboard},
                              {"keyboard_user", s.input.keyboard_user},
                              {"left_deadzone", s.input.left_deadzone},
                              {"right_deadzone", s.input.right_deadzone},
                              {"vibration", s.input.vibration}}},
        {"keyboard", keys},
        {"player", toml::table{{"screenshots", s.screenshots}}},
    };
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return error = "cannot write " + path.string(), false;
    out << "# XenoForge's player (docs/runtime.md, \"The player\"; docs/input.md for the keyboard)\n" << t << "\n";
    return true;
}

} // namespace xf::frontend
