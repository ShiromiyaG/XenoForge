// Emission (recomp.h; docs/recompiler.md): a discovered function as C++.
// Every instruction is one line calling the same ops the interpreter runs
// (ppc_generated.h's I()); the emitter does the control flow: labels and
// gotos inside the function, C++ calls for bl, tail calls for branches to
// other functions, return for blr, a switch on CTR for a jump table, and
// dispatch() for whatever is indirect or has no generated code.
//
// Stack switches (docs/recompiler.md, "Stack switches"): a function that
// switches the stack checks each return against its entry's LR and r1
// (XF_CHECKED_RETURN), and a branch to KeSetCurrentStackPointers' thunk is
// switch_stack(), which says where this frame goes on when its stack is
// switched back to. A function with resume sites has the runtime run its
// body (resumable()): an Unwind to one of them with r1 inside its frame goes
// on there. The generated code never catches: the runtime throws and catches
// every Unwind, so the code needs no C++ runtime (a game library, D1).

#include "xenoforge/recomp.h"

#include "xenoforge/ppc.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace xf::recomp {
namespace {

using ppc::Instr;
using ppc::Opcode;

int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

// In a function of more than PART_FUNCTION instructions, each stretch of
// straight-line code (no branch, no label inside, no hook) of PART_MINIMUM
// instructions or more goes, PART_LENGTH at a time, into a part: a function
// of its own (ppc_generated.h's XF_PART, a static member of a class local
// to the function) that the body calls. clang's optimizer takes time that
// grows faster than a function's length: game D's longest, 27,500
// instructions of floating point, took 4.3 minutes in one function (14
// before fpu.h's slow paths stopped taking the context) and 38 s in parts.
// Functions that long are a handful in a game (game D: 11 over 4,096 of
// 85,859); a part costs a call and the registers it holds in the context
// read again, once every thousand instructions. From 2,048 instructions
// (4,096 before): a function of game A of 3,516, mostly loads, stores and
// floating point, took 12.7 s and 0.62 GB of clang alone, 4.1 s and 0.10 GB
// in parts (docs/recompiler.md, "The recompile step's memory").
constexpr size_t PART_FUNCTION = 2048, PART_LENGTH = 1024, PART_MINIMUM = 256;

class Emitter {
public:
    Emitter(const Image &image, const Discovery &d, const Function &f, const std::string &prefix,
            std::set<uint32_t> *calls, const EmitHooks *hooks)
        : image_(image), d_(d), f_(f), prefix_(prefix), end_(f.address + std::max<uint32_t>(f.size, 4)), calls_(calls),
          checked_(f.switches_stack), hooks_(hooks && !hooks->empty() ? hooks : nullptr) {
        for (uint32_t a : f.resumes)
            if (a > f.address && a < end_) resumes_.insert(a);
    }

