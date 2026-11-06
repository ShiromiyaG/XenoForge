// Guest textures read out of guest memory into host images: the fetch
// constant parsed, the texels untiled (2D, 3D, cube, stacked), byte-swapped
// as the constant says, every mip level (packed mips included), in the host
// format Xenia's texture caches give the guest's (vulkan_texture_cache.cc's
// and d3d12_texture_cache.h's tables): most keep the guest's bits (the
// block-compressed formats stay compressed), the rest are rearranged,
// widened or decompressed by their loads as Xenia's load shaders do. What the
// renderer uploads and samples (plan B3).
//
// Signedness, gamma, the exponent adjust and the component swizzle of the
// fetch constant are not applied here: they are the renderer's (image view
// swizzle, sampler, shader). Where the host needs other bits for signed
// components (10_11_11 and 11_11_10), the signed version is a plan of its
// own (Xenia's separate signed versions).

#pragma once

#include "xenoforge/guest_memory.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xf::texture {

// The host formats, by the bits an image holds. The packed 16-bit ones are
// Vulkan's formats of those names, which every device samples: R5G6B5 has
// red in the high bits, A1R5G5B5 red in bits 10-14, B4G4R4A4 alpha in the
// low bits.
enum class HostFormat {
    Unsupported,
    R8, RG8, RGBA8,             // 8, 8_8, 8_8_8_8, the packed 4:2:2 formats decompressed
    R5G6B5, A1R5G5B5, B4G4R4A4, // 5_6_5 and 6_5_5, 1_5_5_5, 4_4_4_4 and DXT3A_AS_1_1_1_1
    RGB10A2,                    // 2_10_10_10
    R16, RG16, RGBA16,          // 16-bit fixed point raw; 10_11_11 and 11_11_10 widened to it
    R16F, RG16F, RGBA16F,
    R32, RG32, RGBA32,          // 32-bit integers, raw (render targets' copies; no texture reads them)
    R32F, RG32F, RGBA32F,
    BC1, BC2, BC3, BC4, BC5,    // DXT1, DXT2_3, DXT4_5, DXT5A, DXN
};
constexpr uint32_t HOST_FORMAT_COUNT = uint32_t(HostFormat::BC5) + 1;

uint32_t bytes_per_block(HostFormat format); // a texel, or a 4x4 block when compressed
bool is_compressed(HostFormat format);
const char *host_format_name(HostFormat format);

// What the host can sample, a bit per host format (from the device's format
// properties): sampled, filtered linearly, and whether its snorm counterpart
// is both. A format is used when it is sampled and filtered (the 32-bit
// floats sampled alone: a sampler of a format the host does not filter
// points, as Xenia's GetSamplerParameters does); where it is not, a plan
// takes a fallback, as Xenia's Vulkan texture cache does: the DXT formats
// decompressed (DXT1-5 to RGBA8, DXN to RG8, DXT5A to R8), 16-bit fixed
// point as 16-bit floats, the packed 16-bit formats unpacked to RGBA8; or it
// gives up with the reason. All by default.
struct Support {
    uint64_t sampled = ~uint64_t(0), linear = ~uint64_t(0), snorm = ~uint64_t(0);
    bool usable(HostFormat f) const;
    bool filters(HostFormat f) const { return (sampled >> uint32_t(f)) & (linear >> uint32_t(f)) & 1; }
    bool snorm_usable(HostFormat f) const { return (snorm >> uint32_t(f)) & 1; }
};

