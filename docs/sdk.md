# The SDK: a port on an installed XenoForge

A game's port lives in its own repository and builds against an installed
XenoForge, as a port builds against the rexglue fork today. The port
holds its own code (hooks, replacements, native rendering, options), a project
file for the recompiler and a few lines of CMake; XenoForge holds the system.
The disc is never in the repository: the port builds from the player's own ISO.
The port's repository carries none of XenoForge's code: it sees XenoForge's
port API (a handful of headers) and links its libraries, and its executables
take from them only what a game with its code linked in uses.

## Where the SDK is in this repository

Everything a port is made from is under `sdk/`; the runtime it links is the
rest of the tree.

```
sdk/install.cmake            the package: what `cmake --install --component sdk` puts in the prefix
sdk/XenoForgeConfig.cmake.in find_package(XenoForge): the imported targets, by hand
sdk/xenoforge_port.cmake     xenoforge_add_port (also the repository's own XF_GAMES)
sdk/game_library/            the project that compiles a port's generated code
sdk/new_port.cmake           a new port's repository from sdk/template/
sdk/template/                that repository: CMake, presets, main, hooks, project file
sdk/examples/                minimal_port, native_port (tests/sdk_test.cmake builds them)
```

## Starting a port

```
cmake --install <XenoForge's build> --component sdk --prefix <sdk>
cmake -DNAME=mygame "-DTITLE=My Game" -DDIR=<new repository> -DISO=<disc image> -P <sdk>/share/xenoforge/new_port.cmake
cd <new repository>
cmake --preset local
cmake --build --preset local
build/windows/boot_mygame <disc image> --seconds 20     (build/linux/ on Linux)
```

