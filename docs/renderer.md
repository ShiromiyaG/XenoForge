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
                           textures: decoded by compute, keyed by fetch constant
                           guest memory: one buffer, pages watched
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
  again compiles nothing.
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
  endianness, format, the optional clear) and marks those pages as written
  by the GPU; the texture cache then uses the image directly, and memory gets
  the bytes only when the CPU reads them (page guards).
- A resolution scale applies from the start: images at N times their size,
  resolves keep a scaled copy for sampling, the CPU sees them scaled back.

**Textures decoded on the GPU.** Keyed by the fetch constant (address,
format, size, tiling, endianness, mip address); untiled and decoded by
compute shaders, one per format family, from a table of formats; dropped
when the CPU writes their pages (page guards over guest memory). HD texture
packs hook in here, as in the 3DS runtime.

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
the registers and gamma ramps restored). Every draw is one Vulkan
submission, waited for: plain, not fast.

- **Draws**: the translated vertex and pixel shaders (colors scaled by the
  target's exponent bias, 16-bit fixed targets kept as snorm with their
  -32..32, gamma targets written through the piecewise linear curve); guest memory a 512 MB
  storage buffer the vertex fetches read, what changed copied over before
  each draw; indices swapped and offset on the CPU (quad lists split,
  strips restarted at the reset index); rectangle lists through a geometry
  shader; viewport, scissor and the position's formats as Xenia's
  `GetHostViewportInfo` and `GetScissor`; blend, depth, stencil, culling
  and the alpha test from the registers.
- **Render targets** stand for EDRAM as host images, one per base, pitch,
  MSAA and format, as many rows as EDRAM holds from the base. EDRAM itself
  is a 10 MB buffer in Xenia's layout (`edram.glsl`, from `edram.xesli`):
  when a target needs tiles another holds (down to the scissor's bottom),
  the holder stores them into EDRAM in the guest's bits and the new one
  loads them, in any format and MSAA, as Xenia's ownership transfers do;
  depth tiles have their halves swapped; the stencil is exported, or
  written a bit per pass where the device cannot export it (NVIDIA). The
  trace's EDRAM snapshot is where EDRAM starts (zeros, live).
- **Textures** decoded from guest memory (`texture.h`: 1D, 2D, stacked, 3D,
  cube, tiled or linear, packed mips, the block formats kept compressed),
  with the fetch constant's swizzle and sampler; each is kept with a hash
  of the memory it was decoded from (the spans the decoder read) and
  decoded again when that memory differs at a use, or at once when the
  processor writes over it (`memory_written`).
- **Vertex data**: before each draw, the ranges of the vertex fetch
  constants the shaders use (address and size, as Xenia's IssueDraw
  requests them) are copied from guest memory to the GPU's copy of it.
- **Resolves** read the rectangle back (MSAA averaged), write it into guest
  memory as the destination says (`resolve.h`), then clear.
- **The swap** shows texture fetch constant 0 through the processor's gamma
  table, as Xenia's presenter does, for the swaps asked for (numbered from 0
  as Xenia numbers a trace's frame).

Nothing tells the renderer when the CPU writes guest memory (Xenia watches
pages with memory protection), so what it copies is checked or copied
again at each use: textures by their hash, vertex data and indices copied
at each draw.

Against Xenia's trace dumps (`replay_test` with `XF_TRACES` and
`XF_DUMPS`): all ten frames of the two games within 0.013 of 255 on
average, no pixel off by more than 16: El Chavo Kart's video (three planes),
Ben 10's intro, and Ben 10's title, drawn in two predicated tiles of a 2x
MSAA target that the game clears by drawing into the same samples seen as a
640 4x target.

Not done yet, counted where it shows: MSAA resolves of one sample; depth
resolves of MSAA targets; the display's piecewise linear gamma; signed,
gamma and exponent-adjusted textures; memexport; point lists; float16 and
float32 targets moved through EDRAM by value rather than by bits (NaN
payloads).

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
  same Sink and match.

What the live runs ask for and the renderer does not do yet, counted:
point lists (168,840 of El Chavo Kart's 202,089 draws in its first 1,887
swaps, past the movie), texture fetch offsets in pixel shaders and stacked
(array) textures (about one each a frame on its title screen, which comes
out in red alone, its green and blue lost: no trace of that screen yet to
tell which gap it is), D24FS8 depth kept as 32-bit float, Ben 10's texture
slots without a texture constant.

The cost (runs of the same length back to back, other work sharing the
machine): El Chavo Kart is bound by the interpreter, and rendering on the
processor's thread costs it nothing measurable (747 swaps in 90 s headless,
742 rendered; the renderer busy 24 s of the 90). Ben 10 is bound by the
renderer: 59.4 swaps a second headless (vblank's pace), 18.4 rendered, the
renderer busy 39 s of the 45 after the first swap, 30 s of them in its
three resolves a frame (12 ms each) and most of the rest in its 72 draws a
frame. Every draw is a submission waited for and every resolve is read
back and tiled by the CPU, as planned for traces; batching a frame's draws
into one submission and resolving on the GPU (B4) are what would take this
away.

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
