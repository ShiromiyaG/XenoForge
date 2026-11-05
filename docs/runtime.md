# The runtime (design, 2026-11-04)

Plan step A4: the guest's memory, the loader, the kernel and `boot`, so
the recompiled code (and the interpreter) run a game to its title loop
without a picture, every kernel call counted. Xenia (`../xenia-canary`,
BSD) is the reference for what each export does; the structure is written
anew, as lean as the games allow ([lean-kernel.md](lean-kernel.md)).

## Memory

`memory.h`: Xenia's map, one shared-memory section viewed several times
in a 4 GB host range, so a guest address is an offset from the base (plus
0x1000 at 0xE0000000 and above); heaps hand out pages as the kernel's
allocators ask. The generated code and the interpreter reach memory through
`ops::host`, which applies that offset. A 32-bit load or store at the
devices' registers (0x7FC80000 and up) reaches the device that registered
the range (`Kernel::add_mmio`): the GPU's, 0x7FC80000-0x7FC8FFFF, and the
XMA decoder's, 0x7FEA0000-0x7FEAFFFF, as Xenia maps them (see "The GPU"
and "Sound").

## Code: generated, interpreted, imported

One rule for every call the code cannot make directly (`dispatch`): the
address is looked up in the generated code of the module that holds it; an
import thunk runs its export; anything else runs in the interpreter until
it returns. The interpreter's own `bl` to an address with a generated
function calls it. So a game runs with all its code generated, with none
(bring-up, before the build), or with a module whose image is not the one
its code was made from (a title update, another version) interpreted.

The generated code is per module (`ppc_module.h`; docs/recompiler.md,
"Modules"): the game library carries a table for each executable and DLL on
the disc (its path, base, size, the digest of its image and its functions),
handed to the kernel before the load (`Kernel::set_generated_modules`).
When an image is placed at its base (the executable in `load`, a DLL or a
second executable as XexLoadImage loads it), the kernel looks for the table
with the same base and size and, only then, digests the image as unpacked
(`image_digest`, before the imports are bound; a few milliseconds for
30 MB) and compares (`add_module_code`); the module's report line says
which: "generated code, N functions", or why it runs interpreted. A table
that matches gets the module's range and a slot per instruction from its
first function to its last ((address - first) / 4: 36 MB for El Chavo
Kart's 4.7M-instruction range, made the first time the image loads and
kept). Dispatch reads one immutable map of those ranges (an atomic
pointer, a new map published at each load and unload, the old ones kept
for threads still reading them): one compare per module loaded and one
load, about 2 ns, where the binary search over one table it replaced took
29 to 79 ns on El Chavo Kart's 73,293 functions. XexUnloadImage drops the range with the pages,
so BC2's Splash.xex and Game.xex, one after the other at 0x82A00000, each
run their own code. Functions given with `set_generated` (any address, the
tests' host functions) are looked up first.

Returns to another frame (longjmp, a C++ catch's continuation, a fiber
switch: the functions discovery leaves to the interpreter for it) are the
runtime's: each dispatch knows the stack its call was made on (r1, its
"stack top") and the return address; the interpreter it runs treats a
`blr` elsewhere with r1 above that stack top as an unwind
(`Stopped::Unwound`), and the dispatch throws it (a C++ exception, through
the generated code's frames, as ExitThread's is). The first dispatch whose
stack top is at or above the restored r1 catches it and interprets on from
the target: the rest of the functions between, interpreted (their generated
frames are gone, and the generated code keeps nothing outside the context,
so the interpreter takes over exactly), until the guest returns to that
dispatch's return address with r1 back at its stack top, and the generated
code after it goes on. A return to that address with r1 below it is a
recursive call's, not this one's. A switch back to a fiber lands on the
dispatch that left it (its saved LR and r1 are that call's), which then
returns normally. With r1 above every frame of a call the kernel made (a
fiber stack above the thread's), the call interprets on until the guest
returns from it, and so does any switch thrown up to it meanwhile (Reach's
fibers, after their first switch, run so: correct, and slower than their
generated code). Mid-function jumps the generated code dispatches with a
stale LR (an unresolved table case) end the same way, where before they
could interpret past their function forever.

A game library's import thunk (a DLL's function another module imports:
`lis r11; ori r11` before the thunk's own `mtctr r11; bctr`, written at
load) is known as such to the kernel: dispatch to it sets r11 and CTR as
the thunk would and dispatches to the export, so generated code calling
across modules reaches the other module's generated code without
interpreting the thunk.

The loader rewrites each import thunk (16 bytes) to `sc` and `blr`: the
interpreter reaches the export through `sc` (the kernel looks the thunk up
by the address), generated code through `dispatch`. Variable imports
receive the address of the kernel's object for them.

## Exports

`kernel.h`: an export is a function of the kernel and the calling thread's
context, registered by library and ordinal:

```cpp
XF_EXPORT(xboxkrnl, NtAllocateVirtualMemory) {
    uint32_t base_ptr = arg(c, 0), size_ptr = arg(c, 1);
    ...
    result(c, X_STATUS_SUCCESS);
}
```

Arguments as Xenia's shims read them: r3-r10, then the stack at
`r1 + 0x54 + 8 * (n - 8)`: a 32-bit argument (`arg`) is the word there,
where the caller's `stw` puts it (the games store VdSwap's ninth and tenth
at 0x54 and 0x5C), a 64-bit one or a vararg (`arg64`) the doubleword.
Every call is counted by export; an import with
no implementation logs once, counts, and returns 0, and `boot` prints both
lists: what the game calls is what gets written next.

