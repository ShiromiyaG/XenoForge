/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The render driver side of the sound family (docs/runtime.md, "Sound"): the
// XAudio* exports, the clients, the worker that calls their callbacks, the
// pacing output, and the family's report. Translated from Xenia (BSD; see
// LICENSE-xenia): kernel/xboxkrnl/xboxkrnl_audio.cc (the exports),
// apu/audio_system.cc (RegisterClient, UnregisterClient, SubmitFrame,
// WorkerThreadMain), apu/pacing_audio_driver.cc (the output); checked
// against them. Where it differs:
//
// - The worker paces on the scheduler's clock and waits through the
//   kernel's objects (Xenia: the host's steady clock, a host event and a
//   precise host sleep), so the deterministic mode can run it as it runs the
//   guest's threads. A frame lasts 256/48000 s exactly (Xenia's pump: 5333
//   microseconds), counted in thirds of the clock's 100 ns.
// - A wake for a client coming or going does not cost the due client its
//   turn (Xenia advances the client's next pump before it waits).
// - The output is behind audio::Output (Xenia's AudioDriver). A client's
//   frame slots are counted by the worker (Xenia's per-client host
//   semaphore), and the pacing output keeps no thread of its own: the worker
//   asks it what has played when the client is due.
// - No host lock is held while a callback runs (Xenia holds the client's
//   mutex): an unregistration waits for a callback in progress on a kernel
//   event, unless it comes from the callback itself.
// - With every client taken, a registration fails with E_OUTOFMEMORY (Xenia
//   asserts).
// - The ducker's getters, which Xenia leaves unimplemented, answer as the
//   ducker off (XAudioEnableDucker is Xenia's no-op); their form, a float
//   in f1 and no arguments, is from the games' calls.

#include "audio.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace xf::audio {
namespace {

// a frame, 256 samples at 48 kHz, in thirds of the clock's 100 ns (5.333 ms)
constexpr uint64_t FRAME_THIRDS = 3ull * 10'000'000 * Output::CHANNEL_SAMPLES / Output::FREQUENCY;
static_assert(FRAME_THIRDS == 160'000);

// Xenia's PacingAudioDriver, on the scheduler's clock: the end of each frame
// submitted, the next one starting where the last ended, or now if the
// output ran dry
class PacingOutput final : public Output {
public:
    void submit(const float *, uint64_t now) override {
        next_end_ = std::max(now * 3, next_end_) + FRAME_THIRDS;
        ends_.push_back(next_end_);
    }
    uint32_t played(uint64_t now) override {
        uint32_t n = 0;
        for (; !ends_.empty() && ends_.front() <= now * 3; n++) ends_.pop_front();
        return n;
    }

private:
    std::deque<uint64_t> ends_; // in thirds of 100 ns
    uint64_t next_end_ = 0;
};

// The deterministic mode's (docs/runtime.md, "The deterministic mode"):
// another output (the player's device) gets the frames, but what has played
// is the pacing output's on the virtual clock, so the device's timing does not
// reach the guest
class VirtualClockOutput final : public Output {
public:
    explicit VirtualClockOutput(std::unique_ptr<Output> inner) : inner_(std::move(inner)) {}
    void submit(const float *frame, uint64_t now) override {
        inner_->submit(frame, now);
        pacing_.submit(frame, now);
    }
    uint32_t played(uint64_t now) override {
        inner_->played(now);
        return pacing_.played(now);
    }

private:
    std::unique_ptr<Output> inner_;
    PacingOutput pacing_;
};

// Another output's frames also written to a WAV file (32-bit floats, the six
// channels interleaved, 48 kHz), its sizes brought up to date once a second
// of sound so a run cut short leaves a file that plays: XF_AUDIO_WAV
class WavOutput final : public Output {
public:
    WavOutput(std::unique_ptr<Output> inner, std::FILE *file) : inner_(std::move(inner)), file_(file) {
        write_header();
        interleaved_.resize(FRAME_SAMPLES);
    }
    ~WavOutput() override {
        write_header();
        std::fclose(file_);
    }
    void submit(const float *frame, uint64_t now) override {
        for (uint32_t ch = 0; ch < CHANNELS; ch++)
            for (uint32_t i = 0; i < CHANNEL_SAMPLES; i++)
                interleaved_[i * CHANNELS + ch] = frame[ch * CHANNEL_SAMPLES + i];
        std::fwrite(interleaved_.data(), sizeof(float), interleaved_.size(), file_);
        bytes_ += uint32_t(interleaved_.size() * sizeof(float));
        if (++frames_ % (FREQUENCY / CHANNEL_SAMPLES) == 0) write_header();
        inner_->submit(frame, now);
    }
    uint32_t played(uint64_t now) override { return inner_->played(now); }

private:
    void write_header() {
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, file_); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, file_); };
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, file_);
        u32(36 + bytes_);
        std::fwrite("WAVEfmt ", 1, 8, file_);
        u32(16), u16(3 /* IEEE float */), u16(CHANNELS), u32(FREQUENCY), u32(FREQUENCY * CHANNELS * 4);
        u16(CHANNELS * 4), u16(32);
        std::fwrite("data", 1, 4, file_);
        u32(bytes_);
        std::fseek(file_, 0, SEEK_END);
        std::fflush(file_);
    }

    std::unique_ptr<Output> inner_;
    std::FILE *file_;
    std::vector<float> interleaved_;
    uint32_t bytes_ = 0, frames_ = 0;
};

} // namespace