    std::string run() {
        walk();
        // the resume sites the walk reached (an invalid word stops it)
        std::erase_if(resumes_, [&](uint32_t a) { return !code_.count(a); });
        for (uint32_t a : resumes_) referenced_.insert(a);
        if (!resumes_.empty()) base_ = 2;
        // first pass: which labels are jumped to
        for (uint32_t pc : code_) instruction(pc, true);
        place_parts();
        std::string body, parts;
        out_ = &body;
        for (auto it = code_.begin(); it != code_.end(); ++it) {
            const uint32_t pc = *it;
            if (referenced_.count(pc)) line("L_" + hex8(pc) + ":", -1);
            auto part = parts_.find(pc);
            if (part == parts_.end()) {
                instruction(pc, false);
                continue;
            }
            // the stretch into its part, the call here
            const std::string name = "p" + std::to_string(parts_emitted_++);
            line("xf_parts::" + name + "(c);");
            const int base = base_;
            out_ = &parts, base_ = 2;
            line("XF_PART(" + name + ") {", 0);
            base_ = 3;
            // a region (place_regions) has its own labels, its first
            // instruction's too (a loop back to its start), and its exit's,
            // the instruction after it, which its branches out reach
            const bool region = regions_.count(pc) != 0;
            uint32_t last = pc;
            for (size_t k = 0; k < part->second; k++, ++it) {
                if (region && referenced_.count(*it)) line("L_" + hex8(*it) + ":", -1);
                instruction(last = *it, false);
            }
            if (region && referenced_.count(last + 4)) line("L_" + hex8(last + 4) + ":;", -1);
            --it;
            base_ = 2;
            line("}");
            out_ = &body, base_ = base;
        }
        if (!parts.empty()) parts = "    struct xf_parts {\n" + parts + "    };\n";
        // A function the port hooks: its body is the original (xfo_), and
        // its own name the hook slot's call (XF_HOOKED), what every caller
        // and the table reach.
        const bool slot = hooks_ && hooks_->slot;
        std::string head = "XF_FN(" + (slot ? "xfo_" + hex8(f_.address) : name(f_.address)) + ") {";
        if (!f_.name.empty()) head += " // " + f_.name;
        head += "\n";
        const std::string wrapper = slot ? "XF_FN(" + name(f_.address) + ") { XF_HOOKED(xf_hook_" + hex8(f_.address) +
                                               ", xfo_" + hex8(f_.address) + "); }\n"
                                         : std::string();
        head += parts;
        if (resumes_.empty()) {
            // where its returns are checked against (XF_CHECKED_RETURN)
            if (checked_) head += "    const uint32_t xf_lr = uint32_t(c.lr), xf_sp = uint32_t(c.r[1]);\n";
            return head + body + "}\n" + wrapper;
        }
        // Its body a function the runtime runs (ppc_generated.h's
        // XF_RESUMABLE_BODY), from the entry and again from the resume site an
        // Unwind came back to with r1 inside its frame: the runtime catches,
        // so the generated code carries no C++ exception handling of its own.
        // The body gets the entry's LR and r1 (xf_lr, xf_sp) for its checked
        // returns.
        std::string sites, cases;
        for (uint32_t a : resumes_) {
            sites += (sites.empty() ? "0x" : ", 0x") + hex8(a) + "u";
            cases += "        case 0x" + hex8(a) + ": goto L_" + hex8(a) + ";\n";
        }
        return head + "    static constexpr uint32_t xf_sites[] = {" + sites + "};\n" +
               "    ::xf::ppc::resumable(c, XF_RESUMABLE_BODY {\n" + "        switch (xf_at) {\n" + cases +
               "        }\n" + body + "    }, uint32_t(c.lr), uint32_t(c.r[1]), xf_sites, " +
               std::to_string(resumes_.size()) + ");\n" + "}\n" + wrapper;
    }

    std::string name(uint32_t address) const { return prefix_ + hex8(address); }

private:
    // a function the code can call directly (imports go through dispatch)
    bool callable(uint32_t address) const {
        auto it = d_.functions.find(address);
        return it != d_.functions.end() && it->second.source != Source::Import;
    }
    bool internal(uint32_t target) const {
        return target >= f_.address && target < end_ && code_.count(target) &&
               (target == f_.address || !d_.functions.count(target));
    }
    // a return: checked in a function that switches the stack
    std::string ret() const { return checked_ ? "XF_CHECKED_RETURN();" : "return;"; }

    // the instructions reached from the entry, the blocks discovery found,
    // the jump tables' targets and the resume sites (which another function
    // may start at: the pointer scan takes some catch continuations for
    // starts)
    void walk() {
        std::vector<uint32_t> work(f_.blocks.begin(), f_.blocks.end());
        work.push_back(f_.address);
        work.insert(work.end(), resumes_.begin(), resumes_.end());
        auto inside = [&](uint32_t t) {
            return t >= f_.address && t < end_ && (t == f_.address || !d_.functions.count(t) || resumes_.count(t));
        };
        while (!work.empty()) {
            uint32_t pc = work.back();
            work.pop_back();
            for (; pc < end_ && !code_.count(pc); pc += 4) {
                Instr i{image_.word(pc)};
                Opcode op = ppc::decode(i.code);
                if (i.code == 0 || op == Opcode::Invalid) break;
                code_.insert(pc);
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
                    auto table = d_.tables.find(pc);
                    if (op == Opcode::bcctrx && table != d_.tables.end())
                        for (uint32_t t : table->second.targets)
                            if (inside(t)) work.push_back(t);
                    break;
                }
            }
        }
    }

