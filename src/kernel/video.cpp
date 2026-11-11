/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The video family (plan step A4, "GPU live"; docs/runtime.md, "The GPU"):
// the GPU's registers as MMIO, the command processor on its thread, vblank,
// the graphics interrupt, and the Vd* exports with xam's XGetVideoMode.
// Translated from Xenia (BSD; see LICENSE-xenia): gpu/graphics_system.cc
// (ReadRegister, WriteRegister, the frame limiter's vblank, MarkVblank,
// DispatchInterruptCallback), gpu/command_processor.cc (WorkerThreadMain,
// UpdateWritePointer), kernel/kernel_state.cc (EmulateCPInterruptDPC,
// Begin/EndDPCImpersonation), kernel/xboxkrnl/xboxkrnl_video.cc and
// kernel/xam/xam_video.cc (the exports); checked against them. Where it
// differs:
//
// - The command processor's thread waits on a kernel event the CP_RB_WPTR
//   writes set (Xenia: an auto-reset host event, after spinning 500 times),
//   and the threads sleep on the scheduler's clock, so the deterministic
//   mode can run them as it runs the guest's.
// - Vblank comes 60 times a second on that clock, paced to a deadline with
//   the missed beats delivered up to a second behind (Xenia's Linux branch;
//   its Windows one spins the last 10% of each period), and
//   D1MODE_V_COUNTER moves once a vblank (Xenia's Windows branch moves it
//   on every turn of its spin).
// - Interrupts on one CPU are delivered one at a time, as DPCs on a
//   processor run (Xenia lets the vblank and the command processor's run
//   at once).
// - The threads start with the first call into the video driver, not with
//   the emulator.

#include "video.h"

#include "xenoforge/xenos.h"

#include <chrono>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace xf::kernel {
namespace {

using namespace xenos;

constexpr uint32_t GPU_REGISTERS = 0x7FC80000, GPU_REGISTERS_MASK = 0xFFFF0000;
constexpr uint64_t SECOND = 10'000'000; // the scheduler's clock: 100 ns
constexpr uint64_t VBLANK_HZ = 60;
constexpr uint32_t STACK = 128 * 1024; // Xenia's XHostThreads'

uint32_t float_bits(float f) { return std::bit_cast<uint32_t>(f); }

// X_VIDEO_MODE (0x30)
void write_video_mode(Kernel &k, uint32_t address) {
    VideoMode m = video_state(k).mode();
    std::memset(k.host(address), 0, 0x30);
    k.store32(address + 0x00, m.width);
    k.store32(address + 0x04, m.height);
    k.store32(address + 0x08, 0); // interlaced
    k.store32(address + 0x0C, m.widescreen);
    k.store32(address + 0x10, m.width >= 0x500); // high definition
    k.store32(address + 0x14, float_bits(m.refresh));
    k.store32(address + 0x18, m.standard);
    k.store32(address + 0x1C, 0x8A); // pixel rate
    k.store32(address + 0x20, m.widescreen ? 1 : 3);
}

} // namespace

// ---- the GPU's state ----

Video &video_state(Kernel &k) { return k.state<Video>(); }

// A setting as a big-endian word, or the fallback without one: through
// ExGetXConfigSetting, as the console's video driver reads them.
uint32_t Video::setting32(uint16_t category, uint16_t id, uint32_t fallback) {
    static const ExportFn get = find_export_fn("xboxkrnl", "ExGetXConfigSetting");
    if (!get || !settings_buffer_) return fallback;
    std::lock_guard<std::mutex> l(settings_mutex_);
    ppc::Context c{};
    k->attach(c);
    c.r[3] = category, c.r[4] = id, c.r[5] = settings_buffer_, c.r[6] = 4, c.r[7] = settings_buffer_ + 4;
    get(*k, c);
    if (uint32_t(c.r[3]) != X_STATUS_SUCCESS || k->load16(settings_buffer_ + 4) != 4) return fallback;
    return k->load32(settings_buffer_);
}

