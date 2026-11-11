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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void *, unsigned long long *);
#else
#include <time.h>
#endif

namespace xf::render {
namespace {

// The calling thread's processor time so far: what a repeated play's
// commands cost the thread, without what it waits for (the GPU, a
// pipeline). In ticks: the processor's cycles on Windows (the time stamp
// counter's, counted while the thread runs), nanoseconds elsewhere;
// cpu_ticks_a_second says how many make a second.
uint64_t thread_cpu_ticks() {
#ifdef _WIN32
    unsigned long long cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}
double cpu_ticks_a_second() {
#ifdef _WIN32
    // (the counter's rate against the clock, over a busy twentieth of a second)
    static const double rate = [] {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = thread_cpu_ticks();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) {}
        const uint64_t c1 = thread_cpu_ticks();
        return double(c1 - c0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }();
    return rate;
#else
    return 1e9;
#endif
}

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
        if (!repeating_) {
            processor_.execute_packets(address, count);
            return;
        }
        auto start = std::chrono::steady_clock::now();
        const uint64_t ticks = thread_cpu_ticks();
        processor_.execute_packets(address, count);
        command_ticks_ += thread_cpu_ticks() - ticks;
        command_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override {
        // a repeated play: what memory holds already is not written again
        // (a game's frame drawn again finds its memory as it left it)
        if (repeating_ && unchanged(address, bytes)) return;
        write(address, bytes);
        renderer_.memory_written(address & 0x1FFFFFFF, uint32_t(bytes.size()));
    }
    void registers(uint32_t first, std::span<const uint32_t> values) override {
        processor_.restore_registers(first, values);
    }
    void edram_snapshot(std::span<const uint8_t> bytes) override {
        // a repeated play keeps EDRAM as the last one left it, as a game's
        // next frame finds it
        if (restore_edram_ && !repeating_) renderer_.restore_edram(bytes);
    }
    void gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t> pwl) override {
        processor_.restore_gamma_ramp(table, pwl);
    }

    bool repeating_ = false;
    double command_seconds_ = 0; // in the processor, over the repeated plays
    uint64_t command_ticks_ = 0; // the thread's processor time in it (thread_cpu_ticks)

private:
    bool unchanged(uint32_t address, std::span<const uint8_t> bytes) const {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t at = (address + uint32_t(i)) & 0x1FFFFFFF;
            size_t n = std::min<size_t>(0x20000000 - at, bytes.size() - i);
            if (std::memcmp(memory_.translate_physical(at), bytes.data() + i, n) != 0) return false;
            i += n;
        }
        return true;
    }
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
            std::string &error, bool restore_edram, uint32_t repeat) {
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
    processor.set_occlusion_queries(options.occlusion_queries);
    processor.set_screen_extents(options.screen_extents);
    Player player(*memory, processor, *renderer, restore_edram);
    bool walked = walk_gpu_trace(trace, player, error);
    // the plays after the first, each finished on the GPU: the GPU time that
    // of the fastest one (other work on the GPU only ever adds to a play's)
    std::map<std::string, Report::GpuTime> fastest_gpu_time;
    double fastest_gpu_seconds = -1;
    std::vector<double> command_seconds;
    double command_cpu_seconds = 0;
    if (walked && repeat > 1) {
        renderer->drain();
        Report before = renderer->report();
        player.repeating_ = true;
        uint64_t ticks = 0;
        for (uint32_t k = 1; k < repeat && walked; k++) {
            player.command_seconds_ = 0;
            walked = walk_gpu_trace(trace, player, error);
            command_seconds.push_back(player.command_seconds_);
            ticks += player.command_ticks_, player.command_ticks_ = 0;
            command_cpu_seconds = double(ticks) / cpu_ticks_a_second() / double(k);
            renderer->drain();
            Report after = renderer->report();
            double seconds = after.gpu_seconds - before.gpu_seconds;
            if (fastest_gpu_seconds < 0 || seconds < fastest_gpu_seconds) {
                fastest_gpu_seconds = seconds;
                fastest_gpu_time.clear();
                for (const auto &[what, t] : after.gpu_time) {
                    Report::GpuTime play = t;
                    if (auto it = before.gpu_time.find(what); it != before.gpu_time.end())
                        play.seconds -= it->second.seconds, play.intervals -= it->second.intervals;
                    if (play.intervals) fastest_gpu_time[what] = play;
                }
            }
            before = std::move(after);
        }
    }
    processor.set_sink(nullptr);
    renderer->drain(); // the front buffers asked for, read back
    report = renderer->report();
    if (fastest_gpu_seconds >= 0) {
        report.gpu_time = std::move(fastest_gpu_time);
        report.gpu_seconds = fastest_gpu_seconds;
    }
    report.packets = processor.counts().packets.load();
    report.command_seconds = std::move(command_seconds);
    report.command_cpu_seconds = command_cpu_seconds;
    // XF_REPLAY_DUMP_MEMORY=<hex address>:<hex size>:<file>: that physical
    // memory after the trace, to set beside Xenia's and the reference's
    // (XF_REF_DUMP_MEMORY). What resolves wrote is in it with
    // Options::readback_resolve full (xfreplay --readback-resolve full).
    if (const char *dump = std::getenv("XF_REPLAY_DUMP_MEMORY")) {
        unsigned address = 0, size = 0;
        char path[512] = {};
        if (std::sscanf(dump, "%x:%x:%511[^\n]", &address, &size, path) == 3)
            if (FILE *f = std::fopen(path, "wb")) {
                std::fwrite(memory->translate_physical(address), 1, size, f);
                std::fclose(f);
            }
    }
    if (!walked) return false;
    return !renderer->failed(error);
}

} // namespace xf::render
