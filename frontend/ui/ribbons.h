// The launcher's backdrop ribbons (docs/runtime.md, "The backdrop"): silk
// ribbons in three dimensions, made as triangles on the CPU every frame and
// drawn by the UI renderer under the interface.
//
// Each ribbon is a strip of constant width along a space curve (it sweeps down
// the window, leaning, and winds round a spiral that turns slowly with the
// clock) and is twisted about its length as it goes, so where it turns it
// shows its other face, passes edge-on and, curled, goes over or under itself.
// The picture is a perspective projection; the triangles are lit (a diffuse
// term, a silk sheen, the back face a little darker), sorted far to near and
// blended, each edge with a one pixel fringe of falling alpha to stand in for
// multisampling. At the left two greys (a thick dark one, #6d6d6f, and a thin
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
};

// `time` in seconds; `tint` the greens' colour (0..1); `alpha` multiplies the
// ribbons' opacity (0 makes none). The vertices and the indices of the
// triangles in the order to draw them (far to near); both are cleared first.
void build_ribbons(float time, uint32_t width, uint32_t height, const float tint[3], float alpha,
                   std::vector<RibbonVertex> &vertices, std::vector<uint32_t> &indices);

} // namespace xf::ui
