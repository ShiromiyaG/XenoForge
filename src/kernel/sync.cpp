// Synchronization exports (objects.h): events, semaphores, mutants and
// timers in their Ke (guest pointer) and Nt (handle) forms, the waits, the
// RTL critical sections and the executive's read-write locks. Translated
// from Xenia (src/xenia/kernel: xboxkrnl/xboxkrnl_threading.cc,
// xboxkrnl_rtl.cc's critical sections, xevent.cc, xsemaphore.cc,
// xmutant.cc, xtimer.cc; BSD, see LICENSE-xenia) and checked against them.
// What the objects do is the NT kernel's, over the dispatcher in
// objects.cpp; where Xenia's host objects answer differently (KeSetEvent's
// and KeResetEvent's previous state, NtReleaseMutant's previous count, an
// absolute timeout), the NT answer, as noted.
//
// Also where it differs:
//   - KeInitializeEvent and KeInitializeTimerEx on a header a host object of
//     the same kind already stands behind keep that object (Xenia clears the
//     header's marker, so a new one is made and the old one stays behind);
//     the timer is disarmed;
//   - the wait list of every initialized header (events, semaphores, mutants,
//     timers, critical sections, the read-write lock's members) is an empty
//     list, as the console's initializers leave it, where Xenia writes a
//     host-object marker there; the host object is found through a table
//     (objects.cpp). RtlInitializeCriticalSectionAndSpinCount returns the
//     critical section's pointer, as the console does (Xenia: 0);
//   - NtQueryEvent on a handle of another kind answers OBJECT_TYPE_MISMATCH,
//     as the other Nt event calls here (Xenia: INVALID_HANDLE);
//   - ExReleaseReadWriteLock wakes a waiting writer when the last reader or
//     the writer leaves, as Cxbx-Reloaded's ExReleaseReadWriteLock (which
//     Xenia's follows) does; Xenia decrements the readers' count also when
//     it is 0, and so never wakes a writer queued behind another writer.

#include "objects.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

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

// the host object made for a header, if it is of that kind (lock held)
Object *existing(Objects &o, uint32_t address, Object::Type type) {
    Object *object = o.find_native(address);
    return object && object->type() == type ? object : nullptr;
}

// Xenia's KeInitializeEvent: the header written (type 0 notification, 1
// synchronization) with an empty wait list (the console's; Xenia leaves its
// host-object marker there), the host object made, or the one behind it kept
void initialize_event(Kernel &k, uint32_t address, uint32_t type, uint32_t state) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    auto *e = static_cast<Event *>(existing(o, address, Object::Type::Event));
    uint8_t *h = k.host(address);
    h[header::TYPE] = uint8_t(type), h[1] = 0, h[2] = 0, h[3] = 0;
    k.store32(address + header::SIGNAL_STATE, state);
    list_init(k, address + header::WAIT_LIST);
    if (e) {
        if (int32_t(state) > 0) o.signaled(*e);
        return;
    }
    o.native_locked(address);
}

// Xenia's KeInitializeSemaphore, as the read-write lock's initialization
// calls it; the wait list empty, as the console's
void initialize_semaphore(Kernel &k, uint32_t address, uint32_t count, uint32_t limit) {
    k.host(address)[header::TYPE] = uint8_t(GuestType::Semaphore);
    k.store32(address + header::SIGNAL_STATE, count);
    list_init(k, address + header::WAIT_LIST);
    k.store32(address + ksemaphore::LIMIT, limit);
    objects(k).native(address);
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
    // only types 0 (notification) and 1 (synchronization), as the console's kernel checks (Xenia takes any nonzero
    // as synchronization)
    if (type > 1) return result(c, X_STATUS_INVALID_PARAMETER);
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

// (event, type, initial state)
XF_EXPORT(xboxkrnl, KeInitializeEvent) { initialize_event(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)); }

// (event, increment, wait): the state before the pulse
XF_EXPORT(xboxkrnl, KePulseEvent) {
    Objects &o = objects(k);
    Ref<Event> e = as<Event>(o.native(arg(k, c, 0)));
    if (!e) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    result(c, uint32_t(pulse_event(o, *e)));
}

