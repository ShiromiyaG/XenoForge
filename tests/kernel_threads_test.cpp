// The kernel's threads, objects and synchronization (src/kernel/objects.h):
// guest threads made and joined, events, semaphores, mutants and timers
// signaled and waited on across host threads with timeouts, TLS slots,
// critical sections, spin locks, SLists, APCs and DPCs, handles. The guest
// code is host functions in the generated code's table, calling the
// exports as the recompiled code reaches them.

#include "check.h"

#include "objects.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

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
    sleep_ms(c, 30);
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
}

void main_thread(ppc::Context &c) {
    threads(c);
    events(c);
    semaphores(c);
    mutants(c);
    multiple(c);
    timers(c);
    tls(c);
    locks(c);
    slists(c);
    links(c);
    apcs_and_dpcs(c);
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
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    return finish("kernel_threads");
}
