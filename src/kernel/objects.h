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

#include "stacks.h"
#include "xenoforge/kernel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xf::kernel {

// ---- statuses (Xenia's xbox.h) ----

constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_NO_YIELD_PERFORMED = 0x40000024;
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
// Xenia's kXObjSignature. A dispatcher header's wait list no longer holds it
// (the console leaves an empty list; Objects keeps the table); XAM's
// enumerators still mark their bodies with it.
constexpr uint32_t SIGNATURE = 0x58454E00; // 'XEN\0'
} // namespace header

// X_OBJECT_HEADER, before the bodies the kernel allocates
namespace object_header {
constexpr uint32_t POINTER_COUNT = 0x0, HANDLE_COUNT = 0x4, OBJECT_TYPE = 0x8, FLAGS = 0xC, SIZE = 0x10;
constexpr uint16_t IS_TITLE_OBJECT = 0x10;
} // namespace object_header

// X_OBJECT_TYPE (0x1C): the procedures the object manager calls (allocate,
// free, close, delete, parse), the default object, the pool tag
namespace object_type {
constexpr uint32_t ALLOCATE = 0x0, FREE = 0x4, CLOSE = 0x8, DELETE = 0xC, PARSE = 0x10, DEFAULT_OBJECT = 0x14;
constexpr uint32_t POOL_TAG = 0x18, SIZE = 0x1C;
} // namespace object_type

// X_OBJECT_HEADER_NAME_INFO (0x10), before the header of a named object:
// the next in its directory, the directory, the name (an X_ANSI_STRING)
namespace object_name_info {
constexpr uint32_t NEXT = 0x0, DIRECTORY = 0x4, NAME = 0x8, SIZE = 0x10;
} // namespace object_name_info

// X_OBJECT_HEADER's flags
namespace object_flags {
constexpr uint16_t NAMED = 0x1;
}

// OBJ_* in X_OBJECT_ATTRIBUTES' attributes
namespace obj {
constexpr uint32_t PERMANENT = 0x10, CASE_INSENSITIVE = 0x40, OPEN_IF = 0x80;
constexpr uint32_t TITLE_POOL = 0x1000, SYSTEM_POOL = 0x2000; // the xbox's: which pool ObCreateObject uses
} // namespace obj

namespace ksemaphore {
constexpr uint32_t LIMIT = 0x10, SIZE = 0x14;
}
namespace kmutant {
constexpr uint32_t LIST = 0x10, OWNER = 0x18, ABANDONED = 0x1C, SIZE = 0x20;
}
namespace ktimer {
constexpr uint32_t DUE_TIME = 0x10, DPC = 0x20, PERIOD = 0x24, SIZE = 0x28;
}

// X_KWAIT_BLOCK (0x18): the list entry, then the thread, the object, the next block, the key and the wait type
namespace kwait_block {
constexpr uint32_t THREAD = 0x8, OBJECT = 0xC, NEXT = 0x10, KEY = 0x14, TYPE = 0x16, SIZE = 0x18;
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
constexpr uint32_t WAIT_BLOCKS = 0xA8, ON_SUSPEND_APC = 0xD4, SUSPEND_SEMAPHORE = 0xFC;
// X_KTHREAD_STATE_FLAGS
constexpr uint8_t INITIALIZED = 0, READY = 1, RUNNING = 2, TERMINATED_STATE = 4, WAITING = 5;
} // namespace kthread

