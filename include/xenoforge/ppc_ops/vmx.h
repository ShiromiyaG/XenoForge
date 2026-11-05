// VMX and VMX128: integer, logic, compares, shifts, permutes, packs and
// splats (ppc_ops.h). Translated from Xenia's ppc_emit_altivec.cc and what its
// backend does with it (BSD; see LICENSE-xenia). The instructions Xenia leaves
// unimplemented (vmsum*, vmul[eo]*, vsum*, vmhaddshs, vmhraddshs, vmladduhm,
// vupkhpx, vupklpx) follow the AltiVec PEM.
//
// VSCR[SAT] is sticky, as the architecture has it: a saturating instruction
// sets c.vscr_sat when an element saturated and never clears it (Xenia's
// emitters store a per-instruction flag that its backends always compute as
// 0; its tests do not read it).

#pragma once

#include "xenoforge/ppc_ops.h"

#include <type_traits>

namespace xf::ppc::ops {

// Lane helpers. Element-wise ops work on the host lanes as they are: element n
// of every operand sits in the same host lane. Only the ops that move elements
// between positions need the guest order (byte n at u8[n ^ 3], halfword n at
// u16[n ^ 1], word n at u32[n]).
namespace vmxi {

// the type a lane's arithmetic is done in before it saturates
template <typename T> using Wide = std::conditional_t<(sizeof(T) < 4), int32_t, int64_t>;

template <typename T, typename W> XF_INLINE T clamp(W w, uint32_t &sat) {
    constexpr W lo = W(std::numeric_limits<T>::min()), hi = W(std::numeric_limits<T>::max());
    W v = w < lo ? lo : w > hi ? hi : w;
    sat |= v != w;
    return T(v);
}

// r[k] = f(a[k], b[k]) over lanes of T, truncated to T
template <typename T, typename F> XF_INLINE Vec128 map(const Vec128 &a, const Vec128 &b, F f) {
    constexpr int N = 16 / sizeof(T);
    T x[N], y[N], r[N];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int k = 0; k < N; k++) r[k] = T(f(x[k], y[k]));
    Vec128 v;
    std::memcpy(&v, r, 16);
    return v;
}

// a[k] + b[k] (Sub: a[k] - b[k]) saturated to T; VSCR[SAT] if any lane did,
// which is where the saturated result differs from the wrapped one. Three
// separate loops: the compiler makes the first a saturating add (paddsb...)
// and the others narrow lanes, where one loop would widen every lane.
template <typename T, bool Sub> XF_INLINE Vec128 add_sat(Context &c, const Vec128 &a, const Vec128 &b) {
    using U = std::make_unsigned_t<T>;
    constexpr int N = 16 / sizeof(T);
    constexpr Wide<T> lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    T x[N], y[N], r[N], w[N];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int k = 0; k < N; k++) {
        Wide<T> s = Sub ? Wide<T>(x[k]) - y[k] : Wide<T>(x[k]) + y[k];
        r[k] = T(s < lo ? lo : s > hi ? hi : s);
    }
    for (int k = 0; k < N; k++) w[k] = T(Sub ? U(U(x[k]) - U(y[k])) : U(U(x[k]) + U(y[k])));
    U any = 0;
    for (int k = 0; k < N; k++) any |= U(r[k] ^ w[k]);
    c.vscr_sat |= any != 0;
    Vec128 v;
    std::memcpy(&v, r, 16);
    return v;
}

// all ones where f(a[k], b[k]); CR6 for the dot forms
template <typename T, typename F> XF_INLINE void compare(Context &c, uint32_t d, uint32_t a, uint32_t b, bool rc, F f) {
    Vec128 v = map<T>(c.v[a], c.v[b], [f](T x, T y) { return f(x, y) ? T(~T(0)) : T(0); });
    c.v[d] = v;
    if (rc) update_cr6(c, v);
}

inline constexpr auto add = [](auto x, auto y) { return x + y; };
inline constexpr auto sub = [](auto x, auto y) { return x - y; };
inline constexpr auto max = [](auto x, auto y) { return x > y ? x : y; };
inline constexpr auto min = [](auto x, auto y) { return x < y ? x : y; };
inline constexpr auto eq = [](auto x, auto y) { return x == y; };
inline constexpr auto gt = [](auto x, auto y) { return x > y; };
// (a + b + 1) >> 1 without overflow
inline constexpr auto avg = [](auto x, auto y) { return (Wide<decltype(x)>(x) + y + 1) >> 1; };
// shifts by the low log2(bits) bits of each element of b
inline constexpr auto shl = [](auto x, auto y) { return x << (y & (sizeof(x) * 8 - 1)); };
inline constexpr auto shr = [](auto x, auto y) { return x >> (y & (sizeof(x) * 8 - 1)); };
inline constexpr auto rotl = [](auto x, auto y) {
    constexpr unsigned bits = sizeof(x) * 8;
    unsigned s = unsigned(y) & (bits - 1);
    return (x << s) | (x >> ((bits - s) & (bits - 1)));
};
inline constexpr auto and_ = [](uint64_t x, uint64_t y) { return x & y; };
inline constexpr auto andc = [](uint64_t x, uint64_t y) { return x & ~y; };
inline constexpr auto nor = [](uint64_t x, uint64_t y) { return ~(x | y); };
inline constexpr auto or_ = [](uint64_t x, uint64_t y) { return x | y; };
inline constexpr auto xor_ = [](uint64_t x, uint64_t y) { return x ^ y; };

