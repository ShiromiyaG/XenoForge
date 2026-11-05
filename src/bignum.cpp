// Modular exponentiation by Montgomery's method (the Handbook of Applied
// Cryptography, algorithms 14.36 and 14.94): with R = 2^(64 * words), the
// base goes to base * R mod n, squarings and multiplications stay in that
// form (each a multiplication followed by a division by R that only needs
// the low word made zero), and a last multiplication by 1 brings the result
// back. The words are 64 bits; products go through 128-bit integers.

#include "xenoforge/bignum.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace xf::bignum {
namespace {

using u128 = unsigned __int128;

// -m^-1 mod 2^64 for an odd m: Newton's iteration x = x * (2 - m * x)
// doubles the correct low bits, and m is its own inverse to three bits
uint64_t neg_inverse(uint64_t m) {
    uint64_t x = m;
    for (int i = 0; i < 5; i++) x *= 2 - m * x;
    return 0 - x;
}

// a < b, both size words
bool less(const uint64_t *a, const uint64_t *b, size_t size) {
    for (size_t i = size; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

// a -= b, both size words, the borrow out dropped
void subtract(uint64_t *a, const uint64_t *b, size_t size) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < size; i++) {
        uint64_t d = a[i] - b[i];
        uint64_t next = (a[i] < b[i]) | (d < borrow);
        a[i] = d - borrow;
        borrow = next;
    }
}

struct Montgomery {
    const uint64_t *n;
    size_t size;
    uint64_t n0; // -n^-1 mod 2^64
    std::vector<uint64_t> t;

    // out = a * b / R mod n, for a < R and b < n: the sum before the last
    // subtraction is (a * b + m * n) / R < 2n
    void mul(const uint64_t *a, const uint64_t *b, uint64_t *out) {
        std::fill(t.begin(), t.end(), 0);
        for (size_t i = 0; i < size; i++) {
            // t += a * b[i]
            uint64_t carry = 0;
            for (size_t j = 0; j < size; j++) {
                u128 s = u128(a[j]) * b[i] + t[j] + carry;
                t[j] = uint64_t(s);
                carry = uint64_t(s >> 64);
            }
            u128 s = u128(t[size]) + carry;
            t[size] = uint64_t(s);
            t[size + 1] = uint64_t(s >> 64);
            // t += m * n, which makes the low word zero, and t /= 2^64
            uint64_t m = t[0] * n0;
            s = u128(m) * n[0] + t[0];
            carry = uint64_t(s >> 64);
            for (size_t j = 1; j < size; j++) {
                s = u128(m) * n[j] + t[j] + carry;
                t[j - 1] = uint64_t(s);
                carry = uint64_t(s >> 64);
            }
            s = u128(t[size]) + carry;
            t[size - 1] = uint64_t(s);
            t[size] = t[size + 1] + uint64_t(s >> 64);
        }
        if (t[size] || !less(t.data(), n, size)) subtract(t.data(), n, size);
        std::copy_n(t.begin(), size, out);
    }
};

} // namespace

bool mod_exp(std::span<const uint64_t> base, uint32_t exponent, std::span<const uint64_t> modulus,
             std::span<uint64_t> out) {
    size_t size = modulus.size();
    if (!size || !(modulus[0] & 1) || base.size() != size || out.size() != size) return false;
    Montgomery mont{modulus.data(), size, neg_inverse(modulus[0]), std::vector<uint64_t>(size + 2)};

    // 1 mod n (0 when n is 1), then R^2 mod n by doubling it 2 * 64 * size
    // times, subtracting n whenever the double reaches it
    std::vector<uint64_t> one(size), r2(size);
    one[0] = 1;
    r2[0] = 1;
    if (!less(r2.data(), modulus.data(), size)) subtract(r2.data(), modulus.data(), size);
    for (size_t bit = 0; bit < 128 * size; bit++) {
        uint64_t carry = 0;
        for (size_t i = 0; i < size; i++) {
            uint64_t next = r2[i] >> 63;
            r2[i] = r2[i] << 1 | carry;
            carry = next;
        }
        if (carry || !less(r2.data(), modulus.data(), size)) subtract(r2.data(), modulus.data(), size);
    }

    // base * R and R (1) in Montgomery's form; square and multiply from the
    // exponent's top bit
    std::vector<uint64_t> x(size), acc(size);
    mont.mul(base.data(), r2.data(), x.data());
    mont.mul(one.data(), r2.data(), acc.data());
    for (int bit = 31 - std::countl_zero(exponent); bit >= 0; bit--) {
        mont.mul(acc.data(), acc.data(), acc.data());
        if (exponent >> bit & 1) mont.mul(acc.data(), x.data(), acc.data());
    }
    mont.mul(acc.data(), one.data(), out.data());
    return true;
}

} // namespace xf::bignum
