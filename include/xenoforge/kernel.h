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
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
class Lockstep; // the lockstep check (lockstep.h, src/kernel/lockstep.cpp)
struct LockstepOptions;
struct LockstepCounts;
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

// A call count a call bumps with one relaxed atomic add (no lock, no string),
// copyable so an Import is.
struct CallCount {
    mutable std::atomic<uint64_t> n{0};
    CallCount() = default;
    CallCount(const CallCount &o) : n(o.n.load(std::memory_order_relaxed)) {}
    CallCount &operator=(const CallCount &o) {
        n.store(o.n.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

struct Import {
    std::string library, name; // name: the export's, or library_ordinal
    CallCount calls;           // the calls made (Kernel::calls sums them by export)
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
// A system module from its file (the console's own, read from the player's
// disc: xfxam's XAM), loaded as XexLoadImage loads a game's DLL: its image at
// its base, listed under path, its imports bound to the kernel's exports as
// a game's are. Its HMODULE, or 0 and why. Its entry point is the caller's to
// run, and threads starting and ending do not call it (the console's kernel
// gives its own modules no DLL notifications). Its exports are module_export's;
// the kernel's module of the same name (xam.xex) stays what games bind to.
uint32_t load_module(Kernel &k, const std::string &path, const std::vector<uint8_t> &data, std::string &error);
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
// module_at for the crash report: the name and base, false when none holds
// address or the modules' lock is taken (it never waits)
bool module_at_try(Kernel &k, uint32_t address, std::string &name, uint32_t &base);
// the base and size of the game's image (the executable or a DLL) holding
// address, for the exception dispatch's function tables (its .pdata)
bool module_range(Kernel &k, uint32_t address, uint32_t &base, uint32_t &size);
// A table of .pdata entries (8 bytes each, as the Xbox 360's PE writes them)
// for code in [begin, end) no loaded image holds (NT's RtlAddFunctionTable;
// the tests' programs). The exception dispatch (exceptions.cpp) looks
// functions up in these first, then in the loaded images' exception
// directories.
void add_function_table(Kernel &k, uint32_t begin, uint32_t end, uint32_t table, uint32_t count);
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

// How content a title creates (saves, caches) is kept under the content root: as STFS packages, files the console
// and Xenia's reader take (the default; a package is a working directory while a title has it open and is written
// as a package file when it closes it, xam_content.cpp), or as Xenia's directory packages. Existing content keeps
// the form it has. Set before the title runs.
enum class ContentFormat { Stfs, Directory };
void set_content_format(Kernel &k, ContentFormat format);

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
// The kernel thread that runs the DPCs of timers (KeSetTimerEx), made with the
// first timer that has one (sync.cpp); ended and joined as the kernel ends.
void shutdown_timer_dpcs(Kernel &k);
// The kernel threads that complete asynchronous file requests (files.cpp) and
// run XAM's tasks (xam.cpp), made with the first request or task; ended and
// joined as the kernel ends.
void shutdown_file_completion(Kernel &k);
void shutdown_xam_tasks(Kernel &k);
std::string video_report(Kernel &k);
// The GPU's command processor, for installing its Sink (the renderer)
// before the game starts; the Sink's calls then come on the "GPU commands"
// thread.
gpu::CommandProcessor &gpu_command_processor(Kernel &k);

// What the player chooses of the console's settings (config.toml's [system];
// boot's --language, --resolution, --widescreen, --refresh), written into the
// console's own settings before the title runs (apply_system_settings, misc.cpp).
// Zero or -1 leaves a setting as it is.
struct SystemSettings {
    std::filesystem::path file; // where the settings a title writes are kept between runs ("": this run only)
    uint32_t language = 0;      // XLanguage: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, ...
    int country = -1;           // the console's country code (103: United States)
    uint32_t width = 0, height = 0; // the screen size: 640x480, 1280x720, 1920x1080
    int widescreen = -1;        // 1 or 0: the 16:9 video flag
    int refresh = 0;            // 50 or 60 Hz
};
void apply_system_settings(Kernel &k, const SystemSettings &settings);

// The system's dialogs (the guide: message boxes, the keyboard, the device selector, sign-in, the friends, messages,
// gamer card and other pages; xam_ui.cpp). With no handler each answers as a headless system does: the dialog's
// default (the focused button, the default text, the hard drive, the user already signed in), at once. A host that
// draws an overlay sets a handler: it is given each request and, when it takes it (returns true), calls `done`
// with the user's answer, now or later and from any thread; until then the title's overlapped stays pending, the
// guide counts as up (XamIsUIActive) and XN_SYS_UI is "on". A cancelled dialog completes its overlapped with
// ERROR_CANCELLED, as the console's does when the user backs out. A handler that returns false leaves the request
// to the default.
enum class UiKind : uint32_t {
    MessageBox, Keyboard, DeviceSelector, SignIn, Marketplace, Achievements, GamerCard, EditProfile, Friends,
    Messages, Players, QuickChat, GameInvite, FriendRequest, PlayerReview, MessageCompose, CustomMessageCompose,
    CustomPlayerList, Other,
};
struct UiRequest {
    UiKind kind = UiKind::Other;
    uint32_t user = 0xFF;                // the user index the title asked for (0xFF: any)
    uint64_t xuid = 0;                   // the other player: gamer card, friend request, review
    std::vector<uint64_t> xuids;         // recipients, players
    std::u16string title, text;          // a message box's, a keyboard's title and description, a message's text
    std::vector<std::u16string> buttons; // a message box's, in order
    uint32_t default_button = 0;         // the focused one
    uint32_t flags = 0;                  // the message box's icon and style, the device selector's, the keyboard's
    std::u16string default_text;         // the keyboard's
    uint32_t max_length = 0;             // the keyboard's, in characters
    uint32_t content_type = 0;           // the device selector's
    uint64_t bytes_required = 0;         // the device selector's
    uint32_t users_needed = 0;           // sign-in's
};
struct UiResult {
    bool cancelled = false;
    uint32_t button = 0;     // the message box's button pressed
    std::u16string text;     // the keyboard's text
    uint32_t device_id = 1;  // the device selector's (1: the hard drive)
};
using UiDone = std::function<void(const UiResult &)>;
using UiHandler = std::function<bool(const UiRequest &, UiDone done)>;
void set_ui_handler(Kernel &k, UiHandler handler);

// What a host that draws notifications shows: an achievement the title unlocked (its name, its gamerscore, the icon
// as the SPA's PNG when it has one) and the text of XNotifyQueueUI. Called from the guest's threads with no kernel
// lock held; it must not block. With no handler they are only logged.
struct Toast {
    enum class Kind { Achievement, Notice } kind = Kind::Notice;
    std::u16string title, text; // an achievement's name and description; a notice's text (title empty)
    uint32_t gamerscore = 0;    // an achievement's
    std::vector<uint8_t> icon;  // an achievement's icon, PNG bytes (none when the SPA has no such image)
};
using ToastHandler = std::function<void(const Toast &)>;
void set_toast_handler(Kernel &k, ToastHandler handler);

// The profile a host shows (its gamer card, its achievements page): the user, and the running title's achievements
// as the user's GPD for it holds them (a locked one for each the SPA lists), with their icons as PNG bytes.
struct ProfileAchievement {
    uint32_t id = 0, gamerscore = 0, image_id = 0;
    bool unlocked = false;
    uint64_t unlock_time = 0; // FILETIME
    std::u16string label, description, unachieved;
    std::vector<uint8_t> icon;
};
struct ProfileSummary {
    std::string gamertag;
    uint64_t xuid = 0;
    bool signed_in = false;
    uint32_t title_id = 0;
    std::vector<ProfileAchievement> achievements;
    uint32_t gamerscore = 0;       // the earned ones' sum, in this title
    uint32_t gamerscore_total = 0; // all the title's
};
ProfileSummary profile_summary(Kernel &k);
// The user's gamertag (config.toml's), before the title runs
void set_gamertag(Kernel &k, const std::string &name);
// The user a profile is: its XUID (the directory its saves and GPDs are under) and gamertag, set before the title runs;
// a zero XUID leaves the one the storage root has (the first profile found, else the default).
void set_user(Kernel &k, uint64_t xuid, const std::string &name);

// The launch data the next title finds through XamLoaderGetLaunchData (what
// the title before it set with XamLoaderSetLaunchData; Kernel::title_exit),
// set before the title runs.
void set_launch_data(Kernel &k, const std::vector<uint8_t> &data);

// The sound family's parts (audio.cpp, xma.cpp; its seams in
// xenoforge/audio.h). setup_audio, as the kernel starts: the XMA decoder's
// registers as MMIO and its contexts in physical memory (the threads start
// with the first call into the audio driver). shutdown_audio, as the kernel
// ends: those threads stopped and joined. audio_report: what it did (render
// driver frames, XMA contexts and kicks), for boot.
void setup_audio(Kernel &k);
void shutdown_audio(Kernel &k);
std::string audio_report(Kernel &k);

// The deterministic mode (plan A5; docs/runtime.md, "The deterministic mode"):
// every guest thread a fiber on one host thread, handed over in a fixed order,
// on a virtual clock that moves with the guest's kernel calls and time base
// reads and jumps to the next deadline when every thread waits. The same disc
// and the same input give the same run. Costs and the slice are in the clock's
// 100 ns.
struct Deterministic {
    uint64_t system_time = 133'801'632'000'000'000ull; // the guest's system time at the start: 2025-01-01 00:00 UTC
    uint64_t call_cost = 10;      // a kernel call: 1 us
    uint64_t time_base_cost = 1;  // a time base read (mftb): 100 ns
    uint64_t dispatch_cost = 1;   // an indirect call (Kernel::dispatch): 100 ns
    // The recompiler's yield points (a loop's back edge, a priority hint;
    // ppc_ops.h) are counted in units of 1/yield_units of 100 ns: a back edge
    // one (about 3 ns), a priority hint eight. 0: not counted.
    uint32_t yield_units = 32;
    uint64_t slice = 10'000;      // a thread runs 1 ms of the clock before the next ready one does
    // Guest code that runs this long on the host without handing over (a spin
    // with no kernel call, time base read or indirect call) is handed over
    // where it is: the run goes on, no longer exact (0: never; Windows only).
    uint32_t force_after_ms = 100;
    // Paced: the clock never runs ahead of the host's (the player's, so the
    // game plays at its speed); else it runs as fast as the host can (boot).
    bool paced = false;
    // Asked as each thread is handed over (on the scheduler's thread, the
    // kernel's lock held): true stops the run there for good, every guest
    // thread parked where it handed over (boot's --until-swap, --until).
    std::function<bool()> halt;
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
    // the host id of the first running guest thread whose name (the
    // kernel's own, or the guest's SetThreadName) contains `name`; 0 none
    // (or the deterministic mode, whose threads are fibers)
    uint32_t host_thread_id(const std::string &name);
    // The deterministic mode (Deterministic), chosen before launch: false
    // when a guest thread has started already. halted: its halt said stop,
    // and every guest thread is parked.
    bool make_deterministic(const Deterministic &options);
    bool deterministic() const;
    bool halted() const;
    // a hash (FNV-1a, 64-bit) of the guest memory committed and readable,
    // region by region in address order: two runs that left the same memory
    // give the same one
    uint64_t memory_hash();
    // the host addresses the generated code spans (its lowest and highest
    // function), where the deterministic mode may stop a thread
    std::pair<uintptr_t, uintptr_t> generated_code_span();
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
    // generated, an import or the port's hooks there (hooks false: those
    // left out, for a hooked function's original body): ran it
    bool try_call(ppc::Context &c, uint32_t target, bool hooks = true);
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
    // Called once per address the first time the interpreter is entered at it
    // as a function: a dispatch or a thread's start with no generated code,
    // and a call the interpreted code makes to an address with none (not the
    // returns an Unwind goes on from). With generated code loaded, what
    // discovery missed: the generic runtime logs them beside the cached game
    // library for the next build (docs/plan.md, D3; game_build.h's EntryLog).
    // Set before the guest runs; the hook is called from any guest thread.
    using EntryHook = void (*)(void *user, uint32_t address);
    void set_entry_hook(EntryHook hook, void *user) { entry_hook_ = hook, entry_hook_user_ = user; }

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

    // Every import call as it is made (returned false) and as its export
    // returns (true), on the calling thread: a tool's trace of the kernel
    // calls code makes (xfxam sets the calls of the console's XAM beside
    // ours). Set before guest threads run; none by default.
    using ImportHook = void (*)(void *user, const Import &import, ppc::Context &c, bool returned);
    void set_import_hook(ImportHook hook, void *user) { import_hook_ = hook, import_hook_user_ = user; }

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
    // Stops the run (the first reason stays), with the crash report (crash.h)
    // of the calling guest thread, as it was when it stopped it.
    void stop(const std::string &reason);
    std::string stop_report, stop_stack; // the report's text, and its one-line stack

    // What the title asked when it ended the run by launching another title
    // (XamLoaderLaunchTitle and its variants) or by terminating itself
    // (XamLoaderTerminateTitle, ExTerminateTitleProcess): the host starts the
    // next title as a process of its own (boot's --follow-launches). Set under
    // the stop's first reason, before the run stops; read after it.
    struct TitleExit {
        bool launch = false;             // another title follows (an empty path: the dashboard)
        std::string path;                // the executable as the guest named it ("game:\Game.xex")
        uint32_t flags = 0;              // the launch flags
        uint32_t exit_code = 0;          // ExTerminateTitleProcess's code
        std::vector<uint8_t> launch_data; // XamLoaderSetLaunchData's, for the next title
    };
    TitleExit title_exit;

    // Where a guest address is in the generated code's tables, for the
    // reports: its module's file name and the function it falls in (the last
    // one starting at or before it, within 64 KB). Locks nothing (the crash
    // path uses them).
    bool guest_function(uint32_t address, const char *&module, uint32_t &start) const;
    // The generated function whose host code is nearest below a host code
    // address (within 64 KB): the guest function a fault there came from.
    bool guest_function_of_host(const void *host, uint32_t &start) const;

    // The lockstep check (lockstep.h): the interpreter checks the generated
    // code of the calls chosen, mismatches printed as they are found. Set
    // before guest threads run; off by default.
    bool enable_lockstep(const LockstepOptions &options, std::string &error);
    bool lockstep_enabled() const { return lockstep_ != nullptr; }
    LockstepCounts lockstep_counts() const;
    std::string lockstep_report() const; // the counts, a few lines

    // Every guest thread parked at a safe point (docs/runtime.md, "Pausing
    // the guest"): at a kernel call (as it enters or leaves one; a thread
    // waiting inside one counts as parked, and stays there until resume),
    // or at a yield point of the generated code (the deterministic mode's,
    // and in the other mode the next back edge, the yield points being
    // turned on for the pause). True once none runs guest code; false after
    // timeout_ms (the threads still running are left to park as they come).
    // Either way the pause holds until resume(). What the port's hooks are
    // reloaded under (port_module.h). Not from a guest thread.
    bool pause(uint32_t timeout_ms);
    void resume();
    bool pause_requested() const { return pause_requested_.load(std::memory_order_relaxed); }
    // the guest threads that ran guest code when pause() returned
    uint32_t pause_unparked() const { return pause_unparked_; }

private:
    friend class Lockstep;
    // (try_call's) generated code run, or checked by the lockstep check
    void run_generated(ppc::Context &c, uint32_t target, void (*fn)(ppc::Context &));
    // The port's hooks (xenoforge/hooks.h): a hooked function reached through
    // dispatch whose generated code has no hook slot (or none at all) run
    // with its hooks; its original body (System::run_original); and the
    // marker the lockstep check names a hook by, as an import.
    bool call_hooked(ppc::Context &c, uint32_t target);
    void run_original(ppc::Context &c, uint32_t address);
    const Import *hook_marker(uint32_t address);
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
    // what a kernel call and a time base read cost the deterministic mode's
    // clock (0 when playing)
    uint64_t deterministic_call_cost_ = 0, deterministic_time_base_cost_ = 0, deterministic_dispatch_cost_ = 0;
    // the yield points' units in 100 ns (0: not counted; attach)
    uint32_t deterministic_yield_units_ = 0;
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
    std::set<std::string> unimplemented_logged_; // under counts_mutex_
    void each_import(const std::function<void(const Import &)> &visit) const;
    std::atomic<uint64_t> interpreted_{0};
    std::unordered_map<uint32_t, uint64_t> interpreted_targets_; // under counts_mutex_
    EntryHook entry_hook_ = nullptr;
    void *entry_hook_user_ = nullptr;
    std::unordered_map<uint32_t, bool> entries_seen_; // under counts_mutex_
    void entered(uint32_t address); // the entry hook, once per address
    // add_thunk's (a map: its entries stay where they are)
    mutable std::shared_mutex thunks_mutex_;
    std::map<uint32_t, Import> thunks_;
    // removed ones, kept: a thread may still be running one
    std::vector<std::map<uint32_t, Import>::node_type> retired_thunks_;
    std::atomic<bool> has_thunks_{false};
    std::vector<Mmio> mmio_;
    ImportHook import_hook_ = nullptr;
    void *import_hook_user_ = nullptr;
    // destroyed after the objects and threads, before the rest
    std::mutex states_mutex_;
    std::map<const void *, std::shared_ptr<void>> states_;
    std::unique_ptr<Lockstep> lockstep_;
    std::mutex hook_markers_mutex_;
    std::map<std::string, Import> hook_markers_; // never dropped: the lockstep check keeps pointers
    // pause(): asked, and where threads wait for resume()
    std::atomic<bool> pause_requested_{false};
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    uint32_t pause_unparked_ = 0;
    // a guest thread at a safe point: parked until resume() (pause_requested_ set)
    void park();
    friend struct GuestCodeScope;
    std::unique_ptr<Objects> objects_; // last: it reaches the rest
};

} // namespace xf::kernel
