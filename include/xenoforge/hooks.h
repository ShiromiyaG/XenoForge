// The runtime's side of the port's hooks (xenoforge/port.h; docs/recompiler.md,
// "Replacing and hooking"): the registry by guest address, the hook slots of
// the generated code bound to it, and what runs a hooked function or a
// mid-function hook. The kernel's dispatch, the interpreter (through
// ppc::System) and the generated code (ppc_runtime.h's hooked and mid_hook)
// all come here, so every path runs the same hooks.
//
// The registry is the process's: a port registers before main (static
// registrations) or while the game runs; the slots of every module whose
// code the kernel takes in are bound as it does (bind), and rebound when a
// hook at their address is registered or removed. What a slot holds is an
// Entry that never changes once published (a registration publishes a new
// one; the old ones are kept, as a thread may still run them).
//
// For the debugging side (docs/runtime.md, "The port's hooks"): each hook a
// thread runs is a frame on that guest context's list of hook frames, which
// the crash and stop reports put between the guest frames; each hook may be
// counted (set_stats); and every hook belongs to an owner, the executable (0)
// or a module loaded at run time (port_module.h), so a module's hooks are
// dropped together and its code is unloaded only once no thread runs it.

#pragma once

#include "xenoforge/port.h"
#include "xenoforge/ppc_module.h"

#include <atomic>
#include <string>
#include <vector>

namespace xf::hooks {

enum class Kind : uint8_t { Before, After, Replace, Mid };
const char *kind_name(Kind kind); // "before", "after", "replace", "mid"

// A hook's calls, counted while set_stats is on. One per address, kind and
// hook name, kept across registrations (a module reloaded counts on).
struct Counter {
    std::atomic<uint64_t> calls{0};
};

// Who registered a hook: the executable (owner 0) or a module loaded at run
// time. Its code is in use while `active` is above 0.
struct Owner {
    uint32_t id = 0;
    std::string name;
    std::atomic<int64_t> active{0};
};

template <class F> struct Hook {
    F fn = nullptr;
    std::string name; // the hook's own (XF_BEFORE's second argument), else empty
    Owner *owner = nullptr;
    Counter *counter = nullptr;
};

struct Entry {
    uint32_t address = 0;
    std::string name; // the function's, for the reports
    Hook<port::Replacement> replace;
    std::vector<Hook<port::Callback>> before, after;
    std::vector<Hook<port::MidHook>> mid;
    std::vector<Owner *> owners; // the run-time modules with code here (pinned while it runs)
    bool function() const { return replace.fn || !before.empty() || !after.empty(); }
};

// A bit per 4 KB page of the guest's addresses holding a hook (ppc::System's
// hook_pages): what the interpreter and dispatch test before looking further.
const uint8_t *pages();
inline bool in_hooked_page(uint32_t address) { return (pages()[address >> 15] >> ((address >> 12) & 7)) & 1; }

// The hooks at an address (nullptr: none). The entry stays valid, but a
// module's code it points at may be unloaded: running it takes a Pin.
const Entry *find(uint32_t address);
// whether a bound module's generated code reaches the function at an address
// through a hook slot (then its own code runs its hooks)
bool slotted(uint32_t address);
// the size of the hooked function at an address, from its slot (0: unknown)
uint32_t function_size(uint32_t address);

// What keeps a module's code loaded while a thread runs its hooks: made
// before the entry is read (from a slot or by find), then given it; the
// entry's modules stay pinned until the Pin goes.
class Pin {
  public:
    Pin();
    ~Pin();
    Pin(const Pin &) = delete;
    Pin &operator=(const Pin &) = delete;
    const Entry *take(const Entry *e); // e, pinned
    const Entry *find(uint32_t address) { return take(hooks::find(address)); }

  private:
    const Entry *entry_ = nullptr;
    bool resolving_ = true;
};

// A module's slots bound to the registry, and its names taken in (the
// kernel, as the module's code is taken in); unbound as it is dropped.
void bind(const ppc::GeneratedModule &module);
void unbind(const ppc::GeneratedModule &module);

// A hooked function: before, the replacement or `fn` (its body; nullptr: the
// interpreter through System::run_original), after. The entry is pinned.
void run(ppc::Context &c, const Entry &e, void (*fn)(ppc::Context &));
// The mid-function hooks at e's address: 0 to run on, 1 to return, else the
// address to go on at. The entry is pinned.
uint32_t run_mid(ppc::Context &c, const Entry &e);

// ---- the hook frames: what the reports show of the port's code ----

// One hook running on a guest context, innermost first in frames().
struct FrameInfo {
    Kind kind = Kind::Before;
    uint32_t address = 0;     // the hooked function's (a mid hook's: its instruction's)
    std::string function;     // the function's name (the project file's, the port's), else empty
    std::string hook;         // the hook's own name, else empty
    std::string owner;        // its module ("" for the executable)
    const void *fn = nullptr; // its host code
    uint32_t sp = 0, lr = 0;  // r1 and LR as the hook was entered
    bool in_guest = false;    // it called guest code (original(), port::call) that has not returned
};
// The hooks running on a context, innermost first. Read racily for another
// thread's context (a report); exact for the calling thread's.
std::vector<FrameInfo> frames(const ppc::Context &c);
// "before hook count_frames on Camera_Update 0x82012340" (kind, hook, function)
std::string describe(const FrameInfo &f);

// ---- counting (boot's and the player's --hook-stats) ----

// On: each hook's calls are counted (one relaxed increment). Off (the
// default): a load and a branch.
void set_stats(bool on);
bool stats_on();
struct Stat {
    uint32_t address = 0;
    Kind kind = Kind::Before;
    std::string function, hook;
    uint64_t calls = 0;
};
std::vector<Stat> stats();  // by address, then kind
std::string stats_report(); // a line per hook, for the run's report

// ---- owners: the modules loaded at run time (port_module.h) ----

// A new owner; registrations made while it is the registering one
// (set_registering) are its.
Owner *make_owner(const std::string &name);
// The owner the calling thread's registrations belong to from now on
// (nullptr: the executable). Returns the one before.
Owner *set_registering(Owner *owner);
// Every hook an owner registered dropped (new entries published without
// them); the names it gave stay.
void remove_owner(Owner *owner);
// Whether no thread runs the owner's code any more: after remove_owner, true
// once every thread that read an entry with its hooks has left them.
bool drained(Owner *owner, int wait_ms);

} // namespace xf::hooks