    // A long function's parts (PART_FUNCTION): its stretches of plain
    // instructions (not a branch, no hook, the next one emitted: a line or
    // two each, no control flow), each begun anew at a label (one may be
    // jumped to only at a part's start, before its call), PART_LENGTH at a
    // time, those of PART_MINIMUM or more
    bool plain(uint32_t pc) const {
        const Opcode op = ppc::decode(image_.word(pc));
        if (op == Opcode::bx || op == Opcode::bcx || op == Opcode::bclrx || op == Opcode::bcctrx) return false;
        if (hooks_ && hooks_->mid.count(pc)) return false;
        return code_.count(pc + 4) != 0;
    }
    void place_parts() {
        if (code_.size() <= PART_FUNCTION) return;
        std::vector<uint32_t> run;
        auto flush = [&] {
            for (size_t k = 0; k < run.size(); k += PART_LENGTH) {
                const size_t n = std::min(PART_LENGTH, run.size() - k);
                if (n >= PART_MINIMUM) parts_[run[k]] = n;
            }
            run.clear();
        };
        for (uint32_t pc : code_) {
            const bool p = plain(pc);
            if (!run.empty() && (!p || referenced_.count(pc) || run.back() + 4 != pc)) flush();
            if (p) run.push_back(pc);
        }
        flush();
        place_regions();
    }

    // What the plain stretches leave of a long function, in regions: code
    // with branches inside, one entry (its first instruction) and one exit
    // (the instruction after its last), PART_LENGTH instructions at most and
    // PART_MINIMUM at least, which go into parts the same way. A region's
    // instructions are consecutive and each one either an instruction of
    // the semantics, a branch to a label in the function, or a call; no
    // return, tail call, dispatch that returns or table switch (none of
    // which a part can do for its function), and no stack switch. Every
    // branch inside goes to an instruction inside or to the exit, and every
    // branch from outside to the first instruction. A function 12,000
    // instructions long with branches throughout (no plain stretch long
    // enough) took clang 48 s alone in one function (docs/recompiler.md,
    // "The recompile step's memory"). Not in a function whose returns are
    // checked, with resume sites or a port's hooks: their bodies return in
    // ways a part cannot.
    bool region_kind(uint32_t pc) const {
        const Instr i{image_.word(pc)};
        const Opcode op = ppc::decode(i.code);
        auto callee = [&](uint32_t t) { return !d_.stack_thunks.count(t); };
        switch (op) {
        case Opcode::bx: {
            const uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            return i.I.LK ? callee(t) : internal(t);
        }
        case Opcode::bcx: {
            const uint32_t t = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
            return i.B.LK ? callee(t) : internal(t);
        }
        case Opcode::bclrx:
        case Opcode::bcctrx: return i.XL.LK != 0; // an indirect call
        default: return true;
        }
    }
    // the branch target of a branch inside a region (0: none)
    uint32_t region_target(uint32_t pc) const {
        const Instr i{image_.word(pc)};
        const Opcode op = ppc::decode(i.code);
        if (op == Opcode::bx && !i.I.LK) return uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
        if (op == Opcode::bcx && !i.B.LK) return uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
        return 0;
    }
    void place_regions() {
        if (checked_ || !resumes_.empty() || hooks_) return;
        const std::vector<uint32_t> pcs(code_.begin(), code_.end());
        std::set<uint32_t> covered;
        for (const auto &[start, n] : parts_)
            for (size_t k = 0; k < n; k++) covered.insert(start + uint32_t(4 * k));
        for (size_t i = 0; i < pcs.size();) {
            const uint32_t first = pcs[i];
            // the lowest target or label source seen, the highest target, the highest label source
            uint32_t low = UINT32_MAX, target_high = 0, source_high = 0;
            size_t best = 0;
            for (size_t j = i; j < pcs.size() && j - i < PART_LENGTH; j++) {
                const uint32_t pc = pcs[j];
                if ((j > i && pc != pcs[j - 1] + 4) || covered.count(pc) || !region_kind(pc)) break;
                if (const uint32_t t = region_target(pc))
                    low = std::min(low, t), target_high = std::max(target_high, t);
                // a label after the first: every branch to it from inside
                if (auto s = sources_.find(pc); j > i && s != sources_.end())
                    low = std::min(low, s->second.first), source_high = std::max(source_high, s->second.second);
                if (low < first) break; // a branch back before it, or into it from before: none further
                // an end: the next instruction emitted, the targets inside or
                // the exit, the labels' branches from inside
                if (j + 1 - i >= PART_MINIMUM && code_.count(pc + 4) && low >= first && target_high <= pc + 4 &&
                    source_high <= pc)
                    best = j + 1 - i;
            }
            if (best) {
                parts_[first] = best;
                regions_.insert(first);
                i += best;
            } else {
                i++;
            }
        }
    }

    // a line of the body, indent levels past the body's own
    void line(const std::string &text, int indent = 0) {
        if (!out_) return;
        out_->append(size_t(base_ + indent) * 4, ' ');
        *out_ += text;
        *out_ += '\n';
    }

