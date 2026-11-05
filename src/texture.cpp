// Guest textures read out of guest memory (texture.h). Translated from
// Xenia: the fetch constant (src/xenia/gpu/xenos.h, xe_gpu_texture_fetch_t),
// the formats' blocks (texture_info_formats.inl), the tiled addressing
// (texture_address.h), the levels and where they lie (texture_util.cc:
// GetSubresourcesFromFetchConstant, GetPackedMipLevel, GetPackedMipOffset,
// GetGuestTextureLayout), the loads' addressing, levels and byte swaps
// (vulkan/vulkan_texture_cache.cc's LoadTextureData, shaders/texture_load.xesli
// and texture_load_*bpb.xesli, endian.xesli), the host formats
// (vulkan_texture_cache.cc's table), the decompressions (texture_load_dxt3a,
// texture_load_ctx1 and the DXT helpers of pixel_formats.xesli) and the depth
// loads (texture_load_depth_unorm, texture_load_depth_float). BSD; see
// LICENSE-xenia.

#include "xenoforge/texture.h"

#include "xenoforge/xenos.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace xf::texture {

namespace {

// Formats Xenia's Vulkan texture cache turns into others on the CPU side of
// the load; the rest keep the guest's bits. Depth (24_8, 24_8_FLOAT) becomes
// 32-bit floats as texture_load_depth_unorm and texture_load_depth_float make
// it.
using Convert = Load;

struct FormatEntry {
    uint8_t block_width, block_height;
    uint8_t bits; // per texel (texture_info_formats.inl's bits_per_pixel)
    HostFormat host;
    Convert convert = Convert::Copy;
};

using H = HostFormat;
constexpr FormatEntry FORMATS[64] = {
    {1, 1, 1, H::Unsupported},       // k_1_REVERSE
    {1, 1, 1, H::Unsupported},       // k_1
    {1, 1, 8, H::R8},                // k_8
    {1, 1, 16, H::A1R5G5B5},         // k_1_5_5_5
    {1, 1, 16, H::B5G6R5},           // k_5_6_5
    {1, 1, 16, H::Unsupported},      // k_6_5_5
    {1, 1, 32, H::RGBA8},            // k_8_8_8_8
    {1, 1, 32, H::RGB10A2},          // k_2_10_10_10
    {1, 1, 8, H::R8},                // k_8_A
    {1, 1, 8, H::R8},                // k_8_B
    {1, 1, 16, H::RG8},              // k_8_8
    {2, 1, 16, H::Unsupported},      // k_Cr_Y1_Cb_Y0_REP
    {2, 1, 16, H::Unsupported},      // k_Y1_Cr_Y0_Cb_REP
    {1, 1, 32, H::Unsupported},      // k_16_16_EDRAM
    {1, 1, 32, H::RGBA8},            // k_8_8_8_8_A
    {1, 1, 16, H::A4R4G4B4},         // k_4_4_4_4
    {1, 1, 32, H::Unsupported},      // k_10_11_11
    {1, 1, 32, H::Unsupported},      // k_11_11_10
    {4, 4, 4, H::BC1},               // k_DXT1
    {4, 4, 8, H::BC2},               // k_DXT2_3
    {4, 4, 8, H::BC3},               // k_DXT4_5
    {1, 1, 64, H::Unsupported},      // k_16_16_16_16_EDRAM
    {1, 1, 32, H::R32F, Convert::DepthUnormToFloat}, // k_24_8
    {1, 1, 32, H::R32F, Convert::DepthFloatToFloat}, // k_24_8_FLOAT
    {1, 1, 16, H::R16},              // k_16
    {1, 1, 32, H::RG16},             // k_16_16
    {1, 1, 64, H::RGBA16},           // k_16_16_16_16
    {1, 1, 16, H::R16F},             // k_16_EXPAND: 16-bit floats, as Xenia reads them
    {1, 1, 32, H::RG16F},            // k_16_16_EXPAND
    {1, 1, 64, H::RGBA16F},          // k_16_16_16_16_EXPAND
    {1, 1, 16, H::R16F},             // k_16_FLOAT
    {1, 1, 32, H::RG16F},            // k_16_16_FLOAT
    {1, 1, 64, H::RGBA16F},          // k_16_16_16_16_FLOAT
    {1, 1, 32, H::R32},              // k_32
    {1, 1, 64, H::RG32},             // k_32_32
    {1, 1, 128, H::RGBA32},          // k_32_32_32_32
    {1, 1, 32, H::R32F},             // k_32_FLOAT
    {1, 1, 64, H::RG32F},            // k_32_32_FLOAT
    {1, 1, 128, H::RGBA32F},         // k_32_32_32_32_FLOAT
    {4, 1, 8, H::Unsupported},       // k_32_AS_8
    {2, 1, 16, H::Unsupported},      // k_32_AS_8_8
    {1, 1, 16, H::Unsupported},      // k_16_MPEG
    {1, 1, 32, H::Unsupported},      // k_16_16_MPEG
    {1, 1, 8, H::Unsupported},       // k_8_INTERLACED
    {4, 1, 8, H::Unsupported},       // k_32_AS_8_INTERLACED
    {1, 1, 16, H::Unsupported},      // k_32_AS_8_8_INTERLACED
    {1, 1, 16, H::Unsupported},      // k_16_INTERLACED
    {1, 1, 16, H::Unsupported},      // k_16_MPEG_INTERLACED
    {1, 1, 32, H::Unsupported},      // k_16_16_MPEG_INTERLACED
    {4, 4, 8, H::BC5},               // k_DXN
    {1, 1, 32, H::RGBA8},            // k_8_8_8_8_AS_16_16_16_16
    {4, 4, 4, H::BC1},               // k_DXT1_AS_16_16_16_16
    {4, 4, 8, H::BC2},               // k_DXT2_3_AS_16_16_16_16
    {4, 4, 8, H::BC3},               // k_DXT4_5_AS_16_16_16_16
    {1, 1, 32, H::RGB10A2},          // k_2_10_10_10_AS_16_16_16_16
    {1, 1, 32, H::Unsupported},      // k_10_11_11_AS_16_16_16_16
    {1, 1, 32, H::Unsupported},      // k_11_11_10_AS_16_16_16_16
    {1, 1, 96, H::Unsupported},      // k_32_32_32_FLOAT
    {4, 4, 4, H::R8, Convert::Dxt3aToR8},  // k_DXT3A
    {4, 4, 4, H::BC4},               // k_DXT5A
    {4, 4, 4, H::RG8, Convert::Ctx1ToRg8}, // k_CTX1
    {4, 4, 4, H::Unsupported},       // k_DXT3A_AS_1_1_1_1
    {1, 1, 32, H::Unsupported},      // k_8_8_8_8_GAMMA_EDRAM
    {1, 1, 32, H::Unsupported},      // k_2_10_10_10_FLOAT_EDRAM
};

constexpr uint32_t MAX_MIPS = 14;           // xenos::kTextureMaxMips
constexpr uint32_t TILE = 32;               // kTextureTileWidthHeight
constexpr uint32_t TILE_DEPTH = 4;          // kTextureTileDepth
constexpr uint32_t LINEAR_ROW_ALIGNMENT = 256;
constexpr uint32_t SUBRESOURCE_ALIGNMENT = 4096;

uint32_t log2_floor(uint32_t v) { return v ? 31 - uint32_t(std::countl_zero(v)) : 0; }
uint32_t log2_ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
uint32_t next_pow2(uint32_t v) { return v <= 1 ? 1 : 1u << log2_ceil(v); }
uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t packed_mip_level(uint32_t width, uint32_t height) {
    uint32_t log2_size = log2_ceil(std::min(width, height));
    return log2_size > 4 ? log2_size - 4 : 0;
}

template <typename Address> Address tiled_combine(Address bytes, uint32_t bank, uint32_t pipe, uint32_t y_lsb) {
    return Address((y_lsb << 4) | (pipe << 6) | (bank << 11)) | (bytes & 0xF) | (((bytes >> 4) & 1) << 5) |
           (((bytes >> 5) & 7) << 8) | (bytes >> 8 << 12);
}

// Xenia's TextureGuestLayout, without the extents (the loads here find what
// they read themselves).
struct LevelLayout {
    uint32_t row_pitch_bytes = 0, z_slice_stride_block_rows = 0, array_slice_stride_bytes = 0;
};
struct GuestLayout {
    uint32_t array_size = 0, max_level = 0, packed_level = UINT32_MAX;
    LevelLayout base;
    LevelLayout mips[MAX_MIPS];
    uint32_t mip_offsets_bytes[MAX_MIPS] = {};
};

GuestLayout guest_layout(uint32_t dimension, uint32_t base_pitch_div_32, uint32_t width, uint32_t height,
                         uint32_t depth_or_array_size, bool tiled, uint32_t format, bool packed_levels, bool has_base,
                         uint32_t max_level) {
    GuestLayout layout;
    const FormatEntry &f = FORMATS[format];
    uint32_t bytes_per_block = f.bits * f.block_width * f.block_height / 8;
    if (dimension == 0 && height <= 1) height = 1;
    uint32_t depth = dimension == 2 ? depth_or_array_size : 1;
    layout.array_size = dimension == 1 ? depth_or_array_size : dimension == 3 ? 6 : 1;
    if (!width || !height || !depth || !layout.array_size) return GuestLayout{};
    uint32_t mip_array_size =
        dimension == 1 && layout.array_size > 1 ? align(layout.array_size, TILE_DEPTH) : layout.array_size;

    max_level = std::min(max_level, log2_floor(std::max(std::max(width, height), depth)));
    layout.max_level = max_level;
    layout.packed_level = packed_levels ? packed_mip_level(width, height) : UINT32_MAX;
    uint32_t max_stored_level = std::min(max_level, layout.packed_level);

    uint32_t loop_level_last = layout.packed_level == 0 ? uint32_t(max_level != 0) : max_stored_level;
    uint32_t mip_offset_bytes = 0;
    for (uint32_t loop_level = has_base ? 0 : 1; loop_level <= loop_level_last; ++loop_level) {
        bool is_base = loop_level == 0;
        uint32_t level = layout.packed_level == 0 ? 0 : loop_level;
        LevelLayout &l = is_base ? layout.base : layout.mips[level];

        uint32_t row_pitch_texels, z_rows_texels;
        if (is_base) {
            row_pitch_texels = base_pitch_div_32 << 5;
            z_rows_texels = layout.packed_level == 0 ? next_pow2(height) : height;
        } else {
            row_pitch_texels = std::max(next_pow2(width) >> level, 1u);
            z_rows_texels = std::max(next_pow2(height) >> level, 1u);
        }
        uint32_t row_alignment = TILE;
        if (!tiled && !is_base) row_alignment = std::max(LINEAR_ROW_ALIGNMENT / bytes_per_block, TILE);
        uint32_t row_pitch_blocks = align(align(row_pitch_texels, f.block_width) / f.block_width, row_alignment);
        l.row_pitch_bytes = row_pitch_blocks * bytes_per_block;
        l.z_slice_stride_block_rows =
            dimension != 0 ? align(align(z_rows_texels, f.block_height) / f.block_height, TILE) : 1;
        l.array_slice_stride_bytes = l.row_pitch_bytes * l.z_slice_stride_block_rows;
        if (dimension == 2)
            l.array_slice_stride_bytes *= align(
                is_base && layout.packed_level != 0 ? depth : std::max(next_pow2(depth) >> level, 1u), TILE_DEPTH);
        l.array_slice_stride_bytes = align(l.array_slice_stride_bytes, SUBRESOURCE_ALIGNMENT);

        if (!is_base) {
            layout.mip_offsets_bytes[level] = mip_offset_bytes;
            mip_offset_bytes += l.array_slice_stride_bytes * mip_array_size;
        }
    }
    return layout;
}

// endian.xesli, on the little-endian words the GPU loads
uint32_t swap16(uint32_t v, uint32_t endian) {
    if (endian == 1) v = ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    return v;
}
uint32_t swap32(uint32_t v, uint32_t endian) {
    if (endian == 1 || endian == 2) v = ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    if (endian == 2 || endian == 3) v = (v << 16) | (v >> 16);
    return v;
}
uint32_t load_le32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void store_le32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t load_le16(const uint8_t *p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// A block as the load shaders swap it: none for 8 bits, 16-bit halves for 16
// bits, 32-bit words for the rest (the fetch constant's endian has no 8in64).
void swap_block(uint8_t *p, uint32_t bytes, uint32_t endian) {
    if (bytes == 1 || !endian) return;
    if (bytes == 2) {
        uint16_t v = load_le16(p);
        v = uint16_t(swap16(v, endian));
        std::memcpy(p, &v, 2);
        return;
    }
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) store_le32(p + i, swap32(load_le32(p + i), endian));
}

// pixel_formats.xesli's XeDXTHighColorWeights: the 2-bit codes of a DXT color
// block turned into the second endpoint's weight, 0-3.
uint32_t dxt_high_color_weights(uint32_t codes) {
    codes = ((codes & 0x55555555u) << 1) | ((codes & 0xAAAAAAAAu) >> 1);
    return codes ^ ((codes & 0xAAAAAAAAu) >> 1);
}

// texture_load_dxt3a's XeDXT3FourBlocksRowToA8, a texel at a time: the 4-bit
// alphas, texel i in bits 4i of the block's 64 bits.
void dxt3a_to_r8(const uint8_t *block, uint8_t out[16]) {
    uint32_t w[2] = {load_le32(block), load_le32(block + 4)};
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t a = (w[i >> 3] >> (4 * (i & 7))) & 0xF;
        out[i] = uint8_t(a | (a << 4));
    }
}

