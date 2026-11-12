// The deterministic mode (plan A5; docs/runtime.md, "The deterministic mode"):
// the scheduler that runs every guest thread as a host fiber on one host
// thread, in a fixed order, on a virtual clock. Written anew: Xenia runs guest
// threads as host threads and has no such mode.
//
// - One guest thread runs at a time; it hands over only where the kernel sees
//   it: a wait that blocks, a yield (NtYieldExecution, a spin lock's spins),
//   and, at a kernel call, a time base read, an indirect call (dispatch) or
//   the recompiler's yield points (a loop's back edge, a priority hint;
//   Kernel's system_.yield), the end of its slice or a deadline come due. The thread that ran goes to the tail of the ready
//   queue; woken threads join the tail in the order they were woken; waits
//   whose deadline passed are ended in the order the threads were made. All
//   of it follows from the guest's own steps, so the same disc and the same
//   input give the same order.
// - The clock is virtual: those four move it by fixed
//   costs (Deterministic), and when every thread waits it jumps to the
//   earliest deadline (a wait's timeout, a timer). KeTimeStampBundle and the
//   timers move on it every millisecond, as the host scheduler's clock thread
//   moves them. Paced, it never runs ahead of the host's clock.
// - A thread at DISPATCH_LEVEL or above (an interrupt's or a DPC's callback,
//   a spin lock held) is not handed over at a kernel call, as NT does not
//   preempt there; the video family also holds a host mutex per CPU around an
//   interrupt's callback.
// - Guest code that runs without any of those (a straight run of code with
//   no loop, or a thread stuck at DISPATCH_LEVEL) never hands over by
//   itself: a watchdog stops it after a while of the host's time and hands
//   it over from there (watchdog(); the run is then exact only if it stopped
//   in a pure spin). With the yield points it should not fire; report()
//   counts it, and names a thread that has run a second without handing
//   over.

#include "objects.h"
#include "stacks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace xf::kernel {
namespace {

constexpr uint64_t MILLISECOND = 10'000; // in the clock's 100 ns

// the scheduler's host thread (and every fiber on it)
thread_local bool on_runner = false;

// the calling thread's processor time (user and kernel), in seconds
double thread_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    auto ticks = [](const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 1e7;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
#endif
}

class DeterministicScheduler;

// a guest thread as the scheduler sees it (Thread::scheduled)
struct Entry {
    DeterministicScheduler *owner = nullptr;
    Thread *thread = nullptr; // nullptr once its body returned (it may be gone)
    void *fiber = nullptr;    // made for it, deleted once it ended
    void *resume = nullptr;   // where it goes on: its fiber, or a guest stack's (stacks.h)
    void *walks = nullptr;    // its exception dispatches under way (exceptions.cpp)
    enum class State : uint8_t { Ready, Running, Blocked, Done } state = State::Ready;
    uint64_t deadline = Scheduler::NEVER;
};

void fiber_main(void *arg);

class DeterministicScheduler final : public Scheduler {
public:
    DeterministicScheduler(Objects &o, const Deterministic &d) : o_(o), d_(d) {}

    ~DeterministicScheduler() override {
        bool idle;
        {
            std::lock_guard<std::mutex> l(o_.lock);
            stopping_ = true;
            idle = idle_;
        }
        wake_cv_.notify_all();
        watchdog_stop_ = true;
        if (watchdog_.joinable()) watchdog_.join();
#ifdef _WIN32
        if (runner_handle_) CloseHandle(runner_handle_);
#endif
        // a guest thread still running holds the runner: it is left be, as
        // the host scheduler leaves its threads
        if (runner_.joinable()) {
            if (idle) runner_.join();
            else runner_.detach();
        }
    }

    bool start(Thread &t) override {
        std::lock_guard<std::mutex> l(o_.lock);
        auto e = std::make_unique<Entry>();
        e->owner = this, e->thread = &t;
        e->fiber = e->resume = create_fiber(fiber_main, e.get());
        if (!e->fiber) return false;
        t.scheduled = e.get();
        ready_.push_back(e.get());
        entries_.push_back(std::move(e));
        made_++;
        if (!runner_.joinable()) {
            runner_ = std::thread([this] { run(); });
            if (d_.force_after_ms) watchdog_ = std::thread([this] { watchdog(); });
        } else if (!on_runner) {
            wake_cv_.notify_one();
        }
        return true;
    }

