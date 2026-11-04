// The CPU interpreter on microcode made here: every vector and scalar
// operation on chosen and edge values (0, -0, infinities, NaN), swizzles and
// modifiers, a0-relative and out-of-range constants, write masks, exports
// with constant components, saturation, the predicate (setp, predicated
// execs and instructions), bool-constant execs, a loop with aL-relative
// writes and a predicated break, a call, a return and a jump, kills, vertex
// fetches of three formats with their endianness, the harness's stand-in for
// texture fetches, and what it refuses. Expected values are worked out by
// hand from Xenia's semantics.

#include "check.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/ucode_parse.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::ucode;

namespace {

constexpr float N = NAN, I = INFINITY;

// ---- an assembler, just enough ----

struct Cf {
    uint32_t d0, d1; // the low 32 bits and the high 16
};

Cf exec_cf(bool end, uint32_t address, uint32_t count, uint32_t sequence) {
    return {address | count << 12 | sequence << 16, 1u << 9 | (end ? 2u : 1u) << 12};
}
Cf cond_exec(uint32_t address, uint32_t count, uint32_t sequence, uint32_t bool_constant, bool condition) {
    return {address | count << 12 | sequence << 16, bool_constant << 2 | uint32_t(condition) << 10 | 3u << 12};
}
Cf cond_exec_pred(uint32_t address, uint32_t count, uint32_t sequence, bool condition) {
    return {address | count << 12 | sequence << 16, 1u << 9 | uint32_t(condition) << 10 | 5u << 12};
}
Cf loop_start(uint32_t skip, uint32_t loop, bool repeat) { return {skip | uint32_t(repeat) << 13 | loop << 16, 7u << 12}; }
Cf loop_end(uint32_t body, uint32_t loop, bool predicated_break, bool condition) {
    return {body | loop << 16 | uint32_t(predicated_break) << 21, uint32_t(condition) << 10 | 8u << 12};
}
Cf branch(uint32_t opcode, uint32_t target) { return {target | 1u << 13, opcode << 12}; } // unconditional
Cf ret() { return {0, 10u << 12}; }
Cf nop() { return {0, 0}; }

struct Ins {
    uint32_t w[3];
    bool fetch;
};

// A swizzle for an ALU source: component i reads source c, stored as (c - i) & 3.
uint32_t swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return ((x - 0) & 3) | ((y - 1) & 3) << 2 | ((z - 2) & 3) << 4 | ((w - 3) & 3) << 6;
}

struct Src {
    bool temp = true;
    uint32_t reg = 0;
    uint32_t swizzle = 0; // xyzw
    bool negate = false, abs = false, relative = false;
};
Src R(uint32_t r, uint32_t s = 0) { return {true, r, s}; }
Src C(uint32_t c, uint32_t s = 0) { return {false, c, s}; }

uint32_t src_byte(const Src &s) {
    return s.temp ? (s.reg & 0x3F) | uint32_t(s.relative) << 6 | uint32_t(s.abs) << 7 : s.reg & 0xFF;
}

struct Alu {
    uint32_t vop = 2, sop = 50; // max (unwritten: skipped) and retain_prev
    uint32_t vdest = 0, vmask = 0, sdest = 0, smask = 0;
    bool vclamp = false, sclamp = false, exp = false, vrel = false, srel = false, abs_constants = false;
    Src s1 = R(0), s2 = R(0), s3 = R(0);
    bool predicated = false, condition = false, const_a0 = false, const0_addressed = false, const1_addressed = false;

    Ins ins() const {
        uint32_t w0 = vdest | uint32_t(vrel) << 6 | uint32_t(abs_constants) << 7 | sdest << 8 | uint32_t(srel) << 14 |
                      uint32_t(exp) << 15 | vmask << 16 | smask << 20 | uint32_t(vclamp) << 24 |
                      uint32_t(sclamp) << 25 | sop << 26;
        uint32_t w1 = s3.swizzle | s2.swizzle << 8 | s1.swizzle << 16 | uint32_t(s3.negate) << 24 |
                      uint32_t(s2.negate) << 25 | uint32_t(s1.negate) << 26 | uint32_t(condition) << 27 |
                      uint32_t(predicated) << 28 | uint32_t(const_a0) << 29 | uint32_t(const1_addressed) << 30 |
                      uint32_t(const0_addressed) << 31;
        uint32_t w2 = src_byte(s3) | src_byte(s2) << 8 | src_byte(s1) << 16 | vop << 24 | uint32_t(s3.temp) << 29 |
                      uint32_t(s2.temp) << 30 | uint32_t(s1.temp) << 31;
        return {{w0, w1, w2}, false};
    }
};

// a fetch's destination swizzle: 0-3 xyzw, 4 zero, 5 one, 7 keep
constexpr uint32_t dst(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 3 | z << 6 | w << 9; }
constexpr uint32_t XYZW = dst(0, 1, 2, 3), XYZ1 = dst(0, 1, 2, 5);

