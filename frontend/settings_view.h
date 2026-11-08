// The launcher's settings (docs/runtime.md, "The launcher's interface"): the
// second page of the main window, beside the library (launcher.rml's #spage;
// choosing "settings" in the header slides the window's strip to it, as the
// console's dashboard slides from one blade to the next). Tabs (video, sound,
// controllers, keyboard, interface) over the selected game's settings as the
// player reads them (the shared ones, or the game's own). It lives in the
// main window's document and context: LauncherView gives it the document
// (attach) and its keys and pad buttons while the page is the one shown;
// the mouse reaches it as events of the document. What it changes is saved
// as it changes; the look (theme, accent) and the games folder go back to
// the window through SettingsHost.
//
// Input: the arrows and a pad move focus by direction (launcher_model.h's
// nav_pick) and Left and Right change a slider, a dropdown or a check box;
// Tab (forward) and Ctrl+Tab or Shift+Tab (back), or the shoulder buttons, change tabs; Escape or B closes a
// dropdown, a key being set, then leaves the page.

#pragma once

#include "launcher_core.h"
#include "launcher_html.h"
#include "ui_sound.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace xf::frontend {

// What the settings page asks of the main window.
class SettingsHost {
  public:
    virtual ~SettingsHost() = default;
    // the theme or the accent changed: the window drawn again with it
    virtual void look_changed() = 0;
    // the system's folder dialog over the window, and a folder chosen or typed
    virtual void browse_folder() = 0;
    virtual void set_folder(const std::string &folder) = 0;
    virtual void rescan() = 0;
    // Escape or B at the page's top: back to the library
    virtual void leave_settings() = 0;
    // the pad (not the mouse or keys) is what the user last used: focus shows
    virtual bool pad_mode() const = 0;
    // a cue of the window's own sounds (nothing when they are off)
    virtual void play_sound(Cue cue) = 0;
};

class SettingsView final : public Rml::EventListener {
  public:
    SettingsView(LauncherCore &core, SettingsHost &host);
    ~SettingsView() override;

    // The page in a document (made again whenever the look changes: detach
    // before the old one goes, attach to the new one).
    void attach(Rml::Context *context, Rml::ElementDocument *document);
    void detach();

    // The page becomes the one shown, on a tab; or is left.
    void enter(SettingsTab tab);
    void leave();
    bool active() const { return active_; }
    SettingsTab tab() const { return tab_; }

    // the game selected in the list (nullptr: none): whose settings are edited
    void set_game(const GameEntry *game);
    // the game's settings become its own ("Custom settings for ..." checked), when they are not yet
    void use_own_settings();
    // direction: which way the page comes in, +1 from the right (a later tab), -1 from the left; 0: by the tabs' order
    void show_tab(SettingsTab tab, int direction = 0);
    void folder_changed(); // the games folder changed elsewhere
    void begin_capture(const std::string &binding);

    // The keys and the pad's buttons and stick while the page is shown:
    // true when it used them.
    bool on_key(SDL_Keycode key, SDL_Scancode scancode, bool repeat);
    bool on_pad_button(SDL_GamepadButton button, bool down);
    void on_pad_axis(SDL_GamepadAxis axis, int value);
    // Once a frame: the work put off to after the layout.
    void update();
    // still changing: a picture of it waits
    bool busy() const { return !deferred_.empty(); }

    void ProcessEvent(Rml::Event &event) override;

  private:
    void rebuild(); // the scope, the tabs and the page
    void slide_page_in(int direction); // the page's rows come in from that side (+1 right, -1 left), fading in
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

    void navigate(NavDir dir);
    void activate();
    void back();
    std::vector<Rml::Element *> focusables();
    void ensure_visible(Rml::Element *element);
    void focus_first();
    bool mine(Rml::Element *element) const; // inside the page or its dropdown's layer
    Rml::Element *ancestor_with(Rml::Element *element, const char *attribute) const;
    Rml::Element *by_id(const char *id) const { return doc_ ? doc_->GetElementById(id) : nullptr; }
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

    LauncherCore &core_;
    SettingsHost &host_;
    Rml::Context *ctx_ = nullptr;
    Rml::ElementDocument *doc_ = nullptr;

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
    bool active_ = false;
    float scroll_ = 0;  // the rows' scroll, kept while the document is made again
    int stick_dir_ = 0;

    std::vector<std::function<void()>> deferred_;
};

} // namespace xf::frontend
