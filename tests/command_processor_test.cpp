// The command processor (command_processor.h) on hand-built ring and
// indirect buffers in guest physical memory: register writes of every kind,
// their side effects (scratch write-back, COHER_STATUS_HOST, the gamma
// ramp), fences (MEM_WRITE, EVENT_WRITE_SHD, REG_TO_MEM, COND_WRITE), the
// events' writes, a WAIT_REG_MEM satisfied later, interrupts, predication,
// draws, resolves, shaders and swaps reported to a sink, the ring's wrap
// and the read pointer's write-back. One thread, no clock: the waits are
// the host's, counted.

#include "check.h"

#include "gpu/deferred_sink.h"
#include "xenoforge/command_processor.h"
#include "xenoforge/memory.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::gpu;
using xenos::pm4::type0;
using xenos::pm4::type1;
using xenos::pm4::type2;
using xenos::pm4::type3;
namespace pm4 = xenos::pm4;
namespace reg = xenos::reg;

namespace {

Memory *M = nullptr;

uint32_t be32(uint32_t physical) {
    const uint8_t *p = M->translate_physical(physical);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(uint32_t physical) {
    const uint8_t *p = M->translate_physical(physical);
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
void put_be32(uint32_t physical, uint32_t v) {
    uint8_t *p = M->translate_physical(physical);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// physical memory, zeroed: its physical address
uint32_t physical_alloc(uint32_t size) { return M->physical_address(M->system_alloc(size, 0x1000, true)); }

// words written one after another from a physical address
struct Words {
    uint32_t at = 0, n = 0;
    explicit Words(uint32_t physical) : at(physical) {}
    Words &operator()(std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) put_be32(at + 4 * n++, w);
        return *this;
    }
};

struct Host : CommandProcessor::Host {
    std::vector<uint32_t> interrupts;
    uint32_t waits = 0;
    bool stopping = false;
    std::function<void(uint32_t)> on_wait; // with the waits so far
    void interrupt(uint32_t cpu) override { interrupts.push_back(cpu); }
    bool wait(uint32_t) override {
        waits++;
        if (on_wait) on_wait(waits);
        return !stopping;
    }
};

struct Recorder : Sink {
    std::vector<Draw> draws, resolves;
    std::vector<uint32_t> draw_modes; // RB_MODECONTROL at each draw
    std::vector<Swap> swaps;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shaders;
    std::vector<std::vector<uint32_t>> packets;
    std::vector<std::string> buffers;
    uint32_t written = 0, read = 0;
    void buffer_start(bool indirect, uint32_t, uint32_t dwords) override {
        buffers.push_back((indirect ? "ib " : "rb ") + std::to_string(dwords));
    }
    void buffer_end(bool indirect) override { buffers.push_back(indirect ? "ib end" : "rb end"); }
    void packet(uint32_t, std::span<const uint32_t> words) override {
        packets.emplace_back(words.begin(), words.end());
    }
    bool wants_packets() override { return true; }
    void memory_read(uint32_t, uint32_t) override { read++; }
    void memory_written(uint32_t, uint32_t) override { written++; }
    void shader(uint32_t type, uint32_t, std::span<const uint32_t> code) override {
        shaders.push_back({type, std::vector<uint32_t>(code.begin(), code.end())});
    }
    void draw(const RegisterFile &r, const Draw &d) override {
        draws.push_back(d);
        draw_modes.push_back(r[reg::RB_MODECONTROL]);
    }
    void resolve(const RegisterFile &, const Draw &d) override { resolves.push_back(d); }
    void swap(const RegisterFile &, const Swap &s) override { swaps.push_back(s); }
};

struct Fixture {
    Host host;
    Recorder sink;
    CommandProcessor cp{*M, host};
    uint32_t ring = physical_alloc(0x1000), writeback = physical_alloc(0x1000);
    Fixture() {
        cp.set_sink(&sink);
        cp.initialize_ring_buffer(ring, 9); // 2^12 bytes
        cp.enable_read_pointer_write_back(writeback, 6);
    }
    // the ring's words from the read pointer on, then run up to their end
    Words packets() { return Words(ring + 4 * cp.read_pointer()); }
    void kick(const Words &w) { cp.run((w.at - ring) / 4 + w.n); }
};

void registers() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t constants = physical_alloc(0x1000);
    Words{constants}({0x3F800000, 0x40000000, 0x40400000, 0x40800000});
    Words w = f.packets();
    w({type0(0x2100, 3), 11, 12, 13});                      // three in a row
    w({type0(0x2110, 3, true), 21, 22, 23});                // one, three times
    w({type1(0x120, 0x121), 31, 32});                       // two named in the header
    w({type3(pm4::SET_CONSTANT, 3), 0u << 16 | 4, 41, 42}); // ALU constants 4, 5
    w({type3(pm4::SET_CONSTANT, 2), 1u << 16 | 6, 51});     // fetch constant word 6
    w({type3(pm4::SET_CONSTANT, 2), 2u << 16 | 1, 61});     // bool
    w({type3(pm4::SET_CONSTANT, 2), 3u << 16 | 2, 71});     // loop
    w({type3(pm4::SET_CONSTANT, 2), 4u << 16 | 0x180, 81}); // registers from 0x2000
    w({type3(pm4::SET_CONSTANT2, 3), 0x2200, 91, 92});      // by index
    w({type3(pm4::SET_SHADER_CONSTANTS, 2), 0x4100, 93});
    w({type3(pm4::LOAD_ALU_CONSTANT, 3), constants | 0x40000000, 0u << 16 | 8, 4}); // from memory
    // REG_RMW reaches the registers below 0x2000 (13 bits)
    w({type0(0x0300, 3), 0xF0F0, 0x0F00, 12});
    w({type3(pm4::REG_RMW, 3), 0x0300, 0xFF00, 0x000F});            // (0xF0F0 & 0xFF00) | 0xF
    w({type3(pm4::REG_RMW, 3), 3u << 30 | 0x0301, 0x0300, 0x0302}); // & a register, | another
    f.kick(w);
    CHECK(r[0x2100] == 11 && r[0x2101] == 12 && r[0x2102] == 13);
    CHECK(r[0x2110] == 23 && r[0x2111] == 0);
    CHECK(r[0x120] == 31 && r[0x121] == 32);
    CHECK(r[0x4004] == 41 && r[0x4005] == 42);
    CHECK(r[0x4806] == 51 && r[0x4901] == 61 && r[0x490A] == 71 && r[0x2180] == 81);
    CHECK(r[0x2200] == 91 && r[0x2201] == 92 && r[0x4100] == 93);
    CHECK(r[0x4008] == 0x3F800000 && r[0x400B] == 0x40800000);
    CHECK(r[0x0300] == 0xF00F);
    CHECK(r[0x0301] == ((0x0F00 & 0xF00F) | 12));
    // the read pointer moved to the end and was written back, big-endian
    CHECK(f.cp.read_pointer() == w.n);
    CHECK(be32(f.writeback) == w.n);
    CHECK(f.sink.packets.size() == 14 && f.sink.packets[0] == (std::vector<uint32_t>{type0(0x2100, 3), 11, 12, 13}));
    CHECK(f.sink.read == 1); // LOAD_ALU_CONSTANT's
    CHECK(f.cp.counts().packets == 14 && f.cp.counts().opcodes[pm4::SET_CONSTANT] == 5);
}

void memory_writes() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t m = physical_alloc(0x1000);
    f.cp.increment_counter(), f.cp.increment_counter(), f.cp.increment_counter();
    Words w = f.packets();
    w({type3(pm4::MEM_WRITE, 3), (m + 0x00) | 2, 0x11223344, 0x55667788});   // 8 in 32: as the CPU reads it
    w({type3(pm4::MEM_WRITE, 2), (m + 0x10) | 0, 0x11223344});               // no swap: little-endian
    w({type3(pm4::MEM_WRITE, 2), (m + 0x14) | 1, 0x11223344});               // 8 in 16
    w({type3(pm4::MEM_WRITE, 2), (m + 0x18) | 3, 0x11223344});               // 16 in 32
    w({type3(pm4::EVENT_WRITE_SHD, 3), 0x15, (m + 0x20) | 2, 0xFE11CE});     // a fence's value
    w({type3(pm4::EVENT_WRITE_SHD, 3), 1u << 31 | 0x15, (m + 0x24) | 2, 0}); // the counter
    w({type0(0x2130, 1), 0xCAFE});
    w({type3(pm4::REG_TO_MEM, 2), 0x2130, (m + 0x28) | 2});
    w({type3(pm4::EVENT_WRITE, 1), 0x45});
    f.kick(w);
    CHECK(be32(m + 0x00) == 0x11223344 && be32(m + 0x04) == 0x55667788);
    CHECK(le32(m + 0x10) == 0x11223344);
    CHECK(le32(m + 0x14) == 0x22114433);
    CHECK(le32(m + 0x18) == 0x33441122);
    CHECK(be32(m + 0x20) == 0xFE11CE);
    CHECK(be32(m + 0x24) == 3);
    CHECK(be32(m + 0x28) == 0xCAFE);
    CHECK(r[reg::VGT_EVENT_INITIATOR] == 0x05);
    CHECK(f.sink.written == 8 + 1); // and the read pointer's write-back

    // conditional writes: to memory, to a register, and one that fails
    put_be32(m + 0x40, 5);
    w = f.packets();
    w({type3(pm4::COND_WRITE, 6), 0x110 | 3, (m + 0x40) | 2, 5, 0xFFFFFFFF, (m + 0x44) | 2, 0xAB}); // == 5: write
    w({type3(pm4::COND_WRITE, 6), 0x000 | 6, 0x2130, 0x1000, 0xFFFF, 0x2131, 0xCD});                // reg > 0x1000
    w({type3(pm4::COND_WRITE, 6), 0x110 | 1, (m + 0x40) | 2, 5, 0xFFFFFFFF, (m + 0x48) | 2, 0xEF}); // < 5: no
    // the screen extent, and an occlusion query's report (Xenia's fake one)
    w({type3(pm4::EVENT_WRITE_EXT, 2), 0x1A, (m + 0x60) | 1});
    w({type0(reg::RB_SAMPLE_COUNT_ADDR, 1), m + 0x80});
    w({type3(pm4::EVENT_WRITE_ZPD, 1), 0x1B});
    w({type0(reg::RB_SAMPLE_COUNT_ADDR, 1), m + 0xA0});
    w({type3(pm4::EVENT_WRITE_ZPD, 1), 0x1B});
    f.kick(w);
    CHECK(be32(m + 0x44) == 0xAB && r[0x2131] == 0xCD && be32(m + 0x48) == 0);
    CHECK(be32(m + 0x60) == (0u << 16 | 1024) && be32(m + 0x64) == (0u << 16 | 1024) && be32(m + 0x68) == 1);
    // 100 samples, then 99 more: ZPass and Total split in two lanes, little-endian
    CHECK(le32(m + 0x80) == 50 && le32(m + 0x84) == 50 && le32(m + 0x90) == 50 && le32(m + 0x94) == 50);
    CHECK(le32(m + 0x88) == 0 && le32(m + 0x98) == 0);
    CHECK(le32(m + 0xA0 + 0x10) == 100 && le32(m + 0xA0 + 0x14) == 99);

    // a fence in the writeback range lands at 0x7F000000 and up
    if (M->heap_for(0x7F000000)
            ->alloc_fixed(0x7F000000, 0x1000, 0x1000, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE)) {
        w = f.packets();
        w({type0(reg::WRITEBACK_START, 2), 0x7F000000, 0x1000});
        w({type3(pm4::EVENT_WRITE_SHD, 3), 0x15, 0x7F000010 | 2, 0x5EED});
        f.kick(w);
        const uint8_t *p = M->translate(0x7F000010);
        CHECK((uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) == 0x5EED);
    } else {
        CHECK(!"the writeback range");
    }
}

void special_registers() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t scratch = physical_alloc(0x1000);
    Words w = f.packets();
    w({type0(reg::SCRATCH_UMSK, 2), 0b101, scratch});
    w({type0(reg::SCRATCH_REG0, 3), 0x11, 0x22, 0x33});
    // COHER_STATUS_HOST marked pending, then waited on: made coherent at once
    w({type0(reg::COHER_STATUS_HOST, 1), 0x03000000});
    w({type3(pm4::WAIT_REG_MEM, 5), 3, reg::COHER_STATUS_HOST, 0, 0xFFFFFFFF, 0x100});
    f.kick(w);
    CHECK(be32(scratch) == 0x11 && be32(scratch + 4) == 0 && be32(scratch + 8) == 0x33);
    CHECK(r[reg::SCRATCH_REG0 + 1] == 0x22);
    CHECK(r[reg::COHER_STATUS_HOST] == 0 && f.host.waits == 0);