Ins vfetch(bool mini, uint32_t dest, uint32_t src, uint32_t constant, uint32_t sel, uint32_t format, uint32_t swizzle,
           uint32_t stride, int32_t offset, bool is_signed = false, bool integer = false) {
    uint32_t w0 = 0u | src << 5 | dest << 12 | 1u << 19 | constant << 20 | sel << 25;
    uint32_t w1 = swizzle | uint32_t(is_signed) << 12 | uint32_t(integer) << 13 | format << 16 | uint32_t(mini) << 30;
    uint32_t w2 = stride | (uint32_t(offset) & 0x7FFFFF) << 8;
    return {{w0, w1, w2}, true};
}

Ins tfetch(uint32_t opcode, uint32_t dimension, uint32_t dest, uint32_t src, uint32_t src_swizzle, uint32_t constant) {
    return {{opcode | src << 5 | dest << 12 | constant << 20 | src_swizzle << 26, XYZW, dimension << 14}, true};
}

void pack(std::vector<uint32_t> &out, Cf a, Cf b) {
    out.push_back(a.d0);
    out.push_back((a.d1 & 0xFFFF) | b.d0 << 16);
    out.push_back(b.d0 >> 16 | b.d1 << 16);
}

uint32_t sequence(const std::vector<Ins> &ins, size_t first, size_t count) {
    uint32_t s = 0;
    for (size_t i = 0; i < count; i++)
        if (ins[first + i].fetch) s |= 1u << (2 * i);
    return s;
}

// A control flow of its own (instructions start after it, at cf.size() / 2 pairs, rounded up).
std::vector<uint32_t> assemble(std::vector<Cf> cf, const std::vector<Ins> &ins) {
    if (cf.size() & 1) cf.push_back(nop());
    std::vector<uint32_t> d;
    for (size_t i = 0; i < cf.size(); i += 2) pack(d, cf[i], cf[i + 1]);
    for (const Ins &x : ins) d.insert(d.end(), x.w, x.w + 3);
    return d;
}

// The instructions in execs of up to six, the last an exec end.
std::vector<uint32_t> program(const std::vector<Ins> &ins) {
    uint32_t execs = uint32_t((ins.size() + 5) / 6), base = (execs + 1) / 2;
    std::vector<Cf> cf;
    for (uint32_t e = 0; e < execs; e++) {
        uint32_t first = e * 6, count = std::min<uint32_t>(6, uint32_t(ins.size()) - first);
        cf.push_back(exec_cf(e + 1 == execs, base + first, count, sequence(ins, first, count)));
    }
    return assemble(cf, ins);
}

// ---- running ----

struct Run {
    harness::CaseInput in{};
    harness::Constants k{};
    std::vector<uint8_t> memory;
    harness::CaseOutput out{};
    std::string unsupported;
    bool ok = false;

    float *r(uint32_t i) { return in.registers[i]; }
    const float *o(uint32_t i) const { return out.registers[i]; }
    bool go(const std::vector<uint32_t> &dwords, ShaderType type = ShaderType::Vertex) {
        ParsedShader s = parse_shader(type, dwords);
        ok = shader::interpret(s, in, k, memory, out, unsupported);
        if (!ok) std::printf("  refused: %s\n", unsupported.c_str());
        return ok;
    }
};

void set(float *v, float x, float y, float z, float w) {
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
}

uint32_t bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Equal bit for bit (so +0 is not -0), any NaN equal to any NaN.
bool same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || bits(a) == bits(b); }

bool same4(const float *v, float x, float y, float z, float w) {
    bool ok = same(v[0], x) && same(v[1], y) && same(v[2], z) && same(v[3], w);
    if (!ok) std::printf("  got (%g %g %g %g), want (%g %g %g %g)\n", v[0], v[1], v[2], v[3], x, y, z, w);
    return ok;
}

// ---- vector operations ----

struct VectorCase {
    uint32_t op;
    float a[4], b[4], c[4], expect[4];
};