// X_KPCR (0x2D8), r13; its X_KPRCB at 0x100
namespace kpcr {
constexpr uint32_t TLS_PTR = 0x0, PROCESS_TYPE_IN_DPC = 0xC, CURRENT_IRQL = 0x18, PCR_PTR = 0x30;
constexpr uint32_t USE_ALTERNATIVE_STACK = 0x6C, STACK_BASE = 0x70, STACK_END = 0x74;
constexpr uint32_t ALT_STACK_BASE = 0x78, ALT_STACK_END = 0x7C, PRCB_DATA = 0x100;
constexpr uint32_t CURRENT_THREAD = 0x100, CURRENT_CPU = 0x10C, PROCESSOR_MASK = 0x110, DPC_ACTIVE = 0x150;
constexpr uint32_t PRCB = 0x2A8, SIZE = 0x2D8;
// the FPU and vector state owners (-1: none; a save area's address while the kernel's own use holds them), and the
// KPRCB's queue of DPCs (a list head; the console's KeInsertQueueDpc links a DPC there)
constexpr uint32_t FPU_OWNER = 0x10, VMX_OWNER = 0x14, DPC_QUEUE = PRCB_DATA + 0x48;
// the byte of software interrupts requested (2: the DPC interrupt, which KfLowerIrql honors below DISPATCH_LEVEL)
constexpr uint32_t SOFTWARE_INTERRUPT = 0x8;
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

// X_OBJECT_ATTRIBUTES: the root directory, the name (a PANSI_STRING), the
// attributes
namespace object_attributes {
constexpr uint32_t ROOT_DIRECTORY = 0x0, NAME = 0x4, ATTRIBUTES = 0x8;
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
    // the files and XAM families' kinds: File (an auto-reset event, signaled
    // after each I/O), IoCompletion (a queue), NotifyListener (a manual-reset
    // event, signaled while notifications wait); Other has no dispatcher
    // header
    enum class Type : uint8_t { Event, Semaphore, Mutant, Timer, Thread, File, IoCompletion, NotifyListener, Other };

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
    ~Mutant() override; // off its owner's list (the guest's too) before the object goes
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
    uint32_t dpc = 0; // the KDPC (KeSetTimerEx's), queued as the timer comes due
};

// An object the guest made with ObCreateObject (or the kernel made the same
// way, as IoCreateDevice does), of a type the kernel or a title defined: its
// body is in guest memory after an X_OBJECT_HEADER, and the header holds the
// counts (pointer and handle), as NT's. It has no dispatcher header of the
// kernel's kinds; what it does is the type's procedures (close, delete).
class GuestObject : public Object {
public:
    explicit GuestObject(Objects &owner) : Object(owner, Type::Other) {}
    uint32_t header() const { return guest - object_header::SIZE; }

    uint32_t allocation = 0;     // the allocation's start: the name info of a named object, else the header
    bool host_allocated = false; // not from the type's allocate procedure: freed by the host
    bool inserted = false;       // ObInsertObject (or the kernel's own insertion) done
    bool permanent = false;      // OBJ_PERMANENT: the name stays with no handle open
    uint32_t permanent_handle = 0;
    std::string path; // the full name it is inserted under (its header holds only the last element)
};

// Thrown by ExTerminateThread and caught where the thread started.
struct ThreadExit {
    uint32_t code;
};

// KeBugCheckEx (misc.cpp): the stop code and its parameters logged, the
// kernel stopped, the process ended; also what an exception no handler takes
// ends in (exceptions.cpp).
[[noreturn]] void bug_check(Kernel &k, uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4);

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
        // A kernel thread whose body is host code (Xenia's XHostThread: the
        // GPU's command processor and vblank), in place of a start address;
        // it calls guest code on its own context (Objects::call).
        void (*host)(Thread &self, void *arg) = nullptr;
        void *host_arg = nullptr;
        const char *name = nullptr; // for the thread report
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
    std::string guest_name; // what the guest named it (SetThreadName), for the thread report

    // what makes the thread stop at its next kernel call (a safe point)
    enum : uint32_t { PENDING_SUSPEND = 1, PENDING_KERNEL_APC = 2 };
    std::atomic<uint32_t> pending{0};

    // the guest stacks it switched between and their host fibers (stacks.h),
    // from its first switch; where the generated code that branched to
    // KeSetCurrentStackPointers goes on when its stack is switched back to
    // (ppc::switch_stack, read by the switch)
    std::unique_ptr<GuestStacks> stacks;
    uint32_t stack_resume = 0;

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
    Object *resolve_guest(GuestObject &g); // lock held: the host object behind a guest object's header, else g
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
    // the host object made for the header at a guest address, if any (lock
    // held; not retained). The header holds no pointer to it: its wait list
    // is empty, as on the console.
    Object *find_native(uint32_t address);
    // any object by its body's guest address (lock held; not retained)
    Object *by_guest(uint32_t address);

