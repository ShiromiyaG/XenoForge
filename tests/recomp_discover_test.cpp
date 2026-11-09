// Discovery on a small image assembled here, no game data: the data pointer
// scan (a pointer that splits a function is dropped, one into code another
// function ran on into is kept, a lis/addi pair is found, a jump table's
// address is not a start, an empty method only a vtable reaches comes back),
// the gap filler (a run of leaf methods longer than its old rounds, a dead
// blr after a tail call, an exception handler's record before a function),
// a jump table bounded by a cmplwi after a clrlwi, one bounded by its mask
// alone that is shorter than the mask (cut where its entries leave the
// function), a branch into a function the walk ran on into past a call that
// never returns, the functions that
// switch the stack (a longjmp, fiber switches) and the resume sites (after a
// call to a setjmp, itself or behind a tail branch; a catch continuation a
// funclet returns), and how those are emitted.

#include "check.h"
#include "xenoforge/recomp.h"

#include <cstdint>
#include <string>
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
uint32_t stw(uint32_t s, int32_t off, uint32_t a) { return 36u << 26 | s << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t std_(uint32_t s, int32_t off, uint32_t a) { return 62u << 26 | s << 21 | a << 16 | (uint32_t(off) & 0xFFFC); }
uint32_t stwu(uint32_t s, int32_t off, uint32_t a) { return 37u << 26 | s << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t cmpwi(uint32_t cr, uint32_t a, int32_t v) { return 11u << 26 | cr << 23 | a << 16 | (uint32_t(v) & 0xFFFF); }
uint32_t cmplwi(uint32_t cr, uint32_t a, uint32_t v) { return 10u << 26 | cr << 23 | a << 16 | (v & 0xFFFF); }
uint32_t rlwinm(uint32_t a, uint32_t s, uint32_t sh, uint32_t mb, uint32_t me) {
    return 21u << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1;
}
uint32_t lwzx(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 23u << 1; }
uint32_t lbzx(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 87u << 1; }
uint32_t add(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 266u << 1; }
uint32_t mtctr(uint32_t s) { return 0x7C0903A6 | s << 21; }
uint32_t b(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC); }
uint32_t bl(uint32_t from, uint32_t to) { return b(from, to) | 1; }
uint32_t bc(uint32_t bo, uint32_t bi, uint32_t from, uint32_t to) {
    return 16u << 26 | bo << 21 | bi << 16 | ((to - from) & 0xFFFC);
}
constexpr uint32_t BLR = 0x4E800020, BCTR = 0x4E800420, MFLR_R12 = 0x7D8802A6, MFLR_R0 = 0x7C0802A6, NOP = 0x60000000;

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
constexpr uint32_t SJ = T + 0xB00;    // setjmp: r1 and LR kept in r3's buffer, returns 0
constexpr uint32_t SJW = T + 0xB20;   // b SJ: it saves the stack through a tail branch
constexpr uint32_t SC = T + 0xB40;    // calls SJW, SJ and FS2: resume sites after the first two
constexpr uint32_t FS2 = T + 0xB80;   // keeps r1 and switches (a SwitchToFiber): not a setjmp
constexpr uint32_t FS3 = T + 0xBA0;   // a bc to the thunk
constexpr uint32_t EP = T + 0xC08;    // .pdata with exception data, its handler record at EP - 8
constexpr uint32_t CONT = EP + 0x10;  // inside EP, reached only by the funclet's return
constexpr uint32_t FUNCLET = T + 0xC40; // a catch block: lis r3, addi r3 to CONT, blr
constexpr uint32_t MB = T + 0xD00;          // a switch on r9 & 7, no compare: byte offsets from MB_CASES
constexpr uint32_t MB_CASES = MB + 0x2C;    // its cases
constexpr uint32_t MB_NEXT = MB + 0x44;     // the function after it
constexpr uint32_t MB_TABLE = RDATA + 0x80; // 3 cases, then other data (into MB_NEXT)

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
    m.code(SJ, {stw(1, 0, 3), MFLR_R0, stw(0, 4, 3), li(3, 0), BLR});
    m.code(SJW, {b(SJW, SJ)});
    m.code(SC, {MFLR_R12, stwu(1, -0x20, 1), bl(SC + 8, SJW), cmpwi(0, 3, 0), bl(SC + 0x10, SJ), bl(SC + 0x14, FS2),
                addi(1, 1, 0x20), BLR});
    m.code(FS2, {std_(1, 0x30, 5), b(FS2 + 4, THUNK)});
    m.code(FS3, {cmpwi(0, 3, 0), bc(12, 2, FS3 + 4, THUNK), BLR});
    m.code(EP - 8, {A, RDATA + 0x40});
    m.code(EP, {MFLR_R12, stwu(1, -0x20, 1), bl(EP + 8, Z), b(EP + 0xC, EP + 0x14), li(3, 1), addi(1, 1, 0x20), BLR});
    m.code(FUNCLET, {lis(3, CONT >> 16), addi(3, 3, int32_t(CONT & 0xFFFF)), BLR});
    m.code(MB,
           {lwz(9, 0, 3), rlwinm(10, 9, 0, 29, 31), lis(12, MB_TABLE >> 16), addi(12, 12, int32_t(MB_TABLE & 0xFFFF)),
            lbzx(0, 12, 10), rlwinm(0, 0, 2, 0, 29), lis(12, MB_CASES >> 16), addi(12, 12, int32_t(MB_CASES & 0xFFFF)),
            add(12, 12, 0), mtctr(12), BCTR, li(3, 0), BLR, li(3, 1), BLR, li(3, 2), BLR});
    m.code(MB_NEXT, {li(3, 9), BLR, BLR});
    m.code(MB_TABLE, {0x00020407, 0x00000000});
    m.code(RDATA, {P, V, EMPTY});
    m.image.pdata.push_back({A, 0x1C, 1, false});
    m.image.pdata.push_back({TB, 0x60, 0, false});
    m.image.pdata.push_back({E, 8, 0, true});
    m.image.pdata.push_back({EP, 0x1C, 2, true});
    m.image.pdata.push_back({FUNCLET, 0xC, 0, false});
    m.image.pdata.push_back({MB, 0x44, 0, false});
    m.image.pdata.push_back({MB_NEXT, 0xC, 0, false});
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

    // the table the mask alone bounds (8 entries): its fourth entry goes
    // into the next function, so it has 3; an index past them dispatches
    CHECK(d.tables.count(MB + 0x28));
    if (d.tables.count(MB + 0x28)) {
        const JumpTable &t = d.tables.at(MB + 0x28);
        CHECK(t.kind == JumpTable::ShiftedByteOffset && t.table == MB_TABLE && t.base == MB_CASES && t.shift == 2);
        CHECK((t.targets == std::vector<uint32_t>{MB_CASES, MB_CASES + 8, MB_CASES + 0x10}));
    }
    CHECK(d.counts["jump tables cut at their function's end"] == 1);
    CHECK(d.functions.count(MB) && d.functions.at(MB).tail_calls.empty());

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
    // what switches the stack: r1 loaded from a buffer (LJ), a branch to
    // KeSetCurrentStackPointers (FS, FS2, and FS3's bc); not an epilogue
    CHECK(d.functions.count(LJ) && d.functions.at(LJ).switches_stack);
    CHECK(d.functions.count(FS) && d.functions.at(FS).switches_stack && d.functions.at(FS).tail_calls.count(THUNK));
    CHECK(d.functions.count(FS2) && d.functions.at(FS2).switches_stack);
    CHECK(d.functions.count(FS3) && d.functions.at(FS3).switches_stack);
    CHECK(d.functions.count(U) && !d.functions.at(U).switches_stack && !d.functions.at(A).switches_stack);
    CHECK(d.counts["functions that switch the stack"] == 4);
    CHECK(d.stack_thunks == std::set<uint32_t>{THUNK});

    // what saves the stack and returns (SJ, SJW through its tail branch); FS2
    // keeps r1 too, but switches: its callers go on through the switch back
    CHECK(d.functions.count(SJ) && d.functions.at(SJ).saves_stack && !d.functions.at(SJ).switches_stack);
    CHECK(d.functions.count(SJW) && d.functions.at(SJW).saves_stack);
    CHECK(!d.functions.at(A).saves_stack && !d.functions.at(SC).saves_stack);
    // the resume sites: after the calls to SJW and SJ, not FS2; the catch
    // continuation, which the pointer scan also took for a start
    CHECK(d.functions.count(SC) && (d.functions.at(SC).resumes == std::set<uint32_t>{SC + 0xC, SC + 0x14}));
    CHECK(d.functions.count(EP) && (d.functions.at(EP).resumes == std::set<uint32_t>{CONT}));
    CHECK(d.functions.count(CONT) && d.functions.at(CONT).source == Source::Pointer);
    CHECK(d.functions.at(FUNCLET).resumes.empty() && d.functions.at(A).resumes.empty());
    CHECK(d.counts["resume sites: after a call that saves the stack"] == 2);
    CHECK(d.counts["resume sites: catch continuations"] == 1);
    CHECK(d.functions.at(SC).call_sites.size() == 3);

    // Emitted: a switch's returns checked, its branch to the thunk a
    // switch_stack with its own return address; the setjmp's caller a body
    // the runtime runs again at its resume sites (resumable), without tail
    // calls; the continuation a label of EP's
    auto emitted = [&](uint32_t a) { return emit_function(image, d, d.functions.at(a), "xf_"); };
    auto contains = [](const std::string &text, const std::string &part) {
        return text.find(part) != std::string::npos;
    };
    std::string lj = emitted(LJ), fs = emitted(FS), fs3 = emitted(FS3), sc = emitted(SC), ep = emitted(EP);
    CHECK(contains(lj, "const uint32_t xf_lr = uint32_t(c.lr), xf_sp = uint32_t(c.r[1]);"));
    CHECK(contains(lj, "XF_CHECKED_RETURN();") && !contains(lj, "    return;"));
    CHECK(contains(fs, "{ ::xf::ppc::switch_stack(c, 0x82000A40, xf_lr); XF_CHECKED_RETURN(); }"));
    // (a branch backward, so a yield point first: docs/recompiler.md, "Yield points")
    CHECK(contains(fs3, "if (BC(12, 2)) { XF_YIELD(); { ::xf::ppc::switch_stack(c, 0x82000A40, xf_lr); "
                        "XF_CHECKED_RETURN(); } }"));
    CHECK(contains(sc, "static constexpr uint32_t xf_sites[] = {0x82000B4Cu, 0x82000B54u};") &&
          contains(sc, "::xf::ppc::resumable(c, XF_RESUMABLE_BODY {") &&
          contains(sc, "case 0x82000B4C: goto L_82000B4C;") && contains(sc, "case 0x82000B54: goto L_82000B54;") &&
          contains(sc, "L_82000B4C:") && contains(sc, "}, uint32_t(c.lr), uint32_t(c.r[1]), xf_sites, 2);") &&
          contains(sc, "xf_82000B20(c);") && contains(sc, "c.lr = 0x82000B58; xf_82000B80(c);"));
    CHECK(contains(ep, "case 0x82000C18: goto L_82000C18;") && contains(ep, "L_82000C18:"));
    // the generated code never catches (the runtime does)
    CHECK(!contains(sc, "XF_TAIL") && !contains(sc, "try {") && !contains(ep, "catch") &&
          !contains(emitted(A), "resumable"));
    std::string mb = emitted(MB);
    CHECK(contains(mb, "case 0x82000D3C:") && !contains(mb, "case 0x82000D48:") &&
          contains(mb, "default: ::xf::ppc::dispatch(c, uint32_t(c.ctr) & ~3u);"));
    CHECK(!contains(lj + fs + sc + ep + mb, "xf_parts"));

    // A very long function (recomp_emit.cpp, "parts"): 3,000 additions, a
    // loop of 1,000 more (from a label, back by a bc), 999 more and a blr. Its
    // stretches go into parts of up to 1,024 instructions: 1,024, 1,024 and
    // 952; the loop's body, begun anew at its label, called after it; the
    // last 999. The loop's branch stays in the body.
    {
        constexpr uint32_t N = 5000, LOOP = T + 3000 * 4, BACK = T + 4000 * 4;
        Image big;
        big.base = T, big.entry = T;
        big.bytes.assign(size_t(N + 4) * 4, 0);
        big.sections.push_back({".text", T, uint32_t(big.bytes.size()), true, false});
        auto put = [&](uint32_t at, uint32_t w) {
            uint8_t *p = &big.bytes[at - T];
            p[0] = uint8_t(w >> 24), p[1] = uint8_t(w >> 16), p[2] = uint8_t(w >> 8), p[3] = uint8_t(w);
        };
        for (uint32_t k = 0; k < N; k++) put(T + k * 4, addi(3, 3, 1));
        put(BACK, bc(4, 0, BACK, LOOP));
        put(T + N * 4, BLR);
        Discovery bd = discover(big, {.scan_pointers = false});
        CHECK(bd.functions.count(T) && bd.functions.at(T).size == (N + 1) * 4);
        if (bd.functions.count(T)) {
            const std::string text = emit_function(big, bd, bd.functions.at(T), "xf_");
            CHECK(contains(text, "    struct xf_parts {\n        XF_PART(p0) {\n"));
            CHECK(contains(text, "XF_PART(p4) {") && !contains(text, "XF_PART(p5)"));
            CHECK(contains(text, "    xf_parts::p0(c);\n    xf_parts::p1(c);\n    xf_parts::p2(c);\n"));
            CHECK(contains(text, "L_82002EE0:\n    xf_parts::p3(c);\n")); // LOOP
            CHECK(contains(text, "if (BC(4, 0)) { XF_YIELD(); goto L_82002EE0; }\n"));
            CHECK(contains(text, "    xf_parts::p4(c);\n    // 82004E20 ") && contains(text, "    return;\n}\n"));
        }
    }
    return finish("recomp_discover");
}
