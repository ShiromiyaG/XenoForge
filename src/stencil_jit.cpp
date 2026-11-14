// The copy-and-patch tier (stencil.h; docs/recompiler.md, "A copy-and-patch
// first tier"): each function discovery found becomes a run of stencils
// (src/stencil/stencils.cpp, extracted by tools/xfstencil.cpp into
// stencils.inc), copied into executable memory with their holes written.
// The control flow is recomp_emit.cpp's, stencil for line: the same walk of a
// function's instructions, the same yield points, calls to the functions
// given code here and dispatch for the rest, so a run is bit for bit the
// interpreter's and the game library's.
//
// The memory is one reservation (well under 2 GB, so every pc-relative field
// reaches across it): the runtime's table, stubs to the C library's
// functions, the helpers and constants the stencils share and their unwind
// information first, then each function: its C++ entry (xf_st_entry), its
// body, the branches' out-of-line sequences, its jump tables' data. Written,
// then made read-only and executable; the instruction counter
// (Options::count) sits on a page of its own that stays writable.

#include "xenoforge/stencil.h"

#include "stencil_format.h"
#include "xenoforge/cpu_level.h"
#include "xenoforge/ppc.h"
#include "xenoforge/recomp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#if XF_STENCIL_TIER
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

#if XF_STENCIL_TIER
namespace xf::stencil::gen {
using ::xf::stencil::Piece;
using ::xf::stencil::Reloc;
using ::xf::stencil::RelocKind;
using ::xf::stencil::TargetKind;
} // namespace xf::stencil::gen
#include "stencils.inc"
#endif

namespace xf::stencil {

bool requested() {
    const char *tier = std::getenv("XF_TIER");
    return tier && std::string_view(tier) == "stencil";
}

#if !XF_STENCIL_TIER

bool available(std::string *why) {
    if (why) *why = "this build has no stencils (clang on x86-64 makes them)";
    return false;
}
std::unique_ptr<Module> compile(const recomp::Image &, const recomp::Discovery &, const ppc::Runtime &,
                                const std::string &, uint64_t, const Options &, Stats &, std::string &error) {
    available(&error);
    return nullptr;
}

#else

namespace {

using ppc::Instr;
using ppc::Opcode;
using recomp::Discovery;
using recomp::Function;
using recomp::Image;
using recomp::Source;

int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }

// ---- the stencils by name ----

struct Ids {
    uint16_t op[ppc::OPCODE_COUNT];
    uint16_t entry, ret, go, count, yield, set_lr, set_pc, call, call_dispatch, tail_dispatch, tail_dispatch_ctr,
        invalid, table;
    uint16_t bc[32], bcctr[32], bcl[32], bclr[32], bclrl[32], bcctrl[32];
    bool ok = false;
    std::string missing;
};

const Ids &ids() {
    static const Ids table = [] {
        Ids t{};
        std::unordered_map<std::string_view, uint16_t> by_name;
        for (size_t k = 0; k < gen::piece_count; k++)
            if (gen::pieces[k].flags & PIECE_STENCIL) by_name[gen::pieces[k].name] = uint16_t(k);
        auto find = [&](const std::string &name) -> uint16_t {
            auto it = by_name.find(name);
            if (it != by_name.end()) return it->second;
            if (t.missing.empty()) t.missing = name;
            return 0;
        };
        for (uint32_t k = 0; k < ppc::OPCODE_COUNT; k++)
            t.op[k] = find(std::string("xf_st_op_") + ppc::info(Opcode(k)).name);
        t.entry = find("xf_st_entry"), t.ret = find("xf_st_ret"), t.go = find("xf_st_goto");
        t.count = find("xf_st_count"), t.yield = find("xf_st_yield"), t.set_lr = find("xf_st_set_lr");
        t.set_pc = find("xf_st_set_pc"), t.call = find("xf_st_call"), t.call_dispatch = find("xf_st_call_dispatch");
        t.tail_dispatch = find("xf_st_tail_dispatch"), t.tail_dispatch_ctr = find("xf_st_tail_dispatch_ctr");
        t.invalid = find("xf_st_invalid"), t.table = find("xf_st_table");
        for (int bo = 0; bo < 32; bo++) {
            const std::string n = std::to_string(bo);
            t.bc[bo] = find("xf_st_bc_" + n), t.bcctr[bo] = find("xf_st_bcctr_" + n);
            t.bcl[bo] = find("xf_st_bcl_" + n), t.bclr[bo] = find("xf_st_bclr_" + n);
            t.bclrl[bo] = find("xf_st_bclrl_" + n), t.bcctrl[bo] = find("xf_st_bcctrl_" + n);
        }
        t.ok = t.missing.empty();
        return t;
    }();
    return table;
}

// ---- executable memory ----

size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return size_t(sysconf(_SC_PAGESIZE));
#endif
}

