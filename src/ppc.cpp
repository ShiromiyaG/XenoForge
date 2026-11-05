// The Xenon's instructions decoded and disassembled (ppc.h). The decoder's
// body and the disassembly functions are written by tools/ppc_gen.py from
// Xenia's instruction table; DisasmPPC, PadStringBuffer and the hand-written
// PrintDisasm_bcx are translated from Xenia's ppc_opcode_info.cc and
// ppc_opcode_disasm.cc (BSD; see LICENSE-xenia).

#include "xenoforge/ppc.h"

#include "ppc/decode_data.h"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace xf::ppc {
namespace {

constexpr OpcodeInfo INFO[] = {
#define XF_PPC_OP(name, opcode, form, group, description) {#name, opcode, Form::form, (#group)[0], description},
#include "xenoforge/ppc_opcodes.inc"
#undef XF_PPC_OP
};
static_assert(sizeof(INFO) / sizeof(INFO[0]) == OPCODE_COUNT);

constexpr uint32_t bits(uint32_t v, uint32_t a, uint32_t b) { return (v >> (32 - 1 - b)) & ((1 << (b - a + 1)) - 1); }

using Data = fields::PPCDecodeData;
using DisasmFn = void (*)(const Data &, std::string &);

// the name padded to 11 columns (kNamePad)
void pad(std::string &out, size_t start) {
    size_t added = out.size() - start;
    if (added < 11) out.append(11 - added, ' ');
}
// fmt's "{}": booleans as words
template <typename T> void dec(std::string &out, T value) {
    if constexpr (std::is_same_v<T, bool>) out += value ? "true" : "false";
    else out += std::to_string(value);
}
// fmt's "0x{:X}"
template <typename T> void hex(std::string &out, T value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", (unsigned long long)std::make_unsigned_t<T>(value));
    out += buffer;
}
// "-0x{:X}" or "0x{:X}" of the magnitude
void signed_hex(std::string &out, int32_t value) {
    if (value < 0) out += '-';
    hex(out, uint32_t(std::abs(value)));
}

// [bc][LK][AA][+-] [BO], [BI], [ADDR], with the condition's mnemonics
void disasm_bcx(const Data &d, std::string &out) {
    size_t start = out.size();
    const uint32_t bo = d.B.BO(), bi = d.B.BI(), addr = d.B.ADDR();
    const bool bo0 = bo & 0x10, bo1 = bo & 0x08, bo2 = bo & 0x04, bo3 = bo & 0x02, bo4 = bo & 0x01;
    int sign = 0;
    auto condition = [&](const char *lt, const char *gt, const char *eq, int s) {
        switch (bi % 4) {
        case 0: out += lt, sign = s; break;
        case 1: out += gt, sign = s; break;
        case 2: out += eq, sign = s; break;
        }
    };
    if (bo0 && !bo1 && !bo2 && bo3 && !bo4) out += "bdz";
    else if (bo0 && bo1 && !bo2 && bo3 && !bo4) out += "bdz", sign = -1;
    else if (bo0 && bo1 && !bo2 && bo3 && bo4) out += "bdz", sign = +1;
    else if (bo0 && !bo1 && !bo2 && !bo3 && !bo4) out += "bdnz";
    else if (bo0 && bo1 && !bo2 && !bo3 && !bo4) out += "bdnz", sign = -1;
    else if (bo0 && bo1 && !bo2 && !bo3 && bo4) out += "bdnz", sign = +1;
    else if (!bo0 && !bo1 && bo2 && !bo3 && !bo4) condition("bge", "ble", "bne", 0);
    else if (!bo0 && !bo1 && bo2 && bo3 && !bo4) condition("bge", "ble", "bne", -1);
    else if (!bo0 && !bo1 && bo2 && bo3 && bo4) condition("bge", "ble", "bne", +1);
    else if (!bo0 && bo1 && bo2 && !bo3 && !bo4) condition("blt", "bgt", "beq", 0);
    else if (!bo0 && bo1 && bo2 && bo3 && !bo4) condition("blt", "bgt", "beq", -1);
    else if (!bo0 && bo1 && bo2 && bo3 && bo4) condition("blt", "bgt", "beq", +1);

    if (start == out.size()) {
        out += "bc";
        if (d.B.LK()) out += 'l';
        if (d.B.AA()) out += 'a';
        pad(out, start);
        dec(out, bo);
        out += ", ";
        dec(out, bi);
    } else {
        if (d.B.LK()) out += 'l';
        if (d.B.AA()) out += 'a';
        if (sign > 0) out += '+';
        else if (sign < 0) out += '-';
        pad(out, start);
        out += "crf" + std::to_string(bi / 4);
    }
    out += ", ";
    hex(out, addr);
}

#include "ppc/disasm.inc"
static_assert(sizeof(DISASM) / sizeof(DISASM[0]) == OPCODE_COUNT);

} // namespace

const OpcodeInfo &info(Opcode op) { return INFO[size_t(op)]; }

Opcode decode(uint32_t code) {
#define HIT(name) return Opcode::name
#define MISS return Opcode::Invalid
#include "ppc/decode.inc"
#undef HIT
#undef MISS
}

std::string disassemble(uint32_t address, uint32_t code) {
    Opcode op = decode(code);
    if (op == Opcode::Invalid) return "DISASM ERROR";
    std::string out;
    if (DisasmFn fn = DISASM[size_t(op)]) {
        Data d;
        d.address = address;
        d.code = code;
        fn(d, out);
    } else {
        // "{:<8}"
        out = INFO[size_t(op)].name;
        if (out.size() < 8) out.append(8 - out.size(), ' ');
    }
    return out;
}

} // namespace xf::ppc
