// launcher: the games in a folder, their settings and controls, and a start
// button (plan track C2; docs/runtime.md, "The launcher"). This file is its
// view, a form in Dear ImGui on an SDL_Renderer with the system's folder
// dialog, after the 3DS runtime's frontend/launcher.cpp; what it does is
// LauncherCore's (launcher_core.h), which has no UI code: the games are the
// folder's ISOs (and bare default.xex files), read in place for their title,
// title ID and digest (library.h); each starts in the player beside the
// launcher that carries its code (player_<game>, asked with --identify),
// else in the generic player, which interprets; the settings are the
// player's config.toml (settings.h), the shared ones or a game's own, which
// the player then reads for it; the player runs as a process of its own with
// its output in a log beside the settings (logs/<title ID>.log), and the
// launcher stays and shows how the run ended.
//
// For checks without a person at the screen: --list prints the folder's
// games and the player each would start in, without a window; --quit-after S
// ends the launcher after S seconds (once a game it started has ended) and
// --screenshot <png> saves its window's last picture; --select N selects the
// Nth game and --controls opens its controls' tab; --start N starts the
// Nth game as the button does, and --player-arg <arg> (repeatable) adds an
// argument to the player's command line (--quit-after 20, say).
//
//   launcher [--config <file>] [--games <folder>] [--list] [--select N] [--controls] [--start N]
//            [--player-arg <arg>]... [--quit-after S] [--screenshot <png>]

#include "launcher_core.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// a folder the system's dialog picked, handed over from SDL's callback
// (the 3DS runtime's way)
struct Picked {
    std::mutex lock;
    std::string path;
    bool ready = false;
};
Picked picked;

void SDLCALL on_picked(void *, const char *const *files, int) {
    std::lock_guard guard(picked.lock);
    if (files && files[0]) picked.path = files[0], picked.ready = true;
}

const char *USER_NAMES[] = {"1", "2", "3", "4"};