// Xenia's VdQueryVideoMode, from the settings: the HDMI/composite screen
// size, widescreen by its aspect or the video flags, the AV region's
// standard and refresh rate
VideoMode Video::mode() {
    VideoMode m;
    uint32_t screen = setting32(3, 0x13, 1280u << 16 | 720); // XCONFIG_USER_AV_COMPOSITE_SCREENSZ
    m.width = screen >> 16, m.height = screen & 0xFFFF;
    uint32_t divisor = std::max(1u, std::gcd(m.width, m.height));
    bool four_three = m.width / divisor == 4 && m.height / divisor == 3;
    m.widescreen = !four_three || (setting32(3, 0x0A, 0) & 0x00010000); // XCONFIG_USER_VIDEO_FLAGS
    uint32_t av_region = setting32(2, 0x02, 0x00400100);                // XCONFIG_SECURED_AV_REGION
    m.standard = (av_region & 0xFF00) >> 8;
    if (m.standard == 4) m.standard = 3; // PAL-50 is PAL at 50 Hz
    if (m.standard < 1 || m.standard > 4) m.standard = 1;
    m.refresh = (av_region >> 23) & 1 ? 50.0f : 60.0f;
    return m;
}

void Video::setup(Kernel &kernel) {
    k = &kernel;
    settings_buffer_ = kernel.memory().system_alloc(8);
    cp = std::make_unique<gpu::CommandProcessor>(kernel.memory(), *this);
    // the registers, 0x7FC80000-0x7FC8FFFF (Xenia's AddVirtualMappedRange)
    Kernel::Mmio range;
    range.base = GPU_REGISTERS, range.mask = GPU_REGISTERS_MASK, range.device = this;
    range.read = [](void *v, uint32_t address) { return static_cast<Video *>(v)->read_register(address); };
    range.write = [](void *v, uint32_t address, uint32_t value) {
        static_cast<Video *>(v)->write_register(address, value);
    };
    kernel.add_mmio(range);
}

void Video::set_event(Objects &o, Event &e) {
    std::lock_guard<std::mutex> l(o.lock);
    e.set_signal_state(1);
    o.signaled(e);
}

void Video::start() {
    std::call_once(started_, [this] {
        Objects &o = objects(*k);
        // kick_ wakes the command processor (Xenia's write_ptr_index_event_,
        // auto-reset), stop_ every wait of the GPU's threads
        kick_ = o.create<Event>(GuestType::EventSynchronization, header::SIZE);
        stop_ = o.create<Event>(GuestType::EventNotification, header::SIZE);
        // kernel threads in the idle process, which the guest globals lay
        // out just before the title's (modules.cpp)
        Thread::Params p;
        p.stack_size = STACK;
        p.process = title_process(*k) - kprocess::SIZE;
        p.host_arg = this;
        uint32_t status = X_STATUS_NO_MEMORY;
        if (kick_ && stop_) {
            p.host = [](Thread &self, void *v) { static_cast<Video *>(v)->commands_loop(self); };
            p.name = "GPU commands";
            commands = o.create_thread(p, status);
            p.host = [](Thread &self, void *v) { static_cast<Video *>(v)->vblank_loop(self); };
            p.name = "GPU vblank";
            if (commands) vblank = o.create_thread(p, status);
        }
        if (!commands || !vblank) {
            std::fprintf(stderr, "video: the GPU's threads were not made (status 0x%08X)\n", status);
            release();
            return;
        }
        kick_event_ = kick_.get();
        running = true;
        commands_started_ = o.scheduler->start(*commands);
        vblank_started_ = o.scheduler->start(*vblank);
        if (!commands_started_ || !vblank_started_) std::fprintf(stderr, "video: no host thread for the GPU\n");
    });
}

