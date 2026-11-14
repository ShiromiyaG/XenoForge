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
//
// Every stencil returns how its function's body ends, for the frame that
// checks the returns of a function that switches the stack (xf_st_checked,
// xf_st_resume_body_checked), which holds the entry's LR and r1 as the
// emitter's xf_lr and xf_sp: EXIT_RETURN, a return to check; EXIT_PLAIN, one
// not checked (after an invalid instruction); any other value, the stack
// switch's thunk a branch went to, which that frame calls with its entry's LR
// before the check (the emitter's `switch_stack(c, thunk, xf_lr)`). The other
// bodies' callers ignore it.

// Every symbol declared from here on is hidden: in position independent code
// (ELF) a hidden one is reached pc-relative, never through the GOT, so its
// relocation is a field the tier can write (xf::ppc::runtime's too).
#pragma GCC visibility push(hidden)

#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_ops.h"

using namespace xf::ppc;

#define XF_CC __attribute__((preserve_none))
#define XF_STENCIL extern "C" XF_CC uint32_t

extern "C" {
extern char xf_hole_word[], xf_hole_a[], xf_hole_b[], xf_hole_data[];
XF_CC uint32_t xf_hole_next(Context *c);
XF_CC uint32_t xf_hole_target(Context *c);
XF_CC uint32_t xf_hole_call(Context *c);
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

using Body = XF_CC uint32_t (*)(Context *c);

constexpr uint32_t EXIT_RETURN = 0, EXIT_PLAIN = 1;

// A checked function's end (ppc_generated.h's XF_CHECKED_RETURN), from what
// its body returned and the entry's LR and r1.
XF_INLINE uint32_t checked_exit(Context *c, uint32_t exit, uint32_t lr, uint32_t sp) {
    if (exit == EXIT_PLAIN) return EXIT_RETURN;
    if (exit != EXIT_RETURN) switch_stack(*c, exit, lr);
    if (uint32_t(c->r[1]) != sp || (uint32_t(c->lr) & ~3u) != lr) unwind(*c);
    return EXIT_RETURN;
}

// A body with resume sites entered at `at` (0: its start; else a resume site,
// looked up in the data: the sites' guest addresses, a, then their code's
// host addresses, 8-byte aligned).
XF_INLINE Body resume_at(uint32_t at) {
    if (!at) return xf_hole_call;
    const uint32_t n = HOLE_A;
    const uint32_t *sites = reinterpret_cast<const uint32_t *>(xf_hole_data);
    const uint64_t *hosts = reinterpret_cast<const uint64_t *>(xf_hole_data + ((size_t(n) * 4 + 7) & ~size_t(7)));
    for (uint32_t k = 0; k < n; k++)
        if (sites[k] == at) return reinterpret_cast<Body>(hosts[k]);
    return xf_hole_call; // (resumable() passes only its sites)
}

// A mid-function hook's data: its slot, and the labels it may jump to.
struct MidData {
    HookSlot *slot;
    uint64_t count;
    struct {
        uint64_t guest, host;
    } jumps[1];
};

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
XF_STENCIL xf_st_ret(Context *) { return EXIT_RETURN; }
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
XF_STENCIL xf_st_tail_dispatch(Context *c) {
    dispatch(*c, HOLE_A);
    return EXIT_RETURN;
}
// a tail branch to CTR through dispatch (bctr with no table), then the return
XF_STENCIL xf_st_tail_dispatch_ctr(Context *c) {
    dispatch(*c, uint32_t(c->ctr) & ~3u);
    return EXIT_RETURN;
}
// an invalid instruction at address a reached: the runtime told, then the
// return, which is not checked (the emitter's plain `return` after invalid())
XF_STENCIL xf_st_invalid(Context *c) {
    invalid(*c, HOLE_A);
    return EXIT_PLAIN;
}

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
    return EXIT_RETURN;
}

// ---- stack switches, resume sites and the port's hooks ----

