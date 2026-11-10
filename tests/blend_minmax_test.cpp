// Minimum and maximum blending with factors (docs/renderer.md, "The host path
// against the exact one"): the console applies the blend factors before the
// minimum or maximum, a host's fixed-function blending ignores them. Drawn
// from packets built here: an 8_8_8_8 target filled with one color, then 16
// cells, each a rectangle of one source color under one RB_BLENDCONTROL, its
// right half then showing the alpha it left (white times the destination's
// alpha). Each cell's color and alpha are checked against the console's
// arithmetic worked out here, on host render targets (where the pixel shader
// multiplies the factor in: shader::PixelOptions::blend_premultiply) and on
// the interlock path, and the two pictures against each other. The last two
// cells have factors the host path cannot take (one needs the destination,
// one multiplies an alpha the color's blend reads): there the host path only
// has to count them.

#include "check.h"
#include "xenoforge/command_processor.h"
#include "xenoforge/memory.h"
#include "xenoforge/renderer.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::gpu;
using xenos::pm4::type0;
using xenos::pm4::type3;
namespace pm4 = xenos::pm4;
namespace reg = xenos::reg;

namespace {

// registers xenos.h does not name (Xenia's register_table.inc)
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100, RB_BLEND_RED = 0x2105, PA_CL_VPORT_XSCALE = 0x210F;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181, SQ_INTERPOLATOR_CNTL = 0x2182;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204;
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206, VGT_OUTPUT_PATH_CNTL = 0x2284;

constexpr uint32_t SIZE = 320, CELL = 80; // the frame: 4 by 4 cells

// RB_BLENDCONTROL's factors and operations
enum : uint32_t {
    ZERO = 0,
    ONE = 1,
    SRC_COLOR = 4,
    INV_SRC_COLOR = 5,
    SRC_ALPHA = 6,
    INV_SRC_ALPHA = 7,
    DST_COLOR = 8,
    DST_ALPHA = 10,
    CONSTANT_COLOR = 12,
    INV_CONSTANT_COLOR = 13,
    CONSTANT_ALPHA = 14,
    INV_CONSTANT_ALPHA = 15,
};
enum : uint32_t { ADD = 0, MIN = 2, MAX = 3 };
constexpr uint32_t half(uint32_t source, uint32_t op, uint32_t destination) {
    return source | op << 5 | destination << 8;
}

struct Case {
    uint32_t color, alpha; // RB_BLENDCONTROL's halves
    bool host_counts;      // the host path leaves it to the host's blending, counted
};
const Case CASES[16] = {
    {half(SRC_ALPHA, MIN, ONE), half(SRC_ALPHA, MIN, ONE), false},
    {half(SRC_ALPHA, MAX, ONE), half(ONE, MAX, ONE), false},
    {half(INV_SRC_COLOR, MIN, ONE), half(INV_SRC_ALPHA, MAX, ONE), false},
    {half(SRC_COLOR, MAX, ONE), half(SRC_COLOR, MIN, ONE), false},
    {half(CONSTANT_COLOR, MAX, ONE), half(CONSTANT_ALPHA, MAX, ONE), false},
    {half(INV_CONSTANT_ALPHA, MIN, ONE), half(INV_CONSTANT_COLOR, MIN, ONE), false},
    {half(SRC_ALPHA, MAX, ZERO), half(SRC_ALPHA, MAX, ZERO), false},
    {half(CONSTANT_ALPHA, MIN, ZERO), half(ONE, ADD, ZERO), false},
    {half(INV_CONSTANT_COLOR, MAX, ONE), half(INV_CONSTANT_ALPHA, MAX, ZERO), false},
    {half(INV_SRC_ALPHA, MIN, ONE), half(ONE, ADD, ZERO), false},
    {half(ZERO, MAX, ONE), half(ZERO, MIN, ONE), false},
    {half(ONE, MIN, ONE), half(CONSTANT_COLOR, MIN, ONE), false},
    {half(SRC_COLOR, MIN, ONE), half(INV_SRC_COLOR, MAX, ONE), false},
    {half(SRC_ALPHA, MAX, ONE), half(ZERO, MAX, ZERO), false},
    {half(DST_COLOR, MAX, ONE), half(ONE, ADD, ZERO), true},
    {half(SRC_ALPHA, ADD, INV_SRC_ALPHA), half(CONSTANT_ALPHA, MAX, ONE), true},
};
const float SOURCE[4] = {0.8f, 0.3f, 0.55f, 0.6f}, BACKGROUND[4] = {0.5f, 0.25f, 0.7f, 0.4f};
const float CONSTANT[4] = {0.9f, 0.5f, 0.2f, 0.75f};

// The console's blend of one component (edram_fsi.frag's arithmetic on an
// 8_8_8_8 target: everything within 0-1)
float factor(uint32_t f, float value, uint32_t k, const float *s, const float *d, bool alpha) {
    switch (f) {
    case ZERO: return 0;
    case SRC_COLOR: return value * s[alpha ? 3 : k];
    case INV_SRC_COLOR: return value * (1 - s[alpha ? 3 : k]);
    case SRC_ALPHA: return value * s[3];
    case INV_SRC_ALPHA: return value * (1 - s[3]);
    case DST_COLOR: return value * d[alpha ? 3 : k];
    case DST_ALPHA: return value * d[3];
    case CONSTANT_COLOR: return value * CONSTANT[alpha ? 3 : k];
    case INV_CONSTANT_COLOR: return value * (1 - CONSTANT[alpha ? 3 : k]);
    case CONSTANT_ALPHA: return value * CONSTANT[3];
    case INV_CONSTANT_ALPHA: return value * (1 - CONSTANT[3]);
    default: return value;
    }
}
float blended(uint32_t h, uint32_t k, const float *s, const float *d) {
    const bool alpha = k == 3;
    float a = factor(h & 0x1F, s[k], k, s, d, alpha), b = factor((h >> 8) & 0x1F, d[k], k, s, d, alpha);
    uint32_t op = (h >> 5) & 7;
    return std::clamp(op == MIN ? std::min(a, b) : op == MAX ? std::max(a, b) : a + b, 0.0f, 1.0f);
}

// ---- microcode (Xenia's ucode.h: AluInstruction, ControlFlowExecInstruction) ----

struct Src {
    bool temp;
    uint32_t index;
    const char *swizzle;
};
Src r(uint32_t i, const char *s = "xyzw") { return {true, i, s}; }
Src c(uint32_t i, const char *s = "xyzw") { return {false, i, s}; }
struct Dst {
    bool exported;
    uint32_t index, mask;
};
Dst t(uint32_t i) { return {false, i, 0xF}; }
Dst e(uint32_t i, uint32_t mask = 0xF) { return {true, i, mask}; }
constexpr uint32_t POSITION = 62;
enum Op : uint32_t { MUL = 1, MAXV = 2, MAD = 11 };

uint32_t swizzle(const char *s) {
    uint32_t v = 0;
    for (uint32_t j = 0; j < 4; j++) {
        uint32_t k = s[j] == 'x' ? 0 : s[j] == 'y' ? 1 : s[j] == 'z' ? 2 : 3;
        v |= ((k - j) & 3) << (2 * j);
    }
    return v;
}
std::array<uint32_t, 3> alu(Op op, Dst d, std::initializer_list<Src> sources) {
    std::array<uint32_t, 3> w = {d.index | (d.exported ? 1u << 15 : 0u) | d.mask << 16 | 50u << 26, 0,
                                 uint32_t(op) << 24};
    uint32_t k = 0;
    for (const Src &s : sources) {
        uint32_t shift = 8 * (2 - k);
        w[1] |= swizzle(s.swizzle) << shift;
        w[2] |= s.index << shift | (s.temp ? 1u << (31 - k) : 0u);
        k++;
    }
    return w;
}
std::vector<uint32_t> program(const std::vector<std::array<uint32_t, 3>> &code) {
    uint32_t count = uint32_t(code.size());
    std::vector<uint32_t> words = {1 | count << 12, 2u << 12, 0};
    for (const auto &i : code) words.insert(words.end(), i.begin(), i.end());
    return words;
}
// rectangles: vertex i at c0 + i * c1 + i * i * c2, colored c3
std::vector<uint32_t> rectangle() {
    return program({alu(MUL, t(1), {r(0, "xxxx"), r(0, "xxxx")}), alu(MAD, t(2), {r(0, "xxxx"), c(1), c(0)}),
                    alu(MAD, e(POSITION), {r(1), c(2), r(2)}), alu(MAXV, e(0), {c(3), c(3)})});
}
std::vector<uint32_t> pixel() { return program({alu(MAXV, e(0), {r(0), r(0)})}); }

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
    void shader(uint32_t type, const std::vector<uint32_t> &code) {
        w({type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(code.size())), type, uint32_t(code.size())});
        for (uint32_t v : code) w({v});
    }
    void constants(std::initializer_list<std::array<float, 4>> values) {
        w({type0(reg::ALU_CONSTANTS, 4 * uint32_t(values.size()))});
        for (const auto &v : values)
            for (float f : v) w({std::bit_cast<uint32_t>(f)});
    }
    // a rectangle list's three vertices, auto-indexed
    void draw_rectangle() { w({type3(pm4::DRAW_INDX_2, 1), 8u | 2u << 6 | 3u << 16}); }
};

