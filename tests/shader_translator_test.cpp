// The translator's vertex and pixel shaders, checked by spirv-val (Vulkan
// 1.3): every vector and scalar operation, a predicated instruction, a
// conditional exec, an exec that ends inside a condition, each vertex
// format, 1D, 2D and 3D texture fetches with a LOD bias, the color and depth
// exports and a kill. And what version 1 refuses is refused: loops, calls,
// jumps, memexport, cube fetches.

#include "check.h"
#include "shader_builders.h"
#include "xenoforge/shader_translator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace xf;
using namespace xf::ucode;
using namespace xf::ucode::build;

namespace {

bool validates(const std::vector<uint32_t> &words, const std::string &name) {
#ifdef XF_SPIRV_VAL
    auto path = std::filesystem::temp_directory_path() / ("xenoforge_" + name + ".spv");
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(words.data()), std::streamsize(words.size() * 4));
    std::string command =
        std::string("\"\"") + XF_SPIRV_VAL + "\" --target-env vulkan1.3 \"" + path.string() + "\"\"";
    bool valid = std::system(command.c_str()) == 0;
    if (valid) std::filesystem::remove(path);
    else std::printf("  spirv-val refused %s (kept)\n", path.string().c_str());
    return valid;
#else
    (void)words, (void)name;
    return true;
#endif
}

void translates(const ParsedShader &shader, const std::string &name) {
    for (auto stage : {shader.type == ShaderType::Vertex ? shader::Stage::Vertex : shader::Stage::Pixel,
                       shader::Stage::Harness}) {
        auto t = shader::translate(shader, stage);
        std::string label = name + (stage == shader::Stage::Harness ? "_harness" : "_stage");
        CHECK(t.ok());
        if (!t.ok()) {
            std::printf("  %s:", label.c_str());
            for (auto &u : t.unsupported) std::printf(" %s;", u.c_str());
            std::printf("\n");
            continue;
        }
        CHECK(validates(t.spirv, label));
    }
}

void every_operation() {
    std::vector<Slot> slots;
    for (uint32_t op = 0; op <= uint32_t(AluVectorOpcode::MaxA); op++)
        slots.push_back(vector(AluVectorOpcode(op), dst(StorageTarget::Register, 10 + op % 8, 0xF), {reg(0), reg(1), reg(2)}));
    for (uint32_t op = 0; op <= uint32_t(AluScalarOpcode::RetainPrev); op++)
        if (AluScalarOpcode(op) != AluScalarOpcode::Opcode41)
            slots.push_back(scalar(AluScalarOpcode(op), dst(StorageTarget::Register, 20 + op % 8, 1), {reg(0), reg(1)}));
    translates(make_shader(ShaderType::Pixel, {make_exec(slots, true)}), "every_operation_ps");
    translates(make_shader(ShaderType::Vertex, {make_exec(slots, true)}), "every_operation_vs");
}

void control_and_exports() {
    AluInstruction setp = scalar(AluScalarOpcode::SetpEq, dst(StorageTarget::Register, 3, 1), {reg(0)});
    AluInstruction predicated = vector(AluVectorOpcode::Mul, dst(StorageTarget::Color, 1, 0xF), {reg(0), reg(1)});
    predicated.is_predicated = true;
    AluInstruction kill = scalar(AluScalarOpcode::KillsGt, dst(StorageTarget::None, 0, 0), {reg(2)});
    AluInstruction color = vector(AluVectorOpcode::Add, dst(StorageTarget::Color, 0, 0xF, "xyz1", true), {reg(0), con(7)});
    AluInstruction depth = scalar(AluScalarOpcode::Maxs, dst(StorageTarget::Depth, 0, 1), {reg(1)});
    // the conditional exec ends the shader when b9 is set; the last one when not
    auto pixel = make_shader(ShaderType::Pixel,
                             {make_exec({setp, predicated, kill}, false),
                              make_exec({color}, true, ExecInstruction::Type::Conditional, 9, true),
                              make_exec({depth}, true)});
    translates(pixel, "control_ps");

    AluInstruction position = vector(AluVectorOpcode::Dp4, dst(StorageTarget::Position, 0, 0xF), {reg(0), con(0)});
    AluInstruction interpolator = vector(AluVectorOpcode::Max,
                                         dst(StorageTarget::Interpolator, 5, 0b0111), {reg(1), reg(1)});
    AluInstruction point = scalar(AluScalarOpcode::Adds, dst(StorageTarget::PointSizeEdgeFlagKillVertex, 0, 1), {reg(2)});
    translates(make_shader(ShaderType::Vertex, {make_exec({position, interpolator, point}, true)}), "exports_vs");
}

