/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Guest threads (objects.h): their creation as Xenia's XThread::Create
// makes them (stack, TLS, PCR, KTHREAD), the host scheduler, APCs and DPCs,
// TLS slots, IRQL and spin locks, critical regions, SLists, time, the stack
// limits, and the thread exports. Translated from Xenia (src/xenia/kernel:
// xthread.cc, kernel_state.cc's AllocateTLS, FreeTLS, InitializeProcess,
// SetProcessTLSVars and UpdateKeTimestampBundle, xboxkrnl/
// xboxkrnl_threading.cc, xboxkrnl_ob.cc's thread lookups, xboxkrnl_rtl.cc's
// RtlGetStackLimits; BSD, see LICENSE-xenia) and checked against them;
// where Xenia knowingly approximates (priorities, suspension, a DPC left
// queued after it ran), the NT kernel's behavior, as noted.

#include "objects.h"
#include "stacks.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace xf::kernel {
namespace {

thread_local Thread *current_ = nullptr;

constexpr uint32_t STACK_RANGE_BEGIN = 0x70000000, STACK_RANGE_END = 0x7F000000; // Xenia's XThread
constexpr size_t HOST_STACK = 16u << 20; // generated code recurses as deep as the guest's

// ---- the host scheduler: a host thread per guest thread ----

// The host's timer at its finest (Xenia's main_win.cc RequestHighPerformance:
// NtSetTimerResolution at the maximum, 0.5 ms), so the clock's millisecond
// and the guest's short waits are not Windows' default 15.6 ms.
void fine_host_timer() {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    using Query = LONG(NTAPI *)(PULONG, PULONG, PULONG);
    using Set = LONG(NTAPI *)(ULONG, BOOLEAN, PULONG);
    auto query = reinterpret_cast<Query>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryTimerResolution")));
    auto set = reinterpret_cast<Set>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtSetTimerResolution")));
    if (!query || !set) return;
    ULONG minimum, maximum, current;
    query(&minimum, &maximum, &current);
    set(maximum, TRUE, &current);
#endif
}

class HostScheduler final : public Scheduler {
public:
    explicit HostScheduler(Objects &o) : o_(o), epoch_(std::chrono::steady_clock::now()) {}
    ~HostScheduler() override {
        {
            std::lock_guard<std::mutex> l(o_.lock);
            stopping_ = true;
        }
        clock_cv_.notify_all();
        if (clock_.joinable()) clock_.join();
    }

    bool start(Thread &t) override {
        std::call_once(clock_started_, [this] {
            fine_host_timer();
            clock_ = std::thread([this] { clock(); });
        });
#ifdef _WIN32
        HANDLE h = CreateThread(
            nullptr, HOST_STACK,
            [](LPVOID p) -> DWORD {
                static_cast<Thread *>(p)->body();
                return 0;
            },
            &t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) return false;
        CloseHandle(h);
#else
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, HOST_STACK);
        pthread_t id;
        int r = pthread_create(
            &id, &attr,
            [](void *p) -> void * {
                static_cast<Thread *>(p)->body();
                return nullptr;
            },
            &t);
        pthread_attr_destroy(&attr);
        if (r) return false;
        pthread_detach(id);
#endif
        return true;
    }

    void join(Thread &t) override {
        std::unique_lock<std::mutex> l(o_.lock);
        o_.thread_exited.wait(l, [&] { return t.finished; });
    }

    bool block(Thread &self, std::unique_lock<std::mutex> &lock, uint64_t deadline) override {
        self.woken = false;
        while (!self.woken) {
            if (deadline == NEVER)
                self.cv.wait(lock);
            else if (self.cv.wait_until(lock, time_point(deadline)) == std::cv_status::timeout)
                return self.woken;
        }
        return true;
    }

    void wake(Thread &t) override {
        t.woken = true;
        t.cv.notify_one();
    }

    void yield() override { std::this_thread::yield(); }

    uint64_t now() override {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch_)
                            .count()) /
               100;
    }

    void timers_changed() override { clock_cv_.notify_one(); }

private:
    std::chrono::steady_clock::time_point time_point(uint64_t t) const {
        constexpr uint64_t CENTURY = 100ull * 365 * 24 * 3600 * 10'000'000;
        return epoch_ + std::chrono::nanoseconds(std::min(t, CENTURY) * 100);
    }

    // timers and KeTimeStampBundle, every millisecond (Xenia's 1 ms timer)
    void clock() {
        std::unique_lock<std::mutex> l(o_.lock);
        while (!stopping_) {
            o_.on_clock();
            uint64_t next = std::min(o_.next_due(), now() + 10'000);
            clock_cv_.wait_until(l, time_point(next));
        }
    }

    Objects &o_;
    std::chrono::steady_clock::time_point epoch_;
    std::once_flag clock_started_;
    std::thread clock_;
    std::condition_variable clock_cv_;
    bool stopping_ = false;
};

// IRQL in the PCR (Xenia's xeKfRaiseIrql, xeKfLowerIrql)
uint8_t raise_irql(Kernel &k, uint32_t pcr, uint8_t irql) {
    uint8_t old = k.host(pcr)[kpcr::CURRENT_IRQL];
    k.host(pcr)[kpcr::CURRENT_IRQL] = irql;
    return old;
}

// Xenia's xeKeKfAcquireSpinLock: the owner's PCR in the lock word
void acquire_spin_lock(Objects &o, ppc::Context &c, uint32_t lock) {
    auto word = guest_word(o.k, lock);
    uint32_t mine = bswap32(uint32_t(c.r[13]));
    for (uint32_t spins = 0;; spins++) {
        uint32_t expected = 0;
        if (word.compare_exchange_weak(expected, mine, std::memory_order_acquire)) return;
        if (spins >= 64) o.scheduler->yield();
    }
}

void release_spin_lock(Objects &o, uint32_t lock) { guest_word(o.k, lock).store(0, std::memory_order_release); }

// Xenia's GetFakeCpuNumber: the processor a thread starts on
uint8_t fake_cpu(Objects &o, uint8_t mask) {
    if (!mask) {
        if (Thread *parent = current_thread()) return o.k.host(parent->pcr)[kpcr::CURRENT_CPU];
        o.next_cpu = uint8_t((o.next_cpu + 1) % 6);
        return o.next_cpu;
    }
    return uint8_t(std::min(31 - __builtin_clz(uint32_t(mask)), 5));
}

void set_cpu(Kernel &k, Thread &t, uint8_t cpu) {
    k.host(t.pcr)[kpcr::CURRENT_CPU] = cpu;
    k.host(t.kthread())[kthread::CURRENT_CPU] = cpu;
}

} // namespace

Thread *current_thread() { return current_; }

std::unique_ptr<Scheduler> make_host_scheduler(Objects &objects) { return std::make_unique<HostScheduler>(objects); }

// ---- threads (Xenia's XThread) ----

Thread::Thread(Objects &owner) : Dispatcher(owner, Type::Thread) {}

Thread::~Thread() {
    Memory &memory = owner.k.memory();
    memory.system_free(tls_static);
    memory.system_free(pcr);
    if (stack_alloc)
        if (Heap *heap = memory.heap_for(stack_alloc)) heap->release(stack_alloc);
}

