/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The kernel's core (kernel.h; docs/runtime.md). The import binding
// follows Xenia's XexModule::SetupLibraryImports (src/xenia/cpu/
// xex_module.cc, BSD; see LICENSE-xenia): a system library's function
// thunks become sc 2, blr, nop, nop; a function's descriptor record gets
// 0xDEADC0DE; a variable's gets the address of the kernel's object for it.
// A game's library's (a DLL's) function thunk gets lis r11 / ori r11 with
// the export's address in place of its first two instructions, so its own
// mtctr r11; bctr jumps there (dispatch to the thunk goes to the export
// itself, so generated code reaches the export's generated code: not in
// Xenia, whose JIT runs the thunk); a variable's record gets the address; what
// the library does not export gets 0xF00DF00D, or an unimplemented thunk.
// An import no table names is called "<library>_<ordinal, 3 hex digits>", as
// Xenia names it. Unlike Xenia, the imports from xbdm.xex are bound as the
// other system libraries' (Xenia loads no xbdm module unless its console_type
// is set, -1 by default, and leaves them unresolved).

#include "xenoforge/kernel.h"

#include "lockstep_check.h"
#include "objects.h"
#include "video.h"
#include "xenoforge/crash.h"
#include "xenoforge/exports.h"
#include "xenoforge/hooks.h"
#include "xenoforge/ppc_interp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace xf::kernel {
namespace {

std::map<std::string, ExportFn> &registry() {
    static std::map<std::string, ExportFn> r;
    return r;
}

// "xboxkrnl.exe" -> "xboxkrnl"
std::string short_library(std::string_view library) {
    std::string s(library.substr(0, library.find('.')));
    for (char &ch : s) ch = char(std::tolower(uint8_t(ch)));
    return s;
}

constexpr uint32_t SENTINEL = 0xBCBCBCBC; // a return address no code is at

// the FILETIME's unit
using Ticks100ns = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;

uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

} // namespace

bool register_export(const char *library, const char *name, ExportFn fn) {
    auto [it, added] = registry().insert({std::string(library) + "!" + name, fn});
    if (!added) std::fprintf(stderr, "kernel: %s!%s is implemented twice\n", library, name);
    return true;
}

ExportFn find_export_fn(const char *library, const char *name) {
    auto it = registry().find(std::string(library) + "!" + name);
    return it == registry().end() ? nullptr : it->second;
}

uint64_t arg64(Kernel &k, ppc::Context &c, int n) {
    if (n < 8) return c.r[3 + n];
    return k.load64(uint32_t(c.r[1]) + 0x54 + uint32_t(n - 8) * 8);
}

uint32_t arg(Kernel &k, ppc::Context &c, int n) {
    if (n < 8) return uint32_t(c.r[3 + n]);
    return k.load32(uint32_t(c.r[1]) + 0x54 + uint32_t(n - 8) * 8);
}

// What try_call reads: the generated code of each module loaded, as a range
// and a table with a slot per instruction ((address - begin) / 4), so a
// lookup is one range compare per module and one load. Never changed once
// published: a load or unload publishes a new one.
struct Kernel::CodeMap {
    using Fn = void (*)(ppc::Context &);
    struct Range {
        uint32_t begin = 0, size = 0; // the first function to the end of the last one's first word
        const Fn *table = nullptr;
        const ppc::GeneratedModule *module = nullptr;
    };
    std::vector<Range> ranges;
};

struct Kernel::Code {
    std::mutex mutex; // the writers'
    std::vector<const ppc::GeneratedModule *> available;
    // each module's range and table, made the first time its image loads and
    // kept (a module unloaded and loaded again finds it)
    std::map<const ppc::GeneratedModule *, CodeMap::Range> ranges;
    std::vector<std::unique_ptr<CodeMap::Fn[]>> tables;
    std::map<uint32_t, const ppc::GeneratedModule *> loaded; // by base
    // every map published: a thread may still read one replaced
    std::vector<std::unique_ptr<CodeMap>> maps;
};

// A guest thread running guest code (running) or in the kernel (not), for
// Kernel::pause: set for the scope, put back after; a thread that goes back
// to guest code while a pause is asked parks first.
struct GuestCodeScope {
    Kernel &k;
    Thread *t;
    bool was = false;
    GuestCodeScope(Kernel &kernel, bool running) : k(kernel), t(current_thread()) {
        if (!t) return;
        was = t->guest_running.exchange(running, std::memory_order_seq_cst);
        if (running && k.pause_requested_.load(std::memory_order_seq_cst)) [[unlikely]]
            k.park();
    }
    ~GuestCodeScope() {
        if (!t) return;
        t->guest_running.store(was, std::memory_order_seq_cst);
        if (was && k.pause_requested_.load(std::memory_order_seq_cst)) [[unlikely]]
            k.park();
    }
};

