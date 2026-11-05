// The GPU's command processor, live (plan step A4, "GPU live";
// docs/runtime.md, "The GPU"): the ring buffer the CPU fills and the
// indirect buffers it points at, executed packet by packet with their
// effects on the GPU's registers and on guest memory, as Xenia's
// CommandProcessor executes them (src/xenia/gpu/command_processor.cc,
// pm4_command_processor_implement.h, graphics_system.cc's registers; BSD,
// see LICENSE-xenia): register writes and the scratch write-back, fences
// (MEM_WRITE, EVENT_WRITE_SHD), waits on memory or a register, interrupts,
// the display's gamma ramp, occlusion query reports.
//
// It draws nothing. Draws, resolves, swaps and shader loads go to a Sink
// with the register file standing as they happen, so boot counts them and
// the renderer can take them later in place of a trace's packets (the
// headless backend is Xenia's null command processor: every draw and copy
// succeeds and leaves no picture).
//
// No kernel here: guest memory is Memory's physical view, and what the
// processor needs from the system (an interrupt on a CPU, a wait between
// polls) comes through Host. One thread executes packets; the register
// file may be read and written from others (the CPU's MMIO), as on the
// console.

#pragma once

#include "xenoforge/memory.h"
#include "xenoforge/xenos.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace xf::gpu {

// How the GPU swaps a word it reads or writes in memory: the low 2 bits of
// the address a packet gives (Xenia's xenos::Endian and GpuSwap).
enum class Endian : uint32_t { None = 0, In16 = 1, In32 = 2, Half = 3 };
constexpr uint32_t gpu_swap(uint32_t v, Endian e) {
    switch (e) {
    case Endian::In16: return (v << 8 & 0xFF00FF00) | (v >> 8 & 0x00FF00FF);
    case Endian::In32: return __builtin_bswap32(v);
    case Endian::Half: return v >> 16 | v << 16;
    default: return v;
    }
}

// The GPU's registers (Xenia's RegisterFile), by index (xenos.h's reg). Each
// access is a relaxed atomic one: the CPU reads and writes some of them
// through MMIO while the command processor runs.
class RegisterFile {
public:
    static constexpr uint32_t COUNT = xenos::REGISTER_COUNT;
    RegisterFile() : values_(COUNT, 0) {}
    uint32_t operator[](uint32_t index) const { return index < COUNT ? get(index) : 0; }
    uint32_t get(uint32_t index) const {
        return std::atomic_ref<uint32_t>(const_cast<uint32_t &>(values_[index])).load(std::memory_order_relaxed);
    }
    void set(uint32_t index, uint32_t value) {
        std::atomic_ref<uint32_t>(values_[index]).store(value, std::memory_order_relaxed);
    }
    // all of them, for whoever runs on the command processor's thread (a
    // Sink's callbacks): constants copied in bulk
    const uint32_t *data() const { return values_.data(); }

private:
    std::vector<uint32_t> values_;
};

// A draw as DRAW_INDX and DRAW_INDX_2 give it (VGT_DRAW_INITIATOR's fields);
// the rest of what it needs is in the registers.
struct Draw {
    uint32_t initiator = 0; // VGT_DRAW_INITIATOR
    uint32_t primitive = 0; // its prim_type
    uint32_t source = 0;    // its source_select: 0 DMA (indexed), 2 auto
    uint32_t count = 0;     // num_indices
    bool major_mode_explicit = false;
    uint32_t viz_query = 0; // DRAW_INDX's VIZ token (bit 8: predicated on query bits 0-5)
    // indexed: the index buffer (Xenia's IndexBufferInfo), physical
    bool indexed = false;
    bool index32 = false;
    uint32_t index_base = 0, index_bytes = 0;
    Endian index_endian = Endian::None;
};

// XE_SWAP, the packet VdSwap writes: the front buffer, physical
struct Swap {
    uint32_t frontbuffer = 0, width = 0, height = 0;
};

