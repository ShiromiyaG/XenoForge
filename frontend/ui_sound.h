// The launcher's own sounds (docs/runtime.md, "The launcher's interface"): a
// tick as the selection moves, a swish as a page or a tab changes, a soft
// note for choosing and another for going back, a pop for a menu. They are
// made here, not loaded: each cue is synthesized once when the device opens
// (a few sine notes and a swept band of noise, no recording of anything), so
// there is no sound file to ship. One SDL3 audio stream on the default
// playback device mixes whatever is playing; a cue is a few tenths of a
// second, short enough that two overlap rather than queue.

#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace xf::frontend {

enum class Cue {
    Focus,     // the selection or the focus moved to another entry
    Select,    // an entry chosen, a button pressed
    Back,      // going back, a dialog closed
    Tab,       // another tab or menu title
    Page,      // the strip sliding to the other page
    MenuOpen,  // a menu, a dropdown or a dialog opening
    MenuClose, // a menu closing
    Toggle,    // a check box or a dropdown's value changed
    Deny,      // something that cannot be done, or a build that failed
    Done,      // a build that finished
    Count
};

constexpr int UI_SOUND_RATE = 48000;

// A cue's samples: interleaved stereo floats at UI_SOUND_RATE, peak below 1.
std::vector<float> synth_cue(Cue cue);

class UiSound {
  public:
    UiSound() = default;
    UiSound(const UiSound &) = delete;
    UiSound &operator=(const UiSound &) = delete;
    ~UiSound();

    // The default playback device opened and the cues made; false, and why, without a device.
    bool open(std::string &error);
    bool is_open() const { return stream_ != nullptr; }
    // A cue started (nothing when there is no device); the same cue twice within 30 ms is played once.
    void play(Cue cue);

  private:
    static void SDLCALL pull(void *self, SDL_AudioStream *stream, int additional, int total);
    void mix(float *out, int frames);

    SDL_AudioStream *stream_ = nullptr;
    bool audio_started_ = false;
    std::array<std::vector<float>, size_t(Cue::Count)> cues_;
    std::array<uint64_t, size_t(Cue::Count)> last_{};
    struct Voice {
        const std::vector<float> *samples;
        size_t at;
    };
    std::mutex lock_;
    std::vector<Voice> voices_;
};

} // namespace xf::frontend
