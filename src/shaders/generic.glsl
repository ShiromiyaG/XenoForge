// The generic shaders' interpreter (docs/renderer.md, "The generic shaders";
// shader_program.h has the program's layout): any Xenos shader run from a
// buffer of programs, so a draw whose translated pipeline is not made yet is
// drawn at once. The semantics are the translator's, written again here
// instruction by instruction (shader_translator.cpp, which follows Xenia's
// SPIR-V translator: BSD, see LICENSE-xenia): the legacy multiply, the
// operations without contraction (precise), the ordered and unordered
// comparisons, the 21-bit results, the vertex formats, the texture fetch's
// LOD, gradients, signs, gamma and number format, the pixel parameters, the
// alpha test, alpha to coverage and the color outputs; checked against the
// CPU interpreter by shader_compare_test through the compute form. Memory
// exports are not run (encode_program refuses the shaders with them).
//
// Included by generic.vert, generic.frag and generic.comp with one of
// GENERIC_VERTEX, GENERIC_PIXEL, GENERIC_HARNESS defined.

#extension GL_EXT_control_flow_attributes : enable

// ---- resources ------------------------------------------------------------

#ifdef GENERIC_HARNESS
layout(set = 0, binding = 1, std430) readonly buffer Constants {
    vec4 c[256];
    uvec4 bools[2];
    uvec4 loops[8];
    uvec4 fetch[48];
} consts;
layout(set = 0, binding = 2, std430) readonly buffer Memory { uint words[]; } memory;
layout(set = 0, binding = 4, std430) readonly buffer Programs { uvec4 units[]; } programs;
const uint program_base = 0u;
#else
#extension GL_EXT_nonuniform_qualifier : require
#ifdef GENERIC_PIXEL
layout(set = 0, binding = 3, std140) uniform Constants {
#else
layout(set = 0, binding = 0, std140) uniform Constants {
#endif
    vec4 c[256];
    uvec4 bools[2];
    uvec4 loops[8];
    uvec4 fetch[48];
} consts;
layout(set = 0, binding = 1, std430) readonly buffer Memory { uint words[]; } memory;
// renderer.cpp's System, as the translator declares it, with the generic
// shaders' word at the end (x: the vertex shader's program, y the pixel
// shader's, in 16-byte units)
layout(set = 0, binding = 2, std140) uniform System {
    vec4 ndc_scale;
    vec4 ndc_offset;
    uvec4 slots[8];
    uvec4 flags;
    vec4 values;
    vec4 color_scales;
    vec4 point;
    uvec4 signs[8];
    uvec4 signed_slots[8];
    vec4 clip_planes[6];
    vec4 extent_map;
    uvec4 extent_report;
    uvec4 integer_scale[8];
    uvec4 blend_premultiply;
    vec4 blend_constant;
    uvec4 generic;
} sys;
layout(set = 0, binding = 8, std430) readonly buffer Programs { uvec4 units[]; } programs;
layout(set = 1, binding = 0) uniform texture2DArray images2d[];
layout(set = 1, binding = 1) uniform texture3D images3d[];
layout(set = 1, binding = 2) uniform textureCube imagescube[];
layout(set = 1, binding = 3) uniform sampler samplers[];
#ifdef GENERIC_PIXEL
#define program_base sys.generic.y
#else
#define program_base sys.generic.x
#endif
#endif

// Xenia's texture_point_snap (VertexOptions::texture_point_snap), the run's
layout(constant_id = 21) const bool POINT_SNAP = false;

#define FLT_MAX_ uintBitsToFloat(0x7F7FFFFFu)
#define INF uintBitsToFloat(0x7F800000u)

// ---- the shader's state ----------------------------------------------------

vec4 r[128];
bool p0;
int a0, aL;
float ps;
int vfetch_address, vfetch_bound;
bool killed;
vec4 o[16];
vec4 oPos, oPts, oDepth;
vec4 oC[4];
uint oC_written;
uint loop_count[4];
int loop_address[4];
uint call_stack[4];
uint call_depth;
float tfetch_lod;
vec4 gradient_h, gradient_v;

void reset_state() {
    p0 = false;
    a0 = 0, aL = 0;
    ps = 0.0;
    vfetch_address = 0, vfetch_bound = 0;
    killed = false;
    for (uint k = 0u; k < 16u; k++) o[k] = vec4(0.0);
    oPos = vec4(0.0), oPts = vec4(0.0), oDepth = vec4(0.0);
    for (uint k = 0u; k < 4u; k++) oC[k] = vec4(0.0);
    oC_written = 0u;
    for (uint k = 0u; k < 4u; k++) loop_count[k] = 0u, loop_address[k] = 0, call_stack[k] = 0u;
    call_depth = 0u;
    tfetch_lod = 0.0;
    gradient_h = vec4(0.0), gradient_v = vec4(0.0);
}

// ---- values -----------------------------------------------------------------

float nclamp(float x, float lo, float hi) { return isnan(x) ? lo : clamp(x, lo, hi); }
float nmax(float a, float b) { return isnan(a) ? b : isnan(b) ? a : max(a, b); }

// Xenos's multiply: 0 times anything (infinity, NaN) is 0
float legacy_mul(float a, float b) {
    precise float p = a * b;
    return (abs(a) == 0.0 || abs(b) == 0.0) ? 0.0 : p;
}
vec4 legacy_mul(vec4 a, vec4 b) {
    return vec4(legacy_mul(a.x, b.x), legacy_mul(a.y, b.y), legacy_mul(a.z, b.z), legacy_mul(a.w, b.w));
}

// 21 mantissa bits, rounded, not into infinity; infinities and NaN untouched
float reduce_precision(float value) {
    uint b = floatBitsToUint(value);
    const uint mask = ~3u;
    uint truncated = b & mask, discarded = b & ~mask;
    bool round_up = discarded >= 2u;
    uint rounded = truncated + 4u;
    bool finite = (b & 0x7F800000u) != 0x7F800000u;
    bool to_infinity = (rounded & 0x7F800000u) == 0x7F800000u;
    uint safe = (finite && to_infinity) ? truncated : rounded;
    uint result = round_up ? safe : truncated;
    return uintBitsToFloat(finite ? result : b);
}

uint fetch_dword(uint d) { return consts.fetch[d >> 2u][d & 3u]; }
bool bool_constant(uint b) {
    uint word = consts.bools[b >> 7u][(b >> 5u) & 3u];
    return ((word >> (b & 31u)) & 1u) != 0u;
}
uint loop_constant(uint i) { return consts.loops[(i & 31u) >> 2u][i & 3u]; }

// ---- operands and results --------------------------------------------------

int relative_base(uint addressing) { return addressing == 1u ? a0 : aL; }

vec4 load_storage(uint w) {
    uint index = w & 511u, addressing = (w >> 10u) & 3u;
    if ((w & 512u) == 0u) {
        int at = addressing == 0u ? int(index & 127u) : ((relative_base(addressing) + int(index)) & 127);
        return r[at];
    }
    if (addressing == 0u) return index > 255u ? vec4(0.0) : consts.c[index];
    // relative: outside 0-255 reads zeros
    int at = relative_base(addressing) + int(index);
    return uint(at) < 256u ? consts.c[clamp(at, 0, 255)] : vec4(0.0);
}
vec4 modifiers(vec4 v, uint w) {
    if ((w & (1u << 13u)) != 0u) v = abs(v);
    if ((w & (1u << 12u)) != 0u) v = -v;
    return v;
}
uint operand_component(uint w, uint k) { return (w >> (14u + 2u * k)) & 3u; }
vec4 operand(uint w) {
    vec4 v = load_storage(w);
    vec4 s = vec4(v[operand_component(w, 0u)], v[operand_component(w, 1u)], v[operand_component(w, 2u)],
                  v[operand_component(w, 3u)]);
    return modifiers(s, w);
}

const uint T_NONE = 0u, T_REGISTER = 1u, T_INTERPOLATOR = 2u, T_POSITION = 3u, T_POINT_SIZE = 4u,
           T_EXPORT_ADDRESS = 5u, T_EXPORT_DATA = 6u, T_COLOR = 7u, T_DEPTH = 8u;

// value: false where the operation gave none (its masked components are all
// constants then)
void store_result(uint w, vec4 value, bool has_value) {
    uint mask = (w >> 14u) & 15u;
    if (mask == 0u) return;
    uint target = w & 15u, index = (w >> 4u) & 127u;
    bool clamped = (w & (1u << 13u)) != 0u;
    vec4 current;
    int at = 0;
    switch (target) {
    case T_REGISTER: {
        uint addressing = (w >> 11u) & 3u;
        at = addressing == 0u ? int(index & 127u) : ((relative_base(addressing) + int(index)) & 127);
        current = r[at];
        break;
    }
    case T_INTERPOLATOR: current = o[index & 15u]; break;
    case T_POSITION: current = oPos; break;
    case T_POINT_SIZE: current = oPts; break;
    case T_COLOR: current = oC[index & 3u]; oC_written |= 1u << (index & 3u); break;
    case T_DEPTH: current = oDepth; break;
    default: return; // none, and eA and eM# (memory exports are not run)
    }
    for (uint k = 0u; k < 4u; k++) {
        if ((mask & (1u << k)) == 0u) continue;
        uint s = (w >> (18u + 3u * k)) & 7u;
        float c;
        if (s >= 4u || !has_value) {
            c = s == 5u ? 1.0 : 0.0;
        } else {
            c = value[s];
            if (clamped || target == T_DEPTH) c = nclamp(c, 0.0, 1.0);
        }
        current[k] = c;
    }
    switch (target) {
    case T_REGISTER: r[at] = current; break;
    case T_INTERPOLATOR: o[index & 15u] = current; break;
    case T_POSITION: oPos = current; break;
    case T_POINT_SIZE: oPts = current; break;
    case T_COLOR: oC[index & 3u] = current; break;
    case T_DEPTH: oDepth = current; break;
    default: break;
    }
}

// ---- ALU ------------------------------------------------------------------

void kill(bool condition) {
    if (condition) {
        killed = true;
#ifdef GENERIC_PIXEL
        demote;
#endif
    }
}

void set_address_register(float value, float low, float high) { a0 = int(nclamp(floor(value), low, high)); }

float b2f(bool b) { return b ? 1.0 : 0.0; }

vec4 vector_op(uint opcode, uint w0, uint wo0, uint wo1, uint wo2) {
    uint count = opcode == 18u ? 1u : (w0 >> 17u) & 3u;
    vec4 a = count > 0u ? operand(wo0) : vec4(0.0);
    vec4 b = count > 1u ? operand(wo1) : vec4(0.0);
    vec4 c = count > 2u ? operand(wo2) : vec4(0.0);
    switch (opcode) {
    case 0u: { precise vec4 s = a + b; return s; }
    case 1u: return legacy_mul(a, b);
    case 11u: { precise vec4 s = legacy_mul(a, b) + c; return s; }
    case 2u:
    case 29u:
        if (opcode == 29u) {
            precise float x = a.w + 0.5;
            set_address_register(x, -256.0, 255.0);
        }
        return vec4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w);
    case 3u: return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w);
    case 4u: return vec4(b2f(a.x == b.x), b2f(a.y == b.y), b2f(a.z == b.z), b2f(a.w == b.w));
    case 5u: return vec4(b2f(a.x > b.x), b2f(a.y > b.y), b2f(a.z > b.z), b2f(a.w > b.w));
    case 6u: return vec4(b2f(a.x >= b.x), b2f(a.y >= b.y), b2f(a.z >= b.z), b2f(a.w >= b.w));
    case 7u: return vec4(b2f(a.x != b.x), b2f(a.y != b.y), b2f(a.z != b.z), b2f(a.w != b.w));
    case 8u: return fract(a);
    case 9u: return trunc(a);
    case 10u: return floor(a);
    case 12u: return vec4(a.x == 0.0 ? b.x : c.x, a.y == 0.0 ? b.y : c.y, a.z == 0.0 ? b.z : c.z, a.w == 0.0 ? b.w : c.w);
    case 13u: return vec4(a.x >= 0.0 ? b.x : c.x, a.y >= 0.0 ? b.y : c.y, a.z >= 0.0 ? b.z : c.z, a.w >= 0.0 ? b.w : c.w);
    case 14u: return vec4(a.x > 0.0 ? b.x : c.x, a.y > 0.0 ? b.y : c.y, a.z > 0.0 ? b.z : c.z, a.w > 0.0 ? b.w : c.w);
    case 15u:
    case 16u:
    case 17u: {
        uint n = opcode == 15u ? 4u : opcode == 16u ? 3u : 2u;
        precise float sum = legacy_mul(a.x, b.x);
        for (uint k = 1u; k < n; k++) sum = sum + legacy_mul(a[k], b[k]);
        if (opcode == 17u) sum = sum + c.x;
        return vec4(sum);
    }
    case 18u: {
        // the operand's z, w, x as the cube's x, y, z
        float x = a.z, y = a.w, z = a.x;
        float ax = abs(x), ay = abs(y), az = abs(z);
        bool z_major = az >= ax && az >= ay, y_major = ay >= ax;
        vec4 on_z = vec4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);
        vec4 on_y = vec4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);
        vec4 on_x = vec4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);
        vec4 result = z_major ? on_z : (y_major ? on_y : on_x);
        precise float doubled = 2.0 * result.z;
        result.z = doubled;
        return result;
    }
    case 19u: {
        // the largest of the components the swizzle names, NaN passed over
        uint named = 0u;
        for (uint k = 0u; k < 4u; k++) named |= 1u << operand_component(wo0, k);
        vec4 m = modifiers(load_storage(wo0), wo0);
        float result = 0.0;
        bool any = false;
        for (uint k = 0u; k < 4u; k++) {
            if ((named & (1u << k)) == 0u) continue;
            result = any ? nmax(result, m[k]) : m[k];
            any = true;
        }
        return vec4(result);
    }
    case 20u:
    case 21u:
    case 22u:
    case 23u: {
        bool t[4];
        for (uint k = 0u; k < 4u; k++) {
            float y = b[k];
            bool test = opcode == 20u ? y == 0.0 : opcode == 21u ? y != 0.0 : opcode == 22u ? y > 0.0 : y >= 0.0;
            t[k] = a[k] == 0.0 && test;
        }
        p0 = t[3];
        precise float v = (t[0] ? -1.0 : a.x) + 1.0;
        return vec4(v);
    }
    case 24u:
    case 25u:
    case 26u:
    case 27u: {
        bool any = false;
        for (uint k = 0u; k < 4u; k++) {
            float x = a[k], y = b[k];
            any = any || (opcode == 24u ? x == y : opcode == 25u ? x > y : opcode == 26u ? x >= y : x != y);
        }
        kill(any);
        return vec4(0.0);
    }
    case 28u: return vec4(1.0, legacy_mul(a.y, b.y), a.z, b.w);
    }
    return vec4(0.0);
}