    void join(Thread &t) override {
        if (on_runner) {
            // a guest thread waits for another: it yields until that one ends
            for (;;) {
                {
                    std::lock_guard<std::mutex> l(o_.lock);
                    if (t.finished) return;
                }
                yield();
            }
        }
        std::unique_lock<std::mutex> l(o_.lock);
        o_.thread_exited.wait(l, [&] { return t.finished; });
    }

    bool block(Thread &self, std::unique_lock<std::mutex> &lock, uint64_t deadline) override {
        Entry *e = static_cast<Entry *>(self.scheduled);
        self.woken = false;
        if (!e || !on_runner || running_ != e) {
            // not one of the fibers (cannot be: every guest thread is)
            std::fprintf(stderr, "deterministic: thread %u blocks off the scheduler's thread\n", self.id);
            while (!self.woken) self.cv.wait(lock);
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
        e->state = Entry::State::Ready;
        e->deadline = NEVER;
        ready_.push_back(e);
        if (!on_runner) {
            foreign_wakes_++;
            wake_cv_.notify_one();
        }
    }

    void yield() override {
        if (!on_runner || !running_) return std::this_thread::yield();
        yields_++;
        hand_over(*running_);
    }

    // A spin (kernel.cpp's system_.yield, a run of yield points that ran out
    // on priority hints): with spin_idle and no other thread ready, what it
    // waits for can only come with a deadline, so the clock jumps there as
    // when every thread waits (run()); then it hands over as a yield does.
    void spin() override {
        if (!on_runner || !running_) return std::this_thread::yield();
        if (d_.spin_idle) {
            std::lock_guard<std::mutex> l(o_.lock);
            const uint64_t now = now_.load(), next = ready_.empty() ? next_event() : NEVER;
            if (next != NEVER && next > now) {
                spin_jumps_++;
                spin_clock_ += next - now;
                now_.store(next);
            }
        }
        yields_++;
        hand_over(*running_);
    }

    uint64_t now() override { return now_.load(std::memory_order_relaxed); }

    void timers_changed() override {
        if (on_runner) {
            switch_at_ = std::min(switch_at_, o_.next_due());
        } else {
            pokes_++;
            wake_cv_.notify_one();
        }
    }

    void advance(uint64_t cost) override {
        if (!on_runner || !running_) return; // a host thread's call, or the scheduler's own
        uint64_t now = now_.load(std::memory_order_relaxed) + cost;
        now_.store(now, std::memory_order_relaxed);
        if (now < switch_at_) return;
        Thread *t = running_->thread;
        if (t && o_.k.host(t->pcr)[kpcr::CURRENT_IRQL] >= 2) { // at DISPATCH_LEVEL: on to its end
            deferred_++;
            return;
        }
        slices_++;
        hand_over(*running_);
    }

    bool deterministic() const override { return true; }

    std::string report() override { // (the kernel's lock held)
        char line[640];
        double wall = seconds_since(started_);
        std::snprintf(line, sizeof(line),
                      "deterministic: clock %.3f s in %.3f s on the host (%s); %llu threads made, %llu runs: %llu "
                      "slices ended, %llu yields, %llu waits blocked (%llu ended by their deadline), %llu wakes from "
                      "host threads; %llu jumps of the idle clock (%.3f s); %llu times every thread waited with no "
                      "deadline; %llu hand-overs forced%s%s\n",
                      double(now_.load()) / 1e7, wall, d_.paced ? "paced" : "as fast as it goes",
                      (unsigned long long)made_, (unsigned long long)switches_, (unsigned long long)slices_,
                      (unsigned long long)yields_, (unsigned long long)blocks_, (unsigned long long)timeouts_,
                      (unsigned long long)foreign_wakes_, (unsigned long long)jumps_, double(idle_clock_) / 1e7,
                      (unsigned long long)stuck_, (unsigned long long)forced_.load(),
                      forced_ ? " (the run is not exact)" : "", halted_ ? "; halted" : "");
        std::string out = line;
        // the scheduler's host thread's processor time to the halt (every
        // guest thread's code, the kernel's calls, the command processor's
        // fiber), and the spins taken for idle waits (spin_idle)
        std::snprintf(line, sizeof(line),
                      "deterministic: the scheduler's thread used %.3f s of processor time%s; %llu spins taken for "
                      "idle waits (%.3f s of the clock)\n",
                      runner_cpu_.load(), halted_ ? " to the halt" : " so far", (unsigned long long)spin_jumps_,
                      double(spin_clock_) / 1e7);
        out += line;
        // a thread that has not handed over for a second of the host's: guest
        // code waiting without a kernel call (the header's last point)
        uint32_t id = running_id_.load();
        double running = seconds_since(running_since_.load());
        Entry *e = running_;
        if (id && running >= 1 && e) {
            Thread *t = e->thread;
            std::snprintf(line, sizeof(line),
                          "deterministic: thread %u has run %.1f s without handing over (IRQL %u, %llu deferred "
                          "hand-overs)\n",
                          id, running, t ? unsigned(o_.k.host(t->pcr)[kpcr::CURRENT_IRQL]) : 0u,
                          (unsigned long long)deferred_);
            out += line;
        }
        return out;
    }

    bool halted() {
        std::lock_guard<std::mutex> l(o_.lock);
        return halted_;
    }

    // the runner: a fiber made from the scheduler's host thread
    void *runner_fiber_ = nullptr;

private:
    using Clock = std::chrono::steady_clock;

    static double seconds_since(int64_t since_ns) {
        if (!since_ns) return 0;
        int64_t now = Clock::now().time_since_epoch().count();
        return double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::duration(now - since_ns)).count()) /
               1e9;
    }

