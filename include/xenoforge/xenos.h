// The Xenos GPU's register indices, PM4 opcodes and the names of its formats,
// primitives and microcode opcodes. Numbers and names from Xenia's
// src/xenia/gpu/{register_table.inc, xenos.h, ucode.h} (BSD; see
// LICENSE-xenia).

#pragma once

#include <cstdint>

namespace xf::xenos {

constexpr uint32_t REGISTER_COUNT = 0x5003;

namespace reg {
// the command processor's and the display's (gpu/command_processor.h)
constexpr uint32_t CP_RB_WPTR = 0x01C5;
constexpr uint32_t SCRATCH_UMSK = 0x01DC;
constexpr uint32_t SCRATCH_ADDR = 0x01DD;
constexpr uint32_t SCRATCH_REG0 = 0x0578; // to SCRATCH_REG7, 0x057F
constexpr uint32_t WRITEBACK_START = 0x0A04;
constexpr uint32_t WRITEBACK_SIZE = 0x0A05;
constexpr uint32_t COHER_SIZE_HOST = 0x0A2F;
constexpr uint32_t COHER_BASE_HOST = 0x0A30;
constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
constexpr uint32_t RB_EDRAM_TIMING = 0x0F00;
constexpr uint32_t RB_BC_CONTROL = 0x0F01;
constexpr uint32_t RB_HSIO_INTERFACE_ALIGNER_VALUE = 0x0F2D;
constexpr uint32_t D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x1844;
constexpr uint32_t DC_LUT_RW_MODE = 0x1921;
constexpr uint32_t DC_LUT_RW_INDEX = 0x1922;
constexpr uint32_t DC_LUT_SEQ_COLOR = 0x1923;
constexpr uint32_t DC_LUT_PWL_DATA = 0x1924;
constexpr uint32_t DC_LUT_30_COLOR = 0x1925;
constexpr uint32_t DC_LUT_WRITE_EN_MASK = 0x1927;
constexpr uint32_t D1MODE_V_COUNTER = 0x194C;
constexpr uint32_t D1MODE_VBLANK_VLINE_STATUS = 0x1951;
constexpr uint32_t D1MODE_VIEWPORT_SIZE = 0x1961; // AVIVO's
constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
constexpr uint32_t PA_SC_VIZ_QUERY = 0x2293;
constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;
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
constexpr uint32_t VIZ_QUERY = 0x23;
constexpr uint32_t WAIT_FOR_IDLE = 0x26;
constexpr uint32_t IM_LOAD = 0x27;
constexpr uint32_t IM_LOAD_IMMEDIATE = 0x2B;
constexpr uint32_t SET_CONSTANT = 0x2D;
constexpr uint32_t LOAD_ALU_CONSTANT = 0x2F;
constexpr uint32_t DRAW_INDX_2 = 0x36;
constexpr uint32_t INDIRECT_BUFFER_PFD = 0x37;
constexpr uint32_t INVALIDATE_STATE = 0x3B;
constexpr uint32_t WAIT_REG_MEM = 0x3C;
constexpr uint32_t MEM_WRITE = 0x3D;
constexpr uint32_t REG_TO_MEM = 0x3E;
constexpr uint32_t INDIRECT_BUFFER = 0x3F;
constexpr uint32_t COND_WRITE = 0x45;
constexpr uint32_t EVENT_WRITE = 0x46;
constexpr uint32_t ME_INIT = 0x48;
constexpr uint32_t SET_BIN_MASK = 0x50;
constexpr uint32_t SET_BIN_SELECT = 0x51;
constexpr uint32_t INTERRUPT = 0x54;
constexpr uint32_t SET_CONSTANT2 = 0x55;
constexpr uint32_t SET_SHADER_CONSTANTS = 0x56;
constexpr uint32_t EVENT_WRITE_SHD = 0x58;
constexpr uint32_t EVENT_WRITE_EXT = 0x5A;
constexpr uint32_t EVENT_WRITE_ZPD = 0x5B;
constexpr uint32_t WAIT_IB_PFD_COMPLETE = 0x5D;
constexpr uint32_t CONTEXT_UPDATE = 0x5E;
constexpr uint32_t SET_BIN_MASK_LO = 0x60;
constexpr uint32_t SET_BIN_MASK_HI = 0x61;
constexpr uint32_t SET_BIN_SELECT_LO = 0x62;
constexpr uint32_t SET_BIN_SELECT_HI = 0x63;
constexpr uint32_t XE_SWAP = 0x64; // Xenia's own: VdSwap's packet

// the packets' headers (Xenia's MakePacketType*)
constexpr uint32_t type0(uint32_t index, uint32_t count, bool one_register = false) {
    return ((count - 1) & 0x3FFF) << 16 | (one_register ? 0x8000u : 0u) | (index & 0x7FFF);
}
constexpr uint32_t type1(uint32_t index_1, uint32_t index_2) {
    return 1u << 30 | (index_2 & 0x7FF) << 11 | (index_1 & 0x7FF);
}
constexpr uint32_t type2() { return 2u << 30; }
constexpr uint32_t type3(uint32_t opcode, uint32_t count, bool predicated = false) {
    return 3u << 30 | ((count - 1) & 0x3FFF) << 16 | (opcode & 0x7F) << 8 | (predicated ? 1u : 0u);
}
// XE_SWAP's first word, 'SWAP'
constexpr uint32_t SWAP_SIGNATURE = 0x53574150;
} // namespace pm4

// VGT_EVENT_INITIATOR's events that the command processor writes itself
constexpr uint32_t EVENT_VIZQUERY_START = 7, EVENT_VIZQUERY_END = 8;
constexpr uint32_t EVENT_SCREEN_EXT_INIT = 25, EVENT_SCREEN_EXT_RPT = 26;

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
