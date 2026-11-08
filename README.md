<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/branding/xenoforge-wordmark-dark.png">
    <img alt="XenoForge" src="assets/branding/xenoforge-wordmark-light.png" width="420">
  </picture>
</p>

# XenoForge

An ahead-of-time Xbox 360 emulator, and an SDK for ports of specific games.

- **The emulator** recompiles your own disc on your machine into native code,
  once, and runs it on its own kernel, GPU, sound and input.
- **The SDK** lets a port of one game replace and hook its functions and draw
  natively, built to make errors easy to find.

Written from scratch in C++20 (Vulkan, SDL3), with
[Xenia](https://github.com/xenia-canary/xenia-canary)'s research as the
reference for how the console behaves. No game is included or needed to
build it; to run anything you need your own copy of a game.

**Status: early, in development.**

## What works today

- **The CPU:** PowerPC and VMX128 recompiled ahead of time into C++, with an
  interpreter for what is not. Xenia's instruction tests pass.
- **The kernel:** every system export Xenia implements (xboxkrnl, XAM, xbdm).
- **The GPU:** a Vulkan renderer of the Xenos GPU, checked frame by frame
  against Xenia's pictures.
- **The sound:** XMA decoded byte for byte as Xenia decodes it.
- **The player:** a window, gamepad and keyboard, sound, and a debug overlay
  (F1). The launcher lists your discs and their settings, recompiles a disc on
  your machine into a cached game library (Prepare), and starts the generic
  player on it.
- **Games:** two retail games play with
  picture, sound and pad; six more boot.

## Building

Needs CMake 3.25+, Ninja, clang and the Vulkan SDK.

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 4
ctest --test-dir build
```

A game's generated code is built with `-DXF_GAMES="<name>;..."` from what
`xfrecomp emit <iso> <dir>/<name>` writes (`-DXF_GEN_DIR=<dir>`). That builds
`boot_<name>` (no window) and `player_<name>`.

## Documents

- [docs/recompiler.md](docs/recompiler.md): the recompiler.
- [docs/runtime.md](docs/runtime.md): the kernel, sound, player and launcher.
- [docs/renderer.md](docs/renderer.md): the GPU.
- [docs/input.md](docs/input.md): input.

## References

XenoForge stands on the work of others. The main ones:

- **Emulators and recompilers:**
  - [Xenia / Xenia Canary](https://github.com/xenia-canary/xenia-canary)
    (BSD), the main reference and the oracle. Its D3D12 backend comes first
    for the GPU.
  - [XenonRecomp and XenosRecomp](https://github.com/hedge-dev) (MIT),
    hedge-dev's Xbox 360 recompilers.
  - [ReXGlue](https://github.com/rexglue/rexglue-sdk).
  - [RPCS3](https://github.com/RPCS3/rpcs3): ahead-of-time PowerPC
    recompilation.
  - [N64Recomp](https://github.com/N64Recomp/N64Recomp): a recompilation SDK
    with mods.
  - [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded): the
    original Xbox's kernel in high level.
  - [xenon-emu](https://github.com/xenon-emu/xenon): the Xbox 360's
    hardware, low level.
- **Documentation:**
  - [Free60](https://free60.org) and its wiki archive
    ([Free60Project/wiki](https://github.com/Free60Project/wiki)).
  - [libxenon](https://github.com/Free60Project/libxenon).
  - [x360-research](https://github.com/InvoxiPlayGames/x360-research).
  - "Xbox 360 System Architecture" (Andrews and Baker, IEEE Micro, 2006).
  - IBM's Cell Broadband Engine and PowerPC/AltiVec manuals.
  - AMD's R5xx/R6xx register and ISA documents.
  - Microsoft's documentation of the NT kernel routines.
- **Second implementations:**
  - [Mesa](https://gitlab.freedesktop.org/mesa/mesa)'s r600 and freedreno
    a2xx drivers.
  - [Wine](https://gitlab.winehq.org/wine/wine) and
    [ReactOS](https://github.com/reactos/reactos).
  - [Dolphin](https://github.com/dolphin-emu/dolphin).
- **Tools:** XNA Game Studio 4.0's Xbox 360 shader compiler, used to make
  test shaders ([tools/xna/README.md](tools/xna/README.md)).

Code under the MIT or BSD licenses is translated with its notice at the top of
the file. GPL and unlicensed sources are only read, never copied.

## License

Written by ShiromiyaG with Claude (Anthropic). XenoForge is MIT (`LICENSE`).

The rest is under its own license:
- **Xenia (BSD 3-clause, `LICENSE-xenia`):** parts are translated from it,
  and its export tables are included unchanged. Each such file says so at
  its top.
- **rexglue (`LICENSE-rexglue`):** the data pointer scan is translated from
  it.
- **FFmpeg (LGPL 2.1 or later, `LICENSE-ffmpeg`):** XMA is decoded by FFmpeg,
  built as a separate library (`ffmpeg-xma`) that the programs load at run
  time. Without it, they run with XMA silent.
- **The player's package:** `cmake --install build --prefix <folder>` makes
  it, with the notices of everything linked in. A game's own player, which
  holds that game's recompiled code, is never part of the package.

Nothing derived from any game is in this repository.