Kernel::Kernel(Memory &memory) : memory_(memory), code_(std::make_unique<Code>()) {
    system_.owner = this;
    system_.syscall = [](ppc::Context &c) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        const Import *import = nullptr;
        if (auto it = k.by_thunk_.find(c.pc); it != k.by_thunk_.end()) import = &k.imports_[it->second];
        else import = k.find_thunk(c.pc);
        if (!import) return;
        // the thunk's blr goes on where LR says after the export, and so does
        // the code that waits in a stack switch made here (stacks.h)
        static const ExportFn stack_switch = find_export_fn("xboxkrnl", "KeSetCurrentStackPointers");
        if (import->fn && import->fn == stack_switch)
            if (Thread *t = current_thread(); t && t->context.get() == &c) t->stack_resume = STACK_RESUME_ANYWHERE;
        k.run_import(c, *import);
    };
    system_.call = [](ppc::Context &c, uint32_t target) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        if (k.try_call(c, target)) return true;
        // the interpreter goes on into a function with no generated code
        if (k.entry_hook_) k.entered(target);
        return false;
    };
    system_.mmio_read = [](ppc::Context &c, uint32_t address) -> uint32_t {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        if (const Mmio *m = k.find_mmio(address)) return m->read ? m->read(m->device, address) : 0;
        k.unanswered_mmio("read", address, c.pc);
        return 0;
    };
    system_.mmio_write = [](ppc::Context &c, uint32_t address, uint32_t value) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        if (const Mmio *m = k.find_mmio(address)) {
            if (m->write) m->write(m->device, address, value);
            return;
        }
        k.unanswered_mmio("write", address, c.pc);
    };
    system_.unimplemented = [](ppc::Context &c, ppc::Opcode op, uint32_t code) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        char reason[96];
        std::snprintf(reason, sizeof(reason), "%s (0x%08X) at 0x%08X has no semantics", ppc::info(op).name, code, c.pc);
        k.stop(reason);
    };
    // a trap that fired: a debug print, or a breakpoint through the title's handlers (exceptions.cpp)
    system_.trap = [](ppc::Context &c, uint32_t address) {
        trap(*static_cast<Kernel *>(c.system->owner), c, address);
    };
    system_.time_base = [](ppc::Context &c) {
        // the Xenon's time base runs at 50 MHz: 5 ticks in the scheduler's
        // clock's 100 ns
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        // what the lockstep check's interpreter read here, read again (the
        // read was counted on the clock once, as it was made)
        uint64_t replayed;
        if (k.lockstep_ && k.lockstep_->replay_time_base(c, replayed)) [[unlikely]]
            return replayed;
        // a read moves the deterministic mode's clock: a loop that waits on
        // the time base gets there, and may hand over
        k.objects_->scheduler->advance(k.deterministic_time_base_cost_);
        return k.objects_->now() * 5;
    };
    // the port's hooks (xenoforge/hooks.h), for the interpreter
    system_.hook_pages = hooks::pages();
    system_.enter_hooked = [](ppc::Context &c, uint32_t target, uint32_t from) {
        // a branch from outside a hooked function to its start (its size from
        // its slot; unknown, every branch there: a loop back to a function's
        // first instruction is rarer than a tail call to it)
        const hooks::Entry *e = hooks::find(target);
        if (!e || !e->function()) return false;
        const uint32_t size = hooks::function_size(target);
        if (from >= target && from - target < size) return false;
        return static_cast<Kernel *>(c.system->owner)->try_call(c, target);
    };
    system_.mid_hook = [](ppc::Context &c, uint32_t pc) -> uint32_t {
        hooks::Pin pin;
        const hooks::Entry *e = pin.find(pc);
        return e && !e->mid.empty() ? hooks::run_mid(c, *e) : 0;
    };
    system_.run_original = [](ppc::Context &c, uint32_t address) {
        static_cast<Kernel *>(c.system->owner)->run_original(c, address);
    };
    system_.hook_boundary = [](ppc::Context &c, uint32_t address, bool after, uint32_t resume) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        if (!k.lockstep_) return;
        // the port's code is to the check what an import is: run once, for
        // real, between two segments
        if (!after) k.lockstep_->before_import(c, *k.hook_marker(address));
        else k.lockstep_->after_import(c, resume, resume == address);
    };
    // a long spin on db16cyc hints (ppc_ops.h's spin_hint): the processor to
    // whatever else is ready on it (the host scheduler's: SwitchToThread,
    // sched_yield; the console scheduler's: its next ready fiber)
    // A spin that goes on (hand-overs less than SPIN_STREAK_GAP apart for
    // SPIN_WAIT_AFTER) while the command processor has commands to run is,
    // most likely, Direct3D's wait for the GPU: a loop of hints around a
    // read of what the processor writes back (the read pointer's copy, a
    // fence's value), which kept a host core busy for as long as the GPU
    // took. Such a thread sleeps until the next write-back or interrupt
    // (video.h's write_backs), SPIN_WAIT_US at most, and not at all when
    // one came since its last hand-over (its loop has yet to see it). A
    // spin on something else meanwhile (another thread's flag) looks again
    // at each write-back and each SPIN_WAIT_US. XF_SPIN_WAIT=0: hand-overs
    // only.
    system_.spin = [](ppc::Context &c) {
        static constexpr uint64_t SPIN_STREAK_GAP = 10'000, SPIN_WAIT_AFTER = 2'000; // 100 ns units
        static constexpr uint32_t SPIN_WAIT_US = 1'000;
        static const bool waiting = [] {
            const char *v = std::getenv("XF_SPIN_WAIT");
            return !(v && v[0] == '0');
        }();
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        // handing over is a safe point: the thread waiting there is parked
        GuestCodeScope parked(k, false);
        Scheduler &s = *k.objects_->scheduler;
        Thread *t = waiting ? current_thread() : nullptr;
        if (!t) return s.spin();
        Video &v = video_state(k);
        const uint64_t now = s.now();
        const uint32_t seen = v.write_backs.load(std::memory_order_acquire);
        if (now - t->spin_last > SPIN_STREAK_GAP) t->spin_since = now;
        if (now - t->spin_since >= SPIN_WAIT_AFTER && seen == t->spin_seen && v.busy.load(std::memory_order_relaxed)) {
            v.spin_waits.fetch_add(1, std::memory_order_relaxed);
            s.spin_wait(v.write_backs, seen, SPIN_WAIT_US);
        } else {
            s.spin();
        }
        t->spin_seen = seen;
        t->spin_last = s.now();
    };
    system_.yield = [](ppc::Context &c) {
        // the yield points' budget ran out (ppc_ops.h): the clock moves by
        // the whole ticks spent, and another thread may run (a spin on a flag
        // with no kernel call reaches its slice's end here)
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        // a pause (Kernel::pause) parks the thread here
        if (k.pause_requested_.load(std::memory_order_relaxed)) [[unlikely]]
            k.park();
        const int64_t units = k.deterministic_yield_units_;
        if (!units) {
            // (counting was turned on for a pause or a preemption: off again,
            // and the processor handed over if the scheduler asked for it)
            c.yield_budget = INT64_MAX;
            __atomic_store_n(&c.yield_counting, false, __ATOMIC_RELAXED);
            GuestCodeScope parked(k, false);
            k.objects_->scheduler->preempt_point();
            return;
        }
        const int64_t ticks = 1 + (-(c.yield_budget + 1)) / units;
        c.yield_budget += ticks * units;
        // handing over is a safe point: the thread waiting there is parked
        GuestCodeScope parked(k, false);
        k.objects_->scheduler->advance(uint64_t(ticks));
    };
    // the globals first: the objects and threads use their processes,
    // object types and KeTimeStampBundle
    setup_kernel(*this);
    objects_ = std::make_unique<Objects>(*this);
    create_devices(*this);
    // the devices, before any guest thread runs
    setup_video(*this);
    setup_audio(*this);
}

