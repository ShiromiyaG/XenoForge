// Integer arithmetic, logic, compares, rotates and shifts, and the memory
// barriers (ppc_ops.h). Translated from Xenia's ppc_emit_alu.cc and
// ppc_emit_memory.cc (sync, eieio, isync), with what its x64 backend does
// with the HIR they emit (BSD; see LICENSE-xenia).
//
// The Xenon runs code in 32-bit mode (MSR[SF] = 0; Xenia's MSR dump is
// 0x9030): results are 64-bit, but CA, CR0 and OV come from the low word.
// Xenia leaves OE (XER[OV]) unimplemented; here it follows the architecture
// for 32-bit mode (add, subtract and negate: signed overflow of the low
// word; mullw: the product does not fit in 32 bits; mulld: in 64; the
// divides: divisor 0 or the most negative number by -1). Division by zero
// and that overflow give 0, as Xenia's backend does (the architecture
// leaves them undefined).

#pragma once

#include "xenoforge/ppc_ops.h"

#include <atomic>

namespace xf::ppc::ops {

// ---- helpers ----

// The XO-form adds and subtracts, as one: RT <- a + b + cin; CA (when the
// instruction sets it) the carry out of the low word; OV the signed
// overflow of the low word; CR0 after OV, so SO is the new one.
XF_INLINE void add_xo(Context &c, Instr i, uint64_t a, uint64_t b, uint32_t cin, bool sets_ca) {
    uint64_t v = a + b + cin;
    c.r[i.XO.RT] = v;
    if (sets_ca) c.xer_ca = uint8_t((uint64_t(uint32_t(a)) + uint32_t(b) + cin) >> 32);
    if (i.XO.OE) set_ov(c, int32_t((uint32_t(a) ^ uint32_t(v)) & (uint32_t(b) ^ uint32_t(v))) < 0);
    if (i.XO.Rc) update_cr0(c, v);
}

// D-form adds that set CA (addic, addic., subfic): carry out of the low word
XF_INLINE uint8_t carry32(uint64_t a, uint64_t b, uint32_t cin) {
    return uint8_t((uint64_t(uint32_t(a)) + uint32_t(b) + cin) >> 32);
}

// cmp/cmpi/cmpl/cmpli: field n from two values of any width; SO is XER's
template <typename T> XF_INLINE void compare(Context &c, uint32_t n, T a, T b) {
    c.cr[4 * n + 0] = a < b;
    c.cr[4 * n + 1] = a > b;
    c.cr[4 * n + 2] = a == b;
    c.cr[4 * n + 3] = c.xer_so;
}

// ROTL32 as the rotates see it: the rotated low word in both halves
XF_INLINE uint64_t rotl32_both(uint64_t rs, uint32_t n) {
    uint64_t r = std::rotl(uint32_t(rs), int(n & 31));
    return r | (r << 32);
}

// The XO-form results that set OV only through OE, then CR0
XF_INLINE void store_xo(Context &c, Instr i, uint64_t v, bool overflow) {
    c.r[i.XO.RT] = v;
    if (i.XO.OE) set_ov(c, overflow);
    if (i.XO.Rc) update_cr0(c, v);
}

// mulh*: no OE (the bit is reserved; the decoder still sends it here), CR0
XF_INLINE void store_xo_no_oe(Context &c, Instr i, uint64_t v) {
    c.r[i.XO.RT] = v;
    if (i.XO.Rc) update_cr0(c, v);
}

// X-form logic: RA <- v, CR0 if Rc
XF_INLINE void store_x(Context &c, Instr i, uint64_t v) {
    c.r[i.X.RA] = v;
    if (i.X.Rc) update_cr0(c, v);
}

// MD/MDS-form rotates: RA <- v, CR0 if Rc (MD and MDS share RA and Rc)
XF_INLINE void store_md(Context &c, Instr i, uint64_t v) {
    c.r[i.MD.RA] = v;
    if (i.MD.Rc) update_cr0(c, v);
}

// M-form rotates: RA <- v, CR0 if Rc
XF_INLINE void store_m(Context &c, Instr i, uint64_t v) {
    c.r[i.M.RA] = v;
    if (i.M.Rc) update_cr0(c, v);
}

// sraw/srawi: the low word shifted right by n (0-63, 32 and up give the sign),
// sign-extended; CA if the word is negative and 1 bits were shifted out
inline void shift_right_algebraic_word(Context &c, Instr i, uint32_t n) {
    int32_t s = int32_t(c.r[i.X.RT]);
    int64_t v;
    bool lost;
    if (n & 0x20) {
        v = s >> 31;
        lost = s != 0;
    } else {
        v = s >> n;
        lost = (uint32_t(s) & ((uint32_t(1) << n) - 1)) != 0;
    }
    c.xer_ca = s < 0 && lost;
    store_x(c, i, uint64_t(v));
}

// srad/sradi: the same on the doubleword, n 0-127 (64 and up give the sign)
inline void shift_right_algebraic_double(Context &c, Instr i, uint32_t n) {
    int64_t s = int64_t(c.r[i.X.RT]);
    int64_t v;
    bool lost;
    if (n & 0x40) {
        v = s >> 63;
        lost = s != 0;
    } else {
        v = s >> n;
        lost = (uint64_t(s) & ((uint64_t(1) << n) - 1)) != 0;
    }
    c.xer_ca = s < 0 && lost;
    store_x(c, i, uint64_t(v));
}

// ---- add and subtract ----

XF_OP(addx) {
    // RD <- (RA) + (RB)
    add_xo(c, i, c.r[i.XO.RA], c.r[i.XO.RB], 0, false);
}

XF_OP(addcx) {
    // RD <- (RA) + (RB), CA <- carry
    add_xo(c, i, c.r[i.XO.RA], c.r[i.XO.RB], 0, true);
}

XF_OP(addex) {
    // RD <- (RA) + (RB) + XER[CA], CA <- carry
    add_xo(c, i, c.r[i.XO.RA], c.r[i.XO.RB], c.xer_ca, true);
}

XF_OP(addmex) {
    // RT <- (RA) + CA - 1, CA <- carry
    add_xo(c, i, c.r[i.XO.RA], ~uint64_t(0), c.xer_ca, true);
}

XF_OP(addzex) {
    // RT <- (RA) + CA, CA <- carry
    add_xo(c, i, c.r[i.XO.RA], 0, c.xer_ca, true);
}

XF_OP(addi) {
    // RT <- (RA|0) + EXTS(SI)
    c.r[i.D.RT] = (i.D.RA ? c.r[i.D.RA] : 0) + uint64_t(exts16(i.D.DS));
}

XF_OP(addic) {
    // RT <- (RA) + EXTS(SI), CA <- carry
    uint64_t ra = c.r[i.D.RA], si = uint64_t(exts16(i.D.DS));
    c.r[i.D.RT] = ra + si;
    c.xer_ca = carry32(ra, si, 0);
}

XF_OP(addicx) {
    // RT <- (RA) + EXTS(SI), CA <- carry, CR0
    uint64_t ra = c.r[i.D.RA], si = uint64_t(exts16(i.D.DS)), v = ra + si;
    c.r[i.D.RT] = v;
    c.xer_ca = carry32(ra, si, 0);
    update_cr0(c, v);
}

XF_OP(addis) {
    // RT <- (RA|0) + EXTS(SI || i16.0)
    c.r[i.D.RT] = (i.D.RA ? c.r[i.D.RA] : 0) + uint64_t(exts16(i.D.DS) << 16);
}

XF_OP(subfx) {
    // RT <- ¬(RA) + (RB) + 1
    add_xo(c, i, ~c.r[i.XO.RA], c.r[i.XO.RB], 1, false);
}

XF_OP(subfcx) {
    // RT <- ¬(RA) + (RB) + 1, CA <- carry
    add_xo(c, i, ~c.r[i.XO.RA], c.r[i.XO.RB], 1, true);
}

XF_OP(subfex) {
    // RT <- ¬(RA) + (RB) + CA, CA <- carry
    add_xo(c, i, ~c.r[i.XO.RA], c.r[i.XO.RB], c.xer_ca, true);
}

XF_OP(subfmex) {
    // RT <- ¬(RA) + CA - 1, CA <- carry
    add_xo(c, i, ~c.r[i.XO.RA], ~uint64_t(0), c.xer_ca, true);
}

XF_OP(subfzex) {
    // RT <- ¬(RA) + CA, CA <- carry
    add_xo(c, i, ~c.r[i.XO.RA], 0, c.xer_ca, true);
}

XF_OP(subficx) {
    // RT <- ¬(RA) + EXTS(SI) + 1, CA <- carry
    uint64_t ra = c.r[i.D.RA], si = uint64_t(exts16(i.D.DS));
    c.r[i.D.RT] = si - ra;
    c.xer_ca = carry32(~ra, si, 1);
}

XF_OP(negx) {
    // RT <- ¬(RA) + 1
    add_xo(c, i, ~c.r[i.XO.RA], 0, 1, false);
}

// ---- multiply and divide ----

XF_OP(mulli) {
    // RT <- ((RA) × EXTS(SI))[64:127]
    c.r[i.D.RT] = c.r[i.D.RA] * uint64_t(exts16(i.D.DS));
}

XF_OP(mullwx) {
    // RT <- (RA)[32:63] × (RB)[32:63], the whole 64-bit product
    int64_t v = int64_t(int32_t(c.r[i.XO.RA])) * int64_t(int32_t(c.r[i.XO.RB]));
    store_xo(c, i, uint64_t(v), v != int64_t(int32_t(v)));
}

XF_OP(mulldx) {
    // RT <- ((RA) × (RB))[64:127]
    int64_t v;
    bool overflow = __builtin_mul_overflow(int64_t(c.r[i.XO.RA]), int64_t(c.r[i.XO.RB]), &v);
    store_xo(c, i, uint64_t(v), overflow);
}

XF_OP(mulhwx) {
    // RT <- ((RA)[32:63] × (RB)[32:63])[0:31], sign-extended (as Xenia)
    int64_t v = (int64_t(int32_t(c.r[i.XO.RA])) * int64_t(int32_t(c.r[i.XO.RB]))) >> 32;
    store_xo_no_oe(c, i, uint64_t(v));
}

XF_OP(mulhwux) {
    // RT <- ((RA)[32:63] × (RB)[32:63])[0:31], zero-extended
    uint64_t v = (uint64_t(uint32_t(c.r[i.XO.RA])) * uint32_t(c.r[i.XO.RB])) >> 32;
    store_xo_no_oe(c, i, v);
}

XF_OP(mulhdx) {
    // RT <- ((RA) × (RB))[0:63], signed
    int64_t v = int64_t((__int128(int64_t(c.r[i.XO.RA])) * int64_t(c.r[i.XO.RB])) >> 64);
    store_xo_no_oe(c, i, uint64_t(v));
}

XF_OP(mulhdux) {
    // RT <- ((RA) × (RB))[0:63], unsigned
    uint64_t v = uint64_t((static_cast<unsigned __int128>(c.r[i.XO.RA]) * c.r[i.XO.RB]) >> 64);
    store_xo_no_oe(c, i, v);
}

XF_OP(divwx) {
    // RT[32:63] <- (RA)[32:63] ÷ (RB)[32:63], RT[0:31] <- 0 (as Xenia)
    int32_t a = int32_t(c.r[i.XO.RA]), b = int32_t(c.r[i.XO.RB]);
    bool overflow = b == 0 || (a == std::numeric_limits<int32_t>::min() && b == -1);
    store_xo(c, i, overflow ? 0 : uint64_t(uint32_t(a / b)), overflow);
}

XF_OP(divwux) {
    // RT[32:63] <- (RA)[32:63] ÷ (RB)[32:63] unsigned, RT[0:31] <- 0
    uint32_t a = uint32_t(c.r[i.XO.RA]), b = uint32_t(c.r[i.XO.RB]);
    store_xo(c, i, b == 0 ? 0 : uint64_t(a / b), b == 0);
}

XF_OP(divdx) {
    // RT <- (RA) ÷ (RB)
    int64_t a = int64_t(c.r[i.XO.RA]), b = int64_t(c.r[i.XO.RB]);
    bool overflow = b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1);
    store_xo(c, i, overflow ? 0 : uint64_t(a / b), overflow);
}

