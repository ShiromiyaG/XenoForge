// Xenos shaders to SPIR-V. Written anew on XenoForge's SPIR-V writer; the
// value each instruction computes follows Xenia's spirv_shader_translator_alu.cc
// and spirv_shader_translator_fetch.cc (BSD; see LICENSE-xenia): the legacy
// multiply (0 times anything is 0), multiplies and adds without contraction,
// the ordered and unordered comparisons, the 21-bit results of exp, log,
// rcp, rsq and sqrt, the clamps of rcpc, rcpf, rsqc, rsqf and logc, maxa's
// address register, the vertex formats and their normalization, the texture
// fetch's LOD and gradients, its signs, gamma and exponent adjust, the
// pixel parameters (spirv_shader_translator.cc's StartFragmentShaderInMain),
// and memexport (spirv_shader_translator_memexport.cc, and the D3D12
// translator's dxbc_shader_translator_memexport.cc where the two differ:
// integer components clamped to their range; with where an export ends from
// shader_translator.cc's Shader::AnalyzeUcode): eA and eM0-eM4 packed into
// the stream's format and stored into guest memory, the storage buffer the
// vertex fetches read. The vertex shader's host forms (HostVertex) follow
// Xenia's too: the domain shaders' inputs dxbc_shader_translator.cc's
// StartVertexOrDomainShader, their execution modes and the point sprites
// expanded in the vertex shader spirv_shader_translator.cc.
//
// The state is Private: r0-r127 (a vec4 each), a0, aL, the predicate, the
// previous scalar, the vertex fetch address, the exports. The body is a
// function, so an exec that ends the shader returns from it; each stage's
// main fills the registers, calls it, and passes the exports on.
//
// The control flow runs straight when it is only execs (plain, on a bool
// constant, on the predicate), allocs and nops; with loops, calls or jumps,
// as a loop over a switch on the next control flow instruction, with the
// interpreter's stacks (four loops deep, four calls deep) and step limit.
// Unnormalized cube map coordinates and the texture queries other than
// getGradients, getCompTexLOD and getWeights (those two on the host's
// images only, not in the harness) are reported as unsupported.

#include "xenoforge/shader_translator.h"

#include "xenoforge/shader_harness.h"
#include "xenoforge/spirv.h"

#include <GLSL.std.450.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>

namespace xf::shader {
namespace {

using namespace ucode;
using spirv::Id;
using spv::Op;

constexpr uint32_t OUTPUT_VEC4S = sizeof(harness::CaseOutput) / 16;
constexpr uint32_t OUT_INTERPOLATORS = harness::REGISTERS, OUT_POSITION = OUT_INTERPOLATORS + 16,
                   OUT_POINT_SIZE = OUT_POSITION + 1, OUT_COLORS = OUT_POINT_SIZE + 1, OUT_DEPTH = OUT_COLORS + 4,
                   OUT_MISC = OUT_DEPTH + 1;
static_assert(OUT_MISC + 1 == OUTPUT_VEC4S);

class Translator {
public:
  Translator(const ParsedShader &shader, Stage stage, const VertexOptions &vertex, const PixelOptions &pixel)
      : s_(shader), stage_(stage), vertex_(vertex), pixel_(pixel) {}
  Translation run();

private:
    // --- values -----------------------------------------------------------
    Id f(float v) { return m_.const_float(v); }
    Id i(int32_t v) { return m_.const_int(v); }
    Id u(uint32_t v) { return m_.const_uint(v); }
    Id vec4_of(Id x, Id y, Id z, Id w) { return m_.op(Op::OpCompositeConstruct, vec4_, {x, y, z, w}); }
    Id splat(Id x) { return vec4_of(x, x, x, x); }
    Id splat_bool(Id b) { return m_.op(Op::OpCompositeConstruct, bvec4_, {b, b, b, b}); }
    Id vec4_const(float x, float y, float z, float w) {
        const Id parts[] = {f(x), f(y), f(z), f(w)};
        return m_.const_composite(vec4_, parts);
    }
    Id comp(Id v, uint32_t c) { return m_.op(Op::OpCompositeExtract, float_, {v, c}); }
    Id load(Id type, Id pointer) { return m_.op(Op::OpLoad, type, {pointer}); }
    void store(Id pointer, Id value) { m_.op_void(Op::OpStore, {pointer, value}); }
    // a float operation kept as written (no fused multiply-add)
    Id exact(Op op, Id type, std::initializer_list<uint32_t> operands) {
        Id r = m_.op(op, type, operands);
        m_.decorate(r, spv::Decoration::NoContraction);
        return r;
    }
    Id glsl(uint32_t instruction, Id type, std::initializer_list<Id> operands) {
        return m_.glsl(type, instruction, operands);
    }
    Id select(Id type, Id condition, Id a, Id b) { return m_.op(Op::OpSelect, type, {condition, a, b}); }
    Id is_zero(Id type_bool, Id x, Id zero) {
        return m_.op(Op::OpFOrdEqual, type_bool, {glsl(GLSLstd450FAbs, type_bool == bool_ ? float_ : vec4_, {x}), zero});
    }
    // Xenos's multiply: 0 times anything (infinity, NaN) is 0
    Id legacy_mul(Id a, Id b, bool vector) {
        Id type = vector ? vec4_ : float_, btype = vector ? bvec4_ : bool_, zero = vector ? zero4_ : f(0);
        Id product = exact(Op::OpFMul, type, {a, b});
        Id either = m_.op(Op::OpLogicalOr, btype, {is_zero(btype, a, zero), is_zero(btype, b, zero)});
        return select(type, either, zero, product);
    }
    // the hardware's precision for exp, log, rcp, rsq, sqrt: 21 mantissa
    // bits, rounded, not rounding into infinity, infinities and NaN untouched
    Id reduce_precision(Id value) {
        const uint32_t truncate_bits = 23 - 21, mask = ~((1u << truncate_bits) - 1);
        Id bits = m_.op(Op::OpBitcast, uint_, {value});
        Id truncated = m_.op(Op::OpBitwiseAnd, uint_, {bits, u(mask)});
        Id discarded = m_.op(Op::OpBitwiseAnd, uint_, {bits, u(~mask)});
        Id round_up = m_.op(Op::OpUGreaterThanEqual, bool_, {discarded, u(1u << (truncate_bits - 1))});
        Id rounded = m_.op(Op::OpIAdd, uint_, {truncated, u(1u << truncate_bits)});
        Id exponent_mask = u(0x7F800000);
        Id finite = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {bits, exponent_mask}), exponent_mask});
        Id to_infinity = m_.op(Op::OpIEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {rounded, exponent_mask}), exponent_mask});
        Id safe = select(uint_, m_.op(Op::OpLogicalAnd, bool_, {finite, to_infinity}), truncated, rounded);
        Id result = select(uint_, round_up, safe, truncated);
        result = select(uint_, finite, result, bits);
        return m_.op(Op::OpBitcast, float_, {result});
    }

    // --- control ------------------------------------------------------------
    // if (condition) { then() }
    template <typename F> void when(Id condition, F then) {
        Id then_label = m_.id(), merge = m_.id();
        m_.op_void(Op::OpSelectionMerge, {merge, uint32_t(spv::SelectionControlMask::MaskNone)});
        m_.op_void(Op::OpBranchConditional, {condition, then_label, merge});
        m_.label(then_label);
        bool returned = then();
        if (!returned) m_.op_void(Op::OpBranch, {merge});
        m_.label(merge);
    }
    Id predicate_is(bool condition) {
        Id p = load(bool_, predicate_);
        return condition ? p : m_.op(Op::OpLogicalNot, bool_, {p});
    }

    // --- state and resources ----------------------------------------------
    void declare();
    Id global(Id pointee, spv::StorageClass storage, const char *name, Id initializer = 0) {
        Id v = m_.global_variable(m_.type_pointer(storage, pointee), storage, initializer);
        m_.name(v, name);
        interface_.push_back(v);
        return v;
    }
    Id constants_member(uint32_t member, Id index, Id element) {
        return m_.op(Op::OpAccessChain, m_.type_pointer(constants_storage_, element), {constants_, u(member), index});
    }
    // dword d of a 16-byte-element member (bools 1, loops 2, fetch constants 3)
    Id constant_dword(uint32_t member, uint32_t d) {
        Id vector = load(uvec4_, constants_member(member, u(d >> 2), uvec4_));
        return m_.op(Op::OpCompositeExtract, uint_, {vector, d & 3});
    }
    Id bool_constant(uint32_t b) {
        Id word = constant_dword(1, b >> 5);
        Id bit = m_.op(Op::OpBitwiseAnd, uint_, {m_.op(Op::OpShiftRightLogical, uint_, {word, u(b & 31)}), u(1)});
        return m_.op(Op::OpINotEqual, bool_, {bit, u(0)});
    }

    // --- operands and results ---------------------------------------------
    Id relative_base(StorageAddressing addressing) {
        return load(int_, addressing == StorageAddressing::AddressRegisterRelative ? a0_ : aL_);
    }
    Id register_pointer(uint32_t index, StorageAddressing addressing) {
        Id at = addressing == StorageAddressing::Absolute
                    ? i(int32_t(index & 127))
                    : m_.op(Op::OpBitwiseAnd, int_, {m_.op(Op::OpIAdd, int_, {relative_base(addressing), i(int32_t(index))}), i(127)});
        return m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, at});
    }
    Id load_storage(const Operand &o) {
        if (o.source == StorageSource::Register) return load(vec4_, register_pointer(o.index, o.addressing));
        // (the microcode's ALU operands are registers or float constants by
        // their source select bit, all ucode_parse makes of them; the fetch
        // constants are the fetches' operands only, as in Xenia's parser)
        if (o.source != StorageSource::ConstantFloat) {
            unsupported("an ALU operand neither a register nor a float constant");
            return zero4_;
        }
        if (o.addressing == StorageAddressing::Absolute) {
            if (o.index > 255) return zero4_;
            return load(vec4_, constants_member(0, u(o.index), vec4_));
        }
        // relative: outside 0-255 reads zeros
        Id at = m_.op(Op::OpIAdd, int_, {relative_base(o.addressing), i(int32_t(o.index))});
        Id inside = m_.op(Op::OpULessThan, bool_, {m_.op(Op::OpBitcast, uint_, {at}), u(256)});
        Id clamped = m_.op(Op::OpBitcast, uint_, {glsl(GLSLstd450SClamp, int_, {at, i(0), i(255)})});
        return select(vec4_, inside, load(vec4_, constants_member(0, clamped, vec4_)), zero4_);
    }
    Id modifiers(Id v, const Operand &o) {
        if (o.absolute) v = glsl(GLSLstd450FAbs, vec4_, {v});
        if (o.negated) v = m_.op(Op::OpFNegate, vec4_, {v});
        return v;
    }
    // the operand swizzled, with its absolute value and negation
    Id operand(const Operand &o) {
        Id v = load_storage(o);
        if (!o.is_standard_swizzle()) {
            uint32_t c[4];
            for (uint32_t k = 0; k < 4; k++) c[k] = uint32_t(o.component(k)) & 3;
            v = m_.op(Op::OpVectorShuffle, vec4_, {v, v, c[0], c[1], c[2], c[3]});
        }
        return modifiers(v, o);
    }
    void store_result(const Result &r, Id value, bool clamp_depth = true);

    // --- instructions -------------------------------------------------------
    // true when the code it wrote ends the block (the shader returned)
    bool exec(const Exec &e);
    bool control_flow(const ControlFlow &cf, uint32_t index);
    void dispatch();
    Id taken(bool unconditional, bool predicated, uint32_t bool_index, bool condition) {
        if (unconditional) return m_.const_bool(true);
        if (predicated) return predicate_is(condition);
        Id b = bool_constant(bool_index);
        return condition ? b : m_.op(Op::OpLogicalNot, bool_, {b});
    }
    Id element(Id array, Id pointer_type, Id index) { return m_.op(Op::OpAccessChain, pointer_type, {array, index}); }
    void alu(const AluInstruction &a);
    Id vector_op(const AluInstruction &a);
    Id scalar_op(const AluInstruction &a);
    void kill(Id condition) {
        when(condition, [&] {
            // the exports under way are done first: a helper invocation's
            // stores are dropped (Xenia's KillPixel)
            export_to_memory(em_before_);
            store(killed_, m_.const_bool(true));
            if (stage_ == Stage::Pixel) m_.op_void(Op::OpDemoteToHelperInvocation);
            return false;
        });
    }
    void set_address_register(Id value, float low, float high) {
        store(a0_, m_.op(Op::OpConvertFToS, int_, {glsl(GLSLstd450NClamp, float_, {glsl(GLSLstd450Floor, float_, {value}), f(low), f(high)})}));
    }
    void vertex_fetch(const VertexFetchInstruction &v);
    void texture_fetch(const TextureFetchInstruction &t);
    // member: the system constants' slot table the image comes from (2: the
    // textures, 8: their signed views)
    // what sample_texture gives: the sample, getCompTexLOD's LOD, or
    // getWeights's filtering weights
    enum class Query { Sample, Lod, Weights };
    Id sample_texture(const TextureFetchInstruction &t, Id coordinates, uint32_t member = 2, Query query = Query::Sample);
    Id texture_signs(const TextureFetchInstruction &t, Id coordinates, Id sample);
    Id fetch_post(uint32_t used, bool point_allowed, Id sample, Id signed_sample, Id info, Id scale_word);
    void emit_fetch_posts();
    std::map<uint32_t, Id> fetch_posts_; // texture_signs' functions by components kept and point sampling
    static bool allows_point_sampling(const TextureFetchInstruction &t);
    Id pwl_gamma_to_linear(Id gamma);
    Id slot_word(uint32_t member, uint32_t slot);
    Id swap_endian(Id word, Id endian);

    // --- memexport (Xenia's spirv_shader_translator_memexport.cc) ----------
    void analyze_memexport();
    void alloc(const AllocInstruction &a);
    void export_to_memory(uint8_t em);
    Id pack_half2(Id xy);
    Id swap_endian_128(Id words, Id endian);
    void store_memory_word(Id value, Id dword_index, Id replace_mask = 0);

    void unsupported(std::string what) {
        for (const std::string &u : unsupported_)
            if (u == what) return;
        unsupported_.push_back(std::move(what));
    }

    void main_harness();
    void main_vertex();
    void main_pixel();
    void reset_state();

    const ParsedShader &s_;
    Stage stage_;
    VertexOptions vertex_;
    PixelOptions pixel_;
    spirv::Module m_;
    std::vector<std::string> unsupported_;
    std::vector<std::string> approximate_; // Translation::approximate
    std::vector<Id> interface_;

    Id void_ = 0, bool_ = 0, int_ = 0, uint_ = 0, float_ = 0, vec2_ = 0, vec4_ = 0, uvec4_ = 0, bvec4_ = 0;
    Id ptr_private_vec4_ = 0, zero4_ = 0;
    spv::StorageClass constants_storage_ = spv::StorageClass::Uniform;
    Id constants_ = 0, memory_ = 0, memory_ptr_uint_ = 0, system_ = 0;
    Id images2d_ = 0, images3d_ = 0, imagescube_ = 0, samplers_ = 0, image2d_type_ = 0, image3d_type_ = 0,
       imagecube_type_ = 0, sampler_type_ = 0;
    Id registers_ = 0, predicate_ = 0, a0_ = 0, aL_ = 0, previous_scalar_ = 0, vfetch_address_ = 0, vfetch_bound_ = 0;
    Id killed_ = 0, interpolators_ = 0, position_ = 0, point_size_ = 0, colors_ = 0, depth_ = 0;
    // loops and calls: four-deep stacks of counts, of aL (aL_ is the top's
    // copy), of return addresses; the next control flow instruction
    Id loop_count_ = 0, loop_address_ = 0, call_stack_ = 0, call_depth_ = 0, pc_ = 0, steps_ = 0;
    Id ptr_private_uint_ = 0, ptr_private_int_ = 0;
    // the texture LOD and gradients the setters leave for later fetches
    Id tfetch_lod_ = 0, gradient_h_ = 0, gradient_v_ = 0;
    Id signed_sample_ = 0; // a fetch's sample from the texture's signed view
    Id fetch_scaled_ = 0;  // a fetch's result through its number format (texture_signs)
    Id body_ = 0;
    bool ended_ = false; // the straight control flow has returned
    // memexport: which eM# the shader writes; per control flow instruction,
    // which may have been written before it since the last export, and
    // before the end (Shader::AnalyzeUcode's cf_memexport_info); the same
    // within the exec being translated
    uint8_t em_written_ = 0, em_before_end_ = 0, em_before_ = 0;
    std::vector<uint8_t> em_before_cf_;
    // eA, eM0-eM4, the eM# written since the last export (a bit each), and
    // an export's packed words and element size (log2 bytes; 5 for none)
    Id export_address_ = 0, export_data_[5] = {}, export_written_ = 0, export_packed_[5] = {}, export_size_ = 0;
    // whether this invocation's exports are stored (a bool; 0: always), for
    // the host vertex types that run a guest vertex in several invocations
    Id export_allowed_ = 0;
    // the interlock path (PixelOptions::fsi): the color targets written on
    // the shader's path (a bit each), the library's xf_fsi and its parameters
    Id colors_written_ = 0, fsi_function_ = 0;
    std::vector<Id> fsi_parameters_;
};

void Translator::declare() {
    m_.capability(spv::Capability::Shader);
    m_.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    void_ = m_.type_void();
    bool_ = m_.type_bool();
    int_ = m_.type_sint();
    uint_ = m_.type_uint();
    float_ = m_.type_float();
    vec2_ = m_.type_vector(float_, 2);
    vec4_ = m_.type_vector(float_, 4);
    uvec4_ = m_.type_vector(uint_, 4);
    bvec4_ = m_.type_vector(bool_, 4);
    ptr_private_vec4_ = m_.type_pointer(spv::StorageClass::Private, vec4_);
    zero4_ = vec4_const(0, 0, 0, 0);

    // the constants: 256 float4, 2 uvec4 of bools, 8 of loops, 48 of fetch constants
    constants_storage_ = stage_ == Stage::Harness ? spv::StorageClass::StorageBuffer : spv::StorageClass::Uniform;
    const Id members[] = {m_.type_array(vec4_, 256, 16), m_.type_array(uvec4_, 2, 16), m_.type_array(uvec4_, 8, 16),
                          m_.type_array(uvec4_, 48, 16)};
    Id block = m_.type_struct(members);
    m_.decorate(block, spv::Decoration::Block);
    for (uint32_t k = 0, offset = 0; k < 4; k++) {
        m_.member_decorate(block, k, spv::Decoration::Offset, {offset});
        offset += k == 0 ? 4096 : k == 1 ? 32 : k == 2 ? 128 : 768;
    }
    constants_ = global(block, constants_storage_, "constants");
    m_.decorate(constants_, spv::Decoration::DescriptorSet, {0});
    // the vertex and the pixel shader read their own c0-c255
    m_.decorate(constants_, spv::Decoration::Binding, {stage_ == Stage::Harness ? 1u : stage_ == Stage::Pixel ? 3u : 0u});
    if (stage_ == Stage::Harness) m_.decorate(constants_, spv::Decoration::NonWritable);

    // guest memory, as words
    Id words = m_.type_runtime_array(uint_, 4);
    const Id memory_members[] = {words};
    Id memory_block = m_.type_struct(memory_members);
    m_.decorate(memory_block, spv::Decoration::Block);
    m_.member_decorate(memory_block, 0, spv::Decoration::Offset, {0});
    memory_ = global(memory_block, spv::StorageClass::StorageBuffer, "memory");
    m_.decorate(memory_, spv::Decoration::DescriptorSet, {0});
    m_.decorate(memory_, spv::Decoration::Binding, {stage_ == Stage::Harness ? 2u : 1u});
    // written by memexport
    if (!em_written_) m_.decorate(memory_, spv::Decoration::NonWritable);
    memory_ptr_uint_ = m_.type_pointer(spv::StorageClass::StorageBuffer, uint_);
    if (em_written_) {
        export_address_ = global(vec4_, spv::StorageClass::Private, "eA");
        for (uint32_t k = 0; k < 5; k++) {
            if (!(em_written_ & (1u << k))) continue;
            export_data_[k] = global(vec4_, spv::StorageClass::Private, "eM");
            export_packed_[k] = global(uvec4_, spv::StorageClass::Private, "eM_packed");
        }
        export_written_ = global(uint_, spv::StorageClass::Private, "eM_written");
        export_size_ = global(uint_, spv::StorageClass::Private, "eM_size");
        if (stage_ == Stage::Vertex && (vertex_.host == HostVertex::PointListAsTriangleStrip ||
                                        vertex_.host == HostVertex::RectangleListAsTriangleStrip))
            export_allowed_ = global(bool_, spv::StorageClass::Private, "export_allowed");
    }

    if (stage_ != Stage::Harness) {
        // ndc_scale, ndc_offset, the image and sampler index of each fetch
        // slot, then flags (x: the alpha test, bit 3 on, bits 0-2 its
        // function; y: the position's formats, bits 8-15 the pixel
        // parameters; z: gamma targets; w: D24FS8 depth), values (x: the
        // alpha test's reference, y and z: the point
        // diameter's bounds), the color targets' scales and the point
        // constants (renderer.cpp's System, point_list.geom), then per fetch
        // slot its texture's signs and the signed view's image and sampler,
        // then the user clip planes enabled, packed (VertexOptions), then the
        // screen extents' map from the host's NDC to console pixels (x scale
        // and offset, y scale and offset) and the report's first word
        // (VertexOptions::extents), then per fetch slot its texture's integer
        // scale word (texture::integer_scale_bits, Xenia's GetIntegerScaleBits),
        // then per color target the factors a minimum or maximum blend's
        // source is multiplied by here (PixelOptions::blend_premultiply) and
        // the blend constant
        Id slot_table = m_.type_array(uvec4_, 8, 16);
        const Id system_members[] = {vec4_, vec4_, slot_table, uvec4_,     vec4_,
                                     vec4_, vec4_, slot_table, slot_table, m_.type_array(vec4_, 6, 16),
                                     vec4_, uvec4_, slot_table, uvec4_, vec4_};
        Id system_block = m_.type_struct(system_members);
        m_.decorate(system_block, spv::Decoration::Block);
        m_.member_decorate(system_block, 0, spv::Decoration::Offset, {0});
        m_.member_decorate(system_block, 1, spv::Decoration::Offset, {16});
        m_.member_decorate(system_block, 2, spv::Decoration::Offset, {32});
        m_.member_decorate(system_block, 3, spv::Decoration::Offset, {160});
        m_.member_decorate(system_block, 4, spv::Decoration::Offset, {176});
        m_.member_decorate(system_block, 5, spv::Decoration::Offset, {192});
        m_.member_decorate(system_block, 6, spv::Decoration::Offset, {208});
        m_.member_decorate(system_block, 7, spv::Decoration::Offset, {224});
        m_.member_decorate(system_block, 8, spv::Decoration::Offset, {352});
        m_.member_decorate(system_block, 9, spv::Decoration::Offset, {480});
        m_.member_decorate(system_block, 10, spv::Decoration::Offset, {576});
        m_.member_decorate(system_block, 11, spv::Decoration::Offset, {592});
        m_.member_decorate(system_block, 12, spv::Decoration::Offset, {608});
        m_.member_decorate(system_block, 13, spv::Decoration::Offset, {736});
        m_.member_decorate(system_block, 14, spv::Decoration::Offset, {752});
        system_ = global(system_block, spv::StorageClass::Uniform, "system");
        m_.decorate(system_, spv::Decoration::DescriptorSet, {0});
        m_.decorate(system_, spv::Decoration::Binding, {2});
    }

    // the shader's state
    registers_ = global(m_.type_array(vec4_, harness::REGISTERS), spv::StorageClass::Private, "r");
    predicate_ = global(bool_, spv::StorageClass::Private, "p0");
    a0_ = global(int_, spv::StorageClass::Private, "a0");
    aL_ = global(int_, spv::StorageClass::Private, "aL");
    previous_scalar_ = global(float_, spv::StorageClass::Private, "ps");
    vfetch_address_ = global(int_, spv::StorageClass::Private, "vfetch_address");
    vfetch_bound_ = global(int_, spv::StorageClass::Private, "vfetch_bound");
    killed_ = global(bool_, spv::StorageClass::Private, "killed");
    interpolators_ = global(m_.type_array(vec4_, 16), spv::StorageClass::Private, "o");
    position_ = global(vec4_, spv::StorageClass::Private, "oPos");
    point_size_ = global(vec4_, spv::StorageClass::Private, "oPts");
    colors_ = global(m_.type_array(vec4_, 4), spv::StorageClass::Private, "oC");
    if (stage_ == Stage::Pixel && pixel_.fsi) colors_written_ = global(uint_, spv::StorageClass::Private, "oC_written");
    depth_ = global(vec4_, spv::StorageClass::Private, "oDepth");
    ptr_private_uint_ = m_.type_pointer(spv::StorageClass::Private, uint_);
    ptr_private_int_ = m_.type_pointer(spv::StorageClass::Private, int_);
    loop_count_ = global(m_.type_array(uint_, 4), spv::StorageClass::Private, "loop_count");
    loop_address_ = global(m_.type_array(int_, 4), spv::StorageClass::Private, "loop_address");
    call_stack_ = global(m_.type_array(uint_, 4), spv::StorageClass::Private, "call_stack");
    call_depth_ = global(uint_, spv::StorageClass::Private, "call_depth");
    pc_ = global(uint_, spv::StorageClass::Private, "pc");
    steps_ = global(uint_, spv::StorageClass::Private, "steps");
    tfetch_lod_ = global(float_, spv::StorageClass::Private, "tfetch_lod");
    gradient_h_ = global(vec4_, spv::StorageClass::Private, "gradient_h");
    gradient_v_ = global(vec4_, spv::StorageClass::Private, "gradient_v");
    signed_sample_ = global(vec4_, spv::StorageClass::Private, "signed_sample");
    fetch_scaled_ = global(vec4_, spv::StorageClass::Private, "fetch_scaled");
}

