/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The command processor (command_processor.h). Translated from Xenia
// (BSD; see LICENSE-xenia): src/xenia/gpu/pm4_command_processor_implement.h
// (every packet), command_processor.cc (the ring buffer, register writes
// and their side effects, MakeCoherent, the gamma ramp's defaults and
// writes), xenos.h (GpuSwap, the packets' fields), xenos_zpd_report.h (the
// occlusion query report's layout) and base/ring_buffer (the reads and
// their wrapping); checked against them. Where it differs:
//
// - Draws and copies go to the Sink, as Xenia's null backend takes them
//   (every one succeeds). EVENT_WRITE_ZPD writes Xenia's fake reports (its
//   occlusion_query=fake, and its fallback without host queries) unless
//   OcclusionQueries::Fast and a sink that counts samples: then the reports
//   follow command_processor.cc's "fast" mode (QueueZPDReport,
//   PumpPendingRetire; 2026), with the ZPass counts of host occlusion
//   queries and without its VIZ queries, hybrid totals or the deadline for
//   reports that never resolve (the sink answers each). VIZ_QUERY marks
//   VGT_EVENT_INITIATOR and tells the sink, which measures the query's
//   survey draws and predicates its draws itself (Xenia's
//   occlusion_query_viz, there off by default; the renderer's "VIZ
//   queries").
// - EVENT_WRITE_EXT's screen extents are, with ScreenExtents::Measured and a
//   sink that measures, the bounds of what the report's draws rasterized
//   (beyond Xenia, which writes the whole screen; event_write_ext).
// - A type-3 packet ends where its header's count says, even when a handler
//   read less (IM_LOAD_IMMEDIATE's padding) or more; Xenia asserts the two
//   agree. Type 1 and XE_SWAP do not read past their buffer.
// - A WAIT_REG_MEM that does not match asks the Host to wait (a yield below
//   0x100, else wait / 0x100 ms): Xenia spins below 0x100 and sleeps above.
// - Indirect buffers nest at most 8 deep (Xenia: unbounded recursion).
// - Restoring a trace's gamma ramps starts the next sequential write at red
//   (Xenia restores the component the trace recorded).

#include "xenoforge/command_processor.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace xf::gpu {
namespace {

using namespace xenos;

constexpr uint32_t PHYSICAL_MASK = 0x1FFFFFFF;
constexpr uint32_t PHYSICAL_SIZE = 0x20000000;
constexpr uint32_t MAX_INDIRECT_DEPTH = 8;
// the constant blocks SET_CONSTANT and LOAD_ALU_CONSTANT write: ALU, fetch,
// bool, loop, and the registers from 0x2000
constexpr uint32_t CONSTANT_BASES[5] = {reg::ALU_CONSTANTS, reg::FETCH_CONSTANTS, reg::BOOL_CONSTANTS,
                                        reg::LOOP_CONSTANTS, 0x2000};

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t *p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
void put_le32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}
void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// WAIT_REG_MEM's and COND_WRITE's test (Xenia's MatchValueAndRef): the low 3
// bits of wait_info
bool match(uint32_t value, uint32_t ref, uint32_t wait_info) {
    switch (wait_info & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

} // namespace

// ---- the buffers being read ----

uint32_t CommandProcessor::Reader::next() {
    uint32_t v = be32(base + read);
    read += 4;
    if (read >= capacity) read = 0;
    return v;
}

uint32_t CommandProcessor::Reader::peek(uint32_t words_ahead) const {
    return be32(base + uint32_t((uint64_t(read) + 4ull * words_ahead) % capacity));
}

CommandProcessor::CommandProcessor(Memory &memory, Host &host) : memory_(memory), host_(host) {
    // the gamma ramps start linear (Xenia's CommandProcessor::Initialize,
    // what games set with VdGetCurrentDisplayGamma's sRGB)
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = i * 0x3FF / 0xFF;
        gamma_table_[i] = v | v << 10 | v << 20;
    }
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t base = (i * 0xFFFF / 0x7F) & ~0x3Fu, delta = i < 0x7F ? 0x200 : 0;
        for (uint32_t j = 0; j < 3; j++) gamma_pwl_[i * 3 + j] = base | delta << 16;
    }
}

// ---- guest memory as the GPU sees it ----

uint32_t CommandProcessor::load_swapped(uint32_t address, Endian e) const {
    return gpu_swap(le32(physical(address)), e);
}

void CommandProcessor::store_swapped(uint32_t address, uint32_t value, Endian e) {
    put_le32(physical(address), gpu_swap(value, e));
    if (sink_) sink_->memory_written(address & PHYSICAL_MASK, 4);
    host_.wrote_back();
}

void CommandProcessor::store_be32(uint32_t address, uint32_t value) {
    put_be32(physical(address), value);
    if (sink_) sink_->memory_written(address & PHYSICAL_MASK, 4);
    host_.wrote_back();
}

// ---- the ring buffer ----

void CommandProcessor::initialize_ring_buffer(uint32_t physical_address, uint32_t size_log2) {
    uint32_t start = physical_address & PHYSICAL_MASK;
    if (size_log2 + 3 >= 29 || start + (1u << (size_log2 + 3)) > PHYSICAL_SIZE) {
        std::fprintf(stderr, "gpu: a ring buffer of 2^%u bytes at 0x%08X is past physical memory\n", size_log2 + 3,
                     physical_address);
        return;
    }
    read_ptr_ = 0;
    primary_ptr_ = physical_address;
    primary_size_ = 1u << (size_log2 + 3);
    std::memset(physical(start), 0, primary_size_);
}

