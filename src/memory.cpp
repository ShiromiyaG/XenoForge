// The guest's memory (memory.h). Translated from Xenia: src/xenia/memory.cc
// and memory.h (Memory::Initialize, MapViews and its map_info, LookupHeap,
// LookupHeapByType, GetPhysicalAddress and SystemHeapAlloc; BaseHeap with
// its page table and free block tracker; PhysicalHeap) and the host
// primitives they use from src/xenia/base/memory_win.cc and memory_posix.cc.
// The write watches on physical memory follow PhysicalHeap's
// EnableAccessCallbacks and TriggerCallbacks, the invalidations in its
// Release, Decommit and Protect, Memory::AccessViolationCallback, and the
// fault handlers of src/xenia/base/exception_handler_win.cc and
// exception_handler_posix.cc (watches below). Left out: the MMIO handler
// (0x7FC80000-0x7FFFFFFF is committed no-access instead, so an access
// faults), the read watches (data providers, which nothing in Xenia uses
// yet), Reset, Save and Restore, DumpMap, the scribble_heap and
// protect_on_release options (off in Xenia by default), and the merged
// protection of hosts whose pages are larger than 4 KB (left as the views
// map them). BSD; see LICENSE-xenia.

#include "xenoforge/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static_assert(sizeof(void *) == 8, "the guest's views need a 64-bit host");

namespace xf {
namespace {

// ---------------------------------------------------------------------------
// The host's memory (Xenia's base/memory_win.cc and memory_posix.cc)

enum class Access { none, read_only, read_write };

// as Xenia's ToPageAccess: write-combined memory is writable too
Access to_access(uint32_t protect) {
    bool writable = (protect & PROTECT_WRITE) || (protect & PROTECT_WRITE_COMBINE);
    if (!(protect & PROTECT_READ)) return Access::none;
    return writable ? Access::read_write : Access::read_only;
}

// as Xenia's FromPageAccess
uint32_t from_access(Access access) {
    switch (access) {
    case Access::read_only: return PROTECT_READ;
    case Access::read_write: return PROTECT_READ_WRITE;
    default: return 0;
    }
}

#ifdef _WIN32

using MappingHandle = HANDLE;

DWORD win32_protect(Access access) {
    switch (access) {
    case Access::read_only: return PAGE_READONLY;
    case Access::read_write: return PAGE_READWRITE;
    default: return PAGE_NOACCESS;
    }
}

size_t host_page_size() {
    static const size_t size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return size_t(info.dwPageSize);
    }();
    return size;
}

// pagefile-backed and reserved, not committed (SEC_RESERVE): pages are
// committed as the heaps hand them out
bool create_mapping(uint64_t size, MappingHandle &out) {
    out = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, DWORD(size >> 32),
                             DWORD(size), nullptr);
    return out != nullptr;
}

void close_mapping(MappingHandle handle) { CloseHandle(handle); }

uint8_t *map_view(MappingHandle handle, uint8_t *base, size_t length, uint64_t offset) {
    return static_cast<uint8_t *>(
        MapViewOfFileEx(handle, FILE_MAP_ALL_ACCESS, DWORD(offset >> 32), DWORD(offset), length, base));
}

void unmap_view(uint8_t *view, size_t) { UnmapViewOfFile(view); }

// Memory::PrivateView's views: copy on write, mapped into placeholders
// (VirtualAlloc2, MapViewOfFile3, UnmapViewOfFile2; Windows 10 1803 and up,
// found at run time), so the host picks where the 4.5 GB go and a view's
// copies are dropped by mapping it again into its own placeholder, a hole no
// other thread can take meanwhile.
struct Placeholders {
    using Alloc2 = PVOID(WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
    using Map3 = PVOID(WINAPI *)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
    using Unmap2 = BOOL(WINAPI *)(HANDLE, PVOID, ULONG);
    Alloc2 alloc2 = nullptr;
    Map3 map3 = nullptr;
    Unmap2 unmap2 = nullptr;
    // (winnt.h's, which older headers lack)
    static constexpr ULONG reserve_placeholder = 0x00040000, replace_placeholder = 0x00004000,
                           preserve_placeholder = 0x00000002;
    Placeholders() {
        if (HMODULE m = GetModuleHandleW(L"kernelbase.dll")) {
            alloc2 = reinterpret_cast<Alloc2>(reinterpret_cast<void *>(GetProcAddress(m, "VirtualAlloc2")));
            map3 = reinterpret_cast<Map3>(reinterpret_cast<void *>(GetProcAddress(m, "MapViewOfFile3")));
            unmap2 = reinterpret_cast<Unmap2>(reinterpret_cast<void *>(GetProcAddress(m, "UnmapViewOfFile2")));
        }
    }
    bool usable() const { return alloc2 && map3 && unmap2; }
};
const Placeholders &placeholders() {
    static const Placeholders p;
    return p;
}

// room for a private view of `total` bytes, nullptr if none
uint8_t *reserve_private(size_t total) {
    if (!placeholders().usable()) return nullptr;
    return static_cast<uint8_t *>(placeholders().alloc2(GetCurrentProcess(), nullptr, total,
                                                        MEM_RESERVE | Placeholders::reserve_placeholder,
                                                        PAGE_NOACCESS, nullptr, 0));
}
// the room cut where a view of `length` starts at `at` (each view its own)
bool split_private(uint8_t *at, size_t length) {
    return VirtualFree(at, length, MEM_RELEASE | Placeholders::preserve_placeholder) != 0;
}
uint8_t *map_private_view(MappingHandle handle, uint8_t *at, size_t length, uint64_t offset) {
    return static_cast<uint8_t *>(placeholders().map3(handle, GetCurrentProcess(), at, offset, length,
                                                      Placeholders::replace_placeholder, PAGE_WRITECOPY, nullptr, 0));
}
// the view's copies dropped: back to its placeholder, mapped again
uint8_t *remap_private_view(MappingHandle handle, uint8_t *view, size_t length, uint64_t offset) {
    if (!placeholders().unmap2(GetCurrentProcess(), view, Placeholders::preserve_placeholder)) return nullptr;
    return map_private_view(handle, view, length, offset);
}
// a view (if mapped) and its room given back
void release_private(uint8_t *at, [[maybe_unused]] size_t length, bool mapped) {
    if (mapped) placeholders().unmap2(GetCurrentProcess(), at, Placeholders::preserve_placeholder);
    VirtualFree(at, 0, MEM_RELEASE);
}

bool host_commit(void *address, size_t length, Access access) {
    return VirtualAlloc(address, length, MEM_COMMIT, win32_protect(access)) != nullptr;
}

bool host_protect(void *address, size_t length, Access access) {
    DWORD old;
    return VirtualProtect(address, length, win32_protect(access), &old) != 0;
}

// The handler of write faults (exception_handler_win.cc): a vectored handler,
// first in the process's list when it is added, so it sees a fault before a
// handler added earlier that reports crashes; a write it does not take goes
// on to them.
bool (*write_fault)(void *host_address) = nullptr;

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    return write_fault(reinterpret_cast<void *>(r->ExceptionInformation[1])) ? EXCEPTION_CONTINUE_EXECUTION
                                                                             : EXCEPTION_CONTINUE_SEARCH;
}

void install_write_fault_handler(bool (*handler)(void *host_address)) {
    write_fault = handler;
    AddVectoredExceptionHandler(1, vectored_handler);
}

#else

using MappingHandle = int;

int posix_protect(Access access) {
    switch (access) {
    case Access::read_only: return PROT_READ;
    case Access::read_write: return PROT_READ | PROT_WRITE;
    default: return PROT_NONE;
    }
}

