// xfinspect: what an Xbox 360 game disc or executable holds, read straight
// from the ISO: the executable's headers, the XDK libraries linked into it,
// what it imports from the system, the disc's files, and the image itself
// decrypted and unpacked (for reverse engineering).

#include "xenoforge/disc.h"
#include "xenoforge/exports.h"
#include "xenoforge/xex.h"

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
                "  --xex <path>         the executable on the disc (default.xex)\n"
                "  --files              list the disc's files\n"
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

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path input;
    std::string xex_path = "default.xex", dump_path, dump_xex_path;
    bool files = false, imports = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--xex" && i + 1 < argc) xex_path = argv[++i];
        else if (a == "--files") files = true;
        else if (a == "--imports") imports = true;
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
    else std::printf("image         decrypted with the %s key\n", xex.devkit ? "devkit" : "retail");

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
