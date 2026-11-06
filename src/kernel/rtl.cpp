/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The Rtl family's pure functions (plan step A4; docs/runtime.md): memory
// and string helpers, time conversions, status codes to Win32 errors, and
// the CRT's printf family (DbgPrint included). Translated from Xenia (BSD;
// see LICENSE-xenia): xboxkrnl_rtl.cc, xboxkrnl_error.cc (its tables written
// out by value, the named codes resolved from xbox.h) and
// xboxkrnl_strings.cc; checked against them. Critical sections, unwinding
// and the stack limits belong to the threads family. The CRC-32 table is
// made from the polynomial's definition (Xenia writes it out); where
// RtlImageDirectoryEntryToData differs from Xenia, it says.
//
// Where the console's own kernel answered differently from Xenia (the
// oracle, XenoForge-private's xfkrnl, on system software 2.0.16202), the
// console wins, as noted at each: the Unicode case mapping,
// RtlCopyUnicodeString, RtlUnicodeStringToAnsiString and the formatter's
// "%ll" (read as 32 bits; "%I64" stays 64).

#include "xenoforge/kernel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_BUFFER_OVERFLOW = 0x80000005;

// ---- memory and strings (xboxkrnl_rtl.cc) ----

XF_EXPORT(xboxkrnl, RtlCompareMemory) {
    // (source1, source2, length): the length of the common prefix (the NT
    // kernel's answer; Xenia counts every byte that matches)
    uint32_t a = arg(k, c, 0), b = arg(k, c, 1), length = arg(k, c, 2);
    uint32_t count = 0;
    while (count < length && k.host(a)[count] == k.host(b)[count]) count++;
    result(c, count);
}

XF_EXPORT(xboxkrnl, RtlCompareMemoryUlong) {
    // (source, length, pattern): the bytes of the leading words equal to the
    // pattern
    uint32_t source = arg(k, c, 0), length = arg(k, c, 1), pattern = arg(k, c, 2);
    uint32_t compared = 0;
    for (uint32_t aligned = length & 0xFFFFFFFC; aligned; aligned -= 4, compared += 4)
        if (k.load32(source + compared) != pattern) break;
    result(c, compared);
}

XF_EXPORT(xboxkrnl, RtlFillMemoryUlong) {
    // (destination, length, pattern): length is a multiple of 4
    uint32_t destination = arg(k, c, 0), count = arg(k, c, 1) >> 2, pattern = arg(k, c, 2);
    for (uint32_t n = 0; n < count; n++) k.store32(destination + 4 * n, pattern);
}

constexpr unsigned char rtl_lower_table[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
    0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x5B, 0x5C, 0x5D, 0x5E,
    0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD,
    0xBE, 0xBF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xD7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3,
    0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

constexpr unsigned char rtl_upper_table[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
    0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E,
    0x5F, 0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51,
    0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD,
    0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0,
    0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xC0, 0xC1, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6,
    0xF7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0x3F};

XF_EXPORT(xboxkrnl, RtlUpperChar) { result(c, rtl_upper_table[arg(k, c, 0) & 0xFF]); }
XF_EXPORT(xboxkrnl, RtlLowerChar) { result(c, rtl_lower_table[arg(k, c, 0) & 0xFF]); }

// RtlCompareStringN_impl: a length of 0xFFFFFFFF is up to the terminator
int compare_strings(Kernel &k, uint32_t s1, uint32_t len1, uint32_t s2, uint32_t len2, bool case_insensitive) {
    const uint8_t *a = k.host(s1), *b = k.host(s2);
    if (len1 == 0xFFFFFFFF) len1 = uint32_t(std::strlen(reinterpret_cast<const char *>(a)));
    if (len2 == 0xFFFFFFFF) len2 = uint32_t(std::strlen(reinterpret_cast<const char *>(b)));
    const uint8_t *end = a + std::min(len1, len2);
    while (a < end) {
        unsigned c1 = *a++, c2 = *b++;
        if (c1 == c2) continue;
        if (!case_insensitive) return int(c1 - c2);
        unsigned u1 = rtl_upper_table[c1], u2 = rtl_upper_table[c2];
        if (u1 != u2) return int(u1 - u2);
    }
    return int(len1 - len2);
}

XF_EXPORT(xboxkrnl, RtlCompareStringN) {
    // (string1, length1, string2, length2, case_insensitive)
    result(c, uint32_t(compare_strings(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4) != 0)));
}

XF_EXPORT(xboxkrnl, RtlCompareString) {
    // (ANSI_STRING *, ANSI_STRING *, case_insensitive)
    uint32_t a = arg(k, c, 0), b = arg(k, c, 1);
    result(c,
           uint32_t(compare_strings(k, k.load32(a + 4), k.load16(a), k.load32(b + 4), k.load16(b), arg(k, c, 2) != 0)));
}

// X_ANSI_STRING and X_UNICODE_STRING: length, maximum length, pointer
void set_string(Kernel &k, uint32_t string, uint16_t length, uint16_t maximum, uint32_t pointer) {
    k.store16(string + 0, length);
    k.store16(string + 2, maximum);
    k.store32(string + 4, pointer);
}

XF_EXPORT(xboxkrnl, RtlInitAnsiString) {
    uint32_t destination = arg(k, c, 0), source = arg(k, c, 1);
    if (source) {
        uint16_t length = uint16_t(std::strlen(reinterpret_cast<const char *>(k.host(source))));
        set_string(k, destination, length, uint16_t(length + 1), source);
    } else {
        set_string(k, destination, 0, 0, 0);
    }
}

XF_EXPORT(xboxkrnl, RtlInitUnicodeString) {
    uint32_t destination = arg(k, c, 0), source = arg(k, c, 1);
    if (source) {
        uint32_t size = 0;
        while (k.load16(source + 2 * size)) size++;
        set_string(k, destination, uint16_t(size * 2), uint16_t((size + 1) * 2), source);
    } else {
        set_string(k, destination, 0, 0, 0);
    }
    result(c, destination);
}

XF_EXPORT(xboxkrnl, RtlFreeAnsiString) {
    uint32_t string = arg(k, c, 0);
    if (uint32_t pointer = k.load32(string + 4)) k.memory().system_free(pointer);
    set_string(k, string, 0, 0, 0);
}

XF_EXPORT(xboxkrnl, RtlFreeUnicodeString) {
    uint32_t string = arg(k, c, 0);
    if (uint32_t pointer = k.load32(string + 4)) k.memory().system_free(pointer);
    set_string(k, string, 0, 0, 0);
}

XF_EXPORT(xboxkrnl, RtlCopyString) {
    uint32_t destination = arg(k, c, 0), source = arg(k, c, 1);
    if (!source) return k.store16(destination, 0);
    uint16_t length = std::min(k.load16(destination + 2), k.load16(source));
    if (length) std::memcpy(k.host(k.load32(destination + 4)), k.host(k.load32(source + 4)), length);
    k.store16(destination, length);
}

