// The launcher's logic (launcher_core.h).

#include "launcher_core.h"

#include <SDL3/SDL.h>

#include <fstream>

namespace xf::frontend {

std::string utf8(const std::filesystem::path &path) {
    std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path from_utf8(const std::string &text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

namespace {

// how a run ended, from its log: the player's own lines (the reason, the
// frames, the sound), else the last lines (an error before the window)
std::vector<std::string> run_summary(const std::filesystem::path &log) {
    std::ifstream in(log);
    std::vector<std::string> lines, player;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("player: ")) player.push_back(line);
        lines.push_back(line);
    }
    if (!player.empty()) return player;
    if (lines.size() > 4) lines.erase(lines.begin(), lines.end() - 4);
    if (lines.empty()) lines.push_back("(the player wrote nothing)");
    return lines;
}

} // namespace

LauncherCore::LauncherCore(std::filesystem::path config_file, std::filesystem::path players_dir)
    : config_file_(std::move(config_file)), players_dir_(std::move(players_dir)) {
    if (!load(config_file_, shared_, load_error_)) shared_ = Settings();
}

LauncherCore::~LauncherCore() {
    stop_scan();
    if (process_) SDL_DestroyProcess(process_); // the player goes on: only the handle is let go
}

bool LauncherCore::set_games_folder(const std::string &folder, bool save, std::string &error) {
    shared_.launcher.games = folder;
    start_scan();
    if (!save) return true;
    return set_launcher(shared_.launcher, error);
}

bool LauncherCore::set_accent(const std::string &accent, std::string &error) {
    Settings::Launcher l = shared_.launcher;
    l.accent = accent;
    return set_launcher(l, error);
}

bool LauncherCore::set_launcher(const Settings::Launcher &launcher, std::string &error) {
    shared_.launcher = launcher;
    // the file as it is, the launcher's section changed (a game's own tables kept)
    Settings s;
    std::string ignored;
    load(config_file_, s, ignored);
    s.launcher = shared_.launcher, s.screenshots = shared_.screenshots;
    return frontend::save(config_file_, s, error);
}

void LauncherCore::rescan() { start_scan(); }

void LauncherCore::use_demo_games(std::vector<GameEntry> games) {
    demo_games_ = std::move(games);
    start_scan();
}

std::vector<GameEntry> LauncherCore::demo_list() {
    // six titles to see the list with (names, IDs and versions only)
    static const struct {
        const char *title;
        uint32_t id, version;
    } demo[] = {{"Battlefield Bad Co. 2", 0x454108A8, 1}, {"Gears of War 3", 0x4D5309C9, 10}, {"Forza Horizon", 0x4D5309C9, 1},
                {"Halo: Reach", 0x4D53085B, 11},         {"Red Dead Redemption", 0x5454082B, 1}, {"SONIC UNLEASHED", 0x53450812, 1}};
    std::vector<GameEntry> games;
    for (const auto &d : demo) {
        GameEntry g;
        g.title = d.title, g.title_id = d.id, g.version = d.version, g.digest = uint64_t(games.size() + 1);
        g.path = from_utf8(std::string("demo/") + d.title + ".iso");
        games.push_back(std::move(g));
    }
    return games;
}

void LauncherCore::show_demo_games(bool on) {
    stop_scan(); // the scan reads demo_games_
    demo_games_ = on ? demo_list() : std::vector<GameEntry>();
    start_scan();
}

// The folder read on a thread of its own: the players beside the launcher
// asked first, then each game read in turn, the list growing as they come
// (about a second a disc).
void LauncherCore::start_scan() {
    stop_scan();
    {
        std::lock_guard lock(mutex_);
        games_.clear();
        total_ = 0;
    }
    busy_ = true;
    const std::filesystem::path folder = from_utf8(shared_.launcher.games), players_dir = players_dir_;
    if (!demo_games_.empty()) { // listed as they are, nothing read from a disc
        scan_ = std::thread([this, players_dir] {
            auto players = find_players(players_dir);
            std::lock_guard lock(mutex_);
            players_ = std::move(players);
            games_ = demo_games_;
            total_ = games_.size();
            busy_ = false;
        });
        return;
    }
    scan_ = std::thread([this, folder, players_dir] {
        auto players = find_players(players_dir);
        auto paths = folder.empty() ? std::vector<std::filesystem::path>{} : find_games(folder);
        {
            std::lock_guard lock(mutex_);
            players_ = std::move(players);
            total_ = paths.size();
        }
        for (const auto &path : paths) {
            if (cancel_) break;
            GameEntry game = read_game(path);
            std::lock_guard lock(mutex_);
            games_.push_back(std::move(game));
        }
        busy_ = false;
    });
}

void LauncherCore::stop_scan() {
    cancel_ = true;
    if (scan_.joinable()) scan_.join();
    cancel_ = false;
}

LauncherCore::Listing LauncherCore::listing() const {
    Listing l;
    l.busy = busy_; // read first: a list read after the scan ended is whole
    std::lock_guard lock(mutex_);
    l.games = games_;
    l.players = players_;
    l.total = total_;
    return l;
}

bool LauncherCore::load_settings(const GameEntry &game, Settings &settings, std::string &error) const {
    settings = Settings();
    if (load(config_file_, settings, error, game.title_id_text())) return true;
    error = utf8(config_file_) + ": " + error;
    return false;
}

bool LauncherCore::save_settings(Settings &settings, std::string &error) const {
    // the shared parts as the launcher has them, not as they were read
    settings.launcher = shared_.launcher;
    settings.screenshots = shared_.screenshots;
    return save(config_file_, settings, error);
}

bool LauncherCore::set_own_settings(const GameEntry &game, bool own, Settings &settings, std::string &error) const {
    bool ok = true;
    if (own) {
        settings.game = game.title_id_text(); // what it had, now its own
        ok = save_settings(settings, error);
    } else {
        ok = forget_game(config_file_, game.title_id_text(), error);
    }
    std::string why;
    if (!load_settings(game, settings, why) && ok) error = why, ok = false;
    return ok;
}

void LauncherCore::default_keys(Settings &settings) {
    for (const Binding &b : BINDINGS) settings.keys[b.name] = b.keys;
}

std::vector<std::string> LauncherCore::gamepads() {
    std::vector<std::string> names;
    int count = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&count);
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetGamepadNameForID(pads[i]);
        names.push_back(name ? name : "gamepad");
    }
    SDL_free(pads);
    return names;
}

