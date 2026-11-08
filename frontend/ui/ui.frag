#version 450
#extension GL_GOOGLE_include_directive : require
// The launcher's fragment stage: what RmlUi's render interface draws, as one
// shader with a mode (the 3DS runtime's and RmlUi's GL3 backend's shaders,
// folded): the texture times the vertex color, gradients, a color matrix, a
// mask, a drop shadow's silhouette and a Gaussian blur's pass. Everything is
// premultiplied alpha, blended in non-linear space, as RmlUi expects.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

#include "ui_draw.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 0) uniform sampler2D mask_tex;

const int MODE_TEXTURE = 0, MODE_GRADIENT = 1, MODE_COLOR_MATRIX = 2, MODE_MASK = 3, MODE_SHADOW = 4, MODE_BLUR = 5,
          MODE_CLAMPED = 6, MODE_BACKDROP = 7;
const int LINEAR = 0, RADIAL = 1, CONIC = 2, REPEATING_LINEAR = 3, REPEATING_RADIAL = 4, REPEATING_CONIC = 5;
const float PI = 3.14159265;

vec4 gradient_color(float t) {
    vec4 c = draw.stop_colors[0];
    for (int i = 1; i < draw.imode.z; i++) {
        const float a = draw.stop_positions[(i - 1) >> 2][(i - 1) & 3], b = draw.stop_positions[i >> 2][i & 3];
        c = mix(c, draw.stop_colors[i], smoothstep(a, b, t));
    }
    return c;
}

float blur_weight(int i) { return draw.blur_weights[abs(i)]; }

// ---- the backdrop: the launcher's grey
//
// The gradient the main window used to wear, top to bottom, so that the text
// over it keeps its contrast, and a hair of noise against banding. The ribbons
// over it are triangles (ribbons.cpp), drawn after this by the renderer.

const vec3 BACKDROP_GREY[10] = vec3[](
    vec3(0.247, 0.239, 0.263), vec3(0.247, 0.239, 0.263), vec3(0.278, 0.271, 0.294), vec3(0.345, 0.341, 0.361),
    vec3(0.420, 0.416, 0.431), vec3(0.478, 0.478, 0.486), vec3(0.604, 0.604, 0.596), vec3(0.690, 0.682, 0.690),
    vec3(0.729, 0.722, 0.733), vec3(0.769, 0.757, 0.769));
const float BACKDROP_AT[10] = float[](0.0, 0.12, 0.21, 0.29, 0.47, 0.58, 0.69, 0.81, 0.90, 1.0);

vec3 backdrop_grey(float y) {
    vec3 c = BACKDROP_GREY[0];
    for (int i = 1; i < 10; i++)
        c = mix(c, BACKDROP_GREY[i], clamp((y - BACKDROP_AT[i - 1]) / (BACKDROP_AT[i] - BACKDROP_AT[i - 1]), 0.0, 1.0));
    return c;
}

float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

vec3 backdrop() {
    vec3 col = backdrop_grey(in_uv.y);
    col += (hash12(gl_FragCoord.xy) - 0.5) / 255.0;
    return clamp(col, 0.0, 1.0);
}

void main() {
    const int mode = draw.imode.x;
    if (mode == MODE_TEXTURE) {
        out_color = in_color * draw.color * texture(tex, in_uv);
    } else if (mode == MODE_GRADIENT) {
        const int func = draw.imode.y;
        const vec2 p = draw.pv.xy, v = draw.pv.zw;
        float t = 0.0;
        if (func == LINEAR || func == REPEATING_LINEAR) {
            t = dot(v, in_uv - p) / dot(v, v);
        } else if (func == RADIAL || func == REPEATING_RADIAL) {
            t = length(v * (in_uv - p));
        } else {
            const mat2 r = mat2(v.x, -v.y, v.y, v.x);
            const vec2 d = r * (in_uv - p);
            t = 0.5 + atan(-d.x, d.y) / (2.0 * PI);
        }
        if (func >= REPEATING_LINEAR) {
            const float t0 = draw.stop_positions[0].x;
            const int last = draw.imode.z - 1;
            const float t1 = draw.stop_positions[last >> 2][last & 3];
            t = t0 + mod(t - t0, t1 - t0);
        }
        out_color = in_color * gradient_color(t);
    } else if (mode == MODE_BACKDROP) {
        out_color = vec4(backdrop(), 1.0);
    } else if (mode == MODE_COLOR_MATRIX) {
        const vec4 c = texture(tex, in_uv);
        out_color = vec4(vec3(draw.color_matrix * c), c.a);
    } else if (mode == MODE_MASK) {
        out_color = texture(tex, in_uv) * texture(mask_tex, in_uv).a;
    } else if (mode == MODE_SHADOW) {
        const vec2 inside = step(draw.limits.xy, in_uv) * step(in_uv, draw.limits.zw);
        out_color = texture(tex, in_uv).a * inside.x * inside.y * draw.color;
    } else if (mode == MODE_BLUR) {
        vec4 sum = vec4(0.0);
        for (int i = -3; i <= 3; i++) {
            const vec2 uv = in_uv + float(i) * draw.texel.xy;
            const vec2 inside = step(draw.limits.xy, uv) * step(uv, draw.limits.zw);
            sum += texture(tex, uv) * inside.x * inside.y * blur_weight(i);
        }
        out_color = sum;
    } else {
        out_color = in_color * draw.color * texture(tex, clamp(in_uv, draw.limits.xy, draw.limits.zw));
    }
}