void Video::stop() {
    if (!k) return;
    Objects &o = objects(*k);
    if (running.exchange(false)) {
        kick_event_ = nullptr;
        set_event(o, *kick_);
        set_event(o, *stop_);
        if (commands_started_) o.scheduler->join(*commands);
        if (vblank_started_) o.scheduler->join(*vblank);
    }
    release();
}

// the handles the threads and events were made with, then the references
// (after the lock: the last one deletes)
void Video::release() {
    Objects &o = objects(*k);
    {
        std::lock_guard<std::mutex> l(o.lock);
        for (Object *object : {static_cast<Object *>(commands.get()), static_cast<Object *>(vblank.get()),
                               static_cast<Object *>(kick_.get()), static_cast<Object *>(stop_.get())})
            if (object && object->handle()) o.release_handle(object->handle());
    }
    commands = {}, vblank = {}, kick_ = {}, stop_ = {};
}

Video &video(Kernel &k) {
    Video &v = k.state<Video>();
    v.start();
    return v;
}

void setup_video(Kernel &k) { k.state<Video>().setup(k); }
void shutdown_video(Kernel &k) { k.state<Video>().stop(); }
gpu::CommandProcessor &gpu_command_processor(Kernel &k) { return *k.state<Video>().cp; }

// ---- the registers (GraphicsSystem::ReadRegister, WriteRegister) ----

uint32_t Video::read_register(uint32_t address) {
    uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
    case reg::RB_EDRAM_TIMING: return 0x08100748;
    case reg::RB_BC_CONTROL: return 0x0000200E;
    case reg::RB_HSIO_INTERFACE_ALIGNER_VALUE: return 0x00BBBBBB;
    case reg::D1MODE_VBLANK_VLINE_STATUS: return 1;    // the interrupt status: vblank
    case reg::D1MODE_VIEWPORT_SIZE: return 0x050002D0; // 1280x720
    default: return cp->registers()[r];
    }
}

void Video::write_register(uint32_t address, uint32_t value) {
    uint32_t r = (address & 0xFFFF) / 4;
    cp->registers().set(r, value);
    if (r == reg::CP_RB_WPTR) {
        // the CPU has put packets in the ring buffer up to here (Xenia's
        // UpdateWritePointer): the command processor's thread goes on
        write_pointer.store(value);
        if (Event *e = kick_event_.load()) set_event(objects(*k), *e);
    }
}

// ---- the command processor's thread (WorkerThreadMain) ----

void Video::commands_loop(Thread &self) {
    Objects &o = objects(*k);
    Dispatcher *const kick[] = {kick_.get()};
    while (running.load()) {
        uint32_t write = write_pointer.load();
        if (write == cp->read_pointer()) {
            o.wait(self, kick, false, false, Scheduler::NEVER);
            continue;
        }
        busy.store(true, std::memory_order_relaxed);
        const auto began = std::chrono::steady_clock::now();
        cp->run(write);
        cp->counts().run_ns.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count()),
            std::memory_order_relaxed);
        busy.store(write_pointer.load() != cp->read_pointer(), std::memory_order_relaxed);
        wrote_back(); // (the last of the run: the spins see the processor idle)
    }
}

// what the CPU polls for changed: the threads asleep in a spin look again
void Video::wrote_back() {
    write_backs.fetch_add(1, std::memory_order_release);
    objects(*k).scheduler->spin_wake();
}

void Video::interrupt(uint32_t cpu) {
    if (Thread *self = current_thread()) dispatch_interrupt(*self, 1, cpu);
}

bool Video::wait(uint32_t wait) {
    Thread *self = current_thread();
    Objects &o = objects(*k);
    const auto began = std::chrono::steady_clock::now();
    if (wait >= 0x100 && self) {
        Dispatcher *const stop[] = {stop_.get()};
        o.wait(*self, stop, false, false, o.now() + uint64_t(wait / 0x100) * (SECOND / 1000));
    } else {
        o.scheduler->yield();
    }
    cp->counts().wait_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count()),
        std::memory_order_relaxed);
    return running.load();
}