`new_port.cmake` writes the repository from `sdk/template` (`NAME`: the
targets' name, letters, digits and `_`; `TITLE`: the window's title; `DIR`:
`./<NAME>` by default, empty or new; `ISO`: optional; `TITLE_ID`: optional,
the game's title ID, so the build bakes the shaders its runs recorded):

| file | what |
|---|---|
| `CMakeLists.txt` | `find_package(XenoForge <the SDK's version>)` from `XENOFORGE_SDK` (cache or environment) or `CMAKE_PREFIX_PATH`, then `xenoforge_add_port` on `GAME_ISO` with `project.toml` (or on `GAME_GENERATED_DIR`, code emitted earlier) |
| `CMakePresets.json` | `windows`, `linux`, `linux-arm64`, `android-arm64` (A port on every host, below): Ninja, RelWithDebInfo, `build/<preset>/` |
| `CMakeUserPresets.json` | `local`: this host's preset (`windows` or `linux`) with this machine's SDK prefix and disc; not versioned |
| `src/main.cpp` | the port's main: its name, an option, `on_init` and `on_shutdown` |
| `src/hooks.cpp` | the hooks, a module reloaded while the game runs (`PORT_HOT_RELOAD`, on by default but on Android) |
| `project.toml` | the recompiler's project file, empty |
| `README.md`, `.gitignore` | the platforms, build and run lines; build trees, Android's packaging files, editors' files, discs and executables kept out |

## A port on every host

A port is made for Windows, Linux and Android at once: its code is the same
everywhere (no host API in it; XenoForge's runtime has a branch for each
host), and the host is the preset's and the SDK's:

| preset | the port's host | built on | the SDK it takes |
|---|---|---|---|
| `windows` | Windows x86-64 | Windows | one built there |
| `linux` | Linux, the build machine's CPU | Linux | one built there |
| `linux-arm64` | Linux ARM64 | Linux x86-64 | one built with `cmake/toolchains/aarch64-linux-gnu.cmake` (installed in `share/xenoforge/toolchains`) |
| `android-arm64` | Android arm64-v8a, API 29+ | any host with the NDK (`ANDROID_NDK_HOME`) | one built with the NDK's toolchain |

- **Cross-compiling.** The recompiler runs during the build, on the build
  machine, so a cross build names an `xfrecomp` built for the build machine:
  `XENOFORGE_HOST_XFRECOMP` (cache or environment; XenoForgeConfig.cmake
  defines it). Without it, a port built from an ISO stops at configure and
  says so; one on `GENERATED_DIR` needs none. `xenoforge_add_port` passes
  the toolchain (`CMAKE_TOOLCHAIN_FILE`, `CMAKE_SYSROOT`, `ANDROID_ABI`,
  `ANDROID_PLATFORM`, `ANDROID_STL`) to the project that compiles the
  generated code, and on ARM64 compiles it with no x86 flags (the CPU level
  is `arm64`). The template finds the package through `XenoForge_DIR`
  (`<XENOFORGE_SDK>/lib/cmake/XenoForge`), which a toolchain's find root
  (the target's sysroot) does not hide.
- **Android.** `player_<name>` is a shared library, `libplayer_<name>.so`:
  SDL's Java activity (SDLActivity) loads it from the APK and calls the
  `SDL_main` it exports, which is the port's `main` (`xenoforge_add_port`
  force-includes a header declaring `extern "C" SDL_main` and defining
  `main` as it). `boot_<name>` stays an executable (run with `adb shell`).
  The hooks are linked in (no reloading inside an APK). The APK itself
  (Gradle, SDLActivity's Java sources, the manifest) waits for XenoForge's
  own Android build (docs/plan.md, "Hosts"): until there is an SDK built for
  Android, the `android-arm64` preset has nothing to link.
- **Checked** (2026-11-14): the template's port built and run on Windows
  and on Linux x86-64 (WSL, `ctest -R sdk`), and the whole port of game A
  on Windows. Android: the template's port configured with the NDK 30's
  toolchain and its main compiled for arm64-v8a (`SDL_main` a global C
  symbol), the toolchain passed to the generated code's project, the x86
  flags gone, and a build from an ISO without `XENOFORGE_HOST_XFRECOMP`
  stopping with its message; nothing linked (no Android package yet). ARM64
  Linux: written, not built.

## What a port links

Its executables link the port's code, its generated code and the runtime:
the kernel, the renderer, the player or boot. Not the recompiler: the
generic boot and player load a game's code from the cache (the game library)
or make it at start (the copy-and-patch tier), and that part of them
(`tools/game_tiers.cpp`: the library loader and tier-up, the tier, the
recompiler's discovery and the recompile step's cache code it calls) is
reached only through a table their main turns on (`game::enable_tiers`,
`tools/stub_main.cpp`). A port's main (`xf::run`) never does, so the linker
leaves all of it out: on the SDK's examples, 11.5 to 9.6 MB for boot and
16.8 to 14.9 MB for the player (2026-11-14). The recompiler runs at build
time, as `xfrecomp` from the package's `bin/`.

## The package

`cmake --install build --prefix <dir> --component sdk` (the default component
is the player's flat folder, `docs/runtime.md`, "Shipping the player"; a plain
`cmake --install` makes both). Layout:

```
bin/                         xfrecomp, xfinspect, xfshader (the shader bake), ffmpeg-xma.dll (+ LICENSE-ffmpeg)
include/xenoforge/           the port's API (run.h, port.h, native_render.h,
                             native_shaders.h, port_module.h), the generated code's (ppc_generated.h,
                             ppc_module.h, game_library.h, ppc_runtime.h, and the
                             interface version, game_interface.h) and what those
                             include (kernel.h, memory.h, ppc_context.h, xex.h,
                             vk_device.h, ...): 26 headers (25 of the tree's 79, and the
                             generated game_interface.h), not XenoForge's other 54
lib/                         xenoforge, xenoforge_kernel (an archive of the kernel's
                             objects), xenoforge_vk, xenoforge_boot and, with the
                             player, xenoforge_player, xf_frontend, xf_imgui, SDL3-static
lib/cmake/XenoForge/         XenoForgeConfig.cmake, its version file,
                             xenoforge_port.cmake, game_library/, sdl3_system_libs.txt
share/xenoforge/             LICENSE, LICENSE-xenia, LICENSE-rexglue,
                             new_port.cmake and template/ (Starting a port)
```

The headers are found at configure time from the API's list
(`XF_SDK_API_HEADERS` in `sdk/install.cmake`) through their `#include
"xenoforge/..."` lines, so a header the API comes to include ships with it;
a header a port needs that is not reached so belongs in that list (and is
then part of the API).

`find_package(XenoForge REQUIRED)` (with `CMAKE_PREFIX_PATH` set to the prefix)
gives these imported targets: `XenoForge::core` (the recompiled code's
runtime, the interpreter, the disc and executable readers), `::kernel` (the
system's exports; linked whole, as they register by static initializers),
`::vk` (the renderer), `::boot`, `::player` (when XenoForge was built with
the player), and the executables `::xfrecomp` and `::xfinspect`; plus the
variable `XENOFORGE_RUNTIME_FILES` (what `xenoforge_add_port` copies beside the
executables). SDL3, Dear ImGui and toml++ are inside `::player`: a port does
not fetch them. The package is built and tested on Windows and, without the
player, on Linux (2026-11-12: the `sdk` test passes there; the kernel's
archive names core and the renderer as its dependencies, so a linker that
reads archives once and in order sees them after it). SDL3's system
libraries are read from the build that made them.

## `xenoforge_add_port`

```cmake
find_package(XenoForge REQUIRED)
xenoforge_add_port(mygame
    ISO     ${GAME_ISO}              # the disc image
    PROJECT mygame.toml              # optional: the recompiler's project file
    SOURCES src/main.cpp src/hooks.cpp
    OPT     -O2)                     # optional; default XF_GAME_OPT or -O2
```

It makes:

| target | what |
|---|---|
| `game_mygame` | the generated code as a static library |
| `player_mygame` | SOURCES + the player + `game_mygame`: the window, input, sound |
| `boot_mygame` | the same linked to boot: headless, for tests, traces and `--render` |

The generated code's files are not known until `xfrecomp emit` has written
them, so emission and compilation are one build step (`sdk/build_game.cmake`,
run by `cmake -P`): `xfrecomp emit <iso> <build>/xenoforge/mygame/code
[--project <toml>]`, then a small project
(`game_library/CMakeLists.txt`) is configured over what was written and built
with `-O2 -g0` and the CPU level's `-march` (`-DXF_GAME_CPU_LEVEL=v3`, the
default, or `v2`: docs/runtime.md, "The CPU level"; both executables check
at start, before any static initializer of the generated code, that the CPU
has that level, and say so and exit when it lacks it). The step runs only
when the ISO, the project file, `xfrecomp` or the generated code's headers
changed; `xfrecomp` writes only the files whose text changed, so after a
change to the project file or a recompiler fix only those files recompile.
The inner build runs as many compilers at once as `xfrecomp jobs` says:
the recompile step's own policy (game_build.h's `job_policy`), the cores and
the memory free when the step starts, an eighth of the machine (at least
1 GB) kept for the rest and about 0.5 GB a compiler. The cores alone would
run a 16 GB machine with an editor and a browser open out of memory (a
first build of game A's 291 files at 12 compilers); limited by hand to 4 it
took about 40 minutes. `-DXF_GAME_JOBS=<n>` forces the count. The step runs
in the console pool (`USES_TERMINAL`): alone, its progress shown as it goes.
The flags travel as space-separated strings (`-DXF_GAME_CPU_FLAGS=-march=x86-64-v3
-mprefer-vector-width=128`), so no step on the way splits v3's two flags (a
list once did, and a port from an ISO failed to configure its code).

`SHADER_STORAGE <file>`: the shaders a run of the game recorded
(`<cache>/<title id>.xfshaders`; `xenoforge_shader_storage(<var> <title id>)`
gives that path on this machine) baked into `shaders/` beside the
executables by `xfshader bake`, again whenever the storage grew (none yet:
said, and no bake); a port's `ShaderCache` reads them there
(native_shaders.h).

Other arguments: `DEFINITIONS`, `INCLUDES`, `LIBRARIES` for the port's
sources; `XEX <path>` for an executable other than `default.xex`; `NO_PLAYER`,
`NO_BOOT`; and, for the repository's own use, `GENERATED_DIR <dir>` (code
emitted earlier, compiled in this build) and `PREBUILT <library>`
(`-DXF_GAMES` and `XF_GAME_LIBS` go through the function this way).

