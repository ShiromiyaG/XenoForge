# Contributing to XenoForge

Thanks for helping. XenoForge is an ahead-of-time Xbox 360 emulator and an SDK
for ports of specific games (README.md). This page covers the rules every
change must follow, how to build and test, and what a change should come
with. The design is in `docs/`. Read the part you are changing before you
change it:

- [docs/recompiler.md](docs/recompiler.md): the recompiler;
- [docs/runtime.md](docs/runtime.md): the kernel, sound, player and launcher;
- [docs/renderer.md](docs/renderer.md): the GPU;
- [docs/sdk.md](docs/sdk.md): ports;
- [docs/lean-kernel.md](docs/lean-kernel.md): what games import.

## Ground rules

These are not negotiable. A change that breaks one is not merged.

1. **Nothing from any game goes into the repository.** That means no disc
   images, XEX files, extracted assets, screenshots, GPU traces, memory or
   shader dumps, saves, or recordings. Test data lives outside the
   repository (for example `../traces/`), and the tests find it through
   environment variables.
2. **No game names or title IDs in the repository.** That covers code,
   comments, tests, CMake, scripts, examples and docs, and commit messages
   too.
   - Write "a game" or `<game>`, and use made-up addresses in examples.
   - Measurements name games as "game A", "game B" and so on.
   - Names and IDs are fine in an issue or a chat, never in a commit.
3. **No confidential material.** Never use leaked or non-public Microsoft
   material: XDK leaks, internal documents, leaked source. Never commit
   console keys, key vaults, NAND images or firmware. Public documentation,
   the community's research and your own measurements are fine.
4. **Licenses.** XenoForge is MIT (`LICENSE`).
   - **MIT and BSD code** may be translated, with its notice at the top of the
     file. Xenia's (BSD) is the usual one; check the translation against
     Xenia's source in the same change. Code taken from hedge-dev's
     XenonRecomp or XenosRecomp (MIT) also needs a `LICENSE-hedge-dev` at the
     root.
   - **GPL, LGPL, MPL and unlicensed sources are read, never copied.** That
     covers Dolphin, Cemu, RPCS3 and binutils, including XenonRecomp's
     `thirdparty/disasm`. You may learn how they solve a problem, then write
     your own.
   - **Third-party code** that is compiled in goes into `third_party/<name>`
     with its license, and gets a line in the README's License section.
5. **The emulator implements the console, never a game.**
   - The kernel and the renderer do what the hardware and the system do. No
     case for one title.
   - What a game asks for and the emulator does not do yet is counted
     (`approximate`, `skip`, the reports), not hidden.
   - Behaviour specific to one game belongs in that game's port, through the
     SDK.
6. **Xenia is the reference.** Where XenoForge does something differently
   from Xenia, the difference is documented. Going beyond Xenia in accuracy
   needs independent evidence: hardware documents, Microsoft's public
   documentation, a second implementation, tests. Say which in the docs.
   - A host-side tweak, such as thread priorities, affinities or spin
     policies, comes in only if Xenia or another emulator does the same.
7. **Every host stays possible.** XenoForge targets Windows, Linux and
   Android (ARM64).
   - No Win32 call outside `#if defined(_WIN32)` without its POSIX twin. Take
     sizes from the host.
   - x86-only code (intrinsics, assembly) needs a portable path next to it.
   - Vulkan 1.3 is the floor. Anything above it is optional, with a fallback.

## Building

You need CMake 3.25+, Ninja and clang. The Vulkan SDK is not needed (the
README explains why).

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 4
ctest --test-dir build
```

- Build with `-j 4` or less on a machine with 16 GB or less: the renderer's
  and the generated code's files are large.
- To build a game's statically linked code, see the README
  (`-DXF_GAMES=...`). The generic player does not need it: the launcher's
  Prepare step builds a game library from your own disc.

## Testing

- **`ctest`** runs everything that needs no game data. It must pass before
  you submit.
- **The renderer** is checked frame by frame against Xenia's pictures of
  the same frames:
  ```sh
  XF_TRACES=<traces> XF_DUMPS=<dumps> build/replay_test
  ```
  Run it on both render target paths: the exact path is the default, and
  `XF_RENDER_TARGET_PATH=host` gives the other. You need your own traces
  and Xenia's dumps of them (docs/renderer.md, "How it is checked").
- **The CPU** runs Xenia's instruction tests through both the interpreter
  and the recompiled code (docs/recompiler.md).
- **The deterministic mode** compares game hashes against expected values
  kept outside the repository (`XF_DETERMINISTIC_EXPECTED`).
- Run long tests and boots under a timeout (`timeout`,
  `ctest --timeout`), and never leave processes running.
- A bug fix comes with a test that fails without it, where one can be
  written.

## What a change should come with

- **Docs.** Update the section of `docs/` that describes what you changed,
  in the same change. Say what it does and why, what you measured, and what
  you tried and dropped.
- **A switch for risky behaviour.** New behaviour that could regress a game
  goes behind an `XF_*` environment variable, so it can be compared with the
  old one. Document the default and the reason for it.
- **Measurements for performance work.**
  - Time before and after on the same machine. Interleave the runs (A B A B)
    over several rounds, and report the median and the spread.
  - Keep a change only if it gains beyond the noise.
  - For the renderer, `xfreplay --repeat` over a trace is the benchmark
    (`XF_REPLAY_KEPT_PLAY=1` runs it at a live pace). In the player,
    `XF_SLOW_FRAMES=<ms>[,all]` prints where each frame's time went.
- **Shaders:** commit a changed internal shader together with its compiled
  SPIR-V `.inc`. A build that finds glslc regenerates it.
- **The interface:** changing a header of the generated code
  (`include/xenoforge/game_library.h`, `ppc_runtime.h` and the headers they
  include) or the recompiler changes the interface version. That makes every
  cached game library stale. Say so in the commit message.

## Code style

- C++20, CMake, Ninja and clang. Everything is in namespace `xf`.
- Format with the repository's `.clang-format`: 4 spaces, 120 columns.
- Comments, docs, logs and commit messages are in English.
- Comments say why, not what. A translated function says where it comes
  from.

## Commits and pull requests

- Keep each commit to one change. The subject names the area, then what
  changed, with its switch if it has one:
  ```
  gpu: screen extents reduced over the subgroup in vertex shaders (XF_EXTENTS_SUBGROUP=0 off)
  ```
  Areas in use: `renderer`, `gpu`, `kernel`, `ppc`, `recomp`, `player`,
  `launcher`, `tools`, `build`, `docs`, `tests`.
- The body says why, and gives the measurements and the checks run.
- Open pull requests against `master`. Say what you tested and on which OS
  and GPU, and also what you could not test.

## Reporting problems

Include:

- the XenoForge commit or version;
- the OS, CPU, GPU and driver version;
- the picture settings (`[video]` in `config.toml`);
- the player's log for the game. It is in the `logs` folder beside
  `config.toml`; on Windows that is
  `%APPDATA%\xenoforge\logs\<title id>.log`;
- any crash report it names.

A frame captured with F4 is a GPU trace. It holds the game's data, so share it
privately with the maintainers, never attach it to a public issue.
