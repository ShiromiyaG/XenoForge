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
#include <utility>
#include <vector>

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
    // Keep where each stencil's copy is, so Module::describe names the
    // stencil an address is in (for a sampling profile; about 8 bytes a copy).
    bool map = false;
};

// A function given code, and how hot it ran (Module::heat): what the
// recompile step builds first (docs/runtime.md, "Hot functions first").
struct FunctionHeat {
    uint32_t address = 0, instructions = 0;
    uint64_t entries = 0;       // its entries in this tier's code (not the jumps to its own start)
    uint64_t back_edges = 0;    // its loops' back edges taken (each a yield point)
    bool adopted = false;       // its entry jumps to a game library's code now
    uint64_t library_calls = 0; // since: the calls its adapter made to the library's code
    bool released = false;      // its code released (Module::release)
};

// What Module::release did.
struct ReleaseStats {
    size_t released = 0;       // functions whose code was released now
    size_t live = 0;           // adopted, but a frame is in them: kept
    size_t not_adopted = 0;    // the library has no code for them: kept
    size_t bytes = 0;          // the memory released now (whole pages)
    size_t unwind_dropped = 0; // their unwind entries dropped
};

struct Stats {
    size_t functions = 0, compiled = 0;                   // discovered (not imports), and given code
    uint64_t instructions = 0, compiled_instructions = 0; // in them
    uint64_t copies = 0;                                  // stencils copied
    size_t code_bytes = 0, region_bytes = 0;              // the functions' code, and all the tier holds
    size_t unwind_entries = 0;                            // the host's unwind table's (Windows)
    size_t checked = 0, resumable = 0, hooked = 0;        // functions with checked returns, resume sites, a slot
    size_t mid_hooks = 0;                                 // mid-function hooks placed
    uint64_t variants = 0;                                // instructions given a variant (docs/recompiler.md)
    uint64_t fused = 0;                                   // compares fused with the bc after them
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
    // while every guest thread is parked (Kernel::pause). A function adopted
    // before (from a partial library: docs/runtime.md, "Hot functions first")
    // is pointed at `library`'s code. How many functions moved or were
    // pointed again; 0, and why, when none could.
    virtual size_t adopt(const ppc::GeneratedModule &library, std::string &error) = 0;
    // Each function's heat, by guest address.
    virtual std::vector<FunctionHeat> heat() const = 0;
    // Options::count: the host code run a guest instruction, the stencils that
    // ran most, the shared helpers' calls (the slow paths); with a file, every
    // stencil's runs and bytes, the helpers' calls and the opcodes' counts
    // written there. Empty without Options::count.
    virtual std::string count_report(size_t top, const std::string &pieces_file) const = 0;
    // The stencil an address of this tier's code is in (Options::map), else its
    // function ("tier 0x82..."); false outside this tier's memory.
    virtual bool describe(uint64_t address, std::string &name, uint64_t &displacement) const = 0;
    // [begin, end) of the code (the shared pieces, the entries, the functions).
    virtual std::pair<uintptr_t, uintptr_t> code_range() const = 0;
    // After a tier-up: the code of the functions adopted that no frame is in
    // released (`live`: every value found on the guest threads' host stacks
    // at a pause, docs/runtime.md, "Releasing the first tier"). Only while
    // every guest thread is parked. The bytes released now.
    virtual size_t release(const std::vector<uintptr_t> &live, ReleaseStats &stats, std::string &error) = 0;
    // the memory this tier holds committed (code, data, adapters)
    virtual size_t committed_bytes() const = 0;
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
