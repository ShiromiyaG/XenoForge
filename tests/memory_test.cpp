// The guest memory (memory.h): its views, the fixed allocations Xenia's
// Memory::Initialize makes, and the heaps' allocations, with the addresses
// Xenia's allocator gives (worked out from its memory.cc).

#include "check.h"

#include "xenoforge/memory.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace xf;

static uint32_t load_be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

static void store_be32(uint8_t *p, uint32_t value) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

static RegionInfo query(Heap *heap, uint32_t address) {
    RegionInfo info;
    CHECK(heap->query(address, info));
    return info;
}

constexpr uint32_t RC = ALLOCATE_RESERVE | ALLOCATE_COMMIT;
constexpr uint32_t RW = PROTECT_READ_WRITE;

static void layout(Memory &mem) {
    Heap *v4k = mem.heap_by_type(false, 0x1000);
    Heap *v64k = mem.heap_by_type(false, 0x10000);
    Heap *p4k = mem.heap_by_type(true, 0x1000);
    Heap *p64k = mem.heap_by_type(true, 0x10000);
    Heap *p16m = mem.heap_by_type(true, 0x1000000);
    CHECK(mem.heap_for(0x00000000) == v4k && mem.heap_for(0x3FFFFFFF) == v4k);
    CHECK(mem.heap_for(0x40000000) == v64k && mem.heap_for(0x7EFFFFFF) == v64k);
    CHECK(mem.heap_for(0xA0000000) == p64k && mem.heap_for(0xBFFFFFFF) == p64k);
    CHECK(mem.heap_for(0xC0000000) == p16m && mem.heap_for(0xDFFFFFFF) == p16m);
    CHECK(mem.heap_for(0xE0000000) == p4k && mem.heap_for(0xFFCFFFFF) == p4k);
    CHECK(mem.heap_by_type(true, 0x2000) == p64k);
    CHECK(mem.heap_by_type(false, 0x2000) == v64k);
    CHECK(v4k->heap_base() == 0 && v4k->heap_size() == 0x40000000 && v4k->page_size() == 0x1000);
    CHECK(v64k->heap_base() == 0x40000000 && v64k->heap_size() == 0x3F000000 && v64k->page_size() == 0x10000);
    CHECK(p16m->page_size() == 0x1000000 && p4k->heap_size() == 0x1FD00000);
    Heap *writeback = mem.heap_for(0x7F000000);
    CHECK(writeback && writeback->heap_base() == 0x7F000000 && writeback->heap_size() == 0xC80000);
    CHECK(writeback && writeback->page_size() == 0x1000 && writeback->contains(0x7FC7FFFF));
    CHECK(writeback && !writeback->contains(0x7FC80000));
    Heap *xex64k = mem.heap_for(0x80000000);
    Heap *xex4k = mem.heap_for(0x90000000);
    CHECK(xex64k && xex64k->page_size() == 0x10000 && xex64k->heap_size() == 0x10000000);
    CHECK(xex4k && xex4k->page_size() == 0x1000 && xex4k->heap_base() == 0x90000000);
    // MMIO and the top of the 4 KB window: no heap
    CHECK(mem.heap_for(0x7FC80000) == nullptr && mem.heap_for(0x7FFFFFFF) == nullptr);
    CHECK(mem.heap_for(0xFFD00000) == nullptr && mem.heap_for(0xFFFFFFFF) == nullptr);
}