    // a COHER_STATUS_HOST write alone stays pending
    w = f.packets();
    w({type0(reg::COHER_STATUS_HOST, 1), 0x01000000});
    f.kick(w);
    CHECK(r[reg::COHER_STATUS_HOST] == 0x81000000);

    // the gamma ramp: a 30-bit color, a sequential one, a PWL entry
    w = f.packets();
    w({type0(reg::DC_LUT_RW_MODE, 2), 0, 0}); // the 256-entry table, from entry 0
    w({type0(reg::DC_LUT_WRITE_EN_MASK, 1), 7});
    w({type0(reg::DC_LUT_30_COLOR, 1), 0x3FF00FFC});
    w({type0(reg::DC_LUT_SEQ_COLOR, 3, true), 0x1000, 0x2040, 0xFFFF}); // red, green, blue
    w({type0(reg::DC_LUT_RW_MODE, 2), 1, 5});                           // PWL, entry 5
    w({type0(reg::DC_LUT_PWL_DATA, 3, true), 0x0200FFC0, 0x0300F000, 0x0400003F});
    f.kick(w);
    const auto &table = f.cp.gamma_ramp_table();
    CHECK(table[0] == 0x3FF00FFC);
    CHECK(table[1] == ((0x1000u >> 6) << 20 | (0x2040u >> 6) << 10 | (0xFFFFu >> 6)));
    CHECK(table[2] == 2 * 0x3FF / 0xFF * 0x100401); // untouched: linear
    const auto &pwl = f.cp.gamma_ramp_pwl();
    CHECK(pwl[5 * 3 + 0] == 0x0200FFC0 && pwl[5 * 3 + 1] == 0x0300F000 && pwl[5 * 3 + 2] == 0x04000000);
    CHECK(r[reg::DC_LUT_RW_INDEX] == 6);
    CHECK(f.cp.gamma_ramp_version() == 7);
}

