/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The memory family (plan step A4; docs/runtime.md): virtual and physical
// memory, the pools, the kernel stacks. Translated from Xenia's
// src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc (BSD; see LICENSE-xenia) onto
// memory.h's heaps; checked against it export by export. Where Xenia would
// dereference a heap that does not exist (an address outside every heap),
// these answer as for an invalid parameter instead.

#include "xenoforge/kernel.h"

#include "stacks.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace xf::kernel {
namespace {

// xboxkrnl's MEM_* and PAGE_* (Xenia's kernel.h)
constexpr uint32_t X_MEM_COMMIT = 0x00001000;
constexpr uint32_t X_MEM_RESERVE = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t X_MEM_FREE = 0x00010000;
constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
constexpr uint32_t X_MEM_RESET = 0x00080000;
constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t X_MEM_NOZERO = 0x00800000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

constexpr uint32_t X_PAGE_NOACCESS = 0x00000001;
constexpr uint32_t X_PAGE_READONLY = 0x00000002;
constexpr uint32_t X_PAGE_READWRITE = 0x00000004;
constexpr uint32_t X_PAGE_EXECUTE = 0x00000010;
constexpr uint32_t X_PAGE_EXECUTE_READ = 0x00000020;
constexpr uint32_t X_PAGE_EXECUTE_READWRITE = 0x00000040;
constexpr uint32_t X_PAGE_EXECUTE_WRITECOPY = 0x00000080;
constexpr uint32_t X_PAGE_NOCACHE = 0x00000200;
constexpr uint32_t X_PAGE_WRITECOMBINE = 0x00000400;

constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t X_STATUS_INVALID_PAGE_PROTECTION = 0xC0000045;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;

constexpr uint32_t RC = ALLOCATE_RESERVE | ALLOCATE_COMMIT;

// Xenia's HeapType, from where the heap is (memory.h's map)
enum class HeapType { Virtual, Xex, Physical };
HeapType heap_type(const Heap *heap) {
    uint32_t base = heap->heap_base();
    if (base == 0x00000000 || base == 0x40000000) return HeapType::Virtual;
    if (base == 0x80000000 || base == 0x90000000) return HeapType::Xex;
    return HeapType::Physical;
}

// xe::round_up: zero rounds up to one multiple
uint32_t round_up(uint32_t value, uint32_t multiple) {
    return value ? (value + multiple - 1) / multiple * multiple : multiple;
}

uint32_t sat_sub(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }

[[gnu::format(printf, 1, 2)]] void log(const char *format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fprintf(stderr, "kernel: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

uint32_t to_xdk_protect(uint32_t protect) {
    uint32_t result;
    if (!(protect & PROTECT_READ) && !(protect & PROTECT_WRITE))
        result = X_PAGE_NOACCESS;
    else if ((protect & PROTECT_READ) && !(protect & PROTECT_WRITE))
        result = X_PAGE_READONLY;
    else
        result = X_PAGE_READWRITE;
    if (protect & PROTECT_NO_CACHE) result |= X_PAGE_NOCACHE;
    if (protect & PROTECT_WRITE_COMBINE) result |= X_PAGE_WRITECOMBINE;
    return result;
}

uint32_t from_xdk_protect(uint32_t protect) {
    uint32_t result = 0;
    if ((protect & X_PAGE_READONLY) | (protect & X_PAGE_EXECUTE_READ))
        result = PROTECT_READ;
    else if ((protect & X_PAGE_READWRITE) | (protect & X_PAGE_EXECUTE_READWRITE))
        result = PROTECT_READ_WRITE;
    if (protect & X_PAGE_NOCACHE) result |= PROTECT_NO_CACHE;
    if (protect & X_PAGE_WRITECOMBINE) result |= PROTECT_WRITE_COMBINE;
    return result;
}

// xeMmAllocatePhysicalMemoryEx
uint32_t allocate_physical(Kernel &k, uint32_t flags, uint32_t region_size, uint32_t protect_bits,
                           uint32_t min_addr_range, uint32_t max_addr_range, uint32_t alignment) {
    // a missing access mode is read/write (a title asks for its front
    // buffer with only X_MEM_LARGE_PAGES | X_PAGE_WRITECOMBINE)
    constexpr uint32_t access_bits =
        X_PAGE_READONLY | X_PAGE_READWRITE | X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE;
    if (!(protect_bits & access_bits)) {
        log("MmAllocatePhysicalMemoryEx: no access mode in protection bits %08X (flags %08X, size %08X), assuming "
            "read/write",
            protect_bits, flags, region_size);
        protect_bits |= X_PAGE_READWRITE;
    }
    uint32_t page_size = 4 * 1024;
    if (protect_bits & X_MEM_LARGE_PAGES)
        page_size = 64 * 1024;
    else if (protect_bits & X_MEM_16MB_PAGES)
        page_size = 16 * 1024 * 1024;

    uint32_t adjusted_size = round_up(region_size, page_size);
    uint32_t adjusted_alignment = round_up(alignment, page_size);
    uint32_t protect = from_xdk_protect(protect_bits);
    Heap *heap = k.memory().heap_by_type(true, page_size);
    // the range is in physical memory, not in the window
    uint32_t heap_base = heap->heap_base();
    uint32_t heap_physical_address_offset = k.memory().physical_address(heap_base);
    uint32_t heap_min_addr = sat_sub(min_addr_range, heap_physical_address_offset);
    uint32_t heap_max_addr = sat_sub(max_addr_range, heap_physical_address_offset);
    uint32_t heap_size = heap->heap_size();
    heap_min_addr = heap_base + std::min(heap_min_addr, heap_size - 1);
    heap_max_addr = heap_base + std::min(heap_max_addr, heap_size - 1);
    uint32_t base_address = 0;
    if (!heap->alloc_range(heap_min_addr, heap_max_addr, adjusted_size, adjusted_alignment, RC, protect, true,
                           &base_address)) {
        log("MmAllocatePhysicalMemoryEx: allocation failed, size %08X", adjusted_size);
        return 0;
    }
    return base_address;
}

// xeAllocatePoolTypeWithTag: a small pool block has an 8-byte header (byte 2
// is 170, then the tag); a large one is page aligned, with none
uint32_t allocate_pool(Kernel &k, uint32_t size, uint32_t tag, uint32_t) {
    if (size <= 0xFD8) {
        uint32_t address = k.memory().system_alloc(size + 8, 64);
        if (!address) return 0;
        k.host(address)[2] = 170;
        k.store32(address + 4, tag);
        return address + 8;
    }
    return k.memory().system_alloc(size, 4096);
}

// xeFreePool
void free_pool(Kernel &k, uint32_t address) {
    if ((address & (4096 - 1)) == 0)
        k.memory().system_free(address);
    else
        k.memory().system_free(address - 8);
}

XF_EXPORT(xboxkrnl, NtAllocateVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, alloc_type, protect_bits, debug_memory)
    uint32_t base_addr_ptr = arg(k, c, 0), region_size_ptr = arg(k, c, 1);
    uint32_t alloc_type = arg(k, c, 2), protect_bits = arg(k, c, 3), debug_memory = arg(k, c, 4);
    // devkit memory is not apart here: the flag is ignored
    if (debug_memory && base_addr_ptr && region_size_ptr)
        log("NtAllocateVirtualMemory: devkit debug memory asked for (base %08X, size %08X), allocated as usual",
            k.load32(base_addr_ptr), k.load32(region_size_ptr));
    if (!base_addr_ptr || !region_size_ptr || !k.load32(region_size_ptr)) return result(c, X_STATUS_INVALID_PARAMETER);
    if (!(alloc_type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) return result(c, X_STATUS_INVALID_PARAMETER);
    // MEM_RESET alone or not at all
    if ((alloc_type & X_MEM_RESET) && (alloc_type & ~X_MEM_RESET)) return result(c, X_STATUS_INVALID_PARAMETER);
    if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY))
        log("NtAllocateVirtualMemory: the game sets an EXECUTE bit (%08X)", protect_bits);

    uint32_t base_addr = k.load32(base_addr_ptr), region_size = k.load32(region_size_ptr);
    uint32_t page_size;
    if (base_addr != 0) {
        // a base address takes its heap's page size; the XPS/MMIO range has
        // no heap (the status for it is a guess of Xenia's)
        Heap *heap = k.memory().heap_for(base_addr);
        if (!heap || heap_type(heap) != HeapType::Virtual) return result(c, X_STATUS_INVALID_PARAMETER);
        page_size = heap->page_size();
    } else {
        page_size = (alloc_type & X_MEM_LARGE_PAGES) ? 64 * 1024 : 4 * 1024;
    }

    uint32_t adjusted_base = base_addr - (base_addr % page_size);
    // some games pass negative sizes
    uint32_t adjusted_size = int32_t(region_size) < 0 ? uint32_t(-int32_t(region_size)) : region_size;
    adjusted_size = round_up(adjusted_size, adjusted_base ? page_size : 64 * 1024);

    uint32_t allocation_type = 0;
    if (alloc_type & X_MEM_RESERVE) allocation_type |= ALLOCATE_RESERVE;
    if (alloc_type & X_MEM_COMMIT) allocation_type |= ALLOCATE_COMMIT;
    if (alloc_type & X_MEM_RESET) log("NtAllocateVirtualMemory: X_MEM_RESET is not implemented (%08X)", alloc_type);
    uint32_t protect = from_xdk_protect(protect_bits);
    uint32_t address = 0;
    Heap *heap;
    bool was_committed = false;
    if (adjusted_base != 0) {
        heap = k.memory().heap_for(adjusted_base);
        if (heap->page_size() != page_size) return result(c, X_STATUS_ACCESS_DENIED);
        RegionInfo previous;
        was_committed = heap->query(adjusted_base, previous) && (previous.state & ALLOCATE_COMMIT) != 0;
        if (heap->alloc_fixed(adjusted_base, adjusted_size, page_size, allocation_type, protect))
            address = adjusted_base;
    } else {
        bool top_down = (alloc_type & X_MEM_TOP_DOWN) != 0;
        heap = k.memory().heap_by_type(false, page_size);
        heap->alloc(adjusted_size, page_size, allocation_type, protect, top_down, &address);
    }
    if (!address) return result(c, X_STATUS_NO_MEMORY);

    // zeroed, unless asked not to or already committed
    if (!(alloc_type & X_MEM_NOZERO) && (alloc_type & X_MEM_COMMIT)) {
        if (!(protect & PROTECT_WRITE)) heap->protect(address, adjusted_size, PROTECT_READ_WRITE);
        if (!was_committed) std::memset(k.host(address), 0, adjusted_size);
        if (!(protect & PROTECT_WRITE)) heap->protect(address, adjusted_size, protect);
    }
    k.store32(base_addr_ptr, address);
    k.store32(region_size_ptr, adjusted_size);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtProtectVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, protect_bits, old_protect, debug_memory)
    uint32_t base_addr_ptr = arg(k, c, 0), region_size_ptr = arg(k, c, 1), protect_bits = arg(k, c, 2);
    uint32_t old_protect_ptr = arg(k, c, 3);
    if (!base_addr_ptr || !region_size_ptr || !k.load32(region_size_ptr)) return result(c, X_STATUS_INVALID_PARAMETER);
    if (protect_bits & (X_PAGE_EXECUTE | X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE | X_PAGE_EXECUTE_WRITECOPY)) {
        log("NtProtectVirtualMemory: the game sets an EXECUTE bit (%08X)", protect_bits);
        return result(c, X_STATUS_INVALID_PAGE_PROTECTION);
    }
    uint32_t base_addr = k.load32(base_addr_ptr);
    Heap *heap = k.memory().heap_for(base_addr);
    if (!heap || heap_type(heap) != HeapType::Virtual) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t adjusted_base = base_addr - (base_addr % heap->page_size());
    uint32_t adjusted_size = round_up(k.load32(region_size_ptr), heap->page_size());
    uint32_t old_protect = 0;
    // (a range over several regions fails, as in Xenia)
    if (!heap->protect(adjusted_base, adjusted_size, from_xdk_protect(protect_bits), &old_protect))
        return result(c, X_STATUS_ACCESS_DENIED);
    k.store32(base_addr_ptr, adjusted_base);
    k.store32(region_size_ptr, adjusted_size);
    // the heap's flags, not the XDK's, as Xenia writes them
    if (old_protect_ptr) k.store32(old_protect_ptr, old_protect);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtFreeVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, free_type, debug_memory)
    uint32_t base_addr_ptr = arg(k, c, 0), region_size_ptr = arg(k, c, 1), free_type = arg(k, c, 2);
    uint32_t base_addr_value = k.load32(base_addr_ptr), region_size_value = k.load32(region_size_ptr);
    if (!base_addr_value) return result(c, X_STATUS_MEMORY_NOT_ALLOCATED);
    Heap *heap = k.memory().heap_for(base_addr_value);
    if (!heap || heap_type(heap) != HeapType::Virtual) return result(c, X_STATUS_INVALID_PARAMETER);
    bool ok;
    if (free_type == X_MEM_DECOMMIT) {
        region_size_value = round_up(region_size_value, heap->page_size());
        ok = heap->decommit(base_addr_value, region_size_value);
    } else {
        ok = heap->release(base_addr_value, &region_size_value);
    }
    if (!ok) return result(c, X_STATUS_UNSUCCESSFUL);
    k.store32(base_addr_ptr, base_addr_value);
    k.store32(region_size_ptr, region_size_value);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtQueryVirtualMemory) {
    // (base_address, X_MEMORY_BASIC_INFORMATION *, region_type)
    uint32_t base_address = arg(k, c, 0), info = arg(k, c, 1), region_type = arg(k, c, 2);
    if (region_type > 2) return result(c, X_STATUS_INVALID_PARAMETER);
    Heap *heap = k.memory().heap_for(base_address);
    RegionInfo region;
    if (!heap || !heap->query(base_address, region)) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t state = (region.state & ALLOCATE_COMMIT)    ? X_MEM_COMMIT
                     : (region.state & ALLOCATE_RESERVE) ? X_MEM_RESERVE
                                                         : X_MEM_FREE;
    k.store32(info + 0x00, region.base_address);
    k.store32(info + 0x04, region.allocation_base);
    k.store32(info + 0x08, to_xdk_protect(region.allocation_protect));
    k.store32(info + 0x0C, region.region_size);
    k.store32(info + 0x10, state);
    k.store32(info + 0x14, to_xdk_protect(region.protect));
    k.store32(info + 0x18, X_MEM_PRIVATE);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtAllocateEncryptedMemory) {
    // (unknown, region_size, base_addr_ptr)
    uint32_t region_size = arg(k, c, 1), base_addr_ptr = arg(k, c, 2);
    if (!region_size) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t adjusted = round_up(region_size, 64 * 1024);
    if (adjusted > 16 * 1024 * 1024) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t address = 0;
    if (!k.memory()
             .heap_for(0x8C000000)
             ->alloc_range(0x8C000000, 0x8FFFFFFF, adjusted, 64 * 1024, ALLOCATE_COMMIT, PROTECT_READ_WRITE, false,
                           &address))
        return result(c, X_STATUS_UNSUCCESSFUL);
    k.store32(base_addr_ptr, address);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, NtFreeEncryptedMemory) {
    // (region_type, base_address_ptr): Xenia takes the value as a page
    // number in the 0x80000000 heap
    uint32_t base_address_ptr = arg(k, c, 1);
    if (!base_address_ptr) return result(c, X_STATUS_INVALID_PARAMETER);
    Heap *heap = k.memory().heap_for(0x80000000);
    uint32_t address = heap->heap_base() + heap->page_size() * k.load32(base_address_ptr);
    Heap *encrypted = k.memory().heap_for(address);
    if (!encrypted || heap_type(encrypted) != HeapType::Xex) return result(c, X_STATUS_INVALID_PARAMETER);
    k.memory().system_free(address);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, MmAllocatePhysicalMemoryEx) {
    // (flags, region_size, protect_bits, min_addr_range, max_addr_range, alignment)
    result(c, allocate_physical(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5)));
}

