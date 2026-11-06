// Guest textures and resolves (texture.h, resolve.h): the tiled and packed
// mip addressing against hand computations, textures of each kind laid out
// in guest memory by hand and decoded, and resolves written and read back as
// textures.

#include "xenoforge/resolve.h"
#include "xenoforge/texture.h"
#include "xenoforge/xenos.h"

#include "check.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <set>

using namespace xf;
using texture::HostFormat;

namespace {

uint32_t bswap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v & 0xFF00) << 8) | (v << 24); }

uint32_t hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352D, x ^= x >> 15, x *= 0x846CA68B, x ^= x >> 16;
    return x;
}

// A fetch constant's dwords from the fields a test sets.
struct Fetch {
    uint32_t format = 6, dimension = 1, width = 1, height = 1, depth = 1;
    bool tiled = true, stacked = false, packed = false;
    uint32_t pitch = 0; // texels
    uint32_t endian = 0, base = 0, mip_base = 0, mip_min = 0, mip_max = 0;
    uint32_t signs = 0; // two bits a component

    texture::Info info() const {
        uint32_t d[6] = {};
        d[0] = 2 | (signs << 2) | ((pitch >> 5) << 22) | (uint32_t(tiled) << 31);
        d[1] = format | (endian << 6) | (uint32_t(stacked) << 10) | (base & 0xFFFFF000);
        if (dimension == 0) d[2] = width - 1;
        else if (dimension == 2) d[2] = (width - 1) | ((height - 1) << 11) | ((depth - 1) << 22);
        else d[2] = (width - 1) | ((height - 1) << 13) | ((dimension == 3 ? 5 : depth - 1) << 26);
        d[3] = 0x688 << 1; // swizzle xyzw
        d[4] = (mip_min << 2) | (mip_max << 6);
        d[5] = (dimension << 9) | (uint32_t(packed) << 11) | (mip_base & 0xFFFFF000);
        return texture::parse_fetch_constant(d);
    }
};

void put_le32(GuestMemory &m, uint32_t address, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    m.write(address, b);
}

void test_tiled_addresses() {
    using texture::tiled_offset_2d, texture::tiled_offset_3d;
    // 32 bits per block, a pitch of one 32x32 tile
    CHECK(tiled_offset_2d(0, 0, 32, 2) == 0);
    CHECK(tiled_offset_2d(1, 0, 32, 2) == 4);
    CHECK(tiled_offset_2d(4, 0, 32, 2) == 32);   // byte bit 4 moves to bit 5
    CHECK(tiled_offset_2d(8, 0, 32, 2) == 64);   // pipe 1
    CHECK(tiled_offset_2d(0, 1, 32, 2) == 16);   // y's low bit at byte bit 4
    CHECK(tiled_offset_2d(0, 2, 32, 2) == 256);  // byte bits 5-7 move to 8-10
    CHECK(tiled_offset_2d(0, 8, 32, 2) == 1152); // pipe 2, inner row 4
    CHECK(tiled_offset_2d(0, 16, 32, 2) == 2048); // bank
    CHECK(tiled_offset_2d(32, 0, 64, 2) == 4096); // the next tile across
    CHECK(tiled_offset_2d(0, 32, 64, 2) == 8192); // the next tile row
    CHECK(tiled_offset_2d(16, 0, 32, 0) == 128); // 8 bits: pipe 2
    CHECK(tiled_offset_3d(0, 0, 1, 32, 32, 2) == 1024);
    CHECK(tiled_offset_3d(0, 0, 4, 32, 32, 2) == 18560); // the next 4 slices: tile, bank and pipe
    // a 64x64 32-bit surface: every block somewhere else, all inside it
    std::set<uint32_t> seen;
    bool inside = true;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t o = tiled_offset_2d(x, y, 64, 2);
            seen.insert(o);
            inside &= o < 64 * 64 * 4 && o % 4 == 0;
        }
    CHECK(seen.size() == 64 * 64);
    CHECK(inside);
}

void test_packed_mips() {
    uint32_t x, y, z;
    // 16x16: 16 at x 16, 8 at x 8, 4 at x 4, then down the left
    CHECK(texture::packed_mip_offset(16, 16, 1, 6, 0, x, y, z) && x == 16 && y == 0 && z == 0);
    CHECK(texture::packed_mip_offset(16, 16, 1, 6, 1, x, y, z) && x == 8 && y == 0);
    CHECK(texture::packed_mip_offset(16, 16, 1, 6, 2, x, y, z) && x == 4 && y == 0);
    CHECK(texture::packed_mip_offset(16, 16, 1, 6, 3, x, y, z) && x == 0 && y == 8 && z == 0);
    CHECK(texture::packed_mip_offset(16, 16, 1, 6, 4, x, y, z) && x == 0 && y == 4 && z == 0);
    // 256x64, wide: the tail from level 2, across the top
    CHECK(!texture::packed_mip_offset(256, 64, 1, 6, 1, x, y, z));
    CHECK(texture::packed_mip_offset(256, 64, 1, 6, 2, x, y, z) && x == 0 && y == 16);
    CHECK(texture::packed_mip_offset(256, 64, 1, 6, 5, x, y, z) && x == 32 && y == 0);
    CHECK(texture::packed_mip_offset(256, 64, 1, 6, 8, x, y, z) && x == 4 && y == 0 && z == 0);
    // DXT1 32x32: in 4x4 blocks
    CHECK(texture::packed_mip_offset(32, 32, 1, 18, 1, x, y, z) && x == 4 && y == 0);
    CHECK(texture::packed_mip_offset(32, 32, 1, 18, 4, x, y, z) && x == 0 && y == 2);
    // 3D 4x4x64: offsets under 4 go to deeper slices
    CHECK(texture::packed_mip_offset(4, 4, 64, 6, 4, x, y, z) && x == 0 && y == 1 && z == 8);
}

