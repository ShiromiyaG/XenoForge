// The console's settings a player can choose (console_options.cpp): the languages, countries and video modes
// config.toml's [system] takes, shared by the launcher's System page and the player's F1 overlay, with the validation
// that keeps what is written to what the kernel's SystemSettings (kernel.h, apply_system_settings) knows.

#pragma once

#include "settings.h"

#include <string>
#include <vector>

namespace xf::frontend {

struct Option {
    int value;
    const char *name;
};

// the values with their names; the first of each is "leave the console's default"
const std::vector<Option> &languages();
const std::vector<Option> &countries();
const std::vector<Option> &resolutions(); // by the screen's height: 0, 480, 720, 1080
const std::vector<Option> &refresh_rates();
const std::vector<Option> &widescreen_modes();

// the resolution choice a [system] holds (its height, 0 for the default), and one set into it
int resolution_of(const Settings::System &s);
void set_resolution(Settings::System &s, int height);
// values the kernel would not know put back to the default
void clamp_system(Settings::System &s);

// the index of a value in a list (0 when it is not there)
size_t index_of(const std::vector<Option> &list, int value);

} // namespace xf::frontend
