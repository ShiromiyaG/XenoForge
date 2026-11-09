// A parsed Xenos shader run on the CPU, one case of shader_harness.h at a
// time. Translated from Xenia's CPU interpreter
// (src/xenia/gpu/shader_interpreter.cc and .h): its operand fetching, ALU
// operations, predicate, address register and control flow. Where it and
// Xenia's SPIR-V translator disagree, this follows the translator, which is
// what the GPU runs (spirv_shader_translator.cc, spirv_shader_translator_alu.cc,
// spirv_shader_translator_fetch.cc): dot products summed from the first
// product, max4 with NMax over the components it reads, the vector and scalar
// kills returning 0 and killing, maxa's a0 as clamp(floor(w + 0.5), -256, 255)
// with NaN to -256, maxas and maxasf clamping a0 to 0-255, saturation as NClamp
// (NaN to 0), oDepth clamped to 0-1, loops and aL as the translator keeps them,
// and vertex fetches as the translator unpacks them (shifted packed fields,
// scales multiplied by their reciprocal, IEEE half floats). BSD; see
// LICENSE-xenia.

#include "xenoforge/shader_interpreter.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <variant>

// Every product and sum rounded on its own, as the translator's NoContraction.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace xf::shader {

using namespace ucode;

namespace {

uint32_t float_bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

float bits_float(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// Xenia's interpreter flushes each operand it reads: the hardware has no
// denormals. The sign stays.
float flush_denormal(float v) {
    uint32_t b = float_bits(v);
    if (!(b & 0x7F800000u)) b &= 0x80000000u;
    return bits_float(b);
}

float negate(float v) { return bits_float(float_bits(v) ^ 0x80000000u); }
float absolute(float v) { return bits_float(float_bits(v) & 0x7FFFFFFFu); }

// GLSL.std.450's NMax and NMin: the other operand when one is NaN.
float nmax(float a, float b) {
    if (std::isnan(a)) return b;
    if (std::isnan(b)) return a;
    return a < b ? b : a;
}
float nmin(float a, float b) {
    if (std::isnan(a)) return b;
    if (std::isnan(b)) return a;
    return b < a ? b : a;
}
float nclamp(float x, float lo, float hi) { return nmin(nmax(x, lo), hi); }

// Shader Model 3: +0 (or a denormal, flushed when read) times anything is +0,
// even infinity or NaN. A denormal product is flushed too, before anything
// adds to it: the hardware has none in the middle of an operation either (the
// GPU's flush-to-zero gives the same; a mad of 0.001 * FLT_MIN + FLT_MIN is
// FLT_MIN on both).
float legacy_mul(float a, float b) {
    float product = flush_denormal(a * b);
    return nmin(absolute(a), absolute(b)) == 0.0f ? 0.0f : product;
}

// The float clamped into 0-255 or -256-255 is whole, so the conversion is exact.
int32_t to_address(float clamped) { return int32_t(clamped); }

uint32_t gpu_swap(uint32_t v, uint32_t endian) {
    switch (endian & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); // 8 in 16
    case 2: return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); // 8 in 32
    case 3: return (v >> 16) | (v << 16); // 16 in 32
    default: return v;
    }
}

