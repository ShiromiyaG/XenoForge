// The launcher's settings window (docs/runtime.md, "The launcher's
// interface"): a window of its own beside the main one, with tabs (video,
// sound, controllers, keyboard, interface) over the selected game's settings
// as the player reads them (the shared ones, or the game's own). It has its
// own Vulkan device and swapchain (ui::Window) and its own RmlUi context;
// fonts are RmlUi's, shared. What it changes is saved as it changes; the look
// (theme, accent) and the games folder go back to the main window through
// SettingsHost.
//
// Input: the mouse and keys of its window as RmlUi takes them; the arrows and
// a pad move focus by direction (launcher_model.h's nav_pick) and Left and
// Right change a slider, a dropdown or a check box; Ctrl+Tab or the shoulder
// buttons change tabs; Escape or B closes a dropdown, a key being set, then
// the window.

#pragma once

#include "launcher_core.h"
#include "launcher_html.h"
#include "ui/ui_window.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xf::frontend {

// What the settings window asks of the main one.
class SettingsHost {
  public:
    virtual ~SettingsHost() = default;
    // a stylesheet with the theme's and the accent's tokens filled in
    virtual std::string styled(const std::string &rcss) const = 0;
    // the theme or the accent changed: both windows drawn again with it
    virtual void look_changed() = 0;
    // the system's folder dialog over this window, and a folder chosen or typed
    virtual void browse_folder(SDL_Window *parent) = 0;
    virtual void set_folder(const std::string &folder) = 0;
    virtual void rescan() = 0;
};

class SettingsView final : public Rml::EventListener {
  public:
    SettingsView(LauncherCore &core, SettingsHost &host);
    ~SettingsView() override;

    // The window made over the main one (parent) on a tab. scale > 0 fixes the
    // interface's scale (else the display's); offscreen: never shown, drawn at
    // its own size for a picture.
    bool open(SDL_Window *parent, const std::filesystem::path &assets, SettingsTab tab, float scale, bool offscreen, std::string &error);
    // the game selected in the list (nullptr: none): whose settings are edited
    void set_game(const GameEntry *game);
    void show_tab(SettingsTab tab);
    void restyle();        // the theme or the accent changed (done at the next frame)
    void folder_changed(); // the games folder changed elsewhere
    void begin_capture(const std::string &binding);

    SDL_Window *window() const { return window_; }
    SDL_WindowID window_id() const { return window_ ? SDL_GetWindowID(window_) : 0; }
    Rml::Context *context() const { return ctx_; }
    ui::Window &surface() { return *surface_; }
    bool has_focus() const;

    // An event of this window (or the pad's while it has focus): true when
    // the window used it and its context should not see it.
    bool on_event(const SDL_Event &event);
    // Once a frame: its own updates, the layout and the picture.
    void frame();
    // its Close or Escape asked for the window to go
    bool closing() const { return close_; }
    // still changing: a picture of it waits
    bool busy() const { return now_ < settle_until_ || !deferred_.empty() || restyle_; }

    void ProcessEvent(Rml::Event &event) override;

  private:
    bool build_document(std::string &error);
    void rebuild(); // the scope, the tabs and the page
    void reload_edit();
    void save_edit();
    SettingsPage page();

    void do_action(const std::string &name, const std::string &arg, Rml::Element *element);
    bool *bool_field(const std::string &key);
    bool get_slider(const std::string &key, double &fraction, std::string &text);
    void set_slider(const std::string &key, double fraction);
    void refresh_slider(Rml::Element *row);
    void drag_slider(float mouse_x);
    void toggle_setting(Rml::Element *row);
    std::vector<Choice> choices_for(const std::string &key) const;
    std::string choice_value(const std::string &key) const;
    void open_popup(Rml::Element *row);
    void close_popup();
    void choose_option(const std::string &key, const std::string &value, Rml::Element *row);
    void end_capture();
    // the Profile tab: the profile actions (rename, create, use, delete), each checked by profiles.h's rules
    void profile_action(const std::string &name, const std::string &arg);
    std::string input_value(const char *id) const;

    bool on_key(SDL_Keycode key, SDL_Scancode scancode, bool repeat);
    bool on_pad_button(SDL_GamepadButton button, bool down);
    void navigate(NavDir dir);
    void activate();
    void back();
    std::vector<Rml::Element *> focusables();
    void ensure_visible(Rml::Element *element);
    void focus_first();
    Rml::Element *ancestor_with(Rml::Element *element, const char *attribute) const;
    Rml::Element *by_id(const char *id) const { return doc_ ? doc_->GetElementById(id) : nullptr; }
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

    LauncherCore &core_;
    SettingsHost &host_;
    SDL_Window *window_ = nullptr;
    std::unique_ptr<ui::Window> surface_;
    Rml::Context *ctx_ = nullptr;
    Rml::ElementDocument *doc_ = nullptr;
    std::string name_; // the context's
    std::filesystem::path assets_;
    std::string rml_template_, rcss_template_;
    float scale_ = 0;
    bool offscreen_ = false;
    uint32_t off_w_ = 0, off_h_ = 0;

    SettingsTab tab_ = SettingsTab::Video;
    GameEntry game_;
    bool has_game_ = false;
    Settings edit_;
    bool edit_valid_ = false;
    std::string capturing_;
    std::string profile_message_, confirm_delete_; // the Profile tab's last message; the profile asked to be deleted
    std::string popup_key_;
    Rml::Element *popup_row_ = nullptr;
    Rml::Element *drag_row_ = nullptr;
    bool close_ = false, restyle_ = false;
    bool pad_mode_ = false;

    double now_ = 0, settle_until_ = 0.6;
    uint64_t last_ticks_ = 0;
    std::vector<std::function<void()>> deferred_;
};

} // namespace xf::frontend
