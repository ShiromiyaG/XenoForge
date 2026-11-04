// Xenos microcode made here, with what the games' shaders seen so far lack:
// conditional and predicated execs, a loop with a predicated break, a call
// and a jump, an alloc of memory exports, a full and a mini vertex fetch, a
// texture fetch with filters and offsets, constants addressed by a0, an
// absolute negated constant, exports to oPos, eA and eM0 with constant
// components; and a pixel shader with a kill and the color and depth
// exports. Checked field by field, and line by line against what Xenia's
// disassembler prints for them.

#include "check.h"
#include "xenoforge/ucode_parse.h"

#include <string>
#include <vector>

using namespace xf::ucode;

namespace {

struct Cf {
    uint32_t d0, d1; // the low 32 bits and the high 16
};

Cf exec(uint32_t opcode, uint32_t address, uint32_t count, uint32_t sequence, bool clean = true) {
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | opcode << 12};
}
Cf cond_exec(uint32_t address, uint32_t count, uint32_t sequence, uint32_t bool_constant, bool condition) {
    return {address | count << 12 | sequence << 16, bool_constant << 2 | uint32_t(condition) << 10 | 3u << 12};
}
Cf cond_exec_pred(uint32_t address, uint32_t count, uint32_t sequence, bool condition, bool clean) {
    return {address | count << 12 | sequence << 16, uint32_t(clean) << 9 | uint32_t(condition) << 10 | 5u << 12};
}
Cf loop_start(uint32_t skip, uint32_t loop, bool repeat) { return {skip | uint32_t(repeat) << 13 | loop << 16, 7u << 12}; }
Cf loop_end(uint32_t body, uint32_t loop, bool predicated_break, bool condition) {
    return {body | loop << 16 | uint32_t(predicated_break) << 21, uint32_t(condition) << 10 | 8u << 12};
}
Cf branch(uint32_t opcode, uint32_t target, bool unconditional, bool predicated, uint32_t bool_constant,
          bool condition) {
    return {target | uint32_t(unconditional) << 13 | uint32_t(predicated) << 14,
            bool_constant << 2 | uint32_t(condition) << 10 | opcode << 12};
}
Cf ret() { return {0, 10u << 12}; }
Cf alloc(uint32_t type, uint32_t size) { return {size, type << 9 | 12u << 12}; }
Cf nop() { return {0, 0}; }

void pack(std::vector<uint32_t> &out, Cf a, Cf b) {
    out.push_back(a.d0);
    out.push_back((a.d1 & 0xFFFF) | b.d0 << 16);
    out.push_back(b.d0 >> 16 | b.d1 << 16);
}

constexpr uint32_t XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9; // a fetch's destination swizzle

