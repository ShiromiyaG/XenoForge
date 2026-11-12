// The interlock path's output merger (edram_fsi.frag has what it does and
// where it comes from): xf_fsi and what it needs, included by edram_fsi.frag
// and by the generic pixel shader (generic.frag). BSD; see LICENSE-xenia.

#define EDRAM_NO_TARGET
#include "edram.glsl"

layout(set = 0, binding = 4, std140) uniform Fsi {
    uint flags;      // the F_ bits below, the MSAA mode in bits 0-1
    uint depth_base; // dwords
    uint pitch;      // a row of 32bpp tiles, dwords
    uint zpd_index;  // the occlusion counter the draw counts in; ~0 none
    uvec4 rt;        // per color target: format (0-3), write mask (4-7), written (8)
    uvec4 blend;     // per color target: RB_BLENDCONTROL & 0x1FFF1FFF
    uvec4 rt_base;   // per color target: its base in dwords
    // front reference, read and write masks (bits 0-7, 8-15, 16-23); front
    // function and ops (RB_DEPTHCONTROL bits 8-19); the back's two
    uvec4 stencil;
    vec4 poly_offset; // front scale (in depth units per pixel), front offset, back scale, back offset
    vec4 blend_constant;
    vec4 exp_bias; // per color target: its exponent bias's scale
    // the resolution scale in quarters (render_scale.h; even on this path),
    // and a tile's dwords at it: tiles of 80x16 samples that many quarters
    // across and down
    uvec4 scale;
} fsi;
layout(set = 0, binding = 5, std430) coherent restrict buffer Edram { uint words[]; } edram;
layout(set = 0, binding = 6, std430) buffer Zpd { uint counts[]; } zpd;

layout(constant_id = 0) const uint SPECIALIZED = 0u;
layout(constant_id = 1) const uint SPEC_FLAGS = 0u;
layout(constant_id = 2) const uint SPEC_RT0 = 0u;
layout(constant_id = 3) const uint SPEC_RT1 = 0u;
layout(constant_id = 4) const uint SPEC_RT2 = 0u;
layout(constant_id = 5) const uint SPEC_RT3 = 0u;
layout(constant_id = 6) const uint SPEC_BLEND0 = 0u;
layout(constant_id = 7) const uint SPEC_BLEND1 = 0u;
layout(constant_id = 8) const uint SPEC_BLEND2 = 0u;
layout(constant_id = 9) const uint SPEC_BLEND3 = 0u;

const uint F_2X_AS_4X = 4u;  // guest 2x rasterized as host 4x (no 2x without attachments)
const uint F_DEPTH_STENCIL = 8u;
const uint F_DEPTH_LESS = 16u, F_DEPTH_EQUAL = 32u, F_DEPTH_GREATER = 64u;
const uint F_DEPTH_WRITE = 128u;
const uint F_STENCIL = 256u;
const uint F_FLOAT24 = 512u; // D24FS8

uint p_flags() { return SPECIALIZED != 0u ? SPEC_FLAGS : fsi.flags; }
uint p_rt(uint i) {
    if (SPECIALIZED != 0u) return i == 0u ? SPEC_RT0 : i == 1u ? SPEC_RT1 : i == 2u ? SPEC_RT2 : SPEC_RT3;
    return fsi.rt[i];
}
uint p_blend(uint i) {
    if (SPECIALIZED != 0u) return i == 0u ? SPEC_BLEND0 : i == 1u ? SPEC_BLEND1 : i == 2u ? SPEC_BLEND2 : SPEC_BLEND3;
    return fsi.blend[i];
}

// The host's coverage in the guest's sample order (FSI_LoadSampleMask): 2x
// from host samples 1 and 0 (or 2 and 1 when rasterized as 4x), 4x from 0,
// 3, 2, 1, the pairing of draw_util.h's GetHostSampleXenosPositions.
uint guest_coverage(uint host, uint flags) {
    uint msaa = flags & 3u;
    if (msaa == 0u) return host & 1u;
    if (msaa == 1u)
        return (flags & F_2X_AS_4X) != 0u ? ((host >> 2u) & 1u) | (host & 2u) : ((host >> 1u) & 1u) | ((host & 1u) << 1u);
    return (host & 5u) | ((host >> 2u) & 2u) | ((host & 2u) << 2u);
}

