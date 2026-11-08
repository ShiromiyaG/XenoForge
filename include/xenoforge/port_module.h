// A port's hooks as a module loaded at run time, so they can be reloaded
// while the game runs (docs/sdk.md, "Reloading the hooks"; plan E5):
// xenoforge_add_port(... HOOK_SOURCES <files> HOT_RELOAD) builds those
// sources into a DLL (a .so on POSIX) beside the executables, and the player
// and boot load it as the game opens and load it again when it changes.
//
// The module's side is xenoforge/port.h as ever: XF_REPLACE, XF_BEFORE, ...
// and the run-time calls. Built with XF_PORT_MODULE, those reach the runtime
// through a table of C functions (ModuleApi) the runtime hands the module's
// one export, so the module links nothing of XenoForge; its static
// registrations, made as it is loaded, wait for that table. The module's own
// code is compiled into it (xenoforge_add_port writes a source that defines
// XF_PORT_MODULE_ENTRY and includes this header).
//
// The runtime's side (PortModule, src/kernel/port_module.cpp): the module
// copied to a name of its own (so the build can write the file again) and
// loaded; on a reload, the new copy loaded and checked first, then every
// guest thread parked at a safe point (Kernel::pause), the old module's
// hooks dropped and the new one's registered, the threads let go; the old
// copy is unloaded once no thread runs its code (hooks::drained: a thread
// that was inside one of its hooks, waiting in a kernel call made by guest
// code the hook called, keeps it loaded until it comes back out).
//
// What survives a reload is what the runtime keeps: the registry's counts
// (--hook-stats), the names, and the blocks of port::persistent(). What does
// not: the module's statics, its heap (a pointer kept in a persistent block
// to the module's heap dangles after a reload), function pointers into it
// held anywhere but the registry, and threads it started (a module must not
// leave any running).

#pragma once

#include "xenoforge/port.h"

#include <cstddef>
#include <cstdint>

namespace xf::port {

// The table the runtime hands a module: C types only, so a module and the
// runtime need not share a C++ library. `owner` goes back with each call.
constexpr uint32_t MODULE_API_VERSION = 1;
struct ModuleApi {
    uint32_t version; // MODULE_API_VERSION
    uint32_t size;    // sizeof(ModuleApi)
    void *owner;
    void (*replace)(void *owner, uint32_t address, Replacement fn, const char *name);
    void (*before)(void *owner, uint32_t address, Callback fn, const char *name);
    void (*after)(void *owner, uint32_t address, Callback fn, const char *name);
    void (*mid)(void *owner, uint32_t address, MidHook fn, const char *name);
    void (*remove)(void *owner, uint32_t address);
    void (*name)(uint32_t address, const char *name);
    // the name's length; up to size - 1 bytes of it, and a 0, written to out
    size_t (*name_of)(uint32_t address, char *out, size_t size);
    void (*original)(Context &c, uint32_t address, void (*fn)(Context &));
    void (*call)(Context &c, uint32_t address);
    void *(*persistent)(const char *key, size_t size, bool *made);
};

} // namespace xf::port

// a module's exports: its interface version (checked before anything is
// registered), and the entry that registers its hooks
#define XF_PORT_MODULE_VERSION_NAME "xf_port_module_version"
#define XF_PORT_MODULE_ENTRY_NAME "xf_port_module"
using XfPortModuleVersion = uint32_t (*)();
using XfPortModuleEntry = bool (*)(const xf::port::ModuleApi *api);

#ifndef XF_PORT_MODULE
#include <filesystem>
#include <memory>
#include <string>

