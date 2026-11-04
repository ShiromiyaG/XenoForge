// A parsed Xenos shader run on the CPU, one case at a time, as
// shader_harness.h lays the cases out: the answer the GPU translation is
// checked against (plan B2). Its semantics are Xenia's: its CPU interpreter
// (shader_interpreter.cc), and its SPIR-V translator where the two disagree.

#pragma once

#include "xenoforge/shader_harness.h"
#include "xenoforge/ucode_parse.h"

#include <cstdint>
#include <span>
#include <string>

namespace xf::shader {

// Runs the shader from the case's registers to its end. False, with what it
// met that it does not do in `unsupported`, for memory exports, texture
// operations other than tfetch, vertex formats it does not know, opcode 41,
// or control flow that does not end.
bool interpret(const ucode::ParsedShader &shader, const harness::CaseInput &input, const harness::Constants &constants,
               std::span<const uint8_t> memory, harness::CaseOutput &output, std::string &unsupported);

// A result rounded to mantissa_bits (1-22) of mantissa, to nearest with ties
// away from zero, never to infinity; infinities and NaNs unchanged. What
// Xenia applies to rcp, rsq, log, exp and sqrt to match the hardware.
float reduce_precision(float value, uint32_t mantissa_bits);

} // namespace xf::shader
