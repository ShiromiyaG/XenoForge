// The sound family's state (audio.cpp, xma.cpp; docs/runtime.md, "Sound"):
// the APU as the games' code sees it, headless. Two parts, each with a
// kernel thread of its own whose body is host code (Xenia's XHostThreads):
//
// - Audio: the render driver's clients (XAudioRegisterRenderDriverClient).
//   "Audio Worker" calls each client's guest callback once a frame period
//   (256 samples at 48 kHz) while its output has a free slot of the 64 a
//   client may have in flight; the frames the client submits go to its
//   output (audio::Output, the pacing one by default).
// - Xma: the XMA decoder's registers at 0x7FEA0000 and its 320 contexts in
//   physical memory. A kick (a register write, XMAEnableContext) wakes "XMA
//   Decoder", which walks each kicked context's input as Xenia's default
//   context does (packets, frames, subframes, loops, the two input buffers,
//   the output ring buffer) with each frame decoded by audio::FrameDecoder,
//   and the kicking thread waits until it is done.

#pragma once

#include "objects.h"
#include "xenoforge/audio.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xf::kernel {

class Audio {
public:
    static constexpr uint32_t MAX_CLIENTS = 8;         // Xenia's kMaximumClientCount
    static constexpr uint32_t QUEUED_FRAMES = 64;      // a client's frames in flight (kMaximumQueuedFrames)
    static constexpr uint32_t DRIVER_TAG = 0x41550000; // 'AU', the high half of a client's driver handle

    void setup(Kernel &k);
    // the worker, once (the first call into the audio driver)
    void start();
    // the worker stopped and joined, the clients let go
    void stop();

    // X_STATUS or an HRESULT; *index the client's
    uint32_t register_client(uint32_t callback, uint32_t callback_arg, uint32_t &index);
    void unregister_client(uint32_t index);
    // a driver handle a registration gave and no unregistration took back (the console's calls refuse any other)
    bool valid_client(uint32_t driver);
    // a frame of 6 x 256 big-endian floats at guest address samples
    void submit_frame(uint32_t index, uint32_t samples);

    Kernel *k = nullptr;
    std::function<std::unique_ptr<audio::Output>()> make_output;

    // counts: clients registered, callbacks run, frames submitted, callbacks
    // due with no free slot, frames submitted to no client
    std::atomic<uint64_t> registered{0}, callbacks{0}, frames{0}, skipped{0}, submitted_to_none{0};
    // the frames submitted, sampled once a second by the worker: (milliseconds
    // since the kernel started, frames)
    std::vector<std::pair<uint32_t, uint64_t>> frame_samples() {
        std::lock_guard<std::mutex> l(mutex_);
        return frame_samples_;
    }

private:
    struct Client {
        bool in_use = false;
        uint32_t callback = 0, callback_arg = 0;
        // guest memory holding callback_arg: what the callback gets (Xenia's
        // wrapped_callback_arg)
        uint32_t wrapped_arg = 0;
        // when the callback is next due, in thirds of the clock's 100 ns (a
        // frame lasts 160000 of them exactly)
        uint64_t next_pump = 0;
        uint32_t slots = 0; // free frame slots (Xenia's semaphore count)
        std::unique_ptr<audio::Output> output;
    };

    void worker(Thread &self);
    void set_idle(bool idle);

    std::mutex mutex_; // the clients; taken before the dispatcher lock, never after
    std::array<Client, MAX_CLIENTS> clients_;
    int32_t calling_ = -1;               // the client whose callback runs now
    std::vector<uint32_t> pending_free_; // arguments freed once the callback in progress returns
    std::vector<std::pair<uint32_t, uint64_t>> frame_samples_;
    uint32_t next_sample_ms_ = 1000;
    std::once_flag started_;
    std::atomic<bool> running_{false};
    bool started_thread_ = false;
    // work_ wakes the worker (a client came or went, the kernel stops);
    // idle_ is set while no callback runs (an unregistration waits for it)
    Ref<Event> work_, idle_;
    Ref<Thread> thread_;
    std::vector<float> frame_; // a frame made host floats
};