uint32_t Thread::create(const Params &p) {
    Kernel &k = owner.k;
    Memory &memory = k.memory();
    params = p;
    uint32_t process = p.process ? p.process : owner.title_process;
    uint32_t t = kthread();

    // the stack, guard pages either side (Xenia's AllocateStack)
    Heap *heap = memory.heap_for(STACK_RANGE_BEGIN);
    uint32_t page = heap->page_size();
    uint32_t size = (std::max<uint32_t>(p.stack_size, 16 * 1024) + page - 1) & ~(page - 1);
    if (!heap->alloc_range(STACK_RANGE_BEGIN, STACK_RANGE_END, size + 2 * page, page,
                           ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE, false, &stack_alloc)) {
        stack_alloc = 0;
        return X_STATUS_NO_MEMORY;
    }
    stack_limit = stack_alloc + page, stack_base = stack_limit + size;
    heap->protect(stack_alloc, page, 0);
    heap->protect(stack_base, page, 0);

    // TLS: the slots, after the executable's __declspec(thread) data
    uint32_t slots = owner.tls_slot_count, data = owner.tls_data_size;
    tls_slots_size = slots * 4;
    tls_total = tls_slots_size + data;
    tls_static = memory.system_alloc(tls_total);
    pcr = memory.system_alloc(kpcr::SIZE);
    if (!tls_static || !pcr) return X_STATUS_NO_MEMORY;
    tls_dynamic = tls_static + data;
    if (data && owner.tls_raw_address)
        std::memcpy(k.host(tls_static), k.host(owner.tls_raw_address), std::min(owner.tls_raw_size, data));

    context = std::make_unique<ppc::Context>();
    std::memset(context.get(), 0, sizeof(ppc::Context));
    k.attach(*context);
    context->r[1] = stack_base;
    context->r[2] = 0x20000000;
    context->r[13] = pcr;
    context->msr = 0x9030;

    std::lock_guard<std::mutex> l(owner.lock);
    id = ++owner.next_thread_id;
    uint8_t cpu = fake_cpu(owner, uint8_t(p.flags >> 24));

    // the KTHREAD (Xenia's InitializeGuestObject)
    uint8_t *g = k.host(t);
    g[header::TYPE] = uint8_t(GuestType::Thread);
    suspend_count = p.flags & 1;
    g[kthread::SUSPEND_COUNT] = uint8_t(suspend_count);
    list_init(k, t + kthread::MUTANTS_LIST);
    uint32_t timeout_timer = t + kthread::WAIT_TIMEOUT_TIMER, block = t + kthread::WAIT_TIMEOUT_BLOCK;
    k.store32(block + 0, timeout_timer + header::WAIT_LIST);
    k.store32(block + 4, timeout_timer + header::WAIT_LIST);
    k.store32(block + 8, t);
    k.store32(block + 0xC, timeout_timer);
    k.store16(block + 0x14, uint16_t(X_STATUS_TIMEOUT));
    k.store16(block + 0x16, 1); // WaitAny
    k.store32(t + kthread::STACK_BASE, stack_base);
    k.store32(t + kthread::STACK_LIMIT, stack_limit);
    k.store32(t + kthread::STACK_KERNEL, stack_base - 240);
    k.store32(t + kthread::TLS_ADDRESS, tls_dynamic);
    g[kthread::STATE] = kthread::INITIALIZED;
    g[kthread::PROCESS_TYPE_DUP] = g[kthread::PROCESS_TYPE] = k.host(process)[kprocess::PROCESS_TYPE];
    list_init(k, t + kthread::APC_LISTS);
    list_init(k, t + kthread::APC_LISTS + 8);
    g[kthread::PROCESS_PRIORITY_CLASS] = k.host(process)[kprocess::PRIORITY_CLASS];
    uint8_t priority = k.host(process)[kprocess::DEFAULT_PRIORITY];
    g[kthread::BASE_PRIORITY_COPY] = g[kthread::BASE_PRIORITY] = g[kthread::PRIORITY] = priority;
    g[kthread::MAX_DYNAMIC_PRIORITY] = k.host(process)[kprocess::MAX_DYNAMIC_PRIORITY];
    k.store32(t + kthread::QUANTUM, k.load32(process + kprocess::QUANTUM));
    k.store32(t + kthread::A_PRCB, pcr + kpcr::PRCB_DATA);
    k.store32(t + kthread::ANOTHER_PRCB, pcr + kpcr::PRCB_DATA);
    g[kthread::MAY_QUEUE_APCS] = 1;
    k.store32(t + kthread::MSR_MASK, 0xFDFFD7FF);
    k.store32(t + kthread::PROCESS, process);
    k.store32(t + kthread::STACK_ALLOC_BASE, stack_base);
    k.store64(t + kthread::CREATE_TIME, owner.system_time());
    list_init(k, t + kthread::TIMER_LIST);
    k.store32(t + kthread::THREAD_ID, id);
    k.store32(t + kthread::START_ADDRESS, p.start_address);
    list_init(k, t + kthread::UNK_154);
    k.store32(t + kthread::CREATION_FLAGS, p.flags);
    list_insert_tail(k, process + kprocess::THREAD_LIST, t + kthread::PROCESS_THREADS);
    k.store32(process + kprocess::THREAD_COUNT, k.load32(process + kprocess::THREAD_COUNT) + 1);

    // the PCR
    k.store32(pcr + kpcr::TLS_PTR, tls_static);
    k.store64(pcr + kpcr::PCR_PTR, pcr);
    k.store32(pcr + kpcr::CURRENT_THREAD, t);
    k.store32(pcr + kpcr::PRCB, pcr + kpcr::PRCB_DATA);
    k.store32(pcr + kpcr::STACK_BASE, stack_base);
    k.store32(pcr + kpcr::STACK_END, stack_limit);
    k.store32(pcr + kpcr::DPC_ACTIVE, 0);
    k.store32(pcr + kpcr::FPU_OWNER, 0xFFFFFFFF);
    k.store32(pcr + kpcr::VMX_OWNER, 0xFFFFFFFF);
    list_init(k, pcr + kpcr::DPC_QUEUE);
    set_cpu(k, *this, cpu);
    if (suspend_count) pending.fetch_or(PENDING_SUSPEND);
    return X_STATUS_SUCCESS;
}

