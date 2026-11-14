// The main of the executables that are only XenoForge's player or boot, with
// no port's code of their own: the generic player and boot (the game library
// cached for the disc, else the interpreter), and boot_<game> / player_<game>
// from CMake's XF_GAMES, which link a game's generated code (XF_PORT_LINKED,
// the tables of the game library) and take the same path a port's main does.
// XF_PORT_BOOT makes it boot, else the player. The generic ones turn on the
// game library and the first tier (tools/game_tiers.cpp), which a port's
// executables leave out.

#include "xenoforge/run.h"

#ifndef XF_PORT_LINKED
namespace xf::game {
void enable_tiers(); // tools/game_tiers.cpp: the game library and the first tier
}
#endif

int main(int argc, char **argv) {
    xf::PortInfo port;
#ifdef XF_PORT_LINKED
    return xf::run(argc, argv, port);
#else
    xf::game::enable_tiers();
#endif
#if defined(XF_PORT_BOOT)
    return xf::run_boot(argc, argv, port);
#else
    return xf::run_player(argc, argv, port);
#endif
}
