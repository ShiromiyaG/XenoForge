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

// One setting of the port, declared once: kept in config.toml under [port]
// (the game's own table when it has one), set by --option, shown in the
// player's overlay (F1) by category with its description, read by hooks in a
// reloaded module (port::option, port.h), and the list an in-game options
// menu is made from (docs/sdk.md, "The port's options"); after the 3DS
// runtime's PORT_OPTION and hedge-dev's recompilations' config_def.h. The port owns the
// variable; the player writes it when the file is read, when the overlay
// changes it and when --option sets it, always before the game starts or from
// the overlay's thread while it runs (a port reads it where a torn read does
// not matter, or keeps an atomic).
//
//   static int aspect = 0;
//   port.options.push_back(xf::PortOption::choice("aspect", "Aspect ratio", aspect, {"auto", "16:9", "21:9"})
//                              .describe("The picture's shape; auto follows the window.")
//                              .in("Video")
//                              .changed([] { apply_aspect(); }));
struct PortOption {
    enum class Kind { Boolean, Integer, Real, Choice };
    std::string key;         // the name in config.toml and --option
    std::string label;       // what the overlay and a menu show
    std::string description; // what it does, a sentence or two (the overlay's tooltip, a menu's text)
    std::string category;    // the group it is shown in ("Video", "Gameplay"); empty: the port's own
    Kind kind = Kind::Boolean;
    bool *flag = nullptr;
    int *integer = nullptr; // an Integer, or a Choice's index
    double *real = nullptr;
    double min = 0, max = 0;                 // the range of an Integer or Real (a Choice: 0, count - 1)
    std::vector<std::string> choices;        // a Choice's values as config.toml and --option name them
    std::vector<std::string> choice_labels;  // and as they are shown (empty: the names)
    double default_value = 0;                // the value when declared (a menu's reset)
    bool restart = false;                    // it takes effect at the next start
    std::function<void()> on_change;         // after the overlay or a menu changed it (the overlay's thread)

    static PortOption boolean(std::string key, std::string label, bool &value) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Boolean, o.flag = &value;
        o.min = 0, o.max = 1, o.default_value = value ? 1 : 0;
        return o;
    }
    static PortOption integer_range(std::string key, std::string label, int &value, int min, int max) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Integer, o.integer = &value;
        o.min = min, o.max = max, o.default_value = value;
        return o;
    }
    static PortOption real_range(std::string key, std::string label, double &value, double min, double max) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Real, o.real = &value;
        o.min = min, o.max = max, o.default_value = value;
        return o;
    }
    // one of named values, its index in `index`; `labels` as they are shown (empty: the names)
    static PortOption choice(std::string key, std::string label, int &index, std::vector<std::string> names,
                             std::vector<std::string> labels = {}) {
        PortOption o;
        o.key = std::move(key), o.label = std::move(label), o.kind = Kind::Choice, o.integer = &index;
        o.choices = std::move(names), o.choice_labels = std::move(labels);
        o.min = 0, o.max = o.choices.empty() ? 0 : double(o.choices.size() - 1), o.default_value = index;
        return o;
    }
    PortOption &describe(std::string text) { return description = std::move(text), *this; }
    PortOption &in(std::string group) { return category = std::move(group), *this; }
    PortOption &needs_restart(bool yes = true) { return restart = yes, *this; }
    PortOption &changed(std::function<void()> fn) { return on_change = std::move(fn), *this; }

    // The value as a number: a boolean 0 or 1, an integer, a real, a choice's index.
    double value() const {
        return kind == Kind::Boolean ? (*flag ? 1.0 : 0.0) : kind == Kind::Real ? *real : double(*integer);
    }
    // Set from a number, clamped to the range (no on_change: the caller's to call).
    void set_value(double v) const {
        if (v < min) v = min;
        if (v > max) v = max;
        if (kind == Kind::Boolean) *flag = v != 0;
        else if (kind == Kind::Real) *real = v;
        else *integer = int(v);
    }
    // a Choice's label for its index (the name where it has no label)
    const std::string &choice_label(size_t k) const {
        return k < choice_labels.size() && !choice_labels[k].empty() ? choice_labels[k] : choices[k];
    }

    // The value set from text ("true", "0", "1.5", a choice's name or index), clamped to the range; false
    // when the text is not one.
    bool set_text(const std::string &text) const {
        if (kind == Kind::Boolean) {
            if (text == "1" || text == "true" || text == "on") return *flag = true, true;
            if (text == "0" || text == "false" || text == "off") return *flag = false, true;
            return false;
        }
        if (kind == Kind::Choice)
            for (size_t k = 0; k < choices.size(); k++)
                if (choices[k] == text) return *integer = int(k), true;
        char *end = nullptr;
        double v = std::strtod(text.c_str(), &end);
        if (text.empty() || *end) return false;
        set_value(v);
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
    // The port's hooks built as a module of their own (xenoforge_add_port's
    // HOOK_SOURCES with HOT_RELOAD; xenoforge/port_module.h): its file (a
    // bare name: beside the executable), loaded after on_init and reloaded
    // when it changes (the player: also F9; boot: --reload-hooks-every S).
    // xf::run sets it from the build. Empty: none.
    std::string hook_module;

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
#ifdef XF_PORT_HOOK_MODULE
    if (port.hook_module.empty()) port.hook_module = XF_PORT_HOOK_MODULE;
#endif
#ifdef XF_PORT_BOOT
    return run_boot(argc, argv, port);
#else
    return run_player(argc, argv, port);
#endif
}
#endif

} // namespace xf