void CommandProcessor::enable_read_pointer_write_back(uint32_t physical_address, uint32_t block_size_log2) {
    // CP_RB_RPTR_ADDR, and CP_RB_CNTL's RB_BLKSZ: log2 of the quadwords read
    // between updates (Xenia writes back after every run, as here)
    read_ptr_writeback_ = physical_address;
    read_ptr_update_freq_ = block_size_log2 < 32 ? (1u << block_size_log2) >> 2 : 0;
}

void CommandProcessor::run(uint32_t write_index) {
    uint32_t read = execute_primary_buffer(read_ptr_, write_index);
    read_ptr_ = read;
    // the CPU's copy of the read pointer (CP_RB_RPTR_ADDR), big-endian
    if (uint32_t writeback = read_ptr_writeback_) store_be32(writeback, read);
}

uint32_t CommandProcessor::execute_primary_buffer(uint32_t read_index, uint32_t write_index) {
    uint32_t size = primary_size_, ptr = primary_ptr_;
    if (!size) return write_index;
    bump(counts_.primary_buffers);
    Reader saved = reader_;
    reader_ = {};
    reader_.base = physical(ptr);
    reader_.address = ptr & PHYSICAL_MASK;
    reader_.capacity = size;
    reader_.read = uint32_t((uint64_t(read_index) * 4) % size);
    reader_.write = uint32_t((uint64_t(write_index) * 4) % size);
    if (sink_) sink_->buffer_start(false, reader_.address + reader_.read, reader_.count() / 4);
    do {
        if (!execute_packet()) {
            // the rest of the run is dropped, as in Xenia
            bump(counts_.bad_packets);
            break;
        }
    } while (reader_.count());
    if (sink_) sink_->buffer_end(false);
    reader_ = saved;
    return write_index;
}

void CommandProcessor::execute_indirect_buffer(uint32_t physical_address, uint32_t count) {
    uint32_t start = physical_address & PHYSICAL_MASK;
    count = std::min(count, (PHYSICAL_SIZE - start) / 4);
    if (!count) return;
    if (depth_ >= MAX_INDIRECT_DEPTH) {
        std::fprintf(stderr, "gpu: an indirect buffer at 0x%08X nested %u deep: skipped\n", start, depth_);
        return;
    }
    bump(counts_.indirect_buffers);
    if (sink_) sink_->buffer_start(true, start, count);
    depth_++;
    execute_straight(start, count);
    depth_--;
    if (sink_) sink_->buffer_end(true);
}

void CommandProcessor::execute_packets(uint32_t physical_address, uint32_t count) {
    uint32_t start = physical_address & PHYSICAL_MASK;
    count = std::min(count, (PHYSICAL_SIZE - start) / 4);
    if (count) execute_straight(start, count);
}

void CommandProcessor::execute_straight(uint32_t start, uint32_t count) {
    Reader saved = reader_;
    // a straight run: the write offset is the capacity, which wraps to 0
    // (Xenia's RingBuffer), so the reads stop when they come back to it
    reader_ = {};
    reader_.base = physical(start);
    reader_.address = start;
    reader_.capacity = count * 4;
    do {
        if (!execute_packet()) {
            bump(counts_.bad_packets);
            break;
        }
    } while (reader_.count());
    reader_ = saved;
}

void CommandProcessor::restore_registers(uint32_t first, std::span<const uint32_t> values) {
    for (size_t i = 0; i < values.size() && first + i < RegisterFile::COUNT; i++)
        regs_.write(uint32_t(first + i), values[i]);
}

void CommandProcessor::restore_gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t> pwl) {
    std::copy_n(table.begin(), std::min(table.size(), gamma_table_.size()), gamma_table_.begin());
    std::copy_n(pwl.begin(), std::min(pwl.size(), gamma_pwl_.size()), gamma_pwl_.begin());
    gamma_component_ = 0;
    gamma_version_++;
}

// ---- packets ----

void CommandProcessor::report_packet_words(uint32_t address, uint32_t packet, uint32_t count) {
    words_.resize(1 + count);
    words_[0] = packet;
    for (uint32_t i = 0; i < count; i++) words_[1 + i] = reader_.peek(i);
    sink_->packet(address, words_);
}

bool CommandProcessor::execute_packet() {
    uint32_t address = reader_.address + reader_.read;
    uint32_t packet = reader_.next();
    bump(counts_.packets);
    uint32_t type = packet >> 30;
    // 0 and 0x0BADF00D are padding, type 2 a one-word no-op
    if (packet == 0 || packet == 0x0BADF00D || type == 2) {
        report_packet(address, packet, 0);
        end_packet();
        return true;
    }
    if (type == 0) return type0(address, packet);
    if (type == 1) return type1(address, packet);
    return type3(address, packet);
}

// count registers from base_index, or one register count times (bit 15)
bool CommandProcessor::type0(uint32_t address, uint32_t packet) {
    uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (reader_.count() < count * 4) {
        std::fprintf(stderr, "gpu: a type-0 packet of %u words with %u left\n", count, reader_.count() / 4);
        return false;
    }
    report_packet(address, packet, count);
    uint32_t base = packet & 0x7FFF;
    bool one_register = (packet >> 15) & 1;
    if (!one_register && reader_.read + count * 4 <= reader_.capacity) {
        // the words lie in a row in the buffer: written as a run
        write_register_run(base, reader_.base + reader_.read, count);
        reader_.skip(count * 4);
    } else {
        for (uint32_t i = 0; i < count; i++) write_register(one_register ? base : base + i, reader_.next());
    }
    end_packet();
    return true;
}