void test_parse() {
    uint32_t d[6] = {};
    d[0] = 2 | (1u << 2) | (3u << 4) | (2u << 10) | (5u << 22) | (1u << 31); // sign x signed, y gamma; clamp x 2
    d[1] = 20 | (1u << 6) | 0x12345000;                                     // DXT4_5, 8in16
    d[2] = 255 | (127u << 13);                                               // 256x128
    d[3] = 1 | (0x123u << 1) | (uint32_t(-3 & 0x3F) << 13) | (1u << 19);    // integer, exp_adjust -3, mag linear
    d[4] = (2u << 2) | (7u << 6) | (uint32_t(-32 & 0x3FF) << 12);           // mips 2-7, lod bias -1
    d[5] = (1u << 9) | (1u << 11) | 0x0ABCD000 | 2;                          // 2D, packed, border 2
    texture::Info i = texture::parse_fetch_constant(d);
    CHECK(i.format == 20 && i.host == HostFormat::BC3);
    CHECK(i.dimension == 1 && i.width == 256 && i.height == 128 && i.depth == 1);
    CHECK(i.tiled && i.pitch == 160 && i.endian == 1);
    CHECK(i.base_address == 0x12345000 && i.mip_address == 0x0ABCD000);
    CHECK(i.mip_min == 2 && i.mip_max == 7 && i.packed_mips);
    CHECK(i.signs[0] == 1 && i.signs[1] == 3 && i.gamma);
    CHECK(i.swizzle == 0x123 && i.exp_adjust == -3 && i.num_format == 1);
    CHECK(i.clamp[0] == 2 && i.mag_filter == 1 && i.lod_bias == -32 && i.border_color == 2);
    // stacked and cube depths
    Fetch f;
    f.width = f.height = 32, f.depth = 5, f.stacked = true;
    CHECK(f.info().depth == 5);
    f.stacked = false;
    CHECK(f.info().depth == 1);
    f.dimension = 3;
    CHECK(f.info().depth == 6);
}

// Each of these is laid out in guest memory as the hand computation says and
// read back.
void test_decode_linear_rgba8() {
    GuestMemory m;
    Fetch f;
    f.format = 6, f.width = 40, f.height = 16, f.pitch = 64, f.tiled = false, f.endian = 2, f.base = 0x100000;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 40; x++) put_le32(m, 0x100000 + (x + 64 * y) * 4, bswap(hash(x + 100 * y)));
    texture::Image img;
    std::string why;
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.format == HostFormat::RGBA8 && img.levels.size() == 1 && img.levels[0].size() == 40 * 16 * 4);
    bool same = true;
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 40; x++) {
            uint32_t v;
            std::memcpy(&v, img.levels[0].data() + (y * 40 + x) * 4, 4);
            same &= v == hash(x + 100 * y);
        }
    CHECK(same);
    // the memory it read: the rows from the first texel to the last
    CHECK(img.footprint.size() == 1);
    CHECK(img.footprint[0].first == 0x100000 && img.footprint[0].second == (64 * 15 + 40) * 4);
}

void test_decode_tiled_8() {
    GuestMemory m;
    Fetch f;
    f.format = 2, f.width = 64, f.height = 64, f.pitch = 64, f.base = 0x200000;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t b = uint8_t(hash(x * 64 + y));
            m.write(0x200000 + texture::tiled_offset_2d(x, y, 64, 0), {&b, 1});
        }
    texture::Image img;
    std::string why;
    CHECK(texture::decode(f.info(), m, img, why));
    bool same = img.format == HostFormat::R8 && img.levels[0].size() == 64 * 64;
    for (uint32_t y = 0; y < 64 && same; y++)
        for (uint32_t x = 0; x < 64; x++) same &= img.levels[0][y * 64 + x] == uint8_t(hash(x * 64 + y));
    CHECK(same);
    // the memory it read: the 4 KB pages of its tiles, every texel's tiled
    // offset inside
    CHECK(img.footprint.size() == 1 && img.footprint[0].first == 0x200000);
    uint32_t last = 0;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) last = std::max(last, texture::tiled_offset_2d(x, y, 64, 0));
    CHECK(img.footprint[0].second == 4096 && last < 4096);
}

// The spans a plan gives each slice hold every block of it, and are the pages
// of its tiles: for 2D and 3D tiling at every block size, with and without a
// packed tail's offset, on sizes that are not multiples of a tile.
void test_plan_spans() {
    bool held = true, pages = true;
    uint32_t checked = 0;
    for (uint32_t format : {2u, 10u, 6u, 26u, 38u, 18u, 20u}) // 1, 2, 4, 8, 16 bytes; DXT1 8, DXT4_5 16 per block
        for (uint32_t dimension : {1u, 2u})
            for (uint32_t size : {7u, 32u, 70u, 200u}) {
                Fetch f;
                f.format = format, f.dimension = dimension, f.width = size, f.height = size / 2 + 1;
                f.depth = dimension == 2 ? 5 : 1, f.pitch = (size + 31) & ~31u, f.base = 0x800000;
                f.mip_max = 4, f.mip_base = 0x900000, f.packed = true;
                texture::Plan p;
                std::string why;
                if (!texture::plan(f.info(), p, why)) {
                    held = false;
                    continue;
                }
                for (const texture::SliceLoad &s : p.slices) {
                    uint64_t low = UINT64_MAX, high = 0;
                    for (uint32_t y = 0; y < s.blocks_y; y++)
                        for (uint32_t x = 0; x < s.blocks_x; x++) {
                            uint64_t o = s.offset(x, y);
                            low = std::min(low, o), high = std::max(high, o + (1u << s.log2_bytes_per_block));
                        }
                    held &= s.low <= low && high <= s.high;
                    // no more than the pages of the blocks' tiles: at most a
                    // tile and a page beyond the blocks at either end
                    uint64_t tile = uint64_t(s.is_3d ? 32 * 16 * 4 : 32 * 32) << s.log2_bytes_per_block;
                    uint64_t slack = s.tiled ? tile + 4096 : 0;
                    pages &= (!s.tiled || (s.low % 4096 == 0 && s.high % 4096 == 0)) && low - s.low <= slack &&
                             s.high - high <= slack;
                    checked++;
                }
            }
    CHECK(held);
    CHECK(pages);
    CHECK(checked > 100);
}

