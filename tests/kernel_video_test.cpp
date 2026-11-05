// The video family (src/kernel/video.h) live: a guest thread sets the
// graphics interrupt callback, initializes the ring buffer and its read
// pointer's write-back, fills the ring with packets and kicks the GPU by
// writing CP_RB_WPTR through MMIO, as the XDK's D3D does; the command
// processor's thread executes them (register writes, an indirect buffer,
// fences, an interrupt that reaches the guest callback on a GPU thread as
// a DPC, a WAIT_REG_MEM the CPU satisfies later), VdSwap's packet is
// executed as a swap, vblank interrupts arrive, and the Vd* queries answer
// as Xenia's. The guest code is host functions in the generated code's
// table, calling the exports and reaching the registers as recompiled code
// does. Every check waits for a state, never for a time, so the run is the
// same each time: kernel_video_test [repeats].

#include "check.h"

#include "objects.h"
#include "video.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/ppc_ops.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>

using namespace xf;
using namespace xf::kernel;
namespace pm4 = xenos::pm4;
namespace reg = xenos::reg;

namespace {

Kernel *K = nullptr;
Memory *M = nullptr;

uint32_t ex(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s!%s\n", library, name);
        failures++;
        return 0xFFFFFFFF;
    }
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    fn(*K, c);
    return uint32_t(c.r[3]);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args = {}) {
    return ex(c, "xboxkrnl", name, args);
}

// the 32-bit arguments past the eighth, on the stack: words at r1 + 0x54 +
// 8 * (n - 8), as the games' callers store them (stw)
void stack_args(ppc::Context &c, std::initializer_list<uint32_t> args) {
    uint32_t n = 8;
    for (uint32_t a : args) K->store32(uint32_t(c.r[1]) + 0x54 + 8 * (n++ - 8), a);
}

// the GPU's registers, through lwz and stw on 0x7FC80000 (base register r3)
uint32_t mmio_read(ppc::Context &c, uint32_t index) { return ppc::ops::load32_io(c, 3, 0x7FC80000 + index * 4); }
void mmio_write(ppc::Context &c, uint32_t index, uint32_t value) {
    ppc::ops::store32_io(c, 3, 0x7FC80000 + index * 4, value);
}

uint32_t scratch = 0; // guest memory for the main thread