// false: no value (retain_prev)
bool scalar_op(uint opcode, uint w0, uint wo0, uint wo1, out float result) {
    uint count = (w0 >> 19u) & 3u;
    vec4 a = count > 0u ? operand(wo0) : vec4(0.0);
    vec4 b = count > 1u ? operand(wo1) : vec4(0.0);
    float x = a.x, y = a.y;
    precise float v = 0.0;
    switch (opcode) {
    case 0u: v = x + y; break;
    case 25u: v = x - y; break;
    case 1u: v = x + ps; break;
    case 26u: v = x - ps; break;
    case 2u: v = legacy_mul(x, y); break;
    case 3u: v = legacy_mul(x, ps); break;
    case 4u: {
        float p = ps;
        bool ok = p != -FLT_MAX_ && -abs(p) >= -FLT_MAX_ && -abs(y) >= -FLT_MAX_ && y > 0.0;
        v = ok ? legacy_mul(x, p) : -FLT_MAX_;
        break;
    }
    case 5u: v = x >= y ? x : y; break;
    case 6u: v = x < y ? x : y; break;
    case 23u:
    case 24u: {
        precise float half_up = x + 0.5;
        set_address_register(opcode == 23u ? half_up : x, 0.0, 255.0);
        v = x >= y ? x : y;
        break;
    }
    case 7u: v = b2f(x == 0.0); break;
    case 8u: v = b2f(x > 0.0); break;
    case 9u: v = b2f(x >= 0.0); break;
    case 10u: v = b2f(x != 0.0); break;
    case 11u: v = fract(x); break;
    case 12u: v = trunc(x); break;
    case 13u: v = floor(x); break;
    case 48u: v = sin(x); break;
    case 49u: v = cos(x); break;
    case 14u: v = reduce_precision(exp2(x)); break;
    case 16u: v = reduce_precision(log2(x)); break;
    case 40u: v = reduce_precision(sqrt(x)); break;
    case 22u: v = reduce_precision(inversesqrt(x)); break;
    case 19u: { precise float q = 1.0 / x; v = reduce_precision(q); break; }
    case 15u: {
        float l = reduce_precision(log2(x));
        v = l == -INF ? -FLT_MAX_ : l;
        break;
    }
    case 17u:
    case 20u: {
        precise float q = 1.0 / x;
        float l = reduce_precision(opcode == 17u ? q : inversesqrt(x));
        l = l == -INF ? -FLT_MAX_ : l;
        v = l == INF ? FLT_MAX_ : l;
        break;
    }
    case 18u:
    case 21u: {
        precise float q = 1.0 / x;
        float l = reduce_precision(opcode == 18u ? q : inversesqrt(x));
        l = l == INF ? 0.0 : l;
        v = uintBitsToFloat(l == -INF ? 0x80000000u : floatBitsToUint(l));
        break;
    }
    case 27u:
    case 28u:
    case 29u:
    case 30u: {
        bool p = opcode == 27u ? x == 0.0 : opcode == 28u ? x != 0.0 : opcode == 29u ? x > 0.0 : x >= 0.0;
        p0 = p;
        v = p ? 0.0 : 1.0;
        break;
    }
    case 31u: {
        bool p = x == 1.0;
        p0 = p;
        v = p ? 0.0 : (x == 0.0 ? 1.0 : x);
        break;
    }
    case 32u: {
        precise float t = x - 1.0;
        bool p = t <= 0.0;
        p0 = p;
        v = p ? 0.0 : t;
        break;
    }
    case 33u: p0 = false; v = FLT_MAX_; break;
    case 34u: {
        bool p = x == 0.0;
        p0 = p;
        v = p ? 0.0 : x;
        break;
    }
    case 35u: kill(x == 0.0); v = 0.0; break;
    case 36u: kill(x > 0.0); v = 0.0; break;
    case 37u: kill(x >= 0.0); v = 0.0; break;
    case 38u: kill(x != 0.0); v = 0.0; break;
    case 39u: kill(x == 1.0); v = 0.0; break;
    case 42u:
    case 43u: v = legacy_mul(x, b.x); break;
    case 44u:
    case 45u: v = x + b.x; break;
    case 46u:
    case 47u: v = x - b.x; break;
    default: result = 0.0; return false; // retain_prev
    }
    result = v;
    return true;
}