void Thread::body() {
    Objects &o = owner;
    Kernel &k = o.k;
    current_ = this;
    {
        std::unique_lock<std::mutex> l(o.lock);
        started = true;
        while (suspend_count > 0) {
            blocked = Blocked::Suspended;
            o.scheduler->block(*this, l, Scheduler::NEVER);
            blocked = Blocked::No;
        }
        pending.fetch_and(~uint32_t(PENDING_SUSPEND));
        k.host(guest)[kthread::STATE] = kthread::RUNNING;
    }
    ppc::Context &c = *context;
    uint32_t code = 0;
    try {
        // the DLLs' entry points (Xenia's OnThreadExecute; the kernel's own
        // threads, host code, get none)
        if (!params.host) modules_thread_start(k, c);
        o.deliver_apcs(*this, 1); // the ones queued before it ran (Xenia's DeliverAPCs)
        // Xenia's Processor::Execute pads the stack by 64 + 112
        c.r[1] = stack_base - (64 + 112);
        if (params.host) {
            params.host(*this, params.host_arg);
        } else if (params.xapi_startup) {
            c.r[3] = params.start_address, c.r[4] = params.start_context;
            k.call(c, params.xapi_startup);
        } else {
            c.r[3] = params.start_context;
            k.call(c, params.start_address);
            code = uint32_t(c.r[3]);
        }
    } catch (const ThreadExit &e) {
        code = e.code;
    }
    // back on its own stack (a fiber's exit is thrown again there): the
    // other guest stacks' host fibers go
    end_guest_stacks(k, *this);
    // and as it exits (OnThreadExit), before it is marked terminated and
    // taken off its process (NT's order; Xenia's XThread::Exit calls it
    // last). An ExitThread in a DllMain there ends that DllMain only
    // (modules_thread_end); the exit code stays.
    if (!params.host) modules_thread_end(k, c);

    // the exit (Xenia's XThread::Exit): signaled, its mutants abandoned
    std::unique_lock<std::mutex> l(o.lock);
    exit_code = code;
    uint32_t t = guest;
    k.host(t)[kthread::TERMINATED] = 1;
    k.host(t)[kthread::STATE] = kthread::TERMINATED_STATE;
    k.store32(t + kthread::EXIT_STATUS, code);
    k.store64(t + kthread::EXIT_TIME, o.system_time());
    while (!mutants.empty()) {
        Mutant *m = mutants.back();
        m->abandoned = true;
        k.host(m->guest)[kmutant::ABANDONED] = 1;
        m->set_signal_state(1);
        m->set_owner(nullptr);
        o.signaled(*m);
    }
    uint32_t process = k.load32(t + kthread::PROCESS);
    list_remove(k, t + kthread::PROCESS_THREADS);
    k.store32(process + kprocess::THREAD_COUNT, k.load32(process + kprocess::THREAD_COUNT) - 1);
    terminated = true;
    set_signal_state(1);
    o.signaled(*this);
    std::erase(o.threads, this);
    finished = true;
    o.thread_exited.notify_all();
    current_ = nullptr;
    // the thread's hold on itself, last: this may be gone after it
    o.release_handle(self_handle);
}

Ref<Thread> Objects::create_thread(const Thread::Params &params, uint32_t &status) {
    Ref<Thread> t(new Thread(*this));
    if (!make_native(*t, GuestType::Thread, kthread::SIZE)) {
        status = X_STATUS_NO_MEMORY;
        return {};
    }
    status = t->create(params);
    std::lock_guard<std::mutex> l(lock);
    if (status != X_STATUS_SUCCESS) {
        release_handle(t->handle());
        return {};
    }
    // the thread holds itself until it exits (Xenia's RetainHandle in Create)
    t->self_handle = t->handle();
    retain_handle(t->self_handle);
    threads.push_back(t.get());
    return t;
}

uint64_t Objects::call(ppc::Context &c, uint32_t function, std::initializer_list<uint64_t> args, uint32_t reserve) {
    auto saved = std::make_unique<ppc::Context>(c);
    c.r[1] = c.r[1] - reserve - (64 + 112);
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    k.call(c, function);
    uint64_t r = c.r[3];
    c = *saved;
    return r;
}

void Objects::safe_point(Thread &t) {
    uint32_t p = t.pending.load();
    if (p & Thread::PENDING_SUSPEND) {
        std::unique_lock<std::mutex> l(lock);
        while (t.suspend_count > 0) {
            t.blocked = Thread::Blocked::Suspended;
            scheduler->block(t, l, Scheduler::NEVER);
            t.blocked = Thread::Blocked::No;
        }
        t.pending.fetch_and(~uint32_t(Thread::PENDING_SUSPEND));
    }
    if ((p & Thread::PENDING_KERNEL_APC) && !k.load32(t.kthread() + kthread::APC_DISABLE_COUNT)) deliver_apcs(t, 0);
}

// ---- the main thread ----

void Objects::set_tls(uint32_t slot_count, uint32_t data_size, uint32_t raw_data_address, uint32_t raw_data_size) {
    tls_slot_count = slot_count, tls_data_size = data_size;
    tls_raw_address = raw_data_address, tls_raw_size = raw_data_size;
    set_process_tls(k, title_process, slot_count, data_size, raw_data_address);
}

bool Kernel::launch(std::string &error) {
    Objects &o = *objects_;
    if (xex_ && xex_->has_tls)
        o.set_tls(xex_->tls.slot_count, xex_->tls.data_size, xex_->tls.raw_data_address, xex_->tls.raw_data_size);
    // the title's default stack (Xenia's LaunchModule; the title process
    // has its size from setup_modules)
    uint32_t stack = xex_ ? xex_->stack_size : 0;
    Thread::Params p;
    p.stack_size = stack;
    p.start_address = entry_;
    uint32_t status;
    o.main_thread = o.create_thread(p, status);
    if (!o.main_thread) {
        char text[64];
        std::snprintf(text, sizeof(text), "the main thread: status 0x%08X", status);
        return error = text, false;
    }
    o.main_thread->main = true;
    if (!o.scheduler->start(*o.main_thread)) return error = "the main thread: no host thread", false;
    return true;
}

bool Kernel::wait_main(uint32_t timeout_ms, uint32_t *exit_code) {
    Objects &o = *objects_;
    if (!o.main_thread) return false;
    std::unique_lock<std::mutex> l(o.lock);
    Thread &t = *o.main_thread;
    if (!o.thread_exited.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return t.finished; })) return false;
    if (exit_code) *exit_code = t.exit_code;
    return true;
}

ppc::Context *Kernel::current_context() {
    Thread *t = current_thread();
    return t ? t->context.get() : nullptr;
}

std::string Kernel::thread_report() {
    Objects &o = *objects_;
    std::lock_guard<std::mutex> l(o.lock);
    std::string out;
    for (Thread *t : o.threads) {
        const char *state = t->blocked == Thread::Blocked::Wait        ? "waiting"
                            : t->blocked == Thread::Blocked::Suspended ? "suspended"
                                                                       : "running";
        char line[320];
        const char *name = t->params.name ? t->params.name : t->guest_name.c_str();
        std::snprintf(line, sizeof(line), "  thread %u%s%s%s (handle 0x%08X, start 0x%08X): %s, pc 0x%08X, lr 0x%08X\n",
                      t->id, t->main ? " (main)" : "", name[0] ? ", " : "", name, t->self_handle,
                      t->params.start_address, state, t->context->pc, uint32_t(t->context->lr));
        out += line;
        // what a waiting thread waits on: each object's kind, handle, body
        // and signal state
        if (t->blocked == Thread::Blocked::Wait && !t->waiting_on.empty()) {
            static const char *const kinds[] = {"event", "semaphore",      "mutant",          "timer", "thread",
                                                "file",  "I/O completion", "notify listener", "other"};
            out += t->waiting_on.size() > 1 ? (t->wait_all ? "    waits for all of:" : "    waits for any of:")
                                            : "    waits for";
            for (Dispatcher *d : t->waiting_on) {
                std::snprintf(line, sizeof(line), " %s 0x%08X (body 0x%08X, type %u, state %d%s%s)",
                              kinds[size_t(d->type())], d->handle(), d->guest, unsigned(d->guest_type()),
                              d->signal_state(), d->name.empty() ? "" : ", ", d->name.c_str());
                out += line;
            }
            out += t->alertable ? ", alertable\n" : "\n";
        }
        // the guest's stack, by its back chain
        std::string frames;
        for (uint32_t lr : guest_backtrace(*this, uint32_t(t->context->r[1]))) {
            std::snprintf(line, sizeof(line), " 0x%08X", lr);
            frames += line;
        }
        if (!frames.empty()) out += "    stack:" + frames + "\n";
    }
    return out;
}

