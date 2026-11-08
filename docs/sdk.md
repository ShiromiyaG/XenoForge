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
with `-O2 -g0 -mfma -mavx2` (the game libraries' flags). The step runs only
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
  built. It checks the package (headers, libraries, imported targets, the
  function, the runtime files), not a game's recompile.
- By hand, a real port end to end: the example with `-DMINIMAL_ISO=<a disc>`,
  then `boot_minimal <disc> --seconds 20`.
