// Synchronization exports (objects.h): events, semaphores, mutants and
// timers in their Ke (guest pointer) and Nt (handle) forms, the waits, and
// the RTL critical sections. Translated from Xenia (src/xenia/kernel:
// xboxkrnl/xboxkrnl_threading.cc, xboxkrnl_rtl.cc's critical sections,
// xevent.cc, xsemaphore.cc, xmutant.cc, xtimer.cc; BSD, see LICENSE-xenia)
// and checked against them. What the objects do is the NT kernel's, over
// the dispatcher in objects.cpp; where Xenia's host objects answer
// differently (KeSetEvent's and KeResetEvent's previous state,
// NtReleaseMutant's previous count, an absolute timeout), the NT answer,
// as noted.

#include "objects.h"

#include <algorithm>
#include <cstdio>

namespace xf::kernel {
namespace {

// ---- the objects' operations (lock held) ----

// NT's KeSetEvent returns the previous state (Xenia: 1)
int32_t set_event(Objects &o, Event &e) {
    int32_t previous = e.signal_state();
    e.set_signal_state(1);
    o.signaled(e);
    return previous;
}

int32_t reset_event(Event &e) {
    int32_t previous = e.signal_state();
    e.set_signal_state(0);
    return previous;
}

// waiters that can go now go, and the event ends reset
int32_t pulse_event(Objects &o, Event &e) {
    int32_t previous = e.signal_state();
    if (!previous) {
        e.set_signal_state(1);
        o.signaled(e);
    }
    e.set_signal_state(0);
    return previous;
}

template <typename T> Ref<T> as(Ref<Dispatcher> d) {
    if (!d || !dynamic_cast<T *>(d.get())) return {};
    return Ref<T>(static_cast<T *>(d.release()));
}

// a handle's object of type T, else the status saying why not
template <typename T> Ref<T> by_handle(Objects &o, uint32_t handle, uint32_t &status) {
    Ref<T> object = o.lookup<T>(handle);
    status = object                     ? X_STATUS_SUCCESS
             : o.lookup<Object>(handle) ? X_STATUS_OBJECT_TYPE_MISMATCH
                                        : X_STATUS_INVALID_HANDLE;
    return object;
}

// Xenia's LookupNamedObject: a name already taken by an object of the same
// kind gives that object's handle, retained, and OBJECT_NAME_EXISTS
bool existing_named(Kernel &k, Objects &o, Object::Type type, uint32_t attributes, uint32_t handle_ptr,
                    uint32_t &status) {
    std::string name = attributes_name(k, attributes);
    if (name.empty()) return false;
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t handle = o.find_name(name);
    if (!handle) return false;
    if (o.object_in(handle)->type() != type) {
        o.release_handle(handle);
        status = X_STATUS_INVALID_HANDLE;
        return true;
    }
    if (handle_ptr)
        k.store32(handle_ptr, handle);
    else
        o.release_handle(handle);
    status = X_STATUS_OBJECT_NAME_EXISTS;
    return true;
}

void named(Kernel &k, Objects &o, Object &object, uint32_t attributes) {
    std::string name = attributes_name(k, attributes);
    if (name.empty()) return;
    std::lock_guard<std::mutex> l(o.lock);
    o.set_name(object, name);
}

// a wait's result: user APCs run when it says so (Xenia's xeProcessUserApcs)
uint32_t finish_wait(Objects &o, Thread &t, uint32_t status) {
    if (status == X_STATUS_USER_APC) o.deliver_apcs(t, 1);
    return status;
}

} // namespace

// ---- events ----

XF_EXPORT(xboxkrnl, KeSetEvent) {
    Objects &o = objects(k);
    Ref<Event> e = as<Event>(o.native(arg(k, c, 0)));
    if (!e) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    result(c, uint32_t(set_event(o, *e)));
}

XF_EXPORT(xboxkrnl, KeResetEvent) {
    Objects &o = objects(k);
    Ref<Event> e = as<Event>(o.native(arg(k, c, 0)));
    if (!e) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    result(c, uint32_t(reset_event(*e)));
}

// event_type 0: notification (manual reset), else synchronization
XF_EXPORT(xboxkrnl, NtCreateEvent) {
    uint32_t handle_ptr = arg(k, c, 0), attributes = arg(k, c, 1), type = arg(k, c, 2), initial = arg(k, c, 3);
    Objects &o = objects(k);
    uint32_t status;
    if (existing_named(k, o, Object::Type::Event, attributes, handle_ptr, status)) return result(c, status);
    Ref<Event> e = o.create<Event>(type ? GuestType::EventSynchronization : GuestType::EventNotification, header::SIZE);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    e->set_signal_state(initial ? 1 : 0);
    named(k, o, *e, attributes);
    if (handle_ptr) k.store32(handle_ptr, e->handle());
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtSetEvent) {
    uint32_t previous_ptr = arg(k, c, 1), status;
    Objects &o = objects(k);
    Ref<Event> e = by_handle<Event>(o, arg(k, c, 0), status);
    if (!e) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    int32_t previous = set_event(o, *e);
    if (previous_ptr) k.store32(previous_ptr, uint32_t(previous));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtClearEvent) {
    uint32_t status;
    Objects &o = objects(k);
    Ref<Event> e = by_handle<Event>(o, arg(k, c, 0), status);
    if (!e) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    reset_event(*e);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtPulseEvent) {
    uint32_t previous_ptr = arg(k, c, 1), status;
    Objects &o = objects(k);
    Ref<Event> e = by_handle<Event>(o, arg(k, c, 0), status);
    if (!e) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    int32_t previous = pulse_event(o, *e);
    if (previous_ptr) k.store32(previous_ptr, uint32_t(previous));
    result(c, X_STATUS_SUCCESS);
}

// ---- semaphores ----

XF_EXPORT(xboxkrnl, NtCreateSemaphore) {
    uint32_t handle_ptr = arg(k, c, 0), attributes = arg(k, c, 1);
    int32_t count = int32_t(arg(k, c, 2)), limit = int32_t(arg(k, c, 3));
    Objects &o = objects(k);
    uint32_t status;
    if (existing_named(k, o, Object::Type::Semaphore, attributes, handle_ptr, status)) return result(c, status);
    // what a host semaphore refuses (Xenia's creation fails on it)
    if (limit <= 0 || count < 0 || count > limit) {
        if (handle_ptr) k.store32(handle_ptr, 0);
        return result(c, X_STATUS_INVALID_PARAMETER);
    }
    Ref<Semaphore> s = o.create<Semaphore>(GuestType::Semaphore, ksemaphore::SIZE);
    if (!s) return result(c, X_STATUS_NO_MEMORY);
    s->set_signal_state(count);
    k.store32(s->guest + ksemaphore::LIMIT, uint32_t(limit));
    named(k, o, *s, attributes);
    if (handle_ptr) k.store32(handle_ptr, s->handle());
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtReleaseSemaphore) {
    int32_t count = int32_t(arg(k, c, 1));
    uint32_t previous_ptr = arg(k, c, 2), status;
    Objects &o = objects(k);
    Ref<Semaphore> s = by_handle<Semaphore>(o, arg(k, c, 0), status);
    if (!s) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    int32_t previous = s->signal_state();
    if (count <= 0 || int64_t(previous) + count > s->limit()) {
        status = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    } else {
        s->set_signal_state(previous + count);
        o.signaled(*s);
    }
    if (previous_ptr) k.store32(previous_ptr, uint32_t(previous));
    result(c, status);
}

// ---- mutants ----

XF_EXPORT(xboxkrnl, NtCreateMutant) {
    uint32_t handle_ptr = arg(k, c, 0), attributes = arg(k, c, 1), initial_owner = arg(k, c, 2);
    Objects &o = objects(k);
    uint32_t status;
    if (existing_named(k, o, Object::Type::Mutant, attributes, handle_ptr, status)) return result(c, status);
    Ref<Mutant> m = o.create<Mutant>(GuestType::Mutant, kmutant::SIZE);
    if (!m) return result(c, X_STATUS_NO_MEMORY);
    {
        std::lock_guard<std::mutex> l(o.lock);
        Thread *t = current_thread();
        m->set_signal_state(initial_owner && t ? 0 : 1);
        if (initial_owner && t) m->set_owner(t);
    }
    named(k, o, *m, attributes);
    if (handle_ptr) k.store32(handle_ptr, m->handle());
    result(c, X_STATUS_SUCCESS);
}

// The previous count through the pointer, as NT gives it (Xenia leaves it).
XF_EXPORT(xboxkrnl, NtReleaseMutant) {
    uint32_t previous_ptr = arg(k, c, 1), status;
    Objects &o = objects(k);
    Ref<Mutant> m = by_handle<Mutant>(o, arg(k, c, 0), status);
    if (!m) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    if (m->owner_thread != current_thread()) return result(c, X_STATUS_MUTANT_NOT_OWNED);
    int32_t previous = m->signal_state();
    m->set_signal_state(previous + 1);
    if (previous + 1 > 0) {
        m->set_owner(nullptr);
        o.signaled(*m);
    }
    if (previous_ptr) k.store32(previous_ptr, uint32_t(previous));
    result(c, X_STATUS_SUCCESS);
}

// ---- timers ----

// timer_type 0: notification, 1: synchronization
XF_EXPORT(xboxkrnl, NtCreateTimer) {
    uint32_t handle_ptr = arg(k, c, 0), attributes = arg(k, c, 1), type = arg(k, c, 2);
    Objects &o = objects(k);
    uint32_t status;
    if (existing_named(k, o, Object::Type::Timer, attributes, handle_ptr, status)) return result(c, status);
    Ref<Timer> t = o.create<Timer>(type ? GuestType::TimerSynchronization : GuestType::TimerNotification, ktimer::SIZE);
    if (!t) return result(c, X_STATUS_NO_MEMORY);
    named(k, o, *t, attributes);
    if (handle_ptr) k.store32(handle_ptr, t->handle());
    result(c, X_STATUS_SUCCESS);
}

// (handle, due time, APC routine, mode, routine's argument, resume, period
// in ms); the routine runs as a user APC on the thread that set the timer
XF_EXPORT(xboxkrnl, NtSetTimerEx) {
    uint32_t due_ptr = arg(k, c, 1), routine = arg(k, c, 2), routine_arg = arg(k, c, 4);
    uint32_t resume = arg(k, c, 5), period_ms = arg(k, c, 6), status;
    Objects &o = objects(k);
    Ref<Timer> t = by_handle<Timer>(o, arg(k, c, 0), status);
    if (!t) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    t->set_signal_state(0);
    t->due = due_ptr ? o.deadline(due_ptr) : o.now();
    t->period = uint64_t(period_ms) * 10'000;
    t->routine = routine, t->routine_arg = routine_arg;
    t->apc_thread = Ref<Thread>::retain(routine ? current_thread() : nullptr);
    k.store64(t->guest + ktimer::DUE_TIME, o.system_time_base + t->due);
    k.store32(t->guest + ktimer::PERIOD, period_ms);
    o.arm(*t);
    result(c, resume ? X_STATUS_TIMER_RESUME_IGNORED : X_STATUS_SUCCESS);
}

// The timer's state through the pointer (Xenia: 0).
XF_EXPORT(xboxkrnl, NtCancelTimer) {
    uint32_t state_ptr = arg(k, c, 1), status;
    Objects &o = objects(k);
    Ref<Timer> t = by_handle<Timer>(o, arg(k, c, 0), status);
    if (!t) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    o.disarm(*t);
    t->apc_thread = {};
    if (state_ptr) k.store32(state_ptr, t->signal_state() > 0);
    result(c, X_STATUS_SUCCESS);
}

// ---- waits ----

XF_EXPORT(xboxkrnl, KeWaitForSingleObject) {
    uint32_t alertable = arg(k, c, 3), timeout_ptr = arg(k, c, 4);
    Objects &o = objects(k);
    Thread *t = current_thread();
    Ref<Dispatcher> d = o.native(arg(k, c, 0));
    if (!d || !t) return result(c, X_STATUS_ABANDONED_WAIT_0); // Xenia's answer to an unknown object
    Dispatcher *objects[] = {d.get()};
    result(c, finish_wait(o, *t, o.wait(*t, objects, false, alertable != 0, o.deadline(timeout_ptr))));
}

// (count, objects, wait type: 0 all, 1 any, reason, mode, alertable,
// timeout, wait blocks)
XF_EXPORT(xboxkrnl, KeWaitForMultipleObjects) {
    uint32_t count = arg(k, c, 0), objects_ptr = arg(k, c, 1), type = arg(k, c, 2);
    uint32_t alertable = arg(k, c, 5), timeout_ptr = arg(k, c, 6);
    Objects &o = objects(k);
    Thread *t = current_thread();
    if (!t || !count || count > 64 || type > 1) return result(c, X_STATUS_INVALID_PARAMETER);
    std::vector<Ref<Dispatcher>> refs;
    std::vector<Dispatcher *> list;
    for (uint32_t n = 0; n < count; n++) {
        refs.push_back(o.native(k.load32(objects_ptr + 4 * n)));
        if (!refs.back()) return result(c, X_STATUS_INVALID_PARAMETER);
        list.push_back(refs.back().get());
    }
    result(c, finish_wait(o, *t, o.wait(*t, list, type == 0, alertable != 0, o.deadline(timeout_ptr))));
}

// An object with nothing to wait on (no dispatcher header) is signaled, as
// Xenia's.
XF_EXPORT(xboxkrnl, NtWaitForSingleObjectEx) {
    uint32_t handle = arg(k, c, 0), alertable = arg(k, c, 2), timeout_ptr = arg(k, c, 3);
    Objects &o = objects(k);
    Thread *t = current_thread();
    Ref<Object> object = o.lookup<Object>(handle);
    if (!object) return result(c, X_STATUS_INVALID_HANDLE);
    if (!object->dispatcher() || !t) return result(c, X_STATUS_SUCCESS);
    Dispatcher *objects[] = {static_cast<Dispatcher *>(object.get())};
    result(c, finish_wait(o, *t, o.wait(*t, objects, false, alertable != 0, o.deadline(timeout_ptr))));
}

// (count, handles, wait type, mode, alertable, timeout)
XF_EXPORT(xboxkrnl, NtWaitForMultipleObjectsEx) {
    uint32_t count = arg(k, c, 0), handles = arg(k, c, 1), type = arg(k, c, 2);
    uint32_t alertable = arg(k, c, 4), timeout_ptr = arg(k, c, 5);
    Objects &o = objects(k);
    Thread *t = current_thread();
    if (!t || !count || count > 64 || type > 1) return result(c, X_STATUS_INVALID_PARAMETER);
    std::vector<Ref<Object>> refs;
    std::vector<Dispatcher *> list;
    for (uint32_t n = 0; n < count; n++) {
        refs.push_back(o.lookup<Object>(k.load32(handles + 4 * n)));
        if (!refs.back() || !refs.back()->dispatcher()) return result(c, X_STATUS_INVALID_PARAMETER);
        list.push_back(static_cast<Dispatcher *>(refs.back().get()));
    }
    result(c, finish_wait(o, *t, o.wait(*t, list, type == 0, alertable != 0, o.deadline(timeout_ptr))));
}

// ---- critical sections (Xenia's X_RTL_CRITICAL_SECTION, 28 bytes) ----
//
// A synchronization event's header, then lock_count (-1 free), recursion
// count and the owning KTHREAD; kept big-endian, as the game reads them.

namespace {

constexpr uint32_t CS_LOCK_COUNT = 0x10, CS_RECURSION = 0x14, CS_OWNER = 0x18;

int32_t atomic_add(Kernel &k, uint32_t address, int32_t delta) {
    auto word = guest_word(k, address);
    uint32_t old = word.load();
    for (;;) {
        uint32_t value = bswap32(uint32_t(int32_t(bswap32(old)) + delta));
        if (word.compare_exchange_weak(old, value)) return int32_t(bswap32(value));
    }
}

bool compare_exchange(Kernel &k, uint32_t address, int32_t expected, int32_t desired) {
    uint32_t e = bswap32(uint32_t(expected));
    return guest_word(k, address).compare_exchange_strong(e, bswap32(uint32_t(desired)));
}

void initialize_critical_section(Kernel &k, uint32_t cs, uint8_t spin_count_256) {
    k.host(cs)[header::TYPE] = uint8_t(GuestType::EventSynchronization);
    k.host(cs)[header::ABSOLUTE] = spin_count_256;
    k.store32(cs + header::SIGNAL_STATE, 0);
    k.store32(cs + CS_LOCK_COUNT, 0xFFFFFFFF);
    k.store32(cs + CS_RECURSION, 0);
    k.store32(cs + CS_OWNER, 0);
}

uint32_t current_kthread() {
    Thread *t = current_thread();
    return t ? t->kthread() : 0;
}

} // namespace

XF_EXPORT(xboxkrnl, RtlInitializeCriticalSection) { initialize_critical_section(k, arg(k, c, 0), 0); }

// the spin count in 256s, rounded up, at most 255
XF_EXPORT(xboxkrnl, RtlInitializeCriticalSectionAndSpinCount) {
    uint32_t spin = (arg(k, c, 1) + 255) >> 8;
    initialize_critical_section(k, arg(k, c, 0), uint8_t(std::min(spin, 255u)));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, RtlEnterCriticalSection) {
    uint32_t cs = arg(k, c, 0);
    if (!cs) return;
    uint32_t self = current_kthread();
    if (k.load32(cs + CS_OWNER) == self) {
        atomic_add(k, cs + CS_LOCK_COUNT, 1);
        k.store32(cs + CS_RECURSION, k.load32(cs + CS_RECURSION) + 1);
        return;
    }
    for (uint32_t spin = k.host(cs)[header::ABSOLUTE] * 256u; spin; spin--) {
        if (compare_exchange(k, cs + CS_LOCK_COUNT, -1, 0)) {
            k.store32(cs + CS_OWNER, self);
            k.store32(cs + CS_RECURSION, 1);
            return;
        }
    }
    if (atomic_add(k, cs + CS_LOCK_COUNT, 1) != 0) {
        // taken: wait for the leaving owner's signal
        Objects &o = objects(k);
        Ref<Dispatcher> d = o.native(cs);
        if (Thread *t = current_thread(); d && t) {
            Dispatcher *objects[] = {d.get()};
            o.wait(*t, objects, false, false, Scheduler::NEVER);
        }
    }
    k.store32(cs + CS_OWNER, self);
    k.store32(cs + CS_RECURSION, 1);
}

XF_EXPORT(xboxkrnl, RtlTryEnterCriticalSection) {
    uint32_t cs = arg(k, c, 0);
    if (!cs) return result(c, 1);
    uint32_t self = current_kthread();
    if (compare_exchange(k, cs + CS_LOCK_COUNT, -1, 0)) {
        k.store32(cs + CS_OWNER, self);
        k.store32(cs + CS_RECURSION, 1);
        return result(c, 1);
    }
    if (k.load32(cs + CS_OWNER) == self) {
        atomic_add(k, cs + CS_LOCK_COUNT, 1);
        k.store32(cs + CS_RECURSION, k.load32(cs + CS_RECURSION) + 1);
        return result(c, 1);
    }
    result(c, 0);
}

XF_EXPORT(xboxkrnl, RtlLeaveCriticalSection) {
    uint32_t cs = arg(k, c, 0);
    if (!cs) return;
    uint32_t recursion = k.load32(cs + CS_RECURSION) - 1;
    k.store32(cs + CS_RECURSION, recursion);
    if (recursion) {
        atomic_add(k, cs + CS_LOCK_COUNT, -1);
        return;
    }
    k.store32(cs + CS_OWNER, 0);
    if (atomic_add(k, cs + CS_LOCK_COUNT, -1) != -1) {
        // a waiter: wake one (the header is an auto-reset event)
        Objects &o = objects(k);
        if (Ref<Event> e = as<Event>(o.native(cs))) {
            std::lock_guard<std::mutex> l(o.lock);
            set_event(o, *e);
        }
    }
}

} // namespace xf::kernel
