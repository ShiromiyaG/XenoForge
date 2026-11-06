// The emulator's recompile step (docs/plan.md, D2; docs/runtime.md, "The
// game library"): a disc's game library built on the player's machine into a
// cache, and found again by its executable's digest. The launcher calls it
// on a worker thread and shows the progress; boot and the generic player
// find the cached library (game_library.h).
//
// This header is the contract between the launcher's view and the build:
// discovery and emission in process into a work directory under the cache,
// then clang and lld (the toolchain beside the launcher, else an installed
// LLVM) compiling the files in parallel and linking the library, which is
// kept as <cache>/<digest>-<interface version>.dll (.so).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xf::build {

// the stages a build goes through, in order
enum class Stage { Reading, Discovering, Emitting, Compiling, Linking, Done, Failed };

// what the build reports as it goes, from its worker thread: the stage,
// how far it is in it (done of total units, 0 of 0 when unknown) and a
// line to show (a module's name, a file, the error)
struct Progress {
    std::function<void(Stage stage, uint64_t done, uint64_t total, std::string_view detail)> report;
    // set by the caller to stop the build; it stops at the next unit (the
    // compilers under way are ended)
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

// the cached library for an executable's digest (16 hex digits, either
// case), if one was built for this XenoForge's interface version
std::optional<CachedGame> find_cached(const std::filesystem::path &cache_dir, std::string_view digest);

// builds the disc's game library into the cache; blocks, so it runs on a
// worker thread. false with the error when it fails or is cancelled.
bool prepare_game(const std::filesystem::path &disc, const std::filesystem::path &cache_dir, Progress &progress,
                  std::string &error);

// ---- extensions (the launcher may leave them alone) ----

struct BuildOptions {
    unsigned jobs = 4;               // compilers at once (more ran out of memory on 16 GB)
    std::filesystem::path toolchain; // where clang and lld are; empty: find_toolchain's search
    bool keep_work = false;          // keep the emitted code and objects (they are hundreds of MB)
    bool force = false;              // build even when the cache has the library
};
bool prepare_game(const std::filesystem::path &disc, const std::filesystem::path &cache_dir, Progress &progress,
                  std::string &error, const BuildOptions &options);

// The compile and link alone: `sources` (paths relative to src_root: what
// xfrecomp emit wrote, its modules.cpp among them) built into a game library
// at `library`, with the headers, objects and logs in `work`. What
// prepare_game does after the emission; for the tests and ports' tools.
bool build_library(const std::filesystem::path &src_root, const std::vector<std::string> &sources,
                   const std::filesystem::path &work, const std::filesystem::path &library, Progress &progress,
                   std::string &error, const BuildOptions &options = {});

// The directory holding clang and lld: `hint` (or its bin/), XF_TOOLCHAIN
// (or its bin/), toolchain/bin beside the executable (what the launcher
// ships), then an installed LLVM (%ProgramFiles%\LLVM\bin) and the PATH.
std::optional<std::filesystem::path> find_toolchain(const std::filesystem::path &hint = {});

// whether this CPU runs what the build makes (AVX2 and FMA, as the
// statically linked games are built); why not when it does not
bool cpu_supported(std::string &why);

} // namespace xf::build
