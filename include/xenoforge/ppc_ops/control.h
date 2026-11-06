// Control (ppc_ops.h): the branch conditions the emitter and the
// interpreter test, CR logic, SPRs, MSR, VSCR, traps and the system call.
// Translated from Xenia's ppc_emit_control.cc and, for mfvscr/mtvscr,
// ppc_emit_altivec.cc (BSD; see LICENSE-xenia).
//
// Where this differs from Xenia, it follows the hardware: XER is SO, OV and
// CA (Xenia's has only CA), so mfxer/mtxer/mcrxr see all three; VSCR is
// built from vscr_sat with NJ always set (the vector unit runs in non-Java
// mode; Xenia keeps a separate copy of VSCR whose SAT the saturating
// instructions never update). VRSAVE reads 0 and ignores writes.

#pragma once

#include "xenoforge/ppc_ops.h"

namespace xf::ppc::ops {

// bcx and bclrx: CTR counts down unless BO says not (BO's bits are counted
// from the right here, as Xenia's select_bits: 4 ignore the condition, 3
// the value expected, 2 leave CTR alone, 1 branch when CTR reaches 0), CTR
// tested on its low 32 bits
XF_INLINE bool branch_condition(Context &c, uint32_t bo, uint32_t bi) {
    bool ctr_ok = true;
    if (!(bo & 0x04)) {
        c.ctr -= 1;
        ctr_ok = (uint32_t(c.ctr) == 0) == bool(bo & 0x02);
    }
    bool cond_ok = (bo & 0x10) || bool(c.cr[bi]) == bool(bo & 0x08);
    return ctr_ok && cond_ok;
}

// bcctrx: the condition alone
XF_INLINE bool branch_condition_ctr(Context &c, uint32_t bo, uint32_t bi) {
    return (bo & 0x10) || bool(c.cr[bi]) == bool(bo & 0x08);
}

// ---- helpers ----

// An instruction the system has to see: an SPR not modelled
inline void report_unimplemented(Context &c, Opcode op, Instr i) {
    if (c.system && c.system->unimplemented) c.system->unimplemented(c, op, i.code);
}

// The XFX form's SPR number, its two halves swapped back
XF_INLINE uint32_t spr_number(Instr i) { return ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F); }

XF_INLINE uint64_t time_base(Context &c) { return c.system && c.system->time_base ? c.system->time_base(c) : 0; }

// XER as the register: SO, OV, CA in bits 31, 30, 29 and the byte count of
// lswx/stswx in bits 6-0 (57-63 of the architecture's 64); the rest reads 0
XF_INLINE uint64_t xer_word(const Context &c) {
    return (uint64_t(c.xer_so & 1) << 31) | (uint64_t(c.xer_ov & 1) << 30) | (uint64_t(c.xer_ca & 1) << 29) |
           (c.xer_bc & 0x7F);
}
XF_INLINE void set_xer_word(Context &c, uint64_t v) {
    c.xer_so = (v >> 31) & 1;
    c.xer_ov = (v >> 30) & 1;
    c.xer_ca = (v >> 29) & 1;
    c.xer_bc = v & 0x7F;
}

// CR field n from bits 4*(7-n)+3 .. 4*(7-n) of v (StoreCR(n, v))
XF_INLINE void store_cr_field(Context &c, uint32_t n, uint64_t v) {
    uint32_t shift = 4 * (7 - n);
    c.cr[4 * n + 0] = (v >> (shift + 3)) & 1;
    c.cr[4 * n + 1] = (v >> (shift + 2)) & 1;
    c.cr[4 * n + 2] = (v >> (shift + 1)) & 1;
    c.cr[4 * n + 3] = (v >> shift) & 1;
}

// CR field n alone, in its place in the word (LoadCR(n))
XF_INLINE uint64_t load_cr_field(const Context &c, uint32_t n) {
    uint32_t shift = 4 * (7 - n);
    return (uint64_t(c.cr[4 * n + 0] & 1) << (shift + 3)) | (uint64_t(c.cr[4 * n + 1] & 1) << (shift + 2)) |
           (uint64_t(c.cr[4 * n + 2] & 1) << (shift + 1)) | (uint64_t(c.cr[4 * n + 3] & 1) << shift);
}

// mfocrf/mtocrf name one field in FXM; Xenia takes any other count as
// invalid. Returns the field, or -1.
XF_INLINE int single_cr_field(uint32_t fxm) { return lib::popcount(fxm) == 1 ? 7 - lib::countr_zero(fxm) : -1; }

// mfcr / mfocrf
inline uint64_t read_cr(const Context &c, Instr i) {
    if (!(i.XFX.spr & (1 << 9))) return c.cr_word();
    int n = single_cr_field((i.XFX.spr & 0x1FF) >> 1);
    return n < 0 ? 0 : load_cr_field(c, uint32_t(n));
}

// mtcrf / mtocrf
inline void write_cr(Context &c, Instr i, uint64_t v) {
    uint32_t fxm = (i.XFX.spr & 0x1FF) >> 1;
    if (i.XFX.spr & (1 << 9)) {
        int n = single_cr_field(fxm);
        if (n >= 0) store_cr_field(c, uint32_t(n), v);
        else lib::memset(c.cr, 0, sizeof(c.cr)); // invalid: Xenia stores zero to CR
        return;
    }
    for (uint32_t b = 0; b < 8; b++)
        if (fxm & (1u << b)) store_cr_field(c, 7 - b, v);
}

// the trap condition: TO's bits (from the left) signed <, signed >, =,
// unsigned <, unsigned >
XF_INLINE bool trap_condition(uint32_t to, int64_t a, int64_t b) {
    return ((to & 0x10) && a < b) || ((to & 0x08) && a > b) || ((to & 0x04) && a == b) ||
           ((to & 0x02) && uint64_t(a) < uint64_t(b)) || ((to & 0x01) && uint64_t(a) > uint64_t(b));
}

