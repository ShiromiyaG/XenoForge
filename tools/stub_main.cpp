// The main of the executables that are only XenoForge's player or boot, with
// no port's code of their own: the generic player and boot (the game library
// cached for the disc, else the interpreter), and boot_<game> / player_<game>
// from CMake's XF_GAMES, which link a game's generated code (XF_PORT_LINKED,
// the tables of the game library) and take the same path a port's main does.
// XF_PORT_BOOT makes it boot, else the player.

#include "xenoforge/run.h"

int main(int argc, char **argv) {
    xf::PortInfo port;
#ifdef XF_PORT_LINKED
    return xf::run(argc, argv, port);
#elif defined(XF_PORT_BOOT)
    return xf::run_boot(argc, argv, port);
#else
    return xf::run_player(argc, argv, port);
#endif
}
