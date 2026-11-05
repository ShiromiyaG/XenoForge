// A guest thread's registers as the generated code and the interpreter see
// them (docs/recompiler.md, "The context"), laid out after Xenia's
// PPCContext (src/xenia/cpu/ppc/ppc_context.h, BSD; see LICENSE-xenia):
// the condition register as a byte per bit, vectors with their words in
// guest order.

#pragma once

#include "xenoforge/ppc.h"

#include <cstdint>
#include <cstring>

namespace xf::ppc {

// A VMX register. Words are in guest order (u32[0] is the first word in
// memory); halfwords and bytes inside them are host order, so guest element
// n is u16[n ^ 1] and u8[n ^ 3] (Xenia's vec128 conventions). u64[0] holds
// words 1 and 0, not the guest's first doubleword.
union alignas(16) Vec128 {
    uint8_t u8[16];
    int8_t i8[16];
    uint16_t u16[8];
    int16_t i16[8];
    uint32_t u32[4];
    int32_t i32[4];
    float f32[4];
    uint64_t u64[2];
};
static_assert(sizeof(Vec128) == 16);

struct Context;

// What the code asks of the system. Each is set by whoever runs the code
// (the runtime, a test); the defaults do nothing.
struct System {
    void (*syscall)(Context &c) = nullptr;                    // sc
    void (*trap)(Context &c, uint32_t address) = nullptr;     // a trap instruction that fired
    void (*unimplemented)(Context &c, Opcode op, uint32_t code) = nullptr;
    uint64_t (*time_base)(Context &c) = nullptr;              // mftb
};

struct alignas(64) Context {
    uint64_t r[32];
    uint64_t lr, ctr;
    double f[32];
    Vec128 v[128];
    // CR0-CR7: lt, gt, eq, so for each field (cr[4 * n + bit])
    uint8_t cr[32];
    uint8_t xer_ca, xer_ov, xer_so;
    uint8_t vscr_sat;
    uint32_t fpscr;
    // lwarx/ldarx's reservation
    uint32_t reserve_address;
    uint64_t reserve_value;
    bool reserved;
    // msr, as mfmsr/mtmsr see it (EE only)
    uint64_t msr;
    // where guest address 0 is
    uint8_t *base;
    // the instruction being run, for the system's callbacks
    uint32_t pc;
    System *system;

    // CR as the 32-bit register (mfcr), field 0 in the top nibble
    uint32_t cr_word() const {
        uint32_t value = 0;
        for (int i = 0; i < 32; i++) value |= uint32_t(cr[i] & 1) << (31 - i);
        return value;
    }
    void set_cr_word(uint32_t value) {
        for (int i = 0; i < 32; i++) cr[i] = (value >> (31 - i)) & 1;
    }
};

} // namespace xf::ppc