std::vector<uint32_t> vertex_shader() {
    std::vector<uint32_t> d;
    pack(d, exec(1, 5, 3, 0x1D), loop_start(5, 3, true));              // fetch, fetch (serialized), fetch
    pack(d, cond_exec(8, 1, 0, 7, true), cond_exec_pred(9, 1, 0, false, false));
    pack(d, loop_end(2, 3, true, true), branch(9, 8, false, false, 1, false)); // ccall !b1, L8
    pack(d, branch(11, 9, false, true, 0, true), alloc(3, 2));                // (p0) jmp L9
    pack(d, exec(2, 10, 3, 0), ret());
    // 5: vfetch_full r1, r0.x from vertex constant 2*3+1, 32_32_32_FLOAT, stride 3, rounded, prefetch 3
    d.insert(d.end(), {0u | 1u << 12 | 1u << 19 | 2u << 20 | 1u << 25 | 2u << 27, XYZW | 1u << 15 | 57u << 16, 3});
    // 6: vfetch_mini r2.xy__, 8_8_8_8, signed, integer, exp adjust -1, offset 4
    d.insert(d.end(), {0u | 2u << 12 | 1u << 19,
                       (0 | 1 << 3 | 7 << 6 | 7 << 9) | 1u << 12 | 1u << 13 | 6u << 16 | (0x3Fu & uint32_t(-1)) << 24 |
                           1u << 30,
                       4u << 8});
    // 7: tfetch2D r7, r1.zw, tf3: filters, LOD bias 0.5, offsets -1.5 and 1 (z is ignored in 2D)
    d.insert(d.end(), {1u | 1u << 5 | 7u << 12 | 3u << 20 | 0xEu << 26,
                       XYZW | 1u << 12 | 0u << 14 | 3u << 16 | 3u << 18 | 3u << 24 | 3u << 26 | 1u << 28,
                       8u << 2 | 1u << 14 | 0x1Du << 16 | 2u << 21 | 1u << 26});
    // 8: mul r3, r1, c[5+a0] + adds r4.x, r1.xy
    d.insert(d.end(), {3u | 4u << 8 | 0xFu << 16 | 1u << 20 | 0u << 26, 0x41u | 1u << 29 | 1u << 31,
                       1u | 5u << 8 | 1u << 16 | 1u << 24 | 1u << 29 | 1u << 31});
    // 9: (!p0) rcp r6.x, -c_abs[7].w, the vector half the assembler's nop
    d.insert(d.end(), {1u << 7 | 6u << 8 | 1u << 20 | 19u << 26, 1u << 24 | 1u << 28, 7u | 2u << 24 | 1u << 30 | 1u << 31});
    // 10: add oPos, r1, c0
    d.insert(d.end(), {62u | 1u << 15 | 0xFu << 16 | 50u << 26, 0, 1u << 16 | 0u << 24 | 1u << 31});
    // 11: mad eA, r1, c1, c2
    d.insert(d.end(), {32u | 1u << 15 | 0xFu << 16 | 50u << 26, 0, 2u | 1u << 8 | 1u << 16 | 11u << 24 | 1u << 31});
    // 12: mul eM0.x010, r1, r1: y and w constant 0 (the relative scalar bit), z constant 1
    d.insert(d.end(), {33u | 1u << 14 | 1u << 15 | 0x5u << 16 | 0x4u << 20 | 50u << 26, 0,
                       1u << 8 | 1u << 16 | 1u << 24 | 1u << 30 | 1u << 31});
    return d;
}

const std::string FETCH = "         "; // "   " and no predicate's "      "

