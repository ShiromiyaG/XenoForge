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

#include <cstdio>
#include <set>

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

class Emitter {
public:
    Emitter(const Image &image, const Discovery &d, const Function &f, const std::string &prefix,
            std::set<uint32_t> *calls)
        : image_(image), d_(d), f_(f), prefix_(prefix), end_(f.address + std::max<uint32_t>(f.size, 4)), calls_(calls),
          checked_(f.switches_stack) {
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
        std::string body;
        out_ = &body;
        for (uint32_t pc : code_) {
            if (referenced_.count(pc)) line("L_" + hex8(pc) + ":", -1);
            instruction(pc, false);
        }
        std::string head = "XF_FN(" + name(f_.address) + ") {";
        if (!f_.name.empty()) head += " // " + f_.name;
        head += "\n";
        if (resumes_.empty()) {
            // where its returns are checked against (XF_CHECKED_RETURN)
            if (checked_) head += "    const uint32_t xf_lr = uint32_t(c.lr), xf_sp = uint32_t(c.r[1]);\n";
            return head + body + "}\n";
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
               std::to_string(resumes_.size()) + ");\n" + "}\n";
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

    // a line of the body, indent levels past the body's own
    void line(const std::string &text, int indent = 0) {
        if (!out_) return;
        out_->append(size_t(base_ + indent) * 4, ' ');
        *out_ += text;
        *out_ += '\n';
    }

    std::string go(uint32_t target, bool first) {
        if (internal(target)) {
            if (first) referenced_.insert(target);
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

    void instruction(uint32_t pc, bool first) {
        Instr i{image_.word(pc)};
        Opcode op = ppc::decode(i.code);
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
                if (!first) line(s);
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
                     "; c.lr = " + next + "; if (taken) ::xf::ppc::dispatch(c, target); }");
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
                     "; c.lr = " + next + "; if (taken) ::xf::ppc::dispatch(c, target); }");
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
    std::string *out_ = nullptr;
    std::set<uint32_t> *calls_;  // the functions it calls by name, when asked
    bool checked_;               // it switches the stack: its returns are checked
    std::set<uint32_t> resumes_; // its resume sites inside its range
    int base_ = 1;               // the body's indentation
};

} // namespace

std::string function_name(const std::string &prefix, uint32_t address) { return prefix + hex8(address); }

std::string emit_function(const Image &image, const Discovery &d, const Function &f, const std::string &prefix,
                          std::set<uint32_t> *calls) {
    return Emitter(image, d, f, prefix, calls).run();
}

} // namespace xf::recomp
