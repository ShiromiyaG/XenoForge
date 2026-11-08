// Whether a game's cached library is out of date because the runtime logged
// interpreted entries the library was not built with (plan 2a, D3's
// feedback), so the launcher can show it and offer a rebuild.
// The answer is game_build.h's has_new_entries: the runtime's entry log for
// that digest holds addresses past what the cached library's build took in,
// not counting the functions that build left out.
#pragma once

#include <filesystem>
#include <string_view>

namespace xf::frontend {

// true when the library cached for `digest` (16 hex digits) should be rebuilt to take in new entries
bool library_has_new_entries(const std::filesystem::path &cache_dir, std::string_view digest);

} // namespace xf::frontend
