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
`ops::host`, which applies that offset. A 32-bit load or store at the
devices' registers (0x7FC80000 and up) reaches the device that registered
the range (`Kernel::add_mmio`): the GPU's, 0x7FC80000-0x7FC8FFFF, as Xenia
maps them (see "The GPU").

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
`r1 + 0x54 + 8 * (n - 8)`: a 32-bit argument (`arg`) is the word there,
where the caller's `stw` puts it (the games store VdSwap's ninth and tenth
at 0x54 and 0x5C), a 64-bit one or a vararg (`arg64`) the doubleword.
Every call is counted by export; an import with
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
  defaults).
- **Crypto** (`crypt.cpp`): every XeCrypt*/XeKeys* export Xenia has, plus
  SHA-384 and XeKeysConsoleSignatureVerification (it trusts the console's
  own signatures, so the XDK's partition block check takes the normal
  path), with Xenia's guest context layouts, over algorithms written from
  their standards (sha1, sha2, md5, aes, des, bignum; no third-party code).
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

## The GPU

The video driver's side of the GPU, live and headless (plan step A4, "GPU
live"): what the games' Direct3D needs to get through its initialization
and run its frame loop, nothing drawn. Translated from Xenia
(`graphics_system.cc`, `command_processor.cc`,
`pm4_command_processor_implement.h`, `kernel_state.cc`'s
EmulateCPInterruptDPC, `xboxkrnl_video.cc`, `xam_video.cc`); where it
differs is noted at the top of each file.

As built (`command_processor.h`, `src/gpu/command_processor.cpp`;
`src/kernel/video.h`, `video.cpp`):

- **The command processor** executes the ring buffer from its read pointer
  to the CPU's CP_RB_WPTR, wrapping, and the indirect buffers it points at:
  packet types 0-3 and every type-3 opcode Xenia's handles, with their
  effects on the register file (0x5003 registers) and on physical memory:
  fences (MEM_WRITE, EVENT_WRITE_SHD with a value or the vblank counter,
  REG_TO_MEM, COND_WRITE), the scratch registers' write-back, the read
  pointer's write-back after each run, COHER_STATUS_HOST, the gamma ramps,
  occlusion query reports (Xenia's fake ones: no host queries here). No
  kernel in it: memory, and a `Host` for the CPU interrupt and the wait
  between two polls of a WAIT_REG_MEM.
- **A `Sink`** takes what it does not do itself, in order, with the
  register file as it stands: buffers' starts and ends, packets as they
  run, memory read and written, shader loads, draws, resolves (a draw in
  copy mode) and swaps. That is Xenia's backend (IssueDraw, IssueCopy,
  IssueSwap, LoadShader) and its trace writer behind one interface; the
  headless GPU is Xenia's null backend (every draw succeeds, no picture).
- **The registers** answer the CPU's MMIO as Xenia's GraphicsSystem: reads
  from the register file but for the five it answers itself (the EDRAM's
  timing and controls, the vblank status, the 1280x720 viewport); writes
  store, and CP_RB_WPTR wakes the command processor.
- **Two kernel threads** run it, guest threads with a stack, a PCR and a
  KTHREAD in the idle process whose body is host code
  (`Thread::Params::host`, Xenia's XHostThread). "GPU commands" waits on an
  auto-reset kernel event the CP_RB_WPTR writes set, then runs the ring
  buffer. "GPU vblank" ticks 60 times a second on the scheduler's clock,
  paced to a deadline (missed beats made up to a second behind), moving
  D1MODE_V_COUNTER and the counter EVENT_WRITE_SHD can write. Both wait
  through `Objects::wait`, so the deterministic mode will run them as it
  runs the guest's. They start with the first call into the video driver
  and stop with the kernel.
- **The interrupt**: the callback VdSetGraphicsInterruptCallback names runs
  on the thread that raised it (vblank: source 0 on CPU 2; INTERRUPT:
  source 1 on each CPU of its mask), as EmulateCPInterruptDPC: the thread's
  PCR moved to that CPU, at DISPATCH_LEVEL with DPC_ACTIVE set and the
  title's process type in the DPC; the callback with r3 = source, r4 = its
  data on the thread's own stack (`Objects::call` keeps the context); then
  the PCR as it was. Interrupts on one CPU run one at a time.
- **The exports**: the 21 Vd* the games import and xam's XGetVideoMode,
  as Xenia's. The video mode comes from the console's settings, read
  through ExGetXConfigSetting; VdSwap writes the front buffer's fetch
  constant (its address made physical) to fetch constant 0 and Xenia's
  XE_SWAP packet into the 64 words the caller set aside in the ring.

The renderer (B3) replays traces through its own PM4 front end. Driven
live, it becomes the command processor's Sink: shaders from `shader`,
draws and resolves from `draw` and `resolve` with the register file the
processor keeps (its own register tracking and packet parsing go), guest
memory read where it lies (physical memory instead of the trace's copy,
`memory_written` invalidating what it caches), the front buffer from
`swap` and the gamma ramps from the processor. The same Sink with a trace
writer behind it records Xenia's .xtr from a live game.

## `boot`

`boot <game.iso> [--frames N] [--interpret]`: loads the game, runs its
entry point, and prints the kernel calls (counts, unimplemented ones), the
GPU's packets, draws, resolves, swaps (and swaps per second) and
interrupts, and where it
stopped.

## Order

1. Memory (an agent, from Xenia's memory.cc).
2. Kernel core: exports, arguments, counts; the loader (image, imports,
   thunks); the main thread; dispatch; `boot` with the interpreter.
3. What the games call first, family by family (memory, threads and
   synchronization, files on the disc, video and the GPU front end, XAM),
   measured by `boot`.
