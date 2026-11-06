// The primitive types the renderer expands or tessellates (docs/renderer.md,
// "Pipelines, primitive types and occlusion queries"), drawn from packets
// built here as a game's would be: point lists (PA_SU_POINT_SIZE's size, the
// vertex shader's, indices from memory with an offset and the index clamp),
// rectangle lists (their three vertices in each order), and the tessellator's
// cases that Xenia draws: triangle and quad patches in the discrete,
// continuous and adaptive modes, and triangle lists, strips (with a reset
// index) and fans and quad lists indexed by control point. The shaders are
// microcode written here from Xenia's ucode.h, the domain shaders reading
// the tessellator's coordinates and indices where Xenia's translators put
// them. One frame of 4 by 4 cells, resolved and swapped.
//
// Drawn twice, with the geometry shaders and with the vertex shader's
// expansion of points and rectangles: the two must agree, and nothing may be
// skipped (where the device lacks tessellation shaders, the tessellated
// draws are, and only those). With XF_SYNTH_TRACES=<folder> the frame is
// also written there as a trace (58460001_1.xtr) for Xenia's trace dump;
// where its picture (58460001_1.png) is there too, the frame is compared
// with it.

#include "check.h"
#include "xenoforge/command_processor.h"
#include "xenoforge/gpu_trace_writer.h"
#include "xenoforge/memory.h"
#include "xenoforge/renderer.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

using namespace xf;
using namespace xf::gpu;
using xenos::pm4::type0;
using xenos::pm4::type3;
namespace pm4 = xenos::pm4;
namespace reg = xenos::reg;