void test_decode_tiled_dxt1() {
    // 64x64 DXT1: 16x16 blocks of 8 bytes, the pitch aligned to 32 blocks;
    // 8in16 swaps the bytes of each 16-bit half
    GuestMemory m;
    Fetch f;
    f.format = 18, f.width = 64, f.height = 64, f.pitch = 64, f.endian = 1, f.base = 0x300000;
    for (uint32_t by = 0; by < 16; by++)
        for (uint32_t bx = 0; bx < 16; bx++) {
            uint8_t b[8];
            for (int i = 0; i < 8; i++) b[i ^ 1] = uint8_t(hash(by * 16 + bx) >> (i % 4 * 8)) + uint8_t(i);
            m.write(0x300000 + texture::tiled_offset_2d(bx, by, 32, 3), b);
        }
    texture::Image img;
    std::string why;
    CHECK(texture::decode(f.info(), m, img, why));
    bool same = img.format == HostFormat::BC1 && img.levels[0].size() == 16 * 16 * 8;
    for (uint32_t by = 0; by < 16 && same; by++)
        for (uint32_t bx = 0; bx < 16; bx++)
            for (int i = 0; i < 8; i++)
                same &= img.levels[0][(by * 16 + bx) * 8 + i] == uint8_t(uint8_t(hash(by * 16 + bx) >> (i % 4 * 8)) + i);
    CHECK(same);
    // and decompressed: one block by hand, color0 > color1 (four colors)
    GuestMemory one;
    f.width = f.height = 4, f.pitch = 32, f.endian = 0;
    const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0}; // red, blue; row 0: codes 0 1 2 3
    one.write(0x300000, block);
    CHECK(texture::decode(f.info(), one, img, why));
    std::vector<uint8_t> rgba = texture::to_rgba8(img);
    CHECK(rgba.size() == 64);
    CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255); // red
    CHECK(rgba[4] == 0 && rgba[6] == 255);                                   // blue
    CHECK(rgba[8] == 170 && rgba[10] == 85);                                 // 2:1
    CHECK(rgba[12] == 85 && rgba[14] == 170);                                // 1:2
}

void test_decode_3d_and_cube() {
    GuestMemory m;
    // 3D 32x32x8, 32 bits: slices of 32 rows, 4 at a time in a tile
    Fetch f;
    f.format = 6, f.dimension = 2, f.width = 32, f.height = 32, f.depth = 8, f.pitch = 32, f.base = 0x400000;
    for (uint32_t z = 0; z < 8; z++)
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++)
                put_le32(m, 0x400000 + uint32_t(texture::tiled_offset_3d(x, y, z, 32, 32, 2)), hash(x | y << 8 | z << 16));
    texture::Image img;
    std::string why;
    CHECK(texture::decode(f.info(), m, img, why));
    bool same = img.levels[0].size() == 32 * 32 * 8 * 4;
    for (uint32_t z = 0; z < 8 && same; z++)
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++) {
                uint32_t v;
                std::memcpy(&v, img.levels[0].data() + ((z * 32 + y) * 32 + x) * 4, 4);
                same &= v == hash(x | y << 8 | z << 16);
            }
    CHECK(same);

    // a cube 32x32: six faces, each a 4 KB slice
    Fetch c;
    c.format = 6, c.dimension = 3, c.width = 32, c.height = 32, c.pitch = 32, c.base = 0x500000;
    for (uint32_t face = 0; face < 6; face++)
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++)
                put_le32(m, 0x500000 + face * 4096 + texture::tiled_offset_2d(x, y, 32, 2), hash(face * 7777 + y * 32 + x));
    CHECK(texture::decode(c.info(), m, img, why));
    same = img.depth == 6 && img.levels[0].size() == 6 * 32 * 32 * 4;
    for (uint32_t face = 0; face < 6 && same; face++)
        for (uint32_t i = 0; i < 32 * 32; i++) {
            uint32_t v;
            std::memcpy(&v, img.levels[0].data() + (face * 1024 + i) * 4, 4);
            same &= v == hash(face * 7777 + i);
        }
    CHECK(same);
}

void test_decode_mips() {
    // 64x64 tiled 32-bit with mips to 1x1, packed: level 1 at the mip address
    // (32 blocks a row, 4 KB), levels 2-6 together in the next tile, where
    // packed_mip_offset says (worked out by hand above for 16x16)
    GuestMemory m;
    Fetch f;
    f.format = 6, f.width = 64, f.height = 64, f.pitch = 64, f.base = 0x600000, f.mip_base = 0x700000;
    f.packed = true, f.mip_max = 6;
    auto value = [](uint32_t level, uint32_t x, uint32_t y) { return hash(level << 24 | y << 12 | x); };
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) put_le32(m, 0x600000 + texture::tiled_offset_2d(x, y, 64, 2), value(0, x, y));
    const uint32_t ox[7] = {0, 0, 16, 8, 4, 0, 0}, oy[7] = {0, 0, 0, 0, 0, 8, 4};
    for (uint32_t level = 1; level <= 6; level++) {
        uint32_t size = 64 >> level, base = 0x700000 + (level == 1 ? 0 : 4096);
        for (uint32_t y = 0; y < size; y++)
            for (uint32_t x = 0; x < size; x++)
                put_le32(m, base + texture::tiled_offset_2d(ox[level] + x, oy[level] + y, 32, 2), value(level, x, y));
    }
    texture::Image img;
    std::string why;
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels.size() == 7);
    for (uint32_t level = 0; level < img.levels.size(); level++) {
        uint32_t size = 64 >> level;
        bool same = img.levels[level].size() == size * size * 4;
        for (uint32_t y = 0; y < size && same; y++)
            for (uint32_t x = 0; x < size; x++) {
                uint32_t v;
                std::memcpy(&v, img.levels[level].data() + (y * size + x) * 4, 4);
                same &= v == value(level, x, y);
            }
        CHECK(same);
    }
    // the levels the constant gives
    texture::Levels l = texture::levels(f.info());
    CHECK(l.base && l.mips && l.mip_min == 0 && l.mip_max == 6);
    f.base = 0; // mips alone: from level 1
    l = texture::levels(f.info());
    CHECK(!l.base && l.mips && l.mip_min == 1);
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels.size() == 7 && img.levels[0].empty() && img.levels[1].size() == 32 * 32 * 4);
}