void Translator::reset_state() {
    store(predicate_, m_.const_bool(false));
    store(a0_, i(0));
    store(aL_, i(0));
    store(previous_scalar_, f(0));
    store(vfetch_address_, i(0));
    store(vfetch_bound_, i(0));
    store(killed_, m_.const_bool(false));
    for (uint32_t k = 0; k < 16; k++)
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {interpolators_, i(int32_t(k))}), zero4_);
    store(position_, zero4_);
    store(point_size_, zero4_);
    for (uint32_t k = 0; k < 4; k++) store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(int32_t(k))}), zero4_);
    if (colors_written_) store(colors_written_, u(0));
    store(depth_, zero4_);
    store(loop_count_, m_.const_null(m_.type_array(uint_, 4)));
    store(loop_address_, m_.const_null(m_.type_array(int_, 4)));
    store(call_stack_, m_.const_null(m_.type_array(uint_, 4)));
    store(call_depth_, u(0));
    store(tfetch_lod_, f(0));
    store(gradient_h_, zero4_);
    store(gradient_v_, zero4_);
    if (em_written_) {
        store(export_address_, zero4_);
        for (uint32_t k = 0; k < 5; k++)
            if (export_data_[k]) store(export_data_[k], zero4_);
        store(export_written_, u(0));
    }
}

void Translator::store_result(const Result &r, Id value, bool clamp_depth) {
    uint32_t mask = r.used_write_mask();
    if (!mask) return;
    Id pointer = 0;
    switch (r.target) {
    case StorageTarget::None: return;
    case StorageTarget::Register: pointer = register_pointer(r.index, r.addressing); break;
    case StorageTarget::Interpolator:
        pointer = m_.op(Op::OpAccessChain, ptr_private_vec4_, {interpolators_, i(int32_t(r.index & 15))});
        break;
    case StorageTarget::Position: pointer = position_; break;
    case StorageTarget::PointSizeEdgeFlagKillVertex: pointer = point_size_; break;
    case StorageTarget::Color:
        pointer = m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(int32_t(r.index & 3))});
        if (colors_written_)
            store(colors_written_, m_.op(Op::OpBitwiseOr, uint_, {load(uint_, colors_written_), u(1u << (r.index & 3))}));
        break;
    case StorageTarget::Depth: pointer = depth_; break;
    // eA without any eM# written exports nothing (Xenia drops it)
    case StorageTarget::ExportAddress:
        if (!em_written_) return;
        pointer = export_address_;
        break;
    case StorageTarget::ExportData:
        if (r.index >= 5 || !export_data_[r.index]) return;
        pointer = export_data_[r.index];
        store(export_written_, m_.op(Op::OpBitwiseOr, uint_, {load(uint_, export_written_), u(1u << r.index)}));
        break;
    }
    uint32_t constant_values = 0;
    uint32_t constants = r.used_constant_components(constant_values);
    if (!value) constants = mask;
    Id result = load(vec4_, pointer);
    for (uint32_t k = 0; k < 4; k++) {
        if (!(mask & (1u << k))) continue;
        Id component;
        if (constants & (1u << k)) {
            component = f((constant_values >> k) & 1 ? 1.0f : 0.0f);
        } else {
            component = comp(value, uint32_t(r.components[k]) & 3);
            if (r.clamped || (r.target == StorageTarget::Depth && clamp_depth))
                component = glsl(GLSLstd450NClamp, float_, {component, f(0), f(1)});
        }
        result = m_.op(Op::OpCompositeInsert, vec4_, {component, result, k});
    }
    store(pointer, result);
}

Id Translator::vector_op(const AluInstruction &a) {
    const Result &r = a.vector_and_constant_result;
    if (!r.used_result_components() && !vector_opcode_info(a.vector_opcode).changed_state) return 0;
    uint32_t count = a.vector_opcode == AluVectorOpcode::Cube ? 1 : a.vector_operand_count;
    Id o[3] = {};
    for (uint32_t k = 0; k < count; k++) o[k] = operand(a.vector_operands[k]);
    auto compare = [&](Op op, Id x, Id y) { return m_.op(op, bvec4_, {x, y}); };
    auto ones = vec4_const(1, 1, 1, 1);

    switch (a.vector_opcode) {
    case AluVectorOpcode::Add: return exact(Op::OpFAdd, vec4_, {o[0], o[1]});
    case AluVectorOpcode::Mul: return legacy_mul(o[0], o[1], true);
    case AluVectorOpcode::Mad: return exact(Op::OpFAdd, vec4_, {legacy_mul(o[0], o[1], true), o[2]});
    case AluVectorOpcode::Max:
    case AluVectorOpcode::MaxA:
        if (a.vector_opcode == AluVectorOpcode::MaxA)
            set_address_register(exact(Op::OpFAdd, float_, {comp(o[0], 3), f(0.5f)}), -256.0f, 255.0f);
        return select(vec4_, compare(Op::OpFOrdGreaterThanEqual, o[0], o[1]), o[0], o[1]);
    case AluVectorOpcode::Min: return select(vec4_, compare(Op::OpFOrdLessThan, o[0], o[1]), o[0], o[1]);
    case AluVectorOpcode::Seq: return select(vec4_, compare(Op::OpFOrdEqual, o[0], o[1]), ones, zero4_);
    case AluVectorOpcode::Sgt: return select(vec4_, compare(Op::OpFOrdGreaterThan, o[0], o[1]), ones, zero4_);
    case AluVectorOpcode::Sge: return select(vec4_, compare(Op::OpFOrdGreaterThanEqual, o[0], o[1]), ones, zero4_);
    case AluVectorOpcode::Sne: return select(vec4_, compare(Op::OpFUnordNotEqual, o[0], o[1]), ones, zero4_);
    case AluVectorOpcode::Frc: return glsl(GLSLstd450Fract, vec4_, {o[0]});
    case AluVectorOpcode::Trunc: return glsl(GLSLstd450Trunc, vec4_, {o[0]});
    case AluVectorOpcode::Floor: return glsl(GLSLstd450Floor, vec4_, {o[0]});
    case AluVectorOpcode::CndEq: return select(vec4_, compare(Op::OpFOrdEqual, o[0], zero4_), o[1], o[2]);
    case AluVectorOpcode::CndGe: return select(vec4_, compare(Op::OpFOrdGreaterThanEqual, o[0], zero4_), o[1], o[2]);
    case AluVectorOpcode::CndGt: return select(vec4_, compare(Op::OpFOrdGreaterThan, o[0], zero4_), o[1], o[2]);
    case AluVectorOpcode::Dp4:
    case AluVectorOpcode::Dp3:
    case AluVectorOpcode::Dp2Add: {
        uint32_t n = a.vector_opcode == AluVectorOpcode::Dp4 ? 4 : a.vector_opcode == AluVectorOpcode::Dp3 ? 3 : 2;
        Id sum = 0;
        for (uint32_t k = 0; k < n; k++) {
            Id product = legacy_mul(comp(o[0], k), comp(o[1], k), false);
            sum = k ? exact(Op::OpFAdd, float_, {sum, product}) : product;
        }
        if (a.vector_opcode == AluVectorOpcode::Dp2Add) sum = exact(Op::OpFAdd, float_, {sum, comp(o[2], 0)});
        return splat(sum);
    }
    case AluVectorOpcode::Cube: {
        // the operand's z, w, x as the cube's x, y, z
        Id x = comp(o[0], 2), y = comp(o[0], 3), z = comp(o[0], 0);
        Id ax = glsl(GLSLstd450FAbs, float_, {x}), ay = glsl(GLSLstd450FAbs, float_, {y}),
           az = glsl(GLSLstd450FAbs, float_, {z});
        Id nx = m_.op(Op::OpFNegate, float_, {x}), ny = m_.op(Op::OpFNegate, float_, {y}),
           nz = m_.op(Op::OpFNegate, float_, {z});
        auto negative = [&](Id v) { return m_.op(Op::OpFOrdLessThan, bool_, {v, f(0)}); };
        Id z_major = m_.op(Op::OpLogicalAnd, bool_, {m_.op(Op::OpFOrdGreaterThanEqual, bool_, {az, ax}),
                                                     m_.op(Op::OpFOrdGreaterThanEqual, bool_, {az, ay})});
        Id y_major = m_.op(Op::OpFOrdGreaterThanEqual, bool_, {ay, ax});
        Id on_z = vec4_of(ny, select(float_, negative(z), nx, x), z, select(float_, negative(z), f(5), f(4)));
        Id on_y = vec4_of(select(float_, negative(y), nz, z), x, y, select(float_, negative(y), f(3), f(2)));
        Id on_x = vec4_of(ny, select(float_, negative(x), z, nz), x, select(float_, negative(x), f(1), f(0)));
        Id result = select(vec4_, z_major, on_z, select(vec4_, y_major, on_y, on_x));
        return m_.op(Op::OpCompositeInsert, vec4_, {exact(Op::OpFMul, float_, {f(2), comp(result, 2)}), result, 2});
    }
    case AluVectorOpcode::Max4: {
        // the largest of the components the swizzle names, NaN passed over
        const Operand &op0 = a.vector_operands[0];
        uint32_t named = 0;
        for (uint32_t k = 0; k < 4; k++) named |= 1u << (uint32_t(op0.component(k)) & 3);
        Id modified = modifiers(load_storage(op0), op0);
        Id result = 0;
        for (uint32_t k = 0; k < 4; k++) {
            if (!(named & (1u << k))) continue;
            result = result ? glsl(GLSLstd450NMax, float_, {result, comp(modified, k)}) : comp(modified, k);
        }
        return splat(result);
    }
    case AluVectorOpcode::SetpEqPush:
    case AluVectorOpcode::SetpNePush:
    case AluVectorOpcode::SetpGtPush:
    case AluVectorOpcode::SetpGePush: {
        Op op = a.vector_opcode == AluVectorOpcode::SetpEqPush   ? Op::OpFOrdEqual
                : a.vector_opcode == AluVectorOpcode::SetpNePush ? Op::OpFUnordNotEqual
                : a.vector_opcode == AluVectorOpcode::SetpGtPush ? Op::OpFOrdGreaterThan
                                                                 : Op::OpFOrdGreaterThanEqual;
        auto test = [&](uint32_t c) {
            return m_.op(Op::OpLogicalAnd, bool_, {m_.op(Op::OpFOrdEqual, bool_, {comp(o[0], c), f(0)}),
                                                   m_.op(op, bool_, {comp(o[1], c), f(0)})});
        };
        store(predicate_, test(3));
        return splat(exact(Op::OpFAdd, float_, {select(float_, test(0), f(-1), comp(o[0], 0)), f(1)}));
    }
    case AluVectorOpcode::KillEq:
    case AluVectorOpcode::KillGt:
    case AluVectorOpcode::KillGe:
    case AluVectorOpcode::KillNe: {
        Op op = a.vector_opcode == AluVectorOpcode::KillEq   ? Op::OpFOrdEqual
                : a.vector_opcode == AluVectorOpcode::KillGt ? Op::OpFOrdGreaterThan
                : a.vector_opcode == AluVectorOpcode::KillGe ? Op::OpFOrdGreaterThanEqual
                                                             : Op::OpFUnordNotEqual;
        kill(m_.op(Op::OpAny, bool_, {compare(op, o[0], o[1])}));
        return zero4_;
    }
    case AluVectorOpcode::Dst:
        return vec4_of(f(1), legacy_mul(comp(o[0], 1), comp(o[1], 1), false), comp(o[0], 2), comp(o[1], 3));
    }
    unsupported("an unknown vector operation");
    return 0;
}

Id Translator::scalar_op(const AluInstruction &a) {
    Id o0 = a.scalar_operand_count > 0 ? operand(a.scalar_operands[0]) : zero4_;
    Id o1 = a.scalar_operand_count > 1 ? operand(a.scalar_operands[1]) : zero4_;
    Id x = comp(o0, 0), y = comp(o0, 1);
    auto ps = [&] { return load(float_, previous_scalar_); };
    auto cmp = [&](Op op, Id p, Id q) { return m_.op(op, bool_, {p, q}); };
    auto one_or_zero = [&](Id condition) { return select(float_, condition, f(1), f(0)); };
    const float max = FLT_MAX;

    switch (a.scalar_opcode) {
    case AluScalarOpcode::Adds: return exact(Op::OpFAdd, float_, {x, y});
    case AluScalarOpcode::Subs: return exact(Op::OpFSub, float_, {x, y});
    case AluScalarOpcode::AddsPrev: return exact(Op::OpFAdd, float_, {x, ps()});
    case AluScalarOpcode::SubsPrev: return exact(Op::OpFSub, float_, {x, ps()});
    case AluScalarOpcode::Muls: return legacy_mul(x, y, false);
    case AluScalarOpcode::MulsPrev: return legacy_mul(x, ps(), false);
    case AluScalarOpcode::MulsPrev2: {
        Id p = ps();
        Id ok = cmp(Op::OpFUnordNotEqual, p, f(-max));
        ok = m_.op(Op::OpLogicalAnd, bool_, {ok, cmp(Op::OpFOrdGreaterThanEqual,
                                                       m_.op(Op::OpFNegate, float_, {glsl(GLSLstd450FAbs, float_, {p})}), f(-max))});
        ok = m_.op(Op::OpLogicalAnd, bool_, {ok, cmp(Op::OpFOrdGreaterThanEqual,
                                                       m_.op(Op::OpFNegate, float_, {glsl(GLSLstd450FAbs, float_, {y})}), f(-max))});
        ok = m_.op(Op::OpLogicalAnd, bool_, {ok, cmp(Op::OpFOrdGreaterThan, y, f(0))});
        return select(float_, ok, legacy_mul(x, p, false), f(-max));
    }
    case AluScalarOpcode::Maxs: return select(float_, cmp(Op::OpFOrdGreaterThanEqual, x, y), x, y);
    case AluScalarOpcode::Mins: return select(float_, cmp(Op::OpFOrdLessThan, x, y), x, y);
    case AluScalarOpcode::MaxAs:
    case AluScalarOpcode::MaxAsf:
        set_address_register(a.scalar_opcode == AluScalarOpcode::MaxAs ? exact(Op::OpFAdd, float_, {x, f(0.5f)}) : x,
                             0.0f, 255.0f);
        return select(float_, cmp(Op::OpFOrdGreaterThanEqual, x, y), x, y);
    case AluScalarOpcode::Seqs: return one_or_zero(cmp(Op::OpFOrdEqual, x, f(0)));
    case AluScalarOpcode::Sgts: return one_or_zero(cmp(Op::OpFOrdGreaterThan, x, f(0)));
    case AluScalarOpcode::Sges: return one_or_zero(cmp(Op::OpFOrdGreaterThanEqual, x, f(0)));
    case AluScalarOpcode::Snes: return one_or_zero(cmp(Op::OpFUnordNotEqual, x, f(0)));
    case AluScalarOpcode::Frcs: return glsl(GLSLstd450Fract, float_, {x});
    case AluScalarOpcode::Truncs: return glsl(GLSLstd450Trunc, float_, {x});
    case AluScalarOpcode::Floors: return glsl(GLSLstd450Floor, float_, {x});
    case AluScalarOpcode::Sin: return glsl(GLSLstd450Sin, float_, {x});
    case AluScalarOpcode::Cos: return glsl(GLSLstd450Cos, float_, {x});
    case AluScalarOpcode::Exp: return reduce_precision(glsl(GLSLstd450Exp2, float_, {x}));
    case AluScalarOpcode::Log: return reduce_precision(glsl(GLSLstd450Log2, float_, {x}));
    case AluScalarOpcode::Sqrt: return reduce_precision(glsl(GLSLstd450Sqrt, float_, {x}));
    case AluScalarOpcode::Rsq: return reduce_precision(glsl(GLSLstd450InverseSqrt, float_, {x}));
    case AluScalarOpcode::Rcp: return reduce_precision(exact(Op::OpFDiv, float_, {f(1), x}));
    case AluScalarOpcode::Logc: {
        Id r = reduce_precision(glsl(GLSLstd450Log2, float_, {x}));
        return select(float_, cmp(Op::OpFOrdEqual, r, f(-INFINITY)), f(-max), r);
    }
    case AluScalarOpcode::Rcpc:
    case AluScalarOpcode::Rsqc: {
        Id r = reduce_precision(a.scalar_opcode == AluScalarOpcode::Rcpc ? exact(Op::OpFDiv, float_, {f(1), x})
                                                                         : glsl(GLSLstd450InverseSqrt, float_, {x}));
        r = select(float_, cmp(Op::OpFOrdEqual, r, f(-INFINITY)), f(-max), r);
        return select(float_, cmp(Op::OpFOrdEqual, r, f(INFINITY)), f(max), r);
    }
    case AluScalarOpcode::Rcpf:
    case AluScalarOpcode::Rsqf: {
        Id r = reduce_precision(a.scalar_opcode == AluScalarOpcode::Rcpf ? exact(Op::OpFDiv, float_, {f(1), x})
                                                                         : glsl(GLSLstd450InverseSqrt, float_, {x}));
        r = select(float_, cmp(Op::OpFOrdEqual, r, f(INFINITY)), f(0), r);
        Id bits = select(uint_, cmp(Op::OpFOrdEqual, r, f(-INFINITY)), u(0x80000000u), m_.op(Op::OpBitcast, uint_, {r}));
        return m_.op(Op::OpBitcast, float_, {bits});
    }
    case AluScalarOpcode::SetpEq:
    case AluScalarOpcode::SetpNe:
    case AluScalarOpcode::SetpGt:
    case AluScalarOpcode::SetpGe: {
        Op op = a.scalar_opcode == AluScalarOpcode::SetpEq   ? Op::OpFOrdEqual
                : a.scalar_opcode == AluScalarOpcode::SetpNe ? Op::OpFUnordNotEqual
                : a.scalar_opcode == AluScalarOpcode::SetpGt ? Op::OpFOrdGreaterThan
                                                             : Op::OpFOrdGreaterThanEqual;
        Id p = cmp(op, x, f(0));
        store(predicate_, p);
        return select(float_, p, f(0), f(1));
    }
    case AluScalarOpcode::SetpInv: {
        Id p = cmp(Op::OpFOrdEqual, x, f(1));
        store(predicate_, p);
        return select(float_, p, f(0), select(float_, cmp(Op::OpFOrdEqual, x, f(0)), f(1), x));
    }
    case AluScalarOpcode::SetpPop: {
        Id t = exact(Op::OpFSub, float_, {x, f(1)});
        Id p = cmp(Op::OpFOrdLessThanEqual, t, f(0));
        store(predicate_, p);
        return select(float_, p, f(0), t);
    }
    case AluScalarOpcode::SetpClr: store(predicate_, m_.const_bool(false)); return f(max);
    case AluScalarOpcode::SetpRstr: {
        Id p = cmp(Op::OpFOrdEqual, x, f(0));
        store(predicate_, p);
        return select(float_, p, f(0), x);
    }
    case AluScalarOpcode::KillsEq: kill(cmp(Op::OpFOrdEqual, x, f(0))); return f(0);
    case AluScalarOpcode::KillsGt: kill(cmp(Op::OpFOrdGreaterThan, x, f(0))); return f(0);
    case AluScalarOpcode::KillsGe: kill(cmp(Op::OpFOrdGreaterThanEqual, x, f(0))); return f(0);
    case AluScalarOpcode::KillsNe: kill(cmp(Op::OpFUnordNotEqual, x, f(0))); return f(0);
    case AluScalarOpcode::KillsOne: kill(cmp(Op::OpFOrdEqual, x, f(1))); return f(0);
    case AluScalarOpcode::Mulsc0:
    case AluScalarOpcode::Mulsc1: return legacy_mul(x, comp(o1, 0), false);
    case AluScalarOpcode::Addsc0:
    case AluScalarOpcode::Addsc1: return exact(Op::OpFAdd, float_, {x, comp(o1, 0)});
    case AluScalarOpcode::Subsc0:
    case AluScalarOpcode::Subsc1: return exact(Op::OpFSub, float_, {x, comp(o1, 0)});
    case AluScalarOpcode::RetainPrev: return 0;
    case AluScalarOpcode::Opcode41: break;
    }
    unsupported("scalar opcode 41");
    return 0;
}

void Translator::alu(const AluInstruction &a) {
    if (a.is_nop()) return;
    auto body = [&] {
        // the vector operation first (its a0 and predicate seen by the
        // scalar one), then the scalar, then both results stored
        Id vector = vector_op(a);
        Id scalar = scalar_op(a);
        if (scalar) store(previous_scalar_, scalar);
        else if (a.scalar_result.used_write_mask()) scalar = load(float_, previous_scalar_);
        store_result(a.vector_and_constant_result, vector);
        store_result(a.scalar_result, scalar ? splat(scalar) : 0);
        return false;
    };
    if (a.is_predicated) when(predicate_is(a.predicate_condition), body);
    else body();
}

Id Translator::swap_endian(Id v, Id endian) {
    auto shl = [&](Id x, uint32_t n) { return m_.op(Op::OpShiftLeftLogical, uint_, {x, u(n)}); };
    auto shr = [&](Id x, uint32_t n) { return m_.op(Op::OpShiftRightLogical, uint_, {x, u(n)}); };
    auto band = [&](Id x, uint32_t mask) { return m_.op(Op::OpBitwiseAnd, uint_, {x, u(mask)}); };
    auto bor = [&](Id x, Id y) { return m_.op(Op::OpBitwiseOr, uint_, {x, y}); };
    Id in16 = bor(shl(band(v, 0x00FF00FF), 8), band(shr(v, 8), 0x00FF00FF)); // 8in16
    Id in32 = bor(shl(in16, 16), shr(in16, 16));                              // 8in32
    Id halves = bor(shl(v, 16), shr(v, 16));                                  // 16in32
    auto is = [&](uint32_t e) { return m_.op(Op::OpIEqual, bool_, {endian, u(e)}); };
    Id r = select(uint_, is(1), in16, v);
    r = select(uint_, is(2), in32, r);
    return select(uint_, is(3), halves, r);
}

