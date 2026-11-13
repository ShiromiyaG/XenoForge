// The console scheduler (docs/runtime.md, "The console scheduler";
// XF_SCHEDULER=console): guest threads scheduled as the console's kernel
// schedules them, on the Xenon's six hardware threads, rather than each a
// host thread handed to the host's scheduler. Written anew (Xenia runs each
// guest thread as a host thread); the model is Cemu's, which runs the Wii
// U's threads as fibers on a host thread per emulated core.
//
// - Six processors, each a host thread with a fiber per guest thread on it:
//   the processor a thread is made on (KPCR CURRENT_CPU, which ExCreateThread's
//   processor mask and the parent choose, as Xenia's GetFakeCpuNumber), or
//   the one KeSetAffinityThread names before it first runs (the XDK's
//   CreateThread suspended, XSetThreadProcessor, ResumeThread). A thread that
//   has run stays on its processor's host thread: the kernel's code reads
//   host thread-locals, whose addresses the compiler may keep across a call
//   that would move it (the moves asked then are counted). The thread-locals
//   the kernel keeps per guest thread are swapped at each switch, as in the
//   deterministic mode.
// - A wait that blocks, a yield, a thread's end: the processor switches to its
//   next ready fiber in user mode (no host wait, no host wake); with nothing
//   ready it sleeps until a wait's deadline or a wake from elsewhere.
// - Each processor runs its ready fibers by their KTHREAD priority (first
//   come first among equals). One made ready above the one running preempts
//   it at its next yield point (a loop's back edge, a priority hint: the
//   mechanism Kernel::pause uses); a fiber that has run QUANTUM with another
//   as high ready is preempted there too, as the console's kernel ends a
//   quantum. One that has waited STARVING runs next whatever its priority
//   (Xenia gives the host every thread, priorities ignored: no game is left
//   waiting on a low thread a high one starves).
// - The XDK's spin-wait hint (db16cyc; ppc_ops.h's spin_hint), a yield and a
//   slice given up hand the processor to the others ready on it, whatever
//   their priority (a spin waits for one of them), behind them.
// - The kernel's own threads whose body is host code (the GPU's, the sound's,
//   XAM's workers) stay host threads of their own, as the host scheduler
//   runs them: they wait on host objects.
// - The clock is the host's (as the host scheduler's), and a clock thread
//   moves the timers and KeTimeStampBundle every millisecond, ends the waits
//   of busy processors whose deadline passed and asks for the preemptions.

#include "host_clock.h"
#include "objects.h"
#include "stacks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace xf::kernel {
namespace {

constexpr uint32_t PROCESSORS = 6;    // the Xenon's hardware threads
constexpr uint64_t QUANTUM = 20'000;  // 2 ms, in the clock's 100 ns
constexpr uint64_t STARVING = 80'000; // 8 ms ready and not run

class ConsoleScheduler;

// a guest thread on a processor (Thread::scheduled)
struct Entry {
    ConsoleScheduler *owner = nullptr;
    Thread *thread = nullptr; // nullptr once its body returned
    void *fiber = nullptr;    // made for it, deleted once it ended
    void *resume = nullptr;   // where it goes on: its fiber, or a guest stack's (stacks.h)
    void *walks = nullptr;    // its exception dispatches under way (exceptions.cpp)
    enum class State : uint8_t { Ready, Running, Blocked, Done } state = State::Ready;
    uint64_t deadline = Scheduler::NEVER;
    uint32_t processor = 0, wanted = 0; // where it is, where KeSetAffinityThread wants it
    bool ran = false;                   // picked once: it stays on its processor from then on
    bool held = false;                  // made suspended: not run until resumed (it may still move)
    bool yielding = false;              // handed over to the others: behind them
    uint8_t priority = 0;               // the KTHREAD's, as it was made ready
    uint64_t ready_since = 0;
    uint64_t returned = 0, polling_since = 0; // give_up_slice's polling, per thread
};

struct Processor {
    uint32_t index = 0;
    std::thread host;
    void *runner = nullptr; // the host thread as a fiber, which picks the next one
    // under the kernel's lock
    std::deque<Entry *> ready;
    std::vector<Entry *> entries; // the threads on it
    Entry *running = nullptr;
    uint8_t running_priority = 0;
    bool idle = false, started = false;
    std::condition_variable cv; // a fiber of it made ready, a deadline, the end
    // read without the lock
    std::atomic<uint32_t> ready_count{0};
    std::atomic<uint64_t> running_since{0};
    std::atomic<bool> preempt{false};
    void *host_handle = nullptr; // for its CPU time in the report (Windows)
#ifndef _WIN32
    clockid_t host_clock{}; // the same on POSIX: its thread's CPU clock
    bool host_clock_set = false;
#endif
};

thread_local Processor *on_processor = nullptr;

void fiber_main(void *arg);

class ConsoleScheduler final : public Scheduler {
public:
    explicit ConsoleScheduler(Objects &o) : o_(o), epoch_(host_clock::now()) {
        for (uint32_t i = 0; i < PROCESSORS; i++) processors_[i].index = i;
    }