// What the command processor reports, on its thread, in order. The defaults
// do nothing. Xenia's backend calls (IssueDraw, IssueCopy, IssueSwap,
// LoadShader) and its trace writer's (packets, buffers, memory read and
// written), so a renderer or a trace writer can stand behind it.
class Sink {
public:
    virtual ~Sink() = default;
    // a primary (indirect false) or indirect buffer begins or ends
    virtual void buffer_start(bool /*indirect*/, uint32_t /*address*/, uint32_t /*dwords*/) {}
    virtual void buffer_end(bool /*indirect*/) {}
    // a packet, before it runs: its address (physical) and its words, host order
    virtual void packet(uint32_t /*address*/, std::span<const uint32_t> /*words*/) {}
    // memory the processor read (constants, shaders, polled words) or wrote
    // (fences, write-backs, reports), physical
    virtual void memory_read(uint32_t /*address*/, uint32_t /*bytes*/) {}
    virtual void memory_written(uint32_t /*address*/, uint32_t /*bytes*/) {}
    // a shader loaded (IM_LOAD, IM_LOAD_IMMEDIATE): 0 vertex, 1 pixel
    virtual void shader(uint32_t /*type*/, uint32_t /*address*/, std::span<const uint32_t> /*code*/) {}
    // a draw outside copy mode, and a resolve (a draw with RB_MODECONTROL in
    // copy mode: EDRAM to memory, RB_COPY_*)
    virtual void draw(const RegisterFile &, const Draw &) {}
    virtual void resolve(const RegisterFile &, const Draw &) {}
    virtual void swap(const RegisterFile &, const Swap &) {}
};

// What the command processor counts, read from any thread.
struct Counts {
    std::atomic<uint64_t> packets{0};         // of every type, executed or skipped
    std::atomic<uint64_t> primary_buffers{0}; // primary buffer runs
    std::atomic<uint64_t> indirect_buffers{0};
    std::atomic<uint64_t> draws{0}, resolves{0}, swaps{0};
    std::atomic<uint64_t> failed_draws{0};                    // immediate indices or no valid source, as Xenia reports
    std::atomic<uint64_t> interrupts{0};                      // INTERRUPT's, one per CPU in its mask
    std::atomic<uint64_t> waits{0};                           // WAIT_REG_MEM polls that did not match
    std::atomic<uint64_t> bad_packets{0};                     // ones that stopped their buffer
    std::array<std::atomic<uint64_t>, 128> opcodes{};         // type-3 packets by opcode
    std::array<std::atomic<uint64_t>, 128> unknown_opcodes{}; // the ones it has no handler for
};

class CommandProcessor {
public:
    // What the processor asks of the system.
    class Host {
    public:
        virtual ~Host() = default;
        // PM4_INTERRUPT for one CPU: the guest's graphics interrupt callback
        // with source 1 (Xenia's DispatchInterruptCallback(1, cpu)), run
        // before the next packet
        virtual void interrupt(uint32_t cpu) = 0;
        // A WAIT_REG_MEM that did not match: wait before polling again (the
        // packet's wait field: wait / 0x100 ms from 0x100 up, else no more
        // than a yield). False: stop executing (the GPU is shutting down).
        virtual bool wait(uint32_t wait) = 0;
    };

    CommandProcessor(Memory &memory, Host &host);

    void set_sink(Sink *sink) { sink_ = sink; }
    RegisterFile &registers() { return regs_; }
    const Counts &counts() const { return counts_; }

    // The ring buffer (VdInitializeRingBuffer): physical, 2^(size_log2 + 3)
    // bytes, zeroed; the read pointer back at its start.
    void initialize_ring_buffer(uint32_t physical, uint32_t size_log2);
    // Where the read pointer is written back after each run
    // (VdEnableRingBufferRPtrWriteBack), physical; 0 none.
    void enable_read_pointer_write_back(uint32_t physical, uint32_t block_size_log2);
    uint32_t read_pointer_write_back() const { return read_ptr_writeback_; }
    uint32_t ring_buffer() const { return primary_ptr_; }
    uint32_t ring_buffer_bytes() const { return primary_size_; }
    // where the processor has read the ring buffer to, in dwords
    uint32_t read_pointer() const { return read_ptr_; }

    // Executes the ring buffer from the read pointer to write_index (the
    // CPU's CP_RB_WPTR, in dwords), then writes the read pointer back (the
    // body of Xenia's WorkerThreadMain).
    void run(uint32_t write_index);
    // Executes the ring buffer from read_index to write_index (in dwords,
    // wrapping) and returns the new read index (Xenia's
    // ExecutePrimaryBuffer). A packet that cannot run ends the run, as in
    // Xenia.
    uint32_t execute_primary_buffer(uint32_t read_index, uint32_t write_index);
    // Executes count dwords of packets at a physical address (an indirect
    // buffer, a test's packets).
    void execute_indirect_buffer(uint32_t physical, uint32_t count);

