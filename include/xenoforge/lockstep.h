// The lockstep check (docs/recompiler.md, "The lockstep check"; docs/plan.md,
// "Debugging"): the interpreter checks the generated code. A call the
// kernel makes to generated code (Kernel::try_call: a dispatch, a thread's
// start, a call interpreted code makes), chosen by address or one in N, runs
// twice from the same registers and memory:
//
// - first the interpreter, on a copy of the context and a private view of
//   the guest memory (Memory::PrivateView: copy on write, so nothing it
//   writes is seen by the game or another thread), every function it calls
//   interpreted too, until the call returns or the code reaches an import;
// - then the generated code, for real, as it always runs.
//
// At the return, or at the import, the two are compared: every register and
// every byte the interpreter stored. An import ends a segment: the import
// runs once, for real, from the generated code's state, and the next
// segment starts from what it left (the interpreter again, then the
// generated code on to the next import or the return). So a call is checked
// whole, its nested calls included, while the kernel sees each import once.
// mftb's reads are the interpreter's, replayed to the generated code.
//
// What the check cannot compare it gives up on, counted: a segment past the
// step limit, an MMIO access, a trap, a stack switch, an unwind, what the
// interpreter cannot run. Another guest thread writing memory the call reads
// between the two runs makes a difference that is not the recompiler's (the
// deterministic mode removes it); a store the generated code makes where the
// interpreter stored nothing is not seen.

#pragma once

#include "xenoforge/ppc_interp.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xf::ppc {

// What an interpreted run shows each instruction to (interpret_observed):
// before it runs, decoded, and after it ran (a call the system ran is
// inside the bl's run).
class Observer {
  public:
    virtual ~Observer() = default;
    virtual void before(Context &c, uint32_t pc, Opcode op, Instr i) = 0;
    virtual void after(Context &c, uint32_t pc, Opcode op) = 0;
};

// interpret() with an observer (the same loop; ppc_interp.cpp)
Stopped interpret_observed(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps, uint32_t stack_top,
                           Observer &observer);

// The bytes an instruction is about to store, [*address, *address + *size),
// from the registers before it runs; false for one that stores nothing. A
// vector store gives its aligned 16 bytes, dcbz and dcbz128 the 128-byte line.
bool store_extent(const Context &c, Opcode op, Instr i, uint32_t *address, uint32_t *size);
// The same for the bytes an instruction is about to load (a vector load: its
// aligned 16 bytes).
bool load_extent(const Context &c, Opcode op, Instr i, uint32_t *address, uint32_t *size);

} // namespace xf::ppc

namespace xf::kernel {

struct LockstepOptions {
    std::vector<uint32_t> functions; // checked on every call the kernel makes to them
    uint32_t sample = 0;             // and one call in this many to any generated function (0: none)
    uint64_t step_limit = 4'000'000; // the interpreter's instructions per segment
    uint32_t print_limit = 20;       // mismatches printed in full (stderr) as they are found
};

struct LockstepCounts {
    uint64_t calls = 0;    // calls checked
    uint64_t segments = 0; // segments compared
    uint64_t matched = 0;  // of them, the same
    uint64_t mismatched = 0;
    // different, but a byte the interpreter read (and did not store) changed
    // meanwhile: another thread's or a device's write, not the recompiler's
    uint64_t raced = 0;
    std::map<std::string, uint64_t> given_up; // segments not compared, by why
    std::map<uint32_t, uint64_t> mismatches;  // by the address the segment started at
};

// XF_LOCKSTEP=<address,...> and XF_LOCKSTEP_SAMPLE=N, as boot's --lockstep and
// --lockstep-sample; false (and the options left) when neither is set or
// one does not parse (why says which).
bool lockstep_options_from_env(LockstepOptions &options, std::string &why);
// "0x82001234,82005678" (hex, the 0x optional)
bool parse_lockstep_functions(const std::string &list, std::vector<uint32_t> &out);

} // namespace xf::kernel