XF_EXPORT(xboxkrnl, RtlCopyUnicodeString) {
    // (destination, source): as NT's and the console's: at most the destination's
    // maximum length in bytes (Xenia copies twice the byte count, past the
    // buffer), the length set, and a terminator after the text when there is
    // room for one
    uint32_t destination = arg(k, c, 0), source = arg(k, c, 1);
    if (!source) return k.store16(destination, 0);
    uint32_t maximum = k.load16(destination + 2), buffer = k.load32(destination + 4);
    uint16_t length = uint16_t(std::min<uint32_t>(maximum, k.load16(source)));
    if (length) std::memcpy(k.host(buffer), k.host(k.load32(source + 4)), length);
    k.store16(destination, length);
    if (length < maximum) k.store16(buffer + length, 0);
}

XF_EXPORT(xboxkrnl, RtlUnicodeStringToAnsiString) {
    // (ANSI_STRING *destination, UNICODE_STRING *source, allocate): as the
    // console's, which is NT's. Each UTF-16 unit becomes one byte, the units
    // past Latin-1 a '?' (RtlUnicodeToMultiByteN's; Xenia writes UTF-8). The
    // destination's Length is the converted length, cut to its maximum less
    // one when the buffer is too short (BUFFER_OVERFLOW; a maximum of 0
    // writes nothing), a terminator after the text.
    uint32_t destination = arg(k, c, 0), source = arg(k, c, 1), allocate = arg(k, c, 2);
    uint32_t chars = k.load16(source) / 2, pointer = k.load32(source + 4);
    uint32_t length = chars, status = X_STATUS_SUCCESS, buffer;
    if (allocate) {
        buffer = k.memory().system_alloc(length + 1);
        if (!buffer) return result(c, X_STATUS_NO_MEMORY);
        set_string(k, destination, uint16_t(length), uint16_t(length + 1), buffer);
    } else {
        uint32_t maximum = k.load16(destination + 2);
        buffer = k.load32(destination + 4);
        if (length >= maximum) {
            k.store16(destination, uint16_t(length));
            if (!maximum) return result(c, X_STATUS_BUFFER_OVERFLOW);
            length = maximum - 1;
            status = X_STATUS_BUFFER_OVERFLOW;
        }
        k.store16(destination, uint16_t(length));
    }
    uint8_t *text = k.host(buffer);
    for (uint32_t i = 0; i < length; i++) {
        uint16_t ch = k.load16(pointer + 2 * i);
        text[i] = ch < 256 ? uint8_t(ch) : '?';
    }
    text[length] = 0;
    result(c, status);
}

XF_EXPORT(xboxkrnl, RtlMultiByteToUnicodeN) {
    // (destination, destination_length in bytes, written_ptr, source,
    // source_length): bytes widened as they are
    uint32_t destination = arg(k, c, 0), destination_length = arg(k, c, 1), written_ptr = arg(k, c, 2);
    uint32_t source = arg(k, c, 3), source_length = arg(k, c, 4);
    uint32_t copy_length = std::min(destination_length >> 1, source_length);
    for (uint32_t i = 0; i < copy_length; i++) k.store16(destination + 2 * i, k.host(source)[i]);
    if (written_ptr) k.store32(written_ptr, copy_length << 1);
    result(c, 0);
}

XF_EXPORT(xboxkrnl, RtlUnicodeToMultiByteN) {
    // (destination, destination_length, written_ptr, source, source_length
    // in bytes): what is past Latin-1 becomes '?'
    uint32_t destination = arg(k, c, 0), destination_length = arg(k, c, 1), written_ptr = arg(k, c, 2);
    uint32_t source = arg(k, c, 3), source_length = arg(k, c, 4);
    uint32_t copy_length = std::min(source_length >> 1, destination_length);
    for (uint32_t i = 0; i < copy_length; i++) {
        uint16_t ch = k.load16(source + 2 * i);
        k.host(destination)[i] = ch < 256 ? uint8_t(ch) : '?';
    }
    if (written_ptr) k.store32(written_ptr, copy_length);
    result(c, 0);
}

// The console's case table (Unicode 2.1.2's simple mappings of the Basic
// Multilingual Plane, as the console's kernel has them: tools/gen_unicode_case.py
// says what changed and why). Xenia maps with the host's towupper/towlower.
struct CaseRun {
    uint16_t first, last;
    uint8_t stride;
    int32_t delta;
};

#include "unicode_case.inc"

uint16_t map_case(const CaseRun *begin, const CaseRun *end, uint16_t ch) {
    // the last run that starts at or before ch (the runs are sorted and apart)
    const CaseRun *run =
        std::upper_bound(begin, end, ch, [](uint16_t value, const CaseRun &r) { return value < r.first; });
    if (run == begin) return ch;
    --run;
    if (ch > run->last || (ch - run->first) % run->stride) return ch;
    return uint16_t(ch + run->delta);
}

XF_EXPORT(xboxkrnl, RtlUpcaseUnicodeChar) {
    result(c, map_case(std::begin(upcase_runs), std::end(upcase_runs), uint16_t(arg(k, c, 0))));
}
XF_EXPORT(xboxkrnl, RtlDowncaseUnicodeChar) {
    result(c, map_case(std::begin(downcase_runs), std::end(downcase_runs), uint16_t(arg(k, c, 0))));
}

// ---- time (FILETIME: 100 ns since 1601-01-01) ----

