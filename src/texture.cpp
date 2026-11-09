// Guest textures read out of guest memory (texture.h). Translated from
// Xenia: the fetch constant (src/xenia/gpu/xenos.h, xe_gpu_texture_fetch_t),
// the formats' blocks (texture_info_formats.inl), the tiled addressing
// (texture_address.h), the levels and where they lie (texture_util.cc:
// GetSubresourcesFromFetchConstant, GetPackedMipLevel, GetPackedMipOffset,
// GetGuestTextureLayout), the loads' addressing, levels and byte swaps
// (vulkan/vulkan_texture_cache.cc's LoadTextureData, shaders/texture_load.xesli
// and texture_load_*bpb.xesli, endian.xesli), the host formats, their
// swizzles, signed versions and fallbacks (d3d12/d3d12_texture_cache.h's and
// vulkan_texture_cache.cc's tables and Initialize, texture_info.h's
// GetBaseFormat), the 1D textures it refuses (texture_cache.cc's
// BindingInfoFromFetchConstant), the conversions (pixel_formats.xesli's
// 16-bit rearrangements, R11G11B10 and R10G11B11 widenings, GBGR8 and BGRG8
// decompressions, the DXT, DXT3A, DXT3A_AS_1_1_1_1 and CTX1 decoders) and the
// depth loads (texture_load_depth_unorm, texture_load_depth_float). BSD; see
// LICENSE-xenia.

#include "xenoforge/texture.h"

#include "xenoforge/xenos.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace xf::texture {

