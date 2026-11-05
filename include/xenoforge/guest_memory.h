// The console's physical memory as the GPU sees it: 512 MB, addresses masked
// to 29 bits, one contiguous host range. Either the system's own (a view of
// Memory::translate_physical(0), so the renderer reads guest memory where it
// lies) or one of its own, zeroed, for the tools and tests that fill it
// themselves (the operating system hands out its pages as they are first
// touched). Big-endian words are read and written as the guest stores them.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

namespace xf {

class GuestMemory {
public:
    static constexpr uint32_t PHYSICAL_MASK = 0x1FFFFFFF;
    static constexpr uint32_t SIZE = 0x20000000;

    // its own 512 MB, zeroed (std::bad_alloc when the host has no room)
    GuestMemory();
    // a view of physical memory that lies elsewhere: SIZE bytes from physical
    explicit GuestMemory(uint8_t *physical) : data_(physical) {}
    ~GuestMemory();
    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }

    // Bytes at an address; a range past the end of physical memory wraps to
    // its start, as the address mask does.
    void write(uint32_t address, std::span<const uint8_t> bytes) {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t a = (address + uint32_t(i)) & PHYSICAL_MASK;
            size_t n = std::min<size_t>(SIZE - a, bytes.size() - i);
            std::memcpy(data_ + a, bytes.data() + i, n);
            i += n;
        }
    }
    void read(uint32_t address, std::span<uint8_t> out) const {
        for (size_t i = 0; i < out.size();) {
            uint32_t a = (address + uint32_t(i)) & PHYSICAL_MASK;
            size_t n = std::min<size_t>(SIZE - a, out.size() - i);
            std::memcpy(out.data() + i, data_ + a, n);
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

private:
    uint8_t *data_ = nullptr;
    uint8_t *owned_ = nullptr; // what the first constructor allocated
};

} // namespace xf
