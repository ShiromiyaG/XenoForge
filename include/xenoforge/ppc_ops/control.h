// Control (ppc_ops.h): the branch conditions the emitter and the
// interpreter test, CR logic, SPRs, traps, sync. Translated from Xenia's
// ppc_emit_control.cc (BSD; see LICENSE-xenia).

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

} // namespace xf::ppc::ops
