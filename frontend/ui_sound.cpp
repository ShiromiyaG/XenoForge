// The launcher's own sounds (ui_sound.h).

#include "ui_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xf::frontend {

namespace {

constexpr float PI = 3.14159265f;
constexpr float RATE = float(UI_SOUND_RATE);

// a mono sound being built
class Mono {
  public:
    explicit Mono(float seconds) : s_(size_t(seconds * RATE) + 1, 0.f) {}

    // A note: a sine with its second and third harmonics, gliding from f0 to f1 over its length,
    // a short attack then an exponential decay (decay: 1/s), starting `at` seconds in.
    void tone(float f0, float f1, float seconds, float decay, float at = 0.f, float amp = 1.f, float attack = 0.002f) {
        const size_t first = size_t(at * RATE), n = size_t(seconds * RATE);
        float phase = 0;
        for (size_t i = 0; i < n && first + i < s_.size(); i++) {
            const float t = float(i) / RATE, u = float(i) / float(n);
            const float f = f0 * std::pow(f1 / f0, u);
            phase += 2 * PI * f / RATE;
            const float env = std::min(t / attack, 1.f) * std::exp(-t * decay) * (1.f - u * u * u * u); // out to nothing at the end
            s_[first + i] += amp * env * (std::sin(phase) + 0.28f * std::sin(2 * phase) + 0.08f * std::sin(3 * phase));
        }
    }

    // A swish: noise through a band-pass whose centre rises from f0 to the peak at the middle and falls to f1,
    // under a smooth hump. q is the filter's damping (small: narrow, whistling).
    void swish(float seconds, float f0, float peak, float f1, float q, float at = 0.f, float amp = 1.f) {
        const size_t first = size_t(at * RATE), n = size_t(seconds * RATE);
        uint32_t seed = 0x1234567u + uint32_t(f0) * 7919u;
        float low = 0, band = 0;
        for (size_t i = 0; i < n && first + i < s_.size(); i++) {
            const float u = float(i) / float(n);
            const float centre = u < 0.5f ? f0 + (peak - f0) * std::sin(u * PI) : peak + (f1 - peak) * std::sin((u - 0.5f) * PI);
            seed = seed * 1664525u + 1013904223u;
            const float noise = float(int32_t(seed)) / 2147483648.f;
            const float f = 2.f * std::sin(PI * std::clamp(centre, 40.f, 9000.f) / RATE);
            const float high = noise - low - q * band;
            band += f * high;
            low += f * band;
            const float hump = std::sin(u * PI);
            s_[first + i] += amp * q * band * hump * hump;
        }
    }

    // scaled to a peak, then widened to stereo (the right a hair behind the left)
    std::vector<float> stereo(float peak) const {
        float top = 1e-6f;
        for (float v : s_) top = std::max(top, std::fabs(v));
        const float k = peak / top;
        std::vector<float> out(s_.size() * 2);
        constexpr size_t LAG = 14; // about 0.3 ms
        for (size_t i = 0; i < s_.size(); i++) {
            out[i * 2] = s_[i] * k;
            out[i * 2 + 1] = (i >= LAG ? s_[i - LAG] : 0.f) * k;
        }
        return out;
    }

  private:
    std::vector<float> s_;
};

} // namespace

