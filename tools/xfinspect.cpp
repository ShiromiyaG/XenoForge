// xfinspect: what an Xbox 360 game disc or executable holds, read straight
// from the ISO: the executable's headers, the XDK libraries linked into it,
// what it imports from the system, the disc's files, and the image itself
// decrypted and unpacked (for reverse engineering).

#include "xenoforge/bytes.h"
#include "xenoforge/disc.h"
#include "xenoforge/exports.h"
#include "xenoforge/package.h"
#include "xenoforge/xex.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xf;

namespace {

void usage() {
    std::printf("usage: xfinspect <game.iso | file.xex> [options]\n"
                "       xfinspect --package <file> [--list] [--extract <dir>]\n"
                "  --package <file>     a content package (STFS or SVOD: DLC, update, XBLA, system update)\n"
                "  --list               its files\n"
                "  --extract <dir>      write its files under <dir>\n"
                "  --xex <path>         the executable on the disc (default.xex)\n"
                "  --files              list the disc's files\n"
                "  --exports            each export of the module itself (ordinal, name when ours knows it, address)\n"
                "  --imports            each import: library, ordinal, name, kind (tab separated)\n"
                "  --dump-image <file>  write the image decrypted and unpacked (offset 0 = base address)\n"
                "  --dump-xex <file>    write the executable as the disc holds it\n");
}

bool write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    return bool(out);
}

std::string version(uint32_t v) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
    return s;
}

const char *section_name(uint8_t type) {
    switch (XexSection(type)) {
    case XexSection::Code: return "code";
    case XexSection::Data: return "data";
    case XexSection::ReadOnly: return "read-only";
    }
    return "?";
}

const char *approval_name(uint8_t approval) {
    switch (approval) {
    case 0: return "unapproved";
    case 1: return "possibly approved";
    case 2: return "approved";
    case 3: return "expired";
    }
    return "?";
}

void list_files(const DiscEntry &dir, const std::string &prefix) {
    for (const DiscEntry &e : dir.children) {
        std::string path = prefix.empty() ? e.name : prefix + "/" + e.name;
        if (e.directory) {
            std::printf("%12s  %s/\n", "", path.c_str());
            list_files(e, path);
        } else {
            std::printf("%12llu  %s\n", static_cast<unsigned long long>(e.size), path.c_str());
        }
    }
}

void find_executables(const DiscEntry &dir, const std::string &prefix, std::vector<std::string> &out) {
    for (const DiscEntry &e : dir.children) {
        std::string path = prefix.empty() ? e.name : prefix + "/" + e.name;
        if (e.directory) {
            find_executables(e, path, out);
            continue;
        }
        std::string lower = e.name;
        for (char &c : lower)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (lower.ends_with(".xex") || lower.ends_with(".xexp") || lower.ends_with(".xexdll")) out.push_back(path);
    }
}

bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool looks_like_xex(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    return in && std::memcmp(magic, "XEX2", 4) == 0;
}

std::string utf8(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else out += char(0xE0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

std::filesystem::path from_utf8(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

void package_files(const PackageNode &dir, uint64_t &files, uint64_t &bytes, bool list) {
    for (const auto &n : dir.children) {
        if (n->directory) {
            if (list) std::printf("%12s  %s/\n", "", n->path.c_str());
            package_files(*n, files, bytes, list);
        } else {
            files++, bytes += n->size;
            if (list) std::printf("%12llu  %s\n", static_cast<unsigned long long>(n->size), n->path.c_str());
        }
    }
}

// A name from a package is data from outside: nothing in it may leave the
// directory it is written under.
std::string safe_part(const std::string &name) {
    std::string out;
    for (char c : name) out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                                c == '>' || c == '|' || uint8_t(c) < 0x20) ? '_' : c;
    if (out.empty() || out == "." || out == "..") out = "_" + out;
    return out;
}

bool extract(const Package &package, const PackageNode &dir, const std::filesystem::path &to, uint64_t &written) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    for (const auto &n : dir.children) {
        std::filesystem::path path = to / from_utf8(safe_part(n->name));
        if (n->directory) {
            if (!extract(package, *n, path, written)) return false;
            continue;
        }
        std::ofstream out(path, std::ios::binary);
        std::vector<uint8_t> chunk(1 << 20);
        for (uint64_t at = 0; at < n->size;) {
            size_t got = package.read(*n, at, chunk.data(), chunk.size());
            if (!got) {
                std::fprintf(stderr, "%s: only 0x%llX of 0x%llX bytes are in the package\n", n->path.c_str(),
                             static_cast<unsigned long long>(at), static_cast<unsigned long long>(n->size));
                break;
            }
            out.write(reinterpret_cast<const char *>(chunk.data()), std::streamsize(got));
            at += got;
            written += got;
        }
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
            return false;
        }
    }
    return true;
}