namespace {

// A guest format's block and how Xenia's texture caches hold it: the host
// format and the load that makes it, the host swizzle (the last component a
// format has stands for those it lacks, "on the Xenos, it appears that the
// last existing component of a texture is replicated into the missing
// components"), and its signed version. Where the two backends differ the
// D3D12 one is followed (24_8's signed version is its R32_FLOAT view), except
// where DXGI lacks a format Vulkan has (2_10_10_10's signed view). The
// formats GetBaseFormat folds (the 8_A, _AS_16_16_16_16 and 8_8_8_8_GAMMA_EDRAM
// ones) are their base format's rows. Unsupported, as in Xenia: the 1-bit,
// 32_AS_8, MPEG, interlaced and 96-bit formats, the 32-bit integer ones (no
// host format filters them), and the EDRAM ones other than 8_8_8_8_GAMMA
// ("not usable as a texture").
struct FormatEntry {
    uint8_t block_width, block_height;
    uint8_t bits; // per texel (texture_info_formats.inl's bits_per_pixel)
    HostFormat host = HostFormat::Unsupported;
    Load load = Load::Copy;
    uint32_t swizzle = SWIZZLE_RGBA;
    Signed signs = Signed::None;
    Load signed_load = Load::Copy; // a separate signed version's
};

using H = HostFormat;
using L = Load;
using S = Signed;
constexpr FormatEntry FORMATS[64] = {
    {1, 1, 1},                                                         // k_1_REVERSE
    {1, 1, 1},                                                         // k_1
    {1, 1, 8, H::R8, L::Copy, SWIZZLE_RRRR, S::View},                  // k_8
    {1, 1, 16, H::A1R5G5B5, L::R5G5B5A1ToB5G5R5A1},                    // k_1_5_5_5
    {1, 1, 16, H::R5G6B5, L::R5G6B5ToB5G6R5, SWIZZLE_RGBB},            // k_5_6_5
    {1, 1, 16, H::R5G6B5, L::R5G5B6ToB5G6R5, SWIZZLE_RBGG},            // k_6_5_5: green in blue, blue in green
    {1, 1, 32, H::RGBA8, L::Copy, SWIZZLE_RGBA, S::View},              // k_8_8_8_8
    {1, 1, 32, H::RGB10A2, L::Copy, SWIZZLE_RGBA, S::View},            // k_2_10_10_10
    {1, 1, 8, H::R8, L::Copy, SWIZZLE_RRRR, S::View},                  // k_8_A
    {1, 1, 8, H::R8, L::Copy, SWIZZLE_RRRR, S::View},                  // k_8_B
    {1, 1, 16, H::RG8, L::Copy, SWIZZLE_RGGG, S::View},                // k_8_8
    {2, 1, 16, H::RGBA8, L::GBGR8ToRGBA8, SWIZZLE_RGBB, S::View},      // k_Cr_Y1_Cb_Y0_REP
    {2, 1, 16, H::RGBA8, L::BGRG8ToRGBA8, SWIZZLE_RGBB, S::View},      // k_Y1_Cr_Y0_Cb_REP
    {1, 1, 32},                                                        // k_16_16_EDRAM
    {1, 1, 32, H::RGBA8, L::Copy, SWIZZLE_RGBA, S::View},              // k_8_8_8_8_A
    {1, 1, 16, H::B4G4R4A4, L::R4G4B4A4ToA4R4G4B4},                    // k_4_4_4_4
    {1, 1, 32, H::RGBA16, L::R11G11B10ToRGBA16, SWIZZLE_RGBB, S::Separate, L::R11G11B10ToRGBA16Snorm}, // k_10_11_11
    {1, 1, 32, H::RGBA16, L::R10G11B11ToRGBA16, SWIZZLE_RGBB, S::Separate, L::R10G11B11ToRGBA16Snorm}, // k_11_11_10
    {4, 4, 4, H::BC1},                                                 // k_DXT1
    {4, 4, 8, H::BC2},                                                 // k_DXT2_3
    {4, 4, 8, H::BC3},                                                 // k_DXT4_5
    {1, 1, 64},                                                        // k_16_16_16_16_EDRAM
    {1, 1, 32, H::R32F, L::DepthUnormToFloat, SWIZZLE_RRRR, S::View},  // k_24_8
    {1, 1, 32, H::R32F, L::DepthFloatToFloat, SWIZZLE_RRRR, S::View},  // k_24_8_FLOAT
    {1, 1, 16, H::R16, L::Copy, SWIZZLE_RRRR, S::View},                // k_16
    {1, 1, 32, H::RG16, L::Copy, SWIZZLE_RGGG, S::View},               // k_16_16
    {1, 1, 64, H::RGBA16, L::Copy, SWIZZLE_RGBA, S::View},             // k_16_16_16_16
    {1, 1, 16, H::R16F, L::Copy, SWIZZLE_RRRR, S::View},               // k_16_EXPAND: floats, as Xenia reads it
    {1, 1, 32, H::RG16F, L::Copy, SWIZZLE_RGGG, S::View},              // k_16_16_EXPAND
    {1, 1, 64, H::RGBA16F, L::Copy, SWIZZLE_RGBA, S::View},            // k_16_16_16_16_EXPAND
    {1, 1, 16, H::R16F, L::Copy, SWIZZLE_RRRR, S::View},               // k_16_FLOAT
    {1, 1, 32, H::RG16F, L::Copy, SWIZZLE_RGGG, S::View},              // k_16_16_FLOAT
    {1, 1, 64, H::RGBA16F, L::Copy, SWIZZLE_RGBA, S::View},            // k_16_16_16_16_FLOAT
    {1, 1, 32},                                                        // k_32
    {1, 1, 64},                                                        // k_32_32
    {1, 1, 128},                                                       // k_32_32_32_32
    {1, 1, 32, H::R32F, L::Copy, SWIZZLE_RRRR, S::View},               // k_32_FLOAT
    {1, 1, 64, H::RG32F, L::Copy, SWIZZLE_RGGG, S::View},              // k_32_32_FLOAT
    {1, 1, 128, H::RGBA32F, L::Copy, SWIZZLE_RGBA, S::View},           // k_32_32_32_32_FLOAT
    {4, 1, 8},                                                         // k_32_AS_8
    {2, 1, 16},                                                        // k_32_AS_8_8
    {1, 1, 16},                                                        // k_16_MPEG
    {1, 1, 32},                                                        // k_16_16_MPEG
    {1, 1, 8},                                                         // k_8_INTERLACED
    {4, 1, 8},                                                         // k_32_AS_8_INTERLACED
    {1, 1, 16},                                                        // k_32_AS_8_8_INTERLACED
    {1, 1, 16},                                                        // k_16_INTERLACED
    {1, 1, 16},                                                        // k_16_MPEG_INTERLACED
    {1, 1, 32},                                                        // k_16_16_MPEG_INTERLACED
    {4, 4, 8, H::BC5, L::Copy, SWIZZLE_RGGG, S::View},                 // k_DXN (BC5_SNORM: beyond Xenia)
    {1, 1, 32, H::RGBA8, L::Copy, SWIZZLE_RGBA, S::View},              // k_8_8_8_8_AS_16_16_16_16
    {4, 4, 4, H::BC1},                                                 // k_DXT1_AS_16_16_16_16
    {4, 4, 8, H::BC2},                                                 // k_DXT2_3_AS_16_16_16_16
    {4, 4, 8, H::BC3},                                                 // k_DXT4_5_AS_16_16_16_16
    {1, 1, 32, H::RGB10A2, L::Copy, SWIZZLE_RGBA, S::View},            // k_2_10_10_10_AS_16_16_16_16
    // k_10_11_11_AS_16_16_16_16
    {1, 1, 32, H::RGBA16, L::R11G11B10ToRGBA16, SWIZZLE_RGBB, S::Separate, L::R11G11B10ToRGBA16Snorm},
    // k_11_11_10_AS_16_16_16_16
    {1, 1, 32, H::RGBA16, L::R10G11B11ToRGBA16, SWIZZLE_RGBB, S::Separate, L::R10G11B11ToRGBA16Snorm},
    {1, 1, 96},                                                        // k_32_32_32_FLOAT
    {4, 4, 4, H::R8, L::Dxt3aToR8, SWIZZLE_RRRR},                      // k_DXT3A
    {4, 4, 4, H::BC4, L::Copy, SWIZZLE_RRRR, S::View},                 // k_DXT5A (BC4_SNORM: beyond Xenia)
    {4, 4, 4, H::RG8, L::Ctx1ToRg8, SWIZZLE_RGGG},                     // k_CTX1
    {4, 4, 4, H::B4G4R4A4, L::Dxt3aAs1111ToARGB4},                     // k_DXT3A_AS_1_1_1_1
    {1, 1, 32, H::RGBA8, L::Copy, SWIZZLE_RGBA, S::View},              // k_8_8_8_8_GAMMA_EDRAM: as 8_8_8_8
    {1, 1, 32},                                                        // k_2_10_10_10_FLOAT_EDRAM
};

bool is_float(HostFormat f) {
    return f == H::R16F || f == H::RG16F || f == H::RGBA16F || f == H::R32F || f == H::RG32F || f == H::RGBA32F;
}
bool is_norm16(HostFormat f) { return f == H::R16 || f == H::RG16 || f == H::RGBA16; }
HostFormat half_of(HostFormat f) { return f == H::R16 ? H::R16F : f == H::RG16 ? H::RG16F : H::RGBA16F; }

// A version of a texture as the host holds it.
struct Version {
    HostFormat host = H::Unsupported;
    Load load = L::Copy;
    Half half = Half::None;
    uint32_t swizzle = SWIZZLE_RGBA;
    bool snorm = false;
    std::string approximate;
};

// The unsigned version: the format's host format where the host samples and
// filters it, else the fallbacks of Xenia's Vulkan texture cache (Initialize:
// the DXT formats decompressed, 16-bit fixed point as 16-bit floats); the
// packed 16-bit formats, which Vulkan requires, unpacked to RGBA8 (beyond
// Xenia, which has no fallback for them).
Version unsigned_version(const FormatEntry &f, const Support &support) {
    Version v;
    v.host = f.host, v.load = f.load, v.swizzle = f.swizzle;
    if (support.usable(v.host)) return v;
    switch (v.host) {
    case H::BC1: v.host = H::RGBA8, v.load = L::Dxt1ToRGBA8; break;
    case H::BC2: v.host = H::RGBA8, v.load = L::Dxt3ToRGBA8; break;
    case H::BC3: v.host = H::RGBA8, v.load = L::Dxt5ToRGBA8; break;
    case H::BC4: v.host = H::R8, v.load = L::Dxt5aToR8; break;
    case H::BC5: v.host = H::RG8, v.load = L::DxnToRG8; break;
    case H::R16: case H::RG16: case H::RGBA16:
        v.approximate = std::string(host_format_name(v.host)) + " as 16-bit floats";
        v.host = half_of(v.host), v.half = Half::FromUnorm;
        break;
    case H::R5G6B5: case H::A1R5G5B5: case H::B4G4R4A4:
        v.approximate = std::string(host_format_name(v.host)) + " as RGBA8";
        v.load = v.load == L::R5G6B5ToB5G6R5       ? L::R5G6B5ToRGBA8
                 : v.load == L::R5G5B6ToB5G6R5     ? L::R5G5B6ToRGBA8
                 : v.load == L::R5G5B5A1ToB5G5R5A1 ? L::R5G5B5A1ToRGBA8
                 : v.load == L::R4G4B4A4ToA4R4G4B4 ? L::R4G4B4A4ToRGBA8
                                                   : L::Dxt3aAs1111ToRGBA8;
        if (v.load == L::Dxt3aAs1111ToRGBA8) v.approximate.clear(); // its bits are 0 or 1: exact
        if (v.swizzle == SWIZZLE_RBGG) v.swizzle = SWIZZLE_RGBB;     // 6_5_5 unpacked in its own order
        v.host = H::RGBA8;
        break;
    default: break;
    }
    if (!support.usable(v.host)) v.host = H::Unsupported;
    return v;
}

// The version signed components read (Signed::None when there is none):
// the same image where the bits are the same (a snorm view; the floats are
// signed either way), else one of its own: 10_11_11 and 11_11_10 widened as
// signed (Xenia's R11G11B10ToRGBA16SNorm), 16-bit snorm as floats where the
// host has no 16-bit snorm (R16SNormToFloat). DXN and DXT5A decompressed
// have none, as in Xenia.
Signed signed_of(const FormatEntry &f, const Support &support, const Version &u, Version &out) {
    if (f.signs == S::None || u.host == H::Unsupported) return S::None;
    Load load = f.signs == S::Separate ? f.signed_load : f.load;
    if (is_float(f.host)) {
        out = u;
        return S::View;
    }
    if ((f.host == H::BC4 || f.host == H::BC5) && u.host != f.host) return S::None;
    if (support.snorm_usable(f.host)) {
        out = Version{};
        out.host = f.host, out.load = load, out.swizzle = u.swizzle, out.snorm = true;
    } else if (is_norm16(f.host)) {
        out = Version{};
        out.host = half_of(f.host), out.load = load, out.half = Half::FromSnorm, out.swizzle = u.swizzle;
        out.approximate = std::string(host_format_name(f.host)) + " snorm as 16-bit floats";
        if (!support.usable(out.host)) return S::None;
    } else {
        return S::None;
    }
    return out.host == u.host && out.load == u.load && out.half == u.half ? S::View : S::Separate;
}

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

// pixel_formats.xesli's 16-bit rearrangements: the guest's packed formats
// have red in the low bits, the host's in the high ones (R5G6B5, A1R5G5B5) or
// the alpha there (B4G4R4A4); 6_5_5's components land as R5G6B5's red, blue
// and green (read through RBGG).
uint16_t transform16(Load load, uint32_t v) {
    switch (load) {
    case L::R5G6B5ToB5G6R5: v = (v & 0x07E0) | ((v & 0x001F) << 11) | ((v & 0xF800) >> 11); break;
    case L::R5G5B6ToB5G6R5: v = ((v & 0x001F) << 11) | ((v & 0xFFE0) >> 5); break;
    case L::R5G5B5A1ToB5G5R5A1: v = (v & 0x83E0) | ((v & 0x001F) << 10) | ((v & 0x7C00) >> 10); break;
    case L::R4G4B4A4ToA4R4G4B4: v = ((v & 0x0FFF) << 4) | (v >> 12); break;
    default: break;
    }
    return uint16_t(v);
}
bool is_transform16(Load load) {
    return load == L::R5G6B5ToB5G6R5 || load == L::R5G5B6ToB5G6R5 || load == L::R5G5B5A1ToB5G5R5A1 ||
           load == L::R4G4B4A4ToA4R4G4B4;
}

// pixel_formats.xesli's XeR11G11B10UNormToRGBA16 and kin: an n-bit unsigned
// component to 16 bits by replicating its top bits below it; a signed one
// (XeSNorm11To16, XeSNorm10To16) by its magnitude, the most negative value
// taken as the one above it (both are -1), replicated to 15 bits, the sign
// put back.
uint32_t unorm_to_16(uint32_t v, uint32_t bits) { return (v << (16 - bits)) | (v >> (2 * bits - 16)); }
uint32_t snorm_to_16(uint32_t s, uint32_t bits) {
    uint32_t sign = s >> (bits - 1), mask = (1u << bits) - 1;
    if (s == (1u << (bits - 1))) s++;
    uint32_t magnitude = (s ^ (sign ? mask : 0)) + sign;
    magnitude = (magnitude << (16 - bits)) | (magnitude >> (2 * bits - 17));
    return ((magnitude ^ (sign ? 0xFFFFu : 0)) + sign) & 0xFFFF;
}
bool is_widening(Load load) {
    return load == L::R11G11B10ToRGBA16 || load == L::R10G11B11ToRGBA16 || load == L::R11G11B10ToRGBA16Snorm ||
           load == L::R10G11B11ToRGBA16Snorm;
}
// a 32-bit texel to RGBA16, alpha 1: 10_11_11 has red in its low 11 bits,
// green in the next 11 and blue in the top 10; 11_11_10 red in its low 10,
// green and blue in 11 each
void widen(Load load, uint32_t texel, uint16_t out[4]) {
    bool r11 = load == L::R11G11B10ToRGBA16 || load == L::R11G11B10ToRGBA16Snorm;
    bool snorm = load == L::R11G11B10ToRGBA16Snorm || load == L::R10G11B11ToRGBA16Snorm;
    uint32_t widths[3] = {r11 ? 11u : 10u, 11u, r11 ? 10u : 11u}, shift = 0;
    for (uint32_t c = 0; c < 3; shift += widths[c], c++) {
        uint32_t v = (texel >> shift) & ((1u << widths[c]) - 1);
        out[c] = uint16_t(snorm ? snorm_to_16(v, widths[c]) : unorm_to_16(v, widths[c]));
    }
    out[3] = snorm ? 0x7FFF : 0xFFFF;
}

// The packed 16-bit formats unpacked to RGBA8 where the host lacks their own
// (5_6_5, 6_5_5, 1_5_5_5, 4_4_4_4 from the low bits): each component's bits
// replicated down to 8, alpha 1 where there is none.
bool is_unpack16(Load load) {
    return load == L::R5G6B5ToRGBA8 || load == L::R5G5B6ToRGBA8 || load == L::R5G5B5A1ToRGBA8 ||
           load == L::R4G4B4A4ToRGBA8;
}
uint32_t unpack16(Load load, uint32_t v) {
    static const uint8_t widths[4][4] = {{5, 6, 5, 0}, {5, 5, 6, 0}, {5, 5, 5, 1}, {4, 4, 4, 4}};
    const uint8_t *w = widths[uint32_t(load) - uint32_t(L::R5G6B5ToRGBA8)];
    uint32_t out = 0, shift = 0;
    for (uint32_t c = 0; c < 4; shift += w[c], c++) {
        uint32_t bits = w[c], x = (v >> shift) & ((1u << bits) - 1);
        uint32_t byte = !bits ? 255 : bits == 1 ? x * 255 : (x << (8 - bits)) | (x >> (2 * bits - 8));
        out |= byte << (8 * c);
    }
    return out;
}

// pixel_formats.xesli's pack_half_2x16 of XeRG16UNormToRG16Float and kin: a
// 16-bit unorm (or snorm, clamped to -1) component as the float nearest it,
// to half precision rounding to nearest even (texture_load.comp computes it
// the same way, bit for bit)
uint16_t float_to_half(float f) {
    uint32_t x = std::bit_cast<uint32_t>(f), sign = (x >> 16) & 0x8000;
    x &= 0x7FFFFFFF;
    if (x < 0x38800000) { // below the smallest normal half: a denormal (or 0)
        if (x < 0x33000000) return uint16_t(sign);
        uint32_t mantissa = (x & 0x7FFFFF) | 0x800000, e = x >> 23; // f = mantissa * 2^(e - 150)
        uint32_t shift = 126 - e;                                   // to units of 2^-24
        uint32_t h = mantissa >> shift, rest = mantissa & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1))) h++;
        return uint16_t(sign | h);
    }
    uint32_t h = ((x - 0x38000000) >> 13), rest = x & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) h++;
    return uint16_t(sign | h);
}
uint16_t norm16_to_half(uint32_t v, Half half) {
    // 1/32767 and 1/65535 by their bits, as texture_load.comp has them
    float f = half == Half::FromSnorm ? std::max(float(int16_t(uint16_t(v))) * std::bit_cast<float>(0x38000100u), -1.0f)
                                      : float(v & 0xFFFF) * std::bit_cast<float>(0x37800080u);
    return float_to_half(f);
}

