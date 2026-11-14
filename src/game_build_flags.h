// What a game library is compiled and linked with (game_build.cpp). Its own
// file because it is part of the game library's interface version
// (cmake/game_interface.cmake): a change here makes every cached library
// stale, where a change to the rest of the recompile step does not.
#pragma once

#include "xenoforge/cpu_level.h"

#include <vector>

namespace xf::build {

// The instruction set a CPU level allows (cpu_level.h). The statically
// linked games and the tests take the same from XF_GAME_CPU_LEVEL
// (cmake/xenoforge_port.cmake's xenoforge_cpu_level_flags).
//
// At v3 the vectorizer is told to prefer 128-bit vectors: the guest's own
// are 128-bit (VMX128), and widening its loops to 256 bits made the code
// slower. Game B's deterministic benchmark (docs/runtime.md, "The CPU
// level"; a Zen 3 processor): v3 11.16 s, v2 10.67, v3 with 128-bit vectors
// preferred 10.08, v3 without MOVBE 11.17, v3 with no vectorizing 14.75.
inline std::vector<const char *> cpu_level_flags(CpuLevel level) {
    switch (level) {
    case CpuLevel::X86_64_V2: return {"-march=x86-64-v2"};
    case CpuLevel::X86_64_V3: return {"-march=x86-64-v3", "-mprefer-vector-width=128"};
    // ARM64: its baseline, with the atomics inline: clang's default on Linux
    // calls libgcc's __aarch64_cas* and the like, which a library that imports
    // nothing does not have (the first tier's stencils are built the same way)
    case CpuLevel::Arm64: return {"-mno-outline-atomics"};
    default: return {};
    }
}

// Each file: the statically linked games' flags (CMakeLists.txt: -O2 -g0 -w
// and the level's; never -ffast-math), freestanding (no header but the ones
// written beside the code, no C library: math without errno), and the game
// library's side of ppc_runtime.h. The interface version is added by the
// caller.
inline std::vector<const char *> base_compile_flags(CpuLevel level) {
    std::vector<const char *> flags = {"-std=c++20",      "-O2",       "-g0",         "-w",
                                       "-fno-math-errno", "-nostdinc", "-nostdinc++", "-DXF_GAME_LIBRARY"};
    for (const char *f : cpu_level_flags(level)) flags.push_back(f);
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
