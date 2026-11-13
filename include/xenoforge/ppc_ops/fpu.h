// Floating point (ppc_ops.h). Translated from Xenia's ppc_emit_fpu.cc and
// what its backend does with it (BSD; see LICENSE-xenia).
//
// Arithmetic runs on the host's doubles in the host's default environment
// (round to nearest, no flush to zero; nothing may leave it changed). An
// instruction leaves that fast path for fp_arith in its Rc form, when
// FPSCR[RN] is not round to nearest, when the result is not ordinary (a NaN,
// or a denormal in its format, which FPSCR[NI] would flush), and in the
// fused single forms when the double result sits on a tie between two
// singles and FRA or FRC is not a single (fp_single_untie). fp_arith does
// what the console does: the rounding mode (the host's control register
// around the operation, lib::FpEnv), non-IEEE mode flushing denormal
// results to zero (Xenia's FTZ), NaN operands looked for in A, B, C order
// and quieted, the positive default NaN (the host's is negative), one
// rounding to single (round to odd in double first), and in the Rc forms the
// FPSCR's exception and status bits (FX, OX, UX, ZX, XX, the VX bits, FR,
// FI, FPRF) with CR1 from them, as the console-generated tests expect.
//
// Without Rc, arithmetic leaves the FPSCR alone: the console sets XX and
// FPRF on nearly every instruction, nothing reads them, and keeping them
// would cost every instruction. (Xenia clears FEX and VX on every one and
// sets CR1 to 0.) Compares set FPCC, and VXSNAN/VXVC on NaNs; the FPSCR
// instructions keep FEX and VX the summaries the architecture makes them.
//
// Not modelled: enabled exceptions (VE..XE: traps, untouched targets), the
// estimate of fres (the console's is a table, about 8 bits; this is the
// single-rounded reciprocal, as Xenia: its tests hold too few of the
// console's values to rebuild it). The console's single forms with a
// double-denormal operand are (Xenia skips those tests): fadds, fsubs, fmuls
// and the fused forms give the default NaN and raise nothing unless an
// operand is infinite or a NaN, fdivs and fsqrts give the double result; the
// fast path checks the single forms' operands for them.
// The fast path of fadds, fsubs, fmuls, fdivs, fsqrts and fres rounds twice
// (to double, then to single), as Xenia: exact when the operands are singles,
// as they are from lfs and the single forms; the fused forms check for ties.

#pragma once

#include "xenoforge/ppc_ops.h"

#if defined(__clang__) || defined(__GNUC__)
#define XF_FP_SLOW __attribute__((noinline, cold))
// a slow path that is a function of its arguments alone: no memory read or
// written, so the code around its call keeps what it holds across it
#define XF_FP_PURE __attribute__((const))
#else
#define XF_FP_SLOW __declspec(noinline)
#define XF_FP_PURE
#endif

