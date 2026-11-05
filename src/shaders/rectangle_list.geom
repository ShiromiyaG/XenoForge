// Xenos rectangle lists: three vertices of a rectangle, the fourth made by
// mirroring the vertex opposite the longest edge (the diagonal) across it,
// position and interpolators alike. As Xenia's rectangle list geometry
// shader does (vulkan_pipeline_cache.cc): the strip starts at the vertex
// opposite the longest edge.
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) in vec4 in_interpolators[][16];
layout(location = 0) out vec4 out_interpolators[16];

void emit_vertex(int v) {
    gl_Position = gl_in[v].gl_Position;
    for (int k = 0; k < 16; k++) out_interpolators[k] = in_interpolators[v][k];
    EmitVertex();
}

float edge(int a, int b) {
    vec2 d = gl_in[b].gl_Position.xy - gl_in[a].gl_Position.xy;
    return dot(d, d);
}

void main() {
    float e12 = edge(1, 2), e20 = edge(2, 0), e01 = edge(0, 1);
    int a = (e12 > e20 && e12 > e01) ? 0 : (e20 > e01 ? 1 : 2);
    int b = a + 1 < 3 ? a + 1 : a - 2;
    int c = a + 2 < 3 ? a + 2 : a - 1;
    emit_vertex(a);
    emit_vertex(b);
    emit_vertex(c);
    gl_Position = -gl_in[a].gl_Position + gl_in[b].gl_Position + gl_in[c].gl_Position;
    for (int k = 0; k < 16; k++)
        out_interpolators[k] = -in_interpolators[a][k] + in_interpolators[b][k] + in_interpolators[c][k];
    EmitVertex();
    EndPrimitive();
}
