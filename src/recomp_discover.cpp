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

#include "xenoforge/recomp.h"

#include "xenoforge/ppc.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>

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

// ---- following a value backwards ----

struct Expr {
    enum Kind { Const, Reg, Load, Add, Shl, Mask } kind = Reg;
    int64_t value = 0;     // Const's value; Reg's register; Load's size; Shl's amount; Mask's mask
    const Expr *a = nullptr, *b = nullptr;
    uint32_t at = 0;       // Reg: where the scan stopped looking for it
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

    // an index register, through slwi and clrlwi; anything else (a move
    // too: the bound is on the register the table is read with) ends it
    // (as a register used at `pc`)
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
                uint32_t sh = i.M.SH, mb = i.M.MB, me = i.M.ME;
                if (mb == 0 && me == 31 - sh) return make({Expr::Shl, int64_t(sh), index(i.M.RT, at, depth + 1)});
                if (sh == 0 && me == 31) return make({Expr::Mask, int64_t(0xFFFFFFFFull >> mb), index(i.M.RT, at, depth + 1)});
            }
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
    if (e->kind == Expr::Mask) {
        count = uint32_t(std::min<int64_t>(e->value + 1, TABLE_MAX));
        e = e->a;
    }
    return e->kind == Expr::Reg && e->value < 32 ? e : nullptr;
}

class Discoverer {
public:
    explicit Discoverer(const Image &image) : image_(image) {}

    Discovery run() {
        add_imports();
        add_helpers();
        for (const PdataEntry &p : image_.pdata) {
            if (!image_.is_code(p.begin)) continue;
            if (add(p.begin, Source::Pdata, p.length)) d_.counts["pdata"]++;
            if (p.exception) d_.counts["pdata with exception data"]++;
        }
        if (image_.is_code(image_.entry)) add(image_.entry, Source::Entry);
        settle();
        // gaps: code nothing covers, cut after each end of a function
        for (int round = 0; round < 8; round++) {
            size_t before = d_.functions.size();
            fill_gaps();
            settle();
            if (d_.functions.size() == before) break;
        }
        for (auto &[a, f] : d_.functions) d_.counts[std::string("functions from ") + source_name(f.source)]++;
        d_.counts["functions"] = d_.functions.size();
        d_.counts["jump tables"] = d_.tables.size();
        d_.counts["indirect branches"] = d_.indirect.size();
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
        for (const Import &i : image_.imports)
            if (add(i.thunk, Source::Import, 16, "__imp__" + i.library + "_" + std::to_string(i.ordinal)))
                d_.counts["imports"]++;
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
        f.blocks.clear(), f.calls.clear(), f.tail_calls.clear();
        std::deque<uint32_t> work{f.address};
        std::set<uint32_t> seen;
        uint32_t reached = f.address;
        auto internal = [&](uint32_t t) { return t >= f.address && t < end && (t == f.address || !d_.functions.count(t)); };
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
                if (op == Opcode::bx) {
                    uint32_t target = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
                    if (i.I.LK) {
                        f.calls.insert(target);
                        add(target, Source::Call);
                        continue;
                    }
                    if (internal(target)) work.push_back(target);
                    else f.tail_calls.insert(target);
                    break;
                }
                if (op == Opcode::bcx) {
                    uint32_t target = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
                    bool always = (i.B.BO & 0x14) == 0x14;
                    if (i.B.LK) {
                        f.calls.insert(target);
                        add(target, Source::Call);
                        continue;
                    }
                    if (internal(target)) work.push_back(target);
                    else f.tail_calls.insert(target);
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
        index = index_of(rest, scale, count);
        if (!index) return false;
        t.index = uint32_t(index->value);
        if (!count) count = bound(function, index->at, t.index);
        if (!count) count = bound_anywhere(function, bctr, end, t.index);
        uint32_t entry = 1u << scale;
        for (uint32_t k = 0; k < (count ? count : TABLE_MAX); k++) {
            uint32_t at = t.table + k * entry;
            if (!image_.contains(at, entry)) break;
            uint32_t v = image_.word(at & ~3u);
            uint32_t raw = entry == 4 ? v : entry == 2 ? (v >> (16 - 8 * (at & 2))) & 0xFFFF : (v >> (24 - 8 * (at & 3))) & 0xFF;
            uint32_t targetaddr = t.kind == JumpTable::Absolute ? raw : t.base + (raw << t.shift);
            const Section *s = image_.section_of(targetaddr);
            bool ok = !(targetaddr & 3) && s && s->code && targetaddr >= function && image_.word(targetaddr) != 0;
            if (!ok) {
                if (count) return false; // a bounded table must hold code everywhere
                break;
            }
            if (!count && targetaddr >= end && !sized_[function]) break;
            t.targets.push_back(targetaddr);
        }
        if (t.targets.empty()) return false;
        if (!count) d_.counts["jump tables without a bound"]++;
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

    // cmplwi crN, rI, n (n + 1 entries) between where the index is used and
    // where it was made
    uint32_t bound(uint32_t function, uint32_t from, uint32_t reg) {
        for (uint32_t at = from - 4, steps = 0; at >= function && at < from && steps < BACKWARD_MAX; at -= 4, steps++) {
            Instr i{image_.word(at)};
            Opcode op = ppc::decode(i.code);
            if ((op == Opcode::cmpli || op == Opcode::cmpi) && i.D.RA == reg) return std::min<uint32_t>(i.D.DS + 1, TABLE_MAX);
            std::optional<uint32_t> w = written_gpr(op, i);
            if (w && *w == reg) break;
        }
        return 0;
    }

    void fill_gaps() {
        // what functions cover
        std::vector<std::pair<uint32_t, uint32_t>> covered;
        for (auto &[a, f] : d_.functions) covered.push_back({a, a + std::max<uint32_t>(f.size, 4)});
        std::sort(covered.begin(), covered.end());
        auto is_covered = [&](uint32_t a) {
            auto it = std::upper_bound(covered.begin(), covered.end(), std::make_pair(a, UINT32_MAX));
            return it != covered.begin() && a < std::prev(it)->second;
        };
        for (const Section &s : image_.sections) {
            if (!s.code) continue;
            bool after_end = true; // a gap starts after an end, or at the section's start
            for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) {
                uint32_t w = image_.word(a);
                if (is_covered(a)) {
                    after_end = false;
                    Instr i{w};
                    Opcode op = ppc::decode(w);
                    after_end = w == BLR || w == BCTR || (op == Opcode::bx && !i.I.LK);
                    continue;
                }
                if (w == 0) {
                    after_end = true;
                    continue;
                }
                if (after_end && ppc::decode(w) != Opcode::Invalid) {
                    if (add(a, Source::Gap)) d_.counts["gaps"]++;
                    after_end = false;
                }
            }
        }
    }

    const Image &image_;
    Discovery d_;
    std::map<uint32_t, bool> sized_;
    std::deque<uint32_t> pending_;
};

} // namespace

Discovery discover(const Image &image) { return Discoverer(image).run(); }

} // namespace xf::recomp