void alu(uvec4 i0, uvec4 i1) {
    uint w0 = i0.x;
    if ((w0 & 4u) != 0u && p0 != ((w0 & 8u) != 0u)) return;
    // the vector operation first (its a0 and predicate seen by the scalar
    // one), then the scalar, then both results stored
    vec4 vector = vec4(0.0);
    bool vector_value = (w0 & (1u << 15u)) != 0u;
    if (vector_value) vector = vector_op((w0 >> 4u) & 31u, w0, i0.w, i1.x, i1.y);
    float scalar;
    bool scalar_value = scalar_op((w0 >> 9u) & 63u, w0, i1.z, i1.w, scalar);
    if (scalar_value) {
        ps = scalar;
    } else if (((i0.z >> 14u) & 15u) != 0u) {
        scalar = ps;
        scalar_value = true;
    }
    store_result(i0.y, vector, vector_value);
    store_result(i0.z, vec4(scalar), scalar_value);
}

// ---- vertex fetch ------------------------------------------------------------

uint swap_endian(uint v, uint endian) {
    uint in16 = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    uint in32 = (in16 << 16u) | (in16 >> 16u);
    uint halves = (v << 16u) | (v >> 16u);
    return endian == 1u ? in16 : endian == 2u ? in32 : endian == 3u ? halves : v;
}

