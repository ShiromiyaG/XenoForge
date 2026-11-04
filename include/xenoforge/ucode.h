// What a Xenos shader's microcode uses: its control flow, the ALU's vector
// and scalar operations, the fetches and the constants they read, memexport.
// The first step of the translator (plan B2); the layouts are Xenia's
// src/xenia/gpu/ucode.h (BSD; see LICENSE-xenia).

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>

namespace xf::ucode {

struct ShaderSummary {
    uint32_t dwords = 0;
    uint32_t alu = 0, fetch = 0; // instructions reached from the control flow
    std::map<uint32_t, uint32_t> cf_ops, vector_ops, scalar_ops, fetch_ops;
    std::map<uint32_t, uint32_t> vertex_formats;  // per vfetch
    std::map<uint32_t, uint32_t> texture_dims;    // per tfetch
    std::set<uint32_t> vertex_slots;              // vertex fetch constants, 0-95
    std::set<uint32_t> texture_slots;             // texture fetch constants, 0-31
    bool memexport = false;                       // an alloc of memory exports
    bool kills = false;
    bool truncated = false;                       // an exec reaches past the end
};

// dwords in host order (the guest's big-endian words already swapped)
ShaderSummary analyze(std::span<const uint32_t> dwords);

} // namespace xf::ucode
