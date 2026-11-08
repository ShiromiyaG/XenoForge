// VMX and VMX128 floating point: arithmetic, estimates, conversions,
// compares, dot products, the D3D packs (ppc_ops.h). Translated from Xenia's
// ppc_emit_altivec.cc and what its backend does with it (BSD; see
// LICENSE-xenia).
//
// The vector unit runs in non-Java mode, as Xenia keeps it (its MXCSR for
// vector work has DAZ and FTZ): a denormal input reads as a zero of its sign
// and a denormal result is written as one. That and the NaNs are done here
// on the bits, lane by lane, so nothing depends on the host's floating-point
// modes (the host stays in round to nearest, without FTZ/DAZ). Java mode
// (VSCR[NJ] cleared by mtvscr; Context::vscr_java) keeps denormals, operands
// and results, as IEEE 754 does: every helper below takes J (Java mode), and
// each instruction runs the J = false form inline and the J = true form out of
// line (XF_VFP: a load and a branch on the common path). The D3D packs and
// the estimates' tables are the same in both modes (what the console's packs
// do with denormals is not known; an estimate of a denormal divides).
//
// Where Xenia's x86 results differ from the console-generated tests
// (instr__gen_v*, the cases its skip.txt skips), the console is followed:
// NaN operands come out quieted, the first in operand order vA, vB, vC (the
// addend before the multiplier in vmaddfp; vnmsubfp does not negate them);
// an invalid operation gives 0x7FC00000, not x86's 0xFFC00000; vmaxfp and
// vminfp quiet a signaling NaN; vcmpbfp's NaN and CR6 rules; vrefp's
// estimate (as much of its table as the tests hold). All the tests pass but
// 17 estimate cases (vexptefp, vlogefp: the console's own estimates, which
// Xenia does not model either and its tests hold too little of to rebuild).
// Xenia's hand-written vrefp tests expect exact reciprocals where the
// console's give its estimate; tests/ppc/skip.txt skips them.

#pragma once

#include "xenoforge/ppc_ops.h"