// A block decompressed to its texels (row-major, the host format's bytes
// each), as Xenia's decompressing loads make them.
uint32_t texel_bytes(Load load) {
    switch (load) {
    case L::Dxt3aToR8: case L::Dxt5aToR8: return 1;
    case L::Dxt3aAs1111ToARGB4: case L::Ctx1ToRg8: case L::DxnToRG8: return 2;
    default: return 4;
    }
}
void decompress(Load load, const uint8_t *block, uint8_t *out) {
    switch (load) {
    case L::GBGR8ToRGBA8: case L::BGRG8ToRGBA8: {
        // pixel_formats.xesli's XeGBGR8ToRGB8WithRGBBSwizzle and
        // XeBGRG8ToRGB8WithRGBBSwizzle: Cr to red, each texel's Y to green,
        // Cb to blue and to alpha (right for the signed reading too)
        uint32_t v = load_le32(block);
        bool gbgr = load == L::GBGR8ToRGBA8; // Cr_Y1_Cb_Y0: Y0, Cb, Y1, Cr from the low byte
        uint8_t cr = uint8_t(gbgr ? v >> 24 : v >> 16), cb = uint8_t(gbgr ? v >> 8 : v);
        uint8_t y[2] = {uint8_t(gbgr ? v : v >> 8), uint8_t(gbgr ? v >> 16 : v >> 24)};
        for (uint32_t t = 0; t < 2; t++)
            out[4 * t] = cr, out[4 * t + 1] = y[t], out[4 * t + 2] = cb, out[4 * t + 3] = cb;
        break;
    }
    case L::Dxt3aToR8: dxt3a_to_r8(block, out); break;
    case L::Dxt3aAs1111ToARGB4: {
        // pixel_formats.xesli's XeDXT3AAs1111TwoBlocksRowToARGB4: a texel's
        // 4 alpha bits are its red (bit 0), green, blue and alpha (bit 3), each
        // widened to B4G4R4A4's 4 bits
        uint32_t w[2] = {load_le32(block), load_le32(block + 4)};
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t a = (w[i >> 3] >> (4 * (i & 7))) & 0xF;
            uint32_t v = ((a & 1) << 4) | (((a >> 1) & 1) << 8) | (((a >> 2) & 1) << 12) | ((a >> 3) & 1);
            v |= v << 1;
            v |= v << 2;
            out[2 * i] = uint8_t(v), out[2 * i + 1] = uint8_t(v >> 8);
        }
        break;
    }
    case L::Dxt3aAs1111ToRGBA8: {
        // the same bits as RGBA8's components, each 0 or 255
        uint32_t w[2] = {load_le32(block), load_le32(block + 4)};
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t a = (w[i >> 3] >> (4 * (i & 7))) & 0xF;
            for (uint32_t c = 0; c < 4; c++) out[4 * i + c] = (a >> c) & 1 ? 255 : 0;
        }
        break;
    }
    case L::Ctx1ToRg8: ctx1_to_rg8(block, out); break;
    case L::Dxt1ToRGBA8: dxt_color(block, true, out); break;
    case L::Dxt3ToRGBA8:
        dxt_color(block + 8, false, out);
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t a = (load_le32(block + 4 * (i >> 3)) >> (4 * (i & 7))) & 0xF;
            out[4 * i + 3] = uint8_t(a | (a << 4));
        }
        break;
    case L::Dxt5ToRGBA8: {
        dxt_color(block + 8, false, out);
        uint8_t a[16];
        dxt5_alpha(block, a);
        for (uint32_t i = 0; i < 16; i++) out[4 * i + 3] = a[i];
        break;
    }
    case L::DxnToRG8: {
        uint8_t r[16], g[16];
        dxt5_alpha(block, r);
        dxt5_alpha(block + 8, g);
        for (uint32_t i = 0; i < 16; i++) out[2 * i] = r[i], out[2 * i + 1] = g[i];
        break;
    }
    case L::Dxt5aToR8: dxt5_alpha(block, out); break;
    default: break;
    }
}
} // namespace