void vector_operations() {
    const float A[4] = {1.5f, -2.0f, 0.0f, N}, B[4] = {-0.0f, 3.0f, I, 2.0f}, Cc[4] = {0.25f, -1.0f, 4.0f, -8.0f};
    const float A2[4] = {1, 2, N, -0.0f}, B2[4] = {1, 3, N, 0}, A3[4] = {5, -1, 0, 1}, B3[4] = {4, -1, 0, 2};
    const float F[4] = {1.5f, -1.25f, -0.0f, I}, Z[4] = {};
    const float D1[4] = {1, 2, 0, 4}, D2[4] = {0.5f, -1, I, 0.25f}, D3[4] = {10, 0, 0, 0};
    const VectorCase cases[] = {
        {0, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {}, {1.5f, 1, I, N}},              // add
        {1, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {}, {0.0f, -6, 0.0f, N}},          // mul: +0, not -0
        {2, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {}, {1.5f, 3, I, 2}},              // max
        {3, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {}, {-0.0f, -2, 0, 2}},            // min
        {4, {A2[0], A2[1], A2[2], A2[3]}, {B2[0], B2[1], B2[2], B2[3]}, {}, {1, 0, 0, 1}},         // seq
        {5, {A3[0], A3[1], A3[2], A3[3]}, {B3[0], B3[1], B3[2], B3[3]}, {}, {1, 0, 0, 0}},         // sgt
        {6, {A3[0], A3[1], A3[2], A3[3]}, {B3[0], B3[1], B3[2], B3[3]}, {}, {1, 1, 1, 0}},         // sge
        {7, {A2[0], A2[1], A2[2], A2[3]}, {B2[0], B2[1], B2[2], B2[3]}, {}, {0, 1, 1, 0}},         // sne
        {8, {F[0], F[1], F[2], F[3]}, {}, {}, {0.5f, 0.75f, 0.0f, N}},                             // frc
        {9, {F[0], F[1], F[2], F[3]}, {}, {}, {1, -1, -0.0f, I}},                                  // trunc
        {10, {F[0], F[1], F[2], F[3]}, {}, {}, {1, -2, -0.0f, I}},                                 // floor
        {11, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {Cc[0], Cc[1], Cc[2], Cc[3]}, {0.25f, -7, 4, N}}, // mad
        {12, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {Cc[0], Cc[1], Cc[2], Cc[3]}, {0.25f, -1, I, -8}}, // cndeq
        {13, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {Cc[0], Cc[1], Cc[2], Cc[3]}, {-0.0f, -1, I, -8}}, // cndge
        {14, {A[0], A[1], A[2], A[3]}, {B[0], B[1], B[2], B[3]}, {Cc[0], Cc[1], Cc[2], Cc[3]}, {-0.0f, -1, 4, -8}}, // cndgt
        {15, {D1[0], D1[1], D1[2], D1[3]}, {D2[0], D2[1], D2[2], D2[3]}, {}, {-0.5f, -0.5f, -0.5f, -0.5f}}, // dp4: 0 * inf is 0
        {16, {D1[0], D1[1], D1[2], D1[3]}, {D2[0], D2[1], D2[2], D2[3]}, {}, {-1.5f, -1.5f, -1.5f, -1.5f}}, // dp3
        {17, {D1[0], D1[1], D1[2], D1[3]}, {D2[0], D2[1], D2[2], D2[3]}, {D3[0], D3[1], D3[2], D3[3]}, {8.5f, 8.5f, 8.5f, 8.5f}}, // dp2add
        {19, {1, 7, N, 3}, {}, {}, {7, 7, 7, 7}},                                                   // max4: NMax skips NaN
        {28, {9, 2, 3, 9}, {9, 4, 9, 5}, {}, {1, 8, 3, 5}},                                         // dst
        {24, {1, 2, 3, 4}, {5, 2, 7, 8}, {}, {0, 0, 0, 0}},                                         // kill_eq: kills, 0
    };
    (void)Z;
    for (const VectorCase &c : cases) {
        Run run;
        std::memcpy(run.r(1), c.a, 16);
        std::memcpy(run.r(2), c.b, 16);
        std::memcpy(run.r(3), c.c, 16);
        Alu alu;
        alu.vop = c.op;
        alu.vdest = 4;
        alu.vmask = 0xF;
        alu.s1 = R(1);
        alu.s2 = R(2);
        alu.s3 = R(3);
        if (!run.go(program({alu.ins()}))) {
            CHECK(false);
            continue;
        }
        if (!same4(run.o(4), c.expect[0], c.expect[1], c.expect[2], c.expect[3])) std::printf("  vector op %u\n", c.op);
        CHECK(same4(run.o(4), c.expect[0], c.expect[1], c.expect[2], c.expect[3]));
        CHECK(run.out.killed == (c.op == 24 ? 1u : 0u));
    }
}

// cube: the operand read as .z_xy
void cube() {
    struct {
        float in[4], expect[4];
    } cases[] = {
        {{1, 0, 0.5f, -0.25f}, {0.25f, 0.5f, 2, 4}},   // +z major
        {{0.5f, 0, -2, 1}, {-1, 0.5f, -4, 1}},         // -x major
        {{0.25f, 0, 0.5f, -3}, {-0.25f, 0.5f, -6, 3}}, // -y major
    };
    for (auto &c : cases) {
        Run run;
        std::memcpy(run.r(1), c.in, 16);
        Alu alu;
        alu.vop = 18;
        alu.vdest = 4;
        alu.vmask = 0xF;
        alu.s1 = alu.s2 = R(1);
        CHECK(run.go(program({alu.ins()})));
        CHECK(same4(run.o(4), c.expect[0], c.expect[1], c.expect[2], c.expect[3]));
    }
}