std::unique_ptr<Output> make_pacing_output() { return std::make_unique<PacingOutput>(); }

std::unique_ptr<Output> make_wav_output(std::unique_ptr<Output> inner, const std::string &path) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "audio: cannot write %s\n", path.c_str());
        return inner;
    }
    std::fprintf(stderr, "audio: writing a client's frames to %s\n", path.c_str());
    return std::make_unique<WavOutput>(std::move(inner), file);
}

} // namespace xf::audio

namespace xf::kernel {
namespace {

constexpr uint32_t STACK = 128 * 1024; // Xenia's XHostThreads'
constexpr uint64_t FRAME_THIRDS = audio::FRAME_THIRDS;

// HRESULTs (Xenia's xbox.h)
constexpr uint32_t X_E_INVALIDARG = 0x80070057, X_E_OUTOFMEMORY = 0x80070008;

void set_event(Objects &o, Event &e, int32_t state) {
    std::lock_guard<std::mutex> l(o.lock);
    e.set_signal_state(state);
    if (state) o.signaled(e);
}

} // namespace

void set_audio_output(Kernel &k, std::function<std::unique_ptr<audio::Output>()> make) {
    k.state<Audio>().make_output = std::move(make);
}

// ---- the clients (AudioSystem) ----

void Audio::setup(Kernel &kernel) {
    k = &kernel;
    if (!make_output) make_output = audio::make_pacing_output;
    frame_.resize(audio::Output::FRAME_SAMPLES);
}

void Audio::start() {
    std::call_once(started_, [this] {
        Objects &o = objects(*k);
        work_ = o.create<Event>(GuestType::EventSynchronization, header::SIZE);
        idle_ = o.create<Event>(GuestType::EventNotification, header::SIZE);
        uint32_t status = X_STATUS_NO_MEMORY;
        if (work_ && idle_) {
            set_event(o, *idle_, 1);
            // a kernel thread in the system process, as Xenia's "Audio Worker"
            Thread::Params p;
            p.stack_size = STACK;
            p.process = system_process(*k);
            p.host = [](Thread &self, void *a) { static_cast<Audio *>(a)->worker(self); };
            p.host_arg = this;
            p.name = "Audio Worker";
            thread_ = o.create_thread(p, status);
        }
        if (!thread_) {
            std::fprintf(stderr, "audio: the worker was not made (status 0x%08X)\n", status);
            return;
        }
        running_ = true;
        started_thread_ = o.scheduler->start(*thread_);
        if (!started_thread_) std::fprintf(stderr, "audio: no host thread for the worker\n");
    });
}

void Audio::stop() {
    if (!k) return;
    Objects &o = objects(*k);
    if (running_.exchange(false)) {
        set_event(o, *work_, 1);
        if (started_thread_) o.scheduler->join(*thread_);
    }
    {
        std::lock_guard<std::mutex> l(mutex_);
        for (Client &c : clients_) {
            if (c.wrapped_arg) k->memory().system_free(c.wrapped_arg);
            c = Client{};
        }
        std::lock_guard<std::mutex> ol(o.lock);
        for (Object *object : {static_cast<Object *>(thread_.get()), static_cast<Object *>(work_.get()),
                               static_cast<Object *>(idle_.get())})
            if (object && object->handle()) o.release_handle(object->handle());
    }
    thread_ = {}, work_ = {}, idle_ = {};
}

uint32_t Audio::register_client(uint32_t callback, uint32_t callback_arg, uint32_t &index) {
    std::lock_guard<std::mutex> l(mutex_);
    auto free = std::find_if(clients_.begin(), clients_.end(), [](const Client &c) { return !c.in_use; });
    if (free == clients_.end()) return X_E_OUTOFMEMORY;
    uint32_t wrapped = k->memory().system_alloc(4);
    if (!wrapped) return X_E_OUTOFMEMORY;
    k->store32(wrapped, callback_arg);
    Client &c = *free;
    c.output = make_output();
    if (c.output && k->deterministic()) c.output = std::make_unique<VirtualClockOutput>(std::move(c.output));
    // XF_AUDIO_WAV=<prefix>: the client's frames also to <prefix>.client<N>.wav
    if (const char *wav = std::getenv("XF_AUDIO_WAV"); wav && *wav && c.output) {
        const std::string path = std::string(wav) + ".client" + std::to_string(free - clients_.begin()) + ".wav";
        c.output = audio::make_wav_output(std::move(c.output), path);
    }
    c.slots = QUEUED_FRAMES;
    c.next_pump = 0;
    c.callback = callback, c.callback_arg = callback_arg, c.wrapped_arg = wrapped;
    c.in_use = true;
    index = uint32_t(free - clients_.begin());
    registered++;
    // the worker looks again, and starts pacing this client at once
    if (work_) set_event(objects(*k), *work_, 1);
    return X_STATUS_SUCCESS;
}

bool Audio::valid_client(uint32_t driver) {
    std::lock_guard<std::mutex> l(mutex_);
    uint32_t index = driver & 0xFFFF;
    return (driver & 0xFFFF0000) == DRIVER_TAG && index < MAX_CLIENTS && clients_[index].in_use;
}

void Audio::unregister_client(uint32_t index) {
    Objects &o = objects(*k);
    Thread *self = current_thread();
    bool in_callback = false;
    uint32_t wrapped = 0;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (index >= MAX_CLIENTS || !clients_[index].in_use) return;
        Client &c = clients_[index];
        wrapped = c.wrapped_arg;
        c = Client{};
        in_callback = calling_ == int32_t(index);
        if (in_callback && self == thread_.get()) {
            // the callback unregisters its own client: its argument is freed
            // once it returns
            pending_free_.push_back(wrapped);
            wrapped = 0, in_callback = false;
        }
    }
    if (work_) set_event(o, *work_, 1);
    // a callback in progress ends before the client is gone (Xenia: the
    // client's lock)
    if (in_callback && self && idle_) {
        Dispatcher *const idle[] = {idle_.get()};
        o.wait(*self, idle, false, false, Scheduler::NEVER);
    }
    if (wrapped) k->memory().system_free(wrapped);
}

