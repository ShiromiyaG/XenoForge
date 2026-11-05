// The Xbox 360's compiled shaders as the XDK's compiler stores them: a
// container (magic 0x102A11xx, low bit set for a vertex shader) with a
// virtual part (constant table, definitions, the shader's header) and a
// physical part (the microcode). Found in effect binaries (XNA's, the
// games' own) and in game data. The layout follows XenosRecomp's shader.h
// (hedge-dev, MIT), which reads the same containers.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace xf {

struct ShaderContainer {
    size_t offset = 0;     // in the data searched
    bool vertex = false;
    uint32_t flags = 0;
    std::vector<uint32_t> microcode; // host order
};

// Every container in the data (searched a word at a time), with sizes that
// fit and microcode inside it.
std::vector<ShaderContainer> find_shader_containers(std::span<const uint8_t> data);

} // namespace xf