namespace {

// registers xenos.h does not name (Xenia's register_table.inc)
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100; // then VGT_MIN_VTX_INDX and VGT_INDX_OFFSET
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103, PA_CL_VPORT_XSCALE = 0x210F;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181, SQ_INTERPOLATOR_CNTL = 0x2182;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204;
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206, PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281;
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284, VGT_HOS_CNTL = 0x2285, VGT_HOS_MAX_TESS_LEVEL = 0x2286,
                   VGT_HOS_MIN_TESS_LEVEL = 0x2287;

constexpr uint32_t SIZE = 320, CELL = 80; // the frame: 4 by 4 cells
constexpr uint32_t TITLE = 0x58460001;

// ---- microcode (Xenia's ucode.h: AluInstruction, ControlFlowExecInstruction) ----

struct Src {
    bool temp;
    uint32_t index;
    const char *swizzle;
};
Src r(uint32_t i, const char *s = "xyzw") { return {true, i, s}; }
Src c(uint32_t i, const char *s = "xyzw") { return {false, i, s}; }
// a temporary register, or an export: o0-o15, oPos, oPts (oC0-oC3 in a pixel shader)
struct Dst {
    bool exported;
    uint32_t index, mask;
};
Dst t(uint32_t i) { return {false, i, 0xF}; }
Dst e(uint32_t i, uint32_t mask = 0xF) { return {true, i, mask}; }
constexpr uint32_t POSITION = 62, POINT_SIZE = 63;
enum Op : uint32_t { ADD = 0, MUL = 1, MAX = 2, MAD = 11 };

// a swizzle's bits: each component relative to its own place
uint32_t swizzle(const char *s) {
    uint32_t v = 0;
    for (uint32_t j = 0; j < 4; j++) {
        uint32_t k = s[j] == 'x' ? 0 : s[j] == 'y' ? 1 : s[j] == 'z' ? 2 : 3;
        v |= ((k - j) & 3) << (2 * j);
    }
    return v;
}

// A vector instruction; its scalar half retains the previous result and
// writes nothing.
std::array<uint32_t, 3> alu(Op op, Dst d, std::initializer_list<Src> sources) {
    std::array<uint32_t, 3> w = {d.index | (d.exported ? 1u << 15 : 0u) | d.mask << 16 | 50u << 26, 0,
                                 uint32_t(op) << 24};
    uint32_t k = 0;
    for (const Src &s : sources) {
        uint32_t shift = 8 * (2 - k); // source 1 in bits 16-23
        w[1] |= swizzle(s.swizzle) << shift;
        w[2] |= s.index << shift | (s.temp ? 1u << (31 - k) : 0u);
        k++;
    }
    return w;
}

// ALU instructions only: one control flow pair of two execs (six
// instructions each at most), the instructions after it.
std::vector<uint32_t> program(const std::vector<std::array<uint32_t, 3>> &code) {
    uint32_t first = uint32_t(std::min<size_t>(code.size(), 6)), second = uint32_t(code.size()) - first;
    // exec (1) or exece (2): the address in instructions, the count
    uint32_t a0 = 1 | first << 12, a1 = (second ? 1u : 2u) << 12;
    uint32_t b0 = second ? (1 + first) | second << 12 : 0, b1 = second ? 2u << 12 : 0;
    std::vector<uint32_t> words = {a0, (a1 & 0xFFFF) | b0 << 16, b0 >> 16 | b1 << 16};
    for (const auto &i : code) words.insert(words.end(), i.begin(), i.end());
    return words;
}

// the domain shaders: position from three corners (c0-c2) or a corner and
// two sides, bowed by c3 times the product of two coordinates, moved by c5
// times the sum of the indices; the coordinates as the color, tinted by c4
// times the indices
std::vector<uint32_t> triangle_domain() {
    // r0.xyz the barycentric coordinates, r1.xyz the control points' (or
    // r1.x the patch's) indices
    return program({alu(MUL, t(2), {r(0, "xxxx"), c(0)}), alu(MAD, t(2), {r(0, "yyyy"), c(1), r(2)}),
                    alu(MAD, t(2), {r(0, "zzzz"), c(2), r(2)}), alu(MUL, t(3), {r(0, "xxxx"), r(0, "yyyy")}),
                    alu(MAD, t(2), {r(3), c(3), r(2)}), alu(ADD, t(3), {r(1, "xxxx"), r(1, "yyyy")}),
                    alu(ADD, t(3), {r(3), r(1, "zzzz")}), alu(MAD, e(POSITION), {r(3), c(5), r(2)}),
                    alu(MAD, e(0), {r(1), c(4), r(0)})});
}
std::vector<uint32_t> quad_patch_domain() {
    // r0.x the patch's index, r0.yz the UV coordinates
    return program({alu(MAD, t(2), {r(0, "yyyy"), c(1), c(0)}), alu(MAD, t(2), {r(0, "zzzz"), c(2), r(2)}),
                    alu(MUL, t(3), {r(0, "yyyy"), r(0, "zzzz")}), alu(MAD, t(2), {r(3), c(3), r(2)}),
                    alu(MAD, e(POSITION), {r(0, "xxxx"), c(5), r(2)}), alu(MAX, e(0), {r(0, "yzxw"), r(0, "yzxw")})});
}
std::vector<uint32_t> quad_cp_domain() {
    // r0.xy the UV coordinates, r0.z and r1.xyz the control points' indices
    return program({alu(MAD, t(2), {r(0, "xxxx"), c(1), c(0)}), alu(MAD, t(2), {r(0, "yyyy"), c(2), r(2)}),
                    alu(MUL, t(3), {r(0, "xxxx"), r(0, "yyyy")}), alu(MAD, t(2), {r(3), c(3), r(2)}),
                    alu(ADD, t(3), {r(0, "zzzz"), r(1, "xxxx")}), alu(ADD, t(3), {r(3), r(1, "yyyy")}),
                    alu(ADD, t(3), {r(3), r(1, "zzzz")}), alu(MAD, e(POSITION), {r(3), c(5), r(2)}),
                    alu(MAD, e(0), {r(1), c(4), r(0)})});
}
// points: at c0 + index * c1, colored c3 + index * c4, sized c2.x + index *
// c2.y where sized
std::vector<uint32_t> points(bool sized) {
    std::vector<std::array<uint32_t, 3>> code = {alu(MAD, e(POSITION), {r(0, "xxxx"), c(1), c(0)}),
                                                 alu(MAD, e(0), {r(0, "xxxx"), c(4), c(3)})};
    if (sized) code.push_back(alu(MAD, e(POINT_SIZE, 1), {r(0, "xxxx"), c(2, "yyyy"), c(2, "xxxx")}));
    return program(code);
}
// rectangles: vertex i at c0 + i * c1 + i * i * c2, colored c3 + i * c4
std::vector<uint32_t> rectangle() {
    return program({alu(MUL, t(1), {r(0, "xxxx"), r(0, "xxxx")}), alu(MAD, t(2), {r(0, "xxxx"), c(1), c(0)}),
                    alu(MAD, e(POSITION), {r(1), c(2), r(2)}), alu(MAD, e(0), {r(0, "xxxx"), c(4), c(3)})});
}
// the color: the first interpolator
std::vector<uint32_t> pixel() { return program({alu(MAX, e(0), {r(0), r(0)})}); }

// ---- the packets ----

struct Host : CommandProcessor::Host {
    void interrupt(uint32_t) override {}
    bool wait(uint32_t) override { return false; }
};

struct Stream {
    Memory &m;
    uint32_t at, n = 0; // physical address, words written
    void put(uint32_t physical, uint32_t v) const {
        uint8_t *p = m.translate_physical(physical);
        p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    }
    void w(std::initializer_list<uint32_t> words) {
        for (uint32_t v : words) put(at + 4 * n++, v);
    }
    void regs(uint32_t first, std::initializer_list<uint32_t> values) {
        w({type0(first, uint32_t(values.size()))});
        w(values);
    }
    void reg_float(uint32_t index, float v) { regs(index, {std::bit_cast<uint32_t>(v)}); }
    void shader(uint32_t type, const std::vector<uint32_t> &code) {
        w({type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(code.size())), type, uint32_t(code.size())});
        for (uint32_t v : code) w({v});
    }
    // the vertex shader's c0 on
    void constants(std::initializer_list<std::array<float, 4>> values) {
        w({type0(reg::ALU_CONSTANTS, 4 * uint32_t(values.size()))});
        for (const auto &v : values)
            for (float f : v) w({std::bit_cast<uint32_t>(f)});
    }
    // VGT_DRAW_INITIATOR: the primitive, auto indices (2) or memory's (0),
    // the explicit major mode, 32-bit indices, the count
    static uint32_t initiator(uint32_t primitive, bool auto_index, bool explicit_mode, bool index32, uint32_t count) {
        return primitive | (auto_index ? 2u : 0u) << 6 | (explicit_mode ? 1u : 0u) << 8 | (index32 ? 1u : 0u) << 11 |
               count << 16;
    }
    void draw_auto(uint32_t primitive, uint32_t count, bool explicit_mode = false) {
        w({type3(pm4::DRAW_INDX_2, 1), initiator(primitive, true, explicit_mode, false, count)});
    }
    // indices at a physical address, big-endian (8in16 or 8in32)
    void draw_indexed(uint32_t primitive, uint32_t count, uint32_t base, bool index32, bool explicit_mode = false) {
        w({type3(pm4::DRAW_INDX, 4), 0, initiator(primitive, false, explicit_mode, index32, count), base,
           count | (index32 ? 2u : 1u) << 30});
    }
};

