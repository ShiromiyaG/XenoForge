// player: a game in a window (plan track C1; docs/runtime.md, "The player"),
// after the 3DS runtime's frontend/player.cpp. The game is opened, run and
// reported as boot does it (tools/game.cpp); the window shows the front
// buffer of the guest's latest swap, scaled to fit; XAM's input answers from
// SDL3's gamepads and the keyboard (sdl_input.h, docs/input.md); the sound
// goes to the sound card (sdl_audio.h). F1 opens the overlay (Dear ImGui):
// the rates, the controllers, the volume, a screenshot; F11 switches
// fullscreen, F12 saves a screenshot. With a port drawing its own frames
// (xenoforge/native_render.h), F6 switches between them and the emulated
// ones and the overlay shows what it skipped. F9 reloads the port's hooks
// (when they are a module; also when its file changes). The settings are config.toml
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
// --window-shots <dir> saves the window itself (the overlay and the system's dialogs over the picture) every
// --window-shot-every S seconds (2); --ui-demo <list> opens samples of the system's dialogs (docs/runtime.md);
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
//        [--screenshot-every N] [--press <b@s>]... [--no-sound] [--overlay] [--rates] [--hook-stats]
//   player[_<game>] --identify

#include "presenter.h"
#include "sdl_audio.h"
#include "sdl_input.h"
#include "console_options.h"
#include "guide_theme.h"
#include "library.h"
#include "window_icon.h"
#include "overlay_system.h"
#include "profiles.h"
#include "settings.h"
#include "system_ui_view.h"

#include "../tools/game.h"
#include "xenoforge/hooks.h"
#include "xenoforge/native_render.h"
#include "xenoforge/run.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
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

// A sample of each dialog a title can open, for --ui-demo (what a check can see without the title's help)
bool demo_request(const std::string &name, kernel::UiRequest &r) {
    using kernel::UiKind;
    r = {};
    r.user = 0;
    if (name == "message") {
        r.kind = UiKind::MessageBox;
        r.title = u"Save game";
        r.text = u"Do you want to overwrite the existing save? This cannot be undone.";
        r.buttons = {u"Yes", u"No", u"Cancel"};
        r.default_button = 1;
        r.flags = 2;
    } else if (name == "keyboard") {
        r.kind = UiKind::Keyboard;
        r.title = u"Name your profile";
        r.text = u"Type a name (up to 15 characters)";
        r.default_text = u"Player";
        r.max_length = 15;
    } else if (name == "device") {
        r.kind = UiKind::DeviceSelector;
        r.bytes_required = 3u << 20;
    } else if (name == "signin") {
        r.kind = UiKind::SignIn;
        r.users_needed = 1;
    } else if (name == "marketplace") {
        r.kind = UiKind::Marketplace;
    } else if (name == "achievements") {
        r.kind = UiKind::Achievements;
    } else if (name == "gamercard") {
        r.kind = UiKind::GamerCard;
    } else if (name == "editprofile") {
        r.kind = UiKind::EditProfile;
    } else if (name == "friends") {
        r.kind = UiKind::Friends;
    } else if (name == "messages") {
        r.kind = UiKind::Messages;
    } else if (name == "players") {
        r.kind = UiKind::Players;
    } else if (name == "quickchat") {
        r.kind = UiKind::QuickChat;
    } else if (name == "gameinvite") {
        r.kind = UiKind::GameInvite;
    } else if (name == "friendrequest") {
        r.kind = UiKind::FriendRequest;
        r.xuid = 0xE000123456789ABCull;
    } else if (name == "playerreview") {
        r.kind = UiKind::PlayerReview;
        r.xuid = 0xE000123456789ABCull;
    } else if (name == "compose") {
        r.kind = UiKind::MessageCompose;
        r.xuids = {0xE000123456789ABCull};
    } else if (name == "customcompose") {
        r.kind = UiKind::CustomMessageCompose;
        r.text = u"Join my game!";
    } else if (name == "playerlist") {
        r.kind = UiKind::CustomPlayerList;
        r.xuids = {0xE000123456789ABCull, 0xE0000FEDCBA98765ull};
    } else {
        return false;
    }
    return true;
}

int fail(const std::string &message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

} // namespace