int inspect_package(const std::filesystem::path &path, bool list, const std::string &extract_to) {
    std::string error;
    auto package = Package::open(path, error);
    if (!package) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), error.c_str());
        return 1;
    }
    const PackageInfo &i = package->info();
    char magic[5] = {char(i.magic >> 24), char(i.magic >> 16), char(i.magic >> 8), char(i.magic), 0};
    std::printf("package       %s (%s, %s, header 0x%X)\n", path.string().c_str(), magic,
                package->is_stfs() ? "STFS" : "SVOD", i.header_size);
    std::printf("content       type 0x%X  title id %08X  media id %08X  version %s (base %s)\n", i.content_type,
                i.title_id, i.media_id, version(i.version).c_str(), version(i.base_version).c_str());
    std::printf("name          \"%s\" by \"%s\", title \"%s\"\n", utf8(i.display_name[0]).c_str(),
                utf8(i.publisher).c_str(), utf8(i.title_name).c_str());
    std::printf("metadata      version %u, content size 0x%llX, profile %016llX, %zu + %zu bytes of thumbnails\n",
                i.metadata_version, static_cast<unsigned long long>(i.content_size),
                static_cast<unsigned long long>(i.profile_id), package->thumbnail().size(),
                package->title_thumbnail().size());
    if (package->is_stfs())
        std::printf("volume        %u blocks (%u free), file table %u blocks at %u, %s\n", i.stfs.total_block_count,
                    i.stfs.free_block_count, i.stfs.file_table_block_count, i.stfs.file_table_block_number,
                    i.stfs.read_only_format() ? "read-only format" : "two tables per level");
    else
        std::printf("volume        %u data files, %u blocks from block %u, layout %s\n", i.data_file_count,
                    i.svod.num_data_blocks, i.svod.start_data_block, package->layout().c_str());
    uint64_t files = 0, bytes = 0;
    if (list) std::printf("files\n");
    package_files(package->root(), files, bytes, list);
    std::printf("files         %llu, %llu bytes\n", static_cast<unsigned long long>(files),
                static_cast<unsigned long long>(bytes));
    for (const std::string &w : package->warnings()) std::printf("warning       %s\n", w.c_str());
    if (!extract_to.empty()) {
        uint64_t written = 0;
        if (!extract(*package, package->root(), from_utf8(extract_to), written)) return 1;
        std::printf("extracted     %llu bytes to %s\n", static_cast<unsigned long long>(written), extract_to.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path input, package_input;
    std::string xex_path = "default.xex", dump_path, dump_xex_path, extract_path;
    bool files = false, imports = false, list = false, exports = false;
    std::string table_library; // the export table of ours that names this module's ordinals
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--xex" && i + 1 < argc) xex_path = argv[++i];
        else if (a == "--package" && i + 1 < argc) package_input = std::filesystem::path(argv[++i]);
        else if (a == "--list") list = true;
        else if (a == "--extract" && i + 1 < argc) extract_path = argv[++i];
        else if (a == "--files") files = true;
        else if (a == "--imports") imports = true;
        else if (a == "--exports") exports = true;
        else if (a == "--dump-image" && i + 1 < argc) dump_path = argv[++i];
        else if (a == "--dump-xex" && i + 1 < argc) dump_xex_path = argv[++i];
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (input.empty() && !a.starts_with("--")) input = std::filesystem::path(a);
        else {
            usage();
            return 2;
        }
    }
    if (!package_input.empty()) return inspect_package(package_input, list, extract_path);
    {
        std::string stem = input.filename().string();
        for (char &c : stem) c = char(std::tolower(uint8_t(c)));
        if (stem == "$flash_xam.xex" || stem == "xam.xex") table_library = "xam.xex";
    }
    if (input.empty()) {
        usage();
        return 2;
    }

    std::string error;
    std::vector<uint8_t> file;
    if (looks_like_xex(input)) {
        if (!read_file(input, file)) {
            std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
            return 1;
        }
        std::printf("executable    %s (%zu bytes)\n", input.string().c_str(), file.size());
    } else {
        auto disc = Disc::open(input, error);
        if (!disc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("disc          %s (game partition at 0x%llX)\n", input.string().c_str(),
                    static_cast<unsigned long long>(disc->game_offset()));
        if (files) list_files(disc->root(), "");
        std::vector<std::string> executables;
        find_executables(disc->root(), "", executables);
        if (executables.size() > 1) {
            std::printf("executables  ");
            for (const std::string &e : executables) std::printf(" %s", e.c_str());
            std::printf("\n");
        }
        const DiscEntry *entry = disc->find(xex_path);
        if (!entry || entry->directory) {
            std::fprintf(stderr, "no %s on the disc\n", xex_path.c_str());
            return 1;
        }
        file = disc->read_all(*entry);
        if (file.size() != entry->size) {
            std::fprintf(stderr, "cannot read %s from the disc\n", xex_path.c_str());
            return 1;
        }
        std::printf("executable    %s (%zu bytes)\n", xex_path.c_str(), file.size());
    }
    if (!dump_xex_path.empty()) {
        if (!write_file(dump_xex_path, file)) {
            std::fprintf(stderr, "cannot write %s\n", dump_xex_path.c_str());
            return 1;
        }
        std::printf("wrote         %s\n", dump_xex_path.c_str());
    }

    Xex xex;
    if (!parse_xex(file, xex, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (xex.has_execution_info) {
        const XexExecutionInfo &x = xex.execution;
        std::printf("title id      %08X\n", x.title_id);
        std::printf("media id      %08X\n", x.media_id);
        std::printf("version       %s (base %s)\n", version(x.version).c_str(), version(x.base_version).c_str());
        std::printf("disc          %u of %u\n", x.disc_number, x.disc_count);
    }
    std::printf("module flags  0x%08X%s\n", xex.module_flags, xex.is_patch() ? " (patch)" : "");
    std::printf("base          0x%08X  entry 0x%08X  image 0x%X bytes  pages of 0x%X\n", xex.base_address,
                xex.entry_point, xex.image_size, xex.page_size());
    std::printf("stack, heap   0x%X, 0x%X\n", xex.stack_size, xex.heap_size);
    std::printf("encryption    %s  compression %s", xex.encryption == XexEncryption::Normal ? "normal" : "none",
                compression_name(xex.compression));
    if (xex.compression == XexCompression::Normal) std::printf(", window 0x%X", xex.lzx_window);
    std::printf("\n");
    if (!xex.original_pe_name.empty()) std::printf("original name %s\n", xex.original_pe_name.c_str());

    // consecutive pages of a kind as one section
    uint32_t address = xex.base_address;
    for (size_t i = 0; i < xex.pages.size();) {
        uint8_t type = xex.pages[i].type;
        uint64_t size = 0;
        for (; i < xex.pages.size() && xex.pages[i].type == type; i++)
            size += uint64_t(xex.pages[i].count) * xex.page_size();
        std::printf("section       %-9s 0x%08X-0x%08llX\n", section_name(type), address,
                    static_cast<unsigned long long>(address + size));
        address += uint32_t(size);
    }

    if (!xex.static_libraries.empty()) {
        std::printf("XDK libraries\n");
        for (const XexStaticLibrary &l : xex.static_libraries)
            std::printf("  %-9s %u.%u.%u.%u  %s\n", l.name.c_str(), l.major, l.minor, l.build, l.qfe,
                        approval_name(l.approval()));
    }

    bool have_image = load_xex_image(file, xex, error);
    if (!have_image) std::printf("image         not loaded: %s\n", error.c_str());
    else if (xex.encryption == XexEncryption::None) std::printf("image         loaded (not encrypted)\n");
    else std::printf("image         decrypted with the %s key\n", xex.devkit ? "devkit" : "retail");

    // the module's own exports: the table in the image, at the address the
    // security info gives (xex2_export_table: count and base at 0x24 and 0x28,
    // then an address for each ordinal, the image base's upper half added)
    if (have_image && xex.header.size() >= size_t(xex.security_offset) + 0x164) {
        uint32_t address = be32(xex.header.data() + xex.security_offset + 0x160);
        uint64_t at = uint64_t(address) - xex.base_address;
        if (address && address >= xex.base_address && at + 0x2C <= xex.image.size()) {
            const uint8_t *t = xex.image.data() + at;
            uint32_t count = be32(t + 0x24), base = be32(t + 0x28), image_base = be32(t + 0x20);
            if (at + 0x2C + uint64_t(count) * 4 <= xex.image.size()) {
                uint32_t used = 0, named = 0;
                std::string listing;
                for (uint32_t n = 0; n < count; n++) {
                    uint32_t offset = be32(t + 0x2C + 4 * n);
                    if (!offset) continue;
                    used++;
                    const Export *e = table_library.empty() ? nullptr : find_export(table_library, uint16_t(base + n));
                    if (e) named++;
                    char line[160];
                    std::snprintf(line, sizeof(line), "%s\t0x%03X\t%s\t0x%08X\n", "export", base + n,
                                  e ? e->name : "?", offset + (image_base << 16));
                    listing += line;
                }
                std::printf("exports       %u slots from ordinal 0x%X, %u used, %u named in our table\n", count, base,
                            used, named);
                if (exports) std::fputs(listing.c_str(), stdout);
            } else {
                std::printf("exports       table at 0x%08X is not in the image\n", address);
            }
        } else {
            std::printf("exports       none\n");
        }
    }

    // one line per (library, ordinal); the descriptor and the thunk count once
    struct Seen {
        bool thunk = false;
    };
    std::map<std::pair<size_t, uint16_t>, Seen> unique;
    if (have_image)
        for (const XexImport &i : list_imports(xex)) unique[{i.library, i.ordinal}].thunk |= i.thunk;

    std::printf("imports\n");
    for (size_t li = 0; li < xex.import_libraries.size(); li++) {
        const XexImportLibrary &lib = xex.import_libraries[li];
        int functions = 0, variables = 0, unknown = 0;
        for (const auto &[key, seen] : unique) {
            if (key.first != li) continue;
            const Export *e = find_export(lib.name, key.second);
            if (!e) unknown++;
            else if (e->kind == ExportKind::Function) functions++;
            else variables++;
        }
        std::printf("  %-13s %-14s %d functions, %d variables", lib.name.c_str(), version(lib.version).c_str(),
                    functions, variables);
        if (unknown) std::printf(", %d unknown ordinals", unknown);
        if (!have_image) std::printf(" (%zu records)", lib.records.size());
        std::printf("\n");
    }
    if (imports) {
        for (const auto &[key, seen] : unique) {
            const std::string &lib = xex.import_libraries[key.first].name;
            const Export *e = find_export(lib, key.second);
            std::printf("%s\t0x%03X\t%s\t%s\n", lib.c_str(), key.second, e ? e->name : "?",
                        !e ? "?" : e->kind == ExportKind::Function ? "function" : "variable");
        }
    }

    if (!dump_path.empty()) {
        if (!have_image) {
            std::fprintf(stderr, "no image to dump\n");
            return 1;
        }
        std::ofstream out(std::filesystem::path(dump_path), std::ios::binary);
        out.write(reinterpret_cast<const char *>(xex.image.data()), std::streamsize(xex.image.size()));
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", dump_path.c_str());
            return 1;
        }
        std::printf("wrote         %s (0x%zX bytes at 0x%08X)\n", dump_path.c_str(), xex.image.size(),
                    xex.base_address);
    }
    return 0;
}
