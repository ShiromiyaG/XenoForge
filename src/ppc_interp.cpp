// The PPC interpreter (ppc_interp.h). Branches as Xenia's bx, bcx, bclrx
// and bcctrx emitters (ppc_emit_control.cc; BSD, see LICENSE-xenia): LR is
// written after the target is read, so `blrl` goes to the old LR.

#include "xenoforge/ppc_interp.h"

#include "xenoforge/lockstep.h"
#include "xenoforge/ppc_ops.h"

#include <cstddef>
#include <type_traits>

namespace xf::ppc {
namespace {

using Fn = void (*)(Context &, Instr);

constexpr Fn TABLE[] = {
#define XF_PPC_OP(name, opcode, form, group, description) &ops::run<Opcode::name>,
#include "xenoforge/ppc_opcodes.inc"
#undef XF_PPC_OP
};
constexpr bool IMPLEMENTED[] = {
#define XF_PPC_OP(name, opcode, form, group, description) ops::implemented<Opcode::name>,
#include "xenoforge/ppc_opcodes.inc"
#undef XF_PPC_OP
};

// The loop, with an observer (lockstep.h) or without one (Watch nullptr_t:
// the observer's calls compile away).
template <class Watch>
Stopped run(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps, uint32_t stack_top, Watch *watch) {
    constexpr bool watched = !std::is_same_v<Watch, std::nullptr_t>;
    // the port's hooks (System::hook_pages): a page holding one is looked at
    // further, for mid-function hooks here and hooked functions entered by a
    // branch (calls reach them through System::call)
    const uint8_t *const hook_pages = c.system ? c.system->hook_pages : nullptr;
    auto hooked_page = [&](uint32_t a) { return hook_pages && ((hook_pages[a >> 15] >> ((a >> 12) & 7)) & 1); };
    // a branch without LK to a hooked function's start from outside it: the
    // hooks and the function run, then on at LR as after a call
    auto entered = [&](uint32_t target, uint32_t from) {
        return hooked_page(target) && c.system->enter_hooked && c.system->enter_hooked(c, target, from);
    };
    uint64_t step = 0;
    auto stopped = [&](Stopped s) {
        s.steps = step;
        return s;
    };
    for (; step < max_steps; step++) {
        if (pc == until && uint32_t(c.r[1]) >= stack_top) return stopped({Stopped::Returned, pc});
        if (hooked_page(pc) && c.system->mid_hook) [[unlikely]] {
            c.pc = pc;
            const uint32_t to = c.system->mid_hook(c, pc);
            if (to == 1) {
                // a return, as blr
                const uint32_t target = uint32_t(c.lr) & ~3u;
                if (stack_top && target != until && uint32_t(c.r[1]) > stack_top)
                    return stopped({Stopped::Unwound, target});
                pc = target;
                continue;
            }
            if (to) {
                pc = to;
                continue;
            }
        }
        Instr i{ops::load32(c, pc)};
        Opcode op = decode(i.code);
        c.pc = pc;
        if constexpr (watched) watch->before(c, pc, op, i);
        uint32_t next = pc + 4;
        switch (op) {
        case Opcode::bx: {
            uint32_t target = uint32_t(ops::exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            if (i.I.LK) {
                c.lr = next;
                if (c.system && c.system->call && c.system->call(c, target)) break; // ran it: on after the call
            } else {
                if (target <= pc) ops::yield_point(c, ops::YIELD_BACK_EDGE); // a loop's back edge, as the generated code's
                if (entered(target, pc)) [[unlikely]] {
                    next = uint32_t(c.lr) & ~3u;
                    break;
                }
            }
            next = target;
            break;
        }
        case Opcode::bcx: {
            bool taken = ops::branch_condition(c, i.B.BO, i.B.BI);
            uint32_t target = uint32_t(ops::exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
            if (i.B.LK) {
                c.lr = next;
                if (taken && c.system && c.system->call && c.system->call(c, target)) break;
            } else if (taken) {
                if (target <= pc) ops::yield_point(c, ops::YIELD_BACK_EDGE);
                if (entered(target, pc)) [[unlikely]] {
                    next = uint32_t(c.lr) & ~3u;
                    break;
                }
            }
            if (taken) next = target;
            break;
        }
        case Opcode::bclrx: {
            bool taken = ops::branch_condition(c, i.XL.BO, i.XL.BI);
            uint32_t target = uint32_t(c.lr) & ~3u;
            if (i.XL.LK) {
                c.lr = next;
                if (taken) ops::yield_point(c, ops::YIELD_INDIRECT_CALL); // an indirect call, as the generated code's
                if (taken && c.system && c.system->call && c.system->call(c, target)) break;
            } else if (taken && stack_top && target != until && uint32_t(c.r[1]) > stack_top) {
                c.pc = pc;
                return stopped({Stopped::Unwound, target});
            }
            if (taken) next = target;
            break;
        }
        case Opcode::bcctrx: {
            bool taken = ops::branch_condition_ctr(c, i.XL.BO, i.XL.BI);
            uint32_t target = uint32_t(c.ctr) & ~3u;
            if (i.XL.LK) {
                c.lr = next;
                if (taken) ops::yield_point(c, ops::YIELD_INDIRECT_CALL);
                if (taken && c.system && c.system->call && c.system->call(c, target)) break;
            } else if (taken && entered(target, pc)) [[unlikely]] {
                next = uint32_t(c.lr) & ~3u;
                break;
            }
            if (taken) next = target;
            break;
        }
        case Opcode::Invalid: return stopped({Stopped::Invalid, pc, op});
        default:
            if (!IMPLEMENTED[size_t(op)]) return stopped({Stopped::Unimplemented, pc, op});
            TABLE[size_t(op)](c, i);
            break;
        }
        if constexpr (watched) watch->after(c, pc, op);
        pc = next;
    }
    return stopped({Stopped::Steps, pc});
}

} // namespace

bool implemented(Opcode op) {
    switch (op) {
    case Opcode::bx: case Opcode::bcx: case Opcode::bclrx: case Opcode::bcctrx: return true;
    case Opcode::Invalid: return false;
    default: return IMPLEMENTED[size_t(op)];
    }
}

Stopped interpret(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps, uint32_t stack_top) {
    return run<std::nullptr_t>(c, pc, until, max_steps, stack_top, nullptr);
}

Stopped interpret_observed(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps, uint32_t stack_top,
                           Observer &observer) {
    return run(c, pc, until, max_steps, stack_top, &observer);
}

bool store_extent(const Context &c, Opcode op, Instr i, uint32_t *address, uint32_t *size) {
    // the effective addresses as ppc_ops/memory.h computes them (RA 0 reads
    // as 0; the update forms' RA is never 0 in a valid word, so the same)
    auto reg = [&](uint32_t n) { return n ? c.r[n] : 0; };
    const uint32_t d = uint32_t(reg(i.D.RA) + uint64_t(ops::exts16(i.D.DS)));
    const uint32_t ds = uint32_t(reg(i.DS.RA) + uint64_t(ops::exts16(i.DS.DS << 2)));
    const uint32_t x = uint32_t(reg(i.X.RA) + c.r[i.X.RB]);
    auto out = [&](uint32_t a, uint32_t n) {
        *address = a, *size = n;
        return true;
    };
    switch (op) {
    case Opcode::stb: case Opcode::stbu: return out(d, 1);
    case Opcode::sth: case Opcode::sthu: return out(d, 2);
    case Opcode::stw: case Opcode::stwu: case Opcode::stfs: case Opcode::stfsu: return out(d, 4);
    case Opcode::stfd: case Opcode::stfdu: return out(d, 8);
    case Opcode::stmw: return out(d, 4 * (32 - i.D.RT));
    case Opcode::std: case Opcode::stdu: return out(ds, 8);
    case Opcode::stbx: case Opcode::stbux: return out(x, 1);
    case Opcode::sthx: case Opcode::sthux: case Opcode::sthbrx: return out(x, 2);
    case Opcode::stwx: case Opcode::stwux: case Opcode::stwbrx: case Opcode::stwcx:
    case Opcode::stfsx: case Opcode::stfsux: case Opcode::stfiwx: return out(x, 4);
    case Opcode::stdx: case Opcode::stdux: case Opcode::stdbrx: case Opcode::stdcx:
    case Opcode::stfdx: case Opcode::stfdux: return out(x, 8);
    case Opcode::stswi: return out(uint32_t(reg(i.X.RA)), i.X.RB ? i.X.RB : 32);
    case Opcode::stswx: return out(x, c.xer_bc & 0x7F);
    // the vector stores (VX128_1's RA and RB sit where X's do): the aligned
    // 16 bytes around the address, more than an element or a half stores
    case Opcode::stvebx: case Opcode::stvehx: case Opcode::stvewx: case Opcode::stvewx128:
    case Opcode::stvx: case Opcode::stvx128: case Opcode::stvxl: case Opcode::stvxl128:
    case Opcode::stvlx: case Opcode::stvlx128: case Opcode::stvlxl: case Opcode::stvlxl128:
    case Opcode::stvrx: case Opcode::stvrx128: case Opcode::stvrxl: case Opcode::stvrxl128:
        return out(x & ~15u, 16);
    case Opcode::dcbz: case Opcode::dcbz128: return out(x & ~127u, 128);
    default: return false;
    }
}

bool load_extent(const Context &c, Opcode op, Instr i, uint32_t *address, uint32_t *size) {
    auto reg = [&](uint32_t n) { return n ? c.r[n] : 0; };
    const uint32_t d = uint32_t(reg(i.D.RA) + uint64_t(ops::exts16(i.D.DS)));
    const uint32_t ds = uint32_t(reg(i.DS.RA) + uint64_t(ops::exts16(i.DS.DS << 2)));
    const uint32_t x = uint32_t(reg(i.X.RA) + c.r[i.X.RB]);
    auto out = [&](uint32_t a, uint32_t n) {
        *address = a, *size = n;
        return true;
    };
    switch (op) {
    case Opcode::lbz: case Opcode::lbzu: return out(d, 1);
    case Opcode::lhz: case Opcode::lhzu: case Opcode::lha: case Opcode::lhau: return out(d, 2);
    case Opcode::lwz: case Opcode::lwzu: case Opcode::lfs: case Opcode::lfsu: return out(d, 4);
    case Opcode::lfd: case Opcode::lfdu: return out(d, 8);
    case Opcode::lmw: return out(d, 4 * (32 - i.D.RT));
    case Opcode::ld: case Opcode::ldu: return out(ds, 8);
    case Opcode::lwa: return out(ds, 4);
    case Opcode::lbzx: case Opcode::lbzux: return out(x, 1);
    case Opcode::lhzx: case Opcode::lhzux: case Opcode::lhax: case Opcode::lhaux: case Opcode::lhbrx: return out(x, 2);
    case Opcode::lwzx: case Opcode::lwzux: case Opcode::lwax: case Opcode::lwaux: case Opcode::lwbrx:
    case Opcode::lwarx: case Opcode::lfsx: case Opcode::lfsux: return out(x, 4);
    case Opcode::ldx: case Opcode::ldux: case Opcode::ldbrx: case Opcode::ldarx:
    case Opcode::lfdx: case Opcode::lfdux: return out(x, 8);
    case Opcode::lswi: return out(uint32_t(reg(i.X.RA)), i.X.RB ? i.X.RB : 32);
    case Opcode::lswx: return out(x, c.xer_bc & 0x7F);
    case Opcode::lvebx: case Opcode::lvehx: case Opcode::lvewx: case Opcode::lvewx128:
    case Opcode::lvx: case Opcode::lvx128: case Opcode::lvxl: case Opcode::lvxl128:
    case Opcode::lvlx: case Opcode::lvlx128: case Opcode::lvlxl: case Opcode::lvlxl128:
    case Opcode::lvrx: case Opcode::lvrx128: case Opcode::lvrxl: case Opcode::lvrxl128:
        return out(x & ~15u, 16);
    default: return false;
    }
}

} // namespace xf::ppc
