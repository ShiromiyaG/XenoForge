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
the range (`Kernel::add_mmio`): the GPU's, 0x7FC80000-0x7FC8FFFF, and the
XMA decoder's, 0x7FEA0000-0x7FEAFFFF, as Xenia maps them (see "The GPU"
and "Sound").

## Code: generated, interpreted, imported

One rule for every call the code cannot make directly (`dispatch`): the
address is looked up in the generated code of the module that holds it; an
import thunk runs its export; anything else runs in the interpreter until
it returns. The interpreter's own `bl` to an address with a generated
function calls it. So a game runs with all its code generated, with none
(bring-up, before the build), or with a module whose image is not the one
its code was made from (a title update, another version) interpreted.

The generated code is per module (`ppc_module.h`; docs/recompiler.md,
"Modules"): the game library carries a table for each executable and DLL on
the disc (its path, base, size, the digest of its image and its functions),
handed to the kernel before the load (`Kernel::set_generated_modules`).
When an image is placed at its base (the executable in `load`, a DLL or a
second executable as XexLoadImage loads it), the kernel looks for the table
with the same base and size and, only then, digests the image as unpacked
(`image_digest`, before the imports are bound; a few milliseconds for
30 MB) and compares (`add_module_code`); the module's report line says
which: "generated code, N functions", or why it runs interpreted. A table
that matches gets the module's range and a slot per instruction from its
first function to its last ((address - first) / 4: 36 MB for El Chavo
Kart's 4.7M-instruction range, made the first time the image loads and
kept). Dispatch reads one immutable map of those ranges (an atomic
pointer, a new map published at each load and unload, the old ones kept
for threads still reading them): one compare per module loaded and one
load, about 2 ns, where the binary search over one table it replaced took
29 to 79 ns on El Chavo Kart's 73,293 functions. XexUnloadImage drops the range with the pages,
so BC2's Splash.xex and Game.xex, one after the other at 0x82A00000, each
run their own code. Functions given with `set_generated` (any address, the
tests' host functions) are looked up first.

Returns to another frame (a longjmp, a C++ catch's continuation, a stack
switched back to elsewhere than where it left) are thrown as `ppc::Unwind`
(a C++ exception, through the host frames, as ExitThread's is): by the
generated code of a function that switches the stack when a return is not
to its caller (docs/recompiler.md, "Stack switches"), and by a dispatch
whose interpreter takes a `blr` elsewhere with r1 above the stack its call
was made on (r1, its "stack top"; `Stopped::Unwound`). Each frame on the
way knows the r1 it was entered with, and the innermost one entered above
the restored r1 holds the guest's frame there: a generated function with a
resume site at the target goes on there, in its own frame; a dispatch
interprets on from the target (a function's start there runs generated):
the rest of the functions between, interpreted (their generated frames are
gone, and the generated code keeps nothing outside the context, so the
interpreter takes over exactly), until the guest returns to that dispatch's
return address with r1 back at its stack top, and the generated code after
it goes on. A return to that address with r1 below it is a recursive
call's, not this one's. A dispatch whose stack top is the restored r1 lets
the return through to its caller's frame when the target is a resume site
of the generated code (`Kernel::resumable`, from each module's table). A
fiber switch is no unwind: each guest stack runs on a host stack of its own
("Stacks" below). With r1 above every frame of a call the kernel made (a
longjmp out of a callback), the call interprets on until the guest returns
from it. Mid-function jumps the generated code dispatches with a stale LR
(an unresolved table case) end the same way, where before they could
interpret past their function forever.

A game library's import thunk (a DLL's function another module imports:
`lis r11; ori r11` before the thunk's own `mtctr r11; bctr`, written at
load) is known as such to the kernel: dispatch to it sets r11 and CTR as
the thunk would and dispatches to the export, so generated code calling
across modules reaches the other module's generated code without
interpreting the thunk.

The loader rewrites each import thunk (16 bytes) to `sc` and `blr`: the
interpreter reaches the export through `sc` (the kernel looks the thunk up
by the address), generated code through `dispatch`. Variable imports
receive the address of the kernel's object for them.

## Structured exceptions

(2026-11-06, `src/kernel/exceptions.cpp`; docs/xenia-todos-kernel.md A1
and A2.) Xenia logs RtlRaiseException and returns, so a C++ throw or an
`__except` never reaches its handler. The kernel here dispatches as NT's
PowerPC ports do (the handlers' protocol of its RISC design, read in Wine's
ARM64 dispatcher for behavior), with what the console's code fixes read
from the games' own CRT.

- **Raising.** RtlRaiseException (RaiseException, the C++ runtime's throw,
  SetThreadName) and DbgBreakPoint (STATUS_BREAKPOINT, one parameter,
  BREAKPOINT_BREAK, as NT's trap). The record's address becomes the call's
  return address; the context is the caller's at the call.
- **The layouts.** CONTEXT, 0xA40 bytes: flags, MSR (0x9030), IAR, LR, CTR
  (8 bytes), r0-r31 as doublewords at 0x18, CR, XER, FPSCR (8 bytes), f0-f31
  at 0x128, VSCR at 0x230, v0-v127 at 0x240 (the C++ runtime's jump to a
  catch's continuation loads exactly these, and copies 0xA40 bytes).
  EXCEPTION_RECORD: code, flags, chained record, address, count, 15
  parameters. DISPATCHER_CONTEXT: ControlPc, FunctionEntry (the 8-byte
  `.pdata` entry), EstablisherFrame, ContextRecord (the C++ handler reads
  the handler data at FunctionEntry's begin - 4).
- **Functions.** The image's exception directory (PE data directory 3,
  `.pdata`), looked up by the loaded module holding the address; with the
  exception flag the handler and its data are the two words before the
  function. `add_function_table` (kernel.h, NT's RtlAddFunctionTable) for
  code no image holds: the tests.
- **The virtual unwind.** The prologue (up to the call, if the call is in
  it) run forward over symbols: what each register holds relative to the
  entry's r1, where each entry value was stored. The XDK's forms: `mflr r12`
  then `stw r12, -8(r1)` or `bl __savegprlr_N`; LR kept in a nonvolatile
  (`std r31, -0x48(r1); mflr r31`); `std`/`stw` of nonvolatiles; `addi r12,
  r1, -X` then `__savefpr_N` and `__savevmx_N` (their stores simulated);
  `mfcr` and its store; `stwu r1` or `stwux` by a size in a register (else the
  back chain); a frame pointer in a nonvolatile (`addi r31, r1, -0xA0` before
  the `stwu`, or `mr r31, r1` after), which gives the entry's r1 past an
  alloca. The establisher frame is r1 at the function's entry (the games'
  `_local_unwind(r31 + frame size, target)`). A frame's ControlPc is its
  call (the return address - 4): a scope's range holds the call, not the
  return (SetThreadName's `__try` ends right after its `bl`). A handler runs
  only past its function's prologue (NT's InFunction).
- **The search** (RtlDispatchException): the frames from the raise up, each
  handler called with the record, its establisher frame, the raise's
  CONTEXT and a DISPATCHER_CONTEXT. ContinueExecution goes on with the
  CONTEXT as the handler left it (a noncontinuable exception raises
  STATUS_NONCONTINUABLE_EXCEPTION over it instead, the original chained),
  ContinueSearch goes on up, another answer raises
  STATUS_INVALID_DISPOSITION.
- **The unwind** (RtlUnwind(target frame, target IP, record, value)): the
  frames from RtlUnwind's caller up to the target frame (all of them
  without one: EXCEPTION_EXIT_UNWIND), each handler called with
  EXCEPTION_UNWINDING and the frame's own context (EXCEPTION_TARGET_UNWIND
  at the target); then on at the target IP in the target frame's context
  as its handler left it, r3 the value. Without a record it makes one,
  STATUS_UNWIND.
- **`__C_specific_handler`** (the kernel's export; the scope table, a count
  then begin, end, filter and target per scope, inner scopes first, is the
  handler data): searching, an `__except` whose range holds ControlPc has
  its filter run (EXCEPTION_POINTERS in r3, the frame in r4 and r12, as the
  funclets read it); positive unwinds to its block with the code as the
  value, negative continues. Unwinding, each `__finally` whose range holds
  ControlPc runs (TRUE in r3), ControlPc moved past the scope first; in the
  target frame the scope that holds the target ends it.
- **C++** is the game's own CRT (`__CxxFrameHandler`, from FuncInfo). What it
  asks of the kernel: searching, it captures its context (RtlCaptureContext,
  which leaves r3 alone: the CRT passes it on after the call), points its
  IAR at its own return point, and RtlUnwinds to the catching frame with
  that target; its handler, called for the target frame, keeps the frame's
  context and swaps the captured one in, so the unwind comes back into the
  CRT's own frame; it runs the catch funclet (frame in r12) and jumps to the
  continuation the funclet returns by loading the frame's context and a
  `blr` (a function that switches the stack: docs/recompiler.md, "Stack
  switches").
- **Handlers are guest calls** on a frame below the raise's (its back chain
  to it), returning to `HANDLER_RETURN` (0xBCBCBCB0), the registers around
  them put back. A walk that reaches that return is in the frames of a
  dispatch or unwind in progress on the thread, and goes on from the code
  that started that one, as NT's walks go through the frames of its
  dispatcher: an exception raised inside a handler or a catch funclet (a
  rethrow) gets EXCEPTION_NESTED_CALL up to the frame whose handler was
  running; an unwind passes a dispatch's frames on to the raise (the C++
  runtime's unwind from inside its handler); an unwind that reaches another
  unwind's handler takes that frame again from the ControlPc its
  DISPATCHER_CONTEXT holds (EXCEPTION_COLLIDED_UNWIND).
- **Going on.** Execution goes on with a context: back from the call when
  that is where it points (a continued exception, the C++ runtime's unwind
  into its own frame); anywhere else a `ppc::Unwind` to the target, as a
  longjmp ("Code" above): a generated function with a resume site there
  (discovery makes each `__except` block one, beside the catch
  continuations and `_local_unwind` targets lis/addi pairs make), else a
  dispatch that interprets from there.
- **No handler.** A log of the exception and the frames walked, then
  KeBugCheckEx(KMODE_EXCEPTION_NOT_HANDLED, code, address, the first two
  parameters): NT's kernel-mode answer, as titles run in supervisor state
  (MSR 0x9030 has no PR). The XAPI's own top-level filter (mainCRTStartup's
  `__except`) answers continue-search when no debugger is attached, so this
  is where an exception a title does not catch ends. SetThreadName's
  (0x406D1388) alone goes on silently when nothing takes it: it is a
  debugger's notification (the XDK's SetThreadName wraps it in its own
  `__try` anyway). The thread is named first, as a debugger would.

Where it differs from NT, and why: the CONTEXT's flags are NT PowerPC's
CONTEXT_FULL (0x7; the console's value is not known and nothing seen reads
it); a restored CR takes fields 2-4 only (the nonvolatile ones); a frame
with no `.pdata` ends a walk (NT takes it for a leaf, right only for the
innermost frame); the stack's bounds are the PCR's; no debugger's first or
second chance (none is attached); RtlVirtualUnwind, RtlLookupFunctionEntry,
RtlUnwind2 and the vectored handlers are not written, as none of the eight
games imports them. Traps in guest code (`tw`, `twi`) still raise nothing.

`kernel_exceptions_test` runs a guest program written as the XDK's
compiler writes (`tests/seh_program.h`), interpreted and then recompiled
by `seh_gen` at build time: `__except` and its filter (nonvolatiles,
including f31, back in the frame), nested `__try` with `__finally` run on
the unwind in its own frame and in the frame between, a continued
exception, a noncontinuable one continued, DbgBreakPoint, SetThreadName's
exception with no handler, RtlUnwind to its own frame and two frames up
with a value, a C++ throw caught two frames up with a destructor run on the
way, rethrown from the catch and caught one frame further (the C++ handler
is the test's host code, at a guest address with a guest frame and
`.pdata`), and, in a child process, an exception nothing takes stopping the
title. Recompiled, every unwind lands at a resume site: nothing is
interpreted.

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
- **Second executables** (`modules.cpp`, Xenia's LoadUserModule and
  XexModule): XexLoadImage reads a DLL through the file system (a bare
  name next to the executable), places its image at its base, makes its
  module (loader entry, header) and binds its imports as the executable's
  (`Kernel::bind_imports`), loading first the game's libraries it imports
  from; their thunks jump to the exports (lis/ori r11 before the thunk's
  own bctr). DllMain runs on the calling thread with the HMODULE:
  DLL_PROCESS_ATTACH at the load (the libraries it brought first),
  DLL_THREAD_ATTACH and DETACH as guest threads start and end,
  DLL_PROCESS_DETACH when XexUnloadImage takes the last load, which
  releases the pages (BC2 loads Splash.xex and Game.xex at the same base,
  one after the other). Every DllMain call runs under the loader lock
  (NT's), recursive, also held across XexLoadImage's lookup and load,
  XexUnloadImage's detach and removal and XexGetModuleHandle: a load waits
  for an unload in progress, and no DllMain gets a module that is gone.
  boot's module report names the lock's owner when it is held. Each
  module's code is generated or interpreted as its image matches a table
  of the game library ("Code" above).
- **BC2's launcher chain** (`kernel_modules_test`'s `launcher_chain` runs
  the same sequence). Its `default.xex` (Frost.Game.Launcher) is a small
  executable; `Splash.xex` (splash.dll) and `Game.xex` (game.dll) are DLLs
  (module flags 0x9) at 0x82A00000, each exporting one function, ordinal 1,
  its `dllMain`. The launcher loads `GAME:\splash.xex` (XexLoadImage, its
  DllMain attached), looks up ordinal 1 (and ordinal 2, which must be
  absent, else it warns) and calls it with a launcher object of its own (a
  C++ interface on its stack whose methods the DLL calls: a "go on" flag,
  which executable comes next, the index of the pad that pressed START,
  as Splash.xex's code reads); when it returns with the flag set,
  the launcher unloads it and loads `GAME:\game.xex` the same way, and with
  the flag clear it ends with XLaunchNewImage(NULL) (the dashboard). The
  state passed on is that object in memory, not XAM's: Game.xex calls
  XamLoaderGetLaunchData once and goes on with none (X_ERROR_NOT_FOUND, a
  cold boot's answer). Splash.xex plays its movies and waits on its "press
  START" screen (START or A on any pad moves on, as on the console), so an
  idle pad keeps it there for good; `--press start@20 --press start@30
  ...` takes the chain to Game.xex, which loads (about 15 s on its main
  thread, no swaps) and runs its menus (2026-11-06: 30 swaps a second,
  5,700 draws a second, its single-player menu reached with presses).
  Booted alone (`--xex Game.xex`), its entry point is the CRT's DllMain,
  which returns: the main thread ends at once.
- **Memory exports** (`memory_exports.cpp`): NtAllocateVirtualMemory and
  the Mm*/Ex* pools on the heaps of memory.h, with Xenia's addresses.
- **Rtl and misc** (`rtl.cpp`, `misc.cpp`): strings, time fields, status
  codes (Xenia's table), the printf family (narrow and wide), CRC-32, PE
  image headers, XConfig settings (Xenia's defaults, then what the title
  writes, for the kernel's life), the HAL's system management controller
  (Xenia's SMC: the DVD tray, open at start, the AV pack, temperatures).
  Where the console's own kernel answered differently (the oracle, `xfkrnl`
  of XenoForge-private, on system software 2.0.16202), the console wins: the
  Unicode case mapping is the console's table (all 65536 values checked;
  Unicode 2.1.2's simple mappings less the titlecase letters and the variant
  forms, `tools/gen_unicode_case.py`), RtlCopyUnicodeString copies the bytes
  it says and ends the text, RtlUnicodeStringToAnsiString makes a byte per
  unit with the converted Length, `%lld` reads 32 bits (`%I64d` 64), and
  KeDelayExecutionThread with a null interval is an access violation.
- **The debug monitor** (`xbdm.cpp`): Xenia's 36 xbdm exports, a retail
  console's answers (console type -1, no debugger, the end of the module
  list). The imports bind as the other system libraries', but no xbdm.xex
  module is listed, so XexGetModuleHandle("xbdm.xex") fails as on a retail
  console.
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
  names case-insensitive. A dispatcher header's wait list is an empty list
  pointing at itself, as the console's own initializers leave it (Xenia
  writes a marker, `'XEN\0'` and the handle, there; the oracle showed the
  console does not). The host object is found from the guest pointer through
  a table (guest address to handle, checked against the header's type byte),
  and one the code initialized itself (the XDK inlines KeInitializeEvent)
  gets one on first use.
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
  the calling thread, as in Xenia, except those of timers (KeSetTimerEx),
  which run on a kernel thread of their own. ExTerminateThread unwinds the host
  stack to the thread's start (a C++ exception).
- `Kernel::launch` makes the main thread (the executable's stack size and
  TLS header) and starts it; `boot` waits for it and reports the threads.

### Stacks

A guest thread may move between guest stacks: the XDK's fibers
(SwitchToFiber saves the registers into the current fiber, loads the next
one's and branches to KeSetCurrentStackPointers, which moves r1 to the next
fiber's stack; Reach's streaming decompressor runs on one). Each guest stack
runs on a host stack of its own (`src/kernel/stacks.h`, stacks.cpp; Windows
fibers, ucontext elsewhere), so a guest switch is a host switch and each
stack's generated frames wait where they are:

- **KeSetCurrentStackPointers** on the calling thread's own context sets the
  KTHREAD's and PCR's stack fields and r1, as Xenia's does, then moves the
  thread to the new stack's host fiber: the thread's own host thread is made
  a fiber at its first switch; a stack entered for the first time gets a
  fiber (16 MB reserved, as a thread's stack) that runs the guest from LR
  with no frame to return to (`Kernel::run_from`: the function there, then
  on where it returns); a stack left before is resumed inside its own call
  to KeSetCurrentStackPointers, which returns into the generated code that
  made it. The calling stack's fiber waits in the call meanwhile. Stacks
  are found by their base (KeSetCurrentStackPointers' fourth argument), or
  the range that holds the new r1. A stack first entered at an address no
  function starts at (the code after a `bl` to the thunk, going on on the
  new stack) runs the frames it returns through interpreted, their calls
  generated; the XDK's fibers start at a routine's start (Reach's at
  0x8295DCD0, which calls the fiber's procedure).
- **Resumed as it left, or unwound.** A stack is resumed as it left only
  when the switch back comes with the r1 it left with and with LR where its
  code waits: where generated code says with `ppc::switch_stack` (the
  address after its `bl` to the thunk, or the switching function's own
  return address for a tail branch); anywhere, for the interpreter running
  the thunk's `sc`, whose `blr` goes where LR says; else the call's own
  return address (the interpreter's `bl`, an indirect call).
  Otherwise the guest went elsewhere on that stack (a fiber restarted from
  its top, a longjmp across fibers): its host frames get an Unwind to LR as
  they resume, which finds the frame that holds the new r1 or reaches the
  stack's start, which runs on from LR.
- **Ends.** MmDeleteKernelStack (a fiber's stack freed) drops the host
  fibers of the stacks in its range, deleted from another of the thread's
  fibers at its next switch. An exception that reaches a fiber's start
  (ExitThread's ThreadExit, anything) is thrown again on the thread's own
  stack, which takes it to `Thread::body`, where the thread's other fibers
  go and the host thread stops being a fiber.
- **Moved fibers.** A guest stack switched to by another thread than the one
  whose host fiber holds its frames (a guest fiber that moved between
  threads) starts over on a fiber of the new thread from LR, as those
  frames use the other thread's context; its outer frames run interpreted.
  None of the eight games does it.
- **The deterministic mode** (fibers on one host thread) will have to resume
  a guest thread on the host fiber of the stack it is on
  (`GuestStacks::current`), not on its own; a thread that is a fiber
  already is used as it is, and not made a thread again at its end.
- Xenia runs a fiber switch on the thread's one host stack: its JIT can
  enter any address, so KeSetCurrentStackPointers drops the host frames and
  re-enters the thread at LR (`XThread::Reenter`). rexglue's fork replaces
  the XAPI's CreateFiber, SwitchToFiber and ConvertThreadToFiber, hooked by
  address, with host fibers of its own; here the game's own XAPI code runs,
  and only the kernel export it ends in is the system's.

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

The renderer (B3) is the command processor's Sink (2026-11-05): shaders
from `shader`, draws and resolves from `draw` and `resolve` with the
register file the processor keeps, guest memory read where it lies (the
processor's physical memory; `memory_written` marks the textures under
the processor's writes), the front buffer at `swap` through the
processor's gamma ramps. Its calls come on the "GPU commands" thread; the
Vulkan device lives with it. Traces replay through a processor of their
own, so there is one PM4 front end ([renderer.md](renderer.md)).
`gpu_command_processor(k)` (`kernel.h`) is where boot installs it. The same
Sink with a trace writer behind it would record Xenia's .xtr from a live
game.

## Sound

The APU as the games' code sees it: the render driver's clients and the
XMA decoder, with XMA decoded by FFmpeg's xmaframes decoder as Xenia
decodes it (plan step A6); the player sends the clients' frames to the
sound card (see "The player"). Translated from Xenia (`audio_system.cc`,
`pacing_audio_driver.cc`, `xma_decoder.cc`, `xma_context_new.cc`,
`xma_context.cc`, `xboxkrnl_audio.cc`, `xboxkrnl_audio_xma.cc`); where it
differs is noted at the top of each file.

As built (`src/kernel/audio.h`; audio.cpp, xma.cpp; the seams in
`xenoforge/audio.h`):

- **Render driver clients** (XAudioRegisterRenderDriverClient, eight at
  most): a kernel thread with a host body, "Audio Worker" in the system
  process, calls each client's guest callback (r3 a word holding its
  argument, as Xenia passes it) once a frame period, 256 samples at
  48 kHz, paced on the scheduler's clock, while the client has a free slot
  of the 64 frames it may have in flight. XAudioSubmitRenderDriverFrame
  hands the frame (6 channels of 256 big-endian floats, one channel after
  another) to the client's `audio::Output` as host floats; the default
  one is Xenia's PacingAudioDriver: the samples are dropped and each frame
  is played a period after the one before it, freeing its slot. The player
  sets a real output (`set_audio_output`). An unregistration waits for a
  callback in progress. `XF_AUDIO_WAV=<prefix>` also writes each client's
  frames to `<prefix>.client<N>.wav` (six channels of 32-bit floats at
  48 kHz, the game's own mix of its voices: what the player plays), to
  listen to a headless run.
- **The XMA decoder**: its registers at 0x7FEA0000 through MMIO (the games
  reach them with lwbrx and stwbrx: the context array's physical address,
  a rotating current context, ten kick, lock and clear registers of 32
  contexts each), and 320 contexts of 64 bytes (XMA_CONTEXT_DATA) in
  physical memory, where Xenia puts them. A kick wakes a second kernel
  thread, "XMA Decoder" in the idle process, and the kicking thread waits
  until it has walked the context as Xenia's default context does:
  packets, frames (a frame split across packets read whole), subframes
  handed to the output ring buffer `subframe_decode_count` at a time,
  loops with their subframe end and skip, the two input buffers swapped,
  the output full or starved, the context data written back merged with
  what the game changed meanwhile. Each frame's decode is one call to an
  `audio::FrameDecoder` (`set_xma_decoder`).
- **FFmpeg's decoder** (`src/xma_decoder.cpp`, the default): Xenia's
  XmaContextNew around FFmpeg's `xmaframes` decoder, from has207's fork
  (Xenia's submodule, commit d980192): a codec context per XMA context,
  opened again when its rate or channels change, each frame one packet
  (the byte with the frame's bit padding, then its bits), the planar
  floats made 16-bit big-endian samples as Xenia's ConvertFrame does
  (times 32767, rounded to nearest even, saturated). FFmpeg's first errors
  are printed and all counted. `XF_XMA_SILENT=1` puts the silent decoder
  in its place, to measure what decoding costs.
- **FFmpeg, built** (`third_party/ffmpeg/ffmpeg.cmake`): CMake's `XF_XMA`
  (on by default) fetches the fork's archive at that commit, or builds from
  `XF_FFMPEG_DIR` (a checkout of it; xenia-canary's
  `third_party/FFmpeg` is taken by itself from the sibling checkout when it
  is at the commit), and compiles only the part of libavutil and libavcodec
  the decoder needs, over a configuration overlay after Xenia's
  `third_party/ffmpeg-xenia` (one codec, no assembly): one shared library,
  `ffmpeg-xma` (0.8 MB), beside the executables. With `XF_XMA` off the core
  builds without FFmpeg and every XMA frame decodes to silence.
- **Loaded at run time** (2026-11-05): the core is compiled against
  FFmpeg's headers and never linked to the library. The first decoder made
  loads `ffmpeg-xma` (LoadLibrary, dlopen) from the executable's folder,
  else wherever the system finds libraries, and looks up the thirteen
  functions it calls by name. Without it, or with one that lacks a
  function, one line on stderr says so and every XMA frame decodes to
  silence, as with `XF_XMA` off; the program starts and runs either way
  (before, linked through the import library, it would not start). Xenia
  links its FFmpeg statically.
- **FFmpeg is LGPL 2.1 or later** (`LICENSE-ffmpeg`), and stays a separate
  library loaded at run time, so XenoForge's own code keeps its license
  (MIT): the LGPL covers `ffmpeg-xma` alone. Shipping the player (or
  anything built with `XF_XMA`) means shipping `ffmpeg-xma.dll` (`.so`)
  with: the LGPL's text and a notice that the program uses FFmpeg under it;
  the library's source, or a written pointer to it, which is the fork's
  commit plus `third_party/ffmpeg` (the overlay and the build script); and
  nothing that keeps a user from putting their own build of it in its
  place (the player loads whatever `ffmpeg-xma` it finds beside it).
  Linking FFmpeg statically would add the duty to ship the player's
  objects for relinking, so it is not done. The package below does all
  of it.
- **Shipping the player** (`cmake/package.cmake`): `cmake --install build
  --prefix <folder>` (or the `player_package` target: `build/xenoforge-player/`
  and `build/xenoforge-player.zip`) puts in one folder the player (which
  runs any game interpreted; a `player_<game>` holds that game's
  recompiled code, so it goes in only with `-DXF_PACKAGE_GAME_PLAYERS=ON`,
  for a folder that stays on the machine that built it: plan.md's fifth
  principle); `ffmpeg-xma.dll`; XenoForge's `LICENSE`; `LICENSE-ffmpeg`,
  `COPYING.LGPLv2.1` from the FFmpeg checkout that was built,
  `FFMPEG-SOURCE.txt` (the fork's commit, as built, and XenoForge's commit
  for the overlay, written at install time) and `ffmpeg-source/` (the
  overlay itself: XenoForge has no public repository yet); `LICENSE-xenia`
  and `LICENSE-rexglue`; and `THIRD-PARTY-NOTICES.txt` with the licenses
  of what is linked into the player (SDL3 and its HIDAPI, Dear ImGui,
  toml++, stb_image_write), taken from the fetched sources. The Microsoft
  C++ runtime the executables import (`MSVCP140.dll`, `VCRUNTIME140*.dll`)
  is not in it: it comes with Windows' Visual C++ Redistributable.
- **Recordings and Xenia's oracle**: with `XF_XMA_RECORD=<file.xmar>`
  (boot or the player) the decoder writes every run of a context (its
  data before and after, its input buffers and its output buffer before
  and after, each distinct buffer once) and the clears and releases
  between them (`xenoforge/xma_recording.h`); it hashes every buffer of
  every run, which slows a game that kicks often (Reach's client fell to
  a few frames a second while recording). `xma_oracle`
  (`tools/xenia_oracle/build.bat xma`, against the xenia-canary build and
  its FFmpeg) walks a recording through Xenia's own XmaContextNew and
  writes its runs beside it (`<name>.xenia.xmar`); `xma_replay_test` walks
  each recording under `XF_XMA_TRACES` through the kernel again and
  compares every run with the live one (the decoder's fields, the output)
  and with Xenia's (all 64 bytes of the context, every byte of the
  output); `XF_XMA_WAV=<dir>` writes each context's output, ours and
  Xenia's, as WAV files. Recordings are game data: `../traces/xma`,
  never the repository. Without `XF_XMA_TRACES` the test has nothing to
  run.
- **The exports**: Xenia's XAudio* (the speaker configuration from the
  console's settings, full voice category volumes, no volume changes, the
  ducker off) and all its XMA*; the ducker's getters, which Xenia lacks,
  return their float in f1, as the games' calls read it.
- `boot` reports the clients, callbacks and frames (and frames per
  second), and the XMA contexts created and kicked, kicks, runs (and runs
  starved of input: the game reads what it wrote before), buffer swaps,
  loops, frames decoded, frames with sound and the loudest sample, frames
  that decoded to nothing, bad input (a frame of the invalid length, a read
  offset past its buffer, a split frame whose rest is missing), runs cut
  at their pass limit and FFmpeg's errors.

## Files

As built (`src/kernel/files.h`; vfs.cpp, files.cpp), from Xenia's vfs and
xboxkrnl_io: a guest path is made canonical, its symbolic links resolved
(the object manager's list: ObCreateSymbolicLink's and the system's), and
given to the device whose mount path starts it; each device keeps a tree of
entries, names any case.

- **Devices**: the disc (`\Device\Cdrom0`, read in place through
  `disc.h`), or a bare executable's directory
  (`\Device\Harddisk0\Partition1`, read-only), with `game:` and `d:` on it;
  Xenia's null device at `\Device\Harddisk0` (the raw partitions answer
  with nothing); host directories for the cache partitions and content.
- **Content packages** (STFS and SVOD; `package.h`, `package.cpp`, and
  `make_package_device` in vfs.cpp), from Xenia's xcontent container
  devices: a CON, LIVE or PIRS header with its metadata, then a read-only
  volume. `Package` reads the header and walks the whole tree at open: for
  STFS the file table through the block hash tables (the active copy of
  each pair chosen by the active-index bits, levels 0 to 2), for SVOD the
  data fragments in `<package>.data` and the GDF directory trees of its
  four layouts (EGDF, XSF, single file, multiple files). A file is a list of
  runs in the host files; reads are positioned, under a lock, never a
  mapping, and every read is bounds-checked (a damaged package fails to
  open or reads short). The device is read only (its writes answer
  ACCESS_DENIED, as Xenia's) and is named STFS or, for SVOD, FATX.
  `xfinspect --package <file> [--list] [--extract <dir>]` lists and
  extracts one. A package file in the content root
  (`content/<XUID>/<TITLE>/<TYPE>/<name>`) is opened by XamContentCreate
  with disposition open: its first bytes are its header (no header file),
  its license mask the or of the licenses', and it is listed by the
  enumerators. Left: creating or writing STFS (Xenia's
  ContentPackageContainer writes it; new content here is a directory
  package), the hash and signature checks (Xenia does none), and deleting
  an SVOD's `.data` with it.
- **File objects** are dispatchers (an auto-reset event in guest memory,
  set when a request completes, as Xenia's XFile is waitable). A request
  that fails, or is made on a handle opened with FILE_SYNCHRONOUS_IO_*,
  completes in the call: the I/O status block, the event, the APC routine
  queued to the calling thread, the completion ports (`IoCompletion`, a
  queue whose signal state counts its packets). A request that succeeds on
  a handle opened neither synchronous nor buffered (the requests Xenia
  answers STATUS_PENDING for) pends as NT's does (2026-11-06): the call
  answers STATUS_PENDING, the status block says STATUS_PENDING, the
  request's event is reset, and the request completes later on a kernel
  thread ("File completion", made with the first one): the status block
  (information, then status), the ports and the file's own event (only when
  the request had no event: NT's rule), the APC and the event. The data is
  moved in the call (so the position, the size and the end-of-file answer
  are known, and a failure is never pending); only the completion is late,
  ordered by a per-device queue with a small cost (20 microseconds and a
  nanosecond a byte; a request due within a millisecond completes as soon
  as the thread runs, so loading is not slowed; `XF_FILE_LATENCY_US` adds a
  delay to each completion, which the tests use). NtCancelIoFile completes
  the calling thread's pending requests on the file as STATUS_CANCELLED
  (their data has moved) and writes its own status block. FATX devices (the
  cache partitions, the hard drive) take no new name past 42 characters or
  path past 240 (STATUS_OBJECT_NAME_INVALID; Free60's FATX page), and say
  42 for the component limit of NtQueryVolumeInformationFile. Host paths
  take Windows' extended-length prefix, so 240 characters fit under a deep
  storage root.
- **The storage root** (`boot --content <dir>`, a temporary directory's
  `xenoforge` by default; never in the repository) has Xenia's layout, so
  saves are shared with it: `cache0`, `cache1` and `cache` (`cache0:`...),
  and `content/<XUID>/<TITLE>/<TYPE>/<name>/` for a package with its header
  at `content/<XUID>/<TITLE>/Headers/<TYPE>/<name>.header` (Xenia's
  XContentContainerHeader). The user is Xenia's default profile (XUID
  B13EBABEBABEBABE), or the first profile Xenia made there.
- `XF_LOG_FILES=1` logs each open and attribute query with its answer.

## XAM

As built (`src/kernel/xam.h`; xam.cpp, xam_content.cpp, xam_user.cpp,
xam_input.cpp, xam_msg.cpp, xam_ui.cpp, xam_net.cpp), from Xenia's xam: one user signed
in offline in slot 0 with Xenia's default profile settings; content as
above; the notifications Xenia queues at start; the controllers from the
drivers boot or the player installs ([input.md](input.md); none, every
user disconnected, without one); the guide's UI answered headless (below); XMsg's apps offline; the disc's media
verification never failing; the network offline (below).
Overlapped requests Xenia defers (25 ms, on a dispatch thread) complete at
once here, before the export returns ERROR_IO_PENDING.

XAM's tasks (XamTaskSchedule, xam.cpp) are the console's, as its XAM was
seen to do (xfxam, 2026-11-06): no thread a task but a pool of six workers
(kernel threads of the system process with 64 KB stacks, made with the first
task) taking tasks from a queue, a semaphore a task; the handle written is
the task object's address with the low bit set (a block in the system heap,
its reference count at +4, two when made: the handle's and the queue's);
XamTaskCloseHandle takes one reference and answers the count left (the
object goes with the last); XamTaskShouldExit asks whether the kernel is
ending. Left from the console's: its separate dispatcher thread, the
workers' affinities and priorities, and the fault of ShouldExit outside a
task. The Kinect answers are the console's with no sensor: the device status
success with the bits 3, the tilt status and angle E_DEVICE_NOT_CONNECTED,
the tilt controller type none. XNetGetTitleXnAddr before the title's
XNetStartup answers 0 (pending) with a zeroed structure but a 1 at +0x20.

The rest of Xenia's system families, ported whole (xam_content.cpp,
xam_info.cpp, xam_avatar.cpp): every XamContent* (the internal forms,
paths resolved, thumbnails kept in the package's header, metadata and
attributes, the devices' names, volume paths and enumerator; launching an
image ends the run, as does XamSwapDisc to a disc other than the one in);
the dashboard's context and app information, title IDs, features, versions
and the settings XAM reads (ExGetXConfigSetting's); the Live hive, XAuth and
network storage offline; avatars with nothing to load, Kinect never set up
(XNuiTitleInitialize's versions and flags kept), gamer tiles never found,
toy portals never connected. Each file's header says where it differs from
Xenia.

The system UI (xam_ui.cpp, every export of Xenia's xam_ui.cc) draws no
dialog: each XamShow*UI answers as Xenia does headless, with the dialog's
default (a message box's focused button, the keyboard's default text, the
hard drive from the device selector, sign-in with the user already there
and XN_SYS_SIGNINCHANGED, the marketplace's downloads succeeded), completes
its overlapped, and sends XN_SYS_UI on and off around the answer (Xenia
sends "off" 200 ms later). Kinect's UI is refused (no sensor), the party,
community and Gold upgrade pages fail, XamIsUIActive is always 0. A player
overlay would plug in at `present` and `present_async` in xam_ui.cpp, the
one place every dialog goes through: it would send XN_SYS_UI on, show the
dialog (title, text, buttons, default) on the player's thread, and make the
answer when the player closes it, completing the overlapped then (the
export already returns ERROR_IO_PENDING) and sending XN_SYS_UI off;
XamIsUIActive would answer whether one is up, and XNotifyQueueUI's toasts
(printed now, by xam_info.cpp) would be drawn by it. The XamShow*UI calls Xenia does not
implement either (the friends, players, messages, game invites, gamer card
for a XUID...) stay unimplemented here: counted, answering 0.

The network is offline, as Xenia's answers make it: no Ethernet link, the
title's address the loopback, DNS and QoS lookups failed at once. The
sockets (xam_net.cpp) are the kernel's own objects with no host socket
behind them (Xenia opens a host socket for each): a datagram socket binds,
keeps its options, and has nothing to read (WSAEWOULDBLOCK, or a blocking
read waits up to SO_RCVTIMEO); datagrams to the loopback or broadcast
reach this title's sockets bound to that port, from 127.0.0.1, and those
to anywhere else are sent and lost; a stream socket listens and never gets
a connection, and connect fails at once. select, the WSA events and
closesocket wake what waits, through the dispatcher. The overlapped calls
(WSARecv, WSARecvFrom, WSASend, WSASendTo, WSAGetOverlappedResult,
WSACancelOverlappedIO, WSAEventSelect; Xenia has only stubs of two) follow
Winsock: a send completes at once, a receive takes a queued datagram or
pends (WSA_IO_PENDING) until one comes, it is cancelled or the socket is
closed; the WSAOVERLAPPED's Internal and InternalHigh hold the status and
the bytes, and its event is set or its completion routine queued as a user
APC. Gears of War 3 polled
recvfrom on an unimplemented socket (an answer of 0, an empty datagram)
and stalled its frame loop there; with these it goes on.

## XAM's oracle

The console's own XAM is the system update's `$flash_xam.xex` (plan,
"Accuracy beyond Xenia"): El Chavo Kart's disc carries 2.0.16202
unencrypted (xamc.dll, base 0x815F0000, 6 MB, 1,570 exports in a table of
2,890 slots). `xfinspect --package` extracts it; `xfxam` (tools/xfxam.cpp)
runs it on our kernel and sets each of its answers beside ours. The file is
read from the path given; nothing of it (code, data, strings, tables) enters
the repository, its docs or its tests, only what it does.

```
xfxam <xam.xex> imports                what its imports bind to here
xfxam <xam.xex> init [--trace]         its entry run on our kernel: the calls, the threads, where it ended
xfxam <xam.xex> call <Export> [arg..]  one export, ours and the console's, compared
xfxam <xam.xex> survey [--out <file>]  every export both have, sorted
  --seconds N (a call's limit), --init-seconds N, --no-init (the export without XAM's start),
  --system (the call on a system thread), --trace (every kernel call printed), --work <dir>
  --no-title (no title running, as before the third survey), --user (ours keeps its signed-in user),
  --prepare <addr[:arg]>,... (functions of the console's XAM called after its start), --launch (XAM's own
  launch of the title, below), --only <A,B | @file>, imports --all (every function it imports)
```

**Loading.** `load_module` (kernel.h) loads it as XexLoadImage loads a
game's DLL: its image at its base, its 429 function and 20 variable imports
from xboxkrnl bound to our exports as a game's are, listed as
`\SystemRoot\xam.xex`. It is a system module: its entry is its loader's to
run, and threads starting and ending do not call it (the console's kernel
gives its own modules no DLL notifications). The kernel's own `xam.xex`
stays what games bind to. Of the 429 functions, 241 are implemented here
(32 of them Xenia's stubs, ported as they are: the Vd* display calls, the
XConfig writes, IoCreateDevice, NtDeviceIoControlFile...) and 188 are not,
none of which Xenia has: XeKeys* (27), XInputd* (22, the input driver),
Hal* (13), Xex* (12), Etx* (9, event tracing), XVoiced* (8), Vd* (8), Drv*
(8), XAudio* (7), the network adapter (Nic*, 10), Mtpd* (8), the boot
animation (Ani*, 4), LZX decompression (LDI*, 4), the USB hubs (Usbd*, 5).

**Starting.** The entry is DllMain(hmodule, DLL_PROCESS_ATTACH, 0) on a
system-process thread, in the interpreter. It returns TRUE in 0.27 s of
1,431 kernel calls: it allocates its heaps (NtAllocateVirtualMemory with a
fifth argument set, MmAllocatePhysicalMemoryEx, NtAllocateEncryptedMemory),
initializes some 150 critical sections and read-write locks, makes eight
worker threads (one start address, a work queue each, priorities and
affinities set) and a ninth, looks for the system update's trace and
diagnostic switches on the flash, the hard disk and a memory unit and for
`systemupdate.xex` on the disc, a USB drive and the flash, tries to load a
security expansion from `\Device\Harddisk0\Partition1`, registers its
title-termination notifications, sets its drivers' callbacks, and queues a
first look at the content storage's notifications to a worker. Of the
imports it calls, 16 are not implemented (each answers 0; it called 17
before KeSetTimerEx, below): EtxProducerRegister,
the Drv*Callback setters (user binding, device configuration, mic array,
audio latency, content storage), XInputdSetFailedConnectionOrBindCallback,
HalRegisterHdDvdRomNotification, KeSetPriorityClassThread, XeKeysGetStatus,
DumpRegisterDedicatedDataBlock, DumpGetRawDumpInfo, IoDismountVolumeByName,
HalGetPowerUpCause, XexShimLock,
HalGetNotedArgonErrors. DrvGetContentStorageNotification is answered
(system.cpp): XAM takes the notifications until an error, and the 0 of an
unimplemented import made it read an empty one and fault. What its start
does not have, and so its exports do not either (the first four were all
that the oracle had until the third survey; "A title running" below says
what is now made):
- no title launched through it: the heap XAM makes for a title is absent,
  so the calls that allocate on a title's thread fail (E_OUTOFMEMORY,
  ERROR_NO_SYSTEM_RESOURCES; XamAlloc works on a system thread);
- no profile and no user signed in (ERROR_NO_SUCH_USER);
- no `\Device\Harddisk0\Partition1` (the storage root is Xenia's layout,
  not a partition device), so no content device;
- no input driver (the XInputd* family), no network adapter.

**A title running** (third survey). `xfxam` now gives each side a title, the
way the dashboard's launch leaves one, from a synthetic XEX of its own
(`make_title_xex`: unencrypted, one page at 0x82000000, an entry that
returns, a made-up title ID 0x58450001; no game's data):
- `\Device\Harddisk0\Partition1` is a writable directory of the host (the
  call's work directory, `<export>.real.hdd`) holding `default.xex`, mounted
  before XAM's start, so its look for a security expansion there finds the
  device and answers not found, as a console without one does;
- our kernel holds the title as boot does (`Kernel::load` and `launch`; ours
  mounts the same directory as the title's and keeps its storage root
  apart), on the console's side too: XAM reads who is running from the
  kernel's executable module (RtlImageXexHeaderField on it, which is what
  XamGetExecutionId calls), so the two sides agree on the title;
- the state XAM keeps of a running title is made by two of its own functions,
  which `--prepare` calls after its start on a system thread, found by
  reading 2.0.16202's code (`builds` in xfxam.cpp, selected by the entry
  point; a build it does not know gets none): 0x816E5118 makes the title's
  heap (heap 0 of XAM's table of nine; a call on a title's thread picks it
  by its class, 0x19... from KeGetCurrentProcessType), and 0x816DF2F8 stores
  the current title ID (XamGetCurrentTitleId, XNetLogonGetTitleID's source);
- ours is signed out (`set_signed_in`): a profile is a package on a content
  device the oracle has not been given, so the console's XAM has no user, and
  the user calls are compared as they answer with none (`--user` keeps
  Xenia's).

**Why not the console's own launch.** `--launch` calls XamLoaderLaunchTitleEx
from a system thread, which is the console's path (XamLoaderLaunchTitle adds
flag 4, which turns a path that starts with a backslash into the dashboard).
It posts a request to XAM's loader thread, which asks a worker to dismount the
drives, loads the executable (the kernel's XexLoadExecutable, which here takes
the console's argument order: name, handle pointer, flags, version; Xenia's
alias of XexLoadImage wrote through the version), checks its privilege,
creates a thread of its own and waits for it. What it loaded, whatever path
was given (also through XamLoaderLaunchTitleEx with flags 0, and from a title's
thread, which never returned), was `\SystemRoot\dash.xex`: why was not found
(the request's type is made from the path and the caller's process type,
and the launch state machine's later states were not read). It gets that far
when that file is the synthetic title (its absence is a fatal error screen,
VdDisplayFatalError 0x12247), and then stops in a loop
of the worker's: IoDismountVolumeByName and ObReferenceObjectByName on
`\Device\HdDvdRom`, which the kernel here has no device object for (it is
answered 0xC0000034, and the loop wants 0xC000000E or an object). That is the
kernel's device model (objects for devices, a process per title), not XAM's,
and is left; XexStartExecutable (a thread of the title process at the
module's entry) is in place for the day it gets there.

**A call.** Each side runs in a process of its own (`xfxam ... side hle|real`,
which `call` and `survey` start), on a fresh kernel, so a fault or a hang
costs only that side's answer. The arguments are numbers or memory made for
them at the same addresses on both sides (0x5F000000 up: `buf:<size>[:<hex>]`,
`fill:<size>:<byte>`, `u32:`, `u64:`, `str:`, `wstr:`). The call runs on a
new thread of the title's process, as a game's (`--system`: the system's);
ours through a thunk, the console's at its export's address after its
start. Each side's answer: how it ended (returned, timed out after
`--seconds`, the interpreter stopped, the thread exited, or a fault, with
the guest address, the pc and the callers' return addresses from the
stack's back chain), r3, the arguments' memory after, and the kernel calls
made on the calling thread in order (Kernel::set_import_hook; ours makes
none but the export itself). Two handles are the same answer whatever
their numbers (XAM's start made objects first). The verdicts:
- **matches**: the same result and memory;
- **differs**;
- **needs state**: the console's timed out, or answered that its start did
  not set up what the call needs (the codes above);
- **faults**, on either side or both.

`XF_XAM_XEX=<file> ctest -R xam_oracle` runs `tests/xam_oracle_test.cpp`
(skipped without it): the imports, the start's TRUE, a call that matches,
the console's own answer to XamGetSystemVersion, a memory difference found
and placed, and a fault kept to its side's process. Its expectations are
2.0.16202's.

**The first survey** (2026-11-06; 105 seconds, sequential). Of our 387 xam
exports, 383 are the console's too: XamFirstRunExperienceShouldRun,
XamGetLocaleEx, XamSetCountry and XamShowMultiplayerUpgradeUI are later
dashboards' and fall in empty slots of its table (1,320 of the 2,890 are
empty, and our loader no longer answers the image's base for one, as
Xenia's does). Each is called with arguments made from its parameters as
Xenia declares them (`tools/xam_signatures.inc`, from
`tools/xam_signatures.py`): user 0, sizes 0x100, counts 1, a zeroed 4 KB
buffer for each pointer, no overlapped. On the title's threads: 185 match,
144 differ, 39 need state, 15 fault (8 the console's, 2 ours, 5 both; most
are a null pointer the generic arguments gave, which neither side checks,
and the console's XamTask* calls outside a task). Much of what differs is
the arguments: the
console's XAM checks them (ERROR_INVALID_PARAMETER or E_INVALIDARG for a
zeroed structure, in 51 answers) where ours takes them. The telling ones,
candidates for our HLE (the console's answer first; where Xenia's answer is
the one we ported, the console's is Microsoft's own and wins). These are
the first survey's findings as they were; "The second survey" below says
what was done with each:
- XamGetSystemVersion(): 0x20076000, ours 0 (Xenia returns 0 on purpose);
  XamUpdateGetCurrentSystemVersion(): 0x20076000, ours 0x760;
  XamUpdateGetBaseSystemVersion(): 0, ours 0x760;
- XamInputGetCapabilities(0, 0, buf): it asks the kernel
  (HidGetCapabilities, which answers success here) and returns 0 with the
  type byte 2 written; ours answers ERROR_DEVICE_NOT_CONNECTED from XAM's
  own state. Ours disagrees with our own kernel;
- XamResetInactivity(): the interrupt time it read (KeQueryInterruptTime),
  ours 0 (Xenia's stub);
- XamVoiceGetMicArrayStatus(): 2, ours 0 (Xenia's stub);
- XamGetLocaleDateFormat(0): 2, ours 3;
- XamFeatureEnabled(n) reads bit n - 1 of Xenia's own mask (1-32 and 39
  on, 0 and 33-38 off), where Xenia reads bit n of its low 36 bits: fixed
  here (xam_info.cpp), and the five edges now match;
- XamIsXbox1TitleId(0): 0, ours 1 (Xenia's test, title IDs below 0x10000);
- XamNotifyCreateListener(0, 0): no listener (0) for an empty mask, ours a
  handle;
- NetDll_WSAStartup(0, 0, buf): WSAEINVAL (0x2726) for version 0, ours 0;
  NetDll_XNetRandom calls XeCryptRandom, as ours could;
- XamFormatDateString and XamFormatTimeString(0, 0, buf, 1): -1 with no
  write, where ours writes through the second argument read as a pointer
  (Xenia's reading for an invalid time; this XAM's arguments differ);
- XamShowMessageBoxUI(0, "T", "X", 1, buttons, 0, 0, result, overlapped):
  without an overlapped ERROR_INVALID_PARAMETER (the box is never
  synchronous), ours the default answer at once; with one,
  ERROR_FUNCTION_FAILED here (no UI to show it), ours ERROR_IO_PENDING.

The noise to read past: thread IDs and tick counts (its start made threads
and took time), pointers into XAM's own data (XamGetOnlineSchema), and
calls that end the title (XamLoaderLaunchTitle, XamLoaderTerminateTitle).

**The second survey** (2026-11-06, after the fixes below): 214 match, 117
differ, 39 need state, 13 fault (8 the console's, 5 both; none ours), against
185, 144, 39 and 15. The
fixes, each confirmed with `xfxam call` before and after, each commented
beside its code with the console's answer and Xenia's:
- the versions: XamGetSystemVersion and XamUpdateGetCurrentSystemVersion
  0x20076000, XamUpdateGetBaseSystemVersion 0. Xenia's 0 was on purpose
  ("if we pretend to be old we have less to implement"); El Chavo Kart and
  Ben 10 import XamGetSystemVersion and both boot as before (same swaps per
  second as on the build without the fix, no interpreted entry);
- XamInputGetCapabilities: the kernel's HidGetCapabilities is now ours
  (xam_input.cpp), answering from the input drivers (success for a pad that
  is there, 0xC000009D for another) so the two agree, and the console's XAM
  run against it answers ERROR_DEVICE_NOT_CONNECTED as ours does. Ours does
  not call the export: both read the same drivers;
- XamResetInactivity keeps the interrupt time (nothing times a user out);
  XamVoiceGetMicArrayStatus 2; XamNuiIsChatMicEnabled 1;
  XamIsXbox1TitleId(0) 0, and a title with 0xFF in the top byte is taken by
  its low bits; XamNotifyCreateListener(Internal) with an empty mask 0;
  XamGetOnlineCountryFromLocale(0) 0;
- XamGetLocaleDateFormat: every one of the 126 locales asked: Xenia's
  table with its 3 (also its default) read as 2, but locale 13 (0) and 103
  (3);
- NetDll_WSAStartup(Ex) and NetDll_XNetStartup(Ex) take callers 1 and 2,
  any other is WSAEINVAL; a WSACleanup with no startup before it is -1 with
  WSANOTINITIALISED, and the title's XNetCleanup the same status;
- XamFormatDateString and XamFormatTimeString: an invalid time (in the
  second form too) or no room is -1 with nothing written (ours used to
  fault on a null buffer); a done one answers the characters written;
- XamShowMessageBoxUI(Ex): ERROR_INVALID_PARAMETER with no overlapped, a user
  past 3 that is not 0xFF, a button count of 0 or past 3, an active button
  past the count, no result pointer, or flags outside the icon (0-4), bit
  8 and bits 12-21; the console's XAM has no UI here and answers
  ERROR_FUNCTION_FAILED to a good call, where ours still answers the
  default button, headless;
- XamSessionRefObjByHandle and XamGetPrivateEnumStructureFromHandle: a handle
  that is not theirs is ERROR_INVALID_HANDLE (it maps a status with
  RtlNtStatusToDosError); XamContentClose, Flush and OpenFile and
  XamProfileClose with no name: ERROR_INVALID_NAME; XamInputSetState a
  user past the slots is the Win32 ERROR_DEVICE_NOT_CONNECTED, and the pad
  is asked about before the vibration pointer; XamLoaderGetMediaInfo(Ex)
  answer 0; XAutomationpUnbindController 0;
- KeSetTimerEx, KeSetTimer and KeCancelTimer (xboxkrnl, sync.cpp; Xenia has
  none): NT's, over the timers NtSetTimerEx arms; a timer with a DPC
  queues it as it comes due (the arguments the system time's two words) to
  a kernel thread, "Timer DPCs", made with the first such timer, which
  runs it at DISPATCH_LEVEL with the PCR saying a DPC runs. XAM's start
  sets four timers, and they fire now.

Then (2026-11-06) the Kinect answers (XamNuiGetDeviceStatus, the tilt status
and angle, the tilt controller type and SetFlags: the console's, with no
sensor), NetDll_XNetGetTitleXnAddr before the title's XNetStartup (pending,
zeroed but a 1 at +0x20) and XamTaskSchedule as a worker pool (the handle
the task object's address with the low bit set; XamTaskCloseHandle takes a
reference of the count at +4): 218 match, 113 differ, 39 need state, 13 fault.
The task calls stay in "differ": the handle is a pointer into another heap,
and the console's XamTaskShouldExit and CloseHandle fault outside a task.

What is left in "differ", and why (the survey's generic arguments, and the
oracle's start, cannot say more):
- **state, not arguments**: most of the 51 "rejects zeroed" answers do not
  depend on the arguments. The avatar calls (XamAvatar*, 18), XamGetDvrStorage
  and SetDvrStorage, the party calls, XamNuiCameraTiltSetCallback and the
  content calls with a name (XamContentCreate*, Delete, GetCreator,
  GetDeviceData...) answer E_INVALIDARG or ERROR_INVALID_PARAMETER whatever
  is given (tried with a valid overlapped, a content structure of a saved
  game, every flags value): XAM's start has no title launched through it,
  no profile and no content device. They would answer as ours only with a
  title running, which this oracle does not have; they are left as they
  are. The same for the calls that need the network (XNet*, XamReadTile,
  XamWriteGamerTile: ERROR_FUNCTION_FAILED), a user (XamUserGetSigninState 0:
  ours has Xenia's one signed-in user on purpose, XamUserWriteProfileSettings)
  and a UI (XamShowGoldUpgradeUI, XNotifyQueueUI, XamShowSigninUIEx);
- **noise**: thread IDs, ticks, handles, pointers into XAM's own data
  (XCustomRegisterDynamicActions, XamGetOnlineSchema, XampXAuthShutdown,
  XamBuild*ResourceLocator and XamParseGamerTileKey only in the memory they
  leave), and XamResetInactivity's r3, a leftover;
- **past a table's end**: XamGetLanguageFromOnlineLanguage past its table
  answers what lies after the table in XAM's memory: not an answer;
- **left as Xenia's**: XamGetLocaleDateFormat's table entries other than 3
  (they match, all of them); XamGetLanguageTypefacePatch (the console's
  start has loaded no font path: 0); XGetGameRegion (a title's); XamSwapDisc
  (ours asks for the disc as the title's would be asked).

**The third survey** (2026-11-06; a title running, Partition1 a directory,
no user: "A title running" above). Of the 383, against the second survey run
again on the same build (218 match, 112 differ, 39 need state, 14 fault): 234
match, 127 differ, 5 need state, 17 fault (13 the console's, 3 both, 1 ours).
Of the 39 that needed state: 14 match, 17 differ, 3 still need it and 5
fault (the console's side). Ten match with the user calls compared as they
answer with none (XamContentCreateEnumerator, XamShowForcedNameChangeUI,
XamShowMarketplaceUI(Ex), XamUserAreUsersFriends, XamUserCheckPrivilege,
XamUserGetGamerTag, GetName, GetSubscriptionType and GetUserTenure all
ERROR_NO_SUCH_USER on both sides) and four after the fixes below
(XamUserContentRestrictionGetFlags, GetSigninInfo,
IsUnsafeProgrammingAllowed, NuiEnableBiometric); XamAlloc(Ex) and the handle makers
(XamSessionCreateHandle, XamCreateEnumeratorHandle) now answer, and differ
only in addresses and handle values (the console allocates from the title heap
at 0x106A0, ours from the kernel's pool); the UI calls answer
ERROR_FUNCTION_FAILED (0x65B) where ours answers headless; five still wait
(XamLoaderLaunchTitle and TerminateTitle, which load the dashboard, and the
three XamShow*UI that wait for a dialog's thread). The survey also has
`variants` now (14 cases, tallied apart: 2 match, 9 differ, 3 fault): the
exports whose generic arguments are turned away before the state is looked at
(a user count of 0, a zero XUID, a zero content structure) run again with the
arguments a title passes (`variants` in xfxam.cpp). Each fix is commented
beside its code with the console's answer and Xenia's:
- XamUserIsUnsafeProgrammingAllowed with no user signed in: "allowed" is
  written, then ERROR_NO_SUCH_USER (ours allowed it);
- XamUserContentRestrictionGetFlags with no user: the console's parental
  control setting (XConfig 3/0x0F, here 3) is written to the flags, then
  ERROR_NO_SUCH_USER (ours wrote nothing); with a user the console's answer
  is not known and Xenia's 0 stays;
- XamUserGetSigninInfo: the sponsor's user index (+0x14) is written 0xFE
  (none) before the user is looked for (ours left 0);
- XamUserNuiEnableBiometric with no user: ERROR_NO_SUCH_USER as an HRESULT
  (ours E_INVALIDARG; with a user Xenia's stays);
- in the kernel (not XAM's, but the oracle's own launch needs them):
  XexLoadExecutable takes the console's order (name, handle pointer, flags,
  version: its XAM's launcher calls it so), and XexStartExecutable starts a
  loaded module as a title. Xenia's alias of XexLoadImage made the console's
  call write through the version.
What the oracle's compare now sets aside as noise: a pointer into the heaps
(0x30000000-0x4FFFFFFF) in the arguments' memory or the result, a thread's
ID, and our stopping when the title ends (the run is over, the console's
thread did not return either).

Left, and why:
- **ObInsertObject is not implemented here** (xboxkrnl; the kernel oracle's
  list). XAM makes each of its enumerators, sessions and voice objects with
  ObCreateObject and ObInsertObject, and gets no handle: its
  ObReferenceObjectByHandle then answers STATUS_INVALID_HANDLE, which is
  where some of the 0x65B answers (XamUserCreateAchievementEnumerator and
  TitlesPlayed) and the 13 faults of the console's side come from (the
  enumerator calls, XamProfileFindAccount: its error paths read through a
  null). The differences on these are not the HLE's yet.
- **A profile and a signed-in user.** A profile is an STFS package on a
  content device (Content\<xuid>\FFFE07D1\00010000\<xuid>); XAM learns of
  the device from DrvGetContentStorageNotification, whose structure is not
  known, and XamProfileCreate needs it. XamUserLogon with a made-up XUID
  faults on the console's side. Until that is built the user calls are
  compared with none, and the content calls (XamContentGetDeviceData and
  State answer 0x57 for device 1 on the console, a device ours has; Create
  answers 0x57 before it looks for a user, ours 0x525) are the oracle's
  lack of a device, not ours.
- **The console's own launch** (above): the kernel's device model.
- The UI calls (0x65B against our headless answers), XamVoiceCreate (the
  console makes a voice object with no microphone, ours is Xenia's stub,
  ERROR_ACCESS_DENIED), XNetGetTitleXnAddr's MAC (XAM's own, different on
  each run), XNetLogonGetTitleID (the console's XAM answers 0 with the title
  running; ours the title's: the reason was not read): left as Xenia's
  design, noise or open.
- XamTaskSchedule and CreateThread with the generic arguments run garbage as
  a callback on our side; whether the interpreter stops before the call
  returns varies (one "faults (ours)" in the last run was CreateThread).

## `boot`

`boot <game.iso> [--seconds N] [--steps N] [--content <dir>] [--interpret]
[--no-controller] [--press <buttons@seconds>]...`: loads the game, runs its entry point, and prints the modules loaded
(each one's generated code, or why it runs interpreted), the
interpreter's entries (the most entered addresses first, with the module
holding each), the switches between guest stacks (host fibers made and
dropped) and the returns to another frame (longjmps, catches: taken by
generated code at a resume site, by a dispatch interpreting on, or at a
stack's start; `stacks_report`), the kernel calls
(counts, unimplemented ones), the GPU's packets, draws, resolves, swaps (and
swaps per second) and interrupts, the sound's frames and XMA contexts, and
where it stopped. Built as `boot_<game>` (CMake's `XF_GAMES`), it runs the
game's generated code, every module's ("Code" above); `--interpret` leaves it out. `--steps` bounds the
interpreter's instructions per entry (2 billion by default: about 20 s of
El Chavo Kart's main thread). XAM's input answers from an idle pad for
user 0, a controller plugged in with nothing pressed, so a game waiting
for one goes on (Ben 10 paused on "Controller Lost Connection" without
it); `--no-controller` leaves every user disconnected, and `--press
start@40` holds buttons at a time on the scheduler's clock
([input.md](input.md)). What boot shares with the player (opening the
game, the renderer, the launch, the report) is `tools/game.cpp`.

`--render` draws what the GPU is sent (the renderer as the processor's
Sink, on a Vulkan device without a window; off by default, so runs stay
headless). `--shots <dir>` writes the front buffer of chosen swaps there as
`<title>_<swap>.png` (`--shot-swaps 300,301,890-910`, `--shot-every N`),
the swaps numbered as Xenia's `--trace_gpu_frames` numbers a trace's frame;
`xfcompare <xenia.png> <dir>` finds the shot closest to a Xenia dump. The
report adds the renderer's counts: draws drawn and skipped by reason, what
is approximate, resolves, swaps, and the time spent (in submissions,
resolves, checking and decoding textures, copying vertex data, making
pipelines). `--rates` prints a line a second on the host's clock: the
swaps, draws and resolves a second, the scheduler's clock, the share of
the second the renderer was busy (`-` when a call of its was under way),
to set boot and the player side by side over a run.

The thread report (at the end, and with `--stall-report S` whenever no
swap has come for S seconds, 5 S before the first, once a stall) gives
each thread's name (the host's, or what the guest named it with
SetThreadName), its state, what a waiting thread waits for (each object's
kind, handle, body, type and signal state) and its guest stack: the saved
LR of each frame on the back chain, read where the pages are readable. A
title's main thread often waits for good on its game thread (Reach's
`main` waits at lr 0x820CCFA8, in WaitForSingleObjectEx, for its
MAIN_THREAD), so the thread to look at is the one that stopped, not the
main one. Booting a DLL as the executable (`--xex Game.xex` on BC2) says
so: its entry point is a DllMain that returns at once.

Reach with its game library (2026-11-06): the intro at 60 swaps a second;
2,300 to 2,750 swaps in, MAIN_THREAD loads the main menu itself (running,
16 frames deep; 0 to 6 s with no swap, longer as the machine is busier),
then the menu at 30 swaps a second, 6,349 swaps in 180 s. Its start was
run 30 times beside other load (16 beside 12 busy processes, 8 beside two
writers flushing to the hard disk the disc image is on, 6 under a game
library's build): the first swap came at 2 to 11 s every time.

## The player

Plan track C1 (2026-11-05), after the 3DS runtime's `frontend/player.cpp`:
a game in an SDL3 window, with its controllers, its sound and an overlay.
Built as `player_<game>` beside `boot_<game>` (the same generated code
linked) and as `player` (interpreted), when CMake's `XF_PLAYER` is on (by
default with `XF_GAMES`); only then are SDL3 (release-3.4.16), Dear ImGui
(v1.91.9) and toml++ (v3.4.0) fetched, the 3DS runtime's pins, so the core
and its tests never need them.

```
player[_<game>] <game.iso | default.xex> [--xex <path>] [--content <dir>] [--config <file>] [--steps N]
    [--interpret] [--quit-after S] [--screenshots <dir>] [--screenshot-every N] [--press <b@s>]... [--no-sound]
    [--overlay]
```

The game is opened, launched and reported by `tools/game.cpp`, boot's own
code. As built (`frontend/`):

- **The window** (`presenter.cpp`, 2026-11-05: on the GPU): the window is
  on the renderer's own Vulkan device (`vk::Device::open` with the
  window's surface), and the front buffer never leaves the GPU. At each
  swap the renderer hands the presenter its image (`render::Options::on_swap`,
  a `render::FrontBuffer`: texture fetch constant 0's image, its swizzle,
  the display's gamma ramp) right after submitting the swap's work; the
  presenter makes it the window's picture with a compute shader
  (`front_buffer.comp`: the swizzle, opaque, the 256-entry gamma ramp, as
  the read-back's CPU code does; byte for byte the picture `boot --shots`
  writes) into one of three 8-bit RGBA images of its own, submitted on the
  renderer's queue, whose order is the synchronization. The window's
  thread blits the newest picture into a swapchain image, scaled to fit
  with its shape kept (`video.aspect = "auto"`, letterboxed or pillarboxed)
  or stretched, filtered (`video.smooth`) or not, with the overlay drawn
  over it, on a second queue of the device where it has one. Timeline
  semaphores order the two queues: the window waits on the GPU for a
  picture's conversion, and a picture is converted again only once the
  window's last use of it is done.
- **One clock.** The presenter's conversion records and submits a few
  commands on the renderer's thread and returns; it never waits for the
  GPU or the window (with no picture free, the swap is not shown and is
  counted). The window's thread shows the newest picture whenever the
  display lets it (`video.vsync`: FIFO, else mailbox), and that wait is
  its alone: it presents on its own queue, so even a present that blocks
  for the display holds nothing of the renderer's. (A device whose
  graphics family has one queue shares it, under the queue's lock.)
- **Input** ([input.md](input.md)): SDL3's gamepads (Xenia's SDL driver),
  then the keyboard as a controller (Xenia's keyboard driver, its
  bindings), the deadzones; the scripted presses on user 0.
- **Sound** (`sdl_audio.cpp`): each render driver client's frames to an
  SDL3 audio stream on the default playback device, in 5.1 where the
  device takes six channels, else folded to stereo as Xenia folds it
  (center halved on both sides, the back channels on theirs, the LFE
  dropped, over 2.5). The guest's timing stays the scheduler's: each output
  is the pacing output (a frame played every 256/48000 s on that clock)
  with the samples sent on beside it; the stream is kept about 60 ms ahead
  by playing a little faster or slower (half a percent at most), primed
  with silence when it ran dry, and a frame is let go when a quarter second
  is queued. `audio.volume` and `audio.mute` are the device's gain.
- **The overlay** (F1, Dear ImGui): the guest's swaps per second, the
  frames presented and the window's refreshes per second, the sound frames
  played (and how many had sound), the input states delivered, each user's
  controller (what drives it, its buttons, triggers and sticks), the
  volume, mute, vsync, scaling, fullscreen, and a screenshot button. F11
  switches fullscreen; F12 saves the window's next picture (copied back
  from the GPU) as `<title>_<swap>_key<n>.png` in `player.screenshots`
  (default: a `screenshots` folder beside config.toml, never the
  repository).
- **config.toml** (`settings.cpp`; `--config`, else SDL's preference folder,
  `%APPDATA%\xenoforge\config.toml` on Windows), written with the defaults
  the first time and saved when the overlay closes:

  ```toml
  [video]    # fullscreen, vsync (true), smooth (true), aspect ("auto" or "stretch")
  [audio]    # volume (0-100), mute
  [input]    # keyboard (true), keyboard_user (0), left_deadzone, right_deadzone (0-1), vibration (true)
  [keyboard] # each pad control's keys (input.md)
  [player]   # screenshots: the folder F12 writes to
  [launcher] # games: the folder the launcher lists
  [games.4D5307E6.video] # a game's own settings (the launcher's), any of the four sections above
  ```

For checks without a person at the screen: `--quit-after <seconds>`
ends the run with a report (the swaps, the front buffers taken on the GPU
and not, the frames presented, the window's refreshes and its waits, the
input states delivered, the sound frames submitted, sent to the device,
with sound, dropped and starved, then boot's report); `--screenshots <dir>`
writes every `--screenshot-every N` swaps' front buffer (300 by default)
as `<title>_<swap>.png` (the renderer's read-back, boot's `--shots`
picture) and the window's picture of the same swap beside it as
`<title>_<swap>_window.png`, printing the time on the scheduler's clock
each came at, which is the clock `--press` takes (or `--press
start@#1100`, at a swap); `--overlay` opens the overlay from the start;
`--no-sound` keeps the pacing output alone; `--rates` prints a line a
second (boot's `--rates`: the swaps, draws and resolves a second, the
renderer's busy share; then the pictures taken and presented, the
window's refreshes and waits, the sound frames sent).

Slow at the start (2026-11-05, the user's report: the beginning at about
15 frames a second, the rest at 30; boot `--render` at 30 swaps a second).
Measured with `--rates`, boot and the player back to back: the guest swaps
30 a second in both, and what fell to 14-20 was the frames the window got.
The causes were the player's alone. Each front buffer the window took was
read back and made a picture on the CPU on the renderer's thread
(`deliver`: decoded, swizzled, through the gamma ramp, about 13 ms at
720p): the renderer was busy 40-60% of the intro movie where boot's is 4-6%
(boot reading back every swap the same way: 40%). The window's device was
a second Vulkan device on the same GPU, and the renderer waited for the GPU
for 3 s of 100 where boot waits 0.1 s. And a front buffer was read back
only once the window had taken the one before, so as soon as a read-back
took longer than a frame (a busy GPU: other work on the machine, a heavy
scene) every other swap went unshown: 15 a second. The window's own waits
(its frame, the swapchain's image, the present) were a few milliseconds a
second: neither vsync nor the window's size. Presenting on the GPU took all
three away. Back to back under the same load (other agents' builds), 100 s
each: boot 6% busy in the movie and 0.1 s waiting for the GPU; the player
5-7% and 0.2 s, 2823 of 2823 swaps taken and 2819 presented, 30 a second
with the guest's swaps. With the validation layer, 1154 of 1154 taken and
nothing reported; the window's pictures are byte for byte the renderer's
read-back (12 of 12 swaps compared). Sound was not a cause: the game's
movie and loading run on the scheduler's clock, and the SDL output is
paced by the pacing output underneath it. Nor were pipelines, in the main:
the title screen's first frame makes about 24, 0.2 s in all on an idle
CPU with or without the driver's own disk cache, though 2.5 s once with
every core busy compiling other builds; the renderer now keeps Vulkan's
pipeline cache on disk (renderer.md, "Decisions"), 0.03 s on the next run.
(A real pad plugged into the machine, with the deadzones at 0, took the
player's runs past the title into the menus, which boot's idle pad does
not: compare the two before the title.)

Checked (2026-11-05, both games with their generated code, on a machine
other builds were loading, so the rates vary from run to run; the shots and
logs in `../traces/player/`):

- **El Chavo Kart**: the intro movie at 10 to 20 swaps a second, each front
  buffer handed over shown (1204 of 1221 swaps in 60 s, 1600 of 1615 in
  270 s); its title screen came at 96.5 s on the scheduler's clock, and
  `--press start@100` took it to the main menu, where the game's
  first-time dialog ("Would you like to? A Yes, B No") waits. Two XAudio
  clients play 187.5 frames a second each; 10,685 of 99,085 frames carried
  sound (the movie's PCM; XMA was silence then, before A6), none dropped. The
  game asks for users 0-3 each frame and calls XamInputSetState (441 to
  599 times in a run). A capture of the window with `--overlay` shows the
  front buffer letterboxed under the overlay's rates, controllers and
  settings.
- **Ben 10**: no "Controller Lost Connection": in 90 s, 738 swaps, the
  Cartoon Network logo by swap 300 and Papaya's by 600 (where boot used to
  stop swapping before 600); one XAudio client at 187.5 frames a second,
  3,867 of 16,160 with sound. It polls XamInputGetKeystrokeEx with
  `ANY_USER | KEYBOARD`, which no driver here answers (as in Xenia).
- Left for a person at the screen: that a real pad and the keyboard move
  things in the game (only their drivers' states and the scripted presses
  are checked here), that the sound is heard and sounds right, how the
  window feels resized, fullscreen and with vsync off.

## The launcher

Plan track C2 (2026-11-05, a first one), after the 3DS runtime's
`frontend/launcher.cpp`, built as `launcher` beside the players (a window
program on Windows: no console) and installed with them by `cmake
--install`. What it does is `LauncherCore` (`launcher_core.h`), with no UI
code: the games folder and its scan, a game's settings loaded, saved, made
its own or shared again, the gamepads connected, a game started and polled
until it ends. The view is RmlUi drawn through XenoForge's own Vulkan inside
the SDL3 window (the owner's decision, 2026-11-05; built 2026-11-06), with
Dear ImGui kept for the debugging tools only (the player's F1 overlay).

```
launcher [--config <file>] [--games <folder>] [--list] [--select N] [--screen <page>] [--start N]
    [--player-arg <arg>]... [--build-jobs N] [--quit-after S] [--screenshot <png>] [--demo-progress] [--demo-fail]
    [--demo-speed X] [--show-log] [--capture-binding <name>] [--render-size WxH] [--scale F] [--assets <dir>]
```

### The launcher's interface (`frontend/ui`, `launcher_view.*`)

Pieces, each apart from the others:

- **`ui/ui_renderer.*`**: `Rml::RenderInterface` on `vk::Device`, written
  again for Vulkan 1.3's dynamic rendering after the structure of RmlUi's own
  GL3 backend (MIT). Geometry in an arena of mapped buffers (so thousands of
  geometries are a few allocations), textures and layers all in
  `VK_IMAGE_LAYOUT_GENERAL` with one global barrier between passes, a
  per-frame uniform ring with dynamic offsets, one shader pair (`ui.vert`,
  `ui.frag`: texture, linear/radial/conic gradients, color matrix, mask,
  drop shadow, blur) and eight pipelines (blend over or replace, times
  stencil: off, test, write, increment). Scissor, a stencil clip mask
  (rounded `overflow: hidden`), transforms, layers with filters (blur by
  halving and a 7-tap Gaussian, drop shadow, opacity, brightness, contrast,
  invert, grayscale, sepia, hue-rotate, saturate, mask images),
  `SaveLayerAsTexture` (box shadows are made of it). What RmlUi releases
  mid-frame is destroyed once the frame is done.
- **`ui/ui_window.*`**: the device opened with the SDL window, its swapchain
  (two frames in flight, FIFO), and `frame()`: the UI drawn into the base
  layer and blitted to the swapchain image. A frame can be drawn at a size of
  its own without the window (`--render-size`) and read back.
- **`ui/ui_system.*`**: RmlUi's system interface on SDL3 and SDL events as
  RmlUi's input.
- **`launcher_model.*`, `launcher_html.*`**: what the screens say (a game's
  status, durations, the accent colour's shades, the bindings' names, the
  spatial navigation) and their structure as RML strings, as plain functions
  with no RmlUi in them (`frontend_test` checks them). **`prepare_job.*`**: a
  build on its worker thread, as state to read (no UI, no SDL).
- **`launcher_view.*`, `launcher_screens.cpp`, `launcher_input.cpp`**: the
  connection to the document: parts rebuilt when what they show changes,
  clicks, keys and the pad into actions, focus moved by direction.
- **`ui/assets/launcher.rml`, `launcher.rcss`**: the look. The stylesheet has
  `{accent...}` tokens the launcher fills when it loads it, so the accent
  colour (six swatches in Settings, `[launcher] accent` in config.toml) is the
  user's; changing it loads the document again. `ui/fonts/`: Inter (SIL OFL
  1.1; its license beside it), four weights.

The layout is made for 1280 x 720 dp and scales with the window
(`min(width / 1280, height / 720)`, 0.5 to 4), so 1080p is 1.5, 4K is 3.
Navigation: the mouse as usual; the keyboard with the arrows, Enter, Escape
and Tab (pages); the pad with the d-pad or left stick (repeating), A, B, X
(game settings), Y (rescan), the shoulders (pages) and Start (play). The
hints at the bottom follow the last device used.

- **The games** (`library.cpp`): the ISOs (and bare `default.xex` files) in
  a folder the user picks (`launcher.games` in config.toml; the system's
  folder dialog) and its subfolders one level down, read in place on a
  thread of their own: the disc's `default.xex` parsed and unpacked for its
  title ID, version and the digest of its image (`xf::image_digest`, what
  generated code is tied to), and the title's name from the executable's
  XDBF resource (the one named by its title ID: the string 0x8000 of the
  English table, else of the XSTC's default language, as Xenia's
  `GameInfoDatabase::GetTitleName` gives it on an English console). About a
  second per disc (the executable decrypted and unpacked).
- **Which player**: each `player` and `player_<game>` beside the launcher
  is run once with `--identify` (no window, no console), which prints the
  digest of every module whose generated code it carries. A game whose
  disc has a library in the cache (`find_cached` on its digest, in the
  default cache) starts in the generic `player`, which loads it; without
  one, in the `player_<game>` that carries its `default.xex`, else the card
  offers Prepare (the recompile step) and, if that fails, the generic
  player interprets (the list says which: "Runs in"). `player_for` in
  `library.h` is that choice.
- **Settings and controls**: the selected game's settings, edited in the
  player's own config.toml and saved as they change: the video (fullscreen,
  vsync, scaling, shape) and sound (volume, mute) of the player's overlay,
  and the controls: the gamepads connected, vibration, the sticks'
  deadzones, the keyboard as a controller (on or off, for which user) and
  each of the 25 bindings (typed, or "Set" and the next key pressed;
  "Xenia's keys" puts the defaults back). These are the shared settings
  unless "Settings of its own for this game" is ticked: then a
  `[games.<title ID>]` table holds the game's own copy of the video, audio,
  input and keyboard sections, the player reads it over the shared ones for
  that game (its title ID, once the disc is open) and its overlay saves
  back into it. Unticked, the table is dropped.
- **Start** (or a double click): the player as a process of its own on the
  ISO, with no console, its working directory its own, its output and
  errors in `logs/<title ID>.log` beside config.toml. The launcher stays
  open; when the player ends, it shows the player's closing lines (how the
  run ended, the frames, the sound) or, when there are none, the log's
  last lines, and reads the settings again (the overlay may have changed
  them). "Stop" ends the process.

For checks without a person at the screen: `--list` prints the players
(with how many modules each carries) and the folder's games (title ID,
title, version, digest, the player each starts in), without a window;
`--select N` selects a game, `--controls` opens its controls' tab,
`--start N` presses
its Play (a disc not yet built is prepared first, then `--keys a` plays it),
`--build-jobs N` the compilers a Prepare runs at once (4 by default; each
takes a gigabyte or two), `--player-arg` adds to the player's command line (e.g.
`--quit-after 20`), `--quit-after S` closes the launcher after S seconds
once a game it started has ended, printing how it ended, and `--screenshot`
saves its window's last picture.

Checked (2026-11-05): `--list` over `F:\RECOMPs` found the 9 discs in
its subfolders (over `F:\RECOMPs\Chaves`, the extracted `assets\default.xex`
too) with their names from the XDBF (BEN 10: VILGAX ATTACKS, El Chavo Kart, Battlefield Bad Co. 2, Forza
Horizon, Gears of War 3, Halo: Reach, Red Dead Redemption, SONIC
UNLEASHED), 0.1 to 1.7 s each; both El Chavo Kart discs matched
`player_chaves` by digest, Ben 10 `player_ben10`, the rest the generic
player. `--start` ran El Chavo Kart's USA disc in `player_chaves` with a
`[games.475807D7]` table in the settings ("settings: ... (the game's own,
475807D7)" in its log): 1153 swaps in 120 s, 1130 frames presented, 20,713
sound frames played, 5,380 input states delivered, its screenshots and
its closing lines shown in the launcher. The launcher's own pictures (the
list, the controls' tab, a run's end) are saved by `--screenshot`. Once,
after a two-minute run, that picture came out blank (the window's
background alone); a three-second run's was whole. Left for a person:
the folder dialog, typing and "Set" for the bindings, and how the form
feels.

Checked (2026-11-06, the RmlUi view): every screen and state saved by
`--screenshot` at 1920 x 1080 and the library at 3840 x 2160 (`--render-size`,
outside the window; the pictures hold the games' icons, so they are kept in
`traces/launcher_ui/`, never in the repo): the library (9 discs, a focused
tile, ready and needs-preparing, scanning, no folder chosen, no games found),
the preparing screen (`--demo-progress`, `--demo-fail`, `--show-log`),
settings (with a dropdown open), controls (a binding lit on the drawn pad, one
waiting for a key) and about. `--keys` sent pad presses through the whole
navigation (d-pad focus, the dropdown, the scrolled list); Vulkan validation
reported nothing. A window run drew 168 frames a second (the display's rate);
`--start 0` ran El Chavo Kart in `player_chaves` as before and `--list` is
unchanged. RmlUi quirks learned, kept in launcher.rcss: `div` is not block
until the stylesheet says so; text directly inside a flex container is not
drawn (wrap it in a block); `box-shadow` cannot be transitioned; an element
with its own `opacity` inside one with `opacity: 0` is still drawn (the
pages are `visibility: hidden` once faded); `overflow: hidden` clips only
when the content overflows, `clip: always` makes it. A blurred `box-shadow`
needs a spread: RmlUi draws the shadow's shape with the box cut out of it
before it blurs, so one that is not bigger than its box (offset 0, spread 0)
has nothing to blur and shows nothing, which is why the big cards and the
focus glows had none; launcher.rcss gives each about 0.4 of its blur as
spread. A shadow's texture is made in a window-sized layer, so one taller
than the window is cut (the detail card's is kept under that). The fades at
the edges of the scrolling pages and of the grid are `mask-image` on the
scroller (the content itself fades), not dark overlays, which showed as
faint bands over the coloured backdrop.
The backdrop and the detail's hero are the icon shrunk and blurred on the
CPU (`add_soft_image`): a full-window GPU blur costs several passes a frame
and fades at the window's edges, while the blur filter is kept for what
RmlUi makes of it (box shadows). Left for a person: the folder dialog, typing
in the games folder's field, the mouse (hover, drag on a slider), a real pad
(only the scripted presses were run), how the transitions feel.

The prepare flow, end to end (2026-11-06): Sonic Unleashed's disc, never
built before (its interrupted build resumed from the nine objects in the
cache's work folder, `--build-jobs 2`): Play opened the preparing screen
(reading, analysing, writing code, "9 of 252 files" compiling with the time
left), about 13 minutes later the library `<digest>-<version>.dll` was in
the cache, the card said Ready, and Play (a pad's A on the finished screen)
started the generic `player` on the disc, which loaded it from the default
cache: 90 s, 2481 swaps (27.6 a second), 2481 frames presented, sound
played without a drop, no stop; the title screen is drawn. Which player a
game starts in is `player_for` (`library.h`): with a library cached, the
generic player; without, the `player_<game>` that carries its code, else
Prepare. `plan_launch` turns that into a `LaunchPlan` (program, whether it
loads the library, arguments) and `LauncherCore::start` is the only place
that carries one out, with SDL's process API, so a host that runs the game
inside the launcher's own process reads the same plan.

Left out of this first launcher: port options, title updates and content
packages, cover art. The recompile step itself is below ("The game
library"); the preparing screen calls it.

### The game library (plan D1 and D2, 2026-11-06)

The generated code reaches a player two ways, from one emission and one set
of headers:

- **linked statically**, as before: CMake compiles `xfrecomp emit`'s files
  into `game_<name>.lib` and links it into `boot_<game>` and
  `player_<game>`. Ports (the SDK, track E) build this way.
- **as a game library**: the recompile step (`game_build.h`; `xfbuild` on
  the command line, the launcher on a worker thread) emits the disc's
  modules in process, compiles them with clang and links them with lld into
  a DLL (a `.so` on Linux) in the cache, and the generic `boot` and `player`
  load the one built for the disc's executable.

**The interface** (`game_library.h`, `ppc_runtime.h`): a C table each way.
The library exports one function, `xf_game_library(const Runtime *)`, which
checks the runtime's interface version and the table's size, copies the
table, and gives back its modules' tables (`xf_modules`, `ppc_module.h`) and
the version it was built for; another version gets nullptr. The runtime's
table holds the five calls the generated code makes (`dispatch`, `invalid`,
`unwind`, `switch_stack`, and `resumable`, which runs the body of a function
with resume sites: docs/recompiler.md, "Stack switches") and the C library's
pieces the instructions' semantics use: `memcpy`, `memmove` and `memset`
(the compiler calls them on its own), `llrint` (fctiw and fctid's fast
path), `exp2f` and `log2f` (vexptefp, vlogefp), `fmod`, and `fesetround`,
`feclearexcept` and `fetestexcept` (the slow path of the FPU's arithmetic),
with the rounding modes and flags in XenoForge's own numbering, translated
by the runtime. Everything else the generated code needs comes in the
`Context` each call is given: the registers, the guest memory's base, and
the system's callbacks (`sc`, traps, `mftb`, MMIO). The rest of the
standard library the ops used (`bit_cast`, `rotl`, `countl_zero`, `fma`,
`sqrt`, the atomics' fence, `numeric_limits`...) is compiler builtins in
`ppc_runtime.h`'s `lib::`, for both ways, so the statically linked code and
the library compile to the same instructions; the five calls are direct
calls in one and calls through the table in the other (an indirect call,
as a call to a DLL's import is).

**The version** (`XF_GAME_INTERFACE_VERSION`, `cmake/game_interface.cmake`):
the first 32 bits of a SHA-256 of what makes a library: the generated
code's headers, the recompiler's sources (discovery and emission) and
`game_build.cpp` (the compiler's flags). Any change to them makes every
cached library stale, and the launcher builds it again; a library from
another XenoForge never runs against these headers. The same script embeds
those headers in the core, and the recompile step writes them beside the
code it compiles, so the code is always built against the headers of the
XenoForge that loads it and nothing but the toolchain ships for the build.

**The Unwind across the boundary.** An `Unwind` (a longjmp, a catch's
continuation, a stack switched back elsewhere) is a C++ exception. It is
thrown only by the runtime (`unwind()`, the kernel's dispatch) and caught
only by the runtime (the kernel's dispatch, `resumable()`): the emitter
used to write a try block into each function with resume sites (9 in El
Chavo Kart, 274 in the eight games), and now writes its body as a lambda
the runtime's `resumable` runs and runs again at a resume site. So the
exception only passes through the library's frames, which carry their
unwind tables (`.pdata` and `.xdata` on Windows, which RtlUnwindEx finds in
any loaded module; `.eh_frame` with `--eh-frame-hdr` on Linux) and no
handlers: no personality routine, no type information, no C++ runtime in
the library. Measured (game_library_test, the same code linked and in a
library, 5,000 each): through 2 frames 3,695 ns linked and 3,666 ns in the
library, through 17 frames 6,244 and 6,265 ns, with other builds running;
on a quieter machine 1,885 and 1,877 ns, 3,229 and 2,959 ns. The same.

**The toolchain shipped** (decided): `clang.exe` and `lld.exe` from the
LLVM release (23.1.2 here), 184.9 MB (109.5 and 75.4 MB; 71.6 MB as
gzip, 51.2 MB as xz), against 3.0 GB for the installed LLVM. `lld.exe` is
run as `lld -flavor link` when there is no `lld-link.exe`, so one copy of
it does. Nothing else: the generated code's headers
are freestanding when built as a library (`XF_GAME_LIBRARY`: the integer
types from the compiler's predefined macros, no `#include` of a standard
header), the files are compiled with `-nostdinc -nostdinc++` (so clang
looks for no Visual Studio, Windows SDK or resource headers) and linked
with `/nodefaultlib /noentry` (so no C runtime and no import library). The
library's import directory is empty (checked by game_library_test, and on
El Chavo Kart's and Ben 10's libraries). Both binaries import only
Windows' own DLLs (KERNEL32, ADVAPI32, ntdll, ole32, OLEAUT32, SHELL32,
VERSION): the LLVM release links its C runtime statically. Apache 2.0 with
LLVM's exception, redistributable. The search (`find_toolchain`):
`--toolchain`, `XF_TOOLCHAIN`, `toolchain/bin` beside the executable (the
bundle), an installed LLVM (`%ProgramFiles%\LLVM\bin`), the PATH.

**The build** (`prepare_game`): the disc's executables read (all XEX2 files
but title updates and the system update's), `default.xex`'s image digest
taken; discovery and emission in process (`recomp::emit_modules`, what
`xfrecomp emit` writes) into `<cache>/work/<digest>-<version>/src`, the
headers and the library's entry (`xf_library_entry.cpp`) beside them; each
file compiled by its own clang process (`-std=c++20 -O2 -g0 -w -mfma
-mavx2 -fno-math-errno -nostdinc -nostdinc++ -DXF_GAME_LIBRARY`, the
statically linked games' flags; never `-ffast-math`; on Linux also
`-fPIC -fvisibility=hidden -fno-stack-protector`), 4 at once by default
(`BuildOptions::jobs`: more ran out of memory on 16 GB), the largest files
first, at below-normal priority with only its log inherited; then
`lld-link /dll /noentry /nodefaultlib /opt:ref /Brepro` through a response
file (`ld.lld -shared --eh-frame-hdr --gc-sections` on Linux).
The library goes to `<cache>/<digest>-<version>.dll`, replacing any other
version's for that digest, and the work directory is removed (`keep_work`
keeps it). Progress is reported per module (discovery), per file (emission
and compiling) and for the link, always from the caller's thread; the
cancel flag ends the compilers under way. A stopped build goes on from the
objects it left (an object newer than its source is not compiled again).
The CPU must have AVX2 and FMA (checked first, `cpu_supported`).
`find_cached(cache, digest)` finds `<digest>-<this version>.dll`.

```
xfbuild <game.iso | default.xex> [cache dir] [--jobs N] [--toolchain <dir>] [--keep] [--force]
        [--cancel-after S] [--quiet]
boot <iso> [--cache <dir>] [--game-library <dll>] ...
player <iso> [--cache <dir>] [--game-library <dll>] ...
```

The generic `boot` and `player` look in the cache (the launcher's by
default) for the executable's digest; with a library they print
"generated code: N modules, M functions (the game library <path>)", with
none they interpret as before. `boot_<game>` and `player_<game>` link their
code as before; `--interpret` leaves either out.

Measured (2026-11-06, a Ryzen 5 5600G, 6 cores and 12 threads, 16 GB; the
discs and the cache on a hard disk; another agent's build running beside
the two builds):

| | reading | discovery | emission | compiling | linking | total | library |
|---|---|---|---|---|---|---|---|
| El Chavo Kart (291 files + 2) | 3.2 s | 4.4 s | 7.9 s | 543.6 s | 15.8 s | 575 s | 180 MB |
| Ben 10 (106 files + 2) | 2.7 s | 1.4 s | 2.2 s | 266.2 s | 6.6 s | 279 s | 62 MB |

Neither library imports anything (an empty import directory, one export,
`.pdata` for the unwinds, no initializers). A build cancelled after 40 s
stopped at 40.2 s with no compiler left running; run again, it compiled
the 100 files left (249 s), and its library is the clean build's but for
the link's time stamp (two bytes; `/Brepro`, added after these builds,
leaves it out).

The libraries run as fast as the statically linked games (boot with
`--render`, back to back, swaps counted by the GPU; interpreted entries 0
in every run):

| | 30 s | 45 s |
|---|---|---|
| El Chavo Kart, linked | 878 | 1,337 (59.0 s of CPU) |
| El Chavo Kart, library | 898 | 1,347 (59.8 s of CPU) |
| Ben 10, linked | 1,119; 1,123 | 1,652; 1,669 |
| Ben 10, library | 1,132 | 1,803; 1,872 |

El Chavo Kart's menus run at the game's 30 frames a second either way.
Ben 10 over 15 s: 585 and 650 swaps linked, 627 and 640 in the library;
with ten compilers running beside (a library build), 604 and 622 linked,
599 and 607 in the library. The start varies between runs of either
(a first swap at 4.7 s instead of 1.2 s: one library run at 30 s, 881
swaps, and the static warm-up). Three runs of the library while the
resumed build above was writing its sources to the same hard disk crawled
(0 to 33 swaps in 15 s, the player's window stalled with them), and a
static run between them did not; not seen again in eight runs under a
build's load since. The cache belongs on the system drive, as its default
(`%LOCALAPPDATA%`) has it.

Left: the Linux side (`.so`, `posix_spawn`, `ld.lld`) is written but not built
or run; title updates (D3).

## Order

1. Memory (an agent, from Xenia's memory.cc).
2. Kernel core: exports, arguments, counts; the loader (image, imports,
   thunks); the main thread; dispatch; `boot` with the interpreter.
3. What the games call first, family by family (memory, threads and
   synchronization, files on the disc, video and the GPU front end, XAM),
   measured by `boot`.
