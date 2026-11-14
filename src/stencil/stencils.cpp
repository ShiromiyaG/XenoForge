// The copy-and-patch tier's stencils (docs/recompiler.md, "A copy-and-patch
// first tier"): each function below is what one guest instruction, or one
// piece of the control flow the emitter writes in C++, becomes, compiled by
// clang once, when XenoForge is built, with the game library's flags
// (game_build_flags.h: XF_GAME_LIBRARY, freestanding, the same -O2 and CPU
// level), so it runs exactly the code the library would. tools/xfstencil.cpp
// takes their machine code and the places where it refers to the extern
// symbols below (its holes) out of the object file; at run time
// src/stencil_jit.cpp copies a stencil per instruction and writes the holes:
//
// - xf_hole_word, xf_hole_a, xf_hole_b: 32-bit values (the instruction word, a
//   guest address, a condition bit). Only the low 32 bits of their address
//   are used (hole32), so a hole is patched by its pc-relative displacement
//   alone, wherever the code is.
// - xf_hole_data: the address of data the tier lays out beside the code (a
//   jump table's host addresses).
// - xf_hole_next: the stencil that follows (a tail jump; dropped when it is
//   the last instruction and the next stencil is placed right after).
// - xf_hole_target: a branch's target, xf_hole_call: a function's body called.
//
// Stencils chain with clang's preserve_none convention and musttail calls, as
// CPython's JIT does: the context stays in one register (r12 on x86-64, x20 on
// ARM64), a tail call is a jump, and no stencil saves a register for the next.
// A function's body is entered by a call and left by the `ret` of xf_st_ret;
// xf_st_entry is the C++ entry (GeneratedFunction::fn) around it.

// Every symbol declared from here on is hidden: in position independent code
// (ELF) a hidden one is reached pc-relative, never through the GOT, so its
// relocation is a field the tier can write (xf::ppc::runtime's too).
#pragma GCC visibility push(hidden)

#include "xenoforge/ppc_ops.h"

using namespace xf::ppc;

#define XF_CC __attribute__((preserve_none))
#define XF_STENCIL extern "C" XF_CC void

extern "C" {
extern char xf_hole_word[], xf_hole_a[], xf_hole_b[], xf_hole_data[];
XF_CC void xf_hole_next(Context *c);
XF_CC void xf_hole_target(Context *c);
XF_CC void xf_hole_call(Context *c);
}

namespace {

// A 32-bit hole's value: its address's low half, kept in a register so the
// compiler neither folds it into another address nor assumes anything of it
// (an address is never null, an int may be).
XF_INLINE uint32_t hole32(const char *p) {
    uint32_t v = uint32_t(uintptr_t(p));
    __asm__("" : "+r"(v));
    return v;
}
#define WORD hole32(xf_hole_word)
#define HOLE_A hole32(xf_hole_a)
#define HOLE_B hole32(xf_hole_b)
#define NEXT [[clang::musttail]] return xf_hole_next(c)
#define TARGET [[clang::musttail]] return xf_hole_target(c)

using Body = XF_CC void (*)(Context *c);

} // namespace

// ---- every instruction of the semantics: ops::run with the word as a hole ----

#define XF_PPC_OP(name, opcode, form, group, description)                                                              \
    XF_STENCIL xf_st_op_##name(Context *c) {                                                                           \
        ops::run<Opcode::name>(*c, Instr{WORD});                                                                       \
        NEXT;                                                                                                          \
    }
#include "xenoforge/ppc_opcodes.inc"
#undef XF_PPC_OP

// ---- the control flow, as recomp_emit.cpp writes it in C++ ----

// the C++ entry: the body called with the preserve_none convention (which
// saves nothing, so this frame saves the host's callee-saved registers)
extern "C" void xf_st_entry(Context &c) { xf_hole_call(&c); }

