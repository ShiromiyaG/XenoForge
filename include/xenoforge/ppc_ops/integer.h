// Integer arithmetic, logic, compares, rotates and shifts (ppc_ops.h).
// Translated from Xenia's ppc_emit_alu.cc (BSD; see LICENSE-xenia).

#pragma once

#include "xenoforge/ppc_ops.h"

namespace xf::ppc::ops {

XF_OP(addx) {
    // RD <- (RA) + (RB)
    uint64_t ra = c.r[i.XO.RA], rb = c.r[i.XO.RB], v = ra + rb;
    c.r[i.XO.RT] = v;
    if (i.XO.OE) {
        int64_t a = int64_t(ra), b = int64_t(rb), s = int64_t(v);
        set_ov(c, ((a ^ s) & (b ^ s)) < 0);
    }
    if (i.XO.Rc) update_cr0(c, v);
}

XF_OP(addi) {
    // RT <- (RA|0) + EXTS(SI)
    c.r[i.D.RT] = (i.D.RA ? c.r[i.D.RA] : 0) + uint64_t(exts16(i.D.DS));
}

XF_OP(addis) {
    // RT <- (RA|0) + EXTS(SI || i16.0)
    c.r[i.D.RT] = (i.D.RA ? c.r[i.D.RA] : 0) + uint64_t(exts16(i.D.DS) << 16);
}

} // namespace xf::ppc::ops
