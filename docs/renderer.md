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

**Guest memory on the GPU: the mirror (B4, 2026-11-05).** Xenia keeps all
of guest memory on the GPU (its shared memory) and learns of the CPU's
writes from page watches. XenoForge has no page watches yet, and guest
memory is host memory the renderer reads where it lies; so its copy on
the GPU (512 MB, device-local) holds what the GPU writes and what reads of
that need around it.
- A resolve runs on the GPU: the rectangle copied from its render target
  into a scratch buffer (to one sample first when multisampled, as before),
  then `resolve.comp` (resolve.cpp's write, ported with its arithmetic kept
  exact) writes it into the mirror, tiled. The 32x32 tiles the rectangle
  cuts at its edges get guest memory's bytes first, so the rest of them
  is the CPU's (`resolve::destination_ranges`).
- Until guest memory has it, what a resolve wrote is pending: textures over
  it are loaded from the mirror; a draw whose vertex data lies there reads
  the mirror instead of the arena; a front buffer read back and textures
  in formats only the CPU decodes (DXT3A, CTX1) wait for it.
- Guest memory gets it once the GPU is done, without waiting: the resolve's
  ranges are copied into a read-back buffer on the GPU, and into guest
  memory at the first call of the processor's that finds the submission
  done (its textures are then checked by hash again). A later resolve of the
  same range takes over (only the newest bytes are copied); the processor's
  own writes there (`memory_written`, a trace's memory) cancel it.
- Why copy it back at all rather than when the CPU reads it (the page
  guards of "EDRAM as render targets" above): nothing tells the renderer
  when the CPU reads, and a game reading a resolved surface (a screenshot,
  a save's thumbnail, data rendered on the GPU) must find it there. Xenia's
  default (`readback_resolve=fast`) copies every resolve back too, a frame
  late; here it lands when the GPU is done. With page guards it can become
  lazy. Until then, the CPU writing a range between a resolve and its copy
  back loses that write (Xenia's fast readback has the same window, a frame
  long).

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

**The rest, as games ask:** memexport (shaders writing guest memory, then
what it overlaps invalidated), rectangle and quad lists and point sprites
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
  arena; indices swapped and offset on the CPU (quad lists split,
  strips restarted at the reset index); rectangle lists and point lists
  through geometry shaders (points as sprites of the vertex shader's
  clamped oPts.x or PA_SU_POINT_SIZE, as Xenia's); viewport, scissor and
  the position's formats as Xenia's `GetHostViewportInfo` and
  `GetScissor`; blend, depth, stencil, culling and the alpha test from the
  registers. A draw that can write nothing (no color channel enabled,
  depth and stencil off, no memexport) is dropped: occlusion queries are
  the processor's fake counts, so nothing could see it.
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
  (color bits too) survives the host's 0-1 depth whole. The trace's EDRAM
  snapshot is where EDRAM starts (zeros, live).
- **Textures** decoded from guest memory (`texture.h`: 1D, 2D, stacked, 3D,
  cube, tiled or linear, packed mips, the block formats kept compressed;
  depth formats as 32-bit floats, the EXPAND formats as 16-bit floats, as
  Xenia's texture cache loads them), with the fetch constant's swizzle and
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
resolves of MSAA targets; the display's piecewise linear gamma; signed,
biased, gamma and exponent-adjusted textures; memexport; unnormalized cube
map coordinates; point sprite coordinates (pixel parameter generation);
float16 and float32 targets moved through EDRAM by value rather than by
bits (NaN payloads).

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
`texture_address.h`, `texture_util.h`, `draw_util.h`, `primitive_processor.h`,
`spirv_shader_translator*.cc`, `shader_interpreter.h`, `trace_protocol.h`,
`trace_reader.h`, `trace_dump.h`; `vulkan/` for how Xenia drives Vulkan.