// Sample 0's dwords (FSI_LoadEdramOffsets): the depth's (with its base,
// wrapped), and a color target's at 32 and 64 bits per sample (before its
// base). 4x: the pixel's samples two columns and two rows apart; 2x: two
// columns.
struct Offsets {
    uint depth, c32, c64;
};
// (at the resolution scale: the grid of host pixels, the tiles and their
// halves scaled, as Xenia's ROV addressing with draw_resolution_scale)
uint tile_width() { return 20u * fsi.scale.x; }
uint edram_dwords() { return fsi.scale.y * 2048u; }
Offsets fsi_offsets(uvec2 pixel, uint msaa) {
    uint u = pixel.x, v = pixel.y;
    if (msaa >= 2u) {
        u = ((pixel.x >> 1u) << 2u) | (pixel.x & 1u);
        v = ((pixel.y >> 1u) << 2u) | (pixel.y & 1u);
    } else if (msaa == 1u) {
        u = pixel.x & ~2u;
        v = ((pixel.y >> 1u) << 2u) | (pixel.y & 1u) | (pixel.x & 2u);
    }
    // 40x16-sample halves of 32bpp tiles (whole 64bpp tiles)
    uint tw = tile_width(), half_width = tw >> 1u, th = 4u * fsi.scale.x;
    uvec2 half_index = uvec2(u / half_width, v / th), in_half = uvec2(u % half_width, v % th);
    uint row_in_tile = in_half.y * tw, tile_row = fsi.pitch * half_index.y;
    uint first_half = tile_row + fsi.scale.y * (half_index.x >> 1u) + row_in_tile + in_half.x;
    bool second = (half_index.x & 1u) != 0u;
    Offsets o;
    // depth tiles have their halves swapped
    o.depth = (fsi.depth_base + first_half + (second ? 0u : half_width)) % edram_dwords();
    o.c32 = first_half + (second ? half_width : 0u);
    o.c64 = (tile_row << 1u) + fsi.scale.y * half_index.x + row_in_tile + (in_half.x << 1u);
    return o;
}
uint sample_offset(uint i, bool wide) { return (i >> 1u) * 2u * tile_width() + (i & 1u) * (wide ? 4u : 2u); }

// The guest's sample positions in pixels (draw_util.h's kXenosSamplePositions)
vec2 sample_location(uint i, uint msaa) {
    if (msaa == 0u) return vec2(0.0);
    if (msaa == 1u) return i == 0u ? vec2(-4.0, 4.0) * (1.0 / 16.0) : vec2(4.0, -4.0) * (1.0 / 16.0);
    vec2 p = i == 0u ? vec2(-2.0, -2.0) : i == 1u ? vec2(2.0, 2.0) : i == 2u ? vec2(-6.0, 6.0) : vec2(6.0, -6.0);
    return p * (1.0 / 16.0);
}

