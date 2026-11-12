/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Structured exception handling for guest code (docs/runtime.md, "Structured
// exceptions"): RtlRaiseException's dispatch, RtlUnwind, __C_specific_handler,
// RtlCaptureContext and DbgBreakPoint, after NT's PowerPC design (the
// handlers' protocol of its RISC ports, read for behavior in Wine's ARM64
// dispatcher, whose design is the same; nothing copied). Xenia logs and
// returns from RtlRaiseException; only SetThreadName's naming and the C++
// throw's log are translated from it (xboxkrnl_debug.cc; BSD, see
// LICENSE-xenia), checked against it.
//
// What the console's code fixes, read from the games' own CRT (facts, not
// code): the CONTEXT (0xA40 bytes: flags, MSR, IAR, LR, CTR, r0-r31 as
// doublewords at 0x18, CR, XER, FPSCR, f0-f31 at 0x128, VSCR at 0x230,
// v0-v127 at 0x240), which the C++ runtime's jump to a catch's continuation
// loads; the DISPATCHER_CONTEXT (ControlPc, FunctionEntry, EstablisherFrame,
// ContextRecord), FunctionEntry the 8-byte .pdata entry and the handler and
// its data the two words before the function; the establisher frame, r1 at
// the function's entry (`_local_unwind(r31 + frame size, target)`); the
// funclets' frame in r12 (`addi r31, r12, -0xA0`); a scope record's range
// holding the call, not its return address; RtlCaptureContext leaving r3
// alone (the CRT passes it on after the call); and the C++ runtime's
// RtlUnwind back into its own frame, its handler swapping in the context it
// captured when the target frame is unwound.

#include "objects.h"
#include "xenoforge/kernel.h"
#include "xenoforge/ppc.h"
#include "xenoforge/ppc_module.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace xf::kernel {
namespace {

// NT's exception flags, codes and dispositions
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 0x01, EXCEPTION_UNWINDING = 0x02, EXCEPTION_EXIT_UNWIND = 0x04,
                   EXCEPTION_STACK_INVALID = 0x08, EXCEPTION_NESTED_CALL = 0x10, EXCEPTION_TARGET_UNWIND = 0x20,
                   EXCEPTION_COLLIDED_UNWIND = 0x40, EXCEPTION_UNWIND = 0x66;
constexpr uint32_t STATUS_BREAKPOINT = 0x80000003, STATUS_NONCONTINUABLE_EXCEPTION = 0xC0000025,
                   STATUS_INVALID_DISPOSITION = 0xC0000026, STATUS_UNWIND = 0xC0000027, STATUS_BAD_STACK = 0xC0000028,
                   STATUS_INVALID_UNWIND_TARGET = 0xC0000029, STATUS_BAD_FUNCTION_TABLE = 0xC00000FF;
constexpr uint32_t SET_THREAD_NAME = 0x406D1388, CXX_EXCEPTION = 0xE06D7363;
constexpr uint32_t KMODE_EXCEPTION_NOT_HANDLED = 0x1E;
enum : uint32_t { ContinueExecution = 0, ContinueSearch = 1, NestedException = 2, CollidedUnwind = 3 };

// EXCEPTION_RECORD: code, flags, the record it chains to, address, the
// parameter count, 15 parameters
namespace record {
constexpr uint32_t CODE = 0, FLAGS = 4, RECORD = 8, ADDRESS = 0xC, COUNT = 0x10, INFO = 0x14, SIZE = 0x50;
}
namespace dispatcher {
constexpr uint32_t CONTROL_PC = 0, FUNCTION_ENTRY = 4, ESTABLISHER_FRAME = 8, CONTEXT_RECORD = 0xC;
}
namespace context {
constexpr uint32_t FLAGS = 0, MSR = 4, IAR = 8, LR = 0xC, CTR = 0x10, GPR = 0x18, CR = 0x118, XER = 0x11C,
                   FPSCR = 0x120, FPR = 0x128, VSCR = 0x230, VR = 0x240, SIZE = 0xA40;
// NT PowerPC's CONTEXT_CONTROL | CONTEXT_FLOATING_POINT | CONTEXT_INTEGER (the
// Xbox 360's value is not known; nothing seen reads it)
constexpr uint32_t FULL = 0x7;
// the title's machine state (docs/recompiler.md: 32-bit, supervisor)
constexpr uint32_t TITLE_MSR = 0x9030;
} // namespace context

// The return address the kernel's calls into a handler (or a funclet a
// handler runs) are made with: a walk that reaches it is in the frames of a
// dispatch or an unwind in progress (Active below), as NT's walks reach the
// frames of RtlpExecuteHandlerForException and its unwind twin. No code is
// there, and the kernel's own return sentinel (0xBCBCBCBC) ends a walk.
constexpr uint32_t HANDLER_RETURN = 0xBCBCBCB0, THREAD_RETURN = 0xBCBCBCBC;
constexpr int MAX_FRAMES = 4096;

// The guest CONTEXT, host-side
struct Regs {
    uint32_t flags = 0, msr = 0, iar = 0, lr = 0;
    uint64_t ctr = 0;
    uint64_t gpr[32] = {};
    uint32_t cr = 0, xer = 0;
    uint64_t fpscr = 0;
    uint64_t fpr[32] = {};
    uint32_t vscr = 0;
    uint32_t vr[128][4] = {};
};

// The registers as they are, IAR the return address (a capture at a call)
Regs capture(const ppc::Context &c) {
    Regs r;
    r.flags = context::FULL, r.msr = context::TITLE_MSR;
    r.iar = r.lr = uint32_t(c.lr);
    r.ctr = c.ctr;
    for (int i = 0; i < 32; i++) r.gpr[i] = c.r[i], r.fpr[i] = std::bit_cast<uint64_t>(c.f[i]);
    r.cr = c.cr_word();
    r.xer = uint32_t(c.xer_so) << 31 | uint32_t(c.xer_ov) << 30 | uint32_t(c.xer_ca) << 29 | (c.xer_bc & 0x7F);
    r.fpscr = c.fpscr;
    r.vscr = (c.vscr_java ? 0u : 0x00010000u) | (c.vscr_sat & 1);
    for (int i = 0; i < 128; i++)
        for (int k = 0; k < 4; k++) r.vr[i][k] = c.v[i].u32[k];
    return r;
}

// NtContinue's part: every register from the context (the MSR stays)
void restore(const Regs &r, ppc::Context &c) {
    c.lr = r.lr;
    c.ctr = r.ctr;
    for (int i = 0; i < 32; i++) c.r[i] = r.gpr[i], c.f[i] = std::bit_cast<double>(r.fpr[i]);
    c.set_cr_word(r.cr);
    c.xer_so = (r.xer >> 31) & 1, c.xer_ov = (r.xer >> 30) & 1, c.xer_ca = (r.xer >> 29) & 1;
    c.xer_bc = r.xer & 0x7F;
    c.fpscr = uint32_t(r.fpscr);
    c.vscr_sat = r.vscr & 1;
    // NJ stays as it is: the contexts the dispatch reads are not all captures
    // (a title's own may leave VSCR 0), and which flags restore VSCR on the
    // console is not known here
    for (int i = 0; i < 128; i++)
        for (int k = 0; k < 4; k++) c.v[i].u32[k] = r.vr[i][k];
}

void write_context(Kernel &k, uint32_t a, const Regs &r) {
    k.store32(a + context::FLAGS, r.flags);
    k.store32(a + context::MSR, r.msr);
    k.store32(a + context::IAR, r.iar);
    k.store32(a + context::LR, r.lr);
    k.store64(a + context::CTR, r.ctr);
    for (int i = 0; i < 32; i++) k.store64(a + context::GPR + 8 * i, r.gpr[i]);
    k.store32(a + context::CR, r.cr);
    k.store32(a + context::XER, r.xer);
    k.store64(a + context::FPSCR, r.fpscr);
    for (int i = 0; i < 32; i++) k.store64(a + context::FPR + 8 * i, r.fpr[i]);
    k.store32(a + 0x228, 0); // UserModeControl
    k.store32(a + 0x22C, 0);
    for (int i = 0; i < 3; i++) k.store32(a + context::VSCR + 4 * i, 0);
    k.store32(a + context::VSCR + 12, r.vscr); // as mfvscr leaves it: the last word
    for (int i = 0; i < 128; i++)
        for (int w = 0; w < 4; w++) k.store32(a + context::VR + 16 * i + 4 * w, r.vr[i][w]);
}

Regs read_context(Kernel &k, uint32_t a) {
    Regs r;
    r.flags = k.load32(a + context::FLAGS);
    r.msr = k.load32(a + context::MSR);
    r.iar = k.load32(a + context::IAR);
    r.lr = k.load32(a + context::LR);
    r.ctr = k.load64(a + context::CTR);
    for (int i = 0; i < 32; i++) r.gpr[i] = k.load64(a + context::GPR + 8 * i);
    r.cr = k.load32(a + context::CR);
    r.xer = k.load32(a + context::XER);
    r.fpscr = k.load64(a + context::FPSCR);
    for (int i = 0; i < 32; i++) r.fpr[i] = k.load64(a + context::FPR + 8 * i);
    r.vscr = k.load32(a + context::VSCR + 12);
    for (int i = 0; i < 128; i++)
        for (int w = 0; w < 4; w++) r.vr[i][w] = k.load32(a + context::VR + 16 * i + 4 * w);
    return r;
}

// ---- the function tables ----

struct Tables {
    struct Table {
        uint32_t begin, end, entries, count;
    };
    std::mutex mutex;
    std::vector<Table> tables;
};

// A function as .pdata gives it: the entry's own address (what
// RtlLookupFunctionEntry returns and DISPATCHER_CONTEXT holds), its range and
// prologue, and with the exception flag its handler and the handler's data
// (the two words before the function)
struct FunctionEntry {
    uint32_t address = 0, begin = 0, end = 0, prolog = 0;
    uint32_t handler = 0, data = 0;
};

uint32_t le32(Kernel &k, uint32_t a) {
    const uint8_t *p = k.host(a);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// the entry of a sorted table of count 8-byte entries at `entries` holding pc
bool search_table(Kernel &k, uint32_t entries, uint32_t count, uint32_t pc, FunctionEntry &out) {
    uint32_t low = 0, high = count;
    while (low < high) { // the first entry beginning past pc
        uint32_t mid = low + (high - low) / 2;
        if (k.load32(entries + 8 * mid) <= pc)
            low = mid + 1;
        else
            high = mid;
    }
    if (!low) return false;
    uint32_t at = entries + 8 * (low - 1), begin = k.load32(at), data = k.load32(at + 4);
    uint32_t instructions = (data >> 8) & 0x3FFFFF;
    uint32_t end = begin + 4 * (instructions ? instructions : 1);
    if (!begin || pc >= end) return false;
    out = FunctionEntry{at, begin, end, data & 0xFF, 0, 0};
    // the exception flag without the 32-bit one marks the register save and
    // restore helpers' entries (every game has three, a blr where the data
    // would be), not a handler
    if ((data >> 31) && (data & 0x40000000)) out.handler = k.load32(begin - 8), out.data = k.load32(begin - 4);
    return true;
}

// The function holding pc: the tables added first, then the exception
// directory of the image holding it (data directory 3, in the PE headers the
// image keeps at its base, little-endian; the entries big-endian)
bool lookup_function(Kernel &k, uint32_t pc, FunctionEntry &out) {
    {
        Tables &t = k.state<Tables>();
        std::lock_guard lock(t.mutex);
        for (const Tables::Table &table : t.tables)
            if (pc >= table.begin && pc < table.end) return search_table(k, table.entries, table.count, pc, out);
    }
    uint32_t base = 0, size = 0;
    if (!module_range(k, pc, base, size) || size < 0x200) return false;
    if (k.host(base)[0] != 'M' || k.host(base)[1] != 'Z') return false;
    uint32_t pe = le32(k, base + 0x3C);
    if (pe > size - 0x100 || le32(k, base + pe) != 0x00004550) return false;
    uint32_t optional = base + pe + 24;
    if (le32(k, optional + 92) <= 3) return false;
    uint32_t rva = le32(k, optional + 96 + 8 * 3), bytes = le32(k, optional + 96 + 8 * 3 + 4);
    if (!rva || rva >= size || bytes > size - rva) return false;
    return search_table(k, base + rva, bytes / 8, pc, out);
}

// ---- the virtual unwind ----
//
// The prologue run forward over symbols (NT's unwinder runs it backward;
// forward gives the same with the save helpers' bases known): what each
// register holds relative to the function's entry, where each value from the
// entry was stored. The XDK's forms: mflr r12 then stw r12, -8(r1) or a call
// to __savegprlr_N (std rN..r31 below the entry r1, LR at -8); mflr into a
// nonvolatile register kept there (`std r31, -0x48(r1); mflr r31`); std/stw of
// nonvolatiles; addi r12, r1, -X then __savefpr_N (stfd off r12) and
// __savevmx_N (li r11, -X; stvx off r11 and r12); mfcr and its store; stwu
// r1 (stwux with a size in a register); a frame pointer set before or after
// it (`addi r31, r1, -0xA0; stwu r1, -0xA0(r1)`).

struct Value {
    enum Kind : uint8_t { Unknown, Entry, Stack, Const, Lr, Cr } kind = Unknown;
    int64_t n = 0; // Entry: the register; Stack: the offset from the entry's r1; Const: the value
};

struct Save {
    enum Kind : uint8_t { Gpr, Gpr32, Fpr, Vr, Lr, Cr } kind;
    uint32_t reg;
    int64_t offset; // from the entry's r1
};

struct Prologue {
    Value g[32];
    std::vector<Save> saves;
    int64_t sp = 0;          // r1 minus the entry's r1
    bool sp_unknown = false; // a stwux by a size not known: the back chain has the entry's r1
};

int64_t simm16(uint32_t v) { return int16_t(uint16_t(v)); }

void store_value(Prologue &p, const Value &v, int64_t offset, bool doubleword) {
    if (v.kind == Value::Entry)
        p.saves.push_back({doubleword ? Save::Gpr : Save::Gpr32, uint32_t(v.n), offset});
    else if (v.kind == Value::Lr)
        p.saves.push_back({Save::Lr, 0, offset});
    else if (v.kind == Value::Cr)
        p.saves.push_back({Save::Cr, 0, offset});
}

// an address made of two registers (indexed forms), when one is a stack
// offset and the other a constant (or r0 as the base: 0)
bool indexed(const Prologue &p, uint32_t ra, uint32_t rb, int64_t &offset) {
    Value a = ra ? p.g[ra] : Value{Value::Const, 0}, b = p.g[rb];
    if (a.kind == Value::Stack && b.kind == Value::Const) return offset = a.n + b.n, true;
    if (b.kind == Value::Stack && a.kind == Value::Const) return offset = a.n + b.n, true;
    return false;
}

void simulate(Kernel &k, uint32_t from, uint32_t to, Prologue &p, int depth) {
    using ppc::Opcode;
    for (uint32_t a = from; a < to; a += 4) {
        ppc::Instr i{k.load32(a)};
        Opcode op = ppc::decode(i.code);
        Value base = i.D.RA ? p.g[i.D.RA] : Value{Value::Const, 0};
        switch (op) {
        case Opcode::stwu:
        case Opcode::stdu: {
            int64_t d = op == Opcode::stwu ? simm16(i.D.DS) : simm16(i.D.DS & ~3u);
            if (i.D.RA == 1 && i.D.RT == 1 && !p.sp_unknown) p.sp += d, p.g[1] = {Value::Stack, p.sp};
            break;
        }
        case Opcode::stwux:
        case Opcode::stdux:
            if (i.X.RA == 1 && i.X.RT == 1) {
                if (p.g[i.X.RB].kind == Value::Const && !p.sp_unknown)
                    p.sp += p.g[i.X.RB].n, p.g[1] = {Value::Stack, p.sp};
                else
                    p.sp_unknown = true, p.g[1] = {};
            }
            break;
        case Opcode::stw:
        case Opcode::std:
            if (base.kind == Value::Stack)
                store_value(p, p.g[i.D.RT], base.n + (op == Opcode::std ? simm16(i.D.DS & ~3u) : simm16(i.D.DS)),
                            op == Opcode::std);
            break;
        case Opcode::stwx:
        case Opcode::stdx: {
            int64_t offset;
            if (indexed(p, i.X.RA, i.X.RB, offset)) store_value(p, p.g[i.X.RT], offset, op == Opcode::stdx);
            break;
        }
        case Opcode::stfd:
            if (base.kind == Value::Stack) p.saves.push_back({Save::Fpr, i.D.RT, base.n + simm16(i.D.DS)});
            break;
        case Opcode::stvx:
        case Opcode::stvxl: {
            int64_t offset;
            if (indexed(p, i.X.RA, i.X.RB, offset)) p.saves.push_back({Save::Vr, i.X.RT, offset});
            break;
        }
        case Opcode::stvx128:
        case Opcode::stvxl128: {
            int64_t offset;
            uint32_t v = i.VX128_1.VD128l | (i.VX128_1.VD128h << 5);
            if (indexed(p, i.VX128_1.RA, i.VX128_1.RB, offset)) p.saves.push_back({Save::Vr, v, offset});
            break;
        }
        case Opcode::mfspr: {
            uint32_t spr = ((i.XFX.spr & 0x1F) << 5) | (i.XFX.spr >> 5);
            p.g[i.XFX.RT] = spr == 8 ? Value{Value::Lr, 0} : Value{};
            break;
        }
        case Opcode::mfcr: p.g[i.X.RT] = {Value::Cr, 0}; break;
        case Opcode::addi:
        case Opcode::addis: {
            int64_t si = op == Opcode::addi ? simm16(i.D.DS) : simm16(i.D.DS) * 65536;
            p.g[i.D.RT] =
                base.kind == Value::Stack || base.kind == Value::Const ? Value{base.kind, base.n + si} : Value{};
            break;
        }
        case Opcode::ori: { // ori rA, rS, UI: the D form's RT is rS
            Value s = p.g[i.D.RT];
            p.g[i.D.RA] = s.kind == Value::Const ? Value{Value::Const, s.n | int64_t(i.D.DS)} : Value{};
            break;
        }
        case Opcode::orx: // mr rA, rS
            p.g[i.X.RA] = i.X.RT == i.X.RB ? p.g[i.X.RT] : Value{};
            break;
        case Opcode::negx:
            p.g[i.XO.RT] = p.g[i.XO.RA].kind == Value::Const ? Value{Value::Const, -p.g[i.XO.RA].n} : Value{};
            break;
        case Opcode::bx:
            // a save helper: its stores are the prologue's (its blr ends it);
            // in a helper, a branch ends the helper
            if (depth) return;
            if (i.I.LK && !i.I.AA) {
                uint32_t target = a + uint32_t(int32_t(i.I.LI << 8) >> 6);
                simulate(k, target, target + 4 * 64, p, depth + 1);
            }
            break;
        case Opcode::bcx:
        case Opcode::bclrx:
        case Opcode::bcctrx:
            if (depth) return;
            break;
        default: {
            // anything else the prologue holds (scheduled body code): what it
            // may write is no longer known; stores write no register
            const char *name = ppc::info(op).name;
            if (name[0] == 's' && name[1] == 't') break;
            if (i.D.RT != 1) p.g[i.D.RT] = {};
            if (i.D.RA != 1) p.g[i.D.RA] = {};
            break;
        }
        }
    }
}

// The frame of the function fe at pc unwound: r from its body's registers to
// its caller's (r1 its entry's, LR and IAR where it returns, the registers its
// prologue saved), and its establisher frame (r1 at its entry). in_body: pc
// past its prologue (its handler runs only then, as NT's InFunction).
bool unwind_frame(Kernel &k, const FunctionEntry &fe, uint32_t pc, Regs &r, uint32_t &establisher, bool &in_body) {
    uint32_t prologue_end = std::min(fe.begin + 4 * fe.prolog, fe.end);
    in_body = pc >= prologue_end;
    Prologue p;
    for (int i = 0; i < 32; i++) p.g[i] = {Value::Entry, i};
    p.g[1] = {Value::Stack, 0};
    simulate(k, fe.begin, std::min(pc, prologue_end), p, 0);

    // the entry's r1: by a frame pointer (a nonvolatile holding an offset
    // from it: what alloca leaves alone), else by r1 itself
    uint64_t entry;
    int frame_register = -1;
    for (int g = 31; g >= 14 && in_body; g--)
        if (p.g[g].kind == Value::Stack) {
            frame_register = g;
            break;
        }
    if (frame_register >= 0)
        entry = r.gpr[frame_register] - uint64_t(p.g[frame_register].n);
    else if (p.sp_unknown)
        entry = k.load32(uint32_t(r.gpr[1]));
    else
        entry = r.gpr[1] - uint64_t(p.sp);
    entry &= 0xFFFFFFFF;
    if (!entry) return false;

    // LR first: a register still holding it is restored below
    uint32_t lr = r.lr;
    bool lr_saved = false;
    for (const Save &s : p.saves)
        if (s.kind == Save::Lr) lr = k.load32(uint32_t(entry + s.offset)), lr_saved = true;
    if (!lr_saved)
        for (int g = 31; g >= 0; g--)
            if (p.g[g].kind == Value::Lr) {
                lr = uint32_t(r.gpr[g]);
                break;
            }
    for (const Save &s : p.saves) {
        uint32_t a = uint32_t(entry + s.offset);
        switch (s.kind) {
        case Save::Gpr: r.gpr[s.reg] = k.load64(a); break;
        case Save::Gpr32: r.gpr[s.reg] = k.load32(a); break;
        case Save::Fpr: r.fpr[s.reg] = k.load64(a); break;
        case Save::Vr:
            for (int w = 0; w < 4; w++) r.vr[s.reg][w] = k.load32(a + 4 * w);
            break;
        case Save::Cr: r.cr = (r.cr & ~0x00FFF000u) | (k.load32(a) & 0x00FFF000u); break; // CR2-CR4
        case Save::Lr: break;
        }
    }
    r.gpr[1] = entry;
    r.lr = r.iar = lr;
    establisher = uint32_t(entry);
    return true;
}

// ---- walks in progress ----
//
// Each dispatch and unwind in progress on a thread, innermost first: a walk
// that reaches HANDLER_RETURN goes on from the code that started the walk
// whose handler (or funclet) it is in (NT's walks through the frames of
// RtlpExecuteHandlerForException, RtlDispatchException and RtlRaiseException,
// or RtlUnwind's own). They live on the host stack; the context tells the
// guest threads apart where fibers share a host thread.

struct Active {
    const ppc::Context *c = nullptr;
    uint32_t frame = 0; // the guest r1 its handlers are called on
    bool unwind = false;
    Regs original; // the walk's start: the raise's caller, RtlUnwind's caller
    uint32_t first_pc = 0;
    // the frame whose handler runs now
    Regs current;
    uint32_t current_pc = 0, establisher = 0, dc = 0;
    uint32_t target_ip = 0; // an unwind's
    Active *prev = nullptr;
};

thread_local Active *active_walks = nullptr;

} // namespace

void *exchange_exception_walks(void *walks) { return std::exchange(active_walks, static_cast<Active *>(walks)); }

namespace {

struct ActiveScope {
    Active &a;
    explicit ActiveScope(Active &a) : a(a) { a.prev = active_walks, active_walks = &a; }
    ~ActiveScope() { active_walks = a.prev; }
};

// the innermost walk of this context whose handlers run at or above sp
Active *walk_at(const ppc::Context &c, uint32_t sp) {
    for (Active *a = active_walks; a; a = a->prev)
        if (a->c == &c && a->frame >= sp) return a;
    return nullptr;
}

// Guest code run from the kernel on a frame of its own at sp, returning to
// HANDLER_RETURN: r3 and up the arguments, r12 the frame for a funclet. The
// registers around it are put back; a return to a frame further up (a catch's
// continuation, an unwind's target) goes on through as a ppc::Unwind.
uint32_t call_guest(Kernel &k, ppc::Context &c, uint32_t fn, uint32_t sp, std::initializer_list<uint32_t> args,
                    uint32_t r12 = 0) {
    ppc::Context saved = c;
    k.store32(sp, uint32_t(c.r[1])); // its back chain
    c.r[1] = sp;
    int n = 3;
    for (uint32_t a : args) c.r[n++] = a;
    if (r12) c.r[12] = r12;
    c.lr = HANDLER_RETURN;
    k.dispatch(c, fn);
    uint32_t result = uint32_t(c.r[3]);
    c = saved;
    return result;
}

// the thread's stack (the PCR's bounds; none known: anything goes)
bool in_stack(Kernel &k, const ppc::Context &c, uint32_t sp) {
    if ((sp & 3) || !sp) return false;
    uint32_t pcr = uint32_t(c.r[13]);
    if (!pcr) return true;
    uint32_t base = k.load32(pcr + kpcr::STACK_BASE), limit = k.load32(pcr + kpcr::STACK_END);
    if (!base || base <= limit) return true;
    return sp >= limit && sp <= base;
}

// Where execution goes on with a context (NtContinue): back from the call
// that started it when that is where the context points (the C++ runtime's
// unwind back into its own frame, a continued exception), else a return to
// another frame, thrown to the frame that holds it (ppc::Unwind; a generated
// function's resume site, else a dispatch that interprets from there).
void continue_at(Kernel &k, ppc::Context &c, const Regs &r, uint32_t caller_sp, uint32_t caller_return) {
    restore(r, c);
    if (uint32_t(r.gpr[1]) == caller_sp && r.iar == caller_return) return;
    k.unwinds_thrown++;
    throw ppc::Unwind{r.iar & ~3u};
}

std::string describe(Kernel &k, uint32_t rec) {
    char line[160];
    uint32_t code = k.load32(rec + record::CODE), count = std::min(k.load32(rec + record::COUNT), 15u);
    std::snprintf(line, sizeof(line), "0x%08X (flags 0x%X) at 0x%08X", code, k.load32(rec + record::FLAGS),
                  k.load32(rec + record::ADDRESS));
    std::string out = line;
    for (uint32_t i = 0; i < count && i < 4; i++) {
        std::snprintf(line, sizeof(line), "%s0x%08X", i ? ", " : ", parameters ", k.load32(rec + record::INFO + 4 * i));
        out += line;
    }
    return out;
}

// No handler took it: the frames walked, then KeBugCheckEx as NT's kernel
// does (titles run in supervisor state: an exception no frame handles is
// KMODE_EXCEPTION_NOT_HANDLED, not a process's end)
[[noreturn]] void unhandled(Kernel &k, ppc::Context &c, uint32_t rec, const Regs &start, uint32_t first_pc,
                            const char *why) {
    std::fprintf(stderr, "kernel: unhandled exception %s (%s); the frames:\n", describe(k, rec).c_str(), why);
    Regs r = start;
    uint32_t pc = first_pc;
    for (int n = 0; n < 32; n++) {
        FunctionEntry fe;
        uint32_t establisher = 0;
        bool in_body = false;
        bool found = lookup_function(k, pc, fe);
        std::fprintf(stderr, "kernel:   0x%08X  r1 0x%08X  %s%s\n", pc, uint32_t(r.gpr[1]),
                     found ? "" : "(no function entry)", found && fe.handler ? "(has a handler)" : "");
        if (!found || !unwind_frame(k, fe, pc, r, establisher, in_body)) break;
        if (r.lr == THREAD_RETURN || r.lr == HANDLER_RETURN || !r.lr) break;
        pc = r.lr - 4;
    }
    (void)c;
    bug_check(k, KMODE_EXCEPTION_NOT_HANDLED, k.load32(rec + record::CODE), k.load32(rec + record::ADDRESS),
              k.load32(rec + record::INFO), k.load32(rec + record::INFO + 4));
}

// ---- the dispatch (RtlDispatchException) ----

// (Stopped: the kernel stopped while a handler ran, so its answer is none)
enum class Searched { Continue, Unhandled, Noncontinuable, InvalidDisposition, Stopped };

// The frames from the raise up, each with a handler asked (EXCEPTION_RECORD at
// rec, the raise's context at ctx, written there): the first that continues
// ends it. A walk reaching the frames of a dispatch in progress (this one
// raised by a handler) goes on from the code that raised that one, marking the
// frames up to the frame whose handler was running EXCEPTION_NESTED_CALL.
Searched search(Kernel &k, ppc::Context &c, Active &a, uint32_t rec, uint32_t ctx, uint32_t dc) {
    uint32_t flags = k.load32(rec + record::FLAGS) & EXCEPTION_NONCONTINUABLE;
    uint32_t nested = 0;
    Regs r = a.original;
    uint32_t pc = a.first_pc;
    for (int n = 0; n < MAX_FRAMES; n++) {
        if (pc == HANDLER_RETURN - 4) {
            Active *outer = walk_at(c, uint32_t(r.gpr[1]));
            if (!outer) return Searched::Unhandled;
            if (!outer->unwind) {
                // raised by a handler of a dispatch in progress
                flags |= EXCEPTION_NESTED_CALL;
                nested = std::max(nested, outer->establisher);
            }
            r = outer->original, pc = outer->first_pc;
            continue;
        }
        FunctionEntry fe;
        if (!lookup_function(k, pc, fe)) {
            std::fprintf(stderr, "kernel: the exception dispatch found no function entry at 0x%08X\n", pc);
            return Searched::Unhandled;
        }
        Regs body = r;
        uint32_t establisher;
        bool in_body;
        if (!unwind_frame(k, fe, pc, r, establisher, in_body) || !in_stack(k, c, establisher)) {
            k.store32(rec + record::FLAGS, flags | EXCEPTION_STACK_INVALID);
            return Searched::Unhandled;
        }
        if (fe.handler && in_body) {
            k.store32(dc + dispatcher::CONTROL_PC, pc);
            k.store32(dc + dispatcher::FUNCTION_ENTRY, fe.address);
            k.store32(dc + dispatcher::ESTABLISHER_FRAME, establisher);
            k.store32(dc + dispatcher::CONTEXT_RECORD, ctx);
            k.store32(rec + record::FLAGS, flags);
            a.current = body, a.current_pc = pc, a.establisher = establisher, a.dc = dc;
            uint32_t disposition = call_guest(k, c, fe.handler, a.frame, {rec, establisher, ctx, dc});
            if (k.stopped) return Searched::Stopped;
            flags |= k.load32(rec + record::FLAGS) & EXCEPTION_NONCONTINUABLE;
            k.store32(rec + record::FLAGS, flags);
            switch (disposition) {
            case ContinueExecution:
                return flags & EXCEPTION_NONCONTINUABLE ? Searched::Noncontinuable : Searched::Continue;
            case ContinueSearch: break;
            case NestedException:
                flags |= EXCEPTION_NESTED_CALL;
                nested = std::max(nested, k.load32(dc + dispatcher::ESTABLISHER_FRAME));
                break;
            default: return Searched::InvalidDisposition;
            }
        }
        if (nested == establisher) flags &= ~EXCEPTION_NESTED_CALL, nested = 0;
        if (!r.lr || r.lr == THREAD_RETURN) return Searched::Unhandled;
        pc = r.lr - 4;
    }
    return Searched::Unhandled;
}

// The guest frame a dispatch or an unwind works in, below the caller's r1 (the
// stack below it is free): its CONTEXT, DISPATCHER_CONTEXT, records it makes,
// and the frame its handlers are called on.
struct Scratch {
    uint32_t ctx, dc, records, frame;
    explicit Scratch(uint32_t top) {
        ctx = ((top - 0x100) & ~15u) - context::SIZE;
        dc = ctx - 0x20;
        records = dc - 4 * 0x60; // four records' room
        frame = (records - 0x100) & ~15u;
    }
};

// SetThreadName's exception (Xenia's HandleSetThreadName): what a debugger
// does with it on its first chance; kept for the thread report
void name_thread(Kernel &k, uint32_t rec) {
    // X_THREADNAME_INFO in the parameters: type 0x1000, name, thread id, flags
    uint32_t name = k.load32(rec + record::INFO + 4), thread_id = k.load32(rec + record::INFO + 8);
    std::string text;
    if (name)
        for (uint32_t p = name; k.host(p)[0] && text.size() < 256; p++) {
            char ch = char(k.host(p)[0]);
            text += (ch < 32 || ch > 126) ? '?' : ch;
        }
    std::fprintf(stderr, "kernel: SetThreadName(%d, %s)\n", int32_t(thread_id), text.c_str());
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    for (Thread *t : o.threads)
        if (thread_id == 0xFFFFFFFF ? t == current_thread() : t->id == thread_id) t->guest_name = text;
}

// A C++ throw (Xenia's HandleCppException logs it): the thrown type's name,
// from the ThrowInfo (its CatchableTypeArray's first type's TypeDescriptor),
// each pointer followed only into a loaded image (where the compiler puts
// them), so a bad one costs the name, not the host
void log_throw(Kernel &k, uint32_t rec) {
    static std::atomic<int> logged{0};
    if (logged++ >= 32) return;
    auto in_image = [&](uint32_t a, uint32_t bytes) {
        uint32_t base = 0, size = 0;
        return a && module_range(k, a, base, size) && a - base <= size - bytes;
    };
    std::string name = "?";
    if (k.load32(rec + record::COUNT) >= 3 && k.load32(rec + record::INFO) == 0x19930520) {
        uint32_t info = k.load32(rec + record::INFO + 8);
        if (!info)
            name = "a rethrow";
        else if (in_image(info, 0x10))
            if (uint32_t types = k.load32(info + 0xC); in_image(types, 8) && k.load32(types) > 0)
                if (uint32_t type = k.load32(types + 4); in_image(type, 8))
                    if (uint32_t descriptor = k.load32(type + 4); in_image(descriptor, 0x10)) {
                        name.clear();
                        for (uint32_t p = descriptor + 8; in_image(p, 1) && k.host(p)[0] && name.size() < 128; p++)
                            name += char(k.host(p)[0]);
                    }
    }
    std::fprintf(stderr, "kernel: a C++ exception (%s) thrown from 0x%08X\n", name.c_str(),
                 k.load32(rec + record::ADDRESS));
}

// RtlRaiseException's work: the exception at rec raised by the call that
// returns to c.lr, the frames below top free; returns when a handler continues
// it at its own return address, else goes on elsewhere (ppc::Unwind) or ends
// the title. With `at`, an instruction raised it (a trap): the exception's
// address and the context's IAR are its own, and a handler continues the code
// after it.
void raise(Kernel &k, ppc::Context &c, uint32_t rec, uint32_t top, uint32_t at = 0) {
    const uint32_t caller_sp = uint32_t(c.r[1]), caller_return = at ? at + 4 : uint32_t(c.lr);
    Regs start = capture(c);
    if (at) start.iar = at;
    k.store32(rec + record::ADDRESS, at ? at : caller_return);
    uint32_t code = k.load32(rec + record::CODE);
    static std::atomic<int> logged{0};
    if (code == SET_THREAD_NAME)
        name_thread(k, rec);
    else if (code == CXX_EXCEPTION)
        log_throw(k, rec);
    else if (logged++ < 32)
        std::fprintf(stderr, "kernel: exception %s raised\n", describe(k, rec).c_str());

    Scratch s(top);
    Active a;
    a.c = &c, a.frame = s.frame, a.original = start, a.first_pc = at ? at : caller_return - 4;
    ActiveScope scope(a);
    for (int attempt = 0;; attempt++) {
        write_context(k, s.ctx, start);
        Searched result = search(k, c, a, rec, s.ctx, s.dc);
        if (result == Searched::Stopped) return;
        if (result == Searched::Continue) {
            // the context as the handler left it
            continue_at(k, c, read_context(k, s.ctx), caller_sp, caller_return);
            return;
        }
        if (result == Searched::Unhandled) {
            // a debugger's notification, with none attached: on, silently
            if (k.load32(rec + record::CODE) == SET_THREAD_NAME) return;
            unhandled(k, c, rec, start, a.first_pc, "no handler took it");
        }
        // a handler's answer the dispatch cannot follow: a new exception
        // raised over it (RtlRaiseStatus), the first chained to it
        if (attempt == 3) unhandled(k, c, rec, start, a.first_pc, "the handlers' answers raised too many");
        uint32_t next = s.records + record::SIZE * attempt;
        k.store32(next + record::CODE,
                  result == Searched::Noncontinuable ? STATUS_NONCONTINUABLE_EXCEPTION : STATUS_INVALID_DISPOSITION);
        k.store32(next + record::FLAGS, EXCEPTION_NONCONTINUABLE);
        k.store32(next + record::RECORD, rec);
        k.store32(next + record::ADDRESS, caller_return);
        k.store32(next + record::COUNT, 0);
        rec = next;
    }
}

// ---- the unwind (RtlUnwind) ----

[[noreturn]] void bad_unwind(Kernel &k, ppc::Context &c, uint32_t rec, uint32_t status, const Regs &start,
                             uint32_t first_pc) {
    // NT raises the status; nothing above an unwind gone wrong can take it
    k.store32(rec + record::CODE, status);
    unhandled(k, c, rec, start, first_pc, "the unwind failed");
}

// The frames from RtlUnwind's caller up to target_frame (0: all of them, an
// exit unwind), each handler called with EXCEPTION_UNWINDING, then execution
// on at target_ip in the target frame with r3 value. A walk reaching the
// frames of an exception dispatch in progress goes on from the code that
// raised it; one reaching another unwind's (a handler of it unwinding) takes
// over that one's frame again, EXCEPTION_COLLIDED_UNWIND.
void unwind(Kernel &k, ppc::Context &c, uint32_t target_frame, uint32_t target_ip, uint32_t rec, uint32_t value) {
    const uint32_t caller_sp = uint32_t(c.r[1]), caller_return = uint32_t(c.lr);
    Regs start = capture(c);
    Scratch s(caller_sp);
    if (!rec) {
        rec = s.records;
        k.store32(rec + record::CODE, STATUS_UNWIND);
        k.store32(rec + record::FLAGS, 0);
        k.store32(rec + record::RECORD, 0);
        k.store32(rec + record::ADDRESS, caller_return);
        k.store32(rec + record::COUNT, 0);
    }
    uint32_t flags = k.load32(rec + record::FLAGS) | EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND);
    Active a;
    a.c = &c, a.frame = s.frame, a.unwind = true, a.original = start, a.first_pc = caller_return - 4;
    a.target_ip = target_ip;
    ActiveScope scope(a);

    Regs r = start, final_regs;
    uint32_t pc = a.first_pc;
    for (int n = 0;; n++) {
        if (n == MAX_FRAMES) bad_unwind(k, c, rec, STATUS_BAD_STACK, start, a.first_pc);
        if (pc == HANDLER_RETURN - 4) {
            Active *outer = walk_at(c, uint32_t(r.gpr[1]));
            if (!outer) bad_unwind(k, c, rec, STATUS_BAD_STACK, start, a.first_pc);
            if (outer->unwind) {
                // the frame whose handler started this one, again, from where
                // its handler left its DISPATCHER_CONTEXT's ControlPc
                flags |= EXCEPTION_COLLIDED_UNWIND;
                r = outer->current, pc = k.load32(outer->dc + dispatcher::CONTROL_PC);
            } else {
                r = outer->original, pc = outer->first_pc;
            }
            continue;
        }
        FunctionEntry fe;
        if (!lookup_function(k, pc, fe)) {
            std::fprintf(stderr, "kernel: the unwind found no function entry at 0x%08X\n", pc);
            bad_unwind(k, c, rec, STATUS_BAD_FUNCTION_TABLE, start, a.first_pc);
        }
        Regs body = r;
        uint32_t establisher;
        bool in_body;
        if (!unwind_frame(k, fe, pc, r, establisher, in_body) || !in_stack(k, c, establisher)) {
            k.store32(rec + record::FLAGS, flags | EXCEPTION_STACK_INVALID);
            bad_unwind(k, c, rec, STATUS_BAD_STACK, start, a.first_pc);
        }
        if (target_frame && establisher > target_frame)
            bad_unwind(k, c, rec, STATUS_INVALID_UNWIND_TARGET, start, a.first_pc);
        if (establisher == target_frame) flags |= EXCEPTION_TARGET_UNWIND;
        if (fe.handler && in_body) {
            write_context(k, s.ctx, body);
            k.store32(s.dc + dispatcher::CONTROL_PC, pc);
            k.store32(s.dc + dispatcher::FUNCTION_ENTRY, fe.address);
            k.store32(s.dc + dispatcher::ESTABLISHER_FRAME, establisher);
            k.store32(s.dc + dispatcher::CONTEXT_RECORD, s.ctx);
            k.store32(rec + record::FLAGS, flags);
            a.current = body, a.current_pc = pc, a.establisher = establisher, a.dc = s.dc;
            uint32_t disposition = call_guest(k, c, fe.handler, a.frame, {rec, establisher, s.ctx, s.dc});
            if (k.stopped) return; // (its answer is none)
            if (disposition != ContinueSearch) bad_unwind(k, c, rec, STATUS_INVALID_DISPOSITION, start, a.first_pc);
            flags &= ~EXCEPTION_COLLIDED_UNWIND;
            // the target frame's handler may change the context execution
            // goes on with (the C++ runtime's, back into its own frame)
            if (establisher == target_frame) body = read_context(k, s.ctx);
        }
        if (establisher == target_frame) {
            final_regs = body;
            break;
        }
        if (!r.lr || r.lr == THREAD_RETURN)
            bad_unwind(k, c, rec, target_frame ? STATUS_INVALID_UNWIND_TARGET : STATUS_UNWIND, start, a.first_pc);
        pc = r.lr - 4;
    }
    k.store32(rec + record::FLAGS, flags & ~EXCEPTION_TARGET_UNWIND);
    final_regs.iar = target_ip;
    final_regs.gpr[3] = value;
    continue_at(k, c, final_regs, caller_sp, caller_return);
}

// ---- the exports ----

XF_EXPORT(xboxkrnl, RtlRaiseException) {
    uint32_t rec = uint32_t(c.r[3]);
    raise(k, c, rec, uint32_t(c.r[1]));
}

// A breakpoint the title hits: NT's trap, STATUS_BREAKPOINT with
// BREAKPOINT_BREAK, through the same dispatch (Xenia breaks into the host's
// debugger and returns); with no handler the title stops, as on the console
// with no debugger attached.
XF_EXPORT(xboxkrnl, DbgBreakPoint) {
    uint32_t rec = (uint32_t(c.r[1]) - 0x80) & ~15u;
    k.store32(rec + record::CODE, STATUS_BREAKPOINT);
    k.store32(rec + record::FLAGS, 0);
    k.store32(rec + record::RECORD, 0);
    k.store32(rec + record::COUNT, 1);
    k.store32(rec + record::INFO, 0); // BREAKPOINT_BREAK
    raise(k, c, rec, rec);
}

XF_EXPORT(xboxkrnl, RtlUnwind) { unwind(k, c, uint32_t(c.r[3]), uint32_t(c.r[4]), uint32_t(c.r[5]), uint32_t(c.r[6])); }

// The caller's registers (IAR its return address); r3 stays the CONTEXT's
// address, as the CRT expects after the call.
XF_EXPORT(xboxkrnl, RtlCaptureContext) { write_context(k, uint32_t(c.r[3]), capture(c)); }

// The language handler of C's __try: the scope table (a count, then begin,
// end, handler and target for each scope, inner scopes first) is the
// function's handler data. Searching, an __except whose range holds the
// frame's ControlPc has its filter run (EXCEPTION_POINTERS in r3, the
// establisher frame in r4 and r12, as the funclets read it); a positive
// answer unwinds to its block, a negative one continues the exception.
// Unwinding, each __finally whose range holds ControlPc runs (TRUE in r3),
// ControlPc moved past it first (a collided unwind takes it from there), up
// to the scope the target is in the target frame.
XF_EXPORT(xboxkrnl, __C_specific_handler) {
    const uint32_t rec = uint32_t(c.r[3]), frame = uint32_t(c.r[4]), ctx = uint32_t(c.r[5]), dc = uint32_t(c.r[6]);
    const uint32_t sp = uint32_t(c.r[1]);
    uint32_t pc = k.load32(dc + dispatcher::CONTROL_PC), entry = k.load32(dc + dispatcher::FUNCTION_ENTRY);
    uint32_t table = k.load32(k.load32(entry) - 4), count = k.load32(table);
    uint32_t flags = k.load32(rec + record::FLAGS);
    uint32_t pointers = (sp - 0x10) & ~7u, call_sp = (pointers - 0x80) & ~15u;
    if (!(flags & EXCEPTION_UNWIND)) {
        k.store32(pointers, rec);
        k.store32(pointers + 4, ctx);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t scope = table + 4 + 16 * i;
            uint32_t begin = k.load32(scope), end = k.load32(scope + 4), filter = k.load32(scope + 8),
                     target = k.load32(scope + 12);
            if (pc < begin || pc >= end || !target) continue;
            int32_t value = filter == 1 ? 1 : int32_t(call_guest(k, c, filter, call_sp, {pointers, frame}, frame));
            if (k.stopped) break;
            if (value < 0) return result(c, ContinueExecution);
            if (value > 0) unwind(k, c, frame, target, rec, k.load32(rec + record::CODE)); // goes on at target
        }
    } else {
        uint32_t target_ip = 0;
        for (Active *a = active_walks; a; a = a->prev)
            if (a->c == &c && a->unwind) {
                target_ip = a->target_ip;
                break;
            }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t scope = table + 4 + 16 * i;
            uint32_t begin = k.load32(scope), end = k.load32(scope + 4), handler = k.load32(scope + 8),
                     target = k.load32(scope + 12);
            if (pc < begin || pc >= end) continue;
            // in the target frame: a goto out of an inner scope or a jump
            // back into this one ends the termination handlers
            if ((flags & EXCEPTION_TARGET_UNWIND) && target_ip >= begin && target_ip <= end) break;
            if (target) {
                if ((flags & EXCEPTION_TARGET_UNWIND) && target == target_ip) break;
                continue;
            }
            k.store32(dc + dispatcher::CONTROL_PC, end + 4);
            call_guest(k, c, handler, call_sp, {1, frame}, frame);
        }
    }
    result(c, ContinueSearch);
}

} // namespace

