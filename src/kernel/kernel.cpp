// The kernel's core (kernel.h; docs/runtime.md). The import binding
// follows Xenia's XexModule::SetupLibraryImports (src/xenia/cpu/
// xex_module.cc, BSD; see LICENSE-xenia): function thunks become sc 2,
// blr, nop, nop; a function's descriptor record gets 0xDEADC0DE; a
// variable's gets the address of the kernel's object for it.

#include "xenoforge/kernel.h"

#include "xenoforge/exports.h"
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

uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

} // namespace

bool register_export(const char *library, const char *name, ExportFn fn) {
    registry()[std::string(library) + "!" + name] = fn;
    return true;
}

uint64_t arg64(Kernel &k, ppc::Context &c, int n) {
    if (n < 8) return c.r[3 + n];
    return k.load64(uint32_t(c.r[1]) + 0x54 + uint32_t(n - 8) * 8);
}

Kernel::Kernel(Memory &memory) : memory_(memory) {
    system_.owner = this;
    system_.syscall = [](ppc::Context &c) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        auto it = k.by_thunk_.find(c.pc);
        if (it != k.by_thunk_.end()) k.run_import(c, k.imports_[it->second]);
    };
    system_.call = [](ppc::Context &c, uint32_t target) {
        return static_cast<Kernel *>(c.system->owner)->try_call(c, target);
    };
    system_.unimplemented = [](ppc::Context &c, ppc::Opcode op, uint32_t code) {
        Kernel &k = *static_cast<Kernel *>(c.system->owner);
        char reason[96];
        std::snprintf(reason, sizeof(reason), "%s (0x%08X) at 0x%08X has no semantics", ppc::info(op).name, code, c.pc);
        if (!k.stopped.exchange(true)) k.stop_reason = reason;
    };
    system_.time_base = [](ppc::Context &) {
        // the Xenon's time base runs at 50 MHz
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
        return uint64_t(ns / 20);
    };
}

Kernel::~Kernel() = default;

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
    auto it = variables_.find(name);
    if (it != variables_.end()) return it->second;
    // zeroed until the family that owns it fills it in (exports with a
    // value set them through here first)
    uint32_t address = memory_.system_alloc(0x100);
    return variables_[name] = address;
}

bool Kernel::load(const Xex &xex, std::string &error) {
    xex_ = &xex;
    uint32_t base = xex.base_address, size = uint32_t(xex.image.size());
    Heap *heap = memory_.heap_for(base);
    if (!heap) return error = "no heap holds the image's base", false;
    uint32_t pages = (size + heap->page_size() - 1) & ~(heap->page_size() - 1);
    if (!heap->alloc_fixed(base, pages, heap->page_size(), ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE))
        return error = "could not map the image at its base", false;
    std::memcpy(host(base), xex.image.data(), size);
    entry_ = xex.entry_point;

    for (const XexImportLibrary &library : xex.import_libraries) {
        std::string lib = short_library(library.name);
        for (uint32_t record : library.records) {
            uint32_t value = load32(record);
            uint32_t type = value >> 24;
            uint16_t ordinal = uint16_t(value & 0xFFFF);
            const Export *e = find_export(library.name, ordinal);
            std::string name = e ? e->name : lib + "_" + std::to_string(ordinal);
            if (type == 0) {
                if (e && e->kind == ExportKind::Variable) store32(record, variable(name));
                else store32(record, 0xDEADC0DE);
            } else if (type == 1) {
                // sc 2; blr; nop; nop
                store32(record + 0, 0x44000042);
                store32(record + 4, 0x4E800020);
                store32(record + 8, 0x60000000);
                store32(record + 12, 0x60000000);
                Import im;
                im.library = lib, im.name = name, im.ordinal = ordinal, im.thunk = record;
                auto fn = registry().find(lib + "!" + name);
                if (fn != registry().end()) im.fn = fn->second;
                by_thunk_[record] = imports_.size();
                imports_.push_back(std::move(im));
            }
        }
    }
    return true;
}

void Kernel::set_generated(const Generated *table, size_t count) { generated_ = table, generated_count_ = count; }

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
    auto thunk = by_thunk_.find(target);
    if (thunk != by_thunk_.end()) {
        run_import(c, imports_[thunk->second]);
        return true;
    }
    return false;
}

void Kernel::dispatch(ppc::Context &c, uint32_t target) {
    if (stopped) return;
    if (try_call(c, target)) return;
    interpreted_++;
    ppc::Stopped s = ppc::interpret(c, target, uint32_t(c.lr), step_limit);
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
    dispatch(c, address);
}

std::map<std::string, uint64_t> Kernel::calls() const {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    return calls_;
}
std::map<std::string, uint64_t> Kernel::unimplemented() const {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    return unimplemented_;
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
