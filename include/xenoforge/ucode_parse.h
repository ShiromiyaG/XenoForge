// Xenos microcode parsed into instructions: the control flow, and the ALU and
// fetch instructions each exec runs, with their operands and results spelled
// out. What the disassembler prints and what the translator to SPIR-V reads.
// Translated from Xenia's src/xenia/gpu/shader.h (the Parsed* structures),
// shader_translator.cc (the Parse* functions, Shader::AnalyzeUcode's walk),
// ucode.h and ucode.cc (the layouts and opcode tables), BSD; see
// LICENSE-xenia.

#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace xf::ucode {

enum class ShaderType : uint32_t { Vertex = 0, Pixel = 1 };

enum class ControlFlowOpcode : uint32_t {
    Nop = 0,
    Exec = 1,
    ExecEnd = 2,
    CondExec = 3,
    CondExecEnd = 4,
    CondExecPred = 5,
    CondExecPredEnd = 6,
    LoopStart = 7,
    LoopEnd = 8,
    CondCall = 9,
    Return = 10,
    CondJmp = 11,
    Alloc = 12,
    CondExecPredClean = 13,
    CondExecPredCleanEnd = 14,
    MarkVsFetchDone = 15,
};

enum class AllocType : uint32_t { None = 0, VsPosition = 1, VsInterpolators = 2, PsColors = 2, Memory = 3 };

enum class FetchOpcode : uint32_t {
    VertexFetch = 0,
    TextureFetch = 1,
    GetTextureBorderColorFrac = 16,
    GetTextureComputedLod = 17,
    GetTextureGradients = 18,
    GetTextureWeights = 19,
    SetTextureLod = 24,
    SetTextureGradientsHorz = 25,
    SetTextureGradientsVert = 26,
};

enum class AluVectorOpcode : uint32_t {
    Add, Mul, Max, Min, Seq, Sgt, Sge, Sne, Frc, Trunc, Floor, Mad, CndEq, CndGe, CndGt, Dp4, Dp3, Dp2Add, Cube,
    Max4, SetpEqPush, SetpNePush, SetpGtPush, SetpGePush, KillEq, KillGt, KillGe, KillNe, Dst, MaxA,
};

enum class AluScalarOpcode : uint32_t {
    Adds, AddsPrev, Muls, MulsPrev, MulsPrev2, Maxs, Mins, Seqs, Sgts, Sges, Snes, Frcs, Truncs, Floors, Exp, Logc,
    Log, Rcpc, Rcpf, Rcp, Rsqc, Rsqf, Rsq, MaxAs, MaxAsf, Subs, SubsPrev, SetpEq, SetpNe, SetpGt, SetpGe, SetpInv,
    SetpPop, SetpClr, SetpRstr, KillsEq, KillsGt, KillsGe, KillsNe, KillsOne, Sqrt, Opcode41, Mulsc0, Mulsc1, Addsc0,
    Addsc1, Subsc0, Subsc1, Sin, Cos, RetainPrev,
};

enum class TextureFilter : uint32_t { Point = 0, Linear = 1, BaseMap = 2, UseFetchConst = 3 };
enum class AnisoFilter : uint32_t { Disabled = 0, Max1to1, Max2to1, Max4to1, Max8to1, Max16to1, UseFetchConst = 7 };
enum class FetchOpDimension : uint32_t { D1 = 0, D2 = 1, D3OrStacked = 2, Cube = 3 };
enum class SignedRepeatingFractionMode : uint32_t { ZeroClampMinusOne = 0, NoZero = 1 };

uint32_t dimension_component_count(FetchOpDimension dimension);

// What an ALU operation changes besides its result.
constexpr uint32_t CHANGES_ADDRESS_REGISTER = 1, CHANGES_PREDICATE = 2, CHANGES_PIXEL_KILL = 4;

struct AluVectorOpcodeInfo {
    const char *name;
    uint32_t operand_components_used[3];
    uint32_t changed_state;
    uint32_t operand_count() const {
        return operand_components_used[2] ? 3 : operand_components_used[1] ? 2 : operand_components_used[0] ? 1 : 0;
    }
};

struct AluScalarOpcodeInfo {
    const char *name;
    uint32_t operand_count;
    bool single_operand_is_two_component;
    uint32_t changed_state;
};

const AluVectorOpcodeInfo &vector_opcode_info(AluVectorOpcode opcode);
const AluScalarOpcodeInfo &scalar_opcode_info(AluScalarOpcode opcode);