void kills_that_do_not() {
    struct {
        uint32_t op;
        float a[4], b[4];
        uint32_t killed;
    } cases[] = {
        {27, {1, 2, 3, 4}, {1, 2, 3, 4}, 0},   // kill_ne, all equal
        {25, {1, 2, 3, 4}, {1, 2, 3, 3.5f}, 1}, // kill_gt
        {26, {1, 2, 3, N}, {2, 3, 4, N}, 0},   // kill_ge, NaN compares false
    };
    for (auto &c : cases) {
        Run run;
        std::memcpy(run.r(1), c.a, 16);
        std::memcpy(run.r(2), c.b, 16);
        Alu alu;
        alu.vop = c.op;
        alu.s1 = R(1);
        alu.s2 = R(2); // no write: a kill still runs
        CHECK(run.go(program({alu.ins()}), ShaderType::Pixel));
        CHECK(run.out.killed == c.killed);
    }
}

void setp_push_and_maxa() {
    struct {
        uint32_t op;
        float a[4], b[4];
        bool predicate;
        float result;
    } cases[] = {
        {20, {0, 0, 0, 0}, {0, 0, 0, 0}, true, 0},  // setp_eq_push
        {20, {2, 0, 0, 0}, {0, 0, 0, 1}, false, 3},
        {21, {0, 0, 0, 0}, {5, 0, 0, N}, true, 0},  // setp_ne_push: NaN is not 0
        {22, {1, 0, 0, 0}, {2, 0, 0, 3}, true, 2},  // setp_gt_push
        {23, {0, 0, 0, 0}, {-0.0f, 0, 0, -1}, false, 0}, // setp_ge_push: -0 >= 0
    };
    for (auto &c : cases) {
        Run run;
        std::memcpy(run.r(1), c.a, 16);
        std::memcpy(run.r(2), c.b, 16);
        Alu alu;
        alu.vop = c.op;
        alu.vdest = 4;
        alu.vmask = 0xF;
        alu.s1 = R(1);
        alu.s2 = R(2);
        CHECK(run.go(program({alu.ins()})));
        CHECK(run.out.predicate == uint32_t(c.predicate));
        CHECK(same4(run.o(4), c.result, c.result, c.result, c.result));
    }

    struct {
        float w;
        int32_t a0;
    } maxa[] = {{2.5f, 3}, {N, -256}, {1000, 255}, {-1000, -256}, {-0.5f, 0}};
    for (auto &m : maxa) {
        Run run;
        set(run.r(1), 1, 2, 3, m.w);
        set(run.r(2), 0, 5, 0, 0);
        Alu alu;
        alu.vop = 29;
        alu.vdest = 4;
        alu.vmask = 0b0111;
        alu.s1 = R(1);
        alu.s2 = R(2);
        CHECK(run.go(program({alu.ins()})));
        CHECK(run.out.address_register == m.a0);
        CHECK(same4(run.o(4), 1, 5, 3, 0));
    }
}

// ---- scalar operations ----

struct ScalarCase {
    uint32_t op;
    float a, b, ps; // ps set by an adds before
    float expect;
    int predicate; // -1 unchecked
    uint32_t killed;
    int32_t a0; // INT32_MIN unchecked
};

