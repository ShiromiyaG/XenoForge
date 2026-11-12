// The trace writer (gpu_trace_writer.h) on hand-built packets run by a
// command processor, read back with the trace reader (gpu_trace.h): frames
// opened after the swap before them and closed after their own; the
// register file, the gamma ramps and EDRAM first; packets in order with
// their words as the guest's, an indirect buffer's own packet giving way to
// the packets inside it; the processor's reads and writes; the backend's
// reads recorded only where memory differs from what the trace gave, and
// never what the GPU wrote; compressed and uncompressed alike.

#include "check.h"

#include "xenoforge/command_processor.h"
#include "xenoforge/gpu_trace.h"
#include "xenoforge/gpu_trace_writer.h"
#include "xenoforge/memory.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::gpu;
using xenos::pm4::type0;
using xenos::pm4::type3;
namespace pm4 = xenos::pm4;
namespace reg = xenos::reg;

namespace {

Memory *M = nullptr;

void put_be32(uint32_t physical, uint32_t v) {
    uint8_t *p = M->translate_physical(physical);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint32_t physical_alloc(uint32_t size) { return M->physical_address(M->system_alloc(size, 0x1000, true)); }

struct Words {
    uint32_t at = 0, n = 0;
    explicit Words(uint32_t physical) : at(physical) {}
    Words &operator()(std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) put_be32(at + 4 * n++, w);
        return *this;
    }
};

struct Host : CommandProcessor::Host {
    void interrupt(uint32_t) override {}
    bool wait(uint32_t) override { return false; }
};

// a backend: reads memory at its draws, as the renderer reports them, and
// keeps an EDRAM of its own
struct Backend : Sink, TraceSource {
    MemoryTrace *trace = nullptr;
    uint32_t draw_address = 0, draw_bytes = 0;
    uint32_t settled = 0;
    std::vector<uint8_t> edram = std::vector<uint8_t>(10 << 20);
    void set_memory_trace(MemoryTrace *t) override { trace = t; }
    void settle() override { settled++; }
    bool snapshot_edram(std::vector<uint8_t> &out) override {
        out = edram;
        return true;
    }
    void draw(const RegisterFile &, const Draw &) override {
        if (trace) trace->read(draw_address, draw_bytes);
    }
};

// what the reader hands out, in order, as text
struct Seen : TraceVisitor {
    std::vector<std::string> events;
    std::vector<std::vector<uint8_t>> reads;
    std::vector<uint32_t> registers_seen;
    std::vector<uint8_t> edram;
    uint32_t gamma0 = 0;
    void packet(uint32_t address, std::span<const uint8_t> words) override {
        uint32_t header = uint32_t(words[0]) << 24 | uint32_t(words[1]) << 16 | uint32_t(words[2]) << 8 | words[3];
        events.push_back("packet " + std::to_string(words.size() / 4) + " " + std::to_string(header >> 30));
        (void)address;
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override {
        events.push_back("read " + std::to_string(address) + " " + std::to_string(bytes.size()));
        reads.emplace_back(bytes.begin(), bytes.end());
    }
    void memory_write(uint32_t address, std::span<const uint8_t> bytes) override {
        events.push_back("write " + std::to_string(address) + " " + std::to_string(bytes.size()));
    }
    void registers(uint32_t first, std::span<const uint32_t> values) override {
        events.push_back("registers " + std::to_string(first) + " " + std::to_string(values.size()));
        registers_seen.assign(values.begin(), values.end());
    }
    void edram_snapshot(std::span<const uint8_t> bytes) override {
        events.push_back("edram");
        edram.assign(bytes.begin(), bytes.end());
    }
    void event(uint32_t type) override { events.push_back("event " + std::to_string(type)); }
    void gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t>) override {
        events.push_back("gamma");
        gamma0 = table[1];
    }
};

void frames(bool compress) {
    Host host;
    Backend backend;
    CommandProcessor cp(*M, host);
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "xf_trace_writer_test";
    std::filesystem::remove_all(dir);
    TraceOptions options;
    options.directory = dir;
    options.frames = {1};
    options.title_id = 0x58460001;
    options.compress = compress;
    TraceWriter writer(cp, &backend, &backend, options);
    cp.set_sink(&writer);
    // (the backend is told of the writer while a frame is traced, not before)
    CHECK(backend.trace == nullptr);
    for (size_t i = 0; i < backend.edram.size(); i++) backend.edram[i] = uint8_t(i * 7 / 4096);

    uint32_t ring = physical_alloc(0x10000), data = physical_alloc(0x10000), ib = physical_alloc(0x1000);
    uint32_t vertices = physical_alloc(0x4000);
    for (uint32_t i = 0; i < 0x4000; i += 4) put_be32(vertices + i, i * 3 + 1);
    backend.draw_address = vertices + 100, backend.draw_bytes = 5000; // across two pages
    cp.initialize_ring_buffer(ring, 13);
    Words w(ring);
    auto swap = [&] { w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, 0x1000000, 1280, 720}); };
    auto draw = [&] { w({type3(pm4::DRAW_INDX_2, 1), 4 | 2u << 6 | 3u << 16}); }; // a triangle list, auto
    // frame 0: not traced
    w({type0(0x2100, 1), 7});
    draw();
    swap();
    cp.run(w.n);
    CHECK(!std::filesystem::exists(dir / "58460001_0.xtr"));
    CHECK(backend.settled == 1);
    CHECK(backend.trace == &writer);

    // frame 1: a register write, constants from memory, a fence, an
    // indirect buffer with a register write inside, draws reading memory
    // that changes between them and that the GPU wrote, then the swap
    for (uint32_t i = 0; i < 8; i++) put_be32(data + 4 * i, 0x3F800000 + i);
    Words inner(ib);
    inner({type0(0x2101, 1), 9});
    w({type0(0x2100, 1), 8});
    w({type3(pm4::LOAD_ALU_CONSTANT, 3), data, 0, 8});
    w({type3(pm4::MEM_WRITE, 2), (data + 64) | 2, 5});
    w({type3(pm4::INDIRECT_BUFFER, 2), ib, inner.n});
    draw(); // reads 5000 bytes: recorded
    draw(); // the same again: nothing
    w({type3(pm4::NOP, 1), 0});
    cp.run(w.n);
    put_be32(vertices + 4096 + 8, 0xDEADBEEF); // the CPU writes the second page
    draw();                                     // only that page's part recorded
    cp.run(w.n);
    // the GPU wrote the first page (a resolve reaching memory): known, not recorded
    put_be32(vertices + 200, 0xCAFEF00D);
    writer.gpu_wrote(vertices, 4096);
    draw();
    swap();
    cp.run(w.n);
    CHECK(backend.settled == 1); // only frame 1 opened
    // frame 2: not traced
    w({type0(0x2100, 1), 10});
    swap();
    cp.run(w.n);

    std::vector<std::filesystem::path> done = writer.finished();
    CHECK(done.size() == 1 && done[0].filename() == "58460001_1.xtr");
    CHECK(writer.errors().empty());
    if (done.size() != 1) return;
    GpuTrace trace;
    std::string error;
    CHECK(open_gpu_trace(done[0], trace, error));
    CHECK(trace.version == 1 && trace.title_id == 0x58460001);
    Seen seen;
    CHECK(walk_gpu_trace(trace, seen, error));
    std::string pages = std::to_string(vertices + 100) + " " + std::to_string(4096 - 100);
    std::vector<std::string> expected = {
        "registers 0 20483", "gamma", "edram",
        "packet 2 0",                                         // the register write
        "read " + std::to_string(data) + " 32", "packet 4 3", // LOAD_ALU_CONSTANT after what it read
        "write " + std::to_string(data + 64) + " 4", "packet 3 3",
        "packet 2 0", // the indirect buffer's packet, giving way to the one inside it
        "read " + std::to_string(vertices + 100) + " 5000", "packet 2 3", // the first draw
        "packet 2 3",                                                     // the second
        "packet 2 3",                                                     // the NOP
        "read " + std::to_string(vertices + 4096) + " " + std::to_string(5100 - 4096), "packet 2 3",
        "packet 2 3", // the GPU's bytes: nothing read
        "packet 5 3", "event 0"};
    CHECK(seen.events == expected);
    if (seen.events != expected)
        for (const std::string &e : seen.events) std::printf("  %s\n", e.c_str());
    CHECK(seen.registers_seen.size() == 0x5003 && seen.registers_seen[0x2100] == 7);
    CHECK(seen.edram == backend.edram);
    CHECK(seen.gamma0 == (1 * 0x3FF / 0xFF) * 0x100401u);
    // the bytes as memory held them when each was read
    CHECK(seen.reads.size() == 3 && seen.reads[1].size() == 5000 &&
          seen.reads[1][0] == uint8_t((100 * 3 + 1) >> 24) && seen.reads[1][3] == uint8_t(100 * 3 + 1));
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("no memory: %s\n", error.c_str());
        return 1;
    }
    M = memory.get();
    frames(true);
    frames(false);
    return finish("gpu_trace_writer");
}
