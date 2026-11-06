// The host vertex shader of tessellated draws: a control point's two values,
// made by the renderer on the CPU, passed on to the hull shader
// (tessellation.tesc). As Xenia's tessellation_indexed.vs and
// tessellation_adaptive.vs (BSD, see LICENSE-xenia), but with what they
// compute done where the renderer reads the indices: x is the control point's
// index for the domain shader, swapped, offset, cut to 24 bits and clamped to
// VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX (for adaptive tessellation the
// patch's index, the primitive's plus the offset, as Xenia's adaptive hull
// shaders make it); y is its edge factor, the float the guest put in the
// index buffer plus 1, clamped to VGT_HOS_MIN_TESS_LEVEL and
// VGT_HOS_MAX_TESS_LEVEL plus 1 (adaptive), or the latter alone.
#version 450

layout(location = 0) in vec2 in_control_point;
layout(location = 0) out vec2 out_control_point;

void main() { out_control_point = in_control_point; }
