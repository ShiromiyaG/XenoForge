// Xenos point lists: each point a sprite of two triangles, as Xenia's point
// list geometry shader makes it (vulkan_pipeline_cache.cc,
// GetGeometryShader, kPointList; BSD, see LICENSE-xenia). The diameter is
// the vertex shader's (oPts.x, already clamped, location 16) or, where it
// writes none (-1), PA_SU_POINT_SIZE's; points with a position that is NaN
// or a diameter that is not positive are dropped. The interpolators are the
// point's on all four corners: top-left, bottom-left, top-right,
// bottom-right.
#version 450

layout(points) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) in vec4 in_interpolators[][16];
layout(location = 16) in float in_point_size[];
layout(location = 0) out vec4 out_interpolators[16];

// renderer.cpp's System, up to the point constants: the constant diameter
// in guest pixels (xy), the guest pixel diameter to NDC radius (zw)
layout(set = 0, binding = 2) uniform System {
    vec4 ndc_scale;
    vec4 ndc_offset;
    uvec4 slots[8];
    uvec4 flags;
    vec4 values;
    vec4 color_scales;
    vec4 point;
} system;

void main() {
    vec4 position = gl_in[0].gl_Position;
    if (any(isnan(position))) return;
    vec2 diameter = in_point_size[0] >= 0.0 ? vec2(in_point_size[0]) : system.point.xy;
    if (!(diameter.x > 0.0 && diameter.y > 0.0)) return;
    // the diameter in guest pixels to the radius in clip space, without
    // fused multiply-adds (as Xenia's NoContraction)
    precise vec2 radius = diameter * system.point.zw * position.w;
    precise vec2 low = position.xy - radius, high = position.xy + radius;
    for (int i = 0; i < 4; i++) {
        gl_Position = vec4((i >> 1) != 0 ? high.x : low.x, (i & 1) != 0 ? high.y : low.y, position.zw);
        for (int k = 0; k < 16; k++) out_interpolators[k] = in_interpolators[0][k];
        EmitVertex();
    }
    EndPrimitive();
}
