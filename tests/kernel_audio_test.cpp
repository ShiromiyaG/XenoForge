// The sound family (src/kernel/audio.h) live, on real guest threads. The
// render driver: clients registered with guest callbacks, which the "Audio
// Worker" calls once a frame period while the client has a free slot of the
// 64 it may have in flight (one that never submits stops at 64, and gets one
// more callback for each frame it then submits, as the pacing output plays
// it); a client that submits from its callback runs at the playback rate,
// its frames reaching the output as host floats; unregistered, even while
// its callback runs, it gets no more. The XMA decoder: contexts created,
// their registers through MMIO, synthetic XMA2 input (hand-built packets
// and frame headers, no game data) kicked, the frames each walk hands the
// decoder, the offsets, buffer flags and output blocks it leaves checked
// against Xenia's walk (traced by hand in the comments), loops, split
// frames, the two input buffers, a stereo context, lock, clear, release.
// The guest code is host functions in the generated code's table. Every
// check waits for a state or counts on the scheduler's clock, never on a
// host time, so the run is the same each time: kernel_audio_test [repeats].

#include "check.h"

#include "audio.h"
#include "objects.h"
#include "xenoforge/audio.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/ppc_ops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
Memory *M = nullptr;
uint32_t scratch = 0; // guest memory for the main thread
bool verbose = true;  // the first run prints the rate it measured

uint32_t ex(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn("xboxkrnl", name);
    if (!fn) {
        std::printf("no export %s\n", name);
        failures++;
        return 0xFFFFFFFF;
    }
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    fn(*K, c);
    return uint32_t(c.r[3]);
}