// ---- vblank (the frame limiter's thread, MarkVblank) ----

void Video::vblank_loop(Thread &self) {
    Objects &o = objects(*k);
    Dispatcher *const stop[] = {stop_.get()};
    uint64_t start = o.now(), beats = 0;
    while (running.load()) {
        uint64_t due = start + (beats + 1) * SECOND / VBLANK_HZ, now = o.now();
        if (now > due + SECOND) {
            // a freeze: the beats it missed are given up
            start = now, beats = 0;
            continue;
        }
        if (now < due) o.wait(self, stop, false, false, due);
        if (!running.load()) break;
        beats++;
        mark_vblank(self);
    }
}

void Video::mark_vblank(Thread &self) {
    gpu::RegisterFile &regs = cp->registers();
    regs.set(reg::D1MODE_V_COUNTER, regs.get(reg::D1MODE_V_COUNTER) + mode().height);
    // the counter EVENT_WRITE_SHD can write: the game sees the GPU go on
    cp->increment_counter();
    uint64_t n = vblanks.fetch_add(1) + 1;
    if (n % VBLANK_HZ == 0) {
        std::lock_guard<std::mutex> l(samples_mutex);
        swap_samples.push_back({k->uptime_ms(), cp->counts().swaps.load()});
    }
    dispatch_interrupt(self, 0, 2);
}

// ---- the interrupt (EmulateCPInterruptDPC) ----

void Video::dispatch_interrupt(Thread &self, uint32_t source, uint32_t cpu) {
    uint32_t fn = callback.load(), data = callback_data.load();
    if (!fn) return;
    std::lock_guard<std::mutex> dpc(cpu_dpcs_[cpu % 6]);
    uint32_t pcr = self.pcr;
    uint8_t *p = k->host(pcr);
    // on the interrupt's CPU (SetActiveCpu: a host thread's PCR), at
    // DISPATCH_LEVEL with the PCR saying a DPC runs, as the title's
    p[kpcr::CURRENT_CPU] = uint8_t(cpu);
    uint8_t irql = p[kpcr::CURRENT_IRQL];
    p[kpcr::CURRENT_IRQL] = 2;
    k->store32(pcr + kpcr::DPC_ACTIVE, 1);
    p[kpcr::PROCESS_TYPE_IN_DPC] = kprocess::TITLE;
    objects(*k).call(*self.context, fn, {source, data});
    p[kpcr::PROCESS_TYPE_IN_DPC] = kprocess::IDLE;
    p[kpcr::CURRENT_IRQL] = irql;
    k->store32(pcr + kpcr::DPC_ACTIVE, 0);
    interrupts[source & 1].fetch_add(1);
    wrote_back(); // (the callback may have set what a spin waits for)
}

// ---- the report ----