## Modules, memory, Rtl

- **The guest globals** (`modules.cpp`, Xenia's KernelGuestGlobals and
  kernel_state.cc) are set up as the kernel starts (`setup_kernel`): the
  ten object types, the default object, the idle, title and system
  processes, the kernel modules (xboxkrnl.exe, xam.xex with their
  GetProcAddress trampolines) and the variables at Xenia's addresses
  (KeTimeStampBundle at 0x80240EE0, XboxHardwareInfo, the Vd* ones...).
  The load (`setup_modules`) adds the executable's module, its loader
  entry and header, and the title process's TLS and stack size. The
  objects and threads take the processes and object types from there.
- **Second executables** (`modules.cpp`, Xenia's LoadUserModule and
  XexModule): XexLoadImage reads a DLL through the file system (a bare
  name next to the executable), places its image at its base, makes its
  module (loader entry, header) and binds its imports as the executable's
  (`Kernel::bind_imports`), loading first the game's libraries it imports
  from; their thunks jump to the exports (lis/ori r11 before the thunk's
  own bctr). DllMain runs on the calling thread with the HMODULE:
  DLL_PROCESS_ATTACH at the load (the libraries it brought first),
  DLL_THREAD_ATTACH and DETACH as guest threads start and end,
  DLL_PROCESS_DETACH when XexUnloadImage takes the last load, which
  releases the pages (BC2 loads Splash.xex and Game.xex at the same base,
  one after the other). Every DllMain call runs under the loader lock
  (NT's), recursive, also held across XexLoadImage's lookup and load,
  XexUnloadImage's detach and removal and XexGetModuleHandle: a load waits
  for an unload in progress, and no DllMain gets a module that is gone.
  boot's module report names the lock's owner when it is held. Each
  module's code is generated or interpreted as its image matches a table
  of the game library ("Code" above).
- **Memory exports** (`memory_exports.cpp`): NtAllocateVirtualMemory and
  the Mm*/Ex* pools on the heaps of memory.h, with Xenia's addresses.
- **Rtl and misc** (`rtl.cpp`, `misc.cpp`): strings, time fields, status
  codes (Xenia's table), the printf family, XConfig settings (Xenia's
  defaults).
- **Crypto** (`crypt.cpp`): every XeCrypt*/XeKeys* export Xenia has, plus
  SHA-384 and XeKeysConsoleSignatureVerification (it trusts the console's
  own signatures, so the XDK's partition block check takes the normal
  path), with Xenia's guest context layouts, over algorithms written from
  their standards (sha1, sha2, md5, aes, des, bignum; no third-party code).
- **One clock**: the scheduler's (100 ns since the kernel started). The
  interrupt time is it, the system time is it plus the host's time at the
  start, the time base (mftb) is it times 5 (50 MHz); KeTimeStampBundle
  moves with it every millisecond once threads run. On Windows the host
  timer is raised to its finest (0.5 ms), as Xenia does, or the clock and
  short waits would step at 15.6 ms.

## Threads and objects

Each guest thread is a host thread with its own context, a guest stack, a
PCR (r13) and TLS as Xenia's XThread sets them up. Kernel objects (events,
semaphores, mutants, timers, threads) live in guest memory where the game
put them (KEVENT and friends) and are backed by host objects found by the
guest address, as Xenia's XObject does. The deterministic mode (fibers on
one host thread, plan "Threads") comes on the same interface later.

