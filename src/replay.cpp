/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// A trace replayed through the command processor into the renderer
// (renderer.h's replay), as Xenia's trace player feeds its command
// processor (src/xenia/gpu/trace_player.cc, PlayTraceOnThread; BSD, see
// LICENSE-xenia), checked against it: each packet written to guest memory
// at its address and executed there on its own (the packets of an indirect
// buffer one by one, the buffer's own packet giving way to them); the
// memory it read written before it runs, and what it wrote skipped (the
// processor writes its own); the registers and gamma ramps restored; the
// EDRAM snapshot given to the renderer. Where it differs:
//
// - The whole trace plays, not one frame of it up to its swap.
// - A packet's words are written after the memory it read (the walk hands
//   a packet out at its end; Xenia writes them at its start, before).
// - Xenia's Vulkan backend drops the EDRAM snapshot (its Direct3D 12 one
//   restores it); the renderer starts from it.
// - The processor's host raises no interrupts, and a WAIT_REG_MEM that the
//   recorded memory does not satisfy ends its packet at once (Xenia's would
//   poll forever).

#include "xenoforge/renderer.h"

#include "xenoforge/memory.h"

#include <algorithm>
#include <cstring>

namespace xf::render {
namespace {

// What the processor asks of the system, during a replay: nothing happens
// between two polls, so a wait that does not match never will.
class TraceHost final : public gpu::CommandProcessor::Host {
public:
    void interrupt(uint32_t) override {}
    bool wait(uint32_t) override { return false; }
};

class Player final : public TraceVisitor {
public:
    Player(Memory &memory, gpu::CommandProcessor &processor, Renderer &renderer, bool restore_edram)
        : memory_(memory), processor_(processor), renderer_(renderer), restore_edram_(restore_edram) {}

    void packet(uint32_t address, std::span<const uint8_t> words) override {
        uint32_t count = uint32_t(words.size() / 4);
        if (!count) return;
        write(address, words);
        processor_.execute_packets(address, count);
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override {
        write(address, bytes);
        renderer_.memory_written(address & 0x1FFFFFFF, uint32_t(bytes.size()));
    }
    void registers(uint32_t first, std::span<const uint32_t> values) override {
        processor_.restore_registers(first, values);
    }
    void edram_snapshot(std::span<const uint8_t> bytes) override {
        if (restore_edram_) renderer_.restore_edram(bytes);
    }
    void gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t> pwl) override {
        processor_.restore_gamma_ramp(table, pwl);
    }

private:
    // physical memory, wrapping at its end as the address mask does
    void write(uint32_t address, std::span<const uint8_t> bytes) {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t at = (address + uint32_t(i)) & 0x1FFFFFFF;
            size_t n = std::min<size_t>(0x20000000 - at, bytes.size() - i);
            std::memcpy(memory_.translate_physical(at), bytes.data() + i, n);
            i += n;
        }
    }

    Memory &memory_;
    gpu::CommandProcessor &processor_;
    Renderer &renderer_;
    bool restore_edram_;
};

} // namespace

bool replay(const GpuTrace &trace, vk::Device &device, const Options &options, Image &frame, Report &report,
            std::string &error, bool restore_edram) {
    // the console's memory, all of its physical memory committed, as Xenia's
    // player allocates it
    std::unique_ptr<Memory> memory = Memory::create(error);
    if (!memory) return false;
    TraceHost host;
    gpu::CommandProcessor processor(*memory, host);
    Options o = options;
    if (!o.frame_wanted) o.frame_wanted = [](uint64_t) { return true; };
    o.on_frame = [&, user = options.on_frame](uint64_t swap, const Image &image) {
        frame = image;
        if (user) user(swap, image);
    };
    std::unique_ptr<Renderer> renderer = Renderer::create(device, processor, o, error);
    if (!renderer) return false;
    processor.set_sink(renderer.get());
    Player player(*memory, processor, *renderer, restore_edram);
    bool walked = walk_gpu_trace(trace, player, error);
    processor.set_sink(nullptr);
    renderer->drain(); // the front buffers asked for, read back
    report = renderer->report();
    report.packets = processor.counts().packets.load();
    if (!walked) return false;
    return !renderer->failed(error);
}

} // namespace xf::render
