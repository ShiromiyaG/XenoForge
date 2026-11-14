// The copy-and-patch tier's stencils as tools/xfstencil.cpp writes them out of
// the object file clang made from src/stencil/stencils.cpp, and as
// src/stencil_jit.cpp reads them (docs/recompiler.md, "A copy-and-patch first
// tier"). Nothing here depends on the object's format (COFF, ELF) or the
// host's instruction set: the extractor turns each format's relocations into
// the kinds below, and the patcher writes them.

#pragma once

#include <cstdint>

namespace xf::stencil {

// How a hole's bytes are written, with S the value the hole stands for, A the
// relocation's addend and P the address of the bytes:
enum class RelocKind : uint8_t {
    PcRel32, // x86-64: 32 bits of S + A - P (a rel32 jump, call or lea)
    Abs64,   // 64 bits of S + A
    // ARM64 (patched for ELF; the COFF reader names them too, but Windows on
    // ARM64 has no unwind information read for the copies yet):
    Branch26,  // b, bl: (S + A - P) >> 2 in bits 0-25
    AdrPage21, // adrp: the 4 KB page of S + A less P's, in immlo/immhi
    AddLo12,   // add: the low 12 bits of S + A
    LdSt8Lo12, // ldr/str: the low 12 bits of S + A, scaled by the access's size
    LdSt16Lo12,
    LdSt32Lo12,
    LdSt64Lo12,
    LdSt128Lo12,
    MovwG0, // movz/movk: bits 0-15 of S + A
    MovwG1, // bits 16-31
    MovwG2, // bits 32-47
    MovwG3, // bits 48-63
};

// What a relocation points at.
enum class TargetKind : uint8_t {
    Piece,  // another piece (index), at its start plus the addend
    Hole,   // a hole (Hole), filled per copy
    Extern, // a host symbol (Extern), the same for every copy
};

// The holes stencils.cpp names, filled for each copy of a stencil.
enum class Hole : uint16_t {
    Word,   // xf_hole_word: a 32-bit value (an instruction word)
    A,      // xf_hole_a: a 32-bit value
    B,      // xf_hole_b: a 32-bit value
    Data,   // xf_hole_data: an address of data beside the code
    Next,   // xf_hole_next: the next stencil
    Target, // xf_hole_target: a branch's target
    Call,   // xf_hole_call: a body called
    Count,
};

// The host symbols the stencils may refer to, filled once.
enum class Extern : uint16_t {
    Runtime, // xf::ppc::runtime, the game library's copy of the runtime's table (ppc_runtime.h)
    Memcpy,
    Memmove,
    Memset,
    Count,
};

struct Reloc {
    uint32_t offset; // in the piece
    RelocKind kind;
    TargetKind target;
    uint16_t index; // the piece, Hole or Extern
    int64_t addend;
};

enum PieceFlags : uint8_t {
    PIECE_STENCIL = 1, // a stencil (xf_st_*): copied per use
    PIECE_CODE = 2,    // code (a stencil, or a helper the stencils call: copied once)
    // its last instruction is a jump to xf_hole_next (x86-64: E9 and a rel32;
    // ARM64: a b), left out when the next stencil is placed right after it
    PIECE_TAIL_JUMP = 4,
};

// The size of that last jump.
constexpr uint32_t tail_jump_size(bool arm64) { return arm64 ? 4 : 5; }

// A stencil, or code or read-only data stencils refer to (a helper kept out
// of line, a constant), copied once.
struct Piece {
    const char *name;
    uint32_t offset, size; // in the bytes
    uint32_t align;
    uint32_t flags;
    uint32_t reloc_first, reloc_count;
    // Its unwind information, in the unwind bytes; size 0: none (a leaf,
    // which never holds a return address, so never has a frame to unwind).
    // - Windows x64 (COFF): an UNWIND_INFO with no handler.
    // - ELF: a CIE and an FDE's rules, from .eh_frame: two bytes (little
    //   endian) giving the CIE body's size; the CIE body (what follows its id:
    //   version, augmentation, alignment factors, return address register,
    //   augmentation data, initial instructions) with its pointer encoding
    //   ('R') made DW_EH_PE_absptr, so the run time writes 8-byte addresses
    //   wherever its table lives; then what follows the FDE's address range
    //   (its augmentation data's length and its instructions).
    uint32_t unwind_offset, unwind_size;
};

} // namespace xf::stencil