namespace xf::ppc::ops {

namespace vfp {

constexpr uint32_t QNAN = 0x7FC00000u; // the NaN an invalid operation makes

inline float flt(uint32_t v) { return lib::bit_cast<float>(v); }
inline uint32_t bits(float v) { return lib::bit_cast<uint32_t>(v); }
inline bool is_nan(uint32_t v) { return (v & 0x7FFFFFFFu) > 0x7F800000u; }
inline uint32_t quiet(uint32_t v) { return v | 0x00400000u; }
// non-Java mode, both ways: a denormal is a zero of its sign
inline uint32_t flush(uint32_t v) { return v & 0x7F800000u ? v : v & 0x80000000u; }
// the flush in non-Java mode, nothing in Java mode
template <bool J> inline uint32_t flush_j(uint32_t v) {
    if constexpr (J) return v;
    else return flush(v);
}
// a result the host computed from operands that were not NaN
template <bool J = false> inline uint32_t result(float r) { return r != r ? QNAN : flush_j<J>(bits(r)); }
inline uint32_t mask(bool b) { return b ? 0xFFFFFFFFu : 0u; }

template <typename F> inline Vec128 lanes(const Vec128 &a, F f) {
    Vec128 r;
    for (int k = 0; k < 4; k++) r.u32[k] = f(a.u32[k]);
    return r;
}
template <typename F> inline Vec128 lanes(const Vec128 &a, const Vec128 &b, F f) {
    Vec128 r;
    for (int k = 0; k < 4; k++) r.u32[k] = f(a.u32[k], b.u32[k]);
    return r;
}
template <typename F> inline Vec128 lanes(const Vec128 &a, const Vec128 &b, const Vec128 &c, F f) {
    Vec128 r;
    for (int k = 0; k < 4; k++) r.u32[k] = f(a.u32[k], b.u32[k], c.u32[k]);
    return r;
}

// ---- arithmetic ----

template <bool J = false> inline uint32_t add(uint32_t a, uint32_t b) {
    a = flush_j<J>(a), b = flush_j<J>(b);
    if (is_nan(a) || is_nan(b)) return quiet(is_nan(a) ? a : b);
    return result<J>(flt(a) + flt(b));
}
template <bool J = false> inline uint32_t sub(uint32_t a, uint32_t b) {
    a = flush_j<J>(a), b = flush_j<J>(b);
    if (is_nan(a) || is_nan(b)) return quiet(is_nan(a) ? a : b);
    return result<J>(flt(a) - flt(b));
}
template <bool J = false> inline uint32_t mul(uint32_t a, uint32_t b) {
    a = flush_j<J>(a), b = flush_j<J>(b);
    if (is_nan(a) || is_nan(b)) return quiet(is_nan(a) ? a : b);
    return result<J>(flt(a) * flt(b));
}
// vmaddfp: a * c + b rounded once, as Xenia's MUL_ADD (vfmadd213ps); the
// console fuses too (rounding the product first fails 588 of the 15600 tests).
// lib::fma is one instruction where the build has FMA, a libm call otherwise.
template <bool J = false> inline uint32_t madd(uint32_t a, uint32_t b, uint32_t c) {
    a = flush_j<J>(a), b = flush_j<J>(b), c = flush_j<J>(c);
    if (is_nan(a) || is_nan(b) || is_nan(c)) return quiet(is_nan(a) ? a : is_nan(b) ? b : c);
    return result<J>(lib::fma(flt(a), flt(c), flt(b)));
}
// vnmsubfp: -(a * c - b) rounded once (Xenia: Neg(MulSub)); a NaN is not negated
template <bool J = false> inline uint32_t nmsub(uint32_t a, uint32_t b, uint32_t c) {
    a = flush_j<J>(a), b = flush_j<J>(b), c = flush_j<J>(c);
    if (is_nan(a) || is_nan(b) || is_nan(c)) return quiet(is_nan(a) ? a : is_nan(b) ? b : c);
    return result<J>(-lib::fma(flt(a), flt(c), -flt(b)));
}
// vmaxfp/vminfp: +0 above -0; a NaN operand comes out quieted, vA's first
// (Xenia passes a signaling NaN through unchanged; the console tests quiet it)
template <bool J = false> inline uint32_t max(uint32_t a, uint32_t b) {
    a = flush_j<J>(a), b = flush_j<J>(b);
    if (is_nan(a) || is_nan(b)) return quiet(is_nan(a) ? a : b);
    float x = flt(a), y = flt(b);
    return x > y ? a : y > x ? b : a & b;
}
template <bool J = false> inline uint32_t min(uint32_t a, uint32_t b) {
    a = flush_j<J>(a), b = flush_j<J>(b);
    if (is_nan(a) || is_nan(b)) return quiet(is_nan(a) ? a : b);
    float x = flt(a), y = flt(b);
    return x < y ? a : y < x ? b : a | b;
}

// ---- estimates ----

// vrefp: the console's estimate, built as vrsqrtefp's: 32 segments by the
// top 5 fraction bits, a line in each (base << 10 less slope times the next
// 9 bits; the low 9 are not used), normalized, rounded to nearest even to 23
// bits. Xenia divides. This table is read back from the console's tests
// (instr__gen_vrefp, every case of which it passes): 16 segments whole, from
// two to six distinct inputs in most, which fit one line each; of segment 16
// only its start (1.5; slope 0: used there alone). The 15 segments no test
// reaches (0) divide, as Xenia does.
template <bool J = false> inline uint32_t recip(uint32_t v) {
    static constexpr uint32_t TABLE[32] = {
        0x0F84FFF8, 0x0E9CF838, 0x0DC4F0EA, 0x0D00EA08, 0x0C4CE388, 0,          0x0B10D790, 0,
        0x09FCCCC8, 0,          0,          0,          0,          0x07ECB608, 0,          0,
        0x0000AAA8, 0,          0,          0x062CA09E, 0,          0x05B89A90, 0,          0x056C94F8,
        0,          0x050C8FBE, 0,          0,          0x04688884, 0,          0x0420841C, 0x0418820C,
    };
    v = flush_j<J>(v);
    if (is_nan(v)) return quiet(v);
    uint32_t sign = v & 0x80000000u, exponent = (v >> 23) & 0xFF, mantissa = v & 0x7FFFFF;
    uint32_t entry = TABLE[mantissa >> 18], low = (mantissa >> 9) & 511;
    // zeros and infinities, and what the table does not hold
    if (!exponent || exponent == 255 || !entry || (!(entry >> 16) && low)) return result<J>(1.0f / flt(v));
    int32_t estimate = int32_t((entry & 0xFFFF) << 10) - int32_t(low * (entry >> 16));
    int32_t result_exponent = 253 - int32_t(exponent);
    if (!(estimate & (1 << 25))) {
        int zeros = lib::countl_zero(uint32_t(estimate) & 0x1FFFFFFu);
        result_exponent += 6 - zeros;
        estimate = int32_t(uint32_t(estimate) << (zeros - 6));
    }
    if ((estimate & 5) && (estimate & 2)) estimate += 4;
    if (result_exponent <= 0) return J ? result<J>(1.0f / flt(v)) : sign; // a denormal result: flushed
    return sign | uint32_t(result_exponent) << 23 | ((uint32_t(estimate) >> 2) & 0x7FFFFF);
}
// vrsqrtefp: the console's table and interpolation, as Xenia's
// EmitScalarVRsqrteHelper (and the PpcVrsqrtefpLane of its a64 backend)
template <bool J = false> inline uint32_t rsqrte(uint32_t v) {
    static constexpr uint32_t TABLE[32] = {
        0x0568B4FD, 0x04F3AF97, 0x048DAAA5, 0x0435A618, 0x03E7A1E4, 0x03A29DFE, 0x03659A5C, 0x032E96F8,
        0x02FC93CA, 0x02D090CE, 0x02A88DFE, 0x02838B57, 0x026188D4, 0x02438673, 0x02268431, 0x020B820B,
        0x03D27FFA, 0x03807C29, 0x033878AA, 0x02F97572, 0x02C27279, 0x02926FB7, 0x02666D26, 0x023F6AC0,
        0x021D6881, 0x01FD6665, 0x01E16468, 0x01C76287, 0x01AF60C1, 0x01995F12, 0x01855D79, 0x01735BF4,
    };
    uint32_t sign = v & 0x80000000u, exponent = (v >> 23) & 0xFF, mantissa = v & 0x7FFFFF;
    if (v == 0xFF800000u) return QNAN;                      // -inf
    if (!exponent && !(J && mantissa)) return sign ? 0xFF800000u : 0x7F800000u; // +-0 and denormals: +-inf
    if (!exponent) return sign ? QNAN : result<J>(1.0f / lib::sqrt(flt(v))); // Java mode: a denormal's
    if (exponent == 255) return mantissa ? quiet(v) : 0;    // NaN; +inf: +0
    if (sign) return QNAN;
    uint32_t index = (((uint32_t(int32_t(exponent) - 127) << 4) & 16) | mantissa >> 19) ^ 16;
    uint32_t entry = TABLE[index];
    int32_t result_exponent = (127 - int32_t(exponent)) >> 1;
    int32_t estimate = int32_t((entry << 10) & 0x3FFFC00u) - int32_t(((mantissa >> 9) & 1023) * (entry >> 16));
    if (!(estimate & (1 << 25))) {
        int zeros = lib::countl_zero(uint32_t(estimate) & 0x1FFFFFFu);
        result_exponent += 6 - zeros;
        estimate = int32_t(uint32_t(estimate) << (zeros - 6));
    }
    if ((estimate & 5) && (estimate & 2)) estimate += 4;
    return flush_j<J>((uint32_t(result_exponent << 23) + 0x3F800000u) | ((uint32_t(estimate) >> 2) & 0x7FFFFF));
}
// vexptefp, vlogefp: Xenia calls exp2 and log2
template <bool J = false> inline uint32_t exp2e(uint32_t v) {
    v = flush_j<J>(v);
    if (is_nan(v)) return quiet(v);
    return result<J>(lib::exp2(flt(v)));
}
template <bool J = false> inline uint32_t log2e(uint32_t v) {
    v = flush_j<J>(v);
    if (is_nan(v)) return quiet(v);
    if (v & 0x80000000u) return v == 0x80000000u ? 0xFF800000u : QNAN;
    return result<J>(lib::log2(flt(v)));
}

// ---- rounding to an integer value (vrfi*), on the bits ----

enum Round { NEAREST, FLOOR, CEIL, TRUNC };
template <Round mode, bool J = false> inline uint32_t round(uint32_t v) {
    v = flush_j<J>(v);
    if (is_nan(v)) return quiet(v);
    uint32_t exponent = (v >> 23) & 0xFF, sign = v & 0x80000000u;
    if (exponent >= 150) return v; // integral already, or infinite
    if (!(v & 0x7FFFFFFFu)) return v;
    if (exponent < 127) { // |v| < 1
        bool one = mode == NEAREST ? exponent == 126 && (v & 0x7FFFFF) : mode == FLOOR ? sign : mode == CEIL && !sign;
        return sign | (one ? 0x3F800000u : 0);
    }
    uint32_t fraction = (1u << (150 - exponent)) - 1, rest = v & fraction, t = v & ~fraction;
    if (!rest) return v;
    bool up = mode == FLOOR ? sign : mode == CEIL ? !sign : false;
    if (mode == NEAREST) {
        uint32_t half = (fraction >> 1) + 1;
        up = rest > half || (rest == half && (t & (half << 1)));
    }
    return up ? t + fraction + 1 : t;
}

// ---- conversions ----

// float(word) * 2^-uimm: rounded once by the conversion, the scaling exact
inline Vec128 from_signed(const Vec128 &a, uint32_t uimm) {
    float scale = flt((127 - uimm) << 23);
    return lanes(a, [scale](uint32_t v) { return bits(float(int32_t(v)) * scale); });
}
inline Vec128 from_unsigned(const Vec128 &a, uint32_t uimm) {
    float scale = flt((127 - uimm) << 23);
    return lanes(a, [scale](uint32_t v) { return bits(float(v) * scale); });
}
// v * 2^uimm truncated and saturated, a NaN converted to 0. VSCR[SAT] is set
// when a lane saturates and never cleared here (Xenia's DidSaturate is a
// stub that stores 0)
template <bool J = false> inline uint32_t to_signed(uint32_t v, uint32_t uimm, bool &sat) {
    v = flush_j<J>(v);
    if (is_nan(v)) return 0;
    double x = double(flt(v)) * double(1ull << uimm);
    if (x >= 2147483648.0) return sat = true, 0x7FFFFFFFu;
    if (x < -2147483648.0) return sat = true, 0x80000000u;
    return uint32_t(int32_t(x));
}
template <bool J = false> inline uint32_t to_unsigned(uint32_t v, uint32_t uimm, bool &sat) {
    v = flush_j<J>(v);
    if (is_nan(v)) return 0;
    double x = double(flt(v)) * double(1ull << uimm);
    if (x >= 4294967296.0) return sat = true, 0xFFFFFFFFu;
    if (x <= -1.0) return sat = true, 0u;
    return x < 1.0 ? 0u : uint32_t(x);
}
template <bool J = false> inline Vec128 to_signed(const Vec128 &a, uint32_t uimm, uint8_t &vscr_sat) {
    bool sat = false;
    Vec128 r = lanes(a, [&](uint32_t v) { return to_signed<J>(v, uimm, sat); });
    if (sat) vscr_sat = 1;
    return r;
}
template <bool J = false> inline Vec128 to_unsigned(const Vec128 &a, uint32_t uimm, uint8_t &vscr_sat) {
    bool sat = false;
    Vec128 r = lanes(a, [&](uint32_t v) { return to_unsigned<J>(v, uimm, sat); });
    if (sat) vscr_sat = 1;
    return r;
}

// ---- compares ----

template <bool J = false> inline Vec128 cmp_eq(const Vec128 &a, const Vec128 &b) {
    return lanes(a, b, [](uint32_t x, uint32_t y) { return mask(flt(flush_j<J>(x)) == flt(flush_j<J>(y))); });
}
template <bool J = false> inline Vec128 cmp_ge(const Vec128 &a, const Vec128 &b) {
    return lanes(a, b, [](uint32_t x, uint32_t y) { return mask(flt(flush_j<J>(x)) >= flt(flush_j<J>(y))); });
}
template <bool J = false> inline Vec128 cmp_gt(const Vec128 &a, const Vec128 &b) {
    return lanes(a, b, [](uint32_t x, uint32_t y) { return mask(flt(flush_j<J>(x)) > flt(flush_j<J>(y))); });
}
// vcmpbfp: bit 0 set when not a <= b, bit 1 when not a >= -b, so both for a
// NaN (Xenia's a > b leaves bit 0 clear; the console tests set it)
template <bool J = false> inline Vec128 cmp_bounds(const Vec128 &a, const Vec128 &b) {
    return lanes(a, b, [](uint32_t x, uint32_t y) {
        float fa = flt(flush_j<J>(x)), fb = flt(flush_j<J>(y));
        return (fa <= fb ? 0 : 0x80000000u) | (fa >= -fb ? 0 : 0x40000000u);
    });
}
// the result to vD; Rc: CR6 as UpdateCR6 (all true, none true)
inline void compared(Context &c, uint32_t vd, const Vec128 &v, bool rc) {
    if (rc) update_cr6(c, v);
    c.v[vd] = v;
}
// vcmpbfp's Rc: CR6 bit 2 when every element is within bounds, the rest 0
// (Xenia's UpdateCR6 also sets bit 0 when none is; the console tests do not)
inline void bounded(Context &c, uint32_t vd, const Vec128 &v, bool rc) {
    if (rc) c.cr[24] = 0, c.cr[25] = 0, c.cr[26] = (v.u32[0] | v.u32[1] | v.u32[2] | v.u32[3]) == 0, c.cr[27] = 0;
    c.v[vd] = v;
}

// ---- dot products (vmsum3fp128, vmsum4fp128), as Xenia's DOT_PRODUCT_3/4:
// the products in double (exact), summed in double as (p0 + p2) + p1 or
// (p0 + p2) + (p1 + p3), rounded once to float; a float overflow of a finite
// sum gives the default NaN; the result goes to all four lanes ----

// a NaN result, through the operations as x86 runs them in Xenia: the first
// operand's NaN, else the second's; an invalid operation makes the default
// NaN (the console's, not x86's negative one)
inline double widen_nan(uint32_t v) { // cvtps2pd: payload kept, quieted
    return lib::bit_cast<double>(uint64_t(v & 0x80000000u) << 32 | 0x7FF8000000000000ull |
                                 uint64_t(v & 0x3FFFFF) << 29);
}
inline double x86_op(double x, double y, double r) {
    return x != x ? x : y != y ? y : r != r ? lib::bit_cast<double>(0x7FF8000000000000ull) : r;
}
template <bool J = false> inline uint32_t dot_nan(const Vec128 &a, const Vec128 &b, int n) {
    double p[4];
    for (int k = 0; k < 4; k++) {
        uint32_t x = flush_j<J>(a.u32[k]), y = flush_j<J>(b.u32[k]);
        double dx = is_nan(x) ? widen_nan(x) : double(flt(x)), dy = is_nan(y) ? widen_nan(y) : double(flt(y));
        p[k] = x86_op(dx, dy, dx * dy);
    }
    double s = x86_op(p[0], p[2], p[0] + p[2]);
    if (n == 4) {
        double t = x86_op(p[1], p[3], p[1] + p[3]);
        s = x86_op(s, t, s + t);
    } else {
        s = x86_op(s, p[1], s + p[1]);
    }
    uint64_t v = lib::bit_cast<uint64_t>(s); // cvtsd2ss of a NaN: the top of the payload
    return uint32_t(v >> 32 & 0x80000000u) | 0x7FC00000u | uint32_t(v >> 29 & 0x3FFFFF);
}
template <bool J = false> inline uint32_t dot(const Vec128 &a, const Vec128 &b, int n) {
    double p[4];
    for (int k = 0; k < 4; k++) p[k] = double(flt(flush_j<J>(a.u32[k]))) * double(flt(flush_j<J>(b.u32[k])));
    double sum = n == 4 ? (p[0] + p[2]) + (p[1] + p[3]) : (p[0] + p[2]) + p[1];
    if (sum != sum) return dot_nan<J>(a, b, n);
    float r = float(sum);
    if (lib::isinf(r) && !lib::isinf(sum)) return QNAN;
    return flush_j<J>(bits(r));
}
inline Vec128 splat(uint32_t v) {
    Vec128 r;
    r.u32[0] = r.u32[1] = r.u32[2] = r.u32[3] = v;
    return r;
}

// ---- the D3D packs (vpkd3d128, vupkd3d128), as Xenia's PACK and UNPACK ----

// float_to_xenos_half and xenos_half_to_float (xenia/base/math.h) without
// denormals: the extended range, saturating, no infinity or NaN. Unpacking a
// negative zero or denormal half gives -0.0 as xenos_half_to_float (and
// Xenia's FLOAT16_2 and constant paths) do; the SSE path of its FLOAT16_4
// unpack gives -2^-15 * (1 + m / 1024) there, masking the exponent bytewise.
inline uint32_t to_half(uint32_t v, bool round_to_nearest_even) {
    uint32_t abs = v & 0x7FFFFFFFu, h;
    if (abs >= 0x47FFE000u) {
        h = 0x7FFF;
    } else {
        h = abs < 0x38800000u ? 0 : abs + 0xC8000000u;
        if (round_to_nearest_even) h += 0xFFF + ((h >> 13) & 1);
        h = (h >> 13) & 0x7FFF;
    }
    return h | (v & 0x80000000u) >> 16;
}
inline uint32_t from_half(uint32_t h) {
    uint32_t mantissa = h & 0x3FF, exponent = (h >> 10) & 0x1F;
    uint32_t magnitude = exponent ? (exponent + 112) << 23 | mantissa << 13 : 0;
    return (h & 0x8000) << 16 | magnitude;
}
// vmaxps then vminps against the format's range (a NaN takes the minimum)
inline uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi) {
    float x = flt(v), l = flt(lo), h = flt(hi);
    x = x > l ? x : l;
    x = x < h ? x : h;
    return bits(x);
}

enum PackType { D3DCOLOR, SHORT_2, UINT_2101010, FLOAT16_2, SHORT_4, FLOAT16_4, ULONG_4202020 };

// the packed lanes (in the last word or two, or in all four for
// UINT_2101010); false for a type Xenia does not handle
inline bool pack(const Vec128 &s, uint32_t type, Vec128 &r) {
    r = Vec128{};
    const uint32_t *v = s.u32;
    switch (type) {
    case D3DCOLOR: {
        uint32_t c[4];
        for (int k = 0; k < 4; k++) c[k] = clamp(v[k], 0x40400000u, 0x404000FFu) & 0xFF;
        r.u32[3] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
        return true;
    }
    case SHORT_2:
    case SHORT_4: {
        uint32_t c[4];
        for (int k = 0; k < 4; k++) c[k] = clamp(v[k], 0x403F8001u, 0x40407FFFu) & 0xFFFF;
        if (type == SHORT_2)
            r.u32[3] = c[0] << 16 | c[1];
        else
            r.u32[2] = c[0] << 16 | c[1], r.u32[3] = c[2] << 16 | c[3];
        return true;
    }
    case UINT_2101010: {
        uint32_t x = clamp(v[0], 0x403FFE01u, 0x404001FFu) & 0x3FF, y = clamp(v[1], 0x403FFE01u, 0x404001FFu) & 0x3FF,
                 z = clamp(v[2], 0x403FFE01u, 0x404001FFu) & 0x3FF, w = clamp(v[3], 0x40400000u, 0x40400003u) & 0x3;
        r = splat(x | y << 10 | z << 20 | w << 30);
        return true;
    }
    case ULONG_4202020: {
        uint32_t x = clamp(v[0], 0x40380001u, 0x4047FFFFu) & 0xFFFFF,
                 y = clamp(v[1], 0x40380001u, 0x4047FFFFu) & 0xFFFFF,
                 z = clamp(v[2], 0x40380001u, 0x4047FFFFu) & 0xFFFFF, w = clamp(v[3], 0x40400000u, 0x4040000Fu) & 0xF;
        r.u32[2] = w << 28 | z << 8 | y >> 12;
        r.u32[3] = y << 20 | x;
        return true;
    }
    case FLOAT16_2: // float_to_xenos_half's default: toward zero
        r.u32[3] = to_half(v[0], false) << 16 | to_half(v[1], false);
        return true;
    case FLOAT16_4: // emit_fast_f16_pack: to nearest even
        r.u32[2] = to_half(v[0], true) << 16 | to_half(v[1], true);
        r.u32[3] = to_half(v[2], true) << 16 | to_half(v[3], true);
        return true;
    }
    return false;
}
// where the packed lanes go in vD: hir's MakePermuteMask(sel_x, x, sel_y, y,
// ...), lane k from (control >> 8k) & 3 of vD (sel 0) or the packed (sel 1)
constexpr uint32_t permute_mask(uint32_t sx, uint32_t x, uint32_t sy, uint32_t y, uint32_t sz, uint32_t z, uint32_t sw,
                                uint32_t w) {
    return x | sx << 2 | y << 8 | sy << 10 | z << 16 | sz << 18 | w << 24 | sw << 26;
}
inline bool pack_d3d(Vec128 &d, const Vec128 &b, uint32_t imm, uint32_t shift) {
    static constexpr uint32_t MASKS[3][4] = {
        // VPACK_32
        {permute_mask(0, 0, 0, 1, 0, 2, 1, 3), permute_mask(0, 0, 0, 1, 1, 3, 0, 3),
         permute_mask(0, 0, 1, 3, 0, 2, 0, 3), permute_mask(1, 3, 0, 1, 0, 2, 0, 3)},
        // 64 bits
        {permute_mask(0, 0, 0, 1, 1, 2, 1, 3), permute_mask(0, 0, 1, 2, 1, 3, 0, 3),
         permute_mask(1, 2, 1, 3, 0, 2, 0, 3), permute_mask(1, 3, 0, 1, 0, 2, 0, 3)},
        {permute_mask(0, 0, 0, 1, 1, 2, 1, 3), permute_mask(0, 0, 1, 2, 1, 3, 0, 3),
         permute_mask(1, 2, 1, 3, 0, 2, 0, 3), permute_mask(0, 0, 0, 1, 0, 2, 1, 2)},
    };
    Vec128 packed;
    if (!(imm & 3) || !pack(b, imm >> 2, packed)) return false;
    uint32_t control = MASKS[(imm & 3) - 1][shift];
    Vec128 r;
    for (int k = 0; k < 4; k++) {
        uint32_t lane = control >> (8 * k);
        r.u32[k] = (lane & 4 ? packed : d).u32[lane & 3];
    }
    d = r;
    return true;
}
inline bool unpack_d3d(Vec128 &d, const Vec128 &s, uint32_t type) {
    const uint32_t w2 = s.u32[2], w3 = s.u32[3];
    Vec128 r;
    switch (type) {
    case D3DCOLOR: // ARGB to RGBA, each 1.0 + byte * 2^-23
        r.u32[0] = 0x3F800000u | (w3 >> 16 & 0xFF), r.u32[1] = 0x3F800000u | (w3 >> 8 & 0xFF);
        r.u32[2] = 0x3F800000u | (w3 & 0xFF), r.u32[3] = 0x3F800000u | w3 >> 24;
        break;
    case SHORT_2:
    case SHORT_4: { // 3.0 + n * 2^-22; -32768 is a NaN
        uint32_t n[4] = {w3 >> 16, w3 & 0xFFFF, 0, 0};
        if (type == SHORT_4) n[0] = w2 >> 16, n[1] = w2 & 0xFFFF, n[2] = w3 >> 16, n[3] = w3 & 0xFFFF;
        for (int k = 0; k < 4; k++) r.u32[k] = 0x40400000u + uint32_t(int32_t(int16_t(n[k])));
        if (type == SHORT_2) r.u32[2] = 0, r.u32[3] = 0x3F800000u;
        for (uint32_t &v : r.u32) v = v == 0x403F8000u ? QNAN : v;
        break;
    }
    case UINT_2101010: // xyz 3.0 + n * 2^-22 (n of 10 bits, -512 a NaN), w 1.0 + n * 2^-23
        for (int k = 0; k < 3; k++) {
            uint32_t v = 0x40400000u + uint32_t(int32_t(w3 << (22 - 10 * k)) >> 22);
            r.u32[k] = v == 0x403FFE00u ? QNAN : v;
        }
        r.u32[3] = 0x3F800000u + (w3 >> 30);
        break;
    case ULONG_4202020: { // xyz of 20 bits (-2^19 a NaN), w of 4
        uint32_t n[3] = {w3, w3 >> 20 | w2 << 12, w2 >> 8};
        for (int k = 0; k < 3; k++) {
            uint32_t v = 0x40400000u + uint32_t(int32_t(n[k] << 12) >> 12);
            r.u32[k] = v == 0x40380000u ? QNAN : v;
        }
        r.u32[3] = 0x3F800000u + (w2 >> 28);
        break;
    }
    case FLOAT16_2:
        r.u32[0] = from_half(w3 >> 16), r.u32[1] = from_half(w3 & 0xFFFF), r.u32[2] = 0, r.u32[3] = 0x3F800000u;
        break;
    case FLOAT16_4:
        r.u32[0] = from_half(w2 >> 16), r.u32[1] = from_half(w2 & 0xFFFF);
        r.u32[2] = from_half(w3 >> 16), r.u32[3] = from_half(w3 & 0xFFFF);
        break;
    default: return false;
    }
    d = r;
    return true;
}

// The instruction in non-Java mode inline (J false), in Java mode (VSCR[NJ]
// clear: Context::vscr_java) out of line: a load and a branch not taken on the
// common path
template <typename F> [[gnu::cold, gnu::noinline]] void java_mode(F f) { f(); }
#define XF_VFP(...)                                                                                                    \
    if (c.vscr_java) [[unlikely]] {                                                                                    \
        vfp::java_mode([&] {                                                                                           \
            [[maybe_unused]] constexpr bool J = true;                                                                  \
            __VA_ARGS__;                                                                                               \
        });                                                                                                            \
    } else {                                                                                                           \
        [[maybe_unused]] constexpr bool J = false;                                                                     \
        __VA_ARGS__;                                                                                                   \
    }

} // namespace vfp

