// The recompiler's view of an executable and what discovery finds in it
// (docs/recompiler.md, "Discovery"; plan step A2): the PE image's sections,
// its .pdata, its imports, then the functions (where they start and end,
// where each came from) and the jump tables behind their bctr.

#pragma once

#include "xenoforge/xex.h"

#include <cstdint>
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
    std::vector<Import> imports; // functions only

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
    Call,    // a bl target
    Gap,     // code nothing else covered
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
};

struct Discovery {
    std::map<uint32_t, Function> functions;
    std::map<uint32_t, JumpTable> tables;  // by bctr address
    std::set<uint32_t> indirect;           // bctr without a table, blrl, branches nowhere known
    std::map<std::string, uint64_t> counts; // what was found, by kind
};

Discovery discover(const Image &image);

// A function's C++ (ppc_generated.h's form), named prefix + its address in
// hex; what it calls is named the same way.
std::string emit_function(const Image &image, const Discovery &d, const Function &f, const std::string &prefix);
std::string function_name(const std::string &prefix, uint32_t address);

} // namespace xf::recomp