// the normalization scales, as the translator's CPU computes them (1 over
// 2^w - 1, 2^(w-1) - 1, and 2^(w-1) - 0.5), by width
const uint UNSIGNED_SCALE[17] = uint[](0u, 0x3F800000u, 0x3EAAAAABu, 0x3E124925u, 0x3D888889u, 0x3D042108u, 0x3C820821u,
                                       0x3C010204u, 0x3B808081u, 0x3B004020u, 0x3A802008u, 0x3A001002u, 0x39800801u,
                                       0x39000400u, 0x38800200u, 0x38000100u, 0x37800080u);
const uint SIGNED_SCALE[17] = uint[](0u, 0x7F800000u, 0x3F800000u, 0x3EAAAAABu, 0x3E124925u, 0x3D888889u, 0x3D042108u,
                                     0x3C820821u, 0x3C010204u, 0x3B808081u, 0x3B004020u, 0x3A802008u, 0x3A001002u,
                                     0x39800801u, 0x39000400u, 0x38800200u, 0x38000100u);
const uint NO_ZERO_SCALE[17] = uint[](0u, 0x40000000u, 0x3F2AAAABu, 0x3E924925u, 0x3E088889u, 0x3D842108u, 0x3D020821u,
                                      0x3C810204u, 0x3C008081u, 0x3B804020u, 0x3B002008u, 0x3A801002u, 0x3A000801u,
                                      0x39800400u, 0x39000200u, 0x38800100u, 0x38000080u);

void vertex_fetch(uvec4 i0, uvec4 i1) {
    uint w0 = i0.x;
    if ((w0 & 4u) != 0u && p0 != ((w0 & 8u) != 0u)) return;
    uint format = (w0 >> 5u) & 63u, slot = (w0 >> 16u) & 255u;
    bool is_signed = (w0 & (1u << 11u)) != 0u, is_integer = (w0 & (1u << 12u)) != 0u;
    bool no_zero = (w0 & (1u << 14u)) != 0u;
    int offset = int(i0.w);
    uint stride = i1.x;
    int exp_adjust = int(i1.y);
    // the format: words, components, kind (0 packed, 1 half, 2 int32, 3 float32)
    uint words = 1u, components = 4u, kind = 0u;
    uvec4 width = uvec4(0u), shift = uvec4(0u), word_of = uvec4(0u);
    switch (format) {
    case 6u: width = uvec4(8u); shift = uvec4(0u, 8u, 16u, 24u); break;
    case 7u: width = uvec4(10u, 10u, 10u, 2u); shift = uvec4(0u, 10u, 20u, 30u); break;
    case 16u: components = 3u; width = uvec4(11u, 11u, 10u, 0u); shift = uvec4(0u, 11u, 22u, 0u); break;
    case 17u: components = 3u; width = uvec4(10u, 11u, 11u, 0u); shift = uvec4(0u, 10u, 21u, 0u); break;
    case 25u: components = 2u; width = uvec4(16u, 16u, 0u, 0u); shift = uvec4(0u, 16u, 0u, 0u); break;
    case 26u: words = 2u; width = uvec4(16u); shift = uvec4(0u, 16u, 0u, 16u); word_of = uvec4(0u, 0u, 1u, 1u); break;
    case 31u: components = 2u; kind = 1u; break;
    case 32u: words = 2u; kind = 1u; break;
    case 33u: components = 1u; kind = 2u; break;
    case 34u: words = 2u; components = 2u; kind = 2u; break;
    case 35u: words = 4u; kind = 2u; break;
    case 36u: components = 1u; kind = 3u; break;
    case 37u: words = 2u; components = 2u; kind = 3u; break;
    case 57u: words = 3u; components = 3u; kind = 3u; break;
    default: words = 4u; kind = 3u; break; // 38: 32_32_32_32_FLOAT
    }
    uint word1 = fetch_dword(slot * 2u + 1u);
    int address, bound;
    if ((w0 & 16u) != 0u) {
        address = vfetch_address;
        bound = vfetch_bound;
    } else {
        uint word0 = fetch_dword(slot * 2u);
        address = int((word0 >> 2u) & 0x07FFFFFFu);
        int size = int((word1 >> 2u) & 0xFFFFFFu);
        bound = address + size;
        vfetch_bound = bound;
        if (stride != 0u) {
            precise float index = operand(i0.z).x;
            if ((w0 & (1u << 13u)) != 0u) index = index + 0.5;
            int i = int(floor(index));
            if (stride > 1u) i = i * int(stride);
            address = address + i;
        }
        vfetch_address = address;
    }
    uint endian = word1 & 3u;
    uint length = uint(memory.words.length());
    uint w[4];
    for (uint k = 0u; k < 4u; k++) {
        w[k] = 0u;
        if (k >= words) continue;
        uint at = uint(address + offset + int(k));
        bool inside = at < uint(bound) && at < length;
        uint word = memory.words[inside ? at : 0u];
        w[k] = swap_endian(inside ? word : 0u, endian);
    }
    vec4 c = vec4(0.0);
    if (kind == 3u) {
        for (uint k = 0u; k < components; k++) c[k] = uintBitsToFloat(w[k]);
    } else if (kind == 1u) {
        for (uint k = 0u; k < words; k++) {
            vec2 pair = unpackHalf2x16(w[k]);
            c[k * 2u] = pair.x;
            c[k * 2u + 1u] = pair.y;
        }
    } else if (kind == 2u) {
        for (uint k = 0u; k < components; k++) {
            precise float value = is_signed ? float(int(w[k])) : float(w[k]);
            if (!is_integer) {
                if (!is_signed) {
                    value = value * (1.0 / 4294967296.0);
                } else if (!no_zero) {
                    value = value * (1.0 / 2147483648.0);
                } else {
                    value = value * (1.0 / 2147483648.0);
                    value = value + (0.5 / 2147483648.0);
                }
            }
            c[k] = value;
        }
    } else {
        for (uint k = 0u; k < components; k++) {
            uint wd = width[k];
            uint word = w[word_of[k]];
            precise float value = is_signed ? float(bitfieldExtract(int(word), int(shift[k]), int(wd)))
                                            : float(bitfieldExtract(word, int(shift[k]), int(wd)));
            if (!is_integer) {
                float scale = uintBitsToFloat(!is_signed ? UNSIGNED_SCALE[wd] : no_zero ? NO_ZERO_SCALE[wd] : SIGNED_SCALE[wd]);
                value = value * scale;
                if (is_signed) {
                    if (!no_zero) {
                        value = max(value, -1.0);
                    } else {
                        precise float half_step = 0.5 * scale;
                        value = value + half_step;
                    }
                }
            }
            c[k] = value;
        }
    }
    if (exp_adjust != 0) {
        float factor = ldexp(1.0, exp_adjust);
        for (uint k = 0u; k < components; k++) {
            precise float scaled = c[k] * factor;
            c[k] = scaled;
        }
    }
    store_result(i0.y, c, true);
}

