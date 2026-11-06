// The emulator's recompile step (docs/plan.md, D1 and D2): a disc's game
// library built on the player's machine into a cache, and found again by
// its executable's digest. The launcher calls it on a worker thread and
// shows the progress; boot and the generic player find the cached library.
//
// This header is the contract between the launcher's view and the build.
// Until the build is in, prepare_game() reports that it is not and
// find_cached() finds nothing (src/game_build.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace xf::build {

// the stages a build goes through, in order
enum class Stage { Reading, Discovering, Emitting, Compiling, Linking, Done, Failed };

// what the build reports as it goes, from its worker thread: the stage,
// how far it is in it (done of total units, 0 of 0 when unknown) and a
// line to show (a module's name, a file, the error)
struct Progress {
    std::function<void(Stage stage, uint64_t done, uint64_t total, std::string_view detail)> report;
    // set by the caller to stop the build; it stops at the next unit
    std::atomic<bool> *cancel = nullptr;
};

// a game library in the cache
struct CachedGame {
    std::filesystem::path library; // the DLL (.so) to load
    std::string digest;            // the executable's image digest, as the launcher's library shows it
    uint32_t interface_version = 0;
};

// where the cache lives by default (%LOCALAPPDATA%\xenoforge\cache, or
// $XDG_CACHE_HOME/xenoforge)
std::filesystem::path default_cache_dir();

// the cached library for an executable's digest, if one was built for this
// XenoForge's interface version
std::optional<CachedGame> find_cached(const std::filesystem::path &cache_dir, std::string_view digest);

// builds the disc's game library into the cache; blocks, so it runs on a
// worker thread. false with the error when it fails or is cancelled.
bool prepare_game(const std::filesystem::path &disc, const std::filesystem::path &cache_dir, Progress &progress,
                  std::string &error);

} // namespace xf::build
