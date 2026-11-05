// The PPC interpreter (ppc_interp.h). Branches as Xenia's bx, bcx, bclrx
// and bcctrx emitters (ppc_emit_control.cc; BSD, see LICENSE-xenia): LR is
// written after the target is read, so `blrl` goes to the old LR.

#include "xenoforge/ppc_interp.h"

#include "xenoforge/ppc_ops.h"

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

} // namespace

bool implemented(Opcode op) {
    switch (op) {
    case Opcode::bx: case Opcode::bcx: case Opcode::bclrx: case Opcode::bcctrx: return true;
    case Opcode::Invalid: return false;
    default: return IMPLEMENTED[size_t(op)];
    }
}

Stopped interpret(Context &c, uint32_t pc, uint32_t until, uint64_t max_steps) {
    for (uint64_t step = 0; step < max_steps; step++) {
        if (pc == until) return {Stopped::Returned, pc};
        Instr i{ops::load32(c, pc)};
        Opcode op = decode(i.code);
        c.pc = pc;
        uint32_t next = pc + 4;
        switch (op) {
        case Opcode::bx: {
            uint32_t target = uint32_t(ops::exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            if (i.I.LK) {
                c.lr = next;
                if (c.system && c.system->call && c.system->call(c, target)) break; // ran it: on after the call
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
            }
            if (taken) next = target;
            break;
        }
        case Opcode::bclrx: {
            bool taken = ops::branch_condition(c, i.XL.BO, i.XL.BI);
            uint32_t target = uint32_t(c.lr) & ~3u;
            if (i.XL.LK) {
                c.lr = next;
                if (taken && c.system && c.system->call && c.system->call(c, target)) break;
            }
            if (taken) next = target;
            break;
        }
        case Opcode::bcctrx: {
            bool taken = ops::branch_condition_ctr(c, i.XL.BO, i.XL.BI);
            uint32_t target = uint32_t(c.ctr) & ~3u;
            if (i.XL.LK) {
                c.lr = next;
                if (taken && c.system && c.system->call && c.system->call(c, target)) break;
            }
            if (taken) next = target;
            break;
        }
        case Opcode::Invalid: return {Stopped::Invalid, pc, op};
        default:
            if (!IMPLEMENTED[size_t(op)]) return {Stopped::Unimplemented, pc, op};
            TABLE[size_t(op)](c, i);
            break;
        }
        pc = next;
    }
    return {Stopped::Steps, pc};
}

} // namespace xf::ppc
