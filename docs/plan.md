# XenoForge: the plan (2026-11-04)

A lean Xbox 360 system for statically recompiled games, written from scratch
with Xenia's research as the reference: the recompiler, the kernel calls, the
GPU, the sound and the input the games ask for, measured, not a whole
emulator. The method is the 3DS runtime's (`../runtime`): its own
recompiler, its own renderer, a player and a launcher, tools that run a game
without a window and compare it with a reference emulator.

## Principles

1. **Measured, not guessed.** The kernel implements what the games import
   and call ([lean-kernel.md](lean-kernel.md)); the renderer what the games
   and the traces use. What is asked for and missing is counted and
   reported, never silently skipped.
2. **The hardware, not the games.** The kernel and the renderer implement
   the console. What is particular to a game lives in its port (hooks,
   native rendering, options), never in XenoForge.
3. **An oracle, not a dependency.** xenia-canary (`../xenia-canary`, BSD) is
   the reference for how the console behaves; the tools compare against it.
   The rexglue fork keeps running El Chavo Kart and Ben 10 until XenoForge
   runs them.
4. **Translate what is specified and dull, write anew what is design.**
   Translated from Xenia, with its notice at the top of each file and checked
   against it in the same session: the disc, the executable, the export
   tables, formats and tiling, the microcode's layout, VMX128's semantics.
   Written anew: the recompiler's output, the scheduler and the kernel's
   objects, the renderer, the tools, the player and the launcher.
5. **Nothing of any game in the repository.** XenoForge reads the player's
   own disc; ports and their libraries are built on the player's machine.
6. **Every step checks itself**: tests in the repository, and a check against
   real data or the oracle written down with the step.

## Stack

C++20, CMake, Ninja, clang (MSVC tolerated); SDL3, Vulkan 1.3, Dear ImGui,
toml++, fetched by CMake when needed. Windows first, Linux kept building.
The recompiled code includes one small header (the guest context, memory
access, the calls into the system), so its thousands of files compile fast.

## Architecture

| Part | What it is | State |
| --- | --- | --- |
| Disc | XDVDFS read from the ISO in place (`disc.h`) | done |
| Executable | XEX2 headers, AES, basic and normal (LZX) compression, imports (`xex.h`, `lzx.h`) | done |
| Exports | xboxkrnl, xam, xbdm by ordinal (`exports.h`) | done |
| `xfinspect` | a disc's executable, its XDK libraries and imports, the image for reverse engineering | done |
| Recompiler | PPC and VMX128 to C++: discovery, emission, `#line` listing, yield points, dispatch table | step A2 |
| Interpreter | PPC and VMX128, from the same instruction descriptions as the recompiler | step A3 |
| Memory | 4 GB of guest space, the physical mirrors (0xA0000000, 0xC0000000, 0xE0000000), page guards, the GPU's registers as MMIO | step A4 |
| Kernel | objects, waits, threads, APCs and DPCs, TLS, files; exports declared with their signatures, each call counted | step A4 |
| XAM | content, users, input, settings; the network offline, the guide's UI cancelled | step A4 |
| Sound | the APU's XMA contexts (XAudio2 itself is the game's code) | step A6 |
| GPU | PM4 front end and the Xenos renderer ([renderer.md](renderer.md)) | track B |
| Player | SDL3 window, input, sound, ImGui overlay (F1), `config.toml` | C1 in ([runtime.md](runtime.md), [input.md](input.md)) |
| Launcher | the player's ISO set up and recompiled on their machine; settings, controls, port options | track C, from the 3DS runtime |
| `boot` | a game run without a window: counts, shots, recordings, the deterministic mode | step A4 |

## Threads

The Xbox 360 runs six hardware threads and its games use them, with
`lwarx`/`stwcx.` spin locks and job systems. So there are two modes, over
one kernel written against a scheduler interface:

- **Playing**: each guest thread is a host thread; waits are the host's.
- **Deterministic**: every guest thread is a fiber on one host thread, on a
  virtual clock. The recompiler's yield points (where loops turn back and
  functions start, as the 3DS runtime's interface 6) and the reservation
  instructions hand over; the same input gives the same run. For `boot`,
  recordings, comparisons, and for catching the races the playing mode
  shows.

## Debugging

- **PPC in the host debugger.** The recompiler writes a listing of each
  function's PPC and `#line` directives into the C++ pointing at it, so
  Visual Studio or WinDbg step through PPC instructions; a natvis shows the
  guest context's registers.
- **Guest breakpoints without rebuilding.** A function with a breakpoint is
  sent to the interpreter (the dispatch table), which checks every
  instruction; watchpoints use page guards. A GDB remote stub on top lets
  IDA, Ghidra or gdb attach to the guest.