// FSI_DepthStencilTest: the depth at each covered sample (the center's, with
// the polygon offset unless the shader wrote it, extrapolated to the guest's
// sample position by the depth's derivatives), to 24-bit unorm (rounded to
// the nearest even) or 20e4 (rounded to the nearest even), tested and
// written with the stencil as RB_DEPTHCONTROL says; the samples that fail
// leave the coverage.
uint fsi_depth_stencil(uint mask, uint flags, uint address, float center, vec2 dz, bool writes_depth, bool front) {
    if ((flags & F_DEPTH_STENCIL) == 0u) return mask;
    float z_center = center;
    if (!writes_depth) {
        vec2 offset = front ? fsi.poly_offset.xy : fsi.poly_offset.zw;
        precise float slope = max(abs(dz.x), abs(dz.y));
        precise float bias = offset.x * slope + offset.y;
        precise float biased = center + bias;
        z_center = biased;
    }
    uvec2 st = front ? fsi.stencil.xy : fsi.stencil.zw;
    uint reference = st.x & 0xFFu, read_mask = (st.x >> 8u) & 0xFFu, write_mask = (st.x >> 16u) & 0xFFu;
    uint reference_masked = reference & read_mask;
    uint msaa = flags & 3u;
    for (uint i = 0u; i < 4u; i++) {
        if ((mask & (1u << i)) == 0u) continue;
        uint at = address + sample_offset(i, false);
        uint old = edram.words[at];
        vec2 location = sample_location(i, msaa);
        precise float dx = location.x * dz.x;
        precise float dy = location.y * dz.y;
        precise float z_unclamped = z_center + (dx + dy);
        // saturated, NaN to 0 (NClamp)
        float z = z_unclamped > 0.0 ? min(z_unclamped, 1.0) : 0.0;
        precise float z_scaled = z * 16777215.0;
        uint z24 = (flags & F_FLOAT24) != 0u ? float32_to_20e4(z, true) : uint(roundEven(z_scaled));
        uint old_z = old >> 8u;
        bool z_pass = ((flags & F_DEPTH_LESS) != 0u && z24 < old_z) || ((flags & F_DEPTH_EQUAL) != 0u && z24 == old_z) ||
                      ((flags & F_DEPTH_GREATER) != 0u && z24 > old_z);
        bool s_pass = true;
        uint depth_stencil = old;
        if ((flags & F_STENCIL) != 0u) {
            uint old_masked = old & read_mask;
            s_pass = ((st.y & 1u) != 0u && reference_masked < old_masked) ||
                     ((st.y & 2u) != 0u && reference_masked == old_masked) ||
                     ((st.y & 4u) != 0u && reference_masked > old_masked);
            uint op = bitfieldExtract(st.y, s_pass ? (z_pass ? 6 : 9) : 3, 3);
            uint s = old;
            switch (op) {
            case 1u: s = 0u; break;
            case 2u: s = reference; break;
            case 3u: s = min(old & 0xFFu, 254u) + 1u; break;
            case 4u: s = max(old & 0xFFu, 1u) - 1u; break;
            case 5u: s = ~old; break;
            case 6u: s = old + 1u; break;
            case 7u: s = old - 1u; break;
            default: break;
            }
            depth_stencil = (old & ~write_mask) | (s & write_mask);
        }
        bool pass = z_pass && s_pass;
        if (!pass) mask &= ~(1u << i);
        uint result = pass && (flags & F_DEPTH_WRITE) != 0u ? bitfieldInsert(depth_stencil, z24, 8, 24) : depth_stencil;
        if (result != old) edram.words[at] = result;
    }
    return mask;
}

// Colors, as the guest's formats keep them.

bool is_64bpp(uint format) { return format == 5u || format == 7u || format == 15u; }
// blending sources, factors and results clamped (kPSIColorFormatFlag_FixedPoint*)
bool fixed_color(uint format) { return format <= 2u || format == 4u || format == 5u || format == 10u; }
bool fixed_alpha(uint format) { return fixed_color(format) || format == 3u || format == 12u; }
vec2 fixed_range(uint format) { return format == 4u || format == 5u ? vec2(-32.0, 32767.0 / 1024.0) : vec2(0.0, 1.0); }

