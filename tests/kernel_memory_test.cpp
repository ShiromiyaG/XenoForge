// The memory family's exports (src/kernel/memory_exports.cpp), called as a
// game calls them, against what Xenia's xboxkrnl_memory.cc gives on the
// same heaps.

#include "kernel_harness.h"

using namespace xf;

namespace {

constexpr uint32_t X_MEM_COMMIT = 0x1000, X_MEM_RESERVE = 0x2000, X_MEM_DECOMMIT = 0x4000, X_MEM_RELEASE = 0x8000;
constexpr uint32_t X_MEM_FREE = 0x10000, X_MEM_PRIVATE = 0x20000, X_MEM_RESET = 0x80000, X_MEM_TOP_DOWN = 0x100000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_PAGE_NOACCESS = 1, X_PAGE_READONLY = 2, X_PAGE_READWRITE = 4, X_PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t X_PAGE_WRITECOMBINE = 0x400;

void virtual_memory(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t base_ptr = h.alloc(4), size_ptr = h.alloc(4), info = h.alloc(0x1C);

    // the 4 KB heap, bottom-up, the size taken up to 64 KB; zeroed
    k.store32(base_ptr, 0);
    k.store32(size_ptr, 0x1234);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_RESERVE | X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          0);
    uint32_t a = k.load32(base_ptr);
    CHECK(a != 0 && a < 0x40000000 && k.load32(size_ptr) == 0x10000);
    CHECK(k.load32(a) == 0 && k.load32(a + 0xFFFC) == 0);
    CHECK(h.krnl("NtQueryVirtualMemory", {a + 0x2000, info, 0}) == 0);
    CHECK(k.load32(info + 0x00) == a + 0x2000);       // base address
    CHECK(k.load32(info + 0x04) == a);                // allocation base
    CHECK(k.load32(info + 0x08) == X_PAGE_READWRITE); // allocation protect
    CHECK(k.load32(info + 0x0C) == 0xE000);           // region size, from the address
    CHECK(k.load32(info + 0x10) == X_MEM_COMMIT);     // state
    CHECK(k.load32(info + 0x14) == X_PAGE_READWRITE); // protect
    CHECK(k.load32(info + 0x18) == X_MEM_PRIVATE);    // type
    CHECK(h.krnl("NtQueryVirtualMemory", {a, info, 3}) == kernel::X_STATUS_INVALID_PARAMETER);

    // top-down: as high as Xenia's scan goes (its last page is left out of
    // the scan, the start on 4 KB)
    k.store32(base_ptr, 0);
    k.store32(size_ptr, 0x1000);
    CHECK(h.krnl("NtAllocateVirtualMemory",
                 {base_ptr, size_ptr, X_MEM_RESERVE | X_MEM_COMMIT | X_MEM_TOP_DOWN, X_PAGE_READWRITE, 0}) == 0);
    uint32_t top = k.load32(base_ptr);
    CHECK(top == 0x3FFEF000);

    // large pages: the 64 KB heap
    k.store32(base_ptr, 0);
    k.store32(size_ptr, 0x20000);
    CHECK(h.krnl("NtAllocateVirtualMemory",
                 {base_ptr, size_ptr, X_MEM_RESERVE | X_MEM_COMMIT | X_MEM_LARGE_PAGES, X_PAGE_READWRITE, 0}) == 0);
    uint32_t large = k.load32(base_ptr);
    CHECK(large >= 0x40000000 && large < 0x70000000 && k.load32(size_ptr) == 0x20000);