inline void raise_trap(Context &c) {
    if (c.system && c.system->trap) c.system->trap(c, c.pc);
}

// ---- condition register logic: bt = BO, ba = BI, bb = BB ----

XF_OP(crand) { c.cr[i.XL.BO] = c.cr[i.XL.BI] & c.cr[i.XL.BB] & 1; }
XF_OP(crandc) { c.cr[i.XL.BO] = c.cr[i.XL.BI] & ~c.cr[i.XL.BB] & 1; }
XF_OP(creqv) { c.cr[i.XL.BO] = ~(c.cr[i.XL.BI] ^ c.cr[i.XL.BB]) & 1; }
XF_OP(crnand) { c.cr[i.XL.BO] = ~(c.cr[i.XL.BI] & c.cr[i.XL.BB]) & 1; }
XF_OP(crnor) { c.cr[i.XL.BO] = ~(c.cr[i.XL.BI] | c.cr[i.XL.BB]) & 1; }
XF_OP(cror) { c.cr[i.XL.BO] = (c.cr[i.XL.BI] | c.cr[i.XL.BB]) & 1; }
XF_OP(crorc) { c.cr[i.XL.BO] = (c.cr[i.XL.BI] | ~c.cr[i.XL.BB]) & 1; }
XF_OP(crxor) { c.cr[i.XL.BO] = (c.cr[i.XL.BI] ^ c.cr[i.XL.BB]) & 1; }

XF_OP(mcrf) {
    // CR field BO >> 2 <- CR field BI >> 2
    uint32_t field;
    lib::memcpy(&field, &c.cr[4 * (i.XL.BI >> 2)], 4);
    lib::memcpy(&c.cr[4 * (i.XL.BO >> 2)], &field, 4);
}

XF_OP(mcrxr) {
    // CR field RT >> 2 <- XER[SO, OV, CA, 0]; those XER bits <- 0
    uint32_t n = i.X.RT >> 2;
    c.cr[4 * n + 0] = c.xer_so, c.cr[4 * n + 1] = c.xer_ov, c.cr[4 * n + 2] = c.xer_ca, c.cr[4 * n + 3] = 0;
    c.xer_so = c.xer_ov = c.xer_ca = 0;
}

XF_OP(mfcr) { c.r[i.XFX.RT] = read_cr(c, i); }
XF_OP(mtcrf) { write_cr(c, i, c.r[i.XFX.RT]); }

// ---- SPRs ----

XF_OP(mfspr) {
    uint64_t v;
    switch (spr_number(i)) {
    case 1: v = xer_word(c); break;
    case 8: v = c.lr; break;
    case 9: v = c.ctr; break;
    case 256: v = 0; break;                     // VRSAVE
    case 268: v = time_base(c); break;          // TB
    case 269: v = time_base(c) >> 32; break;    // TBU
    case 287: v = 0x710700; break;              // PVR: Xenia's default (cvars::pvr)
    default: report_unimplemented(c, Opcode::mfspr, i); return;
    }
    c.r[i.XFX.RT] = v;
}

XF_OP(mtspr) {
    uint64_t v = c.r[i.XFX.RT];
    switch (spr_number(i)) {
    case 1: set_xer_word(c, v); break;
    case 8: c.lr = v; break;
    case 9: c.ctr = v; break;
    case 256: break; // VRSAVE
    default: report_unimplemented(c, Opcode::mtspr, i); break;
    }
}

XF_OP(mftb) {
    // 268 the whole time base, 269 its upper word
    uint64_t tb = time_base(c);
    c.r[i.XFX.RT] = spr_number(i) == 268 ? tb : tb >> 32;
}

// ---- MSR (Xenia keeps it for EE; mtmsrd writes EE alone) ----

XF_OP(mfmsr) { c.r[i.X.RT] = c.msr; }
XF_OP(mtmsr) { c.msr = c.r[i.X.RT]; }
XF_OP(mtmsrd) { c.msr = (c.r[i.X.RT] & 0x8000) | (c.msr & ~uint64_t(0x8000)); }

// ---- VSCR: NJ (bit 16 of the last word) and SAT (bit 0) ----

XF_OP(mfvscr) {
    c.v[i.VX.VD] = Vec128{};
    c.v[i.VX.VD].u32[3] = 0x00010000u | (c.vscr_sat & 1);
}

XF_OP(mtvscr) { c.vscr_sat = c.v[i.VX.VB].u32[3] & 1; }

// ---- traps: TO is the RT field; tw/twi compare the low words, signed ----

XF_OP(td) {
    if (trap_condition(i.X.RT, int64_t(c.r[i.X.RA]), int64_t(c.r[i.X.RB]))) raise_trap(c);
}

XF_OP(tdi) {
    if (trap_condition(i.D.RT, int64_t(c.r[i.D.RA]), exts16(i.D.DS))) raise_trap(c);
}

XF_OP(tw) {
    if (trap_condition(i.X.RT, int32_t(c.r[i.X.RA]), int32_t(c.r[i.X.RB]))) raise_trap(c);
}

XF_OP(twi) {
    // twi 31, r0, n (always taken) is how the system's debug breaks and
    // prints are written: the handler reads n from the word at c.pc
    if (trap_condition(i.D.RT, int32_t(c.r[i.D.RA]), exts16(i.D.DS))) raise_trap(c);
}

// ---- system call (any LEV; Xenia's LEV 2 is its own import thunks) ----

XF_OP(sc) {
    if (c.system && c.system->syscall) c.system->syscall(c);
}

} // namespace xf::ppc::ops
