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
// Version 1 runs the control flow straight: execs (unconditional, on a bool
// constant, on the predicate), allocs, nops. Loops, calls, jumps, memexport,
// and texture fetches other than plain 2D and 3D sampling are reported as
// unsupported.

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
    void exec(const Exec &e);
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
    Id images2d_ = 0, images3d_ = 0, samplers_ = 0, image2d_type_ = 0, image3d_type_ = 0, sampler_type_ = 0;
    Id registers_ = 0, predicate_ = 0, a0_ = 0, aL_ = 0, previous_scalar_ = 0, vfetch_address_ = 0, vfetch_bound_ = 0;
    Id killed_ = 0, interpolators_ = 0, position_ = 0, point_size_ = 0, colors_ = 0, depth_ = 0;
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
    m_.decorate(constants_, spv::Decoration::Binding, {stage_ == Stage::Harness ? 1u : 0u});
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
        // ndc_scale, ndc_offset, then the image and sampler index of each fetch slot
        const Id system_members[] = {vec4_, vec4_, m_.type_array(uvec4_, 8, 16)};
        Id system_block = m_.type_struct(system_members);
        m_.decorate(system_block, spv::Decoration::Block);
        m_.member_decorate(system_block, 0, spv::Decoration::Offset, {0});
        m_.member_decorate(system_block, 1, spv::Decoration::Offset, {16});
        m_.member_decorate(system_block, 2, spv::Decoration::Offset, {32});
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
    if (t.opcode != FetchOpcode::TextureFetch) {
        unsupported(std::string("texture fetch operation ") + t.opcode_name);
        return;
    }
    if (!t.has_result()) return;
    if (stage_ != Stage::Harness) {
        if (t.dimension == FetchOpDimension::Cube) unsupported("cube texture fetches");
        if (t.unnormalized_coordinates) unsupported("unnormalized texture coordinates");
        if (t.offset_x != 0 || t.offset_y != 0 || t.offset_z != 0) unsupported("texture fetch offsets");
        if (!t.use_computed_lod || t.use_register_lod || t.use_register_gradients)
            unsupported("texture fetches with a LOD or gradients from registers");
    }
    auto body = [&] {
        Id coordinates = operand(t.operands[0]);
        uint32_t dims = dimension_component_count(t.dimension);
        uint32_t slot = t.operands[1].index;
        Id result;
        if (stage_ == Stage::Harness) {
            // shader_harness.h's stand-in: (c.x + s, c.y - s, c.x * c.y, s)
            Id s = f(float(slot));
            Id cx = comp(coordinates, 0), cy = dims >= 2 ? comp(coordinates, 1) : f(0);
            result = vec4_of(exact(Op::OpFAdd, float_, {cx, s}), exact(Op::OpFSub, float_, {cy, s}),
                             exact(Op::OpFMul, float_, {cx, cy}), s);
        } else {
            // the image and sampler of this slot, from the system constants
            Id entry = m_.op(Op::OpCompositeExtract, uint_,
                             {load(uvec4_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::Uniform, uvec4_), {system_, u(2), u(slot >> 2)})), slot & 3});
            Id image_index = m_.op(Op::OpBitwiseAnd, uint_, {entry, u(0xFFFF)});
            Id sampler_index = m_.op(Op::OpShiftRightLogical, uint_, {entry, u(16)});
            bool three = t.dimension == FetchOpDimension::D3OrStacked;
            Id image_type = three ? image3d_type_ : image2d_type_;
            Id image = load(image_type, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::UniformConstant, image_type),
                                              {three ? images3d_ : images2d_, image_index}));
            Id sampler = load(sampler_type_, m_.op(Op::OpAccessChain, m_.type_pointer(spv::StorageClass::UniformConstant, sampler_type_),
                                                   {samplers_, sampler_index}));
            Id sampled = m_.op(Op::OpSampledImage, m_.type_sampled_image(image_type), {image, sampler});
            Id coords;
            if (three) {
                coords = m_.op(Op::OpVectorShuffle, m_.type_vector(float_, 3), {coordinates, coordinates, 0, 1, 2});
            } else {
                // a 1D texture is a row of a 2D one
                Id cy = dims >= 2 ? comp(coordinates, 1) : f(0.5f);
                coords = m_.op(Op::OpCompositeConstruct, vec2_, {comp(coordinates, 0), cy});
            }
            if (stage_ == Stage::Pixel) {
                if (t.lod_bias != 0) {
                    result = m_.op(Op::OpImageSampleImplicitLod, vec4_, {sampled, coords, uint32_t(spv::ImageOperandsMask::Bias), f(t.lod_bias)});
                } else {
                    result = m_.op(Op::OpImageSampleImplicitLod, vec4_, {sampled, coords});
                }
            } else {
                result = m_.op(Op::OpImageSampleExplicitLod, vec4_, {sampled, coords, uint32_t(spv::ImageOperandsMask::Lod), f(t.lod_bias)});
            }
        }
        store_result(t.result, result);
        return false;
    };
    if (t.is_predicated) when(predicate_is(t.predicate_condition), body);
    else body();
}