uint32_t physical_alloc(Memory &m, uint32_t size) { return m.physical_address(m.system_alloc(size, 0x1000, true)); }

void build(Memory &m, Stream &s, uint32_t &draws) {
    uint32_t data = physical_alloc(m, 0x1000), front = physical_alloc(m, SIZE * SIZE * 4);
    draws = 0;
    s.w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, front, SIZE, SIZE});

    // a 320x320 8_8_8_8 target at EDRAM's start, no depth; the viewport over
    // it; Direct3D's pixel centers
    s.regs(reg::RB_SURFACE_INFO, {SIZE, 0, 0});
    s.regs(reg::RB_MODECONTROL, {4});
    s.regs(reg::RB_COLOR_MASK, {0xF});
    s.regs(reg::RB_DEPTHCONTROL, {0});
    s.regs(RB_COLORCONTROL, {0});
    s.regs(PA_CL_CLIP_CNTL, {0});
    s.regs(reg::PA_SU_SC_MODE_CNTL, {0});
    s.regs(PA_CL_VTE_CNTL, {0x43F});
    const float half_size = float(SIZE) / 2;
    s.regs(PA_CL_VPORT_XSCALE,
           {std::bit_cast<uint32_t>(half_size), std::bit_cast<uint32_t>(half_size), std::bit_cast<uint32_t>(-half_size),
            std::bit_cast<uint32_t>(half_size), std::bit_cast<uint32_t>(1.0f), 0});
    s.regs(reg::PA_SU_VTX_CNTL, {0});
    s.regs(reg::PA_SC_WINDOW_OFFSET, {0});
    s.regs(reg::PA_SC_WINDOW_SCISSOR_TL, {0x80000000, SIZE | SIZE << 16});
    s.regs(reg::PA_SC_SCREEN_SCISSOR_TL, {0, SIZE | SIZE << 16});
    s.regs(VGT_MAX_VTX_INDX, {0xFFFFFF, 0, 0});
    s.regs(reg::SQ_PROGRAM_CNTL, {3 | 0u << 8});
    s.regs(SQ_CONTEXT_MISC, {0});
    s.regs(SQ_INTERPOLATOR_CNTL, {0});
    s.regs(VGT_OUTPUT_PATH_CNTL, {0});
    s.regs(RB_BLEND_RED, {std::bit_cast<uint32_t>(CONSTANT[0]), std::bit_cast<uint32_t>(CONSTANT[1]),
                          std::bit_cast<uint32_t>(CONSTANT[2]), std::bit_cast<uint32_t>(CONSTANT[3])});
    s.shader(1, pixel());
    s.shader(0, rectangle());

    // a rectangle (x0, y0)-(x1, y1) in normalized device coordinates, of one color
    auto rect = [&](float x0, float y0, float x1, float y1, const float *color, uint32_t blend) {
        // c0 + i c1 + i^2 c2 through (x0, y0), (x1, y0), (x1, y1)
        float c2x = -(x1 - x0) / 2, c2y = (y1 - y0) / 2;
        float c1x = (x1 - x0) - c2x, c1y = -c2y;
        s.constants({{x0, y0, 0, 1}, {c1x, c1y, 0, 0}, {c2x, c2y, 0, 0}, {color[0], color[1], color[2], color[3]}});
        s.regs(RB_BLENDCONTROL0, {blend});
        s.draw_rectangle(), draws++;
    };
    rect(-1, 1, 1, -1, BACKGROUND, 0x00010001);
    const float white[4] = {1, 1, 1, 1};
    for (uint32_t k = 0; k < 16; k++) {
        float x0 = -1.0f + 0.5f * float(k % 4) + 0.05f, y0 = 1.0f - 0.5f * float(k / 4) - 0.05f;
        rect(x0, y0, x0 + 0.4f, y0 - 0.4f, SOURCE, CASES[k].color | CASES[k].alpha << 16);
        // the alpha it left, as a gray: white times the destination's alpha, the alpha kept
        rect(x0 + 0.2f, y0, x0 + 0.4f, y0 - 0.4f, white, half(DST_ALPHA, ADD, ZERO) | half(ZERO, ADD, ONE) << 16);
    }

    // ---- the resolve into the front buffer, and the swap ----
    uint32_t rect_at = data + 0x800;
    const float v[6] = {-0.5f, -0.5f, float(SIZE) - 0.5f, -0.5f, float(SIZE) - 0.5f, float(SIZE) - 0.5f};
    for (uint32_t k = 0; k < 6; k++) s.put(rect_at + 4 * k, std::bit_cast<uint32_t>(v[k]));
    s.regs(RB_BLENDCONTROL0, {0x00010001});
    s.regs(reg::RB_MODECONTROL, {6});
    s.regs(reg::RB_COPY_CONTROL, {0, front, SIZE | SIZE << 16, 2 | 6u << 7});
    s.regs(reg::FETCH_CONSTANTS, {rect_at | 3, 6u << 2 | 2});
    s.draw_rectangle();
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
};