// XMA_CONTEXT_DATA (64 bytes, big-endian words in guest memory), as Xenia's
// xma_context.h lays it out: each word's fields from its least significant
// bit up. Loaded and stored whole; the reserved bits travel along.
struct XmaContextData {
    // DWORD 0
    uint32_t input_buffer_0_packet_count : 12; // 2 KB packets
    uint32_t loop_count : 8;
    uint32_t input_buffer_0_valid : 1;
    uint32_t input_buffer_1_valid : 1;
    uint32_t output_buffer_block_count : 5; // 256-byte blocks
    uint32_t output_buffer_write_offset : 5;
    // DWORD 1
    uint32_t input_buffer_1_packet_count : 12;
    uint32_t loop_subframe_start : 2;
    uint32_t loop_subframe_end : 3;
    uint32_t loop_subframe_skip : 3;
    uint32_t subframe_decode_count : 4;
    uint32_t output_buffer_padding : 3; // extra output blocks reserved per frame (Xenia's guess)
    uint32_t sample_rate : 2;           // 24, 32, 44.1, 48 kHz
    uint32_t is_stereo : 1;
    uint32_t unk_dword_1_c : 1;
    uint32_t output_buffer_valid : 1;
    // DWORD 2
    uint32_t input_buffer_read_offset : 26; // in bits
    uint32_t error_status : 5;
    uint32_t error_set : 1;
    // DWORD 3
    uint32_t loop_start : 26; // in bits
    uint32_t parser_error_status : 5;
    uint32_t parser_error_set : 1;
    // DWORD 4
    uint32_t loop_end : 26; // in bits
    uint32_t packet_metadata : 5;
    uint32_t current_buffer : 1;
    // DWORDs 5-8: physical addresses
    uint32_t input_buffer_0_ptr;
    uint32_t input_buffer_1_ptr;
    uint32_t output_buffer_ptr;
    uint32_t work_buffer_ptr;
    // DWORD 9
    uint32_t output_buffer_read_offset : 5;
    uint32_t dword_9_reserved : 25;
    uint32_t stop_when_done : 1;
    uint32_t interrupt_when_done : 1;
    // DWORDs 10-15
    uint32_t reserved[6];

    XmaContextData() { std::memset(this, 0, sizeof(*this)); }
    explicit XmaContextData(const uint8_t *guest) {
        uint32_t *w = reinterpret_cast<uint32_t *>(this);
        std::memcpy(w, guest, 64);
        for (int i = 0; i < 16; i++) w[i] = __builtin_bswap32(w[i]);
    }
    void store(uint8_t *guest) const {
        uint32_t w[16];
        std::memcpy(w, this, 64);
        for (uint32_t &v : w) v = __builtin_bswap32(v);
        std::memcpy(guest, w, 64);
    }
    uint32_t word(int i) const {
        uint32_t w[16];
        std::memcpy(w, this, 64);
        return w[i];
    }

    bool input_buffer_valid(uint32_t buffer) const { return buffer == 0 ? input_buffer_0_valid : input_buffer_1_valid; }
    bool current_input_buffer_valid() const { return input_buffer_valid(current_buffer); }
    bool any_input_buffer_valid() const { return input_buffer_0_valid || input_buffer_1_valid; }
    uint32_t input_buffer_address(uint32_t buffer) const {
        return buffer == 0 ? input_buffer_0_ptr : input_buffer_1_ptr;
    }
    uint32_t input_buffer_packet_count(uint32_t buffer) const {
        return buffer == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
    }
    bool consume_only() const { return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0; }
};
static_assert(sizeof(XmaContextData) == 64);

