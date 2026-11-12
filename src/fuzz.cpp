// The differential fuzzer's generator and runner (xenoforge/fuzz.h).

#include "xenoforge/fuzz.h"

#include "xenoforge/ppc_interp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace xf::ppc::fuzz {

namespace {

struct Rng {
    std::mt19937_64 engine;
    uint64_t next() { return engine(); }
    uint32_t below(uint32_t n) { return uint32_t(next() % n); }
    bool percent(uint32_t p) { return below(100) < p; }
    template <class T, size_t N> T pick(const T (&a)[N]) { return a[below(N)]; }
};

const uint64_t GPR_EDGES[] = {
    0,
    1,
    2,
    0xFFFFFFFFFFFFFFFFull,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFF,
    0x100000000ull,
    0xFFFFFFFF80000000ull,
    0x00000000FFFF8000ull,
    0x7FFFFFFFFFFFFFFFull,
    0x8000000000000000ull,
    0xFFFFFFFF7FFFFFFFull,
    0x0000000080000001ull,
    0x8000,
    0x7FFF,
    0xFFFF,
    0x10000,
    31,
    32,
    63,
    64,
    0xFFFFFFFFFFFFFFFEull,
    0x00FF00FF00FF00FFull,
};

const uint64_t DOUBLE_EDGES[] = {
    0x0000000000000000ull, 0x8000000000000000ull, // zeros
    0x3FF0000000000000ull, 0xBFF0000000000000ull, // 1, -1
    0x7FF0000000000000ull, 0xFFF0000000000000ull, // infinities
    0x7FF8000000000000ull, 0xFFF8000000000000ull, // the default NaN, negated
    0x7FF8000012345678ull, 0xFFFC00000000BEEFull, // quiet NaNs with payloads
    0x7FF0000000000001ull, 0xFFF4000000000001ull, // signalling NaNs
    0x7FF7FFFFE0000000ull,                        // a signalling NaN a single keeps
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, // double denormals
    0x0008000000000000ull,                        //
    0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, // smallest normal, largest
    0x3810000000000000ull, 0x3800000000000000ull, // 2^-126 (single's smallest normal), 2^-127 (single denormal)
    0x36A0000000000000ull, 0xB690000000000000ull, // 2^-149 (smallest single denormal), below it
    0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, // FLT_MAX, past it (rounds to infinity)
    0x47F0000000000000ull,                        // 2^128
    0x3FF0000010000000ull, 0x3FF0000030000000ull, // single rounding ties (even, odd)
    0x3FF00000F0000000ull,                        //
    0x3FE0000000000000ull, 0x3FF8000000000000ull, // 0.5, 1.5
    0x4004000000000000ull, 0xC004000000000000ull, // 2.5, -2.5
    0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, // 2^31 - 1, 2^31
    0xC1E0000000000000ull, 0xC1E0000000100000ull, // -2^31, below it
    0x43E0000000000000ull, 0xC3E0000000000000ull, // 2^63, -2^63
    0x4330000000000000ull,                        // 2^52
};

const uint32_t FLOAT_EDGES[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, // zeros, ones, infinities
    0x7FC00000, 0xFFC00000, 0x7FC12345, 0xFFFFFFFF,                         // quiet NaNs
    0x7F800001, 0xFFA00000, 0x7FBFFFFF,                                     // signalling NaNs
    0x00000001, 0x807FFFFF, 0x00400000,                                     // denormals
    0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF,                         // smallest normal, largest
    0x3F000000, 0x3FC00000, 0xBFC00000, 0x40200000,                         // 0.5, 1.5, -1.5, 2.5
    0x4EFFFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0xCF000001,             // around 2^31, 2^32
    0x40400000, 0x40400001, 0x404FFFFF, 0x40407FFF,                         // 3.0 + x (the D3D packs)
    0x47000000, 0x477FE000, 0x477FF000, 0x38800000, 0x387FFFFF,             // half's range
};

const uint32_t WORD_EDGES[] = {
    0x00000000, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x00007FFF, 0x00008000, 0x0000FFFF, 0x0000007F,
    0x00000080, 0x000000FF, 0x7F7F7F7F, 0x80808080, 0x00010001, 0x7FFF7FFF, 0x80008000, 0xFFFF0000,
    0x01020304, 0x0000001F, 0x00000020, 0x10101010,
};

uint64_t gpr_value(Rng &g) {
    switch (g.below(4)) {
    case 0: return g.pick(GPR_EDGES);
    case 1: return uint64_t(int64_t(int32_t(uint32_t(g.next())))); // a 32-bit value, sign-extended
    case 2: return g.next() & 0xFF;
    default: return g.next();
    }
}

uint64_t double_value(Rng &g) {
    switch (g.below(5)) {
    case 0:
    case 1: return g.pick(DOUBLE_EDGES);
    case 2: { // a single's value
        uint32_t f = g.percent(30) ? g.pick(FLOAT_EDGES) : uint32_t(g.next());
        float v;
        std::memcpy(&v, &f, 4);
        double d = double(v);
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        return bits;
    }
    case 3: { // an ordinary double near 1, with low bits that rounding to single sees
        uint64_t exponent = 0x3F0 + g.below(32);
        return (g.next() & 0x800FFFFFFFFFFFFFull) | (exponent << 52);
    }
    default: return g.next();
    }
}

uint32_t vector_word(Rng &g, uint32_t kind) {
    switch (kind) {
    case 0: return g.percent(60) ? g.pick(FLOAT_EDGES) : uint32_t(g.next());
    case 1: return g.percent(60) ? g.pick(WORD_EDGES) : uint32_t(g.next());
    case 2: { // an ordinary float
        uint32_t exponent = 100 + g.below(56);
        return (uint32_t(g.next()) & 0x807FFFFF) | (exponent << 23);
    }
    default: return uint32_t(g.next());
    }
}

// FPSCR bits: the exception bits (sticky), the status, the enables and NI
// as random as they are useful. The rounding mode starts at nearest: Xenia
// takes it from mtfsf/mtfsfi/mtfsb only, so the sequences set it themselves.
uint32_t fpscr_value(Rng &g) {
    if (g.percent(50)) return 0;
    uint32_t v = 0;
    if (g.percent(50)) v |= uint32_t(g.next()) & 0x9FF80700; // FX, the exception bits but VX/FEX
    if (g.percent(30)) v |= uint32_t(g.next()) & 0x0007F000; // FR, FI, FPRF
    if (g.percent(15)) v |= uint32_t(g.next()) & 0x000000F8; // the enables
    if (v & 0x01F80700) v |= 0x20000000;                     // VX summarises them
    return v;
}

// The instructions left out: control flow and traps (the emitter's own, and
// the kernel's), the time base (a clock), the MSR writes (interrupt state).
bool excluded(Opcode op) {
    const OpcodeInfo &in = info(op);
    if (in.group == 'b') return true;
    switch (op) {
    case Opcode::mftb:
    case Opcode::mtmsr:
    case Opcode::mtmsrd: return true;
    default: return false;
    }
}

// For each instruction, the bits of the word's low 26 that can be set and
// still decode to it, one at a time.
std::vector<uint32_t> free_bits() {
    std::vector<uint32_t> free(OPCODE_COUNT, 0);
    for (uint32_t k = 0; k < OPCODE_COUNT; k++) {
        Opcode op = Opcode(k);
        uint32_t value = info(op).opcode;
        if (decode(value) != op) continue;
        for (uint32_t b = 0; b < 26; b++)
            if (!(value & (1u << b)) && decode(value | (1u << b)) == op) free[k] |= 1u << b;
    }
    return free;
}

uint32_t field(uint32_t w, int shift) { return (w >> shift) & 31; }
uint32_t set_field(uint32_t w, int shift, uint32_t v) { return (w & ~(31u << shift)) | ((v & 31) << shift); }

bool string_op(Opcode op) {
    return op == Opcode::lswi || op == Opcode::lswx || op == Opcode::stswi || op == Opcode::stswx;
}

// A word for op, its fields random and then fixed so it stays in the case's
// memory and leaves r1 (the pointer) and r2 (the index) alone.
bool make_word(Rng &g, Opcode op, uint32_t free, uint32_t &word) {
    const OpcodeInfo &in = info(op);
    for (int attempt = 0; attempt < 64; attempt++) {
        uint32_t w = in.opcode | (uint32_t(g.next()) & free);
        if (in.group == 'i' || in.group == 'c' || in.group == 'm') {
            // r1 and r2 are never written: fields 21 and 16 stay off them
            for (int shift : {21, 16})
                if (field(w, shift) == 1 || field(w, shift) == 2) w = set_field(w, shift, 3 + g.below(29));
        }
        if (in.group == 'm') {
            w = set_field(w, 16, 1); // RA = r1
            bool indexed = in.form == Form::X || in.form == Form::VX128_1 || in.form == Form::DCBZ;
            if (op == Opcode::lswi || op == Opcode::stswi) indexed = false; // NB in RB's place
            if (indexed) w = set_field(w, 11, 2);
            if (in.form == Form::D) w = (w & 0xFFFF0000u) | (uint32_t(int32_t(g.below(128)) - 64) & 0xFFFF);
            if (in.form == Form::DS) w = (w & 0xFFFF0003u) | ((uint32_t(int32_t(g.below(128)) - 64) & 0xFFFC));
        }
        if (op == Opcode::mfspr || op == Opcode::mtspr) {
            // XER, CTR, and LR for reads (a write would leave the case elsewhere)
            static const uint32_t reads[] = {1, 8, 9}, writes[] = {1, 9};
            uint32_t spr = op == Opcode::mfspr ? g.pick(reads) : g.pick(writes);
            w = (w & ~0x001FF800u) | ((spr & 31) << 16) | ((spr >> 5) << 11);
        }
        if (decode(w) == op) return word = w, true;
    }
    return false;
}

} // namespace

