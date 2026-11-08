// A guest program for kernel_exceptions_test (docs/runtime.md, "Structured
// exceptions"): PowerPC functions written as the XDK's compiler writes them
// (prologues, save helpers, .pdata, __C_specific_handler's scope tables,
// funclets taking their frame in r12), raising and catching through the
// kernel's exception dispatch. The test runs it interpreted, and seh_gen
// (tests/seh_gen.cpp) recompiles the same image at build time so the test
// runs it generated too.
//
// The C++ handler is the test's host code (seh_cxx_handler in the test), at
// SEH_CXX_HANDLER with a guest prologue and .pdata of its own so walks pass
// through its frame as through the CRT's: it follows the protocol the games'
// C++ runtime follows with the kernel (an RtlUnwind back into its own frame,
// the captured context swapped in at the target frame, the catch funclet
// called with the frame in r12, then a jump to the continuation it returns).

#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace seh {

constexpr uint32_t BASE = 0x82100000, SIZE = 0x10000;
constexpr uint32_t THUNKS = BASE + 0x100, CODE = BASE + 0x1000, CODE_END = BASE + 0x7000;
constexpr uint32_t DATA = BASE + 0x8000;
// the log: a count, then the words logged
constexpr uint32_t LOG = DATA, NAME = DATA + 0x400, FCONST = DATA + 0x420, FSLOT = DATA + 0x428;
// the C++ handler's two contexts: the one it captures before its unwind, and
// the target frame's
constexpr uint32_t UNWIND_CTX = DATA + 0x1000, FRAME_CTX = DATA + 0x2000;
constexpr uint32_t TABLES = DATA + 0x3000, PDATA = BASE + 0xC000;
constexpr uint32_t TYPE_T = 0x54000000;

struct Import {
    const char *name;
    uint16_t ordinal;
    uint32_t thunk;
};
inline const Import IMPORTS[] = {
    {"RtlRaiseException", 0x136, THUNKS + 0x00},    {"RtlUnwind", 0x147, THUNKS + 0x10},
    {"__C_specific_handler", 0x1A5, THUNKS + 0x20}, {"RtlCaptureContext", 0x119, THUNKS + 0x30},
    {"DbgBreakPoint", 0x001, THUNKS + 0x40},
};

struct Function {
    uint32_t begin = 0, end = 0, prolog = 0;
    bool exception = false;
};

struct Program {
    std::vector<uint8_t> image; // SIZE bytes from BASE
    std::vector<Function> functions;
    std::map<std::string, uint32_t> labels;
    uint32_t pdata_count = 0;
    uint32_t at(const std::string &label) const { return labels.at(label); }
};

class Asm {
  public:
    enum Kind { REL24, REL14, HA, LO, WORD };
    Asm(std::map<std::string, uint32_t> &labels, uint32_t base) : labels_(labels), base_(base) {}
    uint32_t here() const { return base_ + uint32_t(4 * words_.size()); }
    void label(const std::string &name) {
        if (!labels_.emplace(name, here()).second) throw std::runtime_error("label twice: " + name);
    }
    void w(uint32_t x) { words_.push_back(x); }
    void fix(Kind kind, const std::string &label, uint32_t x) {
        fixes_.push_back({words_.size(), label, kind});
        w(x);
    }
    void resolve(std::vector<uint8_t> &image) const {
        for (const Fix &f : fixes_) {
            uint32_t target = labels_.at(f.label), at = base_ + uint32_t(4 * f.index);
            uint32_t &x = const_cast<uint32_t &>(words_[f.index]);
            switch (f.kind) {
            case REL24: x |= (target - at) & 0x03FFFFFC; break;
            case REL14: x |= (target - at) & 0xFFFC; break;
            case HA: x |= ((target + 0x8000) >> 16) & 0xFFFF; break;
            case LO: x |= target & 0xFFFF; break;
            case WORD: x = target; break;
            }
        }
        for (size_t i = 0; i < words_.size(); i++) {
            uint32_t o = base_ - BASE + uint32_t(4 * i), x = words_[i];
            image[o] = uint8_t(x >> 24), image[o + 1] = uint8_t(x >> 16), image[o + 2] = uint8_t(x >> 8),
            image[o + 3] = uint8_t(x);
        }
    }

