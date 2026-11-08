// player: a game in a window (plan track C1; docs/runtime.md, "The player"),
// after the 3DS runtime's frontend/player.cpp. The game is opened, run and
// reported as boot does it (tools/game.cpp); the window shows the front
// buffer of the guest's latest swap, scaled to fit; XAM's input answers from
// SDL3's gamepads and the keyboard (sdl_input.h, docs/input.md); the sound
// goes to the sound card (sdl_audio.h). F1 opens the overlay (Dear ImGui):
// the rates, the controllers, the volume, a screenshot; F11 switches
// fullscreen, F12 saves a screenshot. With a port drawing its own frames
// (xenoforge/native_render.h), F6 switches between them and the emulated
// ones and the overlay shows what it skipped. The settings are config.toml
// (settings.h).
//
// One clock, the scheduler's: the guest runs on its own threads and never
// waits for the window. The window is on the renderer's Vulkan device: at
// each swap the renderer hands its front buffer to the presenter on the "GPU
// commands" thread, which makes it the window's next picture on the GPU and
// goes on without waiting (presenter.h); this thread shows the newest picture
// whenever the display lets it. Only screenshots are read back to the CPU.
//
// For checks without a person at the screen: --quit-after <seconds> ends the
// run and prints the report (frames presented, swaps, input states
// delivered, sound frames played, then boot's); --screenshots <dir> writes
// every --screenshot-every N swaps' front buffer there (300 by default);
// --press start@40 (repeatable; tools/game.h) holds buttons on user 0's
// controller at times on the scheduler's clock; --overlay opens the overlay
// from the start; --rates prints a line a second (boot's, with the frames
// handed over and presented and the sound frames sent). --identify alone
// prints the modules whose generated code the player carries (the launcher
// asks it, frontend/library.h). The generic player runs the game library the
// recompile step cached for the disc (game_build.h; --cache names the cache,
// --game-library a library), else interprets.
//
//   player[_<game>] <game.iso | default.xex> [--xex <path>] [--content <dir>] [--config <file>] [--steps N]
//        [--interpret] [--cache <dir>] [--game-library <file>] [--quit-after S] [--screenshots <dir>]
//        [--screenshot-every N] [--press <b@s>]... [--no-sound] [--overlay] [--rates]
//   player[_<game>] --identify

#include "presenter.h"
#include "sdl_audio.h"
#include "sdl_input.h"
#include "settings.h"

#include "../tools/game.h"
#include "xenoforge/native_render.h"
#include "xenoforge/run.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace xf;
using Clock = std::chrono::steady_clock;

namespace {

// a screenshot's front buffer, its swap and when it came on the scheduler's
// clock (the time --press takes)
struct Shot {
    uint64_t swap = 0, ms = 0;
    render::Image frame;
};

// The screenshots the renderer reads back (--screenshots), handed over on
// its thread
struct Mailbox {
    std::mutex mutex;
    std::deque<Shot> shots;
};

bool write_png(const std::filesystem::path &path, const render::Image &frame) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), int(frame.width), int(frame.height), 4, frame.rgba.data(),
                          int(frame.width) * 4) != 0;
}

const char *button_names(uint16_t b, std::string &out) {
    static const std::pair<uint16_t, const char *> names[] = {
        {input::A, "A"},
        {input::B, "B"},
        {input::X, "X"},
        {input::Y, "Y"},
        {input::START, "Start"},
        {input::BACK, "Back"},
        {input::GUIDE, "Guide"},
        {input::LEFT_SHOULDER, "LB"},
        {input::RIGHT_SHOULDER, "RB"},
        {input::LEFT_THUMB, "LS"},
        {input::RIGHT_THUMB, "RS"},
        {input::DPAD_UP, "Up"},
        {input::DPAD_DOWN, "Down"},
        {input::DPAD_LEFT, "Left"},
        {input::DPAD_RIGHT, "Right"},
    };
    out.clear();
    for (const auto &[bit, name] : names)
        if (b & bit) out += out.empty() ? name : std::string(" ") + name;
    return out.empty() ? "-" : out.c_str();
}

