/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The trace writer (gpu_trace_writer.h). Translated from Xenia (BSD; see
// LICENSE-xenia): src/xenia/gpu/trace_writer.cc (each command's layout, the
// compression of what is longer than 1 KB), trace_protocol.h (the format,
// version 1), command_processor.cc's InitializeTrace (the register file and
// the gamma ramps first) and pm4_command_processor_implement.h (where each
// command is written: a packet's start before it runs, with 2 words for an
// indirect buffer's so that a player does not run the buffer twice; its end
// after; the swap event after the swap's packet, then the next frame
// opened); checked against them. Where it differs:
//
// - Memory the backend reads is recorded where it differs from what the
//   trace gave already (a shadow of it, by 4 KB page), not where a page
//   watch saw the CPU write: XenoForge has no page watches. What the GPU
//   wrote during the frame is known as given (the replay's resolves make
//   it), as Xenia's shared memory keeps GPU-written pages valid.
// - At a trace's start the backend's work is finished first, so guest
//   memory holds what earlier resolves wrote and the frame's reads take it
//   from there (Xenia downloads the GPU-written ranges into the trace).
// - EDRAM's snapshot is written by any backend that keeps one (Xenia's
//   Direct3D 12 backend writes it, its Vulkan backend does not).
// - Frame 0 can be recorded too: from the first primary buffer.
// - The commit field holds "xenoforge".

#include "xenoforge/gpu_trace_writer.h"

#include "xenoforge/snappy.h"

#include <algorithm>
#include <cstring>

namespace xf::gpu {
namespace {

constexpr uint32_t FORMAT_VERSION = 1;
// TraceCommandType
enum : uint32_t {
    PRIMARY_BUFFER_START,
    PRIMARY_BUFFER_END,
    INDIRECT_BUFFER_START,
    INDIRECT_BUFFER_END,
    PACKET_START,
    PACKET_END,
    MEMORY_READ,
    MEMORY_WRITE,
    EDRAM_SNAPSHOT,
    EVENT,
    REGISTERS,
    GAMMA_RAMP,
};
constexpr uint32_t ENCODING_NONE = 0, ENCODING_SNAPPY = 1;
constexpr size_t COMPRESSION_THRESHOLD = 1024; // memory longer than this is compressed
constexpr uint32_t PHYSICAL_SIZE = 0x20000000;
constexpr uint32_t PAGE = 4096;

void set_u32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }

} // namespace

TraceWriter::TraceWriter(CommandProcessor &processor, Sink *next, TraceSource *source, TraceOptions options)
    : processor_(processor), next_(next), source_(source), options_(std::move(options)),
      memory_(processor.memory().translate_physical(0)) {
    if (source_) source_->set_memory_trace(this);
}

TraceWriter::~TraceWriter() {
    if (source_) source_->set_memory_trace(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;
    // a frame that never reached its swap: kept aside, not as a trace
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::rename(path_, std::filesystem::path(path_).concat(".partial"), ec);
}

std::vector<std::filesystem::path> TraceWriter::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

std::vector<std::string> TraceWriter::errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
}

// ---- the file ----

void TraceWriter::put(const void *data, size_t bytes) {
    if (file_ && bytes && std::fwrite(data, 1, bytes, file_) != bytes) {
        errors_.push_back("could not write " + path_.string());
        std::fclose(file_);
        file_ = nullptr;
    }
}

void TraceWriter::put_encoded(std::span<const uint8_t> head, size_t length_at, std::span<const uint8_t> data) {
    std::vector<uint8_t> h(head.begin(), head.end());
    std::vector<uint8_t> packed;
    // the encoding is the word before the encoded length in every such command
    bool compress = options_.compress && data.size() > COMPRESSION_THRESHOLD;
    if (compress) {
        packed = snappy_compress(data);
        data = packed;
    }
    set_u32(h.data() + length_at - 4, compress ? ENCODING_SNAPPY : ENCODING_NONE);
    set_u32(h.data() + length_at, uint32_t(data.size()));
    put(h.data(), h.size());
    put(data.data(), data.size());
}

// MemoryCommand: type, base_ptr, encoding_format, encoded_length,
// decoded_length; the range lies within physical memory
void TraceWriter::put_memory(uint32_t command, uint32_t address, uint32_t bytes) {
    uint8_t head[20] = {};
    set_u32(head, command);
    set_u32(head + 4, address);
    set_u32(head + 16, bytes);
    put_encoded(head, 12, {memory_ + address, bytes});
}

void TraceWriter::open(uint64_t frame) {
    // the backend's work finished (what it calls back with comes before the
    // trace) and EDRAM read, without the lock: the backend reports to it
    std::vector<uint8_t> edram;
    bool have_edram = false;
    if (source_) {
        source_->settle();
        have_edram = source_->snapshot_edram(edram);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    char name[64];
    std::snprintf(name, sizeof(name), "%08X_%llu.xtr", options_.title_id, (unsigned long long)frame);
    path_ = options_.directory / name;
    std::error_code ec;
    std::filesystem::create_directories(options_.directory, ec);
    file_ = std::fopen(path_.string().c_str(), "wb");
    if (!file_) {
        errors_.push_back("could not open " + path_.string());
        return;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    pages_.clear();

    // TraceHeader: version, the commit (40 characters), the title id
    uint8_t header[48] = {};
    set_u32(header, FORMAT_VERSION);
    std::memcpy(header + 4, "xenoforge", 9);
    set_u32(header + 44, options_.title_id);
    put(header, sizeof(header));

    // InitializeTrace: every register, loaded straight into the register
    // file on playback (RegistersCommand: type, first, count,
    // execute_callbacks as a padded bool, encoding, encoded length)
    {
        const uint32_t *values = processor_.registers().data();
        uint8_t head[24] = {};
        set_u32(head, REGISTERS);
        set_u32(head + 4, 0);
        set_u32(head + 8, RegisterFile::COUNT);
        put_encoded(head, 20, {reinterpret_cast<const uint8_t *>(values), RegisterFile::COUNT * 4});
    }
    // the gamma ramps (GammaRampCommand: type, the next component as a
    // padded byte, encoding, encoded length), the table then the PWL ramp
    {
        std::vector<uint32_t> ramps(processor_.gamma_ramp_table().begin(), processor_.gamma_ramp_table().end());
        ramps.insert(ramps.end(), processor_.gamma_ramp_pwl().begin(), processor_.gamma_ramp_pwl().end());
        uint8_t head[16] = {};
        set_u32(head, GAMMA_RAMP);
        head[4] = uint8_t(processor_.gamma_ramp_rw_component());
        put_encoded(head, 12, {reinterpret_cast<const uint8_t *>(ramps.data()), ramps.size() * 4});
    }
    // EDRAM (EdramSnapshotCommand: type, encoding, encoded length), always
    // compressed as Xenia's
    if (have_edram) {
        uint8_t head[12] = {};
        set_u32(head, EDRAM_SNAPSHOT);
        bool compress = options_.compress;
        std::vector<uint8_t> packed;
        if (compress) packed = snappy_compress(edram);
        const std::vector<uint8_t> &data = compress ? packed : edram;
        set_u32(head + 4, compress ? ENCODING_SNAPPY : ENCODING_NONE);
        set_u32(head + 8, uint32_t(data.size()));
        put(head, sizeof(head));
        put(data.data(), data.size());
    }
}

void TraceWriter::close() {
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
    finished_.push_back(path_);
    pages_.clear();
}

// ---- the shadow of what the trace gave ----

void TraceWriter::learn(uint32_t address, uint32_t bytes) {
    uint64_t end = uint64_t(address) + bytes;
    for (uint64_t at = address; at < end;) {
        uint32_t page = uint32_t(at / PAGE), lo = uint32_t(at % PAGE);
        uint32_t hi = uint32_t(std::min<uint64_t>(PAGE, lo + (end - at)));
        std::unique_ptr<Page> &p = pages_[page];
        if (!p) {
            p = std::make_unique<Page>();
            std::memset(p->known, 0, sizeof(p->known));
        }
        std::memcpy(p->bytes + lo, memory_ + at, hi - lo);
        for (uint32_t b = lo; b < hi;) {
            uint32_t word = b / 64, first = b % 64, n = std::min(64 - first, hi - b);
            p->known[word] |= (n == 64 ? ~0ull : ((1ull << n) - 1)) << first;
            b += n;
        }
        at += hi - lo;
    }
}

void TraceWriter::read_locked(uint32_t address, uint32_t bytes) {
    if (!file_ || !bytes) return;
    address &= PHYSICAL_SIZE - 1;
    if (uint64_t(address) + bytes > PHYSICAL_SIZE) {
        // past the end of physical memory it wraps to the start, as the
        // address mask wraps it
        uint32_t first = PHYSICAL_SIZE - address;
        read_locked(address, first);
        read_locked(0, std::min<uint32_t>(bytes - first, PHYSICAL_SIZE));
        return;
    }
    // runs of page parts that differ from the shadow (or that it lacks)
    uint64_t end = uint64_t(address) + bytes, run_start = 0, run_end = 0;
    auto flush = [&] {
        if (run_end <= run_start) return;
        put_memory(MEMORY_READ, uint32_t(run_start), uint32_t(run_end - run_start));
        learn(uint32_t(run_start), uint32_t(run_end - run_start));
        run_start = run_end = 0;
    };
    for (uint64_t at = address; at < end;) {
        uint32_t page = uint32_t(at / PAGE), lo = uint32_t(at % PAGE);
        uint32_t hi = uint32_t(std::min<uint64_t>(PAGE, lo + (end - at)));
        auto it = pages_.find(page);
        bool same = it != pages_.end();
        if (same) {
            const Page &p = *it->second;
            for (uint32_t b = lo; b < hi && same;) {
                uint32_t word = b / 64, first = b % 64, n = std::min(64 - first, hi - b);
                uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << first;
                same = (p.known[word] & mask) == mask;
                b += n;
            }
            same = same && std::memcmp(p.bytes + lo, memory_ + at, hi - lo) == 0;
        }
        if (same) {
            flush();
        } else {
            if (run_end != at) flush(), run_start = at;
            run_end = at + (hi - lo);
        }
        at += hi - lo;
    }
    flush();
}

// ---- the processor's calls ----

void TraceWriter::buffer_start(bool indirect, uint32_t address, uint32_t dwords) {
    if (!indirect && !started_) {
        started_ = true;
        if (options_.frames.count(0)) open(0);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) {
            // Primary and IndirectBufferStartCommand: type, base_ptr, count
            // (written as 0 by Xenia, so no words follow)
            const uint32_t command[3] = {indirect ? INDIRECT_BUFFER_START : PRIMARY_BUFFER_START, address, 0};
            put(command, sizeof(command));
        }
    }
    if (next_) next_->buffer_start(indirect, address, dwords);
}

void TraceWriter::buffer_end(bool indirect) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) put_u32(indirect ? INDIRECT_BUFFER_END : PRIMARY_BUFFER_END);
    }
    if (next_) next_->buffer_end(indirect);
}