// The bits the write mask keeps (GetPSIColorFormatInfo); both all ones when
// nothing is written
uvec2 keep_masks(uint format, uint write) {
    uvec2 keep = uvec2(0u);
    switch (format) {
    case 0u: case 1u:
        for (uint k = 0u; k < 4u; k++)
            if ((write & (1u << k)) == 0u) keep.x |= 0xFFu << (8u * k);
        break;
    case 2u: case 3u: case 10u: case 12u:
        for (uint k = 0u; k < 3u; k++)
            if ((write & (1u << k)) == 0u) keep.x |= 0x3FFu << (10u * k);
        if ((write & 8u) == 0u) keep.x |= 3u << 30u;
        break;
    case 4u: case 5u: case 6u: case 7u:
        if ((write & 1u) == 0u) keep.x |= 0xFFFFu;
        if ((write & 2u) == 0u) keep.x |= 0xFFFF0000u;
        if (format == 5u || format == 7u) {
            if ((write & 4u) == 0u) keep.y |= 0xFFFFu;
            if ((write & 8u) == 0u) keep.y |= 0xFFFF0000u;
        } else {
            write &= 3u;
        }
        break;
    case 14u:
        write &= 1u;
        if ((write & 1u) == 0u) keep.x = ~0u;
        break;
    case 15u:
        write &= 3u;
        if ((write & 1u) == 0u) keep.x = ~0u;
        if ((write & 2u) == 0u) keep.y = ~0u;
        break;
    default: write = 0u; break;
    }
    return write == 0u ? uvec2(~0u) : keep;
}

vec4 saturate_n(vec4 v) { return mix(clamp(v, 0.0, 1.0), vec4(0.0), isnan(v)); }
float saturate_n(float v) { return isnan(v) ? 0.0 : clamp(v, 0.0, 1.0); }

// Xenia's PackFloat16x2ExtendedRange: the Xenos float16 has no infinity or
// NaN, its exponent 31 is finite (to 131008); NaN as 0
uint pack_half2_extended(vec2 v) {
    v = mix(v, vec2(0.0), isnan(v));
    uint standard = packHalf2x16(v);
    bool low_over = (standard & 0x7C00u) == 0x7C00u, high_over = ((standard >> 16u) & 0x7C00u) == 0x7C00u;
    precise vec2 halved = clamp(v, -131008.0, 131008.0) * 0.5;
    uint extended = packHalf2x16(halved) + 0x04000400u;
    return (low_over ? extended & 0xFFFFu : standard & 0xFFFFu) |
           (high_over ? extended & 0xFFFF0000u : standard & 0xFFFF0000u);
}
vec2 unpack_half2_extended(uint p) {
    bool low_over = (p & 0x7C00u) == 0x7C00u, high_over = ((p >> 16u) & 0x7C00u) == 0x7C00u;
    vec2 standard = unpackHalf2x16(p);
    precise vec2 extended = unpackHalf2x16(p - ((low_over ? 0x0400u : 0u) | (high_over ? 0x04000000u : 0u))) * 2.0;
    return vec2(low_over ? extended.x : standard.x, high_over ? extended.y : standard.y);
}

// PWLGammaToLinear of a saturated gamma value
float pwl_gamma_to_linear(float gamma) {
    float scale, offset;
    if (gamma >= 96.0 / 255.0) {
        scale = gamma >= 192.0 / 255.0 ? 8.0 / 1024.0 : 4.0 / 1024.0;
        offset = gamma >= 192.0 / 255.0 ? -1024.0 : -256.0;
    } else {
        scale = gamma >= 64.0 / 255.0 ? 2.0 / 1024.0 : 1.0 / 1024.0;
        offset = gamma >= 64.0 / 255.0 ? -64.0 : 0.0;
    }
    precise float scaled = gamma * (255.0 * 1024.0);
    precise float linear = scaled * scale + offset;
    precise float stepped = linear * scale;
    precise float result = (linear + trunc(stepped)) * (1.0 / 1023.0);
    return result;
}