bool generated(Opcode op) { return op != Opcode::Invalid && !excluded(op); }

Opcode focus(const Case &c) {
    for (uint32_t w : c.code)
        if (info(decode(w)).group != 'b') return decode(w);
    return Opcode::Invalid;
}

std::vector<Case> generate(const Options &options) {
    Rng g{std::mt19937_64(options.seed)};
    std::vector<uint32_t> free = free_bits();
    std::vector<Opcode> pool;
    for (uint32_t k = 0; k < OPCODE_COUNT; k++)
        if (generated(Opcode(k)) && decode(info(Opcode(k)).opcode) == Opcode(k)) pool.push_back(Opcode(k));
    std::vector<Opcode> focuses;
    for (Opcode op : pool)
        if (options.only.empty() ||
            std::find(options.only.begin(), options.only.end(), std::string(info(op).name)) != options.only.end())
            focuses.push_back(op);

    std::vector<Case> cases;
    for (Opcode op : focuses) {
        for (uint32_t n = 0; n < options.per_op; n++) {
            Case c{};
            // the instructions: the focus, then others
            std::vector<Opcode> ops{op};
            if (!g.percent(options.single_percent) && !string_op(op)) {
                uint32_t more = 1 + g.below(3);
                for (uint32_t k = 0; k < more; k++) {
                    Opcode o = pool[g.below(uint32_t(pool.size()))];
                    if (string_op(o)) continue;
                    ops.push_back(o);
                }
            }
            for (Opcode o : ops) {
                uint32_t w;
                if (make_word(g, o, free[uint32_t(o)], w)) c.code.push_back(w);
            }
            if (c.code.empty()) continue;
            // a forward bc over the next instruction, reading CR or CTR
            if (c.code.size() >= 2 && g.percent(30)) {
                const uint32_t bos[] = {12, 4, 16, 18, 0, 2, 8, 10, 20};
                uint32_t at = 1 + g.below(uint32_t(c.code.size()) - 1);
                c.code.insert(c.code.begin() + at, 0x40000008u | g.pick(bos) << 21 | g.below(32) << 16);
            }

            State &s = c.in;
            for (int k = 0; k < 32; k++) s.r[k] = gpr_value(g), s.f[k] = double_value(g);
            s.r[1] = POINTER;
            s.r[2] = g.percent(50) ? g.below(64) & ~15u : g.below(64);
            uint32_t vkind = g.below(4);
            for (auto &v : s.v)
                for (uint32_t &w : v) w = vector_word(g, g.percent(80) ? vkind : g.below(4));
            s.cr = uint32_t(g.next());
            s.fpscr = fpscr_value(g);
            s.xer_ca = g.below(2), s.xer_ov = g.below(2), s.xer_so = g.below(2) | s.xer_ov;
            s.vscr_sat = g.percent(20), s.vscr_nj = 1;
            s.lr = SENTINEL, s.ctr = g.percent(30) ? g.below(3) : gpr_value(g), s.msr = MSR;
            for (uint32_t k = 0; k < WINDOW_SIZE; k += 4) {
                uint32_t w;
                switch (g.below(4)) {
                case 0: w = g.pick(WORD_EDGES); break;
                case 1: w = g.pick(FLOAT_EDGES); break;
                case 2: w = uint32_t(g.pick(DOUBLE_EDGES) >> (k & 4 ? 0 : 32)); break;
                default: w = uint32_t(g.next());
                }
                // big-endian, as the guest sees it
                s.memory[k] = uint8_t(w >> 24), s.memory[k + 1] = uint8_t(w >> 16), s.memory[k + 2] = uint8_t(w >> 8),
                s.memory[k + 3] = uint8_t(w);
            }
            cases.push_back(std::move(c));
        }
    }
    return cases;
}

