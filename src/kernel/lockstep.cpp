// The lockstep check (xenoforge/lockstep.h; docs/recompiler.md, "The
// lockstep check"). A checked call runs in segments, each from the call's
// start or an import's return to the next import or the call's return:
//
//   1. the interpreter runs the segment on a copy of the context and a
//      private view of the memory (copy on write), its calls interpreted
//      too, recording each instruction's stores and which instruction last
//      wrote each register; the view's copies are dropped after;
//   2. the generated code runs on, for real, to the same point (the import,
//      reached through Kernel::run_import, or the call's return);
//   3. the registers and the bytes the interpreter stored are compared;
//      a difference is printed with the instruction that wrote the
//      interpreter's value.
//
// The interpreter's System is this check's own: a call runs in it (false:
// interpreted), sc at an import's thunk ends the segment, mftb reads the
// clock and is recorded for the generated code, and what the check cannot
// repeat (MMIO, a trap, a stack switch) gives the segment up.

#include "lockstep_check.h"

#include "objects.h"

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace xf::kernel {
namespace {

std::atomic<uint64_t> next_generation{1};
std::mutex print_mutex;

// what ends an interpreted segment early, thrown from the System's callbacks
struct Stop {
    const Import *import = nullptr; // an import's thunk reached (sc): compare there
    const char *why = nullptr;      // else: given up, why
};

// which instruction of the segment last wrote each piece of the context
struct Writers {
    uint32_t r[32] = {}, f[32] = {}, v[128] = {}, cr[32] = {};
    uint32_t lr = 0, ctr = 0, xer = 0, fpscr = 0, vscr = 0, msr = 0, reserve = 0;
};

struct Store {
    uint32_t address, size, pc;
};
struct Byte {
    uint32_t address;
    uint8_t value;
    uint32_t pc; // the store that left it
};

// The interpreter's observer in a segment: its stores (before each one runs,
// with the registers it reads) and the last writer of each register.
class Recorder final : public ppc::Observer {
public:
    explicit Recorder(const ppc::Context &start) : prev_(std::make_unique<ppc::Context>(start)) {}

    void before(ppc::Context &c, uint32_t pc, ppc::Opcode op, ppc::Instr i) override {
        steps++;
        if (op == ppc::Opcode::Invalid || ppc::info(op).group != 'm') return;
        uint32_t address, size;
        if (ppc::store_extent(c, op, i, &address, &size) && size) {
            stores.push_back({address, size, pc});
            stored += size;
        }
    }
    void after(ppc::Context &c, uint32_t pc, ppc::Opcode op) override {
        ppc::Context &p = *prev_;
        for (int n = 0; n < 32; n++) {
            if (c.r[n] != p.r[n]) p.r[n] = c.r[n], writers.r[n] = pc;
            if (std::memcmp(&c.f[n], &p.f[n], 8)) p.f[n] = c.f[n], writers.f[n] = pc;
            if (c.cr[n] != p.cr[n]) p.cr[n] = c.cr[n], writers.cr[n] = pc;
        }
        // (the vector registers only where an instruction can write them)
        char group = op == ppc::Opcode::Invalid ? 0 : ppc::info(op).group;
        if (group == 'v' || group == 'm')
            for (int n = 0; n < 128; n++)
                if (std::memcmp(&c.v[n], &p.v[n], 16)) p.v[n] = c.v[n], writers.v[n] = pc;
        if (c.lr != p.lr) p.lr = c.lr, writers.lr = pc;
        if (c.ctr != p.ctr) p.ctr = c.ctr, writers.ctr = pc;
        if (c.xer_ca != p.xer_ca || c.xer_ov != p.xer_ov || c.xer_so != p.xer_so || c.xer_bc != p.xer_bc)
            p.xer_ca = c.xer_ca, p.xer_ov = c.xer_ov, p.xer_so = c.xer_so, p.xer_bc = c.xer_bc, writers.xer = pc;
        if (c.fpscr != p.fpscr) p.fpscr = c.fpscr, writers.fpscr = pc;
        if (c.vscr_sat != p.vscr_sat) p.vscr_sat = c.vscr_sat, writers.vscr = pc;
        if (c.msr != p.msr) p.msr = c.msr, writers.msr = pc;
        if (c.reserved != p.reserved || c.reserve_address != p.reserve_address)
            p.reserved = c.reserved, p.reserve_address = c.reserve_address, writers.reserve = pc;
    }

    std::vector<Store> stores;
    uint64_t stored = 0, steps = 0;
    Writers writers;

private:
    std::unique_ptr<ppc::Context> prev_;
};

// the segments' limit on the bytes stored (past it, given up)
constexpr uint64_t STORED_LIMIT = 4u << 20;

// the host offset of a guest address (Memory::translate's)
uint64_t host_offset(uint32_t address) { return uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u); }

// the guest ranges the private view's views cover (memory.cpp's map_info),
// so a segment's stores drop each view's copies once
constexpr uint32_t VIEW_STARTS[] = {0x00000000, 0x40000000, 0x7F000000, 0x80000000,
                                    0x90000000, 0xA0000000, 0xC0000000, 0xE0000000};

} // namespace

