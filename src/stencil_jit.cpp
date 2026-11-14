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
//
// Hosts: x86-64 (Windows, Linux) and ARM64 (Linux, Android). ARM64's direct
// branches reach 128 MB, less than the reservation: a call or jump farther
// goes through a veneer (an indirect jump through x16, which the procedure
// call standard leaves to such veneers), to the shared pieces from islands
// of veneers laid out every 64 MB, to other functions from veneers after the
// caller's code. The unwind information is Windows' function tables
// (RtlAddFunctionTable) there, and elsewhere a .eh_frame of the copies' own
// given to the C++ runtime's unwinder (__register_frame).

#include "xenoforge/stencil.h"

#include "stencil/stencil_fields.h"
#include "stencil_format.h"
#include "xenoforge/cpu_level.h"
#include "xenoforge/ppc.h"
#include "xenoforge/recomp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
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

Choice choice() {
    const char *tier = std::getenv("XF_TIER");
    if (!tier || !*tier) return Choice::Default;
    const std::string_view t = tier;
    if (t == "stencil") return Choice::Stencil;
    if (t == "library") return Choice::Library;
    if (t == "interpret") return Choice::Interpret;
    return Choice::Default;
}
bool requested() { return choice() == Choice::Stencil; }

#if !XF_STENCIL_TIER

bool available(std::string *why) {
    if (why) *why = "this build has no stencils (clang on x86-64 or ARM64 makes them)";
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

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr bool ARM64 = true;
#else
constexpr bool ARM64 = false;
#endif
// what fills the gaps between pieces: int3, or ARM64's udf #0
constexpr uint8_t FILL = ARM64 ? 0x00 : 0xCC;
constexpr size_t TAIL_JUMP = tail_jump_size(ARM64);

int64_t exts16(uint32_t v) { return int64_t(int16_t(v)); }
int64_t exts26(uint32_t v) { return int64_t(v & 0x02000000 ? int32_t(v) | int32_t(0xFC000000) : int32_t(v)); }

// ---- the stencils by name ----

struct Ids {
    uint16_t op[ppc::OPCODE_COUNT];
    uint16_t entry, ret, go, count, yield, set_lr, set_pc, call, call_dispatch, tail_dispatch, tail_dispatch_ctr,
        invalid, table;
    uint16_t switch_call, switch_tail, switch_tail_checked, checked, resumable, resume_body, resume_body_checked,
        hooked, mid, adopted;
    uint16_t bc[32], bcctr[32], bcl[32], bclr[32], bclrl[32], bcctrl[32];
    // the variants by opcode (stencil_fields.h), each with what it knows of
    // the word: tried in order, the general stencil when none fits
    struct Variant {
        uint16_t piece;
        stencil::Variant v;
    };
    std::vector<Variant> variants[ppc::OPCODE_COUNT];
    // the compare-and-branch superinstructions: by the compare (cmp, cmpi,
    // cmpl, cmpli), the bc's sense (0: taken when clear) and the CR bit
    uint16_t cmpbc[4][2][4] = {};
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
        // an instruction's stencil: the shorter of its two (stencils.cpp's
        // XF_OP_STENCILS: the register fields from holes, or from the word)
        auto shorter = [&](const std::string &name) -> uint16_t {
            const uint16_t fields = find(name), word = find(name + "_w");
            return gen::pieces[word].size < gen::pieces[fields].size ? word : fields;
        };
        for (uint32_t k = 0; k < ppc::OPCODE_COUNT; k++)
            t.op[k] = shorter(std::string("xf_st_op_") + ppc::info(Opcode(k)).name);
        t.entry = find("xf_st_entry"), t.ret = find("xf_st_ret"), t.go = find("xf_st_goto");
        t.count = find("xf_st_count"), t.yield = find("xf_st_yield"), t.set_lr = find("xf_st_set_lr");
        t.set_pc = find("xf_st_set_pc"), t.call = find("xf_st_call"), t.call_dispatch = find("xf_st_call_dispatch");
        t.tail_dispatch = find("xf_st_tail_dispatch"), t.tail_dispatch_ctr = find("xf_st_tail_dispatch_ctr");
        t.invalid = find("xf_st_invalid"), t.table = find("xf_st_table");
        t.switch_call = find("xf_st_switch_call"), t.switch_tail = find("xf_st_switch_tail");
        t.switch_tail_checked = find("xf_st_switch_tail_checked"), t.checked = find("xf_st_checked");
        t.resumable = find("xf_st_resumable"), t.resume_body = find("xf_st_resume_body");
        t.resume_body_checked = find("xf_st_resume_body_checked"), t.hooked = find("xf_st_hooked");
        t.mid = find("xf_st_mid"), t.adopted = find("xf_st_adopted");
        for (int bo = 0; bo < 32; bo++) {
            const std::string n = std::to_string(bo);
            t.bc[bo] = find("xf_st_bc_" + n), t.bcctr[bo] = find("xf_st_bcctr_" + n);
            t.bcl[bo] = find("xf_st_bcl_" + n), t.bclr[bo] = find("xf_st_bclr_" + n);
            t.bclrl[bo] = find("xf_st_bclrl_" + n), t.bcctrl[bo] = find("xf_st_bcctrl_" + n);
        }
        // the variants, as stencils.cpp names them from the same lists
        auto variant = [&](Opcode op, const char *suffix, const stencil::Variant &v) {
            t.variants[size_t(op)].push_back({shorter(std::string("xf_st_op_") + ppc::info(op).name + suffix), v});
        };
        static const char *const ra4[4] = {"_ra0", "_ra1", "_ra13", "_ran"}, *const ra2[2] = {"_ra0", "_ran"};
#define XF_JIT_RA4(name, form)                                                                                         \
    for (size_t k = 0; k < 4; k++) variant(Opcode::name, ra4[k], stencil::RA4_VARIANTS[k]);
#define XF_JIT_RA2(name, form)                                                                                         \
    for (size_t k = 0; k < 2; k++) variant(Opcode::name, ra2[k], stencil::RA2_VARIANTS[k]);
#define XF_JIT_SH16(name, form)                                                                                        \
    for (uint32_t sh = 0; sh < 16; sh++)                                                                               \
        variant(Opcode::name, ("_sh" + std::to_string(sh)).c_str(), stencil::sh_variant(sh));
#define XF_JIT_SPR_ONE(spr) sprs.push_back(spr);
#define XF_JIT_SPR(name, form)                                                                                         \
    for (uint32_t spr : sprs) variant(Opcode::name, ("_spr" + std::to_string(spr)).c_str(), stencil::spr_variant(spr));
#define XF_JIT_OR(name, form)                                                                                          \
    variant(Opcode::name, "_mr", stencil::OR_VARIANTS[0]);                                                             \
    variant(Opcode::name, "_or", stencil::OR_VARIANTS[1]);
        std::vector<uint32_t> sprs;
        XF_STENCIL_SPRS(XF_JIT_SPR_ONE)
        XF_STENCIL_RA4(XF_JIT_RA4)
        XF_STENCIL_RA2(XF_JIT_RA2)
        XF_STENCIL_SH16(XF_JIT_SH16)
        XF_STENCIL_SPR(XF_JIT_SPR)
        XF_STENCIL_OR(XF_JIT_OR)
#undef XF_JIT_RA4
#undef XF_JIT_RA2
#undef XF_JIT_SH16
#undef XF_JIT_SPR_ONE
#undef XF_JIT_SPR
#undef XF_JIT_OR
        // Rc 0, for the forms that have the bit (after the others: an RA
        // variant drops more)
        for (uint32_t k = 0; k < ppc::OPCODE_COUNT; k++)
            if (stencil::rc_mask(ppc::info(Opcode(k)).form))
                variant(Opcode(k), "_rc0", stencil::rc0_variant(ppc::info(Opcode(k)).form));
        const char *const compares[4] = {"cmp", "cmpi", "cmpl", "cmpli"};
        for (int cmp = 0; cmp < 4; cmp++)
            for (int sense = 0; sense < 2; sense++)
                for (int bit = 0; bit < 4; bit++)
                    t.cmpbc[cmp][sense][bit] = find(std::string("xf_st_cmpbc_") + compares[cmp] + "_" +
                                                    std::to_string(sense) + "_" + std::to_string(bit));
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
        if (at > used_) std::memset(base_ + used_, FILL, at - used_);
        used_ = at + n;
        return base_ + at;
    }
    // [start, end) read and executed from now on (start on a page boundary);
    // what follows stays writable
    bool seal(size_t start, size_t end) { return protect(start, end, false); }
    // [start, end), sealed, made writable too while it stays executable (the
    // tier-up's jumps written while every guest thread is parked), or sealed again
    bool writable_code(size_t start, size_t end, bool writable) { return protect(start, end, writable); }
    // the next piece on a page of its own (past what was sealed)
    void align_to_page() { used_ = (used_ + page_ - 1) & ~(page_ - 1); }
    size_t page() const { return page_; }

  private:
    bool protect(size_t start, size_t end, bool writable) {
        end = (end + page_ - 1) & ~(page_ - 1);
        if (end <= start) return true;
#if defined(_WIN32)
        DWORD old;
        if (!VirtualProtect(base_ + start, end - start, writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ, &old))
            return false;
        FlushInstructionCache(GetCurrentProcess(), base_ + start, end - start);
#else
        if (mprotect(base_ + start, end - start, PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0)) != 0)
            return false;
        __builtin___clear_cache(reinterpret_cast<char *>(base_ + start), reinterpret_cast<char *>(base_ + end));
#endif
        return true;
    }
    uint8_t *base_ = nullptr;
    size_t size_ = 0, used_ = 0, committed_ = 0, page_ = 4096;
};

// ---- writing a hole ----

