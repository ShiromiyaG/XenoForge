// How discovered functions keep the guest's calling convention (xfrecomp
// analyze --calls; docs/recompiler.md, "Trusting the calling convention,
// weighed"): which could hold their non-volatile registers in C locals.

#pragma once

#include "xenoforge/recomp.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace xf::recomp {

// Non-volatile registers: r14-r31, f14-f31, v14-v31 and v64-v127, a bit each
// at the register's number
struct NonVolatileSet {
    uint32_t gpr = 0, fpr = 0;
    uint64_t vr[2] = {};
    bool empty() const { return !gpr && !fpr && !vr[0] && !vr[1]; }
    void add(const NonVolatileSet &o) { gpr |= o.gpr, fpr |= o.fpr, vr[0] |= o.vr[0], vr[1] |= o.vr[1]; }
    bool within(const NonVolatileSet &o) const {
        return !(gpr & ~o.gpr) && !(fpr & ~o.fpr) && !(vr[0] & ~o.vr[0]) && !(vr[1] & ~o.vr[1]);
    }
    size_t count() const;
};

enum class Convention : uint8_t {
    Keeps,         // saves and restores every non-volatile register it names
    NoNonVolatile, // names none
    Resumes,       // has resume sites
    SwitchesStack, // switches the stack
    SavesStack,    // keeps r1 for a later return (setjmp)
    ExceptionData, // .pdata says it has an exception handler
    Multiple,      // lmw, stmw, lswi and the like
    NotSaved,      // names a non-volatile register it does not save or restore
    Skipped,       // a helper or an import
};
const char *convention_name(Convention c);

struct CallingConvention {
    Convention kind = Convention::Skipped;
    size_t instructions = 0;
    NonVolatileSet named, saved, restored;
};

// Each function's (by address) as discovery found it
std::map<uint32_t, CallingConvention> analyze_calling_convention(const Image &image, const Discovery &d);
// The instructions the emitter emits for a function (recomp_emit.cpp's walk)
std::set<uint32_t> function_instructions(const Image &image, const Discovery &d, const Function &f);
// The non-volatile registers each instruction names
std::vector<NonVolatileSet> named_non_volatile(const Image &image, const std::vector<uint32_t> &addresses);

} // namespace xf::recomp