// ---- memexport ----------------------------------------------------------
//
// As Xenia's SPIR-V translator does it (spirv_shader_translator_memexport.cc,
// and Shader::AnalyzeUcode for where exports end): eA and eM0-eM4 are
// variables; an alloc ends the export under way (any alloc, as Xenia's), as
// do the end of the shader and a kill; an export checks eA (its upper bits
// as the sequencer validates them), packs each eM# written since the last
// export into the stream's format (red and blue swapped, signed or not,
// normalized or not, NaN as 0, Direct3D's rounding, the extended-range
// float16), swaps its bytes by the stream's 128-bit endianness and stores
// the elements within the stream's count into guest memory (the storage
// buffer), bytes and halves by atomic AND and OR on their word.

// Which eM# each control flow instruction may follow without an export in
// between: Xenia's walk over the successors of each instruction that writes
// them (a call taken as writing all of them, an alloc ending the walk). Where
// a successor already has the bits the walk goes on with the others (Xenia's
// stops there): more bits only add code the eM# written bits guard anyway.
void Translator::analyze_memexport() {
    uint32_t n = uint32_t(s_.control_flow.size());
    std::vector<uint8_t> by(n, 0);
    for (uint32_t k = 0; k < n; k++) {
        const auto *e = std::get_if<Exec>(&s_.control_flow[k].instruction);
        if (!e) continue;
        for (const ExecSlot &slot : e->slots)
            if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                    if (r->target == StorageTarget::ExportData && r->index < 5 && r->used_write_mask())
                        by[k] |= uint8_t(1u << r->index);
        em_written_ |= by[k];
    }
    em_before_cf_.assign(n, 0);
    if (!em_written_) return;
    std::vector<uint32_t> stack;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t written = s_.control_flow[k].opcode == ControlFlowOpcode::CondCall ? em_written_ : by[k];
        if (!written) continue;
        bool head = true;
        stack.push_back(k);
        while (!stack.empty()) {
            uint32_t at = stack.back();
            stack.pop_back();
            if ((em_before_cf_[at] & written) == written) {
                head = false;
                continue;
            }
            if (!head) em_before_cf_[at] |= written;
            head = false;
            const ControlFlow &cf = s_.control_flow[at];
            bool next = true;
            switch (cf.opcode) {
            case ControlFlowOpcode::ExecEnd:
                em_before_end_ |= written;
                next = false;
                break;
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredEnd:
            case ControlFlowOpcode::CondExecPredCleanEnd: em_before_end_ |= written; break;
            case ControlFlowOpcode::LoopStart:
                stack.push_back(std::get<LoopStartInstruction>(cf.instruction).skip_address);
                break;
            case ControlFlowOpcode::LoopEnd:
                stack.push_back(std::get<LoopEndInstruction>(cf.instruction).body_address);
                break;
            case ControlFlowOpcode::CondCall: stack.push_back(std::get<CallInstruction>(cf.instruction).target); break;
            case ControlFlowOpcode::Return: next = false; break;
            case ControlFlowOpcode::CondJmp: {
                const auto &j = std::get<JumpInstruction>(cf.instruction);
                stack.push_back(j.target);
                next = j.type != JumpInstruction::Type::Unconditional;
                break;
            }
            case ControlFlowOpcode::Alloc: next = false; break;
            default: break;
            }
            if (next) {
                if (at + 1 < n) stack.push_back(at + 1);
                else em_before_end_ |= written;
            }
            // targets past the control flow end the shader
            while (!stack.empty() && stack.back() >= n) {
                stack.pop_back();
                em_before_end_ |= written;
            }
        }
    }
}

// An alloc ends the export under way; an alloc of exports starts with eA
// invalid (Xenia's ProcessAllocInstruction).
void Translator::alloc(const AllocInstruction &a) {
    if (!em_written_) return;
    uint8_t em = a.cf_index < em_before_cf_.size() ? em_before_cf_[a.cf_index] : 0;
    if (em) {
        export_to_memory(em);
        store(export_written_, u(0));
        for (uint32_t k = 0; k < 5; k++)
            if (em & (1u << k)) store(export_data_[k], zero4_);
    }
    if (a.type == AllocType::Memory) store(export_address_, zero4_);
}

// The extended-range float16 pair Xenia's PackFloat16x2ExtendedRange makes:
// NaN as 0, exponent 31 for magnitudes past 65504 (finite on the Xbox 360).
Id Translator::pack_half2(Id xy) {
    Id bvec2 = m_.type_vector(bool_, 2);
    Id zero2 = m_.const_null(vec2_);
    xy = select(vec2_, m_.op(Op::OpIsNan, bvec2, {xy}), zero2, xy);
    Id standard = glsl(GLSLstd450PackHalf2x16, uint_, {xy});
    auto band = [&](Id x, uint32_t mask) { return m_.op(Op::OpBitwiseAnd, uint_, {x, u(mask)}); };
    Id lower_overflow = m_.op(Op::OpIEqual, bool_, {band(standard, 0x7C00), u(0x7C00)});
    Id upper_overflow =
        m_.op(Op::OpIEqual, bool_, {band(m_.op(Op::OpShiftRightLogical, uint_, {standard, u(16)}), 0x7C00), u(0x7C00)});
    const Id limits[2][2] = {{f(-131008.0f), f(-131008.0f)}, {f(131008.0f), f(131008.0f)}};
    Id clamped = glsl(GLSLstd450FClamp, vec2_,
                      {xy, m_.const_composite(vec2_, limits[0]), m_.const_composite(vec2_, limits[1])});
    const Id halves[] = {f(0.5f), f(0.5f)};
    Id halved = m_.op(Op::OpFMul, vec2_, {clamped, m_.const_composite(vec2_, halves)});
    Id extended = m_.op(Op::OpIAdd, uint_, {glsl(GLSLstd450PackHalf2x16, uint_, {halved}), u(0x04000400)});
    Id lower = select(uint_, lower_overflow, band(extended, 0xFFFF), band(standard, 0xFFFF));
    Id upper = select(uint_, upper_overflow, band(extended, 0xFFFF0000u), band(standard, 0xFFFF0000u));
    return m_.op(Op::OpBitwiseOr, uint_, {lower, upper});
}

// Xenia's EndianSwap128Uint4: 8-in-64 and 8-in-128 as word moves, then 8-in-32
Id Translator::swap_endian_128(Id words, Id endian) {
    Id is_8in64 = m_.op(Op::OpIEqual, bool_, {endian, u(4)});
    Id is_8in128 = m_.op(Op::OpIEqual, bool_, {endian, u(5)});
    auto moved = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
        return m_.op(Op::OpVectorShuffle, uvec4_, {words, words, x, y, z, w});
    };
    words = select(uvec4_, splat_bool(is_8in64), moved(1, 0, 3, 2), words);
    words = select(uvec4_, splat_bool(is_8in128), moved(3, 2, 1, 0), words);
    endian = select(uint_, m_.op(Op::OpLogicalOr, bool_, {is_8in64, is_8in128}), u(2), endian);
    Id c[4];
    for (uint32_t k = 0; k < 4; k++) c[k] = swap_endian(m_.op(Op::OpCompositeExtract, uint_, {words, k}), endian);
    return m_.op(Op::OpCompositeConstruct, uvec4_, {c[0], c[1], c[2], c[3]});
}

// A word of guest memory written, or only the bits of replace_mask (by atomic
// AND and OR, as Xenia's StoreUint32ToSharedMemory); past the buffer, nothing
// (Xenia's robust buffer access drops it).
void Translator::store_memory_word(Id value, Id dword_index, Id replace_mask) {
    Id inside = m_.op(Op::OpULessThan, bool_, {dword_index, m_.op(Op::OpArrayLength, uint_, {memory_, 0})});
    when(inside, [&] {
        Id pointer = m_.op(Op::OpAccessChain, memory_ptr_uint_, {memory_, u(0), dword_index});
        if (!replace_mask) {
            store(pointer, value);
            return false;
        }
        Id device = u(uint32_t(spv::Scope::Device)), relaxed = u(0);
        m_.op(Op::OpAtomicAnd, uint_, {pointer, device, relaxed, m_.op(Op::OpNot, uint_, {replace_mask})});
        m_.op(Op::OpAtomicOr, uint_, {pointer, device, relaxed, m_.op(Op::OpBitwiseAnd, uint_, {value, replace_mask})});
        return false;
    });
}

// Xenia's ExportToMemory for the eM# in em.
void Translator::export_to_memory(uint8_t em) {
    em &= em_written_;
    if (!em) return;
    Id ea = m_.op(Op::OpBitcast, uvec4_, {load(vec4_, export_address_)});
    const Id shift_parts[] = {u(30), u(23), u(23), u(23)}, expected_parts[] = {u(1), u(0x96), u(0x96), u(0x96)};
    Id shifted = m_.op(Op::OpShiftRightLogical, uvec4_, {ea, m_.const_composite(uvec4_, shift_parts)});
    Id valid = m_.op(Op::OpAll, bool_,
                     {m_.op(Op::OpIEqual, bvec4_, {shifted, m_.const_composite(uvec4_, expected_parts)})});
    if (export_allowed_) valid = m_.op(Op::OpLogicalAnd, bool_, {valid, load(bool_, export_allowed_)});
    auto each = [&](auto fn) {
        for (uint32_t k = 0; k < 5; k++)
            if (em & (1u << k)) fn(k);
    };
    when(valid, [&] {
        Id info = m_.op(Op::OpCompositeExtract, uint_, {ea, 2});
        auto bit = [&](uint32_t b) {
            return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {info, u(1u << b)}), u(0)});
        };
        Id swap_red_blue = bit(19), is_signed = bit(16);
        Id is_norm = m_.op(Op::OpLogicalNot, bool_, {bit(17)});
        Id swapped[5] = {};
        each([&](uint32_t k) {
            Id v = load(vec4_, export_data_[k]);
            Id zyxw = m_.op(Op::OpVectorShuffle, vec4_, {v, v, 2, 1, 0, 3});
            swapped[k] = select(vec4_, splat_bool(swap_red_blue), zyxw, v);
        });

        // Fixed-point formats: the components converted as Direct3D does
        // (NaN as 0, normalized ones clamped and scaled, integer ones clamped
        // to the component's range, then rounded half away from zero), the
        // first `widths` packed from bit 0 up. The clamps of integers and the
        // scales (none for signed components of 1 or 2 bits) are Xenia's
        // D3D12 translator's (dxbc_shader_translator_memexport.cc), which its
        // Vulkan one lacks.
        Id ivec4 = m_.type_vector(int_, 4);
        Id half_up = vec4_const(0.5f, 0.5f, 0.5f, 0.5f), half_down = vec4_const(-0.5f, -0.5f, -0.5f, -0.5f);
        Id ones = vec4_const(1, 1, 1, 1), minus_ones = vec4_const(-1, -1, -1, -1);
        auto to_integers = [&](Id v, std::array<uint32_t, 4> widths) {
            float sscale[4], smin[4], smax[4], umax[4];
            for (uint32_t c = 0; c < 4; c++) {
                uint32_t w = widths[c] ? widths[c] : 16;
                smax[c] = float((1u << (w - 1)) - 1);
                smin[c] = -1.0f - smax[c];
                sscale[c] = w > 2 ? smax[c] : 1.0f;
                umax[c] = float((1u << w) - 1);
            }
            Id flushed = select(vec4_, m_.op(Op::OpIsNan, bvec4_, {v}), zero4_, v);
            Id snorm = exact(Op::OpFMul, vec4_, {glsl(GLSLstd450FClamp, vec4_, {flushed, minus_ones, ones}),
                                                  vec4_const(sscale[0], sscale[1], sscale[2], sscale[3])});
            Id sint = glsl(GLSLstd450FClamp, vec4_, {flushed, vec4_const(smin[0], smin[1], smin[2], smin[3]),
                                                     vec4_const(smax[0], smax[1], smax[2], smax[3])});
            Id sv = select(vec4_, splat_bool(is_norm), snorm, sint);
            Id rounding = select(vec4_, m_.op(Op::OpFOrdLessThan, bvec4_, {sv, zero4_}), half_down, half_up);
            Id si = m_.op(Op::OpConvertFToS, ivec4, {exact(Op::OpFAdd, vec4_, {sv, rounding})});
            si = m_.op(Op::OpBitcast, uvec4_, {si});
            Id umax_vector = vec4_const(umax[0], umax[1], umax[2], umax[3]);
            Id unorm = exact(Op::OpFMul, vec4_, {glsl(GLSLstd450FClamp, vec4_, {flushed, zero4_, ones}), umax_vector});
            Id uint = glsl(GLSLstd450FClamp, vec4_, {flushed, zero4_, umax_vector});
            Id uv = select(vec4_, splat_bool(is_norm), unorm, uint);
            Id ui = m_.op(Op::OpConvertFToU, uvec4_, {exact(Op::OpFAdd, vec4_, {uv, half_up})});
            return select(uvec4_, splat_bool(is_signed), si, ui);
        };
        auto packed_store = [&](uint32_t k, Id x, Id y = 0) {
            Id zero = u(0);
            store(export_packed_[k], m_.op(Op::OpCompositeConstruct, uvec4_, {x, y ? y : zero, zero, zero}));
        };
        auto shuffle2 = [&](Id a, Id b, uint32_t x, uint32_t y) {
            return m_.op(Op::OpVectorShuffle, vec2_, {a, b, x, y});
        };
        // float16: x alone, xy, or xy and zw; float32: the bits as they are
        auto halves = [&](uint32_t pairs) {
            each([&](uint32_t k) {
                if (pairs == 0) packed_store(k, pack_half2(shuffle2(swapped[k], zero4_, 0, 4)));
                else if (pairs == 1) packed_store(k, pack_half2(shuffle2(swapped[k], swapped[k], 0, 1)));
                else
                    packed_store(k, pack_half2(shuffle2(swapped[k], swapped[k], 0, 1)),
                                 pack_half2(shuffle2(swapped[k], swapped[k], 2, 3)));
            });
        };
        auto floats = [&] {
            each([&](uint32_t k) { store(export_packed_[k], m_.op(Op::OpBitcast, uvec4_, {swapped[k]})); });
        };
        auto pack = [&](std::array<uint32_t, 4> widths) {
            each([&](uint32_t k) {
                Id ints = to_integers(swapped[k], widths);
                Id word = m_.op(Op::OpCompositeExtract, uint_, {ints, 0});
                for (uint32_t c = 1, offset = widths[0]; c < 4 && widths[c]; offset += widths[c], c++)
                    word = m_.op(Op::OpBitFieldInsert, uint_,
                                 {word, m_.op(Op::OpCompositeExtract, uint_, {ints, c}), u(offset), u(widths[c])});
                packed_store(k, word);
            });
        };

        // the packed words and the element's size, by the stream's format
        struct Case {
            std::vector<uint32_t> formats;
            uint32_t size_log2;
            std::function<void()> body;
        };
        const Case cases[] = {
            {{2, 8, 9}, 0, [&] { pack({8, 0, 0, 0}); }},                // 8, 8_A, 8_B
            {{3}, 1, [&] { pack({5, 5, 5, 1}); }},                      // 1_5_5_5
            {{4}, 1, [&] { pack({5, 6, 5, 0}); }},                      // 5_6_5
            {{5}, 1, [&] { pack({5, 5, 6, 0}); }},                      // 6_5_5
            {{6, 14, 50}, 2, [&] { pack({8, 8, 8, 8}); }},              // 8_8_8_8, 8_8_8_8_A, 8_8_8_8_AS_16_16_16_16
            {{7, 54}, 2, [&] { pack({10, 10, 10, 2}); }},               // 2_10_10_10 (and AS_16_16_16_16)
            {{10}, 1, [&] { pack({8, 8, 0, 0}); }},                     // 8_8
            {{15}, 1, [&] { pack({4, 4, 4, 4}); }},                     // 4_4_4_4
            {{16, 55}, 2, [&] { pack({11, 11, 10, 0}); }},              // 10_11_11 (and AS_16_16_16_16)
            {{17, 56}, 2, [&] { pack({10, 11, 11, 0}); }},              // 11_11_10 (and AS_16_16_16_16)
            {{24}, 1, [&] { pack({16, 0, 0, 0}); }},                    // 16
            {{25}, 2, [&] { pack({16, 16, 0, 0}); }},                   // 16_16
            {{26}, 3,                                                   // 16_16_16_16
             [&] {
                 each([&](uint32_t k) {
                     Id ints = to_integers(swapped[k], {16, 16, 16, 16});
                     auto c = [&](uint32_t i) { return m_.op(Op::OpCompositeExtract, uint_, {ints, i}); };
                     packed_store(k, m_.op(Op::OpBitFieldInsert, uint_, {c(0), c(1), u(16), u(16)}),
                                  m_.op(Op::OpBitFieldInsert, uint_, {c(2), c(3), u(16), u(16)}));
                 });
             }},
            {{30}, 1, [&] { halves(0); }}, // 16_FLOAT
            {{31}, 2, [&] { halves(1); }}, // 16_16_FLOAT
            {{32}, 3, [&] { halves(2); }}, // 16_16_16_16_FLOAT
            {{36}, 2, floats},             // 32_FLOAT
            {{37}, 3, floats},             // 32_32_FLOAT
            {{38}, 4, floats},             // 32_32_32_32_FLOAT
        };
        Id format = m_.op(Op::OpBitFieldUExtract, uint_, {info, u(8), u(6)});
        Id merge = m_.id(), fallback = m_.id();
        std::vector<Id> labels;
        std::vector<uint32_t> operands{format, fallback};
        for (const Case &c : cases) {
            labels.push_back(m_.id());
            for (uint32_t literal : c.formats) operands.insert(operands.end(), {literal, labels.back()});
        }
        m_.op_void(Op::OpSelectionMerge, {merge, uint32_t(spv::SelectionControlMask::MaskNone)});
        m_.op_void(Op::OpSwitch, std::span<const uint32_t>(operands));
        for (size_t c = 0; c < std::size(cases); c++) {
            m_.label(labels[c]);
            cases[c].body();
            store(export_size_, u(cases[c].size_log2));
            m_.op_void(Op::OpBranch, {merge});
        }
        m_.label(fallback);
        store(export_size_, u(5)); // not a format exports take: nothing is stored
        m_.op_void(Op::OpBranch, {merge});
        m_.label(merge);

        Id size = load(uint_, export_size_);
        Id endian = m_.op(Op::OpBitFieldUExtract, uint_, {info, u(0), u(3)});
        Id packed[5] = {};
        each([&](uint32_t k) { packed[k] = swap_endian_128(load(uvec4_, export_packed_[k]), endian); });

        auto shl = [&](Id x, Id n) { return m_.op(Op::OpShiftLeftLogical, uint_, {x, n}); };
        auto shr = [&](Id x, uint32_t n) { return m_.op(Op::OpShiftRightLogical, uint_, {x, u(n)}); };
        auto band = [&](Id x, uint32_t mask) { return m_.op(Op::OpBitwiseAnd, uint_, {x, u(mask)}); };
        auto add = [&](Id x, uint32_t n) { return m_.op(Op::OpIAdd, uint_, {x, u(n)}); };
        auto low23 = [&](uint32_t c) {
            return m_.op(Op::OpBitFieldUExtract, uint_, {m_.op(Op::OpCompositeExtract, uint_, {ea, c}), u(0), u(23)});
        };
        // the eM# written since the last export, from the stream's index of
        // eM0 up to its count
        Id index = low23(1), count = low23(3);
        Id left = m_.op(Op::OpISub, int_, {m_.op(Op::OpBitcast, int_, {count}), m_.op(Op::OpBitcast, int_, {index})});
        left = m_.op(Op::OpBitcast, uint_, {glsl(GLSLstd450SClamp, int_, {left, i(0), i(5)})});
        Id to_store = m_.op(Op::OpBitFieldUExtract, uint_, {load(uint_, export_written_), u(0), left});
        // eM0's address in bytes: the stream's base (in words; shifted out
        // of its upper bits) plus the index's elements
        Id address = m_.op(Op::OpIAdd, uint_, {shl(m_.op(Op::OpCompositeExtract, uint_, {ea, 0}), u(2)), shl(index, size)});
        Id dwords = shr(address, 2);
        auto word = [&](uint32_t k, uint32_t c) { return m_.op(Op::OpCompositeExtract, uint_, {packed[k], c}); };
        for (uint32_t s = 0; s <= 4; s++) {
            when(m_.op(Op::OpIEqual, bool_, {size, u(s)}), [&] {
                each([&](uint32_t k) {
                    when(m_.op(Op::OpINotEqual, bool_, {band(to_store, 1u << k), u(0)}), [&] {
                        if (s == 0) {
                            // a byte: 8 * (address & 3) into its word
                            Id at = add(address, k);
                            Id shift = shl(band(at, 3), u(3));
                            store_memory_word(shl(word(k, 0), shift), shr(at, 2), shl(u(0xFF), shift));
                        } else if (s == 1) {
                            // a half: 16 * (half's index & 1) into its word
                            Id at = add(shr(address, 1), k);
                            Id shift = shl(band(at, 1), u(4));
                            store_memory_word(shl(word(k, 0), shift), shr(at, 1), shl(u(0xFFFF), shift));
                        } else {
                            uint32_t words = 1u << (s - 2);
                            Id at = add(dwords, words * k);
                            for (uint32_t c = 0; c < words; c++) store_memory_word(word(k, c), c ? add(at, c) : at);
                        }
                        return false;
                    });
                });
                return false;
            });
        }
        return false;
    });
}