bool run(vk::Device &device, render::Options::RenderTargetPath path, Run &out) {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("no memory: %s\n", error.c_str());
        return false;
    }
    Host host;
    CommandProcessor cp(*memory, host);
    render::Options options;
    options.render_target_path = path;
    options.frame_wanted = [](uint64_t swap) { return swap == 1; };
    options.on_frame = [&](uint64_t, const render::Image &frame) { out.frame = frame; };
    auto renderer = render::Renderer::create(device, cp, options, error);
    if (!renderer) {
        std::printf("no renderer: %s\n", error.c_str());
        return false;
    }
    cp.set_sink(renderer.get());
    uint32_t ring = physical_alloc(*memory, 0x40000);
    cp.initialize_ring_buffer(ring, 15);
    Stream s{*memory, ring};
    build(*memory, s, out.draws);
    cp.run(s.n);
    renderer->drain();
    out.report = renderer->report();
    return true;
}

// a cell's color (its left half) and the alpha shown in its right half
void sample(const render::Image &frame, uint32_t k, int out[4]) {
    uint32_t x = (k % 4) * CELL + 24, y = (k / 4) * CELL + 40;
    for (uint32_t j = 0; j < 3; j++) out[j] = frame.rgba[(size_t(y) * SIZE + x) * 4 + j];
    out[3] = frame.rgba[(size_t(y) * SIZE + x + 32) * 4];
}

