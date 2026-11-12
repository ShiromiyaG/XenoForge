// A draw's guest indices to the host's, in one pass (the renderer's draw;
// docs/renderer.md, "The command thread's index work"): each index as the
// fetch's endianness puts it in host order, then the reset index kept as the
// host's restart index (UINT32_MAX) and the others, unless `remap` is off
// (adaptive tessellation's edge factors), offset, cut to 24 bits and clamped
// to VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX, as Xenia's vertex shaders do
// (RemapAndConvertVertexIndices). With AVX2 where the host has it, eight
// indices at a time; the same answer either way.

#pragma once

#include <cstdint>

namespace xf::render {

struct IndexConversion {
    bool index32 = false;
    uint32_t endian = 0; // gpu::Endian: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32 (16-bit indices: 1 swaps)
    bool restart = false;
    uint32_t reset = 0; // compared with the swapped index (16-bit indices: the low half)
    bool remap = true;
    uint32_t offset = 0, min = 0, max = 0xFFFFFF;
};

// in: count indices' bytes as guest memory holds them; out: count words
void convert_indices(const uint8_t *in, uint32_t *out, uint32_t count, const IndexConversion &c);
// the portable loop alone (for the check against the vector one)
void convert_indices_scalar(const uint8_t *in, uint32_t *out, uint32_t count, const IndexConversion &c);

} // namespace xf::render
