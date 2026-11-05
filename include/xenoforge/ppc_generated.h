// What the generated code includes (docs/recompiler.md): the ops, the
// branch conditions, and the two things whoever links the code provides.
// A generated function is `void name(xf::ppc::Context &c)`; each
// instruction is one line:
//
//   I(lwz, 0x81430008);        // ops::run<Opcode::lwz>(c, Instr{...})
//   if (BC(12, 26)) goto L_822D28E4;
//   XF_TAIL(xf_822D2910);      // a tail call, no frame left behind

#pragma once

#include "xenoforge/ppc_ops.h"

namespace xf::ppc {

// An indirect call or jump, or a call to code with no generated function:
// the function at `target`, else the interpreter from there until it
// returns. LR is already set as the code set it.
void dispatch(Context &c, uint32_t target);
// An invalid instruction reached.
void invalid(Context &c, uint32_t pc);

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