// LOAD_VECTOR_SHL/SHR: guest byte n is base + n
XF_INLINE Vec128 byte_ramp(uint32_t base) {
    Vec128 v;
    for (uint32_t k = 0; k < 4; k++) v.u32[k] = (base + 4 * k) * 0x01010101u + 0x00010203u;
    return v;
}
// the low 4 bits of (RA|0) + (RB)
XF_INLINE uint32_t ea_low4(Context &c, uint32_t ra, uint32_t rb) {
    return uint32_t((ra ? c.r[ra] : 0) + c.r[rb]) & 0xF;
}

// the register as one 128-bit number, word 0 most significant
struct U128 {
    uint64_t hi, lo;
};
XF_INLINE U128 u128(const Vec128 &v) {
    return {uint64_t(v.u32[0]) << 32 | v.u32[1], uint64_t(v.u32[2]) << 32 | v.u32[3]};
}
XF_INLINE Vec128 vec(U128 x) {
    Vec128 v;
    v.u32[0] = uint32_t(x.hi >> 32), v.u32[1] = uint32_t(x.hi), v.u32[2] = uint32_t(x.lo >> 32),
    v.u32[3] = uint32_t(x.lo);
    return v;
}
// shifts by 0 to 127 bits
XF_INLINE U128 shl128(U128 x, uint32_t s) {
    if (s == 0) return x;
    if (s >= 64) return {x.lo << (s - 64), 0};
    return {x.hi << s | x.lo >> (64 - s), x.lo << s};
}
XF_INLINE U128 shr128(U128 x, uint32_t s) {
    if (s == 0) return x;
    if (s >= 64) return {0, x.hi >> (s - 64)};
    return {x.hi >> s, x.lo >> s | x.hi << (64 - s)};
}

// vsl, vsr: the register shifted by bits. The architecture defines it when the
// low 3 bits of every byte of b agree, and Xenia shifts by those of byte 15;
// the console shifts each byte by its own count, with the bits from its
// neighbour, which is the same where it is defined and matches the console's
// results (Xenia's skip.txt skips the 876 test cases where they differ).
inline Vec128 shl_bits(const Vec128 &a, const Vec128 &b) {
    Vec128 r;
    for (uint32_t n = 0; n < 16; n++) {
        uint32_t pair = uint32_t(a.u8[n ^ 3]) << 8 | (n < 15 ? a.u8[(n + 1) ^ 3] : 0);
        r.u8[n ^ 3] = uint8_t((pair << (b.u8[n ^ 3] & 7)) >> 8);
    }
    return r;
}
inline Vec128 shr_bits(const Vec128 &a, const Vec128 &b) {
    Vec128 r;
    for (uint32_t n = 0; n < 16; n++) {
        uint32_t pair = (n > 0 ? uint32_t(a.u8[(n - 1) ^ 3]) << 8 : 0) | a.u8[n ^ 3];
        r.u8[n ^ 3] = uint8_t(pair >> (b.u8[n ^ 3] & 7));
    }
    return r;
}

// PERMUTE by bytes: guest byte n is byte (control n & 31) of a || b. Byte k of
// a || b is at tab[k ^ 3] (bit 4 picks b, and ^ 3 keeps it).
inline Vec128 perm(const Vec128 &a, const Vec128 &b, const Vec128 &control) {
    uint8_t tab[32];
    std::memcpy(tab, &a, 16);
    std::memcpy(tab + 16, &b, 16);
    Vec128 r;
    for (uint32_t j = 0; j < 16; j++) r.u8[j] = tab[(control.u8[j] & 0x1F) ^ 3];
    return r;
}
// (a || b) << (sh * 8), the high 16 bytes
inline Vec128 sldoi(const Vec128 &a, const Vec128 &b, uint32_t sh) {
    uint8_t tab[32];
    std::memcpy(tab, &a, 16);
    std::memcpy(tab + 16, &b, 16);
    Vec128 r;
    for (uint32_t n = 0; n < 16; n++) r.u8[n ^ 3] = tab[(n + sh) ^ 3];
    return r;
}