void Translator::vertex_fetch(const VertexFetchInstruction &v) {
    // the 15 vertex formats: words, components, packed widths and offsets
    struct Format {
        uint32_t words, components;
        enum Kind { Packed, Half, Int32, Float32 } kind;
        uint8_t width[4], offset[4], word[4];
    };
    Format format;
    switch (v.data_format) {
    case 6: format = {1, 4, Format::Packed, {8, 8, 8, 8}, {0, 8, 16, 24}, {0, 0, 0, 0}}; break;     // 8_8_8_8
    case 7: format = {1, 4, Format::Packed, {10, 10, 10, 2}, {0, 10, 20, 30}, {0, 0, 0, 0}}; break; // 2_10_10_10
    case 16: format = {1, 3, Format::Packed, {11, 11, 10, 0}, {0, 11, 22, 0}, {0, 0, 0, 0}}; break; // 10_11_11
    case 17: format = {1, 3, Format::Packed, {10, 11, 11, 0}, {0, 10, 21, 0}, {0, 0, 0, 0}}; break; // 11_11_10
    case 25: format = {1, 2, Format::Packed, {16, 16, 0, 0}, {0, 16, 0, 0}, {0, 0, 0, 0}}; break;   // 16_16
    case 26: format = {2, 4, Format::Packed, {16, 16, 16, 16}, {0, 16, 0, 16}, {0, 0, 1, 1}}; break; // 16_16_16_16
    case 31: format = {1, 2, Format::Half, {}, {}, {}}; break;    // 16_16_FLOAT
    case 32: format = {2, 4, Format::Half, {}, {}, {}}; break;    // 16_16_16_16_FLOAT
    case 33: format = {1, 1, Format::Int32, {}, {}, {}}; break;   // 32
    case 34: format = {2, 2, Format::Int32, {}, {}, {}}; break;   // 32_32
    case 35: format = {4, 4, Format::Int32, {}, {}, {}}; break;   // 32_32_32_32
    case 36: format = {1, 1, Format::Float32, {}, {}, {}}; break; // 32_FLOAT
    case 37: format = {2, 2, Format::Float32, {}, {}, {}}; break; // 32_32_FLOAT
    case 57: format = {3, 3, Format::Float32, {}, {}, {}}; break; // 32_32_32_FLOAT
    case 38: format = {4, 4, Format::Float32, {}, {}, {}}; break; // 32_32_32_32_FLOAT
    default: unsupported("vertex format " + std::to_string(v.data_format)); return;
    }

    auto body = [&] {
        uint32_t slot = v.operands[1].index;
        Id word1 = constant_dword(3, slot * 2 + 1);
        Id address, bound;
        if (v.is_mini_fetch) {
            address = load(int_, vfetch_address_);
            bound = load(int_, vfetch_bound_);
        } else {
            Id word0 = constant_dword(3, slot * 2);
            address = m_.op(Op::OpBitcast, int_, {m_.op(Op::OpBitwiseAnd, uint_, {m_.op(Op::OpShiftRightLogical, uint_, {word0, u(2)}), u(0x07FFFFFF)})});
            Id size = m_.op(Op::OpBitcast, int_, {m_.op(Op::OpBitwiseAnd, uint_, {m_.op(Op::OpShiftRightLogical, uint_, {word1, u(2)}), u(0xFFFFFF)})});
            bound = m_.op(Op::OpIAdd, int_, {address, size});
            store(vfetch_bound_, bound);
            if (v.stride) {
                Id index = comp(operand(v.operands[0]), 0);
                if (v.is_index_rounded) index = exact(Op::OpFAdd, float_, {index, f(0.5f)});
                index = m_.op(Op::OpConvertFToS, int_, {glsl(GLSLstd450Floor, float_, {index})});
                if (v.stride > 1) index = m_.op(Op::OpIMul, int_, {index, i(int32_t(v.stride))});
                address = m_.op(Op::OpIAdd, int_, {address, index});
            }
            store(vfetch_address_, address);
        }
        Id endian = m_.op(Op::OpBitwiseAnd, uint_, {word1, u(3)});
        Id length = m_.op(Op::OpArrayLength, uint_, {memory_, 0});
        Id words[4] = {};
        for (uint32_t k = 0; k < format.words; k++) {
            Id at = m_.op(Op::OpIAdd, int_, {address, i(v.offset + int32_t(k))});
            Id at_u = m_.op(Op::OpBitcast, uint_, {at});
            // past the fetch constant's size, or the buffer, reads 0
            Id inside = m_.op(Op::OpLogicalAnd, bool_, {m_.op(Op::OpULessThan, bool_, {at_u, m_.op(Op::OpBitcast, uint_, {bound})}),
                                                        m_.op(Op::OpULessThan, bool_, {at_u, length})});
            Id safe = select(uint_, inside, at_u, u(0));
            Id word = load(uint_, m_.op(Op::OpAccessChain, memory_ptr_uint_, {memory_, u(0), safe}));
            words[k] = swap_endian(select(uint_, inside, word, u(0)), endian);
        }

        Id c[4] = {f(0), f(0), f(0), f(0)};
        switch (format.kind) {
        case Format::Float32:
            for (uint32_t k = 0; k < format.components; k++) c[k] = m_.op(Op::OpBitcast, float_, {words[k]});
            break;
        case Format::Half:
            for (uint32_t k = 0; k < format.words; k++) {
                Id pair = glsl(GLSLstd450UnpackHalf2x16, vec2_, {words[k]});
                c[k * 2] = m_.op(Op::OpCompositeExtract, float_, {pair, 0});
                c[k * 2 + 1] = m_.op(Op::OpCompositeExtract, float_, {pair, 1});
            }
            break;
        case Format::Int32:
            for (uint32_t k = 0; k < format.components; k++) {
                Id value = v.is_signed ? m_.op(Op::OpConvertSToF, float_, {m_.op(Op::OpBitcast, int_, {words[k]})})
                                       : m_.op(Op::OpConvertUToF, float_, {words[k]});
                if (!v.is_integer) {
                    if (!v.is_signed) {
                        value = exact(Op::OpFMul, float_, {value, f(1.0f / 4294967295.0f)});
                    } else if (v.signed_rf_mode == SignedRepeatingFractionMode::ZeroClampMinusOne) {
                        value = exact(Op::OpFMul, float_, {value, f(1.0f / 2147483647.0f)});
                    } else {
                        value = exact(Op::OpFMul, float_, {value, f(1.0f / 2147483647.5f)});
                        value = exact(Op::OpFAdd, float_, {value, f(0.5f / 2147483647.5f)});
                    }
                }
                c[k] = value;
            }
            break;
        case Format::Packed:
            for (uint32_t k = 0; k < format.components; k++) {
                uint32_t width = format.width[k];
                Id word = words[format.word[k]];
                Id value = v.is_signed
                               ? m_.op(Op::OpConvertSToF, float_, {m_.op(Op::OpBitFieldSExtract, int_, {m_.op(Op::OpBitcast, int_, {word}), i(format.offset[k]), i(int32_t(width))})})
                               : m_.op(Op::OpConvertUToF, float_, {m_.op(Op::OpBitFieldUExtract, uint_, {word, i(format.offset[k]), i(int32_t(width))})});
                if (!v.is_integer) {
                    float inverse = v.is_signed ? float((1u << (width - 1)) - 1) : float((1u << width) - 1);
                    if (v.is_signed && v.signed_rf_mode == SignedRepeatingFractionMode::NoZero) inverse += 0.5f;
                    float scale = 1.0f / inverse;
                    value = exact(Op::OpFMul, float_, {value, f(scale)});
                    if (v.is_signed) {
                        value = v.signed_rf_mode == SignedRepeatingFractionMode::ZeroClampMinusOne
                                    ? glsl(GLSLstd450FMax, float_, {value, f(-1)})
                                    : exact(Op::OpFAdd, float_, {value, f(0.5f * scale)});
                    }
                }
                c[k] = value;
            }
            break;
        }
        if (v.exp_adjust) {
            for (uint32_t k = 0; k < format.components; k++)
                c[k] = exact(Op::OpFMul, float_, {c[k], f(std::ldexp(1.0f, v.exp_adjust))});
        }
        store_result(v.result, vec4_of(c[0], c[1], c[2], c[3]));
        return false;
    };
    if (v.is_predicated) when(predicate_is(v.predicate_condition), body);
    else body();
}

void Translator::texture_fetch(const TextureFetchInstruction &t) {
    // the setters keep state for later fetches
    if (t.opcode == FetchOpcode::SetTextureLod || t.opcode == FetchOpcode::SetTextureGradientsHorz ||
        t.opcode == FetchOpcode::SetTextureGradientsVert) {
        auto body = [&] {
            Id value = operand(t.operands[0]);
            if (t.opcode == FetchOpcode::SetTextureLod) store(tfetch_lod_, comp(value, 0));
            else store(t.opcode == FetchOpcode::SetTextureGradientsHorz ? gradient_h_ : gradient_v_, value);
            return false;
        };
        if (t.is_predicated) when(predicate_is(t.predicate_condition), body);
        else body();
        return;
    }
    // getBCF, the fraction of the border color the fetch would blend in
    // (Microsoft's docs: ucode.h's link): Xenia's translators make it a
    // translation error that is not fatal and store 0 (its TODO: sample a
    // black texture with a white border), so 0 here, as Xenia draws it
    if (t.opcode == FetchOpcode::GetTextureBorderColorFrac) {
        if (!t.has_result()) return;
        approximate_.push_back("getBCF answered 0 (as Xenia)");
        auto zero = [&] {
            store_result(t.result, zero4_);
            return false;
        };
        if (t.is_predicated)
            when(predicate_is(t.predicate_condition), zero);
        else
            zero();
        return;
    }
    // getCompTexLOD and getWeights on the host's images only (the harness
    // has none)
    bool lod_query = t.opcode == FetchOpcode::GetTextureComputedLod && stage_ != Stage::Harness;
    bool weights = t.opcode == FetchOpcode::GetTextureWeights && stage_ != Stage::Harness;
    if (t.opcode != FetchOpcode::TextureFetch && t.opcode != FetchOpcode::GetTextureGradients && !lod_query &&
        !weights) {
        unsupported(std::string("texture fetch operation ") + t.opcode_name);
        return;
    }
    if (!t.has_result()) return;
    bool gradients = t.opcode == FetchOpcode::GetTextureGradients;
    // Unnormalized cube map coordinates are the face's x and y in texels
    // (divided by the face's size below, the offsets added first) and the
    // face index as it is, as Xenia's ProcessTextureFetchInstruction
    // normalizes a cube fetch's xy only; register gradients stay as given
    // (Xenia leaves a cube map's unnormalized too).
    auto body = [&] {
        Id coordinates = operand(t.operands[0]);
        uint32_t dims = dimension_component_count(t.dimension);
        uint32_t slot = t.operands[1].index;
        Id result;
        if (gradients) {
            // (d/dx c.x, d/dy c.x, d/dx c.y, d/dy c.y); the harness has no quads
            Id x = comp(coordinates, 0), y = comp(coordinates, 1);
            if (stage_ == Stage::Harness) {
                result = vec4_of(exact(Op::OpFMul, float_, {x, f(0.5f)}), exact(Op::OpFMul, float_, {x, f(-0.5f)}),
                                 exact(Op::OpFMul, float_, {y, f(0.5f)}), exact(Op::OpFMul, float_, {y, f(-0.5f)}));
            } else if (stage_ == Stage::Pixel) {
                m_.capability(spv::Capability::DerivativeControl);
                result = vec4_of(m_.op(Op::OpDPdxCoarse, float_, {x}), m_.op(Op::OpDPdyCoarse, float_, {x}),
                                 m_.op(Op::OpDPdxCoarse, float_, {y}), m_.op(Op::OpDPdyCoarse, float_, {y}));
            } else {
                result = zero4_; // no derivatives in a vertex shader
            }
        } else if (stage_ == Stage::Harness) {
            // shader_harness.h's stand-in, with the LOD and gradient registers
            Id s = f(float(slot));
            Id cx = comp(coordinates, 0), cy = dims >= 2 ? comp(coordinates, 1) : f(0);
            Id z = exact(Op::OpFMul, float_, {cx, cy}), w = s;
            if (t.use_register_gradients)
                z = exact(Op::OpFAdd, float_, {exact(Op::OpFAdd, float_, {z, comp(load(vec4_, gradient_h_), 0)}),
                                               comp(load(vec4_, gradient_v_), 1)});
            if (t.use_register_lod) w = exact(Op::OpFAdd, float_, {s, load(float_, tfetch_lod_)});
            result = vec4_of(exact(Op::OpFAdd, float_, {cx, s}), exact(Op::OpFSub, float_, {cy, s}), z, w);
        } else if (lod_query) {
            // the LOD the hardware would compute, as Xenia's (OpImageQueryLod's
            // unclamped level in x): only in pixel shaders with the computed
            // LOD, else zeros (with an explicit LOD or register gradients it
            // contradicts MSDN, as Xenia reports)
            bool computed = stage_ == Stage::Pixel && t.use_computed_lod && !t.use_register_gradients;
            result = computed ? sample_texture(t, coordinates, 2, Query::Lod) : zero4_;
        } else if (weights) {
            result = sample_texture(t, coordinates, 2, Query::Weights);
        } else {
            result = texture_signs(t, coordinates, sample_texture(t, coordinates));
            // the fetch constant's exponent adjust (word 3 bits 13-18,
            // signed): the result times its power of two, as Xenia's
            Id word3 = m_.op(Op::OpBitcast, int_, {constant_dword(3, slot * 6 + 3)});
            Id adjust = m_.op(Op::OpBitFieldSExtract, int_, {word3, u(13), u(6)});
            result = exact(Op::OpVectorTimesScalar, vec4_, {result, glsl(GLSLstd450Ldexp, float_, {f(1), adjust})});
        }
        store_result(t.result, result);
        return false;
    };
    if (t.is_predicated) when(predicate_is(t.predicate_condition), body);
    else body();
}

// A texture fetch on the host's images, as Xenia's
// ProcessTextureFetchInstruction (spirv_shader_translator_fetch.cc) makes
// it: the sizes from the fetch constant; the coordinates offset by the
// instruction's offsets plus Xenia's rounding epsilon (kTextureCoordEpsilon,
// 1.5/1024 of a texel), normalized when they are not; 1D and 2D textures
// sampled as 2D arrays at layer 0; a 3D-or-stacked fetch choosing at run
// time by the fetch constant's dimension between the 3D image and the
// layers of a stacked one (two of them blended when the volume filter is
// linear); cube maps from the face coordinates.
// A slot's word in one of the system constants' per-slot tables (member 2
// the images, 7 the signs, 8 the signed views).
Id Translator::slot_word(uint32_t member, uint32_t slot) {
    return m_.op(Op::OpCompositeExtract, uint_,
                 {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_),
                                     {system_, u(member), u(slot >> 2)})),
                  slot & 3});
}

// The fetch constant's signs on the sampled components, then its number
// format, as Xenia's ProcessTextureFetchInstruction applies them
// (spirv_shader_translator_fetch.cc), by two per-slot words of the
// renderer's: the signs (system member 7: two bits per result component,
// already through the constant's swizzle, Xenia's SwizzleSigns) and the
// integer scale word (member 12: texture::integer_scale_bits, Xenia's
// GetIntegerScaleBits: per component six bits, the width less one and the
// sign; bit 24 a fixed-point format read as fractions; bit 26 the constant
// filters by point).
// The signs: unsigned as sampled; signed from the texture's signed view
// (member 8, sampled only when a component asks for it and there is one;
// else the sample as it is, which float formats already give signed);
// biased as signed offset binary, (n - 2^(w-1)) / (2^(w-1) - 1) for a width
// known, else 2u - 1, unclamped; gamma back to linear through the piecewise
// linear curve.
// The number format, where the word has more than bit 26 (fixed-point
// formats; behind a uniform branch, as Xenia's): read as fractions, a
// point-sampled fetch's 4 to 6 bit unsigned components rebuilt as the
// console expands them (the texel n as n (2^w + 1) / 2^2w, its bits
// repeated), the unsigned ones rounded to 16 fractional bits (half to even:
// the console's filtered values are Q16, and games compare them with Q16
// constants), the biased ones no lower than -1; read as integers, each
// component times 2^w - 1 (unsigned) or 2^(w-1) - 1 (signed, biased; a
// 1-bit biased one times a half less a half), rounded when the fetch and the
// constant both sample by point.
//
// The signs and the number format are one function of the module's for each
// set of components kept and point sampling allowed, called by each fetch
// (fetch_post): the drivers compile them once, not once a fetch (inlined at
// every fetch, they were a third of the pipelines' compile time).
Id Translator::texture_signs(const TextureFetchInstruction &t, Id coordinates, Id sample) {
    const uint32_t slot = t.operands[1].index;
    Id info = slot_word(7, slot), signed_entry = slot_word(8, slot), scale_word = slot_word(12, slot);
    // only the components the instruction keeps (Xenia's
    // GetNonZeroResultComponents); the others go nowhere
    const uint32_t used = t.non_zero_result_components();
    if (!used) return sample;
    Id any_signed = m_.const_bool(false);
    for (uint32_t k = 0; k < 4; k++)
        if (used & (1u << k))
            any_signed = m_.op(Op::OpLogicalOr, bool_,
                               {any_signed, m_.op(Op::OpIEqual, bool_,
                                                  {m_.op(Op::OpBitFieldUExtract, uint_, {info, u(2 * k), u(2)}), u(1)})});
    store(signed_sample_, sample);
    when(m_.op(Op::OpLogicalAnd, bool_, {any_signed, m_.op(Op::OpINotEqual, bool_, {signed_entry, u(0)})}), [&] {
        store(signed_sample_, sample_texture(t, coordinates, 8));
        return false;
    });
    Id signed_sample = load(vec4_, signed_sample_);
    const uint32_t key = used | (allows_point_sampling(t) ? 16u : 0u);
    auto it = fetch_posts_.find(key);
    if (it == fetch_posts_.end()) it = fetch_posts_.emplace(key, m_.id()).first;
    return m_.op(Op::OpFunctionCall, vec4_, {it->second, sample, signed_sample, info, scale_word});
}

// fetch_posts_' functions, after the body that calls them
void Translator::emit_fetch_posts() {
    for (const auto &[key, function] : fetch_posts_) {
        const Id parameters[4] = {vec4_, vec4_, uint_, uint_};
        m_.begin_function(vec4_, m_.type_function(vec4_, parameters), spv::FunctionControlMask::MaskNone, function);
        Id sample = m_.function_parameter(vec4_), signed_sample = m_.function_parameter(vec4_);
        Id info = m_.function_parameter(uint_), scale_word = m_.function_parameter(uint_);
        m_.label();
        m_.op_void(Op::OpReturnValue, {fetch_post(key & 15, (key & 16) != 0, sample, signed_sample, info, scale_word)});
        m_.end_function();
    }
}

// The signs and the number format on a fetch's sample (texture_signs)
Id Translator::fetch_post(uint32_t used, bool point_allowed, Id sample, Id signed_sample, Id info, Id scale_word) {
    auto field = [&](Id word, uint32_t offset, uint32_t count) {
        return m_.op(Op::OpBitFieldUExtract, uint_, {word, u(offset), u(count)});
    };
    auto mul = [&](Id a, Id b) { return exact(Op::OpFMul, float_, {a, b}); };
    auto add = [&](Id a, Id b) { return exact(Op::OpFAdd, float_, {a, b}); };
    auto sub = [&](Id a, Id b) { return exact(Op::OpFSub, float_, {a, b}); };
    auto div = [&](Id a, Id b) { return exact(Op::OpFDiv, float_, {a, b}); };
    auto ieq = [&](Id a, uint32_t b) { return m_.op(Op::OpIEqual, bool_, {a, u(b)}); };
    Id signs[4] = {};
    for (uint32_t k = 0; k < 4; k++)
        if (used & (1u << k)) signs[k] = field(info, 2 * k, 2);
    Id out[4];
    for (uint32_t k = 0; k < 4; k++)
        out[k] = used & (1u << k) ? select(float_, ieq(signs[k], 1), comp(signed_sample, k), comp(sample, k))
                                  : comp(sample, k);
    store(fetch_scaled_, vec4_of(out[0], out[1], out[2], out[3]));
    // biased and gamma components, each behind a uniform branch of its own
    // (Xenia's switch on the sign), from the unsigned sample
    auto set_component = [&](uint32_t k, Id value) {
        store(fetch_scaled_, m_.op(Op::OpCompositeInsert, vec4_, {value, load(vec4_, fetch_scaled_), k}));
    };
    for (uint32_t k = 0; k < 4; k++) {
        if (!(used & (1u << k))) continue;
        Id value = comp(sample, k);
        when(ieq(signs[k], 2), [&] {
            Id width_less_one = field(scale_word, 6 * k, 4);
            Id half = m_.op(Op::OpConvertUToF, float_, {m_.op(Op::OpShiftLeftLogical, uint_, {u(1), width_less_one})});
            Id offset_binary = div(sub(mul(value, sub(mul(half, f(2)), f(1))), half), sub(half, f(1)));
            Id twice_less_one = add(mul(value, f(2)), f(-1));
            set_component(k, select(float_, m_.op(Op::OpINotEqual, bool_, {width_less_one, u(0)}), offset_binary,
                                    twice_less_one));
            return false;
        });
        when(ieq(signs[k], 3), [&] {
            set_component(k, pwl_gamma_to_linear(value));
            return false;
        });
    }

    // the number format (bit 26 alone: nothing to scale)
    Id active = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {scale_word, u((1u << 26) - 1)}), u(0)});
    // (each part behind a uniform branch of its own, as Xenia's: a fetch runs
    // only the part its constant asks for)
    auto lane_of = [&](uint32_t k) { return field(scale_word, 6 * k, 6); };
    auto word_has = [&](uint32_t mask) {
        return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {scale_word, u(mask)}), u(0)});
    };
    when(active, [&] {
        Id normalized = word_has(1u << 24);
        // as fractions
        when(normalized, [&] {
            if (point_allowed) {
                when(word_has((1u << 24) - 1), [&] {
                    Id v4 = load(vec4_, fetch_scaled_), rebuilt[4];
                    for (uint32_t k = 0; k < 4; k++) {
                        Id v = comp(v4, k), lane = lane_of(k);
                        if (!(used & (1u << k))) {
                            rebuilt[k] = v;
                            continue;
                        }
                        Id pow2 =m_.op(Op::OpConvertUToF, float_,
                                        {m_.op(Op::OpShiftLeftLogical, uint_, {u(2), m_.op(Op::OpBitwiseAnd, uint_, {lane, u(0xF)})})});
                        Id texel = glsl(GLSLstd450RoundEven, float_, {mul(v, sub(pow2, f(1)))});
                        // (where the lane is 1 to 15: unsigned with a width)
                        Id grid_lane = m_.op(Op::OpULessThan, bool_, {m_.op(Op::OpISub, uint_, {lane, u(1)}), u(15)});
                        rebuilt[k] = select(float_, grid_lane, div(mul(texel, add(pow2, f(1))), mul(pow2, pow2)), v);
                    }
                    store(fetch_scaled_, vec4_of(rebuilt[0], rebuilt[1], rebuilt[2], rebuilt[3]));
                    return false;
                });
            }
            Id v4 = load(vec4_, fetch_scaled_), n[4];
            for (uint32_t k = 0; k < 4; k++) {
                n[k] = comp(v4, k);
                if (!(used & (1u << k))) continue;
                Id v = n[k], sign = m_.op(Op::OpShiftRightLogical, uint_, {lane_of(k), u(4)});
                Id q16 = mul(glsl(GLSLstd450RoundEven, float_, {mul(v, f(65536.0f))}), f(1.0f / 65536.0f));
                n[k] = select(float_, ieq(sign, 0), q16, v);
                n[k] = select(float_, ieq(sign, 2), glsl(GLSLstd450FMax, float_, {n[k], f(-1)}), n[k]);
            }
            store(fetch_scaled_, vec4_of(n[0], n[1], n[2], n[3]));
            return false;
        });
        // as integers
        when(m_.op(Op::OpLogicalNot, bool_, {normalized}), [&] {
            Id point = word_has(1u << 26);
            Id v4 = load(vec4_, fetch_scaled_), r[4];
            for (uint32_t k = 0; k < 4; k++) {
                r[k] = comp(v4, k);
                if (!(used & (1u << k))) continue;
                Id v = r[k], lane = lane_of(k);
                Id width = m_.op(Op::OpBitwiseAnd, uint_, {lane, u(0xF)});
                Id sign = m_.op(Op::OpShiftRightLogical, uint_, {lane, u(4)});
                Id halved = m_.op(Op::OpULessThan, bool_, {m_.op(Op::OpISub, uint_, {sign, u(1)}), u(2)});
                Id shift =
                    m_.op(Op::OpISub, uint_, {m_.op(Op::OpIAdd, uint_, {width, u(1)}), select(uint_, halved, u(1), u(0))});
                Id scale = m_.op(Op::OpConvertUToF, float_,
                                 {m_.op(Op::OpISub, uint_, {m_.op(Op::OpShiftLeftLogical, uint_, {u(1), shift}), u(1)})});
                Id offset = sub(glsl(GLSLstd450FMin, float_, {scale, f(0.5f)}), f(0.5f));
                r[k] = add(mul(v, glsl(GLSLstd450FMax, float_, {scale, f(0.5f)})), offset);
                if (point_allowed) r[k] = select(float_, point, glsl(GLSLstd450RoundEven, float_, {r[k]}), r[k]);
            }
            store(fetch_scaled_, vec4_of(r[0], r[1], r[2], r[3]));
            return false;
        });
        return false;
    });
    return load(vec4_, fetch_scaled_);
}