// what the overlay shows of the last second
struct Rates {
    double swaps = 0, presented = 0, refreshes = 0, sound = 0, states = 0;
};

int fail(const std::string &message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

} // namespace

int xf::run_player(int argc, char **argv, PortInfo &port) {
    const char *usage = "usage: player <game.iso | default.xex> [--xex <path>] [--content <dir>] [--config <file>] "
                        "[--steps N] [--interpret] [--cache <dir>] [--game-library <file>] [--quit-after S] "
                        "[--screenshots <dir>] [--screenshot-every N] "
                        "[--press <buttons@seconds[:hold]>]... [--no-sound] [--overlay] [--rates] "
                        "[--option <key>=<value>]...\n";
    if (argc < 2) return std::fprintf(stderr, "%s", usage), 2;
    if (std::string(argv[1]) == "--identify") {
        // for the launcher (library.h): the modules whose generated code
        // this player carries, by the digest of the image each was made
        // from; the generic player carries none
        std::printf("player\n");
        for (size_t i = 0; i < port.module_count; i++)
            std::printf("module %016llx %s\n", (unsigned long long)port.modules[i]->digest, port.modules[i]->name);
        return 0;
    }
    game::Options o;
    o.input = argv[1];
    o.modules = port.modules, o.module_count = port.module_count;
    std::vector<std::string> option_args; // --option's, applied over the config file's values
    std::error_code ec;
    o.storage = std::filesystem::temp_directory_path(ec) / "xenoforge";
    std::filesystem::path config_file, screenshots;
    double quit_after = 0;
    uint64_t screenshot_every = 300;
    bool sound_wanted = true, overlay = false, print_rates = false;
    std::vector<game::Press> presses;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--xex" && i + 1 < argc)
            o.xex_path = argv[++i];
        else if (a == "--content" && i + 1 < argc)
            o.storage = argv[++i];
        else if (a == "--steps" && i + 1 < argc)
            o.steps = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--interpret")
            o.interpret = true;
        else if (a == "--cache" && i + 1 < argc)
            o.cache = argv[++i];
        else if (a == "--game-library" && i + 1 < argc)
            o.game_library = argv[++i];
        else if (a == "--config" && i + 1 < argc)
            config_file = argv[++i];
        else if (a == "--quit-after" && i + 1 < argc)
            quit_after = std::strtod(argv[++i], nullptr);
        else if (a == "--screenshots" && i + 1 < argc)
            screenshots = argv[++i];
        else if (a == "--screenshot-every" && i + 1 < argc)
            screenshot_every = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--overlay")
            overlay = true;
        else if (a == "--no-sound")
            sound_wanted = false;
        else if (a == "--rates")
            print_rates = true;
        else if (a == "--option" && i + 1 < argc)
            option_args.push_back(argv[++i]);
        else if (a == "--press" && i + 1 < argc) {
            std::string why;
            if (!game::parse_press(argv[++i], presses.emplace_back(), why)) return fail(why);
        } else
            std::fprintf(stderr, "player: %s ignored\n", a.c_str());
    }
    if (!screenshot_every) screenshot_every = 1;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (sound_wanted ? SDL_INIT_AUDIO : 0)))
        return fail(std::string("SDL: ") + SDL_GetError());
    // the game, opened as boot opens it
    std::string error;
    game::Game g;
    if (!game::open(o, g, error)) return fail(error);
    kernel::Kernel &k = *g.kernel;
    char title_id[16];
    std::snprintf(title_id, sizeof(title_id), "%08X", g.xex.execution.title_id);

    // the settings (the game's own when it has them), written with the
    // defaults the first time so there is a file to edit
    frontend::Settings settings;
    if (config_file.empty()) config_file = frontend::default_path();
    const bool config_read = frontend::load(config_file, settings, error, title_id);
    if (!config_read)
        std::fprintf(stderr, "%s: %s (the defaults instead)\n", config_file.string().c_str(), error.c_str());
    // the port's options: the file's values into the port's variables, the
    // defaults (the variables as the port set them) into the file where it has
    // none, then --option over the variables for this run
    bool port_new = false;
    for (const PortOption &op : port.options) {
        auto at = settings.port.find(op.key);
        if (at == settings.port.end()) {
            port_new = true;
            if (op.kind == PortOption::Kind::Boolean)
                settings.port[op.key] = *op.flag;
            else if (op.kind == PortOption::Kind::Integer)
                settings.port[op.key] = int64_t(*op.integer);
            else
                settings.port[op.key] = *op.real;
            continue;
        }
        const auto &v = at->second;
        if (op.kind == PortOption::Kind::Boolean) {
            if (auto *b = std::get_if<bool>(&v)) *op.flag = *b;
        } else if (op.kind == PortOption::Kind::Integer) {
            if (auto *i = std::get_if<int64_t>(&v)) *op.integer = int(std::clamp<double>(double(*i), op.min, op.max));
        } else if (auto *d = std::get_if<double>(&v)) {
            *op.real = std::clamp(*d, op.min, op.max);
        } else if (auto *i = std::get_if<int64_t>(&v)) {
            *op.real = std::clamp(double(*i), op.min, op.max);
        }
    }
    if (config_read && (!std::filesystem::exists(config_file, ec) || port_new))
        if (!frontend::save(config_file, settings, error)) std::fprintf(stderr, "%s\n", error.c_str());
    for (const std::string &assignment : option_args) {
        std::string why;
        if (!port.set_option(assignment, why)) return fail(why);
    }
    std::printf("settings: %s%s\n", config_file.string().c_str(),
                settings.game.empty() ? "" : (std::string(" (the game's own, ") + title_id + ")").c_str());
    std::filesystem::path key_shots =
        settings.screenshots.empty() ? config_file.parent_path() / "screenshots" : settings.screenshots;

    SDL_Window *window = SDL_CreateWindow((std::string("XenoForge - ") + (port.name.empty() ? title_id : port.name)).c_str(), 1280, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) return fail(std::string("SDL: ") + SDL_GetError());
    // one Vulkan device for the renderer and the window: the front buffers
    // never leave the GPU
    vk::Device::Window surface;
    Uint32 extensions = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&extensions);
    if (!names) return fail(std::string("SDL: no Vulkan: ") + SDL_GetError());
    surface.instance_extensions.assign(names, names + extensions);
    surface.make_surface = [window](VkInstance instance) {
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &s))
            std::fprintf(stderr, "SDL: no surface: %s\n", SDL_GetError());
        return s;
    };
    g.device = vk::Device::open(error, &surface);
    if (!g.device) return fail("Vulkan: " + error);
    frontend::Presenter presenter;
    if (!presenter.open(*g.device, error)) return fail("the window cannot show Vulkan's pictures: " + error);

    // the overlay (Dear ImGui) on the presenter's device
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL3_InitForVulkan(window);
    VkFormat format = presenter.format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = presenter.instance();
    info.PhysicalDevice = presenter.physical();
    info.Device = presenter.device();
    info.QueueFamily = presenter.family();
    info.Queue = presenter.queue();
    info.DescriptorPool = presenter.overlay_pool();
    info.MinImageCount = 2;
    info.ImageCount = 3;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    ImGui_ImplVulkan_Init(&info);
    {
        std::lock_guard<std::mutex> lock(presenter.queue_mutex()); // it submits on the window's queue
        ImGui_ImplVulkan_CreateFontsTexture();
    }

    // the sound: SDL's device for each client, else the pacing output alone
    frontend::Sound sound;
    bool sounding = false;
    if (sound_wanted) {
        if (sound.open(error)) {
            sounding = true;
            kernel::set_audio_output(k, sound.outputs());
            std::printf("sound: %u channels to the device\n", sound.channels());
        } else {
            std::fprintf(stderr, "sound: %s (paced, not heard)\n", error.c_str());
        }
    }
    auto apply = [&] {
        sound.set_gain(settings.audio.mute ? 0.0f : settings.audio.volume / 100.0f);
        SDL_SetWindowFullscreen(window, settings.video.fullscreen);
    };

    // the input: the gamepads, then the keyboard, as Xenia asks its drivers;
    // the scripted presses at a swap count the GPU's swaps
    auto swaps = [&k] { return kernel::gpu_command_processor(k).counts().swaps.load(); };
    frontend::Controllers controllers(settings, game::presses_overlay(presses, swaps));
    kernel::add_input_driver(k, controllers.gamepads());
    kernel::add_input_driver(k, controllers.keyboard());
    if (!presses.empty() && !settings.input.keyboard)
        kernel::add_input_driver(k, input::make_idle_pad(game::presses_overlay(presses, swaps)));
    kernel::set_input_deadzones(k, settings.input.left_deadzone, settings.input.right_deadzone);
    apply();

    // the renderer hands each front buffer to the presenter at its swap, on
    // the GPU. Screenshots: --screenshots' swaps read back by the renderer
    // (boot's --shots picture) and the window's picture of the same swap
    // beside it (<swap>_window.png, which should be the same); F12's, the
    // window's next picture
    Mailbox mailbox;
    render::Options options;
    auto shot_wanted = [&](uint64_t swap) { return !screenshots.empty() && swap % screenshot_every == 0; };
    std::atomic<int> keys_wanted{0};
    options.on_swap = [&](const render::FrontBuffer &front) { presenter.take(front); };
    // (the driver's compiled pipelines and the shader storage are kept
    // between runs beside the game library, in the cache, where the
    // launcher's build step makes them: game::start_renderer's default)
    // new pipelines made on worker threads, their draws drawn meanwhile by
    // Options::pipeline_stand_in's default (boot and the replays wait for
    // each, as Xenia's trace dump does)
    options.async_pipelines = true;
    options.frame_wanted = [&](uint64_t swap) {
        if (!shot_wanted(swap)) return false;
        presenter.capture_next(); // this swap's picture, taken after this
        return true;
    };
    options.on_frame = [&](uint64_t swap, const render::Image &frame) {
        std::lock_guard<std::mutex> lock(mailbox.mutex);
        mailbox.shots.push_back({swap, k.uptime_ms(), frame});
    };
    if (!game::start_renderer(g, options, error)) return fail(error);
    std::printf("rendering on %s, the window on the same device (%s)\n", g.device->name().c_str(),
                presenter.own_queue() ? "a queue of its own" : "the renderer's queue");
    stbi_write_png_compression_level = 1;
    if (port.on_init) port.on_init(k);
    if (!game::launch(g, error)) return fail("launch: " + error);
    const Clock::time_point started = Clock::now();

    bool quit = false, changed = false, shown_any = false;
    uint64_t presented = 0, refreshes = 0, key_screenshots = 0, shots_written = 0;
    std::string ended;
    Rates rates;
    Clock::time_point second = started, last_present = started;
    uint64_t swaps_before = 0, presented_before = 0, refreshes_before = 0, sound_before = 0, states_before = 0;
    uint64_t taken_before = 0;
    game::Rates rate_log;
    std::string button_text;
    auto key_screenshot = [&] {
        keys_wanted++;
        presenter.capture_next();
    };

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true, ended = "closed";
            controllers.event(e);
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_F1) {
                    overlay = !overlay;
                    if (!overlay && changed) {
                        if (!frontend::save(config_file, settings, error)) std::fprintf(stderr, "%s\n", error.c_str());
                        changed = false;
                    }
                    continue;
                }
                if (e.key.scancode == SDL_SCANCODE_F11) {
                    settings.video.fullscreen = !settings.video.fullscreen;
                    SDL_SetWindowFullscreen(window, settings.video.fullscreen);
                    changed = true;
                }
                if (e.key.scancode == SDL_SCANCODE_F12) key_screenshot();
                // the port's frames or the emulated ones, for this run (the
                // port's option, when it binds one, keeps the setting)
                if (e.key.scancode == SDL_SCANCODE_F6 && native::has_output_renderer()) native::toggle();
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        controllers.read_keyboard(focused, overlay);

        // the run's end: time asked for, the code stopped, the main thread done
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - started).count();
        if (quit_after > 0 && seconds >= quit_after)
            quit = true, ended = "quit after " + std::to_string(seconds) + " s";
        if (k.stopped) quit = true, ended = "stopped: " + k.stop_reason;
        uint32_t exit_code = 0;
        if (k.wait_main(0, &exit_code)) quit = true, ended = "the main thread ended";

        // the screenshots asked for
        std::deque<Shot> shots;
        {
            std::lock_guard<std::mutex> lock(mailbox.mutex);
            shots.swap(mailbox.shots);
        }
        for (const Shot &shot : shots) {
            std::filesystem::path path =
                screenshots / (std::string(title_id) + "_" + std::to_string(shot.swap) + ".png");
            if (write_png(path, shot.frame)) {
                shots_written++;
                std::printf("screenshot: swap %llu at %.1f s, %s\n", (unsigned long long)shot.swap, shot.ms / 1000.0,
                            path.string().c_str());
            } else {
                std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            }
        }
        // the window's pictures copied back: a --screenshots swap's, or F12's
        render::Image picture;
        uint64_t picture_swap = 0;
        while (presenter.captured(picture, picture_swap)) {
            const bool key = !shot_wanted(picture_swap) && keys_wanted > 0;
            if (!key && !shot_wanted(picture_swap)) continue;
            std::filesystem::path path =
                key ? key_shots / (std::string(title_id) + "_" + std::to_string(picture_swap) + "_key" +
                                   std::to_string(++key_screenshots) + ".png")
                    : screenshots / (std::string(title_id) + "_" + std::to_string(picture_swap) + "_window.png");
            if (key) keys_wanted--;
            if (write_png(path, picture))
                std::printf("screenshot: %s\n", path.string().c_str());
            else
                std::fprintf(stderr, "could not write %s\n", path.string().c_str());
        }
        // F12's capture taken by a --screenshots swap: asked again
        if (keys_wanted > 0 && !presenter.capturing()) presenter.capture_next();

        // the rates over the last second
        const uint64_t swapped = swaps();
        if (now - second >= std::chrono::seconds(1)) {
            const double dt = std::chrono::duration<double>(now - second).count();
            const uint64_t states = kernel::input_states_delivered(k);
            rates.swaps = (swapped - swaps_before) / dt;
            rates.presented = (presented - presented_before) / dt;
            rates.refreshes = (refreshes - refreshes_before) / dt;
            rates.sound = (sound.sent - sound_before) / dt;
            rates.states = (states - states_before) / dt;
            if (print_rates) {
                static double fence = 0, acquire = 0, present = 0;
                char extra[256];
                std::snprintf(extra, sizeof(extra),
                              "; taken %.1f/s, presented %.1f/s, refreshes %.1f/s, sound sent %.1f/s; the window "
                              "waited %.0f ms for its frame, %.0f ms for an image, %.0f ms presenting",
                              (presenter.taken - taken_before) / dt, rates.presented, rates.refreshes, rates.sound,
                              (presenter.fence_seconds - fence) * 1000, (presenter.acquire_seconds - acquire) * 1000,
                              (presenter.present_seconds - present) * 1000);
                fence = presenter.fence_seconds, acquire = presenter.acquire_seconds;
                present = presenter.present_seconds;
                game::print_rates(g, rate_log, seconds, extra);
            }
            taken_before = presenter.taken;
            swaps_before = swapped, presented_before = presented, refreshes_before = refreshes;
            sound_before = sound.sent, states_before = states;
            second = now;
        }

        // nothing new and nothing moving: no present (with vsync each
        // present waits for the display; this thread alone)
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const bool minimized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) || !pw || !ph;
        if (minimized || (!presenter.fresh() && !overlay && now - last_present < std::chrono::milliseconds(100))) {
            SDL_Delay(minimized ? 10 : 1);
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (overlay) {
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_FirstUseEver);
            bool keep = true;
            ImGui::Begin("XenoForge (F1)", &keep, ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("guest swaps %.1f/s (%llu), frames presented %.1f/s (%llu)", rates.swaps,
                        (unsigned long long)swapped, rates.presented, (unsigned long long)presented);
            ImGui::Text("window refreshes %.1f/s, run %.0f s; front buffers not shown (no picture free) %llu",
                        rates.refreshes, seconds, (unsigned long long)presenter.skipped.load());
            ImGui::Text("sound: %llu frames played (%.1f/s, %u channels), %llu with sound, %llu starved",
                        (unsigned long long)sound.sent.load(), rates.sound, sound.channels(),
                        (unsigned long long)sound.audible.load(), (unsigned long long)sound.starved.load());
            ImGui::Text("input: %llu states delivered (%.1f/s)", (unsigned long long)kernel::input_states_delivered(k),
                        rates.states);
            ImGui::SeparatorText("Controllers");
            if (ImGui::BeginTable("pads", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                auto view = controllers.view(k.uptime_ms());
                for (uint32_t u = 0; u < input::USERS; u++) {
                    const auto &v = view[u];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", u);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(v.source.empty() ? "(none)" : v.source.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(v.source.empty() ? "" : button_names(v.pad.buttons, button_text));
                    ImGui::TableNextColumn();
                    if (!v.source.empty())
                        ImGui::Text("LT %3u RT %3u  L %6d %6d  R %6d %6d", v.pad.left_trigger, v.pad.right_trigger,
                                    v.pad.thumb_lx, v.pad.thumb_ly, v.pad.thumb_rx, v.pad.thumb_ry);
                }
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Settings");
            bool moved = false;
            moved |= ImGui::SliderInt("Volume", &settings.audio.volume, 0, 100, "%d%%");
            moved |= ImGui::Checkbox("Mute", &settings.audio.mute);
            moved |= ImGui::Checkbox("Vertical sync (the window's only)", &settings.video.vsync);
            ImGui::SameLine();
            moved |= ImGui::Checkbox("Smooth scaling", &settings.video.smooth);
            bool stretch = settings.video.aspect == "stretch";
            if (ImGui::Checkbox("Stretch", &stretch))
                settings.video.aspect = stretch ? "stretch" : "auto", moved = true;
            ImGui::SameLine();
            moved |= ImGui::Checkbox("Fullscreen (F11)", &settings.video.fullscreen);
            if (moved) apply(), changed = true;
            if (!port.options.empty()) {
                ImGui::SeparatorText(port.name.empty() ? "Port" : port.name.c_str());
                bool port_moved = false;
                for (const PortOption &op : port.options) {
                    const char *label = op.label.empty() ? op.key.c_str() : op.label.c_str();
                    bool one = false;
                    if (op.kind == PortOption::Kind::Boolean) {
                        one = ImGui::Checkbox(label, op.flag);
                        if (one) settings.port[op.key] = *op.flag;
                    } else if (op.kind == PortOption::Kind::Integer) {
                        one = ImGui::SliderInt(label, op.integer, int(op.min), int(op.max));
                        if (one) settings.port[op.key] = int64_t(*op.integer);
                    } else {
                        one = ImGui::SliderScalar(label, ImGuiDataType_Double, op.real, &op.min, &op.max, "%.3f");
                        if (one) settings.port[op.key] = *op.real;
                    }
                    port_moved |= one;
                }
                if (port_moved) changed = true;
            }
            if (native::has_output_renderer()) {
                const native::Counters n = native::counters();
                ImGui::SeparatorText("Native renderer (F6)");
                ImGui::Text("%s; %llu frames the port's, %llu yielded (%llu held)",
                            !native::enabled() ? "off: the emulated frames"
                            : native::active() ? "on: the port's frames"
                                               : "on: the port yields",
                            (unsigned long long)n.frames_native, (unsigned long long)n.frames_yielded,
                            (unsigned long long)n.frames_held);
                ImGui::Text("skipped %llu draws, %llu resolves; kept %llu, %llu; %llu exporting memory; %llu "
                            "occlusion reports faked",
                            (unsigned long long)n.draws_skipped, (unsigned long long)n.resolves_skipped,
                            (unsigned long long)n.draws_kept, (unsigned long long)n.resolves_kept,
                            (unsigned long long)n.memexport_kept, (unsigned long long)n.occlusion_faked);
            }
            ImGui::SeparatorText("Screenshot");
            if (ImGui::Button("Save (F12)")) key_screenshot();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", key_shots.string().c_str());
            ImGui::End();
            if (!keep) overlay = false;
        }
        ImGui::Render();
        frontend::Presenter::View view;
        view.width = uint32_t(pw), view.height = uint32_t(ph);
        view.vsync = settings.video.vsync;
        view.smooth = settings.video.smooth;
        view.stretch = settings.video.aspect == "stretch";
        const bool fresh = presenter.present(
            view, [](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
        last_present = Clock::now();
        refreshes++;
        if (fresh) presented++, shown_any = true;
    }

    // the report: the player's counts, then boot's
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (changed && !frontend::save(config_file, settings, error)) std::fprintf(stderr, "%s\n", error.c_str());
    std::printf("player: %s\n", ended.c_str());
    const uint64_t swapped = swaps();
    std::printf("player: %.1f s; %llu swaps (%.1f/s), %llu front buffers taken on the GPU (%llu not, no picture "
                "free), %llu frames presented (%.1f/s), %llu window refreshes; %llu screenshots written to %s\n",
                seconds, (unsigned long long)swapped, swapped / seconds, (unsigned long long)presenter.taken.load(),
                (unsigned long long)presenter.skipped.load(), (unsigned long long)presented, presented / seconds,
                (unsigned long long)refreshes, (unsigned long long)shots_written,
                screenshots.empty() ? "(none asked)" : screenshots.string().c_str());
    std::printf("player: the window waited %.1f s for its frames in flight, %.1f s for swapchain images, %.1f s "
                "presenting%s\n",
                presenter.fence_seconds, presenter.acquire_seconds, presenter.present_seconds,
                shown_any ? "" : " (nothing was shown)");
    std::printf("player: input states delivered %llu\n", (unsigned long long)kernel::input_states_delivered(k));
    if (sounding)
        std::printf("player: sound frames: %llu submitted, %llu played to the device (%u channels), %llu with sound, "
                    "%llu dropped, %llu times starved\n",
                    (unsigned long long)sound.submitted.load(), (unsigned long long)sound.sent.load(), sound.channels(),
                    (unsigned long long)sound.audible.load(), (unsigned long long)sound.dropped.load(),
                    (unsigned long long)sound.starved.load());
    else
        std::printf("player: no sound device\n");
    std::printf("threads:\n%s", k.thread_report().c_str());
    game::print_report(g, true);
    if (port.on_shutdown) port.on_shutdown(k);
    // the guest's threads still run: no destructors under them (boot's way)
    presenter.idle();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
