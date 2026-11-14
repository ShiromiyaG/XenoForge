// What each instruction does, once, for both the generated code and the
// interpreter (docs/recompiler.md, "One description per instruction"):
//
//   ops::run<Opcode::addx>(c, base, Instr{code});
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
// An instruction is defined with XF_OP(name) { ... }, with `c` the context,
// `base` the guest memory's base (c.base, passed beside the context so that
// no access reads it from there) and `i` the word (i.XO.RT, as Xenia's
// InstrData); one not defined runs
// the primary template, which reports it.

#pragma once

#include "xenoforge/ppc.h"
#include "xenoforge/ppc_context.h"
// the standard library's pieces (lib::), freestanding in a game library
#include "xenoforge/ppc_runtime.h"

namespace xf::ppc::ops {

template <Opcode> inline constexpr bool implemented = false;

template <Opcode op> XF_INLINE void run(Context &c, [[maybe_unused]] uint8_t *base, Instr i) {
    if (c.system && c.system->unimplemented) c.system->unimplemented(c, op, i.code);
}

#define XF_OP(name)                                                                                                    \
    template <> inline constexpr bool implemented<Opcode::name> = true;                                                \
    template <>                                                                                                        \
    XF_INLINE void run<Opcode::name>([[maybe_unused]] Context & c, [[maybe_unused]] uint8_t *base,                     \
                                     [[maybe_unused]] Instr i)

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

// the 4 KB offset of the 0xE0000000 physical view (memory.h; Xenia's
// host_address_offset)
// The guest's memory and the Context are separate allocations (the context
// lives on the host's heap, never inside the guest's reservation): said to
// the compiler, a guest store no longer makes it reload the registers it
// holds from the context, nor the base (game B: about 12% less CPU time
// running deterministically; docs/recompiler.md, "Where the time goes").
// The base itself comes as a parameter (every instruction's `base`, beside
// the context): a generated function reads it from the context once, at its
// entry, the first tier's stencils pass it along in a register of their
// convention, and the interpreter reads it once a run; never a load of the
// context's field per access (docs/recompiler.md, "The memory base in a
// register").
XF_INLINE uint8_t *host(Context &c, uint8_t *base, uint32_t ea) {
    uint8_t *p = base + ea + (ea >= 0xE0000000u ? 0x1000u : 0u);
#if __has_builtin(__builtin_assume_separate_storage)
    __builtin_assume_separate_storage(p, &c);
#endif
    return p;
}
XF_INLINE uint8_t load8(Context &c, uint8_t *base, uint32_t ea) { return *host(c, base, ea); }
XF_INLINE uint16_t load16(Context &c, uint8_t *base, uint32_t ea) {
    uint16_t v;
    lib::memcpy(&v, host(c, base, ea), 2);
    return __builtin_bswap16(v);
}
XF_INLINE uint32_t load32(Context &c, uint8_t *base, uint32_t ea) {
    uint32_t v;
    lib::memcpy(&v, host(c, base, ea), 4);
    return __builtin_bswap32(v);
}
XF_INLINE uint64_t load64(Context &c, uint8_t *base, uint32_t ea) {
    uint64_t v;
    lib::memcpy(&v, host(c, base, ea), 8);
    return __builtin_bswap64(v);
}
XF_INLINE void store8(Context &c, uint8_t *base, uint32_t ea, uint8_t v) { *host(c, base, ea) = v; }
XF_INLINE void store16(Context &c, uint8_t *base, uint32_t ea, uint16_t v) {
    v = __builtin_bswap16(v);
    lib::memcpy(host(c, base, ea), &v, 2);
}
XF_INLINE void store32(Context &c, uint8_t *base, uint32_t ea, uint32_t v) {
    v = __builtin_bswap32(v);
    lib::memcpy(host(c, base, ea), &v, 4);
}
XF_INLINE void store64(Context &c, uint8_t *base, uint32_t ea, uint64_t v) {
    v = __builtin_bswap64(v);
    lib::memcpy(host(c, base, ea), &v, 8);
}

// ---- MMIO: the devices' registers (the GPU's at 0x7FC80000) ----
// Xenia catches an access there with a page fault and decodes its JIT's
// 32-bit mov or movbe (cpu/mmio_handler.cc), the only form it handles: the
// games reach the registers with lwz and stw. Here those check the address
// and call the system's handlers; a base of r1 (the stack) or r13 (the
// PCR) never points there, and with the word a constant the check is gone
// for them.

constexpr uint32_t MMIO_BEGIN = 0x7FC80000, MMIO_END = 0x80000000;
XF_INLINE bool mmio(uint32_t ra, uint32_t ea) { return ra != 1 && ra != 13 && ea - MMIO_BEGIN < MMIO_END - MMIO_BEGIN; }
[[gnu::cold, gnu::noinline]] inline uint32_t mmio_read(Context &c, uint32_t ea) {
    return c.system && c.system->mmio_read ? c.system->mmio_read(c, ea) : 0;
}
[[gnu::cold, gnu::noinline]] inline void mmio_write(Context &c, uint32_t ea, uint32_t v) {
    if (c.system && c.system->mmio_write) c.system->mmio_write(c, ea, v);
}
// a 32-bit load or store through base register ra
XF_INLINE uint32_t load32_io(Context &c, uint8_t *base, uint32_t ra, uint32_t ea) {
    if (mmio(ra, ea)) [[unlikely]]
        return mmio_read(c, ea);
    return load32(c, base, ea);
}
XF_INLINE void store32_io(Context &c, uint8_t *base, uint32_t ra, uint32_t ea, uint32_t v) {
    if (mmio(ra, ea)) [[unlikely]]
        return mmio_write(c, ea, v);
    store32(c, base, ea, v);
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

// ---- yield points (docs/recompiler.md, "Yield points") ----

// A yield point's weight, in the budget's units (a 32nd of the deterministic
// clock's 100 ns, about ten Xenon cycles): a loop's back edge one, a priority
// hint (or rx,rx,rx, which the XDK's spin loops run) eight, so a spin reaches
// the end of its slice sooner than a loop doing work; an indirect call (a
// taken bcctrl or bclrl) 32, 100 ns of the clock. The indirect call is
// counted here, at the guest's instruction, in the generated code and the
// interpreter alike, not where the runtime's dispatch is entered: which calls
// reach dispatch depends on what discovery found, and the clock must not.
constexpr int64_t YIELD_BACK_EDGE = 1;
constexpr int64_t YIELD_PRIORITY_HINT = 8;
constexpr int64_t YIELD_INDIRECT_CALL = 32;

// the budget ran out: the system's hook, or a fresh budget when there is none
[[gnu::cold]] [[gnu::noinline]] inline void yield_hook(Context &c, int64_t weight) {
    if (c.system && c.system->yield)
        c.system->yield(c, weight);
    else
        c.yield_budget = INT64_MAX;
}
// Not counting (playing): a load and a branch. Counting: the weight off the
// budget, and the hook below 0 (it may hand the processor over). The flag is
// loaded as an atomic (relaxed: a plain load on the hosts) because other
// threads set it as the thread runs (Kernel::pause, the console scheduler's
// preemption), and a loop must see it: a plain load could be hoisted out.
XF_INLINE bool yield_counting(const Context &c) { return __atomic_load_n(&c.yield_counting, __ATOMIC_RELAXED); }
XF_INLINE void yield_point(Context &c, int64_t weight) {
    if (yield_counting(c)) [[unlikely]] {
        c.yield_budget -= weight;
        if (c.yield_budget < 0) yield_hook(c, weight);
    }
}

// db16cyc (or 31,31,31), the XDK's spin-wait hint: the Xenon gives the
// core's other hardware thread the pipeline. Here, by how long the spin has
// gone on: the host's own spin-wait hint each time (x86's pause, ARM's
// yield, as Xenia's DELAY_EXECUTION: the SMT sibling gets the core), and
// every SPIN_HINTS_BEFORE_YIELD of them in a row, the processor given to
// whatever else is ready on it (System::spin), so a spin that lasts frees
// the core for the host's threads, at no cost when nothing else is ready.
// (Beyond Xenia, which pauses only.) The deterministic mode counts the hint
// as a yield point and hands over there instead.
constexpr uint32_t SPIN_HINTS_BEFORE_YIELD = 2048;
[[gnu::cold]] [[gnu::noinline]] inline void spin_hook(Context &c) {
    c.spin_hints = 0;
    if (c.system && c.system->spin) c.system->spin(c);
}
XF_INLINE void spin_hint(Context &c) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
    if (++c.spin_hints >= SPIN_HINTS_BEFORE_YIELD) [[unlikely]]
        spin_hook(c);
}

} // namespace xf::ppc::ops

#include "xenoforge/ppc_ops/integer.h"
#include "xenoforge/ppc_ops/control.h"
#include "xenoforge/ppc_ops/memory.h"
#include "xenoforge/ppc_ops/fpu.h"
#include "xenoforge/ppc_ops/vmx.h"
#include "xenoforge/ppc_ops/vmx_float.h"