// A call to a stack switch's thunk (KeSetCurrentStackPointers'; LR already
// set): this frame goes on at b when its stack is switched back to.
XF_STENCIL xf_st_switch_call(Context *c) {
    switch_stack(*c, HOLE_A, HOLE_B);
    NEXT;
}
// A branch to a stack switch's thunk, then the return: this frame goes on
// where its own return goes.
XF_STENCIL xf_st_switch_tail(Context *c) {
    switch_stack(*c, HOLE_A, uint32_t(c->lr) & ~3u);
    return EXIT_RETURN;
}
// The same in a function whose returns are checked: the checking frame
// switches with its entry's LR (checked_exit).
XF_STENCIL xf_st_switch_tail_checked(Context *) { return HOLE_A; }

// A function that switches the stack (the emitter's XF_CHECKED_RETURN): its
// body called with the entry's LR and r1 kept here, every return checked
// against them.
XF_STENCIL xf_st_checked(Context *c) {
    const uint32_t lr = uint32_t(c->lr), sp = uint32_t(c->r[1]);
    return checked_exit(c, xf_hole_call(c), lr, sp);
}

// A function with resume sites (the emitter's resumable(), XF_RESUMABLE_BODY):
// the runtime runs its body through the target (xf_st_resume_body, the C++
// convention), from the start and again from a resume site an Unwind came
// back to. Data: the sites (a of them), as resume_at reads them.
XF_STENCIL xf_st_resumable(Context *c) {
    resumable(*c, reinterpret_cast<ResumableBody>(reinterpret_cast<void *>(&xf_hole_target)), uint32_t(c->lr),
              uint32_t(c->r[1]), reinterpret_cast<const uint32_t *>(xf_hole_data), HOLE_A);
    return EXIT_RETURN;
}
extern "C" void xf_st_resume_body(Context &c, uint32_t at, uint32_t, uint32_t) { resume_at(at)(&c); }
extern "C" void xf_st_resume_body_checked(Context &c, uint32_t at, uint32_t lr, uint32_t sp) {
    checked_exit(&c, resume_at(at)(&c), lr, sp);
}

// A function the port hooks (the emitter's XF_HOOKED): the hooks when its slot
// (data: a pointer to it) holds any, else on into its body.
XF_STENCIL xf_st_hooked(Context *c) {
    HookSlot &slot = **reinterpret_cast<HookSlot *const *>(xf_hole_data);
    if (__atomic_load_n(&slot.hooks, __ATOMIC_ACQUIRE)) {
        hooked(*c, slot);
        return EXIT_RETURN;
    }
    NEXT;
}

// A mid-function hook before an instruction (the emitter's mid_hook): when its
// slot holds hooks, they say where the code goes on: here (0), a return (1),
// one of the labels in the data, else a dispatch to the address and a return.
XF_STENCIL xf_st_mid(Context *c) {
    const MidData &m = *reinterpret_cast<const MidData *>(xf_hole_data);
    if (__atomic_load_n(&m.slot->hooks, __ATOMIC_ACQUIRE)) {
        const uint32_t to = mid_hook(*c, *m.slot);
        if (to == 1) return EXIT_RETURN;
        if (to) {
            for (uint64_t k = 0; k < m.count; k++)
                if (m.jumps[k].guest == to) [[clang::musttail]]
                    return reinterpret_cast<Body>(m.jumps[k].host)(c);
            dispatch(*c, to);
            return EXIT_RETURN;
        }
    }
    NEXT;
}

// What a function's entry jumps to once the game library has it (the
// tier-up, stencil.h's Module::adopt): the library's function (data: a
// pointer to it), called with the C++ convention, then the return.
XF_STENCIL xf_st_adopted(Context *c) {
    (*reinterpret_cast<void (*const *)(Context &)>(xf_hole_data))(*c);
    return EXIT_RETURN;
}

// The conditional forms by BO (the branch's condition), BI a hole: the
// compiler folds what BO decides, as it does with the word as a constant.
#define XF_BO_ALL(X)                                                                                                   \
    X(0)                                                                                                               \
    X(1)                                                                                                               \
    X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21)    \
        X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

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
        if (ops::branch_condition(*c, bo, HOLE_A)) return EXIT_RETURN;                                                 \
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
