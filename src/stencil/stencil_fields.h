// What the copy-and-patch tier's stencils know of an instruction word
// (docs/recompiler.md, "Stencils that know their operands"), shared by
// src/stencil/stencils.cpp, compiled into the stencils, and
// src/stencil_jit.cpp, which fills their holes, so the two always agree.
//
// - Register fields as holes. A stencil gets the word through xf_hole_word
//   and each of its form's register numbers through a hole of its own
//   (xf_hole_f0..f4), written back into the word with the same bitfields
//   (ppc.h's Instr) the semantics read them with: the compiler then reads a
//   register number as the hole's value instead of shifting and masking the
//   word (and for VMX128's numbers split across the word, instead of putting
//   three pieces together). The tier fills each hole with the field's value
//   in the real word, so the word the semantics see is the real one.
// - Variants. A stencil may also be compiled with some of the word's bits
//   constant (Variant::mask and bits) and some field's value known or known
//   not to be some values (Variant::ra); the tier copies it only for a word
//   that has those bits and that value, so it is exact by construction. The
//   compiler folds what the constant bits decide: an Rc of 0 drops the CR0
//   update, a vsldoi's shift becomes one shuffle, an RA of 0, 1 or 13 drops
//   the (RA|0) branch and, for r1 and r13 (the stack and the thread's block),
//   the MMIO check.
#pragma once

#include "xenoforge/ppc.h"

