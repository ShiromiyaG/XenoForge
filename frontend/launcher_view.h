// The launcher's main window in RmlUi (docs/runtime.md, "The launcher's
// interface"), in the manner of the classic emulator front ends: a menu bar,
// a toolbar, the games as a table (sortable, its columns resizable, filtered
// by the search box) and a status bar; preparing a game, the about box and
// messages as dialogs over it; the settings as a second page beside the
// library, which the window's strip slides to (settings_view.h). Over
// LauncherCore (what the launcher does) and
// PrepareJob (a game being prepared). The structure and words are
// launcher_html.cpp's, the look the stylesheets' (ui/assets), what a status or
// a colour is launcher_model.cpp's; this file connects them to the document.
//
// Input: the mouse and keys as a desktop program takes them (the arrows,
// Page Up and Down, Home and End move the selection, Enter plays, F2 the
// game's settings, F5 rescans, Ctrl+O opens a folder, Ctrl+F searches, Alt or
// F10 the menus); a pad moves the selection with the d-pad or the stick, A or
// Start plays, X opens the game's settings, Y rescans, B closes, Back opens
// the menus.

#pragma once

#include "launcher_core.h"
#include "launcher_html.h"
#include "prepare_job.h"
#include "settings_view.h"
#include "ui/ui_renderer.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace xf::frontend {

enum class Screen { Library = 0, Settings, Controls, About };
enum class Section { Library, Settings }; // the page of the strip that is shown

struct ViewOptions {
    Screen screen = Screen::Library; // --screen: settings and controls slide to the settings page, about opens the about box
    long select = -1;                // --select N
    long start_index = -1;           // --start N: started as soon as it is listed
    bool demo_progress = false;      // --demo-progress: a fake build on show
    bool demo_fail = false;          // ... that fails
    double demo_speed = 12;          // times real time
    bool show_log = false;           // the prepare dialog's log open
    std::string capture;             // --capture-binding: a binding waiting for a key, for pictures
    std::string menu;                // --menu <name>: that menu open, for pictures
    std::string tab;                 // --tab <name>: the settings page's tab --screen settings opens
    bool context_menu = false;       // --context-menu: the selected row's menu open, for pictures
    std::vector<std::string> player_args;
    unsigned build_jobs = 0; // --build-jobs N: compilers at once while preparing (0: the build's default)
    float scale = 0;         // --scale: the interface's scale (0: the display's)
    bool offscreen = false;  // --render-size: the window drawn without being shown
};

class LauncherView final : public Rml::EventListener, public SettingsHost {
  public:
    LauncherView(LauncherCore &core, ui::Renderer &renderer, SDL_Window *window, ViewOptions options);
    ~LauncherView() override;

    // The document made from the files in assets_dir (launcher.rml, theme.rcss
    // and launcher.rcss, with the theme's and accent's tokens filled in).
    bool load(Rml::Context *context, const std::filesystem::path &assets_dir, std::string &error);
    // Once a frame, before the context's Update.
    void update();
    // An SDL event of the main window (and the pad's while it has focus):
    // true when it was used and the context should not see it.
    bool on_event(const SDL_Event &event);
    // the desktop's theme changed (SDL_EVENT_SYSTEM_THEME_CHANGED)
    void system_theme_changed();

    // The view is waiting for something that --quit-after must not cut: a
    // listing under way, a game it was told to start, one running.
    bool busy() const;
    size_t games_listed() const { return listing_.games.size(); }
    bool wants_quit() const { return quit_; } // File > Exit

    void ProcessEvent(Rml::Event &event) override;

    // SettingsHost
    void look_changed() override;
    void browse_folder() override;
    void set_folder(const std::string &folder) override;
    void rescan() override;
    void leave_settings() override;
    bool pad_mode() const override { return pad_mode_; }
    void play_sound(Cue cue) override;

  private:
    enum class Dialog { None, Prepare, About, Message };

    // building
    std::string styled(const std::string &rcss) const; // a stylesheet with the theme's and the accent's tokens filled in
    bool build_document(std::string &error);
    void reload_document();
    bool resolve_dark() const;
    void rebuild_head();
    void rebuild_rows(bool force = false);
    void update_toolbar();
    void update_status();
    void update_backdrop();
    void fade_glow(Rml::Element *row, bool on);
    void animate_rows_in(); // the rows slide in from the left one after another (a list that has just appeared)
    void set_message(const std::string &text, int kind, double seconds); // kind: 0 plain, 1 good, 2 error

    // state
    void poll_listing();
    const GameEntry *selected_game() const;
    bool cached_of(const GameEntry &game) const;
    const PlayerBinary *player_of(const GameEntry &game) const;
    GameStatus status_of(const GameEntry &game) const;
    std::string icon_name(const GameEntry &game);
    void select_game(long index, bool scroll);
    void move_selection(int delta, bool to_end = false);
    int visible_rows() const;
    void save_layout();