// Howard Hinnant's civil calendar algorithms (public domain), as Xenia's
// date library computes them: days since 1970-01-01 and back
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = unsigned(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = unsigned(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = int64_t(yoe) + era * 400 + (m <= 2);
}

constexpr int64_t DAYS_1601_TO_1970 = 134774;
constexpr int64_t MS_PER_DAY = 86'400'000;

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// X_TIME_FIELDS: year, month, day, hour, minute, second, milliseconds,
// weekday (16 bits each)
XF_EXPORT(xboxkrnl, RtlTimeToTimeFields) {
    uint32_t time_ptr = arg(k, c, 0), fields = arg(k, c, 1);
    int64_t ms = floor_div(int64_t(k.load64(time_ptr)), 10000);
    int64_t days = floor_div(ms, MS_PER_DAY) - DAYS_1601_TO_1970; // since 1970
    int64_t time = ms - (days + DAYS_1601_TO_1970) * MS_PER_DAY;
    int64_t year;
    unsigned month, day;
    civil_from_days(days, year, month, day);
    k.store16(fields + 0x0, uint16_t(year));
    k.store16(fields + 0x2, uint16_t(month));
    k.store16(fields + 0x4, uint16_t(day));
    k.store16(fields + 0x6, uint16_t(time / 3'600'000));
    k.store16(fields + 0x8, uint16_t(time / 60'000 % 60));
    k.store16(fields + 0xA, uint16_t(time / 1000 % 60));
    k.store16(fields + 0xC, uint16_t(time % 1000));
    k.store16(fields + 0xE, uint16_t(((days % 7) + 11) % 7)); // Sunday 0 (1970-01-01 was a Thursday)
}

XF_EXPORT(xboxkrnl, RtlTimeFieldsToTime) {
    uint32_t fields = arg(k, c, 0), time_ptr = arg(k, c, 1);
    unsigned year = k.load16(fields + 0x0), month = k.load16(fields + 0x2), day = k.load16(fields + 0x4);
    unsigned hour = k.load16(fields + 0x6), minute = k.load16(fields + 0x8), second = k.load16(fields + 0xA);
    unsigned milliseconds = k.load16(fields + 0xC);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 ||
        milliseconds > 999)
        return result(c, 0);
    // a day the month has
    static constexpr unsigned days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day > days_in_month[month - 1] + (month == 2 && leap)) return result(c, 0);
    int64_t days = days_from_civil(year, month, day) + DAYS_1601_TO_1970;
    int64_t ms = days * MS_PER_DAY + hour * 3'600'000ll + minute * 60'000ll + second * 1000ll + milliseconds;
    k.store64(time_ptr, uint64_t(ms * 10000));
    result(c, 1);
}

// ---- status codes to Win32 errors (xboxkrnl_error.cc) ----

struct ErrorTable {
    uint32_t base_code;
    size_t count;
    const uint32_t *entries;
};

// clang-format off
constexpr uint32_t error_table_0x00000103[] = {
    0x3E5, 0, 0xEA, 0x514, 0x515, 0, 0, 0,
    0, 0x3FE, 0x516,
};
constexpr uint32_t error_table_0x40000002[] = {
    0x57, 0, 0, 0, 0x517, 0, 0x460, 0x3F6,
    0, 0, 0x461, 0x518,
};
constexpr uint32_t error_table_0x40020056[] = {
    0x720,
};
constexpr uint32_t error_table_0x400200AF[] = {
    0x779,
};
constexpr uint32_t error_table_0x80000001[] = {
    0x80000001, 0x3E6, 0x80000003, 0x80000004, 0xEA, 0x12, 0, 0,
    0, 0, 0x56F, 0, 0x12B, 0x1C, 0x15, 0x15,
    0xAA, 0x103, 0xFE, 0xFF, 0xFF, 0x456, 0, 0,
    0, 0x103, 0x44D, 0x456, 0x457, 0x44C, 0x44E, 0,
    0x44F, 0x450, 0, 0, 0x962,
};
constexpr uint32_t error_table_0x80000288[] = {
    0x48D, 0x48E,
};
constexpr uint32_t error_table_0x80090300[] = {
    0x5AA, 0x6, 0x1, 0x35, 0x54F, 0x554, 0x120, 0x554,
    0x57, 0x57, 0x32, 0x558, 0x52E, 0x57, 0x520, 0x5,
    0x5, 0x51F, 0, 0, 0, 0, 0x554, 0,
    0x6F8, 0, 0, 0, 0, 0, 0, 0,
    0x57, 0x7A, 0x574, 0, 0, 0x6FE, 0x57, 0x57,
    0x532, 0x1770, 0, 0, 0, 0, 0, 0,
    0x1771, 0x1,
};
constexpr uint32_t error_table_0xC0000001[] = {
    0x1F, 0x1, 0x57, 0x18, 0x3E6, 0x3E7, 0x5AE, 0x6,
    0x3E9, 0xC1, 0x57, 0, 0x57, 0x2, 0x2, 0x1,
    0x26, 0x22, 0x15, 0x6F9, 0x1B, 0xEA, 0x8, 0x1E7,
    0x1E7, 0x57, 0x57, 0x1, 0xC000001D, 0x5, 0x5, 0xC1,
    0x5, 0x5, 0x7A, 0x6, 0xC0000025, 0xC0000026, 0, 0,
    0, 0x9E, 0xC000002B, 0x1E7, 0x1E7, 0, 0, 0x57,
    0, 0x571, 0x7B, 0x2, 0xB7, 0, 0x6, 0,
    0xA1, 0x3, 0xA1, 0x45D, 0x45D, 0x17, 0x17, 0x8,
    0x5, 0x6, 0x20, 0x718, 0x57, 0x120, 0x12A, 0x57,
    0x57, 0x9C, 0x5, 0x57, 0x57, 0x57, 0x11A, 0xFF,
    0x570, 0x570, 0x570, 0x21, 0x21, 0x5, 0x32, 0x519,
    0x51A, 0x51B, 0x51C, 0x51D, 0x51E, 0x51F, 0x520, 0x521,
    0x522, 0x523, 0x524, 0x525, 0x526, 0x527, 0x528, 0x529,
    0x52A, 0x56, 0x52C, 0x52D, 0x52E, 0x52F, 0x530, 0x531,
    0x532, 0x533, 0x534, 0x535, 0x536, 0x537, 0x538, 0x539,
    0x53A, 0x7F, 0xC1, 0x3F0, 0x53C, 0x9E, 0x70, 0x53D,
    0x53E, 0x44, 0x103, 0x53F, 0x103, 0x9A, 0xE, 0x1E7,
    0x714, 0x715, 0x716, 0xC000008C, 0xC000008D, 0xC000008E, 0xC000008F, 0xC0000090,
    0xC0000091, 0xC0000092, 0xC0000093, 0xC0000094, 0x216, 0xC0000096, 0x8, 0x3EE,
    0x540, 0x5AA, 0x3, 0x17, 0x48F, 0x15, 0x1E7, 0x1E7,
    0x5AD, 0x13, 0x15, 0x541, 0x542, 0x543, 0x544, 0x545,
    0x57, 0, 0xE7, 0xE7, 0xE6, 0xE7, 0x1, 0xE9,
    0xE8, 0x217, 0x218, 0xE6, 0x79, 0x26, 0, 0,
    0, 0x5, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x58, 0, 0x11, 0x5, 0xF0, 0x546, 0,
    0xE8, 0x547, 0, 0x548, 0x549, 0x54A, 0x54B, 0x54C,
    0x54D, 0x12C, 0x12D, 0x54E, 0x54F, 0x550, 0x551, 0x6F8,
    0, 0, 0, 0, 0x552, 0x553, 0x57, 0x57,
    0x57, 0x57, 0x57, 0x57, 0x57, 0x57, 0x57, 0x57,
    0x57, 0x57, 0x3, 0, 0x3E9, 0x554, 0, 0xCB,
    0x91, 0x570, 0x10B, 0x555, 0x556, 0xCE, 0x961, 0x964,
    0x13D, 0x5, 0x557, 0, 0x558, 0x420, 0, 0,
    0, 0, 0, 0, 0, 0, 0x5A4, 0,
    0, 0, 0, 0, 0, 0x3EE, 0x4, 0x3E3,
    0x5, 0x4BA, 0x5, 0x55B, 0x55C, 0x55D, 0x55E, 0x6,
    0, 0, 0x55F, 0, 0x5AF, 0, 0, 0xC1,
    0, 0, 0x576, 0, 0x7E, 0, 0, 0xB6,
    0x7F, 0, 0x40, 0x40, 0x33, 0x3B, 0x3B, 0x3B,
    0x3B, 0x45A, 0, 0, 0, 0, 0, 0x7C,
    0x56, 0, 0x6D, 0x3F1, 0x3F8, 0, 0x3ED, 0x45E,
    0x560, 0x561, 0x562, 0x563, 0x564, 0x565, 0x566, 0x567,
    0x3EF, 0x568, 0x569, 0x3F9, 0x56A, 0, 0, 0,
    0, 0x459, 0, 0, 0, 0, 0, 0,
    0x466, 0x467, 0x468, 0x45F, 0, 0, 0, 0,
    0, 0x451, 0x452, 0x453, 0x454, 0x455, 0x469, 0x458,
    0, 0x56B, 0x56C, 0x3FA, 0x3FB, 0x56D, 0x56E, 0x3FC,
    0x3FD, 0x57, 0x45D, 0x16, 0x45D, 0x45D, 0, 0x5DE,
    0x13, 0x6FA, 0x6FB, 0x6FC, 0x6FD, 0x5DC, 0x5DD, 0x6FE,
    0, 0x700, 0x701, 0x46B, 0x4C3, 0x4C4, 0x5DF, 0x70F,
    0x710, 0x711, 0x712,
};
constexpr uint32_t error_table_0xC0000202[] = {
    0x572, 0x3B, 0x717, 0x46A, 0x6F8, 0x4BE, 0x4BE, 0x44,
    0x34, 0x40, 0x40, 0x40, 0x44, 0x3B, 0x3B, 0x3B,
    0x3B, 0x3B, 0x3B, 0x3B, 0x32, 0x32, 0, 0,
    0, 0, 0x17E6, 0, 0, 0, 0x46C, 0xC1,
    0, 0, 0x773, 0x490, 0, 0, 0, 0,
    0xC000022A, 0xC000022B, 0, 0x4D5, 0, 0, 0x492, 0,
    0, 0x774, 0x775, 0x6, 0x4C9, 0x4CA, 0x4CB, 0x4CC,
    0x4CD, 0x4CE, 0x4CF, 0x4D0, 0x4D1, 0x4D2, 0x4D3, 0x4D4,
    0, 0x4C8, 0, 0, 0x4D6, 0x4D7, 0x4D8, 0xC1,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x54F, 0, 0, 0, 0x4D0, 0, 0x573,
    0, 0, 0, 0, 0x422, 0, 0, 0,
    0xB6, 0x7F, 0x120, 0x476, 0, 0x10FE, 0, 0,
    0, 0, 0x7D1, 0x4B1, 0x15, 0, 0, 0,
    0x491, 0, 0, 0x1126, 0x1129, 0x112A, 0x1128, 0x780,
    0, 0, 0, 0, 0, 0, 0x781, 0xA1,
    0, 0x488, 0x489, 0x48A, 0x48B, 0x48C, 0, 0,
    0x5, 0x5, 0, 0x5, 0x5, 0x5, 0x5, 0x1777,
    0x1778, 0x1772, 0, 0, 0, 0, 0, 0,
    0, 0, 0x1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x1127, 0, 0, 0, 0x651, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x575, 0, 0x3E6, 0, 0, 0, 0,
    0x10E8, 0, 0x4E3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x52,
};
constexpr uint32_t error_table_0xC0020001[] = {
    0x6A4, 0x6A5, 0x6, 0x6A7, 0x6A8, 0x6A9, 0x6AA, 0x6AB,
    0x6AC, 0x6AD, 0x6AE, 0x6AF, 0x6B0, 0x6B1, 0x6B2, 0x6B3,
    0x6B4, 0x6B5, 0x6B6, 0x6B7, 0x6B8, 0x6B9, 0x6BA, 0x6BB,
    0x6BC, 0x6BD, 0x6BE, 0x6BF, 0x6C0, 0, 0x6C2, 0,
    0x6C4, 0x6C5, 0x6C6, 0x6C7, 0x6C8, 0x6C9, 0, 0x6CB,
    0x6CC, 0x6CD, 0x6CE, 0x6CF, 0x6D0, 0x6D1, 0x6D2, 0x6D3,
    0x6D4, 0x6D5, 0x6D6, 0x6D7, 0x6D8, 0x6D9, 0x6DA, 0x6DB,
    0x6DC, 0x6DD, 0x6DE, 0x6DF, 0x6E0, 0x6E1, 0x6E2, 0x6E3,
    0x6E4, 0x6E5, 0x6E6, 0x6E7, 0x6E8, 0x6E9, 0x6EA, 0x6EB,
    0x6FF, 0x70E, 0x76A, 0x76B, 0x76C, 0, 0x719, 0x71A,
    0x71B, 0x71C, 0x71D, 0x71E, 0x71F, 0, 0x721, 0x722,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x77A, 0x77B,
};
constexpr uint32_t error_table_0xC0030001[] = {
    0x6EC, 0x6ED, 0x6EE, 0x6, 0x6, 0x6F1, 0x6F2, 0x6F3,
    0x6F4, 0x6F5, 0x6F6, 0x6F7,
};
constexpr uint32_t error_table_0xC0030059[] = {
    0x723, 0x724, 0x725, 0x726, 0, 0x728, 0x77C, 0x77D,
    0x77E,
};
constexpr uint32_t error_table_0xC0050003[] = {
    0x45D, 0x456,
};
constexpr uint32_t error_table_0xC0980001[] = {
    0x37, 0x37, 0, 0, 0, 0, 0, 0x37,
};
#define XF_ERROR_ENTRY(x) {x, std::size(error_table_##x), error_table_##x}
constexpr ErrorTable error_tables[] = {
    XF_ERROR_ENTRY(0x00000103), XF_ERROR_ENTRY(0x40000002), XF_ERROR_ENTRY(0x40020056), XF_ERROR_ENTRY(0x400200AF),
    XF_ERROR_ENTRY(0x80000001), XF_ERROR_ENTRY(0x80000288), XF_ERROR_ENTRY(0x80090300), XF_ERROR_ENTRY(0xC0000001),
    XF_ERROR_ENTRY(0xC0000202), XF_ERROR_ENTRY(0xC0020001), XF_ERROR_ENTRY(0xC0030001), XF_ERROR_ENTRY(0xC0030059),
    XF_ERROR_ENTRY(0xC0050003), XF_ERROR_ENTRY(0xC0980001),
    {0, 0, nullptr},
};
#undef XF_ERROR_ENTRY

// xeRtlNtStatusToDosError
uint32_t nt_status_to_dos_error(uint32_t source_status) {
    uint32_t status = source_status;
    if (!status || (status & 0x20000000)) return status;
    if ((status >> 16) == 0x8007) return status & 0xFFFF;
    // the high bits do not matter
    if ((status & 0xF0000000) == 0xD0000000) status &= ~0x30000000u;
    for (const ErrorTable *table = error_tables; table->base_code; ++table) {
        if (status < table->base_code) break;
        uint32_t index = status - table->base_code;
        if (index < table->count) {
            uint32_t error = table->entries[index];
            if (!error) break;
            return error;
        }
    }
    if ((status >> 16) == 0xC001) return status & 0xFFFF;
    std::fprintf(stderr, "kernel: RtlNtStatusToDosError: no error for %08X\n", source_status);
    return 317; // ERROR_MR_MID_NOT_FOUND
}
// clang-format on

XF_EXPORT(xboxkrnl, RtlNtStatusToDosError) { result(c, nt_status_to_dos_error(arg(k, c, 0))); }

// ---- the printf family (xboxkrnl_strings.cc) ----

// the CRT's, as MSDN describes it ("Format Specification Syntax: printf and
// wprintf Functions"), assumed to be the console's
enum FormatState {
    FS_Invalid = 0,
    FS_Unknown,
    FS_Start,
    FS_Flags,
    FS_Width,
    FS_PrecisionStart,
    FS_Precision,
    FS_Size,
    FS_Type,
    FS_End,
};

enum FormatFlags {
    FF_LeftJustify = 1 << 0,
    FF_AddLeadingZeros = 1 << 1,
    FF_AddPositive = 1 << 2,
    FF_AddPositiveAsSpace = 1 << 3,
    FF_AddNegative = 1 << 4,
    FF_AddPrefix = 1 << 5,
    FF_IsShort = 1 << 6,
    FF_IsLong = 1 << 7,
    FF_IsLongLong = 1 << 8,
    FF_IsWide = 1 << 9,
    FF_IsSigned = 1 << 10,
    FF_ForceLeadingZero = 1 << 11,
    FF_InvertWide = 1 << 12,
};

// the format string in guest memory (narrow, or wide big-endian) and what it
// makes (StringFormatData, WideStringFormatData)
class FormatData {
  public:
    FormatData(Kernel &k, uint32_t input, bool wide) : k_(k), input_(input), wide_(wide) {}
    uint16_t get() {
        uint16_t result = peek(0);
        if (result) input_ += wide_ ? 2 : 1;
        return result;
    }
    uint16_t peek(int32_t offset) { return wide_ ? k_.load16(input_ + 2 * offset) : k_.host(input_ + offset)[0]; }
    void skip(int32_t count) {
        while (count-- > 0)
            if (!get()) break;
    }
    bool put(uint16_t c) {
        if (!wide_ && c >= 0x100) return false;
        output_.push_back(char16_t(c));
        return true;
    }
    std::string str() const { return std::string(output_.begin(), output_.end()); }
    const std::u16string &wstr() const { return output_; }

  private:
    Kernel &k_;
    uint32_t input_;
    bool wide_;
    std::u16string output_;
};

// the arguments: the call's (StackArgList) or an array of 8-byte slots
// (ArrayArgList, a va_list)
class ArgList {
  public:
    ArgList(Kernel &k, ppc::Context &c, int32_t index) : k_(k), c_(c), index_(index) {}
    ArgList(Kernel &k, ppc::Context &c, uint32_t array, bool) : k_(k), c_(c), array_(array), from_array_(true) {}
    uint32_t get32() { return uint32_t(get64()); }
    uint64_t get64() { return from_array_ ? k_.load64(array_ + 8 * index_++) : arg64(k_, c_, index_++); }

  private:
    Kernel &k_;
    ppc::Context &c_;
    uint32_t array_ = 0;
    bool from_array_ = false;
    int32_t index_ = 0;
};

std::string format_double(double value, int32_t precision, uint16_t c, uint32_t flags) {
    if (precision < 0)
        precision = 6;
    else if (precision == 0 && c == 'g')
        precision = 1;
    std::ostringstream temp;
    temp << std::setprecision(precision);
    if (c == 'f')
        temp << std::fixed;
    else if (c == 'e' || c == 'E')
        temp << std::scientific;
    else if (c == 'a' || c == 'A')
        temp << std::hexfloat;
    else if (c == 'g' || c == 'G')
        temp << std::defaultfloat;
    if (c == 'E' || c == 'G' || c == 'A') temp << std::uppercase;
    if (flags & FF_AddPrefix) temp << std::showpoint;
    temp << value;
    return temp.str();
}

// format_core, as Xenia's (its state machine and its quirks)
int32_t format_core(Kernel &k, FormatData &data, ArgList &args, const bool wide) {
    int32_t count = 0;
    char work8[512];
    char16_t work16[4];
    struct {
        const void *buffer; // host memory: guest memory for a string argument
        int32_t length;
        bool is_wide;
        bool swap_wide; // a wide string in guest memory (big-endian)
    } text;
    struct {
        char buffer[2];
        int32_t length;
    } prefix;

    auto state = FS_Unknown;
    uint32_t flags = 0;
    int32_t width = 0;
    int32_t precision = -1;
    int32_t radix = 0;
    const char *digits = nullptr;

    text.buffer = nullptr;
    text.is_wide = false;
    text.swap_wide = true;
    text.length = 0;
    prefix.buffer[0] = '\0';
    prefix.length = 0;

    for (uint16_t c = data.get();; c = data.get()) {
        if (state == FS_Unknown) {
            if (!c) { // the end
                return count;
            } else if (c != '%') {
            output:
                if (!data.put(c)) return -1;
                ++count;
                continue;
            }
            state = FS_Start;
            c = data.get();
        }

        // in any state, \0 is bad
        if (!c) return -1;

    restart:
        switch (state) {
        case FS_Invalid:
        case FS_Unknown:
        case FS_End:
        default:
            // (Xenia asserts here)
            [[fallthrough]];

        case FS_Start: {
            if (c == '%') {
                state = FS_Unknown;
                goto output;
            }
            state = FS_Flags;
            // the defaults
            flags = 0;
            width = 0;
            precision = -1;
            radix = 0;
            digits = nullptr;
            text.buffer = nullptr;
            text.is_wide = false;
            text.swap_wide = true;
            text.length = 0;
            prefix.buffer[0] = '\0';
            prefix.length = 0;
            [[fallthrough]];
        }

        case FS_Flags: {
            if (c == '-') {
                flags |= FF_LeftJustify;
                continue;
            } else if (c == '+') {
                flags |= FF_AddPositive;
                continue;
            } else if (c == '0') {
                flags |= FF_AddLeadingZeros;
                continue;
            } else if (c == ' ') {
                flags |= FF_AddPositiveAsSpace;
                continue;
            } else if (c == '#') {
                flags |= FF_AddPrefix;
                continue;
            }
            state = FS_Width;
            [[fallthrough]];
        }

        case FS_Width: {
            if (c == '*') {
                width = int32_t(args.get32());
                if (width < 0) {
                    flags |= FF_LeftJustify;
                    width = -width;
                }
                state = FS_PrecisionStart;
                continue;
            } else if (c >= '0' && c <= '9') {
                width = width * 10 + (c - '0');
                continue;
            }
            state = FS_PrecisionStart;
            [[fallthrough]];
        }

        case FS_PrecisionStart: {
            if (c == '.') {
                state = FS_Precision;
                precision = 0;
                continue;
            }
            state = FS_Size;
            goto restart;
        }

        case FS_Precision: {
            if (c == '*') {
                precision = int32_t(args.get32());
                if (precision < 0) precision = -1;
                state = FS_Size;
                continue;
            } else if (c >= '0' && c <= '9') {
                precision = precision * 10 + (c - '0');
                continue;
            }
            state = FS_Size;
            [[fallthrough]];
        }

        case FS_Size: {
            if (c == 'l') {
                if (data.peek(0) == 'l') {
                    // "ll": the console reads 32 bits (the oracle: "%lld" of
                    // 0x100000000 prints 0, of 0xFFFFFFFF prints -1); only
                    // "%I64" reads 64. Xenia reads 64 for both.
                    data.skip(1);
                    flags |= FF_IsLong;
                } else {
                    flags |= FF_IsLong;
                }
                state = FS_Type;
                continue;
            } else if (c == 'L') {
                // (a title uses 'L' for 'l'; xboxkrnl may take it as invalid)
                state = FS_Type;
                continue;
            } else if (c == 'h') {
                flags |= FF_IsShort;
                state = FS_Type;
                continue;
            } else if (c == 'w') {
                flags |= FF_IsWide;
                state = FS_Type;
                continue;
            } else if (c == 'I') {
                if (data.peek(0) == '6' && data.peek(1) == '4') {
                    data.skip(2);
                    flags |= FF_IsLongLong;
                } else if (data.peek(0) == '3' && data.peek(1) == '2') {
                    data.skip(2);
                }
                state = FS_Type;
                continue;
            }
            [[fallthrough]];
        }

        case FS_Type: {
            switch (c) {
            // wide character
            case 'C': {
                flags |= FF_InvertWide;
                [[fallthrough]];
            }
            // character
            case 'c': {
                bool is_wide;
                if (flags & (FF_IsLong | FF_IsWide))
                    is_wide = true; // lc, lC, wc, wC: C in printf
                else if (flags & FF_IsShort)
                    is_wide = false; // hc, hC: c in printf
                else
                    is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
                auto value = args.get32();
                if (!is_wide) {
                    work8[0] = char(uint8_t(value));
                    text.buffer = &work8[0];
                    text.length = 1;
                    text.is_wide = false;
                } else {
                    work16[0] = char16_t(uint16_t(value));
                    text.buffer = &work16[0];
                    text.length = 1;
                    text.is_wide = true;
                    text.swap_wide = false;
                }
                break;
            }

            // signed decimal integer
            case 'd':
            case 'i': {
                flags |= FF_IsSigned;
                digits = "0123456789";
                radix = 10;

            integer:
                int64_t value;
                if (flags & FF_IsLongLong)
                    value = int64_t(args.get64());
                else if (flags & FF_IsLong)
                    value = int32_t(args.get32());
                else if (flags & FF_IsShort)
                    value = int16_t(args.get32());
                else
                    value = int32_t(args.get32());

                if (precision >= 0)
                    precision = std::min(precision, int32_t(sizeof(work8)));
                else
                    precision = 1;

                if ((flags & FF_IsSigned) && value < 0) {
                    value = int64_t(0 - uint64_t(value));
                    flags |= FF_AddNegative;
                }
                if (!(flags & FF_IsLongLong)) value &= UINT32_MAX;
                if (value == 0) prefix.length = 0;

                char *end = &work8[sizeof(work8) - 1];
                char *start = end;
                start[0] = '\0';
                while (precision-- > 0 || value != 0) {
                    const auto digit = uint64_t(value) % uint64_t(radix);
                    value /= radix;
                    *--start = digits[digit];
                }
                if ((flags & FF_ForceLeadingZero) && (start == end || *start != '0')) *--start = '0';

                text.buffer = start;
                text.length = int32_t(end - start);
                text.is_wide = false;
                break;
            }

            // unsigned octal integer
            case 'o': {
                digits = "01234567";
                radix = 8;
                if (flags & FF_AddPrefix) flags |= FF_ForceLeadingZero;
                goto integer;
            }

            // unsigned decimal integer
            case 'u': {
                digits = "0123456789";
                radix = 10;
                goto integer;
            }

            // unsigned hexadecimal integer
            case 'x':
            case 'X': {
                digits = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
                radix = 16;
                if (flags & FF_AddPrefix) {
                    prefix.buffer[0] = '0';
                    prefix.buffer[1] = c == 'x' ? 'x' : 'X';
                    prefix.length = 2;
                }
                goto integer;
            }

            // floating point with exponent, without, with or without, in hex
            case 'e':
            case 'E':
            case 'f': {
            floatingpoint:
                flags |= FF_IsSigned;
                uint64_t bits = args.get64();
                double value;
                std::memcpy(&value, &bits, sizeof(value));
                if (value < 0) {
                    value = -value;
                    flags |= FF_AddNegative;
                }
                std::string s = format_double(value, precision, c, flags);
                int32_t length = int32_t(std::min(s.size(), sizeof(work8) - 1));
                std::memcpy(work8, s.c_str(), size_t(length));
                work8[length] = '\0';
                text.buffer = &work8[0];
                text.length = length;
                text.is_wide = false;
                break;
            }
            case 'g':
            case 'G':
            case 'a':
            case 'A': goto floatingpoint;

            // pointer to an integer
            case 'n': {
                auto pointer = args.get32();
                if (flags & FF_IsShort)
                    k.store16(pointer, uint16_t(count));
                else
                    k.store32(pointer, uint32_t(count));
                // (the state stays: Xenia's)
                continue;
            }

            // pointer
            case 'p': {
                digits = "0123456789ABCDEF";
                radix = 16;
                precision = 8;
                flags &= ~uint32_t(FF_IsLongLong | FF_IsShort);
                flags |= FF_IsLong;
                goto integer;
            }

            // wide string
            case 'S': {
                flags |= FF_InvertWide;
                [[fallthrough]];
            }
            // string
            case 's': {
                uint32_t pointer = args.get32();
                int32_t cap = precision < 0 ? INT32_MAX : precision;
                if (pointer == 0) {
                    static const char nullstr[] = "(null)";
                    text.buffer = nullstr;
                    text.length = std::min(int32_t(sizeof(nullstr) - 1), cap);
                    text.is_wide = false;
                } else {
                    bool is_wide;
                    if (flags & (FF_IsLong | FF_IsWide))
                        is_wide = true; // ls, lS, ws, wS: S in printf
                    else if (flags & FF_IsShort)
                        is_wide = false; // hs, hS: s in printf
                    else
                        is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
                    int32_t length = 0;
                    if (!is_wide) {
                        for (uint32_t s = pointer; cap > 0 && k.host(s)[0]; ++s, cap--) length++;
                    } else {
                        for (uint32_t s = pointer; cap > 0 && k.load16(s); s += 2, cap--) length++;
                    }
                    text.buffer = k.host(pointer);
                    text.length = length;
                    text.is_wide = is_wide;
                }
                break;
            }

            // ANSI_STRING / UNICODE_STRING (Xenia asserts)
            case 'Z': break;

            default: break; // (Xenia asserts)
            }
        }
        }

        if (flags & FF_IsSigned) {
            if (flags & FF_AddNegative) {
                prefix.buffer[0] = '-';
                prefix.length = 1;
            } else if (flags & FF_AddPositive) {
                prefix.buffer[0] = '+';
                prefix.length = 1;
            } else if (flags & FF_AddPositiveAsSpace) {
                prefix.buffer[0] = ' ';
                prefix.length = 1;
            }
        }

        int32_t padding = width - text.length - prefix.length;

        if (!(flags & (FF_LeftJustify | FF_AddLeadingZeros)) && padding > 0) {
            count += padding;
            while (padding-- > 0)
                if (!data.put(' ')) return -1;
        }

        if (prefix.length > 0) {
            int32_t remaining = prefix.length;
            count += prefix.length;
            const char *b = &prefix.buffer[0];
            while (remaining-- > 0)
                if (!data.put(uint8_t(*b++))) return -1;
        }

        if ((flags & FF_AddLeadingZeros) && !(flags & FF_LeftJustify) && padding > 0) {
            count += padding;
            while (padding-- > 0)
                if (!data.put('0')) return -1;
        }

        int32_t remaining = text.length;
        if (!text.is_wide) {
            auto b = static_cast<const uint8_t *>(text.buffer);
            while (remaining-- > 0)
                if (!data.put(*b++)) return -1;
        } else if (text.swap_wide) {
            // a guest string: big-endian
            auto b = static_cast<const uint8_t *>(text.buffer);
            while (remaining-- > 0) {
                if (!data.put(uint16_t(b[0] << 8 | b[1]))) return -1;
                b += 2;
            }
        } else {
            auto b = static_cast<const char16_t *>(text.buffer);
            while (remaining-- > 0)
                if (!data.put(*b++)) return -1;
        }
        count += text.length;

        // right padding
        if ((flags & FF_LeftJustify) && padding > 0) {
            count += padding;
            while (padding-- > 0)
                if (!data.put(' ')) return -1;
        }

        state = FS_Unknown;
    }
}

// what fits of the text at buffer: count characters, terminated when there
// is room (_snprintf, _vsnprintf)
int32_t write_bounded(Kernel &k, uint32_t buffer, uint32_t buffer_count, int32_t count, const std::string &text) {
    uint8_t *out = k.host(buffer);
    if (count < 0) {
        out[0] = '\0'; // a null, to be safe
    } else if (uint32_t(count) <= buffer_count) {
        std::memcpy(out, text.data(), size_t(count));
        if (uint32_t(count) < buffer_count) out[count] = '\0';
    } else {
        std::memcpy(out, text.data(), buffer_count);
        count = -1;
    }
    return count;
}

// the text at buffer, terminated (sprintf, vsprintf)
int32_t write_unbounded(Kernel &k, uint32_t buffer, int32_t count, const std::string &text) {
    uint8_t *out = k.host(buffer);
    if (count <= 0) {
        out[0] = '\0';
    } else {
        std::memcpy(out, text.data(), size_t(count));
        out[count] = '\0';
    }
    return count;
}

XF_EXPORT(xboxkrnl, DbgPrint) {
    // (format, ...): logged, trailing whitespace trimmed
    uint32_t format = arg(k, c, 0);
    if (!format) return result(c, X_STATUS_INVALID_PARAMETER);
    ArgList args(k, c, 1);
    FormatData data(k, format, false);
    int32_t count = format_core(k, data, args, false);
    if (count > 0) {
        std::string text = data.str();
        while (!text.empty() && std::isspace(uint8_t(text.back()))) text.pop_back();
        std::fprintf(stderr, "DbgPrint: %s\n", text.c_str());
    }
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, _snprintf) {
    // (buffer, buffer_count, format, ...)
    uint32_t buffer = arg(k, c, 0), buffer_count = arg(k, c, 1), format = arg(k, c, 2);
    if (!buffer || !buffer_count || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, 3);
    FormatData data(k, format, false);
    int32_t count = format_core(k, data, args, false);
    result(c, uint32_t(write_bounded(k, buffer, buffer_count, count, data.str())));
}

XF_EXPORT(xboxkrnl, sprintf) {
    // (buffer, format, ...)
    uint32_t buffer = arg(k, c, 0), format = arg(k, c, 1);
    if (!buffer || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, 2);
    FormatData data(k, format, false);
    int32_t count = format_core(k, data, args, false);
    result(c, uint32_t(write_unbounded(k, buffer, count, data.str())));
}

XF_EXPORT(xboxkrnl, _vsnprintf) {
    // (buffer, buffer_count, format, va_list)
    uint32_t buffer = arg(k, c, 0), buffer_count = arg(k, c, 1), format = arg(k, c, 2), arg_ptr = arg(k, c, 3);
    if (!buffer || !buffer_count || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, arg_ptr, true);
    FormatData data(k, format, false);
    int32_t count = format_core(k, data, args, false);
    result(c, uint32_t(write_bounded(k, buffer, buffer_count, count, data.str())));
}

XF_EXPORT(xboxkrnl, vsprintf) {
    // (buffer, format, va_list)
    uint32_t buffer = arg(k, c, 0), format = arg(k, c, 1), arg_ptr = arg(k, c, 2);
    if (!buffer || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, arg_ptr, true);
    FormatData data(k, format, false);
    int32_t count = format_core(k, data, args, false);
    result(c, uint32_t(write_unbounded(k, buffer, count, data.str())));
}

// the text at buffer as wide characters, big-endian
void put_wide(Kernel &k, uint32_t buffer, const std::u16string &text, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) k.store16(buffer + 2 * i, uint16_t(text[i]));
}

