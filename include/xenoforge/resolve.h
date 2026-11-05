// A resolve: the GPU copying a rectangle of a render target (EDRAM) into
// guest memory as a texture, and optionally clearing it. Here the registers
// are read into what the renderer needs (the source surface and rectangle,
// the clears), and the rectangle, read back from the host render target, is
// written into guest memory as the destination says: format conversion,
// exponent bias, red/blue swap, endianness, tiling (plan B3).

#pragma once

#include "xenoforge/guest_memory.h"
#include "xenoforge/texture.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xf::resolve {

struct Info {
    // the source, in EDRAM
    bool depth = false;          // the depth buffer, else color target color_target
    uint32_t color_target = 0;   // 0-3
    uint32_t edram_base = 0;     // in tiles, of the source surface
    uint32_t surface_pitch = 0;  // in pixels
    uint32_t msaa = 0;           // 0 1x, 1 2x, 2 4x
    uint32_t source_format = 0;  // a color target format, or the depth format
    uint32_t x = 0, y = 0, width = 0, height = 0; // the rectangle, in surface pixels
    // the destination, in guest memory
    uint32_t dest_base = 0, dest_pitch = 0, dest_height = 0; // address; pitch and height in texels
    uint32_t dest_format = 0;    // a texture format
    uint32_t dest_endian = 0;
    int32_t dest_exp_bias = 0;
    bool dest_swap = false;      // red and blue swapped
    uint32_t dest_x = 0, dest_y = 0, dest_slice = 0;
    uint32_t dest_number = 0;    // xenos.h's SurfaceNumberFormat: 0 unsigned fraction, 1 signed, 2/3 integer, 7 float
    bool dest_array = false;     // a 3D (array) destination, slice dest_slice
    bool copy = true;            // false: a clear alone
    // the clears done after the copy
    bool clear_color = false, clear_depth = false;
    uint32_t clear_color_value[2] = {}; // RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO
    uint32_t clear_depth_value = 0;     // RB_DEPTH_CLEAR
};

// From the registers at a draw in copy mode (RB_MODECONTROL = 6); the
// rectangle may come from the draw's vertices in guest memory. The
// destination is as Xenia's GetResolveInfo leaves it: dest_base moved to the
// 32x32 tile holding the rectangle's corner, dest_x and dest_y the rest;
// dest_pitch and dest_height aligned to 32. An empty rectangle is no copy and
// no clears; a destination format that cannot be resolved to is no copy, the
// clears still done, with error saying why (and true returned).
bool get_info(std::span<const uint32_t> registers, const GuestMemory &memory, Info &out, std::string &error);

// The host format the renderer keeps a color target format in, and so the
// format of the pixels it reads back; Unsupported if none yet. 8_8_8_8_GAMMA
// is RGBA8 holding the gamma-encoded bits, as the EDRAM does; 16_16 and
// 16_16_16_16 are RG16/RGBA16 holding the EDRAM's bits (signed, 10 fractional
// bits: -32 to 32); 2_10_10_10_FLOAT is RGBA16F.
texture::HostFormat host_format_of_color_target(uint32_t format);

// pixels: the rectangle, row by row, in host_format_of_color_target(source)
// (or D24S8 as 32-bit words, depth in the high 24 bits, for depth); each
// sample of an MSAA target already resolved to one.
bool write(const Info &info, std::span<const uint8_t> pixels, GuestMemory &memory, std::string &error);

// The copy keeps the source's bits (Xenia's fast resolve shaders): depth,
// or a color format bitwise equal to the destination's, unscaled.
bool raw_copy(const Info &info);

// Where a copy writes guest memory, as [start, end) ranges of physical
// memory in address order: written, the ranges its texels lie in; partial,
// those of them that also hold texels it does not write. With texels of 4
// bytes or more in 2D, a 32x32 tile is a run of memory of its own, so
// written is the runs of the rectangle's tiles, row by row, and partial the
// tiles the rectangle's edges cut. Smaller texels and 3D destinations: the
// span from the lowest texel to the highest, both. False when the
// destination format is not one write() writes.
bool destination_ranges(const Info &info, std::vector<std::pair<uint32_t, uint32_t>> &written,
                        std::vector<std::pair<uint32_t, uint32_t>> &partial);

} // namespace xf::resolve