// texture_load_ctx1's XeCTX1TwoBlocksRowToR8G8: two 8:8 endpoints (red in the
// second byte of each), four steps between them.
void ctx1_to_rg8(const uint8_t *block, uint8_t out[32]) {
    uint32_t ends = load_le32(block), weights_high = dxt_high_color_weights(load_le32(block + 4));
    uint32_t r0 = (ends >> 8) & 0xFF, g0 = ends & 0xFF, r1 = ends >> 24, g1 = (ends >> 16) & 0xFF;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t high = (weights_high >> (2 * i)) & 3, low = 3 - high;
        out[2 * i] = uint8_t((low * r0 + high * r1) / 3);
        out[2 * i + 1] = uint8_t((low * g0 + high * g1) / 3);
    }
}

// Where a slice's blocks lie, from its base, without visiting them. Linear:
// from the first block to the end of the last. Tiled: the 4 KB pages of the
// tiles they are in. A tile (32x32 blocks, 32x16x4 in 3D) holds its blocks'
// bytes in the address bits from 12 up only through its index
// (tiled_combine moves the bits of the tiled byte offset from 8 up to 12
// up), so its bytes lie in the pages of [index * tile bytes, (index + 1) *
// tile bytes), and the indices of the blocks' tiles run from the first
// block's to the last's.
void slice_span(SliceLoad &s) {
    uint32_t x0 = s.ox, y0 = s.oy, x1 = s.ox + s.blocks_x - 1, y1 = s.oy + s.blocks_y - 1, z = s.z + s.oz;
    uint32_t log2 = s.log2_bytes_per_block;
    if (!s.tiled) {
        s.low = uint64_t(x0 + uint64_t(s.pitch_blocks) * (y0 + uint64_t(s.z_rows) * z)) << log2;
        s.high = (uint64_t(x1 + uint64_t(s.pitch_blocks) * (y1 + uint64_t(s.z_rows) * z)) << log2) + (1u << log2);
        return;
    }
    uint64_t first, last, tile_bytes;
    if (s.is_3d) {
        auto index = [&](uint32_t x, uint32_t y) {
            return ((uint64_t(z >> 2) * (s.z_rows >> 4) + (y >> 4)) * (s.pitch_blocks >> 5)) + (x >> 5);
        };
        first = index(x0, y0), last = index(x1, y1), tile_bytes = uint64_t(32 * 16 * 4) << log2;
    } else {
        first = uint64_t(y0 >> 5) * (s.pitch_blocks >> 5) + (x0 >> 5);
        last = uint64_t(y1 >> 5) * (s.pitch_blocks >> 5) + (x1 >> 5);
        tile_bytes = uint64_t(32 * 32) << log2;
    }
    s.low = first * tile_bytes & ~uint64_t(4095);
    s.high = ((last + 1) * tile_bytes + 4095) & ~uint64_t(4095);
}