// GLSL.std.450's UnpackHalf2x16: IEEE half floats, with infinities, NaNs and
// denormals (the translator's choice, not the Xbox 360's extended range).
float half_to_float(uint32_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0x1F) return bits_float(sign | 0x7F800000u | (mantissa << 13));
    if (exponent == 0) {
        if (!mantissa) return bits_float(sign);
        float value = std::ldexp(float(mantissa), -24);
        return sign ? -value : value;
    }
    return bits_float(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

uint32_t vertex_format_components(uint32_t format) {
    switch (format) {
    case 6:  // 8_8_8_8
    case 7:  // 2_10_10_10
    case 26: // 16_16_16_16
    case 32: // 16_16_16_16_FLOAT
    case 35: // 32_32_32_32
    case 38: // 32_32_32_32_FLOAT
        return 4;
    case 16: // 10_11_11
    case 17: // 11_11_10
    case 57: // 32_32_32_FLOAT
        return 3;
    case 25: // 16_16
    case 31: // 16_16_FLOAT
    case 34: // 32_32
    case 37: // 32_32_FLOAT
        return 2;
    case 33: // 32
    case 36: // 32_FLOAT
        return 1;
    default: return 0;
    }
}

class Machine {
public:
    Machine(const ParsedShader &shader, const harness::Constants &k, std::span<const uint8_t> memory,
            harness::CaseOutput &out, Environment *environment = nullptr, bool pixel = false)
        : shader_(shader), k_(k), memory_(memory), out_(out), env_(environment), pixel_(pixel) {}

    float r[harness::REGISTERS][4] = {};
    float ps = 0.0f;
    bool p = false;
    int32_t a0 = 0;
    bool killed = false;
    std::string unsupported;
    uint32_t colors_written = 0; // oC# written on the shader's path, a bit each

    bool run() { return steps() == Invocation::Status::Done; }

    // Runs from where it stands: to its end, a failure, or (a pixel shader's
    // with an environment) a fetch that waits for its quad's coordinates.
    Invocation::Status steps() {
        const auto &flow = shader_.control_flow;
        for (; pc_ < flow.size(); steps_++) {
            if (steps_ > (1u << 20)) {
                fail("the control flow does not end");
                return Invocation::Status::Failed;
            }
            uint32_t pc = pc_, next = pc + 1;
            bool end = false;
            std::visit(
                [&](const auto &cf) {
                    using T = std::decay_t<decltype(cf)>;
                    if constexpr (std::is_same_v<T, Exec>) end = exec(cf);
                    else if constexpr (std::is_same_v<T, LoopStartInstruction>) loop_start(cf, next);
                    else if constexpr (std::is_same_v<T, LoopEndInstruction>) loop_end(cf, next);
                    else if constexpr (std::is_same_v<T, CallInstruction>) call(cf, pc, next);
                    else if constexpr (std::is_same_v<T, ReturnInstruction>) {
                        if (call_depth_) next = call_stack_[--call_depth_];
                    } else if constexpr (std::is_same_v<T, JumpInstruction>) {
                        if (taken(cf.type == JumpInstruction::Type::Unconditional,
                                  cf.type == JumpInstruction::Type::Predicated, cf.bool_constant, cf.condition))
                            next = cf.target;
                    } else if constexpr (std::is_same_v<T, AllocInstruction>) {
                        alloc(cf);
                    }
                    // nop and mark_vs_fetch_done change nothing here
                },
                flow[pc].instruction);
            if (!unsupported.empty()) return Invocation::Status::Failed;
            if (waiting_) return Invocation::Status::Waiting;
            if (end) break;
            pc_ = next;
        }
        pc_ = uint32_t(flow.size());
        // the export under way at the end
        export_now();
        return Invocation::Status::Done; // ended, or ran off the end of the control flow
    }

    uint64_t waiting_at() const { return uint64_t(pc_) << 32 | resume_slot_; }
    const float *waiting() const { return waiting_coordinates_; }
    void resume(const float quad[4][4], uint32_t lane) {
        std::memcpy(quad_, quad, sizeof(quad_));
        lane_ = lane;
        have_quad_ = true;
        waiting_ = false;
    }

private:
    bool fail(const char *what) {
        if (unsupported.empty()) unsupported = what;
        return false;
    }

    bool bool_constant(uint32_t index) const { return (k_.bool_constants[(index >> 5) & 7] >> (index & 31)) & 1; }

    bool taken(bool unconditional, bool predicated, uint32_t bool_index, bool condition) const {
        if (unconditional) return true;
        if (predicated) return p == condition;
        return bool_constant(bool_index) == condition;
    }

    // ---- control flow ----

    // Whether the shader ends here: an exec end that ran.
    bool exec(const Exec &e) {
        size_t first = 0;
        if (resume_slot_ != NOT_WAITING) {
            // back at the fetch it waited at, its condition already met
            first = resume_slot_;
            resume_slot_ = NOT_WAITING;
        } else {
            bool run = true;
            if (e.cf.type == ExecInstruction::Type::Conditional)
                run = bool_constant(e.cf.bool_constant) == e.cf.condition;
            else if (e.cf.type == ExecInstruction::Type::Predicated)
                run = p == e.cf.condition;
            if (!run) return false;
        }
        for (size_t i = first; i < e.slots.size(); i++) {
            std::visit(
                [&](const auto &instruction) {
                    using T = std::decay_t<decltype(instruction)>;
                    if constexpr (std::is_same_v<T, AluInstruction>) alu(instruction);
                    else if constexpr (std::is_same_v<T, VertexFetchInstruction>) vertex_fetch(instruction);
                    else texture_fetch(instruction);
                },
                e.slots[i].instruction);
            if (!unsupported.empty()) return false;
            if (waiting_) {
                resume_slot_ = uint32_t(i);
                return false;
            }
        }
        return e.cf.is_end;
    }

    // An alloc ends the export under way; an alloc of exports starts with
    // eA invalid (Xenia's ProcessAllocInstruction, as the translator).
    void alloc(const AllocInstruction &a) {
        if (!env_) return;
        export_now();
        if (a.type == AllocType::Memory) std::memset(export_address_, 0, sizeof(export_address_));
    }

    // The eM# written since the last export, to the environment; none once
    // a pixel was killed (a helper invocation's stores are dropped).
    void export_now() {
        if (!env_ || !export_written_) return;
        if (exports_allowed_) env_->memory_export(export_address_, export_data_, export_written_);
        export_written_ = 0;
        std::memset(export_data_, 0, sizeof(export_data_));
    }

    // The translator's loop: a four-deep stack of counts and of aL, the top at 0.
    void loop_start(const LoopStartInstruction &l, uint32_t &next) {
        uint32_t constant = k_.loop_constants[l.loop_constant & 31];
        uint32_t count = constant & 0xFF;
        int32_t old_al = loop_address_[0];
        for (int i = 3; i > 0; i--) {
            loop_count_[i] = loop_count_[i - 1];
            loop_address_[i] = loop_address_[i - 1];
        }
        loop_count_[0] = count;
        loop_address_[0] = l.is_repeat ? old_al : int32_t((constant >> 8) & 0xFF);
        if (!count) next = l.skip_address;
    }

    void loop_end(const LoopEndInstruction &l, uint32_t &next) {
        uint32_t count = loop_count_[0] - 1;
        bool done = count == 0 || (l.is_predicated_break && p == l.predicate_condition);
        if (!done) {
            loop_count_[0] = count;
            int32_t step = int32_t(int8_t(uint8_t((k_.loop_constants[l.loop_constant & 31] >> 16) & 0xFF)));
            loop_address_[0] = int32_t(uint32_t(loop_address_[0]) + uint32_t(step));
            next = l.body_address;
            return;
        }
        for (int i = 0; i < 3; i++) {
            loop_count_[i] = loop_count_[i + 1];
            loop_address_[i] = loop_address_[i + 1];
        }
        loop_count_[3] = 0;
        loop_address_[3] = 0;
    }

    // Calls as Xenia's interpreter has them (its translator has none): a
    // four-deep return stack, a fifth call skipped.
    void call(const CallInstruction &c, uint32_t pc, uint32_t &next) {
        if (call_depth_ >= 4) return;
        if (!taken(c.type == CallInstruction::Type::Unconditional, c.type == CallInstruction::Type::Predicated,
                   c.bool_constant, c.condition))
            return;
        call_stack_[call_depth_++] = pc + 1;
        next = c.target;
    }

    // ---- operands ----

    uint32_t register_index(uint32_t index, StorageAddressing addressing) const {
        int32_t i = int32_t(index);
        if (addressing == StorageAddressing::LoopRelative) i += loop_address_[0];
        else if (addressing == StorageAddressing::AddressRegisterRelative) i += a0;
        return uint32_t(i) & (harness::REGISTERS - 1);
    }

    // The operand's register or constant as stored, before swizzle and modifiers.
    void load(const Operand &o, float raw[4]) const {
        if (o.source == StorageSource::Register) {
            std::memcpy(raw, r[register_index(o.index, o.addressing)], 16);
            return;
        }
        int64_t i = int64_t(o.index);
        if (o.addressing == StorageAddressing::AddressRegisterRelative) i += a0;
        else if (o.addressing == StorageAddressing::LoopRelative) i += loop_address_[0];
        if (o.source != StorageSource::ConstantFloat || i < 0 || i > 255) {
            std::memset(raw, 0, 16);
            return;
        }
        std::memcpy(raw, k_.float_constants[i], 16);
    }

    float modify(const Operand &o, float v, bool flush) const {
        if (flush) v = flush_denormal(v);
        if (o.absolute) v = absolute(v);
        if (o.negated) v = negate(v);
        return v;
    }

    // The operand's component i (by its swizzle, with its modifiers).
    float component(const Operand &o, uint32_t i, bool flush = true) const {
        float raw[4];
        load(o, raw);
        return modify(o, raw[uint32_t(o.component(i)) & 3], flush);
    }

    void operand4(const Operand &o, float v[4]) const {
        float raw[4];
        load(o, raw);
        for (uint32_t i = 0; i < 4; i++) v[i] = modify(o, raw[uint32_t(o.component(i)) & 3], true);
    }

    // ---- results ----

    // Writes the result's components from value (a vector by result
    // component, or a scalar for every one), its constants as 0 and 1.
    void store(const Result &result, const float *value, bool scalar) {
        uint32_t mask = result.used_write_mask();
        if (!mask) return;
        float *target = nullptr;
        switch (result.target) {
        case StorageTarget::None: return;
        case StorageTarget::Register: target = r[register_index(result.index, result.addressing)]; break;
        case StorageTarget::Interpolator:
            if (result.index >= harness::INTERPOLATORS) return;
            target = out_.interpolators[result.index];
            break;
        case StorageTarget::Position: target = out_.position; break;
        case StorageTarget::PointSizeEdgeFlagKillVertex: target = out_.point_size; break;
        case StorageTarget::Color:
            if (result.index >= 4) return;
            target = out_.colors[result.index];
            colors_written |= 1u << result.index;
            break;
        case StorageTarget::Depth: target = out_.depth; break;
        case StorageTarget::ExportAddress:
            if (!env_) {
                fail("memory export (eA, eM)");
                return;
            }
            target = export_address_;
            break;
        case StorageTarget::ExportData:
            if (!env_) {
                fail("memory export (eA, eM)");
                return;
            }
            if (result.index >= 5) return;
            target = export_data_[result.index];
            export_written_ |= 1u << result.index;
            break;
        }
        for (uint32_t i = 0; i < 4; i++) {
            if (!(mask & (1u << i))) continue;
            Swizzle c = result.components[i];
            float x;
            if (c == Swizzle::Zero) {
                x = 0.0f;
            } else if (c == Swizzle::One) {
                x = 1.0f;
            } else {
                x = scalar ? value[0] : value[uint32_t(c)];
                if (result.clamped) x = nclamp(x, 0.0f, 1.0f);
                // depth outside 0-1 clamped, as the translator does when not saturated
                if (result.target == StorageTarget::Depth && !result.clamped) x = nclamp(x, 0.0f, 1.0f);
            }
            target[i] = x;
        }
    }

    // ---- ALU ----

    void alu(const AluInstruction &in) {
        if (in.is_nop()) return;
        if (in.is_predicated && p != in.predicate_condition) return;

        // the vector operation first: its maxa and setp change a0 and the
        // predicate before the scalar operation reads its operands
        float vector[4] = {};
        if (in.vector_and_constant_result.used_result_components() || vector_opcode_info(in.vector_opcode).changed_state)
            vector_operation(in, vector);

        float scalar;
        if (in.scalar_opcode == AluScalarOpcode::RetainPrev) {
            scalar = ps;
        } else {
            scalar = scalar_operation(in);
            ps = scalar;
        }
        store(in.vector_and_constant_result, vector, false);
        store(in.scalar_result, &scalar, true);
        // a kill: the exports under way done first, none after (Xenia's KillPixel)
        if (killed && env_ && pixel_ && exports_allowed_) {
            export_now();
            exports_allowed_ = false;
        }
    }

    void vector_operation(const AluInstruction &in, float res[4]) {
        using V = AluVectorOpcode;
        const Operand *o = in.vector_operands;
        float a[4] = {}, b[4] = {}, c[4] = {};
        uint32_t count = in.vector_opcode == V::Cube ? 1 : in.vector_operand_count;
        if (count > 0) operand4(o[0], a);
        if (count > 1) operand4(o[1], b);
        if (count > 2) operand4(o[2], c);
        uint32_t same = count > 1 ? o[0].identical_components(o[1]) : 0;
        auto all = [&](float v) { res[0] = res[1] = res[2] = res[3] = v; };
        auto product = [&](uint32_t i) { return (same & (1u << i)) ? flush_denormal(a[i] * b[i]) : legacy_mul(a[i], b[i]); };

        switch (in.vector_opcode) {
        case V::Add:
            for (int i = 0; i < 4; i++) res[i] = a[i] + b[i];
            break;
        case V::Mul:
            for (uint32_t i = 0; i < 4; i++) res[i] = product(i);
            break;
        case V::Mad:
            for (uint32_t i = 0; i < 4; i++) {
                float m = product(i);
                res[i] = m + c[i];
            }
            break;
        case V::Max:
        case V::MaxA:
            if (in.vector_opcode == V::MaxA) a0 = to_address(nclamp(std::floor(a[3] + 0.5f), -256.0f, 255.0f));
            for (uint32_t i = 0; i < 4; i++) res[i] = (same & (1u << i)) ? a[i] : (a[i] >= b[i] ? a[i] : b[i]);
            break;
        case V::Min:
            for (uint32_t i = 0; i < 4; i++) res[i] = (same & (1u << i)) ? a[i] : (a[i] < b[i] ? a[i] : b[i]);
            break;
        case V::Seq:
            for (int i = 0; i < 4; i++) res[i] = a[i] == b[i] ? 1.0f : 0.0f;
            break;
        case V::Sgt:
            for (int i = 0; i < 4; i++) res[i] = a[i] > b[i] ? 1.0f : 0.0f;
            break;
        case V::Sge:
            for (int i = 0; i < 4; i++) res[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            break;
        case V::Sne:
            for (int i = 0; i < 4; i++) res[i] = a[i] != b[i] ? 1.0f : 0.0f; // unordered: NaN is unequal
            break;
        case V::Frc:
            for (int i = 0; i < 4; i++) res[i] = a[i] - std::floor(a[i]);
            break;
        case V::Trunc:
            for (int i = 0; i < 4; i++) res[i] = std::trunc(a[i]);
            break;
        case V::Floor:
            for (int i = 0; i < 4; i++) res[i] = std::floor(a[i]);
            break;
        case V::CndEq:
            for (int i = 0; i < 4; i++) res[i] = a[i] == 0.0f ? b[i] : c[i];
            break;
        case V::CndGe:
            for (int i = 0; i < 4; i++) res[i] = a[i] >= 0.0f ? b[i] : c[i];
            break;
        case V::CndGt:
            for (int i = 0; i < 4; i++) res[i] = a[i] > 0.0f ? b[i] : c[i];
            break;
        case V::Dp4:
        case V::Dp3:
        case V::Dp2Add: {
            uint32_t n = in.vector_opcode == V::Dp4 ? 4 : in.vector_opcode == V::Dp3 ? 3 : 2;
            float sum = product(0);
            for (uint32_t i = 1; i < n; i++) sum = flush_denormal(sum + product(i));
            if (in.vector_opcode == V::Dp2Add) sum = sum + c[0];
            all(sum);
            break;
        }
        case V::Cube: {
            // the operand is .z_xy: z in 0, x in 2, y in 3
            float x = a[2], y = a[3], z = a[0];
            float ax = absolute(x), ay = absolute(y), az = absolute(z);
            float tc, sc, ma, id;
            if (az >= ax && az >= ay) {
                bool neg = z < 0.0f;
                tc = negate(y);
                sc = neg ? negate(x) : x;
                ma = z;
                id = neg ? 5.0f : 4.0f;
            } else if (ay >= ax) {
                bool neg = y < 0.0f;
                tc = neg ? negate(z) : z;
                sc = x;
                ma = y;
                id = neg ? 3.0f : 2.0f;
            } else {
                bool neg = x < 0.0f;
                tc = negate(y);
                sc = neg ? z : negate(z);
                ma = x;
                id = neg ? 1.0f : 0.0f;
            }
            res[0] = tc;
            res[1] = sc;
            res[2] = 2.0f * ma;
            res[3] = id;
            break;
        }
        case V::Max4: {
            // NMax over the register's components the swizzle reads, in order
            float raw[4];
            load(o[0], raw);
            uint32_t read = 0;
            for (uint32_t i = 0; i < 4; i++) read |= 1u << (uint32_t(o[0].component(i)) & 3);
            bool first = true;
            float m = 0.0f;
            for (uint32_t i = 0; i < 4; i++) {
                if (!(read & (1u << i))) continue;
                float v = modify(o[0], raw[i], true);
                m = first ? v : nmax(m, v);
                first = false;
            }
            all(m);
            break;
        }
        case V::SetpEqPush:
        case V::SetpNePush:
        case V::SetpGtPush:
        case V::SetpGePush: {
            auto test = [&](float v) {
                switch (in.vector_opcode) {
                case V::SetpEqPush: return v == 0.0f;
                case V::SetpNePush: return v != 0.0f;
                case V::SetpGtPush: return v > 0.0f;
                default: return v >= 0.0f;
                }
            };
            p = a[3] == 0.0f && test(b[3]);
            bool zero = a[0] == 0.0f && test(b[0]);
            all((zero ? -1.0f : a[0]) + 1.0f);
            break;
        }
        case V::KillEq:
        case V::KillGt:
        case V::KillGe:
        case V::KillNe: {
            bool any = false;
            for (int i = 0; i < 4; i++) {
                switch (in.vector_opcode) {
                case V::KillEq: any |= a[i] == b[i]; break;
                case V::KillGt: any |= a[i] > b[i]; break;
                case V::KillGe: any |= a[i] >= b[i]; break;
                default: any |= a[i] != b[i]; break;
                }
            }
            killed |= any;
            all(0.0f);
            break;
        }
        case V::Dst:
            res[0] = 1.0f;
            res[1] = (same & 0b0010) ? flush_denormal(a[1] * b[1]) : legacy_mul(a[1], b[1]);
            res[2] = a[2];
            res[3] = b[3];
            break;
        default: fail("an unknown vector operation"); break;
        }
    }

    float scalar_operation(const AluInstruction &in) {
        using S = AluScalarOpcode;
        const Operand &o = in.scalar_operands[0];
        uint32_t operands = scalar_opcode_info(in.scalar_opcode).operand_count;
        // one operand: a its first component, b its second (when it has two)
        float a = 0.0f, b = 0.0f;
        bool a_is_b = false;
        if (operands == 1) {
            a = component(o, 0);
            b = component(o, 1);
            a_is_b = o.component(0) == o.component(1);
        }
        // two operands: a constant's component and a register's
        float x = 0.0f, y = 0.0f;
        bool x_is_y = false;
        if (operands == 2) {
            x = component(in.scalar_operands[0], 0);
            y = component(in.scalar_operands[1], 0);
            x_is_y = (in.scalar_operands[0].identical_components(in.scalar_operands[1]) & 1) != 0;
        }
        auto setp = [&](bool predicate) {
            p = predicate;
            return predicate ? 0.0f : 1.0f;
        };

        switch (in.scalar_opcode) {
        case S::Adds: return a + b;
        case S::AddsPrev: return a + ps;
        case S::Subs: return a - b;
        case S::SubsPrev: return a - ps;
        case S::Muls: return a_is_b ? flush_denormal(a * b) : legacy_mul(a, b);
        case S::MulsPrev: return legacy_mul(a, ps);
        case S::MulsPrev2: {
            bool multiply = !(ps == -FLT_MAX) && negate(absolute(ps)) >= -FLT_MAX &&
                            negate(absolute(b)) >= -FLT_MAX && b > 0.0f;
            if (!multiply) return -FLT_MAX;
            return legacy_mul(o.component(0) != o.component(1) ? a : b, ps);
        }
        case S::Maxs:
        case S::MaxAs:
        case S::MaxAsf:
            if (in.scalar_opcode == S::MaxAs) a0 = to_address(nclamp(std::floor(a + 0.5f), 0.0f, 255.0f));
            else if (in.scalar_opcode == S::MaxAsf) a0 = to_address(nclamp(std::floor(a), 0.0f, 255.0f));
            return a_is_b ? a : (a >= b ? a : b);
        case S::Mins: return a_is_b ? a : (a < b ? a : b);
        case S::Seqs: return a == 0.0f ? 1.0f : 0.0f;
        case S::Sgts: return a > 0.0f ? 1.0f : 0.0f;
        case S::Sges: return a >= 0.0f ? 1.0f : 0.0f;
        case S::Snes: return a != 0.0f ? 1.0f : 0.0f;
        case S::Frcs: return a - std::floor(a);
        case S::Truncs: return std::trunc(a);
        case S::Floors: return std::floor(a);
        case S::Exp: return reduce_precision(std::exp2(a), 21);
        case S::Log: return reduce_precision(std::log2(a), 21);
        case S::Logc: {
            float v = reduce_precision(std::log2(a), 21);
            return v == -INFINITY ? -FLT_MAX : v;
        }
        case S::Rcp: return reduce_precision(1.0f / a, 21);
        case S::Rcpc:
        case S::Rsqc: {
            float v = reduce_precision(in.scalar_opcode == S::Rcpc ? 1.0f / a : 1.0f / std::sqrt(a), 21);
            if (v == -INFINITY) return -FLT_MAX;
            if (v == INFINITY) return FLT_MAX;
            return v;
        }
        case S::Rcpf:
        case S::Rsqf: {
            float v = reduce_precision(in.scalar_opcode == S::Rcpf ? 1.0f / a : 1.0f / std::sqrt(a), 21);
            if (v == INFINITY) return 0.0f;
            if (v == -INFINITY) return -0.0f;
            return v;
        }
        case S::Rsq: return reduce_precision(1.0f / std::sqrt(a), 21);
        case S::Sqrt: return reduce_precision(std::sqrt(a), 21);
        case S::SetpEq: return setp(a == 0.0f);
        case S::SetpNe: return setp(a != 0.0f);
        case S::SetpGt: return setp(a > 0.0f);
        case S::SetpGe: return setp(a >= 0.0f);
        case S::SetpInv:
            p = a == 1.0f;
            return p ? 0.0f : (a == 0.0f ? 1.0f : a);
        case S::SetpPop: {
            float t = a - 1.0f;
            p = t <= 0.0f;
            return p ? 0.0f : t;
        }
        case S::SetpClr: p = false; return FLT_MAX;
        case S::SetpRstr:
            p = a == 0.0f;
            return p ? 0.0f : a;
        case S::KillsEq: killed |= a == 0.0f; return 0.0f;
        case S::KillsGt: killed |= a > 0.0f; return 0.0f;
        case S::KillsGe: killed |= a >= 0.0f; return 0.0f;
        case S::KillsNe: killed |= a != 0.0f; return 0.0f;
        case S::KillsOne: killed |= a == 1.0f; return 0.0f;
        case S::Mulsc0:
        case S::Mulsc1: return x_is_y ? flush_denormal(x * y) : legacy_mul(x, y);
        case S::Addsc0:
        case S::Addsc1: return x + y;
        case S::Subsc0:
        case S::Subsc1: return x - y;
        case S::Sin: return std::sin(a);
        case S::Cos: return std::cos(a);
        default: fail("an unknown scalar operation (opcode 41 or above 50)"); return 0.0f;
        }
    }

    // ---- fetches ----

    // Writes a fetch's value (by result component) through its destination swizzle.
    void store_fetch(const Result &result, const float value[4]) { store(result, value, false); }

    uint32_t memory_word(uint32_t address) const {
        uint64_t at = uint64_t(address) * 4;
        if (at + 4 > memory_.size()) return 0;
        const uint8_t *m = memory_.data() + at;
        return uint32_t(m[0]) | uint32_t(m[1]) << 8 | uint32_t(m[2]) << 16 | uint32_t(m[3]) << 24;
    }

    void vertex_fetch(const VertexFetchInstruction &f) {
        if (f.is_predicated && p != f.predicate_condition) return;
        uint32_t constant = f.operands[1].index % 96;
        uint32_t w0 = k_.fetch_constants[constant * 2], w1 = k_.fetch_constants[constant * 2 + 1];

        // the address in dwords: the base, plus the index times the stride
        if (!f.is_mini_fetch) {
            uint32_t base = (w0 >> 2) & 0x07FFFFFF;
            vfetch_end_ = base + ((w1 >> 2) & 0xFFFFFF);
            uint32_t address = base;
            if (f.stride) {
                float index = component(f.operands[0], 0, false);
                if (f.is_index_rounded) index = index + 0.5f;
                index = std::floor(index);
                int32_t i = std::isnan(index)          ? 0
                            : index >= 2147483648.0f   ? INT32_MAX
                            : index < -2147483648.0f   ? INT32_MIN
                                                       : int32_t(index);
                address = base + uint32_t(i) * f.stride;
            }
            vfetch_address_ = address;
        }

        uint32_t components = vertex_format_components(f.data_format);
        if (!components) {
            fail("a vertex format the interpreter does not know");
            return;
        }
        float value[4] = {};
        if (f.result.used_result_components()) {
            // words at or past the buffer's end read as 0
            uint32_t words[4];
            for (uint32_t w = 0; w < 4; w++) {
                uint32_t address = vfetch_address_ + uint32_t(f.offset) + w;
                words[w] = gpu_swap(address < vfetch_end_ ? memory_word(address) : 0, w1 & 3);
            }
            unpack(f, words, value);
            if (f.exp_adjust) {
                float scale = std::ldexp(1.0f, f.exp_adjust);
                for (uint32_t i = 0; i < components; i++) value[i] = value[i] * scale;
            }
        }
        store_fetch(f.result, value);
    }

    void unpack(const VertexFetchInstruction &f, const uint32_t words[4], float v[4]) const {
        struct Packed {
            uint32_t count, widths[4], offsets[4], words[4];
        };
        Packed packed{};
        switch (f.data_format) {
        case 6: packed = {4, {8, 8, 8, 8}, {0, 8, 16, 24}, {0, 0, 0, 0}}; break;
        case 7: packed = {4, {10, 10, 10, 2}, {0, 10, 20, 30}, {0, 0, 0, 0}}; break;
        case 16: packed = {3, {11, 11, 10, 0}, {0, 11, 22, 0}, {0, 0, 0, 0}}; break;
        case 17: packed = {3, {10, 11, 11, 0}, {0, 10, 21, 0}, {0, 0, 0, 0}}; break;
        case 25: packed = {2, {16, 16, 0, 0}, {0, 16, 0, 0}, {0, 0, 0, 0}}; break;
        case 26: packed = {4, {16, 16, 16, 16}, {0, 16, 0, 16}, {0, 0, 1, 1}}; break;
        case 31: // 16_16_FLOAT
        case 32: // 16_16_16_16_FLOAT
            for (uint32_t i = 0; i < vertex_format_components(f.data_format); i++)
                v[i] = half_to_float((words[i >> 1] >> (16 * (i & 1))) & 0xFFFF);
            return;
        case 33:
        case 34:
        case 35: { // 32, 32_32, 32_32_32_32
            for (uint32_t i = 0; i < vertex_format_components(f.data_format); i++) {
                float value = f.is_signed ? float(int32_t(words[i])) : float(words[i]);
                if (!f.is_integer) {
                    if (!f.is_signed) {
                        value = value * (1.0f / 4294967295.0f);
                    } else if (f.signed_rf_mode == SignedRepeatingFractionMode::NoZero) {
                        value = value * (1.0f / 2147483647.5f);
                        value = value + 0.5f / 2147483647.5f;
                    } else {
                        value = value * (1.0f / 2147483647.0f);
                    }
                }
                v[i] = value;
            }
            return;
        }
        default: // 32_FLOAT, 32_32_FLOAT, 32_32_32_32_FLOAT, 32_32_32_FLOAT
            for (uint32_t i = 0; i < vertex_format_components(f.data_format); i++) v[i] = bits_float(words[i]);
            return;
        }

        for (uint32_t i = 0; i < packed.count; i++) {
            uint32_t word = words[packed.words[i]], width = packed.widths[i], offset = packed.offsets[i];
            float value;
            if (f.is_signed) {
                int32_t field = int32_t(word << (32 - offset - width)) >> (32 - width);
                value = float(field);
            } else {
                value = float((word >> offset) & ((1u << width) - 1));
            }
            if (!f.is_integer) {
                float scale_inverse;
                if (f.is_signed) {
                    scale_inverse = float((1u << (width - 1)) - 1);
                    if (f.signed_rf_mode == SignedRepeatingFractionMode::NoZero) scale_inverse += 0.5f;
                } else {
                    scale_inverse = float((1u << width) - 1);
                }
                float scale = 1.0f / scale_inverse;
                value = value * scale;
                if (f.is_signed) {
                    if (f.signed_rf_mode == SignedRepeatingFractionMode::NoZero) value = value + 0.5f * scale;
                    else value = value < -1.0f ? -1.0f : value; // FMax with -1, the value never NaN
                }
            }
            v[i] = value;
        }
    }

    // The harness's stand-in for sampling: (c.x + s, c.y - s, c.x * c.y, s).
    // shader_harness.h's stand-ins: sampling and derivatives are the GPU's
    void texture_fetch(const TextureFetchInstruction &f) {
        if (f.is_predicated && p != f.predicate_condition) return;
        if (env_ && f.opcode != FetchOpcode::SetTextureLod && f.opcode != FetchOpcode::SetTextureGradientsHorz &&
            f.opcode != FetchOpcode::SetTextureGradientsVert) {
            environment_fetch(f);
            return;
        }
        switch (f.opcode) {
        case FetchOpcode::SetTextureLod: tfetch_lod_ = component(f.operands[0], 0, false); return;
        case FetchOpcode::SetTextureGradientsHorz:
        case FetchOpcode::SetTextureGradientsVert: {
            float *g = f.opcode == FetchOpcode::SetTextureGradientsHorz ? gradient_h_ : gradient_v_;
            for (uint32_t i = 0; i < 3; i++) g[i] = component(f.operands[0], i, false);
            return;
        }
        case FetchOpcode::GetTextureGradients: {
            float x = component(f.operands[0], 0, false), y = component(f.operands[0], 1, false);
            float value[4] = {x * 0.5f, x * -0.5f, y * 0.5f, y * -0.5f};
            if (f.has_result()) store_fetch(f.result, value);
            return;
        }
        case FetchOpcode::TextureFetch: break;
        default: fail("a texture operation other than tfetch, getGradients and the LOD and gradient setters"); return;
        }
        float c[3] = {};
        uint32_t n = dimension_component_count(f.dimension);
        for (uint32_t i = 0; i < n; i++) c[i] = component(f.operands[0], i, false);
        float s = float(f.operands[1].index);
        float z = c[0] * c[1], w = s;
        if (f.use_register_gradients) z = (z + gradient_h_[0]) + gradient_v_[1];
        if (f.use_register_lod) w = s + tfetch_lod_;
        float value[4] = {c[0] + s, c[1] - s, z, w};
        if (f.has_result()) store_fetch(f.result, value);
    }

    // A texture operation through the environment: in a pixel shader, one
    // that takes implicit derivatives (a computed LOD without register
    // gradients, getGradients) waits for its quad's coordinates first.
    void environment_fetch(const TextureFetchInstruction &f) {
        if (!f.has_result()) return;
        TextureFetch t;
        t.instruction = &f;
        t.fetch_constant = &k_.fetch_constants[(f.operands[1].index & 31) * 6];
        for (uint32_t i = 0; i < 4; i++) t.coordinates[i] = component(f.operands[0], i, false);
        t.lod = tfetch_lod_;
        std::memcpy(t.gradient_h, gradient_h_, sizeof(t.gradient_h));
        std::memcpy(t.gradient_v, gradient_v_, sizeof(t.gradient_v));
        const bool implicit =
            f.opcode == FetchOpcode::GetTextureGradients ||
            ((f.opcode == FetchOpcode::TextureFetch || f.opcode == FetchOpcode::GetTextureComputedLod) &&
             f.use_computed_lod && !f.use_register_gradients);
        if (pixel_ && implicit) {
            if (!have_quad_) {
                std::memcpy(waiting_coordinates_, t.coordinates, sizeof(waiting_coordinates_));
                waiting_ = true;
                return;
            }
            have_quad_ = false;
            t.pixel = true;
            std::memcpy(t.quad, quad_, sizeof(t.quad));
            t.lane = lane_;
        } else if (pixel_) {
            t.pixel = true;
            for (auto &lane : t.quad) std::memcpy(lane, t.coordinates, sizeof(lane));
        }
        float value[4] = {};
        std::string why;
        if (!env_->texture(t, value, why)) {
            if (unsupported.empty()) unsupported = why.empty() ? "a texture operation" : why;
            return;
        }
        store_fetch(f.result, value);
    }

    static constexpr uint32_t NOT_WAITING = UINT32_MAX;
    const ParsedShader &shader_;
    const harness::Constants &k_;
    std::span<const uint8_t> memory_;
    harness::CaseOutput &out_;
    Environment *env_ = nullptr;
    bool pixel_ = false;
    uint32_t pc_ = 0, resume_slot_ = NOT_WAITING;
    uint64_t steps_ = 0;
    bool waiting_ = false, have_quad_ = false;
    float waiting_coordinates_[4] = {}, quad_[4][4] = {};
    uint32_t lane_ = 0;
    float export_address_[4] = {}, export_data_[5][4] = {};
    uint32_t export_written_ = 0;
    bool exports_allowed_ = true;
    uint32_t loop_count_[4] = {};
    int32_t loop_address_[4] = {}; // aL is loop_address_[0]
    uint32_t call_stack_[4] = {};
    uint32_t call_depth_ = 0;
    uint32_t vfetch_address_ = 0, vfetch_end_ = 0; // the last full fetch's, for mini fetches
    float tfetch_lod_ = 0.0f, gradient_h_[3] = {}, gradient_v_[3] = {};
};

} // namespace

float reduce_precision(float value, uint32_t mantissa_bits) {
    uint32_t bits = float_bits(value);
    if ((bits & 0x7F800000u) == 0x7F800000u) return value;
    uint32_t truncate_bits = 23 - mantissa_bits;
    uint32_t discarded_mask = (1u << truncate_bits) - 1;
    uint32_t truncated = bits & ~discarded_mask;
    uint32_t rounded = truncated + (1u << truncate_bits);
    // never round a finite result up to infinity
    if ((rounded & 0x7F800000u) == 0x7F800000u) rounded = truncated;
    uint32_t round_bit = 1u << (truncate_bits - 1);
    return bits_float((bits & discarded_mask) >= round_bit ? rounded : truncated);
}

bool interpret(const ParsedShader &shader, const harness::CaseInput &input, const harness::Constants &constants,
               std::span<const uint8_t> memory, harness::CaseOutput &output, std::string &unsupported) {
    std::memset(&output, 0, sizeof(output));
    Machine m(shader, constants, memory, output);
    std::memcpy(m.r, input.registers, sizeof(m.r));
    bool ok = m.run();
    std::memcpy(output.registers, m.r, sizeof(output.registers));
    output.killed = m.killed ? 1 : 0;
    output.predicate = m.p ? 1 : 0;
    output.address_register = m.a0;
    unsupported = m.unsupported;
    return ok;
}

struct Invocation::State {
    harness::CaseOutput out{};
    Machine machine;
    State(const ParsedShader &shader, const harness::Constants &constants, std::span<const uint8_t> memory,
          Environment *environment, bool pixel)
        : machine(shader, constants, memory, out, environment, pixel) {}
};

Invocation::Invocation(const ParsedShader &shader, const harness::Constants &constants,
                       std::span<const uint8_t> memory, Environment *environment, bool pixel)
    : state_(new State(shader, constants, memory, environment, pixel)) {}
Invocation::~Invocation() { delete state_; }
float (*Invocation::registers())[4] { return state_->machine.r; }
Invocation::Status Invocation::run() {
    Status s = state_->machine.steps();
    if (s == Status::Done) state_->out.killed = state_->machine.killed ? 1 : 0;
    return s;
}
uint64_t Invocation::waiting_at() const { return state_->machine.waiting_at(); }
const float *Invocation::waiting() const { return state_->machine.waiting(); }
void Invocation::resume(const float quad[4][4], uint32_t lane) { state_->machine.resume(quad, lane); }
const harness::CaseOutput &Invocation::output() const { return state_->out; }
uint32_t Invocation::colors_written() const { return state_->machine.colors_written; }
bool Invocation::killed() const { return state_->machine.killed; }
const std::string &Invocation::unsupported() const { return state_->machine.unsupported; }

} // namespace xf::shader
