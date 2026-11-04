// The SPIR-V module writer. Layout and encoding from the SPIR-V
// specification (2.3 Physical Layout, 2.4 Logical Layout): a five-word
// header, then capabilities, extensions, imports, the memory model, entry
// points, execution modes, debug names, annotations, types with constants
// and global variables, and the functions.

#include "xenoforge/spirv.h"

#include <cstring>

namespace xf::spirv {

using spv::Op;

void Module::emit(std::vector<uint32_t> &to, Op opcode, std::span<const uint32_t> words) {
    to.push_back(uint32_t(words.size() + 1) << 16 | uint32_t(opcode));
    to.insert(to.end(), words.begin(), words.end());
}

void Module::append_string(std::vector<uint32_t> &words, std::string_view s) {
    // UTF-8, nul-terminated, padded to a word
    size_t n = s.size() / 4 + 1;
    size_t at = words.size();
    words.resize(at + n, 0);
    std::memcpy(words.data() + at, s.data(), s.size());
}

Id Module::unique(Op opcode, std::initializer_list<uint32_t> operands, bool has_result_type) {
    return unique(opcode, std::span<const uint32_t>(operands.begin(), operands.size()), has_result_type);
}

Id Module::unique(Op opcode, std::span<const uint32_t> operands, bool has_result_type) {
    std::vector<uint32_t> key{uint32_t(opcode)};
    key.insert(key.end(), operands.begin(), operands.end());
    auto it = unique_.find(key);
    if (it != unique_.end()) return it->second;
    Id result = id();
    std::vector<uint32_t> words;
    if (has_result_type) {
        // a constant: result type, result id, then the rest
        words.push_back(operands[0]);
        words.push_back(result);
        words.insert(words.end(), operands.begin() + 1, operands.end());
    } else {
        words.push_back(result);
        words.insert(words.end(), operands.begin(), operands.end());
    }
    emit(globals_, opcode, words);
    unique_.emplace(std::move(key), result);
    return result;
}

void Module::capability(spv::Capability capability) {
    if (!capabilities_set_.insert(uint32_t(capability)).second) return;
    const uint32_t w[] = {uint32_t(capability)};
    emit(capabilities_, Op::OpCapability, w);
}

void Module::extension(std::string_view name) {
    std::vector<uint32_t> w;
    append_string(w, name);
    emit(extensions_, Op::OpExtension, w);
}

Id Module::glsl_std_450() {
    if (glsl_) return glsl_;
    glsl_ = id();
    std::vector<uint32_t> w{glsl_};
    append_string(w, "GLSL.std.450");
    emit(imports_, Op::OpExtInstImport, w);
    return glsl_;
}

void Module::memory_model(spv::AddressingModel addressing, spv::MemoryModel memory) {
    memory_model_.clear();
    const uint32_t w[] = {uint32_t(addressing), uint32_t(memory)};
    emit(memory_model_, Op::OpMemoryModel, w);
}

void Module::entry_point(spv::ExecutionModel model, Id function, std::string_view name, std::span<const Id> interface) {
    std::vector<uint32_t> w{uint32_t(model), function};
    append_string(w, name);
    w.insert(w.end(), interface.begin(), interface.end());
    emit(entry_points_, Op::OpEntryPoint, w);
}

void Module::execution_mode(Id function, spv::ExecutionMode mode, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{function, uint32_t(mode)};
    w.insert(w.end(), args);
    emit(execution_modes_, Op::OpExecutionMode, w);
}

void Module::name(Id target, std::string_view name) {
    std::vector<uint32_t> w{target};
    append_string(w, name);
    emit(debug_, Op::OpName, w);
}

void Module::member_name(Id type, uint32_t member, std::string_view name) {
    std::vector<uint32_t> w{type, member};
    append_string(w, name);
    emit(debug_, Op::OpMemberName, w);
}

void Module::decorate(Id target, spv::Decoration decoration, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{target, uint32_t(decoration)};
    w.insert(w.end(), args);
    emit(annotations_, Op::OpDecorate, w);
}

void Module::member_decorate(Id type, uint32_t member, spv::Decoration decoration,
                             std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{type, member, uint32_t(decoration)};
    w.insert(w.end(), args);
    emit(annotations_, Op::OpMemberDecorate, w);
}

Id Module::type_void() { return unique(Op::OpTypeVoid, {}); }
Id Module::type_bool() { return unique(Op::OpTypeBool, {}); }
Id Module::type_int(uint32_t width, bool is_signed) { return unique(Op::OpTypeInt, {width, is_signed ? 1u : 0u}); }
Id Module::type_float(uint32_t width) { return unique(Op::OpTypeFloat, {width}); }
Id Module::type_vector(Id component, uint32_t count) { return unique(Op::OpTypeVector, {component, count}); }

Id Module::type_array(Id element, uint32_t length, uint32_t stride) {
    Id length_id = const_uint(length);
    if (!stride) return unique(Op::OpTypeArray, {element, length_id});
    // a strided array is its own type: the key carries the stride
    std::vector<uint32_t> key{uint32_t(Op::OpTypeArray), element, length_id, 0xFFFFFFFFu, stride};
    auto it = unique_.find(key);
    if (it != unique_.end()) return it->second;
    Id result = id();
    const uint32_t w[] = {result, element, length_id};
    emit(globals_, Op::OpTypeArray, w);
    decorate(result, spv::Decoration::ArrayStride, {stride});
    unique_.emplace(std::move(key), result);
    return result;
}

Id Module::type_runtime_array(Id element, uint32_t stride) {
    Id result = id();
    const uint32_t w[] = {result, element};
    emit(globals_, Op::OpTypeRuntimeArray, w);
    if (stride) decorate(result, spv::Decoration::ArrayStride, {stride});
    return result;
}

Id Module::type_struct(std::span<const Id> members) {
    Id result = id();
    std::vector<uint32_t> w{result};
    w.insert(w.end(), members.begin(), members.end());
    emit(globals_, Op::OpTypeStruct, w);
    return result;
}

Id Module::type_pointer(spv::StorageClass storage, Id pointee) {
    return unique(Op::OpTypePointer, {uint32_t(storage), pointee});
}

Id Module::type_function(Id result, std::span<const Id> parameters) {
    std::vector<uint32_t> operands{result};
    operands.insert(operands.end(), parameters.begin(), parameters.end());
    return unique(Op::OpTypeFunction, operands);
}

Id Module::type_image(Id sampled_type, spv::Dim dim, uint32_t depth, bool arrayed, bool multisampled, uint32_t sampled,
                      spv::ImageFormat format) {
    return unique(Op::OpTypeImage, {sampled_type, uint32_t(dim), depth, arrayed ? 1u : 0u, multisampled ? 1u : 0u,
                                    sampled, uint32_t(format)});
}

Id Module::type_sampled_image(Id image) { return unique(Op::OpTypeSampledImage, {image}); }
Id Module::type_sampler() { return unique(Op::OpTypeSampler, {}); }

Id Module::const_bool(bool value) {
    return unique(value ? Op::OpConstantTrue : Op::OpConstantFalse, {type_bool()}, true);
}
Id Module::const_uint(uint32_t value) { return unique(Op::OpConstant, {type_uint(), value}, true); }
Id Module::const_int(int32_t value) { return unique(Op::OpConstant, {type_sint(), uint32_t(value)}, true); }
Id Module::const_float(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return unique(Op::OpConstant, {type_float(), bits}, true);
}
Id Module::const_composite(Id type, std::span<const Id> parts) {
    std::vector<uint32_t> operands{type};
    operands.insert(operands.end(), parts.begin(), parts.end());
    return unique(Op::OpConstantComposite, operands, true);
}
Id Module::const_null(Id type) { return unique(Op::OpConstantNull, {type}, true); }

Id Module::global_variable(Id pointer_type, spv::StorageClass storage, Id initializer) {
    Id result = id();
    std::vector<uint32_t> w{pointer_type, result, uint32_t(storage)};
    if (initializer) w.push_back(initializer);
    emit(globals_, Op::OpVariable, w);
    return result;
}

Id Module::begin_function(Id result_type, Id function_type, spv::FunctionControlMask control) {
    in_function_ = true;
    first_block_ = true;
    function_head_.clear();
    function_variables_.clear();
    function_body_.clear();
    Id result = id();
    const uint32_t w[] = {result_type, result, uint32_t(control), function_type};
    emit(function_head_, Op::OpFunction, w);
    return result;
}

Id Module::function_parameter(Id type) {
    Id result = id();
    const uint32_t w[] = {type, result};
    emit(function_head_, Op::OpFunctionParameter, w);
    return result;
}

Id Module::local_variable(Id pointer_type, Id initializer) {
    Id result = id();
    std::vector<uint32_t> w{pointer_type, result, uint32_t(spv::StorageClass::Function)};
    if (initializer) w.push_back(initializer);
    emit(function_variables_, Op::OpVariable, w);
    return result;
}

Id Module::label(Id label_id) {
    if (!label_id) label_id = id();
    const uint32_t w[] = {label_id};
    emit(function_body_, Op::OpLabel, w);
    if (first_block_) {
        first_label_end_ = function_body_.size();
        first_block_ = false;
    }
    return label_id;
}

Id Module::op(Op opcode, Id result_type, std::initializer_list<uint32_t> operands) {
    return op(opcode, result_type, std::span<const uint32_t>(operands.begin(), operands.size()));
}

Id Module::op(Op opcode, Id result_type, std::span<const uint32_t> operands) {
    Id result = id();
    std::vector<uint32_t> w{result_type, result};
    w.insert(w.end(), operands.begin(), operands.end());
    emit(function_body_, opcode, w);
    return result;
}

void Module::op_void(Op opcode, std::initializer_list<uint32_t> operands) {
    op_void(opcode, std::span<const uint32_t>(operands.begin(), operands.size()));
}

void Module::op_void(Op opcode, std::span<const uint32_t> operands) { emit(function_body_, opcode, operands); }

Id Module::glsl(Id result_type, uint32_t instruction, std::initializer_list<Id> operands) {
    std::vector<uint32_t> w{glsl_std_450(), instruction};
    w.insert(w.end(), operands);
    return op(Op::OpExtInst, result_type, w);
}

void Module::end_function() {
    functions_.insert(functions_.end(), function_head_.begin(), function_head_.end());
    // the variables open the first block, right after its label
    functions_.insert(functions_.end(), function_body_.begin(), function_body_.begin() + first_label_end_);
    functions_.insert(functions_.end(), function_variables_.begin(), function_variables_.end());
    functions_.insert(functions_.end(), function_body_.begin() + first_label_end_, function_body_.end());
    const uint32_t none[1] = {};
    emit(functions_, Op::OpFunctionEnd, std::span<const uint32_t>(none, 0));
    in_function_ = false;
}

std::vector<uint32_t> Module::finish() {
    std::vector<uint32_t> out{spv::MagicNumber, version_, 0 /* generator */, next_id_ /* bound */, 0 /* schema */};
    for (const auto *section : {&capabilities_, &extensions_, &imports_, &memory_model_, &entry_points_,
                                &execution_modes_, &debug_, &annotations_, &globals_, &functions_})
        out.insert(out.end(), section->begin(), section->end());
    return out;
}

} // namespace xf::spirv
