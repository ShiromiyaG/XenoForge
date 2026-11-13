#include "thread_cpu.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace xf {

std::atomic<unsigned long> gpu_back_end_os_thread{0};

ThreadCpu::~ThreadCpu() {
#ifdef _WIN32
    if (const uint64_t id = id_.load()) CloseHandle(reinterpret_cast<HANDLE>(id));
#endif
}

void ThreadCpu::bind_current() {
#ifdef _WIN32
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    if (h) id_.store(reinterpret_cast<uint64_t>(h), std::memory_order_release);
    os_id_.store(GetCurrentThreadId(), std::memory_order_release);
#else
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock) == 0)
        id_.store(uint64_t(uint32_t(clock)) + 1, std::memory_order_release);
#endif
}

uint64_t ThreadCpu::ticks() const {
    const uint64_t id = id_.load(std::memory_order_acquire);
    if (!id) return 0;
#ifdef _WIN32
    ULONG64 cycles = 0;
    QueryThreadCycleTime(reinterpret_cast<HANDLE>(id), &cycles);
    return cycles;
#else
    timespec t{};
    clock_gettime(clockid_t(uint32_t(id - 1)), &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}

uint64_t current_thread_cpu_ticks() {
#ifdef _WIN32
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto value = [](const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 1e-7; };
    return value(kernel) + value(user);
#else
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
#endif
}

} // namespace xf