size_t host_page_size() {
    static const size_t size = size_t(sysconf(_SC_PAGESIZE));
    return size;
}

// shm_open as Xenia's memory_posix does, unlinked at once: the descriptor
// keeps the memory, and nothing is left behind
bool create_mapping(uint64_t size, MappingHandle &out) {
    static std::atomic<unsigned> counter{0};
    char name[64];
    std::snprintf(name, sizeof(name), "/xenoforge_memory_%ld_%u", long(getpid()), counter++);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    shm_unlink(name);
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return false;
    }
    out = fd;
    return true;
}

void close_mapping(MappingHandle handle) { close(handle); }

// read-write as Xenia maps them: mmap has no reserve and commit
uint8_t *map_view(MappingHandle handle, uint8_t *base, size_t length, uint64_t offset) {
    int flags = MAP_SHARED;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    void *view = mmap(base, length, PROT_READ | PROT_WRITE, flags, handle, off_t(offset));
    if (view == MAP_FAILED) return nullptr;
    if (view != base) {
        munmap(view, length);
        return nullptr;
    }
    return static_cast<uint8_t *>(view);
}

void unmap_view(uint8_t *view, size_t length) { munmap(view, length); }

// Memory::PrivateView's views: private mappings of the shared memory's
// descriptor (copy on write: they read the shared pages until they write
// them), over room reserved for them; MAP_FIXED replaces a mapping at once,
// so a view's copies are dropped by mapping it again over itself.
uint8_t *reserve_private(size_t total) {
    void *room = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return room == MAP_FAILED ? nullptr : static_cast<uint8_t *>(room);
}
bool split_private(uint8_t *, size_t) { return true; }
uint8_t *map_private_view(MappingHandle handle, uint8_t *at, size_t length, uint64_t offset) {
    void *view = mmap(at, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, handle, off_t(offset));
    return view == MAP_FAILED ? nullptr : static_cast<uint8_t *>(view);
}
uint8_t *remap_private_view(MappingHandle handle, uint8_t *view, size_t length, uint64_t offset) {
    return map_private_view(handle, view, length, offset);
}
void release_private(uint8_t *at, size_t length, bool) { munmap(at, length); }

bool host_commit(void *address, size_t length, Access access) {
    return mprotect(address, length, posix_protect(access)) == 0;
}

bool host_protect(void *address, size_t length, Access access) {
    return mprotect(address, length, posix_protect(access)) == 0;
}

// The handler of write faults (exception_handler_posix.cc): SIGSEGV (and
// SIGBUS, which some hosts raise for a protected page). Only a write faults
// on a page the watches made read-only, so the handler needs no decoding of
// the access (Xenia reads the x86 page fault code or ARM64's ESR for it). A
// fault it does not take goes to the handler that was there before, or, with
// none, to the default action: restored, it acts when the access comes again.
bool (*write_fault)(void *host_address) = nullptr;
struct sigaction previous_segv, previous_bus;

void signal_handler(int signal, siginfo_t *info, void *context) {
    if (write_fault(info->si_addr)) return;
    struct sigaction &previous = signal == SIGBUS ? previous_bus : previous_segv;
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
        return;
    }
    struct sigaction fallback = {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
}

void install_write_fault_handler(bool (*handler)(void *host_address)) {
    write_fault = handler;
    struct sigaction action = {};
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previous_segv);
    sigaction(SIGBUS, &action, &previous_bus);
}

#endif

// ---------------------------------------------------------------------------
// Heaps

// as Xenia's xe::round_up: zero rounds up to one multiple
uint32_t round_up(uint32_t value, uint32_t multiple) {
    return value ? (value + multiple - 1) / multiple * multiple : multiple;
}
uint32_t align(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
uint32_t shift_of(uint32_t value) {
    uint32_t shift = 0;
    while ((1u << shift) < value) shift++;
    return shift;
}

enum class HeapType { virt, xex, physical };

// one page of a heap (Xenia's PageEntry)
struct PageEntry {
    uint32_t base_address : 20;      // the region's first page
    uint32_t region_page_count : 20; // its pages
    uint32_t allocation_protect : 4; // PROTECT_* as reserved
    uint32_t current_protect : 4;    // PROTECT_* now
    uint32_t state : 2;              // ALLOCATE_*
};

// pages handed out from a range (Xenia's BaseHeap and VirtualHeap)
class BaseHeap : public Heap {
public:
    void init(std::recursive_mutex *lock, uint8_t *membase, HeapType type, uint32_t heap_base, uint32_t heap_size,
              uint32_t page_size, uint32_t host_address_offset = 0) {
        lock_ = lock;
        membase_ = membase;
        heap_type_ = type;
        heap_base_ = heap_base;
        heap_size_ = heap_size;
        page_size_ = page_size;
        page_size_shift_ = shift_of(page_size);
        host_address_offset_ = host_address_offset;
        page_table_.assign(heap_size / page_size, PageEntry{});
        free_blocks_.clear();
        free_blocks_[0] = uint32_t(page_table_.size());
    }

    uint32_t heap_base() const override { return heap_base_; }
    uint32_t heap_size() const override { return heap_size_; }
    uint32_t page_size() const override { return page_size_; }
    HeapType heap_type() const { return heap_type_; }
    uint8_t *translate_relative(uint32_t offset) const { return membase_ + heap_base_ + host_address_offset_ + offset; }

    bool alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
               uint32_t *out) override;
    bool alloc_fixed(uint32_t base_address, uint32_t size, uint32_t alignment, uint32_t type,
                     uint32_t protect) override;
    bool alloc_range(uint32_t low_address, uint32_t high_address, uint32_t size, uint32_t alignment, uint32_t type,
                     uint32_t protect, bool top_down, uint32_t *out) override;
    // Xenia's AllocSystemHeap: the virtual heaps' top 256 MB
    virtual bool alloc_system(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
                              uint32_t *out);
    bool release(uint32_t base_address, uint32_t *out_size = nullptr) override;
    bool decommit(uint32_t address, uint32_t size) override;
    bool protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old_protect = nullptr) override;
    bool query(uint32_t address, RegionInfo &out) const override;
    bool query_size(uint32_t address, uint32_t *out_size) const override;

protected:
    uint32_t page_count(uint32_t size) const { return round_up(size, page_size_) / page_size_; }
    void rebuild_free_blocks();
    void remove_free_block(uint32_t start_page, uint32_t page_count);
    void insert_free_block(uint32_t start_page, uint32_t page_count);
    // pages taken from the host: committed with the access; nothing to do to
    // reserve (the views are mapped), and nothing when the host's pages are
    // larger than 4 KB (Xenia's ShouldSkipHostCommit: the views are mapped
    // read-write there)
    bool host_alloc(uint32_t start_page, uint32_t page_count, uint32_t type, uint32_t protect) {
        if (!(type & ALLOCATE_COMMIT) || host_page_size() > 0x1000) return true;
        return host_commit(translate_relative(start_page << page_size_shift_), size_t(page_count) << page_size_shift_,
                           to_access(protect));
    }

    std::recursive_mutex *lock_ = nullptr; // Xenia's global_critical_region: one for every heap
    uint8_t *membase_ = nullptr;
    HeapType heap_type_ = HeapType::virt;
    uint32_t heap_base_ = 0;
    uint32_t heap_size_ = 0;
    uint32_t page_size_ = 0;
    uint32_t page_size_shift_ = 0;
    uint32_t host_address_offset_ = 0; // added to the guest address on the host (0x1000 at 0xE0000000)
    std::vector<PageEntry> page_table_;
    // runs of free pages, first page to count, kept as the page table changes
    std::map<uint32_t, uint32_t> free_blocks_;
};

