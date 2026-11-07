# Launcher backdrop: 3D ribbons (work in progress)

Notes to continue from (2026-11-07). What is committed is the backdrop with its
ribbons drawn in the fragment shader (checked: built, `frontend_test` passed,
pictures taken). What is not committed is the replacement: ribbons as real 3D
meshes. This file says what is wanted, what exists and what is left.

## What the owner wants (the mock is `mock.webp`, untracked in the repo root)

- Silk ribbons behind the launcher, as in the mock: broad, crisp, in layers,
  overlapping, at the two margins the list leaves.
- **Fixed colours**, they never change (not with the selected game):
  - left, 2 ribbons: a thick dark grey `#6d6d6f` and a thin light grey `#aaaaaa`;
  - right, 4 ribbons: greens of different tones (now tones of the accent:
    x0.55, x0.80, x1.0 and 35% to white; the default accent is green).
- **No thinning out and fading** (that was tried and rejected). Ribbons stay at
  full width.
- **"Dobrando" means real folding in space**: the owner confirmed they are 3D
  (option 1 of the question asked): a strip of constant width that twists and
  curls, shows its other face where it turns, goes edge-on, and passes over or
  under itself. The zigzag-with-a-crease in the committed shader is only a
  stand-in and is not what was meant.
- Crisp (not blurred, not soft), slow motion, never a hairline.
- The header, the toolbar and the game rows are translucent so that the ribbons
  show through (done, in `launcher.rcss`).

## What is committed

- `ui.frag`, `MODE_BACKDROP`: the grey gradient and the six folding ribbons as a
  function of position and clock. `Renderer::set_backdrop(Backdrop{on, time,
  strength, tint})`; `LauncherView::update_backdrop` feeds it every frame (the
  clock runs while the window is visible, the tint is the accent).
- `[launcher] backdrop = "animated" | "still" | "off"` and the Interface-tab
  select. (The "ribbon colour follows the game" option was removed on purpose.)
- `#app` and `body` transparent; header, toolbar and rows with alpha.
- `docs/runtime.md`, "The backdrop", describes the committed shader version.
  Update it when the mesh replaces it.

## The WIP (uncommitted in the working tree)

Files: `frontend/ui/ribbons.h`, `frontend/ui/ribbons.cpp` (new), and edits to
`frontend/ui/ui.frag` (ribbons taken out, the gradient stays),
`frontend/ui/ui_renderer.cpp` (draws the mesh after the gradient pass),
`CMakeLists.txt` (`ribbons.cpp` in `xf_ui`), `tests/frontend_test.cpp` (a test of
the mesh). A copy of these is in the session's scratchpad (`.../scratchpad/wip/`).

Design, as written, **never compiled or run**:

- `build_ribbons(time, width, height, tint, alpha, vertices, indices)` is a pure
  CPU function. Each ribbon: a centre line in 3D (sweeps down the window,
  leans up to the right, sways, and winds round a spiral of radius ~0.07-0.12
  that turns with the clock), a frame from the tangent, and the width's
  direction rotated about the tangent by a twist angle (`theta = twist +
  0.9*phi + 0.6*sin(...)`), so it goes edge-on and shows the back face.
  Constant half width (0.034 to 0.100 window heights).
- Perspective projection (camera 2.2 heights away). Per vertex lighting:
  diffuse, a silk sheen, the surface bent a little across the width, the back
  face x0.86. 150 sections x 7 vertices across (2 are a one-pixel fringe of
  alpha 0, standing in for multisampling) x 6 ribbons.
- Triangles sorted far to near by depth and drawn blended (alpha 0.9) with the
  UI's own geometry path: the renderer builds `Rml::Vertex` (premultiplied
  colour), `alloc_geometry`, `draw` with `M_TEXTURE` on the white texel and
  `projection` as the transform, then frees the geometry in `defer`.

Left to do, in order:

1. Get the build going again (see the next section), build, run `frontend_test`
   (the mesh test: finite, indices in range, premultiplied, deterministic,
   moves with the clock, none at strength 0).
2. Look at pictures: `build/launcher.exe --config <tmp.toml> --demo-games
   --render-size 1240x760 --snap-at <s> --screenshot <png>` at several times;
   compare with `mock.webp`. Expect to tune: the centre lines (so that they
   stay in the margins and overlap), the spiral radius and the twist (to get
   real folds that go over themselves, not just wobble), the lighting (the
   greys must read as `#6d6d6f` and `#aaaaaa`), the fringe, the opacity.
3. Check the cost (6k vertices, 10k triangles, sorted every frame; the launcher
   draws every frame).
4. Vulkan validation on (`XF_VK_VALIDATION=1`): one geometry allocated and
   freed per frame.
5. Replace the shader ribbons in `docs/runtime.md`; remove the note in
   `ui.frag`'s comment pointing here; delete this file.

Open questions for the owner: how many folds should be visible at once; whether
the ribbons should also pass behind the list (they do now: it is translucent).

## The build environment (the session left it broken)

- The shell does not have the Visual Studio environment: run `vcvars64.bat`
  first (the compiler finds no C++ headers otherwise), put
  `C:\Users\TwistZero\tools\llvm\bin` on `PATH` and set
  `VULKAN_SDK=C:/VulkanSDK/1.4.363.1` (`find_path(SPIRV_HEADERS_DIR ...)` needs
  it). The VS CMake is at `...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- **A bare `cmake -S . -B build` wiped `build/CMakeCache.txt`** (the generator
  was not given), so the options set before are gone: `XF_PLAYER` is back to
  OFF (the `launcher` target does not exist until `-DXF_PLAYER=ON`), `XF_GAMES`,
  `XF_GAME_LIBS`, `XF_PPC_TESTS_DIR` and `STB_DIR` are empty. Configure again
  with the command in `CLAUDE.md` plus `-DXF_PLAYER=ON`, then the others.
- `cmake/package.cmake` (an uncommitted change of the owner's) reads
  `${STB_DIR}/stb_image_write.h`, and `STB_DIR` defaults to
  `../xenia-canary/third_party/stb`, which does not exist beside this checkout
  (`C:\Users\TwistZero\repos\`). The old cache must have had it pointing at
  Xenia's copy (probably under `F:\RECOMPs`); find it and pass `-DSTB_DIR=...`.
- `build/launcher.exe` is the last one that linked (02:01), which should be the
  committed shader-ribbons version (not checked after the cache was wiped).