// Reads the blocks of one slice: the span of guest memory they lie in read at
// once, then each block copied out (a linear slice's rows whole) and
// byte-swapped.
void read_blocks(const SliceLoad &load, const GuestMemory &memory, uint32_t endian, std::vector<uint8_t> &out) {
    uint32_t bytes_per_block = 1u << load.log2_bytes_per_block;
    size_t start = out.size();
    out.resize(start + size_t(load.blocks_x) * load.blocks_y * bytes_per_block);
    uint8_t *dst = out.data() + start;
    uint64_t span = load.high - load.low;
    if (span <= (64u << 20)) {
        std::vector<uint8_t> src(span);
        memory.read(uint32_t(load.base + load.low), src);
        if (!load.tiled) {
            size_t row = size_t(load.blocks_x) * bytes_per_block;
            for (uint32_t y = 0; y < load.blocks_y; y++, dst += row)
                std::memcpy(dst, src.data() + (load.offset(0, y) - load.low), row);
        } else {
            for (uint32_t y = 0; y < load.blocks_y; y++)
                for (uint32_t x = 0; x < load.blocks_x; x++, dst += bytes_per_block)
                    std::memcpy(dst, src.data() + (load.offset(x, y) - load.low), bytes_per_block);
        }
    } else {
        for (uint32_t y = 0; y < load.blocks_y; y++)
            for (uint32_t x = 0; x < load.blocks_x; x++, dst += bytes_per_block)
                memory.read(uint32_t(load.base + load.offset(x, y)), {dst, bytes_per_block});
    }
    if (!endian || bytes_per_block == 1) return; // nothing to swap
    dst = out.data() + start;
    for (size_t i = 0; i < size_t(load.blocks_x) * load.blocks_y; i++, dst += bytes_per_block)
        swap_block(dst, bytes_per_block, endian);
}