void indirect_buffers() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t a = physical_alloc(0x1000), b = physical_alloc(0x1000), fence = physical_alloc(0x1000);
    Words ib2(b);
    ib2({type0(0x2140, 1), 2});
    ib2({type3(pm4::MEM_WRITE, 2), fence | 2, 2}); // its fence
    Words ib1(a);
    ib1({type0(0x2141, 1), 1});
    ib1({type3(pm4::INDIRECT_BUFFER_PFD, 2), b | 0xA0000000, ib2.n}); // a CPU address, made physical
    ib1({type3(pm4::MEM_WRITE, 2), (fence + 4) | 2, 1});
    Words w = f.packets();
    w({type3(pm4::INDIRECT_BUFFER, 2), a, ib1.n});
    w({type3(pm4::MEM_WRITE, 2), (fence + 8) | 2, 3});
    f.kick(w);
    CHECK(r[0x2140] == 2 && r[0x2141] == 1);
    CHECK(be32(fence) == 2 && be32(fence + 4) == 1 && be32(fence + 8) == 3);
    CHECK(f.cp.counts().indirect_buffers == 2);
    std::vector<std::string> expected = {"rb 6", "ib 8", "ib 5", "ib end", "ib end", "rb end"};
    CHECK(f.sink.buffers == expected);

    // an indirect buffer that points at itself stops at the nesting limit
    Words loop(a);
    loop({type3(pm4::INDIRECT_BUFFER, 2), a, 3});
    w = f.packets();
    w({type3(pm4::INDIRECT_BUFFER, 2), a, 3});
    w({type0(0x2142, 1), 42});
    f.kick(w);
    CHECK(r[0x2142] == 42);
}

