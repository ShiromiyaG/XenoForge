// xma_oracle: a recording of XMA contexts' runs (include/xenoforge/xma_recording.h,
// written by XenoForge's kernel under XF_XMA_RECORD) walked again by Xenia's
// own XMA context (xenia-apu's XmaContextNew, Xenia's default "new" decoder,
// with Xenia's FFmpeg), so the replay test (tests/xma_replay_test.cpp) can
// compare XenoForge's walk and decode with Xenia's on the same input. For each
// run: the context's data as it was before it, its input buffers and its
// output buffer written where they were (physical memory), then Enable and
// Work as Xenia's decoder thread does after a kick; the context's data and
// output buffer after it are written to <out> as a recording of the same form.
// Clears and releases call Clear and Release (XmaDecoder's register writes,
// XMAReleaseContext). Built by build.bat against the xenia-canary built in
// ../xenia-canary, not part of XenoForge's build.
//
// usage: xma_oracle <in.xmar> <out.xmar>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "xenia/apu/xma_context_new.h"
#include "xenia/memory.h"

#include "xenoforge/xma_recording.h"

namespace rec = xf::xma_recording;

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xma_oracle <in.xmar> <out.xmar>\n");
        return 2;
    }
    rec::Recording in;
    std::string error;
    if (!rec::read(argv[1], in, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (!error.empty()) std::fprintf(stderr, "warning: %s\n", error.c_str());

    xe::Memory memory;
    if (!memory.Initialize()) {
        std::fprintf(stderr, "Xenia's memory did not initialize\n");
        return 1;
    }
    // all of physical memory committed: the buffers go where the game had them
    uint8_t *physical = memory.TranslatePhysical(0);
    if (!VirtualAlloc(physical, 0x20000000, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "physical memory not committed\n");
        return 1;
    }
    // the contexts: 320 of 64 bytes at a physical address no run's buffer
    // touches, reached through the 0xA0000000 window (as the console's are)
    auto overlaps = [&](uint32_t start, uint32_t size) {
        for (const rec::Event &e : in.events) {
            if (e.type != rec::RUN) continue;
            const uint8_t *d = e.run.before;
            const uint32_t ranges[3][2] = {{rec::word(d, 5), (rec::word(d, 0) & 0xFFF) * 2048},
                                           {rec::word(d, 6), (rec::word(d, 1) & 0xFFF) * 2048},
                                           {rec::word(d, 7), ((rec::word(d, 0) >> 22) & 0x1F) * 256}};
            for (const auto &r : ranges) {
                uint32_t p = r[0] & 0x1FFFFFFF;
                if (r[1] && p < start + size && start < p + r[1]) return true;
            }
        }
        return false;
    };
    uint32_t contexts_physical = 0x1FF00000;
    while (contexts_physical && overlaps(contexts_physical, 320 * 64)) contexts_physical -= 0x10000;
    const uint32_t contexts_guest = 0xA0000000 + contexts_physical;
    if (!VirtualAlloc(memory.TranslateVirtual(contexts_guest), 320 * 64, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "the contexts' page not committed\n");
        return 1;
    }

    std::vector<std::unique_ptr<xe::apu::XmaContextNew>> contexts(320);
    auto context = [&](uint32_t id) -> xe::apu::XmaContextNew & {
        if (!contexts[id]) {
            contexts[id] = std::make_unique<xe::apu::XmaContextNew>();
            contexts[id]->Setup(id, &memory, contexts_guest + id * 64);
        }
        return *contexts[id];
    };

    rec::Writer out;
    if (!out.open(argv[2])) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    size_t runs = 0, missing = 0;
    for (const rec::Event &e : in.events) {
        const uint32_t id = e.run.context;
        if (id >= 320) continue;
        xe::apu::XmaContextNew &x = context(id);
        uint8_t *data = memory.TranslateVirtual(x.guest_ptr());
        if (e.type == rec::CLEAR) {
            x.Clear();
            out.event(rec::CLEAR, id);
            continue;
        }
        if (e.type == rec::RELEASE) {
            if (x.is_allocated()) x.Release();
            out.event(rec::RELEASE, id);
            continue;
        }
        const rec::Run &r = e.run;
        std::memcpy(data, r.before, 64);
        const uint32_t addresses[3] = {rec::word(r.before, 5), rec::word(r.before, 6), rec::word(r.before, 7)};
        const uint64_t hashes[3] = {r.input[0], r.input[1], r.output_before};
        for (int b = 0; b < 3; b++) {
            if (!hashes[b]) continue;
            const std::vector<uint8_t> *bytes = in.buffer(hashes[b]);
            if (!bytes) {
                missing++;
                continue;
            }
            std::memcpy(memory.TranslatePhysical(addresses[b]), bytes->data(), bytes->size());
        }
        // a context the game made (XMACreateContext), kicked (XMAEnableContext)
        x.set_is_allocated(true);
        x.Enable();
        x.Work();
        rec::Run result = r;
        std::memcpy(result.after, data, 64);
        const uint32_t output_size = ((rec::word(r.before, 0) >> 22) & 0x1F) * 256;
        result.output_after = output_size ? out.buffer(memory.TranslatePhysical(addresses[2]), output_size) : 0;
        // the run's input, as it came, in the output recording too
        auto copy = [&](uint64_t h) -> uint64_t {
            const std::vector<uint8_t> *bytes = h ? in.buffer(h) : nullptr;
            return bytes ? out.buffer(bytes->data(), uint32_t(bytes->size())) : 0;
        };
        result.input[0] = copy(r.input[0]);
        result.input[1] = copy(r.input[1]);
        result.output_before = copy(r.output_before);
        out.run(result);
        runs++;
    }
    out.close();
    std::printf("%zu runs walked by Xenia's XmaContextNew (%zu buffers missing)\n", runs, missing);
    // Xenia's memory and contexts are left to the process's end
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
