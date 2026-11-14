// A thread's processor time, read from another thread: for timing the GPU's
// threads (the back end, the recording thread) where xfreplay prints them.
// The thread names itself once (bind_current); anyone reads its ticks after.
// Ticks are the processor's cycles on Windows (QueryThreadCycleTime) and
// nanoseconds elsewhere (the thread's CPU clock), as replay.cpp's own.
#pragma once

#include <atomic>
#include <cstdint>

namespace xf {

class ThreadCpu {
  public:
    ThreadCpu() = default;
    ThreadCpu(const ThreadCpu &) = delete;
    ThreadCpu &operator=(const ThreadCpu &) = delete;
    ~ThreadCpu();
    // on the thread to be timed, once
    void bind_current();
    // its processor time so far; 0 before it is bound
    uint64_t ticks() const;
    // the system's id of the thread (Windows; 0 elsewhere and before it is bound)
    unsigned long os_id() const { return os_id_.load(std::memory_order_acquire); }

  private:
    std::atomic<unsigned long> os_id_{0};
    std::atomic<uint64_t> id_{0}; // Windows: a handle of the thread's; elsewhere its clock's id plus one
};

// the calling thread's processor time, in the same ticks
uint64_t current_thread_cpu_ticks();
// the GPU back end's thread's system id, once it runs (for a profiler to sample it), else 0
extern std::atomic<unsigned long> gpu_back_end_os_thread;
// the renderer's recording thread's system id, once it runs (for a profiler), else 0
extern std::atomic<unsigned long> gpu_recording_os_thread;
// the whole process's processor time so far, all of its threads, in seconds
double process_cpu_seconds();
// how many ticks make a second (Windows: the cycle counter's rate, measured over a busy 20 ms the
// first time it is asked for; elsewhere 1e9)
double cpu_ticks_a_second();

} // namespace xf
