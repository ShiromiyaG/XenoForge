// What a game library is compiled and linked with (game_build.cpp). Its own
// file because it is part of the game library's interface version
// (cmake/game_interface.cmake): a change here makes every cached library
// stale, where a change to the rest of the recompile step does not.
#pragma once

#include <vector>

namespace xf::build {

// Each file: the statically linked games' flags (CMakeLists.txt: -O2 -g0 -w
// -mfma -mavx2; never -ffast-math), freestanding (no header but the ones
// written beside the code, no C library: math without errno), and the game
// library's side of ppc_runtime.h. The interface version is added by the
// caller.
inline std::vector<const char *> base_compile_flags() {
    std::vector<const char *> flags = {"-std=c++20", "-O2",         "-g0",         "-w",
                                       "-mfma",      "-mavx2",      "-fno-math-errno",
                                       "-nostdinc",  "-nostdinc++", "-DXF_GAME_LIBRARY"};
#ifndef _WIN32
    // position independent, only the entry exported, and no stack
    // protector (some distributions' clang turns it on; it needs the C
    // library's __stack_chk_fail)
    for (const char *f : {"-fPIC", "-fvisibility=hidden", "-fno-stack-protector"}) flags.push_back(f);
#endif
    return flags;
}

// The link: no entry point, no default libraries (the library imports
// nothing); /Brepro: no time stamp, the same bytes from the same objects
inline std::vector<const char *> link_flags() {
#ifdef _WIN32
    return {"/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/opt:ref", "/Brepro"};
#else
    return {"-shared", "--eh-frame-hdr", "-z", "noexecstack", "--gc-sections"};
#endif
}

} // namespace xf::build
