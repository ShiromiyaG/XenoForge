// XEX2 on an executable made here: encrypted with a session key (the file key
// sealed with the retail key, then with the devkit's), packed with basic
// compression, with two import libraries; its headers, image and imports
// read back. Cut short, it is refused without reading past the end.

#include "check.h"
#include "xenoforge/aes.h"
#include "xenoforge/bytes.h"
#include "xenoforge/exports.h"
#include "xenoforge/xex.h"

#include <cstring>
#include <vector>

using namespace xf;

namespace {

constexpr uint8_t RETAIL_KEY[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr uint32_t BASE = 0x82000000;
constexpr uint32_t HEADER_SIZE = 0x1000;

void put_be16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

// The image as the game sees it: "MZ", the import records, a second block
// after 0x100 zeros.
std::vector<uint8_t> plain_image() {
    std::vector<uint8_t> image(0x10000, 0);
    image[0] = 'M';
    image[1] = 'Z';
    put_be32(&image[0x10], 0x0000000D); // ExCreateThread's descriptor
    put_be32(&image[0x14], 0x0100000D); // and its thunk
    put_be32(&image[0x18], 0x00000156); // XboxHardwareInfo, a variable
    put_be32(&image[0x300], 0x01000191); // XamInputGetState's thunk, in the second block
    for (int i = 0x20; i < 0x200; i++) image[i] = uint8_t(i);
    for (int i = 0x304; i < 0x400; i++) image[i] = uint8_t(~i);
    return image;
}

std::vector<uint8_t> make_xex(const uint8_t *console_key) {
    std::vector<uint8_t> f(HEADER_SIZE, 0);
    uint8_t *p = f.data();
    std::memcpy(p, "XEX2", 4);
    put_be32(p + 4, 0x00000001); // a title
    put_be32(p + 8, HEADER_SIZE);
    put_be32(p + 0x10, 0x100); // security info
    const uint32_t headers[][2] = {
        {xex_key::FILE_FORMAT_INFO, 0x300},     {xex_key::IMAGE_BASE_ADDRESS, BASE},
        {xex_key::ENTRY_POINT, BASE + 0x100},   {xex_key::EXECUTION_INFO, 0x340},
        {xex_key::IMPORT_LIBRARIES, 0x400},     {xex_key::STATIC_LIBRARIES, 0x600},
        {xex_key::DEFAULT_STACK_SIZE, 0x40000},
    };
    put_be32(p + 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put_be32(p + 0x18 + 8 * i, headers[i][0]);
        put_be32(p + 0x1C + 8 * i, headers[i][1]);
    }

    // security info: the image's size and address, the sealed key, one code page
    uint8_t session[16];
    for (int i = 0; i < 16; i++) session[i] = uint8_t(0xA0 + i);
    uint8_t *s = p + 0x100;
    put_be32(s + 4, 0x10000);
    put_be32(s + 0x110, BASE);
    aes::encrypt_block(aes::expand(console_key), session, s + 0x150);
    put_be32(s + 0x180, 1);
    put_be32(s + 0x184, (1u << 4) | 1);

    // file format: encrypted, basic compression, two blocks
    uint8_t *ffi = p + 0x300;
    put_be32(ffi, 8 + 2 * 8);
    put_be16(ffi + 4, 1);
    put_be16(ffi + 6, 1);
    put_be32(ffi + 8, 0x200);
    put_be32(ffi + 12, 0x100);
    put_be32(ffi + 16, 0x100);
    put_be32(ffi + 20, 0);

    uint8_t *exec = p + 0x340;
    put_be32(exec, 0x11223344);      // media id
    put_be32(exec + 4, 0x10000300);  // version 1.0.3.0
    put_be32(exec + 0xC, 0x475807D7); // title id
    exec[0x12] = 1;
    exec[0x13] = 1;

    // import libraries: the names, then xboxkrnl.exe (two records) and xam.xex (one)
    uint8_t *imp = p + 0x400;
    put_be32(imp + 4, 24);
    put_be32(imp + 8, 2);
    std::memcpy(imp + 12, "xboxkrnl.exe", 13);
    std::memcpy(imp + 12 + 16, "xam.xex", 8);
    uint8_t *lib = imp + 12 + 24;
    put_be32(lib, 0x28 + 3 * 4);
    put_be32(lib + 0x1C, 0x20052A00);
    put_be16(lib + 0x24, 0);
    put_be16(lib + 0x26, 3);
    put_be32(lib + 0x28, BASE + 0x10);
    put_be32(lib + 0x2C, BASE + 0x14);
    put_be32(lib + 0x30, BASE + 0x18);
    lib += 0x28 + 3 * 4;
    put_be32(lib, 0x28 + 4);
    put_be16(lib + 0x24, 1);
    put_be16(lib + 0x26, 1);
    put_be32(lib + 0x28, BASE + 0x300);
    put_be32(imp, uint32_t(lib + 0x28 + 4 - imp));

    uint8_t *stat = p + 0x600;
    put_be32(stat, 4 + 2 * 16);
    std::memcpy(stat + 4, "XAPILIB", 7);
    put_be16(stat + 4 + 8, 2);
    put_be16(stat + 4 + 0xC, 21173);
    stat[4 + 0xE] = 0x40; // approved
    std::memcpy(stat + 20, "D3D9", 4);

    // the payload: the blocks' data back to back, encrypted as one CBC stream
    auto image = plain_image();
    std::vector<uint8_t> payload(image.begin(), image.begin() + 0x200);
    payload.insert(payload.end(), image.begin() + 0x300, image.begin() + 0x400);
    const uint8_t iv[16] = {};
    aes::encrypt_cbc(aes::expand(session), iv, payload.data(), payload.size());
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void reads_an_executable(const uint8_t *console_key, bool devkit) {
    auto file = make_xex(console_key);
    Xex xex;
    std::string error;
    CHECK(parse_xex(file, xex, error));
    CHECK(xex.base_address == BASE && xex.entry_point == BASE + 0x100);
    CHECK(xex.image_size == 0x10000 && xex.page_size() == 0x10000);
    CHECK(xex.pages.size() == 1 && xex.pages[0].type == uint8_t(XexSection::Code) && xex.pages[0].count == 1);
    CHECK(xex.encryption == XexEncryption::Normal && xex.compression == XexCompression::Basic);
    CHECK(xex.stack_size == 0x40000);
    CHECK(xex.has_execution_info && xex.execution.title_id == 0x475807D7 && xex.execution.media_id == 0x11223344);
    CHECK(xex.static_libraries.size() == 2);
    if (xex.static_libraries.size() == 2) {
        CHECK(xex.static_libraries[0].name == "XAPILIB" && xex.static_libraries[0].major == 2 &&
              xex.static_libraries[0].build == 21173 && xex.static_libraries[0].approval() == 2);
        CHECK(xex.static_libraries[1].name == "D3D9");
    }
    CHECK(xex.import_libraries.size() == 2);
    if (xex.import_libraries.size() == 2) {
        CHECK(xex.import_libraries[0].name == "xboxkrnl.exe" && xex.import_libraries[0].records.size() == 3);
        CHECK(xex.import_libraries[1].name == "xam.xex" && xex.import_libraries[1].records.size() == 1);
    }

    CHECK(load_xex_image(file, xex, error));
    CHECK(xex.devkit == devkit);
    CHECK(xex.image == plain_image());

    auto imports = list_imports(xex);
    CHECK(imports.size() == 4);
    if (imports.size() == 4) {
        CHECK(imports[0].library == 0 && imports[0].ordinal == 0x0D && !imports[0].thunk);
        CHECK(imports[1].library == 0 && imports[1].ordinal == 0x0D && imports[1].thunk);
        CHECK(imports[3].library == 1 && imports[3].ordinal == 0x191 && imports[3].thunk);
        const Export *e = find_export("xboxkrnl.exe", imports[0].ordinal);
        CHECK(e && std::strcmp(e->name, "ExCreateThread") == 0 && e->kind == ExportKind::Function);
        e = find_export("XBOXKRNL.EXE", imports[2].ordinal);
        CHECK(e && std::strcmp(e->name, "XboxHardwareInfo") == 0 && e->kind == ExportKind::Variable);
        e = find_export("xam.xex", imports[3].ordinal);
        CHECK(e && std::strcmp(e->name, "XamInputGetState") == 0);
    }
    CHECK(is_system_library("xam.xex") && !is_system_library("game.xexdll"));
}

void refuses_a_short_file() {
    auto file = make_xex(RETAIL_KEY);
    for (size_t size : {size_t(0), size_t(0x10), size_t(0x120), size_t(0x320), size_t(0x420), size_t(HEADER_SIZE + 0x40)}) {
        std::vector<uint8_t> cut(file.begin(), file.begin() + std::min(size, file.size()));
        Xex xex;
        std::string error;
        bool parsed = parse_xex(cut, xex, error);
        if (size < 0x620) CHECK(!parsed);
        if (parsed) CHECK(!load_xex_image(cut, xex, error)); // the blocks run past the end
    }
}

void refuses_a_wrong_key() {
    uint8_t other[16] = {1, 2, 3};
    auto file = make_xex(other);
    Xex xex;
    std::string error;
    CHECK(parse_xex(file, xex, error));
    CHECK(!load_xex_image(file, xex, error));
}

} // namespace

int main() {
    reads_an_executable(RETAIL_KEY, false);
    const uint8_t devkit[16] = {};
    reads_an_executable(devkit, true);
    refuses_a_short_file();
    refuses_a_wrong_key();
    return finish("xex");
}
