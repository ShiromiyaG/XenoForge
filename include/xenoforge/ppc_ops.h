// What each instruction does, once, for both the generated code and the
// interpreter (docs/recompiler.md, "One description per instruction"):
//
//   ops::run<Opcode::addx>(c, Instr{code});
//
// The generated code passes the word as a constant and the compiler folds
// the fields away; the interpreter passes the word it fetched. Translated
// from Xenia's emitters (src/xenia/cpu/ppc/ppc_emit_*.cc) and what its
// backend does with them (BSD; see LICENSE-xenia). Branches are not here:
// the emitter and the interpreter do them, with the conditions below
// (control.h).
//
// Groups, a header each under ppc_ops/: integer, control (branch
// conditions, CR, SPRs, traps, sync), memory (every load and store,
// reservations, cache lines), fpu, vmx (integer and permutes), vmx_float.
// An instruction is defined with XF_OP(name) { ... }, with `c` the context
// and `i` the word (i.XO.RT, as Xenia's InstrData); one not defined runs
// the primary template, which reports it.

#pragma once

#include "xenoforge/ppc.h"
#include "xenoforge/ppc_context.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__clang__) || defined(__GNUC__)
#define XF_INLINE inline __attribute__((always_inline))
#else
#define XF_INLINE __forceinline
#endif

namespace xf::ppc::ops {

template <Opcode> constexpr bool implemented = false;

template <Opcode op> XF_INLINE void run(Context &c, Instr i) {
    if (c.system && c.system->unimplemented) c.system->unimplemented(c, op, i.code);
}

#define XF_OP(name)                                                                                                    \
    template <> constexpr bool implemented<Opcode::name> = true;                                                       \
    template <> XF_INLINE void run<Opcode::name>([[maybe_unused]] Context & c, [[maybe_unused]] Instr i)

// ---- the instruction word (ppc_decode_data.h's helpers) ----

constexpr int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
constexpr int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }
constexpr uint64_t extz16(uint32_t v) { return uint64_t(uint16_t(v)); }
// bits mstart..mstop of 64 set, wrapping when mstart > mstop (XEMASK)
constexpr uint64_t mask(uint32_t mstart, uint32_t mstop) {
    mstart &= 0x3F;
    mstop &= 0x3F;
    uint64_t value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
    return mstart <= mstop ? value : ~value;
}
// the 128-register numbers of the VMX128 forms (ppc_emit_altivec.cc's macros)
#define VX128_VD128 (i.VX128.VD128l | (i.VX128.VD128h << 5))
#define VX128_VA128 (i.VX128.VA128l | (i.VX128.VA128h << 5) | (i.VX128.VA128H << 6))
#define VX128_VB128 (i.VX128.VB128l | (i.VX128.VB128h << 5))
#define VX128_1_VD128 (i.VX128_1.VD128l | (i.VX128_1.VD128h << 5))
#define VX128_2_VD128 (i.VX128_2.VD128l | (i.VX128_2.VD128h << 5))
#define VX128_2_VA128 (i.VX128_2.VA128l | (i.VX128_2.VA128h << 5) | (i.VX128_2.VA128H << 6))
#define VX128_2_VB128 (i.VX128_2.VB128l | (i.VX128_2.VB128h << 5))
#define VX128_2_VC (i.VX128_2.VC)
#define VX128_3_VD128 (i.VX128_3.VD128l | (i.VX128_3.VD128h << 5))
#define VX128_3_VB128 (i.VX128_3.VB128l | (i.VX128_3.VB128h << 5))
#define VX128_3_IMM (i.VX128_3.IMM)
#define VX128_5_VD128 (i.VX128_5.VD128l | (i.VX128_5.VD128h << 5))
#define VX128_5_VA128 ((i.VX128_5.VA128l | (i.VX128_5.VA128h << 5)) | (i.VX128_5.VA128H << 6))
#define VX128_5_VB128 (i.VX128_5.VB128l | (i.VX128_5.VB128h << 5))
#define VX128_5_SH (i.VX128_5.SH)
#define VX128_R_VD128 (i.VX128_R.VD128l | (i.VX128_R.VD128h << 5))
#define VX128_R_VA128 (i.VX128_R.VA128l | (i.VX128_R.VA128h << 5) | (i.VX128_R.VA128H << 6))
#define VX128_R_VB128 (i.VX128_R.VB128l | (i.VX128_R.VB128h << 5))

