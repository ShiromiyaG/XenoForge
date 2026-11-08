// The launcher's games (docs/runtime.md, "The launcher"): the discs (and
// bare executables) in a folder, each read in place for what the launcher
// shows of it, and the players beside the launcher, each asked which
// executables its generated code was made from, so a game starts in the
// player that carries its code.

#pragma once

#include "xenoforge/xex.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace xf::frontend {

struct GameEntry {
    std::filesystem::path path; // the ISO, or a bare default.xex
    std::string title;          // the title's name from its XDBF, else the file's name
    uint32_t title_id = 0;
    uint32_t version = 0, media_id = 0; // the execution info's
    uint64_t digest = 0;                // default.xex's image (xf::image_digest), what generated code is tied to
    uint64_t size = 0;                  // the file's
    std::vector<uint8_t> icon;          // the title's image from its XDBF (a PNG; empty when it has none), read at run time
    std::string error;                  // why it could not be read (then only path is set)
    std::string title_id_text() const;  // "5846000A"
};

// A player executable and the modules it carries generated code for (none:
// the generic player, which interprets).
struct PlayerBinary {
    std::filesystem::path path;
    bool generic = false;                                  // the plain `player`
    std::vector<std::pair<uint64_t, std::string>> modules; // digest, the module's path on the disc
};

// The title's name in a loaded executable's XDBF resource (the one named by
// its title ID), in English or else the XDBF's default language, as Xenia's
// GameInfoDatabase::GetTitleName gives it for an English console; "" when
// there is none.
std::string title_name(const Xex &xex);
// The title's image in the same XDBF (a PNG), or empty.
std::vector<uint8_t> title_icon(const Xex &xex);

// One game read: the disc's default.xex (or the bare executable) parsed and
// unpacked for its title, ID and digest.
GameEntry read_game(const std::filesystem::path &path);

// The ISOs (and bare default.xex files) in a folder and its subfolders one
// level down, sorted by path; each read with read_game.
std::vector<std::filesystem::path> find_games(const std::filesystem::path &folder);

// The players in a directory (the launcher's): player and player_<game>,
// each run with --identify for the modules it carries.
std::vector<PlayerBinary> find_players(const std::filesystem::path &directory);

// The player for a game. With a library cached for its disc (the recompile
// step's, game_build.h) the generic player, which loads it; without one, the
// player carrying the game's default.xex code, else the generic one (which
// then interprets). Null when none fits.
const PlayerBinary *player_for(const std::vector<PlayerBinary> &players, const GameEntry &game, bool cached_library);

// What to start for a game, decided apart from how it is started: the
// program that runs it (the player's executable; where the game runs inside
// the launcher's own process, as on Android, there is none and the same plan
// is what that start reads), whether it loads the cached library of the disc,
// and the arguments after the disc's path. LauncherCore::start is the one
// place that carries a plan out.
struct LaunchPlan {
    std::filesystem::path program; // the player's executable
    bool generic = false;          // the generic player (loads the cached library, else interprets)
    bool from_library = false;     // a library is cached for the disc
    std::vector<std::string> args; // after the disc's path
};
// The plan for a game; false when no player fits (see player_for).
bool plan_launch(const std::vector<PlayerBinary> &players, const GameEntry &game, bool cached_library,
                 const std::vector<std::string> &extra_args, LaunchPlan &plan);

} // namespace xf::frontend