    // the running thread's fiber gives the host thread back to the runner;
    // it goes on here when the runner picks it again
    void hand_over(Entry &e) {
        e.resume = current_fiber();
        e.walks = exchange_exception_walks(nullptr);
        switch_fiber(runner_fiber_);
    }

    // the earliest deadline: a blocked wait's, a timer's (lock held)
    uint64_t next_event() {
        uint64_t next = o_.next_due();
        for (auto &e : entries_)
            if (e->state == Entry::State::Blocked) next = std::min(next, e->deadline);
        return next;
    }

    // paced: the host's time for a time on the clock, and a sleep until then
    // (the lock let go meanwhile)
    void pace(std::unique_lock<std::mutex> &l, uint64_t clock) {
        if (!d_.paced) return;
        auto at = wall_start_ + std::chrono::nanoseconds((clock - clock_start_) * 100);
        if (at <= Clock::now() + std::chrono::milliseconds(1)) return;
        l.unlock();
        std::this_thread::sleep_until(at);
        l.lock();
    }

    // The forced hand-over: guest code that has not handed over for
    // force_after_ms of the host's time (a spin on a flag, with no kernel call,
    // time base read or indirect call) is stopped where it is, if that is in
    // the generated code, and handed over from there; its registers are put
    // back as it was when it is picked again. Where it stops depends on the
    // host's speed, so a run with one is no longer exact (the report counts
    // them). Windows only so far.
    // TODO(posix): pthread_kill and a signal handler that swaps the ucontext.
    void watchdog() {
#ifdef _WIN32
        uint64_t last = UINT64_MAX;
        auto since = Clock::now();
        bool forced_this_run = false;
        auto [low, high] = o_.k.generated_code_span();
        while (!watchdog_stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uint64_t gen = generation_.load();
            if (gen != last || !running_id_.load()) {
                last = gen, since = Clock::now(), forced_this_run = false;
                continue;
            }
            if (forced_this_run || !runner_handle_ || low == high ||
                Clock::now() - since < std::chrono::milliseconds(d_.force_after_ms))
                continue;
            if (SuspendThread(runner_handle_) == DWORD(-1)) continue;
            alignas(16) CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_ALL;
            Entry *e = running_;
            bool ok = generation_.load() == gen && e && e->thread && GetThreadContext(runner_handle_, &ctx) &&
                      ctx.Rip >= low && ctx.Rip < high && o_.k.host(e->thread->pcr)[kpcr::CURRENT_IRQL] < 2;
            if (ok) {
                // a call to forced_entry on the same stack, below what the
                // code had (a frame aligned as a call leaves it)
                forced_context_ = ctx;
                ctx.Rsp = ((ctx.Rsp - 1024) & ~uint64_t(15)) - 8;
                ctx.Rip = reinterpret_cast<DWORD64>(&forced_entry);
                ok = SetThreadContext(runner_handle_, &ctx);
                if (ok) forced_++, forced_this_run = true;
            }
            ResumeThread(runner_handle_);
        }
#endif
    }

#ifdef _WIN32
    // where a forced hand-over enters on the scheduler's thread: the thread
    // sleeps as long on the clock as it spun on the host (time passes for a
    // spinning thread: what it waits for may move on the clock, a timeout, the
    // sound's pace), then the registers as the code had them
    static void forced_entry() {
        DeterministicScheduler *s = runner_scheduler;
        alignas(16) CONTEXT ctx = s->forced_context_;
        Entry &e = *s->running_;
        {
            std::lock_guard<std::mutex> l(s->o_.lock);
            e.state = Entry::State::Blocked;
            e.deadline = s->now_.load() + std::max<uint64_t>(s->d_.slice, s->d_.force_after_ms * MILLISECOND);
        }
        s->hand_over(e);
        RtlRestoreContext(&ctx, nullptr);
    }
    static thread_local DeterministicScheduler *runner_scheduler;
    CONTEXT forced_context_ = {};
    std::atomic<HANDLE> runner_handle_{nullptr};
#endif

