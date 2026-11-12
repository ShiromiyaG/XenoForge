/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The reference renderer's arithmetic on the guest's bits: the 7e3, 20e4,
// float16 and piecewise linear gamma conversions (xenos.cc's Float7e3To32,
// Float32To7e3, Float20e4To32, Float32To20e4, PWLGammaToLinear,
// LinearToPWLGamma; Xenia's PackFloat16x2ExtendedRange), the color target
// formats' packing and unpacking and the blender (spirv_shader_translator_rb.cc's
// FSI_ClampAndPackColor, FSI_UnpackColor, FSI_ApplyColorBlendFactor,
// FSI_ApplyAlphaBlendFactor, FSI_BlendColorOrAlphaWithUnclampedResult, and
// render_target_cache.cc's GetPSIColorFormatInfo keep masks), translated
// through src/shaders/edram.glsl and edram_fsi.frag, which run the same on
// the GPU. Every product and sum rounded on its own (no contraction), as
// those shaders' `precise`. The sample positions and the fill rule are the
// R6xx/R7xx 3D register reference's (PA_SC_AA_SAMPLE_LOCS_*; the rasterizer's
// top-left rule) with Xenia's kXenosSamplePositions.

#include "reference_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace xf::reference::pieces {

float float7e3_to_32(uint32_t f10) {
    f10 &= 0x3FF;
    if (!f10) return 0.0f;
    uint32_t mantissa = f10 & 0x7F;
    int32_t exponent = int32_t(f10 >> 7);
    if (!exponent) {
        uint32_t lzcnt = 7 - uint32_t(std::bit_width(mantissa) - 1);
        exponent = 1 - int32_t(lzcnt);
        mantissa = (mantissa << lzcnt) & 0x7F;
    }
    return std::bit_cast<float>(uint32_t(exponent + 124) << 23 | mantissa << 16);
}

