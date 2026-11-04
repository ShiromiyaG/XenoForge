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