// two registers, each with its index in the header
bool CommandProcessor::type1(uint32_t address, uint32_t packet) {
    if (reader_.count() < 8) return false;
    report_packet(address, packet, 2);
    uint32_t value_1 = reader_.next(), value_2 = reader_.next();
    write_register(packet & 0x7FF, value_1);
    write_register((packet >> 11) & 0x7FF, value_2);
    end_packet();
    return true;
}

bool CommandProcessor::type3(uint32_t address, uint32_t packet) {
    uint32_t opcode = (packet >> 8) & 0x7F, count = ((packet >> 16) & 0x3FFF) + 1;
    if (reader_.count() < count * 4) {
        std::fprintf(stderr, "gpu: a %s packet of %u words with %u left\n", pm4_opcode_name(opcode), count,
                     reader_.count() / 4);
        return false;
    }
    report_packet(address, packet, count);
    bump(counts_.opcodes[opcode]);
    uint32_t data = reader_.read;
    // predicated (bit 0): only for the selected bins of predicated tiling; a
    // predicated swap never runs
    if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == pm4::XE_SWAP)) {
        if (opcode == pm4::DRAW_INDX || opcode == pm4::DRAW_INDX_2) extents_skipped_++;
        reader_.skip(count * 4);
        end_packet();
        return true;
    }
    if (opcode == pm4::DRAW_INDX || opcode == pm4::DRAW_INDX_2) extents_draws_++;
    bool ok = type3_opcode(opcode, count);
    reader_.read = uint32_t((uint64_t(data) + count * 4) % reader_.capacity);
    end_packet();
    return ok;
}

bool CommandProcessor::type3_opcode(uint32_t opcode, uint32_t count) {
    switch (opcode) {
    case pm4::ME_INIT:
        // the micro-engine's initialization words, kept
        me_bin_.resize(count);
        for (uint32_t i = 0; i < count; i++) me_bin_[i] = reader_.next();
        return true;
    case pm4::NOP: reader_.skip(count * 4); return true;
    case pm4::INTERRUPT: {
        uint32_t cpu_mask = reader_.next();
        // the CPU may read the screen extents in its interrupt
        poll_extents(true);
        for (uint32_t n = 0; n < 6; n++)
            if (cpu_mask & (1u << n)) {
                bump(counts_.interrupts);
                host_.interrupt(n);
            }
        return true;
    }
    case pm4::XE_SWAP: return xe_swap(count);
    case pm4::INDIRECT_BUFFER:
    case pm4::INDIRECT_BUFFER_PFD: {
        uint32_t list = reader_.next() & PHYSICAL_MASK;
        uint32_t length = reader_.next() & 0xFFFFF;
        execute_indirect_buffer(list, length);
        return true;
    }
    case pm4::WAIT_REG_MEM: return wait_reg_mem();
    case pm4::REG_RMW: {
        uint32_t rmw_info = reader_.next(), and_mask = reader_.next(), or_mask = reader_.next();
        uint32_t value = regs_[rmw_info & 0x1FFF];
        value &= (rmw_info >> 31) & 1 ? regs_[and_mask & 0x1FFF] : and_mask;
        value |= (rmw_info >> 30) & 1 ? regs_[or_mask & 0x1FFF] : or_mask;
        write_register(rmw_info & 0x1FFF, value);
        return true;
    }
    case pm4::REG_TO_MEM: {
        uint32_t reg_index = reader_.next(), address = reader_.next();
        store_swapped(address & ~3u, regs_[reg_index], Endian(address & 3));
        return true;
    }
    case pm4::MEM_WRITE: {
        uint32_t address = reader_.next();
        for (uint32_t i = 0; i + 1 < count; i++, address += 4)
            store_swapped(address & ~3u, reader_.next(), Endian(address & 3));
        return true;
    }
    case pm4::COND_WRITE: return cond_write();
    case pm4::EVENT_WRITE: {
        // an event flag; a write to an address (count > 1) is not known
        uint32_t initiator = reader_.next() & 0x3F;
        regs_.write(reg::VGT_EVENT_INITIATOR, initiator);
        if (initiator == EVENT_SCREEN_EXT_INIT && extents_ == ScreenExtents::Measured && sink_ &&
            sink_->measures_extents())
            sink_->extent_init();
        if (count > 1) reader_.skip((count - 1) * 4);
        return true;
    }
    case pm4::EVENT_WRITE_SHD: return event_write_shd();
    case pm4::EVENT_WRITE_EXT: return event_write_ext();
    case pm4::EVENT_WRITE_ZPD: return event_write_zpd();
    case pm4::DRAW_INDX: {
        uint32_t viz_query = reader_.next();
        return draw(viz_query, count - 1);
    }
    case pm4::DRAW_INDX_2: return draw(0, count);
    case pm4::SET_CONSTANT: {
        uint32_t offset_type = reader_.next();
        uint32_t index = offset_type & 0x7FF, kind = (offset_type >> 16) & 0xFF;
        if (kind > 4) {
            reader_.skip((count - 1) * 4);
            return true;
        }
        write_registers_from_ring(CONSTANT_BASES[kind] + index, count - 1);
        return true;
    }
    case pm4::SET_CONSTANT2:
    case pm4::SET_SHADER_CONSTANTS: {
        uint32_t index = reader_.next() & 0xFFFF;
        write_registers_from_ring(index, count - 1);
        return true;
    }
    case pm4::LOAD_ALU_CONSTANT: {
        uint32_t address = reader_.next() & 0x3FFFFFFF;
        uint32_t offset_type = reader_.next();
        uint32_t size = reader_.next() & 0xFFF;
        uint32_t index = offset_type & 0x7FF, kind = (offset_type >> 16) & 0xFF;
        if (kind > 4) return true;
        write_registers_from_memory(CONSTANT_BASES[kind] + index, address, size);
        return true;
    }
    case pm4::IM_LOAD: return im_load();
    case pm4::IM_LOAD_IMMEDIATE: return im_load_immediate(count);
    case pm4::INVALIDATE_STATE: reader_.next(); return true; // a mask of state pointers to drop
    case pm4::VIZ_QUERY: {
        uint32_t dword0 = reader_.next();
        regs_.write(reg::VGT_EVENT_INITIATOR, dword0 & 0x100 ? EVENT_VIZQUERY_END : EVENT_VIZQUERY_START);
        if (sink_) sink_->viz_query(dword0 & 0x3F, dword0 & 0x100);
        return true;
    }
    case pm4::SET_BIN_MASK_LO: bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader_.next(); return true;
    case pm4::SET_BIN_MASK_HI: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | uint64_t(reader_.next()) << 32; return true;
    case pm4::SET_BIN_SELECT_LO: bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader_.next(); return true;
    case pm4::SET_BIN_SELECT_HI:
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | uint64_t(reader_.next()) << 32;
        return true;
    case pm4::SET_BIN_MASK: {
        uint64_t hi = reader_.next(), lo = reader_.next();
        bin_mask_ = hi << 32 | lo;
        return true;
    }
    case pm4::SET_BIN_SELECT: {
        uint64_t hi = reader_.next(), lo = reader_.next();
        bin_select_ = hi << 32 | lo;
        return true;
    }
    case pm4::CONTEXT_UPDATE: reader_.next(); return true;
    // the indirect buffers run as they come, so there is nothing to wait for
    case pm4::WAIT_IB_PFD_COMPLETE: reader_.skip(count * 4); return true;
    case pm4::WAIT_FOR_IDLE: reader_.next(); return true;
    default:
        if (counts_.unknown_opcodes[opcode].load(std::memory_order_relaxed) == 0)
            std::fprintf(stderr, "gpu: PM4 opcode 0x%02X (%s) is not implemented\n", opcode, pm4_opcode_name(opcode));
        bump(counts_.unknown_opcodes[opcode]);
        reader_.skip(count * 4);
        return false;
    }
}