bool BaseHeap::alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down, uint32_t *out) {
    *out = 0;
    size = round_up(size, page_size_);
    alignment = round_up(alignment, page_size_);
    // the top 240 MB of the 64 KB virtual heap (0x70000000-0x7EFFFFFF) are
    // left out of general allocation: the threads' stacks are there
    uint32_t stacks = heap_type_ == HeapType::virt && page_size_ == 0x10000 ? 0x0F000000 : 0;
    return alloc_range(heap_base_, heap_base_ + (heap_size_ - 1) - stacks, size, alignment, type, protect, top_down,
                       out);
}

bool BaseHeap::alloc_fixed(uint32_t base_address, uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect) {
    type &= ALLOCATE_RESERVE | ALLOCATE_COMMIT;
    protect &= 0xF;
    // a type that neither reserves nor commits fails on the host in Xenia
    if (!type) return false;
    alignment = round_up(alignment, page_size_);
    size = align(size, alignment);
    uint32_t count = page_count(size);
    uint32_t start_page = (base_address - heap_base_) / page_size_;
    uint32_t end_page = start_page + count - 1;
    // (Xenia lets the end page be one past its table)
    if (start_page >= page_table_.size() || end_page >= page_table_.size() || end_page < start_page) return false;

    std::lock_guard guard(*lock_);

    // reserving: none of the pages may be reserved already; committing: they
    // may be committed already
    const bool pure_reserve = type == ALLOCATE_RESERVE;
    for (uint32_t page = start_page; page <= end_page; ++page) {
        uint32_t state = page_table_[page].state;
        if (type == ALLOCATE_RESERVE && state) return false;
        if (type == ALLOCATE_COMMIT && !(state & ALLOCATE_RESERVE)) {
            // a commit of pages not reserved: reserved too
            type |= ALLOCATE_RESERVE;
            break;
        }
    }

    if (!host_alloc(start_page, count, type, protect)) return false;

    bool had_free_pages = false;
    for (uint32_t page = start_page; page <= end_page; ++page) {
        PageEntry &entry = page_table_[page];
        if (type & ALLOCATE_RESERVE) {
            // the region is the reservation
            entry.base_address = start_page;
            entry.region_page_count = count;
        }
        entry.allocation_protect = protect;
        entry.current_protect = protect;
        if (!(entry.state & ALLOCATE_RESERVE)) had_free_pages = true;
        entry.state = ALLOCATE_RESERVE | type;
    }
    if (had_free_pages) {
        // a pure reserve found every page free, so in one free block; a
        // commit may have spanned several
        if (pure_reserve)
            remove_free_block(start_page, count);
        else
            rebuild_free_blocks();
    }
    return true;
}

template <typename T> T quick_mod(T value, uint32_t modulus) {
    return (modulus & (modulus - 1)) == 0 ? value & (modulus - 1) : value % modulus;
}

bool BaseHeap::alloc_range(uint32_t low_address, uint32_t high_address, uint32_t size, uint32_t alignment,
                           uint32_t type, uint32_t protect, bool top_down, uint32_t *out) {
    *out = 0;
    type &= ALLOCATE_RESERVE | ALLOCATE_COMMIT;
    protect &= 0xF;
    if (!type) return false;

    alignment = round_up(alignment, page_size_);
    uint32_t count = page_count(size);
    low_address = std::max(heap_base_, align(low_address, alignment));
    // the ceiling is not rounded up to the alignment (that could place the
    // allocation above it); the page rounding below keeps a window exactly
    // the request's size from losing its top stride
    high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
    if (low_address > high_address) return false;

    uint32_t low_page = (low_address - heap_base_) >> page_size_shift_;
    uint32_t high_page = (high_address - heap_base_ + (page_size_ - 1)) >> page_size_shift_;
    uint32_t last_page = uint32_t(page_table_.size()) - 1;
    low_page = std::min(last_page, low_page);
    high_page = std::min(last_page, high_page);
    if (count > high_page - low_page) return false;

    std::lock_guard guard(*lock_);

    // a free run whose first page is on the alignment; high_page is exclusive
    uint32_t start_page = UINT32_MAX;
    uint32_t stride = alignment >> page_size_shift_;
    if (top_down) {
        auto it = free_blocks_.upper_bound(high_page);
        while (it != free_blocks_.begin()) {
            --it;
            uint32_t block_start = it->first;
            uint32_t block_count = it->second;
            uint32_t block_end = block_start + block_count;
            if (block_end <= low_page) break; // below the range
            if (block_count < count) continue;
            // the highest aligned start in this block and the range
            uint32_t high_aligned = high_page - quick_mod(high_page, stride);
            uint32_t usable_end = std::min(block_end, high_aligned);
            if (usable_end < count) continue;
            uint32_t latest_start = usable_end - count;
            latest_start -= quick_mod(latest_start, stride);
            uint32_t usable_start = std::max(block_start, low_page);
            if (latest_start >= usable_start && latest_start + count <= block_end) {
                start_page = latest_start;
                break;
            }
        }
    } else {
        auto it = free_blocks_.lower_bound(low_page);
        // the block before may reach into the range
        if (it != free_blocks_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second > low_page) it = prev;
        }
        for (; it != free_blocks_.end(); ++it) {
            uint32_t block_start = it->first;
            uint32_t block_count = it->second;
            uint32_t block_end = block_start + block_count;
            if (block_start > high_page) break; // above the range
            if (block_count < count) continue;
            // the lowest aligned start in this block and the range
            uint32_t earliest = std::max(block_start, low_page);
            uint32_t aligned_start = (earliest + stride - 1) / stride * stride;
            if (aligned_start + count <= block_end && aligned_start + count <= high_page) {
                start_page = aligned_start;
                break;
            }
        }
    }
    if (start_page == UINT32_MAX) return false; // out of memory

    if (!host_alloc(start_page, count, type, protect)) return false;
    remove_free_block(start_page, count);
    for (uint32_t page = start_page; page < start_page + count; ++page) {
        PageEntry &entry = page_table_[page];
        entry.base_address = start_page;
        entry.region_page_count = count;
        entry.allocation_protect = protect;
        entry.current_protect = protect;
        entry.state = ALLOCATE_RESERVE | type;
    }
    *out = heap_base_ + (start_page << page_size_shift_);
    return true;
}

bool BaseHeap::alloc_system(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
                            uint32_t *out) {
    *out = 0;
    size = round_up(size, page_size_);
    alignment = round_up(alignment, page_size_);
    uint32_t low_address = heap_base_;
    if (heap_type_ == HeapType::virt) low_address = heap_base_ + heap_size_ - 0x10000000;
    return alloc_range(low_address, heap_base_ + (heap_size_ - 1), size, alignment, type, protect, top_down, out);
}

bool BaseHeap::decommit(uint32_t address, uint32_t size) {
    uint32_t count = page_count(size);
    uint32_t start_page = (address - heap_base_) / page_size_;
    uint32_t end_page = start_page + count - 1;
    uint32_t last_page = uint32_t(page_table_.size()) - 1;
    start_page = std::min(last_page, start_page);
    end_page = std::min(last_page, end_page);

    std::lock_guard guard(*lock_);
    // the host keeps the pages: a view of the section cannot be decommitted
    for (uint32_t page = start_page; page <= end_page; ++page) page_table_[page].state &= ~ALLOCATE_COMMIT;
    return true;
}

