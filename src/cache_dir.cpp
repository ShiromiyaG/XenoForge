// game_build.h's default_cache_dir, in a file of its own: the player and boot
// keep their pipeline cache and shader storage there also when they link a
// port's code and none of the recompile step.

#include "xenoforge/game_build.h"

#include <cstdlib>

namespace xf::build {

namespace fs = std::filesystem;

fs::path default_cache_dir() {
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA")) return fs::path(local) / "xenoforge" / "cache";
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME")) return fs::path(xdg) / "xenoforge";
    if (const char *home = std::getenv("HOME")) return fs::path(home) / ".cache" / "xenoforge";
#endif
    return fs::temp_directory_path() / "xenoforge-cache";
}

} // namespace xf::build