// A trap instruction that fired (ppc::System::trap). twi 31, r0, 20 (and 26)
// is the XDK's debug print: r3 the text, r4 its length, printed, and the code
// goes on (Xenia's TrapDebugPrint). 25 Xenia ignores, and so does this. Any
// other is the console's breakpoint (DbgBreakPoint is twi 31, r0, 22):
// STATUS_BREAKPOINT through the title's handlers, which on the console with no
// debugger ends the title when none takes it (Xenia logs "This should be a
// crash!" and goes on).
void trap(Kernel &k, ppc::Context &c, uint32_t at) {
    const uint32_t word = k.load32(at);
    const bool always = (word & 0xFFFF0000u) == 0x0FE00000u; // twi 31, r0, imm
    if (always && ((word & 0xFFFF) == 20 || (word & 0xFFFF) == 26)) {
        uint32_t text = uint32_t(c.r[3]), length = std::min<uint32_t>(uint32_t(c.r[4]), 4096);
        std::string s;
        for (uint32_t i = 0; i < length && text && k.host(text + i)[0]; i++) s += char(k.host(text + i)[0]);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::fprintf(stderr, "guest: %s\n", s.c_str());
        return;
    }
    if (always && (word & 0xFFFF) == 25) return;
    // the record well below the stack pointer: the code may be between a
    // register save under r1 and its stwu
    uint32_t rec = (uint32_t(c.r[1]) - 0x400) & ~15u;
    k.store32(rec + record::CODE, STATUS_BREAKPOINT);
    k.store32(rec + record::FLAGS, 0);
    k.store32(rec + record::RECORD, 0);
    k.store32(rec + record::COUNT, 1);
    k.store32(rec + record::INFO, 0); // BREAKPOINT_BREAK
    raise(k, c, rec, rec, at);
}

void add_function_table(Kernel &k, uint32_t begin, uint32_t end, uint32_t table, uint32_t count) {
    Tables &t = k.state<Tables>();
    std::lock_guard lock(t.mutex);
    t.tables.push_back({begin, end, table, count});
}

} // namespace xf::kernel
