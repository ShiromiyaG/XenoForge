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
// - FFmpeg is loaded at run time (LoadLibrary, dlopen): the shared library
//   ffmpeg-xma beside the executable, else wherever the system finds it, and
//   the thirteen functions called here looked up in it. Without it one line
//   says so and every frame is silence, as when built without FFmpeg (CMake's
//   XF_XMA off); the program runs either way, and a user can put their own
//   build of the library in its place (the LGPL's terms, docs/runtime.md).
//   Xenia links its FFmpeg statically.

#include "xenoforge/audio.h"

#include <cstdio>
#include <cstring>

#if XF_XMA
// FFmpeg's headers for its types and constants only: nothing is linked
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
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
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

// The FFmpeg functions the decoder calls, looked up in ffmpeg-xma once, at the
// first decoder made
struct Ffmpeg {
#define XF_FFMPEG_FUNCTIONS(X)                                                                                         \
    X(av_log_set_callback)                                                                                             \
    X(avcodec_find_decoder)                                                                                            \
    X(avcodec_alloc_context3)                                                                                          \
    X(avcodec_free_context)                                                                                            \
    X(avcodec_open2)                                                                                                   \
    X(avcodec_send_packet)                                                                                             \
    X(avcodec_receive_frame)                                                                                           \
    X(av_channel_layout_default)                                                                                       \
    X(av_frame_alloc)                                                                                                  \
    X(av_frame_free)                                                                                                   \
    X(av_frame_unref)                                                                                                  \
    X(av_packet_alloc)                                                                                                 \
    X(av_packet_free)
#define XF_FFMPEG_POINTER(name) decltype(&::name) name = nullptr;
    XF_FFMPEG_FUNCTIONS(XF_FFMPEG_POINTER)
#undef XF_FFMPEG_POINTER
    // looked up too, not needed: a decoder kept and put back as opened
    // (FfmpegDecoder::keep) needs both; xf_xmaframes_state_size is only in
    // XenoForge's build of the library (third_party/ffmpeg/libavcodec/xf_wmaprodec.c)
    decltype(&::avcodec_flush_buffers) avcodec_flush_buffers = nullptr;
    int (*xf_xmaframes_state_size)() = nullptr;
};

#ifdef _WIN32
constexpr const char *FFMPEG_LIBRARY = "ffmpeg-xma.dll";
#elif defined(__APPLE__)
constexpr const char *FFMPEG_LIBRARY = "libffmpeg-xma.dylib";
#else
constexpr const char *FFMPEG_LIBRARY = "libffmpeg-xma.so";
#endif

// the directory of the running executable, where the library is shipped
std::filesystem::path executable_directory() {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    if (n == 0 || n >= std::size(path)) return {};
    return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(path, size_t(n))).parent_path();
#endif
}

// ffmpeg-xma beside the executable, else by its name where the system looks;
// nullptr, after one line saying why, when it is not there or lacks a function
const Ffmpeg *load_ffmpeg() {
    static const Ffmpeg *loaded = []() -> const Ffmpeg * {
        const std::filesystem::path beside = executable_directory() / FFMPEG_LIBRARY;
        std::string where = beside.string();
#ifdef _WIN32
        HMODULE library = LoadLibraryW(beside.c_str());
        if (!library) library = LoadLibraryA(FFMPEG_LIBRARY), where = FFMPEG_LIBRARY;
        auto symbol = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(library, name)); };
#else
        void *library = dlopen(beside.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library) library = dlopen(FFMPEG_LIBRARY, RTLD_NOW | RTLD_LOCAL), where = FFMPEG_LIBRARY;
        auto symbol = [&](const char *name) { return dlsym(library, name); };
#endif
        if (!library) {
            std::fprintf(stderr, "xma: no %s beside the executable or where the system finds libraries: XMA decodes "
                                 "to silence\n",
                         FFMPEG_LIBRARY);
            return nullptr;
        }
        static Ffmpeg f;
        const char *missing = nullptr;
