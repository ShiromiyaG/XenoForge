// The guest's calling convention as the generated code could trust it
// (calling_convention.h; docs/recompiler.md, "Trusting the calling
// convention, weighed"): which functions keep it plainly enough that their
// non-volatile registers could live in C locals.
//
// A function keeps it when every non-volatile register it names (r14-r31,
// f14-f31, v14-v31 and v64-v127) is saved in its prologue and restored in
// its epilogue, by the save and restore helpers (__savegprlr_N and the
// others, which discovery names) or by its own stores to and loads from its
// frame; and nothing else reads the registers' values from outside it while
// it runs: no resume site (a longjmp's or a catch's return reads the context
// in its frame), no stack switch, no setjmp (which reads the registers it is
// handed), no exception data (its handlers and funclets read the registers
// the unwinder restores), no load or store of several registers at once.

#include "calling_convention.h"

#include "xenoforge/ppc.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace xf::recomp {
namespace {

using ppc::Instr;
using ppc::Opcode;

int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }

// The registers an instruction names, read from its disassembly (Xenia's
// format: r<n>, fr<n>, vr<n>; crf<n> and crb<n> are condition register
// fields), in the order it names them.
struct Named {
    char kind; // 'r', 'f', 'v'
    uint32_t n;
};
std::vector<Named> named_registers(uint32_t pc, uint32_t code) {
    std::vector<Named> out;
    const std::string text = ppc::disassemble(pc, code);
    // past the mnemonic
    size_t k = text.find(' ');
    while (k != std::string::npos && k < text.size()) {
        if (!std::isalpha(uint8_t(text[k]))) {
            k++;
            continue;
        }
        size_t word = k;
        while (k < text.size() && std::isalpha(uint8_t(text[k]))) k++;
        const std::string letters = text.substr(word, k - word);
        size_t digits = k;
        while (k < text.size() && std::isdigit(uint8_t(text[k]))) k++;
        if (k == digits) continue;
        const uint32_t n = uint32_t(std::stoul(text.substr(digits, k - digits)));
        if (letters == "r")
            out.push_back({'r', n});
        else if (letters == "fr")
            out.push_back({'f', n});
        else if (letters == "vr")
            out.push_back({'v', n});
    }
    return out;
}

bool non_volatile(const Named &r) {
    if (r.kind == 'v') return (r.n >= 14 && r.n <= 31) || r.n >= 64;
    return r.n >= 14 && r.n <= 31;
}
void add(NonVolatileSet &s, const Named &r) {
    if (r.kind == 'r')
        s.gpr |= 1u << r.n;
    else if (r.kind == 'f')
        s.fpr |= 1u << r.n;
    else
        s.vr[r.n / 64] |= uint64_t(1) << (r.n % 64);
}

bool helper_range(const std::string &name, const char *prefix, uint32_t &first) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (name.compare(0, n, prefix) != 0) return false;
    first = uint32_t(std::stoul(name.substr(n)));
    return true;
}
// what a save or restore helper covers (its name's number to 31, or to 127
// for the VMX128 family's)
NonVolatileSet helper_set(const std::string &name, bool &save, bool &restore) {
    NonVolatileSet s;
    uint32_t first = 0;
    save = restore = false;
    if (helper_range(name, "__savegprlr_", first) || helper_range(name, "__restgprlr_", first)) {
        for (uint32_t n = first; n <= 31; n++) s.gpr |= 1u << n;
    } else if (helper_range(name, "__savefpr_", first) || helper_range(name, "__restfpr_", first)) {
        for (uint32_t n = first; n <= 31; n++) s.fpr |= 1u << n;
    } else if (helper_range(name, "__savevmx_", first) || helper_range(name, "__restvmx_", first)) {
        for (uint32_t n = first; n <= (first >= 64 ? 127u : 31u); n++) s.vr[n / 64] |= uint64_t(1) << (n % 64);
    } else {
        return s;
    }
    save = name.compare(0, 6, "__save") == 0;
    restore = !save;
    return s;
}

bool frame_base(uint32_t r) { return r == 1 || r == 11 || r == 12; }

bool is_store(Opcode op) {
    switch (op) {
    case Opcode::stw:
    case Opcode::std:
    case Opcode::stfd:
    case Opcode::stvx:
    case Opcode::stvx128:
    case Opcode::stvxl:
    case Opcode::stvxl128: return true;
    default: return false;
    }
}
bool is_load(Opcode op) {
    switch (op) {
    case Opcode::lwz:
    case Opcode::ld:
    case Opcode::lfd:
    case Opcode::lvx:
    case Opcode::lvx128:
    case Opcode::lvxl:
    case Opcode::lvxl128: return true;
    default: return false;
    }
}

} // namespace

size_t NonVolatileSet::count() const {
    return size_t(__builtin_popcount(gpr) + __builtin_popcount(fpr) + __builtin_popcountll(vr[0]) +
                  __builtin_popcountll(vr[1]));
}

