// A Vulkan device without a window, for running compute shaders over
// buffers and reading them back: how the microcode translator's output is
// checked on the GPU (plan B2) against the CPU's answer.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace xf::vk {

class Compute {
public:
    // The first device with a compute queue, a discrete GPU first. False,
    // with the reason, when there is no Vulkan 1.3 device.
    static std::unique_ptr<Compute> open(std::string &error);
    ~Compute();

    const std::string &device_name() const { return device_name_; }

    // The shader (entry point "main") sees the buffers as storage buffers at
    // set 0, bindings 0, 1, 2... in order; they are read back after.
    bool run(std::span<const uint32_t> spirv, std::span<std::vector<uint8_t> *const> buffers, uint32_t groups_x,
             std::string &error);

private:
    Compute() = default;
    struct State;
    std::unique_ptr<State> s_;
    std::string device_name_;
};

} // namespace xf::vk