void parses_and_disassembles_a_vertex_shader() {
    auto s = parse_shader(ShaderType::Vertex, vertex_shader());
    CHECK(s.cf_pair_bound == 5 && !s.reads_past_end);
    CHECK(s.label_addresses == std::set<uint32_t>({2, 5, 8, 9}));
    CHECK(s.control_flow.size() == 10);
    if (s.control_flow.size() != 10) return;

    const Exec &first = std::get<Exec>(s.control_flow[0].instruction);
    CHECK(first.cf.address == 5 && first.cf.count == 3 && first.slots.size() == 3);
    CHECK(!first.slots[0].serialize && first.slots[1].serialize);
    const auto &full = std::get<VertexFetchInstruction>(first.slots[0].instruction);
    CHECK(!full.is_mini_fetch && full.operands[1].index == 7 && full.stride == 3 && full.data_format == 57);
    CHECK(full.is_index_rounded && full.prefetch_count == 2);
    const auto &mini = std::get<VertexFetchInstruction>(first.slots[1].instruction);
    CHECK(mini.is_mini_fetch && mini.operands[1].index == 7 && mini.stride == 3 && mini.is_index_rounded);
    CHECK(mini.exp_adjust == -1 && mini.offset == 4 && mini.is_signed && mini.is_integer);
    CHECK(mini.result.original_write_mask == 0b0011 && mini.result.used_result_components() == 0b0011);
    const auto &tex = std::get<TextureFetchInstruction>(first.slots[2].instruction);
    CHECK(tex.opcode == FetchOpcode::TextureFetch && tex.dimension == FetchOpDimension::D2);
    CHECK(tex.operands[1].index == 3 && tex.operands[0].component_count == 2);
    CHECK(tex.lod_bias == 0.5f && tex.offset_x == -1.5f && tex.offset_y == 1.0f && tex.offset_z == 0.5f);
    CHECK(!tex.fetch_valid_only && tex.aniso_filter == AnisoFilter::Max4to1);

    const auto &loop = std::get<LoopStartInstruction>(s.control_flow[1].instruction);
    CHECK(loop.loop_constant == 3 && loop.skip_address == 5 && loop.is_repeat);
    const Exec &conditional = std::get<Exec>(s.control_flow[2].instruction);
    CHECK(conditional.cf.type == ExecInstruction::Type::Conditional && conditional.cf.bool_constant == 7);
    const auto &mul = std::get<AluInstruction>(conditional.slots[0].instruction);
    CHECK(mul.vector_operands[1].source == StorageSource::ConstantFloat &&
          mul.vector_operands[1].addressing == StorageAddressing::AddressRegisterRelative);
    CHECK(mul.scalar_operand_count == 1 && mul.scalar_operands[0].component_count == 2);
    const Exec &predicated = std::get<Exec>(s.control_flow[3].instruction);
    CHECK(predicated.cf.type == ExecInstruction::Type::Predicated && !predicated.cf.condition &&
          !predicated.cf.is_predicate_clean);
    const auto &rcp = std::get<AluInstruction>(predicated.slots[0].instruction);
    CHECK(rcp.is_predicated && !rcp.predicate_condition && rcp.is_vector_op_default_nop() && !rcp.is_nop());
    CHECK(rcp.scalar_operands[0].negated && rcp.scalar_operands[0].absolute);
    const auto &end = std::get<LoopEndInstruction>(s.control_flow[4].instruction);
    CHECK(end.is_predicated_break && end.predicate_condition && end.body_address == 2);
    const auto &call = std::get<CallInstruction>(s.control_flow[5].instruction);
    CHECK(call.type == CallInstruction::Type::Conditional && call.bool_constant == 1 && call.target == 8);
    const auto &jump = std::get<JumpInstruction>(s.control_flow[6].instruction);
    CHECK(jump.type == JumpInstruction::Type::Predicated && jump.condition && jump.target == 9);
    const auto &memory = std::get<AllocInstruction>(s.control_flow[7].instruction);
    CHECK(memory.type == AllocType::Memory && memory.count == 2);
    const Exec &last = std::get<Exec>(s.control_flow[8].instruction);
    CHECK(last.cf.is_end && last.slots.size() == 3);
    const auto &position = std::get<AluInstruction>(last.slots[0].instruction);
    CHECK(position.vector_and_constant_result.target == StorageTarget::Position && position.is_scalar_op_default_nop());
    const auto &address = std::get<AluInstruction>(last.slots[1].instruction);
    CHECK(address.vector_and_constant_result.target == StorageTarget::ExportAddress &&
          address.memexport_stream_constant() == 2);
    const auto &data = std::get<AluInstruction>(last.slots[2].instruction);
    uint32_t values = 0;
    CHECK(data.vector_and_constant_result.target == StorageTarget::ExportData &&
          data.vector_and_constant_result.used_constant_components(values) == 0b1110 && values == 0b0100);
    CHECK(std::holds_alternative<ReturnInstruction>(s.control_flow[9].instruction));

    std::string text = disassemble(s);
    const std::string expected[] = {
        "/*    0.0 */       exec\n",
        "/*    5   */ " + FETCH +
            "vfetch_full r1, r0.x, vf88, RoundIndex=true, DataFormat=FMT_32_32_32_FLOAT, Stride=3, PrefetchCount=3\n",
        "/*    6   */          serialize\n             " + FETCH +
            "vfetch_mini r2.xy__, ExpAdjust=-1, Offset=4, DataFormat=FMT_8_8_8_8, Signed=true, NumFormat=integer\n",
        "/*    7   */ " + FETCH +
            "tfetch2D r7, r1.zw, tf3, FetchValidOnly=false, MagFilter=linear, MinFilter=point, AnisoFilter=max4to1, "
            "LODBias=0.5, OffsetX=-1.5, OffsetY=1\n",
        "/*    0.1 */       loop i3, L5, Repeat=true\n",
        // a plain cexec is never predicate-clean in Xenia's parse
        "                label L2\n/*    1.0 */       cexec b7    // PredicateClean=false\n",
        "/*    8   */          mul r3, r1, c[5+a0]\n              +       adds r4.x___, r1.xy\n",
        "/*    1.1 */ (!p0) exec    // PredicateClean=false\n",
        "/*    9   */    (!p0) rcp r6.x___, -c_abs[7].w\n",
        "/*    2.0 */  (p0) endloop i3, L2\n",
        "                label L5\n/*    2.1 */       ccall !b1, L8\n",
        "/*    3.0 */  (p0) jmp L9\n",
        "/*    3.1 */       alloc export = 2\n",
        "                label L8\n/*    4.0 */       exece\n",
        "/*   10   */          add oPos, r1, c0\n",
        "/*   11   */          mad eA, r1, c1, c2\n",
        "/*   12   */          mul eM0.x010, r1, r1\n",
        "                label L9\n/*    4.1 */       ret\n",
    };
    size_t at = 0;
    for (const std::string &line : expected) {
        size_t found = text.find(line, at);
        CHECK(found != std::string::npos);
        if (found == std::string::npos) std::printf("  missing, in order: %s", line.c_str());
        else at = found + line.size();
    }
    CHECK(at == text.size()); // nothing else after the last line
}

