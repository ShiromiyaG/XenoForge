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
                           textures: keyed by memory, format and size
                             (Xenia's TextureKey), a view per swizzle and
                             sign, bindless; images in pooled memory blocks
                           guest memory: a mirror resolves write, an upload
                             arena per submission
```

The front end runs on its own thread, the game's threads only write the ring
buffer and the registers (as the 3DS runtime's `AsyncGpu`); the player
presents the frames the front end hands over.

## The floor

Vulkan 1.3 with descriptor indexing (bindless textures), buffer device
address, extended dynamic state 1 and 2, 16-bit storage. GPUs since about
2018 on Windows and Linux have all of it. No D3D12, no fallback paths for
older hardware: each of those is a second implementation Xenia carries and
XenoForge does not. Above the floor, found by a query with the Xenia-like
path beside it: fragment shader interlock, the exact EDRAM path and the
default where the device has it ("Exact EDRAM through fragment shader
interlock" below; host render targets elsewhere, as Xenia's default).

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
  `drain()`. Game A's title screen makes about 24 pipelines in its
  first frame: 0.2 s on an idle CPU (2.5 s once with every core busy), 0.03
  s from the cache. The report counts the time spent making pipelines.
  Since 2026-11-07 the pipeline cache lives beside the game library in the
  launcher's cache (`<cache>/<title id>.vkpipelines`, boot's and the
  player's alike), with the title's shader and pipeline storage
  (`<title id>.xfshaders`), and the build step makes what earlier runs met
  ("Shaders and pipelines at the launcher's build step").
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
  `readback_memexport` would, without its wait); in its run game A
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
shaders, the UE3 global shader cache of game A's data and HLSL
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
average (game B's within 0.001 since the rounding epsilon), no pixel off by
more than 16: Game A's video (three planes) and its title screen,
Game B's intro, and game B's title, drawn in two predicated tiles of a 2x
MSAA target that the game clears by drawing into the same samples seen as a
640 4x target.

Not done yet, counted where it shows: MSAA resolves of one sample; depth
resolves of MSAA targets;
point sprite coordinates in the pixel parameters; signed components of
formats with no signed version (as in Xenia: "Texture formats" below);
float16 and float32 targets moved through EDRAM by value rather than by
bits (NaN payloads; by their bits since, "Xenia's GPU TODOs, category A").
Signed, biased, gamma and exponent-adjusted textures,
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

- **game A**: all four frames are drawn exactly (mean 0.003 to
  0.005 of 255, no pixel off by more than 16), at other swaps: Xenia's 300
  and 301 (the Unreal Engine logo) by swaps 289 and 290 of one run (286 for
  301 in another), its 900 (the intro movie's beach) by swap 604, its 1800
  (the movie's karts) by swap 1082. The videos run on the clock, and the
  interpreted game swaps about 7 to 10 times a second where Xenia swaps up
  to 60, so a movie frame comes at a lower swap, and not at the same one
  from run to run; boot's run reaches the title screen by swap 1800.
- **game B**: XAM reports no controller, where Xenia's default input (its
  keyboard driver) answers for user 0; the game pauses on "Controller Lost
  Connection" from about swap 100, which the renderer draws, so its frames
  are not Xenia's. With user 0 answered as Xenia's keyboard driver answers
  (an experiment, not kept), the intro plays (the Cartoon Network logo) and
  then stops swapping before Xenia's frame 600, headless as well as
  rendered (at 506 and 345 swaps): what it waits for next is not in the
  kernel yet (sound, most likely). Game B's six traces replay through the
  same Sink and match. (Later the same day boot answered from an idle pad
  for user 0, [input.md](input.md), and with the generated code the game
  plays on past the Cartoon Network logo to Papaya's, past swap 700.)

Game A's title screen ("Press START" over clouds, from about swap
1800 live, Xenia's frame 2400 and later) came out in red alone. Its trace
(`A_2400`, in the regression) showed why: the menu's frame moves
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
too), game B's texture slots without a texture constant. On the title the
renderer is the bottleneck: 590 s of the 600 busy, 442 s of them in its
fifteen resolves a frame, read back and tiled by the CPU (B4). Its "Press
START" pulse shows one phase at every hundredth swap from 2100 to 6300,
not the phase of Xenia's frame 2400 nor that of its 3000 and 3600: the
pulse is the game's (its clock or its frame count), not the renderer's.

The cost, before B4 (runs of the same length back to back, other work
sharing the machine): Game A is bound by the interpreter, and rendering on the
processor's thread costs it nothing measurable (747 swaps in 90 s headless,
742 rendered; the renderer busy 24 s of the 90). Game B is bound by the
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
| Game B, 45 s headless | 60.0, 60.0 | 40.9, 60.0 |
| Game B, 45 s rendered | 14.7, 19.4 | 43.2, 60.0 |
| renderer busy | 41.3 s (25.9 resolves, 19.6 submissions) | 2.3 s |
| Game A, 150 s headless | 22.3, 30.0 (3,353 swaps) | 29.9, 29.9 (4,590) |
| Game A, 150 s rendered | 17.0, 8.0 on the title (2,566) | 30.0, 30.0 on the title (4,504) |
| renderer busy | 146.6 s (90.1 resolves, 37.1 submissions, 25.3 decoding textures) | 11.5 s |

(game B's whole-run rate fell headless as well: since the player's merge
boot answers an idle pad, and the game plays past its logo into scenes
that pace themselves; both runs end at 60.) After, game A's 11.5 s
are 3.9 s copying vertex data (103 GB: the fetch constants' whole ranges,
about 1 MB a draw), 1.8 s recording resolves, 1.3 s checking textures, 1.1
s decoding them (the intro movie's three planes a frame, written by the
CPU), 1.1 s copying what resolves wrote into guest memory (16 GB), 0.4 s
submitting (0.1 s of it waiting for the GPU); 4,508 submissions and 4 waits
in the run. Game B's 2.3 s: 0.7 s in resolves, 0.5 s checking textures,
0.4 s decoding, 0.4 s copying 6.9 GB back; 2 waits. On the title every
texture reloaded in a frame (about 17) is a resolve's, loaded on the GPU.

Live against Xenia's dumps (shots of every swap around each frame): El
Game A's 300 at swap 290 (0.005 of 255, no pixel off by 16), its 900
at swap 818 (0.003), 1800 at 1615 (0.004), its title (2400) at swap 2910
(0.000, the "Press START" pulse in the same phase); its 301 lies between
two of our swaps (the logo's fade runs on the clock, and Xenia swaps twice
as often there: our 290 differs from Xenia's 301 by what Xenia's 300 does,
5.08). Game B, now past its logo: 600 and 601 at swaps 574 and 575 (0.000),
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

Game A and game B kept an RTX 5060 at 100% in their 3D scenes and
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
  interpolator): Game A's FXAA pass went from 5.5 ms to 0.12 ms at
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
  resolve to the same memory many times a frame (game A's
  post-processing chain goes through one texture's memory). The read-backs
  are recorded at the submission's end now, for what no later resolve wrote
  over (26 MB a race frame became 18 MB).

GPU time a frame, before (master's renderer with the timing added) and after,
`xfreplay --gpu-time --repeat 10`, the GPU otherwise idle:

| trace | before | after |
| --- | --- | --- |
| Game A, race (`xf_race`) | 55.5 ms | 8.9 ms |
| park behind "Press START" (`xf_park`) | 44.7 ms | 7.5 ms |
| Cups menu (`xf_menu`) | 56.3 ms | 9.4 ms |
| loading screen (`xf_loading`) | 6.5 ms | 1.3 ms |
| title (2400) | 35.0 ms | 7.5 ms |
| intro movie (1800) | 6.5 ms | 1.5 ms |
| Game B, 600 / 1800 / 3600 | 21.2 / 43.1 / 43.1 ms | 2.7 / 3.0 / 3.0 ms |

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
| Game B, its 3D scene (70-150 s) | 14.2 swaps/s, GPU busy 98.9% | 59.5 swaps/s (vblank), 58.5% |
| Game B, 150 s: GPU time, renderer waiting for it | 146.8 s, 110.6 s | 29.7 s, 0.0 s |
| Game A, title (swaps 2100-3350) | 28.3 swaps/s, 97.6% | 29.4 swaps/s, 90.6% |
| menus and the park (4000-6800) | 15.6 swaps/s, 94.6% | 23.9 swaps/s, 88.9% |
| the race (7700 on) | 5.9 swaps/s, 88.9% | 15.1 swaps/s, 80.9% |
| 500 s: GPU time, renderer waiting for it | 392.3 s, 245.6 s | 166.8 s, 0.1 s |

The renderer no longer waits for the GPU; game A's race is bound by
the renderer's thread now (68.8 s checking textures by hash, 72.5 s
comparing vertex data, of 342 s busy). `nvidia-smi` keeps reporting a busy
GPU where the timestamps add up to far less: what it counts is mostly the
read-backs, which live take 38% (game A) and 65% (game B) of the
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
as before, game B's 3600 and 3601 and game H's title closer (0.001 to
0.000, 0.007 to 0.000: their MSAA resolves).

Live, game A (`boot_<A> --render --rates`, 600 s, the press
script of "game A past the title" with the race's presses at
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
in its fade's next phase (the clock's). Game B plays at 49-60 swaps a
second, its watches taking 12,593 faults in 120 s.

Game G's title (`G_850` and `_950`): 10.7-10.9 of 255
off Xenia (15% of pixels) before, 9.21 and 9.29 (13.4% and 13.5%) with its
MSAA resolves (4x MSAA depth resolved to 24_8 textures among them) done. The colored noise over its
ground and buildings is left: they sample 4_4_4_4 textures with gamma
signs that the game makes by resolving 4x MSAA targets drawn as
8_8_8_8_GAMMA and resolved through their 8_8_8_8 view; not found yet.

Left: the POSIX fault handler is not built here (Windows only so far),
untried on Linux and Android; the write count is 32 bits (the race run
numbers about 2,000 writes a second, the XMA decoder's most of them: weeks
before it wraps). The bindless table's places were never given back, so
the 600 s race run filled the 2D table (33,165 loads refused near its
end): "The texture cache's lifetime" below.

## The texture cache's lifetime (2026-11-06)

As Xenia's texture cache keeps its textures (texture_cache.cc: `Texture`'s
`MarkAsUsed`, `CompletedSubmissionUpdated`, the
`texture_cache_memory_limit_*` options; d3d12_texture_cache.cc's
`~D3D12Texture`, which gives its descriptors back). Each texture knows the
GPU memory its images hold (`vkGetImageMemoryRequirements`, as Xenia's
`GetResourceAllocationInfo`), the last submission that used it and the
host's clock then, and its place in a list by last use: a use moves it to
the end, once a submission. When the GPU has finished submissions since
the last look, before the next texture is looked up, the least recently
used textures go while the cache holds more than the soft limit (384 MB)
and their last use is done: if unused for the soft lifetime (30 s), or at
once above the hard limit (768 MB); the render-to-texture part (24 MB) is
what a draw resolution scale would add, none at scale 1.
`Options::texture_memory_*`, `XF_TEXTURE_MEMORY=soft,hard[,lifetime]` for
boot and the player, `xfreplay --texture-memory`.

A texture is destroyed only once the GPU is done with its last use, so its
places in the bindless tables go back to a free list then, pointed first
at the stand-in (no descriptor ever names a destroyed view; the tables are
update-after-bind and update-unused-while-pending, and nothing in flight
reads those places). An image that a constant replaces (another shape for
the same constant) goes with its places when the submission being
recorded is done. Textures written by the CPU (their watches) are loaded
again in place at their next use, as Xenia's outdated textures, and age
out with the others when never used again.

Where it differs from Xenia: a use counts for the submission after the one
being recorded, because the draw that asked records after its loads,
which may submit that one, and a swap's presenter reads the front buffer
after it is submitted (one submission later than Xenia, never sooner).
Xenia's one table holds 262,144 descriptors; ours hold 16,384 2D and 4,096
3D and cube (the slot word's 14 bits), so a table without room destroys
the least recently used textures of its kind the GPU is done with, waiting
for submissions in flight if it must (never the one being recorded): a load
is refused only when the submission being recorded holds the whole table.
(The cache was keyed by the whole fetch constant then; it is keyed as
Xenia's since: "The texture cache's key, its loads and the image pool"
below.)

Measured live, game A's 600 s race run (`boot_<A> --render
--rates`, the press script above, `XF_GPU_TIME=1`, two other agents
building): no load refused (33,165 before), 16,385 textures destroyed by
the limits and none to make room, 304,950 loads. The cache reached the soft
limit at 210 s and stayed there through the menus and the park (2,200-3,900
textures in 382-384 MB), went above it as the race began (643 MB at 480 s,
707 MB at most: the race's textures all used within 30 s) and came back to
384 MB (4,530 textures) from 510 s on. The race at 29.6-30.2 swaps a second
(the cap is 30), the menus and park at 28.9-31.0. The process's resident
memory: 1,009 MB at 180 s, 1,083 MB at 300 s, 1,143 MB at 480 s, then
1,156-1,160 MB from 510 s to the end. The same run for 420 s with
`XF_TEXTURE_MEMORY=0,0` (every texture destroyed one submission after its
last use, 22,510 of them; 189-473 held, 85 MB at most) had the same
resident memory at the same times (995 MB at 180 s, 1,071 at 300 s, 1,129
at 420 s): its growth is the game's (its pipelines, shaders and the race's
data), not the cache's, whose images are the GPU's memory. Its shots
differ from Xenia's dumps only where they animate (the title's blinking
"Press START", the Cups menu's turning driver), and the replays of three
traces with every texture destroyed and loaded again between plays (a
build counting a use for the submission being recorded, under validation)
gave the same pictures as one play.

## The texture cache's key, its loads and the image pool (2026-11-06)

**Why game A loads about 500 textures a second.** The suspicion
was the write watches: a fault widens to the 64 pages around (as Xenia's
shared memory does) and would invalidate textures beside what the CPU
wrote. Measured with a build that kept, by watch page, the last exact write
and what numbered it (a fault's own page, its widening, a host write), and
a hash of each texture's guest bytes at each load, over the 600 s race run
of "The texture cache's lifetime" (283,139 loads):

| why the load | loads | |
| --- | --- | --- |
| a resolve or a memory export wrote the texture's memory | 241,723 | 85% |
| the texture was not held (new, or evicted at the soft limit) | 16,632 | 6% |
| the CPU wrote its memory (its watches) | 7,123 | 2.5% |
| of those, a page of it written exactly / only by a fault's widening | 6,604 / 519 | |
| of those, its bytes unchanged | 415 | |

So the watches and their widening are not the cause: what the CPU writes
loads about 12 textures a second (six textures the game rewrites in place,
1,407 and 521 loads each), their bytes changed in 94% of those loads, and
the widening alone accounts for under 1 a second. The loads come from render to
texture: the race resolves 540 times a second into the memory of a few
render targets (in a second such run, with the key below, seven bases
between 0x141D0000 and 0x15580000 took 207,000 of 209,000 such loads, up to
four a frame for one of them: a chain of passes each resolving into the same
memory and sampling it) and draws read each result
as a texture, which is loaded again from the mirror on the GPU. Xenia does
the same: a resolve's `RangeWrittenByGpu` fires the watches of the textures
over its pages (`FireWatches` with `invalidated_by_gpu`), and their next use
loads them again from shared memory with its load shaders. Avoiding those
loads (a resolve writing straight into the texture that reads it) would go
beyond Xenia and is left open.

What the measurement did find: the cache held an image per fetch constant,
so the same memory under constants that differed only in their sampler
(filters, clamps, the smallest level) was loaded once for each of them,
after every resolve, and filled the cache with copies (4,243 textures in
384 MB at its soft limit, 605 MB at most, 12,389 destroyed by the limits and
loaded again).

**The cache keyed by Xenia's `TextureKey`** (texture_cache.h,
`BindingInfoFromFetchConstant`; `D3D12Texture::SRVDescriptorKey` and
`FindOrCreateTextureDescriptor`): a texture is keyed by the pitch and
tiling, the format, endianness and stacking, the size, the levels the
constant gives (`texture::levels`: the base and mip pages within physical
memory, so the windows' aliases of a page are one, and the last level), the
dimension and packed mips, and whether it is the separate signed version.
Constants that differ in their swizzle and signs read it through views of
their own, made the first time one asks, each at a place of its own in the
texture's bindless table: a view by the swizzle through the host format's
and the sign, as Xenia's descriptors by `SRVDescriptorKey`. The filters,
clamps, border, LOD bias and smallest level were the samplers' already.
An image a signed view may read is made mutable with both formats listed
(`VkImageFormatListCreateInfo`, as Xenia's Vulkan textures), whether or not
the constant that made it reads signed components.

**Textures by bucket of memory** (Xenia's shared memory watch buckets,
`kWatchBucketSizeLog2`: 4 MB): a resolve or a memory export looked at every
texture held for the ones over what it wrote; now each texture is listed in
the 4 MB buckets its ranges span, and they look only at the buckets they
wrote. (It also no longer takes the first and last of the written ranges as
their bounds, which a destination wrapping past the end of physical memory
would have broken.)

**The image pool** (vk_device.cpp; Xenia's Vulkan backend has VMA place its
images, its D3D12 one makes committed resources): images lie in blocks of
64 MB of device memory (an eighth of a smaller heap) shared by many, each
image in the smallest free range of a block of its memory type that holds
it at its alignment, the free ranges merged with their neighbors as they
come back; an image larger than half a block, or whose driver prefers or
requires memory of its own (`VkMemoryDedicatedRequirements`), gets its own.
Only images are pooled, all of optimal tiling, so the buffer-image
granularity never applies; buffers keep memory of their own (a few dozen).
A block left empty is freed while another empty one of its type is kept.
`XF_VK_DEDICATED_IMAGES=1` gives every image its own memory, as before. No
third-party allocator: VMA would be a new dependency for what the cache
needs of it.

**Counted**: the loads by why (`Report::texture_loads_new`,
`texture_loads_cpu_written`, `texture_loads_gpu_written`), the views held,
the device's memory allocations (`vk::Device::memory_counts`: live, most at
once, made; the pool's blocks and images), and the loads a second and the
allocations on the `--rates` line.

Measured live, game A's 600 s race run (the press script of
"The texture cache's lifetime", the pipeline cache warm in both, other
agents building on the machine; "before" is master with the loads a second
counted):

| | before | after |
| --- | --- | --- |
| swaps | 17,961; the race (from 420 s) at 30.0 a second | 17,956; the race at 30.0 |
| texture loads | 299,405; 641 a second in the race | 233,530 (1,483 new, 7,168 after the CPU's writes, 224,879 after resolves and exports); 499 a second in the race |
| textures held at the end | 3,979 in 384 MB (664 MB at most), 15,411 destroyed by the limits | 1,483 (1,483 views) in 137 MB (137 MB at most), none destroyed |
| renderer busy | 128.8 s (20.4 s in resolves); 23.1% in the race | 118.3 s (4.8 s in resolves); 20.6% in the race |
| device memory allocations | one an image (about 4,000 live, 19,400 made) | 27 (27 made): 1,488 images in 3 blocks (140 of 192 MB), 18 of their own (114 MB) |
| resident memory | 998 MB at 180 s, 1,103 at 360 s, 1,177 at 600 s | 886, 1,016, 1,140 MB |

A run with the key and the pool but before the buckets spent 9.4 s in
resolves (20.4 before, 4.8 with the buckets). Game B (120 s, no input):
6,399 swaps before and 6,387 after (60 a second), 60 loads a second in both
(one render to texture a frame), 47 textures in 17.7 MB before and 44 in
13.5 MB after, 423 MB resident in both, 12 device memory allocations with
the pool and 58 with `XF_VK_DEDICATED_IMAGES=1`. The race's trace
(`xfreplay --gpu-time --repeat 10`): 6.11 ms of GPU time a play, 0.64 ms
(10.4%) of it the 13 texture loads after the frame's resolves; the picture
0.010 off Xenia's as before. `replay_test` gives the same numbers for its 29
frames as master, under validation and with `XF_VK_DEDICATED_IMAGES=1`.

Left: the loads after resolves (render to texture, nearly all of the 499 a
second in the race), as Xenia's; a resolve could write the texture it feeds instead
(beyond Xenia). Xenia also keeps a texture's base and mips outdated apart
and loads again only the part written; here a texture is loaded whole.

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

**Game A past the title** (traces in `../traces/xf_<A>`, Xenia's
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

Four of them are in the regression now (`../traces/<A>_xf`, Xenia's
dumps as `../traces/dumps/A_xf_*.png`): the Cups menu, a loading
screen, the park and the race, all within 0.017 and 0.03%; with a frame
each of game D and game H (below, `../traces/<D>_xf` and
`../traces/<H>_xf`) `replay_test` compares 17 frames. The loading
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
  passes); point sprite coordinates there since 2026-11-08 ("Resolve
  targets, and Xenia's approximations closed").
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

- **game D** draws everything it sends; its front buffer is
  2_10_10_10, shown through the display's piecewise linear gamma ramp
  (Xenia's apply_gamma_pwl: a component's 10 bits choose an entry by their
  top 7, base plus delta times the low 3 over 8), which was left out:
  frames 600, 1500 and 3000 were 40.2-53.4 of 255 off Xenia (84-99.7% of
  pixels), and are 0.001-0.014 (none off by 16) with it. Counted: depth
  resolves of MSAA targets (7,857 in 150 s), not in those frames (done
  since, from EDRAM).
- **game H** reaches its title; its title's draws use
  getCompTexLOD (27,146 skipped in 180 s) and then getWeights (46,844),
  now translated as Xenia's (OpImageQueryLod's unclamped level in x, in
  pixel shaders with the computed LOD; the bilinear lerp factors, the LOD
  disregarded as Xenia does). Its frames 300, 450, 600 and 800 match
  Xenia's (0.000 skipping those draws, 0.007-0.008 drawing them). Also
  counted: MSAA depth resolves, texture slots without a texture constant.
- **game G** reaches its title (a 3D scene behind "Press
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
Xenia's trace dump does. Game A's race frame, cold (no cache on
disk): 1.11 s of the renderer's thread waiting for pipelines, 0.58 s with
them made aside (334 of its draws skipped that frame, 6 the next, none
after). The Vulkan pipeline cache on disk is shared by the workers.

**Fewer pipelines**: Vulkan 1.3's extended dynamic state 1 and 2 (core, so
no fallback) take the topology within its class, primitive restart,
culling, the front face, the depth and stencil tests and ops and the depth
bias's enable at the draw, out of the pipeline key (Xenia's Vulkan backend
keys them: a TODO in its command processor). The race: 102 pipelines, 96
now. Extended dynamic state 3 takes the blend state too where the device
has it (below, "Pipelines while they are made").

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
RTX 5060 with `xfreplay --gpu-time-draws --repeat`: Game G's
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

## Pipelines while they are made, and the interpolators' centroids (2026-11-06)

**Something drawn while a pipeline is made** (B5; `Options::pipeline_stand_in`;
`XF_PIPELINES=wait|skip|placeholder|library` for `boot` and the player,
`xfreplay --async-pipelines --stand-in ...`). With pipelines made on the
workers, a draw whose pipeline was not made yet was skipped, as Xenia's
D3D12 backend skips it (`skip`). Two ways to draw it instead are built:
- `placeholder`, Xenia's Vulkan backend's pipeline hot-swap
  (`vulkan_pipeline_cache.cc`): a pipeline of the draw's own state and
  vertex shader with a pixel shader writing transparent black
  (`placeholder.frag`, Xenia's `placeholder.ps.glsl`, but writing the four
  colors where Xenia's writes oC0 alone, so no bound target gets undefined
  values), made at once on the renderer's thread and drawn until the whole
  pipeline replaces it.
- `library`, VK_EXT_graphics_pipeline_library's fast linking: a pipeline
  in its four parts, each a library kept by its own key (the vertex input's
  attribute and topology class; the vertex, tessellation and geometry
  shaders' modules with the patch size, depth clamp and bias, and
  rasterization discarded; the pixel shader's module with the samples; the
  targets' formats and samples, with the blend state where it is not
  dynamic). The two with the guest's shaders are made on the workers, ahead
  of everything queued; the vertex input and fragment output, which have no
  shader, on the renderer's thread. Once an entry's four are made, a
  pipeline is linked from them (no link-time optimization) and draws until
  the whole pipeline replaces it. The whole one is queued only then, and
  made only when nothing else waits, on half the workers at most: it draws
  what the linked one draws (the race frame's GPU time 6.18 ms with whole
  pipelines, 6.24 with linked ones), so it must never delay a library a
  draw waits for. A new shader's draws are still skipped until its library
  is made; a new combination of known shaders and states draws at once. The
  libraries go in the pipeline cache on disk, the linked pipelines do not.
  A stand-in replaced is destroyed once the submission recording it is
  done.

The device's libraries come from a feature query
(`vk::Device::pipeline_library`: the extension with
`graphicsPipelineLibraryFastLinking`); `XF_VK_NO_GPL=1`
turns them off, and then `library` skips the draws, as `skip` does.
`Options::linked_pipelines_only` (`xfreplay --linked-pipelines`,
`XF_LINKED_PIPELINES=1` for `replay_test`) draws every draw with a linked
pipeline and none whole, made at once: the 29 frames match exactly as with
whole pipelines, validation clean (the race frame: 153 libraries for its 96
pipelines).

Measured cold, which needs both caches empty: the pipeline cache on disk
(`XF_PIPELINE_CACHE=<file>`) and the NVIDIA driver's own
(`__GL_SHADER_DISK_CACHE_PATH=<empty folder>`; with it warm, the race
frame's 96 pipelines take 0.3 s, cold 9.2 s). The report counts hitches:
frames in which the renderer's thread spent more than 4 ms making
pipelines, the most it spent in one, and swaps more than 50 and 100 ms
apart. Game A live (`boot_<A> --render --rates`, 680 s, the press
script of "game A past the title" with the race's presses at
10000-10750; title, menus and park up to swap 10000, the race after it, to
swap 19,900-20,100; RTX 5060, 12 logical cores, so 9 workers, other agents
building; the `--rates` lines now give the pipelines and skipped draws so
far, so runs compare at the same swap):

| | skip | placeholder | library | library, no EDS3 |
| --- | --- | --- | --- | --- |
| pipelines (whole) | 181 | 180 | 177 | 179 |
| and | | 171 placeholders | 244 libraries, 168 linked | 258 libraries, 170 linked |
| draws skipped, to swap 10000 | 1,877 | 0 | 1,275 | 1,838 |
| draws skipped, in the race | 422 | 0 | 373 | 311 |
| draws by a stand-in | 0 | 590, transparent black | 5,343, exact | 5,367, exact |
| the renderer's thread making pipelines | 1.01 s | 10.49 s | 0.91 s | 1.11 s |
| frames with more than 4 ms of it (the most) | 6 (747 ms) | 54 (1,076 ms) | 6 (691 ms) | 6 (839 ms) |
| swaps more than 50 / 100 ms apart | 701 / 54 | 1,226 / 41 | 113 / 24 | 1,103 / 11 |

`skip`'s six long frames can only be pipelines of draws without a pixel
shader, made at once as before (Xenia's too); `library` has as many, as
long, so its links and shaderless libraries add none. `library` is the
default: fewer draws skipped than `skip` in both runs (2,149 and 1,648
against 2,299; the runs vary with the machine's load, other agents'
builds), every stand-in drawing exactly, no more time on the renderer's
thread. Its gain is bounded by game A's pipelines being new pairs of
shaders (89 of the race frame's 96): a new shader's draws wait for its
library as they waited for the whole pipeline, and only new combinations
of known shaders and states draw at once. `placeholder` skips nothing but
makes every new pipeline twice, once on the renderer's thread (54 long
frames, 10.5 s), and draws its stand-ins black; it stays an option. The
swaps' gaps vary too much from run to run to rank them. A first form of
`library`, which queued each whole pipeline at once beside its libraries,
skipped more draws than `skip` (2,184 against 1,449 in 500 s): the
workers made every new shader twice at the same time.

**The libraries at the launcher's build step.** What costs is the driver
compiling the shaders (the race frame's 96 pipelines: 9.2 s from an empty
driver cache, 0.3 s from a full one). The libraries make that cost per
shader rather than per pipeline (153 libraries for the race's 96
pipelines), which makes compiling ahead practical: built on 2026-11-07, in
"Shaders and pipelines at the launcher's build step" below.

**Extended dynamic state 3** (`vk::Device::dynamic_blend`; `XF_VK_NO_EDS3=1`
turns it off): the blend enables, blend equations and color write masks
set at the draw, so pipelines key the targets' formats alone (and the
fragment output libraries the formats and samples alone). Counted on the
traces, without and with it: Game A's race 96 and 96 (its 96
pipelines are 89 pairs of shaders; the other 7 differ by MSAA, depth format
or a color format, none by blend), park 54/54, menu 36/36; game B 8/5;
Game E 44/39 (45/39 at 3600); game H's title 32/30; game G
148/148 and 147/147. Live (the table above), game A's
680 s made 177 whole pipelines with it and 179 without, and 244 libraries
against 258 (the fragment output's). The earlier guess that the race's
pipelines differed mostly in blend state was wrong: what makes pipelines
there is shaders, so B5's stand-in is what removes the skips, not fewer
keys.

**Game G's edges: the interpolators' centroids.** Frames 850
and 950 were 0.45 off Xenia's D3D12 picture (0.46% of pixels off by more
than 16) after the host depth kept across transfers, on the edges of
geometry, brighter than Xenia's: 3,818 of the 4,226 pixels off D3D12's were
off Vulkan's too, where Xenia's two agree. Under MSAA the console reads
interpolators at the centroid of the pixel's covered samples, not its
center, by SQ_CONTEXT_MISC's `sc_sample_cntl` (bits 2-3: centroids only,
centers only, or both by SQ_INTERPOLATOR_CNTL's `sampling_pattern`, a bit
an interpolator, 1 the center); at the center a triangle's edge pixels
extrapolate its values past it. Xenia's xenos.h names this game for it
(`GetInterpolatorSamplingPattern`) and its translators decorate those
interpolators Centroid (`interpolators_centroid`). Now the pixel shader
takes the mask as a variant (`shader::PixelOptions::centroid`, in the
pipeline key): its interpolators one variable each, Centroid-decorated
where the mask says, the array as before otherwise. The frame uses
centroids only in 1,046 draws and patterns in 1,171. Frames 850 and 950:
0.452 and 0.453 to 0.347 and 0.347 off D3D12's picture, 0.459% and 0.458%
to 0.238% of pixels; still above `replay_test`'s 0.1%, so not in its
corpus. What is left is small vegetation on the ground (alpha to coverage
draws, 2,106 of the frame's) and thin texture features such as the cactus
ribs, where Xenia's two pictures again agree with each other (1,887 of the
2,189 pixels off D3D12's are off Vulkan's too). Ruled out by trying: the 2x
sample order (swapping it makes 2.98% off), alpha to coverage's 2x
thresholds swapped (1.57%) and its dither's axes swapped (unchanged), the
interpolators through InterpolateAtCentroid instead of the decoration
(identical), an invariant position (identical; kept, as Xenia's outputs
are invariant), and the samplers, made as Xenia's Vulkan backend makes
them. The 29 frames of `replay_test` are unchanged.

## Shaders and pipelines at the launcher's build step (2026-11-07)

A game played again compiles no shader: the runs record what they meet,
the launcher's build step makes it into the game's pipeline cache, and the
next run finds every library and pipeline made.

**The storage** (`shader_storage.h`, as Xenia's per-title shader storage,
its pipeline caches' `InitializeShaderStorage`: `<title id>.xsh` for the
microcode, `.xpso` for the pipelines' descriptions, both appended to while
the game runs): one file, `<cache>/<title id>.xfshaders`, beside the game
library in the launcher's cache, and the pipeline cache beside it,
`<title id>.vkpipelines` (boot and the player alike; `game::start_renderer`;
`XF_SHADER_STORAGE=<file>|none`, `XF_PIPELINE_CACHE=<file>`). A header
(magic, version), then records appended and flushed as they come, so a run
cut short keeps what it met: each shader's microcode the first time it is
loaded; each new pipeline's description; and what a build step made, for
the device and driver it made it on. The renderer writes it
(`render::Options::shader_storage`).

**A pipeline's description** is the whole `VkGraphicsPipelineCreateInfo`
the draw made, state by state, with its shader modules by name rather than
handle: a guest shader's hash and host form (`Shader::variants`' key: the
vertex shader's `VertexOptions`, the pixel shader's `PixelOptions`, both
of which the key gives back), or one of the renderer's own (the geometry,
tessellation, interlock depth and float24 shaders). Before it is described
or made, the values of the states the pipeline sets dynamically are set to
one each (topology to its class's first, cull mode, front face, the depth
and stencil tests, the blend under extended dynamic state 3): Vulkan
ignores them, and so the same pipeline gets the same description, and the
same entry in the driver's cache, whichever draw asked for it first. The
libraries' keys are taken from the description the same way (the modules
by name), so a run and the build step key them alike.

**The build step** (`render::precompile`; `render::run_shader_step` is
`BuildOptions::shader_step`, given by the launcher's prepare job and by
`xfbuild`, which runs it after building the library and also when the
library is cached; `xfbuild --shaders-only` runs it alone,
`--no-shaders` and `--no-disc-shaders` leave parts out). On a Vulkan
device of its own and a renderer made as a run's (its layout and own
modules): the storage's shaders translated, every description made back
into a pipeline's (a description naming a shader or form the renderer
cannot make here is counted and left), then the libraries of its four
parts (each once) and the whole pipeline queued on the pipeline workers,
the cache written, and a record of the libraries' and pipelines' key
hashes appended to the storage with the device's vendor, device, driver
version and pipeline cache UUID. Descriptions an earlier record names for
the same device and driver are not made again while the cache file's
header is still that device's. Without a device it reports why and
builds the library all the same; a cache of another device or driver is
dropped by Vulkan and everything is made again. What it made goes in the
library's `.build.txt` (`shaders` lines, `BuildReport::shaders`). The disc
is searched for shader containers too (`xfeffect`'s search over each file
up to 64 MB, media left out): they are translated and counted, but
without a pipeline state no library is made of them (a pipeline's fixed
state, its targets' formats and the pixel shader's form all come from the
draw).

**The run** reads the records that match its device and driver, while the
pipeline cache file's header is still theirs: a library the build step
made is made at once on the renderer's thread (a lookup in the cache)
instead of being queued for the workers, so a new pipeline whose libraries
are all made links at once and draws; a pipeline the build step made whole
is made at once too where the draw would otherwise wait for the workers
(the `skip` and `placeholder` modes, or a device without graphics pipeline
libraries). The report and the `--rates` lines count the pipelines and
libraries made so ("as built").

Measured on game A (`boot_<A> --render --rates --seconds 430`,
`XF_PIPELINES=library` as the player's default, the press script to the
race of "Traces of live runs", about 12,500 swaps with the race from
swap 10,750; RTX 5060, other agents' builds and boots on the machine): a
cold run (both caches empty), then from the same caches and storage a
second run without the build step, and one after it.

| | cold | second run | after the build step |
| --- | --- | --- | --- |
| pipelines (whole) | 175 | 168 (5 new) | 169 (4 new to the storage) |
| libraries made (as built) | 233 | 222 (none) | 225 (221) |
| draws skipped waiting for a pipeline | 2,852 | 246 | 17 |
| draws by a linked stand-in | 10,531 | 247 | 251 |
| the renderer's thread making pipelines | 1.82 s | 0.01 s | 0.06 s |
| frames with more than 4 ms of it (the most) | 8 (476 ms) | 0 (1 ms) | 3 (8 ms) |
| draws without a pipeline, on average (the most) | 1,563 ms (69 s) | 972 ms (62 s) | 6 ms (299 ms) |

The build step for those: the storage's 229 shaders and 175 descriptions
made into 247 libraries and 175 whole pipelines in 2.2 s from the caches
the cold run had warmed (14.2 s of the driver's on 9 workers); from empty caches (a new driver) the same took 14.3 s (125.5 s of the driver's), the cache written 25.4 MB.
The disc's search read 374 files in 19-31 s and found 88 shaders, 75 of
them not met by the run (UE3's global shader cache, packages being
compressed), 29 the translator cannot translate yet. The 17 draws still
skipped are those of the 4 pipelines the cold run had not met. The second
run without the build step already had both caches warm: its libraries
cost the workers little, but every new pipeline's draws still went
without one until a worker had looked its libraries up (246 skipped, 972
ms on average, the longest 62 s: measured until a draw next asked for
the pipeline, so a scene shown again a minute later counted the minute; since
2026-11-08 the wait ends when the worker made the pipeline, or a linked
stand-in's last library); with the build step's
record they are made on the renderer's thread in 0.06 s for the whole run.
The swaps' long gaps (33-36 above 100 ms) are the loading screens in all
three runs. The race frame's replay is unchanged by the descriptions'
dynamic states set to one each (0.008 of 255, 0.014% of pixels off, whole
and linked).

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
  `replay_test` stay within a mean of 0.34 of Xenia's; game A's park
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
- the same (0.000) on game B's six frames, seven of game A's (the
  five of `<A>`, the loading and menu screens of `<A>_xf`), game
  D's four, game H's five and game G's frame
  600;
- Game A's park and race (`<A>_xf`): D3D12 3.144 (6.9%) and 1.707
  (4.4%) off Vulkan, darker in the brightest parts (the blossoming trees,
  lamps and curbs of the HDR scene); ours is 0.017 and 0.010 off Vulkan's,
  so 3.142 and 1.712 off D3D12's. The cause is the host's anisotropic
  filtering (found 2026-11-06, below), not Xenia's logic, and ours stays as
  it is;
- the memory export frames of the race (`xf_<A>/memexport4`): 0.13-0.15
  (0.1-0.3%) between Xenia's two, ours 0.01-0.05 off Vulkan's;
- Game G's frames 850 and 950: 0.21-0.22 (0.1%); ours 0.45
  off D3D12's since the host depth (below).

**New traces from Xenia** (`tools/capture_traces.ps1`, no input, frames
600, 1200, 2400 and 3600; `../traces/xenia_<C>`, `xenia_<E>`,
`xenia_<F>`), all twelve in `replay_test`'s corpus, Xenia's two pictures
the same on each:
- Game C (C): the title, two frames of the intro
  video, the Frostbite logo; ours 0.000-0.002 off;
- Game E (E): the start screen behind its "Connection Lost"
  dialog, three frames, and a white one; ours 0.039-0.043 off;
- Game F (F): the copyright screen, a dark frame, the intro's
  planet and the main menu over its 3D scene; ours 0.000-0.082 off (the menu
  counts 25 draws with D24FS8 depth as 32-bit float and 7 texture slots
  without a texture constant).

**Game G's noise** (frames 850 and 950 of `xf_<G>`, 10.8 of
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
loads EDRAM's value as before. Game G's frames 850 and 950
went from 9.20 and 9.28 off D3D12's picture to 0.45 and 0.46 (0.46-0.47%
of pixels off by more than 16; 0.36 and 0.38 off Vulkan's, whose two
pictures are 0.21 apart): the noise is gone, what is left are edges (the
interpolators' centroids, "Pipelines while they are made" above). The 29
frames of `replay_test` are unchanged. `XF_DEPTH_FALLBACK=1` makes
D24S8 a 32-bit float depth, as on devices without 24-bit unorm depth
(AMD), to test that path and its unorm24 half. Xenia's `depth_float24_round` is
an option (`Options::depth_float24_round`, `xfreplay
--depth-float24-round`, `XF_DEPTH_FLOAT24_ROUND=1`), off as there: the
EDRAM stores and the loads' comparison round 20e4 to the nearest even
rather than towards zero. Single-sample depth resolves follow the option
too since, and `depth_float24_convert_in_pixel_shader` is an option of its
own ("Xenia's GPU TODOs, category A" below); off, as Xenia's default, the
25 draws of game F's menu and the rest keep counting "D24FS8 depth as
32-bit float".

**Game A's park and race, D3D12 against Vulkan** (2026-11-06): the
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

## Xenia's GPU TODOs, category A (2026-11-06)

The items of `docs/xenia-todos-gpu.md`'s category A but the first (exact
EDRAM through fragment shader interlock, its own task), each as Xenia's code
does it; where Xenia has nothing, AMD's guides (R5xx, R6xx, RV630) and Mesa
say what the registers mean.

**What the corpus and the games use** (`Report::used`, printed as `uses`
by `xfreplay` and the runs: what the draws and packets ask for, done or
not). Over the 115 traces in `../traces`: Game G's title (two
traces) has 288 draws with user clip planes, 4,878 EVENT_WRITE_EXT packets,
232 VIZ_QUERY packets, 844 draws predicated on a VIZ query, 116 VIZ survey
draws and 91 draws into 8_8_8_8_GAMMA targets; game B's four traces 128
EVENT_WRITE_EXT; game D 4 draws and game F 1 into gamma targets.
None draws wide lines, with immediate indices or getBCF, or a point list
with user clip planes. `DC_LUTA_CONTROL` is 0 at every swap but game
D's (3, its 10-bit front buffer). Live, game A (420 s to the
race, 11,139 swaps) asks for none of them but D24FS8 depth (2,751,412
draws); game B (300 s, 7,067 swaps) for 48,676 EVENT_WRITE_EXT packets and
nothing else.

- **8_8_8_8_GAMMA targets blended in linear** (Xenia's
  `gamma_render_target_as_unorm16`, on by default as there,
  `Options::gamma_render_target_as_unorm16`, `--no-gamma-unorm16`,
  `XF_VK_NO_GAMMA_UNORM16=1`): such a target is an RGBA16 unorm image
  holding linear color where the device samples, renders to and blends that
  format, so the blend is in linear as the console's; the pixel shader
  writes linear. Its gamma is made where it moves through EDRAM: the store
  encodes with xenos.cc's LinearToPWLGamma, the load decodes each byte to
  the middle of its linear range (Xenia's GammaByteToLinearMidpoint, which
  comes back to the same byte through the 16-bit unorm); clears hold the
  clear color linear (PWLGammaToLinear); resolves of such a target read
  EDRAM's bits, as Xenia's always do. Without it the shader writes the
  curve and the blend works on it (counted). Game G's frame
  950 went from 0.457 to 0.453 of 255 off Xenia's Direct3D 12 picture
  (0.469% to 0.458% of pixels), frame 850 and game D's and game F's
  frames did not move.
- **User clip planes** (`PA_CL_CLIP_CNTL`'s `ucp_ena_0-5` while clipping is
  on, `PA_CL_UCP_0_X` on), as Xenia's shader modifications
  `user_clip_plane_count` and `_cull`: the enabled planes packed in the
  system constants, each one's distance the dot product of the guest's
  clip-space position (after `PA_CL_VTE_CNTL`'s formats, before the host's
  viewport transform) with it, a clip distance, or a cull distance with
  `ucp_cull_only_ena` (`shader::VertexOptions`). Rectangles and points are
  expanded in the vertex shader then (the corners' distances are made
  there; a rectangle's fourth corner's mirrored as its position). Points
  follow `ps_ucp_mode` as Xenia's TODO describes it from AMD's guides
  (Xenia's geometry shader copies the center's distances to the corners,
  which is modes 0 and 1): the center's distances for 0 and 1 (the sprite
  kept or dropped whole), the center's as cull and the corners' as clip
  distances for 2, the corners' for 3; mode 2 with more than four planes
  clips by the corners only (8 distances is what every device has; counted).
  Mode 1's "radius-based distance" is the center's, as Xenia's TODO says;
  the radius it means is not documented. Without clip or cull distances on
  the device (`XF_VK_NO_CLIP_DISTANCE=1`) the draw is skipped and counted.
  game G's 144 draws a frame (a water plane's
  `(-0.0001, -0.386, 586.3, -586.5)`) cut nothing visible: frames 850 and
  950 did not move; with the planes negated the frame changes (0.018), so
  the distances are in effect.
- **D24FS8 depth converted in the pixel shader** (Xenia's
  `depth_float24_convert_in_pixel_shader`, off by default as there,
  `Options::depth_float24_convert_in_pixel_shader`,
  `--depth-float24-convert`): for draws into D24FS8 with the depth test on,
  the pixel shader writes every fragment's depth (oDepth's, or the
  rasterized one from the host's 0-0.5 back to 0-1, saturated) rounded to
  20e4, towards zero (with the conservative DepthLess, as Xenia's
  `SV_DepthLessEqual`) or to the nearest even with `depth_float24_round`,
  then halved (`shader::PixelOptions`, Xenia's
  `CompletePixelShader_DSV_DepthTo24Bit`). It runs per sample under MSAA
  through a sample-decorated position, as Xenia's `IsSampleRate` (the other
  inputs stay at the pixel's center: with the pipeline's sample shading
  instead they moved to the samples, and game G's foliage
  edges came out 1.09 of 255 off). Depth-only draws that test or write
  depth get `depth_float24.frag` (Xenia's float24 truncate and round
  fragment shaders), per sample by the pipeline. The viewport's depth
  bounds go through 20e4 (nearest even), the host's depth is no longer kept
  beside EDRAM for D24FS8, and EDRAM stores do not round again (Xenia's
  `IsHostDepthEncodingDifferent` and dump). Not done: Xenia's newer polygon
  offset in the shader (`depth_bias_shader_offset`, off by default there).
  game G's frame 950 with it is 0.660 of 255 (1.04% of pixels)
  off Xenia's Vulkan picture with the same option; Xenia's Direct3D 12
  picture with it is broken (colored noise over the scene, 20.6 off either),
  so the Vulkan one is the reference there.
- **Single-sample depth resolves** honor `depth_float24_round` in
  `resolve.comp` (20e4 rounded to the nearest even from the host's float),
  as Xenia's resolves follow it through its EDRAM dump.
- **Immediate indices** (`DRAW_INDX`/`DRAW_INDX_2` with source select 1,
  which Xenia fails: "report the game"): the processor takes the indices
  from the packet as AMD's `DRAW_INDEX_IMMD` lays them out (one 32-bit
  index a word, or two 16-bit ones, the first in the low half) into
  `gpu::Draw::immediate`, and the renderer draws them as an index buffer's
  (the reset index, offset, 24 bits, clamps). Which half a big-endian CPU's
  first 16-bit index lands in only hardware can say (counted as
  approximate). No trace or game sends one.
- **The display's look-up table by `DC_LUTA_CONTROL`** (register 0x1930;
  AMD's RV630 guide, the register Direct3D and libxenon program): the
  increment exponent picks the 256-entry table (0, indexed by the
  component's 8 bits; a 10-bit front buffer's top 8, which no reference
  states, counted) or the piecewise linear ramp (index bits INC+6 to INC,
  the bits below times the entry's delta over 2^INC added to its base),
  `render::display_gamma`, in the read-back and in the player's presenter
  (which had applied no ramp to 10-bit fronts). The RV630 has an increment
  per component (blue bits 0-3, green 8-11, red 16-19); Direct3D writes 3
  for game D's 10-bit front buffer, which read per component turned
  red and green to the table and its intro 3.955 of 255 off Xenia's
  picture, so on the Xenos the blue field's increment applies to all three
  (the others counted when set). Xenia picks by the front buffer's format
  with the increment 8; both agree on what Direct3D writes (0 for 8-bit
  fronts, 3 for 10-bit). Signed and float data enables are read as
  unsigned (counted).
- **Float targets' bits through EDRAM** (16_16_FLOAT, 16_16_16_16_FLOAT,
  32_FLOAT, 32_32_FLOAT): their images are made mutable and moved through a
  UINT view of the same image (stores read it, loads render into it,
  clears write the clear color's bits through it), so NaN payloads and
  denormals survive as the console's EDRAM keeps them (Xenia's D3D12 TODO on
  D3D11.3's NaN rules); where the device cannot sample and render the UINT
  format at the target's samples (`framebufferIntegerColorSampleCounts`),
  or with `XF_VK_NO_RAW_FLOAT_TARGETS=1`, by value as before. 2_10_10_10_FLOAT
  is 7e3 in EDRAM, not the host's bits, and is not among them.
- **getBCF** (the border color fraction): Xenia's translators make it a
  translation error that is not fatal and store 0 (its TODO: sample a black
  texture with a white border); translated so here, counted ("getBCF
  answered 0"), where the shader was refused and its draws skipped before.
  What the hardware blends at a border with each filter and clamp mode is
  only the console's to say.
- **Wide lines** (`PA_SU_LINE_CNTL`'s WIDTH, half the width in 12.4 fixed
  point per AMD's R6xx guide and freedreno's a2xx.xml; 8, one pixel, is
  Xenia's reset value): the line width is dynamic state set at each draw
  from the register where the device has wide lines
  (`XF_VK_NO_WIDE_LINES=1` turns them off), clamped to its range; without
  them one pixel, as Xenia draws every line (a TODO there), counted.
- **Screen extents and VIZ queries** stay as Xenia's defaults: every
  EVENT_WRITE_EXT is answered with the whole screen, draws predicated on a
  VIZ query and VIZ survey draws are drawn (counted: "draws predicated on a
  VIZ query", "VIZ survey draws"). Xenia's `occlusion_query_viz` (off by
  default: coarse occlusion queries feeding VIZ predicates, survey draws
  skipped in copy mode) is not done; real extents (the pixels and depths
  the draws between two events cover) would be a pass over the occlusion
  queries' draw ranges, and how hi-Z interplays with them wants a console.

## Exact EDRAM through fragment shader interlock (2026-11-06)

Xenia's interlock render target path (`render_target_path_vulkan=fsi`, the
Direct3D 12 backend's `rov`), ported: the pixel shader reads and writes EDRAM
itself, under fragment shader interlock, so EDRAM holds the guest's bits as
the console's does and every format blends as the console blends it. Item A1
of `docs/xenia-todos-gpu.md`. `Options::render_target_path`
(`XF_RENDER_TARGET_PATH=fsi` or `host` forces one); the device's
`VK_EXT_fragment_shader_interlock` is found by a query (`XF_VK_NO_FSI=1`
turns it off) and a device without it keeps host render targets.

**How it is made.**
- The output merger is GLSL, `src/shaders/edram_fsi.frag`, from
  `spirv_shader_translator_rb.cc` (FSI_LoadSampleMask, FSI_LoadEdramOffsets,
  FSI_DepthStencilTest, FSI_ClampAndPackColor, FSI_UnpackColor, the blend
  factor and equation functions, CompleteFragmentShaderInMain's color writes)
  and `vulkan_command_processor.cc`'s UpdateSystemConstantValues for the state.
  Compiled ahead as a library (`-DLIBRARY`), it is taken into each translated
  pixel shader by `spirv::Module::import` (its ids kept, this module's own
  starting past its bound, its types and constants shared with what the
  translator makes after); the translated shader, with `PixelOptions::fsi`,
  writes no outputs: after the guest's body, the alpha test (a demotion) and
  alpha to coverage (a mask in the guest's sample order, Xenia's
  FSI_AlphaToMask, where oC0 is written on the shader's path), it takes the
  rasterized depth's coarse derivatives and calls `xf_fsi` between
  OpBeginInvocationInterlockEXT and OpEndInvocationInterlockEXT, with the
  colors, the targets written on its path (Direct3D 9's rule: those it did
  not write keep EDRAM's) and oDepth. Draws without a pixel shader that test
  depth or stencil get the same file built as a whole fragment shader
  (`-DDEPTH_ONLY`).
- Coverage: the host's samples in the guest's order (2x from host samples 1
  and 0, or 2 and 1 where a pass without attachments takes no 2x and the
  guest's 2x is rasterized as 4x; 4x from 0, 3, 2, 1), minus helpers, kills,
  the alpha test and alpha to coverage. The host rasterizes at the Xenos's
  sample positions where the device places samples
  (`VK_EXT_sample_locations`, `XF_VK_NO_SAMPLE_LOCATIONS=1`), as Xenia's
  interlock paths always do; host render targets keep the standard ones, as
  Xenia's default.
- Depth and stencil per covered sample: the center's depth plus the
  polygon offset (the registers' scale in 1/16 pixels and offset in depth
  units, the front or back face's), extrapolated to the guest sample's
  position by the derivatives, saturated, then 24-bit unorm or 20e4, both
  rounded to the nearest even (oDepth instead, unbiased, where the shader
  writes it); tested and written with the stencil's ops as RB_DEPTHCONTROL
  says (GetNormalizedDepthControl's depth test that neither writes nor
  rejects is off; off too where a color target over the same base writes the
  bits they use, ColorOverlapsDepthStencil). The samples that fail leave the
  coverage.
- Colors per covered sample: times the exponent bias; blended where
  RB_BLENDCONTROL is not one-zero-add, with the destination unpacked from
  EDRAM's bits, sources, factors and results flushed of NaN and clamped for
  fixed-point components, the factors applied before min and max as the
  Xenos does; packed into the guest format (8_8_8_8 and 2_10_10_10 rounded
  half up, the gamma curve made after blending, 7e3, 16-bit fixed in
  -32..32 with 10 fraction bits, float16 in the Xenos's extended range
  without infinity or NaN, 32-bit floats by their bits); the bits the write
  mask keeps kept (GetPSIColorFormatInfo's keep masks).
- Resolves read EDRAM, as the MSAA ones did (`resolve.comp`); a resolve's
  clears write it (`edram_clear.comp`: RB_DEPTH_CLEAR, or RB_COLOR_CLEAR with
  RB_COLOR_CLEAR_LO first at 64 bits per sample). No render targets are made,
  no EDRAM transfers, no host depth beside EDRAM.
- Occlusion queries: the shader adds the samples that passed depth and
  stencil to a counter (binding 6), a counter per report in each
  submission's share, zeroed as the submission begins and copied for the CPU
  as it ends (Xenia's FSI_AddMSAASamplesToZPD).
- The draw's descriptor set has three more bindings: the state's uniform
  block (`FsiConstants`, binding 4), EDRAM (5), the counters (6). Passes have
  no attachments, one a sample count.
- The host path's approximations and their options do not apply: D24FS8 is
  20e4 itself (`depth_float24_round`, `depth_float24_convert_in_pixel_shader`
  and the host depth beside EDRAM are host render targets' answers to its
  32-bit float), gamma targets blend in linear and are encoded after
  (`gamma_render_target_as_unorm16`), float targets keep their bits without
  UINT views, and the "D24FS8 depth as 32-bit float" and gamma counts are not
  raised.

**Beyond Xenia: the state as specialization.** Xenia's TODO says its path is
slow mainly because everything in it is dynamic. Here the formats, write
masks, blending and the depth and stencil tests' state (edram_fsi.frag's
`constant_id` 0-9) are the pipeline's specialization constants, so the driver
drops every branch that does not apply (`Options::fsi_specialized`;
`XF_FSI_DYNAMIC=1` makes it Xenia's, all read from the uniform block). The
pipelines keyed by it are as many as host render targets' (the race frame:
97 against 96; dynamic 91).

**Where it differs from Xenia.** The depth and stencil test is always late,
after the shader (Xenia tests early, holding the interlock through the
shader, where the shader neither writes depth nor exports); it measured
faster than host render targets without that. EDRAM bases leave the window
offset out, as the host path does (Xenia's newer `GetWindowOffsetTiles`).
The keep masks are made in the shader from the format and the write mask.
No resolution scale.

**Measured, and the default.** Xenia's exact pictures of the 29 frames were
made with its trace dumps (`--render_target_path_d3d12=rov`,
`--render_target_path_vulkan=fsi`, in `../traces/dumps/rov` and
`rov/d3d12`); `replay_test` compares with them when the renderer takes the
interlock path (`render::render_target_path`). Against them all 29 pass:
0.000-0.006 of 255 but game F's menu (0.083, as the host path's 0.082 against
Xenia's default) and game A's park and race (0.007 and 0.008 off the
Vulkan FSI picture; Xenia's ROV picture is 3.08 and 1.75 off its own FSI one,
the anisotropic filtering above). Where exactness shows, against Xenia's
default pictures: Game E 0.25-0.27 (0.005 against ROV), game B's 1800
and 3600 0.043-0.051 (0.000), the park and race 0.554 and 0.245 off Vulkan's
(0.007, 0.008). Game G's 850 and 950 (not in `replay_test`):
0.112 and 0.118 off Xenia's Vulkan FSI picture, 0.247 off its ROV one
(Xenia's two are 0.196 apart; without the Xenos sample positions they were
1.11, edges of the 2x MSAA scene); host render targets are 1.27 and 1.36 off
those. With `XF_FSI_DYNAMIC=1` and with `XF_LINKED_PIPELINES=1` the pictures
are the same; validation is clean on both paths.

GPU time a frame (`xfreplay --gpu-time --repeat 30`, twice interleaved, the
texture loads' time equal across the three: same clocks):

| trace | host render targets | interlock | interlock, dynamic |
| --- | --- | --- | --- |
| Game A, race | 6.01-6.22 ms | 4.59-4.75 ms | 5.06-5.28 ms |
| park | 4.46-4.50 ms | 3.11 ms | 3.39-3.43 ms |
| Game B, 3600 | 1.81-1.84 ms | 1.65 ms | 1.81 ms |
| Game G, 850 | 15.08-15.13 ms | 13.43 ms | 14.66-14.74 ms |

The EDRAM transfers go (the race's stores and loads, the stencil's
bit-by-bit loads without stencil export), and resolves read EDRAM rather
than copying images; without early depth the draws themselves cost about
what they did. Live (`boot_<game> --render --rates`, `XF_PIPELINES=library`
as the player, `XF_GPU_TIME=1`, game A's press script to the race,
600 s; game B 240 s; nothing else on the machine):

| | host render targets | interlock |
| --- | --- | --- |
| Game A: title, menus and park, race (swaps/s; the cap is 30) | 30.0, 29.8, 29.9 | 30.0, 29.8, 30.0 |
| renderer busy in those phases | 4%, 17%, 23% | 3%, 15%, 18% |
| renderer's time, GPU time (600 s) | 128.4 s, 93.6 s | 98.9 s, 93.4 s |
| hitches: frames over 4 ms making pipelines, swaps over 50 / 100 ms apart | 5, 26 / 6 | 0, 23 / 6 |
| Game B, 70-150 s: swaps/s (vblank), renderer busy; GPU time (240 s) | 60.0, 5%; 19.7 s | 60.0, 3%; 21.5 s |

The interlock path is the default where the device has fragment shader
interlock: it matches Xenia's exact pictures as closely as host render
targets match its default ones, is faster in every replay, and costs the
same live (both at the games' caps) with a quarter less of the renderer's
time. Its pipelines' shaders are larger: a cold driver cache made the first
interlock run of game A spend 30.6 s making pipelines (boot's
default, waiting for them), a warm one 0.01 s with libraries.

**Fixed on the way (both paths):** a 64-bit target's row of tiles is twice
the 32-bit one, rounded at 32 bits (xenos.h's GetSurfacePitchTiles); it was
rounded at 64 bits, which differs for pitches whose samples are not a
multiple of 80. No traced frame has such a pitch.

## Resolve targets, and Xenia's approximations closed (2026-11-08)

**Resolve targets** (beyond Xenia; `Options::no_resolve_targets`,
`XF_RESOLVE_TARGETS=0`, `XF_VK_NO_RESOLVE_TARGETS=1`). Xenia's resolves
write memory, and every texture over what they wrote is loaded again from
it at its next use; in game A's race those loads were about 14% of the
GPU's frame. Here a resolve also writes the texture a later draw reads over
its destination, and that texture is not loaded again. A texture is made a
resolve target when it is loaded from memory the GPU wrote and is one tiled
2D level of 32- or 64-bit texels whose image holds the guest's bits as they
are (a plain copy load, no 16-bit fallback): its image is then storable
through an `R32_UINT` or `R32G32_UINT` view (mutable, extended usage), in
one of 64 places per size of the resolves' set 1 (partially bound, updated
after binding). A resolve feeds up to two of them (the push constants'
room) that are current (not stale), with the destination's base (the
`RB_COPY_DEST_BASE` address less the x offset Direct3D carries in it), pitch,
texel size and endianness, whatever their format: a 16_16_FLOAT target read
back as 16_16_EXPAND is the same bits. `resolve.comp` stores each pixel's
words before the destination's byte swap at texel `texel_x0 + dest_x + x`
(`resolve::Info::texture_base`, `texel_x0`, `texel_y0`), bounds checked;
the rest of the texture is memory the resolve left as it was. Textures over
the same memory with another pitch or texel size, depth (24_8, not a
copy), and a third alias are loaded again as before. Needs
`descriptorBindingStorageImageUpdateAfterBind` and storage image array
indexing (the RTX 5060 has both); without them the resolves write memory
alone.

Game A's race trace (`xfreplay --gpu-time --repeat 10`, the fastest play
of each run; three runs each way, interleaved, the machine otherwise idle
but for another agent's boot): 4.620 ms of GPU a play without (4.540-4.644),
4.198 with (4.198-4.200), -9% (the trace's front buffer read-back, 0.50 ms,
is the trace's own); its texture loads after resolves from 13 (0.62-0.64 ms)
to 5 (0.23 ms) a play, the resolves' own cost 0.273 to 0.292 ms; the
picture unchanged (0.008 off Xenia's Vulkan FSI picture), validation
clean. Xenia's "base and mips loaded apart" was measured first: every
texture the race loads after a resolve is one level, so it would gain
nothing there and is left.

**Vertex data and the CPU's decodes.** Two costs of the renderer's thread:
- *Every texture loaded on the GPU* (`XF_TEXTURE_LOADS=cpu` for the CPU's
  decode of memory the GPU did not write, as before): guest memory's pages
  go into the mirror as for a vertex stream, and `texture_load.comp` makes
  the texture, as it already did over a resolve's memory and for the DXT
  fallbacks (the same loads, byte for byte: `texture_gpu_test`). The race
  trace's first play (282 new textures; three runs each way, interleaved):
  the renderer's thread 0.866 s (0.771-0.868) with the CPU's decodes, 0.760 s
  (0.740-0.785) without, its 0.061 s of decoding gone; the GPU's first play
  32.3 ms (31.3-32.6) against 36.2 ms (35.2-36.7), the raw pages uploaded and
  converted on the GPU rather than the decoded texels uploaded. The GPU being
  idle most of a frame and the renderer's thread the one hitches come from,
  the GPU's way is the default.
- *A vertex stream's pages checked once* (`Impl::RequestMemo`): a draw
  requests its streams' pages from the mirror, which looked at every page of
  them (an atomic load each) at every draw, and built the list of their CPU
  parts for a trace writer whether or not one was there. Now the ranges last
  found current are remembered with the write count and a mirror epoch: while
  nothing was written since, a draw with the same streams checks no page.

Game A live (`boot_<A> --render --rates --seconds 480`, the press script to
the race above, `XF_GPU_TIME=1`; one run each, so these are rough: other
agents' builds and a boot shared the machine): without resolve targets, the
GPU decodes and the memo, then with all three:

| | before | after |
| --- | --- | --- |
| swaps, draws | 14,342, 8.19 M | 14,180, 8.10 M |
| GPU time | 68.97 s (4.81 ms a swap) | 66.07 s (4.66 ms a swap) |
| of it texture loads / CPU decodes' uploads / mirror uploads | 6.78 / 1.22 / 1.11 s | 2.85 / 0 / 1.88 s |
| texture loads after resolves | 165,693 (424 a second in the race) | 53,080 (150-165 a second) |
| the renderer's thread | 97.8 s: 5.2 vertex data, 2.1 decoding, 3.7 resolves | 74.7 s: 3.2, 0, 2.9 |

**Xenia's approximations, closed or explained** (the "Approximated" list):
- **Polygon modes** (`PA_SU_SC_MODE_CNTL` poly_mode, front and back types):
  as Xenia's Vulkan pipeline cache, one mode for both faces, the lowest type
  of the faces not culled; points and lines through `fillModeNonSolid`
  (optional, `XF_VK_NO_FILL_MODES=1`; without it filled and counted). The
  mode is in the pipeline key.
- **Point sprite coordinates** in the pixel parameters: the point
  expansion (point_list.geom, or the vertex shader's) writes the corner's
  coordinates at location 17 and a point list's pixel shader
  (`PixelOptions::point_coordinates`, Xenia's `param_gen_point`) puts them,
  clamped to 0-1, in the parameters' z and w. The hardware's order and
  sampling point stay a hardware question (Xenia's TODO C19).
- **YCbCr border colors**: (Cr, Y, Cb) and (Y, Cr, Cb) black, through
  `VK_EXT_custom_border_color` without a format, as Xenia's Vulkan cache
  (`XF_VK_NO_CUSTOM_BORDER=1`: black, counted); `force_bc_w_to_max` (dword
  5 bit 2) gives the border an alpha of 1, as Xenia's both caches.
- **Samplers past the bindless table**: a full table (1,024) destroys the
  sampler least recently used by a finished submission and takes its place,
  as Xenia keeps its samplers within the device's limit; only with all in
  use by running work is the stand-in taken, counted.
- **An ALU operand from a fetch constant**: not something the microcode
  can say. An ALU operand is a register or a float constant by its source
  select bit, all the parser makes of it (as Xenia's); the translator's
  check stays as an internal one.
- **Vertex fetches from a texture fetch constant**: drawn with the
  constant's bits read as a vertex constant, as Xenia (its
  `gpu_allow_invalid_fetch_constants`, on by default); what the fetch unit
  does with the type bits only hardware can say. Counted.

**The disc's shaders the build step rejected** (29 of game A's 88): all are
vertex shaders as Direct3D keeps them in a shader container, before binding:
their full vertex fetches have no format (0) and no stride, because Direct3D
writes each fetch's format, stride and offset into the microcode from the
vertex declaration when it binds the shader (the container lists the
fetches by usage; XenosRecomp maps them to host vertex inputs for the same
reason). The copy a draw runs is that patched one, under another hash,
which the runs record. `ucode::awaits_vertex_declaration` tells them; the
build step counts them apart ("vertex shaders awaiting a vertex
declaration") and translates the 59 pixel shaders, all of which translate.

## The standing benchmark against Xenia (2026-11-08)

The bar is Xenia, beaten on the same scene: frame time and GPU time a frame,
ours against xenia_canary's Direct3D 12 backend, live, on scenes both reach
without input (title screens, attract loops, the first seconds). Direct3D 12
is Xenia's best renderer, so it is the bar (the owner, 2026-11-08); its
Vulkan backend stays only where it is a trace's accuracy reference.

**The method** (`tools/bench/`, Windows only, as Xenia is). PresentMon and
any ETW tracing need an administrator or the Performance Log Users group,
which the machine's account is not; GPU timestamps are not something Xenia
gives. So both programs are timed from outside by the same code:
- `xfbench [--seconds N] [--warmup S] [--step S] [--timeline] [--vk | --dxgi]
  -- <program> [args]` starts the program (in a job object, killed at the
  end) and times the window from `--warmup` to `--seconds`.
- **Frames**: `xfbench_hook.dll` records the time of every present.
  `--vk` loads it as a Vulkan layer (`VK_LAYER_XF_bench`, its manifest
  beside it, through `VK_ADD_LAYER_PATH`) wrapping `vkQueuePresentKHR`;
  `--dxgi` injects it into a Direct3D 12 program before its main thread runs
  (a remote `LoadLibraryW`, then its `xfbench_hook_dxgi`), which patches the
  DXGI factory class's `CreateSwapChainForHwnd` so that every swap chain's
  `Present` and `Present1` are wrapped. Both programs present once a new
  frame (ours: the player's window shows only fresh pictures, Xenia's
  presenter paints on a guest output refresh), so presents are frames.
- **GPU time**: the process's running time on each engine as Windows'
  scheduler counts it, the "GPU Engine(pid_*)\Running Time" performance
  counter (100 ns units; checked against a replay's own utilization), the
  same accounting for our Vulkan and Xenia's Direct3D 12. Reported as ms a frame and a
  second, the 3D and compute engines summed, copy apart. It includes all the
  process's GPU work: the presenter's, the transfers, the idle gaps the
  driver bills.
- **CPU time**: the process's user and kernel time a second.
- `--timeline` prints each `--step`'s frames a second and GPU time a frame,
  to find a run's phases and pick a window both programs spend in the same
  scene.

`tools/bench/bench_vs_xenia.ps1 -Disc <iso> -Ours <player> -Xenia <exe>
-Seconds S -Warmup W -Runs 3 -Configs ours,xenia-d3d12 [-Timeline]` runs the
configurations in turn (A B A B ...), appends one line a run and prints the
median and the spread of each. `ours-host` forces our host render targets;
`xenia-d3d12-rov` is Xenia's exact EDRAM path (its default is host render
targets). Xenia's defaults
otherwise (vsync on, no resolution scale, its shader storage warm). The load
is read before each set (`Get-Process clang*,ninja*,...`); another agent's
builds or games on the machine are named with the numbers.

**Results** (RTX 5060, Ryzen 5 5600G; median of 3 runs, spread in
brackets; GPU ms a frame by the counter):

| scene (window) | ours (interlock, the default) | ours, host targets | Xenia D3D12 (host targets, its default) |
| --- | --- | --- | --- |
| Game B, 3D title scene (90-130 s, 60 fps cap) | 1.39 (1.38-1.39) | 1.53 (1.52-1.56) | 0.83 (0.83-0.85) |
| Game A, attract loop (120-160 s, 30 fps cap) | 4.90 (2.16-5.04) | not run | 5.64 (1.99-6.17) |

Game B: Xenia's exact path (ROV) 1.26, one run, against our 1.39 on ours.
Every configuration holds the cap (frame time medians 16.6-17.0 ms); CPU
1.33 (1.33-1.49) s a second ours, 1.24 (1.20-1.29) Xenia's. Load: an xfbuild
of another disc (2 jobs, below-normal priority) during the first runs, an
idle machine otherwise. Before the resolve fix below, one run: 1.46.

Game A: its attract loop alternates a 3D scene and lighter ones, at times
that differ run to run, so the window is not always the same scene (the
low ends of both spreads are runs that were in the lighter one). Where the
timelines show both in the 3D scene (one run each with `--timeline`,
120-180 s): ours 4.80 ms a frame, Xenia's 5.94 (19% less); 30 fps both,
frame time median 33.3 ms both, CPU 1.33 against 1.30 s a second. A
standing scene for game A needs input (the race) or a fixed point of the
loop; until then its numbers are rough.

Not measured: games D and H (their libraries were rebuilt for the interface
of the time, stale after the merge that changed it); game G (no library).

Where we lose and why, for game B:
- Our default is the exact EDRAM (interlock), Xenia's is host render
  targets. Exact against exact we lose by 10% (ROV 1.26); the interlock's
  draws cost about twice the host's (the scene's
  32 2x-MSAA draws: 0.55 against 0.25 ms in the 3600 trace's replay), the
  price of exactness.
- Our host path loses to Xenia's host path because its ownership transfers
  go through the EDRAM buffer, a store by the holder and a load by the new
  target (0.50 ms of the replay's frame, eight passes), where Xenia's
  render target cache draws the holder's image straight into the new target
  (one pass, no EDRAM buffer). Porting Xenia's direct transfers is the next
  step for the host path.
- Fixed here: a texture the game renders to every frame was loaded again
  every frame (0.15 ms of the live frame): a second, smaller surface is
  placed inside the first one's memory, 384 rows down, and its resolve made
  the first texture stale. A resolve now feeds a target texture whose memory
  holds the destination a whole number of 32-row macro tile rows in (the
  texel rows offset by as many), and a stale target the resolve covers
  whole is current again. Game B's 3600 trace: 29 loads after resolves a
  run to 0, 87 resolves writing the texture they feed instead of 58, GPU
  1.483 to 1.426 ms a play, the picture unchanged (0.000 off Xenia's ROV).

**Screen extents: what the guest does with them** (game G's 850 trace,
read packet by packet). Direct3D's predicated tiling draws the scene twice,
once a tile (`SET_BIN_SELECT` 0x80000003, then 0xC). In the first pass every
draw is followed by an `EVENT_WRITE_EXT` (`SCREEN_EXT_RPT`, 26) and an
`EVENT_WRITE` (the next `SCREEN_EXT_INIT`), both predicated on bin 31, so
they run in the first pass alone: 1,219 extent reports into an array of
16-byte records. The pass ends with an `INTERRUPT` and `WAIT_REG_MEM`s (also
bin 31): the CPU reads the extents and patches the `SET_BIN_MASK_LO`
payload in front of each draw in the command buffer, which the second pass
then runs. With Xenia's whole-screen answer every mask is 0x8000000F (all
four bins), so every draw runs in both passes; with real extents a draw
inside the top tile would get 0x80000003 and be skipped in the second.
So exact extents change no picture (the CPU only narrows the bins a draw
runs in), only how many draws the second pass makes: up to half of game G's
3D draws. What it would take: the extent of each draw between an INIT and
a RPT (its clipped primitives' bounding box, in 8-pixel units as Xenia's
`>> 3` reads them), in guest memory before the CPU reads it after the
interrupt: the vertex shader's positions bounded on the GPU (an atomic min
and max a report, as a pipeline variant) and the interrupt held until that
submission is done, or the positions bounded on the CPU (the shader
interpreter Xenia's draw extent estimator uses, too slow for 1,219 draws of
hundreds of vertices). The rounding of the 8-pixel units and the z range
(Xenia writes 0 and 1) are hardware questions; a bounding box rounded
outwards is safe, since a wider extent only keeps a draw in more bins.
Open; the gain is only measurable live in game G, whose library is not
built for this interface.

**Game G's frames 850 and 950, what is left** (0.149% and 0.150% of pixels
off Xenia's ROV picture; Xenia's ROV and FSI pictures are 0.083% apart).
Of the 1,374 pixels off ROV's, 1,070 are off Xenia's FSI picture too where
Xenia's two agree; 895 of those are in the ground's band (rows 520-600:
small foliage drawn with alpha to coverage), 175 elsewhere. Above the band
ours is nearer Xenia's FSI picture (0.17 of 255 mean, no bias: +0.07,
+0.07, +0.06 signed) than Xenia's two pictures are to each other (0.54). In
the band the foliage's covered pixels differ in shape, about as many
brighter as darker (497, 573): coverage, not color. Alpha to coverage
itself is Xenia's code; what feeds it (the alpha of DXT5 foliage
textures at a distance: the filtering, the mip chosen, the packed mip
tail) is where to look next, draw by draw, which needs a replay that stops
after a given draw against a reference that can too (Xenia's trace viewer).

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
  (game A XDK 21250, game B XDK 8276; `xfinspect` prints it). A
  database of the D3D functions' signatures per XDK version names them in
  any game, so hooks on `D3DDevice_*` (render target roles, the back buffer,
  viewport and aspect) come without reverse engineering each game: the way
  to widescreen, a separate UI and resolution independence.
- **The engine** (the native-renderer port model): a port's renderer receives the device
  and the frame's command buffer and replaces passes (UE3's scene, Scaleform
  UI); the generic renderer keeps everything else. F6 switches between the
  two for comparison. In since 2026-11-08: below.

## A port's own renderer (2026-11-08)

`xenoforge/native_render.h` (docs/sdk.md, "Native rendering"; plan E4). The
renderer stays general: it calls what a port registered and skips what the
port's filter selects; nothing in it knows a game.

- **Where.** `Impl::swap`, before the front buffer is taken:
  `native_frame` takes the port's settings (copied again only when their
  version moved: one relaxed load a swap otherwise), and when an output
  renderer is set and the toggle is on, makes the output image
  (`R8G8B8A8_UNORM`, remade on a resize, the old one destroyed when its
  submission is done), records a full barrier and calls the port with a
  `NativeFrame` (the command buffer through `outside()`, the texture cache's
  `texture()` and `texture_view()`, the arena). After it, `bound_pipeline_`
  is reset so the draws bind theirs again; the swap's `flush()` follows. A
  frame the port drew is handed to `on_swap` as the front buffer, with an
  identity display table (the presenter's conversion leaves it as it is), and
  the guest's front texture is not loaded unless a read-back wants it.
- **What is skipped.** `native_suppress_` is set for the frame after one the
  port drew. In `draw`, after the check for draws that write nothing (so the
  memexport test is already made): a draw exporting memory is kept and
  counted; any other goes through the filter (`native_keep`) and, skipped,
  marks its occlusion report `suppressed`, answered with at least the port's
  fake count (1000, Xenia's `query_occlusion_fake_sample_count`, as the
  rexglue fork answers while it suppresses). In `resolve`, first thing. The
  command processor is untouched: packets, registers, fences, events,
  interrupts and swaps run as ever, since the skipping is in the Sink.
- **Held frames.** The decision is a frame late (the callback's answer comes
  at the swap, after the frame's draws). A frame whose passes were skipped
  and that the port did not draw (it yielded, or the toggle went off) has no
  picture: `on_swap` is not called, the window keeps the port's last, and the
  next frame is emulated in full. Read-backs (`frame_wanted`, boot's
  `--shots`) are always the guest's front buffer.
- **Without a port.** Nothing registered: one relaxed load and two branches a
  swap, one branch a draw and a resolve; replay_test is unchanged.
- **Measured** (game B's first 30 s, its intros and logos: about 5 draws and
  3 resolves a swap, guest-paced at 60 then 32 swaps a second;
  `examples/native_port`, `XF_GPU_TIME=1`). Boot with `--render`: emulated,
  1102 swaps, 1.87 s of GPU time and 0.8 s in the renderer's calls; the
  port's frames with everything skipped, 1116 swaps, 0.02 s of GPU time (the
  port's frame 17 µs a swap) and 0.2 s, 5389 draws and 3347 resolves skipped;
  the port drawing over the emulated work (`suppress=false`), 1105 swaps,
  2.39 s and 1.0 s. The player: 37.3 swaps/s either way (the guest sets the
  pace), 1.98 s against 0.04 s of GPU time. 0 interpreted entries; the window
  shows the port's frames (window screenshots), with `yield_every=200` the
  two alternate and each switch to emulated holds one frame (3 in 30 s).
  Validation (`XF_VK_VALIDATION=1`): no message.

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
