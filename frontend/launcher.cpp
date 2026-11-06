// launcher: the games in a folder, their settings and controls, and a start
// button (plan track C2; docs/runtime.md, "The launcher"). This file is the
// program around its view: the SDL3 window, the Vulkan device and RmlUi's
// render interface on it (ui/), the context the screens live in
// (launcher_view.cpp) and the loop. What the launcher does is LauncherCore's
// (launcher_core.h), which has no UI code: the games are the folder's ISOs
// (and bare default.xex files), read in place for their title, title ID,
// picture and digest (library.h); a game with a library cached for its disc
// starts in the generic player, which loads it; without one, in the player
// beside the launcher that carries its code (player_<game>, asked with
// --identify), else the card offers Prepare (the recompile step) and, if
// that fails, the generic player interprets; the settings are the player's
// config.toml (settings.h), the shared ones or a game's own, which the player
// then reads for it; the player runs as a process of its own with its output
// in a log beside the settings (logs/<title ID>.log), and the launcher stays
// and shows how the run ended.
//
// For checks without a person at the screen: --list prints the folder's
// games and the player each would start in, without a window; --quit-after S
// ends the launcher after S seconds (once a game it started has ended) and
// --screenshot <png> saves its last picture; --select N selects the Nth game,
// --screen <library|settings|controls|about> opens a page (--controls is
// --screen controls) and --start N starts the Nth game as the button does,
// --player-arg <arg> (repeatable) adds an argument to the player's command
// line (--quit-after 20, say), --build-jobs N the compilers a Prepare runs at
// once (4 unless told; each takes a gigabyte or two). For the pictures of the screens: --demo-progress
// shows a fake build under way (--demo-fail: one that fails, --demo-speed X:
// times real time, --show-log: its log open), --snap-at S ends at S seconds whatever the view is waiting for, --capture-binding <name> a
// binding waiting for its key, --render-size WxH draws at that size without
// the window (a 4K picture on a 1080p desktop), --scale F the interface's
// size, --assets <dir> the screens' files (to edit them without a rebuild),
// and --keys up,down,a,b,... presses of the pad (the d-pad, a b x y, lb rb,
// start; "wait" waits) sent to the view a few tenths of a second apart once
// the screen has settled, to check navigation without a person at the pad.
//
//   launcher [--config <file>] [--games <folder>] [--list] [--select N] [--screen <page>] [--start N] [--keys <list>]
//            [--player-arg <arg>]... [--build-jobs N] [--quit-after S] [--screenshot <png>] [--demo-progress] [--demo-fail]
//            [--demo-speed X] [--show-log] [--capture-binding <name>] [--render-size WxH] [--scale F]
//            [--assets <dir>]

#include "launcher_core.h"
#include "xenoforge/game_build.h"
#include "launcher_view.h"
#include "ui/ui_system.h"
#include "ui/ui_window.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace xf;
using frontend::from_utf8;
using frontend::utf8;
using Clock = std::chrono::steady_clock;

namespace {

bool write_png(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height, const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(utf8(path).c_str(), int(width), int(height), 4, rgba.data(), int(width) * 4) != 0;
}

int fail(const std::string &message) {
    std::fprintf(stderr, "launcher: %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    // a window program: its output goes to the console it was started from,
    // when there is one and nothing redirects it
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::filesystem::path config_file, screenshot, assets;
    std::string games_arg;
    bool list = false, games_given = false;
    double quit_after = 0;
    float scale = 0, snap_at = 0;
    std::vector<std::string> key_script;
    uint32_t render_w = 0, render_h = 0;
    frontend::ViewOptions options;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc)
            config_file = from_utf8(argv[++i]);
        else if (a == "--games" && i + 1 < argc)
            games_arg = argv[++i], games_given = true;
        else if (a == "--list")
            list = true;
        else if (a == "--controls")
            options.screen = frontend::Screen::Controls;
        else if (a == "--screen" && i + 1 < argc) {
            const std::string name = argv[++i];
            options.screen = name == "settings"   ? frontend::Screen::Settings
                             : name == "controls" ? frontend::Screen::Controls
                             : name == "about"    ? frontend::Screen::About
                                                  : frontend::Screen::Library;
        } else if (a == "--select" && i + 1 < argc)
            options.select = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--start" && i + 1 < argc)
            options.start_index = options.select = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--build-jobs" && i + 1 < argc)
            options.build_jobs = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--player-arg" && i + 1 < argc)
            options.player_args.push_back(argv[++i]);
        else if (a == "--quit-after" && i + 1 < argc)
            quit_after = std::strtod(argv[++i], nullptr);
        else if (a == "--screenshot" && i + 1 < argc)
            screenshot = from_utf8(argv[++i]);
        else if (a == "--demo-progress")
            options.demo_progress = true;
        else if (a == "--demo-fail")
            options.demo_progress = options.demo_fail = true;
        else if (a == "--demo-speed" && i + 1 < argc)
            options.demo_speed = std::max(0.1, std::strtod(argv[++i], nullptr));
        else if (a == "--show-log")
            options.show_log = true;
        else if (a == "--capture-binding" && i + 1 < argc)
            options.capture = argv[++i], options.screen = frontend::Screen::Controls;
        else if (a == "--render-size" && i + 1 < argc)
            std::sscanf(argv[++i], "%ux%u", &render_w, &render_h);
        else if (a == "--snap-at" && i + 1 < argc)
            snap_at = std::strtof(argv[++i], nullptr);
        else if (a == "--scale" && i + 1 < argc)
            scale = std::strtof(argv[++i], nullptr);
        else if (a == "--assets" && i + 1 < argc)
            assets = from_utf8(argv[++i]);
        else if (a == "--keys" && i + 1 < argc) {
            const std::string list = argv[++i];
            for (size_t at = 0; at < list.size();) {
                const size_t end = list.find(',', at);
                key_script.push_back(list.substr(at, end == std::string::npos ? std::string::npos : end - at));
                at = end == std::string::npos ? list.size() : end + 1;
            }
        }
        else
            std::fprintf(stderr, "launcher: %s ignored\n", a.c_str());
    }
    if (config_file.empty()) config_file = frontend::default_path();
    std::filesystem::path players_dir;
    if (const char *base = SDL_GetBasePath()) players_dir = from_utf8(base);
    if (assets.empty()) assets = players_dir / "ui";

