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
| Executable | XEX2 headers, AES, basic compression, imports (`xex.h`); LZX when a game needs it | done but LZX |
| Exports | xboxkrnl, xam, xbdm by ordinal (`exports.h`) | done |
| `xfinspect` | a disc's executable, its XDK libraries and imports, the image for reverse engineering | done |
| Recompiler | PPC and VMX128 to C++: discovery, emission, `#line` listing, yield points, dispatch table | step A2 |
| Interpreter | PPC and VMX128, from the same instruction descriptions as the recompiler | step A3 |
| Memory | 4 GB of guest space, the physical mirrors (0xA0000000, 0xC0000000, 0xE0000000), page guards, the GPU's registers as MMIO | step A4 |
| Kernel | objects, waits, threads, APCs and DPCs, TLS, files; exports declared with their signatures, each call counted | step A4 |
| XAM | content, users, input, settings; the network offline, the guide's UI cancelled | step A4 |
| Sound | the APU's XMA contexts (XAudio2 itself is the game's code) | step A6 |
| GPU | PM4 front end and the Xenos renderer ([renderer.md](renderer.md)) | track B |
| Player | SDL3 window, input, sound, ImGui overlay (F1), `config.toml` | track C, from the 3DS runtime |
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
- **A2. The recompiler.** Instruction decoding translated from Xenia
  (`cpu/ppc/ppc_opcode_*`, `ppc_decode_data.h`); discovery (functions,
  jump tables, the data pointers rexglue's fork learned to scan); emission
  with the listing, yield points and dispatch table. Checked by Xenia's 571
  instruction tests (`cpu/ppc/testing`, assembled with the VMX128 binutils
  in `../rexglue-sdk/tools/binutils`), and by recompiling both games.
- **A3. The interpreter**, from the same instruction descriptions; the same
  tests pass recompiled and interpreted.
- **A4. Memory, loader, kernel, `boot`.** Both games run without a picture
  (the GPU counts packets and draws) to their title loop; every kernel call
  counted; the counts set against the imports.
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
  bits differ by a step and subtractions grow it). Left: memexport, fetch
  offsets, unnormalized coordinates, getCompTexLOD and getWeights, the
  parameter generator.
- **B3. The renderer**: front end, render targets, textures, draws; a trace
  replayed and compared with Xenia's picture. First form in (2026-11-04,
  [renderer.md](renderer.md), "The renderer, first form"): EDRAM moved
  between render targets as Xenia moves it; all ten frames of both games
  match Xenia's (within 0.013 of 255). Next: traces of more games, and of
  the games themselves past their titles.
- **B4. Scale and speed**: resolution scale, the pipeline cache on disk,
  shaders translated in the build.
- **B5. The generic pipeline** (the microcode interpreter on the GPU), if
  hitches still show.

**Track C: the frontend**, from the 3DS runtime's

- **C1. The player** (window, input, sound, overlay, settings).
- **C2. The launcher** (ISO, recompile, settings, controls, port options).

**Where they meet**

- **M1. El Chavo Kart on XenoForge with a picture** (A4, B3, C1), then
  Ben 10.
- **N1. Native**: the XDK's D3D by signature, widescreen, the UE3 work
  (`../Chaves/docs/native-renderer-re.md`) moved over as El Chavo Kart's port.

Next: A2 and B1, side by side.

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

## Open

- LZX (normal compression): written here from Microsoft's open
  specification ([MS-PATCH]), checked against Xenia's (libmspack) output,
  when a game needs it. Neither game does.
- The recompiler's discovery: translated from rexglue's (BSD), or written
  anew on Xenia's scanner; decided at A2 after reading both.
- XMA: Xenia's FFmpeg fork (its `xmaframes` decoder) or upstream FFmpeg's
  `xma2`; decided at A6.