XF_OP(divdux) {
    // RT <- (RA) ÷ (RB) unsigned
    uint64_t a = c.r[i.XO.RA], b = c.r[i.XO.RB];
    store_xo(c, i, b == 0 ? 0 : a / b, b == 0);
}

// ---- compare: BF = RT >> 2, L = RT & 1 (64-bit operands) ----

XF_OP(cmp) {
    uint64_t a = c.r[i.X.RA], b = c.r[i.X.RB];
    if (i.X.RT & 1) compare(c, i.X.RT >> 2, int64_t(a), int64_t(b));
    else compare(c, i.X.RT >> 2, int32_t(a), int32_t(b));
}

XF_OP(cmpi) {
    uint64_t a = c.r[i.D.RA];
    if (i.D.RT & 1) compare(c, i.D.RT >> 2, int64_t(a), exts16(i.D.DS));
    else compare(c, i.D.RT >> 2, int32_t(a), int32_t(exts16(i.D.DS)));
}

XF_OP(cmpl) {
    uint64_t a = c.r[i.X.RA], b = c.r[i.X.RB];
    if (i.X.RT & 1) compare(c, i.X.RT >> 2, a, b);
    else compare(c, i.X.RT >> 2, uint32_t(a), uint32_t(b));
}

XF_OP(cmpli) {
    uint64_t a = c.r[i.D.RA];
    if (i.D.RT & 1) compare(c, i.D.RT >> 2, a, extz16(i.D.DS));
    else compare(c, i.D.RT >> 2, uint32_t(a), uint32_t(i.D.DS));
}

