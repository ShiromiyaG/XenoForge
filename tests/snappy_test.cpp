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

} // namespace

int main() {
    decodes();
    refuses();
    return finish("snappy");
}
