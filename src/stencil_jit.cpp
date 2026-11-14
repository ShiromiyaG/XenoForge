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
    Ref target, call;
    int32_t data = -1;    // its data block, in the function's
    bool counter = false; // xf_st_count: data is the counter
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

// A function's entry as the other functions' code reaches it: eight bytes of
// no-ops before what runs (its body, or the frame around it), which the
// tier-up turns into a jump to the game library's code (Module::adopt).
// Calls and tail branches between functions given code all go there; a
// branch inside the function to its own start does not. ARM64: two nops, and
// before them an 8-byte literal the tier-up fills when its jump is indirect.
constexpr std::array<uint8_t, 8> FRONT = ARM64 ? std::array<uint8_t, 8>{0x1F, 0x20, 0x03, 0xD5, 0x1F, 0x20, 0x03, 0xD5}
                                               // nop dword [rax+rax]; nop dword [rax]
                                               : std::array<uint8_t, 8>{0x0F, 0x1F, 0x44, 0x00, 0x00, 0x0F, 0x1F, 0x00};
constexpr size_t FRONT_SIZE = FRONT.size();
constexpr size_t FRONT_LITERAL = ARM64 ? 8 : 0;

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
    size_t adopt(const ppc::GeneratedModule &library, std::string &error) override;
    ~StencilModule() override {
#if defined(_WIN32)
        for (auto &table : unwind_tables_)
            if (!table.empty()) RtlDeleteFunctionTable(table.data());
#endif
    }

  private:
    friend class Compiler;
    Region region_;
    std::string name_;
    std::vector<ppc::GeneratedFunction> functions_;
    std::vector<uint32_t> resumes_;
    std::deque<ppc::HookSlot> slots_;         // the hook slots (hooks::bind fills them)
    std::vector<ppc::HookSlot *> slot_table_; // their table, as a game library's
    ppc::GeneratedModule module_{};
    uint64_t *counter_ = nullptr;
    size_t code_end_ = 0;                                // [0, code_end_): the code, sealed
    std::vector<std::pair<uint32_t, uint8_t *>> fronts_; // each function's front, by guest address
    std::vector<uint8_t> adopted_;                       // the fronts that jump to a library's code now
    uint16_t adopted_piece_ = 0;
    uint32_t adopted_unwind_rva_ = 0; // its unwind information, copied with the shared pieces
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
        if (!header(runtime)) return error = "out of the code's memory", false;
        for (size_t k = 0; k < plans_.size(); k++)
            if (!function(k)) return error = "out of the code's memory", false;
        for (const Pending &p : pending_)
            if (!place(p.field, p.kind, int64_t(bodies_[p.function]), p.addend, p.veneer))
                return error = "a call out of the code's reach", false;
        const size_t code_end = m_.region_.used();
        m_.code_end_ = code_end;
        if (options_.count) {
            // the counter on a page of its own, left writable
            uint8_t *page = m_.region_.take(m_.region_.page(), m_.region_.page());
            if (!page) return error = "out of the code's memory", false;
            std::memset(page, 0, m_.region_.page());
            m_.counter_ = reinterpret_cast<uint64_t *>(page);
            // the count stencils' data holes, written now that it exists
            for (const Pending &p : counter_fields_)
                if (!write_reloc(p.field, p.kind, int64_t(uintptr_t(page)) + 8 * int64_t(p.function), p.addend, false))
                    return error = "the counter out of the code's reach", false;
        }
        if (!m_.region_.seal(0, code_end)) return error = "could not make the code executable", false;
#if defined(_WIN32)
        if (!unwind_.empty()) {
            m_.unwind_tables_.push_back(std::move(unwind_));
            auto &table = m_.unwind_tables_.back();
            if (!RtlAddFunctionTable(table.data(), DWORD(table.size()), DWORD64(uintptr_t(m_.region_.base()))))
                return error = "could not register the code's unwind information", false;
            stats_.unwind_entries = table.size();
        }
#else
        stats_.unwind_entries = m_.eh_frames_.front().fdes();
        if (!m_.eh_frames_.front().install()) return error = "could not register the code's unwind information", false;
