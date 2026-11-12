// A game library loaded (xenoforge/game_library.h).

#include "xenoforge/game_library.h"

#include "xenoforge/cpu_level.h"
#include "xenoforge/game_interface.h" // XF_GAME_INTERFACE_VERSION, made by the build

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xf::ppc {

uint32_t game_interface_version() { return XF_GAME_INTERFACE_VERSION; }

const GameLibrary *load_game_library(const std::filesystem::path &path, const Runtime &runtime, std::string &error) {
    GameLibraryEntry entry = nullptr;
#ifdef _WIN32
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (!module) {
        error = path.string() + ": cannot be loaded (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    entry = reinterpret_cast<GameLibraryEntry>(GetProcAddress(module, XF_GAME_LIBRARY_ENTRY_NAME));
    const void *level = reinterpret_cast<const void *>(GetProcAddress(module, XF_GAME_LIBRARY_CPU_LEVEL_NAME));
    auto unload = [&] { FreeLibrary(module); };
#else
    void *module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        const char *why = dlerror();
        error = path.string() + ": cannot be loaded (" + (why ? why : "?") + ")";
        return nullptr;
    }
    entry = reinterpret_cast<GameLibraryEntry>(dlsym(module, XF_GAME_LIBRARY_ENTRY_NAME));
    const void *level = dlsym(module, XF_GAME_LIBRARY_CPU_LEVEL_NAME);
    auto unload = [&] { dlclose(module); };
#endif
    if (!entry) {
        unload();
        error = path.string() + ": not a game library (no " XF_GAME_LIBRARY_ENTRY_NAME ")";
        return nullptr;
    }
    // Its CPU level, before any of its code runs (code built for a level
    // this CPU lacks stops at its first instruction the CPU does not have).
    // A library without one is from before the levels: another interface.
    if (!level) {
        unload();
        char why[96];
        std::snprintf(why, sizeof(why), ": built for another interface version than this one (%08X)", runtime.version);
        error = path.string() + why;
        return nullptr;
    }
    const CpuLevel built_for = CpuLevel(*static_cast<const uint32_t *>(level));
    if (!cpu_has_level(built_for)) {
        unload();
        error = path.string() + ": built for the CPU level " + cpu_level_name(built_for) + ", which this CPU lacks (" +
                cpu_level_name(host_cpu_level()) + ")";
        return nullptr;
    }
    const GameLibrary *library = entry(&runtime);
    if (!library || library->version != runtime.version) {
        unload();
        char why[96];
        std::snprintf(why, sizeof(why), ": built for another interface version than this one (%08X)", runtime.version);
        error = path.string() + why;
        return nullptr;
    }
    return library;
}

} // namespace xf::ppc
