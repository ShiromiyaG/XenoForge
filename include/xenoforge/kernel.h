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
#include "xenoforge/ppc_module.h"
#include "xenoforge/xex.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xf::gpu {
class CommandProcessor;
}

namespace xf::kernel {

class Kernel;
class Objects; // the objects and threads (src/kernel/objects.h)
using ExportFn = void (*)(Kernel &k, ppc::Context &c);

// Registers an export's implementation by library ("xboxkrnl", "xam") and
// name (Xenia's export tables give the ordinal). XF_EXPORT does it.
bool register_export(const char *library, const char *name, ExportFn fn);
// an export's implementation (nullptr: none), for the kernel's own calls
// and the tests
ExportFn find_export_fn(const char *library, const char *name);

#define XF_EXPORT(library, name)                                                                                       \
    static void xf_export_##library##_##name(::xf::kernel::Kernel &k, ::xf::ppc::Context &c);                         \
    [[maybe_unused]] static const bool xf_registered_##library##_##name =                                              \
        ::xf::kernel::register_export(#library, #name, xf_export_##library##_##name);                                  \
    static void xf_export_##library##_##name([[maybe_unused]] ::xf::kernel::Kernel &k,                                 \
                                             [[maybe_unused]] ::xf::ppc::Context &c)

// ---- arguments and results, as Xenia's shim_utils ----

// the n-th integer argument: r3-r10, then the stack at r1 + 0x54 + 8 * (n - 8),
// a doubleword there as varargs and 64-bit values are passed (Xenia's
// get_arg_64)
uint64_t arg64(Kernel &k, ppc::Context &c, int n);
// a 32-bit argument: on the stack, the word at that address, where the
// caller's stw puts it (Xenia's get_arg_32; VdSwap's callers store its ninth
// and tenth at 0x54 and 0x5C)
uint32_t arg(Kernel &k, ppc::Context &c, int n);
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
    // a game library's function (a DLL's export): where the thunk jumps
    uint32_t forward = 0;
};

using Generated = ppc::GeneratedFunction;

// The modules family's parts (modules.cpp). setup_kernel, as the kernel
// starts: the guest globals (object types, processes), the kernel's memory
// and variables at Xenia's addresses, the kernel modules. setup_modules, at
// the load before the imports are bound: the executable's module with its
// loader entry, the title process's TLS and stack size.
void setup_kernel(Kernel &k);
void setup_modules(Kernel &k, const Xex &xex);
// The executable's guest path, set before load() ("game:\default.xex" if
// never): its module's path, and the directory a module named bare is
// loaded from.
void set_executable_path(Kernel &k, const std::string &path);
// A game's own library an image imports from (no system library: a DLL),
// for the binding, as Xenia's XexModule::LoadContinue finds it: its loader
// entry (its HMODULE), loaded first if it is not, 0 if it cannot be; and the
// address of a module's export by ordinal (0: none).
uint32_t import_module(Kernel &k, const std::string &library);
uint32_t module_export(Kernel &k, uint32_t hmodule, uint16_t ordinal);
// A guest thread starts or ends (Xenia's KernelState::OnThreadExecute and
// OnThreadExit): each DLL loaded has its entry point called on it, with
// DLL_THREAD_ATTACH (DLL_PROCESS_ATTACH if not attached yet) or
// DLL_THREAD_DETACH.
void modules_thread_start(Kernel &k, ppc::Context &c);
void modules_thread_end(Kernel &k, ppc::Context &c);
// the modules, for boot: the kernel's the game loaded, the executable and
// the DLLs (path, range, entry, load count, generated code), and those
// unloaded
std::string modules_report(Kernel &k);
// the file name of the game's module loaded at address ("" if none), for
// boot's report of where the interpreter was entered
std::string module_at(Kernel &k, uint32_t address);
// the guest stacks, for boot (stacks.cpp): the switches between them, the
// host fibers made and dropped, and the returns to another frame
// (ppc::Unwind) and what took them
std::string stacks_report(Kernel &k);
// The file system (files.cpp, xam.cpp; Xenia's VirtualFileSystem and its
// mounts): the game's disc at \Device\Cdrom0, or a bare executable's
// directory at \Device\Harddisk0\Partition1 (read-only), with game: and d:
// linked to it; the writable storage root in Xenia's layout, its cache
// partitions (cache:, cache0:, cache1:) and content root (content/, where
// saves go, interchangeable with Xenia's).
bool mount_disc(Kernel &k, const std::filesystem::path &iso, std::string &error);
bool mount_directory(Kernel &k, const std::filesystem::path &dir, std::string &error);
bool mount_storage(Kernel &k, const std::filesystem::path &root, std::string &error);

// The DVD tray's state as the system management controller has it
// (misc.cpp; Xenia's X_DVD_TRAY_STATE: 0 open, 2 closed), for
// XamLoaderGetDvdTrayState.
uint32_t smc_tray_state(Kernel &k);

