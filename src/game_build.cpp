// The recompile step's placeholder (xenoforge/game_build.h): the cache's
// place, and no build yet. Replaced by the build (docs/plan.md, D2).
#include "xenoforge/game_build.h"

#include <cstdlib>

namespace xf::build {

std::filesystem::path default_cache_dir() {
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA")) return std::filesystem::path(local) / "xenoforge" / "cache";
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME")) return std::filesystem::path(xdg) / "xenoforge";
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".cache" / "xenoforge";
#endif
    return std::filesystem::temp_directory_path() / "xenoforge-cache";
}

std::optional<CachedGame> find_cached(const std::filesystem::path &, std::string_view) { return std::nullopt; }

bool prepare_game(const std::filesystem::path &, const std::filesystem::path &, Progress &progress,
                  std::string &error) {
    error = "the recompile step is not built yet";
    if (progress.report) progress.report(Stage::Failed, 0, 0, error);
    return false;
}

} // namespace xf::build
