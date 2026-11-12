/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The XMA decoder (docs/runtime.md, "Sound"): its registers at 0x7FEA0000,
// the 320 contexts in physical memory, kick, lock and clear, the decoder's
// thread, each context's walk through its input, and the XMA* exports.
// Translated from Xenia (BSD; see LICENSE-xenia): apu/xma_decoder.cc (Setup,
// AllocateContext, ReleaseContext, BlockOnContext, ReadRegister,
// WriteRegister, WorkerThreadMain), apu/xma_register_table.inc,
// apu/xma_context.h (XMA_CONTEXT_DATA, in audio.h), apu/xma_context_new.cc
// (the default context, xma_decoder = "new": Work, Clear, Release, Decode,
// Consume and the packet and frame searches), apu/xma_helpers.h,
// base/bit_stream.cc and base/ring_buffer.cc (what the walk uses), and
// kernel/xboxkrnl/xboxkrnl_audio_xma.cc (the exports); checked against them.
// Where it differs:
//
// - A frame's decode is audio::FrameDecoder's one call (Xenia: FFmpeg's
//   xmaframes decoder inside the context): FFmpeg's, as Xenia drives it
//   (src/xma_decoder.cpp), unless something else is set; the walk around it,
//   which finds the frame, builds its packet and moves the offsets, is
//   Xenia's.
// - The decoder's thread waits on a kernel event, and a kicking guest thread
//   waits through the kernel's objects until the decoder has walked its
//   contexts (Xenia: host events). A kick from a host thread with no guest
//   thread (the kernel's own calls) is walked there, as Xenia does without
//   its dedicated thread.
// - A kicked context that is not allocated, or that a lock disables before
//   the decoder reaches it, completes its kick unwalked (Xenia's kicker
//   waits for ever).
// - Releasing a context also forgets its walk (a frame half consumed, the
//   loop's state); Xenia keeps it for the context's next owner until a
//   clear.
// - An output buffer of no blocks gives no room (Xenia divides by zero); a
//   frame's bits are copied only as far as the two packets in hand reach;
//   one run of a context stops after 65536 passes (Xenia's could loop for
//   ever on a loop region with no frame in it).
// - Unhandled registers are logged once each (Xenia logs every access).
// - The runs can be recorded (XF_XMA_RECORD, Xma::record_to) and the walk
//   counts what it decodes, what has sound and what goes wrong (the report).

#include "audio.h"
#include "xenoforge/xma_recording.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace xf::kernel {
namespace {

constexpr uint32_t XMA_REGISTERS = 0x7FEA0000, XMA_REGISTERS_MASK = 0xFFFF0000;
constexpr uint32_t STACK = 128 * 1024;
constexpr uint32_t CONTEXT_SIZE = 64;

// XmaContext's and XmaContextNew's constants
constexpr uint32_t BYTES_PER_PACKET = 2048, BYTES_PER_PACKET_HEADER = 4;
constexpr uint32_t BYTES_PER_PACKET_DATA = BYTES_PER_PACKET - BYTES_PER_PACKET_HEADER;
constexpr uint32_t BITS_PER_PACKET = BYTES_PER_PACKET * 8, BITS_PER_PACKET_HEADER = 32, BITS_PER_FRAME_HEADER = 15;
constexpr uint32_t MAX_FRAME_SIZE_IN_BITS = 0x4000 - BITS_PER_PACKET_HEADER;
constexpr uint32_t SAMPLES_PER_FRAME = 512, BYTES_PER_FRAME_CHANNEL = SAMPLES_PER_FRAME * 2;
constexpr uint32_t OUTPUT_BYTES_PER_BLOCK = 256;
constexpr uint32_t MAX_FRAME_LENGTH = 0x7FFF; // a frame size that ends the packet's frames
constexpr uint32_t SAMPLE_RATES[4] = {24000, 32000, 44100, 48000};

constexpr uint32_t X_E_FALSE = 0x80000000;

// the registers' names (xma_register_table.inc), for the log
const char *register_name(uint32_t r) {
    switch (r) {
    case Xma::CONTEXT_ARRAY_ADDRESS: return "ContextArrayAddress";
    case Xma::CURRENT_CONTEXT_INDEX: return "CurrentContextIndex";
    case Xma::NEXT_CONTEXT_INDEX: return "NextContextIndex";
    }
    if (r >= 0x610 && r < 0x61A) return "Unknown610";
    if (r >= 0x620 && r < 0x62A) return "Unknown620";
    if (r >= Xma::KICK && r < Xma::KICK + 10) return "Kick";
    if (r >= 0x660 && r < 0x66A) return "Unknown660";
    if (r >= Xma::LOCK && r < Xma::LOCK + 10) return "Lock";
    if (r >= Xma::CLEAR && r < Xma::CLEAR + 10) return "Clear";
    return "unknown";
}

// ---- a packet's header (xma_helpers.h) ----

// the frames that begin in it (XMA2)
uint8_t packet_frame_count(const uint8_t *p) { return p[0] >> 2; }
uint8_t packet_metadata(const uint8_t *p) { return p[2] & 0x7; }
bool packet_is_xma2(const uint8_t *p) { return packet_metadata(p) == 1; }
uint8_t packet_skip_count(const uint8_t *p) { return p[3]; }
// the first frame's offset in bits from the packet's start
uint32_t packet_frame_offset(const uint8_t *p) {
    return uint32_t(uint16_t(((p[0] & 0x3) << 13) | (p[1] << 5) | (p[2] >> 3))) + 32;
}

// ---- bits, most significant first (base/bit_stream.cc) ----

class BitStream {
public:
    BitStream(const uint8_t *buffer, size_t size_bits) : buffer_(buffer), size_(size_bits) {}
    size_t offset() const { return offset_; }
    void set_offset(size_t offset) { offset_ = std::min(offset, size_); }
    void advance(size_t bits) { set_offset(offset_ + bits); }
    size_t bits_remaining() const { return size_ - offset_; }

