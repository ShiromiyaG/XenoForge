// The player's settings in config.toml (settings.h), read and written with
// toml++ as the 3DS runtime's frontend/settings.cpp does.

#include "settings.h"

#include "xenoforge/input.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>

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

int clamp_resolution_scale(int percent) {
    // to the nearest step of 25 (halves up), then into 50-800
    int steps = (std::clamp(percent, 0, 100000) + 12) / 25;
    return std::clamp(steps * 25, 50, 800);
}

void clamp_video(Settings &s) {
    auto one_of = [](std::string &value, std::initializer_list<const char *> choices) {
        for (const char *c : choices)
            if (value == c) return;
        value = *choices.begin();
    };
    s.video.resolution_scale = clamp_resolution_scale(s.video.resolution_scale);
    s.video.min_scalable = std::clamp(s.video.min_scalable, 0, 256);
    one_of(s.video.aspect, {"auto", "integer", "stretch"});
    one_of(s.video.upscaler, {"none", "fsr", "cas"});
    one_of(s.video.antialiasing, {"none", "fxaa", "fxaa_extreme", "smaa_medium", "smaa_high", "smaa_ultra"});
    one_of(s.video.resampling,
           {"auto", "bilinear", "area", "bspline", "mitchell", "catmull_rom", "sharp_bilinear"});
    one_of(s.video.texture_filter, {"game", "trilinear", "nearest"});
    one_of(s.video.edram, {"fast", "exact"});
    s.video.fsr_sharpness = std::isfinite(s.video.fsr_sharpness) ? std::clamp(s.video.fsr_sharpness, 0.0, 2.0) : 0.2;
    s.video.cas_sharpness = std::isfinite(s.video.cas_sharpness) ? std::clamp(s.video.cas_sharpness, 0.0, 1.0) : 0.0;
    // the powers of two up to 16, a value between taken down to one
    int a = std::clamp(s.video.anisotropy, 0, 16);
    s.video.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : a;
}

