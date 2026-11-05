// Xenos shaders to SPIR-V. Written anew on XenoForge's SPIR-V writer; the
// value each instruction computes follows Xenia's spirv_shader_translator_alu.cc
// and spirv_shader_translator_fetch.cc (BSD; see LICENSE-xenia): the legacy
// multiply (0 times anything is 0), multiplies and adds without contraction,
// the ordered and unordered comparisons, the 21-bit results of exp, log,
// rcp, rsq and sqrt, the clamps of rcpc, rcpf, rsqc, rsqf and logc, maxa's
// address register, the vertex formats and their normalization.
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
// Memexport, unnormalized cube map coordinates and the texture queries
// other than getGradients are reported as unsupported.

#include "xenoforge/shader_translator.h"

#include "xenoforge/shader_harness.h"
#include "xenoforge/spirv.h"

#include <GLSL.std.450.h>

#include <cfloat>
#include <cmath>

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
    Translator(const ParsedShader &shader, Stage stage) : s_(shader), stage_(stage) {}
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
        if (o.source != StorageSource::ConstantFloat) {
            unsupported("an ALU operand from a fetch constant");
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
    Id sample_texture(const TextureFetchInstruction &t, Id coordinates);
    Id swap_endian(Id word, Id endian);

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
    spirv::Module m_;
    std::vector<std::string> unsupported_;
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
    Id body_ = 0;
    bool ended_ = false; // the straight control flow has returned
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
    m_.decorate(memory_, spv::Decoration::NonWritable);
    memory_ptr_uint_ = m_.type_pointer(spv::StorageClass::StorageBuffer, uint_);

    if (stage_ != Stage::Harness) {
        // ndc_scale, ndc_offset, the image and sampler index of each fetch
        // slot, then flags (x: the alpha test, bit 3 on, bits 0-2 its
        // function; y: the position's formats; z: gamma targets; w: D24FS8
        // depth), values (x: the alpha test's reference, y and z: the point
        // diameter's bounds), the color targets' scales and the point
        // constants (renderer.cpp's System, point_list.geom)
        const Id system_members[] = {vec4_, vec4_, m_.type_array(uvec4_, 8, 16), uvec4_, vec4_, vec4_, vec4_};
        Id system_block = m_.type_struct(system_members);
        m_.decorate(system_block, spv::Decoration::Block);
        m_.member_decorate(system_block, 0, spv::Decoration::Offset, {0});
        m_.member_decorate(system_block, 1, spv::Decoration::Offset, {16});
        m_.member_decorate(system_block, 2, spv::Decoration::Offset, {32});
        m_.member_decorate(system_block, 3, spv::Decoration::Offset, {160});
        m_.member_decorate(system_block, 4, spv::Decoration::Offset, {176});
        m_.member_decorate(system_block, 5, spv::Decoration::Offset, {192});
        m_.member_decorate(system_block, 6, spv::Decoration::Offset, {208});
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
    store(depth_, zero4_);
    store(loop_count_, m_.const_null(m_.type_array(uint_, 4)));
    store(loop_address_, m_.const_null(m_.type_array(int_, 4)));
    store(call_stack_, m_.const_null(m_.type_array(uint_, 4)));
    store(call_depth_, u(0));
    store(tfetch_lod_, f(0));
    store(gradient_h_, zero4_);
    store(gradient_v_, zero4_);
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
        break;
    case StorageTarget::Depth: pointer = depth_; break;
    case StorageTarget::ExportAddress:
    case StorageTarget::ExportData: unsupported("memexport"); return;
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
    if (t.opcode != FetchOpcode::TextureFetch && t.opcode != FetchOpcode::GetTextureGradients) {
        unsupported(std::string("texture fetch operation ") + t.opcode_name);
        return;
    }
    if (!t.has_result()) return;
    bool gradients = t.opcode == FetchOpcode::GetTextureGradients;
    if (stage_ != Stage::Harness && !gradients && t.unnormalized_coordinates && t.dimension == FetchOpDimension::Cube)
        unsupported("unnormalized cube map coordinates");
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
        } else {
            result = sample_texture(t, coordinates);
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
Id Translator::sample_texture(const TextureFetchInstruction &t, Id coordinates) {
    const uint32_t slot = t.operands[1].index;
    const float epsilon = 1.5f / 1024.0f;
    const Id vec3 = m_.type_vector(float_, 3);
    auto add = [&](Id a, Id b) { return exact(Op::OpFAdd, float_, {a, b}); };
    auto mul = [&](Id a, Id b) { return exact(Op::OpFMul, float_, {a, b}); };
    auto div = [&](Id a, Id b) { return exact(Op::OpFDiv, float_, {a, b}); };

    // the slot's image (bits 0-13), the kind of image it is (bits 14-15: a
    // 2D array, 3D, cube) and its sampler (bits 16-31), from the system
    // constants
    Id entry = m_.op(Op::OpCompositeExtract, uint_,
                     {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_),
                                         {system_, u(2), u(slot >> 2)})),
                      slot & 3});
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

    // the coordinates, offset and normalized
    Id c[3] = {comp(coordinates, 0), one ? f(0) : comp(coordinates, 1), cube || volume ? comp(coordinates, 2) : f(0)};
    const float offsets[3] = {t.offset_x + epsilon, one ? 0.0f : t.offset_y + epsilon,
                              cube ? t.offset_z : volume ? t.offset_z + epsilon : 0.0f};
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

    // the sample: the computed LOD in pixel shaders, or with gradients from
    // registers (normalized if the coordinates are not); n gradient components
    bool computed = t.use_computed_lod && (stage_ == Stage::Pixel || t.use_register_gradients);
    auto sample = [&](Id sampled, Id coords, uint32_t n) {
        if (t.use_register_gradients) {
            Id gradients[2];
            for (uint32_t g = 0; g < 2; g++) {
                Id v = load(vec4_, g ? gradient_v_ : gradient_h_);
                Id parts[3];
                for (uint32_t k = 0; k < n; k++)
                    parts[k] = t.unnormalized_coordinates && size[k] ? div(comp(v, k), size[k]) : comp(v, k);
                gradients[g] = n == 3 ? m_.op(Op::OpCompositeConstruct, vec3, {parts[0], parts[1], parts[2]})
                                      : m_.op(Op::OpCompositeConstruct, vec2_, {parts[0], parts[1]});
            }
            return m_.op(Op::OpImageSampleExplicitLod, vec4_,
                         {sampled, coords, uint32_t(spv::ImageOperandsMask::Grad), gradients[0], gradients[1]});
        }
        if (t.use_register_lod || !computed) {
            Id lod = f(t.lod_bias);
            if (t.use_register_lod) lod = add(load(float_, tfetch_lod_), lod);
            return m_.op(Op::OpImageSampleExplicitLod, vec4_,
                         {sampled, coords, uint32_t(spv::ImageOperandsMask::Lod), lod});
        }
        if (t.lod_bias != 0)
            return m_.op(Op::OpImageSampleImplicitLod, vec4_,
                         {sampled, coords, uint32_t(spv::ImageOperandsMask::Bias), f(t.lod_bias)});
        return m_.op(Op::OpImageSampleImplicitLod, vec4_, {sampled, coords});
    };
    auto vec3_of = [&](Id x, Id y, Id z) { return m_.op(Op::OpCompositeConstruct, vec3, {x, y, z}); };

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
        return sample(sampled_image(2), coords, 3);
    }
    if (!volume) return sample(sampled_image(0), vec3_of(c[0], c[1], f(0)), 2);

    // 3D, or stacked; both sampled (the right one chosen after, so the
    // implicit LOD's derivatives stay in uniform control flow)
    Id sampled_3d = sample(sampled_image(1), vec3_of(c[0], c[1], z_3d), 3);
    // the filter between layers: the instruction's, or the fetch
    // constant's (word 4 bits 0 and 1, magnifying and minifying); with a
    // computed LOD and the two differing, chosen by the layer coordinate's
    // gradient as Xenia does
    bool mag_constant = t.vol_mag_filter == TextureFilter::UseFetchConst;
    bool min_constant = t.vol_min_filter == TextureFilter::UseFetchConst;
    bool mag_linear = t.vol_mag_filter == TextureFilter::Linear, min_linear = t.vol_min_filter == TextureFilter::Linear;
    Id word4 = constant_dword(3, slot * 6 + 4);
    auto word4_bit = [&](uint32_t bit) {
        return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {word4, u(1u << bit)}), u(0)});
    };
    Id linear = 0; // chosen at run time
    if (computed && (mag_constant || min_constant || mag_linear != min_linear)) {
        Id lod = mul(signed_bits(word4, 12, 10), f(1.0f / 32.0f));
        if (t.use_register_lod) lod = add(load(float_, tfetch_lod_), lod);
        if (t.lod_bias != 0) lod = add(lod, f(t.lod_bias));
        Id scale_h = glsl(GLSLstd450Exp2, float_, {add(lod, signed_bits(word4, 22, 5))});
        Id scale_v = glsl(GLSLstd450Exp2, float_, {add(lod, signed_bits(word4, 27, 5))});
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
    Id stacked = sample(sampled_image(0), vec3_of(c[0], c[1], layer_0), 2);
    if (linear || always_linear) {
        Id next = sample(sampled_image(0), vec3_of(c[0], c[1], m_.op(Op::OpFAdd, float_, {layer_0, f(1)})), 2);
        Id factor = glsl(GLSLstd450Fract, float_, {layer_coordinate});
        Id step = exact(Op::OpVectorTimesScalar, vec4_, {exact(Op::OpFSub, vec4_, {next, stacked}), factor});
        Id filtered = exact(Op::OpFAdd, vec4_, {stacked, step});
        stacked = linear ? select(vec4_, splat_bool(linear), filtered, stacked) : filtered;
    }
    return select(vec4_, splat_bool(data_is_3d), sampled_3d, stacked);
}

