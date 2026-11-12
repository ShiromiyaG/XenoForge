// A draw's guest indices to the host's (index_convert.h).

#include "xenoforge/index_convert.h"

#include <algorithm>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define XF_INDEX_AVX2 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace xf::render {
namespace {

uint32_t load_index(const uint8_t *in, uint32_t i, const IndexConversion &c) {
    if (!c.index32) {
        const uint8_t *p = in + size_t(i) * 2;
        return c.endian == 1 ? uint32_t(p[0]) << 8 | p[1] : uint32_t(p[0]) | uint32_t(p[1]) << 8;
    }
    uint32_t v;
    std::memcpy(&v, in + size_t(i) * 4, 4);
    switch (c.endian) {
    case 1: return (v << 8 & 0xFF00FF00) | (v >> 8 & 0x00FF00FF);
    case 2: return __builtin_bswap32(v);
    case 3: return v >> 16 | v << 16;
    default: return v;
    }
}

uint32_t finish(uint32_t v, const IndexConversion &c) {
    if (c.restart && v == c.reset) return UINT32_MAX;
    if (!c.remap) return v;
    return std::min(c.max, std::max(c.min, (v + c.offset) & 0xFFFFFF));
}

#ifdef XF_INDEX_AVX2
__attribute__((target("xsave"))) unsigned long long xcr0() { return _xgetbv(0); }

bool has_avx2() {
    static const bool yes = [] {
        unsigned a = 0, b = 0, cx = 0, d = 0;
#ifdef _MSC_VER
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuid(r, 1);
        cx = unsigned(r[2]);
#else
        if (__get_cpuid_max(0, nullptr) < 7) return false;
        __cpuid(1, a, b, cx, d);
#endif
        // OSXSAVE and AVX, and the OS saving the YMM state
        if ((cx & (1u << 27)) == 0 || (cx & (1u << 28)) == 0) return false;
        if ((xcr0() & 6) != 6) return false;
#ifdef _MSC_VER
        __cpuidex(r, 7, 0);
        b = unsigned(r[1]);
#else
        __cpuid_count(7, 0, a, b, cx, d);
#endif
        return (b & (1u << 5)) != 0;
    }();
    return yes;
}

// eight indices a time; the rest left to the scalar loop (the count done)
__attribute__((target("avx2"))) uint32_t convert_avx2(const uint8_t *in, uint32_t *out, uint32_t count,
                                                      const IndexConversion &c) {
    const __m256i reset = _mm256_set1_epi32(int(c.reset)), offset = _mm256_set1_epi32(int(c.offset));
    const __m256i low24 = _mm256_set1_epi32(0xFFFFFF), lo = _mm256_set1_epi32(int(c.min)),
                  hi = _mm256_set1_epi32(int(c.max));
    // the byte orders within each 32-bit lane (pshufb within 128-bit halves)
    const __m256i swap16 = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7,
                                            6, 9, 8, 11, 10, 13, 12, 15, 14);
    const __m256i swap32 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6,
                                            5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i halves = _mm256_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13, 2, 3, 0, 1, 6, 7,
                                            4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
    const __m128i swap16_128 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i v;
        if (c.index32) {
            v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(in + size_t(i) * 4));
            if (c.endian == 1) v = _mm256_shuffle_epi8(v, swap16);
            else if (c.endian == 2) v = _mm256_shuffle_epi8(v, swap32);
            else if (c.endian == 3) v = _mm256_shuffle_epi8(v, halves);
        } else {
            __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i *>(in + size_t(i) * 2));
            if (c.endian == 1) h = _mm_shuffle_epi8(h, swap16_128);
            v = _mm256_cvtepu16_epi32(h);
        }
        __m256i r = v;
        if (c.remap)
            r = _mm256_min_epu32(hi, _mm256_max_epu32(lo, _mm256_and_si256(_mm256_add_epi32(v, offset), low24)));
        if (c.restart) r = _mm256_or_si256(r, _mm256_cmpeq_epi32(v, reset));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i), r);
    }
    return i;
}
#endif

} // namespace

void convert_indices_scalar(const uint8_t *in, uint32_t *out, uint32_t count, const IndexConversion &c) {
    for (uint32_t i = 0; i < count; i++) out[i] = finish(load_index(in, i, c), c);
}

void convert_indices(const uint8_t *in, uint32_t *out, uint32_t count, const IndexConversion &c) {
    uint32_t done = 0;
#ifdef XF_INDEX_AVX2
    if (has_avx2()) done = convert_avx2(in, out, count, c);
#endif
    for (uint32_t i = done; i < count; i++) out[i] = finish(load_index(in, i, c), c);
}

} // namespace xf::render
