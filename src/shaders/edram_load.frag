// EDRAM into a render target, per sample, in the tiles the mask marks
// (renderer.cpp). DEPTH: a depth target; STENCIL: with the stencil exported
// too; without, the stencil is written a bit per pass, the passes for the
// bits not set discarded (as Xenia does). BSD; see LICENSE-xenia.
#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef STENCIL
#extension GL_ARB_shader_stencil_export : require
#endif

layout(set = 0, binding = 0) readonly buffer Edram { uint words[]; } edram;
layout(set = 0, binding = 1) readonly buffer Mask { uint tiles[]; } mask;

#include "edram.glsl"

#ifndef DEPTH
layout(location = 0) out vec4 color;
#endif

void main() {
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint at = edram_offset(pixel, guest_sample(uint(gl_SampleID)));
    if (mask.tiles[at / TILE_DWORDS] == 0u) discard;
    uint low = edram.words[at];
#ifdef DEPTH
    gl_FragDepth = unpack_depth(low);
#ifdef STENCIL
    gl_FragStencilRefARB = int(low & 0xFFu);
#else
    if (target.stencil_bit < 8u && ((low >> target.stencil_bit) & 1u) == 0u) discard;
#endif
#else
    uint high = target.ints_log2 != 0u ? edram.words[(at + 1u) % EDRAM_DWORDS] : 0u;
    color = unpack_color(uvec2(low, high));
#endif
}