// FSI_ClampAndPackColor
uvec2 fsi_pack(vec4 c, uint format) {
    switch (format) {
    case 0u: case 1u: {
        vec4 v = saturate_n(c);
        if (format == 1u) v.rgb = vec3(linear_to_pwl_gamma(c.r), linear_to_pwl_gamma(c.g), linear_to_pwl_gamma(c.b));
        precise vec4 scaled = v * 255.0 + 0.5;
        uvec4 u = uvec4(scaled);
        return uvec2(u.x | (u.y << 8u) | (u.z << 16u) | (u.w << 24u), 0u);
    }
    case 2u: case 10u: {
        precise vec4 scaled = saturate_n(c) * vec4(1023.0, 1023.0, 1023.0, 3.0) + 0.5;
        uvec4 u = uvec4(scaled);
        return uvec2(u.x | (u.y << 10u) | (u.z << 20u) | (u.w << 30u), 0u);
    }
    case 3u: case 12u: {
        precise float alpha = saturate_n(c.a) * 3.0 + 0.5;
        return uvec2(float32_to_7e3(c.r) | (float32_to_7e3(c.g) << 10u) | (float32_to_7e3(c.b) << 20u) |
                         (uint(alpha) << 30u),
                     0u);
    }
    case 4u: case 5u: {
        // -32 to 32 with 10 fraction bits, rounded half up (-32 is 0x8000), NaN as 0
        vec4 v = clamp(mix(c, vec4(0.0), isnan(c)), -32.0, 32767.0 / 1024.0);
        precise vec4 scaled = v * 1024.0 + 0.5;
        uvec4 u = uvec4(ivec4(floor(scaled))) & 0xFFFFu;
        return uvec2(u.x | (u.y << 16u), u.z | (u.w << 16u));
    }
    case 6u: case 7u: return uvec2(pack_half2_extended(c.xy), pack_half2_extended(c.zw));
    }
    return uvec2(floatBitsToUint(c.x), floatBitsToUint(c.y));
}

// FSI_UnpackColor: the components a format lacks are 0, alpha 1
vec4 fsi_unpack(uvec2 b, uint format) {
    switch (format) {
    case 0u: case 1u: {
        precise vec4 v = vec4(b.x & 0xFFu, (b.x >> 8u) & 0xFFu, (b.x >> 16u) & 0xFFu, b.x >> 24u) * (1.0 / 255.0);
        if (format == 1u) v.rgb = vec3(pwl_gamma_to_linear(v.r), pwl_gamma_to_linear(v.g), pwl_gamma_to_linear(v.b));
        return v;
    }
    case 2u: case 10u: {
        precise vec4 v = vec4(b.x & 0x3FFu, (b.x >> 10u) & 0x3FFu, (b.x >> 20u) & 0x3FFu, b.x >> 30u) *
                         vec4(1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 1023.0, 1.0 / 3.0);
        return v;
    }
    case 3u: case 12u: {
        precise float alpha = float(b.x >> 30u) * (1.0 / 3.0);
        return vec4(float7e3_to_32(b.x), float7e3_to_32(b.x >> 10u), float7e3_to_32(b.x >> 20u), alpha);
    }
    case 4u: case 5u: {
        ivec4 s = ivec4(bitfieldExtract(int(b.x), 0, 16), bitfieldExtract(int(b.x), 16, 16),
                        bitfieldExtract(int(b.y), 0, 16), bitfieldExtract(int(b.y), 16, 16));
        precise vec4 v = vec4(s) * (1.0 / 1024.0);
        return format == 4u ? vec4(v.xy, 0.0, 1.0) : v;
    }
    case 6u: return vec4(unpack_half2_extended(b.x), 0.0, 1.0);
    case 7u: return vec4(unpack_half2_extended(b.x), unpack_half2_extended(b.y));
    case 14u: return vec4(uintBitsToFloat(b.x), 0.0, 0.0, 1.0);
    }
    return vec4(uintBitsToFloat(b.x), uintBitsToFloat(b.y), 0.0, 1.0);
}

// FSI_FlushNaNClampAndInBlending
vec3 blend_clamp(vec3 v, bool fixed_point, vec2 range) {
    return fixed_point ? clamp(mix(v, vec3(0.0), isnan(v)), range.x, range.y) : v;
}
float blend_clamp(float v, bool fixed_point, vec2 range) {
    return fixed_point ? clamp(isnan(v) ? 0.0 : v, range.x, range.y) : v;
}