// the devices' threads end while the objects they run on are still here
Kernel::~Kernel() {
    // the port's hooks no longer reach this kernel's modules through their slots
    for (const auto &[base, m] : code_->loaded) hooks::unbind(*m);
    shutdown_xam_tasks(*this);
    shutdown_file_completion(*this);
    shutdown_audio(*this);
    shutdown_video(*this);
    shutdown_timer_dpcs(*this);
}

uint32_t Kernel::load32(uint32_t a) { return be32(host(a)); }
uint16_t Kernel::load16(uint32_t a) { return be16(host(a)); }
uint64_t Kernel::load64(uint32_t a) { return uint64_t(load32(a)) << 32 | load32(a + 4); }
void Kernel::store32(uint32_t a, uint32_t v) {
    uint8_t *p = host(a);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void Kernel::store16(uint32_t a, uint16_t v) {
    uint8_t *p = host(a);
    p[0] = uint8_t(v >> 8), p[1] = uint8_t(v);
}
void Kernel::store64(uint32_t a, uint64_t v) { store32(a, uint32_t(v >> 32)), store32(a + 4, uint32_t(v)); }

uint32_t Kernel::variable(const std::string &name) {
    {
        std::shared_lock lock(variables_mutex_);
        auto it = variables_.find(name);
        if (it != variables_.end()) return it->second;
    }
    std::unique_lock lock(variables_mutex_);
    auto it = variables_.find(name);
    if (it != variables_.end()) return it->second;
    // zeroed until the family that owns it fills it in (exports with a
    // value set them through here first)
    uint32_t address = memory_.system_alloc(0x100);
    return variables_[name] = address;
}

void Kernel::set_variable(const std::string &name, uint32_t address) {
    std::unique_lock lock(variables_mutex_);
    variables_[name] = address;
}

void Kernel::add_thunk(uint32_t address, const std::string &library, const std::string &name) {
    // sc 2; blr, as the import thunks (Xenia's trampolines are the same 8 bytes)
    store32(address + 0, 0x44000042);
    store32(address + 4, 0x4E800020);
    Import im;
    im.library = library, im.name = name, im.thunk = address;
    auto fn = registry().find(library + "!" + name);
    if (fn != registry().end()) im.fn = fn->second;
    std::unique_lock lock(thunks_mutex_);
    thunks_[address] = std::move(im);
    has_thunks_ = true;
}

bool Kernel::implemented(const std::string &library, const std::string &name) {
    return registry().count(library + "!" + name) != 0;
}

const Import *Kernel::find_thunk(uint32_t address) const {
    if (!has_thunks_) return nullptr;
    std::shared_lock lock(thunks_mutex_);
    auto it = thunks_.find(address);
    return it == thunks_.end() ? nullptr : &it->second;
}

// one clock, the scheduler's (100 ns since the kernel started): the
// interrupt time; the system time is it plus the host's time at the start
void Kernel::add_mmio(const Mmio &range) { mmio_.push_back(range); }

const Kernel::Mmio *Kernel::find_mmio(uint32_t address) const {
    for (const Mmio &m : mmio_)
        if ((address & m.mask) == m.base) return &m;
    return nullptr;
}

void Kernel::unanswered_mmio(const char *what, uint32_t address, uint32_t pc) {
    std::lock_guard<std::mutex> l(counts_mutex_);
    char key[48];
    std::snprintf(key, sizeof(key), "mmio %s 0x%08X", what, address);
    if (unimplemented_[key]++ == 0)
        std::fprintf(stderr, "kernel: no device answers a %s at 0x%08X (from 0x%08X)\n", what, address, pc);
}

uint64_t Kernel::interrupt_time() const { return objects_->now(); }
uint64_t Kernel::system_time() const { return objects_->system_time(); }
uint32_t Kernel::uptime_ms() const { return uint32_t(std::min<uint64_t>(objects_->now() / 10'000, UINT32_MAX)); }

bool Kernel::load(const Xex &xex, std::string &error) {
    xex_ = &xex;
    if (!map_image(xex, error)) return false;
    entry_ = xex.entry_point;
    // the kernel's memory, variables and modules, before the imports take
    // the variables' addresses
    setup_modules(*this, xex);
    bind(xex, true);
    return true;
}

bool Kernel::map_image(const Xex &xex, std::string &error) {
    uint32_t base = xex.base_address, size = uint32_t(xex.image.size());
    Heap *heap = memory_.heap_for(base);
    if (!heap) return error = "no heap holds the image's base", false;
    uint32_t pages = (size + heap->page_size() - 1) & ~(heap->page_size() - 1);
    if (!size ||
        !heap->alloc_fixed(base, pages, heap->page_size(), ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE)) {
        char text[96];
        std::snprintf(text, sizeof(text), "could not map the image at its base (0x%08X-0x%08X)", base, base + pages);
        return error = text, false;
    }
    std::memcpy(host(base), xex.image.data(), size);
    // pages another image had before are not zeroed by their release
    std::memset(host(base) + size, 0, pages - size);
    return true;
}

void Kernel::bind_imports(const Xex &xex) { bind(xex, false); }

void Kernel::bind(const Xex &xex, bool executable) {
    for (const XexImportLibrary &library : xex.import_libraries) {
        std::string lib = short_library(library.name);
        // a game's own library: its module, loaded first if need be
        bool system = is_system_library(library.name);
        uint32_t module = system ? 0 : import_module(*this, library.name);
        for (uint32_t record : library.records) {
            uint32_t value = load32(record);
            uint32_t type = value >> 24;
            uint16_t ordinal = uint16_t(value & 0xFFFF);
            const Export *e = system ? find_export(library.name, ordinal) : nullptr;
            uint32_t user = module ? module_export(*this, module, ordinal) : 0;
            // what no table names: Xenia's "{}_{:03X}" (the ordinal in hex)
            char unnamed[16];
            std::snprintf(unnamed, sizeof(unnamed), "_%03X", ordinal);
            std::string name = e ? e->name : lib + unnamed;
            if (type == 0) {
                if (e) store32(record, e->kind == ExportKind::Variable ? variable(name) : 0xDEADC0DE);
                else store32(record, user ? user : 0xF00DF00D);
            } else if (type == 1) {
                if (user) {
                    // lis r11, hi; ori r11, r11, lo: the thunk's mtctr r11;
                    // bctr goes to the export. Dispatch to the thunk goes
                    // there itself (try_call), so generated code reaches the
                    // export's generated code with no interpreted thunk.
                    store32(record + 0, 0x3D600000 | (user >> 16));
                    store32(record + 4, 0x616B0000 | (user & 0xFFFF));
                    Import im;
                    im.library = lib, im.name = name, im.ordinal = ordinal, im.thunk = record, im.forward = user;
                    std::unique_lock lock(thunks_mutex_);
                    thunks_[record] = std::move(im);
                    has_thunks_ = true;
                    continue;
                }
                // sc 2; blr; nop; nop
                store32(record + 0, 0x44000042);
                store32(record + 4, 0x4E800020);
                store32(record + 8, 0x60000000);
                store32(record + 12, 0x60000000);
                Import im;
                im.library = lib, im.name = name, im.ordinal = ordinal, im.thunk = record;
                auto fn = registry().find(lib + "!" + name);
                if (fn != registry().end()) im.fn = fn->second;
                if (executable) {
                    by_thunk_[record] = imports_.size();
                    imports_.push_back(std::move(im));
                } else {
                    // a module loaded while threads run: with add_thunk's
                    std::unique_lock lock(thunks_mutex_);
                    thunks_[record] = std::move(im);
                    has_thunks_ = true;
                }
            }
        }
    }
}

void Kernel::remove_thunks(uint32_t begin, uint32_t end) {
    std::unique_lock lock(thunks_mutex_);
    for (auto it = thunks_.lower_bound(begin); it != thunks_.end() && it->first < end;)
        retired_thunks_.push_back(thunks_.extract(it++));
}

void Kernel::set_generated(const Generated *table, size_t count, const uint32_t *resumes, size_t resume_count) {
    generated_ = table, generated_count_ = count;
    generated_resumes_ = resumes, generated_resume_count_ = resume_count;
}

bool Kernel::resumable(uint32_t target) const {
    if (std::binary_search(generated_resumes_, generated_resumes_ + generated_resume_count_, target)) return true;
    if (const CodeMap *map = code_map_.load(std::memory_order_acquire))
        for (const CodeMap::Range &r : map->ranges)
            if (target - r.begin < r.size)
                return r.module->resumes &&
                       std::binary_search(r.module->resumes, r.module->resumes + r.module->resume_count, target);
    return false;
}

void Kernel::set_generated_modules(const ppc::GeneratedModule *const *modules, size_t count) {
    std::lock_guard lock(code_->mutex);
    code_->available.assign(modules, modules + count);
}

std::pair<uintptr_t, uintptr_t> Kernel::generated_code_span() {
    uintptr_t low = UINTPTR_MAX, high = 0;
    auto take = [&](void (*fn)(ppc::Context &)) {
        if (!fn) return;
        uintptr_t a = reinterpret_cast<uintptr_t>(fn);
        low = std::min(low, a), high = std::max(high, a);
    };
    {
        std::lock_guard lock(code_->mutex);
        for (const ppc::GeneratedModule *m : code_->available)
            for (size_t i = 0; i < m->count; i++) take(m->functions[i].fn);
    }
    for (size_t i = 0; i < generated_count_; i++) take(generated_[i].fn);
    return low < high ? std::pair{low, high} : std::pair<uintptr_t, uintptr_t>{0, 0};
}

std::string Kernel::add_module_code(const Xex &xex) {
    std::lock_guard lock(code_->mutex);
    if (code_->available.empty()) return {};
    // the base and size first: the digest is computed only for a candidate
    uint32_t base = xex.base_address, size = uint32_t(xex.image.size());
    const ppc::GeneratedModule *match = nullptr, *other = nullptr;
    uint64_t digest = 0;
    bool digested = false;
    for (const ppc::GeneratedModule *m : code_->available) {
        if (m->base != base || m->size != size) continue;
        if (!digested) digest = image_digest(xex.image), digested = true;
        if (m->digest == digest) {
            match = m;
            break;
        }
        other = m;
    }
    if (!match)
        return other ? std::string("interpreted: its image is not the one ") + other->name +
                           "'s generated code was made from"
                     : "interpreted: no generated code for it";
    auto [it, made] = code_->ranges.try_emplace(match);
    CodeMap::Range &r = it->second;
    if (made && match->count) {
        uint32_t begin = UINT32_MAX, end = 0;
        for (size_t i = 0; i < match->count; i++)
            begin = std::min(begin, match->functions[i].address), end = std::max(end, match->functions[i].address + 4);
        auto table = std::make_unique<CodeMap::Fn[]>((end - begin) / 4);
        for (size_t i = 0; i < match->count; i++)
            table[(match->functions[i].address - begin) / 4] = match->functions[i].fn;
        r = {begin, end - begin, table.get(), match};
        code_->tables.push_back(std::move(table));
    }
    code_->loaded[base] = match;
    publish_code();
    // its hook slots bound to the port's hooks, its names taken in
    hooks::bind(*match);
    return "generated code, " + std::to_string(match->count) + " functions";
}

void Kernel::remove_module_code(uint32_t base) {
    std::lock_guard lock(code_->mutex);
    auto it = code_->loaded.find(base);
    if (it == code_->loaded.end()) return;
    hooks::unbind(*it->second);
    code_->loaded.erase(it);
    publish_code();
}

// (code_->mutex held)
void Kernel::publish_code() {
    auto map = std::make_unique<CodeMap>();
    for (const auto &[base, m] : code_->loaded) {
        const CodeMap::Range &r = code_->ranges[m];
        if (r.size) map->ranges.push_back(r);
    }
    code_map_.store(map.get(), std::memory_order_release);
    code_->maps.push_back(std::move(map));
}

void Kernel::attach(ppc::Context &c) {
    c.base = memory_.base();
    c.system = &system_;
    // the deterministic mode counts the yield points (system_.yield)
    c.yield_counting = deterministic_yield_units_ != 0;
    c.yield_budget = 0;
    c.spin_hints = 0;
}

void Kernel::park() {
    Thread *t = current_thread();
    if (!t) return;
    std::unique_lock l(pause_mutex_);
    const bool was = t->guest_running.load(std::memory_order_seq_cst);
    for (;;) {
        t->guest_running.store(false, std::memory_order_seq_cst);
        pause_cv_.notify_all();
        pause_cv_.wait(l, [&] { return !pause_requested_.load(std::memory_order_seq_cst); });
        t->guest_running.store(was, std::memory_order_seq_cst);
        if (!was || !pause_requested_.load(std::memory_order_seq_cst)) break;
    }
}

bool Kernel::pause(uint32_t timeout_ms) {
    pause_requested_.store(true, std::memory_order_seq_cst);
    const bool exact = deterministic();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        uint32_t running = 0;
        {
            std::lock_guard l(objects_->lock);
            for (Thread *t : objects_->threads) {
                if (!t->guest_running.load(std::memory_order_seq_cst)) continue;
                running++;
                // Not in the deterministic mode, the yield points count only for
                // this: the thread's next back edge calls System::yield, which
                // parks it. (Written from here as it runs, as the deterministic
                // mode's watchdog does: a store it misses is made again.)
                if (!exact && t->context) {
                    __atomic_store_n(&t->context->yield_budget, int64_t(-1), __ATOMIC_RELAXED);
                    __atomic_store_n(&t->context->yield_counting, true, __ATOMIC_RELEASE);
                }
            }
        }
        pause_unparked_ = running;
        if (!running) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::unique_lock l(pause_mutex_);
        pause_cv_.wait_for(l, std::chrono::milliseconds(2));
    }
}

void Kernel::resume() {
    {
        std::lock_guard l(pause_mutex_);
        pause_requested_.store(false, std::memory_order_seq_cst);
    }
    pause_cv_.notify_all();
}

void Kernel::run_import(ppc::Context &c, const Import &import) {
    // in the kernel: a safe point for a pause, on the way in and out
    GuestCodeScope in_kernel(*this, false);
    // (nothing of this frame outlives the count: a host stack may be dropped
    // while it waits in the export, KeSetCurrentStackPointers's switch)
    // (counted on the import itself: no string made, no lock taken, per call)
    if (import.calls.n.fetch_add(1, std::memory_order_relaxed) == 0 && !import.fn) {
        // the first call of an import with no implementation is logged (once per
        // import: a name bound twice, by ordinal and by name, logs for each)
        std::lock_guard<std::mutex> lock(counts_mutex_);
        if (unimplemented_logged_.insert(import.library + "!" + import.name).second)
            std::fprintf(stderr, "kernel: %s!%s is not implemented (r3 0x%08X, r4 0x%08X, from 0x%08X)\n",
                         import.library.c_str(), import.name.c_str(), uint32_t(c.r[3]), uint32_t(c.r[4]),
                         uint32_t(c.lr));
    }
    note_kernel_call(import, uint32_t(c.lr));
    // the deterministic mode's clock moves, and another thread may run
    objects_->scheduler->advance(deterministic_call_cost_);
    // a suspension or kernel APCs waiting for this thread happen here
    if (Thread *t = current_thread(); t && t->pending.load(std::memory_order_relaxed)) objects_->safe_point(*t);
    if (import_hook_) import_hook_(import_hook_user_, import, c, false);
    // (the lockstep check compares at an import, and starts its next segment
    // from what the import left)
    if (lockstep_) [[unlikely]]
        lockstep_->before_import(c, import);
    if (import.fn) import.fn(*this, c);
    else result(c, 0);
    if (lockstep_) [[unlikely]]
        lockstep_->after_import(c);
    if (import_hook_) import_hook_(import_hook_user_, import, c, true);
}

bool Kernel::try_call(ppc::Context &c, uint32_t target, bool hooks) {
    // the port's hooks, where a page holds any (a function with a slot runs
    // its own, below)
    if (hooks && hooks::in_hooked_page(target)) [[unlikely]]
        if (call_hooked(c, target)) return true;
    if (generated_count_) {
        const Generated *end = generated_ + generated_count_;
        const Generated *it =
            std::lower_bound(generated_, end, target, [](const Generated &g, uint32_t a) { return g.address < a; });
        if (it != end && it->address == target && it->fn) { // (none: a function left out)
            run_generated(c, target, it->fn);
            return true;
        }
    }
    // the module whose generated code holds the address, then its slot
    if (const CodeMap *map = code_map_.load(std::memory_order_acquire)) {
        for (const CodeMap::Range &r : map->ranges) {
            uint32_t offset = target - r.begin;
            if (offset >= r.size) continue;
            if (CodeMap::Fn fn = r.table[offset / 4]; fn && !(offset & 3)) {
                run_generated(c, target, fn);
                return true;
            }
            break;
        }
    }
    auto thunk = by_thunk_.find(target);
    if (thunk != by_thunk_.end()) {
        run_import(c, imports_[thunk->second]);
        return true;
    }
    if (const Import *added = find_thunk(target)) {
        if (added->forward) {
            // a game library's thunk: what its lis/ori r11, mtctr r11, bctr
            // do, then the export, generated or interpreted
            c.r[11] = c.ctr = added->forward;
            dispatch(c, added->forward);
        } else {
            run_import(c, *added);
        }
        return true;
    }
    return false;
}

bool Kernel::call_hooked(ppc::Context &c, uint32_t target) {
    hooks::Pin pin;
    const hooks::Entry *e = pin.find(target);
    if (!e || !e->function() || hooks::slotted(target)) return false;
    hooks::run(c, *e, nullptr); // the original through run_original
    return true;
}

void Kernel::run_original(ppc::Context &c, uint32_t address) {
    // the function as dispatch runs it, its hooks left out: generated code,
    // an import, else the interpreter until it returns
    const uint32_t until = uint32_t(c.lr) & ~3u, stack_top = uint32_t(c.r[1]);
    // (a function with a hook slot asks only when its body was left out of
    // the generated code, game_build's left_out_original_stub: its table
    // entry is the wrapper that ran the hooks, so the interpreter it is)
    if (!hooks::slotted(address) && try_call(c, address, false)) return;
    interpreted_++;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        interpreted_targets_[address]++;
    }
    if (entry_hook_) entered(address);
    interpret_from(c, address, until, stack_top);
}

const Import *Kernel::hook_marker(uint32_t address) {
    // named by the hooks there now ("before count_frames, replace clamp:
    // Update (0x82012340)"): a marker per text, so a reload's new hooks get
    // theirs
    std::string hooks;
    if (const hooks::Entry *e = hooks::find(address)) {
        auto add = [&](const char *kind, const std::string &name) {
            hooks += (hooks.empty() ? "" : ", ") + std::string(kind) + (name.empty() ? "" : " " + name);
        };
        for (const auto &h : e->before) add("before", h.name);
        if (e->replace.fn) add("replace", e->replace.name);
        for (const auto &h : e->after) add("after", h.name);
        for (const auto &h : e->mid) add("mid", h.name);
    }
    std::string name = port::name_of(address);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", address);
    std::string text = (hooks.empty() ? std::string() : hooks + ": ") + (name.empty() ? hex : name + " (" + hex + ")");
    std::lock_guard lock(hook_markers_mutex_);
    auto [it, made] = hook_markers_.try_emplace(text);
    if (made) it->second.library = "port", it->second.name = text, it->second.thunk = address;
    return &it->second;
}

void Kernel::run_generated(ppc::Context &c, uint32_t target, void (*fn)(ppc::Context &)) {
    if (lockstep_ && lockstep_->wanted(c, target)) [[unlikely]]
        return lockstep_->check(c, target, fn);
    fn(c);
}

bool Kernel::enable_lockstep(const LockstepOptions &options, std::string &error) {
    auto lockstep = std::make_unique<Lockstep>(*this, options);
    if (!lockstep->usable(error)) return false;
    lockstep_ = std::move(lockstep);
    return true;
}
LockstepCounts Kernel::lockstep_counts() const { return lockstep_ ? lockstep_->counts() : LockstepCounts{}; }
std::string Kernel::lockstep_report() const { return lockstep_ ? lockstep_->report() : std::string(); }

// A return to another frame (docs/runtime.md, "Code"; ppc::Unwind): a
// longjmp, a catch's continuation, a stack switched back to elsewhere than
// where it left; an interpreted blr out of the frames its dispatch began in,
// or a checked return of generated code that switches the stack. Thrown
// through the host frames (C++ cannot return into the middle of one) to the
// innermost frame entered with r1 above the restored one, which holds the
// guest's frame at r1: a generated function with a resume site there goes on
// at it; a dispatch interprets on from the target, the rest of the functions
// between interpreted, until the guest returns to that dispatch's own return
// address and stack (a function's start on the way runs its generated code).
// A dispatch whose stack top is the restored r1 leaves the return to its
// caller's frame when that frame's generated code takes it.
void Kernel::dispatch(ppc::Context &c, uint32_t target) {
    if (stopped) return;
    // an indirect call moves the deterministic mode's clock and may hand over
    // (a loop that waits on another thread through virtual calls gets there)
    if (deterministic_dispatch_cost_) objects_->scheduler->advance(deterministic_dispatch_cost_);
    // what a return from this call comes back to
    const uint32_t until = uint32_t(c.lr), stack_top = uint32_t(c.r[1]);
    uint32_t pc = target;
    for (bool unwound = false;;) {
        try {
            if (!unwound) {
                if (try_call(c, target)) return;
            } else {
                // a function's start: its code, then on where it returns
                while (!stopped && try_call(c, pc)) {
                    pc = uint32_t(c.lr) & ~3u;
                    uint32_t sp = uint32_t(c.r[1]);
                    if (pc == until && sp >= stack_top) return;
                    if (sp > stack_top) {
                        unwinds_thrown++;
                        throw ppc::Unwind{pc};
                    }
                }
                if (stopped) return;
            }
            interpreted_++;
            {
                std::lock_guard<std::mutex> lock(counts_mutex_);
                interpreted_targets_[pc]++;
            }
            if (!unwound && entry_hook_) entered(pc);
            interpret_from(c, pc, until, stack_top);
            return;
        } catch (const ppc::Unwind &u) {
            // a frame of this call's, or one further up
            uint32_t sp = uint32_t(c.r[1]);
            if (sp > stack_top || (sp == stack_top && resumable(u.target))) throw;
            unwinds_by_dispatch++;
            pc = u.target, unwound = true;
        }
    }
}

void Kernel::run_from(ppc::Context &c, uint32_t pc) {
    const uint32_t start = pc;
    while (!stopped) {
        if (try_call(c, pc)) {
            pc = uint32_t(c.lr) & ~3u; // where it returned
            continue;
        }
        interpreted_++;
        {
            std::lock_guard<std::mutex> lock(counts_mutex_);
            interpreted_targets_[pc]++;
        }
        if (pc == start && entry_hook_) entered(pc); // a start, not where a return went
        interpret_from(c, pc, SENTINEL, 0);
        return;
    }
}

void Kernel::interpret_from(ppc::Context &c, uint32_t pc, uint32_t until, uint32_t stack_top) {
    ppc::Stopped s = ppc::interpret(c, pc, until, step_limit, stack_top);
    if (s.reason == ppc::Stopped::Unwound) {
        unwinds_thrown++;
        throw ppc::Unwind{s.pc};
    }
    if (s.reason != ppc::Stopped::Returned && !stopped) {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "the interpreter stopped at 0x%08X: %s", s.pc,
                      s.reason == ppc::Stopped::Invalid ? "an invalid instruction"
                      : s.reason == ppc::Stopped::Steps ? "the step limit"
                                                        : (std::string(ppc::info(s.op).name) + " has no semantics").c_str());
        stop(reason);
    }
}

void Kernel::call(ppc::Context &c, uint32_t address) {
    // guest code from here (a thread's start, a callback the kernel makes)
    GuestCodeScope running(*this, true);
    c.lr = SENTINEL;
    uint32_t target;
    try {
        dispatch(c, address);
        return;
    } catch (const ppc::Unwind &u) {
        target = u.target;
        unwinds_by_dispatch++;
    }
    // r1 above every frame of this call: a longjmp out of a callback the
    // kernel made (a stack switch runs on a host stack of its own, so it
    // ends up here no more). The guest goes on from there until it returns
    // from this call (a function's start on the way generated, the rest
    // interpreted), its own unwinds no longer checked; one from a dispatch
    // below comes here too.
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "kernel: a blr to 0x%08X with r1 0x%08X left the frames of a call to 0x%08X\n", target,
                     uint32_t(c.r[1]), address);
    for (;;) {
        try {
            while (target != SENTINEL && !stopped && try_call(c, target)) target = uint32_t(c.lr) & ~3u;
            if (target == SENTINEL || stopped) return;
            interpreted_++;
            {
                std::lock_guard<std::mutex> lock(counts_mutex_);
                interpreted_targets_[target]++;
            }
            interpret_from(c, target, SENTINEL, 0);
            return;
        } catch (const ppc::Unwind &u) {
            target = u.target;
            unwinds_by_dispatch++;
        }
    }
}

