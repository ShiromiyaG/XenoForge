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
    enum Reason { Returned, Unimplemented, Invalid, Steps } reason = Returned;
    uint32_t pc = 0;
    Opcode op = Opcode::Invalid; // what it could not run
};

// Runs from pc until a branch goes to `until` (a return address no code is
// at), at most max_steps instructions.
Stopped interpret(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps = 100'000'000);

// Whether ops::run has the instruction (the primary template otherwise).
bool implemented(Opcode op);

} // namespace xf::ppc
