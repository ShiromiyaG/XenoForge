// The resolution scale's arithmetic (docs/renderer.md, "Resolution scale"):
// the console's pixels drawn as more (or fewer) host pixels, in steps of a
// quarter, 50% to 800%, as RPCS3's resolution scale steps by 25%. Xenia's
// draw_resolution_scale_x/y (whole multiples only) lays out the scaled EDRAM
// the same way: a tile of 80x16 32-bit samples becomes 80s x 16s, which is
// whole for every quarter s, as is every resolve rectangle (multiples of 8
// pixels) and every texel offset a resolve writes at (multiples of 8 and 32).
//
// Pure functions: the renderer and its shaders compute the same (edram.glsl,
// resolve.comp, edram_fsi.frag), and tests/render_scale_test.cpp checks them.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace xf::render {

struct Scale {
    uint32_t q4 = 4; // in quarters: 2 (50%) to 32 (800%)

    static constexpr uint32_t MIN_Q4 = 2, MAX_Q4 = 32;
    // a percentage to the nearest quarter, within 50-800%
    static constexpr Scale from_percent(int percent) {
        int q = (std::clamp(percent, 0, 100000) + 12) / 25;
        return Scale{uint32_t(std::clamp(q, int(MIN_Q4), int(MAX_Q4)))};
    }
    constexpr uint32_t percent() const { return q4 * 25; }
    constexpr bool one() const { return q4 == 4; }
    constexpr float factor() const { return float(q4) * 0.25f; }

    // a length of console pixels in host pixels: rounded down (offsets, and
    // lengths that are whole multiples of 4 pixels, which every quarter maps
    // exactly), or up (a surface's size, so the last console pixel has a host
    // pixel)
    constexpr uint32_t down(uint32_t pixels) const { return uint32_t(uint64_t(pixels) * q4 / 4); }
    constexpr uint32_t up(uint32_t pixels) const { return uint32_t((uint64_t(pixels) * q4 + 3) / 4); }

    // EDRAM at this scale: a tile's 32-bit samples across and down, and its
    // dwords (80x16 at 100%)
    constexpr uint32_t tile_width() const { return 20 * q4; }
    constexpr uint32_t tile_height() const { return 4 * q4; }
    constexpr uint32_t tile_dwords() const { return 80 * q4 * q4; }

    // The pixel of a grid at scale `to` whose center is nearest to (at or
    // just past) the center of pixel x of a grid at scale `from`: floor((x +
    // 0.5) * to / from). With from at 100% it is the host pixel a console
    // pixel's value is taken from (the representative a resolve writes guest
    // memory with); with to at 100%, the console pixel a host pixel lies in.
    static constexpr uint32_t map(uint32_t x, Scale from, Scale to) {
        return uint32_t((uint64_t(2 * x + 1) * to.q4) / (uint64_t(2) * from.q4));
    }
};

// RPCS3's minimum scalable dimension: a surface narrower or shorter than it
// (in console pixels) stays at the console's size
constexpr bool scalable(uint32_t width, uint32_t height, uint32_t min_dimension) {
    return width >= min_dimension && height >= min_dimension;
}

// An RGBA8 picture made another size, for comparing a scaled frame with the
// console's: each destination pixel the average of the source pixels whose
// centers lie in its footprint (a box filter), or the nearest source pixel
// where the footprint holds none (a picture made larger).
inline std::vector<uint8_t> resample_box(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
                                         uint32_t to_width, uint32_t to_height) {
    std::vector<uint8_t> out(size_t(to_width) * to_height * 4);
    for (uint32_t y = 0; y < to_height; y++) {
        // the source rows whose centers lie in [y, y + 1) * height / to_height
        uint32_t y0 = uint32_t((uint64_t(y) * height * 2 + to_height - 1) / (2ull * to_height));
        uint32_t y1 = uint32_t((uint64_t(y + 1) * height * 2 + to_height - 1) / (2ull * to_height));
        if (y1 <= y0) y0 = std::min(uint32_t(uint64_t(2 * y + 1) * height / (2ull * to_height)), height - 1), y1 = y0 + 1;
        for (uint32_t x = 0; x < to_width; x++) {
            uint32_t x0 = uint32_t((uint64_t(x) * width * 2 + to_width - 1) / (2ull * to_width));
            uint32_t x1 = uint32_t((uint64_t(x + 1) * width * 2 + to_width - 1) / (2ull * to_width));
            if (x1 <= x0) x0 = std::min(uint32_t(uint64_t(2 * x + 1) * width / (2ull * to_width)), width - 1), x1 = x0 + 1;
            uint32_t sum[4] = {};
            for (uint32_t sy = y0; sy < y1 && sy < height; sy++)
                for (uint32_t sx = x0; sx < x1 && sx < width; sx++)
                    for (int c = 0; c < 4; c++) sum[c] += rgba[(size_t(sy) * width + sx) * 4 + c];
            const uint32_t n = (std::min(y1, height) - y0) * (std::min(x1, width) - x0);
            for (int c = 0; c < 4; c++) out[(size_t(y) * to_width + x) * 4 + c] = uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}

} // namespace xf::render