// FSI_ApplyColorBlendFactor: zero not multiplied (no infinity or NaN times
// 0), one the value itself, the undefined factors as one
vec3 color_factor(uint factor, vec3 value, vec3 sc, float sa, vec3 dc, float da, vec3 kc, float ka, bool fixed_point,
                  vec2 range) {
    if (factor == 0u) return vec3(0.0);
    precise vec3 r = value;
    switch (factor) {
    case 4u: r = value * sc; break;
    case 5u: r = value * (vec3(1.0) - sc); break;
    case 6u: r = value * sa; break;
    case 7u: r = value * (1.0 - sa); break;
    case 8u: r = value * dc; break;
    case 9u: r = value * (vec3(1.0) - dc); break;
    case 10u: r = value * da; break;
    case 11u: r = value * (1.0 - da); break;
    case 12u: r = value * kc; break;
    case 13u: r = value * (vec3(1.0) - kc); break;
    case 14u: r = value * ka; break;
    case 15u: r = value * (1.0 - ka); break;
    case 16u: r = value * min(sa, 1.0 - da); break;
    default: break;
    }
    return blend_clamp(r, fixed_point, range);
}
// FSI_ApplyAlphaBlendFactor: the color factors as their alpha's
float alpha_factor(uint factor, float value, float sa, float da, float ka, bool fixed_point, vec2 range) {
    if (factor == 0u) return 0.0;
    precise float r = value;
    switch (factor) {
    case 4u: case 6u: r = value * sa; break;
    case 5u: case 7u: r = value * (1.0 - sa); break;
    case 8u: case 10u: r = value * da; break;
    case 9u: case 11u: r = value * (1.0 - da); break;
    case 12u: case 14u: r = value * ka; break;
    case 13u: case 15u: r = value * (1.0 - ka); break;
    case 16u: r = value * min(sa, 1.0 - da); break;
    default: break;
    }
    return blend_clamp(r, fixed_point, range);
}
// FSI_BlendColorOrAlphaWithUnclampedResult's equations, after the factors
// (unlike Vulkan's, the Xenos min and max take the factors too)
vec3 blend_equation(uint op, vec3 s, vec3 d) {
    precise vec3 r = s + d;
    if (op == 1u) r = s - d;
    else if (op == 4u) r = d - s;
    else if (op == 2u) r = min(s, d);
    else if (op == 3u) r = max(s, d);
    return r;
}
float blend_equation(uint op, float s, float d) {
    precise float r = s + d;
    if (op == 1u) r = s - d;
    else if (op == 4u) r = d - s;
    else if (op == 2u) r = min(s, d);
    else if (op == 3u) r = max(s, d);
    return r;
}

