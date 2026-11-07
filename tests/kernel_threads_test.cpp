// The kernel's threads, objects and synchronization (src/kernel/objects.h):
// guest threads made and joined, events, semaphores, mutants and timers
// signaled and waited on across host threads with timeouts, TLS slots,
// critical sections, spin locks, SLists, APCs and DPCs, handles, and guest
// stacks switched on host fibers of their own (stacks.h). The guest code is
// host functions in the generated code's table, calling the exports as the
// recompiled code reaches them.

#include "check.h"

#include "objects.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace xf::ppc {
void switch_stack(Context &c, uint32_t thunk, uint32_t resume); // ppc_generated.h
}

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
uint32_t scratch = 0; // guest memory the tests share (a page each, 16 of them)

// an export, called as the code calls it
uint32_t ex(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn("xboxkrnl", name);
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

// per-thread guest scratch below the stack pointer
uint32_t local(ppc::Context &c, uint32_t offset) { return uint32_t(c.r[1]) - 0x400 + offset; }

// a relative timeout in guest memory, for the exports that take a pointer
uint32_t timeout_ms(ppc::Context &c, int64_t ms, uint32_t slot = 0) {
    uint32_t p = local(c, 0x300 + slot * 8);
    K->store64(p, uint64_t(-ms * 10'000));
    return p;
}

void sleep_ms(ppc::Context &c, int64_t ms) { ex(c, "KeDelayExecutionThread", {1, 0, timeout_ms(c, ms, 7)}); }

uint32_t create_thread(ppc::Context &c, uint32_t start, uint32_t context, uint32_t flags = 0, uint32_t xapi = 0) {
    uint32_t handle_ptr = local(c, 0x200), id_ptr = local(c, 0x204);
    CHECK(ex(c, "ExCreateThread", {handle_ptr, 0, id_ptr, xapi, start, context, flags}) == X_STATUS_SUCCESS);
    CHECK(K->load32(id_ptr) != 0);
    return K->load32(handle_ptr);
}

uint32_t wait_handle(ppc::Context &c, uint32_t handle, int64_t ms = -1) {
    return ex(c, "NtWaitForSingleObjectEx", {handle, 0, 0, ms < 0 ? 0 : timeout_ms(c, ms)});
}

void join_and_close(ppc::Context &c, uint32_t handle) {
    CHECK(wait_handle(c, handle) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {handle}) == X_STATUS_SUCCESS);
}

uint32_t create_event(ppc::Context &c, bool manual, bool initial) {
    uint32_t handle_ptr = local(c, 0x208);
    CHECK(ex(c, "NtCreateEvent", {handle_ptr, 0, manual ? 0u : 1u, initial}) == X_STATUS_SUCCESS);
    return K->load32(handle_ptr);
}

int64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

// ---- the guest functions ----

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_RETURN_PLUS_ONE = 0x82000010,
    F_TERMINATE = 0x82000020,
    F_XAPI = 0x82000030,
    F_SET_EVENT_LATER = 0x82000040,
    F_WAIT_SEMAPHORE = 0x82000050,
    F_TAKE_MUTANT = 0x82000060,
    F_ABANDON_MUTANT = 0x82000070,
    F_CRITICAL = 0x82000080,
    F_SPIN = 0x82000090,
    F_SET_FLAG = 0x820000A0,
    F_COUNT_APC = 0x820000B0,
    F_ALERTABLE_SLEEP = 0x820000C0,
    F_DPC = 0x820000D0,
    F_TLS_OTHER = 0x820000E0,
    F_WAIT_MANUAL = 0x820000F0,
    F_APC_KERNEL = 0x82000100,
    F_FIBER = 0x82000110,
    F_FIBER_AGAIN = 0x82000120,
    F_FIBER_AGAIN2 = 0x82000130,
    F_FIBER_THREAD = 0x82000140,
    F_FIBER_EXIT = 0x82000150,
    F_TIMER_DPC = 0x82000160,
    F_SET_BODY_LATER = 0x82000170,
    F_RELEASE_BODY_LATER = 0x82000180,
    F_WAIT_BODY = 0x82000190,
};

std::atomic<uint32_t> flag{0}, apcs{0}, woken{0};
std::atomic<uint32_t> xapi_args[2];
uint32_t semaphore = 0, mutant = 0, event = 0, cs = 0, spin_lock = 0, counter = 0, tls_slot = 0;

void return_plus_one(ppc::Context &c) { c.r[3] = c.r[3] + 1; }

void terminate(ppc::Context &c) {
    ex(c, "ExTerminateThread", {uint32_t(c.r[3])});
    flag = 999; // never reached
}

void xapi(ppc::Context &c) { xapi_args[0] = uint32_t(c.r[3]), xapi_args[1] = uint32_t(c.r[4]); }

void set_event_later(ppc::Context &c) {
    uint32_t handle = uint32_t(c.r[3]);
    sleep_ms(c, 30);
    ex(c, "NtSetEvent", {handle, 0});
}

// a signal on an object's body (made with ObCreateObject), after the waiter has blocked
void set_body_later(ppc::Context &c) {
    uint32_t body = uint32_t(c.r[3]);
    sleep_ms(c, 30);
    ex(c, "KeSetEvent", {body, 0, 0});
}

void release_body_later(ppc::Context &c) {
    uint32_t body = uint32_t(c.r[3]);
    sleep_ms(c, 30);
    ex(c, "KeReleaseSemaphore", {body, 0, 1, 0});
}

// takes the mutant at the context's address (held by the creator until it releases), then releases it
void wait_body(ppc::Context &c) {
    uint32_t body = uint32_t(c.r[3]);
    if (ex(c, "KeWaitForSingleObject", {body, 0, 0, 0, 0}) == X_STATUS_SUCCESS) woken++;
    ex(c, "KeReleaseMutant", {body, 1, 0, 0});
}

void wait_semaphore(ppc::Context &c) {
    if (wait_handle(c, semaphore) == X_STATUS_SUCCESS) woken++;
}

void take_mutant(ppc::Context &c) {
    CHECK(wait_handle(c, mutant) == X_STATUS_SUCCESS);
    flag = 1;
    CHECK(ex(c, "NtReleaseMutant", {mutant, 0}) == X_STATUS_SUCCESS);
}

void abandon_mutant(ppc::Context &c) { CHECK(wait_handle(c, mutant) == X_STATUS_SUCCESS); }

// a counter in guest memory, read and written apart, behind the section
void critical(ppc::Context &c) {
    for (int i = 0; i < 300; i++) {
        ex(c, "RtlEnterCriticalSection", {cs});
        uint32_t v = K->load32(counter);
        if (i % 16 == 0) ex(c, "NtYieldExecution", {});
        K->store32(counter, v + 1);
        ex(c, "RtlLeaveCriticalSection", {cs});
    }
}

void spin(ppc::Context &c) {
    for (int i = 0; i < 300; i++) {
        uint32_t irql = ex(c, "KfAcquireSpinLock", {spin_lock});
        CHECK(K->host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] == 2);
        uint32_t v = K->load32(counter + 4);
        if (i % 32 == 0) std::this_thread::yield();
        K->store32(counter + 4, v + 1);
        ex(c, "KfReleaseSpinLock", {spin_lock, irql});
    }
}

