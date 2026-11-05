// The XMA decoder against Xenia's on what games gave it: every recording
// under XF_XMA_TRACES (boot or the player with XF_XMA_RECORD=<file.xmar>;
// include/xenoforge/xma_recording.h) is walked again through the kernel's XMA
// decoder (src/kernel/xma.cpp, with FFmpeg's decoder when built with XF_XMA),
// each run's context data and its input and output buffers put back where the
// game had them, then a kick from this host thread. Each run's result is
// compared with:
//
// - the recording itself (the live run): the fields the decoder owns (the
//   rest the game may have changed while the run went on), and the output;
// - Xenia's, where <name>.xenia.xmar is beside it (tools/xenia_oracle's
//   xma_oracle walks the recording through Xenia's XmaContextNew): all 64
//   bytes of the context and every byte of the output buffer.
//
// XF_XMA_WAV=<dir> writes each context's output, ours and Xenia's, as WAV
// files there. Recordings are game data: they stay outside the repository
// (../traces/xma), and without XF_XMA_TRACES there is nothing to run.

#include "check.h"

#include "audio.h"
#include "xenoforge/audio.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/xma_recording.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::kernel;
namespace rec = xf::xma_recording;
namespace fs = std::filesystem;

namespace {

struct Result {
    uint8_t after[64];
    std::vector<uint8_t> output;
};

// a context's PCM as its runs wrote it (16-bit big-endian, channels
// interleaved), for the WAV files
struct Stream {
    uint32_t rate = 0, channels = 0;
    std::vector<uint8_t> pcm;
};

constexpr uint32_t RATES[4] = {24000, 32000, 44100, 48000};

// the blocks a run wrote to its output ring: from the write offset before to
// the one after (a run that was starved of input wrote none)
void append_written(Stream &s, const uint8_t *before, const uint8_t *after, const std::vector<uint8_t> &output) {
    XmaContextData b(before), a(after);
    if (!b.any_input_buffer_valid() || !b.output_buffer_block_count) return;
    const uint32_t blocks = b.output_buffer_block_count;
    if (output.size() < blocks * 256u) return;
    s.rate = RATES[std::min<uint32_t>(b.sample_rate, 3)];
    s.channels = b.is_stereo ? 2 : 1;
    for (uint32_t i = b.output_buffer_write_offset % blocks; i != a.output_buffer_write_offset % blocks;
         i = (i + 1) % blocks)
        s.pcm.insert(s.pcm.end(), output.begin() + i * 256, output.begin() + (i + 1) * 256);
}

bool write_wav(const fs::path &path, const Stream &s) {
    if (s.pcm.empty() || !s.channels) return false;
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t size = uint32_t(s.pcm.size());
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + size);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(uint16_t(s.channels)), u32(s.rate), u32(s.rate * s.channels * 2);
    u16(uint16_t(s.channels * 2)), u16(16);
    std::fwrite("data", 1, 4, f);
    u32(size);
    std::vector<uint8_t> le(s.pcm.size());
    for (size_t i = 0; i + 1 < s.pcm.size(); i += 2) le[i] = s.pcm[i + 1], le[i + 1] = s.pcm[i];
    std::fwrite(le.data(), 1, le.size(), f);
    std::fclose(f);
    return true;
}

// the fields of a context the decoder writes back (StoreContextMerged)
bool decoder_fields_equal(const uint8_t *x, const uint8_t *y) {
    XmaContextData a(x), b(y);
    return a.loop_count == b.loop_count && a.output_buffer_write_offset == b.output_buffer_write_offset &&
           a.input_buffer_0_valid == b.input_buffer_0_valid && a.input_buffer_1_valid == b.input_buffer_1_valid &&
           a.output_buffer_valid == b.output_buffer_valid && a.input_buffer_read_offset == b.input_buffer_read_offset &&
           a.error_status == b.error_status && a.current_buffer == b.current_buffer &&
           a.output_buffer_read_offset == b.output_buffer_read_offset;
}

// the largest difference between two outputs' 16-bit samples
int max_sample_difference(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    int worst = 0;
    for (size_t i = 0; i + 1 < std::min(a.size(), b.size()); i += 2) {
        int x = int16_t(uint16_t(a[i] << 8 | a[i + 1])), y = int16_t(uint16_t(b[i] << 8 | b[i + 1]));
        worst = std::max(worst, std::abs(x - y));
    }
    return worst;
}

std::string describe(const uint8_t *x) {
    XmaContextData d(x);
    char s[200];
    std::snprintf(s, sizeof(s), "read offset %u, current %u, valid %u%u, out w%u r%u valid %u, loop %u, error %u",
                  d.input_buffer_read_offset, d.current_buffer, d.input_buffer_0_valid, d.input_buffer_1_valid,
                  d.output_buffer_write_offset, d.output_buffer_read_offset, d.output_buffer_valid, d.loop_count,
                  d.error_status);
    return s;
}