// Xenia's ParsedTextureFetchInstruction::AllowsPointSampling: the fetch can
// return a single texel, no filter of the instruction's linear (nor
// anisotropic, with a computed LOD)
bool Translator::allows_point_sampling(const TextureFetchInstruction &t) {
    return t.mag_filter != TextureFilter::Linear && t.min_filter != TextureFilter::Linear &&
           t.mip_filter != TextureFilter::Linear &&
           (!t.use_computed_lod || t.aniso_filter == AnisoFilter::Disabled || t.aniso_filter == AnisoFilter::UseFetchConst);
}

// xenos.cc's PWLGammaToLinear as Xenia's translators write it
// (SpirvShaderTranslator::PWLGammaToLinear): saturated (NaN to 0), then the
// curve's four pieces inverted.
Id Translator::pwl_gamma_to_linear(Id gamma) {
    gamma = glsl(GLSLstd450NClamp, float_, {gamma, f(0), f(1)});
    auto at_least = [&](float edge) { return m_.op(Op::OpFOrdGreaterThanEqual, bool_, {gamma, f(edge)}); };
    Id piece_3 = at_least(192.0f / 255.0f), piece_2 = at_least(96.0f / 255.0f), piece_1 = at_least(64.0f / 255.0f);
    Id scale = select(float_, piece_2, select(float_, piece_3, f(8.0f / 1024.0f), f(4.0f / 1024.0f)),
                      select(float_, piece_1, f(2.0f / 1024.0f), f(1.0f / 1024.0f)));
    Id offset = select(float_, piece_2, select(float_, piece_3, f(-1024.0f), f(-256.0f)),
                       select(float_, piece_1, f(-64.0f), f(0.0f)));
    // linear = gamma * 255 * 1024 * scale + offset, plus trunc(linear * scale), over 1023
    Id linear = exact(Op::OpFAdd, float_,
                      {exact(Op::OpFMul, float_, {exact(Op::OpFMul, float_, {gamma, f(255.0f * 1024.0f)}), scale}), offset});
    linear = exact(Op::OpFAdd, float_,
                   {linear, glsl(GLSLstd450Trunc, float_, {exact(Op::OpFMul, float_, {linear, scale})})});
    return exact(Op::OpFMul, float_, {linear, f(1.0f / 1023.0f)});
}

Id Translator::sample_texture(const TextureFetchInstruction &t, Id coordinates, uint32_t member, Query query) {
    const bool lod_query = query == Query::Lod;
    const uint32_t slot = t.operands[1].index;
    const float epsilon = 1.5f / 1024.0f;
    const Id vec3 = m_.type_vector(float_, 3);
    auto add = [&](Id a, Id b) { return exact(Op::OpFAdd, float_, {a, b}); };
    auto mul = [&](Id a, Id b) { return exact(Op::OpFMul, float_, {a, b}); };
    auto div = [&](Id a, Id b) { return exact(Op::OpFDiv, float_, {a, b}); };

    // the slot's image (bits 0-13), the kind of image it is (bits 14-15: a
    // 2D array, 3D, cube) and its sampler (bits 16-31), from the system
    // constants
    Id entry = slot_word(member, slot);
    Id image_index = m_.op(Op::OpBitwiseAnd, uint_, {entry, u(0x3FFF)});
    Id kind = m_.op(Op::OpBitwiseAnd, uint_, {m_.op(Op::OpShiftRightLogical, uint_, {entry, u(14)}), u(3)});
    Id sampler_index = m_.op(Op::OpShiftRightLogical, uint_, {entry, u(16)});
    auto constant_pointer = [&](Id type) { return m_.type_pointer(spv::StorageClass::UniformConstant, type); };
    Id sampler =
        load(sampler_type_, m_.op(Op::OpAccessChain, constant_pointer(sampler_type_), {samplers_, sampler_index}));
    // the slot's image as kind k (0 2D array, 1 3D, 2 cube); another kind's
    // index would name the wrong image, so the stand-in (0) instead
    auto sampled_image = [&](uint32_t k) {
        Id type = k == 0 ? image2d_type_ : k == 1 ? image3d_type_ : imagecube_type_;
        Id images = k == 0 ? images2d_ : k == 1 ? images3d_ : imagescube_;
        Id index = select(uint_, m_.op(Op::OpIEqual, bool_, {kind, u(k)}), image_index, u(0));
        Id image = load(type, m_.op(Op::OpAccessChain, constant_pointer(type), {images, index}));
        return m_.op(Op::OpSampledImage, m_.type_sampled_image(type), {image, sampler});
    };
    auto bits = [&](Id word, uint32_t offset, uint32_t count) {
        return m_.op(Op::OpBitFieldUExtract, uint_, {word, u(offset), u(count)});
    };
    auto signed_bits = [&](Id word, uint32_t offset, uint32_t count) {
        return m_.op(Op::OpConvertSToF, float_,
                     {m_.op(Op::OpBitFieldSExtract, int_, {m_.op(Op::OpBitcast, int_, {word}), u(offset), u(count)})});
    };
    auto size_of = [&](Id minus_one) {
        return m_.op(Op::OpConvertUToF, float_, {m_.op(Op::OpIAdd, uint_, {minus_one, u(1)})});
    };

    const bool cube = t.dimension == FetchOpDimension::Cube, volume = t.dimension == FetchOpDimension::D3OrStacked;
    // a 1D fetch with more than one coordinate is a 2D one (Xenia's coordinate_dimension)
    const bool one = t.dimension == FetchOpDimension::D1 && t.operands[0].component_count <= 1;

    // the size, from fetch constant word 2 by the dimension in word 5
    Id word2 = constant_dword(3, slot * 6 + 2), word5 = constant_dword(3, slot * 6 + 5);
    Id data_is_3d = 0;
    Id size[3] = {};
    if (one) {
        size[0] = size_of(bits(word2, 0, 24));
    } else if (volume) {
        data_is_3d = m_.op(Op::OpIEqual, bool_, {bits(word5, 9, 2), u(2)});
        size[0] = size_of(select(uint_, data_is_3d, bits(word2, 0, 11), bits(word2, 0, 13)));
        size[1] = size_of(select(uint_, data_is_3d, bits(word2, 11, 11), bits(word2, 13, 13)));
        size[2] = size_of(select(uint_, data_is_3d, bits(word2, 22, 10), bits(word2, 26, 6)));
    } else {
        size[0] = size_of(bits(word2, 0, 13));
        size[1] = size_of(bits(word2, 13, 13));
        if (t.dimension == FetchOpDimension::D1) {
            // its texture may still be 1D: one row as wide as it is
            Id data_is_1d = m_.op(Op::OpIEqual, bool_, {bits(word5, 9, 2), u(0)});
            size[0] = select(float_, data_is_1d, size_of(bits(word2, 0, 24)), size[0]);
            size[1] = select(float_, data_is_1d, f(1), size[1]);
        }
    }

    // the coordinates, offset and normalized; with the point snap
    // (VertexOptions::texture_point_snap, Xenia's CanSnapToTexelCenter: a
    // point-sampled 2D fetch with normalized coordinates) the epsilon comes
    // at the sample, where the snap does not take its place
    const bool snap = (stage_ == Stage::Pixel ? pixel_.texture_point_snap : vertex_.texture_point_snap) &&
                      t.opcode == FetchOpcode::TextureFetch && t.dimension == FetchOpDimension::D2 &&
                      !t.unnormalized_coordinates && allows_point_sampling(t) && query == Query::Sample;
    const float rounding = snap ? 0.0f : epsilon;
    Id c[3] = {comp(coordinates, 0), one ? f(0) : comp(coordinates, 1), cube || volume ? comp(coordinates, 2) : f(0)};
    const float offsets[3] = {t.offset_x + rounding, one ? 0.0f : t.offset_y + rounding,
                              cube ? t.offset_z : volume ? t.offset_z + rounding : 0.0f};
    if (query == Query::Weights) {
        // getWeights as Xenia's (the LOD disregarded, as it does): per axis
        // the bilinear lerp factor, fract of the coordinate in texels plus
        // the offsets less half a texel; the LOD's factor in w, 0
        uint32_t axes = one ? 1u : volume ? 3u : 2u;
        Id w[4] = {f(0), f(0), f(0), f(0)};
        for (uint32_t k = 0; k < axes; k++) {
            Id v = t.unnormalized_coordinates ? c[k] : mul(c[k], size[k]);
            w[k] = glsl(GLSLstd450Fract, float_, {add(v, f(offsets[k] - 0.5f))});
        }
        return vec4_of(w[0], w[1], w[2], w[3]);
    }
    for (uint32_t k = 0; k < (one ? 1u : 2u); k++)
        c[k] = t.unnormalized_coordinates ? div(add(c[k], f(offsets[k])), size[k])
                                          : add(c[k], div(f(offsets[k]), size[k]));
    Id z_3d = 0, layer = 0; // volume: the 3D texture's z, the stacked texture's layer
    if (volume) {
        if (t.unnormalized_coordinates) {
            layer = add(c[2], f(offsets[2]));
            z_3d = div(layer, size[2]);
        } else {
            z_3d = add(c[2], div(f(offsets[2]), size[2]));
            // within the layers, so an infinity or NaN picks one of them
            layer = glsl(GLSLstd450NClamp, float_,
                         {add(mul(c[2], size[2]), f(offsets[2])), f(0), exact(Op::OpFSub, float_, {size[2], f(1)})});
        }
    }

    // The sample, as Xenia's ProcessTextureFetchInstruction makes it. The
    // LOD: the fetch constant's bias (word 4 bits 12-21, in 1/32), the
    // register's, the instruction's. With a computed LOD (pixel shaders, or
    // register gradients), explicit gradients: the registers' (normalized
    // if the coordinates are not) or the coordinates' coarse derivatives,
    // times 2 to the LOD plus the constant's per-axis exponents (word 4 bits
    // 22-26 across, 27-31 down); a cube map without register gradients
    // takes the implicit LOD with the LOD and the larger exponent as a bias.
    // Otherwise the LOD itself. n: the gradients' components; derivatives
    // of `differentiated` (normalized coordinates).
    bool computed = t.use_computed_lod && (stage_ == Stage::Pixel || t.use_register_gradients);
    Id word4 = constant_dword(3, slot * 6 + 4);
    Id lod = mul(signed_bits(word4, 12, 10), f(1.0f / 32.0f));
    if (t.use_register_lod) lod = add(load(float_, tfetch_lod_), lod);
    if (t.lod_bias != 0) lod = add(lod, f(t.lod_bias));
    Id exponent_h = signed_bits(word4, 22, 5), exponent_v = signed_bits(word4, 27, 5);
    auto sample = [&](Id sampled, Id coords, uint32_t n, Id differentiated) {
        if (lod_query) {
            // the coordinates without an array layer
            m_.capability(spv::Capability::ImageQuery);
            Id lod = m_.op(Op::OpImageQueryLod, vec2_, {sampled, differentiated});
            return vec4_of(comp(lod, 1), f(0), f(0), f(0));
        }
        if (computed && cube && !t.use_register_gradients) {
            Id bias = add(lod, glsl(GLSLstd450FMax, float_, {exponent_h, exponent_v}));
            return m_.op(Op::OpImageSampleImplicitLod, vec4_,
                         {sampled, coords, uint32_t(spv::ImageOperandsMask::Bias), bias});
        }
        if (!computed)
            return m_.op(Op::OpImageSampleExplicitLod, vec4_,
                         {sampled, coords, uint32_t(spv::ImageOperandsMask::Lod), lod});
        Id type = n == 3 ? vec3 : vec2_;
        Id gradients[2];
        if (t.use_register_gradients) {
            for (uint32_t g = 0; g < 2; g++) {
                Id v = load(vec4_, g ? gradient_v_ : gradient_h_);
                Id parts[3];
                for (uint32_t k = 0; k < n; k++)
                    parts[k] = t.unnormalized_coordinates && !cube && size[k] ? div(comp(v, k), size[k]) : comp(v, k);
                gradients[g] = n == 3 ? m_.op(Op::OpCompositeConstruct, vec3, {parts[0], parts[1], parts[2]})
                                      : m_.op(Op::OpCompositeConstruct, vec2_, {parts[0], parts[1]});
            }
        } else {
            m_.capability(spv::Capability::DerivativeControl);
            gradients[0] = m_.op(Op::OpDPdxCoarse, type, {differentiated});
            gradients[1] = m_.op(Op::OpDPdyCoarse, type, {differentiated});
        }
        gradients[0] = exact(Op::OpVectorTimesScalar, type,
                             {gradients[0], glsl(GLSLstd450Exp2, float_, {add(lod, exponent_h)})});
        gradients[1] = exact(Op::OpVectorTimesScalar, type,
                             {gradients[1], glsl(GLSLstd450Exp2, float_, {add(lod, exponent_v)})});
        return m_.op(Op::OpImageSampleExplicitLod, vec4_,
                     {sampled, coords, uint32_t(spv::ImageOperandsMask::Grad), gradients[0], gradients[1]});
    };
    auto vec3_of = [&](Id x, Id y, Id z) { return m_.op(Op::OpCompositeConstruct, vec3, {x, y, z}); };
    auto vec2_of = [&](Id x, Id y) { return m_.op(Op::OpCompositeConstruct, vec2_, {x, y}); };

    if (cube) {
        // the cube instruction's face coordinates (sc and tc in 1-2, the
        // face 0-5) back to a direction, as Xenia does
        Id sc = add(mul(c[0], f(2)), f(-3)), tc = add(mul(c[1], f(2)), f(-3));
        Id face_value = offsets[2] != 0 ? add(c[2], f(offsets[2])) : c[2];
        Id face = m_.op(Op::OpConvertFToU, uint_, {glsl(GLSLstd450NClamp, float_, {face_value, f(0), f(5)})});
        Id axis = m_.op(Op::OpShiftRightLogical, uint_, {face, u(1)});
        Id negative = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {face, u(1)}), u(0)});
        Id sign = select(float_, negative, f(-1), f(1));
        Id nsc = m_.op(Op::OpFNegate, float_, {sc}), ntc = m_.op(Op::OpFNegate, float_, {tc});
        Id on_x = vec3_of(sign, ntc, select(float_, negative, sc, nsc));
        Id on_y = vec3_of(sc, sign, select(float_, negative, ntc, tc));
        Id on_z = vec3_of(select(float_, negative, nsc, sc), ntc, sign);
        Id coords = select(vec3, m_.op(Op::OpIEqual, bool_, {axis, u(0)}), on_x,
                           select(vec3, m_.op(Op::OpIEqual, bool_, {axis, u(1)}), on_y, on_z));
        return sample(sampled_image(2), coords, 3, coords);
    }
    if (!volume) {
        Id x = c[0], y = c[1];
        if (snap) {
            // the texel's center where the constant filters by point too
            // (the integer scale word's bit 26), else the epsilon after all
            Id point = m_.op(Op::OpINotEqual, bool_,
                             {m_.op(Op::OpBitwiseAnd, uint_, {slot_word(12, slot), u(1u << 26)}), u(0)});
            Id *axes[2] = {&x, &y};
            for (uint32_t k = 0; k < 2; k++) {
                Id snapped = div(add(glsl(GLSLstd450Floor, float_, {mul(c[k], size[k])}), f(0.5f)), size[k]);
                *axes[k] = select(float_, point, snapped, add(c[k], div(f(epsilon), size[k])));
            }
        }
        return sample(sampled_image(0), vec3_of(x, y, f(0)), 2, vec2_of(c[0], c[1]));
    }

    // 3D, or stacked; both sampled (the right one chosen after, so the
    // implicit LOD's derivatives stay in uniform control flow)
    Id coords_3d = vec3_of(c[0], c[1], z_3d);
    Id sampled_3d = sample(sampled_image(1), coords_3d, 3, coords_3d);
    // the filter between layers: the instruction's, or the fetch
    // constant's (word 4 bits 0 and 1, magnifying and minifying); with a
    // computed LOD and the two differing, chosen by the layer coordinate's
    // gradient as Xenia does
    bool mag_constant = t.vol_mag_filter == TextureFilter::UseFetchConst;
    bool min_constant = t.vol_min_filter == TextureFilter::UseFetchConst;
    bool mag_linear = t.vol_mag_filter == TextureFilter::Linear, min_linear = t.vol_min_filter == TextureFilter::Linear;
    auto word4_bit = [&](uint32_t bit) {
        return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {word4, u(1u << bit)}), u(0)});
    };
    Id linear = 0; // chosen at run time
    if (computed && (mag_constant || min_constant || mag_linear != min_linear)) {
        Id scale_h = glsl(GLSLstd450Exp2, float_, {add(lod, exponent_h)});
        Id scale_v = glsl(GLSLstd450Exp2, float_, {add(lod, exponent_v)});
        Id gradient_h, gradient_v;
        if (t.use_register_gradients) {
            gradient_h = comp(load(vec4_, gradient_h_), 2), gradient_v = comp(load(vec4_, gradient_v_), 2);
            if (t.unnormalized_coordinates) {
                gradient_h = div(gradient_h, size[2]);
                gradient_v = div(gradient_v, size[2]);
            }
        } else {
            m_.capability(spv::Capability::DerivativeControl);
            gradient_h = m_.op(Op::OpDPdxCoarse, float_, {layer});
            gradient_v = m_.op(Op::OpDPdyCoarse, float_, {layer});
        }
        Id layer_gradient = glsl(GLSLstd450NMax, float_, {mul(gradient_h, scale_h), mul(gradient_v, scale_v)});
        if (!t.unnormalized_coordinates) layer_gradient = mul(layer_gradient, size[2]);
        // NaN: magnifying
        Id minifying = m_.op(Op::OpFOrdLessThan, bool_, {layer_gradient, f(1)});
        Id mag = mag_constant ? word4_bit(0) : m_.const_bool(mag_linear);
        Id min = min_constant ? word4_bit(1) : m_.const_bool(min_linear);
        linear = select(bool_, minifying, min, mag);
    } else if (mag_constant) {
        linear = word4_bit(0);
    }
    bool always_linear = !linear && mag_linear;
    // linear: the first layer's middle at 0.5; the layer floored, as the
    // hardware addresses layers like a 3D texture's slices
    Id half_back = exact(Op::OpFSub, float_, {layer, f(0.5f)});
    Id layer_coordinate = linear ? select(float_, linear, half_back, layer) : always_linear ? half_back : layer;
    Id layer_0 = glsl(GLSLstd450Floor, float_, {layer_coordinate});
    Id stacked = sample(sampled_image(0), vec3_of(c[0], c[1], layer_0), 2, vec2_of(c[0], c[1]));
    if (linear || always_linear) {
        Id next = sample(sampled_image(0), vec3_of(c[0], c[1], m_.op(Op::OpFAdd, float_, {layer_0, f(1)})), 2,
                         vec2_of(c[0], c[1]));
        Id factor = glsl(GLSLstd450Fract, float_, {layer_coordinate});
        Id step = exact(Op::OpVectorTimesScalar, vec4_, {exact(Op::OpFSub, vec4_, {next, stacked}), factor});
        Id filtered = exact(Op::OpFAdd, vec4_, {stacked, step});
        stacked = linear ? select(vec4_, splat_bool(linear), filtered, stacked) : filtered;
    }
    return select(vec4_, splat_bool(data_is_3d), sampled_3d, stacked);
}

bool Translator::exec(const Exec &e) {
    auto instructions = [&] {
        // the eM# a kill here exports: those before the exec, and its own so far
        em_before_ = e.cf.cf_index < em_before_cf_.size() ? em_before_cf_[e.cf.cf_index] : 0;
        for (const ExecSlot &slot : e.slots) {
            if (const auto *a = std::get_if<AluInstruction>(&slot.instruction)) {
                alu(*a);
                for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                    if (r->target == StorageTarget::ExportData && r->index < 5 && r->used_write_mask())
                        em_before_ |= uint8_t(1u << r->index);
            } else if (const auto *v = std::get_if<VertexFetchInstruction>(&slot.instruction)) vertex_fetch(*v);
            else texture_fetch(std::get<TextureFetchInstruction>(slot.instruction));
        }
        if (e.cf.is_end) {
            m_.op_void(Op::OpReturn);
            return true;
        }
        return false;
    };
    switch (e.cf.type) {
    case ExecInstruction::Type::Unconditional: return instructions();
    case ExecInstruction::Type::Conditional: {
        Id condition = bool_constant(e.cf.bool_constant);
        if (!e.cf.condition) condition = m_.op(Op::OpLogicalNot, bool_, {condition});
        when(condition, instructions);
        return false;
    }
    case ExecInstruction::Type::Predicated: when(predicate_is(e.cf.condition), instructions); return false;
    }
    return false;
}

// One control flow instruction inside the dispatcher; pc_ already holds the
// next index, which a loop, call, return or jump may change.
bool Translator::control_flow(const ControlFlow &cf, uint32_t index) {
    auto set_pc = [&](Id value) { store(pc_, value); };
    auto count_at = [&](uint32_t k) { return element(loop_count_, ptr_private_uint_, i(int32_t(k))); };
    auto address_at = [&](uint32_t k) { return element(loop_address_, ptr_private_int_, i(int32_t(k))); };
    return std::visit(
        [&](const auto &instruction) -> bool {
            using T = std::decay_t<decltype(instruction)>;
            if constexpr (std::is_same_v<T, Exec>) {
                return exec(instruction);
            } else if constexpr (std::is_same_v<T, LoopStartInstruction>) {
                // push the loop: its count, and aL (the start, or aL kept)
                Id constant = constant_dword(2, instruction.loop_constant & 31);
                Id count = m_.op(Op::OpBitwiseAnd, uint_, {constant, u(0xFF)});
                Id start = m_.op(Op::OpBitcast, int_, {m_.op(Op::OpBitFieldUExtract, uint_, {constant, i(8), i(8)})});
                Id old_al = load(int_, address_at(0));
                for (uint32_t k = 3; k > 0; k--) {
                    store(count_at(k), load(uint_, count_at(k - 1)));
                    store(address_at(k), load(int_, address_at(k - 1)));
                }
                store(count_at(0), count);
                Id al = instruction.is_repeat ? old_al : start;
                store(address_at(0), al);
                store(aL_, al);
                set_pc(select(uint_, m_.op(Op::OpIEqual, bool_, {count, u(0)}), u(instruction.skip_address), u(index + 1)));
                return false;
            } else if constexpr (std::is_same_v<T, LoopEndInstruction>) {
                Id count = m_.op(Op::OpISub, uint_, {load(uint_, count_at(0)), u(1)});
                Id done = m_.op(Op::OpIEqual, bool_, {count, u(0)});
                if (instruction.is_predicated_break)
                    done = m_.op(Op::OpLogicalOr, bool_, {done, predicate_is(instruction.predicate_condition)});
                Id constant = constant_dword(2, instruction.loop_constant & 31);
                Id step = m_.op(Op::OpBitFieldSExtract, int_, {m_.op(Op::OpBitcast, int_, {constant}), i(16), i(8)});
                // again: the count down, aL a step on; done: the stack pops
                Id counts[4], addresses[4];
                for (uint32_t k = 0; k < 4; k++) {
                    counts[k] = load(uint_, count_at(k));
                    addresses[k] = load(int_, address_at(k));
                }
                Id again_al = m_.op(Op::OpIAdd, int_, {addresses[0], step});
                for (uint32_t k = 0; k < 4; k++) {
                    Id popped_count = k < 3 ? counts[k + 1] : u(0), popped_address = k < 3 ? addresses[k + 1] : i(0);
                    Id again_count = k == 0 ? count : counts[k], again_address = k == 0 ? again_al : addresses[k];
                    store(count_at(k), select(uint_, done, popped_count, again_count));
                    store(address_at(k), select(int_, done, popped_address, again_address));
                }
                store(aL_, load(int_, address_at(0)));
                set_pc(select(uint_, done, u(index + 1), u(instruction.body_address)));
                return false;
            } else if constexpr (std::is_same_v<T, CallInstruction>) {
                // a fifth call deep is skipped, as Xenia's interpreter does
                Id depth = load(uint_, call_depth_);
                Id go = m_.op(Op::OpLogicalAnd, bool_,
                              {m_.op(Op::OpULessThan, bool_, {depth, u(4)}),
                               taken(instruction.type == CallInstruction::Type::Unconditional,
                                     instruction.type == CallInstruction::Type::Predicated, instruction.bool_constant,
                                     instruction.condition)});
                when(go, [&] {
                    Id d = load(uint_, call_depth_);
                    store(element(call_stack_, ptr_private_uint_, d), u(index + 1));
                    store(call_depth_, m_.op(Op::OpIAdd, uint_, {d, u(1)}));
                    set_pc(u(instruction.target));
                    return false;
                });
                return false;
            } else if constexpr (std::is_same_v<T, ReturnInstruction>) {
                when(m_.op(Op::OpUGreaterThan, bool_, {load(uint_, call_depth_), u(0)}), [&] {
                    Id d = m_.op(Op::OpISub, uint_, {load(uint_, call_depth_), u(1)});
                    store(call_depth_, d);
                    set_pc(load(uint_, element(call_stack_, ptr_private_uint_, d)));
                    return false;
                });
                return false;
            } else if constexpr (std::is_same_v<T, JumpInstruction>) {
                Id go = taken(instruction.type == JumpInstruction::Type::Unconditional,
                              instruction.type == JumpInstruction::Type::Predicated, instruction.bool_constant,
                              instruction.condition);
                set_pc(select(uint_, go, u(instruction.target), u(index + 1)));
                return false;
            } else if constexpr (std::is_same_v<T, AllocInstruction>) {
                alloc(instruction);
                return false;
            } else {
                return false; // nop, mark_vs_fetch_done
            }
        },
        cf.instruction);
}

