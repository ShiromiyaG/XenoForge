// The windowless Vulkan device runs a compute shader written with the SPIR-V
// writer (out[i] = in[i] * 2 + 1 over 256 values) and reads the answer back.
// Without a Vulkan 1.3 device it says so and passes.

#include "check.h"
#include "xenoforge/spirv.h"
#include "xenoforge/vk_compute.h"

#include <cstring>

using namespace xf;
using spirv::Id;
using spv::Op;

namespace {

std::vector<uint32_t> double_plus_one() {
    spirv::Module m;
    m.capability(spv::Capability::Shader);
    m.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    Id u32 = m.type_uint(), v3 = m.type_vector(u32, 3);
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
    Id gid = m.global_variable(m.type_pointer(spv::StorageClass::Input, v3), spv::StorageClass::Input);
    m.decorate(gid, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::GlobalInvocationId)});

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
    return m.finish();
}

void runs_on_the_gpu() {
    std::string error;
    auto gpu = vk::Compute::open(error);
    if (!gpu) {
        std::printf("  no Vulkan device (%s): not run\n", error.c_str());
        return;
    }
    std::printf("  on %s\n", gpu->device_name().c_str());
    std::vector<uint8_t> in(256 * 4), out(256 * 4);
    for (uint32_t i = 0; i < 256; i++) std::memcpy(&in[i * 4], &i, 4);
    std::vector<uint8_t> *buffers[] = {&in, &out};
    CHECK(gpu->run(double_plus_one(), buffers, 256 / 64, error));
    if (!error.empty()) std::printf("  %s\n", error.c_str());
    bool all = true;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v;
        std::memcpy(&v, &out[i * 4], 4);
        all &= v == i * 2 + 1;
    }
    CHECK(all);
}

} // namespace

int main() {
    runs_on_the_gpu();
    return finish("vk_compute");
}
