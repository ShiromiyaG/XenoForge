// The executable as the recompiler sees it (recomp.h): the PE headers inside
// the XEX's image (little-endian, as PE is) give the sections and the
// exception directory; .pdata's entries are the PPC's, big-endian.

#include "xenoforge/recomp.h"

#include <cstring>

namespace xf::recomp {
namespace {

uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

} // namespace

uint32_t Image::word(uint32_t address) const {
    if (!contains(address)) return 0;
    return be32(bytes.data() + (address - base));
}

const Section *Image::section_of(uint32_t address) const {
    for (const Section &s : sections)
        if (address >= s.address && address - s.address < s.size) return &s;
    return nullptr;
}

const char *source_name(Source s) {
    switch (s) {
    case Source::Import: return "import";
    case Source::Helper: return "helper";
    case Source::Pdata: return "pdata";
    case Source::Entry: return "entry";
    case Source::Export: return "export";
    case Source::Pointer: return "pointer";
    case Source::Branch: return "branch";
    case Source::Call: return "call";
    case Source::Gap: return "gap";
    }
    return "?";
}

bool read_image(const Xex &xex, Image &out, std::string &error) {
    out = Image{};
    out.base = xex.base_address;
    out.entry = xex.entry_point;
    out.bytes = xex.image;
    const std::vector<uint8_t> &b = out.bytes;
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return error = "the image has no MZ header", false;
    uint32_t pe = le32(&b[0x3C]);
    if (uint64_t(pe) + 24 > b.size() || std::memcmp(&b[pe], "PE\0\0", 4) != 0) return error = "the image has no PE header", false;
    uint16_t section_count = le16(&b[pe + 6]), optional_size = le16(&b[pe + 20]);
    uint32_t optional = pe + 24;
    if (uint64_t(optional) + optional_size + uint64_t(section_count) * 40 > b.size())
        return error = "the PE headers run past the image", false;
    if (le16(&b[optional]) != 0x10B) return error = "not a PE32 optional header", false;

    for (uint32_t k = 0; k < section_count; k++) {
        const uint8_t *h = &b[optional + optional_size + 40 * k];
        Section s;
        s.name.assign(reinterpret_cast<const char *>(h), strnlen(reinterpret_cast<const char *>(h), 8));
        s.size = le32(h + 8);
        s.address = out.base + le32(h + 12);
        uint32_t flags = le32(h + 36);
        s.code = (flags & 0x20) || (flags & 0x20000000); // code, or executable
        s.writable = flags & 0x80000000;
        out.sections.push_back(s);
    }

    // the exception directory (data directory 3): .pdata
    uint32_t directories = le32(&b[optional + 92]);
    if (directories > 3) {
        uint32_t rva = le32(&b[optional + 96 + 8 * 3]), size = le32(&b[optional + 96 + 8 * 3 + 4]);
        if (rva && uint64_t(rva) + size <= b.size()) {
            for (uint32_t at = rva; at + 8 <= rva + size; at += 8) {
                PdataEntry e;
                e.begin = be32(&b[at]);
                uint32_t data = be32(&b[at + 4]);
                if (!e.begin) continue;
                e.prolog = data & 0xFF;
                uint32_t instructions = (data >> 8) & 0x3FFFFF;
                e.length = (instructions ? instructions : 1) * 4;
                e.exception = data >> 31;
                out.pdata.push_back(e);
            }
        }
    }

    for (const XexImport &i : list_imports(xex))
        if (i.thunk) out.imports.push_back({xex.import_libraries[i.library].name, i.ordinal, i.record});

    // A DLL's exports, as the kernel's GetProcAddress finds them (Xenia's
    // XexModule::GetProcAddress): the export table the security info points
    // at (xex2_export_table: the image base >> 16 at 0x20, the count at
    // 0x24, the offsets from 0x2C), and the PE export directory
    // XEX_HEADER_EXPORTS_BY_NAME gives (its RVA first; AddressOfFunctions'
    // RVAs, little-endian). Data exports are no code and are not starts.
    const std::vector<uint8_t> &h = xex.header;
    if (uint64_t(xex.security_offset) + 0x164 <= h.size()) {
        uint32_t table = be32(&h[xex.security_offset + 0x160]);
        if (table && out.contains(table, 0x2C)) {
            uint32_t image_base = out.word(table + 0x20) << 16, count = out.word(table + 0x24);
            for (uint32_t k = 0; k < count && out.contains(table + 0x2C + 4 * k); k++)
                if (uint32_t offset = out.word(table + 0x2C + 4 * k)) out.exports.push_back(image_base + offset);
        }
    }
    for (const auto &[key, offset] : xex.opt_headers) {
        if (key != 0x00E10402 || uint64_t(offset) + 4 > h.size()) continue;
        uint32_t directory = be32(&h[offset]);
        if (uint64_t(directory) + 0x28 > b.size()) continue;
        uint32_t count = le32(&b[directory + 0x14]), functions = le32(&b[directory + 0x1C]);
        for (uint32_t k = 0; k < count && uint64_t(functions) + 4 * (k + 1) <= b.size(); k++)
            if (uint32_t rva = le32(&b[functions + 4 * k])) out.exports.push_back(out.base + rva);
    }
    return true;
}

} // namespace xf::recomp