void predication() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    Words w = f.packets();
    w({type3(pm4::SET_BIN_SELECT, 2), 0, 0});           // no bin selected
    w({type3(pm4::SET_CONSTANT2, 2, true), 0x2150, 1}); // predicated: skipped
    w({type3(pm4::SET_BIN_SELECT_LO, 1), 4});           // bin 2
    w({type3(pm4::SET_BIN_MASK, 2), 0, 6});
    w({type3(pm4::SET_CONSTANT2, 2, true), 0x2151, 2});              // runs
    w({type3(pm4::XE_SWAP, 4, true), pm4::SWAP_SIGNATURE, 0, 1, 1}); // a predicated swap never does
    f.kick(w);
    CHECK(r[0x2150] == 0 && r[0x2151] == 2);
    CHECK(f.sink.swaps.empty() && f.cp.counts().swaps == 0);
}

void draws_shaders_swaps() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t code = physical_alloc(0x1000);
    Words{code}({0xC0DE0001, 0xC0DE0002, 0xC0DE0003});
    uint32_t indices = physical_alloc(0x1000);
    Words w = f.packets();
    w({type3(pm4::IM_LOAD, 2), code | 1, 3});                            // a pixel shader from memory
    w({type3(pm4::IM_LOAD_IMMEDIATE, 5), 0, 2, 0xAAAA, 0xBBBB, 0xFFFF}); // a vertex shader, a word of padding
    w({type0(reg::RB_MODECONTROL, 1), 4});                               // color and depth
    // indexed: 32-bit indices, 8-in-32, 3 of them; a CPU address made physical
    w({type3(pm4::DRAW_INDX, 4), 0x123, 3u << 16 | 1u << 11 | 0u << 6 | 4, (indices + 2) | 0xA0000000, 2u << 30 | 3});
    w({type3(pm4::DRAW_INDX_2, 1), 6u << 16 | 2u << 6 | 8});  // auto-indexed rectangles
    w({type3(pm4::DRAW_INDX_2, 3), 3u << 16 | 1u << 6 | 4, 0x00050004, 0x0006}); // immediate 16-bit indices 4, 5, 6
    w({type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 3u << 6 | 4});                     // no valid source: not issued
    w({type0(reg::RB_MODECONTROL, 1), 6});                    // copy
    w({type3(pm4::DRAW_INDX, 2), 0, 3u << 16 | 2u << 6 | 8}); // the resolve
    w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, 0x1E000000, 1280, 720});
    f.kick(w);
    CHECK(f.sink.shaders.size() == 2);
    if (f.sink.shaders.size() == 2) {
        CHECK(f.sink.shaders[0].first == 1 &&
              f.sink.shaders[0].second == (std::vector<uint32_t>{0xC0DE0001, 0xC0DE0002, 0xC0DE0003}));
        CHECK(f.sink.shaders[1].first == 0 && f.sink.shaders[1].second == (std::vector<uint32_t>{0xAAAA, 0xBBBB}));
    }
    CHECK(f.sink.draws.size() == 3 && f.sink.resolves.size() == 1);
    if (f.sink.draws.size() == 3) {
        const Draw &d = f.sink.draws[0];
        CHECK(d.viz_query == 0x123 && d.primitive == 4 && d.count == 3 && d.indexed && d.index32);
        CHECK(d.index_base == indices && d.index_bytes == 12 && d.index_endian == Endian::In32);
        CHECK(f.sink.draw_modes[0] == 4);
        CHECK(!f.sink.draws[1].indexed && f.sink.draws[1].count == 6 && f.sink.draws[1].primitive == 8);
        const Draw &immediate = f.sink.draws[2];
        CHECK(immediate.indexed && !immediate.index32 && immediate.count == 3 && immediate.index_bytes == 6 &&
              immediate.immediate == (std::vector<uint32_t>{0x00050004, 0x0006}));
    }
    CHECK(r[reg::VGT_DMA_BASE] == (indices + 2) && r[reg::VGT_DMA_SIZE] == (2u << 30 | 3));
    CHECK(f.cp.counts().draws == 3 && f.cp.counts().resolves == 1 && f.cp.counts().failed_draws == 1 &&
          f.cp.counts().immediate_draws == 1);
    CHECK(f.sink.swaps.size() == 1 && f.cp.counts().swaps == 1 && f.cp.counter() == 1);
    if (!f.sink.swaps.empty())
        CHECK(f.sink.swaps[0].frontbuffer == 0x1E000000 && f.sink.swaps[0].width == 1280 &&
              f.sink.swaps[0].height == 720);
}

