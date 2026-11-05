# The runtime (design, 2026-11-04)

Plan step A4: the guest's memory, the loader, the kernel and `boot`, so
the recompiled code (and the interpreter) run a game to its title loop
without a picture, every kernel call counted. Xenia (`../xenia-canary`,
BSD) is the reference for what each export does; the structure is written
anew, as lean as the games allow ([lean-kernel.md](lean-kernel.md)).

## Memory

`memory.h`: Xenia's map, one shared-memory section viewed several times
in a 4 GB host range, so a guest address is an offset from the base (plus
0x1000 at 0xE0000000 and above); heaps hand out pages as the kernel's
allocators ask. The generated code and the interpreter reach memory through
`ops::host`, which applies that offset. The GPU's registers (MMIO at
0x7FC80000) come with the GPU's live front end.

## Code: generated, interpreted, imported

One rule for every call the code cannot make directly (`dispatch`): the
address is looked up in the generated functions' table; an import thunk
runs its export; anything else runs in the interpreter until it returns.
The interpreter's own `bl` to an address with a generated function calls
it. So a game runs with all its code generated, with none (bring-up, before
the 3-minute build), or with a title update's changed functions
interpreted.

The loader rewrites each import thunk (16 bytes) to `sc` and `blr`: the
interpreter reaches the export through `sc` (the kernel looks the thunk up
by the address), generated code through `dispatch`. Variable imports
receive the address of the kernel's object for them.

## Exports

`kernel.h`: an export is a function of the kernel and the calling thread's
context, registered by library and ordinal:

```cpp
XF_EXPORT(xboxkrnl, NtAllocateVirtualMemory) {
    uint32_t base_ptr = arg(c, 0), size_ptr = arg(c, 1);
    ...
    result(c, X_STATUS_SUCCESS);
}
```

Arguments as Xenia's shims read them: r3-r10, then the stack at
`r1 + 0x54 + 8 * (n - 8)`. Every call is counted by export; an import with
no implementation logs once, counts, and returns 0, and `boot` prints both
lists: what the game calls is what gets written next.

## Modules, memory, Rtl

- **The guest globals** (`modules.cpp`, Xenia's KernelGuestGlobals and
  kernel_state.cc) are set up as the kernel starts (`setup_kernel`): the
  ten object types, the default object, the idle, title and system
  processes, the kernel modules (xboxkrnl.exe, xam.xex with their
  GetProcAddress trampolines) and the variables at Xenia's addresses
  (KeTimeStampBundle at 0x80240EE0, XboxHardwareInfo, the Vd* ones...).
  The load (`setup_modules`) adds the executable's module, its loader
  entry and header, and the title process's TLS and stack size. The
  objects and threads take the processes and object types from there.
- **Memory exports** (`memory_exports.cpp`): NtAllocateVirtualMemory and
  the Mm*/Ex* pools on the heaps of memory.h, with Xenia's addresses.
- **Rtl and misc** (`rtl.cpp`, `misc.cpp`): strings, time fields, status
  codes (Xenia's table), the printf family, XConfig settings (Xenia's
  defaults), SHA-1.
- **One clock**: the scheduler's (100 ns since the kernel started). The
  interrupt time is it, the system time is it plus the host's time at the
  start, the time base (mftb) is it times 5 (50 MHz); KeTimeStampBundle
  moves with it every millisecond once threads run. On Windows the host
  timer is raised to its finest (0.5 ms), as Xenia does, or the clock and
  short waits would step at 15.6 ms.

## Threads and objects

Each guest thread is a host thread with its own context, a guest stack, a
PCR (r13) and TLS as Xenia's XThread sets them up. Kernel objects (events,
semaphores, mutants, timers, threads) live in guest memory where the game
put them (KEVENT and friends) and are backed by host objects found by the
guest address, as Xenia's XObject does. The deterministic mode (fibers on
one host thread, plan "Threads") comes on the same interface later.

As built (`src/kernel/objects.h`; objects.cpp, threads.cpp, sync.cpp):

- **Handles** as Xenia's (0xF8000000 + slot * 4, -2 the current thread),
  names case-insensitive. A dispatcher header the kernel has seen holds
  Xenia's stash (`'XEN\0'` and the handle in its wait list), so the host
  object is found from the guest pointer, and one the code initialized
  itself (the XDK inlines KeInitializeEvent) gets one on first use.
- **The dispatcher is NT's, not the host's waits**: one lock over every
  object; the signal state is the guest header's own (the game reads it,
  and what it writes there counts); a signal satisfies the waits it can,
  oldest first (KiWaitTest), taking what they acquire (auto-reset events,
  semaphore counts, mutant ownership); wait-any, wait-all, timeouts
  relative and absolute, abandoned mutants, alertable waits.
- **The scheduler** is an interface of seven calls (start, join, block
  until woken or a deadline, wake, yield, the clock, timers changed). The
  host scheduler gives each guest thread a host thread with a 16 MB stack
  and a clock thread (timers, KeTimeStampBundle, every millisecond); the
  time base (mftb) is the same clock. The fiber scheduler replaces it.
- **Safe points**: a thread suspended by another, or with kernel APCs
  queued, stops at its next kernel call (Xenia suspends the host thread
  wherever it is). User APCs run in alertable waits; DPCs run at once on
  the calling thread, as in Xenia. ExTerminateThread unwinds the host
  stack to the thread's start (a C++ exception).
- `Kernel::launch` makes the main thread (the executable's stack size and
  TLS header) and starts it; `boot` waits for it and reports the threads.

## `boot`

`boot <game.iso> [--frames N] [--interpret]`: loads the game, runs its
entry point, and prints the kernel calls (counts, unimplemented ones), the
GPU's packets and swaps (the trace front end counting), and where it
stopped.

## Order

1. Memory (an agent, from Xenia's memory.cc).
2. Kernel core: exports, arguments, counts; the loader (image, imports,
   thunks); the main thread; dispatch; `boot` with the interpreter.
3. What the games call first, family by family (memory, threads and
   synchronization, files on the disc, video and the GPU front end, XAM),
   measured by `boot`.
