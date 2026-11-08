// The deterministic mode (src/kernel/deterministic.cpp; docs/runtime.md, "The
// deterministic mode") and input recordings (docs/input.md, "Recordings"):
// guest threads that wait, yield, signal each other and run kernel calls
// without blocking (handed over at the end of their slice) leave the same
// trace of who ran when on the virtual clock, the same clock and the same
// guest memory, run after run; a run that records the input answers and one
// that replays the recording with no controller see the same states. The
// guest code is host functions in the generated code's table.
//
// (The threads' own scenarios run on this scheduler too: kernel_threads_test
// built as kernel_threads_deterministic_test.)

#include "check.h"

#include "objects.h"
#include "xenoforge/input.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <filesystem>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
uint32_t qpc_thunk = 0; // KeQueryInterruptTime as the code calls it (counted, the clock moved)
std::string trace;      // what the guest threads did, in order, with the clock

void note(const char *what, uint32_t a = 0, uint32_t b = 0) {
    char line[96];
    std::snprintf(line, sizeof(line), "%llu %s %u %u\n", (unsigned long long)K->interrupt_time(), what, a, b);
    trace += line;
}

uint32_t ex(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
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
uint32_t kr(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return ex(c, "xboxkrnl", name, args);
}

uint32_t local(ppc::Context &c, uint32_t offset) { return uint32_t(c.r[1]) - 0x400 + offset; }

void sleep_ms(ppc::Context &c, int64_t ms) {
    uint32_t p = local(c, 0x300);
    K->store64(p, uint64_t(-ms * 10'000));
    kr(c, "KeDelayExecutionThread", {1, 0, p});
}

enum : uint32_t { F_MAIN = 0x82000000, F_SLEEPER = 0x82000010, F_BUSY = 0x82000020, F_PINGER = 0x82000030 };

uint32_t ping = 0, pong = 0; // two auto-reset events' handles

// sleeps of different lengths, noting each wake
void sleeper(ppc::Context &c) {
    uint32_t n = uint32_t(c.r[3]);
    for (uint32_t i = 0; i < 12; i++) {
        sleep_ms(c, 3 + (i * 7 + n) % 11);
        note("sleeper", n, i);
    }
}

// kernel calls that never block: the slice ends hand it over
void busy(ppc::Context &c) {
    uint32_t n = uint32_t(c.r[3]);
    for (uint32_t i = 0; i < 30'000; i++) {
        K->call(c, qpc_thunk);
        if (i % 2'500 == 0) note("busy", n, i);
        if (i % 7'000 == 0) kr(c, "NtYieldExecution", {});
    }
}

// a ping-pong with the main thread through two events
void pinger(ppc::Context &c) {
    for (uint32_t i = 0; i < 20; i++) {
        kr(c, "NtWaitForSingleObjectEx", {ping, 0, 0, 0});
        note("pong", i);
        kr(c, "NtSetEvent", {pong, 0});
    }
}

uint32_t create_thread(ppc::Context &c, uint32_t start, uint32_t context) {
    uint32_t handle_ptr = local(c, 0x200), id_ptr = local(c, 0x204);
    CHECK(kr(c, "ExCreateThread", {handle_ptr, 0, id_ptr, 0, start, context, 0}) == X_STATUS_SUCCESS);
    return K->load32(handle_ptr);
}

uint32_t create_event(ppc::Context &c) {
    uint32_t handle_ptr = local(c, 0x208);
    CHECK(kr(c, "NtCreateEvent", {handle_ptr, 0, 1, 0}) == X_STATUS_SUCCESS);
    return K->load32(handle_ptr);
}

void main_thread(ppc::Context &c) {
    ping = create_event(c), pong = create_event(c);
    std::vector<uint32_t> threads = {create_thread(c, F_SLEEPER, 1), create_thread(c, F_BUSY, 2),
                                     create_thread(c, F_SLEEPER, 3), create_thread(c, F_BUSY, 4),
                                     create_thread(c, F_PINGER, 5)};
    // the pad polled every 16 ms (a frame), between the pings
    uint32_t state = local(c, 0x100);
    for (uint32_t i = 0; i < 40; i++) {
        if (i < 20) {
            kr(c, "NtSetEvent", {ping, 0});
            kr(c, "NtWaitForSingleObjectEx", {pong, 0, 0, 0});
        }
        uint32_t r = ex(c, "xam", "XamInputGetState", {0, 0, state});
        char line[96];
        std::snprintf(line, sizeof(line), "input %u: %X packet %u buttons %04X\n", i, r, K->load32(state),
                      K->load16(state + 4));
        trace += line;
        sleep_ms(c, 16);
    }
    for (uint32_t h : threads) {
        CHECK(kr(c, "NtWaitForSingleObjectEx", {h, 0, 0, 0}) == X_STATUS_SUCCESS);
        kr(c, "NtClose", {h});
    }
    note("main done");
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_SLEEPER, sleeper},
    {F_BUSY, busy},
    {F_PINGER, pinger},
};

struct Run {
    std::string trace, input;
    uint64_t clock = 0, hash = 0;
    std::map<std::string, uint64_t> calls;
};

// input: 0 an idle pad pressing A from 200 to 400 ms, 1 that and recorded, 2
// no pad, replaying the recording
Run run(int input, const std::filesystem::path &recording) {
    Run out;
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("memory: %s\n", error.c_str());
        failures++;
        return out;
    }
    {
        Kernel k(*memory);
        K = &k;
        trace.clear();
        CHECK(k.make_deterministic({}));
        k.set_generated(generated, std::size(generated));
        qpc_thunk = memory->system_alloc(0x10);
        k.add_thunk(qpc_thunk, "xboxkrnl", "KeQueryInterruptTime");
        if (input < 2) {
            add_input_driver(k, input::make_idle_pad([](uint32_t, uint64_t now_ms) {
                input::Gamepad g;
                if (now_ms >= 200 && now_ms < 400) g.buttons = input::A, g.thumb_lx = 12000;
                return g;
            }));
        }
        if (input == 1) CHECK(record_input(k, recording, error));
        if (input == 2) CHECK(replay_input(k, recording, error));
        Objects &o = objects(k);
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
        out.trace = trace;
        for (size_t at = 0; (at = trace.find("input ", at)) != std::string::npos;) {
            size_t end = trace.find('\n', at);
            out.input += trace.substr(at, end + 1 - at);
            at = end;
        }
        out.clock = k.interrupt_time();
        out.hash = k.memory_hash();
        out.calls = k.calls();
        shutdown_timer_dpcs(k);
    }
    return out;
}

} // namespace

int main() {
    std::filesystem::path recording = std::filesystem::temp_directory_path() / "xf_deterministic_test_input.txt";
    Run first = run(1, recording);
    Run second = run(0, recording);
    Run replayed = run(2, recording);
    std::error_code ec;
    std::filesystem::remove(recording, ec);

    // the run repeats: the order, the clock, the memory, the calls
    CHECK(!first.trace.empty());
    CHECK(first.trace == second.trace);
    CHECK(first.clock == second.clock && first.clock > 0);
    CHECK(first.hash == second.hash);
    CHECK(first.calls == second.calls);
    // the busy threads were handed over by their slices: their notes interleave
    size_t busy2 = first.trace.find("busy 2 25000"), busy4 = first.trace.find("busy 4 2500\n");
    CHECK(busy2 != std::string::npos && busy4 != std::string::npos && busy4 < busy2);
    // the pad: A seen pressed, then let go, and the same with the recording
    CHECK(first.input.find("buttons 1000") != std::string::npos);
    CHECK(first.input.find("buttons 0000") != std::string::npos);
    CHECK(replayed.input == first.input);
    CHECK(replayed.trace == first.trace);
    if (first.trace != second.trace || replayed.input != first.input) {
        std::printf("first run:\n%s\nsecond run:\n%s\nreplayed:\n%s\n", first.trace.c_str(), second.trace.c_str(),
                    replayed.input.c_str());
    }
    std::printf("deterministic: clock %.4f s, %zu trace lines, memory %016llx\n", double(first.clock) / 1e7,
                size_t(std::count(first.trace.begin(), first.trace.end(), '\n')), (unsigned long long)first.hash);
    return finish("kernel_deterministic");
}