void test_endian() {
    GuestMemory m;
    Fetch f;
    f.format = 10, f.width = 2, f.height = 1, f.pitch = 32, f.tiled = false, f.base = 0x800000; // 8_8
    const uint8_t guest[4] = {0x12, 0x34, 0x56, 0x78};
    m.write(0x800000, guest);
    texture::Image img;
    std::string why;
    f.endian = 1; // 8in16: each 16-bit texel's bytes swapped
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x34, 0x12, 0x78, 0x56}));
    f.endian = 2; // 8in32 does nothing to 16-bit texels (endian.xesli's XeEndianSwap16)
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x12, 0x34, 0x56, 0x78}));
    f.format = 6, f.width = 1; // 32 bits
    f.endian = 3;              // 16in32: the halves swapped
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x56, 0x78, 0x12, 0x34}));
    f.endian = 2; // 8in32
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}));
    f.endian = 1; // 8in16 on 32 bits
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x34, 0x12, 0x78, 0x56}));
    f.format = 2; // 8 bits: never swapped
    CHECK(texture::decode(f.info(), m, img, why));
    CHECK(img.levels[0] == std::vector<uint8_t>({0x12}));
}

// The registers of a resolve of the rectangle (x0, y0)-(x1, y1) of color
// target 0, Direct3D's way: three vertices in vertex fetch constant 0.
struct ResolveSetup {
    std::vector<uint32_t> regs = std::vector<uint32_t>(xenos::REGISTER_COUNT);
    GuestMemory memory;

    ResolveSetup(float x0, float y0, float x1, float y1, uint32_t source_format, uint32_t dest_format,
                 uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian, uint32_t number = 0, int32_t bias = 0,
                 bool swap = false) {
        using namespace xenos::reg;
        regs[RB_SURFACE_INFO] = 1280;
        regs[RB_COLOR_INFO] = 0x40 | (source_format << 16);
        regs[RB_COPY_CONTROL] = 0;
        regs[RB_COPY_DEST_BASE] = dest_base;
        regs[RB_COPY_DEST_PITCH] = dest_pitch | (dest_pitch << 16);
        regs[RB_COPY_DEST_INFO] = dest_endian | (dest_format << 7) | (number << 13) | ((uint32_t(bias) & 0x3F) << 16) |
                                  (uint32_t(swap) << 24);
        regs[PA_SC_WINDOW_SCISSOR_TL] = 0x80000000; // no window offset
        regs[PA_SC_WINDOW_SCISSOR_BR] = 8191 | (8191u << 16);
        regs[PA_SC_SCREEN_SCISSOR_BR] = 8192 | (8192u << 16);
        regs[PA_SU_VTX_CNTL] = 0; // Direct3D's pixel centers: the vertices are half a pixel up and left
        regs[FETCH_CONSTANTS] = 0x00F00000 | 3;
        regs[FETCH_CONSTANTS + 1] = (6u << 2) | 2; // 6 words, 8in32
        const float v[6] = {x0 - 0.5f, y0 - 0.5f, x1 - 0.5f, y0 - 0.5f, x1 - 0.5f, y1 - 0.5f};
        for (int i = 0; i < 6; i++) memory.put_be32(0x00F00000 + 4 * i, std::bit_cast<uint32_t>(v[i]));
    }
};

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = uint8_t(hash(uint32_t(i) * 2654435761u + seed));
    return v;
}

bool resolve_and_read(ResolveSetup &s, std::span<const uint8_t> pixels, const Fetch &fetch, texture::Image &img) {
    resolve::Info info;
    std::string error;
    if (!resolve::get_info(s.regs, s.memory, info, error) || !info.copy) return false;
    if (!resolve::write(info, pixels, s.memory, error)) return false;
    return texture::decode(fetch.info(), s.memory, img, error);
}

void test_resolve_info() {
    // a rectangle off the corner, into a k_8 destination whose base is a 1 KB
    // (8-bit 32x32) tile into its 4 KB page
    ResolveSetup s(192, 160, 256, 192, 0, 2, 0x1000000 + 1024, 512, 0);
    s.regs[xenos::reg::RB_COPY_CONTROL] = (1u << 8) | (1u << 9); // and clear color and depth
    s.regs[xenos::reg::RB_COLOR_CLEAR] = 0x11223344;
    resolve::Info info;
    std::string error;
    CHECK(resolve::get_info(s.regs, s.memory, info, error));
    CHECK(info.copy && !info.depth && info.color_target == 0 && info.edram_base == 0x40);
    CHECK(info.x == 192 && info.y == 160 && info.width == 64 && info.height == 32);
    CHECK(info.surface_pitch == 1280 && info.msaa == 0 && info.source_format == 0);
    CHECK(info.dest_format == 2 && info.dest_pitch == 512 && info.dest_height == 512);
    // x 192 + 32 (the phase); 8-bit tiles are moved to the base 128 at a time
    CHECK(info.dest_x == 96 && info.dest_y == 32);
    CHECK(info.dest_base == 0x1000000 + texture::tiled_offset_2d(128, 128, 512, 0));
    CHECK(info.clear_color && info.clear_depth && info.clear_color_value[0] == 0x11223344);

    // written: red, at (192 + 32 + i, 160 + j) of the texture at the page
    std::vector<uint8_t> pixels = random_bytes(64 * 32 * 4, 1);
    CHECK(resolve::write(info, pixels, s.memory, error));
    Fetch f;
    f.format = 2, f.width = 512, f.height = 512, f.pitch = 512, f.base = 0x1000000;
    texture::Image img;
    CHECK(texture::decode(f.info(), s.memory, img, error));
    bool same = true;
    for (uint32_t j = 0; j < 32; j++)
        for (uint32_t i = 0; i < 64; i++) same &= img.levels[0][(160 + j) * 512 + 224 + i] == pixels[(j * 64 + i) * 4];
    CHECK(same);

    // outside the scissor: nothing at all
    ResolveSetup empty(0, 0, 64, 64, 0, 6, 0x1000000, 64, 0);
    empty.regs[xenos::reg::PA_SC_WINDOW_SCISSOR_TL] = 0x80000000 | 128 | (128u << 16);
    empty.regs[xenos::reg::RB_COPY_CONTROL] = 1u << 8;
    CHECK(resolve::get_info(empty.regs, empty.memory, info, error));
    CHECK(!info.copy && info.width == 0 && !info.clear_color);
    // unaligned: expanded to 8
    ResolveSetup odd(3, 5, 61, 30, 0, 6, 0x1000000, 64, 0);
    CHECK(resolve::get_info(odd.regs, odd.memory, info, error));
    CHECK(info.x == 0 && info.y == 0 && info.width == 64 && info.height == 32);
}