// a cell's corner (left, top) in normalized device coordinates
std::array<float, 2> cell(uint32_t column, uint32_t row) {
    return {-1.0f + 0.5f * float(column) + 0.05f, 1.0f - 0.5f * float(row) - 0.05f};
}

uint32_t physical_alloc(Memory &m, uint32_t size) { return m.physical_address(m.system_alloc(size, 0x1000, true)); }

// The frame: 16 draws, then the resolve and the swap (after an empty frame,
// so the trace writer can record it as frame 1).
void build(Memory &m, Stream &s, uint32_t &draws) {
    uint32_t data = physical_alloc(m, 0x1000), front = physical_alloc(m, SIZE * SIZE * 4);
    draws = 0;
    s.w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, front, SIZE, SIZE});

    // a 320x320 8_8_8_8 target at EDRAM's start, no depth, no blending; the
    // viewport over it; Direct3D's pixel centers
    s.regs(reg::RB_SURFACE_INFO, {SIZE, 0, 0});
    s.regs(reg::RB_MODECONTROL, {4});
    s.regs(reg::RB_COLOR_MASK, {0xF});
    s.regs(reg::RB_DEPTHCONTROL, {0});
    s.regs(RB_BLENDCONTROL0, {0x00010001});
    s.regs(RB_COLORCONTROL, {0});
    s.regs(PA_CL_CLIP_CNTL, {0});
    s.regs(reg::PA_SU_SC_MODE_CNTL, {0});
    s.regs(PA_CL_VTE_CNTL, {0x43F});
    const float half = float(SIZE) / 2;
    s.regs(PA_CL_VPORT_XSCALE,
           {std::bit_cast<uint32_t>(half), std::bit_cast<uint32_t>(half), std::bit_cast<uint32_t>(-half),
            std::bit_cast<uint32_t>(half), std::bit_cast<uint32_t>(1.0f), 0});
    s.regs(reg::PA_SU_VTX_CNTL, {0});
    s.regs(reg::PA_SC_WINDOW_OFFSET, {0});
    s.regs(reg::PA_SC_WINDOW_SCISSOR_TL, {0x80000000, SIZE | SIZE << 16});
    s.regs(reg::PA_SC_SCREEN_SCISSOR_TL, {0, SIZE | SIZE << 16});
    s.regs(VGT_MAX_VTX_INDX, {0xFFFFFF, 0, 0});
    s.regs(reg::SQ_PROGRAM_CNTL, {3 | 0u << 8}); // r0-r3 in the vertex shaders, r0 in the pixel shader
    s.regs(SQ_CONTEXT_MISC, {0});
    s.regs(SQ_INTERPOLATOR_CNTL, {0});
    s.regs(VGT_OUTPUT_PATH_CNTL, {0});
    s.shader(1, pixel());

    // ---- points ----
    s.regs(PA_SU_POINT_SIZE, {64u << 16 | 32u}); // 8 pixels wide, 4 high (radii in 12.4)
    s.regs(PA_SU_POINT_MINMAX, {0xFFFFu << 16});
    float x = 0, y = 0;
    auto at_cell = [&](uint32_t column, uint32_t row) { x = cell(column, row)[0], y = cell(column, row)[1]; };
    at_cell(0, 0);
    s.shader(0, points(false));
    s.constants(
        {{x + 0.05f, y - 0.05f, 0, 1}, {0.1f, -0.1f, 0, 0}, {0, 0, 0, 0}, {1, 0.2f, 0.2f, 1}, {-0.2f, 0.2f, 0.25f, 0}});
    s.draw_auto(1, 4), draws++;
    at_cell(1, 0);
    s.shader(0, points(true));
    s.constants(
        {{x + 0.05f, y - 0.05f, 0, 1}, {0.1f, -0.1f, 0, 0}, {4, 3, 0, 0}, {0.2f, 1, 0.2f, 1}, {0.2f, -0.2f, 0.25f, 0}});
    s.draw_auto(1, 4), draws++;
    // indices from memory (16-bit), offset by 2 and clamped to 5: 5, 3, 4, 2, 5
    {
        uint32_t at = data;
        const uint16_t indices[] = {3, 1, 2, 0, 7, 0};
        for (uint32_t k = 0; k < 6; k += 2) s.put(at + 2 * k, uint32_t(indices[k]) << 16 | indices[k + 1]);
        at_cell(2, 0);
        s.constants({{x - 0.05f, y + 0.05f, 0, 1},
                     {0.06f, -0.06f, 0, 0},
                     {2, 1, 0, 0},
                     {0.2f, 0.2f, 1, 1},
                     {0.15f, 0.15f, -0.15f, 0}});
        s.regs(VGT_MAX_VTX_INDX, {5, 0, 2});
        s.draw_indexed(1, 5, at, false), draws++;
        s.regs(VGT_MAX_VTX_INDX, {0xFFFFFF, 0, 0});
    }

    // ---- rectangles: the corner, the top right, the bottom right ----
    s.shader(0, rectangle());
    auto rect = [&](uint32_t column, uint32_t row) {
        auto [x0, y0] = cell(column, row);
        float x1 = x0 + 0.4f, y1 = y0 - 0.3f;
        // c0 + i c1 + i^2 c2 through (x0, y0), (x1, y0), (x1, y1)
        float c2x = ((x1 - x0) - 2 * (x1 - x0)) / 2, c2y = ((y1 - y0) - 0) / 2;
        float c1x = (x1 - x0) - c2x, c1y = -c2y;
        s.constants(
            {{x0, y0, 0, 1}, {c1x, c1y, 0, 0}, {c2x, c2y, 0, 0}, {0.1f, 0.3f, 0.9f, 1}, {0.4f, 0.3f, -0.4f, 0}});
    };
    rect(3, 0);
    s.draw_auto(8, 3), draws++;
    // the same three vertices in the other orders, from memory (32-bit)
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t at = data + 0x100 + 0x20 * k;
        const uint32_t order[2][3] = {{1, 2, 0}, {2, 0, 1}};
        for (uint32_t j = 0; j < 3; j++) s.put(at + 4 * j, order[k][j]);
        rect(k, 1);
        s.draw_indexed(8, 3, at, true), draws++;
    }

    // ---- tessellation ----
    s.regs(VGT_OUTPUT_PATH_CNTL, {1});
    auto tess = [&](uint32_t mode, float max_level) {
        s.regs(VGT_HOS_CNTL, {mode});
        s.reg_float(VGT_HOS_MAX_TESS_LEVEL, max_level);
        s.reg_float(VGT_HOS_MIN_TESS_LEVEL, 0.0f);
    };
    // a triangle's corners, its bow, its tint and the move per index
    auto triangle = [&](uint32_t column, uint32_t row, float move) {
        auto [x0, y0] = cell(column, row);
        s.constants({{x0, y0, 0, 1},
                     {x0 + 0.3f, y0, 0, 1},
                     {x0, y0 - 0.3f, 0, 1},
                     {0.2f, -0.2f, 0, 0},
                     {0.04f, 0.02f, 0.01f, 0},
                     {move, -move, 0, 0}});
    };
    auto quad = [&](uint32_t column, uint32_t row, float move) {
        auto [x0, y0] = cell(column, row);
        s.constants({{x0, y0, 0, 1},
                     {0.3f, 0, 0, 0},
                     {0, -0.3f, 0, 0},
                     {0.15f, 0.1f, 0, 0},
                     {0.02f, 0.02f, 0.02f, 0},
                     {move, -move, 0, 0}});
    };
    s.shader(0, triangle_domain());
    tess(0, 3.0f), triangle(2, 1, 0.05f), s.draw_auto(0x11, 1), draws++;
    // some with faces culled (PA_SU_SC_MODE_CNTL: 1 front, 2 back), which
    // shows the tessellator's winding
    auto cull = [&](uint32_t faces) { s.regs(reg::PA_SU_SC_MODE_CNTL, {faces}); };
    cull(2), tess(1, 2.5f), triangle(3, 1, 0.05f), s.draw_auto(0x11, 1), draws++, cull(0);
    {
        // two patches' edge factors (32-bit floats in an index buffer)
        uint32_t at = data + 0x200;
        const float factors[] = {1, 3, 5, 2, 7, 0.5f};
        for (uint32_t k = 0; k < 6; k++) s.put(at + 4 * k, std::bit_cast<uint32_t>(factors[k]));
        tess(2, 14.0f), triangle(0, 2, 0.15f), s.draw_indexed(0x11, 6, at, true), draws++;
    }
    s.shader(0, quad_patch_domain());
    tess(0, 3.0f), quad(1, 2, 0.05f), s.draw_auto(0x12, 1), draws++;
    cull(2), tess(1, 4.5f), quad(2, 2, 0.05f), s.draw_auto(0x12, 1), draws++, cull(0);
    {
        uint32_t at = data + 0x300;
        const float factors[] = {1, 2, 3, 4};
        for (uint32_t k = 0; k < 4; k++) s.put(at + 4 * k, std::bit_cast<uint32_t>(factors[k]));
        tess(2, 14.0f), quad(3, 2, 0.05f), s.draw_indexed(0x12, 4, at, true), draws++;
    }
    // indexed by control point: two triangles of a list; a strip of two and
    // one of one, cut by the reset index (16-bit); a fan of three; two quads
    s.shader(0, triangle_domain());
    cull(2), tess(1, 2.0f), triangle(0, 3, 0.012f), s.draw_auto(4, 6, true), draws++, cull(0);
    {
        uint32_t at = data + 0x400;
        const uint16_t indices[] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6};
        for (uint32_t k = 0; k < 8; k += 2) s.put(at + 2 * k, uint32_t(indices[k]) << 16 | indices[k + 1]);
        s.regs(VGT_MULTI_PRIM_IB_RESET_INDX, {0xFFFF});
        s.regs(reg::PA_SU_SC_MODE_CNTL, {1u << 21});
        tess(0, 2.0f), triangle(1, 3, 0.008f), s.draw_indexed(6, 8, at, false, true), draws++;
        s.regs(reg::PA_SU_SC_MODE_CNTL, {0});
    }
    tess(1, 1.5f), triangle(2, 3, 0.01f), s.draw_auto(5, 5, true), draws++;
    s.shader(0, quad_cp_domain());
    tess(0, 2.0f), quad(3, 3, 0.006f), s.draw_auto(13, 8, true), draws++;
    s.regs(VGT_OUTPUT_PATH_CNTL, {0});

    // ---- the resolve into the front buffer, and the swap ----
    uint32_t rect_at = data + 0x800;
    const float v[6] = {-0.5f, -0.5f, float(SIZE) - 0.5f, -0.5f, float(SIZE) - 0.5f, float(SIZE) - 0.5f};
    for (uint32_t k = 0; k < 6; k++) s.put(rect_at + 4 * k, std::bit_cast<uint32_t>(v[k]));
    s.regs(reg::RB_MODECONTROL, {6});
    s.regs(reg::RB_COPY_CONTROL, {0, front, SIZE | SIZE << 16, 2 | 6u << 7});
    s.regs(reg::FETCH_CONSTANTS, {rect_at | 3, 6u << 2 | 2});
    s.draw_auto(8, 3);
    s.regs(reg::RB_MODECONTROL, {4});
    // the front buffer: tiled 8_8_8_8, 8in32, as the resolve wrote it
    s.regs(reg::FETCH_CONSTANTS,
           {2 | (SIZE / 32) << 22 | 1u << 31, front | 6 | 2u << 6, (SIZE - 1) | (SIZE - 1) << 13, 0xD10, 0, 0x200});
    s.w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, front, SIZE, SIZE});
}

