// The crypto family (src/kernel/crypt.cpp), called as a game calls it,
// against the standards' vectors (FIPS 180-4 and its examples, RFC 1321,
// RFC 2202, FIPS-197, SP 800-38A, FIPS 81, the RC4 and DES classics) and
// against what the contexts in guest memory hold after each call (Xenia's
// layouts and leftovers). Where the console departs from a standard (an
// HMAC key past the block is cut, not hashed) or the construction is
// Xenia's own (XeCryptRotSumSha, XeCryptBnQwBeSigFormat), the values come
// from Python's hashlib, hmac and PyCryptodome following the same steps;
// the RSA ones from Python's integers.

#include "kernel_harness.h"

#include <span>
#include <string_view>
#include <vector>

using namespace xf;

namespace {

std::vector<uint8_t> hex(std::string_view s) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(nibble(s[i]) << 4 | nibble(s[i + 1])));
    return out;
}

std::vector<uint8_t> text(std::string_view s) { return {s.begin(), s.end()}; }

// guest memory holding data
uint32_t put(KernelHarness &h, std::span<const uint8_t> data) {
    uint32_t address = h.alloc(uint32_t(data.size() + 16));
    if (!data.empty()) std::memcpy(h.k->host(address), data.data(), data.size());
    return address;
}
uint32_t put(KernelHarness &h, std::string_view s) { return put(h, text(s)); }

std::vector<uint8_t> get(KernelHarness &h, uint32_t address, size_t size) {
    const uint8_t *p = h.k->host(address);
    return {p, p + size};
}

bool all(KernelHarness &h, uint32_t address, size_t size, uint8_t value) {
    for (uint8_t b : get(h, address, size))
        if (b != value) return false;
    return true;
}

// A number given as big-endian hex into guest memory as the XeCryptBnQw*
// functions hold it: 64-bit words, least significant first, each
// big-endian.
void put_number(KernelHarness &h, uint32_t address, std::string_view number) {
    std::vector<uint8_t> bytes = hex(number);
    size_t words = bytes.size() / 8;
    for (size_t i = 0; i < words; i++)
        std::memcpy(h.k->host(address + uint32_t(8 * i)), bytes.data() + 8 * (words - 1 - i), 8);
}
bool number_is(KernelHarness &h, uint32_t address, std::string_view number) {
    std::vector<uint8_t> bytes = hex(number);
    size_t words = bytes.size() / 8;
    for (size_t i = 0; i < words; i++)
        if (std::memcmp(h.k->host(address + uint32_t(8 * i)), bytes.data() + 8 * (words - 1 - i), 8)) return false;
    return true;
}

const std::string_view MSG56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
const std::string_view MSG112 =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqr"
    "smnopqrstnopqrstu";

// a made-up 2048-bit key (exponent 3) and its signature of the test's hash and salt
const char *const TEST_MODULUS =
    "686da64b941e5ab1c48ab6f70d920d95b2a4a09a012f105ba9abfef39e7a4e8f6e0e1b5d663b4c6199b1ea96da7f4ece874a"
    "491eec5e5c030af98288c6bcc88ffa39c4b6af306a9def4f99dc68a0b34d43d11460fabf6b43a537e7d40cf72702be9e12ff"
    "4cd78d5647cb737774274f2118fa00c99cb00288a12ca2899ab8838f74a156322451b8c1029063f2d6cc919cad1b2de34410"
    "e7df7c1d9a8eca0f5c92ba2bb3358c75d50fbdfe8cbafd5e20cb6ad571ad71364d88dce8449c937465a5f528849c9717a1ea"
    "6689663a26fe068f21c37dbf1d1a3a9133f24073e6f8066bc406fd59cd393632e410c90b12f38f9e24f9fa935d672e54bfa4"
    "658af4d4267d";
const char *const TEST_SIGNATURE =
    "48c7ec71392dce2ce8fddcb4ba67951809ac1e22ff5abcd63970f117378900734eab0c252049fe48f8d0b84f41788f5a0ebd"
    "dcde3422743fbc87e29653803643a7233ef5cbaade2873c18e0ae85b1b99a72f8b48e8b76df91f14aa3fa8eea7ca6f65e8c6"
    "724512c9658d5b08a0ff6ffe69f75bf8d5fafd63647e6535bd960c967819bf5b9b4da3cad46738fd659cdfcfdf65d64f7005"
    "e945cf7298dfa497d160cbd8aacc2e59c6df5b59093f29c697149e23f05ed215181f4c1050aaede8ef7eea0656a2d8f514ba"
    "ea4b284fe21c09eb30e6588dee8bebbbf8d3c62b92bcd899ce75d5907d842ca577fcbe7596f5532a593dbd3d568a2f275edf"
    "c3ee689dd2ed";

