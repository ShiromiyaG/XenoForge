// A game library (plan D1; docs/runtime.md, "The game library"): a disc's
// generated code built into a DLL (a .so on Linux) by the recompile step on
// the player's machine (game_build.h), found in the cache by its
// executable's digest and loaded by the generic player and boot. Ports
// (boot_<game>, player_<game>, the SDK) link the same code statically.
//
// The interface is a C table each way:
// - the library exports one function, xf_game_library(const Runtime *): it
//   takes the runtime's table (ppc_runtime.h: dispatch, invalid, unwind,
//   switch_stack, resumable and the C library's pieces), keeps a copy, and
//   gives back its modules' tables (ppc_module.h) with the interface version
//   it was built for; nullptr when the runtime's version is another one;
// - it imports nothing: no C or C++ runtime, no system library. The guest's
//   memory and the system's callbacks come in each Context.
// The interface version is a digest of what makes a library (the generated
// code's headers, the recompiler, the build's flags; made by
// cmake/game_interface.cmake), so a library is only used by a XenoForge whose
// headers and recompiler made it.
//
// An Unwind (ppc_module.h) crosses the library as a C++ exception the
// runtime throws (unwind(), the kernel's dispatch) and catches (the kernel's
// dispatch, resumable()). It only passes through the library's frames, which
// have their unwind tables (.pdata on Windows, .eh_frame on Linux) and no
// handlers: the library needs no C++ runtime of its own.

#pragma once

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_runtime.h"

namespace xf::ppc {

// what a game library gives back
struct GameLibrary {
    uint32_t version; // the interface version it was built for
    const GeneratedModule *const *modules;
    size_t count;
};
using GameLibraryEntry = const GameLibrary *(*)(const Runtime *runtime);

} // namespace xf::ppc

// the library's one export
#define XF_GAME_LIBRARY_ENTRY_NAME "xf_game_library"
// and the CPU level it was built for (a uint32_t: cpu_level.h's CpuLevel),
// which the loader reads before it runs any of the library's code
#define XF_GAME_LIBRARY_CPU_LEVEL_NAME "xf_game_library_cpu_level"

#ifdef XF_GAME_LIBRARY_ENTRY
// The library's entry and the C library's symbols the compiler may call on
// its own (copies, fills), compiled once into each game library: the build
// writes a library.cpp that defines XF_GAME_LIBRARY_ENTRY and includes this.
#ifndef XF_GAME_INTERFACE_VERSION
#error "a game library is compiled with XF_GAME_INTERFACE_VERSION (game_build.cpp)"
#endif
#if defined(_WIN32)
#define XF_GAME_LIBRARY_EXPORT __declspec(dllexport)
#else
#define XF_GAME_LIBRARY_EXPORT __attribute__((visibility("default")))
#endif

// every module's table (xfrecomp emit's modules.cpp)
extern "C" const ::xf::ppc::GeneratedModule *const xf_modules[];
extern "C" const size_t xf_module_count;

namespace xf::ppc {
Runtime runtime;
GameLibrary game_library;
} // namespace xf::ppc

// the CPU level, from what the compiler was told it may use (the build's
// -march: game_build_flags.h), numbered as cpu_level.h's CpuLevel
extern "C" XF_GAME_LIBRARY_EXPORT const uint32_t xf_game_library_cpu_level =
#if defined(__aarch64__)
    0x100;
#elif defined(__AVX2__) && defined(__FMA__) && defined(__BMI2__) && defined(__LZCNT__) && defined(__MOVBE__)
    3;
#elif defined(__SSE4_2__) && defined(__POPCNT__)
    2;
#else
    0;
#endif

extern "C" XF_GAME_LIBRARY_EXPORT const ::xf::ppc::GameLibrary *xf_game_library(const ::xf::ppc::Runtime *runtime) {
    if (!runtime || runtime->version != XF_GAME_INTERFACE_VERSION || runtime->size != sizeof(::xf::ppc::Runtime))
        return nullptr;
    // copied inline: memcpy is the table's
    __builtin_memcpy_inline(&::xf::ppc::runtime, runtime, sizeof(::xf::ppc::Runtime));
    ::xf::ppc::game_library.version = XF_GAME_INTERFACE_VERSION;
    ::xf::ppc::game_library.modules = xf_modules;
    ::xf::ppc::game_library.count = xf_module_count;
    return &::xf::ppc::game_library;
}

extern "C" {
void *memcpy(void *to, const void *from, size_t n) { return ::xf::ppc::runtime.memcpy(to, from, n); }
void *memmove(void *to, const void *from, size_t n) { return ::xf::ppc::runtime.memmove(to, from, n); }
void *memset(void *to, int value, size_t n) { return ::xf::ppc::runtime.memset(to, value, n); }
#if defined(_WIN32)
// what MSVC's C runtime defines when floating point is used; clang refers to it
int _fltused = 0;
#endif
}
#endif // XF_GAME_LIBRARY_ENTRY

#ifndef XF_GAME_LIBRARY
#include <filesystem>
#include <string>

namespace xf::ppc {

// this build's interface version (what its libraries are built for)
uint32_t game_interface_version();

// The core's part of the runtime's table (ppc_runtime.cpp): resumable and the
// C library's pieces, no version.
Runtime core_runtime();

// The runtime's whole table: the core's part and the four calls whoever
// links the generated code defines (the kernel).
inline Runtime host_runtime() {
    Runtime r = core_runtime();
    r.version = game_interface_version();
    r.dispatch = &dispatch;
    r.invalid = &invalid;
    r.unwind = &unwind;
    r.switch_stack = &switch_stack;
    return r;
}

// A game library loaded and given the runtime's table; nullptr, and why, when
// it cannot be loaded, has no entry or was built for another interface
// version. A library stays loaded for the process's life (its functions are
// in the kernel's tables).
const GameLibrary *load_game_library(const std::filesystem::path &path, const Runtime &runtime, std::string &error);

} // namespace xf::ppc
#endif