// ---- logic: RS is the RT field, the result goes to RA ----

XF_OP(andx) { store_x(c, i, c.r[i.X.RT] & c.r[i.X.RB]); }
XF_OP(andcx) { store_x(c, i, c.r[i.X.RT] & ~c.r[i.X.RB]); }
XF_OP(eqvx) { store_x(c, i, ~(c.r[i.X.RT] ^ c.r[i.X.RB])); }
XF_OP(nandx) { store_x(c, i, ~(c.r[i.X.RT] & c.r[i.X.RB])); }
XF_OP(norx) { store_x(c, i, ~(c.r[i.X.RT] | c.r[i.X.RB])); }
// or rx,rx,rx is a no-op (or a priority hint, 0x7FFFFB78 "db16cyc" in
// spin loops): the store below changes nothing
XF_OP(orx) { store_x(c, i, c.r[i.X.RT] | c.r[i.X.RB]); }
XF_OP(orcx) { store_x(c, i, c.r[i.X.RT] | ~c.r[i.X.RB]); }
XF_OP(xorx) { store_x(c, i, c.r[i.X.RT] ^ c.r[i.X.RB]); }

XF_OP(andix) {
    // RA <- (RS) & (i48.0 || UI), CR0
    uint64_t v = c.r[i.D.RT] & extz16(i.D.DS);
    c.r[i.D.RA] = v;
    update_cr0(c, v);
}

