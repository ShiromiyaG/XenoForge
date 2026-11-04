// AES-128 against the standards' vectors: FIPS-197 appendix C.1 and
// SP 800-38A F.2.1/F.2.2 (CBC).

#include "check.h"
#include "xenoforge/aes.h"

#include <cstring>
#include <vector>

using namespace xf;

namespace {

std::vector<uint8_t> hex(const char *s) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (; s[0] && s[1]; s += 2) out.push_back(uint8_t(nibble(s[0]) << 4 | nibble(s[1])));
    return out;
}

void fips197() {
    auto key = aes::expand(hex("000102030405060708090a0b0c0d0e0f").data());
    auto plain = hex("00112233445566778899aabbccddeeff");
    auto cipher = hex("69c4e0d86a7b0430d8cdb78070b4c55a");
    uint8_t out[16];
    aes::encrypt_block(key, plain.data(), out);
    CHECK(std::memcmp(out, cipher.data(), 16) == 0);
    aes::decrypt_block(key, cipher.data(), out);
    CHECK(std::memcmp(out, plain.data(), 16) == 0);
}

void sp800_38a_cbc() {
    auto key = aes::expand(hex("2b7e151628aed2a6abf7158809cf4f3c").data());
    auto iv = hex("000102030405060708090a0b0c0d0e0f");
    auto plain = hex("6bc1bee22e409f96e93d7e117393172a"
                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                     "30c81c46a35ce411e5fbc1191a0a52ef"
                     "f69f2445df4f9b17ad2b417be66c3710");
    auto cipher = hex("7649abac8119b246cee98e9b12e9197d"
                      "5086cb9b507219ee95db113a917678b2"
                      "73bed6b8e3c1743b7116e69e22229516"
                      "3ff1caa1681fac09120eca307586e1a7");
    auto data = plain;
    aes::encrypt_cbc(key, iv.data(), data.data(), data.size());
    CHECK(data == cipher);
    aes::decrypt_cbc(key, iv.data(), data.data(), data.size());
    CHECK(data == plain);
}

void ecb_is_blockwise() {
    auto key = aes::expand(hex("000102030405060708090a0b0c0d0e0f").data());
    auto data = hex("69c4e0d86a7b0430d8cdb78070b4c55a69c4e0d86a7b0430d8cdb78070b4c55a");
    aes::decrypt_ecb(key, data.data(), data.size());
    auto plain = hex("00112233445566778899aabbccddeeff");
    CHECK(std::memcmp(data.data(), plain.data(), 16) == 0);
    CHECK(std::memcmp(data.data() + 16, plain.data(), 16) == 0);
}

} // namespace

int main() {
    fips197();
    sp800_38a_cbc();
    ecb_is_blockwise();
    return finish("aes");
}
