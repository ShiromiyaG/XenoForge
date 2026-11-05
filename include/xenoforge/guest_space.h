// The guest's 4 GB of address space, reserved in the host's at once so a
// guest address is an offset from base(); pages are committed as the system
// maps them (plan, "Memory"; step A4 adds the physical mirrors and guards).

#pragma once

#include <cstdint>

namespace xf {

class GuestSpace {
public:
    GuestSpace();
    ~GuestSpace();
    GuestSpace(const GuestSpace &) = delete;
    GuestSpace &operator=(const GuestSpace &) = delete;

    bool ok() const { return base_ != nullptr; }
    uint8_t *base() const { return base_; }
    // readable and writable, zeroed when first committed
    bool commit(uint32_t address, uint32_t size);
    // back to zeros and uncommitted
    void decommit(uint32_t address, uint32_t size);

private:
    uint8_t *base_ = nullptr;
};

} // namespace xf