- **The overlay's debugger**: threads and their guest stacks (a shadow stack
  in debug builds), memory big-endian, the kernel calls counted, the GPU's
  counts.
- **Reverse engineering**: `xfinspect --dump-image` (done; replaces
  El Chavo Kart's `xex_dump_image.py`, byte for byte the same image), then
  an index of calls, references and strings, and the discovered functions
  and imports exported to Ghidra.
- **GPU frames** in Xenia's trace format, so a frame captured here opens in
  Xenia's trace viewer and the other way round.

## Porting a game

- The launcher reads the player's disc, recompiles the game on their
  machine and keeps two libraries: **the game** (generated, rebuilt only when
  the executable or its discovery changes) and **the port** (hooks, native
  rendering, options). Hooks bind by guest address through the dispatch
  table, so a port rebuilds alone in seconds and, where it can, reloads
  without restarting the game.
- **Code checked as it loads**: each function's guest bytes are hashed when
  it is recompiled; what no longer matches (a title update, a patch, a mod)
  runs in the interpreter, as the 3DS runtime's `recomp_origins` does.
  Title updates (`.xexp` delta patches) apply at load on the same footing,
  their changed functions interpreted until the next build takes them in.
- **The XDK by signature**: the static libraries header gives each game's
  XDK version (`xfinspect` prints it); signatures of the XDK's functions per
  version name D3D, XAudio2 and the CRT in any game. Hooks on D3D come
  without reverse engineering, and the CRT's hot functions (`memcpy`,
  `memset`, string functions) run as the host's.
- Port options (`PORT_OPTION`) in the player and the launcher, texture packs
  and mods as in the 3DS runtime.

## Steps

Three tracks; A and B run in parallel because the renderer is checked on
traces, without a recompiled game.

**Track A: the CPU and the system**

- **A1. Disc and executable** (done 2026-11-04). `xfinspect` reads both
  games' ISOs in place; El Chavo Kart's image is byte for byte the one its
  Python script made; the imports measured ([lean-kernel.md](lean-kernel.md)).
- **A2. The recompiler** (under way, 2026-11-04; [recompiler.md](recompiler.md)).
  Done: the decoder and disassembler, from Xenia's instruction table,
  identical to Xenia's on every word of both games' code and 2M random
  words; every instruction's semantics once (`ppc_ops.h`), for the
  generated code and the interpreter alike; of Xenia's 170,383 instruction
  test cases (many generated on the console), all 141,696 Xenia runs pass,
  interpreted and recompiled, and 26,086 of the 28,687 it skips (the rest:
  the console's low-precision estimates and double-denormal operands of
  single-precision forms); discovery (`.pdata`, helpers, calls, gaps, jump
  tables by following CTR's value backwards, the data pointer scan after
  rexglue's fork: 73,587 functions and 595 tables on El Chavo Kart, 21,896
  and 473 on Ben 10, no direct branch to an address without a start, the
  differences with rexglue's lists explained in
  [recompiler.md](recompiler.md)); emission (labels, calls, tail calls, jump
  table switches, dispatch); both games emitted and compiled, and in 20 s of
  `boot` neither enters the interpreter (El Chavo Kart did 262,094 times
  before the scan and the gap fix). Then (2026-11-05, later): generated code
  per module (a table per executable and DLL, matched to the image loaded by
  its digest, dispatch through a slot per instruction), files that declare
  what they call and are rewritten only when they change, and all eight
  games emitted (28M instructions, 16 modules) and built
  ([recompiler.md](recompiler.md), "Modules and the game library"). Next:
  exception funclets from the scope tables, the listing and `#line`, yield
  points.
- **A3. The interpreter** (in, 2026-11-04): the same ops, the branches its
  own; runs the instruction tests and whatever dispatch finds without
  generated code.
