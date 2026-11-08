// boot: a game run without a window (plan step A4; docs/runtime.md). Loads
// the disc's executable, maps it, binds its imports, starts the main thread
// the kernel makes (Kernel::launch: stack, PCR, KTHREAD, TLS, as Xenia's
// XThread), runs the entry point (generated code where linked, else the
// interpreter) and prints what the kernel saw: the modules loaded (the
// executable, the DLLs it loaded or unloaded), every export called and how
// often, the ones with no implementation, the threads and where it stopped,
// and what the GPU did (packets, draws, resolves, swaps, interrupts).
// The disc is the game's file system (game:, d:), or a bare executable's
// directory is; --content names the writable storage root (Xenia's layout:
// content/ for saves, cache*/), a temporary directory's xenoforge by default.
//
// Built as boot_<game> (CMake's XF_GAMES), it links that game's generated
// code (xfrecomp emit: a table per executable and DLL) and dispatch runs a
// module's once the module is loaded; --interpret leaves it out. Plain boot
// loads the game library the recompile step cached for the executable
// (xfbuild, game_build.h): from --cache <dir> (the launcher's cache by
// default), or the one --game-library names; with none it interprets.
// --render draws what the GPU is sent (the renderer as the command
// processor's Sink, on a Vulkan device without a window); off by default,
// so the GPU stays headless. --shots <dir> writes the front buffer of chosen
// swaps there as <title>_<swap>.png, the swaps numbered from 0 as Xenia's
// --trace_gpu_frames numbers a trace's frame (frame N: what the swap after
// the first N shows): --shot-swaps 300,301,890-910, or --shot-every N. The
// report adds the renderer's counts. --trace-frames 300,301 records those
// frames as Xenia's GPU traces (gpu_trace_writer.h), numbered as the shots,
// into --trace-dir (./traces by default); it renders.
//
// XAM's input answers from an idle pad for user 0 (a controller plugged in,
// nothing pressed), so a game that waits for one goes on; --no-controller
// leaves every user disconnected. --press start@40 (repeatable; game.h)
// holds buttons at times on the scheduler's clock. What boot shares with the
// player (opening the game, the renderer, the launch, the report) is in
// game.cpp. --rates prints a line a second (the swaps, the scheduler's
// clock, the renderer's time), as the player's --rates does. --stall-report S
// prints the threads (what each waits for, its guest stack by the back chain)
// when no swap has come for S seconds (5 S before the first), once a stall.
// --deterministic runs every guest thread as a fiber on one host thread in a
// fixed order on a virtual clock (docs/runtime.md, "The deterministic mode";
// --paced keeps that clock behind the host's): the same disc and input give
// the same run. --until-swap N or --until S (seconds of its clock) halts it
// there, every thread parked, and the report adds a hash of guest memory.
// --record-input <file> writes what XAM's input answered, --replay-input
// <file> answers from such a recording (docs/input.md, "Recordings").
// The last line is "xf-summary {json}" (game.h, print_summary), what xfcompat
// reads: title, digest, state (ok, stall, stopped, ended, crash), generated
// code, interpreter entries, swaps, the exports and GPU gaps counted.
// --lockstep 82001234,... and --lockstep-sample N have the interpreter check
// the generated code of the calls to those functions and of one call in N
// (xenoforge/lockstep.h; XF_LOCKSTEP and XF_LOCKSTEP_SAMPLE in the player):
// mismatches are printed as found, the counts in the report.
// --hook-stats counts the calls of each of the port's hooks (before, after,
// replace, mid) and the report lists them. A port whose hooks are a module
// of their own (PortInfo::hook_module) loads it before the game runs and
// loads it again when the file changes, or every S seconds with
// --reload-hooks-every S (docs/sdk.md, "Reloading the hooks").
//
//   boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] [--content <dir>] [--interpret]
//        [--cache <dir>] [--game-library <file>]
//        [--render [--shots <dir> [--shot-swaps <list>] [--shot-every N]]] [--trace-frames <list> [--trace-dir <dir>]]
//        [--no-controller] [--press <b@s>]... [--rates] [--stall-report S] [--crash-dir <dir>]
//        [--deterministic [--paced] [--until-swap N] [--until S]] [--record-input <file>] [--replay-input <file>]
//        [--hook-stats] [--reload-hooks-every S]

#include "game.h"
#include "xenoforge/hooks.h"
#include "xenoforge/lockstep.h"
#include "xenoforge/run.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace xf;

