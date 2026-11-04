// Names of the Xenos GPU's values, from Xenia's src/xenia/gpu/xenos.h and
// ucode.h (BSD; see LICENSE-xenia). A value with no name prints as "?".

#include "xenoforge/xenos.h"

#include <iterator>

namespace xf::xenos {
namespace {

template <size_t N> const char *lookup(const char *const (&names)[N], uint32_t value) {
    return value < N && names[value] ? names[value] : "?";
}

} // namespace

const char *pm4_opcode_name(uint32_t opcode) {
    switch (opcode) {
    case 0x10: return "NOP";
    case 0x21: return "REG_RMW";
    case 0x22: return "DRAW_INDX";
    case 0x23: return "VIZ_QUERY";
    case 0x25: return "SET_STATE";
    case 0x26: return "WAIT_FOR_IDLE";
    case 0x27: return "IM_LOAD";
    case 0x2B: return "IM_LOAD_IMMEDIATE";
    case 0x2C: return "IM_STORE";
    case 0x2D: return "SET_CONSTANT";
    case 0x2E: return "LOAD_CONSTANT_CONTEXT";
    case 0x2F: return "LOAD_ALU_CONSTANT";
    case 0x34: return "DRAW_INDX_BIN";
    case 0x35: return "DRAW_INDX_2_BIN";
    case 0x36: return "DRAW_INDX_2";
    case 0x37: return "INDIRECT_BUFFER_PFD";
    case 0x3B: return "INVALIDATE_STATE";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3D: return "MEM_WRITE";
    case 0x3E: return "REG_TO_MEM";
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x44: return "COND_EXEC";
    case 0x45: return "COND_WRITE";
    case 0x46: return "EVENT_WRITE";
    case 0x48: return "ME_INIT";
    case 0x4A: return "SET_SHADER_BASES";
    case 0x4B: return "SET_BIN_BASE_OFFSET";
    case 0x4F: return "MEM_WRITE_CNTR";
    case 0x50: return "SET_BIN_MASK";
    case 0x51: return "SET_BIN_SELECT";
    case 0x52: return "WAIT_REG_EQ";
    case 0x53: return "WAIT_REG_GTE";
    case 0x54: return "INTERRUPT";
    case 0x55: return "SET_CONSTANT2";
    case 0x56: return "SET_SHADER_CONSTANTS";
    case 0x58: return "EVENT_WRITE_SHD";
    case 0x59: return "EVENT_WRITE_CFL";
    case 0x5A: return "EVENT_WRITE_EXT";
    case 0x5B: return "EVENT_WRITE_ZPD";
    case 0x5C: return "WAIT_UNTIL_READ";
    case 0x5D: return "WAIT_IB_PFD_COMPLETE";
    case 0x5E: return "CONTEXT_UPDATE";
    case 0x60: return "SET_BIN_MASK_LO";
    case 0x61: return "SET_BIN_MASK_HI";
    case 0x62: return "SET_BIN_SELECT_LO";
    case 0x63: return "SET_BIN_SELECT_HI";
    case 0x64: return "XE_SWAP";
    }
    return "?";
}

const char *primitive_name(uint32_t type) {
    static const char *const names[] = {
        "none",           "point list",       "line list",         "line strip",        "triangle list",
        "triangle fan",   "triangle strip",   "triangle w/ flags", "rectangle list",    nullptr,
        nullptr,          nullptr,            "line loop",         "quad list",         "quad strip",
        "polygon",        "2D copy rect v0",  "2D copy rect v1",   "2D copy rect v2",   "2D copy rect v3",
        "2D fill rect",   "2D line strip",    "2D triangle strip",
    };
    return lookup(names, type);
}

const char *edram_mode_name(uint32_t mode) {
    switch (EdramMode(mode)) {
    case EdramMode::NoOperation: return "no operation";
    case EdramMode::ColorDepth: return "color and depth";
    case EdramMode::DepthOnly: return "depth only";
    case EdramMode::Copy: return "copy (resolve)";
    }
    return "?";
}

const char *color_target_format_name(uint32_t format) {
    static const char *const names[] = {
        "8_8_8_8", "8_8_8_8_GAMMA", "2_10_10_10", "2_10_10_10_FLOAT", "16_16", "16_16_16_16", "16_16_FLOAT",
        "16_16_16_16_FLOAT", nullptr, nullptr, "2_10_10_10_AS_10_10_10_10", nullptr,
        "2_10_10_10_FLOAT_AS_16_16_16_16", nullptr, "32_FLOAT", "32_32_FLOAT",
    };
    return lookup(names, format);
}

const char *depth_target_format_name(uint32_t format) { return format == 0 ? "D24S8" : format == 1 ? "D24FS8" : "?"; }

const char *format_name(uint32_t format) {
    static const char *const names[] = {
        "1_REVERSE", "1", "8", "1_5_5_5", "5_6_5", "6_5_5", "8_8_8_8", "2_10_10_10", "8_A", "8_B", "8_8",
        "Cr_Y1_Cb_Y0_REP", "Y1_Cr_Y0_Cb_REP", "16_16_EDRAM", "8_8_8_8_A", "4_4_4_4", "10_11_11", "11_11_10",
        "DXT1", "DXT2_3", "DXT4_5", "16_16_16_16_EDRAM", "24_8", "24_8_FLOAT", "16", "16_16", "16_16_16_16",
        "16_EXPAND", "16_16_EXPAND", "16_16_16_16_EXPAND", "16_FLOAT", "16_16_FLOAT", "16_16_16_16_FLOAT", "32",
        "32_32", "32_32_32_32", "32_FLOAT", "32_32_FLOAT", "32_32_32_32_FLOAT", "32_AS_8", "32_AS_8_8",
        "16_MPEG", "16_16_MPEG", "8_INTERLACED", "32_AS_8_INTERLACED", "32_AS_8_8_INTERLACED", "16_INTERLACED",
        "16_MPEG_INTERLACED", "16_16_MPEG_INTERLACED", "DXN", "8_8_8_8_AS_16_16_16_16", "DXT1_AS_16_16_16_16",
        "DXT2_3_AS_16_16_16_16", "DXT4_5_AS_16_16_16_16", "2_10_10_10_AS_16_16_16_16",
        "10_11_11_AS_16_16_16_16", "11_11_10_AS_16_16_16_16", "32_32_32_FLOAT", "DXT3A", "DXT5A", "CTX1",
        "DXT3A_AS_1_1_1_1", "8_8_8_8_GAMMA_EDRAM", "2_10_10_10_FLOAT_EDRAM",
    };
    return lookup(names, format);
}

const char *dimension_name(uint32_t dimension) {
    static const char *const names[] = {"1D", "2D", "3D or stacked", "cube"};
    return lookup(names, dimension);
}

namespace ucode {

const char *cf_opcode_name(uint32_t opcode) {
    static const char *const names[] = {
        "nop",       "exec",           "exec_end",    "cond_exec", "cond_exec_end", "cond_exec_pred",
        "cond_exec_pred_end", "loop_start", "loop_end", "cond_call", "return",   "cond_jmp",
        "alloc",     "cond_exec_pred_clean", "cond_exec_pred_clean_end", "mark_vs_fetch_done",
    };
    return lookup(names, opcode);
}

const char *vector_opcode_name(uint32_t opcode) {
    static const char *const names[] = {
        "add",  "mul",  "max",   "min",    "seq",  "sgt",  "sge",    "sne",         "frc",
        "trunc", "floor", "mad", "cndeq",  "cndge", "cndgt", "dp4",  "dp3",         "dp2add",
        "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",
        "kill_gt", "kill_ge", "kill_ne", "dst", "maxa",
    };
    return lookup(names, opcode);
}

const char *scalar_opcode_name(uint32_t opcode) {
    static const char *const names[] = {
        "adds",     "adds_prev", "muls",     "muls_prev", "muls_prev2", "maxs",     "mins",     "seqs",
        "sgts",     "sges",      "snes",     "frcs",      "truncs",     "floors",   "exp",      "logc",
        "log",      "rcpc",      "rcpf",     "rcp",       "rsqc",       "rsqf",     "rsq",      "maxas",
        "maxasf",   "subs",      "subs_prev", "setp_eq",  "setp_ne",    "setp_gt",  "setp_ge",  "setp_inv",
        "setp_pop", "setp_clr",  "setp_rstr", "kills_eq", "kills_gt",   "kills_ge", "kills_ne", "kills_one",
        "sqrt",     nullptr,     "mulsc0",   "mulsc1",    "addsc0",     "addsc1",   "subsc0",   "subsc1",
        "sin",      "cos",       "retain_prev",
    };
    return lookup(names, opcode);
}

const char *fetch_opcode_name(uint32_t opcode) {
    switch (opcode) {
    case 0: return "vfetch";
    case 1: return "tfetch";
    case 16: return "get_border_color_frac";
    case 17: return "get_computed_lod";
    case 18: return "get_gradients";
    case 19: return "get_weights";
    case 24: return "set_lod";
    case 25: return "set_gradients_h";
    case 26: return "set_gradients_v";
    }
    return "?";
}

} // namespace ucode
} // namespace xf::xenos