// Writes a relocation of `kind` at `field` for the value S + A (stencil_format.h),
// false when it does not reach. wrap: S is a 32-bit hole (xf_hole_word, _a,
// _b), of whose address the stencil uses the low 32 bits only, so any address
// with those bits will do: the one nearest the field, which a pc-relative
// field always reaches.
bool write_reloc(uint8_t *field, RelocKind kind, int64_t s, int64_t addend, bool wrap) {
    const int64_t p = int64_t(uintptr_t(field));
    int64_t v = s + addend;
    if (wrap) {
        const uint64_t low = uint64_t(v) & 0xFFFFFFFFu;
        int64_t best = int64_t((uint64_t(p) & ~uint64_t(0xFFFFFFFF)) | low);
        for (int64_t c : {best - (int64_t(1) << 32), best + (int64_t(1) << 32)})
            if (std::llabs(c - p) < std::llabs(best - p)) best = c;
        v = best;
    }
    auto insn = [&](uint32_t mask, uint32_t bits) {
        uint32_t w;
        std::memcpy(&w, field, 4);
        w = (w & ~mask) | (bits & mask);
        std::memcpy(field, &w, 4);
    };
    switch (kind) {
    case RelocKind::PcRel32: {
        const int64_t d = v - p;
        if (!wrap && d != int64_t(int32_t(d))) return false;
        const uint32_t w = uint32_t(uint64_t(d));
        std::memcpy(field, &w, 4);
        return true;
    }
    case RelocKind::Abs64: {
        const uint64_t w = uint64_t(v);
        std::memcpy(field, &w, 8);
        return true;
    }
    case RelocKind::Branch26: {
        const int64_t d = v - p;
        if (d & 3 || d < -(int64_t(1) << 27) || d >= (int64_t(1) << 27)) return false;
        insn(0x03FFFFFFu, uint32_t(d >> 2));
        return true;
    }
    case RelocKind::AdrPage21: {
        const int64_t pages = (v >> 12) - (p >> 12);
        if (pages < -(int64_t(1) << 20) || pages >= (int64_t(1) << 20)) return false;
        const uint32_t imm = uint32_t(pages) & 0x1FFFFF;
        insn(0x60FFFFE0u, (imm & 3) << 29 | (imm >> 2) << 5);
        return true;
    }
    case RelocKind::AddLo12: insn(0x003FFC00u, uint32_t(v & 0xFFF) << 10); return true;
    case RelocKind::LdSt8Lo12:
    case RelocKind::LdSt16Lo12:
    case RelocKind::LdSt32Lo12:
    case RelocKind::LdSt64Lo12:
    case RelocKind::LdSt128Lo12: {
        const int scale = int(kind) - int(RelocKind::LdSt8Lo12);
        if (v & ((int64_t(1) << scale) - 1)) return false; // misaligned for the access
        insn(0x003FFC00u, uint32_t((v & 0xFFF) >> scale) << 10);
        return true;
    }
    case RelocKind::MovwG0:
    case RelocKind::MovwG1:
    case RelocKind::MovwG2:
    case RelocKind::MovwG3: {
        const int shift = 16 * (int(kind) - int(RelocKind::MovwG0));
        insn(0x001FFFE0u, uint32_t((uint64_t(v) >> shift) & 0xFFFF) << 5);
        return true;
    }
    }
    return false;
}

// An ARM64 veneer (16 bytes): ldr x16, #8; br x16; the target's address.
void write_veneer(uint8_t *at, uint64_t target) {
    const uint32_t code[2] = {0x58000050u, 0xD61F0200u};
    std::memcpy(at, code, 8);
    std::memcpy(at + 8, &target, 8);
}
constexpr size_t VENEER_SIZE = 16;

// XF_STENCIL_FAR=1 (ARM64, for tests): every branch to another function or a
// shared piece goes through its veneer, and the tier-up's jumps through the
// literal, as they do past 128 MB, so a small program checks those paths.
bool force_far() {
    static const bool far_ = ARM64 && std::getenv("XF_STENCIL_FAR") && *std::getenv("XF_STENCIL_FAR") == '1';
    return far_;
}

#if !defined(_WIN32)
} // namespace
} // namespace xf::stencil
// libgcc's and LLVM's libunwind's: both take a whole .eh_frame (from a CIE to
// a zero length; libunwind since LLVM 13, the version Android's NDK has had
// since r23)
extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);
namespace xf::stencil {
namespace {

// The copies' unwind information on ELF hosts: a .eh_frame of their own, with
// each CIE the stencils use once and an FDE for each copy of a piece that has
// rules (stencil_format.h's Piece), its addresses absolute (8 bytes), so the
// table may lie anywhere. Registered once it is whole; kept, unchanged, until
// it is deregistered.
class EhFrame {
  public:
    EhFrame() = default;
    EhFrame(const EhFrame &) = delete;
    EhFrame &operator=(const EhFrame &) = delete;
    ~EhFrame() {
        if (registered_) __deregister_frame(bytes_.data());
    }
    void add(const Piece &p, const uint8_t *at, size_t size) {
        if (!p.unwind_size) return;
        const uint8_t *u = gen::unwind + p.unwind_offset;
        const size_t cie_size = size_t(u[0]) | size_t(u[1]) << 8;
        const uint8_t *cie = u + 2, *tail = cie + cie_size;
        const size_t tail_size = p.unwind_size - 2 - cie_size;
        size_t cie_at = SIZE_MAX;
        for (const auto &[offset, size_, bytes] : cies_)
            if (size_ == cie_size && std::memcmp(bytes, cie, cie_size) == 0) cie_at = offset;
        if (cie_at == SIZE_MAX) {
            cie_at = bytes_.size();
            cies_.push_back({cie_at, cie_size, cie});
            const size_t start = begin();
            u32(0); // a CIE's id
            bytes_.insert(bytes_.end(), cie, cie + cie_size);
            end(start);
        }
        const size_t start = begin();
        u32(uint32_t(bytes_.size() - cie_at)); // back to its CIE
        u64(uint64_t(uintptr_t(at)));
        u64(size);
        bytes_.insert(bytes_.end(), tail, tail + tail_size);
        end(start);
        fdes_++;
    }
    size_t fdes() const { return fdes_; }
    // The FDEs of the code whose start `gone` says is released, dropped: the
    // table made again without them (each CIE kept) and registered in place
    // of this one. How many were dropped.
    template <class Gone> size_t drop(Gone gone) {
        if (!registered_) return 0;
        std::vector<uint8_t> out;
        std::vector<std::pair<size_t, size_t>> cie_at; // old offset, new
        size_t dropped = 0;
        for (size_t at = 0; at + 4 <= bytes_.size();) {
            uint32_t length, id;
            std::memcpy(&length, bytes_.data() + at, 4);
            if (!length) break;
            std::memcpy(&id, bytes_.data() + at + 4, 4);
            const size_t size = 4 + size_t(length);
            if (id == 0) {
                cie_at.push_back({at, out.size()});
                out.insert(out.end(), bytes_.begin() + at, bytes_.begin() + at + size);
            } else {
                uint64_t pc;
                std::memcpy(&pc, bytes_.data() + at + 8, 8);
                if (gone(pc)) {
                    dropped++;
                } else {
                    // its pointer back to its CIE, from where it is now
                    const size_t cie = at + 4 - id;
                    size_t now = 0;
                    for (const auto &[old, moved] : cie_at)
                        if (old == cie) now = moved;
                    const size_t start = out.size();
                    out.insert(out.end(), bytes_.begin() + at, bytes_.begin() + at + size);
                    const uint32_t back = uint32_t(start + 4 - now);
                    std::memcpy(out.data() + start + 4, &back, 4);
                }
            }
            at += size;
        }
        if (!dropped) return 0;
        const uint32_t zero = 0;
        out.insert(out.end(), reinterpret_cast<const uint8_t *>(&zero), reinterpret_cast<const uint8_t *>(&zero) + 4);
        __deregister_frame(bytes_.data());
        bytes_ = std::move(out);
        fdes_ -= dropped;
        __register_frame(bytes_.data());
        return dropped;
    }
    bool install() {
        if (!fdes_) return true;
        u32(0); // the end
        __register_frame(bytes_.data());
        registered_ = true;
        return true;
    }