// a thread's call being checked, and its private view (kept between calls)
struct Lockstep::Check {
    uint64_t generation = 0;
    std::unique_ptr<Memory::PrivateView> view;
    bool active = false;
    ppc::Context *c = nullptr; // the generated code's
    uint32_t root = 0, until = 0, stack_top = 0;
    int import_depth = 0;             // imports under way (a callback's nest)
    const Import *previous = nullptr; // the import the segment started after
    // the segment the interpreter ran, waiting for the generated code
    bool live = false;
    uint32_t from = 0;
    const Import *import = nullptr; // where it stopped (nullptr: the return)
    std::unique_ptr<ppc::Context> ctx = std::make_unique<ppc::Context>();
    Writers writers;
    std::vector<Byte> bytes;
    uint64_t steps = 0;
    std::vector<uint64_t> times;
    size_t time_next = 0;
};

namespace {
thread_local std::unique_ptr<Lockstep::Check> t_check;
}

Lockstep::Lockstep(Kernel &k, const LockstepOptions &options)
    : k_(k), options_(options), functions_(options.functions.begin(), options.functions.end()),
      generation_(next_generation++) {
    system_.owner = this;
    // every call interpreted: the segment is the call's whole tree
    system_.call = [](ppc::Context &, uint32_t) { return false; };
    system_.syscall = [](ppc::Context &c) {
        Lockstep &l = *static_cast<Lockstep *>(c.system->owner);
        const Import *import = nullptr;
        if (auto it = l.k_.by_thunk_.find(c.pc); it != l.k_.by_thunk_.end()) import = &l.k_.imports_[it->second];
        else import = l.k_.find_thunk(c.pc);
        if (!import) throw Stop{nullptr, "a system call outside an import's thunk"};
        if (import->name == "KeSetCurrentStackPointers") throw Stop{nullptr, "a stack switch"};
        throw Stop{import, nullptr};
    };
    system_.trap = [](ppc::Context &, uint32_t) { throw Stop{nullptr, "a trap"}; };
    system_.unimplemented = [](ppc::Context &, ppc::Opcode, uint32_t) {
        throw Stop{nullptr, "an instruction with no semantics"};
    };
    system_.mmio_read = [](ppc::Context &, uint32_t) -> uint32_t { throw Stop{nullptr, "an MMIO access"}; };
    system_.mmio_write = [](ppc::Context &, uint32_t, uint32_t) { throw Stop{nullptr, "an MMIO access"}; };
    system_.time_base = [](ppc::Context &c) {
        Lockstep &l = *static_cast<Lockstep *>(c.system->owner);
        uint64_t value = l.k_.objects_->now() * 5; // as the kernel's (kernel.cpp)
        if (t_check) t_check->times.push_back(value);
        return value;
    };
}

bool Lockstep::usable(std::string &error) { return k_.memory().private_view(error) != nullptr; }

Lockstep::Check *Lockstep::current(ppc::Context &c) {
    Check *check = t_check.get();
    if (!check || check->generation != generation_ || !check->active || check->c != &c) return nullptr;
    return check;
}

bool Lockstep::wanted(ppc::Context &c, uint32_t target) {
    if (c.system != &k_.system_) return false;
    if (t_check && t_check->generation == generation_ && t_check->active) return false;
    if (functions_.count(target)) return true;
    if (!options_.sample) return false;
    thread_local uint64_t calls = 0;
    return ++calls % options_.sample == 0;
}

void Lockstep::give_up(const std::string &why) {
    std::lock_guard lock(mutex_);
    counts_.given_up[why]++;
}

