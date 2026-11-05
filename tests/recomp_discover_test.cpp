// Discovery on a small image assembled here, no game data: the data pointer
// scan (a pointer that splits a function is dropped, one into code another
// function ran on into is kept, a lis/addi pair is found, a jump table's
// address is not a start, an empty method only a vtable reaches comes back),
// the gap filler (a run of leaf methods longer than its old rounds, a dead
// blr after a tail call, an exception handler's record before a function),
// a jump table bounded by a cmplwi after a clrlwi, a branch into a function
// the walk ran on into past a call that never returns, and the functions that
// switch the stack (a longjmp, a fiber switch).

#include "check.h"
#include "xenoforge/recomp.h"

#include <cstdint>
#include <vector>

using namespace xf;
using namespace xf::recomp;

namespace {

constexpr uint32_t T = 0x82000000;     // .text
constexpr uint32_t RDATA = 0x82001000; // .rdata

uint32_t addi(uint32_t d, uint32_t a, int32_t v) { return 14u << 26 | d << 21 | a << 16 | (uint32_t(v) & 0xFFFF); }
uint32_t li(uint32_t d, int32_t v) { return addi(d, 0, v); }
uint32_t lis(uint32_t d, uint32_t v) { return 15u << 26 | d << 21 | (v & 0xFFFF); }
uint32_t lwz(uint32_t d, int32_t off, uint32_t a) { return 32u << 26 | d << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t stwu(uint32_t s, int32_t off, uint32_t a) { return 37u << 26 | s << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t cmpwi(uint32_t cr, uint32_t a, int32_t v) { return 11u << 26 | cr << 23 | a << 16 | (uint32_t(v) & 0xFFFF); }
uint32_t cmplwi(uint32_t cr, uint32_t a, uint32_t v) { return 10u << 26 | cr << 23 | a << 16 | (v & 0xFFFF); }
uint32_t rlwinm(uint32_t a, uint32_t s, uint32_t sh, uint32_t mb, uint32_t me) {
    return 21u << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1;
}
uint32_t lwzx(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 23u << 1; }
uint32_t mtctr(uint32_t s) { return 0x7C0903A6 | s << 21; }
uint32_t b(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC); }
uint32_t bl(uint32_t from, uint32_t to) { return b(from, to) | 1; }
uint32_t bc(uint32_t bo, uint32_t bi, uint32_t from, uint32_t to) {
    return 16u << 26 | bo << 21 | bi << 16 | ((to - from) & 0xFFFC);
}
constexpr uint32_t BLR = 0x4E800020, BCTR = 0x4E800420, MFLR_R12 = 0x7D8802A6, NOP = 0x60000000;

struct Builder {
    Image image;
    Builder() {
        image.base = T;
        image.entry = T;
        image.bytes.assign(0x1100, 0);
        image.sections.push_back({".text", T, 0x1000, true, false});
        image.sections.push_back({".rdata", RDATA, 0x100, false, false});
    }
    void put(uint32_t at, uint32_t w) {
        uint8_t *p = &image.bytes[at - T];
        p[0] = uint8_t(w >> 24), p[1] = uint8_t(w >> 16), p[2] = uint8_t(w >> 8), p[3] = uint8_t(w);
    }
    void code(uint32_t at, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) put(at, w), at += 4;
    }
};

// where things are
constexpr uint32_t A = T + 0x000;  // the entry: calls U, W and the table's function, makes Q's address
constexpr uint32_t U = T + 0x100;  // a loop whose exit branch crosses P
constexpr uint32_t P = U + 0x14;   // after a b, held by .rdata: splits U
constexpr uint32_t W = T + 0x200;  // calls Z, then runs on into V
constexpr uint32_t V = W + 4;      // stwu r1, held by .rdata
constexpr uint32_t TB = T + 0x300; // a switch: clrlwi to 5 bits, then cmplwi 2
constexpr uint32_t TABLE = TB + 0x28;
constexpr uint32_t Z = T + 0x400;    // blr alone, a bl target
constexpr uint32_t Q = T + 0x500;    // only lis/addi make its address
constexpr uint32_t LEAF = T + 0x600; // 12 leaf methods of 8 bytes
constexpr uint32_t G = T + 0x700;    // b Z, then a dead blr
constexpr uint32_t EMPTY = T + 0x710; // blr alone, held by .rdata
constexpr uint32_t E = T + 0x808;    // .pdata with exception data: its handler record is at E - 8
constexpr uint32_t NR = T + 0x900;   // a call that never returns (a blr here: discovery cannot tell)
constexpr uint32_t H = T + 0x910;    // a gap: li r3, 14; bl NR; then R, a routine of its own
constexpr uint32_t R = H + 8;        // only S's b reaches it
constexpr uint32_t S = T + 0x930;    // b R, a stub H2 calls
constexpr uint32_t H2 = T + 0x940;
constexpr uint32_t LJ = T + 0xA00;    // lwz r1, 0(r3): a longjmp
constexpr uint32_t FS = T + 0xA20;    // b to KeSetCurrentStackPointers' thunk: a fiber switch
constexpr uint32_t THUNK = T + 0xA40; // that import's thunk

Image build() {
    Builder m;
    m.code(A, {MFLR_R12, bl(A + 4, U), bl(A + 8, W), bl(A + 12, TB), lis(11, Q >> 16), addi(11, 11, int32_t(Q & 0xFFFF)), BLR});
    m.code(U, {li(4, 0), cmpwi(0, 4, 10), bc(4, 0, U + 8, U + 0x1C), addi(4, 4, 1), b(U + 0x10, U + 4), li(5, 1), NOP, BLR});
    m.code(W, {bl(W, Z), stwu(1, -0x10, 1), addi(1, 1, 0x10), BLR});
    m.code(TB, {lwz(11, 0, 3), rlwinm(11, 11, 0, 27, 31), cmplwi(6, 11, 2), bc(12, 25, TB + 0xC, TB + 0x58),
                lis(12, TABLE >> 16), rlwinm(0, 11, 2, 0, 29), addi(12, 12, int32_t(TABLE & 0xFFFF)), lwzx(0, 12, 0),
                mtctr(0), BCTR, TB + 0x34, TB + 0x3C, TB + 0x44, li(3, 0), BLR, li(3, 1), BLR, li(3, 2), BLR, NOP, NOP,
                NOP, li(3, -1), BLR});
    m.code(Z, {BLR});
    m.code(Q, {li(3, 7), BLR});
    for (uint32_t k = 0; k < 12; k++) m.code(LEAF + 8 * k, {lwz(3, int32_t(4 * k), 3), BLR});
    m.code(G, {b(G, Z), BLR, li(3, 3), BLR, BLR});
    m.code(E - 8, {A, RDATA + 0x40, li(3, 0), BLR});
    m.code(NR, {BLR});
    m.code(H, {li(3, 14), bl(H + 4, NR), lwz(4, 0x100, 13), li(3, 0), BLR});
    m.code(S, {b(S, R)});
    m.code(H2, {MFLR_R12, bl(H2 + 4, S), BLR});
    m.code(LJ, {lwz(1, 0, 3), lwz(0, 4, 3), 0x7C0803A6 /* mtlr r0 */, BLR});
    m.code(FS, {lwz(3, 0x30, 3), b(FS + 4, THUNK)});
    m.code(THUNK, {li(3, 0), BLR, NOP, NOP});
    m.image.imports.push_back({"xboxkrnl.exe", 0x9B, THUNK}); // KeSetCurrentStackPointers
    m.code(RDATA, {P, V, EMPTY});
    m.image.pdata.push_back({A, 0x1C, 1, false});
    m.image.pdata.push_back({TB, 0x60, 0, false});
    m.image.pdata.push_back({E, 8, 0, true});
    return m.image;
}

bool has(const std::vector<uint32_t> &v, uint32_t a) {
    for (uint32_t x : v)
        if (x == a) return true;
    return false;
}

} // namespace