    // menus
    std::vector<MenuItem> menu_items(const std::string &name);
    void open_menu(const std::string &name, bool keyboard);
    void open_context_menu(float x, float y);
    void close_menu();   // closed, no sound: what was chosen sounds for itself
    void dismiss_menu(); // closed without choosing: its sound

    void menu_move(int delta);
    void menu_switch(int delta);

    // dialogs
    void show_dialog(Dialog dialog, const std::string &title, const std::string &body, const std::string &buttons, int width_dp);
    void close_dialog();
    void message_box(const std::string &title, const std::string &text);
    void show_about();

    // actions
    void do_action(const std::string &name, const std::string &arg);
    void play(const GameEntry &game, bool force_interpreted);
    void start_game(const GameEntry &game, bool interpreted);
    void begin_prepare(const GameEntry &game, bool demo);
    void show_prepare();
    void hide_prepare();
    void update_prepare();
    void open_settings(SettingsTab tab, bool own = false);
    void show_section(Section section);
    void open_path(const std::filesystem::path &path);

    // input
    bool on_key(SDL_Keycode key, SDL_Scancode scancode, bool repeat);
    bool on_pad_button(SDL_GamepadButton button, bool down);
    void on_pad_axis(SDL_GamepadAxis axis, int value);
    void dialog_navigate(NavDir dir);
    void update_input();
    Rml::Element *ancestor_with(Rml::Element *element, const char *attribute) const;
    Rml::Element *by_id(const char *id) const { return doc_ ? doc_->GetElementById(id) : nullptr; }
    float dp() const { return ctx_ ? ctx_->GetDensityIndependentPixelRatio() : 1.f; }
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    void defer_until(double when, std::function<void()> fn) { timers_.emplace_back(when, std::move(fn)); }

    LauncherCore &core_;
    ui::Renderer &renderer_;
    SDL_Window *window_;
    ViewOptions opt_;
    Rml::Context *ctx_ = nullptr;
    Rml::ElementDocument *doc_ = nullptr;
    std::filesystem::path assets_;
    std::string rml_template_, rcss_template_;
    bool dark_ = true;

    using Clock = std::chrono::steady_clock;
    Clock::time_point last_update_, last_listing_;
    double now_ = 0;
    double settle_until_ = 1.0; // the window still changing: a check's picture waits (busy)
    std::vector<std::function<void()>> deferred_;
    std::vector<std::pair<double, std::function<void()>>> timers_;

    // the games and the table
    LauncherCore::Listing listing_;
    std::vector<size_t> order_; // the listing's indices as the table shows them (sorted, filtered)
    std::string rows_sig_;
    bool animate_rows_ = true; // the next full list of rows comes in animated (the start, a new folder, a new sort)
    UiSound sound_;
    std::set<uint32_t> icons_;  // title IDs whose image the renderer has
    std::set<uint32_t> failed_; // games whose preparing failed here
    long selected_ = -1;
    std::string filter_;
    Column sort_ = Column::Title;
    bool ascending_ = true;
    // whether a library is cached for a disc, by its digest: asked of the
    // cache folder once, again after a rescan or a build
    mutable std::map<uint64_t, bool> cached_;
    mutable std::map<uint64_t, bool> stale_; // by digest: the cached library lacks entries the runtime logged
    bool stale_of(const GameEntry &game) const;

    // the backdrop's ribbons: their clock (it stops with the window out of sight)
    double backdrop_clock_ = 7.0, backdrop_seen_ = 0;

    // the status bar's message
    std::string message_;
    int message_kind_ = 0;
    double message_until_ = 0;
    std::string status_sig_;

    // menus
    std::string menu_;        // the open one ("" none; "context" the row's)
    std::vector<MenuItem> menu_items_;
    int hot_ = -1;            // the item the keys move
    bool menu_keyboard_ = false;

    // dialogs
    Dialog dialog_ = Dialog::None;
    bool pad_mode_ = false;

    // preparing a game
    PrepareJob job_;
    bool prepare_hidden_ = false; // running behind the status bar
    bool prepare_listed_ = false;
    bool log_open_ = false;
    long prepare_game_ = -1;
    uint32_t prepare_title_id_ = 0;
    std::string prepare_title_;
    int prepare_stage_ = 0;
    size_t prepare_log_lines_ = 0;
    std::string prepare_kind_; // running, failed, done: which buttons are shown
    std::string last_prep_count_, last_prep_time_, last_prep_file_, last_prep_state_;

    // checks and pictures
    long start_pending_ = -1;
    long select_pending_ = -1;
    bool demo_pending_ = false;
    std::string menu_pending_;
    bool context_pending_ = false;
    bool screen_pending_ = false;

    // the settings page, and which page of the strip is shown
    std::unique_ptr<SettingsView> settings_;
    Section section_ = Section::Library;
    std::string capture_pending_;

    // the pad's repeats
    struct Repeat {
        bool active = false;
        int delta = 0;
        double next = 0;
    };
    Repeat pad_repeat_, stick_repeat_;
    bool quit_ = false;
};

} // namespace xf::frontend