// ---- registers ----

void CommandProcessor::write_register(uint32_t index, uint32_t value) {
    if (index >= RegisterFile::COUNT) {
        std::fprintf(stderr, "gpu: a register write past the register file (0x%X)\n", index);
        return;
    }
    regs_.write(index, value);
    if (index - reg::SCRATCH_REG0 < 8 || index == reg::COHER_STATUS_HOST ||
        index - reg::DC_LUT_RW_INDEX <= reg::DC_LUT_30_COLOR - reg::DC_LUT_RW_INDEX)
        special_register_write(index, value);
}

// A run of registers: one loop over the words where no register in it has a
// side effect and all are in the file (a type-0 packet's state, SET_CONSTANT's
// and LOAD_ALU_CONSTANT's constants: a seventh of the command thread went to
// writing them one by one through write_register, each with its checks);
// XF_GPU_REGISTER_RUNS=0 writes them one by one again.
void CommandProcessor::write_register_run(uint32_t base, const uint8_t *words, uint32_t count) {
    static const bool runs = [] {
        const char *v = std::getenv("XF_GPU_REGISTER_RUNS");
        return !(v && v[0] == '0');
    }();
    if (runs && base >= PLAIN_REGISTERS && uint64_t(base) + count <= RegisterFile::COUNT) {
        regs_.write_big_endian(base, words, count);
        return;
    }
    for (uint32_t i = 0; i < count; i++) write_register(base + i, be32(words + 4 * i));
}

void CommandProcessor::write_registers_from_ring(uint32_t base, uint32_t count) {
    if (reader_.read + uint64_t(count) * 4 <= reader_.capacity) {
        write_register_run(base, reader_.base + reader_.read, count);
        reader_.skip(count * 4);
        return;
    }
    for (uint32_t i = 0; i < count; i++) write_register(base + i, reader_.next());
}

void CommandProcessor::write_registers_from_memory(uint32_t base, uint32_t address, uint32_t count) {
    if (sink_) sink_->memory_read(address & PHYSICAL_MASK, count * 4);
    // (in a row in physical memory, short of its end)
    if (uint64_t(address & PHYSICAL_MASK) + uint64_t(count) * 4 <= PHYSICAL_SIZE) {
        if (count) write_register_run(base, physical(address), count);
        return;
    }
    for (uint32_t i = 0; i < count; i++) write_register(base + i, be32(physical(address + 4 * i)));
}

