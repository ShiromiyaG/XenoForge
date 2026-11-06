// Discovery (recomp.h; docs/recompiler.md, "Discovery"). Written anew after
// what rexglue's fork does (../rexglue-sdk/src/codegen) and XenonAnalyse's
// jump table patterns (../XenonRecomp, MIT); no code taken from either. The
// save and restore helpers are found by their first words, as rexglue's
// detectSaveRestoreHelpers finds them, one function per register.
//
// The jump tables are read by following the value moved to CTR backwards
// through the instructions that made it, into constants, loads from a table
// and shifts; the four forms the XDK's compiler writes are recognized in the
// result, whatever order the instructions came in.
//
// The data pointer scan at the end of the file (scan_pointers and discover's
// passes) is translated from rexglue's fork, codegen/pointer_scan.cpp (BSD;
// see LICENSE-rexglue and the notice there).

#include "xenoforge/recomp.h"

#include "xenoforge/exports.h"
#include "xenoforge/ppc.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>

namespace xf::recomp {
namespace {

using ppc::Instr;
using ppc::Opcode;

int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }
uint32_t spr_number(Instr i) { return ((i.XFX.spr & 0x1F) << 5) | ((i.XFX.spr >> 5) & 0x1F); }

constexpr uint32_t BLR = 0x4E800020;
constexpr uint32_t BCTR = 0x4E800420;
constexpr uint32_t TABLE_MAX = 512;   // entries read when no bound is found (rexglue's)
constexpr uint32_t BACKWARD_MAX = 64; // instructions looked back from a bctr

// The general-purpose register an instruction writes, if any (enough for
// following a value backwards: an instruction not listed writes none).
std::optional<uint32_t> written_gpr(Opcode op, Instr i, uint32_t *second = nullptr) {
    const ppc::OpcodeInfo &in = ppc::info(op);
    std::string_view name = in.name;
    if (in.group == 'm' && name[0] == 'l' && name.substr(0, 2) != "lv" && name.substr(0, 2) != "lf") {
        // loads; the update forms write RA too
        bool update = name.find('u') != std::string_view::npos && name != "lhbrx" && name != "lwbrx";
        if (second && update) *second = i.D.RA;
        return uint32_t(i.D.RT);
    }
    if (in.group == 'm' && name.substr(0, 2) == "lf" && name.find('u') != std::string_view::npos) return uint32_t(i.D.RA);
    if (in.group == 'm' && name.substr(0, 2) == "st" && name.find('u') != std::string_view::npos) return uint32_t(i.D.RA);
    switch (op) {
    case Opcode::addi: case Opcode::addis: case Opcode::addic: case Opcode::addicx: case Opcode::mulli:
    case Opcode::subficx: case Opcode::mfspr: case Opcode::mfcr: case Opcode::mftb: case Opcode::mfmsr:
        return uint32_t(i.D.RT);
    case Opcode::ori: case Opcode::oris: case Opcode::xori: case Opcode::xoris: case Opcode::andix: case Opcode::andisx:
        return uint32_t(i.D.RA);
    default: break;
    }
    if (in.group != 'i') return std::nullopt;
    switch (in.form) {
    case ppc::Form::XO: return uint32_t(i.XO.RT);
    case ppc::Form::X:
        if (op == Opcode::cmp || op == Opcode::cmpl || op == Opcode::sync || op == Opcode::eieio) return std::nullopt;
        return uint32_t(i.X.RA); // logic, shifts, counts, extends: rS -> rA
    case ppc::Form::M: case ppc::Form::MD: case ppc::Form::MDS: case ppc::Form::XS: return uint32_t(i.X.RA);
    default: return std::nullopt;
    }
}

// A store of a whole register (a word or a doubleword: what can hold a stack
// pointer), its source in i.D.RT and its base in i.D.RA.
bool stores_gpr(Opcode op) {
    switch (op) {
    case Opcode::stw: case Opcode::stwu: case Opcode::stwx: case Opcode::stwux:
    case Opcode::std: case Opcode::stdu: case Opcode::stdx: case Opcode::stdux:
        return true;
    default: return false;
    }
}

// ---- following a value backwards ----

struct Expr {
    enum Kind { Const, Reg, Load, Add, Shl, Mask } kind = Reg;
    int64_t value = 0;     // Const's value; Reg's register; Load's size; Shl's amount; Mask's mask
    const Expr *a = nullptr, *b = nullptr;
    uint32_t at = 0;       // Reg: where the scan stopped looking for it; an index's Shl, Mask: where it was used
    uint32_t reg = 32;     // an index's Shl, Mask: the register holding the result there
};

class Tracer {
public:
    Tracer(const Image &image, uint32_t floor) : image_(image), floor_(floor) {}

    // the value of reg just before `pc`
    const Expr *value(uint32_t reg, uint32_t pc, uint32_t depth = 0) {
        if (depth > 12) return reg_expr(reg, pc);
        uint32_t steps = 0;
        for (uint32_t at = pc - 4; at >= floor_ && at < pc && steps < BACKWARD_MAX; at -= 4, steps++) {
            Instr i{image_.word(at)};
            if (i.code == BLR || i.code == BCTR) break;
            Opcode op = ppc::decode(i.code);
            if (op == Opcode::bx && !i.I.LK) break; // reached from elsewhere: stop
            if (op == Opcode::bx && i.I.LK) {
                if (reg == 3 || (reg >= 4 && reg <= 12) || reg == 0) return reg_expr(reg, at); // a call clobbers it
                continue;
            }
            uint32_t second = 32;
            std::optional<uint32_t> w = written_gpr(op, i, &second);
            if ((!w || *w != reg) && second != reg) continue;
            if (second == reg && (!w || *w != reg)) return reg_expr(reg, at); // updated base: give up
            return define(op, i, at, depth);
        }
        return reg_expr(reg, pc);
    }

