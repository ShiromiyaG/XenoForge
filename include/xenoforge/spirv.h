// A SPIR-V module written word by word: the sections in the order the
// specification lays them out, types and constants made once each, one
// function at a time with its variables gathered in its first block. The
// opcodes and enums are Khronos's (spirv.hpp11 from SPIRV-Headers, MIT).
// What the Xenos microcode translator (plan B2) writes its shaders with.

#pragma once

#include <spirv.hpp11>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace xf::spirv {

using Id = uint32_t;

class Module {
public:
    // SPIR-V 1.6, what Vulkan 1.3 takes
    explicit Module(uint32_t version = 0x00010600) : version_(version) {}

    Id id() { return next_id_++; }

    void capability(spv::Capability capability);
    void extension(std::string_view name);
    Id glsl_std_450(); // the extended instruction set, imported once
    void memory_model(spv::AddressingModel addressing, spv::MemoryModel memory);
    void entry_point(spv::ExecutionModel model, Id function, std::string_view name, std::span<const Id> interface);
    void execution_mode(Id function, spv::ExecutionMode mode, std::initializer_list<uint32_t> args = {});
    void name(Id target, std::string_view name);
    void member_name(Id type, uint32_t member, std::string_view name);
    void decorate(Id target, spv::Decoration decoration, std::initializer_list<uint32_t> args = {});
    void member_decorate(Id type, uint32_t member, spv::Decoration decoration, std::initializer_list<uint32_t> args = {});

    // Types, made once each, but for structures and runtime arrays: those
    // carry decorations of their own.
    Id type_void();
    Id type_bool();
    Id type_int(uint32_t width, bool is_signed);
    Id type_uint() { return type_int(32, false); }
    Id type_sint() { return type_int(32, true); }
    Id type_float(uint32_t width = 32);
    Id type_vector(Id component, uint32_t count);
    // stride 0: no ArrayStride (for Private and Function variables)
    Id type_array(Id element, uint32_t length, uint32_t stride = 0);
    Id type_runtime_array(Id element, uint32_t stride); // stride 0: none (arrays of images, samplers)
    Id type_struct(std::span<const Id> members);
    Id type_pointer(spv::StorageClass storage, Id pointee);
    Id type_function(Id result, std::span<const Id> parameters);
    Id type_image(Id sampled_type, spv::Dim dim, uint32_t depth, bool arrayed, bool multisampled, uint32_t sampled,
                  spv::ImageFormat format);
    Id type_sampled_image(Id image);
    Id type_sampler();

    // Constants, made once each.
    Id const_bool(bool value);
    Id const_uint(uint32_t value);
    Id const_int(int32_t value);
    Id const_float(float value);
    Id const_composite(Id type, std::span<const Id> parts);
    Id const_null(Id type);

    Id global_variable(Id pointer_type, spv::StorageClass storage, Id initializer = 0);

    // One function at a time.
    Id begin_function(Id result_type, Id function_type,
                      spv::FunctionControlMask control = spv::FunctionControlMask::MaskNone);
    Id function_parameter(Id type);
    Id local_variable(Id pointer_type, Id initializer = 0); // placed in the first block
    // Begins a block; pass an id made earlier (a branch's target) or none.
    Id label(Id id = 0);
    Id op(spv::Op opcode, Id result_type, std::initializer_list<uint32_t> operands);
    Id op(spv::Op opcode, Id result_type, std::span<const uint32_t> operands);
    void op_void(spv::Op opcode, std::initializer_list<uint32_t> operands = {});
    void op_void(spv::Op opcode, std::span<const uint32_t> operands);
    // An instruction of the GLSL.std.450 set.
    Id glsl(Id result_type, uint32_t instruction, std::initializer_list<Id> operands);
    void end_function();

    // The module's words; the module is spent.
    std::vector<uint32_t> finish();

private:
    static void emit(std::vector<uint32_t> &to, spv::Op opcode, std::span<const uint32_t> words);
    static void append_string(std::vector<uint32_t> &words, std::string_view s);
    // a type or constant: its opcode and operands (without the result id) are its key
    Id unique(spv::Op opcode, std::initializer_list<uint32_t> operands, bool has_result_type = false);
    Id unique(spv::Op opcode, std::span<const uint32_t> operands, bool has_result_type = false);

    uint32_t version_;
    Id next_id_ = 1;
    Id glsl_ = 0;
    std::set<uint32_t> capabilities_set_;
    std::vector<uint32_t> capabilities_, extensions_, imports_, memory_model_, entry_points_, execution_modes_, debug_,
        annotations_, globals_, functions_;
    std::map<std::vector<uint32_t>, Id> unique_;

    bool in_function_ = false, first_block_ = false;
    std::vector<uint32_t> function_head_, function_variables_, function_body_;
    size_t first_label_end_ = 0; // where the first block's variables go in the body
};

} // namespace xf::spirv