const char *host_format_name(HostFormat f) {
    static const char *names[] = {"Unsupported", "R8",      "RG8",     "RGBA8",   "R5G6B5", "A1R5G5B5", "B4G4R4A4",
                                  "RGB10A2",     "R16",     "RG16",    "RGBA16",  "R16F",   "RG16F",    "RGBA16F",
                                  "R32",         "RG32",    "RGBA32",  "R32F",    "RG32F",  "RGBA32F",  "BC1",
                                  "BC2",         "BC3",     "BC4",     "BC5"};
    static_assert(sizeof(names) / sizeof(names[0]) == HOST_FORMAT_COUNT);
    return uint32_t(f) < HOST_FORMAT_COUNT ? names[uint32_t(f)] : "?";
}

bool decompresses(Load load) { return load >= L::GBGR8ToRGBA8; }

bool Support::usable(HostFormat f) const {
    // the 32-bit floats need no filtering: a sampler of them points where the host does not filter them
    bool needs_linear = f != H::R32F && f != H::RG32F && f != H::RGBA32F;
    return f != H::Unsupported && ((sampled >> uint32_t(f)) & 1) && (!needs_linear || ((linear >> uint32_t(f)) & 1));
}

uint32_t compose_swizzle(uint32_t guest, uint32_t host) {
    uint32_t out = 0;
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t g = (guest >> (3 * c)) & 7;
        uint32_t v = g < 4 ? (host >> (3 * g)) & 7 : g == 5 ? 5 : 4;
        out |= v << (3 * c);
    }
    return out;
}