void set_flag(ppc::Context &c) { flag = uint32_t(c.r[3]); }

// an APC's normal routine: (context, arg1, arg2)
void count_apc(ppc::Context &c) {
    CHECK(c.r[3] == 0x1234);
    apcs++;
}

// an APC's kernel routine: (apc, &normal_routine, &context, &arg1, &arg2);
// what it writes there is what the normal routine gets
void apc_kernel(ppc::Context &c) {
    CHECK(c.r[3] == scratch + 0x500);
    CHECK(K->load32(uint32_t(c.r[4])) == F_COUNT_APC && K->load32(uint32_t(c.r[5])) == 0x9999);
    K->store32(uint32_t(c.r[5]), 0x1234);
}

void alertable_sleep(ppc::Context &c) {
    flag = 1;
    uint32_t status = ex(c, "KeDelayExecutionThread", {1, 1, timeout_ms(c, 2000)});
    CHECK(status == X_STATUS_USER_APC);
    CHECK(apcs == 1);
    woken = 1;
}

// a DPC's routine: (dpc, context, arg1, arg2), at DISPATCH_LEVEL
void dpc(ppc::Context &c) {
    CHECK(c.r[4] == 0x55 && c.r[5] == 1 && c.r[6] == 2);
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::DPC_ACTIVE) == 1);
    flag = 7;
}

void tls_other(ppc::Context &c) {
    CHECK(ex(c, "KeTlsGetValue", {tls_slot}) == 0); // its own slot, not the creator's
    CHECK(ex(c, "KeTlsSetValue", {tls_slot, 77}) == 1);
    CHECK(ex(c, "KeTlsGetValue", {tls_slot}) == 77);
}

// how many threads wait on the object a handle names now
size_t waiters_on(uint32_t handle) {
    Ref<Dispatcher> d = objects(*K).lookup<Dispatcher>(handle);
    if (!d) return 0;
    std::lock_guard<std::mutex> l(objects(*K).lock);
    return d->waiters.size();
}

void wait_manual(ppc::Context &c) {
    if (wait_handle(c, event) == X_STATUS_SUCCESS) woken++;
}

// ---- the scenarios, on a guest main thread ----

void threads(ppc::Context &c) {
    // a thread's return value is its exit code, read from its KTHREAD
    uint32_t h = create_thread(c, F_RETURN_PLUS_ONE, 41);
    CHECK(wait_handle(c, h) == X_STATUS_SUCCESS);
    uint32_t kthread_ptr = local(c, 0x10);
    CHECK(ex(c, "ObReferenceObjectByHandle", {h, K->variable("ExThreadObjectType"), kthread_ptr}) == X_STATUS_SUCCESS);
    uint32_t kthread = K->load32(kthread_ptr);
    CHECK(K->load32(kthread + kthread::EXIT_STATUS) == 42);
    CHECK(K->host(kthread)[kthread::TERMINATED] == 1);
    CHECK(K->host(kthread)[header::TYPE] == uint8_t(GuestType::Thread));
    // a type that is not the object's
    CHECK(ex(c, "ObReferenceObjectByHandle", {h, K->variable("ExEventObjectType"), local(c, 0x14)}) ==
          X_STATUS_OBJECT_TYPE_MISMATCH);
    ex(c, "ObDereferenceObject", {kthread});
    CHECK(ex(c, "NtClose", {h}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {h}) == X_STATUS_INVALID_HANDLE);

    // ExTerminateThread ends it where it is
    h = create_thread(c, F_TERMINATE, 7, 0x80); // ReturnKThreadPtr: the KTHREAD, not a handle
    CHECK(h < 0xF8000000);
    uint32_t list = local(c, 0x20);
    K->store32(list, h);
    CHECK(ex(c, "KeWaitForMultipleObjects", {1, list, 1, 0, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(h + kthread::EXIT_STATUS) == 7);
    CHECK(flag == 0);

    // through XapiThreadStartup(start, context)
    h = create_thread(c, F_SET_FLAG, 0x99, 0, F_XAPI);
    join_and_close(c, h);
    CHECK(xapi_args[0] == F_SET_FLAG && xapi_args[1] == 0x99);

    // created suspended, it runs once resumed
    flag = 0;
    h = create_thread(c, F_SET_FLAG, 5, 1);
    sleep_ms(c, 20);
    CHECK(flag == 0);
    uint32_t count_ptr = local(c, 0x30);
    CHECK(ex(c, "NtResumeThread", {h, count_ptr}) == X_STATUS_SUCCESS);
    CHECK(K->load32(count_ptr) == 1);
    join_and_close(c, h);
    CHECK(flag == 5);

    // the current thread's pseudo-handle, duplicated into a real one
    uint32_t dup_ptr = local(c, 0x34);
    CHECK(ex(c, "NtDuplicateObject", {0xFFFFFFFE, dup_ptr, 0}) == X_STATUS_SUCCESS);
    uint32_t self = K->load32(dup_ptr);
    CHECK(self >= 0xF8000000);
    CHECK(ex(c, "ObReferenceObjectByHandle", {self, 0, kthread_ptr}) == X_STATUS_SUCCESS);
    CHECK(K->load32(kthread_ptr) == K->load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD));
    ex(c, "ObDereferenceObject", {K->load32(kthread_ptr)});
    CHECK(ex(c, "NtClose", {self}) == X_STATUS_SUCCESS);

    // the process type, the priority and the affinity
    CHECK(ex(c, "KeGetCurrentProcessType", {}) == kprocess::TITLE);
    uint32_t me = K->load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD);
    CHECK(ex(c, "KeSetBasePriorityThread", {me, 2}) == 0);
    CHECK(ex(c, "KeQueryBasePriorityThread", {me}) == 2);
    CHECK(ex(c, "KeSetAffinityThread", {me, 1u << 4, local(c, 0x38)}) == X_STATUS_SUCCESS);
    CHECK(K->host(uint32_t(c.r[13]))[kpcr::CURRENT_CPU] == 4);
    CHECK(ex(c, "KeSetAffinityThread", {me, 1u << 2, local(c, 0x38)}) == X_STATUS_SUCCESS);
    CHECK(K->load32(local(c, 0x38)) == 1u << 4);
}

