// The guest's address space (guest_space.h).

#include "xenoforge/guest_space.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace xf {

namespace {
constexpr uint64_t SPACE = 1ull << 32;
constexpr uint32_t PAGE = 4096;
} // namespace

GuestSpace::GuestSpace() {
#ifdef _WIN32
    base_ = static_cast<uint8_t *>(VirtualAlloc(nullptr, SPACE, MEM_RESERVE, PAGE_NOACCESS));
#else
    void *p = mmap(nullptr, SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
#endif
}

GuestSpace::~GuestSpace() {
    if (!base_) return;
#ifdef _WIN32
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, SPACE);
#endif
}

bool GuestSpace::commit(uint32_t address, uint32_t size) {
    if (!base_ || !size) return false;
    uint64_t start = address & ~uint64_t(PAGE - 1);
    uint64_t end = (uint64_t(address) + size + PAGE - 1) & ~uint64_t(PAGE - 1);
    if (end > SPACE) return false;
#ifdef _WIN32
    return VirtualAlloc(base_ + start, size_t(end - start), MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base_ + start, size_t(end - start), PROT_READ | PROT_WRITE) == 0;
#endif
}

void GuestSpace::decommit(uint32_t address, uint32_t size) {
    if (!base_ || !size) return;
    uint64_t start = address & ~uint64_t(PAGE - 1);
    uint64_t end = (uint64_t(address) + size + PAGE - 1) & ~uint64_t(PAGE - 1);
#ifdef _WIN32
    VirtualFree(base_ + start, size_t(end - start), MEM_DECOMMIT);
#else
    mmap(base_ + start, size_t(end - start), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
#endif
}

} // namespace xf