void sha1(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(64);
    // SHA-1("abc"), in three parts
    h.krnl("XeCryptSha", {put(h, "a"), 1, put(h, "bc"), 2, 0, 0, out, 20});
    CHECK(get(h, out, 20) == hex("a9993e364706816aba3e25717850c26c9cd0d89d"));
    // of nothing, cut to 8 bytes
    std::memset(k.host(out), 0, 64);
    h.krnl("XeCryptSha", {0, 0, 0, 0, 0, 0, out, 8});
    CHECK(get(h, out, 8) == hex("da39a3ee5e6b4b0d") && k.load32(out + 8) == 0);
    // a long message: a million 'a's
    std::vector<uint8_t> million(1000000, 'a');
    uint32_t a_million = put(h, million);
    h.krnl("XeCryptSha", {a_million, 1000000, 0, 0, 0, 0, out, 20});
    CHECK(get(h, out, 20) == hex("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

    // XECRYPT_SHA_STATE: count, five words, the buffer
    uint32_t ctx = h.alloc(0x58), msg = put(h, MSG56);
    std::memset(k.host(ctx), 0xCC, 0x58);
    h.krnl("XeCryptShaInit", {ctx});
    CHECK(k.load32(ctx) == 0 && k.load32(ctx + 4) == 0x67452301 && k.load32(ctx + 0x14) == 0xC3D2E1F0);
    CHECK(all(h, ctx + 0x18, 64, 0));
    h.krnl("XeCryptShaUpdate", {ctx, msg, 10});
    CHECK(k.load32(ctx) == 10 && get(h, ctx + 0x18, 10) == text(MSG56.substr(0, 10)));
    CHECK(k.load32(ctx + 4) == 0x67452301);
    h.krnl("XeCryptShaUpdate", {ctx, msg + 10, 46});
    CHECK(k.load32(ctx) == 56);
    h.krnl("XeCryptShaFinal", {ctx, out, 20});
    std::vector<uint8_t> digest = hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(get(h, out, 20) == digest);
    // the digest in the words, the count as it was, and the last block the
    // padding compressed (its second: zeros and the length, 448 bits) in
    // the buffer
    CHECK(k.load32(ctx) == 56 && get(h, ctx + 4, 20) == digest);
    std::vector<uint8_t> last(64, 0);
    last[62] = 0x01, last[63] = 0xC0;
    CHECK(get(h, ctx + 0x18, 64) == last);
    // finalized again: from the digest, the buffer's zeros taken as the tail
    h.krnl("XeCryptShaFinal", {ctx, out, 20});
    CHECK(get(h, out, 20) == hex("f93474782ecf913f87951b72c58589a66a8e4cf6"));

    // an update writes back only the partial block
    h.krnl("XeCryptShaInit", {ctx});
    std::memset(k.host(ctx + 0x18), 0xAA, 64);
    std::vector<uint8_t> seventy(70, 'a');
    h.krnl("XeCryptShaUpdate", {ctx, put(h, seventy), 70});
    CHECK(k.load32(ctx) == 70 && all(h, ctx + 0x18, 6, 'a') && all(h, ctx + 0x18 + 6, 58, 0xAA));
    std::memset(k.host(out), 0, 64);
    h.krnl("XeCryptShaFinal", {ctx, out, 8});
    CHECK(get(h, out, 8) == hex("ed6c69d9e8b4373a") && k.load32(out + 8) == 0);
    CHECK(get(h, ctx + 4, 20) == hex("ed6c69d9e8b4373af86303dfaa3528dfbc129902"));
}

void hmac_sha(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(32);
    // RFC 2202's cases; 6 and 7 have an 80-byte key, which the console cuts
    // to its first 64 bytes instead of hashing (RFC 2202's own answers are
    // aa4ae5e1... and e8e99d0f...)
    struct Case {
        std::vector<uint8_t> key, data;
        const char *mac;
    };
    std::vector<uint8_t> key25;
    for (int i = 1; i <= 25; i++) key25.push_back(uint8_t(i));
    const Case cases[] = {
        {std::vector<uint8_t>(20, 0x0B), text("Hi There"), "b617318655057264e28bc0b6fb378c8ef146be00"},
        {text("Jefe"), text("what do ya want for nothing?"), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
        {std::vector<uint8_t>(20, 0xAA), std::vector<uint8_t>(50, 0xDD), "125d7342b9ac11cd91a39af48aa17b4f63f175d3"},
        {key25, std::vector<uint8_t>(50, 0xCD), "4c9007f4026250c6bc8414f9bf50c86c2d7235da"},
        {std::vector<uint8_t>(20, 0x0C), text("Test With Truncation"), "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04"},
        {std::vector<uint8_t>(80, 0xAA), text("Test Using Larger Than Block-Size Key - Hash Key First"),
         "070a98992c4c1a83474cb780fc564608df3cf503"},
        {std::vector<uint8_t>(80, 0xAA),
         text("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"),
         "a6aed3e73996c8be5fc7ba1260fd81172663ec65"},
    };
    for (const Case &t : cases) {
        uint32_t key = put(h, t.key), data = put(h, t.data);
        uint32_t half = uint32_t(t.data.size() / 2), rest = uint32_t(t.data.size()) - half;
        h.krnl("XeCryptHmacSha", {key, t.key.size(), data, half, 0, 0, data + half, rest, out, 20});
        CHECK(get(h, out, 20) == hex(t.mac));
        uint32_t mac = put(h, hex(t.mac));
        CHECK(h.krnl("XeCryptHmacShaVerify", {key, t.key.size(), data, t.data.size(), 0, 0, 0, 0, mac, 20}) == 1);
        k.host(mac + 19)[0] ^= 1;
        CHECK(h.krnl("XeCryptHmacShaVerify", {key, t.key.size(), data, t.data.size(), 0, 0, 0, 0, mac, 20}) == 0);
        // only as much as is given is compared
        CHECK(h.krnl("XeCryptHmacShaVerify", {key, t.key.size(), data, t.data.size(), 0, 0, 0, 0, mac, 10}) == 1);
    }

    // XECRYPT_HMACSHA_STATE: the inner and the outer SHA states, each after
    // its pad (a whole block: the buffers stay zero)
    uint32_t ctx = h.alloc(0xB0);
    std::memset(k.host(ctx), 0xCC, 0xB0);
    h.krnl("XeCryptHmacShaInit", {ctx, put(h, "Jefe"), 4});
    CHECK(k.load32(ctx) == 64 && k.load32(ctx + 0x58) == 64);
    CHECK(all(h, ctx + 0x18, 64, 0) && all(h, ctx + 0x58 + 0x18, 64, 0));
    h.krnl("XeCryptHmacShaUpdate", {ctx, put(h, "what do ya want "), 16});
    h.krnl("XeCryptHmacShaUpdate", {ctx, put(h, "for nothing?"), 12});
    CHECK(k.load32(ctx) == 64 + 28);
    h.krnl("XeCryptHmacShaFinal", {ctx, out, 20});
    std::vector<uint8_t> mac = hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    CHECK(get(h, out, 20) == mac);
    // the outer state took the inner digest and holds the MAC
    CHECK(k.load32(ctx + 0x58) == 64 + 20 && get(h, ctx + 0x58 + 4, 20) == mac);
}

void sha2(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(128);
    struct Hash {
        const char *name, *oneshot;
        size_t digest, state_size, count_size, block;
        const char *abc, *m56, *m112, *million;
    };
    const Hash hashes[] = {
        {"XeCryptSha256", "XeCryptSha256", 32, 0x64, 4, 64,
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
        {"XeCryptSha384", "XeCryptSha384", 48, 0xC8, 8, 128,
         "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
         "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b",
         "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039",
         "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"},
        {"XeCryptSha512", "XeCryptSha512", 64, 0xC8, 8, 128,
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
         "454d4423643ce80e2a9ac94fa54ca49f",
         "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca0"
         "31ad85c7a71dd70354ec631238ca3445",
         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433a"
         "c7d329eeb6dd26545e96e55b874be909",
         "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31b"
         "eb009c5c2c49aa2e4eadb217ad8cc09b"},
    };
    std::vector<uint8_t> million(1000000, 'a');
    uint32_t a_million = put(h, million), m56 = put(h, MSG56), m112 = put(h, MSG112);
    for (const Hash &t : hashes) {
        std::string name = t.name;
        auto call = [&](const char *suffix, std::initializer_list<uint64_t> args) {
            return h.krnl((name + suffix).c_str(), args);
        };
        // one-shot, the inputs in parts
        h.krnl(t.oneshot, {put(h, "a"), 1, put(h, "b"), 1, put(h, "c"), 1, out, 128});
        CHECK(get(h, out, t.digest) == hex(t.abc));
        h.krnl(t.oneshot, {m56, 56, 0, 0, 0, 0, out, uint64_t(t.digest)});
        CHECK(get(h, out, t.digest) == hex(t.m56));
        h.krnl(t.oneshot, {m112, 100, m112 + 100, 12, 0, 0, out, uint64_t(t.digest)});
        CHECK(get(h, out, t.digest) == hex(t.m112));

        // the context: zeroed, the initial words
        uint32_t ctx = h.alloc(uint32_t(t.state_size));
        std::memset(k.host(ctx), 0xCC, t.state_size);
        call("Init", {ctx});
        uint32_t words = ctx + uint32_t(t.count_size), buffer = words + uint32_t(t.block / 2);
        CHECK(all(h, ctx, t.count_size, 0) && all(h, buffer, t.block, 0));
        CHECK(t.block == 64 ? k.load32(words) == 0x6A09E667
                            : k.load64(words) == (t.digest == 48 ? 0xCBBB9D5DC1059ED8 : 0x6A09E667F3BCC908));
        // a million 'a's in uneven parts
        uint32_t done = 0;
        for (uint32_t part : {1u, 63u, 64u, 200u, 127u, 129u, 999416u}) {
            call("Update", {ctx, a_million + done, part});
            done += part;
        }
        CHECK(done == 1000000);
        // (a 64-bit count is two words, the low one first, as the console's kernel keeps it)
        auto count_of = [&] {
            return t.count_size == 8 ? uint64_t(k.load32(ctx + 4)) << 32 | k.load32(ctx) : uint64_t(k.load32(ctx));
        };
        CHECK(count_of() == 1000000);
        call("Final", {ctx, out, 128});
        std::vector<uint8_t> digest = hex(t.million);
        CHECK(get(h, out, t.digest) == digest);
        // Final pads in the context's buffer (the length in bits ends it) and leaves the digest in the words, the
        // ones past a shorter digest zero, and the count as it was: the console's way (Xenia's leaves the words)
        std::vector<uint8_t> words_after = digest;
        words_after.resize(t.block / 2, 0);
        CHECK(get(h, words, t.block / 2) == words_after);
        CHECK(get(h, buffer + uint32_t(t.block - 8), 8) == hex("00000000007a1200"));
        CHECK(count_of() == 1000000);
        // cut short
        std::memset(k.host(out), 0, 128);
        call("Init", {ctx});
        call("Final", {ctx, out, 5});
        // (the digest of nothing, 5 bytes)
        CHECK(get(h, out, 5) == hex(t.digest == 32   ? "e3b0c44298"
                                    : t.digest == 48 ? "38b060a751"
                                                     : "cf83e1357e") &&
              k.load32(out + 5) == 0);
    }

    // the buffer as each update leaves it: SHA-256 keeps the input's whole
    // blocks out of it (Brumme's), SHA-512 copies the first through it
    // (FFmpeg's)
    std::vector<uint8_t> bytes130(130);
    for (int i = 0; i < 130; i++) bytes130[i] = uint8_t(i);
    uint32_t input = put(h, bytes130);
    uint32_t ctx = h.alloc(0xC8);
    h.krnl("XeCryptSha256Init", {ctx});
    h.krnl("XeCryptSha256Update", {ctx, input, 130});
    CHECK(get(h, ctx + 0x24, 2) == hex("8081") && all(h, ctx + 0x24 + 2, 62, 0));
    h.krnl("XeCryptSha256Final", {ctx, out, 32});
    CHECK(get(h, out, 32) == hex("8d39b60b9c767c58975b270c1d6b13c9b4507e5aee7ad496a3528e4c7f880721"));
    h.krnl("XeCryptSha512Init", {ctx});
    h.krnl("XeCryptSha512Update", {ctx, input, 130});
    std::vector<uint8_t> staged(bytes130.begin(), bytes130.begin() + 128);
    staged[0] = 128, staged[1] = 129;
    CHECK(k.load32(ctx) == 130 && k.load32(ctx + 4) == 0 && get(h, ctx + 0x48, 128) == staged);
    h.krnl("XeCryptSha512Final", {ctx, out, 64});
    CHECK(get(h, out, 64) == hex("90272b89212c81b9700897f611f13ac1d291c33a437000c1423336b4d962dd39ce23413160f02396"
                                 "3e12f4ccf90d2762b31bfc6818ef865e8a7cbf918a94c1db"));
}

void md5(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(32);
    // RFC 1321's test suite
    const std::pair<std::string_view, const char *> suite[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (auto [message, digest] : suite) {
        uint32_t m = put(h, message), size = uint32_t(message.size()), third = size / 3;
        h.krnl("XeCryptMd5", {m, third, m + third, third, m + 2 * third, size - 2 * third, out, 16});
        CHECK(get(h, out, 16) == hex(digest));
    }

    // XECRYPT_MD5_STATE: count, A to D, the buffer
    uint32_t ctx = h.alloc(0x54);
    std::memset(k.host(ctx), 0xCC, 0x54);
    h.krnl("XeCryptMd5Init", {ctx});
    CHECK(k.load32(ctx) == 0 && k.load32(ctx + 4) == 0x67452301 && k.load32(ctx + 0x10) == 0x10325476);
    CHECK(all(h, ctx + 0x14, 64, 0));
    h.krnl("XeCryptMd5Update", {ctx, put(h, "message "), 8});
    h.krnl("XeCryptMd5Update", {ctx, put(h, "digest"), 6});
    CHECK(k.load32(ctx) == 14 && get(h, ctx + 0x14, 14) == text("message digest"));
    h.krnl("XeCryptMd5Final", {ctx, out, 16});
    std::vector<uint8_t> digest = hex("f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(get(h, out, 16) == digest);
    // the digest left in the words, as bytes; the count and buffer stay
    CHECK(get(h, ctx + 4, 16) == digest && k.load32(ctx) == 14 && get(h, ctx + 0x14, 14) == text("message digest"));

    // HMAC-MD5, RFC 2202's first cases
    h.krnl("XeCryptHmacMd5", {put(h, std::vector<uint8_t>(16, 0x0B)), 16, put(h, "Hi There"), 8, 0, 0, 0, 0, out, 16});
    CHECK(get(h, out, 16) == hex("9294727a3638bb1c13f48ef8158bfc9d"));
    uint32_t data = put(h, "what do ya want for nothing?");
    h.krnl("XeCryptHmacMd5", {put(h, "Jefe"), 4, data, 10, data + 10, 18, 0, 0, out, 16});
    CHECK(get(h, out, 16) == hex("750c783e6ab0b503eaa86e310a5db738"));
    h.krnl("XeCryptHmacMd5", {put(h, std::vector<uint8_t>(16, 0xAA)), 16, put(h, std::vector<uint8_t>(50, 0xDD)), 50, 0,
                              0, 0, 0, out, 16});
    CHECK(get(h, out, 16) == hex("56be34521d144c88dbb8c733f0e8b3f6"));
}

void aes(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // XECRYPT_AES_STATE from FIPS-197 C.1's key: the round keys, then those
    // of the equivalent inverse cipher in the same order (C.1 lists the
    // ninth's as round[1].ik_sch)
    uint32_t state = h.alloc(0x160), key = put(h, hex("000102030405060708090a0b0c0d0e0f"));
    h.krnl("XeCryptAesKey", {state, key});
    CHECK(get(h, state, 16) == hex("000102030405060708090a0b0c0d0e0f"));
    CHECK(get(h, state + 160, 16) == hex("13111d7fe3944a17f307a78b4d2b30c5"));
    CHECK(get(h, state + 0xB0, 16) == hex("000102030405060708090a0b0c0d0e0f"));
    CHECK(get(h, state + 0xB0 + 9 * 16, 16) == hex("13aa29be9c8faff6f770f58000f7bf03"));
    CHECK(get(h, state + 0xB0 + 160, 16) == hex("13111d7fe3944a17f307a78b4d2b30c5"));
    uint32_t block = put(h, hex("00112233445566778899aabbccddeeff")), out = h.alloc(16);
    h.krnl("XeCryptAesEcb", {state, block, out, 1});
    CHECK(get(h, out, 16) == hex("69c4e0d86a7b0430d8cdb78070b4c55a"));
    h.krnl("XeCryptAesEcb", {state, out, out, 0});
    CHECK(get(h, out, 16) == hex("00112233445566778899aabbccddeeff"));

    // CBC, SP 800-38A F.2.1 and F.2.2: whole blocks only, the feed left at
    // the last ciphertext block
    std::vector<uint8_t> sp_key = hex("2b7e151628aed2a6abf7158809cf4f3c"), iv = hex("000102030405060708090a0b0c0d0e0f");
    std::vector<uint8_t> plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    std::vector<uint8_t> cipher = hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                                      "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    h.krnl("XeCryptAesKey", {state, put(h, sp_key)});
    uint32_t feed = put(h, iv), in = put(h, plain), data = h.alloc(80);
    std::memset(k.host(data), 0xEE, 80);
    h.krnl("XeCryptAesCbc", {state, in, 64 + 5, data, feed, 1});
    CHECK(get(h, data, 64) == cipher && all(h, data + 64, 16, 0xEE));
    CHECK(get(h, feed, 16) == std::vector<uint8_t>(cipher.end() - 16, cipher.end()));
    std::memcpy(k.host(feed), iv.data(), 16);
    h.krnl("XeCryptAesCbc", {state, data, 64, data, feed, 0}); // in place
    CHECK(get(h, data, 64) == plain && get(h, feed, 16) == std::vector<uint8_t>(cipher.end() - 16, cipher.end()));

    // keys obscured with Xenia's key-obfuscation key (zero)
    uint32_t obscured = h.alloc(16);
    h.krnl("XeKeysObscureKey", {put(h, std::vector<uint8_t>(16, 0)), obscured});
    CHECK(get(h, obscured, 16) == hex("66e94bd4ef8a2c3b884cfa59ca342b2e"));
    CHECK(h.krnl("XeKeysObscureKey", {put(h, sp_key), obscured}) == 0);
    CHECK(get(h, obscured, 16) == hex("f63059b4705c3469160e7e423734f35a"));
    std::memcpy(k.host(feed), iv.data(), 16);
    CHECK(h.krnl("XeKeysAesCbcUsingKey", {obscured, in, 64, data, feed, 1}) == 0);
    CHECK(get(h, data, 64) == cipher);
}

void rc4(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    const std::pair<const char *, const char *> vectors[] = {
        {"Key", "Plaintext"}, {"Wiki", "pedia"}, {"Secret", "Attack at dawn"}};
    const char *ciphers[] = {"bbf316e8d940af0ad3", "1021bf0420", "45a01f645fc35b383552544b9bf5"};
    for (int i = 0; i < 3; i++) {
        uint32_t data = put(h, vectors[i].second);
        uint32_t size = uint32_t(std::strlen(vectors[i].second));
        h.krnl("XeCryptRc4", {put(h, vectors[i].first), std::strlen(vectors[i].first), data, size});
        CHECK(get(h, data, size) == hex(ciphers[i]));
    }
    // XECRYPT_RC4_STATE (S, i, j) carried across calls
    uint32_t state = h.alloc(0x102), data = put(h, "Attack at dawn");
    h.krnl("XeCryptRc4Key", {state, put(h, "Secret"), 6});
    CHECK(k.host(state + 0x100)[0] == 0 && k.host(state + 0x101)[0] == 0);
    h.krnl("XeCryptRc4Ecb", {state, data, 6});
    CHECK(k.host(state + 0x100)[0] == 6);
    h.krnl("XeCryptRc4Ecb", {state, data + 6, 8});
    CHECK(get(h, data, 14) == hex("45a01f645fc35b383552544b9bf5"));
}

void des(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // XECRYPT_DES_STATE: the round keys as Xenia's DES library keeps them
    // (the first, of the classic key 133457799BBCDFF1, is 1B02EFFC7072)
    uint32_t state = h.alloc(0x180), block = h.alloc(8), out = h.alloc(8);
    h.krnl("XeCryptDesKey", {state, put(h, hex("133457799bbcdff1"))});
    CHECK(get(h, state, 8) == hex("7270fcef021b0000"));
    k.store64(block, 0x0123456789ABCDEF);
    h.krnl("XeCryptDesEcb", {state, block, out, 1});
    CHECK(k.load64(out) == 0x85E813540F0AB405);
    h.krnl("XeCryptDesEcb", {state, out, out, 0});
    CHECK(k.load64(out) == 0x0123456789ABCDEF);
    // FIPS 81's examples: ECB, and CBC with its IV
    h.krnl("XeCryptDesKey", {state, put(h, hex("0123456789abcdef"))});
    h.krnl("XeCryptDesEcb", {state, put(h, "Now is t"), out, 1});
    CHECK(k.load64(out) == 0x3FA40E8A984D4815);
    uint32_t data = put(h, "Now is the time for all "), cipher = h.alloc(24), feed = put(h, hex("1234567890abcdef"));
    h.krnl("XeCryptDesCbc", {state, data, 24, cipher, feed, 1});
    std::vector<uint8_t> want = hex("e5c7cdde872bf27c43e934008c389c0f683788499a7c05f6");
    CHECK(get(h, cipher, 24) == want && k.load64(feed) == 0x683788499A7C05F6);
    k.store64(feed, 0x1234567890ABCDEF);
    h.krnl("XeCryptDesCbc", {state, cipher, 24, cipher, feed, 0});
    CHECK(get(h, cipher, 24) == text("Now is the time for all ") && k.load64(feed) == 0x683788499A7C05F6);

    // triple DES, encrypt-decrypt-encrypt with three keys
    uint32_t keys = put(h, hex("0123456789abcdef23456789abcdef01456789abcdef0123"));
    h.krnl("XeCryptDes3Key", {state, keys});
    h.krnl("XeCryptDes3Ecb", {state, put(h, "The qufc"), out, 1});
    CHECK(k.load64(out) == 0xA826FD8CE53B855F);
    h.krnl("XeCryptDes3Ecb", {state, out, out, 0});
    CHECK(get(h, out, 8) == text("The qufc"));
    k.store64(feed, 0x1234567890ABCDEF);
    h.krnl("XeCryptDes3Cbc", {state, data, 24, cipher, feed, 1});
    CHECK(get(h, cipher, 24) == hex("f3c0ff026c023089656fbb169def7edb30ba36075d6f0176"));

    // parity: each byte's low bit makes it odd
    uint32_t bytes = put(h, hex("000102fefffe80")), fixed = h.alloc(8);
    h.krnl("XeCryptDesParity", {bytes, 7, fixed});
    CHECK(get(h, fixed, 7) == hex("010102fefefe80"));
}

void bignum(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // byte-swapped 64-bit words; copied words
    uint32_t a = put(h, hex("0102030405060708a1a2a3a4a5a6a7a8")), b = h.alloc(16);
    h.krnl("XeCryptBnQw_SwapDwQwLeBe", {a, b, 2});
    CHECK(get(h, b, 16) == hex("0807060504030201a8a7a6a5a4a3a2a1"));
    h.krnl("XeCryptBnQw_Copy", {a, b, 2});
    CHECK(get(h, b, 16) == get(h, a, 16));

    // RSA's public operation with the PIRS key (2048 bits, exponent 3) from
    // the key vault
    uint32_t rsa = h.alloc(0x110), length = h.alloc(4);
    k.store32(length, 0x110);
    CHECK(h.krnl("XeKeysGetKey", {0x39, rsa, length}) == 0);
    CHECK(k.load32(rsa) == 32 && k.load32(rsa + 4) == 3);
    uint32_t x = h.alloc(256), y = h.alloc(256);
    put_number(h, x,
               "18c2fb239a4375bd2ad8ad1cd82bb12451ad040e0f348e7c3b6cc1b1fd0111743c3e75edf23c4853e89c79de3b0e47af0fe927"
               "32e8002a8de30775ba257b552572865b0e7d546914387e66fc61e7fd43a6f76be2d9922b9befae13e1ba5c2b18c2ed27bc00ba"
               "7d3efd454563c72415f6669b9e2ba1ea3c8562c98a2b8159a682e8072a2844fd6195e659c401eeeb52b4d41909852724ee21b3"
               "53f786a3a80c19fb8e8bdc2c9c9878df89b786810512d8a2d5d71fc77e6e219e9b3a41a5ce067aa8a928ccd1806fb8cfd06739"
               "d033ce78ec2666fe3180eea93eb0b88dc04afd92327b9828233a8cbcd2753048b515ad091623fad1f7ef903d9edc6a9dc77c5f"
               "7b");
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, y, rsa}) == 1);
    CHECK(number_is(h, y,
                    "03ab9a78507a802485ef288e277ce1a94b52d295654267f9470ac9f2fc695ab718678424f328ef6bf57ea8b3a3c471e55f"
                    "8a031e45fd6423836c11b2d869ad5ebdbc70946bac8306a5d0cccb2e62415298d35e1320508442ad927bf6700695caacf1"
                    "e0865182e8716fe1703df0ce34f66098eec1f1c4c72ec53737d64650992df09af43058ccac68bcd5621c97ebaa5a706432"
                    "b3dbd5a82d8d149c049126a3a4512bcd18958bbe26f4f65bc1a508614ca91846666240b2e88f615ed24f63de7bb20682a7"
                    "2949847a4adac0ffd4fc96fce6cb6cae818cfc87041cfc950e7778122b3dc5bf012f01be1e6d66d05c27ff41ab89f96bb8"
                    "3fd63959ba49d71879409d"));
    // an input past the modulus is reduced first
    std::memset(k.host(x), 0xFF, 256);
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, x, rsa}) == 1);
    CHECK(number_is(h, x,
                    "6e4254b09006b7dbb615f2e1918a71dded1af00513a72bef7e557ab9178e97778fa8c5c5a71bf1b17b746206e9670bf29f"
                    "95e5d2d0a0d331fc7e5036cf7cae2ee2d4520ab0a7c30315d4bbd682ac7339da50b86b26af6eca951906f2da54d42c6133"
                    "d12f41c161ad72b7490498e6112b9b8710e108e4c8548c51f3e35b2068b5e4e12601a75ffd115ac52b40761181e22a22c3"
                    "3f0cdb7b73f349fb0acbfa5bb44b26ae3892cf399be2bca0654e8ee48964746bcf9d89ed3b6734e2275b8fa83be1d77e64"
                    "bde83991d862d9642fc568dafdeb703fdac33d77814c3f4e35c16152cc16ea530e16cbde81694baf6c05090944a0f9a087"
                    "ac4db88afd2b838fa9e549"));
    // 1024 bits (a modulus with a short top word), exponent 65537
    uint32_t rsa1024 = h.alloc(0x10 + 128);
    k.store32(rsa1024, 16);
    k.store32(rsa1024 + 4, 65537);
    put_number(h, rsa1024 + 0x10,
               "000000abcf34f38a8576773eb09525f93f1d2b6a911330867dd3948ec8ae58cecd0a1fc3428ff2c2becb8ba8c52582d25f3090"
               "1c6564d9a3444eb07e36aa292cb3287ca510b561648ddff5021780fb32cb144b86002c76e4343565eb85b12269d2a180df23f5"
               "b0a12d497a2f7589683cd49f12222b1bcc1ab1f6e55ce872d413");
    put_number(h, x,
               "000000315f1e144f8a9116efc71159bd6472e1d2fa583821c773f38a7680a9e70576a52bd59c225ccf1cf241589bd89bc32bd2"
               "cc1ed0017cd082f39bff2e1f00b1a39e17bc970f529483056f6e1375406abc3b81f6674bfba3aa6748a46e28404c7757e04795"
               "3ec20d9f7d24a53fd2123c8b3fef4ce8d1c268efb924630aad11");
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, y, rsa1024}) == 1);
    CHECK(number_is(h, y,
                    "0000001ec54b25fdc29e6e513d3d3fcda2cb10b72e47cceda12e10cb031d80d03224c6a7bb051579aa19fd7372f2d50826"
                    "6b219cfb1ad973079e1cff6c31933aae0ef9ac30bd8ce96c58f3dc6be4eb197a39373be78194edefe910853fc50bfd659a"
                    "aa031e829792ec3a88b22f119499d8adda5ad8fe06157202d7082d287c62"));
    // exponent 0: 1
    k.store32(rsa1024 + 4, 0);
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, y, rsa1024}) == 1);
    CHECK(k.load64(y) == 1 && all(h, y + 8, 120, 0));
    // keys under 512 bits and even moduli fail, the output untouched
    std::memset(k.host(y), 0x5A, 128);
    k.store32(rsa1024, 7);
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, y, rsa1024}) == 0);
    k.store32(rsa1024, 16);
    k.host(rsa1024 + 0x10 + 7)[0] &= 0xFE;
    CHECK(h.krnl("XeCryptBnQwNeRsaPubCrypt", {x, y, rsa1024}) == 0 && all(h, y, 128, 0x5A));

    // Montgomery's multiplication with the inverse XeCryptBnQwNeModInv
    // gives: a * b / 2^256 mod n
    uint32_t n = h.alloc(32), ma = h.alloc(32), mb = h.alloc(32), product = h.alloc(32);
    put_number(h, n, "eaa905bf176b6d2a989a835f4c11c2a61bd8c35268258f17f169ee0755fdc629");
    put_number(h, ma, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
    put_number(h, mb, "3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d");
    uint64_t inverse = h.call("xboxkrnl", "XeCryptBnQwNeModInv", {k.load64(n)});
    CHECK(inverse == 0x19CD92925E733619 && inverse * k.load64(n) == 1);
    h.krnl("XeCryptBnQwNeModMul", {ma, mb, product, inverse, n, 4});
    CHECK(number_is(h, product, "627191e8aa359dd8dcb76ec9bbcf67608f3652fd76e7ab3864ae3d52e5bccef1"));
    h.krnl("XeCryptBnQwNeModMul", {ma, mb, ma, inverse, n, 4}); // into an input
    CHECK(number_is(h, ma, "627191e8aa359dd8dcb76ec9bbcf67608f3652fd76e7ab3864ae3d52e5bccef1"));

    // the signature block XeCryptBnQwBeSigVerify compares (Xenia's
    // construction, the console's the same)
    uint32_t sig = h.alloc(0x100), hash = h.alloc(20);
    for (int i = 0; i < 20; i++) k.host(hash + i)[0] = uint8_t(0x10 + i);
    h.krnl("XeCryptBnQwBeSigFormat", {sig, hash, put(h, "XBOX360SVC")});
    CHECK(get(h, sig, 0x100) ==
          hex("6868205bdf220cbc71f3973eab481a8260d608d605c4420ecba92e7f64db4c051b23bcfb048bc4a9367ea9939bcd936f903e7d"
              "abf6c82480ef654c56be7b918d2c319eb464203a1dca985b3e0d0eb83a78ad66bdc2f68fc876ee5c40aaa700ffdac149ea2b4e"
              "6c26e1237c93afe5eb3ee16fac3533423f87ee60abded09b2eddb1771d7f62b9f057f327c5c29a9d5018f9adb1f5604000f740"
              "d45ef87fe84cb01f85ad5fc5756cde7eb05c492afd50de0f5423d9c08ab774f364e4281f756bc52af6a2dfa25fd00b34c02d0b"
              "76966ca3a718dd652ca967bb1f384ff779602fedeed8552a51cacd4811226c300622fb5142b3d0a67c7e0b272dfccea5e6f40b"
              "d7"));
    // the verification is the console's (found with its oracle): a key of 0x20 words and an exponent of 3 or 65537,
    // the signature through Montgomery products with no conversion, compared with that block. This signature was
    // made for the key by the exponent's inverse over the block times 2^(2048 * (e - 1)).
    uint32_t key = h.alloc(0x110), signature = h.alloc(0x100);
    std::vector<uint8_t> modulus = hex(TEST_MODULUS);
    k.store32(key, 0x20), k.store32(key + 4, 3);
    std::memcpy(k.host(key + 0x10), modulus.data(), modulus.size());
    std::vector<uint8_t> signature_bytes = hex(TEST_SIGNATURE);
    std::memcpy(k.host(signature), signature_bytes.data(), signature_bytes.size());
    uint32_t salt = put(h, "XBOX360SVC");
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, salt, key}) == 1);
    k.host(signature)[5] ^= 1; // one bit
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, salt, key}) == 0);
    k.host(signature)[5] ^= 1;
    k.host(hash)[3] ^= 1; // another hash
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, salt, key}) == 0);
    k.host(hash)[3] ^= 1;
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, put(h, "XBOX360XTT"), key}) == 0); // another salt
    k.store32(key + 4, 5); // an exponent the console refuses
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, salt, key}) == 0);
    k.store32(key + 4, 3), k.store32(key, 0x1F); // a key of another size
    CHECK(h.krnl("XeCryptBnQwBeSigVerify", {signature, hash, salt, key}) == 0);

    // PKCS #1 v1.5 for SHA-1 (RFC 8017 section 9.2), little-endian: reversed,
    // 00 01 FF.. 00, the DigestInfo, the hash
    uint32_t block = h.alloc(128);
    h.krnl("XeCryptBnDwLePkcs1Format", {hash, 0, block, 128});
    std::vector<uint8_t> em = hex("0001");
    em.insert(em.end(), 128 - 3 - 35, 0xFF);
    std::vector<uint8_t> digest_info = hex("003021300906052b0e03021a05000414");
    em.insert(em.end(), digest_info.begin(), digest_info.end());
    std::vector<uint8_t> hash_bytes = get(h, hash, 20);
    em.insert(em.end(), hash_bytes.begin(), hash_bytes.end());
    std::vector<uint8_t> formatted = get(h, block, 128);
    CHECK(std::vector<uint8_t>(formatted.rbegin(), formatted.rend()) == em);
    CHECK(h.krnl("XeCryptBnDwLePkcs1Verify", {hash, block, 128}) == 1);
    k.host(block + 40)[0] = 0;
    CHECK(h.krnl("XeCryptBnDwLePkcs1Verify", {hash, block, 128}) == 0);
    // formats 1 (no NULL parameters) and 2 (no DigestInfo) are told apart
    h.krnl("XeCryptBnDwLePkcs1Format", {hash, 1, block, 128});
    CHECK(k.host(block + 0x16)[0] == 0x1A && h.krnl("XeCryptBnDwLePkcs1Verify", {hash, block, 128}) == 1);
    h.krnl("XeCryptBnDwLePkcs1Format", {hash, 2, block, 128});
    CHECK(k.host(block + 0x14)[0] == 0 && k.host(block + 0x15)[0] == 0xFF);
    CHECK(h.krnl("XeCryptBnDwLePkcs1Verify", {hash, block, 128}) == 1);
    // sizes out of range: only the fill, and no verification
    h.krnl("XeCryptBnDwLePkcs1Format", {hash, 0, block, 38});
    CHECK(all(h, block, 38, 0xFF) && h.krnl("XeCryptBnDwLePkcs1Verify", {hash, block, 38}) == 0);
}

