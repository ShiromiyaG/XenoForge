// The console's own settings in the player's F1 overlay (Dear ImGui): the System page (language, country, video), the
// Profile page (gamertag, the profiles, the saves' format) and the title's achievements, read-only; the System
// page's Picture section holds the emulator's picture options (settings.h's video section). They edit the
// same Settings the launcher's pages do (console_options.h and profiles.h hold the choices and the rules). A change
// that the running title reads only when it starts says so and offers a restart; the gamertag and the saves' format
// apply at once.

#pragma once

#include "settings.h"
#include "system_ui_view.h"

#include "xenoforge/kernel.h"

#include <filesystem>
#include <functional>
#include <string>

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
    };
    // Between Begin and End of the overlay's window; true when the settings changed (to be saved).
    bool draw(Host &host, double now);
    // The same pages as the console's guide (guide_theme.h): a window of their own sliding in from the left,
    // Achievements, Profile and System switched by the shoulders; `open` goes false when closed from inside.
    // True when the settings changed.
    bool draw_guide(Host &host, double now, bool &open);
    // the guide's page: 0 achievements, 1 profile, 2 system
    void set_guide_page(int page) { tab_ = page % 3; }

  private:
    void load(Host &h);
    bool system_page(Host &h);
    bool picture_page(Host &h);
    bool profile_page(Host &h, float field_width);
    void restart_notice(Host &h);
    void achievements_page(Host &h, double now, float icon);
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

} // namespace xf::frontend
