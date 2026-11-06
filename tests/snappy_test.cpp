// Snappy on streams written by hand from the format description: literals
// short and long, copies with 1- and 2-byte offsets (one overlapping what it
// writes), a multi-byte length, and broken streams refused.

#include "check.h"
#include "xenoforge/snappy.h"

#include <string>
#include <vector>

using namespace xf;

namespace {

std::string run(std::vector<uint8_t> in, bool *ok = nullptr) {
    std::vector<uint8_t> out;
    bool good = snappy_decompress(in, out);
    if (ok) *ok = good;
    return good ? std::string(out.begin(), out.end()) : std::string("<refused>");
}

void decodes() {
    // "abc", then 6 bytes from 3 back (overlapping)
    CHECK(run({0x09, 0x08, 'a', 'b', 'c', 0x09, 0x03}) == "abcabcabc");
    // "abcd", then 8 bytes from 4 back with a 2-byte offset
    CHECK(run({0x0C, 0x0C, 'a', 'b', 'c', 'd', 0x1E, 0x04, 0x00}) == "abcdabcdabcd");

    // a literal of 100 (length in one byte after the tag)
    std::vector<uint8_t> in = {100, 0xF0, 99};
    for (int i = 0; i < 100; i++) in.push_back(uint8_t('a' + i % 26));
    std::string out = run(in);
    CHECK(out.size() == 100 && out[0] == 'a' && out[99] == 'a' + 99 % 26);

    // 300 bytes: a two-byte length, a literal whose length takes two bytes
    in = {0xAC, 0x02, 0xF4, 0x2B, 0x01};
    for (int i = 0; i < 300; i++) in.push_back(uint8_t(i));
    out = run(in);
    CHECK(out.size() == 300 && uint8_t(out[299]) == uint8_t(299));

    uint32_t length = 0;
    CHECK(snappy_length(in, length) && length == 300);
}

void refuses() {
    bool ok = true;
    run({0x09, 0x08, 'a', 'b', 'c', 0x09, 0x04}, &ok); // offset before the start
    CHECK(!ok);
    run({0x0A, 0x08, 'a', 'b', 'c', 0x09, 0x03}, &ok); // announces more than it makes
    CHECK(!ok);
    run({0x08, 0x08, 'a', 'b', 'c', 0x09, 0x03}, &ok); // makes more than it announces
    CHECK(!ok);
    run({0x09, 0x08, 'a', 'b'}, &ok); // a literal cut short
    CHECK(!ok);
    run({0x80, 0x80, 0x80, 0x80, 0x80, 0x01}, &ok); // a length that never ends
    CHECK(!ok);
}

// What the compressor writes reads back the same: empty, short, runs,
// repeats across and past a 64 KB block, incompressible bytes; runs of
// zeros shrink to a small part of their size.
void compresses() {
    auto round_trip = [](const std::vector<uint8_t> &data) {
        std::vector<uint8_t> packed = snappy_compress(data), back;
        return snappy_decompress(packed, back) && back == data;
    };
    CHECK(round_trip({}));
    CHECK(round_trip({1, 2, 3}));
    std::vector<uint8_t> zeros(10 << 20, 0);
    CHECK(round_trip(zeros));
    CHECK(snappy_compress(zeros).size() < zeros.size() / 20);
    std::vector<uint8_t> mixed;
    uint32_t seed = 12345;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    for (int i = 0; i < 300000; i++) {
        uint32_t kind = next() >> 30;
        if (kind == 0 && mixed.size() > 70000) {
            // a repeat from far back (another block) and from near
            size_t from = mixed.size() - 1 - (next() >> 8) % 70000, n = 4 + (next() >> 24) % 200;
            for (size_t k = 0; k < n; k++) mixed.push_back(mixed[from + k]);
        } else if (kind == 1) {
            uint8_t b = uint8_t(next() >> 24);
            for (int k = 0; k < 70; k++) mixed.push_back(b);
        } else {
            mixed.push_back(uint8_t(next() >> 24));
        }
    }
    CHECK(round_trip(mixed));
    std::vector<uint8_t> noise(100000);
    for (uint8_t &b : noise) b = uint8_t(next() >> 24);
    CHECK(round_trip(noise));
}

} // namespace

int main() {
    decodes();
    refuses();
    compresses();
    return finish("snappy");
}
