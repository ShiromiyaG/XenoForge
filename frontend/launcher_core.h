// What the launcher does, apart from how it is drawn (docs/runtime.md, "The
// launcher"): the games folder and its list, read on a thread of its own;
// each game's settings and controls as the player reads them (config.toml,
// the shared ones or the game's own); and a game started in its player, as a
// process of its own, until it ends. No UI code here: a view (the RmlUi
// screens, launcher_view.cpp) calls this each frame and draws what it says.

#pragma once

#include "library.h"
#include "settings.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_Process;

namespace xf::frontend {

class LauncherCore {
  public:
    // config_file: the player's config.toml; players_dir: where player and
    // player_<game> are (the launcher's own directory). Nothing is listed
    // until rescan() or set_games_folder().
    LauncherCore(std::filesystem::path config_file, std::filesystem::path players_dir);
    ~LauncherCore(); // the scan stopped; a game under way goes on running

    const std::filesystem::path &config_file() const { return config_file_; }
    // why the settings file could not be read at the start ("" when it was)
    const std::string &load_error() const { return load_error_; }

    // The games folder (the shared [launcher] games) and its scan.
    const std::string &games_folder() const { return shared_.launcher.games; }
    // A folder to list: rescanned, and saved in config.toml when `save`.
    bool set_games_folder(const std::string &folder, bool save, std::string &error);
    void rescan(); // the players asked again, the games read again
    // The launcher's accent colour ("#RRGGBB", "" for the default), saved in config.toml.
    const std::string &accent() const { return shared_.launcher.accent; }
    bool set_accent(const std::string &accent, std::string &error);

    // The list as the scan has it so far.
    struct Listing {
        std::vector<GameEntry> games;
        std::vector<PlayerBinary> players;
        size_t total = 0;  // games found in the folder (the list grows to it)
        bool busy = false; // still reading
    };
    Listing listing() const;

    // A game's settings as the player will read them (its own when it has
    // them: settings.game is then its title ID).
    bool load_settings(const GameEntry &game, Settings &settings, std::string &error) const;
    // Saved where settings.game says, with the shared parts (the games
    // folder, the screenshots') as the launcher has them.
    bool save_settings(Settings &settings, std::string &error) const;
    // A game given settings of its own (a copy of the ones it had) or put
    // back on the shared ones; settings is read again either way.
    bool set_own_settings(const GameEntry &game, bool own, Settings &settings, std::string &error) const;
    // Xenia's default keyboard bindings put back.
    static void default_keys(Settings &settings);
    // The gamepads connected, by name, in SDL's order (the users the player
    // gives them).
    static std::vector<std::string> gamepads();

    // A game run: the player started on it, its log, how it ended.
    struct Run {
        std::string title;
        std::filesystem::path log;
        bool running = false;
        std::chrono::steady_clock::time_point started;
        std::vector<std::string> ended; // the log's "player:" lines, or its last lines
    };
    // The player on the game, with this settings file and the extra
    // arguments; false and why when it does not start (or one runs already).
    bool start(const GameEntry &game, const PlayerBinary &player, const std::vector<std::string> &extra,
               std::string &error);
    // Checks on the run: true once, when the one under way has ended (then
    // run().ended says how).
    bool poll();
    const Run &run() const { return run_; }
    void stop(); // the run under way ended

  private:
    void start_scan();
    void stop_scan();

    std::filesystem::path config_file_, players_dir_;
    std::string load_error_;
    Settings shared_; // the games folder and the screenshots' folder

    mutable std::mutex mutex_; // the scan's results
    std::vector<GameEntry> games_;
    std::vector<PlayerBinary> players_;
    size_t total_ = 0;
    std::atomic<bool> busy_{false}, cancel_{false};
    std::thread scan_;

    Run run_;
    SDL_Process *process_ = nullptr;
};

// A path as UTF-8 and back (what SDL and config.toml take).
std::string utf8(const std::filesystem::path &path);
std::filesystem::path from_utf8(const std::string &text);

} // namespace xf::frontend