// ---- texture fetch -----------------------------------------------------------

#ifndef GENERIC_HARNESS
uint slot_word(uint member, uint slot) {
    uint q = slot >> 2u, k = slot & 3u;
    return member == 2u ? sys.slots[q][k] : member == 7u ? sys.signs[q][k] : member == 8u ? sys.signed_slots[q][k]
                                                                                    : sys.integer_scale[q][k];
}

// xenos.cc's PWLGammaToLinear as the translator writes it
float texture_gamma_to_linear(float gamma) {
    gamma = nclamp(gamma, 0.0, 1.0);
    bool piece_3 = gamma >= 192.0 / 255.0, piece_2 = gamma >= 96.0 / 255.0, piece_1 = gamma >= 64.0 / 255.0;
    float scale = piece_2 ? (piece_3 ? 8.0 / 1024.0 : 4.0 / 1024.0) : (piece_1 ? 2.0 / 1024.0 : 1.0 / 1024.0);
    float offset = piece_2 ? (piece_3 ? -1024.0 : -256.0) : (piece_1 ? -64.0 : 0.0);
    precise float linear = (gamma * (255.0 * 1024.0)) * scale + offset;
    precise float stepped = linear * scale;
    precise float result = (linear + trunc(stepped)) * (1.0 / 1023.0);
    return result;
}

// What sample_texture asks of an image (the translator's `sample` lambda)
struct Sampling {
    bool lod_query, computed, cube_implicit, register_gradients, unnormalized, cube;
    float lod, exponent_h, exponent_v;
    vec3 size;
};

vec3 gradient_of(Sampling s, vec4 g, uint n) {
    vec3 parts = g.xyz;
    if (s.unnormalized && !s.cube) {
        for (uint k = 0u; k < n; k++) {
            precise float q = parts[k] / s.size[k];
            parts[k] = q;
        }
    }
    return parts;
}

vec4 sample_2d(Sampling s, uint image, uint sampler_index, vec3 coords, vec2 differentiated) {
#define t sampler2DArray(images2d[image], samplers[sampler_index])
#ifdef GENERIC_PIXEL
    if (s.lod_query) return vec4(textureQueryLod(t, differentiated).y, 0.0, 0.0, 0.0);
#else
    if (s.lod_query) return vec4(0.0);
#endif
    if (!s.computed) return textureLod(t, coords, s.lod);
    vec2 gh, gv;
    if (s.register_gradients) {
        gh = gradient_of(s, gradient_h, 2u).xy;
        gv = gradient_of(s, gradient_v, 2u).xy;
    } else {
#ifdef GENERIC_PIXEL
        gh = dFdxCoarse(differentiated);
        gv = dFdyCoarse(differentiated);
#else
        gh = gv = vec2(0.0);
#endif
    }
    precise float fh = exp2(s.lod + s.exponent_h), fv = exp2(s.lod + s.exponent_v);
    precise vec2 h = gh * fh, v = gv * fv;
    return textureGrad(t, coords, h, v);
#undef t
}

vec4 sample_3d(Sampling s, uint image, uint sampler_index, vec3 coords) {
#define t sampler3D(images3d[image], samplers[sampler_index])
#ifdef GENERIC_PIXEL
    if (s.lod_query) return vec4(textureQueryLod(t, coords).y, 0.0, 0.0, 0.0);
#else
    if (s.lod_query) return vec4(0.0);
#endif
    if (!s.computed) return textureLod(t, coords, s.lod);
    vec3 gh, gv;
    if (s.register_gradients) {
        gh = gradient_of(s, gradient_h, 3u);
        gv = gradient_of(s, gradient_v, 3u);
    } else {
#ifdef GENERIC_PIXEL
        gh = dFdxCoarse(coords);
        gv = dFdyCoarse(coords);
#else
        gh = gv = vec3(0.0);
#endif
    }
    precise float fh = exp2(s.lod + s.exponent_h), fv = exp2(s.lod + s.exponent_v);
    precise vec3 h = gh * fh, v = gv * fv;
    return textureGrad(t, coords, h, v);
#undef t
}

vec4 sample_cube(Sampling s, uint image, uint sampler_index, vec3 coords) {
#define t samplerCube(imagescube[image], samplers[sampler_index])
#ifdef GENERIC_PIXEL
    if (s.lod_query) return vec4(textureQueryLod(t, coords).y, 0.0, 0.0, 0.0);
    if (s.cube_implicit) {
        precise float bias = s.lod + max(s.exponent_h, s.exponent_v);
        return texture(t, coords, bias);
    }
#else
    if (s.lod_query) return vec4(0.0);
#endif
    if (!s.computed) return textureLod(t, coords, s.lod);
    vec3 gh, gv;
    if (s.register_gradients) {
        gh = gradient_of(s, gradient_h, 3u);
        gv = gradient_of(s, gradient_v, 3u);
    } else {
#ifdef GENERIC_PIXEL
        gh = dFdxCoarse(coords);
        gv = dFdyCoarse(coords);
#else
        gh = gv = vec3(0.0);
#endif
    }
    precise float fh = exp2(s.lod + s.exponent_h), fv = exp2(s.lod + s.exponent_v);
    precise vec3 h = gh * fh, v = gv * fv;
    return textureGrad(t, coords, h, v);
#undef t
}

float signed_bits(uint word, int offset, int count) { return float(bitfieldExtract(int(word), offset, count)); }
float size_of(uint minus_one) { return float(minus_one + 1u); }

const uint QUERY_SAMPLE = 0u, QUERY_LOD = 1u, QUERY_WEIGHTS = 2u;