void scalar_operations() {
    constexpr int U = -1;
    constexpr int32_t NA = INT32_MIN;
    float third;
    uint32_t third_bits = 0x3EAAAAACu; // 1/3 (0x3EAAAAAB) rounded to 21 bits, up
    std::memcpy(&third, &third_bits, 4);
    const ScalarCase cases[] = {
        {0, 1.5f, 2, 0, 3.5f, U, 0, NA},          // adds
        {1, 1.5f, 0, 10, 11.5f, U, 0, NA},        // adds_prev
        {2, 0, I, 0, 0.0f, U, 0, NA},             // muls: 0 * inf = +0
        {2, -2, 3, 0, -6, U, 0, NA},
        {3, 0.5f, 0, 10, 5, U, 0, NA},            // muls_prev
        {3, 0, 0, I, 0.0f, U, 0, NA},
        {4, 0.5f, 2, 10, 5, U, 0, NA},            // muls_prev2
        {4, 0.5f, -1, 10, -FLT_MAX, U, 0, NA},
        {4, 0.5f, 2, -FLT_MAX, -FLT_MAX, U, 0, NA},
        {4, 0.5f, I, 10, -FLT_MAX, U, 0, NA},
        {5, N, 1, 0, 1, U, 0, NA},                // maxs: NaN >= 1 is false
        {5, 2, 1, 0, 2, U, 0, NA},
        {6, N, 1, 0, 1, U, 0, NA},                // mins
        {6, -0.0f, 0, 0, 0.0f, U, 0, NA},
        {7, 0, 0, 0, 1, U, 0, NA},                // seqs
        {7, N, 0, 0, 0, U, 0, NA},
        {8, 1, 0, 0, 1, U, 0, NA},                // sgts
        {9, -0.0f, 0, 0, 1, U, 0, NA},            // sges
        {10, N, 0, 0, 1, U, 0, NA},               // snes
        {11, -1.25f, 0, 0, 0.75f, U, 0, NA},      // frcs
        {12, -1.75f, 0, 0, -1, U, 0, NA},         // truncs
        {13, -1.25f, 0, 0, -2, U, 0, NA},         // floors
        {14, 3, 0, 0, 8, U, 0, NA},               // exp
        {15, 0, 0, 0, -FLT_MAX, U, 0, NA},        // logc
        {16, 8, 0, 0, 3, U, 0, NA},               // log
        {16, 0, 0, 0, -I, U, 0, NA},
        {17, 0, 0, 0, FLT_MAX, U, 0, NA},         // rcpc
        {17, -0.0f, 0, 0, -FLT_MAX, U, 0, NA},
        {18, 0, 0, 0, 0.0f, U, 0, NA},            // rcpf
        {18, -0.0f, 0, 0, -0.0f, U, 0, NA},
        {19, 4, 0, 0, 0.25f, U, 0, NA},           // rcp
        {19, 3, 0, 0, third, U, 0, NA},
        {19, 0, 0, 0, I, U, 0, NA},
        {20, 0, 0, 0, FLT_MAX, U, 0, NA},         // rsqc
        {21, 0, 0, 0, 0.0f, U, 0, NA},            // rsqf
        {22, 4, 0, 0, 0.5f, U, 0, NA},            // rsq
        {22, -1, 0, 0, N, U, 0, NA},
        {23, 2.5f, 1, 0, 2.5f, U, 0, 3},          // maxas: a0 = floor(a + 0.5)
        {23, -5, -9, 0, -5, U, 0, 0},             //   clamped to 0-255
        {24, 2.9f, 1, 0, 2.9f, U, 0, 2},          // maxasf: a0 = floor(a)
        {25, 5, 2, 0, 3, U, 0, NA},               // subs
        {26, 4, 0, 10, -6, U, 0, NA},             // subs_prev
        {27, 0, 0, 0, 0, 1, 0, NA},               // setp_eq
        {27, 2, 0, 0, 1, 0, 0, NA},
        {28, N, 0, 0, 0, 1, 0, NA},               // setp_ne
        {29, 1, 0, 0, 0, 1, 0, NA},               // setp_gt
        {30, -0.0f, 0, 0, 0, 1, 0, NA},           // setp_ge
        {31, 1, 0, 0, 0, 1, 0, NA},               // setp_inv
        {31, 0, 0, 0, 1, 0, 0, NA},
        {31, 5, 0, 0, 5, 0, 0, NA},
        {32, 1, 0, 0, 0, 1, 0, NA},               // setp_pop
        {32, 3, 0, 0, 2, 0, 0, NA},
        {33, 7, 0, 0, FLT_MAX, 0, 0, NA},         // setp_clr
        {34, 0, 0, 0, 0, 1, 0, NA},               // setp_rstr
        {34, 7, 0, 0, 7, 0, 0, NA},
        {35, 0, 0, 0, 0, U, 1, NA},               // kills_eq
        {36, -1, 0, 0, 0, U, 0, NA},              // kills_gt
        {37, 0, 0, 0, 0, U, 1, NA},               // kills_ge
        {38, 0, 0, 0, 0, U, 0, NA},               // kills_ne
        {39, 1, 0, 0, 0, U, 1, NA},               // kills_one
        {40, 16, 0, 0, 4, U, 0, NA},              // sqrt
        {40, -1, 0, 0, N, U, 0, NA},
        {48, 0, 0, 0, 0, U, 0, NA},               // sin
        {49, 0, 0, 0, 1, U, 0, NA},               // cos
        {50, 0, 0, 10, 10, U, 0, NA},             // retain_prev
    };
    for (const ScalarCase &c : cases) {
        Run run;
        set(run.r(1), c.b, 0, 0, c.a); // a is .w, b is .x
        set(run.r(2), c.ps, 0, 0, 0);  // the adds before: ps = r2.w + r2.x
        Alu before;
        before.sop = 0;
        before.s3 = R(2);
        Alu alu;
        alu.sop = c.op;
        alu.sdest = 5;
        alu.smask = 0xF;
        alu.s3 = R(1);
        if (!run.go(program({before.ins(), alu.ins()}), ShaderType::Pixel)) {
            CHECK(false);
            continue;
        }
        if (!same4(run.o(5), c.expect, c.expect, c.expect, c.expect)) std::printf("  scalar op %u\n", c.op);
        CHECK(same4(run.o(5), c.expect, c.expect, c.expect, c.expect));
        if (c.predicate != U) CHECK(run.out.predicate == uint32_t(c.predicate));
        CHECK(run.out.killed == c.killed);
        if (c.a0 != NA) CHECK(run.out.address_register == c.a0);
    }
}