// ---- arithmetic ----

XF_OP(vaddfp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VA], c.v[i.VX.VB], vfp::add<J>)); }
XF_OP(vaddfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], vfp::add<J>)); }
XF_OP(vsubfp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VA], c.v[i.VX.VB], vfp::sub<J>)); }
XF_OP(vsubfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], vfp::sub<J>)); }
XF_OP(vmulfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], vfp::mul<J>)); }
// (VD) <- (VA) * (VC) + (VB)
XF_OP(vmaddfp) { XF_VFP(c.v[i.VXA.VD] = vfp::lanes(c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC], vfp::madd<J>)); }
// (VD) <- (VA) * (VB) + (VD)
XF_OP(vmaddfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VD128], c.v[VX128_VB128], vfp::madd<J>)); }
// (VD) <- (VA) * (VD) + (VB)
XF_OP(vmaddcfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], c.v[VX128_VD128], vfp::madd<J>)); }
// (VD) <- -((VA) * (VC) - (VB))
XF_OP(vnmsubfp) { XF_VFP(c.v[i.VXA.VD] = vfp::lanes(c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC], vfp::nmsub<J>)); }
// (VD) <- -((VA) * (VB) - (VD))
XF_OP(vnmsubfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VD128], c.v[VX128_VB128], vfp::nmsub<J>)); }
XF_OP(vmaxfp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VA], c.v[i.VX.VB], vfp::max<J>)); }
XF_OP(vmaxfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], vfp::max<J>)); }
XF_OP(vminfp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VA], c.v[i.VX.VB], vfp::min<J>)); }
XF_OP(vminfp128) { XF_VFP(c.v[VX128_VD128] = vfp::lanes(c.v[VX128_VA128], c.v[VX128_VB128], vfp::min<J>)); }