uint32_t float32_to_7e3(float f) {
    // clamped to 0-31.875 first, NaN to 0
    uint32_t u = std::bit_cast<uint32_t>(f > 0.0f ? std::min(f, 31.875f) : 0.0f);
    uint32_t biased =
        u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> std::min(125u - (u >> 23), 24u) : u + 0xC2000000u;
    return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

float float20e4_to_32(uint32_t f24) {
    f24 &= 0xFFFFFF;
    if (!f24) return 0.0f;
    uint32_t mantissa = f24 & 0xFFFFF;
    int32_t exponent = int32_t(f24 >> 20);
    if (!exponent) {
        uint32_t lzcnt = 20 - uint32_t(std::bit_width(mantissa) - 1);
        exponent = 1 - int32_t(lzcnt);
        mantissa = (mantissa << lzcnt) & 0xFFFFF;
    }
    return std::bit_cast<float>(uint32_t(exponent + 112) << 23 | mantissa << 3);
}

uint32_t float32_to_20e4(float f, bool round_to_nearest_even) {
    uint32_t u = std::bit_cast<uint32_t>(f);
    u = std::min(u <= 0x7FFFFFFFu ? u : 0u, 0x3FFFFFF8u);
    uint32_t denormalized = ((u & 0x7FFFFFu) | 0x800000u) >> std::min(113u - std::min(u >> 23, 113u), 24u);
    uint32_t f24 = u < 0x38800000u ? denormalized : u + 0xC8000000u;
    if (round_to_nearest_even) f24 += 3u + ((f24 >> 3) & 1u);
    return (f24 >> 3) & 0xFFFFFFu;
}

float linear_to_pwl_gamma(float linear) {
    float l = std::isnan(linear) ? 0.0f : std::clamp(linear, 0.0f, 1.0f);
    float scale, offset;
    if (l >= 128.0f / 1023.0f) {
        scale = l >= 512.0f / 1023.0f ? 1023.0f / 8.0f : 1023.0f / 4.0f;
        offset = l >= 512.0f / 1023.0f ? 128.0f / 255.0f : 64.0f / 255.0f;
    } else {
        scale = l >= 64.0f / 1023.0f ? 1023.0f / 2.0f : 1023.0f;
        offset = l >= 64.0f / 1023.0f ? 32.0f / 255.0f : 0.0f;
    }
    float scaled = l * scale;
    float stepped = std::trunc(scaled) * (1.0f / 255.0f);
    return stepped + offset;
}

float pwl_gamma_to_linear(float gamma) {
    gamma = std::isnan(gamma) ? 0.0f : std::clamp(gamma, 0.0f, 1.0f);
    float scale, offset;
    if (gamma >= 96.0f / 255.0f) {
        scale = gamma >= 192.0f / 255.0f ? 8.0f / 1024.0f : 4.0f / 1024.0f;
        offset = gamma >= 192.0f / 255.0f ? -1024.0f : -256.0f;
    } else {
        scale = gamma >= 64.0f / 255.0f ? 2.0f / 1024.0f : 1.0f / 1024.0f;
        offset = gamma >= 64.0f / 255.0f ? -64.0f : 0.0f;
    }
    float scaled = gamma * (255.0f * 1024.0f);
    float product = scaled * scale;
    float linear = product + offset;
    float stepped = linear * scale;
    float sum = linear + std::trunc(stepped);
    return sum * (1.0f / 1023.0f);
}

uint16_t float_to_half(float f) {
    uint32_t u = std::bit_cast<uint32_t>(f);
    uint32_t sign = (u >> 16) & 0x8000u;
    uint32_t magnitude = u & 0x7FFFFFFFu;
    if (magnitude > 0x7F800000u) return uint16_t(sign | 0x7E00u);  // NaN
    if (magnitude >= 0x477FF000u) return uint16_t(sign | 0x7C00u); // past 65504 once rounded: infinity
    if (magnitude < 0x38800000u) {
        // a denormal (or zero), rounded to nearest even
        if (magnitude < 0x33000000u) return uint16_t(sign); // below half the smallest denormal
        uint32_t exponent = magnitude >> 23;
        uint32_t mantissa = (magnitude & 0x7FFFFFu) | 0x800000u;
        uint32_t shift = 126 - exponent; // to a denormal's 10 bits
        uint32_t value = mantissa >> shift;
        uint32_t rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (value & 1))) value++;
        return uint16_t(sign | value);
    }
    uint32_t value = ((magnitude - 0x38000000u) >> 13);
    uint32_t rest = magnitude & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (value & 1))) value++;
    return uint16_t(sign | value);
}

float half_to_float(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0) {
        float v = std::ldexp(float(mantissa), -24);
        return sign ? -v : v;
    }
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

uint32_t pack_half2_extended(float x, float y) {
    float v[2] = {std::isnan(x) ? 0.0f : x, std::isnan(y) ? 0.0f : y};
    uint32_t out = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t standard = float_to_half(v[i]);
        if ((standard & 0x7C00u) == 0x7C00u) {
            float halved = std::clamp(v[i], -131008.0f, 131008.0f) * 0.5f;
            standard = uint32_t(float_to_half(halved)) + 0x0400u;
        }
        out |= (standard & 0xFFFFu) << (16 * i);
    }
    return out;
}

void unpack_half2_extended(uint32_t packed, float &x, float &y) {
    float out[2];
    for (int i = 0; i < 2; i++) {
        uint32_t h = (packed >> (16 * i)) & 0xFFFF;
        if ((h & 0x7C00u) == 0x7C00u)
            out[i] = half_to_float(uint16_t(h - 0x0400u)) * 2.0f;
        else
            out[i] = half_to_float(uint16_t(h));
    }
    x = out[0], y = out[1];
}

void sample_position(uint32_t msaa, uint32_t sample, int32_t &x16, int32_t &y16) {
    static const int8_t two[2][2] = {{-4, 4}, {4, -4}};
    static const int8_t four[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};
    if (msaa == 0) {
        x16 = y16 = 0;
    } else if (msaa == 1) {
        x16 = two[sample & 1][0], y16 = two[sample & 1][1];
    } else {
        x16 = four[sample & 3][0], y16 = four[sample & 3][1];
    }
}