// what Memory::Initialize allocates
static void fixed_allocations(Memory &mem) {
    Heap *v4k = mem.heap_by_type(false, 0x1000);
    RegionInfo zero = query(v4k, 0);
    CHECK(zero.state == RC && zero.protect == 0 && zero.allocation_base == 0);
    CHECK(zero.allocation_size == 0x10000 && zero.region_size == 0x10000);
    CHECK(!v4k->release(0));
    // the free 4 KB heap after the first 64 KB
    RegionInfo free = query(v4k, 0x10000);
    CHECK(free.state == 0 && free.region_size == 0x40000000 - 0x10000);

    // the hypervisor's 256 KB, and the value at 0x8000001C
    Heap *xex = mem.heap_for(0x80000000);
    RegionInfo hypervisor = query(xex, 0x80000000);
    CHECK(hypervisor.state == RC && hypervisor.protect == RW && hypervisor.allocation_size == 0x40000);
    CHECK(load_be32(mem.translate(0x8000001C)) == 0x2A6E3F38);

    // the GPU writeback: 16 MB at 0xC0000000, 12.5 MB at 0x7F000000
    RegionInfo writeback = query(mem.heap_for(0xC0000000), 0xC0000000);
    CHECK(writeback.state == RC && writeback.protect == RW && writeback.allocation_size == 0x1000000);
    RegionInfo writeback_virtual = query(mem.heap_for(0x7F000000), 0x7F000000);
    CHECK(writeback_virtual.state == RC && writeback_virtual.allocation_size == 0xC80000);
    CHECK(writeback_virtual.region_size == 0xC80000);

    // Xenia's unexplained reservation at the top of the 64 KB window
    RegionInfo unknown = query(mem.heap_for(0xA0000000), 0xBFCB0000);
    CHECK(unknown.state == ALLOCATE_RESERVE && unknown.allocation_size == 0x340000 && unknown.protect == 0);
    CHECK(query(mem.heap_for(0xA0000000), 0xBFCA0000).state == 0);
}

// the windows see the same physical memory
static void views(Memory &mem) {
    Heap *p64k = mem.heap_by_type(true, 0x10000);
    Heap *p4k = mem.heap_by_type(true, 0x1000);
    // physical 0-64 KB and 0x1000-0x1FFF are inside the GPU writeback's
    // pages; Xenia takes them again
    CHECK(p64k->alloc_fixed(0xA0000000, 0x10000, 0, RC, RW));
    CHECK(p4k->alloc_fixed(0xE0000000, 0x1000, 0, RC, RW));
    store_be32(mem.translate(0xA0001000), 0x12345678);
    CHECK(load_be32(mem.translate(0xC0001000)) == 0x12345678);
    CHECK(load_be32(mem.translate(0xE0000000)) == 0x12345678);
    CHECK(load_be32(mem.translate(0x7F001000)) == 0x12345678);
    CHECK(load_be32(mem.translate_physical(0x1000)) == 0x12345678);
    CHECK(mem.translate_physical(0x20001000) == mem.translate_physical(0x1000));
    CHECK(mem.translate(0xE0000000) == mem.base() + 0xE0001000);
    store_be32(mem.translate(0xE0000FFC), 0xCAFEF00D);
    CHECK(load_be32(mem.translate_physical(0x1FFC)) == 0xCAFEF00D);
    CHECK(load_be32(mem.translate(0xA0001FFC)) == 0xCAFEF00D);

    // physical addresses
    CHECK(mem.physical_address(0xA0001000) == 0x1000);
    CHECK(mem.physical_address(0xC0001000) == 0x1000);
    CHECK(mem.physical_address(0xE0000000) == 0x1000);
    CHECK(mem.physical_address(0xE0001000) == 0x2000);
    CHECK(mem.physical_address(0x7F001000) == 0x1000);
    CHECK(mem.physical_address(0xBFFFFFFF) == 0x1FFFFFFF);
    // virtual: the address itself in the 4 KB heap, its offset in its heap
    // elsewhere (Xenia's)
    CHECK(mem.physical_address(0x00001000) == 0x1000);
    CHECK(mem.physical_address(0x30000000) == 0x30000000);
    CHECK(mem.physical_address(0x40010000) == 0x10000);
    CHECK(mem.physical_address(0x82000000) == 0x02000000);
    CHECK(mem.physical_address(0x7FC80000) == UINT32_MAX);
    CHECK(mem.physical_address(0xFFD00000) == UINT32_MAX);
}

