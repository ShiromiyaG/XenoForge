# The recompiler (design, 2026-11-04)

PPC and VMX128 to C++, plan step A2, with the interpreter (A3) built from the
same pieces. Xenia (`../xenia-canary/src/xenia/cpu/ppc`, BSD) is the
reference for what each instruction does; the shape of the output is
written anew.

## One description per instruction

Every instruction is described once, and that description is what both the
generated code and the interpreter run:

- **The table** (`src/ppc/ppc-instructions.xml`, Xenia's, unchanged: 455
  instructions with their encoding, form, the fields they read and write,
  and their disassembly). `tools/ppc_gen.py` turns it into
  `src/ppc/opcodes.inc`: the opcode list, the decoder (by primary and
  extended opcode, as Xenia's `ppc-table-gen.py` builds it) and the
  disassembly patterns. `ppc.h` decodes and disassembles.
- **The semantics** (`include/xenoforge/ppc_ops.h`): one inline function per
  instruction, `ops::addx(Context &c, uint32_t code)`, translated from
  Xenia's emitters (`ppc_emit_*.cc`) and from what its backend does with
  them (saturation, the vector unit's non-Java mode, FPSCR). The fields
  come from the instruction word through Xenia's `InstrData` layout
  (`i.XO.RT`).
- **The interpreter** decodes a word and calls the same function with it.
- **The generated code** calls it with the word as a constant:

  ```cpp
  // 822D28C0  lwz r10, 8(r3)
  ops::lwz(c, 0x8143'0008);
  ```

  The compiler folds the fields away, so `c.r[10] = load32(c.r[3] + 8)` is
  what runs; the listing comment and `#line` point the host debugger at the
  PPC. Branches, calls and returns are the emitter's own (labels, C++ calls
  through the dispatch table, `return`), never an `ops::` call.

So the instruction tests check one thing that both paths run: the decoder,
the semantics, and then the emitter's control flow on its own.

## Checks

- **The decoder and the disassembler** against Xenia's own (its
  `xenia-cpu.lib` built in `../xenia-canary`, through a small oracle
  program kept outside the repository): every word of the test binaries,
  of both games' code, and random words, byte for byte.