std::string video_report(Kernel &k) {
    Video &v = k.state<Video>();
    if (!v.cp) return {};
    const gpu::Counts &n = v.cp->counts();
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "gpu: %llu packets (%llu ring buffer runs, %llu indirect buffers), %llu draws, %llu resolves, "
                  "%llu swaps\n",
                  (unsigned long long)n.packets.load(), (unsigned long long)n.primary_buffers.load(),
                  (unsigned long long)n.indirect_buffers.load(), (unsigned long long)n.draws.load(),
                  (unsigned long long)n.resolves.load(), (unsigned long long)n.swaps.load());
    out += line;
    // swaps per second: since the first second with a swap, and over the
    // last five seconds sampled
    std::vector<std::pair<uint32_t, uint64_t>> samples;
    {
        std::lock_guard<std::mutex> l(v.samples_mutex);
        samples = v.swap_samples;
    }
    auto first = std::find_if(samples.begin(), samples.end(), [](const auto &s) { return s.second > 0; });
    if (first != samples.end() && samples.back().first > first->first) {
        const auto &last = samples.back();
        const auto &recent = samples[samples.size() > 5 ? samples.size() - 6 : 0];
        double overall = double(last.second - first->second) * 1000.0 / (last.first - first->first);
        double lately = last.first > recent.first
                            ? double(last.second - recent.second) * 1000.0 / (last.first - recent.first)
                            : 0.0;
        std::snprintf(line, sizeof(line),
                      "gpu: %.1f swaps per second since the first (at %.1f s), %.1f over the last %.0f s\n", overall,
                      first->first / 1000.0, lately, (last.first - recent.first) / 1000.0);
        out += line;
    }
    std::snprintf(line, sizeof(line),
                  "gpu: %llu vblanks; interrupts delivered: %llu vblank, %llu command processor (%llu raised); "
                  "%llu waits; %llu draws not issued; %llu packets that stopped their buffer\n",
                  (unsigned long long)v.vblanks.load(), (unsigned long long)v.interrupts[0].load(),
                  (unsigned long long)v.interrupts[1].load(), (unsigned long long)n.interrupts.load(),
                  (unsigned long long)n.waits.load(), (unsigned long long)n.failed_draws.load(),
                  (unsigned long long)n.bad_packets.load());
    out += line;
    if (uint64_t waits = n.waits.load()) {
        // what the processor waited for (WAIT_REG_MEM polls that did not match)
        uint32_t poll = n.last_wait_poll.load();
        std::snprintf(line, sizeof(line),
                      "gpu: of %llu WAIT_REG_MEM packets %llu did not match at once: %llu polls, %llu on memory, %llu "
                      "with a sleep; the last on %s 0x%X, reference 0x%X, mask 0x%X, wait 0x%X\n",
                      (unsigned long long)n.opcodes[pm4::WAIT_REG_MEM].load(),
                      (unsigned long long)n.waiting_packets.load(), (unsigned long long)waits,
                      (unsigned long long)n.waits_on_memory.load(), (unsigned long long)n.waits_asleep.load(),
                      poll & 0x80000000u ? "memory" : "register", poll & 0x7FFFFFFF, n.last_wait_ref.load(),
                      n.last_wait_mask.load(), n.last_wait_field.load());
        out += line;
    }
    if (uint64_t waits = v.spin_waits.load()) {
        std::snprintf(line, sizeof(line), "gpu: %llu sleeps of threads spinning while it worked (%u write-backs)\n",
                      (unsigned long long)waits, v.write_backs.load());
        out += line;
    }
    std::string opcodes, unknown;
    for (uint32_t op = 0; op < 128; op++) {
        if (uint64_t c = n.opcodes[op].load()) {
            std::snprintf(line, sizeof(line), " %s %llu", pm4_opcode_name(op), (unsigned long long)c);
            opcodes += line;
        }
        if (uint64_t c = n.unknown_opcodes[op].load()) {
            std::snprintf(line, sizeof(line), " 0x%02X (%s) %llu", op, pm4_opcode_name(op), (unsigned long long)c);
            unknown += line;
        }
    }
    if (!opcodes.empty()) out += "gpu: type-3 packets:" + opcodes + "\n";
    if (!unknown.empty()) out += "gpu: not implemented:" + unknown + "\n";
    return out;
}

// ---- the exports (xboxkrnl_video.cc) ----

// (type_ptr, power_ptr): 1 sRGB, 2 BT.709 (an HDTV's, Xenia's default), 3
// the power given
XF_EXPORT(xboxkrnl, VdGetCurrentDisplayGamma) {
    video(k);
    k.store32(arg(k, c, 0), 2);
    k.store32(arg(k, c, 1), float_bits(2.22222233f));
}