void Kernel::stop(const std::string &reason) {
    if (stopped.exchange(true)) return;
    stop_reason = reason;
    CrashReport report = make_crash_report(*this, reason);
    stop_report = std::move(report.text), stop_stack = std::move(report.stack);
}

bool Kernel::guest_function(uint32_t address, const char *&module, uint32_t &start) const {
    // the last function starting at or before address, if within 64 KB
    auto find = [&](const Generated *f, size_t n, uint32_t &found) {
        const Generated *end = f + n;
        const Generated *it = std::upper_bound(f, end, address,
                                               [](uint32_t a, const Generated &g) { return a < g.address; });
        if (it == f || address - (it - 1)->address >= 0x10000) return false;
        return found = (it - 1)->address, true;
    };
    if (const CodeMap *map = code_map_.load(std::memory_order_acquire))
        for (const CodeMap::Range &r : map->ranges) {
            const ppc::GeneratedModule *m = r.module;
            if (address - m->base >= m->size) continue;
            module = m->name;
            return find(m->functions, m->count, start);
        }
    module = nullptr;
    return generated_count_ && find(generated_, generated_count_, start);
}

bool Kernel::guest_function_of_host(const void *host, uint32_t &start) const {
    const uintptr_t at = uintptr_t(host);
    uintptr_t best = 0;
    uint32_t found = 0;
    auto scan = [&](const Generated *f, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uintptr_t p = uintptr_t(f[i].fn);
            if (p && p <= at && p > best) best = p, found = f[i].address;
        }
    };
    scan(generated_, generated_count_);
    if (const CodeMap *map = code_map_.load(std::memory_order_acquire))
        for (const CodeMap::Range &r : map->ranges) scan(r.module->functions, r.module->count);
    if (!best || at - best >= 0x10000) return false;
    return start = found, true;
}