static void virtual_4k(Memory &mem) {
    Heap *h = mem.heap_by_type(false, 0x1000);
    uint32_t a = 0, b = 0, c = 0, top = 0, top_aligned = 0;
    // bottom-up, first fit after the first 64 KB; sizes to pages
    CHECK(h->alloc(0x1800, 0, RC, RW, false, &a) && a == 0x10000);
    RegionInfo info = query(h, a);
    CHECK(info.base_address == a && info.allocation_base == a && info.allocation_size == 0x2000);
    CHECK(info.region_size == 0x2000 && info.state == RC && info.protect == RW && info.allocation_protect == RW);
    uint32_t size = 0;
    CHECK(h->query_size(a + 0x1000, &size) && size == 0x2000);
    store_be32(mem.translate(a + 0x1FFC), 0xA5A5A5A5);
    CHECK(load_be32(mem.translate(a + 0x1FFC)) == 0xA5A5A5A5);
    // alignment: the next 64 KB boundary
    CHECK(h->alloc(0x1000, 0x10000, RC, RW, false, &b) && b == 0x20000);
    CHECK(h->alloc(0x1000, 0, RC, RW, false, &c) && c == 0x12000);
    // top-down: the heap's top page is never handed out (Xenia's exclusive
    // high page)
    CHECK(h->alloc(0x1000, 0, RC, RW, true, &top) && top == 0x3FFFE000);
    CHECK(h->alloc(0x1000, 0x10000, RC, RW, true, &top_aligned) && top_aligned == 0x3FFE0000);

    // protect: per page, in one region, committed
    uint32_t old = 0;
    CHECK(h->protect(a, 0x1000, PROTECT_READ, &old) && old == RW);
    info = query(h, a);
    CHECK(info.protect == PROTECT_READ && info.allocation_protect == RW && info.region_size == 0x1000);
    info = query(h, a + 0x1000);
    CHECK(info.protect == RW && info.region_size == 0x1000 && info.allocation_base == a);
    CHECK(h->protect(a, 0x1001, RW, &old) && old == PROTECT_READ);
    CHECK(query(h, a).region_size == 0x2000);
    // write-combined reads back as plain read-write
    CHECK(h->protect(a, 1, PROTECT_READ | PROTECT_WRITE_COMBINE, nullptr));
    CHECK(h->protect(a, 1, RW, &old) && old == RW);
    CHECK(!h->protect(a + 0x1000, 0x2000, PROTECT_READ)); // two regions
    CHECK(!h->protect(0x13000, 0x1000, PROTECT_READ));    // free
    CHECK(!h->protect(a, 0, PROTECT_READ));

    // release: the whole region, from its base
    CHECK(!h->release(a + 0x1000));
    CHECK(h->release(a, &size) && size == 0x2000);
    info = query(h, a);
    CHECK(info.state == 0 && info.region_size == 0x2000 && info.allocation_base == 0);
    CHECK(!h->release(a));
    CHECK(h->release(b) && h->release(c) && h->release(top) && h->release(top_aligned));

    // reserve, then commit part of it
    CHECK(h->alloc_fixed(0x00100000, 0x3000, 0, ALLOCATE_RESERVE, RW));
    info = query(h, 0x00100000);
    CHECK(info.state == ALLOCATE_RESERVE && info.allocation_size == 0x3000 && info.region_size == 0x3000);
    CHECK(!h->alloc_fixed(0x00100000, 0x1000, 0, ALLOCATE_RESERVE, RW));
    CHECK(h->alloc_fixed(0x00101000, 0x1000, 0, ALLOCATE_COMMIT, RW));
    info = query(h, 0x00101000);
    CHECK(info.state == RC && info.allocation_base == 0x00100000 && info.allocation_size == 0x3000);
    CHECK(info.region_size == 0x1000);
    CHECK(query(h, 0x00100000).region_size == 0x1000);
    store_be32(mem.translate(0x00101000), 0x01020304);
    CHECK(load_be32(mem.translate(0x00101000)) == 0x01020304);
    CHECK(h->query_size(0x00101000, &size) && size == 0x3000);
    CHECK(h->decommit(0x00101000, 0x1000));
    CHECK(query(h, 0x00101000).state == ALLOCATE_RESERVE);
    CHECK(query(h, 0x00100000).region_size == 0x3000);
    CHECK(!h->protect(0x00101000, 0x1000, PROTECT_READ)); // decommitted
    CHECK(h->release(0x00100000) && query(h, 0x00100000).state == 0);
    // a commit of pages never reserved reserves them too
    CHECK(h->alloc_fixed(0x00200000, 0x2000, 0, ALLOCATE_COMMIT, RW));
    info = query(h, 0x00200000);
    CHECK(info.state == RC && info.allocation_size == 0x2000);
    CHECK(h->release(0x00200000));
    CHECK(!h->alloc_fixed(0x40000000, 0x1000, 0, RC, RW)); // outside

    // ranges
    uint32_t r = 0;
    CHECK(h->alloc_range(0x00300000, 0x003FFFFF, 0x1000, 0, RC, RW, false, &r) && r == 0x00300000);
    CHECK(h->alloc_range(0x00300000, 0x003FFFFF, 0x1000, 0, RC, RW, true, &r) && r == 0x003FF000);
    CHECK(h->alloc_range(0x00300000, 0x00301FFF, 0x1000, 0, RC, RW, false, &r) && r == 0x00301000);
    CHECK(!h->alloc_range(0x00300000, 0x00301FFF, 0x1000, 0, RC, RW, false, &r) && r == 0);
    CHECK(!h->alloc_range(0x00400000, 0x00400FFF, 0x2000, 0, RC, RW, false, &r));
    CHECK(h->release(0x00300000) && h->release(0x003FF000) && h->release(0x00301000));
}

