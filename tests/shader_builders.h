// Parsed Xenos instructions built by hand for the translator's tests: the
// same structures the parser fills (ucode_parse.h), field by field.

#pragma once

#include "xenoforge/ucode_parse.h"

#include <initializer_list>
#include <variant>
#include <vector>

namespace xf::ucode::build {

inline Swizzle sw(char c) { return c == 'x' ? Swizzle::X : c == 'y' ? Swizzle::Y : c == 'z' ? Swizzle::Z : c == 'w' ? Swizzle::W : c == '0' ? Swizzle::Zero : Swizzle::One; }

inline Operand reg(uint32_t index, const char *swizzle = "xyzw", bool negated = false, bool absolute = false) {
    Operand o;
    o.source = StorageSource::Register;
    o.index = index;
    o.negated = negated;
    o.absolute = absolute;
    for (int k = 0; k < 4; k++) o.components[k] = sw(swizzle[k]);
    return o;
}

inline Operand con(uint32_t index, const char *swizzle = "xyzw", StorageAddressing addressing = StorageAddressing::Absolute) {
    Operand o = reg(index, swizzle);
    o.source = StorageSource::ConstantFloat;
    o.addressing = addressing;
    return o;
}

inline Result dst(StorageTarget target, uint32_t index, uint32_t mask, const char *swizzle = "xyzw", bool clamped = false) {
    Result r;
    r.target = target;
    r.index = index;
    r.original_write_mask = mask;
    r.clamped = clamped;
    for (int k = 0; k < 4; k++) r.components[k] = sw(swizzle[k]);
    return r;
}

inline AluInstruction vector(AluVectorOpcode op, Result result, std::initializer_list<Operand> operands) {
    AluInstruction a;
    a.vector_opcode = op;
    a.vector_opcode_name = vector_opcode_info(op).name;
    a.vector_and_constant_result = result;
    a.vector_operand_count = uint32_t(operands.size());
    uint32_t k = 0;
    for (const Operand &o : operands) a.vector_operands[k++] = o;
    // the scalar half does nothing
    a.scalar_opcode = AluScalarOpcode::RetainPrev;
    a.scalar_opcode_name = scalar_opcode_info(a.scalar_opcode).name;
    return a;
}

inline AluInstruction scalar(AluScalarOpcode op, Result result, std::initializer_list<Operand> operands) {
    AluInstruction a;
    a.scalar_opcode = op;
    a.scalar_opcode_name = scalar_opcode_info(op).name;
    a.scalar_result = result;
    a.scalar_operand_count = uint32_t(operands.size());
    uint32_t k = 0;
    for (const Operand &o : operands) a.scalar_operands[k++] = o;
    // the vector half writes nothing and changes nothing
    a.vector_opcode = AluVectorOpcode::Add;
    a.vector_opcode_name = vector_opcode_info(a.vector_opcode).name;
    a.vector_operand_count = 2;
    a.vector_operands[0] = a.vector_operands[1] = reg(0);
    return a;
}

using Slot = std::variant<AluInstruction, VertexFetchInstruction, TextureFetchInstruction>;

inline Exec make_exec(std::vector<Slot> slots, bool end, ExecInstruction::Type type = ExecInstruction::Type::Unconditional,
               uint32_t bool_constant = 0, bool condition = true) {
    Exec e;
    e.cf.opcode = end ? (type == ExecInstruction::Type::Conditional ? ControlFlowOpcode::CondExecEnd : ControlFlowOpcode::ExecEnd)
                      : (type == ExecInstruction::Type::Conditional ? ControlFlowOpcode::CondExec : ControlFlowOpcode::Exec);
    e.cf.type = type;
    e.cf.is_end = end;
    e.cf.bool_constant = bool_constant;
    e.cf.condition = condition;
    e.cf.count = uint32_t(slots.size());
    for (size_t k = 0; k < slots.size(); k++) e.slots.push_back({uint32_t(k), false, slots[k]});
    return e;
}

inline ParsedShader make_shader(ShaderType type, std::vector<Exec> execs) {
    ParsedShader s;
    s.type = type;
    uint32_t index = 0;
    for (Exec &e : execs) {
        e.cf.cf_index = index;
        s.control_flow.push_back({index++, e.cf.opcode, std::move(e)});
    }
    s.cf_pair_bound = (index + 1) / 2;
    return s;
}

} // namespace xf::ucode::build
