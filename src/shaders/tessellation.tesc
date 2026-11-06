// The hull shaders of tessellated draws, as Xenia's (shaders/*.hs.hlsl and
// their .hs.glsl; BSD, see LICENSE-xenia): the control points' indices
// passed on to the domain shader (the guest's vertex shader, translated as a
// tessellation evaluation shader), and the tessellation levels.
//
// Defines: CONTROL_POINTS, the input patch's size (1, 3 or 4); ADAPTIVE with
// QUAD or without it, the adaptive hull shaders, which take a patch of edge
// factors and give one control point, the patch's index. The partitioning
// (integer for discrete, fractional even otherwise) is the evaluation
// shader's, as SPIR-V has it.
//
// Each input control point (tessellation.vert) is the index for the domain
// shader in x and the edge factor in y.
#version 450

#ifdef ADAPTIVE
layout(vertices = 1) out;
#else
layout(vertices = CONTROL_POINTS) out;
#endif

layout(location = 0) in vec2 in_control_point[];
layout(location = 0) out float out_index[];

void main() {
    out_index[gl_InvocationID] = in_control_point[gl_InvocationID].x;
    if (gl_InvocationID != 0) return;
#if defined(ADAPTIVE) && defined(QUAD)
    // adaptive_quad.hs: the Xbox 360's factors go along the perimeter from
    // U0V0 to U1V0 (the first), then U1V1, U0V1; Direct3D's and Vulkan's
    // first edge is U0's (U0V1 to U0V0), so edge i takes factor (i + 3) & 3.
    // The inner levels are the minimum of the edges' along each axis.
    float edges[4];
    for (int i = 0; i < 4; i++) edges[i] = in_control_point[(i + 3) & 3].y;
    for (int i = 0; i < 4; i++) gl_TessLevelOuter[i] = edges[i];
    gl_TessLevelInner[0] = min(edges[1], edges[3]);
    gl_TessLevelInner[1] = min(edges[0], edges[2]);
#elif defined(ADAPTIVE)
    // adaptive_triangle.hs: the Xbox 360's factor 0 is the v0 to v1 edge, 1
    // v1 to v2, 2 v2 to v0; Direct3D's and Vulkan's edge 0 is U0's (v1 to
    // v2), so edge i takes factor (i + 1) % 3. The inner level is the
    // minimum of the edges'.
    float edges[3];
    for (int i = 0; i < 3; i++) edges[i] = in_control_point[(i + 1) % 3].y;
    for (int i = 0; i < 3; i++) gl_TessLevelOuter[i] = edges[i];
    gl_TessLevelInner[0] = min(min(edges[0], edges[1]), edges[2]);
#else
    // discrete_*.hs and continuous_*.hs: every level is the largest factor
    // (VGT_HOS_MAX_TESS_LEVEL plus 1), the Xenos's uniform grid being out of
    // reach of the PC's tessellator; a triangle's take the first three outer
    // levels and the first inner one
    float factor = in_control_point[0].y;
    for (int i = 0; i < 4; i++) gl_TessLevelOuter[i] = factor;
    gl_TessLevelInner[0] = factor;
    gl_TessLevelInner[1] = factor;
#endif
}
