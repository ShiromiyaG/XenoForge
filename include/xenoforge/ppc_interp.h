// The PPC interpreter (plan step A3): decodes each word and runs it through
// the same ops::run the generated code calls (ppc_ops.h); branches are its
// own. For what has no generated code (a title update's changed functions,
// a function with a guest breakpoint) and for the instruction tests.

#pragma once

#include "xenoforge/ppc.h"
#include "xenoforge/ppc_context.h"

#include <cstdint>

namespace xf::ppc {

struct Stopped {
    // Unwound: a blr left the frames this run started in (r1 above
    // stack_top) for another return address than `until` (longjmp to a
    // frame further up); pc is where it went, the registers are as it left
    // them
    enum Reason { Returned, Unimplemented, Invalid, Steps, Unwound } reason = Returned;
    uint32_t pc = 0;
    Opcode op = Opcode::Invalid; // what it could not run
    uint64_t steps = 0;          // the instructions it ran (not those of the calls the system ran)
};

// Runs from pc until a branch goes to `until` (a return address no code is
// at), at most max_steps instructions. With a stack_top (r1 where the call
// being run was made), the return counts only with r1 back at or above it
// (a deeper frame returning to the same address is a recursive call's), and
// a blr elsewhere with r1 above it stops as Unwound.
Stopped interpret(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps = 100'000'000, uint32_t stack_top = 0);

// Whether ops::run has the instruction (the primary template otherwise).
bool implemented(Opcode op);

} // namespace xf::ppc