// mulsc, addsc and subsc: c#.w and a register's .x (its index in the opcode's
// low bit, the source's select bit and the swizzle's middle bits)
void scalar_constant_operations() {
    struct {
        uint32_t op;
        float c, r, expect;
    } cases[] = {
        {42, 3, 5, 15},     // mulsc0, r2
        {42, 0, I, 0.0f},   //   0 * inf = +0
        {43, 3, 4, 12},     // mulsc1, r3
        {44, 3, 5, 8},      // addsc0
        {46, 3, 5, -2},     // subsc0
    };
    for (auto &c : cases) {
        Run run;
        set(run.k.float_constants[7], 0, 0, 0, c.c);
        set(run.r(2), c.r, 0, 0, 0);
        set(run.r(3), c.r, 0, 0, 0);
        Alu alu;
        alu.sop = c.op;
        alu.sdest = 5;
        alu.smask = 0xF;
        alu.s3 = {true, 7, 0}; // c7 and, by the select bit, register 2 or 3
        CHECK(run.go(program({alu.ins()})));
        CHECK(same4(run.o(5), c.expect, c.expect, c.expect, c.expect));
    }
}

// ---- operands and results ----

void operands_and_results() {
    { // add r4, -|r1|.wzyx, r2.xxxx
        Run run;
        set(run.r(1), 1, -2, 3, -4);
        set(run.r(2), 10, 20, 30, 40);
        Alu alu;
        alu.vop = 0;
        alu.vdest = 4;
        alu.vmask = 0xF;
        alu.s1 = R(1, swz(3, 2, 1, 0));
        alu.s1.negate = alu.s1.abs = true;
        alu.s2 = R(2, swz(0, 0, 0, 0));
        CHECK(run.go(program({alu.ins()})));
        CHECK(same4(run.o(4), 6, 7, 8, 9));
    }
    { // maxa sets a0 = 2, then add r4, |c[1+a0]|, r0 and add r5, c[254+a0], r0 (past c255: zeros)
        Run run;
        set(run.r(1), 0, 0, 0, 2);
        set(run.k.float_constants[3], -1, 2, -3, 4);
        set(run.k.float_constants[0], 9, 9, 9, 9);
        Alu maxa;
        maxa.vop = 29;
        maxa.s1 = maxa.s2 = R(1);
        Alu add;
        add.vop = 0;
        add.vdest = 4;
        add.vmask = 0xF;
        add.s1 = C(1);
        add.const0_addressed = add.const_a0 = add.abs_constants = true;
        add.s2 = R(0);
        Alu past = add;
        past.vdest = 5;
        past.s1 = C(254);
        past.abs_constants = false;
        set(run.r(5), 7, 7, 7, 7);
        CHECK(run.go(program({maxa.ins(), add.ins(), past.ins()})));
        CHECK(run.out.address_register == 2);
        CHECK(same4(run.o(4), 1, 2, 3, 4));
        CHECK(same4(run.o(5), 0, 0, 0, 0));
    }
    { // write mask .x_z_, and saturation (NaN to 0)
        Run run;
        set(run.r(1), 1, 2, 3, 4);
        set(run.r(2), 1, 1, 1, 1);
        set(run.r(4), 9, 9, 9, 9);
        set(run.r(3), 1.5f, -2, N, 0.5f);
        Alu masked;
        masked.vop = 0;
        masked.vdest = 4;
        masked.vmask = 0b0101;
        masked.s1 = R(1);
        masked.s2 = R(2);
        Alu clamped;
        clamped.vop = 0;
        clamped.vdest = 6;
        clamped.vmask = 0xF;
        clamped.vclamp = true;
        clamped.s1 = R(3);
        clamped.s2 = R(0);
        CHECK(run.go(program({masked.ins(), clamped.ins()})));
        CHECK(same4(run.o(4), 2, 9, 4, 9));
        CHECK(same4(run.o(6), 1, 0, 0, 0.5f));
    }
}

void exports() {
    { // vertex: oPos = r1 + r2; o3 = (r1.xy, 0, 1)
        Run run;
        set(run.r(1), 1, 2, 3, 4);
        set(run.r(2), 10, 20, 30, 40);
        Alu pos;
        pos.vop = 0;
        pos.exp = true;
        pos.vdest = 62;
        pos.vmask = 0xF;
        pos.s1 = R(1);
        pos.s2 = R(2);
        Alu interpolator;
        interpolator.vop = 2;
        interpolator.exp = true;
        interpolator.vdest = 3;
        interpolator.vmask = 0b1011;
        interpolator.smask = 0b1000; // w constant 1
        interpolator.srel = true;    // z constant 0
        interpolator.s1 = interpolator.s2 = R(1);
        CHECK(run.go(program({pos.ins(), interpolator.ins()})));
        CHECK(same4(run.out.position, 11, 22, 33, 44));
        CHECK(same4(run.out.interpolators[3], 1, 2, 0, 1));
        CHECK(same4(run.out.interpolators[2], 0, 0, 0, 0));
    }
    { // pixel: oC1 = r1, oDepth = r3.x clamped to 0-1
        Run run;
        set(run.r(1), 0.25f, 0.5f, 0.75f, 1);
        set(run.r(3), 1.5f, 0, 0, 0);
        Alu color;
        color.exp = true;
        color.vdest = 1;
        color.vmask = 0xF;
        color.s1 = color.s2 = R(1);
        Alu depth;
        depth.exp = true;
        depth.vdest = 61;
        depth.vmask = 0b0001;
        depth.s1 = depth.s2 = R(3);
        CHECK(run.go(program({color.ins(), depth.ins()}), ShaderType::Pixel));
        CHECK(same4(run.out.colors[1], 0.25f, 0.5f, 0.75f, 1));
        CHECK(same(run.out.depth[0], 1.0f));
    }
}