// shader_translator.cpp's sample_texture, its fixed parts read from the
// instruction's words at run time
vec4 sample_texture(uvec4 i0, uvec4 i1, vec4 coordinates, uint member, uint query) {
    uint w0 = i0.x;
    uint opcode = (w0 >> 4u) & 31u, dimension = (w0 >> 9u) & 3u, slot = (w0 >> 20u) & 31u;
    bool unnormalized = (w0 & (1u << 11u)) != 0u, use_computed_lod = (w0 & (1u << 12u)) != 0u;
    bool use_register_lod = (w0 & (1u << 13u)) != 0u, use_register_gradients = (w0 & (1u << 14u)) != 0u;
    bool point_allowed = (w0 & (1u << 15u)) != 0u, one = (w0 & (1u << 25u)) != 0u;
    uint vol_mag = (w0 >> 26u) & 3u, vol_min = (w0 >> 28u) & 3u;
    float lod_bias = uintBitsToFloat(i0.w);
    vec3 instruction_offsets = vec3(uintBitsToFloat(i1.x), uintBitsToFloat(i1.y), uintBitsToFloat(i1.z));
    const float epsilon = 1.5 / 1024.0;

    uint entry = slot_word(member, slot);
    uint image_index = entry & 0x3FFFu, kind = (entry >> 14u) & 3u, sampler_index = entry >> 16u;
    bool cube = dimension == 3u, volume = dimension == 2u;

    uint word2 = fetch_dword(slot * 6u + 2u), word5 = fetch_dword(slot * 6u + 5u);
    bool data_is_3d = false;
    vec3 size = vec3(0.0);
    if (one) {
        size.x = size_of(bitfieldExtract(word2, 0, 24));
    } else if (volume) {
        data_is_3d = bitfieldExtract(word5, 9, 2) == 2u;
        size.x = size_of(data_is_3d ? bitfieldExtract(word2, 0, 11) : bitfieldExtract(word2, 0, 13));
        size.y = size_of(data_is_3d ? bitfieldExtract(word2, 11, 11) : bitfieldExtract(word2, 13, 13));
        size.z = size_of(data_is_3d ? bitfieldExtract(word2, 22, 10) : bitfieldExtract(word2, 26, 6));
    } else {
        size.x = size_of(bitfieldExtract(word2, 0, 13));
        size.y = size_of(bitfieldExtract(word2, 13, 13));
        if (dimension == 0u) {
            bool data_is_1d = bitfieldExtract(word5, 9, 2) == 0u;
            size.x = data_is_1d ? size_of(bitfieldExtract(word2, 0, 24)) : size.x;
            size.y = data_is_1d ? 1.0 : size.y;
        }
    }

    bool snap = POINT_SNAP && opcode == 1u && dimension == 1u && !unnormalized && point_allowed && query == QUERY_SAMPLE;
    float rounding = snap ? 0.0 : epsilon;
    vec3 c = vec3(coordinates.x, one ? 0.0 : coordinates.y, cube || volume ? coordinates.z : 0.0);
    precise vec3 offsets = vec3(instruction_offsets.x + rounding, one ? 0.0 : instruction_offsets.y + rounding,
                                cube ? instruction_offsets.z : volume ? instruction_offsets.z + rounding : 0.0);
    if (query == QUERY_WEIGHTS) {
        uint axes = one ? 1u : volume ? 3u : 2u;
        vec4 wt = vec4(0.0);
        for (uint k = 0u; k < axes; k++) {
            precise float v = unnormalized ? c[k] : c[k] * size[k];
            precise float moved = v + (offsets[k] - 0.5);
            wt[k] = fract(moved);
        }
        return wt;
    }
    for (uint k = 0u; k < (one ? 1u : 2u); k++) {
        precise float n = unnormalized ? (c[k] + offsets[k]) / size[k] : c[k] + offsets[k] / size[k];
        c[k] = n;
    }
    float z_3d = 0.0, layer = 0.0;
    if (volume) {
        if (unnormalized) {
            precise float l = c.z + offsets.z;
            layer = l;
            precise float z = l / size.z;
            z_3d = z;
        } else {
            precise float z = c.z + offsets.z / size.z;
            z_3d = z;
            precise float l = c.z * size.z + offsets.z;
            precise float top = size.z - 1.0;
            layer = nclamp(l, 0.0, top);
        }
    }

    Sampling s;
    s.lod_query = query == QUERY_LOD;
#ifdef GENERIC_PIXEL
    s.computed = use_computed_lod;
#else
    s.computed = use_computed_lod && use_register_gradients;
#endif
    s.cube_implicit = s.computed && cube && !use_register_gradients;
    s.register_gradients = use_register_gradients;
    s.unnormalized = unnormalized;
    s.cube = cube;
    s.size = size;
    uint word4 = fetch_dword(slot * 6u + 4u);
    precise float lod = signed_bits(word4, 12, 10) * (1.0 / 32.0);
    if (use_register_lod) lod = tfetch_lod + lod;
    if (lod_bias != 0.0) lod = lod + lod_bias;
    s.lod = lod;
    s.exponent_h = signed_bits(word4, 22, 5);
    s.exponent_v = signed_bits(word4, 27, 5);

    if (cube) {
        // the cube instruction's face coordinates back to a direction
        precise float sc = c.x * 2.0 + -3.0, tc = c.y * 2.0 + -3.0;
        precise float face_value = offsets.z != 0.0 ? c.z + offsets.z : c.z;
        uint face = uint(nclamp(face_value, 0.0, 5.0));
        uint axis = face >> 1u;
        bool negative = (face & 1u) != 0u;
        float sign_ = negative ? -1.0 : 1.0;
        vec3 on_x = vec3(sign_, -tc, negative ? sc : -sc);
        vec3 on_y = vec3(sc, sign_, negative ? -tc : tc);
        vec3 on_z = vec3(negative ? -sc : sc, -tc, sign_);
        vec3 coords = axis == 0u ? on_x : axis == 1u ? on_y : on_z;
        return sample_cube(s, kind == 2u ? image_index : 0u, sampler_index, coords);
    }
    if (!volume) {
        float x = c.x, y = c.y;
        if (snap) {
            bool point = (slot_word(12u, slot) & (1u << 26u)) != 0u;
            precise float sx = (floor(c.x * size.x) + 0.5) / size.x, ex = c.x + epsilon / size.x;
            precise float sy = (floor(c.y * size.y) + 0.5) / size.y, ey = c.y + epsilon / size.y;
            x = point ? sx : ex;
            y = point ? sy : ey;
        }
        return sample_2d(s, kind == 0u ? image_index : 0u, sampler_index, vec3(x, y, 0.0), c.xy);
    }

    // 3D, or stacked; both sampled, the right one chosen after
    vec3 coords_3d = vec3(c.x, c.y, z_3d);
    vec4 sampled_3d = sample_3d(s, kind == 1u ? image_index : 0u, sampler_index, coords_3d);
    bool mag_constant = vol_mag == 3u, min_constant = vol_min == 3u;
    bool mag_linear = vol_mag == 1u, min_linear = vol_min == 1u;
    bool dynamic = false, linear = false;
    if (s.computed && (mag_constant || min_constant || mag_linear != min_linear)) {
        precise float scale_h = exp2(lod + s.exponent_h), scale_v = exp2(lod + s.exponent_v);
        float gh, gv;
        if (use_register_gradients) {
            gh = gradient_h.z, gv = gradient_v.z;
            if (unnormalized) {
                precise float qh = gh / size.z, qv = gv / size.z;
                gh = qh, gv = qv;
            }
        } else {
#ifdef GENERIC_PIXEL
            gh = dFdxCoarse(layer);
            gv = dFdyCoarse(layer);
#else
            gh = gv = 0.0;
#endif
        }
        precise float mh = gh * scale_h, mv = gv * scale_v;
        precise float layer_gradient = nmax(mh, mv);
        if (!unnormalized) layer_gradient = layer_gradient * size.z;
        bool minifying = layer_gradient < 1.0; // NaN: magnifying
        bool mag = mag_constant ? (word4 & 1u) != 0u : mag_linear;
        bool min_ = min_constant ? (word4 & 2u) != 0u : min_linear;
        linear = minifying ? min_ : mag;
        dynamic = true;
    } else if (mag_constant) {
        linear = (word4 & 1u) != 0u;
        dynamic = true;
    }
    bool always_linear = !dynamic && mag_linear;
    precise float half_back = layer - 0.5;
    float layer_coordinate = dynamic ? (linear ? half_back : layer) : always_linear ? half_back : layer;
    float layer_0 = floor(layer_coordinate);
    uint stacked_image = kind == 0u ? image_index : 0u;
    vec4 stacked = sample_2d(s, stacked_image, sampler_index, vec3(c.x, c.y, layer_0), c.xy);
    if (dynamic || always_linear) {
        precise float next_layer = layer_0 + 1.0;
        vec4 next = sample_2d(s, stacked_image, sampler_index, vec3(c.x, c.y, next_layer), c.xy);
        float factor = fract(layer_coordinate);
        precise vec4 filtered = stacked + (next - stacked) * factor;
        stacked = dynamic ? (linear ? filtered : stacked) : filtered;
    }
    return data_is_3d ? sampled_3d : stacked;
}

