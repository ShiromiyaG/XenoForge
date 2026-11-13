// The SPIR-V writer on two modules built here and checked by the Vulkan
// SDK's spirv-val for Vulkan 1.3: a compute shader over storage buffers, and
// a fragment shader with a texture, a branch and a loop. Types and constants
// come out once each.

#include "check.h"
#include "xenoforge/spirv.h"

#include <GLSL.std.450.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace xf::spirv;
using spv::Op;

namespace {

// spirv-val on the words; true when it accepts them (or is not installed).
// A module judged against expectations is kept in the temporary folder.
bool validates(const std::vector<uint32_t> &words, const char *name, bool expected = true) {
#ifdef XF_SPIRV_VAL
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(words.data()), std::streamsize(words.size() * 4));
    std::string command = std::string("\"") + XF_SPIRV_VAL + "\" --target-env vulkan1.3 \"" + path.string() + "\"";
#ifdef _WIN32
    // cmd.exe takes the line's first and last quotes away
    command = "\"" + command + (expected ? "" : " 2>nul") + "\"";
#else
    if (!expected) command += " 2>/dev/null";
#endif
    bool valid = std::system(command.c_str()) == 0;
    if (valid == expected) std::filesystem::remove(path);
    else std::printf("  spirv-val %s %s (kept)\n", valid ? "accepted" : "refused", path.string().c_str());
    return valid;
#else
    (void)words, (void)expected;
    std::printf("  %s: spirv-val not found, not validated\n", name);
    return expected;
#endif
}

// the check itself refuses: a module without its memory model
void refuses_a_broken_module() {
    Module m;
    m.capability(spv::Capability::Shader);
    CHECK(!validates(m.finish(), "xenoforge_spirv_broken.spv", false));
}

void compute() {
    Module m;
    m.capability(spv::Capability::Shader);
    m.capability(spv::Capability::Shader); // once
    m.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    Id u32 = m.type_uint(), v3 = m.type_vector(u32, 3);
    CHECK(m.type_uint() == u32 && m.type_vector(u32, 3) == v3);
    CHECK(m.const_uint(2) == m.const_uint(2) && m.const_uint(2) != m.const_uint(3));

    // two storage buffers of uints: in at binding 0, out at binding 1
    Id array = m.type_runtime_array(u32, 4);
    const Id members[] = {array};
    Id block = m.type_struct(members);
    m.decorate(block, spv::Decoration::Block);
    m.member_decorate(block, 0, spv::Decoration::Offset, {0});
    Id block_ptr = m.type_pointer(spv::StorageClass::StorageBuffer, block);
    Id in = m.global_variable(block_ptr, spv::StorageClass::StorageBuffer);
    Id out = m.global_variable(block_ptr, spv::StorageClass::StorageBuffer);
    m.decorate(in, spv::Decoration::DescriptorSet, {0});
    m.decorate(in, spv::Decoration::Binding, {0});
    m.decorate(out, spv::Decoration::DescriptorSet, {0});
    m.decorate(out, spv::Decoration::Binding, {1});
    m.decorate(in, spv::Decoration::NonWritable);
    Id gid = m.global_variable(m.type_pointer(spv::StorageClass::Input, v3), spv::StorageClass::Input);
    m.decorate(gid, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::GlobalInvocationId)});
    m.name(in, "in");
    m.name(out, "out");

    Id void_type = m.type_void();
    Id main = m.begin_function(void_type, m.type_function(void_type, {}));
    m.label();
    Id uint_ptr = m.type_pointer(spv::StorageClass::StorageBuffer, u32);
    Id x = m.op(Op::OpCompositeExtract, u32, {m.op(Op::OpLoad, v3, {gid}), 0});
    Id value = m.op(Op::OpLoad, u32, {m.op(Op::OpAccessChain, uint_ptr, {in, m.const_uint(0), x})});
    Id result = m.op(Op::OpIAdd, u32, {m.op(Op::OpIMul, u32, {value, m.const_uint(2)}), m.const_uint(1)});
    m.op_void(Op::OpStore, {m.op(Op::OpAccessChain, uint_ptr, {out, m.const_uint(0), x}), result});
    m.op_void(Op::OpReturn);
    m.end_function();
    const Id interface[] = {in, out, gid};
    m.entry_point(spv::ExecutionModel::GLCompute, main, "main", interface);
    m.execution_mode(main, spv::ExecutionMode::LocalSize, {64, 1, 1});

    auto words = m.finish();
    CHECK(words.size() > 5 && words[0] == spv::MagicNumber && words[1] == 0x00010600);
    CHECK(validates(words, "xenoforge_spirv_compute.spv"));
}