    // a negative size rounds past 4 GB: the console's kernel refuses it (Xenia took its magnitude)
    k.store32(base_ptr, 0);
    k.store32(size_ptr, uint32_t(-0x1000));
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_RESERVE, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    k.store32(size_ptr, 0x1000);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_RESERVE, X_PAGE_READWRITE, 0}) == 0);
    CHECK(k.load32(size_ptr) == 0x10000);
    uint32_t reserved = k.load32(base_ptr);
    CHECK(h.krnl("NtQueryVirtualMemory", {reserved, info, 0}) == 0 && k.load32(info + 0x10) == X_MEM_RESERVE);

    // at a base: rounded down to its page, the size to pages; then a commit
    // in it, the protection read-only (zeroed through a writable view)
    k.store32(base_ptr, 0x20000010);
    k.store32(size_ptr, 0x3000);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_RESERVE, X_PAGE_READWRITE, 0}) == 0);
    CHECK(k.load32(base_ptr) == 0x20000000 && k.load32(size_ptr) == 0x3000);
    k.store32(base_ptr, 0x20001000);
    k.store32(size_ptr, 0x1000);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT, X_PAGE_READONLY, 0}) == 0);
    CHECK(k.load32(base_ptr) == 0x20001000 && k.load32(size_ptr) == 0x1000);
    CHECK(h.krnl("NtQueryVirtualMemory", {0x20001000, info, 0}) == 0);
    CHECK(k.load32(info + 0x04) == 0x20000000 && k.load32(info + 0x0C) == 0x1000);
    CHECK(k.load32(info + 0x10) == X_MEM_COMMIT && k.load32(info + 0x14) == X_PAGE_READONLY);
    CHECK(h.krnl("MmQueryAddressProtect", {0x20001000}) == X_PAGE_READONLY);
    CHECK(h.krnl("MmQueryAllocationSize", {0x20001000}) == 0x3000);

    // NtProtectVirtualMemory: the old protection as the heap keeps it
    uint32_t old = h.alloc(4);
    k.store32(base_ptr, 0x20001004);
    k.store32(size_ptr, 0x10);
    CHECK(h.krnl("NtProtectVirtualMemory", {base_ptr, size_ptr, X_PAGE_READWRITE, old, 0}) == 0);
    CHECK(k.load32(base_ptr) == 0x20001000 && k.load32(size_ptr) == 0x1000 && k.load32(old) == PROTECT_READ);
    CHECK(h.krnl("MmQueryAddressProtect", {0x20001000}) == X_PAGE_READWRITE);
    // the protections the console's kernel refuses: execute alone, write-copy, none, two modes, other bits
    for (uint32_t bad : {0x10u, 0x08u, 0x80u, 0x00u, 0x06u, 0x4004u, 0x101u})
        CHECK(h.krnl("NtProtectVirtualMemory", {base_ptr, size_ptr, bad, old, 0}) == 0xC0000045);
    // and takes the execute-read modes (Xenia refused them)
    CHECK(h.krnl("NtProtectVirtualMemory", {base_ptr, size_ptr, X_PAGE_EXECUTE_READWRITE, old, 0}) == 0);
    k.store32(base_ptr, 0);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT, 0x08, 0}) == 0xC0000045);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT | 0x400000, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);

    // the invalid ones
    k.store32(base_ptr, 0);
    k.store32(size_ptr, 0);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    k.store32(size_ptr, 0x1000);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, 0, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_RESET | X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    CHECK(h.krnl("NtAllocateVirtualMemory", {0, size_ptr, X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    k.store32(base_ptr, 0x82000000); // not a virtual heap
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);
    k.store32(base_ptr, 0x7FC80000); // MMIO: no heap
    CHECK(h.krnl("NtAllocateVirtualMemory", {base_ptr, size_ptr, X_MEM_COMMIT, X_PAGE_READWRITE, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER);

    // NtFreeVirtualMemory: decommit a page, then release the whole
    k.store32(base_ptr, 0x20001000);
    k.store32(size_ptr, 0x10);
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, X_MEM_DECOMMIT, 0}) == 0);
    CHECK(k.load32(size_ptr) == 0x1000);
    CHECK(h.krnl("NtQueryVirtualMemory", {0x20001000, info, 0}) == 0 && k.load32(info + 0x10) == X_MEM_RESERVE);
    k.store32(base_ptr, 0x20001000); // not the region's start
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, X_MEM_RELEASE, 0}) == kernel::X_STATUS_UNSUCCESSFUL);
    k.store32(base_ptr, 0x20000000);
    k.store32(size_ptr, 0);
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, X_MEM_RELEASE, 0}) == 0);
    CHECK(k.load32(base_ptr) == 0x20000000 && k.load32(size_ptr) == 0x3000);
    CHECK(h.krnl("NtQueryVirtualMemory", {0x20000000, info, 0}) == 0);
    CHECK(k.load32(info + 0x10) == X_MEM_FREE && k.load32(info + 0x14) == X_PAGE_NOACCESS);
    k.store32(base_ptr, 0);
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, X_MEM_RELEASE, 0}) == kernel::X_STATUS_INVALID_PARAMETER);
    k.store32(base_ptr, 0x20000000);
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, X_MEM_RELEASE | X_MEM_DECOMMIT, 0}) ==
          kernel::X_STATUS_INVALID_PARAMETER); // only one of them
    CHECK(h.krnl("NtFreeVirtualMemory", {base_ptr, size_ptr, 0, 0}) == kernel::X_STATUS_INVALID_PARAMETER);

    // MmIsAddressValid: committed and readable
    CHECK(h.krnl("MmIsAddressValid", {a}) == 1);
    CHECK(h.krnl("MmIsAddressValid", {reserved}) == 0);
    CHECK(h.krnl("MmIsAddressValid", {0}) == 0);          // the null pages: no access
    CHECK(h.krnl("MmIsAddressValid", {0x7FC80000}) == 0); // no heap
}