    ~ConsoleScheduler() override {
        {
            std::lock_guard<std::mutex> l(o_.lock);
            stopping_ = true;
        }
        clock_cv_.notify_all();
        if (clock_.joinable()) clock_.join();
        for (Processor &p : processors_) {
            p.cv.notify_all();
            // a processor still in a guest thread is left be, as the host
            // scheduler leaves its threads
            bool idle;
            {
                std::lock_guard<std::mutex> l(o_.lock);
                idle = !p.running;
            }
            if (p.host.joinable()) {
                if (idle) p.host.join();
                else p.host.detach();
            }
#ifdef _WIN32
            if (p.host_handle) CloseHandle(p.host_handle);
#endif
        }
    }

    bool start(Thread &t) override {
        start_clock();
        if (t.params.host) return start_host(t);
        std::lock_guard<std::mutex> l(o_.lock);
        auto e = std::make_unique<Entry>();
        e->owner = this, e->thread = &t;
        e->fiber = e->resume = create_fiber(fiber_main, e.get());
        if (!e->fiber) return false;
        e->processor = e->wanted = cpu_of(t);
        t.scheduled = e.get();
        processors_[e->processor].entries.push_back(e.get());
        Entry *raw = e.get();
        entries_.push_back(std::move(e));
        made_++;
        if (t.suspend_count > 0) {
            // as if it waited in its body's suspension (Thread::body), which
            // the resume ends; until then it has not run, and moves where
            // KeSetAffinityThread says
            raw->state = Entry::State::Blocked;
            raw->held = true;
            t.set_blocked(Thread::Blocked::Suspended);
            return true;
        }
        make_ready(*raw);
        return true;
    }

    void join(Thread &t) override {
        if (running_entry()) {
            // a guest thread waits for another: it gives up its processor until that one ends
            for (;;) {
                {
                    std::lock_guard<std::mutex> l(o_.lock);
                    if (t.finished) return;
                }
                give_up_slice();
            }
        }
        std::unique_lock<std::mutex> l(o_.lock);
        o_.thread_exited.wait(l, [&] { return t.finished; });
    }

    bool block(Thread &self, std::unique_lock<std::mutex> &lock, uint64_t deadline) override {
        Entry *e = running_entry();
        self.woken = false;
        if (!e || e->thread != &self) {
            // a host thread: its own condition variable, as the host scheduler's
            while (!self.woken) {
                if (deadline == NEVER)
                    self.cv.wait(lock);
                else if (self.cv.wait_until(lock, time_point(deadline)) == std::cv_status::timeout)
                    return self.woken;
            }
            return true;
        }
        e->state = Entry::State::Blocked;
        e->deadline = deadline;
        blocks_++;
        lock.unlock();
        hand_over(*e);
        lock.lock();
        return self.woken;
    }

    void wake(Thread &t) override {
        t.woken = true;
        Entry *e = static_cast<Entry *>(t.scheduled);
        if (!e) return t.cv.notify_one();
        if (e->state != Entry::State::Blocked) return;
        if (e->held) {
            e->held = false;
            t.set_blocked(Thread::Blocked::No); // (its body waits again if it is still suspended)
        }
        e->state = Entry::State::Ready;
        e->deadline = NEVER;
        make_ready(*e);
    }

