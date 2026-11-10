// The generic vertex shader (generic.glsl): any guest vertex shader run from
// its program while its translation's pipeline is made, in the host form
// shader::HostVertex::Vertex (the renderer uses it for those draws only), as
// the translator's main_vertex writes that form: the registers zeroed, the
// vertex index in r0.x, the position's formats and the host's viewport, oPts.x
// clamped to the point size's bounds (-1 when the shader writes none), o0-o15
// out. Variants: EXTENTS (VertexOptions::extents: the final position bounds
// the draw's screen extent report), CLIP or CULL (the user clip planes, as
// many as CLIP_PLANES, into clip or cull distances; the six declared, the
// ones past the count at 1, which neither clips nor culls).

#version 460
#define GENERIC_VERTEX
#include "generic.glsl"

layout(location = 0) out vec4 interpolators[16];
layout(location = 16) out float point_size;
invariant gl_Position;
layout(constant_id = 22) const uint CLIP_PLANES = 0u;
#ifdef CLIP
out float gl_ClipDistance[6];
#endif
#ifdef CULL
out float gl_CullDistance[6];
#endif

#ifdef EXTENTS
layout(set = 0, binding = 7, std430) buffer Extents { uint words[]; } extents;

// the final position bounding its report: xy over w, clamped to the NDC
// square, mapped to console pixels, the edges as whole pixels outwards; the
// whole viewport for a position not in front of the eye or not a number
void bound_extent(vec4 p) {
    float w = p.w;
    vec2 ndc = p.xy / vec2(w, w);
    bool valid = w > 0.0 && !any(isnan(ndc));
    vec2 lo = valid ? clamp(ndc, vec2(-1.0), vec2(1.0)) : vec2(-1.0);
    vec2 hi = valid ? clamp(ndc, vec2(-1.0), vec2(1.0)) : vec2(1.0);
    vec4 map = sys.extent_map;
    vec2 scale = map.xz, offset = map.yw;
    vec2 a = lo * scale + offset, b = hi * scale + offset;
    vec2 low = clamp(floor(min(a, b)), vec2(0.0), vec2(8192.0));
    vec2 high = clamp(ceil(max(a, b)), vec2(0.0), vec2(8192.0));
    uvec2 low_u = uvec2(low), high_u = uvec2(high);
    uint first = sys.extent_report.x;
    atomicMax(extents.words[first], 8192u - low_u.x);
    atomicMax(extents.words[first + 1u], 8192u - low_u.y);
    atomicMax(extents.words[first + 2u], high_u.x + 1u);
    atomicMax(extents.words[first + 3u], high_u.y + 1u);
}
#endif

void main() {
    for (uint k = 0u; k < 128u; k++) r[k] = vec4(0.0);
    r[0] = vec4(float(gl_VertexIndex), 0.0, 0.0, 0.0);
    reset_state();
    run_program();
    // the position's formats (PA_CL_VTE_CNTL, the system flags' y): W given
    // as 1/W unless bit 0, XY divided by W with bit 1, Z with bit 2
    uint flags = sys.flags.y;
    vec4 p = oPos;
    precise float inverse = 1.0 / p.w;
    float w = (flags & 1u) != 0u ? p.w : inverse;
    precise float x = p.x * w, y = p.y * w, z = p.z * w;
    vec4 guest = vec4((flags & 2u) != 0u ? x : p.x, (flags & 2u) != 0u ? y : p.y, (flags & 4u) != 0u ? z : p.z, w);
    precise vec4 moved = guest * sys.ndc_scale + sys.ndc_offset * guest.w;
    vec4 position = vec4(moved.xyz, guest.w);
#if defined(CLIP) || defined(CULL)
    for (uint k = 0u; k < 6u; k++) {
        float distance = k < CLIP_PLANES ? dot(guest, sys.clip_planes[k]) : 1.0;
#ifdef CLIP
        gl_ClipDistance[k] = distance;
#else
        gl_CullDistance[k] = distance;
#endif
    }
#endif
    gl_Position = position;
#ifdef EXTENTS
    bound_extent(position);
#endif
    for (uint k = 0u; k < 16u; k++) interpolators[k] = o[k];
    float size = -1.0;
    if ((program_flags() & 8u) != 0u) {
        int bits = max(floatBitsToInt(sys.values.y), floatBitsToInt(oPts.x));
        size = intBitsToFloat(min(floatBitsToInt(sys.values.z), bits));
    }
    point_size = size;
}
