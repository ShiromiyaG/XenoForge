// The smallest port: XenoForge's player (or boot, which xenoforge_add_port
// builds from this same file with XF_PORT_BOOT) with a name, one option and
// the two callbacks a port's code goes in.

#include <xenoforge/run.h>

#include <cstdio>

namespace {
bool g_verbose = false; // --option verbose=true, or [port] verbose in config.toml, or the overlay (F1)
}

int main(int argc, char **argv) {
    xf::PortInfo port;
    port.name = "Minimal port";
    port.options.push_back(xf::PortOption::boolean("verbose", "Say when the game starts", g_verbose));
    // where a real port registers its hooks and replacements (xenoforge/port.h;
    // docs/recompiler.md, "Replacing and hooking"), for instance
    //   xf::port::before(0x82012340, [](xf::port::Context &c) { ... }, "Camera_Update");
    // with 0x82012340 in project.toml's `hooks` so the generated code's direct
    // calls reach it too
    port.on_init = [](xf::kernel::Kernel &) {
        if (g_verbose) std::printf("minimal port: starting\n");
    };
    port.on_shutdown = [](xf::kernel::Kernel &) {
        if (g_verbose) std::printf("minimal port: done\n");
    };
    return xf::run(argc, argv, port);
}
