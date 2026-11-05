// A kernel to call exports on, for the kernel's tests: the guest memory, a
// Kernel, a thread's context with a stack and a PCR (as boot sets one up),
// and call(), which runs an export by name through a thunk (Kernel::add_thunk)
// with the arguments as a guest caller passes them (r3-r10, then the stack).

#pragma once

#include "check.h"

#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>

struct KernelHarness {
    std::unique_ptr<xf::Memory> memory;
    std::unique_ptr<xf::kernel::Kernel> k;
    std::unique_ptr<xf::ppc::Context> c;
    uint32_t stack = 0, pcr = 0;
    std::map<std::string, uint32_t> thunks;
    uint32_t thunk_page = 0, thunk_next = 0;

    bool init() {
        std::string error;
        memory = xf::Memory::create(error);
        if (!memory) {
            std::printf("memory: %s\n", error.c_str());
            return false;
        }
        k = std::make_unique<xf::kernel::Kernel>(*memory);
        c = std::make_unique<xf::ppc::Context>();
        std::memset(c.get(), 0, sizeof(xf::ppc::Context));
        k->attach(*c);
        xf::Heap *stacks = memory->heap_by_type(false, 64 * 1024);
        if (!stacks->alloc(1u << 20, 64 * 1024, xf::ALLOCATE_RESERVE | xf::ALLOCATE_COMMIT, xf::PROTECT_READ_WRITE,
                           true, &stack))
            return false;
        pcr = memory->system_alloc(0x2D8);
        c->r[1] = stack + (1u << 20) - 0x100;
        c->r[13] = pcr;
        thunk_page = memory->system_alloc(0x1000);
        return true;
    }

    // the export's result (r3), the arguments past the eighth on the stack as
    // doublewords (varargs, 64-bit values); a fixed 32-bit argument there is
    // a word at the slot's address (arg()), so a test of one stores it itself
    uint64_t call(const char *library, const char *name, std::initializer_list<uint64_t> args = {}) {
        std::string key = std::string(library) + "!" + name;
        auto it = thunks.find(key);
        if (it == thunks.end()) {
            uint32_t address = thunk_page + 8 * thunk_next++;
            k->add_thunk(address, library, name);
            it = thunks.emplace(key, address).first;
        }
        // past the eighth, a 64-bit value at r1 + 0x54 + 8 * (n - 8) (what
        // arg64 and the printf family's 64-bit reads take); a 32-bit one
        // there for arg and in the next word too, where the printf family's
        // 32-bit reads (Xenia's, the low half of those 8 bytes) look
        int n = 0;
        for (uint64_t a : args) {
            uint32_t slot = uint32_t(c->r[1]) + 0x54 + 8 * (n - 8);
            if (n < 8)
                c->r[3 + n] = a;
            else if (a > 0xFFFFFFFFull)
                k->store64(slot, a);
            else
                k->store32(slot, uint32_t(a)), k->store32(slot + 4, uint32_t(a));
            n++;
        }
        k->call(*c, it->second);
        return c->r[3];
    }
    uint32_t call32(const char *library, const char *name, std::initializer_list<uint64_t> args = {}) {
        return uint32_t(call(library, name, args));
    }
    uint32_t krnl(const char *name, std::initializer_list<uint64_t> args = {}) {
        return call32("xboxkrnl", name, args);
    }

    // scratch guest memory: a string, zeroed bytes
    uint32_t string(const char *s) {
        uint32_t address = memory->system_alloc(uint32_t(std::strlen(s) + 1));
        std::memcpy(k->host(address), s, std::strlen(s) + 1);
        return address;
    }
    uint32_t wide(const char16_t *s) {
        size_t n = std::char_traits<char16_t>::length(s);
        uint32_t address = memory->system_alloc(uint32_t(2 * (n + 1)));
        for (size_t i = 0; i <= n; i++) k->store16(address + uint32_t(2 * i), uint16_t(s[i]));
        return address;
    }
    uint32_t alloc(uint32_t size) { return memory->system_alloc(size); }
    std::string read(uint32_t address) { return reinterpret_cast<const char *>(k->host(address)); }
};
