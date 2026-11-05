// The kernel's objects and threads (docs/runtime.md, "Threads and objects";
// plan step A4). Handles; the dispatcher objects (events, semaphores,
// mutants, timers, threads), whose X_DISPATCH_HEADER lives in guest memory
// where the game or the kernel put it and whose host object is found by it;
// waits; the guest threads and the scheduler they run on.
//
// The guest structures' layouts, the handle values and what each export
// does follow Xenia (src/xenia/kernel: xobject.h, xthread.h,
// util/object_table.h, kernel_state.h; BSD, see LICENSE-xenia). The
// dispatcher is written here, as the NT kernel works: one lock over every
// object's state, a signal state kept in the guest header (so the game sees
// it, and what the game writes there counts), waits satisfied by whoever
// signals (KiWaitTest), and the scheduler behind a small interface, so the
// deterministic mode (fibers on one host thread, plan "Threads") replaces
// the host threads without touching the objects.

#pragma once

#include "xenoforge/kernel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace xf::kernel {

// ---- statuses (Xenia's xbox.h) ----

constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_PENDING = 0x00000103;
constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
constexpr uint32_t X_STATUS_TIMER_RESUME_IGNORED = 0x40000025;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t X_STATUS_THREAD_IS_TERMINATING = 0xC000004B;
// (internal) a wait woken to run kernel APCs, waiting again after them
constexpr uint32_t WAKE_FOR_KERNEL_APC = 0xFFFFFFFF;

// ---- guest structures: offsets as Xenia's structs lay them out ----

// X_OBJECT_TYPES: the dispatcher header's type byte
enum class GuestType : uint8_t {
    EventNotification = 0x0,    // manual reset
    EventSynchronization = 0x1, // auto reset
    Mutant = 0x2,
    Process = 0x3,
    Queue = 0x4,
    Semaphore = 0x5,
    Thread = 0x6,
    TimerNotification = 0x8,
    TimerSynchronization = 0x9,
    Apc = 0x12,
    Dpc = 0x13,
};

// X_DISPATCH_HEADER (0x10): type, absolute (critical sections: spin count
// / 256), size, inserted, signal_state, wait_list
namespace header {
constexpr uint32_t TYPE = 0x0, ABSOLUTE = 0x1, SIGNAL_STATE = 0x4, WAIT_LIST = 0x8, SIZE = 0x10;
// Xenia's kXObjSignature: in wait_list.flink, a host object stands behind
// the header, its handle in wait_list.blink
constexpr uint32_t SIGNATURE = 0x58454E00; // 'XEN\0'
} // namespace header

// X_OBJECT_HEADER, before the bodies the kernel allocates
namespace object_header {
constexpr uint32_t POINTER_COUNT = 0x0, HANDLE_COUNT = 0x4, OBJECT_TYPE = 0x8, FLAGS = 0xC, SIZE = 0x10;
constexpr uint16_t IS_TITLE_OBJECT = 0x10;
} // namespace object_header

// X_OBJECT_TYPE (0x1C)
namespace object_type {
constexpr uint32_t POOL_TAG = 0x18, SIZE = 0x1C;
}

namespace ksemaphore {
constexpr uint32_t LIMIT = 0x10, SIZE = 0x14;
}
namespace kmutant {
constexpr uint32_t LIST = 0x10, OWNER = 0x18, ABANDONED = 0x1C, SIZE = 0x20;
}
namespace ktimer {
constexpr uint32_t DUE_TIME = 0x10, DPC = 0x20, PERIOD = 0x24, SIZE = 0x28;
}