// (handle, EVENT_BASIC_INFORMATION *): the type (0 notification, 1
// synchronization) and the signal state
XF_EXPORT(xboxkrnl, NtQueryEvent) {
    uint32_t out = arg(k, c, 1), status;
    Objects &o = objects(k);
    Ref<Event> e = by_handle<Event>(o, arg(k, c, 0), status);
    if (!e) return result(c, status);
    std::lock_guard<std::mutex> l(o.lock);
    k.store32(out, e->manual_reset() ? 0 : 1);
    k.store32(out + 4, uint32_t(e->signal_state()));
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
    // (the console's kernel leaves the output handle alone then; Xenia's wrote 0)
    if (limit <= 0 || count < 0 || count > limit) return result(c, X_STATUS_INVALID_PARAMETER);
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

// ---- the Ke forms of semaphores and mutants (a header in the game's memory) ----

// (semaphore, count, limit): the header and limit written, the host object
// made or found (Xenia's KeInitializeSemaphore; the wait list an empty list,
// as the console's, where Xenia leaves a host-object marker)
XF_EXPORT(xboxkrnl, KeInitializeSemaphore) { initialize_semaphore(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)); }

// (semaphore, increment, adjustment, wait): the previous count; past the
// limit nothing changes (NT raises an exception there, Xenia ignores it)
XF_EXPORT(xboxkrnl, KeReleaseSemaphore) {
    int32_t adjustment = int32_t(arg(k, c, 2));
    Objects &o = objects(k);
    Ref<Semaphore> s = as<Semaphore>(o.native(arg(k, c, 0)));
    if (!s) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    int32_t previous = s->signal_state();
    if (adjustment > 0 && int64_t(previous) + adjustment <= s->limit()) {
        s->set_signal_state(previous + adjustment);
        o.signaled(*s);
    }
    result(c, uint32_t(previous));
}

// (mutant, initial owner): NT's KeInitializeMutant (Xenia has none): owned
// by the caller, or signaled
XF_EXPORT(xboxkrnl, KeInitializeMutant) {
    uint32_t address = arg(k, c, 0);
    bool owned = arg(k, c, 1) != 0;
    k.host(address)[header::TYPE] = uint8_t(GuestType::Mutant);
    k.store32(address + header::SIGNAL_STATE, owned ? 0 : 1);
    list_init(k, address + header::WAIT_LIST);
    k.host(address)[kmutant::ABANDONED] = 0;
    Objects &o = objects(k);
    Ref<Mutant> m = as<Mutant>(o.native(address));
    if (!m) return;
    std::lock_guard<std::mutex> l(o.lock);
    m->abandoned = false;
    m->set_owner(owned ? current_thread() : nullptr);
}

// (mutant, increment, abandoned, wait): NT's KeReleaseMutant, the previous
// signal state. Abandoning releases it whoever owns it, as NT does.
XF_EXPORT(xboxkrnl, KeReleaseMutant) {
    bool abandon = arg(k, c, 2) != 0;
    Objects &o = objects(k);
    Ref<Mutant> m = as<Mutant>(o.native(arg(k, c, 0)));
    if (!m) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    int32_t previous = m->signal_state();
    if (abandon) {
        m->set_signal_state(1);
        m->abandoned = true;
        k.host(m->guest)[kmutant::ABANDONED] = 1;
    } else {
        if (m->owner_thread != current_thread()) return result(c, uint32_t(previous)); // NT raises here
        m->set_signal_state(previous + 1);
    }
    if (m->signal_state() > 0) {
        m->set_owner(nullptr);
        o.signaled(*m);
    }
    result(c, uint32_t(previous));
}

// ---- timers ----

// timer_type 0: notification, 1: synchronization
XF_EXPORT(xboxkrnl, NtCreateTimer) {
    uint32_t handle_ptr = arg(k, c, 0), attributes = arg(k, c, 1), type = arg(k, c, 2);
    Objects &o = objects(k);
    uint32_t status;
    if (type > 1) return result(c, X_STATUS_INVALID_PARAMETER); // as NtCreateEvent
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
    t->dpc = 0;
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
    t->dpc = 0;
    if (state_ptr) k.store32(state_ptr, t->signal_state() > 0);
    result(c, X_STATUS_SUCCESS);
}