// The recording's runs through a kernel's XMA decoder: ours.
bool replay(const rec::Recording &recording, std::vector<Result> &results) {
    std::string error;
    std::unique_ptr<Memory> memory = Memory::create(error);
    if (!memory) {
        std::printf("memory: %s\n", error.c_str());
        return false;
    }
    Kernel k(*memory);
    Xma &x = k.state<Xma>();
    // every context allocated, as the game's were
    for (uint32_t i = 0; i < Xma::CONTEXT_COUNT; i++) x.allocate_context();
    // physical memory committed, 64 KB at a time, where the buffers lie
    Heap *window = memory->heap_by_type(true, 64 * 1024);
    std::set<uint32_t> committed;
    auto commit = [&](uint32_t physical, uint32_t size) {
        if (!size) return true;
        physical &= 0x1FFFFFFF;
        for (uint32_t page = physical & ~0xFFFFu; page < physical + size; page += 0x10000) {
            if (!committed.insert(page).second) continue;
            if (!window->alloc_fixed(window->heap_base() + page, 0x10000, 0x10000, ALLOCATE_RESERVE | ALLOCATE_COMMIT,
                                     PROTECT_READ_WRITE)) {
                std::printf("physical page 0x%08X not committed\n", page);
                return false;
            }
        }
        return true;
    };
    auto reg = [&](uint32_t base, uint32_t id) {
        x.write_register(0x7FEA0000 + (base + id / 32) * 4, __builtin_bswap32(1u << (id % 32)));
    };
    for (const rec::Event &e : recording.events) {
        const uint32_t id = e.run.context;
        if (id >= Xma::CONTEXT_COUNT) continue;
        const uint32_t guest = x.context_array + id * 64;
        if (e.type == rec::CLEAR) {
            reg(Xma::CLEAR, id);
            continue;
        }
        if (e.type == rec::RELEASE) {
            x.release_context(guest);
            x.allocate_context(); // the one free: this one again
            continue;
        }
        const rec::Run &r = e.run;
        XmaContextData d(r.before);
        const uint32_t addresses[3] = {d.input_buffer_0_ptr, d.input_buffer_1_ptr, d.output_buffer_ptr};
        const uint32_t sizes[3] = {d.input_buffer_0_packet_count * 2048u, d.input_buffer_1_packet_count * 2048u,
                                   d.output_buffer_block_count * 256u};
        const uint64_t hashes[3] = {r.input[0], r.input[1], r.output_before};
        for (int b = 0; b < 3; b++) {
            if (!addresses[b] || !commit(addresses[b], sizes[b])) continue;
            if (const std::vector<uint8_t> *bytes = hashes[b] ? recording.buffer(hashes[b]) : nullptr)
                std::memcpy(memory->translate_physical(addresses[b]), bytes->data(), bytes->size());
        }
        std::memcpy(k.host(guest), r.before, 64);
        reg(Xma::KICK, id); // walked here: no guest thread
        Result result;
        std::memcpy(result.after, k.host(guest), 64);
        if (addresses[2] && sizes[2]) {
            const uint8_t *out = memory->translate_physical(addresses[2]);
            result.output.assign(out, out + sizes[2]);
        }
        results.push_back(std::move(result));
    }
    // the kernel's report of the replay, indented
    std::string report = audio_report(k);
    for (size_t start = 0, end; start < report.size(); start = end + 1) {
        end = report.find('\n', start);
        if (end == std::string::npos) end = report.size();
        std::printf("  %.*s\n", int(end - start), report.c_str() + start);
    }
    shutdown_audio(k);
    return true;
}

