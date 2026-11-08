// The translator's vertex and pixel shaders, checked by spirv-val (Vulkan
// 1.3): every vector and scalar operation, a predicated instruction, a
// conditional exec, an exec that ends inside a condition, each vertex
// format, 1D, 2D, 3D-or-stacked and cube texture fetches with a LOD bias,
// offsets, unnormalized coordinates, the LOD or gradients from registers and
// the stacked layers' filters, the color and depth exports and a kill. And
// memexport (straight, at a kill, through the dispatcher). Vertex shaders in
// every host form (shader::HostVertex), with user clip planes, and pixel
// shaders converting D24FS8 depth. getBCF translated as Xenia's 0. And what
// is refused is refused: getWeights in the harness, the wrong stage.

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

// in its stage and the harness; a vertex shader in every host form too (the
// expansions of points and rectangles, the domain shaders)
void translates(const ParsedShader &shader, const std::string &name) {
    std::vector<std::pair<shader::Stage, shader::VertexOptions>> forms = {
        {shader.type == ShaderType::Vertex ? shader::Stage::Vertex : shader::Stage::Pixel, {}},
        {shader::Stage::Harness, {}}};
    if (shader.type == ShaderType::Vertex) {
        using HV = shader::HostVertex;
        for (uint32_t h = 1; h <= uint32_t(HV::QuadDomainPatchIndexed); h++)
            forms.push_back({shader::Stage::Vertex, {HV(h), h % 2 == 0}});
        // user clip planes: clip and cull distances, a point's by ps_ucp_mode
        // (mode 2 with more than 4 planes by the corners alone), a
        // rectangle's mirrored, a domain shader's
        auto planes = [](HV host, uint8_t count, bool cull, uint8_t mode) {
            shader::VertexOptions o{host};
            o.clip_planes = count, o.clip_planes_cull = cull, o.point_ucp_mode = mode;
            return o;
        };
        forms.push_back({shader::Stage::Vertex, planes(HV::Vertex, 3, false, 0)});
        forms.push_back({shader::Stage::Vertex, planes(HV::Vertex, 6, true, 0)});
        for (uint8_t mode = 0; mode < 4; mode++) {
            forms.push_back({shader::Stage::Vertex, planes(HV::PointListAsTriangleStrip, 3, false, mode)});
            forms.push_back({shader::Stage::Vertex, planes(HV::PointListAsTriangleStrip, 6, mode & 1, mode)});
        }
        forms.push_back({shader::Stage::Vertex, planes(HV::RectangleListAsTriangleStrip, 4, false, 0)});
        forms.push_back({shader::Stage::Vertex, planes(HV::QuadDomainPatchIndexed, 2, true, 0)});
    }
    for (auto [stage, options] : forms) {
        auto t = shader::translate(shader, stage, options);
        std::string label = name + (stage == shader::Stage::Harness ? "_harness" : "_stage");
        if (options.key()) label += "_host" + std::to_string(options.key());
        CHECK(t.ok());
        if (!t.ok()) {
            std::printf("  %s:", label.c_str());
            for (auto &u : t.unsupported) std::printf(" %s;", u.c_str());
            std::printf("\n");
            continue;
        }
        CHECK(validates(t.spirv, label));
    }
    // a pixel shader's D24FS8 depth converted to 20e4, towards zero and to
    // the nearest even (shader::PixelOptions)
    if (shader.type == ShaderType::Pixel)
        for (uint8_t mode = 1; mode <= 2; mode++) {
            auto t = shader::translate(shader, shader::Stage::Pixel, {}, {mode});
            CHECK(t.ok());
            if (t.ok()) CHECK(validates(t.spirv, name + "_float24_" + std::to_string(mode)));
        }
    // a point list's: the pixel parameters' sprite coordinates read from
    // the point expansion (shader::PixelOptions::point_coordinates)
    if (shader.type == ShaderType::Pixel) {
        shader::PixelOptions options;
        options.point_coordinates = true;
        auto t = shader::translate(shader, shader::Stage::Pixel, {}, options);
        CHECK(t.ok());
        if (t.ok()) CHECK(validates(t.spirv, name + "_point_coordinates"));
    }
#ifdef XF_EDRAM_FSI_LIBRARY
    // the interlock path: the output merger taken in (edram_fsi.frag's
    // library), under pixel and sample interlock, with centroids
    if (shader.type == ShaderType::Pixel) {
        static const uint32_t library[] = {
#include XF_EDRAM_FSI_LIBRARY
        };
        for (uint8_t fsi = 1; fsi <= 2; fsi++) {
            shader::PixelOptions options;
            options.fsi = fsi;
            options.centroid = fsi == 2 ? 0xFFFF : 0;
            options.edram_library = library;
            auto t = shader::translate(shader, shader::Stage::Pixel, {}, options);
            CHECK(t.ok());
            if (!t.ok())
                for (auto &u : t.unsupported) std::printf("  %s fsi: %s\n", name.c_str(), u.c_str());
            if (t.ok()) CHECK(validates(t.spirv, name + "_fsi_" + std::to_string(fsi)));
        }
    }
#endif
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

    // offsets, unnormalized coordinates, the LOD and gradients from
    // registers, a 1D fetch of two coordinates, and the stacked layers'
    // filters: the instruction's linear, the fetch constant's, and the two
    // differing (chosen by the layer's gradient)
    std::vector<Slot> variants;
    uint32_t variant = 0;
    for (auto dimension :
         {FetchOpDimension::D1, FetchOpDimension::D2, FetchOpDimension::D3OrStacked, FetchOpDimension::Cube})
        for (uint32_t v = 0; v < 4; v++, variant++) {
            TextureFetchInstruction t;
            t.opcode_name = "tfetch";
            t.dimension = dimension;
            t.result = dst(StorageTarget::Register, 7, 0xF);
            t.operand_count = 2;
            t.operands[0] = reg(2);
            if (dimension == FetchOpDimension::D1) t.operands[0].component_count = v & 1 ? 2 : 1;
            t.operands[1].source = StorageSource::TextureFetchConstant;
            t.operands[1].index = variant % 32;
            t.offset_x = v & 1 ? -0.5f : 1.0f, t.offset_y = 0.5f, t.offset_z = v & 2 ? 1.0f : 0.0f;
            t.unnormalized_coordinates = v == 1 && dimension != FetchOpDimension::Cube;
            t.use_register_lod = v == 2;
            t.use_register_gradients = v == 3;
            t.vol_mag_filter = v == 0   ? TextureFilter::Linear
                               : v == 1 ? TextureFilter::Point
                                        : TextureFilter::UseFetchConst;
            t.vol_min_filter = v == 1 ? TextureFilter::Linear : TextureFilter::UseFetchConst;
            variants.push_back(t);
        }
    translates(make_shader(ShaderType::Pixel, {make_exec(variants, true)}), "tfetch_variants_ps");
    translates(make_shader(ShaderType::Vertex, {make_exec(variants, true)}), "tfetch_variants_vs");
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

// memexport: allocs of exports, eA from a stream constant, eM0-eM4, the
// export at an alloc, at a kill and at the end, straight and through the
// dispatcher (a loop exporting each time round)
void memexport() {
    AluInstruction address = vector(AluVectorOpcode::Mad, dst(StorageTarget::ExportAddress, 0, 0xF),
                                    {reg(0, "xxxx"), con(255, "xyxx"), con(6)});
    std::vector<Slot> data = {address};
    for (uint32_t k = 0; k < 5; k++)
        data.push_back(vector(AluVectorOpcode::Max, dst(StorageTarget::ExportData, k, 0xF), {reg(1 + k), reg(1 + k)}));
    AluInstruction kill = scalar(AluScalarOpcode::KillsGt, dst(StorageTarget::None, 0, 0), {reg(2)});
    AluInstruction position = vector(AluVectorOpcode::Max, dst(StorageTarget::Position, 0, 0xF), {reg(0), reg(0)});
    AluInstruction color = vector(AluVectorOpcode::Max, dst(StorageTarget::Color, 0, 0xF), {reg(0), reg(0)});
    for (ShaderType type : {ShaderType::Vertex, ShaderType::Pixel}) {
        bool vertex = type == ShaderType::Vertex;
        // alloc export; exec (eA, eM0-eM4); alloc export; exec (eA, eM0-eM4, kill);
        // alloc position or colors; exec end
        ParsedShader s;
        s.type = type;
        uint32_t index = 0;
        auto add_alloc = [&](AllocType t) {
            s.control_flow.push_back({index, ControlFlowOpcode::Alloc, AllocInstruction{index, t, 1, vertex}});
            index++;
        };
        auto add_exec = [&](Exec e) {
            e.cf.cf_index = index;
            s.control_flow.push_back({index++, e.cf.opcode, e});
        };
        add_alloc(AllocType::Memory);
        add_exec(make_exec(data, false));
        add_alloc(AllocType::Memory);
        std::vector<Slot> killed = data;
        killed.push_back(kill);
        add_exec(make_exec(killed, false));
        add_alloc(vertex ? AllocType::VsPosition : AllocType::PsColors);
        add_exec(make_exec({vertex ? position : color}, true));
        s.cf_pair_bound = (index + 1) / 2;
        translates(s, vertex ? "memexport_vs" : "memexport_ps");
    }
    // 0: alloc export; 1: loop i0 (skip to 4); 2: exec (eA, eM0); 3: endloop
    // (back to 2); 4: exec end, exporting what the last time round left
    ParsedShader s;
    s.type = ShaderType::Vertex;
    s.control_flow.push_back({0, ControlFlowOpcode::Alloc, AllocInstruction{0, AllocType::Memory, 1, true}});
    LoopStartInstruction start;
    start.cf_index = 1, start.skip_address = 4;
    s.control_flow.push_back({1, ControlFlowOpcode::LoopStart, start});
    Exec e = make_exec({address, data[1]}, false);
    e.cf.cf_index = 2;
    s.control_flow.push_back({2, e.cf.opcode, e});
    LoopEndInstruction end;
    end.cf_index = 3, end.body_address = 2;
    s.control_flow.push_back({3, ControlFlowOpcode::LoopEnd, end});
    Exec last = make_exec({position}, true);
    last.cf.cf_index = 4;
    s.control_flow.push_back({4, last.cf.opcode, last});
    s.cf_pair_bound = 3;
    translates(s, "memexport_loop_vs");
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
    // getBCF: translated as Xenia's 0, and said so
    TextureFetchInstruction bcf = weights;
    bcf.opcode = FetchOpcode::GetTextureBorderColorFrac;
    bcf.opcode_name = "getBCF2D";
    auto bcf_shader = make_shader(ShaderType::Pixel, {make_exec({bcf}, true)});
    translates(bcf_shader, "getbcf_ps");
    auto t = shader::translate(bcf_shader, shader::Stage::Pixel);
    CHECK(t.ok() && t.approximate.size() == 1);
    // a stage that is not the shader's
    CHECK(!shader::translate(make_shader(ShaderType::Vertex, {make_exec({}, true)}), shader::Stage::Pixel).ok());
}

} // namespace

int main() {
    every_operation();
    control_and_exports();
    fetches();
    branches();
    memexport();
    refuses();
    return finish("shader_translator");
}