// The control flow as a loop over a switch on the next instruction, for
// shaders with loops, calls or jumps. The interpreter's step limit keeps a
// shader that never ends from hanging the GPU.
void Translator::dispatch() {
    uint32_t n = uint32_t(s_.control_flow.size());
    Id header = m_.id(), body = m_.id(), continue_block = m_.id(), merge = m_.id(), switch_merge = m_.id(),
       fallback = m_.id();
    store(pc_, u(0));
    store(steps_, u(0));
    m_.op_void(Op::OpBranch, {header});
    m_.label(header);
    m_.op_void(Op::OpLoopMerge, {merge, continue_block, uint32_t(spv::LoopControlMask::MaskNone)});
    m_.op_void(Op::OpBranch, {body});
    m_.label(body);
    std::vector<Id> cases(n);
    std::vector<uint32_t> operands{load(uint_, pc_), fallback};
    for (uint32_t k = 0; k < n; k++) {
        cases[k] = m_.id();
        operands.push_back(k);
        operands.push_back(cases[k]);
    }
    m_.op_void(Op::OpSelectionMerge, {switch_merge, uint32_t(spv::SelectionControlMask::MaskNone)});
    m_.op_void(Op::OpSwitch, operands);
    for (uint32_t k = 0; k < n; k++) {
        m_.label(cases[k]);
        store(pc_, u(k + 1));
        if (!control_flow(s_.control_flow[k], k)) m_.op_void(Op::OpBranch, {switch_merge});
    }
    m_.label(fallback);
    store(pc_, u(n));
    m_.op_void(Op::OpBranch, {switch_merge});
    m_.label(switch_merge);
    m_.op_void(Op::OpBranch, {continue_block});
    m_.label(continue_block);
    Id steps = m_.op(Op::OpIAdd, uint_, {load(uint_, steps_), u(1)});
    store(steps_, steps);
    Id again = m_.op(Op::OpLogicalAnd, bool_, {m_.op(Op::OpULessThan, bool_, {load(uint_, pc_), u(n)}),
                                               m_.op(Op::OpULessThanEqual, bool_, {steps, u(1u << 20)})});
    m_.op_void(Op::OpBranchConditional, {again, header, merge});
    m_.label(merge);
}

void Translator::main_harness() {
    Id u3 = m_.type_vector(uint_, 3);
    Id gid = global(u3, spv::StorageClass::Input, "gid");
    m_.decorate(gid, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::GlobalInvocationId)});
    auto buffer = [&](uint32_t binding, const char *name, bool writable) {
        Id array = m_.type_runtime_array(vec4_, 16);
        const Id members[] = {array};
        Id block = m_.type_struct(members);
        m_.decorate(block, spv::Decoration::Block);
        m_.member_decorate(block, 0, spv::Decoration::Offset, {0});
        Id v = global(block, spv::StorageClass::StorageBuffer, name);
        m_.decorate(v, spv::Decoration::DescriptorSet, {0});
        m_.decorate(v, spv::Decoration::Binding, {binding});
        if (!writable) m_.decorate(v, spv::Decoration::NonWritable);
        return v;
    };
    Id in = buffer(0, "cases_in", false), out = buffer(3, "cases_out", true);
    Id ptr_sb_vec4 = m_.type_pointer(spv::StorageClass::StorageBuffer, vec4_);

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    Id the_case = m_.op(Op::OpCompositeExtract, uint_, {load(u3, gid), 0});
    Id in_base = m_.op(Op::OpIMul, uint_, {the_case, u(harness::REGISTERS)});
    for (uint32_t k = 0; k < harness::REGISTERS; k++) {
        Id value = load(vec4_, m_.op(Op::OpAccessChain, ptr_sb_vec4, {in, u(0), m_.op(Op::OpIAdd, uint_, {in_base, u(k)})}));
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}), value);
    }
    reset_state();
    m_.op(Op::OpFunctionCall, void_, {body_});
    export_to_memory(em_before_end_); // the export under way at the end
    Id out_base = m_.op(Op::OpIMul, uint_, {the_case, u(OUTPUT_VEC4S)});
    auto put = [&](uint32_t at, Id value) {
        store(m_.op(Op::OpAccessChain, ptr_sb_vec4, {out, u(0), m_.op(Op::OpIAdd, uint_, {out_base, u(at)})}), value);
    };
    auto element = [&](Id array, uint32_t k) { return load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {array, i(int32_t(k))})); };
    for (uint32_t k = 0; k < harness::REGISTERS; k++) put(k, element(registers_, k));
    for (uint32_t k = 0; k < 16; k++) put(OUT_INTERPOLATORS + k, element(interpolators_, k));
    put(OUT_POSITION, load(vec4_, position_));
    put(OUT_POINT_SIZE, load(vec4_, point_size_));
    for (uint32_t k = 0; k < 4; k++) put(OUT_COLORS + k, element(colors_, k));
    put(OUT_DEPTH, load(vec4_, depth_));
    auto as_float = [&](Id x) { return m_.op(Op::OpBitcast, float_, {x}); };
    Id killed = select(uint_, load(bool_, killed_), u(1), u(0));
    Id predicate = select(uint_, load(bool_, predicate_), u(1), u(0));
    put(OUT_MISC, vec4_of(as_float(killed), as_float(predicate), as_float(load(int_, a0_)), f(0)));
    m_.op_void(Op::OpReturn);
    m_.end_function();
    m_.entry_point(spv::ExecutionModel::GLCompute, main, "main", interface_);
    m_.execution_mode(main, spv::ExecutionMode::LocalSize, {harness::LOCAL_SIZE, 1, 1});
}

void Translator::main_vertex() {
    const HostVertex host = vertex_.host;
    const bool domain = host >= HostVertex::TriangleDomainCPIndexed;
    const bool points = host == HostVertex::PointListAsTriangleStrip;
    const bool rectangles = host == HostVertex::RectangleListAsTriangleStrip;
    const Id vec3 = m_.type_vector(float_, 3), uvec3 = m_.type_vector(uint_, 3);
    Id vertex_index = 0, guest_indices = 0, control_points = 0, tess_coord = 0;
    const uint32_t control_point_count = host == HostVertex::TriangleDomainCPIndexed ? 3
                                         : host == HostVertex::QuadDomainCPIndexed   ? 4
                                                                                     : 1;
    if (domain) {
        // the tessellator's coordinates, and per control point of the patch
        // its index (the patch's for the patch-indexed types), as the hull
        // shader (tessellation.tesc) passes it on
        m_.capability(spv::Capability::Tessellation);
        control_points =
            global(m_.type_array(float_, control_point_count), spv::StorageClass::Input, "control_point_indices");
        m_.decorate(control_points, spv::Decoration::Location, {0});
        tess_coord = global(vec3, spv::StorageClass::Input, "tess_coord");
        m_.decorate(tess_coord, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::TessCoord)});
    } else {
        vertex_index = global(int_, spv::StorageClass::Input, "vertex_index");
        m_.decorate(vertex_index, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::VertexIndex)});
        if (rectangles) {
            guest_indices = global(uvec3, spv::StorageClass::Input, "guest_indices");
            m_.decorate(guest_indices, spv::Decoration::Location, {0});
        }
    }
    Id position = global(vec4_, spv::StorageClass::Output, "position");
    m_.decorate(position, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::Position)});
    // the same position from the same computation in any shader (a depth
    // pre-pass and a pass testing EQUAL against it), as Xenia's translators
    // declare their outputs invariant
    m_.decorate(position, spv::Decoration::Invariant);
    Id outputs = global(m_.type_array(vec4_, 16), spv::StorageClass::Output, "interpolators");
    m_.decorate(outputs, spv::Decoration::Location, {0});
    // the screen extents' reports (VertexOptions::extents), as words
    Id extents = 0;
    if (vertex_.extents) {
        const Id extent_members[] = {m_.type_runtime_array(uint_, 4)};
        Id extent_block = m_.type_struct(extent_members);
        m_.decorate(extent_block, spv::Decoration::Block);
        m_.member_decorate(extent_block, 0, spv::Decoration::Offset, {0});
        extents = global(extent_block, spv::StorageClass::StorageBuffer, "extents");
        m_.decorate(extents, spv::Decoration::DescriptorSet, {0});
        m_.decorate(extents, spv::Decoration::Binding, {7});
    }
    // the point diameter for point lists' geometry shader (point_list.geom),
    // after the interpolators
    Id point_size = global(float_, spv::StorageClass::Output, "point_size");
    m_.decorate(point_size, spv::Decoration::Location, {16});
    // an expanded point's sprite coordinates (0 to 1 from the left and top
    // corners), for the pixel parameters (PixelOptions::point_coordinates),
    // as Xenia's xe_out_point_coordinates; point_list.geom writes the same
    Id point_coordinates = 0;
    if (points) {
        point_coordinates = global(m_.type_vector(float_, 2), spv::StorageClass::Output, "point_coordinates");
        m_.decorate(point_coordinates, spv::Decoration::Location, {17});
        m_.decorate(point_coordinates, spv::Decoration::Invariant);
    }
    // A shader that exports memory is drawn alone as a point list when the
    // draw rasterizes nothing (renderer.cpp), and Vulkan without
    // maintenance5 wants a point size from it then: 1.
    Id host_point_size = 0;
    if (em_written_ && host == HostVertex::Vertex) {
        host_point_size = global(float_, spv::StorageClass::Output, "host_point_size");
        m_.decorate(host_point_size, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::PointSize)});
    }
    bool writes_point_size = false;
    for (const ControlFlow &cf : s_.control_flow)
        if (const auto *e = std::get_if<Exec>(&cf.instruction))
            for (const ExecSlot &slot : e->slots)
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                    for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                        writes_point_size |=
                            r->target == StorageTarget::PointSizeEdgeFlagKillVertex && (r->used_write_mask() & 1);
    // User clip planes (VertexOptions): which distances go to the clip and
    // cull distances. A vertex's own (a point's corners'), or a point's
    // center's; with ps_ucp_mode 2 the center's cull and the corners' clip,
    // the corners' alone where both would pass the 8 clip and cull distances
    // every Vulkan device has.
    const uint32_t planes = std::min<uint32_t>(vertex_.clip_planes, 6);
    const bool cull_only = vertex_.clip_planes_cull;
    const uint32_t ucp_mode = vertex_.point_ucp_mode & 3;
    bool center_to_clip = false, center_to_cull = false, own_to_clip = false, own_to_cull = false;
    if (planes) {
        if (!points || ucp_mode == 3) {
            (cull_only ? own_to_cull : own_to_clip) = true;
        } else if (ucp_mode <= 1) {
            (cull_only ? center_to_cull : center_to_clip) = true;
        } else {
            center_to_cull = cull_only || planes <= 4;
            own_to_clip = !cull_only;
        }
    }
    Id clip_distances = 0, cull_distances = 0;
    if (center_to_clip || own_to_clip) {
        m_.capability(spv::Capability::ClipDistance);
        clip_distances = global(m_.type_array(float_, planes), spv::StorageClass::Output, "clip_distances");
        m_.decorate(clip_distances, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::ClipDistance)});
    }
    if (center_to_cull || own_to_cull) {
        m_.capability(spv::Capability::CullDistance);
        cull_distances = global(m_.type_array(float_, planes), spv::StorageClass::Output, "cull_distances");
        m_.decorate(cull_distances, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::CullDistance)});
    }
    // a rectangle's three guest vertices, run one after another: their host
    // positions and interpolators, and their guest positions for the planes
    Id rectangle_positions = 0, rectangle_interpolators = 0, rectangle_guest_positions = 0;
    if (rectangles) {
        rectangle_positions = global(m_.type_array(vec4_, 3), spv::StorageClass::Private, "rectangle_positions");
        rectangle_interpolators =
            global(m_.type_array(vec4_, 48), spv::StorageClass::Private, "rectangle_interpolators");
        if (planes)
            rectangle_guest_positions =
                global(m_.type_array(vec4_, 3), spv::StorageClass::Private, "rectangle_guest_positions");
    }

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    Id ptr_u_vec4 = m_.type_pointer(spv::StorageClass::Uniform, vec4_);
    Id ptr_out_vec4 = m_.type_pointer(spv::StorageClass::Output, vec4_);
    auto reg = [&](uint32_t k) { return m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}); };
    auto interpolator = [&](uint32_t k) {
        return m_.op(Op::OpAccessChain, ptr_private_vec4_, {interpolators_, i(int32_t(k))});
    };
    auto as_uint = [&](Id x) { return m_.op(Op::OpBitcast, uint_, {x}); };
    // the strip's corner this invocation makes (the expanded types)
    Id corner = 0;
    if (points || rectangles) corner = m_.op(Op::OpBitwiseAnd, uint_, {as_uint(load(int_, vertex_index)), u(3)});

    // The guest shader once: the registers zeroed (as Xenia's, for shaders
    // that read what they set only on some paths), the inputs in r0 and r1,
    // the export under way at the end done.
    auto run_guest = [&](Id r0, Id r1) {
        for (uint32_t k = 0; k < harness::REGISTERS; k++) store(reg(k), zero4_);
        store(reg(0), r0);
        if (r1) store(reg(1), r1);
        reset_state();
        m_.op(Op::OpFunctionCall, void_, {body_});
        export_to_memory(em_before_end_);
    };
    // The position's formats (PA_CL_VTE_CNTL, in the system flags' y), as
    // Xenia's CompleteVertexOrTessEvalShaderInMain: W given as 1/W unless bit
    // 0, XY divided by W with bit 1, Z with bit 2. Then to Vulkan's clip
    // space: xyz * ndc_scale + ndc_offset * w.
    auto guest_position = [&]() {
        Id flags = m_.op(Op::OpCompositeExtract, uint_,
                         {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_),
                                             {system_, u(3)})),
                          1});
        auto flag = [&](uint32_t bit) {
            return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(bit)}), u(0)});
        };
        Id p = load(vec4_, position_);
        Id w = comp(p, 3);
        w = select(float_, flag(1), w, exact(Op::OpFDiv, float_, {f(1), w}));
        Id x = select(float_, flag(2), exact(Op::OpFMul, float_, {comp(p, 0), w}), comp(p, 0));
        Id y = select(float_, flag(2), exact(Op::OpFMul, float_, {comp(p, 1), w}), comp(p, 1));
        Id z = select(float_, flag(4), exact(Op::OpFMul, float_, {comp(p, 2), w}), comp(p, 2));
        return vec4_of(x, y, z, w);
    };
    auto to_host = [&](Id xyzw) {
        Id scale = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(0)}));
        Id offset = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(1)}));
        Id w = comp(xyzw, 3);
        Id moved = exact(Op::OpFAdd, vec4_,
                         {exact(Op::OpFMul, vec4_, {xyzw, scale}), exact(Op::OpVectorTimesScalar, vec4_, {offset, w})});
        return vec4_of(comp(moved, 0), comp(moved, 1), comp(moved, 2), w);
    };
    // The guest position's distance to each enabled user clip plane (system
    // member 9), as Xenia's dp4, into a clip or cull distance array.
    auto put_distances = [&](Id array, Id xyzw) {
        if (!array) return;
        Id ptr_out_float = m_.type_pointer(spv::StorageClass::Output, float_);
        for (uint32_t k = 0; k < planes; k++) {
            Id plane = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(9), u(k)}));
            store(m_.op(Op::OpAccessChain, ptr_out_float, {array, i(int32_t(k))}),
                  m_.op(Op::OpDot, float_, {xyzw, plane}));
        }
    };
    // The final host position bounding its report (VertexOptions::extents):
    // xy over w, clamped to the viewport's NDC square (the host clips there),
    // mapped to console pixels (system member 10), then the edges as whole
    // pixels outwards, clamped to 0-8192; the whole viewport for a position
    // whose w is not above 0 (its primitive's clipped part can reach any
    // edge) or whose xy are not numbers. Atomic maxima at the report's
    // words (system member 11's x): 8192 less the left and top edges, the
    // right and bottom ones plus one.
    auto bound_extent = [&](Id p) {
        if (!extents) return;
        Id w = comp(p, 3);
        Id ndc =
            m_.op(Op::OpFDiv, vec2_,
                  {m_.op(Op::OpVectorShuffle, vec2_, {p, p, 0, 1}), m_.op(Op::OpCompositeConstruct, vec2_, {w, w})});
        Id in_front = m_.op(Op::OpFOrdGreaterThan, bool_, {w, f(0)});
        Id bvec2 = m_.type_vector(bool_, 2);
        Id numbers = m_.op(Op::OpLogicalNot, bool_, {m_.op(Op::OpAny, bool_, {m_.op(Op::OpIsNan, bvec2, {ndc})})});
        Id valid = m_.op(Op::OpLogicalAnd, bool_, {in_front, numbers});
        Id ones = m_.op(Op::OpCompositeConstruct, vec2_, {f(1), f(1)});
        Id minus_ones = m_.op(Op::OpCompositeConstruct, vec2_, {f(-1), f(-1)});
        Id lo = select(vec2_, valid, glsl(GLSLstd450FClamp, vec2_, {ndc, minus_ones, ones}), minus_ones);
        Id hi = select(vec2_, valid, glsl(GLSLstd450FClamp, vec2_, {ndc, minus_ones, ones}), ones);
        Id map = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(10)}));
        Id scale = m_.op(Op::OpVectorShuffle, vec2_, {map, map, 0, 2});
        Id offset = m_.op(Op::OpVectorShuffle, vec2_, {map, map, 1, 3});
        // both ends mapped, as a negative scale swaps them
        Id a = m_.op(Op::OpFAdd, vec2_, {m_.op(Op::OpFMul, vec2_, {lo, scale}), offset});
        Id b = m_.op(Op::OpFAdd, vec2_, {m_.op(Op::OpFMul, vec2_, {hi, scale}), offset});
        Id zeros = m_.op(Op::OpCompositeConstruct, vec2_, {f(0), f(0)});
        Id limits = m_.op(Op::OpCompositeConstruct, vec2_, {f(8192), f(8192)});
        Id low = glsl(GLSLstd450FClamp, vec2_,
                      {glsl(GLSLstd450Floor, vec2_, {glsl(GLSLstd450FMin, vec2_, {a, b})}), zeros, limits});
        Id high = glsl(GLSLstd450FClamp, vec2_,
                       {glsl(GLSLstd450Ceil, vec2_, {glsl(GLSLstd450FMax, vec2_, {a, b})}), zeros, limits});
        Id uvec2 = m_.type_vector(uint_, 2);
        Id low_u = m_.op(Op::OpConvertFToU, uvec2, {low}), high_u = m_.op(Op::OpConvertFToU, uvec2, {high});
        Id first = m_.op(Op::OpCompositeExtract, uint_,
                         {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_),
                                             {system_, u(11)})),
                          0});
        Id ptr = m_.type_pointer(spv::StorageClass::StorageBuffer, uint_);
        Id device = u(uint32_t(spv::Scope::Device)), relaxed = u(0);
        const Id values[4] = {m_.op(Op::OpISub, uint_, {u(8192), m_.op(Op::OpCompositeExtract, uint_, {low_u, 0})}),
                              m_.op(Op::OpISub, uint_, {u(8192), m_.op(Op::OpCompositeExtract, uint_, {low_u, 1})}),
                              m_.op(Op::OpIAdd, uint_, {m_.op(Op::OpCompositeExtract, uint_, {high_u, 0}), u(1)}),
                              m_.op(Op::OpIAdd, uint_, {m_.op(Op::OpCompositeExtract, uint_, {high_u, 1}), u(1)})};
        for (uint32_t k = 0; k < 4; k++) {
            Id at = m_.op(Op::OpAccessChain, ptr, {extents, u(0), m_.op(Op::OpIAdd, uint_, {first, u(k)})});
            m_.op(Op::OpAtomicUMax, uint_, {at, device, relaxed, values[k]});
        }
    };
    // oPts.x clamped to PA_SU_POINT_MINMAX (system values' y and z) as Xenia
    // does, like signed integers (-NaN and negatives to the minimum, NaN to
    // the maximum); -1 when the shader does not write it, for the constant
    // size
    auto guest_point_size = [&]() {
        if (!writes_point_size) return f(-1.0f);
        Id values = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(4)}));
        auto as_int = [&](Id x) { return m_.op(Op::OpBitcast, int_, {x}); };
        Id bits = glsl(GLSLstd450SMax, int_, {as_int(comp(values, 1)), as_int(comp(load(vec4_, point_size_), 0))});
        bits = glsl(GLSLstd450SMin, int_, {as_int(comp(values, 2)), bits});
        return m_.op(Op::OpBitcast, float_, {bits});
    };
    auto put_interpolators = [&]() {
        for (uint32_t k = 0; k < 16; k++)
            store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))}), load(vec4_, interpolator(k)));
    };

    if (rectangles) {
        // The three guest vertices, then the strip rectangle_list.geom makes
        // of them (Xenia's rectangle list geometry shader): the vertex
        // opposite the longest edge first, the fourth mirrored across that
        // edge (-a + b + c), position and interpolators alike. Guest vertex k
        // exports memory in corner k's invocation only.
        Id indices = load(uvec3, guest_indices);
        Id ptr_private_vec4 = ptr_private_vec4_;
        for (uint32_t k = 0; k < 3; k++) {
            if (export_allowed_) store(export_allowed_, m_.op(Op::OpIEqual, bool_, {corner, u(k)}));
            Id index = m_.op(Op::OpCompositeExtract, uint_, {indices, k});
            run_guest(vec4_of(m_.op(Op::OpConvertUToF, float_, {index}), f(0), f(0), f(0)), 0);
            Id guest = guest_position();
            store(m_.op(Op::OpAccessChain, ptr_private_vec4, {rectangle_positions, i(int32_t(k))}), to_host(guest));
            if (rectangle_guest_positions)
                store(m_.op(Op::OpAccessChain, ptr_private_vec4, {rectangle_guest_positions, i(int32_t(k))}), guest);
            for (uint32_t j = 0; j < 16; j++)
                store(m_.op(Op::OpAccessChain, ptr_private_vec4, {rectangle_interpolators, i(int32_t(k * 16 + j))}),
                      load(vec4_, interpolator(j)));
        }
        Id p[3];
        for (uint32_t k = 0; k < 3; k++)
            p[k] = load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4, {rectangle_positions, i(int32_t(k))}));
        auto edge = [&](uint32_t a, uint32_t b) {
            Id d = m_.op(Op::OpFSub, vec2_,
                         {m_.op(Op::OpVectorShuffle, vec2_, {p[b], p[b], 0, 1}),
                          m_.op(Op::OpVectorShuffle, vec2_, {p[a], p[a], 0, 1})});
            return m_.op(Op::OpDot, float_, {d, d});
        };
        Id e12 = edge(1, 2), e20 = edge(2, 0), e01 = edge(0, 1);
        auto greater = [&](Id x, Id y) { return m_.op(Op::OpFOrdGreaterThan, bool_, {x, y}); };
        Id first = select(uint_, m_.op(Op::OpLogicalAnd, bool_, {greater(e12, e20), greater(e12, e01)}), u(0),
                          select(uint_, greater(e20, e01), u(1), u(2)));
        auto mod3 = [&](Id x) { return m_.op(Op::OpUMod, uint_, {x, u(3)}); };
        Id second = mod3(m_.op(Op::OpIAdd, uint_, {first, u(1)}));
        Id third = mod3(m_.op(Op::OpIAdd, uint_, {first, u(2)}));
        Id fourth = m_.op(Op::OpIEqual, bool_, {corner, u(3)});
        Id chosen = mod3(m_.op(Op::OpIAdd, uint_, {first, corner}));
        auto pick = [&](Id array, uint32_t base) {
            auto at = [&](Id v) {
                Id index = base ? m_.op(Op::OpIAdd, uint_, {m_.op(Op::OpIMul, uint_, {v, u(16)}), u(base - 1)}) : v;
                return load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4, {array, index}));
            };
            Id mirrored =
                exact(Op::OpFAdd, vec4_,
                      {exact(Op::OpFAdd, vec4_, {exact(Op::OpFNegate, vec4_, {at(first)}), at(second)}), at(third)});
            return select(vec4_, fourth, mirrored, at(chosen));
        };
        store(position, pick(rectangle_positions, 0));
        bound_extent(load(vec4_, position));
        if (rectangle_guest_positions) {
            // the corner's guest position, made as its host one: distances
            // mirrored as the position is
            Id guest = pick(rectangle_guest_positions, 0);
            put_distances(clip_distances, guest);
            put_distances(cull_distances, guest);
        }
        for (uint32_t j = 0; j < 16; j++)
            store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(j))}),
                  pick(rectangle_interpolators, j + 1));
        store(point_size, f(-1.0f));
    } else {
        Id r0 = 0, r1 = 0;
        if (domain) {
            // as Xenia's D3D12 translator's StartVertexOrDomainShader: the
            // barycentric coordinates as zyx in r0.xyz (415607E1, 4D5307F2),
            // the UV coordinates in r0.xy for control point indexed quads and
            // r0.yz for patch indexed ones, the indices after them, and 0 as
            // the coordinates' swizzle for the patch indexed types (r1.y for
            // triangles, r1.x for quads); w stays 0 (Xenia's Vulkan
            // translator writes 1 in some of them; its D3D12 one does not)
            Id tc = load(vec3, tess_coord);
            Id ptr_input_float = m_.type_pointer(spv::StorageClass::Input, float_);
            auto cp = [&](uint32_t k) {
                return load(float_, m_.op(Op::OpAccessChain, ptr_input_float, {control_points, i(int32_t(k))}));
            };
            auto t = [&](uint32_t k) { return m_.op(Op::OpCompositeExtract, float_, {tc, k}); };
            switch (host) {
            case HostVertex::TriangleDomainCPIndexed:
                r0 = vec4_of(t(2), t(1), t(0), f(0));
                r1 = vec4_of(cp(0), cp(1), cp(2), f(0));
                break;
            case HostVertex::TriangleDomainPatchIndexed:
                r0 = vec4_of(t(2), t(1), t(0), f(0));
                r1 = vec4_of(cp(0), f(0), f(0), f(0));
                break;
            case HostVertex::QuadDomainCPIndexed:
                r0 = vec4_of(t(0), t(1), cp(0), f(0));
                r1 = vec4_of(cp(1), cp(2), cp(3), f(0));
                break;
            default: // QuadDomainPatchIndexed
                r0 = vec4_of(cp(0), t(0), t(1), f(0));
                break;
            }
        } else {
            // the vertex index in r0.x (the renderer adds the index offset and
            // endianness); a point's is above its sprite's corner
            Id index = load(int_, vertex_index);
            Id value = points ? m_.op(Op::OpConvertUToF, float_,
                                      {m_.op(Op::OpShiftRightLogical, uint_, {as_uint(index), u(2)})})
                              : m_.op(Op::OpConvertSToF, float_, {index});
            if (export_allowed_) store(export_allowed_, m_.op(Op::OpIEqual, bool_, {corner, u(0)}));
            r0 = vec4_of(value, f(0), f(0), f(0));
        }
        run_guest(r0, r1);
        Id guest = guest_position();
        Id p = to_host(guest);
        Id size = guest_point_size();
        if (points) {
            // The sprite, as Xenia's vertex shader expansion makes it
            // (spirv_shader_translator.cc, kPointListAsTriangleStrip) and
            // point_list.geom: the vertex shader's diameter where it writes
            // one (not negative), else PA_SU_POINT_SIZE's, in guest pixels to
            // the radius in clip space, without fused multiply-adds; corner
            // bit 1 the right side, bit 0 the bottom.
            Id point = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(6)}));
            Id own = m_.op(Op::OpFOrdGreaterThanEqual, bool_, {size, f(0)});
            Id w = comp(p, 3);
            auto radius = [&](uint32_t c) {
                Id diameter = select(float_, own, size, comp(point, c));
                return exact(Op::OpFMul, float_, {exact(Op::OpFMul, float_, {diameter, comp(point, c + 2)}), w});
            };
            auto side = [&](uint32_t bit) {
                return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {corner, u(bit)}), u(0)});
            };
            Id rx = radius(0), ry = radius(1);
            Id x = exact(Op::OpFAdd, float_,
                         {comp(p, 0), select(float_, side(2), rx, exact(Op::OpFNegate, float_, {rx}))});
            Id y = exact(Op::OpFAdd, float_,
                         {comp(p, 1), select(float_, side(1), ry, exact(Op::OpFNegate, float_, {ry}))});
            p = vec4_of(x, y, comp(p, 2), w);
            store(point_coordinates, m_.op(Op::OpCompositeConstruct, m_.type_vector(float_, 2),
                                           {select(float_, side(2), f(1), f(0)), select(float_, side(1), f(1), f(0))}));
            if (planes) {
                // the center's distances, and the corner's: its offset from
                // the center taken back to the guest's clip space through the
                // NDC scale (0 where the scale is)
                put_distances(center_to_clip ? clip_distances : 0, guest);
                put_distances(center_to_cull ? cull_distances : 0, guest);
                if (own_to_clip || own_to_cull) {
                    Id scale = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(0)}));
                    auto guest_offset = [&](Id host_offset, uint32_t c) {
                        Id s = comp(scale, c);
                        return select(float_, m_.op(Op::OpFOrdEqual, bool_, {s, f(0)}), f(0),
                                      exact(Op::OpFDiv, float_, {host_offset, s}));
                    };
                    Id dx = guest_offset(select(float_, side(2), rx, exact(Op::OpFNegate, float_, {rx})), 0);
                    Id dy = guest_offset(select(float_, side(1), ry, exact(Op::OpFNegate, float_, {ry})), 1);
                    Id corner_guest =
                        vec4_of(exact(Op::OpFAdd, float_, {comp(guest, 0), dx}),
                                exact(Op::OpFAdd, float_, {comp(guest, 1), dy}), comp(guest, 2), comp(guest, 3));
                    put_distances(own_to_clip ? clip_distances : 0, corner_guest);
                    put_distances(own_to_cull ? cull_distances : 0, corner_guest);
                }
            }
        } else {
            put_distances(clip_distances, guest);
            put_distances(cull_distances, guest);
        }
        store(position, p);
        bound_extent(p);
        put_interpolators();
        store(point_size, size);
    }
    if (host_point_size) store(host_point_size, f(1.0f));
    m_.op_void(Op::OpReturn);
    m_.end_function();
    if (domain) {
        m_.entry_point(spv::ExecutionModel::TessellationEvaluation, main, "main", interface_);
        bool quads = host == HostVertex::QuadDomainCPIndexed || host == HostVertex::QuadDomainPatchIndexed;
        m_.execution_mode(main, quads ? spv::ExecutionMode::Quads : spv::ExecutionMode::Triangles);
        // Xenia's hull shaders' partitioning: integer for discrete, fractional
        // even otherwise; their triangle_cw winding (Xenia's Vulkan
        // translator: VertexOrderCw keeps the faces Direct3D 12's have)
        m_.execution_mode(main, vertex_.equal_spacing ? spv::ExecutionMode::SpacingEqual
                                                      : spv::ExecutionMode::SpacingFractionalEven);
        m_.execution_mode(main, spv::ExecutionMode::VertexOrderCw);
    } else {
        m_.entry_point(spv::ExecutionModel::Vertex, main, "main", interface_);
    }
}