void parses_a_pixel_shader() {
    std::vector<uint32_t> d;
    pack(d, exec(2, 1, 3, 0), nop());
    // kill_gt r1._, r2, c3: writes nothing but kills
    d.insert(d.end(), {1u | 50u << 26, 0, 3u << 8 | 2u << 16 | 25u << 24 | 1u << 31});
    // mul_sat oC0, r0, c1
    d.insert(d.end(), {0u | 1u << 15 | 0xFu << 16 | 1u << 24 | 50u << 26, 0, 1u << 8 | 0u << 16 | 1u << 24 | 1u << 31});
    // max oDepth.x, r0, r0: the depth export has one component
    d.insert(d.end(), {61u | 1u << 15 | 0xFu << 16 | 50u << 26, 0, 2u << 24 | 1u << 30 | 1u << 31});

    auto s = parse_shader(ShaderType::Pixel, d);
    const Exec &e = std::get<Exec>(s.control_flow[0].instruction);
    const auto &kill = std::get<AluInstruction>(e.slots[0].instruction);
    CHECK(kill.vector_opcode == AluVectorOpcode::KillGt && !kill.is_nop());
    CHECK(vector_opcode_info(kill.vector_opcode).changed_state & CHANGES_PIXEL_KILL);
    const auto &color = std::get<AluInstruction>(e.slots[1].instruction);
    CHECK(color.vector_and_constant_result.target == StorageTarget::Color && color.vector_and_constant_result.clamped);
    const auto &depth = std::get<AluInstruction>(e.slots[2].instruction);
    CHECK(depth.vector_and_constant_result.target == StorageTarget::Depth &&
          depth.vector_and_constant_result.used_write_mask() == 0b0001);

    std::string text = disassemble(s);
    CHECK(text.find("/*    1   */          kill_gt r1._, r2, c3\n") != std::string::npos);
    CHECK(text.find("/*    2   */          mul_sat oC0, r0, c1\n") != std::string::npos);
    CHECK(text.find("/*    3   */          max oDepth, r0, r0\n") != std::string::npos);
    CHECK(text.find("/*    0.1 */       cnop\n") != std::string::npos);
}

void reads_past_the_end_as_zeros() {
    std::vector<uint32_t> d;
    pack(d, exec(2, 1, 2, 0), nop()); // two instructions, one present
    d.insert(d.end(), {0, 0, 0});
    auto s = parse_shader(ShaderType::Vertex, d);
    CHECK(s.reads_past_end && std::get<Exec>(s.control_flow[0].instruction).slots.size() == 2);
}

} // namespace

int main() {
    parses_and_disassembles_a_vertex_shader();
    parses_a_pixel_shader();
    reads_past_the_end_as_zeros();
    return finish("ucode_parse");
}