bool Translator::exec(const Exec &e) {
    auto instructions = [&] {
        for (const ExecSlot &slot : e.slots) {
            if (const auto *a = std::get_if<AluInstruction>(&slot.instruction)) alu(*a);
            else if (const auto *v = std::get_if<VertexFetchInstruction>(&slot.instruction)) vertex_fetch(*v);
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
            } else {
                return false; // nop, alloc, mark_vs_fetch_done
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
    Id vertex_index = global(int_, spv::StorageClass::Input, "vertex_index");
    m_.decorate(vertex_index, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::VertexIndex)});
    Id position = global(vec4_, spv::StorageClass::Output, "position");
    m_.decorate(position, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::Position)});
    Id outputs = global(m_.type_array(vec4_, 16), spv::StorageClass::Output, "interpolators");
    m_.decorate(outputs, spv::Decoration::Location, {0});
    // the point diameter for point lists' geometry shader (point_list.geom),
    // after the interpolators
    Id point_size = global(float_, spv::StorageClass::Output, "point_size");
    m_.decorate(point_size, spv::Decoration::Location, {16});
    bool writes_point_size = false;
    for (const ControlFlow &cf : s_.control_flow)
        if (const auto *e = std::get_if<Exec>(&cf.instruction))
            for (const ExecSlot &slot : e->slots)
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                    for (const Result *r : {&a->vector_and_constant_result, &a->scalar_result})
                        writes_point_size |=
                            r->target == StorageTarget::PointSizeEdgeFlagKillVertex && (r->used_write_mask() & 1);

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    for (uint32_t k = 0; k < harness::REGISTERS; k++)
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}), zero4_);
    // the vertex index in r0.x (B3 adds the index offset and endianness)
    store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(0)}),
          vec4_of(m_.op(Op::OpConvertSToF, float_, {load(int_, vertex_index)}), f(0), f(0), f(0)));
    reset_state();
    m_.op(Op::OpFunctionCall, void_, {body_});
    // The position's formats (PA_CL_VTE_CNTL, in the system flags' y), as
    // Xenia's CompleteVertexOrTessEvalShaderInMain: W given as 1/W unless bit
    // 0, XY divided by W with bit 1, Z with bit 2. Then to Vulkan's clip
    // space: xyz * ndc_scale + ndc_offset * w.
    Id ptr_u_vec4 = m_.type_pointer(spv::StorageClass::Uniform, vec4_);
    Id scale = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(0)}));
    Id offset = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(1)}));
    Id flags = m_.op(Op::OpCompositeExtract, uint_,
                     {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(3)})), 1});
    auto flag = [&](uint32_t bit) { return m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {flags, u(bit)}), u(0)}); };
    Id p = load(vec4_, position_);
    Id w = comp(p, 3);
    w = select(float_, flag(1), w, exact(Op::OpFDiv, float_, {f(1), w}));
    Id x = select(float_, flag(2), exact(Op::OpFMul, float_, {comp(p, 0), w}), comp(p, 0));
    Id y = select(float_, flag(2), exact(Op::OpFMul, float_, {comp(p, 1), w}), comp(p, 1));
    Id z = select(float_, flag(4), exact(Op::OpFMul, float_, {comp(p, 2), w}), comp(p, 2));
    Id xyzw = vec4_of(x, y, z, w);
    Id moved = exact(Op::OpFAdd, vec4_, {exact(Op::OpFMul, vec4_, {xyzw, scale}), exact(Op::OpVectorTimesScalar, vec4_, {offset, w})});
    store(position, vec4_of(comp(moved, 0), comp(moved, 1), comp(moved, 2), w));
    Id ptr_out_vec4 = m_.type_pointer(spv::StorageClass::Output, vec4_);
    for (uint32_t k = 0; k < 16; k++)
        store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))}),
              load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {interpolators_, i(int32_t(k))})));
    // oPts.x clamped to PA_SU_POINT_MINMAX (system values' y and z) as Xenia
    // does, like signed integers (-NaN and negatives to the minimum, NaN to
    // the maximum); -1 when the shader does not write it, for the constant
    // size
    if (writes_point_size) {
        Id values = load(vec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, vec4_),
                                      {system_, u(4)}));
        auto as_int = [&](Id x) { return m_.op(Op::OpBitcast, int_, {x}); };
        Id bits = glsl(GLSLstd450SMax, int_, {as_int(comp(values, 1)), as_int(comp(load(vec4_, point_size_), 0))});
        bits = glsl(GLSLstd450SMin, int_, {as_int(comp(values, 2)), bits});
        store(point_size, m_.op(Op::OpBitcast, float_, {bits}));
    } else {
        store(point_size, f(-1.0f));
    }
    m_.op_void(Op::OpReturn);
    m_.end_function();
    m_.entry_point(spv::ExecutionModel::Vertex, main, "main", interface_);
}