struct Run {
    render::Image frame;
    render::Report report;
    uint32_t draws = 0;
    std::vector<std::filesystem::path> traces;
};

bool run(vk::Device &device, bool in_vs, const char *trace_dir, Run &out) {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("no memory: %s\n", error.c_str());
        return false;
    }
    Host host;
    CommandProcessor cp(*memory, host);
    render::Options options;
    options.expand_in_vertex_shader = in_vs;
    options.frame_wanted = [](uint64_t swap) { return swap == 1; };
    options.on_frame = [&](uint64_t, const render::Image &frame) { out.frame = frame; };
    auto renderer = render::Renderer::create(device, cp, options, error);
    if (!renderer) {
        std::printf("no renderer: %s\n", error.c_str());
        return false;
    }
    std::unique_ptr<TraceWriter> writer;
    if (trace_dir) {
        TraceOptions t;
        t.directory = trace_dir;
        t.frames = {1};
        t.title_id = TITLE;
        writer = std::make_unique<TraceWriter>(cp, renderer.get(), renderer.get(), t);
        cp.set_sink(writer.get());
    } else {
        cp.set_sink(renderer.get());
    }
    uint32_t ring = physical_alloc(*memory, 0x40000);
    cp.initialize_ring_buffer(ring, 15);
    Stream s{*memory, ring};
    build(*memory, s, out.draws);
    cp.run(s.n);
    renderer->drain();
    out.report = renderer->report();
    if (writer) out.traces = writer->finished();
    writer.reset();
    return true;
}