// every import's count, summed by export name (the executable's imports, the
// modules' thunks, and those of modules unloaded since)
void Kernel::each_import(const std::function<void(const Import &)> &visit) const {
    for (const Import &im : imports_) visit(im);
    std::shared_lock lock(thunks_mutex_);
    for (const auto &[address, im] : thunks_) visit(im);
    for (const auto &node : retired_thunks_) visit(node.mapped());
}
std::map<std::string, uint64_t> Kernel::calls() const {
    std::map<std::string, uint64_t> out;
    each_import([&](const Import &im) {
        if (uint64_t n = im.calls.n.load(std::memory_order_relaxed)) out[im.library + "!" + im.name] += n;
    });
    std::lock_guard<std::mutex> lock(counts_mutex_);
    for (const auto &[key, n] : calls_) out[key] += n; // what else counted by name (the devices)
    return out;
}
std::map<std::string, uint64_t> Kernel::unimplemented() const {
    std::map<std::string, uint64_t> out;
    each_import([&](const Import &im) {
        if (uint64_t n = im.calls.n.load(std::memory_order_relaxed); n && !im.fn) out[im.library + "!" + im.name] += n;
    });
    std::lock_guard<std::mutex> lock(counts_mutex_);
    for (const auto &[key, n] : unimplemented_) out[key] += n; // the devices' (mmio)
    return out;
}
void Kernel::entered(uint32_t address) {
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        if (!entries_seen_.emplace(address, true).second) return;
    }
    entry_hook_(entry_hook_user_, address);
}

