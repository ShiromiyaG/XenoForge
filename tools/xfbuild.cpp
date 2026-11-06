// xfbuild: the recompile step from the command line (plan D2; game_build.h),
// what the launcher runs on a worker thread: the disc's executables
// discovered and emitted, compiled by clang (4 at once unless --jobs says)
// and linked by lld into the game library, kept in the cache by the
// executable's digest and the interface version. Prints each stage as it
// goes and how long it took.
//
//   xfbuild <game.iso | default.xex> [cache dir] [--jobs N] [--toolchain <dir>] [--keep] [--force]
//           [--cancel-after S] [--quiet]
//
// The cache is game_build.h's default_cache_dir() unless named. --keep keeps
// the work directory (the emitted code and the objects), --force builds even
// when the cache has the library, --cancel-after stops the build after S
// seconds as the launcher's cancel does, --quiet prints the stages without a
// line per file.

#include "xenoforge/game_build.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace xf;
using Clock = std::chrono::steady_clock;

namespace {

const char *stage_name(build::Stage s) {
    switch (s) {
    case build::Stage::Reading: return "reading";
    case build::Stage::Discovering: return "discovering";
    case build::Stage::Emitting: return "emitting";
    case build::Stage::Compiling: return "compiling";
    case build::Stage::Linking: return "linking";
    case build::Stage::Done: return "done";
    case build::Stage::Failed: return "failed";
    }
    return "?";
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: xfbuild <game.iso | default.xex> [cache dir] [--jobs N] [--toolchain <dir>] "
                             "[--keep] [--force] [--cancel-after S] [--quiet]\n");
        return 2;
    }
    std::filesystem::path disc = argv[1], cache = build::default_cache_dir();
    build::BuildOptions options;
    double cancel_after = 0;
    bool quiet = false;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--jobs" && k + 1 < argc)
            options.jobs = unsigned(std::max(1, std::atoi(argv[++k])));
        else if (a == "--toolchain" && k + 1 < argc)
            options.toolchain = argv[++k];
        else if (a == "--keep")
            options.keep_work = true;
        else if (a == "--force")
            options.force = true;
        else if (a == "--cancel-after" && k + 1 < argc)
            cancel_after = std::strtod(argv[++k], nullptr);
        else if (a == "--quiet")
            quiet = true;
        else if (!a.starts_with("--") && k == 2)
            cache = a;
        else
            return std::fprintf(stderr, "xfbuild: %s?\n", a.c_str()), 2;
    }
    auto toolchain = build::find_toolchain(options.toolchain);
    std::printf("disc %s\ncache %s\ntoolchain %s, %u jobs\n", disc.string().c_str(), cache.string().c_str(),
                toolchain ? toolchain->string().c_str() : "(none found)", options.jobs);

    std::atomic<bool> cancel{false};
    build::Progress progress;
    progress.cancel = &cancel;
    const Clock::time_point start = Clock::now();
    Clock::time_point stage_start = start;
    build::Stage stage = build::Stage::Reading;
    bool first = true;
    std::string library;
    auto seconds = [](Clock::time_point from) { return std::chrono::duration<double>(Clock::now() - from).count(); };
    progress.report = [&](build::Stage s, uint64_t done, uint64_t total, std::string_view detail) {
        if (first || s != stage) {
            if (!first) std::printf("  %-11s %7.1f s\n", stage_name(stage), seconds(stage_start));
            stage = s, stage_start = Clock::now(), first = false;
        }
        if (s == build::Stage::Done) library = detail;
        if (s == build::Stage::Failed || s == build::Stage::Done) return;
        if (!quiet || done == total || done == 0)
            std::printf("    %-11s %llu/%llu %.*s\n", stage_name(s), (unsigned long long)done,
                        (unsigned long long)total, int(detail.size()), detail.data());
        std::fflush(stdout);
    };
    std::thread canceller;
    if (cancel_after > 0)
        canceller = std::thread([&] {
            while (seconds(start) < cancel_after) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cancel = true;
        });
    std::string error;
    bool ok = build::prepare_game(disc, cache, progress, error, options);
    const double total = seconds(start);
    if (canceller.joinable()) {
        cancel = true;
        canceller.join();
    }
    if (!ok) {
        std::printf("failed after %.1f s: %s\n", total, error.c_str());
        return 1;
    }
    std::error_code ec;
    std::printf("library %s (%.1f MB)\nprepared in %.1f s\n", library.c_str(),
                double(std::filesystem::file_size(library, ec)) / (1 << 20), total);
    return 0;
}