bool top_left(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    // y down, wound clockwise on screen: a top edge is horizontal with the
    // triangle below it (going right), a left edge goes up
    int64_t dy = y1 - y0, dx = x1 - x0;
    return (dy == 0 && dx > 0) || dy < 0;
}

uint32_t coverage(const int64_t x[3], const int64_t y[3], int32_t px, int32_t py, uint32_t msaa) {
    // clockwise on screen (y down): the area's sign
    int64_t area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (!area) return 0;
    int order[3] = {0, 1, 2};
    if (area < 0) std::swap(order[1], order[2]);
    uint32_t mask = 0;
    uint32_t samples = msaa == 0 ? 1 : msaa == 1 ? 2 : 4;
    for (uint32_t s = 0; s < samples; s++) {
        int32_t sx16, sy16;
        sample_position(msaa, s, sx16, sy16);
        // the sample in 1/256 pixel: the pixel's center plus its offset
        int64_t sx = int64_t(px) * 256 + 128 + sx16 * 16, sy = int64_t(py) * 256 + 128 + sy16 * 16;
        bool inside = true;
        for (int e = 0; e < 3 && inside; e++) {
            int a = order[e], b = order[(e + 1) % 3];
            int64_t w = (x[b] - x[a]) * (sy - y[a]) - (y[b] - y[a]) * (sx - x[a]);
            inside = w > 0 || (w == 0 && top_left(x[a], y[a], x[b], y[b]));
        }
        if (inside) mask |= 1u << s;
    }
    return mask;
}

namespace {

float saturate_n(float v) { return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f); }

bool fixed_color(uint32_t f) { return f <= 2 || f == 4 || f == 5 || f == 10; }
bool fixed_alpha(uint32_t f) { return fixed_color(f) || f == 3 || f == 12; }
void fixed_range(uint32_t f, float &lo, float &hi) {
    if (f == 4 || f == 5)
        lo = -32.0f, hi = 32767.0f / 1024.0f;
    else
        lo = 0.0f, hi = 1.0f;
}
float blend_clamp(float v, bool fixed, float lo, float hi) {
    return fixed ? std::clamp(std::isnan(v) ? 0.0f : v, lo, hi) : v;
}

float color_factor(uint32_t factor, float value, float sc, float sa, float dc, float da, float kc, float ka, bool fixed,
                   float lo, float hi) {
    if (factor == 0) return 0.0f;
    float r = value;
    switch (factor) {
    case 4: r = value * sc; break;
    case 5: r = value * (1.0f - sc); break;
    case 6: r = value * sa; break;
    case 7: r = value * (1.0f - sa); break;
    case 8: r = value * dc; break;
    case 9: r = value * (1.0f - dc); break;
    case 10: r = value * da; break;
    case 11: r = value * (1.0f - da); break;
    case 12: r = value * kc; break;
    case 13: r = value * (1.0f - kc); break;
    case 14: r = value * ka; break;
    case 15: r = value * (1.0f - ka); break;
    case 16: r = value * std::min(sa, 1.0f - da); break;
    default: break;
    }
    return blend_clamp(r, fixed, lo, hi);
}

float alpha_factor(uint32_t factor, float value, float sa, float da, float ka, bool fixed, float lo, float hi) {
    if (factor == 0) return 0.0f;
    float r = value;
    switch (factor) {
    case 4:
    case 6: r = value * sa; break;
    case 5:
    case 7: r = value * (1.0f - sa); break;
    case 8:
    case 10: r = value * da; break;
    case 9:
    case 11: r = value * (1.0f - da); break;
    case 12:
    case 14: r = value * ka; break;
    case 13:
    case 15: r = value * (1.0f - ka); break;
    case 16: r = value * std::min(sa, 1.0f - da); break;
    default: break;
    }
    return blend_clamp(r, fixed, lo, hi);
}

// GLSL's min and max: NaN operands give either (here the other's, as most GPUs)
float gl_min(float a, float b) { return b < a ? b : a; }
float gl_max(float a, float b) { return a < b ? b : a; }

