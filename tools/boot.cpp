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
// module's once the module is loaded; --interpret leaves it out.
// --render draws what the GPU is sent (the renderer as the command
// processor's Sink, on a Vulkan device without a window); off by default,
// so the GPU stays headless. --shots <dir> writes the front buffer of chosen
// swaps there as <title>_<swap>.png, the swaps numbered from 0 as Xenia's
// --trace_gpu_frames numbers a trace's frame (frame N: what the swap after
// the first N shows): --shot-swaps 300,301,890-910, or --shot-every N. The
// report adds the renderer's counts.
//
// XAM's input answers from an idle pad for user 0 (a controller plugged in,
// nothing pressed), so a game that waits for one goes on; --no-controller
// leaves every user disconnected. --press start@40 (repeatable; game.h)
// holds buttons at times on the scheduler's clock. What boot shares with the
// player (opening the game, the renderer, the launch, the report) is in
// game.cpp. --rates prints a line a second (the swaps, the scheduler's
// clock, the renderer's time), as the player's --rates does.
//
//   boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] [--content <dir>] [--interpret]
//        [--render [--shots <dir> [--shot-swaps <list>] [--shot-every N]]] [--no-controller] [--press <b@s>]...
//        [--rates]

#include "game.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace xf;

int main(int argc, char **argv) {
    const char *usage = "usage: boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] "
                        "[--content <dir>] [--interpret] [--render [--shots <dir> [--shot-swaps <list>] "
                        "[--shot-every N]]] [--no-controller] [--press <buttons@seconds[:hold]>]... [--rates]\n";
    if (argc < 2) return std::fprintf(stderr, "%s", usage), 2;
    game::Options o;
    o.input = argv[1];
    std::error_code ec;
    o.storage = std::filesystem::temp_directory_path(ec) / "xenoforge";
    uint32_t seconds = 30;
    bool rendering = false, controller = true, rates = false;
    std::vector<game::Press> presses;
    std::filesystem::path shots;
    std::set<uint64_t> shot_swaps;
    uint64_t shot_every = 0;
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
        else if (a == "--interpret")
            o.interpret = true;
        else if (a == "--render")
            rendering = true;
        else if (a == "--shots" && k + 1 < argc)
            shots = argv[++k];
        else if (a == "--shot-swaps" && k + 1 < argc) {
            if (!game::parse_numbers(argv[++k], shot_swaps))
                return std::fprintf(stderr, "bad --shot-swaps\n%s", usage), 2;
        } else if (a == "--shot-every" && k + 1 < argc)
            shot_every = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--no-controller")
            controller = false;
        else if (a == "--rates")
            rates = true;
        else if (a == "--press" && k + 1 < argc) {
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
        std::printf("rendering on %s%s%s\n", g.device->name().c_str(), shots.empty() ? "" : ", shots in ",
                    shots.string().c_str());
    }

    if (!game::launch(g, error)) return std::fprintf(stderr, "launch: %s\n", error.c_str()), 1;
    // until the main thread ends, the code hits what cannot run, or time is up
    uint32_t exit_code = 0;
    bool ended = false;
    // (on the host's clock: a wait may end early)
    game::Rates rate_log;
    const auto started = std::chrono::steady_clock::now();
    double next_rates = 1;
    for (double elapsed = 0; elapsed < seconds && !k.stopped;) {
        if ((ended = k.wait_main(100, &exit_code))) break;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (rates && elapsed >= next_rates) game::print_rates(g, rate_log, elapsed), next_rates += 1;
    }

    if (k.stopped)
        std::printf("stopped: %s\n", k.stop_reason.c_str());
    else if (ended)
        std::printf("the main thread ended (exit code 0x%08X)\n", exit_code);
    else
        std::printf("still running after %u s\n", seconds);
    std::printf("threads:\n%s", k.thread_report().c_str());
    game::print_report(g, true);
    // other threads may still run: no destructors under them
    std::fflush(stdout);
    std::_Exit(0);
}