// The ranges a copy writes (resolve::destination_ranges) hold every byte of
// every texel it writes, and the bytes they hold besides lie in the partial
// ranges: for texels of 1 to 16 bytes, 2D and 3D, rectangles inside a tile,
// across tiles and at offsets within the first.
void test_resolve_destination_ranges() {
    struct Rect {
        uint32_t x, y, width, height, pitch;
    };
    const Rect rects[] = {{0, 0, 64, 32, 64}, {8, 16, 40, 24, 64}, {0, 0, 1280, 720, 1280}, {24, 8, 8, 8, 32},
                          {0, 0, 48, 40, 160}, {16, 0, 96, 64, 128}};
    bool held = true, rest_partial = true, some_full = false;
    for (uint32_t format : {6u, 26u, 38u, 2u, 4u}) // 4, 8, 16, 1 and 2 bytes per texel
        for (bool array : {false, true})
            for (const Rect &r : rects) {
                resolve::Info info;
                info.copy = true, info.width = r.width, info.height = r.height, info.dest_x = r.x, info.dest_y = r.y;
                info.dest_pitch = r.pitch, info.dest_height = (r.y + r.height + 31) & ~31u;
                info.dest_format = format, info.dest_array = array, info.dest_slice = array ? 2 : 0;
                info.dest_base = 0x1000000;
                std::vector<std::pair<uint32_t, uint32_t>> written, partial;
                CHECK(resolve::destination_ranges(info, written, partial));
                if (written.empty()) {
                    held = false;
                    continue;
                }
                uint32_t bytes = texture::format_block(format).bytes, log2 = uint32_t(std::countr_zero(bytes));
                uint32_t low = written.front().first, span = written.back().second - low;
                std::vector<uint8_t> in_written(span), in_partial(span), texel(span);
                for (auto [s, e] : written) std::fill(in_written.begin() + (s - low), in_written.begin() + (e - low), 1);
                for (auto [s, e] : partial)
                    if (s >= low && e <= low + span)
                        std::fill(in_partial.begin() + (s - low), in_partial.begin() + (e - low), 1);
                for (uint32_t j = 0; j < r.height; j++)
                    for (uint32_t i = 0; i < r.width; i++) {
                        uint32_t x = r.x + i, y = r.y + j;
                        uint64_t o = array ? texture::tiled_offset_3d(x, y, 2, r.pitch, info.dest_height, log2)
                                           : texture::tiled_offset_2d(x, y, r.pitch, log2);
                        uint64_t at = info.dest_base + o;
                        if (at < low || at + bytes > uint64_t(low) + span) {
                            held = false;
                            continue;
                        }
                        for (uint32_t b = 0; b < bytes; b++) {
                            held &= in_written[at - low + b] != 0;
                            texel[at - low + b] = 1;
                        }
                    }
                for (uint32_t k = 0; k < span; k++) {
                    if (in_written[k] && !texel[k]) rest_partial &= in_partial[k] != 0;
                    some_full |= in_written[k] && !in_partial[k];
                }
            }
    CHECK(held);
    CHECK(rest_partial);
    CHECK(some_full); // whole tiles are not partial
}

