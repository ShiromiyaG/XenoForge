// A parsed Xenos shader as the generic shaders read it (docs/renderer.md, "The
// generic shaders"): src/shaders/generic.glsl, an interpreter of the
// microcode written as a Vulkan vertex, pixel and compute shader, runs any
// shader from a buffer of these programs while its translation's pipeline
// is made. The program is the parse (ucode_parse.h) laid out in words, so
// the GPU decodes nothing ucode_parse resolved (operand swizzles past their
// component count, the scalar operands, a mini fetch's operands), and the
// semantics stay the translator's (shader_translator.cpp), which the
// interpreter follows instruction by instruction.
//
// The layout, in 16-byte units from the program's start:
//   0: the header: x the control flow's length (n), y where the
//      instructions start, z the PROGRAM_* flags, w 0
//   1 to n: the control flow, an entry each (x: CF_* kind in bits 0-3, the
//      condition's type in 4-5 (0 none, 1 a bool constant, 2 the
//      predicate), its sense in 6, the bool constant in 8-15, an exec's end
//      in 16, a loop start's repeat or a loop end's predicated break in 17,
//      that break's sense in 18, the loop constant in 20-24; y: an exec's
//      first instruction, a loop start's skip, a loop end's body, a call's or
//      jump's target; z: an exec's instruction count)
//   then the instructions, four units each (16 words): word 0 the kind in
//      bits 0-1 (0 ALU, 1 vertex fetch, 2 texture fetch, 3 none), predicated
//      in bit 2, its sense in 3, then by kind:
//   - ALU: word 0 bits 4-8 the vector opcode, 9-14 the scalar opcode, 15
//     the vector operation runs (its result is used or it changes state),
//     17-18 the vector operands, 19-20 the scalar operands; word 1 the
//     vector result, 2 the scalar result, 3-5 the vector operands, 6-7 the
//     scalar operands
//   - vertex fetch: word 0 bit 4 a mini fetch, 5-10 the format, 11 signed,
//     12 integer, 13 the index rounded, 14 the NoZero signed mode, 16-23 the
//     fetch constant (0-95); word 1 the result, 2 the index operand, 3 the
//     offset (signed), 4 the stride, 5 the exponent adjust (signed)
//   - texture fetch: word 0 bits 4-8 the opcode (FetchOpcode), 9-10 the
//     dimension, 11 unnormalized coordinates, 12 the computed LOD, 13 the
//     register LOD, 14 the register gradients, 15 point sampling allowed,
//     16-19 the result's components that are not always zero, 20-24 the
//     fetch constant, 25 a 1D fetch with one coordinate, 26-27 and 28-29 the
//     volume's magnifying and minifying filters, 30 it has a result; word 1
//     the result, 2 the coordinates operand, 3 the LOD bias, 4-6 the offsets
//     (floats' bits)
// A result: the StorageTarget in bits 0-3, the index in 4-10, the
// addressing in 11-12, saturated in 13, the used write mask in 14-17, the
// four components' Swizzle in 18-29 (three bits each). An operand: the index
// in bits 0-8, a float constant (not a register) in 9, the addressing in
// 10-11, negated in 12, absolute in 13, the four components (two bits each,
// past the count the last repeated) in 14-21.

#pragma once

#include "xenoforge/ucode_parse.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xf::shader {

constexpr uint32_t CF_NOP = 0, CF_EXEC = 1, CF_LOOP_START = 2, CF_LOOP_END = 3, CF_CALL = 4, CF_RETURN = 5,
                   CF_JUMP = 6;
// the header's flags: what a stage's main needs to know (the translator's
// scans of the whole shader)
constexpr uint32_t PROGRAM_WRITES_DEPTH = 1;      // an ALU result to oDepth
constexpr uint32_t PROGRAM_KILLS = 2;             // a kill
constexpr uint32_t PROGRAM_WRITES_COLOR_0 = 4;    // oC0 written somewhere
constexpr uint32_t PROGRAM_WRITES_POINT_SIZE = 8; // oPts.x written somewhere
constexpr uint32_t PROGRAM_COLORS_SHIFT = 4;      // bits 4-7: the color targets written somewhere

struct Program {
    std::vector<uint32_t> words; // 16-byte units, four words each
    uint32_t flags = 0;
    // what the generic shaders cannot run (empty: it runs): memory exports,
    // vertex formats and texture operations the translator does not do,
    // scalar opcode 41; the first reason
    std::string unsupported;
    bool ok() const { return unsupported.empty(); }
};

Program encode_program(const ucode::ParsedShader &shader);

} // namespace xf::shader
