// The kernel (plan step A4; docs/runtime.md): the exports a game imports,
// the loader that maps its image and binds its imports, and the one rule
// for calls the code cannot make directly (dispatch: generated code, an
// import, else the interpreter). Exports are written family by family,
// translated from Xenia's xboxkrnl and xam (BSD; see LICENSE-xenia), each
// call counted.
//
//   XF_EXPORT(xboxkrnl, KeGetCurrentProcessType) {
//       result(c, 1);
//   }

#pragma once

#include "xenoforge/memory.h"
#include "xenoforge/ppc_context.h"
#include "xenoforge/xex.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xf::kernel {

class Kernel;
using ExportFn = void (*)(Kernel &k, ppc::Context &c);

// Registers an export's implementation by library ("xboxkrnl", "xam") and
// name (Xenia's export tables give the ordinal). XF_EXPORT does it.
bool register_export(const char *library, const char *name, ExportFn fn);

#define XF_EXPORT(library, name)                                                                                       \
    static void xf_export_##library##_##name(::xf::kernel::Kernel &k, ::xf::ppc::Context &c);                         \
    [[maybe_unused]] static const bool xf_registered_##library##_##name =                                              \
        ::xf::kernel::register_export(#library, #name, xf_export_##library##_##name);                                  \
    static void xf_export_##library##_##name([[maybe_unused]] ::xf::kernel::Kernel &k,                                 \
                                             [[maybe_unused]] ::xf::ppc::Context &c)

// ---- arguments and results, as Xenia's shim_utils ----

// the n-th integer argument: r3-r10, then the stack at r1 + 0x54 + 8 * (n - 8)
uint64_t arg64(Kernel &k, ppc::Context &c, int n);
inline uint32_t arg(Kernel &k, ppc::Context &c, int n) { return uint32_t(arg64(k, c, n)); }
// a 32-bit result, sign-extended (SHIM_SET_RETURN_32)
inline void result(ppc::Context &c, uint32_t v) { c.r[3] = uint64_t(int64_t(int32_t(v))); }

constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;

struct Import {
    std::string library, name; // name: the export's, or library_ordinal
    uint16_t ordinal = 0;
    uint32_t thunk = 0;
    ExportFn fn = nullptr;     // nullptr: not implemented yet
};

struct Generated {
    uint32_t address;
    void (*fn)(ppc::Context &);
};

class Kernel {
public:
    explicit Kernel(Memory &memory);
    ~Kernel();

    Memory &memory() { return memory_; }

    // guest memory, big-endian
    uint8_t *host(uint32_t address) { return memory_.translate(address); }
    uint32_t load32(uint32_t address);
    void store32(uint32_t address, uint32_t value);
    uint16_t load16(uint32_t address);
    void store16(uint32_t address, uint16_t value);
    uint64_t load64(uint32_t address);
    void store64(uint32_t address, uint64_t value);

    // Maps the image (loaded by load_xex_image) at its base and binds its
    // imports: function thunks rewritten to sc 2 / blr, variables pointed
    // at the kernel's objects for them.
    bool load(const Xex &xex, std::string &error);
    uint32_t entry_point() const { return entry_; }
    const Xex *xex() const { return xex_; }
    const std::vector<Import> &imports() const { return imports_; }

    // the generated code, if linked (sorted by address)
    void set_generated(const Generated *table, size_t count);

    // A thread's context set up to run here (its System points back).
    void attach(ppc::Context &c);
    // Runs the function at address with the context's registers, until it
    // returns: generated code, an import, or the interpreter.
    void call(ppc::Context &c, uint32_t address);
    // what generated code's dispatch() and the interpreter's calls reach
    void dispatch(ppc::Context &c, uint32_t target);
    bool try_call(ppc::Context &c, uint32_t target); // generated or import: ran it

    // Counts, by export ("library!name"): calls, and calls to imports with
    // no implementation (each logged once).
    std::map<std::string, uint64_t> calls() const;
    std::map<std::string, uint64_t> unimplemented() const;
    uint64_t interpreted_entries() const { return interpreted_; }

    // a variable export's value (the kernel's object for it), by name
    uint32_t variable(const std::string &name);

    // the interpreter's limit per entry (boot stops a run with it)
    uint64_t step_limit = UINT64_MAX;

    // set when the code hit what cannot run (an invalid instruction, an
    // instruction with no semantics): where, and why
    std::atomic<bool> stopped{false};
    std::string stop_reason;

private:
    void run_import(ppc::Context &c, const Import &import);

    Memory &memory_;
    const Xex *xex_ = nullptr;
    uint32_t entry_ = 0;
    ppc::System system_;
    std::vector<Import> imports_;
    std::unordered_map<uint32_t, size_t> by_thunk_;
    const Generated *generated_ = nullptr;
    size_t generated_count_ = 0;
    std::map<std::string, uint32_t> variables_;
    mutable std::mutex counts_mutex_;
    std::map<std::string, uint64_t> calls_, unimplemented_;
    std::atomic<uint64_t> interpreted_{0};
};

} // namespace xf::kernel