XF_OP(andisx) {
    // RA <- (RS) & (i32.0 || UI || i16.0), CR0
    uint64_t v = c.r[i.D.RT] & (extz16(i.D.DS) << 16);
    c.r[i.D.RA] = v;
    update_cr0(c, v);
}

XF_OP(ori) { c.r[i.D.RA] = c.r[i.D.RT] | extz16(i.D.DS); }
XF_OP(oris) { c.r[i.D.RA] = c.r[i.D.RT] | (extz16(i.D.DS) << 16); }
XF_OP(xori) { c.r[i.D.RA] = c.r[i.D.RT] ^ extz16(i.D.DS); }
XF_OP(xoris) { c.r[i.D.RA] = c.r[i.D.RT] ^ (extz16(i.D.DS) << 16); }

XF_OP(cntlzwx) { store_x(c, i, uint64_t(std::countl_zero(uint32_t(c.r[i.X.RT])))); }
XF_OP(cntlzdx) { store_x(c, i, uint64_t(std::countl_zero(c.r[i.X.RT]))); }
XF_OP(extsbx) { store_x(c, i, uint64_t(int64_t(int8_t(c.r[i.X.RT])))); }
XF_OP(extshx) { store_x(c, i, uint64_t(int64_t(int16_t(c.r[i.X.RT])))); }
XF_OP(extswx) { store_x(c, i, uint64_t(int64_t(int32_t(c.r[i.X.RT])))); }

// ---- rotate: mb/me and sh are 6-bit with their top bit apart (MB5, SH5) ----

XF_OP(rldclx) {
    // RA <- ROTL64((RS), (RB)[58:63]) & MASK(mb, 63)
    uint64_t v = std::rotl(c.r[i.MDS.RT], int(c.r[i.MDS.RB] & 0x3F));
    store_md(c, i, v & mask((i.MDS.MB5 << 5) | i.MDS.MB, 63));
}

XF_OP(rldcrx) {
    // RA <- ROTL64((RS), (RB)[58:63]) & MASK(0, me)
    uint64_t v = std::rotl(c.r[i.MDS.RT], int(c.r[i.MDS.RB] & 0x3F));
    store_md(c, i, v & mask(0, (i.MDS.MB5 << 5) | i.MDS.MB));
}

XF_OP(rldicx) {
    // RA <- ROTL64((RS), sh) & MASK(mb, ¬sh)
    uint32_t sh = (i.MD.SH5 << 5) | i.MD.SH;
    store_md(c, i, std::rotl(c.r[i.MD.RT], int(sh)) & mask((i.MD.MB5 << 5) | i.MD.MB, ~sh));
}