  private:
    size_t begin() {
        const size_t start = bytes_.size();
        u32(0);
        return start;
    }
    // the record's length written, padded with DW_CFA_nop to 8 bytes
    void end(size_t start) {
        while ((bytes_.size() - start) % 8) bytes_.push_back(0);
        const uint32_t length = uint32_t(bytes_.size() - start - 4);
        std::memcpy(bytes_.data() + start, &length, 4);
    }
    void u32(uint32_t v) {
        bytes_.insert(bytes_.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 4);
    }
    void u64(uint64_t v) {
        bytes_.insert(bytes_.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 8);
    }
    struct Cie {
        size_t offset, size;
        const uint8_t *bytes;
    };
    std::vector<Cie> cies_;
    std::vector<uint8_t> bytes_;
    size_t fdes_ = 0;
    bool registered_ = false;
};
#endif

// ---- a function as stencils ----

// Where a hole Target or Call, or a host address in data, goes.
struct Ref {
    // Pre, Main, Ool, Post: an index in that sequence; Pc: the instruction at
    // a guest address; Function: a function's entry (its front)
    enum Kind : uint8_t { None, Pre, Main, Ool, Post, Pc, Function } kind = None;
    uint32_t value = 0;
};

struct Inst {
    Inst(uint16_t p = 0, uint32_t w = 0, uint32_t a_ = 0, uint32_t b_ = 0, Ref t = {})
        : piece(p), word(w), a(a_), b(b_), target(t) {}
    uint16_t piece;
    uint32_t word, a, b;
    uint32_t f[stencil::MAX_FIELDS] = {}; // the register fields' holes (stencil_fields.h)
    Ref target, call;
    int32_t data = -1;  // its data block, in the function's
    int64_t rw = -1;    // else its data in the writable data (RwLayout's offset: xf_st_count's counter)
    bool close = false; // a C++ entry among the others, near the table of entries: never a veneer
};

// Data laid out after a function's code: bytes, and the host addresses
// written into them once the code is placed.
struct Block {
    std::vector<uint8_t> bytes;
    std::vector<std::pair<size_t, Ref>> refs; // an 8-byte field each
    void u32(uint32_t v) {
        bytes.insert(bytes.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 4);
    }
    void u64(uint64_t v) {
        bytes.insert(bytes.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 8);
    }
    void ref(const Ref &r) {
        refs.push_back({bytes.size(), r});
        u64(0);
    }
    void align8() { bytes.resize((bytes.size() + 7) & ~size_t(7), 0); }
};

// A function's entry as the other functions' code reaches it: its slot in the
// table of entries laid out after the shared pieces, eight bytes that jump to
// its code, which the tier-up makes a jump to an adapter that calls the game
// library's (Module::adopt). Calls and tail branches between functions given
// code all go there; a branch inside the function to its own start does not.
// They are kept apart from the code so that the code of the functions the
// library took can be released (Module::release) while the code still in use
// keeps calling them. x86-64: a jmp rel32 and a 3-byte nop. ARM64: an 8-byte
// literal, then a b and a nop, or, farther than a b reaches, ldr x16 from the
// literal and br x16.
constexpr size_t ENTRY_SIZE = ARM64 ? 16 : 8;
constexpr size_t FRONT_LITERAL = ARM64 ? 8 : 0; // the front: past the literal
constexpr size_t FRONT_SIZE = 8;

// The eight bytes of a front that jumps to `to` (ARM64: the literal written
// first when the jump is indirect).
uint64_t front_jump(uint8_t *front, const uint8_t *to, bool far_ = false) {
    uint8_t bytes[8];
    if (ARM64) {
        const uint32_t nop = 0xD503201Fu;
        const int64_t d = int64_t(uintptr_t(to)) - int64_t(uintptr_t(front));
        if (d >= -(int64_t(1) << 27) && d < (int64_t(1) << 27) && !far_) {
            const uint32_t code[2] = {0x14000000u | (uint32_t(d >> 2) & 0x03FFFFFFu), nop};
            std::memcpy(bytes, code, 8);
        } else {
            const uint64_t a = uint64_t(uintptr_t(to));
            __atomic_store_n(reinterpret_cast<uint64_t *>(front - 8), a, __ATOMIC_SEQ_CST);
            const uint32_t code[2] = {0x58FFFFD0u, 0xD61F0200u}; // ldr x16, #-8; br x16
            std::memcpy(bytes, code, 8);
        }
    } else {
        bytes[0] = 0xE9;
        const int32_t w = int32_t(int64_t(uintptr_t(to)) - int64_t(uintptr_t(front + 5)));
        std::memcpy(bytes + 1, &w, 4);
        bytes[5] = 0x0F, bytes[6] = 0x1F, bytes[7] = 0x00; // nop dword [rax]
    }
    uint64_t word;
    std::memcpy(&word, bytes, 8);
    return word;
}

// The writable data after the code (one reservation, so the stencils reach it
// pc-relative), by offset: the shared helpers' call counters (Options::count),
// then two counters a function (its entries, its loops' back edges taken),
// then a counter a guest instruction (Options::count).
struct RwLayout {
    size_t helpers = 0, functions = 0, instructions = 0;
    size_t helper(size_t k) const { return 8 * k; }
    size_t entries(size_t k) const { return 8 * helpers + 16 * k; }
    size_t back_edges(size_t k) const { return entries(k) + 8; }
    size_t instruction(size_t k) const { return 8 * helpers + 16 * functions + 8 * k; }
    size_t size() const { return instruction(instructions); }
};

class Compiler;

class StencilModule final : public Module {
  public:
    const ppc::GeneratedModule *table() const override { return &module_; }
    uint64_t executed() const override {
        uint64_t n = 0;
        for (size_t k = 0; counter_ && k < counter_op_.size(); k++) n += counter_[k];
        return n;
    }
    const uint64_t *executed_by_opcode() const override {
        if (!counter_) return nullptr;
        std::fill(std::begin(by_opcode_), std::end(by_opcode_), 0);
        for (size_t k = 0; k < counter_op_.size(); k++) by_opcode_[counter_op_[k]] += counter_[k];
        return by_opcode_;
    }
    size_t adopt(const ppc::GeneratedModule &library, std::string &error) override;
    std::vector<FunctionHeat> heat() const override;
    std::string count_report(size_t top, const std::string &pieces_file) const override;
    bool describe(uint64_t address, std::string &name, uint64_t &displacement) const override;
    std::pair<uintptr_t, uintptr_t> code_range() const override {
        return {uintptr_t(region_.base()), uintptr_t(region_.base() + code_end_)};
    }
    size_t release(const std::vector<uintptr_t> &live, ReleaseStats &stats, std::string &error) override;
    size_t committed_bytes() const override { return region_.committed() - released_bytes_; }
    ~StencilModule() override {
#if defined(_WIN32)
        for (auto &table : unwind_tables_)
            if (!table.empty()) RtlDeleteFunctionTable(table.data());
#endif
    }

  private:
    friend class Compiler;
    // a function given code: its guest address, its index (its counters), its
    // front, and [begin, end) in the region: its code and its data
    struct Front {
        uint32_t address, index, instructions;
        uint8_t *front;
        size_t begin, end;
        bool operator<(const Front &o) const { return address < o.address; }
    };
    Region region_;
    std::string name_;
    std::vector<ppc::GeneratedFunction> functions_;
    std::vector<uint32_t> resumes_;
    std::deque<ppc::HookSlot> slots_;         // the hook slots (hooks::bind fills them)
    std::vector<ppc::HookSlot *> slot_table_; // their table, as a game library's
    ppc::GeneratedModule module_{};
    size_t code_end_ = 0;             // [0, code_end_): the code, sealed
    size_t entries_begin_ = 0;        // the table of entries, in the code
    std::vector<Front> fronts_;       // by guest address
    std::vector<uint64_t *> adopted_; // each front's adapter's data (the library's function, its calls), or null
    std::vector<uint8_t> released_;   // each front's code released (Module::release)
    size_t released_bytes_ = 0;
    std::vector<uint8_t> decommitted_; // the code's pages released, by page
    uint16_t adopted_piece_ = 0;
    uint32_t adopted_unwind_rva_ = 0; // its unwind information, copied with the shared pieces
    // the writable data (RwLayout) and its layout
    uint8_t *rw_ = nullptr;
    RwLayout rw_layout_;
    // Options::count: a counter a guest instruction (in the writable data),
    // its opcode, the stencils made for it, and the shared helpers counted
    uint64_t *counter_ = nullptr;
    std::vector<uint16_t> counter_op_;
    std::vector<uint32_t> counter_first_; // into counter_pieces_ (one more: the end)
    std::vector<uint16_t> counter_pieces_;
    std::vector<uint16_t> helpers_; // the pieces whose calls are counted
    mutable uint64_t by_opcode_[ppc::OPCODE_COUNT] = {};
    // Options::map: each copy's offset in the region and its piece, in order
    std::vector<std::pair<uint32_t, uint16_t>> copies_;
#if defined(_WIN32)
    // the unwind tables registered (the code's, then each adopt()'s)
    std::vector<std::vector<RUNTIME_FUNCTION>> unwind_tables_;
#else
    std::deque<EhFrame> eh_frames_; // the same
#endif
};

class Compiler {
  public:
    Compiler(const Image &image, const Discovery &d, const Options &options, Stats &stats, StencilModule &m)
        : image_(image), d_(d), options_(options), stats_(stats), m_(m), ids_(ids()) {}

    bool run(const ppc::Runtime &runtime, std::string &error) {
        plan();
        // a gigabyte: games' code makes a few hundred megabytes at most (about
        // a hundred bytes an instruction), and every pc-relative field reaches across it
        if (!m_.region_.reserve(size_t(1) << 30)) return error = "could not reserve the code's memory", false;
#if !defined(_WIN32)
        m_.eh_frames_.emplace_back();
#endif
        rw_.functions = plans_.size();
        if (!header(runtime)) return error = "out of the code's memory", false;
        // the table of entries, each first a trap until its function is made
        Region &r = m_.region_;
        uint8_t *entries = r.take(plans_.size() * ENTRY_SIZE + 8, 16);
        if (!entries) return error = "out of the code's memory", false;
        std::memset(entries, FILL, plans_.size() * ENTRY_SIZE + 8);
        m_.entries_begin_ = size_t(entries - r.base());
        for (size_t k = 0; k < plans_.size(); k++)
            bodies_[k] = uint64_t(uintptr_t(entries + k * ENTRY_SIZE + FRONT_LITERAL));
        // the C++ entries after it (function()), kept with it when the code is released
        c_entry_size_ = (gen::pieces[ids_.entry].size + 15) & ~size_t(15);
        c_entries_ = r.take(plans_.size() * c_entry_size_ + 16, 16);
        if (!c_entries_) return error = "out of the code's memory", false;
        std::memset(c_entries_, FILL, plans_.size() * c_entry_size_ + 16);
        for (size_t k = 0; k < plans_.size(); k++)
            if (!function(k)) return error = "out of the code's memory", false;
        for (const Pending &p : pending_)
            if (!place(p.field, p.kind, int64_t(bodies_[p.function]), p.addend, p.veneer))
                return error = "a call out of the code's reach", false;
        const size_t code_end = r.used();
        m_.code_end_ = code_end;
        // the writable data on pages of their own, past the code
        rw_.instructions = m_.counter_op_.size();
        r.align_to_page();
        uint8_t *rw = r.take(std::max<size_t>(rw_.size(), 8), r.page());
        if (!rw) return error = "out of the code's memory", false;
        std::memset(rw, 0, rw_.size());
        m_.rw_ = rw, m_.rw_layout_ = rw_;
        if (options_.count) m_.counter_ = reinterpret_cast<uint64_t *>(rw + rw_.instruction(0));
        for (const RwField &f : rw_fields_)
            if (!write_reloc(f.field, f.kind, int64_t(uintptr_t(rw + f.offset)), f.addend, false))
                return error = "the writable data out of the code's reach", false;
        m_.counter_first_.push_back(uint32_t(m_.counter_pieces_.size()));
        if (!r.seal(0, code_end)) return error = "could not make the code executable", false;
#if defined(_WIN32)
        // (sorted by address, as the system looks entries up)
        std::sort(unwind_.begin(), unwind_.end(),
                  [](const RUNTIME_FUNCTION &a, const RUNTIME_FUNCTION &b) { return a.BeginAddress < b.BeginAddress; });
        if (!unwind_.empty()) {
            m_.unwind_tables_.push_back(std::move(unwind_));
            auto &table = m_.unwind_tables_.back();
            if (!RtlAddFunctionTable(table.data(), DWORD(table.size()), DWORD64(uintptr_t(r.base()))))
                return error = "could not register the code's unwind information", false;
            stats_.unwind_entries = table.size();
        }
#else
        stats_.unwind_entries = m_.eh_frames_.front().fdes();
        if (!m_.eh_frames_.front().install()) return error = "could not register the code's unwind information", false;
#endif
        stats_.region_bytes = r.used();
        m_.adopted_piece_ = ids_.adopted;
        return true;
    }