The port's sources are compiled for each executable, boot's with
`XF_PORT_BOOT` defined. Both executables get `XENOFORGE_RUNTIME_FILES`
(`ffmpeg-xma.dll`, the sound's XMA decoder, loaded at run time) copied next to
them. Without SOURCES the function writes a main with no port code.

`HOOK_SOURCES <files>` are the port's hooks (`xenoforge/port.h`): compiled into
both executables as SOURCES are, or, with `HOT_RELOAD`, into a module of their
own, `<name>_hooks` (a DLL; a .so on POSIX), beside the executables, which
load it as the game opens and load it again when it changes (below).

## Reloading the hooks

(Plan E5, 2026-11-08.) With `HOT_RELOAD`, a port's hooks are edited and
rebuilt (`cmake --build build --target mygame_hooks`) while the player runs,
and the player takes the new ones without restarting the game:

- **When.** The player and boot look at the module's file as they run; a
  change that has stayed for half a second (the build done writing) is
  reloaded. F9 in the player reloads at once; boot's `--reload-hooks-every S`
  reloads on a clock (the tests' way).
- **How** (`xenoforge/port_module.h`, `src/kernel/port_module.cpp`). The new
  file is copied to a name of its own in the temporary directory (so the
  build can write the file again, and a debugger holds no lock on it) and
  loaded; its interface is checked before anything stops, so a bad build
  changes nothing. Then every guest thread is parked at a safe point
  (`Kernel::pause`: a kernel call, or a yield point of the generated code;
  docs/runtime.md, "Pausing the guest"), the old module's hooks are dropped
  and the new one's registered, and the threads go on. The pause lasts the
  registration (well under a millisecond on game B); a thread that has not
  parked within a second is left running, and said so.
- **The old copy** is unloaded once no thread runs its code: a thread inside
  one of its hooks (a replacement whose original waits in a kernel call)
  keeps it loaded until it comes back out (`hooks::drained`). Each reload, and
  the player's loop, unload the copies so drained.
- **What survives** is what the runtime keeps: the registry (names,
  `--hook-stats` counts, by address, kind and hook name, so counts go on
  across reloads) and `port::persistent` blocks:

  ```cpp
  struct Stats { uint64_t frames; float speed; };       // plain data
  XF_BEFORE(0x82012400, count_frames) {
      Stats &s = xf::port::persistent<Stats>("stats");  // the same block after a reload
      s.frames++;
  }
  ```

  A block is zeroed (or value-initialized) the first time, kept by its key
  for the life of the process, and made anew (with a warning) when its size
  changes. **What does not survive**: the module's statics and its heap (a
  pointer to either kept in a persistent block dangles after a reload),
  function pointers into the module held anywhere but the registry, and
  threads the module starts (it must not leave any running). Hooks
  registered at run time by the module (from a hook body) belong to it and
  go with it; hooks the executable registers are never touched.
- **The module's side.** Built with `XF_PORT_MODULE`, port.h's functions go
  to the runtime through a table of C functions the module's one export
  receives (`ModuleApi`): the module links nothing of XenoForge, sees port.h
  alone (no kernel, no renderer), and its static registrations wait for the
  table. `xenoforge_add_port` compiles the table's side into the module
  (a written `port_module_entry.cpp`).
- **What is not reloaded**: the executable's own SOURCES (the main,
  `on_init`, options, a native renderer); only HOOK_SOURCES.

