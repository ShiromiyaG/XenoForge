// The launcher's screens in RmlUi (docs/runtime.md, "The launcher"): the
// library with its detail panel, the preparing of a game, the settings, the
// controls and the about page, over LauncherCore (what the launcher does) and
// PrepareJob (a game being prepared). The text and structure of each screen
// are launcher_html.cpp's, its look launcher.rcss's, what a status or a
// shade is, launcher_model.cpp's; this file connects them to the document:
// it rebuilds a screen's parts when what they show changes, turns clicks,
// keys and the pad into actions, and moves focus (a pad has no pointer).
//
// Input comes as SDL events: the mouse and keys the context takes as they
// are (ui_system.h); the pad's d-pad and left stick move focus with the
// spatial navigation of launcher_model.h, A presses, B goes back, the
// shoulder buttons turn pages.

#pragma once

#include "launcher_core.h"
#include "launcher_html.h"
#include "prepare_job.h"
#include "ui/ui_renderer.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace xf::frontend {

enum class Screen { Library = 0, Settings, Controls, About };

struct ViewOptions {
    Screen screen = Screen::Library;
    long select = -1;          // --select N
    long start_index = -1;     // --start N: started as soon as it is listed
    bool demo_progress = false; // --demo-progress: a fake build on show
    bool demo_fail = false;     // ... that fails
    double demo_speed = 12;     // times real time
    bool show_log = false;      // the prepare screen's log open
    std::string capture;        // --capture-binding: a binding waiting for a key, for pictures
    std::vector<std::string> player_args;
    unsigned build_jobs = 0;    // --build-jobs N: compilers at once while preparing (0: the build's default)
};

class LauncherView final : public Rml::EventListener {
  public:
    LauncherView(LauncherCore &core, ui::Renderer &renderer, SDL_Window *window, ViewOptions options);
    ~LauncherView() override;

    // The document made from the files in assets_dir (launcher.rml and
    // .rcss, with the accent's tokens filled in) on the context.
    bool load(Rml::Context *context, const std::filesystem::path &assets_dir, std::string &error);
    // Once a frame, before the context's Update.
    void update();
    // An SDL event: what the view uses it for (the pad, keys that act). True
    // when it was used and the context should not see it.
    bool on_event(const SDL_Event &event);

    // The view is waiting for something that --quit-after must not cut: a
    // listing under way, a game it was told to start, one running.
    bool busy() const;
    // the games listed and the run's end, for the check's last lines
    size_t games_listed() const { return listing_.games.size(); }

    void ProcessEvent(Rml::Event &event) override;

  private:
    struct Action {
        std::string name, arg;
        Rml::Element *element = nullptr;
    };

    // building
    bool build_document(std::string &error);
    void reload_document(); // the accent changed: the same screens, new tokens
    void rebuild_library();
    void rebuild_detail();
    void rebuild_settings();
    void rebuild_controls();
    void rebuild_hints();
    void show_screen(Screen screen, bool instant = false);
    void set_backdrop(const GameEntry *game);
    void place_pill();

    // state
    void poll_listing();
    const GameEntry *selected_game() const;
    bool prepare_listed_ = false; // the library was drawn again after the build ended
    bool cached_of(const GameEntry &game) const; // a library is cached for the disc
    const PlayerBinary *player_of(const GameEntry &game) const;
    GameStatus status_of(const GameEntry &game) const;
    std::string icon_name(const GameEntry &game);
    void select_game(int index, bool focus_tile);
    void reload_edit();
    void save_edit();

    // actions
    void do_action(const Action &action);
    void play(const GameEntry &game, bool force_interpreted);
    void start_game(const GameEntry &game, bool interpreted);
    void begin_prepare(const GameEntry &game, bool demo);
    void close_prepare();
    void update_prepare();
    void browse();
    void set_folder(const std::string &folder);
    void toast(const std::string &text, bool error, bool with_stop, double seconds);
    void clear_toast();