    // up to 57 bits, from the 8 bytes holding them (never past the end)
    uint64_t peek(size_t bits) const {
        size_t offset_bytes = std::min(offset_ >> 3, (size_ - 64) >> 3);
        size_t rel = offset_ - (offset_bytes << 3);
        uint64_t v;
        std::memcpy(&v, buffer_ + offset_bytes, 8);
        v = __builtin_bswap64(v);
        v >>= 64 - (rel + bits);
        return v & ((1ull << bits) - 1);
    }
    uint64_t read(size_t bits) {
        uint64_t v = peek(bits);
        advance(bits);
        return v;
    }
    // bits copied to dest at the same alignment in their first byte: that
    // alignment (the bits before the first in it)
    size_t copy(uint8_t *dest, size_t bits) {
        size_t offset_bytes = offset_ >> 3, rel = offset_ - (offset_bytes << 3);
        size_t left = bits, out = 0;
        if (rel) {
            uint64_t v = peek(8 - rel);
            dest[out] &= uint8_t(~((1u << rel) - 1));
            dest[out] |= uint8_t(v);
            left -= 8 - rel;
            advance(8 - rel);
            out++;
        }
        if (left >= 8) {
            std::memcpy(dest + out, buffer_ + offset_bytes + out, left / 8);
            out += left / 8;
            advance((left / 8) * 8);
            left -= (left / 8) * 8;
        }
        if (left) {
            uint64_t v = peek(left) << (8 - left);
            dest[out] &= uint8_t((1u << left) - 1);
            dest[out] |= uint8_t(v);
            advance(left);
        }
        return rel;
    }

private:
    const uint8_t *buffer_;
    size_t offset_ = 0, size_;
};

// ---- the output buffer as a ring (base/ring_buffer.cc) ----

class RingBuffer {
public:
    RingBuffer(uint8_t *buffer, uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}
    uint32_t capacity() const { return capacity_; }
    bool empty() const { return read_ == write_; }
    uint32_t write_offset() const { return write_; }
    void set_read_offset(uint32_t offset) { read_ = capacity_ ? offset % capacity_ : 0; }
    void set_write_offset(uint32_t offset) { write_ = capacity_ ? offset % capacity_ : 0; }
    // the room to write: all of it when the offsets meet
    uint32_t write_count() const {
        if (read_ == write_) return capacity_;
        if (write_ < read_) return read_ - write_;
        return capacity_ - write_ + read_;
    }
    void write(const uint8_t *data, uint32_t count) {
        count = std::min(count, capacity_);
        if (!count) return;
        if (write_ + count < capacity_) {
            std::memcpy(buffer_ + write_, data, count);
            write_ += count;
        } else {
            uint32_t left = capacity_ - write_, right = count - left;
            std::memcpy(buffer_ + write_, data, left);
            std::memcpy(buffer_, data + left, right);
            write_ = right;
        }
    }

private:
    uint8_t *buffer_;
    uint32_t capacity_, read_ = 0, write_ = 0;
};

struct PacketInfo {
    uint8_t frame_count = 0, current_frame = 0;
    uint32_t current_frame_size = 0;
    bool last_frame_in_packet() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

struct PacketHandle {
    uint32_t buffer = 0, packet = 0;
    bool valid = false;
};

void set_event(Objects &o, Event &e) {
    std::lock_guard<std::mutex> l(o.lock);
    e.set_signal_state(1);
    o.signaled(e);
}

} // namespace

void set_xma_decoder(Kernel &k, std::unique_ptr<audio::FrameDecoder> decoder) {
    k.state<Xma>().decoder = std::move(decoder);
}

// ---- a context (XmaContext, XmaContextNew) ----

struct Xma::Context {
    Xma *owner = nullptr;
    uint32_t id = 0, guest = 0;
    std::atomic<bool> allocated{false}, kicked{false};

    // kicks, under the dispatcher lock: enabled until the decoder takes it;
    // each kick a generation; done set while every kick so far is complete
    bool enabled = false;
    uint64_t generation = 0, completed = 0, walking = 0, cancelled = 0;
    Ref<Event> done;

    // the walk, under lock (held while the decoder walks it)
    std::mutex lock;
    int32_t remaining_subframe_blocks = 0;
    uint8_t current_frame_remaining_subframes = 0;
    uint8_t loop_frame_output_limit = 0; // subframes to output from this frame (loop end); 0 none
    bool loop_start_skip_pending = false;
    std::array<uint8_t, BYTES_PER_FRAME_CHANNEL * 2> raw_frame{}; // the frame's PCM, 16-bit big-endian

    Kernel &k() { return *owner->k; }
    uint8_t *physical(uint32_t address) { return k().memory().translate_physical(address); }