void events(ppc::Context &c) {
    // a timeout, then the signal from another thread
    uint32_t e = create_event(c, false, false);
    auto start = std::chrono::steady_clock::now();
    CHECK(wait_handle(c, e, 20) == X_STATUS_TIMEOUT);
    CHECK(elapsed_ms(start) >= 15);
    uint32_t h = create_thread(c, F_SET_EVENT_LATER, e);
    start = std::chrono::steady_clock::now();
    CHECK(wait_handle(c, e) == X_STATUS_SUCCESS);
    CHECK(elapsed_ms(start) >= 20);
    join_and_close(c, h);
    // auto reset: taken by the wait
    CHECK(wait_handle(c, e, 0) == X_STATUS_TIMEOUT);
    uint32_t previous = local(c, 0x40);
    CHECK(ex(c, "NtSetEvent", {e, previous}) == X_STATUS_SUCCESS && K->load32(previous) == 0);
    CHECK(ex(c, "NtSetEvent", {e, previous}) == X_STATUS_SUCCESS && K->load32(previous) == 1);
    CHECK(wait_handle(c, e, 0) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {e}) == X_STATUS_SUCCESS);

    // manual reset: every wait goes until cleared; a pulse frees the waiters
    event = create_event(c, true, true);
    CHECK(wait_handle(c, event, 0) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, event, 0) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClearEvent", {event}) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, event, 0) == X_STATUS_TIMEOUT);
    woken = 0;
    uint32_t w1 = create_thread(c, F_WAIT_MANUAL, 0), w2 = create_thread(c, F_WAIT_MANUAL, 0);
    // A pulse frees only the threads already waiting, so wait until both
    // are: a fixed sleep lost one to a slow start under load (it waited
    // after the pulse, forever).
    for (int tries = 0; waiters_on(event) < 2 && tries < 5000; tries++) sleep_ms(c, 1);
    CHECK(waiters_on(event) == 2);
    CHECK(woken == 0);
    CHECK(ex(c, "NtPulseEvent", {event, previous}) == X_STATUS_SUCCESS && K->load32(previous) == 0);
    join_and_close(c, w1);
    join_and_close(c, w2);
    CHECK(woken == 2);
    CHECK(wait_handle(c, event, 0) == X_STATUS_TIMEOUT);
    CHECK(ex(c, "NtClose", {event}) == X_STATUS_SUCCESS);

    // a KEVENT the code initialized itself (the XDK inlines it), found by
    // its address
    uint32_t kevent = scratch + 0x100;
    K->host(kevent)[header::TYPE] = uint8_t(GuestType::EventSynchronization);
    K->store32(kevent + header::SIGNAL_STATE, 0);
    K->store32(kevent + header::WAIT_LIST, kevent + header::WAIT_LIST);
    K->store32(kevent + header::WAIT_LIST + 4, kevent + header::WAIT_LIST);
    CHECK(ex(c, "KeWaitForSingleObject", {kevent, 0, 0, 0, timeout_ms(c, 5)}) == X_STATUS_TIMEOUT);
    CHECK(ex(c, "KeSetEvent", {kevent, 1, 0}) == 0);
    CHECK(K->load32(kevent + header::SIGNAL_STATE) == 1); // the game sees the state
    CHECK(ex(c, "KeWaitForSingleObject", {kevent, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(kevent + header::SIGNAL_STATE) == 0);
    K->store32(kevent + header::SIGNAL_STATE, 1); // and what it writes there counts
    CHECK(ex(c, "KeWaitForSingleObject", {kevent, 0, 0, 0, timeout_ms(c, 5)}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "KeResetEvent", {kevent}) == 0);

    // named: the second creation finds the first
    uint32_t name = scratch + 0x200, string = scratch + 0x210, attributes = scratch + 0x220;
    std::memcpy(K->host(name), "Name", 4);
    K->store16(string, 4), K->store16(string + 2, 4), K->store32(string + 4, name);
    K->store32(attributes, 0), K->store32(attributes + 4, string), K->store32(attributes + 8, 0);
    uint32_t handle_ptr = local(c, 0x44);
    CHECK(ex(c, "NtCreateEvent", {handle_ptr, attributes, 0, 0}) == X_STATUS_SUCCESS);
    uint32_t named = K->load32(handle_ptr);
    CHECK(ex(c, "NtCreateEvent", {handle_ptr, attributes, 0, 0}) == X_STATUS_OBJECT_NAME_EXISTS);
    CHECK(K->load32(handle_ptr) == named);
    CHECK(ex(c, "NtCreateSemaphore", {handle_ptr, attributes, 0, 1}) == X_STATUS_INVALID_HANDLE);
    CHECK(ex(c, "NtClose", {named}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {named}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {named}) == X_STATUS_INVALID_HANDLE);
}

void semaphores(ppc::Context &c) {
    uint32_t handle_ptr = local(c, 0x50), previous = local(c, 0x54);
    CHECK(ex(c, "NtCreateSemaphore", {handle_ptr, 0, 0, 0}) == X_STATUS_INVALID_PARAMETER);
    CHECK(ex(c, "NtCreateSemaphore", {handle_ptr, 0, 0, 3}) == X_STATUS_SUCCESS);
    semaphore = K->load32(handle_ptr);
    CHECK(ex(c, "NtReleaseSemaphore", {semaphore, 4, previous}) == X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    // three waiters, released together
    woken = 0;
    uint32_t w[3];
    for (uint32_t &h : w) h = create_thread(c, F_WAIT_SEMAPHORE, 0);
    sleep_ms(c, 20);
    CHECK(woken == 0);
    CHECK(ex(c, "NtReleaseSemaphore", {semaphore, 3, previous}) == X_STATUS_SUCCESS);
    CHECK(K->load32(previous) == 0);
    for (uint32_t h : w) join_and_close(c, h);
    CHECK(woken == 3);
    CHECK(wait_handle(c, semaphore, 0) == X_STATUS_TIMEOUT);
    CHECK(ex(c, "NtReleaseSemaphore", {semaphore, 2, previous}) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, semaphore, 0) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, semaphore, 0) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, semaphore, 0) == X_STATUS_TIMEOUT);
    CHECK(ex(c, "NtClose", {semaphore}) == X_STATUS_SUCCESS);
}