// merges: elements off..off+count/2-1 of a and b interleaved
XF_INLINE Vec128 merge8(const Vec128 &a, const Vec128 &b, uint32_t off) {
    Vec128 r;
    for (uint32_t k = 0; k < 8; k++)
        r.u8[(2 * k) ^ 3] = a.u8[(k + off) ^ 3], r.u8[(2 * k + 1) ^ 3] = b.u8[(k + off) ^ 3];
    return r;
}
XF_INLINE Vec128 merge16(const Vec128 &a, const Vec128 &b, uint32_t off) {
    Vec128 r;
    for (uint32_t k = 0; k < 4; k++)
        r.u16[(2 * k) ^ 1] = a.u16[(k + off) ^ 1], r.u16[(2 * k + 1) ^ 1] = b.u16[(k + off) ^ 1];
    return r;
}
XF_INLINE Vec128 merge32(const Vec128 &a, const Vec128 &b, uint32_t off) {
    Vec128 r;
    r.u32[0] = a.u32[off], r.u32[1] = b.u32[off], r.u32[2] = a.u32[off + 1], r.u32[3] = b.u32[off + 1];
    return r;
}

XF_INLINE Vec128 splat32(uint32_t x) {
    Vec128 v;
    for (uint32_t k = 0; k < 4; k++) v.u32[k] = x;
    return v;
}
// the 5-bit SIMM sign-extended
XF_INLINE int32_t simm5(uint32_t v) { return int32_t(v << 27) >> 27; }

// PACK: the elements of a then b, each to Out (saturated or truncated). x[k]
// holds a's then b's host lanes, so guest element n is x[n ^ 1] for
// halfwords and x[n] for words; the narrowing and the saturation test are
// separate loops over the host lanes (Out's range fits In, so the clamp is
// done in In), then a fixed shuffle.
template <typename In, typename Out, bool Sat, int N, typename F>
XF_INLINE void narrow(Context &c, const Vec128 &a, const Vec128 &b, F store) {
    constexpr In lo = In(std::numeric_limits<Out>::min()), hi = In(std::numeric_limits<Out>::max());
    In x[N];
    Out y[N];
    std::memcpy(x, &a, 16);
    std::memcpy(x + N / 2, &b, 16);
    for (int k = 0; k < N; k++) y[k] = Out(Sat ? (x[k] < lo ? lo : x[k] > hi ? hi : x[k]) : x[k]);
    if constexpr (Sat) {
        In any = 0;
        for (int k = 0; k < N; k++) any |= In(In(y[k]) ^ x[k]);
        c.vscr_sat |= any != 0;
    }
    store(y);
}
template <typename In, typename Out, bool Sat> XF_INLINE Vec128 pack16(Context &c, const Vec128 &a, const Vec128 &b) {
    Vec128 r;
    narrow<In, Out, Sat, 16>(c, a, b, [&r](const Out *y) {
        for (uint32_t n = 0; n < 16; n++) r.u8[n ^ 3] = uint8_t(y[n ^ 1]);
    });
    return r;
}
template <typename In, typename Out, bool Sat> XF_INLINE Vec128 pack32(Context &c, const Vec128 &a, const Vec128 &b) {
    Vec128 r;
    narrow<In, Out, Sat, 8>(c, a, b, [&r](const Out *y) {
        for (uint32_t n = 0; n < 8; n++) r.u16[n ^ 1] = uint16_t(y[n]);
    });
    return r;
}
// vpkpx: 8:8:8:8 to 1:5:5:5
XF_INLINE Vec128 pack_pixel(const Vec128 &a, const Vec128 &b) {
    uint32_t x[8];
    uint16_t y[8];
    std::memcpy(x, &a, 16);
    std::memcpy(x + 4, &b, 16);
    for (int k = 0; k < 8; k++) y[k] = uint16_t(((x[k] >> 9) & 0xFC00) | ((x[k] >> 6) & 0x3E0) | ((x[k] >> 3) & 0x1F));
    Vec128 r;
    for (uint32_t n = 0; n < 8; n++) r.u16[n ^ 1] = y[n];
    return r;
}
// UNPACK, sign-extended: elements off.. of b
XF_INLINE Vec128 unpack8(const Vec128 &b, uint32_t off) {
    Vec128 r;
    for (uint32_t n = 0; n < 8; n++) r.i16[n ^ 1] = int8_t(b.u8[(n + off) ^ 3]);
    return r;
}
XF_INLINE Vec128 unpack16(const Vec128 &b, uint32_t off) {
    Vec128 r;
    for (uint32_t n = 0; n < 4; n++) r.i32[n] = int16_t(b.u16[(n + off) ^ 1]);
    return r;
}
// vupkhpx/vupklpx: 1:5:5:5 to 8:8:8:8, the 1 sign-extended
inline Vec128 unpack_pixel(const Vec128 &b, uint32_t off) {
    Vec128 r;
    for (uint32_t n = 0; n < 4; n++) {
        uint32_t h = b.u16[(n + off) ^ 1];
        r.u32[n] = (h & 0x8000 ? 0xFF000000u : 0) | ((h >> 10) & 0x1F) << 16 | ((h >> 5) & 0x1F) << 8 | (h & 0x1F);
    }
    return r;
}

