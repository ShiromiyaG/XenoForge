// The copy-and-patch tier (docs/recompiler.md, "A copy-and-patch first tier"):
// a module's functions made into host code at run time, in a fraction of a
// second, out of the stencils clang compiled from the same semantics when
// XenoForge was built (src/stencil/stencils.cpp, tools/xfstencil.cpp). It sits
// between the interpreter and the game library: what it makes is a module's
// table (ppc_module.h), given to the kernel as a game library's is, so calls
// and returns go through the same dispatch, and whatever it leaves out runs in
// the interpreter. Off unless XF_TIER=stencil.

#pragma once

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_runtime.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace xf::recomp {
struct Image;
struct Discovery;
} // namespace xf::recomp

namespace xf::stencil {

struct Options {
    // Count the guest instructions its code runs (a stencil before each one:
    // slower, for measuring coverage; Module::executed). Not atomic: exact on
    // the deterministic mode's one thread.
    bool count = false;
};

struct Stats {
    size_t functions = 0, compiled = 0;                   // discovered (not imports), and given code
    uint64_t instructions = 0, compiled_instructions = 0; // in them
    uint64_t copies = 0;                                  // stencils copied
    size_t code_bytes = 0, region_bytes = 0;              // the functions' code, and all the tier holds
    size_t unwind_entries = 0;                            // the host's unwind table's (Windows)
    double seconds = 0;                                   // the compile, discovery not counted
    std::map<std::string, size_t> left_out;               // why functions were left to the interpreter
};

class Module {
  public:
    virtual ~Module() = default;
    // the table the kernel is given (its functions' fn: their entries here)
    virtual const ppc::GeneratedModule *table() const = 0;
    // the guest instructions its code ran (Options::count; 0 without it)
    virtual uint64_t executed() const = 0;
    // the same by opcode (ppc::OPCODE_COUNT counters; nullptr without Options::count)
    virtual const uint64_t *executed_by_opcode() const = 0;
};

// Whether this build has stencils this host can run (why not, when not).
bool available(std::string *why = nullptr);
// XF_TIER=stencil
bool requested();

// The functions discovery found in `image` made into code, those it can (the
// others left to dispatch, which interprets them). `runtime` is the table the
// code calls (ppc_runtime.h's, as a game library gets it), copied. nullptr,
// and why, when the tier is not available.
std::unique_ptr<Module> compile(const recomp::Image &image, const recomp::Discovery &d, const ppc::Runtime &runtime,
                                const std::string &name, uint64_t digest, const Options &options, Stats &stats,
                                std::string &error);

} // namespace xf::stencil
