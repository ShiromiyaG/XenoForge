# XenoForge

A lean Xbox 360 system for statically recompiled games: the recompiler, the
kernel calls, the GPU, the sound and the input the games ask for, measured,
in place of a whole emulator. Written from scratch in C++20, with
[Xenia](https://github.com/xenia-canary/xenia-canary)'s research as the
reference for how the console behaves.

No game is included or needed to build it. To run anything you need your own
copy of a game.

## What works

Early days: the first step of [the plan](docs/plan.md).

| Part | Where | State |
| --- | --- | --- |
| The disc: XDVDFS read from the ISO in place (XGD2/XGD3 dumps, rebuilt images) | `disc.h`, `src/disc.cpp` | done; tests |
| The executable: XEX2 headers, AES-128, basic compression, the import libraries | `xex.h`, `aes.h`, `src/xex.cpp`, `src/aes.cpp` | done but LZX; tests |
| The system's exports by ordinal (xboxkrnl, xam, xbdm) | `exports.h`, `src/exports/` (Xenia's tables) | done |
| `xfinspect`: a disc's executable, its XDK libraries and imports, the image dumped | `tools/xfinspect.cpp` | done |
| Xenia's GPU traces read (snappy included) and walked through a PM4 front end that counts | `gpu_trace.h`, `trace_stats.h`, `snappy.h` | done; tests |
| Xenos microcode walked: control flow, ALU and fetch operations, the constants it reads | `ucode.h`, `xenos.h` | done; tests |
| `xftrace`: what a corpus of traces uses (packets, draws, formats, shaders, textures) | `tools/xftrace.cpp` | done |
| Xenos microcode parsed as Xenia parses it, and disassembled: byte for byte Xenia's text on every shader of the traces | `ucode_parse.h`, `tools/xfshader.cpp` | done; tests |
| A SPIR-V module writer (checked by `spirv-val`) and a windowless Vulkan compute runner (checked on the GPU) | `spirv.h`, `vk_compute.h` | done; tests |

## Building

Needs CMake 3.25+, Ninja and clang (or MSVC).

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build
```

## Tools

```sh
# what a game disc holds: title, executable, XDK libraries, imports
build/xfinspect "<game>.iso"
build/xfinspect "<game>.iso" --imports              # every import, tab separated
build/xfinspect "<game>.iso" --files                # the disc's files
build/xfinspect "<game>.iso" --dump-image image.bin # the image, decrypted and unpacked
build/xfinspect default.xex                          # an executable outside a disc

# GPU traces: recorded by a xenia-canary built with -DXENIA_ENABLE_GPU_TRACE=ON
# (release downloads leave the trace writer out), then counted
tools/capture_traces.ps1 -Xenia <xenia_canary.exe> -Game "<game>.iso" -Out <folder> -Frames 300,900,1800
build/xftrace <folder>                               # each trace, then all of them

# a shader's microcode disassembled (Xenia's --dump_shaders writes them host-order)
build/xfshader shader_<hash>.ucode.bin.frag
tools/compare_disasm.ps1 <folder>                    # against Xenia's own disassembly
```

## Documents

- [`docs/plan.md`](docs/plan.md): what XenoForge is, its principles, parts and steps.
- [`docs/renderer.md`](docs/renderer.md): the Xenos renderer's design.
- [`docs/lean-kernel.md`](docs/lean-kernel.md): what the games import from the
  system ([`docs/imports.tsv`](docs/imports.tsv)).

## License

Written by ShiromiyaG with Claude (Anthropic). MIT (`LICENSE`). Parts are
translated from Xenia (BSD 3-clause, Copyright (c) 2015 Ben Vanik and the
Xenia contributors, `LICENSE-xenia`), and its export tables are included
unchanged; each such file says so at its top. Nothing derived from any game
is in this repository.
