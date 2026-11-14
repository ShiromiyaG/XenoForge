// xfstencil: the copy-and-patch tier's stencils made at XenoForge's build time
// (docs/recompiler.md, "A copy-and-patch first tier"). Compiles
// src/stencil/stencils.cpp with clang and the game library's flags
// (game_build_flags.h), reads the object file it made (COFF or ELF), and
// writes each stencil's machine code and holes, and the helpers and constants
// they refer to, as a C++ table (stencil_format.h) that src/stencil_jit.cpp
// includes:
//
//   xfstencil <clang++> <stencils.cpp> <include dir> <out dir> [--target <triple>]
//   xfstencil --extract <object> <out.inc>
//
// The second form reads an object made elsewhere (a cross-compiled one, to
// check the ELF reader on Windows). What the formats call their relocations
// becomes stencil_format.h's kinds here, so the patcher knows no format.

#include "../src/game_build_flags.h"
#include "../src/stencil_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xf::stencil;

namespace {

[[noreturn]] void fail(const std::string &why) {
    std::fprintf(stderr, "xfstencil: %s\n", why.c_str());
    std::exit(1);
}

// ---- an object file, whatever its format ----

struct Rel {
    uint32_t offset;
    RelocKind kind;
    uint32_t symbol;
    int64_t addend; // what is added to the symbol's address (P already taken out for PcRel32's implicit part)
};
struct Section {
    std::string name;
    std::vector<uint8_t> bytes;
    uint32_t align = 1;
    bool code = false, writable = false, bss = false;
    std::vector<Rel> rels;
    // its function's unwind information (stencil_format.h's Piece): Windows
    // x64's UNWIND_INFO (from .pdata and .xdata), or ELF's CIE and FDE rules
    std::vector<uint8_t> unwind;
};
struct Symbol {
    std::string name;
    int section = -1; // -1: undefined
    uint64_t value = 0;
};
struct Object {
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    bool arm64 = false;
};

template <class T> T rd(const std::vector<uint8_t> &b, size_t at) {
    if (at + sizeof(T) > b.size()) fail("truncated object");
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

// COFF (Windows): the regular header (clang switches to /bigobj's past 65,279
// sections; the stencils make about 1,400)
void read_coff(const std::vector<uint8_t> &f, Object &o) {
    const uint16_t machine = rd<uint16_t>(f, 0);
    const bool x64 = machine == 0x8664, arm64 = machine == 0xAA64;
    if (!x64 && !arm64) fail("COFF machine not x86-64 or ARM64");
    o.arm64 = arm64;
    const uint16_t nsections = rd<uint16_t>(f, 2);
    const uint32_t symtab = rd<uint32_t>(f, 8), nsymbols = rd<uint32_t>(f, 12);
    const uint16_t optional = rd<uint16_t>(f, 16);
    const size_t strtab = symtab + size_t(nsymbols) * 18;
    auto string_at = [&](uint32_t offset) {
        std::string s;
        for (size_t k = strtab + offset; k < f.size() && f[k]; k++) s += char(f[k]);
        return s;
    };
    o.sections.resize(nsections);
    struct Raw {
        uint32_t relocs;
        uint16_t nrelocs;
    };
    std::vector<Raw> raw(nsections);
    for (uint16_t s = 0; s < nsections; s++) {
        const size_t h = 20 + optional + size_t(s) * 40;
        Section &sec = o.sections[s];
        char name[9] = {};
        std::memcpy(name, f.data() + h, 8);
        sec.name = name[0] == '/' ? string_at(uint32_t(std::strtoul(name + 1, nullptr, 10))) : name;
        const uint32_t size = rd<uint32_t>(f, h + 16), data = rd<uint32_t>(f, h + 20);
        const uint32_t ch = rd<uint32_t>(f, h + 36);
        sec.code = ch & 0x20;
        sec.bss = ch & 0x80;
        sec.writable = ch & 0x80000000u;
        const uint32_t align = (ch >> 20) & 0xF;
        sec.align = align ? 1u << (align - 1) : 1;
        if (sec.bss)
            sec.bytes.assign(size, 0);
        else if (size)
            sec.bytes.assign(f.begin() + data, f.begin() + data + size);
        raw[s] = {rd<uint32_t>(f, h + 24), rd<uint16_t>(f, h + 32)};
    }
    o.symbols.resize(nsymbols);
    for (uint32_t k = 0; k < nsymbols;) {
        const size_t at = symtab + size_t(k) * 18;
        Symbol &sym = o.symbols[k];
        sym.name = rd<uint32_t>(f, at) == 0 ? string_at(rd<uint32_t>(f, at + 4))
                                            : std::string(reinterpret_cast<const char *>(f.data() + at),
                                                          strnlen(reinterpret_cast<const char *>(f.data() + at), 8));
        sym.value = rd<uint32_t>(f, at + 8);
        const int16_t section = rd<int16_t>(f, at + 12);
        sym.section = section > 0 ? section - 1 : -1;
        k += 1 + f[at + 17];
    }
    for (uint16_t s = 0; s < nsections; s++) {
        Section &sec = o.sections[s];
        for (uint16_t r = 0; r < raw[s].nrelocs; r++) {
            const size_t at = raw[s].relocs + size_t(r) * 10;
            const uint32_t offset = rd<uint32_t>(f, at), symbol = rd<uint32_t>(f, at + 4);
            const uint16_t type = rd<uint16_t>(f, at + 8);
            Rel rel{offset, RelocKind::PcRel32, symbol, 0};
            if (x64) {
                if (type >= 4 && type <= 9) { // REL32, REL32_1..5: from the end of the field and k bytes more
                    rel.addend = int64_t(rd<int32_t>(sec.bytes, offset)) - 4 - (type - 4);
                } else if (type == 1) { // ADDR64
                    rel.kind = RelocKind::Abs64, rel.addend = rd<int64_t>(sec.bytes, offset);
                } else if (type == 3) { // ADDR32NB: .pdata's, read below
                    continue;
                } else {
                    fail("COFF x86-64 relocation type " + std::to_string(type) + " in " + sec.name);
                }
            } else {
                if (type == 3)
                    rel.kind = RelocKind::Branch26;
                else if (type == 4)
                    rel.kind = RelocKind::AdrPage21;
                else if (type == 6)
                    rel.kind = RelocKind::AddLo12;
                else if (type == 0xE)
                    rel.kind = RelocKind::Abs64, rel.addend = rd<int64_t>(sec.bytes, offset);
                else if (type == 2)
                    continue; // ADDR32NB: .pdata's
                else
                    fail("COFF ARM64 relocation type " + std::to_string(type) + " in " + sec.name);
            }
            sec.rels.push_back(rel);
        }
    }
    if (!x64) return; // ARM64's .pdata is another format (packed entries): not read yet
    // .pdata: RUNTIME_FUNCTIONs (begin, end, unwind info), their three fields
    // ADDR32NB relocations to the function's section and to .xdata
    for (uint16_t s = 0; s < nsections; s++) {
        if (o.sections[s].name != ".pdata") continue;
        std::map<uint32_t, std::pair<uint32_t, int32_t>> fields; // offset: symbol, addend
        for (uint16_t r = 0; r < raw[s].nrelocs; r++) {
            const size_t at = raw[s].relocs + size_t(r) * 10;
            const uint32_t offset = rd<uint32_t>(f, at);
            fields[offset] = {rd<uint32_t>(f, at + 4), rd<int32_t>(o.sections[s].bytes, offset)};
        }
        for (uint32_t e = 0; e + 12 <= o.sections[s].bytes.size(); e += 12) {
            auto begin = fields.find(e), info = fields.find(e + 8);
            if (begin == fields.end() || info == fields.end()) fail(".pdata entry without its relocations");
            const Symbol &fn = o.symbols[begin->second.first], &xd = o.symbols[info->second.first];
            if (fn.section < 0 || xd.section < 0) fail(".pdata entry for an undefined symbol");
            if (fn.value + begin->second.second != 0) fail("a function with more than one .pdata entry: " + fn.name);
            const std::vector<uint8_t> &x = o.sections[size_t(xd.section)].bytes;
            const size_t u = size_t(xd.value + info->second.second);
            const uint8_t flags = x.at(u) >> 3, codes = x.at(u + 2);
            if (flags) fail("unwind information with a handler or chained: " + fn.name);
            const size_t size = 4 + 2 * size_t((codes + 1) & ~1);
            o.sections[size_t(fn.section)].unwind.assign(x.begin() + u, x.begin() + u + size);
        }
    }
}

uint64_t uleb(const std::vector<uint8_t> &b, size_t &at) {
    uint64_t v = 0;
    for (int shift = 0;; shift += 7) {
        const uint8_t byte = b.at(at++);
        v |= uint64_t(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return v;
    }
}
void skip_sleb(const std::vector<uint8_t> &b, size_t &at) {
    while (b.at(at++) & 0x80) {
    }
}

// ELF's unwind information: each stencil's and helper's FDE in .eh_frame, with
// its CIE, kept as stencil_format.h's Piece describes, for the run time to
// write an FDE for every copy. The CIEs clang writes are "zR" (no personality,
// no LSDA: nothing in the stencils catches) with pc-relative 4-byte addresses,
// which the run time cannot use where its table lies more than 2 GB from the
// code, so the encoding becomes 8-byte absolute addresses.
void read_eh_frame(Object &o, size_t index, const std::map<uint64_t, std::pair<uint32_t, int64_t>> &relocs) {
    const std::vector<uint8_t> &b = o.sections[index].bytes;
    std::map<size_t, std::vector<uint8_t>> cies; // by offset: the body, rewritten
    for (size_t at = 0; at + 4 <= b.size();) {
        const uint32_t length = rd<uint32_t>(b, at);
        if (length == 0) break;
        if (length == 0xFFFFFFFFu) fail(".eh_frame: a 64-bit entry");
        const size_t end = at + 4 + length;
        if (end > b.size()) fail(".eh_frame: an entry past its end");
        const uint32_t id = rd<uint32_t>(b, at + 4);
        if (id == 0) {
            std::vector<uint8_t> body(b.begin() + at + 8, b.begin() + end);
            size_t p = 1; // past the version
            std::string augmentation;
            while (body.at(p)) augmentation += char(body[p++]);
            p++;
            if (augmentation != "zR") fail(".eh_frame: a CIE with augmentation \"" + augmentation + "\"");
            uleb(body, p);      // code alignment
            skip_sleb(body, p); // data alignment
            if (body[0] == 1)
                p++; // the return address register: a byte in version 1
            else
                uleb(body, p);
            if (uleb(body, p) != 1) fail(".eh_frame: a CIE's augmentation data is not one byte");
            if (body.at(p) != 0x1B) fail(".eh_frame: a CIE's addresses are not pc-relative 4-byte ones");
            body[p] = 0x00; // DW_EH_PE_absptr
            cies[at] = std::move(body);
        } else {
            auto cie = cies.find(at + 4 - id);
            if (cie == cies.end()) fail(".eh_frame: an FDE before its CIE");
            auto r = relocs.find(at + 8);
            if (r == relocs.end()) fail(".eh_frame: an FDE without its start's relocation");
            const Symbol &sym = o.symbols.at(r->second.first);
            if (sym.section < 0) fail(".eh_frame: an FDE for an undefined symbol");
            if (int64_t(sym.value) + r->second.second != 0) fail(".eh_frame: an FDE not at its section's start");
            // after the start and the range (4 bytes each): the augmentation
            // data's length (none for "zR") and the instructions
            std::vector<uint8_t> tail(b.begin() + at + 16, b.begin() + end);
            size_t q = 0;
            if (uleb(tail, q) != 0) fail(".eh_frame: an FDE with augmentation data");
            bool rules = false;
            for (size_t k = q; k < tail.size(); k++) rules |= tail[k] != 0; // 0: DW_CFA_nop
            Section &fn = o.sections[size_t(sym.section)];
            if (!fn.unwind.empty()) fail(".eh_frame: two FDEs for " + fn.name);
            // a leaf, which never holds a return address, has nothing past
            // the CIE's rules: no FDE for its copies (as Windows has no .pdata entry)
            if (rules) {
                const std::vector<uint8_t> &body = cie->second;
                fn.unwind = {uint8_t(body.size()), uint8_t(body.size() >> 8)};
                fn.unwind.insert(fn.unwind.end(), body.begin(), body.end());
                fn.unwind.insert(fn.unwind.end(), tail.begin(), tail.end());
            }
        }
        at = end;
    }
    // ARM64: a function that calls (bl, blr) keeps LR in its frame, so it has
    // rules; one without would unwind to garbage
    if (o.arm64)
        for (Section &s : o.sections) {
            if (!s.code || !s.unwind.empty()) continue;
            for (size_t k = 0; k + 4 <= s.bytes.size(); k += 4) {
                const uint32_t w = rd<uint32_t>(s.bytes, k);
                if ((w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u)
                    fail(s.name + " calls but has no unwind rules");
            }
        }
}

// ELF64 (Linux, Android): relocations with explicit addends
void read_elf(const std::vector<uint8_t> &f, Object &o) {
    if (f.size() < 64 || f[4] != 2 || f[5] != 1) fail("ELF: not 64-bit little-endian");
    const uint16_t machine = rd<uint16_t>(f, 18);
    const bool x64 = machine == 62, arm64 = machine == 183;
    if (!x64 && !arm64) fail("ELF machine not x86-64 or AArch64");
    o.arm64 = arm64;
    const uint64_t shoff = rd<uint64_t>(f, 40);
    const uint16_t shentsize = rd<uint16_t>(f, 58), shnum = rd<uint16_t>(f, 60), shstrndx = rd<uint16_t>(f, 62);
    struct Sh {
        uint32_t name, type;
        uint64_t flags, offset, size;
        uint32_t link, info;
        uint64_t align;
    };
    std::vector<Sh> sh(shnum);
    for (uint16_t s = 0; s < shnum; s++) {
        const size_t h = shoff + size_t(s) * shentsize;
        sh[s] = {rd<uint32_t>(f, h),      rd<uint32_t>(f, h + 4),  rd<uint64_t>(f, h + 8),  rd<uint64_t>(f, h + 24),
                 rd<uint64_t>(f, h + 32), rd<uint32_t>(f, h + 40), rd<uint32_t>(f, h + 44), rd<uint64_t>(f, h + 48)};
    }
    auto str = [&](uint16_t table, uint32_t at) {
        std::string s;
        for (size_t k = sh[table].offset + at; k < f.size() && f[k]; k++) s += char(f[k]);
        return s;
    };
    o.sections.resize(shnum);
    int symtab = -1;
    for (uint16_t s = 0; s < shnum; s++) {
        Section &sec = o.sections[s];
        sec.name = str(shstrndx, sh[s].name);
        sec.code = sh[s].flags & 4;
        sec.writable = sh[s].flags & 1;
        sec.bss = sh[s].type == 8;
        sec.align = uint32_t(sh[s].align ? sh[s].align : 1);
        if (!(sh[s].flags & 2)) continue; // not loaded: nothing to copy
        if (sec.bss)
            sec.bytes.assign(sh[s].size, 0);
        else
            sec.bytes.assign(f.begin() + sh[s].offset, f.begin() + sh[s].offset + sh[s].size);
        (void)symtab;
    }
    for (uint16_t s = 0; s < shnum; s++)
        if (sh[s].type == 2) symtab = s;
    if (symtab < 0) fail("ELF without a symbol table");
    const uint16_t strtab = uint16_t(sh[symtab].link);
    for (uint64_t at = sh[symtab].offset; at + 24 <= sh[symtab].offset + sh[symtab].size; at += 24) {
        Symbol sym;
        sym.name = str(strtab, rd<uint32_t>(f, at));
        const uint16_t shndx = rd<uint16_t>(f, at + 6);
        sym.section = shndx == 0 || shndx >= 0xFF00 ? -1 : shndx;
        sym.value = rd<uint64_t>(f, at + 8);
        if (sym.name.empty() && (f[at + 4] & 0xF) == 3) sym.name = o.sections[shndx].name; // a section's symbol
        o.symbols.push_back(sym);
    }
    // .eh_frame's relocations: the FDEs' starts (offset: symbol, addend)
    std::map<uint64_t, std::pair<uint32_t, int64_t>> eh_relocs;
    int eh_frame = -1;
    for (uint16_t s = 0; s < shnum; s++) {
        if (sh[s].type != 4) continue; // SHT_RELA
        Section &sec = o.sections[sh[s].info];
        if (sec.name == ".eh_frame") {
            eh_frame = int(sh[s].info);
            for (uint64_t at = sh[s].offset; at + 24 <= sh[s].offset + sh[s].size; at += 24) {
                const uint32_t type = uint32_t(rd<uint64_t>(f, at + 8));
                // x86-64 PC32, AArch64 PREL32: the FDE's start, pc-relative (encoding 0x1B)
                if ((x64 && type != 2) || (arm64 && type != 261))
                    fail("ELF .eh_frame relocation type " + std::to_string(type));
                eh_relocs[rd<uint64_t>(f, at)] = {uint32_t(rd<uint64_t>(f, at + 8) >> 32), rd<int64_t>(f, at + 16)};
            }
            continue;
        }
        if (!(sh[sh[s].info].flags & 2)) continue;
        for (uint64_t at = sh[s].offset; at + 24 <= sh[s].offset + sh[s].size; at += 24) {
            const uint64_t info = rd<uint64_t>(f, at + 8);
            const uint32_t type = uint32_t(info);
            Rel rel{uint32_t(rd<uint64_t>(f, at)), RelocKind::PcRel32, uint32_t(info >> 32), rd<int64_t>(f, at + 16)};
            if (x64) {
                if (type == 2 || type == 4)
                    rel.kind = RelocKind::PcRel32; // PC32, PLT32
                else if (type == 1)
                    rel.kind = RelocKind::Abs64;
                else
                    fail("ELF x86-64 relocation type " + std::to_string(type) + " in " + sec.name +
                         " (a symbol reached through the GOT: not hidden?)");
            } else {
                if (type == 282 || type == 283)
                    rel.kind = RelocKind::Branch26;
                else if (type == 275)
                    rel.kind = RelocKind::AdrPage21;
                else if (type == 277)
                    rel.kind = RelocKind::AddLo12;
                else if (type == 278)
                    rel.kind = RelocKind::LdSt8Lo12;
                else if (type == 284)
                    rel.kind = RelocKind::LdSt16Lo12;
                else if (type == 285)
                    rel.kind = RelocKind::LdSt32Lo12;
                else if (type == 286)
                    rel.kind = RelocKind::LdSt64Lo12;
                else if (type == 299)
                    rel.kind = RelocKind::LdSt128Lo12;
                else if (type == 257)
                    rel.kind = RelocKind::Abs64;
                else if (type == 264)
                    rel.kind = RelocKind::MovwG0;
                else if (type == 266)
                    rel.kind = RelocKind::MovwG1;
                else if (type == 268)
                    rel.kind = RelocKind::MovwG2;
                else if (type == 269)
                    rel.kind = RelocKind::MovwG3;
                else
                    fail("ELF AArch64 relocation type " + std::to_string(type) + " in " + sec.name);
            }
            sec.rels.push_back(rel);
        }
    }
    if (eh_frame >= 0) read_eh_frame(o, size_t(eh_frame), eh_relocs);
}

// ---- the table ----

int hole_index(const std::string &name) {
    static const char *const names[] = {"xf_hole_word", "xf_hole_a",      "xf_hole_b",    "xf_hole_data",
                                        "xf_hole_next", "xf_hole_target", "xf_hole_call", "xf_hole_f0",
                                        "xf_hole_f1",   "xf_hole_f2",     "xf_hole_f3",   "xf_hole_f4"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(Hole::Count));
    for (int k = 0; k < int(Hole::Count); k++)
        if (name == names[k]) return k;
    return -1;
}
int extern_index(const std::string &name) {
    // xf::ppc::runtime, as MSVC's and the Itanium ABI mangle it
    if (name == "?runtime@ppc@xf@@3URuntime@12@A" || name == "_ZN2xf3ppc7runtimeE") return int(Extern::Runtime);
    if (name == "memcpy") return int(Extern::Memcpy);
    if (name == "memmove") return int(Extern::Memmove);
    if (name == "memset") return int(Extern::Memset);
    return -1;
}

std::string write_table(const Object &o, const std::string &source) {
    // the stencils: a section holding a function xf_st_* at its start
    std::map<int, std::string> stencils;
    for (const Symbol &s : o.symbols)
        if (s.section >= 0 && s.value == 0 && s.name.starts_with("xf_st_") && o.sections[size_t(s.section)].code)
            stencils[s.section] = s.name;
    if (stencils.empty()) fail("no stencils in the object");
    // and what they reach
    std::vector<int> order;
    std::map<int, uint32_t> piece_of;
    for (auto &[s, name] : stencils) piece_of[s] = uint32_t(order.size()), order.push_back(s);
    for (size_t k = 0; k < order.size(); k++)
        for (const Rel &r : o.sections[size_t(order[k])].rels) {
            const Symbol &sym = o.symbols.at(r.symbol);
            if (sym.section < 0 || piece_of.count(sym.section)) continue;
            const Section &target = o.sections[size_t(sym.section)];
            if (target.writable) fail("a stencil refers to writable data: " + target.name + " (" + sym.name + ")");
            piece_of[sym.section] = uint32_t(order.size()), order.push_back(sym.section);
        }

    std::string bytes, relocs, pieces, unwind;
    size_t nbytes = 0, nrelocs = 0, nunwind = 0;
    auto hex = [](unsigned v) {
        char b[8];
        std::snprintf(b, sizeof(b), "0x%02X,", v);
        return std::string(b);
    };
    static const char *const kinds[] = {"PcRel32",   "Abs64",      "Branch26",   "AdrPage21",  "AddLo12",
                                        "LdSt8Lo12", "LdSt16Lo12", "LdSt32Lo12", "LdSt64Lo12", "LdSt128Lo12",
                                        "MovwG0",    "MovwG1",     "MovwG2",     "MovwG3"};
    size_t stencil_bytes = 0;
    for (size_t k = 0; k < order.size(); k++) {
        const Section &sec = o.sections[size_t(order[k])];
        const bool stencil = stencils.count(order[k]) != 0;
        const size_t first = nrelocs;
        bool tail = false;
        for (const Rel &r : sec.rels) {
            const Symbol &sym = o.symbols.at(r.symbol);
            std::string target;
            int64_t addend = r.addend;
            if (sym.section >= 0) {
                // a stencil is copied per use: only it may refer to itself, and
                // nothing to another stencil
                if (stencils.count(sym.section) && sym.section != order[k])
                    fail(sec.name + " refers to the stencil " + stencils.at(sym.section));
                if (sym.section == order[k]) fail("a stencil refers to itself through a relocation: " + sec.name);
                target = "TargetKind::Piece, " + std::to_string(piece_of.at(sym.section));
                addend += int64_t(sym.value);
            } else if (int h = hole_index(sym.name); h >= 0) {
                if (!stencil) fail("a hole outside a stencil: " + sec.name);
                target = "TargetKind::Hole, " + std::to_string(h);
                // the last five bytes a jmp rel32 to the next stencil (x86-64),
                // or the last four a b (ARM64)
                if (h == int(Hole::Next) && !o.arm64 && r.kind == RelocKind::PcRel32 &&
                    r.offset + 4 == sec.bytes.size() && r.offset >= 1 && sec.bytes[r.offset - 1] == 0xE9 &&
                    r.addend == -4)
                    tail = true;
                if (h == int(Hole::Next) && o.arm64 && r.kind == RelocKind::Branch26 &&
                    r.offset + 4 == sec.bytes.size() && r.addend == 0 &&
                    (rd<uint32_t>(sec.bytes, r.offset) & 0xFC000000u) == 0x14000000u)
                    tail = true;
            } else if (int e = extern_index(sym.name); e >= 0) {
                target = "TargetKind::Extern, " + std::to_string(e);
            } else {
                fail("a stencil refers to " + sym.name + ", which the tier does not provide");
            }
            relocs += "    {" + std::to_string(r.offset) + ", RelocKind::" + kinds[int(r.kind)] + ", " + target + ", " +
                      std::to_string(addend) + "},\n";
            nrelocs++;
        }
        // (a helper by the symbol at its start, which says more than its section's name)
        std::string name = stencil ? stencils.at(order[k]) : sec.name;
        if (!stencil)
            for (const Symbol &s : o.symbols)
                if (s.section == order[k] && s.value == 0 && !s.name.empty() && s.name[0] != '.' &&
                    s.name.find('$') == std::string::npos) {
                    name = s.name;
                    break;
                }
        uint32_t flags = (stencil ? PIECE_STENCIL : 0) | (sec.code ? PIECE_CODE : 0) | (tail ? PIECE_TAIL_JUMP : 0);
        pieces += "    {\"" + name + "\", " + std::to_string(nbytes) + ", " + std::to_string(sec.bytes.size()) + ", " +
                  std::to_string(sec.align) + ", " + std::to_string(flags) + ", " + std::to_string(first) + ", " +
                  std::to_string(nrelocs - first) + ", " + std::to_string(sec.unwind.empty() ? 0 : nunwind) + ", " +
                  std::to_string(sec.unwind.size()) + "},\n";
        for (size_t b = 0; b < sec.bytes.size(); b++) {
            if (b % 24 == 0) bytes += "\n   ";
            bytes += hex(sec.bytes[b]);
        }
        for (uint8_t u : sec.unwind) unwind += hex(u);
        nunwind += sec.unwind.size();
        nbytes += sec.bytes.size();
        if (stencil) stencil_bytes += sec.bytes.size();
    }
    std::printf("xfstencil: %zu stencils (%zu bytes), %zu pieces they refer to, %zu holes\n", stencils.size(),
                stencil_bytes, order.size() - stencils.size(), nrelocs);
    return "// Written by tools/xfstencil.cpp from " + source +
           ". Do not edit.\n// clang-format off\n\nnamespace xf::stencil::gen {\n\nconst unsigned char bytes[] = {" +
           bytes + "\n};\n\nconst Reloc relocs[] = {\n" + relocs + "};\n\nconst unsigned char unwind[] = {" +
           (unwind.empty() ? "0" : unwind) + "};\n\nconst Piece pieces[] = {\n" + pieces +
           "};\n\nconstexpr size_t piece_count = " + std::to_string(order.size()) +
           ";\n\n} // namespace xf::stencil::gen\n";
}

std::vector<uint8_t> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot read " + p.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void extract(const fs::path &object, const fs::path &out) {
    std::vector<uint8_t> f = read_file(object);
    Object o;
    if (f.size() >= 4 && std::memcmp(f.data(),
                                     "\x7F"
                                     "ELF",
                                     4) == 0)
        read_elf(f, o);
    else
        read_coff(f, o);
    const std::string text = write_table(o, object.filename().string());
    // written only when it changes, so what includes it is not rebuilt for nothing
    if (fs::exists(out)) {
        std::vector<uint8_t> old = read_file(out);
        if (std::string(old.begin(), old.end()) == text) return;
    }
    std::ofstream(out, std::ios::binary) << text;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--extract") {
        extract(argv[2], argv[3]);
        return 0;
    }
    if (argc != 5 && argc != 7) {
        std::fprintf(stderr, "usage: xfstencil <clang++> <stencils.cpp> <include dir> <out dir> [--target <triple>]\n"
                             "       xfstencil --extract <object> <out.inc>\n");
        return 2;
    }
    const fs::path out = argv[4];
    fs::create_directories(out);
    const fs::path object = out / "stencils.o";
    // the game library's flags at its CPU level for this host's family, each
    // function and constant in a section of its own
    std::string command = std::string("\"") + argv[1] + "\"";
    const std::string target = argc == 7 ? argv[6] : "";
#if defined(__aarch64__) || defined(_M_ARM64)
    bool arm64 = target.empty() || target.starts_with("aarch64") || target.starts_with("arm64");
#else
    bool arm64 = target.starts_with("aarch64") || target.starts_with("arm64");
#endif
#ifdef _WIN32
    const bool elf = target.find("linux") != std::string::npos || target.find("android") != std::string::npos;
#else
    const bool elf = target.find("windows") == std::string::npos;
#endif
    const xf::CpuLevel level = arm64 ? xf::CpuLevel::Arm64 : xf::CpuLevel::X86_64_V3;
    for (const char *flag : xf::build::base_compile_flags(level)) command += std::string(" ") + flag;
    // and no jump tables: one is data that names its function's code, which
    // a copy per use cannot share
    command += " -ffunction-sections -fdata-sections -fno-jump-tables";
    // (ARM64's level flags keep the atomics inline, -mno-outline-atomics: the
    // tier does not provide libgcc's __aarch64_cas* helpers either)
    // ELF: position independent (an absolute 32-bit field would need the code
    // in the low 2 GB); stencils.cpp declares every symbol hidden, so a hole
    // is a pc-relative field, never reached through the GOT
    if (elf) command += " -fPIC";
    if (!target.empty()) command += " --target=" + target;
    command += std::string(" -I\"") + argv[3] + "\" -c \"" + argv[2] + "\" -o \"" + object.string() + "\"";
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe's quoting
#endif
    if (std::system(command.c_str()) != 0) fail("the stencils did not compile: " + command);
    extract(object, out / "stencils.inc");
    return 0;
}
