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

// ---- the backdrop: the launcher's grey, with ribbons of silk over it
//
// The grey is the gradient the main window used to wear, top to bottom, so
// that the text over it keeps its contrast. The ribbons keep to the two
// margins the list leaves, in fixed colours: at the left a thick dark grey one
// (#6d6d6f) and a thin light grey one (#aaaaaa), at the right four greens of
// different tones (the accent's, darker and lighter). They are always full
// width: they do not thin out and vanish, they fold, like a strip of paper
// folded back and forth: the silhouette zigzags, narrows to a crease where it
// turns, and the face shown changes there (one lighter, one darker). Edges
// are covered to the pixel. Everything is a function of the position and the
// clock; there is no texture and no geometry.
// (This is the version that was checked; the ribbons are to become real 3D
// meshes, docs/backdrop-wip.md.)

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
    const float t = draw.pv.x, aspect = draw.pv.y, strength = draw.pv.z;
    const vec3 tint = draw.stop_colors[0].rgb; // the accent, which the greens are tones of
    // p is in window heights (so the same shapes at any size): across, up to the
    // aspect; down, 0..1. One pixel is px of it.
    const vec2 p = vec2(in_uv.x * aspect, in_uv.y);
    const float px = 1.0 / max(draw.limits.y, 1.0);
    vec3 col = backdrop_grey(in_uv.y);
    // six ribbons, the ones drawn later over the ones drawn before; translucent,
    // so where they cross both show (and so does the list's glass)
    const vec3 hues[6] = vec3[](vec3(0.427, 0.427, 0.435), vec3(0.667), tint * 0.55, tint * 0.80, tint,
                                mix(tint, vec3(1.0), 0.35));
    const float widths[6] = float[](0.100, 0.032, 0.085, 0.065, 0.080, 0.050);
    const float base[6] = float[](0.03, 0.10, 0.88, 0.96, 0.80, 0.93);  // where, in aspects, from the left
    const float amp[6] = float[](0.09, 0.07, 0.10, 0.09, 0.11, 0.08);   // how far it sways
    const float lean[6] = float[](0.10, 0.08, 0.20, 0.17, 0.22, 0.15);  // how far right its top is from its bottom
    const float u = p.y; // along a ribbon
    for (int i = 0; i < 6; i++) {
        const float f = float(i);
        const float sp = 0.75 + 0.10 * f;
        float centre = aspect * (base[i] + 0.03 * sin(t * 0.04 * sp + f * 1.7))
                     + amp[i] * sin(u * (1.3 + 0.17 * f) + t * 0.16 * sp + f * 2.3)
                     + lean[i] * (0.5 - u);
        // the folds: the ribbon is a strip folded back and forth along its length;
        // between two folds it bulges to one side, and at the fold it is narrowest
        const float ph = (u * (3.0 + 0.45 * f) - t * 0.06 * sp + f * 1.9) / 3.14159265;
        const float seg = floor(ph), fr = ph - seg;
        const float side = mod(seg, 2.0) * 2.0 - 1.0; // which face is shown
        const float fold = sin(3.14159265 * fr);      // 0 at a fold, 1 between
        centre += side * 0.045 * fold;
        const float half_w = widths[i] * (0.40 + 0.60 * fold);
        // the distance to its edge in pixels: the slant of the wave counts
        const float slope = abs(dFdy(centre)) / px;
        const float dx = p.x - centre;
        const float cover = clamp((half_w - abs(dx)) / (px * sqrt(1.0 + slope * slope)) + 0.5, 0.0, 1.0);
        // the face turned to the light is lighter; a crease darkens at the fold;
        // a touch of light across the width. The colour stays the one given.
        const float d = dx / max(half_w, px);
        float shade = side > 0.0 ? 1.07 : 0.88;
        shade *= 0.84 + 0.16 * smoothstep(0.0, 0.30, fold);
        shade *= 0.95 + 0.10 * clamp(0.5 - 0.5 * d * side, 0.0, 1.0);
        vec3 silk = min(hues[i] * shade, vec3(0.97));
        col = mix(col, silk, cover * 0.88 * strength);
    }
    // a hair of noise against banding
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