// ---- estimates ----

XF_OP(vrefp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::recip<J>)); }
XF_OP(vrefp128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::recip<J>)); }
XF_OP(vrsqrtefp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::rsqrte<J>)); }
XF_OP(vrsqrtefp128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::rsqrte<J>)); }
XF_OP(vexptefp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::exp2e<J>)); }
XF_OP(vexptefp128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::exp2e<J>)); }
XF_OP(vlogefp) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::log2e<J>)); }
XF_OP(vlogefp128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::log2e<J>)); }

// ---- rounding ----

XF_OP(vrfim) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::round<vfp::FLOOR, J>)); }
XF_OP(vrfim128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::round<vfp::FLOOR, J>)); }
XF_OP(vrfin) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::round<vfp::NEAREST, J>)); }
XF_OP(vrfin128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::round<vfp::NEAREST, J>)); }
XF_OP(vrfip) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::round<vfp::CEIL, J>)); }
XF_OP(vrfip128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::round<vfp::CEIL, J>)); }
XF_OP(vrfiz) { XF_VFP(c.v[i.VX.VD] = vfp::lanes(c.v[i.VX.VB], vfp::round<vfp::TRUNC, J>)); }
XF_OP(vrfiz128) { XF_VFP(c.v[VX128_3_VD128] = vfp::lanes(c.v[VX128_3_VB128], vfp::round<vfp::TRUNC, J>)); }

