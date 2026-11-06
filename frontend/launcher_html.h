// The launcher's screens as RML (docs/runtime.md, "The launcher"): strings
// built from plain values, which launcher_view.cpp puts into the document.
// The look is launcher.rcss's; here are only the structure and the words.
// Elements that do something carry data-action (and data-arg); the settings'
// rows carry data-kind and data-key. No RmlUi here.

#pragma once

#include "launcher_model.h"
#include "settings.h"

#include <string>
#include <vector>

namespace xf::frontend {

struct TileInfo {
    int index = 0;
    std::string title, id_text;
    std::string icon; // the renderer's name for the game's image ("" when it has none)
    uint32_t title_id = 0;
    GameStatus status = GameStatus::Ready;
    std::string error; // set for a file that could not be read
};

struct DetailInfo {
    std::string title, id_text, version, file, runs_in, icon;
    uint32_t title_id = 0;
    GameStatus status = GameStatus::Ready;
    bool has_player = true;
    bool running = false;
    std::vector<std::string> last_run; // how its last run ended
};

// the pictures: an image by name, or a coloured tile with the title's initial
std::string art_style(const std::string &icon, uint32_t title_id);
std::string art_letter(const std::string &icon, const std::string &title);

std::string tiles_html(const std::vector<TileInfo> &tiles);
std::string skeleton_html(int count);
std::string detail_html(const DetailInfo &game);
std::string detail_empty_html(bool scanning);
std::string empty_html(bool no_folder, const std::string &folder);

struct SettingsPage {
    const Settings *settings = nullptr;
    std::string game_title;  // "" when no game is selected
    std::string title_id;
    std::string folder;      // the games folder
    std::string accent;      // the current accent, "#rrggbb"
};
std::string settings_html(const SettingsPage &page);
// the labels of a select's options, and which is current
struct Choice {
    std::string value, label;
};
std::vector<Choice> aspect_choices();
std::vector<Choice> user_choices();

struct ControlsPage {
    const Settings *settings = nullptr;
    std::vector<std::string> gamepads;
    std::string capturing; // the binding waiting for a key
    std::string game_title;
};
std::string controls_html(const ControlsPage &page);
std::string keycaps_html(const std::string &keys, bool waiting);

std::string about_html();

// the prepare screen's card, whose parts the view then updates by id
std::string prepare_html(const std::string &title, const std::string &icon, uint32_t title_id);
const char *prepare_stage_label(int stage); // 0 to 4

} // namespace xf::frontend