void physical_memory(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // 64 KB pages: the 0xA0000000 window, top-down; the physical address
    // the offset in it
    uint32_t a =
        h.krnl("MmAllocatePhysicalMemoryEx", {0, 0x18000, X_PAGE_READWRITE | X_MEM_LARGE_PAGES, 0, 0xFFFFFFFF, 0});
    CHECK(a >= 0xA0000000 && a < 0xC0000000 && (a & 0xFFFF) == 0);
    CHECK(h.krnl("MmGetPhysicalAddress", {a}) == a - 0xA0000000);
    CHECK(h.krnl("MmQueryAllocationSize", {a}) == 0x20000);
    CHECK(h.krnl("MmQueryAddressProtect", {a}) == X_PAGE_READWRITE);
    // 4 KB pages: the 0xE0000000 window, its physical address 0x1000 on;
    // a range in physical memory (the first 16 MB are the GPU's writeback),
    // the alignment the physical address's
    uint32_t b = h.krnl("MmAllocatePhysicalMemoryEx", {0, 0x2000, X_PAGE_READWRITE, 0x01000000, 0x01FFFFFF, 0x10000});
    CHECK(b >= 0xE0000000);
    uint32_t physical = h.krnl("MmGetPhysicalAddress", {b});
    CHECK(physical == b - 0xE0000000 + 0x1000 && (physical & 0xFFFF) == 0);
    CHECK(physical >= 0x01000000 && physical + 0x2000 <= 0x02000000);
    CHECK(h.krnl("MmAllocatePhysicalMemoryEx", {0, 0x2000, X_PAGE_READWRITE, 0, 0x00FFFFFF, 0}) == 0);
    // no access mode: read/write; write combined kept
    uint32_t wc =
        h.krnl("MmAllocatePhysicalMemoryEx", {0, 0x1000, X_PAGE_WRITECOMBINE | X_MEM_LARGE_PAGES, 0, 0xFFFFFFFF, 0});
    CHECK(wc != 0 && h.krnl("MmQueryAddressProtect", {wc}) == (X_PAGE_READWRITE | X_PAGE_WRITECOMBINE));
    uint32_t c = h.krnl("MmAllocatePhysicalMemory", {0, 0x1000, X_PAGE_READWRITE});
    CHECK(c >= 0xE0000000);
    // MmSetAddressProtect: exactly one access mode
    h.krnl("MmSetAddressProtect", {b, 0x2000, X_PAGE_READONLY});
    CHECK(h.krnl("MmQueryAddressProtect", {b}) == X_PAGE_READONLY);
    h.krnl("MmSetAddressProtect", {b, 0x2000, X_PAGE_READWRITE | X_PAGE_READONLY});
    CHECK(h.krnl("MmQueryAddressProtect", {b}) == X_PAGE_READONLY);
    h.krnl("MmSetAddressProtect", {b, 0x2000, X_PAGE_WRITECOMBINE});
    CHECK(h.krnl("MmQueryAddressProtect", {b}) == X_PAGE_READONLY);
    // freed
    h.krnl("MmFreePhysicalMemory", {0, a});
    CHECK(h.krnl("MmQueryAllocationSize", {a}) == 0);
    CHECK(h.krnl("MmGetPhysicalAddress", {0x10000}) == 0x10000); // virtual: the address
    CHECK(h.krnl("MmMapIoSpace", {2, 0x7FEA1800, 0x40, 0x404}) == 0x7FEA1800);

    // MmQueryStatistics: the size first
    uint32_t stats = h.alloc(104);
    k.store32(stats, 100);
    CHECK(h.krnl("MmQueryStatistics", {stats}) == 0xC0000023);
    k.store32(stats, 104);
    CHECK(h.krnl("MmQueryStatistics", {stats}) == 0);
    CHECK(k.load32(stats + 4) == 0x20000 && k.load32(stats + 8) == 0x100);
    uint32_t available = k.load32(stats + 0x0C);
    CHECK(available > 0x10000 && available < 0x20000 - 0x100);
    CHECK(k.load32(stats + 0x10) == 0x2FFE0000 && k.load32(stats + 0x64) == 0x1FFFF);
    CHECK(k.load32(stats + 0x0C + 0x2C) == 0); // the system's section
    CHECK(h.krnl("MmQueryStatistics", {0}) == kernel::X_STATUS_INVALID_PARAMETER);
}

