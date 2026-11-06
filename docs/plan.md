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
  | BC2 (launcher, Splash.xex) | 1.2 s / 11.3 s | 30.0 / 14.4 | 16 (8 longjmps; 0 now, below) |
  | Forza Horizon (+ XMediaFacade) | 1.1 s / 6.4 s | 52.7 / 13.8 | 0 |
  | Gears of War 3 | 1.0 s / 1.1 s | 14.9 / 2.2 | 0 |
  | Reach (+ four Waves DLLs) | 1.4 s / 3.2 s | 58.4 / 25.2 | 1,803 (fiber switches; 0 now, below) |
  | Red Dead Redemption | 5.0 s / 8.5 s | 27.0 / 21.5 | 0 |
  | Sonic Unleashed | 2.0 s / 21.1 s | 29.9 / 24.1 | 0 |

  Gears of War 3, generated, gets as far as its sockets: the xam NetDll_*
  it calls are not implemented (NetDll_socket answers 0), and it polls
  NetDll_recvfrom 4.26M times in 30 s.

  Then (2026-11-05, later): stack switches in generated code
  ([recompiler.md](recompiler.md), "Stack switches"; [runtime.md](runtime.md),
  "Stacks"). Each guest stack runs on a host fiber of its own, so
  KeSetCurrentStackPointers (the XDK's SwitchToFiber) is a host fiber switch
  and each fiber's generated frames wait where they are; the functions that
  switch the stack (longjmp, the jump to a catch's continuation,
  SwitchToFiber) are generated, their returns checked; a longjmp or a catch
  goes on in the generated frame that called setjmp or holds the try, at a
  resume site discovery found. In 30 s, Reach enters the interpreter 0 times
  (1,803 before), its 1,202 switches host switches, its 34 fibers on 34
  host fibers dropped with their stacks; BC2 0 times (16 before), its 8
  longjmps taken by the setjmp caller's frame; the other six 0, as before.
  All eight run 30 s generated.
- **A5. The deterministic mode**, input recording and replay.
- **A6. Sound** (XMA in, 2026-11-05; input, saves to come). XMA is decoded
  by FFmpeg's `xmaframes` decoder from has207's fork, Xenia's own (commit
  d980192), driven as Xenia's XmaContextNew drives it; FFmpeg (LGPL) is a
  shared library of its own, `ffmpeg-xma`, fetched and built from the part
  the decoder needs (CMake's `XF_XMA`, on by default; off, XMA decodes to
  silence) ([runtime.md](runtime.md), "Sound"). Checked against Xenia on the
  games' own input without putting it in the repository: the kernel records
  what each context consumed (`XF_XMA_RECORD`), Xenia's XmaContextNew walks
  the same recording (`tools/xenia_oracle`'s `xma_oracle`, linked with
  xenia-canary's libraries and its FFmpeg), and `xma_replay_test` compares
  the two run by run. Every run of the recordings made so far, 28,329 runs
  in five games, leaves the same context data and the same PCM, byte for
  byte, as Xenia's (`boot_<game>` with scripted presses):

  | game | seconds | runs | contexts kicked | frames | loops, starved runs, releases |
  |---|---|---|---|---|---|
  | Ben 10 | 60 | 3,352 | 2 | 1,370 | 0, 0, 0 |
  | El Chavo Kart | 220 (its menus) | 4,819 | 30 | 2,960 | 0, 0, 37 |
  | Forza Horizon | 120 | 4,515 | 4 | 1,962 | 0, 0, 0 |
  | Halo: Reach | 120 | 5,332 | 30 | 2,286 | 3, 0, 0 |
  | Red Dead Redemption | 120 | 10,311 | 83 | 2,690 | 23, 14, 208 |

  Gears of War 3 and Sonic Unleashed kicked none in 40 s (interpreted), BC2
  was not run. Decoding costs little: RDR, which decodes about 1,100 frames
  a second, keeps its client at 172 frames a second against 184 with
  silence (`XF_XMA_SILENT=1`). The WAVs written from the recordings
  (`XF_XMA_WAV`) and of the clients' mix (`XF_AUDIO_WAV`) are music and
  effects, not yet heard by a person.

**Track B: the GPU** ([renderer.md](renderer.md))

- **B1. Traces** (under way, 2026-11-04). xenia-canary built from source with
  `-DXENIA_ENABLE_GPU_TRACE=ON -DXENIA_BUILD_MISC=ON` (release downloads
  leave the trace writer out; `build/version.h` is written by hand as
  `xb` would), so it records traces and its `trace-dump` draws the
  oracle's picture of each. XenoForge reads them (`gpu_trace.h`) and counts
  what each frame uses (`xftrace`). First traces: both games' intros, with
  no input; the game itself needs a player pressing F4 in Xenia, or input
  sent to it. Then (2026-11-05, later) XenoForge writes them itself
  (`gpu_trace_writer.h`, Xenia's TraceWriter translated): `boot
  --trace-frames` (or `XF_TRACE_FRAMES` in the player) records chosen frames
  of a live run, which Xenia's trace dump draws, so any scene our runs reach
  has Xenia's own picture of the same commands ([renderer.md](renderer.md),
  "Traces of live runs").
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
  came with B3. Left: memexport (getCompTexLOD, getWeights and the
  parameter generator came with the traces of live runs, 2026-11-05).
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
  shaders translated in the build. The renderer's speed first (2026-11-05,
  [renderer.md](renderer.md), "Decisions" and "Speed"): a frame's work is
  recorded into one submission, three in flight, render passes kept open
  across draws, constants, indices and vertex data in an upload arena,
  textures bindless; resolves run on the GPU into a mirror of guest memory
  (the CPU's resolve ported to a compute shader), the textures over them
  are loaded there on the GPU (the CPU decode's plan, ported), guest memory
  gets the bytes when the GPU is done, and front buffers are read back
  without waiting. Rendering now costs the games little: Ben 10 renders at
  its 60 swaps a second (14.7 before, the renderer busy 2.3 s of 45
  instead of 41.3), El Chavo Kart's title at its 30 (8 before), within a
  percent or two of the same runs headless; the eleven trace frames match
  Xenia as before, and live both games draw Xenia's dumps of their intros
  and titles exactly (Ben 10's too, now that boot answers an idle pad).
  Left: the vertex data and the CPU's texture decodes
  (most of what the renderer still spends), resolution scale, the pipeline
  cache.
- **B5. The generic pipeline** (the microcode interpreter on the GPU), if
  hitches still show.

**Track C: the frontend**, from the 3DS runtime's

- **C1. The player** (window, input, sound, overlay, settings; in,
  2026-11-05: [runtime.md](runtime.md), "The player"; [input.md](input.md)).
  `player_<game>` links the game's generated code as `boot_<game>` does
  and shares boot's opening, launch and report (`tools/game.cpp`); SDL3,
  Dear ImGui and toml++ are fetched only when it is built. The window shows
  each swap's front buffer through a Vulkan swapchain on the renderer's
  own device, letterboxed, the picture made on the GPU at the swap and
  never read back (since 2026-11-05: before, it was read back and made a
  picture on the CPU, on a device of its own, which slowed El Chavo Kart's
  start to about 15 frames a second); the guest never waits for it.
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
  reach the device; XMA is decoded since A6).
- **C2. The launcher** (ISO, recompile, settings, controls, port options).

**Where they meet**

- **M1. El Chavo Kart on XenoForge with a picture** (A4, B3, C1), then
  Ben 10.
- **N1. Native**: the XDK's D3D by signature, widescreen, the UE3 work
  (`../Chaves/docs/native-renderer-re.md`) moved over as El Chavo Kart's port.

Next (2026-11-05, end of the day): M1 is there as a person sees it. El
Chavo Kart and Ben 10 run generated in the player, rendered at their caps
(30 and 60 swaps a second, B4), with input and with XMA decoded as Xenia
decodes it (A6). The owner played El Chavo Kart in the player: a pad moves
it past the title into the menus, and the sound is right (music and
effects). They saw small color errors past the menus (under way). The
keyboard is not confirmed yet.

Then, in order:
1. Play both games through: what they ask of the kernel and the renderer
   past the menus (races, levels, saves). Begun for the renderer
   (2026-11-05, later): traces written by our own runs and drawn by Xenia
   as the reference; El Chavo Kart's menus, its park scene and a race went
   from 55-131 of 255 off Xenia (3D frames) to 0.005-0.14 (gamma, signed
   and biased textures, pixel parameters, the LOD bias, samplers, polygon
   offset and render targets keyed by storage format as Xenia's;
   [renderer.md](renderer.md), "Traces of live runs"). Next there:
   memexport (the race's kart is black, in Xenia's picture of our traces
   too).
2. ~~The player presents the front buffer on the GPU~~ (done 2026-11-05:
   renderer.md, "Decisions"; runtime.md, "The player"). FFmpeg is loaded
   at run time and the player has a package to ship (runtime.md, "Sound").
3. The other six games under the renderer, against Xenia's traces. Begun
   with our own traces (2026-11-05, later; [renderer.md](renderer.md),
   "Traces of live runs"): Forza Horizon matches Xenia with the display's
   PWL gamma ramp in; Sonic Unleashed's title matches with getCompTexLOD
   and getWeights in; Red Dead Redemption's title is 10.9 of 255 off
   (textures it makes from MSAA gamma targets), and its renderer cost is
   the texture checks. BC2, Gears of War 3 and Reach not run yet.
4. The kernel families they reach first. Done for the first two: the
   network offline (NetDll_*, XNet; Gears of War 3 polls its socket once a
   frame and runs at 30 swaps a second) and XCustomRegisterDynamicActions
   (Reach); runtime.md, "XAM". Next for those two: the XamShow*UI calls,
   XamVoiceSetMicArrayIdleUsers (looked up, absent in Xenia too).
5. A fiber scheduler of our own (done, 2026-11-05: each guest stack on a
   host fiber of its own, the switching functions generated, longjmps and
   catches taken by the frame that set them up; recompiler.md, "Stack
   switches"; runtime.md, "Stacks"). All eight games enter the interpreter
   0 times in 30 s (Reach 1,803 before, BC2 16).
6. The launcher (C2), the deterministic mode (A5), and the A2 leftovers
   (exception funclets, `#line`, yield points).

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
- XMA: decided at A6, Xenia's FFmpeg fork (its `xmaframes` decoder, one
  frame a packet, as Xenia's walk hands them), so the PCM is Xenia's byte
  for byte. Upstream FFmpeg's `xma2` decodes whole packets and would need
  a walk of its own.

## Everything left (2026-11-05)

What is known to be missing for XenoForge to be a whole Xbox 360 for
recompiled games, whether or not the eight games ask for it yet. It is taken
from the code: the instruction table, the renderer's `approximate` and `skip`
counters, the translator's `unsupported`, the texture table, and Xenia's
exports set against ours. What a game asks for still comes first (the
steps above); this is the inventory they draw from. Update it when
something lands.

### The recompiler and the CPU (A2, A3, A5)

Stack switches came in on 2026-11-05: guest fibers run on host fibers,
and longjmp and catch continuations unwind host frames. All eight games
now enter the interpreter 0 times in 30 s; before, Reach entered it 1,803
times and BC2 16.

- **Stack switches still left to the interpreter.** These are correct but
  slower, and none of the eight games has hit one:
  - returns to an address that discovery did not mark as a resume site;
  - a stack first entered mid-function;
  - a fiber that moves to another thread;
  - an interpreted longjmp landing in its dispatch's caller.
  Catch continuations are covered only by tests so far. One Reach stall at
  startup under heavy load (1 run in 16) is not explained.

- **Instructions.** 454 of the 456 in Xenia's table have semantics. Left:
  `lswx` and `stswx`, whose byte count is in XER.
- **Precision.** 2,601 of Xenia's instruction test cases still fail:
  - the console's low-precision estimates (`fres`, `frsqrte`, `vrefp`,
    `vrsqrtefp` to the console's bits);
  - double-denormal operands of the single-precision forms.
- **Exceptions.** SEH `__try`/`__except` and C++ `catch`: the funclets
  should come from the scope and FuncInfo tables, checked in a game that
  throws. Generated catch continuations came in with the stack switches.
- **Discovery.** Forza Horizon's 8 table cases past the mask. Every
  interpreter entry in every game should be counted down to 0.
- **Code that is not on the disc at build time:**
  - title updates: apply the `.xexp` delta patch to the base executable,
    then recompile (`xex.cpp` refuses patches today);
  - executables inside packages (DLC, XBLA, Games on Demand; see STFS
    below);
  - `XexLoadImageFromMemory`, and code written at run time.
  Each is recompiled at install (the launcher) or interpreted.
- **Debugging and determinism:**
  - `#line` and the listing in the generated C++;
  - yield points where loops turn back and functions start;
  - the deterministic mode (A5), with input recording and replay.
- **Build time.** Rebuild only the modules that changed. Today all eight
  games at `-O2` take about half an hour.

### The kernel and the system (A4, A6)

**Exports, against Xenia.** Xenia's counts include its stubs; the appendix
below names every missing export.

| | the export table | Xenia implements | XenoForge implements | Xenia's, missing here |
| --- | --- | --- | --- | --- |
| xboxkrnl | 922 | 310 | 287 | 46 |
| xam | 1,741 | 382 | 135 | 247 |
| xbdm | 255 | 38 | 0 | 36 |

Exports that neither implements wait until a game asks for them.

**The XAM families missing:**
- **Win32 re-exports:** `CreateThread`, `Sleep`, `WaitForSingleObject`,
  `GetTickCount`, `CloseHandle`, `GetLastError` and the like, which XAM
  exports for titles.
- **Profiles and accounts:**
  - `XamProfile*`;
  - `XamUserGet*`: gamertag, membership tier, flags, tenure, online
    country and language;
  - `XamUserLogon*`, content restrictions, user preferences, guests.
  Today there is one profile. Several profiles signed in at once is
  local multiplayer.
- **Locale and strings:**
  - `XamGetLocale*`, `XamGetLanguage*`, `XamGetCountry*`,
    `XamGetOnline*`;
  - `XamFormatDateString` and `XamFormatTimeString`;
  - `XamReadString`, `XamLookupCommonStringByIndex`, `XTLGetLanguageV2`.
- **The system's UI:**
  - `XamShow*`: sign-in, profile creation and editing, gamercard,
    marketplace, upgrades, and the Kinect ones;
  - an overlay in the player for what these dialogs show. GoW3 and Reach
    already call some of them.
- **Content:**
  - the device enumerator, device names and volume paths;
  - thumbnails, opening and resolving files, change callbacks;
  - launching images, and the `Internal` variants.
- **Avatars:** `XamAvatar*` (24 exports).
- **Kinect (NUI):** `XamNui*`, `XamUserNui*`, `XamShowNui*`, the camera,
  `XamIsNatal*` (about 35 exports).
- **Tiles:** `XamReadTile*`, `XamReadTileToTexture`, `XamWriteGamerTile`,
  `XamParseGamerTileKey`.
- **Loading and launching:**
  - `XamLoaderGetMediaInfo*`, the tray state, launch request callbacks;
  - `XamSwapDisc`, for games on several discs;
  - `XamContentLaunchImage*`, plus xboxkrnl's `XexLoadExecutable` and
    `XexLoadImageFromMemory`.
- **The dashboard and system queries:**
  - the dash context, system title IDs, system update versions;
  - DVR storage, feature flags, the Live hive, IPTV, XAuth, network
    storage;
  - `XNotifyBroadcast` and `XNotifyQueueUI`.
- **The network beyond what came in on 2026-11-05:**
  - asynchronous Winsock (`WSARecv`, `WSASend`, `WSARecvFrom`,
    `WSASendTo`, overlapped results, `WSAEventSelect`; Forza Horizon and
    BC2 import them);
  - `XHttp*`, `XNetStartupEx`, `XNetInAddrToString`.
  Host sockets for system link (LAN play) would be an option, off by
  default.
- **Input beyond the pad:**
  - `XamInputNonController*`, raw input;
  - the other XInput device subtypes: guitar, drums, wheel, dance pad,
    arcade stick, big button;
  - the chatpad, and Kinect.

**The xboxkrnl families missing:**
- **Locks and settings:** `Ex*` read-write locks; writing `XConfig`
  settings.
- **Kernel primitives:**
  - `KeInitializeEvent`, `KeInitializeTimerEx`, `KePulseEvent`;
  - `KeRemoveQueueApc` and `KeRemoveQueueDpc`;
  - `KeSetPriorityThread`, `KeSuspendThread`, `KfRaiseIrql`.
- **Objects:**
  - `NtQueueApcThread`, `NtQueryEvent`, the symbolic link calls;
  - `ObCreateObject`, `ObLookupThreadByThreadId`, `ObOpenObjectByName`,
    `ObReferenceObjectByName`.
- **Hardware:**
  - `Hal*`: the tray, SMC messages, the AV pack;
  - `HidReadKeys`;
  - `Vd*`: the GPU's ASIC ID and the real video mode;
  - `XUsbcam*`: the camera.
- **Runtime helpers:**
  - `Rtl*`: CRC-32, stack limits, image headers;
  - the wide `printf` family;
  - `FscGetCacheElementCount`, `InterlockedPushEntrySList`.

**XBDM, the dev kit's debug monitor:** `Dm*` and the CAP profiler hooks.
Retail games rarely call it.

**Beyond the exports:**
- **Packages.** Read STFS and SVOD: DLC, title updates, XBLA and Games on
  Demand titles. Today an STFS package a title mounts itself gets no
  device. Title updates are applied as above.
- **Saves and profiles.** Saves are content packages in Xenia's layout
  (runtime.md, "The storage root"). Still to come:
  - saving and loading checked in play;
  - saves kept per profile;
  - achievements written and shown;
  - gamer pictures;
  - the profile settings games read, beyond the defaults.
- **Voice.** XHV and `XamVoice*`: the headset, mic input, Kinect audio.
- **Video and settings.** The video modes and widescreen setting games
  read (480p, 720p, 1080i), and the dashboard's `XConfig` settings, from
  config.toml.
- **The deterministic mode (A5).** The kernel's side of it: a virtual
  clock and thread order.

### The GPU (B)

**Under way:**
- a GPU trace writer in Xenia's format, so any scene a run reaches can be
  checked against Xenia's trace dump;
- El Chavo Kart's colors past the menus.

The player presents on the GPU and keeps up with the guest, and the
pipeline cache is on disk (both came in on 2026-11-05).

**Approximated** (drawn, but counted as not what the console does):
- **Textures:**
  - signed components;
  - biased components;
  - gamma textures read as linear;
  - exponent adjust;
  - formats sampled at 8 bits per component;
  - mip levels shorter than their size;
  - vertex fetches from a texture fetch constant;
  - an ALU operand from a fetch constant;
  - samplers past the bindless table.
- **Output:**
  - the display's piecewise linear gamma ramp;
  - D24FS8 depth kept as 32-bit float;
  - clears of some target formats;
  - float16 and float32 targets moved through EDRAM by value rather than
    by bits (NaN payloads).
- **Rasterizer:**
  - polygon offset;
  - polygon modes other than fill (wireframe, points);
  - alpha to coverage;
  - pixel parameter generation (point sprite coordinates).
- **Resolves:**
  - resolves of one MSAA sample, which are averaged instead;
  - depth resolves of MSAA targets.

**Not drawn** (each draw is skipped and counted):
- **Memexport:** shaders writing guest memory, then what they overlap
  invalidated.
- **Primitive types:**
  - triangle with flags (7), line loop (12), quad strip (14), polygon (15);
  - the 2D copy rectangle lists (16 to 20);
  - tessellation: line, triangle and quad patches (21 to 23), with the
    tessellator.
- **Shaders:**
  - unnormalized cube map coordinates;
  - the undefined scalar opcode 41;
  - an exec past the end of the microcode.
- **Texture formats (25):**
  - 1-bit: `k_1`, `k_1_REVERSE`;
  - `k_6_5_5`;
  - packed YUV: `k_Cr_Y1_Cb_Y0_REP`, `k_Y1_Cr_Y0_Cb_REP`;
  - `k_10_11_11` and `k_11_11_10`, plain and `_AS_16_16_16_16`;
  - `k_32_AS_8` and `k_32_AS_8_8`;
  - the MPEG formats and the interlaced ones;
  - `k_32_32_32_FLOAT`;
  - `k_DXT3A_AS_1_1_1_1`;
  - the EDRAM formats read as textures: `k_16_16_EDRAM`,
    `k_16_16_16_16_EDRAM`, `k_8_8_8_8_GAMMA_EDRAM`,
    `k_2_10_10_10_FLOAT_EDRAM`.
  Also tiled 1D textures.
- **Occlusion queries:** the answers are fake, as Xenia's default. Real
  host queries are left.

**Infrastructure and speed:**
- **Shader compiles** (the pipeline cache on disk is in):
  - compiling on a worker thread with the generic pipeline drawing
    meanwhile (B5);
  - the shaders translated in the launcher's build step.
- **The renderer's remaining cost:**
  - vertex data and the CPU's texture decodes;
  - knowing when the CPU writes guest memory by watching its pages, as
    Xenia does, instead of hashing at each use.
- **Options:** resolution scale (B4), anisotropic filtering, frame pacing
  and vsync.
- **The other six games:** none of them has been checked against Xenia
  yet.

**Complete:**
- the vertex formats: all 15 that Xenia knows;
- the color target formats: all 12;
- both depth formats.

### Appendix: Xenia's exports missing here (2026-11-05)

xboxkrnl (46): ExAcquireReadWriteLockExclusive,
ExAcquireReadWriteLockShared, ExInitializeReadWriteLock,
ExReadModifyWriteXConfigSettingUlong, ExReleaseReadWriteLock,
ExSetXConfigSetting, ExTryToAcquireReadWriteLockExclusive,
ExTryToAcquireReadWriteLockShared, FscGetCacheElementCount,
HalGetCurrentAVPack, HalOpenCloseODDTray, HalSendSMCMessage, HidReadKeys,
InterlockedPushEntrySList, KeInitializeEvent, KeInitializeTimerEx,
KePulseEvent, KeRemoveQueueApc, KeRemoveQueueDpc, KeSetPriorityThread,
KeSuspendThread, KfRaiseIrql, NtOpenSymbolicLinkObject, NtQueryEvent,
NtQuerySymbolicLinkObject, NtQueueApcThread, ObCreateObject,
ObLookupThreadByThreadId, ObOpenObjectByName, ObReferenceObjectByName,
RtlComputeCrc32, RtlGetStackLimits, RtlImageDirectoryEntryToData,
RtlImageNtHeader, RtlRip, VdGetGraphicsAsicID, VdQueryRealVideoMode,
XUsbcamCreate, XUsbcamGetState, XexLoadExecutable, XexLoadImageFromMemory,
_snwprintf, _vscwprintf, _vsnwprintf, swprintf, vswprintf.

xam (247): CloseHandle, CreateThread, EnumerateMediaObjects,
EnumerateMediaObjects_, EnumerateMediaObjects_0, EnumerateMediaObjects_1,
ExitThread, GetCurrentThreadId, GetLastError, GetModuleHandleA,
GetSystemTimeAsFileTime, GetTickCount, NetDll_WSARecvFrom,
NetDll_WSASendTo, NetDll_WSAStartupEx, NetDll_XHttpOpen,
NetDll_XHttpStartup, NetDll_XNetGetDebugXnAddr, NetDll_XNetInAddrToString,
NetDll_XNetStartupEx, QueryPerformanceFrequency, Refresh, ResumeThread,
RtlGetLastError, RtlRandom, RtlSetLastNTError, RtlSleep, Sleep, SleepEx,
WaitForSingleObject, WaitForSingleObjectEx, XAutomationpUnbindController,
XGetAudioFlags, XGetOverlappedExtendedError, XGetVideoCapabilities,
XNotifyBroadcast, XNotifyQueueUI, XTLGetLanguageV2, XamAllocEx,
XamAvatarBeginEnumAssets, XamAvatarEndEnumAssets, XamAvatarEnumAssets,
XamAvatarGenerateMipMaps, XamAvatarGetAssetBinary, XamAvatarGetAssetIcon,
XamAvatarGetAssets, XamAvatarGetAssetsResultSize,
XamAvatarGetInstalledAssetPackageDescription, XamAvatarGetInstrumentation,
XamAvatarGetManifestLocalUser, XamAvatarGetManifestsByXuid,
XamAvatarGetMetadataRandom, XamAvatarGetMetadataSignedOutProfile,
XamAvatarGetMetadataSignedOutProfileCount, XamAvatarInitialize,
XamAvatarLoadAnimation, XamAvatarManifestGetBodyType,
XamAvatarReinstallAwardedAsset, XamAvatarSetCustomAsset,
XamAvatarSetManifest, XamAvatarSetMocks, XamAvatarShutdown,
XamAvatarWearNow, XamBuildGamercardResourceLocator,
XamBuildLegacySystemResourceLocator, XamBuildResourceLocator,
XamBuildSharedSystemResourceLocator, XamBuildXamResourceLocator,
XamContentCreate, XamContentCreateDeviceEnumerator,
XamContentCreateEnumeratorInternal, XamContentCreateInternal,
XamContentDeleteInternal, XamContentGetAttributesInternal,
XamContentGetDeviceName, XamContentGetDeviceVolumePath,
XamContentGetLocalizedDeviceData, XamContentGetLocalizedString,
XamContentGetMetaDataInternal, XamContentGetThumbnail,
XamContentLaunchImage, XamContentLaunchImageFromFileInternal,
XamContentLaunchImageInternal, XamContentLaunchImageInternalEx,
XamContentOpenFile, XamContentRegisterChangeCallback, XamContentResolve,
XamContentResolveInternal, XamContentSetThumbnail,
XamContentSetThumbnailInternal, XamDoesOmniNeedConfiguration,
XamFeatureEnabled, XamFirstRunExperienceShouldRun, XamFormatDateString,
XamFormatTimeString, XamGetActiveDashAppInfo, XamGetCachedTitleName,
XamGetCountry, XamGetCountryFromOnlineCountry, XamGetCountryString,
XamGetDashBackstackData, XamGetDashContext, XamGetDvrStorage,
XamGetLanguage, XamGetLanguageFromOnlineLanguage,
XamGetLanguageLocaleFallbackString, XamGetLanguageLocaleString,
XamGetLanguageString, XamGetLanguageTypeface, XamGetLanguageTypefacePatch,
XamGetLiveHiveValueA, XamGetLiveHiveValueW, XamGetLocale,
XamGetLocaleDateFormat, XamGetLocaleEx, XamGetLocaleFromOnlineCountry,
XamGetLocaleString, XamGetOnlineCountryFeatures,
XamGetOnlineCountryFromLocale, XamGetOnlineCountryString,
XamGetOnlineLanguageAndCountry, XamGetOnlineLanguageAndCountryString,
XamGetOnlineLanguageString, XamGetOnlineSchema, XamGetStagingMode,
XamInputNonControllerGetRaw, XamInputNonControllerGetRawEx,
XamInputNonControllerSetRaw, XamInputNonControllerSetRawEx,
XamIptvGetServiceName, XamIsChildAccountSignedIn, XamIsCurrentTitleDash,
XamIsIptvEnabled, XamIsNatalPlaybackEnabled, XamIsNuiAutomationEnabled,
XamIsNuiUIActive, XamIsSystemExperienceTitleId, XamIsSystemTitleId,
XamIsXbox1TitleId, XamLoaderGetDvdTrayState, XamLoaderGetMediaInfo,
XamLoaderGetMediaInfoEx, XamLoaderRegisterLaunchRequestCallback,
XamLogLocalizationEtx, XamLookupCommonStringByIndex,
XamMediaVerificationClose, XamMediaVerificationInject,
XamNetworkStorageHasUserEnabledStorage, XamNetworkStorageUserHasPrivilege,
XamNotifyCreateListenerInternal, XamNuiCameraElevationGetAngle,
XamNuiCameraGetTiltControllerType, XamNuiCameraSetFlags,
XamNuiCameraTiltGetStatus, XamNuiCameraTiltSetCallback,
XamNuiGetDepthCalibration, XamNuiGetDeviceStatus,
XamNuiHudGetEngagedTrackingID, XamNuiHudGetInitializeFlags,
XamNuiHudGetVersions, XamNuiHudIsEnabled, XamNuiHudSetEngagedTrackingID,
XamNuiIdentityAbort, XamNuiIdentityEnrollForSignIn,
XamNuiIdentityGetSessionId, XamNuiIsChatMicEnabled, XamNuiIsDeviceReady,
XamNuiPlayerEngagementUpdate, XamNuiSkeletonGetBestSkeletonIndex,
XamParseGamerTileKey, XamProfileClose, XamProfileCreate,
XamProfileCreateEnumerator, XamProfileEnumerate, XamProfileFindAccount,
XamProfileGetCreationStatus, XamProfileGetLiveLegalLocale,
XamProfileLoadAccountInfo, XamProfileOpen, XamQueryLiveHiveA,
XamQueryLiveHiveW, XamReadString, XamReadTile, XamReadTileEx,
XamReadTileToTexture, XamSetActiveDashAppInfo, XamSetCountry,
XamSetDashContext, XamSetDvrStorage, XamShowCreateProfileUI,
XamShowCreateProfileUIEx, XamShowEditProfileUI, XamShowForcedNameChangeUI,
XamShowGamerCardUI, XamShowGoldUpgradeUI, XamShowMarketplaceUIEx,
XamShowMultiplayerUpgradeUI, XamShowNuiDeviceSelectorUI,
XamShowNuiDirtyDiscErrorUI, XamShowNuiGuideUI, XamShowNuiHardwareRequiredUI,
XamShowNuiMessageBoxUI, XamShowNuiSigninUI, XamShowNuiTroubleshooterUI,
XamShowSigninUIEx, XamShowSigninUIp, XamSwapDisc,
XamUpdateGetBaseSystemVersion, XamUpdateGetCurrentSystemVersion,
XamUserContentRestrictionCheckAccess, XamUserContentRestrictionGetFlags,
XamUserContentRestrictionGetRating, XamUserCreateTitlesPlayedEnumerator,
XamUserGetCachedUserFlags, XamUserGetGamerTag, XamUserGetMembershipTier,
XamUserGetMembershipTierFromXUID, XamUserGetOnlineCountryFromXUID,
XamUserGetOnlineLanguageFromXUID, XamUserGetSubscriptionType,
XamUserGetUserFlags, XamUserGetUserFlagsFromXUID, XamUserGetUserTenure,
XamUserIsGuest, XamUserIsOnlineEnabled, XamUserIsParentalControlled,
XamUserIsUnsafeProgrammingAllowed, XamUserLogon, XamUserLogonEx,
XamUserNuiEnableBiometric, XamUserNuiGetUserIndex,
XamUserNuiGetUserIndexForBind, XamUserNuiGetUserIndexForSignin,
XamUserReadProfileSettingsEx, XamUserReadUserPreference,
XamUserWriteUserPreference, XamVoiceGetMicArrayStatus, XamWriteGamerTile,
XamXStudioRequest, XampWebInstrumentationSetProfileCounts,
XampXAuthGetTitleBuffer, XampXAuthShutdown, XampXAuthStartup,
XapiFormatTimeOut, XapipCreateThread, XapipGetLocale, lstrlenW.

xbdm (36): DmAllocatePool, DmCaptureStackBackTrace, DmCloseLoadedModules,
DmFindPdbSignature, DmFreePool, DmGetConsoleDebugMemoryStatus,
DmGetConsoleType, DmGetMemory, DmGetSystemInfo, DmGetThreadInfoEx,
DmGetThreadList, DmGetXbeInfo, DmGetXboxName, DmIsDebuggerPresent,
DmIsFastCAPEnabled, DmMapDevkitDrive, DmNotify, DmOpenNotificationSession,
DmPMCGetCounterName, DmPMCInstallAndStart, DmQueryTitleMemoryStatistics,
DmRegisterCommandProcessor, DmRegisterCommandProcessorEx, DmResumeThread,
DmSendNotificationString, DmSetDumpMode, DmSetMemory, DmSetProfilingOptions,
DmStartProfiling, DmStopProfiling, DmSuspendThread, DmWalkLoadedModules,
__CAP_End_Profiling, __CAP_Enter_Function, __CAP_Exit_Function,
__CAP_Start_Profiling.