// Where a result goes.
enum class StorageTarget {
    None,
    Register,                    // r0-r63
    Interpolator,                // o0-o15, vertex shaders
    Position,                    // oPos
    PointSizeEdgeFlagKillVertex, // oPts
    ExportAddress,               // eA, memexport's address
    ExportData,                  // eM0-eM4
    Color,                       // oC0-oC3, pixel shaders
    Depth,                       // oDepth
};

// Components of the target that exist (oPts has 3, oDepth 1).
uint32_t used_component_count(StorageTarget target);

enum class StorageAddressing { Absolute, AddressRegisterRelative, LoopRelative }; // [n], [n+a0], [n+aL]

enum class Swizzle : uint8_t { X, Y, Z, W, Zero, One };

char swizzle_char(Swizzle s);

struct Result {
    StorageTarget target = StorageTarget::None;
    uint32_t index = 0;
    StorageAddressing addressing = StorageAddressing::Absolute;
    bool clamped = false;
    uint32_t original_write_mask = 0; // as in the microcode
    Swizzle components[4] = {Swizzle::X, Swizzle::Y, Swizzle::Z, Swizzle::W};

    // The write mask limited to the components the target has.
    uint32_t used_write_mask() const;
    bool is_standard_swizzle() const;
    // Components of the value (before swizzling) written and not replaced by 0 or 1.
    uint32_t used_result_components() const;
    // Components written with a constant; their values in constant_values (1 = one).
    uint32_t used_constant_components(uint32_t &constant_values) const;
};

enum class StorageSource { Register, ConstantFloat, VertexFetchConstant, TextureFetchConstant };

struct Operand {
    StorageSource source = StorageSource::Register;
    uint32_t index = 0;
    StorageAddressing addressing = StorageAddressing::Absolute;
    bool negated = false;
    bool absolute = false; // taken before the negation
    uint32_t component_count = 4;
    Swizzle components[4] = {Swizzle::X, Swizzle::Y, Swizzle::Z, Swizzle::W};

    // The rightmost component repeats past component_count.
    Swizzle component(uint32_t i) const { return components[i < component_count ? i : component_count - 1]; }
    bool is_standard_swizzle() const;
    // Components always bitwise equal to the other operand's.
    uint32_t identical_components(const Operand &other) const;
};

struct ExecInstruction {
    enum class Type { Unconditional, Conditional, Predicated };
    uint32_t cf_index = 0;
    ControlFlowOpcode opcode = ControlFlowOpcode::Exec;
    const char *opcode_name = "exec";
    uint32_t address = 0; // of the first ALU or fetch instruction
    uint32_t count = 0;
    Type type = Type::Unconditional;
    uint32_t bool_constant = 0; // Conditional
    bool condition = false;     // Conditional, Predicated
    bool is_end = false;
    bool is_predicate_clean = true;
    bool is_yield = false;
    uint32_t sequence = 0; // two bits per instruction: fetch, serialize
};

struct LoopStartInstruction {
    uint32_t cf_index = 0;
    uint32_t loop_constant = 0; // i0-i31: count, start aL, aL step
    bool is_repeat = false;     // keep aL
    uint32_t skip_address = 0;  // control flow index
};

struct LoopEndInstruction {
    uint32_t cf_index = 0;
    bool is_predicated_break = false;
    bool predicate_condition = false;
    uint32_t loop_constant = 0;
    uint32_t body_address = 0;
};

struct CallInstruction {
    enum class Type { Unconditional, Conditional, Predicated };
    uint32_t cf_index = 0;
    uint32_t target = 0;
    Type type = Type::Unconditional;
    uint32_t bool_constant = 0;
    bool condition = false;
};

struct ReturnInstruction {
    uint32_t cf_index = 0;
};

struct JumpInstruction {
    enum class Type { Unconditional, Conditional, Predicated };
    uint32_t cf_index = 0;
    uint32_t target = 0;
    Type type = Type::Unconditional;
    uint32_t bool_constant = 0;
    bool condition = false;
};

struct AllocInstruction {
    uint32_t cf_index = 0;
    AllocType type = AllocType::None;
    int count = 0;
    bool is_vertex_shader = false;
};

struct VertexFetchInstruction {
    const char *opcode_name = "vfetch_full";
    bool is_mini_fetch = false; // the operands and stride are the last full fetch's
    bool is_predicated = false;
    bool predicate_condition = false;
    Result result;
    uint32_t operand_count = 0;
    Operand operands[2]; // the index register, the vertex fetch constant (0-95)
    uint32_t data_format = 0; // a VertexFormat (the texture formats' numbering)
    int32_t offset = 0;
    uint32_t stride = 0; // in dwords
    int32_t exp_adjust = 0;
    uint32_t prefetch_count = 0; // minus one
    SignedRepeatingFractionMode signed_rf_mode = SignedRepeatingFractionMode::ZeroClampMinusOne;
    bool is_index_rounded = false;
    bool is_signed = false;
    bool is_integer = false;
};