void test_resolve_round_trips() {
    texture::Image img;
    // 8_8_8_8 to 8_8_8_8 8in32: the bits as they were
    {
        ResolveSetup s(0, 0, 64, 32, 0, 6, 0x2000000, 64, 2);
        std::vector<uint8_t> pixels = random_bytes(64 * 32 * 4, 2);
        Fetch f;
        f.format = 6, f.width = 64, f.height = 32, f.pitch = 64, f.endian = 2, f.base = 0x2000000;
        CHECK(resolve_and_read(s, pixels, f, img) && img.levels[0] == pixels);
        // red and blue swapped
        ResolveSetup sw(0, 0, 64, 32, 0, 6, 0x2000000, 64, 2, 0, 0, true);
        CHECK(resolve_and_read(sw, pixels, f, img));
        bool swapped = img.levels[0].size() == pixels.size();
        for (size_t i = 0; i < pixels.size() && swapped; i += 4)
            swapped &= img.levels[0][i] == pixels[i + 2] && img.levels[0][i + 2] == pixels[i] &&
                       img.levels[0][i + 1] == pixels[i + 1] && img.levels[0][i + 3] == pixels[i + 3];
        CHECK(swapped);
        // exponent bias -1: converted, each component halved
        ResolveSetup half(0, 0, 64, 32, 0, 6, 0x2000000, 64, 2, 0, -1);
        CHECK(resolve_and_read(half, pixels, f, img));
        bool halved = img.levels[0].size() == pixels.size();
        for (size_t i = 0; i < pixels.size() && halved; i++)
            halved &= img.levels[0][i] == uint8_t(float(pixels[i]) / 255.0f * 0.5f * 255.0f + 0.5f);
        CHECK(halved);
    }
    // 8_8_8_8 to 5_6_5 8in16: packed as XePackR5G6B5 rounds
    {
        ResolveSetup s(0, 0, 32, 32, 0, 4, 0x2100000, 32, 1);
        std::vector<uint8_t> pixels = random_bytes(32 * 32 * 4, 3);
        Fetch f;
        f.format = 4, f.width = 32, f.height = 32, f.pitch = 32, f.endian = 1, f.base = 0x2100000;
        CHECK(resolve_and_read(s, pixels, f, img));
        // (read back as R5G6B5, red in the high bits)
        bool same = img.format == HostFormat::R5G6B5 && img.levels[0].size() == 32 * 32 * 2;
        auto q = [](uint8_t v, uint32_t bits) { return uint32_t(float(v) / 255.0f * float((1u << bits) - 1) + 0.5f); };
        for (uint32_t i = 0; i < 32 * 32 && same; i++) {
            uint32_t v = img.levels[0][2 * i] | img.levels[0][2 * i + 1] << 8;
            same &= v == (q(pixels[4 * i], 5) << 11 | q(pixels[4 * i + 1], 6) << 5 | q(pixels[4 * i + 2], 5));
        }
        CHECK(same);
    }
    // 16_16_16_16_FLOAT to 16_16_16_16_FLOAT, float numbers, 8in32
    {
        ResolveSetup s(0, 0, 32, 16, 7, 32, 0x2200000, 32, 2, 7);
        std::vector<uint8_t> pixels = random_bytes(32 * 16 * 8, 4);
        Fetch f;
        f.format = 32, f.width = 32, f.height = 16, f.pitch = 32, f.endian = 2, f.base = 0x2200000;
        CHECK(resolve_and_read(s, pixels, f, img) && img.format == HostFormat::RGBA16F && img.levels[0] == pixels);
    }
    // 32_FLOAT to 32_FLOAT
    {
        ResolveSetup s(0, 0, 32, 32, 14, 36, 0x2300000, 32, 2, 7);
        std::vector<uint8_t> pixels = random_bytes(32 * 32 * 4, 5);
        Fetch f;
        f.format = 36, f.width = 32, f.height = 32, f.pitch = 32, f.endian = 2, f.base = 0x2300000;
        CHECK(resolve_and_read(s, pixels, f, img) && img.format == HostFormat::R32F && img.levels[0] == pixels);
    }
    // 8_8_8_8 to 16_16_16_16_FLOAT: converted to halves
    {
        ResolveSetup s(0, 0, 8, 8, 0, 32, 0x2400000, 32, 0, 7);
        std::vector<uint8_t> pixels = random_bytes(8 * 8 * 4, 6);
        Fetch f;
        f.format = 32, f.width = 8, f.height = 8, f.pitch = 32, f.base = 0x2400000;
        CHECK(resolve_and_read(s, pixels, f, img));
        bool close = img.levels[0].size() == 8 * 8 * 8;
        std::vector<uint8_t> rgba = texture::to_rgba8(img);
        for (size_t i = 0; i < rgba.size() && close; i++) close &= std::abs(int(rgba[i]) - int(pixels[i])) <= 1;
        CHECK(close);
    }
    // depth: the D24S8 words as they are, read as a 24_8 texture's floats
    // (XeUNorm24To32 of the high 24 bits)
    {
        ResolveSetup s(0, 0, 32, 8, 0, 6, 0x2500000, 32, 2);
        s.regs[xenos::reg::RB_COPY_CONTROL] = 4;
        s.regs[xenos::reg::RB_DEPTH_INFO] = 0x80;
        resolve::Info info;
        std::string error;
        CHECK(resolve::get_info(s.regs, s.memory, info, error));
        CHECK(info.depth && info.dest_format == 22 && info.edram_base == 0x80 && !info.dest_swap);
        std::vector<uint8_t> words = random_bytes(32 * 8 * 4, 7);
        Fetch f;
        f.format = 22, f.width = 32, f.height = 8, f.pitch = 32, f.endian = 2, f.base = 0x2500000;
        std::vector<uint8_t> floats(words.size());
        for (size_t i = 0; i < words.size(); i += 4) {
            uint32_t w, d24;
            std::memcpy(&w, &words[i], 4);
            d24 = w >> 8;
            float v = float(d24 + (d24 >> 23)) / 16777216.0f;
            std::memcpy(&floats[i], &v, 4);
        }
        CHECK(resolve_and_read(s, words, f, img) && img.format == HostFormat::R32F && img.levels[0] == floats);
    }
    // 8_8_8_8_GAMMA: decoded to linear by the piecewise curve, in 8 bits
    {
        ResolveSetup s(0, 0, 8, 8, 1, 6, 0x2600000, 32, 0);
        std::vector<uint8_t> pixels(8 * 8 * 4);
        for (size_t i = 0; i < pixels.size(); i++) pixels[i] = uint8_t(i * 4);
        Fetch f;
        f.format = 6, f.width = 8, f.height = 8, f.pitch = 32, f.base = 0x2600000;
        CHECK(resolve_and_read(s, pixels, f, img));
        // Codes 0-63 are linear 0-63 of 1023 (4 -> 4/1023 -> 1 in 8 bits),
        // 96-191 are 4c - 256 plus 4c/1024 (128 -> 257/1023 -> 64), 192-255
        // 8c - 1024 plus 8c/1024 (240 -> 903/1023 -> 225); alpha untouched.
        CHECK(img.levels[0][0] == 0 && img.levels[0][1] == 1 && img.levels[0][2] == 2 && img.levels[0][3] == 12);
        CHECK(img.levels[0][32] == 64);
        CHECK(img.levels[0][60] == 225);
    }
}

// A linear texture of the given bytes (endian 0) at 0x900000, decoded: its
// level 0, with the plan it took.
std::vector<uint8_t> decode_bytes(uint32_t format, uint32_t width, uint32_t height, const std::vector<uint8_t> &bytes,
                                  texture::Plan &plan, const texture::Support &support = {}, uint32_t signs = 0,
                                  bool signed_version = false) {
    GuestMemory m;
    m.write(0x900000, bytes);
    Fetch f;
    f.format = format, f.width = width, f.height = height, f.pitch = 32, f.tiled = false, f.base = 0x900000;
    f.signs = signs;
    texture::Image img;
    std::string why;
    if (!texture::plan(f.info(), plan, why, support, signed_version)) return {};
    if (!texture::decode(f.info(), m, img, why, support, signed_version)) return {};
    return img.levels[0];
}
std::vector<uint8_t> le16s(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> out;
    for (uint32_t v : values) out.push_back(uint8_t(v)), out.push_back(uint8_t(v >> 8));
    return out;
}
std::vector<uint8_t> le32s(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> out;
    for (uint32_t v : values)
        for (int k = 0; k < 4; k++) out.push_back(uint8_t(v >> (8 * k)));
    return out;
}

