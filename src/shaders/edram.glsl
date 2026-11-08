// EDRAM as Xenia lays it out, for moving render targets' contents through
// it (renderer.cpp): where a sample lives (Xenia's
// src/xenia/gpu/shaders/edram.xesli, XeEdramOffsetBytes, at resolution scale
// 1), and the guest bits of each render target format, from and to the
// values the host keeps (pixel_formats.xesli; the 7e3 packing of
// dxbc_shader_translator_om.cc); the host's depth kept across transfers as
// Xenia's host render targets keep it (render_target_cache.h's
// OwnershipRange, the host depth transfers of d3d12_render_target_cache.cc).
// BSD; see LICENSE-xenia.
//
// With EDRAM_NO_TARGET defined (edram_fsi.frag, the pixel shader writing
// EDRAM itself), only the conversions that need no push constants. With
// EDRAM_TWO_TARGETS (edram_transfer.frag, a target's samples drawn straight
// into another), `target` is a variable the shader sets to the side it works
// on, from push constants of its own.

#if defined(EDRAM_TWO_TARGETS)
struct EdramParams {
    uint base_tiles, pitch_tiles, msaa, format, width, height, is_depth, ints_log2, stencil_bit, flags;
};
EdramParams target;
#elif !defined(EDRAM_NO_TARGET)
layout(push_constant) uniform Target {
    uint base_tiles;
    uint pitch_tiles; // at the target's bits per sample
    uint msaa;        // 0 1x, 1 2x, 2 4x
    uint format;      // the color target format, or the depth format
    uint width, height;
    uint is_depth;
    uint ints_log2;   // 1 for 64-bit samples
    uint stencil_bit; // loads without stencil export: the bit this pass writes, 8 or more for the depth pass
    // depth targets: bit 0, the host's depth is kept beside EDRAM (its format
    // is encoded differently on the host: D24FS8, and D24S8 on a device
    // without 24-bit unorm depth); bit 1, 20e4 rounded to the nearest even
    // (Xenia's depth_float24_round). Color targets: bit 2, an 8_8_8_8_GAMMA
    // target the host keeps linear (as RGBA16 unorm, Xenia's
    // gamma_render_target_as_unorm16), its gamma made at stores and undone
    // at loads. Bit 3, a float target (16_16_FLOAT, 16_16_16_16_FLOAT,
    // 32_FLOAT, 32_32_FLOAT) moved by its bits, through a UINT view of its
    // image (pack_raw), so NaN payloads and denormals survive.
    uint flags;
    // edram_clear.comp: the rectangle's corner and the clear's words
    uint x, y, clear_lo, clear_hi;
} target;
#endif

const uint TILE_DWORDS = 80u * 16u;
const uint EDRAM_DWORDS = TILE_DWORDS * 2048u;

#ifndef EDRAM_NO_TARGET
// The dword of a pixel's guest sample. 4x: bit 0 of the sample horizontal,
// bit 1 vertical; 2x: sample 0 the top one.
uint edram_offset(uvec2 pixel, uint sample_index) {
    uvec2 s;
    if (target.msaa >= 2u) {
        uvec2 offset = (uvec2(sample_index) >> uvec2(0u, 1u)) & 1u;
        s = ((pixel & ~1u) << 1u) | (pixel & 1u) | (offset << 1u);
    } else if (target.msaa == 1u) {
        s.x = (pixel.x & ~2u) | ((sample_index & 1u) << 1u);
        s.y = ((pixel.y & ~1u) << 1u) | (pixel.y & 1u) | (pixel.x & 2u);
    } else {
        s = pixel;
    }
    uvec2 tile_size = uvec2(80u >> target.ints_log2, 16u);
    uvec2 tile_xy = s / tile_size;
    uint tile = target.base_tiles + tile_xy.y * target.pitch_tiles + tile_xy.x;
    s -= tile_xy * tile_size;
    if (target.is_depth != 0u) {
        // depth tiles have their halves swapped
        uint half_width = tile_size.x >> 1u;
        s.x = s.x >= half_width ? s.x - half_width : s.x + half_width;
    }
    return (tile * TILE_DWORDS + ((s.y * tile_size.x + s.x) << target.ints_log2)) % EDRAM_DWORDS;
}

// The host's sample index to the guest's: Vulkan's standard 2x locations put
// the top sample at index 1.
uint guest_sample(uint host_sample) { return target.msaa == 1u ? host_sample ^ 1u : host_sample; }
#endif

float float7e3_to_32(uint f10) {
    f10 &= 0x3FFu;
    if (f10 == 0u) return 0.0;
    uint mantissa = f10 & 0x7Fu;
    int exponent = int(f10 >> 7u);
    if (exponent == 0) {
        uint lzcnt = 7u - uint(findMSB(mantissa));
        exponent = 1 - int(lzcnt);
        mantissa = (mantissa << lzcnt) & 0x7Fu;
    }
    return uintBitsToFloat((uint(exponent + 124) << 23u) | (mantissa << 16u));
}

