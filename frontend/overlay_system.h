// The console's own settings in the player's F1 overlay (Dear ImGui): the System page (language, country, video), the
// Profile page (gamertag, the profiles, the saves' format) and the title's achievements, read-only. They edit the
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
    };
    // Between Begin and End of the overlay's window; true when the settings changed (to be saved).
    bool draw(Host &host, double now);

  private:
    char gamertag_[32] = {}, new_name_[32] = {};
    bool loaded_ = false;
    std::string error_, confirm_delete_;
    bool restart_needed_ = false;
    double summary_at_ = -10;
    kernel::ProfileSummary summary_;
};

} // namespace xf::frontend