// The formats Xenia's texture caches convert, texel by texel against
// pixel_formats.xesli worked by hand; Xenia's host swizzles; the signed
// versions; the fallbacks of a host without the optional formats; and what
// stays unsupported, as in Xenia.
void test_formats() {
    using texture::Load, texture::Signed, texture::Half;
    texture::Plan p;
    // 16-bit packed (R 1, G 2, B 3, A 4 or 1): the host's packed formats
    CHECK(decode_bytes(4, 1, 1, le16s({1 | 2 << 5 | 3 << 11}), p) == le16s({1 << 11 | 2 << 5 | 3})); // 5_6_5
    CHECK(p.format == HostFormat::R5G6B5 && p.host_swizzle == texture::SWIZZLE_RGBB && p.signed_read == Signed::None);
    // 6_5_5: its blue in R5G6B5's green, its green in blue, read through RBGG
    CHECK(decode_bytes(5, 1, 1, le16s({1 | 2 << 5 | 3 << 10}), p) == le16s({1 << 11 | 3 << 5 | 2}));
    CHECK(p.format == HostFormat::R5G6B5 && p.host_swizzle == texture::SWIZZLE_RBGG);
    CHECK(decode_bytes(3, 1, 1, le16s({1 | 2 << 5 | 3 << 10 | 1 << 15}), p) == le16s({1 << 15 | 1 << 10 | 2 << 5 | 3}));
    CHECK(p.format == HostFormat::A1R5G5B5); // 1_5_5_5
    CHECK(decode_bytes(15, 1, 1, le16s({1 | 2 << 4 | 3 << 8 | 4 << 12}), p) == le16s({3 << 12 | 2 << 8 | 1 << 4 | 4}));
    CHECK(p.format == HostFormat::B4G4R4A4); // 4_4_4_4
    // 10_11_11 (red 11 bits low, blue 10 high) widened by replicating bits;
    // its signed version of its own: 0x7FF is -1/1023, 0x400 -1, blue 1/511
    uint32_t texel = 0x7FF | 0x400 << 11 | 1u << 22;
    CHECK(decode_bytes(16, 1, 1, le32s({texel}), p) == le16s({0xFFFF, 0x8010, 0x0040, 0xFFFF}));
    CHECK(p.format == HostFormat::RGBA16 && p.host_swizzle == texture::SWIZZLE_RGBB &&
          p.signed_read == Signed::Separate && !p.snorm);
    CHECK(decode_bytes(16, 1, 1, le32s({texel}), p, {}, 0x55, true) == le16s({0xFFE0, 0x8001, 0x0040, 0x7FFF}));
    CHECK(p.format == HostFormat::RGBA16 && p.snorm && p.load == Load::R11G11B10ToRGBA16Snorm);
    // 11_11_10 (red 10 bits low) and its _AS_16_16_16_16 twin
    CHECK(decode_bytes(17, 1, 1, le32s({0x3FF | 0x7FFu << 21}), p) == le16s({0xFFFF, 0, 0xFFFF, 0xFFFF}));
    CHECK(decode_bytes(56, 1, 1, le32s({0x3FF | 0x7FFu << 21}), p) == le16s({0xFFFF, 0, 0xFFFF, 0xFFFF}));
    // the packed 4:2:2 formats, 3 texels of two blocks: Cr to red, Y to green, Cb to blue and alpha
    std::vector<uint8_t> yuv = {0x10, 0x80, 0xEB, 0x40, 0x20, 0x81, 0x30, 0x41}; // Cr_Y1_Cb_Y0: Y0 Cb Y1 Cr
    CHECK(decode_bytes(11, 3, 1, yuv, p) == std::vector<uint8_t>({0x40, 0x10, 0x80, 0x80, 0x40, 0xEB, 0x80, 0x80,
                                                                   0x41, 0x20, 0x81, 0x81}));
    CHECK(p.format == HostFormat::RGBA8 && p.host_swizzle == texture::SWIZZLE_RGBB && p.signed_read == Signed::View);
    CHECK(decode_bytes(12, 3, 1, yuv, p) == // Y1_Cr_Y0_Cb: Cb Y0 Cr Y1
          std::vector<uint8_t>({0xEB, 0x80, 0x10, 0x10, 0xEB, 0x40, 0x10, 0x10, 0x30, 0x81, 0x20, 0x20}));
    // DXT3A_AS_1_1_1_1: a texel's 4 alpha bits are B4G4R4A4's red (bit 0),
    // green, blue and alpha (bit 3), each a full 4 bits
    std::vector<uint8_t> dxt3a = le32s({0xF681, 0});
    std::vector<uint8_t> argb4 = decode_bytes(61, 4, 1, dxt3a, p);
    CHECK(p.format == HostFormat::B4G4R4A4 && argb4.size() == 8);
    CHECK(argb4 == le16s({0x00F0, 0x000F, 0xFF00, 0xFFFF}));
    // 8_8_8_8_GAMMA_EDRAM is 8_8_8_8 (GetBaseFormat)
    CHECK(decode_bytes(62, 1, 1, {1, 2, 3, 4}, p) == std::vector<uint8_t>({1, 2, 3, 4}));
    CHECK(p.format == HostFormat::RGBA8 && p.signed_read == Signed::View);
    // the signed versions
    CHECK(!decode_bytes(2, 1, 1, {1}, p).empty() && p.signed_read == Signed::View);   // k_8: R8_SNORM
    CHECK(!decode_bytes(30, 1, 1, {0, 0}, p).empty() && p.signed_read == Signed::View); // 16_FLOAT: the same
    CHECK(!decode_bytes(18, 4, 4, le32s({0, 0}), p).empty() && p.signed_read == Signed::None); // DXT1: none
    CHECK(!decode_bytes(58, 4, 4, le32s({0, 0}), p).empty() && p.signed_read == Signed::None); // DXT3A: none

    // a host without the optional formats (XF_TEXTURE_FALLBACKS)
    texture::Support bare;
    for (HostFormat h : {HostFormat::BC1, HostFormat::BC2, HostFormat::BC3, HostFormat::BC4, HostFormat::BC5,
                         HostFormat::R16, HostFormat::RG16, HostFormat::RGBA16, HostFormat::R5G6B5,
                         HostFormat::A1R5G5B5, HostFormat::B4G4R4A4})
        bare.sampled &= ~(uint64_t(1) << uint32_t(h));
    bare.snorm = 0;
    // DXT1 decompressed to RGBA8 as the host's BC1 would be read
    std::vector<uint8_t> block = le32s({0xF800 | 0x001Fu << 16, 0xE4E4E4E4}); // red over blue, codes 0 1 2 3
    std::vector<uint8_t> rgba = decode_bytes(18, 4, 4, block, p, bare);
    CHECK(p.format == HostFormat::RGBA8 && p.load == Load::Dxt1ToRGBA8 && p.signed_read == Signed::None);
    texture::Image bc;
    bc.format = HostFormat::BC1, bc.width = 4, bc.height = 4, bc.levels = {block};
    CHECK(rgba == texture::to_rgba8(bc));
    CHECK(rgba.size() == 64 && rgba[0] == 255 && rgba[2] == 0 && rgba[4] == 0 && rgba[6] == 255); // red, blue
    CHECK(rgba[8] == 170 && rgba[10] == 85 && rgba[12] == 85 && rgba[14] == 170);                 // 2:1, 1:2
    // DXN to RG8, DXT5A to R8
    CHECK(decode_bytes(49, 4, 4, le32s({0x00FF, 0, 0xFF00, 0}), p, bare).size() == 32 && p.format == HostFormat::RG8);
    CHECK(decode_bytes(59, 4, 4, le32s({0x00FF, 0}), p, bare).size() == 16 && p.format == HostFormat::R8);
    // 16-bit unorm as halves; its signed version of its own, from snorm
    CHECK(decode_bytes(24, 3, 1, le16s({0xFFFF, 0x8000, 0}), p, bare) == le16s({0x3C00, 0x3800, 0}));
    CHECK(p.format == HostFormat::R16F && p.half == Half::FromUnorm && p.signed_read == Signed::Separate &&
          !p.approximate.empty());
    CHECK(decode_bytes(24, 3, 1, le16s({0x7FFF, 0x8000, 0x8001}), p, bare, 0x55, true) ==
          le16s({0x3C00, 0xBC00, 0xBC00}));
    CHECK(p.format == HostFormat::R16F && p.half == Half::FromSnorm);
    // and with 16-bit snorm left: the signed version is R16_SNORM of the same bits
    texture::Support snorm16 = bare;
    snorm16.snorm = uint64_t(1) << uint32_t(HostFormat::R16);
    CHECK(decode_bytes(24, 1, 1, le16s({0x8001}), p, snorm16, 0x55, true) == le16s({0x8001}));
    CHECK(p.format == HostFormat::R16 && p.snorm && p.half == Half::None);
    // the packed 16-bit formats unpacked to RGBA8, bits replicated
    CHECK(decode_bytes(4, 1, 1, le16s({1 | 2 << 5 | 3 << 11}), p, bare) == std::vector<uint8_t>({8, 8, 24, 255}));
    CHECK(p.format == HostFormat::RGBA8 && p.host_swizzle == texture::SWIZZLE_RGBB);
    CHECK(decode_bytes(5, 1, 1, le16s({1 | 2 << 5 | 3 << 10}), p, bare) == std::vector<uint8_t>({8, 16, 12, 255}));
    CHECK(p.host_swizzle == texture::SWIZZLE_RGBB); // 6_5_5 unpacked in its own order
    CHECK(decode_bytes(3, 1, 1, le16s({1 << 15}), p, bare) == std::vector<uint8_t>({0, 0, 0, 255}));
    CHECK(decode_bytes(15, 1, 1, le16s({0x4321}), p, bare) == std::vector<uint8_t>({0x11, 0x22, 0x33, 0x44}));
    CHECK(decode_bytes(61, 4, 1, dxt3a, p, bare) ==
          std::vector<uint8_t>({255, 0, 0, 0, 0, 0, 0, 255, 0, 255, 255, 0, 255, 255, 255, 255}));
    // the 32-bit floats need no filtering; a sampler of them points
    texture::Support unfiltered;
    unfiltered.linear &= ~(uint64_t(1) << uint32_t(HostFormat::R32F));
    Fetch ff;
    ff.format = 36, ff.width = 4, ff.height = 4, ff.pitch = 32, ff.base = 0x900000;
    std::string reason;
    CHECK(texture::plan(ff.info(), p, reason, unfiltered) && !texture::filterable(ff.info(), unfiltered));
    CHECK(texture::filterable(ff.info(), {}));

    // the halves' conversion constants are the floats nearest 1/65535 and 1/32767
    CHECK(std::bit_cast<uint32_t>(1.0f / 65535.0f) == 0x37800080u);
    CHECK(std::bit_cast<uint32_t>(1.0f / 32767.0f) == 0x38000100u);
    // swizzles composed with the host format's
    CHECK(texture::compose_swizzle(texture::SWIZZLE_RGBA, texture::SWIZZLE_RGBB) == texture::SWIZZLE_RGBB);
    CHECK(texture::compose_swizzle(3 | 2 << 3 | 4 << 6 | 5 << 9, texture::SWIZZLE_RBGG) ==
          (1 | 1 << 3 | 4 << 6 | 5 << 9));

    // unsupported, as in Xenia: no host format for them
    for (uint32_t format :
         {0u, 1u, 13u, 21u, 33u, 34u, 35u, 39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u, 47u, 48u, 57u, 63u}) {
        Fetch f;
        f.format = format, f.width = 8, f.height = 8, f.pitch = 32;
        std::string why;
        CHECK(!texture::plan(f.info(), p, why) && why.rfind("format ", 0) == 0);
    }
    // nor tiled 1D textures, nor 1D ones with packed mips
    Fetch one;
    one.format = 6, one.dimension = 0, one.width = 64, one.base = 0x900000;
    std::string why;
    CHECK(!texture::plan(one.info(), p, why) && why == "tiled 1D");
    one.tiled = false, one.packed = true;
    CHECK(!texture::plan(one.info(), p, why) && why == "1D with packed mips");
    one.packed = false;
    CHECK(texture::plan(one.info(), p, why));
}

} // namespace

int main() {
    test_formats();
    test_tiled_addresses();
    test_packed_mips();
    test_parse();
    test_decode_linear_rgba8();
    test_decode_tiled_8();
    test_plan_spans();
    test_decode_tiled_dxt1();
    test_decode_3d_and_cube();
    test_decode_mips();
    test_endian();
    test_resolve_info();
    test_resolve_round_trips();
    test_resolve_destination_ranges();
    return finish("texture");
}