// vmule*/vmulo*: the even (odd = 0) or odd (odd = 1) elements of a and b
// multiplied into elements twice as wide
template <typename T> inline Vec128 mul_even_odd(const Vec128 &a, const Vec128 &b, uint32_t odd) {
    Vec128 r;
    if constexpr (sizeof(T) == 1) {
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t n = (2 * k + odd) ^ 3;
            r.u16[k ^ 1] = uint16_t(int32_t(T(a.u8[n])) * int32_t(T(b.u8[n])));
        }
    } else {
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t n = (2 * k + odd) ^ 1;
            r.u32[k] = uint32_t(int64_t(T(a.u16[n])) * int64_t(T(b.u16[n])));
        }
    }
    return r;
}
// for each word, the sum of the products of the elements of a and b in it
// (any order inside the word gives the same sum)
template <typename TA, typename TB> XF_INLINE void products(const Vec128 &a, const Vec128 &b, int64_t sum[4]) {
    constexpr int N = 4 / sizeof(TA);
    TA x[16 / sizeof(TA)];
    TB y[16 / sizeof(TB)];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int w = 0; w < 4; w++) {
        int64_t s = 0;
        for (int k = 0; k < N; k++) s += int64_t(x[w * N + k]) * int64_t(y[w * N + k]);
        sum[w] = s;
    }
}
// vmsum*: the products plus the words of c, modulo or saturated to T
template <typename TA, typename TB, typename T, bool Sat>
inline Vec128 msum(Context &ctx, const Vec128 &a, const Vec128 &b, const Vec128 &c) {
    int64_t sum[4];
    products<TA, TB>(a, b, sum);
    Vec128 r;
    uint32_t sat = 0;
    for (int w = 0; w < 4; w++) {
        int64_t t = sum[w] + int64_t(T(c.u32[w]));
        r.u32[w] = uint32_t(Sat ? clamp<T>(t, sat) : T(t));
    }
    if (Sat) ctx.vscr_sat |= uint8_t(sat);
    return r;
}
// vsum4*: for each word, the elements of a in it plus the word of b, saturated
template <typename TA, typename T> inline Vec128 sum4(Context &c, const Vec128 &a, const Vec128 &b) {
    constexpr int N = 4 / sizeof(TA);
    TA x[16 / sizeof(TA)];
    std::memcpy(x, &a, 16);
    Vec128 r;
    uint32_t sat = 0;
    for (int w = 0; w < 4; w++) {
        int64_t t = int64_t(T(b.u32[w]));
        for (int k = 0; k < N; k++) t += x[w * N + k];
        r.u32[w] = uint32_t(clamp<T>(t, sat));
    }
    c.vscr_sat |= uint8_t(sat);
    return r;
}
// vsum2sws (2 words to each of words 1 and 3) and vsumsws (4 to word 3)
template <int Words> inline Vec128 sum_across(Context &c, const Vec128 &a, const Vec128 &b) {
    Vec128 r{};
    uint32_t sat = 0;
    for (int g = 0; g < 4; g += Words) {
        int64_t t = b.i32[g + Words - 1];
        for (int k = 0; k < Words; k++) t += a.i32[g + k];
        r.i32[g + Words - 1] = clamp<int32_t>(t, sat);
    }
    c.vscr_sat |= uint8_t(sat);
    return r;
}
// vmhaddshs/vmhraddshs: ((a * b + round) >> 15) + c, saturated
inline Vec128 mhadd(Context &ctx, const Vec128 &a, const Vec128 &b, const Vec128 &c, int32_t round) {
    Vec128 r;
    uint32_t sat = 0;
    for (int k = 0; k < 8; k++)
        r.i16[k] = clamp<int16_t>(((int32_t(a.i16[k]) * b.i16[k] + round) >> 15) + c.i16[k], sat);
    ctx.vscr_sat |= uint8_t(sat);
    return r;
}

} // namespace vmxi

// ---- lvsl, lvsr: the permute controls for unaligned data ----

XF_OP(lvsl) { c.v[i.X.RT] = vmxi::byte_ramp(vmxi::ea_low4(c, i.X.RA, i.X.RB)); }
XF_OP(lvsl128) { c.v[VX128_1_VD128] = vmxi::byte_ramp(vmxi::ea_low4(c, i.VX128_1.RA, i.VX128_1.RB)); }
XF_OP(lvsr) { c.v[i.X.RT] = vmxi::byte_ramp(16 - vmxi::ea_low4(c, i.X.RA, i.X.RB)); }
XF_OP(lvsr128) { c.v[VX128_1_VD128] = vmxi::byte_ramp(16 - vmxi::ea_low4(c, i.VX128_1.RA, i.VX128_1.RB)); }