    // another ready fiber of this processor runs; with none, the host's yield
    void yield() override {
        Entry *e = running_entry();
        if (!e) return std::this_thread::yield();
        if (!on_processor->ready_count.load(std::memory_order_relaxed)) return std::this_thread::yield();
        yields_++;
        hand_over(*e, true);
    }

    // the spin-wait hint's: to a ready fiber of this processor, if any
    void spin() override {
        Entry *e = running_entry();
        if (!e) return std::this_thread::yield();
        if (!on_processor->ready_count.load(std::memory_order_relaxed)) return;
        spins_++;
        hand_over(*e, true);
    }

    // A spin that waits for the GPU: to a ready fiber of this processor, if
    // any; else the processor sleeps on its condition variable, which a
    // fiber made ready there and spin_wake() end.
    void spin_wait(const std::atomic<uint32_t> &word, uint32_t seen, uint32_t us) override {
        Entry *e = running_entry();
        if (!e) return std::this_thread::yield();
        Processor &p = *on_processor;
        if (!p.ready_count.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> l(o_.lock);
            p.idle = true;
            spin_sleepers_.fetch_add(1);
            p.cv.wait_for(l, std::chrono::microseconds(us),
                          [&] { return !p.ready.empty() || stopping_ || word.load() != seen; });
            spin_sleepers_.fetch_sub(1);
            p.idle = false;
            if (p.ready.empty()) return;
        }
        spins_++;
        hand_over(*e, true);
    }
    void spin_wake() override {
        if (!spin_sleepers_.load()) return;
        { std::lock_guard<std::mutex> l(o_.lock); }
        for (Processor &p : processors_) p.cv.notify_all();
    }

    // A zero-interval delay or NtYieldExecution: a ready fiber of the
    // processor runs, if any. A thread that has done nothing but yield (at most
    // SPIN_GAP of guest work between the yields) for SPIN_RUN is polling: its
    // yields then nap, as the host scheduler's do, on the processor's
    // condition variable, which a fiber made ready there ends at once.
    void give_up_slice() override {
        Entry *e = running_entry();
        if (!e) return give_up_host_slice();
        Processor &p = *on_processor;
        if (p.ready_count.load(std::memory_order_relaxed)) {
            yields_++;
            hand_over(*e, true);
            e->returned = now();
            return;
        }
        uint64_t t = now();
        if (t - e->returned > SPIN_GAP) e->polling_since = t;
        if (t - e->polling_since >= SPIN_RUN) {
            naps_++;
            std::unique_lock<std::mutex> l(o_.lock);
            p.idle = true;
            p.cv.wait_for(l, std::chrono::microseconds(500), [&] { return !p.ready.empty() || stopping_; });
            p.idle = false;
            bool other = !p.ready.empty();
            l.unlock();
            if (other) hand_over(*e, true);
        } else {
            std::this_thread::yield();
        }
        e->returned = now();
    }

    void preempt_point() override {
        Entry *e = running_entry();
        if (!e) return;
        Processor &p = *on_processor;
        if (!p.preempt.exchange(false, std::memory_order_relaxed)) return;
        if (!p.ready_count.load(std::memory_order_relaxed)) return;
        preemptions_++;
        hand_over(*e);
    }

    void affinity_changed(Thread &t) override { // (lock held)
        Entry *e = static_cast<Entry *>(t.scheduled);
        if (!e || e->state == Entry::State::Done) return;
        e->wanted = cpu_of(t);
        if (e->wanted == e->processor) return;
        if (e->ran) {
            // (see the top: it stays)
            moves_refused_++;
            e->wanted = e->processor;
            return;
        }
        moves_++;
        switch (e->state) {
        case Entry::State::Ready: {
            Processor &from = processors_[e->processor];
            auto it = std::find(from.ready.begin(), from.ready.end(), e);
            if (it != from.ready.end()) {
                from.ready.erase(it);
                from.ready_count.fetch_sub(1, std::memory_order_relaxed);
            }
            move(*e);
            make_ready(*e);
            break;
        }
        case Entry::State::Blocked: move(*e); break;
        default: break;
        }
    }