uint float32_to_7e3(float f) {
    // clamped to 0-31.875 first, NaN to 0
    uint u = floatBitsToUint(f > 0.0 ? min(f, 31.875) : 0.0);
    uint biased = u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u) : u + 0xC2000000u;
    return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

// XeFloat32To20e4: rounded towards zero, as Xenia does by default, or to the
// nearest even with its depth_float24_round
uint float32_to_20e4(float f, bool round_to_nearest_even) {
    uint u = floatBitsToUint(f);
    u = min(u <= 0x7FFFFFFFu ? u : 0u, 0x3FFFFFF8u);
    uint denormalized = ((u & 0x7FFFFFu) | 0x800000u) >> min(113u - (u >> 23u), 24u);
    uint f24 = u < 0x38800000u ? denormalized : u + 0xC8000000u;
    if (round_to_nearest_even) f24 += 3u + ((f24 >> 3u) & 1u);
    return (f24 >> 3u) & 0xFFFFFFu;
}

float float20e4_to_32(uint f24) {
    f24 &= 0xFFFFFFu;
    if (f24 == 0u) return 0.0;
    uint mantissa = f24 & 0xFFFFFu;
    int exponent = int(f24 >> 20u);
    if (exponent == 0) {
        uint lzcnt = 20u - uint(findMSB(mantissa));
        exponent = 1 - int(lzcnt);
        mantissa = (mantissa << lzcnt) & 0xFFFFFu;
    }
    return uintBitsToFloat((uint(exponent + 112) << 23u) | (mantissa << 3u));
}

#ifndef EDRAM_NO_TARGET
// Depth as the host keeps it to the guest's 24 bits. D24FS8's 0-2 is kept as
// 0-1 (halved, as Xenia's host render targets keep it), so all of it survives
// the host's 0-1 depth: color bits moved through a depth target come back
// whole.
uint depth_to_guest(float depth) {
    return target.format == 0u ? uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0))
                               : float32_to_20e4(depth * 2.0, (target.flags & 2u) != 0u);
}
// and with the stencil to the guest's D24S8 or D24FS8 word
uint pack_depth(float depth, uint stencil) { return (depth_to_guest(depth) << 8u) | (stencil & 0xFFu); }

// The host's depth kept beside EDRAM (flags bit 0), in EDRAM's layout:
// D24FS8's in the first half, D24S8's in the second (Xenia's
// OwnershipRange::host_depth_render_target_float24 and _unorm24, tracked
// apart so that clearing through the other format does not drop it).
uint host_depth_offset(uint at) { return at + (target.format == 0u ? EDRAM_DWORDS : 0u); }
float unpack_depth(uint word) {
    uint d24 = word >> 8u;
    // xenos::UNorm24To32: exact, 0xFFFFFF to 1
    return target.format == 0u ? float(d24 + (d24 >> 23u)) * (1.0 / 16777216.0) : float20e4_to_32(d24) * 0.5;
}
#endif

// xenos.cc's LinearToPWLGamma (saturated, NaN to 0): the 8-bit gamma code
// over 255, for an 8_8_8_8_GAMMA target kept linear
float linear_to_pwl_gamma(float linear) {
    float l = clamp(linear, 0.0, 1.0);
    if (isnan(linear)) l = 0.0;
    float scale, offset;
    if (l >= 128.0 / 1023.0) {
        scale = l >= 512.0 / 1023.0 ? 1023.0 / 8.0 : 1023.0 / 4.0;
        offset = l >= 512.0 / 1023.0 ? 128.0 / 255.0 : 64.0 / 255.0;
    } else {
        scale = l >= 64.0 / 1023.0 ? 1023.0 / 2.0 : 1023.0;
        offset = l >= 64.0 / 1023.0 ? 32.0 / 255.0 : 0.0;
    }
    precise float scaled = l * scale;
    precise float gamma = trunc(scaled) * (1.0 / 255.0) + offset;
    return gamma;
}
// An 8-bit gamma code to the middle of the linear range that encodes to it
// (Xenia's GammaByteToLinearMidpoint, vulkan_render_target_cache.cc, and the
// D3D12 render target cache's transfers): a value on a range's edge could
// fall below it in the 16-bit unorm and come back one code lower.
float pwl_gamma_byte_to_linear_midpoint(uint b) {
    float recip = 1.0 / 1023.0, offset = 0.5 / 1023.0;
    if (b >= 64u) recip = 1.0 / 511.5, offset = -31.5 / 511.5;
    if (b >= 96u) recip = 1.0 / 255.75, offset = -63.5 / 255.75;
    if (b >= 192u) recip = 1.0 / 127.875, offset = -127.5 / 127.875;
    precise float linear = float(b) * recip + offset;
    return linear;
}