void Lockstep::check(ppc::Context &c, uint32_t target, void (*fn)(ppc::Context &)) {
    if (!t_check || t_check->generation != generation_) {
        t_check = std::make_unique<Check>();
        t_check->generation = generation_;
    }
    Check &check = *t_check;
    check.active = true, check.c = &c, check.root = target;
    check.until = uint32_t(c.lr) & ~3u, check.stack_top = uint32_t(c.r[1]);
    check.import_depth = 0, check.previous = nullptr;
    {
        std::lock_guard lock(mutex_);
        counts_.calls++;
    }
    begin_segment(check, c, target);
    try {
        fn(c);
    } catch (...) {
        // an Unwind out of the call (its segment's interpreter gave up
        // already, or differed) or a host thread's end
        if (check.live) give_up("an unwind out of the call");
        check.active = check.live = false;
        throw;
    }
    if (check.active && check.import_depth == 0) compare(check, c, nullptr);
    check.active = check.live = false;
}

void Lockstep::before_import(ppc::Context &c, const Import &import) {
    Check *check = current(c);
    if (!check) return;
    if (check->import_depth++ > 0) return; // one a callback under an import calls
    if (import.name == "KeSetCurrentStackPointers") {
        // the code goes on on another host stack: the check ends here
        if (check->live) give_up("a stack switch");
        check->active = check->live = false;
        return;
    }
    compare(*check, c, &import);
}

void Lockstep::after_import(ppc::Context &c) {
    Check *check = current(c);
    if (!check || --check->import_depth > 0) return;
    // the import returns to LR (its thunk's blr): the next segment from there
    begin_segment(*check, c, uint32_t(c.lr) & ~3u);
}

bool Lockstep::replay_time_base(ppc::Context &c, uint64_t &value) {
    Check *check = current(c);
    if (!check || !check->live || check->import_depth || check->time_next >= check->times.size()) return false;
    value = check->times[check->time_next++];
    return true;
}

void Lockstep::begin_segment(Check &check, ppc::Context &c, uint32_t pc) {
    check.live = false;
    check.from = pc, check.import = nullptr, check.times.clear(), check.time_next = 0, check.bytes.clear();
    if (!check.view) {
        std::string error;
        check.view = k_.memory().private_view(error);
        if (!check.view) return give_up("no private view: " + error);
    }
    ppc::Context &ic = *check.ctx;
    ic = c;
    ic.base = check.view->base();
    ic.system = &system_;
    Recorder recorder(ic);
    const char *why = nullptr;
    // the interpreter's floating-point environment (rounding, flags) is put
    // back for the generated code
    std::fenv_t env;
    std::fegetenv(&env);
    try {
        ppc::Stopped s = ppc::interpret_observed(ic, pc, check.until, options_.step_limit, check.stack_top, recorder);
        switch (s.reason) {
        case ppc::Stopped::Returned: break;
        case ppc::Stopped::Steps: why = "the step limit"; break;
        case ppc::Stopped::Unwound: why = "an unwind"; break;
        case ppc::Stopped::Invalid: why = "an invalid instruction"; break;
        case ppc::Stopped::Unimplemented: why = "an instruction with no semantics"; break;
        }
    } catch (const Stop &stop) {
        check.import = stop.import, why = stop.why;
    }
    std::fesetenv(&env);
    if (!why && recorder.stored > STORED_LIMIT) why = "the stores past the limit";
    // the bytes the interpreter stored, last store first, then the view's
    // copies dropped
    if (!why) {
        check.bytes.reserve(size_t(recorder.stored));
        for (const Store &s : recorder.stores)
            for (uint32_t n = 0; n < s.size; n++) {
                uint32_t a = s.address + n;
                check.bytes.push_back({a, check.view->base()[host_offset(a)], s.pc});
            }
        std::stable_sort(check.bytes.begin(), check.bytes.end(),
                         [](const Byte &a, const Byte &b) { return a.address < b.address; });
        // keep each address's last store (the last of equal ones after the sort)
        std::vector<Byte> last;
        last.reserve(check.bytes.size());
        for (size_t n = 0; n < check.bytes.size(); n++)
            if (n + 1 == check.bytes.size() || check.bytes[n + 1].address != check.bytes[n].address)
                last.push_back(check.bytes[n]);
        check.bytes.swap(last);
    }
    bool touched[std::size(VIEW_STARTS)] = {};
    for (const Store &s : recorder.stores)
        for (uint32_t a : {s.address, s.address + (s.size ? s.size - 1 : 0)})
            touched[std::upper_bound(std::begin(VIEW_STARTS), std::end(VIEW_STARTS), a) - std::begin(VIEW_STARTS) -
                    1] = true;
    for (size_t n = 0; n < std::size(VIEW_STARTS); n++)
        if (touched[n] && !check.view->reset(VIEW_STARTS[n], 1)) {
            check.view.reset();
            if (!why) why = "a private view that could not be mapped again";
        }
    if (why) {
        give_up(why);
        return;
    }
    check.writers = recorder.writers;
    check.steps = recorder.steps;
    check.live = true;
}

