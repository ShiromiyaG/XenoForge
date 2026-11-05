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

#include "objects.h"
#include "xenoforge/exports.h"
#include "xenoforge/ppc_interp.h"

#include <algorithm>
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

Kernel::Kernel(Memory &memory) : memory_(memory), code_(std::make_unique<Code>()) {
    system_.owner = this;
    system_.syscall = [](ppc::Context &c) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        auto it = k.by_thunk_.find(c.pc);
        if (it != k.by_thunk_.end()) k.run_import(c, k.imports_[it->second]);
        else if (const Import *thunk = k.find_thunk(c.pc)) k.run_import(c, *thunk);
    };
    system_.call = [](ppc::Context &c, uint32_t target) {
        return static_cast<Kernel *>(c.system->owner)->try_call(c, target);
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
        if (!k.stopped.exchange(true)) k.stop_reason = reason;
    };
    system_.time_base = [](ppc::Context &c) {
        // the Xenon's time base runs at 50 MHz: 5 ticks in the scheduler's
        // clock's 100 ns
        return static_cast<Kernel *>(c.system->owner)->objects_->now() * 5;
    };
    // the globals first: the objects and threads use their processes,
    // object types and KeTimeStampBundle
    setup_kernel(*this);
    objects_ = std::make_unique<Objects>(*this);
    // the devices, before any guest thread runs
    setup_video(*this);
    setup_audio(*this);
}

// the devices' threads end while the objects they run on are still here
Kernel::~Kernel() {
    shutdown_audio(*this);
    shutdown_video(*this);
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

void Kernel::set_generated(const Generated *table, size_t count) { generated_ = table, generated_count_ = count; }

void Kernel::set_generated_modules(const ppc::GeneratedModule *const *modules, size_t count) {
    std::lock_guard lock(code_->mutex);
    code_->available.assign(modules, modules + count);
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
    return "generated code, " + std::to_string(match->count) + " functions";
}

void Kernel::remove_module_code(uint32_t base) {
    std::lock_guard lock(code_->mutex);
    if (code_->loaded.erase(base)) publish_code();
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
}

void Kernel::run_import(ppc::Context &c, const Import &import) {
    std::string key = import.library + "!" + import.name;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        calls_[key]++;
        if (!import.fn && unimplemented_[key]++ == 0)
            std::fprintf(stderr, "kernel: %s is not implemented (r3 0x%08X, r4 0x%08X, from 0x%08X)\n", key.c_str(),
                         uint32_t(c.r[3]), uint32_t(c.r[4]), uint32_t(c.lr));
    }
    // a suspension or kernel APCs waiting for this thread happen here
    if (Thread *t = current_thread(); t && t->pending.load(std::memory_order_relaxed)) objects_->safe_point(*t);
    if (import.fn) import.fn(*this, c);
    else result(c, 0);
}

bool Kernel::try_call(ppc::Context &c, uint32_t target) {
    if (generated_count_) {
        const Generated *end = generated_ + generated_count_;
        const Generated *it =
            std::lower_bound(generated_, end, target, [](const Generated &g, uint32_t a) { return g.address < a; });
        if (it != end && it->address == target) {
            it->fn(c);
            return true;
        }
    }
    // the module whose generated code holds the address, then its slot
    if (const CodeMap *map = code_map_.load(std::memory_order_acquire)) {
        for (const CodeMap::Range &r : map->ranges) {
            uint32_t offset = target - r.begin;
            if (offset >= r.size) continue;
            if (CodeMap::Fn fn = r.table[offset / 4]; fn && !(offset & 3)) {
                fn(c);
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

// A longjmp (docs/runtime.md, "Code"): an interpreted blr out of the frames
// its dispatch began in, to a frame further up. Thrown through the generated
// code's frames (C++ cannot return into the middle of one) to the dispatch
// whose frames hold the stack it restored, which interprets on from where it
// went: the rest of the functions between, interpreted, until the guest
// returns to that dispatch's own return address and stack.
struct GuestUnwind {
    uint32_t target;
};

void Kernel::dispatch(ppc::Context &c, uint32_t target) {
    if (stopped) return;
    // what a return from this call comes back to
    const uint32_t until = uint32_t(c.lr), stack_top = uint32_t(c.r[1]);
    uint32_t pc = target;
    for (bool unwound = false;;) {
        try {
            if (!unwound && try_call(c, target)) return;
            interpreted_++;
            {
                std::lock_guard<std::mutex> lock(counts_mutex_);
                interpreted_targets_[pc]++;
            }
            interpret_from(c, pc, until, stack_top);
            return;
        } catch (const GuestUnwind &u) {
            // a frame of this call's, or one further up
            if (uint32_t(c.r[1]) > stack_top) throw;
            pc = u.target, unwound = true;
        }
    }
}

void Kernel::interpret_from(ppc::Context &c, uint32_t pc, uint32_t until, uint32_t stack_top) {
    ppc::Stopped s = ppc::interpret(c, pc, until, step_limit, stack_top);
    if (s.reason == ppc::Stopped::Unwound) throw GuestUnwind{s.pc};
    if (s.reason != ppc::Stopped::Returned && !stopped.exchange(true)) {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "the interpreter stopped at 0x%08X: %s", s.pc,
                      s.reason == ppc::Stopped::Invalid ? "an invalid instruction"
                      : s.reason == ppc::Stopped::Steps ? "the step limit"
                                                        : (std::string(ppc::info(s.op).name) + " has no semantics").c_str());
        stop_reason = reason;
    }
}

void Kernel::call(ppc::Context &c, uint32_t address) {
    c.lr = SENTINEL;
    uint32_t target;
    try {
        dispatch(c, address);
        return;
    } catch (const GuestUnwind &u) {
        target = u.target;
    }
    // r1 above every frame of this call: a fiber whose stack lies above the
    // thread's (or a longjmp out of a callback the kernel made). The guest
    // goes on from there, interpreted, until it returns from this call, its
    // own unwinds no longer checked; one from a dispatch below (generated
    // code it calls switching fibers again) comes here too.
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "kernel: a blr to 0x%08X with r1 0x%08X left the frames of a call to 0x%08X\n", target,
                     uint32_t(c.r[1]), address);
    for (;;) {
        interpreted_++;
        {
            std::lock_guard<std::mutex> lock(counts_mutex_);
            interpreted_targets_[target]++;
        }
        try {
            interpret_from(c, target, SENTINEL, 0);
            return;
        } catch (const GuestUnwind &u) {
            target = u.target;
        }
    }
}

std::map<std::string, uint64_t> Kernel::calls() const {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    return calls_;
}
std::map<std::string, uint64_t> Kernel::unimplemented() const {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    return unimplemented_;
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
    if (!k.stopped.exchange(true)) k.stop_reason = reason;
}
} // namespace xf::ppc
