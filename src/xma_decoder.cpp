/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The decode of one XMA frame (xenoforge/audio.h, FrameDecoder; plan step
// A6): FFmpeg's xmaframes decoder, from has207's fork (third_party/ffmpeg,
// LGPL, a shared library of its own), as Xenia drives it. Translated from
// Xenia (BSD; see LICENSE-xenia): apu/xma_context_new.cc (Setup,
// PrepareDecoder, DecodePacket, the codec context per XMA context) and
// apu/xma_context.cc (ConvertFrame); checked against them. Where it differs:
//
// - The packet is handed to FFmpeg without a buffer of its own, so FFmpeg
//   copies it (Xenia sets an unused 128 KB buffer on the packet, and FFmpeg
//   then reads the frame in place): the same bytes either way.
// - FFmpeg's log goes through a callback that prints its first errors and
//   counts the rest (Xenia lets FFmpeg print every one).
// - Built without FFmpeg (CMake's XF_XMA off), every frame is silence.

#include "xenoforge/audio.h"

#include <cstdio>
#include <cstring>

#if XF_XMA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#endif

namespace xf::audio {
namespace {

constexpr uint32_t SAMPLES_PER_FRAME = 512;

class SilentDecoder final : public FrameDecoder {
public:
    bool decode(uint32_t, const uint8_t *, uint32_t, uint32_t, uint32_t channels, uint8_t *pcm) override {
        std::memset(pcm, 0, SAMPLES_PER_FRAME * 2 * channels); // 16-bit samples
        return true;
    }
};

#if XF_XMA

// FFmpeg's messages: its first errors printed, every one counted
std::atomic<uint32_t> ffmpeg_messages{0};
constexpr uint32_t PRINTED_MESSAGES = 20;

void ffmpeg_log(void *, int level, const char *format, va_list args) {
    if (level > AV_LOG_ERROR) return;
    uint32_t n = ffmpeg_messages.fetch_add(1);
    if (n >= PRINTED_MESSAGES) return;
    char line[512];
    std::vsnprintf(line, sizeof(line), format, args);
    std::fprintf(stderr, "ffmpeg: %s%s", line, n + 1 == PRINTED_MESSAGES ? "ffmpeg: (no more messages printed)\n" : "");
}

// ConvertFrame: each sample times 32767, rounded to the nearest (even on a
// tie), saturated to 16 bits, big-endian, the channels interleaved. Xenia's
// x86 path (cvtps2dq, packssdw) turns a NaN or a value past 32 bits into
// -32768, and so does this.
inline int16_t to_sample(float v) {
    v *= float((1 << 15) - 1);
#if defined(__x86_64__) || defined(_M_X64)
    int32_t i = _mm_cvt_ss2si(_mm_set_ss(v));
#else
    int32_t i = std::isnan(v) ? INT32_MIN : int32_t(std::lrintf(std::clamp(v, -2147483648.0f, 2147483520.0f)));
#endif
    return int16_t(std::clamp(i, -32768, 32767));
}

// (a stereo context whose frame has one channel gets it as a mono frame's,
// the rest of the output left as it was, as Xenia's x86 path does)
void convert_frame(const float *const *samples, bool two_channels, uint8_t *out) {
    const uint32_t channels = two_channels && samples[1] ? 2 : 1;
    uint32_t o = 0;
    for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            uint16_t s = uint16_t(to_sample(samples[ch][i]));
            out[o++] = uint8_t(s >> 8);
            out[o++] = uint8_t(s);
        }
    }
}

class FfmpegDecoder final : public FrameDecoder {
public:
    static constexpr uint32_t CONTEXTS = 320;

    FfmpegDecoder() {
        av_log_set_callback(ffmpeg_log);
        codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!codec_) std::fprintf(stderr, "xma: FFmpeg has no xmaframes decoder; frames decode to nothing\n");
    }
    ~FfmpegDecoder() override {
        for (State &s : states_) {
            if (s.context) avcodec_free_context(&s.context);
            if (s.frame) av_frame_free(&s.frame);
            if (s.packet) av_packet_free(&s.packet);
        }
    }

    bool decode(uint32_t context, const uint8_t *data, uint32_t size, uint32_t sample_rate, uint32_t channels,
                uint8_t *pcm) override {
        if (!codec_ || context >= CONTEXTS) return false;
        State &s = states_[context];
        if (!s.frame && !setup(s)) return false;
        if (prepare(s, int(sample_rate), int(channels)) < 0) return false;
        s.packet->data = const_cast<uint8_t *>(data);
        s.packet->size = int(size);
        int ret = avcodec_send_packet(s.context, s.packet);
        s.packet->data = nullptr, s.packet->size = 0;
        if (ret < 0) return false;
        ret = avcodec_receive_frame(s.context, s.frame);
        if (ret < 0) return false; // EAGAIN: more input wanted first; else an error FFmpeg logged
        if (s.frame->nb_samples < int(SAMPLES_PER_FRAME)) {
            av_frame_unref(s.frame);
            return false;
        }
        convert_frame(reinterpret_cast<const float *const *>(s.frame->data), channels == 2, pcm);
        av_frame_unref(s.frame);
        return true;
    }

private:
    struct State {
        AVCodecContext *context = nullptr;
        AVFrame *frame = nullptr;
        AVPacket *packet = nullptr;
    };

    // Setup: the codec context made, not opened (PrepareDecoder opens it)
    bool setup(State &s) {
        s.packet = av_packet_alloc();
        s.context = avcodec_alloc_context3(codec_);
        s.frame = av_frame_alloc();
        if (!s.packet || !s.context || !s.frame) return false;
        s.context->ch_layout = AVChannelLayout{};
        s.context->sample_rate = 0;
        return true;
    }

    // PrepareDecoder: a new codec context when the rate or the channels change
    int prepare(State &s, int sample_rate, int channels) {
        if (s.context->sample_rate == sample_rate && s.context->ch_layout.nb_channels == channels) return 0;
        avcodec_free_context(&s.context);
        s.context = avcodec_alloc_context3(codec_);
        if (!s.context) return -1;
        s.context->sample_rate = sample_rate;
        av_channel_layout_default(&s.context->ch_layout, channels);
        s.context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        if (avcodec_open2(s.context, codec_, nullptr) < 0) {
            std::fprintf(stderr, "xma: FFmpeg's xmaframes decoder did not open (%d Hz, %d channels)\n", sample_rate,
                         channels);
            // a closed context: the next frame tries again
            s.context->sample_rate = 0;
            return -1;
        }
        return 1;
    }

    const AVCodec *codec_ = nullptr;
    std::array<State, CONTEXTS> states_;
};

#endif

} // namespace

std::unique_ptr<FrameDecoder> make_silent_decoder() { return std::make_unique<SilentDecoder>(); }

std::unique_ptr<FrameDecoder> make_ffmpeg_decoder() {
#if XF_XMA
    return std::make_unique<FfmpegDecoder>();
#else
    return nullptr;
#endif
}

std::unique_ptr<FrameDecoder> make_xma_decoder() {
    if (auto d = make_ffmpeg_decoder()) return d;
    return make_silent_decoder();
}

uint32_t ffmpeg_errors() {
#if XF_XMA
    return ffmpeg_messages.load();
#else
    return 0;
#endif
}

} // namespace xf::audio
