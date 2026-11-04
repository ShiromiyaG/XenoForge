// Xenos microcode made here: an alloc of memory exports and an exec of a
// texture fetch, a vertex fetch and an ALU instruction; then a kill, and an
// exec that points past the end.

#include "check.h"
#include "xenoforge/ucode.h"

#include <vector>

using namespace xf;

namespace {

struct Cf {
    uint32_t d0, d1; // 48 bits: d1 holds the top 16
};

Cf exec(uint32_t opcode, uint32_t address, uint32_t count, uint32_t sequence) {
    return {address | count << 12 | sequence << 16, opcode << 12};
}

Cf alloc_memory() { return {0, 3u << 9 | 12u << 12}; }

// two control flow instructions in three dwords, as the hardware packs them
void pack(std::vector<uint32_t> &out, Cf a, Cf b) {
    out.push_back(a.d0);
    out.push_back((a.d1 & 0xFFFF) | b.d0 << 16);
    out.push_back(b.d0 >> 16 | b.d1 << 16);
}

void reads_a_shader() {
    std::vector<uint32_t> code;
    // instruction 1 is a fetch, 2 a fetch, 3 an ALU instruction: sequence 0b000101
    pack(code, alloc_memory(), exec(2 /* exec_end */, 1, 3, 0x5));
    // tfetch from texture constant 7, a cube
    code.insert(code.end(), {1u | 7u << 20, 0, 3u << 14});
    // vfetch from vertex constant 2*3+1, format 32_32_32_32_FLOAT
    code.insert(code.end(), {0u | 2u << 20 | 1u << 25, 38u << 16, 0});
    // mad to a register, rsq to one component
    code.insert(code.end(), {0xFu << 16 | 1u << 20 | 22u << 26, 0, 11u << 24});

    auto s = ucode::analyze(code);
    CHECK(s.dwords == 12 && s.fetch == 2 && s.alu == 1 && !s.truncated);
    CHECK(s.cf_ops.at(12) == 1 && s.cf_ops.at(2) == 1);
    CHECK(s.memexport && !s.kills);
    CHECK(s.texture_slots == std::set<uint32_t>{7} && s.texture_dims.at(3) == 1);
    CHECK(s.vertex_slots == std::set<uint32_t>{7} && s.vertex_formats.at(38) == 1);
    CHECK(s.vector_ops.size() == 1 && s.vector_ops.at(11) == 1);
    CHECK(s.scalar_ops.size() == 1 && s.scalar_ops.at(22) == 1);
}

void kills_and_cut_short() {
    std::vector<uint32_t> code;
    pack(code, exec(1 /* exec */, 1, 1, 0), exec(2, 5, 1, 0)); // the second points past the end
    // kill_eq writes no register but counts
    code.insert(code.end(), {0, 0, 24u << 24});
    auto s = ucode::analyze(code);
    CHECK(s.kills && s.vector_ops.at(24) == 1);
    CHECK(s.truncated);
    CHECK(!s.memexport && s.fetch == 0 && s.alu == 1);
}

} // namespace

int main() {
    reads_a_shader();
    kills_and_cut_short();
    return finish("ucode");
}
