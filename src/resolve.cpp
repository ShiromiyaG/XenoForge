// Resolves (resolve.h). Translated from Xenia: the registers read as
// draw_util.cc's GetResolveInfo and GetScissor do (the rectangle from the
// vertices of vertex fetch constant 0 by the top-left rule, the window offset
// and scissors, the destination base split around its 32x32 tile), the copy
// as ResolveInfo::GetCopyShader chooses it (a raw copy when the formats are
// bitwise equal, else converted) and the resolve shaders do it
// (shaders/resolve.xesli, resolve_fast_*.xesli and resolve_full_*.xesli:
// unpacking, piecewise-linear gamma decoding, exponent bias, red/blue swap,
// packing per number format as pixel_formats.xesli's XePack*, byte swaps as
// endian.xesli's, the tiled destination address). Also ui/graphics_util.cc's
// FloatToD3D11Fixed16p8. BSD; see LICENSE-xenia.

#include "xenoforge/resolve.h"

#include "xenoforge/xenos.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace xf::resolve {

namespace {

namespace reg = xenos::reg;
using texture::HostFormat;

constexpr uint32_t EDRAM_TILE_COUNT = 2048;
constexpr uint32_t RESOLVE_ALIGNMENT = 8;
constexpr uint32_t MAX_RESOLVE_SIZE = (1 << 14) - RESOLVE_ALIGNMENT;

// texture formats (xenos.h's TextureFormat)
enum : uint32_t {
    k_8 = 2, k_1_5_5_5 = 3, k_5_6_5 = 4, k_6_5_5 = 5, k_8_8_8_8 = 6, k_2_10_10_10 = 7, k_8_A = 8, k_8_B = 9,
    k_8_8 = 10, k_16_16_EDRAM = 13, k_8_8_8_8_A = 14, k_4_4_4_4 = 15, k_10_11_11 = 16, k_11_11_10 = 17,
    k_16_16_16_16_EDRAM = 21, k_24_8 = 22, k_24_8_FLOAT = 23, k_16 = 24, k_16_16 = 25, k_16_16_16_16 = 26,
    k_16_FLOAT = 30, k_16_16_FLOAT = 31, k_16_16_16_16_FLOAT = 32, k_32_FLOAT = 36, k_32_32_FLOAT = 37,
    k_32_32_32_32_FLOAT = 38, k_8_8_8_8_AS_16_16_16_16 = 50, k_2_10_10_10_AS_16_16_16_16 = 54,
    k_10_11_11_AS_16_16_16_16 = 55, k_11_11_10_AS_16_16_16_16 = 56,
};
// color render target formats (xenos.h's ColorRenderTargetFormat)
enum : uint32_t {
    rt_8_8_8_8 = 0, rt_8_8_8_8_GAMMA = 1, rt_2_10_10_10 = 2, rt_2_10_10_10_FLOAT = 3, rt_16_16 = 4,
    rt_16_16_16_16 = 5, rt_16_16_FLOAT = 6, rt_16_16_16_16_FLOAT = 7, rt_2_10_10_10_AS_10_10_10_10 = 10,
    rt_2_10_10_10_FLOAT_AS_16_16_16_16 = 12, rt_32_FLOAT = 14, rt_32_32_FLOAT = 15,
};

// texture_info_formats.inl's kResolvable
bool resolvable(uint32_t f) {
    switch (f) {
    case k_8: case k_1_5_5_5: case k_5_6_5: case k_6_5_5: case k_8_8_8_8: case k_2_10_10_10: case k_8_A: case k_8_B:
    case k_8_8: case k_8_8_8_8_A: case k_4_4_4_4: case k_10_11_11: case k_11_11_10: case k_16: case k_16_16:
    case k_16_16_16_16: case k_16_FLOAT: case k_16_16_FLOAT: case k_16_16_16_16_FLOAT: case k_32_FLOAT:
    case k_32_32_FLOAT: case k_32_32_32_32_FLOAT: case k_8_8_8_8_AS_16_16_16_16: case k_2_10_10_10_AS_16_16_16_16:
    case k_10_11_11_AS_16_16_16_16: case k_11_11_10_AS_16_16_16_16: return true;
    }
    return false;
}

bool is_float_format(uint32_t f) {
    return f == k_16_FLOAT || f == k_16_16_FLOAT || f == k_16_16_16_16_FLOAT || f == k_32_FLOAT ||
           f == k_32_32_FLOAT || f == k_32_32_32_32_FLOAT;
}

// xenos.h's IsColorResolveFormatBitwiseEquivalent
bool bitwise_equivalent(uint32_t rt, uint32_t f) {
    switch (rt) {
    case rt_8_8_8_8: case rt_8_8_8_8_GAMMA: return f == k_8_8_8_8 || f == k_8_8_8_8_A || f == k_8_8_8_8_AS_16_16_16_16;
    case rt_2_10_10_10:
    case rt_2_10_10_10_AS_10_10_10_10: return f == k_2_10_10_10 || f == k_2_10_10_10_AS_16_16_16_16;
    case rt_16_16_FLOAT: return f == k_16_16_FLOAT;
    case rt_16_16_16_16_FLOAT: return f == k_16_16_16_16_FLOAT;
    case rt_32_FLOAT: return f == k_32_FLOAT;
    case rt_32_32_FLOAT: return f == k_32_32_FLOAT;
    }
    return false;
}

bool is_64bpp(uint32_t rt) {
    return rt == rt_16_16_16_16 || rt == rt_16_16_16_16_FLOAT || rt == rt_2_10_10_10_FLOAT_AS_16_16_16_16 ||
           rt == rt_32_32_FLOAT;
}

int32_t sign_extend(uint32_t v, uint32_t bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

// xenos.h's GpuSwap
uint32_t gpu_swap(uint32_t v, uint32_t endian) {
    switch (endian) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
    case 2: return (v >> 24) | ((v >> 8) & 0xFF00) | ((v & 0xFF00) << 8) | (v << 24);
    case 3: return (v >> 16) | (v << 16);
    }
    return v;
}

// ui/graphics_util.cc's FloatToD3D11Fixed16p8
int32_t float_to_fixed16p8(float f) {
    if (!(std::abs(f) >= 1.0f / 512.0f)) return 0;
    if (f >= 32768.0f - 1.0f / 256.0f) return (1 << 23) - 1;
    if (f <= -32768.0f) return -32768 * 256;
    uint32_t bits = std::bit_cast<uint32_t>(f);
    int32_t result = int32_t((bits & ((1 << 23) - 1)) | (1 << 23));
    if (bits >> 31) result = -result;
    int32_t exponent = int32_t((bits >> 23) & 255) - 127;
    uint32_t extra_bits = uint32_t(15 - exponent);
    if (extra_bits) {
        uint32_t u = uint32_t(result);
        u += (1u << (extra_bits - 1)) - 1 + ((u >> extra_bits) & 1);
        result = int32_t(u) >> extra_bits;
    }
    return result;
}

// endian.xesli's, with Endian128
uint32_t swap16(uint32_t v, uint32_t e) {
    if (e == 1) v = ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    return v;
}
uint32_t swap32(uint32_t v, uint32_t e) {
    if (e == 1 || e == 2) v = ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    if (e == 2 || e == 3) v = (v << 16) | (v >> 16);
    return v;
}
void swap_words(uint32_t *w, uint32_t count, uint32_t e) {
    if (count == 4 && e == 5) std::swap(w[0], w[3]), std::swap(w[1], w[2]), e = 2; // 8in128
    if (count >= 2 && e == 4) {                                                    // 8in64
        for (uint32_t i = 0; i + 1 < count; i += 2) std::swap(w[i], w[i + 1]);
        e = 2;
    }
    for (uint32_t i = 0; i < count; i++) w[i] = swap32(w[i], e);
}

uint32_t load_le32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t load_le16(const uint8_t *p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

float half_to_float(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0x1F) bits = sign | 0x7F800000 | (mantissa << 13);
    else if (exponent) bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    else if (!mantissa) bits = sign;
    else {
        int e = -1;
        do e++, mantissa <<= 1;
        while (!(mantissa & 0x400));
        bits = sign | (uint32_t(112 - e) << 23) | ((mantissa & 0x3FF) << 13);
    }
    return std::bit_cast<float>(bits);
}

// single to half, rounding to nearest even, overflowing to infinity
uint16_t float_to_half(float f) {
    uint32_t bits = std::bit_cast<uint32_t>(f);
    uint32_t sign = (bits >> 16) & 0x8000, exponent = (bits >> 23) & 0xFF, mantissa = bits & 0x7FFFFF;
    if (exponent == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    int32_t e = int32_t(exponent) - 127 + 15;
    if (e >= 31) return uint16_t(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        mantissa |= 0x800000;
        uint32_t shift = uint32_t(14 - e);
        uint32_t half = mantissa >> shift, rest = mantissa & ((1u << shift) - 1), midpoint = 1u << (shift - 1);
        if (rest > midpoint || (rest == midpoint && (half & 1))) half++;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(e) << 10) | (mantissa >> 13), rest = mantissa & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) half++; // may carry into the exponent, as it should
    return uint16_t(sign | half);
}
uint32_t pack_half2(float a, float b) { return uint32_t(float_to_half(a)) | uint32_t(float_to_half(b)) << 16; }

// pixel_formats.xesli's XePackFixed
uint32_t pack_fixed(float value, uint32_t bits, uint32_t number) {
    uint32_t unsigned_max = (1u << bits) - 1;
    if (std::isnan(value)) value = 0;
    uint32_t packed;
    if (number == 1) { // signed repeating fraction
        float positive_max = float((1u << (bits - 1)) - 1);
        packed = uint32_t(int32_t(std::clamp(value, -1.0f, 1.0f) * positive_max + (value >= 0 ? 0.5f : -0.5f)));
    } else if (number == 2) { // unsigned integer
        packed = uint32_t(std::clamp(value, 0.0f, float(unsigned_max)) + 0.5f);
    } else if (number == 3) { // signed integer
        float lo = -float(1u << (bits - 1)), hi = float((1u << (bits - 1)) - 1);
        packed = uint32_t(int32_t(std::clamp(value, lo, hi) + (value >= 0 ? 0.5f : -0.5f)));
    } else {
        packed = uint32_t(std::clamp(value, 0.0f, 1.0f) * float(unsigned_max) + 0.5f);
    }
    return packed & unsigned_max;
}

// resolve.xesli's piecewise-linear gamma, for 8_8_8_8_GAMMA sources
uint32_t pwl_gamma_to_linear_code(uint32_t c) {
    uint32_t scale = c < 64 ? 1 : c < 96 ? 2 : c < 192 ? 4 : 8;
    uint32_t offset = c < 64 ? 0 : c < 96 ? 64 : c < 192 ? 256 : 1024;
    uint32_t linear_code = c * scale - offset;
    return linear_code + ((linear_code * scale) >> 10);
}
float average_pwl_gamma(float sum, float sample_weight) {
    uint32_t m = uint32_t(sum * sample_weight);
    uint32_t c = m < 64 ? m : m < 128 ? (m + 64) >> 1 : m < 512 ? (m + 256) >> 2 : (m + 1024) >> 3;
    c = std::min(c, 255u);
    uint32_t n = pwl_gamma_to_linear_code(c);
    return n == 1023 ? 1.0f : float((n << 14) | (n << 4) | (n >> 6)) * (1.0f / 16777216.0f);
}

// A source pixel, from the host format, as the resolve shaders unpack it
// from the EDRAM (XeResolveUnpack*): two-component formats with blue and
// alpha 0, one-component ones with the rest 0.
void unpack(uint32_t rt, const uint8_t *p, float c[4]) {
    c[0] = c[1] = c[2] = c[3] = 0;
    switch (rt) {
    case rt_8_8_8_8:
    case rt_8_8_8_8_GAMMA:
        for (int i = 0; i < 4; i++) c[i] = float(p[i]) * (1.0f / 255.0f);
        break;
    case rt_2_10_10_10:
    case rt_2_10_10_10_AS_10_10_10_10: {
        uint32_t v = load_le32(p);
        for (int i = 0; i < 3; i++) c[i] = float((v >> (10 * i)) & 1023) * (1.0f / 1023.0f);
        c[3] = float(v >> 30) * (1.0f / 3.0f);
        break;
    }
    case rt_16_16:
    case rt_16_16_16_16:
        for (int i = 0; i < (rt == rt_16_16 ? 2 : 4); i++) c[i] = float(int16_t(load_le16(p + 2 * i))) * (1.0f / 1024.0f);
        break;
    case rt_16_16_FLOAT:
    case rt_16_16_16_16_FLOAT:
    case rt_2_10_10_10_FLOAT:
    case rt_2_10_10_10_FLOAT_AS_16_16_16_16:
        for (int i = 0; i < (rt == rt_16_16_FLOAT ? 2 : 4); i++) c[i] = half_to_float(load_le16(p + 2 * i));
        break;
    case rt_32_FLOAT: c[0] = std::bit_cast<float>(load_le32(p)); break;
    case rt_32_32_FLOAT:
        c[0] = std::bit_cast<float>(load_le32(p)), c[1] = std::bit_cast<float>(load_le32(p + 4));
        break;
    }
}

// The destination's words for one pixel, converted (the full resolve
// shaders); returns the word count, 0 for a 1-byte or 2-byte destination,
// which goes in small.
uint32_t convert(const Info &info, const uint8_t *src, uint32_t dest_bytes, uint32_t w[4], uint32_t &small) {
    uint32_t rt = info.source_format;
    float c[4];
    unpack(rt, src, c);
    float factor = std::ldexp(1.0f, info.dest_exp_bias);
    bool gamma = rt == rt_8_8_8_8_GAMMA;
    if (dest_bytes == 1) {
        // XeResolveLoad8ScalarColors: D3D maps A8 to k_8 with the swap
        bool alpha = info.dest_swap;
        float v = alpha ? c[3] : c[0];
        if (gamma && !alpha) v = average_pwl_gamma(float(pwl_gamma_to_linear_code(uint32_t(v * 255.0f + 0.5f))), 1);
        small = pack_fixed(v * factor, 8, info.dest_number);
        return 0;
    }
    if (gamma) {
        for (int i = 0; i < 3; i++)
            c[i] = average_pwl_gamma(float(pwl_gamma_to_linear_code(uint32_t(c[i] * 255.0f + 0.5f))), 1) * factor;
        c[3] *= factor;
    } else {
        for (float &v : c) v *= factor;
    }
    if (info.dest_swap) std::swap(c[0], c[2]);
    uint32_t n = info.dest_number;
    auto fixed4 = [&](uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3) {
        return pack_fixed(c[0], b0, n) | pack_fixed(c[1], b1, n) << b0 |
               (b2 ? pack_fixed(c[2], b2, n) << (b0 + b1) : 0) | (b3 ? pack_fixed(c[3], b3, n) << (b0 + b1 + b2) : 0);
    };
    switch (dest_bytes) {
    case 2:
        switch (info.dest_format) {
        case k_1_5_5_5: small = fixed4(5, 5, 5, 1); break;
        case k_5_6_5: small = fixed4(5, 6, 5, 0); break;
        case k_6_5_5: small = fixed4(5, 5, 6, 0); break;
        case k_8_8: small = fixed4(8, 8, 0, 0); break;
        case k_4_4_4_4: small = fixed4(4, 4, 4, 4); break;
        case k_16: small = pack_fixed(c[0], 16, n); break;
        default: small = float_to_half(c[0]); break; // as 16_FLOAT
        }
        return 0;
    case 4:
        switch (info.dest_format) {
        case k_8_8_8_8: case k_8_8_8_8_A: case k_8_8_8_8_AS_16_16_16_16: w[0] = fixed4(8, 8, 8, 8); break;
        case k_2_10_10_10: case k_2_10_10_10_AS_16_16_16_16: w[0] = fixed4(10, 10, 10, 2); break;
        case k_10_11_11: case k_10_11_11_AS_16_16_16_16: w[0] = fixed4(11, 11, 10, 0); break;
        case k_11_11_10: case k_11_11_10_AS_16_16_16_16: w[0] = fixed4(10, 11, 11, 0); break;
        case k_16_16_EDRAM: case k_16_16: w[0] = fixed4(16, 16, 0, 0); break;
        case k_16_16_FLOAT: w[0] = pack_half2(c[0], c[1]); break;
        default: w[0] = std::bit_cast<uint32_t>(c[0]); break; // as 32_FLOAT
        }
        return 1;
    case 8:
        switch (info.dest_format) {
        case k_16_16_16_16_EDRAM:
        case k_16_16_16_16:
            w[0] = pack_fixed(c[0], 16, n) | pack_fixed(c[1], 16, n) << 16;
            w[1] = pack_fixed(c[2], 16, n) | pack_fixed(c[3], 16, n) << 16;
            break;
        case k_16_16_16_16_FLOAT: w[0] = pack_half2(c[0], c[1]), w[1] = pack_half2(c[2], c[3]); break;
        default: w[0] = std::bit_cast<uint32_t>(c[0]), w[1] = std::bit_cast<uint32_t>(c[1]); break; // 32_32_FLOAT
        }
        return 2;
    default:
        for (int i = 0; i < 4; i++) w[i] = std::bit_cast<uint32_t>(c[i]);
        return 4;
    }
}

// The fast resolve shaders: the source bits as they are, red and blue swapped
// in place (XeResolveSwap*PixelsRedBlue*).
uint32_t copy_raw(const Info &info, const uint8_t *src, uint32_t w[4]) {
    if (info.depth) {
        w[0] = load_le32(src);
        return 1;
    }
    uint32_t rt = info.source_format;
    uint32_t count = is_64bpp(rt) ? 2 : 1;
    for (uint32_t i = 0; i < count; i++) w[i] = load_le32(src + 4 * i);
    if (info.dest_swap) {
        switch (rt) {
        case rt_8_8_8_8:
        case rt_8_8_8_8_GAMMA: w[0] = (w[0] & ~0xFF00FFu) | ((w[0] & 0xFF) << 16) | ((w[0] >> 16) & 0xFF); break;
        case rt_2_10_10_10:
        case rt_2_10_10_10_FLOAT:
        case rt_2_10_10_10_AS_10_10_10_10:
        case rt_2_10_10_10_FLOAT_AS_16_16_16_16:
            w[0] = (w[0] & ~0x3FF003FFu) | ((w[0] & 0x3FF) << 20) | ((w[0] >> 20) & 0x3FF);
            break;
        case rt_16_16_16_16:
        case rt_16_16_16_16_FLOAT: {
            uint32_t r = w[0] & 0xFFFF, b = w[1] & 0xFFFF;
            w[0] = (w[0] & ~0xFFFFu) | b, w[1] = (w[1] & ~0xFFFFu) | r;
            break;
        }
        }
    }
    return count;
}

} // namespace

