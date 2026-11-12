// The CPU level check of an executable with statically linked generated code
// (boot_<game>, player_<game>, an SDK port; docs/runtime.md, "The CPU level").
// That code is compiled for XF_GAME_CPU_LEVEL (x86-64-v3 by default), and on a
// CPU without it the first AVX2 or BMI2 instruction would end the program on
// an illegal instruction, with no word of why. The generated code may run
// before main: its files can have static initializers. So one baseline-compiled
// file of the executable (xenoforge_add_port writes it) defines
// XF_CPU_LEVEL_CHECK as the level's short name and includes this header, which
// registers a check that runs before every ordinary static initializer: in the
// MSVC runtime's .CRT$XCB section on Windows (after the runtime's own
// .CRT$XCA, before the compiler's .CRT$XCU), as a constructor of priority 101
// (the first one free to programs) on POSIX. It prints what is missing and
// exits.
#pragma once

#include "xenoforge/cpu_level.h"

namespace xf {

// the exit code of a program stopped by the check (no other exit of XenoForge's uses it)
constexpr int kCpuLevelExitCode = 86;

// Exits (kCpuLevelExitCode) with a message when this CPU (host_cpu_level, which
// XF_HOST_CPU_LEVEL can lower, to test this) lacks the level `built` names
// ("v3", "v2", "arm64"); returns when it has it.
void check_static_cpu_level(const char *built);

} // namespace xf

#ifdef XF_CPU_LEVEL_CHECK
namespace xf::detail {
inline void cpu_level_check_entry() { check_static_cpu_level(XF_CPU_LEVEL_CHECK); }
} // namespace xf::detail
#if defined(_WIN32)
static void xf_cpu_level_check_initializer() { xf::detail::cpu_level_check_entry(); }
#pragma section(".CRT$XCB", read)
// a .CRT$XC* entry is a plain function pointer the runtime's _initterm calls;
// kept by the linker though nothing names it
__declspec(allocate(".CRT$XCB"))
__attribute__((used)) static void (*xf_cpu_level_check_pointer)() = &xf_cpu_level_check_initializer;
#else
__attribute__((constructor(101))) static void xf_cpu_level_check_initializer() { xf::detail::cpu_level_check_entry(); }
#endif
#endif