XF_EXPORT(xboxkrnl, _snwprintf) {
    // (buffer, buffer_count, format, ...): what fits, -1 when it does not
    uint32_t buffer = arg(k, c, 0), buffer_count = arg(k, c, 1), format = arg(k, c, 2);
    if (!buffer || !buffer_count || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, 3);
    FormatData data(k, format, true);
    int32_t count = format_core(k, data, args, true);
    if (count < 0) {
        k.store16(buffer, 0);
    } else if (uint32_t(count) <= buffer_count) {
        put_wide(k, buffer, data.wstr(), uint32_t(count));
        if (uint32_t(count) < buffer_count) k.store16(buffer + 2 * uint32_t(count), 0);
    } else {
        put_wide(k, buffer, data.wstr(), buffer_count);
        count = -1;
    }
    result(c, uint32_t(count));
}

XF_EXPORT(xboxkrnl, _vsnwprintf) {
    // (buffer, buffer_count, format, va_list): what fits; the whole text's
    // count even when it does not (Xenia's, unlike _snwprintf)
    uint32_t buffer = arg(k, c, 0), buffer_count = arg(k, c, 1), format = arg(k, c, 2), arg_ptr = arg(k, c, 3);
    if (!buffer || !buffer_count || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, arg_ptr, true);
    FormatData data(k, format, true);
    int32_t count = format_core(k, data, args, true);
    if (count < 0) {
        k.store16(buffer, 0);
    } else if (uint32_t(count) <= buffer_count) {
        put_wide(k, buffer, data.wstr(), uint32_t(count));
        if (uint32_t(count) < buffer_count) k.store16(buffer + 2 * uint32_t(count), 0);
    } else {
        put_wide(k, buffer, data.wstr(), buffer_count);
    }
    result(c, uint32_t(count));
}