// the title and system processes (X_KPROCESS) in the guest globals
uint32_t title_process(Kernel &k);
uint32_t system_process(Kernel &k);
// a process's TLS (Xenia's SetProcessTLSVars): the slots free in its bitmap
void set_process_tls(Kernel &k, uint32_t process, uint32_t slots, uint32_t data_size, uint32_t static_data);

// The video family's parts (video.cpp). setup_video, as the kernel starts:
// the GPU's registers as MMIO and its command processor (its threads start
// with the first call into the video driver). shutdown_video, as the kernel
// ends: those threads stopped and joined. video_report: what the GPU did
// (packets, draws, resolves, swaps, interrupts), for boot.
void setup_video(Kernel &k);
void shutdown_video(Kernel &k);
std::string video_report(Kernel &k);
// The GPU's command processor, for installing its Sink (the renderer)
// before the game starts; the Sink's calls then come on the "GPU commands"
// thread.
gpu::CommandProcessor &gpu_command_processor(Kernel &k);

// The sound family's parts (audio.cpp, xma.cpp; its seams in
// xenoforge/audio.h). setup_audio, as the kernel starts: the XMA decoder's
// registers as MMIO and its contexts in physical memory (the threads start
// with the first call into the audio driver). shutdown_audio, as the kernel
// ends: those threads stopped and joined. audio_report: what it did (render
// driver frames, XMA contexts and kicks), for boot.
void setup_audio(Kernel &k);
void shutdown_audio(Kernel &k);
std::string audio_report(Kernel &k);

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
    // Places an image at its base: its pages reserved and committed,
    // read-write, the rest of the last page zeroed (Xenia's
    // XexModule::ReadImage*); false, and why, when they are taken.
    bool map_image(const Xex &xex, std::string &error);
    // Binds an image's imports (the executable's in load(), a module's as
    // XexLoadImage loads it; Xenia's SetupLibraryImports): a system
    // library's as load() says; a game's library's (import_module) to its
    // exports, a function's thunk made to jump there, a variable given its
    // address.
    void bind_imports(const Xex &xex);
    // Drops the thunks written in [begin, end) (an image unloaded).
    void remove_thunks(uint32_t begin, uint32_t end);
    uint32_t entry_point() const { return entry_; }
    const Xex *xex() const { return xex_; }
    const std::vector<Import> &imports() const { return imports_; }

    // The generated code (docs/runtime.md, "Code"). A game library carries a
    // table per module (ppc_module.h), set before load(): when an image is
    // placed at its base (the executable in load(), a DLL as XexLoadImage
    // loads it), the table made from the same image (its base, size and
    // digest) runs the code in its range, until the image is unloaded; an
    // image no table matches (another version, a title update) runs in the
    // interpreter. add_module_code says which, for the modules' report.
    void set_generated_modules(const ppc::GeneratedModule *const *modules, size_t count);
    std::string add_module_code(const Xex &xex);
    void remove_module_code(uint32_t base);
    // Functions at any address, looked up before the modules' (sorted by
    // address; the tests' host functions), and their resume sites (sorted;
    // GeneratedModule::resumes).
    void set_generated(const Generated *table, size_t count, const uint32_t *resumes = nullptr,
                       size_t resume_count = 0);

    // The title's main thread (Xenia's KernelState::LaunchModule): the
    // entry point on a guest thread with the executable's default stack and
    // TLS, started on its own host thread.
    bool launch(std::string &error);
    // Waits up to timeout_ms for the main thread to end: true, and its exit
    // code, if it did.
    bool wait_main(uint32_t timeout_ms, uint32_t *exit_code = nullptr);
    // the guest threads alive: each one's state and where it was
    std::string thread_report();
    // the context of the guest thread on this host thread (nullptr off one)
    static ppc::Context *current_context();
    Objects &objects() { return *objects_; }

    // A thread's context set up to run here (its System points back).
    void attach(ppc::Context &c);
    // Runs the function at address with the context's registers, until it
    // returns: generated code, an import, or the interpreter.
    void call(ppc::Context &c, uint32_t address);
    // what generated code's dispatch() and the interpreter's calls reach
    void dispatch(ppc::Context &c, uint32_t target);
    bool try_call(ppc::Context &c, uint32_t target); // generated or import: ran it
    // Runs the guest from pc with no frame to return to (a guest stack's
    // host stack, from where the stack was entered): the function at pc,
    // generated or imported, then on where it returns; the interpreter where
    // there is none. Returns only when the kernel stops (or the guest
    // returns to the sentinel). An Unwind is the caller's to catch.
    void run_from(ppc::Context &c, uint32_t pc);
    // whether generated code takes an Unwind to target in its own frame (a
    // resume site of a loaded module's function, or of the tests' table)
    bool resumable(uint32_t target) const;

    // Counts, by export ("library!name"): calls, and calls to imports with
    // no implementation (each logged once).
    std::map<std::string, uint64_t> calls() const;
    std::map<std::string, uint64_t> unimplemented() const;
    uint64_t interpreted_entries() const { return interpreted_; }
    // the addresses the interpreter was entered at, and how often: with
    // generated code linked, what discovery missed
    std::unordered_map<uint32_t, uint64_t> interpreted_targets() const;

    // a variable export's value (the kernel's object for it), by name
    uint32_t variable(const std::string &name);
    // where a variable export lives, set before load() binds the imports
    // (the modules family puts them at Xenia's addresses)
    void set_variable(const std::string &name, uint32_t address);

    // Writes a thunk (sc 2; blr) at address that runs library!name as an
    // import runs: counted, its absence logged once (Xenia's trampolines:
    // XexGetProcedureAddress, the object types' procedures). The registry
    // takes any library name, so the kernel's own guest-callable helpers are
    // XF_EXPORT(kernel, Name).
    void add_thunk(uint32_t address, const std::string &library, const std::string &name);
    // whether library!name has an implementation
    static bool implemented(const std::string &library, const std::string &name);

    // A device's registers (Xenia's AddVirtualMappedRange): the 32-bit
    // loads and stores at an address where (address & mask) == base reach
    // read and write (ppc_ops.h's MMIO). Added as the devices start, before
    // guest threads run; one nobody answers for reads 0 and is logged once.
    struct Mmio {
        uint32_t base = 0, mask = 0;
        uint32_t (*read)(void *device, uint32_t address) = nullptr;
        void (*write)(void *device, uint32_t address, uint32_t value) = nullptr;
        void *device = nullptr;
    };
    void add_mmio(const Mmio &range);

    // The guest's clock, as Xenia's Clock unscaled: the system time (a
    // FILETIME, 100 ns since 1601) from the host's at start, the interrupt
    // time (100 ns, monotonic), and the milliseconds since the start.
    uint64_t system_time() const;
    uint64_t interrupt_time() const;
    uint32_t uptime_ms() const;

    // A family's own state, made on first use and destroyed with the kernel
    // (before anything else in it).
    template <class T> T &state() {
        std::lock_guard<std::mutex> lock(states_mutex_);
        std::shared_ptr<void> &p = states_[state_key<T>()];
        if (!p) p = std::make_shared<T>();
        return *static_cast<T *>(p.get());
    }

    // the interpreter's limit per entry (boot stops a run with it)
    uint64_t step_limit = UINT64_MAX;

    // Returns to another frame (ppc::Unwind) thrown, and those a dispatch (or
    // a call the kernel made) took, interpreting on, and a guest stack's
    // start took (stacks.cpp); generated code took the rest at a resume site.
    std::atomic<uint64_t> unwinds_thrown{0}, unwinds_by_dispatch{0}, unwinds_by_stack_start{0};

    // set when the code hit what cannot run (an invalid instruction, an
    // instruction with no semantics): where, and why
    std::atomic<bool> stopped{false};
    std::string stop_reason;