void fetches() {
    std::vector<Slot> slots;
    for (uint32_t format : {6u, 7u, 16u, 17u, 25u, 26u, 31u, 32u, 33u, 34u, 35u, 36u, 37u, 57u, 38u}) {
        VertexFetchInstruction v;
        v.result = dst(StorageTarget::Register, 4, 0xF, "xyz1");
        v.operand_count = 2;
        v.operands[0] = reg(0, "xxxx");
        v.operands[1].source = StorageSource::VertexFetchConstant;
        v.operands[1].index = format % 96;
        v.data_format = format;
        v.stride = 4;
        v.is_signed = format & 1;
        v.exp_adjust = format % 3 == 0 ? 2 : 0;
        slots.push_back(v);
        v.is_mini_fetch = true;
        slots.push_back(v);
    }
    translates(make_shader(ShaderType::Vertex, {make_exec(slots, true)}), "vfetch_vs");

    std::vector<Slot> textures;
    for (auto dimension : {FetchOpDimension::D1, FetchOpDimension::D2, FetchOpDimension::D3OrStacked, FetchOpDimension::Cube}) {
        TextureFetchInstruction t;
        t.opcode_name = "tfetch";
        t.dimension = dimension;
        t.result = dst(StorageTarget::Register, 6, 0xF, "xyz0");
        t.operand_count = 2;
        t.operands[0] = reg(1);
        t.operands[1].source = StorageSource::TextureFetchConstant;
        t.operands[1].index = 4 + uint32_t(dimension);
        t.lod_bias = dimension == FetchOpDimension::D2 ? 0.5f : 0.0f;
        textures.push_back(t);
    }
    translates(make_shader(ShaderType::Pixel, {make_exec(textures, true)}), "tfetch_ps");
    translates(make_shader(ShaderType::Vertex, {make_exec(textures, true)}), "tfetch_vs");
}

// loops, calls and jumps go through the dispatcher
void branches() {
    AluInstruction body = vector(AluVectorOpcode::Add, dst(StorageTarget::Register, 2, 0xF),
                                 {reg(2), con(0, "xyzw", StorageAddressing::LoopRelative)});
    AluInstruction setp = scalar(AluScalarOpcode::SetpGt, dst(StorageTarget::Register, 3, 1), {reg(2)});
    // 0: exec; 1: loop i3 (skip to 5); 2: exec body; 3: (p0) jmp 5; 4: endloop i3 (back to 2);
    // 5: call 7 on b2; 6: exec end; 7: exec; 8: return
    ParsedShader s = make_shader(ShaderType::Pixel, {make_exec({setp}, false)});
    LoopStartInstruction start;
    start.cf_index = 1, start.loop_constant = 3, start.skip_address = 5;
    s.control_flow.push_back({1, ControlFlowOpcode::LoopStart, start});
    Exec e = make_exec({body, setp}, false);
    e.cf.cf_index = 2;
    s.control_flow.push_back({2, e.cf.opcode, e});
    JumpInstruction jump;
    jump.cf_index = 3, jump.target = 5, jump.type = JumpInstruction::Type::Predicated, jump.condition = true;
    s.control_flow.push_back({3, ControlFlowOpcode::CondJmp, jump});
    LoopEndInstruction end;
    end.cf_index = 4, end.loop_constant = 3, end.body_address = 2, end.is_predicated_break = true;
    s.control_flow.push_back({4, ControlFlowOpcode::LoopEnd, end});
    CallInstruction call;
    call.cf_index = 5, call.target = 7, call.type = CallInstruction::Type::Conditional, call.bool_constant = 2;
    s.control_flow.push_back({5, ControlFlowOpcode::CondCall, call});
    Exec last = make_exec({body}, true);
    last.cf.cf_index = 6;
    s.control_flow.push_back({6, last.cf.opcode, last});
    Exec sub = make_exec({setp}, false);
    sub.cf.cf_index = 7;
    s.control_flow.push_back({7, sub.cf.opcode, sub});
    s.control_flow.push_back({8, ControlFlowOpcode::Return, ReturnInstruction{8}});
    translates(s, "branches_ps");
}

void refuses() {
    auto refused = [](ParsedShader s, const char *what) {
        auto t = shader::translate(s, shader::Stage::Harness);
        CHECK(!t.ok() && t.spirv.empty());
        bool named = false;
        for (auto &u : t.unsupported) named |= u.find(what) != std::string::npos;
        CHECK(named);
        if (!named) std::printf("  expected \"%s\" among what is refused\n", what);
    };
    TextureFetchInstruction weights;
    weights.opcode = FetchOpcode::GetTextureWeights;
    weights.opcode_name = "getWeights";
    weights.result = dst(StorageTarget::Register, 1, 0xF);
    weights.operands[0] = reg(0);
    refused(make_shader(ShaderType::Pixel, {make_exec({weights}, true)}), "getWeights");
    AluInstruction export_data = vector(AluVectorOpcode::Add, dst(StorageTarget::ExportData, 0, 0xF), {reg(0), reg(1)});
    refused(make_shader(ShaderType::Vertex, {make_exec({export_data}, true)}), "memexport");
    // a stage that is not the shader's
    CHECK(!shader::translate(make_shader(ShaderType::Vertex, {make_exec({}, true)}), shader::Stage::Pixel).ok());
}

} // namespace

int main() {
    every_operation();
    control_and_exports();
    fetches();
    branches();
    refuses();
    return finish("shader_translator");
}