#define XF_FFMPEG_LOOKUP(name)                                                                                         \
    f.name = reinterpret_cast<decltype(f.name)>(symbol(#name));                                                        \
    if (!f.name && !missing) missing = #name;
        XF_FFMPEG_FUNCTIONS(XF_FFMPEG_LOOKUP)
#undef XF_FFMPEG_LOOKUP
        if (missing) {
            std::fprintf(stderr, "xma: %s has no %s: XMA decodes to silence\n", where.c_str(), missing);
            return nullptr;
        }
        f.avcodec_flush_buffers = reinterpret_cast<decltype(f.avcodec_flush_buffers)>(symbol("avcodec_flush_buffers"));
        f.xf_xmaframes_state_size =
            reinterpret_cast<decltype(f.xf_xmaframes_state_size)>(symbol("xf_xmaframes_state_size"));
        return &f;
    }();
    return loaded;
}

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

    explicit FfmpegDecoder(const Ffmpeg &f) : f_(f) {
        f_.av_log_set_callback(ffmpeg_log);
        codec_ = f_.avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
        if (!codec_) std::fprintf(stderr, "xma: FFmpeg has no xmaframes decoder; frames decode to nothing\n");
        // XF_XMA_KEEP_DECODERS=0: every change of rate or channels opens a
        // new decoder, as Xenia does
        const char *keep = std::getenv("XF_XMA_KEEP_DECODERS");
        if (f_.avcodec_flush_buffers && f_.xf_xmaframes_state_size && !(keep && std::strcmp(keep, "0") == 0))
            state_size_ = size_t(std::max(0, f_.xf_xmaframes_state_size()));
    }
    ~FfmpegDecoder() override {
        for (State &s : states_) {
            if (s.context) f_.avcodec_free_context(&s.context);
            if (s.frame) f_.av_frame_free(&s.frame);
            if (s.packet) f_.av_packet_free(&s.packet);
        }
        for (auto &[key, kept] : kept_)
            for (Kept &k : kept) f_.avcodec_free_context(&k.context);
        if (opens_ || reuses_)
            std::fprintf(stderr, "xma: %u decoders opened, %u kept ones put back\n", opens_.load(), reuses_.load());
    }

    bool decode(uint32_t context, const uint8_t *data, uint32_t size, uint32_t sample_rate, uint32_t channels,
                uint8_t *pcm) override {
        if (!codec_ || context >= CONTEXTS) return false;
        State &s = states_[context];
        if (!s.frame && !setup(s)) return false;
        if (prepare(s, int(sample_rate), int(channels)) < 0) return false;
        s.packet->data = const_cast<uint8_t *>(data);
        s.packet->size = int(size);
        int ret = f_.avcodec_send_packet(s.context, s.packet);
        s.packet->data = nullptr, s.packet->size = 0;
        if (ret < 0) return false;
        ret = f_.avcodec_receive_frame(s.context, s.frame);
        if (ret < 0) return false; // EAGAIN: more input wanted first; else an error FFmpeg logged
        if (s.frame->nb_samples < int(SAMPLES_PER_FRAME)) {
            f_.av_frame_unref(s.frame);
            return false;
        }
        convert_frame(reinterpret_cast<const float *const *>(s.frame->data), channels == 2, pcm);
        f_.av_frame_unref(s.frame);
        return true;
    }