    // an index register, through slwi, clrlwi and mr, each step keeping the
    // register it was in (the bound is a compare on one of them); anything
    // else ends it (as a register used at `pc`)
    const Expr *index(uint32_t reg, uint32_t pc, uint32_t depth = 0) {
        uint32_t steps = 0;
        for (uint32_t at = pc - 4; depth < 4 && at >= floor_ && at < pc && steps < BACKWARD_MAX; at -= 4, steps++) {
            Instr i{image_.word(at)};
            Opcode op = ppc::decode(i.code);
            if (i.code == BLR || i.code == BCTR || (op == Opcode::bx && !i.I.LK)) break;
            uint32_t second = 32;
            std::optional<uint32_t> w = written_gpr(op, i, &second);
            if ((!w || *w != reg) && second != reg) continue;
            if (op == Opcode::rlwinmx && w && *w == reg) {
                // clrlwi first: rlwinm rA, rS, 0, 0, 31 is a 32-bit move, not a shift by 0
                uint32_t sh = i.M.SH, mb = i.M.MB, me = i.M.ME;
                if (sh == 0 && me == 31) {
                    int64_t mask = int64_t(0xFFFFFFFFull >> mb);
                    return make({Expr::Mask, mask, index(i.M.RT, at, depth + 1), nullptr, pc, reg});
                }
                if (mb == 0 && me == 31 - sh)
                    return make({Expr::Shl, int64_t(sh), index(i.M.RT, at, depth + 1), nullptr, pc, reg});
            }
            // mr: the compare may be on the register moved from (a mask that bounds nothing)
            if (op == Opcode::orx && w && *w == reg && i.X.RT == i.X.RB)
                return make({Expr::Mask, int64_t(0xFFFFFFFF), index(i.X.RT, at, depth + 1), nullptr, pc, reg});
            break;
        }
        return reg_expr(reg, pc);
    }

private:
    const Expr *make(Expr e) {
        pool_.push_back(std::make_unique<Expr>(e));
        return pool_.back().get();
    }
    const Expr *reg_expr(uint32_t reg, uint32_t at) { return make({Expr::Reg, int64_t(reg), nullptr, nullptr, at}); }
    const Expr *constant(int64_t v) { return make({Expr::Const, v}); }
    const Expr *add(const Expr *a, const Expr *b) {
        if (a->kind == Expr::Const && b->kind == Expr::Const) return constant(int64_t(uint32_t(a->value + b->value)));
        if (b->kind == Expr::Const && b->value == 0) return a; // ori rX, rX, 0 is the compiler's nop
        if (a->kind == Expr::Const && a->value == 0) return b;
        return make({Expr::Add, 0, a, b});
    }
    const Expr *define(Opcode op, Instr i, uint32_t at, uint32_t depth) {
        auto ra0 = [&](uint32_t r) { return r ? value(r, at, depth + 1) : constant(0); };
        switch (op) {
        case Opcode::addi: return add(ra0(i.D.RA), constant(exts16(i.D.DS)));
        case Opcode::addis: return add(ra0(i.D.RA), constant(exts16(i.D.DS) << 16));
        case Opcode::ori: {
            const Expr *s = value(i.D.RT, at, depth + 1); // ori writes RA from RS (in RT's place)
            return add(s, constant(int64_t(i.D.DS)));
        }
        case Opcode::addx: return add(value(i.XO.RA, at, depth + 1), value(i.XO.RB, at, depth + 1));
        case Opcode::orx:
            if (i.X.RT == i.X.RB) return value(i.X.RT, at, depth + 1); // mr
            break;
        case Opcode::rlwinmx: {
            uint32_t sh = i.M.SH, mb = i.M.MB, me = i.M.ME;
            const Expr *s = value(i.M.RT, at, depth + 1);
            if (sh == 0 && mb == 0 && me == 31) return s; // a 32-bit move
            if (mb == 0 && me == 31 - sh) return make({Expr::Shl, int64_t(sh), s}); // slwi
            if (sh == 0 && me == 31) return make({Expr::Mask, int64_t(0xFFFFFFFFull >> mb), s}); // clrlwi
            break;
        }
        case Opcode::lwzx: case Opcode::lbzx: case Opcode::lhzx: {
            // one operand is the table's address, the other the index: the
            // index is followed through shifts and masks only, so it stays
            // the register the code bounds
            int64_t size = op == Opcode::lwzx ? 4 : op == Opcode::lhzx ? 2 : 1;
            const Expr *a = ra0(i.X.RA), *b = value(i.X.RB, at, depth + 1);
            if (a->kind == Expr::Const) b = index(i.X.RB, at);
            else if (b->kind == Expr::Const && i.X.RA) a = index(i.X.RA, at);
            return make({Expr::Load, size, add(a, b)});
        }
        case Opcode::lwz: case Opcode::lbz: case Opcode::lhz: {
            int64_t size = op == Opcode::lwz ? 4 : op == Opcode::lhz ? 2 : 1;
            return make({Expr::Load, size, add(ra0(i.D.RA), constant(exts16(i.D.DS)))});
        }
        default: break;
        }
        return reg_expr(32, at); // something else made it
    }

    const Image &image_;
    uint32_t floor_;
    std::vector<std::unique_ptr<Expr>> pool_;
};

// Add(x, y) in either order with one side constant
bool split_const(const Expr *e, int64_t &c, const Expr *&rest) {
    if (e->kind != Expr::Add) return false;
    if (e->a->kind == Expr::Const) return c = e->a->value, rest = e->b, true;
    if (e->b->kind == Expr::Const) return c = e->b->value, rest = e->a, true;
    return false;
}
// the register a table is indexed by, through shifts by `shift` and masks
const Expr *index_of(const Expr *e, uint32_t shift, uint32_t &count) {
    if (shift) {
        if (e->kind != Expr::Shl || e->value != shift) return nullptr;
        e = e->a;
    }
    for (; e->kind == Expr::Mask; e = e->a) {
        if (e->value == 0xFFFFFFFF) continue; // a move
        uint32_t n = uint32_t(std::min<int64_t>(e->value + 1, TABLE_MAX));
        count = count ? std::min(count, n) : n;
    }
    return e->kind == Expr::Reg && e->value < 32 ? e : nullptr;
}

class Discoverer {
public:
    // `extra`: starts from the data pointer scan (Pointer, Branch), registered
    // after .pdata and before anything is walked, so the functions around
    // them stop there
    Discoverer(const Image &image, const std::map<uint32_t, Source> &extra) : image_(image), extra_(extra) {}