    // the module's table (functions and resume sites in address order, as
    // ppc_module.h has them)
    void finish(const std::string &name, uint64_t digest) {
        m_.name_ = name;
        std::sort(
            m_.functions_.begin(), m_.functions_.end(),
            [](const ppc::GeneratedFunction &a, const ppc::GeneratedFunction &b) { return a.address < b.address; });
        std::sort(m_.resumes_.begin(), m_.resumes_.end());
        m_.resumes_.erase(std::unique(m_.resumes_.begin(), m_.resumes_.end()), m_.resumes_.end());
        std::sort(m_.fronts_.begin(), m_.fronts_.end());
        m_.adopted_.assign(m_.fronts_.size(), nullptr);
        m_.released_.assign(m_.fronts_.size(), 0);
        for (ppc::HookSlot &s : m_.slots_) m_.slot_table_.push_back(&s);
        m_.module_.name = m_.name_.c_str();
        m_.module_.base = image_.base;
        m_.module_.size = uint32_t(image_.bytes.size());
        m_.module_.digest = digest;
        m_.module_.functions = m_.functions_.data();
        m_.module_.count = m_.functions_.size();
        m_.module_.resumes = m_.resumes_.data();
        m_.module_.resume_count = m_.resumes_.size();
        m_.module_.hooks = m_.slot_table_.data();
        m_.module_.hook_count = m_.slot_table_.size();
    }

  private:
    // A field written once its target is placed: a function's entry (function:
    // its index), when a call to it is made before it (ARM64's veneers).
    struct Pending {
        uint8_t *field;
        uint32_t function;
        int64_t addend;
        RelocKind kind;
        uint8_t *veneer; // ARM64: a veneer kept for a branch that may not reach
    };
    // A field to the writable data, written once it is placed after the code.
    struct RwField {
        uint8_t *field;
        size_t offset;
        int64_t addend;
        RelocKind kind;
    };

    // ---- which functions get code ----

    bool callable(uint32_t a) const {
        auto it = d_.functions.find(a);
        return it != d_.functions.end() && it->second.source != Source::Import;
    }