    void walk();
    void clear_locked(XmaContextData &data);
    void forget();
    RingBuffer output_ring(XmaContextData &data);
    void swap_input_buffer(XmaContextData &data);
    void consume(RingBuffer &output, const XmaContextData &data);
    void decode(XmaContextData &data);
    void update_loop_status(XmaContextData &data);
    PacketInfo packet_info(const uint8_t *packet, uint32_t frame_offset);
    PacketHandle packet_handle(const XmaContextData &data, uint32_t buffer, uint32_t packet, uint32_t count);
    const uint8_t *next_packet(const XmaContextData &data, uint32_t next_packet_index, uint32_t count);
    uint32_t next_packet_read_offset(const uint8_t *buffer, uint32_t next_packet_index, uint32_t count);
    uint32_t next_packet_read_offset(const XmaContextData &data, uint32_t next_packet_index, uint32_t count);
    void store_merged(const XmaContextData &data, const XmaContextData &initial, uint8_t *context);
};

// PrepareOutputRingBuffer: 256-byte blocks from the read offset to the write
// offset hold what the game has not read yet
RingBuffer Xma::Context::output_ring(XmaContextData &data) {
    RingBuffer output(physical(data.output_buffer_ptr), data.output_buffer_block_count * OUTPUT_BYTES_PER_BLOCK);
    output.set_read_offset(data.output_buffer_read_offset * OUTPUT_BYTES_PER_BLOCK);
    output.set_write_offset(data.output_buffer_write_offset * OUTPUT_BYTES_PER_BLOCK);
    remaining_subframe_blocks = int32_t(output.write_count() / OUTPUT_BYTES_PER_BLOCK);
    return output;
}

// Work, after the decoder took the kick (lock held)
void Xma::Context::walk() {
    uint8_t *context = k().host(guest);
    XmaContextData data(context);
    const XmaContextData initial = data;
    if (!data.output_buffer_valid) return;
    if (!data.output_buffer_block_count) return; // no room at all
    RingBuffer output = output_ring(data);
    // the output is written through the physical view, which no write watch
    // protects: the watches are told of the whole buffer as the walk ends
    struct Written {
        Memory &memory;
        uint32_t physical, bytes;
        ~Written() { memory.physical_written(physical, bytes); }
    } written{k().memory(), uint32_t(data.output_buffer_ptr) & 0x1FFFFFFF,
              uint32_t(data.output_buffer_block_count) * OUTPUT_BYTES_PER_BLOCK};

    if (data.consume_only()) {
        // nothing to drain: the offsets stay as the game left them
        if (current_frame_remaining_subframes == 0) return;
        consume(output, data);
        data.output_buffer_write_offset = output.write_offset() / OUTPUT_BYTES_PER_BLOCK;
        if (output.empty()) data.output_buffer_valid = 0;
        store_merged(data, initial, context);
        return;
    }

    // the blocks one pass writes (the subframes Consume hands over, and the
    // padding asked for)
    const uint32_t effective_sdc = std::max(1u, uint32_t(data.subframe_decode_count));
    const int32_t minimum_subframe_decode_count = int32_t(effective_sdc) + int32_t(data.output_buffer_padding);
    if (minimum_subframe_decode_count > remaining_subframe_blocks) {
        store_merged(data, initial, context);
        return;
    }

    for (uint32_t passes = 0; remaining_subframe_blocks >= minimum_subframe_decode_count; passes++) {
        if (passes == 65536) {
            owner->stalls++;
            break;
        }
        const uint32_t pre_decode_offset = data.input_buffer_read_offset;
        const uint8_t pre_remaining_subframes = current_frame_remaining_subframes;
        decode(data);
        consume(output, data);
        // a frame partly consumed is drained before the walk stops: nothing
        // else would deliver the rest
        if ((!data.any_input_buffer_valid() || data.error_status == 4) && current_frame_remaining_subframes == 0) break;
        // no progress: the next buffer is awaited
        if (pre_remaining_subframes == 0 && data.input_buffer_read_offset == pre_decode_offset &&
            current_frame_remaining_subframes == 0)
            break;
    }

    if (initial.any_input_buffer_valid()) {
        data.output_buffer_write_offset = output.write_offset() / OUTPUT_BYTES_PER_BLOCK;
    } else {
        owner->starved++;
        if (data.output_buffer_write_offset != data.output_buffer_read_offset) {
            // starved of input: the write offset meets the read offset (games
            // take a difference there for a stall)
            data.output_buffer_write_offset = data.output_buffer_read_offset;
            data.output_buffer_valid = 0;
        }
    }
    // the output buffer full
    if (remaining_subframe_blocks == 0 && output.empty()) data.output_buffer_valid = 0;
    store_merged(data, initial, context);
}

void Xma::Context::clear_locked(XmaContextData &data) {
    data.input_buffer_0_valid = 0;
    data.input_buffer_1_valid = 0;
    data.output_buffer_valid = 0;
    data.input_buffer_read_offset = BITS_PER_PACKET_HEADER;
    data.output_buffer_read_offset = 0;
    data.output_buffer_write_offset = 0;
    forget();
}

void Xma::Context::forget() {
    current_frame_remaining_subframes = 0;
    loop_frame_output_limit = 0;
    loop_start_skip_pending = false;
}

// the current buffer is done: the other one, from its first packet
void Xma::Context::swap_input_buffer(XmaContextData &data) {
    if (data.current_buffer == 0)
        data.input_buffer_0_valid = 0;
    else
        data.input_buffer_1_valid = 0;
    data.current_buffer ^= 1;
    data.input_buffer_read_offset = BITS_PER_PACKET_HEADER;
    owner->swaps++;
}

// the decoded frame's subframes to the output, subframe_decode_count at most
// at a time
void Xma::Context::consume(RingBuffer &output, const XmaContextData &data) {
    if (!current_frame_remaining_subframes) return;
    const uint8_t total_subframes = uint8_t((BYTES_PER_FRAME_CHANNEL / OUTPUT_BYTES_PER_BLOCK) << data.is_stereo);
    // the loop's end reached in this frame: the rest is dropped
    if (loop_frame_output_limit > 0) {
        const uint8_t consumed = uint8_t(total_subframes - current_frame_remaining_subframes);
        if (consumed >= loop_frame_output_limit) {
            remaining_subframe_blocks -= data.output_buffer_padding;
            current_frame_remaining_subframes = 0;
            loop_frame_output_limit = 0;
            return;
        }
    }
    const uint8_t effective_sdc = uint8_t(std::max(1u, uint32_t(data.subframe_decode_count)));
    int8_t subframes_to_write = std::min(int8_t(current_frame_remaining_subframes), int8_t(effective_sdc));
    if (loop_frame_output_limit > 0) {
        const uint8_t consumed = uint8_t(total_subframes - current_frame_remaining_subframes);
        const int8_t remaining_until_limit = int8_t(loop_frame_output_limit - consumed);
        if (subframes_to_write > remaining_until_limit) subframes_to_write = remaining_until_limit;
    }
    const int8_t raw_frame_read_offset = int8_t(total_subframes - current_frame_remaining_subframes);
    output.write(raw_frame.data() + OUTPUT_BYTES_PER_BLOCK * raw_frame_read_offset,
                 uint32_t(subframes_to_write) * OUTPUT_BYTES_PER_BLOCK);
    // the padding's blocks once the frame is all out
    const int8_t headroom =
        current_frame_remaining_subframes - subframes_to_write == 0 ? int8_t(data.output_buffer_padding) : 0;
    remaining_subframe_blocks -= subframes_to_write + headroom;
    current_frame_remaining_subframes -= subframes_to_write;
}

// the next frame found, decoded, and the read offset moved past it
void Xma::Context::decode(XmaContextData &data) {
    if (!data.any_input_buffer_valid()) return;
    if (current_frame_remaining_subframes > 0) return;
    if (!data.current_input_buffer_valid()) {
        swap_input_buffer(data);
        if (!data.current_input_buffer_valid()) return;
    }
    const uint8_t *current_input_buffer = physical(data.input_buffer_address(data.current_buffer));
    // the packet's data and the next's, headers left out
    std::array<uint8_t, BYTES_PER_PACKET_DATA * 2> input{};

    // the loop's end frame, before UpdateLoopStatus moves the offset
    bool is_loop_end_frame = false;
    if (data.loop_count > 0) {
        const uint32_t loop_end = std::max(BITS_PER_PACKET_HEADER, uint32_t(data.loop_end));
        is_loop_end_frame = data.input_buffer_read_offset == loop_end;
    }
    update_loop_status(data);

    if (!data.output_buffer_block_count) return;
    // an offset in the packet's header (games kick before setting one)
    if (data.input_buffer_read_offset < BITS_PER_PACKET_HEADER) data.input_buffer_read_offset = BITS_PER_PACKET_HEADER;

    const uint32_t current_input_size = data.input_buffer_packet_count(data.current_buffer) * BYTES_PER_PACKET;
    const uint32_t current_input_packet_count = current_input_size / BYTES_PER_PACKET;
    // GetPacketNumber
    const uint32_t read_offset = data.input_buffer_read_offset;
    if (read_offset >= uint64_t(current_input_size) * 8) {
        owner->errors++;
        return;
    }
    const uint32_t packet_index = (read_offset >> 3) / BYTES_PER_PACKET;

    const uint8_t *packet = current_input_buffer + packet_index * BYTES_PER_PACKET;
    const uint32_t packet_first_frame_offset = packet_frame_offset(packet);
    uint32_t relative_offset = data.input_buffer_read_offset % BITS_PER_PACKET;
    // the tail of a frame from the previous packet: its start is not here, so
    // the walk goes on from the first frame that begins in this one
    if (relative_offset < packet_first_frame_offset) {
        data.input_buffer_read_offset = packet_index * BITS_PER_PACKET + packet_first_frame_offset;
        relative_offset = packet_first_frame_offset;
    }
    const uint8_t skip_count = packet_skip_count(packet);

    // no frame begins in this packet: the next one
    if (skip_count == 0xFF) {
        owner->packets_skipped++;
        const uint32_t next_index = packet_index + 1;
        const uint32_t next_offset = next_packet_read_offset(data, next_index, current_input_packet_count);
        if (next_index >= current_input_packet_count || next_offset == BITS_PER_PACKET_HEADER) swap_input_buffer(data);
        data.input_buffer_read_offset = next_offset;
        return;
    }

    PacketInfo info = packet_info(packet, relative_offset);
    const uint32_t next_packet_index = packet_index + skip_count + 1;

    // the frame's 15-bit header runs into the next packet: its size from the
    // two together
    if (info.current_frame_size == 0) {
        const uint8_t *next = next_packet(data, next_packet_index, current_input_packet_count);
        if (!next) {
            swap_input_buffer(data);
            return;
        }
        std::memcpy(input.data(), packet + BYTES_PER_PACKET_HEADER, BYTES_PER_PACKET_DATA);
        std::memcpy(input.data() + BYTES_PER_PACKET_DATA, next + BYTES_PER_PACKET_HEADER, BYTES_PER_PACKET_DATA);
        BitStream combined(input.data(), (BITS_PER_PACKET - BITS_PER_PACKET_HEADER) * 2);
        combined.set_offset(relative_offset - BITS_PER_PACKET_HEADER);
        const uint64_t frame_size = combined.peek(BITS_PER_FRAME_HEADER);
        if (frame_size == MAX_FRAME_LENGTH) {
            data.error_status = 4;
            owner->errors++;
            return;
        }
        info.current_frame_size = uint32_t(frame_size);
    }

    BitStream stream(current_input_buffer, (packet_index + 1) * BITS_PER_PACKET);
    stream.set_offset(data.input_buffer_read_offset);
    const uint32_t bits_to_copy = uint32_t(std::min<size_t>(stream.bits_remaining(), info.current_frame_size));
    if (bits_to_copy == 0) {
        swap_input_buffer(data);
        return;
    }
    // the packet's last frame, split: the rest is in the next packet
    if (info.last_frame_in_packet() && stream.bits_remaining() < info.current_frame_size) {
        const uint8_t *next = next_packet(data, next_packet_index, current_input_packet_count);
        if (!next) {
            data.error_status = 4;
            owner->errors++;
            return;
        }
        std::memcpy(input.data() + BYTES_PER_PACKET_DATA, next + BYTES_PER_PACKET_HEADER, BYTES_PER_PACKET_DATA);
    }
    std::memcpy(input.data(), packet + BYTES_PER_PACKET_HEADER, BYTES_PER_PACKET_DATA);

    // the frame's bits, after a byte FFmpeg's xmaframes decoder reads their
    // alignment from (PreparePacket)
    std::array<uint8_t, 1 + 4096 + 8> xma_frame{};
    stream = BitStream(input.data(), (BITS_PER_PACKET - BITS_PER_PACKET_HEADER) * 2);
    stream.set_offset(relative_offset - BITS_PER_PACKET_HEADER);
    const uint32_t frame_size = info.current_frame_size;
    const uint32_t copied = uint32_t(std::min<size_t>(frame_size, stream.bits_remaining()));
    const uint32_t padding_start = uint8_t(stream.copy(xma_frame.data() + 1, copied));
    const uint32_t size = 1 + (padding_start + frame_size) / 8 + ((padding_start + frame_size) % 8 ? 1 : 0);
    const uint32_t padding_end = size * 8 - (8 + padding_start + frame_size);
    xma_frame[0] = uint8_t(((padding_start & 7) << 5) | ((padding_end & 7) << 2));

    raw_frame.fill(0);
    const uint32_t channels = data.is_stereo ? 2 : 1;
    if (!owner->decoder->decode(id, xma_frame.data(), size, SAMPLE_RATES[std::min(uint32_t(data.sample_rate), 3u)],
                                channels, raw_frame.data())) {
        owner->frames_failed++;
    } else {
        owner->frames++;
        // the loudest sample (16-bit big-endian)
        uint32_t peak = 0;
        for (uint32_t i = 0; i < BYTES_PER_FRAME_CHANNEL * channels; i += 2) {
            const int32_t sample = int16_t(uint16_t(raw_frame[i] << 8 | raw_frame[i + 1]));
            peak = std::max(peak, uint32_t(sample < 0 ? -sample : sample));
        }
        if (peak) {
            owner->frames_with_sound++;
            for (uint32_t seen = owner->peak.load(); peak > seen && !owner->peak.compare_exchange_weak(seen, peak);) {
            }
        }
        current_frame_remaining_subframes = uint8_t(4 << data.is_stereo);
        // the loop's end frame: subframes up to loop_subframe_end only
        loop_frame_output_limit = is_loop_end_frame ? uint8_t((data.loop_subframe_end + 1) << data.is_stereo) : 0;
        // the loop's start frame: loop_subframe_skip subframes skipped
        if (loop_start_skip_pending) {
            const uint8_t skip = uint8_t(data.loop_subframe_skip << data.is_stereo);
            if (skip < current_frame_remaining_subframes) current_frame_remaining_subframes -= skip;
            loop_start_skip_pending = false;
        }
    }

    // where the walk goes next: the packet's next frame
    if (!info.last_frame_in_packet()) {
        const uint32_t next_frame_offset = (data.input_buffer_read_offset + bits_to_copy) % BITS_PER_PACKET;
        data.input_buffer_read_offset = packet_index * BITS_PER_PACKET + next_frame_offset;
        return;
    }
    // or the first frame of the next packet of this stream, in this buffer or
    // the next
    uint32_t next_input_offset = next_packet_read_offset(data, next_packet_index, current_input_packet_count);
    if (next_packet_index >= current_input_packet_count || next_input_offset == BITS_PER_PACKET_HEADER)
        swap_input_buffer(data);
    if (next_input_offset == BITS_PER_PACKET_HEADER && data.any_input_buffer_valid()) {
        // the start of the next buffer: its first packet's first frame, or
        // the next packet if no frame begins in it
        next_input_offset = packet_frame_offset(physical(data.input_buffer_address(data.current_buffer)));
        if (next_input_offset > MAX_FRAME_SIZE_IN_BITS) {
            swap_input_buffer(data);
            return;
        }
    }
    data.input_buffer_read_offset = next_input_offset;
}

// at the loop's end, back to its start (loop_count 255: for ever)
void Xma::Context::update_loop_status(XmaContextData &data) {
    if (data.loop_count == 0) return;
    const uint32_t loop_start = std::max(BITS_PER_PACKET_HEADER, uint32_t(data.loop_start));
    const uint32_t loop_end = std::max(BITS_PER_PACKET_HEADER, uint32_t(data.loop_end));
    if (data.input_buffer_read_offset != loop_end) return;
    data.input_buffer_read_offset = loop_start;
    loop_start_skip_pending = true;
    if (data.loop_count != 255) data.loop_count--;
    owner->loops++;
}

// the frames that begin in the packet, and which of them starts at
// frame_offset (its size 0 when its header runs into the next packet)
PacketInfo Xma::Context::packet_info(const uint8_t *packet, uint32_t frame_offset) {
    PacketInfo info;
    const uint32_t first_frame_offset = packet_frame_offset(packet);
    BitStream stream(packet, BITS_PER_PACKET);
    stream.set_offset(first_frame_offset);
    // the tail of a split frame
    if (frame_offset < first_frame_offset) {
        info.current_frame = 0;
        info.current_frame_size = first_frame_offset - frame_offset;
    }
    for (;;) {
        if (stream.bits_remaining() < BITS_PER_FRAME_HEADER) {
            // a frame whose header runs into the next packet counts, its size
            // unknown here
            if (stream.bits_remaining() > 0) {
                if (stream.offset() == frame_offset) {
                    info.current_frame = info.frame_count;
                    info.current_frame_size = 0;
                }
                info.frame_count++;
            }
            break;
        }
        const uint64_t frame_size = stream.peek(BITS_PER_FRAME_HEADER);
        if (frame_size == 0 || frame_size == MAX_FRAME_LENGTH) break;
        if (stream.offset() == frame_offset) {
            info.current_frame = info.frame_count;
            info.current_frame_size = uint32_t(frame_size);
        }
        info.frame_count++;
        if (frame_size > stream.bits_remaining()) break; // the last, split
        stream.advance(frame_size - 1);
        // the frame's last bit: whether another follows
        if (stream.read(1) == 0) break;
    }
    if (packet_is_xma2(packet)) {
        const uint8_t xma2_frame_count = packet_frame_count(packet);
        if (xma2_frame_count > info.frame_count) {
            // a header split across the packets: the count the packet gives
            if (info.current_frame_size == 0) info.current_frame = info.frame_count;
            info.frame_count = xma2_frame_count;
        }
    }
    return info;
}

// a packet by its index from the current buffer's start, in it or the next
PacketHandle Xma::Context::packet_handle(const XmaContextData &data, uint32_t buffer, uint32_t packet, uint32_t count) {
    if (packet >= count) {
        buffer ^= 1;
        packet -= count;
    }
    if (!data.input_buffer_valid(buffer) || !data.input_buffer_address(buffer) ||
        packet >= data.input_buffer_packet_count(buffer))
        return {};
    return {buffer, packet, true};
}

const uint8_t *Xma::Context::next_packet(const XmaContextData &data, uint32_t next_packet_index, uint32_t count) {
    PacketHandle h = packet_handle(data, data.current_buffer, next_packet_index, count);
    if (!h.valid) return nullptr;
    return physical(data.input_buffer_address(h.buffer)) + h.packet * BYTES_PER_PACKET;
}

// the first packet from next_packet_index on in which a frame begins: that
// frame's offset in the buffer, or the header's size if none
uint32_t Xma::Context::next_packet_read_offset(const uint8_t *buffer, uint32_t next_packet_index, uint32_t count) {
    while (next_packet_index < count) {
        const uint8_t *next = buffer + next_packet_index * BYTES_PER_PACKET;
        const uint32_t offset = packet_frame_offset(next);
        if (offset <= MAX_FRAME_SIZE_IN_BITS) return next_packet_index * BITS_PER_PACKET + offset;
        // all of it a frame's continuation: this packet's own skip
        const uint8_t next_skip = packet_skip_count(next);
        if (next_skip == 0xFF) break;
        next_packet_index += next_skip + 1;
    }
    return BITS_PER_PACKET_HEADER;
}

uint32_t Xma::Context::next_packet_read_offset(const XmaContextData &data, uint32_t next_packet_index, uint32_t count) {
    PacketHandle h = packet_handle(data, data.current_buffer, next_packet_index, count);
    if (!h.valid) return BITS_PER_PACKET_HEADER;
    return next_packet_read_offset(physical(data.input_buffer_address(h.buffer)), h.packet,
                                   data.input_buffer_packet_count(h.buffer));
}

// StoreContextMerged: the context read again and only the decoder's fields
// written, so what the game changed meanwhile stays
void Xma::Context::store_merged(const XmaContextData &data, const XmaContextData &initial, uint8_t *context) {
    XmaContextData fresh(context);
    fresh.loop_count = data.loop_count;
    fresh.output_buffer_write_offset = data.output_buffer_write_offset;
    // a valid flag cleared only if the decoder consumed that buffer
    if (initial.input_buffer_0_valid && !data.input_buffer_0_valid) fresh.input_buffer_0_valid = 0;
    if (initial.input_buffer_1_valid && !data.input_buffer_1_valid) fresh.input_buffer_1_valid = 0;
    if (initial.output_buffer_valid && !data.output_buffer_valid) fresh.output_buffer_valid = 0;
    fresh.input_buffer_read_offset = data.input_buffer_read_offset;
    fresh.error_status = data.error_status;
    fresh.current_buffer = data.current_buffer;
    fresh.output_buffer_read_offset = data.output_buffer_read_offset;
    fresh.store(context);
}

// ---- the recording (xenoforge/xma_recording.h) ----

namespace {

// whether a host range is committed and readable (a buffer address the game
// left in a context may point at pages that are not)
bool readable(const uint8_t *p, size_t size) {
#ifdef _WIN32
    for (const uint8_t *end = p + size; p < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        p = static_cast<const uint8_t *>(info.BaseAddress) + info.RegionSize;
    }
#else
    (void)p, (void)size;
#endif
    return true;
}

} // namespace

struct Xma::Recorder {
    std::mutex mutex;
    xma_recording::Writer writer;