std::set<uint32_t> function_instructions(const Image &image, const Discovery &d, const Function &f) {
    // as the emitter walks it (recomp_emit.cpp's walk): from the entry, the
    // blocks discovery found, the jump tables' targets and the resume sites
    const uint32_t end = f.address + std::max<uint32_t>(f.size, 4);
    std::set<uint32_t> code;
    std::vector<uint32_t> work(f.blocks.begin(), f.blocks.end());
    work.push_back(f.address);
    for (uint32_t a : f.resumes)
        if (a > f.address && a < end) work.push_back(a);
    auto inside = [&](uint32_t t) {
        return t >= f.address && t < end && (t == f.address || !d.functions.count(t) || f.resumes.count(t));
    };
    while (!work.empty()) {
        uint32_t pc = work.back();
        work.pop_back();
        for (; pc < end && !code.count(pc); pc += 4) {
            Instr i{image.word(pc)};
            Opcode op = ppc::decode(i.code);
            if (i.code == 0 || op == Opcode::Invalid) break;
            code.insert(pc);
            if (op == Opcode::bx && !i.I.LK) {
                uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
                if (inside(t)) work.push_back(t);
                break;
            }
            if (op == Opcode::bcx && !i.B.LK) {
                uint32_t t = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
                if (inside(t)) work.push_back(t);
                if ((i.B.BO & 0x14) == 0x14) break;
            }
            if ((op == Opcode::bclrx || op == Opcode::bcctrx) && !i.XL.LK && (i.XL.BO & 0x14) == 0x14) {
                auto table = d.tables.find(pc);
                if (op == Opcode::bcctrx && table != d.tables.end())
                    for (uint32_t t : table->second.targets)
                        if (inside(t)) work.push_back(t);
                break;
            }
        }
    }
    return code;
}

const char *convention_name(Convention c) {
    switch (c) {
    case Convention::Keeps: return "keeps it";
    case Convention::NoNonVolatile: return "names no non-volatile register";
    case Convention::Resumes: return "has resume sites";
    case Convention::SwitchesStack: return "switches the stack";
    case Convention::SavesStack: return "saves the stack (setjmp)";
    case Convention::ExceptionData: return "has exception data";
    case Convention::Multiple: return "loads or stores several registers at once";
    case Convention::NotSaved: return "names a non-volatile register it does not save and restore";
    case Convention::Skipped: return "a helper or an import";
    }
    return "?";
}

std::map<uint32_t, CallingConvention> analyze_calling_convention(const Image &image, const Discovery &d) {
    std::map<uint32_t, bool> exception;
    for (const PdataEntry &p : image.pdata) exception[p.begin] = exception[p.begin] || p.exception;
    std::map<uint32_t, CallingConvention> out;
    for (const auto &[address, f] : d.functions) {
        CallingConvention &cc = out[address];
        if (f.source == Source::Import || f.source == Source::Helper) {
            cc.kind = Convention::Skipped;
            continue;
        }
        const std::set<uint32_t> code = function_instructions(image, d, f);
        cc.instructions = code.size();
        bool multiple = false;
        for (uint32_t pc : code) {
            Instr i{image.word(pc)};
            Opcode op = ppc::decode(i.code);
            if (op == Opcode::lmw || op == Opcode::stmw || op == Opcode::lswi || op == Opcode::lswx ||
                op == Opcode::stswi || op == Opcode::stswx)
                multiple = true;
            // a call or tail branch to a save or restore helper
            if (op == Opcode::bx) {
                uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
                auto it = d.functions.find(t);
                if (it != d.functions.end() && it->second.source == Source::Helper) {
                    bool save, restore;
                    NonVolatileSet s = helper_set(it->second.name, save, restore);
                    if (save) cc.saved.add(s);
                    if (restore) cc.restored.add(s);
                    continue;
                }
            }
            const std::vector<Named> regs = named_registers(pc, i.code);
            for (size_t k = 0; k < regs.size(); k++) {
                if (!non_volatile(regs[k])) continue;
                add(cc.named, regs[k]);
                // the frame's own saves and restores: the register first, its
                // frame's base (r1, or r11/r12 holding an address in it) second
                if (k == 0 && regs.size() >= 2 && regs[1].kind == 'r' && frame_base(regs[1].n)) {
                    if (is_store(op)) add(cc.saved, regs[0]);
                    if (is_load(op)) add(cc.restored, regs[0]);
                }
            }
        }
        auto ex = exception.find(address);
        if (!f.resumes.empty())
            cc.kind = Convention::Resumes;
        else if (f.switches_stack)
            cc.kind = Convention::SwitchesStack;
        else if (f.saves_stack)
            cc.kind = Convention::SavesStack;
        else if (ex != exception.end() && ex->second)
            cc.kind = Convention::ExceptionData;
        else if (multiple)
            cc.kind = Convention::Multiple;
        else if (cc.named.empty())
            cc.kind = Convention::NoNonVolatile;
        else if (!cc.named.within(cc.saved) || !cc.named.within(cc.restored))
            cc.kind = Convention::NotSaved;
        else
            cc.kind = Convention::Keeps;
    }
    return out;
}

std::vector<NonVolatileSet> named_non_volatile(const Image &image, const std::vector<uint32_t> &addresses) {
    std::vector<NonVolatileSet> out(addresses.size());
    for (size_t k = 0; k < addresses.size(); k++)
        for (const Named &r : named_registers(addresses[k], image.word(addresses[k])))
            if (non_volatile(r)) add(out[k], r);
    return out;
}

} // namespace xf::recomp