XF_EXPORT(xboxkrnl, MmAllocatePhysicalMemory) {
    // (flags, region_size, protect_bits)
    result(c, allocate_physical(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), 0, 0xFFFFFFFF, 0));
}

XF_EXPORT(xboxkrnl, MmFreePhysicalMemory) {
    // (type, base_address): what MmAllocatePhysicalMemory gave
    uint32_t base_address = arg(k, c, 1);
    if (Heap *heap = k.memory().heap_for(base_address)) heap->release(base_address);
}

XF_EXPORT(xboxkrnl, MmQueryAddressProtect) {
    uint32_t base_address = arg(k, c, 0);
    Heap *heap = k.memory().heap_for(base_address);
    RegionInfo region;
    uint32_t access = heap && heap->query(base_address, region) ? region.protect : 0;
    result(c, access ? to_xdk_protect(access) : 0);
}

XF_EXPORT(xboxkrnl, MmSetAddressProtect) {
    // (base_address, region_size, protect_bits)
    uint32_t base_address = arg(k, c, 0), region_size = arg(k, c, 1), protect_bits = arg(k, c, 2);
    constexpr uint32_t required =
        X_PAGE_NOACCESS | X_PAGE_READONLY | X_PAGE_READWRITE | X_PAGE_EXECUTE_READ | X_PAGE_EXECUTE_READWRITE;
    // exactly one access mode (many titles pass none: ignored)
    if (std::popcount(protect_bits & required) != 1) return;
    Heap *heap = k.memory().heap_for(base_address);
    // the XEX range keeps its protection (as the console seems to)
    if (!heap || heap_type(heap) == HeapType::Xex) return;
    heap->protect(base_address, region_size, from_xdk_protect(protect_bits));
}

