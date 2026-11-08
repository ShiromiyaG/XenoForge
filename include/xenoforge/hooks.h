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

#pragma once

#include "xenoforge/port.h"
#include "xenoforge/ppc_module.h"

#include <string>
#include <vector>

namespace xf::hooks {

struct Entry {
    uint32_t address = 0;
    std::string name;
    port::Replacement replace = nullptr;
    std::vector<port::Callback> before, after;
    std::vector<port::MidHook> mid;
    bool function() const { return replace || !before.empty() || !after.empty(); }
};

// A bit per 4 KB page of the guest's addresses holding a hook (ppc::System's
// hook_pages): what the interpreter and dispatch test before looking further.
const uint8_t *pages();
inline bool in_hooked_page(uint32_t address) { return (pages()[address >> 15] >> ((address >> 12) & 7)) & 1; }

// the hooks at an address (nullptr: none)
const Entry *find(uint32_t address);
// whether a bound module's generated code reaches the function at an address
// through a hook slot (then its own code runs its hooks)
bool slotted(uint32_t address);
// the size of the hooked function at an address, from its slot (0: unknown)
uint32_t function_size(uint32_t address);

// A module's slots bound to the registry, and its names taken in (the
// kernel, as the module's code is taken in); unbound as it is dropped.
void bind(const ppc::GeneratedModule &module);
void unbind(const ppc::GeneratedModule &module);

// A hooked function: before, the replacement or `fn` (its body; nullptr: the
// interpreter through System::run_original), after.
void run(ppc::Context &c, const Entry &e, void (*fn)(ppc::Context &));
// The mid-function hooks at e's address: 0 to run on, 1 to return, else the
// address to go on at.
uint32_t run_mid(ppc::Context &c, const Entry &e);

} // namespace xf::hooks
