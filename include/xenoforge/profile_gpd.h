// A profile's achievements read from its GPD file with no kernel running: what the launcher's profile page shows for
// a game that is not running. The kernel writes the file (docs/runtime.md, "Profiles and achievements"); the running
// player reads it through kernel::profile_summary instead.

#pragma once

#include "xenoforge/kernel.h"

#include <filesystem>
#include <vector>

namespace xf {

// the title's GPD in a profile of the storage root: content/<XUID>/FFFE07D1/00010000/<XUID>/<TITLE ID>.gpd
std::filesystem::path profile_gpd_path(const std::filesystem::path &storage, const std::string &xuid, uint32_t title_id);

// The achievements a GPD file holds, locked and unlocked, with the earned total; empty (and false) when the file
// is missing or not a GPD (a game that has not run yet). Icons are not in a GPD file's achievement entries read
// here (the image entries are: `icon` is filled when the GPD has the image).
bool read_gpd_achievements(const std::filesystem::path &gpd_file, std::vector<kernel::ProfileAchievement> &out);

} // namespace xf
