// GuestMemory's own 512 MB (guest_memory.h): reserved and committed at once,
// the pages zeroed by the operating system as they are first touched, so a
// test that writes a few textures costs only those pages.

#include "xenoforge/guest_memory.h"

#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace xf {

GuestMemory::GuestMemory() {
#ifdef _WIN32
    owned_ = static_cast<uint8_t *>(VirtualAlloc(nullptr, SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void *p = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    owned_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
#endif
    if (!owned_) throw std::bad_alloc();
    data_ = owned_;
}

GuestMemory::~GuestMemory() {
    if (!owned_) return;
#ifdef _WIN32
    VirtualFree(owned_, 0, MEM_RELEASE);
#else
    munmap(owned_, SIZE);
#endif
}

} // namespace xf
