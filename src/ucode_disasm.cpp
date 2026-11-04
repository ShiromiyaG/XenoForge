// Xenos microcode disassembled character for character as Xenia does it.
// Translated from Xenia's src/xenia/gpu/shader_translator_disasm.cc and the
// printing in Shader::AnalyzeUcode and GatherExecInformation
// (shader_translator.cc), BSD; see LICENSE-xenia.

#include "xenoforge/ucode_parse.h"

#include <cstdio>
#include <string>

namespace xf::ucode {
namespace {

void append_format(std::string &out, const char *format, auto... args) {
    char buffer[128];
    int n = std::snprintf(buffer, sizeof(buffer), format, args...);
    if (n > 0) out.append(buffer, size_t(n) < sizeof(buffer) ? size_t(n) : sizeof(buffer) - 1);
}

void append_index(std::string &out, StorageAddressing addressing, uint32_t index) {
    switch (addressing) {
    case StorageAddressing::Absolute: append_format(out, "%u", index); break;
    case StorageAddressing::AddressRegisterRelative: append_format(out, "[%u+a0]", index); break;
    case StorageAddressing::LoopRelative: append_format(out, "[%u+aL]", index); break;
    }
}

void result_operand(const Result &r, std::string &out) {
    bool indexed = false;
    switch (r.target) {
    case StorageTarget::Register: out += 'r', indexed = true; break;
    case StorageTarget::Interpolator: out += 'o', indexed = true; break;
    case StorageTarget::Position: out += "oPos"; break;
    case StorageTarget::PointSizeEdgeFlagKillVertex: out += "oPts"; break;
    case StorageTarget::ExportAddress: out += "eA"; break;
    case StorageTarget::ExportData: out += "eM", indexed = true; break;
    case StorageTarget::Color: out += "oC", indexed = true; break;
    case StorageTarget::Depth: out += "oDepth"; break;
    case StorageTarget::None: break;
    }
    if (indexed) append_index(out, r.addressing, r.index);
    // the microcode's own mask, components without an effect included
    if (!r.original_write_mask) {
        out += "._";
    } else if (r.original_write_mask != 0b1111 || r.components[0] != Swizzle::X || r.components[1] != Swizzle::Y ||
               r.components[2] != Swizzle::Z || r.components[3] != Swizzle::W) {
        out += '.';
        for (int i = 0; i < 4; i++) out += (r.original_write_mask & (1u << i)) ? swizzle_char(r.components[i]) : '_';
    }
}

void source_operand(const Operand &op, std::string &out) {
    if (op.negated) out += '-';
    switch (op.source) {
    case StorageSource::Register: out += 'r'; break;
    case StorageSource::ConstantFloat: out += 'c'; break;
    default: break; // fetch constants print as vf/tf by their instructions
    }
    if (op.absolute) out += "_abs";
    if (op.addressing == StorageAddressing::Absolute && op.absolute) append_format(out, "[%u]", op.index);
    else append_index(out, op.addressing, op.index);
    if (!op.is_standard_swizzle()) {
        out += '.';
        for (uint32_t j = 0; j < op.component_count; j++) out += swizzle_char(op.components[j]);
    }
}

void predicate_prefix(bool is_predicated, bool condition, std::string &out) {
    out += is_predicated ? (condition ? " (p0) " : "(!p0) ") : "      ";
}

void exec_line(const ExecInstruction &e, std::string &out) {
    switch (e.type) {
    case ExecInstruction::Type::Unconditional: append_format(out, "      %s", e.opcode_name); break;
    case ExecInstruction::Type::Predicated:
        out += e.condition ? " (p0) " : "(!p0) ";
        out += e.opcode_name;
        break;
    case ExecInstruction::Type::Conditional:
        append_format(out, "      %s %sb%u", e.opcode_name, e.condition ? "" : "!", e.bool_constant);
        break;
    }
    if (e.is_yield) {
        // `exec, Yield=true` would not assemble; `cexec b0, Yield=true` needs the comma
        if (e.type == ExecInstruction::Type::Conditional) out += ',';
        out += " Yield=true";
    }
    if (!e.is_predicate_clean) out += "    // PredicateClean=false";
    out += '\n';
}

void loop_start_line(const LoopStartInstruction &l, std::string &out) {
    append_format(out, "      loop i%u, L%u", l.loop_constant, l.skip_address);
    if (l.is_repeat) out += ", Repeat=true";
    out += '\n';
}

void loop_end_line(const LoopEndInstruction &l, std::string &out) {
    out += l.is_predicated_break ? (l.predicate_condition ? " (p0) " : "(!p0) ") : "      ";
    append_format(out, "endloop i%u, L%u\n", l.loop_constant, l.body_address);
}

template <typename T> void branch_line(const T &b, const char *name, std::string &out) {
    switch (b.type) {
    case T::Type::Unconditional: append_format(out, "      %s ", name); break;
    case T::Type::Predicated:
        out += b.condition ? " (p0) " : "(!p0) ";
        append_format(out, "%s ", name);
        break;
    case T::Type::Conditional:
        append_format(out, "      c%s ", name);
        if (!b.condition) out += '!';
        append_format(out, "b%u, ", b.bool_constant);
        break;
    }
    append_format(out, "L%u\n", b.target);
}

void alloc_line(const AllocInstruction &a, std::string &out) {
    out += "      alloc ";
    switch (a.type) {
    case AllocType::None: break;
    case AllocType::VsPosition: out += "position"; break;
    case AllocType::VsInterpolators: out += a.is_vertex_shader ? "interpolators" : "colors"; break;
    case AllocType::Memory: append_format(out, "export = %d", a.count); break;
    }
    out += '\n';
}

const char *vertex_format_name(uint32_t format) {
    switch (format) {
    case 6: return "FMT_8_8_8_8";
    case 7: return "FMT_2_10_10_10";
    case 16: return "FMT_10_11_11";
    case 17: return "FMT_11_11_10";
    case 25: return "FMT_16_16";
    case 26: return "FMT_16_16_16_16";
    case 31: return "FMT_16_16_FLOAT";
    case 32: return "FMT_16_16_16_16_FLOAT";
    case 33: return "FMT_32";
    case 34: return "FMT_32_32";
    case 35: return "FMT_32_32_32_32";
    case 36: return "FMT_32_FLOAT";
    case 37: return "FMT_32_32_FLOAT";
    case 38: return "FMT_32_32_32_32_FLOAT";
    case 57: return "FMT_32_32_32_FLOAT";
    }
    return "?"; // Xenia's table has no name for it
}

void vertex_fetch_line(const VertexFetchInstruction &f, std::string &out) {
    out += "   ";
    predicate_prefix(f.is_predicated, f.predicate_condition, out);
    out += f.opcode_name;
    out += ' ';
    result_operand(f.result, out);
    if (!f.is_mini_fetch) {
        out += ", ";
        source_operand(f.operands[0], out);
        append_format(out, ", vf%u", 95 - f.operands[1].index);
        if (f.is_index_rounded) out += ", RoundIndex=true";
    }
    if (f.exp_adjust) append_format(out, ", ExpAdjust=%d", f.exp_adjust);
    if (f.offset) append_format(out, ", Offset=%d", f.offset);
    if (f.data_format != 0) append_format(out, ", DataFormat=%s", vertex_format_name(f.data_format));
    if (!f.is_mini_fetch && f.stride) append_format(out, ", Stride=%u", f.stride);
    if (f.is_signed) out += ", Signed=true";
    if (f.is_integer) out += ", NumFormat=integer";
    if (f.prefetch_count) append_format(out, ", PrefetchCount=%u", f.prefetch_count + 1);
    out += '\n';
}

void texture_fetch_line(const TextureFetchInstruction &f, std::string &out) {
    static const char *const filters[] = {"point", "linear", "basemap", "keep"};
    static const char *const anisos[] = {"disabled", "max1to1", "max2to1", "max4to1", "max8to1", "max16to1", "keep"};
    out += "   ";
    predicate_prefix(f.is_predicated, f.predicate_condition, out);
    out += f.opcode_name;
    out += ' ';
    bool comma = false;
    if (f.has_result()) {
        result_operand(f.result, out);
        comma = true;
    }
    if (comma) out += ", ";
    source_operand(f.operands[0], out);
    if (f.operand_count > 1) {
        if (comma) out += ", ";
        append_format(out, "tf%u", f.operands[1].index);
    }
    if (!f.fetch_valid_only) out += ", FetchValidOnly=false";
    if (f.unnormalized_coordinates) out += ", UnnormalizedTextureCoords=true";
    auto filter = [&](const char *name, TextureFilter value) {
        if (value != TextureFilter::UseFetchConst) append_format(out, ", %s=%s", name, filters[uint32_t(value)]);
    };
    filter("MagFilter", f.mag_filter);
    filter("MinFilter", f.min_filter);
    filter("MipFilter", f.mip_filter);
    if (f.aniso_filter != AnisoFilter::UseFetchConst)
        append_format(out, ", AnisoFilter=%s", uint32_t(f.aniso_filter) < 7 ? anisos[uint32_t(f.aniso_filter)] : "?");
    filter("VolMagFilter", f.vol_mag_filter);
    filter("VolMinFilter", f.vol_min_filter);
    if (!f.use_computed_lod) out += ", UseComputedLOD=false";
    if (f.use_register_lod) out += ", UseRegisterLOD=true";
    if (f.use_register_gradients) out += ", UseRegisterGradients=true";
    if (f.lod_bias != 0.0f) append_format(out, ", LODBias=%g", double(f.lod_bias));
    uint32_t components = dimension_component_count(f.dimension);
    if (f.offset_x != 0.0f) append_format(out, ", OffsetX=%g", double(f.offset_x));
    if (components > 1 && f.offset_y != 0.0f) append_format(out, ", OffsetY=%g", double(f.offset_y));
    if (components > 2 && f.offset_z != 0.0f) append_format(out, ", OffsetZ=%g", double(f.offset_z));
    out += '\n';
}

void alu_lines(const AluInstruction &a, std::string &out) {
    bool vector_nop = a.is_vector_op_default_nop(), scalar_nop = a.is_scalar_op_default_nop();
    if (vector_nop && scalar_nop) {
        out += "   ";
        predicate_prefix(a.is_predicated, a.predicate_condition, out);
        out += "nop\n";
        return;
    }
    if (!vector_nop) {
        out += "   ";
        predicate_prefix(a.is_predicated, a.predicate_condition, out);
        out += a.vector_opcode_name;
        if (a.vector_and_constant_result.clamped) out += "_sat";
        out += ' ';
        result_operand(a.vector_and_constant_result, out);
        for (uint32_t i = 0; i < a.vector_operand_count; i++) {
            out += ", ";
            source_operand(a.vector_operands[i], out);
        }
        out += '\n';
    }
    if (!scalar_nop) {
        out += vector_nop ? "   " : "              + ";
        predicate_prefix(a.is_predicated, a.predicate_condition, out);
        out += a.scalar_opcode_name;
        if (a.scalar_result.clamped) out += "_sat";
        out += ' ';
        result_operand(a.scalar_result, out);
        for (uint32_t i = 0; i < a.scalar_operand_count; i++) {
            out += ", ";
            source_operand(a.scalar_operands[i], out);
        }
        out += '\n';
    }
}

struct FlowPrinter {
    std::string &out;
    void operator()(const std::monostate &) {}
    void operator()(const Exec &e) {
        exec_line(e.cf, out);
        for (const ExecSlot &slot : e.slots) {
            append_format(out, "/* %4u   */ ", slot.address);
            if (slot.serialize) out += "         serialize\n             ";
            if (auto *alu = std::get_if<AluInstruction>(&slot.instruction)) alu_lines(*alu, out);
            else if (auto *vf = std::get_if<VertexFetchInstruction>(&slot.instruction)) vertex_fetch_line(*vf, out);
            else texture_fetch_line(std::get<TextureFetchInstruction>(slot.instruction), out);
        }
    }
    void operator()(const LoopStartInstruction &l) { loop_start_line(l, out); }
    void operator()(const LoopEndInstruction &l) { loop_end_line(l, out); }
    void operator()(const CallInstruction &c) { branch_line(c, "call", out); }
    void operator()(const ReturnInstruction &) { out += "      ret\n"; }
    void operator()(const JumpInstruction &j) { branch_line(j, "jmp", out); }
    void operator()(const AllocInstruction &a) { alloc_line(a, out); }
};

} // namespace

std::string disassemble(const ParsedShader &shader) {
    std::string out;
    for (const ControlFlow &flow : shader.control_flow) {
        if (shader.label_addresses.count(flow.index)) append_format(out, "                label L%u\n", flow.index);
        append_format(out, "/* %4u.%u */ ", flow.index / 2, flow.index % 2);
        if (flow.opcode == ControlFlowOpcode::Nop) {
            out += "      cnop\n";
            continue;
        }
        // mark_vs_fetch_done prints nothing after its prefix, as in Xenia
        std::visit(FlowPrinter{out}, flow.instruction);
    }
    return out;
}

} // namespace xf::ucode