// the mean difference (of 255) and the share of pixels off by more than 16
void compare(const render::Image &a, const unsigned char *b, double &mean, double &share, int &worst) {
    uint64_t sum = 0, off = 0;
    worst = 0;
    size_t pixels = size_t(a.width) * a.height;
    for (size_t i = 0; i < pixels; i++) {
        int pixel_worst = 0;
        for (int k = 0; k < 3; k++) {
            int d = std::abs(int(a.rgba[i * 4 + k]) - int(b[i * 4 + k]));
            sum += d;
            pixel_worst = std::max(pixel_worst, d);
        }
        off += pixel_worst > 16;
        worst = std::max(worst, pixel_worst);
    }
    mean = double(sum) / (double(pixels) * 3);
    share = 100.0 * double(off) / double(pixels);
}

} // namespace

int main() {
    std::string error;
    auto device = vk::Device::open(error);
    if (!device) {
        std::printf("no Vulkan device (%s): nothing to draw\n", error.c_str());
        return 0;
    }
    const char *trace_dir = std::getenv("XF_SYNTH_TRACES");
    Run gs, vs;
    CHECK(run(*device, false, trace_dir, gs));
    CHECK(run(*device, true, nullptr, vs));
    for (Run *r : {&gs, &vs}) {
        CHECK(r->frame.width == SIZE && r->frame.height == SIZE);
        // only the tessellated draws (10) may be skipped, where the device has
        // no tessellation shaders
        uint64_t skipped = 0;
        for (const auto &[why, n] : r->report.skipped) {
            std::printf("  skipped %llu: %s\n", (unsigned long long)n, why.c_str());
            skipped += n;
            CHECK(!device->tessellation_shader() && why.starts_with("tessellation without tessellation shaders"));
        }
        CHECK(r->report.drawn + skipped == r->draws);
        if (r->frame.rgba.size() != size_t(SIZE) * SIZE * 4) return finish("primitives");
        // every cell drawn
        for (uint32_t cell_index = 0; cell_index < 16; cell_index++) {
            if (!device->tessellation_shader() && cell_index >= 6) break;
            uint32_t x0 = (cell_index % 4) * CELL, y0 = (cell_index / 4) * CELL, lit = 0;
            for (uint32_t y = y0; y < y0 + CELL; y++)
                for (uint32_t x = x0; x < x0 + CELL; x++)
                    lit += r->frame.rgba[(size_t(y) * SIZE + x) * 4 + 3] != 0 ||
                           r->frame.rgba[(size_t(y) * SIZE + x) * 4] != 0 ||
                           r->frame.rgba[(size_t(y) * SIZE + x) * 4 + 1] != 0;
            if (!lit) std::printf("  cell %u empty\n", cell_index);
            CHECK(lit > 0);
        }
    }
    // the geometry shaders' points and rectangles and the vertex shader's alike
    if (gs.frame.rgba.size() == vs.frame.rgba.size() && !gs.frame.rgba.empty()) {
        double mean, share;
        int worst;
        compare(gs.frame, vs.frame.rgba.data(), mean, share, worst);
        std::printf("geometry shaders against the vertex shader's expansion: mean %.4f, worst %d\n", mean, worst);
        CHECK(worst <= 1);
    }
    // against Xenia's picture of the trace, where it is there
    if (trace_dir) {
        CHECK(gs.traces.size() == 1);
        std::filesystem::path png = std::filesystem::path(trace_dir) / "58460001_1.png";
        int w = 0, h = 0, n = 0;
        unsigned char *ref = std::filesystem::exists(png) ? stbi_load(png.string().c_str(), &w, &h, &n, 4) : nullptr;
        if (ref && uint32_t(w) == SIZE && uint32_t(h) == SIZE) {
            double mean, share;
            int worst;
            compare(gs.frame, ref, mean, share, worst);
            std::printf("against Xenia: mean %.3f, %.3f%% off, worst %d\n", mean, share, worst);
            CHECK(mean <= 1.0 && share <= 0.1);
        }
        if (ref) stbi_image_free(ref);
    }
    return finish("primitives");
}
