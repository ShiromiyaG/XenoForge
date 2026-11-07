// Whether a game's cached library is out of date because the runtime logged
// interpreted entries the library was not built with (plan 2a, D3's
// feedback), so the launcher can show it and offer a rebuild.
//
// EXPECTED API: the answer belongs to game_build.h (xf-recomp is adding it).
// Until it is there, library_has_new_entries is a stub that returns false;
// the one place to connect is its body in stale_library.cpp, which should
// call the game_build.h function (assumed shape:
//   bool xf::build::has_new_entries_since_built(const std::filesystem::path &cache_dir, std::string_view digest);
// true when the runtime's log of interpreted entries for that digest holds
// addresses the cached library was built without).
#pragma once

#include <filesystem>
#include <string_view>

namespace xf::frontend {

// true when the library cached for `digest` (16 hex digits) should be rebuilt to take in new entries
bool library_has_new_entries(const std::filesystem::path &cache_dir, std::string_view digest);

} // namespace xf::frontend
