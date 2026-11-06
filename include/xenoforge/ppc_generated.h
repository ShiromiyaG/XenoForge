// What the generated code includes (docs/recompiler.md): the ops, the
// branch conditions, and the things whoever links the code provides.
// A generated function is `void name(xf::ppc::Context &c)`; each
// instruction is one line:
//
//   I(lwz, 0x81430008);        // ops::run<Opcode::lwz>(c, Instr{...})
//   if (BC(12, 26)) goto L_822D28E4;
//   XF_TAIL(xf_822D2910);      // a tail call, no frame left behind

#pragma once

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_ops.h"

namespace xf::ppc {

// An indirect call or jump, or a call to code with no generated function:
// the function at `target`, else the interpreter from there until it
// returns. LR is already set as the code set it.
void dispatch(Context &c, uint32_t target);
// An invalid instruction reached.
void invalid(Context &c, uint32_t pc);

// Throws an Unwind (ppc_module.h) to where LR points (a function that
// switches the stack returning elsewhere than to its caller).
[[noreturn]] void unwind(Context &c);
// A branch to KeSetCurrentStackPointers' thunk (a fiber switch), with where
// this frame's code goes on when its stack is switched back to: the return
// address after a bl, the function's own return address for a tail branch.
// The kernel runs each guest stack on a host stack of its own.
void switch_stack(Context &c, uint32_t thunk, uint32_t resume);

} // namespace xf::ppc

#define XF_FN(name) void name([[maybe_unused]] ::xf::ppc::Context &c)
#define I(op, code) ::xf::ppc::ops::run<::xf::ppc::Opcode::op>(c, ::xf::ppc::Instr{code})
#define BC(bo, bi) ::xf::ppc::ops::branch_condition(c, bo, bi)
#define BCCTR(bo, bi) ::xf::ppc::ops::branch_condition_ctr(c, bo, bi)
#if defined(__clang__)
#define XF_TAIL(f) [[clang::musttail]] return f(c)
#else
#define XF_TAIL(f) return f(c)
#endif
// A return from a function that switches the stack: to its caller when LR
// and r1 are its entry's (xf_lr, xf_sp), else thrown to the frame it goes to.
#define XF_CHECKED_RETURN()                                                                                            \
    do {                                                                                                               \
        if (uint32_t(c.r[1]) != xf_sp || (uint32_t(c.lr) & ~3u) != xf_lr) ::xf::ppc::unwind(c);                        \
        return;                                                                                                        \
    } while (0)
