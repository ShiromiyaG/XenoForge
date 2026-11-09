// The console's own settings in the player's guide (Dear ImGui, in guide_theme.h's look): the System page (language,
// country, video), the Profile page (gamertag, the profiles, the saves' format) and the title's achievements,
// read-only; the System page's Picture section holds the emulator's picture options (settings.h's video section).
// F1 opens the same guide on the pages the player adds (Host::extra_pages: the window's and the sound's settings,
// the picture, the port's options; the run's counters). They edit the same Settings the launcher's pages do
// (console_options.h and profiles.h hold the choices and the rules). A change that the running title reads only when
// it starts says so and offers a restart; the gamertag and the saves' format apply at once.

#pragma once

#include "settings.h"
#include "system_ui_view.h"

#include "xenoforge/kernel.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xf::frontend {

class ConsolePages {
  public:
    struct Host {
        Settings &settings;
        std::filesystem::path storage; // the storage root: where the profiles are
        kernel::Kernel &kernel;
        IconCache &icons;
        // the [system] values written to the kernel's console settings (kernel::apply_system_settings)
        std::function<void()> apply_system;
        // the title started again, to take what it reads at start (a process of its own)
        std::function<void()> restart;
        // the picture options taken by the renderer and the window at once (the anisotropy, the texture filter, the
        // output scaling); the resolution scale waits for a restart
        std::function<void()> apply_video = {};
        // what the renderer says of the scale it draws at (a scale it could not take, and the one it took instead)
        std::string picture_notice = {};
        // pages after the console's three, switched like them: the player's own (what F1 opens). A page draws its
        // widgets with guide_label / guide_caption and returns true when the settings changed.
        struct Page {
            std::string name;
            std::function<bool()> draw;
        };
        std::vector<Page> extra_pages = {};
    };
    // The same pages as the console's guide (guide_theme.h): a window of their own sliding in from the left,
    // Achievements, Profile and System switched by the shoulders; `open` goes false when closed from inside.
    // True when the settings changed.
    bool draw_guide(Host &host, double now, bool &open);
    // the guide's page: 0 achievements, 1 profile, 2 system, then the host's extra pages
    void set_guide_page(int page) { tab_ = page < 0 ? 0 : page; }
    // the notice that a change waits for a restart, with its button (under a page's settings)
    void restart_notice(Host &h);
    // The picture options alone (the F1 overlay shows them open, the guide in its System page); true when the
    // settings changed
    bool picture_page(Host &host);

  private:
    void load(Host &h);
    bool system_page(Host &h);
    bool profile_page(Host &h, float field_width);
    void achievement_rows(Host &h, double now, const guide::Scale &sc, float width);

    char gamertag_[32] = {}, new_name_[32] = {};
    bool loaded_ = false;
    int tab_ = 0;
    double guide_shown_at_ = -1;
    std::string error_, confirm_delete_;
    bool restart_needed_ = false;
    double summary_at_ = -10;
    kernel::ProfileSummary summary_;
};

// Inside a guide page: a setting's name at the left with its control filling the row (the string is the control's
// label), and a section's caption
std::string guide_label(const char *label);
void guide_caption(const char *text);

} // namespace xf::frontend