    // settings widgets
    bool *bool_field(const std::string &key);
    bool get_slider(const std::string &key, double &fraction, std::string &text);
    void set_slider(const std::string &key, double fraction);
    void toggle_setting(Rml::Element *row);
    void refresh_slider(Rml::Element *row);
    void open_popup(Rml::Element *row);
    void close_popup();
    void choose_option(const std::string &value);
    std::vector<Choice> choices_for(const std::string &key) const;
    std::string choice_value(const std::string &key) const;
    void drag_slider(float mouse_x);

    // input
    bool on_key(SDL_Keycode key, SDL_Scancode scancode, bool down, bool repeat);
    bool on_pad_button(SDL_GamepadButton button, bool down);
    void on_pad_axis(SDL_GamepadAxis axis, int value);
    void navigate(NavDir dir);
    void activate();
    void back();
    void next_page(int delta);
    std::vector<Rml::Element *> focusables();
    void ensure_visible(Rml::Element *element);
    void focus_first();
    void light_part(const std::string &part, bool on);
    void caption_for(Rml::Element *bind);
    void begin_capture(const std::string &binding);
    void end_capture(bool saved);
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    void defer_until(double when, std::function<void()> fn) { timers_.emplace_back(when, std::move(fn)); }
    void update_input(double dt);
    Rml::Element *ancestor_with(Rml::Element *element, const char *attribute) const;
    Rml::Element *ancestor_with_class(Rml::Element *element, const char *name) const;
    Rml::Element *by_id(const char *id) const { return doc_ ? doc_->GetElementById(id) : nullptr; }
    float dp() const { return ctx_ ? ctx_->GetDensityIndependentPixelRatio() : 1.f; }

    LauncherCore &core_;
    ui::Renderer &renderer_;
    SDL_Window *window_;
    ViewOptions opt_;
    Rml::Context *ctx_ = nullptr;
    Rml::ElementDocument *doc_ = nullptr;
    std::filesystem::path assets_;
    std::string rml_template_, rcss_template_;

    using Clock = std::chrono::steady_clock;
    Clock::time_point last_update_, last_listing_;
    double now_ = 0;
    double settle_until_ = 1.0; // the screen still changing: a check's picture waits (busy)

    LauncherCore::Listing listing_;
    std::string library_sig_, detail_sig_;
    std::set<uint32_t> icons_;       // title IDs whose image the renderer has
    std::set<uint32_t> failed_;      // games whose preparing failed here
    long selected_ = -1;
    Settings edit_;
    uint32_t edit_title_ = 0;
    bool edit_valid_ = false;
    std::string message_;
    std::string capturing_;
    long start_pending_ = -1;
    long select_pending_ = -1;
    bool demo_pending_ = false;

    Screen screen_ = Screen::Library;
    bool pad_mode_ = false;
    bool library_empty_ = false; // the library screen shows an empty state
    int pill_frames_ = 3;
    int art_front_ = 0; // which backdrop layer is showing
    uint32_t art_title_ = 0xFFFFFFFF;

    // the prepare screen
    PrepareJob job_;
    bool prepare_open_ = false, log_open_ = false;
    long prepare_game_ = -1;
    uint32_t prepare_title_id_ = 0;
    int prepare_stage_ = 0;
    size_t prepare_log_lines_ = 0;
    std::string prepare_kind_; // running, failed, done: which actions are shown
    std::string last_prep_count_, last_prep_time_, last_prep_file_, last_prep_state_;

    // popup (a dropdown's options)
    std::string popup_key_;
    Rml::Element *popup_row_ = nullptr;

    // a slider being dragged
    Rml::Element *drag_row_ = nullptr;

    // toast
    double toast_until_ = 0;
    bool toast_stop_ = false;
    std::string toast_text_;
    bool toast_error_ = false;

    // smooth scrolling and the pad's repeats
    std::map<Rml::Element *, float> scroll_targets_;
    struct Repeat {
        bool active = false;
        NavDir dir = NavDir::Down;
        double next = 0;
    };
    Repeat dpad_repeat_, stick_repeat_;
    bool stick_dir_[4] = {false, false, false, false};
    int stick_x_ = 0, stick_y_ = 0;

    std::vector<std::function<void()>> deferred_;
    std::vector<std::pair<double, std::function<void()>>> timers_;
    double prepare_clear_at_ = 0;
    std::string capture_pending_;
};

} // namespace xf::frontend
