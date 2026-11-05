// Guest memory's tiled addressing and byte swaps, as texture.cpp and
// resolve.cpp do them on the CPU (Xenia's texture_address.h and
// endian.xesli). BSD; see LICENSE-xenia.

const uint PHYSICAL_MASK = 0x1FFFFFFFu;

uint tiled_combine(uint bytes, uint bank, uint pipe, uint y_lsb) {
    return (y_lsb << 4u) | (pipe << 6u) | (bank << 11u) | (bytes & 0xFu) | (((bytes >> 4u) & 1u) << 5u) |
           (((bytes >> 5u) & 7u) << 8u) | ((bytes >> 8u) << 12u);
}

// the byte offset of block (x, y) of a 2D surface, pitch in blocks
uint tiled_offset_2d(uint x, uint y, uint pitch, uint log2_bytes) {
    uint outer = ((y >> 5u) * (pitch >> 5u) + (x >> 5u)) << 6u;
    uint inner = (((y >> 1u) & 7u) << 3u) | (x & 7u);
    uint bytes = (outer | inner) << log2_bytes;
    uint bank = (y >> 4u) & 1u, pipe = ((x >> 3u) & 3u) ^ (((y >> 3u) & 1u) << 1u);
    return tiled_combine(bytes, bank, pipe, y & 1u);
}

// the byte offset of block (x, y, z) of a 3D surface, pitch and height in blocks
uint tiled_offset_3d(uint x, uint y, uint z, uint pitch, uint height, uint log2_bytes) {
    uint outer = ((((z >> 2u) * (height >> 4u) + (y >> 4u)) * (pitch >> 5u)) + (x >> 5u)) << 7u;
    uint inner = ((z & 3u) << 5u) | (((y >> 1u) & 3u) << 3u) | (x & 7u);
    uint bytes = (outer | inner) << log2_bytes;
    uint bank = ((y >> 3u) ^ (z >> 2u)) & 1u, pipe = ((x >> 3u) & 3u) ^ (bank << 1u);
    return tiled_combine(bytes, bank, pipe, y & 1u);
}

// endian.xesli on the little-endian words the GPU loads: 8in16, 8in32, 16in32
uint swap16(uint v, uint endian) {
    if (endian == 1u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
    return v;
}
uint swap32(uint v, uint endian) {
    if (endian == 1u || endian == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
    if (endian == 2u || endian == 3u) v = (v << 16u) | (v >> 16u);
    return v;
}
