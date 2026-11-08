// Guest stacks on host stacks of their own (docs/runtime.md, "Stacks"): a
// thread that moves to another guest stack (KeSetCurrentStackPointers, the
// XDK's SwitchToFiber) moves to that stack's host fiber, so each guest
// stack's generated frames stay where they are while another runs, and a
// switch back returns into them. stacks.cpp.

#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <vector>

namespace xf::ppc {
struct Context;
}

namespace xf::kernel {

class Kernel;
class Thread;

// A guest stack a thread has run on, and the host stack its code runs on.
struct GuestStack {
    uint32_t base = 0, limit = 0; // the guest stack, [limit, base]; base its high end
    void *fiber = nullptr;        // its host fiber (the thread's own stack: the thread, converted)
    bool own = false;             // the thread's own stack
    uint32_t start = 0;           // where the guest entered it first
    // When it was switched away from: r1, and where the code waiting in
    // that switch goes on (ppc::switch_stack; 0: where LR says, as the
    // interpreter does).
    uint32_t sp = 0, resume = 0;
    // switched back to elsewhere: an Unwind to LR as it resumes
    bool unwind = false;
};

// A thread's guest stacks, from its first switch (Thread::stacks). Changed
// under the kernel's lock for them (stacks.cpp), as another thread may drop
// one (MmDeleteKernelStack).
struct GuestStacks {
    std::vector<std::unique_ptr<GuestStack>> stacks; // the first: the thread's own
    GuestStack *current = nullptr;
    std::vector<void *> dead;   // host fibers of stacks gone, deleted from another one
    std::exception_ptr rethrow; // what a stack's code ended with (ExitThread), for its own stack to throw
    bool converted = false;     // the host thread was made a fiber here (and is made a thread again at the end)
};

// Thread::stack_resume when the interpreter runs the thunk's sc: its code
// goes on where LR says after the call (it was a tail branch). Unset, the
// call goes on at its own return address (LR at the call: a bl, a dispatch).
constexpr uint32_t STACK_RESUME_ANYWHERE = 1;

// Host fibers, also the deterministic scheduler's (deterministic.cpp), which
// runs every guest thread on one host thread: a thread converted to one
// (converted: made one here, not already one), made (a 16 MB reservation),
// switched to, deleted (from another), and the one running (nullptr: the
// thread is no fiber).
void *convert_thread(bool &converted);
void revert_thread();
void *create_fiber(void (*entry)(void *), void *arg);
void switch_fiber(void *fiber);
void delete_fiber(void *fiber);
void *current_fiber();

// KeSetCurrentStackPointers on the calling thread's context, r1 already set
// to the new stack (from_sp: where the old one was): when the new stack is
// another, its host fiber runs (made the first time, from LR), and this one
// waits in the call until a switch comes back to it.
void switch_guest_stack(Kernel &k, Thread &t, ppc::Context &c, uint32_t from_sp, uint32_t base, uint32_t limit);
// MmDeleteKernelStack: the host fibers of guest stacks in [end, base) go.
void guest_stacks_freed(Kernel &k, uint32_t base, uint32_t end);
// The thread ends, on its own stack: its other host fibers go.
void end_guest_stacks(Kernel &k, Thread &t);

// A guest stack's frames by its back chain from sp: each frame's saved LR
// (the XDK's prologues store it 8 bytes below the caller's frame), at most
// `max`. Read only where the pages are readable: inside [low, high) when
// given (a thread's own stack: no lock is taken, as the crash report needs),
// else by asking the heap (a stack's guard pages are not readable).
std::vector<uint32_t> guest_backtrace(Kernel &k, uint32_t sp, uint32_t low = 0, uint32_t high = 0, int max = 16);

} // namespace xf::kernel