XF_OP(rldiclx) {
    // RA <- ROTL64((RS), sh) & MASK(mb, 63)
    uint32_t sh = (i.MD.SH5 << 5) | i.MD.SH;
    store_md(c, i, std::rotl(c.r[i.MD.RT], int(sh)) & mask((i.MD.MB5 << 5) | i.MD.MB, 63));
}

XF_OP(rldicrx) {
    // RA <- ROTL64((RS), sh) & MASK(0, me)
    uint32_t sh = (i.MD.SH5 << 5) | i.MD.SH;
    store_md(c, i, std::rotl(c.r[i.MD.RT], int(sh)) & mask(0, (i.MD.MB5 << 5) | i.MD.MB));
}

XF_OP(rldimix) {
    // RA <- ROTL64((RS), sh) & m | (RA) & ¬m, m = MASK(mb, ¬sh)
    uint32_t sh = (i.MD.SH5 << 5) | i.MD.SH;
    uint64_t m = mask((i.MD.MB5 << 5) | i.MD.MB, ~sh);
    store_md(c, i, (std::rotl(c.r[i.MD.RT], int(sh)) & m) | (c.r[i.MD.RA] & ~m));
}

XF_OP(rlwinmx) {
    // RA <- ROTL32((RS)[32:63], SH) & MASK(MB+32, ME+32)
    store_m(c, i, rotl32_both(c.r[i.M.RT], i.M.SH) & mask(i.M.MB + 32, i.M.ME + 32));
}

XF_OP(rlwnmx) {
    // RA <- ROTL32((RS)[32:63], (RB)[59:63]) & MASK(MB+32, ME+32); RB is the SH field
    store_m(c, i, rotl32_both(c.r[i.M.RT], uint32_t(c.r[i.M.SH])) & mask(i.M.MB + 32, i.M.ME + 32));
}

XF_OP(rlwimix) {
    // RA <- ROTL32((RS)[32:63], SH) & m | (RA) & ¬m, m = MASK(MB+32, ME+32)
    uint64_t m = mask(i.M.MB + 32, i.M.ME + 32);
    store_m(c, i, (rotl32_both(c.r[i.M.RT], i.M.SH) & m) | (c.r[i.M.RA] & ~m));
}

// ---- shift: amounts of the register's width and up give 0 (or the sign) ----

XF_OP(slwx) {
    // RA <- (RS)[32:63] << (RB)[58:63], zero-extended
    uint32_t n = uint32_t(c.r[i.X.RB]) & 0x3F;
    store_x(c, i, n & 0x20 ? 0 : uint64_t(uint32_t(c.r[i.X.RT]) << n));
}

XF_OP(srwx) {
    // RA <- (RS)[32:63] >> (RB)[58:63], zero-extended
    uint32_t n = uint32_t(c.r[i.X.RB]) & 0x3F;
    store_x(c, i, n & 0x20 ? 0 : uint64_t(uint32_t(c.r[i.X.RT]) >> n));
}

XF_OP(sldx) {
    // RA <- (RS) << (RB)[57:63]
    uint32_t n = uint32_t(c.r[i.X.RB]) & 0x7F;
    store_x(c, i, n & 0x40 ? 0 : c.r[i.X.RT] << n);
}

XF_OP(srdx) {
    // RA <- (RS) >> (RB)[57:63]
    uint32_t n = uint32_t(c.r[i.X.RB]) & 0x7F;
    store_x(c, i, n & 0x40 ? 0 : c.r[i.X.RT] >> n);
}

XF_OP(srawx) { shift_right_algebraic_word(c, i, uint32_t(c.r[i.X.RB]) & 0x3F); }
XF_OP(srawix) { shift_right_algebraic_word(c, i, i.X.RB); } // SH is the RB field
XF_OP(sradx) { shift_right_algebraic_double(c, i, uint32_t(c.r[i.X.RB]) & 0x7F); }
XF_OP(sradix) { shift_right_algebraic_double(c, i, (i.XS.SH5 << 5) | i.XS.SH); }

// ---- memory barriers (ppc_emit_memory.cc): sync and eieio are a full
// fence (Xenia's mfence), isync nothing ----

XF_OP(sync) { std::atomic_thread_fence(std::memory_order_seq_cst); }
XF_OP(eieio) { std::atomic_thread_fence(std::memory_order_seq_cst); }
XF_OP(isync) {}

} // namespace xf::ppc::ops