As built (`src/kernel/objects.h`; objects.cpp, threads.cpp, sync.cpp):

- **Handles** as Xenia's (0xF8000000 + slot * 4, -2 the current thread),
  names case-insensitive. A dispatcher header the kernel has seen holds
  Xenia's stash (`'XEN\0'` and the handle in its wait list), so the host
  object is found from the guest pointer, and one the code initialized
  itself (the XDK inlines KeInitializeEvent) gets one on first use.
- **The dispatcher is NT's, not the host's waits**: one lock over every
  object; the signal state is the guest header's own (the game reads it,
  and what it writes there counts); a signal satisfies the waits it can,
  oldest first (KiWaitTest), taking what they acquire (auto-reset events,
  semaphore counts, mutant ownership); wait-any, wait-all, timeouts
  relative and absolute, abandoned mutants, alertable waits.
- **The scheduler** is an interface of seven calls (start, join, block
  until woken or a deadline, wake, yield, the clock, timers changed). The
  host scheduler gives each guest thread a host thread with a 16 MB stack
  and a clock thread (timers, KeTimeStampBundle, every millisecond); the
  time base (mftb) is the same clock. The fiber scheduler replaces it.
- **Safe points**: a thread suspended by another, or with kernel APCs
  queued, stops at its next kernel call (Xenia suspends the host thread
  wherever it is). User APCs run in alertable waits; DPCs run at once on
  the calling thread, as in Xenia. ExTerminateThread unwinds the host
  stack to the thread's start (a C++ exception).
- `Kernel::launch` makes the main thread (the executable's stack size and
  TLS header) and starts it; `boot` waits for it and reports the threads.

## The GPU

The video driver's side of the GPU, live and headless (plan step A4, "GPU
live"): what the games' Direct3D needs to get through its initialization
and run its frame loop, nothing drawn. Translated from Xenia
(`graphics_system.cc`, `command_processor.cc`,
`pm4_command_processor_implement.h`, `kernel_state.cc`'s
EmulateCPInterruptDPC, `xboxkrnl_video.cc`, `xam_video.cc`); where it
differs is noted at the top of each file.

As built (`command_processor.h`, `src/gpu/command_processor.cpp`;
`src/kernel/video.h`, `video.cpp`):

- **The command processor** executes the ring buffer from its read pointer
  to the CPU's CP_RB_WPTR, wrapping, and the indirect buffers it points at:
  packet types 0-3 and every type-3 opcode Xenia's handles, with their
  effects on the register file (0x5003 registers) and on physical memory:
  fences (MEM_WRITE, EVENT_WRITE_SHD with a value or the vblank counter,
  REG_TO_MEM, COND_WRITE), the scratch registers' write-back, the read
  pointer's write-back after each run, COHER_STATUS_HOST, the gamma ramps,
  occlusion query reports (Xenia's fake ones: no host queries here). No
  kernel in it: memory, and a `Host` for the CPU interrupt and the wait
  between two polls of a WAIT_REG_MEM.
- **A `Sink`** takes what it does not do itself, in order, with the
  register file as it stands: buffers' starts and ends, packets as they
  run, memory read and written, shader loads, draws, resolves (a draw in
  copy mode) and swaps. That is Xenia's backend (IssueDraw, IssueCopy,
  IssueSwap, LoadShader) and its trace writer behind one interface; the
  headless GPU is Xenia's null backend (every draw succeeds, no picture).