    Discovery run() {
        add_imports();
        add_helpers();
        for (const PdataEntry &p : image_.pdata) {
            if (!image_.is_code(p.begin)) continue;
            if (add(p.begin, Source::Pdata, p.length)) d_.counts["pdata"]++;
            if (p.exception) {
                d_.counts["pdata with exception data"]++;
                handler_data_.insert(p.begin - 8), handler_data_.insert(p.begin - 4);
            }
        }
        if (image_.is_code(image_.entry)) add(image_.entry, Source::Entry);
        for (uint32_t a : image_.exports)
            if (add(a, Source::Export)) d_.counts["exports"]++;
        for (const auto &[a, source] : extra_) add(a, source);
        settle();
        // gaps: code nothing covers, cut after each end of a function; a
        // round again when a function shrank and left code uncovered
        for (int round = 0; round < 8; round++) {
            size_t before = d_.functions.size();
            fill_gaps();
            settle();
            if (d_.functions.size() == before) break;
        }
        for (auto &[a, f] : d_.functions) {
            d_.counts[std::string("functions from ") + source_name(f.source)]++;
            if (f.switches_stack) d_.counts["functions that switch the stack"]++;
        }
        d_.counts["functions"] = d_.functions.size();
        d_.counts["jump tables"] = d_.tables.size();
        for (uint32_t bctr : cut_tables_)
            if (d_.tables.count(bctr)) d_.counts["jump tables cut at their function's end"]++;
        d_.counts["indirect branches"] = d_.indirect.size();
        // the branches out of each function that reach no start
        for (const auto &[a, list] : exits_)
            for (const auto &[site, target] : list)
                if (!d_.functions.count(target)) d_.unresolved.push_back({a, site, target});
        d_.counts["unresolved branches"] = d_.unresolved.size();
        d_.stack_thunks = stack_thunks_;
        return std::move(d_);
    }

private:
    bool add(uint32_t address, Source source, uint32_t size = 0, std::string name = {}) {
        if (address & 3 || !image_.is_code(address)) return false;
        auto it = d_.functions.find(address);
        if (it != d_.functions.end()) return false; // the first (more certain) source wins
        Function f;
        f.address = address, f.size = size, f.source = source, f.name = std::move(name);
        d_.functions.emplace(address, std::move(f));
        sized_[address] = size != 0;
        pending_.push_back(address);
        // a function without its own size that held this address ends here now
        auto prev = d_.functions.find(address);
        if (prev != d_.functions.begin()) {
            --prev;
            if (!sized_[prev->first] && prev->second.address + prev->second.size > address)
                pending_.push_back(prev->first);
        }
        return true;
    }

    void add_imports() {
        for (const Import &i : image_.imports) {
            if (add(i.thunk, Source::Import, 16, "__imp__" + i.library + "_" + std::to_string(i.ordinal)))
                d_.counts["imports"]++;
            // the export that moves the caller to another stack (a fiber's)
            const Export *e = find_export(i.library, i.ordinal);
            if (e && std::string_view(e->name) == "KeSetCurrentStackPointers") stack_thunks_.insert(i.thunk);
        }
    }

    // rexglue's detectSaveRestoreHelpers and registerHelpers
    void add_helpers() {
        uint32_t found[8] = {};
        for (const Section &s : image_.sections) {
            if (!s.code) continue;
            for (uint32_t a = s.address; a + 8 <= s.address + s.size; a += 4) {
                uint32_t w = image_.word(a), next = image_.word(a + 4);
                const uint32_t single[] = {0xE9C1FF68, 0xF9C1FF68, 0xC9CCFF70, 0xD9CCFF70};
                for (int k = 0; k < 4; k++)
                    if (w == single[k] && !found[k]) found[k] = a;
                if (w == 0x3960FEE0 && next == 0x7DCB60CE && !found[4]) found[4] = a;
                if (w == 0x3960FEE0 && next == 0x7DCB61CE && !found[5]) found[5] = a;
                if (w == 0x3960FC00 && next == 0x100B60CB && !found[6]) found[6] = a;
                if (w == 0x3960FC00 && next == 0x100B61CB && !found[7]) found[7] = a;
            }
        }
        auto family = [&](uint32_t base14, const char *prefix, uint32_t stride, uint32_t last, uint32_t extra, uint32_t first) {
            if (!base14) return;
            for (uint32_t n = first; n <= last; n++)
                if (add(base14 + (n - first) * stride, Source::Helper, (last + 1 - n) * stride + extra, prefix + std::to_string(n)))
                    d_.counts["helpers"]++;
        };
        family(found[0], "__restgprlr_", 4, 31, 12, 14);
        family(found[1], "__savegprlr_", 4, 31, 8, 14);
        family(found[2], "__restfpr_", 4, 31, 4, 14);
        family(found[3], "__savefpr_", 4, 31, 4, 14);
        family(found[4], "__restvmx_", 8, 31, 4, 14);
        family(found[5], "__savevmx_", 8, 31, 4, 14);
        family(found[6], "__restvmx_", 8, 127, 4, 64);
        family(found[7], "__savevmx_", 8, 127, 4, 64);
    }

    // where a function without its own size must stop: the next start, the section's end
    uint32_t limit(uint32_t address) const {
        const Section *s = image_.section_of(address);
        uint32_t end = s ? s->address + s->size : address + 4;
        auto next = d_.functions.upper_bound(address);
        if (next != d_.functions.end()) end = std::min(end, next->first);
        return end;
    }