// Xenia's HandleSpecialRegisterWrite
void CommandProcessor::special_register_write(uint32_t index, uint32_t value) {
    if (index - reg::SCRATCH_REG0 < 8) {
        // the scratch registers the mask enables are written back to memory
        uint32_t n = index - reg::SCRATCH_REG0;
        if ((1u << n) & regs_.get(reg::SCRATCH_UMSK)) store_be32(regs_.get(reg::SCRATCH_ADDR) + n * 4, value);
        return;
    }
    // the gamma ramp's write enable mask is blue, green, red; its sequential
    // writes red, green, blue
    auto component_enabled = [&] { return (regs_.get(reg::DC_LUT_WRITE_EN_MASK) >> (2 - gamma_component_)) & 1; };
    switch (index) {
    case reg::COHER_STATUS_HOST:
        // marked pending: the next WAIT_REG_MEM on it makes memory coherent
        regs_.write(index, regs_.get(index) | 0x80000000u);
        break;
    case reg::DC_LUT_RW_INDEX: gamma_component_ = 0; break;
    case reg::DC_LUT_SEQ_COLOR: {
        uint32_t rw_index = regs_.get(reg::DC_LUT_RW_INDEX);
        bool write = component_enabled();
        if (write) {
            // bits 0-5 are hardwired to zero: 10 bits of color
            uint32_t color = (regs_.get(reg::DC_LUT_SEQ_COLOR) & 0xFFFF) >> 6;
            uint32_t shift = gamma_component_ == 0 ? 20 : gamma_component_ == 1 ? 10 : 0;
            uint32_t &entry = gamma_table_[rw_index & 0xFF];
            entry = (entry & ~(0x3FFu << shift)) | color << shift;
        }
        if (++gamma_component_ >= 3) {
            gamma_component_ = 0;
            write_register(reg::DC_LUT_RW_INDEX, (rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF));
        }
        if (write) gamma_version_++;
        break;
    }
    case reg::DC_LUT_PWL_DATA: {
        uint32_t rw_index = regs_.get(reg::DC_LUT_RW_INDEX), pwl_index = rw_index & 0x7F; // bit 7 ignored
        bool write = component_enabled();
        if (write) {
            uint32_t data = regs_.get(reg::DC_LUT_PWL_DATA);
            gamma_pwl_[pwl_index * 3 + gamma_component_] = (data & 0xFFC0) | (data & 0xFFC00000);
        }
        if (++gamma_component_ >= 3) {
            gamma_component_ = 0;
            write_register(reg::DC_LUT_RW_INDEX, (rw_index & ~0x7Fu) | ((pwl_index + 1) & 0x7F));
        }
        if (write) gamma_version_++;
        break;
    }
    case reg::DC_LUT_30_COLOR: {
        uint32_t rw_index = regs_.get(reg::DC_LUT_RW_INDEX), mask = regs_.get(reg::DC_LUT_WRITE_EN_MASK) & 7;
        if (mask) {
            uint32_t color = regs_.get(reg::DC_LUT_30_COLOR), &entry = gamma_table_[rw_index & 0xFF];
            for (uint32_t c = 0; c < 3; c++)
                if (mask & (1u << c)) entry = (entry & ~(0x3FFu << (10 * c))) | (color & (0x3FFu << (10 * c)));
        }
        gamma_component_ = 0;
        write_register(reg::DC_LUT_RW_INDEX, (rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF));
        if (mask) gamma_version_++;
        break;
    }
    }
}

// COHER_STATUS_HOST's pending bit cleared: memory is coherent (there are no
// caches to flush here)
void CommandProcessor::make_coherent() {
    if (regs_.get(reg::COHER_STATUS_HOST) & 0x80000000u) regs_.write(reg::COHER_STATUS_HOST, 0);
}

// ---- waits, conditional writes, events ----

bool CommandProcessor::wait_reg_mem() {
    uint32_t wait_info = reader_.next(), poll = reader_.next(), ref = reader_.next(), mask = reader_.next();
    uint32_t wait = reader_.next();
    bool is_memory = wait_info & 0x10;
    for (bool first = true;; first = false) {
        uint32_t value;
        if (is_memory) {
            if (sink_) sink_->memory_read(poll & ~3u & PHYSICAL_MASK, 4);
            value = load_swapped(poll & ~3u, Endian(poll & 3));
        } else {
            if (poll == reg::COHER_STATUS_HOST) make_coherent();
            value = regs_[poll];
        }
        if (match(value & mask, ref, wait_info)) return true;
        bump(counts_.waits);
        // what was waited for, for the report (the kernel's video line)
        if (is_memory) bump(counts_.waits_on_memory);
        if (wait >= 0x100) bump(counts_.waits_asleep);
        if (first) {
            bump(counts_.waiting_packets);
            counts_.last_wait_poll.store((poll & 0x7FFFFFFF) | (is_memory ? 0x80000000u : 0u), std::memory_order_relaxed);
            counts_.last_wait_ref.store(ref, std::memory_order_relaxed);
            counts_.last_wait_mask.store(mask, std::memory_order_relaxed);
            counts_.last_wait_field.store(wait, std::memory_order_relaxed);
        }
        // the CPU may read the screen extents before it lets the wait end
        poll_extents(true);
        if (!host_.wait(wait)) return false;
    }
}

bool CommandProcessor::cond_write() {
    uint32_t wait_info = reader_.next(), poll = reader_.next(), ref = reader_.next(), mask = reader_.next();
    uint32_t write_address = reader_.next(), write_data = reader_.next();
    uint32_t value;
    if (wait_info & 0x10) {
        if (sink_) sink_->memory_read(poll & ~3u & PHYSICAL_MASK, 4);
        value = load_swapped(poll & ~3u, Endian(poll & 3));
    } else {
        value = regs_[poll];
    }
    if (!match(value & mask, ref, wait_info)) return true;
    if (wait_info & 0x100)
        store_swapped(write_address & ~3u, write_data, Endian(write_address & 3));
    else
        write_register(write_address, write_data);
    return true;
}