void pools_and_stacks(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // small: an 8-byte header (170, the tag)
    uint32_t p = h.krnl("ExAllocatePoolWithTag", {0x100, 0x54657374});
    CHECK(p != 0 && (p & 0xFFF) == 8);
    CHECK(k.host(p - 8)[2] == 170 && k.load32(p - 4) == 0x54657374);
    uint32_t q = h.krnl("ExAllocatePool", {0x10});
    CHECK(k.load32(q - 4) == 0x656E6F4E); // 'None'
    uint32_t t = h.krnl("ExAllocatePoolTypeWithTag", {0xFD8, 0x41424344, 1});
    CHECK((t & 0xFFF) == 8 && k.load32(t - 4) == 0x41424344);
    // large: page aligned, no header
    uint32_t big = h.krnl("ExAllocatePoolWithTag", {0x2000, 0x54657374});
    CHECK(big != 0 && (big & 0xFFF) == 0);
    h.krnl("ExFreePool", {p});
    h.krnl("ExFreePool", {big});
    CHECK(h.krnl("MmQueryAllocationSize", {p - 8}) == 0);
    CHECK(h.krnl("MmQueryAllocationSize", {big}) == 0);

    // kernel stacks: the top of one in 0x70000000-0x7F000000
    uint32_t top = h.krnl("MmCreateKernelStack", {0x10000, 0});
    CHECK(top > 0x70000000 && top <= 0x7F000000 && (top & 0xFFFF) == 0);
    CHECK(h.krnl("MmIsAddressValid", {top - 4}) == 1);
    CHECK(h.krnl("MmDeleteKernelStack", {top, top - 0x10000}) == 0);
    CHECK(h.krnl("MmIsAddressValid", {top - 4}) == 0);
    uint32_t odd = h.krnl("MmCreateKernelStack", {0x3000, 0}); // 4 KB aligned
    CHECK(odd > 0x70000000 && ((odd - 0x3000) & 0xFFF) == 0);

    CHECK(h.krnl("KeLockL2", {0}) == 0);
    // the image page table entries Xenia makes up
    CHECK(h.krnl("KeGetImagePageTableEntry", {0x82010000}) == 0x201);
    CHECK(h.krnl("KeGetImagePageTableEntry", {0x92003000}) == (0x40000001 | 0x2003));
    CHECK(h.krnl("KeGetImagePageTableEntry", {0x10000}) == 0);

    uint32_t base_ptr = h.alloc(4);
    CHECK(h.krnl("NtAllocateEncryptedMemory", {0, 0x8000, base_ptr}) == 0);
    CHECK(k.load32(base_ptr) >= 0x8C000000 && k.load32(base_ptr) < 0x90000000);
    CHECK(h.krnl("NtAllocateEncryptedMemory", {0, 0x2000000, base_ptr}) == kernel::X_STATUS_INVALID_PARAMETER);
}

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_memory: no kernel\n"), 1;
    virtual_memory(h);
    physical_memory(h);
    pools_and_stacks(h);
    return finish("kernel_memory");
}
