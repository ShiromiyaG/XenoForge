// The index conversion (index_convert.h): the vector path against the
// portable loop, and the portable loop against the rule written out here,
// over random indices with every width, endianness, restart and remap.

#include "check.h"
#include "xenoforge/index_convert.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace xf::render;

namespace {

uint32_t rule(const std::vector<uint8_t> &bytes, uint32_t i, const IndexConversion &c) {
    uint32_t v;
    if (c.index32) {
        const uint8_t *p = &bytes[size_t(i) * 4];
        uint32_t le = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        uint32_t be = uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
        uint32_t pairs = uint32_t(p[1]) | uint32_t(p[0]) << 8 | uint32_t(p[3]) << 16 | uint32_t(p[2]) << 24;
        v = c.endian == 0 ? le : c.endian == 2 ? be : c.endian == 1 ? pairs : (le >> 16 | le << 16);
    } else {
        const uint8_t *p = &bytes[size_t(i) * 2];
        v = c.endian == 1 ? uint32_t(p[0]) << 8 | p[1] : uint32_t(p[0]) | uint32_t(p[1]) << 8;
    }
    if (c.restart && v == c.reset) return UINT32_MAX;
    if (!c.remap) return v;
    return std::min(c.max, std::max(c.min, (v + c.offset) & 0xFFFFFF));
}

} // namespace

int main() {
    uint32_t x = 12345;
    auto next = [&] {
        x = x * 1664525u + 1013904223u;
        return x >> 8;
    };
    int cases = 0;
    for (int index32 = 0; index32 < 2; index32++)
        for (uint32_t endian = 0; endian < 4; endian++)
            for (int restart = 0; restart < 2; restart++)
                for (int remap = 0; remap < 2; remap++)
                    for (uint32_t count : {0u, 1u, 7u, 8u, 9u, 61u, 1000u}) {
                        IndexConversion c;
                        c.index32 = index32, c.endian = endian, c.restart = restart, c.remap = remap;
                        c.offset = next() & 0xFFFFFF, c.min = next() & 0xFFF, c.max = c.min + (next() & 0xFFFFF);
                        std::vector<uint8_t> bytes(size_t(count) * (index32 ? 4 : 2) + 64);
                        for (uint8_t &b : bytes) b = uint8_t(next());
                        // some indices the reset one
                        c.reset = index32 ? 0xFFFFFFFFu : 0xFFFFu;
                        for (uint32_t i = 0; i < count; i += 5)
                            for (uint32_t k = 0; k < (index32 ? 4u : 2u); k++) bytes[size_t(i) * (index32 ? 4 : 2) + k] = 0xFF;
                        std::vector<uint32_t> fast(count + 1, 0xDEADBEEF), slow(count + 1, 0xDEADBEEF);
                        convert_indices(bytes.data(), fast.data(), count, c);
                        convert_indices_scalar(bytes.data(), slow.data(), count, c);
                        CHECK(fast == slow);
                        for (uint32_t i = 0; i < count; i++) CHECK(slow[i] == rule(bytes, i, c));
                        CHECK(fast[count] == 0xDEADBEEF); // nothing past the count
                        cases++;
                    }
    std::printf("  %d conversions checked\n", cases);
    return finish("index_convert");
}