    // a buffer at a physical address, recorded if it is all there
    uint64_t buffer(Kernel &k, uint32_t physical, uint32_t size) {
        if (!physical || !size || (physical & 0x1FFFFFFF) + uint64_t(size) > 0x20000000) return 0;
        const uint8_t *p = k.memory().translate_physical(physical);
        if (!readable(p, size)) return 0;
        return writer.buffer(p, size);
    }
    // the context's data and its buffers as the run starts (its lock held)
    void before(Kernel &k, Context &x, xma_recording::Run &run) {
        const uint8_t *context = k.host(x.guest);
        std::memcpy(run.before, context, 64);
        XmaContextData data(context);
        std::lock_guard<std::mutex> l(mutex);
        run.context = x.id;
        for (uint32_t b = 0; b < 2; b++)
            run.input[b] =
                buffer(k, data.input_buffer_address(b), data.input_buffer_packet_count(b) * BYTES_PER_PACKET);
        run.output_before = buffer(k, data.output_buffer_ptr, data.output_buffer_block_count * OUTPUT_BYTES_PER_BLOCK);
    }
    // and as it ends: the run written
    void after(Kernel &k, Context &x, xma_recording::Run &run) {
        const uint8_t *context = k.host(x.guest);
        std::memcpy(run.after, context, 64);
        XmaContextData data(context);
        std::lock_guard<std::mutex> l(mutex);
        run.output_after = buffer(k, data.output_buffer_ptr, data.output_buffer_block_count * OUTPUT_BYTES_PER_BLOCK);
        writer.run(run);
    }
    void event(uint8_t type, uint32_t id) {
        std::lock_guard<std::mutex> l(mutex);
        writer.event(type, id);
    }
};

bool Xma::record_to(const std::string &path) {
    auto r = std::make_unique<Recorder>();
    if (!r->writer.open(path)) {
        std::fprintf(stderr, "xma: cannot write the recording %s\n", path.c_str());
        return false;
    }
    std::fprintf(stderr, "xma: recording the contexts' runs to %s\n", path.c_str());
    recorder_ = std::move(r);
    return true;
}

// ---- the decoder (XmaDecoder) ----

Xma::Xma() = default;
Xma::~Xma() = default;

void Xma::setup(Kernel &kernel) {
    k = &kernel;
    // XF_XMA_SILENT=1: every frame silence, to measure what decoding costs
    const char *silent = std::getenv("XF_XMA_SILENT");
    if (!decoder) decoder = silent && *silent == '1' ? audio::make_silent_decoder() : audio::make_xma_decoder();
    if (const char *path = std::getenv("XF_XMA_RECORD"); path && *path) record_to(path);
    // the contexts in physical memory, uncached, the array's physical address
    // in a register (the console's kernel allocates them so)
    context_array = kernel.memory().system_alloc(CONTEXT_SIZE * CONTEXT_COUNT, 256, true);
    if (!context_array) {
        std::fprintf(stderr, "xma: no memory for the contexts\n");
        return;
    }
    registers_[CONTEXT_ARRAY_ADDRESS] = kernel.memory().physical_address(context_array);
    registers_[NEXT_CONTEXT_INDEX] = 1;
    contexts_ = std::make_unique<Context[]>(CONTEXT_COUNT);
    for (uint32_t i = 0; i < CONTEXT_COUNT; i++) {
        contexts_[i].owner = this;
        contexts_[i].id = i;
        contexts_[i].guest = context_array + i * CONTEXT_SIZE;
    }
    logged_.assign(REGISTERS, false);
    Kernel::Mmio range;
    range.base = XMA_REGISTERS, range.mask = XMA_REGISTERS_MASK, range.device = this;
    range.read = [](void *x, uint32_t address) { return static_cast<Xma *>(x)->read_register(address); };
    range.write = [](void *x, uint32_t address, uint32_t value) {
        static_cast<Xma *>(x)->write_register(address, value);
    };
    kernel.add_mmio(range);
}

void Xma::start() {
    std::call_once(started_, [this] {
        if (!contexts_) return;
        Objects &o = objects(*k);
        work_ = o.create<Event>(GuestType::EventSynchronization, header::SIZE);
        uint32_t status = X_STATUS_NO_MEMORY;
        if (work_) {
            // a kernel thread in the idle process (it never calls the
            // guest), as Xenia's "XMA Decoder"
            Thread::Params p;
            p.stack_size = STACK;
            p.process = title_process(*k) - kprocess::SIZE;
            p.host = [](Thread &self, void *x) { static_cast<Xma *>(x)->worker(self); };
            p.host_arg = this;
            p.name = "XMA Decoder";
            thread_ = o.create_thread(p, status);
        }
        if (!thread_) {
            std::fprintf(stderr, "xma: the decoder's thread was not made (status 0x%08X)\n", status);
            return;
        }
        running_ = true;
        started_thread_ = o.scheduler->start(*thread_);
        if (!started_thread_) {
            running_ = false;
            std::fprintf(stderr, "xma: no host thread for the decoder\n");
        }
    });
}

void Xma::stop() {
    if (!k || !contexts_) return;
    Objects &o = objects(*k);
    if (running_.exchange(false)) {
        set_event(o, *work_);
        if (started_thread_) o.scheduler->join(*thread_);
    }
    // the references are let go after the lock (the last one deletes), and
    // before the objects they belong to go
    std::vector<Ref<Event>> events;
    {
        std::lock_guard<std::mutex> l(o.lock);
        for (uint32_t i = 0; i < CONTEXT_COUNT; i++) {
            Context &x = contexts_[i];
            // a kick still waiting is let go
            complete(x, x.generation);
            if (x.done && x.done->handle()) o.release_handle(x.done->handle());
            if (x.done) events.push_back(std::move(x.done));
        }
        for (Object *object : {static_cast<Object *>(thread_.get()), static_cast<Object *>(work_.get())})
            if (object && object->handle()) o.release_handle(object->handle());
    }
    events.clear();
    thread_ = {}, work_ = {};
    if (recorder_) {
        std::lock_guard<std::mutex> l(recorder_->mutex);
        recorder_->writer.flush();
    }
}

// a context's kicks complete up to generation (dispatcher lock held)
void Xma::complete(Context &x, uint64_t generation) {
    x.completed = std::max(x.completed, generation);
    if (x.done && x.completed >= x.generation) {
        x.done->set_signal_state(1);
        objects(*k).signaled(*x.done);
    }
}

int32_t Xma::context_id(uint32_t guest) const {
    if (!contexts_ || guest < context_array || guest >= context_array + CONTEXT_SIZE * CONTEXT_COUNT) return -1;
    return int32_t((guest - context_array) / CONTEXT_SIZE);
}

uint32_t Xma::allocate_context() {
    std::lock_guard<std::mutex> l(allocation_mutex_);
    if (!contexts_) return 0;
    for (uint32_t i = 0; i < CONTEXT_COUNT; i++) {
        bool expected = false;
        if (contexts_[i].allocated.compare_exchange_strong(expected, true)) {
            created++;
            return contexts_[i].guest;
        }
    }
    return 0;
}

void Xma::release_context(uint32_t guest) {
    int32_t id = context_id(guest);
    if (id < 0) {
        std::fprintf(stderr, "xma: XMAReleaseContext of 0x%08X, not a context\n", guest);
        return;
    }
    Context &x = contexts_[id];
    // the decoder may be walking it
    std::lock_guard<std::mutex> l(x.lock);
    if (recorder_) recorder_->event(xma_recording::RELEASE, x.id);
    std::memset(k->host(x.guest), 0, CONTEXT_SIZE);
    x.forget();
    x.allocated = false;
    released++;
}

bool Xma::block_on_context(uint32_t guest, bool poll) {
    int32_t id = context_id(guest);
    if (id < 0) return true;
    std::unique_lock<std::mutex> l(contexts_[id].lock, std::try_to_lock);
    if (!l.owns_lock()) {
        if (poll) return false;
        l.lock();
    }
    return true;
}

void Xma::store_indexed_register(uint32_t base_address, uint32_t guest) {
    uint32_t physical = k->memory().physical_address(guest);
    uint32_t hw_index = (physical - registers_[CONTEXT_ARRAY_ADDRESS]) / CONTEXT_SIZE;
    write_register(base_address + (hw_index >> 5) * 4, __builtin_bswap32(1u << (hw_index & 0x1F)));
}

// ---- the registers ----

uint32_t Xma::read_register(uint32_t address) {
    uint32_t r = (address & 0xFFFF) / 4;
    std::lock_guard<std::mutex> l(registers_mutex_);
    switch (r) {
    case CONTEXT_ARRAY_ADDRESS: break;
    case CURRENT_CONTEXT_INDEX:
        // the context the hardware works on: a rotating number, so the games
        // never see one stuck
        registers_[CURRENT_CONTEXT_INDEX] = registers_[NEXT_CONTEXT_INDEX];
        registers_[NEXT_CONTEXT_INDEX] = (registers_[NEXT_CONTEXT_INDEX] + 1) % CONTEXT_COUNT;
        break;
    default:
        if (!logged_.empty() && !logged_[r]) {
            logged_[r] = true;
            std::fprintf(stderr, "xma: a read of register 0x%04X (%s)\n", r, register_name(r));
        }
    }
    return __builtin_bswap32(registers_[r]);
}

void Xma::write_register(uint32_t address, uint32_t value) {
    uint32_t r = (address & 0xFFFF) / 4;
    value = __builtin_bswap32(value);
    {
        std::lock_guard<std::mutex> l(registers_mutex_);
        registers_[r] = value;
    }
    if (!contexts_) return;
    Objects &o = objects(*k);
    if (r >= KICK && r < KICK + 10) {
        // the hardware decodes these contexts (XMAEnableContext)
        kick((r - KICK) * 32, value);
    } else if (r >= LOCK && r < LOCK + 10) {
        // these stop, once the decoder is out of them (XMADisableContext)
        for (uint32_t v = value; v; v &= v - 1) {
            Context &x = contexts_[(r - LOCK) * 32 + std::countr_zero(v)];
            {
                std::lock_guard<std::mutex> l(o.lock);
                if (x.enabled) {
                    // a kick the decoder has not taken yet is complete, unwalked
                    set_enabled(x, false);
                    if (x.walking)
                        x.cancelled = x.generation;
                    else
                        complete(x, x.generation);
                }
            }
            std::lock_guard<std::mutex> l(x.lock);
        }
    } else if (r >= CLEAR && r < CLEAR + 10) {
        // these start over (XMAInitializeContext)
        for (uint32_t v = value; v; v &= v - 1) {
            Context &x = contexts_[(r - CLEAR) * 32 + std::countr_zero(v)];
            std::lock_guard<std::mutex> l(x.lock);
            if (recorder_) recorder_->event(xma_recording::CLEAR, x.id);
            XmaContextData data(k->host(x.guest));
            x.clear_locked(data);
            data.store(k->host(x.guest));
        }
    } else if (r != 0x601) {
        // 0x601 is written around a lock (0x02000000, 0x03000000)
        std::lock_guard<std::mutex> l(registers_mutex_);
        if (!logged_[r]) {
            logged_[r] = true;
            std::fprintf(stderr, "xma: a write of 0x%08X to register 0x%04X (%s)\n", value, r, register_name(r));
        }
    }
}

// The kicked contexts are the decoder's; the kicking thread waits until it
// has walked them, so the game sees their data moved on.
void Xma::kick(uint32_t first, uint32_t mask) {
    if (!mask) return;
    start();
    Objects &o = objects(*k);
    // each context's done event, made on its first kick (x.done is the
    // dispatcher lock's)
    for (uint32_t v = mask; v; v &= v - 1) {
        Context &x = contexts_[first + std::countr_zero(v)];
        {
            std::lock_guard<std::mutex> l(o.lock);
            if (x.done) continue;
        }
        Ref<Event> made = o.create<Event>(GuestType::EventNotification, header::SIZE), extra;
        std::lock_guard<std::mutex> l(o.lock);
        if (!x.done) x.done = std::move(made);
        else if (made) extra = std::move(made), o.release_handle(extra->handle()); // another kick made it first
    }
    std::array<uint64_t, 32> targets{};
    {
        std::lock_guard<std::mutex> l(o.lock);
        for (uint32_t v = mask; v; v &= v - 1) {
            uint32_t bit = std::countr_zero(v);
            Context &x = contexts_[first + bit];
            set_enabled(x, true);
            targets[bit] = ++x.generation;
            if (!x.kicked.exchange(true)) contexts_kicked++;
            if (x.done) x.done->set_signal_state(0);
        }
    }
    kicks += std::popcount(mask);
    Thread *self = current_thread();
    bool on_guest = self && running_.load();
    if (on_guest) set_event(o, *work_);
    for (uint32_t v = mask; v; v &= v - 1) {
        uint32_t bit = std::countr_zero(v);
        Context &x = contexts_[first + bit];
        if (!on_guest) work(x);
        for (;;) {
            Dispatcher *done = nullptr;
            {
                std::lock_guard<std::mutex> l(o.lock);
                if (x.completed >= targets[bit] || !x.done) break;
                done = x.done.get();
            }
            if (on_guest) {
                Dispatcher *const wait_on[] = {done};
                o.wait(*self, wait_on, false, false, Scheduler::NEVER);
            } else {
                std::this_thread::yield(); // the decoder's thread has it
            }
        }
    }
}

// Work: one run of a context the game kicked
bool Xma::work(Context &x) {
    Objects &o = objects(*k);
    uint64_t generation;
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (!x.enabled) return false;
        set_enabled(x, false);
        x.walking = generation = x.generation;
    }
    if (x.allocated.load()) {
        std::lock_guard<std::mutex> l(x.lock);
        xma_recording::Run run;
        if (recorder_) recorder_->before(*k, x, run);
        x.walk();
        if (recorder_) recorder_->after(*k, x, run);
    }
    works++;
    std::lock_guard<std::mutex> l(o.lock);
    x.walking = 0;
    complete(x, std::max(generation, x.cancelled));
    return true;
}