// pixel_formats.xesli's XeFloat20e4To32: 20e4 depth (0-2) to a float
float float20e4_to_float(uint32_t f24) {
    f24 &= 0xFFFFFF;
    if (!f24) return 0.0f;
    uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
    if (!exponent) {
        uint32_t lzcnt = 20 - log2_floor(mantissa);
        exponent = 1 - lzcnt;
        mantissa = (mantissa << lzcnt) & 0xFFFFF;
    }
    return std::bit_cast<float>(((exponent + 112) << 23) | (mantissa << 3));
}

// half precision to single
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

uint8_t unorm8(float v) {
    if (!(v > 0)) return 0; // NaN too
    if (v >= 1) return 255;
    return uint8_t(v * 255 + 0.5f);
}
uint8_t expand(uint32_t v, uint32_t bits) { return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

// pixel_formats.xesli's DXT color decoding: 5:6:5 endpoints widened by bit
// replication, then 2:1 and 1:2 steps (integer division by 3), or in DXT1's
// punchthrough mode (first endpoint not above the second) half way and
// transparent black.
void dxt_color(const uint8_t *block, bool dxt1, uint8_t out[64]) {
    uint32_t ends = load_le32(block), codes = load_le32(block + 4);
    uint32_t c[2][3];
    for (int e = 0; e < 2; e++) {
        uint32_t v = (ends >> (16 * e)) & 0xFFFF;
        uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        c[e][0] = (r << 3) | (r >> 2), c[e][1] = (g << 2) | (g >> 4), c[e][2] = (b << 3) | (b >> 2);
    }
    bool opaque = !dxt1 || (ends & 0xFFFF) > (ends >> 16);
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t code = (codes >> (2 * i)) & 3;
        uint8_t *o = out + 4 * i;
        o[3] = 255;
        for (int k = 0; k < 3; k++) {
            if (opaque) {
                static const uint32_t w0[4] = {3, 0, 2, 1}; // the first endpoint's weight per code
                o[k] = uint8_t((w0[code] * c[0][k] + (3 - w0[code]) * c[1][k]) / 3);
            } else {
                o[k] = uint8_t(code == 0 ? c[0][k] : code == 1 ? c[1][k] : code == 2 ? (c[0][k] + c[1][k]) >> 1 : 0);
            }
        }
        if (!opaque && code == 3) o[3] = 0;
    }
}