void load_state(Context &c, const State &s, uint8_t *base) {
    for (int k = 0; k < 32; k++) c.r[k] = s.r[k], std::memcpy(&c.f[k], &s.f[k], 8);
    for (int k = 0; k < 128; k++) std::memcpy(c.v[k].u32, s.v[k], 16);
    c.set_cr_word(s.cr);
    c.fpscr = s.fpscr;
    c.xer_ca = s.xer_ca, c.xer_ov = s.xer_ov, c.xer_so = s.xer_so;
    c.vscr_sat = s.vscr_sat, c.vscr_java = !s.vscr_nj;
    c.lr = s.lr, c.ctr = s.ctr, c.msr = s.msr;
    std::memcpy(base + WINDOW, s.memory, WINDOW_SIZE);
}

void save_state(const Context &c, State &s, const uint8_t *base) {
    for (int k = 0; k < 32; k++) s.r[k] = c.r[k], std::memcpy(&s.f[k], &c.f[k], 8);
    for (int k = 0; k < 128; k++) std::memcpy(s.v[k], c.v[k].u32, 16);
    s.cr = c.cr_word();
    s.fpscr = c.fpscr;
    s.xer_ca = c.xer_ca, s.xer_ov = c.xer_ov, s.xer_so = c.xer_so;
    s.vscr_sat = c.vscr_sat, s.vscr_nj = !c.vscr_java;
    s.lr = c.lr, s.ctr = c.ctr, s.msr = c.msr;
    std::memcpy(s.memory, base + WINDOW, WINDOW_SIZE);
}