std::vector<VideoChoice> video_choices(const std::string &key) {
    std::vector<VideoChoice> out;
    if (key == "video.resolution_scale") {
        for (int p = 50; p <= 800; p += 25) {
            std::string label = std::to_string(p) + "%";
            if (p == 100) label += " (the console's)";
            if (p % 100 == 0 && p > 100) label += " (" + std::to_string(p / 100) + "x)";
            out.push_back({std::to_string(p), label});
        }
    } else if (key == "video.min_scalable") {
        out = {{"0", "None (everything scaled)"}, {"8", "8 pixels"}, {"16", "16 pixels (default)"}, {"32", "32 pixels"},
               {"64", "64 pixels"}, {"128", "128 pixels"}};
    } else if (key == "video.upscaler") {
        out = {{"none", "None (bilinear or nearest)"}, {"fsr", "AMD FSR 1 (upscale and sharpen)"},
               {"cas", "AMD CAS (sharpen and resample)"}};
    } else if (key == "video.antialiasing") {
        out = {{"none", "None"},
               {"fxaa", "FXAA"},
               {"fxaa_extreme", "FXAA (extreme)"},
               {"smaa_medium", "SMAA (medium)"},
               {"smaa_high", "SMAA (high)"},
               {"smaa_ultra", "SMAA (ultra)"}};
    } else if (key == "video.resampling") {
        out = {{"auto", "Automatic (area when reducing, else bilinear)"},
               {"bilinear", "Bilinear"},
               {"area", "Area (box average)"},
               {"bspline", "Bicubic: B-spline (soft)"},
               {"mitchell", "Bicubic: Mitchell-Netravali"},
               {"catmull_rom", "Bicubic: Catmull-Rom (sharp)"},
               {"sharp_bilinear", "Sharp bilinear"}};
    } else if (key == "video.anisotropy") {
        out = {{"0", "The game's"}, {"1", "Off (1x)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}};
    } else if (key == "video.texture_filter") {
        out = {{"game", "The game's"}, {"trilinear", "Trilinear (smooth between mipmaps)"},
               {"nearest", "Nearest (point sampled)"}};
    } else if (key == "video.edram") {
        out = {{"fast", "Fast (host render targets)"}, {"exact", "Exact (as the console's, more GPU time)"}};
    } else if (key == "video.aspect") {
        out = {{"auto", "The picture's own shape"}, {"integer", "Whole multiples (integer scaling)"},
               {"stretch", "Stretched to the window"}};
    }
    return out;
}

std::string video_value(const Settings &s, const std::string &key) {
    if (key == "video.resolution_scale") return std::to_string(s.video.resolution_scale);
    if (key == "video.min_scalable") return std::to_string(s.video.min_scalable);
    if (key == "video.upscaler") return s.video.upscaler;
    if (key == "video.antialiasing") return s.video.antialiasing;
    if (key == "video.resampling") return s.video.resampling;
    if (key == "video.anisotropy") return std::to_string(s.video.anisotropy);
    if (key == "video.texture_filter") return s.video.texture_filter;
    if (key == "video.edram") return s.video.edram;
    if (key == "video.aspect") return s.video.aspect;
    return {};
}

bool set_video_value(Settings &s, const std::string &key, const std::string &value) {
    bool known = false;
    for (const VideoChoice &c : video_choices(key)) known |= c.value == value;
    if (!known) return false;
    const int number = std::atoi(value.c_str());
    if (key == "video.resolution_scale") s.video.resolution_scale = number;
    else if (key == "video.min_scalable") s.video.min_scalable = number;
    else if (key == "video.upscaler") s.video.upscaler = value;
    else if (key == "video.antialiasing") s.video.antialiasing = value;
    else if (key == "video.resampling") s.video.resampling = value;
    else if (key == "video.anisotropy") s.video.anisotropy = number;
    else if (key == "video.texture_filter") s.video.texture_filter = value;
    else if (key == "video.edram") s.video.edram = value;
    else if (key == "video.aspect") s.video.aspect = value;
    clamp_video(s);
    return true;
}

bool video_needs_restart(const std::string &key) {
    return key == "video.resolution_scale" || key == "video.min_scalable" || key == "video.edram";
}

Settings::Settings() {
    for (const Binding &b : BINDINGS) keys[b.name] = b.keys;
#ifdef XF_DISCORD_CLIENT_ID
    discord.client_id = XF_DISCORD_CLIENT_ID;
#endif
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
    auto video_int = [&](const char *key, int &into) {
        int64_t v = into;
        get(t, "video", key, v);
        into = int(std::clamp<int64_t>(v, -100000, 100000));
    };
    video_int("resolution_scale", s.video.resolution_scale);
    video_int("min_scalable", s.video.min_scalable);
    video_int("anisotropy", s.video.anisotropy);
    get(t, "video", "upscaler", s.video.upscaler);
    get(t, "video", "fsr_sharpness", s.video.fsr_sharpness);
    get(t, "video", "cas_sharpness", s.video.cas_sharpness);
    get(t, "video", "antialiasing", s.video.antialiasing);
    get(t, "video", "resampling", s.video.resampling);
    get(t, "video", "texture_filter", s.video.texture_filter);
    get(t, "video", "edram", s.video.edram);
    get(t, "video", "frame_graph", s.video.frame_graph);
    get(t, "video", "performance", s.video.performance);
    clamp_video(s);
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
    get(t, "saves", "format", s.saves.format);
    get(t, "saves", "root", s.saves.root);
    get(t, "profile", "gamertag", s.profile.gamertag);
    get(t, "profile", "xuid", s.profile.xuid);
    if (auto *names = t["profile"]["names"].as_table())
        for (auto &[key, value] : *names)
            if (auto v = value.value<std::string>()) s.profile.names[std::string(key.str())] = *v;
    auto integer = [&](const char *key, int &into) {
        int64_t v = into;
        get(t, "system", key, v);
        into = int(std::clamp<int64_t>(v, -1, 100000));
    };
    integer("language", s.system.language);
    integer("country", s.system.country);
    integer("width", s.system.width);
    integer("height", s.system.height);
    integer("widescreen", s.system.widescreen);
    integer("refresh", s.system.refresh);
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
    s.patches.clear();
    if (auto *table = t["patches"].as_table())
        for (auto &[key, value] : *table)
            if (auto b = value.value<bool>()) s.patches[std::string(key.str())] = *b;
}

toml::table player_table(const Settings &s) {
    return toml::table{{"screenshots", s.screenshots}, {"follow_launches", s.follow_launches}};
}

void write_sections(toml::table &t, const Settings &s) {
    t.insert_or_assign("saves", toml::table{{"format", s.saves.format}, {"root", s.saves.root}});
    toml::table names;
    for (const auto &[xuid, name] : s.profile.names) names.insert_or_assign(xuid, name);
    t.insert_or_assign("profile", toml::table{{"gamertag", s.profile.gamertag},
                                              {"xuid", s.profile.xuid},
                                              {"names", std::move(names)}});
    toml::table keys;
    for (const Binding &b : BINDINGS) keys.insert(b.name, s.keys.at(b.name));
    t.insert_or_assign("video", toml::table{{"fullscreen", s.video.fullscreen},
                                            {"vsync", s.video.vsync},
                                            {"smooth", s.video.smooth},
                                            {"aspect", s.video.aspect},
                                            {"resolution_scale", s.video.resolution_scale},
                                            {"min_scalable", s.video.min_scalable},
                                            {"upscaler", s.video.upscaler},
                                            {"fsr_sharpness", s.video.fsr_sharpness},
                                            {"cas_sharpness", s.video.cas_sharpness},
                                            {"antialiasing", s.video.antialiasing},
                                            {"resampling", s.video.resampling},
                                            {"anisotropy", s.video.anisotropy},
                                            {"texture_filter", s.video.texture_filter},
                                            {"edram", s.video.edram},
                                            {"frame_graph", s.video.frame_graph},
                                            {"performance", s.video.performance}});
    t.insert_or_assign("audio", toml::table{{"volume", s.audio.volume}, {"mute", s.audio.mute}});
    t.insert_or_assign("input", toml::table{{"keyboard", s.input.keyboard},
                                            {"keyboard_user", s.input.keyboard_user},
                                            {"left_deadzone", s.input.left_deadzone},
                                            {"right_deadzone", s.input.right_deadzone},
                                            {"vibration", s.input.vibration}});
    t.insert_or_assign("keyboard", std::move(keys));
    t.insert_or_assign("system", toml::table{{"language", s.system.language},
                                             {"country", s.system.country},
                                             {"width", s.system.width},
                                             {"height", s.system.height},
                                             {"widescreen", s.system.widescreen},
                                             {"refresh", s.system.refresh}});
    if (!s.port.empty()) {
        toml::table port;
        for (const auto &[key, value] : s.port)
            std::visit([&](auto v) { port.insert_or_assign(key, v); }, value);
        t.insert_or_assign("port", std::move(port));
    }
    // a game's patches turned on or off (patches.h): [games.<id>.patches] "<name>" = true
    if (!s.patches.empty()) {
        toml::table choices;
        for (const auto &[name, on] : s.patches) choices.insert_or_assign(name, on);
        t.insert_or_assign("patches", std::move(choices));
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
    get(t, "launcher", "smooth_edges", l.smooth_edges);
    get(t, "launcher", "background_build", l.background_build);
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
                    {"backdrop", l.backdrop}, {"sounds", l.sounds}, {"smooth_edges", l.smooth_edges},
                    {"background_build", l.background_build}};
    if (l.width > 0) {
        t.insert("x", l.x), t.insert("y", l.y), t.insert("width", l.width), t.insert("height", l.height);
        t.insert("maximized", l.maximized);
    }
    return t;
}

// the Discord section, shared only
void read_discord(const toml::table &t, Settings::Discord &d) {
    get(t, "discord", "enabled", d.enabled);
    get(t, "discord", "client_id", d.client_id);
}

toml::table discord_table(const Settings::Discord &d) {
    return toml::table{{"enabled", d.enabled}, {"client_id", d.client_id}};
}

bool write_file(const std::filesystem::path &path, const toml::table &t, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // written beside and renamed over, so a reader (the launcher and the player share this file) never sees it cut
    std::filesystem::path temporary = path;
    temporary += ".new";
    {
        std::ofstream out(temporary);
        if (!out) return error = "cannot write " + path.string(), false;
        out << "# XenoForge's player (docs/runtime.md, \"The player\"; docs/input.md for the keyboard)\n" << t << "\n";
        if (!out.good()) return error = "cannot write " + path.string(), false;
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return error = "cannot write " + path.string(), false;
    }
    return true;
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
    get(t, "player", "follow_launches", s.follow_launches);
    read_launcher(t, s.launcher);
    read_discord(t, s.discord);
    if (!title.empty())
        if (const toml::table *own = t["games"][title].as_table()) {
            read_sections(*own, s);
            s.game = title;
        }
    s.loaded.reset();
    s.loaded = std::make_shared<const Settings>(s);
    return true;
}

namespace {

bool same_node(const toml::node &a, const toml::node &b) {
    std::ostringstream x, y;
    x << toml::toml_formatter{a};
    y << toml::toml_formatter{b};
    return a.type() == b.type() && x.str() == y.str();
}

// `cur`'s values into `file`, the ones that differ from `base` only (all of them without a base): what this process
// changed since it read the file, over what others wrote since
void merge_changes(toml::table &file, const toml::table &cur, const toml::table *base) {
    for (auto &&[key, value] : cur) {
        const toml::node *before = base ? base->get(key) : nullptr;
        const std::string name(key.str());
        if (const toml::table *sub = value.as_table()) {
            toml::table *into = file.get_as<toml::table>(name);
            if (!into) {
                file.insert_or_assign(name, toml::table{});
                into = file.get_as<toml::table>(name);
            }
            merge_changes(*into, *sub, before ? before->as_table() : nullptr);
        } else if (!before || !same_node(value, *before)) {
            value.visit([&](auto &&leaf) { file.insert_or_assign(name, leaf); });
        }
    }
}

// the tables save() writes for settings: the shared ones with [player] and [launcher], or a game's own sections
toml::table written_tables(const Settings &s) {
    toml::table t;
    write_sections(t, s);
    if (s.game.empty()) {
        t.insert_or_assign("player", player_table(s));
        t.insert_or_assign("launcher", launcher_table(s.launcher));
        t.insert_or_assign("discord", discord_table(s.discord));
    }
    return t;
}

} // namespace

bool save(const std::filesystem::path &path, const Settings &s, std::string &error) {
    toml::table t = read_file(path);
    const toml::table cur = written_tables(s);
    // the baseline: what these settings were when read, for the same scope
    toml::table base_table;
    const toml::table *base = nullptr;
    // (a game's table is compared with what was read even when that was the shared settings: the game's fields
    // that were not changed are then not written)
    if (s.loaded && (s.loaded->game == s.game || !s.game.empty()))
        base_table = written_tables(*s.loaded), base = &base_table;
    if (s.game.empty()) {
        merge_changes(t, cur, base);
    } else {
        // a new file gets the shared defaults beside the game's, so it has
        // the whole set to edit
        if (!t.contains("video")) {
            Settings shared;
            write_sections(t, shared);
            t.insert_or_assign("player", player_table(s));
            t.insert_or_assign("launcher", launcher_table(s.launcher));
            t.insert_or_assign("discord", discord_table(s.discord));
        }
        if (!t["games"].as_table()) t.insert_or_assign("games", toml::table{});
        // a game's table not there yet is written whole
        toml::table *games = t["games"].as_table();
        toml::table *own = games->get_as<toml::table>(s.game);
        if (!own) {
            games->insert_or_assign(s.game, toml::table{});
            own = games->get_as<toml::table>(s.game);
            base = nullptr;
        }
        merge_changes(*own, cur, base);
    }
    if (!write_file(path, t, error)) return false;
    auto now = std::make_shared<Settings>(s);
    now->loaded.reset();
    s.loaded = std::move(now);
    return true;
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
