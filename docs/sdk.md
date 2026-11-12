# The SDK: a port on an installed XenoForge

A game's port lives in its own repository and builds against an installed
XenoForge, as a port builds against the rexglue fork today. The port
holds its own code (hooks, replacements, native rendering, options), a project
file for the recompiler and a few lines of CMake; XenoForge holds the system.
The disc is never in the repository: the port builds from the player's own ISO.

## The package

`cmake --install build --prefix <dir> --component sdk` (the default component
is the player's flat folder, `docs/runtime.md`, "Shipping the player"; a plain
`cmake --install` makes both). Layout:

```
bin/                         xfrecomp, xfinspect, ffmpeg-xma.dll (+ LICENSE-ffmpeg)
include/xenoforge/           every public header, with the generated code's
                             (game_library.h, ppc_runtime.h, ppc_context.h, ...)
                             and run.h; whatever lands in include/xenoforge ships
lib/                         xenoforge, xenoforge_kernel (an archive of the kernel's
                             objects), xenoforge_vk, xenoforge_boot and, with the
                             player, xenoforge_player, xf_frontend, xf_imgui, SDL3-static
lib/cmake/XenoForge/         XenoForgeConfig.cmake, its version file,
                             xenoforge_port.cmake, game_library/, sdl3_system_libs.txt
share/xenoforge/             LICENSE, LICENSE-xenia, LICENSE-rexglue
```

`find_package(XenoForge REQUIRED)` (with `CMAKE_PREFIX_PATH` set to the prefix)
gives these imported targets: `XenoForge::core` (the recompiled code's
runtime, the interpreter, the disc and executable readers), `::kernel` (the
system's exports; linked whole, as they register by static initializers),
`::vk` (the renderer), `::boot`, `::player` (when XenoForge was built with
the player), and the executables `::xfrecomp` and `::xfinspect`; plus the
variable `XENOFORGE_RUNTIME_FILES` (what `xenoforge_add_port` copies beside the
executables). SDL3, Dear ImGui and toml++ are inside `::player`: a port does
not fetch them. The package is Windows now; nothing in it blocks Linux (static
libraries, SDL3's system libraries read from the build that made them).

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
them, so emission and compilation are one build step: `xfrecomp emit <iso>
<build>/xenoforge/mygame/code [--project <toml>]`, then a small project
(`game_library/CMakeLists.txt`) is configured over what was written and built
with `-O2 -g0` and the CPU level's `-march` (`-DXF_GAME_CPU_LEVEL=v3`, the
default, or `v2`: docs/runtime.md, "The CPU level"; both executables check
at start, before any static initializer of the generated code, that the CPU
has that level, and say so and exit when it lacks it). The step runs only
when the ISO, the project file, `xfrecomp` or the generated code's headers
changed; `xfrecomp` writes only the files whose text changed, so after a
change to the project file or a recompiler fix only those files recompile.
The inner build uses the generator's default parallelism.

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
- **Shaders.** SPIR-V the port compiles itself: `examples/native_port`'s
  CMakeLists runs glslc (the Vulkan SDK's) and includes the words with
  `-mfmt=num`. XenoForge itself needs no Vulkan SDK (its package carries
  Vulkan's headers and volk, `XenoForge::volk`: Vulkan's functions are
  pointers loaded at run time, so a port calls them through `<volk.h>`, which
  `xenoforge/vk_device.h` and `native_render.h` include); a port without
  shaders of its own needs no SDK either, and one with them can commit the
  words as XenoForge does (`cmake/vulkan.cmake`, `xf_spirv`).

`examples/native_port` is the test port: the guest's last front buffer
(through `Frame::texture`), tinted, with a spinning triangle (through
`Frame::upload`) over it; `--capture <hex address>` hooks a guest function
with `xf::port::before` and yields every frame it was not called in. Options
`native_render`, `suppress` (skip the emulated work, else draw over it) and
`background`. Inside XenoForge's build, `-DXF_NATIVE_PORT_GAME=<one of
XF_GAMES>` builds it as `build/examples/native_port/{player,boot}_native`.

## Building and running a port

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
build/boot_mygame <iso> --seconds 20          # headless: the report and the xf-summary line
build/player_mygame <iso> [--quit-after 20]   # a window
```

`examples/minimal_port` is the smallest port: a main, an empty project file
and the CMake above (`-DMINIMAL_ISO=<iso>`). The project file's schema is in
`docs/recompiler.md`, "The project file".

## Checks

- `ctest -R sdk` (tests/sdk_test.cmake): installs the `sdk` component to a
  temporary prefix, configures and builds `examples/minimal_port` against it on
  an empty generated-code table (`tests/sdk/modules.cpp`), and runs what it
  built; then `examples/native_port` the same way when glslc is found. It
  checks the package (headers, libraries, imported targets, the function, the
  runtime files, a port's own shaders), not a game's recompile.
- By hand, a real port end to end: the example with `-DMINIMAL_ISO=<a disc>`,
  then `boot_minimal <disc> --seconds 20`.