// ---- conversions (the immediate in VA, or VX128_3's IMM) ----

XF_OP(vcfsx) { c.v[i.VX.VD] = vfp::from_signed(c.v[i.VX.VB], i.VX.VA); }
XF_OP(vcsxwfp128) { c.v[VX128_3_VD128] = vfp::from_signed(c.v[VX128_3_VB128], VX128_3_IMM); }
XF_OP(vcfux) { c.v[i.VX.VD] = vfp::from_unsigned(c.v[i.VX.VB], i.VX.VA); }
XF_OP(vcuxwfp128) { c.v[VX128_3_VD128] = vfp::from_unsigned(c.v[VX128_3_VB128], VX128_3_IMM); }
XF_OP(vctsxs) { XF_VFP(c.v[i.VX.VD] = vfp::to_signed<J>(c.v[i.VX.VB], i.VX.VA, c.vscr_sat)); }
XF_OP(vcfpsxws128) { XF_VFP(c.v[VX128_3_VD128] = vfp::to_signed<J>(c.v[VX128_3_VB128], VX128_3_IMM, c.vscr_sat)); }
XF_OP(vctuxs) { XF_VFP(c.v[i.VX.VD] = vfp::to_unsigned<J>(c.v[i.VX.VB], i.VX.VA, c.vscr_sat)); }
XF_OP(vcfpuxws128) { XF_VFP(c.v[VX128_3_VD128] = vfp::to_unsigned<J>(c.v[VX128_3_VB128], VX128_3_IMM, c.vscr_sat)); }

