// The console's physical memory as the GPU sees it (512 MB, addresses masked
// to 29 bits), kept in 64 KB chunks made as they are first written; what was
// never written reads as zeros. Big-endian words are read and written as
// the guest stores them.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace xf {

class GuestMemory {
public:
    static constexpr uint32_t PHYSICAL_MASK = 0x1FFFFFFF;
    static constexpr uint32_t CHUNK = 1u << 16;

    void write(uint32_t address, std::span<const uint8_t> bytes) {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t a = (address + uint32_t(i)) & PHYSICAL_MASK;
            uint8_t *chunk = writable_chunk(a);
            size_t n = std::min<size_t>(CHUNK - a % CHUNK, bytes.size() - i);
            std::memcpy(chunk + a % CHUNK, bytes.data() + i, n);
            i += n;
        }
    }
    void read(uint32_t address, std::span<uint8_t> out) const {
        for (size_t i = 0; i < out.size();) {
            uint32_t a = (address + uint32_t(i)) & PHYSICAL_MASK;
            const uint8_t *chunk = chunk_for(a);
            size_t n = std::min<size_t>(CHUNK - a % CHUNK, out.size() - i);
            if (chunk) std::memcpy(out.data() + i, chunk + a % CHUNK, n);
            else std::memset(out.data() + i, 0, n);
            i += n;
        }
    }
    uint32_t be32(uint32_t address) const {
        uint8_t b[4];
        read(address, b);
        return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
    void put_be32(uint32_t address, uint32_t value) {
        const uint8_t b[4] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
        write(address, b);
    }

    // The chunks written so far, by index (address / CHUNK), for uploading.
    template <typename F> void each_chunk(F f) const {
        for (const auto &[index, data] : chunks_) f(index, data.get());
    }
    // Bumped by every write: a cheap way to see that something changed.
    uint64_t version() const { return version_; }
    // version() as of the last write to chunk `index` (0: never written).
    uint64_t chunk_version(uint32_t index) const {
        auto it = chunk_versions_.find(index);
        return it == chunk_versions_.end() ? 0 : it->second;
    }
    // The chunks written after version `since`, by index, for uploading what changed.
    template <typename F> void each_chunk_since(uint64_t since, F f) const {
        for (const auto &[index, version] : chunk_versions_)
            if (version > since) f(index, chunks_.at(index).get());
    }

private:
    uint8_t *writable_chunk(uint32_t a) {
        auto &chunk = chunks_[a / CHUNK];
        if (!chunk) chunk = std::make_unique<uint8_t[]>(CHUNK), std::memset(chunk.get(), 0, CHUNK);
        chunk_versions_[a / CHUNK] = ++version_;
        return chunk.get();
    }
    const uint8_t *chunk_for(uint32_t a) const {
        auto it = chunks_.find(a / CHUNK);
        return it == chunks_.end() ? nullptr : it->second.get();
    }
    std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> chunks_;
    std::unordered_map<uint32_t, uint64_t> chunk_versions_;
    uint64_t version_ = 0;
};

} // namespace xf
