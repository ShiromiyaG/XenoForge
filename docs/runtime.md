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
first function to its last ((address - first) / 4: 36 MB for game A
Kart's 4.7M-instruction range, made the first time the image loads and
kept). Dispatch reads one immutable map of those ranges (an atomic
pointer, a new map published at each load and unload, the old ones kept
for threads still reading them): one compare per module loaded and one
load, about 2 ns, where the binary search over one table it replaced took
29 to 79 ns on game A's 73,293 functions. XexUnloadImage drops the range with the pages,
so game C's Splash.xex and Game.xex, one after the other at 0x82A00000, each
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

### The port's hooks

A port's replacements and hooks (docs/recompiler.md, "Replacing and hooking";
`xenoforge/port.h`) sit on this path: `Kernel::try_call` runs a hooked
function's hooks when its generated code has no hook slot (or it has none),
the interpreter asks the kernel at a branch into a hooked function and before
an instruction with a mid hook (`ppc::System`'s `enter_hooked`, `mid_hook`;
`run_original` runs a hooked function's own body, its hooks left out), and
`add_module_code` binds a module's hook slots to the process's registry
(`xenoforge/hooks.h`), `remove_module_code` and the kernel's end unbind them.
The lockstep check takes the port's code as it takes an import
(`hook_boundary`). Ports register in `PortInfo::on_init` (xenoforge/run.h) or
with static registrations, before the guest's first thread.

What the debugging side keeps (plan E5, 2026-11-08; `xenoforge/hooks.h`):

- **Hook frames.** Each run of a hooked function or a mid hook pushes a
  frame on its context's list (on the host stack of the run, linked from a
  head per context, found through a one-entry cache per host thread, so the
  deterministic mode's fibers keep theirs apart): the entry, which hook of it
  runs, r1 and LR as it was entered, and whether it called back into guest
  code (`Original`, `port::call`) that has not returned. Popped by its
  destructor, so an Unwind or a thread's exit through a hook leaves none.
  `hooks::frames(c)` reads them for the reports.
- **Counts.** A counter per address, kind and hook name, kept across
  registrations; one relaxed increment per call while `hooks::set_stats` is
  on (boot's and the player's `--hook-stats`), a load and a branch when off.
- **Owners and pins.** Each hook belongs to an owner: the executable, or a
  module loaded at run time (`port_module.h`), whose hooks
  `hooks::remove_owner` drops together. A run pins the owners of the entry it
  read (`hooks::Pin`: a count of threads between reading an entry and pinning
  it, then a count per owner), so `hooks::drained` says when a module's code
  is no longer run by any thread and it can be unloaded.

## Pausing the guest

(2026-11-08, `Kernel::pause`, `resume`; for reloading the port's hooks.)
Every guest thread parked at a safe point: as it enters or leaves a kernel
call (a thread waiting inside one counts as parked, and does not leave it
until the pause ends), when the deterministic mode hands the processor over,
or at a yield point of the generated code or the interpreter (a loop's back
edge, a priority hint). Outside the deterministic mode the yield points are
not counted, so `pause` turns counting on in each running thread's context
with a budget below zero (written from the pausing thread, as the
deterministic mode's watchdog writes a context; a missed write is made again
on the next round): the thread's next back edge calls `System::yield`, which
parks it and turns counting off again. Each thread says whether it runs
guest code (`Thread::guest_running`: set by `Kernel::call`, cleared around a
kernel call and while parked), and `pause` returns once none does, or after
its timeout with the count of those that did not park
(`pause_unparked`). A thread that goes back to guest code while a pause is
asked parks first (the flag and the thread's state are sequentially
consistent, so a thread starting during the pause cannot be missed). Host
threads that call guest code with no guest thread of their own are not
paused.

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
  releases the pages (game C loads Splash.xex and Game.xex at the same base,
  one after the other). Every DllMain call runs under the loader lock
  (NT's), recursive, also held across XexLoadImage's lookup and load,
  XexUnloadImage's detach and removal and XexGetModuleHandle: a load waits
  for an unload in progress, and no DllMain gets a module that is gone.
  boot's module report names the lock's owner when it is held. Each
  module's code is generated or interpreted as its image matches a table
  of the game library ("Code" above).
- **game C's launcher chain** (`kernel_modules_test`'s `launcher_chain` runs
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
  The second round (2026-11-06, vectors for 259 of the 338 shared exports)
  added: `_vsnwprintf` returns -1 when the text does not fit; the SHA-256,
  SHA-384, SHA-512 and MD5 contexts after Final hold the digest in the state
  words and the padded last block in the buffer, a 64-bit count is two words
  with the low one first; XeCryptHmacShaVerify refuses an expected size past
  20; XeCryptBnQwBeSigVerify verifies (a 0x20-word key, exponent 3 or 65537,
  Montgomery products with no conversion, compared with the
  XeCryptBnQwBeSigFormat block; Xenia's stub says valid); format 3 of
  XeCryptBnDwLePkcs1Format is format 2; IoSetShareAccess, IoCheckShareAccess
  and IoRemoveShareAccess are NT's (the console's file object keeps its
  access in the bits 0x7E of its first word, a SHARE_ACCESS is seven bytes);
  NtAllocateVirtualMemory takes only the type bits the console's does and a
  protection with one of no access, read, read-write, execute-read or
  execute-read-write (INVALID_PAGE_PROTECTION otherwise; NtProtectVirtualMemory
  takes the execute-read modes too), refuses a size that rounds past 4 GB;
  NtFreeVirtualMemory takes exactly decommit or release and a nonzero base;
  NtQueryVirtualMemory a region type of 0 or 1; NtCreateEvent and NtCreateTimer
  a type of 0 or 1; a handle that is no object is STATUS_INVALID_HANDLE for
  NtFlushBuffersFile, NtDeviceIoControlFile, NtQuerySymbolicLinkObject,
  IoDismountVolumeByFileHandle and NtWaitForMultipleObjectsEx; NtClose(-2) is
  refused and NtDuplicateObject leaves its output alone on failure;
  ObReferenceObjectByName answers OBJECT_NAME_INVALID to a name that is not
  rooted; ObIsTitleObject(0) is 0; KeSetAffinityThread of no thread is
  INVALID_PARAMETER. From the console's kernel with its init state built
  (the oracle, third round): an affinity outside the six processors is
  INVALID_PARAMETER; NtYieldExecution and a delay of zero answer 0x40000024
  (STATUS_NO_YIELD_PERFORMED) when no other guest thread is ready, 0 when one
  is (Xenia: 0), and RtlSleep counts it as a sleep done; a mutant is on its
  owner's guest list of mutants (the KTHREAD's head, the mutant's +0x10 entry
  put at the head, unlinked on release with its links left as they were);
  KeWaitForMultipleObjects fills the wait blocks the caller gives (thread,
  object, next block, key, type, as the console's loop does: a wait for any
  stops before the first signaled object, the last block points to the first
  when the wait is satisfied for all or goes on to block); a DPC that has run
  is not inserted and its list entry points at the processor's DPC queue; the
  FPU owner word (PCR +0x10) is -1 and KeSaveFloatingPointState answers the
  previous owner, KeRestoreFloatingPointState puts -1 back; a timer whose due time has passed is signaled in the call that sets it.
- **The debug monitor** (`xbdm.cpp`): Xenia's 36 xbdm exports, a retail
  console's answers (console type -1, no debugger, the end of the module
  list). The imports bind as the other system libraries', but no xbdm.xex
  module is listed, so XexGetModuleHandle("xbdm.xex") fails as on a retail
  console.
- **Crypto** (`crypt.cpp`): every XeCrypt*/XeKeys* export Xenia has, plus
  SHA-384 and XeKeysConsoleSignatureVerification (it trusts the console's
  own signatures, so the XDK's partition block check takes the normal
  path), with Xenia's guest context layouts (the console's where they
  differ: see above), over algorithms written from
  their standards (sha1, sha2, md5, aes, des, bignum; no third-party code).
- **One clock**: the scheduler's (100 ns since the kernel started). The
  interrupt time is it, the system time is it plus the host's time at the
  start, the time base (mftb) is it times 5 (50 MHz); KeTimeStampBundle
  moves with it every millisecond once threads run. On Windows the host
  timer is raised to its finest (0.5 ms), as Xenia does, or the clock and
  short waits would step at 15.6 ms.
- **The host clock** (`src/kernel/host_clock.cpp`) under the scheduler's:
  the CPU's own counter scaled by a 64-bit fixed-point multiply, no lock and
  nothing shared written. x86-64 with an invariant TSC: `rdtscp` (lfence +
  rdtsc without it), the frequency measured against steady_clock over 30 ms
  at the first read; ARM64 Linux/Android: `cntvct_el0` (CNTFRQ checked by
  the same measurement); elsewhere, or with `XF_HOST_CLOCK=steady_clock`,
  steady_clock. Xenia reads QueryPerformanceCounter and takes a mutex per
  read (its raw TSC mode takes the frequency from CPUID, Intel only).
  A bare `rdtsc` costs half as much (9 against 18-21 ns here) but runs ahead
  of a load still missing the cache: a thread that saw another's store and
  then read it got values up to 500 ns older than the other's read before
  the store (`host_clock_test`), a negative interval to a guest timing across
  threads; so the fence stays. On a host whose QueryPerformanceCounter is
  the TSC (frequency 10 MHz) the read costs about what it did (20.5-21.3
  against 21.1-23.4 ns); the gain is the ARM64 counter and no system call
  where the OS clock is not the TSC. Deadlines go to the host's waits as the
  time left, so the measured frequency's few ppm never end a wait early.

## Threads and objects