// (timer, type: 0 notification, 1 synchronization, process type): the
// header, its due time and period (Xenia's xeKeInitializeTimerEx; the other
// fields carry through repeated initializations); the host object is made on
// first use
XF_EXPORT(xboxkrnl, KeInitializeTimerEx) {
    uint32_t timer = arg(k, c, 0), type = arg(k, c, 1), process_type = arg(k, c, 2) & 0xFF;
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    auto *t = static_cast<Timer *>(existing(o, timer, Object::Type::Timer));
    if (t) {
        o.disarm(*t);
        t->apc_thread = {};
        t->routine = t->routine_arg = 0;
        t->dpc = 0;
    }
    uint8_t *h = k.host(timer);
    h[header::TYPE] = uint8_t(type ? GuestType::TimerSynchronization : GuestType::TimerNotification);
    h[2] = uint8_t(process_type);
    h[3] = 0; // inserted
    k.store32(timer + header::SIGNAL_STATE, 0);
    list_init(k, timer + header::WAIT_LIST);
    k.store64(timer + ktimer::DUE_TIME, 0);
    k.store32(timer + ktimer::PERIOD, 0);
}

// ---- the Ke timers, with DPCs (KeSetTimerEx) ----
//
// Xenia has none of KeSetTimer, KeSetTimerEx and KeCancelTimer; these are
// NT's, over the timers NtSetTimerEx arms. A timer that comes due is
// signaled; with a DPC, the DPC is queued as NT's timer expiration does,
// its two arguments the system time's low and high words, and runs on a
// kernel thread of its own at DISPATCH_LEVEL (the PCR saying a DPC runs, as
// the GPU's interrupt DPCs). XAM's own timers need it.

namespace {

struct DpcRun {
    uint32_t dpc, arg1, arg2;
    uint8_t process_type;
};

struct TimerDpcs {
    Kernel *k = nullptr;
    std::once_flag started;
    std::mutex mutex; // the queue; taken under the objects' lock, never around it
    std::deque<DpcRun> queue;
    Ref<Event> wake; // auto-reset: the queue has something
    Ref<Thread> thread;
    std::atomic<bool> running{false};
    bool thread_started = false;

    // the thread, made with the first timer that has a DPC
    void start(Kernel &kernel) {
        std::call_once(started, [&] {
            k = &kernel;
            Objects &o = objects(kernel);
            wake = o.create<Event>(GuestType::EventSynchronization, header::SIZE);
            Thread::Params p;
            p.stack_size = 128 * 1024;
            p.process = title_process(kernel) - kprocess::SIZE; // the idle process, as the GPU's threads
            p.host_arg = this;
            p.host = [](Thread &self, void *v) { static_cast<TimerDpcs *>(v)->loop(self); };
            p.name = "Timer DPCs";
            uint32_t status = X_STATUS_NO_MEMORY;
            if (wake) thread = o.create_thread(p, status);
            if (!thread) {
                std::fprintf(stderr, "timers: the DPC thread was not made (status 0x%08X)\n", status);
                return;
            }
            running = true;
            {
                std::lock_guard<std::mutex> l(o.lock);
                o.on_timer_dpc = [this](Timer &t, uint64_t time) { queue_dpc(t, time); };
            }
            thread_started = o.scheduler->start(*thread);
            if (!thread_started) std::fprintf(stderr, "timers: no host thread for the DPCs\n");
        });
    }

    // a timer came due (lock held)
    void queue_dpc(Timer &t, uint64_t time) {
        if (!running.load() || !wake) return;
        uint8_t process_type = k->host(t.guest)[2]; // KeInitializeTimerEx's
        {
            std::lock_guard<std::mutex> l(mutex);
            queue.push_back(
                {t.dpc, uint32_t(time), uint32_t(time >> 32), process_type ? process_type : kprocess::TITLE});
        }
        set_event(objects(*k), *wake);
    }

    void loop(Thread &self) {
        Objects &o = objects(*k);
        Dispatcher *const wakes[] = {wake.get()};
        while (running.load()) {
            std::deque<DpcRun> batch;
            {
                std::lock_guard<std::mutex> l(mutex);
                batch.swap(queue);
            }
            if (batch.empty()) {
                o.wait(self, wakes, false, false, Scheduler::NEVER);
                continue;
            }
            for (const DpcRun &r : batch) run(self, r);
        }
    }