struct TextureFetchInstruction {
    FetchOpcode opcode = FetchOpcode::TextureFetch;
    const char *opcode_name = "?";
    FetchOpDimension dimension = FetchOpDimension::D1;
    bool is_predicated = false;
    bool predicate_condition = false;
    Result result;
    uint32_t operand_count = 0;
    Operand operands[2]; // the coordinates, the texture fetch constant (0-31)
    bool fetch_valid_only = true;
    bool unnormalized_coordinates = false;
    TextureFilter mag_filter = TextureFilter::UseFetchConst;
    TextureFilter min_filter = TextureFilter::UseFetchConst;
    TextureFilter mip_filter = TextureFilter::UseFetchConst;
    AnisoFilter aniso_filter = AnisoFilter::UseFetchConst;
    TextureFilter vol_mag_filter = TextureFilter::UseFetchConst;
    TextureFilter vol_min_filter = TextureFilter::UseFetchConst;
    bool use_computed_lod = true;
    bool use_register_lod = false;
    bool use_register_gradients = false;
    float lod_bias = 0.0f;
    float offset_x = 0.0f, offset_y = 0.0f, offset_z = 0.0f;

    bool has_result() const { return result.target != StorageTarget::None; }
    // Components of the result that are not always zero.
    uint32_t non_zero_result_components() const;
};

struct AluInstruction {
    AluVectorOpcode vector_opcode = AluVectorOpcode::Add;
    AluScalarOpcode scalar_opcode = AluScalarOpcode::Adds;
    const char *vector_opcode_name = nullptr;
    const char *scalar_opcode_name = nullptr;
    bool is_predicated = false;
    bool predicate_condition = false;
    // The vector operation's result, and an export's constant 0 and 1 components.
    Result vector_and_constant_result;
    Result scalar_result;
    uint32_t vector_operand_count = 0;
    Operand vector_operands[3];
    uint32_t scalar_operand_count = 0;
    Operand scalar_operands[2];

    // As the assembler leaves an omitted operation (for disassembly; a
    // translator looks at the write masks and the changed state instead).
    bool is_vector_op_default_nop() const;
    bool is_scalar_op_default_nop() const;
    // No effect at all (for translation).
    bool is_nop() const;
    // A "normal" eA write (mad with a stream constant): its float constant, else UINT32_MAX.
    uint32_t memexport_stream_constant() const;
};

// One ALU or fetch instruction of an exec.
struct ExecSlot {
    uint32_t address = 0;   // instruction index (3 dwords each)
    bool serialize = false; // the sequence's second bit
    std::variant<AluInstruction, VertexFetchInstruction, TextureFetchInstruction> instruction;
};

struct Exec {
    ExecInstruction cf;
    std::vector<ExecSlot> slots;
};

struct ControlFlow {
    uint32_t index = 0; // pair * 2 + which
    ControlFlowOpcode opcode = ControlFlowOpcode::Nop;
    // monostate: nop and mark_vs_fetch_done
    std::variant<std::monostate, Exec, LoopStartInstruction, LoopEndInstruction, CallInstruction, ReturnInstruction,
                 JumpInstruction, AllocInstruction>
        instruction;
};

struct ParsedShader {
    ShaderType type = ShaderType::Vertex;
    std::vector<uint32_t> dwords;      // host order
    uint32_t cf_pair_bound = 0;        // the control flow's pairs: up to the first exec's instructions
    std::set<uint32_t> label_addresses; // control flow indices targeted by calls, jumps and loops
    std::vector<ControlFlow> control_flow;
    bool reads_past_end = false; // an exec's instructions lie past the microcode (read as zeros)
};

// dwords in host order: a guest's big-endian words already swapped.
ParsedShader parse_shader(ShaderType type, std::span<const uint32_t> dwords);

// A vertex shader as Direct3D keeps it in a shader container, before a
// draw binds it: a full vertex fetch with no format (0, which no vertex
// format is) and no stride. Direct3D writes each fetch's format, stride
// and offset into the microcode from the vertex declaration when it binds
// the shader (the container lists the fetches' addresses by usage, which
// XenosRecomp's shader_recompiler.cpp reads for the same reason), so what
// the GPU runs is that patched copy, under another hash.
bool awaits_vertex_declaration(const ParsedShader &shader);

// The microcode as Xenia disassembles it (Shader::AnalyzeUcode), character for character.
std::string disassemble(const ParsedShader &shader);

} // namespace xf::ucode