    void run() {
        on_runner = true;
        bool converted;
        runner_fiber_ = convert_thread(converted);
        if (!runner_fiber_) {
            std::fprintf(stderr, "deterministic: the scheduler's thread could not be made a fiber\n");
            return;
        }
#ifdef _WIN32
        runner_scheduler = this;
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0);
        runner_handle_ = self;
#endif
        started_ = Clock::now().time_since_epoch().count();
        wall_start_ = Clock::now();
        const double cpu_start = thread_cpu_seconds();
        std::unique_lock<std::mutex> l(o_.lock);
        clock_start_ = now_.load();
        uint64_t next_clock = clock_start_;
        while (!stopping_) {
            uint64_t now = now_.load();
            // KeTimeStampBundle every millisecond, and the timers come due
            if (now >= next_clock || o_.next_due() <= now) {
                o_.on_clock();
                next_clock = (now / MILLISECOND + 1) * MILLISECOND;
            }
            // the waits whose deadline passed, in the threads' order
            for (auto &e : entries_) {
                if (e->state != Entry::State::Blocked || e->deadline > now) continue;
                e->state = Entry::State::Ready;
                e->deadline = NEVER;
                ready_.push_back(e.get());
                timeouts_++;
            }
            // (read now and then: the report's, from another thread)
            if ((switches_ & 4095) == 0) runner_cpu_ = thread_cpu_seconds() - cpu_start;
            if (!halted_ && d_.halt && d_.halt()) {
                runner_cpu_ = thread_cpu_seconds() - cpu_start;
                halted_ = true;
                std::fprintf(stderr, "deterministic: halted at %.4f s of the clock, %llu runs\n", double(now) / 1e7,
                             (unsigned long long)switches_);
            }
            if (halted_) {
                idle_ = true;
                wake_cv_.wait(l, [&] { return stopping_; });
                break;
            }
            if (ready_.empty()) {
                // the clock jumps to the earliest deadline (on_clock then
                // catches KeTimeStampBundle up)
                uint64_t next = next_event();
                if (next == NEVER) {
                    // nothing can run until a host thread wakes one
                    if (stuck_++ == 0)
                        std::fprintf(stderr, "deterministic: every guest thread waits with no deadline (%.4f s)\n",
                                     double(now) / 1e7);
                    idle_ = true;
                    uint64_t pokes = pokes_;
                    wake_cv_.wait(l, [&] { return stopping_ || !ready_.empty() || pokes_ != pokes; });
                    idle_ = false;
                    continue;
                }
                next = std::max(next, now + 1);
                pace(l, next);
                jumps_++;
                idle_clock_ += next - now;
                now_.store(std::max(now_.load(), next));
                continue;
            }
            pace(l, now);
            Entry *e = ready_.front();
            ready_.pop_front();
            e->state = Entry::State::Running;
            running_ = e;
            switch_at_ = std::min({now + d_.slice, next_clock, next_event()});
            switches_++;
            generation_++;
            running_id_ = e->thread ? e->thread->id : 0;
            running_since_ = Clock::now().time_since_epoch().count();
            set_current_thread(e->thread);
            exchange_exception_walks(e->walks);
            l.unlock();
            switch_fiber(e->resume);
            l.lock();
            running_ = nullptr;
            running_id_ = 0;
            set_current_thread(nullptr);
            exchange_exception_walks(nullptr);
            if (e->state == Entry::State::Running) {
                e->state = Entry::State::Ready;
                ready_.push_back(e);
            } else if (e->state == Entry::State::Done) {
                // its record stays (a Thread outlives its body while handles
                // to it are open, and points at it), out of the scan
                delete_fiber(e->fiber);
                e->fiber = e->resume = nullptr;
                auto it = std::find_if(entries_.begin(), entries_.end(),
                                       [e](const std::unique_ptr<Entry> &p) { return p.get() == e; });
                done_.push_back(std::move(*it));
                entries_.erase(it);
            }
        }
        idle_ = true;
    }

    Objects &o_;
    Deterministic d_;
    std::thread runner_;
    std::condition_variable wake_cv_; // a host thread woke one, armed a timer, or the kernel ends
    // under the kernel's lock
    std::vector<std::unique_ptr<Entry>> entries_; // in the order the threads were made
    std::vector<std::unique_ptr<Entry>> done_;    // the threads whose body returned
    std::deque<Entry *> ready_;
    bool stopping_ = false, idle_ = false, halted_ = false;
    uint64_t pokes_ = 0;
    // the scheduler's thread's own
    Entry *running_ = nullptr;
    uint64_t switch_at_ = NEVER;
    std::atomic<uint64_t> now_{0};
    uint64_t clock_start_ = 0;
    Clock::time_point wall_start_;
    // the report's
    std::atomic<int64_t> started_{0}, running_since_{0};
    std::atomic<uint32_t> running_id_{0};
    uint64_t made_ = 0, switches_ = 0, slices_ = 0, yields_ = 0, blocks_ = 0, timeouts_ = 0, foreign_wakes_ = 0;
    uint64_t jumps_ = 0, idle_clock_ = 0, stuck_ = 0;
    uint64_t spin_jumps_ = 0, spin_clock_ = 0; // spins taken for idle waits (spin_idle)
    std::atomic<double> runner_cpu_{0};        // the scheduler's thread's processor time
    std::atomic<uint64_t> deferred_{0}; // hand-overs put off at DISPATCH_LEVEL
    // the forced hand-overs' (watchdog)
    std::thread watchdog_;
    std::atomic<bool> watchdog_stop_{false};
    std::atomic<uint64_t> generation_{0}, forced_{0}; // runs started; hand-overs forced
};