void misc(KernelHarness &h) {
    // random bytes, different each time
    uint32_t r1 = h.alloc(32), r2 = h.alloc(32);
    h.krnl("XeCryptRandom", {r1, 32});
    h.krnl("XeCryptRandom", {r2, 32});
    CHECK(get(h, r1, 32) != get(h, r2, 32) && !all(h, r1, 32, 0));
    h.krnl("XeCryptRandom", {r2, 0});

    // XeCryptRotSumSha (Xenia's construction): inputs with tails short of a
    // word
    std::vector<uint8_t> in1, in2;
    for (int i = 0; i < 40; i++) in1.push_back(uint8_t(i));
    in1.insert(in1.end(), {'x', 'y', 'z'});
    for (int i = 0; i < 100; i++) in2.push_back(uint8_t(i * 7));
    uint32_t out = h.alloc(20);
    h.krnl("XeCryptRotSumSha", {put(h, in1), in1.size(), put(h, in2), in2.size(), out, 20});
    CHECK(get(h, out, 20) == hex("95617bf0ed25d30f59ba4d5cc540ac9e574f1aa9"));
}

void keys(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // the vault's sizes
    CHECK(h.krnl("XeKeysGetKeyProperties", {0x00}) == 1 && h.krnl("XeKeysGetKeyProperties", {0x19}) == 0x10);
    CHECK(h.krnl("XeKeysGetKeyProperties", {0x1B}) == 0x18 && h.krnl("XeKeysGetKeyProperties", {0x33}) == 0x1D0);
    CHECK(h.krnl("XeKeysGetKeyProperties", {0x36}) == 0x1A8 && h.krnl("XeKeysGetKeyProperties", {0x38}) == 0x2108);
    CHECK(h.krnl("XeKeysGetKeyProperties", {0x44}) == 0x100 && h.krnl("XeKeysGetKeyProperties", {0x39}) == 0);

    // the keys Xenia has, to buffers of exactly their size
    uint32_t buffer = h.alloc(32), length = h.alloc(4);
    std::vector<uint8_t> key19 = hex("e1bc159c73b1eae9ab3170f3ad47ebf3");
    k.store32(length, 16);
    CHECK(h.krnl("XeKeysGetKey", {0x19, buffer, length}) == 0 && get(h, buffer, 16) == key19 && k.load32(length) == 16);
    k.store32(length, 15);
    CHECK(h.krnl("XeKeysGetKey", {0x19, buffer, length}) == 0xC0000023);
    k.store32(length, 12);
    CHECK(h.krnl("XeKeysGetKey", {0x10014, buffer, length}) == 0 && get(h, buffer, 12) == text("012345678910"));
    CHECK(h.krnl("XeKeysGetKey", {0x33, buffer, length}) == 0xC0000022);
    CHECK(h.krnl("XeKeysGetKey", {0x2000, buffer, length}) == kernel::X_STATUS_INVALID_PARAMETER);
    std::memset(k.host(buffer), 0x77, 32);
    CHECK(h.krnl("XeKeysGetKey", {0x20, buffer, length}) == 0 && all(h, buffer, 32, 0x77));

    // HMAC-SHA under key 0x19, directly and as an obscured key
    uint32_t out = h.alloc(20), data = put(h, "abc");
    std::vector<uint8_t> mac = hex("b83ff9e5d3485d155c8d43c6b0f4cd1c3a274e8d");
    CHECK(h.krnl("XeKeysHmacSha", {0x19, data, 1, data + 1, 2, 0, 0, out, 20}) == 0 && get(h, out, 20) == mac);
    CHECK(h.krnl("XeKeysHmacSha", {0x18, data, 3, 0, 0, 0, 0, out, 20}) == kernel::X_STATUS_UNSUCCESSFUL);
    uint32_t obscured = h.alloc(16);
    h.krnl("XeKeysObscureKey", {put(h, key19), obscured});
    std::memset(k.host(out), 0, 20);
    CHECK(h.krnl("XeKeysHmacShaUsingKey", {obscured, data, 3, 0, 0, 0, 0, out, 20}) == 0 && get(h, out, 20) == mac);
    CHECK(h.krnl("XeKeysHmacShaUsingKey", {0, data, 3, 0, 0, 0, 0, out, 20}) == kernel::X_STATUS_INVALID_PARAMETER);

    // the console: retail, Xenia's id
    uint32_t type = h.alloc(4), id = h.alloc(5), id_text = h.alloc(16);
    CHECK(h.krnl("XeKeysGetConsoleType", {type}) == 0 && k.load32(type) == 2);
    std::memset(k.host(id_text), 0, 16);
    CHECK(h.krnl("XeKeysGetConsoleID", {id, id_text}) == 0);
    CHECK(get(h, id, 5) == hex("930164e607") && h.read(id_text) == "245149100000");
    CHECK(h.krnl("XeKeysGetConsoleID", {0, 0}) == 0);

    // the console certificate Xenia makes up
    uint32_t certificate = h.alloc(0x1A8);
    CHECK(h.krnl("XeKeysConsolePrivateKeySign", {out, certificate}) == 1);
    CHECK(k.load32(certificate + 0x18) == 2 && k.host(certificate + 0x1C)[3] == 5);
#if defined(_MSC_VER)
    // XE_CONSOLE_ID's bit fields as Xenia's compiler lays them out
#pragma pack(push, 1)
    struct ConsoleId {
        union {
            struct {
                uint8_t RefurbBits : 4;
                uint8_t ManufactureMonth : 4;
                uint32_t ManufactureYear : 4;
                uint32_t MacIndex3 : 8;
                uint32_t MacIndex4 : 8;
                uint32_t MacIndex5 : 8;
                uint32_t Crc : 4;
            };
            uint8_t Data[5];
        };
    };
#pragma pack(pop)
    ConsoleId console{};
    console.RefurbBits = 3, console.ManufactureMonth = 9, console.ManufactureYear = 1;
    console.MacIndex3 = 0x40, console.MacIndex4 = 0x66, console.MacIndex5 = 0x7E, console.Crc = 0;
    CHECK(sizeof(ConsoleId) == 5 && std::memcmp(k.host(certificate + 2), console.Data, 5) == 0);
#endif
    CHECK(get(h, certificate + 2, 5) == hex("930164e607"));
    CHECK(h.krnl("XeKeysConsolePrivateKeySign", {0, certificate}) == 0);

    // every console signature verifies, as this console's own
    uint32_t signature = h.alloc(0x228), compare = h.alloc(4);
    k.store32(compare, 0);
    CHECK(h.krnl("XeKeysConsoleSignatureVerification", {out, signature, compare}) == 1 && k.load32(compare) == 1);
    CHECK(h.krnl("XeKeysConsoleSignatureVerification", {out, signature, 0}) == 1);
}

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_crypt: no kernel\n"), 1;
    sha1(h);
    hmac_sha(h);
    sha2(h);
    md5(h);
    aes(h);
    rc4(h);
    des(h);
    bignum(h);
    misc(h);
    keys(h);
    // every export here is implemented
    CHECK(h.k->unimplemented().empty());
    return finish("kernel_crypt");
}
