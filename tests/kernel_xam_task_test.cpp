// XAM's tasks (src/kernel/xam.cpp, "tasks"): XamTaskSchedule queues to a pool
// of six worker threads of the system process, each callback run on a worker,
// the handle the task object's address with the low bit set, which
// XamTaskCloseHandle takes a reference of (it answers the count left) and
// XamTaskShouldExit asks about. Run on a guest thread; the callbacks are host
// functions in the generated code's table, as in kernel_threads_test.

#include "check.h"

#include "objects.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;

uint32_t call_export(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s\n", name);
        failures++;
        return 0xFFFFFFFF;
    }
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    fn(*K, c);
    return uint32_t(c.r[3]);
}
uint32_t xam(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call_export(c, "xam", name, args);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call_export(c, "xboxkrnl", name, args);
}

uint32_t local(ppc::Context &c, uint32_t offset) { return uint32_t(c.r[1]) - 0x400 + offset; }

// until the condition holds, at most 5 seconds
template <typename Fn> bool eventually(ppc::Context &c, Fn &&condition) {
    for (int i = 0; i < 500; i++) {
        if (condition()) return true;
        uint32_t timeout = local(c, 0x320);
        K->store64(timeout, uint64_t(-10 * 10'000));
        krnl(c, "KeDelayExecutionThread", {1, 0, timeout});
    }
    return false;
}

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_RECORD = 0x82000010,
    F_HOLD = 0x82000020,
};

std::mutex m;
std::set<uint32_t> hold_threads, record_threads;
std::atomic<uint32_t> recorded{0}, held{0}, hold_max{0}, running{0}, record_message{0}, record_exit{9};
std::atomic<uint32_t> record_process{0};
uint32_t gate = 0; // a manual-reset event the held tasks wait on

void record(ppc::Context &c) {
    record_message = uint32_t(c.r[3]);
    record_threads.insert(current_thread()->id);
    record_process = current_thread()->params.process;
    record_exit = xam(c, "XamTaskShouldExit", {});
    recorded++;
}

void hold(ppc::Context &c) {
    uint32_t now = ++running;
    uint32_t seen = hold_max.load();
    while (now > seen && !hold_max.compare_exchange_weak(seen, now)) {}
    {
        std::lock_guard<std::mutex> l(m);
        hold_threads.insert(current_thread()->id);
    }
    held++;
    uint32_t timeout = local(c, 0x340);
    K->store64(timeout, uint64_t(-5'000 * 10'000));
    krnl(c, "NtWaitForSingleObjectEx", {gate, 0, 0, timeout});
    running--;
}

void scenarios(ppc::Context &c) {
    uint32_t handle_ptr = local(c, 0x100);
    uint32_t base_threads;
    {
        std::lock_guard<std::mutex> l(objects(*K).lock);
        base_threads = uint32_t(objects(*K).threads.size());
    }

    // a task: the call answers 0, the handle is an address with the low bit set, the callback
    // runs with its message on a thread of the system process, not on this one
    K->store32(handle_ptr, 0);
    CHECK(xam(c, "XamTaskSchedule", {F_RECORD, 0x1234, 0, handle_ptr}) == 0);
    uint32_t handle = K->load32(handle_ptr);
    CHECK((handle & 1) && handle != 0xF8000004 && (handle & ~1u) >= 0x10000);
    CHECK(eventually(c, [] { return recorded.load() == 1; }));
    CHECK(record_message == 0x1234 && record_exit == 0);
    CHECK(!record_threads.count(current_thread()->id));
    CHECK(record_process == objects(*K).system_process);
    // no handle pointer: allowed (the task is still made)
    CHECK(xam(c, "XamTaskSchedule", {F_RECORD, 0x5678, 0, 0}) == 0);
    CHECK(eventually(c, [] { return recorded.load() == 2; }));
    CHECK(record_message == 0x5678);
    // a caller that is no worker is not asked to exit
    CHECK(xam(c, "XamTaskShouldExit", {}) == 0);

    // the handle: its count goes down with each close, the object goes with the last; the
    // queue's reference was dropped when the task ended, so one close is the last
    CHECK(eventually(c, [&] { return K->load32((handle & ~1u) + 4) <= 1; }));
    CHECK(xam(c, "XamTaskCloseHandle", {handle}) == 0);
    CHECK(xam(c, "XamTaskCloseHandle", {handle}) == 0xFFFFFFFF); // gone
    CHECK(xam(c, "XamTaskCloseHandle", {0x12345679}) == 0xFFFFFFFF);
    CHECK(xam(c, "XamTaskCloseHandle", {0x12345678}) == 0xFFFFFFFF); // no low bit: not a task's

    // a task still running when it is closed: the count is 1, the task goes on, ends, and
    // the handle is gone with it
    uint32_t event_ptr = local(c, 0x110);
    CHECK(krnl(c, "NtCreateEvent", {event_ptr, 0, 0, 0}) == 0); // manual-reset
    gate = K->load32(event_ptr);
    held = 0, running = 0, hold_max = 0;
    CHECK(xam(c, "XamTaskSchedule", {F_HOLD, 1, 0, handle_ptr}) == 0);
    uint32_t running_handle = K->load32(handle_ptr);
    CHECK(eventually(c, [] { return held.load() == 1; }));
    CHECK(xam(c, "XamTaskCloseHandle", {running_handle}) == 1);
    CHECK(krnl(c, "NtSetEvent", {gate, 0}) == 0);
    CHECK(eventually(c, [] { return running.load() == 0; }));
    CHECK(eventually(c, [&] {
        // the worker's reference was the last: the handle is no task's
        return xam(c, "XamTaskCloseHandle", {running_handle}) == 0xFFFFFFFF;
    }));
    CHECK(krnl(c, "NtClearEvent", {gate}) == 0);

    // the pool: six tasks run at once, each on a thread of its own, and a seventh waits
    held = 0, running = 0, hold_max = 0;
    hold_threads.clear();
    for (int i = 0; i < 8; i++) CHECK(xam(c, "XamTaskSchedule", {F_HOLD, uint32_t(i), 0, 0}) == 0);
    CHECK(eventually(c, [] { return held.load() == 6; }));
    uint32_t sleep_timeout = local(c, 0x360);
    K->store64(sleep_timeout, uint64_t(-100 * 10'000));
    krnl(c, "KeDelayExecutionThread", {1, 0, sleep_timeout}); // the seventh would have started by now
    CHECK(held.load() == 6 && running.load() == 6);
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(hold_threads.size() == 6 && !hold_threads.count(current_thread()->id));
    }
    CHECK(krnl(c, "NtSetEvent", {gate, 0}) == 0);
    CHECK(eventually(c, [] { return held.load() == 8 && running.load() == 0; }));
    CHECK(hold_max.load() == 6);
    // the workers are the threads made once, not one a task
    {
        std::lock_guard<std::mutex> l(objects(*K).lock);
        CHECK(objects(*K).threads.size() <= base_threads + 6 + 1);
    }
    CHECK(krnl(c, "NtClose", {gate}) == 0);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, scenarios},
    {F_RECORD, record},
    {F_HOLD, hold},
};

} // namespace

int main() {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        Objects &o = objects(k);
        o.set_tls(64, 0, 0, 0);
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        CHECK(!k.stopped);
        // the pool ended and joined with the kernel
        shutdown_xam_tasks(k);
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    return finish("kernel_xam_task");
}