namespace xf::kernel {

class Kernel;

class PortModule {
  public:
    PortModule();
    ~PortModule();
    // The module at `path`, loaded and its hooks registered (before the game
    // runs: nothing is paused). False, and why, when it cannot be.
    bool load(const std::filesystem::path &path, std::string &error);
    // The file changed since it was last loaded and has stayed the same for
    // `settle_ms` (a build still writing it is waited for).
    bool changed(uint32_t settle_ms = 500);
    // Loads the file again while the game runs: the new copy loaded and
    // checked, then the guest paused (up to pause_ms), the hooks swapped, the
    // guest let go. False, and why, when the new copy cannot be used (the old
    // hooks stay). `report` gets a line for the log.
    bool reload(Kernel &k, std::string &error, std::string *report = nullptr, uint32_t pause_ms = 1000);
    // The copies replaced by a reload that no thread runs any more,
    // unloaded (each reload does this too); how many are still loaded.
    size_t collect();
    uint32_t reloads() const;
    const std::filesystem::path &path() const;

  private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace xf::kernel
#endif

#ifdef XF_PORT_MODULE_ENTRY
// The module's side: port.h's functions through the table, the static
// registrations made before it came kept until then. Compiled once into
// each module (xenoforge_add_port's port_module_entry.cpp).
#ifndef XF_PORT_MODULE
#error "a port module is compiled with XF_PORT_MODULE"
#endif
#if defined(_WIN32)
#define XF_PORT_MODULE_EXPORT __declspec(dllexport)
#else
#define XF_PORT_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#include <string>
#include <vector>

namespace xf::port {
namespace {

const ModuleApi *module_api = nullptr;

struct Waiting {
    int kind; // 0 replace, 1 before, 2 after, 3 mid
    uint32_t address;
    void *fn;
    const char *name;
};
std::vector<Waiting> &waiting() {
    static std::vector<Waiting> w;
    return w;
}

} // namespace

void replace(uint32_t address, Replacement fn, const char *name) {
    if (module_api) return module_api->replace(module_api->owner, address, fn, name);
    waiting().push_back({0, address, reinterpret_cast<void *>(fn), name});
}
void before(uint32_t address, Callback fn, const char *name) {
    if (module_api) return module_api->before(module_api->owner, address, fn, name);
    waiting().push_back({1, address, reinterpret_cast<void *>(fn), name});
}
void after(uint32_t address, Callback fn, const char *name) {
    if (module_api) return module_api->after(module_api->owner, address, fn, name);
    waiting().push_back({2, address, reinterpret_cast<void *>(fn), name});
}
void mid(uint32_t address, MidHook fn, const char *name) {
    if (module_api) return module_api->mid(module_api->owner, address, fn, name);
    waiting().push_back({3, address, reinterpret_cast<void *>(fn), name});
}
void remove(uint32_t address) {
    if (module_api) module_api->remove(module_api->owner, address);
}
void name(uint32_t address, const std::string &name) {
    if (module_api) module_api->name(address, name.c_str());
}
std::string name_of(uint32_t address) {
    if (!module_api) return {};
    char text[256];
    const size_t n = module_api->name_of(address, text, sizeof(text));
    if (n < sizeof(text)) return text;
    std::string out(n + 1, '\0');
    module_api->name_of(address, out.data(), out.size());
    out.resize(n);
    return out;
}
void Original::operator()(Context &c) const { module_api->original(c, address_, fn_); }
void call(Context &c, uint32_t address) { module_api->call(c, address); }
void *persistent(const char *key, size_t size, bool *made) { return module_api->persistent(key, size, made); }

} // namespace xf::port

extern "C" XF_PORT_MODULE_EXPORT uint32_t xf_port_module_version() { return ::xf::port::MODULE_API_VERSION; }

extern "C" XF_PORT_MODULE_EXPORT bool xf_port_module(const ::xf::port::ModuleApi *api) {
    using namespace ::xf::port;
    if (!api || api->version != MODULE_API_VERSION || api->size != sizeof(ModuleApi)) return false;
    module_api = api;
    for (const Waiting &w : waiting()) {
        switch (w.kind) {
        case 0: replace(w.address, reinterpret_cast<Replacement>(w.fn), w.name); break;
        case 1: before(w.address, reinterpret_cast<Callback>(w.fn), w.name); break;
        case 2: after(w.address, reinterpret_cast<Callback>(w.fn), w.name); break;
        default: mid(w.address, reinterpret_cast<MidHook>(w.fn), w.name); break;
        }
    }
    waiting().clear();
    return true;
}
#endif