// ---- APCs (Xenia's xeKeInitializeApc, xeKeInsertQueueApc, xeProcessUserApcs) ----

bool Objects::user_apc_pending(Thread &t) { return !list_empty(k, t.kthread() + kthread::APC_LISTS + 8); }

bool Objects::deliver_apcs(Thread &t, uint32_t mode) {
    ppc::Context &c = *t.context;
    uint32_t head = t.kthread() + kthread::APC_LISTS + 8 * mode;
    if (mode == 0) t.pending.fetch_and(~uint32_t(Thread::PENDING_KERNEL_APC));
    bool any = false;
    for (;;) {
        uint32_t apc, kernel_routine, routine[4];
        {
            std::lock_guard<std::mutex> l(lock);
            if (list_empty(k, head)) break;
            uint32_t entry = k.load32(head);
            apc = entry - kapc::LIST;
            list_remove(k, entry);
            k.host(apc)[kapc::ENQUEUED] = 0;
            kernel_routine = k.load32(apc + kapc::KERNEL_ROUTINE);
            for (int i = 0; i < 4; i++) routine[i] = k.load32(apc + kapc::NORMAL_ROUTINE + 4 * i);
        }
        any = true;
        if (kernel_routine != kapc::DUMMY_KERNEL_ROUTINE) {
            // the kernel routine may change what the normal routine gets:
            // it receives pointers to them, on the guest's stack
            uint32_t scratch = uint32_t(c.r[1]) - 16;
            for (int i = 0; i < 4; i++) k.store32(scratch + 4 * i, routine[i]);
            call(c, kernel_routine, {apc, scratch, scratch + 4, scratch + 8, scratch + 12}, 32);
            for (int i = 0; i < 4; i++) routine[i] = k.load32(scratch + 4 * i);
        } else {
            k.memory().system_free(apc);
        }
        if (routine[0]) call(c, routine[0], {routine[1], routine[2], routine[3]});
    }
    return any;
}

void Objects::apc_queued(Thread &t, uint32_t mode) {
    if (mode == 1) {
        if (t.blocked == Thread::Blocked::Wait && t.alertable) unwait(t, X_STATUS_USER_APC);
        return;
    }
    // kernel APCs run at the thread's next kernel call, or now if it waits
    t.pending.fetch_or(Thread::PENDING_KERNEL_APC);
    if (t.blocked == Thread::Blocked::Wait) unwait(t, WAKE_FOR_KERNEL_APC); // the wait goes on after them
}

void Objects::queue_user_apc(Thread &t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2) {
    uint32_t apc = k.memory().system_alloc(kapc::SIZE);
    if (!apc) return;
    k.store16(apc + kapc::TYPE, 18);
    k.host(apc)[kapc::MODE] = 1;
    k.store32(apc + kapc::THREAD, t.kthread());
    k.store32(apc + kapc::KERNEL_ROUTINE, kapc::DUMMY_KERNEL_ROUTINE);
    k.store32(apc + kapc::NORMAL_ROUTINE, routine);
    k.store32(apc + kapc::NORMAL_CONTEXT, context);
    k.store32(apc + kapc::ARG1, arg1);
    k.store32(apc + kapc::ARG2, arg2);
    list_insert_tail(k, t.kthread() + kthread::APC_LISTS + 8, apc + kapc::LIST);
    k.host(apc)[kapc::ENQUEUED] = 1;
    apc_queued(t, 1);
}

XF_EXPORT(xboxkrnl, KeInitializeApc) {
    uint32_t apc = arg(k, c, 0), thread = arg(k, c, 1), kernel_routine = arg(k, c, 2), rundown = arg(k, c, 3);
    uint32_t normal_routine = arg(k, c, 4), mode = arg(k, c, 5), normal_context = arg(k, c, 6);
    k.store32(apc + kapc::THREAD, thread);
    k.store32(apc + kapc::KERNEL_ROUTINE, kernel_routine);
    k.store32(apc + kapc::RUNDOWN_ROUTINE, rundown);
    k.store32(apc + kapc::NORMAL_ROUTINE, normal_routine);
    k.store16(apc + kapc::TYPE, 18);
    k.host(apc)[kapc::MODE] = normal_routine ? uint8_t(mode) : 0;
    k.store32(apc + kapc::NORMAL_CONTEXT, normal_routine ? normal_context : 0);
    k.host(apc)[kapc::ENQUEUED] = 0;
}

XF_EXPORT(xboxkrnl, KeInsertQueueApc) {
    uint32_t apc = arg(k, c, 0), arg1 = arg(k, c, 1), arg2 = arg(k, c, 2);
    uint32_t thread = k.load32(apc + kapc::THREAD);
    if (!thread) return result(c, 0);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (!k.host(thread)[kthread::MAY_QUEUE_APCS] || k.host(apc)[kapc::ENQUEUED]) return result(c, 0);
    k.store32(apc + kapc::ARG1, arg1);
    k.store32(apc + kapc::ARG2, arg2);
    uint32_t mode = k.host(apc)[kapc::MODE] & 1;
    uint32_t head = thread + kthread::APC_LISTS + 8 * mode;
    if (k.load32(apc + kapc::NORMAL_ROUTINE)) {
        list_insert_tail(k, head, apc + kapc::LIST);
    } else {
        // special APCs (no normal routine) go before the normal ones
        uint32_t at = head;
        for (uint32_t e = k.load32(head); e != head; e = k.load32(e)) {
            at = e;
            if (k.load32(e - kapc::LIST + kapc::NORMAL_ROUTINE)) break;
        }
        if (at == head)
            list_insert_head(k, head, apc + kapc::LIST);
        else
            list_insert_head(k, k.load32(at + 4), apc + kapc::LIST);
    }
    k.host(apc)[kapc::ENQUEUED] = 1;
    // (Xenia leaves the target alone; NT wakes it, as here)
    if (Ref<Dispatcher> d = o.native_locked(thread); d && d->type() == Object::Type::Thread)
        o.apc_queued(static_cast<Thread &>(*d), mode);
    result(c, 1);
}

// (apc): taken off its thread's list if it was queued (Xenia's: under the
// thread's APC lock, the kernel's lock here); whether it was
XF_EXPORT(xboxkrnl, KeRemoveQueueApc) {
    uint32_t apc = arg(k, c, 0);
    if (!k.load32(apc + kapc::THREAD)) return result(c, 0);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (!k.host(apc)[kapc::ENQUEUED]) return result(c, 0);
    k.host(apc)[kapc::ENQUEUED] = 0;
    list_remove(k, apc + kapc::LIST);
    result(c, 1);
}

// (thread handle, routine, context, arg1, arg2): a user APC the kernel
// allocates (its kernel routine the dummy one, freed once delivered) queued
// to the thread (Xenia's xeNtQueueApcThread), waking it if it waits
// alertably
XF_EXPORT(xboxkrnl, NtQueueApcThread) {
    uint32_t routine = arg(k, c, 1), context = arg(k, c, 2), arg1 = arg(k, c, 3), arg2 = arg(k, c, 4);
    Objects &o = objects(k);
    Ref<Thread> t = o.lookup<Thread>(arg(k, c, 0));
    if (!t) return result(c, X_STATUS_INVALID_HANDLE);
    std::lock_guard<std::mutex> l(o.lock);
    if (!k.host(t->kthread())[kthread::MAY_QUEUE_APCS]) return result(c, X_STATUS_UNSUCCESSFUL);
    o.queue_user_apc(*t, routine, context, arg1, arg2);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, KiApcNormalRoutineNop) { result(c, 0); }

