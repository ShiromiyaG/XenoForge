# The Xenos renderer (design, 2026-11-04)

XenoForge's own renderer for the Xbox 360's GPU, on Vulkan. **General**: it
implements the hardware, not cases of games, so any game's command stream
goes through it. **Lean**: one path for each thing, on a modern Vulkan floor,
and features implemented as the games and the trace corpus ask for them;
whatever is asked for and not done is counted, never guessed.

Xenia's GPU code (`xenia-canary/src/xenia/gpu`, BSD) is the reference for the
hardware's behavior, the way Zakuro's was for the 3DS runtime's renderer. The
code here is written anew; where a table or a formula is taken over (formats,
tiling, register layouts), the file says so and carries Xenia's notice.

## Shape

```
ring buffer (PM4)  ──>  front end                    src/gpu/
  the game's D3D,        packets, registers, constants,
  recompiled, writes     events, interrupts, fences
  it; Vd* points the           │ draw / resolve / clear
  GPU at it                    v
                         translation
                           shaders: ucode -> SPIR-V (cached, built ahead)
                           vertex fetch: in the shader, from guest memory
                           state: blend, depth, stencil, viewport
                               │
                               v
                         Vulkan back end              src/vk/
                           render targets: images keyed by EDRAM range
                           textures: keyed by fetch constant, bindless
                           guest memory: a mirror resolves write, an upload
                             arena per submission
```

