// xfbuild: the recompile step from the command line (plan D2; game_build.h),
// what the launcher runs on a worker thread: the disc's executables
// discovered and emitted, compiled by clang (4 at once unless --jobs says)
// and linked by lld into the game library, kept in the cache by the
// executable's digest and the interface version. Prints each stage as it
// goes and how long it took.
//
//   xfbuild <game.iso | default.xex> [cache dir] [--jobs N] [--toolchain <dir>] [--keep] [--force]
//           [--cancel-after S] [--quiet] [--entry <hex>]... [--no-entry-log] [--no-shaders] [--no-disc-shaders] [--shaders-only]
//           [--project <file.toml>]
//
// The cache is game_build.h's default_cache_dir() unless named. --keep keeps
// the work directory (the emitted code and the objects), --force builds even
// when the cache has the library, --cancel-after stops the build after S
// seconds as the launcher's cancel does, --quiet prints the stages without a
// line per file. --entry adds a function entry, --no-entry-log leaves the
// runtime's entry log (D3) out. At the end it prints the extra entries taken
// in and the functions left out, with why. Then the shader step
// (shader_step.h; also on a cached library): the shaders and pipelines runs
// of the game recorded, and the shaders found on the disc, made into the
// title's pipeline cache on a Vulkan device; --no-shaders leaves it out,
// --no-disc-shaders leaves out the disc's search, --shaders-only runs the shader
// step alone (no library built). --project emits with a port's project file
// (docs/recompiler.md, "The project file": hook slots, names, starts); the
// cached library is built again when it was made with another one or none.

#include "xenoforge/game_build.h"
#include "xenoforge/shader_step.h"

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
                             "[--keep] [--force] [--cancel-after S] [--quiet] [--entry <hex>]... "
                             "[--no-entry-log] [--no-shaders] [--no-disc-shaders] [--shaders-only] "
                             "[--project <file.toml>]\n");
        return 2;
    }
    std::filesystem::path disc = argv[1], cache = build::default_cache_dir();
    build::BuildOptions options;
    build::BuildReport result;
    options.report = &result;
    options.shader_step = &render::run_shader_step;
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
        else if (a == "--no-entry-log")
            options.use_entry_log = false;
        else if (a == "--no-shaders")
            options.shader_step = {};
        else if (a == "--shaders-only")
            options.shaders_only = true;
        else if (a == "--no-disc-shaders")
            options.disc_shaders = false;
        else if (a == "--project" && k + 1 < argc)
            options.project = argv[++k];
        else if (a == "--entry" && k + 1 < argc)
            options.entries.push_back(uint32_t(std::strtoul(argv[++k], nullptr, 16)));
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
    if (options.shaders_only) {
        for (const std::string &line : result.shaders) std::printf("shaders: %s\n", line.c_str());
        std::printf("shader step in %.1f s\n", total);
        return 0;
    }
    std::printf("library %s (%.1f MB)\nprepared in %.1f s\n", library.c_str(),
                double(std::filesystem::file_size(library, ec)) / (1 << 20), total);
    // what it took in (D3) and left out (D2)
    std::printf("extra entries %zu (%zu from the entry log)\nleft out %zu functions\n", result.entries,
                result.entries_taken, result.left_out.size());
    for (const build::LeftOutFunction &f : result.left_out)
        std::printf("  %s 0x%08X  %s\n", f.module.c_str(), f.address, f.reason.c_str());
    for (const std::string &line : result.shaders) std::printf("shaders: %s\n", line.c_str());
    return 0;
}