- **A4. Memory, loader, kernel, `boot`.** Both games run without a picture
  (the GPU counts packets and draws) to their title loop; every kernel call
  counted; the counts set against the imports. Under way (2026-11-05,
  [runtime.md](runtime.md)): the memory, modules, Rtl, threads and sync,
  video (the GPU's live front end, headless), files and xam, and crypto
  families are in; MMIO reaches the devices. Then (2026-11-05, later):
  sound enough to boot (XAudio's render driver clients paced on the
  scheduler's clock, the XMA decoder walking contexts to silence; real
  output and decoding are A6), XexLoadImage of a second executable with
  NT's loader lock, and `boot_<game>` linked with the generated code
  (CMake's `XF_GAMES`). With their code generated, both games run their
  frame loop at their caps (El Chavo Kart 30 swaps a second, Ben 10 60)
  with no interpreter entry and no unimplemented call; interpreted, all
  eight games run 30 s without stopping. Then (2026-11-05, later): a
  generated table per module, matched to each image as it loads, and the
  runtime's unwinding for longjmp and fiber switches; all eight games run
  30 s with their code generated, every module's (30 s of `boot_<game>`,
  generated / interpreted, swaps a second over the run):

  | game | first swap | swaps a second | interpreter entries (generated) |
  |---|---|---|---|
  | El Chavo Kart | 1.0 s / 1.1 s | 30.0 / 14.4 | 0 |
  | Ben 10 | 1.0 s / 13.0 s | 60.0 / 58.5 | 0 |
  | BC2 (launcher, Splash.xex) | 1.2 s / 11.3 s | 30.0 / 14.4 | 16 (8 longjmps) |
  | Forza Horizon (+ XMediaFacade) | 1.1 s / 6.4 s | 52.7 / 13.8 | 0 |
  | Gears of War 3 | 1.0 s / 1.1 s | 14.9 / 2.2 | 0 |
  | Reach (+ four Waves DLLs) | 1.4 s / 3.2 s | 58.4 / 25.2 | 1,803 (fiber switches) |
  | Red Dead Redemption | 5.0 s / 8.5 s | 27.0 / 21.5 | 0 |
  | Sonic Unleashed | 2.0 s / 21.1 s | 29.9 / 24.1 | 0 |

  Gears of War 3, generated, gets as far as its sockets: the xam NetDll_*
  it calls are not implemented (NetDll_socket answers 0), and it polls
  NetDll_recvfrom 4.26M times in 30 s.
- **A5. The deterministic mode**, input recording and replay.
- **A6. Sound** (XMA: Xenia decodes it with FFmpeg's XMA decoder from its own
  fork; LGPL, linked dynamically), input, saves.

**Track B: the GPU** ([renderer.md](renderer.md))

- **B1. Traces** (under way, 2026-11-04). xenia-canary built from source with
  `-DXENIA_ENABLE_GPU_TRACE=ON -DXENIA_BUILD_MISC=ON` (release downloads
  leave the trace writer out; `build/version.h` is written by hand as
  `xb` would), so it records traces and its `trace-dump` draws the
  oracle's picture of each. XenoForge reads them (`gpu_trace.h`) and counts
  what each frame uses (`xftrace`). First traces: both games' intros, with
  no input; the game itself needs a player pressing F4 in Xenia, or input
  sent to it.
- **B2. The microcode translator** to SPIR-V, with its harness (version 1
  in, 2026-11-04; [renderer.md](renderer.md), "The translated shader"):
  the microcode parsed as Xenia parses it (its disassembly, byte for byte,
  is the check); the translator (every ALU operation, the 15 vertex formats,
  2D and 3D texture fetches, straight control flow) for the vertex and
  pixel stages and a compute wrapper; a CPU interpreter written apart from
  it, from Xenia's; the two agree on the GPU on every operation's edge
  cases and on the 13 shaders of the traces, and breaking the legacy
  multiply makes 163 cases differ. Version 2 (2026-11-04): loops, calls
  and jumps through a dispatcher with the interpreter's stacks, cube maps,
  getGradients and the LOD and gradient setters. On the 118 shaders of El
  Chavo Kart's UE3 global shader cache (pulled from its data by
  `xfeffect`), the 10 compiled from `tests/hlsl/` and the 13 of the
  traces, every pixel shader translates and validates, and the GPU and
  the interpreter agree on all 141 (whole shaders within 1e-4: rcp's 21
  bits differ by a step and subtractions grow it). Fetch offsets,
  unnormalized coordinates (not yet on cube maps) and stacked textures
  came with B3. Left: memexport, getCompTexLOD and getWeights, the
  parameter generator.