void TraceWriter::packet(uint32_t address, std::span<const uint32_t> words) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ && !words.empty()) {
            // an indirect buffer's packet as its header and address alone: a
            // player that ran it would run the buffer again after its packets
            uint32_t count = uint32_t(words.size());
            uint32_t header = words[0];
            if (header >> 30 == 3 && ((header >> 8) & 0x7F) == xenos::pm4::INDIRECT_BUFFER) count = std::min(count, 2u);
            const uint32_t command[3] = {PACKET_START, address, count};
            put(command, sizeof(command));
            // the guest's words, big-endian as they lie in memory
            std::vector<uint32_t> be(words.begin(), words.begin() + count);
            for (uint32_t &w : be) w = __builtin_bswap32(w);
            put(be.data(), be.size() * 4);
        }
    }
    if (next_) next_->packet(address, words);
}

void TraceWriter::packet_end() {
    if (next_) next_->packet_end();
    bool wanted = false;
    uint64_t frame = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) put_u32(PACKET_END);
        if (!swapped_) return;
        swapped_ = false;
        // the frame ends with its swap
        if (file_) {
            const uint32_t event[2] = {EVENT, 0}; // kSwap
            put(event, sizeof(event));
            close();
        }
        frame = ++swaps_;
        wanted = options_.frames.count(frame) != 0;
    }
    // the next frame, from here, when asked for
    if (wanted) open(frame);
}