// Xenia's texture swizzles (XE_GPU_TEXTURE_SWIZZLE_*): per component x, y, z,
// w, 3 bits each from bit 0, what it reads: 0-3 a texel's component, 4 zero,
// 5 one (6 and 7 read zero).
constexpr uint32_t SWIZZLE_RGBA = 0 | 1 << 3 | 2 << 6 | 3 << 9;
constexpr uint32_t SWIZZLE_RRRR = 0;
constexpr uint32_t SWIZZLE_RGGG = 0 | 1 << 3 | 1 << 6 | 1 << 9;
constexpr uint32_t SWIZZLE_RGBB = 0 | 1 << 3 | 2 << 6 | 2 << 9;
constexpr uint32_t SWIZZLE_RBGG = 0 | 2 << 3 | 1 << 6 | 1 << 9;
// The fetch constant's swizzle (guest components) through a host format's
// (Plan::host_swizzle): per result component, the host component it reads
// (0-3), or 4 zero, 5 one.
uint32_t compose_swizzle(uint32_t guest, uint32_t host);

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
    // the guest memory read for them, (physical address, bytes) per slice or
    // layer of each level: what a cache of the image watches for changes.
    // Tiled slices give the 4 KB pages their tiles lie in (a little more than
    // the texels when the texture is narrower than its pitch).
    std::vector<std::pair<uint32_t, uint32_t>> footprint;
};

// What a decode does to a slice's blocks after reading and byte-swapping
// them, as Xenia's load shaders of those names: nothing (the guest's bits,
// compressed blocks included); depth to 32-bit floats; a 16-bit texel's
// components moved where the host's packed format has them; a 32-bit texel
// widened to 16 bits a component (unsigned, or signed for the signed
// version); a 16-bit texel unpacked to RGBA8 (where the host lacks the
// packed format: the components' bits replicated down, beyond Xenia); or a
// decompression to texels. texture_load.comp makes every one of them, as
// decode does on the CPU (the values are the order texture_load.comp
// numbers them).
enum class Load : uint8_t {
    Copy,
    DepthUnormToFloat, DepthFloatToFloat,
    R5G6B5ToB5G6R5, R5G5B6ToB5G6R5, R5G5B5A1ToB5G5R5A1, R4G4B4A4ToA4R4G4B4,
    R11G11B10ToRGBA16, R10G11B11ToRGBA16, R11G11B10ToRGBA16Snorm, R10G11B11ToRGBA16Snorm,
    R5G6B5ToRGBA8, R5G5B6ToRGBA8, R5G5B5A1ToRGBA8, R4G4B4A4ToRGBA8,
    GBGR8ToRGBA8, BGRG8ToRGBA8, // the 2x1 blocks of Cr_Y1_Cb_Y0 and Y1_Cr_Y0_Cb to two texels each
    Dxt3aToR8, Dxt3aAs1111ToARGB4, Dxt3aAs1111ToRGBA8, Ctx1ToRg8,
    Dxt1ToRGBA8, Dxt3ToRGBA8, Dxt5ToRGBA8, DxnToRG8, Dxt5aToR8, // where the host has no BC
};
// Whether the load decompresses blocks to texels (its output is rows of
// texels, not of the guest's blocks).
bool decompresses(Load load);

// After the load, 16-bit fixed-point components as 16-bit floats, where the
// host has no 16-bit unorm or snorm format it filters (Xenia's
// R16UNormToFloat, R16SNormToFloat and kin): from unorm, or from snorm.
enum class Half : uint8_t { None, FromUnorm, FromSnorm };

// How a texture's signed components are read: through no signed version
// (the host has no signed format for the guest's, as in Xenia: they are
// read as unsigned, counted), a signed view of the same image (snorm over
// the same bits; the float formats are signed either way), or an image of
// its own, loaded by its own plan (plan's signed_version).
enum class Signed : uint8_t { None, View, Separate };

// One slice (or layer) of one level, as the loads read it: Xenia's guest
// addressing of its blocks (texture_load.xesli's XeTextureLoadSourceAddress)
// at the packed tail's offset.
struct SliceLoad {
    uint32_t level = 0;
    uint32_t base = 0;               // its first byte in guest memory (a 3D level's, for 3D)
    uint32_t z = 0;                  // 3D: its slice; else 0
    uint32_t pitch_blocks = 0, z_rows = 0;
    uint32_t ox = 0, oy = 0, oz = 0; // in the packed tail, in blocks
    bool tiled = false, is_3d = false;
    uint32_t blocks_x = 0, blocks_y = 0;
    uint32_t log2_bytes_per_block = 0;
    // where its blocks lie, in bytes from base: [low, high), the 4 KB pages
    // of its tiles when tiled, else exact
    uint64_t low = 0, high = 0;

