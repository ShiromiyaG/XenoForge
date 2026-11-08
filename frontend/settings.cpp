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

// the sections a game may have of its own, read from the file's root or from
// a game's table
void read_sections(const toml::table &t, Settings &s) {
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
    if (auto *table = t["port"].as_table())
        for (auto &[key, value] : *table) {
            if (auto b = value.value<bool>())
                s.port[std::string(key.str())] = *b;
            else if (auto i = value.value<int64_t>())
                s.port[std::string(key.str())] = *i;
            else if (auto d = value.value<double>())
                s.port[std::string(key.str())] = *d;
        }
}

void write_sections(toml::table &t, const Settings &s) {
    toml::table keys;
    for (const Binding &b : BINDINGS) keys.insert(b.name, s.keys.at(b.name));
    t.insert_or_assign("video", toml::table{{"fullscreen", s.video.fullscreen},
                                            {"vsync", s.video.vsync},
                                            {"smooth", s.video.smooth},
                                            {"aspect", s.video.aspect}});
    t.insert_or_assign("audio", toml::table{{"volume", s.audio.volume}, {"mute", s.audio.mute}});
    t.insert_or_assign("input", toml::table{{"keyboard", s.input.keyboard},
                                            {"keyboard_user", s.input.keyboard_user},
                                            {"left_deadzone", s.input.left_deadzone},
                                            {"right_deadzone", s.input.right_deadzone},
                                            {"vibration", s.input.vibration}});
    t.insert_or_assign("keyboard", std::move(keys));
    if (!s.port.empty()) {
        toml::table port;
        for (const auto &[key, value] : s.port)
            std::visit([&](auto v) { port.insert_or_assign(key, v); }, value);
        t.insert_or_assign("port", std::move(port));
    }
}

// the file as it is, or an empty table when it is missing or does not parse
// (then it is written anew)
toml::table read_file(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    try {
        return toml::parse_file(path.u8string());
    } catch (const toml::parse_error &) {
        return {};
    }
}

// the launcher's section, shared only
void read_launcher(const toml::table &t, Settings::Launcher &l) {
    get(t, "launcher", "games", l.games);
    get(t, "launcher", "accent", l.accent);
    get(t, "launcher", "theme", l.theme);
    get(t, "launcher", "sort", l.sort);
    get(t, "launcher", "backdrop", l.backdrop);
    get(t, "launcher", "sounds", l.sounds);
    int64_t x = l.x, y = l.y, width = l.width, height = l.height;
    get(t, "launcher", "x", x);
    get(t, "launcher", "y", y);
    get(t, "launcher", "width", width);
    get(t, "launcher", "height", height);
    l.x = int(std::clamp<int64_t>(x, -100000, 100000)), l.y = int(std::clamp<int64_t>(y, -100000, 100000));
    l.width = int(std::clamp<int64_t>(width, 0, 100000)), l.height = int(std::clamp<int64_t>(height, 0, 100000));
    get(t, "launcher", "maximized", l.maximized);
}

toml::table launcher_table(const Settings::Launcher &l) {
    toml::table t{{"games", l.games}, {"accent", l.accent}, {"theme", l.theme}, {"sort", l.sort},
                    {"backdrop", l.backdrop}, {"sounds", l.sounds}};
    if (l.width > 0) {
        t.insert("x", l.x), t.insert("y", l.y), t.insert("width", l.width), t.insert("height", l.height);
        t.insert("maximized", l.maximized);
    }
    return t;
}

bool write_file(const std::filesystem::path &path, const toml::table &t, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return error = "cannot write " + path.string(), false;
    out << "# XenoForge's player (docs/runtime.md, \"The player\"; docs/input.md for the keyboard)\n" << t << "\n";
    return out.good() || (error = "cannot write " + path.string(), false);
}

} // namespace

bool load(const std::filesystem::path &path, Settings &s, std::string &error, const std::string &title) {
    s.game.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    toml::table t;
    try {
        t = toml::parse_file(path.u8string());
    } catch (const toml::parse_error &e) {
        error = std::string(e.description());
        return false;
    }
    read_sections(t, s);
    get(t, "player", "screenshots", s.screenshots);
    read_launcher(t, s.launcher);
    if (!title.empty())
        if (const toml::table *own = t["games"][title].as_table()) {
            read_sections(*own, s);
            s.game = title;
        }
    return true;
}

bool save(const std::filesystem::path &path, const Settings &s, std::string &error) {
    toml::table t = read_file(path);
    if (s.game.empty()) {
        write_sections(t, s);
        t.insert_or_assign("player", toml::table{{"screenshots", s.screenshots}});
        t.insert_or_assign("launcher", launcher_table(s.launcher));
    } else {
        // a new file gets the shared defaults beside the game's, so it has
        // the whole set to edit
        if (!t.contains("video")) {
            Settings shared;
            write_sections(t, shared);
            t.insert_or_assign("player", toml::table{{"screenshots", s.screenshots}});
            t.insert_or_assign("launcher", launcher_table(s.launcher));
        }
        if (!t["games"].as_table()) t.insert_or_assign("games", toml::table{});
        toml::table own;
        write_sections(own, s);
        t["games"].as_table()->insert_or_assign(s.game, std::move(own));
    }
    return write_file(path, t, error);
}

bool forget_game(const std::filesystem::path &path, const std::string &title, std::string &error) {
    toml::table t = read_file(path);
    toml::table *games = t["games"].as_table();
    if (!games || !games->contains(title)) return true;
    games->erase(title);
    if (games->empty()) t.erase("games");
    return write_file(path, t, error);
}

} // namespace xf::frontend