class Xma {
public:
    static constexpr uint32_t CONTEXT_COUNT = 320; // Xenia's kContextCount
    static constexpr uint32_t REGISTERS = 0x4000;  // 64 KB of 32-bit registers
    static constexpr uint32_t CONTEXT_ARRAY_ADDRESS = 0x600, CURRENT_CONTEXT_INDEX = 0x606, NEXT_CONTEXT_INDEX = 0x607;
    static constexpr uint32_t KICK = 0x650, LOCK = 0x690, CLEAR = 0x6A0; // ten registers each, 32 contexts a register

    Xma();
    ~Xma();
    // as the kernel starts: the context array, the registers' MMIO range
    void setup(Kernel &k);
    // the decoder's thread, once
    void start();
    void stop();

    uint32_t allocate_context();          // its guest address, 0 when none is free
    void release_context(uint32_t guest); // a context XMACreateContext gave
    bool block_on_context(uint32_t guest, bool poll);
    // the register write for a context (Xenia's StoreXmaContextIndexedRegister)
    void store_indexed_register(uint32_t base_address, uint32_t guest);

    // the CPU's MMIO (Xenia's XmaDecoder::ReadRegister, WriteRegister): the
    // value as a lwz reads it and a stw writes it (the registers are
    // little-endian; the games reach them with lwbrx and stwbrx)
    uint32_t read_register(uint32_t address);
    void write_register(uint32_t address, uint32_t value);

    Kernel *k = nullptr;
    uint32_t context_array = 0; // the first context's guest address (0xE0000000 and up)
    std::unique_ptr<audio::FrameDecoder> decoder;

    // counts: frames the decoder gave samples for, frames it gave none
    // (FFmpeg's errors, or more input wanted), frames with a sample not zero;
    // errors are bad input (a frame of the invalid length, a read offset past
    // its buffer, a split frame whose rest is not there), stalls a run cut at
    // its pass limit, starved a run with room to write and no input (an
    // underrun: the game reads what it wrote before); the contexts ever kicked
    std::atomic<uint64_t> created{0}, released{0}, kicks{0}, works{0}, frames{0}, packets_skipped{0}, swaps{0};
    std::atomic<uint64_t> loops{0}, errors{0}, frames_failed{0}, frames_with_sound{0}, stalls{0}, starved{0};
    std::atomic<uint32_t> contexts_kicked{0}, peak{0}; // peak: the largest |sample| decoded

    // XF_XMA_RECORD=<file>, or this, before the first kick: every run of a
    // context written to the file (its data before and after, its input and
    // output buffers), and the clears and releases, for replaying the walk
    // against Xenia's (tests/xma_replay_test.cpp, tools/xenia_oracle)
    bool record_to(const std::string &path);

private:
    struct Context;
    struct Recorder;
    std::unique_ptr<Recorder> recorder_;
    void worker(Thread &self);
    bool work(Context &x);
    void kick(uint32_t first, uint32_t mask);
    void complete(Context &x, uint64_t generation); // lock held
    void set_enabled(Context &x, bool enabled);     // lock held
    int32_t context_id(uint32_t guest) const;

    std::unique_ptr<Context[]> contexts_;
    // the contexts kicked and not yet taken, a bit each (x.enabled's copy,
    // written under the dispatcher lock): the worker looks here and takes
    // the lock only for a context that has a kick, not for all 320
    std::array<std::atomic<uint32_t>, CONTEXT_COUNT / 32> pending_{};
    std::mutex registers_mutex_;
    std::array<uint32_t, REGISTERS> registers_{};
    std::vector<bool> logged_; // unhandled registers logged once
    std::mutex allocation_mutex_;
    std::once_flag started_;
    std::atomic<bool> running_{false};
    bool started_thread_ = false;
    Ref<Event> work_; // wakes the worker (a kick, the kernel stopping)
    Ref<Thread> thread_;
};

Audio &audio(Kernel &k); // the family's state, the worker started
Xma &xma(Kernel &k);     // the decoder's, its thread started

} // namespace xf::kernel