    void settle() {
        while (!pending_.empty()) {
            uint32_t address = pending_.front();
            pending_.pop_front();
            auto it = d_.functions.find(address);
            if (it == d_.functions.end()) continue;
            Function &f = it->second;
            if (f.source == Source::Import || f.source == Source::Helper) continue;
            walk(f);
        }
    }

    void walk(Function &f) {
        bool sized = sized_[f.address];
        uint32_t end = sized ? f.address + f.size : limit(f.address);
        f.blocks.clear(), f.calls.clear(), f.tail_calls.clear(), f.call_sites.clear();
        f.switches_stack = false, f.saves_stack = false;
        std::vector<std::pair<uint32_t, uint32_t>> &exits = exits_[f.address];
        exits.clear();
        std::deque<uint32_t> work{f.address};
        std::set<uint32_t> seen;
        uint32_t reached = f.address;
        auto internal = [&](uint32_t t) { return t >= f.address && t < end && (t == f.address || !d_.functions.count(t)); };
        // a branch out of the function: a tail call if a function starts there
        auto leave = [&](uint32_t site, uint32_t target) {
            f.tail_calls.insert(target);
            exits.push_back({site, target});
        };
        while (!work.empty()) {
            uint32_t pc = work.front();
            work.pop_front();
            if (!seen.insert(pc).second) continue;
            f.blocks.insert(pc);
            uint32_t block = pc;
            for (; pc < end; pc += 4) {
                if (pc != block && seen.count(pc)) break; // walked from there already
                Instr i{image_.word(pc)};
                reached = std::max(reached, pc + 4);
                if (i.code == 0) break;
                Opcode op = ppc::decode(i.code);
                if (op == Opcode::Invalid) {
                    d_.counts["blocks ending in an invalid word"]++;
                    break;
                }
                // r1 loaded from a buffer, not popped from the back chain
                // (lwz r1, 0(r1)): another frame's stack (longjmp)
                if (std::optional<uint32_t> w = written_gpr(op, i);
                    w && *w == 1 && ppc::info(op).group == 'm' && i.D.RA != 1)
                    f.switches_stack = true;
                // r1 kept in memory it does not address (setjmp's buffer, a
                // fiber's context); not the frame's back chain (stwu r1, -N(r1))
                if (stores_gpr(op) && i.D.RT == 1 && i.D.RA != 1) f.saves_stack = true;
                if (op == Opcode::bx) {
                    uint32_t target = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
                    // the kernel moves r1 to another stack (a fiber switch)
                    if (stack_thunks_.count(target)) f.switches_stack = true;
                    if (i.I.LK) {
                        f.calls.insert(target);
                        f.call_sites.push_back({pc, target});
                        add(target, Source::Call);
                        continue;
                    }
                    if (internal(target)) work.push_back(target);
                    else leave(pc, target);
                    break;
                }
                if (op == Opcode::bcx) {
                    uint32_t target = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
                    bool always = (i.B.BO & 0x14) == 0x14;
                    if (stack_thunks_.count(target)) f.switches_stack = true;
                    if (i.B.LK) {
                        f.calls.insert(target);
                        f.call_sites.push_back({pc, target});
                        add(target, Source::Call);
                        continue;
                    }
                    if (internal(target)) work.push_back(target);
                    else leave(pc, target);
                    if (always) break;
                    work.push_back(pc + 4);
                    break;
                }
                if (op == Opcode::bclrx) {
                    bool always = (i.XL.BO & 0x14) == 0x14;
                    if (i.XL.LK) {
                        d_.indirect.insert(pc);
                        continue;
                    }
                    if (always) break;
                    continue;
                }
                if (op == Opcode::bcctrx) {
                    bool always = (i.XL.BO & 0x14) == 0x14;
                    if (i.XL.LK || !always) {
                        d_.indirect.insert(pc);
                        continue;
                    }
                    JumpTable table;
                    if (read_table(f.address, pc, end, table)) {
                        for (uint32_t t : table.targets)
                            if (internal(t)) work.push_back(t);
                            else exits.push_back({pc, t});
                        d_.tables[pc] = std::move(table);
                        d_.indirect.erase(pc);
                    } else {
                        d_.indirect.insert(pc);
                    }
                    break;
                }
            }
            // ran into the next function: a tail call to it
            if (pc >= end && d_.functions.count(end)) f.tail_calls.insert(end);
        }
        if (!sized) f.size = std::max<uint32_t>(4, std::min(reached, end) - f.address);
    }