// pixel_formats.xesli's DXT5 alpha (and DXT5A, DXN) decoding: 8 steps (integer
// division by 7) when the first endpoint is above the second, else 6 (by 5)
// with 0 and 255.
void dxt5_alpha(const uint8_t *block, uint8_t out[16]) {
    uint32_t a0 = block[0], a1 = block[1];
    uint64_t codes = 0;
    for (int i = 0; i < 6; i++) codes |= uint64_t(block[2 + i]) << (8 * i);
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t k = uint32_t(codes >> (3 * i)) & 7;
        uint32_t v;
        if (k == 0) v = a0;
        else if (k == 1) v = a1;
        else if (a0 > a1) v = (a0 * (8 - k) + a1 * (k - 1)) / 7;
        else if (k < 6) v = (a0 * (6 - k) + a1 * (k - 1)) / 5;
        else v = k == 6 ? 0 : 255;
        out[i] = uint8_t(v);
    }
}

} // namespace

uint32_t bytes_per_block(HostFormat format) {
    switch (format) {
    case H::R8: return 1;
    case H::RG8: case H::B5G6R5: case H::A1R5G5B5: case H::A4R4G4B4: case H::R16: case H::R16F: return 2;
    case H::RGBA8: case H::RGB10A2: case H::RG16: case H::RG16F: case H::R32: case H::R32F: return 4;
    case H::RGBA16: case H::RGBA16F: case H::RG32: case H::RG32F: case H::BC1: case H::BC4: return 8;
    case H::RGBA32: case H::RGBA32F: case H::BC2: case H::BC3: case H::BC5: return 16;
    case H::Unsupported: break;
    }
    return 0;
}

bool is_compressed(HostFormat format) {
    return format == H::BC1 || format == H::BC2 || format == H::BC3 || format == H::BC4 || format == H::BC5;
}

Block format_block(uint32_t format) {
    if (format >= 64) return {};
    const FormatEntry &f = FORMATS[format];
    return {f.block_width, f.block_height, uint32_t(f.bits) * f.block_width * f.block_height / 8};
}

uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
    uint32_t outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t bytes = (outer | inner) << bytes_per_block_log2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return tiled_combine(bytes, bank, pipe, y & 1);
}

uint64_t tiled_offset_3d(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height,
                         uint32_t bytes_per_block_log2) {
    uint32_t outer = ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5)) + (x >> 5)) << 7;
    uint32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    uint64_t bytes = uint64_t(outer | inner) << bytes_per_block_log2;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1, pipe = ((x >> 3) & 3) ^ (bank << 1);
    return tiled_combine(bytes, bank, pipe, y & 1);
}

bool packed_mip_offset(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t level,
                       uint32_t &x, uint32_t &y, uint32_t &z) {
    // A 32x32 tile holds the levels of 16 and less: 16x16 at x 16, 8x8 at x
    // 8, 4x4 at x 4, then down the left (or across the top when wide).
    uint32_t log2_width = log2_ceil(width), log2_height = log2_ceil(height);
    uint32_t log2_size = std::min(log2_width, log2_height);
    if (log2_size > 4 + level) {
        x = y = z = 0;
        return false;
    }
    uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
    uint32_t packed_mip = level - packed_mip_base;
    bool is_wide = log2_width > std::max(log2_height, 1u);
    if (packed_mip < 3) {
        if (is_wide) x = 0, y = 16 >> packed_mip;
        else x = 16 >> packed_mip, y = 0;
        z = 0;
    } else {
        uint32_t offset;
        if (is_wide) {
            offset = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
            x = offset, y = 0;
        } else {
            offset = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
            x = 0, y = offset;
        }
        if (offset < 4) {
            uint32_t log2_depth = log2_ceil(depth);
            z = log2_depth > 1 + level ? (log2_depth - level) * 4 : 4;
        } else {
            z = 0;
        }
    }
    const FormatEntry &f = FORMATS[format & 63];
    x >>= log2_floor(f.block_width);
    y >>= log2_floor(f.block_height);
    return true;
}