    std::string go(uint32_t target, bool first) {
        if (internal(target)) {
            if (first) {
                referenced_.insert(target);
                // the branches to it, the lowest and the highest (place_regions)
                auto [s, added] = sources_.try_emplace(target, at_, at_);
                s->second.first = std::min(s->second.first, at_), s->second.second = std::max(s->second.second, at_);
            }
            return "goto L_" + hex8(target) + ";";
        }
        // the stack switch: this frame goes on, when switched back to, where
        // its own return goes
        if (d_.stack_thunks.count(target))
            return "{ ::xf::ppc::switch_stack(c, 0x" + hex8(target) + ", " +
                   (checked_ ? "xf_lr" : "uint32_t(c.lr) & ~3u") + "); " + ret() + " }";
        if (callable(target)) {
            if (calls_) calls_->insert(target);
            // no tail call where the return is checked or in a resumable body
            if (checked_ || !resumes_.empty()) return "{ " + name(target) + "(c); " + ret() + " }";
            return "XF_TAIL(" + name(target) + ");";
        }
        return "{ ::xf::ppc::dispatch(c, 0x" + hex8(target) + "); " + ret() + " }";
    }
    // a call, LR already set to `next`
    std::string call(uint32_t target, const std::string &next) {
        if (d_.stack_thunks.count(target)) return "::xf::ppc::switch_stack(c, 0x" + hex8(target) + ", " + next + ");";
        if (callable(target)) {
            if (calls_) calls_->insert(target);
            return name(target) + "(c);";
        }
        return "::xf::ppc::dispatch(c, 0x" + hex8(target) + ");";
    }

    // what follows an instruction that may fall through, when the next one is not emitted
    void fall_through(uint32_t pc) {
        uint32_t next = pc + 4;
        if (code_.count(next)) return;
        if (next >= end_) {
            line(go(next, false)); // into the next function
            return;
        }
        line("::xf::ppc::invalid(c, 0x" + hex8(next) + "); return;");
    }

    // A mid-function hook before the instruction at pc (EmitHooks::mid): when
    // its slot holds hooks, the runtime runs them and says where the code
    // goes on: here (0), a return (1), one of the labels the project file
    // gave or the next instruction, else a dispatch to the address, as a
    // branch to an address with no label is.
    void mid_hook(uint32_t pc, bool first) {
        if (!hooks_) return;
        auto it = hooks_->mid.find(pc);
        if (it == hooks_->mid.end()) return;
        std::set<uint32_t> jumps(it->second.begin(), it->second.end());
        jumps.insert(pc + 4);
        std::erase_if(jumps, [&](uint32_t t) { return t == 0 || t == 1 || !internal(t); });
        if (first) {
            referenced_.insert(jumps.begin(), jumps.end());
            hooks_->placed.insert(pc);
            return;
        }
        const std::string slot = "xf_mid_" + hex8(pc);
        std::string cases = "case 1: " + ret();
        for (uint32_t t : jumps) cases += " case 0x" + hex8(t) + ": goto L_" + hex8(t) + ";";
        line("if (XF_HOOKS(" + slot + ")) { // the port's mid-function hook");
        line("const uint32_t xf_to = ::xf::ppc::mid_hook(c, " + slot + ");", 1);
        line("if (xf_to) switch (xf_to) { " + cases + " default: ::xf::ppc::dispatch(c, xf_to); " + ret() + " }", 1);
        line("}");
    }