XF_EXPORT(xboxkrnl, MmQueryAllocationSize) {
    uint32_t base_address = arg(k, c, 0);
    Heap *heap = k.memory().heap_for(base_address);
    uint32_t size = 0;
    if (!heap || !heap->query_size(base_address, &size)) size = 0;
    result(c, size);
}

XF_EXPORT(xboxkrnl, MmQueryStatistics) {
    // X_MM_QUERY_STATISTICS_RESULT (104 bytes): size, total physical pages,
    // kernel pages, the title's and the system's sections (11 words each),
    // the highest physical page
    constexpr uint32_t size = 104;
    uint32_t stats = arg(k, c, 0);
    if (!stats) return result(c, X_STATUS_INVALID_PARAMETER);
    if (k.load32(stats) != size) return result(c, X_STATUS_BUFFER_TOO_SMALL);
    std::memset(k.host(stats), 0, size);
    // the physical windows' pages in use (Xenia's GetHeapsPageStatsSummary)
    uint32_t used_pages = 0, reserved_bytes = 0;
    for (uint32_t page_size : {0x1000u, 0x10000u, 0x1000000u}) {
        Heap *heap = k.memory().heap_by_type(true, page_size);
        uint32_t reserved_pages = 0;
        for (uint64_t at = heap->heap_base(); at < uint64_t(heap->heap_base()) + heap->heap_size();) {
            RegionInfo region;
            if (!heap->query(uint32_t(at), region) || !region.region_size) break;
            if (region.state) reserved_pages += region.region_size / heap->page_size();
            at += region.region_size;
        }
        used_pages += uint32_t(uint64_t(reserved_pages) * heap->page_size() / 4096);
        reserved_bytes += reserved_pages * heap->page_size();
    }
    constexpr uint32_t total_physical_pages = 0x00020000; // 512 MB in 4 KB pages
    constexpr uint32_t kernel_pages = 0x00000100;
    k.store32(stats + 0x00, size);
    k.store32(stats + 0x04, total_physical_pages);
    k.store32(stats + 0x08, kernel_pages);
    // the title's section (Xenia's numbers, mostly guessed)
    uint32_t title = stats + 0x0C;
    k.store32(title + 0x00, total_physical_pages - kernel_pages - used_pages); // available pages
    k.store32(title + 0x04, 0x2FFE0000);                                       // total virtual memory bytes
    k.store32(title + 0x08, reserved_bytes);                                   // reserved virtual memory bytes
    k.store32(title + 0x0C, 0x00001000);                                       // physical pages
    k.store32(title + 0x10, 0x00000010);                                       // pool pages
    k.store32(title + 0x14, 0x00000100);                                       // stack pages
    k.store32(title + 0x18, 0x00000100);                                       // image pages
    k.store32(title + 0x1C, 0x00000100);                                       // heap pages
    k.store32(title + 0x20, 0x00000100);                                       // virtual pages
    k.store32(title + 0x24, 0x00000100);                                       // page table pages
    k.store32(title + 0x28, 0x00000100);                                       // cache pages
    // the system's section stays zero
    k.store32(stats + 0x64, 0x0001FFFF); // highest physical page
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, MmGetPhysicalAddress) {
    uint32_t physical = k.memory().physical_address(arg(k, c, 0));
    result(c, physical == UINT32_MAX ? 0 : physical);
}