void Translator::main_pixel() {
    m_.capability(spv::Capability::DemoteToHelperInvocation);
    // The interpolators: one array, or with some read at the centroid
    // (PixelOptions::centroid) one variable each, those Centroid-decorated, as
    // Xenia's translators declare them (an array's elements cannot be
    // decorated apart). The vertex shader's array covers the same locations.
    Id inputs = 0;
    Id interpolators[16] = {};
    if (pixel_.centroid) {
        for (uint32_t k = 0; k < 16; k++) {
            interpolators[k] = global(vec4_, spv::StorageClass::Input, "interpolator");
            m_.decorate(interpolators[k], spv::Decoration::Location, {k});
            if ((pixel_.centroid >> k) & 1) m_.decorate(interpolators[k], spv::Decoration::Centroid);
        }
    } else {
        inputs = global(m_.type_array(vec4_, 16), spv::StorageClass::Input, "interpolators");
        m_.decorate(inputs, spv::Decoration::Location, {0});
    }
    // the interlock path (PixelOptions::fsi) writes EDRAM, no outputs
    const bool fsi = pixel_.fsi != 0;
    Id outputs = 0;
    if (!fsi) {
        outputs = global(m_.type_array(vec4_, 4), spv::StorageClass::Output, "colors");
        m_.decorate(outputs, spv::Decoration::Location, {0});
    }
    bool writes_depth = false, kills = false;
    for (const ControlFlow &cf : s_.control_flow)
        if (const auto *e = std::get_if<Exec>(&cf.instruction))
            for (const ExecSlot &slot : e->slots)
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction)) {
                    writes_depth |= a->vector_and_constant_result.target == StorageTarget::Depth ||
                                    a->scalar_result.target == StorageTarget::Depth;
                    kills |= (vector_opcode_info(a->vector_opcode).changed_state & CHANGES_PIXEL_KILL) ||
                             (scalar_opcode_info(a->scalar_opcode).changed_state & CHANGES_PIXEL_KILL);
                }
    // the depth converted to 20e4 (PixelOptions), oDepth's or the rasterized one
    const uint32_t float24 = fsi ? 0 : pixel_.depth_float24;
    // Xenia's IsForceEarlyDepthStencilGlobalFlagEnabled: the early hint, for a
    // shader whose implicit early depth write is allowed (no kill, no oDepth,
    // no memory export)
    const bool early = pixel_.early_depth_stencil && !fsi && !float24 && !writes_depth && !kills && !em_written_;
    Id frag_depth = 0;
    if (!fsi && (writes_depth || float24)) {
        frag_depth = global(float_, spv::StorageClass::Output, "depth");
        m_.decorate(frag_depth, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::FragDepth)});
    }

    // for the pixel parameters
    Id frag_coord = global(vec4_, spv::StorageClass::Input, "frag_coord");
    m_.decorate(frag_coord, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::FragCoord)});
    // A host pixel's position as the console's pixel at a resolution scale:
    // times the system values' w (1 / the scale; 0 where a caller leaves it,
    // taken as 1). What the guest sees of the pixel's position (the pixel
    // parameters, alpha to coverage's dither) is the console pixel's.
    auto console_position = [&](Id position, uint32_t component) {
        Id per_pixel = comp(load(vec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, vec4_),
                                              {system_, u(4)})),
                            3);
        per_pixel = select(float_, m_.op(Op::OpFOrdEqual, bool_, {per_pixel, f(0)}), f(1), per_pixel);
        return exact(Op::OpFMul, float_, {comp(position, component), per_pixel});
    };
    if (float24 && !writes_depth) {
        // the rasterized depth converted per sample, for the antialiasing of
        // intersections: a Sample-decorated input runs the shader per sample
        // (Xenia's IsSampleRate), the other inputs still at the pixel's center
        m_.capability(spv::Capability::SampleRateShading);
        m_.decorate(frag_coord, spv::Decoration::Sample);
    }
    Id front_facing = global(bool_, spv::StorageClass::Input, "front_facing");
    m_.decorate(front_facing, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::FrontFacing)});
    Id point_coordinates = 0;
    if (pixel_.point_coordinates) {
        point_coordinates = global(m_.type_vector(float_, 2), spv::StorageClass::Input, "point_coordinates");
        m_.decorate(point_coordinates, spv::Decoration::Location, {17});
    }
    // for alpha to coverage, when the shader writes oC0
    bool writes_color_0 = false;
    for (const ControlFlow &cf : s_.control_flow)
        if (const auto *e = std::get_if<Exec>(&cf.instruction))
            for (const ExecSlot &slot : e->slots)
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                    for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                        writes_color_0 |= r->target == StorageTarget::Color && (r->index & 3) == 0 && r->used_write_mask();
    Id sample_mask = 0;
    if (writes_color_0 && !fsi && !early) {
        sample_mask = global(m_.type_array(int_, 1), spv::StorageClass::Output, "sample_mask");
        m_.decorate(sample_mask, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::SampleMask)});
    }
    // the interlock path's: the host's coverage, and whether this is a
    // helper invocation (Volatile: demotion changes it)
    Id sample_mask_in = 0, helper = 0;
    if (fsi) {
        sample_mask_in = global(m_.type_array(int_, 1), spv::StorageClass::Input, "sample_mask_in");
        m_.decorate(sample_mask_in, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::SampleMask)});
        helper = global(bool_, spv::StorageClass::Input, "helper");
        m_.decorate(helper, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::HelperInvocation)});
        m_.decorate(helper, spv::Decoration::Volatile);
    }

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    Id ptr_in_vec4 = m_.type_pointer(spv::StorageClass::Input, vec4_);
    for (uint32_t k = 0; k < harness::REGISTERS; k++) {
        Id value = zero4_;
        if (k < 16)
            value =
                load(vec4_, inputs ? m_.op(Op::OpAccessChain, ptr_in_vec4, {inputs, i(int32_t(k))}) : interpolators[k]);
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}), value);
    }
    // The pixel parameters (SQ_PROGRAM_CNTL's param_gen), as Xenia's
    // StartFragmentShaderInMain writes them into the interpolator
    // SQ_CONTEXT_MISC names, by the system flags' y (bit 8 on, bits 9-12
    // the interpolator, bit 13 a point, 14 a line, 15 a polygon): x the
    // pixel's column (floored), negative for a polygon's back face; y its
    // row, negative for a point; z -0 for a line, else 0, w 0; for a point
    // list (PixelOptions::point_coordinates) z and w the sprite's
    // coordinates, clamped to 0-1 (NClamp: where the pixel's center is not
    // covered they are extrapolated).
    {
        Id flags = m_.op(Op::OpCompositeExtract, uint_,
                         {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_),
                                             {system_, u(3)})),
                          1});
        auto flag = [&](uint32_t bit) {
            return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(1u << bit)}), u(0)});
        };
        when(flag(8), [&] {
            Id position = load(vec4_, frag_coord);
            Id sign_bit = u(1u << 31);
            auto negated = [&](Id x) {
                return m_.op(Op::OpBitcast, float_,
                             {m_.op(Op::OpBitwiseXor, uint_, {m_.op(Op::OpBitcast, uint_, {x}), sign_bit})});
            };
            Id x = glsl(GLSLstd450FAbs, float_, {glsl(GLSLstd450Floor, float_, {console_position(position, 0)})});
            Id y = glsl(GLSLstd450FAbs, float_, {glsl(GLSLstd450Floor, float_, {console_position(position, 1)})});
            Id back = m_.op(Op::OpLogicalAnd, bool_, {flag(15), m_.op(Op::OpLogicalNot, bool_, {load(bool_, front_facing)})});
            x = select(float_, back, negated(x), x);
            y = select(float_, flag(13), negated(y), y);
            Id z = select(float_, flag(14), negated(f(0)), f(0));
            Id w = f(0);
            if (point_coordinates) {
                Id vec2 = m_.type_vector(float_, 2);
                Id zero2 = m_.op(Op::OpCompositeConstruct, vec2, {f(0), f(0)});
                Id one2 = m_.op(Op::OpCompositeConstruct, vec2, {f(1), f(1)});
                Id st = glsl(GLSLstd450NClamp, vec2, {load(vec2, point_coordinates), zero2, one2});
                z = m_.op(Op::OpCompositeExtract, float_, {st, 0});
                w = m_.op(Op::OpCompositeExtract, float_, {st, 1});
            }
            Id index = m_.op(Op::OpBitFieldUExtract, uint_, {flags, u(9), u(4)});
            // into the register the index names, each by a constant index: a
            // variable index into the registers made the host's compiler keep
            // the whole array in memory, every access of every pixel shader a
            // memory access (the post-processing passes of a UE3 game's race
            // took 5 ms of the GPU each at 1280x720, 0.2 ms without)
            Id value = vec4_of(x, y, z, w);
            for (uint32_t k = 0; k < 16; k++) {
                Id pointer = m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))});
                Id here = m_.op(Op::OpIEqual, bool_, {index, u(k)});
                store(pointer, select(vec4_, here, value, load(vec4_, pointer)));
            }
            return false;
        });
    }
    reset_state();
    m_.op(Op::OpFunctionCall, void_, {body_});
    export_to_memory(em_before_end_); // the export under way at the end
    if (frag_depth && float24) {
        // As Xenia's CompletePixelShader_DSV_DepthTo24Bit: oDepth clamped to
        // 0-1, or the rasterized depth from the host's 0-0.5 back to 0-1,
        // saturated; then to 20e4 and back, halved.
        Id depth = writes_depth ? comp(load(vec4_, depth_), 0)
                                : exact(Op::OpFMul, float_, {comp(load(vec4_, frag_coord), 2), f(2.0f)});
        depth = glsl(GLSLstd450NClamp, float_, {depth, f(0), f(1)});
        Id bits = m_.op(Op::OpBitcast, uint_, {depth});
        Id result;
        if (float24 == 1) {
            // towards zero: the mantissa's low bits dropped, 3 for a normal
            // 20e4 (exponent -14 and up), up to 23 at -34; below that 0
            Id exponent = m_.op(Op::OpBitFieldUExtract, uint_, {bits, u(23), u(8)});
            Id drop = glsl(GLSLstd450SMax, int_,
                           {m_.op(Op::OpISub, int_, {i(116), m_.op(Op::OpBitcast, int_, {exponent})}), i(3)});
            drop = glsl(GLSLstd450SMin, int_, {drop, i(31)});
            Id truncated = m_.op(Op::OpBitFieldInsert, uint_, {bits, u(0), u(0), m_.op(Op::OpBitcast, uint_, {drop})});
            Id halved = exact(Op::OpFMul, float_, {m_.op(Op::OpBitcast, float_, {truncated}), f(0.5f)});
            result = select(float_, m_.op(Op::OpUGreaterThanEqual, bool_, {bits, u(0x2E800000)}), halved, f(0));
        } else {
            // to the nearest even (xenos::Float32To20e4 and Float20e4To32)
            Id is_small = m_.op(Op::OpULessThan, bool_, {bits, u(0x38800000)});
            Id shift = m_.op(Op::OpISub, uint_, {u(113), m_.op(Op::OpShiftRightLogical, uint_, {bits, u(23)})});
            shift = glsl(GLSLstd450UMin, uint_, {shift, u(24)});
            Id denormal = m_.op(
                Op::OpShiftRightLogical, uint_,
                {m_.op(Op::OpBitwiseOr, uint_, {m_.op(Op::OpBitwiseAnd, uint_, {bits, u(0x7FFFFF)}), u(0x800000)}),
                 shift});
            Id f24 = select(uint_, is_small, denormal, m_.op(Op::OpIAdd, uint_, {bits, u(0xC8000000u)}));
            f24 = m_.op(Op::OpIAdd, uint_,
                        {f24, m_.op(Op::OpIAdd, uint_,
                                    {u(3), m_.op(Op::OpBitwiseAnd, uint_,
                                                 {m_.op(Op::OpShiftRightLogical, uint_, {f24, u(3)}), u(1)})})});
            f24 = m_.op(Op::OpBitwiseAnd, uint_, {m_.op(Op::OpShiftRightLogical, uint_, {f24, u(3)}), u(0xFFFFFF)});
            f24 = select(uint_, m_.op(Op::OpFOrdGreaterThan, bool_, {depth, f(0)}), f24, u(0));
            Id mantissa = m_.op(Op::OpBitwiseAnd, uint_, {f24, u(0xFFFFF)});
            Id exponent24 = m_.op(Op::OpShiftRightLogical, uint_, {f24, u(20)});
            // a denormal 20e4 normalized: the mantissa's leading one moved to bit 20
            Id msb = glsl(GLSLstd450FindUMsb, int_, {mantissa});
            Id lz = m_.op(Op::OpISub, int_, {i(20), msb});
            Id denormal24 = m_.op(Op::OpIEqual, bool_, {exponent24, u(0)});
            Id exponent32 =
                select(int_, denormal24, m_.op(Op::OpISub, int_, {i(1), lz}), m_.op(Op::OpBitcast, int_, {exponent24}));
            Id mantissa32 =
                select(uint_, denormal24,
                       m_.op(Op::OpBitwiseAnd, uint_,
                             {m_.op(Op::OpShiftLeftLogical, uint_,
                                    {mantissa, m_.op(Op::OpBitcast, uint_, {glsl(GLSLstd450SMax, int_, {lz, i(0)})})}),
                              u(0xFFFFF)}),
                       mantissa);
            Id out_bits =
                m_.op(Op::OpBitwiseOr, uint_,
                      {m_.op(Op::OpShiftLeftLogical, uint_,
                             {m_.op(Op::OpBitcast, uint_, {m_.op(Op::OpIAdd, int_, {exponent32, i(112)})}), u(23)}),
                       m_.op(Op::OpShiftLeftLogical, uint_, {mantissa32, u(3)})});
            Id value = exact(Op::OpFMul, float_, {m_.op(Op::OpBitcast, float_, {out_bits}), f(0.5f)});
            result = select(float_, m_.op(Op::OpIEqual, bool_, {f24, u(0)}), f(0), value);
        }
        store(frag_depth, result);
    } else if (frag_depth) {
        // oDepth clamped to 0-1, as Xenia does, then halved for a D24FS8
        // target (system flags' w, bit 0), which keeps its 0-2 as 0-1
        Id depth = glsl(GLSLstd450NClamp, float_, {comp(load(vec4_, depth_), 0), f(0), f(1)});
        Id flags_pointer =
            m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)});
        Id flags = m_.op(Op::OpCompositeExtract, uint_, {load(uvec4_, flags_pointer), 3});
        Id float24 = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(1)}), u(0)});
        store(frag_depth, select(float_, float24, exact(Op::OpFMul, float_, {depth, f(0.5f)}), depth));
    }
    // the alpha test (state, not microcode): oC0's alpha against the
    // reference, by the function in the system constants (the early form is
    // for draws without one)
    if (!early) {
        Id flags = m_.op(Op::OpCompositeExtract, uint_, {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)})), 0});
        Id reference = comp(load(vec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, vec4_), {system_, u(4)})), 0);
        Id alpha = comp(load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(0)})), 3);
        Id function = m_.op(Op::OpBitwiseAnd, uint_, {flags, u(7)});
        auto is = [&](uint32_t value) { return m_.op(Op::OpIEqual, bool_, {function, u(value)}); };
        // never, less, equal, less or equal, greater, not equal, greater or equal, always
        Id pass = m_.const_bool(true);
        const Op tests[] = {Op::OpFOrdLessThan, Op::OpFOrdEqual, Op::OpFOrdLessThanEqual, Op::OpFOrdGreaterThan,
                            Op::OpFUnordNotEqual, Op::OpFOrdGreaterThanEqual};
        for (uint32_t k = 0; k < 6; k++) pass = select(bool_, is(k + 1), m_.op(tests[k], bool_, {alpha, reference}), pass);
        pass = select(bool_, is(0), m_.const_bool(false), pass);
        Id enabled = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(8)}), u(0)});
        // the interlock path as Xenia's: only where oC0 is written, on the
        // shader's path too
        if (fsi)
            enabled = writes_color_0
                          ? m_.op(Op::OpLogicalAnd, bool_,
                                  {enabled, m_.op(Op::OpINotEqual, bool_,
                                                  {m_.op(Op::OpBitwiseAnd, uint_, {load(uint_, colors_written_), u(1)}),
                                                   u(0)})})
                          : m_.const_bool(false);
        when(m_.op(Op::OpLogicalAnd, bool_, {enabled, m_.op(Op::OpLogicalNot, bool_, {pass})}), [&] {
            m_.op_void(Op::OpDemoteToHelperInvocation);
            return false;
        });
    }
    // Alpha to coverage, as Xenia's host render target path makes it
    // (spirv_shader_translator_rb.cc's FBO alpha to mask): with the system
    // flags' x bit 16 on, oC0's unscaled alpha against each sample's
    // threshold, less the pixel's dither offset (bits 8-15, two bits each,
    // picked by the pixel's column and row parity) times the step, sets the
    // sample's bit of the coverage: 1x 1.0 by quarters; 2x (host samples 1
    // and 0, the host's order) 0.5 and 1.0 by eighths; 4x 0.75, 0.25, 0.5,
    // 1.0 by sixteenths (the sample count in the flags' w bits 1-2). No
    // sample covered: the pixel is dropped.
    if (sample_mask) {
        Id ptr_out_int = m_.type_pointer(spv::StorageClass::Output, int_);
        Id mask_element = m_.op(Op::OpAccessChain, ptr_out_int, {sample_mask, i(0)});
        store(mask_element, i(-1));
        Id flags_vector =
            load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)}));
        Id flags = m_.op(Op::OpCompositeExtract, uint_, {flags_vector, 0});
        Id enabled = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(1u << 16)}), u(0)});
        when(enabled, [&] {
            Id position = load(vec4_, frag_coord);
            Id px = m_.op(Op::OpConvertFToU, uint_, {console_position(position, 0)});
            Id py = m_.op(Op::OpConvertFToU, uint_, {console_position(position, 1)});
            Id index = m_.op(Op::OpBitwiseOr, uint_,
                             {m_.op(Op::OpBitwiseAnd, uint_, {py, u(1)}),
                              m_.op(Op::OpShiftLeftLogical, uint_, {m_.op(Op::OpBitwiseAnd, uint_, {px, u(1)}), u(1)})});
            Id offset_bits = m_.op(Op::OpBitFieldUExtract, uint_,
                                   {flags, m_.op(Op::OpIAdd, uint_, {u(8), m_.op(Op::OpShiftLeftLogical, uint_, {index, u(1)})}),
                                    u(2)});
            Id offset = m_.op(Op::OpConvertUToF, float_, {offset_bits});
            Id alpha = comp(load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(0)})), 3);
            auto coverage = [&](std::initializer_list<std::pair<uint32_t, float>> samples, float step) {
                Id mask = u(0);
                for (auto [sample, base] : samples) {
                    Id threshold = exact(Op::OpFAdd, float_, {f(base), exact(Op::OpFMul, float_, {offset, f(-step)})});
                    Id passes = m_.op(Op::OpFOrdGreaterThanEqual, bool_, {alpha, threshold});
                    mask = select(uint_, passes, m_.op(Op::OpBitwiseOr, uint_, {mask, u(1u << sample)}), mask);
                }
                return mask;
            };
            Id msaa = m_.op(Op::OpBitFieldUExtract, uint_, {m_.op(Op::OpCompositeExtract, uint_, {flags_vector, 3}), u(1), u(2)});
            Id one = coverage({{0, 1.0f}}, 1.0f / 4.0f);
            Id two = coverage({{1, 0.5f}, {0, 1.0f}}, 1.0f / 8.0f);
            Id four = coverage({{0, 0.75f}, {1, 0.25f}, {2, 0.5f}, {3, 1.0f}}, 1.0f / 16.0f);
            Id mask = select(uint_, m_.op(Op::OpIEqual, bool_, {msaa, u(2)}), four,
                             select(uint_, m_.op(Op::OpIEqual, bool_, {msaa, u(1)}), two, one));
            store(mask_element, m_.op(Op::OpBitcast, int_, {mask}));
            when(m_.op(Op::OpIEqual, bool_, {mask, u(0)}), [&] {
                m_.op_void(Op::OpDemoteToHelperInvocation);
                return false;
            });
            return false;
        });
    }
    // The colors out, as Xenia's host render target path writes them: times
    // the target's scale (its exponent bias, and the range of a 16-bit fixed
    // target kept as snorm; system member 5), after the alpha test, which
    // takes the alpha unscaled; then to the piecewise linear gamma for gamma
    // targets (system flags' z, a bit per target; xenos.cc's
    // LinearToPWLGamma).
    if (fsi) {
        // The interlock path: alpha to coverage as a mask in the guest's
        // sample order (Xenia's FSI_AlphaToMask: 1x 1.0 by quarters, 2x 0.5
        // and 1.0 by eighths, 4x 0.75, 0.25, 0.5, 1.0 by sixteenths, less the
        // pixel's dither offset), where oC0 is written on the shader's path;
        // then the depth's coarse derivatives outside the interlock, and the
        // output merger (xf_fsi) inside it.
        Id ptr_function_uint = m_.type_pointer(spv::StorageClass::Function, uint_);
        Id coverage = m_.local_variable(ptr_function_uint);
        store(coverage, u(0xF));
        if (writes_color_0) {
            Id flags_vector = load(
                uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)}));
            Id flags = m_.op(Op::OpCompositeExtract, uint_, {flags_vector, 0});
            Id enabled = m_.op(
                Op::OpLogicalAnd, bool_,
                {m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(1u << 16)}), u(0)}),
                 m_.op(Op::OpINotEqual, bool_,
                       {m_.op(Op::OpBitwiseAnd, uint_, {load(uint_, colors_written_), u(1)}), u(0)})});
            when(enabled, [&] {
                Id position = load(vec4_, frag_coord);
                Id px = m_.op(Op::OpConvertFToU, uint_, {console_position(position, 0)});
                Id py = m_.op(Op::OpConvertFToU, uint_, {console_position(position, 1)});
                Id index = m_.op(
                    Op::OpBitwiseOr, uint_,
                    {m_.op(Op::OpBitwiseAnd, uint_, {py, u(1)}),
                     m_.op(Op::OpShiftLeftLogical, uint_, {m_.op(Op::OpBitwiseAnd, uint_, {px, u(1)}), u(1)})});
                Id offset = m_.op(Op::OpConvertUToF, float_,
                                  {m_.op(Op::OpBitFieldUExtract, uint_,
                                         {flags, m_.op(Op::OpIAdd, uint_, {u(8), m_.op(Op::OpShiftLeftLogical, uint_, {index, u(1)})}),
                                          u(2)})});
                Id alpha = comp(load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(0)})), 3);
                auto coverage_of = [&](std::initializer_list<std::pair<uint32_t, float>> samples, float step) {
                    Id mask = u(0xF);
                    for (auto [sample, base] : samples) {
                        Id threshold = exact(Op::OpFAdd, float_, {f(base), exact(Op::OpFMul, float_, {offset, f(-step)})});
                        Id passes = m_.op(Op::OpFOrdGreaterThanEqual, bool_, {alpha, threshold});
                        mask = select(uint_, passes, mask, m_.op(Op::OpBitwiseAnd, uint_, {mask, u(~(1u << sample))}));
                    }
                    return mask;
                };
                Id msaa = m_.op(Op::OpBitFieldUExtract, uint_,
                                {m_.op(Op::OpCompositeExtract, uint_, {flags_vector, 3}), u(1), u(2)});
                Id one = coverage_of({{0, 1.0f}}, 1.0f / 4.0f);
                Id two = coverage_of({{0, 0.5f}, {1, 1.0f}}, 1.0f / 8.0f);
                Id four = coverage_of({{0, 0.75f}, {1, 0.25f}, {2, 0.5f}, {3, 1.0f}}, 1.0f / 16.0f);
                store(coverage, select(uint_, m_.op(Op::OpIEqual, bool_, {msaa, u(2)}), four,
                                       select(uint_, m_.op(Op::OpIEqual, bool_, {msaa, u(1)}), two, one)));
                return false;
            });
        }
        m_.capability(spv::Capability::DerivativeControl);
        Id position = load(vec4_, frag_coord);
        Id z = comp(position, 2);
        Id dxy = m_.op(Op::OpCompositeConstruct, vec2_,
                       {m_.op(Op::OpDPdxCoarse, float_, {z}), m_.op(Op::OpDPdyCoarse, float_, {z})});
        Id ptr_in_int = m_.type_pointer(spv::StorageClass::Input, int_);
        Id mask_in = m_.op(Op::OpBitcast, uint_, {load(int_, m_.op(Op::OpAccessChain, ptr_in_int, {sample_mask_in, i(0)}))});
        Id alive = m_.op(Op::OpLogicalNot, bool_, {load(bool_, helper)});
        Id facing = load(bool_, front_facing);
        const std::pair<Id, Id> arguments[13] = {
            {load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(0)})), vec4_},
            {load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(1)})), vec4_},
            {load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(2)})), vec4_},
            {load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(3)})), vec4_},
            {load(uint_, colors_written_), uint_},
            {comp(load(vec4_, depth_), 0), float_},
            {m_.const_bool(writes_depth), bool_},
            {position, vec4_},
            {dxy, vec2_},
            {mask_in, uint_},
            {facing, bool_},
            {load(uint_, coverage), uint_},
            {alive, bool_}};
        // glslang's functions take their parameters as pointers
        std::vector<uint32_t> call{fsi_function_};
        for (uint32_t k = 0; k < 13; k++) {
            auto [value, type] = arguments[k];
            Id pointer_type = m_.type_pointer(spv::StorageClass::Function, type);
            if (fsi_parameters_[k] == pointer_type) {
                Id variable = m_.local_variable(pointer_type);
                store(variable, value);
                call.push_back(variable);
            } else {
                call.push_back(value);
            }
        }
        m_.extension("SPV_EXT_fragment_shader_interlock");
        m_.capability(pixel_.fsi == 2 ? spv::Capability::FragmentShaderSampleInterlockEXT
                                      : spv::Capability::FragmentShaderPixelInterlockEXT);
        m_.op_void(Op::OpBeginInvocationInterlockEXT);
        m_.op(Op::OpFunctionCall, void_, std::span<const uint32_t>(call));
        m_.op_void(Op::OpEndInvocationInterlockEXT);
    } else {
        Id ptr_out_vec4 = m_.type_pointer(spv::StorageClass::Output, vec4_);
        Id ptr_u_vec4 = m_.type_pointer(spv::StorageClass::Uniform, vec4_);
        Id scales = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(5)}));
        Id gamma_flags = m_.op(Op::OpCompositeExtract, uint_,
                               {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)})), 2});
        auto pwl = [&](Id linear) {
            Id l = glsl(GLSLstd450FClamp, float_, {linear, f(0), f(1)});
            auto at_least = [&](float edge) { return m_.op(Op::OpFOrdGreaterThanEqual, bool_, {l, f(edge)}); };
            Id high = at_least(512.0f / 1023.0f), mid = at_least(128.0f / 1023.0f), low = at_least(64.0f / 1023.0f);
            Id scale = select(float_, mid, select(float_, high, f(1023.0f / 8.0f), f(1023.0f / 4.0f)),
                              select(float_, low, f(1023.0f / 2.0f), f(1023.0f)));
            Id offset = select(float_, mid, select(float_, high, f(128.0f / 255.0f), f(64.0f / 255.0f)),
                               select(float_, low, f(32.0f / 255.0f), f(0.0f)));
            Id truncated = glsl(GLSLstd450Trunc, float_, {exact(Op::OpFMul, float_, {l, scale})});
            return exact(Op::OpFAdd, float_, {exact(Op::OpFMul, float_, {truncated, f(1.0f / 255.0f)}), offset});
        };
        // Lean (the default): a target the shader never writes gets the
        // zeros its register holds, with no arithmetic, and the gamma curve is
        // computed behind a branch on the target's flag (as Xenia's, which
        // declares only the written targets and branches on the flag) rather
        // than for every pixel and then selected. XF_PIXEL_OUTPUTS=full: every
        // target scaled and curved, then selected, as before.
        static const bool lean = [] {
            const char *v = std::getenv("XF_PIXEL_OUTPUTS");
            return !(v && std::string(v) == "full");
        }();
        uint32_t written = 0;
        for (const ControlFlow &cf : s_.control_flow)
            if (const auto *e = std::get_if<Exec>(&cf.instruction))
                for (const ExecSlot &slot : e->slots)
                    if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                        for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                            if (r->target == StorageTarget::Color) written |= 1u << (r->index & 3);
        // PixelOptions::blend_premultiply: the console applies the blend
        // factors before a minimum or maximum, the host's blending does not,
        // so a source factor that depends on the source and the blend
        // constant alone is multiplied in here, with the interlock path's
        // arithmetic (edram_fsi.frag's color_factor and alpha_factor: the
        // source and the constant clamped to 0-1 and NaN to 0 for a fixed-point
        // target, a factor of zero giving zero without a product, the product
        // clamped again), and against a destination factor of zero the
        // minimum or maximum with zero is taken here too. System member 13,
        // per target: the color's factor in bits 0-4 (RB_BLENDCONTROL's
        // numbers), then in 5-6 the minimum (1) or maximum (2) with zero, 7
        // the color is done here; the alpha's the same in bits 8-15; bit 16
        // the color is fixed-point, 17 the alpha. Member 14: the blend constant.
        Id premultiply_words = 0, premultiply_constant = 0;
        if (pixel_.blend_premultiply) {
            Id ptr_u_uvec4 = m_.type_pointer(spv::StorageClass::Uniform, uvec4_);
            premultiply_words = load(uvec4_, m_.op(Op::OpAccessChain, ptr_u_uvec4, {system_, u(13)}));
            premultiply_constant = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(14)}));
        }
        auto premultiplied = [&](Id color, uint32_t k) {
            Id word = m_.op(Op::OpCompositeExtract, uint_, {premultiply_words, k});
            auto bit = [&](uint32_t n) {
                return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {word, u(1u << n)}), u(0)});
            };
            auto fixed = [&](Id v, Id is_fixed) {
                Id flushed = select(float_, m_.op(Op::OpIsNan, bool_, {v}), f(0), v);
                Id clamped = glsl(GLSLstd450FClamp, float_, {flushed, f(0), f(1)});
                return select(float_, is_fixed, clamped, v);
            };
            Id fixed_color = bit(16), fixed_alpha = bit(17);
            Id source[4], constant[4];
            for (uint32_t j = 0; j < 4; j++) {
                source[j] = fixed(comp(color, j), j < 3 ? fixed_color : fixed_alpha);
                constant[j] = fixed(comp(premultiply_constant, j), j < 3 ? fixed_color : fixed_alpha);
            }
            auto half = [&](uint32_t j, uint32_t shift) {
                Id factor = m_.op(Op::OpBitFieldUExtract, uint_, {word, u(shift), u(5)});
                Id zero_op = m_.op(Op::OpBitFieldUExtract, uint_, {word, u(shift + 5), u(2)});
                auto is = [&](uint32_t n) { return m_.op(Op::OpIEqual, bool_, {factor, u(n)}); };
                auto one_minus = [&](Id v) { return exact(Op::OpFSub, float_, {f(1), v}); };
                // (the alpha's color factors are its alpha ones: source[3] and constant[3] are its own)
                Id by = f(1);
                by = select(float_, is(4), source[j], by);
                by = select(float_, is(5), one_minus(source[j]), by);
                by = select(float_, is(6), source[3], by);
                by = select(float_, is(7), one_minus(source[3]), by);
                by = select(float_, is(12), constant[j], by);
                by = select(float_, is(13), one_minus(constant[j]), by);
                by = select(float_, is(14), constant[3], by);
                by = select(float_, is(15), one_minus(constant[3]), by);
                Id r = fixed(exact(Op::OpFMul, float_, {source[j], by}), j < 3 ? fixed_color : fixed_alpha);
                r = select(float_, is(0), f(0), r);
                r = select(float_, m_.op(Op::OpIEqual, bool_, {zero_op, u(1)}), glsl(GLSLstd450FMin, float_, {r, f(0)}),
                           select(float_, m_.op(Op::OpIEqual, bool_, {zero_op, u(2)}),
                                  glsl(GLSLstd450FMax, float_, {r, f(0)}), r));
                return select(float_, bit(shift + 7), r, comp(color, j));
            };
            Id red = half(0, 0), green = half(1, 0), blue = half(2, 0);
            return vec4_of(red, green, blue, half(3, 8));
        };
        for (uint32_t k = 0; k < 4; k++) {
            Id out = m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))});
            if (lean && !(written & (1u << k))) {
                store(out, zero4_);
                continue;
            }
            Id color = load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(int32_t(k))}));
            color = exact(Op::OpVectorTimesScalar, vec4_, {color, comp(scales, k)});
            if (pixel_.blend_premultiply) color = premultiplied(color, k);
            Id gamma = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {gamma_flags, u(1u << k)}), u(0)});
            if (lean) {
                store(out, color);
                when(gamma, [&] {
                    Id r = comp(color, 0), g = comp(color, 1), b = comp(color, 2);
                    store(out, vec4_of(pwl(r), pwl(g), pwl(b), comp(color, 3)));
                    return false;
                });
                continue;
            }
            Id r = comp(color, 0), g = comp(color, 1), b = comp(color, 2);
            color = vec4_of(select(float_, gamma, pwl(r), r), select(float_, gamma, pwl(g), g),
                            select(float_, gamma, pwl(b), b), comp(color, 3));
            store(out, color);
        }
    }
    m_.op_void(Op::OpReturn);
    m_.end_function();
    m_.entry_point(spv::ExecutionModel::Fragment, main, "main", interface_);
    m_.execution_mode(main, spv::ExecutionMode::OriginUpperLeft);
    if (fsi)
        m_.execution_mode(main, pixel_.fsi == 2 ? spv::ExecutionMode::SampleInterlockOrderedEXT
                                                : spv::ExecutionMode::PixelInterlockOrderedEXT);
    if (frag_depth) m_.execution_mode(main, spv::ExecutionMode::DepthReplacing);
    if (early) m_.execution_mode(main, spv::ExecutionMode::EarlyFragmentTests);
    // truncation never raises the rasterized depth: early depth tests may
    // still reject (Xenia's SV_DepthLessEqual)
    if (float24 == 1 && !writes_depth) m_.execution_mode(main, spv::ExecutionMode::DepthLess);
}

