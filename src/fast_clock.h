// A clock cheap enough to read around every call of a busy thread: the processor's time stamp
// counter where it runs at a constant rate (x86-64 with an invariant TSC; its rate measured
// against steady_clock over 2 ms the first time it is asked for), steady_clock elsewhere.
// steady_clock is QueryPerformanceCounter on Windows, about 100 ns a read on the machine it was
// measured on: two reads a draw were 2% of the GPU command thread, and two around each of a
// replay's 135,000 packets a play were most of what the replay timed.
// XF_STEADY_CLOCK=1: steady_clock.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace xf {

class FastClock {
public:
    static uint64_t now() {
#if defined(__x86_64__) || defined(_M_X64)
        // (the choice read from a plain word once made: a function's static is a guarded one,
        // checked against a thread-local count at every call on Windows)
        int tsc = tsc_.load(std::memory_order_relaxed);
        if (tsc < 0) tsc_.store(tsc = rate().tsc ? 1 : 0, std::memory_order_relaxed);
        if (tsc) return __rdtsc();
#endif
        return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    // seconds from a difference of now()'s
    static double seconds(uint64_t ticks) { return double(ticks) * rate().seconds_a_tick; }

private:
    struct Rate {
        bool tsc = false;
        double seconds_a_tick = double(std::chrono::steady_clock::period::num) / std::chrono::steady_clock::period::den;
    };
    static inline std::atomic<int> tsc_{-1}; // -1 not chosen yet, 0 steady_clock, 1 the time stamp counter
    static const Rate &rate() {
        static const Rate r = [] {
            Rate out;
#if defined(__x86_64__) || defined(_M_X64)
            const char *v = std::getenv("XF_STEADY_CLOCK");
            if (v && v[0] == '1') return out;
            unsigned regs[4] = {};
#ifdef _MSC_VER
            __cpuid(reinterpret_cast<int *>(regs), int(0x80000000));
            if (regs[0] < 0x80000007) return out;
            __cpuid(reinterpret_cast<int *>(regs), int(0x80000007));
#else
            if (__get_cpuid_max(0x80000000, nullptr) < 0x80000007) return out;
            __cpuid(0x80000007, regs[0], regs[1], regs[2], regs[3]);
#endif
            if (!(regs[3] & (1u << 8))) return out; // no invariant TSC
            using clock = std::chrono::steady_clock;
            const clock::time_point t0 = clock::now();
            const uint64_t c0 = __rdtsc();
            clock::time_point t1;
            while ((t1 = clock::now()) - t0 < std::chrono::milliseconds(2)) {}
            const uint64_t c1 = __rdtsc();
            if (c1 > c0) {
                out.tsc = true;
                out.seconds_a_tick = std::chrono::duration<double>(t1 - t0).count() / double(c1 - c0);
            }
#endif
            return out;
        }();
        return r;
    }
};

} // namespace xf