void interrupts_and_waits() {
    Fixture f;
    uint32_t m = physical_alloc(0x1000);
    // an interrupt for CPUs 0 and 5
    Words w = f.packets();
    w({type3(pm4::INTERRUPT, 1), 0b100001});
    f.kick(w);
    CHECK((f.host.interrupts == std::vector<uint32_t>{0, 5}) && f.cp.counts().interrupts == 2);

    // a wait on memory the CPU satisfies later: the fence after it is only
    // written once the wait matches
    bool fence_early = false;
    f.host.on_wait = [&](uint32_t waits) {
        fence_early |= be32(m + 4) != 0;
        if (waits == 3) put_be32(m, 7);
    };
    w = f.packets();
    w({type3(pm4::WAIT_REG_MEM, 5), 0x10 | 3, m | 2, 7, 0xFFFFFFFF, 0x100}); // memory, == 7
    w({type3(pm4::MEM_WRITE, 2), (m + 4) | 2, 0xD0E});
    f.kick(w);
    CHECK(f.host.waits == 3 && !fence_early && be32(m + 4) == 0xD0E);
    CHECK(f.cp.counts().waits == 3);

    // a wait on a register, and one that is told to stop: the rest of the
    // ring is dropped, as in Xenia
    f.host.on_wait = [&](uint32_t waits) {
        if (waits == 5) f.host.stopping = true;
    };
    w = f.packets();
    w({type3(pm4::WAIT_REG_MEM, 5), 6, 0x2160, 0, 0xFFFFFFFF, 0}); // register > 0: never
    w({type3(pm4::MEM_WRITE, 2), (m + 8) | 2, 0xBAD});
    f.kick(w);
    CHECK(f.host.waits == 5 && be32(m + 8) == 0 && f.cp.counts().bad_packets == 1);
    CHECK(f.cp.read_pointer() == (w.at - f.ring) / 4 + w.n); // the read pointer moves on all the same
}

