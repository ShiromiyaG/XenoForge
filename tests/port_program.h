// port_test's guest program (docs/recompiler.md, "Replacing and hooking"):
// a few functions to hook, as an image with .pdata, and its project file.
// port_gen emits it as xfrecomp would with the project file; port_test runs
// it generated and interpreted with the port's hooks.

#pragma once

#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <cstdint>
#include <vector>

namespace port_program {

constexpr uint32_t BASE = 0x82100000, SIZE = 0x1000;
constexpr uint32_t ADD = BASE + 0x100;     // r3 += 1 (hooked: a slot)
constexpr uint32_t CALLER = BASE + 0x200;  // a frame, bl ADD, return
constexpr uint32_t MID = BASE + 0x300;     // r3 = 0 + 5 + 7 + 100, a mid hook before the +7
constexpr uint32_t MID_AT = MID + 0x4;     // the mid hook
constexpr uint32_t MID_JUMP = MID + 0xC;   // the label the project file gives it (the +100)
constexpr uint32_t TAIL = BASE + 0x400;    // b ADD
constexpr uint32_t PLAIN = BASE + 0x500;   // r3 += 2 (not in the project file)
constexpr uint32_t CALLER2 = BASE + 0x600; // a frame, bl PLAIN, return
constexpr uint32_t STATIC = BASE + 0x700;  // r3 += 3 (hooked at link time)

inline uint32_t branch(uint32_t from, uint32_t to, bool link) {
    return 0x48000000u | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}

inline void put(std::vector<uint8_t> &image, uint32_t address, std::vector<uint32_t> words) {
    for (uint32_t w : words) {
        uint32_t at = address - BASE;
        image[at] = uint8_t(w >> 24), image[at + 1] = uint8_t(w >> 16), image[at + 2] = uint8_t(w >> 8),
        image[at + 3] = uint8_t(w);
        address += 4;
    }
}

// a frame around one call (as the compiler writes it)
inline std::vector<uint32_t> frame_call(uint32_t at, uint32_t target) {
    return {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, branch(at + 12, target, true),
            0x38210060, 0x8181FFF8, 0x7D8803A6, 0x4E800020};
}

inline std::vector<uint8_t> image_bytes() {
    std::vector<uint8_t> image(SIZE, 0);
    put(image, ADD, {0x38630001, 0x4E800020});
    put(image, CALLER, frame_call(CALLER, ADD));
    put(image, MID, {0x38800000, 0x38840005, 0x38840007, 0x38840064, 0x7C832378, 0x4E800020});
    put(image, TAIL, {branch(TAIL, ADD, false)});
    put(image, PLAIN, {0x38630002, 0x4E800020});
    put(image, CALLER2, frame_call(CALLER2, PLAIN));
    put(image, STATIC, {0x38630003, 0x4E800020});
    return image;
}

inline xf::recomp::Image image() {
    xf::recomp::Image i;
    i.base = BASE;
    i.bytes = image_bytes();
    i.sections.push_back({".text", BASE, SIZE, true, false});
    i.pdata = {{ADD, 8, 0, false},   {CALLER, 32, 3, false},  {MID, 24, 0, false},  {TAIL, 4, 0, false},
               {PLAIN, 8, 0, false}, {CALLER2, 32, 3, false}, {STATIC, 8, 0, false}};
    return i;
}

// (top-level keys before the first table, as TOML has it)
constexpr const char *PROJECT = R"(# port_test's project file
hooks = [0x82100100, 0x82100700]

[project]
name = "port_test"
module = "port_test.xex"

[names]
"0x82100100" = "add_one"
"0x82100200" = "caller"

[[mid_hooks]]
address = 0x82100304
jumps = [0x8210030C]
name = "mid_hook"
)";

} // namespace port_program