namespace {
bool unimplemented_seen = false;
}

Runner::Runner(const std::vector<Case> &cases) : cases_(cases) {
    uint32_t code_size = (uint32_t(code_image(cases).size()) + 0xFFFF) & ~0xFFFFu;
    ok_ = space_.ok() && space_.commit(0x10000000, 0x10000) && space_.commit(CODE_BASE, code_size ? code_size : 0x10000);
    if (!ok_) return;
    std::vector<uint8_t> image = code_image(cases);
    std::memcpy(space_.base() + CODE_BASE, image.data(), image.size());
    system_.unimplemented = [](Context &, Opcode, uint32_t) { unimplemented_seen = true; };
}

Result Runner::run(size_t index, void (*fn)(Context &)) {
    Result r;
    static Context c;
    std::memset(&c, 0, sizeof(c));
    c.base = space_.base();
    c.system = &system_;
    load_state(c, cases_[index].in, space_.base());
    unimplemented_seen = false;
    if (fn) {
        fn(c);
    } else {
        Stopped s = interpret(c, case_address(index), SENTINEL, 10'000);
        if (s.reason == Stopped::Unimplemented || s.reason == Stopped::Invalid) unimplemented_seen = true;
        else if (s.reason != Stopped::Returned) r.status = STOPPED;
    }
    if (unimplemented_seen) r.status = UNSUPPORTED;
    save_state(c, r.out, space_.base());
    return r;
}

namespace {
const char *const FPSCR_BITS[32] = {"RN0",   "RN1",   "NI",    "XE",     "ZE",    "UE",    "OE",    "VE",
                                    "VXCVI", "VXSQRT", "VXSOFT", "bit20", "FU",    "FE",    "FG",    "FL",
                                    "C",     "FI",    "FR",    "VXVC",   "VXIMZ", "VXZDZ", "VXIDI", "VXISI",
                                    "VXSNAN", "XX",   "ZX",    "UX",     "OX",    "VX",    "FEX",   "FX"};
std::string hex(uint64_t v, int digits) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%0*llX", digits, (unsigned long long)v);
    return b;
}
} // namespace