void Xma::set_enabled(Context &x, bool enabled) {
    x.enabled = enabled;
    const uint32_t bit = 1u << (x.id % 32);
    if (enabled)
        pending_[x.id / 32].fetch_or(bit, std::memory_order_release);
    else
        pending_[x.id / 32].fetch_and(~bit, std::memory_order_relaxed);
}

void Xma::worker(Thread &self) {
    Objects &o = objects(*k);
    Dispatcher *const work_event[] = {work_.get()};
    while (running_.load()) {
        // the contexts in order, as a pass over all 320 took them, each
        // word read again after a run so a kick behind the pass is seen in
        // it; the dispatcher lock (the kernel's, every guest wait's) only
        // for a context with a kick: taking it 320 times a pass made the
        // decoder's thread half lock traffic, and the guest's threads wait
        // behind it
        bool did_work = false;
        for (uint32_t w = 0; w < pending_.size(); w++) {
            uint32_t past = 0; // the bits at or below the last context run
            for (uint32_t v; (v = pending_[w].load(std::memory_order_acquire) & ~past) != 0;) {
                const uint32_t bit = uint32_t(std::countr_zero(v));
                past = bit == 31 ? ~0u : (2u << bit) - 1;
                did_work = work(contexts_[w * 32 + bit]) || did_work;
            }
        }
        if (did_work) continue;
        o.wait(self, work_event, false, false, Scheduler::NEVER);
    }
}