void sleep_ms(ppc::Context &c, int64_t ms) {
    K->store64(scratch + 0xF00, uint64_t(-ms * 10'000));
    krnl(c, "KeDelayExecutionThread", {1, 0, scratch + 0xF00});
}

// waits until done() (at most 10 s, so a broken run ends)
template <typename F> bool wait_until(ppc::Context &c, F done) {
    for (int i = 0; i < 10'000 && !done(); i++) sleep_ms(c, 1);
    return done();
}

// ---- the guest functions ----

enum : uint32_t { F_MAIN = 0x82000000, F_CALLBACK = 0x82000010 };
constexpr uint32_t CALLBACK_DATA = 0xDA7A;

std::atomic<uint32_t> vblank_calls{0}, cp_calls{0}, cp_cpu{0xFF}, bad_calls{0};
uint32_t interrupt_event = 0; // a guest KEVENT the CP interrupt sets
Thread *main_thread = nullptr;

// the graphics interrupt callback: (source, data), at DISPATCH_LEVEL in a
// DPC on a GPU thread, the title's process type
void callback(ppc::Context &c) {
    uint32_t source = uint32_t(c.r[3]), data = uint32_t(c.r[4]), pcr = uint32_t(c.r[13]);
    bool dpc = K->load32(pcr + kpcr::DPC_ACTIVE) == 1 && K->host(pcr)[kpcr::CURRENT_IRQL] == 2;
    Thread *self = current_thread();
    bool gpu_thread = self && self != main_thread && self->params.host && Kernel::current_context() == &c;
    uint32_t cpu = K->host(pcr)[kpcr::CURRENT_CPU];
    bool title = krnl(c, "KeGetCurrentProcessType") == 1;
    if (!dpc || !gpu_thread || !title || data != CALLBACK_DATA) bad_calls++;
    if (source == 0) {
        if (cpu != 2) bad_calls++;
        vblank_calls++;
    } else if (source == 1) {
        cp_cpu = cpu;
        cp_calls++;
        krnl(c, "KeSetEvent", {interrupt_event, 0, 0});
    } else {
        bad_calls++;
    }
}

// a buffer of packets in physical memory, written through its CPU address
struct Packets {
    uint32_t cpu = 0, physical = 0, n = 0;
    explicit Packets(uint32_t size) {
        cpu = M->system_alloc(size, 0x1000, true);
        physical = M->physical_address(cpu);
    }
    Packets &operator()(std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) K->store32(cpu + 4 * n++, w);
        return *this;
    }
};

void ring_buffer(ppc::Context &c) {
    Video &v = video(*K);
    krnl(c, "VdSetGraphicsInterruptCallback", {F_CALLBACK, CALLBACK_DATA});
    interrupt_event = scratch + 0x100;
    K->host(interrupt_event)[0] = 1; // a synchronization event, as KeInitializeEvent makes one
    K->store32(interrupt_event + 4, 0);
    K->store32(interrupt_event + 8, interrupt_event + 8), K->store32(interrupt_event + 12, interrupt_event + 8);

    Packets ring(0x8000), ib(0x1000), fences(0x1000), writeback(0x1000);
    krnl(c, "VdInitializeRingBuffer", {ring.physical, 12}); // 2^15 bytes
    krnl(c, "VdEnableRingBufferRPtrWriteBack", {writeback.physical, 6});
    CHECK(v.cp->ring_buffer() == ring.physical && v.cp->ring_buffer_bytes() == 0x8000);
    auto fence = [&](uint32_t i) { return K->load32(fences.cpu + 4 * i); };
    auto read_pointer = [&] { return K->load32(writeback.cpu); };

    ib({pm4::type0(0x2101, 1), 0x1B});
    ib({pm4::type3(pm4::MEM_WRITE, 2), fences.physical | 2, 0x1B1B}); // the indirect buffer's own fence
    ring({pm4::type0(0x2100, 1), 0xA});
    ring({pm4::type3(pm4::INDIRECT_BUFFER, 2), ib.physical, ib.n});
    ring({pm4::type3(pm4::EVENT_WRITE_SHD, 3), 0x15, (fences.physical + 4) | 2, 0xFE0C}); // a fence
    ring({pm4::type3(pm4::INTERRUPT, 1), 1u << 3});                                       // to CPU 3
    // the GPU waits for the CPU to write 7 at fences[2], then fences[3]
    ring({pm4::type3(pm4::WAIT_REG_MEM, 5), 0x10 | 3, (fences.physical + 8) | 2, 7, 0xFFFFFFFF, 0x100});
    ring({pm4::type3(pm4::MEM_WRITE, 2), (fences.physical + 12) | 2, 0xD0E});
    uint32_t end = ring.n;
    mmio_write(c, reg::CP_RB_WPTR, end);
    CHECK(mmio_read(c, reg::CP_RB_WPTR) == end);

    // the fence, the indirect buffer's, the registers as the CPU reads them
    CHECK(wait_until(c, [&] { return fence(1) == 0xFE0C; }));
    CHECK(fence(0) == 0x1B1B);
    CHECK(mmio_read(c, 0x2100) == 0xA && mmio_read(c, 0x2101) == 0x1B);
    // the interrupt reached the callback on CPU 3, which set the event
    K->store64(scratch + 0xF08, uint64_t(-10'000ll * 10'000));
    CHECK(krnl(c, "KeWaitForSingleObject", {interrupt_event, 0, 0, 0, scratch + 0xF08}) == X_STATUS_SUCCESS);
    CHECK(cp_calls == 1 && cp_cpu == 3);
    // the GPU waits: nothing after the wait, and the run's end not written back
    CHECK(fence(3) == 0 && read_pointer() == 0);
    CHECK(wait_until(c, [&] { return v.cp->counts().waits.load() >= 2; }));
    CHECK(fence(3) == 0 && read_pointer() == 0);
    K->store32(fences.cpu + 8, 7);
    CHECK(wait_until(c, [&] { return read_pointer() == end; }));
    CHECK(fence(3) == 0xD0E);

    // VdSwap: the front buffer's fetch constant (its CPU address in dword 1)
    // and the swap packet in the 64 words set aside in the ring
    uint32_t frontbuffer = M->system_alloc(1280 * 720 * 4, 0x1000, true);
    uint32_t fetch = scratch + 0x200, args = scratch + 0x240;
    const uint32_t fetch_words[6] = {0x00000002, (frontbuffer & 0xFFFFF000) | 0x86, 0x0059F4FF, 0, 0, 0x00000200};
    for (uint32_t i = 0; i < 6; i++) K->store32(fetch + 4 * i, fetch_words[i]);
    K->store32(args + 0, frontbuffer), K->store32(args + 4, 6), K->store32(args + 8, 0);
    K->store32(args + 12, 1280), K->store32(args + 16, 720);
    uint32_t swap_at = ring.cpu + 4 * end;
    stack_args(c, {args + 12, args + 16});
    krnl(c, "VdSwap", {swap_at, fetch, 0, 0xBEEF0000, 0xBEEF0001, args, args + 4, args + 8});
    uint32_t physical = M->physical_address(frontbuffer);
    const uint32_t expected[] = {pm4::type0(reg::FETCH_CONSTANTS, 6),
                                 fetch_words[0],
                                 (physical & 0xFFFFF000) | 0x86,
                                 fetch_words[2],
                                 fetch_words[3],
                                 fetch_words[4],
                                 fetch_words[5],
                                 pm4::type3(pm4::XE_SWAP, 4),
                                 pm4::SWAP_SIGNATURE,
                                 physical,
                                 1280,
                                 720};
    for (uint32_t i = 0; i < 12; i++) CHECK(K->load32(swap_at + 4 * i) == expected[i]);
    for (uint32_t i = 12; i < 64; i++) CHECK(K->load32(swap_at + 4 * i) == pm4::type2());
    mmio_write(c, reg::CP_RB_WPTR, end + 64);
    CHECK(wait_until(c, [&] { return read_pointer() == end + 64; }));
    CHECK(v.cp->counts().swaps == 1);
    CHECK(v.cp->registers()[reg::FETCH_CONSTANTS + 1] == ((physical & 0xFFFFF000) | 0x86));
    CHECK(v.cp->counts().bad_packets == 0);

    // vblanks: the callback with source 0 on CPU 2, the vertical counter on
    CHECK(wait_until(c, [&] { return vblank_calls.load() >= 3; }));
    CHECK(mmio_read(c, reg::D1MODE_V_COUNTER) >= 3 * 720);
    CHECK(bad_calls == 0);
}

void queries(ppc::Context &c) {
    // the video mode, from the settings: 1280x720, widescreen, 60 Hz, NTSC-M
    uint32_t mode = scratch + 0x400;
    const char *const exports[2][2] = {{"xboxkrnl", "VdQueryVideoMode"}, {"xam", "XGetVideoMode"}};
    for (const auto &e : exports) {
        std::memset(K->host(mode), 0xCC, 0x30);
        ex(c, e[0], e[1], {mode});
        CHECK(K->load32(mode + 0x00) == 1280 && K->load32(mode + 0x04) == 720);
        CHECK(K->load32(mode + 0x08) == 0 && K->load32(mode + 0x0C) == 1 && K->load32(mode + 0x10) == 1);
        CHECK(K->load32(mode + 0x14) == std::bit_cast<uint32_t>(60.0f) && K->load32(mode + 0x18) == 1);
        CHECK(K->load32(mode + 0x1C) == 0x8A && K->load32(mode + 0x20) == 1 && K->load32(mode + 0x2C) == 0);
    }
    CHECK(krnl(c, "VdQueryVideoFlags") == 3);

    uint32_t info = scratch + 0x500;
    krnl(c, "VdGetCurrentDisplayInformation", {info});
    CHECK(K->load16(info) == 1280 && K->load16(info + 2) == 720 && K->load32(info + 0x10) == 1280);
    CHECK(K->load32(info + 0x20) == 1 && K->load32(info + 0x30) == 1);
    CHECK(K->load16(info + 0x40) == 320 && K->load16(info + 0x46) == 180 && K->load16(info + 0x56) == 1280);
    CHECK(K->load32(info + 0x4C) == std::bit_cast<uint32_t>(60.0f));

    krnl(c, "VdGetCurrentDisplayGamma", {scratch + 0x600, scratch + 0x604});
    CHECK(K->load32(scratch + 0x600) == 2 && K->load32(scratch + 0x604) == std::bit_cast<uint32_t>(2.22222233f));

    krnl(c, "VdGetSystemCommandBuffer", {scratch + 0x700, scratch + 0x7A0});
    CHECK(K->load32(scratch + 0x700) == 0xBEEF0000 && K->load32(scratch + 0x704) == 0);
    CHECK(K->load32(scratch + 0x7A0) == 0xBEEF0001);

    uint32_t scaler = scratch + 0x800;
    stack_args(c, {0, 0, scaler, 16});
    CHECK(krnl(c, "VdInitializeScalerCommandBuffer", {0, 1280u << 16 | 720, 0, 1280u << 16 | 720, 0, 7, 0, 7}) == 16);
    for (uint32_t i = 0; i < 16; i++) CHECK(K->load32(scaler + 4 * i) == 0x80000000);
    CHECK(video(*K).scaled_width == 1280 && video(*K).scaled_height == 720);

    CHECK(krnl(c, "VdPersistDisplay", {0, scratch + 0x900}) == 1);
    CHECK(K->load32(scratch + 0x900) >= 0xA0000000 && K->load32(scratch + 0x900) < 0xC0000000);

    CHECK(krnl(c, "VdInitializeEngines", {0x4F810000, 0, 0, 0, 0}) == 1);
    CHECK(krnl(c, "VdIsHSIOTrainingSucceeded") == 1);
    CHECK(krnl(c, "VdRetrainEDRAM", {0, 0, 0, 0, 0, 0}) == 0 && krnl(c, "VdRetrainEDRAMWorker", {0}) == 0);
    CHECK(krnl(c, "VdSetDisplayMode", {0x40000000}) == 0 && krnl(c, "VdEnableDisableClockGating", {1}) == 0);
    CHECK(krnl(c, "VdCallGraphicsNotificationRoutines", {1, 0}) == 0);
    krnl(c, "VdSetSystemCommandBufferGpuIdentifierAddress", {0});
    krnl(c, "VdShutdownEngines");

    // the registers Xenia answers itself
    CHECK(mmio_read(c, reg::RB_EDRAM_TIMING) == 0x08100748 && mmio_read(c, reg::RB_BC_CONTROL) == 0x0000200E);
    CHECK(mmio_read(c, reg::D1MODE_VIEWPORT_SIZE) == 0x050002D0 && mmio_read(c, reg::D1MODE_VBLANK_VLINE_STATUS) == 1);
}

void main_function(ppc::Context &c) {
    ring_buffer(c);
    queries(c);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_function},
    {F_CALLBACK, callback},
};

void run() {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("memory: %s\n", error.c_str());
        failures++;
        return;
    }
    M = memory.get();
    vblank_calls = 0, cp_calls = 0, cp_cpu = 0xFF, bad_calls = 0;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        scratch = memory->system_alloc(0x1000);
        Objects &o = objects(k);
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            main_thread = t.get();
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
        {
            // what is left: the GPU's two threads
            std::lock_guard<std::mutex> l(o.lock);
            CHECK(o.threads.size() == 2);
            for (Thread *g : o.threads) CHECK(g->params.host && g->params.name);
        }
        CHECK(video_report(k).find("1 swaps") != std::string::npos);
        // they stop with the kernel; here first, to see them go
        shutdown_video(k);
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    M = nullptr, K = nullptr, main_thread = nullptr;
}

} // namespace

int main(int argc, char **argv) {
    int repeats = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    for (int i = 0; i < repeats && !failures; i++) run();
    return finish("kernel_video");
}