    // the instructions the program uses
    static uint32_t d(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
        return op << 26 | rt << 21 | ra << 16 | (uint32_t(imm) & 0xFFFF);
    }
    void addi(int rt, int ra, int32_t si) { w(d(14, rt, ra, si)); }
    void li(int rt, int32_t si) { addi(rt, 0, si); }
    void lis(int rt, int32_t si) { w(d(15, rt, 0, si)); }
    void ori(int ra, int rs, uint32_t ui) { w(d(24, rs, ra, int32_t(ui))); }
    void li32(int rt, uint32_t v) { lis(rt, int32_t(v >> 16)), ori(rt, rt, v & 0xFFFF); }
    void lwz(int rt, int32_t disp, int ra) { w(d(32, rt, ra, disp)); }
    void stw(int rs, int32_t disp, int ra) { w(d(36, rs, ra, disp)); }
    void stwu(int rs, int32_t disp, int ra) { w(d(37, rs, ra, disp)); }
    void lfd(int frt, int32_t disp, int ra) { w(d(50, frt, ra, disp)); }
    void stfd(int frs, int32_t disp, int ra) { w(d(54, frs, ra, disp)); }
    void ld(int rt, int32_t ds, int ra) { w(58u << 26 | rt << 21 | ra << 16 | (uint32_t(ds) & 0xFFFC)); }
    void std_(int rs, int32_t ds, int ra) { w(62u << 26 | rs << 21 | ra << 16 | (uint32_t(ds) & 0xFFFC)); }
    void mflr(int rt) { w(0x7C0802A6 | rt << 21); }
    void mtlr(int rs) { w(0x7C0803A6 | rs << 21); }
    void mr(int ra, int rs) { w(31u << 26 | rs << 21 | ra << 16 | rs << 11 | 444 << 1); }
    void add(int rt, int ra, int rb) { w(31u << 26 | rt << 21 | ra << 16 | rb << 11 | 266 << 1); }
    void slwi(int ra, int rs, int n) { w(21u << 26 | rs << 21 | ra << 16 | n << 11 | (31 - n) << 1); }
    void cmpw(int ra, int rb) { w(31u << 26 | ra << 16 | rb << 11); }
    void fsub(int frt, int fra, int frb) { w(63u << 26 | frt << 21 | fra << 16 | frb << 11 | 20 << 1); }
    void blr() { w(0x4E800020); }
    void nop() { w(0x60000000); }
    void b(const std::string &l) { fix(REL24, l, 0x48000000); }
    void bl(const std::string &l) { fix(REL24, l, 0x48000001); }
    void bne(const std::string &l) { fix(REL14, l, 16u << 26 | 4 << 21 | 2 << 16); }
    void lis_ha(int rt, const std::string &l) { fix(HA, l, d(15, rt, 0, 0)); }
    void addi_lo(int rt, int ra, const std::string &l) { fix(LO, l, d(14, rt, ra, 0)); }
    void la(int rt, const std::string &l) { lis_ha(rt, l), addi_lo(rt, rt, l); }
    void word(uint32_t x) { w(x); }
    void word(const std::string &l) { fix(WORD, l, 0); }

  private:
    struct Fix {
        size_t index;
        std::string label;
        Kind kind;
    };
    std::map<std::string, uint32_t> &labels_;
    uint32_t base_;
    std::vector<uint32_t> words_;
    std::vector<Fix> fixes_;
};