Info parse_fetch_constant(const uint32_t d[6]) {
    Info info;
    for (int i = 0; i < 4; i++) info.signs[i] = (d[0] >> (2 + 2 * i)) & 3;
    for (int i = 0; i < 3; i++) info.clamp[i] = (d[0] >> (10 + 3 * i)) & 7;
    info.pitch = ((d[0] >> 22) & 0x1FF) << 5;
    info.tiled = (d[0] >> 31) & 1;
    info.format = d[1] & 0x3F;
    info.endian = (d[1] >> 6) & 3;
    bool stacked = (d[1] >> 10) & 1;
    info.base_address = d[1] & 0xFFFFF000;
    info.dimension = (d[5] >> 9) & 3;
    switch (info.dimension) {
    case 0: info.width = (d[2] & 0xFFFFFF) + 1; break;
    case 1:
        info.width = (d[2] & 0x1FFF) + 1, info.height = ((d[2] >> 13) & 0x1FFF) + 1;
        info.depth = stacked ? ((d[2] >> 26) & 0x3F) + 1 : 1;
        break;
    case 2:
        info.width = (d[2] & 0x7FF) + 1, info.height = ((d[2] >> 11) & 0x7FF) + 1;
        info.depth = ((d[2] >> 22) & 0x3FF) + 1;
        break;
    case 3:
        info.width = (d[2] & 0x1FFF) + 1, info.height = ((d[2] >> 13) & 0x1FFF) + 1;
        info.depth = 6;
        break;
    }
    info.num_format = d[3] & 1;
    info.swizzle = (d[3] >> 1) & 0xFFF;
    info.exp_adjust = int32_t(d[3] << (32 - 19)) >> (32 - 6);
    info.mag_filter = (d[3] >> 19) & 3, info.min_filter = (d[3] >> 21) & 3, info.mip_filter = (d[3] >> 23) & 3;
    info.aniso_filter = (d[3] >> 25) & 7;
    info.mip_min = (d[4] >> 2) & 0xF;
    info.mip_max = (d[4] >> 6) & 0xF;
    info.lod_bias = int32_t(d[4] << (32 - 22)) >> (32 - 10);
    info.border_color = d[5] & 3;
    info.packed_mips = (d[5] >> 11) & 1;
    info.mip_address = d[5] & 0xFFFFF000;
    info.gamma = info.signs[0] == 3 || info.signs[1] == 3 || info.signs[2] == 3;
    info.host = FORMATS[info.format].host;
    return info;
}

Levels levels(const Info &info) {
    uint32_t width = info.width, height = info.dimension == 0 ? 1 : info.height;
    uint32_t longest = std::max(width, height);
    if (info.dimension == 2) longest = std::max(longest, info.depth);
    uint32_t size_mip_max_level = log2_floor(longest);

    uint32_t base_page = (info.base_address >> 12) & 0x1FFFF, mip_page = (info.mip_address >> 12) & 0x1FFFF;
    // When level 0 is in the packed tail, the whole tail is at the base.
    if (base_page && info.packed_mips && info.dimension != 0 && packed_mip_level(width, height) == 0)
        mip_page = base_page;

    uint32_t mip_min = 0, mip_max = 0;
    if (mip_page) {
        mip_min = std::min(info.mip_min, size_mip_max_level);
        mip_max = std::max(std::min(info.mip_max, size_mip_max_level), mip_min);
    }
    if (mip_max) {
        if (mip_min && base_page == mip_page) base_page = 0;
        if (!base_page) mip_min = std::max(mip_min, 1u);
    } else {
        mip_page = 0;
    }
    Levels l;
    l.base = base_page != 0, l.mips = mip_page != 0;
    l.base_address = base_page << 12, l.mip_address = mip_page << 12;
    l.mip_min = mip_min, l.mip_max = mip_max;
    return l;
}

uint64_t SliceLoad::offset(uint32_t x, uint32_t y) const {
    x += ox, y += oy;
    uint32_t zz = z + oz;
    if (!tiled) return uint64_t(x + uint64_t(pitch_blocks) * (y + uint64_t(z_rows) * zz)) << log2_bytes_per_block;
    if (is_3d) return tiled_offset_3d(x, y, zz, pitch_blocks, z_rows, log2_bytes_per_block);
    return tiled_offset_2d(x, y, pitch_blocks, log2_bytes_per_block);
}

bool plan(const Info &info, Plan &out, std::string &unsupported) {
    out = Plan{};
    const FormatEntry &f = FORMATS[info.format & 63];
    if (f.host == H::Unsupported) {
        unsupported = std::string("format ") + xenos::format_name(info.format);
        return false;
    }
    if (info.dimension == 0 && info.tiled) {
        unsupported = "tiled 1D";
        return false;
    }
    Levels lv = levels(info);
    if (!lv.base && !lv.mips) {
        unsupported = "no base or mip address";
        return false;
    }
    uint32_t width = info.width, height = info.dimension == 0 ? 1 : info.height;
    uint32_t depth = info.dimension == 2 ? info.depth : 1; // the 3D depth
    GuestLayout layout = guest_layout(info.dimension, info.pitch >> 5, width, height, info.depth, info.tiled,
                                      info.format, info.packed_mips, lv.base, lv.mips ? lv.mip_max : 0);
    if (!layout.array_size) {
        unsupported = "empty texture";
        return false;
    }
    uint32_t bpb = uint32_t(f.bits) * f.block_width * f.block_height / 8;

    out.format = f.host;
    out.load = f.convert;
    out.width = width, out.height = height, out.depth = info.depth;
    out.bytes_per_block = bpb, out.block_width = f.block_width, out.block_height = f.block_height;
    out.endian = info.endian;
    uint32_t level_last = lv.mips ? layout.max_level : 0;
    out.level_count = level_last + 1;
    for (uint32_t level = lv.base ? 0 : 1; level <= level_last; level++) {
        bool is_base = level == 0;
        uint32_t stored = std::min(level, layout.packed_level);
        const LevelLayout &l = is_base ? layout.base : layout.mips[stored];
        SliceLoad load;
        load.level = level;
        load.tiled = info.tiled, load.is_3d = info.dimension == 2;
        load.log2_bytes_per_block = log2_floor(bpb);
        load.pitch_blocks = l.row_pitch_bytes / bpb;
        load.z_rows = l.z_slice_stride_block_rows;
        if (level >= layout.packed_level)
            packed_mip_offset(width, height, depth, info.format, level, load.ox, load.oy, load.oz);
        uint32_t level_base = (is_base ? lv.base_address : lv.mip_address) +
                              (is_base ? 0 : layout.mip_offsets_bytes[stored]);

        uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
        uint32_t ld = std::max(depth >> level, 1u);
        load.blocks_x = (lw + f.block_width - 1) / f.block_width;
        load.blocks_y = (lh + f.block_height - 1) / f.block_height;
        uint32_t slices = info.dimension == 2 ? ld : layout.array_size;
        for (uint32_t s = 0; s < slices; s++) {
            bool layer = info.dimension != 2;
            load.base = level_base + (layer ? s * l.array_slice_stride_bytes : 0);
            load.z = layer ? 0 : s;
            slice_span(load);
            out.slices.push_back(load);
            out.footprint.push_back({uint32_t(load.base + load.low) & GuestMemory::PHYSICAL_MASK,
                                     uint32_t(std::min<uint64_t>(load.high - load.low, GuestMemory::SIZE))});
        }
    }
    return true;
}

