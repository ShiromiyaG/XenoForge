// The guest's memory as the console lays it out (plan step A4; after
// Xenia's src/xenia/memory.h and memory.cc, BSD; see LICENSE-xenia): one
// shared-memory section viewed several times in a 4 GB host range, so a
// guest address is an offset from base() (plus 0x1000 at 0xE0000000 and
// above, the 4 KB offset of that physical view), and the three physical
// windows (0xA0000000, 0xC0000000, 0xE0000000) see the same 512 MB.
//
//   0x00000000-0x3FFFFFFF  virtual, 4 KB pages
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages
//   0x7F000000-0x7FC7FFFF  GPU writeback and XPS (physical, 4 KB)
//   0x7FC80000-0x7FFFFFFF  MMIO (the GPU's registers): not mapped here
//   0x80000000-0x8FFFFFFF  XEX images, 64 KB pages
//   0x90000000-0x9FFFFFFF  XEX images, 4 KB pages
//   0xA0000000-0xBFFFFFFF  physical, 64 KB pages
//   0xC0000000-0xDFFFFFFF  physical, 16 MB pages
//   0xE0000000-0xFFFFFFFF  physical, 4 KB pages
//
// Each range is a heap that hands out pages (reserve, commit, release,
// protect, query) as the kernel's NtAllocateVirtualMemory,
// MmAllocatePhysicalMemoryEx and friends need; the physical windows'
// heaps take their pages from the one physical heap.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace xf {

// as Xenia's kMemoryAllocation* and kMemoryProtect*
enum : uint32_t { ALLOCATE_RESERVE = 1, ALLOCATE_COMMIT = 2 };
enum : uint32_t { PROTECT_READ = 1, PROTECT_WRITE = 2, PROTECT_NO_CACHE = 4, PROTECT_WRITE_COMBINE = 8 };
constexpr uint32_t PROTECT_READ_WRITE = PROTECT_READ | PROTECT_WRITE;

// what NtQueryVirtualMemory reports (Xenia's HeapAllocationInfo)
struct RegionInfo {
    uint32_t base_address = 0;
    uint32_t allocation_base = 0;
    uint32_t allocation_protect = 0;
    uint32_t allocation_size = 0;
    uint32_t region_size = 0;
    uint32_t state = 0;   // ALLOCATE_* bits; 0 free
    uint32_t protect = 0;
};

class Heap {
public:
    virtual ~Heap() = default;
    virtual uint32_t heap_base() const = 0;
    virtual uint32_t heap_size() const = 0;
    virtual uint32_t page_size() const = 0;
    bool contains(uint32_t address) const { return address - heap_base() < heap_size(); }

    // false when it does not fit; *out the guest address
    virtual bool alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
                       uint32_t *out) = 0;
    virtual bool alloc_fixed(uint32_t address, uint32_t size, uint32_t alignment, uint32_t type,
                             uint32_t protect) = 0;
    virtual bool alloc_range(uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, uint32_t type,
                             uint32_t protect, bool top_down, uint32_t *out) = 0;
    // the whole allocation at its base; *out_size its size if not null
    virtual bool release(uint32_t address, uint32_t *out_size = nullptr) = 0;
    virtual bool decommit(uint32_t address, uint32_t size) = 0;
    virtual bool protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old_protect = nullptr) = 0;
    virtual bool query(uint32_t address, RegionInfo &out) const = 0;
    virtual bool query_size(uint32_t address, uint32_t *out_size) const = 0;
};

class Memory {
public:
    static std::unique_ptr<Memory> create(std::string &error);
    virtual ~Memory() = default;

    // host address of guest address 0
    virtual uint8_t *base() const = 0;
    uint8_t *translate(uint32_t address) const { return base() + address + (address >= 0xE0000000u ? 0x1000u : 0u); }
    // a physical address (0-0x1FFFFFFF) as the GPU and DMA see it
    virtual uint8_t *translate_physical(uint32_t physical) const = 0;
    // the physical address behind a guest address in a physical window
    // (0xA0000000 and up, 0x7F000000); the address itself below
    virtual uint32_t physical_address(uint32_t address) const = 0;

    // the heap holding an address (nullptr outside every heap)
    virtual Heap *heap_for(uint32_t address) = 0;
    // by kind, as Xenia's LookupHeapByType: virtual or physical, by page size
    virtual Heap *heap_by_type(bool physical, uint32_t page_size) = 0;

    // the kernel's own small allocations (objects the guest sees), in the
    // 64 KB virtual range, committed, read-write, zeroed
    virtual uint32_t system_alloc(uint32_t size, uint32_t alignment = 0x20) = 0;
    virtual void system_free(uint32_t address) = 0;
};

} // namespace xf
