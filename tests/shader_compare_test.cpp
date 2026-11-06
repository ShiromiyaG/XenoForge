// The translator's SPIR-V run on the GPU against the CPU interpreter, case
// by case (shader_harness.h): every vector and scalar ALU operation on
// chosen and edge inputs (0, -0, 1, -1, infinities, NaN, the largest and
// smallest floats), operand swizzles and modifiers, write masks with
// constant components, saturation, predicates, a0 from maxa, the previous
// scalar, a conditional exec, vertex fetches of each format with each
// endianness, the texture fetch's stand-in; memory exports of each format,
// number format and endianness against the packing written out here. Then
// the shaders of the traces,
// if XF_SHADERS names the folder Xenia dumped them to, on random inputs.
//
// Without a Vulkan 1.3 device it says so and passes.

#include "check.h"
#include "shader_builders.h"
#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/shader_translator.h"
#include "xenoforge/vk_compute.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::ucode;
using namespace xf::ucode::build;
using harness::CaseInput;
using harness::CaseOutput;
using harness::Constants;

namespace {

std::unique_ptr<vk::Compute> gpu;

// --- running ----------------------------------------------------------------

struct Run {
    std::vector<CaseInput> inputs;
    Constants constants{};
    std::vector<uint8_t> memory = std::vector<uint8_t>(256, 0);
};

// tolerance: 0 for bit for bit; otherwise relative, for the operations
// whose GPU results are approximate (exp, log, rcp, rsq, sqrt, sin, cos)
bool same_float(float gpu_value, float cpu_value, float tolerance) {
    if (std::isnan(gpu_value) && std::isnan(cpu_value)) return true;
    // zeros and denormals alike: the GPU may flush them
    if (std::fabs(gpu_value) < FLT_MIN && std::fabs(cpu_value) < FLT_MIN) return true;
    uint32_t a, b;
    std::memcpy(&a, &gpu_value, 4);
    std::memcpy(&b, &cpu_value, 4);
    if (a == b) return true;
    if (tolerance == 0 || std::isinf(gpu_value) || std::isinf(cpu_value) || (a ^ b) >> 31) return false;
    // exp, log, rcp, rsq, sqrt, sin, cos: the GPU's own precision, then 21 bits
    return std::fabs(gpu_value - cpu_value) <= std::fabs(cpu_value) * tolerance + tolerance * 0.1f;
}

// Runs the shader on both sides; reports the first few differences. False
// when it could not run (unsupported, no GPU).
bool compare(const char *what, const ParsedShader &shader, Run &run, float tolerance = 0) {
    auto translation = shader::translate(shader, shader::Stage::Harness);
    if (!translation.ok()) {
        std::printf("  %s: not translated:", what);
        for (auto &u : translation.unsupported) std::printf(" %s;", u.c_str());
        std::printf("\n");
        failures++;
        return false;
    }
    size_t cases = run.inputs.size();
    size_t padded = (cases + harness::LOCAL_SIZE - 1) / harness::LOCAL_SIZE * harness::LOCAL_SIZE;
    run.inputs.resize(padded);
    std::vector<uint8_t> in(padded * sizeof(CaseInput)), consts(sizeof(Constants)), out(padded * sizeof(CaseOutput));
    std::memcpy(in.data(), run.inputs.data(), in.size());
    std::memcpy(consts.data(), &run.constants, sizeof(Constants));
    std::vector<uint8_t> memory = run.memory;
    std::vector<uint8_t> *buffers[] = {&in, &consts, &memory, &out};
    std::string error;
    if (!gpu->run(translation.spirv, buffers, uint32_t(padded / harness::LOCAL_SIZE), error)) {
        std::printf("  %s: GPU run failed: %s\n", what, error.c_str());
        failures++;
        return false;
    }
    int reported = 0;
    for (size_t c = 0; c < cases; c++) {
        CaseOutput expected{}, got;
        std::memcpy(&got, out.data() + c * sizeof(CaseOutput), sizeof(CaseOutput));
        std::string unsupported;
        if (!shader::interpret(shader, run.inputs[c], run.constants, run.memory, expected, unsupported)) {
            std::printf("  %s: the interpreter refused: %s\n", what, unsupported.c_str());
            failures++;
            return false;
        }
        const float *g = &got.registers[0][0], *e = &expected.registers[0][0];
        size_t floats = (offsetof(CaseOutput, killed)) / 4;
        bool same = got.killed == expected.killed && got.predicate == expected.predicate &&
                    got.address_register == expected.address_register;
        size_t bad = SIZE_MAX;
        for (size_t k = 0; k < floats && same; k++)
            if (!same_float(g[k], e[k], tolerance)) same = false, bad = k;
        if (same) continue;
        failures++;
        if (reported++ < 4) {
            std::printf("  %s, case %zu:", what, c);
            if (bad != SIZE_MAX)
                std::printf(" output float %zu (vec4 %zu.%zu) GPU %.9g CPU %.9g;", bad, bad / 4, bad % 4, g[bad], e[bad]);
            std::printf(" killed %u/%u predicate %u/%u a0 %d/%d\n", got.killed, expected.killed, got.predicate,
                        expected.predicate, got.address_register, expected.address_register);
            const float *r = &run.inputs[c].registers[0][0];
            std::printf("    inputs r0 (%g %g %g %g) r1 (%g %g %g %g) r2 (%g %g %g %g)\n", r[0], r[1], r[2], r[3], r[4],
                        r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
        }
    }
    return true;
}

// inputs for r0-r3 from the edge values, every case different
std::vector<CaseInput> edge_inputs(size_t count, uint32_t seed) {
    static const float values[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -2.5f, 3.0f, 7.25f, -0.75f, 100.0f, 1e-3f,
                                   1e30f, -1e30f, FLT_MAX, -FLT_MAX, FLT_MIN, 2.0f, 0.25f,
                                   std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::quiet_NaN(), 255.6f, -256.4f, 1.0e-20f};
    constexpr size_t n = sizeof(values) / sizeof(values[0]);
    std::vector<CaseInput> inputs(count);
    uint32_t x = seed;
    for (CaseInput &c : inputs) {
        std::memset(&c, 0, sizeof(c));
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                x = x * 1664525u + 1013904223u;
                c.registers[r][k] = values[(x >> 16) % n];
            }
    }
    return inputs;
}

// --- the checks ----------------------------------------------------------------

void vector_operations() {
    for (uint32_t op = 0; op <= uint32_t(AluVectorOpcode::MaxA); op++) {
        auto opcode = AluVectorOpcode(op);
        uint32_t n = vector_opcode_info(opcode).operand_count();
        std::initializer_list<Operand> three = {reg(0), reg(1), reg(2)}, two = {reg(0), reg(1)}, one = {reg(0)};
        AluInstruction a = vector(opcode, dst(StorageTarget::Register, 10, 0xF), n == 3 ? three : n == 2 ? two : one);
        // the scalar half sets ps first, for the record
        auto shader = make_shader(ShaderType::Pixel, {make_exec({a}, true)});
        Run run;
        run.inputs = edge_inputs(256, 17 + op);
        compare(vector_opcode_info(opcode).name, shader, run);
    }
}

void scalar_operations() {
    for (uint32_t op = 0; op <= uint32_t(AluScalarOpcode::RetainPrev); op++) {
        auto opcode = AluScalarOpcode(op);
        if (opcode == AluScalarOpcode::Opcode41) continue;
        const auto &info = scalar_opcode_info(opcode);
        // ps from r3.x + r3.y first, so the *_prev operations see something
        AluInstruction first = scalar(AluScalarOpcode::Adds, dst(StorageTarget::Register, 12, 1), {reg(3, "xyzw")});
        AluInstruction a = info.operand_count == 2
                               ? scalar(opcode, dst(StorageTarget::Register, 11, 0xF), {reg(0, "xxxx"), reg(1, "yyyy")})
                               : scalar(opcode, dst(StorageTarget::Register, 11, 0xF), {reg(0, "xyzw")});
        auto shader = make_shader(ShaderType::Pixel, {make_exec({first, a}, true)});
        Run run;
        run.inputs = edge_inputs(256, 101 + op);
        bool approximate = opcode == AluScalarOpcode::Exp || opcode == AluScalarOpcode::Log ||
                           opcode == AluScalarOpcode::Logc || (opcode >= AluScalarOpcode::Rcpc && opcode <= AluScalarOpcode::Rsq) ||
                           opcode == AluScalarOpcode::Sqrt || opcode == AluScalarOpcode::Sin || opcode == AluScalarOpcode::Cos;
        if (opcode == AluScalarOpcode::Sin || opcode == AluScalarOpcode::Cos) {
            // the GPU's sin and cos are exact enough only near the circle
            for (CaseInput &c : run.inputs)
                for (int k = 0; k < 4; k++)
                    if (!std::isfinite(c.registers[0][k]) || std::fabs(c.registers[0][k]) > 3.0f) c.registers[0][k] = 0.75f;
        }
        compare(info.name, shader, run, approximate ? 1e-5f : 0.0f);
    }
}

void operands_and_results() {
    // swizzles, negation, absolute values; a constant; a masked, swizzled,
    // saturated destination with constant 0 and 1 components
    AluInstruction a = vector(AluVectorOpcode::Mad, dst(StorageTarget::Register, 5, 0b1111, "zx01", true),
                              {reg(0, "wzyx", true), reg(1, "xxyy", false, true), con(3, "zwzw")});
    AluInstruction b = scalar(AluScalarOpcode::Mulsc0, dst(StorageTarget::Register, 6, 0b0110, "xx1x"),
                              {reg(2, "wwww", true, true), con(4, "yyyy")});
    auto shader = make_shader(ShaderType::Vertex, {make_exec({a, b}, true)});
    Run run;
    run.inputs = edge_inputs(256, 7);
    for (int k = 0; k < 256; k++) run.constants.float_constants[k % 256][k % 4] = float(k) * 0.125f - 3.0f;
    compare("operands and results", shader, run);
}

void control() {
    // maxa sets a0, then c[a0 + 2] is read; setp sets the predicate, then
    // instructions on either side of it; a conditional exec on b5
    AluInstruction maxa = vector(AluVectorOpcode::MaxA, dst(StorageTarget::Register, 8, 0xF), {reg(0), reg(1)});
    AluInstruction relative = vector(AluVectorOpcode::Add, dst(StorageTarget::Register, 9, 0xF),
                                     {con(2, "xyzw", StorageAddressing::AddressRegisterRelative), reg(2)});
    AluInstruction setp = scalar(AluScalarOpcode::SetpGt, dst(StorageTarget::Register, 10, 1), {reg(3)});
    AluInstruction if_true = vector(AluVectorOpcode::Mul, dst(StorageTarget::Register, 11, 0xF), {reg(0), reg(1)});
    if_true.is_predicated = true, if_true.predicate_condition = true;
    AluInstruction if_false = vector(AluVectorOpcode::Add, dst(StorageTarget::Register, 12, 0xF), {reg(0), reg(1)});
    if_false.is_predicated = true, if_false.predicate_condition = false;
    AluInstruction on_bool = vector(AluVectorOpcode::Max, dst(StorageTarget::Color, 0, 0xF), {reg(1), reg(2)});
    AluInstruction export_depth = scalar(AluScalarOpcode::Adds, dst(StorageTarget::Depth, 0, 1), {reg(0)});
    auto shader = make_shader(ShaderType::Pixel,
                              {make_exec({maxa, relative, setp, if_true, if_false}, false),
                               make_exec({on_bool}, false, ExecInstruction::Type::Conditional, 5, true),
                               make_exec({export_depth}, true)});
    for (uint32_t b : {0u, 1u}) {
        Run run;
        run.inputs = edge_inputs(256, 23 + b);
        for (int k = 0; k < 256; k++) run.constants.float_constants[k][0] = float(k);
        run.constants.bool_constants[0] = b << 5;
        compare(b ? "control, b5 set" : "control, b5 clear", shader, run);
    }
}

void fetches() {
    // the vertex formats, each with each endianness, from r0.x as the index
    const uint32_t formats[] = {6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 57, 38};
    for (uint32_t format : formats) {
        for (uint32_t endian = 0; endian < 4; endian++) {
            for (int is_signed = 0; is_signed < 2; is_signed++) {
                VertexFetchInstruction v;
                v.result = dst(StorageTarget::Register, 4, 0xF);
                v.operand_count = 2;
                v.operands[0] = reg(0, "xxxx");
                v.operands[1].source = StorageSource::VertexFetchConstant;
                v.operands[1].index = 3; // vertex fetch constant 3: slot 1, second
                v.data_format = format;
                v.stride = 5;
                v.offset = 1;
                v.is_signed = is_signed;
                v.is_index_rounded = endian & 1;
                v.exp_adjust = endian == 2 ? -3 : 0;
                v.signed_rf_mode = endian == 3 ? SignedRepeatingFractionMode::NoZero : SignedRepeatingFractionMode::ZeroClampMinusOne;
                auto shader = make_shader(ShaderType::Vertex, {make_exec({v}, true)});
                Run run;
                run.memory.resize(1024);
                for (size_t k = 0; k < run.memory.size(); k++) run.memory[k] = uint8_t(k * 37 + 11);
                // vertex constant 3: address 16 dwords, endianness, 64 dwords
                run.constants.fetch_constants[6] = 3u | 16u << 2;
                run.constants.fetch_constants[7] = endian | 64u << 2;
                run.inputs.resize(64);
                for (size_t c = 0; c < run.inputs.size(); c++) {
                    std::memset(&run.inputs[c], 0, sizeof(CaseInput));
                    run.inputs[c].registers[0][0] = float(c % 16) * 0.75f;
                }
                char name[64];
                std::snprintf(name, sizeof(name), "vfetch format %u endian %u %s", format, endian, is_signed ? "signed" : "unsigned");
                compare(name, shader, run);
            }
        }
    }
    // the texture fetch's stand-in, 2D and 3D, with a destination swizzle
    for (auto dimension : {FetchOpDimension::D1, FetchOpDimension::D2, FetchOpDimension::D3OrStacked}) {
        TextureFetchInstruction t;
        t.opcode_name = "tfetch";
        t.dimension = dimension;
        t.result = dst(StorageTarget::Register, 6, 0b1011, "wzy1");
        t.operand_count = 2;
        t.operands[0] = reg(1, "yxzw");
        t.operands[1].source = StorageSource::TextureFetchConstant;
        t.operands[1].index = 9;
        auto shader = make_shader(ShaderType::Pixel, {make_exec({t}, true)});
        Run run;
        run.inputs = edge_inputs(128, 5);
        compare("tfetch stand-in", shader, run);
    }
}

// --- memexport ---------------------------------------------------------------
//
// The interpreter does not export (Xenia's does not either), so the GPU's
// memory is checked against the packing written out here as Xenia's
// translator does it (spirv_shader_translator_memexport.cc).

float as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// a fixed-point component: NaN as 0, normalized ones clamped and scaled
// (signed ones of 1 or 2 bits not scaled), integer ones clamped to the
// component's range (Xenia's D3D12 translator), rounded half away from zero
// (each step a statement: no contraction)
uint32_t export_integer(float v, uint32_t width, bool is_signed, bool norm) {
    if (std::isnan(v)) v = 0.0f;
    if (is_signed) {
        float max = float((1u << (width - 1)) - 1);
        if (norm) {
            v = std::clamp(v, -1.0f, 1.0f);
            if (width > 2) v = v * max;
        } else {
            v = std::clamp(v, -1.0f - max, max);
        }
        float rounded = v + (v < 0.0f ? -0.5f : 0.5f);
        return uint32_t(int32_t(rounded));
    }
    float max = float((1u << width) - 1);
    if (norm) {
        v = std::clamp(v, 0.0f, 1.0f);
        v = v * max;
    } else {
        v = std::clamp(v, 0.0f, max);
    }
    float rounded = v + 0.5f;
    return uint32_t(rounded);
}

// float to half for values a half holds exactly (the checks pick them), then
// the extended range of PackFloat16x2ExtendedRange past 65504
uint32_t export_half(float v) {
    if (std::isnan(v)) v = 0.0f;
    auto exact_half = [](float x) -> uint32_t {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        uint32_t sign = (bits >> 16) & 0x8000;
        if ((bits & 0x7FFFFFFF) == 0) return sign;
        int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
        if (exponent >= 31) return sign | 0x7C00;
        return sign | uint32_t(exponent) << 10 | ((bits >> 13) & 0x3FF);
    };
    uint32_t standard = exact_half(v);
    if ((standard & 0x7C00) != 0x7C00) return standard;
    float halved = std::clamp(v, -131008.0f, 131008.0f) * 0.5f;
    return exact_half(halved) + 0x400;
}

uint32_t swap32(uint32_t v, uint32_t endian) {
    switch (endian) {
    case 1: return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}

struct ExportFormat {
    uint32_t format, size_log2;
    std::array<uint32_t, 4> widths; // fixed-point; zeros for the float formats
};

// eM's packed words (before the byte swap), as the format says
std::array<uint32_t, 4> export_words(const ExportFormat &f, std::array<float, 4> v, bool is_signed, bool norm,
                                     bool swap_red_blue) {
    if (swap_red_blue) std::swap(v[0], v[2]);
    std::array<uint32_t, 4> w{};
    if (f.widths[0]) {
        uint32_t ints[4] = {};
        for (int c = 0; c < 4; c++) ints[c] = export_integer(v[c], f.widths[c] ? f.widths[c] : 16, is_signed, norm);
        if (f.format == 26) {
            w[0] = (ints[0] & 0xFFFF) | ints[1] << 16;
            w[1] = (ints[2] & 0xFFFF) | ints[3] << 16;
            return w;
        }
        w[0] = ints[0];
        for (uint32_t c = 1, offset = f.widths[0]; c < 4 && f.widths[c]; offset += f.widths[c], c++) {
            uint32_t mask = ((1u << f.widths[c]) - 1) << offset;
            w[0] = (w[0] & ~mask) | ((ints[c] << offset) & mask);
        }
        return w;
    }
    switch (f.format) {
    case 31: w[0] = export_half(v[0]) | export_half(v[1]) << 16; break;
    case 32:
        w[0] = export_half(v[0]) | export_half(v[1]) << 16;
        w[1] = export_half(v[2]) | export_half(v[3]) << 16;
        break;
    default:
        for (int c = 0; c < 4; c++) std::memcpy(&w[c], &v[c], 4);
        break;
    }
    return w;
}

// Each case exports eM0 and eM1 at elements 2c and 2c + 1 of a stream of 7
// (so the last case's eM1 is past its count and not written), in each
// format, number format and endianness.
void memory_exports() {
    const ExportFormat formats[] = {
        {2, 0, {8, 0, 0, 0}},      {3, 1, {5, 5, 5, 1}},     {4, 1, {5, 6, 5, 0}},    {6, 2, {8, 8, 8, 8}},
        {7, 2, {10, 10, 10, 2}},   {10, 1, {8, 8, 0, 0}},    {15, 1, {4, 4, 4, 4}},   {17, 2, {10, 11, 11, 0}},
        {24, 1, {16, 0, 0, 0}},    {25, 2, {16, 16, 0, 0}},  {26, 3, {16, 16, 16, 16}}, {31, 2, {}},
        {32, 3, {}},               {36, 2, {}},              {38, 4, {}},
    };
    AluInstruction address = vector(AluVectorOpcode::Mad, dst(StorageTarget::ExportAddress, 0, 0xF),
                                    {reg(0, "xxxx"), con(1), con(0)});
    AluInstruction em0 = vector(AluVectorOpcode::Max, dst(StorageTarget::ExportData, 0, 0xF), {reg(1), reg(1)});
    AluInstruction em1 = vector(AluVectorOpcode::Max, dst(StorageTarget::ExportData, 1, 0xF), {reg(2), reg(2)});
    ParsedShader shader;
    shader.type = ShaderType::Vertex;
    shader.control_flow.push_back({0, ControlFlowOpcode::Alloc, AllocInstruction{0, AllocType::Memory, 1, true}});
    Exec e = make_exec({address, em0, em1}, true);
    e.cf.cf_index = 1;
    shader.control_flow.push_back({1, e.cf.opcode, e});
    shader.cf_pair_bound = 1;
    auto translation = shader::translate(shader, shader::Stage::Harness);
    CHECK(translation.ok());
    if (!translation.ok()) return;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    // per case, eM0 and eM1 (halves exact in float16, or past it)
    const std::array<float, 4> values[4][2] = {
        {{1.0f, 0.5f, 0.0f, 0.25f}, {-1.0f, -0.5f, 2.0f, -3.0f}},
        {{0.75f, -0.25f, nan, 1.5f}, {131008.0f, 98304.0f, -65504.0f, 1e9f}},
        {{12.0f, -7.0f, 3.0f, 100.0f}, {0.125f, 1024.0f, -2.0f, 0.0f}},
        {{-0.0f, 0.0625f, 0.875f, -0.875f}, {9.0f, 9.0f, 9.0f, 9.0f}},
    };
    const uint32_t base = 64; // bytes
    int checked = 0;
    for (const ExportFormat &f : formats) {
        for (uint32_t endian = 0; endian <= 5; endian++) {
            for (uint32_t number = 0; number < 4; number++) {
                bool is_signed = number & 1, norm = !(number & 2);
                bool swap = (endian + number) & 1;
                Run run;
                run.memory.assign(512, 0xCD);
                // c0: the stream (base in words, eM0's index 0, the format, a count of 7); c1 moves y by 2 per case
                uint32_t stream[4] = {0x40000000u | (base >> 2), 0x4B000000u,
                                      endian | f.format << 8 | (is_signed ? 1u : 0u) << 16 |
                                          (norm ? 0u : 1u) << 17 | (swap ? 1u : 0u) << 19 | 0x4B0u << 20,
                                      0x4B000000u | 7u};
                std::memcpy(run.constants.float_constants[0], stream, 16);
                run.constants.float_constants[1][1] = 2.0f;
                run.inputs.resize(4);
                std::vector<uint8_t> expected = run.memory;
                for (uint32_t c = 0; c < 4; c++) {
                    std::memset(&run.inputs[c], 0, sizeof(CaseInput));
                    run.inputs[c].registers[0][0] = float(c);
                    for (uint32_t k = 0; k < 2; k++) {
                        std::memcpy(run.inputs[c].registers[1 + k], values[c][k].data(), 16);
                        uint32_t element = 2 * c + k;
                        if (element >= 7) continue;
                        auto words = export_words(f, values[c][k], is_signed, norm, swap);
                        // the 128-bit endianness: word moves, then within words
                        uint32_t word_endian = endian;
                        if (endian == 4) std::swap(words[0], words[1]), std::swap(words[2], words[3]), word_endian = 2;
                        if (endian == 5) std::swap(words[0], words[3]), std::swap(words[1], words[2]), word_endian = 2;
                        for (uint32_t &w : words) w = swap32(w, word_endian);
                        uint32_t at = base + (element << f.size_log2);
                        uint8_t bytes[16];
                        std::memcpy(bytes, words.data(), 16);
                        std::memcpy(&expected[at], bytes, size_t(1) << f.size_log2);
                    }
                }
                size_t padded = harness::LOCAL_SIZE;
                run.inputs.resize(padded);
                for (size_t c = 4; c < padded; c++) {
                    // the padding's cases export nothing: eA invalid
                    std::memset(&run.inputs[c], 0, sizeof(CaseInput));
                    run.inputs[c].registers[0][0] = nan;
                }
                std::vector<uint8_t> in(padded * sizeof(CaseInput)), consts(sizeof(Constants)),
                    out(padded * sizeof(CaseOutput));
                std::memcpy(in.data(), run.inputs.data(), in.size());
                std::memcpy(consts.data(), &run.constants, sizeof(Constants));
                std::vector<uint8_t> memory = run.memory;
                std::vector<uint8_t> *buffers[] = {&in, &consts, &memory, &out};
                std::string error;
                if (!gpu->run(translation.spirv, buffers, 1, error)) {
                    std::printf("  memexport: GPU run failed: %s\n", error.c_str());
                    failures++;
                    return;
                }
                checked++;
                if (memory == expected) continue;
                failures++;
                std::printf("  memexport format %u endian %u %s %s%s:", f.format, endian, is_signed ? "signed" : "unsigned",
                            norm ? "normalized" : "integer", swap ? " red-blue swapped" : "");
                for (size_t k = base; k < base + (size_t(8) << f.size_log2); k++)
                    if (memory[k] != expected[k]) {
                        std::printf(" byte %zu GPU %02X expected %02X;", k - base, memory[k], expected[k]);
                        break;
                    }
                std::printf("\n");
            }
        }
    }
    std::printf("  memexport: %d formats, endiannesses and number formats checked\n", checked);
}

// the traces' shaders on random registers and constants
void corpus() {
    const char *folder = std::getenv("XF_SHADERS");
    if (!folder) {
        std::printf("  XF_SHADERS not set: the traces' shaders not run\n");
        return;
    }
    int count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(folder)) {
        std::string name = entry.path().filename().string();
        if (name.find(".ucode.bin.") == std::string::npos || !(name.ends_with(".vert") || name.ends_with(".frag"))) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> dwords(bytes.size() / 4);
        std::memcpy(dwords.data(), bytes.data(), dwords.size() * 4);
        bool vertex = name.ends_with(".vert");
        auto shader = parse_shader(vertex ? ShaderType::Vertex : ShaderType::Pixel, dwords);
        // A compiler leaves a vertex fetch's format empty for the console's
        // D3D to fill from the vertex declaration: filled here as a float4's.
        for (ControlFlow &cf : shader.control_flow)
            if (auto *e = std::get_if<Exec>(&cf.instruction))
                for (ExecSlot &slot : e->slots)
                    if (auto *v = std::get_if<VertexFetchInstruction>(&slot.instruction); v && v->data_format == 0)
                        v->data_format = 38;
        Run run;
        run.inputs.resize(256);
        uint32_t x = uint32_t(count) * 977 + 1;
        auto next = [&] {
            x = x * 1664525u + 1013904223u;
            return float(int32_t(x >> 8) % 20000) / 1000.0f;
        };
        for (CaseInput &c : run.inputs)
            for (auto &r : c.registers)
                for (float &v : r) v = next();
        for (auto &c : run.constants.float_constants)
            for (float &v : c) v = next();
        run.memory.resize(4096);
        for (size_t k = 0; k < run.memory.size(); k++) run.memory[k] = uint8_t(k * 13 + 7);
        for (int slot = 0; slot < 96; slot++) {
            run.constants.fetch_constants[slot * 2] = 3u | 8u << 2;
            run.constants.fetch_constants[slot * 2 + 1] = 2u | 512u << 2;
        }
        // three settings of the bool and loop constants, so branches go
        // both ways and loops run: none set and no iterations; some set and
        // a few iterations stepping up; all set and longer loops stepping down
        for (int setting = 0; setting < 3; setting++) {
            for (int b = 0; b < 8; b++)
                run.constants.bool_constants[b] = setting == 0 ? 0u : setting == 1 ? 0x5A5A5A5Au * uint32_t(b + 1) : ~0u;
            for (uint32_t l = 0; l < 32; l++) {
                uint32_t iterations = setting == 0 ? 0 : setting == 1 ? 1 + l % 3 : 4 + l % 3;
                uint32_t start = setting == 2 ? 8 : l % 2, step = setting == 2 ? uint32_t(uint8_t(-1)) : 1;
                run.constants.loop_constants[l] = iterations | start << 8 | step << 16;
            }
            // the compiler's literals over them, as D3D loads them with the
            // shader (xfeffect writes them beside the microcode)
            std::ifstream defs(entry.path().string() + ".defs");
            for (std::string line; std::getline(defs, line);) {
                unsigned index = 0, w[4] = {};
                if (std::sscanf(line.c_str(), "c%u %x %x %x %x", &index, &w[0], &w[1], &w[2], &w[3]) == 5 && index < 256)
                    std::memcpy(run.constants.float_constants[index], w, 16);
                else if (std::sscanf(line.c_str(), "i%u %x", &index, &w[0]) == 2 && index < 32)
                    run.constants.loop_constants[index] = w[0];
            }
            std::string label = name + " (constants " + std::to_string(setting) + ")";
            // whole shaders chain the approximate operations through
            // subtractions: a step of difference in a 21-bit rcp grows
            compare(label.c_str(), shader, run, 1e-4f);
        }
        count++;
    }
    std::printf("  %d shaders from XF_SHADERS run\n", count);
}

} // namespace

int main() {
    std::string error;
    gpu = vk::Compute::open(error);
    if (!gpu) {
        std::printf("  no Vulkan device (%s): not run\n", error.c_str());
        return finish("shader_compare");
    }
    vector_operations();
    scalar_operations();
    operands_and_results();
    control();
    fetches();
    memory_exports();
    corpus();
    return finish("shader_compare");
}
