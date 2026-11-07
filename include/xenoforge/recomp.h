// The recompiler's view of an executable and what discovery finds in it
// (docs/recompiler.md, "Discovery"; plan step A2): the PE image's sections,
// its .pdata, its imports, then the functions (where they start and end,
// where each came from) and the jump tables behind their bctr.

#pragma once

#include "xenoforge/xex.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace xf::recomp {

struct Section {
    std::string name;
    uint32_t address = 0, size = 0;
    bool code = false, writable = false;
};

// .pdata's RUNTIME_FUNCTION, as the Xbox 360's PE writes it
struct PdataEntry {
    uint32_t begin = 0;
    uint32_t length = 0;  // bytes (instructions * 4; 0 read as one instruction)
    uint32_t prolog = 0;  // instructions
    bool exception = false;
};

struct Import {
    std::string library;
    uint16_t ordinal = 0;
    uint32_t thunk = 0; // the 16-byte thunk the code calls
};

struct Image {
    uint32_t base = 0, entry = 0;
    std::vector<uint8_t> bytes; // offset 0 is base
    std::vector<Section> sections;
    std::vector<PdataEntry> pdata;
    std::vector<Import> imports;   // functions only
    std::vector<uint32_t> exports; // a DLL's: what its export table and PE export directory name

    bool contains(uint32_t address, uint32_t size = 4) const {
        return address >= base && uint64_t(address) + size <= uint64_t(base) + bytes.size();
    }
    uint32_t word(uint32_t address) const; // big-endian; 0 outside
    const Section *section_of(uint32_t address) const;
    bool is_code(uint32_t address) const {
        const Section *s = section_of(address);
        return s && s->code;
    }
};

// From the executable loaded by load_xex_image: the PE headers inside the
// image give the sections and the exception directory (.pdata).
bool read_image(const Xex &xex, Image &out, std::string &error);

enum class Source : uint8_t {
    Import,  // an import's thunk
    Helper,  // a register save or restore helper
    Pdata,   // .pdata
    Entry,   // the entry point
    Export,  // a DLL's export
    Pointer, // a code address held as data (a word in a data section, a lis/addi pair)
    Branch,  // a branch target no function held
    Call,    // a bl target
    Gap,     // code nothing else covered
    Logged,  // an address the runtime entered the interpreter at as a function (DiscoverOptions::entries)
};
const char *source_name(Source s);

struct JumpTable {
    enum Kind : uint8_t { Absolute, ByteOffset, ShiftedByteOffset, HalfwordOffset } kind = Absolute;
    uint32_t bctr = 0;         // the bctr it feeds
    uint32_t table = 0;        // the entries' address
    uint32_t base = 0;         // added to offsets
    uint32_t shift = 0;        // ShiftedByteOffset's
    uint32_t index = 0;        // the register indexing it
    std::vector<uint32_t> targets;
};

struct Function {
    uint32_t address = 0, size = 0; // [address, address + size)
    Source source = Source::Call;
    std::string name;               // helpers' and imports' names
    std::set<uint32_t> blocks;      // where its blocks start
    std::set<uint32_t> calls;       // bl targets
    std::set<uint32_t> tail_calls;  // b and bc to other functions
    std::vector<std::pair<uint32_t, uint32_t>> call_sites; // (bl or bcl, its target)
    // It moves r1 to another stack: loads it from memory r1 does not
    // address (longjmp, a catch's continuation), or has the kernel set it (a
    // branch to KeSetCurrentStackPointers' thunk: a fiber switch). Its return
    // may go to another frame than its caller's: its generated code checks
    // each return against its entry's LR and r1 and throws the others to
    // the frame they go to (docs/recompiler.md, "Stack switches").
    bool switches_stack = false;
    // It keeps r1 in memory r1 does not address and returns normally (setjmp),
    // itself or through a tail branch: a stack switch may come back to the
    // return address of a call to it.
    bool saves_stack = false;
    // Where a stack switch elsewhere may return into this function's frame:
    // after each call to a function that saves the stack (a longjmp comes
    // back after its setjmp), and the catch continuations its exception
    // handlers return (the addresses a lis/addi pair makes inside a function
    // with exception data). Its generated code takes those returns there.
    std::set<uint32_t> resumes;
};

// A direct branch or a jump table's case that leaves its function for an
// address no function starts at: the generated code dispatches it, and the
// interpreter runs it.
struct Unresolved {
    uint32_t function = 0, site = 0, target = 0;
};

struct Discovery {
    std::map<uint32_t, Function> functions;
    std::map<uint32_t, JumpTable> tables;  // by bctr address
    std::set<uint32_t> indirect;           // bctr without a table, blrl, branches nowhere known
    std::vector<Unresolved> unresolved;    // by function, then site
    std::set<uint32_t> stack_thunks;       // KeSetCurrentStackPointers' import thunks
    std::map<std::string, uint64_t> counts; // what was found, by kind
};