void ring_wrap_and_bad_packets() {
    Host host;
    Recorder sink;
    CommandProcessor cp(*M, host);
    cp.set_sink(&sink);
    RegisterFile &r = cp.registers();
    uint32_t ring = physical_alloc(0x1000);
    cp.initialize_ring_buffer(ring, 3); // 64 bytes: 16 words
    Words{ring}({type3(pm4::NOP, 11), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    cp.run(12);
    CHECK(cp.read_pointer() == 12);
    // a packet from word 12 that wraps to words 0 and 1
    put_be32(ring + 12 * 4, type0(0x2170, 5));
    for (uint32_t i = 0; i < 5; i++) put_be32(ring + ((13 + i) % 16) * 4, 100 + i);
    cp.run(2);
    CHECK(r[0x2170] == 100 && r[0x2173] == 103 && r[0x2174] == 104);
    CHECK(sink.packets.size() == 2 &&
          sink.packets[1] == (std::vector<uint32_t>{type0(0x2170, 5), 100, 101, 102, 103, 104}));
    CHECK(cp.read_pointer() == 2);

    // padding and type-2 words are one-word no-ops; an opcode with no
    // handler ends the run
    Words w(ring + 2 * 4);
    w({0, 0x0BADF00D, type2(), type3(0x4F, 2), 0, 0, type0(0x2190, 1), 9});
    cp.run(2 + w.n);
    CHECK(r[0x2190] == 0);
    CHECK(cp.counts().unknown_opcodes[0x4F] == 1);
    CHECK(cp.counts().bad_packets == 1);
    CHECK(cp.read_pointer() == 2 + w.n);
    // a packet longer than what is left of the run ends it too
    w = Words(ring + 4 * cp.read_pointer());
    w({type0(0x2191, 4), 1});
    cp.run((w.at - ring) / 4 + w.n);
    CHECK(r[0x2191] == 0 && cp.counts().bad_packets == 2);
}

// What a trace player does: packets run where they lie, on their own, not
// as a buffer; the registers restored without their side effects; the
// gamma ramps restored.
void trace_player() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t at = physical_alloc(0x1000);
    Words w(at);
    w({type0(0x2100, 2), 5, 6});
    w({type3(pm4::DRAW_INDX_2, 1), 4u | 2u << 6 | 3u << 16}); // auto-indexed, 3 vertices
    f.cp.execute_packets(at, 3);                              // the first packet alone
    CHECK(r[0x2100] == 5 && r[0x2101] == 6 && f.sink.draws.empty());
    f.cp.execute_packets(at + 12, 2);
    CHECK(f.sink.draws.size() == 1 && f.sink.draws[0].count == 3);
    CHECK(f.sink.buffers.empty() && f.cp.counts().indirect_buffers == 0);
    // a scratch register restored is not written back
    r.set(reg::SCRATCH_UMSK, 1);
    r.set(reg::SCRATCH_ADDR, at + 0x800);
    const uint32_t values[] = {0x12345678};
    f.cp.restore_registers(reg::SCRATCH_REG0, values);
    CHECK(r[reg::SCRATCH_REG0] == 0x12345678 && be32(at + 0x800) == 0);
    std::vector<uint32_t> table(256, 0x3FFFFFFF), pwl(3 * 128, 0x00400040);
    uint64_t version = f.cp.gamma_ramp_version();
    f.cp.restore_gamma_ramp(table, pwl);
    CHECK(f.cp.gamma_ramp_table()[7] == 0x3FFFFFFF && f.cp.gamma_ramp_pwl()[100] == 0x00400040);
    CHECK(f.cp.gamma_ramp_version() > version);
}

} // namespace

