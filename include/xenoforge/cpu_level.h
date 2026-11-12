// The CPU level a game library is built for (game_build.h): what the
// generated code may use of the host's instruction set. The recompile step
// runs on the player's machine, so it builds for the best level the CPU has;
// the level is part of the library's name in the cache, and the library
// carries it, so the loader refuses one built for a level this CPU lacks
// (game_library.h).
//
// - x86-64-v3 (AVX2, FMA, BMI2, ...: most x86-64 CPUs since 2013-2015);
// - x86-64-v2 (SSE4.2, POPCNT: every x86-64 CPU since about 2009; the floor,
//   as the generated code needs SSE4.1's rounding and the plain x86-64
//   baseline gains only CPUs too slow to run a game);
// - ARM64's baseline (its FMA and rounding are in every ARM64 CPU).
// Results are the same at every level: without FMA a fused multiply-add is
// the C library's, exact (ppc_runtime.h's lib::fma).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace xf {

enum class CpuLevel : uint32_t {
    None = 0, // below the floor: no generated code runs
    X86_64_V2 = 2,
    X86_64_V3 = 3,
    Arm64 = 0x100,
};

// the level's short name, as file names and switches write it: "v2", "v3",
// "arm64" ("none" for None)
const char *cpu_level_name(CpuLevel level);
// a level by its short name or the full one ("x86-64-v2"); nothing for
// another name
std::optional<CpuLevel> parse_cpu_level(std::string_view name);
// whether this CPU (and its OS, for the AVX state) runs code built for a level
bool cpu_has_level(CpuLevel level);
// the best level this CPU runs (None when it lacks even the floor)
CpuLevel host_cpu_level();
// The level game libraries are built for and looked for in the cache:
// XF_GAME_CPU_LEVEL when the environment sets it (a short or full name: for
// tests), else host_cpu_level().
CpuLevel game_cpu_level();

} // namespace xf