// The player as a process of its own: no console window, its output and
// errors into the log beside the settings, its working directory its own.
bool LauncherCore::start(const GameEntry &game, const LaunchPlan &plan, std::string &error) {
    if (process_) return error = run_.title + " is running", false;
    const std::filesystem::path log = config_file_.parent_path() / "logs" / (game.title_id_text() + ".log");
    std::error_code ec;
    std::filesystem::create_directories(log.parent_path(), ec);
    SDL_IOStream *out = SDL_IOFromFile(utf8(log).c_str(), "w");
    if (!out) return error = std::string("cannot write the log: ") + SDL_GetError(), false;
    const std::string program = utf8(plan.program), input = utf8(game.path), settings = utf8(config_file_);
    std::vector<const char *> args{program.c_str(), input.c_str(), "--config", settings.c_str()};
    for (const std::string &a : plan.args) args.push_back(a.c_str());
    args.push_back(nullptr);
    const std::string cwd = utf8(plan.program.parent_path());
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, out);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_POINTER, out);
    // no console window (SDL's background process; its exit code is not
    // kept, so how it ended comes from the log)
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    process_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_CloseIO(out); // the player has its own handle
    if (!process_) return error = std::string("cannot start ") + program + ": " + SDL_GetError(), false;
    run_ = Run{};
    run_.title = game.title;
    run_.log = log;
    run_.running = true;
    run_.started = std::chrono::steady_clock::now();
    return true;
}

bool LauncherCore::poll() {
    if (!process_ || !SDL_WaitProcess(process_, false, nullptr)) return false;
    SDL_DestroyProcess(process_);
    process_ = nullptr;
    run_.running = false;
    run_.ended = run_summary(run_.log);
    return true;
}

void LauncherCore::stop() {
    if (process_) SDL_KillProcess(process_, true);
}

} // namespace xf::frontend