- **B3. The renderer**: front end, render targets, textures, draws; a trace
  replayed and compared with Xenia's picture. First form in (2026-11-04,
  [renderer.md](renderer.md), "The renderer, first form"): EDRAM moved
  between render targets as Xenia moves it; all ten frames of both games
  match Xenia's (within 0.013 of 255). Driven live (2026-11-05,
  [renderer.md](renderer.md), "Driven live"): the renderer is the command
  processor's Sink, traces replay through the same processor, and `boot
  --render --shots` draws both games headless and writes chosen swaps.
  El Chavo Kart's title screen (2026-11-05, later): it came out red
  because D24FS8 depth was kept as 0-2 in a 0-1 host depth, so the color
  bits the game moves through a float depth target lost their green and
  blue; kept halved, as Xenia's host targets keep it, the title matches
  Xenia's trace (mean 0.000) and live (identical but for its pulsing
  "Press START"). Also in: point lists (sprites through a geometry
  shader), texture fetch offsets with Xenia's rounding epsilon,
  unnormalized coordinates, stacked textures as 2D arrays, depth and
  EXPAND textures as Xenia loads them, 2_10_10_10_FLOAT as 32 bits in
  EDRAM, draws that can write nothing dropped. Next: traces of more games,
  and of the games themselves past their titles.
- **B4. Scale and speed**: resolution scale, the pipeline cache on disk,
  shaders translated in the build.
- **B5. The generic pipeline** (the microcode interpreter on the GPU), if
  hitches still show.

**Track C: the frontend**, from the 3DS runtime's

- **C1. The player** (window, input, sound, overlay, settings; in,
  2026-11-05: [runtime.md](runtime.md), "The player"; [input.md](input.md)).
  `player_<game>` links the game's generated code as `boot_<game>` does
  and shares boot's opening, launch and report (`tools/game.cpp`); SDL3,
  Dear ImGui and toml++ are fetched only when it is built. The window shows
  the front buffer the renderer reads back at each swap, through a Vulkan
  swapchain on a device of its own, letterboxed; the guest never waits for
  it (a front buffer is read back only when the window took the last one).
  XAM's input exports answer from drivers in Xenia's order: SDL3's
  gamepads as Xenia's SDL driver, then the keyboard as a controller with
  Xenia's bindings (config.toml); boot answers from an idle pad for user 0
  (`--no-controller` for none), so Ben 10 no longer pauses on "Controller
  Lost Connection" and plays on past the Cartoon Network logo to
  Papaya's. Sound: each XAudio client's frames to an SDL3 stream, folded to
  stereo as Xenia folds 5.1, paced on the scheduler's clock as before.
  Checked without a person: `--quit-after`, `--screenshots`, the exit
  report, `--press start@100` taking El Chavo Kart from its title screen to
  the main menu (its first-time dialog); a person at the screen still has
  to confirm that a pad moves things and that sound is heard (PCM frames
  reach the device, XMA is silence until A6).
- **C2. The launcher** (ISO, recompile, settings, controls, port options).

**Where they meet**

- **M1. El Chavo Kart on XenoForge with a picture** (A4, B3, C1), then
  Ben 10.
- **N1. Native**: the XDK's D3D by signature, widescreen, the UE3 work
  (`../Chaves/docs/native-renderer-re.md`) moved over as El Chavo Kart's port.

Next (2026-11-05, later still): the player (C1) is in, so M1 is there as a
person would see it, at the renderer's speed (B4: a few swaps a second on
El Chavo Kart's menus, the renderer the bottleneck). Earlier: the renderer
runs live as the command processor's
Sink, headless (`boot --render`); next the player (C1: the window and the
presentation, input) for M1, and the renderer's speed (B4: draws batched
into fewer submissions, resolves on the GPU). Generated tables per module
and the other six games' code generated are in (A2, A4); left there: fiber
switches and longjmps leave the rest of the frames they skip interpreted (a
fiber scheduler of our own, as Xenia re-enters the thread, would keep them
generated), and what each game asks of the kernel past 30 seconds.

## Decisions

- 2026-11-04: written from scratch, Xenia as the reference; named
  XenoForge. Vulkan 1.3 only. xenia-canary is the oracle; the rexglue fork
  is left as it is and keeps running the two games meanwhile.
- 2026-11-04: the renderer is general (the hardware, any game), not made for
  the two games; the corpus of traces decides what comes first.
- 2026-11-04: AES written from FIPS-197 (no library), checked on its
  vectors. The export tables are Xenia's files, unchanged.
- 2026-11-04: the static libraries' approval sits in bits 5-6 of their flags
  byte (0x40 approved), not in the whole byte as Xenia's struct has it.
- 2026-11-04: hedge-dev's XenonRecomp and XenosRecomp (MIT, cloned beside
  in `../XenonRecomp` and `../XenosRecomp`) are references too, and their
  MIT code may be taken where it helps, with their notice (`LICENSE-hedge-dev`
  once something is taken). Not as the base: XenonRecomp decodes with GNU
  binutils' disassembler (GPL v2+, `thirdparty/disasm`), which never enters
  XenoForge, and both are built around one game (Sonic Unleashed). XenoForge
  stays MIT, with Xenia's BSD notice on what is translated from it.
- 2026-11-04: LZX (normal compression, Battlefield: Bad Company 2) written
  from [MS-PATCH] (`lzx.cpp`, MIT); the block chain translated from Xenia.
  The images of 40 executables (BC2's three, 37 system updates on other
  discs) are identical to Xenia's (libmspack), by `tools/xenia_oracle`.

## Open

- The recompiler's discovery: translated from rexglue's (BSD), or written
  anew on Xenia's scanner; decided at A2 after reading both.
- XMA: Xenia's FFmpeg fork (its `xmaframes` decoder) or upstream FFmpeg's
  `xma2`; decided at A6.