// a return (blr), and the end of a function's body
XF_STENCIL xf_st_ret(Context *) {}
// a branch inside the function or a tail call into another one's body
XF_STENCIL xf_st_goto(Context *c) { TARGET; }
// one more guest instruction run (Options::count; data: the counter)
XF_STENCIL xf_st_count(Context *c) {
    ++*reinterpret_cast<uint64_t *>(xf_hole_data);
    NEXT;
}
// a loop's back edge, taken (XF_YIELD)
XF_STENCIL xf_st_yield(Context *c) {
    ops::yield_point(*c, ops::YIELD_BACK_EDGE);
    NEXT;
}
// c.lr = a (bl's return address)
XF_STENCIL xf_st_set_lr(Context *c) {
    c->lr = HOLE_A;
    NEXT;
}
// c.pc = a (before a trap, which tells the system where it is)
XF_STENCIL xf_st_set_pc(Context *c) {
    c->pc = HOLE_A;
    NEXT;
}
// a call to another function's body (LR already set)
XF_STENCIL xf_st_call(Context *c) {
    xf_hole_call(c);
    NEXT;
}
// a call through the runtime's dispatch to address a (no body for it here)
XF_STENCIL xf_st_call_dispatch(Context *c) {
    dispatch(*c, HOLE_A);
    NEXT;
}
// a tail branch to address a through dispatch, then the return
XF_STENCIL xf_st_tail_dispatch(Context *c) { dispatch(*c, HOLE_A); }
// a tail branch to CTR through dispatch (bctr with no table), then the return
XF_STENCIL xf_st_tail_dispatch_ctr(Context *c) { dispatch(*c, uint32_t(c->ctr) & ~3u); }
// an invalid instruction at address a reached: the runtime told, then the return
XF_STENCIL xf_st_invalid(Context *c) { invalid(*c, HOLE_A); }

// A jump table's bctr: CTR's target looked up among the cases (data: a host
// address a case, 0 for none, from guest address a on, b bytes of it), else
// dispatch and return, as the emitter's switch's default.
XF_STENCIL xf_st_table(Context *c) {
    const uint32_t target = uint32_t(c->ctr) & ~3u;
    const uint32_t offset = target - HOLE_A;
    if (offset < HOLE_B) {
        const Body body = reinterpret_cast<const Body *>(xf_hole_data)[offset >> 2];
        if (body) [[clang::musttail]]
            return body(c);
    }
    dispatch(*c, target);
}

// The conditional forms by BO (the branch's condition), BI a hole: the
// compiler folds what BO decides, as it does with the word as a constant.
#define XF_BO_ALL(X)                                                                                                   \
    X(0)                                                                                                               \
    X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20)     \
        X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define XF_BO_STENCILS(bo)                                                                                             \
    /* bc: to the target when taken (BI: a) */                                                                         \
    XF_STENCIL xf_st_bc_##bo(Context *c) {                                                                             \
        if (ops::branch_condition(*c, bo, HOLE_A)) TARGET;                                                             \
        NEXT;                                                                                                          \
    }                                                                                                                  \
    /* bcctr's condition (no CTR decrement): to the target when taken */                                               \
    XF_STENCIL xf_st_bcctr_##bo(Context *c) {                                                                          \
        if (ops::branch_condition_ctr(*c, bo, HOLE_A)) TARGET;                                                         \
        NEXT;                                                                                                          \
    }                                                                                                                  \
    /* bcl: LR = b, then the target (the call's sequence) when taken */                                                \
    XF_STENCIL xf_st_bcl_##bo(Context *c) {                                                                            \
        const bool taken = ops::branch_condition(*c, bo, HOLE_A);                                                      \
        c->lr = HOLE_B;                                                                                                \
        if (taken) TARGET;                                                                                             \
        NEXT;                                                                                                          \
    }                                                                                                                  \
    /* bclr, conditional: the return when taken */                                                                     \
    XF_STENCIL xf_st_bclr_##bo(Context *c) {                                                                           \
        if (ops::branch_condition(*c, bo, HOLE_A)) return;                                                             \
        NEXT;                                                                                                          \
    }                                                                                                                  \
    /* bclrl: an indirect call to the old LR through dispatch when taken; LR = b */                                    \
    XF_STENCIL xf_st_bclrl_##bo(Context *c) {                                                                          \
        const uint32_t target = uint32_t(c->lr) & ~3u;                                                                 \
        const bool taken = ops::branch_condition(*c, bo, HOLE_A);                                                      \
        c->lr = HOLE_B;                                                                                                \
        if (taken) {                                                                                                   \
            ops::yield_point(*c, ops::YIELD_INDIRECT_CALL);                                                            \
            dispatch(*c, target);                                                                                      \
        }                                                                                                              \
        NEXT;                                                                                                          \
    }                                                                                                                  \
    /* bcctrl: an indirect call to CTR through dispatch when taken; LR = b */                                          \
    XF_STENCIL xf_st_bcctrl_##bo(Context *c) {                                                                         \
        const uint32_t target = uint32_t(c->ctr) & ~3u;                                                                \
        const bool taken = ops::branch_condition_ctr(*c, bo, HOLE_A);                                                  \
        c->lr = HOLE_B;                                                                                                \
        if (taken) {                                                                                                   \
            ops::yield_point(*c, ops::YIELD_INDIRECT_CALL);                                                            \
            dispatch(*c, target);                                                                                      \
        }                                                                                                              \
        NEXT;                                                                                                          \
    }
XF_BO_ALL(XF_BO_STENCILS)