void Audio::submit_frame(uint32_t index, uint32_t samples) {
    std::lock_guard<std::mutex> l(mutex_);
    if (index >= MAX_CLIENTS || !clients_[index].in_use || !clients_[index].output) {
        // a client gone: the frame goes nowhere (Xenia logs it)
        submitted_to_none++;
        return;
    }
    if (samples) {
        const uint8_t *p = k->host(samples);
        for (uint32_t i = 0; i < audio::Output::FRAME_SAMPLES; i++) {
            uint32_t v;
            std::memcpy(&v, p + 4 * i, 4);
            frame_[i] = std::bit_cast<float>(__builtin_bswap32(v));
        }
    } else {
        std::fill(frame_.begin(), frame_.end(), 0.0f); // no samples: silence (Xenia's drivers read none)
    }
    clients_[index].output->submit(frame_.data(), objects(*k).now());
    frames++;
}

void Audio::set_idle(bool idle) { set_event(objects(*k), *idle_, idle ? 1 : 0); }

// ---- the worker (WorkerThreadMain) ----

void Audio::worker(Thread &self) {
    Objects &o = objects(*k);
    Dispatcher *const work[] = {work_.get()};
    while (running_.load()) {
        uint64_t now = o.now();
        int32_t index = -1;
        uint64_t earliest = UINT64_MAX;
        {
            std::lock_guard<std::mutex> l(mutex_);
            uint32_t ms = k->uptime_ms();
            if (ms >= next_sample_ms_) {
                frame_samples_.push_back({ms, frames.load()});
                next_sample_ms_ = ms - ms % 1000 + 1000;
            }
            for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
                if (!clients_[i].in_use || clients_[i].next_pump >= earliest) continue;
                earliest = clients_[i].next_pump;
                index = int32_t(i);
            }
        }
        // no client: parked until one registers or the kernel stops
        if (index < 0) {
            o.wait(self, work, false, false, Scheduler::NEVER);
            continue;
        }
        // until the client is due; a client coming or going looks again
        if (earliest > now * 3 && o.wait(self, work, false, false, (earliest + 2) / 3) != X_STATUS_TIMEOUT) continue;
        if (!running_.load()) break;

        uint32_t callback = 0, arg = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            Client &c = clients_[index];
            if (!c.in_use) continue;
            c.next_pump = std::max(earliest, now * 3) + FRAME_THIRDS;
            // the frames its output played since free their slots; the
            // callback only with one free (the frame it will submit)
            c.slots += c.output->played(o.now());
            if (!c.slots) {
                skipped++;
                continue;
            }
            c.slots--;
            callback = c.callback, arg = c.wrapped_arg;
            calling_ = index;
            set_idle(false);
        }
        o.call(*self.context, callback, {arg});
        callbacks++;
        std::vector<uint32_t> to_free;
        {
            std::lock_guard<std::mutex> l(mutex_);
            calling_ = -1;
            set_idle(true);
            to_free.swap(pending_free_);
        }
        for (uint32_t a : to_free) k->memory().system_free(a);
    }
}