// ---- DPCs: run at once on the calling thread, as Xenia does ----

XF_EXPORT(xboxkrnl, KeInitializeDpc) {
    uint32_t dpc = arg(k, c, 0);
    k.store16(dpc + kdpc::TYPE, 19);
    k.host(dpc)[kdpc::SELECTED_CPU] = 0;
    k.host(dpc)[kdpc::DESIRED_CPU] = 0;
    k.store32(dpc + kdpc::ROUTINE, arg(k, c, 1));
    k.store32(dpc + kdpc::CONTEXT, arg(k, c, 2));
}

// The DPCs queued on this processor run when the IRQL falls below DISPATCH_LEVEL (the console's KfLowerIrql sees the
// software interrupt the insert requested, and its dispatch takes them off the PRCB's queue, head first, each at
// DISPATCH_LEVEL with the PCR saying a DPC runs; Xenia runs a DPC as it is inserted, at any IRQL). A DPC taken off is
// not inserted any more and its list entry keeps the links it had (the console unlinks the neighbours only).
void run_queued_dpcs(Kernel &k, ppc::Context &c) {
    uint32_t pcr = uint32_t(c.r[13]), queue = pcr + kpcr::DPC_QUEUE;
    if (k.load32(pcr + kpcr::DPC_ACTIVE) != 0 || !current_thread()) return;
    if (list_empty(k, queue)) {
        k.host(pcr)[kpcr::SOFTWARE_INTERRUPT] = 0;
        return;
    }
    uint8_t irql = k.host(pcr)[kpcr::CURRENT_IRQL];
    k.host(pcr)[kpcr::CURRENT_IRQL] = 2, k.store32(pcr + kpcr::DPC_ACTIVE, 1);
    while (!list_empty(k, queue)) {
        uint32_t entry = k.load32(queue), dpc = entry - kdpc::LIST;
        list_remove(k, entry);
        k.host(dpc)[kdpc::SELECTED_CPU] = 0;
        if (uint32_t routine = k.load32(dpc + kdpc::ROUTINE))
            objects(k).call(
                c, routine, {dpc, k.load32(dpc + kdpc::CONTEXT), k.load32(dpc + kdpc::ARG1), k.load32(dpc + kdpc::ARG2)});
    }
    k.host(pcr)[kpcr::SOFTWARE_INTERRUPT] = 0;
    k.host(pcr)[kpcr::CURRENT_IRQL] = irql, k.store32(pcr + kpcr::DPC_ACTIVE, 0);
}

// (dpc, argument 1, argument 2): 1 if it was queued by this call, 0 if it was queued already (the console's answer:
// the byte at +2 says where it is queued, the processor + 1)
XF_EXPORT(xboxkrnl, KeInsertQueueDpc) {
    uint32_t dpc = arg(k, c, 0), pcr = uint32_t(c.r[13]);
    if (!current_thread()) return result(c, 1); // not on a guest thread: no processor to queue on
    if (k.host(dpc)[kdpc::SELECTED_CPU]) return result(c, 0);
    k.host(dpc)[kdpc::SELECTED_CPU] = uint8_t(k.host(pcr)[kpcr::CURRENT_CPU] + 1);
    k.store32(dpc + kdpc::ARG1, arg(k, c, 1));
    k.store32(dpc + kdpc::ARG2, arg(k, c, 2));
    list_insert_tail(k, pcr + kpcr::DPC_QUEUE, dpc + kdpc::LIST);
    k.host(pcr)[kpcr::SOFTWARE_INTERRUPT] = 2;
    if (k.host(pcr)[kpcr::CURRENT_IRQL] < 2) run_queued_dpcs(k, c);
    result(c, 1);
}

// (dpc): whether it was queued and is no longer (NT's)
XF_EXPORT(xboxkrnl, KeRemoveQueueDpc) {
    uint32_t dpc = arg(k, c, 0);
    if (!k.host(dpc)[kdpc::SELECTED_CPU]) return result(c, 0);
    list_remove(k, dpc + kdpc::LIST);
    k.host(dpc)[kdpc::SELECTED_CPU] = 0;
    result(c, 1);
}

// ---- thread exports ----

XF_EXPORT(xboxkrnl, ExCreateThread) {
    uint32_t handle_ptr = arg(k, c, 0), stack_size = arg(k, c, 1), thread_id_ptr = arg(k, c, 2);
    Objects &o = objects(k);
    Thread::Params p;
    p.xapi_startup = arg(k, c, 3);
    p.start_address = arg(k, c, 4);
    p.start_context = arg(k, c, 5);
    p.flags = arg(k, c, 6);
    p.process = (p.flags & 2) ? o.system_process : o.title_process; // SystemThread
    // the process's default stack, in 4 KB pages, 16 KB at least
    if (!stack_size) stack_size = k.load32(p.process + kprocess::KERNEL_STACK_SIZE);
    p.stack_size = std::max(0x4000u, (stack_size + 0xFFF) & 0xFFFFF000);
    uint32_t status;
    Ref<Thread> t = o.create_thread(p, status);
    if (!t) return result(c, status);
    if (handle_ptr) k.store32(handle_ptr, (p.flags & 0x80) ? t->kthread() : t->handle()); // ReturnKThreadPtr
    if (thread_id_ptr) k.store32(thread_id_ptr, t->id);
    if (!o.scheduler->start(*t)) std::fprintf(stderr, "kernel: no host thread for thread %u\n", t->id);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, ExTerminateThread) { throw ThreadExit{arg(k, c, 0)}; }

namespace {
// one suspension less, the thread going on at none (lock held): the
// previous count
uint32_t resume(Objects &o, Thread &t) {
    uint32_t previous = t.suspend_count;
    if (t.suspend_count && --t.suspend_count == 0) {
        t.pending.fetch_and(~uint32_t(Thread::PENDING_SUSPEND));
        if (t.blocked == Thread::Blocked::Suspended) o.scheduler->wake(t);
    }
    o.k.host(t.kthread())[kthread::SUSPEND_COUNT] = uint8_t(t.suspend_count);
    return previous;
}
} // namespace

XF_EXPORT(xboxkrnl, NtResumeThread) {
    uint32_t handle = arg(k, c, 0), count_ptr = arg(k, c, 1);
    Objects &o = objects(k);
    Ref<Thread> t = o.lookup<Thread>(handle);
    if (!t) return result(c, o.lookup<Object>(handle) ? X_STATUS_OBJECT_TYPE_MISMATCH : X_STATUS_INVALID_HANDLE);
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t previous = resume(o, *t);
    if (count_ptr) k.store32(count_ptr, previous);
    result(c, X_STATUS_SUCCESS);
}