    uint64_t now() override {
        uint64_t t = host_clock::now();
        return t > epoch_ ? t - epoch_ : 0;
    }

    void timers_changed() override { clock_cv_.notify_one(); }

    std::string report() override { // (the kernel's lock held)
        char line[512];
        std::snprintf(line, sizeof(line),
                      "console scheduler: %llu threads on %u processors, %llu switches: %llu waits blocked (%llu "
                      "ended by their deadline), %llu yields, %llu spins handed over, %llu preemptions (%llu asked by a higher one made ready), %llu "
                      "run starving, %llu naps; %llu moves before a thread ran, %llu asked after (not made)\n",
                      (unsigned long long)made_, PROCESSORS, (unsigned long long)switches_,
                      (unsigned long long)blocks_.load(), (unsigned long long)timeouts_,
                      (unsigned long long)yields_.load(), (unsigned long long)spins_.load(),
                      (unsigned long long)preemptions_.load(), (unsigned long long)raised_,
                      (unsigned long long)starved_, (unsigned long long)naps_.load(),
                      (unsigned long long)moves_, (unsigned long long)moves_refused_);
        std::string out = line;
        out += "console scheduler: host CPU by processor:";
        for (Processor &p : processors_) {
            double cpu = -1;
#ifdef _WIN32
            FILETIME created, exited, kernel, user;
            if (p.host_handle && GetThreadTimes(p.host_handle, &created, &exited, &kernel, &user)) {
                auto value = [](FILETIME f) { return double(uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 1e-7; };
                cpu = value(kernel) + value(user);
            }
#else
            timespec t{};
            if (p.host_clock_set && clock_gettime(p.host_clock, &t) == 0)
                cpu = double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
#endif
            std::snprintf(line, sizeof(line), " %u: %zu threads, %.1f s;", p.index, p.entries.size(), cpu);
            out += line;
        }
        out += "\n";
        return out;
    }

private:
    friend void fiber_main(void *arg);

    // the processor KPCR CURRENT_CPU names (set by Thread::create, KeSetAffinityThread)
    uint32_t cpu_of(Thread &t) { return uint32_t(o_.k.host(t.pcr)[kpcr::CURRENT_CPU]) % PROCESSORS; }

    // the calling fiber's record, when it is one running on a processor
    Entry *running_entry() {
        Processor *p = on_processor;
        if (!p || !p->running) return nullptr;
        Thread *t = current_thread();
        return t && p->running->thread == t ? p->running : nullptr;
    }

    // the fiber gives its host thread back to its processor's runner; it goes
    // on here when the runner picks it again
    void hand_over(Entry &e, bool yielding = false) {
        e.yielding = yielding;
        e.resume = current_fiber();
        e.walks = exchange_exception_walks(nullptr);
        switch_fiber(on_processor->runner);
    }

    // onto the processor KeSetAffinityThread chose (lock held; not running)
    void move(Entry &e) {
        Processor &from = processors_[e.processor];
        from.entries.erase(std::remove(from.entries.begin(), from.entries.end(), &e), from.entries.end());
        e.processor = e.wanted;
        processors_[e.processor].entries.push_back(&e);
        if (from.idle) from.cv.notify_one(); // (its deadlines changed)
    }

    uint8_t priority_of(Thread &t) { return o_.k.host(t.kthread())[kthread::PRIORITY]; }

    // to its processor's ready queue, behind those as high (or behind all: a
    // yield), the processor told, started or asked to hand over (lock held)
    void make_ready(Entry &e, bool behind_all = false) {
        if (e.wanted != e.processor) move(e);
        Processor &p = processors_[e.processor];
        e.priority = e.thread ? priority_of(*e.thread) : 0;
        e.ready_since = now();
        auto at = p.ready.end();
        if (!behind_all)
            while (at != p.ready.begin() && (*(at - 1))->priority < e.priority) --at;
        p.ready.insert(at, &e);
        p.ready_count.fetch_add(1, std::memory_order_relaxed);
        if (!p.started) {
            p.started = true;
            p.host = std::thread([this, &p] { run(p); });
        } else if (p.idle) {
            p.cv.notify_one();
        } else if (p.running && !behind_all && e.priority > p.running_priority) {
            raised_++;
            request_preempt(p);
        }
    }

    // a ready one of p that has waited STARVING, if any
    std::deque<Entry *>::iterator starving(Processor &p, uint64_t t) {
        for (auto it = p.ready.begin(); it != p.ready.end(); ++it)
            if (t - (*it)->ready_since >= STARVING) return it;
        return p.ready.end();
    }

    // the processor's running fiber handed over at its next yield point (as
    // Kernel::pause stops threads: written as it runs; a store it misses is
    // made again at the next tick)
    void request_preempt(Processor &p) {
        Entry *e = p.running;
        if (!e || !e->thread || !e->thread->context) return;
        p.preempt.store(true, std::memory_order_relaxed);
        ppc::Context &c = *e->thread->context;
        __atomic_store_n(&c.yield_budget, int64_t(-1), __ATOMIC_RELAXED);
        __atomic_store_n(&c.yield_counting, true, __ATOMIC_RELEASE);
    }

    void run(Processor &p) {
        on_processor = &p;
        bool converted;
        p.runner = convert_thread(converted);
        if (!p.runner) {
            std::fprintf(stderr, "console scheduler: processor %u could not be made a fiber\n", p.index);
            return;
        }
#ifdef _WIN32
        {
            wchar_t name[32];
            std::swprintf(name, 32, L"xf processor %u", p.index);
            SetThreadDescription(GetCurrentThread(), name);
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                            THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
            p.host_handle = self;
        }
#else
        {
            char name[16];
            std::snprintf(name, sizeof(name), "xf proc %u", p.index);
#if defined(__linux__) || defined(__ANDROID__)
            pthread_setname_np(pthread_self(), name);
#endif
            p.host_clock_set = pthread_getcpuclockid(pthread_self(), &p.host_clock) == 0;
        }
#endif
        std::unique_lock<std::mutex> l(o_.lock);
        while (!stopping_) {
            // the waits whose deadline passed, in the threads' order
            uint64_t t = now(), next = NEVER;
            for (Entry *e : p.entries) {
                if (e->state != Entry::State::Blocked || e->deadline == NEVER) continue;
                if (e->deadline <= t) {
                    e->state = Entry::State::Ready;
                    e->deadline = NEVER;
                    p.ready.push_back(e);
                    p.ready_count.fetch_add(1, std::memory_order_relaxed);
                    timeouts_++;
                } else {
                    next = std::min(next, e->deadline);
                }
            }
            if (p.ready.empty()) {
                p.idle = true;
                if (next == NEVER)
                    p.cv.wait(l);
                else
                    p.cv.wait_until(l, time_point(next));
                p.idle = false;
                continue;
            }
            auto pick = starving(p, t);
            if (pick == p.ready.end()) pick = p.ready.begin();
            else if (pick != p.ready.begin()) starved_++;
            Entry *e = *pick;
            p.ready.erase(pick);
            p.ready_count.fetch_sub(1, std::memory_order_relaxed);
            e->state = Entry::State::Running;
            e->ran = true;
            p.running = e;
            p.running_priority = e->priority;
            p.running_since.store(t, std::memory_order_relaxed);
            switches_++;
            set_current_thread(e->thread);
            exchange_exception_walks(e->walks);
            l.unlock();
            switch_fiber(e->resume);
            l.lock();
            p.running = nullptr;
            p.preempt.store(false, std::memory_order_relaxed);
            set_current_thread(nullptr);
            exchange_exception_walks(nullptr);
            if (e->state == Entry::State::Running) {
                // yielded (behind the others) or preempted (behind those as high)
                e->state = Entry::State::Ready;
                make_ready(*e, e->yielding);
            } else if (e->state == Entry::State::Done) {
                // its record stays (a Thread outlives its body while handles to
                // it are open, and points at it), off the processor
                delete_fiber(e->fiber);
                e->fiber = e->resume = nullptr;
                p.entries.erase(std::remove(p.entries.begin(), p.entries.end(), e), p.entries.end());
            }
        }
        p.idle = true;
    }

    // ---- the kernel's host-code threads: host threads, as the host scheduler's ----

    bool start_host(Thread &t) {
#ifdef _WIN32
        HANDLE h = CreateThread(
            nullptr, 16u << 20,
            [](LPVOID p) -> DWORD {
                static_cast<Thread *>(p)->body();
                return 0;
            },
            &t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) return false;
        CloseHandle(h);
        return true;
#else
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16u << 20);
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
        return true;
#endif
    }