- **The semantics**: Xenia's 566 instruction tests (`src/xenia/cpu/ppc/testing`,
  hundreds of thousands of register checks, many generated on the console),
  assembled by `tools/assemble_ppc_tests.sh` with the VMX128 binutils
  (`../rexglue-sdk/tools/binutils`) into `../external/ppc_tests`, run by
  `ppc_test` (`XF_PPC_TESTS`), skipping what Xenia's `skip.txt` skips.
  First through the interpreter, then recompiled. XenoForge's own tests in
  the same format, `tests/ppc` (the string instructions, which Xenia has
  none of), are assembled with them (the script's fourth argument), and
  their `skip.txt` is added to Xenia's (Xenia's hand-written `vrefp` tests,
  which expect exact reciprocals where the console's give its estimate).
- **The games**: both recompiled and compiled; functions found compared
  with rexglue's lists for the same executables.

## The context

`Context`: r0-r31 (64-bit), f0-f31 (double), v0-v127 (`Vec128`, words in
guest order, bytes reached through `^ 3` as Xenia does), CR as 32 bytes
(lt, gt, eq, so per field), XER's CA, OV and SO, LR, CTR, FPSCR, VSCR's
SAT, the reservation of `lwarx`/`stwcx.`, and the guest memory's base.
Memory is 4 GB of reserved host address space, the guest's addresses
added to its base; loads and stores swap bytes.

## Semantics, where it departs from Xenia or the brief

- 32-bit mode (MSR 0x9030, SF clear): results are 64-bit, but CA, CR0 and
  OV come from the low word. OE (XER[OV]) follows the architecture, which
  Xenia leaves out; CR0 copies XER[SO]; CA is set even with OE, unlike
  Xenia's addc, subfc, subfe and addme. `sraw`/`srad` give the sign to CA
  past 31/63 bits. XER is SO, OV and CA.
- Division by zero (and the most negative number by -1) gives 0, as Xenia's
  backend does.
- `dcbz` clears 128 bytes, as Xenia canary ("no short cache line" on the
  Xbox 360); game A's own `dcbz` loops step by 128.
- Reservations: `lwarx`/`ldarx` record address and value after a fence;
  `stwcx.`/`stdcx.` store with one compare-exchange against the reserved
  value and drop the reservation; CR0's SO is left alone, as Xenia.
- The string instructions follow the manual (Xenia has none of them, nor
  tests: `tests/ppc` has some). `lswx`/`stswx` move the byte count XER
  keeps in bits 57-63, which mtxer sets and mfxer reads back (the context's
  `xer_bc`); with a count of 0 nothing moves.
- Traps call `system->trap` when they fire (Xenia ignores them by default;
  the emitter sets `c.pc` before each trap instruction, the interpreter
  always does); `sc` calls `system->syscall`. The kernel's trap
  (`exceptions.cpp`): `twi 31, r0, 20` and `26` are the XDK's debug print
  (r3 the text, r4 its length; printed as `guest: ...` and the code goes on,
  as Xenia's TrapDebugPrint), `25` is ignored as Xenia does, and any other
  fired trap is the console's breakpoint (`DbgBreakPoint` is `twi 31, r0,
  22`): STATUS_BREAKPOINT raised at the instruction through the title's
  handlers, which end the title when none takes it, as the console does
  with no debugger (Xenia logs "This should be a crash!" and goes on). A
  handler that continues goes on after the trap.
- VMX: VSCR[SAT] is sticky (Xenia's backends always clear it). `vsl`/`vsr`
  shift each byte by its own count with the neighbour's bits, as the
  console does where the counts differ (Xenia uses byte 15's for all and
  skips those 876 tests; they pass here). The 22 instructions Xenia lacks
  (`vmsum*`, `vmul[eo]*`, `vsum*`, `vmhaddshs`, `vmhraddshs`, `vmladduhm`,
  `vupk[hl]px`) follow the AltiVec PEM.
- VMX floating point runs in non-Java mode: denormals in and out flush to
  a zero of their sign. **VSCR** (2026-11-08): SAT, and NJ as `mtvscr`
  sets it (`Context::vscr_java`, in the context's padding, 0 = NJ set);
  `mfvscr` and the exception contexts read both. Java mode (NJ clear) keeps
  denormal operands and results (IEEE 754) in the arithmetic, compares,
  rounding, conversions and dot products; each instruction runs its
  non-Java form inline and its Java form out of line (`XF_VFP` in
  `vmx_float.h`: a load and a branch not taken). The D3D packs and the
  estimate tables are the same in both modes (unknown on the console; an
  estimate of a denormal divides). Restoring a CONTEXT leaves NJ alone.
  Xenia switches its denormal mode on `mtvscr`'s NJ too. `vmaddfp`/`vnmsubfp` round once (`std::fma`; one
  instruction with FMA in the build). Where the console's tests contradict
  Xenia, the console wins: a NaN comes out quieted, the first in operand
  order vA, vB, vC; `vnmsubfp` does not negate a NaN; invalid operations
  give 0x7FC00000; `vmaxfp`/`vminfp` quiet signalling NaNs; `vcmpbfp`'s NaN
  sets both bits. `vrsqrtefp` uses the console's table; `vrefp` the
  console's estimate built the same way (32 segments by the top 5 fraction
  bits, a line over the next 9), its table read back from the console's
  tests: 16 segments whole, the start of one; the 15 no test reaches divide,
  as Xenia does. `vexptefp`, `vlogefp` stay exact as Xenia's (the console's
  estimates are 17 skipped tests that hold too little to rebuild them). The
  dot products and D3D packs follow Xenia's sequences bit for bit (checked
  on 2M random vectors per type), but for FLOAT16 -0 and denormals, where
  Xenia's fast path looks wrong. All of it assumes the host rounds to
  nearest.
- Floating point: FPSCR[RN] and NI are honored; the common case (round to
  nearest, no Rc, an ordinary result) costs one compare, the rest goes to a
  cold path that sets the host's rounding (`<cfenv>`) and puts it back. The
  host must stay at its default environment (round to nearest, no FTZ or
  DAZ), and the generated code must not be built with `-ffast-math` or
  `-ffp-contract=fast`; with `-mfma` (x86-64-v3) `std::fma` is one
  instruction instead of a call. Rc forms keep FPSCR's exception and status
  bits and copy them to CR1; compares set FPCC. The console wins over Xenia:
  the default NaN 0x7FF8000000000000, NaNs in A, B, C order, quieted, not
  negated by `fnm*`; fused single forms round once; `fctiw` of a NaN gives
  0xFFFFFFFF80000000; `frsqrte` uses the console's table. Xenia's `mtfsf`
  mask order, `mtfsfi`'s U field and `mcrfs` are fixed; `mtfsb0`/`mtfsb1`
  are done. A single form with a double-denormal operand does what the
  console does: `fadds`, `fsubs`, `fmuls` and the fused forms give the
  default NaN and raise nothing unless an operand is infinite or a NaN;
  `fdivs` and `fsqrts` give the double result, not rounded to single. The
  fast path checks the single forms' operands for them: the smallest
  magnitude against 2^-1022, then each only when it is that small (a zero).
  It is not free: a loop of nothing but `fmadds`, `fmuls` and `fadds` takes
  about 1.5 times as long. Left: `fres`'s estimate (its tests hold the
  console's value for 1.0 alone), and one `fctiwz.` case whose vector
  contradicts the same input's `fctiwz` and `fctiw.` (a NaN in, 0 and no
  exception out).
- For step A4: Xenia adds 0x1000 to addresses at 0xE0000000 and above (the
  4 KB offset of that physical view); `ops::host` is the one place to do it.

## Threads and the system

As the 3DS runtime's interface 6: each guest thread runs on its own stack
(a host thread when playing, a fiber in the deterministic mode); a system
call is a C++ call that comes back. The generated code never leaves partway;
yield points where loops turn back and at the priority hints hand over in
the deterministic mode ("Yield points" below). Each guest stack a thread moves to (a
fiber's) runs on a host stack of its own, so KeSetCurrentStackPointers is a
call that comes back when its stack is switched back to ("Stack switches").

### Yield points (2026-11-07)

Where guest code that waits for another thread with no kernel call (the
XDK's spins on a flag) can be handed over, exactly, in the deterministic
mode (docs/runtime.md, "The deterministic mode"):

- **Where**: every taken branch backward (`b`/`bc` without LK to its own
  address or before it: a loop's back edge, emitted as
  `{ XF_YIELD(); goto L_...; }`, and the same for a backward tail branch to
  another function), and every priority hint, `or rx,rx,rx` with Rc 0
  (`or r1,r1,r1`, `or 31,31,31`; in `orx`'s semantics in ppc_ops, where the
  constant word folds the test away). The interpreter counts the same
  points, so a function counts alike run either way.
- **Cost when playing**: `Context::yield_counting` is false, so a yield
  point is a load and a branch not taken (no store, no call).
- **Counting** (set by `Kernel::attach` in the deterministic mode): each
  point takes its weight off `Context::yield_budget` (a back edge 1, a hint
  8; units of 1/32 of the clock's 100 ns, `Deterministic::yield_units`, so
  about ten Xenon cycles); below 0, `System::yield` moves the clock by the
  whole ticks spent, refills the budget and may hand over at the slice's
  end, as a kernel call does. The hook is a cold out-of-line function in
  the header, so a game library needs no new runtime call.
- The lockstep check's interpreter never counts (its copy of the context has
  `yield_counting` false), so it never hands over in the middle of a check.
- Not yet: yield points at function starts (the plan's other half); none of
  the measured spins needed them, as every spin has a back edge.

## Discovery

Where functions start, where they end, and what each indirect branch can
reach. Learned from rexglue's fork (`../rexglue-sdk/src/codegen`, BSD), which
runs both games today, and from XenonAnalyse (`../XenonRecomp`, MIT):

- **Starts**, the more certain winning: the port's own entries; the import
  thunks; the register save and restore helpers (`__savegprlr_14`... found
  by their first words, one entry per register as rexglue does); `.pdata`
  (8-byte entries: begin, then prolog length, length in instructions,
  32-bit and exception flags; with the exception flag, the handler and its
  scope or FuncInfo table give the funclets); the entry point; a DLL's
  exports (the XEX export table the security info points at, and the PE
  export directory, as GetProcAddress reads them); the addresses earlier
  runs entered the interpreter at (`DiscoverOptions::entries`, source
  `logged`: the runtime's entry log, D3, docs/runtime.md "The game
  library"); the data pointer scan's
  (below); `bl` targets; last, gaps no function covers. On game A
  `.pdata` alone gives 52,360 of the 73,587 functions.
- **The data pointer scan** (`scan_pointers`, `DiscoverOptions`; translated
  from the fork's `codegen/pointer_scan.cpp`, its `[analysis]
  scan_data_pointers`): every word of the sections that are not code
  (vtables, callback tables, fields), and every `lis rX`/`addi rY, rX` pair
  in code, whose value looks like a function's first instruction (`mflr
  r12`, `stwu r1`, or after `blr`/`bctr`/`b`/0 or a `.pdata` function's
  end) is a candidate; engines
  without RTTI (UE3) reach most of their methods only so. A baseline run
  says which the image explains already (a start, or a branch target inside
  a function); the rest become starts and discovery runs again. A candidate
  that splits a function leaves the function's own branches across the
  split pointing at no start (`Discovery::unresolved`: what the generated
  code would hand to the interpreter, where rexglue's builders would trap);
  the culprit is dropped (the first candidate after the branch going
  forward, the last before it going backward, else every one in the span),
  and the passes go on until nothing changes; branch targets no function
  holds are added the same way, and so are those inside a function whose
  end was only walked to (no `.pdata` size) when the branch reaches no start
  without the scan either: the XDK's context-save routine after a call that
  never returns (the walk runs on past the `bl`; game D and game F),
  a tail two functions share (game F); the holder then runs on into it as a
  tail call. Branches unresolved without the scan say
  nothing about the candidates. Not in rexglue: an address inside a jump
  table discovery read is no start (the `lis`/`addi` that loads a table's
  address after its `bctr` looks like one), nor one inside the prologue
  `.pdata` gives (a `stwu r1` there); the passes rerun the whole discovery
  (under a second each), so there is no cache of dropped candidates. Inside
  a `.pdata` function past its prologue pointers do find entries: the
  funclets an exception scope table names. `xfrecomp --no-pointers` leaves
  the scan out.
- **Gaps**: code no function covers gets a function after each end (`blr`,
  `bctr`, `b`, or where `.pdata` says a function ends, which may be a call
  that never returns) and after padding, as rexglue splits its gaps; each is
  walked as soon as it is found, so a run of leaf methods is cut in one
  pass (rounds of one gap per run left all but the first 8 of game A
  Kart's longer runs to the interpreter). A `blr` alone right after an end
  is the compiler's dead return after a tail call, a function only when a
  `bl`, a pointer or a branch reaches it (an empty virtual method comes back
  through its vtable); the 8 bytes before a function with exception data
  are its handler and the handler's data. Coverage counts a sized function
  that holds a nested start, so the rest of its range is no gap.
- **Ends**: `.pdata`'s length where there is one; else the blocks reached,
  never past the next known start. A function that runs on into the next one
  ends in a tail call to it (rexglue copies the code instead, and functions
  without a size there swallow their neighbours).
- **Jump tables**: the four forms the XDK's compiler writes (XenonAnalyse's
  patterns, rexglue's detectJumpTable): `lwzx` of absolute words; `lbzx` of
  byte offsets added to a base, or shifted by an `rlwinm` first; `lhzx` of
  halfword offsets. The bound is the `cmplwi` on any register the index
  passed through (`clrlwi r9, r11, 27; cmplwi r9, 17`, `mr r31, r4;
  cmplwi r4, 18`), looked for back from the load, or on a register loaded
  from the same stack slot in unoptimized code (`lwz r8, 0x70(r1); cmplwi
  r8, 9; ...; lwz r7, 0x70(r1)`); a `clrlwi` only caps it (a 5-bit mask and
  `cmplwi 12` is 13 entries, not 32). A compare guessed from elsewhere in the
  function that the table does not hold code for is dropped for an
  unbounded read.
- **Stack switches**: a function that loads r1 from memory r1 does not
  address (`ld r1, 0x90(r7)`: the CRT's longjmp; `ld r1, 0x20(r4); blr`:
  the C++ exception machinery's jump to a catch's continuation) or branches
  to KeSetCurrentStackPointers' thunk (`b`, `bc` or `bl`: the XDK's
  SwitchToFiber, in game F) may return to another frame than its caller's
  (`Function::switches_stack`); one that keeps r1 in memory r1 does not
  address and returns (setjmp, or a tail branch to one) saves the stack
  (`saves_stack`); the return sites of the calls to those, the catch
  continuations of the functions with exception data, and the `__except`
  blocks of those whose handler is `__C_specific_handler` (its import
  thunk, xboxkrnl 0x1A5; each scope record's target in the scope table the
  handler data points at) are resume sites (`resumes`). How the generated
  code follows them: "Stack switches" below; how the exception dispatch
  lands there: docs/runtime.md, "Structured exceptions".
- **Everything else** that is indirect (`bctr`, `bctrl`, `blrl`, a branch
  to an address no function holds) goes through the dispatch table, and an
  address with no generated code runs in the interpreter: a wrong guess
  costs speed, never correctness. `boot` counts those entries, which is how
  discovery is improved, game by game, without traps (rexglue leaves 26
  unresolved branches and 26 unresolved table entries on game A, and
  needed 633 starts added by hand; here no direct branch or table case of
  either game reaches an address without a start).
- **Checked** against rexglue's lists (`xfrecomp analyze --compare`): El
  game A's project (74,112 entries: 294 imports, 73,818 bodies, 633 of
  them added by hand) and lists the fork wrote with its own scan for both
  games (`rexglue init` and `codegen` with `[entrypoint.analysis]
  scan_data_pointers = true`, outside the repository; game B's fails
  validation without it). Counts on 2026-11-05:

  | | game A | game B |
  |---|---|---|
  | functions (before the scan and the gap fix) | 73,182 | 22,493 |
  | functions now | 73,587 | 21,896 |
  | from the pointer scan (kept / dropped as splits) | 89 / 4 | 183 / 5 |
  | branch targets added | 6 | 0 |
  | unresolved branches (without the scan, with it) | 6, 0 | 1, 0 |
  | jump tables (before, now; none lost) | 566, 595 | 454, 473 |
  | rexglue's own scan: functions | 74,131 | 22,239 |
  | in both (before, now) | 72,743, 73,469 | 22,022, 21,888 |
  | only here (before, now) | 439, 118 | 471, 8 |
  | only there (before, now) | 1,388, 662 | 217, 351 |

  What is only in rexglue's list now: dead `blr`s nothing reaches (365
  and 214); the data of jump tables inline after their `bctr` (189 and
  135), which rexglue's scan takes as starts; code after a `blr` that the
  function's own branch reaches (105 on game A, `bne` over `li r3;
  blr`), labels here; a `stwu r1` inside a prologue (1); 2 on each game
  after a `b` inside a function the scan found, labels here. Only here:
  leaf methods and adjustor thunks right after another function's `bctr`
  or `b` that rexglue merges into it (115 and 8), and 3 functions without
  `.pdata` after a `blr` (game A). Before, game A's project
  list had 1,369 starts only there: the leaf method runs the gap filler's
  rounds left uncovered, besides its hand-added labels and table data.
  In 20 s of `boot` linked with the generated code, game A entered
  the interpreter 262,094 times at 802 addresses before (those leaf runs:
  `lwz r11, 0xA8(r3); lwz r10, 0x624(r11); rlwinm r3, ...; blr`, reached
  through vtables) and game B 10 times at 9 (small functions the same
  rounds left uncovered); now neither does. Game A's first swap comes at 1.0 s instead of 1.5 s, 593
  swaps in the 20 s instead of 509 (30 a second, its cap), and it gets as
  far as registering its XAudio client; game B 1,154 instead of 1,043 (60
  a second).

## Modules and the game library

A game is every executable on its disc, not one: Game D loads
`XMediaFacade_default.xex`, game F four Waves DLLs, game C boots a launcher
that loads `Splash.xex` and then `Game.xex`, both at 0x82A00000. So the
generated code is per module:

- **`xfrecomp emit <iso> <dir>`** discovers and emits each XEX2 file on
  the disc (but the system update's and title updates; `--xex` picks), each
  into a directory of its own named after its path (`default_xex`,
  `waves_L360_dll`), its functions in a namespace of their own
  (`xfm_waves_L360_dll::xf_894206E0`), so modules at the same addresses
  link into one library. Each DLL's 16-byte import records are thunks, as
  the executable's are; a call through one stays a dispatch call, the
  kernel taking it to the export (docs/runtime.md, "Code").
- **The table** (`ppc_module.h`, `table.cpp`): the module's path, base,
  image size and the digest of its image as the loader unpacks it
  (`image_digest`: 64 bits, four lanes of multiply-folds, as fast as memory
  reads; SHA-1 would have cost 80 ms of game A's start), and its
  functions by address. `modules.cpp` lists every module's table
  (`xf_modules`); the kernel gives a table its module's range when an image
  with that digest loads, so another version of a module or a title update
  runs interpreted, never another image's code.
- **Files**: one per 64 KB of the image (about 16,000 instructions), each
  declaring only what it calls, written only when its text changes. A
  discovery fix then rebuilds the files it touched (game D's and
  game F's below: 2 and 4 files of 224 and 149), where one header declaring
  every function made any change rebuild the whole game, and every file
  parsed 1.5 MB of declarations (about 4 minutes per million instructions
  were measured with that layout under other builds' load).
- **Optimization**: `XF_GAME_OPT` (default `-O2`) and `XF_GAME_OPT_<game>`.
  Measured here at -O2 (12 threads): Game A's 4.7M instructions in
  269 s; the other six games' 21.7M in 1,465 s, part of it beside another
  build; game B's 1.7M in 143 s, and 100 s at -O1. Linking a `boot_<game>`
  takes about a minute, which dominates a rebuild after a discovery fix (60
  files of the eight games and eight links: 534 s). -O2 stays the default:
  every game builds in under half an hour.
- **Two ways to link it** (2026-11-06): CMake's `game_<name>.lib` linked
  into `boot_<game>` and `player_<game>` (ports), or a game library: the
  recompile step (`xfbuild`, the launcher; `recomp::emit_modules` in
  process, then clang and lld) builds the same emission into a DLL the
  generic `boot` and `player` load (docs/runtime.md, "The game library").
  The same headers compile both; in the library they are freestanding
  (`XF_GAME_LIBRARY`) and the runtime's calls go through its table
  (`ppc_runtime.h`).

The eight games (2026-11-05, with the stack switches generated, "Stack
switches" below; functions with generated code, all but the imports;
instructions are their sizes / 4; switches: functions that switch the
stack, their returns checked; resumes: resume sites, in how many
functions; unresolved: direct branches and table cases to no start, with
the scan):

| game | module | functions | instructions | switches | resumes | unresolved |
|---|---|---|---|---|---|---|
| Game A | default.xex | 73,293 | 4,720,516 | 1 | 9 in 9 | 0 |
| Game B | default.xex | 21,720 | 1,712,043 | 2 | 7 in 7 | 0 |
| Game C | Default.xex | 292 | 12,518 | 0 | 0 | 0 |
| | Splash.xex | 6,834 | 679,357 | 1 | 7 in 7 | 0 |
| | Game.xex | 54,018 | 2,773,730 | 2 | 2 in 2 | 0 |
| Game D | default.xex | 76,047 | 3,655,459 | 3 | 11 in 11 | 0 |
| | XMediaFacade_default.xex | 3,163 | 525,032 | 0 | 0 | 0 |
| | SpeechFacade_default.xex | 6,649 | 374,049 | 1 | 1 in 1 | 0 |
| Game E | default.xex | 56,302 | 3,465,904 | 2 | 2 in 2 | 0 |
| Game F | default.xex | 37,904 | 2,424,688 | 3 | 41 in 25 | 0 |
| | waveShell-Xbox.dll | 2,653 | 104,041 | 1 | 49 in 43 | 0 |
| | wavesLibDLL.dll | 3,373 | 142,016 | 1 | 64 in 56 | 0 |
| | waves\L360.dll | 1,352 | 47,348 | 1 | 2 in 2 | 0 |
| | waves\Q10.dll | 1,147 | 38,123 | 1 | 2 in 2 | 0 |
| Game G | default.xex | 69,231 | 3,418,826 | 0 | 1 in 1 | 0 |
| Game H | default.xex | 65,842 | 4,046,585 | 2 | 156 in 106 | 0 |

Two discovery fixes came from running them: the branch targets inside a
walked function (above, "The data pointer scan"), and a .pdata function's
end as an end for the gap filler and the pointer scan whatever its last
instruction: a function that ends calling one that never returns (`bl
_CxxThrowException`) is followed by another, game H's adjustor thunks
(`addi r3, r3, 0xC; b ...`) reached only through vtables, which the gap
filler skipped for want of a `blr` or `b` before them (game H entered the
interpreter 135,471 times at 4 of them in 30 s; 12 such starts on game H, 1
each on game D, game F and two of its DLLs).

Game D had 8, one byte-offset table's cases (0x829EB474: `rlwinm
r10, r9, 0, 26, 31`, so 64 entries by the mask, no compare) past the end of
its function, into the middle of the next: the table is shorter than the
mask allows (9 cases, MSVC's `default: __assume(0)` leaving the bound out)
and the bytes after it are other data. A table the mask alone bounds now
ends at its first entry outside its function (here after 24, the 15 past
the 9 cases being other data that lands inside the function); an index past
it goes to the switch's `default`, a dispatch to CTR, which is what the
hardware would do if the index ever got there. The function list is the
same; no unresolved branch is left.

## Stack switches

(2026-11-05, plan "Next", step 5.) Three things in the games move r1 to
another stack and return into another frame than their caller's:

- **longjmp**, the CRT's: the registers, r1 and LR back from the `jmp_buf`
  setjmp filled, r3 the value (game C's Splash.xex, 8 times in 30 s; game F's
  streaming decompressor, to bail out of bad input);
- **the jump to a catch's continuation** in the C++ exception machinery: a
  CONTEXT restored and a `blr` to the address the catch block returned;
- **SwitchToFiber**, the XDK's: the registers saved into the current fiber,
  the next one's loaded (LR and r1 among them), then a tail branch to
  KeSetCurrentStackPointers, which moves r1 to the next fiber's stack
  (game F's decompressor runs on a fiber of its own and switches back to its
  caller whenever its input runs out: 1,202 switches in 30 s of `boot`, 34
  fibers made and deleted).

A C++ return cannot go into the middle of another function's frame. Until
now those functions were left to the interpreter, and their returns were
thrown up to the dispatch whose frames held the restored r1, which
interpreted the guest's frames from there up to its own return (game F
entered the interpreter 1,803 times in 30 s, game C 16; after its first
switch, a fiber of game F's ran interpreted for good). Now the generated
code follows them, and nothing between is interpreted:

- **Each guest stack runs on a host stack of its own** (docs/runtime.md,
  "Stacks"): KeSetCurrentStackPointers suspends the thread's host fiber in
  the call and resumes the one the new guest stack's frames are on (made the
  first time, running from LR). The game's own SwitchToFiber runs generated
  on both sides: its call to the kernel returns on the other stack, into
  the generated frames that were waiting there, which return as the guest's
  do. A fiber switch is a host fiber switch; nothing is unwound or
  interpreted.
- **Functions that switch the stack have generated code**, called directly
  like any other. Each keeps its entry's LR and r1 and checks every return
  against them (`XF_CHECKED_RETURN`, in `ppc_generated.h`): a return with
  both is an ordinary one; any other throws `ppc::Unwind{LR}`
  (`ppc::unwind`, a C++ exception) to the frame it goes to. Tail calls in
  them are calls and a checked return.
- **A branch to KeSetCurrentStackPointers' thunk is `switch_stack(c, thunk,
  resume)`**: the dispatch to the export, saying where this frame's code
  goes on when its stack is switched back to: the address after a `bl`, the
  function's own entry LR for a tail branch (SwitchToFiber's `b` to the
  thunk is its return to its caller). The kernel resumes a suspended stack
  as it left only when the switch back brings the r1 it left with and that
  address in LR; otherwise its host frames get an Unwind as they resume.
- **Resume sites.** A longjmp comes back after the call to its setjmp; a
  catch continuation is an address inside the function the catch belongs
  to. Discovery gives a function a resume site after each call to a
  function that saves the stack (keeps r1 in memory r1 does not address and
  returns normally: setjmp, or a function that tail-branches to one; not
  SwitchToFiber, which saves it too but switches, so its callers go on
  through the switch back), and, for a function with exception data
  (`.pdata`'s flag), at each address inside it, past its start, that a
  lis/addi pair makes anywhere in the code: a catch block is a funclet with
  its own `.pdata` that ends `lis r3, cont@ha; addi r3, r3, cont@l; blr`
  (checked on game F: 0x821D6038's two catch funclets return 0x821D60AC and
  0x821D60B0; the pointer scan took the first for a start, which it stays,
  and the label is emitted in the parent too). A function with resume sites
  has its body run by the runtime (`resumable`, ppc_runtime.h), which runs
  it again from a resume site when an Unwind to one of them comes back with
  r1 inside its frame (game F's setjmp caller, a decompressor entry):

  ```cpp
  XF_FN(xf_827D2B80) {
      static constexpr uint32_t xf_sites[] = {0x827D2BA4u};
      ::xf::ppc::resumable(c, XF_RESUMABLE_BODY {
          switch (xf_at) {
          case 0x827D2BA4: goto L_827D2BA4;
          }
          ...
          c.lr = 0x827D2BA4; xf_829B20A0(c); // setjmp
      L_827D2BA4:
          ...
          return;
      }, uint32_t(c.lr), uint32_t(c.r[1]), xf_sites, 1);
  }
  ```

  `resumable` (src/ppc_runtime.cpp) holds the try block the emitter wrote
  into the function until 2026-11-06: it runs the body (a lambda taking the
  entry's LR and r1 as `xf_lr` and `xf_sp`, and `xf_at`), and on an Unwind
  with r1 below the entry's and its target among the sites runs it again
  from there; any other goes on up. So the generated code never catches:
  every Unwind is thrown and caught by the runtime, and a game library
  carries no C++ runtime (docs/runtime.md, "The game library"). The
  generated code keeps nothing outside the context but `xf_sp`, `xf_lr`
  and `xf_at`, so a frame goes on at a label exactly as the guest's does.
  There are no tail calls in the body (`musttail` cannot leave a lambda for
  a function of another type).
- **Who takes an Unwind**: the innermost host frame entered with r1 above
  the restored one, whose function holds the guest's frame at that r1 (the
  frames inside it were entered at or below it). A generated function with
  the target among its resume sites goes on there, in its own frame, the
  frames above it untouched; one without passes it on; a dispatch (entered
  with its stack top above r1) interprets on from the target as before, a
  function's start on the way running its generated code. A dispatch whose
  stack top is exactly the restored r1 (an indirect call the target frame
  made) passes it on to its caller when the target is a resume site of the
  generated code (`GeneratedModule::resumes`, `Kernel::resumable`), and
  interprets otherwise, as before.
- **Cost**: nothing where nothing switches. The checked returns are in the
  switching functions alone (20 in the 16 modules), a call through
  `resumable` for each call of a function with resume sites (274
  functions, 354 sites, most of them catch continuations in game H and
  game F's Waves DLLs: the table above), and an Unwind is a C++ exception
  (3.7 microseconds through 2 frames, 6.2 through 17, the same in a game
  library: game_library_test), only for longjmps and catches.

Measured (2026-11-05, 30 s of `boot_<game>`, whose report now says what
took each switch): Game F enters the interpreter 0 times (1,803 before: 1,202
at SwitchToFiber, 567 at the decompressor's return from it, 34 at the fiber
start routine); its 1,202 switches are host fiber switches, its 34 fibers
get 34 host fibers, dropped with their stacks (MmDeleteKernelStack), and
nothing is unwound. Game C enters it 0 times (16 before: 8 at longjmp, 8 at the
setjmp caller's return site); its 8 longjmps are taken by the setjmp
caller's generated frame at its resume site. The other six games enter it 0
times, as before, and none of them switches stacks or unwinds in its first
30 s. Swaps a second are as before, at the games' caps when the machine is
not loaded (game F 58 to 60, game C 29.3); the switches and longjmps were too
few for their interpretation to show in the rate.

Left to the interpreter, correct and slower (none of it seen in the eight
games): a return to an address discovery did not make a resume site (a
setjmp called indirectly; a continuation not made by lis/addi), which the
dispatch above the frame interprets from up to its own return, as before; a
stack first entered at an address no function starts at, or a fiber that
moved to another thread, whose outer frames run interpreted there
(docs/runtime.md, "Stacks"); a longjmp that is itself interpreted (a module
without generated code) and lands in the frame of the function that called
the dispatch running it, which interprets that function on.

How the others do it: Xenia's JIT can enter any address, so its
KeSetCurrentStackPointers drops the thread's host frames and re-enters at
LR (`XThread::Reenter`), and its x64 backend keeps a stack of (host rsp,
guest r1, return address) entries so a return after a longjmp resets the
host stack to the guest's (`enable_host_guest_stack_synchronization`); a
fiber's code is entered anew at each switch. rexglue's fork (and
XenonRecomp before it) replaces the XAPI's fiber functions, hooked by
address (`REX_HOOK(rexcrt_SwitchToFiber)` and CreateFiber,
ConvertThreadToFiber: host fibers), and setjmp and longjmp at addresses the
port's configuration names (host setjmp and longjmp in the generated code).
Here nothing is named by hand: the game's own XAPI code runs generated, the
kernel export it ends in does the host side, and discovery finds setjmp's
callers and the catch continuations.

## The lockstep check

The interpreter checks the generated code while a game runs
(`xenoforge/lockstep.h`, `src/kernel/lockstep.cpp`): `boot --lockstep
82001234,... --lockstep-sample N`, or `XF_LOCKSTEP` and `XF_LOCKSTEP_SAMPLE`
for the player. A chosen function, or one call in N to any generated
function, runs twice from the same registers and memory:

1. **the interpreter first**, on a copy of the context and a private view of
   the guest memory (`Memory::PrivateView`: the section mapped again copy on
   write, in placeholders on Windows and over a reserved range on POSIX), so
   nothing it stores is seen by the game or another thread. Every call
   inside is interpreted too: the check is of the call's whole tree. Its
   observer (`interpret_observed`) records each store and load
   (`store_extent`, `load_extent`) with the bytes it read, and which
   instruction last wrote each register;
2. **then the generated code, for real**, as it always runs.

The comparison is at the call's return or at the first import, whichever
comes first. An import ends a *segment*: it runs once, for real, from the
generated code's state, and the next segment starts from what it left (the
interpreter again on a fresh view, then the generated code on). So a call
is checked whole while the kernel sees each import once, and nothing is run
twice for real. mftb's reads in a segment are the interpreter's, replayed to
the generated code. Compared: every register (r, f and v bit for bit, CR,
LR, CTR, XER, FPSCR, VSCR, MSR, the reservation) and every byte the
interpreter stored, against the guest memory as the generated code left it.
A difference is printed with the instruction that wrote the interpreter's
value:

    lockstep: mismatch 1 in the call to 0x3000D040: the segment from 0x3000D050 (after
    xboxkrnl!KeQueryPerformanceFrequency) to the call's return, 10 instructions interpreted
      r3     generated 0x0000000002FAF087, interpreter 0x0000000002FAF086 (the interpreter's
             from 0x3000D000: addi r3, r3, 0x1)
      [3000F003] 1 byte: generated 87, interpreter 86 (the interpreter's from 0x3000D004: stw r3, 0x0(r4))

Choices and limits:

- **Where a check starts**: a call the kernel makes to generated code
  (`Kernel::try_call`: a dispatch, an indirect call, a thread's start, a call
  interpreted code makes). Generated functions call each other directly, so a
  chosen function only ever called that way is checked inside a caller's
  tree (choose a caller, or sample).
- **Other writers**: another thread, a device or the kernel's clock in
  memory may change what the call reads between the two runs. A difference
  after a byte the interpreter read (and had not stored) changed, by the time
  the generated code started or by the comparison, or in a byte an atomic
  update (`stwcx.`, `stdcx.`) left, is counted apart ("different after
  memory they read changed"), not as a mismatch. The deterministic mode will
  remove these.
- **Given up, counted**: an MMIO access, a trap, a stack switch, an unwind,
  the step limit (4 million instructions a segment), what the interpreter
  cannot run. A store the generated code makes where the interpreter stored
  nothing is not seen; the physical windows do not alias inside a private
  view once it writes them.
- **Cost**: four private views shared by the threads (each holds commit
  charge for 4.5 GB on Windows), taken only while a segment's interpreter
  runs. Game A, 30 s at one call in 100: 30 swaps a second fall to 29.

Game A (2026-11-07, one call in 100, 30 s): 200,455 calls, 283,219
segments, 0 mismatches, 20 differences after memory read changed (the
kernel's time stamp bundle, audio positions), 130 given up at MMIO; one in 5,
40 s: 396,431 segments, 0 mismatches.

## The differential fuzzer

Random instruction sequences through three runners, every field of the
state after compared (2026-11-08; `xenoforge/fuzz.h`, `fuzz_format.h`,
`tools/xffuzz.cpp`):

- **The cases**: for each of the 442 instructions the generator makes (all
  456 but branches, `sc`, the traps, `mftb` and the MSR writes, which the
  emitter's and kernel's own tests cover), N cases whose focus it is: alone
  (60%) or followed by one to three random others, now and then with a
  forward `bc` over the next one (reading CR or CTR, decrementing CTR). The
  words are random in every field that still decodes to the instruction;
  loads and stores are made r1-relative (r1 points into a 2 KB window of
  random words; r2 is a small index) and nothing writes r1 or r2. Operands
  come from edge pools: GPRs (0, -1, the 32- and 64-bit extremes, carries'
  edges, shift counts), doubles (signed zeros, infinities, quiet and
  signalling NaNs with payloads, double and single denormals, the single
  range's edges, rounding ties to single, the integer conversions' edges),
  floats and words for the vectors (the same, plus saturation's edges and
  the D3D packs' 3.0 + x), random FPSCR status, exception and enable bits,
  XER, CR, CTR and VSCR[SAT]. The rounding mode starts at nearest: Xenia
  applies FPSCR[RN] only when mtfsf/mtfsfi/mtfsb set it, so the sequences
  set it themselves.
- **The runners**: the interpreter (`xffuzz run`); the generated code
  (`xffuzz emit` recompiles the cases, built at `-O2 -mfma -mavx2` as the
  games are; `ppc_fuzz_test`, in ctest, runs a fixed seed's 5,308 cases
  both ways); Xenia's x64 JIT (`tools/xenia_oracle/ppc_exec_oracle.cc`,
  its test runner's setup: `build.bat exec`).
- **The comparison** (`xffuzz compare <cases> <a> <b> [--singles] [--known]
  [--ignore fields] [--only ops] [--examples N]`): the cases by their focus
  instruction, the fields that differ counted, examples with the operands
  the instructions name. `--known` takes out of Xenia's state the
  departures this document makes on purpose (the list above: OE and
  CR's SO, CA in the OE forms, SAT, mfxer's and mcrxr's OV and SO,
  sraw/srad's CA, the FPSCR without Rc, and the instructions Xenia lacks),
  so what is left is what nothing here explains yet.

Run 2026-11-08 (seed 2, 60 cases per instruction, 26,540 cases):

- interpreter and generated code: 0 differences (and 0 in ctest's 5,303);
- the interpreter and Xenia: 9,376 of 26,376 differ before `--known`;
  with it, and singles only, 797 of 14,886 in 53 instructions, every one
  explained by the departures above, which follow the console's tests or
  the manuals:
  - the FP Rc forms' CR1 and FPSCR (ours: the exception and status bits the
    console-generated tests hold; Xenia clears FEX and VX, keeps few bits);
  - FP values: the positive default NaN (Xenia's is x86's, negative), NaNs
    not negated by `fnm*`, `fctiw*` of a NaN, single forms with a
    double-denormal operand, one rounding in the fused single forms and
    in `fsubs` and the like with operands that are not singles (Xenia
    rounds twice: a tie to single rounded to even where the exact result
    is below it), `frsp` quieting a signalling NaN, `fres`'s estimate
    (neither side has the console's);
  - VMX floating point in non-Java mode: denormal operands and results
    flushed (Xenia's x64 path takes some denormal operands as they are,
    and rounds results up to the smallest normal where the exact result
    is denormal), NaNs quieted in `vmaxfp`/`vminfp`, `vnmsubfp` not
    negating a NaN, `vcmpbfp` on a NaN, the console's `vrefp` estimate,
    invalid results 0x7FC00000 (`vlogefp` of a negative number);
  - `mtvscr`'s NJ bit, found here: ignored then, modeled since (VSCR,
    above; NJ now agrees in all 59 `mtvscr` cases), and its SAT (Xenia's
    `mtvscr` sets only its copy of VSCR).
- Xenia ran 1 case into a fault (an `addze` sequence); the interpreter
  reports 30 `vpkd3d128`/`vupkd3d128` cases with a pack type the console's
  D3D never uses as unimplemented, where Xenia compiles them as nothing.

Running it again: `xffuzz gen cases.xfz --seed S --per-op N`, `xffuzz run
cases.xfz ours.xfs`, `ppc_exec_oracle cases.xfz xenia.xfs`, then `xffuzz
compare cases.xfz ours.xfs xenia.xfs --names xf,xenia --known --ignore
fpscr --singles`. A case file holds 4.7 KB a case (the vector registers
and the memory window): 60 per instruction is 125 MB, outside the
repository (`../external/fuzz`).

## Where the time goes

`boot --profile N [--profile-from S]` (`tools/sampler.cpp`) samples every
thread of the process about once a millisecond, weighting each sample by
the cycles the thread spent since the last one (so waiting threads count
nothing), and prints the swaps a second over the span, the CPU time a swap
(all threads), the shares by part of the program and the N most sampled
functions with their hottest offsets (with their source lines where the
PDB has them; inlined and folded code can be named after a neighbour, and
`llvm-symbolizer --inlines` on the address gives the inline chain). The
generated functions' names come from the PDB's public symbols (their code
is built without debug information); a game library has none, so its
addresses are named from the modules' function tables (the nearest
function at or below, `xf_<guest address>`; `sampler::name_generated`).
`--profile-thread <name>` samples the one guest thread so named (the
kernel's own, as "GPU commands", or the guest's SetThreadName; `0x...`, the
one starting at that address, for threads without a name) with its stack
walked at each sample (x64 unwind data) and adds each function's inclusive
share. The thread report gives
each guest thread's host CPU time and host thread id (host threads are
named "xf guest <id> <name>"), for matching any outside tool's samples.
Windows only for now.

What it showed (2026-11-08, `--render`, 20 to 60 s):
- game A, at 30 swaps a second (its cap): 52 ms of CPU a swap, of which
  the generated code about 23% (much of it the engine's spin waits:
  `or r31,r31,r31` priority hints in a `bdnz` loop polling a flag), the
  host's waits and yields about 60% (a guest thread polling
  `KeDelayExecutionThread(0)`: `SwitchToThread`), the kernel's clock
  (`QueryPerformanceCounter` behind `mftb` and the scheduler's `now`) 5%,
  a `std::map<std::string>` count per import call and its allocations 4%;
- game B, at its cap: 25 ms a swap, the generated code over 60%, half of
  it the same spin wait (`__savegprlr`/`__restgprlr` 7-10%).
Since then (docs/runtime.md, "Polling threads nap" and "The host clock"):
the zero-delay poll naps, which took game A to 27 ms of CPU a swap at 30
swaps a second; the clock's read was the counter read itself (QPC is the
TSC with a fence here), which a spin loop only repeats faster.
So swaps a second do not move with the code's speed in either, and CPU
time a swap mostly measures the spinning. The measure for the generated
code's speed is the deterministic mode, which runs every thread on one
host thread as fast as it can (`--deterministic --until-swap N`): the
process's CPU time to the same swap with the same guest memory hash.

**Guest memory apart from the context** (`ppc_ops.h`, `host`): the
compiler did not know a guest store cannot write the `Context`, so after
every store it reloaded the base and every register it held. Telling it
(`__builtin_assume_separate_storage`) lets registers stay in host
registers across stores. Game B, deterministic to swap 3,000, CPU time,
three runs each interleaved (another agent's clang and Xenia running,
59-79% load): before 18.80, 19.67, 18.97 s (median 18.97); after 16.66,
16.44, 18.97 s (median 16.66): 12% less. The same guest memory hash.

**The floating point slow paths take no context** (`ppc_ops/fpu.h`,
2026-11-09). An arithmetic instruction leaves its fast path for
`fp_arith`, a conversion to integer for `fp_to_int`, an unordered compare
for `fp_compare_unordered`. Each took the `Context`, so to the compiler
each call (one per instruction, though it rarely runs) could write any
register in it and any memory: every register the code held was reloaded
after it, and clang's GVN looked for what was stored where through every
one of those calls. Without Rc they only read the FPSCR, so they now take
it by value and return their result (`fp_arith_value`,
`fp_to_int_value`, `[[gnu::const]]`); the Rc forms and the unordered
compare write the FPSCR and CR1 from what the call returns, in the
generated code. Game D's longest function (27,500 instructions, almost
all `lfs`, `stfs` and single-precision arithmetic: about 14 minutes of a
15-minute build, the other 283 files done in 4) went from 14 minutes of
clang to 261 s, GVN from most of it to 78 s.

**Very long functions in parts** (`recomp_emit.cpp`, `XF_PART`): clang's
optimizer still took time growing faster than the function's length (GVN,
the SLP vectorizer, DSE, the code generator: 9 ms an instruction there
against about 0.5 in ordinary code). In a function of more than 4,096
instructions, each stretch of straight-line code (no branch, no label
inside, no hook) of 256 instructions or more goes, 1,024 at a time, into
a part: a static member function, never inlined, of a class local to the
function (`struct xf_parts`, so the file splitting of a rejected file
keeps it with its function), which the body calls where the stretch was;
a label at the stretch's start stays before the call. Game D's longest
in 1,024-instruction parts: 38 s (2,048: 48 s; 512: 33 s). A part costs a
call and the registers it holds in the context read again, once every
thousand instructions. Compiler memory barriers every 2,048 instructions
in the one function instead changed nothing (255 s), nor did GVN's own
limits (`-memdep-block-number-limit`, `-enable-load-pre=false`); the
MemorySSA GVN took twice as long. Game D has 11 functions over 4,096
instructions of its 85,859. The 3DS runtime, whose generated C keeps the
guest's registers in locals synced with the context around calls, does
not meet this: its longest function in a game we built (21,742 lines of C)
compiles in 12 s, GVN 0.5 s.

**Registers in locals, weighed and not done** (2026-11-09). With the long
functions in parts, clang's time on an ordinary file has no one cost that
locals would remove (`-ftime-report`, game A, two files of 34,000 lines:
4.5 and 7.5 s): instruction selection 20-23%, InstCombine 10%, the SLP
vectorizer 6-10%, the always-inliner 6-8%, GVN 6-9%, EarlyCSE 3-5%, DSE
3-4%, the front end 6-21%. The passes that follow the context through
memory (GVN, EarlyCSE, DSE, LICM) are 15-23% together, and locals would
trade part of that for SROA's work and for the emitter's: which registers
each instruction reads and writes, and every one written stored back
before each call, return and yield point (a PPC function calls every ten
to twenty instructions). A tenth of the time at best, for a second
description of each instruction.

**The header precompiled** (`game_build.cpp`, `Build::compile`): every
functions file parsed `ppc_generated.h` again, 0.55-0.6 s of clang each
(a tenth of an ordinary file's time; 290 files in game A). The recompile
step precompiles it once a build (0.7 s) for the functions files
(`-include-pch`); the objects' code is the same, byte for byte.

**VSCR[NJ]'s cost** (the same measure, with and without it, interleaved,
13-49% load): without 16.78, 16.81, 16.09 s; with 16.56, 17.16, 16.67 s
(medians 16.78 and 16.67): none measurable, so it is not behind a switch.
Two of those six runs, both slow on the wall clock (24 and 30 s for 17 s
of CPU), ended with another guest memory hash than the other four: the
deterministic mode is not quite deterministic under the host's load
(open, the kernel's).

**Against Xenia** (game B, `--render` and Xenia's Vulkan window, 20 to
60 s, interleaved, 18-99% load from other builds): CPU time a frame, ours
before the change 26.8, 26.3, 23.5 ms against Xenia's 31.2, 29.8, 27.6 ms
(ratio of medians 0.88); after 32.5, 28.5, 28.9 ms against 31.4, 32.5,
33.1 ms (0.89). Both run at the game's own pace (ours 30-50 swaps a
second, Xenia 32-45 frames a second by its title bar, through the Vulkan
SDK's monitor layer), so these mostly compare the spinning, and the load
moved them more than the change did.

## PPC in the host debugger

`xfrecomp emit ... --listing` writes `functions_K.ppc` beside each
`functions_K.cpp`: a line per function (`xf_82640868:`) and a line per
instruction (address, word, disassembly). In the C++, a `#line` directive
before each instruction's code points at its line there, and one before each
function at its name, so the debug information (CodeView or DWARF) maps the
generated code to the listing: Visual Studio or WinDbg step PPC instructions
and set breakpoints on them (build the generated code with `-g` and little
optimization: `-O0` or `-Og` keep one instruction's code apart from the
next). The listing is made from the emitter's own comment before each
instruction, after the emission, so the emitter and the game library's
interface version stay as they are; `xfbuild` does not offer it (its
emission is the recompile step's, whose text is in the interface's digest).
`tools/xenoforge.natvis` shows `xf::ppc::Context` as the PPC registers (the
condition register by field, the vector registers by word) in the watch
windows: the linker's `/NATVIS:`, or WinDbg's `.nvload`.

## The project file

(Plan E2, 2026-11-08.) What a port says about the executables on its disc,
in TOML: `xfrecomp emit <iso> <dir> --project <file.toml>` (and
`recomp::emit_modules` through `EmitOptions::project`, `recomp::load_project`
reading it; `xenoforge_add_port(... PROJECT <file>)` passes it on). No project
file, or one that says nothing of a module, emits exactly what the
recompiler emits without one (checked byte for byte on a whole disc).

The top level describes one module, `[project] module` (default
`default.xex`, compared without case); `[modules."<path on the disc>"]`
holds the same keys for any other (a disc has several executables). TOML puts
a table's plain keys before its first `[header]`, so the top level's `hooks`,
`skip` and `names_from` come first.

```toml
# my_game.toml: the port's project file
hooks = [0x82012340]            # functions the port replaces or hooks (a hook slot each, below)
skip = [0x82013000]             # functions left to the interpreter
names_from = { map = "my_game.map", ghidra = ["functions.csv"] } # names, relative to this file
includes = ["more.toml"]        # other project files, read first

[project]
name = "my_game"
module = "default.xex"          # what the top level describes

[functions]                     # starts to add, with what the port knows of them
"0x82010000" = { name = "Camera_Update", end = 0x82010100, hook = true }
"0x82010200" = {}               # a start the discovery missed (rexglue's form)
"0x82010400" = { size = 0x40 }  # an end by size (rexglue's)
"0x82010800" = { skip = true, start = false } # left out, not added

[names]                         # names only
"0x82011000" = "Render"

[[jump_tables]]                 # a bctr's targets, read in place of discovery's
bctr = 0x82010080
targets = [0x82010090, 0x820100A0]

[[mid_hooks]]                   # a hook before the instruction at an address
address = 0x82010010
jumps = [0x82010020]            # where the hook may send the code (labels)
name = "skip_check"

[modules."Other.xex"]           # another executable on the disc
hooks = [0x82500000]
[modules."Other.xex".names]
"0x82500000" = "Other_Entry"
```

- **Starts** (`[functions]`, and every hook): one with an end is registered
  before `.pdata` (its size wins); one without, after the exports and the
  logged entries, so a `.pdata` start keeps its size (`Source::Project`).
- **Names** (`[functions]`'s `name`, `[names]`, and below them those
  `names_from` imports: an MSVC linker map's "Publics by Value" and "Static
  symbols", the functions alone when the map marks them `f`; a Ghidra
  function list, the Functions window exported as CSV or tab-separated text,
  or plain lines of an address and a name, Ghidra's own `FUN_` names left
  out). They go to the function's line in the C++ (`XF_FN(xf_82010000) { //
  Camera_Update`) and so to the listing and its `#line` (the debugger's
  view of the function); to the module's table (`GeneratedModule::names`),
  from which the crash and stop reports and the lockstep check's report name
  the function; and to `names.h` beside `table.cpp`, constants for the port's
  sources (`xfm_default_xex::names::Camera_Update`). And each named
  function gets a symbol of that name (plan E5): an alias of its generated
  code, written after it in its file, in the module's namespace,

  ```cpp
  #ifdef _MSC_VER
  XF_FN(Camera_Update) __attribute__((alias("?xf_82010000@xfm_default_xex@@YAXAEAUContext@ppc@xf@@@Z")));
  #else
  XF_FN(Camera_Update) __attribute__((alias("_ZN15xfm_default_xex11xf_82010000ERN2xf3ppc7ContextE")));
  #endif
  ```

  (the target's symbol as the compiler's C++ ABI mangles it, so the
  generated symbols and every call stay `xf_<address>`; a hooked function's
  alias names its body, `xfo_`). Debuggers and profilers then show
  `xfm_default_xex::Camera_Update`. A name is made an identifier (other
  characters `_`); one that is a keyword, has no lower case (it could be a
  macro), starts as the generated symbols do (`xf`, `XF`, `__`) or is used
  twice gets `_<address>` after it (`recomp::alias_identifier`). Without a
  project file nothing is written: re-emitting games A and B gave the same
  files byte for byte as before.
- **Jump tables**: `[[jump_tables]]` (`bctr`, `targets`) or rexglue's
  `[[switch_tables]]` (`address`, `labels`; `register` ignored).
- **Skip**: the function's generated code is a stub that dispatches to its
  own address, its table entry empty, so it runs in the interpreter.
- **Hooks and mid hooks**: below, "Replacing and hooking".
- **rexglue's function list** is read as a port has it (`[functions]
  "0x82012340" = {}`, `size`, `name`; `parent` and `share_registers` are
  warned of and ignored; `[[midasm_hook]]` is a mid hook, its
  `jump_address*` its jumps, its `registers` and `return*` meaningless here,
  where the hook gets the whole context and says itself where to go on).
  `xfrecomp analyze <iso> --check-functions <file.toml>` says how many of a
  list's starts the discovery finds without it, and where the others fall
  (inside a function, at one of its branch targets, in none); `--project`
  discovers with a project file's starts and tables.
- What does not apply (a hook where no function can be, a mid hook no
  emitted instruction is at, names at no start, unknown keys) is printed as a
  warning by `xfrecomp`, never an error.

The reader is the core's own (`xenoforge/toml.h`: the TOML a project file
uses, without multi-line strings or dates), so xfrecomp needs no TOML
library.

## Replacing and hooking

(Plan E3, 2026-11-08.) A port puts native code on guest functions by their
address (`xenoforge/port.h`):

- **a replacement**, `void fn(Context &c, const Original &original)`, in
  the function's place; `original(c)` runs the function's own body
  (generated code, else the interpreter);
- **before and after**, `void fn(Context &c)`, around the function, which
  runs as ever (after sees its result in r3 or f1);
- **mid-function**, `uint32_t fn(Context &c)`, before the instruction at an
  address inside a function, with the whole context: it returns `CONTINUE`
  (the instruction runs, then the rest), `RETURN` (the function returns, as
  a `blr`: undoing the frame is the hook's) or an address to go on at (one
  of the project file's `jumps` or the next instruction is a `goto`; any
  other is dispatched, as a branch to an address with no label is). This is
  XenonRecomp's midasm hook (`../XenonRecomp`, MIT), made a run-time
  registration with one signature for its return and jump variants.

Registered at link time (`XF_REPLACE(address, name) { ... }`, `XF_BEFORE`,
`XF_AFTER`, `XF_MID`: static registrations in the port's sources, done before
`main`) or at run time (`xf::port::replace`, `before`, `after`, `mid`,
`remove`; a port's `PortInfo::on_init` is the place, xenoforge/run.h). The
porter's ergonomics follow rexglue's `REX_FUNC`/`__imp__` and re:Blue's
`REX_HOOK` in what they let a port say; the mechanism is our own: no weak
symbols, so a hook binds the same way to a statically linked game and to a
game library, and a port's code builds without the game's.

**How the generated code reaches them.** Direct `bl` calls between generated
functions bypass dispatch, so a function the project file lists in `hooks` is
emitted with a **hook slot**: its body becomes `xfo_<address>`, and
`xf_<address>`, what every caller, tail call and the table reach, is

```cpp
XF_FN(xf_82012340) { XF_HOOKED(xf_hook_82012340, xfo_82012340); }
```

a load of the slot, a compare and a tail call to the body while nothing is
hooked there; the slot (`ppc_module.h`'s `HookSlot`, defined in `table.cpp`,
listed in `GeneratedModule::hooks`) holds the hooks when the port registers
any, and the runtime runs them (`ppc_runtime.h`'s `hooked`). A mid hook's
address gets a slot `xf_mid_<address>` and, before its instruction,

```cpp
if (XF_HOOKS(xf_mid_82010010)) { // the port's mid-function hook
    const uint32_t xf_to = ::xf::ppc::mid_hook(c, xf_mid_82010010);
    if (xf_to) switch (xf_to) { case 1: return; case 0x82010014: goto L_82010014; case 0x82010020: goto L_82010020; default: ::xf::ppc::dispatch(c, xf_to); return; }
}
```

Everything the project file does not list is emitted as before, byte for
byte, so its cost is nil.

**Through the recompile step** (`xfbuild --project <file>`,
`BuildOptions::project`; plan E5): the project file goes into the
emission, its digest into the build report beside the library (`project
<digest>`), and a library cached with another project file, or none, is
built again. The step's splitter, which cuts a file clang rejects into
parts by function, keeps a hooked function's body (`xfo_`) and its wrapper
(`xf_`, `XF_HOOKED`) in one part, and a name's alias with its function. A
hooked function clang rejects keeps its wrapper (the hooks, and the
table's entry) and its body becomes a stub that asks the system for the
original (`System::run_original`, which interprets a function with a hook
slot instead of dispatching to it, as a dispatch would come back to the
wrapper; `recomp::left_out_original_stub`).

**The registry** (`xenoforge/hooks.h`, `src/hooks.cpp`) is the process's, by
address. The kernel binds a module's slots as it takes the module's code in
(`Kernel::add_module_code`) and unbinds them as it drops it; a registration
publishes a new, never-changed entry and stores it in the slots of that
address with one atomic store, so threads running the code see the old hooks
or the new ones. A bit per 4 KB page says where hooks are, so dispatch and
the interpreter look further only there.

**Every way in runs the same hooks.**

- Dispatch (indirect calls, imports' thunks, calls from interpreted code,
  `Kernel::call`): `Kernel::try_call` runs a hooked function's hooks itself
  when its code has no slot (`call_hooked`), the original body being the
  generated function or the interpreter (`System::run_original`).
- The interpreter: calls come through dispatch; a branch without LK to a
  hooked function's start from outside it (a tail call; the size comes from
  the slot, and with no slot every branch there counts, a loop back to a
  function's first instruction being rarer than a tail call) runs it and goes
  on at LR (`System::enter_hooked`); before each instruction in a page with a
  hook, its mid hooks (`System::mid_hook`).
- **An address the project file does not list** has no slot: direct calls
  in the generated code go past its hooks, and only dispatch and the
  interpreter reach them; a mid hook there runs only interpreted. The
  registry says so on stderr (`port: 0x82012400 ... is not among the project
  file's hooks`), at registration or when the module's code is taken in.
- **The lockstep check** treats the port's code as an import: the check's
  interpreter stops at a hooked call or a mid hook (a segment's end), the
  generated code runs the hooks once, for real, and the next segment starts
  where the code goes on (after the call, or after the mid hook that ran:
  `System::hook_boundary`). So hooks never run twice and the guest code
  around them is still checked.
- The reports name functions by the project file's names and those a port
  registers (`port::name`): "function Camera_Update 0x82010000+0x34".

Memory and registers for ports, in the same header: `be<T>` (a big-endian
field of a guest structure), `ptr<T>` (a guest pointer), `ref<T>(c, a)`,
`load<T>`/`store<T>`, `arg`, `farg`, `result`, `r`, `f`, and `call(c, a)`
(a guest function called from native code, through dispatch, LR kept).

`port_test` checks all of it on a small program `port_gen` emits with a
project file at build time: replace, before, after, a tail call, dispatch,
each mid-hook answer, an unlisted address (reached through dispatch, not by
the generated code's direct call; by the interpreter's), link-time and
run-time registration, generated and interpreted alike, and a lockstep run
with hooks (4 segments matched). The game library's interface gained the
two calls (`Runtime::hooked`, `mid_hook`), so its version changed.

## Steps

1. Decoder and disassembler, checked against Xenia's (above).
2. Context, memory, the interpreter and the instruction tests; semantics
   group by group (integer, branch and system, memory, floating point,
   VMX and VMX128).
3. Discovery: functions (prologues, calls, the save/restore helpers),
   jump tables, the data pointers rexglue's fork scans; compared with
   rexglue's lists.
4. Emission: C++ per function, the dispatch table, the listing; the
   instruction tests recompiled; both games compiled, then every module
   of the eight games, a table each.

## Vector float arithmetic on whole vectors (2026-11-11)

A game in play had a thread of its own at two thirds of a host core: its
sound mixer, VMX128 float code (`vaddfp128`, `vmulfp128`, `vmaddfp`, unaligned
loads and stores), one function of it 4.7% of the whole process. The vector
unit's arithmetic was done lane by lane on the bits (`vfp::lanes`: the
operands' denormals flushed, the NaNs quieted in operand order, the result's
NaN and denormal handled), a dozen operations and three branches a lane.

`vfp::add4`, `sub4`, `mul4`, `madd4` and `nmsub4` (ppc_ops/vmx_float.h) give
the same bits from the whole vector, by the compiler's vector types (SSE or
NEON as the host has, no intrinsics: the game library is built without the
standard headers): the operands flushed, the operation, the result's NaN
made the invalid operation's and its denormal flushed. A NaN among the
operands goes the lane-by-lane way, as does Java mode. Alone (a loop of
each over random and special values, the same bits both ways): an add 6.1
to 1.6 ns, a multiply-add 8.2 to 2.1 ns. The instruction tests pass as
before, interpreted and recompiled (141,705 cases). In play: the mixer's
thread 63% to 47% of a core, its hot function 4.7% to 1.5% of the process.
Then the same way: minimum and maximum (`max4`, `min4`), the compares
(`cmp_eq`, `cmp_ge`, `cmp_gt`) and the conversion from signed words
(`from_signed`). Left lane by lane: the estimates (tables read by the bits),
the conversions to integers (in double, saturating), the conversion from
unsigned words and the dot products (four products and their sums in
double already). Counted over one game's code, the forms done are the ones
it is made of: multiply 11,135 instructions, multiply-add 7,920, subtract
6,227, negative multiply-subtract 2,809, add 2,786, against 800 minimum and
maximum, 1,150 compares, 1,140 conversions from words.

