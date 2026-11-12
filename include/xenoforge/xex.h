// The console's executables (XEX2): the headers, the image decrypted and
// unpacked, and what it imports from the system. Translated from Xenia's
// XexModule and xex2_info.h (see LICENSE-xenia).

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xf {

enum class XexEncryption : uint16_t { None = 0, Normal = 1 };
enum class XexCompression : uint16_t { None = 0, Basic = 1, Normal = 2, Delta = 3 };

// Optional header keys (xex2_header_keys in Xenia). The low byte says how the
// value is stored: 0x00 or 0x01 in the header itself, otherwise at an offset.
namespace xex_key {
constexpr uint32_t RESOURCE_INFO = 0x000002FF;
constexpr uint32_t FILE_FORMAT_INFO = 0x000003FF;
constexpr uint32_t DELTA_PATCH_DESCRIPTOR = 0x000005FF;
constexpr uint32_t BOUNDING_PATH = 0x000080FF;
constexpr uint32_t ORIGINAL_BASE_ADDRESS = 0x00010001;
constexpr uint32_t ENTRY_POINT = 0x00010100;
constexpr uint32_t IMAGE_BASE_ADDRESS = 0x00010201;
constexpr uint32_t IMPORT_LIBRARIES = 0x000103FF;
constexpr uint32_t ORIGINAL_PE_NAME = 0x000183FF;
constexpr uint32_t STATIC_LIBRARIES = 0x000200FF;
constexpr uint32_t TLS_INFO = 0x00020104;
constexpr uint32_t DEFAULT_STACK_SIZE = 0x00020200;
constexpr uint32_t DEFAULT_FILESYSTEM_CACHE_SIZE = 0x00020301;
constexpr uint32_t DEFAULT_HEAP_SIZE = 0x00020401;
constexpr uint32_t SYSTEM_FLAGS = 0x00030000;
constexpr uint32_t EXECUTION_INFO = 0x00040006;
constexpr uint32_t TITLE_WORKSPACE_SIZE = 0x00040201;
constexpr uint32_t GAME_RATINGS = 0x00040310;
constexpr uint32_t LAN_KEY = 0x00040404;
constexpr uint32_t ALTERNATE_TITLE_IDS = 0x000407FF;
constexpr uint32_t ADDITIONAL_TITLE_MEMORY = 0x00040801;
} // namespace xex_key

enum class XexSection : uint8_t { Code = 1, Data = 2, ReadOnly = 3 };

struct XexPages {
    uint8_t type; // XexSection, or what the file says
    uint32_t count;
};

// An XDK library linked into the program, with its version (the XDK's).
struct XexStaticLibrary {
    std::string name;
    uint16_t major, minor, build;
    uint8_t qfe;
    // Bits 5-6 hold the approval; Xenia's struct takes the whole byte, but
    // the games seen store 0x40 (approved), 0x04 or 0x44: what bit 2 means
    // is not known.
    uint8_t flags;
    uint8_t approval() const { return (flags >> 5) & 3; } // 0 unapproved, 1 possible, 2 approved, 3 expired
};

struct XexImportLibrary {
    std::string name; // "xboxkrnl.exe", "xam.xex"...
    uint32_t id = 0;
    uint32_t version = 0, version_min = 0;
    std::vector<uint32_t> records; // addresses of the import records in the image
};

struct XexExecutionInfo {
    uint32_t media_id = 0, version = 0, base_version = 0, title_id = 0;
    uint8_t platform = 0, executable_table = 0, disc_number = 0, disc_count = 0;
    uint32_t savegame_id = 0;
};

// The thread local storage the executable asks for (TLS_INFO; Xenia's
// xex2_opt_tls_info): slots per thread, and __declspec(thread) data copied
// into each thread's block from the image.
struct XexTls {
    uint32_t slot_count = 0;
    uint32_t raw_data_address = 0;
    uint32_t data_size = 0;
    uint32_t raw_data_size = 0;
};

struct Xex {
    // the main header
    uint32_t module_flags = 0;
    uint32_t header_size = 0; // where the image's data starts in the file
    uint32_t security_offset = 0;
    std::vector<std::pair<uint32_t, uint32_t>> opt_headers; // key, value or offset
    // the file's first header_size bytes, as Xenia's XexModule keeps them (the
    // kernel copies them into guest memory for RtlImageXexHeaderField)
    std::vector<uint8_t> header;

    // from the security info
    uint32_t image_size = 0;
    uint32_t image_flags = 0;
    uint32_t load_address = 0;
    uint8_t file_key[16] = {};
    std::vector<XexPages> pages;

    // from the optional headers
    uint32_t base_address = 0; // IMAGE_BASE_ADDRESS, else load_address
    uint32_t entry_point = 0;
    uint32_t stack_size = 0, heap_size = 0;
    XexEncryption encryption = XexEncryption::None;
    XexCompression compression = XexCompression::None;
    uint32_t lzx_window = 0;
    bool has_execution_info = false;
    XexExecutionInfo execution;
    bool has_tls = false;
    XexTls tls;
    std::string original_pe_name;
    std::vector<XexStaticLibrary> static_libraries;
    std::vector<XexImportLibrary> import_libraries;

    // the image at base_address, once load_image succeeds
    std::vector<uint8_t> image;
    bool devkit = false; // decrypted with the devkit key

    uint32_t page_size() const { return (image_flags & 0x10000000) ? 0x1000 : 0x10000; }
    // MODULE_PATCH, PATCH_FULL or PATCH_DELTA: a title update's .xexp
    bool is_patch() const { return (module_flags & 0x70) != 0; }
};

// The headers alone (no decryption). The file stays the caller's.
bool parse_xex(std::span<const uint8_t> file, Xex &out, std::string &error);

// Decrypts (retail key, else devkit) and unpacks the image into xex.image.
bool load_xex_image(std::span<const uint8_t> file, Xex &xex, std::string &error);

// A title update's patch (a .xexp, Xex::is_patch) applied to its base
// executable, loaded by load_xex_image: the headers and the image as the
// update makes them (xex.header, the fields read from it, xex.image). The
// patch names the executable it was made for (the SHA-1 of its signature):
// another one is refused, and xex is left as it was, as after a patch whose
// headers or keys are wrong. Once the image is being patched a failure
// leaves xex without one.
bool apply_xex_patch(Xex &xex, std::span<const uint8_t> patch_file, std::string &error);

// parse_xex, load_xex_image and, with a patch, apply_xex_patch
bool load_xex(std::span<const uint8_t> file, std::span<const uint8_t> patch, Xex &xex, std::string &error);

// One import: a function's thunk or a variable, from the records in the image.
struct XexImport {
    size_t library; // index in import_libraries
    uint16_t ordinal;
    bool thunk;      // record type 1; type 0 is the descriptor every import has
    uint32_t record; // its address
};

std::vector<XexImport> list_imports(const Xex &xex);

// A digest of an image as load_xex_image unpacks it (64 bits, not
// cryptographic; the host's byte order): what ties a module's generated code
// to the image it was made from (ppc_module.h). Another version of the module
// or a title update gives another digest. Fast enough for every load (a few
// milliseconds for 30 MB).
uint64_t image_digest(std::span<const uint8_t> image);

const char *compression_name(XexCompression c);

} // namespace xf