namespace xf::stencil {

using ppc::Form;
using ppc::Instr;

#if defined(__clang__) || defined(__GNUC__)
#define XF_FIELDS_INLINE inline __attribute__((always_inline))
#else
#define XF_FIELDS_INLINE inline
#endif

constexpr int MAX_FIELDS = 5;

// How many register fields a form's stencils take as holes, and how wide each
// value is (in bits; the VMX128 forms' vector registers are 7, put together
// from three places).
constexpr int field_count(Form f) {
    switch (f) {
    case Form::D:
    case Form::DS:
    case Form::XS:
    case Form::MD:
    case Form::XFX: return 2;
    case Form::X:
    case Form::XO:
    case Form::XL:
    case Form::MDS:
    case Form::VX:
    case Form::VC:
    case Form::VX128:
    case Form::VX128_1:
    case Form::VX128_2:
    case Form::VX128_5:
    case Form::VX128_R: return 3;
    case Form::A:
    case Form::VA: return 4;
    case Form::M: return 5;
    case Form::VX128_3:
    case Form::VX128_4:
    case Form::VX128_P: return 2;
    default: return 0;
    }
}
constexpr uint32_t field_width(Form f, int k) {
    switch (f) {
    case Form::XFX: return k == 1 ? 10 : 5; // spr's ten bits as the word has them
    case Form::VX128:
    case Form::VX128_2:
    case Form::VX128_5:
    case Form::VX128_R: return 7;
    case Form::VX128_1: return k == 0 ? 7 : 5;
    case Form::VX128_3:
    case Form::VX128_4:
    case Form::VX128_P: return 7;
    default: return 5;
    }
}

// The k-th register field of a word of form f, and the same written into one.
// Field 1 is RA wherever the form has one (D, DS, X, XO, XS, M, MD, MDS,
// VX128_1).
constexpr uint32_t get_field(Form f, int k, Instr i) {
    switch (f) {
    case Form::D: return k == 0 ? i.D.RT : i.D.RA;
    case Form::DS: return k == 0 ? i.DS.RT : i.DS.RA;
    case Form::X: return k == 0 ? i.X.RT : k == 1 ? i.X.RA : i.X.RB;
    case Form::XO: return k == 0 ? i.XO.RT : k == 1 ? i.XO.RA : i.XO.RB;
    case Form::XS: return k == 0 ? i.XS.RT : i.XS.RA;
    case Form::XL: return k == 0 ? i.XL.BO : k == 1 ? i.XL.BI : i.XL.BB;
    case Form::XFX: return k == 0 ? i.XFX.RT : i.XFX.spr;
    case Form::A: return k == 0 ? i.A.FRT : k == 1 ? i.A.FRA : k == 2 ? i.A.FRB : i.A.FRC;
    case Form::M: return k == 0 ? i.M.RT : k == 1 ? i.M.RA : k == 2 ? i.M.SH : k == 3 ? i.M.MB : i.M.ME;
    case Form::MD: return k == 0 ? i.MD.RT : i.MD.RA;
    case Form::MDS: return k == 0 ? i.MDS.RT : k == 1 ? i.MDS.RA : i.MDS.RB;
    case Form::VX: return k == 0 ? i.VX.VD : k == 1 ? i.VX.VA : i.VX.VB;
    case Form::VC: return k == 0 ? i.VXR.VD : k == 1 ? i.VXR.VA : i.VXR.VB;
    case Form::VA: return k == 0 ? i.VXA.VD : k == 1 ? i.VXA.VA : k == 2 ? i.VXA.VB : i.VXA.VC;
    case Form::VX128:
    case Form::VX128_2:
    case Form::VX128_5:
    case Form::VX128_R:
        // (the four forms put VD128, VA128 and VB128 in the same places)
        return k == 0   ? i.VX128.VD128l | i.VX128.VD128h << 5
               : k == 1 ? i.VX128.VA128l | i.VX128.VA128h << 5 | i.VX128.VA128H << 6
                        : i.VX128.VB128l | i.VX128.VB128h << 5;
    case Form::VX128_1: return k == 0 ? i.VX128_1.VD128l | i.VX128_1.VD128h << 5 : k == 1 ? i.VX128_1.RA : i.VX128_1.RB;
    case Form::VX128_3:
    case Form::VX128_4:
    case Form::VX128_P:
        return k == 0 ? i.VX128_3.VD128l | i.VX128_3.VD128h << 5 : i.VX128_3.VB128l | i.VX128_3.VB128h << 5;
    default: return 0;
    }
}

XF_FIELDS_INLINE void set_field(Form f, int k, Instr &i, uint32_t v) {
    switch (f) {
    case Form::D: k == 0 ? (void)(i.D.RT = v) : (void)(i.D.RA = v); return;
    case Form::DS: k == 0 ? (void)(i.DS.RT = v) : (void)(i.DS.RA = v); return;
    case Form::X: k == 0 ? (void)(i.X.RT = v) : k == 1 ? (void)(i.X.RA = v) : (void)(i.X.RB = v); return;
    case Form::XO: k == 0 ? (void)(i.XO.RT = v) : k == 1 ? (void)(i.XO.RA = v) : (void)(i.XO.RB = v); return;
    case Form::XS: k == 0 ? (void)(i.XS.RT = v) : (void)(i.XS.RA = v); return;
    case Form::XL: k == 0 ? (void)(i.XL.BO = v) : k == 1 ? (void)(i.XL.BI = v) : (void)(i.XL.BB = v); return;
    case Form::XFX: k == 0 ? (void)(i.XFX.RT = v) : (void)(i.XFX.spr = v); return;
    case Form::A:
        k == 0   ? (void)(i.A.FRT = v)
        : k == 1 ? (void)(i.A.FRA = v)
        : k == 2 ? (void)(i.A.FRB = v)
                 : (void)(i.A.FRC = v);
        return;
    case Form::M:
        k == 0   ? (void)(i.M.RT = v)
        : k == 1 ? (void)(i.M.RA = v)
        : k == 2 ? (void)(i.M.SH = v)
        : k == 3 ? (void)(i.M.MB = v)
                 : (void)(i.M.ME = v);
        return;
    case Form::MD: k == 0 ? (void)(i.MD.RT = v) : (void)(i.MD.RA = v); return;
    case Form::MDS: k == 0 ? (void)(i.MDS.RT = v) : k == 1 ? (void)(i.MDS.RA = v) : (void)(i.MDS.RB = v); return;
    case Form::VX: k == 0 ? (void)(i.VX.VD = v) : k == 1 ? (void)(i.VX.VA = v) : (void)(i.VX.VB = v); return;
    case Form::VC: k == 0 ? (void)(i.VXR.VD = v) : k == 1 ? (void)(i.VXR.VA = v) : (void)(i.VXR.VB = v); return;
    case Form::VA:
        k == 0   ? (void)(i.VXA.VD = v)
        : k == 1 ? (void)(i.VXA.VA = v)
        : k == 2 ? (void)(i.VXA.VB = v)
                 : (void)(i.VXA.VC = v);
        return;
    case Form::VX128:
    case Form::VX128_2:
    case Form::VX128_5:
    case Form::VX128_R:
        if (k == 0) {
            i.VX128.VD128l = v & 31, i.VX128.VD128h = v >> 5;
        } else if (k == 1) {
            i.VX128.VA128l = v & 31, i.VX128.VA128h = (v >> 5) & 1, i.VX128.VA128H = v >> 6;
        } else {
            i.VX128.VB128l = v & 31, i.VX128.VB128h = v >> 5;
        }
        return;
    case Form::VX128_1:
        if (k == 0)
            i.VX128_1.VD128l = v & 31, i.VX128_1.VD128h = v >> 5;
        else
            k == 1 ? (void)(i.VX128_1.RA = v) : (void)(i.VX128_1.RB = v);
        return;
    case Form::VX128_3:
    case Form::VX128_4:
    case Form::VX128_P:
        if (k == 0)
            i.VX128_3.VD128l = v & 31, i.VX128_3.VD128h = v >> 5;
        else
            i.VX128_3.VB128l = v & 31, i.VX128_3.VB128h = v >> 5;
        return;
    default: return;
    }
}

// What a variant knows of RA (field 1): nothing, its value, or that it is none
// of 0, 1 and 13 (Other: the D forms' last case) or not 0 (NonZero: the X
// forms' second).
enum class Ra : int32_t { Any = -1, Other = -2, NonZero = -3 };
constexpr Ra ra_is(uint32_t v) { return Ra(int32_t(v)); }

// What a variant knows of the X form's three registers: nothing; RB is RS and
// RA is not (Move: `mr`, an or that copies a register); or the three are not
// all the same (Distinct: an or that is no priority hint).
enum class Regs : uint8_t { Any, Move, Distinct };

struct Variant {
    uint32_t mask = 0, bits = 0; // the word's bits it has constant
    Ra ra = Ra::Any;
    Regs regs = Regs::Any;
};

constexpr bool matches(const Variant &v, Instr i) {
    if ((i.code & v.mask) != v.bits) return false;
    if (v.regs == Regs::Move && !(i.X.RB == i.X.RT && i.X.RA != i.X.RT)) return false;
    if (v.regs == Regs::Distinct && i.X.RT == i.X.RA && i.X.RA == i.X.RB) return false;
    const uint32_t ra = get_field(ppc::Form::D, 1, i); // RA is bits 16-20 in every form that has one
    switch (v.ra) {
    case Ra::Any: return true;
    case Ra::Other: return ra != 0 && ra != 1 && ra != 13;
    case Ra::NonZero: return ra != 0;
    default: return ra == uint32_t(v.ra);
    }
}

// The Rc bit (and XO's OE) where a form has one, for the variants with them 0.
constexpr uint32_t rc_mask(Form f) {
    switch (f) {
    case Form::X:
    case Form::A:
    case Form::M:
    case Form::MD:
    case Form::MDS:
    case Form::XS: return 1u;
    case Form::XO: return 1u | 1u << 10;
    case Form::VC: return 1u << 10;
    case Form::VX128_R: return 1u << 6;
    default: return 0;
    }
}

// The variants (each a list of V(opcode name, form)):
// - D and DS loads and stores (not the update forms) and addi, addis: RA 0,
//   1, 13 or another (_ra0, _ra1, _ra13, _ran): together they cover every word.
#define XF_STENCIL_RA4(V)                                                                                              \
    V(addi, D)                                                                                                         \
    V(addis, D)                                                                                                        \
    V(lbz, D)                                                                                                          \
    V(ld, DS) V(lfd, D) V(lfs, D) V(lha, D) V(lhz, D) V(lmw, D) V(lwa, DS) V(lwz, D) V(stb, D) V(std, DS) V(stfd, D)   \
        V(stfs, D) V(sth, D) V(stmw, D) V(stw, D)
// - X and VX128_1 loads and stores with (RA|0) (not the update forms): RA 0
//   or not (_ra0, _ran).
#define XF_STENCIL_RA2(V)                                                                                              \
    V(dcbf, X)                                                                                                         \
    V(dcbst, X)                                                                                                        \
    V(dcbt, X)                                                                                                         \
    V(dcbtst, X) V(icbi, X) V(lbzx, X) V(ldarx, X) V(ldbrx, X) V(ldx, X) V(lfdx, X) V(lfsx, X) V(lhax, X) V(lhbrx, X)  \
        V(lhzx, X) V(lvebx, X) V(lvehx, X) V(lvewx, X) V(lvewx128, VX128_1) V(lvlx, X) V(lvlx128, VX128_1) V(lvlxl, X) \
            V(lvlxl128, VX128_1) V(lvrx, X) V(lvrx128, VX128_1) V(lvrxl, X) V(lvrxl128, VX128_1) V(lvx, X)             \
                V(lvx128, VX128_1) V(lvxl, X) V(lvxl128, VX128_1) V(lwarx, X) V(lwax, X) V(lwbrx, X) V(lwzx, X)        \
                    V(stbx, X) V(stdbrx, X) V(stdcx, X) V(stdx, X) V(stfdx, X) V(stfiwx, X) V(stfsx, X) V(sthbrx, X)   \
                        V(sthx, X) V(stvebx, X) V(stvehx, X) V(stvewx, X) V(stvewx128, VX128_1) V(stvlx, X)            \
                            V(stvlx128, VX128_1) V(stvlxl, X) V(stvlxl128, VX128_1) V(stvrx, X) V(stvrx128, VX128_1)   \
                                V(stvrxl, X) V(stvrxl128, VX128_1) V(stvx, X) V(stvx128, VX128_1) V(stvxl, X)          \
                                    V(stvxl128, VX128_1) V(stwbrx, X) V(stwcx, X) V(stwx, X)
// - vsldoi and vsldoi128: each of the 16 shifts (_sh0.._sh15; bits 6-9).
#define XF_STENCIL_SH16(V) V(vsldoi, VA) V(vsldoi128, VX128_5)

// - mfspr and mtspr: the special registers the games move most (_spr1 XER,
//   _spr8 LR, _spr9 CTR, _spr256 VRSAVE), the SPR's switch folded away.
#define XF_STENCIL_SPR(V) V(mfspr, XFX) V(mtspr, XFX)
#define XF_STENCIL_SPRS(S) S(1) S(8) S(9) S(256)
// - orx: `mr` (_mr: RB is RS, RA another, Rc 0) and an or that is no
//   priority hint (_or: the three not all the same, Rc 0).
#define XF_STENCIL_OR(V) V(orx, X)

constexpr Variant RA4_VARIANTS[4] = {{0, 0, ra_is(0)}, {0, 0, ra_is(1)}, {0, 0, ra_is(13)}, {0, 0, Ra::Other}};
constexpr Variant RA2_VARIANTS[2] = {{0, 0, ra_is(0)}, {0, 0, Ra::NonZero}};
constexpr Variant sh_variant(uint32_t sh) { return {0xFu << 6, sh << 6, Ra::Any}; }
constexpr Variant rc0_variant(Form f) { return {rc_mask(f), 0, Ra::Any}; }
// an SPR's number as XFX's spr field has it (its two halves swapped), in place
constexpr uint32_t spr_bits(uint32_t spr) { return (((spr & 31) << 5) | (spr >> 5)) << 11; }
constexpr Variant spr_variant(uint32_t spr) { return {0x3FFu << 11, spr_bits(spr), Ra::Any}; }
constexpr Variant OR_VARIANTS[2] = {{1, 0, Ra::Any, Regs::Move}, {1, 0, Ra::Any, Regs::Distinct}};

// The compare-and-branch superinstructions: cmp, cmpi, cmpl or cmpli, then a
// bc on one of the bits it set (the next instruction, reached only from it),
// taken when the bit is clear (BO 4-7) or set (BO 12-15).
#define XF_STENCIL_CMP(V) V(cmp, X) V(cmpi, D) V(cmpl, X) V(cmpli, D)

} // namespace xf::stencil
