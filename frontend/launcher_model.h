// What the launcher's screens say, apart from how they are drawn: a game's
// status, durations, the accent colour's shades, the bindings' names and
// keys, and the spatial navigation that moves focus with a pad's d-pad. Plain
// functions of plain values (no RmlUi, no SDL), so frontend_test checks them;
// launcher_view.cpp reads them and builds the document (launcher_html.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xf::frontend {

// What a game's Play does: ready (a library for it is cached, or a player
// carries its generated code), needs preparing (the recompile step makes the
// library, once), or interpreted (preparing failed here: it runs in the
// generic player, slowly).
enum class GameStatus { Ready, NeedsPreparing, Interpreted };
GameStatus game_status(bool cached_library, bool generated_player, bool prepare_failed);
const char *status_class(GameStatus status);  // the style class: ready, prep, interp
const char *status_text(GameStatus status);   // "Ready", "Needs preparing", "Interpreted"
const char *status_detail(GameStatus status); // a line saying what that means

// "0:42", "12:05", "1:02:09"
std::string format_duration(double seconds);
// the executable's version as the launcher showed it: "1.0.65535.0"
std::string version_text(uint32_t version);
std::string html_escape(std::string_view text);
// a hue (0-359) from a title ID, for the tile of a game without an image
uint32_t hue_of(uint32_t title_id);
// "hsl(...)" colours for it
std::string tile_background(uint32_t title_id);

// The accent colour and its shades: tokens {accent}, {accent-hi}, ... in
// the stylesheet. An empty or bad "#RRGGBB" gives the default (ember).
struct Accent {
    std::string base, hi, lo, a08, a12, a15, a25, a40, a60, on;
};
Accent accent_from_hex(const std::string &hex);
const std::vector<std::string> &accent_choices(); // the swatches the settings offer
std::string apply_tokens(std::string text, const Accent &accent);

// Keys as a binding holds them ("^A", "_W", "Left", "Q I"): each key's cap
// and whether Shift is held or not.
struct KeyCap {
    std::string label;
    bool shift = false, no_shift = false;
};
std::vector<KeyCap> parse_keys(const std::string &keys);
std::string key_label(const std::string &sdl_name); // "Return" is "Enter", "Left" an arrow

struct BindingInfo {
    const char *label;
    const char *group;
    const char *part; // which part of the drawn controller lights up
};
BindingInfo binding_info(const std::string &name);

// Spatial navigation: from the focused rectangle the best one in a
// direction, as RmlUi's nav: auto does not cross scroll containers.
struct NavRect {
    float x = 0, y = 0, w = 0, h = 0;
};
enum class NavDir { Up, Down, Left, Right };
// the index of the best candidate (not `from`), or -1
int nav_pick(const std::vector<NavRect> &rects, int from, NavDir dir);

} // namespace xf::frontend