uint64_t count_of(const std::map<std::string, uint64_t> &counts, const char *prefix) {
    uint64_t n = 0;
    for (const auto &[what, count] : counts)
        if (what.starts_with(prefix)) n += count;
    return n;
}

} // namespace

int main() {
    std::string error;
    auto device = vk::Device::open(error);
    if (!device) {
        std::printf("no Vulkan device (%s): nothing to draw\n", error.c_str());
        return 0;
    }
    using Path = render::Options::RenderTargetPath;
    // (XF_RENDER_TARGET_PATH would override the paths asked for here)
#ifdef _WIN32
    _putenv_s("XF_RENDER_TARGET_PATH", "");
#else
    unsetenv("XF_RENDER_TARGET_PATH");
#endif
    render::Options exact_options;
    exact_options.render_target_path = Path::fsi;
    const bool interlock = render::render_target_path(*device, exact_options) == Path::fsi;
    Run host, exact;
    CHECK(run(*device, Path::host, host));
    if (interlock)
        CHECK(run(*device, Path::fsi, exact));
    else
        std::printf("no fragment shader interlock: the host path alone\n");

    // the destination as the target keeps it
    float destination[4];
    for (uint32_t j = 0; j < 4; j++) destination[j] = std::round(BACKGROUND[j] * 255.0f) / 255.0f;
    for (Run *r : interlock ? std::initializer_list<Run *>{&host, &exact} : std::initializer_list<Run *>{&host}) {
        const bool is_host = r == &host;
        CHECK(r->frame.width == SIZE && r->frame.height == SIZE);
        if (r->frame.rgba.size() != size_t(SIZE) * SIZE * 4) return finish("blend_minmax");
        CHECK(r->report.skipped.empty());
        CHECK(r->report.drawn == r->draws);
        for (uint32_t k = 0; k < 16; k++) {
            if (is_host && CASES[k].host_counts) continue;
            int got[4];
            sample(r->frame, k, got);
            for (uint32_t j = 0; j < 4; j++) {
                float expected = blended(j < 3 ? CASES[k].color : CASES[k].alpha, j, SOURCE, destination) * 255.0f;
                if (std::fabs(float(got[j]) - expected) > 1.01f)
                    std::printf("  %s, cell %u, component %u: %d, expected %.2f\n", is_host ? "host" : "interlock", k,
                                j, got[j], expected);
                CHECK(std::fabs(float(got[j]) - expected) <= 1.01f);
            }
        }
    }
    // the host path: what it left to the host's blending is counted, the rest multiplied in
    const char *left = "minimum and maximum blending with factors (the host ignores them)";
    const char *done = "minimum and maximum blending with factors multiplied in by the pixel shader";
    CHECK(count_of(host.report.approximate, left) == 2);
    CHECK(count_of(host.report.used, done) == 13); // (one cell has factors of zero and one alone: the host's own blend)
    // the two paths alike, outside the two cells the host path counts
    if (interlock && host.frame.rgba.size() == exact.frame.rgba.size()) {
        int worst = 0;
        for (uint32_t y = 0; y < SIZE; y++)
            for (uint32_t x = 0; x < SIZE; x++) {
                if (CASES[(y / CELL) * 4 + x / CELL].host_counts) continue;
                for (uint32_t j = 0; j < 3; j++) {
                    size_t at = (size_t(y) * SIZE + x) * 4 + j;
                    worst = std::max(worst, std::abs(int(host.frame.rgba[at]) - int(exact.frame.rgba[at])));
                }
            }
        std::printf("host render targets against the interlock path: worst %d\n", worst);
        CHECK(worst <= 1);
    }
    return finish("blend_minmax");
}