struct DiscoverOptions {
    // look for starts among the code addresses the image holds as data,
    // keeping those that do not split a function (recomp_discover.cpp)
    bool scan_pointers = true;
    // Starts known from outside the image: the addresses the runtime entered
    // the interpreter at as functions in earlier runs (docs/plan.md, D3;
    // game_build.h's EntryLog). Each one in this image's code becomes a
    // function (Source::Logged), registered after the exports and before the
    // pointer scan's candidates; the others are ignored. One that lands
    // inside a known function splits it, which costs speed, not correctness.
    std::vector<uint32_t> entries;
};

Discovery discover(const Image &image, const DiscoverOptions &options = {});

// The code addresses the image holds as data: words in sections that are not
// code, and lis/addi pairs in code, where they look like a function's first
// instruction. Sorted, without repeats.
std::vector<uint32_t> scan_pointers(const Image &image);

// A function's C++ (ppc_generated.h's form), named prefix + its address in
// hex; what it calls is named the same way, and added to `calls` when given
// (what the file holding it declares).
std::string emit_function(const Image &image, const Discovery &d, const Function &f, const std::string &prefix,
                          std::set<uint32_t> *calls = nullptr);
std::string function_name(const std::string &prefix, uint32_t address);

// ---- Modules: a disc's executables emitted (recomp_module.cpp) ----

// An executable to emit: its path on the disc (or the host file's name) and
// the file.
struct ModuleFile {
    std::string path;
    std::vector<uint8_t> file;
};

// Every XEX2 executable on a disc but title updates and the system update's,
// or the ones `only` names; a bare executable (a host file starting with
// XEX2) alone. False, and why, when there is none.
bool find_modules(const std::filesystem::path &input, const std::vector<std::string> &only,
                  std::vector<ModuleFile> &out, std::string &error);

enum class EmitStage { Discovering, Emitting };

// A function (or a whole module, address 0) left out of the generated code,
// and why: it runs in the interpreter, as any address without generated code.
struct LeftOut {
    std::string module; // its path on the disc
    uint32_t address = 0;
    std::string reason;
};

struct EmitOptions {
    int files = 0; // about this many files per module; 0: 64 KB of the image each
    DiscoverOptions discover;
    // as each module is discovered (done of the modules) and each of its
    // files written (done of its files); false stops the emission
    std::function<bool(EmitStage stage, size_t done, size_t total, const std::string &detail)> progress;
    // Functions to leave out: a reason (not empty) leaves the function at that
    // address of that module out; for the recompile step's tests, which force
    // the failures it must survive (game_build.h, BuildOptions).
    std::function<std::string(const std::string &module, uint32_t address)> leave_out;
    // An executable that cannot be read is left out whole (a LeftOut with
    // address 0) instead of stopping the emission.
    bool keep_going = false;
};
struct EmittedModule {
    std::string path;      // on the disc: "default.xex", "waves\\L360.dll"
    std::string directory; // under the output: "default_xex", "waves_L360_dll"
    std::string ns;        // its C++ namespace
    uint64_t digest = 0;   // its image's (xf::image_digest)
    size_t functions = 0;
    uint64_t instructions = 0;
    int files = 0, written = 0;
    std::vector<std::string> sources; // what to compile, under the output: its functions_K.cpp, then table.cpp
    std::vector<LeftOut> left_out;    // its functions left out of the generated code
};

// Each module's directory under `out`, holding its functions as C++ in a
// namespace of its own (functions_K.cpp, each declaring what it calls) and
// table.cpp with its table (ppc_module.h: the path, base, size and digest of
// the image it was made from, the functions by address, the resume sites);
// and out/modules.cpp listing every module's table (xf_modules). A file is
// written only when its text changes, so a build after a discovery fix
// compiles the files that fix touched; files of an earlier emission that hold
// nothing now are left empty. False, and why, when an executable cannot be
// read (unless options.keep_going) or the progress stops it.
//
// A function the emitter cannot emit (or options.leave_out names) is left
// out: its body becomes a stub that dispatches to its own address, so its
// callers in other files still link, and its table entry holds no function
// (nullptr), so that dispatch runs it in the interpreter. The recompile step
// (game_build.cpp) relies on the files' layout to split a file clang rejects
// and leave its culprits out the same way: in functions_K.cpp each function
// starts at a line `XF_FN(xf_XXXXXXXX) {` and ends at the line `}` (a stub
// is the one line `XF_FN(xf_XXXXXXXX) { ... }`), after the declarations
// (`XF_FN(...);`); in table.cpp each function is the line
// `    {0xXXXXXXXXu, xf_XXXXXXXX},`.
bool emit_modules(const std::vector<ModuleFile> &modules, const std::filesystem::path &out,
                  const EmitOptions &options, std::vector<EmittedModule> &emitted, std::string &error);

// The stub a left-out function's body becomes (recomp_module.cpp, and the
// recompile step for the functions clang rejects): it dispatches to its own
// address, which has no generated code, so the interpreter runs it.
std::string left_out_stub(const std::string &prefix, uint32_t address, const std::string &reason);

} // namespace xf::recomp