    bool read_table(uint32_t function, uint32_t bctr, uint32_t end, JumpTable &out) {
        // mtctr rS just before
        uint32_t mtctr = 0, source = 0;
        for (uint32_t at = bctr - 4, steps = 0; at >= function && steps < 8; at -= 4, steps++) {
            Instr i{image_.word(at)};
            if (ppc::decode(i.code) == Opcode::mtspr && spr_number(i) == 9) {
                mtctr = at, source = i.XFX.RT;
                break;
            }
        }
        if (!mtctr) return false;
        Tracer tracer(image_, function);
        const Expr *target = tracer.value(source, mtctr);
        JumpTable t;
        t.bctr = bctr;
        uint32_t count = 0;
        const Expr *index = nullptr, *load = nullptr;
        int64_t c;
        const Expr *rest;
        if (target->kind == Expr::Load && target->value == 4) {
            t.kind = JumpTable::Absolute, load = target;
        } else if (split_const(target, c, rest)) {
            t.base = uint32_t(c);
            if (rest->kind == Expr::Load && rest->value == 1) t.kind = JumpTable::ByteOffset, load = rest;
            else if (rest->kind == Expr::Load && rest->value == 2) t.kind = JumpTable::HalfwordOffset, load = rest;
            else if (rest->kind == Expr::Shl && rest->a->kind == Expr::Load && rest->a->value == 1)
                t.kind = JumpTable::ShiftedByteOffset, t.shift = uint32_t(rest->value), load = rest->a;
        }
        if (!load || !split_const(load->a, c, rest)) return false;
        t.table = uint32_t(c);
        uint32_t scale = t.kind == JumpTable::Absolute ? 2 : t.kind == JumpTable::HalfwordOffset ? 1 : 0;
        uint32_t masked = 0; // what a mask on the index allows
        index = index_of(rest, scale, masked);
        if (!index) return false;
        t.index = uint32_t(index->value);
        // The compare on the index is the bound; a mask only caps it (clrlwi
        // to 5 bits, then cmplwi 12: 13 entries, not 32). The compare may be
        // on any register the index passed through on its way to the load
        // (clrlwi r9, r11, 27; cmplwi r9, 17; or mr r31, r4; cmplwi r4, 18),
        // looked for back from the load, nearest the load first.
        std::vector<uint32_t> path; // registers
        uint32_t load_at = 0;
        for (const Expr *e = rest; e; e = e->a) {
            if (!load_at) load_at = e->at; // the first step was used by the load
            if (e->kind == Expr::Reg) {
                path.push_back(uint32_t(e->value));
                break;
            }
            if ((e->kind == Expr::Shl || e->kind == Expr::Mask) && e->reg < 32) path.push_back(e->reg);
        }
        for (uint32_t reg : path)
            if ((count = bound(function, load_at, reg))) break;
        // a compare found elsewhere in the function is a guess: when the
        // table does not hold code that far, it is read as if unbounded
        bool guessed = false;
        for (uint32_t reg : path)
            if (!count) count = bound_anywhere(function, bctr, end, reg), guessed = count != 0;
        // Bounded by the mask alone, the table may be shorter than the mask
        // allows: with no compare (MSVC's `default: __assume(0)`) it ends at
        // the highest case and other data follows it. Its entries go into
        // its own function, so it ends at the first that leaves it; an index
        // past that goes to the switch's dispatch, as the hardware would.
        bool by_mask = masked && (!count || count > masked);
        if (by_mask) count = masked, guessed = false;
        bool cut = false;
        uint32_t entry = 1u << scale;
        auto read = [&](uint32_t count) {
            t.targets.clear();
            for (uint32_t k = 0; k < (count ? count : TABLE_MAX); k++) {
                uint32_t at = t.table + k * entry;
                if (!image_.contains(at, entry)) break;
                uint32_t v = image_.word(at & ~3u);
                uint32_t raw = entry == 4   ? v
                               : entry == 2 ? (v >> (16 - 8 * (at & 2))) & 0xFFFF
                                            : (v >> (24 - 8 * (at & 3))) & 0xFF;
                uint32_t targetaddr = t.kind == JumpTable::Absolute ? raw : t.base + (raw << t.shift);
                const Section *s = image_.section_of(targetaddr);
                bool ok = !(targetaddr & 3) && s && s->code && targetaddr >= function && image_.word(targetaddr) != 0;
                if (ok && by_mask && count && targetaddr >= end) {
                    cut = true;
                    break;
                }
                if (!ok) {
                    if (count) return false; // a bounded table must hold code everywhere
                    break;
                }
                if (!count && targetaddr >= end && !sized_[function]) break;
                t.targets.push_back(targetaddr);
            }
            return !t.targets.empty();
        };
        if (!read(count)) {
            if (!guessed || !read(0)) return false;
            count = 0;
        }
        if (!count) d_.counts["jump tables without a bound"]++;
        if (count && cut) cut_tables_.insert(bctr);
        else cut_tables_.erase(bctr);
        out = std::move(t);
        return true;
    }

    // Where the table is reached by a branch from elsewhere in the function,
    // the bound is on that path: a cmplwi crN, rI, n followed by bgt or
    // ble crN anywhere in the function, the nearest to the bctr.
    uint32_t bound_anywhere(uint32_t function, uint32_t bctr, uint32_t end, uint32_t reg) {
        uint32_t best = 0, best_distance = UINT32_MAX;
        for (uint32_t at = function; at + 8 <= end && at < bctr + 0x1000; at += 4) {
            Instr i{image_.word(at)};
            if (ppc::decode(i.code) != Opcode::cmpli || i.D.RA != reg) continue;
            uint32_t cr = i.D.RT >> 2;
            Instr b{image_.word(at + 4)};
            // bgt crN (BO 12: branch if true) out of the table, or ble crN
            // (BO 4: if false) into it; BI = 4 * crN + 1
            if (ppc::decode(b.code) != Opcode::bcx || (b.B.BO != 12 && b.B.BO != 4) || b.B.BI != 4 * cr + 1) continue;
            uint32_t distance = at < bctr ? bctr - at : at - bctr;
            if (distance < best_distance) best = std::min<uint32_t>(i.D.DS + 1, TABLE_MAX), best_distance = distance;
        }
        return best;
    }

    // cmplwi crN, rI, n (n + 1 entries) back from where the table is read,
    // until rI is written
    uint32_t bound(uint32_t function, uint32_t from, uint32_t reg) {
        for (uint32_t at = from - 4, steps = 0; at >= function && at < from && steps < BACKWARD_MAX; at -= 4, steps++) {
            Instr i{image_.word(at)};
            Opcode op = ppc::decode(i.code);
            if ((op == Opcode::cmpli || op == Opcode::cmpi) && i.D.RA == reg) return std::min<uint32_t>(i.D.DS + 1, TABLE_MAX);
            std::optional<uint32_t> w = written_gpr(op, i);
            if (w && *w == reg) {
                if (op == Opcode::lwz && i.D.RA == 1) return bound_in_slot(function, at, i.D.DS);
                break;
            }
        }
        return 0;
    }

