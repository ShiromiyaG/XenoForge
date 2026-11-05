// Sound's two seams (plan step A6 plugs into them; docs/runtime.md, "Sound"):
// where a render driver client's frames go, and the decode of one XMA frame.
// The kernel's APU (src/kernel/audio.cpp, xma.cpp) runs both headless until
// something else is set: each client's frames discarded at the playback rate
// (Xenia's PacingAudioDriver), every XMA frame decoded as silence.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace xf::kernel {
class Kernel;
}

namespace xf::audio {

// A render driver client's output (Xenia's AudioDriver): frames of 6
// channels of 256 samples at 48 kHz, as the games' XAudio submits them. The
// client may have 64 frames in flight; each one played frees a slot, and the
// kernel calls the client's callback only while it has a slot.
class Output {
public:
    static constexpr uint32_t FREQUENCY = 48000, CHANNELS = 6, CHANNEL_SAMPLES = 256;
    static constexpr uint32_t FRAME_SAMPLES = CHANNELS * CHANNEL_SAMPLES;

    virtual ~Output() = default;
    // A frame, FRAME_SAMPLES floats in the guest's order (each channel's 256
    // samples in turn: front left, front right, center, LFE, back left, back
    // right) made host floats, submitted at `now` on the scheduler's clock
    // (100 ns units, Kernel's one clock).
    virtual void submit(const float *frame, uint64_t now) = 0;
    // How many of the frames submitted have been played by `now` since the
    // last call.
    virtual uint32_t played(uint64_t now) = 0;
};

// Xenia's PacingAudioDriver on the scheduler's clock: the samples are
// discarded, and each frame is played 256/48000 s after the one before it,
// or after its own submission when the output had run dry.
std::unique_ptr<Output> make_pacing_output();

// The decode of one XMA frame (in Xenia's XmaContextNew, PrepareDecoder,
// DecodePacket and ConvertFrame over FFmpeg's xmaframes decoder); the
// context's walk through its packets, frames and buffers is the kernel's.
class FrameDecoder {
public:
    virtual ~FrameDecoder() = default;
    // One frame of context `context` (0-319; a decoder keeps its state by
    // it): `packet` is `size` bytes as FFmpeg's xmaframes decoder takes them
    // (a byte with the frame's first bit in its bits 5-7 and the padding
    // after its last in bits 2-4, then the frame's bits as the stream had
    // them), at `sample_rate` Hz with `channels` 1 or 2. Writes 512 samples
    // per channel, 16-bit big-endian, channels interleaved, to `pcm` (what
    // the output buffer gets); false when the frame gives no samples.
    virtual bool decode(uint32_t context, const uint8_t *packet, uint32_t size, uint32_t sample_rate, uint32_t channels,
                        uint8_t *pcm) = 0;
};

// every frame silence
std::unique_ptr<FrameDecoder> make_silent_decoder();

} // namespace xf::audio

namespace xf::kernel {

// Set before the game runs: what makes each client's output (called as the
// client registers), and the XMA frame decoder.
void set_audio_output(Kernel &k, std::function<std::unique_ptr<audio::Output>()> make);
void set_xma_decoder(Kernel &k, std::unique_ptr<audio::FrameDecoder> decoder);

} // namespace xf::kernel