Audio &audio(Kernel &k) {
    Audio &a = k.state<Audio>();
    a.start();
    return a;
}

// ---- the report ----

std::string audio_report(Kernel &k) {
    Audio &a = k.state<Audio>();
    Xma &x = k.state<Xma>();
    std::string out;
    char line[320];
    if (a.registered.load()) {
        std::vector<std::pair<uint32_t, uint64_t>> samples = a.frame_samples();
        std::snprintf(line, sizeof(line),
                      "audio: %llu clients registered, %llu callbacks, %llu frames submitted, %llu callbacks skipped "
                      "(no free slot)\n",
                      (unsigned long long)a.registered.load(), (unsigned long long)a.callbacks.load(),
                      (unsigned long long)a.frames.load(), (unsigned long long)a.skipped.load());
        out += line;
        // frames per second: since the first second with one, and over the
        // last five seconds sampled
        auto first = std::find_if(samples.begin(), samples.end(), [](const auto &s) { return s.second > 0; });
        if (first != samples.end() && samples.back().first > first->first) {
            const auto &last = samples.back();
            const auto &recent = samples[samples.size() > 5 ? samples.size() - 6 : 0];
            double overall = double(last.second - first->second) * 1000.0 / (last.first - first->first);
            double lately = last.first > recent.first
                                ? double(last.second - recent.second) * 1000.0 / (last.first - recent.first)
                                : 0.0;
            std::snprintf(line, sizeof(line),
                          "audio: %.1f frames per second since the first (at %.1f s), %.1f over the last %.0f s "
                          "(the output plays 187.5)\n",
                          overall, first->first / 1000.0, lately, (last.first - recent.first) / 1000.0);
            out += line;
        }
    }
    if (x.created.load() || x.kicks.load()) {
        std::snprintf(line, sizeof(line),
                      "xma: %llu contexts created (%llu released), %u kicked, %llu kicks, %llu context runs (%llu "
                      "starved of input); %llu input buffer swaps, %llu loops, %llu packets skipped\n",
                      (unsigned long long)x.created.load(), (unsigned long long)x.released.load(),
                      x.contexts_kicked.load(), (unsigned long long)x.kicks.load(), (unsigned long long)x.works.load(),
                      (unsigned long long)x.starved.load(), (unsigned long long)x.swaps.load(),
                      (unsigned long long)x.loops.load(), (unsigned long long)x.packets_skipped.load());
        out += line;
        std::snprintf(line, sizeof(line),
                      "xma: %llu frames decoded, %llu with sound (the loudest sample %u of 32768), %llu that decoded "
                      "to nothing; %llu bad input, %llu runs cut short, %u FFmpeg errors\n",
                      (unsigned long long)x.frames.load(), (unsigned long long)x.frames_with_sound.load(),
                      x.peak.load(), (unsigned long long)x.frames_failed.load(), (unsigned long long)x.errors.load(),
                      (unsigned long long)x.stalls.load(), audio::ffmpeg_errors());
        out += line;
    }
    return out;
}

void setup_audio(Kernel &k) {
    k.state<Audio>().setup(k);
    k.state<Xma>().setup(k);
}

void shutdown_audio(Kernel &k) {
    k.state<Audio>().stop();
    k.state<Xma>().stop();
}

// ---- the exports (xboxkrnl_audio.cc) ----