private:
    // A decoder's private state as avcodec_open2 left it, to put back: the
    // non-zero 64-byte blocks of it (the output and overlap buffers, most of
    // its 250 KB, are zero then), the rest written as zeros
    struct Opened {
        std::vector<std::pair<uint32_t, uint32_t>> blocks; // offset, length
        std::vector<uint8_t> bytes;
    };
    struct State {
        AVCodecContext *context = nullptr;
        std::unique_ptr<Opened> opened; // the context's state when opened, when kept (state_size_)
        AVFrame *frame = nullptr;
        AVPacket *packet = nullptr;
    };
    struct Kept {
        AVCodecContext *context = nullptr;
        std::unique_ptr<Opened> opened;
    };

    // Setup: the codec context made, not opened (PrepareDecoder opens it)
    bool setup(State &s) {
        s.packet = f_.av_packet_alloc();
        s.context = f_.avcodec_alloc_context3(codec_);
        s.frame = f_.av_frame_alloc();
        if (!s.packet || !s.context || !s.frame) return false;
        s.context->ch_layout = AVChannelLayout{};
        s.context->sample_rate = 0;
        return true;
    }

    // PrepareDecoder: a new codec context when the rate or the channels
    // change. Where Xenia opens one each time (the transforms' tables made
    // again: a quarter of a millisecond, at every context a game allocates
    // with another rate or channels than the one before in its slot), an
    // opened decoder is kept for the next context with its rate and
    // channels, its state put back as it was opened: the same as a new one
    int prepare(State &s, int sample_rate, int channels) {
        if (s.context->sample_rate == sample_rate && s.context->ch_layout.nb_channels == channels) return 0;
        if (s.opened)
            keep(s);
        else
            f_.avcodec_free_context(&s.context);
        if (take(s, sample_rate, channels)) return 1;
        s.context = f_.avcodec_alloc_context3(codec_);
        if (!s.context) return -1;
        s.context->sample_rate = sample_rate;
        f_.av_channel_layout_default(&s.context->ch_layout, channels);
        s.context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        if (f_.avcodec_open2(s.context, codec_, nullptr) < 0) {
            std::fprintf(stderr, "xma: FFmpeg's xmaframes decoder did not open (%d Hz, %d channels)\n", sample_rate,
                         channels);
            // a closed context: the next frame tries again
            s.context->sample_rate = 0;
            return -1;
        }
        if (state_size_ && s.context->priv_data) s.opened = save(s.context->priv_data);
        opens_++;
        return 1;
    }

    std::unique_ptr<Opened> save(const void *state) const {
        auto opened = std::make_unique<Opened>();
        const auto *bytes = static_cast<const uint8_t *>(state);
        constexpr uint32_t BLOCK = 64;
        for (uint32_t at = 0; at < state_size_; at += BLOCK) {
            const uint32_t n = uint32_t(std::min<size_t>(BLOCK, state_size_ - at));
            if (std::all_of(bytes + at, bytes + at + n, [](uint8_t b) { return b == 0; })) continue;
            if (!opened->blocks.empty() && opened->blocks.back().first + opened->blocks.back().second == at)
                opened->blocks.back().second += n;
            else
                opened->blocks.emplace_back(at, n);
            opened->bytes.insert(opened->bytes.end(), bytes + at, bytes + at + n);
        }
        return opened;
    }

    // the slot's decoder kept for another context, up to KEPT of them in all
    void keep(State &s) {
        const std::pair<int, int> key{s.context->sample_rate, s.context->ch_layout.nb_channels};
        {
            std::lock_guard<std::mutex> l(kept_mutex_);
            if (kept_count_ < KEPT) {
                kept_[key].push_back(Kept{s.context, std::move(s.opened)});
                kept_count_++;
                s.context = nullptr;
                return;
            }
        }
        f_.avcodec_free_context(&s.context);
        s.opened.reset();
    }

    // a kept decoder of this rate and channels, its state as when opened
    bool take(State &s, int sample_rate, int channels) {
        Kept k;
        {
            std::lock_guard<std::mutex> l(kept_mutex_);
            auto it = kept_.find({sample_rate, channels});
            if (it == kept_.end() || it->second.empty()) return false;
            k = std::move(it->second.back());
            it->second.pop_back();
            kept_count_--;
        }
        auto *state = static_cast<uint8_t *>(k.context->priv_data);
        std::memset(state, 0, state_size_);
        const uint8_t *from = k.opened->bytes.data();
        for (auto [at, n] : k.opened->blocks) std::memcpy(state + at, from, n), from += n;
        f_.avcodec_flush_buffers(k.context); // the codec context's own queues (the decoder has no flush)
        s.context = k.context;
        s.opened = std::move(k.opened);
        reuses_++;
        return true;
    }

    static constexpr uint32_t KEPT = 64;
    const Ffmpeg &f_;
    const AVCodec *codec_ = nullptr;
    std::array<State, CONTEXTS> states_;
    size_t state_size_ = 0; // 0: decoders not kept
    std::mutex kept_mutex_;
    std::map<std::pair<int, int>, std::vector<Kept>> kept_;
    uint32_t kept_count_ = 0;
    std::atomic<uint32_t> opens_{0}, reuses_{0};
};

#endif

} // namespace

std::unique_ptr<FrameDecoder> make_silent_decoder() { return std::make_unique<SilentDecoder>(); }

std::unique_ptr<FrameDecoder> make_ffmpeg_decoder() {
#if XF_XMA
    if (const Ffmpeg *f = load_ffmpeg()) return std::make_unique<FfmpegDecoder>(*f);
    return nullptr;
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