// the text at buffer, terminated (swprintf, vswprintf)
int32_t write_wide_unbounded(Kernel &k, uint32_t buffer, int32_t count, const std::u16string &text) {
    if (count <= 0) {
        k.store16(buffer, 0);
    } else {
        put_wide(k, buffer, text, uint32_t(count));
        k.store16(buffer + 2 * uint32_t(count), 0);
    }
    return count;
}

XF_EXPORT(xboxkrnl, swprintf) {
    // (buffer, format, ...)
    uint32_t buffer = arg(k, c, 0), format = arg(k, c, 1);
    if (!buffer || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, 2);
    FormatData data(k, format, true);
    int32_t count = format_core(k, data, args, true);
    result(c, uint32_t(write_wide_unbounded(k, buffer, count, data.wstr())));
}

XF_EXPORT(xboxkrnl, vswprintf) {
    // (buffer, format, va_list)
    uint32_t buffer = arg(k, c, 0), format = arg(k, c, 1), arg_ptr = arg(k, c, 2);
    if (!buffer || !format) return result(c, uint32_t(-1));
    ArgList args(k, c, arg_ptr, true);
    FormatData data(k, format, true);
    int32_t count = format_core(k, data, args, true);
    result(c, uint32_t(write_wide_unbounded(k, buffer, count, data.wstr())));
}