void sleep_ms(ppc::Context &c, int64_t ms) {
    K->store64(scratch + 0xF00, uint64_t(-ms * 10'000));
    ex(c, "KeDelayExecutionThread", {1, 0, scratch + 0xF00});
}

// waits until done() (at most 20 s, so a broken run ends)
template <typename F> bool wait_until(ppc::Context &c, F done) {
    for (int i = 0; i < 20'000 && !done(); i++) sleep_ms(c, 1);
    return done();
}

uint64_t now() { return objects(*K).now(); }

// the XMA registers as the games reach them: lwbrx and stwbrx (base r3)
uint32_t xma_read(ppc::Context &c, uint32_t index) {
    return __builtin_bswap32(ppc::ops::load32_io(c, 3, 0x7FEA0000 + index * 4));
}
void xma_write(ppc::Context &c, uint32_t index, uint32_t value) {
    ppc::ops::store32_io(c, 3, 0x7FEA0000 + index * 4, __builtin_bswap32(value));
}

// ---- the render driver's clients ----

enum : uint32_t { F_MAIN = 0x82000000, F_CALLBACK_A = 0x82000010, F_CALLBACK_B = 0x82000020 };
constexpr uint32_t ARG_A = 0xA0A0A0A0, ARG_B = 0xB0B0B0B0;
constexpr uint64_t FRAME_100NS = 10'000'000ull * 256 / 48000; // 53333, a frame's length

std::atomic<uint32_t> callbacks_a{0}, callbacks_b{0}, bad_callbacks{0};
std::atomic<bool> in_callback_b{false};
uint32_t driver_b = 0, frame_b = 0;   // client B's handle and the frame it submits
std::array<uint64_t, 4096> times_b{}; // the clock at each of B's callbacks

// what the outputs saw: frames, and the first samples of the last
struct Seen {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> bad_frames{0};
};
Seen seen;

// a pacing output that also looks at each frame: B's are 0.5 * i + channel
class LookingOutput final : public audio::Output {
public:
    void submit(const float *frame, uint64_t at) override {
        seen.frames++;
        if (frame[0] == 0.25f) { // B's
            for (uint32_t ch = 0; ch < CHANNELS; ch++)
                for (uint32_t i = 0; i < CHANNEL_SAMPLES; i += 51)
                    if (frame[ch * CHANNEL_SAMPLES + i] != 0.25f + float(ch) + 0.5f * float(i)) seen.bad_frames++;
        }
        pacing_->submit(frame, at);
    }
    uint32_t played(uint64_t at) override { return pacing_->played(at); }

private:
    std::unique_ptr<audio::Output> pacing_ = audio::make_pacing_output();
};

// the callbacks: on the audio worker (a kernel thread in the system
// process), r3 the guest word holding the argument given at registration
bool on_worker(ppc::Context &c, uint32_t arg) {
    Thread *self = current_thread();
    bool worker = self && self->params.host && std::string(self->params.name) == "Audio Worker" &&
                  K->load32(self->kthread() + kthread::PROCESS) == system_process(*K) &&
                  Kernel::current_context() == &c;
    return worker && K->load32(uint32_t(c.r[3])) == arg;
}

// A never submits a frame
void callback_a(ppc::Context &c) {
    if (!on_worker(c, ARG_A)) bad_callbacks++;
    callbacks_a++;
}

// B submits one each time, and takes a while (an unregistration may come
// meanwhile)
void callback_b(ppc::Context &c) {
    in_callback_b = true;
    if (!on_worker(c, ARG_B)) bad_callbacks++;
    if (callbacks_b < times_b.size()) times_b[callbacks_b] = now();
    ex(c, "XAudioSubmitRenderDriverFrame", {driver_b, frame_b});
    if (callbacks_b % 8 == 0) {
        // a millisecond, on the worker's own stack
        uint32_t timeout = uint32_t(c.r[1]) - 0x100;
        K->store64(timeout, uint64_t(-10'000));
        ex(c, "KeDelayExecutionThread", {0, 0, timeout});
    }
    callbacks_b++;
    in_callback_b = false;
}

void queries(ppc::Context &c) {
    uint32_t out = scratch + 0x100;
    K->store32(out, 0xCCCCCCCC);
    CHECK(ex(c, "XAudioGetSpeakerConfig", {out}) == 0);
    CHECK(K->load32(out) == 0x00010001); // Dolby Digital, Pro Logic: Xenia's default setting
    CHECK(ex(c, "XAudioGetVoiceCategoryVolume", {1, out}) == 0);
    CHECK(K->load32(out) == std::bit_cast<uint32_t>(1.0f));
    K->store32(out, 0xCCCCCCCC);
    CHECK(ex(c, "XAudioGetVoiceCategoryVolumeChangeMask", {0x41550000, out}) == 0);
    CHECK(K->load32(out) == 0);
    CHECK(ex(c, "XAudioEnableDucker", {1}) == 0);
    for (const char *name : {"XAudioGetDuckerLevel", "XAudioGetDuckerThreshold", "XAudioGetDuckerAttackTime",
                             "XAudioGetDuckerReleaseTime", "XAudioGetDuckerHoldTime"}) {
        c.f[1] = -7.0;
        ex(c, name, {});
        CHECK(c.f[1] == 1.0);
    }
}

void clients(ppc::Context &c) {
    Audio &a = K->state<Audio>();
    uint32_t reg = scratch + 0x200, driver = scratch + 0x208;
    // no callback: E_INVALIDARG
    CHECK(ex(c, "XAudioRegisterRenderDriverClient", {0, driver}) == 0x80070057);
    K->store32(reg, 0), K->store32(reg + 4, ARG_A);
    CHECK(ex(c, "XAudioRegisterRenderDriverClient", {reg, driver}) == 0x80070057);

    // A: 64 callbacks, then none while it submits nothing
    K->store32(reg, F_CALLBACK_A), K->store32(reg + 4, ARG_A);
    CHECK(ex(c, "XAudioRegisterRenderDriverClient", {reg, driver}) == 0);
    uint32_t driver_a = K->load32(driver);
    CHECK(driver_a == 0x41550000);
    CHECK(wait_until(c, [] { return callbacks_a.load() >= 64; }));
    CHECK(wait_until(c, [&] { return a.skipped.load() >= 4; })); // due, no slot
    CHECK(callbacks_a == 64);
    // four frames: four callbacks more, as the output plays them
    uint32_t silence = M->system_alloc(6 * 256 * 4);
    for (int i = 0; i < 4; i++) CHECK(ex(c, "XAudioSubmitRenderDriverFrame", {driver_a, silence}) == 0);
    CHECK(wait_until(c, [] { return callbacks_a.load() >= 68; }));
    uint64_t skipped = a.skipped.load();
    CHECK(wait_until(c, [&] { return a.skipped.load() >= skipped + 4; }));
    CHECK(callbacks_a == 68);

    // B, with A still there: index 1; a frame of 0.25 + channel + 0.5 * i
    frame_b = M->system_alloc(6 * 256 * 4);
    for (uint32_t ch = 0; ch < 6; ch++)
        for (uint32_t i = 0; i < 256; i++)
            K->store32(frame_b + 4 * (ch * 256 + i), std::bit_cast<uint32_t>(0.25f + float(ch) + 0.5f * float(i)));
    K->store32(reg, F_CALLBACK_B), K->store32(reg + 4, ARG_B);
    uint64_t frames_before = a.frames.load();
    CHECK(ex(c, "XAudioRegisterRenderDriverClient", {reg, driver}) == 0);
    driver_b = K->load32(driver);
    CHECK(driver_b == 0x41550001);
    CHECK(wait_until(c, [] { return callbacks_b.load() >= 100; }));
    // never faster than the output plays (the first callback's lateness
    // aside), each callback's frame submitted
    uint64_t span = times_b[99] - times_b[0];
    CHECK(span >= 99 * FRAME_100NS * 95 / 100);
    if (verbose)
        std::printf("client B: 100 callbacks in %.1f ms (%.1f a second; the output plays 187.5)\n",
                    double(span) / 10'000.0, 99.0 * 10'000'000.0 / double(span));
    CHECK(a.frames.load() - frames_before >= 100);

    // gone: no callback after the unregistration returns, even if one ran
    CHECK(ex(c, "XAudioUnregisterRenderDriverClient", {driver_b}) == 0);
    CHECK(!in_callback_b);
    uint32_t after = callbacks_b;
    uint64_t submitted = a.frames.load();
    CHECK(ex(c, "XAudioUnregisterRenderDriverClient", {driver_a}) == 0);
    uint64_t callbacks = a.callbacks.load();
    sleep_ms(c, 30);
    CHECK(callbacks_b == after && callbacks_a == 68 && a.callbacks.load() == callbacks);
    // a frame for a client gone goes nowhere
    uint64_t none = a.submitted_to_none.load();
    CHECK(ex(c, "XAudioSubmitRenderDriverFrame", {driver_b, frame_b}) == 0);
    CHECK(a.submitted_to_none.load() == none + 1 && a.frames.load() == submitted);
    CHECK(seen.bad_frames == 0 && seen.frames.load() == a.frames.load());

    // eight clients at most (their callbacks are A's: no frames, 64 calls each)
    K->store32(reg, F_CALLBACK_A), K->store32(reg + 4, ARG_A);
    uint32_t drivers[8];
    for (uint32_t i = 0; i < 8; i++) {
        CHECK(ex(c, "XAudioRegisterRenderDriverClient", {reg, driver}) == 0);
        drivers[i] = K->load32(driver);
        CHECK(drivers[i] == (0x41550000 | i));
    }
    CHECK(ex(c, "XAudioRegisterRenderDriverClient", {reg, driver}) == 0x80070008);
    for (uint32_t d : drivers) CHECK(ex(c, "XAudioUnregisterRenderDriverClient", {d}) == 0);
    CHECK(bad_callbacks == 0);
}

// ---- XMA ----

constexpr uint32_t PACKET = 2048, PACKET_DATA_BITS = (2048 - 4) * 8;

// a frame's bits: its 15-bit size, a pattern, and the last bit telling
// whether another frame follows in the packet
std::vector<bool> make_frame(uint32_t size, uint32_t seed, bool more) {
    std::vector<bool> bits(size);
    for (uint32_t i = 0; i < 15; i++) bits[i] = (size >> (14 - i)) & 1;
    for (uint32_t i = 15; i + 1 < size; i++) bits[i] = ((i * 7 + seed * 13) >> 2) & 1;
    bits[size - 1] = more;
    return bits;
}

// An input buffer: packets whose data, past each 32-bit header, is one bit
// stream with the frames in it (one may run into the next packet).
struct Input {
    uint32_t packets, cpu = 0;
    std::vector<uint8_t> bytes;
    explicit Input(uint32_t n) : packets(n), bytes(n * PACKET, 0) {}
    // a frame at a bit of the data stream
    void frame(uint32_t stream_bit, const std::vector<bool> &bits) {
        for (uint32_t i = 0; i < bits.size(); i++) {
            uint32_t s = stream_bit + i, p = s / PACKET_DATA_BITS, b = 32 + s % PACKET_DATA_BITS;
            if (bits[i]) bytes[p * PACKET + b / 8] |= uint8_t(0x80 >> (b % 8));
        }
    }
    // an XMA2 packet header: the frames that begin in it, the first one's
    // offset in bits from the packet's start, the packets to skip
    void header(uint32_t p, uint32_t frames, uint32_t first_frame_bit, uint8_t skip = 0) {
        uint32_t field = first_frame_bit - 32;
        uint8_t *h = &bytes[p * PACKET];
        h[0] = uint8_t(frames << 2 | (field >> 13 & 3));
        h[1] = uint8_t(field >> 5);
        h[2] = uint8_t((field & 0x1F) << 3 | 1);
        h[3] = skip;
    }
    void put() {
        cpu = M->system_alloc(uint32_t(bytes.size()), 0x1000, true);
        std::memcpy(K->host(cpu), bytes.data(), bytes.size());
    }
};

// The decoder: each frame checked against the next one expected for its
// context, its PCM a pattern that names it: each 256-byte block
// (seq << 4 | block).
struct Expected {
    uint32_t seq;
    std::vector<bool> bits;
    uint32_t channels;
};
std::deque<Expected> expected[320];
std::atomic<uint32_t> decoded{0}, bad_decodes{0};

class LookingDecoder final : public audio::FrameDecoder {
public:
    bool decode(uint32_t context, const uint8_t *packet, uint32_t size, uint32_t rate, uint32_t channels,
                uint8_t *pcm) override {
        decoded++;
        if (context >= 320 || expected[context].empty()) {
            bad_decodes++;
            return false;
        }
        Expected e = std::move(expected[context].front());
        expected[context].pop_front();
        uint32_t pad = packet[0] >> 5, pad_end = (packet[0] >> 2) & 7;
        bool ok = rate == 48000 && channels == e.channels && 8 + pad + e.bits.size() + pad_end == size * 8;
        for (uint32_t i = 0; ok && i < e.bits.size(); i++) {
            uint32_t b = 8 + pad + i;
            ok = bool(packet[b / 8] & (0x80 >> (b % 8))) == e.bits[i];
        }
        if (!ok) {
            std::printf("context %u: frame %u is not the one expected\n", context, e.seq);
            bad_decodes++;
        }
        for (uint32_t block = 0; block < 4 * channels; block++)
            std::memset(pcm + 256 * block, int(e.seq << 4 | block), 256);
        return true;
    }
};

// the byte the decoder fills a block with
uint32_t pattern(uint32_t seq, uint32_t block) { return (seq << 4 | block) & 0xFF; }

struct Context {
    uint32_t guest;
    XmaContextData data() const { return XmaContextData(K->host(guest)); }
};

// XMA_CONTEXT_INIT, then XMAInitializeContext
void initialize(ppc::Context &c, uint32_t context, uint32_t input0, uint32_t packets0, uint32_t input1,
                uint32_t packets1, uint32_t output, uint32_t blocks, uint32_t subframes, uint32_t stereo) {
    uint32_t init = scratch + 0x400;
    std::memset(K->host(init), 0, 56);
    K->store32(init + 0, input0), K->store32(init + 4, packets0);
    K->store32(init + 8, input1), K->store32(init + 12, packets1);
    K->store32(init + 16, 0x1234); // the read offset: the clear sets it to the first packet's data
    K->store32(init + 20, output), K->store32(init + 24, blocks);
    K->store32(init + 32, subframes), K->store32(init + 36, stereo), K->store32(init + 40, 3); // 48 kHz
    CHECK(ex(c, "XMAInitializeContext", {context, init}) == 0);
}

// the output's 256-byte blocks: each one's first byte, the whole block alike
std::vector<int> blocks(uint32_t output, uint32_t n) {
    std::vector<int> out;
    for (uint32_t b = 0; b < n; b++) {
        const uint8_t *p = K->host(output + 256 * b);
        bool same = std::all_of(p, p + 256, [&](uint8_t v) { return v == p[0]; });
        out.push_back(same ? p[0] : -1);
    }
    return out;
}

void xma(ppc::Context &c) {
    Xma &x = K->state<Xma>();
    // the bitfields where Xenia's XMA_CONTEXT_DATA has them
    {
        XmaContextData d;
        d.input_buffer_0_valid = 1, d.output_buffer_write_offset = 3, d.is_stereo = 1, d.output_buffer_valid = 1;
        d.input_buffer_read_offset = 0x20, d.current_buffer = 1, d.output_buffer_read_offset = 5;
        CHECK(d.word(0) == (1u << 20 | 3u << 27) && d.word(1) == (1u << 29 | 1u << 31));
        CHECK(d.word(2) == 0x20 && d.word(4) == 1u << 31 && d.word(9) == 5);
    }

    uint32_t out_ptr = scratch + 0x300;
    CHECK(ex(c, "XMACreateContext", {out_ptr}) == 0);
    Context c0{K->load32(out_ptr)};
    CHECK(ex(c, "XMACreateContext", {out_ptr}) == 0);
    Context c1{K->load32(out_ptr)};
    CHECK(ex(c, "XMACreateContext", {out_ptr}) == 0);
    Context c2{K->load32(out_ptr)};
    CHECK(c0.guest == x.context_array && c1.guest == c0.guest + 64 && c2.guest == c0.guest + 128);
    CHECK(c0.guest >= 0xE0000000);
    // the registers: the array's physical address, a rotating context index
    CHECK(xma_read(c, Xma::CONTEXT_ARRAY_ADDRESS) == M->physical_address(c0.guest));
    uint32_t index = xma_read(c, Xma::CURRENT_CONTEXT_INDEX);
    CHECK(xma_read(c, Xma::CURRENT_CONTEXT_INDEX) == (index + 1) % 320);
    xma_write(c, 0x601, 0x02000000); // around a lock: stored, nothing else

    // ---- c0: mono, frames of 1001 bits, three to a packet; buffer 0 has two
    // packets, buffer 1 one; 16 output blocks (four frames), 4 subframes at a time
    Input in0(2), in1(1);
    uint32_t seq = 0;
    for (Input *in : {&in0, &in1}) {
        for (uint32_t p = 0; p < in->packets; p++) {
            in->header(p, 3, 32);
            for (uint32_t f = 0; f < 3; f++) {
                std::vector<bool> bits = make_frame(1001, seq, f < 2);
                in->frame(p * PACKET_DATA_BITS + f * 1001, bits);
                expected[0].push_back({seq++, std::move(bits), 1});
            }
        }
        in->put();
    }
    uint32_t out0 = M->system_alloc(16 * 256, 0x1000, true);
    initialize(c, c0.guest, in0.cpu, 2, in1.cpu, 1, out0, 16, 4, 0);
    XmaContextData d = c0.data();
    CHECK(d.input_buffer_read_offset == 32 && !d.input_buffer_0_valid && !d.output_buffer_valid); // cleared
    CHECK(d.input_buffer_0_ptr == M->physical_address(in0.cpu) && d.output_buffer_block_count == 16);
    CHECK(d.subframe_decode_count == 4 && !d.is_stereo && d.sample_rate == 3);
    ex(c, "XMASetInputBuffer0Valid", {c0.guest});
    ex(c, "XMASetInputBuffer1Valid", {c0.guest});
    ex(c, "XMASetOutputBufferValid", {c0.guest});
    CHECK(ex(c, "XMAIsInputBuffer0Valid", {c0.guest}) == 1 && ex(c, "XMAIsInputBuffer1Valid", {c0.guest}) == 1);

    // kick 1: frames 0-2 of packet 0 and frame 0 of packet 1 fill the 16
    // blocks; the read offset at packet 1's frame 1 (16384 + 32 + 1001); the
    // write offset back at 0 and the output invalid (full)
    std::memset(K->host(out0), 0xCD, 16 * 256);
    CHECK(ex(c, "XMAEnableContext", {c0.guest}) == 0);
    d = c0.data();
    CHECK(d.input_buffer_read_offset == 17417 && d.current_buffer == 0);
    CHECK(d.input_buffer_0_valid && d.input_buffer_1_valid && !d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 0 && d.output_buffer_read_offset == 0);
    CHECK(ex(c, "XMAGetInputBufferReadOffset", {c0.guest}) == 17417);
    CHECK(ex(c, "XMAIsOutputBufferValid", {c0.guest}) == 0 && ex(c, "XMAGetOutputBufferWriteOffset", {c0.guest}) == 0);
    std::vector<int> want;
    for (uint32_t s = 0; s < 4; s++)
        for (uint32_t b = 0; b < 4; b++) want.push_back(int(pattern(s, b)));
    CHECK(blocks(out0, 16) == want);
    CHECK(expected[0].size() == 5);

    // kick 2 (by the register, as the games' XAudio does): packet 1's frames
    // 1 and 2, then buffer 1's first two after the swap (buffer 0 invalid)
    ex(c, "XMASetOutputBufferValid", {c0.guest});
    xma_write(c, Xma::KICK, 1u << 0);
    d = c0.data();
    CHECK(d.input_buffer_read_offset == 2034 && d.current_buffer == 1);
    CHECK(!d.input_buffer_0_valid && d.input_buffer_1_valid && !d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 0);
    CHECK(expected[0].size() == 1);

    // kick 3: buffer 1's last frame, then nothing valid: 4 blocks written,
    // the output still valid, back to buffer 0 at its first packet
    ex(c, "XMASetOutputBufferValid", {c0.guest});
    std::memset(K->host(out0), 0xCD, 16 * 256);
    xma_write(c, Xma::KICK, 1u << 0);
    d = c0.data();
    CHECK(d.input_buffer_read_offset == 32 && d.current_buffer == 0);
    CHECK(!d.input_buffer_0_valid && !d.input_buffer_1_valid && d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 4 && expected[0].empty());
    want.assign(16, 0xCD);
    for (uint32_t b = 0; b < 4; b++) want[b] = int(pattern(8, b));
    CHECK(blocks(out0, 16) == want);

    // kick 4: starved of input, the write offset meets the read offset and
    // the output is invalid
    ex(c, "XMAEnableContext", {c0.guest});
    d = c0.data();
    CHECK(d.output_buffer_write_offset == 0 && !d.output_buffer_valid && d.input_buffer_read_offset == 32);
    CHECK(x.swaps.load() == 2);

    // ---- c1: mono, frames of 5000 bits in one buffer of two packets: packet
    // 0 has four, the last running into packet 1 (whose first frame then
    // starts at 3680), packet 1 two more
    Input in2(2);
    std::vector<std::vector<bool>> f1;
    for (uint32_t i = 0; i < 6; i++) f1.push_back(make_frame(5000, 100 + i, i != 3 && i != 5));
    for (uint32_t i = 0; i < 6; i++) in2.frame(i * 5000, f1[i]);
    in2.header(0, 4, 32);
    in2.header(1, 2, 3680);
    in2.put();
    for (uint32_t i : {0, 1, 2, 3}) expected[1].push_back({20 + i, f1[i], 1});
    uint32_t out1 = M->system_alloc(16 * 256, 0x1000, true);
    initialize(c, c1.guest, in2.cpu, 2, 0, 0, out1, 16, 4, 0);
    ex(c, "XMASetInputBuffer0Valid", {c1.guest});
    ex(c, "XMASetOutputBufferValid", {c1.guest});
    // kick 1: the four frames of packet 0, the split one whole; the read
    // offset at packet 1's first frame
    xma_write(c, Xma::KICK, 1u << 1);
    d = c1.data();
    CHECK(d.input_buffer_read_offset == 16384 + 3680 && d.input_buffer_0_valid && !d.output_buffer_valid);
    CHECK(expected[1].empty());

    // kick 2, with a loop from packet 0's frame 1 (5032) to packet 1's frame
    // 1 (16384 + 8680), once, ending at subframe 1 and skipping one: packet
    // 1's frame 0 (4 blocks); at the loop's end back to frame 1, of which
    // one subframe (the skip takes the first, the end the last two); frames
    // 2 and 3 (8 blocks); 13 blocks written, 3 left, the output still valid
    for (uint32_t i : {4, 1, 2, 3}) expected[1].push_back({20 + i, f1[i], 1});
    uint32_t loop = scratch + 0x500;
    K->store32(loop + 0, 5032), K->store32(loop + 4, 16384 + 8680);
    K->host(loop)[8] = 1, K->host(loop)[9] = 1, K->host(loop)[10] = 1;
    CHECK(ex(c, "XMASetLoopData", {c1.guest, loop}) == 0);
    ex(c, "XMASetOutputBufferValid", {c1.guest});
    std::memset(K->host(out1), 0xCD, 16 * 256);
    uint64_t loops = x.loops.load();
    CHECK(ex(c, "XMAEnableContext", {c1.guest}) == 0);
    d = c1.data();
    CHECK(d.input_buffer_read_offset == 16384 + 3680 && d.loop_count == 0 && d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 13 && d.input_buffer_0_valid && expected[1].empty());
    CHECK(x.loops.load() == loops + 1);
    want = {int(pattern(24, 0)), int(pattern(24, 1)), int(pattern(24, 2)), int(pattern(24, 3)), int(pattern(21, 1))};
    for (uint32_t s : {22, 23})
        for (uint32_t b = 0; b < 4; b++) want.push_back(int(pattern(s, b)));
    for (int i = 0; i < 3; i++) want.push_back(0xCD);
    CHECK(blocks(out1, 16) == want);

    // ---- c2: stereo, one packet of three 1001-bit frames, 8 output blocks,
    // 2 subframes at a time: one frame's 8 subframes fill them
    Input in3(1);
    std::vector<std::vector<bool>> f2;
    for (uint32_t i = 0; i < 3; i++) f2.push_back(make_frame(1001, 200 + i, i < 2));
    for (uint32_t i = 0; i < 3; i++) in3.frame(i * 1001, f2[i]);
    in3.header(0, 3, 32);
    in3.put();
    expected[2].push_back({1, f2[0], 2});
    uint32_t out2 = M->system_alloc(8 * 256, 0x1000, true);
    initialize(c, c2.guest, in3.cpu, 1, 0, 0, out2, 8, 2, 1);
    CHECK(c2.data().is_stereo);
    ex(c, "XMASetInputBuffer0Valid", {c2.guest});
    ex(c, "XMASetOutputBufferValid", {c2.guest});
    xma_write(c, Xma::KICK, 1u << 2);
    d = c2.data();
    CHECK(d.input_buffer_read_offset == 1033 && d.input_buffer_0_valid && !d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 0 && expected[2].empty());
    want.clear();
    for (uint32_t b = 0; b < 8; b++) want.push_back(int(pattern(1, b)));
    CHECK(blocks(out2, 8) == want);

    // a lock, and a kick of a context nobody made (it completes, untouched)
    CHECK(ex(c, "XMADisableContext", {c1.guest, 1}) == 0);
    CHECK(ex(c, "XMABlockWhileInUse", {c1.guest}) == 0);
    xma_write(c, Xma::KICK + 1, 1u << 5); // context 37
    CHECK(XmaContextData(K->host(x.context_array + 37 * 64)).word(0) == 0);
    // a clear: the offsets and flags start over
    xma_write(c, Xma::CLEAR, 1u << 1);
    d = c1.data();
    CHECK(d.input_buffer_read_offset == 32 && !d.input_buffer_0_valid && !d.output_buffer_valid);
    CHECK(d.output_buffer_write_offset == 0 && d.input_buffer_0_ptr == M->physical_address(in2.cpu));

    // released: zeroed, and the first free one is given again
    CHECK(ex(c, "XMAReleaseContext", {c0.guest}) == 0);
    CHECK(c0.data().word(0) == 0 && c0.data().input_buffer_0_ptr == 0);
    CHECK(ex(c, "XMACreateContext", {out_ptr}) == 0 && K->load32(out_ptr) == c0.guest);
    for (const Context &ctx : {c0, c1}) ex(c, "XMAReleaseContext", {ctx.guest});
    CHECK(x.created.load() == 4 && x.released.load() == 3);
    CHECK(x.frames.load() == 18 && decoded == 18 && bad_decodes == 0);
    CHECK(x.errors.load() == 0);
}

void main_function(ppc::Context &c) {
    queries(c);
    clients(c);
    xma(c);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_function},
    {F_CALLBACK_A, callback_a},
    {F_CALLBACK_B, callback_b},
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
    callbacks_a = 0, callbacks_b = 0, bad_callbacks = 0, in_callback_b = false;
    seen.frames = 0, seen.bad_frames = 0;
    times_b.fill(0);
    decoded = 0, bad_decodes = 0;
    for (auto &e : expected) e.clear();
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        set_audio_output(k, [] { return std::make_unique<LookingOutput>(); });
        set_xma_decoder(k, std::make_unique<LookingDecoder>());
        scratch = memory->system_alloc(0x1000);
        Objects &o = objects(k);
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());

        // a kick from a host thread with no guest thread: walked there (c2's
        // next frame, as the decoder's thread would)
        Xma &x = k.state<Xma>();
        uint32_t c2 = x.context_array + 128;
        expected[2].clear();
        ppc::Context hc{};
        k.attach(hc);
        ex(hc, "XMASetOutputBufferValid", {c2});
        uint64_t frames = x.frames.load();
        std::vector<bool> f1 = make_frame(1001, 201, true);
        expected[2].push_back({2, f1, 2});
        ex(hc, "XMAEnableContext", {c2});
        CHECK(XmaContextData(k.host(c2)).input_buffer_read_offset == 2034 && x.frames.load() == frames + 1);
        CHECK(bad_decodes == 0);

        std::string report = audio_report(k);
        CHECK(report.find("audio: 10 clients registered") != std::string::npos);
        CHECK(report.find("xma: 4 contexts created (3 released)") != std::string::npos);
        {
            // what is left: the GPU's threads are not made; the audio
            // worker and the XMA decoder
            std::lock_guard<std::mutex> l(o.lock);
            CHECK(o.threads.size() == 2);
            for (Thread *g : o.threads) CHECK(g->params.host && g->params.name);
        }
        // they stop with the kernel; here first, to see them go
        shutdown_audio(k);
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    M = nullptr, K = nullptr;
}

} // namespace

int main(int argc, char **argv) {
    int repeats = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    for (int i = 0; i < repeats && !failures; i++) {
        run();
        verbose = false;
    }
    return finish("kernel_audio");
}