// Occlusion queries with real counts (OcclusionQueries::Fast, Xenia's "fast"):
// each report written at once with a guess, and again with the sink's counts
struct Counter : Recorder {
    std::vector<std::pair<uint64_t, uint64_t>> reports; // (ended, next) as told
    std::vector<std::pair<uint64_t, uint64_t>> answers; // given at the next poll
    bool counts_samples() override { return true; }
    void zpd_report(uint64_t ended, uint64_t next) override { reports.push_back({ended, next}); }
    void zpd_poll(std::vector<std::pair<uint64_t, uint64_t>> &resolved) override {
        resolved.insert(resolved.end(), answers.begin(), answers.end());
        answers.clear();
    }
};

void occlusion_queries() {
    Host host;
    Counter sink;
    CommandProcessor cp{*M, host};
    uint32_t ring = physical_alloc(0x1000), m = physical_alloc(0x1000);
    cp.set_sink(&sink);
    cp.initialize_ring_buffer(ring, 9);
    cp.set_occlusion_queries(OcclusionQueries::Fast);
    auto zpd = [&](uint32_t address) {
        Words w(ring + 4 * cp.read_pointer());
        w({type0(reg::RB_SAMPLE_COUNT_ADDR, 1), address});
        w({type3(pm4::EVENT_WRITE_ZPD, 1), 0x1B});
        cp.run((w.at - ring) / 4 + w.n);
    };
    auto z_pass = [&](uint32_t address) { return le32(address + 0x10) + le32(address + 0x14); };
    // the first report counts no draws (0), the next ones those since the last
    zpd(m + 0x00);
    CHECK(sink.reports.size() == 1 && sink.reports[0] == std::make_pair(uint64_t(0), uint64_t(1)));
    CHECK(z_pass(m + 0x00) == 1); // the guess: one sample
    sink.answers.push_back({0, 0});
    zpd(m + 0x20); // report 1 ended; report 0 answered with nothing: rewritten as 0
    CHECK(z_pass(m + 0x00) == 0);
    CHECK(z_pass(m + 0x20) == 1); // the guess over the right counter
    sink.answers.push_back({1, 500});
    zpd(m + 0x40); // report 1 answered with 500 samples: corrected, report 2 guessed over it
    CHECK(z_pass(m + 0x20) == 500);
    CHECK(z_pass(m + 0x40) == 501);
    CHECK(le32(m + 0x20) + le32(m + 0x24) == 500); // Total as ZPass
    sink.answers.push_back({2, 7});
    zpd(m + 0x20); // the address of report 1 again: its last count (500) is the guess
    CHECK(z_pass(m + 0x40) == 507);
    CHECK(z_pass(m + 0x20) == 1007);
    // without a sink that counts, the fake reports, on the same counter (as Xenia's)
    Recorder plain;
    cp.set_sink(&plain);
    zpd(m + 0x60);
    CHECK(z_pass(m + 0x60) == 1007 + 100);
}

// What changed in the register file: a packet's writes stamp a block only
// where a value differs from the one held, as a run and one by one; another
// thread's write (set) counts apart.
void register_stamps() {
    Fixture f;
    RegisterFile &r = f.cp.registers();
    uint32_t at = physical_alloc(0x1000);
    Words w(at);
    w({type0(0x4000, 40), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
       21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40});
    f.cp.execute_packets(at, 41);
    CHECK(r[0x4000] == 1 && r[0x401F] == 32 && r[0x4027] == 40);
    const uint64_t written = r.serial();
    CHECK(written != 0 && r.changed_since(0x4000, 40, 0) && !r.changed_since(0x4000, 40, written));
    CHECK(!r.changed_since(0x4040, 32, 0)); // a block not written
    // the same values again: nothing changed
    f.cp.execute_packets(at, 41);
    CHECK(r.serial() == written);
    // one value in the second block
    Words again(at + 0x400);
    again({type0(0x4021, 1), 99});
    f.cp.execute_packets(at + 0x400, 2);
    CHECK(r[0x4021] == 99 && r.changed_since(0x4020, 32, written) && !r.changed_since(0x4000, 32, written));
    // a register with a side effect goes one by one, stamped too
    const uint64_t before = r.serial(), foreign = r.foreign_writes();
    Words low(at + 0x800);
    low({type0(0x0300, 2), 7, 8, type3(pm4::SET_CONSTANT, 3), 4u << 16 | 0x100, 0x51, 0x52});
    f.cp.execute_packets(at + 0x800, 7);
    CHECK(r[0x0300] == 7 && r[0x0301] == 8 && r.changed_since(0x0300, 2, before));
    CHECK(r[0x2100] == 0x51 && r[0x2101] == 0x52 && r.changed_since(0x2100, 2, before));
    CHECK(r.foreign_writes() == foreign);
    r.set(0x2100, 1);
    CHECK(r.foreign_writes() == foreign + 1);
}