bool write_screenshot(SDL_Renderer *renderer, const std::filesystem::path &path) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return false;
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888); // bytes R, G, B, A
    SDL_DestroySurface(surface);
    if (!rgba) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const bool ok = stbi_write_png(utf8(path).c_str(), rgba->w, rgba->h, 4, rgba->pixels, rgba->pitch) != 0;
    SDL_DestroySurface(rgba);
    return ok;
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
    std::filesystem::path config_file, screenshot;
    std::string games_arg;
    bool list = false, games_given = false, controls_tab = false;
    double quit_after = 0;
    long select = -1, start_index = -1;
    std::vector<std::string> player_args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc)
            config_file = from_utf8(argv[++i]);
        else if (a == "--games" && i + 1 < argc)
            games_arg = argv[++i], games_given = true;
        else if (a == "--list")
            list = true;
        else if (a == "--controls")
            controls_tab = true;
        else if (a == "--select" && i + 1 < argc)
            select = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--start" && i + 1 < argc)
            start_index = select = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--player-arg" && i + 1 < argc)
            player_args.push_back(argv[++i]);
        else if (a == "--quit-after" && i + 1 < argc)
            quit_after = std::strtod(argv[++i], nullptr);
        else if (a == "--screenshot" && i + 1 < argc)
            screenshot = from_utf8(argv[++i]);
        else
            std::fprintf(stderr, "launcher: %s ignored\n", a.c_str());
    }
    if (config_file.empty()) config_file = frontend::default_path();
    std::filesystem::path players_dir;
    if (const char *base = SDL_GetBasePath()) players_dir = from_utf8(base);

    frontend::LauncherCore core(config_file, players_dir);
    if (!core.load_error().empty())
        std::fprintf(stderr, "launcher: %s: %s (the defaults instead)\n", utf8(config_file).c_str(),
                     core.load_error().c_str());
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
            const frontend::PlayerBinary *p = frontend::player_for(l.players, g);
            std::printf("  %s  %s  v%08X  digest %016llx  %s -> %s\n", g.title_id_text().c_str(), g.title.c_str(),
                        g.version, (unsigned long long)g.digest, utf8(g.path).c_str(),
                        p ? utf8(p->path.filename()).c_str() : "(no player)");
        }
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail(std::string("SDL: ") + SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("XenoForge", 1100, 700, SDL_WINDOW_RESIZABLE);
    if (!window) return fail(std::string("SDL: ") + SDL_GetError());
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) return fail(std::string("SDL: ") + SDL_GetError());
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing beside config.toml
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    if (float scale = SDL_GetWindowDisplayScale(window); scale > 0) {
        ImGui::GetStyle().ScaleAllSizes(scale);
        io.FontGlobalScale = scale;
    }
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    char folder_text[1024];
    SDL_strlcpy(folder_text, core.games_folder().c_str(), sizeof(folder_text));

    // the selected game and its settings as the player will read them
    long selected = -1;
    uint32_t edit_title = 0;
    frontend::Settings edit;
    int capturing = -1; // the binding waiting for a key, or -1
    const Clock::time_point began = Clock::now();
    bool quit = false;
    auto load_edit = [&](const frontend::GameEntry &g) {
        core.load_settings(g, edit, message);
        edit_title = g.title_id;
    };
    auto save_edit = [&] { core.save_settings(edit, message); };
    auto start_game = [&](const frontend::GameEntry &g, const frontend::PlayerBinary *p) {
        if (!p) return void(message = "no player beside the launcher");
        if (!core.start(g, *p, player_args, message)) return;
        message.clear();
        std::printf("launcher: started %s on %s, its output in %s\n", utf8(p->path.filename()).c_str(),
                    utf8(g.path).c_str(), utf8(core.run().log).c_str());
    };
    auto new_folder = [&](const std::string &folder) {
        SDL_strlcpy(folder_text, folder.c_str(), sizeof(folder_text));
        if (!core.set_games_folder(folder, true, message)) std::fprintf(stderr, "launcher: %s\n", message.c_str());
        selected = -1;
    };

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
            // a binding waiting for its key takes it (Escape gives up)
            if (capturing >= 0 && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode != SDL_SCANCODE_ESCAPE) {
                    edit.keys[frontend::BINDINGS[capturing].name] = SDL_GetScancodeName(event.key.scancode);
                    save_edit();
                }
                capturing = -1;
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&event);
        }
        {
            std::lock_guard guard(picked.lock);
            if (picked.ready) {
                picked.ready = false;
                new_folder(picked.path);
            }
        }
        // a run's end: how it went, and the settings again (its overlay may
        // have changed them)
        if (core.poll()) {
            const auto &run = core.run();
            const double seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
            std::printf("launcher: %s ended after %.1f s\n", run.title.c_str(), seconds);
            for (const std::string &line : run.ended) std::printf("  %s\n", line.c_str());
            edit_title = 0; // read again below
        }
        const frontend::LauncherCore::Run &run = core.run();

        // the list as the scan has it so far
        const frontend::LauncherCore::Listing listing = core.listing();
        const auto &games = listing.games;
        const auto &players = listing.players;
        if (select >= 0 && select < long(games.size())) selected = select, select = -1;
        if (!listing.busy && start_index >= long(games.size())) { // --start past the list: nothing to start
            std::fprintf(stderr, "launcher: no game %ld to start (%zu listed)\n", start_index, games.size());
            start_index = select = -1;
        }
        if (selected >= long(games.size())) selected = -1;
        const frontend::GameEntry *game = selected >= 0 ? &games[size_t(selected)] : nullptr;
        if (game && !game->error.empty()) game = nullptr;
        if (game && edit_title != game->title_id) load_edit(*game);
        const frontend::PlayerBinary *player = game ? frontend::player_for(players, *game) : nullptr;
        if (start_index >= 0 && game && selected == start_index && !run.running && run.ended.empty()) {
            start_index = -1;
            start_game(*game, player);
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *view = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(view->WorkPos);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::Begin("Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        // the folder
        ImGui::TextUnformatted("Games folder");
        ImGui::SetNextItemWidth(-200);
        if (ImGui::InputText("##folder", folder_text, sizeof(folder_text), ImGuiInputTextFlags_EnterReturnsTrue))
            new_folder(folder_text);
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(95, 0)))
            SDL_ShowOpenFolderDialog(on_picked, nullptr, window,
                                     core.games_folder().empty() ? nullptr : core.games_folder().c_str(), false);
        ImGui::SameLine();
        if (ImGui::Button("Rescan", ImVec2(-1, 0))) {
            core.rescan();
            selected = -1;
        }

        // the games
        const float list_height = ImGui::GetContentRegionAvail().y * 0.42f;
        if (ImGui::BeginTable("games", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_Resizable,
                              ImVec2(0, list_height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            // the fixed columns as wide as their longest content, which the
            // first frames (the scan just begun) do not have yet
            const float pad = ImGui::GetStyle().CellPadding.x * 2;
            ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("DDDDDDDD").x + pad);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("15.15.65535.255").x + pad);
            ImGui::TableSetupColumn("Runs in", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("player (interpreted)").x + pad);
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < games.size(); i++) {
                const frontend::GameEntry &g = games[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(int(i));
                const std::string name = g.error.empty() ? g.title : "(cannot be read)";
                if (ImGui::Selectable(name.c_str(), selected == long(i),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected = long(i);
                    if (ImGui::IsMouseDoubleClicked(0) && g.error.empty() && !run.running)
                        start_game(g, frontend::player_for(players, g));
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (g.error.empty()) ImGui::TextUnformatted(g.title_id_text().c_str());
                ImGui::TableNextColumn();
                if (g.error.empty())
                    ImGui::Text("%u.%u.%u.%u", g.version >> 28, (g.version >> 24) & 15, (g.version >> 8) & 0xFFFF,
                                g.version & 0xFF);
                ImGui::TableNextColumn();
                if (g.error.empty()) {
                    const frontend::PlayerBinary *p = frontend::player_for(players, g);
                    ImGui::TextUnformatted(!p           ? "-"
                                           : p->generic ? "player (interpreted)"
                                                        : utf8(p->path.stem()).c_str());
                } else {
                    ImGui::TextDisabled("%s", g.error.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", utf8(g.path).c_str());
            }
            ImGui::EndTable();
        }
        if (listing.busy)
            ImGui::TextDisabled("Reading the games: %zu of %zu...", games.size(), listing.total);
        else if (core.games_folder().empty())
            ImGui::TextDisabled("Pick the folder with your game discs (.iso). They are read where they are.");
        else
            ImGui::TextDisabled("%zu games; %zu players beside the launcher", games.size(), players.size());

        // the selected game: its settings and controls, and the start button
        if (game) {
            ImGui::SeparatorText(game->title.c_str());
            bool own = !edit.game.empty();
            if (ImGui::Checkbox("Settings of its own for this game", &own))
                core.set_own_settings(*game, own, edit, message);
            ImGui::SameLine();
            if (own)
                ImGui::TextDisabled("(only %s reads these)", game->title_id_text().c_str());
            else
                ImGui::TextDisabled("(the settings every game shares)");
            bool changed = false;
            // the tabs scroll in what the start row and the status lines
            // below leave
            const int footer_lines = 2 + !message.empty() +
                                     (run.running         ? 1
                                      : run.ended.empty() ? 0
                                                          : 1 + int(run.ended.size()));
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y +
                                 footer_lines * ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("settings", ImVec2(0, -footer));
            if (ImGui::BeginTabBar("game")) {
                if (ImGui::BeginTabItem("Settings")) {
                    ImGui::SeparatorText("Video");
                    changed |= ImGui::Checkbox("Fullscreen (F11 in the game)", &edit.video.fullscreen);
                    changed |= ImGui::Checkbox("Vertical sync (the window's only; the game's pace is its own)",
                                               &edit.video.vsync);
                    changed |= ImGui::Checkbox("Smooth scaling", &edit.video.smooth);
                    int aspect = edit.video.aspect == "stretch" ? 1 : 0;
                    const char *aspects[] = {"The picture's own shape (letterboxed)", "Stretched to the window"};
                    ImGui::SetNextItemWidth(320);
                    if (ImGui::Combo("Shape", &aspect, aspects, 2))
                        edit.video.aspect = aspect ? "stretch" : "auto", changed = true;
                    ImGui::SeparatorText("Sound");
                    ImGui::SetNextItemWidth(320);
                    changed |= ImGui::SliderInt("Volume", &edit.audio.volume, 0, 100, "%d%%");
                    changed |= ImGui::Checkbox("Mute", &edit.audio.mute);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls", nullptr,
                                        controls_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                    controls_tab = false;
                    ImGui::SeparatorText("Controllers");
                    const std::vector<std::string> pads = frontend::LauncherCore::gamepads();
                    if (pads.empty()) ImGui::TextDisabled("No gamepad connected.");
                    for (size_t i = 0; i < pads.size(); i++) ImGui::Text("User %zu: %s", i + 1, pads[i].c_str());
                    changed |= ImGui::Checkbox("Vibration", &edit.input.vibration);
                    double deadzone[2] = {edit.input.left_deadzone * 100, edit.input.right_deadzone * 100};
                    const double lo = 0, hi = 100;
                    ImGui::SetNextItemWidth(320);
                    if (ImGui::SliderScalar("Left stick deadzone", ImGuiDataType_Double, &deadzone[0], &lo, &hi,
                                            "%.0f%%"))
                        edit.input.left_deadzone = deadzone[0] / 100, changed = true;
                    ImGui::SetNextItemWidth(320);
                    if (ImGui::SliderScalar("Right stick deadzone", ImGuiDataType_Double, &deadzone[1], &lo, &hi,
                                            "%.0f%%"))
                        edit.input.right_deadzone = deadzone[1] / 100, changed = true;
                    ImGui::SeparatorText("Keyboard");
                    changed |= ImGui::Checkbox("The keyboard as a controller, for user", &edit.input.keyboard);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    changed |= ImGui::Combo("##user", &edit.input.keyboard_user, USER_NAMES, 4);
                    ImGui::SameLine();
                    if (ImGui::Button("Xenia's keys")) frontend::LauncherCore::default_keys(edit), changed = true;
                    ImGui::TextDisabled("Keys by SDL's names, space-separated; ^ before one: only with Shift, _: "
                                        "only without. Set takes the next key pressed (Escape cancels).");
                    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp)) {
                        for (int column = 0; column < 2; column++)
                            ImGui::TableSetupColumn(column ? "b" : "a", ImGuiTableColumnFlags_WidthStretch);
                        const int rows = (25 + 1) / 2;
                        for (int row = 0; row < rows; row++) {
                            ImGui::TableNextRow();
                            for (int column = 0; column < 2; column++) {
                                ImGui::TableNextColumn();
                                const int b = row + column * rows;
                                if (b >= 25) continue;
                                const frontend::Binding &binding = frontend::BINDINGS[b];
                                ImGui::PushID(b);
                                char keys[128];
                                SDL_strlcpy(keys, edit.keys[binding.name].c_str(), sizeof(keys));
                                ImGui::SetNextItemWidth(120);
                                if (ImGui::InputText("##keys", keys, sizeof(keys)))
                                    edit.keys[binding.name] = keys, changed = true;
                                ImGui::SameLine();
                                if (ImGui::Button(capturing == b ? "...##set" : "Set", ImVec2(40, 0))) capturing = b;
                                ImGui::SameLine();
                                ImGui::TextUnformatted(binding.name);
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            if (changed) save_edit();

            ImGui::Separator();
            ImGui::BeginDisabled(run.running || !player);
            if (ImGui::Button("Start", ImVec2(160, 0))) start_game(*game, player);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (!player)
                ImGui::TextDisabled("No player beside the launcher.");
            else
                ImGui::TextDisabled("in %s%s", utf8(player->path.filename()).c_str(),
                                    player->generic ? " (no generated code for this game: interpreted, slow)" : "");
        } else if (selected >= 0 && selected < long(games.size())) {
            ImGui::SeparatorText("This file");
            ImGui::TextWrapped("%s: %s", utf8(games[size_t(selected)].path).c_str(),
                               games[size_t(selected)].error.c_str());
        }

        // the run under way, or how the last one ended
        if (run.running) {
            ImGui::Text("%s is running (%.0f s); its output goes to %s", run.title.c_str(),
                        std::chrono::duration<double>(Clock::now() - run.started).count(), utf8(run.log).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) core.stop();
        } else if (!run.ended.empty()) {
            ImGui::Text("%s ended (the log: %s):", run.title.c_str(), utf8(run.log).c_str());
            for (const std::string &line : run.ended) ImGui::TextDisabled("%s", line.c_str());
        }
        if (!message.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", message.c_str());
        ImGui::TextDisabled("Settings: %s", utf8(config_file).c_str());
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        // the end asked for: after the time, once the game it started is over
        const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
        const bool ending = quit_after > 0 && seconds >= quit_after && !run.running && !listing.busy && start_index < 0;
        if ((ending || quit) && !screenshot.empty()) {
            if (write_screenshot(renderer, screenshot))
                std::printf("launcher: screenshot %s\n", utf8(screenshot).c_str());
            else
                std::fprintf(stderr, "launcher: no screenshot: %s\n", SDL_GetError());
        }
        SDL_RenderPresent(renderer);
        if (ending) quit = true;
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(20); // a form, not a game
    }

    std::printf("launcher: %zu games listed\n", core.listing().games.size());
    if (core.run().running) std::printf("launcher: %s goes on running\n", core.run().title.c_str());
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