// One suspension more (lock held, released while the calling thread stays
// suspended): the previous count. Another thread stops at its next kernel
// call (Xenia suspends the host thread wherever it is); the calling thread
// stops here.
namespace {
uint32_t suspend(Objects &o, Thread &t, std::unique_lock<std::mutex> &l) {
    uint32_t previous = t.suspend_count++;
    o.k.host(t.kthread())[kthread::SUSPEND_COUNT] = uint8_t(t.suspend_count);
    if (&t == current_thread()) {
        while (t.suspend_count > 0) {
            t.blocked = Thread::Blocked::Suspended;
            o.scheduler->block(t, l, Scheduler::NEVER);
            t.blocked = Thread::Blocked::No;
        }
    } else {
        t.pending.fetch_or(Thread::PENDING_SUSPEND);
    }
    return previous;
}
} // namespace

XF_EXPORT(xboxkrnl, NtSuspendThread) {
    uint32_t handle = arg(k, c, 0), count_ptr = arg(k, c, 1);
    Objects &o = objects(k);
    Ref<Thread> t = o.lookup<Thread>(handle);
    if (!t) return result(c, o.lookup<Object>(handle) ? X_STATUS_OBJECT_TYPE_MISMATCH : X_STATUS_INVALID_HANDLE);
    std::unique_lock<std::mutex> l(o.lock);
    if (t->terminated) return result(c, X_STATUS_THREAD_IS_TERMINATING);
    // (the count is written before a thread suspending itself stops)
    uint32_t previous = t->suspend_count;
    if (count_ptr) k.store32(count_ptr, previous);
    suspend(o, *t, l);
    result(c, X_STATUS_SUCCESS);
}

namespace {
Ref<Thread> thread_at(Kernel &k, uint32_t kthread) {
    Ref<Dispatcher> d = objects(k).native(kthread);
    if (!d || d->type() != Object::Type::Thread) return {};
    return Ref<Thread>(static_cast<Thread *>(d.release()));
}
} // namespace

XF_EXPORT(xboxkrnl, KeSetAffinityThread) {
    uint32_t affinity = arg(k, c, 1), previous_ptr = arg(k, c, 2);
    // no thread is a bad parameter on the console's kernel, as is an empty affinity or one with a processor that is
    // not there (it wants the mask to lie within KeActiveProcessors: the six hardware threads)
    if (!affinity || (affinity & ~0x3Fu) || !arg(k, c, 0)) return result(c, X_STATUS_INVALID_PARAMETER);
    Ref<Thread> t = thread_at(k, arg(k, c, 0));
    if (!t) return result(c, X_STATUS_INVALID_HANDLE);
    std::lock_guard<std::mutex> l(objects(k).lock);
    // the previous affinity through the pointer, the status returned
    if (previous_ptr) k.store32(previous_ptr, 1u << k.host(t->pcr)[kpcr::CURRENT_CPU]);
    set_cpu(k, *t, fake_cpu(objects(k), uint8_t(affinity)));
    result(c, X_STATUS_SUCCESS);
}

// Priorities are recorded, not given to the host (Xenia ignores them by
// default too). The base priority's increment over the process's, as NT's
// KeSetBasePriorityThread takes and returns it (Xenia returns the host
// thread's priority, 0 when ignored).
XF_EXPORT(xboxkrnl, KeQueryBasePriorityThread) {
    Ref<Thread> t = thread_at(k, arg(k, c, 0));
    std::lock_guard<std::mutex> l(objects(k).lock);
    result(c, t ? uint32_t(t->base_priority_increment) : 0);
}

XF_EXPORT(xboxkrnl, KeSetBasePriorityThread) {
    Ref<Thread> t = thread_at(k, arg(k, c, 0));
    int32_t increment = int32_t(arg(k, c, 1));
    if (!t) return result(c, 0);
    std::lock_guard<std::mutex> l(objects(k).lock);
    int32_t previous = t->base_priority_increment;
    t->base_priority_increment = increment;
    uint32_t process = k.load32(t->kthread() + kthread::PROCESS);
    int32_t priority = std::clamp(int32_t(k.host(process)[kprocess::DEFAULT_PRIORITY]) + increment, 0, 31);
    k.host(t->kthread())[kthread::BASE_PRIORITY] = uint8_t(priority);
    k.host(t->kthread())[kthread::PRIORITY] = uint8_t(priority);
    result(c, uint32_t(previous));
}

// By the KTHREAD: NT's answer, the previous suspend count (Xenia returns
// its status, success).
XF_EXPORT(xboxkrnl, KeResumeThread) {
    Ref<Thread> t = thread_at(k, arg(k, c, 0));
    if (!t) return result(c, X_STATUS_INVALID_HANDLE);
    std::lock_guard<std::mutex> l(objects(k).lock);
    result(c, resume(objects(k), *t));
}

// (thread): the previous suspend count, the thread suspended (Xenia's: 0
// when the KTHREAD has no thread behind it)
XF_EXPORT(xboxkrnl, KeSuspendThread) {
    Ref<Thread> t = thread_at(k, arg(k, c, 0));
    if (!t) return result(c, 0);
    Objects &o = objects(k);
    std::unique_lock<std::mutex> l(o.lock);
    // (the result is the count before a thread suspending itself stops)
    uint32_t previous = t->suspend_count;
    suspend(o, *t, l);
    result(c, previous);
}

// (thread, priority): the previous priority; the new one in the KTHREAD,
// not given to the host (as KeSetBasePriorityThread here; Xenia ignores
// priorities by default)
XF_EXPORT(xboxkrnl, KeSetPriorityThread) {
    uint32_t thread = arg(k, c, 0);
    int32_t priority = int32_t(arg(k, c, 1));
    if (!thread) return result(c, 0);
    Ref<Thread> t = thread_at(k, thread);
    std::lock_guard<std::mutex> l(objects(k).lock);
    uint8_t previous = k.host(thread)[kthread::PRIORITY];
    if (t) k.host(thread)[kthread::PRIORITY] = uint8_t(std::max(priority, 0));
    result(c, previous);
}

// (thread, disabled): the previous flag (Xenia's: an exchange of the byte)
XF_EXPORT(xboxkrnl, KeSetDisableBoostThread) {
    std::atomic_ref<uint8_t> flag(k.host(arg(k, c, 0))[kthread::BOOST_DISABLED]);
    result(c, flag.exchange(uint8_t(arg(k, c, 1))));
}

// (stack pointer, thread, allocation base, base, limit): a thread moving to
// another stack, a fiber's (Xenia's KeSetCurrentStackPointers). r1 changes
// under the caller. Where Xenia re-enters the thread at LR, dropping its host
// frames, the calling thread here moves to the new stack's host fiber
// (stacks.cpp): this call waits there until a switch comes back to this
// stack.
XF_EXPORT(xboxkrnl, KeSetCurrentStackPointers) {
    uint32_t stack_ptr = arg(k, c, 0), thread = arg(k, c, 1);
    uint32_t base = arg(k, c, 3), limit = arg(k, c, 4), pcr = uint32_t(c.r[13]);
    k.store32(thread + kthread::STACK_ALLOC_BASE, arg(k, c, 2));
    k.store32(thread + kthread::STACK_BASE, base);
    k.store32(thread + kthread::STACK_LIMIT, limit);
    k.store32(pcr + kpcr::STACK_BASE, base);
    k.store32(pcr + kpcr::STACK_END, limit);
    uint32_t from_sp = uint32_t(c.r[1]);
    c.r[1] = stack_ptr;
    if (Thread *t = current_thread(); t && t->kthread() == thread && t->context.get() == &c)
        switch_guest_stack(k, *t, c, from_sp, base, limit);
}

