// Guest thread priorities given to the host (src/kernel/thread_priority.h,
// after Xenia's xthread.cc since b3d8a21b7 and 65b74819a): the mapping of the
// Xenon's 0-31 over the host's five levels, the creation flags, the quantum
// decay and the boost on wake. The arithmetic only, in the kernel's 100 ns.

#include "check.h"

#include "thread_priority.h"

using namespace xf::kernel::priority;

namespace {

constexpr uint64_t MS = 10'000;

void mapping() {
    CHECK(host_level(31) == HIGHEST);
    CHECK(host_level(24) == HIGHEST);
    CHECK(host_level(23) == ABOVE_NORMAL);
    CHECK(host_level(17) == ABOVE_NORMAL);
    CHECK(host_level(16) == NORMAL);
    CHECK(host_level(10) == NORMAL);
    CHECK(host_level(9) == BELOW_NORMAL);
    CHECK(host_level(5) == BELOW_NORMAL);
    CHECK(host_level(4) == LOWEST);
    CHECK(host_level(0) == LOWEST);
    CHECK(host_level(-3) == LOWEST);

    // the creation flags: 0x20 above normal, 0x40 normal, neither nothing
    CHECK(creation_level(0x20) == ABOVE_NORMAL);
    CHECK(creation_level(0x40) == NORMAL);
    CHECK(creation_level(0x60) == ABOVE_NORMAL);
    CHECK(creation_level(0x01) == NO_LEVEL);
    CHECK(creation_level(0x8F000087) == NO_LEVEL);
}

void setting() {
    State s;
    set_priority(s, -5, 7);
    CHECK(s.priority == 0 && s.base == 0 && s.quantum_start == 7);
    set_priority(s, 12, 9);
    CHECK(s.priority == 12 && s.base == 12 && wanted(s) == NORMAL);
}

void decays() {
    State s;
    set_priority(s, 10, 0);
    // a quantum not yet over: nothing
    CHECK(!decay(s, 19 * MS));
    CHECK(s.priority == 10 && s.quantum_start == 0);
    // never below the base: a thread at its base has nothing to lose
    CHECK(!decay(s, 20 * MS));
    CHECK(s.quantum_start == 20 * MS);

    // a boosted thread loses the boost and one level at the quantum's end
    State b;
    set_priority(b, 8, 0);
    CHECK(boost_on_wake(b, 3, false, false, 17, 1 * MS));
    CHECK(b.priority == 11 && b.boost == 3);
    CHECK(!decay(b, 20 * MS)); // 19 ms after the boost: not over
    CHECK(decay(b, 21 * MS));
    CHECK(b.priority == 8 && b.boost == 0); // 11 - 3 - 1, floored at the base
    CHECK(b.quantum_start == 21 * MS);

    // a late timer: a level for each quantum that passed
    State l;
    set_priority(l, 8, 0);
    l.priority = 15; // (as if boosted)
    CHECK(decay(l, 45 * MS)); // two quanta
    CHECK(l.priority == 13);

    // the real-time priorities do not decay
    State r;
    set_priority(r, 18, 0);
    r.priority = 25;
    CHECK(!decay(r, 1000 * MS) && r.priority == 25);
}

void boosts() {
    State s;
    set_priority(s, 9, 0);
    // the increment above the base, to the cap
    CHECK(boost_on_wake(s, 2, false, false, 17, MS));
    CHECK(s.priority == 11 && s.boost == 2 && wanted(s) == NORMAL);
    // never lowers it
    CHECK(!boost_on_wake(s, 1, false, false, 17, 2 * MS));
    CHECK(s.priority == 11);
    // the cap: the process's, or 17 when it has none
    State c;
    set_priority(c, 9, 0);
    CHECK(boost_on_wake(c, 15, false, false, 14, MS) && c.priority == 14);
    State d;
    set_priority(d, 9, 0);
    CHECK(boost_on_wake(d, 15, false, false, 0, MS) && d.priority == 17 && wanted(d) == ABOVE_NORMAL);
    // no increment, a boost disabled for the thread, a decay pending: no boost, the quantum restarts
    for (int i = 0; i < 3; i++) {
        State n;
        set_priority(n, 9, 0);
        CHECK(!boost_on_wake(n, i == 0 ? 0 : 3, i == 1, i == 2, 17, 5 * MS));
        CHECK(n.priority == 9 && n.quantum_start == 5 * MS);
    }
    // a real-time thread: only the quantum restarts
    State r;
    set_priority(r, 20, 0);
    CHECK(!boost_on_wake(r, 3, false, false, 17, 6 * MS));
    CHECK(r.priority == 20 && r.boost == 0 && r.quantum_start == 6 * MS);
    // a boost that keeps a thread's level on the host: no change in the mapping, as no call
    State m;
    set_priority(m, 10, 0);
    CHECK(boost_on_wake(m, 1, false, false, 17, MS) && wanted(m) == NORMAL);
}

} // namespace

int main() {
    mapping();
    setting();
    decays();
    boosts();
    return finish("kernel_priority");
}