// The sink's work on a thread of its own (gpu/deferred_sink.h): the same calls in the same order,
// each with the registers as they stood at it; IM_LOAD's words read by the back end; a fence
// written by the back end once the work before it is done; a time synchronous.
void deferred_sink() {
    Host host;
    Recorder recorder;
    CommandProcessor cp{*M, host};
    uint32_t ring = physical_alloc(0x1000), m = physical_alloc(0x1000), code = physical_alloc(0x1000);
    cp.initialize_ring_buffer(ring, 9);
    DeferredSink deferred(recorder, cp.registers(), *M);
    cp.set_sink(&deferred);
    Words{code}({0xC0DE0001, 0xC0DE0002});
    Words w(ring);
    w({type3(pm4::IM_LOAD, 2), code | 1, 2});
    w({type3(pm4::IM_LOAD_IMMEDIATE, 4), 0, 2, 0xAAAA, 0xBBBB});
    w({type0(reg::RB_MODECONTROL, 1), 4});
    w({type3(pm4::DRAW_INDX_2, 1), 6u << 16 | 2u << 6 | 8});
    w({type0(reg::RB_MODECONTROL, 1), 5});
    w({type3(pm4::DRAW_INDX_2, 3), 3u << 16 | 1u << 6 | 4, 0x00050004, 0x0006});
    w({type3(pm4::EVENT_WRITE_SHD, 3), 0x15, (m + 0x20) | 2, 0xFE11CE});
    w({type0(reg::SCRATCH_UMSK, 2), 1, m + 0x40}); // SCRATCH_UMSK, SCRATCH_ADDR
    w({type0(reg::SCRATCH_REG0, 1), 0x5C});        // written back later
    w({type0(reg::RB_MODECONTROL, 1), 6});
    w({type3(pm4::DRAW_INDX, 2), 0, 3u << 16 | 2u << 6 | 8});
    w({type3(pm4::XE_SWAP, 4), pm4::SWAP_SIGNATURE, 0x1E000000, 1280, 720});
    cp.run(w.n);
    deferred.finish();
    CHECK(recorder.shaders.size() == 2);
    if (recorder.shaders.size() == 2) {
        CHECK(recorder.shaders[0].first == 1 &&
              recorder.shaders[0].second == (std::vector<uint32_t>{0xC0DE0001, 0xC0DE0002}));
        CHECK(recorder.shaders[1].second == (std::vector<uint32_t>{0xAAAA, 0xBBBB}));
    }
    CHECK(recorder.draws.size() == 2 && recorder.draw_modes == (std::vector<uint32_t>{4, 5}));
    if (recorder.draws.size() == 2)
        CHECK(recorder.draws[1].immediate == (std::vector<uint32_t>{0x00050004, 0x0006}) &&
              recorder.draws[1].count == 3 && recorder.draws[1].indexed);
    CHECK(recorder.resolves.size() == 1 && recorder.swaps.size() == 1);
    CHECK(be32(m + 0x20) == 0xFE11CE && be32(m + 0x40) == 0x5C);
    CHECK(deferred.stats().late_writes == 2);
    // synchronous: passed on at once, with the processor's registers
    deferred.set_synchronous(true);
    Words s(ring + 4 * w.n);
    s({type0(reg::RB_MODECONTROL, 1), 4});
    s({type3(pm4::DRAW_INDX_2, 1), 6u << 16 | 2u << 6 | 8});
    cp.run(w.n + s.n);
    CHECK(recorder.draws.size() == 3 && recorder.draw_modes.size() == 3 && recorder.draw_modes.back() == 4);
    // and deferred again, the back end's registers brought up to date in full
    deferred.set_synchronous(false);
    Words t(ring + 4 * (w.n + s.n));
    t({type0(reg::RB_MODECONTROL, 1), 5});
    t({type3(pm4::DRAW_INDX_2, 1), 6u << 16 | 2u << 6 | 8});
    cp.run(w.n + s.n + t.n);
    deferred.finish();
    CHECK(recorder.draws.size() == 4 && recorder.draw_modes.size() == 4 && recorder.draw_modes.back() == 5);
    cp.set_sink(nullptr);
}

int main() {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    M = memory.get();
    registers();
    memory_writes();
    special_registers();
    indirect_buffers();
    predication();
    draws_shaders_swaps();
    interrupts_and_waits();
    ring_wrap_and_bad_packets();
    trace_player();
    register_stamps();
    occlusion_queries();
    deferred_sink();
    return finish("command_processor");
}