// ---- add, subtract, average, max, min ----

XF_OP(vaddcuw) {
    // the carry out of each word
    c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], [](uint32_t a, uint32_t b) { return a + b < a; });
}
XF_OP(vaddsbs) { c.v[i.VX.VD] = vmxi::add_sat<int8_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vaddshs) { c.v[i.VX.VD] = vmxi::add_sat<int16_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vaddsws) { c.v[i.VX.VD] = vmxi::add_sat<int32_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vaddubm) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::add); }
XF_OP(vaddubs) { c.v[i.VX.VD] = vmxi::add_sat<uint8_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vadduhm) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::add); }
XF_OP(vadduhs) { c.v[i.VX.VD] = vmxi::add_sat<uint16_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vadduwm) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::add); }
XF_OP(vadduws) { c.v[i.VX.VD] = vmxi::add_sat<uint32_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }

XF_OP(vsubcuw) {
    // the carry (no borrow) of each word
    c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], [](uint32_t a, uint32_t b) { return a >= b; });
}
XF_OP(vsubsbs) { c.v[i.VX.VD] = vmxi::add_sat<int8_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsubshs) { c.v[i.VX.VD] = vmxi::add_sat<int16_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsubsws) { c.v[i.VX.VD] = vmxi::add_sat<int32_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsububm) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::sub); }
XF_OP(vsububs) { c.v[i.VX.VD] = vmxi::add_sat<uint8_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsubuhm) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::sub); }
XF_OP(vsubuhs) { c.v[i.VX.VD] = vmxi::add_sat<uint16_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsubuwm) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::sub); }
XF_OP(vsubuws) { c.v[i.VX.VD] = vmxi::add_sat<uint32_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }

XF_OP(vavgsb) { c.v[i.VX.VD] = vmxi::map<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }
XF_OP(vavgsh) { c.v[i.VX.VD] = vmxi::map<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }
XF_OP(vavgsw) { c.v[i.VX.VD] = vmxi::map<int32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }
XF_OP(vavgub) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }
XF_OP(vavguh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }
XF_OP(vavguw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::avg); }

XF_OP(vmaxsb) { c.v[i.VX.VD] = vmxi::map<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vmaxsh) { c.v[i.VX.VD] = vmxi::map<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vmaxsw) { c.v[i.VX.VD] = vmxi::map<int32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vmaxub) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vmaxuh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vmaxuw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::max); }
XF_OP(vminsb) { c.v[i.VX.VD] = vmxi::map<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }
XF_OP(vminsh) { c.v[i.VX.VD] = vmxi::map<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }
XF_OP(vminsw) { c.v[i.VX.VD] = vmxi::map<int32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }
XF_OP(vminub) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }
XF_OP(vminuh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }
XF_OP(vminuw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::min); }

// ---- logic ----

XF_OP(vand) { c.v[i.VX.VD] = vmxi::map<uint64_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::and_); }
XF_OP(vand128) { c.v[VX128_VD128] = vmxi::map<uint64_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::and_); }
XF_OP(vandc) { c.v[i.VX.VD] = vmxi::map<uint64_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::andc); }
XF_OP(vandc128) { c.v[VX128_VD128] = vmxi::map<uint64_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::andc); }
XF_OP(vnor) { c.v[i.VX.VD] = vmxi::map<uint64_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::nor); }
XF_OP(vnor128) { c.v[VX128_VD128] = vmxi::map<uint64_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::nor); }
XF_OP(vor) { c.v[i.VX.VD] = vmxi::map<uint64_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::or_); }
XF_OP(vor128) { c.v[VX128_VD128] = vmxi::map<uint64_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::or_); }
XF_OP(vxor) { c.v[i.VX.VD] = vmxi::map<uint64_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::xor_); }
XF_OP(vxor128) { c.v[VX128_VD128] = vmxi::map<uint64_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::xor_); }

XF_OP(vsel) {
    // each bit from b where c has it set, else from a
    const Vec128 &m = c.v[i.VXA.VC];
    c.v[i.VXA.VD] = vmxi::map<uint64_t>(vmxi::map<uint64_t>(c.v[i.VXA.VA], m, vmxi::andc),
                                        vmxi::map<uint64_t>(c.v[i.VXA.VB], m, vmxi::and_), vmxi::or_);
}
XF_OP(vsel128) {
    // the mask is VD
    const Vec128 &m = c.v[VX128_VD128];
    c.v[VX128_VD128] = vmxi::map<uint64_t>(vmxi::map<uint64_t>(c.v[VX128_VA128], m, vmxi::andc),
                                           vmxi::map<uint64_t>(c.v[VX128_VB128], m, vmxi::and_), vmxi::or_);
}

// ---- compares ----