float blend_equation(uint32_t op, float s, float d) {
    float r = s + d;
    if (op == 1)
        r = s - d;
    else if (op == 4)
        r = d - s;
    else if (op == 2)
        r = gl_min(s, d);
    else if (op == 3)
        r = gl_max(s, d);
    return r;
}

} // namespace

void pack_color(const float c[4], uint32_t format, uint32_t out[2]) {
    out[0] = out[1] = 0;
    switch (format) {
    case 0:
    case 1: {
        float v[4];
        for (int i = 0; i < 4; i++) v[i] = saturate_n(c[i]);
        if (format == 1)
            for (int i = 0; i < 3; i++) v[i] = linear_to_pwl_gamma(c[i]);
        for (int i = 0; i < 4; i++) out[0] |= uint32_t(v[i] * 255.0f + 0.5f) << (8 * i);
        return;
    }
    case 2:
    case 10: {
        const float scales[4] = {1023.0f, 1023.0f, 1023.0f, 3.0f};
        const int shifts[4] = {0, 10, 20, 30};
        for (int i = 0; i < 4; i++) out[0] |= uint32_t(saturate_n(c[i]) * scales[i] + 0.5f) << shifts[i];
        return;
    }
    case 3:
    case 12: {
        float alpha = saturate_n(c[3]) * 3.0f + 0.5f;
        out[0] = float32_to_7e3(c[0]) | float32_to_7e3(c[1]) << 10 | float32_to_7e3(c[2]) << 20 | uint32_t(alpha) << 30;
        return;
    }
    case 4:
    case 5: {
        // -32 to 32 with 10 fraction bits, rounded half up (-32 is 0x8000), NaN as 0
        uint32_t u[4];
        for (int i = 0; i < 4; i++) {
            float v = std::clamp(std::isnan(c[i]) ? 0.0f : c[i], -32.0f, 32767.0f / 1024.0f);
            float scaled = v * 1024.0f + 0.5f;
            u[i] = uint32_t(int32_t(std::floor(scaled))) & 0xFFFF;
        }
        out[0] = u[0] | u[1] << 16;
        out[1] = u[2] | u[3] << 16;
        return;
    }
    case 6:
    case 7:
        out[0] = pack_half2_extended(c[0], c[1]);
        out[1] = pack_half2_extended(c[2], c[3]);
        return;
    default:
        out[0] = std::bit_cast<uint32_t>(c[0]);
        out[1] = std::bit_cast<uint32_t>(c[1]);
        return;
    }
}