// X_KTHREAD (0xAB0)
namespace kthread {
constexpr uint32_t MUTANTS_LIST = 0x10, WAIT_TIMEOUT_TIMER = 0x18, WAIT_TIMEOUT_BLOCK = 0x40;
constexpr uint32_t STACK_BASE = 0x5C, STACK_LIMIT = 0x60, STACK_KERNEL = 0x64, TLS_ADDRESS = 0x68;
constexpr uint32_t STATE = 0x6C, ALERTABLE = 0x6F, PRIORITY = 0x70, FPU_EXCEPTIONS = 0x71;
constexpr uint32_t PROCESS_TYPE_DUP = 0x72, PROCESS_TYPE = 0x73, APC_LISTS = 0x74, PROCESS = 0x84;
constexpr uint32_t MAY_QUEUE_APCS = 0x8B, MSR_MASK = 0x9C, WAIT_RESULT = 0xA0, APC_DISABLE_COUNT = 0xB0;
constexpr uint32_t QUANTUM = 0xB4, BASE_PRIORITY = 0xB9, BOOST_DISABLED = 0xBB, SUSPEND_COUNT = 0xBC, TERMINATED = 0xBE;
constexpr uint32_t CURRENT_CPU = 0xBF, A_PRCB = 0xC0, ANOTHER_PRCB = 0xC4, PROCESS_PRIORITY_CLASS = 0xC8;
constexpr uint32_t BASE_PRIORITY_COPY = 0xC9, MAX_DYNAMIC_PRIORITY = 0xCA, STACK_ALLOC_BASE = 0xD0;
constexpr uint32_t PROCESS_THREADS = 0x110, CREATE_TIME = 0x130, EXIT_TIME = 0x138, EXIT_STATUS = 0x140;
constexpr uint32_t TIMER_LIST = 0x144, THREAD_ID = 0x14C, START_ADDRESS = 0x150, UNK_154 = 0x154;
constexpr uint32_t LAST_ERROR = 0x160, CREATION_FLAGS = 0x16C, FPU_CONTEXT = 0x988, SIZE = 0xAB0;
// X_KTHREAD_STATE_FLAGS
constexpr uint8_t INITIALIZED = 0, READY = 1, RUNNING = 2, TERMINATED_STATE = 4, WAITING = 5;
} // namespace kthread

// X_KPCR (0x2D8), r13; its X_KPRCB at 0x100
namespace kpcr {
constexpr uint32_t TLS_PTR = 0x0, PROCESS_TYPE_IN_DPC = 0xC, CURRENT_IRQL = 0x18, PCR_PTR = 0x30;
constexpr uint32_t STACK_BASE = 0x70, STACK_END = 0x74, PRCB_DATA = 0x100;
constexpr uint32_t CURRENT_THREAD = 0x100, CURRENT_CPU = 0x10C, PROCESSOR_MASK = 0x110, DPC_ACTIVE = 0x150;
constexpr uint32_t PRCB = 0x2A8, SIZE = 0x2D8;
} // namespace kpcr

// X_KPROCESS (0x60)
namespace kprocess {
constexpr uint32_t THREAD_LIST = 0x4, QUANTUM = 0xC, THREAD_COUNT = 0x14, PRIORITY_CLASS = 0x18;
constexpr uint32_t DEFAULT_PRIORITY = 0x19, MAX_DYNAMIC_PRIORITY = 0x1A, DISABLE_QUANTUM_DECAY = 0x1B;
constexpr uint32_t KERNEL_STACK_SIZE = 0x1C, TLS_STATIC_DATA = 0x20, TLS_DATA_SIZE = 0x24;
constexpr uint32_t TLS_RAW_DATA_SIZE = 0x28, TLS_SLOT_SIZE = 0x2C, PROCESS_TYPE = 0x2F, TLS_BITMAP = 0x30;
constexpr uint32_t UNK_54 = 0x54, SIZE = 0x60;
// X_PROCTYPE_*
constexpr uint8_t IDLE = 0, TITLE = 1, SYSTEM = 2;
} // namespace kprocess