The front end runs on its own thread, the game's threads only write the ring
buffer and the registers (as the 3DS runtime's `AsyncGpu`); the player
presents the frames the front end hands over.

## The floor

Vulkan 1.3 with descriptor indexing (bindless textures), buffer device
address, extended dynamic state 1 and 2, 16-bit storage. GPUs since about
2018 on Windows and Linux have all of it. No D3D12, no fragment shader
interlock (not on every GPU), no fallback paths for older hardware: each of
those is a second implementation Xenia carries and XenoForge does not.

## Decisions

**Shaders: translated, never compiled while playing if it can be helped.**
- A translator from Xenos microcode to SPIR-V, written here, with Xenia's
  `spirv_shader_translator*.cc` (about 14,000 lines with the output-merger
  emulation XenoForge leaves out) as the reference for each instruction's
  meaning.
- Every shader a game uses is kept in its cache (by hash), and its pipelines
  in Vulkan's pipeline cache on disk. The launcher's build step translates
  the shaders gathered from earlier runs and from traces, so a game played
  again compiles nothing. Built so far (2026-11-05): the pipeline cache, a
  file per game (`render::Options::pipeline_cache`; boot's in a temporary
  directory's `xenoforge-cache`, the player's in a `cache` folder beside
  its config.toml), read when the renderer is made, given to every
  pipeline, written once 300 swaps pass without a new pipeline and at
  `drain()`. El Chavo Kart's title screen makes about 24 pipelines in its
  first frame: 0.2 s on an idle CPU (2.5 s once with every core busy), 0.03
  s from the cache. The report counts the time spent making pipelines.
  The shader cache and the compile on a worker thread are not built.
- A shader seen for the first time is drawn by a **generic pipeline while the
  translated one compiles on a worker thread**: an interpreter of Xenos
  microcode written as a SPIR-V vertex and pixel shader, reading the program
  from a buffer (the 3DS runtime does the same with its specialized
  pipelines, Dolphin with its ubershaders). It is slow and only draws until
  the translated pipeline is ready. It comes after the translator (step B5),
  if hitches still show.

**Vertex fetch in the shader.** Vertices are read from guest memory (one
buffer, by device address) with the format and endianness of the fetch
constant decoded in the shader, as Xenia does. There is no vertex input
state, so pipelines key on less.

**EDRAM as render targets, one model.**
- A render target is a Vulkan image keyed by its EDRAM range (base tile,
  pitch, format, sample count). A draw binds the images its registers name.
- When a range is used again with another format (color and depth, 32 and
  64 bits per pixel, another sample count), its contents are converted by a
  copy shader: the one general reinterpretation, where aliasing bugs live,
  so it gets tests of its own.
- A resolve writes the image into guest memory as a texture (tiling,
  endianness, format, the optional clear) on the GPU, into the mirror
  ("Guest memory on the GPU" below); the textures over it are loaded from
  there on the GPU.
- A resolution scale applies from the start: images at N times their size,
  resolves keep a scaled copy for sampling, the CPU sees them scaled back.

**Textures decoded on the GPU.** Keyed by the fetch constant (address,
format, size, tiling, endianness, mip address); untiled and decoded by
compute shaders, one per format family, from a table of formats; dropped
when the CPU writes their pages (page guards over guest memory). HD texture
packs hook in here, as in the 3DS runtime. So far (B4) the GPU loads the
textures over what resolves wrote (`texture_load.comp`, the CPU decode's
addressing and byte swaps from the same `texture::Plan`); the rest are
decoded on the CPU and checked by a hash of their memory.

**Submissions batched (B4, 2026-11-05).** What the processor sends is
recorded into command buffers a frame at a time: a submission at each
swap, or sooner when its share of the upload arena fills, three in flight.
Render passes stay open from draw to draw while the targets stay the same;
transfers and computes (EDRAM moves, resolves, texture uploads, clears) end
them, with a full barrier on each side (a few a frame). A draw's constants,
indices and vertex data are copied into its submission's share of a
host-visible arena, the vertex fetch constants pointed at the copies (the
shader reads its vertices where they were copied, bounded by the
constant's size as before); textures are bindless, one descriptor set
written after binding, each image at its own index for its life. The
renderer waits for the GPU only to reuse a submission's slot, or when the
CPU must see what the GPU wrote.

**Guest memory on the GPU: the mirror (B4, 2026-11-05; watched since
2026-11-06).** Xenia keeps all of guest memory on the GPU (its shared
memory) and learns of the CPU's writes from page watches; so does the
renderer now ("Write watches and read-backs" below). Its copy on the GPU
(512 MB, device-local) holds what the GPU writes and what reads of that
need around it.
- A resolve runs on the GPU: the rectangle copied from its render target
  into a scratch buffer (from EDRAM when multisampled, below), then
  `resolve.comp` (resolve.cpp's write, ported with its arithmetic kept
  exact) writes it into the mirror, tiled. The pages it does not write
  whole get guest memory's bytes first, so the rest of them is the CPU's
  (`resolve::destination_ranges`).
- Its pages are then the GPU's until the CPU writes them: textures over
  them are loaded from the mirror, vertex data there is read from the
  mirror.
- Guest memory gets the bytes as Xenia's `readback_resolve` says: never by
  default, as Xenia's default since December 2025 ("Write watches and
  read-backs").

**Front buffers read back without waiting (B4).** A swap asked for
(`frame_wanted`) records a copy of its front buffer (fetch constant 0, as a
texture) into a read-back buffer; the picture goes to `on_frame` when the
GPU is done, at a later call of the processor's or at `drain()`. That is
for screenshots and checks now: the player's window takes the image itself
(2026-11-05). `on_swap` hands a `FrontBuffer` (the texture of fetch
constant 0: its image in the general layout, format, size, the fetch
constant's swizzle, the display's gamma ramp) right after the swap's
submission, on the renderer's thread; the queue's order is the
synchronization (what the hook submits on the device's queue before it
returns comes after the swap's work and before the renderer's next), and
the image is the hook's only while it runs. The player's presenter
converts it into a picture of its own on the GPU there and returns
([runtime.md](runtime.md), "The player").

**Memory exports (memexport, 2026-11-05)**, as Xenia does them
(`spirv_shader_translator_memexport.cc` and, where it differs, the more
mature D3D12 translator's `dxbc_shader_translator_memexport.cc`;
`draw_util.cc`'s `AddMemExportRanges`; both backends' `IssueDraw`, which
agree on the shared memory):
- The translator keeps eA and eM0-eM4 in variables and ends an export at
  an alloc, at a kill (before the pixel becomes a helper, whose stores are
  dropped) and at the shader's end; which eM# may be pending where comes
  from Xenia's walk of the control flow (`Shader::AnalyzeUcode`). An
  export checks eA's upper bits as the sequencer does, packs each eM#
  written since the last one into the stream's format (red and blue
  swapped, signed or not, normalized or not, NaN as 0, integers clamped to
  their range and signed components of 1 or 2 bits not scaled as the D3D12
  translator does, Direct3D's rounding, the extended-range float16), swaps
  it by the stream's 128-bit
  endianness and stores the elements within the stream's count into guest
  memory, the storage buffer the vertex fetches read (bytes and halves by
  atomic AND and OR on their word, as Xenia's).
- The renderer binds the mirror for a draw that exports, finds its
  streams from the stream constants, and puts guest memory's bytes under
  their 4 KB pages into the mirror first (Xenia's `RequestRange`). After
  the draw the pages are the GPU's (Xenia's `RangeWrittenByGpu`): textures
  over them are loaded again, from the mirror, and vertex data there is
  read from the mirror. Guest memory never gets the exported bytes, as with
  Xenia's default (`readback_memexport` off). A page stays the GPU's until
  the CPU writes it, which the page watches tell (the processor's own writes
  and a trace's memory are told to them). A trace begins with the GPU's pages as memory
  read, read back from the mirror, as Xenia's trace downloads what its GPU
  wrote.
- Why not copy them back as resolves are: the processor writes a fence
  when it reads the packet, before the GPU has run the export, so the CPU
  goes on and may write the stream's memory again before a late copy lands
  over it. A first form did copy the changed bytes back (as
  `readback_memexport` would, without its wait); in its run El Chavo
  Kart's race loading screen lasted about 3,000 swaps (two and a half
  minutes), where the runs before and after it took 200 to 300.
- A draw that rasterizes nothing (it writes no target, or the vertex
  shader only exports: `vs_export_mode` multipass; or no target, viewport
  or scissor is left) still runs its vertex shader for the exports, alone:
  a point list with rasterization discarded, in a render pass without
  attachments, each index once (Xenia draws it with its pipeline as it
  is). A pixel shader's exports in such a draw are counted, not done.
- Where it differs from Xenia: Xenia draws point lists as triangle strips
  and lets one host vertex in four export; here a vertex shader runs once
  per vertex (points through a geometry shader, or a point list alone). No
  resolution scale yet, so no centre-pixel rule for pixel shaders' exports.
- Checked: spirv-val on exporting vertex and pixel shaders (straight, at a
  kill, through the dispatcher); on the GPU, the harness's exports in each
  of the 15 kinds of format against the packing written out in the test
  (360 combinations of format, number format, endianness and red-blue
  swap, the stream's count stopping the last element).

**The rest, as games ask:** rectangle and quad lists and point sprites
(expanded without geometry shaders), occlusion queries, tessellation. Each
is counted until the corpus or a game needs it.

## The translated shader (B2)

One body, three wrappers: the vertex shader, the pixel shader, and a compute
shader that only the checks use (registers and constants read from buffers,
exports written to one, an invocation per case).

- **Interface** (Vulkan 1.3). Set 0: the stage's 256 float constants (a
  uniform buffer), the bool and loop constants, the 32 fetch constants (6
  dwords each), guest memory as a storage buffer (vertex fetch reads it
  with the fetch constant's address and endianness, memexport writes it).
  Set 1: the textures bindless (2D, 3D and cube arrays) and the samplers,
  reached through a per-draw table from fetch slot to descriptor. A vertex
  shader writes the position, 16 interpolators (locations 0-15) and the
  point size; a pixel shader up to 4 colors, and depth when it exports it.
- **State**: the registers as a Function array of vec4 sized by
  `SQ_PROGRAM_CNTL` (128 at most), relative addressing through `a0` and
  `aL`, the predicate.
- **Control flow**: straight-line code when the program is only execs,
  allocs and conditional execs (on a bool constant or the predicate), which
  is most shaders; otherwise a loop over a switch on the next control flow
  instruction, as Xenia does, for loops, calls and jumps.
- **Semantics kept as the hardware's** (Xenia's
  `spirv_shader_translator_alu.cc` is the reference): multiplies without
  contraction and with 0 times anything giving 0 (`mul`, `mad`, `dp*`);
  `rcp`, `rsq`, `log`, `exp` reduced to 21 bits of precision, like the
  hardware's approximations; the clamps of `rcpc`, `rcpf`, `logc`; `maxa`
  setting `a0 = clamp(floor(w + 0.5), -256, 255)`; kills as demotion to a
  helper invocation.
- **Checked three ways**: `spirv-val` on every shader of the corpus; the
  compute wrapper on the GPU (`vk_compute.h`) against a CPU interpreter of
  the same instructions, on chosen and edge inputs (0, -0, infinities,
  NaN, denormals); and whole frames against Xenia's pictures in B3.

In place (2026-11-04): the SPIR-V writer (`spirv.h`); the translator,
version 1 (`shader_translator.h`): every vector and scalar operation, the 15
vertex formats with their endianness and normalization, 2D and 3D texture
fetches, execs plain, on a bool constant or on the predicate; the CPU
interpreter (`shader_interpreter.h`), translated from Xenia's with the GPU
translator's semantics where the two differ; the windowless compute runner
(`vk_compute.h`). `shader_compare_test` runs both on the GPU and the CPU,
case by case, and they agree, the traces' shaders included (`XF_SHADERS`).

Version 2 adds the dispatcher (loops, calls, jumps), cube maps,
getGradients and the LOD and gradient setters; the corpus is the traces'
shaders, the UE3 global shader cache of El Chavo Kart's data and HLSL
compiled by Microsoft's Xbox 360 compiler (`tests/hlsl/`, `tools/xna/`),
each with the literal constants its container defines.

The hardware has no denormals, not even between a multiply and the add of a
`mad`: the interpreter flushes them there, and the GPU's flush-to-zero does
the same (B3 asks for it with the float controls where the device has
them).

## The renderer, first form (B3)

`renderer.h`, `xfreplay` (2026-11-04). The renderer is the command
processor's Sink (`command_processor.h`, 2026-11-05): one PM4 front end, the
processor's, runs the packets and keeps the registers, constants and PM4
predication; the renderer takes the shaders it loads, its draws, resolves
and swaps with the register file as it stands, and reads guest memory
where it lies (the processor's physical memory). A trace is replayed
through a processor of its own as Xenia's trace player feeds its command
processor (`replay.cpp`: each packet written at its address and executed
there, the memory it read written first, recorded memory writes skipped,
the registers and gamma ramps restored). At first every draw was one
Vulkan submission, waited for; since B4 a frame's work is one submission,
waited for only when the CPU needs what the GPU wrote (Decisions).

- **Draws**: the translated vertex and pixel shaders (colors scaled by the
  target's exponent bias, 16-bit fixed targets kept as snorm with their
  -32..32, gamma targets written through the piecewise linear curve);
  the vertex fetches reading the copies of their ranges in the upload
  arena; indices swapped, offset, cut to 24 bits and clamped on the CPU
  (quad lists split, strips restarted at the reset index); rectangle lists
  and point lists through geometry shaders, or expanded in the vertex
  shader without them (points as sprites of the vertex shader's clamped
  oPts.x or PA_SU_POINT_SIZE, as Xenia's); tessellated draws through hull
  and domain shaders ("Tessellation" below); viewport, scissor and
  the position's formats as Xenia's `GetHostViewportInfo` and
  `GetScissor`; blend, depth, stencil, culling and the alpha test from the
  registers. A draw that can write nothing (no color channel enabled,
  depth and stencil off, no memexport) is dropped: occlusion queries are
  the processor's fake counts, so nothing could see it. One that only
  exports memory runs its vertex shader alone ("Memory exports" above).
- **Render targets** stand for EDRAM as host images, one per base, pitch,
  MSAA and format, as many rows as EDRAM holds from the base. EDRAM itself
  is a 10 MB buffer in Xenia's layout (`edram.glsl`, from `edram.xesli`):
  when a target needs tiles another holds (down to the scissor's bottom),
  the holder stores them into EDRAM in the guest's bits and the new one
  loads them, in any format and MSAA, as Xenia's ownership transfers do;
  depth tiles have their halves swapped; the stencil is exported, or
  written a bit per pass where the device cannot export it (NVIDIA). The
  bits per sample are the guest format's (2_10_10_10_FLOAT is 32 in EDRAM
  though kept as 16-bit floats). D24FS8's range, 0 to 2, is kept halved
  (viewport depth range, oDepth, clears, loads and stores), as Xenia's
  host render targets keep it: whatever moves through a depth target
  (color bits too) survives the host's 0-1 depth whole. The host's 32-bit
  depth is kept beside EDRAM across transfers, as Xenia keeps it ("The
  host's depth kept across transfers" below). The trace's EDRAM
  snapshot is where EDRAM starts (zeros, live).
- **Textures** decoded from guest memory (`texture.h`: 1D, 2D, stacked, 3D,
  cube, tiled or linear, packed mips, the block formats kept compressed;
  depth formats as 32-bit floats, the EXPAND formats as 16-bit floats, as
  Xenia's texture cache loads them; the host formats as "Texture formats"
  below), with the fetch constant's swizzle and
  sampler; each is kept with a hash of the memory it was decoded from (its
  slices' spans: a tiled slice's the 4 KB pages its tiles lie in, found
  without visiting the texels) and decoded again when that memory differs
  at a use, or at once when the processor or a resolve writes over it
  (`memory_written`).
  1D, 2D and stacked textures are 2D arrays, as Xenia binds them; a
  3D-or-stacked fetch chooses at run time by the fetch constant's
  dimension, a stacked one its layer (two blended when the volume filter
  is linear). Every fetch's coordinates carry its offsets plus Xenia's
  rounding epsilon (1.5/1024 of a texel), and unnormalized coordinates are
  divided by the size from the fetch constant, as Xenia's
  `ProcessTextureFetchInstruction` does.
- **Vertex data**: at each draw, the ranges of the vertex fetch constants
  the shaders use (address and size, as Xenia's IssueDraw requests them)
  are copied from guest memory into the upload arena.
- **Resolves** write the rectangle (MSAA averaged) as the destination says
  (`resolve.h`), on the GPU since B4 (into the mirror, then guest memory),
  then clear.
- **The swap** shows texture fetch constant 0 through the processor's gamma
  table, as Xenia's presenter does, for the swaps asked for (numbered from 0
  as Xenia numbers a trace's frame), read back without waiting.

Nothing tells the renderer when the CPU writes guest memory (Xenia watches
pages with memory protection), so what it copies is checked or copied
again at each use: textures by their hash, vertex data and indices copied
at each draw.

Against Xenia's trace dumps (`replay_test` with `XF_TRACES` and
`XF_DUMPS`): all eleven frames of the two games within 0.005 of 255 on
average (Ben 10's within 0.001 since the rounding epsilon), no pixel off by
more than 16: El Chavo Kart's video (three planes) and its title screen,
Ben 10's intro, and Ben 10's title, drawn in two predicated tiles of a 2x
MSAA target that the game clears by drawing into the same samples seen as a
640 4x target.

Not done yet, counted where it shows: MSAA resolves of one sample; depth
resolves of MSAA targets;
point sprite coordinates in the pixel parameters; signed components of
formats with no signed version (as in Xenia: "Texture formats" below);
float16 and float32 targets moved through EDRAM by value rather than by
bits (NaN payloads). Signed, biased, gamma and exponent-adjusted textures,
the pixel parameters, the polygon offset, the samplers' LOD bias, mip
range, anisotropy and border, render targets keyed by storage format,
getCompTexLOD and getWeights, alpha to coverage, and the display's
piecewise linear gamma came with the traces of live runs ("Traces of live
runs" below).

## Driven live (2026-11-05)

`boot --render` installs the renderer as the live command processor's Sink
(`gpu_command_processor`), on a Vulkan device without a window; its calls
come on the "GPU commands" kernel thread. `--shots <dir>` writes chosen
swaps' front buffers as PNGs, numbered as Xenia numbers a trace's frame,
and `xfcompare` finds the shot closest to a Xenia dump.

The traces replay through the same path (a processor fed as Xenia's trace
player feeds its own): the ten frames are byte for byte what the renderer
drew with its own front end, and match Xenia's as before.

Live, against Xenia's dumps of the traces' frames (the games' intros with
no input):

- **El Chavo Kart**: all four frames are drawn exactly (mean 0.003 to
  0.005 of 255, no pixel off by more than 16), at other swaps: Xenia's 300
  and 301 (the Unreal Engine logo) by swaps 289 and 290 of one run (286 for
  301 in another), its 900 (the intro movie's beach) by swap 604, its 1800
  (the movie's karts) by swap 1082. The videos run on the clock, and the
  interpreted game swaps about 7 to 10 times a second where Xenia swaps up
  to 60, so a movie frame comes at a lower swap, and not at the same one
  from run to run; boot's run reaches the title screen by swap 1800.
- **Ben 10**: XAM reports no controller, where Xenia's default input (its
  keyboard driver) answers for user 0; the game pauses on "Controller Lost
  Connection" from about swap 100, which the renderer draws, so its frames
  are not Xenia's. With user 0 answered as Xenia's keyboard driver answers
  (an experiment, not kept), the intro plays (the Cartoon Network logo) and
  then stops swapping before Xenia's frame 600, headless as well as
  rendered (at 506 and 345 swaps): what it waits for next is not in the
  kernel yet (sound, most likely). Ben 10's six traces replay through the
  same Sink and match. (Later the same day boot answered from an idle pad
  for user 0, [input.md](input.md), and with the generated code the game
  plays on past the Cartoon Network logo to Papaya's, past swap 700.)

El Chavo Kart's title screen ("Press START" over clouds, from about swap
1800 live, Xenia's frame 2400 and later) came out in red alone. Its trace
(`475807D7_2400`, in the regression) showed why: the menu's frame moves
its 8_8_8_8 target's tiles through a D24FS8 depth target and back (its
depth-only draws take EDRAM whole), and the depth, kept as 0-2 in the
host's 0-1 depth, was clamped to 1: the 24 bits above the stencil byte
(green, blue, alpha, an opaque alpha making them 1 or more) came back as
1.0's 0xF00000, green and blue zero; red, the stencil byte, survived.
Kept halved,
as Xenia's host render targets keep it (`full_float24_in_0_to_1`), the
title matches Xenia's trace dump with a mean difference of 0.000 and live
is identical to it but for the pulsing "Press START" (whose phase differs;
0.852 over the whole picture, 0 elsewhere). Not the mechanism of
rexglue's Vulkan red tint on the character select (a stencil of 255 in
the red byte), though the same family: color moved through depth.

The title's frame also issues 240 point lists that write nothing (color
mask 0, depth and stencil off), now dropped; its UE3 post-process reads a
16x16x16 color grading table as a stacked texture, a depth texture
(24_8_FLOAT), EXPAND textures and a 2_10_10_10_FLOAT target, all now as
Xenia does them, though the menu covers what they draw (the table, shown
in place of the front buffer in a probe of the trace, matches Xenia's
exactly). A 600 s live run (6,398 swaps, the title from swap 2100 on)
issued 1,258,016 draws, 198,176 of them drawn and the rest writing
nothing, and skipped none. What the live runs still ask for and the
renderer does not do: D24FS8 depth kept as 32-bit float (Xenia's default
too), Ben 10's texture slots without a texture constant. On the title the
renderer is the bottleneck: 590 s of the 600 busy, 442 s of them in its
fifteen resolves a frame, read back and tiled by the CPU (B4). Its "Press
START" pulse shows one phase at every hundredth swap from 2100 to 6300,
not the phase of Xenia's frame 2400 nor that of its 3000 and 3600: the
pulse is the game's (its clock or its frame count), not the renderer's.

The cost, before B4 (runs of the same length back to back, other work
sharing the machine): El Chavo Kart is bound by the interpreter, and rendering on the
processor's thread costs it nothing measurable (747 swaps in 90 s headless,
742 rendered; the renderer busy 24 s of the 90). Ben 10 is bound by the
renderer: 59.4 swaps a second headless (vblank's pace), 18.4 rendered, the
renderer busy 39 s of the 45 after the first swap, 30 s of them in its
three resolves a frame (12 ms each) and most of the rest in its 72 draws a
frame. Every draw is a submission waited for and every resolve is read
back and tiled by the CPU, as planned for traces; batching a frame's draws
into one submission and resolving on the GPU (B4) are what would take this
away.

## Speed (B4, 2026-11-05)

Batched submissions, resolves and the textures over them on the GPU, and
front buffers read back without waiting ("Decisions"). Measured with boot's
report on runs of the same length, headless and rendered back to back, the
generated code linked, an RTX 5060, other agents' builds sharing the
machine (60 to 100% load); swaps a second over the whole run and over its
last 5 s:

| | before (648464d) | after |
| --- | --- | --- |
| Ben 10, 45 s headless | 60.0, 60.0 | 40.9, 60.0 |
| Ben 10, 45 s rendered | 14.7, 19.4 | 43.2, 60.0 |
| renderer busy | 41.3 s (25.9 resolves, 19.6 submissions) | 2.3 s |
| El Chavo Kart, 150 s headless | 22.3, 30.0 (3,353 swaps) | 29.9, 29.9 (4,590) |
| El Chavo Kart, 150 s rendered | 17.0, 8.0 on the title (2,566) | 30.0, 30.0 on the title (4,504) |
| renderer busy | 146.6 s (90.1 resolves, 37.1 submissions, 25.3 decoding textures) | 11.5 s |

(Ben 10's whole-run rate fell headless as well: since the player's merge
boot answers an idle pad, and the game plays past its logo into scenes
that pace themselves; both runs end at 60.) After, El Chavo Kart's 11.5 s
are 3.9 s copying vertex data (103 GB: the fetch constants' whole ranges,
about 1 MB a draw), 1.8 s recording resolves, 1.3 s checking textures, 1.1
s decoding them (the intro movie's three planes a frame, written by the
CPU), 1.1 s copying what resolves wrote into guest memory (16 GB), 0.4 s
submitting (0.1 s of it waiting for the GPU); 4,508 submissions and 4 waits
in the run. Ben 10's 2.3 s: 0.7 s in resolves, 0.5 s checking textures,
0.4 s decoding, 0.4 s copying 6.9 GB back; 2 waits. On the title every
texture reloaded in a frame (about 17) is a resolve's, loaded on the GPU.

Live against Xenia's dumps (shots of every swap around each frame): El
Chavo Kart's 300 at swap 290 (0.005 of 255, no pixel off by 16), its 900
at swap 818 (0.003), 1800 at 1615 (0.004), its title (2400) at swap 2910
(0.000, the "Press START" pulse in the same phase); its 301 lies between
two of our swaps (the logo's fade runs on the clock, and Xenia swaps twice
as often there: our 290 differs from Xenia's 301 by what Xenia's 300 does,
5.08). Ben 10, now past its logo: 600 and 601 at swaps 574 and 575 (0.000),
1800 and 1801 at 1481 and 1482 (0.154 and 0.006); its 3600 (the title's
glow in another phase) was not reached within 1.55. The eleven trace
frames match as before. Vulkan's validation layer, on the trace
regression and on a live run of each game, reports nothing.

Left: the vertex data (copied whole at each draw, with nothing to tell the
renderer which pages changed: page watches, or ranges trimmed to the
indices a draw uses), the CPU's texture decodes and checks (all textures
loaded on the GPU from the mirror, as Xenia's, once the mirror can be kept
current), the copy back of every resolve (lazy with page guards), and
resolution scale and the pipeline cache (B4's other half).

## GPU time (2026-11-06)

El Chavo Kart and Ben 10 kept an RTX 5060 at 100% in their 3D scenes and
lost frames there. Measured, not assumed: `Options::gpu_timing` writes a
timestamp wherever the kind of work being recorded changes (draws by render
pass, EDRAM loads and stores by target, resolves, read-backs, texture loads,
the swap's read-back) and sums the intervals by kind (`Report::gpu_time`;
`xfreplay --gpu-time`, `--gpu-time-draws` for each draw by its shaders,
`XF_GPU_TIME=1` or `=draws` for the games' runs). `xfreplay --repeat N` plays
a trace N times through one renderer, as a game draws a frame again, and
reports the fastest play after the first: the GPU is shared with other
agents' runs and its clocks ramp up, so a single play's time is noise.
The machine's GPU sits on a PCIe 3.0 x8 link (6.5 GB/s measured), which
makes everything that crosses the bus expensive.

Four causes, in the order found, each fixed as Xenia does it:

- **Every pixel shader kept its registers in memory.** The pixel parameters
  were stored into the register array at an index from the system
  constants, a variable index, and the driver's compiler then keeps the
  whole array (128 registers) in memory for every pixel shader, even those
  that never take the branch. Stored by constant indices now (a select per
  interpolator): El Chavo Kart's FXAA pass went from 5.5 ms to 0.12 ms at
  1280x720. (Sizing the array to the registers the shader names, as
  Xenia's register_static_address_bound, changed nothing measurable once
  the indices were constant; not kept.)
- **Vertex data was read from host memory.** Each draw's vertex fetch
  ranges were copied into the upload arena (host memory) and the vertex
  shaders read them there, across the bus: 6.7 ms of the race's 3D scene.
  They are read from the mirror in device memory now, as Xenia's shaders
  read its shared memory; nothing here watches the CPU's writes, so each
  draw compares its ranges page by page with a shadow of what the mirror was
  last given and uploads only the pages that differ (a resolve's write
  makes its pages unknown). The race's 150 MB a frame of copies became
  4 MB of uploads; the CPU compares instead of copying.
- **A draw without a scissor took over all of EDRAM.** The rows a draw takes
  from other targets were the scissor's bottom, and Direct3D 9 leaves the
  scissor at 8192 for clears and screen-space rectangles: a 1280-wide depth
  target took all 2048 tiles three times a frame, every target there
  stored and loaded back. Now Xenia's draw_extent_estimator: the viewport's
  bottom with clipping on, and with clipping off and no scissor the vertex
  shader run on the CPU (shader_interpreter.h) over the draw's vertices
  (up to 1024 of them; Xenia has no limit).
- **Resolves read back what later resolves overwrote.** Every resolve's
  bytes were copied to host memory for guest memory at once; the games
  resolve to the same memory many times a frame (El Chavo Kart's
  post-processing chain goes through one texture's memory). The read-backs
  are recorded at the submission's end now, for what no later resolve wrote
  over (26 MB a race frame became 18 MB).

GPU time a frame, before (master's renderer with the timing added) and after,
`xfreplay --gpu-time --repeat 10`, the GPU otherwise idle:

| trace | before | after |
| --- | --- | --- |
| El Chavo Kart, race (`xf_race`) | 55.5 ms | 8.9 ms |
| park behind "Press START" (`xf_park`) | 44.7 ms | 7.5 ms |
| Cups menu (`xf_menu`) | 56.3 ms | 9.4 ms |
| loading screen (`xf_loading`) | 6.5 ms | 1.3 ms |
| title (2400) | 35.0 ms | 7.5 ms |
| intro movie (1800) | 6.5 ms | 1.5 ms |
| Ben 10, 600 / 1800 / 3600 | 21.2 / 43.1 / 43.1 ms | 2.7 / 3.0 / 3.0 ms |

The race after: read-backs 2.8 ms (18 MB over the bus), the 3D scene's draws
1.0 ms, texture loads from the mirror 0.7 ms, resolves' copies 0.7 ms, the
front buffer's read-back 0.5 ms (xfreplay's; the player's presenter takes
the image on the GPU), EDRAM loads 1.0 ms; before, the 3D scene alone was
20.9 ms and the post-processing passes 6.3, 4.9 and 3.2 ms. The 17 frames
of the regression match Xenia as before.

Live (`boot_<game> --render --rates`, `XF_GPU_TIME=1`, the before runs on a
build of master's renderer with the timing added, nothing else on the GPU;
GPU busy is `nvidia-smi`'s utilization, averaged over the phase):

| | before | after |
| --- | --- | --- |
| Ben 10, its 3D scene (70-150 s) | 14.2 swaps/s, GPU busy 98.9% | 59.5 swaps/s (vblank), 58.5% |
| Ben 10, 150 s: GPU time, renderer waiting for it | 146.8 s, 110.6 s | 29.7 s, 0.0 s |
| El Chavo Kart, title (swaps 2100-3350) | 28.3 swaps/s, 97.6% | 29.4 swaps/s, 90.6% |
| menus and the park (4000-6800) | 15.6 swaps/s, 94.6% | 23.9 swaps/s, 88.9% |
| the race (7700 on) | 5.9 swaps/s, 88.9% | 15.1 swaps/s, 80.9% |
| 500 s: GPU time, renderer waiting for it | 392.3 s, 245.6 s | 166.8 s, 0.1 s |

The renderer no longer waits for the GPU; El Chavo Kart's race is bound by
the renderer's thread now (68.8 s checking textures by hash, 72.5 s
comparing vertex data, of 342 s busy). `nvidia-smi` keeps reporting a busy
GPU where the timestamps add up to far less: what it counts is mostly the
read-backs, which live take 38% (El Chavo Kart) and 65% (Ben 10) of the
GPU's time and cross the bus at about 1.2 GB/s, five times slower than in
a trace's replay (6.5 GB/s, the link's speed), for reasons not found yet
(the link stays at PCIe 3.0; the read-back memory being cached or not made
no difference in a replay).

Left: the read-backs, now the largest share (at PCIe 3.0 speed, slower still
when the link drops to a lower speed at low load): on a transfer queue they
would overlap the next frame's work, and read back only when the CPU touches
the memory (page guards) they would mostly go; the indices, still converted
on the CPU into host memory; the stencil's bit-by-bit EDRAM loads without
stencil export (as Xenia's, nine passes), which could be skipped where the
tiles' stencil is known to be zero. (The read-backs and the checks went on
the same day: "Write watches and read-backs" below.)

## Write watches and read-backs (2026-11-06)

**Write watches** (memory.h, memory.cpp; Xenia's memory.cc physical memory
access callbacks, `EnableAccessCallbacks`, `TriggerCallbacks`, the
invalidations in `PhysicalHeap`'s Release, Decommit and Protect, and
`exception_handler_win.cc` / `exception_handler_posix.cc`). Physical memory
is cut into watch pages of the host's page size (4 KB here; 16 KB on some
ARM64 hosts, never assumed). Each page has the number of the last write
seen to it, out of a count that only grows. Watching a page protects it
read-only in every window the guest may write it through (0x7F000000,
0xA0000000, 0xC0000000, 0xE0000000; never the physical view
`translate_physical` gives). The first write to it, by the game's
generated code, the interpreter or the kernel's host code, faults once
into a vectored handler on Windows or a SIGSEGV/SIGBUS handler on POSIX
(only writes fault on a read-only page, so no decoding of the access, which
Xenia does from x86's page fault code or ARM64's ESR), which numbers the
page, takes the protection off and lets the write go on. As Xenia's shared
memory does, the fault widens to the 64 pages around (256 KB) short of
pages the GPU holds (the renderer's watcher bounds), at most 4 MB. Writes
the watches cannot see are told to them (`physical_written`): the command
processor's own (the renderer's `memory_written`), a trace's memory, the
XMA decoder's output, and file reads, which now go through the physical
view as Xenia's `XFile::Read` does (the host's `ReadFile` fails on a
protected page instead of faulting). The heaps' protect to writable,
alloc, decommit and release number their pages first, so a change of
protection never loses a watch. A consumer watches a page, then takes the
count, then copies the bytes: a page numbered above its count was written
since.

Windows' `GetWriteWatch` was measured as the other way: it fails on our
guest memory (error 87: it only works on private `MEM_WRITE_WATCH`
memory, and the guest's windows are views of one section). On private
memory, 64 MB written a byte a page took 14-34 ms and the query and reset
2.1 ms, where protection and the handler cost 7-10 us a page (113-166 ms
for the same 16,384 pages, before widening): faster per write, but not
usable with aliased views.

**The renderer on the watches.** The mirror keeps, by watch page, the
count its copy is as of; a page is current while nothing numbered above it
came (Xenia's valid bits). `request` (Xenia's `RequestRange`) uploads the
pages of a draw's vertex data or a texture load that are not current,
watched first; a resolve or a memory export (`gpu_written`, Xenia's
`RangeWrittenByGpu`) makes its pages current and the GPU's, watched, until
the CPU writes them. Textures keep the count they were loaded at, and are
loaded again when a page of theirs is numbered above it (one read of the
count when nothing was written anywhere since). This replaces the hashes
of textures at each use, the page-by-page comparison of vertex data with a
512 MB shadow of what the mirror was given (gone), and the exported pages'
hashes.

**Resolve read-backs as Xenia's `readback_resolve`** (command_processor.cc;
`IssueCopy_ReadbackResolvePath` in d3d12_command_processor.cc and the
Vulkan one; `Options::readback_resolve`, `XF_READBACK_RESOLVE`,
`xfreplay --readback-resolve`). Xenia's default is `none` since December
2025 (`UPDATE_from_string(readback_resolve, 2025, 12, 4, 21, "fast")`: the
old default was fast): guest memory never gets a resolve's bytes, the GPU's
copy has them and every read of them on the GPU finds them there. `fast`:
each resolve copies its bytes into one of a pair of read-back buffers kept
by its address and length, and guest memory gets the other, which the same
resolve filled the frame before (the first time, this one's, waited for);
the pairs trade places at each swap, and above 64 pairs those unused for
60 frames go. `full`: each resolve waits for the GPU and copies its bytes
into guest memory at once. Where it differs from Xenia: only the ranges a
resolve writes are copied, not its extent from first byte to last (the
tiles between); fast waits for the other buffer's submission where Xenia
reads it without waiting (a frame old, it is done).

**MSAA resolves from EDRAM**, as Xenia's resolve shaders do them
(resolve.xesli's `XeResolveLoad*Colors`, draw_util.cc's
`SanitizeCopySampleSelect`): a multisampled source's rectangle is stored
into the EDRAM buffer by its target (which keeps its tiles), and
`resolve.comp` reads the samples RB_COPY_CONTROL selects there, as the
guest keeps them: one sample (copied raw where the formats allow, as
Xenia's fast shaders), or two or four summed in Xenia's order and averaged,
8_8_8_8_GAMMA's colors as linear codes taken through the curve again, the
exponent bias with the sample weight; depth one sample. This replaces the
host's average (`vkCmdResolveImage`, approximate for a single sample) and
the skipped MSAA depth resolves.

Measured, the race trace (`xf_race`, `xfreplay --gpu-time --repeat 10`):

| | before | after |
| --- | --- | --- |
| GPU time a frame | 9.05 ms (read-backs 2.81 ms) | 5.95 ms (none) |
| renderer, 10 plays: checking textures, vertex data | 0.332 s, 0.084 s | 0.001 s, 0.009 s |
| `fast`, `full` | | 10.0 ms, 9.9 ms a frame |

The picture is byte for byte the same; the 17 frames of `replay_test` match
as before, Ben 10's 3600 and 3601 and Sonic's title closer (0.001 to
0.000, 0.007 to 0.000: their MSAA resolves).

Live, El Chavo Kart (`boot_chaves --render --rates`, 600 s, the press
script of "El Chavo Kart past the title" with the race's presses at
10000-10750, `XF_GPU_TIME=1`, other agents' builds sharing the machine):

| | before | after |
| --- | --- | --- |
| the race (swap 11000 on) | 15.1 swaps/s | 24.8 swaps/s (27.6 from swap 12000; the cap is 30) |
| title (2100-3350), menus and park (4000-6800) | 30.0, 25.4 | 28.1, 26.9 |
| renderer busy, of 600 s | 430.9 s (113.9 checking textures, 42.9 vertex data, 29.1 copying 166 GB back) | 87.3 s (0.9, 3.4, none) |
| GPU time | 104.5 s (38.6 s read-backs) | 57.3 s |

On top of the same day's pipelines on worker threads and texture formats
(the same run): the race at 29.8 swaps a second from swap 11000 (reached at
372 s instead of 444), the title at 30.0, the menus and park at 29.5;
renderer busy 173.5 s for 10.1 million draws (6.4 million before), GPU
time 90.3 s. The watches there: 1.43 million pages watched, 55,572 write
faults (about 90 a second, 7-10 us each), 7.3 million pages numbered as
written (most of them the XMA decoder's output and faults' widening). The
title's shot matches Xenia's 2400 exactly (0.000), and the logo's 300 is
in its fade's next phase (the clock's). Ben 10 plays at 49-60 swaps a
second, its watches taking 12,593 faults in 120 s.

Red Dead Redemption's title (`5454082B_850` and `_950`): 10.7-10.9 of 255
off Xenia (15% of pixels) before, 9.21 and 9.29 (13.4% and 13.5%) with its
MSAA resolves (4x MSAA depth resolved to 24_8 textures among them) done. The colored noise over its
ground and buildings is left: they sample 4_4_4_4 textures with gamma
signs that the game makes by resolving 4x MSAA targets drawn as
8_8_8_8_GAMMA and resolved through their 8_8_8_8 view; not found yet.

Left: the POSIX fault handler is not built here (Windows only so far),
untried on Linux and Android; the write count is 32 bits (the race run
numbers about 2,000 writes a second, the XMA decoder's most of them: weeks
before it wraps); and the bindless table's places are never
given back, so a texture constant made again takes a new one: the 600 s
race run filled the 2D table (33,165 loads refused near its end).

## Traces of live runs (2026-11-05)

Xenia's traces stop where a person stops playing it; XenoForge's runs go
further (boot's scripted presses, the player). So the runs write Xenia's
trace format themselves (`gpu_trace_writer.h`, Xenia's TraceWriter
translated), and Xenia's trace dump draws the oracle's picture of the very
commands our run sent: any scene a run reaches has a reference without
playing it in Xenia.

- **What a trace holds**, as Xenia's: from the swap before the frame to its
  own swap, the register file and the gamma ramps, EDRAM as the renderer
  holds it (its targets stored into the EDRAM buffer and read back, the
  layout of Xenia's snapshots), then each packet with its words, the memory
  the processor read for it (constants, shaders, polled words) and what it
  wrote (fences, write-backs), and the memory the renderer read for it
  (vertex data, indices, textures, a resolve's rectangle and the tiles it
  cuts). Snappy-compressed above 1 KB as Xenia's writer does (`snappy.h`
  compresses too now). An indirect buffer's packet is recorded with its
  header and address alone, as Xenia's, so a player runs its packets once.
- **Memory recorded once**: a shadow of what the trace has given, by 4 KB
  page, so a range is recorded again only where it changed (Xenia uploads
  again only what its page watches saw the CPU write). What a resolve wrote
  during the frame is never recorded: the shadow learns it as it reaches
  guest memory, and a replay's resolves make it (a recorded copy would hand
  our resolve's output to Xenia's picture). At a trace's start the renderer
  finishes its work, so what earlier frames' resolves wrote is in guest
  memory and is recorded when read (Xenia downloads its GPU-written ranges).
  What memory exports wrote stays on the GPU, so the trace begins with
  those pages read back from the mirror (`TraceSource::gpu_memory`, Xenia's
  `InitializeTraceCompleteDownloads`); a texture loaded from the mirror is
  recorded, at each use, for the parts guest memory gave it.
- **Frames** are numbered as boot's shots and as Xenia's
  `--trace_gpu_frames`: frame N is what the swap after the first N shows,
  `<title id>_<N>.xtr`. `boot --trace-frames 4500,4800 --trace-dir <dir>`
  (it renders), or `XF_TRACE_FRAMES` and `XF_TRACE_DIR` for anything that
  starts the renderer through `game::start_renderer` (the player).
- **Checked**: `gpu_trace_writer_test` (a processor's packets written and
  read back: the commands in order, the indirect buffer's packet giving way,
  memory recorded once and again where it changed, not where the GPU wrote,
  compressed and not); every trace recorded so far replays in `xfreplay` to
  the live shot of the same swap exactly (mean 0.000), and Xenia's
  `xenia-gpu-vulkan-trace-dump` draws each of them (its Vulkan backend leaves
  EDRAM's snapshot out; `xfreplay --no-edram` does the same, for frames
  that read an earlier frame's EDRAM).

**El Chavo Kart past the title** (traces in `../traces/xf_chaves`, Xenia's
dumps in `../traces/xf_dumps`). Presses by swap reach everything the same
way whatever the run's speed: `--press start@#3400` brings the main menu
and its first-time dialog; A takes it to Challenges, whose loading screen
leads to a 3D scene of the park behind "Press START"; B on the dialog,
then A through Cups (`a@#3800` to `a@#5400` every 200 swaps) reaches a
race's "Normal Race" tip over the same park, and `a@#6900 a@#7050
start@#7200 a@#7350 start@#7500 a+rt@#7650:90` starts the race and drives
it. Against Xenia's dumps of the same traces (mean difference of 255,
share of pixels off by more than 16):

| frame | before | after |
| --- | --- | --- |
| menu (dialog; 3 frames) | 0.000, 0% | 0.000, 0% |
| menu, Cups (4000) | 2.367, 8.04% | 0.000, 0% |
| loading screens (5 frames) | 0.019-0.037, 0.07-0.09% | 0.000, 0% |
| park behind "Press START" (4 frames) | 56.4-131.2, 75-95% | 0.005-0.067, 0.01-0.03% |
| race tip over the track (3 frames) | 64.0-88.5, 85-88% | 0.009-0.136, 0.02-0.13% |
| race (5 frames) | 55.0-68.8, 72-86% | 0.010-0.140, 0.01-0.14% |

Four of them are in the regression now (`../traces/chaves_xf`, Xenia's
dumps as `../traces/dumps/475807D7_xf_*.png`): the Cups menu, a loading
screen, the park and the race, all within 0.017 and 0.03%; with a frame
each of Forza Horizon and Sonic Unleashed (below, `../traces/fh_xf` and
`../traces/sonic_xf`) `replay_test` compares 17 frames. The loading
screens' top half is garbage in Xenia's picture as in ours (the game draws
it so, or both read the same uninitialized memory).

What the scenes asked for and the renderer approximated, counted by the
runs (three minutes of the park: 6,363 gamma texture fetches, 14,590 draws
with pixel parameters, 39,211 with a polygon offset, 1,225 with an exponent
adjust), and what else differed from Xenia without being counted:

- **Gamma textures** (the fetch constant's sign 3): read back to linear
  through the piecewise linear curve (Xenia's PWLGammaToLinear) after the
  sample. The scene's color textures (DXT1 and DXT4_5, gamma on red, green
  and blue; 59 fetches in frame 4500) are lit in linear HDR (a
  2_10_10_10_FLOAT target): read as linear, they came out washed white and
  magenta.
- **Signed and biased components**: the swizzled signs per slot (Xenia's
  SwizzleSigns) in the system constants; signed components from a snorm
  view of the same image (made mutable for it), biased ones as signed
  offset binary by the component's width (Xenia's GetIntegerScaleBits),
  else 2u - 1.
- **The exponent adjust** (fetch constant word 3): the result times its
  power of two.
- **Pixel parameters** (SQ_PROGRAM_CNTL's param_gen): the pixel's
  position, its facing and the kind of primitive in the interpolator
  SQ_CONTEXT_MISC names, as Xenia writes them (VPOS for screen-space
  passes); point sprite coordinates are still 0 there.
- **The LOD**: the fetch constant's bias (word 4) and per-axis gradient
  exponents on every fetch, computed LOD as explicit gradients from coarse
  derivatives (cube maps: the implicit LOD with a bias), as Xenia samples.
- **Samplers** as Xenia's: anisotropy, the constant's lowest mip as the
  lowest LOD, the base map clamped there, white borders, the mirrored
  clamps as mirror clamp to edge.
- **The polygon offset** as Xenia's Vulkan backend sets the depth bias
  (GetPreferredFacePolygonOffset, the float24 and unorm24 factors).
- **Render targets by their storage format** (xenos.h's
  GetStorageColorFormat, as Xenia keys them): 2_10_10_10_FLOAT_AS_16_16_16_16
  is 2_10_10_10_FLOAT, 2_10_10_10_AS_10_10_10_10 is 2_10_10_10. The scene's
  HDR target is used as both at the same base; as two images it moved
  through EDRAM's 7e3 bits between them, clamped to 31.875 and to 7 bits of
  mantissa, and the trees' leaves came out darker (0.4 of 255, 0.8% of the
  pixels, before this; 0.02 and 0.03% after).

The race's kart was drawn black, in Xenia's picture of our traces as in
ours: the runs skipped the draws whose vertex shaders export memory (2,891
in five minutes of menus and park, then bursts in the race). The game
copies data with them (one vertex shader: eight 16_16_16_16 vertex
fetches into two exports of four elements, as point lists that write no
target, 128 points and 8 KB a draw, 12 to 35 draws a burst), into pages
that its textures and the kart then read. With memory exports ("Memory
exports" above) the driver and the kart have their colors live (shots at
swaps 11200 and 11600 of a race), and Xenia's pictures of the traces of
those frames show them too: our replays are 0.009, 0.009 and 0.047 of 255
off Xenia's for frames 11200, 11209 (12 export draws) and 11600, and
0.000 off the live shots. The trace of a race frame first came out dark
(the world unlit, 63-79 of 255 off the live shot): textures loaded from
the mirror stayed pending for good over exported pages and were never
recorded again; now their guest memory parts are checked and recorded at
each use. Press script to the race for these runs: `start@#3400 b@#3600`,
A every 200 swaps from 3800 to 5400, then `a@#10000 a@#10150
start@#10300 a@#10450 start@#10600 a+rt@#10750:150` (the race's loading
takes from a few hundred to thousands of swaps, so the race's presses
come late).

**The other games** (`boot_<game> --render --trace-frames`, no input
unless said; traces in `../traces/xf_<game>`, Xenia's dumps beside the
others in `../traces/xf_dumps`):

- **Forza Horizon** draws everything it sends; its front buffer is
  2_10_10_10, shown through the display's piecewise linear gamma ramp
  (Xenia's apply_gamma_pwl: a component's 10 bits choose an entry by their
  top 7, base plus delta times the low 3 over 8), which was left out:
  frames 600, 1500 and 3000 were 40.2-53.4 of 255 off Xenia (84-99.7% of
  pixels), and are 0.001-0.014 (none off by 16) with it. Counted: depth
  resolves of MSAA targets (7,857 in 150 s), not in those frames (done
  since, from EDRAM).
- **Sonic Unleashed** reaches its title; its title's draws use
  getCompTexLOD (27,146 skipped in 180 s) and then getWeights (46,844),
  now translated as Xenia's (OpImageQueryLod's unclamped level in x, in
  pixel shaders with the computed LOD; the bilinear lerp factors, the LOD
  disregarded as Xenia does). Its frames 300, 450, 600 and 800 match
  Xenia's (0.000 skipping those draws, 0.007-0.008 drawing them). Also
  counted: MSAA depth resolves, texture slots without a texture constant.
- **Red Dead Redemption** reaches its title (a 3D scene behind "Press
  START") at about 7 swaps a second rendered: the renderer is its
  bottleneck, 136 s of 160 busy, 101 s of them checking textures by hash
  (over 500 textures a frame). It asked for alpha to coverage on 665,197
  draws in 150 s (its vegetation drawn as solid quads), now made as Xenia's
  host render target path makes it (oC0's alpha against each sample's
  threshold, dithered by RB_COLORCONTROL's offsets, into gl_SampleMask):
  the title's frames 850 and 950 went from 12.6 of 255 off Xenia (19% of
  pixels) to 10.7-10.9 (15%), the cacti and trees now as Xenia's. What
  remains is colored noise over the buildings, ground and hills: the depth
  losing its host precision through EDRAM before a depth EQUAL pass ("Xenia's
  Direct3D 12 picture and more games" below; the 4_4_4_4 impostor textures
  the game makes on the GPU, suspected first, come out right). Counted
  there too: resolves of one MSAA sample (averaged), MSAA depth resolves
  (both done since, from EDRAM: "Write watches and read-backs"; the
  texture checks by hash gone with them).

## Pipelines, primitive types and occlusion queries (2026-11-06)

**Pipelines made off the renderer's thread** (`Options::async_pipelines`,
on in the player; `xfreplay --async-pipelines`), as Xenia's pipeline
creation threads with its `async_shader_compilation`: a draw whose graphics
pipeline is not made yet queues it, with a copy of its description
(`PipelineDesc`), for worker threads (three quarters of the logical cores,
Xenia's default; `pipeline_threads`), and is skipped meanwhile, counted as
"pipeline not ready (being made)"; the draws after it have it once it is
made. That is what Xenia's D3D12 backend does (its Vulkan one draws with a
placeholder pixel shader instead). Draws without a pixel shader get theirs
at once, as Xenia's do. Replays and `boot` wait for each pipeline, as
Xenia's trace dump does. El Chavo Kart's race frame, cold (no cache on
disk): 1.11 s of the renderer's thread waiting for pipelines, 0.58 s with
them made aside (334 of its draws skipped that frame, 6 the next, none
after). The Vulkan pipeline cache on disk is shared by the workers.

**Fewer pipelines**: Vulkan 1.3's extended dynamic state 1 and 2 (core, so
no fallback) take the topology within its class, primitive restart,
culling, the front face, the depth and stencil tests and ops and the depth
bias's enable at the draw, out of the pipeline key (Xenia's Vulkan backend
keys them: a TODO in its command processor). The race: 102 pipelines, 96
now; the rest differ mostly in blend state and color formats, which only
VK_EXT_extended_dynamic_state3 could take (not used yet).

**Geometry shaders are optional** (many mobile GPUs lack them;
`vk::Device::geometry_shader()`, off with `XF_VK_NO_GEOMETRY_SHADER=1`).

**Rectangle and point lists without geometry shaders** are expanded in the
vertex shader (`shader::HostVertex`, Xenia's `Shader::HostVertexShaderType`
kPointListAsTriangleStrip and kRectangleListAsTriangleStrip): a strip of two
triangles per primitive, cut by the restart index, as Xenia's built-in
index buffer of two-triangle strips. A point's four host vertices are its
index shifted left by 2 plus the corner, and the sprite is made at the end
of the vertex shader as point_list.geom makes it (Xenia's Vulkan
translator, which expands points this way where the device has no geometry
shaders); only corner 0 exports memory. Xenia has no rectangle expansion (a
TODO in its Vulkan translator), so it is done as its HostVertexShaderType
describes it: each host vertex runs the guest shader for the rectangle's
three guest vertices (their indices a vertex attribute), then takes its
corner of the strip rectangle_list.geom makes (the vertex opposite the
longest edge first, the fourth mirrored across it); guest vertex k exports
memory in corner k's invocation only. Xenia's D3D12 backend does not expand
in the vertex shader: its primitive processor is told it draws points,
rectangles and quads natively (`InitializeCommon(true, false, false, true,
true, true)`) and its pipeline cache makes geometry shaders. So geometry
shaders stay the path where the device has them; `XF_VK_EXPAND_IN_VS=1` (or
`Options::expand_in_vertex_shader`) forces the expansion. Measured on the
RTX 5060 with `xfreplay --gpu-time-draws --repeat`: Red Dead Redemption's
frame 850, 82 rectangle draws, 0.884 ms with geometry shaders and 0.905 ms
expanded; 600,000 points of 2 pixels (the synthetic frame below, made
heavier), 0.625 ms and 1.797 ms. All 29 frames of `replay_test` match the
same way with geometry shaders, expanded, and without geometry shaders, with
validation.

**Tessellation** (2026-11-06), as Xenia's D3D12 backend draws it, where the
device has tessellation shaders (`vk::Device::tessellation_shader()`, off
with `XF_VK_NO_TESSELLATION=1`; without them the draws are skipped and
counted, "tessellation without tessellation shaders"). A draw is tessellated
when VGT_OUTPUT_PATH_CNTL's path_select is the tessellator and the major
mode is explicit (Xenia's PrimitiveProcessor::Process: games leave the
register set for later draws in the implicit mode, which do not tessellate;
it used to be checked alone here). Then, as Xenia's primitive processor
takes them:
- triangle lists, fans and strips (as lists, each primitive between reset
  indices on its own, in Xenia's TriangleFanToList and TriangleStripToList
  order) and quad lists, discrete or continuous, are patches of 3 and 4
  control points indexed by control point;
- triangle and quad patches (primitive types 17 and 18 there), in any of
  the three modes, are indexed by patch: discrete and continuous, a patch
  per index; adaptive, the "indices" are 32-bit float edge factors, 3 or 4
  per patch, the patch's index its number plus VGT_INDX_OFFSET;
- the rest is skipped and counted, as Xenia fails it: line patches (a TODO
  in Xenia), adaptive lists, mode 3, other primitives.

The host pipeline is tessellation.vert, a hull shader (tessellation.tesc:
1, 3 or 4 control points, or adaptive triangles or quads) and the guest's
vertex shader translated as the tessellation evaluation shader. What Xenia's
tessellation_indexed.vs and tessellation_adaptive.vs compute is done on the
CPU, where the renderer reads indices anyway: per control point, the index
for the domain shader (swapped, offset, cut to 24 bits, clamped to
VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX) and the edge factor (the guest's plus
1, clamped to VGT_HOS_MIN_TESS_LEVEL and VGT_HOS_MAX_TESS_LEVEL plus 1, NaN
to the lowest), two floats of a vertex attribute; so the factors never pass
as indices, which would need fullDrawIndexUint32. The hull shaders are
Xenia's: the discrete and continuous ones put every level at the largest
factor; the adaptive ones take the Xbox 360's edge order to Direct3D's (and
Vulkan's: the same edges) and the inner levels from the edges. Spacing is
the evaluation shader's in SPIR-V: equal for discrete (Direct3D's integer),
fractional even otherwise, clockwise (Xenia's Vulkan translator: what keeps
Direct3D 12's faces). The domain shader gets r0 and r1 as Xenia's D3D12
translator gives them (StartVertexOrDomainShader): triangles' barycentric
coordinates as zyx in r0.xyz with the control points' indices in r1.xyz,
or the patch's in r1.x and 0 in r1.y; control point indexed quads' UV in
r0.xy, the indices in r0.z and r1.xyz; patch indexed quads' patch in r0.x,
UV in r0.yz, 0 in r1.x; r0.w stays 0, where Xenia's Vulkan translator
writes 1. Tessellated triangles and quads are polygons (draw_util's
IsPrimitivePolygonal: faces, culling, the polygon offset), and their
vertices are not run on the CPU for the rows a draw reaches (Xenia's draw
extent estimator does not either). A tessellated draw whose memory exports
would run without rasterization is skipped and counted.

Every draw's indices are now cut to 24 bits and clamped to
VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX after the offset, as Xenia's vertex
shaders do (RemapAndConvertVertexIndices); the 29 frames are unchanged. The
reset index cuts only Xenia's list (line and triangle strips and fans, line
loops, quad strips, polygons, the 2D line and triangle strips).

**Checked on a synthetic frame** (`primitives_test`): packets and microcode
built by hand from Xenia's ucode.h, 4 by 4 cells of 80 pixels: points of
PA_SU_POINT_SIZE's size, of the vertex shader's, and from 16-bit indices
with the offset and the clamp; a rectangle with its three vertices in each
order; triangle patches in the three modes (two adaptive ones), quad
patches in the three modes, and, by control point, a triangle list, a
triangle strip cut by the reset index, a fan and a quad list, each domain
shader bowing its patch so the tessellation shows and coloring it by its
coordinates and indices; three of them with back faces culled, which shows
the tessellator's winding (culling changes the picture by a mean of 2.1,
and Xenia's changes the same way). Drawn with geometry shaders and expanded, the
frames are the same (worst difference 0). With `XF_SYNTH_TRACES=<folder>`
the frame is written there as a trace (58460001_1.xtr); Xenia's D3D12 trace
dump of it (`--trace_dump_path` the same folder) gives the picture the test
then compares with: mean 0.000, 0.000% of pixels off by more than 16, worst
1 (of 255). Xenia's Vulkan trace dump differs (mean 8.5): it draws the
control point indexed cells with the wrong indices and adds a stray square;
the D3D12 dump is the one matched. Validation is clean with tessellation,
without it, and without geometry shaders.

**Primitive types**: line loops as strips closed by their first index
(Xenia's `LineLoopToStrip`, per primitive between reset indices); quad
strips and polygons, which Xenia does not draw, as triangle strips (whole
quads only) and fans: Mesa's r600 and freedreno's a2xx give these
primitive types to OpenGL's `GL_QUAD_STRIP` and `GL_POLYGON`, so their
meaning is OpenGL's. The reset index cuts them as Xenia's list says. Still
skipped and counted: the triangle list with wireframe flags (7) and the 2D
engine's primitives (16 to 22), which Xenia does not draw and no reference
at hand defines; tessellation is above.

**Occlusion queries** (`gpu::OcclusionQueries`; the player and `boot` use
real counts, `XF_OCCLUSION_QUERIES=fake` the made-up ones;
`xfreplay --occlusion-queries`). The command processor keeps Xenia's
"fast" mode (command_processor.cc, 2026): at each EVENT_WRITE_ZPD the
report of the draws since the last one is written at once with a guess (the
last count at that address, or one sample), and again when the counts are
in, the guesses after it rebased on the right counter. The renderer counts
the draws between two events with host occlusion queries (precise where the
device has them), one per render pass they fall in, read when their
submission is done; draws after a swap count for no report until the next
event, as Xenia's EndZPDFrame; a draw that finds no free query makes its
report at least one sample. The race frame: 315 reports, 235 queries,
validation clean, the picture unchanged. Not done: Xenia's "strict" mode
(the guest waiting on the real count), VIZ queries, the hybrid ZFail and
Total counts.

**Shaders**: unnormalized cube map coordinates are translated as Xenia's
`ProcessTextureFetchInstruction` takes them (the face's xy divided by its
size, the face index as it is, register gradients left as given). The
undefined scalar opcode 41 and an exec past the end of the microcode stay
unsupported: Xenia fails the first (a fatal translation error) and reads
past its copy of the microcode for the second, so only hardware could
answer.

## Texture formats (2026-11-06)

Completed by inventory against Xenia's texture caches
(`d3d12_texture_cache.h`'s table first, `vulkan_texture_cache.cc`'s where
DXGI lacks a format; `texture_info.h`'s `GetBaseFormat`; the load shaders'
conversions in `pixel_formats.xesli`). `texture.cpp`'s table gives each guest
format its host format, the load that makes it, Xenia's host swizzle (the
last component a format has stands for the missing ones: 5_6_5, 10_11_11
and the 4:2:2 formats read RGBB, 6_5_5 RBGG) and its signed version.

- **Drawn since**: `k_6_5_5` (R5G6B5, rearranged, through RBGG); the packed
  4:2:2 pair `k_Cr_Y1_Cb_Y0_REP` and `k_Y1_Cr_Y0_Cb_REP` (decompressed to
  RGBA8: Cr, Y, Cb, as Xenia's Vulkan path for any width); `k_10_11_11` and
  `k_11_11_10`, plain and `_AS_16_16_16_16` (RGBA16 widened by replicating
  bits; their signed version is an image of its own, 16-bit snorm, as
  Xenia's `R11G11B10ToRGBA16SNorm`); `k_DXT3A_AS_1_1_1_1` (B4G4R4A4, a
  texel's four alpha bits its four components); `k_8_8_8_8_GAMMA_EDRAM`
  (as 8_8_8_8, which `GetBaseFormat` makes it).
- **The packed 16-bit formats** are Vulkan's required R5G6B5, A1R5G5B5 and
  B4G4R4A4, the loads moving the guest's red from the low bits, as Xenia's
  Vulkan cache does; 1_5_5_5 no longer goes through 8 bits a component.
- **Signed versions**: a snorm view of the same image where the bits are the
  same; an image of its own where they are not (10_11_11, 11_11_10, and
  16-bit snorm held as floats); none, as in Xenia, for the packed 16-bit
  formats, the DXT colors, DXT3A, CTX1 and DXT3A_AS_1_1_1_1. Xenia binds no
  view there (a null descriptor: zeros, logged as unsupported); XenoForge
  reads them as unsigned, counted. No hardware reference says otherwise, so
  that stays. DXT3A's and CTX1's earlier snorm views of the decompressed
  unsigned bytes are gone; DXN's and DXT5A's BC5/BC4 snorm views stay
  (beyond Xenia, which has none).
- **Not drawn, as in Xenia** (no host format for them; each draw is skipped
  and counted): `k_1`, `k_1_REVERSE`, `k_32_AS_8`, `k_32_AS_8_8`, the MPEG and
  interlaced formats, `k_32_32_32_FLOAT`, the 32-bit integer `k_32`,
  `k_32_32`, `k_32_32_32_32` (they were R32_UINT images sampled through the
  float images the shaders declare, which Vulkan leaves undefined), and
  `k_16_16_EDRAM`, `k_16_16_16_16_EDRAM`, `k_2_10_10_10_FLOAT_EDRAM` ("not
  usable as a texture"). Nor tiled 1D textures, nor 1D ones with packed mips,
  which Xenia's `BindingInfoFromFetchConstant` refuses as "completely wrong".
- **Portable hosts.** The host formats are chosen from the device's format
  properties (`texture::Support`: sampled, filtered, the snorm view), with
  Xenia's Vulkan fallbacks: the DXT formats decompressed (DXT1-5 to RGBA8,
  DXN to RG8, DXT5A to R8) on the GPU, 16-bit unorm and snorm as 16-bit floats
  (`R16UNormToFloat` and kin, rounded the same on the CPU and the GPU), and a
  sampler of a format the host does not filter (the 32-bit floats) points, as
  `GetSamplerParameters` does. Beyond Xenia: the packed 16-bit formats
  unpacked to RGBA8 where missing. The lossy fallbacks are counted
  ("a texture format held with fewer bits"). `XF_TEXTURE_FALLBACKS=1` (or a
  list of `bc`, `norm16`, `packed`, `snorm`, `linear32`) forces them on a
  device that has everything. With all of them, the 17 frames of
  `replay_test` stay within a mean of 0.34 of Xenia's; El Chavo Kart's park
  has 0.108% of its pixels off by more than 16 (alpha-tested foliage: Xenia's
  integer DXT decompression against the GPU's BC decoder, the same difference
  Xenia's fallback has), the rest at most 0.068%. Validation stays clean.
- **On the GPU**: `texture_load.comp` makes every load `decode` makes, for
  textures over memory a resolve wrote (formerly the decompressions waited
  for the resolve's bytes and decoded on the CPU) and for the DXT fallbacks.
  `texture_gpu_test` loads every drawn format (45) in six layouts (tiled 2D
  with mips and the packed tail, linear, 3D, cube, stacked, 1D; odd sizes;
  every byte swap), as the host holds it and through the fallbacks, and the
  separate signed versions: 1,313 levels, each byte for byte the CPU's.
- Gone: "formats sampled at 8 bits per component" and "mip levels shorter
  than their size" (the RGBA8 path that served formats without a host
  format, which also kept only a texture's first layer).
- Open: 1D textures wider than 8,192, which Xenia lays out as rows of a 2D
  texture with the shader converting the coordinates (the translator's).
  XNA's `FillXboxBuffer` gives no known texels for these formats: none of
  the ones added is an XNA surface format.

## Xenia's Direct3D 12 picture and more games (2026-11-06)

**The picture to match is now Xenia's Direct3D 12 trace dump**
(`xenia-gpu-d3d12-trace-dump`, built beside the Vulkan one with its trace
viewer; its default render target path on this NVIDIA GPU is host render
targets, ROV only with `--render_target_path_d3d12=rov`). Its PNGs are in
`../traces/dumps/d3d12` beside the Vulkan ones in `../traces/dumps`, and
`../traces/xf_dumps/d3d12/<game>` beside `xf_dumps/<game>`. `replay_test`
compares each frame with the D3D12 picture, and with the Vulkan one where
there is no D3D12 one; it prints both. Where Xenia's two backends differ
from each other by more than the test's bounds, matching the Vulkan picture
passes too and the line says so (the frames below).

Xenia's two pictures of the same traces (mean of 255, share of pixels off
by more than 16):
- the same (0.000) on Ben 10's six frames, seven of El Chavo Kart's (the
  five of `chaves`, the loading and menu screens of `chaves_xf`), Forza
  Horizon's four, Sonic Unleashed's five and Red Dead Redemption's frame
  600;
- El Chavo Kart's park and race (`chaves_xf`): D3D12 3.144 (6.9%) and 1.707
  (4.4%) off Vulkan, darker in the brightest parts (the blossoming trees,
  lamps and curbs of the HDR scene); ours is 0.017 and 0.010 off Vulkan's,
  so 3.142 and 1.712 off D3D12's. The cause is the host's anisotropic
  filtering (found 2026-11-06, below), not Xenia's logic, and ours stays as
  it is;
- the memory export frames of the race (`xf_chaves/memexport4`): 0.13-0.15
  (0.1-0.3%) between Xenia's two, ours 0.01-0.05 off Vulkan's;
- Red Dead Redemption's frames 850 and 950: 0.21-0.22 (0.1%); ours 0.45
  off D3D12's since the host depth (below).

**New traces from Xenia** (`tools/capture_traces.ps1`, no input, frames
600, 1200, 2400 and 3600; `../traces/xenia_bc2`, `xenia_gow3`,
`xenia_reach`), all twelve in `replay_test`'s corpus, Xenia's two pictures
the same on each:
- Battlefield: Bad Company 2 (454108A8): the title, two frames of the intro
  video, the Frostbite logo; ours 0.000-0.002 off;
- Gears of War 3 (4D5308AB): the start screen behind its "Connection Lost"
  dialog, three frames, and a white one; ours 0.039-0.043 off;
- Halo: Reach (4D53085B): the copyright screen, a dark frame, the intro's
  planet and the main menu over its 3D scene; ours 0.000-0.082 off (the menu
  counts 25 draws with D24FS8 depth as 32-bit float and 7 texture slots
  without a texture constant).

**Red Dead Redemption's noise** (frames 850 and 950 of `xf_rdr`, 10.8 of
255 off Xenia's): it is the depth's precision, not its textures. The scene
is lit in two passes over a 2x MSAA D24FS8 depth buffer: a depth and normal
pre-pass, a light pass, then the material pass (draws 1688-2415 of frame
850 for the top 1280x512 tile, again for the bottom one), which tests depth
EQUAL and writes the 2_10_10_10_FLOAT target over the light buffer's EDRAM.
Between the pre-pass and the material pass a depth-only draw (1687) uses
another depth target over the same EDRAM, so the depth moves out and back
through EDRAM's 20e4 bits; the host's 32-bit depth comes back rounded, the
material pass's fragments no longer equal it, and most samples fail at
random, keeping the light buffer's 8_8_8_8 bits seen as 2_10_10_10_FLOAT:
the colored noise. Checked: with the depth test forced to always the noise
goes (the overdraw comes instead); with draw 1687 skipped the top tile is
clean and the frame goes from 10.80 to 4.10 off D3D12's picture (the bottom
tile, aliased the same way, stays noisy). Xenia keeps the host depth across
such transfers: each EDRAM range remembers the last float24 and unorm24
host depth target that held it (`render_target_cache.h`,
`OwnershipRange::host_depth_render_target_float24`), and a transfer back
takes that target's 32-bit depth where it still rounds to the guest's 24
bits (`kDepthAndHostDepthToDepth`, the "host depth" transfers of
`d3d12_render_target_cache.cc`); Xenia's
`depth_float24_convert_in_pixel_shader` is the other way out (the
fragments' depth rounded to 20e4 in the pixel shader, at the cost of early
depth). Also in that frame, apart from the noise: its 8 MSAA depth
resolves, among them the scene's depth read by the material pass as a
texture, came later the same day from EDRAM (10.8 to 9.2 off).

**The host's depth kept across transfers** (2026-11-06), Xenia's answer
ported to EDRAM transfers that go through the EDRAM buffer rather than
from target to target. Beside EDRAM a second buffer of its layout holds the
host's 32-bit depth (`host_depth_`, 10 MB; twice that where D24S8 is kept
as a float too, its half apart as Xenia tracks unorm24 and float24 apart):
an EDRAM store of a depth target whose format the host encodes differently
(Xenia's `IsHostDepthEncodingDifferent`: D24FS8, and D24S8 on a device
without 24-bit unorm depth) writes its samples' depth there beside their
guest bits, and an EDRAM load into such a target takes that depth where it
still rounds to the 24 bits EDRAM holds (rounded as the store rounds), else
EDRAM's (`edram_load.frag`). The last such target to store a sample is the
last of that format to have held it, so this is Xenia's
`host_depth_render_target_float24` and `_unorm24` without the bookkeeping:
a depth-to-depth transfer of the same format comes back whole (Xenia
copies the 32 bits), one through a color target or the other depth format
takes the old host depth back where the guest's bits were not changed
(`kColorAndHostDepthToDepth`, `kDepthAndHostDepthToDepth`), and the buffer
starts as zeros, which round to EDRAM's 0, so a sample no such target held
loads EDRAM's value as before. Red Dead Redemption's frames 850 and 950
went from 9.20 and 9.28 off D3D12's picture to 0.45 and 0.46 (0.46-0.47%
of pixels off by more than 16; 0.36 and 0.38 off Vulkan's, whose two
pictures are 0.21 apart): the noise is gone, what is left are edges. The
29 frames of `replay_test` are unchanged. `XF_DEPTH_FALLBACK=1` makes
D24S8 a 32-bit float depth, as on devices without 24-bit unorm depth
(AMD), to test that path and its unorm24 half. Xenia's `depth_float24_round` is
an option (`Options::depth_float24_round`, `xfreplay
--depth-float24-round`, `XF_DEPTH_FLOAT24_ROUND=1`), off as there: the
EDRAM stores and the loads' comparison round 20e4 to the nearest even
rather than towards zero. Not done: single-sample depth resolves round as
`resolve.comp` does (towards zero; Xenia's resolves come from its EDRAM
dump and follow the option), and `depth_float24_convert_in_pixel_shader`,
which needs the translator to write every D24FS8 fragment's depth rounded
to 20e4 (sample-rate shading under MSAA, the viewport's depth bounds
rounded too) and then turns the host depth off for D24FS8; the 25 draws of
Reach's menu and the rest keep counting "D24FS8 depth as 32-bit float",
Xenia's default too.

**El Chavo Kart's park and race, D3D12 against Vulkan** (2026-11-06): the
host's anisotropic filtering. Found by patching the trace (a script
rewriting the pixel shader's words in the trace's memory reads) and
dumping it with both of Xenia's backends: the bright blossoming trees
(draws 74-80 of the park, an alpha-tested leaf atlas, DXT5 with gamma RGB,
hue-rotated per tree) differ even with the shader writing the texture's
color alone, and stop differing when that fetch's anisotropy is turned off
in the instruction (3.14 to 1.09); with `--anisotropic_override=0` on both
dump tools Xenia's two pictures are the same, 0.006 (park) and 0.003
(race). Both backends ask the host for the same thing, 4:1 anisotropy and
the same coarse-derivative gradients (sample_d, Grad), except that
Direct3D 12 cannot have anisotropy with point mips
(`D3D12_FILTER_ANISOTROPIC` is trilinear; Xenia's `WriteSampler`), so it
makes the mips linear where the fetch constant says point; Vulkan keeps
the constant's mip filter, as ours does. That is not the gap: with linear
mips in both (the instruction patched) they stay 3.01 apart, and ours with
D3D12's sampler is 2.89 off D3D12's picture. The rest is the drivers'
anisotropic filter itself, the same request filtered differently by
NVIDIA's Direct3D 12 and Vulkan drivers, which alpha testing (a third of
the leaves' alpha) and the hue rotation of the HDR scene magnify. Neither
is the console's: Xenos's anisotropic filter is not documented, only that
its fetch constant has the mip filter and the anisotropy apart (as D3D9's
MIPFILTER and MINFILTER), which Vulkan's set-up, and ours, follow. So
`replay_test` keeps matching Vulkan's picture for these two frames.

## What is asked and not done is counted

For every run and every trace: packets not handled, registers written that
nothing reads, texture formats, shader instructions and features, primitive
types, EDRAM conversions, resolves to formats not done. `boot` and `xftrace`
print the counts; the corpus report sums them over every game, which is how
the renderer decides what comes next.

## How it is checked

- **The trace corpus.** xenia-canary writes traces of chosen frames of any
  game with no input (`--trace_gpu_frames=300,1200`, beside its
  `--trace_gpu_prefix`; F4 records the frame being played). It needs a
  build with `XENIA_ENABLE_GPU_TRACE`: release downloads leave the writer
  out. XenoForge reads Xenia's trace format
  (`trace_protocol.h`: packets, memory reads and writes, EDRAM snapshots,
  registers; a packet runs at its end, after the memory it read, as
  Xenia's player runs it), replays a frame into the renderer without a window, and
  writes a PNG; Xenia's `trace_dump` writes the oracle's PNG of the same
  frame; the two are compared as the 3DS runtime compares with Zakuro (mean
  difference, share of pixels off by more than 16). Traces hold game data:
  they stay on disk, outside the repository, and the comparisons run where
  they are.
- **The shader translator** runs synthetic microcode through the translated
  SPIR-V (a compute harness) and through a CPU interpreter, and compares.
- **Games**: `boot`'s shots at given frames against Xenia's, in the
  deterministic mode so both see the same frames.

Because traces need no recompiled game, the renderer is built and checked in
parallel with the recompiler and the kernel (track B of the plan), on as many
games as the user owns.

## On top: native rendering

The generic renderer draws everything. Ports add, where it pays:

- **The XDK's D3D, by signature.** Both games link D3D as a static library
  (El Chavo Kart XDK 21250, Ben 10 XDK 8276; `xfinspect` prints it). A
  database of the D3D functions' signatures per XDK version names them in
  any game, so hooks on `D3DDevice_*` (render target roles, the back buffer,
  viewport and aspect) come without reverse engineering each game: the way
  to widescreen, a separate UI and resolution independence.
- **The engine** (the skate3 model): a port's renderer receives the device
  and the frame's command buffer and replaces passes (UE3's scene, Scaleform
  UI); the generic renderer keeps everything else. F5 switches between the
  two for comparison.

## Reference files in xenia-canary

`src/xenia/gpu/`: `xenos.h`, `registers.h`, `ucode.h` (the microcode),
`pm4_command_processor_implement.h` (packets), `render_target_cache.h` (EDRAM,
with long notes on its layout), `texture_info_formats.inl`,
`texture_address.h`, `texture_util.h`, `texture_info.h` (`GetBaseFormat`),
`d3d12/d3d12_texture_cache.h` and `vulkan/vulkan_texture_cache.cc` (the host
formats, signed versions and fallbacks), `shaders/pixel_formats.xesli` and
`shaders/texture_load_*` (the loads), `draw_util.h`, `primitive_processor.h`,
`spirv_shader_translator*.cc`, `shader_interpreter.h`, `trace_protocol.h`,
`trace_reader.h`, `trace_dump.h`; `vulkan/` for how Xenia drives Vulkan.