std::unordered_map<uint32_t, uint64_t> Kernel::interpreted_targets() const {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    return interpreted_targets_;
}

} // namespace xf::kernel

// what generated code calls (ppc_generated.h)
namespace xf::ppc {
void dispatch(Context &c, uint32_t target) { static_cast<kernel::Kernel *>(c.system->owner)->dispatch(c, target); }
void invalid(Context &c, uint32_t pc) {
    auto &k = *static_cast<kernel::Kernel *>(c.system->owner);
    char reason[64];
    std::snprintf(reason, sizeof(reason), "an invalid instruction at 0x%08X", pc);
    k.stop(reason);
}
[[noreturn]] void unwind(Context &c) {
    static_cast<kernel::Kernel *>(c.system->owner)->unwinds_thrown++;
    throw Unwind{uint32_t(c.lr) & ~3u};
}
void switch_stack(Context &c, uint32_t thunk, uint32_t resume) {
    // KeSetCurrentStackPointers reads it (stacks.cpp); cleared after, in case
    // the thunk led elsewhere
    kernel::Thread *t = kernel::current_thread();
    if (t && t->context.get() != &c) t = nullptr;
    if (t) t->stack_resume = resume;
    static_cast<kernel::Kernel *>(c.system->owner)->dispatch(c, thunk);
    if (t) t->stack_resume = 0;
}
} // namespace xf::ppc
