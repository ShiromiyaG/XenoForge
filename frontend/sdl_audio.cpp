// The player's sound (sdl_audio.h). The fold to stereo is Xenia's
// (apu/conversion.h, BSD; see LICENSE-xenia): its SSE path, which takes the
// fifth and sixth channels as back left and back right, XAudio2's 5.1 order
// (its plain C path swaps them). The queue's keeping is the 3DS runtime's
// player's feed_sound.

#include "sdl_audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace xf::frontend {

using audio::Output;

void downmix_stereo(const float *in, float *out) {
    constexpr uint32_t N = Output::CHANNEL_SAMPLES;
    for (uint32_t i = 0; i < N; i++) {
        const float fl = in[0 * N + i], fr = in[1 * N + i], fc = in[2 * N + i];
        const float bl = in[4 * N + i], br = in[5 * N + i];
        const float center = fc * 0.5f;
        out[i * 2] = (fl + bl + center) * (1.0f / 2.5f);
        out[i * 2 + 1] = (fr + br + center) * (1.0f / 2.5f);
    }
}

void interleave_six(const float *in, float *out) {
    constexpr uint32_t N = Output::CHANNEL_SAMPLES;
    for (uint32_t i = 0; i < N; i++)
        for (uint32_t ch = 0; ch < 6; ch++) out[i * 6 + ch] = in[ch * N + i];
}

// One client's output: the pacing output keeps the guest's time, the
// samples go on to SDL
class SdlOutput final : public Output {
  public:
    SdlOutput(Sound &owner, SDL_AudioStream *stream)
        : owner_(owner), stream_(stream), pacing_(audio::make_pacing_output()),
          buffer_(CHANNEL_SAMPLES * owner.channels_) {}
    ~SdlOutput() override {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

    void submit(const float *frame, uint64_t now) override {
        pacing_->submit(frame, now);
        owner_.submitted++;
        bool sound = false;
        for (uint32_t i = 0; i < FRAME_SAMPLES && !sound; i++) sound = std::fabs(frame[i]) > 1e-6f;
        if (sound) owner_.audible++;
        if (!stream_) return;
        if (owner_.channels_ == 6)
            interleave_six(frame, buffer_.data());
        else
            downmix_stereo(frame, buffer_.data());
        const int bytes = int(buffer_.size() * sizeof(float));
        const int per_second = int(FREQUENCY * owner_.channels_ * sizeof(float)), target = per_second * 60 / 1000;
        int queued = SDL_GetAudioStreamQueued(stream_);
        // nearly out before this came: a gap was heard; primed again with
        // silence to the target, so the next ones do not each run dry
        if (queued < per_second / 200) {
            if (started_) owner_.starved++;
            started_ = true;
            silence_.assign(size_t(std::max(target - queued, 0)) / sizeof(float), 0.0f);
            SDL_PutAudioStreamData(stream_, silence_.data(), int(silence_.size() * sizeof(float)));
            queued = SDL_GetAudioStreamQueued(stream_);
        }
        // a little faster with more queued than the target, slower with less
        float off = std::clamp(float(queued - target) / float(target), -1.0f, 1.0f);
        SDL_SetAudioStreamFrequencyRatio(stream_, 1.0f + 0.005f * off);
        if (queued > per_second / 4) {
            owner_.dropped++; // a quarter second behind: let go
            return;
        }
        SDL_PutAudioStreamData(stream_, buffer_.data(), bytes);
        owner_.sent++;
    }
    uint32_t played(uint64_t now) override { return pacing_->played(now); }

  private:
    Sound &owner_;
    SDL_AudioStream *stream_;
    std::unique_ptr<Output> pacing_;
    std::vector<float> buffer_, silence_;
    bool started_ = false;
};

bool Sound::open(std::string &error) {
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) return error = std::string("no sound device: ") + SDL_GetError(), false;
    SDL_AudioSpec spec{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(device_, &spec, &frames) && spec.channels >= 6) channels_ = 6;
    return true;
}

Sound::~Sound() {
    if (device_) SDL_CloseAudioDevice(device_);
}

void Sound::set_gain(float gain) {
    if (device_) SDL_SetAudioDeviceGain(device_, gain);
}

std::function<std::unique_ptr<Output>()> Sound::outputs() {
    return [this]() -> std::unique_ptr<Output> {
        SDL_AudioSpec spec{SDL_AUDIO_F32, int(channels_), int(Output::FREQUENCY)};
        SDL_AudioStream *stream = device_ ? SDL_CreateAudioStream(&spec, nullptr) : nullptr;
        if (stream && !SDL_BindAudioStream(device_, stream)) {
            std::fprintf(stderr, "sound: a stream not bound: %s\n", SDL_GetError());
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        return std::make_unique<SdlOutput>(*this, stream);
    };
}

} // namespace xf::frontend