    // recomp_emit.cpp's walk: the instructions reached from the entry, the
    // blocks discovery found, the jump tables' targets and the resume sites
    void walk(const Function &f, uint32_t end, const std::set<uint32_t> &resumes, std::vector<uint32_t> &code) const {
        std::vector<uint8_t> seen((end - f.address) / 4, 0);
        std::vector<uint32_t> work(f.blocks.begin(), f.blocks.end());
        work.push_back(f.address);
        work.insert(work.end(), resumes.begin(), resumes.end());
        auto inside = [&](uint32_t t) {
            return t >= f.address && t < end && (t == f.address || !d_.functions.count(t) || resumes.count(t));
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

    void plan() {
        const Project *project = options_.project;
        for (const auto &[a, f] : d_.functions) {
            if (f.source == Source::Import) continue;
            stats_.functions++;
            stats_.instructions += std::max<uint32_t>(f.size, 4) / 4;
            const char *why = nullptr;
            Plan p;
            p.f = &f;
            const uint32_t end = f.address + std::max<uint32_t>(f.size, 4);
            // the resume sites inside it (the emitter's), those the walk reaches
            for (uint32_t r : f.resumes)
                if (r > f.address && r < end) p.resumes.insert(r);
            if (project && project->skip.count(a)) {
                why = "skipped by the project file";
            } else {
                walk(f, end, p.resumes, p.code);
                std::erase_if(p.resumes,
                              [&](uint32_t r) { return !std::binary_search(p.code.begin(), p.code.end(), r); });
                if (p.code.empty() || p.code.front() != f.address) why = "no code at its start";
            }
            if (why) {
                stats_.left_out[why]++;
                continue;
            }
            if (project)
                if (auto h = project->hooks.find(a); h != project->hooks.end() && !h->second.empty())
                    p.hooks = &h->second;
            index_[a] = uint32_t(plans_.size());
            plans_.push_back(std::move(p));
            stats_.compiled++;
            stats_.compiled_instructions += std::max<uint32_t>(f.size, 4) / 4;
        }
        bodies_.assign(plans_.size(), 0);
    }

    // ---- writing ----

    // A field to S + A; an ARM64 branch that does not reach goes through the
    // veneer given (written here), or none
    static bool place(uint8_t *field, RelocKind kind, int64_t s, int64_t addend, uint8_t *veneer) {
        if (!(force_far() && veneer) && write_reloc(field, kind, s, addend, false)) return true;
        if (kind != RelocKind::Branch26 || !veneer) return false;
        write_veneer(veneer, uint64_t(s + addend));
        return write_reloc(field, kind, int64_t(uintptr_t(veneer)), 0, false);
    }

    // ARM64: an island of veneers to the shared code pieces and the C
    // library's stubs, for the branches to them from code more than 128 MB
    // away; one is laid out each time the code has grown 64 MB since the last
    // (the header counts as the first, its pieces reached directly)
    static constexpr size_t ISLAND_EVERY = size_t(64) << 20;
    bool island() {
        Region &r = m_.region_;
        if (!ARM64 || (r.used() - island_at_ < ISLAND_EVERY && !(force_far() && island_at_ == 0))) return true;
        for (size_t k = 0; k < gen::piece_count; k++) {
            if (!shared_[k] || !(gen::pieces[k].flags & PIECE_CODE)) continue;
            uint8_t *v = r.take(VENEER_SIZE, 16);
            if (!v) return false;
            write_veneer(v, uint64_t(uintptr_t(shared_[k])));
            far_piece_[k] = v;
        }
        for (size_t k = 0; k < size_t(Extern::Count); k++) {
            if (k == size_t(Extern::Runtime)) continue; // data
            uint8_t *v = r.take(VENEER_SIZE, 16);
            if (!v) return false;
            write_veneer(v, uint64_t(uintptr_t(externs_[k])));
            far_extern_[k] = v;
        }
        island_at_ = r.used();
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
        // x86-64: jmp [rip+0]; the address. ARM64: a veneer.
        const void *const c_library[] = {reinterpret_cast<const void *>(&std::memcpy),
                                         reinterpret_cast<const void *>(&std::memmove),
                                         reinterpret_cast<const void *>(&std::memset)};
        for (size_t k = 0; k < 3; k++) {
            const uint64_t a = uint64_t(uintptr_t(c_library[k]));
            uint8_t *stub = r.take(ARM64 ? VENEER_SIZE : 14, 16);
            if (!stub) return false;
            if (ARM64) {
                write_veneer(stub, a);
            } else {
                static const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
                std::memcpy(stub, jmp, 6);
                std::memcpy(stub + 6, &a, 8);
            }
            externs_[size_t(Extern::Memcpy) + k] = stub;
        }
        shared_.assign(gen::piece_count, nullptr);
        placed_.assign(gen::piece_count, nullptr);
        far_piece_.assign(gen::piece_count, nullptr);
        unwind_rva_.assign(gen::piece_count, 0);
        for (size_t k = 0; k < gen::piece_count; k++) {
            const Piece &p = gen::pieces[k];
#if defined(_WIN32)
            // Windows: the UNWIND_INFO in the region, which its tables name by offset
            if (p.unwind_size) {
                uint8_t *u = r.take(p.unwind_size, 4);
                if (!u) return false;
                std::memcpy(u, gen::unwind + p.unwind_offset, p.unwind_size);
                unwind_rva_[k] = uint32_t(u - r.base());
            }
#endif
            if (p.flags & PIECE_STENCIL) continue;
            if (options_.count && !ARM64 && (p.flags & PIECE_CODE)) {
                // a helper's calls counted (the stencils' slow paths): inc qword
                // [rip+counter] right before it, where the stencils call
                uint8_t *at = r.take(16 + p.size, std::max<uint32_t>(p.align, 16));
                if (!at) return false;
                static const uint8_t inc[3] = {0x48, 0xFF, 0x05};
                std::memset(at, FILL, 9);
                std::memcpy(at + 9, inc, 3);
                rw_fields_.push_back({at + 12, rw_.helper(m_.helpers_.size()), -4, RelocKind::PcRel32});
                m_.helpers_.push_back(uint16_t(k));
                std::memcpy(at + 16, gen::bytes + p.offset, p.size);
                shared_[k] = at + 9, placed_[k] = at + 16;
                continue;
            }
            uint8_t *at = r.take(p.size, std::max<uint32_t>(p.align, 1));
            if (!at) return false;
            std::memcpy(at, gen::bytes + p.offset, p.size);
            shared_[k] = placed_[k] = at;
        }
        rw_.helpers = m_.helpers_.size();
        for (size_t k = 0; k < gen::piece_count; k++)
            if (placed_[k]) {
                patch(placed_[k], uint16_t(k), gen::pieces[k].size, nullptr, nullptr, 0);
                add_unwind(uint16_t(k), placed_[k], gen::pieces[k].size);
                if (options_.map) m_.copies_.push_back({uint32_t(placed_[k] - r.base()), uint16_t(k)});
            }
        if (options_.map) std::sort(m_.copies_.begin(), m_.copies_.end());
        // the tier-up's adapters are copied later: their unwind information now
        m_.adopted_unwind_rva_ = unwind_rva_[ids_.adopted];
        island_at_ = 0;
        return true;
    }

    void add_unwind([[maybe_unused]] uint16_t piece, [[maybe_unused]] uint8_t *at, [[maybe_unused]] size_t size) {
#if defined(_WIN32) && defined(_M_X64)
        if (!gen::pieces[piece].unwind_size) return;
        RUNTIME_FUNCTION f;
        f.BeginAddress = DWORD(at - m_.region_.base());
        f.EndAddress = DWORD(at - m_.region_.base() + size);
        f.UnwindData = unwind_rva_[piece];
        unwind_.push_back(f);
#elif !defined(_WIN32)
        m_.eh_frames_.front().add(gen::pieces[piece], at, size);
#endif
    }

    // Whether a relocation of a copy is an ARM64 branch to another function,
    // which may need a veneer (function() keeps one for each).
    static bool far_call(const Reloc &rel, const Inst &inst) {
        if (inst.close || rel.kind != RelocKind::Branch26 || rel.target != TargetKind::Hole) return false;
        if (Hole(rel.index) == Hole::Target) return inst.target.kind == Ref::Function;
        if (Hole(rel.index) == Hole::Call) return inst.call.kind == Ref::Function;
        return false;
    }

    [[noreturn]] static void unreachable(const Piece &p, uint32_t k, const Reloc &rel) {
        std::fprintf(stderr, "stencil: %s reloc %u (kind %d) target %d/%u out of reach\n", p.name, k, int(rel.kind),
                     int(rel.target), rel.index);
        std::abort(); // inside one reservation of a gigabyte, with veneers where it is larger than a branch: never
    }

    // Writes a copy's holes. inst: what the per-copy holes stand for (nullptr
    // for a shared piece, which has none); next: the copy's successor; data:
    // its data block's address.
    void patch(uint8_t *at, uint16_t piece, size_t copied, const Inst *inst, const uint8_t *next, uint8_t *data) {
        const Piece &p = gen::pieces[piece];
        for (uint32_t k = 0; k < p.reloc_count; k++) {
            const Reloc &rel = gen::relocs[p.reloc_first + k];
            if (rel.offset + 4 > copied) continue; // the dropped tail jump's
            uint8_t *field = at + rel.offset;
            bool wrap = false;
            int64_t s = 0;
            uint8_t *island_veneer = nullptr; // a veneer to the same target, for a branch that does not reach
            switch (rel.target) {
            case TargetKind::Piece:
                s = int64_t(uintptr_t(shared_[rel.index]));
                island_veneer = far_piece_[rel.index];
                break;
            case TargetKind::Extern:
                s = int64_t(uintptr_t(externs_[rel.index]));
                island_veneer = far_extern_[rel.index];
                break;
            case TargetKind::Hole:
                switch (Hole(rel.index)) {
                case Hole::Word: s = inst->word, wrap = true; break;
                case Hole::A: s = inst->a, wrap = true; break;
                case Hole::B: s = inst->b, wrap = true; break;
                case Hole::Data:
                    if (inst->rw >= 0) {
                        // the writable data is placed after the code: written then
                        rw_fields_.push_back({field, size_t(inst->rw), rel.addend, rel.kind});
                        continue;
                    }
                    s = int64_t(uintptr_t(data));
                    break;
                case Hole::F0:
                case Hole::F1:
                case Hole::F2:
                case Hole::F3:
                case Hole::F4: s = inst->f[rel.index - uint16_t(Hole::F0)], wrap = true; break;
                case Hole::Next: s = int64_t(uintptr_t(next)); break;
                case Hole::Target:
                case Hole::Call: {
                    const Ref &ref = Hole(rel.index) == Hole::Target ? inst->target : inst->call;
                    uint8_t *veneer = far_call(rel, *inst) ? veneers_[next_veneer_++] : nullptr;
                    if (ref.kind == Ref::Function && !bodies_[ref.value]) {
                        pending_.push_back({field, ref.value, rel.addend, rel.kind, veneer});
                        continue;
                    }
                    s = int64_t(uintptr_t(resolve(ref)));
                    if (!place(field, rel.kind, s, rel.addend, veneer)) unreachable(p, k, rel);
                    continue;
                }
                default: break;
                }
                break;
            }
            if (!(force_far() && island_veneer && rel.kind == RelocKind::Branch26) &&
                write_reloc(field, rel.kind, s, rel.addend, wrap))
                continue;
            if (rel.kind != RelocKind::Branch26 || !island_veneer || rel.addend != 0 ||
                !write_reloc(field, rel.kind, int64_t(uintptr_t(island_veneer)), 0, false))
                unreachable(p, k, rel);
        }
    }

    // ---- one function ----

    const uint8_t *resolve(const Ref &ref) const {
        switch (ref.kind) {
        case Ref::Pre: return addr_[ref.value];
        case Ref::Main: return addr_[pre_.size() + ref.value];
        case Ref::Ool: return addr_[pre_.size() + main_.size() + ref.value];
        case Ref::Post: return addr_[pre_.size() + main_.size() + ool_.size() + ref.value];
        case Ref::Pc: return addr_[pre_.size() + label_.at(ref.value)];
        case Ref::Function: return reinterpret_cast<const uint8_t *>(bodies_[ref.value]);
        default: return nullptr;
        }
    }

    bool internal(uint32_t t) const {
        return t >= f_->address && t < end_ && std::binary_search(code_->begin(), code_->end(), t) &&
               (t == f_->address || !d_.functions.count(t));
    }
    // no tail call where the returns are checked or in a body with resume
    // sites (the emitter's go())
    bool framed() const { return checked_ || resumable_; }
    // the emitter's ret(): the return stencil returns EXIT_RETURN, which the
    // checking frame checks
    void ret(std::vector<Inst> &seq) { seq.push_back({ids_.ret}); }
    // A loop's back edge taken: counted for the function (Module::heat), then
    // the yield point
    void back_edge(std::vector<Inst> &seq) {
        Inst c{ids_.count};
        c.rw = int64_t(rw_.back_edges(k_));
        seq.push_back(c);
        seq.push_back({ids_.yield});
    }

    // recomp_emit.cpp's go(): a branch to `t` at the end of a sequence: a jump
    // inside the function, the stack switch, a tail call (a call and the
    // return where the returns are checked or the body has resume sites), or
    // a dispatch and the return
    void jump(std::vector<Inst> &seq, uint32_t t) {
        if (internal(t)) {
            seq.push_back({ids_.go, 0, 0, 0, {Ref::Pc, t}});
        } else if (d_.stack_thunks.count(t)) {
            seq.push_back({checked_ ? ids_.switch_tail_checked : ids_.switch_tail, 0, t});
        } else if (auto it = index_.find(t); it != index_.end()) {
            if (framed()) {
                Inst c{ids_.call};
                c.call = {Ref::Function, it->second};
                seq.push_back(c);
                ret(seq);
            } else {
                seq.push_back({ids_.go, 0, 0, 0, {Ref::Function, it->second}});
            }
        } else {
            seq.push_back({ids_.tail_dispatch, 0, t});
        }
    }
    // the same as a reference: the target itself when the branch is a jump,
    // else an out-of-line sequence doing it (None: a dispatch, where the
    // caller has one of its own: a jump table's default)
    Ref jump_ref(uint32_t t, bool back = false, bool dispatch_none = false) {
        if (!back) {
            if (internal(t)) return {Ref::Pc, t};
            auto it = index_.find(t);
            if (it != index_.end() && !framed() && !d_.stack_thunks.count(t)) return {Ref::Function, it->second};
            if (dispatch_none && it == index_.end() && !d_.stack_thunks.count(t)) return {};
        }
        std::vector<Inst> seq;
        if (back) back_edge(seq);
        jump(seq, t);
        return ool(std::move(seq));
    }
    // a call to `t`, LR already set to `next` (the emitter's call())
    void call(std::vector<Inst> &seq, uint32_t t, uint32_t next) {
        if (d_.stack_thunks.count(t)) {
            seq.push_back({ids_.switch_call, 0, t, next});
        } else if (auto it = index_.find(t); it != index_.end()) {
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
    int32_t block(Block b) {
        blocks_.push_back(std::move(b));
        return int32_t(blocks_.size() - 1);
    }
    // what follows an instruction that may fall through, when the next one is not emitted
    void fall_through(uint32_t pc) {
        const uint32_t next = pc + 4;
        if (std::binary_search(code_->begin(), code_->end(), next)) return;
        if (next >= end_) return jump(main_, next); // into the next function
        main_.push_back({ids_.invalid, 0, next});
    }

    // A mid-function hook before the instruction at pc (the emitter's
    // mid_hook): its slot, the labels the project file gave and the next
    // instruction, those inside the function.
    void mid_hook(uint32_t pc) {
        if (!hooks_) return;
        auto it = hooks_->mid.find(pc);
        if (it == hooks_->mid.end()) return;
        std::set<uint32_t> jumps(it->second.begin(), it->second.end());
        jumps.insert(pc + 4);
        std::erase_if(jumps, [&](uint32_t t) { return t == 0 || t == 1 || !internal(t); });
        ppc::HookSlot &slot = m_.slots_.emplace_back();
        slot.address = pc;
        Block b;
        b.u64(uint64_t(uintptr_t(&slot)));
        b.u64(jumps.size());
        for (uint32_t t : jumps) {
            b.u64(t);
            b.ref({Ref::Pc, t});
        }
        Inst m{ids_.mid};
        m.data = block(std::move(b));
        main_.push_back(m);
        stats_.mid_hooks++;
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
                call(main_, t, next);
                fall_through(pc);
            } else {
                // a branch backward is a yield point (a loop's back edge)
                if (t <= pc) back_edge(main_);
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
                call(seq, t, next);
                seq.push_back({ids_.go, 0, 0, 0, {Ref::Main, uint32_t(main_.size() + 1)}});
                b.target = ool(std::move(seq));
                main_.push_back(b);
                fall_through(pc);
                return;
            }
            const bool back = t <= pc;
            if (always) {
                if (back) back_edge(main_);
                jump(main_, t);
                return;
            }
            Inst b{ids_.bc[i.B.BO], 0, i.B.BI};
            b.target = jump_ref(t, back);
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
                ret(main_);
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
                // the cases' code by CTR's value, from the lowest on (none:
                // the stencil's dispatch and return, the emitter's default)
                const auto [lo, hi] = std::minmax_element(table->second.targets.begin(), table->second.targets.end());
                const uint32_t low = *lo, span = *hi - *lo + 4;
                std::vector<Ref> slots(span / 4);
                for (uint32_t target : std::set<uint32_t>(table->second.targets.begin(), table->second.targets.end()))
                    if (!((target - low) & 3)) slots[(target - low) / 4] = jump_ref(target, false, true);
                Block b;
                for (const Ref &r : slots) {
                    if (r.kind == Ref::None)
                        b.u64(0);
                    else
                        b.ref(r);
                }
                Inst s{ids_.table, 0, low, span};
                s.data = block(std::move(b));
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
            main_.push_back(semantics(op, i));
            stats_.variants += main_.back().piece != ids_.op[size_t(op)];
            fall_through(pc);
            return;
        }
    }

    // An instruction's semantics: the first of its variants that fits the
    // word, else the general stencil, with its register fields' holes.
    Inst semantics(Opcode op, Instr i) const {
        uint16_t piece = ids_.op[size_t(op)];
        for (const Ids::Variant &v : ids_.variants[size_t(op)])
            if (stencil::matches(v.v, i)) {
                piece = v.piece;
                break;
            }
        Inst r{piece, i.code};
        const ppc::Form form = ppc::info(op).form;
        for (int k = 0; k < stencil::field_count(form); k++) r.f[k] = stencil::get_field(form, k, i);
        return r;
    }

    // The addresses inside the function something may branch to (as the
    // emitter labels them): what a superinstruction may not swallow.
    void branch_targets() {
        targets_.clear();
        targets_.insert(f_->blocks.begin(), f_->blocks.end());
        for (uint32_t pc : *code_) {
            const Instr i{image_.word(pc)};
            const Opcode op = ppc::decode(i.code);
            if (op == Opcode::bx) targets_.insert(uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc));
            if (op == Opcode::bcx) targets_.insert(uint32_t(exts16(i.B.BD << 2)) + (i.B.AA ? 0 : pc));
            if (op == Opcode::bcctrx)
                if (auto t = d_.tables.find(pc); t != d_.tables.end())
                    targets_.insert(t->second.targets.begin(), t->second.targets.end());
            // a call's return lands after it
            if ((op == Opcode::bx && i.I.LK) || (op == Opcode::bcx && i.B.LK) ||
                ((op == Opcode::bclrx || op == Opcode::bcctrx) && i.XL.LK))
                targets_.insert(pc + 4);
        }
        targets_.insert(f_->resumes.begin(), f_->resumes.end());
        if (hooks_)
            for (const auto &[at, jumps] : hooks_->mid) {
                targets_.insert(at);
                targets_.insert(jumps.begin(), jumps.end());
            }
    }

    // A compare and the bc after it as one stencil (stencil_fields.h's
    // XF_STENCIL_CMP): the bc on a bit of the field the compare sets, taken on
    // the bit alone (no CTR), and nothing branching to it. True when made.
    // Whether the compare at pc and the bc after it can be one stencil
    // (stencil_fields.h's XF_STENCIL_CMP): the bc on a bit of the field the
    // compare sets, taken on the bit alone (no CTR), nothing branching to it.
    bool fusable(uint32_t pc) const {
        const Instr i{image_.word(pc)};
        const Opcode op = ppc::decode(i.code);
        if (op != Opcode::cmp && op != Opcode::cmpi && op != Opcode::cmpl && op != Opcode::cmpli) return false;
        const uint32_t next = pc + 4;
        if (targets_.count(next) || !std::binary_search(code_->begin(), code_->end(), next)) return false;
        const Instr b{image_.word(next)};
        if (ppc::decode(b.code) != Opcode::bcx || b.B.LK || (hooks_ && hooks_->mid.count(next))) return false;
        const uint32_t kind = b.B.BO & 0x1C; // the bit tested, CTR left alone
        return (kind == 0x04 || kind == 0x0C) && b.B.BI >> 2 == i.D.RT >> 2;
    }
    // The two as one stencil.
    void compare_and_branch(uint32_t pc) {
        const Instr i{image_.word(pc)};
        const Opcode op = ppc::decode(i.code);
        const int cmp = op == Opcode::cmp ? 0 : op == Opcode::cmpi ? 1 : op == Opcode::cmpl ? 2 : 3;
        const uint32_t next = pc + 4;
        const Instr b{image_.word(next)};
        const uint32_t t = uint32_t(exts16(b.B.BD << 2)) + (b.B.AA ? 0 : next);
        Inst s = semantics(op, i);
        s.piece = ids_.cmpbc[cmp][(b.B.BO & 0x1C) == 0x0C][b.B.BI & 3];
        s.target = jump_ref(t, t <= next);
        main_.push_back(s);
        stats_.fused++;
        fall_through(next);
    }

    // Options::count: a counter for the instruction at pc (its opcode kept)
    void count(uint32_t pc) {
        Inst c{ids_.count};
        c.rw = int64_t(rw_.instruction(m_.counter_op_.size()));
        m_.counter_op_.push_back(uint16_t(ppc::decode(image_.word(pc))));
        m_.counter_first_.push_back(uint32_t(m_.counter_pieces_.size()));
        main_.push_back(c);
    }

    // The layout: the C++ entry the kernel's table holds (and, for a hooked
    // function, the one its slot's original is), then its code: the frame
    // pieces (the entry's counter, the hook slot's check, the checking frame
    // or the resumable runner), the body, the out-of-line sequences, the C++
    // runner of a body with resume sites, then the data. Its slot in the table
    // of entries jumps to the code.
    bool function(size_t k) {
        const Plan &plan = plans_[k];
        k_ = k;
        f_ = plan.f;
        code_ = &plan.code;
        end_ = f_->address + std::max<uint32_t>(f_->size, 4);
        checked_ = f_->switches_stack;
        resumable_ = !plan.resumes.empty();
        hooks_ = plan.hooks;
        pre_.clear(), main_.clear(), ool_.clear(), post_.clear(), blocks_.clear(), label_.clear();
        // an entry counted (Module::heat)
        Inst entered{ids_.count};
        entered.rw = int64_t(rw_.entries(k));
        pre_.push_back(entered);
        ppc::HookSlot *slot = nullptr;
        if (hooks_ && hooks_->slot) {
            slot = &m_.slots_.emplace_back();
            slot->address = f_->address;
            slot->size = f_->size;
            Block b;
            b.u64(uint64_t(uintptr_t(slot)));
            Inst h{ids_.hooked};
            h.data = block(std::move(b));
            pre_.push_back(h);
            stats_.hooked++;
        }
        // where a hooked function's original enters: past the slot's check
        const uint32_t original = uint32_t(pre_.size());
        int32_t sites = -1;
        if (resumable_) {
            // the sites, then their code's addresses (stencils.cpp's resume_at)
            Block b;
            for (uint32_t r : plan.resumes) b.u32(r);
            b.align8();
            for (uint32_t r : plan.resumes) b.ref({Ref::Pc, r});
            sites = block(std::move(b));
            const uint32_t n = uint32_t(plan.resumes.size());
            Inst r{ids_.resumable, 0, n};
            r.target = {Ref::Post, 0};
            r.data = sites;
            pre_.push_back(r);
            Inst body{checked_ ? ids_.resume_body_checked : ids_.resume_body, 0, n};
            body.call = {Ref::Main, 0};
            body.data = sites;
            post_.push_back(body);
            m_.resumes_.insert(m_.resumes_.end(), plan.resumes.begin(), plan.resumes.end());
            stats_.resumable++;
        } else if (checked_) {
            Inst c{ids_.checked};
            c.call = {Ref::Main, 0};
            pre_.push_back(c);
        }
        if (checked_) stats_.checked++;
        branch_targets();
        for (size_t n = 0; n < code_->size(); n++) {
            const uint32_t pc = (*code_)[n];
            label_[pc] = uint32_t(main_.size());
            const bool fused = fusable(pc);
            if (options_.count) {
                // a counter for each instruction (two for a compare and branch)
                count(pc);
                if (fused) count(pc + 4);
            }
            const size_t made = main_.size();
            mid_hook(pc);
            if (fused) {
                compare_and_branch(pc);
                n++; // the bc, made with it
            } else {
                instruction(pc);
            }
            if (options_.count) {
                // the stencils made for it (the bc's, for a compare and branch)
                for (size_t j = made; j < main_.size(); j++)
                    if (main_[j].piece != ids_.count) m_.counter_pieces_.push_back(main_[j].piece);
            }
        }
        stats_.copies += pre_.size() + main_.size() + ool_.size() + post_.size() + 1;
        Region &r = m_.region_;
        const size_t n = pre_.size() + main_.size() + ool_.size() + post_.size();
        auto inst = [&](size_t j) -> const Inst & {
            if (j < pre_.size()) return pre_[j];
            j -= pre_.size();
            if (j < main_.size()) return main_[j];
            j -= main_.size();
            if (j < ool_.size()) return ool_[j];
            return post_[j - ool_.size()];
        };
        auto copied = [&](size_t j) {
            const Piece &p = gen::pieces[inst(j).piece];
            return (p.flags & PIECE_TAIL_JUMP) ? p.size - TAIL_JUMP : p.size;
        };
        // The C++ entries: the table's (GeneratedFunction::fn), among the
        // others after the table of entries (Compiler::run), so it stays when
        // the code is released (it calls the function's front); and a hooked
        // function's original, before its code, whose call into the body may
        // need a veneer as any call to a function's front.
        const size_t entry_size = gen::pieces[ids_.entry].size;
        const size_t originals = slot ? 1 : 0;
        Inst entry_inst{ids_.entry}, original_inst{ids_.entry};
        entry_inst.call = {Ref::Function, uint32_t(k)};
        entry_inst.close = true;
        original_inst.call = {Ref::Pre, original};
        // ARM64: a veneer kept after the code for each branch to another
        // function, used if it does not reach
        size_t far_calls = 0;
        if (ARM64) {
            auto count = [&](const Inst &i) {
                const Piece &p = gen::pieces[i.piece];
                for (uint32_t q = 0; q < p.reloc_count; q++) far_calls += far_call(gen::relocs[p.reloc_first + q], i);
            };
            if (originals) count(original_inst);
            for (size_t j = 0; j < n; j++) count(inst(j));
        }
        const size_t code_at = (originals * entry_size + 15) & ~size_t(15);
        size_t total = code_at;
        for (size_t j = 0; j < n; j++) total += copied(j);
        if (far_calls) total = ((total + 7) & ~size_t(7)) + far_calls * VENEER_SIZE;
        if (!island()) return false;
        const size_t start = r.used();
        uint8_t *entry = r.take(total, 16);
        if (!entry) return false;
        if (code_at > originals * entry_size)
            std::memset(entry + originals * entry_size, FILL, code_at - originals * entry_size);
        addr_.resize(n + 1);
        uint8_t *at = entry + code_at;
        for (size_t j = 0; j < n; j++) addr_[j] = at, at += copied(j);
        addr_[n] = at;
        veneers_.clear();
        next_veneer_ = 0;
        if (far_calls) {
            uint8_t *v = entry + (((at - entry) + 7) & ~size_t(7));
            std::memset(at, FILL, size_t(v - at));
            for (size_t j = 0; j < far_calls; j++) veneers_.push_back(v + j * VENEER_SIZE);
            std::memset(v, FILL, far_calls * VENEER_SIZE); // what is not used stays a trap
        }
        // its slot in the table of entries: a jump to its code
        uint8_t *front = reinterpret_cast<uint8_t *>(uintptr_t(bodies_[k]));
        const uint64_t jump = front_jump(front, entry + code_at, force_far());
        std::memcpy(front, &jump, FRONT_SIZE);
        // the data
        std::vector<uint8_t *> data(blocks_.size());
        for (size_t b = 0; b < blocks_.size(); b++) {
            data[b] = r.take(std::max<size_t>(blocks_[b].bytes.size(), 8), 8);
            if (!data[b]) return false;
            std::memcpy(data[b], blocks_[b].bytes.data(), blocks_[b].bytes.size());
            for (const auto &[offset, ref] : blocks_[b].refs) {
                const uint64_t v = uint64_t(uintptr_t(resolve(ref)));
                std::memcpy(data[b] + offset, &v, 8);
            }
        }
        // copied and patched (the unwind table is sorted at the end)
        uint8_t *table_entry = c_entries_ + c_entry_size_ * k;
        std::memcpy(table_entry, gen::bytes + gen::pieces[ids_.entry].offset, entry_size);
        patch(table_entry, ids_.entry, entry_size, &entry_inst, nullptr, nullptr);
        add_unwind(ids_.entry, table_entry, entry_size);
        if (originals) {
            std::memcpy(entry, gen::bytes + gen::pieces[ids_.entry].offset, entry_size);
            patch(entry, ids_.entry, entry_size, &original_inst, nullptr, nullptr);
            add_unwind(ids_.entry, entry, entry_size);
            if (options_.map) m_.copies_.push_back({uint32_t(entry - r.base()), ids_.entry});
        }
        for (size_t j = 0; j < n; j++) {
            const Inst &i = inst(j);
            const size_t size = copied(j);
            std::memcpy(addr_[j], gen::bytes + gen::pieces[i.piece].offset, size);
            patch(addr_[j], i.piece, size, &i, addr_[j + 1], i.data >= 0 ? data[size_t(i.data)] : nullptr);
            add_unwind(i.piece, addr_[j], size);
            if (options_.map) m_.copies_.push_back({uint32_t(addr_[j] - r.base()), i.piece});
        }
        stats_.code_bytes += r.used() - start;
        m_.fronts_.push_back({f_->address, uint32_t(k), uint32_t(code_->size()), front, start, r.used()});
        m_.functions_.push_back({f_->address, reinterpret_cast<void (*)(ppc::Context &)>(table_entry)});
        if (slot) slot->original = reinterpret_cast<void (*)(ppc::Context &)>(entry);
        return true;
    }

    struct Plan {
        const Function *f = nullptr;
        std::vector<uint32_t> code; // the walk's instructions, in order
        std::set<uint32_t> resumes; // its resume sites the walk reached
        const recomp::EmitHooks *hooks = nullptr;
    };

    const Image &image_;
    const Discovery &d_;
    const Options &options_;
    Stats &stats_;
    StencilModule &m_;
    const Ids &ids_;
    // the functions given code, in address order: what each is, its front's address
    std::vector<Plan> plans_;
    std::unordered_map<uint32_t, uint32_t> index_;
    std::vector<uint64_t> bodies_;
    std::vector<Pending> pending_;
    // the shared pieces: where the stencils reach each (its call counter's
    // stub, when counted), and where its bytes are
    std::vector<uint8_t *> shared_, placed_;
    std::vector<uint32_t> unwind_rva_;
    uint8_t *externs_[size_t(Extern::Count)] = {};
    RwLayout rw_;
    std::vector<RwField> rw_fields_;
    // ARM64: the latest island's veneers (none: the piece itself is near),
    // where it was laid out, and the function being made's own veneers
    std::vector<uint8_t *> far_piece_;
    uint8_t *far_extern_[size_t(Extern::Count)] = {};
    size_t island_at_ = 0;
    std::vector<uint8_t *> veneers_;
    size_t next_veneer_ = 0;
#if defined(_WIN32)
    std::vector<RUNTIME_FUNCTION> unwind_;
#endif
    uint8_t *c_entries_ = nullptr; // the C++ entries, c_entry_size_ apart, by function
    size_t c_entry_size_ = 0;
    // the function being made
    size_t k_ = 0;
    const Function *f_ = nullptr;
    const std::vector<uint32_t> *code_ = nullptr;
    uint32_t end_ = 0;
    bool checked_ = false, resumable_ = false;
    const recomp::EmitHooks *hooks_ = nullptr;
    std::vector<Inst> pre_, main_, ool_, post_;
    std::vector<Block> blocks_;
    std::unordered_map<uint32_t, uint32_t> label_;
    std::vector<uint8_t *> addr_;
    std::set<uint32_t> targets_; // branch_targets()
};

} // namespace

// The tier-up: an adapter per function the library has (xf_st_adopted: a call
// to the library's function, read from its data, which counts its calls),
// copied past everything else, sealed and its unwind information registered,
// then each entry made a jump to its adapter. A function adopted before (a
// partial library's, then the whole one's) keeps its adapter, whose data is
// pointed at the new library's function.
size_t StencilModule::adopt(const ppc::GeneratedModule &library, std::string &error) {
    if (library.base != module_.base || library.size != module_.size || library.digest != module_.digest) {
        error = "the library was made from another image";
        return 0;
    }
    const Piece &p = gen::pieces[adopted_piece_];
    struct Move {
        size_t front;
        void (*fn)(ppc::Context &) = nullptr;
        uint8_t *adapter = nullptr;
    };
    std::vector<Move> moves;
    size_t repointed = 0;
    for (size_t k = 0; k < fronts_.size(); k++) {
        const uint32_t address = fronts_[k].address;
        const ppc::GeneratedFunction *end = library.functions + library.count;
        const ppc::GeneratedFunction *it = std::lower_bound(
            library.functions, end, address, [](const ppc::GeneratedFunction &f, uint32_t a) { return f.address < a; });
        if (it == end || it->address != address || !it->fn) continue;
        if (adopted_[k]) {
            // (the threads are parked: the next call reads it)
            __atomic_store_n(&adopted_[k][0], uint64_t(uintptr_t(it->fn)), __ATOMIC_SEQ_CST);
            repointed++;
            continue;
        }
        moves.push_back({k, it->fn});
    }
    if (moves.empty()) return repointed;
    // the adapters' data (the library's function, its calls) on pages left
    // writable, then the adapters on pages of their own, sealed
    region_.align_to_page();
    uint8_t *records = region_.take(16 * moves.size(), region_.page());
    if (!records) return error = "out of the code's memory", 0;
    std::memset(records, 0, 16 * moves.size());
    region_.align_to_page();
    const size_t start = region_.used();
#if defined(_WIN32)
    std::vector<RUNTIME_FUNCTION> unwind;
#else
    EhFrame &eh = eh_frames_.emplace_back();
#endif
    for (size_t m = 0; m < moves.size(); m++) {
        uint8_t *record = records + 16 * m;
        const uint64_t fn = uint64_t(uintptr_t(moves[m].fn));
        std::memcpy(record, &fn, 8);
        uint8_t *adapter = region_.take(p.size, 16);
        if (!adapter) return error = "out of the code's memory", 0;
        std::memcpy(adapter, gen::bytes + p.offset, p.size);
        for (uint32_t r = 0; r < p.reloc_count; r++) {
            const Reloc &rel = gen::relocs[p.reloc_first + r];
            int64_t s = 0;
            if (rel.target == TargetKind::Hole && Hole(rel.index) == Hole::Data)
                s = int64_t(uintptr_t(record));
            else if (rel.target == TargetKind::Extern && rel.index == uint16_t(Extern::Runtime))
                s = int64_t(uintptr_t(region_.base()));
            else
                return error = "the adapter stencil refers to what the tier-up does not provide", 0;
            if (!write_reloc(adapter + rel.offset, rel.kind, s, rel.addend, false))
                return error = "an adapter out of the code's reach", 0;
        }
#if defined(_WIN32)
        if (p.unwind_size) {
            RUNTIME_FUNCTION f;
            f.BeginAddress = DWORD(adapter - region_.base());
            f.EndAddress = DWORD(adapter - region_.base() + p.size);
            f.UnwindData = adopted_unwind_rva_;
            unwind.push_back(f);
        }
#else
        eh.add(p, adapter, p.size);
#endif
        moves[m].adapter = adapter;
    }
    if (!region_.seal(start, region_.used())) return error = "could not make the adapters executable", 0;
#if defined(_WIN32)
    if (!unwind.empty()) {
        unwind_tables_.push_back(std::move(unwind));
        auto &table = unwind_tables_.back();
        if (!RtlAddFunctionTable(table.data(), DWORD(table.size()), DWORD64(uintptr_t(region_.base())))) {
            unwind_tables_.pop_back();
            return error = "could not register the adapters' unwind information", 0;
        }
    }
#else
    if (!eh.install()) return error = "could not register the adapters' unwind information", 0;
#endif
    // the entries: each one's eight bytes stored at once (the threads are
    // parked; the table stays executable while it is written)
    const size_t page = region_.page();
    const size_t table_begin = entries_begin_ & ~(page - 1), table_end = entries_begin_ + fronts_.size() * ENTRY_SIZE;
    if (!region_.writable_code(table_begin, table_end, true)) return error = "could not write the code", 0;
    for (size_t m = 0; m < moves.size(); m++) {
        uint8_t *front = fronts_[moves[m].front].front;
        __atomic_store_n(reinterpret_cast<uint64_t *>(front), front_jump(front, moves[m].adapter, force_far()),
                         __ATOMIC_SEQ_CST);
        adopted_[moves[m].front] = reinterpret_cast<uint64_t *>(records + 16 * m);
    }
    if (!region_.writable_code(table_begin, table_end, false)) return error = "could not seal the code again", 0;
    return moves.size() + repointed;
}

std::vector<FunctionHeat> StencilModule::heat() const {
    std::vector<FunctionHeat> h;
    h.reserve(fronts_.size());
    const uint64_t *counts = reinterpret_cast<const uint64_t *>(rw_ + rw_layout_.entries(0));
    for (size_t k = 0; k < fronts_.size(); k++) {
        const Front &f = fronts_[k];
        FunctionHeat e;
        e.address = f.address, e.instructions = f.instructions;
        e.entries = __atomic_load_n(&counts[2 * f.index], __ATOMIC_RELAXED);
        e.back_edges = __atomic_load_n(&counts[2 * f.index + 1], __ATOMIC_RELAXED);
        e.adopted = adopted_[k] != nullptr;
        e.library_calls = adopted_[k] ? __atomic_load_n(&adopted_[k][1], __ATOMIC_RELAXED) : 0;
        e.released = released_[k] != 0;
        h.push_back(e);
    }
    return h;
}

std::string StencilModule::count_report(size_t top, const std::string &pieces_file) const {
    if (!counter_) return {};
    // each piece's copies run (a guest instruction's main path: what runs
    // when its branches are not taken), and their bytes
    std::vector<uint64_t> runs(gen::piece_count, 0);
    uint64_t instructions = 0, bytes = 0;
    for (size_t k = 0; k < counter_op_.size(); k++) {
        const uint64_t n = counter_[k];
        instructions += n;
        if (!n) continue;
        for (uint32_t j = counter_first_[k]; j < counter_first_[k + 1]; j++) {
            const Piece &p = gen::pieces[counter_pieces_[j]];
            runs[counter_pieces_[j]] += n;
            bytes += n * ((p.flags & PIECE_TAIL_JUMP) ? p.size - TAIL_JUMP : p.size);
        }
    }
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "copy-and-patch tier: %llu guest instructions, %.1f bytes of stencils run a guest instruction (each "
                  "instruction's main path)\n",
                  (unsigned long long)instructions, instructions ? double(bytes) / double(instructions) : 0.0);
    out += line;
    std::vector<std::pair<uint64_t, size_t>> by_bytes;
    for (size_t k = 0; k < gen::piece_count; k++)
        if (runs[k]) by_bytes.push_back({runs[k] * gen::pieces[k].size, k});
    std::sort(by_bytes.rbegin(), by_bytes.rend());
    out += "  the stencils that ran the most bytes (runs, bytes each):\n";
    for (size_t k = 0; k < by_bytes.size() && k < top; k++) {
        const Piece &p = gen::pieces[by_bytes[k].second];
        std::snprintf(line, sizeof(line), "    %5.1f%%  %-28s %12llu  %u\n",
                      bytes ? 100.0 * double(by_bytes[k].first) / double(bytes) : 0.0, p.name,
                      (unsigned long long)runs[by_bytes[k].second], p.size);
        out += line;
    }
    // the shared helpers' calls (the stencils' slow paths)
    const uint64_t *calls = reinterpret_cast<const uint64_t *>(rw_ + rw_layout_.helper(0));
    std::vector<std::pair<uint64_t, uint16_t>> by_calls;
    for (size_t k = 0; k < helpers_.size(); k++)
        if (calls[k]) by_calls.push_back({calls[k], helpers_[k]});
    std::sort(by_calls.rbegin(), by_calls.rend());
    out += "  the shared helpers' calls (slow paths):\n";
    for (size_t k = 0; k < by_calls.size() && k < top; k++) {
        std::snprintf(line, sizeof(line), "    %12llu  %.120s\n", (unsigned long long)by_calls[k].first,
                      gen::pieces[by_calls[k].second].name);
        out += line;
    }
    if (!pieces_file.empty()) {
        // every stencil that ran, its runs and bytes; the helpers' calls; the opcodes' counts
        if (FILE *f = std::fopen(pieces_file.c_str(), "w")) {
            for (size_t k = 0; k < gen::piece_count; k++)
                if (runs[k])
                    std::fprintf(f, "piece %s %llu %u\n", gen::pieces[k].name, (unsigned long long)runs[k],
                                 gen::pieces[k].size);
            for (const auto &[n, k] : by_calls)
                std::fprintf(f, "helper %s %llu\n", gen::pieces[k].name, (unsigned long long)n);
            const uint64_t *ops = executed_by_opcode();
            for (uint32_t k = 0; k < ppc::OPCODE_COUNT; k++)
                if (ops[k]) std::fprintf(f, "op %s %llu\n", ppc::info(Opcode(k)).name, (unsigned long long)ops[k]);
            std::fclose(f);
        }
    }
    return out;
}

bool StencilModule::describe(uint64_t address, std::string &name, uint64_t &displacement) const {
    const uintptr_t base = uintptr_t(region_.base());
    if (address < base || address >= base + region_.used()) return false;
    const uint32_t offset = uint32_t(address - base);
    if (!copies_.empty()) {
        // the stencil (or shared piece) the address is in
        auto it = std::upper_bound(copies_.begin(), copies_.end(), std::make_pair(offset, uint16_t(0xFFFF)));
        if (it != copies_.begin()) {
            --it;
            const Piece &p = gen::pieces[it->second];
            if (offset < it->first + p.size) {
                name = std::string("tier ") + p.name, displacement = offset - it->first;
                return true;
            }
        }
    }
    // else the function
    auto it = std::upper_bound(fronts_.begin(), fronts_.end(), offset,
                               [](uint32_t o, const Front &f) { return o < f.begin; });
    // (fronts_ is by guest address, which is the code's order too)
    if (it != fronts_.begin() && offset < std::prev(it)->end) {
        char text[32];
        std::snprintf(text, sizeof(text), "tier 0x%08X", std::prev(it)->address);
        name = text, displacement = offset - std::prev(it)->begin;
        return true;
    }
    name = offset >= code_end_ ? "tier (adapters)" : offset < entries_begin_ ? "tier (shared)" : "tier (entries)";
    displacement = 0;
    return true;
}

// The code of the functions the library took and no frame is in (no address
// in `live` inside theirs), released: whole pages only, those a function
// still in use does not share; their unwind entries dropped. They stay
// released: their entries jump to the library from now on, so no new frame
// can be in them. Only while every guest thread is parked.
size_t StencilModule::release(const std::vector<uintptr_t> &live, ReleaseStats &stats, std::string &error) {
    const uintptr_t base = uintptr_t(region_.base());
    std::vector<uint32_t> offsets;
    for (uintptr_t a : live)
        if (a >= base && a < base + code_end_) offsets.push_back(uint32_t(a - base));
    std::sort(offsets.begin(), offsets.end());
    auto in = [&](const Front &f) {
        auto it = std::lower_bound(offsets.begin(), offsets.end(), uint32_t(f.begin));
        return it != offsets.end() && *it < f.end;
    };
    stats = {};
    std::vector<uint8_t> dead(fronts_.size(), 0);
    for (size_t k = 0; k < fronts_.size(); k++) {
        if (released_[k]) {
            dead[k] = 1;
            continue;
        }
        if (!adopted_[k]) {
            stats.not_adopted++;
            continue;
        }
        if (in(fronts_[k])) {
            stats.live++;
            continue;
        }
        dead[k] = 1, released_[k] = 1;
        stats.released++;
    }
    // the runs of dead functions next to each other (between two, only the
    // fill their alignment leaves: on ARM64 an island of veneers may sit
    // there, which stays), their whole pages decommitted
    const size_t page = region_.page();
    if (decommitted_.empty()) decommitted_.assign((code_end_ + page - 1) / page, 0);
    size_t freed = 0;
    for (size_t k = 0; k < fronts_.size();) {
        if (!dead[k]) {
            k++;
            continue;
        }
        size_t begin = fronts_[k].begin, end = fronts_[k].end, j = k + 1;
        for (; j < fronts_.size() && dead[j] && fronts_[j].begin >= end && fronts_[j].begin - end < 64; j++)
            end = fronts_[j].end;
        k = j;
        const size_t first = (begin + page - 1) / page, last = end / page;
        for (size_t q = first; q < last;) {
            if (decommitted_[q]) {
                q++;
                continue;
            }
            size_t r = q;
            while (r < last && !decommitted_[r]) decommitted_[r++] = 1;
            uint8_t *at = region_.base() + q * page;
            const size_t size = (r - q) * page;
#if defined(_WIN32)
            if (!VirtualFree(at, size, MEM_DECOMMIT)) return error = "could not decommit the code", freed;
#else
            if (mmap(at, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) == MAP_FAILED)
                return error = "could not release the code", freed;
#endif
            freed += size;
            q = r;
        }
    }
    released_bytes_ += freed;
    // the unwind entries of the dead functions' copies, dropped
    auto dead_at = [&](uint64_t offset) {
        auto it = std::upper_bound(fronts_.begin(), fronts_.end(), offset,
                                   [](uint64_t o, const Front &f) { return o < f.begin; });
        if (it == fronts_.begin()) return false;
        --it;
        return offset < it->end && dead[size_t(it - fronts_.begin())] != 0;
    };
#if defined(_WIN32)
    if (!unwind_tables_.empty() && !unwind_tables_.front().empty()) {
        std::vector<RUNTIME_FUNCTION> kept;
        for (const RUNTIME_FUNCTION &f : unwind_tables_.front())
            if (!dead_at(f.BeginAddress)) kept.push_back(f);
        if (kept.size() != unwind_tables_.front().size()) {
            stats.unwind_dropped = unwind_tables_.front().size() - kept.size();
            RtlDeleteFunctionTable(unwind_tables_.front().data());
            unwind_tables_.front() = std::move(kept);
            if (!unwind_tables_.front().empty() &&
                !RtlAddFunctionTable(unwind_tables_.front().data(), DWORD(unwind_tables_.front().size()),
                                     DWORD64(base)))
                return error = "could not register the code's unwind information again", freed;
        }
    }
#else
    if (!eh_frames_.empty())
        stats.unwind_dropped = eh_frames_.front().drop([&](uint64_t pc) { return pc >= base && dead_at(pc - base); });
#endif
    stats.bytes = freed;
    return freed;
}

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
    return true;
#elif defined(__aarch64__) && !defined(_WIN32)
    // ARM64's baseline (ELF: Linux, Android), its unwind information from .eh_frame
    return true;
#else
    // Windows on ARM64: its .pdata (packed unwind entries) is not read yet
    if (why) *why = "the stencils' unwind information is only read for x86-64 and ELF on ARM64 yet";
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

// recomp_module.cpp's plan(): the project's starts and hooked functions are
// functions, its jump tables tables
void project_discovery(const recomp::ProjectModule &pm, recomp::DiscoverOptions &options) {
    for (const auto &[a, end] : pm.starts) options.functions.emplace(a, end);
    for (uint32_t a : pm.hooks) options.functions.emplace(a, 0);
    for (const auto &[bctr, targets] : pm.jump_tables) options.jump_tables[bctr] = targets;
}

// recomp_module.cpp's apply_project(), its hooks: a slot for each hooked
// function that has code (not an import, not skipped), each mid-function hook
// in the function holding it
Project project_hooks(const recomp::ProjectModule &pm, const recomp::Discovery &d) {
    Project p;
    p.skip = pm.skip;
    for (uint32_t a : pm.hooks) {
        auto it = d.functions.find(a);
        if (it == d.functions.end() || it->second.source == recomp::Source::Import || p.skip.count(a)) continue;
        p.hooks[a].slot = true;
    }
    for (const auto &[a, jumps] : pm.mid_hooks) {
        auto it = d.functions.upper_bound(a);
        if (it == d.functions.begin() ||
            a >= std::prev(it)->first + std::max<uint32_t>(std::prev(it)->second.size, 4) ||
            std::prev(it)->second.source == recomp::Source::Import)
            continue;
        p.hooks[std::prev(it)->first].mid[a] = jumps;
    }
    return p;
}

} // namespace xf::stencil