XF_OP(vcmpequb) { vmxi::compare<uint8_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::eq); }
XF_OP(vcmpequh) { vmxi::compare<uint16_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::eq); }
XF_OP(vcmpequw) { vmxi::compare<uint32_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::eq); }
XF_OP(vcmpequw128) { vmxi::compare<uint32_t>(c, VX128_R_VD128, VX128_R_VA128, VX128_R_VB128, i.VX128_R.Rc, vmxi::eq); }
XF_OP(vcmpgtsb) { vmxi::compare<int8_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }
XF_OP(vcmpgtsh) { vmxi::compare<int16_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }
XF_OP(vcmpgtsw) { vmxi::compare<int32_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }
XF_OP(vcmpgtub) { vmxi::compare<uint8_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }
XF_OP(vcmpgtuh) { vmxi::compare<uint16_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }
XF_OP(vcmpgtuw) { vmxi::compare<uint32_t>(c, i.VXR.VD, i.VXR.VA, i.VXR.VB, i.VXR.Rc, vmxi::gt); }

// ---- multiplies and sums (the AltiVec PEM; Xenia has none of them) ----

XF_OP(vmhaddshs) { c.v[i.VXA.VD] = vmxi::mhadd(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC], 0); }
XF_OP(vmhraddshs) { c.v[i.VXA.VD] = vmxi::mhadd(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC], 0x4000); }
XF_OP(vmladduhm) {
    // (a * b + c) mod 2^16
    Vec128 ab = vmxi::map<uint16_t>(c.v[i.VXA.VA], c.v[i.VXA.VB], [](uint32_t a, uint32_t b) { return a * b; });
    c.v[i.VXA.VD] = vmxi::map<uint16_t>(ab, c.v[i.VXA.VC], vmxi::add);
}
XF_OP(vmsummbm) {
    c.v[i.VXA.VD] = vmxi::msum<int8_t, uint8_t, int32_t, false>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmsumshm) {
    c.v[i.VXA.VD] = vmxi::msum<int16_t, int16_t, int32_t, false>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmsumshs) {
    c.v[i.VXA.VD] = vmxi::msum<int16_t, int16_t, int32_t, true>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmsumubm) {
    c.v[i.VXA.VD] = vmxi::msum<uint8_t, uint8_t, uint32_t, false>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmsumuhm) {
    c.v[i.VXA.VD] = vmxi::msum<uint16_t, uint16_t, uint32_t, false>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmsumuhs) {
    c.v[i.VXA.VD] = vmxi::msum<uint16_t, uint16_t, uint32_t, true>(c, c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]);
}
XF_OP(vmulesb) { c.v[i.VX.VD] = vmxi::mul_even_odd<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmulesh) { c.v[i.VX.VD] = vmxi::mul_even_odd<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmuleub) { c.v[i.VX.VD] = vmxi::mul_even_odd<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmuleuh) { c.v[i.VX.VD] = vmxi::mul_even_odd<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmulosb) { c.v[i.VX.VD] = vmxi::mul_even_odd<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], 1); }
XF_OP(vmulosh) { c.v[i.VX.VD] = vmxi::mul_even_odd<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], 1); }
XF_OP(vmuloub) { c.v[i.VX.VD] = vmxi::mul_even_odd<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], 1); }
XF_OP(vmulouh) { c.v[i.VX.VD] = vmxi::mul_even_odd<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], 1); }
XF_OP(vsum4sbs) { c.v[i.VX.VD] = vmxi::sum4<int8_t, int32_t>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsum4shs) { c.v[i.VX.VD] = vmxi::sum4<int16_t, int32_t>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsum4ubs) { c.v[i.VX.VD] = vmxi::sum4<uint8_t, uint32_t>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsum2sws) { c.v[i.VX.VD] = vmxi::sum_across<2>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsumsws) { c.v[i.VX.VD] = vmxi::sum_across<4>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }

// ---- merges, permutes, splats ----