// A guest thread's fiber: its body, then the runner, for good (the fiber is
// deleted there).
void fiber_main(void *arg) {
    Entry *e = static_cast<Entry *>(arg);
    e->thread->body();
    e->thread = nullptr;
    e->state = Entry::State::Done;
    e->walks = nullptr;
    switch_fiber(e->owner->runner_fiber_);
}

#ifdef _WIN32
thread_local DeterministicScheduler *DeterministicScheduler::runner_scheduler = nullptr;
#endif

} // namespace

std::unique_ptr<Scheduler> make_deterministic_scheduler(Objects &objects, const Deterministic &options) {
    return std::make_unique<DeterministicScheduler>(objects, options);
}

bool Kernel::make_deterministic(const Deterministic &options) {
    Objects &o = *objects_;
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (!o.threads.empty() || o.scheduler->deterministic()) return false;
    }
    o.scheduler = make_deterministic_scheduler(o, options);
    // the clock starts at 0 and the system time at the options' (the host's
    // date would differ from run to run)
    o.system_time_base = options.system_time;
    deterministic_call_cost_ = options.call_cost;
    deterministic_time_base_cost_ = options.time_base_cost;
    deterministic_dispatch_cost_ = options.dispatch_cost;
    deterministic_yield_units_ = options.yield_units;
    deterministic_spin_idle_ = options.spin_idle;
    return true;
}

bool Kernel::deterministic() const { return objects_->scheduler->deterministic(); }

bool Kernel::halted() const {
    auto *s = dynamic_cast<DeterministicScheduler *>(objects_->scheduler.get());
    return s && s->halted();
}

uint64_t Kernel::memory_hash() {
    // FNV-1a over 64-bit words: each region's address and size, then its bytes
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    for (uint64_t a = 0; a < 0x1'0000'0000ull;) {
        Heap *heap = memory_.heap_for(uint32_t(a));
        if (!heap) {
            a += 0x1000;
            continue;
        }
        RegionInfo info;
        if (!heap->query(uint32_t(a), info) || !info.region_size) {
            a += heap->page_size();
            continue;
        }
        uint64_t begin = std::max<uint64_t>(a, info.base_address), end = uint64_t(info.base_address) + info.region_size;
        if ((info.state & ALLOCATE_COMMIT) && (info.protect & PROTECT_READ) && end > begin) {
            mix(begin), mix(end - begin);
            const uint8_t *p = host(uint32_t(begin));
            size_t n = size_t(end - begin);
            size_t i = 0;
            for (; i + 8 <= n; i += 8) {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                mix(w);
            }
            for (; i < n; i++) mix(p[i]);
        }
        a = end > a ? end : a + heap->page_size();
    }
    return h;
}

} // namespace xf::kernel