## Debugging a port

- **The hooks in the reports** (docs/runtime.md, "Crash and stop
  reports"): crash and stop reports put each running hook between the guest
  frames, `#0 port: before hook count_frames on Camera_Update 0x82012400
  (mygame_hooks.dll#2)`, followed by where the guest goes on after it, and
  walk on across it. A fault in a hook's own code is reported as one: the
  hook, the host code address in its module, the r1 and LR it was entered
  with and the guest context as it is. The lockstep check names hooks the
  same way ("port!before count_frames: Camera_Update (0x82012400)").
- **`--hook-stats`** (boot and the player): each hook's calls are counted,
  listed in the run's report (`hook calls:`, a line per hook: address,
  function, kind, hook name, calls) and in the player's overlay (F1). Off,
  a hook pays one load and a branch for it.
- **Names as symbols**: the project file's names are aliases of the
  generated functions (`xfm_default_xex::Camera_Update`), for a debugger's
  or profiler's view (docs/recompiler.md, "The project file").
- **On the repository's own games**: `-DXF_GAME_HOOKS_<name>=<files>` builds
  hook sources (kept outside the repository) as a reloadable module for
  `boot_<name>` and `player_<name>`, with a project file's emission in
  `XF_GEN_DIR` for their slots.

## A port's main