// The FPU registers kept in the current KTHREAD (Xenia's, sketchy as its). The console's save answers the previous
// owner of the FPU state (-1 when there is none; its save area is the control block's own, at +0x300, from then on)
// and its restore gives the state back (-1); the answer is the console's, the registers' keeping is ours.
XF_EXPORT(xboxkrnl, KeSaveFloatingPointState) {
    uint32_t pcr = uint32_t(c.r[13]), thread = k.load32(pcr + kpcr::CURRENT_THREAD);
    for (uint32_t n = 0; n < 32; n++) k.store64(thread + kthread::FPU_CONTEXT + 8 * n, std::bit_cast<uint64_t>(c.f[n]));
    uint32_t owner = k.load32(pcr + kpcr::FPU_OWNER);
    k.store32(pcr + kpcr::FPU_OWNER, pcr + 0x300);
    result(c, owner);
}
XF_EXPORT(xboxkrnl, KeRestoreFloatingPointState) {
    uint32_t pcr = uint32_t(c.r[13]), thread = k.load32(pcr + kpcr::CURRENT_THREAD);
    for (uint32_t n = 0; n < 32; n++) c.f[n] = std::bit_cast<double>(k.load64(thread + kthread::FPU_CONTEXT + 8 * n));
    k.store32(pcr + kpcr::FPU_OWNER, 0xFFFFFFFF);
}

// (thread id, out): the thread's KTHREAD, referenced until
// ObDereferenceObject (Xenia's ObLookupThreadByThreadId; its
// ObLookupAnyThreadByThreadId is the same)
namespace {
void lookup_thread_by_id(Kernel &k, ppc::Context &c) {
    uint32_t id = arg(k, c, 0), out_ptr = arg(k, c, 1);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    for (Thread *t : o.threads) {
        if (t->id != id) continue;
        o.retain_handle(t->handle());
        if (out_ptr) k.store32(out_ptr, t->kthread());
        return result(c, X_STATUS_SUCCESS);
    }
    if (out_ptr) k.store32(out_ptr, 0);
    result(c, X_STATUS_INVALID_PARAMETER);
}
} // namespace

XF_EXPORT(xboxkrnl, ObLookupThreadByThreadId) { lookup_thread_by_id(k, c); }
XF_EXPORT(xboxkrnl, ObLookupAnyThreadByThreadId) { lookup_thread_by_id(k, c); }

XF_EXPORT(xboxkrnl, KeGetCurrentProcessType) {
    uint32_t pcr = uint32_t(c.r[13]);
    if (!k.load32(pcr + kpcr::DPC_ACTIVE))
        return result(c, k.host(k.load32(pcr + kpcr::CURRENT_THREAD))[kthread::PROCESS_TYPE]);
    result(c, k.host(pcr)[kpcr::PROCESS_TYPE_IN_DPC]);
}

XF_EXPORT(xboxkrnl, KeSetCurrentProcessType) {
    uint32_t pcr = uint32_t(c.r[13]);
    if (k.load32(pcr + kpcr::DPC_ACTIVE)) k.host(pcr)[kpcr::PROCESS_TYPE_IN_DPC] = uint8_t(arg(k, c, 0));
}

XF_EXPORT(xboxkrnl, KeEnableFpuExceptions) {
    k.host(k.load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD))[kthread::FPU_EXCEPTIONS] = arg(k, c, 0) != 0;
}

// What giving up the time slice answers. The console's NtYieldExecution (and the zero-interval delay, which is one)
// is STATUS_NO_YIELD_PERFORMED (0x40000024) when no other thread is ready on the processor, success when one ran
// (the oracle: with only the caller running it answers 0x40000024; Xenia answers 0 always). Ready here: another
// guest thread that has started, has not finished, and is not blocked.
static uint32_t yield_status(Objects &o, Thread *self) {
    std::lock_guard<std::mutex> l(o.lock);
    for (Thread *t : o.threads)
        if (t != self && t->started && !t->finished && !t->terminated && t->blocked == Thread::Blocked::No)
            return X_STATUS_SUCCESS;
    return X_STATUS_NO_YIELD_PERFORMED;
}

// (mode, alertable, interval *): a null interval is not a zero wait. The
// console's kernel reads it through the pointer (the oracle: a fault reading
// guest address 0), which a title sees as an access violation; there is no
// exception delivery here yet, so the call fails with that status and does
// not wait. Xenia treats it as zero, with a TODO asking whether it should.
XF_EXPORT(xboxkrnl, KeDelayExecutionThread) {
    uint32_t alertable = arg(k, c, 1), interval_ptr = arg(k, c, 2);
    Objects &o = objects(k);
    Thread *t = current_thread();
    if (!interval_ptr) {
        static std::once_flag warned;
        std::call_once(warned, [&] {
            std::fprintf(stderr,
                         "kernel: KeDelayExecutionThread with a null interval from 0x%08X: an access violation\n",
                         uint32_t(c.lr));
        });
        return result(c, 0xC0000005 /* STATUS_ACCESS_VIOLATION */);
    }
    int64_t interval = int64_t(k.load64(interval_ptr));
    if (!t) return result(c, X_STATUS_SUCCESS);
    if (!interval && !(alertable && o.user_apc_pending(*t))) {
        o.scheduler->yield();
        return result(c, yield_status(o, t));
    }
    uint32_t status = o.wait(*t, {}, false, alertable != 0, o.deadline(interval_ptr));
    if (status == X_STATUS_USER_APC) {
        o.deliver_apcs(*t, 1);
        return result(c, X_STATUS_USER_APC);
    }
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtYieldExecution) {
    Objects &o = objects(k);
    o.scheduler->yield();
    result(c, yield_status(o, current_thread()));
}

// ---- TLS slots (Xenia's AllocateTLS, FreeTLS, XThread's TLS values) ----

XF_EXPORT(xboxkrnl, KeTlsAlloc) {
    Objects &o = objects(k);
    Thread *t = current_thread();
    uint32_t slot = 0xFFFFFFFF;
    {
        std::lock_guard<std::mutex> l(o.lock);
        uint32_t bitmap = (t ? k.load32(t->kthread() + kthread::PROCESS) : o.title_process) + kprocess::TLS_BITMAP;
        for (uint32_t w = 0; w < 8 && slot == 0xFFFFFFFF; w++) {
            uint32_t bits = k.load32(bitmap + 4 * w);
            if (!bits) continue;
            uint32_t zeros = uint32_t(__builtin_clz(bits));
            slot = w * 32 + zeros;
            k.store32(bitmap + 4 * w, bits & ~(1u << (31 - zeros)));
        }
    }
    if (t && slot != 0xFFFFFFFF && slot * 4 < t->tls_slots_size) k.store32(t->tls_dynamic + slot * 4, 0);
    result(c, slot);
}

XF_EXPORT(xboxkrnl, KeTlsFree) {
    uint32_t slot = arg(k, c, 0);
    if (slot == 0xFFFFFFFF) return result(c, 0);
    Objects &o = objects(k);
    Thread *self = current_thread();
    if (slot >= 256) return result(c, 1);
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t process = self ? k.load32(self->kthread() + kthread::PROCESS) : o.title_process;
    uint32_t word = process + kprocess::TLS_BITMAP + 4 * (slot / 32), bit = 1u << (31 - slot % 32);
    uint32_t bits = k.load32(word);
    if (!(bits & bit)) {
        // the slot's value goes in every thread of the process
        for (Thread *t : o.threads)
            if (k.load32(t->kthread() + kthread::PROCESS) == process && slot * 4 < t->tls_slots_size)
                k.store32(t->tls_dynamic + slot * 4, 0);
        k.store32(word, bits | bit);
    }
    result(c, 1);
}

