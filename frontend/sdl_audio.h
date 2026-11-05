// The player's sound (xenoforge/audio.h's Output; docs/runtime.md, "The
// player"): each render driver client's frames to the sound card through an
// SDL3 audio stream on the default playback device, 5.1 as the guest makes
// it where the device takes six channels, else folded to stereo as Xenia
// folds it. The guest's timing stays the scheduler's: each output is the
// pacing output (a frame played every 256/48000 s on the scheduler's clock)
// with the samples sent on to SDL beside it, the stream kept about 60 ms
// ahead by playing it a little faster or slower (half a percent at most), as
// the 3DS runtime's player keeps its own.

#pragma once

#include "xenoforge/audio.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xf::frontend {

// Xenia's sequential_6_BE_to_interleaved_2_LE (its SSE path, the one x64
// runs): the center halved on both sides, the back channels on their side,
// the LFE dropped, all over 2.5. frame: host floats, channel after channel.
void downmix_stereo(const float *frame, float *stereo);
// the six channels interleaved, in the guest's order (SDL's 5.1 order)
void interleave_six(const float *frame, float *out);

class Sound {
  public:
    // the default playback device opened; false, and why, without one
    bool open(std::string &error);
    ~Sound();
    // volume 0-1, 0 when muted
    void set_gain(float gain);
    // what makes each client's output (kernel::set_audio_output)
    std::function<std::unique_ptr<audio::Output>()> outputs();
    uint32_t channels() const { return channels_; }

    // frames submitted by the guest, sent to the device, dropped (more
    // than a quarter second queued), with a sample that is not silence;
    // and the times a stream had nearly run dry when a frame came
    std::atomic<uint64_t> submitted{0}, sent{0}, dropped{0}, audible{0}, starved{0};

  private:
    friend class SdlOutput;
    SDL_AudioDeviceID device_ = 0;
    uint32_t channels_ = 2;
};

} // namespace xf::frontend
