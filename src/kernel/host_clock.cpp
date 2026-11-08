// The host clock (host_clock.h). Xenia's Clock (src/xenia/base/clock*.cc)
// reads QueryPerformanceCounter by default and, with clock_source_raw, the
// TSC at a frequency taken from CPUID (Intel only, a fatal error elsewhere);
// either way its guest tick count goes through a mutex. Here the counter is
// read without a lock and scaled by a multiply fixed once; its frequency is
// measured against steady_clock at the first read, so it works on any CPU
// whose TSC is invariant, and steady_clock remains the fallback.

#include "host_clock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#define XF_HOST_CLOCK_TSC 1
#elif defined(__aarch64__) && !defined(_WIN32)
#define XF_HOST_CLOCK_CNTVCT 1
#endif

namespace xf::kernel::host_clock {
namespace {

using Steady = std::chrono::steady_clock;

enum class Source { Steady, Tsc, Cntvct };

struct State {
    Source source = Source::Steady;
    uint64_t start = 0;      // the counter's value at 0
    uint64_t multiplier = 0; // 100 ns units = (counter - start) * multiplier >> shift
    unsigned shift = 0;
    uint64_t frequency = 10'000'000;
    Steady::time_point steady_start;
};

#if XF_HOST_CLOCK_TSC
// rdtscp: it waits for the loads before it. A bare rdtsc (Xenia's raw clock)
// costs 9 ns against rdtscp's 18 here, but runs ahead of a load still
// missing the cache: a thread that saw another's store and then read the
// clock got values up to 500 ns older than the other's read before the store
// (host_clock_test), a negative interval to a guest timing across threads.
// lfence+rdtsc where rdtscp is missing (CPUs before 2006).
bool has_rdtscp = false;
inline uint64_t read_tsc() {
    if (has_rdtscp) [[likely]] {
        unsigned aux;
        return __builtin_ia32_rdtscp(&aux);
    }
    __builtin_ia32_lfence();
    return __builtin_ia32_rdtsc();
}
#endif

#if XF_HOST_CLOCK_CNTVCT
inline uint64_t read_cntvct() {
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
}
#endif

// floor((high * 2^64) / divisor) for high < divisor, without a 128-bit
// division (a library call some toolchains do not link)
uint64_t divide_shifted(uint64_t high, uint64_t divisor) {
    uint64_t remainder = high, quotient = 0;
    for (int i = 0; i < 64; i++) {
        bool carry = remainder >> 63;
        remainder <<= 1;
        quotient <<= 1;
        if (carry || remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1;
        }
    }
    return quotient;
}

void set_scale(State &s, uint64_t frequency) {
    s.frequency = frequency;
    if (frequency > 10'000'000) {
        s.shift = 64;
        s.multiplier = divide_shifted(10'000'000, frequency);
    } else {
        s.shift = 32;
        s.multiplier = (uint64_t(10'000'000) << 32) / frequency;
    }
}

// the counter's frequency against steady_clock over about 30 ms: each end is
// the steady_clock read whose two bracketing counter reads were closest
template <typename Read> uint64_t measure_frequency(Read read) {
    auto sample = [&](uint64_t &counter, Steady::time_point &time) {
        uint64_t best = ~0ull;
        for (int i = 0; i < 8; i++) {
            uint64_t a = read();
            Steady::time_point t = Steady::now();
            uint64_t b = read();
            if (b - a < best) best = b - a, counter = a + (b - a) / 2, time = t;
        }
    };
    uint64_t c0, c1;
    Steady::time_point t0, t1;
    sample(c0, t0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    sample(c1, t1);
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns <= 0 || c1 <= c0) return 0;
    // at most 20 GHz over 30 ms: 6e8 ticks, times 1e9 fits in 64 bits
    return (c1 - c0) * 1'000'000'000ull / uint64_t(ns);
}

#if XF_HOST_CLOCK_TSC
bool tsc_usable() {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) || eax < 0x80000007) return false;
    __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    has_rdtscp = (edx >> 27) & 1;
    // the invariant TSC (CPUID 80000007h EDX[8]): one rate in every power
    // state, and the same count on every core
    __get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
    return (edx >> 8) & 1;
}
#endif

State make_state() {
    State s;
    s.steady_start = Steady::now();
    const char *forced = std::getenv("XF_HOST_CLOCK");
    bool steady_only = forced && std::strcmp(forced, "steady_clock") == 0;
#if XF_HOST_CLOCK_TSC
    if (!steady_only && tsc_usable()) {
        uint64_t f = measure_frequency(read_tsc);
        // a TSC below 100 MHz or above 20 GHz is not one to trust
        if (f >= 100'000'000 && f <= 20'000'000'000ull) {
            s.source = Source::Tsc;
            set_scale(s, f);
            s.start = read_tsc();
            return s;
        }
    }
#elif XF_HOST_CLOCK_CNTVCT
    if (!steady_only) {
        // the generic timer states its own frequency; it is measured anyway,
        // since some firmware sets CNTFRQ wrong
        uint64_t stated;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(stated));
        uint64_t f = measure_frequency(read_cntvct);
        if (f && stated && (f > stated ? f - stated : stated - f) < stated / 1000) f = stated;
        if (f >= 1'000'000) {
            s.source = Source::Cntvct;
            set_scale(s, f);
            s.start = read_cntvct();
            return s;
        }
    }
#endif
    (void)steady_only;
    return s;
}

const State &state() {
    static const State s = make_state();
    return s;
}

} // namespace

uint64_t now() {
    const State &s = state();
    uint64_t counter;
    switch (s.source) {
#if XF_HOST_CLOCK_TSC
    case Source::Tsc: counter = read_tsc(); break;
#endif
#if XF_HOST_CLOCK_CNTVCT
    case Source::Cntvct: counter = read_cntvct(); break;
#endif
    default:
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now() - s.steady_start).count()) /
               100;
    }
    // (another core's count a hair behind, just after the start)
    uint64_t elapsed = counter > s.start ? counter - s.start : 0;
    return uint64_t((unsigned __int128)elapsed * s.multiplier >> s.shift);
}

const char *source() {
    switch (state().source) {
    case Source::Tsc: return "tsc";
    case Source::Cntvct: return "cntvct";
    default: return "steady_clock";
    }
}

uint64_t frequency() { return state().frequency; }

} // namespace xf::kernel::host_clock