// A reservation committed as it is written.
class Region {
  public:
    bool reserve(size_t size) {
        page_ = page_size();
        size_ = size;
#if defined(_WIN32)
        base_ = static_cast<uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
#else
        void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
#endif
        return base_ != nullptr;
    }
    ~Region() {
        if (!base_) return;
#if defined(_WIN32)
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, size_);
#endif
    }
    uint8_t *base() const { return base_; }
    size_t used() const { return used_; }
    size_t committed() const { return committed_; }
    // n bytes at the given alignment, committed (nullptr past the reservation)
    uint8_t *take(size_t n, size_t align) {
        size_t at = (used_ + align - 1) & ~(align - 1);
        if (at + n > size_) return nullptr;
        if (at + n > committed_) {
            const size_t grow = std::max<size_t>((at + n - committed_ + page_ - 1) & ~(page_ - 1), 16u << 20);
            const size_t to = std::min(size_, committed_ + grow);
#if defined(_WIN32)
            if (!VirtualAlloc(base_ + committed_, to - committed_, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
#else
            if (mprotect(base_ + committed_, to - committed_, PROT_READ | PROT_WRITE) != 0) return nullptr;
#endif
            committed_ = to;
        }
        if (at > used_) std::memset(base_ + used_, 0xCC, at - used_); // int3 between pieces
        used_ = at + n;
        return base_ + at;
    }
    // [0, end) read and executed from now on; [end, used) stays writable
    bool seal(size_t end) {
        end = (end + page_ - 1) & ~(page_ - 1);
#if defined(_WIN32)
        DWORD old;
        if (!VirtualProtect(base_, end, PAGE_EXECUTE_READ, &old)) return false;
        FlushInstructionCache(GetCurrentProcess(), base_, end);
#else
        if (mprotect(base_, end, PROT_READ | PROT_EXEC) != 0) return false;
        __builtin___clear_cache(reinterpret_cast<char *>(base_), reinterpret_cast<char *>(base_ + end));
#endif
        return true;
    }
    size_t page() const { return page_; }

  private:
    uint8_t *base_ = nullptr;
    size_t size_ = 0, used_ = 0, committed_ = 0, page_ = 4096;
};

// ---- a function as stencils ----

// Where a hole Target or Call goes.
struct Ref {
    enum Kind : uint8_t { None, Main, Ool, Pc, Function } kind = None;
    uint32_t value = 0; // an index in the main or out-of-line sequence, a guest address, a function's index
};

struct Inst {
    Inst(uint16_t p = 0, uint32_t w = 0, uint32_t a_ = 0, uint32_t b_ = 0, Ref t = {})
        : piece(p), word(w), a(a_), b(b_), target(t) {}
    uint16_t piece;
    uint32_t word, a, b;
    Ref target, call;
    int32_t table = -1;   // its jump table (xf_st_table), in the function's
    bool counter = false; // xf_st_count: data is the counter
};

struct TableData {
    uint32_t low = 0, span = 0;
    std::vector<Ref> slots; // a word each (None: dispatch)
};

class Compiler;

class StencilModule final : public Module {
  public:
    const ppc::GeneratedModule *table() const override { return &module_; }
    uint64_t executed() const override {
        uint64_t n = 0;
        for (uint32_t k = 0; counter_ && k < ppc::OPCODE_COUNT; k++) n += counter_[k];
        return n;
    }
    const uint64_t *executed_by_opcode() const override { return counter_; }
    ~StencilModule() override {
#if defined(_WIN32)
        if (!unwind_.empty()) RtlDeleteFunctionTable(unwind_.data());
#endif
    }

  private:
    friend class Compiler;
    Region region_;
    std::string name_;
    std::vector<ppc::GeneratedFunction> functions_;
    ppc::GeneratedModule module_{};
    uint64_t *counter_ = nullptr;
#if defined(_WIN32)
    std::vector<RUNTIME_FUNCTION> unwind_;
#endif
};

class Compiler {
  public:
    Compiler(const Image &image, const Discovery &d, const Options &options, Stats &stats, StencilModule &m)
        : image_(image), d_(d), options_(options), stats_(stats), m_(m), ids_(ids()) {}

    bool run(const ppc::Runtime &runtime, std::string &error) {
        plan();
        // a gigabyte: games' code makes a few hundred megabytes at most (about
        // forty bytes an instruction), and every pc-relative field reaches across it
        if (!m_.region_.reserve(size_t(1) << 30)) return error = "could not reserve the code's memory", false;
        if (!header(runtime)) return error = "out of the code's memory", false;
        for (size_t k = 0; k < order_.size(); k++)
            if (!function(k)) return error = "out of the code's memory", false;
        for (const Pending &p : pending_) {
            const uint64_t target = bodies_[p.function];
            if (p.abs) {
                const uint64_t v = target;
                std::memcpy(p.field, &v, 8);
            } else if (!rel32(p.field, int64_t(target) + p.addend - int64_t(uintptr_t(p.field)))) {
                return error = "a call out of the code's reach", false;
            }
        }
        const size_t code_end = m_.region_.used();
        if (options_.count) {
            // the counter on a page of its own, left writable
            uint8_t *page = m_.region_.take(m_.region_.page(), m_.region_.page());
            if (!page) return error = "out of the code's memory", false;
            std::memset(page, 0, m_.region_.page());
            m_.counter_ = reinterpret_cast<uint64_t *>(page);
            // the count stencils' data holes, written now that it exists
            for (size_t k = 0; k < counter_fields_.size(); k++)
                rel32(counter_fields_[k], int64_t(uintptr_t(page)) + 8 * int64_t(counter_index_[k]) +
                                              counter_addends_[k] - int64_t(uintptr_t(counter_fields_[k])));
        }
        if (!m_.region_.seal(code_end)) return error = "could not make the code executable", false;
#if defined(_WIN32)
        if (!m_.unwind_.empty() &&
            !RtlAddFunctionTable(m_.unwind_.data(), DWORD(m_.unwind_.size()), DWORD64(uintptr_t(m_.region_.base()))))
            return error = "could not register the code's unwind information", false;
        stats_.unwind_entries = m_.unwind_.size();
#endif
        stats_.region_bytes = m_.region_.used();
        return true;
    }

    // the module's table (functions in address order, as ppc_module.h has them)
    void finish(const std::string &name, uint64_t digest) {
        m_.name_ = name;
        std::sort(
            m_.functions_.begin(), m_.functions_.end(),
            [](const ppc::GeneratedFunction &a, const ppc::GeneratedFunction &b) { return a.address < b.address; });
        m_.module_.name = m_.name_.c_str();
        m_.module_.base = image_.base;
        m_.module_.size = uint32_t(image_.bytes.size());
        m_.module_.digest = digest;
        m_.module_.functions = m_.functions_.data();
        m_.module_.count = m_.functions_.size();
    }

  private:
    struct Pending {
        uint8_t *field;
        uint32_t function;
        int64_t addend;
        bool abs;
    };

    // ---- which functions get code ----

    bool callable(uint32_t a) const {
        auto it = d_.functions.find(a);
        return it != d_.functions.end() && it->second.source != Source::Import;
    }

    // recomp_emit.cpp's walk: the instructions reached from the entry, the
    // blocks, the jump tables' targets (the resume sites: none, as functions
    // with any are left out)
    void walk(const Function &f, uint32_t end, std::vector<uint32_t> &code) const {
        std::vector<uint8_t> seen((end - f.address) / 4, 0);
        std::vector<uint32_t> work(f.blocks.begin(), f.blocks.end());
        work.push_back(f.address);
        auto inside = [&](uint32_t t) {
            return t >= f.address && t < end && (t == f.address || !d_.functions.count(t));
        };
        while (!work.empty()) {
            uint32_t pc = work.back();
            work.pop_back();
            if (pc < f.address || (pc - f.address) & 3) continue;
            for (; pc < end && !seen[(pc - f.address) / 4]; pc += 4) {
                Instr i{image_.word(pc)};
                Opcode op = ppc::decode(i.code);
                if (i.code == 0 || op == Opcode::Invalid) break;
                seen[(pc - f.address) / 4] = 1;
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
        for (size_t k = 0; k < seen.size(); k++)
            if (seen[k]) code.push_back(f.address + uint32_t(4 * k));
    }

    // what the function's branches and calls reach that the tier leaves to the
    // interpreter: a stack switch (KeSetCurrentStackPointers' thunk)
    bool reaches_stack_thunk(const std::vector<uint32_t> &code) const {
        if (d_.stack_thunks.empty()) return false;
        for (uint32_t pc : code) {
            Instr i{image_.word(pc)};
            Opcode op = ppc::decode(i.code);
            uint32_t t = 0;
            if (op == Opcode::bx)
                t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            else if (op == Opcode::bcx)
                t = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
            else if (op == Opcode::bcctrx && d_.tables.count(pc)) {
                for (uint32_t c : d_.tables.at(pc).targets)
                    if (d_.stack_thunks.count(c)) return true;
                continue;
            } else
                continue;
            if (d_.stack_thunks.count(t)) return true;
        }
        // falling into a thunk past the end
        return false;
    }

    void plan() {
        for (const auto &[a, f] : d_.functions) {
            if (f.source == Source::Import) continue;
            stats_.functions++;
            stats_.instructions += std::max<uint32_t>(f.size, 4) / 4;
            const char *why = nullptr;
            std::vector<uint32_t> code;
            const uint32_t end = f.address + std::max<uint32_t>(f.size, 4);
            if (f.switches_stack)
                why = "switches the stack (its returns are checked)";
            else if (!f.resumes.empty())
                why = "has resume sites";
            else {
                walk(f, end, code);
                if (code.empty() || code.front() != f.address)
                    why = "no code at its start";
                else if (reaches_stack_thunk(code))
                    why = "branches to a stack switch";
            }
            if (why) {
                stats_.left_out[why]++;
                continue;
            }
            index_[a] = uint32_t(order_.size());
            order_.push_back(&f);
            codes_.push_back(std::move(code));
            stats_.compiled++;
            stats_.compiled_instructions += std::max<uint32_t>(f.size, 4) / 4;
        }
        bodies_.assign(order_.size(), 0);
    }

    // ---- writing ----

    static bool rel32(uint8_t *field, int64_t v) {
        if (v != int64_t(int32_t(v))) return false;
        const int32_t w = int32_t(v);
        std::memcpy(field, &w, 4);
        return true;
    }

    // The runtime's table, the C library's stubs, the shared pieces and the
    // unwind information.
    bool header(const ppc::Runtime &runtime) {
        Region &r = m_.region_;
        uint8_t *rt = r.take(sizeof(ppc::Runtime), 64);
        if (!rt) return false;
        std::memcpy(rt, &runtime, sizeof(runtime));
        externs_[size_t(Extern::Runtime)] = rt;
        // jmp [rip+0]; the address
        const void *const c_library[] = {reinterpret_cast<const void *>(&std::memcpy),
                                         reinterpret_cast<const void *>(&std::memmove),
                                         reinterpret_cast<const void *>(&std::memset)};
        for (size_t k = 0; k < 3; k++) {
            uint8_t *stub = r.take(14, 16);
            if (!stub) return false;
            static const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
            std::memcpy(stub, jmp, 6);
            const uint64_t a = uint64_t(uintptr_t(c_library[k]));
            std::memcpy(stub + 6, &a, 8);
            externs_[size_t(Extern::Memcpy) + k] = stub;
        }
        shared_.assign(gen::piece_count, nullptr);
        unwind_rva_.assign(gen::piece_count, 0);
        for (size_t k = 0; k < gen::piece_count; k++) {
            const Piece &p = gen::pieces[k];
            if (p.unwind_size) {
                uint8_t *u = r.take(p.unwind_size, 4);
                if (!u) return false;
                std::memcpy(u, gen::unwind + p.unwind_offset, p.unwind_size);
                unwind_rva_[k] = uint32_t(u - r.base());
            }
            if (p.flags & PIECE_STENCIL) continue;
            uint8_t *at = r.take(p.size, std::max<uint32_t>(p.align, 1));
            if (!at) return false;
            std::memcpy(at, gen::bytes + p.offset, p.size);
            shared_[k] = at;
        }
        for (size_t k = 0; k < gen::piece_count; k++)
            if (shared_[k]) {
                patch(shared_[k], uint16_t(k), gen::pieces[k].size, nullptr, nullptr, 0);
                add_unwind(uint16_t(k), shared_[k], gen::pieces[k].size);
            }
        return true;
    }

    void add_unwind([[maybe_unused]] uint16_t piece, [[maybe_unused]] uint8_t *at, [[maybe_unused]] size_t size) {
#if defined(_WIN32) && defined(_M_X64)
        if (!gen::pieces[piece].unwind_size) return;
        RUNTIME_FUNCTION f;
        f.BeginAddress = DWORD(at - m_.region_.base());
        f.EndAddress = DWORD(at - m_.region_.base() + size);
        f.UnwindData = unwind_rva_[piece];
        m_.unwind_.push_back(f);
#endif
    }

    // Writes a copy's holes. inst: what the per-copy holes stand for (nullptr
    // for a shared piece, which has none); addr: the function's instances'
    // addresses (main, then out of line); next: the copy's successor.
    void patch(uint8_t *at, uint16_t piece, size_t copied, const Inst *inst, const uint8_t *next, uint8_t *data) {
        const Piece &p = gen::pieces[piece];
        for (uint32_t k = 0; k < p.reloc_count; k++) {
            const Reloc &rel = gen::relocs[p.reloc_first + k];
            if (rel.offset + 4 > copied) continue; // the dropped tail jump's
            uint8_t *field = at + rel.offset;
            bool wrap = false;
            int64_t s = 0;
            switch (rel.target) {
            case TargetKind::Piece: s = int64_t(uintptr_t(shared_[rel.index])); break;
            case TargetKind::Extern: s = int64_t(uintptr_t(externs_[rel.index])); break;
            case TargetKind::Hole:
                switch (Hole(rel.index)) {
                case Hole::Word: s = inst->word, wrap = true; break;
                case Hole::A: s = inst->a, wrap = true; break;
                case Hole::B: s = inst->b, wrap = true; break;
                case Hole::Data:
                    if (inst->counter) {
                        // the counter's page is placed at the end: written then
                        counter_fields_.push_back(field);
                        counter_index_.push_back(inst->word);
                        counter_addends_.push_back(rel.addend);
                        continue;
                    }
                    s = int64_t(uintptr_t(data));
                    break;
                case Hole::Next: s = int64_t(uintptr_t(next)); break;
                case Hole::Target:
                case Hole::Call: {
                    const Ref &ref = Hole(rel.index) == Hole::Target ? inst->target : inst->call;
                    if (ref.kind == Ref::Function && !bodies_[ref.value]) {
                        pending_.push_back({field, ref.value, rel.addend, rel.kind == RelocKind::Abs64});
                        continue;
                    }
                    s = int64_t(uintptr_t(resolve(ref)));
                    break;
                }
                default: break;
                }
                break;
            }
            switch (rel.kind) {
            case RelocKind::PcRel32: {
                const int64_t v = s + rel.addend - int64_t(uintptr_t(field));
                if (wrap) {
                    const uint32_t w = uint32_t(uint64_t(v));
                    std::memcpy(field, &w, 4);
                } else if (!rel32(field, v)) {
                    std::fprintf(stderr, "stencil: %s reloc %u target %d/%u out of reach\n", p.name, k, int(rel.target),
                                 rel.index);
                    std::abort(); // inside one reservation of a gigabyte: never
                }
                break;
            }
            case RelocKind::Abs64: {
                const uint64_t v = uint64_t(s + rel.addend);
                std::memcpy(field, &v, 8);
                break;
            }
            default: std::abort(); // ARM64's kinds: not built yet
            }
        }
    }

    // ---- one function ----

    const uint8_t *resolve(const Ref &ref) const {
        switch (ref.kind) {
        case Ref::Main: return addr_[ref.value];
        case Ref::Ool: return addr_[main_.size() + ref.value];
        case Ref::Pc: return addr_[label_.at(ref.value)];
        case Ref::Function: return reinterpret_cast<const uint8_t *>(bodies_[ref.value]);
        default: return nullptr;
        }
    }

    bool internal(uint32_t t) const {
        return t >= f_->address && t < end_ && std::binary_search(code_->begin(), code_->end(), t) &&
               (t == f_->address || !d_.functions.count(t));
    }

    // recomp_emit.cpp's go(): a branch's target as a jump (Pc, Function) or,
    // with none to jump to, a tail dispatch (None)
    Ref go(uint32_t t) const {
        if (internal(t)) return {Ref::Pc, t};
        if (auto it = index_.find(t); it != index_.end()) return {Ref::Function, it->second};
        return {};
    }
    // a jump to `t` at the end of a sequence
    void jump(std::vector<Inst> &seq, uint32_t t) {
        const Ref r = go(t);
        if (r.kind == Ref::None)
            seq.push_back({ids_.tail_dispatch, 0, t});
        else
            seq.push_back({ids_.go, 0, 0, 0, r});
    }
    // a call to `t` (LR already set)
    void call(std::vector<Inst> &seq, uint32_t t) {
        if (auto it = index_.find(t); it != index_.end()) {
            Inst i{ids_.call};
            i.call = {Ref::Function, it->second};
            seq.push_back(i);
        } else {
            seq.push_back({ids_.call_dispatch, 0, t});
        }
    }
    // An out-of-line sequence (a conditional branch's taken path that does
    // more than jump), its reference.
    Ref ool(std::vector<Inst> seq) {
        const Ref r{Ref::Ool, uint32_t(ool_.size())};
        ool_.insert(ool_.end(), seq.begin(), seq.end());
        return r;
    }
    // what follows an instruction that may fall through, when the next one is not emitted
    void fall_through(uint32_t pc) {
        const uint32_t next = pc + 4;
        if (std::binary_search(code_->begin(), code_->end(), next)) return;
        if (next >= end_) return jump(main_, next);
        main_.push_back({ids_.invalid, 0, next});
    }

    void instruction(uint32_t pc) {
        const Instr i{image_.word(pc)};
        const Opcode op = ppc::decode(i.code);
        const uint32_t next = pc + 4;
        switch (op) {
        case Opcode::bx: {
            const uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            if (i.I.LK) {
                main_.push_back({ids_.set_lr, 0, next});
                call(main_, t);
                fall_through(pc);
            } else {
                if (t <= pc) main_.push_back({ids_.yield});
                jump(main_, t);
            }
            return;
        }
        case Opcode::bcx: {
            const uint32_t t = uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc);
            const bool always = (i.B.BO & 0x14) == 0x14;
            if (i.B.LK) {
                // taken: the call, then on after the bcl
                Inst b{ids_.bcl[i.B.BO], 0, i.B.BI, next};
                std::vector<Inst> seq;
                call(seq, t);
                seq.push_back({ids_.go, 0, 0, 0, {Ref::Main, uint32_t(main_.size() + 1)}});
                b.target = ool(std::move(seq));
                main_.push_back(b);
                fall_through(pc);
                return;
            }
            const bool back = t <= pc;
            if (always) {
                if (back) main_.push_back({ids_.yield});
                jump(main_, t);
                return;
            }
            Inst b{ids_.bc[i.B.BO], 0, i.B.BI};
            const Ref r = go(t);
            if (back || r.kind == Ref::None) {
                std::vector<Inst> seq;
                if (back) seq.push_back({ids_.yield});
                jump(seq, t);
                b.target = ool(std::move(seq));
            } else {
                b.target = r;
            }
            main_.push_back(b);
            fall_through(pc);
            return;
        }
        case Opcode::bclrx: {
            const bool always = (i.XL.BO & 0x14) == 0x14;
            if (i.XL.LK) {
                main_.push_back({ids_.bclrl[i.XL.BO], 0, i.XL.BI, next});
                fall_through(pc);
            } else if (always) {
                main_.push_back({ids_.ret});
            } else {
                main_.push_back({ids_.bclr[i.XL.BO], 0, i.XL.BI});
                fall_through(pc);
            }
            return;
        }
        case Opcode::bcctrx: {
            const bool always = (i.XL.BO & 0x14) == 0x14;
            auto table = d_.tables.find(pc);
            if (!i.XL.LK && always && table != d_.tables.end() && !table->second.targets.empty()) {
                const auto [lo, hi] = std::minmax_element(table->second.targets.begin(), table->second.targets.end());
                TableData t;
                t.low = *lo, t.span = *hi - *lo + 4;
                t.slots.assign(t.span / 4, {});
                for (uint32_t target : table->second.targets)
                    if (!((target - t.low) & 3)) t.slots[(target - t.low) / 4] = go(target);
                Inst s{ids_.table, 0, t.low, t.span};
                s.table = int32_t(tables_.size());
                tables_.push_back(std::move(t));
                main_.push_back(s);
                return;
            }
            if (i.XL.LK) {
                main_.push_back({ids_.bcctrl[i.XL.BO], 0, i.XL.BI, next});
                fall_through(pc);
            } else if (always) {
                main_.push_back({ids_.tail_dispatch_ctr});
            } else {
                Inst b{ids_.bcctr[i.XL.BO], 0, i.XL.BI};
                b.target = ool({{ids_.tail_dispatch_ctr}});
                main_.push_back(b);
                fall_through(pc);
            }
            return;
        }
        default:
            // a trap that fires tells the system where it is
            if (op == Opcode::tw || op == Opcode::twi || op == Opcode::td || op == Opcode::tdi)
                main_.push_back({ids_.set_pc, 0, pc});
            main_.push_back({ids_.op[size_t(op)], i.code});
            fall_through(pc);
            return;
        }
    }

    bool function(size_t k) {
        f_ = order_[k];
        code_ = &codes_[k];
        end_ = f_->address + std::max<uint32_t>(f_->size, 4);
        main_.clear(), ool_.clear(), tables_.clear(), label_.clear();
        for (uint32_t pc : *code_) {
            label_[pc] = uint32_t(main_.size());
            if (options_.count) {
                // a counter for each opcode
                Inst c{ids_.count, uint32_t(ppc::decode(image_.word(pc)))};
                c.counter = true;
                main_.push_back(c);
            }
            instruction(pc);
        }
        stats_.copies += main_.size() + ool_.size() + 1;
        // the layout: the entry, the body, the out-of-line sequences, the tables
        Region &r = m_.region_;
        const size_t n = main_.size() + ool_.size();
        auto inst = [&](size_t j) -> const Inst & { return j < main_.size() ? main_[j] : ool_[j - main_.size()]; };
        auto copied = [&](size_t j) {
            const Piece &p = gen::pieces[inst(j).piece];
            return (p.flags & PIECE_TAIL_JUMP) ? p.size - 5 : p.size;
        };
        size_t total = gen::pieces[ids_.entry].size;
        for (size_t j = 0; j < n; j++) total += copied(j);
        const size_t start = r.used();
        uint8_t *entry = r.take(total, 16);
        if (!entry) return false;
        addr_.resize(n + 1);
        uint8_t *at = entry + gen::pieces[ids_.entry].size;
        for (size_t j = 0; j < n; j++) addr_[j] = at, at += copied(j);
        addr_[n] = at;
        bodies_[k] = uint64_t(uintptr_t(addr_[0]));
        // the tables' words
        std::vector<uint8_t *> data(tables_.size());
        for (size_t t = 0; t < tables_.size(); t++) {
            data[t] = r.take(tables_[t].slots.size() * 8, 8);
            if (!data[t]) return false;
            for (size_t s = 0; s < tables_[t].slots.size(); s++) {
                const Ref &ref = tables_[t].slots[s];
                uint64_t v = 0;
                if (ref.kind == Ref::Function && !bodies_[ref.value])
                    pending_.push_back({data[t] + 8 * s, ref.value, 0, true});
                else if (ref.kind != Ref::None)
                    v = uint64_t(uintptr_t(resolve(ref)));
                std::memcpy(data[t] + 8 * s, &v, 8);
            }
        }
        // copied and patched
        {
            std::memcpy(entry, gen::bytes + gen::pieces[ids_.entry].offset, gen::pieces[ids_.entry].size);
            Inst e{ids_.entry};
            e.call = {Ref::Main, 0};
            patch(entry, ids_.entry, gen::pieces[ids_.entry].size, &e, nullptr, nullptr);
            add_unwind(ids_.entry, entry, gen::pieces[ids_.entry].size);
        }
        for (size_t j = 0; j < n; j++) {
            const Inst &i = inst(j);
            const size_t size = copied(j);
            std::memcpy(addr_[j], gen::bytes + gen::pieces[i.piece].offset, size);
            patch(addr_[j], i.piece, size, &i, addr_[j + 1], i.table >= 0 ? data[size_t(i.table)] : nullptr);
            add_unwind(i.piece, addr_[j], size);
        }
        stats_.code_bytes += r.used() - start;
        m_.functions_.push_back({f_->address, reinterpret_cast<void (*)(ppc::Context &)>(entry)});
        return true;
    }

    const Image &image_;
    const Discovery &d_;
    const Options &options_;
    Stats &stats_;
    StencilModule &m_;
    const Ids &ids_;
    // the functions given code, in address order: their code, their bodies' addresses
    std::vector<const Function *> order_;
    std::vector<std::vector<uint32_t>> codes_;
    std::unordered_map<uint32_t, uint32_t> index_;
    std::vector<uint64_t> bodies_;
    std::vector<Pending> pending_;
    std::vector<uint8_t *> shared_;
    std::vector<uint32_t> unwind_rva_;
    uint8_t *externs_[size_t(Extern::Count)] = {};
    std::vector<uint8_t *> counter_fields_;
    std::vector<int64_t> counter_addends_;
    std::vector<uint32_t> counter_index_; // the opcode each counts
    // the function being made
    const Function *f_ = nullptr;
    const std::vector<uint32_t> *code_ = nullptr;
    uint32_t end_ = 0;
    std::vector<Inst> main_, ool_;
    std::vector<TableData> tables_;
    std::unordered_map<uint32_t, uint32_t> label_;
    std::vector<uint8_t *> addr_;
};

} // namespace

bool available(std::string *why) {
    if (!ids().ok) {
        if (why) *why = "the stencils lack " + ids().missing;
        return false;
    }
#if defined(__x86_64__) || defined(_M_X64)
    // the stencils are built at the game library's x86-64-v3 (xfstencil)
    if (!cpu_has_level(CpuLevel::X86_64_V3)) {
        if (why) *why = "the stencils are built for x86-64-v3, which this processor lacks";
        return false;
    }
#if !defined(_WIN32)
    // An Unwind thrown through the copies needs their unwind information
    // registered, which is done for Windows' .pdata only (TODO: .eh_frame's
    // FDEs through __register_frame).
    if (why) *why = "the copies' unwind information is only registered on Windows yet";
    return false;
#endif
    return true;
#else
    if (why) *why = "the stencils' relocations are only written for x86-64 yet";
    return false;
#endif
}

std::unique_ptr<Module> compile(const recomp::Image &image, const recomp::Discovery &d, const ppc::Runtime &runtime,
                                const std::string &name, uint64_t digest, const Options &options, Stats &stats,
                                std::string &error) {
    if (!available(&error)) return nullptr;
    const auto start = std::chrono::steady_clock::now();
    auto m = std::make_unique<StencilModule>();
    Compiler c(image, d, options, stats, *m);
    if (!c.run(runtime, error)) return nullptr;
    c.finish(name, digest);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return m;
}

#endif // XF_STENCIL_TIER

} // namespace xf::stencil
