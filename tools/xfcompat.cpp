// xfcompat: the batch compatibility run (plan D5, "The batch run"). For each
// disc in a folder (ISOs and folders with a default.xex, one level down; or
// one file), one at a time: the game library prepared in the cache
// (build::prepare_game, the cached one reused unless --force), then the
// generic boot run as a child process (--cache, --seconds N, --stall-report,
// --render) under a hard timeout, its output parsed, and one row appended to
// <out>.jsonl (what the run resumes from) and <out>.csv (what is read):
// title, digest, the build's time and library size, the generated code's
// size, the interpreter's entries, the swaps, the first unimplemented export
// and GPU gap counted, and how it ended (ok, stall, stopped, ended, crash
// with its guest stack, build-failed with the reason).
//
//   xfcompat <folder | disc> [--out <base>] [--cache <dir>] [--seconds N] [--boot <exe>] [--jobs N]
//            [--toolchain <dir>] [--force] [--redo] [--no-render] [--stall S] [--limit N]
//
// A disc with a row in <out>.jsonl is skipped (--redo starts the files
// again, --force also rebuilds the libraries). The children are made with
// SDL3's process calls, so the same code runs on every host.

#include "compat.h"

#include "library.h"
#include "xenoforge/game_build.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace xf;
using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point from) { return std::chrono::duration<double>(Clock::now() - from).count(); }