Xma &xma(Kernel &k) {
    Xma &x = k.state<Xma>();
    x.start();
    return x;
}

// ---- the exports (xboxkrnl_audio_xma.cc) ----

namespace {

// the physical address behind a guest pointer, UINT32_MAX if none
uint32_t physical_of(Kernel &k, uint32_t address) { return k.memory().physical_address(address); }

// Changes a context's data in guest memory.
template <typename F> void change(Kernel &k, uint32_t context, F f) {
    XmaContextData data(k.host(context));
    f(data);
    data.store(k.host(context));
}

XmaContextData read(Kernel &k, uint32_t context) { return XmaContextData(k.host(context)); }

} // namespace

XF_EXPORT(xboxkrnl, XMACreateContext) {
    uint32_t out_ptr = arg(k, c, 0);
    uint32_t context = xma(k).allocate_context();
    k.store32(out_ptr, context);
    result(c, context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

XF_EXPORT(xboxkrnl, XMAReleaseContext) {
    xma(k).release_context(arg(k, c, 0));
    result(c, 0);
}

// XMA_CONTEXT_INIT (56 bytes): the input buffers and their packet counts,
// the read offset, the output buffer and its blocks, the work buffer, the
// subframes to decode, the channels, the sample rate, the loop
XF_EXPORT(xboxkrnl, XMAInitializeContext) {
    uint32_t context = arg(k, c, 0), init = arg(k, c, 1);
    Xma &x = xma(k);
    uint32_t input_0 = k.load32(init + 0), input_1 = k.load32(init + 8), output = k.load32(init + 20);
    // the input buffers may be absent (buffer 1 in 415607D4)
    uint32_t input_0_physical = input_0 ? physical_of(k, input_0) : 0;
    uint32_t input_1_physical = input_1 ? physical_of(k, input_1) : 0;
    uint32_t output_physical = physical_of(k, output);
    if (input_0_physical == UINT32_MAX || input_1_physical == UINT32_MAX || output_physical == UINT32_MAX) {
        std::fprintf(stderr, "XMAInitializeContext: a buffer at no physical address (0x%08X, 0x%08X, 0x%08X)\n",
                     input_0, input_1, output);
        return result(c, X_E_FALSE);
    }
    XmaContextData data;
    data.input_buffer_0_ptr = input_0_physical;
    data.input_buffer_0_packet_count = k.load32(init + 4);
    data.input_buffer_1_ptr = input_1_physical;
    data.input_buffer_1_packet_count = k.load32(init + 12);
    data.input_buffer_read_offset = k.load32(init + 16);
    data.output_buffer_ptr = output_physical;
    data.output_buffer_block_count = k.load32(init + 24);
    data.subframe_decode_count = k.load32(init + 32);
    data.is_stereo = k.load32(init + 36) >= 1;
    data.sample_rate = k.load32(init + 40);
    data.loop_start = k.load32(init + 44);
    data.loop_end = k.load32(init + 48);
    data.loop_count = k.host(init)[52];
    data.loop_subframe_end = k.host(init)[53];
    data.loop_subframe_skip = k.host(init)[54];
    data.store(k.host(context));
    x.store_indexed_register(0x1A80, context); // a clear
    result(c, 0);
}

// XMA_LOOP_DATA (12 bytes): start and end offsets in bits, count, the end's
// subframe, the subframes to skip at the start
XF_EXPORT(xboxkrnl, XMASetLoopData) {
    uint32_t loop = arg(k, c, 1);
    change(k, arg(k, c, 0), [&](XmaContextData &d) {
        d.loop_start = k.load32(loop + 0);
        d.loop_end = k.load32(loop + 4);
        d.loop_count = k.host(loop)[8];
        d.loop_subframe_end = k.host(loop)[9];
        d.loop_subframe_skip = k.host(loop)[10];
    });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAGetInputBufferReadOffset) { result(c, read(k, arg(k, c, 0)).input_buffer_read_offset); }

XF_EXPORT(xboxkrnl, XMASetInputBufferReadOffset) {
    uint32_t value = arg(k, c, 1);
    change(k, arg(k, c, 0), [&](XmaContextData &d) { d.input_buffer_read_offset = value; });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMASetInputBuffer0) {
    uint32_t buffer = arg(k, c, 1), packets = arg(k, c, 2), physical = physical_of(k, buffer);
    if (physical == UINT32_MAX) return result(c, X_E_FALSE);
    change(k, arg(k, c, 0), [&](XmaContextData &d) {
        d.input_buffer_0_ptr = physical;
        d.input_buffer_0_packet_count = packets;
    });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAIsInputBuffer0Valid) { result(c, read(k, arg(k, c, 0)).input_buffer_0_valid); }

XF_EXPORT(xboxkrnl, XMASetInputBuffer0Valid) {
    change(k, arg(k, c, 0), [](XmaContextData &d) { d.input_buffer_0_valid = 1; });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMASetInputBuffer1) {
    uint32_t buffer = arg(k, c, 1), packets = arg(k, c, 2), physical = physical_of(k, buffer);
    if (physical == UINT32_MAX) return result(c, X_E_FALSE);
    change(k, arg(k, c, 0), [&](XmaContextData &d) {
        d.input_buffer_1_ptr = physical;
        d.input_buffer_1_packet_count = packets;
    });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAIsInputBuffer1Valid) { result(c, read(k, arg(k, c, 0)).input_buffer_1_valid); }

XF_EXPORT(xboxkrnl, XMASetInputBuffer1Valid) {
    change(k, arg(k, c, 0), [](XmaContextData &d) { d.input_buffer_1_valid = 1; });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAIsOutputBufferValid) { result(c, read(k, arg(k, c, 0)).output_buffer_valid); }

XF_EXPORT(xboxkrnl, XMASetOutputBufferValid) {
    change(k, arg(k, c, 0), [](XmaContextData &d) { d.output_buffer_valid = 1; });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAGetOutputBufferReadOffset) { result(c, read(k, arg(k, c, 0)).output_buffer_read_offset); }

XF_EXPORT(xboxkrnl, XMASetOutputBufferReadOffset) {
    uint32_t value = arg(k, c, 1);
    change(k, arg(k, c, 0), [&](XmaContextData &d) { d.output_buffer_read_offset = value; });
    result(c, 0);
}

XF_EXPORT(xboxkrnl, XMAGetOutputBufferWriteOffset) { result(c, read(k, arg(k, c, 0)).output_buffer_write_offset); }

XF_EXPORT(xboxkrnl, XMAGetPacketMetadata) { result(c, read(k, arg(k, c, 0)).packet_metadata); }

// a kick
XF_EXPORT(xboxkrnl, XMAEnableContext) {
    xma(k).store_indexed_register(0x1940, arg(k, c, 0));
    result(c, 0);
}

// (context, wait): a lock; without wait, S_FALSE-like X_E_FALSE if the
// decoder is in it now
XF_EXPORT(xboxkrnl, XMADisableContext) {
    uint32_t context = arg(k, c, 0), wait = arg(k, c, 1);
    Xma &x = xma(k);
    x.store_indexed_register(0x1A40, context);
    result(c, x.block_on_context(context, !wait) ? 0 : X_E_FALSE);
}

// until neither input buffer is valid, or there is no work buffer (none
// here: XMAInitializeContext leaves it out, as Xenia's)
XF_EXPORT(xboxkrnl, XMABlockWhileInUse) {
    uint32_t context = arg(k, c, 0);
    Objects &o = objects(k);
    for (;;) {
        XmaContextData d = read(k, context);
        if (!d.any_input_buffer_valid() || !d.work_buffer_ptr) break;
        if (Thread *self = current_thread())
            o.wait(*self, {}, false, false, o.now() + 10'000);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result(c, 0);
}

} // namespace xf::kernel
