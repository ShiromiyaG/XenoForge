// A parsed shader laid out for the generic shaders (shader_program.h).

#include "xenoforge/shader_program.h"

#include <bit>
#include <variant>

namespace xf::shader {
namespace {

using namespace ucode;

uint32_t result_word(const Result &r) {
    uint32_t w = uint32_t(r.target) | (r.index & 127) << 4 | uint32_t(r.addressing) << 11 |
                 (r.clamped ? 1u : 0u) << 13 | r.used_write_mask() << 14;
    for (uint32_t k = 0; k < 4; k++) w |= (uint32_t(r.components[k]) & 7) << (18 + 3 * k);
    return w;
}

uint32_t operand_word(const Operand &o) {
    uint32_t w = (o.index & 511) | (o.source == StorageSource::ConstantFloat ? 1u : 0u) << 9 |
                 uint32_t(o.addressing) << 10 | (o.negated ? 1u : 0u) << 12 | (o.absolute ? 1u : 0u) << 13;
    for (uint32_t k = 0; k < 4; k++) w |= (uint32_t(o.component(k)) & 3) << (14 + 2 * k);
    return w;
}

uint32_t predication(bool predicated, bool condition) {
    return (predicated ? 1u : 0u) << 2 | (condition ? 1u : 0u) << 3;
}

// the vertex formats the translator fetches (shader_translator.cpp's vertex_fetch)
bool known_vertex_format(uint32_t format) {
    switch (format) {
    case 6: case 7: case 16: case 17: case 25: case 26: case 31: case 32: case 33: case 34: case 35: case 36:
    case 37: case 57: case 38: return true;
    default: return false;
    }
}

} // namespace

Program encode_program(const ParsedShader &shader) {
    Program p;
    auto fail = [&](std::string why) {
        if (p.unsupported.empty()) p.unsupported = std::move(why);
    };
    const uint32_t n = uint32_t(shader.control_flow.size());
    std::vector<uint32_t> cf(size_t(n) * 4, 0);
    std::vector<uint32_t> code;
    auto instruction = [&]() -> uint32_t * {
        code.resize(code.size() + 16, 0);
        return &code[code.size() - 16];
    };
    auto condition = [](uint32_t type, bool sense, uint32_t bool_constant) {
        return type << 4 | (sense ? 1u : 0u) << 6 | (bool_constant & 255) << 8;
    };
    for (uint32_t k = 0; k < n; k++) {
        uint32_t *entry = &cf[size_t(k) * 4];
        const ControlFlow &c = shader.control_flow[k];
        if (const auto *e = std::get_if<Exec>(&c.instruction)) {
            const uint32_t type = e->cf.type == ExecInstruction::Type::Conditional  ? 1
                                  : e->cf.type == ExecInstruction::Type::Predicated ? 2
                                                                                    : 0;
            entry[0] = CF_EXEC | condition(type, e->cf.condition, e->cf.bool_constant) | (e->cf.is_end ? 1u : 0u) << 16;
            entry[1] = uint32_t(code.size() / 16);
            entry[2] = uint32_t(e->slots.size());
            for (const ExecSlot &slot : e->slots) {
                uint32_t *w = instruction();
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction)) {
                    // (the flags from every ALU instruction, nops too, as the
                    // translator's scans take them)
                    for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result}) {
                        if (r->target == StorageTarget::ExportData && r->index < 5 && r->used_write_mask())
                            fail("a memory export");
                        const uint32_t mask = r->used_write_mask();
                        if (r->target == StorageTarget::Depth) p.flags |= PROGRAM_WRITES_DEPTH;
                        if (r->target == StorageTarget::Color) {
                            p.flags |= 1u << (PROGRAM_COLORS_SHIFT + (r->index & 3));
                            if ((r->index & 3) == 0 && mask) p.flags |= PROGRAM_WRITES_COLOR_0;
                        }
                        if (r->target == StorageTarget::PointSizeEdgeFlagKillVertex && (mask & 1))
                            p.flags |= PROGRAM_WRITES_POINT_SIZE;
                    }
                    if ((vector_opcode_info(a->vector_opcode).changed_state & CHANGES_PIXEL_KILL) ||
                        (scalar_opcode_info(a->scalar_opcode).changed_state & CHANGES_PIXEL_KILL))
                        p.flags |= PROGRAM_KILLS;
                    if (a->is_nop()) {
                        w[0] = 3;
                        continue;
                    }
                    if (a->scalar_opcode == AluScalarOpcode::Opcode41) fail("scalar opcode 41");
                    for (uint32_t o = 0; o < a->vector_operand_count; o++)
                        if (a->vector_operands[o].source != StorageSource::Register &&
                            a->vector_operands[o].source != StorageSource::ConstantFloat)
                            fail("an ALU operand neither a register nor a float constant");
                    for (uint32_t o = 0; o < a->scalar_operand_count; o++)
                        if (a->scalar_operands[o].source != StorageSource::Register &&
                            a->scalar_operands[o].source != StorageSource::ConstantFloat)
                            fail("an ALU operand neither a register nor a float constant");
                    const bool vector_runs = a->vector_and_constant_result.used_result_components() != 0 ||
                                             vector_opcode_info(a->vector_opcode).changed_state != 0;
                    w[0] = 0 | predication(a->is_predicated, a->predicate_condition) |
                           uint32_t(a->vector_opcode) << 4 | uint32_t(a->scalar_opcode) << 9 |
                           (vector_runs ? 1u : 0u) << 15 | (a->vector_operand_count & 3) << 17 |
                           (a->scalar_operand_count & 3) << 19;
                    w[1] = result_word(a->vector_and_constant_result);
                    w[2] = result_word(a->scalar_result);
                    for (uint32_t o = 0; o < 3; o++) w[3 + o] = operand_word(a->vector_operands[o]);
                    for (uint32_t o = 0; o < 2; o++) w[6 + o] = operand_word(a->scalar_operands[o]);
                } else if (const auto *v = std::get_if<VertexFetchInstruction>(&slot.instruction)) {
                    if (!known_vertex_format(v->data_format)) fail("vertex format " + std::to_string(v->data_format));
                    w[0] = 1 | predication(v->is_predicated, v->predicate_condition) | (v->is_mini_fetch ? 1u : 0u) << 4 |
                           (v->data_format & 63) << 5 | (v->is_signed ? 1u : 0u) << 11 |
                           (v->is_integer ? 1u : 0u) << 12 | (v->is_index_rounded ? 1u : 0u) << 13 |
                           (v->signed_rf_mode == SignedRepeatingFractionMode::NoZero ? 1u : 0u) << 14 |
                           (v->operands[1].index & 255) << 16;
                    w[1] = result_word(v->result);
                    w[2] = operand_word(v->operands[0]);
                    w[3] = uint32_t(v->offset);
                    w[4] = v->stride;
                    w[5] = uint32_t(v->exp_adjust);
                } else {
                    const auto &t = std::get<TextureFetchInstruction>(slot.instruction);
                    switch (t.opcode) {
                    case FetchOpcode::TextureFetch:
                    case FetchOpcode::GetTextureGradients:
                    case FetchOpcode::GetTextureComputedLod:
                    case FetchOpcode::GetTextureWeights:
                    case FetchOpcode::GetTextureBorderColorFrac:
                    case FetchOpcode::SetTextureLod:
                    case FetchOpcode::SetTextureGradientsHorz:
                    case FetchOpcode::SetTextureGradientsVert: break;
                    default: fail(std::string("texture fetch operation ") + t.opcode_name); break;
                    }
                    const bool point = t.mag_filter != TextureFilter::Linear && t.min_filter != TextureFilter::Linear &&
                                       t.mip_filter != TextureFilter::Linear &&
                                       (!t.use_computed_lod || t.aniso_filter == AnisoFilter::Disabled ||
                                        t.aniso_filter == AnisoFilter::UseFetchConst);
                    const bool one = t.dimension == FetchOpDimension::D1 && t.operands[0].component_count <= 1;
                    w[0] = 2 | predication(t.is_predicated, t.predicate_condition) | (uint32_t(t.opcode) & 31) << 4 |
                           uint32_t(t.dimension) << 9 | (t.unnormalized_coordinates ? 1u : 0u) << 11 |
                           (t.use_computed_lod ? 1u : 0u) << 12 | (t.use_register_lod ? 1u : 0u) << 13 |
                           (t.use_register_gradients ? 1u : 0u) << 14 | (point ? 1u : 0u) << 15 |
                           (t.non_zero_result_components() & 15) << 16 | (t.operands[1].index & 31) << 20 |
                           (one ? 1u : 0u) << 25 | (uint32_t(t.vol_mag_filter) & 3) << 26 |
                           (uint32_t(t.vol_min_filter) & 3) << 28 | (t.has_result() ? 1u : 0u) << 30;
                    w[1] = result_word(t.result);
                    w[2] = operand_word(t.operands[0]);
                    w[3] = std::bit_cast<uint32_t>(t.lod_bias);
                    w[4] = std::bit_cast<uint32_t>(t.offset_x);
                    w[5] = std::bit_cast<uint32_t>(t.offset_y);
                    w[6] = std::bit_cast<uint32_t>(t.offset_z);
                }
            }
        } else if (const auto *l = std::get_if<LoopStartInstruction>(&c.instruction)) {
            entry[0] = CF_LOOP_START | (l->is_repeat ? 1u : 0u) << 17 | (l->loop_constant & 31) << 20;
            entry[1] = l->skip_address;
        } else if (const auto *l = std::get_if<LoopEndInstruction>(&c.instruction)) {
            entry[0] = CF_LOOP_END | (l->is_predicated_break ? 1u : 0u) << 17 |
                       (l->predicate_condition ? 1u : 0u) << 18 | (l->loop_constant & 31) << 20;
            entry[1] = l->body_address;
        } else if (const auto *call = std::get_if<CallInstruction>(&c.instruction)) {
            const uint32_t type = call->type == CallInstruction::Type::Conditional  ? 1
                                  : call->type == CallInstruction::Type::Predicated ? 2
                                                                                    : 0;
            entry[0] = CF_CALL | condition(type, call->condition, call->bool_constant);
            entry[1] = call->target;
        } else if (std::holds_alternative<ReturnInstruction>(c.instruction)) {
            entry[0] = CF_RETURN;
        } else if (const auto *j = std::get_if<JumpInstruction>(&c.instruction)) {
            const uint32_t type = j->type == JumpInstruction::Type::Conditional  ? 1
                                  : j->type == JumpInstruction::Type::Predicated ? 2
                                                                                 : 0;
            entry[0] = CF_JUMP | condition(type, j->condition, j->bool_constant);
            entry[1] = j->target;
        } else {
            // nop, mark_vs_fetch_done, and alloc (which matters only to memory
            // exports, which the generic shaders do not run)
            entry[0] = CF_NOP;
        }
    }
    const uint32_t first = 1 + n;
    p.words.reserve(size_t(first) * 4 + code.size());
    p.words.insert(p.words.end(), {n, first, p.flags, 0u});
    p.words.insert(p.words.end(), cf.begin(), cf.end());
    // an exec's first instruction, from the instructions' start
    p.words.insert(p.words.end(), code.begin(), code.end());
    return p;
}

} // namespace xf::shader