// ---- control flow ----

Alu add_to(uint32_t dest) { // r[dest] = r[dest] + c0
    Alu a;
    a.vop = 0;
    a.vdest = dest;
    a.vmask = 0xF;
    a.s1 = R(dest);
    a.s2 = C(0);
    return a;
}

void predicates_and_conditions() {
    Run run;
    set(run.k.float_constants[0], 1, 1, 1, 1);
    run.k.bool_constants[0] = 1u << 5; // b5 set, b6 not
    Alu setp;                          // setp_eq on r1.w = 0: p = true
    setp.sop = 27;
    setp.s3 = R(1);
    Alu yes = add_to(4), no = add_to(5);
    yes.predicated = no.predicated = true;
    yes.condition = true;
    no.condition = false;
    std::vector<Ins> ins = {setp.ins(), yes.ins(), no.ins(), add_to(6).ins(), add_to(7).ins(), add_to(8).ins(),
                            add_to(9).ins()};
    uint32_t base = 3; // six control flow instructions
    std::vector<Cf> cf = {
        exec_cf(false, base, 3, 0),
        cond_exec_pred(base + 3, 1, 0, true),  // runs
        cond_exec_pred(base + 4, 1, 0, false), // skipped
        cond_exec(base + 5, 1, 0, 5, true),    // b5: runs
        cond_exec(base + 6, 1, 0, 6, true),    // b6: skipped
        exec_cf(true, base + 7, 0, 0),
    };
    CHECK(run.go(assemble(cf, ins)));
    CHECK(run.out.predicate == 1);
    CHECK(run.o(4)[0] == 1 && run.o(5)[0] == 0 && run.o(6)[0] == 1 && run.o(7)[0] == 0 && run.o(8)[0] == 1 &&
          run.o(9)[0] == 0);
}

void loops() {
    { // i0: count 3, aL from 1 by 2: r[8 + aL] = r1 for aL 1, 3, 5; r20 counts
        Run run;
        set(run.k.float_constants[0], 1, 1, 1, 1);
        set(run.r(1), 7, 7, 7, 7);
        run.k.loop_constants[0] = 3 | 1u << 8 | 2u << 16;
        Alu write;
        write.vop = 2;
        write.vdest = 8;
        write.vrel = true;
        write.vmask = 0xF;
        write.s1 = write.s2 = R(1);
        uint32_t base = 2;
        std::vector<Cf> cf = {loop_start(3, 0, false), exec_cf(false, base, 2, 0), loop_end(1, 0, false, false),
                              exec_cf(true, base + 2, 0, 0)};
        CHECK(run.go(assemble(cf, {write.ins(), add_to(20).ins()})));
        CHECK(run.o(20)[0] == 3);
        CHECK(run.o(9)[0] == 7 && run.o(11)[0] == 7 && run.o(13)[0] == 7);
        CHECK(run.o(8)[0] == 0 && run.o(10)[0] == 0 && run.o(15)[0] == 0);
    }
    { // a count of 0 skips the body
        Run run;
        set(run.k.float_constants[0], 1, 1, 1, 1);
        uint32_t base = 2;
        std::vector<Cf> cf = {loop_start(3, 1, false), exec_cf(false, base, 1, 0), loop_end(1, 1, false, false),
                              exec_cf(true, base + 1, 0, 0)};
        CHECK(run.go(assemble(cf, {add_to(20).ins()})));
        CHECK(run.o(20)[0] == 0);
    }
    { // a predicated break on p: one pass of five
        Run run;
        set(run.k.float_constants[0], 1, 1, 1, 1);
        run.k.loop_constants[2] = 5;
        Alu setp; // p = (r1.w == 0)
        setp.sop = 27;
        setp.s3 = R(1);
        uint32_t base = 3;
        std::vector<Cf> cf = {exec_cf(false, base, 1, 0), loop_start(4, 2, false), exec_cf(false, base + 1, 1, 0),
                              loop_end(2, 2, true, true), exec_cf(true, base + 2, 0, 0)};
        CHECK(run.go(assemble(cf, {setp.ins(), add_to(20).ins()})));
        CHECK(run.o(20)[0] == 1);
    }
}