XF_EXPORT(xboxkrnl, _vscwprintf) {
    // (format, va_list): how many characters the text has
    uint32_t format = arg(k, c, 0), arg_ptr = arg(k, c, 1);
    if (!format) return result(c, uint32_t(-1));
    ArgList args(k, c, arg_ptr, true);
    FormatData data(k, format, true);
    result(c, uint32_t(format_core(k, data, args, true)));
}

// ---- checksums, images, assertions (xboxkrnl_rtl.cc) ----

// The CRC-32 of IEEE 802.3 (ISO-HDLC): the polynomial 0x04C11DB7 reflected,
// one table entry per byte value, made from its definition
constexpr std::array<uint32_t, 256> crc32_table = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t r = n;
        for (int bit = 0; bit < 8; bit++) r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;
        table[n] = r;
    }
    return table;
}();

XF_EXPORT(xboxkrnl, RtlComputeCrc32) {
    // (seed, buffer, length): the CRC continued from the seed (a previous
    // result, 0 to start)
    uint32_t seed = arg(k, c, 0), buffer = arg(k, c, 1), length = arg(k, c, 2);
    if (!length) return result(c, seed);
    uint32_t crc = ~seed;
    const uint8_t *p = k.host(buffer);
    for (uint32_t i = 0; i < length; i++) crc = crc32_table[(p[i] ^ crc) & 0xFF] ^ (crc >> 8);
    result(c, ~crc);
}