    // A register written by a packet: stored, with the side effects some
    // have (the scratch registers' write-back, COHER_STATUS_HOST, the gamma
    // ramp's). The CPU's MMIO writes store without them, as Xenia's.
    void write_register(uint32_t index, uint32_t value);

    // The counter EVENT_WRITE_SHD writes when asked (bit 31 of its
    // initiator): every vblank and every swap move it (Xenia's counter_).
    uint32_t counter() const { return counter_.load(std::memory_order_relaxed); }
    void increment_counter() { counter_.fetch_add(1, std::memory_order_relaxed); }

    // The display's gamma ramps, as DC_LUT_* leave them: 256 DC_LUT_30_COLOR
    // entries (blue, green, red in 10 bits each from bit 0) and 128
    // DC_LUT_PWL_DATA entries of red, green and blue ([entry * 3 + component];
    // base in bits 0-15, delta in 16-31); the version changes with them.
    const std::array<uint32_t, 256> &gamma_ramp_table() const { return gamma_table_; }
    const std::array<uint32_t, 128 * 3> &gamma_ramp_pwl() const { return gamma_pwl_; }
    uint64_t gamma_ramp_version() const { return gamma_version_; }

private:
    // a buffer being read: a ring (the primary buffer) or a straight run (an
    // indirect buffer), as Xenia's RingBuffer
    struct Reader {
        uint8_t *base = nullptr;
        uint32_t address = 0;                       // physical
        uint32_t capacity = 0, read = 0, write = 0; // bytes
        uint32_t count() const { return read <= write ? write - read : capacity - read + write; }
        uint32_t next();
        uint32_t peek(uint32_t words_ahead) const;
        void skip(uint32_t bytes) { read = capacity ? uint32_t((uint64_t(read) + bytes) % capacity) : 0; }
    };

    bool execute_packet();
    bool type0(uint32_t packet);
    bool type1(uint32_t packet);
    bool type3(uint32_t packet);
    bool type3_opcode(uint32_t opcode, uint32_t count);
    void report_packet(uint32_t address, uint32_t words);
    void special_register_write(uint32_t index, uint32_t value);
    void write_registers_from_ring(uint32_t base, uint32_t count);
    void write_registers_from_memory(uint32_t base, uint32_t physical, uint32_t count);
    void make_coherent();
    bool wait_reg_mem();
    bool cond_write();
    bool event_write_shd();
    bool event_write_ext();
    bool event_write_zpd();
    bool draw(uint32_t viz_query, uint32_t remaining);
    bool im_load();
    bool im_load_immediate(uint32_t count);
    bool xe_swap(uint32_t count);

    // guest memory, physical: words as the GPU reads and writes them
    uint8_t *physical(uint32_t address) const { return memory_.translate_physical(address); }
    uint32_t load_swapped(uint32_t address, Endian e) const;        // a word, swapped as e says
    void store_swapped(uint32_t address, uint32_t value, Endian e); // the inverse
    void store_be32(uint32_t address, uint32_t value);
    void bump(std::atomic<uint64_t> &n) { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

    Memory &memory_;
    Host &host_;
    Sink *sink_ = nullptr;
    RegisterFile regs_;
    Counts counts_;
    Reader reader_;
    uint32_t depth_ = 0;          // indirect buffers inside indirect buffers
    std::vector<uint32_t> words_; // a packet's words, for the sink

    // set by the CPU's thread (the Vd* exports), read by the processor's
    std::atomic<uint32_t> primary_ptr_{0}, primary_size_{0}, read_ptr_{0};
    std::atomic<uint32_t> read_ptr_writeback_{0}, read_ptr_update_freq_{0};
    std::atomic<uint32_t> counter_{0};
    uint64_t bin_select_ = 0xFFFFFFFFull, bin_mask_ = 0xFFFFFFFFull;
    std::vector<uint32_t> me_bin_; // ME_INIT's micro-engine words

    // EVENT_WRITE_ZPD: Xenia's fake reports (its occlusion_query=fake and
    // the fallback without host queries): a sample count running down from
    // 100 to 80 and around, added to a free-running counter
    uint32_t fake_zpd_samples_ = 0;
    uint64_t zpd_counter_ = 0;

    std::array<uint32_t, 256> gamma_table_{};
    std::array<uint32_t, 128 * 3> gamma_pwl_{};
    uint32_t gamma_component_ = 0; // the next of red, green, blue DC_LUT_SEQ_COLOR and DC_LUT_PWL_DATA write
    uint64_t gamma_version_ = 0;
};

} // namespace xf::gpu