bool BaseHeap::release(uint32_t base_address, uint32_t *out_size) {
    std::lock_guard guard(*lock_);

    // the address must be a region's first page (Xenia reads past its table
    // outside the heap, and releases everything from a free first page)
    uint32_t base_page = (base_address - heap_base_) / page_size_;
    if (base_page >= page_table_.size()) return false;
    PageEntry base_entry = page_table_[base_page];
    if (!base_entry.state || base_entry.base_address != base_page) return false;
    if (heap_base_ == 0 && base_page == 0) return false; // the null pages
    if (out_size) *out_size = uint32_t(base_entry.region_page_count) * page_size_;

    // the host keeps the pages, committed and as they were protected
    for (uint32_t page = base_page; page < base_page + base_entry.region_page_count; ++page) {
        page_table_[page] = PageEntry{};
    }
    insert_free_block(base_page, base_entry.region_page_count);
    return true;
}

bool BaseHeap::protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old_protect) {
    if (!size) return false;
    protect &= 0xF;
    // as VirtualProtect: every page with a byte of the range, all in one
    // region and committed
    uint32_t start_page = (address - heap_base_) >> page_size_shift_;
    if (start_page >= page_table_.size()) return false;
    uint32_t end_page = uint32_t((uint64_t(address) + size - 1 - heap_base_) >> page_size_shift_);
    if (end_page >= page_table_.size()) return false;

    std::lock_guard guard(*lock_);

    uint32_t first_base = UINT32_MAX;
    for (uint32_t page = start_page; page <= end_page; ++page) {
        const PageEntry &entry = page_table_[page];
        if (first_base == UINT32_MAX)
            first_base = entry.base_address;
        else if (first_base != entry.base_address)
            return false;
        if (!(entry.state & ALLOCATE_COMMIT)) return false;
    }

    // the host's protection, where the range is on its pages (always with
    // 4 KB host pages); with larger host pages, smaller guest pages are left
    // as the views map them (Xenia gives each host page its guest pages'
    // most permissive access)
    uint32_t host_page = uint32_t(host_page_size());
    uint32_t mask = host_page - 1;
    uint32_t count = end_page - start_page + 1;
    if (page_size_ == host_page || ((host_address_offset_ & mask) == 0 && ((count << page_size_shift_) & mask) == 0 &&
                                    ((start_page << page_size_shift_) & mask) == 0)) {
        if (!host_protect(translate_relative(start_page << page_size_shift_), size_t(count) << page_size_shift_,
                          to_access(protect))) {
            return false;
        }
    } else if (page_size_ > host_page) {
        return false;
    }
    // Xenia reads the host's old protection, which only knows read and write
    if (old_protect) *old_protect = from_access(to_access(page_table_[start_page].current_protect));

    for (uint32_t page = start_page; page <= end_page; ++page) page_table_[page].current_protect = protect;
    return true;
}

bool BaseHeap::query(uint32_t address, RegionInfo &out) const {
    uint32_t start_page = (address - heap_base_) >> page_size_shift_;
    if (start_page >= page_table_.size()) return false; // (Xenia: one past its table)

    std::lock_guard guard(*lock_);

    PageEntry start_entry = page_table_[start_page];
    out = RegionInfo{};
    out.base_address = address;
    if (start_entry.state) {
        // reserved or committed: the pages from here alike in region, state
        // and protection
        out.allocation_base = heap_base_ + (uint32_t(start_entry.base_address) << page_size_shift_);
        out.allocation_protect = start_entry.allocation_protect;
        out.allocation_size = uint32_t(start_entry.region_page_count) << page_size_shift_;
        out.state = start_entry.state;
        out.protect = start_entry.current_protect;
        uint32_t end =
            std::min<uint32_t>(start_entry.base_address + start_entry.region_page_count, uint32_t(page_table_.size()));
        for (uint32_t page = start_page; page < end; ++page) {
            const PageEntry &entry = page_table_[page];
            if (entry.base_address != start_entry.base_address || entry.state != start_entry.state ||
                entry.current_protect != start_entry.current_protect) {
                break;
            }
            out.region_size += page_size_;
        }
    } else {
        // free: up to the next reserved page
        for (uint32_t page = start_page; page < page_table_.size(); ++page) {
            if (page_table_[page].state) break;
            out.region_size += page_size_;
        }
    }
    return true;
}

bool BaseHeap::query_size(uint32_t address, uint32_t *out_size) const {
    uint32_t page = (address - heap_base_) >> page_size_shift_;
    if (page >= page_table_.size()) {
        *out_size = 0;
        return false;
    }
    std::lock_guard guard(*lock_);
    *out_size = uint32_t(page_table_[page].region_page_count) << page_size_shift_;
    return true;
}

void BaseHeap::rebuild_free_blocks() {
    free_blocks_.clear();
    uint32_t run_start = UINT32_MAX;
    for (uint32_t page = 0; page < uint32_t(page_table_.size()); ++page) {
        if (!page_table_[page].state) {
            if (run_start == UINT32_MAX) run_start = page;
        } else if (run_start != UINT32_MAX) {
            free_blocks_[run_start] = page - run_start;
            run_start = UINT32_MAX;
        }
    }
    if (run_start != UINT32_MAX) free_blocks_[run_start] = uint32_t(page_table_.size()) - run_start;
}

void BaseHeap::remove_free_block(uint32_t start_page, uint32_t page_count) {
    if (free_blocks_.empty()) return;
    // the free block holding the range, split around it
    auto it = free_blocks_.upper_bound(start_page);
    if (it != free_blocks_.begin()) --it;
    uint32_t block_start = it->first;
    uint32_t block_end = block_start + it->second;
    free_blocks_.erase(it);
    if (block_start < start_page) free_blocks_[block_start] = start_page - block_start;
    uint32_t alloc_end = start_page + page_count;
    if (alloc_end < block_end) free_blocks_[alloc_end] = block_end - alloc_end;
}

void BaseHeap::insert_free_block(uint32_t start_page, uint32_t page_count) {
    uint32_t new_start = start_page;
    uint32_t new_count = page_count;
    // merged with the blocks right after and right before
    auto after = free_blocks_.find(start_page + page_count);
    if (after != free_blocks_.end()) {
        new_count += after->second;
        free_blocks_.erase(after);
    }
    auto at = free_blocks_.lower_bound(start_page);
    if (at != free_blocks_.begin()) {
        auto before = std::prev(at);
        if (before->first + before->second == start_page) {
            new_start = before->first;
            new_count += before->second;
            free_blocks_.erase(before);
        }
    }
    free_blocks_[new_start] = new_count;
}

class Watches;

// A window on the physical memory (Xenia's PhysicalHeap): its pages are
// taken from the physical heap (its parent, 0-512 MB, 4 KB pages) and pinned
// at the same offset here, so every window sees the same physical page.
class PhysicalHeap final : public BaseHeap {
public:
    void init(std::recursive_mutex *lock, uint8_t *membase, uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
              BaseHeap *parent, Watches *watches) {
        // the 0xE0000000 window's view starts on the 64 KB granularity, at
        // physical 0, and the window at physical 0x1000: 4 KB on on the host
        BaseHeap::init(lock, membase, HeapType::physical, heap_base, heap_size, page_size,
                       heap_base >= 0xE0000000 ? 0x1000 : 0);
        parent_ = parent;
        watches_ = watches;
    }

