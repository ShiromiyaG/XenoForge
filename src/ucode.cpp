// Xenos microcode walked from its control flow. The layouts are Xenia's
// (src/xenia/gpu/ucode.h, BSD; see LICENSE-xenia): control flow
// instructions are 48 bits, two to three dwords, at the start of the program;
// the ALU and fetch instructions are three dwords each, reached through exec
// clauses whose sequence bits say which are fetches.

#include "xenoforge/ucode.h"

#include <algorithm>

namespace xf::ucode {
namespace {

constexpr uint32_t CF_EXEC = 1, CF_EXEC_END = 2, CF_COND_EXEC = 3, CF_COND_EXEC_END = 4, CF_COND_EXEC_PRED = 5,
                   CF_COND_EXEC_PRED_END = 6, CF_ALLOC = 12, CF_COND_EXEC_PRED_CLEAN = 13,
                   CF_COND_EXEC_PRED_CLEAN_END = 14;
constexpr uint32_t ALLOC_MEMORY = 3;

bool is_exec(uint32_t op) {
    return op == CF_EXEC || op == CF_EXEC_END || op == CF_COND_EXEC || op == CF_COND_EXEC_END ||
           op == CF_COND_EXEC_PRED || op == CF_COND_EXEC_PRED_END || op == CF_COND_EXEC_PRED_CLEAN ||
           op == CF_COND_EXEC_PRED_CLEAN_END;
}

void fetch(const uint32_t *ins, ShaderSummary &s) {
    uint32_t op = ins[0] & 0x1F;
    s.fetch++;
    s.fetch_ops[op]++;
    uint32_t const_index = (ins[0] >> 20) & 0x1F;
    if (op == 0) { // vfetch: three vertex fetch constants per texture slot
        s.vertex_slots.insert(const_index * 3 + ((ins[0] >> 25) & 3));
        s.vertex_formats[(ins[1] >> 16) & 0x3F]++;
    } else if (op == 1) { // tfetch
        s.texture_slots.insert(const_index);
        s.texture_dims[(ins[2] >> 14) & 3]++;
    } else if (op >= 16 && op <= 19) { // texture queries read the constant too
        s.texture_slots.insert(const_index);
    }
}

void alu(const uint32_t *ins, ShaderSummary &s) {
    s.alu++;
    uint32_t vector_op = (ins[2] >> 24) & 0x1F, scalar_op = (ins[0] >> 26) & 0x3F;
    uint32_t vector_mask = (ins[0] >> 16) & 0xF, scalar_mask = (ins[0] >> 20) & 0xF;
    // an operation counts when it writes something: a register, the
    // predicate, a kill, the address register
    bool vector_effect = vector_mask || (vector_op >= 20 && vector_op <= 27) || vector_op == 29;
    bool scalar_effect = scalar_mask || scalar_op == 23 || scalar_op == 24 || (scalar_op >= 27 && scalar_op <= 39);
    if (vector_effect) s.vector_ops[vector_op]++;
    if (scalar_effect) s.scalar_ops[scalar_op]++;
    if ((vector_effect && vector_op >= 24 && vector_op <= 27) || (scalar_effect && scalar_op >= 35 && scalar_op <= 39))
        s.kills = true;
}

} // namespace

ShaderSummary analyze(std::span<const uint32_t> dwords) {
    ShaderSummary s;
    s.dwords = uint32_t(dwords.size());
    // the control flow ends where the first exec's instructions start
    size_t pair_bound = dwords.size() / 3;
    for (size_t pair = 0; pair < pair_bound; pair++) {
        const uint32_t *w = dwords.data() + pair * 3;
        const uint32_t cf[2][2] = {{w[0], w[1] & 0xFFFF}, {(w[1] >> 16) | (w[2] << 16), w[2] >> 16}};
        for (const auto &c : cf) {
            uint32_t op = (c[1] >> 12) & 0xF;
            s.cf_ops[op]++;
            if (op == CF_ALLOC && ((c[1] >> 9) & 3) == ALLOC_MEMORY) s.memexport = true;
            if (!is_exec(op)) continue;
            uint32_t address = c[0] & 0xFFF, count = (c[0] >> 12) & 7, sequence = (c[0] >> 16) & 0xFFF;
            pair_bound = std::min<size_t>(pair_bound, address);
            for (uint32_t k = 0; k < count; k++) {
                size_t at = size_t(address + k) * 3;
                if (at + 3 > dwords.size()) {
                    s.truncated = true;
                    break;
                }
                if ((sequence >> (2 * k)) & 1) fetch(dwords.data() + at, s);
                else alu(dwords.data() + at, s);
            }
        }
    }
    return s;
}

} // namespace xf::ucode
