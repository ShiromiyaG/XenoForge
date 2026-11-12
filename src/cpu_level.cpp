// The CPU levels game libraries are built for (xenoforge/cpu_level.h).

#include "xenoforge/cpu_level.h"

#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#define XF_CPU_X86 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace xf {
namespace {

#ifdef XF_CPU_X86
struct Cpuid {
    unsigned a = 0, b = 0, c = 0, d = 0;
};
Cpuid cpuid(unsigned leaf, unsigned sub = 0) {
    Cpuid r;
#ifdef _MSC_VER
    int v[4];
    __cpuidex(v, int(leaf), int(sub));
    r.a = unsigned(v[0]), r.b = unsigned(v[1]), r.c = unsigned(v[2]), r.d = unsigned(v[3]);
#else
    __cpuid_count(leaf, sub, r.a, r.b, r.c, r.d);
#endif
    return r;
}
__attribute__((target("xsave"))) unsigned long long xcr0() { return _xgetbv(0); }

// The x86-64 psABI's levels: v2 = CMPXCHG16B, LAHF/SAHF, POPCNT, SSE3,
// SSSE3, SSE4.1, SSE4.2; v3 = v2 and AVX, AVX2, BMI1, BMI2, F16C, FMA,
// LZCNT, MOVBE, with the OS saving the YMM state (OSXSAVE, XCR0 bits 1-2).
CpuLevel detect() {
    const unsigned max = cpuid(0).a, max_extended = cpuid(0x80000000u).a;
    if (max < 1 || max_extended < 0x80000001u) return CpuLevel::None;
    const Cpuid one = cpuid(1), extended = cpuid(0x80000001u);
    auto bit = [](unsigned v, int n) { return (v >> n) & 1; };
    const bool v2 = bit(one.c, 0) && bit(one.c, 9) && bit(one.c, 13) && bit(one.c, 19) && bit(one.c, 20) &&
                    bit(one.c, 23) && bit(extended.c, 0);
    if (!v2) return CpuLevel::None;
    if (max < 7 || !bit(one.c, 27) || (xcr0() & 6) != 6) return CpuLevel::X86_64_V2;
    const Cpuid seven = cpuid(7, 0);
    const bool v3 = bit(one.c, 12) && bit(one.c, 22) && bit(one.c, 28) && bit(one.c, 29) && bit(seven.b, 3) &&
                    bit(seven.b, 5) && bit(seven.b, 8) && bit(extended.c, 5);
    return v3 ? CpuLevel::X86_64_V3 : CpuLevel::X86_64_V2;
}
#elif defined(__aarch64__) || defined(_M_ARM64)
CpuLevel detect() { return CpuLevel::Arm64; }
#else
// TODO: other hosts (no game library is built for them yet)
CpuLevel detect() { return CpuLevel::None; }
#endif

} // namespace

const char *cpu_level_name(CpuLevel level) {
    switch (level) {
    case CpuLevel::X86_64_V2: return "v2";
    case CpuLevel::X86_64_V3: return "v3";
    case CpuLevel::Arm64: return "arm64";
    default: return "none";
    }
}

std::optional<CpuLevel> parse_cpu_level(std::string_view name) {
    if (name == "v2" || name == "x86-64-v2") return CpuLevel::X86_64_V2;
    if (name == "v3" || name == "x86-64-v3") return CpuLevel::X86_64_V3;
    if (name == "arm64") return CpuLevel::Arm64;
    return std::nullopt;
}

CpuLevel host_cpu_level() {
    static const CpuLevel level = detect();
    return level;
}

bool cpu_has_level(CpuLevel level) {
    const CpuLevel host = host_cpu_level();
    if (level == CpuLevel::None || host == CpuLevel::None) return false;
    if (level == CpuLevel::Arm64 || host == CpuLevel::Arm64) return level == host;
    return uint32_t(level) <= uint32_t(host);
}

CpuLevel game_cpu_level() {
    static const CpuLevel level = [] {
        const char *env = std::getenv("XF_GAME_CPU_LEVEL");
        if (!env || !*env) return host_cpu_level();
        if (std::optional<CpuLevel> forced = parse_cpu_level(env)) return *forced;
        std::fprintf(stderr, "XF_GAME_CPU_LEVEL=%s: not a CPU level (v2, v3, arm64); %s taken\n", env,
                     cpu_level_name(host_cpu_level()));
        return host_cpu_level();
    }();
    return level;
}

} // namespace xf