    // the physical memory the window shows, [physical_first, physical_end)
    uint32_t physical_first() const { return heap_base_ >= 0xE0000000 ? 0x1000 : 0; }
    uint32_t physical_end() const { return physical_first() + heap_size_; }
    // a physical address's host address in this window's view (the same
    // offset from the view's start in every window: the 0xE0000000 one starts
    // at physical 0)
    uint8_t *host_of(uint32_t physical) const { return membase_ + heap_base_ + physical; }
    // whether the guest may write all of [first, end) here (physical, within
    // the window): its pages committed and writable (Xenia's
    // SystemPageGuestAccess being read-write); lock held
    bool guest_writable(uint32_t first, uint32_t end) const {
        uint32_t from = (first - physical_first()) >> page_size_shift_;
        uint32_t to = (end - 1 - physical_first()) >> page_size_shift_;
        for (uint32_t page = from; page <= to; ++page) {
            const PageEntry &entry = page_table_[page];
            if (!(entry.state & ALLOCATE_COMMIT) || to_access(entry.current_protect) != Access::read_write)
                return false;
        }
        return true;
    }

    // the physical address of an address in this window
    uint32_t physical_address(uint32_t address) const {
        address -= heap_base_;
        if (heap_base_ >= 0xE0000000) address += 0x1000;
        return address;
    }

    bool alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
               uint32_t *out) override;
    bool alloc_fixed(uint32_t base_address, uint32_t size, uint32_t alignment, uint32_t type,
                     uint32_t protect) override;
    bool alloc_range(uint32_t low_address, uint32_t high_address, uint32_t size, uint32_t alignment, uint32_t type,
                     uint32_t protect, bool top_down, uint32_t *out) override;
    bool alloc_system(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool top_down,
                      uint32_t *out) override {
        return alloc(size, alignment, type, protect, top_down, out);
    }
    bool release(uint32_t base_address, uint32_t *out_size = nullptr) override;
    bool decommit(uint32_t address, uint32_t size) override;
    bool protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old_protect = nullptr) override;

private:
    // the size bytes the parent gave, at parent_address, pinned here: the
    // same pages (Xenia pins with the alignment, which rounds the size up to
    // it, so an alignment above the size reserved more here than in the
    // parent, over the next allocations' pages)
    bool pin(uint32_t parent_address, uint32_t parent_base, uint32_t size, uint32_t alignment, uint32_t type,
             uint32_t protect, uint32_t *out);
    // the watched pages of a guest range here written: their protection
    // comes off before the heap changes it (lock held)
    void invalidate(uint32_t address, uint32_t size);

    BaseHeap *parent_ = nullptr;
    Watches *watches_ = nullptr;
};

// ---------------------------------------------------------------------------
// Write watches on physical memory (memory.h; Xenia's PhysicalHeap
// EnableAccessCallbacks and TriggerCallbacks, with SharedMemory's
// MemoryInvalidationCallback asking how far to go as the watchers' bounds).
// Xenia keeps the watch bits in each window's heap; here they are by
// physical page, each window's protection apart: a physical page is watched
// in every window where the guest may write it, and a change to a window's
// page table (the heaps' protect, alloc, decommit, release) takes the watch
// off first, so the windows' bits stay true.

class Watches {
public:
    void init(std::recursive_mutex *lock, std::array<PhysicalHeap *, 4> windows) {
        lock_ = lock;
        windows_ = windows;
        shift_ = 12;
        while ((size_t(1) << shift_) < host_page_size()) shift_++;
        pages_ = 0x20000000u >> shift_;
        stamps_ = std::make_unique<std::atomic<uint32_t>[]>(pages_);
        for (uint32_t p = 0; p < pages_; p++) stamps_[p].store(0, std::memory_order_relaxed);
        uint32_t words = (pages_ + 63) / 64;
        watched_.assign(words, 0);
        for (auto &bits : protected_) bits.assign(words, 0);
    }

    uint32_t shift() const { return shift_; }
    uint32_t count() const { return count_.load(std::memory_order_acquire); }
    const std::atomic<uint32_t> *stamps() const { return stamps_.get(); }

    // EnableAccessCallbacks for invalidation notifications: each page not
    // watched yet made read-only in the windows where the guest may write it
    void watch(uint32_t physical, uint32_t length) {
        if (!length || physical >= 0x20000000u) return;
        length = std::min(length, 0x20000000u - physical);
        uint32_t first = physical >> shift_, last = (physical + length - 1) >> shift_;
        std::lock_guard guard(*lock_);
        // runs of pages to protect, by window
        std::array<uint32_t, 4> run;
        run.fill(UINT32_MAX);
        auto flush = [&](size_t w, uint32_t end) {
            if (run[w] == UINT32_MAX) return;
            host_protect(windows_[w]->host_of(run[w] << shift_), size_t(end - run[w]) << shift_, Access::read_only);
            run[w] = UINT32_MAX;
        };
        for (uint32_t p = first; p <= last; p++) {
            // a whole word watched: its 64 pages at once
            if ((p & 63) == 0 && p + 63 <= last && watched_[p >> 6] == ~0ull) {
                for (size_t w = 0; w < windows_.size(); w++) flush(w, p);
                p += 63;
                continue;
            }
            bool already = bit(watched_, p), any = false;
            for (size_t w = 0; w < windows_.size(); w++) {
                uint32_t start, end;
                bool take = !already && window_part(w, p, start, end) && windows_[w]->guest_writable(start, end);
                if (!take) {
                    flush(w, p);
                    continue;
                }
                if (run[w] == UINT32_MAX) run[w] = p;
                set(protected_[w], p, true);
                any = true;
            }
            if (any) {
                set(watched_, p, true);
                counts_.watched++;
            }
        }
        for (size_t w = 0; w < windows_.size(); w++) flush(w, last + 1);
    }

    // TriggerCallbacks for a write: the pages numbered as written, and, where
    // watched, their protection back to what the guest has; widened as the
    // watchers allow for a fault (Xenia's unwatch range, at most 4 MB around)
    void written(uint32_t physical, uint32_t length, bool widen) {
        if (!length || physical >= 0x20000000u) return;
        length = std::min(length, 0x20000000u - physical);
        uint32_t first = physical >> shift_, last = (physical + length - 1) >> shift_;
        std::lock_guard guard(*lock_);
        if (widen) {
            uint32_t from = 0, to = pages_ - 1;
            bool asked = false;
            for (const Watcher &w : watchers_) {
                if (!w.bounds) continue;
                auto [a, b] = w.bounds(w.context, first, last);
                from = std::max(from, a), to = std::min(to, b);
                asked = true;
            }
            if (asked) {
                uint32_t block = (4u << 20) >> shift_;
                from = std::max(from, first & ~(block - 1));
                to = std::min(to, last | (block - 1));
                first = std::min(first, from), last = std::max(last, to);
            }
        }
        uint32_t stamp = count_.load(std::memory_order_relaxed) + 1;
        for (uint32_t p = first; p <= last; p++) stamps_[p].store(stamp, std::memory_order_release);
        count_.store(stamp, std::memory_order_release);
        counts_.written += last - first + 1;
        for (size_t w = 0; w < windows_.size(); w++) {
            uint32_t run = UINT32_MAX;
            for (uint32_t p = first; p <= last + 1; p++) {
                bool take = false;
                if (p <= last && bit(protected_[w], p)) {
                    set(protected_[w], p, false);
                    // a page the guest made read-only since keeps its protection
                    uint32_t start, end;
                    take = window_part(w, p, start, end) && windows_[w]->guest_writable(start, end);
                }
                if (take && run == UINT32_MAX) run = p;
                if (!take && run != UINT32_MAX) {
                    host_protect(windows_[w]->host_of(run << shift_), size_t(p - run) << shift_, Access::read_write);
                    run = UINT32_MAX;
                }
            }
        }
        for (uint32_t p = first; p <= last; p++) set(watched_, p, false);
    }