// One color target: its exponent bias, then blended with each covered
// sample's destination or written over it, the bits its write mask keeps
// kept.
void fsi_color(uint i, vec4 color, uint rt, uint blend, uint mask, Offsets o) {
    if ((rt & 0x100u) == 0u) return;
    uint format = rt & 0xFu;
    uvec2 keep = keep_masks(format, (rt >> 4u) & 0xFu);
    if (keep == uvec2(~0u)) return;
    bool wide = is_64bpp(format);
    precise vec4 c = color * fsi.exp_bias[i];
    uint base = (fsi.rt_base[i] + (wide ? o.c64 : o.c32)) % edram_dwords();
    if (blend != 0x00010001u) {
        bool fc = fixed_color(format), fa = fixed_alpha(format);
        vec2 range = fixed_range(format);
        vec3 kc = blend_clamp(fsi.blend_constant.rgb, fc, range);
        float ka = blend_clamp(fsi.blend_constant.a, fa, range);
        vec3 sc = blend_clamp(c.rgb, fc, range);
        float sa = blend_clamp(c.a, fa, range);
        uint color_source = blend & 0x1Fu, color_op = (blend >> 5u) & 7u, color_dest = (blend >> 8u) & 0x1Fu;
        uint alpha_source = (blend >> 16u) & 0x1Fu, alpha_op = (blend >> 21u) & 7u, alpha_dest = (blend >> 24u) & 0x1Fu;
        for (uint s = 0u; s < 4u; s++) {
            if ((mask & (1u << s)) == 0u) continue;
            uint at = base + sample_offset(s, wide);
            uvec2 dest = uvec2(edram.words[at], wide ? edram.words[at + 1u] : 0u);
            vec4 d = fsi_unpack(dest, format);
            vec3 rc = blend_clamp(blend_equation(color_op, color_factor(color_source, sc, sc, sa, d.rgb, d.a, kc, ka, fc, range),
                                                 color_factor(color_dest, d.rgb, sc, sa, d.rgb, d.a, kc, ka, fc, range)),
                                  fc, range);
            float ra = blend_clamp(blend_equation(alpha_op, alpha_factor(alpha_source, sa, sa, d.a, ka, fa, range),
                                                  alpha_factor(alpha_dest, d.a, sa, d.a, ka, fa, range)),
                                   fa, range);
            uvec2 packed = fsi_pack(vec4(rc, ra), format);
            edram.words[at] = (dest.x & keep.x) | (packed.x & ~keep.x);
            if (wide) edram.words[at + 1u] = (dest.y & keep.y) | (packed.y & ~keep.y);
        }
    } else {
        uvec2 packed = fsi_pack(c, format);
        bool load = keep != uvec2(0u);
        for (uint s = 0u; s < 4u; s++) {
            if ((mask & (1u << s)) == 0u) continue;
            uint at = base + sample_offset(s, wide);
            edram.words[at] = load ? (edram.words[at] & keep.x) | (packed.x & ~keep.x) : packed.x;
            if (wide) edram.words[at + 1u] = load ? (edram.words[at + 1u] & keep.y) | (packed.y & ~keep.y) : packed.y;
        }
    }
}

// The output merger, inside the interlock. c0-c3: oC0-oC3 as the shader
// wrote them (before the exponent bias); written: those it wrote on its
// path; depth_dxy: the rasterized depth's coarse derivatives, taken before
// the interlock; coverage: what alpha to coverage leaves (guest order);
// alive: not a helper invocation (killed or alpha tested away).
void xf_fsi(vec4 c0, vec4 c1, vec4 c2, vec4 c3, uint written, float shader_depth, bool writes_depth, vec4 frag_coord,
            vec2 depth_dxy, uint sample_mask_in, bool front_facing, uint coverage, bool alive) {
    uint flags = p_flags();
    uint mask = alive ? guest_coverage(sample_mask_in, flags) & coverage : 0u;
    if (mask == 0u) return;
    Offsets o = fsi_offsets(uvec2(frag_coord.xy), flags & 3u);
    float center = writes_depth ? shader_depth : frag_coord.z;
    vec2 dz = writes_depth ? vec2(0.0) : depth_dxy;
    mask = fsi_depth_stencil(mask, flags, o.depth, center, dz, writes_depth, front_facing);
    // the samples that passed, for the occlusion query (FSI_AddMSAASamplesToZPD)
    uint counter = fsi.zpd_index;
    if (counter != ~0u && mask != 0u) atomicAdd(zpd.counts[counter], uint(bitCount(mask)));
    if (mask == 0u || written == 0u) return;
    if ((written & 1u) != 0u) fsi_color(0u, c0, p_rt(0u), p_blend(0u), mask, o);
    if ((written & 2u) != 0u) fsi_color(1u, c1, p_rt(1u), p_blend(1u), mask, o);
    if ((written & 4u) != 0u) fsi_color(2u, c2, p_rt(2u), p_blend(2u), mask, o);
    if ((written & 8u) != 0u) fsi_color(3u, c3, p_rt(3u), p_blend(3u), mask, o);
}