XF_EXPORT(xboxkrnl, MmMapIoSpace) {
    // (unknown 2, src_address, size 0x40, flags 0x404): seen mapping XMA
    // contexts only, where the source address does
    result(c, arg(k, c, 1));
}

XF_EXPORT(xboxkrnl, ExAllocatePoolTypeWithTag) {
    // (size, tag, pool_selector)
    result(c, allocate_pool(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)));
}

XF_EXPORT(xboxkrnl, ExAllocatePoolWithTag) {
    // (size, tag)
    result(c, allocate_pool(k, arg(k, c, 0), arg(k, c, 1), 0));
}

XF_EXPORT(xboxkrnl, ExAllocatePool) {
    constexpr uint32_t none = 0x656E6F4E; // 'None'
    result(c, allocate_pool(k, arg(k, c, 0), none, 0));
}

XF_EXPORT(xboxkrnl, ExFreePool) { free_pool(k, arg(k, c, 0)); }

XF_EXPORT(xboxkrnl, KeGetImagePageTableEntry) {
    // Xenia's guess at the hypervisor's answer: the page's number in its XEX
    // heap, with 0x40000001 for 4 KB pages
    uint32_t address = arg(k, c, 0);
    Heap *heap = k.memory().heap_for(address);
    if (!heap || heap_type(heap) != HeapType::Xex) return result(c, 0);
    uint32_t value = (address - heap->heap_base()) / heap->page_size();
    if (heap->page_size() < 65536) value |= 0x40000000 | 1;
    result(c, value & 0x400FFFFF);
}