Each guest thread is a host thread with its own context, a guest stack, a
PCR (r13) and TLS as Xenia's XThread sets them up. Kernel objects (events,
semaphores, mutants, timers, threads) live in guest memory where the game
put them (KEVENT and friends) and are backed by host objects found by the
guest address, as Xenia's XObject does. The deterministic mode (fibers on
one host thread, plan "Threads") runs on the same interface ("The
deterministic mode", below).

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
- **The scheduler** is an interface (`Scheduler`, objects.h: start, join,
  block until woken or a deadline, wake, yield, give up a slice, the clock,
  timers changed; and the spin-wait hint's hand-over, a preemption's yield
  point and an affinity changed). The host scheduler gives each guest thread
  a host thread with a 16 MB stack and a clock thread (timers,
  KeTimeStampBundle, every millisecond); the time base (mftb) is the same
  clock. The deterministic mode's fiber scheduler replaces it, and so does
  the console scheduler (below, `XF_SCHEDULER=console`).
- **The console scheduler** (`console_scheduler.cpp`, 2026-11-09;
  `XF_SCHEDULER=console`): the guest's threads scheduled as the console's
  kernel schedules them, on six processors for the Xenon's six hardware
  threads, rather than each a host thread for the host to schedule (Xenia's
  way, and the host scheduler's). The model is Cemu's (the Wii U's threads
  as fibers on a host thread per core), written anew.
  - A processor is a host thread (started at its first thread) with a fiber
    per guest thread on it. A thread is on the processor KPCR CURRENT_CPU
    names when it is made (ExCreateThread's mask, else its parent's, as
    Xenia's GetFakeCpuNumber), or the one KeSetAffinityThread names before
    it first runs: a thread made suspended is held until resumed, so the
    XDK's CreateThread suspended, XSetThreadProcessor, ResumeThread puts it
    where the game asked. A thread that has run stays on its processor's
    host thread (the kernel's code reads host thread-locals, whose addresses
    the compiler may keep across a call that would move it); the moves asked
    then are counted. The thread-locals the kernel keeps per guest thread
    (the current thread, the exception walks) are swapped at each switch.
  - A wait that blocks, a yield, a thread's end: its processor switches to
    the next ready fiber there in user mode, no host wait or wake; with
    nothing ready it sleeps on its condition variable until a wait's
    deadline or a wake from elsewhere. A wake from another processor or a
    host thread puts the fiber on its processor's queue and wakes the
    processor if it sleeps.
  - Each processor runs its ready fibers by their KTHREAD priority, first
    come first among equals. One made ready above the one running preempts
    it at its next yield point (a loop's back edge, a priority hint, as
    `Kernel::pause` stops threads: the yield points' counting turned on for
    that thread, an atomic flag the generated code loads at each); one that
    has run 2 ms with another as high ready is preempted there too, as a
    quantum ends; one that has waited 8 ms runs next whatever its priority
    (Xenia gives every thread to the host with the priorities ignored: no
    game is left waiting on a low thread a high one starves). The
    spin-wait hint (db16cyc, every 2,048 in a row), a yield and a slice given
    up hand the processor to the others ready there, whatever their
    priority, and queue behind them.
  - The kernel's own threads whose body is host code (the GPU's, the
    sound's, XAM's workers) stay host threads of their own, waiting on
    their condition variables as the host scheduler's; so does guest code a
    host thread calls. The loader lock, a host lock held across guest code,
    gives its processor up while it waits (a fiber must not wait on the
    host for another fiber of its processor).
  - The clock is the host's; a clock thread moves the timers every
    millisecond, makes the waits of busy processors whose deadline passed
    ready, and asks for the preemptions.
  - The thread report prints its counts (switches, waits, yields, spins
    handed over, preemptions, moves) and each processor's host CPU time.
    `kernel_threads_console` runs the threads' tests on it.
  - Measured (2026-11-09), game D's played drive (`bench_vs_xenia.ps1
    -Configs ours-host`, the drive's press script, window 125-200 s), one
    run at a time: the host scheduler 27.55 fps, frame p95 50.1 ms, CPU
    2.74 s a second (an earlier run 28.63, 49.3, 2.79); the console
    scheduler 29.17, 35.6, 2.63 and, again, 27.68, 50.3, 2.82. No gain it
    keeps, so the host scheduler stays the default. Its 40 s boot of game
    D: 46 threads, 65,229 switches, 61,916 waits blocked, 2,314
    preemptions (1,202 for a higher thread made ready), 787 threads run
    starving; processor 0 (2 threads, one of them the game's render thread
    polling the GPU) 38.9 s of CPU in 40, the other five 13.4 s together.
- **Polling threads nap.** A zero-interval KeDelayExecutionThread and
  NtYieldExecution go through `give_up_slice`. Xenia answers both with
  `Sleep(0)` (and a 100 ns NtDelayExecution for threads below normal
  priority, which on Windows its priority mapping never reaches), and so did
  we with `std::this_thread::yield`: both return at once when the host has
  nothing else to run, so a guest thread polling a flag between zero delays
  keeps a host core busy as long as it waits (game A: 1.3 M zero delays a
  second, one core). The host scheduler now counts a thread as polling once
  it has done nothing but yield, with at most 20 us of guest work between
  yields, for 200 us; its yields then nap (NtDelayExecution of one unit:
  the timer's next tick, 0.5-0.6 ms; 50 us on POSIX) until a gap longer than
  20 us shows it working. The answers and which threads count as ready are
  unchanged; the poll notices its flag up to a nap later. The fiber
  scheduler keeps its yields. `XF_SPIN_NAP=0` turns it off; the thread
  report prints the naps. Game A (attract, 25 s window, contaminated by
  other agents' load): CPU 1.24 -> 0.42 s a second (Xenia D3D12 1.33), 30 fps
  all, frame time p5-p95 no wider; KeDelayExecutionThread 39.0 M -> 0.59 M
  calls in 30 s. Game D's spin is in guest code (two threads of its media
  DLL, no kernel calls), which this does not reach: 1,711 naps in 30 s.
- **Safe points**: a thread suspended by another, or with kernel APCs
  queued, stops at its next kernel call (Xenia suspends the host thread
  wherever it is). User APCs run in alertable waits; DPCs run on the
  calling thread as the IRQL falls below DISPATCH_LEVEL (at once from an
  insert at IRQL 0, as in Xenia; one inserted at a raised IRQL waits on the
  PRCB's queue in guest memory until KfLowerIrql or KfReleaseSpinLock lowers
  it, as the console's), except those of timers (KeSetTimerEx), which run on
  a kernel thread of their own. ExTerminateThread unwinds the host
  stack to the thread's start (a C++ exception).
- `Kernel::launch` makes the main thread (the executable's stack size and
  TLS header) and starts it; `boot` waits for it and reports the threads.

### Objects the guest makes, and device objects

Xenia has no ObInsertObject (its table entry is unimplemented) and its objects
are host objects behind handles. The console's XAM makes its enumerators,
sessions and voices with ObCreateObject (a type of its own: allocate, free,
close and delete procedures in its data) and ObInsertObject, so this is NT's
object manager as the console keeps it (`guest_objects.cpp`, `devices.cpp`;
`tests/kernel_ob_test.cpp`):

- **A made object** is a `GuestObject` in the object table: its body in guest
  memory after an X_OBJECT_HEADER (and, named, an X_OBJECT_HEADER_NAME_INFO
  holding the last name element), registered by ObCreateObject, which keeps the
  whole path the object will be inserted under. The pointer and handle counts
  are the header's own, and the type's procedures are called as NT calls them
  (on a guest call from the exporting thread, the lock released).
- **ObInsertObject(object, attributes, bias, handle)** (the console's XAM
  passes the handle pointer fourth; the attributes' name, else the creation's
  path, names it): one handle opened (written even when the pointer is null),
  handle count 1, pointer count = creation's + bias (the handle's reference
  replaces the creation's). A name that exists is
  STATUS_OBJECT_NAME_COLLISION, or with OBJ_OPENIF (0x80) a handle to that
  object (types must match: STATUS_OBJECT_TYPE_MISMATCH) and
  STATUS_OBJECT_NAME_EXISTS; the new object is dereferenced and deleted in
  every failure. A name not rooted is STATUS_OBJECT_NAME_INVALID. OBJ_PERMANENT
  (0x10) keeps the name, and a reference, with no handle open until
  ObMakeTemporaryObject. There are no directory objects: the directories of a
  name are not looked for, and an object under a missing one is inserted.
- **References**: ObReferenceObjectByHandle (the type checked against the
  header's, STATUS_OBJECT_TYPE_MISMATCH), ObReferenceObject and
  ObReferenceObjectByName count in the header; ObDereferenceObject at the last
  runs the delete procedure then the free procedure (an object never inserted
  goes the same way); ObOpenObjectByName and ObOpenObjectByPointer open
  another handle (NT's: handle and pointer count one more, where Xenia hands
  back the same handle); NtDuplicateObject counts both; NtClose closes, runs
  the close procedure (object, handles left: the Xbox's two-argument form,
  from NT's five), then dereferences. A type no host kind stands behind
  (dispatcher headers of an Ex type made with ObCreateObject, then
  KeInitializeEvent) is not made into a host event: its handle waits nowhere.
- **Device objects** are such objects of IoDeviceObjectType, permanent and
  named, as IoCreateDevice makes them (Xenia's layout: type 3, size, driver,
  the mounted or self device, flags, extension, device type, stack size; a name
  that exists is a collision, IoDeleteDevice makes it temporary and deletes it
  when nothing refers to it). At start there are `\Device\Cdrom0`,
  `\Device\HdDvdRom`, `\Device\Harddisk0\Partition0`, `\Partition1`, `\Cache0`
  and `\Cache1` (a list of ours: the console's drivers make more, which nothing
  asks for yet). The file systems stay the VFS's: a device object is what a
  driver names and XAM looks for. IoDismountVolume answers
  STATUS_VOLUME_DISMOUNTED (0xC000026E), as the console's, to what is not a
  device object, and success to one (nothing is taken from the VFS);
  IoDismountVolumeByName finds the device as ObReferenceObjectByName does with
  IoDeviceObjectType (invalid name, not found, mismatch) and dismounts it.
- **XexUnloadTitleModules(flags)** unloads every module that is not the
  kernel's or a system module (`load_module`): DLL_PROCESS_DETACH to the DLLs
  that attached, the last loaded first, then their pages and loader entries
  whatever their counts, the executable's too (there is none until
  XexStartExecutable names the next, which also sets the executable handle, the
  image name, the TLS and the stack size from it). What the flags select is not
  known (not read), and the title's threads are the caller's to end.

### Stacks

A guest thread may move between guest stacks: the XDK's fibers
(SwitchToFiber saves the registers into the current fiber, loads the next
one's and branches to KeSetCurrentStackPointers, which moves r1 to the next
fiber's stack; game F's streaming decompressor runs on one). Each guest stack
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
  generated; the XDK's fibers start at a routine's start (game F's at
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
- **The deterministic mode** (fibers on one host thread) resumes a guest
  thread on the host fiber it handed over from (a guest stack's, or its
  own), and a thread that is a fiber already is used as it is, not made a
  thread again at its end. The fiber primitives are stacks.h's, shared.
- Xenia runs a fiber switch on the thread's one host stack: its JIT can
  enter any address, so KeSetCurrentStackPointers drops the host frames and
  re-enters the thread at LR (`XThread::Reenter`). rexglue's fork replaces
  the XAPI's CreateFiber, SwitchToFiber and ConvertThreadToFiber, hooked by
  address, with host fibers of its own; here the game's own XAPI code runs,
  and only the kernel export it ends in is the system's.

### The deterministic mode (plan A5, 2026-11-07)

`src/kernel/deterministic.cpp`, chosen before launch with
`Kernel::make_deterministic(Deterministic)` (`boot --deterministic`,
`XF_DETERMINISTIC=1` or `=paced` for boot and the player, through
`game::open`). Written anew: Xenia has no such mode.

- **One host thread, a fiber per guest thread.** The scheduler's host thread
  is a fiber that picks the next ready thread and switches to it; the thread
  switches back where the kernel sees it: a wait that blocks, a yield
  (NtYieldExecution, a spin lock's spins, the loader lock's waiters), and the
  end of its slice or a deadline come due, checked at each kernel call
  (`Kernel::run_import`), time base read (the `time_base` callback),
  indirect call (`Kernel::dispatch`, which the generated code's `bctrl`s
  reach) and the recompiler's yield points (a loop's back edge, a priority
  hint `or rx,rx,rx`; the `System::yield` callback, docs/recompiler.md,
  "Yield points"). The thread that ran goes to the tail of the ready queue, woken
  threads join it in the order they were woken, timed-out waits end in the
  order the threads were made. Nothing is handed over at DISPATCH_LEVEL or
  above (NT does not preempt there; the video family holds a host mutex per
  CPU around an interrupt's callback).
- **A virtual clock.** A kernel call costs 1 us of it, a time base read and
  an indirect call 100 ns, a loop's back edge 1/32 of 100 ns (about ten
  Xenon cycles) and a priority hint 8/32, a slice is 1 ms (all
  `Deterministic` fields; `yield_units` 0 stops counting the yield points);
  when
  every thread waits, it jumps to the earliest deadline (a timeout, a timer).
  KeQueryPerformanceCounter, mftb, KeQuerySystemTime (from a fixed date,
  2025-01-01), waits, timers, KeTimeStampBundle (every millisecond of it),
  vblank, the sound worker and XMA all run on it. The sound clients' outputs
  answer what has played from a pacing output on that clock
  (`VirtualClockOutput`; the player's device still gets the frames).
  Unpaced (boot), it runs as fast as the host goes; paced (the player), it
  never runs ahead of the host's clock.
- **What else would differ run to run**: XeCryptRandom and XNetRandom take a
  fixed seed; the loader lock's owner is the guest thread, not the host
  thread (all are one). Not covered: the storage root's contents (saves left
  by an earlier run; boot's default is a temporary directory), a fresh
  profile's random XUID, the network.
- **Yield points.** The XDK's spin loops on a flag (with the Xenon's
  `or r1,r1,r1` / `or 31,31,31` priority hints) make no kernel call, time
  base read or indirect call; the generated code and the interpreter count
  each loop's back edge and each hint (docs/recompiler.md, "Yield points"),
  so such a spin runs its slice out on the clock and hands over at the same
  instruction every run. Playing, a yield point is a load and a branch.
- **Spins, playing (2026-11-09).** `or 31,31,31` ("db16cyc", the XDK's
  spin-wait hint) is the host's spin-wait hint too (`ppc_ops.h`'s
  `spin_hint`: x86's pause, ARM's yield, as Xenia's DELAY_EXECUTION, so the
  SMT sibling gets the core), and every 2048 of them in a row the thread
  gives the host processor to whatever else is ready on it
  (`System::spin`, `std::this_thread::yield`; back at once when nothing
  is), beyond Xenia, which pauses only. Game D's render thread spins in such
  a loop while it waits for the GPU (a full core before): the played drive
  went from 27.4 to 28.6 frames a second and 3.25 to 2.79 s of CPU a second
  (Xenia 30 and 1.96).
- **Spins on the GPU sleep (2026-11-09).** That loop is Direct3D's wait
  for the GPU: 32 hints, then a read of what the command processor writes
  back to guest memory (the read pointer's copy, a fence's value), with no
  kernel call; the hand-over above returns at once on a host with idle
  cores, so the thread still kept one busy for as long as the GPU took. A
  thread whose hand-overs have followed each other (less than 1 ms apart)
  for 200 us while the command processor has commands to run now sleeps
  until the processor's next write-back or interrupt
  (`CommandProcessor::Host::wrote_back`, `Video::write_backs`,
  `Scheduler::spin_wait`), 1 ms at most, and not at all when one came
  since its last hand-over. With the processor idle the spin is handed
  over as before. On the console scheduler the thread's processor runs
  its other ready fibers first and sleeps with none. `XF_SPIN_WAIT=0`
  turns it off; the GPU's report counts the sleeps. Game D's played drive,
  one run each, the same session: off 27.83 frames a second, p95 50.3 ms,
  2.83 s of CPU a second; on 27.24, 50.5, 2.15; on, with the console
  scheduler, 27.48, 50.3, 2.12. A quarter less CPU, the frames unchanged
  (they wait for the GPU either way), and the console scheduler no better
  than the host's with it.
- **Forced hand-overs**, a safety net that should no longer fire: a watchdog
  host thread stops a thread that has not handed over for `force_after_ms`
  (100) of the host's time if it is in the generated code at IRQL below 2
  (`SuspendThread`, a call to a hand-over on its stack, `RtlRestoreContext`
  to go on), and it sleeps as long on the clock as it spun. Where it stops
  depends on the host's speed, so a run with one is no longer exact; the
  report counts them. Windows only (TODO: a signal handler on POSIX).
- **Halting and checking.** `--until-swap N` / `--until S` stop the run at
  the first hand-over after swap N or S seconds of the clock, every thread
  parked; boot then prints `guest memory hash` (`Kernel::memory_hash`,
  FNV-1a over every committed readable region). The thread report ends with
  the scheduler's counts (runs, slices, yields, waits, idle jumps, forced
  hand-overs) and names a thread that has run a second without handing over.
- **Input**: recorded and replayed (docs/input.md, "Recordings"); in this
  mode a replay repeats the recorded run.

Measured with the yield points (2026-11-07, headless, beside another
agent's builds; "before" is the code just before them, the playing runs
measured back to back with the same load, the deterministic ones from the
runs above them in this file's history, on game-libs 7ac73df):

| game, run | swaps | host time | swaps/s (host) | clock | forced |
|---|---|---|---|---|---|
| Game A, playing, `--seconds 20` | 597 | 20 s | 30 (30 before) | (the host's) | |
| Game A, deterministic, `--until-swap 1200` | 1200 | 16.2 s | 74 (60-63 before) | 40.42 s | 0 (7 before) |
| Game B, playing, `--seconds 20` | 802 | 20 s | 41 (40 before, same load) | (the host's) | |
| Game B, deterministic, `--seconds 30` | 8118 | 30 s | 271 (12 before) | 144.6 s | 0 (209 before) |
| Game B, deterministic, `--until-swap 1200` | 1200 | 8.4 s | 143 | 28.07 s | 0 |

Three runs of each game to swap 1200 left the same memory hash and the
whole report alike but for host times (game A 17951072 runs,
150981 slices; game B 85548 runs, 22722 slices). Before the yield points,
Game A was exact only when its 7 forced hand-overs landed in pure
spins, and game B (whose main thread spins before its first kernel call)
never was. The cost of one host thread is more than paid back by the idle
clock not waiting for the host's.
`kernel_threads_deterministic_test` runs the threads' scenarios on this
scheduler; `kernel_deterministic_test` checks a mixed workload (sleeps,
ping-pong, slices, a spin on a flag with the watchdog off, handed over by
its yield points alone) and a recording and its replay, run after run.

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
  every run, which slows a game that kicks often (game F's client fell to
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
  enumerators. Left: the hash and signature checks (Xenia does none), and
  deleting an SVOD's `.data` with it. Writing STFS is below, "The storage root".
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
  and `content/<XUID>/<TITLE>/<TYPE>/<name>` for a package: an STFS file
  (below), or a directory with its header at
  `content/<XUID>/<TITLE>/Headers/<TYPE>/<name>.header` (Xenia's
  XContentContainerHeader). The user is Xenia's default profile (XUID
  B13EBABEBABEBABE), or the first profile Xenia made there.
- **STFS writing** (2026-11-08; `src/package_write.cpp`, xam_content.cpp). Saves
  and other content a title creates are real STFS packages, the files the
  console keeps (`CON ` magic, read-write hash tables with a primary and a
  secondary table at each level, the file table first, consecutive file blocks,
  SHA-1 hashes up to the volume descriptor's top hash, the content ID the SHA-1
  of the metadata), read by `xf::Package` and by Xenia's reader (whose
  `stfs_container_device` this follows; Xenia itself writes none: it keeps
  directory packages). While a title has one open it is a directory device in
  `<storage>/content-work/...` (extracted from the file, or new); XamContentClose
  and XamContentFlush write the package file whole (to `<name>.tmp`, then a
  rename: never half a package), the close removes the working directory, and
  one found at an open (a run that never closed it) is what the title sees,
  so a crash loses nothing. A new package is written empty at once, so it exists
  and lists. A package of another form (LIVE, PIRS, SVOD, a directory) stays as it
  is, read-only or writable as before. `ContentFormat::Directory`
  (`set_content_format`, boot's `--directory-saves`) keeps Xenia's directory
  packages for new content. The CON signature is zero: signing needs the
  console's private key from its key vault, which a player's machine and this
  repository do not have (the keys at the player's disposal are the 1BL's, which
  signs nothing); Xenia's reader and ours do not check it, a console would refuse
  the package. Names are 1 to 40 Windows-1252 bytes (a longer one fails the write
  and keeps the working directory, with a message); no blocks are left free. Not
  done: more than 4.9 M blocks (the format's three hash levels), a console's
  in-place transactions (the package is rewritten whole at each flush).
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
its overlapped, and sends "off" 200 ms later). Kinect's UI is refused (no sensor), the party,
community and Gold upgrade pages fail.

**A host for the dialogs** (`UiHandler`, kernel.h; 2026-11-08). `set_ui_handler`
gives the kernel a callback that is handed each request (a `UiRequest`: the kind,
the user, a message box's title, text, buttons and default, the keyboard's default
text and room, the device selector's type and size, the other player's XUID and the
recipients) and an `UiDone` to call with the answer (a `UiResult`: the button, the
text, the device, or cancelled), now or later, from any thread. It returns false to
leave a request to the default. While it holds one the title's overlapped stays
pending (`begin_overlapped`, finished by `complete_overlapped` when `UiDone` runs;
without an overlapped the call waits for it), XamIsUIActive answers 1, XN_SYS_UI is
"on" and goes "off" when the answer comes; a cancelled dialog completes its
overlapped with ERROR_CANCELLED (0x4C7), as the XDK documents for a dialog the user
backs out of, and a page the title does not wait for (sign-in, achievements, gamer
card, the friends pages) returns success at once and runs its follow-up (the sign-in
notification) when it closes, not when it is cancelled. With no handler every dialog
answers as before. The overlay itself (drawing the requests on the player's
window) is the player's work; XNotifyQueueUI's toasts (printed now, by
xam_info.cpp) would be drawn by it too.

The pages Xenia does not implement are in: friends (`XamShowFriendsUI`, `...UIp`),
players, messages, quick chat (`...UIp`), game invite, friend request, player
review, message compose, gamer card for an XUID (`...UIp`), custom message compose
and custom player list. The console's XAM with no user (the oracle) answers
ERROR_ACCESS_DENIED to the first group, 0x65B (ERROR_FUNCTION_FAILED) to the gamer
cards and the achievements (Xenia answers the gamer card ACCESS_DENIED and the
achievements NO_SUCH_USER), and ERROR_INVALID_PARAMETER to the two custom pages
given null arguments, before it looks for the user; the recipients of an invite or
a message are read only for a user that is signed in (the console's XAM reads them
first: a bad pointer faults there). With a user the pages are shown and closed at
once, headless; what else the console checks (the custom pages' arguments, what a
non-Live profile is shown) is not known without a signed-in console.

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
APC. Game E polled
recvfrom on an unimplemented socket (an answer of 0, an empty datagram)
and stalled its frame loop there; with these it goes on.

## XAM's oracle

The console's own XAM is the system update's `$flash_xam.xex` (plan,
"Accuracy beyond Xenia"): Game A's disc carries 2.0.16202
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
VdDisplayFatalError 0x12247). Why it is always the dashboard was not found
beyond this: XamLoaderLaunchTitleEx (0x8169F268 in 2.0.16202) keeps the path
for flags 0 and stores a request type (0xA or 0xB), and the launcher's thread
function (0x8169CD98) loads the request's path, and `\SystemRoot\dash.xex`
(or `\Device\Flash\dash.xex`) goes through a function (0x8169A8D8) that may
swap in another dashboard; which step replaces a given path was not traced.

With the device objects (the second round, 2026-11-06) the launch no longer
stops in the loop of the worker's: the kernel here has `\Device\HdDvdRom` now,
IoDismountVolumeByName and ObReferenceObjectByName on it answer, and the
worker goes on to ask the drive (IoSynchronousDeviceIoControlRequest 0x4D014,
not implemented: answered 0) and to poll on a delay, as a worker of the
console's does. The launch loads the executable (XexLoadExecutable,
`\SystemRoot\dash.xex` being the synthetic title), checks its privilege,
starts a thread that calls ExTerminateTitleProcess(0, ...) (not implemented)
and, as it returns, ends in VdDisplayFatalError 0x12247 (0x8169F164: the
state machine's status was not the one it waits for). XexUnloadTitleModules and
XexStartExecutable are not reached. What is left is the kernel's process per
title: ExTerminateTitleProcess (the title's threads ended, its modules
unloaded) and the launch's status; XexStartExecutable (a thread of the title
process at the module's entry, naming the executable) is in place for the day
it gets there. ObTranslateSymbolicLink, which the launch calls on
`\SystemRoot`-style names, is not implemented either.

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
  ("if we pretend to be old we have less to implement"); game A and
  game B import XamGetSystemVersion and both boot as before (same swaps per
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
- **ObInsertObject** (done after this survey: "Objects the guest makes",
  above). XAM makes each of its enumerators, sessions and voice objects with
  ObCreateObject and ObInsertObject; without a handle its
  ObReferenceObjectByHandle answered STATUS_INVALID_HANDLE and the error paths
  of the enumerator calls and XamProfileFindAccount read through a null. The
  survey run again on the new build against the old one (233 match, 128
  differ, 5 need state, 17 fault, 13 of them the console's side, before):
  236 match, 130 differ, 5 need state, 12 fault (8 the console's side), and
  the variants' faults 3 to 0. Five faults on enumerator calls
  (XamContentAggregateCreateEnumerator, XamContentCreateDeviceEnumerator and
  its variant, XamContentCreateEnumerator#1, XamContentCreateEnumeratorInternal)
  and XamCreateEnumeratorHandle now match; XamProfileCreateEnumerator,
  XamProfileFindAccount and XamUserLogon#1 went from a fault to a difference.
  The 0x65B answers did not move (24 both times): they are the console's XAM
  asking its worker for a user it has none of (XamUserCreateAchievementEnumerator
  makes and inserts its object, references it, closes the handle, then
  answers from the worker), the HLE's answer to compare with, not the
  kernel's. The faults left on the console's side (XamFree, XamTask*,
  XamAvatarInitialize, XamNuiSkeletonGetBestSkeletonIndex,
  XamProfileGetCreationStatus, NetDll_XNetRegisterKey, XMsgCancelIORequest) do
  not go through Ob.
- **A profile and a signed-in user.** A profile is an STFS package on a
  content device (Content\<xuid>\FFFE07D1\00010000\<xuid>); XAM learns of
  the device from DrvGetContentStorageNotification, whose structure is not
  known, and XamProfileCreate needs it. XamUserLogon with a made-up XUID
  faults on the console's side. Until that is built the user calls are
  compared with none, and the content calls (XamContentGetDeviceData and
  State answer 0x57 for device 1 on the console, a device ours has; Create
  answers 0x57 before it looks for a user, ours 0x525) are the oracle's
  lack of a device, not ours.
- **The console's own launch** (above): past the device objects now; it ends
  at ExTerminateTitleProcess (a process per title) and the launch's fatal
  error 0x12247.
- The UI calls (0x65B against our headless answers), XamVoiceCreate (the
  console makes a voice object with no microphone, ours is Xenia's stub,
  ERROR_ACCESS_DENIED), XNetGetTitleXnAddr's MAC (XAM's own, different on
  each run), XNetLogonGetTitleID (the console's XAM answers 0 with the title
  running; ours the title's: the reason was not read): left as Xenia's
  design, noise or open.
- XamTaskSchedule and CreateThread with the generic arguments run garbage as
  a callback on our side; whether the interpreter stops before the call
  returns varies (one "faults (ours)" in the last run was CreateThread).

## The title's end, the profile and the console's settings (plan step 7, 2026-11-08)

**A process per title.** A title ends the run through `XamLoaderTerminateTitle`,
`XamLoaderLaunchTitle`, `XamContentLaunchImage` or `ExTerminateTitleProcess`
(Xenia has the first two only, and asks the user to restart). Each runs the
routines registered with `ExRegisterTitleTerminateNotification` (the highest
priority first, r3 = 0, on the calling thread; Xenia never runs them; the
console's order and arguments are on the hardware list), records the exit in
`Kernel::title_exit` (launched path, flags, exit code, the launch data the title
set with `XamLoaderSetLaunchData`) and stops the run. The host starts the next
title as a process of its own: `boot --follow-launches` takes a path on the disc
(`game:\`, `d:\`, `\Device\Cdrom0\`) and runs boot again on the same disc with
`--xex <path>`, `--launch-data <file>` (what the next title reads through
`XamLoaderGetLaunchData`, `set_launch_data`) and the time that is left. A launch
to the dashboard or to a package on a content device ends there with the
request printed. The player does not follow launches yet. `ExTerminateTitleProcess`
(the console's XAM calls it from its launcher with 0; the argument is the exit
code, as far as the oracle shows) is new.

**The profile in its package** (`gpd.h`, `xam_user.cpp`). Profile settings a
title writes (`XamUserWriteProfileSettings`, the dashboard's user preferences)
and the achievements it earns are kept in the user's GPD files in the profile's
package, Xenia's layout: `content/<XUID>/FFFE07D1/00010000/<XUID>/<title>.gpd`
(`FFFE07D1.gpd` for the dashboard's). A GPD is an XDBF file (header, entry
table, free table, data; settings in section 3, achievements in section 1),
read when first used and written aside and renamed at each change, so a run that
ends in the middle leaves the old file whole. Where Xenia reuses the holes of its
free list, a file here is laid out afresh at each write. The title's
achievements come from its SPA (the executable's resource named by the title ID:
the XACH table and its strings in the console's language, else the SPA's
default, else English), added locked to the GPD at first use;
`XGIUserWriteAchievements` earns each one: its unlock time is the guest's system
time, and the log says `xam: achievement unlocked: "<name>" (<n> G, id <id>)`
(the overlay's log is the only UI for now). `XamUserCreateAchievementEnumerator`
lists them from the GPD, locked and unlocked, with the label, description and
unachieved description as UTF-16 behind the records when its flags ask for them
(Xenia's layout: 36-byte records, the strings after `items_per_enumerate`
records). Not done: gamer pictures, the titles-played list in the dashboard's
GPD, stats, a GPD inside an STFS package (a directory package still).

**The console's settings** (`SystemSettings`, misc.cpp). The player's choice of the
user's language and country, the screen size (640x480, 1280x720, 1920x1080), the
widescreen flag and the refresh rate (50 or 60 Hz) are written into the console's
own XConfig settings before the title runs (config.toml's `[system]`: `language`,
`country`, `width`, `height`, `widescreen`, `refresh`; boot's `--language`,
`--country`, `--resolution WxH`, `--widescreen 0|1`, `--refresh 50|60`), so what a
title reads through XGetVideoMode, VdQueryVideoMode, XamGetLanguage and
XamGetCountry follows, as it would on a console set up that way. 50 Hz writes the
PAL AV region (standard 3, bit 23). The settings a title writes with
`ExSetXConfigSetting` and `ExReadModifyWriteXConfigSettingUlong` are kept in
`<storage>/xconfig.bin` (the deterministic mode starts from the defaults and writes
nothing), read at the start; what the player chose wins over them.

**Kernel speed (2026-11-08).** Every import call used to build the export's
`library!name` string, lock a kernel-wide mutex and bump a map entry; with game
A's main thread spinning in `KeDelayExecutionThread(0)` at millions of calls a
second (44.3 M in 20 s) that was the bulk of the call. A count is now one relaxed
atomic add on the `Import` itself (`CallCount`), summed by name when a report asks
(`Kernel::calls`, `unimplemented`): the same boot made 72.5 M of those calls in
20 s (1.6 times the rate, same 30 swaps a second). Game B's kernel calls are
about 1 M in 20 s; its time is the game's and the disc's. Left: `yield_status`
(the answer a yield gives) walked the thread list under the dispatcher's lock on
every yield; it is now one atomic read of a count of ready threads
(`Thread::refresh_ready`, kept under the lock where a thread starts, ends, blocks or
wakes). Game A, 25 s, the same machine: 37.1 s of CPU before (6.1 kernel) and 28.6 s
after (5.3 kernel), Xenia's 30.4 s (7.5 kernel); the swaps are 27-30 a second in
both. A per-thread look at games A and B in steady state found no other kernel hot
spot: A's spinning thread costs 0.23 us a call (the import's counters, the
safe-point checks and `SwitchToThread`, 0.8 s of kernel in 15 s), B spends 99% of
its CPU in generated code.

**Exports the console's XAM imports and Xenia lacks, as they come:**
`RtlAppendStringToString` and `NtDeleteFile` (NT's; the second answers
OBJECT_NAME_NOT_FOUND, ACCESS_DENIED on a read-only device and
DIRECTORY_NOT_EMPTY). The other 173 the oracle lists (`xfxam <xam.xex> imports`:
XeKeys*, XInputd*, Hal callbacks, Nic*, Mtpd*, Etx*, Drv*, LDI*...) answer 0 as
before: their arguments and results are not known without the console, and a
guess would be a stub with a name.

## Exports nothing implemented (2026-11-09)

Xenia implements 310 of xboxkrnl's exports and 382 of xam's; the console has
more, and games import or look up some of them. The eight test discs' imports
and a run of each (docs/lean-kernel.md, the last section) found 47. Each
answers as the console's own kernel and XAM do with the device absent (XAM's
answers checked with `xfxam call`); the tests are `tests/kernel_absent_test.cpp`, and the answers of
the Winsock side stay with `kernel_net_test`'s threads. Files:
`src/kernel/kernel_absent.cpp` (xboxkrnl), `src/kernel/xam_absent.cpp`, and
next to their neighbours `xam_net.cpp` and `xam_input.cpp`.

| Export | The console's answer here |
| --- | --- |
| `PsCamDeviceRequest(request)` | by the request's type (a word, 0 to 0x69): 0, 10, 29, 38, 45 succeed (0 and 29 write a status in the second word, 0 and not-connected); 13 is `0xC00000A3`; 1-3, 5, 8, 14, 15, 18-21, 24, 25, 27, 30, 33-37, 40, 41, 43, 44, 46, 47 are `0xC000009D`; anything else `0xC0000010` |
| `RmcDeviceRequest(request)` | a u16 type and a u16 receiver: receiver 4 or more `0xC000000D`; type 0 succeeds and zeroes the second word; any other `0xC000009D` |
| `XInputdFF*` (8) | the handle's top nibble must be 1 (else `0xC0000001`); ports 0-3 `0xC0000010`, others `0xC000009D`; `XInputdFFCancelIo` the first for every port |
| `EtxProducerRegister`, `Unregister`, `Log` | the producer registry the console keeps (128 slots, the block's layout: size, flags' high half, events and count, id, the slot in the top ten bits of the word at +0x18); no session is ever open, so `Log` bumps the event's drop count and answers `0xC0000188`; a block under 0x40 bytes `0xC000000D`; a full table `0x8000001A` |
| `LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression` | the cabinet library's LZX on frames (`LzxStream`, `include/xenoforge/lzx.h`): a context starting `'CIDL'`, the block maximum, the configuration's second word; the first configuration word is the window in bytes (a power of two from 0x8000 to 0x2000000; 0 is taken; a window in bits is "not enough memory", 1); the source buffer's minimum is the block plus 0x1800; a frame past the block maximum is 3, a context that is not one 2, a frame the decoder refuses 4 |
| `XamInputGetUserVibrationLevel(user, level *)` | the user's pad as the HID layer says (a pad that is not there `0x48F`); the level is the default, 3 |
| `XamInputRawState` | `0x48F` |
| `XamVoiceSetMicArrayIdleUsers`, `XamBackgroundDownloadSetMode` | 0 |
| `XamNuiCameraElevationSetAngle`, `StopMovement`, `RememberFloor` | `0x8007048F`; `TiltReportStatus(s)` returns `s` |
| `XamReadBiometricData`, `XamWriteBiometricData`, `XamContentInstall` | `0x57` |
| `XamUserGetAgeGroup(user, out *)` | `0x57` for a null pointer, else `0x65B`, signed in or not |
| `XamUserNuiGetEnrollmentIndex(user, out *)` | users 0-3: `0x80070525` and -2 written; a null pointer or a user past them `0x80070057` |
| `XamCacheOpenFile`, `CloseFile`, `Reset` | open: a null structure or a cache id other than 2 (at +4) `0x57`; id 2 is a partition the console has and holds no such file (2, the byte at +2 set to 0x40); close -1; reset of -1 is 0, of any other id 1 |
| `XamXlfsInitializeUploadQueue`, `Uninitialize`, `MountUploadQueueInstance`, `UnmountUploadQueueInstance` | `0x80070005`, `0x80150005`, `0x80070006`, `0x80070006` (initialize's is read from the code's no-service branch: the console, started beside this one, faulted in its init path) |
| `XCustomGetCurrentGamercard`, `GetLastActionPressEx`, `SetDynamicActions`, `UnregisterDynamicActions` | no gamercard selected (xuid 0, FALSE); no action pressed (outputs zeroed, `0xE8`); a set of more than three actions `0x57`, a clearing one 0, a real one `5` (no session); unregister returns nothing |
| `NetDll_XNetConnect`, `XNetServerToInAddr`, `XNetUnregisterInAddr`, `XNetQosGetListenStats` | WSAEINVAL (10022), whatever the caller or address, started or not |
| `NetDll_XNetGetConnectStatus` | 3 (XNET_CONNECT_STATUS_LOST) |
| `NetDll_XNetQosLookup` | nothing to look up: 0, no result; secure addresses without their arrays: WSAEINVAL; else an XNQOS (count, none pending, each entry flagged complete) and the event set at once (the console probes; nothing answers offline) |
| `NetDll_getpeername` | no such socket: SOCKET_ERROR (WSAENOTSOCK); not connected: WSAENOTCONN; else the peer's address, length 0x10 |

Where it is not the console's: `LDICreateDecompression` does not call the
caller's allocator (the context is in the caller's buffer, or the system heap
when one was given); the event trace registry is kept host-side; the flag the
console sets in a producer's block from the processor's mode is not set; the
vibration level is the default whatever the user's setting; the XNQOS's
entries say complete where the console's, in a network that answers, would
say what the probe found; the `getpeername` error codes for a socket that is
not there are Winsock's (the console's call went on to a routine whose error
was not read).

Also followed from the console, in the same round (the exports that were
Xenia's guesses): `_snprintf` and `_vsnprintf` with no buffer format and
answer the length (the count is not looked at; with a buffer and no room, -1,
as before); `FscSetCacheElementCount(cache, count)` refuses a count past
0x400 (invalid parameter); `StfsCreateDevice(parameters, size)` wants the
0x58-byte structure with its version byte (+0x50) at 5 or more before it looks
at anything else; `XeCryptBnQwNeRsaPubCrypt` computes with a key of one to five
words (Xenia refuses under 512 bits; the larger ones follow Xenia, the console's
own code stopping on state that is not built).

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
Game A's main thread). XAM's input answers from an idle pad for
user 0, a controller plugged in with nothing pressed, so a game waiting
for one goes on (game B paused on "Controller Lost Connection" without
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
the texture cache's textures, memory and loads a second, the process's
resident memory and the device's memory allocations, to set boot and the
player side by side over a run.

The thread report (at the end, and with `--stall-report S` whenever no
swap has come for S seconds, 5 S before the first, once a stall) gives
each thread's name (the host's, or what the guest named it with
SetThreadName), its state, what a waiting thread waits for (each object's
kind, handle, body, type and signal state) and its guest stack: the saved
LR of each frame on the back chain, read where the pages are readable. A
title's main thread often waits for good on its game thread (game F's
`main` waits at lr 0x820CCFA8, in WaitForSingleObjectEx, for its
MAIN_THREAD), so the thread to look at is the one that stopped, not the
main one. Booting a DLL as the executable (`--xex Game.xex` on game C) says
so: its entry point is a DllMain that returns at once.

Game F with its game library (2026-11-06): the intro at 60 swaps a second;
2,300 to 2,750 swaps in, MAIN_THREAD loads the main menu itself (running,
16 frames deep; 0 to 6 s with no swap, longer as the machine is busier),
then the menu at 30 swaps a second, 6,349 swaps in 180 s. Its start was
run 30 times beside other load (16 beside 12 busy processes, 8 beside two
writers flushing to the hard disk the disc image is on, 6 under a game
library's build): the first swap came at 2 to 11 s every time.

### Crash and stop reports

`include/xenoforge/crash.h`, `src/kernel/crash.cpp`; test
`kernel_crash_test`. `install_crash_handler` (called by `game::launch`, so
boot and the player have it; before any write watch, so memory's fault
handler comes first in the chain and the handler sees only the faults
memory does not take) catches access faults, illegal instructions and
integer division by zero: a vectored exception handler on Windows, signals
on POSIX (SIGSEGV, SIGBUS, SIGILL, SIGFPE; compiled only there, not yet
built on a POSIX host; TODO there: the faulting host pc from the ucontext,
so a generated function is named). It writes `crash-<date>-<time>.txt` into
the crash directory (`XF_CRASH_DIR`, boot's `--crash-dir`, else the current
directory), calls the tool's hook (boot and the player print the report,
the run's report and the summary) and ends the process with exit code 3.

The report (`make_crash_report`, which takes no lock the faulting code may
hold: the kernel's lock and the rings' are only tried, a thread's stack is
read inside its own range) gives:
- what: the access (reading, writing, executing; the host address and the
  guest address it is);
- the thread: id, name, start address, stack range;
- where: for a fault in generated code, the guest function whose host code
  is nearest below the faulting host address (the generated code does not
  keep the pc; the context's pc is only the last call's, so it is marked so);
  in the interpreter, its exact pc. Each guest address is shown as
  `module+offset` and `function start+offset` from the module tables;
- the stack: LR, then the return addresses by the r1 back chain
  (`guest_backtrace`, the walk `thread_report` also uses), each as a
  function, with the port's hooks running on the thread between them
  (`hooks::frames`): a hook entered with r1 at some frame sits above the
  frames saved below it, as `port: <kind> hook <hook> on <function>
  <address> (<module>)` with its host code, followed by where the guest
  goes on after it (a function hook's LR, its caller; a mid hook's own
  address), and the walk goes on across it. A frame saved at the hook's r1
  holds that same LR, or the native-call return address (`port::call`'s
  and `Kernel::call`'s 0xBCBCBCBC) when the hook called guest code itself,
  and is not shown twice;
- a fault in the port's own code (the innermost hook has not called back
  into guest code): "faulted in: the port's before hook ... on ...", the
  host code address and the hook's, each as `module+offset` (Windows'
  `GetModuleHandleEx`, POSIX's `dladdr`), and the r1 and LR it was entered
  with; the guest context printed is as the hook left it;
- the last kernel calls (`note_kernel_call`, a ring of 16 per thread filled
  in `run_import`: library, export, the LR it was called from, "not
  implemented" marked), this thread's in full and the others' last 6;
- the other threads' state, pc and LR.

A stop (an invalid instruction, an instruction with no semantics, the
interpreter's step limit, KeBugCheck, XamLoaderTerminateTitle: all through
`Kernel::stop`) keeps the report of the thread that stopped it in
`Kernel::stop_report` and its one-line stack in `stop_stack`; boot writes
and prints it. The `xf-summary` line carries `stack` (the one-line stack,
`pc (function+offset) <- ... <- lr ...`) and `report` (the file) for
crash and stopped; xfcompat reads them into the rows' `stack` and `report`
columns (JSON and CSV).

Where it differs from what could be had: an export that is missing returns
0 as before (stderr says so once, and the call shows in the ring marked); it
does not stop the run. An export that fails with an error status is not a
crash.

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
  The window presents only when there is something new to show: a fresh
  picture, the overlay or a dialog of the system (and once more when they
  close), or a window event that asks for it (shown, uncovered, resized,
  moved to another display). A guest that stops swapping, as at a loading
  screen, leaves the last picture on screen with no present, as Xenia's
  presenter paints only on a guest output refresh or a paint request.
  (Until 2026-11-08 it also presented the last picture again every 100 ms,
  which a frame timer such as `xfbench` saw as a run of 101-102 ms gaps
  where Xenia showed one long one; renderer.md, "Game D's gaps".)
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
- **The F1 pages** (since 2026-11-09 F1 opens the guide, in its look, on its Settings page, with the pad as well
  as the mouse and keyboard; a second F1 closes it and saves): Settings (volume, mute, vsync, smooth scaling,
  fullscreen, the picture options, the port's options) and Debug (the guest's swaps per second, the
  frames presented and the window's refreshes per second, the sound frames
  played (and how many had sound), the input states delivered, each user's
  controller (what drives it, its buttons, triggers and sticks), the frame-time graph, the native renderer's and
  the hooks' counters, and a screenshot button). F11
  switches fullscreen; F12 saves the window's next picture (copied back
  from the GPU) as `<title>_<swap>_key<n>.png` in `player.screenshots`
  (default: a `screenshots` folder beside config.toml, never the
  repository). With a port drawing its own frames (docs/sdk.md, "Native
  rendering"), F6 switches between them and the emulated ones, and the
  overlay shows which and what was skipped.
- **config.toml** (`settings.cpp`; `--config`, else SDL's preference folder,
  `%APPDATA%\xenoforge\config.toml` on Windows), written with the defaults
  the first time and saved when the F1 pages close:

  ```toml
  [video]    # fullscreen, vsync (true), smooth (true), aspect ("auto", "integer" or "stretch"), and the
             # picture options below: resolution_scale (100), min_scalable (16), upscaler ("none"),
             # fsr_sharpness (0.2), cas_sharpness (0), antialiasing ("none"), anisotropy (0),
             # texture_filter ("game"), edram ("fast" or "exact"), frame_graph (false),
             # performance (false)
  [audio]    # volume (0-100), mute
  [input]    # keyboard (true), keyboard_user (0), left_deadzone, right_deadzone (0-1), vibration (true)
  [keyboard] # each pad control's keys (input.md)
  [player]   # screenshots: the folder F12 writes to
  [launcher] # games: the folder the launcher lists
  [games.5846000A.video] # a game's own settings (the launcher's), any of the four sections above
  ```

### Picture options (2026-11-08)

What players expect from modern emulators (RPCS3, Xenia, Dolphin), beyond
the console and never changing what the game computes: everything the CPU
reads stays at the console's size. All of them are in config.toml's
`[video]` (shared and per game), the launcher's Video tab (Display,
Rendering, Overlay) and the player's System page (the F1 overlay's
"Picture" section and the guide's System page, in the guide's look),
from one table of choices (`settings.h`'s `video_choices`):
- **Resolution scale** (`resolution_scale`, 50-800% in steps of 25, RPCS3's
  model) and **minimum scalable dimension** (`min_scalable`, console
  pixels, RPCS3's): the renderer's `Options::resolution_scale` and
  `min_scalable` (docs/renderer.md, "Resolution scale"). Taken when the
  title starts: the pages say "restart the title" and offer it. Where the
  renderer draws at another scale than asked (the interlock path takes
  multiples of 50%; a GPU without room for EDRAM at the scale), the F1
  overlay and the guide show its notice and the player prints it.
- **Output scaling** (`upscaler`, RPCS3's separate "output scaling"): a
  plain blit (`smooth` picks linear or nearest), AMD FSR 1 (EASU in passes
  of at most 2x, then RCAS with `fsr_sharpness` stops of sharpness
  reduction) or AMD CAS (sharpening, resampling to the window's size, with
  `cas_sharpness`), as Xenia's presenter offers them (its
  guest_output_ffx_* shaders, ported to compute in
  `frontend/present_effects.comp`; AMD's FidelityFX headers, MIT, in
  `third_party/FidelityFX-*`). FSR asked for where the window is no larger
  than the picture takes CAS, as Xenia's does. **Antialiasing**
  (`antialiasing`): FXAA 3.11 at Xenia's two presets (NVIDIA's header,
  BSD, `third_party/fxaa`), with green as luma (Xenia puts luma in the
  alpha in its gamma pass). The effects run on the window's thread into
  images of the destination rectangle's size, then a 1:1 blit; live.
- **Picture shape** (`aspect`): fitted, whole multiples (integer scaling;
  fitted where not even 1x fits), or stretched; live.
- **Render targets** (`edram`, as the game starts): `fast`, the default,
  draws into host render targets (Xenia's default; game D's played drive:
  8.1 ms of GPU a frame); `exact` has the pixel shaders read and write
  EDRAM themselves under fragment shader interlock (blending, the float
  formats, 20e4 depth and the stencil as the console's; 10.6 ms), host
  render targets where the device lacks it. The player's choice only:
  boot, the replays and the tests keep the renderer's own default (exact
  where the device can), and `XF_RENDER_TARGET_PATH` forces either.
- **Texture filtering** (`anisotropy`: the game's, or 1-16x forced;
  `texture_filter`: the game's, trilinear, nearest): live, through
  `Renderer::set_texture_filtering` (the samplers made as draws ask). The
  anisotropy and trilinear apply where Xenia's `anisotropic_override` does:
  mipmapped textures (not a base map alone) the game filters linearly;
  nearest is everywhere (a look, not a fix).
- **Performance display** (`performance`, off; F3 switches it while
  playing, and the launcher's and the guide's Picture settings): over the
  picture's top left corner, the game's frames a second (its swaps), the
  average and worst time between the frames the window showed with their
  graph (the last 240), the frames shown a second, the command processor's
  work a frame without its waits (`gpu::Counts::run_ns`) and the draws a
  frame. Drawn with the window's pictures, and once a second when none is
  new.
- **Frame-time graph** (`frame_graph`): the Debug page plots the intervals
  between the frames the window showed (the last 240), with the average and
  the worst.

Considered and left out: a frame-rate unlock or a vblank rate above the
console's. The guest's frame pacing is its own (its vblank interrupt,
`VdSwap`, the scheduler's clock): raising the vblank rate makes most games
run their logic faster (physics and timers counted in vblanks), so it is
not safe as a general option; a game's port can do it where the game
measures time (docs/sdk.md). Vsync is the window's only (the guest never
waits for the display). Not done yet, cheap and asked of emulators:
a pixel shader of the player's own after the picture (CRT filters), an
HDR output; and per-game "unlock" presets belong in ports.

For checks without a person at the screen: `--quit-after <seconds>`
ends the run with a report (the swaps, the front buffers taken on the GPU
and not, the frames presented, the window's refreshes and its waits, the
input states delivered, the sound frames submitted, sent to the device,
with sound, dropped and starved, then boot's report); `--screenshots <dir>`
writes every `--screenshot-every N` swaps' front buffer (300 by default)
as `<title>_<swap>.png` (the renderer's read-back, boot's `--shots`
picture) and the window's picture of the same swap beside it as
`<title>_<swap>_window.png` (the converted picture, before the output scaling and antialiasing: to see
those use `--window-shots`, which copies back the swapchain's image), printing the time on the scheduler's clock
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

- **game A**: the intro movie at 10 to 20 swaps a second, each front
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
- **game B**: no "Controller Lost Connection": in 90 s, 738 swaps, the
  Cartoon Network logo by swap 300 and Papaya's by 600 (where boot used to
  stop swapping before 600); one XAudio client at 187.5 frames a second,
  3,867 of 16,160 with sound. It polls XamInputGetKeystrokeEx with
  `ANY_USER | KEYBOARD`, which no driver here answers (as in Xenia).
- Left for a person at the screen: that a real pad and the keyboard move
  things in the game (only their drivers' states and the scripted presses
  are checked here), that the sound is heard and sounds right, how the
  window feels resized, fullscreen and with vsync off.

## The player's system UI

Plan step 7 (2026-11-08), the player's side of the dialogs `xam_ui.cpp` hands to a host (`set_ui_handler`,
"XAM"). Xenia draws a message box, the keyboard, the device selector and sign-in and cancels or stubs the rest;
here every page the kernel asks for is shown, driven by the pad and the keyboard, and answered through `UiDone` in the
kernel's own result shapes. The logic is apart from the view:

- **`frontend/system_ui.*`** (the model, no ImGui, in `xf_frontend`): `SystemUi::handle` is the `UiHandler`. It
  queues each request (from any guest thread), makes it a `Dialog` (title, paragraphs, choices, selection) and, when
  the window's thread navigates (`Nav`: up, down, left, right, accept, cancel, page up and down), answers it:
  message box = the button (B: cancelled, the kernel's ERROR_CANCELLED), keyboard = the text cut to its room in
  characters (UTF-16 for the guest; the OK/Cancel choices), device selector = the hard drive (device 1) or cancel,
  sign-in = sign in as the profile or cancel (the title's follow-up runs only on the first), gamer card and
  achievements from the profile (`kernel::profile_summary`: the user's GPD for the running title, read on the guest's
  thread that asked), and the pages an offline console shows with nothing to list: friends, players, messages, quick
  chat, game invite, friend request, player review, message compose (both), the custom player list, the
  marketplace ("not available"), profile editing. An unknown request (`UiKind::Other`) is declined: the kernel's
  headless default stands. `PadNavigator` turns the pad's state into navigation (press once; a direction repeats after
  400 ms, every 100 ms; the left stick counts). `toast()` takes the kernel's toasts (`set_toast_handler`: an
  unlocked achievement with its icon, and `XNotifyQueueUI`'s text); they live five seconds.
- **`frontend/system_ui_view.*`** (ImGui draw lists, in the player): the top dialog as a lit panel over the dimmed
  game, easing in (title under XenoForge's mark or the message's icon, a green rule, the text, the choices stacked as
  items with the focused one lit green, the controller's A and B in the footer), the toasts as capsules rising at the
  bottom centre (the mark at the left, the achievement's picture at the right), the achievements as rows
  (`draw_achievement_row`: picture, name, description, a gamerscore pill). The PNG icons become textures on first use
  (`IconCache`: stb_image, a staging buffer and one copy on the shared device). A click on a choice accepts it; Enter
  in the keyboard's text field is the field's own.
- **The look** (`frontend/guide_theme.*`, 2026-11-08, the owner's request): an original look in the spirit of the
  console's dashboard of its time, nothing of the console maker's art, fonts or marks. Inter (the launcher's copy in
  `ui/fonts` beside the executable or above it; ImGui's own font without it) at sizes made for the window's height at
  start; panels with a soft shadow and a top-lit gradient, items lit green when focused, A/B/X/Y as coloured discs,
  XenoForge's mark drawn as vectors (the ring of light and the anvil, `assets/branding`). `push_widgets` gives ImGui's
  own widgets the same look inside the guide.
- **The windows' icons** (`frontend/window_icon.*`): the launcher's windows carry XenoForge's mark (built in from
  `assets/branding/xenoforge-256.png` by `cmake/embed_bytes.cmake`); the player's window carries the running game's
  own image and name from its executable's XDBF (`title_icon`, `title_name`), the mark when it has none. The
  executables' icon on Windows is `frontend/xenoforge.rc` (`assets/branding/xenoforge.ico`), when CMake finds a
  resource compiler (llvm-rc beside clang).
- **The guide** (F2, the controller's Guide button, or Back and Start held together for half a second: Windows' Game
  Bar often takes the Guide button): the console pages as a panel sliding in from the left, the profile on top (the
  mark, the gamertag, the title's gamerscore), three pages switched by LB/RB, Page Up/Down or the mouse:
  Achievements (the rows above), Profile and System (a setting's name at the left, its control at the right, green
  section captions). While it is open the title gets neutral controller states and no keyboard, as with a dialog;
  ImGui's own pad and keyboard navigation drives it; B or Escape closes it and saves config.toml if anything changed.
  A dialog the title opens closes it. `--ui-demo guide`, `guide-profile`, `guide-system`, `guide-settings`, `guide-debug` open it at the start.
- **The game stays in charge of its own time.** While a dialog is up the player gives the title neutral controller
  states and no keyboard (`Controllers::set_taken`; the console's guide takes the controller from the title), and
  `XamIsUIActive` answers 1 until the answer; the guest's threads, the sound and the scheduler's clock keep running
  (nothing waits for the window; a title that opened a dialog with no overlapped waits on its own thread only).
- **The guide's console pages** (`frontend/overlay_system.*`; F1 adds Settings and Debug to them, `Host::extra_pages`): System (language, country, resolution,
  aspect, refresh), Profile (the gamertag with its rules, the profiles with Use and Delete, a new profile, the saves'
  format) and the title's achievements, read only. The same choices are the launcher's System and Profile tabs
  (`console_options.*`, `profiles.*`): one list of languages (XLanguage), countries (XOnlineCountry) and video modes
  and one validation (`valid_gamertag`: 1 to 15 letters, digits and single spaces, starting with a letter, no space at
  the end; `clamp_system` puts values the kernel would not know back to its default). In the player the system
  settings go to the kernel at once (`apply_system_settings`: xconfig.bin) but a running title reads them at its start,
  so the page says so and offers "Restart the title"; the gamertag (`set_gamertag`) and the saves' format
  (`set_content_format`) apply live; choosing or deleting a profile needs the restart.
- **Profiles** are directories of the storage root, `content/<XUID>/` with the package
  `FFFE07D1/00010000/<XUID>` (where the kernel keeps the GPDs); the gamertags are config.toml's `[profile.names]`,
  the one the games use `[profile] xuid` (else the first the storage has, as the kernel picks) and `gamertag`. Creating
  one makes a new offline XUID (0xE03...) and its directory; deleting removes the directory with its saves (asked
  twice, never the only profile). The launcher's Profile tab also lists the selected game's achievements from the
  profile's GPD file (`read_gpd_achievements`, no kernel running).
- **config.toml**: `[saves] format` = `"stfs"` (default) or `"directory"` (Xenia's layout; `--directory-saves`),
  `[saves] root` (the storage root; by default `storage/` beside config.toml, where the player used to use a temporary
  directory), `[profile]` as above, `[player] follow_launches` (default true). Per-game tables may override `[saves]`,
  `[profile]` and `[system]` as they do the other sections.
- **A title that launches another** (`XamLoaderLaunchTitle`; `Kernel::title_exit`): when the executable is on the same
  disc (`game::disc_path`) and `follow_launches` is on, the player starts a process of its own for it (SDL3's
  `SDL_CreateProcess`, `game::relaunch_args`: the same command line with `--xex`, `--launch-data` and what is left of
  `--quit-after`), hides its window and waits for it, then exits with its code, as `boot --follow-launches` does; the
  launch data the title left goes through a file (`--launch-data`). A launch to the dashboard or to another disc ends
  the run. What decides the next title is `disc_path` and how it is started is the caller's, so a host that runs the
  game in the launcher's process (Android) can start it its own way.
- **For checks without a person**: `--ui-demo message,keyboard,device,signin,achievements,gamercard,friends,
  messages,players,quickchat,gameinvite,friendrequest,playerreview,compose,customcompose,playerlist,marketplace,
  editprofile,toast,console` opens a sample of each at the start (`console` opens the overlay's console section;
  answered by `--press a@5`, `b@5`...; the player prints each answer, `ui-demo: <name> answered: ...`, and every
  dialog a title opens, `ui: dialog kind N ... opened at S s`). `--window-shots <dir> [--window-shot-every S]` saves
  the window itself with the dialogs and the overlay over the picture (`--screenshots` saves the picture without
  them; the presenter's `capture_window_next`). `--restart-after S` presses the overlay's "Restart the title" at S
  seconds. `--no-system-ui` leaves every dialog to the kernel's default. `tests/system_ui_test.cpp`
  drives the model, the kernel's exports through the handler, the pad's repeat, the toasts, the profile store and the
  settings, and the relaunch's arguments.
- **Left for a person**: the dialogs' look and feel with a real pad and the mouse; the keyboard dialog's text field
  with a physical keyboard (a pad alone cannot type: it accepts the default text); that a dialog the title raises
  mid-game takes the pad without a stray press reaching the game on the frame it opens; the launcher's System and
  Profile tabs with the mouse and the pad; the title restart the overlay offers.

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
    [--demo-speed X] [--show-log] [--capture-binding <name>] [--menu <name>] [--context-menu] [--tab <name>]
    [--render-size WxH] [--scale F] [--assets <dir>]
```

### Start-up

The window is created hidden and shown once its first picture has been presented, so it never opens black. What
runs before that (`launcher --startup-times` prints each step, in milliseconds since the program began): SDL's
video, the window, the Vulkan device (the interface's 26 pipelines compile against a pipeline cache kept in
`ui_pipelines.bin` beside config.toml, which took the step from about 600 ms to about 190 ms), the swapchain (the
slowest single step, about 230 ms, made on another thread while the fonts and the document load) and the first
frame. The controllers' subsystem (a quarter of a second of drivers) starts after the first picture; the pads
already connected arrive as events. On the development machine, a warm start reaches its first presented frame
at about 0.58 s (it was 1.65 s). The library scan was already on its own thread, and the games' pictures are
read by it, not by the first frame.

### The launcher's interface (`frontend/ui`, `launcher_view.*`, `settings_view.*`)

The main window follows a mockup the owner gave on 2026-11-07, in the manner
of the console's own dashboard (it replaced the first TV-style layout and the
desktop-tool one after it), drawn for a window 1240dp wide:

- **The main window** (`launcher.rml`, `launcher.rcss`): a header (the
  wordmark, and the sections: *games* and *settings*, the pages of the
  window, one of them lit; *help*, which opens its menu
  under it); under it a strip twice the window's width with the library and
  the settings side by side, which slides from one to the other (below); in
  the library, the search box (above the list, there while it has games),
  filtering on every word of the title, title ID and file name; a
  toolbar (Play, which becomes Stop while a game runs; Play interpreted; Prepare, Rebuild or Try
  preparing again by the selected game's status; Open Folder, Rescan,
  Settings, Controls; at its right the views: the list, the grid, which is not
  there yet, and the keyboard's and the pad's settings); the games as glossy
  rows (cover, name, status, title ID, version and an Options button that
  opens the row's menu: Play, Play interpreted, Prepare, Game settings, Open
  the disc's folder, Copy the title ID or path; the same menu on a right
  click) under a header whose labels sort, again for descending, the selected
  row in Xbox green; the hints for the pad (A, B, X, Y) and the keyboard;
  and a status bar (the count or the scan's progress, the folder, which opens
  the folder dialog on a click, the running game with Stop, a message that
  fades, a build's progress while its dialog is hidden). With nothing to
  list, the table says why (no folder, no games, a search that hides them
  all) with the button that helps. What the mockup shows and is not here yet:
  its own window buttons (the system's frame is used).
- **The backdrop**: the window's background is not an element's: the renderer
  draws it under the interface at the start of every frame
  (`Renderer::set_backdrop`, `ui.frag` `MODE_BACKDROP`; `#app` and `body` are
  transparent). It is the grey gradient the window used to wear, so the text
  keeps its contrast, with six broad ribbons of silk in layers, in fixed
  colours, as in the mock: at the left a thick dark grey one (#6d6d6f) and a
  thin light grey one (#aaaaaa), at the right four greens of different tones
  (the accent's, darker and lighter; the colours change only when the accent
  does, never with the selected game). They lean up to the right, sway slowly
  and pass over and under one another; the header, the toolbar and the game
  rows are translucent (alpha in `launcher.rcss`'s gradients), which lets the
  ribbons through behind them. A ribbon is always
  full width: a strip of constant width along a curve in space (it sweeps down
  the window, leans up to the right, sways and winds round a spiral that turns
  with the clock) whose width's direction twists about the curve, so it shows
  its other face (a little darker) where it turns, goes edge-on and passes
  over or under itself; it does not thin out and fade. The grey gradient is
  one full-screen pass (`ui.frag`, no texture, a function of the position);
  the ribbons are triangles built on the CPU every frame
  (`frontend/ui/ribbons.cpp`, `build_ribbons`: 150 sections by 9 vertices
  across, lit per vertex with a diffuse term and a silk sheen, a perspective
  camera 2.2 window heights away). They are opaque and drawn with a real depth
  test (the depth in the first texture coordinate, `imode.w = 2` in `ui.vert`;
  the depth is cleared in the pass after the gradient), not sorted, so folds
  and crossings are exact to the pixel; a hair of noise in `ui.frag` hides the
  steps of 8 bits in their gradients. Each edge has a one-pixel fringe of
  alpha 0, drawn after, tested against the depth but not written, blended, so
  the edges stay crisp (no glow, no blur); with multisampling on (below) the
  fringe is not drawn, the samples do that work. Sizes are in window heights. `[launcher] backdrop`:
  `animated`, `still` or `off` (the grey alone), in the Interface tab, read by
  `LauncherView::update_backdrop` every frame; the clock stops while the
  window is minimized, hidden or covered (focus does not matter).
- **Smooth edges**: the window's layers are drawn with 4x multisampling (RmlUi
  draws tilted and rounded shapes, such as the header's diamond, as plain
  geometry and leaves the edges to the renderer). `Renderer::set_msaa`, from
  `[launcher] smooth_edges` (the Interface tab; on unless the file says off),
  read every frame by `LauncherView::update_backdrop`; the most of 8, 4 and 2
  not above the request that the device takes is used, else none. Each layer
  has a multisampled image beside its own: the passes draw into the first, with
  the stencil (so the clip masks are smooth too) at the same count, and resolve
  into the second as they end, which is the image everything else reads; the
  samples are kept, so the next pass over the layer loads them. The filters'
  images (blur, shadow, masks) stay at one sample, with their own pipelines
  without a stencil: their passes are whole-window quads. Changing the count
  makes the targets and the pipelines again at the next frame.
- **Motion**: the dashboard's way of moving, in short times with an ease-out
  (cubic out, as the console's blades; its own blade shuffle is about 180 ms
  and the strip's slide here 340 ms). The selection fades from one row to
  the next (`.tr` transitions its fill, border and margins; the glow is a
  box-shadow, which RmlUi cannot animate, so it is a child `.glow` whose
  opacity is animated, `LauncherView::fade_glow`, only on the row that is or
  was just selected). A list that has just appeared (the start, a new
  folder, a rescan, a new sort) comes in row by row, 40 ms apart, from 28 dp
  to the left and fading in (`animate_rows_in`: each row holds still at
  nothing for its turn and then rises, because RmlUi shows an element at
  rest during an animation's delay). A tab's page comes in from the side the
  tab lies on, 40 dp and fading in (`SettingsView::slide_page_in`); the
  strip's page left behind dims to 30% while the other comes up; menus drop
  in, dialogs rise and grow a little while the window dims (`@keyframes` in
  theme.rcss). A box-shadow cannot change over time, so a state whose
  shadows differ snaps while its fill fades.
- **Pointers** (`ui/cursor_file.*`, `SystemInterface::load_cursors`,
  `frontend/ui/assets/cursors/`): the console browser's, from the set by
  MiniYak_YT that the author released to the public domain (README.txt beside
  the files; the .cur and .ani are shipped as they came, renamed). They are read
  at start as they are (a .cur is a DIB with a hot spot, an .ani a RIFF of
  .cur frames with a time per step) and made SDL colour cursors, with a
  twice-as-large copy for a high-density screen; RmlUi's `cursor` property
  picks one by name: `default` the grey drop, `pointer` the hand (what can
  be clicked), `text`, `unavailable` (the disabled toolbar buttons),
  `progress` (over the build's bars) and `wait` the animated ones, and the
  resize, move and crosshair ones. A name without a file keeps the system's
  cursor, so a missing folder costs only the look. The reading is tested
  (`frontend_test`) on made files and on the shipped ones.
- **Sounds** (`ui_sound.*`, `[launcher] sounds`, the Interface tab): a tick as
  the selection or the focus moves, a swish for a tab or between menu titles
  and a longer one for the strip's slide, a note up for choosing and a note
  down for going back, short pops for a menu or a dialog opening and
  closing, a click for a check box, a low pair for what cannot be done or a
  build that failed and three rising notes for one that finished. They are
  synthesized when the device opens (`synth_cue`: sine notes with their
  harmonics and a swept band of noise, scaled to a peak of 0.2 to 0.3; nothing
  recorded or taken from any system, so no sound file ships), mixed by one
  SDL3 audio stream on the default device; the same cue twice within 30 ms
  plays once. A window drawn offscreen (`--render-size`) opens no device.
- **Dialogs** over the window (theme.rcss's `.dialog`): preparing a game (the
  stages, the bar, the time, the file, the log; Run in background hides it
  behind the status bar's progress, Cancel stops it; then Play, or Try again
  and Play interpreted), About (the licenses), and messages.
- **The settings page** (`settings_view.*`, `settings.rcss`, `#spage` of
  `launcher.rml`): the second page of the window's strip, beside the library
  (`#slide`, `left` from 0 to -100% with a transition, as the console's
  dashboard slides from one blade to the next; the page left behind is
  `visibility: hidden` once it is out of sight, so nothing focuses it). Not a
  window: it lives in the main document and context (`SettingsView::attach`
  gives it the document each time it is made again; the keys, the pad and
  the mouse reach it as LauncherView's input does, and `LauncherView`'s own
  click handler leaves what is inside `#spage` and `#popup-layer` to it).
  Tabs Video, Sound, Controllers, Keyboard, System, Profile (see "The player's
  system UI"), Interface, which are the
  toolbar's buttons; under them, whose settings these are and the "Custom
  settings for <game>" check box; rows like the games' (check boxes,
  sliders and dropdowns in groups, the one with focus in the selected row's
  green); the 25 bindings, each changed by a click and the next key; the
  theme, the accent and the games folder. Saved as they change; the look goes
  back to the window through `SettingsHost`. Escape, B or the *games* section
  slide back. Its look is fixed like the library's (`settings.rcss`, every rule
  under `#spage`); only its dropdowns' lists follow the theme.
- **Desktop scale and place**: sizes are dp at the display's scale
  (`SDL_GetWindowDisplayScale`: 100%, 150%...), not the window's, so a
  bigger window shows more rows; the window opens where it was left
  (`[launcher] x, y, width, height, maximized`, kept only while that place
  is on a display), and the sort is kept too (`sort`).
- **Themes**: the whole window follows `[launcher] theme`: `system` (the
  desktop's light or dark, followed live through
  `SDL_EVENT_SYSTEM_THEME_CHANGED`), `light` or `dark` (the Interface tab).
  Each is a palette of tokens (`palette()` in `launcher_model.cpp`: the
  window, its chrome, lines, three levels of text, the states, the selection
  from the accent, and the console look's glass, glossy buttons, slots and
  the text over the backdrop) filled into the stylesheets when a document is
  made; changing it makes the document again. The accent's glossy greens
  (`{ac-*}`, tones of the accent from `accent_from_hex`) fill the selected
  row, the focused setting, the main buttons and the menus' hot item. Dark is
  grey glass over the dark-to-silver backdrop; light is pearl glass under
  graphite text over a plain `#3e4241`-to-`#919191` one (`Backdrop::light`:
  `ui.frag` takes the ramp from `imode.y`). What sits straight on the backdrop
  (column heads, group titles, the hints at the foot, which carry a dark
  `drop-shadow` for relief) is white in both. The modals are the same in both
  but for their content: a `#889193`-to-`#4e5454` header under white text, the
  content white under `#4f5655` text (inverted in the dark theme), the main
  button a flat `#008001`, and under the block, as wide as it, the pad's
  A Select and B Back (`dialog_html`).
- **Icons**: Lucide (ISC; `ui/fonts/Lucide.ttf`, its license beside it), a
  font of outline icons drawn as text; `{i:name}` in a template and
  `icon_html()` give its glyphs (`launcher_html.cpp` lists the ones used).
- **Controller buttons**: the hint bars' A, B, X, Y, LB and RB are the Xbox 360 pictures of Xelu's Free
  Controller & Key Prompts (CC0; `ui/glyphs/`, license beside them). `LauncherView::load` registers them
  with the renderer as `mem:pad-<button>`, and the stylesheet draws them as `decorator: image(...)`.

Input: the mouse as a desktop program's; the keyboard: the arrows, Page Up
and Down, Home and End move the selection, Enter plays, F2 opens the game's
settings, F5 rescans, Ctrl+O the folder, Ctrl+F the search, Ctrl+, the
settings, Alt+E or Alt+H or F10 the menus (then the arrows, Enter, Escape),
Shift+F10 or the menu key the row's menu; on the settings page the arrows
move focus and change a value, Tab the next tab, Ctrl+Tab the previous, Escape goes back to the library. A pad:
the d-pad or stick moves the selection (repeating), A plays, X the game's
settings, Y prepares, Select (Back) opens the selected game's menu (Rescan is
in it), Start the Help menu, the shoulders a page, the triggers (L2, R2)
switch between the games and the settings; in a menu the d-pad or the stick
(one step a push) moves, X or A chooses, B, Select or Start close it; in
dialogs the d-pad moves and B closes; on the settings page the shoulders
change tabs and B goes back; Up and Down leave the games folder's text box
(Left and Right stay the caret's), and the accent's row, one row like the
dropdowns', takes Left and Right to choose a colour and A for the next. A
look change (theme, accent) makes the document again; the row that had the
pad's focus gets it back (`SettingsView::detach` keeps its `data-key`). The hint bars show these with the Xbox 360
pictures; `--keys` takes `lt` and `rt` too.

The strip that slides between the two pages moves by `translateX(-100vw)`, not
`-50%`: RmlUi does not resolve a percentage in a transform again when the
window's size changes, and a window maximized or made fullscreen on the
settings page kept the old width's offset (the page cut off at the right).

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
- **`launcher_model.*`, `launcher_html.*`**: what the windows say (a game's
  status, durations, the accent's shades, the themes' palettes, the table's
  columns, sort and filter, the bindings' names, the spatial
  navigation) and their structure as RML strings, as plain functions with no
  RmlUi in them (`frontend_test` checks them). **`prepare_job.*`**: a build
  on its worker thread, as state to read (no UI, no SDL).
- **`launcher_view.*`, `launcher_screens.cpp`, `launcher_input.cpp`**: the
  main window's document: the table rebuilt when what it shows changes (a
  signature of the rows), the menus, the dialogs, the status bar, clicks,
  keys and the pad into actions. **`settings_view.*`**: the settings page,
  its widgets and its input.
- **`ui/assets/theme.rcss`** (the base, the controls, menus and dialogs both
  windows share), **`launcher.rcss`**, **`settings.rcss`**, and the `.rml`
  template: the look, with `{tokens}` filled from the palette and the accent
  (six swatches, `[launcher] accent`, green by default). `ui/fonts/`: Inter
  (SIL OFL 1.1), five weights, Roboto Condensed (Apache 2.0), three, for the
  games and the toolbar, and Lucide's icons (ISC), each license beside it.

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
  one, in the `player_<game>` that carries its `default.xex`, else Play
  prepares it first (the recompile step) and, if that fails, the generic
  player interprets (the list says which: "Runs in"). `player_for` in
  `library.h` is that choice.
- **Settings and controls**: the selected game's settings, edited in the
  player's own config.toml and saved as they change: the video (fullscreen,
  vsync, scaling, shape) and sound (volume, mute) of the player's overlay,
  and the controls: the gamepads connected, vibration, the sticks'
  deadzones, the keyboard as a controller (on or off, for which user) and
  each of the 25 bindings (typed, or "Set" and the next key pressed;
  "Xenia's keys" puts the defaults back). These are the shared settings
  unless "Custom settings for" the game is ticked: then a
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
its subfolders (over a port's folder, the extracted `assets\default.xex`
too) with their names from the XDBF (games A to H), 0.1 to 1.7 s each; both game A discs matched
`player_<A>` by digest, game B `player_<B>`, the rest the generic
player. `--start` ran game A's USA disc in `player_<A>` with a
`[games.A]` table in the settings ("settings: ... (the game's own,
A)" in its log): 1153 swaps in 120 s, 1130 frames presented, 20,713
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
`--start 0` ran game A in `player_<A>` as before and `--list` is
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

Checked (2026-11-06, the desktop interface): pictures (`--render-size
1600x900`, light and dark; `--scale 1.5`) of the table with a row selected,
the Options menu (`--menu options`), the row's menu (`--context-menu`), the
prepare dialog running with its log and failed (`--demo-progress`,
`--demo-fail`), About, an empty folder, and the settings window's Video,
Keyboard (a binding waiting for its key) and Interface tabs (`--screen
settings --tab <name>`; the settings were then a window of their own, drawn
by a second device: `Rml::ReleaseRenderManagers()` was needed so its textures
went before it did). Since 2026-11-08 they are a page of the main window:
pictures of every tab, of a dropdown open and of the slide there and back
(`--keys x,rb,b`) show no RmlUi assertion (a slider's fill of width 0 with a
shadow asserted: it has none now). A real window's run
saved its place in `[launcher]`. Left for a person: the mouse (hover, a
column's drag, a right click, a slider's drag), typing in the search and the
folder's field, the folder dialog, a real pad, and the theme followed live
when the desktop's changes.

The prepare flow, end to end (2026-11-06): Game H's disc, never
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
Game A, 274 in the eight games), and now writes its body as a lambda
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
Game A's and game B's libraries). Both binaries import only
Windows' own DLLs (KERNEL32, ADVAPI32, ntdll, ole32, OLEAUT32, SHELL32,
VERSION): the LLVM release links its C runtime statically. Apache 2.0 with
LLVM's exception, redistributable. The search (`find_toolchain`):
`--toolchain`, `XF_TOOLCHAIN`, `toolchain/bin` beside the executable (the
bundle), an installed LLVM (`%ProgramFiles%\LLVM\bin`), the PATH. The
package puts them there (`cmake/package.cmake`, `XF_PACKAGE_TOOLCHAIN`, on:
the clang and lld beside the compiler that built XenoForge, with LLVM's
license in `toolchain/LICENSE-llvm.txt`); without them a machine with no
LLVM runs games interpreted only.

**The build** (`prepare_game`): the disc's executables read (all XEX2 files
but title updates and the system update's), `default.xex`'s image digest
taken; discovery and emission in process (`recomp::emit_modules`, what
`xfrecomp emit` writes) into `<cache>/work/<digest>/src`, the
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
version's for that digest. Progress is reported per module (discovery), per
file (emission and compiling) and for the link, always from the caller's
thread; the cancel flag ends the compilers under way.

**The objects are kept** (`BuildOptions::keep_objects`, on by default,
2026-11-09): after a build the work directory keeps `obj/` and
`objects.key` (game D: 284 objects, about 185 MB) and loses the emitted
code, which the next build writes again (`keep_work` keeps that too,
`--keep`). Beside each object, `<file>.source` holds a key of the text it
was compiled from, so an object is reused whenever its source's text is
the same, however the source was written; a stopped build goes on the same
way. The work directory is the disc's, not the version's: another
XenoForge's build of the same disc (or a rebuild for the entry log)
recompiles only the files whose text changed. `objects.key` is a key of the
headers' code (`code_of`: comments, blank lines and the spaces around lines
left out, so editing a header's comments keeps the objects; no
generated-code header uses `__LINE__`), the flags and the compiler (clang's
size and time); one that differs discards every object. The interface
version is in the entry's text, so the entry alone is recompiled when only
the version moved (a recompiler change that leaves the headers' code
alone, or a comment edited). A change to the generated code's own
semantics (an inline header's code) still recompiles everything, as it
must: every function's code changes.
The CPU must have AVX2 and FMA (checked first, `cpu_supported`).
`find_cached(cache, digest)` finds `<digest>-<this version>.dll`.

**A build that never fails whole** (D2, 2026-11-07). Only a disc that
cannot be read, one without `default.xex` (or whose `default.xex` has no PE
image), a missing toolchain, an unsupported CPU, or a fault in XenoForge's
own files (a module's `table.cpp`, `modules.cpp`, the entry; or every file
rejected at once) fails `prepare_game`. Otherwise:
- a function the emitter cannot emit (it throws) becomes a one-line stub,
  `XF_FN(xf_X) { ::xf::ppc::dispatch(c, 0xXu); }`, and its table entry
  holds `nullptr`: its callers in other files still link and call the stub,
  whose dispatch finds no code and interprets the function. Another
  executable on the disc that cannot be read is left out whole (no table:
  the kernel interprets it). `recomp::EmitOptions::leave_out` and
  `keep_going`;
- a functions file clang rejects is split by function (the layout
  `recomp.h` documents for `emit_modules`) into 8 parts written beside it
  (`functions_K_0.cpp` ...), compiled again; a rejected part is split again,
  until single functions, the culprits. Bounded: 4 rounds, 256 extra
  compiles in all; past either, a rejected part's functions are all left
  out. The culprits' stubs go in `functions_K_left.cpp`, their entries in
  the module's `table.cpp` become `nullptr`, and the parts that compiled
  replace the file in the link (a file whose parts all compile, as when
  clang ran out of memory on it, is simply replaced by them);
- the left-out functions (module, address, why) and the entry log's
  addresses taken in are written to `<cache>/<digest>-<version>.build.txt`
  (`entries_taken N`, `entries N`, then `left<TAB>module<TAB>address<TAB>
  reason` lines), read by `read_build_report`, and given to the caller
  through `BuildOptions::report` (also on a cache hit). `xfbuild` prints
  them. The kernel's `try_call` takes a `nullptr` entry as no code.

The tests force both failures (`BuildOptions::fail_emit`, `fail_compile`:
a line clang rejects put in the chosen functions' bodies):
`game_build_test` builds an executable it makes, with 18 functions in one
file, and finds the rejected one in two rounds.

**What discovery missed** (D3, 2026-11-07). The kernel calls an entry hook
(`Kernel::set_entry_hook`) the first time it enters the interpreter at an
address as a function: a dispatch or a thread's start with no generated
code, and a call interpreted code makes to an address with none (not the
returns an Unwind goes on from). The generic `boot` and `player`, with a
game library loaded, point it at an `EntryLog` on
`<library's directory>/<digest>.entries.txt` (one address in hex per line,
appended and flushed only for a new address; a lock and a lookup for one
seen before; the file kept across XenoForge versions). `prepare_game` reads
the log (`BuildOptions::use_entry_log`, on by default) and gives its
addresses, with `BuildOptions::entries`, to discovery as extra entries
(`DiscoverOptions::entries`, `Source::Logged`: registered after the
exports, before the pointer scan's candidates; one inside a known function
splits it, which costs speed, not correctness). For the launcher's rebuild
offer, `new_entries(cache, digest)` (and `has_new_entries`) counts the log's
addresses past the `entries_taken` the cached library's report holds, but
the functions that build left out (their stubs enter the interpreter by
design); 0 with no library cached for this interface version. A rebuild is
`prepare_game` with `BuildOptions::force`.

```
xfbuild <game.iso | default.xex> [cache dir] [--jobs N] [--toolchain <dir>] [--keep] [--force]
        [--cancel-after S] [--quiet] [--entry <hex>]... [--no-entry-log]
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
| Game A (291 files + 2) | 3.2 s | 4.4 s | 7.9 s | 543.6 s | 15.8 s | 575 s | 180 MB |
| Game B (106 files + 2) | 2.7 s | 1.4 s | 2.2 s | 266.2 s | 6.6 s | 279 s | 62 MB |

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
| Game A, linked | 878 | 1,337 (59.0 s of CPU) |
| Game A, library | 898 | 1,347 (59.8 s of CPU) |
| Game B, linked | 1,119; 1,123 | 1,652; 1,669 |
| Game B, library | 1,132 | 1,803; 1,872 |

Game A's menus run at the game's 30 frames a second either way.
Game B over 15 s: 585 and 650 swaps linked, 627 and 640 in the library;
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