    // Unoptimized code reloads the index from its stack slot for each use:
    // the compare is on another register loaded from the same slot before
    // (lwz r8, 0x70(r1); cmplwi r8, 9; bgt; lwz r7, 0x70(r1); slwi r0, r7, 2).
    uint32_t bound_in_slot(uint32_t function, uint32_t from, uint32_t slot) {
        for (uint32_t at = from - 4, steps = 0; at >= function && at < from && steps < 16; at -= 4, steps++) {
            Instr i{image_.word(at)};
            Opcode op = ppc::decode(i.code);
            std::string_view name = ppc::info(op).name;
            if (name.starts_with("st") && ppc::info(op).form == ppc::Form::D && i.D.RA == 1 && i.D.DS == slot) return 0;
            if (op != Opcode::cmpli && op != Opcode::cmpi) continue;
            // the register compared, loaded from the slot just before
            for (uint32_t b = at - 4, back = 0; b >= function && back < 3; b -= 4, back++) {
                Instr l{image_.word(b)};
                Opcode lop = ppc::decode(l.code);
                if (lop == Opcode::lwz && l.D.RT == i.D.RA && l.D.RA == 1 && l.D.DS == slot)
                    return std::min<uint32_t>(i.D.DS + 1, TABLE_MAX);
                std::optional<uint32_t> w = written_gpr(lop, l);
                if (w && *w == i.D.RA) break;
            }
        }
        return 0;
    }

    // Code no function covers gets a function after each end (blr, bctr, b)
    // and after padding, as rexglue splits its gaps on returns and tail
    // calls. Each one is walked as soon as it is added, so the scan goes on
    // past what it covers: a run of small functions reached only through
    // pointers (a vtable's leaf methods) is cut in one pass.
    void fill_gaps() {
        // Covered: inside the nearest function before, or inside a sized one
        // that started earlier and holds it (a function nested in a .pdata
        // range does not uncover the rest of the range: the sized function's
        // code runs across it).
        uint32_t sized_until = 0;
        auto is_covered = [&](uint32_t a) {
            auto here = d_.functions.find(a);
            if (here != d_.functions.end() && sized_[a]) sized_until = std::max(sized_until, a + here->second.size);
            if (a < sized_until) return true;
            auto it = d_.functions.upper_bound(a);
            if (it == d_.functions.begin()) return false;
            --it;
            return a < it->first + std::max<uint32_t>(it->second.size, 4);
        };
        auto ends = [](uint32_t w) {
            Instr i{w};
            return w == BLR || w == BCTR || (ppc::decode(w) == Opcode::bx && !i.I.LK);
        };
        // where a function .pdata (or a helper's or a thunk's size) bounds
        // ends: an end whatever its last instruction, which may be a call
        // that never returns (bl _CxxThrowException), so the code after it
        // is another function's (Sonic's adjustor thunks there are reached
        // only through vtables)
        std::set<uint32_t> sized_ends;
        for (const auto &[a, f] : d_.functions)
            if (sized_[a]) sized_ends.insert(a + f.size);
        for (const Section &s : image_.sections) {
            if (!s.code) continue;
            bool after_end = true; // a gap starts after an end, or at the section's start
            sized_until = 0;
            for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) {
                uint32_t w = image_.word(a);
                if (is_covered(a)) {
                    after_end = ends(w) || sized_ends.count(a + 4);
                    continue;
                }
                if (w == 0) {
                    after_end = true;
                    continue;
                }
                // A blr alone after an end is the compiler's dead return
                // after a tail call; a function only if something reaches it
                // (a bl, a pointer, a branch). The 8 bytes before a function
                // with exception data are its handler and the handler's data.
                if ((w == BLR && after_end) || handler_data_.count(a)) {
                    after_end = true;
                    continue;
                }
                if (after_end && ppc::decode(w) != Opcode::Invalid) {
                    if (add(a, Source::Gap)) {
                        d_.counts["gaps"]++;
                        settle();
                        if (is_covered(a)) {
                            after_end = ends(w);
                            continue;
                        }
                    }
                }
                after_end = ends(w);
            }
        }
    }

    const Image &image_;
    const std::map<uint32_t, Source> &extra_;
    Discovery d_;
    std::map<uint32_t, bool> sized_;
    std::deque<uint32_t> pending_;
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> exits_; // by function: (site, target) leaving it
    std::set<uint32_t> handler_data_; // the words before functions with exception data
    std::set<uint32_t> stack_thunks_; // KeSetCurrentStackPointers' import thunks
    std::set<uint32_t> cut_tables_;   // the bctrs of tables cut at their function's end, as last read
};

// ---- the data pointer scan ----
//
// After rexglue's codegen/pointer_scan.cpp (rexglue-sdk, the fork's
// [analysis] scan_data_pointers). Copyright (c) 2026, Tom Clay; portions
// Copyright (c) 2022, Ben Vanik and Xenia project contributors. BSD 3-Clause:
// redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the conditions in LICENSE-rexglue
// are met; it is provided "as is", without any warranty (LICENSE-rexglue).
//
// Translated: what counts as a pointer (the words of every section that is not
// code, lis/addi pairs in code), what looks like a function's start, and the
// passes that drop a candidate when it splits a function. Written anew for
// XenoForge's structures: a split shows as a branch out of a function to an
// address no function starts at (Discovery::unresolved, what the generated
// code would dispatch to the interpreter), where rexglue's builders would
// trap; the passes rerun the whole discovery, which takes a fraction of a
// second, so there is no cache of dropped candidates; a bl never shows a
// split here, because every bl target is a start.

constexpr uint32_t MFLR_R12 = 0x7D8802A6;
constexpr uint32_t LIS_ADDI_WINDOW = 8; // how far past a lis the addi that completes it may sit
// Passes after the baseline. Each only drops candidates or adds branch
// targets, so the set settles quickly; this is a backstop.
constexpr int SCAN_PASSES = 16;

bool is_code_address(const Image &image, uint32_t a) { return !(a & 3) && image.is_code(a) && image.contains(a); }