// a VS|PS done event: a value (or, with bit 31, the counter) written to
// memory; how the CPU learns a fence has passed
bool CommandProcessor::event_write_shd() {
    uint32_t initiator = reader_.next(), address = reader_.next(), value = reader_.next();
    regs_.write(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    uint32_t data = (initiator >> 31) & 1 ? counter() : value;
    data = gpu_swap(data, Endian(address & 3));
    address &= ~3u;
    uint8_t *destination = physical(address);
    if (address > PHYSICAL_MASK) {
        // the writeback range (WRITEBACK_START/SIZE) is seen at 0x7F000000
        uint32_t base = regs_.get(reg::WRITEBACK_START), size = regs_.get(reg::WRITEBACK_SIZE);
        if (base && address - base < size) destination = memory_.translate(0x7F000000 + (address - base));
    }
    put_le32(destination, data);
    if (sink_) sink_->memory_written(address & PHYSICAL_MASK, 4);
    host_.wrote_back();
    return true;
}

// A screen extent event (SCREEN_EXT_RPT): the area the draws since the last
// SCREEN_EXT_INIT touched, six 16-bit words (x, y and z, min and max; x and
// y in 8-pixel units, as Xenia's >> 3 writes them) byte-swapped in 16 bits
// (Xenia asserts that endianness and writes it so). Written at once as Xenia
// writes it, the whole of the largest texture; with ScreenExtents::Measured
// and a sink that measures, written again with the sink's bounds when they
// are in (poll_extents), and those are in before the next interrupt or wait,
// after which the CPU may read them. Direct3D's predicated tiling reads them
// to choose the tiles a draw runs in: a bound too small would drop the
// draw's pixels in a tile, one too large only runs it in more tiles, so the
// bounds are rounded outwards. The z range stays 0-1 (a hardware question,
// as is the rounding the console does).
bool CommandProcessor::event_write_ext() {
    uint32_t initiator = reader_.next(), address = reader_.next() & ~3u;
    regs_.write(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
    uint64_t handle = 0;
    if (extents_ == ScreenExtents::Measured && sink_ && sink_->measures_extents()) {
        handle = sink_->extent_report();
        if (handle) extent_pending_.push_back({handle, address});
        // (the latest report at the address, or none: kept, not erased, a
        // frame's reports being at the addresses of the frame before's)
        extent_latest_.at(address) = handle;
    }
    // (a report the sink measures is written again with its bounds before the
    // CPU may read it: nothing polling the write-backs is woken for this
    // one, and the sink is told of the bytes once, with the bounds)
    write_extent(address, extents, handle == 0);
    return true;
}

// (told: the sink told of the bytes and what polls the write-backs woken;
// else the caller does both, for many reports at once)
void CommandProcessor::write_extent(uint32_t address, const uint16_t (&extents)[6], bool told) {
    uint8_t *p = physical(address);
    for (int i = 0; i < 6; i++) p[2 * i] = uint8_t(extents[i] >> 8), p[2 * i + 1] = uint8_t(extents[i]);
    if (!told) return;
    if (sink_) sink_->memory_written(address & PHYSICAL_MASK, sizeof(extents));
    host_.wrote_back();
}

// The sink's answers to the screen extent reports in (all of them, waited
// for, with wait), each written where no later report has been since.
void CommandProcessor::poll_extents(bool wait) {
    if (extent_first_ == extent_pending_.size() || !sink_) return;
    if (wait) sink_->extent_wait();
    extent_resolved_.clear();
    sink_->extent_poll(extent_resolved_);
    // (the reports' records are an array in guest memory: the sink told of
    // each run of neighbours once, not of every record)
    bool written = false;
    uint32_t run_start = 0, run_end = 0;
    auto tell = [&] {
        if (run_end > run_start) sink_->memory_written(run_start, run_end - run_start);
        run_start = run_end = 0;
    };
    for (const Extent &e : extent_resolved_) {
        // (the answers come in the reports' order: found at the first one waiting, or near it)
        size_t at = extent_first_;
        while (at < extent_pending_.size() && extent_pending_[at].first != e.handle) at++;
        if (at == extent_pending_.size()) continue;
        const uint32_t address = extent_pending_[at].second;
        extent_pending_[at].first = 0;
        while (extent_first_ < extent_pending_.size() && !extent_pending_[extent_first_].first) extent_first_++;
        uint64_t *latest = extent_latest_.find(address);
        if (!latest || *latest != e.handle) continue;
        *latest = 0;
        // (the whole screen is there already, written with the report: the sink told of it now)
        const uint32_t physical_address = address & PHYSICAL_MASK;
        // (a record is 16 bytes, 12 of them written: the 4 between two neighbours go with them)
        if (run_end == run_start || physical_address < run_start || physical_address > run_end + 4)
            tell(), run_start = run_end = physical_address;
        run_end = std::max(run_end, physical_address + 12);
        if (e.whole) continue;
        // in 8-pixel units, outwards: the left and top down, the right and
        // bottom (exclusive) up
        uint32_t right = std::min<uint32_t>(e.right, 8192), bottom = std::min<uint32_t>(e.bottom, 8192);
        const uint16_t extents[6] = {uint16_t(std::min(e.left, right) >> 3), uint16_t((right + 7) >> 3),
                                     uint16_t(std::min(e.top, bottom) >> 3), uint16_t((bottom + 7) >> 3), 0, 1};
        write_extent(address, extents, false);
        written = true;
    }
    tell();
    if (extent_first_ == extent_pending_.size()) extent_pending_.clear(), extent_first_ = 0;
    // (what polls the write-backs woken once for all of them)
    if (written) host_.wrote_back();
}

// a z-pass done event: an occlusion query's report (xe_gpu_depth_sample_
// counts, little-endian, each counter split in two lanes) at
// RB_SAMPLE_COUNT_ADDR
bool CommandProcessor::event_write_zpd() {
    regs_.write(reg::VGT_EVENT_INITIATOR, reader_.next() & 0x3F);
    uint32_t report = regs_.get(reg::RB_SAMPLE_COUNT_ADDR);
    if (!report) return true;
    if (occlusion_ == OcclusionQueries::Fast && sink_ && sink_->counts_samples()) {
        queue_zpd(report);
        return true;
    }
    // Xenia's occlusion_query_fake_lower_threshold (80) and _upper (100)
    fake_zpd_samples_ = fake_zpd_samples_ <= 80 ? 100 : fake_zpd_samples_ - 1;
    zpd_speculative_counter_ += fake_zpd_samples_;
    zpd_counter_ = zpd_speculative_counter_;
    write_zpd(report, zpd_counter_);
    return true;
}

// XenosZPDReport::WriteTo with only samples that passed: Total, ZFail, ZPass,
// StencilFail, each split in two lanes
void CommandProcessor::write_zpd(uint32_t address, uint64_t z_pass) {
    uint32_t count = uint32_t(z_pass);
    uint32_t lane_a = count - (count >> 1), lane_b = count >> 1;
    const uint32_t values[8] = {lane_a, lane_b, 0, 0, lane_a, lane_b, 0, 0};
    uint8_t *p = physical(address);
    for (int i = 0; i < 8; i++) put_le32(p + 4 * i, values[i]);
    if (sink_) sink_->memory_written(address & PHYSICAL_MASK, sizeof(values));
    host_.wrote_back();
}

// QueueZPDReport: the report of the draws since the last event (the current
// report) written at once with a guess, the last count at this address when
// it was not zero, else one sample (unknown means visible); the draws from
// here count for the next report
void CommandProcessor::queue_zpd(uint32_t address) {
    sink_->zpd_report(zpd_current_, zpd_next_);
    ZpdReport r;
    r.address = address;
    r.handle = zpd_current_;
    auto cached = zpd_cached_deltas_.find(address);
    r.speculative_delta = cached != zpd_cached_deltas_.end() && cached->second ? cached->second : 1;
    zpd_speculative_counter_ += r.speculative_delta;
    r.speculative_value = zpd_speculative_counter_;
    write_zpd(address, r.speculative_value);
    zpd_reports_.push_back(r);
    zpd_current_ = zpd_next_++;
    poll_zpd();
}

// the sink's counts in, then the reports retired
void CommandProcessor::poll_zpd() {
    if (zpd_reports_.empty() || !sink_) return;
    std::vector<std::pair<uint64_t, uint64_t>> resolved;
    sink_->zpd_poll(resolved);
    for (auto [handle, samples] : resolved) {
        if (zpd_reports_.empty() || handle < zpd_reports_.front().handle) continue;
        size_t index = size_t(handle - zpd_reports_.front().handle);
        if (index >= zpd_reports_.size() || zpd_reports_[index].handle != handle) continue;
        zpd_reports_[index].delta += samples;
        zpd_reports_[index].pending = false;
    }
    retire_zpd();
}

// PumpPendingRetire: the reports whose counts are in, in order, added to the
// counter; a report the guess got wrong written again, and the guesses
// after it made again over the right counter
void CommandProcessor::retire_zpd() {
    bool rebase = false;
    while (!zpd_reports_.empty() && !zpd_reports_.front().pending) {
        ZpdReport &front = zpd_reports_.front();
        zpd_counter_ += front.delta;
        if (zpd_cached_deltas_.size() >= 1024 && !zpd_cached_deltas_.count(front.address)) zpd_cached_deltas_.clear();
        zpd_cached_deltas_[front.address] = front.delta;
        if (front.speculative_value != zpd_counter_) {
            write_zpd(front.address, zpd_counter_);
            rebase = true;
        }
        zpd_reports_.pop_front();
    }
    if (rebase) {
        uint64_t running = zpd_counter_;
        for (ZpdReport &r : zpd_reports_) {
            running += r.speculative_delta;
            if (r.speculative_value != running) {
                write_zpd(r.address, running);
                r.speculative_value = running;
            }
        }
        zpd_speculative_counter_ = running;
    } else if (zpd_reports_.empty()) {
        zpd_speculative_counter_ = zpd_counter_;
    }
}

// ---- draws, shaders, swaps ----

bool CommandProcessor::draw(uint32_t viz_query, uint32_t remaining) {
    if (!remaining) return false; // no VGT_DRAW_INITIATOR
    Draw d;
    d.viz_query = viz_query;
    d.initiator = reader_.next();
    remaining--;
    regs_.write(reg::VGT_DRAW_INITIATOR, d.initiator);
    d.primitive = d.initiator & 0x3F;
    d.source = (d.initiator >> 6) & 3;
    uint32_t major_mode = (d.initiator >> 8) & 3;
    d.index32 = (d.initiator >> 11) & 1;
    d.count = d.initiator >> 16;
    d.major_mode_explicit = major_mode != 0 || d.primitive >= 0x10;
    bool ok = true;
    switch (SourceSelect(d.source)) {
    case SourceSelect::DMA: {
        // indexed: VGT_DMA_BASE, VGT_DMA_SIZE (num_words, swap_mode)
        if (!remaining) return false;
        uint32_t base = reader_.next() & PHYSICAL_MASK;
        remaining--;
        regs_.write(reg::VGT_DMA_BASE, base);
        if (!remaining) return false;
        uint32_t size = reader_.next();
        remaining--;
        regs_.write(reg::VGT_DMA_SIZE, size);
        uint32_t index_size = d.index32 ? 4 : 2;
        d.indexed = true;
        d.index_base = base & ~(index_size - 1);
        d.index_endian = Endian(size >> 30);
        d.index_bytes = (size & 0xFFFFFF) * index_size;
        break;
    }
    case SourceSelect::AutoIndex: break;
    case SourceSelect::Immediate: {
        // The indices in the packet (Xenia fails these draws, a TODO there):
        // AMD's DRAW_INDEX_IMMD gives them after the initiator, 32-bit ones a
        // word each, 16-bit ones two a word with the first in the low half.
        // The words are the ring's, already as the CPU wrote them; which half
        // a big-endian CPU's first index lands in only hardware can say.
        uint32_t words = d.index32 ? d.count : (d.count + 1) / 2;
        if (words > remaining) {
            ok = false;
            break;
        }
        d.immediate.resize(words);
        for (uint32_t i = 0; i < words; i++) d.immediate[i] = reader_.next();
        remaining -= words;
        d.indexed = true;
        d.index_bytes = d.count * (d.index32 ? 4 : 2);
        bump(counts_.immediate_draws);
        break;
    }
    default:
        // no valid source
        ok = false;
        break;
    }
    reader_.skip(remaining * 4);
    if (!ok) {
        bump(counts_.failed_draws);
        return true;
    }
    if (EdramMode(regs_.get(reg::RB_MODECONTROL) & 7) == EdramMode::Copy) {
        bump(counts_.resolves);
        if (sink_) sink_->resolve(regs_, d);
    } else {
        bump(counts_.draws);
        if (sink_) sink_->draw(regs_, d);
    }
    return true;
}

// the shader's microcode from memory: type in the address's low 2 bits,
// start and size in dwords
bool CommandProcessor::im_load() {
    uint32_t address_type = reader_.next(), start_size = reader_.next();
    uint32_t type = address_type & 3, address = address_type & ~3u, size = start_size & 0xFFFF;
    if (type > 1) return false;
    if (sink_) {
        sink_->memory_read(address & PHYSICAL_MASK, size * 4);
        // the same shader from unwritten memory: nothing to copy
        if (sink_->shader_again(type, address & PHYSICAL_MASK, size)) return true;
        words_.resize(size);
        for (uint32_t i = 0; i < size; i++) words_[i] = be32(physical(address + 4 * i));
        sink_->shader(type, address & PHYSICAL_MASK, words_);
    }
    return true;
}

// the shader's microcode in the packet
bool CommandProcessor::im_load_immediate(uint32_t count) {
    if (count < 2) return false;
    uint32_t type = reader_.next(), start_size = reader_.next();
    uint32_t size = std::min(start_size & 0xFFFF, count - 2);
    if (type > 1) return false;
    if (sink_) {
        uint32_t address = reader_.address + reader_.read;
        words_.resize(size);
        for (uint32_t i = 0; i < size; i++) words_[i] = reader_.next();
        sink_->shader(type, address, words_);
    }
    return true;
}

// VdSwap's packet: 'SWAP', the front buffer (physical), its width and height
bool CommandProcessor::xe_swap(uint32_t count) {
    uint32_t words[4] = {};
    for (uint32_t i = 0; i < std::min(count, 4u); i++) words[i] = reader_.next();
    Swap s{words[1], words[2], words[3]};
    bump(counts_.swaps);
    if (sink_) sink_->swap(regs_, s);
    // XF_SLOW_FRAMES=<ms>: each frame that took longer from swap to swap is
    // printed with what the processor did in it: how long it waited
    // (WAIT_REG_MEM), its draws, resolves and extent reports, and whether the
    // extents were measured. For finding what a frame that misses its vblank
    // is made of.
    {
        static const double limit = [] {
            const char *v = std::getenv("XF_SLOW_FRAMES");
            return v ? std::strtod(v, nullptr) : 0.0;
        }();
        if (limit > 0) {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t waited = counts_.wait_ns.load(), draws = counts_.draws.load();
            const uint64_t resolves = counts_.resolves.load(), reports = counts_.opcodes[pm4::EVENT_WRITE_EXT].load();
            const double ms = std::chrono::duration<double, std::milli>(now - slow_at_).count();
            const Sink::Work work = sink_ ? sink_->work() : Sink::Work{};
            // (XF_SLOW_FRAMES=<ms>,all: every frame, for setting the slow ones beside the others)
            static const bool all = [] {
                const char *v = std::getenv("XF_SLOW_FRAMES");
                return v && std::strstr(v, ",all") != nullptr;
            }();
            if (slow_swaps_ && (ms > limit || all))
                std::printf("%s frame: swap %llu, %.1f ms (%.1f ms in waits, %.1f ms waiting for the GPU), %llu draws, "
                            "%llu resolves, %llu extent reports, %llu texture loads, %llu submissions, %llu pipelines%s\n",
                            ms > limit ? "slow" : "a", (unsigned long long)counts_.swaps.load(), ms,
                            double(waited - slow_waited_) / 1e6,
                            (work.gpu_wait_seconds - slow_work_.gpu_wait_seconds) * 1000.0,
                            (unsigned long long)(draws - slow_draws_), (unsigned long long)(resolves - slow_resolves_),
                            (unsigned long long)(reports - slow_reports_),
                            (unsigned long long)(work.texture_loads - slow_work_.texture_loads),
                            (unsigned long long)(work.submissions - slow_work_.submissions),
                            (unsigned long long)(work.pipelines - slow_work_.pipelines),
                            extents_ == ScreenExtents::Measured ? ", extents measured" : "");
            slow_work_ = work;
            slow_at_ = now, slow_waited_ = waited, slow_draws_ = draws, slow_resolves_ = resolves, slow_reports_ = reports;
            slow_swaps_++;
        }
    }
    // the screen extents by their gain: a trial of measuring judged, or a
    // stay with the whole screen over
    if (extents_by_gain_ && ++extents_swaps_ >= extents_window_) {
        if (extents_ == ScreenExtents::Measured &&
            extents_skipped_ * EXTENTS_GAIN < extents_draws_ + extents_skipped_)
            extents_ = ScreenExtents::Whole, extents_window_ = EXTENTS_STAY;
        else
            extents_ = ScreenExtents::Measured, extents_window_ = EXTENTS_TRIAL;
        extents_swaps_ = 0, extents_draws_ = extents_skipped_ = 0;
    }
    // the counts of the frame's reports, as they come in, and the extents
    poll_zpd();
    poll_extents(false);
    counter_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace xf::gpu