std::string utf8(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct Child {
    std::string output;
    int exit_code = -1;
    bool started = false, timed_out = false;
    std::string error;
};

// A program run with its output (stdout and stderr together) read to the
// end, killed once `timeout` seconds have passed.
Child run_child(const std::vector<std::string> &argv, double timeout) {
    Child child;
    std::vector<const char *> args;
    for (const std::string &a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) return child.error = SDL_GetError(), child;
    child.started = true;
    std::atomic<bool> done{false}, killed{false};
    std::thread watchdog([&] {
        const auto start = Clock::now();
        while (!done && since(start) < timeout) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done) killed = true, SDL_KillProcess(process, true);
    });
    size_t size = 0;
    int code = -1;
    void *out = SDL_ReadProcess(process, &size, &code);
    done = true;
    watchdog.join();
    if (out) child.output.assign(static_cast<char *>(out), size), SDL_free(out);
    child.exit_code = code;
    child.timed_out = killed;
    SDL_DestroyProcess(process);
    return child;
}

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
    const char *usage = "usage: xfcompat <folder | disc> [--out <base>] [--cache <dir>] [--seconds N] [--boot <exe>] "
                        "[--jobs N] [--toolchain <dir>] [--force] [--redo] [--no-render] [--stall S] [--limit N]\n";
    if (argc < 2) return std::fprintf(stderr, "%s", usage), 2;
    std::filesystem::path input = argv[1], out = "xfcompat", cache = build::default_cache_dir(), boot;
    build::BuildOptions options;
    unsigned seconds = 60, limit = 0;
    double stall = 10;
    bool redo = false, render = true;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--out" && k + 1 < argc)
            out = argv[++k];
        else if (a == "--cache" && k + 1 < argc)
            cache = argv[++k];
        else if (a == "--seconds" && k + 1 < argc)
            seconds = unsigned(std::max(1, std::atoi(argv[++k])));
        else if (a == "--boot" && k + 1 < argc)
            boot = argv[++k];
        else if (a == "--jobs" && k + 1 < argc)
            options.jobs = unsigned(std::max(1, std::atoi(argv[++k])));
        else if (a == "--toolchain" && k + 1 < argc)
            options.toolchain = argv[++k];
        else if (a == "--stall" && k + 1 < argc)
            stall = std::strtod(argv[++k], nullptr);
        else if (a == "--limit" && k + 1 < argc)
            limit = unsigned(std::atoi(argv[++k]));
        else if (a == "--force")
            options.force = true;
        else if (a == "--redo")
            redo = true;
        else if (a == "--no-render")
            render = false;
        else
            return std::fprintf(stderr, "xfcompat: %s?\n%s", a.c_str(), usage), 2;
    }
    if (boot.empty()) {
        const char *base = SDL_GetBasePath();
#ifdef _WIN32
        boot = std::filesystem::path(base ? base : ".") / "boot.exe";
#else
        boot = std::filesystem::path(base ? base : ".") / "boot";
#endif
    }
    std::error_code ec;
    if (!std::filesystem::exists(boot, ec)) return std::fprintf(stderr, "xfcompat: no boot at %s\n", utf8(boot).c_str()), 2;

    std::vector<std::filesystem::path> discs;
    if (std::filesystem::is_directory(input, ec))
        discs = frontend::find_games(input);
    else if (std::filesystem::exists(input, ec))
        discs.push_back(input);
    if (discs.empty()) return std::fprintf(stderr, "xfcompat: no discs in %s\n", utf8(input).c_str()), 2;

    const compat::Files files = compat::files_for(out);
    if (redo) compat::clear(files);
    const std::set<std::string> done = compat::done_discs(files);
    std::filesystem::create_directories(cache, ec);
    std::printf("%zu discs in %s, %zu done already; rows into %s and %s\n", discs.size(), utf8(input).c_str(),
                done.size(), utf8(files.jsonl).c_str(), utf8(files.csv).c_str());

    size_t index = 0, ran = 0, failed = 0;
    for (const std::filesystem::path &disc : discs) {
        index++;
        const std::string key = utf8(disc);
        if (done.count(key)) {
            std::printf("[%zu/%zu] %s: has a row, skipped\n", index, discs.size(), key.c_str());
            continue;
        }
        if (limit && ran >= limit) break;
        ran++;
        compat::Row row;
        row.disc = key;
        frontend::GameEntry game = frontend::read_game(disc);
        row.title = game.title.empty() ? utf8(disc.stem()) : game.title;
        row.title_id = game.title_id_text();
        char digest[24];
        std::snprintf(digest, sizeof(digest), "%016llx", (unsigned long long)game.digest);
        row.digest = digest;
        std::printf("[%zu/%zu] %s (%s, %s)\n", index, discs.size(), row.title.c_str(), row.title_id.c_str(), digest);
        std::fflush(stdout);
        if (!game.error.empty()) {
            row.state = "build-failed", row.detail = "cannot read the disc: " + game.error;
            compat::append_row(files, row), failed++;
            std::printf("  %s\n", row.detail.c_str());
            continue;
        }

        // the library: the cached one, else built (a build that fails is a row, never the run's end)
        std::filesystem::path library;
        if (!options.force)
            if (auto cached = build::find_cached(cache, digest)) library = cached->library, row.cached = true;
        if (library.empty() || options.force) {
            build::Progress progress;
            build::Stage stage = build::Stage::Reading;
            bool first = true;
            Clock::time_point stage_start = Clock::now();
            progress.report = [&](build::Stage s, uint64_t n, uint64_t total, std::string_view detail) {
                if (first || s != stage) {
                    if (!first) std::printf("  %-11s %7.1f s\n", stage_name(stage), since(stage_start));
                    stage = s, stage_start = Clock::now(), first = false;
                }
                if (s == build::Stage::Done) library = std::string(detail);
                if (s == build::Stage::Failed) std::printf("  failed: %.*s\n", int(detail.size()), detail.data());
                (void)n, (void)total;
            };
            const auto started = Clock::now();
            std::string error;
            const bool ok = build::prepare_game(disc, cache, progress, error, options);
            row.build_seconds = since(started);
            if (!ok) {
                row.state = "build-failed", row.detail = error;
                compat::append_row(files, row), failed++;
                std::printf("  build failed after %.0f s: %s\n", row.build_seconds, error.c_str());
                continue;
            }
            if (library.empty())
                if (auto cached = build::find_cached(cache, digest)) library = cached->library;
            std::printf("  built in %.0f s\n", row.build_seconds);
        } else {
            std::printf("  library cached\n");
        }
        row.library_bytes = std::filesystem::exists(library, ec) ? std::filesystem::file_size(library, ec) : 0;
        // what the build left out (D2), from the report beside the library
        if (auto report = build::read_build_report(cache, digest)) {
            row.left_out = int64_t(report->left_out.size());
            for (size_t i = 0; i < report->left_out.size() && i < 3; i++) {
                const build::LeftOutFunction &f = report->left_out[i];
                char address[16];
                std::snprintf(address, sizeof(address), "0x%08X", f.address);
                row.left_out_why += (i ? "; " : "") + f.module + " " + address + ": " + f.reason;
            }
            if (report->left_out.size()) std::printf("  %zu functions left out\n", report->left_out.size());
        }

        // the boot: a fresh storage root, the hard timeout the run plus the time to start and to end
        const std::filesystem::path content = cache / "compat-content" / row.digest;
        std::filesystem::remove_all(content, ec);
        std::vector<std::string> command{utf8(boot), key, "--cache", utf8(cache), "--seconds", std::to_string(seconds),
                                         "--stall-report", std::to_string(stall), "--content", utf8(content)};
        if (render) command.push_back("--render");
        const auto boot_start = Clock::now();
        Child child = run_child(command, double(seconds) + 90);
        const double boot_seconds = since(boot_start);
        std::filesystem::remove_all(content, ec);
        if (!child.started) {
            row.state = "crash", row.detail = "cannot start boot: " + child.error;
        } else {
            compat::BootResult result = compat::parse_boot_output(child.output, child.exit_code, child.timed_out);
            compat::Row parsed = result.row;
            // what the build step knows stays; what boot saw is taken over
            parsed.disc = row.disc, parsed.title = row.title, parsed.cached = row.cached;
            parsed.build_seconds = row.build_seconds, parsed.library_bytes = row.library_bytes;
            if (parsed.title_id.empty()) parsed.title_id = row.title_id;
            if (parsed.digest.empty()) parsed.digest = row.digest;
            row = parsed;
        }
        row.boot_seconds = boot_seconds;
        row.exit_code = child.exit_code;
        if (row.state != "ok") failed++;
        compat::append_row(files, row);
        const std::string why = row.detail.empty() ? "" : " (" + row.detail + ")";
        std::printf("  %s%s: %llu swaps, %llu interpreted entries, %llu functions%s%s\n", row.state.c_str(),
                    why.c_str(), (unsigned long long)row.swaps, (unsigned long long)row.interpreted_entries,
                    (unsigned long long)row.functions, row.first_unimplemented.empty() ? "" : ", first unimplemented ",
                    row.first_unimplemented.c_str());
        std::fflush(stdout);
    }
    std::printf("done: %zu run, %zu not ok\n", ran, failed);
    return 0;
}
