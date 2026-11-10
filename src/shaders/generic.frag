// The generic pixel shader (generic.glsl): any guest pixel shader run from
// its program while its translation's pipeline is made, as the translator's
// main_pixel writes it: o0-o15 into r0-r15, the pixel parameters, the alpha
// test and alpha to coverage, then the colors scaled, multiplied for a
// minimum or maximum blend (by the system constants, which are zero, so
// nothing, where the draw takes none) and to the gamma curve; or on the
// interlock path (FSI=1 pixel, 2 sample interlock) the output merger of
// edram_fsi.glsl inside the interlock, with the render targets' state from
// the draw's uniform block (no specialization). Variants: WRITES_DEPTH (the
// shader writes oDepth), POINT_COORDINATES (a point list's sprite
// coordinates in the pixel parameters). Not taken: the float24 depth
// conversion, the early depth hint (the state then has neither an alpha test
// nor alpha to coverage, so the same pixels pass) and the centroids (the
// interpolators at the pixel's center; the renderer counts it).

#version 460
#extension GL_EXT_demote_to_helper_invocation : require
#ifdef FSI
#extension GL_ARB_fragment_shader_interlock : require
#endif
#define GENERIC_PIXEL
#include "generic.glsl"
#ifdef FSI
#include "edram_fsi.glsl"
#if FSI == 2
layout(sample_interlock_ordered) in;
#else
layout(pixel_interlock_ordered) in;
#endif
#else
layout(location = 0) out vec4 colors[4];
#endif
layout(location = 0) in vec4 interpolators[16];
#ifdef POINT_COORDINATES
layout(location = 17) in vec2 point_coordinates;
#endif

// a host pixel's position as the console's at a resolution scale
float console_position(float v) {
    float per_pixel = sys.values.w;
    per_pixel = per_pixel == 0.0 ? 1.0 : per_pixel;
    precise float p = v * per_pixel;
    return p;
}

float linear_to_pwl(float linear) {
    float l = clamp(linear, 0.0, 1.0);
    bool high = l >= 512.0 / 1023.0, mid = l >= 128.0 / 1023.0, low = l >= 64.0 / 1023.0;
    float scale = mid ? (high ? 1023.0 / 8.0 : 1023.0 / 4.0) : (low ? 1023.0 / 2.0 : 1023.0);
    float offset = mid ? (high ? 128.0 / 255.0 : 64.0 / 255.0) : (low ? 32.0 / 255.0 : 0.0);
    precise float scaled = l * scale;
    precise float result = trunc(scaled) * (1.0 / 255.0) + offset;
    return result;
}

// the factors of a minimum or maximum blend (PixelOptions::blend_premultiply)
float fixed_value(float v, bool is_fixed) { return is_fixed ? clamp(isnan(v) ? 0.0 : v, 0.0, 1.0) : v; }
float premultiplied_half(uint word, uint j, uint shift, vec4 source, vec4 constant, float color, bool is_fixed) {
    uint factor = bitfieldExtract(word, int(shift), 5), zero_op = bitfieldExtract(word, int(shift + 5u), 2);
    float by = 1.0;
    precise float one_minus_s = 1.0 - source[j], one_minus_sa = 1.0 - source.w;
    precise float one_minus_k = 1.0 - constant[j], one_minus_ka = 1.0 - constant.w;
    by = factor == 4u ? source[j] : by;
    by = factor == 5u ? one_minus_s : by;
    by = factor == 6u ? source.w : by;
    by = factor == 7u ? one_minus_sa : by;
    by = factor == 12u ? constant[j] : by;
    by = factor == 13u ? one_minus_k : by;
    by = factor == 14u ? constant.w : by;
    by = factor == 15u ? one_minus_ka : by;
    precise float product = source[j] * by;
    float r_ = fixed_value(product, is_fixed);
    r_ = factor == 0u ? 0.0 : r_;
    r_ = zero_op == 1u ? min(r_, 0.0) : zero_op == 2u ? max(r_, 0.0) : r_;
    return (word & (1u << (shift + 7u))) != 0u ? r_ : color;
}
vec4 premultiplied(vec4 color, uint k) {
    uint word = sys.blend_premultiply[k];
    bool fixed_color = (word & (1u << 16u)) != 0u, fixed_alpha = (word & (1u << 17u)) != 0u;
    vec4 source, constant;
    for (uint j = 0u; j < 4u; j++) {
        source[j] = fixed_value(color[j], j < 3u ? fixed_color : fixed_alpha);
        constant[j] = fixed_value(sys.blend_constant[j], j < 3u ? fixed_color : fixed_alpha);
    }
    return vec4(premultiplied_half(word, 0u, 0u, source, constant, color.x, fixed_color),
                premultiplied_half(word, 1u, 0u, source, constant, color.y, fixed_color),
                premultiplied_half(word, 2u, 0u, source, constant, color.z, fixed_color),
                premultiplied_half(word, 3u, 8u, source, constant, color.w, fixed_alpha));
}