namespace {
// A path the guest launches, as the path on the disc --xex takes: the device
// prefix (game:, d:, \Device\Cdrom0) dropped, slashes forward; "" for the
// dashboard or a path that is not on the disc
std::string disc_path(std::string path) {
    std::string lower = path;
    for (char &ch : lower) ch = char(std::tolower((unsigned char)ch));
    for (const char *prefix : {"game:", "d:", "\\device\\cdrom0", "\\device\\harddisk0\\cdrom0"}) {
        size_t n = std::strlen(prefix);
        if (lower.compare(0, n, prefix) != 0) continue;
        path = path.substr(n);
        while (!path.empty() && (path[0] == '\\' || path[0] == '/')) path.erase(0, 1);
        for (char &ch : path)
            if (ch == '\\') ch = '/';
        return path;
    }
    return {};
}
} // namespace

int xf::run_boot(int argc, char **argv, PortInfo &port) {
    const char *usage = "usage: boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] "
                        "[--content <dir>] [--interpret] [--cache <dir>] [--game-library <file>] "
                        "[--render [--shots <dir> [--shot-swaps <list>] "
                        "[--shot-every N]]] [--trace-frames <list> [--trace-dir <dir>]] [--no-controller] "
                        "[--press <buttons@seconds[:hold]>]... [--rates] [--stall-report <seconds>] "
                        "[--crash-dir <dir>] [--lockstep <address,...>] [--lockstep-sample N] "
                        "[--deterministic [--paced] [--until-swap N] [--until S]] [--record-input <file>] "
                        "[--replay-input <file>] [--hook-stats] [--reload-hooks-every S] "
                        "[--language N] [--country N] [--resolution WxH] [--widescreen 0|1] [--refresh 50|60] "
                        "[--follow-launches] [--launch-data <file>] [--option <key>=<value>]... [--directory-saves]\n";
    if (argc < 2) return std::fprintf(stderr, "%s", usage), 2;
    game::Options o;
    o.input = argv[1];
    o.modules = port.modules, o.module_count = port.module_count;
    std::error_code ec;
    o.storage = std::filesystem::temp_directory_path(ec) / "xenoforge";
    uint32_t seconds = 30;
    double stall_report = 0;
    bool rendering = false, controller = true, rates = false;
    std::vector<game::Press> presses;
    std::filesystem::path shots;
    std::set<uint64_t> shot_swaps, trace_frames;
    uint64_t shot_every = 0;
    std::filesystem::path trace_dir = "traces";
    double reload_every = 0;
    bool follow_launches = false;
    std::filesystem::path launch_data_file;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc)
            o.xex_path = argv[++k];
        else if (a == "--steps" && k + 1 < argc)
            o.steps = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--seconds" && k + 1 < argc)
            seconds = uint32_t(std::strtoul(argv[++k], nullptr, 10));
        else if (a == "--content" && k + 1 < argc)
            o.storage = argv[++k];
        else if (a == "--directory-saves")
            o.content_format = kernel::ContentFormat::Directory;
        else if (a == "--interpret")
            o.interpret = true;
        else if (a == "--cache" && k + 1 < argc)
            o.cache = argv[++k];
        else if (a == "--game-library" && k + 1 < argc)
            o.game_library = argv[++k];
        else if (a == "--render")
            rendering = true;
        else if (a == "--shots" && k + 1 < argc)
            shots = argv[++k];
        else if (a == "--shot-swaps" && k + 1 < argc) {
            if (!game::parse_numbers(argv[++k], shot_swaps))
                return std::fprintf(stderr, "bad --shot-swaps\n%s", usage), 2;
        } else if (a == "--shot-every" && k + 1 < argc)
            shot_every = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--trace-frames" && k + 1 < argc) {
            if (!game::parse_numbers(argv[++k], trace_frames))
                return std::fprintf(stderr, "bad --trace-frames\n%s", usage), 2;
            rendering = true;
        } else if (a == "--trace-dir" && k + 1 < argc)
            trace_dir = argv[++k];
        else if (a == "--no-controller")
            controller = false;
        else if (a == "--rates")
            rates = true;
        else if (a == "--crash-dir" && k + 1 < argc)
            game::set_crash_directory(argv[++k]);
        else if (a == "--stall-report" && k + 1 < argc)
            stall_report = std::strtod(argv[++k], nullptr);
        else if (a == "--lockstep" && k + 1 < argc) {
            if (!kernel::parse_lockstep_functions(argv[++k], o.lockstep))
                return std::fprintf(stderr, "bad --lockstep\n%s", usage), 2;
        } else if (a == "--lockstep-sample" && k + 1 < argc)
            o.lockstep_sample = uint32_t(std::strtoul(argv[++k], nullptr, 10));
        else if (a == "--deterministic")
            o.deterministic = std::max(o.deterministic, 1);
        else if (a == "--paced")
            o.deterministic = 2;
        else if (a == "--until-swap" && k + 1 < argc)
            o.halt_swap = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--until" && k + 1 < argc)
            o.halt_seconds = std::strtod(argv[++k], nullptr);
        else if (a == "--record-input" && k + 1 < argc)
            o.record_input = argv[++k];
        else if (a == "--replay-input" && k + 1 < argc)
            o.replay_input = argv[++k];
        else if (a == "--language" && k + 1 < argc)
            o.system.language = uint32_t(std::strtoul(argv[++k], nullptr, 10));
        else if (a == "--country" && k + 1 < argc)
            o.system.country = int(std::strtol(argv[++k], nullptr, 10));
        else if (a == "--resolution" && k + 1 < argc) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++k], "%ux%u", &w, &h) != 2)
                return std::fprintf(stderr, "bad --resolution\n%s", usage), 2;
            o.system.width = w, o.system.height = h;
        } else if (a == "--widescreen" && k + 1 < argc)
            o.system.widescreen = std::atoi(argv[++k]) ? 1 : 0;
        else if (a == "--refresh" && k + 1 < argc)
            o.system.refresh = std::atoi(argv[++k]);
        else if (a == "--follow-launches")
            follow_launches = true;
        else if (a == "--launch-data" && k + 1 < argc)
            launch_data_file = argv[++k];
        else if (a == "--hook-stats")
            hooks::set_stats(true);
        else if (a == "--reload-hooks-every" && k + 1 < argc)
            reload_every = std::strtod(argv[++k], nullptr);
        else if (a == "--option" && k + 1 < argc) {
            std::string why;
            if (!port.set_option(argv[++k], why)) return std::fprintf(stderr, "%s\n", why.c_str()), 2;
        } else if (a == "--press" && k + 1 < argc) {
            std::string why;
            if (!game::parse_press(argv[++k], presses.emplace_back(), why))
                return std::fprintf(stderr, "%s\n", why.c_str()), 2;
        } else
            std::fprintf(stderr, "boot: %s ignored\n", a.c_str());
    }
    if (!shots.empty() && !rendering) return std::fprintf(stderr, "--shots needs --render\n"), 2;

    std::string error;
    game::Game g;
    if (!game::open(o, g, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    kernel::Kernel &k = *g.kernel;
    auto swaps = [&k] { return kernel::gpu_command_processor(k).counts().swaps.load(); };
    if (controller)
        kernel::add_input_driver(k, input::make_idle_pad(game::presses_overlay(presses, swaps)));
    else if (!presses.empty())
        std::fprintf(stderr, "boot: --press without a controller does nothing\n");

    // the renderer, as the command processor's Sink before its thread runs
    if (rendering) {
        render::Options options;
        if (!shots.empty()) {
            std::filesystem::create_directories(shots, ec);
            stbi_write_png_compression_level = 1; // fast: the shots are written on the GPU's thread
            char title[16];
            std::snprintf(title, sizeof(title), "%08X", g.xex.execution.title_id);
            options.frame_wanted = [&shot_swaps, shot_every](uint64_t swap) {
                return shot_swaps.count(swap) || (shot_every && swap % shot_every == 0);
            };
            options.on_frame = [dir = shots, prefix = std::string(title)](uint64_t swap, const render::Image &frame) {
                std::filesystem::path path = dir / (prefix + "_" + std::to_string(swap) + ".png");
                if (!stbi_write_png(path.string().c_str(), int(frame.width), int(frame.height), 4, frame.rgba.data(),
                                    int(frame.width) * 4))
                    std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            };
        }
        if (!game::start_renderer(g, options, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
        if (!trace_frames.empty() && !game::start_trace(g, trace_frames, trace_dir, error))
            return std::fprintf(stderr, "%s\n", error.c_str()), 1;
        std::printf("rendering on %s%s%s\n", g.device->name().c_str(), shots.empty() ? "" : ", shots in ",
                    shots.string().c_str());
    }

    if (!launch_data_file.empty()) {
        std::ifstream in(launch_data_file, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        kernel::set_launch_data(k, data);
    }
    if (port.on_init) port.on_init(k);
    if (!port.hook_module.empty() && !game::load_port_module(g, port.hook_module, error))
        return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    if (!game::launch(g, error)) return std::fprintf(stderr, "launch: %s\n", error.c_str()), 1;
    // until the main thread ends, the code hits what cannot run, or time is up
    uint32_t exit_code = 0;
    bool ended = false;
    // (on the host's clock: a wait may end early)
    game::Rates rate_log;
    const auto started = std::chrono::steady_clock::now();
    double next_rates = 1;
    // a stall: no swap for --stall-report seconds (five times that before the
    // first); the threads (what each waits for, its guest stack) are printed
    // once a stall
    uint64_t last_swaps = 0;
    double last_swap_at = 0;
    bool stall_reported = false;
    bool halted = false;
    double next_reload = reload_every;
    for (double elapsed = 0; elapsed < seconds && !k.stopped;) {
        if ((ended = k.wait_main(100, &exit_code))) break;
        if ((halted = k.halted())) break;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // the port's hooks: loaded again when the module's file changed, or on the clock asked
        const bool due = reload_every > 0 && elapsed >= next_reload;
        if (due) next_reload += reload_every;
        game::poll_port_module(g, due);
        if (rates && elapsed >= next_rates) game::print_rates(g, rate_log, elapsed), next_rates += 1;
        if (stall_report > 0) {
            uint64_t now_swaps = swaps();
            if (now_swaps != last_swaps) {
                last_swaps = now_swaps, last_swap_at = elapsed, stall_reported = false;
            } else if (!stall_reported && elapsed - last_swap_at >= (now_swaps ? stall_report : 5 * stall_report)) {
                stall_reported = true;
                std::printf("stall: no swap for %.1f s after swap %llu (%.1f s); threads:\n%s", elapsed - last_swap_at,
                            (unsigned long long)now_swaps, elapsed, k.thread_report().c_str());
                std::fflush(stdout);
            }
        }
    }

    if (k.stopped)
        std::printf("stopped: %s\n", k.stop_reason.c_str());
    else if (ended)
        std::printf("the main thread ended (exit code 0x%08X)\n", exit_code);
    else if (halted)
        std::printf("halted (the deterministic mode's --until-swap or --until)\n");
    else
        std::printf("still running after %u s\n", seconds);
    // a stop's report (the guest stack, the last kernel calls), written to a file
    std::string stop_file = k.stopped ? game::write_stop_report(g) : std::string();
    std::printf("threads:\n%s", k.thread_report().c_str());
    game::print_report(g, true);
    if (k.deterministic()) std::printf("guest memory hash %016llx\n", (unsigned long long)k.memory_hash());
    // the machine-readable end (xfcompat reads it): a stall still under way at the end is the state
    {
        char text[64];
        std::snprintf(text, sizeof(text), "exit code 0x%08X", exit_code);
        if (k.stopped)
            game::print_summary(g, "stopped", k.stop_reason, k.stop_stack, stop_file);
        else if (ended)
            game::print_summary(g, "ended", text);
        else
            game::print_summary(g, stall_reported ? "stall" : "ok", halted ? "halted" : "");
    }
    if (port.on_shutdown) port.on_shutdown(k);
    std::fflush(stdout);
    if (k.stopped && k.title_exit.launch) {
        std::string next = disc_path(k.title_exit.path);
        std::printf("the title asks for %s", next.empty() ? "the dashboard" : next.c_str());
        if (!follow_launches || next.empty()) {
            std::printf("%s\n", next.empty() ? "" : " (--follow-launches starts it)");
        } else {
            // a title is a process: the next one starts as boot again on the same disc, with what
            // the title left it (the launch data) and the time that remains
            std::printf("; starting it\n");
            std::fflush(stdout);
            std::vector<std::string> args;
            for (int i = 0; i < argc; i++) {
                std::string a = argv[i];
                if ((a == "--xex" || a == "--seconds" || a == "--launch-data") && i + 1 < argc) i++;
                else args.push_back(a);
            }
            double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            args.insert(args.end(),
                        {"--xex", next, "--seconds", std::to_string(uint32_t(std::max(1.0, seconds - used)))});
            if (!k.title_exit.launch_data.empty()) {
                std::filesystem::path file = o.storage / "launch-data.bin";
                std::filesystem::create_directories(o.storage, ec);
                std::ofstream out(file, std::ios::binary);
                out.write(reinterpret_cast<const char *>(k.title_exit.launch_data.data()),
                          std::streamsize(k.title_exit.launch_data.size()));
                args.insert(args.end(), {"--launch-data", file.string()});
            }
            std::vector<char *> v;
#ifdef _WIN32
            // _spawnv does not quote: an argument with a space (a disc's path) would split
            for (std::string &s : args)
                if (s.find(' ') != std::string::npos) s = "\"" + s + "\"";
#endif
            for (std::string &s : args) v.push_back(s.data());
            v.push_back(nullptr);
#ifdef _WIN32
            int code = int(_spawnv(_P_WAIT, argv[0], v.data()));
            std::fflush(stdout);
            std::_Exit(code < 0 ? 1 : code);
#else
            execv(argv[0], v.data());
            std::_Exit(1);
#endif
        }
    }
    // other threads may still run: no destructors under them
    std::fflush(stdout);
    std::_Exit(0);
}