// ---- compares; Rc sets CR6 ----

XF_OP(vcmpeqfp) { XF_VFP(vfp::compared(c, i.VXR.VD, vfp::cmp_eq<J>(c.v[i.VXR.VA], c.v[i.VXR.VB]), i.VXR.Rc)); }
XF_OP(vcmpeqfp128) { XF_VFP(vfp::compared(c, VX128_R_VD128, vfp::cmp_eq<J>(c.v[VX128_R_VA128], c.v[VX128_R_VB128]), i.VX128_R.Rc)); }
XF_OP(vcmpgefp) { XF_VFP(vfp::compared(c, i.VXR.VD, vfp::cmp_ge<J>(c.v[i.VXR.VA], c.v[i.VXR.VB]), i.VXR.Rc)); }
XF_OP(vcmpgefp128) { XF_VFP(vfp::compared(c, VX128_R_VD128, vfp::cmp_ge<J>(c.v[VX128_R_VA128], c.v[VX128_R_VB128]), i.VX128_R.Rc)); }
XF_OP(vcmpgtfp) { XF_VFP(vfp::compared(c, i.VXR.VD, vfp::cmp_gt<J>(c.v[i.VXR.VA], c.v[i.VXR.VB]), i.VXR.Rc)); }
XF_OP(vcmpgtfp128) { XF_VFP(vfp::compared(c, VX128_R_VD128, vfp::cmp_gt<J>(c.v[VX128_R_VA128], c.v[VX128_R_VB128]), i.VX128_R.Rc)); }
XF_OP(vcmpbfp) { XF_VFP(vfp::bounded(c, i.VXR.VD, vfp::cmp_bounds<J>(c.v[i.VXR.VA], c.v[i.VXR.VB]), i.VXR.Rc)); }
XF_OP(vcmpbfp128) { XF_VFP(vfp::bounded(c, VX128_R_VD128, vfp::cmp_bounds<J>(c.v[VX128_R_VA128], c.v[VX128_R_VB128]), i.VX128_R.Rc)); }