**A 32-bit access's two checks** (`ppc_ops.h`: the devices' registers at
0x7FC80000, the 4 KB offset of the 0xE0000000 view) were looked at for
leaving out and stay: a game run without the first on loads faulted reading
a GPU register with `lwz`, and without the second ran another course (it
uses that memory). Sparing loads the first would take the GPU's registers
kept big-endian in guest memory where they are read; sparing the second a
mapping of that view without its offset, which Windows' 64 KB mapping
granularity does not give. Neither's gain was measured: the checks cannot
be removed to time what they cost.

## The calls' cost, measured before a new emitter (2026-11-11)

Asked: whether to emit LLVM IR instead of C++, for a calling convention of
the generated code's own (RPCS3's translator does: IR by hand, the GHC
convention, every guest call a tail call, r0, r1 and r2 the only guest
registers passed in host registers, the rest in the thread's structure,
`refs/rpcs3/rpcs3/Emu/Cell/PPUTranslator.cpp`). Within a function the C++
already keeps the guest's registers in host registers between calls
(`__builtin_assume_separate_storage`), so what a new emitter could gain is
at the calls. Two experiments on the emitter as it is, timed on a game run
deterministically to the same swap (the run's whole host time, the three
builds in turn, eight rounds, medians; other measures of it were too
noisy to use, and a first reading of 13% was the measure's, not the code's):