    // AccessViolationCallback: a write fault at a host address; true when it
    // was a watch's (the write goes on)
    bool fault(uintptr_t host, uint8_t *membase) {
        uintptr_t base = uintptr_t(membase);
        if (host < base || host - base >= (uint64_t(1) << 32)) return false;
        uint64_t offset = host - base;
        for (size_t w = 0; w < windows_.size(); w++) {
            const PhysicalHeap &heap = *windows_[w];
            uint64_t start = heap.heap_base() + heap.physical_first(), end = heap.heap_base() + heap.physical_end();
            if (offset < start || offset >= end) continue;
            uint32_t physical = uint32_t(offset - heap.heap_base());
            uint32_t p = physical >> shift_;
            std::lock_guard guard(*lock_);
            counts_.faults++;
            if (bit(protected_[w], p)) {
                written(physical, 1, true);
                return true;
            }
            // not watched here (any more): another thread's fault took the
            // watch off; the write goes on if the guest may write the page
            uint32_t part_start, part_end;
            if (!window_part(w, p, part_start, part_end) || !heap.guest_writable(part_start, part_end)) {
                counts_.faults--;
                return false;
            }
            host_protect(heap.host_of(p << shift_), size_t(1) << shift_, Access::read_write);
            counts_.retried++;
            return true;
        }
        return false;
    }

    void *add(Memory::WatchBounds bounds, void *context) {
        std::lock_guard guard(*lock_);
        watchers_.push_back({bounds, context, ++next_handle_});
        return reinterpret_cast<void *>(uintptr_t(next_handle_));
    }
    void remove(void *handle) {
        std::lock_guard guard(*lock_);
        std::erase_if(watchers_, [&](const Watcher &w) { return w.handle == uintptr_t(handle); });
    }
    // (none while another thread holds the lock: a fault's report may ask)
    Memory::WatchCounts counts() const {
        std::unique_lock guard(*lock_, std::try_to_lock);
        return guard ? counts_ : Memory::WatchCounts{};
    }

private:
    struct Watcher {
        Memory::WatchBounds bounds;
        void *context;
        uintptr_t handle;
    };
    static bool bit(const std::vector<uint64_t> &bits, uint32_t p) { return (bits[p >> 6] >> (p & 63)) & 1; }
    static void set(std::vector<uint64_t> &bits, uint32_t p, bool on) {
        if (on)
            bits[p >> 6] |= 1ull << (p & 63);
        else
            bits[p >> 6] &= ~(1ull << (p & 63));
    }
    // the part of watch page p window w shows, physical [start, end); false
    // when it shows none
    bool window_part(size_t w, uint32_t p, uint32_t &start, uint32_t &end) const {
        const PhysicalHeap &heap = *windows_[w];
        start = std::max(p << shift_, heap.physical_first());
        end = uint32_t(std::min<uint64_t>(uint64_t(p + 1) << shift_, heap.physical_end()));
        return start < end;
    }

    std::recursive_mutex *lock_ = nullptr; // the heaps' (Xenia's global critical region)
    std::array<PhysicalHeap *, 4> windows_{};
    uint32_t shift_ = 12, pages_ = 0;
    std::unique_ptr<std::atomic<uint32_t>[]> stamps_;
    std::atomic<uint32_t> count_{1};
    std::vector<uint64_t> watched_;                 // by physical page: protected in some window
    std::array<std::vector<uint64_t>, 4> protected_; // by window and physical page: made read-only here
    std::vector<Watcher> watchers_;
    uintptr_t next_handle_ = 0;
    Memory::WatchCounts counts_;
};

bool PhysicalHeap::pin(uint32_t parent_address, uint32_t parent_base, uint32_t size, uint32_t alignment, uint32_t type,
                       uint32_t protect, uint32_t *out) {
    uint32_t address = heap_base_ + parent_address - parent_base;
    // the alignment is the host address's (the physical address's)
    if ((address + host_address_offset_) % alignment != 0) {
        parent_->release(parent_address);
        return false;
    }
    // pages committed here already may be taken again: their watches go
    // before the commit sets their protection
    invalidate(address, size);
    if (!BaseHeap::alloc_fixed(address, size, page_size_, type, protect)) {
        parent_->release(parent_address);
        return false;
    }
    if (out) *out = address;
    return true;
}

void PhysicalHeap::invalidate(uint32_t address, uint32_t size) {
    if (!watches_ || !size || address < heap_base_ || address - heap_base_ >= heap_size_) return;
    size = std::min(size, heap_size_ - (address - heap_base_));
    watches_->written(physical_address(address), size, false);
}

bool PhysicalHeap::alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool, uint32_t *out) {
    *out = 0;
    // always top-down: the parent heap is bottom-up, so they do not collide
    size = round_up(size, page_size_);
    alignment = round_up(alignment, page_size_);

    std::lock_guard guard(*lock_);
    uint32_t parent_start = physical_address(heap_base_);
    uint32_t parent_end = physical_address(heap_base_ + (heap_size_ - 1));
    uint32_t parent_address;
    if (!parent_->alloc_range(parent_start, parent_end, size, alignment, type, protect, true, &parent_address)) {
        return false;
    }
    return pin(parent_address, parent_start, size, alignment, type, protect, out);
}

bool PhysicalHeap::alloc_fixed(uint32_t base_address, uint32_t size, uint32_t alignment, uint32_t type,
                               uint32_t protect) {
    size = round_up(size, page_size_);
    alignment = round_up(alignment, page_size_);
    // fixed allocations take the size up to the alignment, here and in the
    // parent alike
    size = align(size, alignment);

    std::lock_guard guard(*lock_);
    // (pages already committed in the parent may be taken again, as in Xenia)
    uint32_t parent_address = physical_address(base_address);
    if (!parent_->alloc_fixed(parent_address, size, alignment, type, protect)) return false;
    return pin(parent_address, physical_address(heap_base_), size, alignment, type, protect, nullptr);
}

bool PhysicalHeap::alloc_range(uint32_t low_address, uint32_t high_address, uint32_t size, uint32_t alignment,
                               uint32_t type, uint32_t protect, bool top_down, uint32_t *out) {
    *out = 0;
    size = round_up(size, page_size_);
    alignment = round_up(alignment, page_size_);

    std::lock_guard guard(*lock_);
    low_address = std::max(heap_base_, low_address);
    high_address = std::min(heap_base_ + (heap_size_ - 1), high_address);
    uint32_t parent_address;
    if (!parent_->alloc_range(physical_address(low_address), physical_address(high_address), size, alignment, type,
                              protect, top_down, &parent_address)) {
        return false;
    }
    return pin(parent_address, physical_address(heap_base_), size, alignment, type, protect, out);
}

bool PhysicalHeap::release(uint32_t base_address, uint32_t *out_size) {
    std::lock_guard guard(*lock_);
    if (!parent_->release(physical_address(base_address), out_size)) return false;
    // the range may be used again by another window's mapping, so what
    // watches it here goes (Xenia: issue 1559)
    uint32_t region_size;
    if (query_size(base_address, &region_size)) invalidate(base_address, region_size);
    return BaseHeap::release(base_address, out_size);
}

bool PhysicalHeap::decommit(uint32_t address, uint32_t size) {
    std::lock_guard guard(*lock_);
    if (!parent_->decommit(physical_address(address), size)) return false;
    invalidate(address, size); // not caring about the contents any more
    return BaseHeap::decommit(address, size);
}

bool PhysicalHeap::protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old_protect) {
    std::lock_guard guard(*lock_);
    // made writable: the guest announces a write, so the pages are written
    // even unwatched (a page read-only when it was last copied never got a
    // watch), as Xenia invalidates them
    if (to_access(protect) == Access::read_write) invalidate(address, size);
    if (!parent_->protect(physical_address(address), size, protect, old_protect)) return false;
    return BaseHeap::protect(address, size, protect);
}

