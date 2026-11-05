// Loads and stores of every kind, reservations, cache lines (ppc_ops.h).
// Translated from Xenia's ppc_emit_memory.cc, the vector loads and stores of
// its ppc_emit_altivec.cc, and what its x64 backend does with them
// (x64_seq_memory.cc, x64_sequences.cc) (BSD; see LICENSE-xenia).
//
// Effective addresses are the low 32 bits of (RA|0) + d or (RA|0) + (RB),
// as Xenia's backend takes them; the update forms write the whole 64-bit sum
// to RA after the load or store (Xenia's StoreEA), so RA wins when RT = RA.

#pragma once

#include "xenoforge/ppc_ops.h"

namespace xf::ppc::ops {

// ---- effective addresses ----

// D form: (RA|0) + EXTS(D); with update, (RA) + EXTS(D)
XF_INLINE uint64_t ea_d(Context &c, Instr i) { return (i.D.RA ? c.r[i.D.RA] : 0) + uint64_t(exts16(i.D.DS)); }
XF_INLINE uint64_t ea_du(Context &c, Instr i) { return c.r[i.D.RA] + uint64_t(exts16(i.D.DS)); }
// DS form: (RA|0) + EXTS(DS || 0b00); with update, (RA) + EXTS(DS || 0b00)
XF_INLINE uint64_t ea_ds(Context &c, Instr i) { return (i.DS.RA ? c.r[i.DS.RA] : 0) + uint64_t(exts16(i.DS.DS << 2)); }
XF_INLINE uint64_t ea_dsu(Context &c, Instr i) { return c.r[i.DS.RA] + uint64_t(exts16(i.DS.DS << 2)); }
// X form: (RA|0) + (RB); with update, (RA) + (RB)
XF_INLINE uint64_t ea_x(Context &c, Instr i) { return (i.X.RA ? c.r[i.X.RA] : 0) + c.r[i.X.RB]; }
XF_INLINE uint64_t ea_xu(Context &c, Instr i) { return c.r[i.X.RA] + c.r[i.X.RB]; }
// the vector forms (X and VX128_1 put RA and RB in the same place)
XF_INLINE uint32_t ea_v(Context &c, uint32_t ra, uint32_t rb) { return uint32_t((ra ? c.r[ra] : 0) + c.r[rb]); }

// ---- what is read and written ----

XF_INLINE uint64_t load_s16(Context &c, uint32_t ea) { return uint64_t(int64_t(int16_t(load16(c, ea)))); }
XF_INLINE uint64_t load_s32(Context &c, uint32_t ea) { return uint64_t(int64_t(int32_t(load32(c, ea)))); }

// lfs: the single widened exactly (Xenia's CONVERT_F64_F32: cvtss2sd, which
// keeps denormals in its FPU MXCSR mode, then the quiet bit the convert forced
// put back as the single had it, so a NaN keeps its payload and stays
// signaling)
XF_INLINE double single_to_double(uint32_t bits) {
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u)
        return std::bit_cast<double>(uint64_t(bits & 0x80000000u) << 32 | 0x7FF0000000000000ull |
                                     uint64_t(bits & 0x007FFFFFu) << 29);
    return double(std::bit_cast<float>(bits));
}
// stfs: rounded as the host rounds (Xenia's CONVERT_F32_F64: cvtsd2ss); a
// NaN keeps its sign, the top of its payload and its own quiet bit
XF_INLINE uint32_t double_to_single(double value) {
    uint64_t bits = std::bit_cast<uint64_t>(value);
    if ((bits & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull)
        return (uint32_t(bits >> 32) & 0x80000000u) | 0x7F800000u | (uint32_t(bits >> 29) & 0x007FFFFFu);
    return std::bit_cast<uint32_t>(float(value));
}

// a vector at an aligned address: the 16 bytes with each word swapped
// (Xenia's Load of a VEC128 then ByteSwap), so words land in guest order
XF_INLINE Vec128 load_vec(Context &c, uint32_t ea) {
    Vec128 v;
    std::memcpy(&v, host(c, ea), 16);
    for (int k = 0; k < 4; k++) v.u32[k] = __builtin_bswap32(v.u32[k]);
    return v;
}
XF_INLINE void store_vec(Context &c, uint32_t ea, const Vec128 &v) {
    Vec128 m;
    for (int k = 0; k < 4; k++) m.u32[k] = __builtin_bswap32(v.u32[k]);
    std::memcpy(host(c, ea), &m, 16);
}

// lvlx (Xenia's LVL): the bytes from EA to the end of its 16-byte block,
// to the left of the register, zeros after them
inline Vec128 load_vector_left(Context &c, uint32_t ea) {
    uint32_t eb = ea & 15;
    uint8_t bytes[32] = {};
    std::memcpy(bytes, host(c, ea & ~15u), 16);
    Vec128 v;
    std::memcpy(&v, bytes + eb, 16);
    for (int k = 0; k < 4; k++) v.u32[k] = __builtin_bswap32(v.u32[k]);
    return v;
}
// lvrx (Xenia's LVR): the bytes from the start of EA's block up to EA, to
// the right of the register, zeros before them; nothing is read when EA is
// aligned (memcpy's tails rely on it at the end of a page)
inline Vec128 load_vector_right(Context &c, uint32_t ea) {
    uint32_t eb = ea & 15;
    if (!eb) return Vec128{};
    uint8_t bytes[32] = {};
    std::memcpy(bytes + 16, host(c, ea & ~15u), 16);
    Vec128 v;
    std::memcpy(&v, bytes + eb, 16);
    for (int k = 0; k < 4; k++) v.u32[k] = __builtin_bswap32(v.u32[k]);
    return v;
}
// stvlx (Xenia's STVL): the register's first 16 - (EA & 15) bytes, from EA
// to the end of its block
inline void store_vector_left(Context &c, uint32_t ea, const Vec128 &v) {
    Vec128 m;
    for (int k = 0; k < 4; k++) m.u32[k] = __builtin_bswap32(v.u32[k]);
    std::memcpy(host(c, ea), &m, 16 - (ea & 15));
}
// stvrx (Xenia's STVR): the register's last EA & 15 bytes, from the start of
// EA's block up to EA; nothing is written when EA is aligned
inline void store_vector_right(Context &c, uint32_t ea, const Vec128 &v) {
    uint32_t eb = ea & 15;
    if (!eb) return;
    Vec128 m;
    for (int k = 0; k < 4; k++) m.u32[k] = __builtin_bswap32(v.u32[k]);
    std::memcpy(host(c, ea & ~15u), m.u8 + 16 - eb, eb);
}

// lswi and stswi (Book I; Xenia leaves the string instructions out): NB
// bytes (32 when 0) from RT on, four to a register in its low word, wrapping
// from r31 to r0, the high word and the last register's unused bytes zero
inline void load_string(Context &c, uint32_t rt, uint32_t ea, uint32_t n) {
    for (uint32_t k = 0, r = rt; k < n; k++, ea++) {
        if (k % 4 == 0) {
            if (k) r = (r + 1) % 32;
            c.r[r] = 0;
        }
        c.r[r] |= uint64_t(load8(c, ea)) << (24 - 8 * (k % 4));
    }
}
inline void store_string(Context &c, uint32_t rs, uint32_t ea, uint32_t n) {
    for (uint32_t k = 0, r = rs; k < n; k++, ea++) {
        if (k && k % 4 == 0) r = (r + 1) % 32;
        store8(c, ea, uint8_t(c.r[r] >> (24 - 8 * (k % 4))));
    }
}

// The reservation (Xenia's RESERVED_LOAD and RESERVED_STORE): lwarx and
// ldarx keep the address and the value read, after a barrier; stwcx. and
// stdcx. store only while it is held and memory still holds that value, as
// one compare-exchange, and drop it either way. CR0 gets lt = gt = 0 and
// eq = stored; Xenia's emitter leaves CR0's SO as it was, and so does this.
inline bool store_conditional32(Context &c, uint32_t ea, uint32_t value) {
    if (!c.reserved) return false;
    c.reserved = false;
    uint32_t expected = __builtin_bswap32(uint32_t(c.reserve_value));
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t *>(host(c, ea)), &expected, __builtin_bswap32(value),
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
inline bool store_conditional64(Context &c, uint32_t ea, uint64_t value) {
    if (!c.reserved) return false;
    c.reserved = false;
    uint64_t expected = __builtin_bswap64(c.reserve_value);
    return __atomic_compare_exchange_n(reinterpret_cast<uint64_t *>(host(c, ea)), &expected, __builtin_bswap64(value),
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
XF_INLINE void reserve(Context &c, uint32_t ea, uint64_t value) {
    c.reserved = true, c.reserve_address = ea, c.reserve_value = value;
}

// ---- integer loads ----

XF_OP(lbz) { c.r[i.D.RT] = load8(c, uint32_t(ea_d(c, i))); }
XF_OP(lbzu) {
    uint64_t ea = ea_du(c, i);
    c.r[i.D.RT] = load8(c, uint32_t(ea)), c.r[i.D.RA] = ea;
}
XF_OP(lbzux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load8(c, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(lbzx) { c.r[i.X.RT] = load8(c, uint32_t(ea_x(c, i))); }

XF_OP(lha) { c.r[i.D.RT] = load_s16(c, uint32_t(ea_d(c, i))); }
XF_OP(lhau) {
    uint64_t ea = ea_du(c, i);
    c.r[i.D.RT] = load_s16(c, uint32_t(ea)), c.r[i.D.RA] = ea;
}
XF_OP(lhaux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load_s16(c, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(lhax) { c.r[i.X.RT] = load_s16(c, uint32_t(ea_x(c, i))); }

XF_OP(lhz) { c.r[i.D.RT] = load16(c, uint32_t(ea_d(c, i))); }
XF_OP(lhzu) {
    uint64_t ea = ea_du(c, i);
    c.r[i.D.RT] = load16(c, uint32_t(ea)), c.r[i.D.RA] = ea;
}
XF_OP(lhzux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load16(c, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(lhzx) { c.r[i.X.RT] = load16(c, uint32_t(ea_x(c, i))); }

XF_OP(lwa) { c.r[i.DS.RT] = load_s32(c, uint32_t(ea_ds(c, i))); }
XF_OP(lwaux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load_s32(c, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(lwax) { c.r[i.X.RT] = load_s32(c, uint32_t(ea_x(c, i))); }

XF_OP(lwz) { c.r[i.D.RT] = load32_io(c, i.D.RA, uint32_t(ea_d(c, i))); }
XF_OP(lwzu) {
    uint64_t ea = ea_du(c, i);
    c.r[i.D.RT] = load32_io(c, i.D.RA, uint32_t(ea)), c.r[i.D.RA] = ea;
}
XF_OP(lwzux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load32_io(c, i.X.RA, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(lwzx) { c.r[i.X.RT] = load32_io(c, i.X.RA, uint32_t(ea_x(c, i))); }

XF_OP(ld) { c.r[i.DS.RT] = load64(c, uint32_t(ea_ds(c, i))); }
XF_OP(ldu) {
    uint64_t ea = ea_dsu(c, i);
    c.r[i.DS.RT] = load64(c, uint32_t(ea)), c.r[i.DS.RA] = ea;
}
XF_OP(ldux) {
    uint64_t ea = ea_xu(c, i);
    c.r[i.X.RT] = load64(c, uint32_t(ea)), c.r[i.X.RA] = ea;
}
XF_OP(ldx) { c.r[i.X.RT] = load64(c, uint32_t(ea_x(c, i))); }

// ---- integer stores ----

XF_OP(stb) { store8(c, uint32_t(ea_d(c, i)), uint8_t(c.r[i.D.RT])); }
XF_OP(stbu) {
    uint64_t ea = ea_du(c, i);
    store8(c, uint32_t(ea), uint8_t(c.r[i.D.RT])), c.r[i.D.RA] = ea;
}
XF_OP(stbux) {
    uint64_t ea = ea_xu(c, i);
    store8(c, uint32_t(ea), uint8_t(c.r[i.X.RT])), c.r[i.X.RA] = ea;
}
XF_OP(stbx) { store8(c, uint32_t(ea_x(c, i)), uint8_t(c.r[i.X.RT])); }

XF_OP(sth) { store16(c, uint32_t(ea_d(c, i)), uint16_t(c.r[i.D.RT])); }
XF_OP(sthu) {
    uint64_t ea = ea_du(c, i);
    store16(c, uint32_t(ea), uint16_t(c.r[i.D.RT])), c.r[i.D.RA] = ea;
}
XF_OP(sthux) {
    uint64_t ea = ea_xu(c, i);
    store16(c, uint32_t(ea), uint16_t(c.r[i.X.RT])), c.r[i.X.RA] = ea;
}
XF_OP(sthx) { store16(c, uint32_t(ea_x(c, i)), uint16_t(c.r[i.X.RT])); }

XF_OP(stw) { store32_io(c, i.D.RA, uint32_t(ea_d(c, i)), uint32_t(c.r[i.D.RT])); }
XF_OP(stwu) {
    uint64_t ea = ea_du(c, i);
    store32_io(c, i.D.RA, uint32_t(ea), uint32_t(c.r[i.D.RT])), c.r[i.D.RA] = ea;
}
XF_OP(stwux) {
    uint64_t ea = ea_xu(c, i);
    store32_io(c, i.X.RA, uint32_t(ea), uint32_t(c.r[i.X.RT])), c.r[i.X.RA] = ea;
}
XF_OP(stwx) { store32_io(c, i.X.RA, uint32_t(ea_x(c, i)), uint32_t(c.r[i.X.RT])); }

XF_OP(std) { store64(c, uint32_t(ea_ds(c, i)), c.r[i.DS.RT]); }
XF_OP(stdu) {
    uint64_t ea = ea_dsu(c, i);
    store64(c, uint32_t(ea), c.r[i.DS.RT]), c.r[i.DS.RA] = ea;
}
XF_OP(stdux) {
    uint64_t ea = ea_xu(c, i);
    store64(c, uint32_t(ea), c.r[i.X.RT]), c.r[i.X.RA] = ea;
}
XF_OP(stdx) { store64(c, uint32_t(ea_x(c, i)), c.r[i.X.RT]); }

// ---- byte-reversed: memory's little-endian value, so no swap ----

XF_OP(lhbrx) { c.r[i.X.RT] = __builtin_bswap16(load16(c, uint32_t(ea_x(c, i)))); }
XF_OP(lwbrx) { c.r[i.X.RT] = __builtin_bswap32(load32_io(c, i.X.RA, uint32_t(ea_x(c, i)))); }
XF_OP(ldbrx) { c.r[i.X.RT] = __builtin_bswap64(load64(c, uint32_t(ea_x(c, i)))); }
XF_OP(sthbrx) { store16(c, uint32_t(ea_x(c, i)), __builtin_bswap16(uint16_t(c.r[i.X.RT]))); }
XF_OP(stwbrx) { store32_io(c, i.X.RA, uint32_t(ea_x(c, i)), __builtin_bswap32(uint32_t(c.r[i.X.RT]))); }
XF_OP(stdbrx) { store64(c, uint32_t(ea_x(c, i)), __builtin_bswap64(c.r[i.X.RT])); }

// ---- multiple and string ----

// lmw: words into RT..r31 from (RA|0) + EXTS(D), skipping RA (Xenia)
XF_OP(lmw) {
    uint64_t b = i.D.RA ? c.r[i.D.RA] : 0;
    for (uint32_t r = i.D.RT; r < 32; r++)
        if (r != i.D.RA) c.r[r] = load32(c, uint32_t(b + uint64_t(exts16(i.D.DS)) + (r - i.D.RT) * 4));
}
XF_OP(stmw) {
    uint64_t b = i.D.RA ? c.r[i.D.RA] : 0;
    for (uint32_t r = i.D.RT; r < 32; r++)
        store32(c, uint32_t(b + uint64_t(exts16(i.D.DS)) + (r - i.D.RT) * 4), uint32_t(c.r[r]));
}
// the byte count is NB, 32 when NB = 0 (the RB field holds NB); lswx and
// stswx take theirs from XER[57:63], which the context does not keep, so
// they stay unimplemented, as in Xenia
XF_OP(lswi) { load_string(c, i.X.RT, uint32_t(i.X.RA ? c.r[i.X.RA] : 0), i.X.RB ? i.X.RB : 32); }
XF_OP(stswi) { store_string(c, i.X.RT, uint32_t(i.X.RA ? c.r[i.X.RA] : 0), i.X.RB ? i.X.RB : 32); }

// ---- reservations ----

XF_OP(lwarx) {
    uint32_t ea = uint32_t(ea_x(c, i));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t value = load32(c, ea);
    reserve(c, ea, value), c.r[i.X.RT] = value;
}
XF_OP(ldarx) {
    uint32_t ea = uint32_t(ea_x(c, i));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t value = load64(c, ea);
    reserve(c, ea, value), c.r[i.X.RT] = value;
}
XF_OP(stwcx) {
    bool stored = store_conditional32(c, uint32_t(ea_x(c, i)), uint32_t(c.r[i.X.RT]));
    c.cr[0] = 0, c.cr[1] = 0, c.cr[2] = stored;
}
XF_OP(stdcx) {
    bool stored = store_conditional64(c, uint32_t(ea_x(c, i)), c.r[i.X.RT]);
    c.cr[0] = 0, c.cr[1] = 0, c.cr[2] = stored;
}

// ---- floating point ----

XF_OP(lfd) { c.f[i.D.RT] = std::bit_cast<double>(load64(c, uint32_t(ea_d(c, i)))); }
XF_OP(lfdu) {
    uint64_t ea = ea_du(c, i);
    c.f[i.D.RT] = std::bit_cast<double>(load64(c, uint32_t(ea))), c.r[i.D.RA] = ea;
}
XF_OP(lfdux) {
    uint64_t ea = ea_xu(c, i);
    c.f[i.X.RT] = std::bit_cast<double>(load64(c, uint32_t(ea))), c.r[i.X.RA] = ea;
}
XF_OP(lfdx) { c.f[i.X.RT] = std::bit_cast<double>(load64(c, uint32_t(ea_x(c, i)))); }

XF_OP(lfs) { c.f[i.D.RT] = single_to_double(load32(c, uint32_t(ea_d(c, i)))); }
XF_OP(lfsu) {
    uint64_t ea = ea_du(c, i);
    c.f[i.D.RT] = single_to_double(load32(c, uint32_t(ea))), c.r[i.D.RA] = ea;
}
XF_OP(lfsux) {
    uint64_t ea = ea_xu(c, i);
    c.f[i.X.RT] = single_to_double(load32(c, uint32_t(ea))), c.r[i.X.RA] = ea;
}
XF_OP(lfsx) { c.f[i.X.RT] = single_to_double(load32(c, uint32_t(ea_x(c, i)))); }

XF_OP(stfd) { store64(c, uint32_t(ea_d(c, i)), std::bit_cast<uint64_t>(c.f[i.D.RT])); }
XF_OP(stfdu) {
    uint64_t ea = ea_du(c, i);
    store64(c, uint32_t(ea), std::bit_cast<uint64_t>(c.f[i.D.RT])), c.r[i.D.RA] = ea;
}
XF_OP(stfdux) {
    uint64_t ea = ea_xu(c, i);
    store64(c, uint32_t(ea), std::bit_cast<uint64_t>(c.f[i.X.RT])), c.r[i.X.RA] = ea;
}
XF_OP(stfdx) { store64(c, uint32_t(ea_x(c, i)), std::bit_cast<uint64_t>(c.f[i.X.RT])); }

XF_OP(stfs) { store32(c, uint32_t(ea_d(c, i)), double_to_single(c.f[i.D.RT])); }
XF_OP(stfsu) {
    uint64_t ea = ea_du(c, i);
    store32(c, uint32_t(ea), double_to_single(c.f[i.D.RT])), c.r[i.D.RA] = ea;
}
XF_OP(stfsux) {
    uint64_t ea = ea_xu(c, i);
    store32(c, uint32_t(ea), double_to_single(c.f[i.X.RT])), c.r[i.X.RA] = ea;
}
XF_OP(stfsx) { store32(c, uint32_t(ea_x(c, i)), double_to_single(c.f[i.X.RT])); }

// the low word of the register's bits
XF_OP(stfiwx) { store32(c, uint32_t(ea_x(c, i)), uint32_t(std::bit_cast<uint64_t>(c.f[i.X.RT]))); }

// ---- vectors ----

// lvx and the element loads, which load the whole aligned vector as Xenia
// does (the other elements are undefined in the architecture)
XF_OP(lvx) { c.v[i.X.RT] = load_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u); }
XF_OP(lvxl) { c.v[i.X.RT] = load_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u); }
XF_OP(lvx128) { c.v[VX128_1_VD128] = load_vec(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~15u); }
XF_OP(lvxl128) { c.v[VX128_1_VD128] = load_vec(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~15u); }
XF_OP(lvebx) { c.v[i.X.RT] = load_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u); }
XF_OP(lvehx) { c.v[i.X.RT] = load_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u); }
XF_OP(lvewx) { c.v[i.X.RT] = load_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u); }
XF_OP(lvewx128) { c.v[VX128_1_VD128] = load_vec(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~15u); }

XF_OP(stvx) { store_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u, c.v[i.X.RT]); }
XF_OP(stvxl) { store_vec(c, ea_v(c, i.X.RA, i.X.RB) & ~15u, c.v[i.X.RT]); }
XF_OP(stvx128) { store_vec(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~15u, c.v[VX128_1_VD128]); }
XF_OP(stvxl128) { store_vec(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~15u, c.v[VX128_1_VD128]); }

// the element stores: element (EA & 15) / size to EA aligned to its size
XF_OP(stvebx) {
    uint32_t ea = ea_v(c, i.X.RA, i.X.RB);
    store8(c, ea, c.v[i.X.RT].u8[(ea & 15) ^ 3]);
}
XF_OP(stvehx) {
    uint32_t ea = ea_v(c, i.X.RA, i.X.RB) & ~1u;
    store16(c, ea, c.v[i.X.RT].u16[((ea & 15) >> 1) ^ 1]);
}
XF_OP(stvewx) {
    uint32_t ea = ea_v(c, i.X.RA, i.X.RB) & ~3u;
    store32(c, ea, c.v[i.X.RT].u32[(ea & 15) >> 2]);
}
XF_OP(stvewx128) {
    uint32_t ea = ea_v(c, i.VX128_1.RA, i.VX128_1.RB) & ~3u;
    store32(c, ea, c.v[VX128_1_VD128].u32[(ea & 15) >> 2]);
}

// the unaligned halves (the "l" forms are the same: a cache hint)
XF_OP(lvlx) { c.v[i.X.RT] = load_vector_left(c, ea_v(c, i.X.RA, i.X.RB)); }
XF_OP(lvlxl) { c.v[i.X.RT] = load_vector_left(c, ea_v(c, i.X.RA, i.X.RB)); }
XF_OP(lvlx128) { c.v[VX128_1_VD128] = load_vector_left(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB)); }
XF_OP(lvlxl128) { c.v[VX128_1_VD128] = load_vector_left(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB)); }
XF_OP(lvrx) { c.v[i.X.RT] = load_vector_right(c, ea_v(c, i.X.RA, i.X.RB)); }
XF_OP(lvrxl) { c.v[i.X.RT] = load_vector_right(c, ea_v(c, i.X.RA, i.X.RB)); }
XF_OP(lvrx128) { c.v[VX128_1_VD128] = load_vector_right(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB)); }
XF_OP(lvrxl128) { c.v[VX128_1_VD128] = load_vector_right(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB)); }
XF_OP(stvlx) { store_vector_left(c, ea_v(c, i.X.RA, i.X.RB), c.v[i.X.RT]); }
XF_OP(stvlxl) { store_vector_left(c, ea_v(c, i.X.RA, i.X.RB), c.v[i.X.RT]); }
XF_OP(stvlx128) { store_vector_left(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB), c.v[VX128_1_VD128]); }
XF_OP(stvlxl128) { store_vector_left(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB), c.v[VX128_1_VD128]); }
XF_OP(stvrx) { store_vector_right(c, ea_v(c, i.X.RA, i.X.RB), c.v[i.X.RT]); }
XF_OP(stvrxl) { store_vector_right(c, ea_v(c, i.X.RA, i.X.RB), c.v[i.X.RT]); }
XF_OP(stvrx128) { store_vector_right(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB), c.v[VX128_1_VD128]); }
XF_OP(stvrxl128) { store_vector_right(c, ea_v(c, i.VX128_1.RA, i.VX128_1.RB), c.v[VX128_1_VD128]); }

// ---- cache lines ----

// dcbz and dcbz128 both zero the 128-byte line, as Xenia does ("On Xbox360
// there is no short cache line"); compiled clearing loops step dcbz by 128
XF_OP(dcbz) { std::memset(host(c, uint32_t(ea_x(c, i)) & ~127u), 0, 128); }
XF_OP(dcbz128) { std::memset(host(c, uint32_t(ea_x(c, i)) & ~127u), 0, 128); }
// hints: nothing to do (Xenia's disable_prefetch_and_cachecontrol default)
XF_OP(dcbf) {}
XF_OP(dcbi) {}
XF_OP(dcbst) {}
XF_OP(dcbt) {}
XF_OP(dcbtst) {}
XF_OP(icbi) {}

} // namespace xf::ppc::ops