inline Program build_program() {
    Program p;
    p.image.assign(SIZE, 0);
    for (const Import &i : IMPORTS) p.labels[i.name] = i.thunk;
    p.labels["LOG"] = LOG, p.labels["NAME"] = NAME, p.labels["FCONST"] = FCONST, p.labels["FSLOT"] = FSLOT;
    Asm a(p.labels, CODE), t(p.labels, TABLES);

    // a function: its handler and handler data first (with the exception flag)
    auto begin = [&](const std::string &name, uint32_t prolog, const std::string &handler = "",
                     const std::string &data = "") {
        Function f;
        if (!handler.empty()) a.word(handler), a.word(data), f.exception = true;
        a.label(name);
        f.begin = a.here(), f.prolog = prolog;
        p.functions.push_back(f);
    };
    auto end = [&] { p.functions.back().end = a.here(); };
    // prologues and epilogues: A saves r30, r31 and LR by hand; B calls the
    // save helper for r29-r31 and LR and keeps a frame pointer in r31; C is a
    // funclet's
    auto prologue_a = [&](int size) {
        a.mflr(12), a.stw(12, -8, 1), a.std_(31, -0x10, 1), a.std_(30, -0x18, 1), a.stwu(1, -size, 1);
    };
    auto epilogue_a = [&](int size) {
        a.addi(1, 1, size), a.lwz(12, -8, 1), a.mtlr(12), a.ld(31, -0x10, 1), a.ld(30, -0x18, 1), a.blr();
    };
    auto prologue_b = [&](int size) { a.mflr(12), a.bl("__savegprlr_29"), a.addi(31, 1, -size), a.stwu(1, -size, 1); };
    auto epilogue_b = [&](int size) { a.addi(1, 31, size), a.b("__restgprlr_29"); };
    auto prologue_c = [&] { a.mflr(12), a.stw(12, -8, 1), a.stwu(1, -0x60, 1); };
    auto epilogue_c = [&] { a.addi(1, 1, 0x60), a.lwz(12, -8, 1), a.mtlr(12), a.blr(); };
    auto log = [&](int32_t value) { a.li(3, value), a.bl("f_log"); };
    auto C = "__C_specific_handler";

    // the save and restore helpers, as the XDK's
    a.label("__savegprlr_14");
    for (int r = 14; r < 32; r++) {
        if (r > 14) a.label("__savegprlr_" + std::to_string(r));
        a.std_(r, -(8 * (32 - r) + 8), 1);
    }
    a.stw(12, -8, 1), a.blr();
    a.label("__restgprlr_14");
    for (int r = 14; r < 32; r++) {
        if (r > 14) a.label("__restgprlr_" + std::to_string(r));
        a.ld(r, -(8 * (32 - r) + 8), 1);
    }
    a.lwz(12, -8, 1), a.mtlr(12), a.blr();
    a.label("__savefpr_14");
    for (int r = 14; r < 32; r++) {
        if (r > 14) a.label("__savefpr_" + std::to_string(r));
        a.stfd(r, -(8 * (32 - r)), 12);
    }
    a.blr();
    a.label("__restfpr_14");
    for (int r = 14; r < 32; r++) {
        if (r > 14) a.label("__restfpr_" + std::to_string(r));
        a.lfd(r, -(8 * (32 - r)), 12);
    }
    a.blr();

    // f_log(r3): appends r3 to the log (touches r9-r11 only)
    begin("f_log", 0);
    a.la(11, "LOG"), a.lwz(10, 0, 11), a.addi(9, 10, 1), a.stw(9, 0, 11), a.slwi(10, 10, 2), a.add(10, 10, 11);
    a.stw(3, 4, 10), a.blr();
    end();

    // f_raise(r3 code, r4 flags): r29-r31 and f31 clobbered (saved by the
    // helpers), the record built on its stack, RtlRaiseException; if that
    // returns, its r31 is logged
    begin("f_raise", 5);
    a.mflr(12), a.bl("__savegprlr_29"), a.addi(12, 1, -0x20), a.bl("__savefpr_31"), a.stwu(1, -0x90, 1);
    a.li(29, 0x0BAD), a.li(30, 0x0BAD), a.li(31, 0x0BAD), a.fsub(31, 31, 31);
    a.stw(3, 0x50, 1), a.stw(4, 0x54, 1), a.li(0, 0), a.stw(0, 0x58, 1), a.stw(0, 0x5C, 1), a.stw(0, 0x60, 1);
    a.addi(3, 1, 0x50), a.bl("RtlRaiseException");
    log(0xC0);
    a.mr(3, 31), a.bl("f_log");
    a.addi(1, 1, 0x90), a.addi(12, 1, -0x20), a.bl("__restfpr_31"), a.b("__restgprlr_29");
    end();

    // filters: the code (EXCEPTION_POINTERS in r3) logged, then 1
    begin("filter_code", 3);
    prologue_c(), a.lwz(3, 0, 3), a.lwz(3, 0, 3), a.bl("f_log"), a.li(3, 1), epilogue_c();
    end();
    // 0x40 logged, then -1 (continue execution)
    begin("filter_m1", 3);
    prologue_c(), log(0x40), a.li(3, -1), epilogue_c();
    end();
    // 0x50 logged; -1 for exception 0xE0000004, else 0 (continue the search)
    begin("filter_m1b", 3);
    prologue_c(), a.lwz(4, 0, 3), a.lwz(5, 0, 4), log(0x50), a.li32(6, 0xE0000004), a.cmpw(5, 6), a.li(3, 0);
    a.bne("filter_m1b_out"), a.li(3, -1);
    a.label("filter_m1b_out");
    epilogue_c();
    end();
    // the code and the code of the record it chains to, then 1
    begin("filter_status", 3);
    prologue_c(), a.lwz(4, 0, 3), a.lwz(3, 0, 4), a.bl("f_log"), a.lwz(3, 8, 4), a.lwz(3, 0, 3), a.bl("f_log");
    a.li(3, 1), epilogue_c();
    end();
    // 0x21, then 1
    begin("filter_n", 3);
    prologue_c(), log(0x21), a.li(3, 1), epilogue_c();
    end();
    // termination handlers
    begin("finally_n", 3);
    prologue_c(), log(0x20), epilogue_c();
    end();
    begin("finally_l", 3);
    prologue_c(), log(0x81), epilogue_c();
    end();
    begin("finally_j", 3);
    prologue_c(), log(0x91), epilogue_c();
    end();
    // a funclet reading its parent's local through the frame in r12
    begin("finally_m", 5);
    a.std_(31, -0x10, 1), a.mr(31, 12), a.mflr(12), a.stw(12, -8, 1), a.stwu(1, -0x60, 1);
    a.lwz(3, -0x20, 31), a.bl("f_log");
    a.addi(1, 1, 0x60), a.lwz(12, -8, 1), a.mtlr(12), a.ld(31, -0x10, 1), a.blr();
    end();

    // S1: __try { f_raise } __except (filter_code) { nonvolatiles checked }
    begin("f_try1", 4, C, "scope_t1");
    prologue_b(0x80);
    a.li(30, 0x1111), a.li(29, 0x2222), a.lis_ha(11, "FCONST"), a.fix(Asm::LO, "FCONST", Asm::d(50, 31, 11, 0));
    log(0x10);
    a.label("t1_begin");
    a.li32(3, 0xE0000001), a.li(4, 0), a.bl("f_raise");
    log(0x11);
    a.label("t1_end");
    a.b("t1_after");
    a.label("t1_except");
    a.mr(3, 30), a.bl("f_log"), a.mr(3, 29), a.bl("f_log");
    a.lis_ha(11, "FSLOT"), a.fix(Asm::LO, "FSLOT", Asm::d(54, 31, 11, 0));
    log(0x12);
    a.label("t1_after");
    epilogue_b(0x80);
    end();
    t.label("scope_t1"), t.word(1), t.word("t1_begin"), t.word("t1_end"), t.word("filter_code"), t.word("t1_except");

    // S2: __try { __try { f_mid } __finally { 0x20 } } __except (filter_n)
    // { 0x22 }; f_mid: __try { f_raise } __finally { finally_m }
    begin("f_mid", 5, C, "scope_m");
    prologue_a(0x70);
    a.li(11, 0x31), a.stw(11, 0x50, 1);
    a.label("m_begin");
    a.li32(3, 0xE0000002), a.li(4, 0), a.bl("f_raise");
    a.label("m_end");
    log(0x31);
    epilogue_a(0x70);
    end();
    t.label("scope_m"), t.word(1), t.word("m_begin"), t.word("m_end"), t.word("finally_m"), t.word(0u);
    begin("f_nested", 4, C, "scope_n");
    prologue_b(0x80);
    a.label("n_outer_begin");
    a.bl("f_mid");
    a.label("n_inner_end");
    log(0x20);
    a.label("n_outer_end");
    a.b("n_after");
    a.label("n_except");
    log(0x22);
    a.label("n_after");
    epilogue_b(0x80);
    end();
    t.label("scope_n"), t.word(2);
    t.word("n_outer_begin"), t.word("n_inner_end"), t.word("finally_n"), t.word(0u);
    t.word("n_outer_begin"), t.word("n_outer_end"), t.word("filter_n"), t.word("n_except");

    // S3: __try { f_raise } __except (filter_m1: continue execution)
    begin("f_continue", 4, C, "scope_c");
    prologue_b(0x80);
    a.label("c_begin");
    a.li32(3, 0xE0000003), a.li(4, 0), a.bl("f_raise");
    a.label("c_end");
    log(0x41);
    a.b("c_after");
    a.label("c_except");
    log(0x99);
    a.label("c_after");
    epilogue_b(0x80);
    end();
    t.label("scope_c"), t.word(1), t.word("c_begin"), t.word("c_end"), t.word("filter_m1"), t.word("c_except");

    // S4: a noncontinuable exception continued: STATUS_NONCONTINUABLE_EXCEPTION
    begin("f_noncont", 4, C, "scope_x");
    prologue_b(0x80);
    a.label("x_begin");
    a.li32(3, 0xE0000004), a.li(4, 1), a.bl("f_raise");
    a.label("x_inner_end");
    a.b("x_after");
    a.label("x_inner_except");
    log(0x99);
    a.label("x_outer_end");
    a.b("x_after");
    a.label("x_outer_except");
    log(0x52);
    a.label("x_after");
    epilogue_b(0x80);
    end();
    t.label("scope_x"), t.word(2);
    t.word("x_begin"), t.word("x_inner_end"), t.word("filter_m1b"), t.word("x_inner_except");
    t.word("x_begin"), t.word("x_outer_end"), t.word("filter_status"), t.word("x_outer_except");

    // S5: __try { DbgBreakPoint() } __except (filter_code) { 0x61 }
    begin("f_break", 4, C, "scope_b");
    prologue_b(0x80);
    a.label("b_begin");
    a.bl("DbgBreakPoint");
    a.label("b_end");
    a.b("b_after");
    a.label("b_except");
    log(0x61);
    a.label("b_after");
    epilogue_b(0x80);
    end();
    t.label("scope_b"), t.word(1), t.word("b_begin"), t.word("b_end"), t.word("filter_code"), t.word("b_except");

    // S5b: __try { __debugbreak() } __except (filter_code) { 0x62 }: the
    // breakpoint is the instruction itself (twi 31, r0, 22), raised at it
    begin("f_trap", 4, C, "scope_tr");
    prologue_b(0x80);
    a.label("tr_begin");
    a.word(0x0FE00016);
    a.label("tr_end");
    a.b("tr_after");
    a.label("tr_except");
    log(0x62);
    a.label("tr_after");
    epilogue_b(0x80);
    end();
    t.label("scope_tr"), t.word(1), t.word("tr_begin"), t.word("tr_end"), t.word("filter_code"), t.word("tr_except");

    // S6: SetThreadName's exception with no handler: on, silently
    begin("f_threadname", 5);
    prologue_a(0x90);
    a.li32(3, 0x406D1388), a.stw(3, 0x50, 1), a.li(0, 0), a.stw(0, 0x54, 1), a.stw(0, 0x58, 1), a.stw(0, 0x5C, 1);
    a.li(0, 4), a.stw(0, 0x60, 1), a.li(0, 0x1000), a.stw(0, 0x64, 1), a.la(11, "NAME"), a.stw(11, 0x68, 1);
    a.li(0, -1), a.stw(0, 0x6C, 1), a.li(0, 0), a.stw(0, 0x70, 1);
    a.addi(3, 1, 0x50), a.bl("RtlRaiseException");
    log(0x70);
    epilogue_a(0x90);
    end();

    // S7: RtlUnwind to its own frame (as _local_unwind): the __finally runs,
    // then on at l_target with r3 the value
    begin("f_local_unwind", 4, C, "scope_l");
    prologue_b(0x80);
    log(0x80);
    a.label("l_begin");
    a.addi(3, 31, 0x80), a.la(4, "l_target"), a.li(5, 0), a.li(6, 0x55), a.bl("RtlUnwind");
    a.label("l_end");
    log(0x99);
    a.label("l_target");
    a.bl("f_log"), log(0x82);
    epilogue_b(0x80);
    end();
    t.label("scope_l"), t.word(1), t.word("l_begin"), t.word("l_end"), t.word("finally_l"), t.word(0u);

    // S8: RtlUnwind two frames up (as a longjmp that unwinds): the inner
    // frame's __finally runs, the outer's r30 comes back
    begin("f_lj_inner", 5, C, "scope_j");
    prologue_a(0x70);
    a.li(30, 0x0BAD);
    a.label("j_begin");
    a.li(5, 0), a.li(6, 0x66), a.bl("RtlUnwind");
    a.label("j_end");
    epilogue_a(0x70);
    end();
    t.label("scope_j"), t.word(1), t.word("j_begin"), t.word("j_end"), t.word("finally_j"), t.word(0u);
    begin("f_lj_outer", 4, C, "scope_none");
    prologue_b(0x80);
    log(0x90);
    a.li(30, 0x3030), a.addi(3, 31, 0x80), a.la(4, "lj_target"), a.bl("f_lj_inner");
    log(0x99);
    a.label("lj_target");
    a.bl("f_log"), a.mr(3, 30), a.bl("f_log"), log(0x92);
    epilogue_b(0x80);
    end();
    t.label("scope_none"), t.word(0u);

    // S9: C++: f_cxx_outer catches (...) what f_cxx_catch's catch rethrows,
    // f_cxx_catch catching what f_cxx_throw throws two frames up; f_cxx_mid's
    // destructor runs on the unwind
    begin("seh_cxx_handler", 3);
    a.mflr(12), a.stw(12, -8, 1), a.stwu(1, -0x100, 1);
    for (int i = 0; i < 12; i++) a.nop();
    a.blr();
    end();
    auto record = [&](uint32_t object, uint32_t type) {
        a.li32(3, 0xE06D7363), a.stw(3, 0x50, 1), a.li(0, 1), a.stw(0, 0x54, 1), a.li(0, 0), a.stw(0, 0x58, 1);
        a.stw(0, 0x5C, 1), a.li(0, 3), a.stw(0, 0x60, 1), a.li32(0, 0x19930520), a.stw(0, 0x64, 1);
        a.li32(0, object), a.stw(0, 0x68, 1), a.li32(0, type), a.stw(0, 0x6C, 1);
        a.addi(3, 1, 0x50), a.bl("RtlRaiseException");
    };
    begin("f_cxx_throw", 5);
    prologue_a(0x90), record(0x1234, TYPE_T), epilogue_a(0x90);
    end();
    begin("f_rethrow", 5);
    prologue_a(0x90), record(0, 0), epilogue_a(0x90);
    end();
    begin("dtor_m", 3);
    prologue_c(), log(0xD1), epilogue_c();
    end();
    begin("f_cxx_mid", 5, "seh_cxx_handler", "funcinfo_m");
    prologue_a(0x70);
    a.label("xm_begin");
    a.bl("f_cxx_throw");
    a.label("xm_end");
    epilogue_a(0x70);
    end();
    begin("f_cxx_catch", 4, "seh_cxx_handler", "funcinfo_c");
    prologue_b(0x80);
    a.li(29, 0x0BAD);
    a.label("xc_begin");
    a.bl("f_cxx_mid");
    a.label("xc_end");
    a.b("xc_after");
    a.label("xc_cont");
    log(0x99);
    a.label("xc_after");
    epilogue_b(0x80);
    end();
    begin("catch_c", 3);
    prologue_c(), log(0xA1), a.bl("f_rethrow"), a.la(3, "xc_cont"), epilogue_c();
    end();
    begin("f_cxx_outer", 4, "seh_cxx_handler", "funcinfo_o");
    prologue_b(0x80);
    a.li(29, 0x4444);
    a.label("xo_begin");
    a.bl("f_cxx_catch");
    a.label("xo_end");
    a.b("xo_after");
    a.label("xo_cont");
    a.mr(3, 29), a.bl("f_log"), log(0xA9);
    a.label("xo_after");
    epilogue_b(0x80);
    end();
    begin("catch_o", 3);
    prologue_c(), log(0xA5), a.la(3, "xo_cont"), epilogue_c();
    end();
    // the test handler's data: try range, the type caught (0: any), the catch
    // funclet, a destructor and the range it covers
    auto funcinfo = [&](const std::string &name, const std::string &tb, const std::string &te, uint32_t type,
                        const std::string &catcher, const std::string &dtor, const std::string &db,
                        const std::string &de) {
        t.label(name);
        for (const std::string *l : {&tb, &te}) l->empty() ? t.word(0u) : t.word(*l);
        t.word(type);
        for (const std::string *l : {&catcher, &dtor, &db, &de}) l->empty() ? t.word(0u) : t.word(*l);
    };
    funcinfo("funcinfo_m", "", "", 0, "", "dtor_m", "xm_begin", "xm_end");
    funcinfo("funcinfo_c", "xc_begin", "xc_end", TYPE_T, "catch_c", "", "", "");
    funcinfo("funcinfo_o", "xo_begin", "xo_end", 0, "catch_o", "", "", "");

    // S10: no handler at all (the test's child process): the title stops
    begin("f_unhandled", 5);
    prologue_a(0x70), a.li32(3, 0xE0000005), a.li(4, 0), a.bl("f_raise"), epilogue_a(0x70);
    end();

    if (a.here() > CODE_END) throw std::runtime_error("the program's code is too long");
    a.resolve(p.image);
    t.resolve(p.image);
    // the data: the name, 1.5, the .pdata
    const char *name = "seh-thread";
    for (size_t i = 0; name[i]; i++) p.image[NAME - BASE + i] = uint8_t(name[i]);
    const uint8_t one_and_half[8] = {0x3F, 0xF8, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; i++) p.image[FCONST - BASE + i] = one_and_half[i];
    uint32_t at = PDATA - BASE;
    for (const Function &f : p.functions) {
        uint32_t data = f.prolog | ((f.end - f.begin) / 4) << 8 | 1u << 30 | (f.exception ? 1u << 31 : 0);
        for (uint32_t x : {f.begin, data})
            p.image[at] = uint8_t(x >> 24), p.image[at + 1] = uint8_t(x >> 16), p.image[at + 2] = uint8_t(x >> 8),
            p.image[at + 3] = uint8_t(x), at += 4;
    }
    p.pdata_count = uint32_t(p.functions.size());
    return p;
}

} // namespace seh
