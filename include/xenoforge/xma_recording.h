// A recording of what XMA contexts consumed (docs/runtime.md, "Sound"): each
// run of a context the decoder made (its 64-byte XMA_CONTEXT_DATA before and
// after, its two input buffers and its output buffer before, the output buffer
// after) and the clears and releases between runs, in order. The kernel writes
// one when XF_XMA_RECORD names a file (src/kernel/xma.cpp); the replay test
// (tests/xma_replay_test.cpp) walks it again through the kernel and Xenia's
// oracle (tools/xenia_oracle/xma_oracle.cc) through Xenia's XmaContextNew, each
// writing its own runs in the same form to compare. Recordings are game data:
// they stay outside the repository (e.g. ../traces/xma).
//
// The file: "XFXMAREC", a 32-bit version, a 32-bit zero; then records, each a
// type byte and its fields, little-endian:
//   'B' a buffer: its 64-bit hash, 32-bit size, the bytes (once per hash)
//   'R' a run: 32-bit context index, the data before (64 bytes, as in guest
//       memory), the hashes of input buffer 0, input buffer 1 and the output
//       buffer before (0: not recorded), the data after (64 bytes), the
//       output buffer's hash after
//   'C' a clear, 'X' a release: the 32-bit context index
// Header-only and plain C++, so the oracle (MSVC, against Xenia) uses it too.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xf::xma_recording {

constexpr char MAGIC[8] = {'X', 'F', 'X', 'M', 'A', 'R', 'E', 'C'};
constexpr uint32_t VERSION = 1;
enum : uint8_t { BUFFER = 'B', RUN = 'R', CLEAR = 'C', RELEASE = 'X' };

struct Run {
    uint32_t context = 0;
    uint8_t before[64] = {}, after[64] = {};
    uint64_t input[2] = {}, output_before = 0, output_after = 0;
};

struct Event {
    uint8_t type = RUN; // RUN, CLEAR or RELEASE
    Run run;            // its context for all three
};

struct Recording {
    std::unordered_map<uint64_t, std::vector<uint8_t>> buffers;
    std::vector<Event> events;

    const std::vector<uint8_t> *buffer(uint64_t hash) const {
        auto it = buffers.find(hash);
        return it == buffers.end() ? nullptr : &it->second;
    }
};

// FNV-1a over the bytes and the size; never 0 (that is "not recorded")
inline uint64_t hash(const uint8_t *data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 0x100000001B3ull;
    h = (h ^ size) * 0x100000001B3ull;
    return h ? h : 1;
}

// a 32-bit big-endian word of a context's data, as the guest stores it
inline uint32_t word(const uint8_t *data, int i) {
    return uint32_t(data[i * 4]) << 24 | uint32_t(data[i * 4 + 1]) << 16 | uint32_t(data[i * 4 + 2]) << 8 |
           data[i * 4 + 3];
}

class Writer {
public:
    ~Writer() { close(); }
    bool open(const std::string &path) {
        close();
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) return false;
        uint32_t header[2] = {VERSION, 0};
        std::fwrite(MAGIC, 1, 8, file_);
        std::fwrite(header, 4, 2, file_);
        return true;
    }
    void close() {
        if (file_) std::fclose(file_);
        file_ = nullptr;
        written_.clear();
    }
    bool is_open() const { return file_ != nullptr; }

    // the buffer's hash, its bytes written the first time they are seen
    uint64_t buffer(const uint8_t *data, uint32_t size) {
        if (!file_ || !data || !size) return 0;
        uint64_t h = hash(data, size);
        if (written_.insert(h).second) {
            put8(BUFFER);
            put(&h, 8);
            put(&size, 4);
            put(data, size);
        }
        return h;
    }
    void run(const Run &r) {
        if (!file_) return;
        put8(RUN);
        put(&r.context, 4);
        put(r.before, 64);
        put(r.input, 16);
        put(&r.output_before, 8);
        put(r.after, 64);
        put(&r.output_after, 8);
    }
    void event(uint8_t type, uint32_t context) {
        if (!file_) return;
        put8(type);
        put(&context, 4);
    }
    void flush() {
        if (file_) std::fflush(file_);
    }

private:
    void put8(uint8_t v) { put(&v, 1); }
    void put(const void *p, size_t n) { std::fwrite(p, 1, n, file_); }

    std::FILE *file_ = nullptr;
    std::unordered_set<uint64_t> written_;
};

inline bool read(const std::string &path, Recording &out, std::string &error) {
    out = {};
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    auto get = [&](void *p, size_t n) { return std::fread(p, 1, n, f) == n; };
    char magic[8];
    uint32_t header[2];
    if (!get(magic, 8) || std::memcmp(magic, MAGIC, 8) || !get(header, 8) || header[0] != VERSION) {
        std::fclose(f);
        error = path + " is not an XMA recording of version " + std::to_string(VERSION);
        return false;
    }
    bool ok = true;
    for (uint8_t type; ok && get(&type, 1);) {
        Event e;
        e.type = type;
        switch (type) {
        case BUFFER: {
            uint64_t h;
            uint32_t size;
            std::vector<uint8_t> bytes;
            ok = get(&h, 8) && get(&size, 4);
            if (ok) bytes.resize(size), ok = get(bytes.data(), size);
            if (ok) out.buffers.emplace(h, std::move(bytes));
            continue;
        }
        case RUN:
            ok = get(&e.run.context, 4) && get(e.run.before, 64) && get(e.run.input, 16) &&
                 get(&e.run.output_before, 8) && get(e.run.after, 64) && get(&e.run.output_after, 8);
            break;
        case CLEAR:
        case RELEASE: ok = get(&e.run.context, 4); break;
        default: ok = false;
        }
        if (ok) out.events.push_back(e);
    }
    std::fclose(f);
    if (!ok) error = path + ": a record cut short or of an unknown type (kept what came before)";
    return true;
}

} // namespace xf::xma_recording