void fragment() {
    Module m;
    m.capability(spv::Capability::Shader);
    m.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    Id f32 = m.type_float(), v2 = m.type_vector(f32, 2), v4 = m.type_vector(f32, 4), b = m.type_bool();
    Id i32 = m.type_sint();

    Id coord = m.global_variable(m.type_pointer(spv::StorageClass::Input, v2), spv::StorageClass::Input);
    m.decorate(coord, spv::Decoration::Location, {0});
    Id color = m.global_variable(m.type_pointer(spv::StorageClass::Output, v4), spv::StorageClass::Output);
    m.decorate(color, spv::Decoration::Location, {0});
    Id image = m.type_image(f32, spv::Dim::Dim2D, 0, false, false, 1, spv::ImageFormat::Unknown);
    Id texture = m.global_variable(m.type_pointer(spv::StorageClass::UniformConstant, image),
                                   spv::StorageClass::UniformConstant);
    Id sampler = m.global_variable(m.type_pointer(spv::StorageClass::UniformConstant, m.type_sampler()),
                                   spv::StorageClass::UniformConstant);
    m.decorate(texture, spv::Decoration::DescriptorSet, {0});
    m.decorate(texture, spv::Decoration::Binding, {0});
    m.decorate(sampler, spv::Decoration::DescriptorSet, {0});
    m.decorate(sampler, spv::Decoration::Binding, {1});

    Id void_type = m.type_void();
    Id main = m.begin_function(void_type, m.type_function(void_type, {}));
    m.label();
    // a Function variable declared late still lands in the first block
    Id uv = m.op(Op::OpLoad, v2, {coord});
    Id sampled = m.op(Op::OpSampledImage, m.type_sampled_image(image),
                      {m.op(Op::OpLoad, image, {texture}), m.op(Op::OpLoad, m.type_sampler(), {sampler})});
    Id texel = m.op(Op::OpImageSampleImplicitLod, v4, {sampled, uv});
    Id result_var = m.local_variable(m.type_pointer(spv::StorageClass::Function, v4));
    m.op_void(Op::OpStore, {result_var, texel});

    // if (texel.x > 0.5) result = sqrt(result)
    Id then_block = m.id(), merge = m.id();
    Id x = m.op(Op::OpCompositeExtract, f32, {texel, 0});
    Id greater = m.op(Op::OpFOrdGreaterThan, b, {x, m.const_float(0.5f)});
    m.op_void(Op::OpSelectionMerge, {merge, uint32_t(spv::SelectionControlMask::MaskNone)});
    m.op_void(Op::OpBranchConditional, {greater, then_block, merge});
    m.label(then_block);
    m.op_void(Op::OpStore, {result_var, m.glsl(v4, GLSLstd450Sqrt, {m.op(Op::OpLoad, v4, {result_var})})});
    m.op_void(Op::OpBranch, {merge});
    m.label(merge);

    // for (i = 0; i < 4; i++) result *= 0.5
    Id i_var = m.local_variable(m.type_pointer(spv::StorageClass::Function, i32), m.const_int(0));
    Id header = m.id(), check = m.id(), body = m.id(), cont = m.id(), done = m.id();
    m.op_void(Op::OpBranch, {header});
    m.label(header);
    m.op_void(Op::OpLoopMerge, {done, cont, uint32_t(spv::LoopControlMask::MaskNone)});
    m.op_void(Op::OpBranch, {check});
    m.label(check);
    Id less = m.op(Op::OpSLessThan, b, {m.op(Op::OpLoad, i32, {i_var}), m.const_int(4)});
    m.op_void(Op::OpBranchConditional, {less, body, done});
    m.label(body);
    m.op_void(Op::OpStore, {result_var, m.op(Op::OpVectorTimesScalar, v4,
                                              {m.op(Op::OpLoad, v4, {result_var}), m.const_float(0.5f)})});
    m.op_void(Op::OpBranch, {cont});
    m.label(cont);
    m.op_void(Op::OpStore, {i_var, m.op(Op::OpIAdd, i32, {m.op(Op::OpLoad, i32, {i_var}), m.const_int(1)})});
    m.op_void(Op::OpBranch, {header});
    m.label(done);
    m.op_void(Op::OpStore, {color, m.op(Op::OpLoad, v4, {result_var})});
    m.op_void(Op::OpReturn);
    m.end_function();

    const Id interface[] = {coord, color, texture, sampler};
    m.entry_point(spv::ExecutionModel::Fragment, main, "main", interface);
    m.execution_mode(main, spv::ExecutionMode::OriginUpperLeft);
    CHECK(m.const_float(0.5f) == m.const_float(0.5f) && m.const_float(-0.0f) != m.const_float(0.0f));
    CHECK(validates(m.finish(), "xenoforge_spirv_fragment.spv"));
}

} // namespace

int main() {
    refuses_a_broken_module();
    compute();
    fragment();
    return finish("spirv");
}
