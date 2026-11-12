// A parsed Xenos shader run on the CPU, one case at a time, as
// shader_harness.h lays the cases out: the answer the GPU translation is
// checked against (plan B2). Its semantics are Xenia's: its CPU interpreter
// (shader_interpreter.cc), and its SPIR-V translator where the two disagree.

#pragma once

#include "xenoforge/shader_harness.h"
#include "xenoforge/ucode_parse.h"

#include <cstdint>
#include <memory>
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

// ---- Whole draws (the software reference renderer, reference.h) ----
//
// The same machine, run for a draw's vertices and pixels rather than for a
// harness case: texture operations sampled and memory exports done by an
// Environment, and pixel shaders run as the quads of 2x2 pixels the GPU runs
// them in, so a fetch that takes implicit derivatives sees its neighbours'
// coordinates. The harness's rules above hold but for the texture fetches
// (the Environment's) and the exports (done, not failed).

// A texture operation as the instruction asks it: its fetch constant, the
// coordinates operand's four components (before any normalization), the LOD
// and gradient registers, and in a pixel shader the coordinates of the
// quad's four lanes (0 top left, 1 top right, 2 bottom left, 3 bottom right;
// a lane that did not reach this fetch repeats this one's).
struct TextureFetch {
    const ucode::TextureFetchInstruction *instruction = nullptr;
    const uint32_t *fetch_constant = nullptr; // 6 dwords
    float coordinates[4] = {};
    float lod = 0.0f; // setTexLOD's
    float gradient_h[3] = {}, gradient_v[3] = {};
    bool pixel = false;            // a pixel shader's: quad holds the lanes
    float quad[4][4] = {};
    uint32_t lane = 0;
};

class Environment {
public:
    virtual ~Environment() = default;
    // tfetch, getCompTexLOD, getWeights, getBCF, getGradients: the result
    // before the instruction's destination swizzle. False with the reason
    // when it is not done (the invocation fails).
    virtual bool texture(const TextureFetch &fetch, float result[4], std::string &unsupported) = 0;
    // An export: eA and the eM# written since the last one (a bit each in
    // written, eM0 bit 0), in the order the shader made them.
    virtual void memory_export(const float address[4], const float data[5][4], uint32_t written) = 0;
};

// One invocation of a shader over a whole draw: its registers set by the
// caller (a vertex's index in r0.x; a pixel's interpolators in r0-r15), run
// to its end. A pixel shader's invocation stops at each fetch that takes
// derivatives, Waiting, with the coordinates it needs in waiting(); the
// quad's driver gives the four lanes' with resume() and runs it again.
class Invocation {
public:
    enum class Status { Done, Failed, Waiting };
    Invocation(const ucode::ParsedShader &shader, const harness::Constants &constants, std::span<const uint8_t> memory,
               Environment *environment, bool pixel);
    ~Invocation();
    Invocation(const Invocation &) = delete;
    Invocation &operator=(const Invocation &) = delete;

    float (*registers())[4];
    Status run();
    // where a Waiting invocation stopped (to pair it with its quad's lanes
    // stopped at the same fetch) and the coordinates it needs
    uint64_t waiting_at() const;
    const float *waiting() const;
    void resume(const float quad[4][4], uint32_t lane);
    // after Done: the exports, the colors written on its path (a bit per
    // oC#), whether it was killed
    const harness::CaseOutput &output() const;
    uint32_t colors_written() const;
    bool killed() const;
    const std::string &unsupported() const;

private:
    struct State;
    State *state_;
};

} // namespace xf::shader