uint pack_snorm16(float v) { return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xFFFFu; }
float unpack_snorm16(uint bits) { return max(-1.0, float(int(bits << 16u) >> 16) / 32767.0); }

#ifndef EDRAM_NO_TARGET
// A color the host keeps to the guest's bits (two dwords for 64-bit formats).
uvec2 pack_color(vec4 v) {
    switch (target.format) {
    case 0u: case 1u: // 8_8_8_8, gamma (encoded by the host, or kept linear: flags bit 2)
        if (target.format == 1u && (target.flags & 4u) != 0u)
            v.rgb = vec3(linear_to_pwl_gamma(v.r), linear_to_pwl_gamma(v.g), linear_to_pwl_gamma(v.b));
        return uvec2(packUnorm4x8(v), 0u);
    case 2u: case 10u: { // 2_10_10_10
        uvec4 u = uvec4(round(clamp(v, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0)));
        return uvec2(u.r | (u.g << 10u) | (u.b << 20u) | (u.a << 30u), 0u);
    }
    case 3u: case 12u: { // 2_10_10_10_FLOAT
        uint a = uint(round(clamp(v.a, 0.0, 1.0) * 3.0));
        return uvec2(float32_to_7e3(v.r) | (float32_to_7e3(v.g) << 10u) | (float32_to_7e3(v.b) << 20u) | (a << 30u), 0u);
    }
    case 4u: return uvec2(pack_snorm16(v.r) | (pack_snorm16(v.g) << 16u), 0u); // 16_16
    case 5u: return uvec2(pack_snorm16(v.r) | (pack_snorm16(v.g) << 16u), pack_snorm16(v.b) | (pack_snorm16(v.a) << 16u));
    case 6u: return uvec2(packHalf2x16(v.rg), 0u);
    case 7u: return uvec2(packHalf2x16(v.rg), packHalf2x16(v.ba));
    case 14u: return uvec2(floatBitsToUint(v.r), 0u);
    case 15u: return uvec2(floatBitsToUint(v.r), floatBitsToUint(v.g));
    }
    return uvec2(0u);
}

// A float target's host components as they are, which are the guest's bits
// (flags bit 3)
uvec2 pack_raw(uvec4 u) {
    switch (target.format) {
    case 6u: return uvec2((u.r & 0xFFFFu) | (u.g << 16u), 0u);
    case 7u: return uvec2((u.r & 0xFFFFu) | (u.g << 16u), (u.b & 0xFFFFu) | (u.a << 16u));
    case 14u: return uvec2(u.r, 0u);
    case 15u: return uvec2(u.r, u.g);
    }
    return uvec2(0u);
}

uvec4 unpack_raw(uvec2 b) {
    switch (target.format) {
    case 6u: return uvec4(b.x & 0xFFFFu, b.x >> 16u, 0u, 0u);
    case 7u: return uvec4(b.x & 0xFFFFu, b.x >> 16u, b.y & 0xFFFFu, b.y >> 16u);
    case 14u: return uvec4(b.x, 0u, 0u, 0u);
    case 15u: return uvec4(b.x, b.y, 0u, 0u);
    }
    return uvec4(0u);
}

vec4 unpack_color(uvec2 b) {
    switch (target.format) {
    case 0u: case 1u:
        if (target.format == 1u && (target.flags & 4u) != 0u)
            return vec4(pwl_gamma_byte_to_linear_midpoint(b.x & 0xFFu), pwl_gamma_byte_to_linear_midpoint((b.x >> 8u) & 0xFFu),
                        pwl_gamma_byte_to_linear_midpoint((b.x >> 16u) & 0xFFu), float(b.x >> 24u) * (1.0 / 255.0));
        return unpackUnorm4x8(b.x);
    case 2u: case 10u:
        return vec4(b.x & 0x3FFu, (b.x >> 10u) & 0x3FFu, (b.x >> 20u) & 0x3FFu, b.x >> 30u) / vec4(1023.0, 1023.0, 1023.0, 3.0);
    case 3u: case 12u:
        return vec4(float7e3_to_32(b.x), float7e3_to_32(b.x >> 10u), float7e3_to_32(b.x >> 20u), float(b.x >> 30u) / 3.0);
    case 4u: return vec4(unpack_snorm16(b.x), unpack_snorm16(b.x >> 16u), 0.0, 0.0);
    case 5u: return vec4(unpack_snorm16(b.x), unpack_snorm16(b.x >> 16u), unpack_snorm16(b.y), unpack_snorm16(b.y >> 16u));
    case 6u: return vec4(unpackHalf2x16(b.x), 0.0, 0.0);
    case 7u: return vec4(unpackHalf2x16(b.x), unpackHalf2x16(b.y));
    case 14u: return vec4(uintBitsToFloat(b.x), 0.0, 0.0, 0.0);
    case 15u: return vec4(uintBitsToFloat(b.x), uintBitsToFloat(b.y), 0.0, 0.0);
    }
    return vec4(0.0);
}
#endif