// A function's first instruction opens a frame (mflr r12, stwu r1), or
// follows a return, an unconditional branch, padding or the end .pdata gives
// the function before (which may end in a call that never returns).
bool looks_like_start(const Image &image, const std::set<uint32_t> &pdata_ends, uint32_t a) {
    if (!is_code_address(image, a)) return false;
    const Section *s = image.section_of(a);
    uint32_t w = image.word(a);
    if (w == 0) return false;
    if (w == MFLR_R12 || (w & 0xFFFF0000) == 0x94210000) return true;
    if (a < s->address + 4) return false; // the word before is in another section
    uint32_t prev = image.word(a - 4);
    bool b = (prev >> 26) == 18 && !(prev & 1);
    return prev == BLR || prev == BCTR || b || prev == 0 || pdata_ends.count(a);
}

// the function whose range holds `a`
const Function *holder(const Discovery &d, uint32_t a) {
    auto it = d.functions.upper_bound(a);
    if (it == d.functions.begin()) return nullptr;
    --it;
    return a < it->first + std::max<uint32_t>(it->second.size, 4) ? &it->second : nullptr;
}

// Every lis rX, hi followed by addi rY, rX, lo in code: the address it makes,
// and where the addi is.
template <class F> void lis_addi_pairs(const Image &image, F &&each) {
    for (const Section &s : image.sections) {
        if (!s.code) continue;
        uint32_t end = s.address + s.size;
        for (uint32_t a = s.address; a + 4 <= end; a += 4) {
            uint32_t lis = image.word(a);
            if ((lis >> 26) != 15 || ((lis >> 16) & 31) != 0) continue; // addis rX, 0, hi
            uint32_t reg = (lis >> 21) & 31, hi = lis << 16;
            for (uint32_t k = 1; k <= LIS_ADDI_WINDOW && a + (k + 1) * 4 <= end; k++) {
                uint32_t w = image.word(a + k * 4);
                if ((w >> 26) == 14 && ((w >> 16) & 31) == reg) { // addi rY, rX, lo
                    each(hi + uint32_t(int32_t(int16_t(w & 0xFFFF))), a + k * 4);
                    break;
                }
                if (((w >> 21) & 31) == reg) break; // the register written before an addi used it
            }
        }
    }
}

// Where a stack switch may return into a function's frame from elsewhere
// (Function::resumes): after a call to a function that saves the stack
// pointer and returns (setjmp, itself or behind tail branches; not a
// function that switches the stack itself, whose callers go on through the
// switch back), and at the catch continuations of a function with exception
// data. A catch block is a funclet that returns its continuation, an address
// inside the function it belongs to, made with lis/addi (lis r3, cont@ha;
// addi r3, r3, cont@l; blr): any such address past a function's start is
// taken. A guess too many costs that function a try block; one missed sends
// that return to the interpreter.
void find_resumes(const Image &image, Discovery &d) {
    // savers through tail branches, until nothing changes
    for (bool changed = true; changed;) {
        changed = false;
        for (auto &[a, f] : d.functions) {
            if (f.saves_stack || f.switches_stack) continue;
            for (uint32_t t : f.tail_calls) {
                auto it = d.functions.find(t);
                if (it != d.functions.end() && it->second.saves_stack && !it->second.switches_stack) {
                    f.saves_stack = changed = true;
                    break;
                }
            }
        }
    }
    uint64_t after_saves = 0, continuations = 0;
    for (auto &[a, f] : d.functions) {
        f.resumes.clear();
        if (f.source == Source::Import || f.source == Source::Helper) continue;
        for (const auto &[site, target] : f.call_sites) {
            auto it = d.functions.find(target);
            if (it != d.functions.end() && it->second.saves_stack && !it->second.switches_stack &&
                f.resumes.insert(site + 4).second)
                after_saves++;
        }
    }
    std::set<uint32_t> handlers; // functions with exception data
    for (const PdataEntry &p : image.pdata)
        if (p.exception) handlers.insert(p.begin);
    std::map<uint32_t, uint32_t> tables; // inline jump tables: start, end
    for (const auto &[bctr, t] : d.tables) {
        uint32_t entry = t.kind == JumpTable::Absolute ? 4 : t.kind == JumpTable::HalfwordOffset ? 2 : 1;
        uint32_t &end = tables[t.table];
        end = std::max<uint32_t>(end, t.table + std::max<uint32_t>(entry * uint32_t(t.targets.size()), 4));
    }
    lis_addi_pairs(image, [&](uint32_t v, uint32_t) {
        if (v & 3) return;
        auto it = d.functions.upper_bound(v);
        // the function with exception data whose range holds it, past its
        // start (another function may start there: the pointer scan takes
        // some continuations for starts)
        while (it != d.functions.begin()) {
            --it;
            if (handlers.count(it->first)) break;
            if (it->second.source == Source::Pdata) return;
        }
        if (it == d.functions.end() || !handlers.count(it->first)) return;
        Function &f = it->second;
        if (v <= f.address || v >= f.address + f.size) return;
        auto table = tables.upper_bound(v);
        if (table != tables.begin() && v < std::prev(table)->second) return;
        if (ppc::decode(image.word(v)) == Opcode::Invalid || image.word(v) == 0) return;
        if (f.resumes.insert(v).second) continuations++;
    });
    uint64_t holders = 0;
    for (const auto &[a, f] : d.functions) holders += !f.resumes.empty();
    d.counts["resume sites: after a call that saves the stack"] = after_saves;
    d.counts["resume sites: catch continuations"] = continuations;
    d.counts["functions with resume sites"] = holders;
}

} // namespace

std::vector<uint32_t> scan_pointers(const Image &image) {
    std::set<uint32_t> found, pdata_ends;
    for (const PdataEntry &p : image.pdata) pdata_ends.insert(p.begin + p.length);
    auto consider = [&](uint32_t v) {
        if (looks_like_start(image, pdata_ends, v)) found.insert(v);
    };
    // vtables, callback tables, fields holding function pointers
    for (const Section &s : image.sections)
        if (!s.code)
            for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) consider(image.word(a));
    // Addresses made in code: lis rX, hi followed by addi rY, rX, lo. A word
    // in a code section is not read as a pointer: there it is an inline jump
    // table.
    lis_addi_pairs(image, [&](uint32_t v, uint32_t) { consider(v); });
    return {found.begin(), found.end()};
}

