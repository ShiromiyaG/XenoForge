// XEX2 headers and image. Translated from Xenia's XexModule
// (src/xenia/cpu/xex_module.cc: GetOptHeader, ReadImage*, the import
// libraries in LoadContinue) and its structures (src/xenia/kernel/util/
// xex2_info.h), BSD; see LICENSE-xenia. Every offset read from the file is
// checked against it. The LZX decoder normal compression ends in is ours
// (lzx.cpp), not Xenia's (libmspack).

#include "xenoforge/xex.h"

#include "xenoforge/aes.h"
#include "xenoforge/bytes.h"
#include "xenoforge/lzx.h"
#include "xenoforge/sha1.h"

#include <algorithm>
#include <cstring>

namespace xf {
namespace {

// What the console decrypts an executable's file key with (Xenia's
// xe_xex2_retail_key); development kits use zeros.
constexpr uint8_t RETAIL_KEY[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr uint8_t DEVKIT_KEY[16] = {};

bool fits(std::span<const uint8_t> file, uint64_t offset, uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
}

const std::pair<uint32_t, uint32_t> *find_header(const Xex &xex, uint32_t key) {
    for (const auto &h : xex.opt_headers)
        if (h.first == key) return &h;
    return nullptr;
}

// A header whose value sits in the header itself (key's low byte 0x00 or 0x01).
bool header_value(const Xex &xex, uint32_t key, uint32_t &value) {
    const auto *h = find_header(xex, key);
    if (!h) return false;
    value = h->second;
    return true;
}

// A header stored at an offset, with at least min_size bytes there.
const uint8_t *header_data(std::span<const uint8_t> file, const Xex &xex, uint32_t key, uint32_t min_size) {
    const auto *h = find_header(xex, key);
    if (!h || !fits(file, h->second, min_size)) return nullptr;
    return file.data() + h->second;
}

bool parse_imports(std::span<const uint8_t> file, Xex &xex, std::string &error) {
    const auto *h = find_header(xex, xex_key::IMPORT_LIBRARIES);
    if (!h) return true;
    const uint8_t *p = header_data(file, xex, xex_key::IMPORT_LIBRARIES, 12);
    uint32_t total = p ? be32(p) : 0;
    if (!p || total < 12 || !fits(file, h->second, total)) {
        error = "the import libraries header runs past the file";
        return false;
    }
    uint32_t strings_size = be32(p + 4), strings_count = be32(p + 8);
    if (uint64_t(strings_size) + 12 > total) {
        error = "the import libraries' names run past their header";
        return false;
    }
    std::vector<std::string> names;
    const char *strings = reinterpret_cast<const char *>(p + 12);
    for (uint32_t i = 0; i < strings_size && names.size() < strings_count;) {
        size_t length = strnlen(strings + i, strings_size - i);
        names.emplace_back(strings + i, length);
        i += uint32_t(length) + 1;
        i = (i + 3) & ~3u; // each name padded to 4 bytes
    }

    for (uint32_t offset = 12 + strings_size; offset + 0x28 <= total;) {
        const uint8_t *lib = p + offset;
        uint32_t size = be32(lib);
        if (size == 0) break;
        uint16_t count = be16(lib + 0x26);
        if (size < 0x28 + uint32_t(count) * 4 || uint64_t(offset) + size > total) {
            error = "an import library runs past its header";
            return false;
        }
        uint32_t name_index = be16(lib + 0x24) & 0xFF;
        XexImportLibrary library;
        library.name = name_index < names.size() ? names[name_index] : "?";
        library.id = be32(lib + 0x18);
        library.version = be32(lib + 0x1C);
        library.version_min = be32(lib + 0x20);
        for (uint32_t i = 0; i < count; i++) library.records.push_back(be32(lib + 0x28 + 4 * i));
        xex.import_libraries.push_back(std::move(library));
        offset += size;
    }
    return true;
}

uint64_t pages_size(const Xex &xex) {
    uint64_t size = 0;
    for (const XexPages &pages : xex.pages) size += uint64_t(pages.count) * xex.page_size();
    return size;
}

// The image zeroed: the security info's size, or its pages' if more.
bool allocate_image(const Xex &xex, std::vector<uint8_t> &image, std::string &error) {
    uint64_t size = std::max<uint64_t>(xex.image_size, pages_size(xex));
    if (size > (512u << 20)) {
        error = "the image is implausibly large";
        return false;
    }
    image.assign(size, 0);
    return true;
}

bool unpack_basic(std::span<const uint8_t> file, const Xex &xex, const std::vector<uint8_t> &data,
                  std::vector<uint8_t> &image, std::string &error) {
    const uint8_t *info = header_data(file, xex, xex_key::FILE_FORMAT_INFO, 8);
    uint32_t info_size = be32(info);
    if (info_size < 8 || !fits(file, find_header(xex, xex_key::FILE_FORMAT_INFO)->second, info_size)) {
        error = "the basic compression's blocks run past the file";
        return false;
    }
    if (!allocate_image(xex, image, error)) return false;
    const uint64_t size = image.size();

    // each block: data_size bytes from the file, then zero_size zeros
    uint64_t from = 0, to = 0;
    for (uint32_t off = 8; off + 8 <= info_size; off += 8) {
        uint32_t data_size = be32(info + off), zero_size = be32(info + off + 4);
        if (data_size > data.size() - std::min<uint64_t>(from, data.size()) || to + data_size > size) {
            error = "a basic compression block runs past the file or the image";
            return false;
        }
        std::memcpy(image.data() + to, data.data() + from, data_size);
        from += data_size;
        to += uint64_t(data_size) + zero_size;
    }
    return true;
}

enum class Unpacked { Ok, WrongKey, Failed };

// Normal compression (ReadImageCompressed): a chain of blocks, the first
// one's size and SHA-1 in the file format info, each next one's in the 24
// bytes that start the block before it. After those, a block holds chunks of
// LZX data, each after its big-endian 16-bit size, until a size of 0. The
// chunks joined are one LZX stream, decompressed into the image with the
// window size of the file format info. A block whose SHA-1 differs was
// decrypted with the wrong key: the caller tries the next one.
Unpacked unpack_normal(std::span<const uint8_t> file, const Xex &xex, const std::vector<uint8_t> &data,
                       std::vector<uint8_t> &image, std::string &error) {
    const uint8_t *info = header_data(file, xex, xex_key::FILE_FORMAT_INFO, 36);
    if (!info || be32(info) < 36) {
        error = "the normal compression's header runs past the file";
        return Unpacked::Failed;
    }
    std::vector<uint8_t> stream;
    stream.reserve(data.size());
    uint32_t block_size = be32(info + 12);
    const uint8_t *block_hash = info + 16;
    for (size_t at = 0; block_size;) {
        if (block_size < 24 || block_size > data.size() - at) {
            error = "a compressed block runs past the file";
            return Unpacked::Failed;
        }
        const uint8_t *block = data.data() + at;
        uint8_t digest[20];
        sha1({block, block_size}, digest);
        if (std::memcmp(digest, block_hash, 20) != 0) {
            error = "a compressed block's SHA-1 differs";
            return Unpacked::WrongKey;
        }
        for (uint32_t p = 24;;) {
            if (p + 2 > block_size) {
                error = "a compressed block's chunks run past it";
                return Unpacked::Failed;
            }
            uint32_t chunk = be16(block + p);
            p += 2;
            if (chunk == 0) break;
            if (chunk > block_size - p) {
                error = "a compressed block's chunks run past it";
                return Unpacked::Failed;
            }
            stream.insert(stream.end(), block + p, block + p + chunk);
            p += chunk;
        }
        at += block_size;
        block_size = be32(block);
        block_hash = block + 4;
    }

    if (!allocate_image(xex, image, error)) return Unpacked::Failed;
    // Xenia decompresses the pages' size (its image_size())
    uint64_t length = pages_size(xex) ? pages_size(xex) : xex.image_size;
    if (!lzx_decompress(stream, std::span(image).first(size_t(length)), xex.lzx_window, error)) return Unpacked::Failed;
    return Unpacked::Ok;
}

} // namespace

const char *compression_name(XexCompression c) {
    switch (c) {
    case XexCompression::None: return "none";
    case XexCompression::Basic: return "basic";
    case XexCompression::Normal: return "normal (LZX)";
    case XexCompression::Delta: return "delta";
    }
    return "?";
}

bool parse_xex(std::span<const uint8_t> file, Xex &out, std::string &error) {
    out = Xex{};
    const uint8_t *f = file.data();
    if (!fits(file, 0, 0x18) || std::memcmp(f, "XEX2", 4) != 0) {
        error = "not an XEX2 executable";
        return false;
    }
    out.module_flags = be32(f + 4);
    out.header_size = be32(f + 8);
    out.security_offset = be32(f + 0x10);
    uint32_t count = be32(f + 0x14);
    if (!fits(file, 0x18, uint64_t(count) * 8) || out.header_size > file.size()) {
        error = "the headers run past the file";
        return false;
    }
    for (uint32_t i = 0; i < count; i++)
        out.opt_headers.emplace_back(be32(f + 0x18 + 8 * i), be32(f + 0x1C + 8 * i));

    // the security info: the image's size, address, key and pages
    uint32_t so = out.security_offset;
    if (!fits(file, so, 0x184)) {
        error = "the security info runs past the file";
        return false;
    }
    out.image_size = be32(f + so + 4);
    out.image_flags = be32(f + so + 0x10C);
    out.load_address = be32(f + so + 0x110);
    std::memcpy(out.file_key, f + so + 0x150, 16);
    uint32_t page_descriptors = be32(f + so + 0x180);
    if (!fits(file, uint64_t(so) + 0x184, uint64_t(page_descriptors) * 24)) {
        error = "the page descriptors run past the file";
        return false;
    }
    for (uint32_t i = 0; i < page_descriptors; i++) {
        uint32_t v = be32(f + so + 0x184 + 24 * i);
        out.pages.push_back({uint8_t(v & 0xF), v >> 4});
    }

    out.base_address = out.load_address;
    header_value(out, xex_key::IMAGE_BASE_ADDRESS, out.base_address);
    header_value(out, xex_key::ENTRY_POINT, out.entry_point);
    header_value(out, xex_key::DEFAULT_STACK_SIZE, out.stack_size);
    header_value(out, xex_key::DEFAULT_HEAP_SIZE, out.heap_size);

    if (const uint8_t *p = header_data(file, out, xex_key::FILE_FORMAT_INFO, 8)) {
        out.encryption = XexEncryption(be16(p + 4));
        out.compression = XexCompression(be16(p + 6));
        if (out.compression == XexCompression::Normal && be32(p) >= 12) out.lzx_window = be32(p + 8);
    } else {
        error = "no file format info";
        return false;
    }

    if (const uint8_t *p = header_data(file, out, xex_key::EXECUTION_INFO, 0x18)) {
        out.has_execution_info = true;
        out.execution.media_id = be32(p);
        out.execution.version = be32(p + 4);
        out.execution.base_version = be32(p + 8);
        out.execution.title_id = be32(p + 0xC);
        out.execution.platform = p[0x10];
        out.execution.executable_table = p[0x11];
        out.execution.disc_number = p[0x12];
        out.execution.disc_count = p[0x13];
        out.execution.savegame_id = be32(p + 0x14);
    }

    if (const uint8_t *p = header_data(file, out, xex_key::ORIGINAL_PE_NAME, 4)) {
        uint32_t size = be32(p);
        const auto *h = find_header(out, xex_key::ORIGINAL_PE_NAME);
        if (size > 4 && fits(file, h->second, size)) {
            const char *name = reinterpret_cast<const char *>(p + 4);
            out.original_pe_name.assign(name, strnlen(name, size - 4));
        }
    }

    if (const uint8_t *p = header_data(file, out, xex_key::STATIC_LIBRARIES, 4)) {
        uint32_t size = be32(p);
        const auto *h = find_header(out, xex_key::STATIC_LIBRARIES);
        if (size >= 4 && fits(file, h->second, size)) {
            for (uint32_t off = 4; off + 16 <= size; off += 16) {
                const uint8_t *lib = p + off;
                XexStaticLibrary library;
                library.name.assign(reinterpret_cast<const char *>(lib), strnlen(reinterpret_cast<const char *>(lib), 8));
                library.major = be16(lib + 8);
                library.minor = be16(lib + 0xA);
                library.build = be16(lib + 0xC);
                library.flags = lib[0xE];
                library.qfe = lib[0xF];
                out.static_libraries.push_back(std::move(library));
            }
        }
    }

    return parse_imports(file, out, error);
}

bool load_xex_image(std::span<const uint8_t> file, Xex &xex, std::string &error) {
    if (xex.is_patch()) {
        error = "a patch (.xexp) has no image of its own: it applies to its base executable";
        return false;
    }
    if (xex.compression != XexCompression::None && xex.compression != XexCompression::Basic &&
        xex.compression != XexCompression::Normal) {
        error = std::string("unsupported compression: ") + compression_name(xex.compression);
        return false;
    }
    std::span<const uint8_t> payload = file.subspan(xex.header_size);

    std::string why;
    for (const uint8_t *key : {RETAIL_KEY, DEVKIT_KEY}) {
        std::vector<uint8_t> data(payload.begin(), payload.end());
        if (xex.encryption == XexEncryption::Normal) {
            // the file key, decrypted with the console's, is the image's key
            uint8_t session[16];
            std::memcpy(session, xex.file_key, 16);
            aes::decrypt_ecb(aes::expand(key), session, 16);
            const uint8_t iv[16] = {};
            aes::decrypt_cbc(aes::expand(session), iv, data.data(), data.size() & ~size_t(15));
        }

        std::vector<uint8_t> image;
        if (xex.compression == XexCompression::None) {
            image = std::move(data);
            if (xex.image_size) image.resize(xex.image_size);
        } else if (xex.compression == XexCompression::Basic) {
            if (!unpack_basic(file, xex, data, image, error)) return false;
        } else {
            Unpacked unpacked = unpack_normal(file, xex, data, image, error);
            if (unpacked == Unpacked::Failed) return false;
            if (unpacked == Unpacked::WrongKey) {
                why = error;
                if (xex.encryption == XexEncryption::None) break;
                continue;
            }
        }

        if (image.size() >= 2 && image[0] == 'M' && image[1] == 'Z') {
            xex.image = std::move(image);
            xex.devkit = key == DEVKIT_KEY;
            return true;
        }
        why = "the image is not a PE image after decryption";
        if (xex.encryption == XexEncryption::None) break;
    }
    error = why;
    if (xex.encryption == XexEncryption::Normal) error += " (neither the retail nor the devkit key)";
    return false;
}

std::vector<XexImport> list_imports(const Xex &xex) {
    std::vector<XexImport> imports;
    for (size_t li = 0; li < xex.import_libraries.size(); li++) {
        for (uint32_t record : xex.import_libraries[li].records) {
            if (record < xex.base_address || uint64_t(record - xex.base_address) + 4 > xex.image.size()) continue;
            uint32_t value = be32(xex.image.data() + (record - xex.base_address));
            imports.push_back({li, uint16_t(value & 0xFFFF), (value >> 24) == 1, record});
        }
    }
    return imports;
}

} // namespace xf