XF_EXPORT(xboxkrnl, KeLockL2) {
    // the L2 cache is not emulated: the XPS range is always backed
    result(c, 0);
}

XF_EXPORT(xboxkrnl, KeUnlockL2) {}

XF_EXPORT(xboxkrnl, MmCreateKernelStack) {
    // (stack_size, unknown): the top of a stack in 0x70000000-0x7F000000
    uint32_t stack_size = arg(k, c, 0);
    uint32_t aligned = (stack_size + 0xFFF) & 0xFFFFF000;
    uint32_t alignment = (stack_size & 0xF000) ? 0x1000 : 0x10000;
    uint32_t address = 0;
    k.memory()
        .heap_for(0x70000000)
        ->alloc_range(0x70000000, 0x7F000000, aligned, alignment, RC, PROTECT_READ_WRITE, false, &address);
    result(c, address + stack_size);
}

XF_EXPORT(xboxkrnl, MmDeleteKernelStack) {
    // (stack_base, stack_end): released by its low end; a fiber's stack, so
    // the host fiber its code ran on goes too (stacks.h)
    uint32_t stack_base = arg(k, c, 0), stack_end = arg(k, c, 1);
    guest_stacks_freed(k, stack_base, stack_end);
    result(c, k.memory().heap_for(0x70000000)->release(stack_end) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL);
}

