// The lockstep check (xenoforge/lockstep.h) on a small guest program with
// hand-written generated code: a function that calls an import and then
// another function is checked in its two segments, a deliberately wrong
// generated function is caught (the register and the bytes, with the
// interpreter's instruction), and nothing the interpreter stores reaches the
// guest's memory. store_extent covers every store.

#include "kernel_harness.h"

#include "xenoforge/lockstep.h"
#include "xenoforge/ppc_generated.h"

#include <cstring>
#include <string>

using xf::ppc::Context;

namespace {

uint32_t code = 0, thunk = 0;
bool wrong = false;

uint32_t bl(uint32_t from, uint32_t to) { return 0x48000001u | ((to - from) & 0x03FFFFFCu); }

// A: r3 += 1; *(r4) = r3; the wrong one adds 2
XF_FN(gen_a) {
    XF_BASE;
    I(addi, 0x38630001);
    if (wrong) I(addi, 0x38630001);
    I(stw, 0x90640000);
}

// B: a frame, KeQueryPerformanceFrequency, r3 += 5, A, mftb into r5
XF_FN(gen_b) {
    XF_BASE;
    I(mfspr, 0x7D8802A6);
    I(stw, 0x9181FFF8);
    I(stwu, 0x9421FFA0);
    c.lr = code + 0x50;
    ::xf::ppc::dispatch(c, thunk);
    I(addi, 0x38630005);
    c.lr = code + 0x58;
    gen_a(c);
    I(mftb, 0x7CAC42E6);
    I(addi, 0x38210060);
    I(lwz, 0x8181FFF8);
    I(mtspr, 0x7D8803A6);
}

void write_program(KernelHarness &h) {
    code = h.alloc(0x100);
    h.k->add_thunk(thunk = h.alloc(0x10), "xboxkrnl", "KeQueryPerformanceFrequency");
    const uint32_t a[] = {0x38630001, 0x90640000, 0x4E800020};
    for (uint32_t n = 0; n < 3; n++) h.k->store32(code + 4 * n, a[n]);
    const uint32_t b[] = {0x7D8802A6, 0x9181FFF8,
                          0x9421FFA0, bl(code + 0x4C, thunk),
                          0x38630005, bl(code + 0x54, code),
                          0x7CAC42E6, 0x38210060,
                          0x8181FFF8, 0x7D8803A6,
                          0x4E800020};
    for (uint32_t n = 0; n < 11; n++) h.k->store32(code + 0x40 + 4 * n, b[n]);
}

void lockstep(KernelHarness &h) {
    write_program(h);
    static xf::ppc::GeneratedFunction table[2];
    table[0] = {code, gen_a}, table[1] = {code + 0x40, gen_b};
    h.k->set_generated(table, 2);
    xf::kernel::LockstepOptions options;
    options.functions = {code + 0x40};
    options.print_limit = 5;
    std::string error;
    CHECK(h.k->enable_lockstep(options, error));
    uint32_t out = h.alloc(16);

    // right: two segments (to the import, then to the return), the same
    h.c->r[3] = 0, h.c->r[4] = out;
    h.k->call(*h.c, code + 0x40);
    xf::kernel::LockstepCounts n = h.k->lockstep_counts();
    CHECK(n.calls == 1);
    CHECK(n.segments == 2);
    CHECK(n.matched == 2);
    CHECK(n.mismatched == 0);
    CHECK(n.given_up.empty());
    CHECK(uint32_t(h.c->r[3]) == 50'000'006);
    CHECK(h.k->load32(out) == 50'000'006);

    // wrong: the second segment differs, in r3 and the word stored
    std::printf("(a mismatch is expected below)\n");
    wrong = true;
    h.c->r[3] = 0, h.c->r[4] = out;
    h.k->store32(out, 0);
    h.k->call(*h.c, code + 0x40);
    wrong = false;
    n = h.k->lockstep_counts();
    CHECK(n.calls == 2);
    CHECK(n.segments == 4);
    CHECK(n.mismatched == 1);
    CHECK(n.mismatches.size() == 1 && n.mismatches.count(code + 0x50) == 1);
    // the generated code's run is the real one; the interpreter's stores
    // stayed in its private view
    CHECK(uint32_t(h.c->r[3]) == 50'000'007);
    CHECK(h.k->load32(out) == 50'000'007);
    std::string report = h.k->lockstep_report();
    CHECK(report.find("2 calls checked, 4 segments compared, 3 the same, 1 different, 0 different after") !=
          std::string::npos);

    // a call not chosen runs unchecked
    h.c->r[3] = 0, h.c->r[4] = out;
    h.k->call(*h.c, code);
    CHECK(h.k->lockstep_counts().calls == 2);
}

void sampling() {
    KernelHarness h;
    if (!h.init()) {
        CHECK(false);
        return;
    }
    write_program(h);
    static xf::ppc::GeneratedFunction table[2];
    table[0] = {code, gen_a}, table[1] = {code + 0x40, gen_b};
    h.k->set_generated(table, 2);
    xf::kernel::LockstepOptions options;
    options.sample = 2;
    std::string error;
    CHECK(h.k->enable_lockstep(options, error));
    uint32_t out = h.alloc(16);
    for (int k = 0; k < 4; k++) {
        h.c->r[3] = 0, h.c->r[4] = out;
        h.k->call(*h.c, code);
    }
    xf::kernel::LockstepCounts n = h.k->lockstep_counts();
    CHECK(n.calls == 2);
    CHECK(n.matched == 2 && n.mismatched == 0);
}

void extents() {
    // every store the decoder knows has an extent (st*, dcbz*), and a few
    // are where memory.h puts them
    Context c{};
    for (int n = 0; n < 32; n++) c.r[n] = 0x1000 * uint64_t(n);
    c.xer_bc = 8;
    for (uint32_t k = 0; k < xf::ppc::OPCODE_COUNT; k++) {
        auto op = xf::ppc::Opcode(k);
        const xf::ppc::OpcodeInfo &info = xf::ppc::info(op);
        bool stores =
            info.group == 'm' && (std::strncmp(info.name, "st", 2) == 0 || std::strncmp(info.name, "dcbz", 4) == 0);
        uint32_t address = 0, size = 0;
        // RT 3, RA 4, RB 5, D 8
        xf::ppc::Instr i{info.opcode | 3u << 21 | 4u << 16 | 5u << 11 | 8u};
        bool got = xf::ppc::store_extent(c, op, i, &address, &size);
        if (got != stores || (stores && !size)) std::printf("store_extent: %s\n", info.name);
        CHECK(got == stores);
    }
    uint32_t address, size;
    CHECK(xf::ppc::store_extent(c, xf::ppc::Opcode::stw, xf::ppc::Instr{0x90640008}, &address, &size) &&
          address == 0x4008 && size == 4);
    CHECK(xf::ppc::store_extent(c, xf::ppc::Opcode::stmw, xf::ppc::Instr{0xBFA40010}, &address, &size) &&
          address == 0x4010 && size == 12);
    CHECK(xf::ppc::store_extent(c, xf::ppc::Opcode::std, xf::ppc::Instr{0xF8640010}, &address, &size) &&
          address == 0x4010 && size == 8);
    // dcbz r4,r5: the 128-byte line of 0x9000
    CHECK(xf::ppc::store_extent(c, xf::ppc::Opcode::dcbz, xf::ppc::Instr{0x7C042FEC}, &address, &size) &&
          address == 0x9000 && size == 128);
}

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("lockstep: no kernel\n"), 1;
    lockstep(h);
    sampling();
    extents();
    return finish("lockstep");
}