// ---------------------------------------------------------------------------
// The memory

// Windows maps views on 64 KB; Xenia masks each view's offset with it, so
// the 0xE0000000 view starts at physical 0 and its window 4 KB in. The same
// mask on every host keeps translate()'s 0x1000 true everywhere.
constexpr uint64_t view_granularity = 0x10000;

// the views of the one section (Xenia's map_info): guest range, offset in
// the section (0x100000000 and up: the physical memory)
constexpr struct {
    uint64_t start, end, target;
} map_info[] = {
    {0x00000000, 0x3FFFFFFF, 0x000000000},   // virtual, 4 KB pages
    {0x40000000, 0x7EFFFFFF, 0x040000000},   // virtual, 64 KB pages
    {0x7F000000, 0x7FFFFFFF, 0x100000000},   // GPU writeback and XPS (and the MMIO range)
    {0x80000000, 0x8FFFFFFF, 0x080000000},   // XEX, 64 KB pages
    {0x90000000, 0x9FFFFFFF, 0x080000000},   // XEX, 4 KB pages: the same memory as 0x80000000, as in Xenia
    {0xA0000000, 0xBFFFFFFF, 0x100000000},   // physical, 64 KB pages
    {0xC0000000, 0xDFFFFFFF, 0x100000000},   // physical, 16 MB pages
    {0xE0000000, 0xFFFFFFFF, 0x100001000},   // physical, 4 KB pages
    {0x100000000, 0x11FFFFFFF, 0x100000000}, // physical, raw (translate_physical)
};
constexpr size_t view_count = sizeof(map_info) / sizeof(map_info[0]);
constexpr uint64_t section_size = 0x120000000; // 4 GB and the 512 MB of physical memory

// Memory::PrivateView: the same views of the section, copy on write, in room
// the host gives (reserve_private), each view in its own part of it
class PrivateViewImpl final : public Memory::PrivateView {
public:
    explicit PrivateViewImpl(MappingHandle mapping) : mapping_(mapping) {}
    ~PrivateViewImpl() override { release(); }

    bool map() {
        room_ = reserve_private(size_t(section_size));
        if (!room_) return false;
        // (the views cover [0, section_size) one after the other; the last
        // is what the cuts leave)
        for (size_t n = 0; n + 1 < view_count; n++)
            if (!split_private(room_ + map_info[n].start, length(n))) return release(), false;
        for (size_t n = 0; n < view_count; n++)
            if (!(mapped_[n] = map_private_view(mapping_, room_ + map_info[n].start, length(n), offset(n)) != nullptr))
                return release(), false;
        base_ = room_;
        return true;
    }

    bool reset(uint32_t address, uint32_t size) override {
        if (!base_) return false;
        // the host offsets the guest range reaches (translate's)
        uint64_t first = uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u);
        uint64_t last = first + (size ? size - 1 : 0);
        for (size_t n = 0; n < view_count; n++) {
            if (last < map_info[n].start || first > map_info[n].end) continue;
            mapped_[n] = remap_private_view(mapping_, room_ + map_info[n].start, length(n), offset(n)) != nullptr;
            if (!mapped_[n]) return release(), false;
        }
        return true;
    }

private:
    static size_t length(size_t n) { return size_t(map_info[n].end - map_info[n].start + 1); }
    static uint64_t offset(size_t n) { return map_info[n].target & ~(view_granularity - 1); }
    void release() {
        if (room_)
            for (size_t n = 0; n < view_count; n++) release_private(room_ + map_info[n].start, length(n), mapped_[n]);
        room_ = base_ = nullptr;
        for (bool &m : mapped_) m = false;
    }

    MappingHandle mapping_;
    uint8_t *room_ = nullptr;
    bool mapped_[view_count] = {};
};

void store_be32(uint8_t *p, uint32_t value) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

// Every Memory whose watches the process's fault handler asks, in a table
// the handler reads without a lock (a fault may come on a thread holding
// any lock).
class MemoryImpl;
constexpr size_t MAX_MEMORIES = 16;
std::atomic<MemoryImpl *> memories[MAX_MEMORIES];
bool on_write_fault(void *host_address);

class MemoryImpl final : public Memory {
public:
    MemoryImpl() = default;
    MemoryImpl(const MemoryImpl &) = delete;
    MemoryImpl &operator=(const MemoryImpl &) = delete;
    ~MemoryImpl() override {
        for (auto &slot : memories) {
            MemoryImpl *self = this;
            slot.compare_exchange_strong(self, nullptr);
        }
        unmap_views();
        if (has_mapping_) close_mapping(mapping_);
    }

    uint32_t watch_page_shift() const override { return watches_.shift(); }
    uint32_t write_count() const override { return watches_.count(); }
    const std::atomic<uint32_t> *write_stamps() const override { return watches_.stamps(); }
    void watch(uint32_t physical, uint32_t length) override {
        // the fault handler, the first time anything is watched: added then,
        // it comes before the handlers added at the game's launch
        static std::once_flag installed;
        std::call_once(installed, [] { install_write_fault_handler(on_write_fault); });
        watches_.watch(physical, length);
    }
    void physical_written(uint32_t physical, uint32_t length) override { watches_.written(physical, length, false); }
    void *add_watcher(WatchBounds bounds, void *context) override { return watches_.add(bounds, context); }
    void remove_watcher(void *handle) override { watches_.remove(handle); }
    WatchCounts watch_counts() const override { return watches_.counts(); }
    bool fault(void *host_address) { return watches_.fault(uintptr_t(host_address), virtual_membase_); }

    bool init(std::string &error);

    uint8_t *base() const override { return virtual_membase_; }
    uint8_t *translate_physical(uint32_t physical) const override {
        return physical_membase_ + (physical & 0x1FFFFFFF);
    }
    uint32_t physical_address(uint32_t address) const override;
    Heap *heap_for(uint32_t address) override { return const_cast<BaseHeap *>(lookup_heap(address)); }
    Heap *heap_by_type(bool physical, uint32_t page_size) override { return lookup_heap_by_type(physical, page_size); }
    uint32_t system_alloc(uint32_t size, uint32_t alignment, bool physical) override;
    void system_free(uint32_t address) override;
    std::unique_ptr<PrivateView> private_view(std::string &error) override {
        auto view = std::make_unique<PrivateViewImpl>(mapping_);
        if (view->map()) return view;
        error = "the host could not map a private view of the guest memory";
        return nullptr;
    }

private:
    bool map_views(uint8_t *mapping_base);
    void unmap_views();
    const BaseHeap *lookup_heap(uint32_t address) const;
    BaseHeap *lookup_heap_by_type(bool physical, uint32_t page_size);

    MappingHandle mapping_{};
    bool has_mapping_ = false;
    uint8_t *views_[view_count] = {};
    uint8_t *virtual_membase_ = nullptr;
    uint8_t *physical_membase_ = nullptr;
    std::recursive_mutex lock_;
    Watches watches_;
    struct {
        BaseHeap v00000000;
        BaseHeap v40000000;
        BaseHeap v80000000;
        BaseHeap v90000000;
        BaseHeap physical;
        PhysicalHeap v7F000000;
        PhysicalHeap vA0000000;
        PhysicalHeap vC0000000;
        PhysicalHeap vE0000000;
    } heaps_;
};

bool MemoryImpl::map_views(uint8_t *mapping_base) {
    for (size_t n = 0; n < view_count; n++) {
        views_[n] =
            map_view(mapping_, mapping_base + map_info[n].start, size_t(map_info[n].end - map_info[n].start + 1),
                     map_info[n].target & ~(view_granularity - 1));
        if (!views_[n]) {
            unmap_views();
            return false;
        }
    }
    return true;
}