void calls_and_jumps() {
    // 0 call 3; 1 r30 += 1; 2 jump 5; 3 r31 += 1; 4 return; 5 r32 += 1, end
    Run run;
    set(run.k.float_constants[0], 1, 1, 1, 1);
    uint32_t base = 3;
    std::vector<Cf> cf = {branch(9, 3),          exec_cf(false, base, 1, 0), branch(11, 5),
                          exec_cf(false, base + 1, 1, 0), ret(),                     exec_cf(true, base + 2, 1, 0)};
    CHECK(run.go(assemble(cf, {add_to(30).ins(), add_to(31).ins(), add_to(32).ins()})));
    CHECK(run.o(30)[0] == 1 && run.o(31)[0] == 1 && run.o(32)[0] == 1);
}

// ---- fetches ----

void put_be32(std::vector<uint8_t> &m, uint32_t dword, uint32_t v) {
    m[dword * 4] = uint8_t(v >> 24);
    m[dword * 4 + 1] = uint8_t(v >> 16);
    m[dword * 4 + 2] = uint8_t(v >> 8);
    m[dword * 4 + 3] = uint8_t(v);
}

void vertex_fetches() {
    Run run;
    run.memory.assign(64 * 4, 0);
    const float floats[] = {1, 2, 3, 4, 5, 6};
    for (uint32_t i = 0; i < 6; i++) put_be32(run.memory, 16 + i, bits(floats[i]));
    put_be32(run.memory, 22, 0x11223344u);
    put_be32(run.memory, 32, 0x80017FFFu);
    // vertex fetch constant 0: 16 dwords at 16, 8-in-32; 1: 4 dwords at 32
    run.k.fetch_constants[0] = 3 | 16u << 2;
    run.k.fetch_constants[1] = 2 | 16u << 2;
    run.k.fetch_constants[2] = 3 | 32u << 2;
    run.k.fetch_constants[3] = 2 | 4u << 2;
    set(run.r(1), 1, 0, 0, 0);  // vertex 1
    set(run.r(7), 10, 0, 0, 0); // vertex 10: past the buffer
    std::vector<Ins> ins = {
        vfetch(false, 2, 1, 0, 0, 57, XYZ1, 3, 0),             // 32_32_32_FLOAT
        vfetch(true, 3, 1, 0, 0, 6, XYZW, 3, 3),               // mini: 8_8_8_8 three dwords on
        vfetch(false, 4, 1, 0, 1, 25, dst(0, 1, 4, 4), 0, 0, true), // 16_16 signed, constant 1
        vfetch(false, 5, 7, 0, 0, 57, XYZ1, 3, 0),             // out of bounds: zeros
    };
    CHECK(run.go(program(ins)));
    CHECK(same4(run.o(2), 4, 5, 6, 1));
    const float s8 = 1.0f / 255.0f;
    CHECK(same4(run.o(3), 68.0f * s8, 51.0f * s8, 34.0f * s8, 17.0f * s8));
    const float s16 = 1.0f / 32767.0f;
    float minus = -32767.0f * s16;
    CHECK(same4(run.o(4), 32767.0f * s16, minus < -1.0f ? -1.0f : minus, 0, 0));
    CHECK(same4(run.o(5), 0, 0, 0, 1));
}

void texture_fetch_stand_in() {
    Run run;
    set(run.r(1), 2, 3, 99, 99);
    // tfetch2D r6, r1.xy, tf5 -> (x + 5, y - 5, x * y, 5)
    CHECK(run.go(program({tfetch(1, 1, 6, 1, 0 | 1u << 2, 5)}), ShaderType::Pixel));
    CHECK(same4(run.o(6), 7, -2, 6, 5));
}

// ---- refusals ----

void refusals() {
    { // eA: memory export
        Run run;
        Alu ea;
        ea.vop = 0;
        ea.exp = true;
        ea.vdest = 32;
        ea.vmask = 0xF;
        CHECK(!run.go(program({ea.ins()})));
    }
    { // getCompTexLOD
        Run run;
        CHECK(!run.go(program({tfetch(17, 1, 6, 1, 0, 5)}), ShaderType::Pixel));
    }
    { // a jump to itself
        Run run;
        CHECK(!run.go(assemble({branch(11, 0), exec_cf(true, 1, 0, 0)}, {})));
    }
}

void reduces_precision() {
    uint32_t third = 0x3EAAAAABu;
    float f;
    std::memcpy(&f, &third, 4);
    CHECK(bits(shader::reduce_precision(f, 21)) == 0x3EAAAAACu);
    CHECK(bits(shader::reduce_precision(1.0f, 21)) == bits(1.0f));
    CHECK(std::isinf(shader::reduce_precision(I, 21)));
    CHECK(bits(shader::reduce_precision(FLT_MAX, 21)) == (bits(FLT_MAX) & ~3u)); // not to infinity
}

} // namespace

int main() {
    vector_operations();
    cube();
    kills_that_do_not();
    setp_push_and_maxa();
    scalar_operations();
    scalar_constant_operations();
    operands_and_results();
    exports();
    predicates_and_conditions();
    loops();
    calls_and_jumps();
    vertex_fetches();
    texture_fetch_stand_in();
    refusals();
    reduces_precision();
    return finish("shader_interpreter");
}
