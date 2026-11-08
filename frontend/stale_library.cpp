#include "stale_library.h"

#include "xenoforge/game_build.h"

namespace xf::frontend {

// game_build.h's count of the addresses the runtime logged since the cached
// library was built (D3), but the functions that build left out
bool library_has_new_entries(const std::filesystem::path &cache_dir, std::string_view digest) {
    return build::has_new_entries(cache_dir, digest);
}

} // namespace xf::frontend
