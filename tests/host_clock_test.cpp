// The host clock behind the kernel's time (src/kernel/host_clock.h): its rate
// against steady_clock, monotonic on a thread and across threads, and what a
// read costs next to steady_clock's (printed, not checked: the machine may be
// busy).

#include "check.h"
#include "host_clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace xf::kernel;
using Steady = std::chrono::steady_clock;

int main() {
    std::printf("host clock: %s at %llu Hz\n", host_clock::source(), (unsigned long long)host_clock::frequency());

    // the rate: 100 ns units, within 0.1% of steady_clock's over 200 ms (the
    // frequency is measured over 30 ms, a few parts per million off)
    uint64_t c0 = host_clock::now();
    Steady::time_point s0 = Steady::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c1 = host_clock::now();
    Steady::time_point s1 = Steady::now();
    double units = double(std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()) / 100;
    double ratio = double(c1 - c0) / units;
    std::printf("rate against steady_clock: %.6f\n", ratio);
    CHECK(ratio > 0.999 && ratio < 1.001);

    // monotonic on each thread, and across threads: a value read after
    // seeing another thread's published read is not older than it (a bare
    // rdtsc fails this: it runs ahead of the load)
    std::atomic<uint64_t> published{0};
    std::atomic<int> backwards{0}, older{0};
    std::atomic<uint64_t> worst{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&] {
            uint64_t last = 0;
            for (int i = 0; i < 2'000'000; i++) {
                uint64_t seen = published.load(std::memory_order_acquire);
                uint64_t v = host_clock::now();
                if (v < last) backwards++;
                if (v < seen) older++;
                if (v < seen && seen - v > worst) worst = seen - v;
                last = v;
                uint64_t expected = published.load(std::memory_order_relaxed);
                while (expected < v && !published.compare_exchange_weak(expected, v, std::memory_order_release)) {
                }
            }
        });
    for (auto &t : threads) t.join();
    CHECK(backwards == 0);
    std::printf("older: %d, at most %llu units\n", older.load(), (unsigned long long)worst.load());
    CHECK(older == 0);

    // what a read costs, against steady_clock's
    constexpr int N = 5'000'000;
    uint64_t sink = 0;
    auto t0 = Steady::now();
    for (int i = 0; i < N; i++) sink += host_clock::now();
    auto t1 = Steady::now();
    for (int i = 0; i < N; i++) sink += uint64_t(Steady::now().time_since_epoch().count());
    auto t2 = Steady::now();
    std::printf("a read: host clock %.1f ns, steady_clock %.1f ns (%llu)\n",
                std::chrono::duration<double, std::nano>(t1 - t0).count() / N,
                std::chrono::duration<double, std::nano>(t2 - t1).count() / N, (unsigned long long)(sink & 1));
    return finish("host_clock");
}