| build | median | fastest |
| --- | --- | --- |
| as it is | 16.33 s | 16.15 s |
| the register save and restore helpers (`__savegprlr_N`, `__restgprlr_N` and the others) emitted in place of the call to them | 16.37 s | 15.96 s |
| and each direct call told to leave the non-volatile registers the function names as they were (their values before it, `__builtin_assume` of the same after) | 16.19 s | 15.92 s |

Neither is worth 1.5% of the run, of which the generated code that is not
the game's spin wait is about half: the calls are not where the time is,
and a new emitter for their sake would not pay. Both were taken out again.

What the code is made of, counted over one game (69,231 functions): 63.5% of
its instructions are in functions with the helpers' prologue and epilogue,
23.6% in functions that save registers with their own stores, 12.8% in
functions that name no non-volatile register, and 12 functions use them
another way: the calling convention holds nearly everywhere, should
something later need to rely on it.

A benchmark of the generated code is still missing: this run is two fifths
the game's wait for the GPU thread (a spin in its Direct3D) and the
scheduler, and the machine's other work moved single runs by a tenth.

## The compile time by optimization level, measured before two tiers (2026-11-11)

The plan was two tiers: a library built quickly without optimization so a
game starts sooner, and the optimized one built behind it. Measured first,
on four files of a game's generated code (about a megabyte and 35,000
lines each; clang 23, the recompile step's flags, one compiler at a time,
twice):

| flags | the four files | an object |
| --- | --- | --- |
| `-O2` (the recompile step's) | 17.6 to 18.0 s | 565 KB |
| `-O2 -fno-slp-vectorize -fno-vectorize` | 15.8 to 17.3 s | 558 KB |
| `-O1` | 14.0 to 15.4 s | 557 KB |
| `-O0` | 13.6 s | 6,536 KB |

Without optimization the compiler takes a quarter less time and makes
objects eleven times larger (the instructions' helpers are always inlined,
and what is inlined is then emitted as written), which the linker and the
game then pay for. `-ftime-trace` on one file at `-O2` (4.1 s): the front
end 0.6 s, the optimizer 2.2 s (1.3 s of it inlining), the code generator
1.3 s. So the time follows the size of the code a guest instruction
becomes, not the level: a first tier at `-O0` or `-O1` gains too little
to be worth a second build of everything, and was not made.

What does shorten the wait, in the order it would pay: more compilers at
once where the memory allows (four is the limit set for 16 GB; the step
takes about eight minutes a game with them), a smaller expansion of the
commonest instructions (each inlined helper is compiled once a use), and
starting the game on the interpreter while the library is built (the
runtime falls back to it for code without a function already), which is
the owner's to decide: it is slow while it lasts.