void Lockstep::compare(Check &check, ppc::Context &c, const Import *import) {
    if (!check.live) {
        check.previous = import;
        return;
    }
    check.live = false;
    const ppc::Context &ic = *check.ctx;
    std::vector<std::string> lines;
    char text[256];
    auto where = [&](uint32_t pc) {
        if (!pc) return std::string("(not written in the segment)");
        return "(the interpreter's from 0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof(b), "%08X", pc);
            return std::string(b);
        }() + ": " + ppc::disassemble(pc, k_.load32(pc)) + ")";
    };
    auto name = [](const Import *i) { return i ? i->library + "!" + i->name : std::string("the call's return"); };
    if (import != check.import) {
        std::snprintf(text, sizeof(text), "  stopped at %s, the interpreter at %s", name(import).c_str(),
                      name(check.import).c_str());
        lines.push_back(text);
    }
    auto reg = [&](const char *what, int n, uint64_t g, uint64_t i, uint32_t pc) {
        if (g == i) return;
        char label[16];
        if (n >= 0) std::snprintf(label, sizeof(label), "%s%d", what, n);
        else std::snprintf(label, sizeof(label), "%s", what);
        std::snprintf(text, sizeof(text), "  %-6s generated 0x%016" PRIX64 ", interpreter 0x%016" PRIX64 " ", label, g,
                      i);
        lines.push_back(text + where(pc));
    };
    for (int n = 0; n < 32; n++) reg("r", n, c.r[n], ic.r[n], check.writers.r[n]);
    for (int n = 0; n < 32; n++) {
        uint64_t g, i;
        std::memcpy(&g, &c.f[n], 8), std::memcpy(&i, &ic.f[n], 8);
        reg("f", n, g, i, check.writers.f[n]);
    }
    for (int n = 0; n < 128; n++) {
        if (!std::memcmp(&c.v[n], &ic.v[n], 16)) continue;
        std::snprintf(text, sizeof(text), "  v%-5d generated %08X %08X %08X %08X, interpreter %08X %08X %08X %08X ", n,
                      c.v[n].u32[0], c.v[n].u32[1], c.v[n].u32[2], c.v[n].u32[3], ic.v[n].u32[0], ic.v[n].u32[1],
                      ic.v[n].u32[2], ic.v[n].u32[3]);
        lines.push_back(text + where(check.writers.v[n]));
    }
    for (int field = 0; field < 8; field++) {
        uint64_t g = 0, i = 0;
        uint32_t pc = 0;
        for (int b = 0; b < 4; b++) {
            g = g << 4 | c.cr[4 * field + b], i = i << 4 | ic.cr[4 * field + b];
            if (c.cr[4 * field + b] != ic.cr[4 * field + b]) pc = check.writers.cr[4 * field + b];
        }
        reg("cr", field, g, i, pc);
    }
    reg("lr", -1, c.lr, ic.lr, check.writers.lr);
    reg("ctr", -1, c.ctr, ic.ctr, check.writers.ctr);
    reg("xer", -1, uint64_t(c.xer_so) << 24 | uint64_t(c.xer_ov) << 16 | uint64_t(c.xer_ca) << 8 | c.xer_bc,
        uint64_t(ic.xer_so) << 24 | uint64_t(ic.xer_ov) << 16 | uint64_t(ic.xer_ca) << 8 | ic.xer_bc,
        check.writers.xer);
    reg("fpscr", -1, c.fpscr, ic.fpscr, check.writers.fpscr);
    reg("vscr", -1, c.vscr_sat, ic.vscr_sat, check.writers.vscr);
    reg("msr", -1, c.msr, ic.msr, check.writers.msr);
    reg("resv", -1, c.reserved ? 0x100000000ull | c.reserve_address : 0,
        ic.reserved ? 0x100000000ull | ic.reserve_address : 0, check.writers.reserve);
    // the bytes the interpreter stored, against the guest memory now: runs of
    // consecutive differing bytes from the same store
    size_t runs = 0;
    for (size_t n = 0; n < check.bytes.size();) {
        const Byte &b = check.bytes[n];
        if (*k_.memory().translate(b.address) == b.value) {
            n++;
            continue;
        }
        size_t end = n + 1;
        while (end < check.bytes.size() && end - n < 16 && check.bytes[end].address == check.bytes[end - 1].address + 1 &&
               check.bytes[end].pc == b.pc)
            end++;
        if (++runs <= 8) {
            std::string g, i;
            for (size_t m = n; m < end; m++) {
                char h[4];
                std::snprintf(h, sizeof(h), "%02X", *k_.memory().translate(check.bytes[m].address)), g += h;
                std::snprintf(h, sizeof(h), "%02X", check.bytes[m].value), i += h;
            }
            std::snprintf(text, sizeof(text), "  [%08X] %zu bytes: generated %s, interpreter %s ", b.address, end - n,
                          g.c_str(), i.c_str());
            lines.push_back(text + where(b.pc));
        }
        n = end;
    }
    if (runs > 8) lines.push_back("  ... " + std::to_string(runs - 8) + " more runs of bytes differ");
    uint64_t number;
    {
        std::lock_guard lock(mutex_);
        counts_.segments++;
        if (lines.empty()) {
            counts_.matched++;
            check.previous = import;
            return;
        }
        number = ++counts_.mismatched;
        counts_.mismatches[check.from]++;
    }
    const Import *after = check.previous; // the import the segment started after (none: the call's start)
    check.previous = import;
    if (number > options_.print_limit) return;
    std::lock_guard lock(print_mutex);
    std::fprintf(stderr,
                 "lockstep: mismatch %" PRIu64 " in the call to 0x%08X: the segment from 0x%08X (%s%s) to %s, %" PRIu64
                 " instructions interpreted\n",
                 number, check.root, check.from, after ? "after " : "the call's start",
                 after ? (after->library + "!" + after->name).c_str() : "", name(check.import).c_str(), check.steps);
    for (const std::string &line : lines) std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