void Translator::main_pixel() {
    m_.capability(spv::Capability::DemoteToHelperInvocation);
    Id inputs = global(m_.type_array(vec4_, 16), spv::StorageClass::Input, "interpolators");
    m_.decorate(inputs, spv::Decoration::Location, {0});
    Id outputs = global(m_.type_array(vec4_, 4), spv::StorageClass::Output, "colors");
    m_.decorate(outputs, spv::Decoration::Location, {0});
    bool writes_depth = false;
    for (const ControlFlow &cf : s_.control_flow)
        if (const auto *e = std::get_if<Exec>(&cf.instruction))
            for (const ExecSlot &slot : e->slots)
                if (const auto *a = std::get_if<AluInstruction>(&slot.instruction))
                    writes_depth |= a->vector_and_constant_result.target == StorageTarget::Depth ||
                                    a->scalar_result.target == StorageTarget::Depth;
    Id frag_depth = 0;
    if (writes_depth) {
        frag_depth = global(float_, spv::StorageClass::Output, "depth");
        m_.decorate(frag_depth, spv::Decoration::BuiltIn, {uint32_t(spv::BuiltIn::FragDepth)});
    }

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    Id ptr_in_vec4 = m_.type_pointer(spv::StorageClass::Input, vec4_);
    for (uint32_t k = 0; k < harness::REGISTERS; k++) {
        Id value = k < 16 ? load(vec4_, m_.op(Op::OpAccessChain, ptr_in_vec4, {inputs, i(int32_t(k))})) : zero4_;
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}), value);
    }
    reset_state();
    m_.op(Op::OpFunctionCall, void_, {body_});
    if (frag_depth) {
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
    // reference, by the function in the system constants
    {
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
        when(m_.op(Op::OpLogicalAnd, bool_, {enabled, m_.op(Op::OpLogicalNot, bool_, {pass})}), [&] {
            m_.op_void(Op::OpDemoteToHelperInvocation);
            return false;
        });
    }
    // The colors out, as Xenia's host render target path writes them: times
    // the target's scale (its exponent bias, and the range of a 16-bit fixed
    // target kept as snorm; system member 5), after the alpha test, which
    // takes the alpha unscaled; then to the piecewise linear gamma for gamma
    // targets (system flags' z, a bit per target; xenos.cc's
    // LinearToPWLGamma).
    {
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
        for (uint32_t k = 0; k < 4; k++) {
            Id color = load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(int32_t(k))}));
            color = exact(Op::OpVectorTimesScalar, vec4_, {color, comp(scales, k)});
            Id gamma = m_.op(Op::OpINotEqual, bool_, {m_.op(Op::OpBitwiseAnd, uint_, {gamma_flags, u(1u << k)}), u(0)});
            Id r = comp(color, 0), g = comp(color, 1), b = comp(color, 2);
            color = vec4_of(select(float_, gamma, pwl(r), r), select(float_, gamma, pwl(g), g),
                            select(float_, gamma, pwl(b), b), comp(color, 3));
            store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))}), color);
        }
    }
    m_.op_void(Op::OpReturn);
    m_.end_function();
    m_.entry_point(spv::ExecutionModel::Fragment, main, "main", interface_);
    m_.execution_mode(main, spv::ExecutionMode::OriginUpperLeft);
    if (writes_depth) m_.execution_mode(main, spv::ExecutionMode::DepthReplacing);
}

Translation Translator::run() {
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
        }
    }
    if (s_.reads_past_end) unsupported("an exec past the end of the microcode");
    if (!ended_) m_.op_void(Op::OpReturn);
    m_.end_function();

    switch (stage_) {
    case Stage::Harness: main_harness(); break;
    case Stage::Vertex: main_vertex(); break;
    case Stage::Pixel: main_pixel(); break;
    }

    Translation t;
    t.unsupported = std::move(unsupported_);
    if (t.unsupported.empty()) t.spirv = m_.finish();
    return t;
}

} // namespace

Translation translate(const ParsedShader &shader, Stage stage) {
    if (stage == Stage::Vertex && shader.type != ShaderType::Vertex) return {{}, {"a pixel shader as a vertex shader"}};
    if (stage == Stage::Pixel && shader.type != ShaderType::Pixel) return {{}, {"a vertex shader as a pixel shader"}};
    return Translator(shader, stage).run();
}

} // namespace xf::shader