// draw_util.cc's SanitizeCopySampleSelect: what the surface's samples allow
// (depth is never averaged)
uint32_t sanitize_sample_select(uint32_t select, uint32_t msaa, bool depth) {
    if (msaa >= 2) {
        select = std::min(select, SAMPLES_0123);
        if (depth) {
            if (select == SAMPLES_01 || select == SAMPLES_0123) select = 0;
            else if (select == SAMPLES_23) select = 2;
        }
    } else if (msaa == 1) {
        if (select == 2) select = 0;
        else if (select == 3) select = 1;
        else if (select > SAMPLES_01) select = SAMPLES_01;
        if (depth && select == SAMPLES_01) select = 0;
    } else {
        select = 0;
    }
    return select;
}

HostFormat host_format_of_color_target(uint32_t format) {
    switch (format) {
    case rt_8_8_8_8: case rt_8_8_8_8_GAMMA: return HostFormat::RGBA8;
    case rt_2_10_10_10: case rt_2_10_10_10_AS_10_10_10_10: return HostFormat::RGB10A2;
    case rt_2_10_10_10_FLOAT: case rt_2_10_10_10_FLOAT_AS_16_16_16_16: return HostFormat::RGBA16F;
    case rt_16_16: return HostFormat::RG16;
    case rt_16_16_16_16: return HostFormat::RGBA16;
    case rt_16_16_FLOAT: return HostFormat::RG16F;
    case rt_16_16_16_16_FLOAT: return HostFormat::RGBA16F;
    case rt_32_FLOAT: return HostFormat::R32F;
    case rt_32_32_FLOAT: return HostFormat::RG32F;
    }
    return HostFormat::Unsupported;
}