// the console's audio flags (XCONFIG_USER_AUDIO_FLAGS), as the settings hold
// them
XF_EXPORT(xboxkrnl, XAudioGetSpeakerConfig) {
    static const ExportFn get = find_export_fn("xboxkrnl", "ExGetXConfigSetting");
    if (uint32_t config_ptr = arg(k, c, 0); config_ptr && get) {
        ppc::Context x{};
        k.attach(x);
        x.r[3] = 3, x.r[4] = 0x0B, x.r[5] = config_ptr, x.r[6] = 4, x.r[7] = 0;
        get(k, x);
    }
    result(c, X_STATUS_SUCCESS);
}

// (driver, out): no voice category's volume changed
XF_EXPORT(xboxkrnl, XAudioGetVoiceCategoryVolumeChangeMask) {
    objects(k).scheduler->yield(); // Xenia's MaybeYield: the games poll it
    // (the console's: a driver that is not registered, or no output, is E_INVALIDARG)
    if (!audio(k).valid_client(arg(k, c, 0)) || !arg(k, c, 1)) return result(c, X_E_INVALIDARG);
    k.store32(arg(k, c, 1), 0);
    result(c, X_STATUS_SUCCESS);
}

// (category, out): full volume; the console's kernel refuses a category of 32 or more (its oracle took 0 to 31)
XF_EXPORT(xboxkrnl, XAudioGetVoiceCategoryVolume) {
    if (arg(k, c, 0) >= 32 || !arg(k, c, 1)) return result(c, X_E_INVALIDARG);
    k.store32(arg(k, c, 1), std::bit_cast<uint32_t>(1.0f));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XAudioEnableDucker) { result(c, X_STATUS_SUCCESS); }

// The ducker's settings: no arguments, a float in f1 (the games' XAudio2
// stores or compares f1 right after each call; Xenia has none of them). The
// game's ducker effect takes the level as a gain (1/level, at most +48 dB),
// ducks while its input's mean square reaches the threshold, and divides by
// the times: the ducker off is no attenuation, a threshold only full scale
// reaches, and times that divide.
XF_EXPORT(xboxkrnl, XAudioGetDuckerLevel) { c.f[1] = 1.0; }
XF_EXPORT(xboxkrnl, XAudioGetDuckerThreshold) { c.f[1] = 1.0; }
XF_EXPORT(xboxkrnl, XAudioGetDuckerAttackTime) { c.f[1] = 1.0; }
XF_EXPORT(xboxkrnl, XAudioGetDuckerReleaseTime) { c.f[1] = 1.0; }
XF_EXPORT(xboxkrnl, XAudioGetDuckerHoldTime) { c.f[1] = 1.0; }

// (callback_ptr: {callback, its argument}, driver_ptr): the client's driver
// handle, 'AU' and its index
XF_EXPORT(xboxkrnl, XAudioRegisterRenderDriverClient) {
    uint32_t callback_ptr = arg(k, c, 0), driver_ptr = arg(k, c, 1);
    if (!callback_ptr) return result(c, X_E_INVALIDARG);
    uint32_t callback = k.load32(callback_ptr);
    if (!callback) return result(c, X_E_INVALIDARG);
    uint32_t index = 0;
    uint32_t status = audio(k).register_client(callback, k.load32(callback_ptr + 4), index);
    if (status != X_STATUS_SUCCESS) return result(c, status);
    k.store32(driver_ptr, Audio::DRIVER_TAG | (index & 0xFFFF));
    result(c, X_STATUS_SUCCESS);
}

// A driver handle that is not a registered client is E_INVALIDARG in the three calls that take one, as on the console's
// kernel (Xenia's drivers took any index and said success).
XF_EXPORT(xboxkrnl, XAudioUnregisterRenderDriverClient) {
    if (!audio(k).valid_client(arg(k, c, 0))) return result(c, X_E_INVALIDARG);
    audio(k).unregister_client(arg(k, c, 0) & 0xFFFF);
    result(c, X_STATUS_SUCCESS);
}

// (driver, samples): 6 channels of 256 big-endian floats, one after another
XF_EXPORT(xboxkrnl, XAudioSubmitRenderDriverFrame) {
    if (!audio(k).valid_client(arg(k, c, 0))) return result(c, X_E_INVALIDARG);
    audio(k).submit_frame(arg(k, c, 0) & 0xFFFF, arg(k, c, 1));
    result(c, X_STATUS_SUCCESS);
}

} // namespace xf::kernel