XF_OP(vmrghb) { c.v[i.VX.VD] = vmxi::merge8(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmrghh) { c.v[i.VX.VD] = vmxi::merge16(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmrghw) { c.v[i.VX.VD] = vmxi::merge32(c.v[i.VX.VA], c.v[i.VX.VB], 0); }
XF_OP(vmrghw128) { c.v[VX128_VD128] = vmxi::merge32(c.v[VX128_VA128], c.v[VX128_VB128], 0); }
XF_OP(vmrglb) { c.v[i.VX.VD] = vmxi::merge8(c.v[i.VX.VA], c.v[i.VX.VB], 8); }
XF_OP(vmrglh) { c.v[i.VX.VD] = vmxi::merge16(c.v[i.VX.VA], c.v[i.VX.VB], 4); }
XF_OP(vmrglw) { c.v[i.VX.VD] = vmxi::merge32(c.v[i.VX.VA], c.v[i.VX.VB], 2); }
XF_OP(vmrglw128) { c.v[VX128_VD128] = vmxi::merge32(c.v[VX128_VA128], c.v[VX128_VB128], 2); }

XF_OP(vperm) { c.v[i.VXA.VD] = vmxi::perm(c.v[i.VXA.VA], c.v[i.VXA.VB], c.v[i.VXA.VC]); }
XF_OP(vperm128) { c.v[VX128_2_VD128] = vmxi::perm(c.v[VX128_2_VA128], c.v[VX128_2_VB128], c.v[VX128_2_VC]); }
XF_OP(vpermwi128) {
    // word k of VD is word (PERM >> (6 - 2k)) & 3 of VB
    const Vec128 b = c.v[i.VX128_P.VB128l | (i.VX128_P.VB128h << 5)];
    uint32_t perm = i.VX128_P.PERMl | (i.VX128_P.PERMh << 5);
    Vec128 r;
    for (uint32_t k = 0; k < 4; k++) r.u32[k] = b.u32[(perm >> (6 - 2 * k)) & 3];
    c.v[i.VX128_P.VD128l | (i.VX128_P.VD128h << 5)] = r;
}
XF_OP(vrlimi128) {
    // VB's words rotated left by z, into the words of VD whose IMM bit is set
    // (bit 3 for word 0)
    uint32_t vd = i.VX128_4.VD128l | (i.VX128_4.VD128h << 5);
    const Vec128 b = c.v[i.VX128_4.VB128l | (i.VX128_4.VB128h << 5)];
    Vec128 r = c.v[vd];
    for (uint32_t k = 0; k < 4; k++)
        if (i.VX128_4.IMM & (8 >> k)) r.u32[k] = b.u32[(k + i.VX128_4.z) & 3];
    c.v[vd] = r;
}
XF_OP(vsldoi) { c.v[i.VXA.VD] = vmxi::sldoi(c.v[i.VXA.VA], c.v[i.VXA.VB], i.VXA.VC & 0xF); }
XF_OP(vsldoi128) { c.v[VX128_5_VD128] = vmxi::sldoi(c.v[VX128_5_VA128], c.v[VX128_5_VB128], VX128_5_SH); }

XF_OP(vspltb) { c.v[i.VX.VD] = vmxi::splat32(c.v[i.VX.VB].u8[(i.VX.VA & 0xF) ^ 3] * 0x01010101u); }
XF_OP(vsplth) { c.v[i.VX.VD] = vmxi::splat32(c.v[i.VX.VB].u16[(i.VX.VA & 0x7) ^ 1] * 0x00010001u); }
XF_OP(vspltw) { c.v[i.VX.VD] = vmxi::splat32(c.v[i.VX.VB].u32[i.VX.VA & 0x3]); }
XF_OP(vspltw128) { c.v[VX128_3_VD128] = vmxi::splat32(c.v[VX128_3_VB128].u32[VX128_3_IMM & 0x3]); }
XF_OP(vspltisb) { c.v[i.VX.VD] = vmxi::splat32(uint8_t(vmxi::simm5(i.VX.VA)) * 0x01010101u); }
XF_OP(vspltish) { c.v[i.VX.VD] = vmxi::splat32(uint16_t(vmxi::simm5(i.VX.VA)) * 0x00010001u); }
XF_OP(vspltisw) { c.v[i.VX.VD] = vmxi::splat32(uint32_t(vmxi::simm5(i.VX.VA))); }
XF_OP(vspltisw128) { c.v[VX128_3_VD128] = vmxi::splat32(uint32_t(vmxi::simm5(VX128_3_IMM))); }

// ---- shifts and rotates ----

XF_OP(vslb) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shl); }
XF_OP(vslh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shl); }
XF_OP(vslw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shl); }
XF_OP(vslw128) { c.v[VX128_VD128] = vmxi::map<uint32_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::shl); }
XF_OP(vsrb) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsrh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsrw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsrw128) { c.v[VX128_VD128] = vmxi::map<uint32_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::shr); }
XF_OP(vsrab) { c.v[i.VX.VD] = vmxi::map<int8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsrah) { c.v[i.VX.VD] = vmxi::map<int16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsraw) { c.v[i.VX.VD] = vmxi::map<int32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::shr); }
XF_OP(vsraw128) { c.v[VX128_VD128] = vmxi::map<int32_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::shr); }
XF_OP(vrlb) { c.v[i.VX.VD] = vmxi::map<uint8_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::rotl); }
XF_OP(vrlh) { c.v[i.VX.VD] = vmxi::map<uint16_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::rotl); }
XF_OP(vrlw) { c.v[i.VX.VD] = vmxi::map<uint32_t>(c.v[i.VX.VA], c.v[i.VX.VB], vmxi::rotl); }
XF_OP(vrlw128) { c.v[VX128_VD128] = vmxi::map<uint32_t>(c.v[VX128_VA128], c.v[VX128_VB128], vmxi::rotl); }

