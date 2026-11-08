#pragma once

// The host clock behind the kernel's time: the scheduler's now() (100 ns
// units), and through it mftb's time base, KeQuerySystemTime and the waits'
// deadlines. Games read the time base millions of times a second, so a read
// is the CPU's own counter scaled by a fixed-point multiply (no system call,
// no lock, nothing shared written): the invariant TSC on x86-64, the generic
// timer's virtual count on ARM64 Linux and Android, std::chrono::steady_clock
// elsewhere and where the counter cannot be trusted.

#include <cstdint>

namespace xf::kernel::host_clock {

// 100 ns units since the clock's first use: never back on a thread, and
// consistent across threads (the counters chosen are synchronized between
// the host's cores, and read after the loads before them)
uint64_t now();

// what now() reads, for logs: "tsc", "cntvct" or "steady_clock"
const char *source();

// the counter's frequency in Hz (10,000,000 for steady_clock's 100 ns units)
uint64_t frequency();

} // namespace xf::kernel::host_clock