LockstepCounts Lockstep::counts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::string Lockstep::report() const {
    LockstepCounts n = counts();
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "lockstep: %" PRIu64 " calls checked, %" PRIu64 " segments compared, %" PRIu64 " the same, %" PRIu64
                  " different\n",
                  n.calls, n.segments, n.matched, n.mismatched);
    out += line;
    for (const auto &[why, count] : n.given_up) {
        std::snprintf(line, sizeof(line), "  given up: %" PRIu64 " at %s\n", count, why.c_str());
        out += line;
    }
    std::vector<std::pair<uint64_t, uint32_t>> worst;
    for (const auto &[at, count] : n.mismatches) worst.push_back({count, at});
    std::sort(worst.rbegin(), worst.rend());
    for (size_t k = 0; k < worst.size() && k < 20; k++) {
        std::snprintf(line, sizeof(line), "  different from 0x%08X: %" PRIu64 "\n", worst[k].second, worst[k].first);
        out += line;
    }
    return out;
}

bool parse_lockstep_functions(const std::string &list, std::vector<uint32_t> &out) {
    size_t at = 0;
    while (at <= list.size()) {
        size_t comma = list.find(',', at);
        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (item.starts_with("0x") || item.starts_with("0X")) item = item.substr(2);
        char *end = nullptr;
        unsigned long long value = std::strtoull(item.c_str(), &end, 16);
        if (item.empty() || *end || value > 0xFFFFFFFFull) return false;
        out.push_back(uint32_t(value));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return true;
}

bool lockstep_options_from_env(LockstepOptions &options, std::string &why) {
    const char *list = std::getenv("XF_LOCKSTEP");
    const char *sample = std::getenv("XF_LOCKSTEP_SAMPLE");
    if ((!list || !*list) && (!sample || !*sample)) return false;
    if (list && *list && !parse_lockstep_functions(list, options.functions)) {
        why = "XF_LOCKSTEP: not a list of hex addresses";
        return false;
    }
    if (sample && *sample) options.sample = uint32_t(std::strtoul(sample, nullptr, 10));
    return true;
}

} // namespace xf::kernel
