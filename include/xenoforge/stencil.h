// The copy-and-patch tier (docs/recompiler.md, "A copy-and-patch first tier"):
// a module's functions made into host code at run time, in a fraction of a
// second, out of the stencils clang compiled from the same semantics when
// XenoForge was built (src/stencil/stencils.cpp, tools/xfstencil.cpp). It sits
// between the interpreter and the game library: what it makes is a module's
// table (ppc_module.h), given to the kernel as a game library's is, so calls
// and returns go through the same dispatch, and whatever it leaves out runs in
// the interpreter. The generic boot and player run it when the cache has no
// game library for the disc (docs/runtime.md, "The first tier"), and move to
// the library when one appears (Module::adopt).

#pragma once

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_runtime.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace xf::recomp {
struct Image;
struct Discovery;
struct DiscoverOptions;
struct EmitHooks;
struct ProjectModule;
} // namespace xf::recomp

namespace xf::stencil {

// What a port's project file asks of one module, for the tier as
// recomp_module.cpp's plan and apply_project make it for the emitter: the
// hooked functions' and mid-function hooks' places, and the functions it
// skips (left to the interpreter).
struct Project {
    std::map<uint32_t, recomp::EmitHooks> hooks; // by function
    std::set<uint32_t> skip;
};
// The project's starts, hooked functions and jump tables, for discovery.
void project_discovery(const recomp::ProjectModule &pm, recomp::DiscoverOptions &options);
// The project's hooks placed in what discovery found.
Project project_hooks(const recomp::ProjectModule &pm, const recomp::Discovery &d);

struct Options {
    // Count the guest instructions its code runs (a stencil before each one:
    // slower, for measuring coverage; Module::executed). Not atomic: exact on
    // the deterministic mode's one thread.
    bool count = false;
    // the port's hooks and skips (nullptr: none)
    const Project *project = nullptr;
};

struct Stats {
    size_t functions = 0, compiled = 0;                   // discovered (not imports), and given code
    uint64_t instructions = 0, compiled_instructions = 0; // in them
    uint64_t copies = 0;                                  // stencils copied
    size_t code_bytes = 0, region_bytes = 0;              // the functions' code, and all the tier holds
    size_t unwind_entries = 0;                            // the host's unwind table's (Windows)
    size_t checked = 0, resumable = 0, hooked = 0;        // functions with checked returns, resume sites, a slot
    size_t mid_hooks = 0;                                 // mid-function hooks placed
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
    // The tier-up (docs/recompiler.md, "Tier-up"): each function `library`
    // (the game library's table for the same image) has code for gets, at its
    // entry here, a jump to that code, so the calls this tier's code makes go
    // there from now on; frames already in this tier's code finish here. Only
    // while every guest thread is parked (Kernel::pause). How many functions
    // moved; 0, and why, when none could.
    virtual size_t adopt(const ppc::GeneratedModule &library, std::string &error) = 0;
};

// Whether this build has stencils this host can run (why not, when not).
bool available(std::string *why = nullptr);

// XF_TIER: which code runs a game that has no code linked in.
enum class Choice {
    Default,   // unset: the game library when the cache has one, else this tier, moving to the library when it appears
    Stencil,   // "stencil": this tier even when a library exists, and no tier-up (for tests)
    Library,   // "library": the library or, without one, the interpreter (the behavior before the tier)
    Interpret, // "interpret": the interpreter (as --interpret)
};
Choice choice();
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