bool get_info(std::span<const uint32_t> regs, const GuestMemory &memory, Info &out, std::string &error) {
    out = Info{};
    auto r = [&](uint32_t index) { return index < regs.size() ? regs[index] : 0u; };
    using namespace xenos::reg;

    uint32_t control = r(reg::RB_COPY_CONTROL);
    uint32_t command = (control >> 20) & 3;
    if (command == 2) {
        error = "resolve copy command 2 (to k_DXT3A_AS_1_1_1_1)";
        return false;
    }
    if (command == 3) {
        error = "resolve copy command 3";
        return false;
    }

    // The rectangle: Direct3D 9 draws it with three vertices in vertex fetch
    // constant 0, written by the CPU.
    uint32_t f0 = r(reg::FETCH_CONSTANTS), f1 = r(reg::FETCH_CONSTANTS + 1);
    if ((f0 & 3) != uint32_t(xenos::FetchConstantType::Vertex) || ((f1 >> 2) & 0xFFFFFF) != 6) {
        error = "resolve vertex buffer format";
        return false;
    }
    uint32_t vertices_address = f0 & ~3u, vertices_endian = f1 & 3;
    // Most vertices have a negative half-pixel offset applied, reversed here.
    float half_pixel = (r(PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
    int32_t v[6];
    for (int i = 0; i < 6; i++) {
        uint8_t b[4];
        memory.read(vertices_address + 4 * i, b);
        v[i] = float_to_fixed16p8(std::bit_cast<float>(gpu_swap(load_le32(b), vertices_endian)) + half_pixel);
    }
    // top-left inclusive (.5 included), bottom-right exclusive
    int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8, y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
    int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8, y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;

    uint32_t window_offset = r(PA_SC_WINDOW_OFFSET);
    int32_t window_x = sign_extend(window_offset & 0x7FFF, 15), window_y = sign_extend((window_offset >> 16) & 0x7FFF, 15);
    if ((r(PA_SU_SC_MODE_CNTL) >> 16) & 1) x0 += window_x, y0 += window_y, x1 += window_x, y1 += window_y;

    // GetScissor, not clamped to the surface pitch: the window scissor moved
    // by the window offset (unless disabled), within the screen scissor
    uint32_t window_tl = r(PA_SC_WINDOW_SCISSOR_TL), window_br = r(PA_SC_WINDOW_SCISSOR_BR);
    uint32_t screen_tl = r(PA_SC_SCREEN_SCISSOR_TL), screen_br = r(PA_SC_SCREEN_SCISSOR_BR);
    int32_t tl_x = int32_t(window_tl & 0x3FFF), tl_y = int32_t((window_tl >> 16) & 0x3FFF);
    int32_t br_x = int32_t(window_br & 0x3FFF), br_y = int32_t((window_br >> 16) & 0x3FFF);
    if (!(window_tl >> 31)) tl_x += window_x, tl_y += window_y, br_x += window_x, br_y += window_y;
    tl_x = std::max(tl_x, sign_extend(screen_tl & 0x7FFF, 15));
    tl_y = std::max(tl_y, sign_extend((screen_tl >> 16) & 0x7FFF, 15));
    br_x = std::min(br_x, sign_extend(screen_br & 0x7FFF, 15));
    br_y = std::min(br_y, sign_extend((screen_br >> 16) & 0x7FFF, 15));
    tl_x = std::max(tl_x, 0), tl_y = std::max(tl_y, 0);
    br_x = std::max(br_x, tl_x), br_y = std::max(br_y, tl_y);
    x0 = std::clamp(x0, tl_x, br_x), y0 = std::clamp(y0, tl_y, br_y);
    x1 = std::clamp(x1, tl_x, br_x), y1 = std::clamp(y1, tl_y, br_y);

    // Direct3D 9 aligns the rectangle to 8, expanding it right and down.
    x0 &= ~int32_t(RESOLVE_ALIGNMENT - 1), y0 &= ~int32_t(RESOLVE_ALIGNMENT - 1);
    x1 = (x1 + int32_t(RESOLVE_ALIGNMENT - 1)) & ~int32_t(RESOLVE_ALIGNMENT - 1);
    y1 = (y1 + int32_t(RESOLVE_ALIGNMENT - 1)) & ~int32_t(RESOLVE_ALIGNMENT - 1);

    uint32_t surface_info = r(reg::RB_SURFACE_INFO);
    out.msaa = (surface_info >> 16) & 3;
    if (out.msaa == 3) {
        error = "resolve from 8x MSAA";
        return false;
    }
    out.surface_pitch = surface_info & 0x3FFF;
    int32_t pitch_aligned = int32_t(out.surface_pitch & ~(RESOLVE_ALIGNMENT - 1));
    if (x1 > pitch_aligned) x0 = std::min(x0, pitch_aligned), x1 = std::min(x1, pitch_aligned);
    if (y1 - y0 > int32_t(MAX_RESOLVE_SIZE)) y1 = y0 + int32_t(MAX_RESOLVE_SIZE);
    if (x0 >= x1 || y0 >= y1) {
        // nothing to copy or clear
        out.copy = false;
        return true;
    }
    out.x = uint32_t(x0), out.y = uint32_t(y0), out.width = uint32_t(x1 - x0), out.height = uint32_t(y1 - y0);

    // the source
    uint32_t source = control & 7;
    out.depth = source >= 4;
    out.sample_select = sanitize_sample_select((control >> 4) & 7, out.msaa, out.depth);
    uint32_t depth_info = r(reg::RB_DEPTH_INFO);
    if (out.depth) {
        out.edram_base = (depth_info & 0x1FFF) & (EDRAM_TILE_COUNT - 1);
        out.source_format = (depth_info >> 16) & 1;
    } else {
        static const uint32_t infos[] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO,
                                         reg::RB_COLOR3_INFO};
        uint32_t color_info = r(infos[source]);
        out.color_target = source;
        out.edram_base = (color_info & 0x1FFF) & (EDRAM_TILE_COUNT - 1);
        out.source_format = (color_info >> 16) & 0xF;
    }

    // the clears
    out.clear_color = (control >> 8) & 1;
    out.clear_depth = (control >> 9) & 1;
    out.clear_color_value[0] = r(RB_COLOR_CLEAR), out.clear_color_value[1] = r(RB_COLOR_CLEAR_LO);
    out.clear_depth_value = r(RB_DEPTH_CLEAR);

    // the destination
    uint32_t dest_info = r(reg::RB_COPY_DEST_INFO);
    out.dest_endian = dest_info & 7;
    out.dest_array = (dest_info >> 3) & 1;
    out.dest_slice = (dest_info >> 4) & 7;
    out.dest_number = (dest_info >> 13) & 7;
    out.dest_exp_bias = out.depth ? 0 : sign_extend((dest_info >> 16) & 0x3F, 6);
    out.dest_swap = !out.depth && ((dest_info >> 24) & 1);
    if (out.depth) {
        // Direct3D gives 8_8_8_8; the depth format keeps the bits as they are
        out.dest_format = out.source_format ? k_24_8_FLOAT : k_24_8;
    } else {
        out.dest_format = (dest_info >> 7) & 0x3F;
        if (out.dest_format == k_8_A || out.dest_format == k_8_B) out.dest_format = k_8;
        else if (out.dest_format == k_8_8_8_8_A) out.dest_format = k_8_8_8_8;
    }

    uint32_t dest_pitch_reg = r(RB_COPY_DEST_PITCH);
    uint32_t dest_pitch = dest_pitch_reg & 0x3FFF, dest_height = (dest_pitch_reg >> 16) & 0x3FFF;
    // For volume resolves Direct3D writes pitch * level height in blocks to
    // RB_COPY_SURFACE_SLICE; copy_dest_height only as a fallback.
    if (out.dest_array) {
        uint32_t slice = r(RB_COPY_SURFACE_SLICE);
        if (slice && dest_pitch) dest_height = slice / dest_pitch;
    }
    out.dest_pitch = (dest_pitch + 31) & ~31u;
    out.dest_height = (dest_height + 31) & ~31u;

    uint32_t dest_base = r(reg::RB_COPY_DEST_BASE);
    if (!out.depth && !resolvable(out.dest_format)) {
        error = std::string("resolve to ") + xenos::format_name(out.dest_format) + ", not a color format";
        out.copy = false;
        return true;
    }
    uint32_t bpp_log2 = std::countr_zero(texture::format_block(out.dest_format).bytes);
    uint32_t x_mask = (1u << (7 - std::min(2u, bpp_log2 + uint32_t(out.dest_array)))) - 1;
    uint32_t y_mask = (1u << (out.dest_array ? 5 : 7 - std::min(2u, bpp_log2))) - 1;
    uint32_t addr_base = dest_base, addr_x0 = out.x, addr_y0 = out.y;
    if (!out.dest_array) {
        // Direct3D advances RB_COPY_DEST_BASE in 32x32 tiles; an 8bpp or 16bpp
        // tile is under 4 KB, so part of the x offset may sit in the base.
        uint32_t tile_bytes_log2 = 10 + bpp_log2;
        uint32_t phase = (dest_base & 4095) >> tile_bytes_log2;
        addr_base -= phase << tile_bytes_log2;
        addr_x0 += phase << 5;
    }
    out.dest_x = addr_x0 & x_mask, out.dest_y = addr_y0 & y_mask;
    uint32_t base_x = addr_x0 & ~x_mask, base_y = addr_y0 & ~y_mask;
    if (out.dest_array)
        out.dest_base = addr_base + uint32_t(texture::tiled_offset_3d(base_x, base_y, 0, out.dest_pitch,
                                                                      out.dest_height, bpp_log2));
    else
        out.dest_base = addr_base + texture::tiled_offset_2d(base_x, base_y, out.dest_pitch, bpp_log2);
    return true;
}

bool write(const Info &info, std::span<const uint8_t> pixels, GuestMemory &memory, std::string &error) {
    if (!info.copy || !info.width || !info.height) return true;
    uint32_t src_bytes = info.depth ? 4 : texture::bytes_per_block(host_format_of_color_target(info.source_format));
    if (!src_bytes) {
        error = std::string("resolve from ") + xenos::color_target_format_name(info.source_format);
        return false;
    }
    if (pixels.size() < size_t(info.width) * info.height * src_bytes) {
        error = "resolve: fewer pixels than the rectangle";
        return false;
    }
    uint32_t dest_bytes = texture::format_block(info.dest_format).bytes;
    if (!dest_bytes || !std::has_single_bit(dest_bytes) || dest_bytes > 16) {
        error = std::string("resolve to ") + xenos::format_name(info.dest_format);
        return false;
    }
    uint32_t bpp_log2 = std::countr_zero(dest_bytes);
    // The raw copy (the fast shaders) when nothing changes the bits.
    bool raw = raw_copy(info);

    auto offset = [&](uint32_t i, uint32_t j) -> uint64_t {
        uint32_t x = info.dest_x + i, y = info.dest_y + j;
        if (info.dest_array)
            return texture::tiled_offset_3d(x, y, info.dest_slice, info.dest_pitch, info.dest_height, bpp_log2);
        return texture::tiled_offset_2d(x, y, info.dest_pitch, bpp_log2);
    };
    // The destination span read, written into and put back whole.
    uint64_t low = UINT64_MAX, high = 0;
    for (uint32_t j = 0; j < info.height; j++)
        for (uint32_t i = 0; i < info.width; i++) {
            uint64_t o = offset(i, j);
            low = std::min(low, o), high = std::max(high, o);
        }
    uint64_t span = high - low + dest_bytes;
    if (span > (256u << 20)) {
        error = "resolve destination span too large";
        return false;
    }
    std::vector<uint8_t> dest(span);
    memory.read(uint32_t(info.dest_base + low), dest);

    for (uint32_t j = 0; j < info.height; j++)
        for (uint32_t i = 0; i < info.width; i++) {
            const uint8_t *src = pixels.data() + (size_t(j) * info.width + i) * src_bytes;
            uint8_t *d = dest.data() + (offset(i, j) - low);
            uint32_t w[4] = {}, small = 0;
            uint32_t words = raw ? copy_raw(info, src, w) : convert(info, src, dest_bytes, w, small);
            if (dest_bytes == 1) {
                d[0] = uint8_t(small);
            } else if (dest_bytes == 2) {
                uint16_t v = uint16_t(swap16(small, info.dest_endian));
                std::memcpy(d, &v, 2);
            } else {
                swap_words(w, words, info.dest_endian);
                std::memcpy(d, w, dest_bytes);
            }
        }
    memory.write(uint32_t(info.dest_base + low), dest);
    return true;
}

bool raw_copy(const Info &info) {
    // (Xenia's fast shaders copy one sample; an average is converted)
    return info.depth || (sample_count(info.sample_select) == 1 && info.source_format != rt_8_8_8_8_GAMMA &&
                          !info.dest_exp_bias &&
                          bitwise_equivalent(info.source_format, info.dest_format) &&
                          info.dest_number == (is_float_format(info.dest_format) ? 7u : 0u));
}

bool destination_ranges(const Info &info, std::vector<std::pair<uint32_t, uint32_t>> &written,
                        std::vector<std::pair<uint32_t, uint32_t>> &partial) {
    written.clear(), partial.clear();
    if (!info.copy || !info.width || !info.height) return true;
    uint32_t dest_bytes = texture::format_block(info.dest_format).bytes;
    if (!dest_bytes || !std::has_single_bit(dest_bytes) || dest_bytes > 16) return false;
    uint32_t log2 = std::countr_zero(dest_bytes);
    constexpr uint64_t SIZE = 0x20000000;
    // [start, end) from the destination's base, wrapping at the end of
    // physical memory as the address mask does, joined to the last range
    // when it follows or overlaps it
    auto add = [&](std::vector<std::pair<uint32_t, uint32_t>> &out, uint64_t start, uint64_t end) {
        uint64_t at = (uint64_t(info.dest_base) + start) % SIZE, bytes = std::min(end - start, SIZE);
        while (bytes) {
            uint64_t n = std::min(bytes, SIZE - at);
            if (!out.empty() && at >= out.back().first && at <= out.back().second)
                out.back().second = uint32_t(std::max<uint64_t>(out.back().second, at + n));
            else
                out.push_back({uint32_t(at), uint32_t(at + n)});
            bytes -= n, at = 0;
        }
    };
    if (!info.dest_array && log2 >= 2) {
        // tiled_combine moves the tiled byte offset's bits from 8 up to 12
        // up and keeps the rest below 12; with texels of 4 bytes or more,
        // those bits are tile t's index times its size (1024 << log2 bytes,
        // 4 KB or more) plus a part of the texel's place under that size, so
        // tile t takes [t, t + 1) * tile bytes from the base (a tile's corner)
        uint64_t tile_bytes = uint64_t(32 * 32) << log2, pitch_tiles = info.dest_pitch >> 5;
        uint32_t x0 = info.dest_x, y0 = info.dest_y, x1 = x0 + info.width, y1 = y0 + info.height;
        uint32_t tx0 = x0 >> 5, tx1 = (x1 - 1) >> 5, ty0 = y0 >> 5, ty1 = (y1 - 1) >> 5;
        for (uint32_t ty = ty0; ty <= ty1; ty++) {
            uint64_t row = ty * pitch_tiles;
            add(written, (row + tx0) * tile_bytes, (row + tx1 + 1) * tile_bytes);
            if ((ty == ty0 && (y0 & 31)) || (ty == ty1 && (y1 & 31))) {
                add(partial, (row + tx0) * tile_bytes, (row + tx1 + 1) * tile_bytes);
                continue;
            }
            if (x0 & 31) add(partial, (row + tx0) * tile_bytes, (row + tx0 + 1) * tile_bytes);
            if (x1 & 31) add(partial, (row + tx1) * tile_bytes, (row + tx1 + 1) * tile_bytes);
        }
        return true;
    }
    uint64_t low = UINT64_MAX, high = 0;
    for (uint32_t j = 0; j < info.height; j++)
        for (uint32_t i = 0; i < info.width; i++) {
            uint32_t x = info.dest_x + i, y = info.dest_y + j;
            uint64_t o = info.dest_array
                             ? texture::tiled_offset_3d(x, y, info.dest_slice, info.dest_pitch, info.dest_height, log2)
                             : texture::tiled_offset_2d(x, y, info.dest_pitch, log2);
            low = std::min(low, o), high = std::max(high, o);
        }
    if (high - low > (256u << 20)) return false;
    add(written, low, high + dest_bytes);
    partial = written;
    return true;
}

} // namespace xf::resolve