int main() {
    Image image = build();

    std::vector<uint32_t> found = scan_pointers(image);
    CHECK(has(found, P) && has(found, V) && has(found, EMPTY)); // words in .rdata
    CHECK(has(found, Q));                                       // lis/addi
    CHECK(has(found, TABLE));                                   // lis/addi too, after a bctr
    CHECK(!has(found, RDATA + 0x40));                           // not code

    // without the scan: W runs on through V, and the empty method is no function
    Discovery plain = discover(image, {.scan_pointers = false});
    CHECK(plain.functions.count(W) && plain.functions.at(W).size == 0x10);
    CHECK(!plain.functions.count(V) && !plain.functions.count(P) && !plain.functions.count(EMPTY));

    Discovery d = discover(image);
    // V: a start W ran on into, W now ending in a tail call to it
    CHECK(d.functions.count(V) && d.functions.at(V).source == Source::Pointer && d.functions.at(V).size == 0xC);
    CHECK(d.functions.count(W) && d.functions.at(W).size == 4 && d.functions.at(W).tail_calls.count(V));
    // P would leave U's exit branch pointing nowhere: dropped
    CHECK(!d.functions.count(P));
    CHECK(d.functions.count(U) && d.functions.at(U).size == 0x20);
    CHECK(d.counts["pointer scan: dropped as splits"] == 1);
    CHECK(d.unresolved.empty());
    // the empty method only .rdata reaches
    CHECK(d.functions.count(EMPTY) && d.functions.at(EMPTY).source == Source::Pointer && d.functions.at(EMPTY).size == 4);
    // Q was a gap already; the table's address is not a start
    CHECK(d.functions.count(Q) && !d.functions.count(TABLE));

    // the table: 3 entries (the cmplwi), not 32 (the mask)
    CHECK(d.tables.count(TB + 0x24));
    if (d.tables.count(TB + 0x24)) {
        const JumpTable &t = d.tables.at(TB + 0x24);
        CHECK(t.kind == JumpTable::Absolute && t.table == TABLE && t.index == 11);
        CHECK((t.targets == std::vector<uint32_t>{TB + 0x34, TB + 0x3C, TB + 0x44}));
    }

    // every leaf method, in one pass of the gap filler
    for (uint32_t k = 0; k < 12; k++) {
        CHECK(d.functions.count(LEAF + 8 * k));
        if (d.functions.count(LEAF + 8 * k)) CHECK(d.functions.at(LEAF + 8 * k).size == 8);
    }
    // a blr after a tail call is dead code; what follows it is a function
    CHECK(d.functions.count(G) && !d.functions.count(G + 4) && d.functions.count(G + 8));
    // the handler's record before E is data
    CHECK(!d.functions.count(E - 8) && !d.functions.count(E - 4));
    CHECK(d.functions.count(E) && d.functions.at(E).source == Source::Pdata);
    // R, inside the gap H ran on into past a call: S's branch to it reaches no
    // start without the scan's passes; with them R is one, H ending in a tail
    // call to it
    CHECK(!plain.functions.count(R) && plain.functions.count(H) && plain.functions.at(H).size == 0x14);
    CHECK(plain.unresolved.size() == 1 && plain.unresolved[0].site == S && plain.unresolved[0].target == R);
    CHECK(d.functions.count(R) && d.functions.at(R).source == Source::Branch && d.functions.at(R).size == 0xC);
    CHECK(d.functions.count(H) && d.functions.at(H).size == 8 && d.functions.at(H).tail_calls.count(R));
    CHECK(d.counts["pointer scan: branch targets added"] == 1);
    // what switches the stack is left to the interpreter: r1 loaded from a
    // buffer (LJ), a branch to KeSetCurrentStackPointers (FS); not an epilogue
    CHECK(d.functions.count(LJ) && d.functions.at(LJ).switches_stack);
    CHECK(d.functions.count(FS) && d.functions.at(FS).switches_stack && d.functions.at(FS).tail_calls.count(THUNK));
    CHECK(d.functions.count(U) && !d.functions.at(U).switches_stack && !d.functions.at(A).switches_stack);
    CHECK(d.counts["functions that switch the stack (left to the interpreter)"] == 2);
    return finish("recomp_discover");
}
