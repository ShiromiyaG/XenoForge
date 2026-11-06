// Guest stacks on host stacks of their own (stacks.h; docs/runtime.md,
// "Stacks"). Written anew. Xenia runs a fiber switch on the thread's one
// host stack and re-enters its JIT at the new LR, dropping the host frames
// (XThread::Reenter, from KeSetCurrentStackPointers); its JIT can enter any
// address, the generated code here only a function's start. So each guest
// stack gets a host fiber: a switch suspends the host stack in the export's
// call and resumes the one the new guest stack's frames are on, and those
// frames return as the guest's do.
//
// A suspended stack is resumed as it left only when the switch back comes
// with the r1 it left with and goes on where its code waits (the return
// address after the switch, ppc::switch_stack); else the guest went
// elsewhere on it (its fiber restarted, a longjmp across fibers), and its
// host frames get an Unwind to LR as they resume, which finds the frame of
// theirs that holds r1 or reaches the stack's start and runs on from there.
// A guest stack entered from another thread than the one whose host fiber
// holds its frames (a fiber that moved) starts over on a host fiber of the
// new thread, as the frames use the other thread's context: its outer
// frames run interpreted there.

#include "stacks.h"

#include "objects.h"
#include "xenoforge/kernel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#endif

namespace xf::kernel {
namespace {

constexpr size_t HOST_STACK = 16u << 20; // reserved; as a thread's (generated code recurses as deep as the guest's)

// ---- host fibers ----

// (FIBER_FLAG_FLOAT_SWITCH: each fiber its own MXCSR, though the generated
// code leaves it at the default between instructions anyway)
#ifdef _WIN32
// the calling thread as a fiber; converted: whether it was made one here
void *convert_thread(bool &converted) {
    converted = !IsThreadAFiber();
    return converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
}
void revert_thread() { ConvertFiberToThread(); }
void *create_fiber(void (*entry)(void *), void *arg) {
    return CreateFiberEx(64 * 1024, HOST_STACK, FIBER_FLAG_FLOAT_SWITCH, reinterpret_cast<LPFIBER_START_ROUTINE>(entry),
                         arg);
}
void switch_fiber(void *fiber) { SwitchToFiber(fiber); }
void delete_fiber(void *fiber) { DeleteFiber(fiber); }
#else
struct PosixFiber {
    ucontext_t context;
    void *stack = nullptr;
    void (*entry)(void *) = nullptr;
    void *arg = nullptr;
};
thread_local PosixFiber *running = nullptr;
void trampoline() {
    running->entry(running->arg);
    std::abort(); // a stack's code never returns (stack_main)
}
void *convert_thread(bool &converted) {
    converted = !running;
    return running ? running : running = new PosixFiber();
}
void revert_thread() {
    delete running;
    running = nullptr;
}
void *create_fiber(void (*entry)(void *), void *arg) {
    void *stack = mmap(nullptr, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) return nullptr;
    auto *f = new PosixFiber();
    f->stack = stack, f->entry = entry, f->arg = arg;
    getcontext(&f->context);
    f->context.uc_stack.ss_sp = stack;
    f->context.uc_stack.ss_size = HOST_STACK;
    f->context.uc_link = nullptr;
    makecontext(&f->context, trampoline, 0);
    return f;
}
void switch_fiber(void *fiber) {
    PosixFiber *from = running;
    running = static_cast<PosixFiber *>(fiber);
    swapcontext(&from->context, &running->context);
}
void delete_fiber(void *fiber) {
    auto *f = static_cast<PosixFiber *>(fiber);
    munmap(f->stack, HOST_STACK);
    delete f;
}
#endif

// ---- the guest stacks of every thread ----

// Every guest stack a host fiber stands for, by its base, and the lock over
// every thread's GuestStacks.
struct Registry {
    std::mutex mutex;
    std::map<uint32_t, std::pair<Thread *, GuestStack *>> by_base;
    // for boot's report: switches between two stacks, host fibers made, stacks
    // switched back to elsewhere than they left, host fibers dropped
    uint64_t switches = 0, made = 0, elsewhere = 0, dropped = 0;
};

bool holds(const GuestStack &s, uint32_t sp, uint32_t base) {
    return s.base == base || (sp >= s.limit && sp <= s.base);
}

// (the lock held) its record goes; its host fiber is deleted from another
// one of that thread (it may be the one running the caller)
void drop(Registry &r, Thread &t, GuestStack *stack) {
    GuestStacks &s = *t.stacks;
    if (auto it = r.by_base.find(stack->base); it != r.by_base.end() && it->second.second == stack) r.by_base.erase(it);
    s.dead.push_back(stack->fiber);
    r.dropped++;
    std::erase_if(s.stacks, [&](const std::unique_ptr<GuestStack> &p) { return p.get() == stack; });
}

// Back on a stack (switched to, or its first run): the host fibers of
// stacks gone deleted, then what it was left to do: an exception another
// stack's code ended with, on the thread's own stack; an Unwind, when the
// switch came back elsewhere than it left.
void resumed(Kernel &k, Registry &r, Thread &t, ppc::Context &c) {
    std::vector<void *> dead;
    std::exception_ptr rethrow;
    bool unwind;
    {
        std::lock_guard lock(r.mutex);
        GuestStacks &s = *t.stacks;
        dead.swap(s.dead);
        unwind = std::exchange(s.current->unwind, false);
        if (s.current->own) rethrow = std::exchange(s.rethrow, nullptr);
    }
    for (void *f : dead) delete_fiber(f);
    if (rethrow) std::rethrow_exception(rethrow);
    if (unwind) {
        k.unwinds_thrown++;
        throw ppc::Unwind{uint32_t(c.lr) & ~3u};
    }
}

// A guest stack's host fiber: the guest from where it entered the stack,
// with no frame to return to. An Unwind that reaches here (no frame of this
// stack holds it) goes on from its target; anything else ends the thread's
// code here, thrown again on the thread's own stack (ExitThread's ThreadExit
// reaches Thread::body there).
void stack_main(void *arg) {
    Thread &t = *static_cast<Thread *>(arg);
    Kernel &k = t.owner.k;
    Registry &r = k.state<Registry>();
    ppc::Context &c = *t.context;
    GuestStack *self;
    {
        std::lock_guard lock(r.mutex);
        self = t.stacks->current;
    }
    uint32_t pc = self->start;
    std::exception_ptr ended;
    for (;;) {
        try {
            resumed(k, r, t, c);
            k.run_from(c, pc);
            ended = std::make_exception_ptr(ThreadExit{0}); // the kernel stopped
            break;
        } catch (const ppc::Unwind &u) {
            k.unwinds_by_stack_start++;
            pc = u.target;
        } catch (...) {
            ended = std::current_exception();
            break;
        }
    }
    void *own;
    {
        std::lock_guard lock(r.mutex);
        GuestStacks &s = *t.stacks;
        s.rethrow = ended;
        GuestStack *home = s.stacks.front().get();
        drop(r, t, self);
        s.current = home;
        own = home->fiber;
    }
    switch_fiber(own); // never back: deleted from there
}

} // namespace

void switch_guest_stack(Kernel &k, Thread &t, ppc::Context &c, uint32_t from_sp, uint32_t base, uint32_t limit) {
    Registry &r = k.state<Registry>();
    const uint32_t sp = uint32_t(c.r[1]), lr = uint32_t(c.lr) & ~3u;
    // where the code waiting in this call goes on (GuestStack::resume)
    uint32_t resume = std::exchange(t.stack_resume, 0);
    if (!resume) resume = lr;
    else if (resume == STACK_RESUME_ANYWHERE) resume = 0;
    GuestStack *to;
    {
        std::lock_guard lock(r.mutex);
        if (!t.stacks) {
            bool converted;
            void *fiber = convert_thread(converted);
            if (!fiber) return; // r1 moves alone; an unwind takes the switch back
            auto own = std::make_unique<GuestStack>();
            own->base = t.stack_base, own->limit = t.stack_limit, own->fiber = fiber, own->own = true;
            t.stacks = std::make_unique<GuestStacks>();
            t.stacks->converted = converted;
            t.stacks->current = own.get();
            r.by_base[own->base] = {&t, own.get()};
            t.stacks->stacks.push_back(std::move(own));
        }
        GuestStacks &s = *t.stacks;
        GuestStack *from = s.current;
        if (holds(*from, sp, base)) return; // the same stack
        to = nullptr;
        for (auto &p : s.stacks)
            if (p->base == base) to = p.get();
        if (!to)
            for (auto &p : s.stacks)
                if (sp >= p->limit && sp <= p->base) to = p.get();
        if (!to) {
            // Another thread's host fiber may hold frames of it (a fiber that
            // moved): theirs use that thread's context, so they are dropped
            // and the stack starts over here. One running there is left be.
            if (auto it = r.by_base.find(base); it != r.by_base.end() && it->second.first != &t) {
                Thread &other = *it->second.first;
                if (other.stacks->current != it->second.second)
                    drop(r, other, it->second.second);
                else
                    r.by_base.erase(it);
            }
            auto made = std::make_unique<GuestStack>();
            made->base = base, made->limit = limit, made->start = lr;
            made->fiber = create_fiber(stack_main, &t);
            if (!made->fiber) {
                std::fprintf(stderr, "kernel: no host fiber for the guest stack at 0x%08X\n", base);
                return;
            }
            to = made.get();
            r.by_base[base] = {&t, to};
            r.made++;
            s.stacks.push_back(std::move(made));
        } else if (to->sp != sp || (to->resume && to->resume != lr)) {
            to->unwind = true;
            r.elsewhere++;
        }
        from->sp = from_sp, from->resume = resume;
        s.current = to;
        r.switches++;
    }
    switch_fiber(to->fiber);
    resumed(k, r, t, c);
}

void guest_stacks_freed(Kernel &k, uint32_t base, uint32_t end) {
    Registry &r = k.state<Registry>();
    std::lock_guard lock(r.mutex);
    for (auto it = r.by_base.upper_bound(end); it != r.by_base.end() && it->first <= base;) {
        auto [thread, stack] = (it++)->second;
        // one in use (the guest freeing the stack it runs on) or a thread's own stays
        if (thread->stacks->current != stack && !stack->own) drop(r, *thread, stack);
    }
}

void end_guest_stacks(Kernel &k, Thread &t) {
    if (!t.stacks) return;
    Registry &r = k.state<Registry>();
    std::vector<void *> fibers;
    bool converted;
    {
        std::lock_guard lock(r.mutex);
        GuestStacks &s = *t.stacks;
        for (auto &p : s.stacks) {
            if (auto it = r.by_base.find(p->base); it != r.by_base.end() && it->second.second == p.get())
                r.by_base.erase(it);
            if (!p->own) fibers.push_back(p->fiber);
        }
        fibers.insert(fibers.end(), s.dead.begin(), s.dead.end());
        converted = s.converted;
        t.stacks.reset();
    }
    for (void *f : fibers) delete_fiber(f);
    if (converted) revert_thread();
}

std::string stacks_report(Kernel &k) {
    Registry &r = k.state<Registry>();
    uint64_t switches, made, elsewhere, dropped;
    {
        std::lock_guard lock(r.mutex);
        switches = r.switches, made = r.made, elsewhere = r.elsewhere, dropped = r.dropped;
    }
    uint64_t thrown = k.unwinds_thrown, by_dispatch = k.unwinds_by_dispatch, by_start = k.unwinds_by_stack_start;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "stacks: %llu switches between guest stacks, %llu host fibers made, %llu dropped, %llu switched "
                  "back to elsewhere\nreturns to another frame: %llu; %llu taken by generated code at a resume site, "
                  "%llu by a dispatch (interpreting on), %llu at a guest stack's start\n",
                  (unsigned long long)switches, (unsigned long long)made, (unsigned long long)dropped,
                  (unsigned long long)elsewhere, (unsigned long long)thrown,
                  (unsigned long long)(thrown - std::min(thrown, by_dispatch + by_start)),
                  (unsigned long long)by_dispatch, (unsigned long long)by_start);
    return line;
}

} // namespace xf::kernel