XF_EXPORT(xboxkrnl, MmIsAddressValid) {
    // committed and readable (Xenia's QueryRangeAccess of the one address)
    uint32_t address = arg(k, c, 0);
    Heap *heap = k.memory().heap_for(address);
    RegionInfo region;
    if (!heap || !heap->query(address, region)) return result(c, 0);
    result(c, (region.state & ALLOCATE_COMMIT) && (region.protect & PROTECT_READ) ? 1 : 0);
}

// The object types' procedures (KernelState::InitializeKernelGuestGlobals):
// the kernel's own, guest-callable through thunks (modules.cpp)

XF_EXPORT(kernel, SimpleForwardAllocatePoolTypeWithTag) {
    result(c, allocate_pool(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)));
}

XF_EXPORT(kernel, SimpleForwardFreePool) { free_pool(k, arg(k, c, 0)); }

XF_EXPORT(kernel, AllocateThread) {
    // (size, tag, pool_selector): a small thread object is 8 bytes larger,
    // and starts 2 bytes in when not page aligned
    uint32_t size = arg(k, c, 0);
    if (size <= 0xFD8) size += 8;
    uint32_t address = allocate_pool(k, size, arg(k, c, 1), arg(k, c, 2));
    if ((address & 0xFFF) != 0) address += 2;
    result(c, address);
}

XF_EXPORT(kernel, FreeThread) {
    uint32_t address = arg(k, c, 0);
    if ((address & 0xFFF) != 0) address -= 8;
    free_pool(k, address);
}

} // namespace
} // namespace xf::kernel