```cpp
#include <xenoforge/run.h>

static bool widescreen = false;

int main(int argc, char **argv) {
    xf::PortInfo port;
    port.name = "My Game";                 // the window's title
    port.options.push_back(xf::PortOption::boolean("widescreen", "Widescreen", widescreen));
    port.on_init = [](xf::kernel::Kernel &k) { /* register hooks, replacements */ };
    port.on_shutdown = [](xf::kernel::Kernel &) {};
    return xf::run(argc, argv, port);      // the player, or boot in boot_mygame
}
```

`xf::run` fills in the generated code's tables and calls `xf::run_player` or
`xf::run_boot` (`run.h`); both take their generic command lines
(`docs/runtime.md`), plus `--option <key>=<value>`, repeatable.
`on_init` runs when the game is opened (executable loaded, kernel made,
options read) and before its main thread starts; `on_shutdown` after the run's
report. Hooks and replacements themselves are `xenoforge/port.h` (plan E3).

Options (the 3DS runtime's `PORT_OPTION`): boolean, integer or real, bound to
the port's variable. In the player they are read from `config.toml` (`[port]`,
or the game's own table `[games."<title id>".port]`), written with their
defaults the first time, shown in the overlay (F1) under the port's name,
saved when the overlay closes, and overridden for a run by `--option`. In
boot only `--option` sets them.

## Native rendering

A port can draw a game's scene itself on XenoForge's Vulkan device, from what
its hooks capture of the game's engine, while the emulated GPU skips the work
the port replaces: the model of the rexglue fork's native guest renderer, on
Vulkan only. The header is `xenoforge/native_render.h`; the renderer's side is
in docs/renderer.md, "A port's own renderer".

```cpp
#include <xenoforge/native_render.h>

static bool draw(const xf::native::OutputContext &ctx) {
    if (!scene_captured) return false;               // this frame is the emulated one
    xf::native::TextureImage albedo;
    std::string why;
    ctx.frame->texture(captured_fetch, albedo, why); // a guest texture, detiled by the texture cache
    VkBuffer vb; VkDeviceSize at;
    ctx.frame->upload(vertices, bytes, 16, vb, at);  // host-visible for this frame
    VkCommandBuffer cmd = ctx.frame->commands();     // after the helpers
    // vkCmdBeginRendering into ctx.output_view (VK_IMAGE_LAYOUT_GENERAL), draw, end
    return true;
}

port.on_init = [](xf::kernel::Kernel &) {
    xf::native::set_output_renderer(draw);
    xf::native::set_pass_filter([](const xf::native::PassInfo &p) { return p.surface_pitch >= 1280; });
};
port.options.push_back(xf::PortOption::boolean("native_render", "Native renderer", xf::native::enabled_flag()));
```

- **The call.** At each swap, on the renderer's thread, with: the device
  (`vk::Device`, the raw handles, the queue family), `frame`, an output image
  (`R8G8B8A8_UNORM`, color attachment, storage, sampled, transfer; in
  `VK_IMAGE_LAYOUT_GENERAL` at the call and left in it; the guest front
  buffer's size unless `set_output_size` says otherwise), the guest's front
  buffer (fetch constant 0: address, size, format, tiling, the six words), the
  register file and guest memory's physical view. It returns whether it drew
  the frame: true shows the output image as it is (no display gamma); false
  shows the emulated picture.
- **Recording.** `frame->commands()` is the renderer's command buffer, outside
  any render pass, after a full barrier. Ask for it again after `texture()` or
  `upload()`: they may submit the renderer's work and begin a new one (the
  queue keeps the order). The port never submits; it leaves no render pass
  open; its bindings end with the callback.
- **Skipped.** After a frame the port drew, the next frame's draws and
  resolves are skipped where the pass filter says so (all, without one). The
  filter gets a `PassInfo` (resolve or draw, the surface pitch, the color
  target's base and format, the primitive and count, the shaders' hashes, a
  resolve's destination, the registers). **Kept always**: the command
  processor's packets and register writes, fences, `EVENT_WRITE*`,
  interrupts, the swaps and their timing, and draws exporting memory
  (memexport). Occlusion query reports a skipped draw counted for are answered
  with `set_occlusion_samples` (1000 by default, Xenia's fake count).
- **Yielding.** A port may yield any frame. The frame after one it drew had
  its passes skipped, so it has no emulated picture: it is *held* (the window
  keeps the port's last picture), and the frame after is emulated in full.
- **The toggle.** `enabled_flag()` (on by default) is read at each swap; a
  port option can bind it, the player's F6 flips it, and the overlay (F1)
  shows the state and the counters (`counters()`: frames drawn, yielded, held;
  draws and resolves skipped and kept; memexport draws kept; faked reports).
  Boot and the player print the same counts in their report.
- **The guest's data.** `vertex_fetch`, `texture_fetch`, `float_constant`
  (0-255 vertex, 256-511 pixel), `bool_constant`, `loop_constant` read the
  registers; `read_words`, `read_floats`, `read_indices` read guest memory
  swapped as the GPU swaps it. On the CPU side, a hook reads the engine's
  structures with `xenoforge/port.h` (`be<T>`, `load<T>`).
- **Shaders.** SPIR-V the port compiles itself: `sdk/examples/native_port`'s
  CMakeLists runs glslc (the Vulkan SDK's) and includes the words with
  `-mfmt=num`. XenoForge itself needs no Vulkan SDK (its package carries
  Vulkan's headers and volk, `XenoForge::volk`: Vulkan's functions are
  pointers loaded at run time, so a port calls them through `<volk.h>`, which
  `xenoforge/vk_device.h` and `native_render.h` include); a port without
  shaders of its own needs no SDK either, and one with them can commit the
  words as XenoForge does (`cmake/vulkan.cmake`, `xf_spirv`).

`sdk/examples/native_port` is the test port: the guest's last front buffer
(through `Frame::texture`), tinted, with a spinning triangle (through
`Frame::upload`) over it; `--capture <hex address>` hooks a guest function
with `xf::port::before` and yields every frame it was not called in. Options
`native_render`, `suppress` (skip the emulated work, else draw over it) and
`background`. Inside XenoForge's build, `-DXF_NATIVE_PORT_GAME=<one of
XF_GAMES>` builds it as `build/sdk/examples/native_port/{player,boot}_native`.

## A port that replaces the guest's Direct3D

`native_render.h` alone lets a port draw while the guest's Direct3D still
builds PM4 command buffers and the command processor still reads them, its
draws skipped. A port can go further: replace the D3D library the game links
statically, so the emulated GPU does no work. The pattern (2026-11-14, from
the first port's design):

- **What is replaced**: the D3D's draws (`DrawIndexedVertices`,
  `DrawVertices`, the "UP" draws' `Begin`/`End`), `ClearF` and `Resolve`,
  with `XF_REPLACE` hooks that never call the original. The `Set*` functions
  stay: they only write the `D3DDevice`'s shadow of the GPU registers, which
  the replacements read whole at each draw (the fetch constants, the float
  constants, the render state, the bound shaders).
- **What is kept**: `Swap` and the fences run as the guest has them, so the
  ring buffer still carries the swap packet and the fence writes and nothing
  else. The command processor then has almost nothing to read; the swaps,
  their pacing by the vblank and the guest's fence waits stay right without
  the port doing any of it; and the output renderer (`set_output_renderer`)
  is still called at each swap, where the port records the frame it captured
  (the replacements run on the guest's render thread; they capture, the
  output renderer records).
- **The toggle**: with `enabled()` off, the replacements call the originals,
  and the emulated path draws the frame (the oracle, F6).

What the SDK gives such a port:

- **The game's own shaders** (`xenoforge/native_shaders.h`):
  `translate_shader` turns Xenos microcode into SPIR-V with the translator
  the emulated path uses, `shader_hash` is the emulated path's hash (the one
  `PassInfo` and the shader storage carry), `ShaderConstants` and
  `SystemConstants` are the blocks the shaders read (their layouts checked
  against the renderer's at XenoForge's build), and the header documents the
  bindings. `ShaderCache` keeps the SPIR-V by hash in memory and in a
  directory, under a folder of `translator_version()` (a digest of the
  translator's sources, so a new translator never reads an old one's
  SPIR-V). **The bake**: `xfshader bake <cache>/<title id>.xfshaders <dir>`
  (in the package's `bin/`) translates every shader a run recorded (the
  generic player and the emulated frames record them) into that directory
  before the port runs; game A's 240 recorded shaders bake in 0.8 s, all
  translated, all valid SPIR-V for Vulkan 1.3 (`spirv-val`), and a second
  bake has nothing to do.
- **Guest memory on the GPU** (`Frame::guest_memory`): the renderer's mirror
  of physical memory, one storage buffer, with the pages under a range made
  current; the translated vertex shaders fetch their vertices from it at the
  fetch constants' addresses and swap the words themselves, so a port copies
  and converts no vertex data.
- **Textures** (`Frame::texture`): a fetch constant's texture, detiled and
  converted by the renderer's texture cache.
- **Checking a capture** (`set_draw_observer`): every draw and resolve the
  command processor issues, with its `PassInfo` (the register file, the
  shaders' hashes, the primitive and count, the swaps before it), whether the
  port draws the frame or not, on the renderer's thread; unset, a draw pays
  one branch. A port that reads each draw's state from the `D3DDevice`'s
  shadow compares it with what the GPU got, draw by draw, before replacing
  anything (the first port's step 2: every draw of game A's opening frames
  matched, its shaders' hashes read through the D3D shader objects'
  microcode descriptors equal to the emulated path's). Boot calls it only
  with `--render` (without it no renderer is made).

- **What a shader reads** (`shader_uses`, `used_float_constants`): its
  float constants, texture fetch slots and vertex fetch constants with
  each one's stride, so a port uploads and binds only those (a shader
  reading relative to a0 or aL reads all its constants) and copies a
  draw's vertices from a vertex buffer by its indices' range.
- **The fixed function** (`native_render.h`): the renderer's own
  conversions of the registers, `vk_compare_op`, `vk_stencil_op`,
  `vk_blend_factor`, `vk_blend_op`, `host_viewport` (the viewport and the
  NDC scale and offset the translated vertex shaders take) and
  `host_scissor`, so a port building pipelines for the translated shaders
  does not write them again.
- **Seeing it**: boot's `--shots` read a frame the port drew from its
  output image (what the window shows), so a run with the port's frames and
  one without compare frame by frame (`xfcompare`).

The first port drew its opening's frames this way (the game's Bink shaders,
its own pipelines and descriptor sets on this layout): every emulated draw
and resolve of those frames skipped, the pictures as the emulated ones.

Still to come: resolves into host images a port owns (render targets as
images, not EDRAM), and notification of CPU writes to guest memory ranges a
port registers.

## Building and running a port

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
build/boot_mygame <iso> --seconds 20          # headless: the report and the xf-summary line
build/player_mygame <iso> [--quit-after 20]   # a window
```

`sdk/examples/minimal_port` is the smallest port: a main, an empty project file
and the CMake above (`-DMINIMAL_ISO=<iso>`). The project file's schema is in
`docs/recompiler.md`, "The project file".

## Checks

- `ctest -R sdk` (tests/sdk_test.cmake): installs the `sdk` component to a
  temporary prefix, configures and builds `sdk/examples/minimal_port` against it on
  an empty generated-code table (`tests/sdk/modules.cpp`), and runs what it
  built; then `sdk/examples/native_port` the same way when glslc is found. It
  checks the package (headers, libraries, imported targets, the function, the
  runtime files, a port's own shaders), not a game's recompile. Then a port
  made by the installed `new_port.cmake` is configured and built the same way
  (its hooks module included), and the package is checked to hold no header
  outside the API's (`renderer.h`, `recomp.h`, `stencil.h`). Last, that port
  is built from an executable as from a disc (`tests/sdk/make_xex.cpp` makes
  a title of four functions; the ISO form's emission and compile, with the
  job count) and run: boot reports the code linked in, and the title's exit
  code is the sum its functions compute. Passes on Windows and on Linux
  x86-64 (WSL).
- By hand, a real port end to end: the example with `-DMINIMAL_ISO=<a disc>`,
  then `boot_minimal <disc> --seconds 20`.
