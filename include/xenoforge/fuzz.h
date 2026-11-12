// The differential fuzzer (docs/recompiler.md, "The differential fuzzer"):
// random instruction sequences over every form the decoder knows, with edge
// operands (NaN payloads, denormals, infinities, saturation, carries and
// overflows, FPSCR/VSCR/XER/CR state), run by the interpreter, the generated
// code (tests/ppc_fuzz_test.cpp) and Xenia's JIT (tools/xenia_oracle/
// ppc_exec_oracle.cc), their states compared field by field. The file format is
// fuzz_format.h; the command line is xffuzz (tools/xffuzz.cpp).

#pragma once

#include "xenoforge/guest_space.h"
#include "xenoforge/ppc.h"
#include "xenoforge/ppc_context.h"
#include "xenoforge/fuzz_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xf::ppc::fuzz {

struct Options {
    uint64_t seed = 1;
    uint32_t per_op = 16;          // cases whose focus is each instruction
    std::vector<std::string> only; // focus only on these instructions (names); all when empty
    uint32_t single_percent = 60;  // cases of the focus instruction alone
};

// Whether the generator makes the instruction (branches, sc, traps, mftb and
// the MSR writes are left to the emitter's and kernel's own tests).
bool generated(Opcode op);
// Cases: per_op for each instruction, its focus first in a sequence of one to
// four (with now and then a forward bc reading CR or CTR).
std::vector<Case> generate(const Options &options);
// The instruction a case is about: its first non-branch word's.
Opcode focus(const Case &c);

void load_state(Context &c, const State &s, uint8_t *base);
void save_state(const Context &c, State &s, const uint8_t *base);

// Runs cases with the guest memory they need (the window, the code).
class Runner {
public:
    explicit Runner(const std::vector<Case> &cases);
    bool ok() const { return ok_; }
    // the interpreter, or `fn` (generated code) when given
    Result run(size_t index, void (*fn)(Context &) = nullptr);
    uint8_t *base() const { return space_.base(); }

private:
    const std::vector<Case> &cases_;
    GuestSpace space_;
    System system_;
    bool ok_ = false;
};

// The fields that differ between two states, one entry each: "r5", "f3",
// "v100", "cr2", "fpscr.FX" (a bit by its name), "xer.ca", "vscr.sat",
// "vscr.nj", "ctr", "lr", "msr", "memory".
std::vector<std::string> differences(const State &a, const State &b);
// A field's value in a state, for the report ("0x...", "[...]").
std::string field_value(const State &s, const std::string &field);

} // namespace xf::ppc::fuzz
