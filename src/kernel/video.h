// The video family's state (video.cpp): the GPU as the kernel runs it. The
// command processor (gpu/command_processor.h) executes on a kernel thread of
// its own, "GPU commands", woken by the CPU's writes to CP_RB_WPTR; a second
// one, "GPU vblank", ticks 60 times a second. Both are guest threads with a
// stack, a PCR and a KTHREAD (Xenia's XHostThreads, in the idle process),
// and the guest's graphics interrupt callback runs on whichever raises the
// interrupt, impersonating a DPC on the interrupt's CPU (Xenia's
// EmulateCPInterruptDPC).

#pragma once

#include "objects.h"
#include "xenoforge/command_processor.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xf::kernel {

// the display, as VdQueryVideoMode reports it
struct VideoMode {
    uint32_t width = 1280, height = 720;
    bool widescreen = true;
    float refresh = 60.0f;
    uint32_t standard = 1; // X_AV_VIDEO_STANDARD: 1 NTSC-M, 2 NTSC-J, 3 PAL
};

class Video final : public gpu::CommandProcessor::Host {
public:
    // the display from the console's settings
    VideoMode mode();

    // as the kernel starts: the command processor, the registers' MMIO range
    void setup(Kernel &k);
    // the two threads, once (the first call into the video driver)
    void start();
    // the threads stopped and joined, their objects let go
    void stop();

    // the command processor's requests, on its thread
    void interrupt(uint32_t cpu) override;
    bool wait(uint32_t wait) override;

    // the CPU's MMIO (Xenia's GraphicsSystem::ReadRegister, WriteRegister)
    uint32_t read_register(uint32_t address);
    void write_register(uint32_t address, uint32_t value);

    // The guest's graphics interrupt callback (VdSetGraphicsInterruptCallback)
    // with r3 = source (0 vblank, 1 the command processor), r4 = its data, on
    // the calling GPU thread's context, as a DPC on cpu.
    void dispatch_interrupt(Thread &self, uint32_t source, uint32_t cpu);

    Kernel *k = nullptr;
    std::unique_ptr<gpu::CommandProcessor> cp;
    std::atomic<uint32_t> write_pointer{0}; // CP_RB_WPTR, in dwords
    std::atomic<uint32_t> callback{0}, callback_data{0};
    std::atomic<bool> running{false};
    // VdInitializeScalerCommandBuffer's output size (the presentation's aspect)
    std::atomic<uint32_t> scaled_width{0}, scaled_height{0};

    // counts: vblanks, interrupts delivered by source
    std::atomic<uint64_t> vblanks{0};
    std::array<std::atomic<uint64_t>, 2> interrupts{};
    // the swaps executed so far, sampled once a second: (milliseconds since
    // the kernel started, swaps)
    std::mutex samples_mutex;
    std::vector<std::pair<uint32_t, uint64_t>> swap_samples;

    Ref<Thread> commands, vblank;

private:
    void commands_loop(Thread &self);
    void vblank_loop(Thread &self);
    void mark_vblank(Thread &self);
    void release();
    static void set_event(Objects &o, Event &e);
    uint32_t setting32(uint16_t category, uint16_t id, uint32_t fallback);

    std::mutex settings_mutex_;
    uint32_t settings_buffer_ = 0; // guest memory ExGetXConfigSetting writes to

    std::once_flag started_;
    bool commands_started_ = false, vblank_started_ = false;
    Ref<Event> kick_, stop_;
    std::atomic<Event *> kick_event_{nullptr}; // kick_ once the threads run, for the MMIO writes
    // interrupts on one CPU run one at a time, as DPCs on a processor do
    std::array<std::mutex, 6> cpu_dpcs_;
};

// the video family's state, its threads started
Video &video(Kernel &k);
// the same, the threads as they are
Video &video_state(Kernel &k);

} // namespace xf::kernel
