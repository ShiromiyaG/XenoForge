// The launcher's windows as RML (docs/runtime.md, "The launcher's interface"):
// strings built from plain values, which launcher_view.cpp and
// settings_view.cpp put into their documents. The look is the stylesheets'
// (ui/assets/*.rcss); here are only the structure and the words. Elements that
// do something carry data-action (and data-arg); the settings' rows carry
// data-kind and data-key. No RmlUi here.

#pragma once

#include "launcher_model.h"
#include "profiles.h"
#include "settings.h"

#include "xenoforge/kernel.h"

#include <string>
#include <string_view>
#include <vector>

namespace xf::frontend {

// An icon of the Lucide font (ISC; ui/fonts/Lucide.ttf), by its name
// ("play", "folder-open"): the glyph as UTF-8, "" for a name not listed here.
std::string icon_glyph(std::string_view name);
std::string icon_html(std::string_view name, const char *classes = "ic");
// "{i:name}" in a template made the icon's span
std::string expand_icons(std::string text);

// A game's picture: an image by the renderer's name, or a coloured square
// with the title's initial.
std::string art_style(const std::string &icon, uint32_t title_id);
std::string art_letter(const std::string &icon, const std::string &title);

// ----------------------------------------------------------------- the main window

// A menu's item: an action (with its argument), what it says, its shortcut's
// text and icon; greyed when it cannot be done now, a check mark for the
// option that is chosen, or a line between groups.
struct MenuItem {
    std::string action, arg, label, shortcut, icon;
    bool enabled = true, checked = false, separator = false;
};
MenuItem menu_separator();
std::string menu_html(const std::vector<MenuItem> &items);

// One row of the games table.
struct RowInfo {
    int index = 0; // in the listing
    std::string title, id_text, version, file;
    std::string icon; // the renderer's name for the game's image ("" when it has none)
    uint32_t title_id = 0;
    GameStatus status = GameStatus::Ready;
    bool running = false;
    bool unreadable = false; // the file could not be read (then title is its name, file why)
};
// the header's labels (the sorted one marked) and a game's row: its cover and
// name, its status, title ID and version, and the Options button
std::string table_head_html(Column sort, bool ascending);
std::string table_row_html(const RowInfo &row, bool selected);
// what the table area says with nothing to list: no folder chosen, a folder
// without games, or a filter that hides them all
std::string empty_html(bool no_folder, bool filtered, bool loading, const std::string &folder);

// A dialog over the window: a title bar (with a close button), a body and a
// row of buttons at the bottom.
std::string dialog_html(const std::string &title, const std::string &body, const std::string &buttons, int width_dp);
std::string button_html(const std::string &action, const std::string &label, bool primary = false, const std::string &id = {},
                        const std::string &icon = {});
std::string about_body_html();
// the prepare dialog's body, whose parts the view then updates by id
std::string prepare_body_html(const std::string &title, const std::string &icon, uint32_t title_id);
const char *prepare_stage_label(int stage); // 0 to 4

// ----------------------------------------------------------------- the settings window

enum class SettingsTab { Video = 0, Sound, Controllers, Keyboard, System, Profile, Interface };
constexpr int SETTINGS_TABS = 7;
const char *settings_tab_label(SettingsTab tab);
const char *settings_tab_name(SettingsTab tab); // "video", "sound"...

struct SettingsPage {
    const Settings *settings = nullptr;
    std::string game_title; // "" when no game is selected
    std::string title_id;
    std::string folder;     // the games folder
    std::string accent;     // the current accent, "#rrggbb"
    std::string theme;      // "system", "light", "dark"
    std::string backdrop;   // the ribbons: "animated", "still", "off"
    bool sounds = true;     // the window's own sounds
    bool smooth_edges = true; // multisampling for the window's drawing
    bool discord = true;      // Discord Rich Presence
    bool discord_has_id = false; // an application ID is set (else presence is off whatever the toggle says)
    std::vector<std::string> gamepads;
    std::string capturing;  // the binding waiting for a key
    // the Profile tab: the storage's profiles, a message for the last action, the profile asked to be deleted (the
    // second click confirms), and the selected game's achievements from the selected profile's GPD
    std::vector<ProfileEntry> profiles;
    std::string profile_message, confirm_delete;
    std::vector<kernel::ProfileAchievement> achievements;
    bool achievements_read = false;
};
std::string settings_tabs_html(SettingsTab current);
// the strip above the tabs: whose settings these are, and the switch that
// gives the selected game settings of its own
std::string settings_scope_html(const SettingsPage &page);
std::string settings_page_html(SettingsTab tab, const SettingsPage &page);

// the labels of a dropdown's options
struct Choice {
    std::string value, label;
};
std::vector<Choice> aspect_choices();
// the video section's choices (settings.h's video_choices) for a dropdown
std::vector<Choice> video_choice_list(const std::string &key);
std::vector<Choice> user_choices();
std::vector<Choice> theme_choices();
std::vector<Choice> backdrop_choices();
std::string keycaps_html(const std::string &keys, bool waiting);

} // namespace xf::frontend