// XAPC (0x28): processor mode 0 kernel, 1 user
namespace kapc {
constexpr uint32_t TYPE = 0x0, MODE = 0x2, ENQUEUED = 0x3, THREAD = 0x4, LIST = 0x8, KERNEL_ROUTINE = 0x10;
constexpr uint32_t RUNDOWN_ROUTINE = 0x14, NORMAL_ROUTINE = 0x18, NORMAL_CONTEXT = 0x1C, ARG1 = 0x20, ARG2 = 0x24;
constexpr uint32_t SIZE = 0x28;
// what the kernel's own APCs (a timer's routine) carry instead of a kernel
// routine: freed after delivery (Xenia's XAPC::kDummyKernelRoutine)
constexpr uint32_t DUMMY_KERNEL_ROUTINE = 0xF00DFF00;
} // namespace kapc

// XDPC (0x1C)
namespace kdpc {
constexpr uint32_t TYPE = 0x0, SELECTED_CPU = 0x2, DESIRED_CPU = 0x3, LIST = 0x4, ROUTINE = 0xC, CONTEXT = 0x10;
constexpr uint32_t ARG1 = 0x14, ARG2 = 0x18;
} // namespace kdpc

// X_TIME_STAMP_BUNDLE (KeTimeStampBundle)
namespace time_stamp_bundle {
constexpr uint32_t INTERRUPT_TIME = 0x0, SYSTEM_TIME = 0x8, TICK_COUNT = 0x10;
}

// X_OBJECT_ATTRIBUTES: the name, a PANSI_STRING, at 4
namespace object_attributes {
constexpr uint32_t NAME = 0x4;
}

class Objects;
class Thread;

// ---- references ----

