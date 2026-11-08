#version 450
#extension GL_GOOGLE_include_directive : require
// The launcher's vertex stage (frontend/ui/ui_renderer.cpp): RmlUi's vertices
// (position in pixels, premultiplied color, texture coordinates) moved by the
// element's transform and the draw's translation, or, for the full-layer
// passes of the filters (imode.w = 1), taken as clip-space positions with the
// texture coordinates scaled and offset; imode.w = 2 is the backdrop's ribbons,
// whose depth (0 near, 1 far) is in the first texture coordinate.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

#include "ui_draw.glsl"

void main() {
    out_color = in_color;
    if (draw.imode.w == 1) {
        out_uv = in_uv * draw.uv_transform.xy + draw.uv_transform.zw;
        gl_Position = vec4(in_position, 0.0, 1.0);
    } else {
        out_uv = in_uv;
        vec4 p = draw.transform * vec4(in_position + draw.translate.xy, 0.0, 1.0);
        // RmlUi's projection is OpenGL's (depth in -1..1); Vulkan clips to 0..1
        p.z = (p.z + p.w) * 0.5;
        if (draw.imode.w == 2) {
            // the backdrop's ribbons: the depth comes in the first texture coordinate
            p.z = in_uv.x * p.w;
            out_uv = vec2(0.5);
        }
        gl_Position = p;
    }
}
