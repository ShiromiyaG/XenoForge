// The core's part of what the generated code calls (ppc_runtime.h): the
// runner of bodies with resume sites, and the C library's pieces as a game
// library's table hands them over (game_library.h).

#include "xenoforge/ppc_runtime.h"
#include "xenoforge/ppc_module.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>

namespace xf::ppc {

void resumable(Context &c, ResumableBody body, uint32_t lr, uint32_t sp, const uint32_t *sites, size_t count) {
    uint32_t at = 0;
    for (;;) {
        try {
            body(c, at, lr, sp);
            return;
        } catch (const Unwind &u) {
            // a frame further up (r1 at or above this one's entry), or not
            // one of its resume sites: on up
            if (uint32_t(c.r[1]) >= sp || !std::binary_search(sites, sites + count, u.target)) throw;
            at = u.target;
        }
    }
}

namespace {

// A game library's rounding modes and exception flags (numbered as
// ppc_runtime.h's Runtime says) as the host's <cfenv> numbers them
constexpr int HOST_ROUND[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
constexpr int HOST_EXCEPT[5] = {FE_INEXACT, FE_UNDERFLOW, FE_OVERFLOW, FE_DIVBYZERO, FE_INVALID};

int to_host_flags(int flags) {
    int host = 0;
    for (int k = 0; k < 5; k++)
        if (flags & (1 << k)) host |= HOST_EXCEPT[k];
    return host;
}
int from_host_flags(int host) {
    int flags = 0;
    for (int k = 0; k < 5; k++)
        if (host & HOST_EXCEPT[k]) flags |= 1 << k;
    return flags;
}

} // namespace

Runtime core_runtime() {
    Runtime r{};
    r.size = sizeof(Runtime);
    r.resumable = &resumable;
    r.hooked = &hooked;
    r.mid_hook = &mid_hook;
    r.memcpy = [](void *to, const void *from, size_t n) { return std::memcpy(to, from, n); };
    r.memmove = [](void *to, const void *from, size_t n) { return std::memmove(to, from, n); };
    r.memset = [](void *to, int value, size_t n) { return std::memset(to, value, n); };
    r.llrint = [](double v) { return std::llrint(v); };
    r.exp2f = [](float v) { return std::exp2(v); };
    r.log2f = [](float v) { return std::log2(v); };
    r.fmod = [](double a, double b) { return std::fmod(a, b); };
    r.fesetround = [](int mode) { return std::fesetround(HOST_ROUND[mode & 3]); };
    r.feclearexcept = [](int flags) { return std::feclearexcept(to_host_flags(flags)); };
    r.fetestexcept = [](int flags) { return from_host_flags(std::fetestexcept(to_host_flags(flags))); };
    // the C library's: the hardware's where the CPU has FMA, exact in
    // software where it does not (the core is built for the baseline)
    r.fma = [](double a, double b, double c) { return std::fma(a, b, c); };
    r.fmaf = [](float a, float b, float c) { return std::fma(a, b, c); };
    return r;
}

} // namespace xf::ppc
