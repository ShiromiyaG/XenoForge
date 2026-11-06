// What the generated code includes (docs/recompiler.md): the ops, the
// branch conditions, and the runtime's calls (ppc_runtime.h: dispatch,
// invalid, unwind, switch_stack, resumable; direct calls when the code is
// linked statically, through the runtime's table in a game library).
// A generated function is `void name(xf::ppc::Context &c)`; each
// instruction is one line:
//
//   I(lwz, 0x81430008);        // ops::run<Opcode::lwz>(c, Instr{...})
//   if (BC(12, 26)) goto L_822D28E4;
//   XF_TAIL(xf_822D2910);      // a tail call, no frame left behind

#pragma once

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_ops.h"
#include "xenoforge/ppc_runtime.h"

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
// A function with resume sites (docs/recompiler.md, "Stack switches"): its
// body is a function of its own the runtime runs, from the entry and again
// from a resume site an Unwind came back to (ppc_runtime.h's resumable):
//
//   XF_FN(xf_82000B20) {
//       static constexpr uint32_t xf_sites[] = {0x82000B4Cu, 0x82000B54u};
//       ::xf::ppc::resumable(c, XF_RESUMABLE_BODY {
//           switch (xf_at) { case 0x82000B4C: goto L_82000B4C; ... }
//           ...
//       }, uint32_t(c.lr), uint32_t(c.r[1]), xf_sites, 2);
//   }
#define XF_RESUMABLE_BODY                                                                                              \
    [](::xf::ppc::Context &c, [[maybe_unused]] uint32_t xf_at, [[maybe_unused]] uint32_t xf_lr,                        \
       [[maybe_unused]] uint32_t xf_sp)