// An intrusive reference (Xenia's object_ref): owns one count.
template <typename T> class Ref {
public:
    Ref() = default;
    explicit Ref(T *p) : p_(p) {} // adopts a count the caller holds
    Ref(const Ref &o) : p_(o.p_) {
        if (p_) p_->retain();
    }
    Ref(Ref &&o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Ref &operator=(Ref o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    ~Ref() {
        if (p_) p_->release();
    }
    static Ref retain(T *p) {
        if (p) p->retain();
        return Ref(p);
    }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    T &operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T *release() { return std::exchange(p_, nullptr); }

private:
    T *p_ = nullptr;
};

// ---- objects ----

class Object {
public:
    enum class Type : uint8_t { Event, Semaphore, Mutant, Timer, Thread, Other };

    Object(Objects &owner, Type type) : owner(owner), type_(type) {}
    virtual ~Object();
    Object(const Object &) = delete;
    Object &operator=(const Object &) = delete;

    Type type() const { return type_; }
    bool dispatcher() const { return type_ != Type::Other; }
    void retain() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }

    // its first handle (0 once every handle is closed)
    uint32_t handle() const { return handles.empty() ? 0 : handles.front(); }

    Objects &owner;
    // its body in guest memory (0: none); allocated: by the kernel, after an
    // X_OBJECT_HEADER (Xenia's CreateNative)
    uint32_t guest = 0;
    bool allocated = false;
    // under the lock
    std::vector<uint32_t> handles;
    std::string name;

private:
    Type type_;
    std::atomic<int32_t> refs_{1};
};

// One thread's wait on one object.
struct Waiter {
    Thread *thread;
    uint32_t index; // in the thread's wait
};

// An object with a dispatcher header in guest memory; its signal state is
// the guest header's, read and written under the lock.
class Dispatcher : public Object {
public:
    using Object::Object;
    int32_t signal_state() const;
    void set_signal_state(int32_t state);
    GuestType guest_type() const;
    // whether a wait by t would be satisfied now
    virtual bool signaled_for(const Thread &) const { return signal_state() > 0; }
    // a satisfied wait takes it; the status base (WAIT_0 or ABANDONED_WAIT_0)
    virtual uint32_t acquire(Thread &) { return X_STATUS_SUCCESS; }

    std::vector<Waiter> waiters; // under the lock, oldest first
};

class Event : public Dispatcher {
public:
    explicit Event(Objects &owner) : Dispatcher(owner, Type::Event) {}
    bool manual_reset() const { return guest_type() == GuestType::EventNotification; }
    uint32_t acquire(Thread &) override {
        if (!manual_reset()) set_signal_state(0);
        return X_STATUS_SUCCESS;
    }
};

class Semaphore : public Dispatcher {
public:
    explicit Semaphore(Objects &owner) : Dispatcher(owner, Type::Semaphore) {}
    int32_t limit() const;
    uint32_t acquire(Thread &) override {
        set_signal_state(signal_state() - 1);
        return X_STATUS_SUCCESS;
    }
};

class Mutant : public Dispatcher {
public:
    explicit Mutant(Objects &owner) : Dispatcher(owner, Type::Mutant) {}
    bool signaled_for(const Thread &t) const override { return signal_state() > 0 || owner_thread == &t; }
    uint32_t acquire(Thread &t) override;
    void set_owner(Thread *t); // the guest's owner field too

    Thread *owner_thread = nullptr;
    bool abandoned = false;
};

class Timer : public Dispatcher {
public:
    explicit Timer(Objects &owner) : Dispatcher(owner, Type::Timer) {}
    bool manual_reset() const { return guest_type() == GuestType::TimerNotification; }
    uint32_t acquire(Thread &) override {
        if (!manual_reset()) set_signal_state(0);
        return X_STATUS_SUCCESS;
    }

    // armed: due on the scheduler's clock; period 0 fires once
    bool armed = false;
    uint64_t due = 0, period = 0;
    uint32_t routine = 0, routine_arg = 0; // queued as a user APC to apc_thread
    Ref<Thread> apc_thread;
};

// Thrown by ExTerminateThread and caught where the thread started.
struct ThreadExit {
    uint32_t code;
};

// A guest thread (Xenia's XThread): a KTHREAD in guest memory, a stack with
// guard pages in 0x70000000-0x7F000000, a PCR (r13), a TLS block, and the
// host's context it runs with.
class Thread : public Dispatcher {
public:
    explicit Thread(Objects &owner);
    ~Thread() override;

    struct Params {
        uint32_t stack_size = 0;
        uint32_t xapi_startup = 0; // XapiThreadStartup(start, context), else start(context)
        uint32_t start_address = 0;
        uint32_t start_context = 0;
        uint32_t flags = 0;   // ExCreateThread's: bit 0 suspended, top byte the processor mask
        uint32_t process = 0; // the KPROCESS (0: the title's)
    };
    // the guest side (stack, TLS, PCR, KTHREAD); X_STATUS
    uint32_t create(const Params &params);
    // what the host thread (or fiber) runs: the guest thread to its end
    void body();

    uint32_t kthread() const { return guest; }

    std::unique_ptr<ppc::Context> context;
    Params params;
    uint32_t id = 0;
    uint32_t pcr = 0, tls_static = 0, tls_dynamic = 0, tls_slots_size = 0, tls_total = 0;
    uint32_t stack_alloc = 0, stack_base = 0, stack_limit = 0; // base the high end, limit the low
    uint32_t exit_code = 0;
    uint32_t self_handle = 0; // the handle its hold on itself is on
    bool main = false;

    // what makes the thread stop at its next kernel call (a safe point)
    enum : uint32_t { PENDING_SUSPEND = 1, PENDING_KERNEL_APC = 2 };
    std::atomic<uint32_t> pending{0};

    // ---- under the lock ----
    enum class Blocked : uint8_t { No, Wait, Suspended };
    Blocked blocked = Blocked::No;
    bool started = false, terminated = false, finished = false; // finished: body() is done
    uint32_t suspend_count = 0;
    // the wait in progress
    std::vector<Dispatcher *> waiting_on;
    bool wait_all = false, alertable = false;
    uint32_t wait_status = X_STATUS_PENDING;
    std::vector<Mutant *> mutants; // owned, abandoned at exit
    int32_t base_priority_increment = 0;

    // the scheduler's
    bool woken = false;
    std::condition_variable cv;
};

// The thread running on this host thread (nullptr off guest threads).
Thread *current_thread();

// ---- the scheduler ----

// How guest threads get time. Playing mode: a host thread each, waits the
// host's (make_host_scheduler). The deterministic mode replaces it with
// fibers on one host thread and a virtual clock. Everything here is called
// with the kernel's lock held, except start, join and yield.
class Scheduler {
public:
    static constexpr uint64_t NEVER = UINT64_MAX;
    virtual ~Scheduler() = default;
    // runs t.body() (a host thread with a 16 MB stack)
    virtual bool start(Thread &t) = 0;
    // the host waits for t.body() to return
    virtual void join(Thread &t) = 0;
    // blocks the calling thread (self) until wake(self) or the deadline
    // (on now()'s clock), the lock released meanwhile; false at the deadline
    virtual bool block(Thread &self, std::unique_lock<std::mutex> &lock, uint64_t deadline) = 0;
    virtual void wake(Thread &t) = 0;
    // another thread may run (spin locks, NtYieldExecution)
    virtual void yield() = 0;
    // the clock: 100 ns units since the kernel started
    virtual uint64_t now() = 0;
    // the earliest timer (Objects::next_due) changed
    virtual void timers_changed() = 0;
};

std::unique_ptr<Scheduler> make_host_scheduler(Objects &objects);

// ---- the kernel's state for its objects ----

class Objects {
public:
    explicit Objects(Kernel &kernel);
    ~Objects();

    Kernel &k;
    // the dispatcher lock: the handle table, the objects' state, waits
    std::mutex lock;
    std::unique_ptr<Scheduler> scheduler;

    // handles: 0xF8000000 + slot * 4, as Xenia's (lock held)
    uint32_t add_handle(Object *object);
    bool retain_handle(uint32_t handle);
    bool release_handle(uint32_t handle);
    Object *object_in(uint32_t handle);  // not retained
    uint32_t translate(uint32_t handle); // -2: the current thread
    // a retained object by handle (nullptr: none, or not of type T)
    template <typename T> Ref<T> lookup(uint32_t handle);
    // names (case-insensitive): the handle, retained, or 0
    uint32_t find_name(const std::string &name);
    void set_name(Object &object, const std::string &name);

    // A new object of a kind with a dispatcher header, its body allocated
    // in guest memory after an X_OBJECT_HEADER (Xenia's CreateNative), with
    // a handle (the caller's to give out).
    template <typename T> Ref<T> create(GuestType type, uint32_t body_size) {
        Ref<T> object(new T(*this));
        if (!make_native(*object, type, body_size)) return {};
        return object;
    }
    bool make_native(Dispatcher &d, GuestType type, uint32_t body_size);
    // The host object behind a dispatcher header in guest memory, made on
    // first use (the XDK inlines KeInitializeEvent and friends; Xenia's
    // GetNativeObject). nullptr for what it cannot be.
    Ref<Dispatcher> native(uint32_t address);
    Ref<Dispatcher> native_locked(uint32_t address);
    // any object by its body's guest address (lock held; not retained)
    Object *by_guest(uint32_t address);

    // Waits (all or any) until the deadline on the scheduler's clock: the NT
    // wait's status (WAIT_0 + n, ABANDONED_WAIT_0 + n, TIMEOUT, USER_APC).
    // The caller delivers user APCs on USER_APC.
    uint32_t wait(Thread &self, std::span<Dispatcher *const> objects, bool all, bool alertable, uint64_t deadline);
    // d's state became signaled: the waits it satisfies (lock held)
    void signaled(Dispatcher &d);
    // ends t's wait with a status, waking it (lock held)
    void unwait(Thread &t, uint32_t status);

    // an NT timeout (negative: relative, in 100 ns; positive: an absolute
    // system time; none: forever) as a deadline on the scheduler's clock
    uint64_t deadline(uint32_t timeout_ptr);
    uint64_t now() { return scheduler->now(); }
    uint64_t system_time() { return system_time_base + now(); }

    // timers (lock held)
    void arm(Timer &t);
    void disarm(Timer &t);
    uint64_t next_due(); // Scheduler::NEVER: none armed
    // the clock ticked (lock held): due timers fire, KeTimeStampBundle moves
    void on_clock();

    // threads: made, not started (scheduler->start)
    Ref<Thread> create_thread(const Thread::Params &params, uint32_t &status);
    Ref<Thread> main_thread;
    std::vector<Thread *> threads; // under the lock, live
    uint32_t next_thread_id = 0;
    uint8_t next_cpu = 0;
    std::condition_variable thread_exited; // the host side's joins

    // a guest function on c's thread, as an interrupt runs (everything but
    // memory restored after); its r3
    // (reserve: bytes kept free above the callee's stack)
    uint64_t call(ppc::Context &c, uint32_t function, std::initializer_list<uint64_t> args, uint32_t reserve = 0);
    // the current thread's kernel calls start here (Kernel::run_import)
    void safe_point(Thread &t);
    // APCs: the thread's queued ones run (user: mode 1)
    bool user_apc_pending(Thread &t); // lock held
    bool deliver_apcs(Thread &t, uint32_t mode);
    // queues an APC the kernel made (a timer's routine) to t
    void queue_user_apc(Thread &t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2);
    // after KeInsertQueueApc put one on t's list (lock held)
    void apc_queued(Thread &t, uint32_t mode);

    // symbolic links (ObCreateSymbolicLink), for the file system
    std::vector<std::pair<std::string, std::string>> symbolic_links;

    // guest memory the objects use
    uint32_t title_process = 0, system_process = 0;
    uint32_t time_stamp_bundle = 0;
    uint32_t type_thread = 0, type_event = 0, type_semaphore = 0, type_mutant = 0, type_timer = 0;
    uint64_t system_time_base = 0; // the system time when the clock was 0

    // The title process's TLS slots and data (Xenia's SetProcessTLSVars),
    // from the executable's TLS header.
    void set_tls(uint32_t slot_count, uint32_t data_size, uint32_t raw_data_address, uint32_t raw_data_size);
    // each thread's block: slots (Xenia's 1024 without a TLS header), data
    uint32_t tls_slot_count = 1024, tls_data_size = 0, tls_raw_address = 0, tls_raw_size = 0;
    uint32_t object_type_for(const Object &o) const;

private:
    struct Slot {
        Object *object = nullptr;
        int32_t refs = 0;
    };
    std::vector<Slot> slots_;
    uint32_t free_cursor_ = 0;
    std::vector<std::pair<std::string, uint32_t>> names_; // lowercase name, handle
    std::vector<Timer *> armed_;
    uint32_t initialize_native(Dispatcher &d, uint32_t address);
    void remove_waits(Thread &t);
    bool try_satisfy(Thread &t, std::span<Dispatcher *const> objects, bool all, uint32_t &status);
};

template <typename T> Ref<T> Objects::lookup(uint32_t handle) {
    std::lock_guard<std::mutex> l(lock);
    Object *o = object_in(handle);
    if (!o) return {};
    if constexpr (!std::is_same_v<T, Object>) {
        if (!dynamic_cast<T *>(o)) return {};
    }
    o->retain();
    return Ref<T>(static_cast<T *>(o));
}

// ---- small things the families share ----

// guest lists (X_LIST_ENTRY: flink, blink)
void list_init(Kernel &k, uint32_t head);
bool list_empty(Kernel &k, uint32_t head);
void list_insert_tail(Kernel &k, uint32_t head, uint32_t entry);
void list_insert_head(Kernel &k, uint32_t head, uint32_t entry);
void list_remove(Kernel &k, uint32_t entry);

// an X_ANSI_STRING's text
std::string ansi_string(Kernel &k, uint32_t address);
// the name in X_OBJECT_ATTRIBUTES, if any
std::string attributes_name(Kernel &k, uint32_t attributes);

// atomics on guest words (big-endian), for spin locks and counters
std::atomic_ref<uint32_t> guest_word(Kernel &k, uint32_t address);
inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

Objects &objects(Kernel &k);

} // namespace xf::kernel