// The signs and the number format on a fetch's sample (fetch_post)
vec4 fetch_post(uint used, bool point_allowed, vec4 sample_, vec4 signed_sample, uint info, uint scale_word) {
    uint signs[4];
    for (uint k = 0u; k < 4u; k++) signs[k] = bitfieldExtract(info, int(2u * k), 2);
    vec4 out_ = sample_;
    for (uint k = 0u; k < 4u; k++)
        if ((used & (1u << k)) != 0u && signs[k] == 1u) out_[k] = signed_sample[k];
    for (uint k = 0u; k < 4u; k++) {
        if ((used & (1u << k)) == 0u) continue;
        float value = sample_[k];
        if (signs[k] == 2u) {
            uint width_less_one = bitfieldExtract(scale_word, int(6u * k), 4);
            float half_ = float(1u << width_less_one);
            precise float offset_binary = (value * (half_ * 2.0 - 1.0) - half_) / (half_ - 1.0);
            precise float twice_less_one = value * 2.0 + -1.0;
            out_[k] = width_less_one != 0u ? offset_binary : twice_less_one;
        }
        if (signs[k] == 3u) out_[k] = texture_gamma_to_linear(value);
    }
    if ((scale_word & ((1u << 26u) - 1u)) != 0u) {
        if ((scale_word & (1u << 24u)) != 0u) {
            // as fractions
            if (point_allowed && (scale_word & ((1u << 24u) - 1u)) != 0u) {
                for (uint k = 0u; k < 4u; k++) {
                    if ((used & (1u << k)) == 0u) continue;
                    uint lane = bitfieldExtract(scale_word, int(6u * k), 6);
                    float pow2 = float(2u << (lane & 0xFu));
                    precise float texel = roundEven(out_[k] * (pow2 - 1.0));
                    precise float rebuilt = (texel * (pow2 + 1.0)) / (pow2 * pow2);
                    if (lane - 1u < 15u) out_[k] = rebuilt;
                }
            }
            for (uint k = 0u; k < 4u; k++) {
                if ((used & (1u << k)) == 0u) continue;
                uint sign_ = bitfieldExtract(scale_word, int(6u * k), 6) >> 4u;
                float v = out_[k];
                precise float q16 = roundEven(v * 65536.0) * (1.0 / 65536.0);
                v = sign_ == 0u ? q16 : v;
                v = sign_ == 2u ? max(v, -1.0) : v;
                out_[k] = v;
            }
        } else {
            // as integers
            bool point = (scale_word & (1u << 26u)) != 0u;
            for (uint k = 0u; k < 4u; k++) {
                if ((used & (1u << k)) == 0u) continue;
                uint lane = bitfieldExtract(scale_word, int(6u * k), 6);
                uint width = lane & 0xFu, sign_ = lane >> 4u;
                bool halved = sign_ - 1u < 2u;
                uint s = width + 1u - (halved ? 1u : 0u);
                float scale = float((1u << s) - 1u);
                precise float offset = min(scale, 0.5) - 0.5;
                precise float v = out_[k] * max(scale, 0.5) + offset;
                out_[k] = point_allowed && point ? roundEven(v) : v;
            }
        }
    }
    return out_;
}

vec4 texture_signs(uvec4 i0, uvec4 i1, vec4 coordinates, vec4 sample_) {
    uint w0 = i0.x;
    uint slot = (w0 >> 20u) & 31u, used = (w0 >> 16u) & 15u;
    if (used == 0u) return sample_;
    uint info = slot_word(7u, slot), signed_entry = slot_word(8u, slot), scale_word = slot_word(12u, slot);
    bool any_signed = false;
    for (uint k = 0u; k < 4u; k++)
        if ((used & (1u << k)) != 0u && bitfieldExtract(info, int(2u * k), 2) == 1u) any_signed = true;
    vec4 signed_sample = sample_;
    if (any_signed && signed_entry != 0u) signed_sample = sample_texture(i0, i1, coordinates, 8u, QUERY_SAMPLE);
    return fetch_post(used, (w0 & (1u << 15u)) != 0u, sample_, signed_sample, info, scale_word);
}
#endif