- **The registers** answer the CPU's MMIO as Xenia's GraphicsSystem: reads
  from the register file but for the five it answers itself (the EDRAM's
  timing and controls, the vblank status, the 1280x720 viewport); writes
  store, and CP_RB_WPTR wakes the command processor.
- **Two kernel threads** run it, guest threads with a stack, a PCR and a
  KTHREAD in the idle process whose body is host code
  (`Thread::Params::host`, Xenia's XHostThread). "GPU commands" waits on an
  auto-reset kernel event the CP_RB_WPTR writes set, then runs the ring
  buffer. "GPU vblank" ticks 60 times a second on the scheduler's clock,
  paced to a deadline (missed beats made up to a second behind), moving
  D1MODE_V_COUNTER and the counter EVENT_WRITE_SHD can write. Both wait
  through `Objects::wait`, so the deterministic mode will run them as it
  runs the guest's. They start with the first call into the video driver
  and stop with the kernel.
- **The interrupt**: the callback VdSetGraphicsInterruptCallback names runs
  on the thread that raised it (vblank: source 0 on CPU 2; INTERRUPT:
  source 1 on each CPU of its mask), as EmulateCPInterruptDPC: the thread's
  PCR moved to that CPU, at DISPATCH_LEVEL with DPC_ACTIVE set and the
  title's process type in the DPC; the callback with r3 = source, r4 = its
  data on the thread's own stack (`Objects::call` keeps the context); then
  the PCR as it was. Interrupts on one CPU run one at a time.
- **The exports**: the 21 Vd* the games import and xam's XGetVideoMode,
  as Xenia's. The video mode comes from the console's settings, read
  through ExGetXConfigSetting; VdSwap writes the front buffer's fetch
  constant (its address made physical) to fetch constant 0 and Xenia's
  XE_SWAP packet into the 64 words the caller set aside in the ring.

The renderer (B3) is the command processor's Sink (2026-11-05): shaders
from `shader`, draws and resolves from `draw` and `resolve` with the
register file the processor keeps, guest memory read where it lies (the
processor's physical memory; `memory_written` marks the textures under
the processor's writes), the front buffer at `swap` through the
processor's gamma ramps. Its calls come on the "GPU commands" thread; the
Vulkan device lives with it. Traces replay through a processor of their
own, so there is one PM4 front end ([renderer.md](renderer.md)).
`gpu_command_processor(k)` (`kernel.h`) is where boot installs it. The same
Sink with a trace writer behind it would record Xenia's .xtr from a live
game.

## Sound

The APU as the games' code sees it: the render driver's clients and the
XMA decoder, with XMA decoded by FFmpeg's xmaframes decoder as Xenia
decodes it (plan step A6); the player sends the clients' frames to the
sound card (see "The player"). Translated from Xenia (`audio_system.cc`,
`pacing_audio_driver.cc`, `xma_decoder.cc`, `xma_context_new.cc`,
`xma_context.cc`, `xboxkrnl_audio.cc`, `xboxkrnl_audio_xma.cc`); where it
differs is noted at the top of each file.

As built (`src/kernel/audio.h`; audio.cpp, xma.cpp; the seams in
`xenoforge/audio.h`):

- **Render driver clients** (XAudioRegisterRenderDriverClient, eight at
  most): a kernel thread with a host body, "Audio Worker" in the system
  process, calls each client's guest callback (r3 a word holding its
  argument, as Xenia passes it) once a frame period, 256 samples at
  48 kHz, paced on the scheduler's clock, while the client has a free slot
  of the 64 frames it may have in flight. XAudioSubmitRenderDriverFrame
  hands the frame (6 channels of 256 big-endian floats, one channel after
  another) to the client's `audio::Output` as host floats; the default
  one is Xenia's PacingAudioDriver: the samples are dropped and each frame
  is played a period after the one before it, freeing its slot. The player
  sets a real output (`set_audio_output`). An unregistration waits for a
  callback in progress. `XF_AUDIO_WAV=<prefix>` also writes each client's
  frames to `<prefix>.client<N>.wav` (six channels of 32-bit floats at
  48 kHz, the game's own mix of its voices: what the player plays), to
  listen to a headless run.
- **The XMA decoder**: its registers at 0x7FEA0000 through MMIO (the games
  reach them with lwbrx and stwbrx: the context array's physical address,
  a rotating current context, ten kick, lock and clear registers of 32
  contexts each), and 320 contexts of 64 bytes (XMA_CONTEXT_DATA) in
  physical memory, where Xenia puts them. A kick wakes a second kernel
  thread, "XMA Decoder" in the idle process, and the kicking thread waits
  until it has walked the context as Xenia's default context does:
  packets, frames (a frame split across packets read whole), subframes
  handed to the output ring buffer `subframe_decode_count` at a time,
  loops with their subframe end and skip, the two input buffers swapped,
  the output full or starved, the context data written back merged with
  what the game changed meanwhile. Each frame's decode is one call to an
  `audio::FrameDecoder` (`set_xma_decoder`).
- **FFmpeg's decoder** (`src/xma_decoder.cpp`, the default): Xenia's
  XmaContextNew around FFmpeg's `xmaframes` decoder, from has207's fork
  (Xenia's submodule, commit d980192): a codec context per XMA context,
  opened again when its rate or channels change, each frame one packet
  (the byte with the frame's bit padding, then its bits), the planar
  floats made 16-bit big-endian samples as Xenia's ConvertFrame does
  (times 32767, rounded to nearest even, saturated). FFmpeg's first errors
  are printed and all counted. `XF_XMA_SILENT=1` puts the silent decoder
  in its place, to measure what decoding costs.
- **FFmpeg, built** (`third_party/ffmpeg/ffmpeg.cmake`): CMake's `XF_XMA`
  (on by default) fetches the fork's archive at that commit, or builds from
  `XF_FFMPEG_DIR` (a checkout of it; xenia-canary's
  `third_party/FFmpeg` is taken by itself from the sibling checkout when it
  is at the commit), and compiles only the part of libavutil and libavcodec
  the decoder needs, over a configuration overlay after Xenia's
  `third_party/ffmpeg-xenia` (one codec, no assembly): one shared library,
  `ffmpeg-xma` (0.8 MB), beside the executables. With `XF_XMA` off the core
  builds without FFmpeg and every XMA frame decodes to silence.
- **FFmpeg is LGPL 2.1 or later** (`LICENSE-ffmpeg`), and stays a separate,
  dynamically linked library so XenoForge's own code keeps its license.
  Shipping the player (or anything built with `XF_XMA`) means shipping
  `ffmpeg-xma.dll` (`.so`) with: the LGPL's text and a notice that the
  program uses FFmpeg under it; the library's source, or a written offer of
  it, which is the fork's commit plus `third_party/ffmpeg` (the overlay and
  the build script); and nothing that keeps a user from putting their own
  build of it in its place (the player loads whatever `ffmpeg-xma` it finds
  beside it). Linking FFmpeg statically would add the duty to ship the
  player's objects for relinking, so it is not done.
- **Recordings and Xenia's oracle**: with `XF_XMA_RECORD=<file.xmar>`
  (boot or the player) the decoder writes every run of a context (its
  data before and after, its input buffers and its output buffer before
  and after, each distinct buffer once) and the clears and releases
  between them (`xenoforge/xma_recording.h`); it hashes every buffer of
  every run, which slows a game that kicks often (Reach's client fell to
  a few frames a second while recording). `xma_oracle`
  (`tools/xenia_oracle/build.bat xma`, against the xenia-canary build and
  its FFmpeg) walks a recording through Xenia's own XmaContextNew and
  writes its runs beside it (`<name>.xenia.xmar`); `xma_replay_test` walks
  each recording under `XF_XMA_TRACES` through the kernel again and
  compares every run with the live one (the decoder's fields, the output)
  and with Xenia's (all 64 bytes of the context, every byte of the
  output); `XF_XMA_WAV=<dir>` writes each context's output, ours and
  Xenia's, as WAV files. Recordings are game data: `../traces/xma`,
  never the repository. Without `XF_XMA_TRACES` the test has nothing to
  run.
- **The exports**: Xenia's XAudio* (the speaker configuration from the
  console's settings, full voice category volumes, no volume changes, the
  ducker off) and all its XMA*; the ducker's getters, which Xenia lacks,
  return their float in f1, as the games' calls read it.
- `boot` reports the clients, callbacks and frames (and frames per
  second), and the XMA contexts created and kicked, kicks, runs (and runs
  starved of input: the game reads what it wrote before), buffer swaps,
  loops, frames decoded, frames with sound and the loudest sample, frames
  that decoded to nothing, bad input (a frame of the invalid length, a read
  offset past its buffer, a split frame whose rest is missing), runs cut
  at their pass limit and FFmpeg's errors.

## Files

As built (`src/kernel/files.h`; vfs.cpp, files.cpp), from Xenia's vfs and
xboxkrnl_io: a guest path is made canonical, its symbolic links resolved
(the object manager's list: ObCreateSymbolicLink's and the system's), and
given to the device whose mount path starts it; each device keeps a tree of
entries, names any case.

- **Devices**: the disc (`\Device\Cdrom0`, read in place through
  `disc.h`), or a bare executable's directory
  (`\Device\Harddisk0\Partition1`, read-only), with `game:` and `d:` on it;
  Xenia's null device at `\Device\Harddisk0` (the raw partitions answer
  with nothing); host directories for the cache partitions and content.
- **File objects** are dispatchers (an auto-reset event in guest memory,
  set after each request, as Xenia's XFile is waitable). Requests complete
  at once, as Xenia completes them: the I/O status block, the event, the
  APC routine queued to the calling thread, the completion ports
  (`IoCompletion`, a queue whose signal state counts its packets). A file
  opened neither synchronous nor buffered answers STATUS_PENDING for a
  request that succeeded, its result already in the status block.
- **The storage root** (`boot --content <dir>`, a temporary directory's
  `xenoforge` by default; never in the repository) has Xenia's layout, so
  saves are shared with it: `cache0`, `cache1` and `cache` (`cache0:`...),
  and `content/<XUID>/<TITLE>/<TYPE>/<name>/` for a package with its header
  at `content/<XUID>/<TITLE>/Headers/<TYPE>/<name>.header` (Xenia's
  XContentContainerHeader). The user is Xenia's default profile (XUID
  B13EBABEBABEBABE), or the first profile Xenia made there.
- `XF_LOG_FILES=1` logs each open and attribute query with its answer.

## XAM

As built (`src/kernel/xam.h`; xam.cpp, xam_content.cpp, xam_user.cpp,
xam_input.cpp, xam_msg.cpp, xam_net.cpp), from Xenia's xam: one user signed
in offline in slot 0 with Xenia's default profile settings; content as
above; the notifications Xenia queues at start; the controllers from the
drivers boot or the player installs ([input.md](input.md); none, every
user disconnected, without one); the guide's UI answered headless; XMsg's apps offline; the disc's media
verification never failing; the network offline (below).
Overlapped requests Xenia defers (25 ms, on a dispatch thread) complete at
once here, before the export returns ERROR_IO_PENDING.

The network is offline, as Xenia's answers make it: no Ethernet link, the
title's address the loopback, DNS and QoS lookups failed at once. The
sockets (xam_net.cpp) are the kernel's own objects with no host socket
behind them (Xenia opens a host socket for each): a datagram socket binds,
keeps its options, and has nothing to read (WSAEWOULDBLOCK, or a blocking
read waits up to SO_RCVTIMEO); datagrams to the loopback or broadcast
reach this title's sockets bound to that port, from 127.0.0.1, and those
to anywhere else are sent and lost; a stream socket listens and never gets
a connection, and connect fails at once. select, the WSA events and
closesocket wake what waits, through the dispatcher. Gears of War 3 polled
recvfrom on an unimplemented socket (an answer of 0, an empty datagram)
and stalled its frame loop there; with these it goes on.

## `boot`

`boot <game.iso> [--seconds N] [--steps N] [--content <dir>] [--interpret]
[--no-controller] [--press <buttons@seconds>]...`: loads the game, runs its entry point, and prints the modules loaded
(each one's generated code, or why it runs interpreted), the
interpreter's entries (the most entered addresses first, with the module
holding each), the kernel calls
(counts, unimplemented ones), the GPU's packets, draws, resolves, swaps (and
swaps per second) and interrupts, the sound's frames and XMA contexts, and
where it stopped. Built as `boot_<game>` (CMake's `XF_GAMES`), it runs the
game's generated code, every module's ("Code" above); `--interpret` leaves it out. `--steps` bounds the
interpreter's instructions per entry (2 billion by default: about 20 s of
El Chavo Kart's main thread). XAM's input answers from an idle pad for
user 0, a controller plugged in with nothing pressed, so a game waiting
for one goes on (Ben 10 paused on "Controller Lost Connection" without
it); `--no-controller` leaves every user disconnected, and `--press
start@40` holds buttons at a time on the scheduler's clock
([input.md](input.md)). What boot shares with the player (opening the
game, the renderer, the launch, the report) is `tools/game.cpp`.

`--render` draws what the GPU is sent (the renderer as the processor's
Sink, on a Vulkan device without a window; off by default, so runs stay
headless). `--shots <dir>` writes the front buffer of chosen swaps there as
`<title>_<swap>.png` (`--shot-swaps 300,301,890-910`, `--shot-every N`),
the swaps numbered as Xenia's `--trace_gpu_frames` numbers a trace's frame;
`xfcompare <xenia.png> <dir>` finds the shot closest to a Xenia dump. The
report adds the renderer's counts: draws drawn and skipped by reason, what
is approximate, resolves, swaps, and the time spent (in submissions,
resolves, checking and decoding textures, copying vertex data).

## The player

Plan track C1 (2026-11-05), after the 3DS runtime's `frontend/player.cpp`:
a game in an SDL3 window, with its controllers, its sound and an overlay.
Built as `player_<game>` beside `boot_<game>` (the same generated code
linked) and as `player` (interpreted), when CMake's `XF_PLAYER` is on (by
default with `XF_GAMES`); only then are SDL3 (release-3.4.16), Dear ImGui
(v1.91.9) and toml++ (v3.4.0) fetched, the 3DS runtime's pins, so the core
and its tests never need them.

```
player[_<game>] <game.iso | default.xex> [--xex <path>] [--content <dir>] [--config <file>] [--steps N]
    [--interpret] [--quit-after S] [--screenshots <dir>] [--screenshot-every N] [--press <b@s>]... [--no-sound]
    [--overlay]
```

The game is opened, launched and reported by `tools/game.cpp`, boot's own
code. As built (`frontend/`):

- **The window** (`presenter.cpp`): the front buffer each swap shows (the
  renderer's read-back, through the display's gamma ramp, the same picture
  `boot --shots` writes) uploaded and blitted into a Vulkan swapchain's
  image, scaled to fit with its shape kept (`video.aspect = "auto"`,
  letterboxed or pillarboxed) or stretched, filtered (`video.smooth`) or
  not, with the overlay drawn over it. The presenter has a Vulkan device of
  its own: the renderer's is windowless and its thread never waits for it.
- **One clock.** The renderer hands a front buffer over on its thread
  (`render::Options::on_frame`, the hook boot's shots already used) and
  goes on; the window's thread shows the latest whenever the display lets
  it (`video.vsync`: FIFO, else mailbox), and that wait is the window's
  thread's alone. A front buffer is read back only when the window has
  taken the one before, or a screenshot wants it (`frame_wanted`), so a
  minimized or slow window costs the guest nothing.
- **Input** ([input.md](input.md)): SDL3's gamepads (Xenia's SDL driver),
  then the keyboard as a controller (Xenia's keyboard driver, its
  bindings), the deadzones; the scripted presses on user 0.
- **Sound** (`sdl_audio.cpp`): each render driver client's frames to an
  SDL3 audio stream on the default playback device, in 5.1 where the
  device takes six channels, else folded to stereo as Xenia folds it
  (center halved on both sides, the back channels on theirs, the LFE
  dropped, over 2.5). The guest's timing stays the scheduler's: each output
  is the pacing output (a frame played every 256/48000 s on that clock)
  with the samples sent on beside it; the stream is kept about 60 ms ahead
  by playing a little faster or slower (half a percent at most), primed
  with silence when it ran dry, and a frame is let go when a quarter second
  is queued. `audio.volume` and `audio.mute` are the device's gain.
- **The overlay** (F1, Dear ImGui): the guest's swaps per second, the
  frames presented and the window's refreshes per second, the sound frames
  played (and how many had sound), the input states delivered, each user's
  controller (what drives it, its buttons, triggers and sticks), the
  volume, mute, vsync, scaling, fullscreen, and a screenshot button. F11
  switches fullscreen; F12 saves the frame shown as
  `<title>_<swap>_key<n>.png` in `player.screenshots` (default: a
  `screenshots` folder beside config.toml, never the repository).
- **config.toml** (`settings.cpp`; `--config`, else SDL's preference folder,
  `%APPDATA%\xenoforge\config.toml` on Windows), written with the defaults
  the first time and saved when the overlay closes:

  ```toml
  [video]    # fullscreen, vsync (true), smooth (true), aspect ("auto" or "stretch")
  [audio]    # volume (0-100), mute
  [input]    # keyboard (true), keyboard_user (0), left_deadzone, right_deadzone (0-1), vibration (true)
  [keyboard] # each pad control's keys (input.md)
  [player]   # screenshots: the folder F12 writes to
  ```

For checks without a person at the screen: `--quit-after <seconds>`
ends the run with a report (the swaps, the front buffers handed over and
presented, the window's refreshes, the input states delivered, the sound
frames submitted, sent to the device, with sound, dropped and starved,
then boot's report); `--screenshots <dir>` writes every
`--screenshot-every N` swaps' front buffer (300 by default) as
`<title>_<swap>.png`, printing the time on the scheduler's clock each came
at, which is the clock `--press` takes (or `--press start@#1100`, at a
swap); `--overlay` opens the overlay from the start; `--no-sound` keeps the pacing
output alone.

Checked (2026-11-05, both games with their generated code, on a machine
other builds were loading, so the rates vary from run to run; the shots and
logs in `../traces/player/`):

- **El Chavo Kart**: the intro movie at 10 to 20 swaps a second, each front
  buffer handed over shown (1204 of 1221 swaps in 60 s, 1600 of 1615 in
  270 s); its title screen came at 96.5 s on the scheduler's clock, and
  `--press start@100` took it to the main menu, where the game's
  first-time dialog ("Would you like to? A Yes, B No") waits. Two XAudio
  clients play 187.5 frames a second each; 10,685 of 99,085 frames carried
  sound (the movie's PCM; XMA was silence then, before A6), none dropped. The
  game asks for users 0-3 each frame and calls XamInputSetState (441 to
  599 times in a run). A capture of the window with `--overlay` shows the
  front buffer letterboxed under the overlay's rates, controllers and
  settings.
- **Ben 10**: no "Controller Lost Connection": in 90 s, 738 swaps, the
  Cartoon Network logo by swap 300 and Papaya's by 600 (where boot used to
  stop swapping before 600); one XAudio client at 187.5 frames a second,
  3,867 of 16,160 with sound. It polls XamInputGetKeystrokeEx with
  `ANY_USER | KEYBOARD`, which no driver here answers (as in Xenia).
- Left for a person at the screen: that a real pad and the keyboard move
  things in the game (only their drivers' states and the scripted presses
  are checked here), that the sound is heard and sounds right, how the
  window feels resized, fullscreen and with vsync off.

## Order

1. Memory (an agent, from Xenia's memory.cc).
2. Kernel core: exports, arguments, counts; the loader (image, imports,
   thunks); the main thread; dispatch; `boot` with the interpreter.
3. What the games call first, family by family (memory, threads and
   synchronization, files on the disc, video and the GPU front end, XAM),
   measured by `boot`.