static void virtual_64k(Memory &mem) {
    Heap *h = mem.heap_by_type(false, 0x10000);
    uint32_t a = 0, b = 0, top = 0, stack = 0;
    CHECK(h->alloc(0x100, 0, RC, RW, false, &a) && a == 0x40000000);
    CHECK(query(h, a).allocation_size == 0x10000);
    CHECK(h->alloc(0x10000, 0x40000, RC, RW, false, &b) && b == 0x40040000);
    // top-down stops below the threads' stacks (0x70000000-0x7EFFFFFF)
    CHECK(h->alloc(0x10000, 0, RC, RW, true, &top) && top == 0x6FFF0000);
    // which a range reaches
    CHECK(h->alloc_range(0x70000000, 0x7EFFFFFF, 0x10000, 0, RC, RW, true, &stack) && stack == 0x7EFE0000);
    store_be32(mem.translate(stack + 0xFFFC), 0xDEADBEEF);
    CHECK(load_be32(mem.translate(stack + 0xFFFC)) == 0xDEADBEEF);
    uint32_t size = 0;
    CHECK(h->release(a, &size) && size == 0x10000);
    CHECK(h->release(b) && h->release(top) && h->release(stack));
    CHECK(h->alloc(0x20000, 0, RC, RW, false, &a) && a == 0x40000000);
    CHECK(h->release(a));
}