// X_DISPLAY_INFO (0x58): the front buffer, the scaler, the overscan, the display
XF_EXPORT(xboxkrnl, VdGetCurrentDisplayInformation) {
    uint32_t info = arg(k, c, 0);
    VideoMode m = video(k).mode();
    std::memset(k.host(info), 0, 0x58);
    k.store16(info + 0x00, uint16_t(m.width));
    k.store16(info + 0x02, uint16_t(m.height));
    k.store32(info + 0x08 + 0x08, m.width); // the scaler's source rectangle: x2, y2
    k.store32(info + 0x08 + 0x0C, m.height);
    k.store32(info + 0x08 + 0x10, m.width); // its output
    k.store32(info + 0x08 + 0x14, m.height);
    k.store32(info + 0x08 + 0x18, 1); // vertical filter
    k.store32(info + 0x08 + 0x28, 1); // horizontal filter
    k.store16(info + 0x40, 320);      // overscan: left, top, right, bottom
    k.store16(info + 0x42, 180);
    k.store16(info + 0x44, 320);
    k.store16(info + 0x46, 180);
    k.store16(info + 0x48, uint16_t(m.width));
    k.store16(info + 0x4A, uint16_t(m.height));
    k.store32(info + 0x4C, float_bits(m.refresh));
    k.store32(info + 0x50, 0); // interlaced
    k.store16(info + 0x56, uint16_t(m.width));
}

XF_EXPORT(xboxkrnl, VdQueryVideoMode) {
    video(k);
    write_video_mode(k, arg(k, c, 0));
}

// the mode the display really has: the same (Xenia's)
XF_EXPORT(xboxkrnl, VdQueryRealVideoMode) {
    video(k);
    write_video_mode(k, arg(k, c, 0));
}

// the GPU's revision: games initialize the EDRAM below 0x10 and retrain it
// from there on; Xenia's 0x11
XF_EXPORT(xboxkrnl, VdGetGraphicsAsicID) { result(c, 0x11); }

// widescreen 1, 720p 2, 1080 4
XF_EXPORT(xboxkrnl, VdQueryVideoFlags) {
    VideoMode m = video(k).mode();
    result(c, (m.widescreen ? 1 : 0) | (m.width >= 1280 ? 2 : 0) | (m.width >= 1920 ? 4 : 0));
}

XF_EXPORT(xboxkrnl, VdSetDisplayMode) {
    video(k);
    result(c, 0);
}

// (width, height, refresh rate, ...)
XF_EXPORT(xboxkrnl, VdSetDisplayModeOverride) {
    video(k);
    result(c, 0);
}

// (0x4F810000, a callback, its argument, the PFP and ME microcode)
XF_EXPORT(xboxkrnl, VdInitializeEngines) {
    video(k);
    result(c, 1);
}

// games pair an initialization with a shutdown to query, then initialize again
XF_EXPORT(xboxkrnl, VdShutdownEngines) { video(k); }

XF_EXPORT(xboxkrnl, VdEnableDisableClockGating) {
    video(k);
    result(c, 0);
}

XF_EXPORT(xboxkrnl, VdSetGraphicsInterruptCallback) {
    Video &v = video(k);
    v.callback_data = arg(k, c, 1);
    v.callback = arg(k, c, 0);
}

// (the ring buffer's physical address, log2 of its size in quadwords)
XF_EXPORT(xboxkrnl, VdInitializeRingBuffer) { video(k).cp->initialize_ring_buffer(arg(k, c, 0), arg(k, c, 1)); }

// (where the read pointer goes, physical; log2 of the block between updates)
XF_EXPORT(xboxkrnl, VdEnableRingBufferRPtrWriteBack) {
    video(k).cp->enable_read_pointer_write_back(arg(k, c, 0), arg(k, c, 1));
}

XF_EXPORT(xboxkrnl, VdGetSystemCommandBuffer) {
    video(k);
    uint32_t p0 = arg(k, c, 0), p1 = arg(k, c, 1);
    std::memset(k.host(p0), 0, 0x94);
    k.store32(p0, 0xBEEF0000);
    k.store32(p1, 0xBEEF0001);
}

