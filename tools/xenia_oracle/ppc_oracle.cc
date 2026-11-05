// Xenia's PPC decoder and disassembler as an oracle: for each big-endian word
// of a file (from an offset, for a length), the address, the word, the opcode
// name and Xenia's disassembly, one per line.
//   oracle <file> <base address hex> [offset hex] [length hex]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "xenia/base/string_buffer.h"
#include "xenia/cpu/ppc/ppc_opcode.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 1;
  std::vector<unsigned char> data;
  unsigned char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  uint32_t base = std::strtoul(argv[2], nullptr, 16);
  size_t offset = argc > 3 ? std::strtoul(argv[3], nullptr, 16) : 0;
  size_t length = argc > 4 ? std::strtoul(argv[4], nullptr, 16) : data.size() - offset;
  for (size_t i = offset; i + 4 <= offset + length && i + 4 <= data.size(); i += 4) {
    uint32_t code = uint32_t(data[i]) << 24 | uint32_t(data[i + 1]) << 16 | uint32_t(data[i + 2]) << 8 | data[i + 3];
    uint32_t address = base + uint32_t(i - offset);
    auto op = xe::cpu::ppc::LookupOpcode(code);
    const char* name = op == xe::cpu::ppc::PPCOpcode::kInvalid ? "?" : xe::cpu::ppc::GetOpcodeDisasmInfo(op).name;
    xe::StringBuffer sb;
    xe::cpu::ppc::DisasmPPC(address, code, &sb);
    std::printf("%08X %08X %s|%s\n", address, code, name, sb.buffer());
  }
  return 0;
}