// the whole register by bits (vsl, vsr) and by octets (byte 15 of VB >> 3 & 15)
XF_OP(vsl) { c.v[i.VX.VD] = vmxi::shl_bits(c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vsr) { c.v[i.VX.VD] = vmxi::shr_bits(c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vslo) {
    uint32_t sh = c.v[i.VX.VB].u8[15 ^ 3] & 0x78;
    c.v[i.VX.VD] = vmxi::vec(vmxi::shl128(vmxi::u128(c.v[i.VX.VA]), sh));
}
XF_OP(vslo128) {
    uint32_t sh = c.v[VX128_VB128].u8[15 ^ 3] & 0x78;
    c.v[VX128_VD128] = vmxi::vec(vmxi::shl128(vmxi::u128(c.v[VX128_VA128]), sh));
}
XF_OP(vsro) {
    uint32_t sh = c.v[i.VX.VB].u8[15 ^ 3] & 0x78;
    c.v[i.VX.VD] = vmxi::vec(vmxi::shr128(vmxi::u128(c.v[i.VX.VA]), sh));
}
XF_OP(vsro128) {
    uint32_t sh = c.v[VX128_VB128].u8[15 ^ 3] & 0x78;
    c.v[VX128_VD128] = vmxi::vec(vmxi::shr128(vmxi::u128(c.v[VX128_VA128]), sh));
}

// ---- packs and unpacks ----

XF_OP(vpkpx) { c.v[i.VX.VD] = vmxi::pack_pixel(c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkshss) { c.v[i.VX.VD] = vmxi::pack16<int16_t, int8_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkshss128) { c.v[VX128_VD128] = vmxi::pack16<int16_t, int8_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkshus) { c.v[i.VX.VD] = vmxi::pack16<int16_t, uint8_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkshus128) { c.v[VX128_VD128] = vmxi::pack16<int16_t, uint8_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkuhum) { c.v[i.VX.VD] = vmxi::pack16<uint16_t, uint8_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkuhum128) { c.v[VX128_VD128] = vmxi::pack16<uint16_t, uint8_t, false>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkuhus) { c.v[i.VX.VD] = vmxi::pack16<uint16_t, uint8_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkuhus128) { c.v[VX128_VD128] = vmxi::pack16<uint16_t, uint8_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkswss) { c.v[i.VX.VD] = vmxi::pack32<int32_t, int16_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkswss128) { c.v[VX128_VD128] = vmxi::pack32<int32_t, int16_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkswus) { c.v[i.VX.VD] = vmxi::pack32<int32_t, uint16_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkswus128) { c.v[VX128_VD128] = vmxi::pack32<int32_t, uint16_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkuwum) { c.v[i.VX.VD] = vmxi::pack32<uint32_t, uint16_t, false>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkuwum128) { c.v[VX128_VD128] = vmxi::pack32<uint32_t, uint16_t, false>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }
XF_OP(vpkuwus) { c.v[i.VX.VD] = vmxi::pack32<uint32_t, uint16_t, true>(c, c.v[i.VX.VA], c.v[i.VX.VB]); }
XF_OP(vpkuwus128) { c.v[VX128_VD128] = vmxi::pack32<uint32_t, uint16_t, true>(c, c.v[VX128_VA128], c.v[VX128_VB128]); }

XF_OP(vupkhpx) { c.v[i.VX.VD] = vmxi::unpack_pixel(c.v[i.VX.VB], 0); }
XF_OP(vupklpx) { c.v[i.VX.VD] = vmxi::unpack_pixel(c.v[i.VX.VB], 4); }
XF_OP(vupkhsb) { c.v[i.VX.VD] = vmxi::unpack8(c.v[i.VX.VB], 0); }
XF_OP(vupklsb) { c.v[i.VX.VD] = vmxi::unpack8(c.v[i.VX.VB], 8); }
XF_OP(vupkhsh) { c.v[i.VX.VD] = vmxi::unpack16(c.v[i.VX.VB], 0); }
XF_OP(vupklsh) { c.v[i.VX.VD] = vmxi::unpack16(c.v[i.VX.VB], 4); }
// VA = 0x60 is the halfword form (vupkhsh128/vupklsh128), as Xenia decodes it
XF_OP(vupkhsb128) {
    c.v[VX128_VD128] = VX128_VA128 == 0x60 ? vmxi::unpack16(c.v[VX128_VB128], 0) : vmxi::unpack8(c.v[VX128_VB128], 0);
}
XF_OP(vupklsb128) {
    c.v[VX128_VD128] = VX128_VA128 == 0x60 ? vmxi::unpack16(c.v[VX128_VB128], 4) : vmxi::unpack8(c.v[VX128_VB128], 8);
}

} // namespace xf::ppc::ops