void Translator::exec(const Exec &e) {
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
    case ExecInstruction::Type::Unconditional:
        if (instructions()) ended_ = true;
        break;
    case ExecInstruction::Type::Conditional: {
        Id condition = bool_constant(e.cf.bool_constant);
        if (!e.cf.condition) condition = m_.op(Op::OpLogicalNot, bool_, {condition});
        when(condition, instructions);
        break;
    }
    case ExecInstruction::Type::Predicated: when(predicate_is(e.cf.condition), instructions); break;
    }
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

    Id main = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.label();
    for (uint32_t k = 0; k < harness::REGISTERS; k++)
        store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(int32_t(k))}), zero4_);
    // the vertex index in r0.x (B3 adds the index offset and endianness)
    store(m_.op(Op::OpAccessChain, ptr_private_vec4_, {registers_, i(0)}),
          vec4_of(m_.op(Op::OpConvertSToF, float_, {load(int_, vertex_index)}), f(0), f(0), f(0)));
    reset_state();
    m_.op(Op::OpFunctionCall, void_, {body_});
    // to Vulkan's clip space: xyz * ndc_scale + ndc_offset * w
    Id ptr_u_vec4 = m_.type_pointer(spv::StorageClass::Uniform, vec4_);
    Id scale = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(0)}));
    Id offset = load(vec4_, m_.op(Op::OpAccessChain, ptr_u_vec4, {system_, u(1)}));
    Id p = load(vec4_, position_);
    Id w = comp(p, 3);
    Id moved = exact(Op::OpFAdd, vec4_, {exact(Op::OpFMul, vec4_, {p, scale}), exact(Op::OpVectorTimesScalar, vec4_, {offset, w})});
    store(position, vec4_of(comp(moved, 0), comp(moved, 1), comp(moved, 2), w));
    Id ptr_out_vec4 = m_.type_pointer(spv::StorageClass::Output, vec4_);
    for (uint32_t k = 0; k < 16; k++)
        store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))}),
              load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {interpolators_, i(int32_t(k))})));
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
    Id ptr_out_vec4 = m_.type_pointer(spv::StorageClass::Output, vec4_);
    for (uint32_t k = 0; k < 4; k++)
        store(m_.op(Op::OpAccessChain, ptr_out_vec4, {outputs, i(int32_t(k))}),
              load(vec4_, m_.op(Op::OpAccessChain, ptr_private_vec4_, {colors_, i(int32_t(k))})));
    if (frag_depth) store(frag_depth, comp(load(vec4_, depth_), 0));
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
        image2d_type_ = m_.type_image(float_, spv::Dim::Dim2D, 0, false, false, 1, spv::ImageFormat::Unknown);
        image3d_type_ = m_.type_image(float_, spv::Dim::Dim3D, 0, false, false, 1, spv::ImageFormat::Unknown);
        sampler_type_ = m_.type_sampler();
        images2d_ = global(m_.type_runtime_array(image2d_type_, 0), spv::StorageClass::UniformConstant, "images2d");
        images3d_ = global(m_.type_runtime_array(image3d_type_, 0), spv::StorageClass::UniformConstant, "images3d");
        samplers_ = global(m_.type_runtime_array(sampler_type_, 0), spv::StorageClass::UniformConstant, "samplers");
        uint32_t binding = 0;
        for (Id v : {images2d_, images3d_, samplers_}) {
            m_.decorate(v, spv::Decoration::DescriptorSet, {1});
            m_.decorate(v, spv::Decoration::Binding, {binding++});
        }
    }

    // the body: the control flow, straight
    body_ = m_.begin_function(void_, m_.type_function(void_, {}));
    m_.name(body_, "xenos_main");
    m_.label();
    for (const ControlFlow &cf : s_.control_flow) {
        if (ended_) break;
        switch (cf.opcode) {
        case ControlFlowOpcode::Nop:
        case ControlFlowOpcode::MarkVsFetchDone:
        case ControlFlowOpcode::Alloc: break;
        case ControlFlowOpcode::LoopStart:
        case ControlFlowOpcode::LoopEnd: unsupported("loops"); break;
        case ControlFlowOpcode::CondCall:
        case ControlFlowOpcode::Return: unsupported("calls"); break;
        case ControlFlowOpcode::CondJmp: unsupported("jumps"); break;
        default: exec(std::get<Exec>(cf.instruction)); break;
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
