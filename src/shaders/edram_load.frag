// EDRAM into a render target, per sample, in the tiles the mask marks
// (renderer.cpp). DEPTH: a depth target; STENCIL: with the stencil exported
// too; without, the stencil is written a bit per pass, the passes for the
// bits not set discarded (as Xenia does). A depth target whose format the
// host encodes differently takes the host's depth kept beside EDRAM where it
// still rounds to EDRAM's 24 bits, else EDRAM's (Xenia's
// kDepthAndHostDepthToDepth and kColorAndHostDepthToDepth transfers). BSD;
// see LICENSE-xenia.
#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef STENCIL
#extension GL_ARB_shader_stencil_export : require
#endif

layout(set = 0, binding = 0) readonly buffer Edram { uint words[]; } edram;
layout(set = 0, binding = 1) readonly buffer Mask { uint tiles[]; } mask;
layout(set = 0, binding = 2) readonly buffer HostDepth { uint words[]; } host_depth;

#include "edram.glsl"

#if defined(UINT)
layout(location = 0) out uvec4 color; // a float target's bits, through its UINT view (edram.glsl's flags bit 3)
#elif !defined(DEPTH)
layout(location = 0) out vec4 color;
#endif

void main() {
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint at = edram_offset(pixel, guest_sample(uint(gl_SampleID)));
    if (mask.tiles[at / TILE_DWORDS] == 0u) discard;
    uint low = edram.words[at];
#ifdef DEPTH
    float depth = unpack_depth(low);
    // The last depth target of this format to hold the sample left its depth
    // beside EDRAM (zeros before any did, which round to 0, the same as
    // EDRAM's 0): if it still rounds to the guest's bits, nothing has written
    // the sample since but a transfer, and the host's precision comes back.
    if ((target.flags & 1u) != 0u && target.stencil_bit >= 8u) {
        float host = uintBitsToFloat(host_depth.words[host_depth_offset(at)]);
        if (depth_to_guest(host) == (low >> 8u)) depth = host;
    }
    gl_FragDepth = depth;
#ifdef STENCIL
    gl_FragStencilRefARB = int(low & 0xFFu);
#else
    if (target.stencil_bit < 8u && ((low >> target.stencil_bit) & 1u) == 0u) discard;
#endif
#else
    uint high = target.ints_log2 != 0u ? edram.words[(at + 1u) % EDRAM_DWORDS] : 0u;
#ifdef UINT
    color = unpack_raw(uvec2(low, high));
#else
    color = unpack_color(uvec2(low, high));
#endif
#endif
}
