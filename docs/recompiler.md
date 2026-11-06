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
  Xbox 360); El Chavo Kart's own `dcbz` loops step by 128.
- Reservations: `lwarx`/`ldarx` record address and value after a fence;
  `stwcx.`/`stdcx.` store with one compare-exchange against the reserved
  value and drop the reservation; CR0's SO is left alone, as Xenia.
- The string instructions follow the manual (Xenia has none of them, nor
  tests: `tests/ppc` has some). `lswx`/`stswx` move the byte count XER
  keeps in bits 57-63, which mtxer sets and mfxer reads back (the context's
  `xer_bc`); with a count of 0 nothing moves.
- Traps call `system->trap` when they fire (Xenia ignores them by default);
  `sc` calls `system->syscall`. VSCR is SAT alone, NJ always set.
- VMX: VSCR[SAT] is sticky (Xenia's backends always clear it). `vsl`/`vsr`
  shift each byte by its own count with the neighbour's bits, as the
  console does where the counts differ (Xenia uses byte 15's for all and
  skips those 876 tests; they pass here). The 22 instructions Xenia lacks
  (`vmsum*`, `vmul[eo]*`, `vsum*`, `vmhaddshs`, `vmhraddshs`, `vmladduhm`,
  `vupk[hl]px`) follow the AltiVec PEM.
- VMX floating point runs in non-Java mode: denormals in and out flush to
  a zero of their sign. `vmaddfp`/`vnmsubfp` round once (`std::fma`; one
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
yield points where loops turn back and functions start hand over in the
deterministic mode (plan, "Threads"). Each guest stack a thread moves to (a
fiber's) runs on a host stack of its own, so KeSetCurrentStackPointers is a
call that comes back when its stack is switched back to ("Stack switches").

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
  export directory, as GetProcAddress reads them); the data pointer scan's
  (below); `bl` targets; last, gaps no function covers. On El Chavo Kart
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
  never returns (the walk runs on past the `bl`; Forza Horizon and Reach),
  a tail two functions share (Reach); the holder then runs on into it as a
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
  pass (rounds of one gap per run left all but the first 8 of El Chavo
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
  SwitchToFiber, in Reach) may return to another frame than its caller's
  (`Function::switches_stack`); one that keeps r1 in memory r1 does not
  address and returns (setjmp, or a tail branch to one) saves the stack
  (`saves_stack`); the return sites of the calls to those, and the catch
  continuations of the functions with exception data, are resume sites
  (`resumes`). How the generated code follows them: "Stack switches" below.
- **Everything else** that is indirect (`bctr`, `bctrl`, `blrl`, a branch
  to an address no function holds) goes through the dispatch table, and an
  address with no generated code runs in the interpreter: a wrong guess
  costs speed, never correctness. `boot` counts those entries, which is how
  discovery is improved, game by game, without traps (rexglue leaves 26
  unresolved branches and 26 unresolved table entries on El Chavo Kart, and
  needed 633 starts added by hand; here no direct branch or table case of
  either game reaches an address without a start).
- **Checked** against rexglue's lists (`xfrecomp analyze --compare`): El
  Chavo Kart's project (74,112 entries: 294 imports, 73,818 bodies, 633 of
  them added by hand) and lists the fork wrote with its own scan for both
  games (`rexglue init` and `codegen` with `[entrypoint.analysis]
  scan_data_pointers = true`, outside the repository; Ben 10's fails
  validation without it). Counts on 2026-11-05:

  | | El Chavo Kart | Ben 10 |
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
  function's own branch reaches (105 on El Chavo Kart, `bne` over `li r3;
  blr`), labels here; a `stwu r1` inside a prologue (1); 2 on each game
  after a `b` inside a function the scan found, labels here. Only here:
  leaf methods and adjustor thunks right after another function's `bctr`
  or `b` that rexglue merges into it (115 and 8), and 3 functions without
  `.pdata` after a `blr` (El Chavo Kart). Before, El Chavo Kart's project
  list had 1,369 starts only there: the leaf method runs the gap filler's
  rounds left uncovered, besides its hand-added labels and table data.
  In 20 s of `boot` linked with the generated code, El Chavo Kart entered
  the interpreter 262,094 times at 802 addresses before (those leaf runs:
  `lwz r11, 0xA8(r3); lwz r10, 0x624(r11); rlwinm r3, ...; blr`, reached
  through vtables) and Ben 10 10 times at 9 (small functions the same
  rounds left uncovered); now neither does. El Chavo Kart's first swap comes at 1.0 s instead of 1.5 s, 593
  swaps in the 20 s instead of 509 (30 a second, its cap), and it gets as
  far as registering its XAudio client; Ben 10 1,154 instead of 1,043 (60
  a second).

## Modules and the game library

A game is every executable on its disc, not one: Forza Horizon loads
`XMediaFacade_default.xex`, Reach four Waves DLLs, BC2 boots a launcher
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
  reads; SHA-1 would have cost 80 ms of El Chavo Kart's start), and its
  functions by address. `modules.cpp` lists every module's table
  (`xf_modules`); the kernel gives a table its module's range when an image
  with that digest loads, so another version of a module or a title update
  runs interpreted, never another image's code.
- **Files**: one per 64 KB of the image (about 16,000 instructions), each
  declaring only what it calls, written only when its text changes. A
  discovery fix then rebuilds the files it touched (Forza Horizon's and
  Reach's below: 2 and 4 files of 224 and 149), where one header declaring
  every function made any change rebuild the whole game, and every file
  parsed 1.5 MB of declarations (about 4 minutes per million instructions
  were measured with that layout under other builds' load).
- **Optimization**: `XF_GAME_OPT` (default `-O2`) and `XF_GAME_OPT_<game>`.
  Measured here at -O2 (12 threads): El Chavo Kart's 4.7M instructions in
  269 s; the other six games' 21.7M in 1,465 s, part of it beside another
  build; Ben 10's 1.7M in 143 s, and 100 s at -O1. Linking a `boot_<game>`
  takes about a minute, which dominates a rebuild after a discovery fix (60
  files of the eight games and eight links: 534 s). -O2 stays the default:
  every game builds in under half an hour.

The eight games (2026-11-05, with the stack switches generated, "Stack
switches" below; functions with generated code, all but the imports;
instructions are their sizes / 4; switches: functions that switch the
stack, their returns checked; resumes: resume sites, in how many
functions; unresolved: direct branches and table cases to no start, with
the scan):

| game | module | functions | instructions | switches | resumes | unresolved |
|---|---|---|---|---|---|---|
| El Chavo Kart | default.xex | 73,293 | 4,720,516 | 1 | 9 in 9 | 0 |
| Ben 10 | default.xex | 21,720 | 1,712,043 | 2 | 7 in 7 | 0 |
| BC2 | Default.xex | 292 | 12,518 | 0 | 0 | 0 |
| | Splash.xex | 6,834 | 679,357 | 1 | 7 in 7 | 0 |
| | Game.xex | 54,018 | 2,773,730 | 2 | 2 in 2 | 0 |
| Forza Horizon | default.xex | 76,047 | 3,655,459 | 3 | 11 in 11 | 0 |
| | XMediaFacade_default.xex | 3,163 | 525,032 | 0 | 0 | 0 |
| | SpeechFacade_default.xex | 6,649 | 374,049 | 1 | 1 in 1 | 0 |
| Gears of War 3 | default.xex | 56,302 | 3,465,904 | 2 | 2 in 2 | 0 |
| Reach | default.xex | 37,904 | 2,424,688 | 3 | 41 in 25 | 0 |
| | waveShell-Xbox.dll | 2,653 | 104,041 | 1 | 49 in 43 | 0 |
| | wavesLibDLL.dll | 3,373 | 142,016 | 1 | 64 in 56 | 0 |
| | waves\L360.dll | 1,352 | 47,348 | 1 | 2 in 2 | 0 |
| | waves\Q10.dll | 1,147 | 38,123 | 1 | 2 in 2 | 0 |
| Red Dead Redemption | default.xex | 69,231 | 3,418,826 | 0 | 1 in 1 | 0 |
| Sonic Unleashed | default.xex | 65,842 | 4,046,585 | 2 | 156 in 106 | 0 |

Two discovery fixes came from running them: the branch targets inside a
walked function (above, "The data pointer scan"), and a .pdata function's
end as an end for the gap filler and the pointer scan whatever its last
instruction: a function that ends calling one that never returns (`bl
_CxxThrowException`) is followed by another, Sonic's adjustor thunks
(`addi r3, r3, 0xC; b ...`) reached only through vtables, which the gap
filler skipped for want of a `blr` or `b` before them (Sonic entered the
interpreter 135,471 times at 4 of them in 30 s; 12 such starts on Sonic, 1
each on Forza Horizon, Reach and two of its DLLs).

Forza Horizon had 8, one byte-offset table's cases (0x829EB474: `rlwinm
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
  setjmp filled, r3 the value (BC2's Splash.xex, 8 times in 30 s; Reach's
  streaming decompressor, to bail out of bad input);
- **the jump to a catch's continuation** in the C++ exception machinery: a
  CONTEXT restored and a `blr` to the address the catch block returned;
- **SwitchToFiber**, the XDK's: the registers saved into the current fiber,
  the next one's loaded (LR and r1 among them), then a tail branch to
  KeSetCurrentStackPointers, which moves r1 to the next fiber's stack
  (Reach's decompressor runs on a fiber of its own and switches back to its
  caller whenever its input runs out: 1,202 switches in 30 s of `boot`, 34
  fibers made and deleted).

A C++ return cannot go into the middle of another function's frame. Until
now those functions were left to the interpreter, and their returns were
thrown up to the dispatch whose frames held the restored r1, which
interpreted the guest's frames from there up to its own return (Reach
entered the interpreter 1,803 times in 30 s, BC2 16; after its first
switch, a fiber of Reach's ran interpreted for good). Now the generated
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
  (checked on Reach: 0x821D6038's two catch funclets return 0x821D60AC and
  0x821D60B0; the pointer scan took the first for a start, which it stays,
  and the label is emitted in the parent too). A function with resume sites
  runs its body in a try block that takes an Unwind with r1 inside its frame
  to one of them (Reach's setjmp caller, a decompressor entry):

  ```cpp
  XF_FN(xf_827D2B80) {
      const uint32_t xf_sp = uint32_t(c.r[1]);
      uint32_t xf_at = 0; // where an Unwind came back to
      for (;;) {
          try {
              switch (xf_at) {
              case 0x827D2BA4: goto L_827D2BA4;
              }
              ...
              c.lr = 0x827D2BA4; xf_829B20A0(c); // setjmp
          L_827D2BA4:
              ...
              return;
          } catch (const ::xf::ppc::Unwind &xf_unwind) {
              if (uint32_t(c.r[1]) >= xf_sp) throw; // a frame further up
              switch (xf_unwind.target) {
              case 0x827D2BA4:
                  xf_at = xf_unwind.target;
                  continue;
              }
              throw;
          }
      }
  }
  ```

  The generated code keeps nothing outside the context but `xf_sp`, `xf_lr`
  and `xf_at`, so a frame goes on at a label exactly as the guest's does.
  There are no tail calls in the try block (`musttail` cannot leave one).
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
  switching functions alone (20 in the 16 modules), the try blocks in the
  functions with resume sites (zero-cost tables on x64; 274 functions, 354
  sites, most of them catch continuations in Sonic and Reach's Waves DLLs:
  the table above), and an Unwind is a C++ exception (tens of
  microseconds), only for longjmps and catches.

Measured (2026-11-05, 30 s of `boot_<game>`, whose report now says what
took each switch): Reach enters the interpreter 0 times (1,803 before: 1,202
at SwitchToFiber, 567 at the decompressor's return from it, 34 at the fiber
start routine); its 1,202 switches are host fiber switches, its 34 fibers
get 34 host fibers, dropped with their stacks (MmDeleteKernelStack), and
nothing is unwound. BC2 enters it 0 times (16 before: 8 at longjmp, 8 at the
setjmp caller's return site); its 8 longjmps are taken by the setjmp
caller's generated frame at its resume site. The other six games enter it 0
times, as before, and none of them switches stacks or unwinds in its first
30 s. Swaps a second are as before, at the games' caps when the machine is
not loaded (Reach 58 to 60, BC2 29.3); the switches and longjmps were too
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
