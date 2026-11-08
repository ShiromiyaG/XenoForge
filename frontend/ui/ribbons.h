// The launcher's backdrop ribbons (docs/runtime.md, "The backdrop"): silk
// ribbons in three dimensions, made as triangles on the CPU every frame and
// drawn by the UI renderer under the interface.
//
// Each ribbon is a strip of constant width along a space curve (they all sweep
// down the window the same way, leaning and bowed, with slight differences,
// and wind round a small spiral that turns slowly with the clock) and is
// twisted a little about its length, so it catches the light and, here and
// there, turns edge-on and passes over or under a neighbour.
// The picture is a perspective projection; the triangles are lit (a diffuse
// term, a silk sheen, the back face a little darker) and opaque: the renderer
// draws them with a depth test (each vertex has its depth), so folds and
// crossings are exact; each edge has a one pixel fringe of falling alpha (its
// own list of triangles, drawn after, tested but not written to the depth) to
// stand in for multisampling. At the left two greys (a thick dark one, #6d6d6f, and a thin
// light one, #aaaaaa), at the right four greens, tones of the tint.
//
// A pure function of its arguments: no device, no state.

#pragma once

#include <cstdint>
#include <vector>

namespace xf::ui {

struct RibbonVertex {
    float x, y;            // pixels, from the top left of the window
    uint8_t r, g, b, a;    // premultiplied
    float depth;           // 0 (near) to 1 (far)
};

// `time` in seconds; `tint` the greens' colour (0..1); `alpha` multiplies the
// ribbons' opacity (0 makes none). The vertices, the indices of the triangles
// of the surface (in any order) and those of the edges' fringes; all three are
// cleared first.
// `greys` (two colours, six floats: the thick ribbon's and the thin one's) replaces the defaults, #6d6d6f and
// #aaaaaa, which suit the dark backdrop; the light one wants paler ones.
void build_ribbons(float time, uint32_t width, uint32_t height, const float tint[3], float alpha,
                   std::vector<RibbonVertex> &vertices, std::vector<uint32_t> &body, std::vector<uint32_t> &fringe,
                   const float *greys = nullptr);

} // namespace xf::ui
