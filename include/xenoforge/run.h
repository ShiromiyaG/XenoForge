// What a game port's main calls (docs/sdk.md): the player and boot as
// libraries, given the port's description. A port's main is a few lines:
//
//   #include <xenoforge/run.h>
//   int main(int argc, char **argv) {
//       xf::PortInfo port;
//       port.name = "My Game";
//       port.on_init = [](xf::kernel::Kernel &k) { /* register hooks */ };
//       static bool wide = false;
//       port.options.push_back(xf::PortOption::boolean("widescreen", "Widescreen", wide));
//       return xf::run(argc, argv, port);
//   }
//
// xenoforge_add_port compiles the same sources twice: for the player (a window,
// input, sound; the overlay and config.toml carry the port's options) and, with
// XF_PORT_BOOT defined, for boot (headless, for tests and traces), so xf::run
// is whichever the target is. Both link the port's generated code (the tables
// xfrecomp emit writes) and take the command line of the generic player and
// boot (docs/runtime.md), plus --option <key>=<value>, repeatable, for the
// port's options.

#pragma once

#include "xenoforge/kernel.h"
#include "xenoforge/ppc_module.h"

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace xf {

// One setting of the port, shown in the player's overlay (F1) and kept in
// config.toml under [port] (the game's own table when it has one), after the
// 3DS runtime's PORT_OPTION. The port owns the variable; the player writes it
// when the file is read, when the overlay changes it and when --option sets
// it, always before the game starts or from the overlay's thread while it
// runs (a port reads it where a torn read does not matter, or keeps an atomic).
struct PortOption {
    enum class Kind { Boolean, Integer, Real };
    std::string key;   // the name in config.toml and --option
    std::string label; // what the overlay shows
    Kind kind = Kind::Boolean;
    bool *flag = nullptr;
    int *integer = nullptr;
    double *real = nullptr;
    double min = 0, max = 0; // the range of an Integer or Real

    static PortOption boolean(std::string key, std::string label, bool &value) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Boolean, o.flag = &value;
        return o;
    }
    static PortOption integer_range(std::string key, std::string label, int &value, int min, int max) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Integer, o.integer = &value;
        o.min = min, o.max = max;
        return o;
    }
    static PortOption real_range(std::string key, std::string label, double &value, double min, double max) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Real, o.real = &value;
        o.min = min, o.max = max;
        return o;
    }

    // The value set from text ("true", "0", "1.5"), clamped to the range; false
    // when the text is not one.
    bool set_text(const std::string &text) const {
        if (kind == Kind::Boolean) {
            if (text == "1" || text == "true" || text == "on") return *flag = true, true;
            if (text == "0" || text == "false" || text == "off") return *flag = false, true;
            return false;
        }
        char *end = nullptr;
        double v = std::strtod(text.c_str(), &end);
        if (text.empty() || *end) return false;
        if (v < min) v = min;
        if (v > max) v = max;
        if (kind == Kind::Integer)
            *integer = int(v);
        else
            *real = v;
        return true;
    }
};

struct PortInfo {
    std::string name;                  // the window's title and the port's name in the reports
    std::vector<PortOption> options;   // see PortOption
    // Called once the game is opened (the executable loaded, the kernel made,
    // the options read) and before its main thread starts: where a port
    // registers its hooks and replacements.
    std::function<void(kernel::Kernel &)> on_init;
    // Called at the run's end, after the report, while the guest's threads
    // may still run (the process leaves without destructors).
    std::function<void(kernel::Kernel &)> on_shutdown;
    // The generated code linked into the executable (xf::run fills these from
    // the game library the build step made); none: the game library cached for
    // the disc, else the interpreter.
    const ppc::GeneratedModule *const *modules = nullptr;
    size_t module_count = 0;

    // --option key=value: the option set, or false and why
    bool set_option(const std::string &assignment, std::string &why) const {
        size_t eq = assignment.find('=');
        if (eq == std::string::npos) return why = "--option wants <key>=<value>", false;
        std::string key = assignment.substr(0, eq);
        for (const PortOption &o : options)
            if (o.key == key)
                return o.set_text(assignment.substr(eq + 1)) || (why = "bad value for option " + key, false);
        return why = "no option " + key, false;
    }
};

// The player's main (frontend/player.cpp) and boot's (tools/boot.cpp) as
// functions: the generic ones' command lines, then --option. They return the
// exit code (the player and boot end the process themselves once running).
int run_player(int argc, char **argv, PortInfo &port);
int run_boot(int argc, char **argv, PortInfo &port);

} // namespace xf

#ifdef XF_PORT_LINKED
// the generated code's tables (xfrecomp emit's modules.cpp), in the game library the port links
extern "C" const xf::ppc::GeneratedModule *const xf_modules[];
extern "C" const size_t xf_module_count;
#endif

namespace xf {

// The port's main, for whichever executable xenoforge_add_port is building
// (XF_PORT_BOOT: boot; else the player), with the game library's tables.
#ifdef XF_PORT_LINKED
inline int run(int argc, char **argv, PortInfo &port) {
    port.modules = xf_modules, port.module_count = xf_module_count;
#ifdef XF_PORT_BOOT
    return run_boot(argc, argv, port);
#else
    return run_player(argc, argv, port);
#endif
}
#endif

} // namespace xf