    // the byte offset from base of block (x, y)
    uint64_t offset(uint32_t x, uint32_t y) const;
};

// How a texture is read out of guest memory, without reading it: its host
// format and size, and every slice of every level in the order decode
// writes them (level by level, each level's slices in turn, tightly packed).
// Both the CPU's decode and the renderer's loads on the GPU follow it.
struct Plan {
    HostFormat format = HostFormat::Unsupported;
    Load load = Load::Copy;
    uint32_t width = 0, height = 0, depth = 1; // as Image's
    uint32_t bytes_per_block = 0;              // the guest's block (a texel, or a compressed 4x4 block)
    uint32_t block_width = 1, block_height = 1;
    uint32_t endian = 0;
    uint32_t level_count = 0; // levels 0 to level_count - 1; level 0 has no slices without a base
    std::vector<SliceLoad> slices;
    std::vector<std::pair<uint32_t, uint32_t>> footprint; // as Image's: (physical address, bytes) per slice
    // what each guest component reads of the host format (SWIZZLE_*: the
    // last component a format has stands for those it lacks, as Xenia's
    // host swizzles have it)
    uint32_t host_swizzle = SWIZZLE_RGBA;
    Half half = Half::None;
    Signed signed_read = Signed::None;
    // a signed version's plan whose image is read as the snorm format of
    // `format` (10_11_11 and 11_11_10 as 16-bit snorm)
    bool snorm = false;
    // the host format holds fewer bits than the guest's (a fallback: 16-bit
    // fixed point as floats, the packed 16-bit formats as RGBA8); the reason
    std::string approximate;
};

// False with the reason when the format or layout is not done. signed_version:
// the plan of the version signed components read when it is an image of its
// own (Signed::Separate); the unsigned one's otherwise.
bool plan(const Info &info, Plan &out, std::string &unsupported, const Support &support = {},
          bool signed_version = false);
bool decode(const Info &info, const GuestMemory &memory, Image &out, std::string &unsupported,
            const Support &support = {}, bool signed_version = false);

// Whether the host filters the formats the constant's components read (its
// unsigned version, and the signed one where a component is signed).
bool filterable(const Info &info, const Support &support);

// How the format's signed components are read on this host (Plan's
// signed_read), without making a plan: what a texture cache keys its images
// by (Xenia's TextureKey::signed_separate).
Signed signed_read(uint32_t format, const Support &support);

// The bytes a level of the plan's image holds (all its slices), as decode
// writes it: texels or compressed blocks of the host format, tightly packed.
uint64_t level_bytes(const Plan &plan, uint32_t level);

// texture_load.comp's constants for one level of a plan (all its slices in
// one dispatch), as the renderer passes them: out_word is the word of the
// output buffer the level starts at, words how many it writes (each
// invocation one), row_words the invocations per row of workgroups,
// out_width and out_height the level's texels per row and rows.
struct GpuLevel {
    uint32_t base, slice_stride, slices, layered, pitch_blocks, z_rows, ox, oy, oz, tiled, is_3d, log2_bytes, endian;
    uint32_t blocks_x, blocks_y, out_word, words, load, half, row_words, out_width, out_height;
};
// Every level of the plan, each starting at a 16-byte aligned offset of the
// output (as the renderer copies them into the image), and the bytes of all.
void gpu_levels(const Plan &plan, std::vector<GpuLevel> &out, uint64_t &total_bytes);

// A level's first slice or layer as row-major RGBA8, for looking at or
// presenting: block-compressed formats decompressed, floats clamped to 0-1,
// missing components 0 (alpha 1), the host format's components in order (no
// swizzle). Empty when the level is.
std::vector<uint8_t> to_rgba8(const Image &image, uint32_t level = 0);

} // namespace xf::texture