uint32_t bytes_per_block(HostFormat format) {
    switch (format) {
    case H::R8: return 1;
    case H::RG8: case H::R5G6B5: case H::A1R5G5B5: case H::B4G4R4A4: case H::R16: case H::R16F: return 2;
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

std::array<uint32_t, 4> component_bits(uint32_t format) {
    switch (format) {
    case 2: case 8: case 9: case 58: case 59: return {8, 0, 0, 0};       // 8, 8_A, 8_B, DXT3A, DXT5A
    case 3: return {5, 5, 5, 1};                                         // 1_5_5_5
    case 4: return {5, 6, 5, 0};                                         // 5_6_5
    case 5: return {5, 5, 6, 0};                                         // 6_5_5
    case 6: case 14: case 50: case 62: return {8, 8, 8, 8};              // 8_8_8_8 and its kin
    case 7: case 54: return {10, 10, 10, 2};                             // 2_10_10_10
    case 10: case 49: case 60: return {8, 8, 0, 0};                      // 8_8, DXN, CTX1
    case 13: case 25: return {16, 16, 0, 0};                             // 16_16
    case 15: return {4, 4, 4, 4};                                        // 4_4_4_4
    case 16: case 55: return {11, 11, 10, 0};                            // 10_11_11
    case 17: case 56: return {10, 11, 11, 0};                            // 11_11_10
    case 21: case 26: return {16, 16, 16, 16};                           // 16_16_16_16
    case 24: return {16, 0, 0, 0};                                       // 16
    default: return {0, 0, 0, 0};
    }
}

uint32_t q16_components(const Info &info) {
    if (info.num_format || !component_bits(info.format)[0]) return 0;
    uint32_t mask = 0;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t source = (info.swizzle >> (3 * k)) & 7;
        if (source >= 4 || info.signs[source] == 0) mask |= 1u << k;
    }
    return mask;
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

bool plan(const Info &info, Plan &out, std::string &unsupported, const Support &support, bool signed_version) {
    out = Plan{};
    const FormatEntry &f = FORMATS[info.format & 63];
    if (f.host == H::Unsupported) {
        unsupported = std::string("format ") + xenos::format_name(info.format);
        return false;
    }
    // the version the host holds, and the signed one
    Version u = unsigned_version(f, support), s;
    if (u.host == H::Unsupported) {
        unsupported = std::string("format ") + xenos::format_name(info.format) + " (the host samples no " +
                      host_format_name(f.host) + " nor its fallback)";
        return false;
    }
    Signed signs = signed_of(f, support, u, s);
    if (signed_version && signs != S::Separate) {
        unsupported = "no separate signed version";
        return false;
    }
    const Version &v = signed_version ? s : u;
    // Xenia's BindingInfoFromFetchConstant: "1D texture has tiling enabled in
    // the fetch constant, but this appears to be completely wrong - ignoring",
    // and likewise packed mips
    if (info.dimension == 0 && info.tiled) {
        unsupported = "tiled 1D";
        return false;
    }
    if (info.dimension == 0 && info.packed_mips) {
        unsupported = "1D with packed mips";
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

    out.format = v.host;
    out.load = v.load;
    out.half = v.half;
    out.host_swizzle = v.swizzle;
    out.signed_read = signs;
    out.snorm = signed_version && v.snorm;
    out.approximate = v.approximate;
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

bool filterable(const Info &info, const Support &support) {
    const FormatEntry &f = FORMATS[info.format & 63];
    Version u = unsigned_version(f, support), s;
    if (u.host == H::Unsupported) return true; // not drawn with anyway
    bool any_signed = false, any_unsigned = false;
    for (uint32_t c = 0; c < 4; c++) (info.signs[c] == 1 ? any_signed : any_unsigned) = true;
    if (any_unsigned && !support.filters(u.host)) return false;
    if (any_signed && signed_of(f, support, u, s) != S::None && !support.filters(s.host)) return false;
    return true;
}

Signed signed_read(uint32_t format, const Support &support) {
    const FormatEntry &f = FORMATS[format & 63];
    if (f.host == H::Unsupported) return S::None;
    Version u = unsigned_version(f, support), s;
    return signed_of(f, support, u, s);
}

uint64_t level_bytes(const Plan &plan, uint32_t level) {
    uint64_t slices = 0;
    for (const SliceLoad &s : plan.slices) slices += s.level == level;
    uint64_t w = std::max(plan.width >> level, 1u), h = std::max(plan.height >> level, 1u);
    uint64_t units = is_compressed(plan.format) ? ((w + 3) / 4) * ((h + 3) / 4) : w * h;
    return units * bytes_per_block(plan.format) * slices;
}

void gpu_levels(const Plan &plan, std::vector<GpuLevel> &out, uint64_t &total_bytes) {
    out.clear();
    total_bytes = 0;
    for (size_t i = 0; i < plan.slices.size();) {
        size_t j = i;
        while (j < plan.slices.size() && plan.slices[j].level == plan.slices[i].level) j++;
        const SliceLoad &s = plan.slices[i];
        uint64_t bytes = level_bytes(plan, s.level);
        GpuLevel g{};
        g.base = s.base;
        g.slice_stride = j - i > 1 && !s.is_3d ? plan.slices[i + 1].base - s.base : 0;
        g.slices = uint32_t(j - i);
        g.layered = s.is_3d ? 0 : 1;
        g.pitch_blocks = s.pitch_blocks, g.z_rows = s.z_rows;
        g.ox = s.ox, g.oy = s.oy, g.oz = s.oz;
        g.tiled = s.tiled, g.is_3d = s.is_3d;
        g.log2_bytes = s.log2_bytes_per_block;
        g.endian = plan.endian;
        g.blocks_x = s.blocks_x, g.blocks_y = s.blocks_y;
        g.out_word = uint32_t(total_bytes / 4);
        g.words = uint32_t((bytes + 3) / 4);
        g.load = uint32_t(plan.load);
        g.half = uint32_t(plan.half);
        uint32_t groups = (g.words + 63) / 64, across = std::max(std::min(groups, 32768u), 1u);
        g.row_words = across * 64;
        g.out_width = std::max(plan.width >> s.level, 1u), g.out_height = std::max(plan.height >> s.level, 1u);
        out.push_back(g);
        total_bytes += (bytes + 15) & ~uint64_t(15);
        i = j;
    }
}

bool decode(const Info &info, const GuestMemory &memory, Image &out, std::string &unsupported,
            const Support &support, bool signed_version) {
    out = Image{};
    Plan p;
    if (!plan(info, p, unsupported, support, signed_version)) return false;
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
        if (p.load == L::DepthUnormToFloat || p.load == L::DepthFloatToFloat) {
            // the depth in the high 24 bits, to a float: unorm24 as
            // XeUNorm24To32 (exact), float24 as XeFloat20e4To32
            for (size_t i = 0; i + 4 <= blocks.size(); i += 4) {
                uint32_t d24 = load_le32(blocks.data() + i) >> 8;
                float value = p.load == L::DepthUnormToFloat ? float(d24 + (d24 >> 23)) * (1.0f / 16777216.0f)
                                                             : float20e4_to_float(d24);
                store_le32(blocks.data() + i, std::bit_cast<uint32_t>(value));
            }
        } else if (is_transform16(p.load)) {
            for (size_t i = 0; i + 2 <= blocks.size(); i += 2) {
                uint16_t v = transform16(p.load, load_le16(blocks.data() + i));
                std::memcpy(blocks.data() + i, &v, 2);
            }
        } else if (is_widening(p.load)) {
            std::vector<uint8_t> wide(blocks.size() * 2);
            for (size_t i = 0; i + 4 <= blocks.size(); i += 4) {
                uint16_t rgba[4];
                widen(p.load, load_le32(blocks.data() + i), rgba);
                std::memcpy(wide.data() + 2 * i, rgba, 8);
            }
            blocks = std::move(wide);
        } else if (is_unpack16(p.load)) {
            std::vector<uint8_t> wide(blocks.size() * 2);
            for (size_t i = 0; i + 2 <= blocks.size(); i += 2)
                store_le32(wide.data() + 2 * i, unpack16(p.load, load_le16(blocks.data() + i)));
            blocks = std::move(wide);
        }
        if (!decompresses(p.load)) {
            // 16-bit fixed point as halves where the host has no format for it
            if (p.half != Half::None)
                for (size_t i = 0; i + 2 <= blocks.size(); i += 2) {
                    uint16_t h = norm16_to_half(load_le16(blocks.data() + i), p.half);
                    std::memcpy(blocks.data() + i, &h, 2);
                }
            dst = std::move(blocks);
            blocks = {};
            continue;
        }
        // decompressed to texels, as Xenia's load shaders for these do: a
        // block's that lie in the level
        uint32_t lw = std::max(p.width >> level, 1u), lh = std::max(p.height >> level, 1u);
        uint32_t tb = texel_bytes(p.load), bw = p.block_width, bh = p.block_height;
        dst.assign(size_t(lw) * lh * slices * tb, 0);
        for (uint32_t s = 0; s < slices; s++)
            for (uint32_t by = 0; by < blocks_y; by++)
                for (uint32_t bx = 0; bx < blocks_x; bx++) {
                    const uint8_t *block = blocks.data() + ((size_t(s) * blocks_y + by) * blocks_x + bx) * bpb;
                    uint8_t texels[64];
                    decompress(p.load, block, texels);
                    for (uint32_t ty = 0; ty < bh && by * bh + ty < lh; ty++)
                        for (uint32_t tx = 0; tx < bw && bx * bw + tx < lw; tx++)
                            std::memcpy(dst.data() + ((size_t(s) * lh + by * bh + ty) * lw + bx * bw + tx) * tb,
                                        texels + (ty * bw + tx) * tb, tb);
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
        case H::R5G6B5: {
            uint32_t v = load_le16(p);
            r = expand(v >> 11, 5), g = expand((v >> 5) & 63, 6), b = expand(v & 31, 5);
            break;
        }
        case H::A1R5G5B5: {
            uint32_t v = load_le16(p);
            r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5);
            a = (v >> 15) ? 255 : 0;
            break;
        }
        case H::B4G4R4A4: {
            uint32_t v = load_le16(p);
            r = expand((v >> 4) & 15, 4), g = expand((v >> 8) & 15, 4), b = expand(v >> 12, 4);
            a = expand(v & 15, 4);
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