#endif
        stats_.region_bytes = m_.region_.used();
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
        m_.adopted_.assign(m_.fronts_.size(), 0);
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
    // A field written once its target is placed: a function's front not made
    // yet (function: its index), or the counter (function: the opcode).
    struct Pending {
        uint8_t *field;
        uint32_t function;
        int64_t addend;
        RelocKind kind;
        uint8_t *veneer; // ARM64: a veneer kept for a branch that may not reach
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
        if (rel.kind != RelocKind::Branch26 || rel.target != TargetKind::Hole) return false;
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
                    if (inst->counter) {
                        // the counter's page is placed at the end: written then
                        counter_fields_.push_back({field, inst->word, rel.addend, rel.kind, nullptr});
                        continue;
                    }
                    s = int64_t(uintptr_t(data));
                    break;
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
        if (back) seq.push_back({ids_.yield});
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
        mid_hook(pc);
        switch (op) {
        case Opcode::bx: {
            const uint32_t t = uint32_t(exts26(i.I.LI << 2)) + (i.I.AA ? 0 : pc);
            if (i.I.LK) {
                main_.push_back({ids_.set_lr, 0, next});
                call(main_, t, next);
                fall_through(pc);
            } else {
                // a branch backward is a yield point (a loop's back edge)
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
                call(seq, t, next);
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
            main_.push_back({ids_.op[size_t(op)], i.code});
            fall_through(pc);
            return;
        }
    }

    // The layout: the C++ entry the kernel's table holds (and, for a hooked
    // function, the one its slot's original is), the front, the frame pieces
    // (the hook slot's check, the checking frame or the resumable runner),
    // the body, the out-of-line sequences, the C++ runner of a body with
    // resume sites, then the data.
    bool function(size_t k) {
        const Plan &plan = plans_[k];
        f_ = plan.f;
        code_ = &plan.code;
        end_ = f_->address + std::max<uint32_t>(f_->size, 4);
        checked_ = f_->switches_stack;
        resumable_ = !plan.resumes.empty();
        hooks_ = plan.hooks;
        pre_.clear(), main_.clear(), ool_.clear(), post_.clear(), blocks_.clear(), label_.clear();
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
        // the entries (the table's, whose call to the body may need a veneer
        // as any call to a function's front, and a hooked function's original)
        const size_t entry_size = gen::pieces[ids_.entry].size;
        const size_t entries = slot ? 2 : 1;
        Inst entry_inst[2] = {Inst{ids_.entry}, Inst{ids_.entry}};
        entry_inst[0].call = {Ref::Function, uint32_t(k)};
        entry_inst[1].call = {Ref::Pre, 1};
        // ARM64: a veneer kept after the code for each branch to another
        // function, used if it does not reach
        size_t far_calls = 0;
        if (ARM64) {
            auto count = [&](const Inst &i) {
                const Piece &p = gen::pieces[i.piece];
                for (uint32_t q = 0; q < p.reloc_count; q++) far_calls += far_call(gen::relocs[p.reloc_first + q], i);
            };
            for (size_t e = 0; e < entries; e++) count(entry_inst[e]);
            for (size_t j = 0; j < n; j++) count(inst(j));
        }
        const size_t front_at = ((entries * entry_size + 7) & ~size_t(7)) + FRONT_LITERAL;
        size_t total = front_at + FRONT_SIZE;
        for (size_t j = 0; j < n; j++) total += copied(j);
        if (far_calls) total = ((total + 7) & ~size_t(7)) + far_calls * VENEER_SIZE;
        if (!island()) return false;
        const size_t start = r.used();
        uint8_t *entry = r.take(total, 16);
        if (!entry) return false;
        if (front_at > entries * entry_size)
            std::memset(entry + entries * entry_size, FILL, front_at - entries * entry_size);
        uint8_t *front = entry + front_at;
        std::memcpy(front, FRONT.data(), FRONT_SIZE);
        addr_.resize(n + 1);
        uint8_t *at = front + FRONT_SIZE;
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
        bodies_[k] = uint64_t(uintptr_t(front));
        m_.fronts_.push_back({f_->address, front});
        // the data
        std::vector<uint8_t *> data(blocks_.size());
        for (size_t b = 0; b < blocks_.size(); b++) {
            data[b] = r.take(std::max<size_t>(blocks_[b].bytes.size(), 8), 8);
            if (!data[b]) return false;
            std::memcpy(data[b], blocks_[b].bytes.data(), blocks_[b].bytes.size());
            for (const auto &[offset, ref] : blocks_[b].refs) {
                if (ref.kind == Ref::Function && !bodies_[ref.value]) {
                    pending_.push_back({data[b] + offset, ref.value, 0, RelocKind::Abs64, nullptr});
                    continue;
                }
                const uint64_t v = uint64_t(uintptr_t(resolve(ref)));
                std::memcpy(data[b] + offset, &v, 8);
            }
        }
        // copied and patched, in address order (the unwind table's)
        for (size_t e = 0; e < entries; e++) {
            uint8_t *copy = entry + e * entry_size;
            std::memcpy(copy, gen::bytes + gen::pieces[ids_.entry].offset, entry_size);
            // the table's entry: the front; a hooked function's original: past the slot's check
            patch(copy, ids_.entry, entry_size, &entry_inst[e], nullptr, nullptr);
            add_unwind(ids_.entry, copy, entry_size);
        }
        for (size_t j = 0; j < n; j++) {
            const Inst &i = inst(j);
            const size_t size = copied(j);
            std::memcpy(addr_[j], gen::bytes + gen::pieces[i.piece].offset, size);
            patch(addr_[j], i.piece, size, &i, addr_[j + 1], i.data >= 0 ? data[size_t(i.data)] : nullptr);
            add_unwind(i.piece, addr_[j], size);
        }
        stats_.code_bytes += r.used() - start;
        m_.functions_.push_back({f_->address, reinterpret_cast<void (*)(ppc::Context &)>(entry)});
        if (slot) slot->original = reinterpret_cast<void (*)(ppc::Context &)>(entry + entry_size);
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
    std::vector<uint8_t *> shared_;
    std::vector<uint32_t> unwind_rva_;
    uint8_t *externs_[size_t(Extern::Count)] = {};
    std::vector<Pending> counter_fields_; // function: the opcode each counts
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
    // the function being made
    const Function *f_ = nullptr;
    const std::vector<uint32_t> *code_ = nullptr;
    uint32_t end_ = 0;
    bool checked_ = false, resumable_ = false;
    const recomp::EmitHooks *hooks_ = nullptr;
    std::vector<Inst> pre_, main_, ool_, post_;
    std::vector<Block> blocks_;
    std::unordered_map<uint32_t, uint32_t> label_;
    std::vector<uint8_t *> addr_;
};

} // namespace

// The tier-up: an adapter per function the library has (xf_st_adopted: a call
// to the library's function), copied past everything else, sealed and its
// unwind information registered, then each front made a jump to its adapter.
size_t StencilModule::adopt(const ppc::GeneratedModule &library, std::string &error) {
    if (library.base != module_.base || library.size != module_.size || library.digest != module_.digest) {
        error = "the library was made from another image";
        return 0;
    }
    const Piece &p = gen::pieces[adopted_piece_];
    // the adapters on pages of their own, past the sealed code and the counter
    region_.align_to_page();
    const size_t start = region_.used();
    struct Move {
        size_t front;
        uint8_t *adapter;
    };
    std::vector<Move> moves;
#if defined(_WIN32)
    std::vector<RUNTIME_FUNCTION> unwind;
#else
    EhFrame &eh = eh_frames_.emplace_back();
#endif
    for (size_t k = 0; k < fronts_.size(); k++) {
        if (adopted_[k]) continue;
        const uint32_t address = fronts_[k].first;
        const ppc::GeneratedFunction *end = library.functions + library.count;
        const ppc::GeneratedFunction *it = std::lower_bound(
            library.functions, end, address, [](const ppc::GeneratedFunction &f, uint32_t a) { return f.address < a; });
        if (it == end || it->address != address || !it->fn) continue;
        // the library function's address, then the adapter reading it
        uint8_t *slot = region_.take(8, 8);
        uint8_t *adapter = region_.take(p.size, 16);
        if (!slot || !adapter) return error = "out of the code's memory", 0;
        const uint64_t fn = uint64_t(uintptr_t(it->fn));
        std::memcpy(slot, &fn, 8);
        std::memcpy(adapter, gen::bytes + p.offset, p.size);
        for (uint32_t r = 0; r < p.reloc_count; r++) {
            const Reloc &rel = gen::relocs[p.reloc_first + r];
            int64_t s = 0;
            if (rel.target == TargetKind::Hole && Hole(rel.index) == Hole::Data)
                s = int64_t(uintptr_t(slot));
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
        moves.push_back({k, adapter});
    }
    if (moves.empty()) return 0;
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
    // the fronts: each one's eight bytes stored at once (the threads are
    // parked; the code stays executable while it is written). x86-64: a jmp
    // rel32 to the adapter and the rest of the no-ops. ARM64: a b to it and a
    // nop, or, farther than a b reaches, ldr x16 from the literal before the
    // front (written first) and br x16.
    if (!region_.writable_code(0, code_end_, true)) return error = "could not write the code", 0;
    for (const Move &m : moves) {
        uint8_t *front = fronts_[m.front].second;
        uint8_t bytes[8];
        std::memcpy(bytes, FRONT.data(), 8);
        if (ARM64) {
            const int64_t d = int64_t(uintptr_t(m.adapter)) - int64_t(uintptr_t(front));
            if (d >= -(int64_t(1) << 27) && d < (int64_t(1) << 27) && !force_far()) {
                const uint32_t b = 0x14000000u | (uint32_t(d >> 2) & 0x03FFFFFFu);
                std::memcpy(bytes, &b, 4);
            } else {
                const uint64_t a = uint64_t(uintptr_t(m.adapter));
                __atomic_store_n(reinterpret_cast<uint64_t *>(front - 8), a, __ATOMIC_SEQ_CST);
                const uint32_t code[2] = {0x58FFFFD0u, 0xD61F0200u}; // ldr x16, #-8; br x16
                std::memcpy(bytes, code, 8);
            }
        } else {
            bytes[0] = 0xE9;
            const int64_t v = int64_t(uintptr_t(m.adapter)) - int64_t(uintptr_t(front + 5));
            const int32_t w = int32_t(v);
            std::memcpy(bytes + 1, &w, 4);
        }
        uint64_t word;
        std::memcpy(&word, bytes, 8);
        __atomic_store_n(reinterpret_cast<uint64_t *>(front), word, __ATOMIC_SEQ_CST);
        adopted_[m.front] = 1;
    }
    if (!region_.writable_code(0, code_end_, false)) return error = "could not seal the code again", 0;
    return moves.size();
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