void MemoryImpl::unmap_views() {
    for (size_t n = 0; n < view_count; n++) {
        if (views_[n]) unmap_view(views_[n], size_t(map_info[n].end - map_info[n].start + 1));
        views_[n] = nullptr;
    }
}

bool MemoryImpl::init(std::string &error) {
    // one section for everything, reserved, not committed
    if (!create_mapping(section_size, mapping_)) {
        error = "cannot create the guest memory's section";
        return false;
    }
    has_mapping_ = true;

    // the views where the host has room, trying a few bases
    for (int n = 32; n < 64 && !virtual_membase_; n++) {
        auto *mapping_base = reinterpret_cast<uint8_t *>(uintptr_t(1) << n);
        if (map_views(mapping_base)) virtual_membase_ = mapping_base;
    }
    if (!virtual_membase_) {
        error = "no room in the host's address space for the guest memory's views";
        return false;
    }
    physical_membase_ = virtual_membase_ + 0x100000000ull;

    // the virtual heaps
    heaps_.v00000000.init(&lock_, virtual_membase_, HeapType::virt, 0x00000000, 0x40000000, 0x1000);
    heaps_.v40000000.init(&lock_, virtual_membase_, HeapType::virt, 0x40000000, 0x40000000 - 0x01000000, 0x10000);
    heaps_.v80000000.init(&lock_, virtual_membase_, HeapType::xex, 0x80000000, 0x10000000, 0x10000);
    heaps_.v90000000.init(&lock_, virtual_membase_, HeapType::xex, 0x90000000, 0x10000000, 0x1000);

    // the physical memory and its windows
    heaps_.physical.init(&lock_, physical_membase_, HeapType::physical, 0x00000000, 0x20000000, 0x1000);
    heaps_.vA0000000.init(&lock_, virtual_membase_, 0xA0000000, 0x20000000, 0x10000, &heaps_.physical, &watches_);
    heaps_.vC0000000.init(&lock_, virtual_membase_, 0xC0000000, 0x20000000, 0x1000000, &heaps_.physical, &watches_);
    heaps_.vE0000000.init(&lock_, virtual_membase_, 0xE0000000, 0x1FD00000, 0x1000, &heaps_.physical, &watches_);
    heaps_.v7F000000.init(&lock_, virtual_membase_, 0x7F000000, 0x00C80000, 0x1000, &heaps_.physical, &watches_);
    watches_.init(&lock_, {&heaps_.v7F000000, &heaps_.vA0000000, &heaps_.vC0000000, &heaps_.vE0000000});

    bool ok = true;
    // the first 64 KB, no access (Xenia's protect_zero), and the physical
    // memory's last 64 KB, reserved
    ok &= heaps_.v00000000.alloc_fixed(0x00000000, 0x10000, 0x10000, ALLOCATE_RESERVE | ALLOCATE_COMMIT, 0);
    ok &= heaps_.physical.alloc_fixed(0x1FFF0000, 0x10000, 0x10000, ALLOCATE_RESERVE, 0);
    // the GPU writeback and XPS: 0xC0000000 is physical, 0x7F000000 virtual,
    // the same memory
    ok &= heaps_.vC0000000.alloc_fixed(0xC0000000, 0x01000000, 32, ALLOCATE_RESERVE | ALLOCATE_COMMIT,
                                       PROTECT_READ_WRITE);
    ok &= heaps_.v7F000000.alloc_fixed(0x7F000000, 0x00C80000, 32, ALLOCATE_RESERVE | ALLOCATE_COMMIT,
                                       PROTECT_READ_WRITE);
    // the GPU sees all of the physical memory, so all of it is committed;
    // the page tables keep the allocations
    ok &= host_commit(heaps_.physical.translate_relative(0x01000000), 0x1F000000, Access::read_write);
    // the MMIO range (the GPU's registers, the audio's) is not mapped here:
    // no access, so an access faults (Xenia commits its ranges no-access and
    // handles the faults)
    ok &= host_commit(translate(0x7FC80000), 0x00380000, Access::none);
    if (!ok) {
        error = "cannot make the guest memory's fixed allocations";
        return false;
    }

    // a reservation Xenia makes and does not explain
    uint32_t unknown;
    heaps_.vA0000000.alloc(0x340000, 0x10000, ALLOCATE_RESERVE, 0, true, &unknown);
    // the memory the hypervisor and the kernel would use
    uint32_t hypervisor;
    heaps_.v80000000.alloc(0x40000, 0x1000, ALLOCATE_COMMIT, PROTECT_READ_WRITE, false, &hypervisor);
    // a value a game (544307D5) reads there; the same on every run
    store_be32(translate(0x8000001C), 0x2A6E3F38);
    return true;
}

const BaseHeap *MemoryImpl::lookup_heap(uint32_t address) const {
    if (address < 0x40000000) return &heaps_.v00000000;
    if (address < 0x7F000000) return &heaps_.v40000000;
    if (address < 0x7FC80000) return &heaps_.v7F000000;
    if (address < 0x80000000) return nullptr;
    if (address < 0x90000000) return &heaps_.v80000000;
    if (address < 0xA0000000) return &heaps_.v90000000;
    if (address < 0xC0000000) return &heaps_.vA0000000;
    if (address < 0xE0000000) return &heaps_.vC0000000;
    if (address < 0xFFD00000) return &heaps_.vE0000000;
    return nullptr;
}

BaseHeap *MemoryImpl::lookup_heap_by_type(bool physical, uint32_t page_size) {
    if (physical) {
        if (page_size <= 0x1000) return &heaps_.vE0000000;
        if (page_size <= 0x10000) return &heaps_.vA0000000;
        return &heaps_.vC0000000;
    }
    return page_size <= 0x1000 ? &heaps_.v00000000 : &heaps_.v40000000;
}

uint32_t MemoryImpl::physical_address(uint32_t address) const {
    const BaseHeap *heap = lookup_heap(address);
    if (!heap) return UINT32_MAX;
    // outside the physical windows Xenia gives the address's offset in its
    // heap (the address itself in the 4 KB virtual heap)
    if (heap->heap_type() != HeapType::physical) return address - heap->heap_base();
    return static_cast<const PhysicalHeap *>(heap)->physical_address(address);
}

uint32_t MemoryImpl::system_alloc(uint32_t size, uint32_t alignment, bool physical) {
    BaseHeap *heap = lookup_heap_by_type(physical, 0x1000);
    uint32_t address;
    if (!heap->alloc_system(size, alignment, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE, false, &address)) {
        return 0;
    }
    std::memset(translate(address), 0, size);
    return address;
}

void MemoryImpl::system_free(uint32_t address) {
    if (!address) return;
    if (Heap *heap = heap_for(address)) heap->release(address);
}

bool on_write_fault(void *host_address) {
    for (auto &slot : memories)
        if (MemoryImpl *m = slot.load(std::memory_order_acquire); m && m->fault(host_address)) return true;
    return false;
}

} // namespace

std::unique_ptr<Memory> Memory::create(std::string &error) {
    auto memory = std::make_unique<MemoryImpl>();
    if (!memory->init(error)) return nullptr;
    bool listed = false;
    for (auto &slot : memories) {
        MemoryImpl *none = nullptr;
        if (slot.compare_exchange_strong(none, memory.get())) {
            listed = true;
            break;
        }
    }
    if (!listed) {
        error = "too many guest memories at once for the write watches' fault handler";
        return nullptr;
    }
    return memory;
}

} // namespace xf