    // KeInsertQueueDpc's run, on this thread
    void run(Thread &self, const DpcRun &r) {
        uint32_t routine = k->load32(r.dpc + kdpc::ROUTINE);
        if (!routine) return;
        k->store32(r.dpc + kdpc::ARG1, r.arg1);
        k->store32(r.dpc + kdpc::ARG2, r.arg2);
        uint8_t *p = k->host(self.pcr);
        uint8_t irql = p[kpcr::CURRENT_IRQL];
        p[kpcr::CURRENT_IRQL] = 2;
        k->store32(self.pcr + kpcr::DPC_ACTIVE, 1);
        p[kpcr::PROCESS_TYPE_IN_DPC] = r.process_type;
        objects(*k).call(*self.context, routine, {r.dpc, k->load32(r.dpc + kdpc::CONTEXT), r.arg1, r.arg2});
        p[kpcr::PROCESS_TYPE_IN_DPC] = kprocess::IDLE;
        p[kpcr::CURRENT_IRQL] = irql;
        k->store32(self.pcr + kpcr::DPC_ACTIVE, 0);
    }

    // the kernel ends: the thread stopped and joined, its handles closed
    void stop() {
        if (!k) return;
        Objects &o = objects(*k);
        if (running.exchange(false)) {
            {
                std::lock_guard<std::mutex> l(o.lock);
                o.on_timer_dpc = nullptr;
                set_event(o, *wake);
            }
            if (thread_started) o.scheduler->join(*thread);
        }
        {
            std::lock_guard<std::mutex> l(o.lock);
            for (Object *object : {static_cast<Object *>(thread.get()), static_cast<Object *>(wake.get())})
                if (object && object->handle()) o.release_handle(object->handle());
        }
        thread = {}, wake = {};
    }
};

} // namespace

void shutdown_timer_dpcs(Kernel &k) { k.state<TimerDpcs>().stop(); }

// (timer, due time (64 bits: negative relative in 100 ns, positive an
// absolute system time), period in ms, dpc): whether the timer was already
// set, NT's answer. The timer is reset when set; a period of 0 fires once.
static uint32_t set_timer(Kernel &k, uint32_t timer, int64_t due, uint32_t period_ms, uint32_t dpc) {
    Objects &o = objects(k);
    Ref<Timer> t = as<Timer>(o.native(timer));
    if (!t) return 0;
    if (dpc) k.state<TimerDpcs>().start(k);
    std::lock_guard<std::mutex> l(o.lock);
    bool was_set = t->armed;
    t->set_signal_state(0);
    t->due = o.deadline_at(due);
    t->period = int32_t(period_ms) > 0 ? uint64_t(period_ms) * 10'000 : 0;
    t->routine = 0, t->routine_arg = 0;
    t->apc_thread = {};
    t->dpc = dpc;
    k.store64(t->guest + ktimer::DUE_TIME, o.system_time_base + t->due);
    k.store32(t->guest + ktimer::PERIOD, period_ms);
    o.arm(*t);
    // a due time that has passed is signaled in the call, as the console's insert finds it expired (the clock's thread
    // would do it a moment later)
    if (t->due <= o.scheduler->now()) o.on_clock();
    return was_set;
}

XF_EXPORT(xboxkrnl, KeSetTimerEx) {
    result(c, set_timer(k, arg(k, c, 0), int64_t(arg64(k, c, 1)), arg(k, c, 2), arg(k, c, 3)));
}

// (timer, due time (64 bits), dpc)
XF_EXPORT(xboxkrnl, KeSetTimer) { result(c, set_timer(k, arg(k, c, 0), int64_t(arg64(k, c, 1)), 0, arg(k, c, 2))); }