namespace xf::ppc::ops {

// ---- the FPSCR (bit 0 of the architecture is the most significant) ----

constexpr uint32_t FPSCR_FX = 1u << 31, FPSCR_FEX = 1u << 30, FPSCR_VX = 1u << 29, FPSCR_OX = 1u << 28,
                   FPSCR_UX = 1u << 27, FPSCR_ZX = 1u << 26, FPSCR_XX = 1u << 25, FPSCR_VXSNAN = 1u << 24,
                   FPSCR_VXISI = 1u << 23, FPSCR_VXIDI = 1u << 22, FPSCR_VXZDZ = 1u << 21, FPSCR_VXIMZ = 1u << 20,
                   FPSCR_VXVC = 1u << 19, FPSCR_FR = 1u << 18, FPSCR_FI = 1u << 17, FPSCR_FPRF = 0x1Fu << 12,
                   FPSCR_FPCC = 0xFu << 12, FPSCR_VXSOFT = 1u << 10, FPSCR_VXSQRT = 1u << 9, FPSCR_VXCVI = 1u << 8,
                   FPSCR_VE = 1u << 7, FPSCR_NI = 1u << 2, FPSCR_RN = 3u;
constexpr uint32_t FPSCR_VX_ALL = FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | FPSCR_VXVC |
                                  FPSCR_VXSOFT | FPSCR_VXSQRT | FPSCR_VXCVI;
// the sticky exception bits (FX apart)
constexpr uint32_t FPSCR_EXCEPTIONS = FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ALL;
// the PPC's default QNaN
constexpr uint64_t FP_DEFAULT_NAN = 0x7FF8000000000000;

XF_INLINE uint64_t fp_bits(double v) { return lib::bit_cast<uint64_t>(v); }
XF_INLINE double fp_double(uint64_t v) { return lib::bit_cast<double>(v); }
XF_INLINE bool fp_is_snan(uint64_t v) {
    return (v & 0x7FF8000000000000) == 0x7FF0000000000000 && (v & 0x0007FFFFFFFFFFFF);
}
// a denormal double (no single is one: lfs and the single forms never make one)
XF_INLINE bool fp_denormal(double v) { return lib::fabs(v) < 0x1p-1022 && v != 0; }

// FEX and VX are summaries: VX the OR of the invalid-operation bits, FEX of
// the exceptions whose enable is set (VX..XX against VE..XE)
XF_INLINE uint32_t fpscr_summarize(uint32_t v) {
    v &= ~(FPSCR_FEX | FPSCR_VX);
    if (v & FPSCR_VX_ALL) v |= FPSCR_VX;
    if ((v >> 22) & v & 0xF8) v |= FPSCR_FEX;
    return v;
}

// An instruction's exceptions (sticky; FX when one goes from 0 to 1) and
// status bits into an FPSCR; into the context's, then CR1 from it
XF_INLINE uint32_t fp_recorded(uint32_t fpscr, uint32_t exceptions, uint32_t status, uint32_t status_mask) {
    uint32_t v = fpscr | exceptions;
    if (exceptions & ~fpscr) v |= FPSCR_FX;
    return fpscr_summarize((v & ~status_mask) | status);
}
inline void fp_record(Context &c, uint32_t exceptions, uint32_t status, uint32_t status_mask) {
    c.fpscr = fp_recorded(c.fpscr, exceptions, status, status_mask);
    fpscr_to_cr1(c);
}

// FPRF (C, <, >, =, ?) for a result, denormal by its format
inline uint32_t fp_class(double r, bool single) {
    bool negative = lib::signbit(r);
    if (r != r) return 0x11u << 12;
    if (lib::isinf(r)) return (negative ? 0x09u : 0x05u) << 12;
    if (r == 0) return (negative ? 0x12u : 0x02u) << 12;
    if (lib::fabs(r) < (single ? 0x1p-126 : 0x1p-1022)) return (negative ? 0x18u : 0x14u) << 12;
    return (negative ? 0x08u : 0x04u) << 12;
}

// ---- arithmetic ----

enum : uint32_t {
    FP_ADD,
    FP_SUB,
    FP_MUL,
    FP_DIV,
    FP_SQRT,
    FP_MADD,
    FP_MSUB,
    FP_NMADD,
    FP_NMSUB,
    FP_RSP,
    FP_RES,
    FP_CFID,
    FP_SINGLE = 0x100,
};

// The operation in the host's current rounding mode (the negated forms are
// negated after rounding, by the caller). Strict floating point, with the
// builtins called here rather than through lib:: (whose bodies are not
// strict: inlined, their operations could move across lib::FpEnv's writes);
// a game library without hardware FMA calls the C library's through the
// runtime's table, a call nothing moves across either.
inline double fp_compute(uint32_t kind, double a, double b, double c) {
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif
#if XF_HARDWARE_FMA || !defined(XF_GAME_LIBRARY)
#define XF_FP_FMA __builtin_fma
#else
#define XF_FP_FMA lib::fma
#endif
    switch (kind) {
    case FP_ADD: return a + b;
    case FP_SUB: return a - b;
    case FP_MUL: return a * c;
    case FP_DIV: return a / b;
    case FP_SQRT: return __builtin_sqrt(b);
    case FP_MADD:
    case FP_NMADD: return XF_FP_FMA(a, c, b);
    case FP_MSUB:
    case FP_NMSUB: return XF_FP_FMA(a, c, -b);
#undef XF_FP_FMA
    case FP_RSP: return b;
    case FP_RES: return 1.0 / b;
    default: return double(lib::bit_cast<int64_t>(b)); // FP_CFID
    }
}

// An arithmetic instruction past the fast path (top of the file). a, b, c
// are FRA, FRB, FRC; fpscr the FPSCR it runs under (its rounding mode and
// NI), and with rc the FPSCR it leaves (its exceptions and status recorded)
// in `recorded`. It reads and writes nothing else (the host's rounding mode
// is put back to nearest; the host's exception flags are cleared before
// each use), so without Rc it is a function of its arguments: fp_slow calls
// it so (fp_arith_value), and the generated code around an instruction that
// leaves the fast path keeps the registers it holds in the context's place
// across it (a call that may write the context, as one taking it would,
// makes the optimizer reload them, and look for what it stored through
// every one of them: most of the compile time of a long run of floating
// point code).
inline double fp_arith_core(uint32_t fpscr, uint32_t op, bool rc, double a, double b, double cc, uint32_t &recorded) {
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif
    const uint32_t kind = op & 0xFF;
    const bool fused = kind >= FP_MADD && kind <= FP_NMSUB;
    bool single = op & FP_SINGLE;
    const bool subtract = kind == FP_MSUB || kind == FP_NMSUB;
    // NaN operands, in the order the PPC takes them
    double operands[3];
    int count = 0;
    if (kind == FP_ADD || kind == FP_SUB || kind == FP_DIV)
        operands[0] = a, operands[1] = b, count = 2;
    else if (kind == FP_MUL)
        operands[0] = a, operands[1] = cc, count = 2;
    else if (fused)
        operands[0] = a, operands[1] = b, operands[2] = cc, count = 3;
    else if (kind != FP_CFID)
        operands[0] = b, count = 1;
    // infinity x 0 is invalid even when FRB is a NaN (the console flags it)
    const bool imz = (kind == FP_MUL || fused) && ((lib::isinf(a) && cc == 0) || (a == 0 && lib::isinf(cc)));
    uint32_t exceptions = imz ? FPSCR_VXIMZ : 0;
    int nan = -1;
    for (int k = 0; k < count; k++) {
        if (operands[k] != operands[k] && nan < 0) nan = k;
        if (fp_is_snan(fp_bits(operands[k]))) exceptions |= FPSCR_VXSNAN;
    }
    if (nan >= 0) {
        // the NaN quieted (a single keeps the top of its fraction), never negated
        uint64_t v = fp_bits(operands[nan]) | 0x0008000000000000;
        if (single) v &= 0xFFFFFFFFE0000000;
        if (rc)
            recorded = fp_recorded(fpscr, exceptions, fp_class(fp_double(v), single), FPSCR_FR | FPSCR_FI | FPSCR_FPRF);
        return fp_double(v);
    }
    // A single form with a double-denormal operand, as the console's tests
    // show: with no infinite operand, add, subtract, multiply and the fused
    // forms give the default NaN and raise nothing; divide and square root
    // give the double result, not rounded to single (and its exceptions)
    if (single && kind <= FP_NMSUB) {
        bool denormal = false, infinite = false;
        for (int k = 0; k < count; k++) denormal |= fp_denormal(operands[k]), infinite |= lib::isinf(operands[k]);
        if (denormal && (kind == FP_DIV || kind == FP_SQRT)) {
            single = false;
        } else if (denormal && !infinite) {
            if (rc) recorded = fp_recorded(fpscr, 0, 0x11u << 12, FPSCR_FR | FPSCR_FI | FPSCR_FPRF);
            return fp_double(FP_DEFAULT_NAN);
        }
    }
    // invalid operations give the default NaN (infinity x 0 above)
    if (imz) {
        // exceptions holds it
    } else if (kind == FP_ADD || kind == FP_SUB || fused) {
        // infinities of opposite effective signs: the product's (or FRA's) against FRB's
        bool infinite = fused ? lib::isinf(a) || lib::isinf(cc) : lib::isinf(a);
        bool negative = fused ? lib::signbit(a) != lib::signbit(cc) : lib::signbit(a);
        if (infinite && lib::isinf(b) && negative != (lib::signbit(b) != (kind == FP_SUB || subtract)))
            exceptions = FPSCR_VXISI;
    } else if (kind == FP_DIV) {
        if (a == 0 && b == 0)
            exceptions = FPSCR_VXZDZ;
        else if (lib::isinf(a) && lib::isinf(b))
            exceptions = FPSCR_VXIDI;
    } else if (kind == FP_SQRT && b < 0) {
        exceptions = FPSCR_VXSQRT;
    }
    if (exceptions) {
        if (rc) recorded = fp_recorded(fpscr, exceptions, 0x11u << 12, FPSCR_FR | FPSCR_FI | FPSCR_FPRF);
        return fp_double(FP_DEFAULT_NAN);
    }
    // the host's environment (lib::FpEnv: its control register, read once
    // and written when the value changes)
    static constexpr decltype(lib::ROUND_NEAREST) MODES[4] = {lib::ROUND_NEAREST, lib::ROUND_ZERO, lib::ROUND_UP,
                                                              lib::ROUND_DOWN};
    const auto mode = MODES[fpscr & FPSCR_RN];
    lib::FpEnv env;
    double r, rz = 0; // rz: toward zero, for FR
    uint32_t flags = 0;
    if (!single) {
        // without Rc the flags are not needed (the NI flush below adds its own)
        if (rc)
            env.start(mode);
        else
            env.round(mode);
        r = fp_compute(kind, a, b, cc);
        if (rc) flags = env.flags(), env.round(lib::ROUND_ZERO), rz = fp_compute(kind, a, b, cc);
    } else {
        // toward zero and odd if inexact: one rounding to single below
        env.start(lib::ROUND_ZERO);
        double d = fp_compute(kind, a, b, cc);
        flags = env.flags();
        // the guest's mode, and the flags of the rounding to single alone (an
        // exact zero computed again raises none)
        env.start(mode);
        if (flags & lib::EXCEPT_INEXACT)
            d = fp_double(fp_bits(d) | 1);
        else if (d == 0)
            d = fp_compute(kind, a, b, cc); // an exact zero's sign is the mode's
        r = double(float(d));
        flags |= env.flags();
        if (rc) env.round(lib::ROUND_ZERO), rz = double(float(d));
    }
    env.end();
    if (kind == FP_NMADD || kind == FP_NMSUB) r = -lib::unfused(r), rz = -lib::unfused(rz);
    if ((fpscr & FPSCR_NI) && r != 0 && lib::fabs(r) < (single ? 0x1p-126 : 0x1p-1022)) {
        r = rz = lib::copysign(0.0, r);
        flags |= lib::EXCEPT_UNDERFLOW | lib::EXCEPT_INEXACT;
    }
    if (rc) {
        exceptions = (flags & lib::EXCEPT_OVERFLOW ? FPSCR_OX : 0) | (flags & lib::EXCEPT_UNDERFLOW ? FPSCR_UX : 0) |
                     (flags & lib::EXCEPT_DIVBYZERO ? FPSCR_ZX : 0);
        uint32_t status = fp_class(r, single);
        // fres: XX untouched, FR and FI undefined (left 0)
        if (kind != FP_RES && (flags & lib::EXCEPT_INEXACT))
            exceptions |= FPSCR_XX, status |= FPSCR_FI | (r != rz ? FPSCR_FR : 0);
        recorded = fp_recorded(fpscr, exceptions, status, FPSCR_FR | FPSCR_FI | FPSCR_FPRF);
    }
    return r;
}
// without Rc: the result alone (the FPSCR read, nothing written)
inline XF_FP_SLOW XF_FP_PURE double fp_arith_value(uint32_t fpscr, uint32_t op, double a, double b, double cc) {
    uint32_t unused = fpscr;
    return fp_arith_core(fpscr, op, false, a, b, cc, unused);
}
// with Rc: the FPSCR's exceptions and status recorded, CR1 from them
inline XF_FP_SLOW double fp_arith(Context &c, uint32_t op, double a, double b, double cc) {
    uint32_t recorded = c.fpscr;
    const double r = fp_arith_core(c.fpscr, op, true, a, b, cc, recorded);
    c.fpscr = recorded;
    fpscr_to_cr1(c);
    return r;
}
// the slow path of an arithmetic instruction (rc is the instruction's: a
// constant in the generated code)
XF_INLINE double fp_slow(Context &c, uint32_t op, bool rc, double a, double b, double cc) {
    return rc ? fp_arith(c, op, a, b, cc) : fp_arith_value(c.fpscr, op, a, b, cc);
}

// The fast path's condition: no Rc, the guest rounds to nearest as the host
// does, and an ordinary result: zero, normal in its format or infinite (one
// compare when normal; a NaN fails it too)
XF_INLINE bool fp_plain(Context &c, bool rc, uint32_t kind, double r) {
    return !rc && !(c.fpscr & FPSCR_RN) &&
           (__builtin_expect(lib::fabs(r) >= (kind & FP_SINGLE ? 0x1p-126 : 0x1p-1022), 1) || r == 0);
}
// a double exactly halfway between two singles (normal ones): rounding the
// fused result again could round the wrong way
XF_INLINE bool fp_single_tie(double d) { return (uint32_t(fp_bits(d)) & 0x1FFFFFFF) == 0x10000000; }
// A fused single form's double result d = fma(a, c, addend) on such a tie,
// moved off it toward the exact result, so that rounding it to single
// rounds as the exact result does; false when that cannot be told here
// (then the slow path). It can when FRA and FRC are singles, as lfs and the
// single forms leave them: their product is then exact in double (48
// significant bits, the exponent far inside double's range), so d is the
// rounded sum of two doubles, whose error TwoSum gives exactly. An exact tie
// stays (the conversion's ties-to-even is the console's); otherwise d moves
// one double ulp toward the error, off the tie and nowhere near the next one.
// (A game's start sent 9 million fmadds a run to the slow path on ties.)
XF_INLINE bool fp_single_untie(double a, double c, double addend, double &d) {
    if ((fp_bits(a) | fp_bits(c)) & 0x1FFFFFFF) return false;
    const double p = a * c; // exact
    const double t = d - p, e = (p - (d - t)) + (addend - t);
    if (e != 0) d = fp_double(fp_bits(d) + ((e > 0) == (d > 0) ? 1 : uint64_t(-1)));
    return true;
}
// No double-denormal operand in a single form (fp_arith has the console's
// rule for them); a double form asks nothing (kind without FP_SINGLE). The
// smallest magnitude is compared once; only when it is that small (a zero
// operand, mostly) is each looked at.
XF_INLINE bool fp_ordinary_operands(uint32_t kind, double a, double b = 1, double c = 1) {
    if (!(kind & FP_SINGLE)) return true;
    double x = lib::fabs(a), y = lib::fabs(b), z = lib::fabs(c), m = x < y ? x : y;
    m = m < z ? m : z;
    return __builtin_expect(m >= 0x1p-1022, 1) || !(fp_denormal(a) || fp_denormal(b) || fp_denormal(c));
}

#define XF_FP_BINARY(name, kind, expr, rhs)                                                                            \
    XF_OP(name) {                                                                                                      \
        double a = c.f[i.A.FRA], b = c.f[i.A.rhs], r = expr;                                                           \
        if (fp_plain(c, i.A.Rc, kind, r) && fp_ordinary_operands(kind, a, b)) [[likely]]                               \
            c.f[i.A.FRT] = r;                                                                                          \
        else                                                                                                           \
            c.f[i.A.FRT] = fp_slow(c, kind, i.A.Rc, a, b, b);                                                          \
    }
// frD <- (frA) + (frB), and so on (fmul's second operand is frC)
// clang-format off
XF_FP_BINARY(faddx, FP_ADD, a + b, FRB)
XF_FP_BINARY(faddsx, FP_ADD | FP_SINGLE, double(float(a + b)), FRB)
XF_FP_BINARY(fsubx, FP_SUB, a - b, FRB)
XF_FP_BINARY(fsubsx, FP_SUB | FP_SINGLE, double(float(a - b)), FRB)
XF_FP_BINARY(fmulx, FP_MUL, a * b, FRC)
XF_FP_BINARY(fmulsx, FP_MUL | FP_SINGLE, double(float(a * b)), FRC)
XF_FP_BINARY(fdivx, FP_DIV, a / b, FRB)
XF_FP_BINARY(fdivsx, FP_DIV | FP_SINGLE, double(float(a / b)), FRB)
// clang-format on
#undef XF_FP_BINARY

// frD <- +-((frA x frC) +- frB), one rounding
#define XF_FP_FUSED(name, kind, sign, addend)                                                                          \
    XF_OP(name) {                                                                                                      \
        double a = c.f[i.A.FRA], b = c.f[i.A.FRB], cc = c.f[i.A.FRC], d = lib::unfused(lib::fma(a, cc, addend));       \
        bool tie = (kind) & FP_SINGLE && fp_single_tie(d);                                                             \
        if (tie) [[unlikely]]                                                                                          \
            tie = !fp_single_untie(a, cc, addend, d);                                                                  \
        double r = sign((kind) & FP_SINGLE ? double(float(d)) : d);                                                    \
        if (fp_plain(c, i.A.Rc, kind, r) && !tie && fp_ordinary_operands(kind, a, b, cc)) [[likely]]                   \
            c.f[i.A.FRT] = r;                                                                                          \
        else                                                                                                           \
            c.f[i.A.FRT] = fp_slow(c, kind, i.A.Rc, a, b, cc);                                                         \
    }
XF_FP_FUSED(fmaddx, FP_MADD, +, b)
XF_FP_FUSED(fmaddsx, FP_MADD | FP_SINGLE, +, b)
XF_FP_FUSED(fmsubx, FP_MSUB, +, -b)
XF_FP_FUSED(fmsubsx, FP_MSUB | FP_SINGLE, +, -b)
XF_FP_FUSED(fnmaddx, FP_NMADD, -, b)
XF_FP_FUSED(fnmaddsx, FP_NMADD | FP_SINGLE, -, b)
XF_FP_FUSED(fnmsubx, FP_NMSUB, -, -b)
XF_FP_FUSED(fnmsubsx, FP_NMSUB | FP_SINGLE, -, -b)
#undef XF_FP_FUSED

#define XF_FP_UNARY(name, kind, expr, form, rt, rb, rc)                                                                \
    XF_OP(name) {                                                                                                      \
        double b = c.f[i.form.rb], r = expr;                                                                           \
        if (fp_plain(c, i.form.rc, kind, r) &&                                                                         \
            fp_ordinary_operands(((kind) & 0xFF) == FP_SQRT ? (kind) : 0, b)) [[likely]]                               \
            c.f[i.form.rt] = r;                                                                                        \
        else                                                                                                           \
            c.f[i.form.rt] = fp_slow(c, kind, i.form.rc, b, b, b);                                                     \
    }
// frD <- sqrt(frB)
XF_FP_UNARY(fsqrtx, FP_SQRT, lib::sqrt(b), A, FRT, FRB, Rc)
XF_FP_UNARY(fsqrtsx, FP_SQRT | FP_SINGLE, double(float(lib::sqrt(b))), A, FRT, FRB, Rc)
// frD <- Round_single(frB)
XF_FP_UNARY(frspx, FP_RSP | FP_SINGLE, double(float(b)), X, RT, RB, Rc)
// frD <- 1 / frB, single (the console's is an estimate; see the top)
XF_FP_UNARY(fresx, FP_RES | FP_SINGLE, double(float(1.0 / b)), A, FRT, FRB, Rc)
// frD <- signed_int64_to_double(frB)
XF_FP_UNARY(fcfidx, FP_CFID, double(lib::bit_cast<int64_t>(b)), X, RT, RB, Rc)
#undef XF_FP_UNARY

// frsqrte: the console's estimate, a 16-entry table by the exponent's parity
// and the fraction's top 3 bits (Xenia's EmitFrsqrteHelper, which the
// console-generated tests agree with); non-IEEE mode takes denormals as zero.
// Exceptions as the architecture's: ZX, VXSQRT, VXSNAN; FR and FI undefined.
inline void fp_frsqrte(Context &c, uint32_t rt, double in, bool rc) {
    static constexpr uint8_t TABLE[16] = {241, 216, 192, 168, 152, 136, 128, 112, 96, 76, 60, 48, 32, 24, 16, 8};
    const uint64_t v = fp_bits(in), sign = v & 0x8000000000000000;
    uint64_t mantissa = v & 0x000FFFFFFFFFFFFF, result;
    int32_t exponent = int32_t((v >> 52) & 0x7FF);
    uint32_t exceptions = 0;
    if (exponent == 0x7FF) {
        if (mantissa)
            result = v | 0x0008000000000000, exceptions = fp_is_snan(v) ? FPSCR_VXSNAN : 0;
        else if (sign)
            result = FP_DEFAULT_NAN, exceptions = FPSCR_VXSQRT;
        else
            result = 0;
    } else if (exponent == 0 && (mantissa == 0 || (c.fpscr & FPSCR_NI))) {
        result = sign | 0x7FF0000000000000, exceptions = FPSCR_ZX;
    } else if (sign) {
        result = FP_DEFAULT_NAN, exceptions = FPSCR_VXSQRT;
    } else {
        if (exponent == 0) {
            int zeros = lib::countl_zero(mantissa);
            mantissa <<= zeros - 11;
            exponent = 12 - zeros;
        }
        uint32_t index = (((uint32_t(exponent) & 1) << 3) | (uint32_t(mantissa >> 49) & 7)) ^ 8;
        result = uint64_t(1022 - ((exponent - 1023) >> 1)) << 52 | uint64_t(TABLE[index]) << 44;
    }
    c.f[rt] = fp_double(result);
    if (rc) fp_record(c, exceptions, fp_class(fp_double(result), false), FPSCR_FR | FPSCR_FI | FPSCR_FPRF);
}
XF_OP(frsqrtex) { fp_frsqrte(c, i.A.FRT, c.f[i.A.FRB], i.A.Rc); }

// ---- conversions to integer ----

// fctiw[z], fctid[z] past the fast path: NaNs and what does not fit saturate
// (VXCVI), rounding by FPSCR[RN] or toward zero, a word sign-extended (the
// console's high word; Xenia leaves 0x00000000_80000000 for a NaN). FPRF
// undefined, left alone. As fp_arith_core: the FPSCR given, and with rc the
// one it leaves in `recorded`; nothing else read or written.
inline double fp_to_int_core(uint32_t fpscr, double b, bool word, bool toward_zero, bool rc, uint32_t &recorded) {
    const int64_t lo = word ? INT32_MIN : INT64_MIN, hi = word ? INT32_MAX : INT64_MAX;
    int64_t v;
    uint32_t exceptions = 0, status = 0;
    if (b != b) {
        v = lo, exceptions = FPSCR_VXCVI | (fp_is_snan(fp_bits(b)) ? FPSCR_VXSNAN : 0);
    } else if (lib::isinf(b)) {
        v = b < 0 ? lo : hi, exceptions = FPSCR_VXCVI;
    } else {
        double t = lib::trunc(b), fraction = b - t; // both exact
        bool away = false;                          // rounded away from zero
        switch (toward_zero ? 1 : fpscr & FPSCR_RN) {
        case 0: away = lib::fabs(fraction) > 0.5 || (lib::fabs(fraction) == 0.5 && lib::fmod(t, 2.0) != 0); break;
        case 2: away = fraction > 0; break;
        case 3: away = fraction < 0; break;
        }
        if (away) t += b < 0 ? -1.0 : 1.0;
        if (t < double(lo) || t >= -double(lo)) {
            v = t < 0 ? lo : hi, exceptions = FPSCR_VXCVI;
        } else {
            v = int64_t(t);
            if (fraction != 0) exceptions = FPSCR_XX, status = FPSCR_FI | (away ? FPSCR_FR : 0);
        }
    }
    if (rc) recorded = fp_recorded(fpscr, exceptions, status, FPSCR_FR | FPSCR_FI);
    return lib::bit_cast<double>(word ? int64_t(int32_t(v)) : v);
}
inline XF_FP_SLOW XF_FP_PURE double fp_to_int_value(uint32_t fpscr, double b, bool word, bool toward_zero) {
    uint32_t unused = fpscr;
    return fp_to_int_core(fpscr, b, word, toward_zero, false, unused);
}
inline XF_FP_SLOW double fp_to_int_recorded(Context &c, double b, bool word, bool toward_zero) {
    uint32_t recorded = c.fpscr;
    const double r = fp_to_int_core(c.fpscr, b, word, toward_zero, true, recorded);
    c.fpscr = recorded;
    fpscr_to_cr1(c);
    return r;
}
XF_INLINE double fp_to_int(Context &c, double b, bool word, bool toward_zero, bool rc) {
    return rc ? fp_to_int_recorded(c, b, word, toward_zero) : fp_to_int_value(c.fpscr, b, word, toward_zero);
}

XF_OP(fctidx) {
    // frD <- double_to_signed_int64(frB), by FPSCR[RN]
    double b = c.f[i.X.RB];
    if (!i.X.Rc && !(c.fpscr & FPSCR_RN) && lib::fabs(b) < 0x1p63) [[likely]]
        c.f[i.X.RT] = lib::bit_cast<double>(int64_t(lib::llrint(b)));
    else
        c.f[i.X.RT] = fp_to_int(c, b, false, false, i.X.Rc);
}

XF_OP(fctidzx) {
    // frD <- double_to_signed_int64(frB), toward zero
    double b = c.f[i.X.RB];
    if (!i.X.Rc && lib::fabs(b) < 0x1p63) [[likely]]
        c.f[i.X.RT] = lib::bit_cast<double>(int64_t(b));
    else
        c.f[i.X.RT] = fp_to_int(c, b, false, true, i.X.Rc);
}

XF_OP(fctiwx) {
    // frD <- double_to_signed_int32(frB), by FPSCR[RN]
    double b = c.f[i.X.RB];
    if (!i.X.Rc && !(c.fpscr & FPSCR_RN) && lib::fabs(b) < 2147483647.0) [[likely]]
        c.f[i.X.RT] = lib::bit_cast<double>(int64_t(lib::llrint(b)));
    else
        c.f[i.X.RT] = fp_to_int(c, b, true, false, i.X.Rc);
}

XF_OP(fctiwzx) {
    // frD <- double_to_signed_int32(frB), toward zero
    double b = c.f[i.X.RB];
    if (!i.X.Rc && lib::fabs(b) < 2147483648.0) [[likely]]
        c.f[i.X.RT] = lib::bit_cast<double>(int64_t(int32_t(b)));
    else
        c.f[i.X.RT] = fp_to_int(c, b, true, true, i.X.Rc);
}

// ---- select, moves (the bits as they are: no NaN is touched) ----

XF_OP(fselx) {
    // frD <- (frA) >= 0.0 ? (frC) : (frB); a NaN selects frB
    c.f[i.A.FRT] = c.f[i.A.FRA] >= 0.0 ? c.f[i.A.FRC] : c.f[i.A.FRB];
    if (i.A.Rc) fpscr_to_cr1(c);
}

XF_OP(fmrx) {
    c.f[i.X.RT] = c.f[i.X.RB];
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(fabsx) {
    c.f[i.X.RT] = fp_double(fp_bits(c.f[i.X.RB]) & ~0x8000000000000000);
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(fnabsx) {
    c.f[i.X.RT] = fp_double(fp_bits(c.f[i.X.RB]) | 0x8000000000000000);
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(fnegx) {
    c.f[i.X.RT] = fp_double(fp_bits(c.f[i.X.RB]) ^ 0x8000000000000000);
    if (i.X.Rc) fpscr_to_cr1(c);
}

// ---- compares ----

// Unordered: FPCC 0001 (the CR field too, by the caller), VXSNAN for an
// SNaN, and for fcmpo VXVC (for an SNaN only when VE is clear): the FPSCR
// it leaves, nothing else read or written (as fp_arith_value)
inline XF_FP_SLOW XF_FP_PURE uint32_t fp_compare_unordered(uint32_t fpscr, double a, double b, bool ordered) {
    bool snan = fp_is_snan(fp_bits(a)) || fp_is_snan(fp_bits(b));
    uint32_t exceptions = snan ? FPSCR_VXSNAN : 0;
    if (ordered && (!snan || !(fpscr & FPSCR_VE))) exceptions |= FPSCR_VXVC;
    uint32_t v = (fpscr & ~FPSCR_FPCC) | (1u << 12) | exceptions;
    if (exceptions & ~fpscr) v |= FPSCR_FX;
    return fpscr_summarize(v);
}

XF_INLINE void fp_compare(Context &c, uint32_t bf, double a, double b, bool ordered) {
    if (a == a && b == b) [[likely]] {
        // exactly one of lt, gt, eq: the field's four bytes in one store
        const bool lt = a < b;
        const bool gt = a > b;
        if constexpr (lib::little_endian) {
            uint32_t field = lt ? 0x1 : gt ? 0x100 : 0x10000;
            lib::memcpy(&c.cr[4 * bf], &field, 4);
        } else {
            c.cr[4 * bf + 0] = lt, c.cr[4 * bf + 1] = gt, c.cr[4 * bf + 2] = !lt && !gt, c.cr[4 * bf + 3] = 0;
        }
        c.fpscr = (c.fpscr & ~FPSCR_FPCC) | (lt ? 0x8000u : gt ? 0x4000u : 0x2000u);
    } else {
        c.cr[4 * bf + 0] = 0, c.cr[4 * bf + 1] = 0, c.cr[4 * bf + 2] = 0, c.cr[4 * bf + 3] = 1;
        c.fpscr = fp_compare_unordered(c.fpscr, a, b, ordered);
    }
}

XF_OP(fcmpu) { fp_compare(c, i.X.RT >> 2, c.f[i.X.RA], c.f[i.X.RB], false); }
XF_OP(fcmpo) { fp_compare(c, i.X.RT >> 2, c.f[i.X.RA], c.f[i.X.RB], true); }

// ---- the FPSCR ----

XF_OP(mffsx) {
    // frD <- FPSCR, zero-extended (as Xenia)
    c.f[i.X.RT] = fp_double(c.fpscr);
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(mcrfs) {
    // CR[crfD] <- FPSCR field crfS; that field's exception bits cleared (Xenia
    // clears the whole field, RN and the enables too)
    uint32_t bf = i.X.RT >> 2, shift = 4 * (7 - (i.X.RA >> 2)), field = (c.fpscr >> shift) & 0xF;
    for (uint32_t k = 0; k < 4; k++) c.cr[4 * bf + k] = (field >> (3 - k)) & 1;
    c.fpscr = fpscr_summarize(c.fpscr & ~((FPSCR_FX | FPSCR_EXCEPTIONS) & (0xFu << shift)));
}

XF_OP(mtfsb0x) {
    // FPSCR[BT] <- 0 (FEX and VX stay the summaries)
    c.fpscr = fpscr_summarize(c.fpscr & ~(0x80000000u >> i.X.RT));
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(mtfsb1x) {
    // FPSCR[BT] <- 1; an exception bit set this way sets FX too
    uint32_t bit = 0x80000000u >> i.X.RT, v = c.fpscr | bit;
    if (bit & FPSCR_EXCEPTIONS & ~c.fpscr) v |= FPSCR_FX;
    c.fpscr = fpscr_summarize(v);
    if (i.X.Rc) fpscr_to_cr1(c);
}

XF_OP(mtfsfx) {
    // FPSCR fields in FM (FM's top bit is field 0) <- frB's low word; L: all
    // of them. Xenia expands FM's fields in reverse, which only FM = 0xFF hides.
    uint32_t mask = 0;
    for (uint32_t k = 0; k < 8; k++)
        if (i.XFL.FM & (0x80u >> k)) mask |= 0xF0000000u >> (4 * k);
    if (i.XFL.L) mask = 0xFFFFFFFF;
    c.fpscr = fpscr_summarize((c.fpscr & ~mask) | (uint32_t(fp_bits(c.f[i.XFL.RB])) & mask));
    if (i.XFL.Rc) fpscr_to_cr1(c);
}

XF_OP(mtfsfix) {
    // FPSCR[crfD] <- U, the top 4 bits of the RB field (Xenia takes all 5,
    // so U lands one bit too high)
    uint32_t shift = 4 * (7 - (i.X.RT >> 2));
    c.fpscr = fpscr_summarize((c.fpscr & ~(0xFu << shift)) | (uint32_t(i.X.RB >> 1) << shift));
    if (i.X.Rc) fpscr_to_cr1(c);
}

} // namespace xf::ppc::ops