void TraceWriter::memory_read(uint32_t address, uint32_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ && bytes) {
            // the processor's reads are recorded every time, as Xenia's
            address &= PHYSICAL_SIZE - 1;
            bytes = std::min(bytes, PHYSICAL_SIZE - address);
            put_memory(MEMORY_READ, address, bytes);
            learn(address, bytes);
        }
    }
    if (next_) next_->memory_read(address, bytes);
}

void TraceWriter::memory_written(uint32_t address, uint32_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_ && bytes) {
            // a player skips these: its processor writes the same
            address &= PHYSICAL_SIZE - 1;
            bytes = std::min(bytes, PHYSICAL_SIZE - address);
            put_memory(MEMORY_WRITE, address, bytes);
            learn(address, bytes);
        }
    }
    if (next_) next_->memory_written(address, bytes);
}

void TraceWriter::shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) {
    if (next_) next_->shader(type, address, code);
}

void TraceWriter::draw(const RegisterFile &registers, const Draw &d) {
    if (next_) next_->draw(registers, d);
}

void TraceWriter::resolve(const RegisterFile &registers, const Draw &d) {
    if (next_) next_->resolve(registers, d);
}

void TraceWriter::swap(const RegisterFile &registers, const Swap &s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        swapped_ = true;
    }
    if (next_) next_->swap(registers, s);
}

// ---- the backend's ----

void TraceWriter::read(uint32_t address, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_locked(address, bytes);
}

void TraceWriter::gpu_wrote(uint32_t address, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || !bytes) return;
    address &= PHYSICAL_SIZE - 1;
    learn(address, std::min(bytes, PHYSICAL_SIZE - address));
}

} // namespace xf::gpu
