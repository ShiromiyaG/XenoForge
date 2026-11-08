// The player's profiles (docs/runtime.md, "The player's system UI"): a profile is a directory of the storage root,
// content/<XUID>/ (its saves, and its package FFFE07D1/00010000/<XUID> with the GPDs, which is where the kernel keeps
// achievements and settings); its gamertag is config.toml's [profile.names]. The profile the games use is
// [profile] xuid (else the first the storage has, as the kernel picks it). The launcher's settings window, the
// player's F1 overlay and the player's start share this, so a rule (the gamertag's) is written once.
//
// Nothing here touches the kernel: a change to the profile the games use applies at the next title start
// (kernel::set_user), a rename of it also at once (kernel::set_gamertag).

#pragma once

#include "settings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xf::frontend {

// The console's gamertag rules, as far as they are checkable offline: 1 to 15 characters, letters, digits and
// single spaces, starting with a letter, no space at the end. False, and why.
bool valid_gamertag(const std::string &name, std::string &why);

struct ProfileEntry {
    std::string xuid; // 16 upper-case hex digits
    std::string name; // its gamertag ("" when none was given)
    bool selected = false;
};

// The profiles the storage root has (a directory content/<XUID> with a profile package inside), with their names,
// the selected one marked; when none exists yet, the selected XUID's entry alone if settings name one.
std::vector<ProfileEntry> list_profiles(const std::filesystem::path &storage, const Settings &settings);
// A new profile: a fresh offline XUID (E03... as the console's offline accounts), its directory and package, its
// gamertag in the settings; it becomes the selected one when there was none. False, and why, on a bad name or a
// storage that cannot be written.
bool create_profile(const std::filesystem::path &storage, Settings &settings, const std::string &name,
                    std::string &xuid_out, std::string &error);
bool rename_profile(Settings &settings, const std::string &xuid, const std::string &name, std::string &error);
// The profile selected for the games (it must exist in the settings' names or the storage)
bool select_profile(Settings &settings, const std::string &xuid, std::string &error);
// A profile's directory (its saves too) removed and its name dropped; the selected one is not removed while it is
// the only one
bool delete_profile(const std::filesystem::path &storage, Settings &settings, const std::string &xuid,
                    std::string &error);
// where the saves and profiles are: [saves] root, else storage/ beside the settings file
std::filesystem::path storage_root(const Settings &settings, const std::filesystem::path &config_file);
// the selected profile's XUID as the kernel takes it (0: the storage's own choice) and its gamertag
uint64_t selected_xuid(const Settings &settings);
std::string selected_gamertag(const std::filesystem::path &storage, const Settings &settings);

} // namespace xf::frontend
