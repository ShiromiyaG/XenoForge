// A made-up executable for the SDK's test (tests/sdk_test.cmake): an XEX2
// file neither encrypted nor compressed, a title, its image a PE with one code
// section, as tests/game_build_test.cpp makes its own. A port is built from it
// as from a disc (xenoforge_add_port's ISO form: xfrecomp emit, the generated
// code compiled), so the step a real disc takes is checked without one.
//
//   make_xex <out.xex>
//
// The code: the entry calls three leaves (each adds to r3) and returns.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <vector>

namespace {

constexpr uint32_t BASE = 0x82000000, IMAGE_SIZE = 0x2000;
constexpr uint32_t E = BASE + 0x1000;      // the entry
constexpr uint32_t LEAVES = BASE + 0x1100; // leaf k at LEAVES + 0x10 * k: addi r3, r3, k + 1; blr
constexpr uint32_t ENTRY_POINT = 0x00010100, IMAGE_BASE_ADDRESS = 0x00010201, FILE_FORMAT_INFO = 0x000003FF,
                   EXECUTION_INFO = 0x00040006; // the optional headers' keys (xex.h's xex_key)

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    v[at] = uint8_t(x >> 24), v[at + 1] = uint8_t(x >> 16), v[at + 2] = uint8_t(x >> 8), v[at + 3] = uint8_t(x);
}
void put_le16(std::vector<uint8_t> &v, size_t at, uint16_t x) { v[at] = uint8_t(x), v[at + 1] = uint8_t(x >> 8); }
void put_le32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    for (int k = 0; k < 4; k++) v[at + k] = uint8_t(x >> (8 * k));
}
uint32_t addi(uint32_t d, uint32_t a, int16_t v) { return 14u << 26 | d << 21 | a << 16 | uint16_t(v); }
uint32_t bl(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC) | 1; }
constexpr uint32_t BLR = 0x4E800020, MFLR_R12 = 0x7D8802A6, MTLR_R12 = 0x7D8803A6;

std::vector<uint8_t> make_image() {
    std::vector<uint8_t> image(IMAGE_SIZE, 0);
    image[0] = 'M', image[1] = 'Z';
    put_le32(image, 0x3C, 0x80);
    std::memcpy(&image[0x80], "PE\0\0", 4);
    put_le16(image, 0x80 + 6, 1);     // sections
    put_le16(image, 0x80 + 20, 0xE0); // the optional header's size
    const uint32_t optional = 0x80 + 24;
    put_le16(image, optional, 0x10B);
    put_le32(image, optional + 92, 16); // data directories
    const uint32_t section = optional + 0xE0;
    std::memcpy(&image[section], ".text", 5);
    put_le32(image, section + 8, 0x1000);
    put_le32(image, section + 12, 0x1000);
    put_le32(image, section + 36, 0x60000020);
    auto code = [&](uint32_t at, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) put32(image, at - BASE, w), at += 4;
    };
    code(E, {MFLR_R12, addi(3, 0, 0)});
    for (uint32_t k = 0; k < 3; k++) code(E + 8 + 4 * k, {bl(E + 8 + 4 * k, LEAVES + 0x10 * k)});
    code(E + 8 + 12, {MTLR_R12, BLR});
    for (uint32_t k = 0; k < 3; k++) code(LEAVES + 0x10 * k, {addi(3, 3, int16_t(k + 1)), BLR});
    return image;
}

std::vector<uint8_t> make_xex() {
    std::vector<uint8_t> f(0x1000, 0);
    std::memcpy(f.data(), "XEX2", 4);
    put32(f, 0x04, 0x1);
    put32(f, 0x08, 0x1000);
    put32(f, 0x10, 0x400);
    const std::pair<uint32_t, uint32_t> headers[] = {
        {ENTRY_POINT, E}, {IMAGE_BASE_ADDRESS, BASE}, {FILE_FORMAT_INFO, 0x600}, {EXECUTION_INFO, 0x700}};
    put32(f, 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put32(f, 0x18 + 8 * i, headers[i].first);
        put32(f, 0x1C + 8 * i, headers[i].second);
    }
    put32(f, 0x400 + 0x004, IMAGE_SIZE);
    put32(f, 0x400 + 0x110, BASE);
    put32(f, 0x600, 8);                  // no encryption, no compression
    put32(f, 0x700 + 0xC, 0x58460002);   // the execution info: a made-up title ID
    put32(f, 0x400 + 0x10C, 0x10000000); // 4 KB pages...
    put32(f, 0x400 + 0x180, 1);          // ...in one run of two, all code
    put32(f, 0x400 + 0x184, (2u << 4) | 1u);
    std::vector<uint8_t> image = make_image();
    f.insert(f.end(), image.begin(), image.end());
    return f;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: make_xex <out.xex>\n"), 2;
    const std::vector<uint8_t> file = make_xex();
    std::ofstream out(argv[1], std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return out ? 0 : 1;
}