    void instruction(uint32_t pc, bool first) {
        at_ = pc;
        Instr i{image_.word(pc)};
        Opcode op = ppc::decode(i.code);
        mid_hook(pc, first);
        if (!first) line("// " + hex8(pc) + " " + ppc::disassemble(pc, i.code));
        std::string next = "0x" + hex8(pc + 4);
        auto bc = [&](uint32_t bo, uint32_t bi, bool ctr) {
            return std::string(ctr ? "BCCTR(" : "BC(") + std::to_string(bo) + ", " + std::to_string(bi) + ")";
        };
        switch (op) {
        case Opcode::bx: {
            uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            if (i.I.LK) {
                if (first) return;
                line("c.lr = " + next + "; " + call(t, next));
                fall_through(pc);
            } else {
                std::string s = go(t, first);
                // a branch backward is a yield point (a loop's back edge)
                if (!first) line(t <= pc ? "XF_YIELD(); " + s : s);
            }
            return;
        }
        case Opcode::bcx: {
            uint32_t t = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
            bool always = (i.B.BO & 0x14) == 0x14;
            if (i.B.LK) {
                if (first) return;
                line("{ bool taken = " + bc(i.B.BO, i.B.BI, false) + "; c.lr = " + next + "; if (taken) " +
                     call(t, next) + " }");
                fall_through(pc);
                return;
            }
            std::string s = go(t, first);
            if (first) return;
            // taken backward: a yield point
            if (t <= pc) s = "{ XF_YIELD(); " + s + " }";
            if (always) line(s);
            else {
                line("if (" + bc(i.B.BO, i.B.BI, false) + ") " + s);
                fall_through(pc);
            }
            return;
        }
        case Opcode::bclrx: {
            if (first) return;
            bool always = (i.XL.BO & 0x14) == 0x14;
            if (i.XL.LK) {
                line("{ uint32_t target = uint32_t(c.lr) & ~3u; bool taken = " + bc(i.XL.BO, i.XL.BI, false) +
                     "; c.lr = " + next + "; if (taken) { XF_INDIRECT(); ::xf::ppc::dispatch(c, target); } }");
                fall_through(pc);
            } else if (always) {
                line(ret());
            } else {
                line("if (" + bc(i.XL.BO, i.XL.BI, false) + ") " + ret());
                fall_through(pc);
            }
            return;
        }
        case Opcode::bcctrx: {
            bool always = (i.XL.BO & 0x14) == 0x14;
            auto table = d_.tables.find(pc);
            if (!i.XL.LK && always && table != d_.tables.end()) {
                std::set<uint32_t> cases(table->second.targets.begin(), table->second.targets.end());
                std::vector<std::string> lines;
                for (uint32_t t : cases) lines.push_back("case 0x" + hex8(t) + ": " + go(t, first));
                if (first) return;
                line("switch (uint32_t(c.ctr) & ~3u) {");
                for (const std::string &l : lines) line(l);
                line("default: ::xf::ppc::dispatch(c, uint32_t(c.ctr) & ~3u); " + ret());
                line("}");
                return;
            }
            if (first) return;
            if (i.XL.LK) {
                line("{ uint32_t target = uint32_t(c.ctr) & ~3u; bool taken = " + bc(i.XL.BO, i.XL.BI, true) +
                     "; c.lr = " + next + "; if (taken) { XF_INDIRECT(); ::xf::ppc::dispatch(c, target); } }");
                fall_through(pc);
            } else if (always) {
                line("::xf::ppc::dispatch(c, uint32_t(c.ctr) & ~3u); " + ret());
            } else {
                line("if (" + bc(i.XL.BO, i.XL.BI, true) + ") { ::xf::ppc::dispatch(c, uint32_t(c.ctr) & ~3u); " +
                     ret() + " }");
                fall_through(pc);
            }
            return;
        }
        default:
            if (first) return;
            // a trap that fires tells the system where it is (System::trap reads
            // c.pc, which the generated code keeps nowhere else)
            if (op == Opcode::tw || op == Opcode::twi || op == Opcode::td || op == Opcode::tdi)
                line("c.pc = 0x" + hex8(pc) + ";");
            line("I(" + std::string(ppc::info(op).name) + ", 0x" + hex8(i.code) + ");");
            fall_through(pc);
            return;
        }
    }

    const Image &image_;
    const Discovery &d_;
    const Function &f_;
    std::string prefix_;
    uint32_t end_;
    std::set<uint32_t> code_, referenced_;
    std::map<uint32_t, size_t> parts_; // a part's first instruction: its length
    std::set<uint32_t> regions_;       // the parts that are regions (place_regions)
    // each label's branches, by the lowest and the highest address they are at
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> sources_;
    uint32_t at_ = 0; // the instruction being emitted
    size_t parts_emitted_ = 0;
    std::string *out_ = nullptr;
    std::set<uint32_t> *calls_;  // the functions it calls by name, when asked
    bool checked_;               // it switches the stack: its returns are checked
    std::set<uint32_t> resumes_; // its resume sites inside its range
    int base_ = 1;               // the body's indentation
    const EmitHooks *hooks_;     // what the port hooks in it (nullptr: nothing)
};

} // namespace

std::string function_name(const std::string &prefix, uint32_t address) { return prefix + hex8(address); }

std::string emit_function(const Image &image, const Discovery &d, const Function &f, const std::string &prefix,
                          std::set<uint32_t> *calls, const EmitHooks *hooks) {
    return Emitter(image, d, f, prefix, calls, hooks).run();
}

} // namespace xf::recomp