static void physical(Memory &mem) {
    Heap *p64k = mem.heap_by_type(true, 0x10000);
    Heap *p16m = mem.heap_by_type(true, 0x1000000);
    Heap *p4k = mem.heap_by_type(true, 0x1000);

    // always top-down, below Xenia's reservation
    uint32_t a = 0;
    CHECK(p64k->alloc(0x10000, 0, RC, RW, false, &a) && a == 0xBFCA0000);
    CHECK(mem.physical_address(a) == 0x1FCA0000);
    store_be32(mem.translate(a + 0x10), 0x0BADF00D);
    CHECK(load_be32(mem.translate_physical(0x1FCA0010)) == 0x0BADF00D);
    RegionInfo info = query(p64k, a);
    CHECK(info.state == RC && info.protect == RW && info.allocation_size == 0x10000);
    uint32_t old = 0;
    CHECK(p64k->protect(a, 0x10000, PROTECT_READ, &old) && old == RW);
    CHECK(query(p64k, a).protect == PROTECT_READ);
    CHECK(p64k->protect(a, 0x10000, RW, &old) && old == PROTECT_READ);
    // released here and in the physical heap: the same pages again
    uint32_t size = 0;
    CHECK(p64k->release(a, &size) && size == 0x10000);
    CHECK(query(p64k, a).state == 0);
    CHECK(p64k->alloc(0x10000, 0, RC, RW, false, &a) && a == 0xBFCA0000);

    // the 4 KB window, physical 0x1000 on: right below in physical memory
    uint32_t e = 0, e_aligned = 0;
    CHECK(p4k->alloc(0x1000, 0, RC, RW, false, &e) && e == 0xFFC9E000);
    CHECK(mem.physical_address(e) == 0x1FC9F000);
    store_be32(mem.translate(e), 0x13579BDF);
    CHECK(load_be32(mem.translate_physical(0x1FC9F000)) == 0x13579BDF);
    // alignments are the physical address's (the host's), not the guest's
    CHECK(p4k->alloc(0x1000, 0x10000, RC, RW, false, &e_aligned) && e_aligned == 0xFFC8F000);
    CHECK(mem.physical_address(e_aligned) % 0x10000 == 0);

    // 16 MB pages, on 16 MB
    uint32_t c = 0;
    CHECK(p16m->alloc(0x1000000, 0, RC, RW, false, &c) && c == 0xDE000000);
    CHECK(mem.physical_address(c) == 0x1E000000);
    CHECK(query(p16m, c).allocation_size == 0x1000000);
    store_be32(mem.translate(c + 0xFFFFFC), 0x2468ACE0);
    CHECK(load_be32(mem.translate_physical(0x1EFFFFFC)) == 0x2468ACE0);

    // a range whose ceiling is not on the alignment stays under it
    uint32_t r = 0;
    CHECK(p64k->alloc_range(0xA1000000, 0xA1FF8000, 0x10000, 0x10000, RC, RW, true, &r) && r == 0xA1FE0000);
    CHECK(r + 0x10000 - 1 <= 0xA1FF8000);
    CHECK(p64k->decommit(r, 0x10000) && query(p64k, r).state == ALLOCATE_RESERVE);
    CHECK(p64k->release(r));

    // the GPU writeback window is full
    uint32_t w = 0;
    CHECK(!mem.heap_for(0x7F000000)->alloc(0x1000, 0, RC, RW, false, &w) && w == 0);

    CHECK(p64k->release(a) && p4k->release(e) && p4k->release(e_aligned) && p16m->release(c));
}

static void system(Memory &mem) {
    // a page each, bottom-up from 0x30000000, zeroed
    uint32_t s1 = mem.system_alloc(0x40);
    uint32_t s2 = mem.system_alloc(0x40);
    CHECK(s1 == 0x30000000 && s2 == 0x30001000);
    CHECK(query(mem.heap_for(s1), s1).allocation_size == 0x1000);
    store_be32(mem.translate(s1), 0xFFFFFFFF);
    mem.system_free(s1);
    uint32_t s3 = mem.system_alloc(0x10, 0x10);
    CHECK(s3 == 0x30000000 && load_be32(mem.translate(s3)) == 0);
    uint32_t s4 = mem.system_alloc(0x2000);
    CHECK(s4 == 0x30002000 && query(mem.heap_for(s4), s4).allocation_size == 0x2000);
    // physical: the 4 KB window
    uint32_t p = mem.system_alloc(40, 32, true);
    CHECK(p >= 0xE0000000 && p < 0xFFD00000 && mem.heap_for(p) == mem.heap_by_type(true, 0x1000));
    CHECK(load_be32(mem.translate(p)) == 0 && query(mem.heap_for(p), p).state == RC);
    mem.system_free(s2);
    mem.system_free(s3);
    mem.system_free(s4);
    mem.system_free(p);
    mem.system_free(0);
    CHECK(query(mem.heap_for(0x30000000), 0x30000000).state == 0);
}

// a watcher letting the watches go three pages either side of a write
static std::pair<uint32_t, uint32_t> three_around(void *, uint32_t first, uint32_t last) {
    return {first >= 3 ? first - 3 : 0, last + 3};
}