// ---- memory: big-endian, guest address added to the base ----

XF_INLINE uint8_t *host(Context &c, uint32_t ea) { return c.base + ea; }
XF_INLINE uint8_t load8(Context &c, uint32_t ea) { return *host(c, ea); }
XF_INLINE uint16_t load16(Context &c, uint32_t ea) {
    uint16_t v;
    std::memcpy(&v, host(c, ea), 2);
    return __builtin_bswap16(v);
}
XF_INLINE uint32_t load32(Context &c, uint32_t ea) {
    uint32_t v;
    std::memcpy(&v, host(c, ea), 4);
    return __builtin_bswap32(v);
}
XF_INLINE uint64_t load64(Context &c, uint32_t ea) {
    uint64_t v;
    std::memcpy(&v, host(c, ea), 8);
    return __builtin_bswap64(v);
}
XF_INLINE void store8(Context &c, uint32_t ea, uint8_t v) { *host(c, ea) = v; }
XF_INLINE void store16(Context &c, uint32_t ea, uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(host(c, ea), &v, 2);
}
XF_INLINE void store32(Context &c, uint32_t ea, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(host(c, ea), &v, 4);
}
XF_INLINE void store64(Context &c, uint32_t ea, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(host(c, ea), &v, 8);
}

// ---- the flags (ppc_hir_builder.cc) ----

// UpdateCR: the result's low 32 bits against 0 (or two values), signed or
// not; SO is XER's
XF_INLINE void update_cr(Context &c, uint32_t n, int32_t lhs, int32_t rhs = 0) {
    c.cr[4 * n + 0] = lhs < rhs;
    c.cr[4 * n + 1] = lhs > rhs;
    c.cr[4 * n + 2] = lhs == rhs;
    c.cr[4 * n + 3] = c.xer_so;
}
XF_INLINE void update_cr_unsigned(Context &c, uint32_t n, uint32_t lhs, uint32_t rhs) {
    c.cr[4 * n + 0] = lhs < rhs;
    c.cr[4 * n + 1] = lhs > rhs;
    c.cr[4 * n + 2] = lhs == rhs;
    c.cr[4 * n + 3] = c.xer_so;
}
XF_INLINE void update_cr0(Context &c, uint64_t result) { update_cr(c, 0, int32_t(uint32_t(result))); }
// UpdateCR6: all elements true (bit 0), none (bit 2)
XF_INLINE void update_cr6(Context &c, const Vec128 &v) {
    bool all = (v.u32[0] & v.u32[1] & v.u32[2] & v.u32[3]) == 0xFFFFFFFFu;
    bool none = (v.u32[0] | v.u32[1] | v.u32[2] | v.u32[3]) == 0;
    c.cr[24] = all, c.cr[25] = 0, c.cr[26] = none, c.cr[27] = 0;
}
// CopyFPSCRToCR1
XF_INLINE void fpscr_to_cr1(Context &c) {
    c.cr[4] = (c.fpscr >> 31) & 1, c.cr[5] = (c.fpscr >> 30) & 1, c.cr[6] = (c.fpscr >> 29) & 1,
    c.cr[7] = (c.fpscr >> 28) & 1;
}
// an overflow (OE): OV, and SO sticky
XF_INLINE void set_ov(Context &c, bool overflow) {
    c.xer_ov = overflow;
    c.xer_so |= overflow;
}

} // namespace xf::ppc::ops

#include "xenoforge/ppc_ops/integer.h"
#include "xenoforge/ppc_ops/control.h"
#include "xenoforge/ppc_ops/memory.h"
#include "xenoforge/ppc_ops/fpu.h"
#include "xenoforge/ppc_ops/vmx.h"
#include "xenoforge/ppc_ops/vmx_float.h"
