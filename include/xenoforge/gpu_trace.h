// Xenia's GPU traces (.xtr): what the GPU was sent over a frame (or a
// stream), with the memory and registers it read. The format is Xenia's
// src/xenia/gpu/trace_protocol.h (BSD; see LICENSE-xenia), version 1;
// xenia-canary writes them with --trace_gpu_frames.

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace xf {

struct GpuTrace {
    uint32_t version = 0;
    std::string commit; // Xenia's, that recorded it
    uint32_t title_id = 0;
    std::vector<uint8_t> data; // the whole file
};

enum class TraceCommand : uint32_t {
    PrimaryBufferStart,
    PrimaryBufferEnd,
    IndirectBufferStart,
    IndirectBufferEnd,
    PacketStart,
    PacketEnd,
    MemoryRead,
    MemoryWrite,
    EdramSnapshot,
    Event,
    Registers,
    GammaRamp,
};

// What a walk over a trace hands out, in the order recorded. Spans live only
// for the call. Packets are the guest's words (big-endian), handed out at
// their end, after the memory they read (as Xenia's player executes them);
// memory addresses are physical.
class TraceVisitor {
public:
    virtual ~TraceVisitor() = default;
    virtual void packet(uint32_t /*address*/, std::span<const uint8_t> /*words*/) {}
    virtual void memory_read(uint32_t /*address*/, std::span<const uint8_t> /*bytes*/) {}
    virtual void memory_write(uint32_t /*address*/, std::span<const uint8_t> /*bytes*/) {}
    virtual void registers(uint32_t /*first*/, std::span<const uint32_t> /*values*/) {}
    virtual void edram_snapshot(std::span<const uint8_t> /*bytes*/) {}
    virtual void event(uint32_t /*type*/) {} // 0: swap
    // the display's gamma ramps: the 256-entry table (DC_LUT_30_COLOR:
    // blue, green, red in 10 bits each from bit 0) and the piecewise linear
    // one (DC_LUT_PWL_DATA, 128 per red, green, blue)
    virtual void gamma_ramp(std::span<const uint32_t> /*table*/, std::span<const uint32_t> /*pwl*/) {}
    // primary and indirect buffers' starts and ends
    virtual void buffer(TraceCommand /*command*/, uint32_t /*address*/) {}
};

bool open_gpu_trace(const std::filesystem::path &path, GpuTrace &out, std::string &error);

// Every length checked against the file; snappy data decompressed.
bool walk_gpu_trace(const GpuTrace &trace, TraceVisitor &visitor, std::string &error);

} // namespace xf