XF_EXPORT(xboxkrnl, VdSetSystemCommandBufferGpuIdentifierAddress) { video(k); }

// Twelve arguments; the destination (the tenth) filled with type-2 no-ops,
// its size in words returned. The scaled output's size is kept for the
// presentation's aspect ratio.
XF_EXPORT(xboxkrnl, VdInitializeScalerCommandBuffer) {
    Video &v = video(k);
    uint32_t scaled_output_wh = arg(k, c, 3), destination = arg(k, c, 10), count = arg(k, c, 11);
    for (uint32_t i = 0; i < count; i++) k.store32(destination + 4 * i, pm4::type2());
    v.scaled_width = scaled_output_wh >> 16 & 0xFFFF;
    v.scaled_height = scaled_output_wh & 0xFFFF;
    result(c, count);
}

XF_EXPORT(xboxkrnl, VdCallGraphicsNotificationRoutines) {
    video(k);
    result(c, 0);
}

XF_EXPORT(xboxkrnl, VdIsHSIOTrainingSucceeded) {
    video(k);
    result(c, 1);
}

// (?, a pointer that gets memory MmFreePhysicalMemory(1, ...) frees later)
XF_EXPORT(xboxkrnl, VdPersistDisplay) {
    video(k);
    if (uint32_t out = arg(k, c, 1)) {
        uint32_t address = 0;
        k.memory().heap_by_type(true, 16 * 1024)->alloc(64, 32, ALLOCATE_RESERVE | ALLOCATE_COMMIT, 0, false, &address);
        k.store32(out, address);
    }
    result(c, 1);
}

XF_EXPORT(xboxkrnl, VdRetrainEDRAMWorker) {
    video(k);
    result(c, 0);
}

XF_EXPORT(xboxkrnl, VdRetrainEDRAM) {
    video(k);
    result(c, 0);
}

// VdSwap(buffer, fetch constant, ?, ?, ?, &front buffer, &texture format,
// &color space, &width, &height): the 64 words the caller set aside in the
// ring buffer get the front buffer's texture fetch constant (its address
// made physical) in fetch constant 0, then Xenia's XE_SWAP packet, then
// no-ops.
XF_EXPORT(xboxkrnl, VdSwap) {
    video(k);
    uint32_t buffer = arg(k, c, 0), fetch_ptr = arg(k, c, 1), width_ptr = arg(k, c, 8), height_ptr = arg(k, c, 9);
    uint32_t fetch[6];
    for (uint32_t i = 0; i < 6; i++) fetch[i] = k.load32(fetch_ptr + 4 * i);
    // the D3D texture header's fetch constant holds the front buffer's
    // virtual address (base_address: dword 1, bits 12-31)
    uint32_t frontbuffer = fetch[1] & 0xFFFFF000;
    uint32_t physical = k.memory().physical_address(frontbuffer);
    if (physical == UINT32_MAX) {
        std::fprintf(stderr, "VdSwap: the front buffer's address 0x%08X is not mapped\n", frontbuffer);
        return;
    }
    fetch[1] = (fetch[1] & 0xFFF) | (physical & 0xFFFFF000);
    uint32_t words[64], n = 0;
    words[n++] = pm4::type0(reg::FETCH_CONSTANTS, 6);
    for (uint32_t f : fetch) words[n++] = f;
    words[n++] = pm4::type3(pm4::XE_SWAP, 4);
    words[n++] = pm4::SWAP_SIGNATURE;
    words[n++] = physical;
    words[n++] = k.load32(width_ptr);
    words[n++] = k.load32(height_ptr);
    while (n < 64) words[n++] = pm4::type2();
    for (uint32_t i = 0; i < 64; i++) k.store32(buffer + 4 * i, words[i]);
}

// ---- xam (xam_video.cc) ----

XF_EXPORT(xam, XGetVideoMode) {
    video(k);
    write_video_mode(k, arg(k, c, 0));
}

} // namespace xf::kernel