// ---- dot products ----

// (VD.xyzw) <- (VA.x * VB.x) + (VA.y * VB.y) + (VA.z * VB.z)
XF_OP(vmsum3fp128) { XF_VFP(c.v[VX128_VD128] = vfp::splat(vfp::dot<J>(c.v[VX128_VA128], c.v[VX128_VB128], 3))); }
// (VD.xyzw) <- (VA.x * VB.x) + (VA.y * VB.y) + (VA.z * VB.z) + (VA.w * VB.w)
XF_OP(vmsum4fp128) { XF_VFP(c.v[VX128_VD128] = vfp::splat(vfp::dot<J>(c.v[VX128_VA128], c.v[VX128_VB128], 4))); }

// ---- the D3D packs; an encoding Xenia does not handle is reported as unimplemented ----

// pack VB as the type IMM >> 2, insert it into VD by IMM & 3 (32 or 64 bits)
// and z (the position)
XF_OP(vpkd3d128) {
    uint32_t vd = i.VX128_4.VD128l | (i.VX128_4.VD128h << 5), vb = i.VX128_4.VB128l | (i.VX128_4.VB128h << 5);
    if (!vfp::pack_d3d(c.v[vd], c.v[vb], i.VX128_4.IMM, i.VX128_4.z) && c.system && c.system->unimplemented)
        c.system->unimplemented(c, Opcode::vpkd3d128, i.code);
}
XF_OP(vupkd3d128) {
    if (!vfp::unpack_d3d(c.v[VX128_3_VD128], c.v[VX128_3_VB128], VX128_3_IMM >> 2) && c.system &&
        c.system->unimplemented)
        c.system->unimplemented(c, Opcode::vupkd3d128, i.code);
}

} // namespace xf::ppc::ops
