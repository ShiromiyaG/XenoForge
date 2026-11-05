// The Xenos GPU's register indices, PM4 opcodes and the names of its formats,
// primitives and microcode opcodes. Numbers and names from Xenia's
// src/xenia/gpu/{register_table.inc, xenos.h, ucode.h} (BSD; see
// LICENSE-xenia).

#pragma once

#include <cstdint>

namespace xf::xenos {

constexpr uint32_t REGISTER_COUNT = 0x5003;

namespace reg {
constexpr uint32_t RB_SURFACE_INFO = 0x2000;
constexpr uint32_t RB_COLOR_INFO = 0x2001;
constexpr uint32_t RB_DEPTH_INFO = 0x2002;
constexpr uint32_t RB_COLOR1_INFO = 0x2003;
constexpr uint32_t RB_COLOR2_INFO = 0x2004;
constexpr uint32_t RB_COLOR3_INFO = 0x2005;
constexpr uint32_t RB_COLOR_MASK = 0x2104;
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
constexpr uint32_t VGT_DMA_BASE = 0x21FA;
constexpr uint32_t VGT_DMA_SIZE = 0x21FB;
constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
constexpr uint32_t RB_MODECONTROL = 0x2208;
constexpr uint32_t RB_COPY_CONTROL = 0x2318;
constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;
constexpr uint32_t RB_COPY_SURFACE_SLICE = 0x2323;
// the resolve rectangle's: scissors, window offset, pixel center
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
// SET_CONSTANT's kinds, each a block of registers
constexpr uint32_t ALU_CONSTANTS = 0x4000;   // 512 float4, vertex then pixel
constexpr uint32_t FETCH_CONSTANTS = 0x4800; // 32 textures of 6 dwords (96 vertex fetches of 2)
constexpr uint32_t BOOL_CONSTANTS = 0x4900;
constexpr uint32_t LOOP_CONSTANTS = 0x4908;
} // namespace reg

namespace pm4 {
constexpr uint32_t NOP = 0x10;
constexpr uint32_t REG_RMW = 0x21;
constexpr uint32_t DRAW_INDX = 0x22;
constexpr uint32_t IM_LOAD = 0x27;
constexpr uint32_t IM_LOAD_IMMEDIATE = 0x2B;
constexpr uint32_t SET_CONSTANT = 0x2D;
constexpr uint32_t LOAD_ALU_CONSTANT = 0x2F;
constexpr uint32_t DRAW_INDX_2 = 0x36;
constexpr uint32_t INDIRECT_BUFFER_PFD = 0x37;
constexpr uint32_t INDIRECT_BUFFER = 0x3F;
constexpr uint32_t SET_CONSTANT2 = 0x55;
constexpr uint32_t SET_SHADER_CONSTANTS = 0x56;
constexpr uint32_t XE_SWAP = 0x64; // Xenia's own: VdSwap's packet
} // namespace pm4

enum class EdramMode : uint32_t { NoOperation = 0, ColorDepth = 4, DepthOnly = 5, Copy = 6 };
enum class SourceSelect : uint32_t { DMA = 0, Immediate = 1, AutoIndex = 2 };
enum class FetchConstantType : uint32_t { InvalidTexture = 0, InvalidVertex = 1, Texture = 2, Vertex = 3 };

const char *pm4_opcode_name(uint32_t opcode);
const char *primitive_name(uint32_t type);
const char *edram_mode_name(uint32_t mode);
const char *color_target_format_name(uint32_t format);
const char *depth_target_format_name(uint32_t format);
// Texture, resolve destination and vertex formats share one numbering.
const char *format_name(uint32_t format);
const char *dimension_name(uint32_t dimension); // the fetch constant's and tfetch's

namespace ucode {
const char *cf_opcode_name(uint32_t opcode);
const char *vector_opcode_name(uint32_t opcode);
const char *scalar_opcode_name(uint32_t opcode);
const char *fetch_opcode_name(uint32_t opcode);
} // namespace ucode

} // namespace xf::xenos