// Discovery with the data pointer scan: a baseline run says which pointers the
// image explains already; the rest become starts, rerun after rerun, each one
// dropped when it splits a function (it leaves the function's own branches
// across the split pointing at no start), until the set holds still.
Discovery discover(const Image &image, const DiscoverOptions &options) {
    const std::map<uint32_t, Source> none;
    Discovery d = Discoverer(image, none).run();
    if (!options.scan_pointers) {
        find_resumes(image, d);
        return d;
    }

    // branches unresolved without the scan say nothing about the candidates:
    // pruning around them would only drop good starts
    std::set<std::pair<uint32_t, uint32_t>> baseline;
    for (const Unresolved &u : d.unresolved) baseline.insert({u.site, u.target});
    const uint64_t baseline_count = d.unresolved.size();

    // A jump table sits in the code after its bctr, so the lis/addi pair that
    // loads its address looks like a pointer to a start: what discovery read
    // as a table is not one (rexglue's scan has no such check).
    std::map<uint32_t, uint32_t> tables; // start, end
    for (const auto &[bctr, t] : d.tables) {
        uint32_t entry = t.kind == JumpTable::Absolute ? 4 : t.kind == JumpTable::HalfwordOffset ? 2 : 1;
        uint32_t &end = tables[t.table];
        end = std::max<uint32_t>(end, t.table + std::max<uint32_t>(entry * uint32_t(t.targets.size()), 4));
    }
    auto in_table = [&](uint32_t a) {
        auto it = tables.upper_bound(a);
        return it != tables.begin() && a < std::prev(it)->second;
    };
    // Nor is an address inside the prologue .pdata gives a function, whatever
    // word happens to hold it: the stwu r1 there looks like a start (rexglue
    // has no such check). Past the prologue, inside the function, pointers
    // do find entries: the funclets an exception scope table names.
    std::map<uint32_t, uint32_t> prologs; // begin, end
    for (const PdataEntry &p : image.pdata)
        prologs[p.begin] = std::max(prologs[p.begin], p.begin + std::min(p.prolog * 4, p.length));
    auto in_prolog = [&](uint32_t a) {
        auto it = prologs.upper_bound(a);
        return it != prologs.begin() && a > std::prev(it)->first && a < std::prev(it)->second;
    };

    std::map<uint32_t, Source> candidates;
    std::set<uint32_t> rejected;
    const std::vector<uint32_t> found = scan_pointers(image);
    for (uint32_t a : found) {
        if (d.functions.count(a) || in_table(a) || in_prolog(a)) continue;
        // a branch target inside a function (a jump table's case) is not an entry
        const Function *owner = holder(d, a);
        if (owner && owner->blocks.count(a)) continue;
        candidates[a] = Source::Pointer;
    }
    const uint64_t from_scan = candidates.size();

    uint64_t branches = 0;
    int pass = 0;
    while (true) {
        pass++;
        d = Discoverer(image, candidates).run();
        if (pass == SCAN_PASSES) break;
        bool changed = false;

        // A candidate that splits a function leaves the function's own
        // branches across the split unresolved. The culprit is the candidate
        // that cut the path: the first after the site going forward, the last
        // at or before it going backward; when there is none, every candidate
        // in the span. Collected first, so one pass judges every branch
        // against the same discovery.
        std::set<uint32_t> drop;
        auto drop_span = [&](uint32_t lo, uint32_t hi) {
            for (auto it = candidates.upper_bound(lo); it != candidates.end() && it->first <= hi; ++it)
                drop.insert(it->first);
        };
        for (const Unresolved &u : d.unresolved) {
            if (baseline.count({u.site, u.target})) continue;
            uint32_t lo = std::min({u.site, u.target, u.function}), hi = std::max(u.site, u.target);
            auto it = candidates.upper_bound(u.site);
            if (u.target > u.site) {
                if (it != candidates.end() && it->first <= u.target) {
                    drop.insert(it->first);
                    continue;
                }
            } else if (it != candidates.begin() && std::prev(it)->first > u.target) {
                drop.insert(std::prev(it)->first);
                continue;
            }
            drop_span(lo, hi);
        }
        for (uint32_t a : drop) {
            candidates.erase(a);
            rejected.insert(a);
            changed = true;
        }

        // Branch targets no function holds (code the discovery left
        // orphaned), made starts too, under the same pruning; and those held
        // by a function whose end was only walked to, when the branch reaches
        // no start without the scan either (a candidate's split is the
        // candidate's fault): a routine after a call that never returns (the
        // walk went on past the bl), or a tail two functions share. The
        // function holding it ends there now, running on into it as a tail
        // call. A function .pdata sizes stays whole.
        for (const Unresolved &u : d.unresolved) {
            uint32_t t = u.target;
            if (rejected.count(t) || candidates.count(t) || d.functions.count(t)) continue;
            if (!is_code_address(image, t)) continue;
            if (const Function *h = holder(d, t);
                h && (h->source == Source::Pdata || h->source == Source::Helper || h->source == Source::Import ||
                      !baseline.count({u.site, u.target})))
                continue;
            candidates[t] = Source::Branch;
            branches++;
            changed = true;
        }
        if (!changed) break;
    }

    uint64_t kept = 0;
    for (const auto &[a, source] : candidates) kept += source == Source::Pointer;
    d.counts["pointer scan: code pointers"] = found.size();
    d.counts["pointer scan: not explained without it"] = from_scan;
    d.counts["pointer scan: kept"] = kept;
    d.counts["pointer scan: dropped as splits"] = rejected.size();
    d.counts["pointer scan: branch targets added"] = branches;
    d.counts["pointer scan: passes"] = uint64_t(pass);
    d.counts["unresolved branches without the scan"] = baseline_count;
    find_resumes(image, d);
    return d;
}

} // namespace xf::recomp
