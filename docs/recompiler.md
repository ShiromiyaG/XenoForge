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
  scope or FuncInfo table give the funclets); `bl` targets; then pointers:
  words in data sections and `lis`/`addi` pairs that look like a function's
  first instruction (`mflr r12`, `stwu r1`, or after `blr`/`bctr`/`b`/0),
  for engines without RTTI (UE3); last, gaps no function covers.
  On El Chavo Kart `.pdata` alone gives about 52,400 of the 73,800
  functions rexglue emits.
- **Ends**: `.pdata`'s length where there is one; else the blocks reached,
  never past the next known start. A function that runs on into the next one
  ends in a tail call to it (rexglue copies the code instead, and functions
  without a size there swallow their neighbours).
- **Jump tables**: the four forms the XDK's compiler writes (XenonAnalyse's
  patterns, rexglue's detectJumpTable): `lwzx` of absolute words; `lbzx` of
  byte offsets added to a base, or shifted by an `rlwinm` first; `lhzx` of
  halfword offsets; bounded by the `cmplwi` or `clrlwi` on the index before.
- **Everything else** that is indirect (`bctr`, `bctrl`, `blrl`, a branch
  to an address no function holds) goes through the dispatch table, and an
  address with no generated code runs in the interpreter: a wrong guess
  costs speed, never correctness. `boot` counts those entries, which is how
  discovery is improved, game by game, without traps (rexglue leaves 26
  unresolved branches and 26 unresolved table entries on El Chavo Kart, and
  needed 633 starts added by hand).
- **Checked** against rexglue's list for El Chavo Kart (74,112 entries: 294
  imports, 73,818 bodies) and `.pdata`; the differences are counted and
  explained.

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