    // ---- objects the guest made with ObCreateObject (guest_objects.cpp) ----
    // Registered when made, whether or not they are inserted yet; the table
    // holds a reference. The counts are the header's. Lock held, except where
    // a guest procedure runs (close_handle, delete_guest_object).
    GuestObject *guest_object(uint32_t body); // not retained
    GuestObject &add_guest_object(uint32_t body, uint32_t allocation, bool host_allocated, std::string path,
                                  bool permanent);
    // the header and name info over an allocation; the body's address (element: the last name part, if named)
    uint32_t init_guest_object(uint32_t memory, uint32_t type, uint32_t body_size, const std::string &element,
                               uint16_t flags);
    // an object the kernel makes itself, in memory of its own (devices): registered, not inserted; 0 on failure
    uint32_t make_guest_object(uint32_t type, uint32_t body_size, const std::string &path, bool title_object);
    // ObInsertObject: its status, the handle, and the new object when it has to be deleted
    struct Insertion {
        uint32_t status = 0, handle = 0;
        Ref<GuestObject> discard;
    };
    Insertion insert_guest_object(GuestObject &g, const std::string &path, uint32_t attributes, uint32_t bias);
    void reference_guest(GuestObject &g);
    bool dereference_guest(GuestObject &g); // true: the last reference is gone, delete_guest_object is next
    uint32_t open_guest_handle(GuestObject &g);
    bool drop_guest_handle(GuestObject &g, uint32_t handle); // a handle closed, no close procedure; true: dead
    bool make_temporary(GuestObject &g);                     // ObMakeTemporaryObject; true: dead
    // the delete procedure, the object forgotten, then the free procedure
    void delete_guest_object(ppc::Context &c, Ref<GuestObject> g);
    // NtClose (lock not held): a guest object's close procedure and, at its last, its deletion
    uint32_t close_handle(ppc::Context &c, uint32_t handle);

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
    uint64_t deadline_at(int64_t timeout); // the same, the time given as a value
    uint64_t now() { return scheduler->now(); }
    uint64_t system_time() { return system_time_base + now(); }

    // timers (lock held)
    void arm(Timer &t);
    void disarm(Timer &t);
    uint64_t next_due(); // Scheduler::NEVER: none armed
    // the clock ticked (lock held): due timers fire, KeTimeStampBundle moves
    void on_clock();
    // where a timer with a DPC (KeSetTimerEx) sends it when it comes due
    // (lock held; the system time then): the family that runs DPCs sets it
    std::function<void(Timer &timer, uint64_t system_time)> on_timer_dpc;

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
    uint32_t type_thread = 0, type_event = 0, type_semaphore = 0, type_mutant = 0, type_timer = 0, type_file = 0;
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
    std::vector<std::pair<std::string, uint32_t>> names_;   // lowercase name, handle
    std::unordered_map<uint32_t, uint32_t> native_handles_; // a header's guest address, the handle of its object
    std::unordered_map<uint32_t, Ref<GuestObject>> guest_objects_; // by body
    std::vector<Timer *> armed_;
    uint32_t initialize_native(Dispatcher &d, uint32_t address);
    void remove_waits(Thread &t);
    bool try_satisfy(Thread &t, std::span<Dispatcher *const> objects, bool all, uint32_t &status);
};

template <typename T> Ref<T> Objects::lookup(uint32_t handle) {
    std::lock_guard<std::mutex> l(lock);
    Object *o = object_in(handle);
    if (!o) return {};
    // a handle to an object the guest made, with a dispatcher header the caller initialized: the host object
    // behind the header (the Ke* family and the waits see one object)
    if constexpr (!std::is_base_of_v<GuestObject, T>) {
        if (auto *g = dynamic_cast<GuestObject *>(o)) o = resolve_guest(*g);
    }
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

// the device objects there are at start (devices.cpp), once Objects exists
void create_devices(Kernel &k);

} // namespace xf::kernel