bool decode(const Info &info, const GuestMemory &memory, Image &out, std::string &unsupported) {
    out = Image{};
    Plan p;
    if (!plan(info, p, unsupported)) return false;
    uint32_t bpb = p.bytes_per_block;
    out.format = p.format;
    out.width = p.width, out.height = p.height, out.depth = p.depth;
    out.levels.resize(p.level_count);
    out.footprint = std::move(p.footprint);
    std::vector<uint8_t> blocks;
    for (size_t first = 0; first < p.slices.size();) {
        // a level's slices, read in turn
        uint32_t level = p.slices[first].level;
        size_t end = first;
        blocks.clear();
        for (; end < p.slices.size() && p.slices[end].level == level; end++)
            read_blocks(p.slices[end], memory, p.endian, blocks);
        uint32_t slices = uint32_t(end - first);
        uint32_t blocks_x = p.slices[first].blocks_x, blocks_y = p.slices[first].blocks_y;
        first = end;

        std::vector<uint8_t> &dst = out.levels[level];
        if (p.load == Convert::Copy) {
            dst = std::move(blocks);
            blocks = {};
            continue;
        }
        if (p.load == Convert::DepthUnormToFloat || p.load == Convert::DepthFloatToFloat) {
            // the depth in the high 24 bits, to a float: unorm24 as
            // XeUNorm24To32 (exact), float24 as XeFloat20e4To32
            for (size_t i = 0; i + 4 <= blocks.size(); i += 4) {
                uint32_t d24 = load_le32(blocks.data() + i) >> 8;
                float value = p.load == Convert::DepthUnormToFloat ? float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f)
                                                                   : float20e4_to_float(d24);
                store_le32(blocks.data() + i, std::bit_cast<uint32_t>(value));
            }
            dst = std::move(blocks);
            blocks = {};
            continue;
        }
        // decompressed to texels, as Xenia's load shaders for these do
        uint32_t lw = std::max(p.width >> level, 1u), lh = std::max(p.height >> level, 1u);
        uint32_t texel_bytes = p.load == Convert::Ctx1ToRg8 ? 2 : 1;
        dst.assign(size_t(lw) * lh * slices * texel_bytes, 0);
        for (uint32_t s = 0; s < slices; s++)
            for (uint32_t by = 0; by < blocks_y; by++)
                for (uint32_t bx = 0; bx < blocks_x; bx++) {
                    const uint8_t *block = blocks.data() + ((size_t(s) * blocks_y + by) * blocks_x + bx) * bpb;
                    uint8_t texels[32];
                    if (p.load == Convert::Dxt3aToR8) dxt3a_to_r8(block, texels);
                    else ctx1_to_rg8(block, texels);
                    for (uint32_t ty = 0; ty < 4 && by * 4 + ty < lh; ty++)
                        for (uint32_t tx = 0; tx < 4 && bx * 4 + tx < lw; tx++)
                            std::memcpy(dst.data() + ((size_t(s) * lh + by * 4 + ty) * lw + bx * 4 + tx) * texel_bytes,
                                        texels + (ty * 4 + tx) * texel_bytes, texel_bytes);
                }
    }
    return true;
}

