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
  First through the interpreter, then recompiled.
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
- `lswi`/`stswi` follow the manual (Xenia has none of the string
  instructions); `lswx`/`stswx` are not done (their count is XER bits
  57-63, which the context does not keep; neither game uses them).
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
  sets both bits. `vrefp`, `vexptefp`, `vlogefp` stay exact as Xenia's (the
  console's estimates are skipped tests); `vrsqrtefp` uses the console's
  table. The dot products and D3D packs follow Xenia's sequences bit for bit
  (checked on 2M random vectors per type), but for FLOAT16 -0 and denormals,
  where Xenia's fast path looks wrong. All of it assumes the host rounds to
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
  are done. Left: double-denormal operands of single forms (the console
  gives a NaN), `fres`'s 8-bit estimate.
- For step A4: Xenia adds 0x1000 to addresses at 0xE0000000 and above (the
  4 KB offset of that physical view); `ops::host` is the one place to do it.

## Threads and the system

As the 3DS runtime's interface 6: each guest thread runs on its own stack
(a host thread when playing, a fiber in the deterministic mode); a system
call is a C++ call that comes back. The generated code never leaves partway;
yield points where loops turn back and functions start hand over in the
deterministic mode (plan, "Threads").

## Discovery

Where functions start, where they end, and what each indirect branch can
reach. Learned from rexglue's fork (`../rexglue-sdk/src/codegen`, BSD), which
runs both games today, and from XenonAnalyse (`../XenonRecomp`, MIT):

- **Starts**, the more certain winning: the port's own entries; the import
  thunks; the register save and restore helpers (`__savegprlr_14`... found
  by their first words, one entry per register as rexglue does); `.pdata`
  (8-byte entries: begin, then prolog length, length in instructions,
  32-bit and exception flags; with the exception flag, the handler and its
  scope or FuncInfo table give the funclets); the entry point; the data
  pointer scan's (below); `bl` targets; last, gaps no function covers.
  On El Chavo Kart `.pdata` alone gives 52,360 of the 73,587 functions.
- **The data pointer scan** (`scan_pointers`, `DiscoverOptions`; translated
  from the fork's `codegen/pointer_scan.cpp`, its `[analysis]
  scan_data_pointers`): every word of the sections that are not code
  (vtables, callback tables, fields), and every `lis rX`/`addi rY, rX` pair
  in code, whose value looks like a function's first instruction (`mflr
  r12`, `stwu r1`, or after `blr`/`bctr`/`b`/0) is a candidate; engines
  without RTTI (UE3) reach most of their methods only so. A baseline run
  says which the image explains already (a start, or a branch target inside
  a function); the rest become starts and discovery runs again. A candidate
  that splits a function leaves the function's own branches across the
  split pointing at no start (`Discovery::unresolved`: what the generated
  code would hand to the interpreter, where rexglue's builders would trap);
  the culprit is dropped (the first candidate after the branch going
  forward, the last before it going backward, else every one in the span),
  and the passes go on until nothing changes; branch targets no function
  holds are added the same way. Branches unresolved without the scan say
  nothing about the candidates. Not in rexglue: an address inside a jump
  table discovery read is no start (the `lis`/`addi` that loads a table's
  address after its `bctr` looks like one), nor one inside the prologue
  `.pdata` gives (a `stwu r1` there); the passes rerun the whole discovery
  (under a second each), so there is no cache of dropped candidates. Inside
  a `.pdata` function past its prologue pointers do find entries: the
  funclets an exception scope table names. `xfrecomp --no-pointers` leaves
  the scan out.
- **Gaps**: code no function covers gets a function after each end (`blr`,
  `bctr`, `b`) and after padding, as rexglue splits its gaps; each is
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

## Steps

1. Decoder and disassembler, checked against Xenia's (above).
2. Context, memory, the interpreter and the instruction tests; semantics
   group by group (integer, branch and system, memory, floating point,
   VMX and VMX128).
3. Discovery: functions (prologues, calls, the save/restore helpers),
   jump tables, the data pointers rexglue's fork scans; compared with
   rexglue's lists.
4. Emission: C++ per function, the dispatch table, the listing; the
   instruction tests recompiled; both games compiled.