void mutants(ppc::Context &c) {
    uint32_t handle_ptr = local(c, 0x60), previous = local(c, 0x64);
    CHECK(ex(c, "NtCreateMutant", {handle_ptr, 0, 1}) == X_STATUS_SUCCESS);
    mutant = K->load32(handle_ptr);
    // owned here: recursion, another thread waits
    CHECK(wait_handle(c, mutant, 0) == X_STATUS_SUCCESS);
    flag = 0;
    uint32_t h = create_thread(c, F_TAKE_MUTANT, 0);
    sleep_ms(c, 20);
    CHECK(flag == 0);
    CHECK(ex(c, "NtReleaseMutant", {mutant, previous}) == X_STATUS_SUCCESS);
    CHECK(int32_t(K->load32(previous)) == -1);
    sleep_ms(c, 10);
    CHECK(flag == 0);
    CHECK(ex(c, "NtReleaseMutant", {mutant, previous}) == X_STATUS_SUCCESS);
    join_and_close(c, h);
    CHECK(flag == 1);
    CHECK(ex(c, "NtReleaseMutant", {mutant, 0}) == X_STATUS_MUTANT_NOT_OWNED);
    // a thread that ends holding it abandons it
    h = create_thread(c, F_ABANDON_MUTANT, 0);
    join_and_close(c, h);
    CHECK(wait_handle(c, mutant, 0) == X_STATUS_ABANDONED_WAIT_0);
    CHECK(ex(c, "NtReleaseMutant", {mutant, 0}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClose", {mutant}) == X_STATUS_SUCCESS);
}

void multiple(ppc::Context &c) {
    uint32_t a = create_event(c, true, false), b = create_event(c, false, false);
    uint32_t handles = local(c, 0x70);
    K->store32(handles, a), K->store32(handles + 4, b);
    // any: the index of the signaled one
    CHECK(ex(c, "NtWaitForMultipleObjectsEx", {2, handles, 1, 0, 0, timeout_ms(c, 5)}) == X_STATUS_TIMEOUT);
    ex(c, "NtSetEvent", {b, 0});
    CHECK(ex(c, "NtWaitForMultipleObjectsEx", {2, handles, 1, 0, 0, timeout_ms(c, 5)}) == 1);
    // all: only with both, and the auto-reset one taken
    ex(c, "NtSetEvent", {a, 0});
    CHECK(ex(c, "NtWaitForMultipleObjectsEx", {2, handles, 0, 0, 0, timeout_ms(c, 5)}) == X_STATUS_TIMEOUT);
    ex(c, "NtSetEvent", {b, 0});
    CHECK(ex(c, "NtWaitForMultipleObjectsEx", {2, handles, 0, 0, 0, timeout_ms(c, 5)}) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, b, 0) == X_STATUS_TIMEOUT);
    CHECK(wait_handle(c, a, 0) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtWaitForMultipleObjectsEx", {0, handles, 1, 0, 0, 0}) == X_STATUS_INVALID_PARAMETER);
    ex(c, "NtClose", {a}), ex(c, "NtClose", {b});
}

void timers(ppc::Context &c) {
    uint32_t handle_ptr = local(c, 0x80);
    CHECK(ex(c, "NtCreateTimer", {handle_ptr, 0, 0}) == X_STATUS_SUCCESS);
    uint32_t t = K->load32(handle_ptr);
    auto start = std::chrono::steady_clock::now();
    CHECK(ex(c, "NtSetTimerEx", {t, timeout_ms(c, 25, 1), 0, 1, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(wait_handle(c, t, 0) == X_STATUS_TIMEOUT);
    CHECK(wait_handle(c, t) == X_STATUS_SUCCESS);
    CHECK(elapsed_ms(start) >= 20);
    CHECK(wait_handle(c, t, 0) == X_STATUS_SUCCESS); // notification: stays signaled
    CHECK(ex(c, "NtClose", {t}) == X_STATUS_SUCCESS);

    // synchronization, periodic: one wait per period
    CHECK(ex(c, "NtCreateTimer", {handle_ptr, 0, 1}) == X_STATUS_SUCCESS);
    t = K->load32(handle_ptr);
    start = std::chrono::steady_clock::now();
    CHECK(ex(c, "NtSetTimerEx", {t, timeout_ms(c, 5, 1), 0, 1, 0, 0, 10, 0}) == X_STATUS_SUCCESS);
    for (int i = 0; i < 3; i++) CHECK(wait_handle(c, t, 1000) == X_STATUS_SUCCESS);
    CHECK(elapsed_ms(start) >= 20);
    uint32_t state = local(c, 0x84);
    CHECK(ex(c, "NtCancelTimer", {t, state}) == X_STATUS_SUCCESS);
    wait_handle(c, t, 0); // a period may have ended before the cancel
    CHECK(wait_handle(c, t, 30) == X_STATUS_TIMEOUT);

    // with a routine: a user APC on this thread, run in an alertable wait
    apcs = 0;
    CHECK(ex(c, "NtSetTimerEx", {t, timeout_ms(c, 5, 1), F_COUNT_APC, 1, 0x1234, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "KeDelayExecutionThread", {1, 1, timeout_ms(c, 2000)}) == X_STATUS_USER_APC);
    CHECK(apcs == 1);
    // closed while it runs: it stops
    CHECK(ex(c, "NtSetTimerEx", {t, timeout_ms(c, 5, 1), 0, 1, 0, 0, 10, 0}) == X_STATUS_SUCCESS);
    {
        std::lock_guard<std::mutex> l(objects(*K).lock);
        CHECK(objects(*K).next_due() != Scheduler::NEVER);
    }
    CHECK(ex(c, "NtClose", {t}) == X_STATUS_SUCCESS);
    std::lock_guard<std::mutex> l(objects(*K).lock);
    CHECK(objects(*K).next_due() == Scheduler::NEVER);
}

// a timer's DPC, run on the DPC thread: (dpc, context, system time low, high)
std::atomic<uint32_t> timer_dpc_runs{0};
void timer_dpc(ppc::Context &c) {
    CHECK(c.r[4] == 0x66 && c.r[6] != 0); // the time's high word is set from 2009 on
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::DPC_ACTIVE) == 1);
    CHECK(K->host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] == 2);
    timer_dpc_runs++;
}

// KeSetTimerEx, KeSetTimer and KeCancelTimer (NT's; Xenia has none): a due
// timer is signaled and its DPC queued to a kernel thread
void timer_dpcs(ppc::Context &c) {
    uint32_t timer = scratch + 0x800, dpc = scratch + 0x840;
    auto relative_ms = [](int64_t ms) { return uint64_t(-ms * 10'000); };
    auto wait_runs = [&](uint32_t n) {
        for (int i = 0; i < 400 && timer_dpc_runs < n; i++) sleep_ms(c, 5);
        return timer_dpc_runs.load() >= n;
    };
    ex(c, "KeInitializeTimerEx", {timer, 0, 1});
    ex(c, "KeInitializeDpc", {dpc, F_TIMER_DPC, 0x66});
    timer_dpc_runs = 0;

    // once: signaled, the DPC run on another thread
    CHECK(ex(c, "KeSetTimerEx", {timer, relative_ms(5), 0, dpc}) == 0);
    CHECK(wait_runs(1));
    CHECK(ex(c, "KeWaitForSingleObject", {timer, 0, 0, 0, timeout_ms(c, 1000, 2)}) == X_STATUS_SUCCESS);
    sleep_ms(c, 30);
    CHECK(timer_dpc_runs == 1);
    CHECK(ex(c, "KeCancelTimer", {timer}) == 0); // it had come due: no longer set

    // periodic: once a period; cancelled, it stops; set again, it says so
    CHECK(ex(c, "KeSetTimerEx", {timer, relative_ms(5), 10, dpc}) == 0);
    CHECK(wait_runs(4));
    CHECK(ex(c, "KeSetTimerEx", {timer, relative_ms(3'600'000), 0, dpc}) == 1);
    uint32_t seen = timer_dpc_runs;
    sleep_ms(c, 40);
    CHECK(timer_dpc_runs <= seen + 1);
    CHECK(ex(c, "KeCancelTimer", {timer}) == 1 && ex(c, "KeCancelTimer", {timer}) == 0);
    seen = timer_dpc_runs;
    sleep_ms(c, 30);
    CHECK(timer_dpc_runs == seen);

    // no DPC: only signaled (KeSetTimer: the same, a DPC the third argument)
    CHECK(ex(c, "KeSetTimer", {timer, relative_ms(5), 0}) == 0);
    CHECK(ex(c, "KeWaitForSingleObject", {timer, 0, 0, 0, timeout_ms(c, 1000, 2)}) == X_STATUS_SUCCESS);
    CHECK(timer_dpc_runs == seen);
}

void tls(ppc::Context &c) {
    tls_slot = ex(c, "KeTlsAlloc", {});
    CHECK(tls_slot == 0);
    CHECK(ex(c, "KeTlsAlloc", {}) == 1);
    CHECK(ex(c, "KeTlsSetValue", {tls_slot, 1234}) == 1);
    uint32_t h = create_thread(c, F_TLS_OTHER, 0);
    join_and_close(c, h);
    CHECK(ex(c, "KeTlsGetValue", {tls_slot}) == 1234);
    CHECK(ex(c, "KeTlsFree", {tls_slot}) == 1);
    CHECK(ex(c, "KeTlsGetValue", {tls_slot}) == 0); // freed: cleared in every thread
    CHECK(ex(c, "KeTlsAlloc", {}) == tls_slot);
    CHECK(ex(c, "KeTlsSetValue", {4096, 1}) == 0);
    // all 64 slots, then none
    uint32_t n = 2;
    while (ex(c, "KeTlsAlloc", {}) != 0xFFFFFFFF) n++;
    CHECK(n == 64);
}

void locks(ppc::Context &c) {
    // critical sections: four threads, a counter only the section keeps right
    cs = scratch + 0x300, counter = scratch + 0x340, spin_lock = scratch + 0x350;
    K->store32(counter, 0), K->store32(counter + 4, 0), K->store32(spin_lock, 0);
    ex(c, "RtlInitializeCriticalSectionAndSpinCount", {cs, 300});
    CHECK(K->host(cs)[header::ABSOLUTE] == 2);
    CHECK(int32_t(K->load32(cs + 0x10)) == -1);
    uint32_t w[4];
    for (uint32_t &h : w) h = create_thread(c, F_CRITICAL, 0);
    for (uint32_t h : w) join_and_close(c, h);
    CHECK(K->load32(counter) == 4 * 300);
    CHECK(int32_t(K->load32(cs + 0x10)) == -1 && K->load32(cs + 0x18) == 0);
    // recursion
    ex(c, "RtlEnterCriticalSection", {cs});
    CHECK(ex(c, "RtlTryEnterCriticalSection", {cs}) == 1);
    CHECK(K->load32(cs + 0x14) == 2);
    ex(c, "RtlLeaveCriticalSection", {cs});
    ex(c, "RtlLeaveCriticalSection", {cs});
    CHECK(int32_t(K->load32(cs + 0x10)) == -1);

    // spin locks
    for (uint32_t &h : w) h = create_thread(c, F_SPIN, 0);
    for (uint32_t h : w) join_and_close(c, h);
    CHECK(K->load32(counter + 4) == 4 * 300);
    CHECK(K->load32(spin_lock) == 0);
    CHECK(K->host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] == 0);

    // critical regions: the KTHREAD's APC disable count
    uint32_t me = K->load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD);
    ex(c, "KeEnterCriticalRegion", {});
    CHECK(int32_t(K->load32(me + kthread::APC_DISABLE_COUNT)) == -1);
    ex(c, "KeLeaveCriticalRegion", {});
    CHECK(K->load32(me + kthread::APC_DISABLE_COUNT) == 0);

    // the initializers leave an empty wait list, as the console's do (the oracle):
    // both links to the list head's own address, +8 and +0xC; the spin-count
    // form of the critical section returns its pointer
    uint32_t block = scratch + 0x900;
    auto empty_list = [&](uint32_t header_address) {
        return K->load32(header_address + 8) == header_address + 8 &&
               K->load32(header_address + 0xC) == header_address + 8;
    };
    std::memset(K->host(block), 0xFF, 0x100);
    ex(c, "KeInitializeEvent", {block, 0, 1});
    ex(c, "KeInitializeEvent", {block + 0x10, 1, 0});
    ex(c, "KeInitializeSemaphore", {block + 0x20, 2, 5});
    ex(c, "KeInitializeMutant", {block + 0x40, 0});
    ex(c, "KeInitializeTimerEx", {block + 0x60, 0, 0});
    CHECK(empty_list(block) && empty_list(block + 0x10) && empty_list(block + 0x20) && empty_list(block + 0x40) &&
          empty_list(block + 0x60));
    std::memset(K->host(block + 0x80), 0xFF, 0x80);
    ex(c, "ExInitializeReadWriteLock", {block + 0x80});
    CHECK(empty_list(block + 0x80 + 0x10) && empty_list(block + 0x80 + 0x20));
    ex(c, "RtlInitializeCriticalSection", {block + 0xE0});
    CHECK(empty_list(block + 0xE0));
    std::memset(K->host(block + 0xE0), 0xFF, 0x20);
    CHECK(ex(c, "RtlInitializeCriticalSectionAndSpinCount", {block + 0xE0, 0}) == block + 0xE0);
    CHECK(empty_list(block + 0xE0));
    // the host objects behind them still work, found without a marker: a wait
    // satisfied by the header's state, the signal and the count seen
    CHECK(ex(c, "KeWaitForSingleObject", {block, 0, 0, 0, 0}) == X_STATUS_SUCCESS); // signaled notification event
    CHECK(ex(c, "KeResetEvent", {block}) == 1);
    CHECK(K->load32(block + 4) == 0 && empty_list(block));
    ex(c, "KeSetEvent", {block, 0, 0});
    CHECK(K->load32(block + 4) == 1 && empty_list(block));
    CHECK(ex(c, "KeReleaseSemaphore", {block + 0x20, 0, 1, 0}) == 2 && K->load32(block + 0x24) == 3);
    // an event re-initialized as another kind keeps no host object of the old
    std::memset(K->host(block + 0x20), 0xFF, 0x10);
    ex(c, "KeInitializeEvent", {block + 0x20, 1, 1});
    CHECK(ex(c, "KeWaitForSingleObject", {block + 0x20, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(block + 0x24) == 0 && empty_list(block + 0x20)); // auto-reset: taken
}

// Objects of the Ex types made with ObCreateObject, their dispatcher headers
// initialized by the caller (the XDK inlines it): a host object is made on
// first use, as Xenia's XObject::GetNativeObject does, and waits and signals
// across threads work on the body, as does a handle opened to it.
uint32_t body_of(ppc::Context &c, const char *type_name, uint32_t size) {
    uint32_t out = local(c, 0x3F0);
    CHECK(ex(c, "ObCreateObject", {K->variable(type_name), 0, size, out}) == X_STATUS_SUCCESS);
    return K->load32(out);
}

void created_dispatchers(ppc::Context &c) {
    uint32_t manual = body_of(c, "ExEventObjectType", 0x10), autoreset = body_of(c, "ExEventObjectType", 0x10);
    ex(c, "KeInitializeEvent", {manual, 0, 0});
    ex(c, "KeInitializeEvent", {autoreset, 1, 0});
    CHECK(ex(c, "KeWaitForSingleObject", {manual, 0, 0, 0, timeout_ms(c, 10)}) == X_STATUS_TIMEOUT);
    uint32_t id = local(c, 0x3E0);
    CHECK(ex(c, "ExCreateThread", {id, 0, id + 4, 0, F_SET_BODY_LATER, manual, 0}) == X_STATUS_SUCCESS);
    uint32_t th = K->load32(id);
    auto started = std::chrono::steady_clock::now();
    CHECK(ex(c, "KeWaitForSingleObject", {manual, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(elapsed_ms(started) >= 20);
    CHECK(K->load32(manual + 4) == 1); // a notification event stays signaled
    CHECK(ex(c, "KeResetEvent", {manual}) == 1 && K->load32(manual + 4) == 0);
    join_and_close(c, th);
    // the synchronization event is taken by the wait it satisfies
    CHECK(ex(c, "ExCreateThread", {id, 0, id + 4, 0, F_SET_BODY_LATER, autoreset, 0}) == X_STATUS_SUCCESS);
    th = K->load32(id);
    CHECK(ex(c, "KeWaitForSingleObject", {autoreset, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(autoreset + 4) == 0);
    join_and_close(c, th);
    // a handle to the body is the same host object: a wait on it sees the body's set
    uint32_t handle_ptr = local(c, 0x3E8);
    CHECK(ex(c, "ObOpenObjectByPointer", {manual, handle_ptr}) == X_STATUS_SUCCESS);
    uint32_t handle = K->load32(handle_ptr);
    CHECK(handle && wait_handle(c, handle, 10) == X_STATUS_TIMEOUT);
    ex(c, "KeSetEvent", {manual, 0, 0});
    CHECK(wait_handle(c, handle, 10) == X_STATUS_SUCCESS);
    CHECK(ex(c, "NtClearEvent", {handle}) == X_STATUS_SUCCESS && K->load32(manual + 4) == 0);
    CHECK(ex(c, "ExCreateThread", {id, 0, id + 4, 0, F_SET_BODY_LATER, manual, 0}) == X_STATUS_SUCCESS);
    th = K->load32(id);
    CHECK(wait_handle(c, handle) == X_STATUS_SUCCESS); // blocks on the handle until the body is set
    join_and_close(c, th);
    ex(c, "NtClose", {handle});

    uint32_t sem = body_of(c, "ExSemaphoreObjectType", 0x14);
    ex(c, "KeInitializeSemaphore", {sem, 0, 3});
    CHECK(ex(c, "KeWaitForSingleObject", {sem, 0, 0, 0, timeout_ms(c, 10)}) == X_STATUS_TIMEOUT);
    CHECK(ex(c, "ExCreateThread", {id, 0, id + 4, 0, F_RELEASE_BODY_LATER, sem, 0}) == X_STATUS_SUCCESS);
    th = K->load32(id);
    CHECK(ex(c, "KeWaitForSingleObject", {sem, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(sem + 4) == 0);
    join_and_close(c, th);
    // released through a handle, taken through the body
    CHECK(ex(c, "ObOpenObjectByPointer", {sem, handle_ptr}) == X_STATUS_SUCCESS);
    handle = K->load32(handle_ptr);
    CHECK(ex(c, "NtReleaseSemaphore", {handle, 2, local(c, 0x3EC)}) == X_STATUS_SUCCESS && K->load32(sem + 4) == 2);
    CHECK(ex(c, "KeWaitForSingleObject", {sem, 0, 0, 0, 0}) == X_STATUS_SUCCESS && K->load32(sem + 4) == 1);
    ex(c, "NtClose", {handle});

    // a mutant, held here while a thread waits for it
    uint32_t mu = body_of(c, "ExMutantObjectType", 0x20);
    ex(c, "KeInitializeMutant", {mu, 1});
    woken = 0;
    CHECK(ex(c, "ExCreateThread", {id, 0, id + 4, 0, F_WAIT_BODY, mu, 0}) == X_STATUS_SUCCESS);
    th = K->load32(id);
    sleep_ms(c, 30);
    CHECK(woken == 0);
    CHECK(ex(c, "KeReleaseMutant", {mu, 1, 0, 0}) == 0);
    join_and_close(c, th);
    CHECK(woken == 1);
}

void slists(ppc::Context &c) {
    // header: next, depth, sequence; entries: next
    uint32_t head = scratch + 0x400, e1 = scratch + 0x410, e2 = scratch + 0x420;
    K->store32(head, e1), K->store16(head + 4, 2), K->store16(head + 6, 9);
    K->store32(e1, e2), K->store32(e2, 0);
    CHECK(ex(c, "InterlockedPopEntrySList", {head}) == e1);
    CHECK(K->load32(head) == e2 && K->load16(head + 4) == 1 && K->load16(head + 6) == 9);
    K->store32(head, e1), K->store16(head + 4, 2);
    CHECK(ex(c, "InterlockedFlushSList", {head}) == e1);
    CHECK(K->load64(head) == 0);
    CHECK(ex(c, "InterlockedPopEntrySList", {head}) == 0);
}

// ObCreateSymbolicLink keeps the link for the file system, without the
// object manager's \??\ prefix
void links(ppc::Context &c) {
    auto ansi = [&](uint32_t at, const char *text) {
        uint16_t n = uint16_t(std::strlen(text));
        std::memcpy(K->host(at + 8), text, n);
        K->store16(at, n), K->store16(at + 2, n), K->store32(at + 4, at + 8);
        return at;
    };
    uint32_t path = ansi(scratch + 0x700, "\\??\\D:"), target = ansi(scratch + 0x740, "\\Device\\Cdrom0\\.\\");
    CHECK(ex(c, "ObCreateSymbolicLink", {path, target}) == X_STATUS_SUCCESS);
    {
        std::lock_guard<std::mutex> l(objects(*K).lock);
        auto &list = objects(*K).symbolic_links;
        CHECK(list.size() == 1 && list[0].first == "D:" && list[0].second == "\\Device\\Cdrom0");
    }
    CHECK(ex(c, "ObDeleteSymbolicLink", {ansi(scratch + 0x780, "\\??\\d:")}) == X_STATUS_SUCCESS);
    CHECK(ex(c, "ObDeleteSymbolicLink", {path}) == X_STATUS_UNSUCCESSFUL);
}

void apcs_and_dpcs(ppc::Context &c) {
    // a user APC to a thread in an alertable wait wakes it
    apcs = 0, flag = 0, woken = 0;
    uint32_t kthread_ptr = local(c, 0x90);
    uint32_t h = create_thread(c, F_ALERTABLE_SLEEP, 0);
    CHECK(ex(c, "ObReferenceObjectByHandle", {h, 0, kthread_ptr}) == X_STATUS_SUCCESS);
    uint32_t target = K->load32(kthread_ptr);
    while (!flag) ex(c, "NtYieldExecution", {});
    sleep_ms(c, 10);
    uint32_t apc = scratch + 0x500;
    ex(c, "KeInitializeApc", {apc, target, F_APC_KERNEL, 0, F_COUNT_APC, 1, 0x9999});
    CHECK(K->host(apc)[kapc::MODE] == 1 && K->load16(apc) == 18);
    CHECK(ex(c, "KeInsertQueueApc", {apc, 0, 0, 0}) == 1);
    join_and_close(c, h);
    CHECK(woken == 1);
    ex(c, "ObDereferenceObject", {target});

    // a DPC runs at once, at DISPATCH_LEVEL
    flag = 0;
    uint32_t d = scratch + 0x600;
    ex(c, "KeInitializeDpc", {d, F_DPC, 0x55});
    CHECK(K->load16(d) == 19);
    CHECK(ex(c, "KeInsertQueueDpc", {d, 1, 2}) == 1);
    CHECK(flag == 7);
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::DPC_ACTIVE) == 0);
    // what the console's dispatch leaves in a DPC it has run (the kernel oracle): not inserted, its list entry still
    // pointing at the processor's queue
    uint32_t queue = uint32_t(c.r[13]) + kpcr::DPC_QUEUE;
    CHECK(K->host(d)[kdpc::SELECTED_CPU] == 0 && K->load32(d + kdpc::LIST) == queue &&
          K->load32(d + kdpc::LIST + 4) == queue);

    // at DISPATCH_LEVEL a DPC waits for the IRQL to fall, and queued twice it is queued once, with the first
    // arguments (the console's: KeInsertQueueDpc answers 0 for a DPC that is queued)
    flag = 0;
    ex(c, "KfRaiseIrql", {2});
    CHECK(ex(c, "KeInsertQueueDpc", {d, 1, 2}) == 1);
    CHECK(ex(c, "KeInsertQueueDpc", {d, 3, 4}) == 0);
    CHECK(flag == 0 && K->host(d)[kdpc::SELECTED_CPU] != 0);
    ex(c, "KfLowerIrql", {0});
    CHECK(flag == 7 && K->host(d)[kdpc::SELECTED_CPU] == 0);
    // queued and removed before the IRQL falls, it never runs
    flag = 0;
    ex(c, "KfRaiseIrql", {2});
    CHECK(ex(c, "KeInsertQueueDpc", {d, 1, 2}) == 1 && ex(c, "KeRemoveQueueDpc", {d}) == 1);
    ex(c, "KfLowerIrql", {0});
    CHECK(flag == 0 && ex(c, "KeRemoveQueueDpc", {d}) == 0);

    // a timer set for a time that has passed is signaled in the call (the console's insert finds it expired)
    uint32_t timer = scratch + 0x6C0;
    ex(c, "KeInitializeTimerEx", {timer, 0, 0});
    CHECK(K->load32(timer + 4) == 0);
    ex(c, "KeSetTimerEx", {timer, 0, 0, 0});
    CHECK(K->load32(timer + 4) == 1);

    // an owned mutant is on its owner's list in the guest, as the console keeps it, and off it once released (its
    // own links stay)
    uint32_t me_thread = K->load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD), mu = scratch + 0x680;
    ex(c, "KeInitializeMutant", {mu, 1});
    CHECK(K->load32(me_thread + kthread::MUTANTS_LIST) == mu + kmutant::LIST &&
          K->load32(mu + kmutant::OWNER) == me_thread);
    CHECK(ex(c, "KeReleaseMutant", {mu, 1, 0, 0}) == 0);
    CHECK(K->load32(me_thread + kthread::MUTANTS_LIST) == me_thread + kthread::MUTANTS_LIST);

    // the wait blocks of a wait for several objects, filled as the console fills them: a wait for any stops at the
    // first object that is signaled (its block and the ones after are left alone)
    uint32_t ev1 = scratch + 0x700, ev2 = scratch + 0x720, objs = scratch + 0x740, blocks = scratch + 0x750,
             zero = scratch + 0x790;
    std::memset(K->host(scratch + 0x700), 0, 0xA0);
    ex(c, "KeInitializeEvent", {ev1, 0, 0});
    ex(c, "KeInitializeEvent", {ev2, 0, 1});
    K->store32(objs, ev1), K->store32(objs + 4, ev2);
    CHECK(ex(c, "KeWaitForMultipleObjects", {2, objs, 1, 0, 0, 0, zero, blocks}) == 1);
    CHECK(K->load32(blocks + kwait_block::THREAD) == me_thread && K->load32(blocks + kwait_block::OBJECT) == ev1 &&
          K->load32(blocks + kwait_block::NEXT) == blocks + kwait_block::SIZE &&
          K->load16(blocks + kwait_block::KEY) == 0 && K->load16(blocks + kwait_block::TYPE) == 1);
    CHECK(K->load32(blocks + kwait_block::SIZE + kwait_block::OBJECT) == 0);
    // a wait for all that is satisfied closes the ring: the last block points to the first
    ex(c, "KeSetEvent", {ev1, 0, 0});
    CHECK(ex(c, "KeWaitForMultipleObjects", {2, objs, 0, 0, 0, 0, zero, blocks}) == X_STATUS_SUCCESS);
    CHECK(K->load32(blocks + kwait_block::SIZE + kwait_block::OBJECT) == ev2 &&
          K->load32(blocks + kwait_block::SIZE + kwait_block::NEXT) == blocks);
    CHECK(K->load32(me_thread + kthread::WAIT_BLOCKS) == blocks);

    // an affinity with a processor that is not there is refused (the console wants the mask within the six)
    CHECK(ex(c, "KeSetAffinityThread", {me_thread, 0x40, local(c, 0x38)}) == X_STATUS_INVALID_PARAMETER);

    // the owner of the FPU state, as the console keeps it: none (-1) before a save, which answers the previous owner,
    // and none again after the restore
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::FPU_OWNER) == 0xFFFFFFFF);
    CHECK(ex(c, "KeSaveFloatingPointState", {}) == 0xFFFFFFFF);
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::FPU_OWNER) == uint32_t(c.r[13]) + 0x300);
    ex(c, "KeRestoreFloatingPointState", {});
    CHECK(K->load32(uint32_t(c.r[13]) + kpcr::FPU_OWNER) == 0xFFFFFFFF);
}

// ---- guest stacks on host fibers ----
//
// The guest's SwitchToFiber as generated code reaches it: LR and r1 those
// the other stack goes on with, then KeSetCurrentStackPointers' thunk
// through ppc::switch_stack, with where this code goes on when its stack is
// switched back to. The fibers' functions are host code in the generated
// table; their locals live on the host stack each guest stack runs on.

uint32_t ksp_thunk = 0;
uint32_t own_base = 0, own_limit = 0, own_sp = 0, fiber_top = 0, fiber_sp = 0;
std::vector<int> trace;
std::atomic<uint32_t> fiber_thread_after{0};
constexpr uint32_t RET_MAIN_1 = 0x82100000, RET_MAIN_2 = 0x82100004, RET_MAIN_3 = 0x82100008, RET_MAIN_4 = 0x8210000C,
                   RET_FIBER_1 = 0x82100010, RET_FIBER_2 = 0x82100014, RET_FIBER_3 = 0x82100018,
                   RET_FIBER_4 = 0x8210001C;

void switch_to(ppc::Context &c, uint32_t sp, uint32_t base, uint32_t limit, uint32_t lr, uint32_t resume) {
    c.r[3] = sp, c.r[4] = K->load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD);
    c.r[5] = base, c.r[6] = base, c.r[7] = limit;
    c.lr = lr;
    ppc::switch_stack(c, ksp_thunk, resume);
}

// F_FIBER: the fiber's first run, from its stack's top
void fiber(ppc::Context &c) {
    int mine = 7;
    trace.push_back(uint32_t(c.r[1]) == fiber_top - 0x100 ? 1 : -1);
    c.r[1] -= 0x40; // a frame of its own
    fiber_sp = uint32_t(c.r[1]);
    switch_to(c, own_sp, own_base, own_limit, RET_MAIN_1, RET_FIBER_1);
    // switched back to where it left: its host frame as it was
    trace.push_back(mine == 7 && uint32_t(c.r[1]) == fiber_sp ? 2 : -2);
    switch_to(c, own_sp, own_base, own_limit, RET_MAIN_2, RET_FIBER_2);
    trace.push_back(-4); // never: its stack starts over above this frame
}

// F_FIBER_AGAIN: the fiber started over from its top; the Unwind went past
// fiber()'s frame to the stack's start, which runs this
void fiber_again(ppc::Context &c) {
    trace.push_back(uint32_t(c.r[1]) == fiber_top - 0x100 ? 3 : -3);
    switch_to(c, own_sp, own_base, own_limit, RET_MAIN_3, RET_FIBER_3);
    trace.push_back(-5); // never: switched back to with the same r1 elsewhere
}

// F_FIBER_AGAIN2: switched back to with fiber_again()'s r1, at a function's
// start: the switch's own dispatch, whose r1 that is, runs it
void fiber_again2(ppc::Context &c) {
    trace.push_back(uint32_t(c.r[1]) == fiber_top - 0x100 ? 6 : -6);
    switch_to(c, own_sp, own_base, own_limit, RET_MAIN_4, RET_FIBER_4);
    trace.push_back(-7); // never: its stack is freed while it waits
}

// F_FIBER_THREAD: a thread that moves to a fiber which ends it
void fiber_thread(ppc::Context &c) {
    Thread *t = current_thread();
    uint32_t top = ex(c, "MmCreateKernelStack", {0x10000, 0});
    switch_to(c, top - 0x100, top, top - 0x10000, F_FIBER_EXIT, 0x82100020);
    fiber_thread_after = t->id; // never: the thread ended on the fiber
}
void fiber_exit(ppc::Context &c) { ex(c, "ExTerminateThread", {0x77}); }

void fibers(ppc::Context &c) {
    Thread *t = current_thread();
    own_base = t->stack_base, own_limit = t->stack_limit, own_sp = uint32_t(c.r[1]);
    const uint64_t interpreted = K->interpreted_entries();
    fiber_top = ex(c, "MmCreateKernelStack", {0x10000, 0});
    CHECK(fiber_top >= 0x70000000 && fiber_top < 0x7F000000);
    int mine = 42;
    // to its own stack: nothing moves
    switch_to(c, own_sp, own_base, own_limit, RET_MAIN_1, RET_MAIN_1);
    CHECK(t->stacks && t->stacks->stacks.size() == 1 && uint32_t(c.r[1]) == own_sp && trace.empty());
    // the fiber's stack: a host fiber of its own, from F_FIBER
    switch_to(c, fiber_top - 0x100, fiber_top, fiber_top - 0x10000, F_FIBER, RET_MAIN_1);
    CHECK(mine == 42 && uint32_t(c.r[1]) == own_sp && (trace == std::vector<int>{1}));
    CHECK(t->stacks->stacks.size() == 2);
    // back where the fiber left
    switch_to(c, fiber_sp, fiber_top, fiber_top - 0x10000, RET_FIBER_1, RET_MAIN_2);
    CHECK(mine == 42 && uint32_t(c.r[1]) == own_sp && (trace == std::vector<int>{1, 2}));
    // the fiber starting over from its top elsewhere: its frames are unwound
    switch_to(c, fiber_top - 0x100, fiber_top, fiber_top - 0x10000, F_FIBER_AGAIN, RET_MAIN_3);
    CHECK(mine == 42 && (trace == std::vector<int>{1, 2, 3}));
    // with the r1 it left with, elsewhere than where it waits
    switch_to(c, fiber_top - 0x100, fiber_top, fiber_top - 0x10000, F_FIBER_AGAIN2, RET_MAIN_4);
    CHECK(mine == 42 && uint32_t(c.r[1]) == own_sp && (trace == std::vector<int>{1, 2, 3, 6}));
    CHECK(K->interpreted_entries() == interpreted); // all of it in the generated table's code
    // its stack freed: its host fiber goes (deleted from this one)
    CHECK(ex(c, "MmDeleteKernelStack", {fiber_top, fiber_top - 0x10000}) == X_STATUS_SUCCESS);
    CHECK(t->stacks->stacks.size() == 1 && t->stacks->dead.size() == 1);

    // ExitThread on a fiber ends its thread, thrown again on the thread's own stack
    uint32_t kthread = create_thread(c, F_FIBER_THREAD, 0, 0x80);
    uint32_t list = local(c, 0x20);
    K->store32(list, kthread);
    CHECK(ex(c, "KeWaitForMultipleObjects", {1, list, 1, 0, 0, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(K->load32(kthread + kthread::EXIT_STATUS) == 0x77 && fiber_thread_after == 0);
}

void main_thread(ppc::Context &c) {
    threads(c);
    events(c);
    semaphores(c);
    mutants(c);
    multiple(c);
    timers(c);
    timer_dpcs(c);
    tls(c);
    locks(c);
    created_dispatchers(c);
    slists(c);
    links(c);
    apcs_and_dpcs(c);
    fibers(c);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_RETURN_PLUS_ONE, return_plus_one},
    {F_TERMINATE, terminate},
    {F_XAPI, xapi},
    {F_SET_EVENT_LATER, set_event_later},
    {F_WAIT_SEMAPHORE, wait_semaphore},
    {F_TAKE_MUTANT, take_mutant},
    {F_ABANDON_MUTANT, abandon_mutant},
    {F_CRITICAL, critical},
    {F_SPIN, spin},
    {F_SET_FLAG, set_flag},
    {F_COUNT_APC, count_apc},
    {F_ALERTABLE_SLEEP, alertable_sleep},
    {F_DPC, dpc},
    {F_TLS_OTHER, tls_other},
    {F_WAIT_MANUAL, wait_manual},
    {F_APC_KERNEL, apc_kernel},
    {F_FIBER, fiber},
    {F_FIBER_AGAIN, fiber_again},
    {F_FIBER_AGAIN2, fiber_again2},
    {F_FIBER_THREAD, fiber_thread},
    {F_FIBER_EXIT, fiber_exit},
    {F_TIMER_DPC, timer_dpc},
    {F_SET_BODY_LATER, set_body_later},
    {F_RELEASE_BODY_LATER, release_body_later},
    {F_WAIT_BODY, wait_body},
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
        scratch = memory->system_alloc(0x1000);
        ksp_thunk = memory->system_alloc(0x10);
        k.add_thunk(ksp_thunk, "xboxkrnl", "KeSetCurrentStackPointers");
        Objects &o = objects(k);
        o.set_tls(64, 0, 0, 0); // as an executable's TLS header asks

        // the scenarios on a guest thread, the host waiting for it
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            CHECK(t->stack_base - t->stack_limit == 256 * 1024 && t->stack_limit >= 0x70000000);
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        // KeTimeStampBundle: the clock, running since the first thread,
        // moves it every millisecond
        uint32_t bundle = k.variable("KeTimeStampBundle");
        uint32_t tick = k.load32(bundle + 0x10);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(k.load32(bundle + 0x10) >= tick + 20);
        CHECK(k.load64(bundle + 8) <= k.system_time() && k.system_time() - k.load64(bundle + 8) < 10'000'000);
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
        shutdown_timer_dpcs(k); // the timers' DPC thread, made by timer_dpcs, ended and joined
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    return finish("kernel_threads");
}