    frontend::LauncherCore core(config_file, players_dir);
    if (!core.load_error().empty())
        std::fprintf(stderr, "launcher: %s: %s (the defaults instead)\n", utf8(config_file).c_str(), core.load_error().c_str());
    std::string message;
    if (games_given)
        core.set_games_folder(games_arg, false, message); // for this run only
    else
        core.rescan();

    // --list: the games and their players, no window
    if (list) {
        while (core.listing().busy) SDL_Delay(10);
        const frontend::LauncherCore::Listing l = core.listing();
        for (const auto &p : l.players)
            std::printf("player %s: %zu modules%s\n", utf8(p.path.filename()).c_str(), p.modules.size(),
                        p.generic ? " (generic, interprets)" : "");
        std::printf("games in %s: %zu\n", core.games_folder().c_str(), l.games.size());
        for (const frontend::GameEntry &g : l.games) {
            if (!g.error.empty()) {
                std::printf("  %s: %s\n", utf8(g.path).c_str(), g.error.c_str());
                continue;
            }
            char digest[24];
            std::snprintf(digest, sizeof(digest), "%016llx", (unsigned long long)g.digest);
            const bool cached = build::find_cached(build::default_cache_dir(), digest).has_value();
            const frontend::PlayerBinary *p = frontend::player_for(l.players, g, cached);
            std::printf("  %s  %s  v%08X  digest %016llx  %s -> %s\n", g.title_id_text().c_str(), g.title.c_str(), g.version,
                        (unsigned long long)g.digest, utf8(g.path).c_str(), p ? utf8(p->path.filename()).c_str() : "(no player)");
        }
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail(std::string("SDL: ") + SDL_GetError());
    Uint64 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const bool offscreen = render_w && render_h;
    if (offscreen) window_flags |= SDL_WINDOW_HIDDEN;
    SDL_Window *window = SDL_CreateWindow("XenoForge", 1280, 720, window_flags);
    if (!window) return fail(std::string("SDL: ") + SDL_GetError());

    // the window on Vulkan, and RmlUi on it
    std::string error;
    auto surface = std::make_unique<ui::Window>();
    if (!surface->open(window, error)) return fail("Vulkan: " + error);
    std::printf("launcher: Vulkan on %s\n", surface->device_name().c_str());
    auto system = std::make_unique<ui::SystemInterface>(window);
    Rml::SetSystemInterface(system.get());
    Rml::SetRenderInterface(&surface->renderer());
    if (!Rml::Initialise()) return fail("RmlUi did not start");
    std::filesystem::path fonts = assets / "fonts";
    if (!std::filesystem::exists(fonts)) fonts = assets.parent_path() / "fonts";
    const std::pair<const char *, int> faces[] = {{"Inter-Regular.ttf", 400}, {"Inter-Medium.ttf", 500}, {"Inter-SemiBold.ttf", 600}, {"Inter-Bold.ttf", 700}};
    for (const auto &[file, weight] : faces)
        if (!Rml::LoadFontFace(utf8(fonts / file), "Inter", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight(weight)))
            std::fprintf(stderr, "launcher: no font %s\n", utf8(fonts / file).c_str());

    int pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    const uint32_t first_w = offscreen ? render_w : uint32_t(std::max(pixel_w, 1)), first_h = offscreen ? render_h : uint32_t(std::max(pixel_h, 1));
    Rml::Context *context = Rml::CreateContext("launcher", Rml::Vector2i(int(first_w), int(first_h)));
    if (!context) return fail("RmlUi has no context");

    auto view_owner = std::make_unique<frontend::LauncherView>(core, surface->renderer(), window, options);
    frontend::LauncherView &view = *view_owner;
    if (!view.load(context, assets, error)) return fail(error);

    // the pads connected
    std::vector<SDL_Gamepad *> pads;
    {
        int count = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; i++)
                if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i])) pads.push_back(g);
            SDL_free(ids);
        }
    }

    const Clock::time_point began = Clock::now();
    size_t key_next = 0;
    double key_at = 0;
    auto press = [&](const std::string &name) {
        static const std::pair<const char *, SDL_GamepadButton> buttons[] = {
            {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},       {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
            {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"a", SDL_GAMEPAD_BUTTON_SOUTH},        {"b", SDL_GAMEPAD_BUTTON_EAST},
            {"x", SDL_GAMEPAD_BUTTON_WEST},           {"y", SDL_GAMEPAD_BUTTON_NORTH},        {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
            {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"start", SDL_GAMEPAD_BUTTON_START}};
        for (const auto &[text, button] : buttons) {
            if (name != text) continue;
            for (bool down : {true, false}) {
                SDL_Event e{};
                e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                e.gbutton.button = Uint8(button);
                e.gbutton.down = down;
                SDL_PushEvent(&e);
            }
        }
    };
    bool quit = false;
    uint64_t frames = 0;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
            if (event.type == SDL_EVENT_GAMEPAD_ADDED)
                if (SDL_Gamepad *g = SDL_OpenGamepad(event.gdevice.which)) pads.push_back(g);
            if (view.on_event(event)) continue;
            ui::process_event(context, window, event);
        }
        // the interface's size: made for 1280 x 720 dp, scaled to the window
        SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
        const uint32_t width = offscreen ? render_w : uint32_t(std::max(pixel_w, 0)), height = offscreen ? render_h : uint32_t(std::max(pixel_h, 0));
        if (width && height) {
            context->SetDimensions(Rml::Vector2i(int(width), int(height)));
            context->SetDensityIndependentPixelRatio(scale > 0 ? scale : std::clamp(std::min(float(width) / 1280.f, float(height) / 720.f), 0.5f, 4.f));
        }
        view.update();
        // the scripted pad presses, once the screen has settled
        if (key_next < key_script.size() && !view.busy()) {
            const double now = std::chrono::duration<double>(Clock::now() - began).count();
            if (key_at == 0) key_at = now + 0.4;
            if (now >= key_at) {
                press(key_script[key_next++]);
                key_at = now + 0.45;
            }
        }
        context->Update();

        // the end asked for: after the time, once what the view waits for is done
        const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
        const bool snap = snap_at > 0 && seconds >= snap_at; // the picture at that moment, busy or not
        const bool ending = snap || (quit_after > 0 && seconds >= quit_after && !view.busy() &&
                                            (key_script.empty() || (key_next >= key_script.size() && seconds >= key_at + 0.6)));
        const bool last = ending || quit;
        if (last && !screenshot.empty()) surface->capture_next();
        const bool shown = surface->frame([&] { context->Render(); }, offscreen ? render_w : 0, offscreen ? render_h : 0);
        frames++;
        if (last && !screenshot.empty()) {
            std::vector<uint8_t> rgba;
            uint32_t w = 0, h = 0;
            if (surface->take_capture(rgba, w, h) && write_png(rgba, w, h, screenshot))
                std::printf("launcher: screenshot %s\n", utf8(screenshot).c_str());
            else
                std::fprintf(stderr, "launcher: no screenshot\n");
        }
        if (ending) quit = true;
        if (!shown || offscreen) SDL_Delay(offscreen ? 8 : 16);
        else if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(8); // a form, not a game
    }

    const double total = std::chrono::duration<double>(Clock::now() - began).count();
    std::printf("launcher: %zu games listed, %llu frames in %.1f s (%.0f a second)\n", core.listing().games.size(),
                (unsigned long long)frames, total, double(frames) / std::max(total, 0.001));
    if (core.run().running) std::printf("launcher: %s goes on running\n", core.run().title.c_str());
    view_owner.reset(); // its document, before the context
    Rml::RemoveContext("launcher");
    Rml::Shutdown(); // the interfaces outlive RmlUi
    system.reset();
    for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
    surface.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