std::vector<uint8_t> to_rgba8(const Image &image, uint32_t level) {
    if (level >= image.levels.size() || image.levels[level].empty()) return {};
    const std::vector<uint8_t> &data = image.levels[level];
    uint32_t w = std::max(image.width >> level, 1u), h = std::max(image.height >> level, 1u);
    std::vector<uint8_t> out(size_t(w) * h * 4);
    H format = image.format;

    if (is_compressed(format)) {
        uint32_t bx_count = (w + 3) / 4, by_count = (h + 3) / 4, bpb = bytes_per_block(format);
        if (data.size() < size_t(bx_count) * by_count * bpb) return {};
        for (uint32_t by = 0; by < by_count; by++)
            for (uint32_t bx = 0; bx < bx_count; bx++) {
                const uint8_t *block = data.data() + (size_t(by) * bx_count + bx) * bpb;
                uint8_t texels[64];
                switch (format) {
                case H::BC1: dxt_color(block, true, texels); break;
                case H::BC2:
                    dxt_color(block + 8, false, texels);
                    for (uint32_t i = 0; i < 16; i++) {
                        uint32_t a = (load_le32(block + 4 * (i >> 3)) >> (4 * (i & 7))) & 0xF;
                        texels[4 * i + 3] = uint8_t(a | (a << 4));
                    }
                    break;
                case H::BC3: {
                    dxt_color(block + 8, false, texels);
                    uint8_t a[16];
                    dxt5_alpha(block, a);
                    for (uint32_t i = 0; i < 16; i++) texels[4 * i + 3] = a[i];
                    break;
                }
                default: { // BC4, BC5: red, then green
                    uint8_t r[16], g[16] = {};
                    dxt5_alpha(block, r);
                    if (format == H::BC5) dxt5_alpha(block + 8, g);
                    for (uint32_t i = 0; i < 16; i++)
                        texels[4 * i] = r[i], texels[4 * i + 1] = g[i], texels[4 * i + 2] = 0, texels[4 * i + 3] = 255;
                    break;
                }
                }
                for (uint32_t ty = 0; ty < 4 && by * 4 + ty < h; ty++)
                    for (uint32_t tx = 0; tx < 4 && bx * 4 + tx < w; tx++)
                        std::memcpy(out.data() + ((size_t(by) * 4 + ty) * w + bx * 4 + tx) * 4,
                                    texels + (ty * 4 + tx) * 4, 4);
            }
        return out;
    }

    uint32_t bpt = bytes_per_block(format);
    if (!bpt || data.size() < size_t(w) * h * bpt) return {};
    for (size_t i = 0; i < size_t(w) * h; i++) {
        const uint8_t *p = data.data() + i * bpt;
        uint8_t *o = out.data() + i * 4;
        uint8_t r = 0, g = 0, b = 0, a = 255;
        switch (format) {
        case H::R8: r = p[0]; break;
        case H::RG8: r = p[0], g = p[1]; break;
        case H::RGBA8: r = p[0], g = p[1], b = p[2], a = p[3]; break;
        case H::B5G6R5: {
            uint32_t v = load_le16(p);
            r = expand(v & 31, 5), g = expand((v >> 5) & 63, 6), b = expand(v >> 11, 5);
            break;
        }
        case H::A1R5G5B5: {
            uint32_t v = load_le16(p);
            r = expand(v & 31, 5), g = expand((v >> 5) & 31, 5), b = expand((v >> 10) & 31, 5);
            a = (v >> 15) ? 255 : 0;
            break;
        }
        case H::A4R4G4B4: {
            uint32_t v = load_le16(p);
            r = expand(v & 15, 4), g = expand((v >> 4) & 15, 4), b = expand((v >> 8) & 15, 4);
            a = expand(v >> 12, 4);
            break;
        }
        case H::RGB10A2: {
            uint32_t v = load_le32(p);
            r = expand(v & 1023, 10), g = expand((v >> 10) & 1023, 10), b = expand((v >> 20) & 1023, 10);
            a = expand(v >> 30, 2);
            break;
        }
        case H::R16: case H::RG16: case H::RGBA16: {
            uint32_t n = format == H::R16 ? 1 : format == H::RG16 ? 2 : 4;
            uint8_t c[4] = {0, 0, 0, 255};
            for (uint32_t k = 0; k < n; k++) c[k] = uint8_t(load_le16(p + 2 * k) >> 8);
            r = c[0], g = c[1], b = c[2], a = c[3];
            break;
        }
        case H::R16F: case H::RG16F: case H::RGBA16F: {
            uint32_t n = format == H::R16F ? 1 : format == H::RG16F ? 2 : 4;
            uint8_t c[4] = {0, 0, 0, 255};
            for (uint32_t k = 0; k < n; k++) c[k] = unorm8(half_to_float(load_le16(p + 2 * k)));
            r = c[0], g = c[1], b = c[2], a = c[3];
            break;
        }
        case H::R32: case H::RG32: case H::RGBA32: {
            uint32_t n = format == H::R32 ? 1 : format == H::RG32 ? 2 : 4;
            uint8_t c[4] = {0, 0, 0, 255};
            for (uint32_t k = 0; k < n; k++) c[k] = uint8_t(load_le32(p + 4 * k) >> 24);
            r = c[0], g = c[1], b = c[2], a = c[3];
            break;
        }
        case H::R32F: case H::RG32F: case H::RGBA32F: {
            uint32_t n = format == H::R32F ? 1 : format == H::RG32F ? 2 : 4;
            uint8_t c[4] = {0, 0, 0, 255};
            for (uint32_t k = 0; k < n; k++) c[k] = unorm8(std::bit_cast<float>(load_le32(p + 4 * k)));
            r = c[0], g = c[1], b = c[2], a = c[3];
            break;
        }
        default: break;
        }
        o[0] = r, o[1] = g, o[2] = b, o[3] = a;
    }
    return out;
}

} // namespace xf::texture
