// xfppc: PPC words decoded and disassembled, one per line, as the oracle
// built from Xenia's decoder prints them (docs/recompiler.md, "Checks"):
// address, word, opcode name|disassembly.
//
//   xfppc <file> <base address hex> [offset hex] [length hex]
//   xfppc --random <count> <seed>      random words, at address 0x82000000

#include "xenoforge/ppc.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {
void line(uint32_t address, uint32_t code) {
    xf::ppc::Opcode op = xf::ppc::decode(code);
    const char *name = op == xf::ppc::Opcode::Invalid ? "?" : xf::ppc::info(op).name;
    std::printf("%08X %08X %s|%s\n", address, code, name, xf::ppc::disassemble(address, code).c_str());
}
} // namespace

int main(int argc, char **argv) {
    if (argc >= 4 && std::string(argv[1]) == "--random") {
        std::mt19937 random(uint32_t(std::strtoul(argv[3], nullptr, 10)));
        unsigned long count = std::strtoul(argv[2], nullptr, 10);
        for (unsigned long i = 0; i < count; i++) line(0x82000000u + uint32_t(i * 4), random());
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: xfppc <file> <base address hex> [offset hex] [length hex]\n"
                             "       xfppc --random <count> <seed>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 1;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint32_t base = uint32_t(std::strtoul(argv[2], nullptr, 16));
    size_t offset = argc > 3 ? std::strtoul(argv[3], nullptr, 16) : 0;
    size_t length = argc > 4 ? std::strtoul(argv[4], nullptr, 16) : data.size() - offset;
    for (size_t i = offset; i + 4 <= offset + length && i + 4 <= data.size(); i += 4) {
        uint32_t code = uint32_t(data[i]) << 24 | uint32_t(data[i + 1]) << 16 | uint32_t(data[i + 2]) << 8 | data[i + 3];
        line(base + uint32_t(i - offset), code);
    }
    return 0;
}