// PE structures are little-endian
uint16_t le16(Kernel &k, uint32_t a) { return uint16_t(k.host(a)[0] | k.host(a)[1] << 8); }
uint32_t le32(Kernel &k, uint32_t a) { return uint32_t(le16(k, a)) | uint32_t(le16(k, a + 2)) << 16; }

// the IMAGE_NT_HEADERS of a module at base: after "MZ", at e_lfanew, the
// "PE" signature; 0 if none
uint32_t image_nt_header(Kernel &k, uint32_t base) {
    if (!base || le16(k, base) != 0x5A4D) return 0;
    uint32_t nt = base + le32(k, base + 0x3C);
    return le32(k, nt) == 0x4550 ? nt : 0;
}

XF_EXPORT(xboxkrnl, RtlImageNtHeader) { result(c, image_nt_header(k, arg(k, c, 0))); }

XF_EXPORT(xboxkrnl, RtlImageDirectoryEntryToData) {
    // (base, mapped as image, directory entry, size out): the entry's data
    // and its size. Base's low bit says the image is a file as read, not
    // mapped: then the data is found through the section that holds it, at
    // its offset in the file (NT's RtlAddressInSectionTable; Xenia leaves out
    // the section's PointerToRawData).
    uint32_t base = arg(k, c, 0), size_ptr = arg(k, c, 3);
    bool mapped = uint8_t(arg(k, c, 1)) != 0;
    uint16_t entry = uint16_t(arg(k, c, 2));
    if (base & 1) base &= ~1u, mapped = false;
    uint32_t nt = image_nt_header(k, base);
    if (!nt) return result(c, 0);
    uint32_t optional = nt + 0x18;
    if (le16(k, optional) != 0x10B) return result(c, 0);        // IMAGE_NT_OPTIONAL_HDR32_MAGIC
    if (entry >= le32(k, optional + 0x5C)) return result(c, 0); // NumberOfRvaAndSizes
    uint32_t directory = optional + 0x60 + 8 * entry;
    uint32_t address = le32(k, directory);
    if (!address) return result(c, 0);
    k.store32(size_ptr, le32(k, directory + 4));
    if (mapped || address < le32(k, optional + 0x3C)) return result(c, base + address); // SizeOfHeaders
    uint32_t sections = le16(k, nt + 6), section = optional + le16(k, nt + 0x14);
    for (uint32_t i = 0; i < sections; i++, section += 0x28) {
        uint32_t virtual_address = le32(k, section + 0xC), raw_size = le32(k, section + 0x10);
        if (address >= virtual_address && address < virtual_address + raw_size)
            return result(c, base + address - virtual_address + le32(k, section + 0x14));
    }
    result(c, 0);
}

XF_EXPORT(xboxkrnl, RtlRip) {
    // (message, message): an assertion's text; logged, and the code goes on
    // (Xenia's)
    auto text = [&](uint32_t address) {
        if (!address) return std::string();
        const char *s = reinterpret_cast<const char *>(k.host(address));
        return std::string(s, strnlen(s, 1024));
    };
    std::fprintf(stderr, "kernel: RtlRip called, arg1 = %s, arg2 = %s\n", text(arg(k, c, 0)).c_str(),
                 text(arg(k, c, 1)).c_str());
}

} // namespace
} // namespace xf::kernel
