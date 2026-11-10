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
//   0x7FC80000-0x7FFFFFFF  MMIO (the GPU's registers): no access here
//   0x80000000-0x8FFFFFFF  XEX images, 64 KB pages
//   0x90000000-0x9FFFFFFF  XEX images, 4 KB pages (the same host memory as
//                          0x80000000, as Xenia maps it)
//   0xA0000000-0xBFFFFFFF  physical, 64 KB pages
//   0xC0000000-0xDFFFFFFF  physical, 16 MB pages
//   0xE0000000-0xFFCFFFFF  physical, 4 KB pages (physical 0x1000 and up)
//
// Each range is a heap that hands out pages (reserve, commit, release,
// protect, query) as the kernel's NtAllocateVirtualMemory,
// MmAllocatePhysicalMemoryEx and friends need; the physical windows'
// heaps take their pages from the one physical heap.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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
    // (0xA0000000 and up, 0x7F000000); elsewhere what Xenia's
    // GetPhysicalAddress gives, the address's offset in its heap (the address
    // itself below 0x40000000: no physical page is behind virtual memory
    // here); UINT32_MAX outside every heap
    virtual uint32_t physical_address(uint32_t address) const = 0;

    // the heap holding an address (nullptr outside every heap)
    virtual Heap *heap_for(uint32_t address) = 0;
    // by kind, as Xenia's LookupHeapByType: virtual or physical, by page size
    virtual Heap *heap_by_type(bool physical, uint32_t page_size) = 0;

    // the kernel's own small allocations (objects the guest sees),
    // committed, read-write, zeroed, a page each (Xenia's SystemHeapAlloc):
    // virtual from the top 256 MB of the 4 KB range (0x30000000 up),
    // physical (what the GPU or the audio read) from the 4 KB window
    // (0xE0000000, top-down); 0 when full
    virtual uint32_t system_alloc(uint32_t size, uint32_t alignment = 0x20, bool physical = false) = 0;
    virtual void system_free(uint32_t address) = 0;

    // ---- write watches on physical memory ----
    // (Xenia's physical memory access callbacks: EnablePhysicalMemoryAccessCallbacks,
    // TriggerPhysicalMemoryCallbacks and the access violation handler.) What
    // keeps a copy of physical memory, as the renderer's mirror and textures
    // do, learns of the writes to it this way. Physical memory is cut into
    // watch pages of the host's page size (4 KB on Windows and most hosts, 16
    // KB on some ARM64 ones). Every watch page has the number of the last
    // write seen to it, out of a count of writes that only grows.
    //
    // A watched page is protected read-only in the windows where the guest may
    // write it (0x7F000000, 0xA0000000, 0xC0000000, 0xE0000000; never in
    // translate_physical's view). The first write to it after that, by the
    // game's code (generated or interpreted) or by the kernel's host code,
    // faults once: the fault numbers the page (and, as Xenia widens it, the
    // pages around it its watchers allow), takes the protection off and lets
    // the write go on. Writes through translate_physical's view (the GPU's
    // command processor, DMA, a file read made there) are not seen: whoever
    // makes them calls physical_written.
    //
    // A watcher's use: watch the pages first, then take write_count(), then
    // copy the bytes. A page whose stamp is at or below that count has not
    // been written since the copy; a later write numbers it higher.
    uint32_t watch_page_size() const { return 1u << watch_page_shift(); }
    virtual uint32_t watch_page_shift() const = 0;
    // the number of the last write seen (they start at 1)
    virtual uint32_t write_count() const = 0;
    // each watch page's: the number of the last write seen to it (0: none
    // since the start); 0x20000000 >> watch_page_shift() of them
    virtual const std::atomic<uint32_t> *write_stamps() const = 0;
    // and per block of 1 << WRITE_BLOCK_SHIFT watch pages, the largest of
    // its pages' (stored after them): a block at or below a count has no page
    // above it, so a check of a range skips the blocks nothing wrote
    static constexpr uint32_t WRITE_BLOCK_SHIFT = 6;
    virtual const std::atomic<uint32_t> *write_block_stamps() const = 0;
    // Watches the pages of [physical, physical + length). Pages the guest
    // cannot write anywhere stay unwatched: making them writable (the heaps'
    // protect, alloc, decommit and release) numbers them as written.
    virtual void watch(uint32_t physical, uint32_t length) = 0;
    // Host code wrote [physical, physical + length) where no watch sees it:
    // the pages are numbered as written, and their watches go.
    virtual void physical_written(uint32_t physical, uint32_t length) = 0;
    // Where host code writes the guest's memory at a guest address so that a
    // watch does not stop it (an operating system's call filling it, as a file
    // read, fails on a protected page instead of faulting): the physical view
    // for an address in a physical window, as Xenia's file reads write there,
    // else translate(). Tell written() after.
    uint8_t *translate_for_host_write(uint32_t address) const {
        uint32_t physical = physical_window(address) ? physical_address(address) : UINT32_MAX;
        return physical != UINT32_MAX ? translate_physical(physical) : translate(address);
    }
    // what host code wrote at a guest address through translate_for_host_write
    void written(uint32_t address, uint32_t length) {
        if (length && physical_window(address)) physical_written(physical_address(address), length);
    }
    static bool physical_window(uint32_t address) {
        return (address >= 0x7F000000u && address < 0x7FC80000u) || address >= 0xA0000000u;
    }
    // A watcher's say in how far around a written page the watches may go
    // (Xenia's invalidation callbacks' unwatch range): given the first and
    // last pages written, the widest [first, last] it accepts seeing as
    // written. Memory takes what every watcher accepts, at most the 4 MB
    // around the write, at least the pages written. Called in the fault
    // handler, under the heaps' lock, on the writing thread: it must not
    // block nor take a lock held around calls of watch().
    using WatchBounds = std::pair<uint32_t, uint32_t> (*)(void *context, uint32_t first_page, uint32_t last_page);
    virtual void *add_watcher(WatchBounds bounds, void *context) = 0;
    virtual void remove_watcher(void *handle) = 0;
    // what the watches did so far: write faults taken (and of them, ones
    // another thread's fault had already let go), pages watched, and pages
    // numbered as written (by faults, widened, and by host writes told)
    struct WatchCounts {
        uint64_t faults = 0, retried = 0, watched = 0, written = 0;
    };
    virtual WatchCounts watch_counts() const = 0;

    // ---- a private view, for the lockstep check (lockstep.h) ----
    // The whole guest memory mapped again at another host base, copy on
    // write: a page reads as the guest's memory is now until this view writes
    // it, and what this view writes stays in it (each of its views copies on
    // its own, so the physical windows stop aliasing each other here once
    // written). Its base stands for base(): guest address a is at
    // base() + a (+ 0x1000 from 0xE0000000, as translate()).
    class PrivateView {
    public:
        virtual ~PrivateView() = default;
        uint8_t *base() const { return base_; }
        // The views holding [address, address + length) mapped again, their
        // copies dropped: they read the guest's memory again. False when one
        // could not be mapped again (the view is unusable then).
        virtual bool reset(uint32_t address, uint32_t length) = 0;

    protected:
        uint8_t *base_ = nullptr;
    };
    // nullptr, and why, when the host has no room for it
    virtual std::unique_ptr<PrivateView> private_view(std::string &error) = 0;
};

} // namespace xf
