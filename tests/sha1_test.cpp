// SHA-1 (FIPS 180-4's examples) and HMAC-SHA-1 (RFC 2202's test cases 1, 2,
// 3, 6 and 7: a key shorter than the block, one of 20 bytes, one past the
// block).

#include "check.h"
#include "xenoforge/sha1.h"

#include <cstring>
#include <string>
#include <vector>

using namespace xf;

namespace {

std::string hex(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        char b[3];
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

std::string mac(const std::vector<uint8_t> &key, const std::vector<uint8_t> &data) {
    uint8_t d[20];
    hmac_sha1(key, data, d);
    return hex(d, 20);
}

} // namespace

int main() {
    uint8_t d[20];
    sha1(bytes("abc"), d);
    CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), d);
    CHECK(hex(d, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    CHECK(mac(std::vector<uint8_t>(20, 0x0b), bytes("Hi There")) == "b617318655057264e28bc0b6fb378c8ef146be00");
    CHECK(mac(bytes("Jefe"), bytes("what do ya want for nothing?")) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    CHECK(mac(std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd)) ==
           "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
    CHECK(mac(std::vector<uint8_t>(80, 0xaa), bytes("Test Using Larger Than Block-Size Key - Hash Key First")) ==
           "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    CHECK(mac(std::vector<uint8_t>(80, 0xaa),
              bytes("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data")) ==
           "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
    return finish("sha1");
}