Translation Translator::run() {
    analyze_memexport();
    if (stage_ == Stage::Pixel && pixel_.fsi) {
        // the interlock path's output merger, taken in first (its ids stay)
        spirv::Module::Imported imported;
        if (m_.import(pixel_.edram_library, imported)) {
            for (const auto &[name, function] : imported.functions)
                if (name.starts_with("xf_fsi(")) fsi_function_ = function;
            if (fsi_function_) fsi_parameters_ = imported.parameters[fsi_function_];
            interface_.insert(interface_.end(), imported.globals.begin(), imported.globals.end());
        }
        if (!fsi_function_ || fsi_parameters_.size() != 13)
            return {{}, {"the interlock path without its output merger (edram_fsi.frag's library)"}};
    }
    declare();
    if (stage_ != Stage::Harness) {
        m_.capability(spv::Capability::RuntimeDescriptorArray);
        // 1D, 2D and stacked textures alike as 2D arrays, as Xenia binds them
        image2d_type_ = m_.type_image(float_, spv::Dim::Dim2D, 0, true, false, 1, spv::ImageFormat::Unknown);
        image3d_type_ = m_.type_image(float_, spv::Dim::Dim3D, 0, false, false, 1, spv::ImageFormat::Unknown);
        imagecube_type_ = m_.type_image(float_, spv::Dim::Cube, 0, false, false, 1, spv::ImageFormat::Unknown);
        sampler_type_ = m_.type_sampler();
        images2d_ = global(m_.type_runtime_array(image2d_type_, 0), spv::StorageClass::UniformConstant, "images2d");
        images3d_ = global(m_.type_runtime_array(image3d_type_, 0), spv::StorageClass::UniformConstant, "images3d");
        imagescube_ = global(m_.type_runtime_array(imagecube_type_, 0), spv::StorageClass::UniformConstant, "imagescube");
        samplers_ = global(m_.type_runtime_array(sampler_type_, 0), spv::StorageClass::UniformConstant, "samplers");
        uint32_t binding = 0;
        for (Id v : {images2d_, images3d_, imagescube_, samplers_}) {
            m_.decorate(v, spv::Decoration::DescriptorSet, {1});
            m_.decorate(v, spv::Decoration::Binding, {binding++});
        }
    }

    // the body: the control flow, straight
    body_ = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.name(body_, "xenos_main");
    m_.label();
    bool branches = false;
    for (const ControlFlow &cf : s_.control_flow)
        branches |= cf.opcode == ControlFlowOpcode::LoopStart || cf.opcode == ControlFlowOpcode::LoopEnd ||
                    cf.opcode == ControlFlowOpcode::CondCall || cf.opcode == ControlFlowOpcode::Return ||
                    cf.opcode == ControlFlowOpcode::CondJmp;
    if (branches) {
        dispatch();
    } else {
        // straight: each exec in order, the first that ends returning
        for (const ControlFlow &cf : s_.control_flow) {
            if (ended_) break;
            if (const auto *e = std::get_if<Exec>(&cf.instruction)) ended_ = exec(*e);
            else if (const auto *a = std::get_if<AllocInstruction>(&cf.instruction)) alloc(*a);
        }
    }
    if (s_.reads_past_end) unsupported("an exec past the end of the microcode");
    if (!ended_) m_.op_void(Op::OpReturn);
    m_.end_function();
    emit_fetch_posts();

    switch (stage_) {
    case Stage::Harness: main_harness(); break;
    case Stage::Vertex: main_vertex(); break;
    case Stage::Pixel: main_pixel(); break;
    }

    Translation t;
    t.unsupported = std::move(unsupported_);
    std::sort(approximate_.begin(), approximate_.end());
    approximate_.erase(std::unique(approximate_.begin(), approximate_.end()), approximate_.end());
    t.approximate = std::move(approximate_);
    if (t.unsupported.empty()) t.spirv = m_.finish();
    return t;
}

} // namespace

Translation translate(const ParsedShader &shader, Stage stage, const VertexOptions &vertex, const PixelOptions &pixel) {
    if (stage == Stage::Vertex && shader.type != ShaderType::Vertex) return {{}, {"a pixel shader as a vertex shader"}};
    if (stage == Stage::Pixel && shader.type != ShaderType::Pixel) return {{}, {"a vertex shader as a pixel shader"}};
    return Translator(shader, stage, stage == Stage::Vertex ? vertex : VertexOptions{},
                      stage == Stage::Pixel ? pixel : PixelOptions{})
        .run();
}

} // namespace xf::shader
