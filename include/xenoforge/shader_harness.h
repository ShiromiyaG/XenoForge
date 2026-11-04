// How a shader is run for checking: the buffers the translator's compute
// wrapper reads and writes, which the CPU interpreter fills the same way.
// One case per invocation; a case is a shader run from given registers to
// its exports. The GPU's answer and the CPU's are then compared (plan B2,
// docs/renderer.md "The translated shader").
//
// The rules both sides follow besides the microcode's own:
// - a0, aL, the predicate and the previous scalar (ps) start at 0, 0, false
//   and 0;
// - a register index relative to aL wraps to 0-127, as Xenia's interpreter
//   does; a float constant index (absolute or relative) outside 0-255 reads
//   zeros;
// - a kill sets `killed` and the shader goes on to its end;
// - a texture fetch does not sample: with s its fetch constant (0-31) as a
//   float and c its coordinates (the components the dimension has, the
//   others 0), it returns (c.x + s, c.y - s, c.x * c.y, s), the multiply
//   without contraction; the instruction's destination swizzle applies;
// - a vertex fetch reads `memory` (guest physical address 0 is its first
//   byte) as the fetch constant describes;
// - exports go to the case's output: oPos, oPts, o0-o15 for a vertex shader,
//   oC0-oC3 and oDepth for a pixel shader; components never written stay 0;
// - registers are r0-r127 in, r0-r127 out.

#pragma once

#include <cstdint>

namespace xf::harness {

constexpr uint32_t REGISTERS = 128;
constexpr uint32_t INTERPOLATORS = 16;
constexpr uint32_t LOCAL_SIZE = 64; // invocations per group; cases are padded to it

// binding 0, read: per case, the registers it starts with
struct CaseInput {
    float registers[REGISTERS][4];
};

// binding 1, read: shared by the cases
struct Constants {
    float float_constants[256][4]; // the stage's c0-c255
    uint32_t bool_constants[8];    // b0-b255, a bit each
    uint32_t loop_constants[32];   // i0-i31
    uint32_t fetch_constants[32 * 6];
};

// binding 2, read: guest memory for vertex fetches, as bytes (big-endian words)

// binding 3, written: per case
struct CaseOutput {
    float registers[REGISTERS][4];
    float interpolators[INTERPOLATORS][4]; // o0-o15
    float position[4];                     // oPos
    float point_size[4];                   // oPts: size, edge flag, kill vertex
    float colors[4][4];                    // oC0-oC3
    float depth[4];                        // oDepth in x
    uint32_t killed;
    uint32_t predicate;        // at the end
    int32_t address_register;  // a0 at the end
    uint32_t padding;
};

static_assert(sizeof(CaseInput) == REGISTERS * 16);
static_assert(sizeof(Constants) == 4096 + 32 + 128 + 768);
static_assert(sizeof(CaseOutput) % 16 == 0);

} // namespace xf::harness