void unpack_color(const uint32_t b[2], uint32_t format, float v[4]) {
    switch (format) {
    case 0:
    case 1:
        for (int i = 0; i < 4; i++) v[i] = float((b[0] >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
        if (format == 1)
            for (int i = 0; i < 3; i++) v[i] = pwl_gamma_to_linear(v[i]);
        return;
    case 2:
    case 10:
        for (int i = 0; i < 3; i++) v[i] = float((b[0] >> (10 * i)) & 0x3FF) * (1.0f / 1023.0f);
        v[3] = float(b[0] >> 30) * (1.0f / 3.0f);
        return;
    case 3:
    case 12:
        v[0] = float7e3_to_32(b[0]), v[1] = float7e3_to_32(b[0] >> 10), v[2] = float7e3_to_32(b[0] >> 20);
        v[3] = float(b[0] >> 30) * (1.0f / 3.0f);
        return;
    case 4:
    case 5: {
        int32_t s[4] = {int32_t(b[0] << 16) >> 16, int32_t(b[0]) >> 16, int32_t(b[1] << 16) >> 16, int32_t(b[1]) >> 16};
        for (int i = 0; i < 4; i++) v[i] = float(s[i]) * (1.0f / 1024.0f);
        if (format == 4) v[2] = 0.0f, v[3] = 1.0f;
        return;
    }
    case 6:
        unpack_half2_extended(b[0], v[0], v[1]);
        v[2] = 0.0f, v[3] = 1.0f;
        return;
    case 7:
        unpack_half2_extended(b[0], v[0], v[1]);
        unpack_half2_extended(b[1], v[2], v[3]);
        return;
    case 14: v[0] = std::bit_cast<float>(b[0]), v[1] = v[2] = 0.0f, v[3] = 1.0f; return;
    default: v[0] = std::bit_cast<float>(b[0]), v[1] = std::bit_cast<float>(b[1]), v[2] = 0.0f, v[3] = 1.0f; return;
    }
}

void keep_masks(uint32_t format, uint32_t write, uint32_t keep[2]) {
    keep[0] = keep[1] = 0;
    switch (format) {
    case 0:
    case 1:
        for (uint32_t k = 0; k < 4; k++)
            if (!(write & (1u << k))) keep[0] |= 0xFFu << (8 * k);
        break;
    case 2:
    case 3:
    case 10:
    case 12:
        for (uint32_t k = 0; k < 3; k++)
            if (!(write & (1u << k))) keep[0] |= 0x3FFu << (10 * k);
        if (!(write & 8)) keep[0] |= 3u << 30;
        break;
    case 4:
    case 5:
    case 6:
    case 7:
        if (!(write & 1)) keep[0] |= 0xFFFFu;
        if (!(write & 2)) keep[0] |= 0xFFFF0000u;
        if (format == 5 || format == 7) {
            if (!(write & 4)) keep[1] |= 0xFFFFu;
            if (!(write & 8)) keep[1] |= 0xFFFF0000u;
        } else {
            write &= 3;
        }
        break;
    case 14:
        write &= 1;
        if (!(write & 1)) keep[0] = ~0u;
        break;
    case 15:
        write &= 3;
        if (!(write & 1)) keep[0] = ~0u;
        if (!(write & 2)) keep[1] = ~0u;
        break;
    default: write = 0; break;
    }
    if (!write) keep[0] = keep[1] = ~0u;
}

void blend(const float source[4], const uint32_t destination[2], uint32_t format, uint32_t control,
           const float constant[4], uint32_t write_mask, uint32_t out[2]) {
    uint32_t keep[2];
    keep_masks(format, write_mask, keep);
    uint32_t packed[2];
    if ((control & 0x1FFF1FFF) != 0x00010001u) {
        bool fc = fixed_color(format), fa = fixed_alpha(format);
        float lo, hi;
        fixed_range(format, lo, hi);
        float kc[3], sc[3];
        for (int i = 0; i < 3; i++)
            kc[i] = blend_clamp(constant[i], fc, lo, hi), sc[i] = blend_clamp(source[i], fc, lo, hi);
        float ka = blend_clamp(constant[3], fa, lo, hi), sa = blend_clamp(source[3], fa, lo, hi);
        uint32_t color_source = control & 0x1F, color_op = (control >> 5) & 7, color_dest = (control >> 8) & 0x1F;
        uint32_t alpha_source = (control >> 16) & 0x1F, alpha_op = (control >> 21) & 7,
                 alpha_dest = (control >> 24) & 0x1F;
        float d[4];
        unpack_color(destination, format, d);
        float result[4];
        for (int i = 0; i < 3; i++)
            result[i] = blend_clamp(
                blend_equation(color_op,
                               color_factor(color_source, sc[i], sc[i], sa, d[i], d[3], kc[i], ka, fc, lo, hi),
                               color_factor(color_dest, d[i], sc[i], sa, d[i], d[3], kc[i], ka, fc, lo, hi)),
                fc, lo, hi);
        result[3] = blend_clamp(blend_equation(alpha_op, alpha_factor(alpha_source, sa, sa, d[3], ka, fa, lo, hi),
                                               alpha_factor(alpha_dest, d[3], sa, d[3], ka, fa, lo, hi)),
                                fa, lo, hi);
        pack_color(result, format, packed);
    } else {
        pack_color(source, format, packed);
    }
    out[0] = (destination[0] & keep[0]) | (packed[0] & ~keep[0]);
    out[1] = (destination[1] & keep[1]) | (packed[1] & ~keep[1]);
}

} // namespace xf::reference::pieces
