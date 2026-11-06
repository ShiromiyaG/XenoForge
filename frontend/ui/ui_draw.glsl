// The uniforms of one draw (std140), shared by ui.vert and ui.frag, and
// ui_renderer.cpp's DrawUniforms, which must match field for field.

#define MAX_STOPS 16

layout(set = 0, binding = 0, std140) uniform Draw {
    mat4 transform;                // projection * the element's transform
    mat4 color_matrix;             // filters: brightness, contrast, ...
    vec4 stop_colors[MAX_STOPS];   // gradients: premultiplied
    vec4 stop_positions[4];        // gradients: 16 floats
    ivec4 imode;                   // x: mode, y: gradient function, z: stops, w: layer pass (vertex stage)
    vec4 translate;                // xy: the draw's translation
    vec4 pv;                       // gradients: p (xy) and v (zw)
    vec4 color;                    // multiplies the texture (opacity, drop shadow's color)
    vec4 limits;                   // texture coordinates' region: min (xy), max (zw)
    vec4 blur_weights;             // blur: the weights of a tap and its neighbours
    vec4 texel;                    // blur: the step between taps, in texture coordinates (xy)
    vec4 uv_transform;             // layer passes: scale (xy) and offset (zw) of the texture coordinates
} draw;
