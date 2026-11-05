// Guest textures read out of guest memory into host images: the fetch
// constant parsed, the texels untiled (2D, 3D, cube, stacked), byte-swapped
// as the constant says, every mip level (packed mips included), in a host
// format that keeps the guest's bits (block-compressed formats stay
// compressed). What the renderer uploads and samples (plan B3).
//
// Signedness, gamma, the exponent adjust and the component swizzle of the
// fetch constant are not applied here: they are the renderer's (image view
// swizzle, sampler, shader).

#pragma once

#include "xenoforge/guest_memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xf::texture {

enum class HostFormat {
    Unsupported,
    R8, RG8, RGBA8,           // 8, 8_8, 8_8_8_8 (and the 8-bit others, expanded)
    B5G6R5, A1R5G5B5, A4R4G4B4, // 5_6_5, 1_5_5_5, 4_4_4_4 as host packed formats
    RGB10A2,                  // 2_10_10_10
    R16, RG16, RGBA16,        // 16-bit fixed point, raw
    R16F, RG16F, RGBA16F,
    R32, RG32, RGBA32,        // 32-bit integers, raw
    R32F, RG32F, RGBA32F,
    BC1, BC2, BC3, BC4, BC5,  // DXT1, DXT2_3, DXT4_5, DXT3A/DXT5A, DXN
};

uint32_t bytes_per_block(HostFormat format); // a texel, or a 4x4 block when compressed
bool is_compressed(HostFormat format);

// The texture fetch constant, as the GPU reads it (xenos.h's xe_gpu_texture_fetch_t).
struct Info {
    uint32_t format = 0;         // the guest format (format_name)
    uint32_t dimension = 0;      // 0 1D, 1 2D or stacked, 2 3D, 3 cube
    uint32_t width = 1, height = 1, depth = 1; // depth: 3D depth, or layers (6 for a cube)
    bool tiled = false;
    uint32_t pitch = 0;          // in texels, the guest's row pitch
    uint32_t endian = 0;
    uint32_t base_address = 0, mip_address = 0;
    uint32_t mip_min = 0, mip_max = 0;
    bool packed_mips = false;
    uint32_t swizzle = 0;        // 12 bits: per component, 0-5 x y z w 0 1
    uint32_t signs[4] = {};
    int32_t exp_adjust = 0;
    bool gamma = false;          // the components to be read as gamma-encoded
    HostFormat host = HostFormat::Unsupported;
    uint32_t num_format = 0;     // 0 fraction, 1 integer
    // the sampler's part, raw (xenos.h's ClampMode, TextureFilter, AnisoFilter, BorderColor)
    uint32_t clamp[3] = {};
    uint32_t mag_filter = 0, min_filter = 0, mip_filter = 0, aniso_filter = 0;
    uint32_t border_color = 0;
    int32_t lod_bias = 0;        // in 1/32
};

Info parse_fetch_constant(const uint32_t dwords[6]);

// The levels the constant gives (Xenia's GetSubresourcesFromFetchConstant):
// the base (level 0) when there is a base address, the mips (1 to mip_max)
// when there is a mip address; mip_min and mip_max as the sampler should
// clamp to them.
struct Levels {
    bool base = false, mips = false;
    uint32_t base_address = 0, mip_address = 0;
    uint32_t mip_min = 0, mip_max = 0;
};
Levels levels(const Info &info);

// A guest format's block (1x1 texel for most, 4x4 for the compressed ones)
// and its size; bytes 0 for an unknown format.
struct Block {
    uint32_t width = 1, height = 1, bytes = 0;
};
Block format_block(uint32_t format);

// The byte offset of a block in a tiled surface (Xenia's texture_address.h);
// pitch and height in blocks, aligned to 32.
uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block_log2);
uint64_t tiled_offset_3d(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height,
                         uint32_t bytes_per_block_log2);
// Where a level sits in the packed mip tail, in blocks (Xenia's
// GetPackedMipOffset); false when the level is not in the tail. depth is 1
// unless 3D.
bool packed_mip_offset(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t level,
                       uint32_t &x, uint32_t &y, uint32_t &z);

struct Image {
    HostFormat format = HostFormat::Unsupported;
    uint32_t width = 0, height = 0, depth = 1; // of level 0; depth as in Info
    // level by level, each tightly packed: rows (of blocks when compressed)
    // of every slice or layer in turn. levels[0] is empty when the constant
    // gives no base level (only mips). The packed 16-bit formats keep the
    // guest's bit order: the first component in the low bits.
    std::vector<std::vector<uint8_t>> levels;
};

// False with the reason when the format or layout is not done.
bool decode(const Info &info, const GuestMemory &memory, Image &out, std::string &unsupported);

// A level's first slice or layer as row-major RGBA8, for looking at or
// presenting: block-compressed formats decompressed, floats clamped to 0-1,
// missing components 0 (alpha 1), no swizzle. Empty when the level is.
std::vector<uint8_t> to_rgba8(const Image &image, uint32_t level = 0);

} // namespace xf::texture