std::vector<float> synth_cue(Cue cue) {
    switch (cue) {
    case Cue::Focus: {
        Mono m(0.08f);
        m.tone(2300, 1500, 0.07f, 75.f, 0.f, 1.f, 0.0008f);
        m.tone(1150, 1150, 0.07f, 95.f, 0.f, 0.3f, 0.0008f);
        return m.stereo(0.2f);
    }
    case Cue::Select: {
        Mono m(0.3f);
        m.tone(740, 740, 0.14f, 26.f, 0.f, 0.9f);
        m.tone(1110, 1110, 0.24f, 17.f, 0.05f, 1.f);
        return m.stereo(0.28f);
    }
    case Cue::Back: {
        Mono m(0.28f);
        m.tone(660, 660, 0.12f, 28.f, 0.f, 0.9f);
        m.tone(440, 440, 0.22f, 18.f, 0.055f, 1.f);
        return m.stereo(0.26f);
    }
    case Cue::Tab: {
        Mono m(0.2f);
        m.swish(0.18f, 700, 2600, 1000, 0.22f);
        m.tone(620, 1350, 0.15f, 14.f, 0.f, 0.22f, 0.02f);
        return m.stereo(0.27f);
    }
    case Cue::Page: {
        Mono m(0.36f);
        m.swish(0.34f, 330, 1900, 480, 0.3f);
        m.tone(260, 760, 0.3f, 8.f, 0.f, 0.18f, 0.05f);
        return m.stereo(0.3f);
    }
    case Cue::MenuOpen: {
        Mono m(0.14f);
        m.swish(0.12f, 900, 2500, 3300, 0.3f);
        m.tone(900, 1500, 0.09f, 26.f, 0.f, 0.3f, 0.004f);
        return m.stereo(0.22f);
    }
    case Cue::MenuClose: {
        Mono m(0.13f);
        m.swish(0.11f, 2900, 1800, 800, 0.3f);
        m.tone(1300, 760, 0.09f, 26.f, 0.f, 0.3f, 0.004f);
        return m.stereo(0.2f);
    }
    case Cue::Toggle: {
        Mono m(0.1f);
        m.tone(1500, 1500, 0.05f, 110.f, 0.f, 1.f, 0.001f);
        m.tone(1000, 1000, 0.05f, 110.f, 0.025f, 0.7f, 0.001f);
        return m.stereo(0.2f);
    }
    case Cue::Deny: {
        Mono m(0.3f);
        m.tone(196, 196, 0.1f, 22.f, 0.f, 1.f, 0.003f);
        m.tone(185, 185, 0.14f, 22.f, 0.12f, 1.f, 0.003f);
        return m.stereo(0.26f);
    }
    case Cue::Done: {
        Mono m(0.5f);
        m.tone(523.25f, 523.25f, 0.3f, 11.f, 0.f, 0.9f);
        m.tone(659.25f, 659.25f, 0.3f, 11.f, 0.08f, 0.9f);
        m.tone(783.99f, 783.99f, 0.4f, 9.f, 0.16f, 1.f);
        return m.stereo(0.28f);
    }
    default: return {};
    }
}

UiSound::~UiSound() {
    if (stream_) SDL_DestroyAudioStream(stream_); // closes the device it was opened with, and waits for a callback under way
    if (audio_started_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool UiSound::open(std::string &error) {
    if (stream_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return error = std::string("no sound: ") + SDL_GetError(), false;
    audio_started_ = true;
    for (size_t i = 0; i < cues_.size(); i++) cues_[i] = synth_cue(Cue(i));
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, UI_SOUND_RATE};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &UiSound::pull, this);
    if (!stream_) {
        error = std::string("no sound device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_started_ = false;
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void UiSound::play(Cue cue) {
    if (!stream_ || cue == Cue::Count) return;
    const uint64_t now = SDL_GetTicks();
    std::lock_guard guard(lock_);
    uint64_t &last = last_[size_t(cue)];
    if (last && now - last < 30) return;
    last = now;
    if (voices_.size() >= 16) voices_.erase(voices_.begin()); // the oldest let go
    voices_.push_back({&cues_[size_t(cue)], 0});
}

void UiSound::mix(float *out, int frames) {
    std::fill(out, out + size_t(frames) * 2, 0.f);
    std::lock_guard guard(lock_);
    for (Voice &v : voices_) {
        const size_t have = v.samples->size() / 2;
        const size_t n = std::min(have - v.at, size_t(frames));
        for (size_t i = 0; i < n; i++) {
            out[i * 2] += (*v.samples)[(v.at + i) * 2];
            out[i * 2 + 1] += (*v.samples)[(v.at + i) * 2 + 1];
        }
        v.at += n;
    }
    std::erase_if(voices_, [](const Voice &v) { return v.at >= v.samples->size() / 2; });
    for (int i = 0; i < frames * 2; i++) out[i] = std::tanh(out[i]); // never over, however many cues meet
}

void SDLCALL UiSound::pull(void *self, SDL_AudioStream *stream, int additional, int) {
    float chunk[512 * 2];
    while (additional > 0) {
        const int frames = std::min(512, std::max(additional / int(sizeof(float) * 2), 1));
        static_cast<UiSound *>(self)->mix(chunk, frames);
        SDL_PutAudioStreamData(stream, chunk, frames * 2 * int(sizeof(float)));
        additional -= frames * 2 * int(sizeof(float));
    }
}

} // namespace xf::frontend