private:
    void bind(const Xex &xex, bool executable);
    void run_import(ppc::Context &c, const Import &import);
    void publish_code();
    // the interpreter from pc until the call returns to until with r1 back
    // at stack_top; a longjmp past stack_top is thrown on (kernel.cpp)
    void interpret_from(ppc::Context &c, uint32_t pc, uint32_t until, uint32_t stack_top);
    // a thunk add_thunk wrote (nullptr if none is at the address)
    const Import *find_thunk(uint32_t address) const;
    const Mmio *find_mmio(uint32_t address) const;
    void unanswered_mmio(const char *what, uint32_t address, uint32_t pc);
    template <class T> static const void *state_key() {
        static const char key = 0;
        return &key;
    }

    Memory &memory_;
    const Xex *xex_ = nullptr;
    uint32_t entry_ = 0;
    ppc::System system_;
    std::vector<Import> imports_;
    std::unordered_map<uint32_t, size_t> by_thunk_;
    const Generated *generated_ = nullptr;
    size_t generated_count_ = 0;
    const uint32_t *generated_resumes_ = nullptr;
    size_t generated_resume_count_ = 0;
    // the modules' generated code (kernel.cpp): the ranges try_call reads, a
    // map replaced whole as modules load and unload, and the rest
    struct CodeMap;
    struct Code;
    std::atomic<const CodeMap *> code_map_{nullptr};
    std::unique_ptr<Code> code_;
    // (a module's imports add to it while threads read it)
    mutable std::shared_mutex variables_mutex_;
    std::map<std::string, uint32_t> variables_;
    mutable std::mutex counts_mutex_;
    std::map<std::string, uint64_t> calls_, unimplemented_;
    std::atomic<uint64_t> interpreted_{0};
    std::unordered_map<uint32_t, uint64_t> interpreted_targets_; // under counts_mutex_
    // add_thunk's (a map: its entries stay where they are)
    mutable std::shared_mutex thunks_mutex_;
    std::map<uint32_t, Import> thunks_;
    // removed ones, kept: a thread may still be running one
    std::vector<std::map<uint32_t, Import>::node_type> retired_thunks_;
    std::atomic<bool> has_thunks_{false};
    std::vector<Mmio> mmio_;
    // destroyed after the objects and threads, before the rest
    std::mutex states_mutex_;
    std::map<const void *, std::shared_ptr<void>> states_;
    std::unique_ptr<Objects> objects_; // last: it reaches the rest
};

} // namespace xf::kernel