int xf::run_player(int argc, char **argv, PortInfo &port) {
    const char *usage = "usage: player <game.iso | default.xex> [--xex <path>] [--content <dir>] [--config <file>] "
                        "[--steps N] [--interpret] [--cache <dir>] [--game-library <file>] [--quit-after S] "
                        "[--screenshots <dir>] [--screenshot-every N] "
                        "[--press <buttons@seconds[:hold]>]... [--no-sound] [--overlay] [--rates] [--hook-stats] "
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
    std::filesystem::path config_file, screenshots, launch_data_file;
    double quit_after = 0;
    uint64_t screenshot_every = 300;
    bool sound_wanted = true, overlay = false, print_rates = false;
    bool storage_given = false, directory_saves = false, system_ui = true;
    std::filesystem::path window_shots; // --window-shots: the window itself (the overlay and dialogs on it) saved here
    double window_shot_every = 2.0, next_window_shot = 1.0;
    uint64_t window_shots_written = 0;
    double restart_after = 0; // --restart-after S: the overlay's "Restart the title" pressed at S seconds (a check of it)
    std::string ui_demo; // --ui-demo's list of dialogs to open at the start (a check without a game's help)
    std::vector<game::Press> presses;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--xex" && i + 1 < argc)
            o.xex_path = argv[++i];
        else if (a == "--content" && i + 1 < argc)
            o.storage = argv[++i], storage_given = true;
        else if (a == "--directory-saves")
            directory_saves = true;
        else if (a == "--window-shots" && i + 1 < argc)
            window_shots = argv[++i];
        else if (a == "--window-shot-every" && i + 1 < argc)
            window_shot_every = std::strtod(argv[++i], nullptr);
        else if (a == "--restart-after" && i + 1 < argc)
            restart_after = std::strtod(argv[++i], nullptr);
        else if (a == "--no-system-ui")
            system_ui = false;
        else if (a == "--ui-demo" && i + 1 < argc)
            ui_demo = argv[++i];
        else if (a == "--launch-data" && i + 1 < argc)
            launch_data_file = argv[++i];
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
        else if (a == "--hook-stats")
            hooks::set_stats(true);
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
    std::string error;
    // the storage root and the saves' format come from the shared settings before the game opens (it mounts them);
    // the game's own settings, read after, may change the format and the profile
    if (config_file.empty()) config_file = frontend::default_path();
    {
        frontend::Settings shared;
        std::string why;
        if (frontend::load(config_file, shared, why)) {
            if (!storage_given) o.storage = frontend::storage_root(shared, config_file);
            if (shared.saves.format == "directory") o.content_format = kernel::ContentFormat::Directory;
        }
    }
    if (directory_saves) o.content_format = kernel::ContentFormat::Directory;
    // the game, opened as boot opens it
    game::Game g;
    if (!game::open(o, g, error)) return fail(error);
    kernel::Kernel &k = *g.kernel;
    char title_id[16];
    std::snprintf(title_id, sizeof(title_id), "%08X", g.xex.execution.title_id);

    // the settings (the game's own when it has them), written with the
    // defaults the first time so there is a file to edit
    frontend::Settings settings;
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
    // the console's own settings the player chose (config.toml's [system]), before the title runs and again when
    // the overlay changes them
    auto apply_system = [&] {
        frontend::clamp_system(settings.system);
        kernel::SystemSettings system;
        system.file = o.storage / "xconfig.bin";
        system.language = uint32_t(std::max(0, settings.system.language));
        system.country = settings.system.country;
        system.width = uint32_t(std::max(0, settings.system.width));
        system.height = uint32_t(std::max(0, settings.system.height));
        system.widescreen = settings.system.widescreen;
        system.refresh = settings.system.refresh;
        kernel::apply_system_settings(*g.kernel, system);
    };
    apply_system();
    // the saves' format (a game's own may differ from the shared one) and the profile the games use: its XUID and
    // gamertag (the storage root's first profile and the default name when the settings name none)
    if (!directory_saves)
        kernel::set_content_format(k, settings.saves.format == "directory" ? kernel::ContentFormat::Directory
                                                                           : kernel::ContentFormat::Stfs);
    kernel::set_user(k, frontend::selected_xuid(settings), frontend::selected_gamertag(o.storage, settings));
    // the system's dialogs and toasts on the window (docs/runtime.md, "The player's system UI"); a request the
    // model does not know is left to the kernel's default
    frontend::SystemUi sysui([&k] { return kernel::profile_summary(k); });
    if (system_ui) {
        kernel::set_ui_handler(k, [&sysui, &k](const kernel::UiRequest &r, kernel::UiDone done) {
            const bool taken = sysui.handle(r, std::move(done));
            std::printf("ui: dialog kind %u (user %u) %s at %.1f s\n", unsigned(r.kind), r.user,
                        taken ? "opened" : "declined", k.uptime_ms() / 1000.0);
            return taken;
        });
        kernel::set_toast_handler(k, [&sysui](const kernel::Toast &t) { sysui.toast(t); });
    }
    if (!launch_data_file.empty()) {
        // what the title before this one left it (a launch: XamLoaderSetLaunchData)
        std::ifstream in(launch_data_file, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        kernel::set_launch_data(k, data);
    }
    std::printf("settings: %s%s\n", config_file.string().c_str(),
                settings.game.empty() ? "" : (std::string(" (the game's own, ") + title_id + ")").c_str());
    std::filesystem::path key_shots =
        settings.screenshots.empty() ? config_file.parent_path() / "screenshots" : settings.screenshots;

    // the window: the game's name and image (its executable's XDBF), else the port's name or the title ID and
    // XenoForge's mark
    std::string window_title = port.name;
    if (window_title.empty()) window_title = frontend::title_name(g.xex);
    if (window_title.empty()) window_title = title_id;
    SDL_Window *window = SDL_CreateWindow((window_title + " - XenoForge").c_str(), 1280, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) return fail(std::string("SDL: ") + SDL_GetError());
    if (!frontend::set_window_icon(window, frontend::title_icon(g.xex)))
        frontend::set_window_icon(window, frontend::xenoforge_icon_png());
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
    {
        // the console screens' typeface (guide_theme.h), made for the window's size at the start
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window, &ww, &wh);
        const float font_scale = std::clamp(float(wh) / 720.0f, 1.0f, 2.0f);
        if (!frontend::guide::load_fonts(frontend::guide::find_fonts(), font_scale))
            std::fprintf(stderr, "player: no Inter fonts beside the executable (ui/fonts); the overlay's own font\n");
        ImGui::GetIO().FontGlobalScale = 1.0f / font_scale;
    }
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
    frontend::IconCache icons(*g.device);
    frontend::ConsolePages pages;
    frontend::PadNavigator pad_nav;
    bool dialog_was_up = false, restart_requested = false;
    uint64_t answered_seen = 0;
    int guide_page_demo = 0; // --ui-demo guide-profile, guide-system: the guide opened on that page
    // --ui-demo: dialogs opened at the start with samples of what titles ask, answered by the pad and keyboard
    for (size_t at = 0; at < ui_demo.size();) {
        size_t comma = ui_demo.find(',', at);
        std::string name = ui_demo.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? ui_demo.size() : comma + 1;
        kernel::UiRequest r;
        if (name == "console" || name == "guide") continue; // (the overlay's console section; the guide)
        if (name == "guide-profile" || name == "guide-system") {
            guide_page_demo = name == "guide-profile" ? 1 : 2;
            continue;
        }
        if (name == "toast") {
            kernel::Toast t;
            t.kind = kernel::Toast::Kind::Achievement;
            t.title = u"First steps", t.text = u"Finish the tutorial", t.gamerscore = 15;
            sysui.toast(t);
            continue;
        }
        if (!demo_request(name, r)) {
            std::fprintf(stderr, "player: --ui-demo %s: no such dialog\n", name.c_str());
            continue;
        }
        sysui.handle(r, [name](const kernel::UiResult &answer) {
            std::printf("ui-demo: %s answered: %s, button %u, text \"%s\", device %u\n", name.c_str(),
                        answer.cancelled ? "cancelled" : "accepted", answer.button,
                        frontend::utf8_of(answer.text).c_str(), answer.device_id);
        });
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
    auto texture_filter_of = [](const std::string &f) { return f == "trilinear" ? 1u : f == "nearest" ? 2u : 0u; };
    auto apply = [&] {
        sound.set_gain(settings.audio.mute ? 0.0f : settings.audio.volume / 100.0f);
        SDL_SetWindowFullscreen(window, settings.video.fullscreen);
        // the texture filtering, live (the resolution scale waits for a restart)
        if (g.renderer)
            g.renderer->set_texture_filtering(uint32_t(settings.video.anisotropy),
                                              texture_filter_of(settings.video.texture_filter));
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
    // the picture beyond the console (settings.h's video section)
    options.resolution_scale = uint32_t(settings.video.resolution_scale / 25);
    options.min_scalable = uint32_t(settings.video.min_scalable);
    options.anisotropy = uint32_t(settings.video.anisotropy);
    options.texture_filter = texture_filter_of(settings.video.texture_filter);
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
    const std::string scale_notice = g.renderer->scale_notice();
    if (g.renderer->resolution_scale() != 4)
        std::printf("drawing at %u%% of the console's resolution%s%s\n", g.renderer->resolution_scale() * 25,
                    scale_notice.empty() ? "" : ": ", scale_notice.c_str());
    std::printf("rendering on %s, the window on the same device (%s)\n", g.device->name().c_str(),
                presenter.own_queue() ? "a queue of its own" : "the renderer's queue");
    stbi_write_png_compression_level = 1;
    if (port.on_init) port.on_init(k);
    if (!port.hook_module.empty() && !game::load_port_module(g, port.hook_module, error)) return fail(error);
    if (!game::launch(g, error)) return fail("launch: " + error);
    const Clock::time_point started = Clock::now();

    bool quit = false, changed = false, shown_any = false;
    // the frame-time graph's: the intervals between the frames the window showed
    constexpr uint32_t FRAME_TIMES = 240;
    float frame_times[FRAME_TIMES] = {};
    uint32_t frame_time_at = 0;
    Clock::time_point last_fresh{};
    // the guide (F2 or the controller's Guide button): the console pages in the guide's look
    bool guide_open = ui_demo.find("guide") != std::string::npos;
    if (guide_page_demo) pages.set_guide_page(guide_page_demo);
    uint16_t guide_buttons_before = 0;
    uint64_t guide_combo_since = 0; // when Back and Start were both down (0: not both)
    bool guide_combo_fired = false;
    auto close_guide = [&] {
        guide_open = false;
        if (changed) {
            if (!frontend::save(config_file, settings, error)) std::fprintf(stderr, "%s\n", error.c_str());
            changed = false;
        }
    };
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
            if (e.type == SDL_EVENT_KEY_DOWN && sysui.active()) {
                // the keyboard drives the dialog on top (Enter in a text field is the field's own)
                frontend::Dialog *d = sysui.front();
                const bool typing = d && d->text_entry;
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: sysui.navigate(frontend::Nav::Up); break;
                case SDL_SCANCODE_DOWN: sysui.navigate(frontend::Nav::Down); break;
                case SDL_SCANCODE_LEFT: if (!typing) sysui.navigate(frontend::Nav::Left); break;
                case SDL_SCANCODE_RIGHT: if (!typing) sysui.navigate(frontend::Nav::Right); break;
                case SDL_SCANCODE_PAGEUP: sysui.navigate(frontend::Nav::PageUp); break;
                case SDL_SCANCODE_PAGEDOWN: sysui.navigate(frontend::Nav::PageDown); break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    if (!typing && !e.key.repeat) sysui.navigate(frontend::Nav::Accept);
                    break;
                case SDL_SCANCODE_ESCAPE:
                    if (!e.key.repeat) sysui.navigate(frontend::Nav::Cancel);
                    break;
                default: break;
                }
            }
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
                if (e.key.scancode == SDL_SCANCODE_F2 && !sysui.active()) {
                    if (guide_open) close_guide();
                    else guide_open = true;
                }
                // the port's frames or the emulated ones, for this run (the
                // port's option, when it binds one, keeps the setting)
                if (e.key.scancode == SDL_SCANCODE_F6 && native::has_output_renderer()) native::toggle();
                if (e.key.scancode == SDL_SCANCODE_F9) game::poll_port_module(g, true); // the port's hooks reloaded
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        // a dialog of the system takes the controllers and the keyboard from the title, as the console's guide does
        const bool dialog_up = sysui.active();
        {
            // the Guide button opens and closes the guide, and so do Back and Start held together for half a second
            // (Windows' Game Bar often takes the Guide button); a dialog of the title's closes it
            const uint64_t at = k.uptime_ms();
            const uint16_t buttons = controllers.view(at)[0].pad.buttons;
            bool toggle = (buttons & input::GUIDE) && !(guide_buttons_before & input::GUIDE);
            constexpr uint16_t combo = input::BACK | input::START;
            if ((buttons & combo) == combo) {
                if (!guide_combo_since) guide_combo_since = at ? at : 1;
                if (!guide_combo_fired && at - guide_combo_since >= 500) toggle = true, guide_combo_fired = true;
            } else {
                guide_combo_since = 0, guide_combo_fired = false;
            }
            if (toggle && !dialog_up) {
                if (guide_open) close_guide();
                else guide_open = true;
            }
            guide_buttons_before = buttons;
            if (dialog_up && guide_open) close_guide();
            ImGuiIO &io = ImGui::GetIO();
            if (guide_open) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
            else io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        }
        controllers.set_taken(dialog_up || guide_open);
        controllers.read_keyboard(focused, overlay || dialog_up || guide_open);
        if (dialog_up) {
            const uint64_t at = k.uptime_ms();
            const input::Gamepad pad = controllers.view(at)[sysui.user()].pad;
            // (a dialog answered and the next one up at once is a new dialog too)
            const bool opened = !dialog_was_up || sysui.answered() != answered_seen;
            answered_seen = sysui.answered();
            if (opened)
                pad_nav.reset(pad, at); // a button already down when it opened is not an answer
            else
                for (frontend::Nav nav : pad_nav.feed(pad, at)) sysui.navigate(nav);
        }
        dialog_was_up = dialog_up;

        // the run's end: time asked for, the code stopped, the main thread done
        const Clock::time_point now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - started).count();
        if (quit_after > 0 && seconds >= quit_after)
            quit = true, ended = "quit after " + std::to_string(seconds) + " s";
        if (restart_after > 0 && seconds >= restart_after && !quit) restart_requested = quit = true, ended = "restart";
        if (k.stopped) quit = true, ended = "stopped: " + k.stop_reason;
        uint32_t exit_code = 0;
        if (k.wait_main(0, &exit_code)) quit = true, ended = "the main thread ended";
        game::poll_port_module(g, false); // the port's hooks, loaded again when their module changed

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
        const bool ui_busy = sysui.active() || sysui.has_toasts() || guide_open;
        if (minimized ||
            (!presenter.fresh() && !overlay && !ui_busy && now - last_present < std::chrono::milliseconds(100))) {
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
            ImGui::SameLine();
            moved |= ImGui::Checkbox("Fullscreen (F11)", &settings.video.fullscreen);
            if (settings.video.frame_graph) {
                // the time between the frames the window showed, the last 240
                float values[FRAME_TIMES];
                float worst = 0, sum = 0;
                for (uint32_t i = 0; i < FRAME_TIMES; i++) {
                    values[i] = frame_times[(frame_time_at + i) % FRAME_TIMES];
                    worst = std::max(worst, values[i]), sum += values[i];
                }
                char caption[96];
                std::snprintf(caption, sizeof(caption), "frame time: %.1f ms average, %.1f ms worst", sum / FRAME_TIMES,
                              worst);
                ImGui::PlotLines("##frames", values, int(FRAME_TIMES), 0, caption, 0.0f, std::max(50.0f, worst * 1.1f),
                                 ImVec2(-FLT_MIN, 70));
            }
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
            if (hooks::stats_on()) {
                // the port's hooks and their calls (--hook-stats)
                ImGui::SeparatorText("Port hooks");
                if (ImGui::BeginTable("hooks", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const hooks::Stat &st : hooks::stats()) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("0x%08X %s", st.address, st.function.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(hooks::kind_name(st.kind));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(st.hook.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu", (unsigned long long)st.calls);
                    }
                    ImGui::EndTable();
                }
            }
            if (ui_demo.find("console") != std::string::npos) ImGui::SetNextItemOpen(true, ImGuiCond_Once); // (--ui-demo console)
            if (ImGui::CollapsingHeader("Console: system, profile, achievements")) {
                frontend::ConsolePages::Host host{settings,
                                                  o.storage,
                                                  k,
                                                  icons,
                                                  apply_system,
                                                  [&] {
                                                      restart_requested = true;
                                                      quit = true;
                                                      ended = "restart";
                                                  },
                                                  [&] { apply(); },
                                                  scale_notice};
                if (pages.draw(host, seconds)) changed = true;
            }
            ImGui::SeparatorText("Screenshot");
            if (ImGui::Button("Save (F12)")) key_screenshot();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", key_shots.string().c_str());
            ImGui::End();
            if (!keep) overlay = false;
        }
        if (guide_open) {
            frontend::ConsolePages::Host host{settings,     o.storage, k, icons, apply_system,
                                              [&] {
                                                  restart_requested = true;
                                                  quit = true;
                                                  ended = "restart";
                                              },
                                              [&] { apply(); }, scale_notice};
            bool keep = true;
            if (pages.draw_guide(host, seconds, keep)) changed = true;
            if (!keep) close_guide();
        }
        frontend::draw_system_ui(sysui, icons, seconds);
        ImGui::Render();
        frontend::Presenter::View view;
        view.width = uint32_t(pw), view.height = uint32_t(ph);
        view.vsync = settings.video.vsync;
        view.smooth = settings.video.smooth;
        view.fit = settings.video.aspect == "stretch"   ? frontend::Fit::stretch
                   : settings.video.aspect == "integer" ? frontend::Fit::integer
                                                        : frontend::Fit::aspect;
        view.upscaler = settings.video.upscaler == "fsr" ? 1u : settings.video.upscaler == "cas" ? 2u : 0u;
        view.fsr_sharpness_reduction = float(settings.video.fsr_sharpness);
        view.cas_sharpness = float(settings.video.cas_sharpness);
        view.antialiasing = settings.video.antialiasing == "fxaa"           ? 1u
                            : settings.video.antialiasing == "fxaa_extreme" ? 2u
                                                                            : 0u;
        if (!window_shots.empty() && seconds >= next_window_shot) {
            presenter.capture_window_next();
            next_window_shot = seconds + window_shot_every;
        }
        const bool fresh = presenter.present(
            view, [](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
        if (render::Image shot; presenter.window_captured(shot)) {
            std::filesystem::path path =
                window_shots / (std::string(title_id) + "_window_" + std::to_string(++window_shots_written) + ".png");
            if (write_png(path, shot))
                std::printf("screenshot: the window, %s\n", path.string().c_str());
            else
                std::fprintf(stderr, "could not write %s\n", path.string().c_str());
        }
        last_present = Clock::now();
        refreshes++;
        if (fresh) {
            presented++, shown_any = true;
            const Clock::time_point shown_at = Clock::now();
            if (last_fresh != Clock::time_point{}) {
                frame_times[frame_time_at] = std::chrono::duration<float, std::milli>(shown_at - last_fresh).count();
                frame_time_at = (frame_time_at + 1) % FRAME_TIMES;
            }
            last_fresh = shown_at;
        }
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
    std::printf("player: system dialogs taken %llu, answered %llu (the rest cancelled as the player closed)\n",
                (unsigned long long)sysui.taken(), (unsigned long long)sysui.answered());
    sysui.cancel_all();
    // the title that follows: the one this launched (XamLoaderLaunchTitle) when its executable is on the same disc,
    // or this one again when the overlay asked for a restart. It is a process of its own, as a console's titles are:
    // the next player starts on the same disc with the launch data the title left it, and this one waits for it.
    std::vector<std::string> next_args;
    if (restart_requested || (ended.rfind("stopped", 0) == 0 && k.title_exit.launch)) {
        std::string next;
        if (!restart_requested) {
            next = game::disc_path(k.title_exit.path);
            std::printf("the title asks for %s", next.empty() ? "the dashboard" : next.c_str());
            std::printf("%s\n", next.empty() ? "" : settings.follow_launches ? "; starting it" : " (follow_launches is off)");
        }
        if (restart_requested || (!next.empty() && settings.follow_launches)) {
            std::vector<std::pair<std::string, std::string>> replace = {{"--launch-data", ""}, {"--quit-after", ""}, {"--restart-after", ""}};
            if (!restart_requested) replace.push_back({"--xex", next});
            if (!restart_requested && !k.title_exit.launch_data.empty()) {
                std::filesystem::path file = o.storage / "launch-data.bin";
                std::filesystem::create_directories(o.storage, ec);
                std::ofstream out(file, std::ios::binary);
                out.write(reinterpret_cast<const char *>(k.title_exit.launch_data.data()),
                          std::streamsize(k.title_exit.launch_data.size()));
                replace[0].second = file.string();
            }
            if (quit_after > 0) replace[1].second = std::to_string(std::max(1.0, quit_after - seconds));
            next_args = game::relaunch_args(std::vector<std::string>(argv, argv + argc), replace);
        }
    }
    if (!next_args.empty()) {
        k.stop("the player starts the next title");
        SDL_HideWindow(window);
        std::vector<const char *> cargs;
        for (const std::string &a : next_args) cargs.push_back(a.c_str());
        cargs.push_back(nullptr);
        std::fflush(stdout);
        std::fflush(stderr);
        int code = 1;
        if (SDL_Process *process = SDL_CreateProcess(cargs.data(), false)) {
            if (!SDL_WaitProcess(process, true, &code)) code = 1;
        } else {
            std::fprintf(stderr, "player: cannot start %s: %s\n", cargs[0], SDL_GetError());
        }
        std::fflush(stdout);
        std::_Exit(code);
    }
    // the guest's threads still run: no destructors under them (boot's way)
    presenter.idle();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