// The write watches: a watched page's first write faults once and numbers
// it, from any thread; host writes through the physical view are told;
// protecting a range writable numbers it; a watcher widens a fault.
static void watches(Memory &mem) {
    const uint32_t page = mem.watch_page_size(), shift = mem.watch_page_shift();
    CHECK(page >= 0x1000 && (1u << shift) == page);
    Heap *heap = mem.heap_by_type(true, 0x10000);
    uint32_t guest = 0;
    CHECK(heap->alloc(0x100000, 0x10000, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE, false, &guest));
    uint32_t physical = mem.physical_address(guest);
    const std::atomic<uint32_t> *stamps = mem.write_stamps();
    auto stamp = [&](uint32_t offset) { return stamps[(physical + offset) >> shift].load(); };

    // watched, then written by the guest's view: numbered, once
    uint32_t before = mem.write_count();
    mem.watch(physical, 4 * page);
    CHECK(mem.write_count() == before);
    mem.translate(guest + page)[5] = 1;
    uint32_t after = mem.write_count();
    CHECK(after > before && stamp(page) == after);
    CHECK(stamp(0) <= before && stamp(2 * page) <= before);
    mem.translate(guest + page)[6] = 2; // the watch is gone: no fault
    CHECK(mem.write_count() == after);
    CHECK(mem.translate(guest + page)[5] == 1 && mem.translate(guest + page)[6] == 2);

    // through the physical view: unseen until told
    mem.translate_for_host_write(guest + 2 * page)[0] = 3;
    CHECK(mem.write_count() == after && mem.translate(guest + 2 * page)[0] == 3);
    mem.written(guest + 2 * page, 1);
    CHECK(stamp(2 * page) > after);
    after = mem.write_count();
    // the other watched pages still fault
    mem.translate(guest + 3 * page)[0] = 4;
    CHECK(stamp(3 * page) > after);

    // threads writing their own watched pages at once
    mem.watch(physical, 0x100000);
    before = mem.write_count();
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 4; t++)
        threads.emplace_back([&, t] {
            for (uint32_t p = t; p < 0x100000 / page; p += 4) mem.translate(guest + p * page)[t] = uint8_t(p);
        });
    for (std::thread &t : threads) t.join();
    bool all = true;
    for (uint32_t p = 0; p < 0x100000 / page; p++) all &= stamp(p * page) > before;
    CHECK(all);

    // made writable again: numbered, watched or not
    after = mem.write_count();
    CHECK(heap->protect(guest, 0x10000, PROTECT_READ));
    CHECK(mem.write_count() == after);
    CHECK(heap->protect(guest, 0x10000, PROTECT_READ_WRITE));
    CHECK(stamp(0) > after && mem.write_count() > after);
    mem.translate(guest)[0] = 5;

    // a watcher's bounds widen a fault
    void *handle = mem.add_watcher(three_around, nullptr);
    mem.watch(physical, 0x100000);
    after = mem.write_count();
    mem.translate(guest + 8 * page)[0] = 6;
    CHECK(stamp(5 * page) > after && stamp(11 * page) > after);
    CHECK(stamp(4 * page) <= after && stamp(12 * page) <= after);
    mem.translate(guest + 11 * page)[0] = 7; // taken off with the rest: no fault
    CHECK(mem.write_count() == stamp(8 * page));
    mem.remove_watcher(handle);

    // released: the watches go with the pages
    after = mem.write_count();
    CHECK(heap->release(guest));
    CHECK(stamp(0) > after && stamp(0x100000 - page) > after);
}

int main() {
    std::string error;
    auto mem = Memory::create(error);
    CHECK(mem != nullptr);
    if (!mem) {
        std::printf("Memory::create: %s\n", error.c_str());
        return finish("memory");
    }
    CHECK(reinterpret_cast<uintptr_t>(mem->base()) >= 0x100000000ull);
    layout(*mem);
    fixed_allocations(*mem);
    views(*mem);
    virtual_4k(*mem);
    virtual_64k(*mem);
    physical(*mem);
    system(*mem);
    watches(*mem);
    mem.reset();
    // a second one after the first is gone
    mem = Memory::create(error);
    CHECK(mem != nullptr);
    return finish("memory");
}