// (timer): whether it was set; its signal state is left as it is
XF_EXPORT(xboxkrnl, KeCancelTimer) {
    Objects &o = objects(k);
    Ref<Timer> t = as<Timer>(o.native(arg(k, c, 0)));
    if (!t) return result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    bool was_set = t->armed;
    o.disarm(*t);
    t->apc_thread = {};
    t->dpc = 0;
    result(c, was_set);
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
    // The wait blocks the caller supplies, as the console fills them (KeWaitForMultipleObjects walks the objects and
    // fills each one's block: the thread, the object, the next block, the key and the type); a wait for any stops at
    // the first object that is signaled, before its block. Xenia leaves them alone.
    // The last block points back to the first when the wait is satisfied for all or goes on to block; a wait that
    // finds nothing and has a timeout of zero leaves it pointing past the array.
    if (uint32_t blocks = arg(k, c, 7)) {
        k.store32(t->kthread() + kthread::WAIT_BLOCKS, blocks);
        bool any_signaled = false, all_signaled = true;
        for (Dispatcher *d : list) {
            bool signaled = d->signaled_for(*t);
            any_signaled = any_signaled || signaled, all_signaled = all_signaled && signaled;
        }
        bool zero_timeout = timeout_ptr && k.load64(timeout_ptr) == 0;
        bool circular = type == 0 ? (all_signaled || !zero_timeout) : (!any_signaled && !zero_timeout);
        uint32_t filled = 0;
        for (uint32_t n = 0; n < count; n++) {
            if (type == 1 && list[n]->signaled_for(*t)) break;
            uint32_t block = blocks + kwait_block::SIZE * n;
            k.store32(block + kwait_block::THREAD, t->kthread());
            k.store32(block + kwait_block::OBJECT, list[n]->guest);
            k.store32(block + kwait_block::NEXT, block + kwait_block::SIZE);
            k.store16(block + kwait_block::KEY, uint16_t(n));
            k.store16(block + kwait_block::TYPE, uint16_t(type));
            filled++;
        }
        if (circular && filled) k.store32(blocks + kwait_block::SIZE * (filled - 1) + kwait_block::NEXT, blocks);
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

// (signal handle, wait handle, mode, alertable, timeout): an event set, a
// semaphore released by one or a mutant released, then the wait (Xenia's
// XObject::SignalAndWait). The two are not one atomic step here: another
// thread may run between them, which a waiter cannot tell from being
// scheduled late.
XF_EXPORT(xboxkrnl, NtSignalAndWaitForSingleObjectEx) {
    uint32_t alertable = arg(k, c, 3), timeout_ptr = arg(k, c, 4);
    Objects &o = objects(k);
    Thread *t = current_thread();
    Ref<Object> signal = o.lookup<Object>(arg(k, c, 0));
    Ref<Object> wait = o.lookup<Object>(arg(k, c, 1));
    if (!signal || !wait || !t) return result(c, X_STATUS_INVALID_HANDLE);
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (auto *e = dynamic_cast<Event *>(signal.get())) {
            set_event(o, *e);
        } else if (auto *s = dynamic_cast<Semaphore *>(signal.get())) {
            if (int64_t(s->signal_state()) + 1 > s->limit()) return result(c, X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
            s->set_signal_state(s->signal_state() + 1);
            o.signaled(*s);
        } else if (auto *m = dynamic_cast<Mutant *>(signal.get())) {
            if (m->owner_thread != t) return result(c, X_STATUS_MUTANT_NOT_OWNED);
            m->set_signal_state(m->signal_state() + 1);
            if (m->signal_state() > 0) {
                m->set_owner(nullptr);
                o.signaled(*m);
            }
        } else {
            return result(c, X_STATUS_OBJECT_TYPE_MISMATCH);
        }
    }
    if (!wait->dispatcher()) return result(c, X_STATUS_SUCCESS);
    Dispatcher *objects[] = {static_cast<Dispatcher *>(wait.get())};
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
        // a handle that is no object is STATUS_INVALID_HANDLE on the console's kernel (checked against it)
        if (!refs.back()) return result(c, X_STATUS_INVALID_HANDLE);
        if (!refs.back()->dispatcher()) return result(c, X_STATUS_OBJECT_TYPE_MISMATCH);
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
    list_init(k, cs + header::WAIT_LIST); // an empty wait list, as the console's (Xenia leaves it alone)
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

// the spin count in 256s, rounded up, at most 255; the console returns the
// pointer it was given (Xenia returns 0, a status)
XF_EXPORT(xboxkrnl, RtlInitializeCriticalSectionAndSpinCount) {
    uint32_t spin = (arg(k, c, 1) + 255) >> 8;
    initialize_critical_section(k, arg(k, c, 0), uint8_t(std::min(spin, 255u)));
    result(c, arg(k, c, 0));
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

// ---- read-write locks (Xenia's X_ERWLOCK, 0x38 bytes) ----
//
// The counts, then a synchronization event the writers wait on, a semaphore
// the readers wait on and a spin lock over the counts; kept big-endian, in
// the game's memory.

namespace {

constexpr uint32_t RW_LOCK_COUNT = 0x0, RW_WRITERS_WAITING = 0x4, RW_READERS_WAITING = 0x8, RW_READERS_ENTRY = 0xC;
constexpr uint32_t RW_WRITER_EVENT = 0x10, RW_READER_SEMAPHORE = 0x20, RW_SPIN_LOCK = 0x34;

// Xenia's xeKeKfAcquireSpinLock and xeKeKfReleaseSpinLock: the caller's PCR
// in the lock word, the IRQL at dispatch level meanwhile
uint8_t acquire_spin(Kernel &k, ppc::Context &c, uint32_t lock) {
    uint32_t pcr = uint32_t(c.r[13]);
    uint8_t old = k.host(pcr)[kpcr::CURRENT_IRQL];
    k.host(pcr)[kpcr::CURRENT_IRQL] = 2;
    auto word = guest_word(k, lock);
    for (uint32_t spins = 0;; spins++) {
        uint32_t expected = 0;
        if (word.compare_exchange_weak(expected, bswap32(pcr), std::memory_order_acquire)) return old;
        if (spins >= 64) objects(k).scheduler->yield();
    }
}

void release_spin(Kernel &k, ppc::Context &c, uint32_t lock, uint8_t old_irql) {
    guest_word(k, lock).store(0, std::memory_order_release);
    if (old_irql < 2) k.host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] = old_irql;
}

// xeKeWaitForSingleObject(object, WrExecutive, kernel mode, not alertable,
// forever)
void wait_forever(Kernel &k, uint32_t address) {
    Objects &o = objects(k);
    Thread *t = current_thread();
    Ref<Dispatcher> d = o.native(address);
    if (!d || !t) return;
    Dispatcher *list[] = {d.get()};
    o.wait(*t, list, false, false, Scheduler::NEVER);
}

uint32_t rw_load(Kernel &k, uint32_t lock, uint32_t field) { return k.load32(lock + field); }
void rw_store(Kernel &k, uint32_t lock, uint32_t field, uint32_t value) { k.store32(lock + field, value); }

} // namespace

XF_EXPORT(xboxkrnl, ExInitializeReadWriteLock) {
    uint32_t lock = arg(k, c, 0);
    rw_store(k, lock, RW_LOCK_COUNT, 0xFFFFFFFF);
    rw_store(k, lock, RW_WRITERS_WAITING, 0);
    rw_store(k, lock, RW_READERS_WAITING, 0);
    rw_store(k, lock, RW_READERS_ENTRY, 0);
    initialize_event(k, lock + RW_WRITER_EVENT, uint32_t(GuestType::EventSynchronization), 0);
    initialize_semaphore(k, lock + RW_READER_SEMAPHORE, 0, 0x7FFFFFFF);
    rw_store(k, lock, RW_SPIN_LOCK, 0);
}

XF_EXPORT(xboxkrnl, ExAcquireReadWriteLockExclusive) {
    uint32_t lock = arg(k, c, 0);
    uint8_t irql = acquire_spin(k, c, lock + RW_SPIN_LOCK);
    int32_t count = int32_t(rw_load(k, lock, RW_LOCK_COUNT)) + 1;
    rw_store(k, lock, RW_LOCK_COUNT, uint32_t(count));
    if (count == 0) return release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    rw_store(k, lock, RW_WRITERS_WAITING, rw_load(k, lock, RW_WRITERS_WAITING) + 1);
    release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    wait_forever(k, lock + RW_WRITER_EVENT);
}

XF_EXPORT(xboxkrnl, ExTryToAcquireReadWriteLockExclusive) {
    uint32_t lock = arg(k, c, 0);
    uint8_t irql = acquire_spin(k, c, lock + RW_SPIN_LOCK);
    bool taken = int32_t(rw_load(k, lock, RW_LOCK_COUNT)) < 0;
    if (taken) rw_store(k, lock, RW_LOCK_COUNT, 0);
    release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    result(c, taken ? 1 : 0);
}

// A reader enters while no writer holds the lock, or while readers hold it
// and no writer waits.
XF_EXPORT(xboxkrnl, ExAcquireReadWriteLockShared) {
    uint32_t lock = arg(k, c, 0);
    uint8_t irql = acquire_spin(k, c, lock + RW_SPIN_LOCK);
    int32_t count = int32_t(rw_load(k, lock, RW_LOCK_COUNT)) + 1;
    rw_store(k, lock, RW_LOCK_COUNT, uint32_t(count));
    if (count == 0 || (rw_load(k, lock, RW_READERS_ENTRY) && !rw_load(k, lock, RW_WRITERS_WAITING))) {
        rw_store(k, lock, RW_READERS_ENTRY, rw_load(k, lock, RW_READERS_ENTRY) + 1);
        return release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    }
    rw_store(k, lock, RW_READERS_WAITING, rw_load(k, lock, RW_READERS_WAITING) + 1);
    release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    wait_forever(k, lock + RW_READER_SEMAPHORE);
}

XF_EXPORT(xboxkrnl, ExTryToAcquireReadWriteLockShared) {
    uint32_t lock = arg(k, c, 0);
    uint8_t irql = acquire_spin(k, c, lock + RW_SPIN_LOCK);
    bool taken = int32_t(rw_load(k, lock, RW_LOCK_COUNT)) < 0 ||
                 (rw_load(k, lock, RW_READERS_ENTRY) && !rw_load(k, lock, RW_WRITERS_WAITING));
    if (taken) {
        rw_store(k, lock, RW_LOCK_COUNT, rw_load(k, lock, RW_LOCK_COUNT) + 1);
        rw_store(k, lock, RW_READERS_ENTRY, rw_load(k, lock, RW_READERS_ENTRY) + 1);
    }
    release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    result(c, taken ? 1 : 0);
}

// The last holder out lets the waiting readers in, all of them, else one
// waiting writer (see the top for where Xenia differs).
XF_EXPORT(xboxkrnl, ExReleaseReadWriteLock) {
    uint32_t lock = arg(k, c, 0);
    uint8_t irql = acquire_spin(k, c, lock + RW_SPIN_LOCK);
    int32_t count = int32_t(rw_load(k, lock, RW_LOCK_COUNT)) - 1;
    rw_store(k, lock, RW_LOCK_COUNT, uint32_t(count));
    if (count < 0) {
        rw_store(k, lock, RW_READERS_ENTRY, 0);
        return release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    }
    Objects &o = objects(k);
    uint32_t entered = rw_load(k, lock, RW_READERS_ENTRY);
    if (!entered) {
        if (uint32_t waiting = rw_load(k, lock, RW_READERS_WAITING)) {
            rw_store(k, lock, RW_READERS_WAITING, 0);
            rw_store(k, lock, RW_READERS_ENTRY, waiting);
            release_spin(k, c, lock + RW_SPIN_LOCK, irql);
            // KeReleaseSemaphore(semaphore, 1, waiting, FALSE)
            if (Ref<Semaphore> s = as<Semaphore>(o.native(lock + RW_READER_SEMAPHORE))) {
                std::lock_guard<std::mutex> l(o.lock);
                int32_t previous = s->signal_state();
                if (int64_t(previous) + waiting <= s->limit()) {
                    s->set_signal_state(previous + int32_t(waiting));
                    o.signaled(*s);
                }
            }
            return;
        }
    } else {
        rw_store(k, lock, RW_READERS_ENTRY, --entered);
        if (entered) return release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    }
    rw_store(k, lock, RW_WRITERS_WAITING, rw_load(k, lock, RW_WRITERS_WAITING) - 1);
    release_spin(k, c, lock + RW_SPIN_LOCK, irql);
    if (Ref<Event> e = as<Event>(o.native(lock + RW_WRITER_EVENT))) {
        std::lock_guard<std::mutex> l(o.lock);
        set_event(o, *e);
    }
}

} // namespace xf::kernel