void check_recording(const fs::path &path, const char *wav_dir) {
    rec::Recording recording;
    std::string error;
    CHECK(rec::read(path.string(), recording, error));
    if (!error.empty()) std::printf("  %s\n", error.c_str());
    std::vector<const rec::Run *> runs;
    for (const rec::Event &e : recording.events)
        if (e.type == rec::RUN) runs.push_back(&e.run);
    std::printf("%s: %zu runs, %zu buffers\n", path.filename().string().c_str(), runs.size(), recording.buffers.size());
    std::vector<Result> ours;
    CHECK(replay(recording, ours));
    if (ours.size() != runs.size()) return;
    const bool decoding = audio::make_ffmpeg_decoder() != nullptr;

    // the live run: the decoder's fields and the output
    size_t live_same = 0;
    for (size_t i = 0; i < runs.size(); i++) {
        const std::vector<uint8_t> *live = recording.buffer(runs[i]->output_after);
        bool same =
            decoder_fields_equal(ours[i].after, runs[i]->after) && (!decoding || !live || *live == ours[i].output);
        live_same += same;
    }
    std::printf("  the live run: %zu of %zu runs the same\n", live_same, runs.size());

    // Xenia's
    fs::path xenia_path = path;
    xenia_path.replace_extension(".xenia.xmar");
    std::map<uint32_t, Stream> ours_streams, xenia_streams;
    for (size_t i = 0; i < runs.size(); i++)
        append_written(ours_streams[runs[i]->context], runs[i]->before, ours[i].after, ours[i].output);
    if (fs::exists(xenia_path)) {
        rec::Recording xenia;
        CHECK(rec::read(xenia_path.string(), xenia, error));
        std::vector<const rec::Run *> xenia_runs;
        for (const rec::Event &e : xenia.events)
            if (e.type == rec::RUN) xenia_runs.push_back(&e.run);
        // Xenia's runs start from the recording's: if not, it was made from
        // another recording of the same name
        bool same_input = xenia_runs.size() == runs.size();
        for (size_t i = 0; same_input && i < runs.size(); i++)
            same_input = std::memcmp(xenia_runs[i]->before, runs[i]->before, 64) == 0 &&
                         xenia_runs[i]->input[0] == runs[i]->input[0] && xenia_runs[i]->input[1] == runs[i]->input[1];
        if (!same_input) {
            std::printf("  %s is not of this recording: run xma_oracle on it again\n",
                        xenia_path.filename().string().c_str());
            CHECK(same_input);
            return;
        }
        size_t data_same = 0, output_same = 0, shown = 0;
        int worst = 0;
        for (size_t i = 0; i < std::min(runs.size(), xenia_runs.size()); i++) {
            const rec::Run &xr = *xenia_runs[i];
            const std::vector<uint8_t> *xenia_output = xenia.buffer(xr.output_after);
            static const std::vector<uint8_t> none;
            const std::vector<uint8_t> &xo = xenia_output ? *xenia_output : none;
            append_written(xenia_streams[xr.context], xr.before, xr.after, xo);
            bool data = std::memcmp(ours[i].after, xr.after, 64) == 0;
            bool output = !decoding || xo == ours[i].output;
            data_same += data, output_same += output;
            if (!output) worst = std::max(worst, max_sample_difference(xo, ours[i].output));
            if ((!data || !output) && shown++ < 5) {
                std::printf(
                    "  run %zu (context %u) differs from Xenia's:%s%s\n    before %s\n    ours   %s\n    Xenia  %s\n",
                    i, xr.context, data ? "" : " data", output ? "" : " output", describe(runs[i]->before).c_str(),
                    describe(ours[i].after).c_str(), describe(xr.after).c_str());
            }
        }
        std::printf("  Xenia's: %zu of %zu runs leave the same context data, %zu the same output%s", data_same,
                    runs.size(), output_same, decoding ? "" : " (not compared: built without FFmpeg)");
        if (worst) std::printf(" (the largest sample difference %d)", worst);
        std::printf("\n");
        CHECK(data_same == runs.size());
        CHECK(output_same == runs.size());
    } else {
        std::printf("  no %s: not compared with Xenia\n", xenia_path.filename().string().c_str());
    }

    if (wav_dir) {
        for (auto *streams : {&ours_streams, &xenia_streams}) {
            const char *who = streams == &ours_streams ? "ours" : "xenia";
            for (const auto &[context, s] : *streams) {
                fs::path wav = fs::path(wav_dir) /
                               (path.stem().string() + ".context" + std::to_string(context) + "." + who + ".wav");
                if (write_wav(wav, s))
                    std::printf("  %s: %.1f s, %u Hz, %u channels\n", wav.string().c_str(),
                                double(s.pcm.size()) / (2.0 * s.channels * s.rate), s.rate, s.channels);
            }
        }
    }
}

} // namespace

int main() {
    // without recordings, what can be checked: FFmpeg's decoder is there when
    // built with it, and turns down a frame whose size is not its own
    if (auto decoder = audio::make_ffmpeg_decoder()) {
        const uint8_t packet[3] = {};
        uint8_t pcm[2048];
        CHECK(!decoder->decode(0, packet, sizeof(packet), 48000, 1, pcm));
        CHECK(audio::ffmpeg_errors() == 1);
    }
    const char *traces = std::getenv("XF_XMA_TRACES");
    if (!traces || !fs::is_directory(traces)) {
        std::printf("XF_XMA_TRACES not set: nothing to replay\n");
        return finish("xma_replay");
    }
    const char *wav_dir = std::getenv("XF_XMA_WAV");
    int recordings = 0;
    for (const auto &entry : fs::directory_iterator(traces)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".xmar" || name.ends_with(".xenia.xmar")) continue;
        check_recording(entry.path(), wav_dir);
        recordings++;
    }
    if (!recordings) std::printf("no recordings (*.xmar) in %s\n", traces);
    return finish("xma_replay");
}