// The kernel's own have no error branch; out of the block, 0.
XF_EXPORT(xboxkrnl, KeTlsGetValue) {
    Thread *t = current_thread();
    uint32_t slot = arg(k, c, 0);
    result(c, t && slot < t->tls_slots_size / 4 ? k.load32(t->tls_dynamic + slot * 4) : 0);
}

XF_EXPORT(xboxkrnl, KeTlsSetValue) {
    Thread *t = current_thread();
    uint32_t slot = arg(k, c, 0);
    if (!t || slot >= t->tls_slots_size / 4) return result(c, 0);
    k.store32(t->tls_dynamic + slot * 4, arg(k, c, 1));
    result(c, 1);
}

// ---- critical regions, IRQL, spin locks ----

XF_EXPORT(xboxkrnl, KeEnterCriticalRegion) {
    uint32_t t = k.load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD);
    k.store32(t + kthread::APC_DISABLE_COUNT, k.load32(t + kthread::APC_DISABLE_COUNT) - 1);
}

XF_EXPORT(xboxkrnl, KeLeaveCriticalRegion) {
    uint32_t t = k.load32(uint32_t(c.r[13]) + kpcr::CURRENT_THREAD);
    k.store32(t + kthread::APC_DISABLE_COUNT, k.load32(t + kthread::APC_DISABLE_COUNT) + 1);
}

XF_EXPORT(xboxkrnl, KfAcquireSpinLock) {
    uint8_t old = raise_irql(k, uint32_t(c.r[13]), 2);
    acquire_spin_lock(objects(k), c, arg(k, c, 0));
    result(c, old);
}

XF_EXPORT(xboxkrnl, KfReleaseSpinLock) {
    uint32_t old_irql = arg(k, c, 1);
    release_spin_lock(objects(k), arg(k, c, 0));
    if (old_irql < 2) {
        k.host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] = uint8_t(old_irql);
        run_queued_dpcs(k, c);
    }
}

// IRQL in the PCR (Xenia's): to dispatch level, the old one back; down
XF_EXPORT(xboxkrnl, KeRaiseIrqlToDpcLevel) { result(c, raise_irql(k, uint32_t(c.r[13]), 2)); }
XF_EXPORT(xboxkrnl, KfLowerIrql) {
    k.host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] = uint8_t(arg(k, c, 0));
    if (arg(k, c, 0) < 2) run_queued_dpcs(k, c);
}
// (new IRQL): the old one
XF_EXPORT(xboxkrnl, KfRaiseIrql) { result(c, raise_irql(k, uint32_t(c.r[13]), uint8_t(arg(k, c, 0)))); }

// one try: 1 if the lock was free and is now the caller's
XF_EXPORT(xboxkrnl, KeTryToAcquireSpinLockAtRaisedIrql) {
    uint32_t expected = 0;
    bool taken = guest_word(k, arg(k, c, 0))
                     .compare_exchange_strong(expected, bswap32(uint32_t(c.r[13])), std::memory_order_acquire);
    result(c, taken ? 1 : 0);
}

XF_EXPORT(xboxkrnl, KeAcquireSpinLockAtRaisedIrql) { acquire_spin_lock(objects(k), c, arg(k, c, 0)); }

XF_EXPORT(xboxkrnl, KeReleaseSpinLockFromRaisedIrql) { release_spin_lock(objects(k), arg(k, c, 0)); }

// ---- SLists (X_SLIST_HEADER: next, depth, sequence; one 64-bit word) ----

namespace {
struct SListHeader {
    uint32_t next;
    uint16_t depth, sequence;
};
SListHeader unpack(uint64_t raw) {
    uint8_t b[8];
    std::memcpy(b, &raw, 8);
    return {uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3], uint16_t(b[4] << 8 | b[5]),
            uint16_t(b[6] << 8 | b[7])};
}
uint64_t pack(SListHeader h) {
    uint8_t b[8] = {uint8_t(h.next >> 24), uint8_t(h.next >> 16), uint8_t(h.next >> 8),     uint8_t(h.next),
                    uint8_t(h.depth >> 8), uint8_t(h.depth),      uint8_t(h.sequence >> 8), uint8_t(h.sequence)};
    uint64_t raw;
    std::memcpy(&raw, b, 8);
    return raw;
}
std::atomic_ref<uint64_t> slist(Kernel &k, uint32_t address) {
    return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t *>(k.host(address)));
}
} // namespace

XF_EXPORT(xboxkrnl, InterlockedPopEntrySList) {
    auto word = slist(k, arg(k, c, 0));
    uint64_t old = word.load();
    for (;;) {
        SListHeader h = unpack(old);
        if (!h.next) return result(c, 0);
        SListHeader next{k.load32(h.next), uint16_t(h.depth - 1), h.sequence};
        if (word.compare_exchange_weak(old, pack(next))) return result(c, h.next);
    }
}

// (list, entry): the entry on top, the depth and sequence one more; the old
// top
XF_EXPORT(xboxkrnl, InterlockedPushEntrySList) {
    uint32_t entry = arg(k, c, 1);
    auto word = slist(k, arg(k, c, 0));
    uint64_t old = word.load();
    for (;;) {
        SListHeader h = unpack(old);
        k.store32(entry, h.next);
        SListHeader top{entry, uint16_t(h.depth + 1), uint16_t(h.sequence + 1)};
        if (word.compare_exchange_weak(old, pack(top))) return result(c, h.next);
    }
}

XF_EXPORT(xboxkrnl, InterlockedFlushSList) {
    uint64_t old = slist(k, arg(k, c, 0)).exchange(0);
    result(c, unpack(old).next);
}

// ---- time ----

XF_EXPORT(xboxkrnl, KeQuerySystemTime) {
    uint32_t time_ptr = arg(k, c, 0);
    if (!time_ptr) return;
    Objects &o = objects(k);
    uint64_t time = o.system_time();
    // KeTimeStampBundle moves with it, as in Xenia
    k.store64(o.time_stamp_bundle + time_stamp_bundle::SYSTEM_TIME, time);
    k.store64(time_ptr, time);
}

// the time base's: 50 MHz
XF_EXPORT(xboxkrnl, KeQueryPerformanceFrequency) { result(c, 50'000'000); }

// ---- the stack's limits (xboxkrnl_rtl.cc) ----

// (low limit out, high limit out): the PCR's stack, or its alternative one
// when it says that is in use
XF_EXPORT(xboxkrnl, RtlGetStackLimits) {
    uint32_t end_ptr = arg(k, c, 0), base_ptr = arg(k, c, 1), pcr = uint32_t(c.r[13]);
    bool alternative = k.load32(pcr + kpcr::USE_ALTERNATIVE_STACK) != 0;
    k.store32(base_ptr, k.load32(pcr + (alternative ? kpcr::ALT_STACK_BASE : kpcr::STACK_BASE)));
    k.store32(end_ptr, k.load32(pcr + (alternative ? kpcr::ALT_STACK_END : kpcr::STACK_END)));
}

} // namespace xf::kernel