    void give_up_host_slice() {
        thread_local uint64_t returned = 0, polling_since = 0;
        uint64_t t = now();
        if (t - returned > SPIN_GAP) polling_since = t;
        if (t - polling_since >= SPIN_RUN) {
            naps_++;
            host_nap();
        } else {
            std::this_thread::yield();
        }
        returned = now();
    }

    // ---- the clock ----

    void start_clock() {
        std::call_once(clock_started_, [this] {
            host_fine_timer();
            clock_ = std::thread([this] { clock(); });
        });
    }

    // a deadline on now()'s clock as one on steady_clock's (as the host scheduler's)
    std::chrono::steady_clock::time_point time_point(uint64_t t) {
        constexpr uint64_t CENTURY = 100ull * 365 * 24 * 3600 * 10'000'000;
        uint64_t current = now();
        uint64_t left = t > current ? std::min(t - current, CENTURY) : 0;
        return std::chrono::steady_clock::now() + std::chrono::nanoseconds(left * 100);
    }

    // every millisecond: the timers and KeTimeStampBundle; on a busy
    // processor, the waits whose deadline passed made ready, and a fiber past
    // its quantum with another ready asked to hand over
    void clock() {
        std::unique_lock<std::mutex> l(o_.lock);
        while (!stopping_) {
            o_.on_clock();
            uint64_t t = now();
            for (Processor &p : processors_) {
                if (!p.running) continue;
                for (Entry *e : p.entries) {
                    if (e->state != Entry::State::Blocked || e->deadline == NEVER || e->deadline > t) continue;
                    e->state = Entry::State::Ready;
                    e->deadline = NEVER;
                    p.ready.push_back(e);
                    p.ready_count.fetch_add(1, std::memory_order_relaxed);
                    timeouts_++;
                }
                if (!p.ready.empty() && t - p.running_since.load(std::memory_order_relaxed) >= QUANTUM &&
                    (p.ready.front()->priority >= p.running_priority || starving(p, t) != p.ready.end()))
                    request_preempt(p);
            }
            uint64_t next = std::min(o_.next_due(), now() + 10'000);
            clock_cv_.wait_until(l, time_point(next));
        }
    }

    // (100 ns units) the guest work between yields that still counts as
    // polling, and how long a thread polls before its yields nap
    static constexpr uint64_t SPIN_GAP = 200, SPIN_RUN = 2'000;

    Objects &o_;
    uint64_t epoch_;
    Processor processors_[PROCESSORS];
    std::vector<std::unique_ptr<Entry>> entries_; // under the lock
    bool stopping_ = false;
    std::once_flag clock_started_;
    std::thread clock_;
    std::condition_variable clock_cv_;
    // the report's (switches_, timeouts_, made_, moves_ under the lock)
    uint64_t made_ = 0, switches_ = 0, timeouts_ = 0, moves_ = 0, moves_refused_ = 0, raised_ = 0, starved_ = 0;
    std::atomic<uint64_t> blocks_{0}, yields_{0}, spins_{0}, preemptions_{0}, naps_{0};
    std::atomic<uint32_t> spin_sleepers_{0}; // processors asleep in spin_wait
};

// A guest thread's fiber: its body, then its processor's runner, for good
// (the fiber is deleted there).
void fiber_main(void *arg) {
    Entry *e = static_cast<Entry *>(arg);
    e->thread->body();
    {
        std::lock_guard<std::mutex> l(e->owner->o_.lock); // (the clock reads it)
        e->thread = nullptr;
        e->state = Entry::State::Done;
        e->walks = nullptr;
    }
    switch_fiber(on_processor->runner);
}

} // namespace

std::unique_ptr<Scheduler> make_console_scheduler(Objects &objects) {
    return std::make_unique<ConsoleScheduler>(objects);
}

} // namespace xf::kernel
