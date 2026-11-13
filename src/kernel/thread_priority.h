/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Guest thread priorities given to the host, as Xenia does them since
// b3d8a21b7 ("Add thread priority mapping with timer-driven quantum decay")
// and 65b74819a ("Implement priority boost on wake"): xthread.cc's
// GuestPriorityToHost, SetPriority, CheckQuantumAndDecay and BoostOnWake, and
// the creation flags' 0x60 mask. Only the arithmetic is here (a state per
// thread, which the callers hold under the kernel's lock, and the time as
// the arguments); threads.cpp gives the levels to the host thread and
// sync.cpp/objects.cpp carry the signalers' increments to the waiters.
//
// Where this differs from Xenia: the time is the kernel's clock (100 ns
// units) rather than the host's uptime in milliseconds; a level the host
// thread already has is not set again; KeSetBasePriorityThread's priority is
// the process's default plus the increment as NT has it (Xenia takes the
// increment as the priority itself).

#pragma once

#include <algorithm>
#include <cstdint>

namespace xf::kernel::priority {

// the host's five levels (Xenia's xe::threading::ThreadPriority)
enum Host : int { LOWEST = -2, BELOW_NORMAL = -1, NORMAL = 0, ABOVE_NORMAL = 1, HIGHEST = 2 };

// at or above it (the real-time priorities) a thread's quantum does not decay
constexpr int32_t REALTIME = 18;
// the cap on a boosted priority when the KTHREAD names none
constexpr int32_t DEFAULT_CAP = 17;
// the quantum's end: the Xenon's 1 ms clock takes 3 of a quantum of 60, 20 ms
constexpr uint64_t QUANTUM_PERIOD = 200'000;

// Xenia's GuestPriorityToHost: the Xenon's 0-31 over the host's five levels
constexpr int host_level(int32_t guest) {
    if (guest >= 24) return HIGHEST;
    if (guest >= 17) return ABOVE_NORMAL;
    if (guest >= 10) return NORMAL;
    if (guest >= 5) return BELOW_NORMAL;
    return LOWEST;
}

// ExCreateThread's flags: 0x20 starts the host thread above normal, 0x40
// normal (Xenia's XThread::Create, flags & 0x60); NO_LEVEL for neither
constexpr int NO_LEVEL = -100;
constexpr int creation_level(uint32_t flags) {
    if (!(flags & 0x60)) return NO_LEVEL;
    return (flags & 0x20) ? ABOVE_NORMAL : NORMAL;
}

struct State {
    int32_t priority = 0;         // effective: the base, boosted, decayed
    int32_t base = 0;             // what decay does not go below
    int32_t boost = 0;            // the boost above the base, drained at the quantum's end
    uint64_t quantum_start = 0;   // the clock when the quantum last restarted
    int host = NORMAL;            // the level it was last asked to have
    int applied = NORMAL;         // the level the host thread has (the host's default is normal)
};

// the level the host thread should have for the effective priority
constexpr int wanted(const State &s) { return host_level(s.priority); }

// Xenia's XThread::SetPriority: negative priorities are zero; both the
// effective and the base priority are the new one
inline void set_priority(State &s, int32_t priority, uint64_t now) {
    s.priority = s.base = std::max(priority, 0);
    s.boost = 0;
    s.quantum_start = now;
}

// Xenia's CheckQuantumAndDecay, called every 20 ms for each running thread:
// a priority of 18 and above does not decay; the others lose a level per
// elapsed quantum, the boost with the first, down to the base. True if the
// effective priority changed.
inline bool decay(State &s, uint64_t now) {
    if (s.priority >= REALTIME) return false;
    uint64_t elapsed = now - s.quantum_start;
    if (elapsed < QUANTUM_PERIOD) return false;
    int32_t steps = int32_t(std::min<uint64_t>(elapsed / QUANTUM_PERIOD, 64));
    int32_t total = s.boost + steps;
    s.boost = 0;
    int32_t next = std::max(s.priority - total, s.base);
    s.quantum_start = now;
    if (next == s.priority) return false;
    s.priority = next;
    return true;
}

// Xenia's BoostOnWake: a thread that wakes from a wait gets the signaler's
// increment above its base, up to the cap, when it has no decay pending and
// boosts are not disabled for it; never lower than it is. A real-time thread
// only has its quantum restarted. True if the effective priority changed.
inline bool boost_on_wake(State &s, int32_t increment, bool boost_disabled, bool decay_pending, int32_t cap,
                          uint64_t now) {
    if (s.priority >= REALTIME) {
        s.boost = 0;
        s.quantum_start = now;
        return false;
    }
    bool changed = false;
    if (increment > 0 && !decay_pending && !boost_disabled) {
        int32_t boosted = std::min(s.base + increment, cap > 0 ? cap : DEFAULT_CAP);
        if (boosted > s.priority) {
            s.priority = boosted;
            s.boost = boosted - s.base;
            changed = true;
        }
    }
    s.quantum_start = now;
    return changed;
}

} // namespace xf::kernel::priority
