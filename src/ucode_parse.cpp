// Xenos microcode parsed. Translated from Xenia: the bit layouts of
// src/xenia/gpu/ucode.h, the opcode tables of ucode.cc, the Parse* functions
// and the Parsed* helpers of shader_translator.cc, and the walk of
// Shader::AnalyzeUcode (BSD; see LICENSE-xenia). Fields are read with shifts
// from the host-order dwords instead of bitfield structs.

#include "xenoforge/ucode_parse.h"

#include <algorithm>
#include <cstdint>

namespace xf::ucode {
namespace {

constexpr uint32_t CHANGES_NONE = 0;

constexpr AluScalarOpcodeInfo SCALAR_INFOS[64] = {
    {"adds", 1, true, CHANGES_NONE},
    {"adds_prev", 1, false, CHANGES_NONE},
    {"muls", 1, true, CHANGES_NONE},
    {"muls_prev", 1, false, CHANGES_NONE},
    {"muls_prev2", 1, true, CHANGES_NONE},
    {"maxs", 1, true, CHANGES_NONE},
    {"mins", 1, true, CHANGES_NONE},
    {"seqs", 1, false, CHANGES_NONE},
    {"sgts", 1, false, CHANGES_NONE},
    {"sges", 1, false, CHANGES_NONE},
    {"snes", 1, false, CHANGES_NONE},
    {"frcs", 1, false, CHANGES_NONE},
    {"truncs", 1, false, CHANGES_NONE},
    {"floors", 1, false, CHANGES_NONE},
    {"exp", 1, false, CHANGES_NONE},
    {"logc", 1, false, CHANGES_NONE},
    {"log", 1, false, CHANGES_NONE},
    {"rcpc", 1, false, CHANGES_NONE},
    {"rcpf", 1, false, CHANGES_NONE},
    {"rcp", 1, false, CHANGES_NONE},
    {"rsqc", 1, false, CHANGES_NONE},
    {"rsqf", 1, false, CHANGES_NONE},
    {"rsq", 1, false, CHANGES_NONE},
    {"maxas", 1, true, CHANGES_ADDRESS_REGISTER},
    {"maxasf", 1, true, CHANGES_ADDRESS_REGISTER},
    {"subs", 1, true, CHANGES_NONE},
    {"subs_prev", 1, false, CHANGES_NONE},
    {"setp_eq", 1, false, CHANGES_PREDICATE},
    {"setp_ne", 1, false, CHANGES_PREDICATE},
    {"setp_gt", 1, false, CHANGES_PREDICATE},
    {"setp_ge", 1, false, CHANGES_PREDICATE},
    {"setp_inv", 1, false, CHANGES_PREDICATE},
    {"setp_pop", 1, false, CHANGES_PREDICATE},
    {"setp_clr", 0, false, CHANGES_PREDICATE},
    {"setp_rstr", 1, false, CHANGES_PREDICATE},
    {"kills_eq", 1, false, CHANGES_PIXEL_KILL},
    {"kills_gt", 1, false, CHANGES_PIXEL_KILL},
    {"kills_ge", 1, false, CHANGES_PIXEL_KILL},
    {"kills_ne", 1, false, CHANGES_PIXEL_KILL},
    {"kills_one", 1, false, CHANGES_PIXEL_KILL},
    {"sqrt", 1, false, CHANGES_NONE},
    {"opcode_41", 0, false, CHANGES_NONE},
    {"mulsc", 2, false, CHANGES_NONE},
    {"mulsc", 2, false, CHANGES_NONE},
    {"addsc", 2, false, CHANGES_NONE},
    {"addsc", 2, false, CHANGES_NONE},
    {"subsc", 2, false, CHANGES_NONE},
    {"subsc", 2, false, CHANGES_NONE},
    {"sin", 1, false, CHANGES_NONE},
    {"cos", 1, false, CHANGES_NONE},
    {"retain_prev", 0, false, CHANGES_NONE},
    {"opcode_51", 0, false, CHANGES_NONE},
    {"opcode_52", 0, false, CHANGES_NONE},
    {"opcode_53", 0, false, CHANGES_NONE},
    {"opcode_54", 0, false, CHANGES_NONE},
    {"opcode_55", 0, false, CHANGES_NONE},
    {"opcode_56", 0, false, CHANGES_NONE},
    {"opcode_57", 0, false, CHANGES_NONE},
    {"opcode_58", 0, false, CHANGES_NONE},
    {"opcode_59", 0, false, CHANGES_NONE},
    {"opcode_60", 0, false, CHANGES_NONE},
    {"opcode_61", 0, false, CHANGES_NONE},
    {"opcode_62", 0, false, CHANGES_NONE},
    {"opcode_63", 0, false, CHANGES_NONE},
};

constexpr AluVectorOpcodeInfo VECTOR_INFOS[32] = {
    {"add", {0b1111, 0b1111}, CHANGES_NONE},
    {"mul", {0b1111, 0b1111}, CHANGES_NONE},
    {"max", {0b1111, 0b1111}, CHANGES_NONE},
    {"min", {0b1111, 0b1111}, CHANGES_NONE},
    {"seq", {0b1111, 0b1111}, CHANGES_NONE},
    {"sgt", {0b1111, 0b1111}, CHANGES_NONE},
    {"sge", {0b1111, 0b1111}, CHANGES_NONE},
    {"sne", {0b1111, 0b1111}, CHANGES_NONE},
    {"frc", {0b1111}, CHANGES_NONE},
    {"trunc", {0b1111}, CHANGES_NONE},
    {"floor", {0b1111}, CHANGES_NONE},
    {"mad", {0b1111, 0b1111, 0b1111}, CHANGES_NONE},
    {"cndeq", {0b1111, 0b1111, 0b1111}, CHANGES_NONE},
    {"cndge", {0b1111, 0b1111, 0b1111}, CHANGES_NONE},
    {"cndgt", {0b1111, 0b1111, 0b1111}, CHANGES_NONE},
    {"dp4", {0b1111, 0b1111}, CHANGES_NONE},
    {"dp3", {0b0111, 0b0111}, CHANGES_NONE},
    {"dp2add", {0b0011, 0b0011, 0b0001}, CHANGES_NONE},
    {"cube", {0b1111, 0b1111}, CHANGES_NONE},
    {"max4", {0b1111}, CHANGES_NONE},
    {"setp_eq_push", {0b1001, 0b1001}, CHANGES_PREDICATE},
    {"setp_ne_push", {0b1001, 0b1001}, CHANGES_PREDICATE},
    {"setp_gt_push", {0b1001, 0b1001}, CHANGES_PREDICATE},
    {"setp_ge_push", {0b1001, 0b1001}, CHANGES_PREDICATE},
    {"kill_eq", {0b1111, 0b1111}, CHANGES_PIXEL_KILL},
    {"kill_gt", {0b1111, 0b1111}, CHANGES_PIXEL_KILL},
    {"kill_ge", {0b1111, 0b1111}, CHANGES_PIXEL_KILL},
    {"kill_ne", {0b1111, 0b1111}, CHANGES_PIXEL_KILL},
    {"dst", {0b0110, 0b1010}, CHANGES_NONE},
    {"maxa", {0b1111, 0b1111}, CHANGES_ADDRESS_REGISTER},
    {"opcode_30", {}, CHANGES_NONE},
    {"opcode_31", {}, CHANGES_NONE},
};

// export registers (ucode.h's ExportRegister)
constexpr uint32_t EXPORT_VS_INTERPOLATOR_15 = 15, EXPORT_VS_POSITION = 62, EXPORT_VS_POINT_SIZE = 63,
                   EXPORT_PS_COLOR_3 = 3, EXPORT_PS_DEPTH = 61, EXPORT_ADDRESS = 32, EXPORT_DATA_0 = 33,
                   EXPORT_DATA_4 = 37;

uint32_t bits(uint32_t value, int shift, int count) { return (value >> shift) & ((1u << count) - 1); }

int32_t signed_bits(uint32_t value, int shift, int count) {
    return int32_t(bits(value, shift, count) << (32 - count)) >> (32 - count);
}

bool is_exec(ControlFlowOpcode op) {
    switch (op) {
    case ControlFlowOpcode::Exec:
    case ControlFlowOpcode::ExecEnd:
    case ControlFlowOpcode::CondExec:
    case ControlFlowOpcode::CondExecEnd:
    case ControlFlowOpcode::CondExecPred:
    case ControlFlowOpcode::CondExecPredEnd:
    case ControlFlowOpcode::CondExecPredClean:
    case ControlFlowOpcode::CondExecPredCleanEnd: return true;
    default: return false;
    }
}

// A 48-bit control flow instruction: d0 the low 32 bits, d1 the high 16.
struct Cf {
    uint32_t d0, d1;
    ControlFlowOpcode opcode() const { return ControlFlowOpcode(bits(d1, 12, 4)); }
};

void unpack(const uint32_t *dwords, Cf out[2]) {
    out[0] = {dwords[0], dwords[1] & 0xFFFF};
    out[1] = {(dwords[1] >> 16) | (dwords[2] << 16), dwords[2] >> 16};
}

// Every exec form keeps its instructions' address, count, yield and sequence alike.
void parse_exec_common(const Cf &cf, uint32_t cf_index, ExecInstruction &e) {
    e.cf_index = cf_index;
    e.opcode = cf.opcode();
    e.address = bits(cf.d0, 0, 12);
    e.count = bits(cf.d0, 12, 3);
    e.is_yield = bits(cf.d0, 15, 1);
    e.sequence = bits(cf.d0, 16, 12);
}

ExecInstruction parse_exec(const Cf &cf, uint32_t cf_index) {
    ExecInstruction e;
    parse_exec_common(cf, cf_index, e);
    e.opcode_name = e.opcode == ControlFlowOpcode::ExecEnd ? "exece" : "exec";
    e.type = ExecInstruction::Type::Unconditional;
    e.is_end = e.opcode == ControlFlowOpcode::ExecEnd;
    e.is_predicate_clean = bits(cf.d1, 9, 1);
    return e;
}

ExecInstruction parse_cond_exec(const Cf &cf, uint32_t cf_index) {
    ExecInstruction e;
    parse_exec_common(cf, cf_index, e);
    e.opcode_name = "cexec";
    if (e.opcode == ControlFlowOpcode::CondExecEnd || e.opcode == ControlFlowOpcode::CondExecPredCleanEnd) {
        e.opcode_name = "cexece";
        e.is_end = true;
    }
    e.type = ExecInstruction::Type::Conditional;
    e.bool_constant = bits(cf.d1, 2, 8);
    e.condition = bits(cf.d1, 10, 1);
    if (e.opcode == ControlFlowOpcode::CondExec || e.opcode == ControlFlowOpcode::CondExecEnd)
        e.is_predicate_clean = false;
    return e;
}

ExecInstruction parse_cond_exec_pred(const Cf &cf, uint32_t cf_index) {
    ExecInstruction e;
    parse_exec_common(cf, cf_index, e);
    e.opcode_name = e.opcode == ControlFlowOpcode::CondExecPredEnd ? "exece" : "exec";
    e.type = ExecInstruction::Type::Predicated;
    e.condition = bits(cf.d1, 10, 1);
    e.is_end = e.opcode == ControlFlowOpcode::CondExecPredEnd;
    e.is_predicate_clean = bits(cf.d1, 9, 1);
    return e;
}

LoopStartInstruction parse_loop_start(const Cf &cf, uint32_t cf_index) {
    return {cf_index, bits(cf.d0, 16, 5), bits(cf.d0, 13, 1) != 0, bits(cf.d0, 0, 13)};
}

LoopEndInstruction parse_loop_end(const Cf &cf, uint32_t cf_index) {
    return {cf_index, bits(cf.d0, 21, 1) != 0, bits(cf.d1, 10, 1) != 0, bits(cf.d0, 16, 5), bits(cf.d0, 0, 13)};
}

// Calls and jumps share their layout: target, unconditional, predicated, bool constant, condition.
template <typename T> T parse_branch(const Cf &cf, uint32_t cf_index) {
    T t;
    t.cf_index = cf_index;
    t.target = bits(cf.d0, 0, 13);
    if (bits(cf.d0, 13, 1)) {
        t.type = T::Type::Unconditional;
    } else if (bits(cf.d0, 14, 1)) {
        t.type = T::Type::Predicated;
        t.condition = bits(cf.d1, 10, 1);
    } else {
        t.type = T::Type::Conditional;
        t.bool_constant = bits(cf.d1, 2, 8);
        t.condition = bits(cf.d1, 10, 1);
    }
    return t;
}

AllocInstruction parse_alloc(const Cf &cf, uint32_t cf_index, bool is_vertex_shader) {
    return {cf_index, AllocType(bits(cf.d1, 9, 2)), int(bits(cf.d0, 0, 3)), is_vertex_shader};
}

uint32_t swizzled_component_index(uint32_t swizzle, uint32_t component) {
    return ((swizzle >> (2 * component)) + component) & 3;
}

void parse_fetch_result(uint32_t dest, uint32_t swizzle, bool relative, Result &result) {
    result.target = StorageTarget::Register;
    result.index = dest;
    result.clamped = false;
    result.addressing = relative ? StorageAddressing::LoopRelative : StorageAddressing::Absolute;
    result.original_write_mask = 0b1111;
    for (uint32_t i = 0; i < 4; i++) {
        Swizzle source = Swizzle::Zero;
        switch ((swizzle >> (3 * i)) & 0b111) {
        case 0: source = Swizzle::X; break;
        case 1: source = Swizzle::Y; break;
        case 2: source = Swizzle::Z; break;
        case 3: source = Swizzle::W; break;
        case 5: source = Swizzle::One; break;
        case 7: result.original_write_mask &= ~(1u << i); break;
        default: source = Swizzle::Zero; break; // 4, or the invalid 6
        }
        result.components[i] = source;
    }
}

// Returns whether the fetch is a full one, which later mini fetches inherit from.
bool parse_vertex_fetch(const uint32_t *op, const uint32_t *previous_full, VertexFetchInstruction &f) {
    bool mini = bits(op[1], 30, 1);
    f.opcode_name = mini ? "vfetch_mini" : "vfetch_full";
    f.is_mini_fetch = mini;
    f.is_predicated = bits(op[1], 31, 1);
    f.predicate_condition = bits(op[2], 31, 1);
    parse_fetch_result(bits(op[0], 12, 6), bits(op[1], 0, 12), bits(op[0], 18, 1), f.result);

    const uint32_t *full = mini ? previous_full : op;
    Operand &src = f.operands[f.operand_count++];
    src.source = StorageSource::Register;
    src.index = bits(full[0], 5, 6);
    src.addressing = bits(full[0], 11, 1) ? StorageAddressing::LoopRelative : StorageAddressing::Absolute;
    src.negated = false;
    src.absolute = false;
    src.component_count = 1;
    src.components[0] = Swizzle(bits(full[0], 30, 2));

    Operand &constant = f.operands[f.operand_count++];
    constant.source = StorageSource::VertexFetchConstant;
    constant.index = bits(full[0], 20, 5) * 3 + bits(full[0], 25, 2);

    f.data_format = bits(op[1], 16, 6);
    f.offset = signed_bits(op[2], 8, 23);
    f.stride = bits(full[2], 0, 8);
    f.exp_adjust = signed_bits(op[1], 24, 6);
    f.prefetch_count = bits(op[0], 27, 3);
    f.is_index_rounded = bits(full[1], 15, 1);
    f.is_signed = bits(op[1], 12, 1);
    f.is_integer = bits(op[1], 13, 1);
    f.signed_rf_mode = SignedRepeatingFractionMode(bits(op[1], 14, 1));
    return !mini;
}

void parse_texture_fetch(const uint32_t *op, TextureFetchInstruction &f) {
    struct Info {
        const char *name;
        bool has_dest, has_const, has_attributes;
        uint32_t override_component_count;
    } info;
    auto dimension = FetchOpDimension(bits(op[2], 14, 2));
    auto by_dimension = [&](const char *const (&names)[4]) { return names[uint32_t(dimension)]; };
    auto opcode = FetchOpcode(bits(op[0], 0, 5));
    switch (opcode) {
    case FetchOpcode::TextureFetch: {
        static const char *const names[] = {"tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"};
        info = {by_dimension(names), true, true, true, 0};
        break;
    }
    case FetchOpcode::GetTextureBorderColorFrac: {
        static const char *const names[] = {"getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"};
        info = {by_dimension(names), true, true, true, 0};
        break;
    }
    case FetchOpcode::GetTextureComputedLod: {
        static const char *const names[] = {"getCompTexLOD1D", "getCompTexLOD2D", "getCompTexLOD3D",
                                            "getCompTexLODCube"};
        info = {by_dimension(names), true, true, true, 0};
        break;
    }
    case FetchOpcode::GetTextureGradients: info = {"getGradients", true, true, true, 2}; break;
    case FetchOpcode::GetTextureWeights: {
        static const char *const names[] = {"getWeights1D", "getWeights2D", "getWeights3D", "getWeightsCube"};
        info = {by_dimension(names), true, true, true, 0};
        break;
    }
    case FetchOpcode::SetTextureLod: info = {"setTexLOD", false, false, false, 1}; break;
    case FetchOpcode::SetTextureGradientsHorz: info = {"setGradientH", false, false, false, 3}; break;
    case FetchOpcode::SetTextureGradientsVert: info = {"setGradientV", false, false, false, 3}; break;
    default: return; // Xenia asserts; the instruction stays as it was made
    }

    f.opcode = opcode;
    f.opcode_name = info.name;
    f.dimension = dimension;
    f.is_predicated = bits(op[1], 31, 1);
    f.predicate_condition = bits(op[2], 31, 1);
    if (info.has_dest) parse_fetch_result(bits(op[0], 12, 6), bits(op[1], 0, 12), bits(op[0], 18, 1), f.result);
    else f.result.target = StorageTarget::None;

    Operand &src = f.operands[f.operand_count++];
    src.source = StorageSource::Register;
    src.index = bits(op[0], 5, 6);
    src.addressing = bits(op[0], 11, 1) ? StorageAddressing::LoopRelative : StorageAddressing::Absolute;
    src.negated = false;
    src.absolute = false;
    src.component_count =
        info.override_component_count ? info.override_component_count : dimension_component_count(dimension);
    uint32_t src_swizzle = bits(op[0], 26, 6);
    if (opcode == FetchOpcode::TextureFetch && dimension == FetchOpDimension::D1) {
        // 1D may give XY for a 2D fetch constant (545407D4's UI, in Xenia's note)
        uint32_t x = src_swizzle & 3;
        if (((src_swizzle >> 2) & 3) != x || ((src_swizzle >> 4) & 3) != x) src.component_count = 2;
    }
    uint32_t swizzle = src_swizzle;
    for (uint32_t j = 0; j < src.component_count; j++, swizzle >>= 2) src.components[j] = Swizzle(swizzle & 3);

    if (info.has_const) {
        Operand &constant = f.operands[f.operand_count++];
        constant.source = StorageSource::TextureFetchConstant;
        constant.index = bits(op[0], 20, 5);
    }

    if (info.has_attributes) {
        f.fetch_valid_only = bits(op[0], 19, 1);
        f.unnormalized_coordinates = bits(op[0], 25, 1);
        f.mag_filter = TextureFilter(bits(op[1], 12, 2));
        f.min_filter = TextureFilter(bits(op[1], 14, 2));
        f.mip_filter = TextureFilter(bits(op[1], 16, 2));
        f.aniso_filter = AnisoFilter(bits(op[1], 18, 3));
        f.vol_mag_filter = TextureFilter(bits(op[1], 24, 2));
        f.vol_min_filter = TextureFilter(bits(op[1], 26, 2));
        f.use_computed_lod = bits(op[1], 28, 1);
        f.use_register_lod = bits(op[1], 29, 1);
        f.use_register_gradients = bits(op[2], 0, 1);
        f.lod_bias = float(signed_bits(op[2], 2, 7)) * (1.0f / 16.0f);
        f.offset_x = float(signed_bits(op[2], 16, 5)) * 0.5f;
        f.offset_y = float(signed_bits(op[2], 21, 5)) * 0.5f;
        f.offset_z = float(signed_bits(op[2], 26, 5)) * 0.5f;
    }
}

// The ALU instruction's fields, by source index 1-3 as ucode.h numbers them.
struct Alu {
    const uint32_t *w;
    bool is_export() const { return bits(w[0], 15, 1); }
    bool abs_constants() const { return bits(w[0], 7, 1); }
    uint32_t vector_dest() const { return bits(w[0], 0, 6); }
    bool vector_dest_relative() const { return bits(w[0], 6, 1); }
    uint32_t scalar_dest() const { return bits(w[0], 8, 6); }
    bool scalar_dest_relative() const { return bits(w[0], 14, 1); }
    uint32_t vector_write_mask() const { return bits(w[0], 16, 4); }
    uint32_t scalar_write_mask() const { return bits(w[0], 20, 4); }
    bool vector_clamp() const { return bits(w[0], 24, 1); }
    bool scalar_clamp() const { return bits(w[0], 25, 1); }
    uint32_t scalar_opcode() const { return bits(w[0], 26, 6); }
    uint32_t src_swizzle(uint32_t i) const { return bits(w[1], 8 * (3 - i), 8); }
    bool src_negate(uint32_t i) const { return bits(w[1], 24 + (3 - i), 1); }
    bool predicate_condition() const { return bits(w[1], 27, 1); }
    bool is_predicated() const { return bits(w[1], 28, 1); }
    bool const_address_register_relative() const { return bits(w[1], 29, 1); }
    bool const_1_addressed() const { return bits(w[1], 30, 1); }
    bool const_0_addressed() const { return bits(w[1], 31, 1); }
    uint32_t src_reg(uint32_t i) const { return bits(w[2], 8 * (3 - i), 8); }
    uint32_t vector_opcode() const { return bits(w[2], 24, 5); }
    bool src_is_temp(uint32_t i) const { return bits(w[2], 29 + (3 - i), 1); }
    bool src_const_is_addressed(uint32_t i) const {
        switch (i) {
        case 1: return const_0_addressed();
        case 2: return src_is_temp(1) ? const_0_addressed() : const_1_addressed();
        default: return (src_is_temp(1) && src_is_temp(2)) ? const_0_addressed() : const_1_addressed();
        }
    }
    uint32_t scalar_const_reg_op_src_temp_reg() const {
        return (scalar_opcode() & 1) | (uint32_t(src_is_temp(3)) << 1) | (src_swizzle(3) & 0x3C);
    }
    uint32_t vector_result_write_mask() const {
        uint32_t mask = vector_write_mask();
        if (is_export()) mask &= ~scalar_write_mask();
        return mask;
    }
    uint32_t scalar_result_write_mask() const {
        uint32_t mask = scalar_write_mask();
        if (is_export()) mask &= ~vector_write_mask();
        return mask;
    }
    uint32_t constant_0_write_mask() const {
        if (!is_export() || !scalar_dest_relative()) return 0;
        return 0b1111 & ~(vector_write_mask() | scalar_write_mask());
    }
    uint32_t constant_1_write_mask() const { return is_export() ? vector_write_mask() & scalar_write_mask() : 0; }
};

void parse_alu_operand(const Alu &op, uint32_t i, uint32_t swizzle_component_count, Operand &out,
                       uint32_t scalar_second_component = 0) {
    out.negated = op.src_negate(i);
    uint32_t reg = op.src_reg(i);
    if (op.src_is_temp(i)) {
        out.source = StorageSource::Register;
        out.index = reg & 0x3F;
        out.absolute = (reg & 0x80) != 0;
        out.addressing = (reg & 0x40) ? StorageAddressing::LoopRelative : StorageAddressing::Absolute;
    } else {
        out.source = StorageSource::ConstantFloat;
        out.index = reg;
        if (op.src_const_is_addressed(i))
            out.addressing = op.const_address_register_relative() ? StorageAddressing::AddressRegisterRelative
                                                                  : StorageAddressing::LoopRelative;
        else
            out.addressing = StorageAddressing::Absolute;
        out.absolute = op.abs_constants();
    }
    out.component_count = swizzle_component_count;
    uint32_t swizzle = op.src_swizzle(i);
    if (swizzle_component_count == 1) {
        out.components[0] = Swizzle(swizzled_component_index(swizzle, 3)); // scalar `a`: W
    } else if (swizzle_component_count == 2) {
        out.components[0] = Swizzle(swizzled_component_index(swizzle, 3));
        out.components[1] = Swizzle(swizzled_component_index(swizzle, scalar_second_component));
    } else if (swizzle_component_count == 4) {
        for (uint32_t j = 0; j < 4; j++) out.components[j] = Swizzle(swizzled_component_index(swizzle, j));
    }
}

void parse_alu(const uint32_t *w, ShaderType shader_type, AluInstruction &instr) {
    Alu op{w};
    instr.is_predicated = op.is_predicated();
    instr.predicate_condition = op.predicate_condition();

    bool is_export = op.is_export();
    StorageTarget target = StorageTarget::Register;
    uint32_t export_index = 0;
    if (is_export) {
        target = StorageTarget::None;
        // both operations export to vector_dest
        uint32_t reg = op.vector_dest();
        if (reg == EXPORT_ADDRESS) {
            target = StorageTarget::ExportAddress;
        } else if (reg >= EXPORT_DATA_0 && reg <= EXPORT_DATA_4) {
            target = StorageTarget::ExportData;
            export_index = reg - EXPORT_DATA_0;
        } else if (shader_type == ShaderType::Vertex) {
            if (reg <= EXPORT_VS_INTERPOLATOR_15) {
                target = StorageTarget::Interpolator;
                export_index = reg;
            } else if (reg == EXPORT_VS_POSITION) {
                target = StorageTarget::Position;
            } else if (reg == EXPORT_VS_POINT_SIZE) {
                target = StorageTarget::PointSizeEdgeFlagKillVertex;
            }
        } else {
            if (reg <= EXPORT_PS_COLOR_3) {
                target = StorageTarget::Color;
                export_index = reg;
            } else if (reg == EXPORT_PS_DEPTH) {
                target = StorageTarget::Depth;
            }
        }
    }

    // the vector operation, and an export's constant 0 and 1 components
    instr.vector_opcode = AluVectorOpcode(op.vector_opcode());
    const AluVectorOpcodeInfo &vector_info = vector_opcode_info(instr.vector_opcode);
    instr.vector_opcode_name = vector_info.name;
    Result &vr = instr.vector_and_constant_result;
    vr.target = target;
    vr.addressing = StorageAddressing::Absolute;
    if (is_export) {
        vr.index = export_index;
    } else {
        vr.index = op.vector_dest();
        if (op.vector_dest_relative()) vr.addressing = StorageAddressing::LoopRelative;
    }
    vr.clamped = op.vector_clamp();
    uint32_t constant_0 = op.constant_0_write_mask(), constant_1 = op.constant_1_write_mask();
    vr.original_write_mask = op.vector_result_write_mask() | constant_0 | constant_1;
    for (uint32_t i = 0; i < 4; i++)
        vr.components[i] = (constant_0 & (1u << i))   ? Swizzle::Zero
                            : (constant_1 & (1u << i)) ? Swizzle::One
                                                       : Swizzle(i);
    instr.vector_operand_count = vector_info.operand_count();
    for (uint32_t i = 0; i < instr.vector_operand_count; i++) parse_alu_operand(op, i + 1, 4, instr.vector_operands[i]);

    // the scalar operation
    instr.scalar_opcode = AluScalarOpcode(op.scalar_opcode());
    const AluScalarOpcodeInfo &scalar_info = scalar_opcode_info(instr.scalar_opcode);
    instr.scalar_opcode_name = scalar_info.name;
    Result &sr = instr.scalar_result;
    sr.target = target;
    sr.addressing = StorageAddressing::Absolute;
    if (is_export) {
        sr.index = export_index;
    } else {
        sr.index = op.scalar_dest();
        if (op.scalar_dest_relative()) sr.addressing = StorageAddressing::LoopRelative;
    }
    sr.clamped = op.scalar_clamp();
    sr.original_write_mask = op.scalar_result_write_mask();
    for (uint32_t i = 0; i < 4; i++) sr.components[i] = Swizzle(i);

    instr.scalar_operand_count = scalar_info.operand_count;
    if (instr.scalar_operand_count == 1) {
        // source 3 is shared with the vector operation: `b` is Z if the
        // vector operation uses source 3, else X
        parse_alu_operand(op, 3, scalar_info.single_operand_is_two_component ? 2 : 1, instr.scalar_operands[0],
                          vector_info.operand_count() == 3 ? 2 : 0);
    } else if (instr.scalar_operand_count == 2) {
        // a constant (`a`, W) and a temporary register (`b`, X)
        bool negate = op.src_negate(3);
        uint32_t swizzle = op.src_swizzle(3);
        Operand &c = instr.scalar_operands[0];
        c.negated = negate;
        c.absolute = op.abs_constants();
        c.source = StorageSource::ConstantFloat;
        c.index = op.src_reg(3);
        if (op.src_const_is_addressed(3))
            c.addressing = op.const_address_register_relative() ? StorageAddressing::AddressRegisterRelative
                                                                : StorageAddressing::LoopRelative;
        else
            c.addressing = StorageAddressing::Absolute;
        c.component_count = 1;
        c.components[0] = Swizzle(swizzled_component_index(swizzle, 3));

        Operand &t = instr.scalar_operands[1];
        t.negated = negate;
        t.absolute = op.abs_constants();
        t.source = StorageSource::Register;
        t.index = op.scalar_const_reg_op_src_temp_reg();
        t.addressing = StorageAddressing::Absolute;
        t.component_count = 1;
        t.components[0] = Swizzle(swizzled_component_index(swizzle, 0));
    }
}

} // namespace

uint32_t dimension_component_count(FetchOpDimension dimension) {
    switch (dimension) {
    case FetchOpDimension::D1: return 1;
    case FetchOpDimension::D2: return 2;
    default: return 3;
    }
}

const AluVectorOpcodeInfo &vector_opcode_info(AluVectorOpcode opcode) { return VECTOR_INFOS[uint32_t(opcode) & 31]; }
const AluScalarOpcodeInfo &scalar_opcode_info(AluScalarOpcode opcode) { return SCALAR_INFOS[uint32_t(opcode) & 63]; }

uint32_t used_component_count(StorageTarget target) {
    switch (target) {
    case StorageTarget::None: return 0;
    case StorageTarget::PointSizeEdgeFlagKillVertex: return 3;
    case StorageTarget::Depth: return 1;
    default: return 4;
    }
}

char swizzle_char(Swizzle s) {
    static const char chars[] = {'x', 'y', 'z', 'w', '0', '1'};
    return chars[uint32_t(s)];
}

uint32_t Result::used_write_mask() const { return original_write_mask & ((1u << used_component_count(target)) - 1); }

bool Result::is_standard_swizzle() const {
    return used_write_mask() == 0b1111 && components[0] == Swizzle::X && components[1] == Swizzle::Y &&
           components[2] == Swizzle::Z && components[3] == Swizzle::W;
}

uint32_t Result::used_result_components() const {
    uint32_t mask = used_write_mask(), used = 0;
    for (uint32_t i = 0; i < 4; i++)
        if ((mask & (1u << i)) && components[i] <= Swizzle::W) used |= 1u << uint32_t(components[i]);
    return used;
}

uint32_t Result::used_constant_components(uint32_t &constant_values) const {
    uint32_t constants = 0, values = 0, mask = used_write_mask();
    for (uint32_t i = 0; i < 4; i++) {
        if (!(mask & (1u << i)) || components[i] <= Swizzle::W) continue;
        constants |= 1u << i;
        if (components[i] == Swizzle::One) values |= 1u << i;
    }
    constant_values = values;
    return constants;
}

bool Operand::is_standard_swizzle() const {
    return component_count == 4 && components[0] == Swizzle::X && components[1] == Swizzle::Y &&
           components[2] == Swizzle::Z && components[3] == Swizzle::W;
}

uint32_t Operand::identical_components(const Operand &o) const {
    if (source != o.source || index != o.index || addressing != o.addressing || negated != o.negated ||
        absolute != o.absolute)
        return 0;
    uint32_t identical = 0;
    for (uint32_t i = 0; i < 4; i++) identical |= uint32_t(component(i) == o.component(i)) << i;
    return identical;
}

uint32_t TextureFetchInstruction::non_zero_result_components() const {
    uint32_t components = 0;
    switch (opcode) {
    case FetchOpcode::TextureFetch:
    case FetchOpcode::GetTextureGradients: components = 0b1111; break;
    case FetchOpcode::GetTextureBorderColorFrac:
    case FetchOpcode::GetTextureComputedLod: components = 0b0001; break;
    case FetchOpcode::GetTextureWeights:
        switch (dimension) {
        case FetchOpDimension::D1: components = 0b1001; break;
        case FetchOpDimension::D2:
        case FetchOpDimension::Cube: components = 0b1011; break;
        case FetchOpDimension::D3OrStacked: components = 0b1111; break;
        }
        if (mip_filter == TextureFilter::BaseMap || mip_filter == TextureFilter::Point) components &= ~0b1000u;
        break;
    default: components = 0; break;
    }
    return result.used_result_components() & components;
}

bool AluInstruction::is_vector_op_default_nop() const {
    const Operand *o = vector_operands;
    auto plain_r0 = [](const Operand &op) {
        return op.source == StorageSource::Register && op.index == 0 && op.addressing == StorageAddressing::Absolute &&
               !op.negated && !op.absolute && op.is_standard_swizzle();
    };
    const Result &r = vector_and_constant_result;
    if (vector_opcode != AluVectorOpcode::Max || r.original_write_mask || r.clamped || !plain_r0(o[0]) ||
        !plain_r0(o[1]))
        return false;
    if (r.target == StorageTarget::Register) {
        if (r.index != 0 || r.addressing != StorageAddressing::Absolute) return false;
    } else if (is_scalar_op_default_nop()) {
        // both nop: the vector operation stays to say the destination is an export
        return false;
    }
    return true;
}

bool AluInstruction::is_scalar_op_default_nop() const {
    const Result &r = scalar_result;
    if (scalar_opcode != AluScalarOpcode::RetainPrev || r.original_write_mask || r.clamped) return false;
    if (r.target == StorageTarget::Register && (r.index != 0 || r.addressing != StorageAddressing::Absolute))
        return false;
    return true;
}

bool AluInstruction::is_nop() const {
    return scalar_opcode == AluScalarOpcode::RetainPrev && !scalar_result.used_write_mask() &&
           !vector_and_constant_result.used_write_mask() && !vector_opcode_info(vector_opcode).changed_state;
}

uint32_t AluInstruction::memexport_stream_constant() const {
    const Result &r = vector_and_constant_result;
    const Operand &c = vector_operands[2];
    if (r.target == StorageTarget::ExportAddress && vector_opcode == AluVectorOpcode::Mad &&
        r.used_result_components() == 0b1111 && !r.clamped && c.source == StorageSource::ConstantFloat &&
        c.addressing == StorageAddressing::Absolute && c.is_standard_swizzle() && !c.negated && !c.absolute)
        return c.index;
    return UINT32_MAX;
}

bool awaits_vertex_declaration(const ParsedShader &shader) {
    if (shader.type != ShaderType::Vertex) return false;
    for (const ControlFlow &cf : shader.control_flow) {
        const Exec *exec = std::get_if<Exec>(&cf.instruction);
        if (!exec) continue;
        for (const ExecSlot &slot : exec->slots) {
            const auto *v = std::get_if<VertexFetchInstruction>(&slot.instruction);
            if (v && !v->is_mini_fetch && v->data_format == 0 && v->stride == 0) return true;
        }
    }
    return false;
}

ParsedShader parse_shader(ShaderType type, std::span<const uint32_t> dwords) {
    ParsedShader s;
    s.type = type;
    s.dwords.assign(dwords.begin(), dwords.end());

    // The control flow comes in pairs of three dwords at the top; it ends
    // where the first exec's instructions start (what freedreno does too).
    s.cf_pair_bound = uint32_t(s.dwords.size() / 3);
    for (uint32_t i = 0; i < s.cf_pair_bound; i++) {
        Cf cf[2];
        unpack(s.dwords.data() + i * 3, cf);
        for (const Cf &c : cf) {
            if (is_exec(c.opcode())) s.cf_pair_bound = std::min(s.cf_pair_bound, bits(c.d0, 0, 12));
            switch (c.opcode()) {
            case ControlFlowOpcode::CondCall:
            case ControlFlowOpcode::CondJmp:
            case ControlFlowOpcode::LoopStart:
            case ControlFlowOpcode::LoopEnd: s.label_addresses.insert(bits(c.d0, 0, 13)); break;
            default: break;
            }
        }
    }

    uint32_t previous_full[3] = {}; // the last full vertex fetch, which mini fetches inherit from
    for (uint32_t i = 0; i < s.cf_pair_bound; i++) {
        Cf cf[2];
        unpack(s.dwords.data() + i * 3, cf);
        for (uint32_t j = 0; j < 2; j++) {
            const Cf &c = cf[j];
            ControlFlow flow;
            flow.index = i * 2 + j;
            flow.opcode = c.opcode();
            Exec exec;
            bool has_exec = false;
            switch (c.opcode()) {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                exec.cf = parse_exec(c, flow.index);
                has_exec = true;
                break;
            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                exec.cf = parse_cond_exec(c, flow.index);
                has_exec = true;
                break;
            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                exec.cf = parse_cond_exec_pred(c, flow.index);
                has_exec = true;
                break;
            case ControlFlowOpcode::LoopStart: flow.instruction = parse_loop_start(c, flow.index); break;
            case ControlFlowOpcode::LoopEnd: flow.instruction = parse_loop_end(c, flow.index); break;
            case ControlFlowOpcode::CondCall: flow.instruction = parse_branch<CallInstruction>(c, flow.index); break;
            case ControlFlowOpcode::Return: flow.instruction = ReturnInstruction{flow.index}; break;
            case ControlFlowOpcode::CondJmp: flow.instruction = parse_branch<JumpInstruction>(c, flow.index); break;
            case ControlFlowOpcode::Alloc:
                flow.instruction = parse_alloc(c, flow.index, type == ShaderType::Vertex);
                break;
            default: break; // nop, mark_vs_fetch_done
            }
            if (has_exec) {
                uint32_t sequence = exec.cf.sequence;
                for (uint32_t a = exec.cf.address; a < exec.cf.address + exec.cf.count; a++, sequence >>= 2) {
                    uint32_t op[3] = {};
                    if (size_t(a) * 3 + 3 <= s.dwords.size()) std::copy_n(s.dwords.data() + size_t(a) * 3, 3, op);
                    else s.reads_past_end = true;
                    ExecSlot slot;
                    slot.address = a;
                    slot.serialize = (sequence & 0b10) != 0;
                    if (sequence & 0b01) {
                        if (bits(op[0], 0, 5) == uint32_t(FetchOpcode::VertexFetch)) {
                            VertexFetchInstruction f;
                            if (parse_vertex_fetch(op, previous_full, f)) std::copy_n(op, 3, previous_full);
                            slot.instruction = f;
                        } else {
                            TextureFetchInstruction f;
                            parse_texture_fetch(op, f);
                            slot.instruction = f;
                        }
                    } else {
                        AluInstruction alu;
                        parse_alu(op, type, alu);
                        slot.instruction = alu;
                    }
                    exec.slots.push_back(std::move(slot));
                }
                flow.instruction = std::move(exec);
            }
            s.control_flow.push_back(std::move(flow));
        }
    }
    return s;
}

} // namespace xf::ucode
