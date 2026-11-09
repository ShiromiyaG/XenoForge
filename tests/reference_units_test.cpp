// The software reference renderer's pieces (reference.h's pieces) against
// values the documents give: the 7e3 and 20e4 formats (xenos.cc's), the
// piecewise linear gamma's pieces, the extended float16, the sample
// positions (R6xx/R7xx's PA_SC_AA_SAMPLE_LOCS), the top-left fill rule, the
// color formats' packing and the blender's equations (factors before min and
// max, as the Xenos blends).

#include "check.h"
#include "xenoforge/reference.h"

#include <cmath>
#include <cstdint>

using namespace xf::reference::pieces;

namespace {

void formats() {
    // 7e3: exponent bias 3, 7 mantissa bits, no infinity: 0x3FF is 31.875
    CHECK(float7e3_to_32(0x3FF) == 31.875f);
    CHECK(float7e3_to_32(0x180) == 1.0f);
    CHECK(float7e3_to_32(0x001) == 1.0f / 512.0f); // the smallest denormal: 2^-2 / 128
    CHECK(float32_to_7e3(1.0f) == 0x180);
    CHECK(float32_to_7e3(100.0f) == 0x3FF); // clamped
    CHECK(float32_to_7e3(-1.0f) == 0);
    CHECK(float32_to_7e3(std::nanf("")) == 0);
    for (uint32_t v = 0; v < 0x400; v++) CHECK(float32_to_7e3(float7e3_to_32(v)) == v);
    // 20e4: exponent bias 15, 20 mantissa bits; 1.0 is 15 << 20
    CHECK(float20e4_to_32(0xF00000) == 1.0f);
    CHECK(float20e4_to_32(0xE00000) == 0.5f);
    CHECK(float32_to_20e4(1.0f, true) == 0xF00000);
    CHECK(float32_to_20e4(0.5f, false) == 0xE00000);
    CHECK(float32_to_20e4(4.0f, true) == 0xFFFFFF); // clamped below 2
    for (uint32_t v : {0x000001u, 0x0FFFFFu, 0x100000u, 0x7ABCDEu, 0xF00000u, 0xFFFFFFu})
        CHECK(float32_to_20e4(float20e4_to_32(v), true) == v);
    // the 16-bit floats: IEEE rounding to the nearest even, and the Xenos's
    // exponent 31 finite (to 131008), NaN as 0
    CHECK(float_to_half(1.0f) == 0x3C00);
    CHECK(float_to_half(65504.0f) == 0x7BFF);
    CHECK(float_to_half(1.0f + 1.0f / 2048.0f) == 0x3C00); // a tie: to even
    CHECK(half_to_float(0x0001) == std::ldexp(1.0f, -24));
    CHECK(pack_half2_extended(131008.0f, 1.0f) == (0x7FFFu | 0x3C00u << 16));
    CHECK(pack_half2_extended(std::nanf(""), 1e9f) == (0x0000u | 0x7FFFu << 16));
    float x, y;
    unpack_half2_extended(0x7C00u | 0x7FFFu << 16, x, y);
    CHECK(x == 65536.0f && y == 131008.0f);
}

void gamma() {
    // the curve's pieces start at codes 64, 96 and 192: linear 64, 128 and
    // 512 + 4 (of 1023; the stepped term of the last piece's slope)
    CHECK(linear_to_pwl_gamma(0.0f) == 0.0f);
    CHECK(linear_to_pwl_gamma(1.0f) == 1.0f);
    CHECK(pwl_gamma_to_linear(1.0f) == 1.0f);
    CHECK(std::lround(pwl_gamma_to_linear(64.0f / 255.0f) * 1023.0f) == 64);
    CHECK(std::lround(pwl_gamma_to_linear(96.0f / 255.0f) * 1023.0f) == 128);
    CHECK(std::lround(pwl_gamma_to_linear(192.0f / 255.0f) * 1023.0f) == 516);
    // every code back through linear to itself or the one below: the
    // encoding truncates, and a code's linear value lies on its range's
    // lower edge (why Xenia's transfers take the range's midpoint)
    for (uint32_t c = 0; c < 256; c++) {
        long back = std::lround(linear_to_pwl_gamma(pwl_gamma_to_linear(float(c) / 255.0f)) * 255.0f);
        CHECK(back == long(c) || back == long(c) - 1);
    }
}

void samples_and_fill() {
    int32_t x, y;
    sample_position(0, 0, x, y);
    CHECK(x == 0 && y == 0);
    sample_position(1, 0, x, y);
    CHECK(x == -4 && y == 4);
    sample_position(1, 1, x, y);
    CHECK(x == 4 && y == -4);
    const int32_t four[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};
    for (uint32_t s = 0; s < 4; s++) {
        sample_position(2, s, x, y);
        CHECK(x == four[s][0] && y == four[s][1]);
    }
    // a triangle with its corners on pixel centers (1/256 pixel units): the
    // top-left corner's pixel in (on its top and left edges), the others out
    // (the hypotenuse is neither top nor left)
    const int64_t tx[3] = {128, 4 * 256 + 128, 128}, ty[3] = {128, 128, 4 * 256 + 128};
    CHECK(coverage(tx, ty, 0, 0, 0) == 1);
    CHECK(coverage(tx, ty, 4, 0, 0) == 0);
    CHECK(coverage(tx, ty, 0, 4, 0) == 0);
    CHECK(coverage(tx, ty, 1, 1, 0) == 1);
    // the same either way round
    const int64_t rx[3] = {tx[0], tx[2], tx[1]}, ry[3] = {ty[0], ty[2], ty[1]};
    CHECK(coverage(rx, ry, 0, 0, 0) == 1);
    CHECK(coverage(rx, ry, 4, 0, 0) == 0);
    // a square in two triangles, its diagonal through pixel centers: every
    // sample of every pixel inside covered exactly once, at 1x, 2x and 4x
    const int64_t ax[3] = {0, 8 * 256, 8 * 256}, ay[3] = {0, 0, 8 * 256};
    const int64_t bx[3] = {0, 8 * 256, 0}, by[3] = {0, 8 * 256, 8 * 256};
    for (uint32_t msaa = 0; msaa < 3; msaa++) {
        uint32_t all = msaa == 0 ? 1 : msaa == 1 ? 3 : 15;
        for (int32_t py = 0; py < 8; py++)
            for (int32_t px = 0; px < 8; px++) {
                uint32_t a = coverage(ax, ay, px, py, msaa), b = coverage(bx, by, px, py, msaa);
                CHECK((a & b) == 0);
                CHECK((a | b) == all);
            }
    }
    // the top-left rule's edges, clockwise on the screen (y down)
    CHECK(top_left(0, 0, 10, 0));  // a top edge
    CHECK(top_left(0, 10, 0, 0));  // a left edge, going up
    CHECK(!top_left(10, 0, 0, 0)); // a bottom edge
    CHECK(!top_left(0, 0, 0, 10)); // a right edge
}

void colors() {
    uint32_t out[2];
    const float one[4] = {1, 1, 1, 1};
    pack_color(one, 0, out);
    CHECK(out[0] == 0xFFFFFFFFu);
    const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    pack_color(half, 0, out);
    CHECK(out[0] == 0x80808080u); // rounded half up
    pack_color(one, 3, out);
    CHECK(out[0] == (0x180u | 0x180u << 10 | 0x180u << 20 | 3u << 30));
    // 16_16: -32 to 32 with 10 fraction bits
    const float fixed[4] = {1.0f, -32.0f, 0, 0};
    pack_color(fixed, 4, out);
    CHECK(out[0] == (0x0400u | 0x8000u << 16));
    float v[4];
    unpack_color(out, 4, v);
    CHECK(v[0] == 1.0f && v[1] == -32.0f && v[3] == 1.0f);
    // the gamma target: linear in, gamma codes out (alpha linear)
    const float linear[4] = {0, 0.5f, 1, 1};
    pack_color(linear, 1, out);
    CHECK((out[0] & 0xFF) == 0 && ((out[0] >> 16) & 0xFF) == 255 && (out[0] >> 24) == 255);
    CHECK(((out[0] >> 8) & 0xFF) == uint32_t(std::lround(linear_to_pwl_gamma(0.5f) * 255.0f)));

    // blending: source alpha over a black destination, alpha one-zero
    const float red[4] = {1, 0, 0, 0.5f};
    const uint32_t black[2] = {0xFF000000u, 0};
    const float constant[4] = {};
    blend(red, black, 0, 0x00010706u, constant, 0xF, out);
    CHECK(out[0] == 0x80000080u);
    // min takes the factors (the Xenos's, unlike Direct3D's): min(0.8 * 0.5, 0.6 * 1)
    const float source[4] = {0.8f, 0.8f, 0.8f, 0.5f};
    const uint32_t gray[2] = {0x00999999u, 0}; // 0.6
    blend(source, gray, 0, 0x00010146u, constant, 0xF, out);
    CHECK((out[0] & 0xFF) == 102);
    // the write mask keeps what it does not write
    blend(red, black, 0, 0x00010001u, constant, 0x1, out);
    CHECK(out[0] == 0xFF0000FFu);
    // reverse subtract, clamped at 0 for fixed point
    blend(red, black, 0, 0x00010181u, constant, 0xF, out); // dest - source, one and one
    CHECK((out[0] & 0xFF) == 0);
}

} // namespace

int main() {
    formats();
    gamma();
    samples_and_fill();
    colors();
    return finish("reference_units");
}