std::vector<std::string> differences(const State &a, const State &b) {
    std::vector<std::string> d;
    for (int k = 0; k < 32; k++)
        if (a.r[k] != b.r[k]) d.push_back("r" + std::to_string(k));
    for (int k = 0; k < 32; k++)
        if (a.f[k] != b.f[k]) d.push_back("f" + std::to_string(k));
    for (int k = 0; k < 128; k++)
        if (std::memcmp(a.v[k], b.v[k], 16)) d.push_back("v" + std::to_string(k));
    for (int k = 0; k < 8; k++)
        if (((a.cr ^ b.cr) >> (28 - 4 * k)) & 15) d.push_back("cr" + std::to_string(k));
    for (int k = 31; k >= 0; k--)
        if (((a.fpscr ^ b.fpscr) >> k) & 1) d.push_back(std::string("fpscr.") + FPSCR_BITS[k]);
    if (a.xer_ca != b.xer_ca) d.push_back("xer.ca");
    if (a.xer_ov != b.xer_ov) d.push_back("xer.ov");
    if (a.xer_so != b.xer_so) d.push_back("xer.so");
    if (a.vscr_sat != b.vscr_sat) d.push_back("vscr.sat");
    if (a.vscr_nj != b.vscr_nj) d.push_back("vscr.nj");
    if (a.ctr != b.ctr) d.push_back("ctr");
    if (a.lr != b.lr) d.push_back("lr");
    if (a.msr != b.msr) d.push_back("msr");
    if (std::memcmp(a.memory, b.memory, WINDOW_SIZE)) d.push_back("memory");
    return d;
}

std::string field_value(const State &s, const std::string &f) {
    int n = 0;
    if (f.starts_with("fpscr")) return hex(s.fpscr, 8);
    if (f.starts_with("cr")) return hex(s.cr, 8);
    if (f.starts_with("xer")) return "ca " + std::to_string(s.xer_ca) + " ov " + std::to_string(s.xer_ov) + " so " +
                                     std::to_string(s.xer_so);
    if (f.starts_with("vscr")) return "sat " + std::to_string(s.vscr_sat) + " nj " + std::to_string(s.vscr_nj);
    if (f == "ctr") return hex(s.ctr, 16);
    if (f == "lr") return hex(s.lr, 16);
    if (f == "msr") return hex(s.msr, 16);
    if (std::sscanf(f.c_str(), "r%d", &n) == 1 && n < 32) return hex(s.r[n], 16);
    if (std::sscanf(f.c_str(), "f%d", &n) == 1 && n < 32) return hex(s.f[n], 16);
    if (std::sscanf(f.c_str(), "v%d", &n) == 1 && n < 128) {
        char b[64];
        std::snprintf(b, sizeof(b), "[%08X %08X %08X %08X]", s.v[n][0], s.v[n][1], s.v[n][2], s.v[n][3]);
        return b;
    }
    return "";
}

} // namespace xf::ppc::fuzz
