// The Xenon's instructions (PPC with VMX128): decoded and disassembled
// exactly as Xenia does, from its instruction table (src/ppc/
// ppc-instructions.xml through tools/ppc_gen.py), and the instruction word's
// fields by form as Xenia's emitters read them (its ppc_instr.h,
// PPCOpcodeBits; BSD, see LICENSE-xenia). Plan step A2, docs/recompiler.md.

#pragma once

// the integer types (freestanding in a game library)
#include "xenoforge/ppc_runtime.h"

#ifndef XF_GAME_LIBRARY
#include <string>
#endif

namespace xf::ppc {

enum class Opcode : uint16_t {
#define XF_PPC_OP(name, opcode, form, group, description) name,
#include "xenoforge/ppc_opcodes.inc"
#undef XF_PPC_OP
    Invalid,
};
constexpr uint32_t OPCODE_COUNT = uint32_t(Opcode::Invalid);

enum class Form : uint8_t {
    SC, D, DS, B, I, X, XL, XFX, XFL, XS, XO, A, M, MD, MDS, DCBZ, VX, VC, VA,
    VX128, VX128_1, VX128_2, VX128_3, VX128_4, VX128_5, VX128_R, VX128_P,
};

struct OpcodeInfo {
    const char *name;
    uint32_t opcode;
    Form form;
    char group; // b branch, c control, m memory, i integer, f floating point, v vector
    const char *description;
};

const OpcodeInfo &info(Opcode op);
Opcode decode(uint32_t code);
#ifndef XF_GAME_LIBRARY
// Xenia's DisasmPPC: "DISASM ERROR" for what does not decode.
std::string disassemble(uint32_t address, uint32_t code);
#endif

// The instruction word's fields by form, as Xenia's InstrData (ppc_instr.h):
// bitfields from the low bit, so i.XO.RT is bits 21-25 of the word as the
// manuals count them.
union Instr {
    uint32_t code;
    struct { uint32_t LK : 1, AA : 1, LI : 24, : 6; } I;
    struct { uint32_t LK : 1, AA : 1, BD : 14, BI : 5, BO : 5, : 6; } B;
    struct { uint32_t : 5, LEV : 7, : 20; } SC;
    struct { uint32_t DS : 16, RA : 5, RT : 5, : 6; } D;
    struct { uint32_t : 2, DS : 14, RA : 5, RT : 5, : 6; } DS;
    struct { uint32_t Rc : 1, : 10, RB : 5, RA : 5, RT : 5, : 6; } X;
    struct { uint32_t LK : 1, : 10, BB : 5, BI : 5, BO : 5, : 6; } XL;
    struct { uint32_t : 1, : 10, spr : 10, RT : 5, : 6; } XFX;
    struct { uint32_t Rc : 1, : 10, RB : 5, W : 1, FM : 8, L : 1, : 6; } XFL;
    struct { uint32_t Rc : 1, SH5 : 1, : 9, SH : 5, RA : 5, RT : 5, : 6; } XS;
    struct { uint32_t Rc : 1, : 9, OE : 1, RB : 5, RA : 5, RT : 5, : 6; } XO;
    struct { uint32_t Rc : 1, XO : 5, FRC : 5, FRB : 5, FRA : 5, FRT : 5, : 6; } A;
    struct { uint32_t Rc : 1, ME : 5, MB : 5, SH : 5, RA : 5, RT : 5, : 6; } M;
    struct { uint32_t Rc : 1, SH5 : 1, idx : 3, MB5 : 1, MB : 5, SH : 5, RA : 5, RT : 5, : 6; } MD;
    struct { uint32_t Rc : 1, idx : 4, MB5 : 1, MB : 5, RB : 5, RA : 5, RT : 5, : 6; } MDS;
    struct { uint32_t Rc : 1, idx : 4, MB5 : 1, MB : 5, RB : 5, RA : 5, RT : 5, : 6; } MDSH;
    struct { uint32_t : 6, VC : 5, VB : 5, VA : 5, VD : 5, : 6; } VXA;
    struct { uint32_t : 11, VB : 5, VA : 5, VD : 5, : 6; } VX;
    struct { uint32_t : 10, Rc : 1, VB : 5, VA : 5, VD : 5, : 6; } VXR;
    struct { uint32_t VB128h : 2, VD128h : 2, : 1, VA128h : 1, VC : 3, : 1, VA128H : 1, VB128l : 5, VA128l : 5, VD128l : 5, : 6; } VX128;
    struct { uint32_t : 2, VD128h : 2, : 7, RB : 5, RA : 5, VD128l : 5, : 6; } VX128_1;
    struct { uint32_t VB128h : 2, VD128h : 2, : 1, VA128h : 1, VC : 3, : 1, VA128H : 1, VB128l : 5, VA128l : 5, VD128l : 5, : 6; } VX128_2;
    struct { uint32_t VB128h : 2, VD128h : 2, : 7, VB128l : 5, IMM : 5, VD128l : 5, : 6; } VX128_3;
    struct { uint32_t VB128h : 2, VD128h : 2, : 2, z : 2, : 3, VB128l : 5, IMM : 5, VD128l : 5, : 6; } VX128_4;
    struct { uint32_t VB128h : 2, VD128h : 2, : 1, VA128h : 1, SH : 4, VA128H : 1, VB128l : 5, VA128l : 5, VD128l : 5, : 6; } VX128_5;
    struct { uint32_t VB128h : 2, VD128h : 2, : 2, PERMh : 3, : 2, VB128l : 5, PERMl : 5, VD128l : 5, : 6; } VX128_P;
    struct { uint32_t VB128h : 2, VD128h : 2, : 1, VA128h : 1, Rc : 1, : 3, VA128H : 1, VB128l : 5, VA128l : 5, VD128l : 5, : 6; } VX128_R;
};
static_assert(sizeof(Instr) == 4);

} // namespace xf::ppc