void texture_fetch(uvec4 i0, uvec4 i1) {
    uint w0 = i0.x;
    uint opcode = (w0 >> 4u) & 31u;
    bool has_result = (w0 & (1u << 30u)) != 0u;
    if ((w0 & 4u) != 0u && p0 != ((w0 & 8u) != 0u)) return;
    // the setters keep state for later fetches
    if (opcode == 24u || opcode == 25u || opcode == 26u) {
        vec4 value = operand(i0.z);
        if (opcode == 24u) tfetch_lod = value.x;
        else if (opcode == 25u) gradient_h = value;
        else gradient_v = value;
        return;
    }
    if (!has_result) return;
    // getBCF: 0, as Xenia
    if (opcode == 16u) {
        store_result(i0.y, vec4(0.0), true);
        return;
    }
    vec4 coordinates = operand(i0.z);
    uint slot = (w0 >> 20u) & 31u;
    vec4 result;
    if (opcode == 18u) {
        // getGradients
#ifdef GENERIC_HARNESS
        precise vec4 g = vec4(coordinates.x * 0.5, coordinates.x * -0.5, coordinates.y * 0.5, coordinates.y * -0.5);
        result = g;
#elif defined(GENERIC_PIXEL)
        result = vec4(dFdxCoarse(coordinates.x), dFdyCoarse(coordinates.x), dFdxCoarse(coordinates.y),
                      dFdyCoarse(coordinates.y));
#else
        result = vec4(0.0);
#endif
    } else {
#ifdef GENERIC_HARNESS
        // shader_harness.h's stand-in, with the LOD and gradient registers
        uint dimension = (w0 >> 9u) & 3u;
        float s = float(slot);
        float cx = coordinates.x, cy = dimension >= 1u ? coordinates.y : 0.0;
        precise float z = cx * cy;
        precise float w = s;
        if ((w0 & (1u << 14u)) != 0u) z = (z + gradient_h.x) + gradient_v.y;
        if ((w0 & (1u << 13u)) != 0u) w = s + tfetch_lod;
        precise float x = cx + s, y = cy - s;
        result = vec4(x, y, z, w);
#else
        if (opcode == 17u) {
#ifdef GENERIC_PIXEL
            bool computed = (w0 & (1u << 12u)) != 0u && (w0 & (1u << 14u)) == 0u;
#else
            bool computed = false;
#endif
            result = computed ? sample_texture(i0, i1, coordinates, 2u, QUERY_LOD) : vec4(0.0);
        } else if (opcode == 19u) {
            result = sample_texture(i0, i1, coordinates, 2u, QUERY_WEIGHTS);
        } else {
            result = texture_signs(i0, i1, coordinates, sample_texture(i0, i1, coordinates, 2u, QUERY_SAMPLE));
            // the fetch constant's exponent adjust (word 3 bits 13-18, signed)
            int adjust = bitfieldExtract(int(fetch_dword(slot * 6u + 3u)), 13, 6);
            precise vec4 scaled = result * ldexp(1.0, adjust);
            result = scaled;
        }
#endif
    }
    store_result(i0.y, result, true);
}

// ---- control flow -------------------------------------------------------------

void run_instruction(uint at) {
    uvec4 i0 = programs.units[at], i1 = programs.units[at + 1u];
    uint kind = i0.x & 3u;
    if (kind == 0u) alu(i0, i1);
    else if (kind == 1u) vertex_fetch(i0, i1);
    else if (kind == 2u) texture_fetch(i0, i1);
}

bool taken(uint x) {
    uint type = (x >> 4u) & 3u;
    bool sense = (x & 64u) != 0u;
    if (type == 0u) return true;
    if (type == 2u) return p0 == sense;
    return bool_constant((x >> 8u) & 255u) == sense;
}

// The control flow as a loop over the next instruction, with the
// translator's four-deep loop and call stacks and its step limit.
void run_program() {
    uint base = program_base;
    uvec4 header = programs.units[base];
    uint n = header.x, code = base + header.y;
    uint pc = 0u, steps = 0u;
    do {
        uint index = pc;
        pc = index + 1u;
        if (index >= n) {
            pc = n;
        } else {
            uvec4 e = programs.units[base + 1u + index];
            uint x = e.x;
            switch (x & 15u) {
            case 1u: // exec
                if (taken(x)) {
                    for (uint k = 0u; k < e.z; k++) run_instruction(code + (e.y + k) * 4u);
                    if ((x & (1u << 16u)) != 0u) return;
                }
                break;
            case 2u: { // loop start: push the count and aL (the start, or aL kept)
                uint constant = loop_constant((x >> 20u) & 31u);
                uint count = constant & 0xFFu;
                int start = int(bitfieldExtract(constant, 8, 8));
                int old_al = loop_address[0];
                for (uint k = 3u; k > 0u; k--) {
                    loop_count[k] = loop_count[k - 1u];
                    loop_address[k] = loop_address[k - 1u];
                }
                loop_count[0] = count;
                int al = (x & (1u << 17u)) != 0u ? old_al : start;
                loop_address[0] = al;
                aL = al;
                pc = count == 0u ? e.y : index + 1u;
                break;
            }
            case 3u: { // loop end: again with the count down and aL a step on, or popped
                uint count = loop_count[0] - 1u;
                bool done = count == 0u;
                if ((x & (1u << 17u)) != 0u) done = done || p0 == ((x & (1u << 18u)) != 0u);
                int step = bitfieldExtract(int(loop_constant((x >> 20u) & 31u)), 16, 8);
                uint counts[4] = loop_count;
                int addresses[4] = loop_address;
                for (uint k = 0u; k < 4u; k++) {
                    uint popped_count = k < 3u ? counts[k + 1u] : 0u;
                    int popped_address = k < 3u ? addresses[k + 1u] : 0;
                    uint again_count = k == 0u ? count : counts[k];
                    int again_address = k == 0u ? addresses[0] + step : addresses[k];
                    loop_count[k] = done ? popped_count : again_count;
                    loop_address[k] = done ? popped_address : again_address;
                }
                aL = loop_address[0];
                pc = done ? index + 1u : e.y;
                break;
            }
            case 4u: // call: a fifth deep is skipped
                if (call_depth < 4u && taken(x)) {
                    call_stack[call_depth] = index + 1u;
                    call_depth = call_depth + 1u;
                    pc = e.y;
                }
                break;
            case 5u: // return
                if (call_depth > 0u) {
                    call_depth = call_depth - 1u;
                    pc = call_stack[call_depth];
                }
                break;
            case 6u: // jump
                pc = taken(x) ? e.y : index + 1u;
                break;
            default: break;
            }
        }
        steps = steps + 1u;
    } while (pc < n && steps <= (1u << 20u));
}

uint program_flags() { return programs.units[program_base].z; }