void main() {
    for (uint k = 0u; k < 128u; k++) r[k] = k < 16u ? interpolators[k] : vec4(0.0);
    uint program = program_flags();
    // the pixel parameters (SQ_PROGRAM_CNTL's param_gen) into the
    // interpolator SQ_CONTEXT_MISC names, by the system flags' y
    uint flags_y = sys.flags.y;
    if ((flags_y & (1u << 8u)) != 0u) {
        float x = abs(floor(console_position(gl_FragCoord.x)));
        float y = abs(floor(console_position(gl_FragCoord.y)));
        bool back = (flags_y & (1u << 15u)) != 0u && !gl_FrontFacing;
        x = back ? uintBitsToFloat(floatBitsToUint(x) ^ 0x80000000u) : x;
        y = (flags_y & (1u << 13u)) != 0u ? uintBitsToFloat(floatBitsToUint(y) ^ 0x80000000u) : y;
        float z = (flags_y & (1u << 14u)) != 0u ? -0.0 : 0.0;
        float w = 0.0;
#ifdef POINT_COORDINATES
        z = nclamp(point_coordinates.x, 0.0, 1.0);
        w = nclamp(point_coordinates.y, 0.0, 1.0);
#endif
        uint index = bitfieldExtract(flags_y, 9, 4);
        vec4 value = vec4(x, y, z, w);
        for (uint k = 0u; k < 16u; k++) r[k] = index == k ? value : r[k];
    }
    reset_state();
    run_program();
#ifdef WRITES_DEPTH
#ifndef FSI
    {
        // oDepth clamped to 0-1, halved for a D24FS8 target
        float depth = nclamp(oDepth.x, 0.0, 1.0);
        precise float halved = depth * 0.5;
        gl_FragDepth = (sys.flags.w & 1u) != 0u ? halved : depth;
    }
#endif
#endif
    bool writes_color_0 = (program & 4u) != 0u;
    // the alpha test: oC0's alpha against the reference
    {
        uint flags = sys.flags.x;
        float reference = sys.values.x, alpha = oC[0].w;
        uint function = flags & 7u;
        bool pass = true;
        pass = function == 1u ? alpha < reference : pass;
        pass = function == 2u ? alpha == reference : pass;
        pass = function == 3u ? alpha <= reference : pass;
        pass = function == 4u ? alpha > reference : pass;
        pass = function == 5u ? alpha != reference : pass;
        pass = function == 6u ? alpha >= reference : pass;
        pass = function == 0u ? false : pass;
        bool enabled = (flags & 8u) != 0u;
#ifdef FSI
        enabled = writes_color_0 && enabled && (oC_written & 1u) != 0u;
#endif
        if (enabled && !pass) demote;
    }
#ifdef FSI
    uint coverage = 0xFu;
    if (writes_color_0 && (sys.flags.x & (1u << 16u)) != 0u && (oC_written & 1u) != 0u) {
        uint px = uint(console_position(gl_FragCoord.x)), py = uint(console_position(gl_FragCoord.y));
        uint index = (py & 1u) | ((px & 1u) << 1u);
        float offset = float(bitfieldExtract(sys.flags.x, int(8u + (index << 1u)), 2));
        float alpha = oC[0].w;
        uint msaa = bitfieldExtract(sys.flags.w, 1, 2);
        uint mask = 0xFu;
        if (msaa == 2u) {
            const float base4[4] = float[](0.75, 0.25, 0.5, 1.0);
            for (uint s = 0u; s < 4u; s++) {
                precise float threshold = base4[s] + offset * (-1.0 / 16.0);
                if (!(alpha >= threshold)) mask &= ~(1u << s);
            }
        } else if (msaa == 1u) {
            const float base2[2] = float[](0.5, 1.0);
            for (uint s = 0u; s < 2u; s++) {
                precise float threshold = base2[s] + offset * (-1.0 / 8.0);
                if (!(alpha >= threshold)) mask &= ~(1u << s);
            }
        } else {
            precise float threshold = 1.0 + offset * (-1.0 / 4.0);
            if (!(alpha >= threshold)) mask &= ~1u;
        }
        coverage = mask;
    }
    vec2 dxy = vec2(dFdxCoarse(gl_FragCoord.z), dFdyCoarse(gl_FragCoord.z));
    uint mask_in = uint(gl_SampleMaskIn[0]);
    bool alive = !gl_HelperInvocation;
#ifdef WRITES_DEPTH
    const bool writes_depth = true;
#else
    const bool writes_depth = false;
#endif
    beginInvocationInterlockARB();
    xf_fsi(oC[0], oC[1], oC[2], oC[3], oC_written, oDepth.x, writes_depth, gl_FragCoord, dxy, mask_in, gl_FrontFacing,
           coverage, alive);
    endInvocationInterlockARB();
#else
    // alpha to coverage, as the host render target path makes it (none where
    // the shader does not write oC0)
    gl_SampleMask[0] = -1;
    if (writes_color_0 && (sys.flags.x & (1u << 16u)) != 0u) {
        uint px = uint(console_position(gl_FragCoord.x)), py = uint(console_position(gl_FragCoord.y));
        uint index = (py & 1u) | ((px & 1u) << 1u);
        float offset = float(bitfieldExtract(sys.flags.x, int(8u + (index << 1u)), 2));
        float alpha = oC[0].w;
        uint msaa = bitfieldExtract(sys.flags.w, 1, 2);
        uint mask = 0u;
        if (msaa == 2u) {
            // host samples 0-3 at 0.75, 0.25, 0.5, 1.0
            const float base4[4] = float[](0.75, 0.25, 0.5, 1.0);
            for (uint s = 0u; s < 4u; s++) {
                precise float threshold = base4[s] + offset * (-1.0 / 16.0);
                if (alpha >= threshold) mask |= 1u << s;
            }
        } else if (msaa == 1u) {
            // host sample 1 at 0.5, 0 at 1.0
            precise float t1 = 0.5 + offset * (-1.0 / 8.0), t0 = 1.0 + offset * (-1.0 / 8.0);
            if (alpha >= t1) mask |= 2u;
            if (alpha >= t0) mask |= 1u;
        } else {
            precise float threshold = 1.0 + offset * (-1.0 / 4.0);
            if (alpha >= threshold) mask |= 1u;
        }
        gl_SampleMask[0] = int(mask);
        if (mask == 0u) demote;
    }
    // the colors out: times the target's scale, the blend's factors, the gamma curve
    uint written = (program >> 4u) & 15u;
    for (uint k = 0u; k < 4u; k++) {
        if ((written & (1u << k)) == 0u) {
            colors[k] = vec4(0.0);
            continue;
        }
        precise vec4 color = oC[k] * sys.color_scales[k];
        color = premultiplied(color, k);
        if ((sys.flags.z & (1u << k)) != 0u)
            color = vec4(linear_to_pwl(color.r), linear_to_pwl(color.g), linear_to_pwl(color.b), color.a);
        colors[k] = color;
    }
#endif
}
