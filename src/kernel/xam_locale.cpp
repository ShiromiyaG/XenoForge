/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's locale and strings: the countries, languages and locales and their
// tables, the console's country and language as XAM reads them, dates and
// times formatted, the title's own strings (its SPA's string tables), the
// resource locators, the language's font paths and the online schema.
// Translated from Xenia (BSD; see LICENSE-xenia): xam_locale.cc,
// xam_info.cc (the resource locators, XamReadString,
// XamLookupCommonStringByIndex), xam_state.cc (the
// language fallbacks and typeface patches), xdbf_io.cc and spa_info.cc (the
// SPA's string tables), game_info_database.cc (GetLocalizedString), xbox.h
// (the online countries' features); checked against them.
//
// Where it differs from Xenia, and why:
//   - XamGetCountry answers the console's country: Xenia reads the one-byte
//     setting as a word, which its XConfig refuses, and answers 0;
//   - an offline profile has no country or language of its own (its account
//     flags are zero), so XamGetOnlineLanguageAndCountry,
//     XamProfileGetLiveLegalLocale and XapipGetLocale take the console's, as
//     Xenia's XamUserGetOnlineCountryFromXUID does (Xenia answers "zz-ZZ"
//     there);
//   - the language fallback and typeface strings live in the system heap,
//     not at Xenia's fixed 0x80D00000 and 0x80D10000; a language past the
//     table has none (0), where Xenia reads past its array;
//   - the settings are read and written through ExGetXConfigSetting and
//     ExSetXConfigSetting, the console's settings being misc.cpp's.

#include "xam.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace xf::kernel {
namespace {

// ---- the console's settings (xconfig.h: XCONFIG_USER_CATEGORY) ----

constexpr uint16_t CONFIG_USER = 3, CONFIG_LANGUAGE = 0x09, CONFIG_COUNTRY = 0x0E;

struct LocaleState {
    std::mutex mutex;
    uint32_t buffer = 0; // guest memory the settings go through
    // the language fallbacks and typeface patches (xam_state.cc), made once
    uint32_t fallbacks[18] = {}, typeface_patches[7] = {};
    bool strings_made = false;
    // the title's SPA strings, in its default language, read once
    std::optional<std::map<uint16_t, std::u16string>> spa_strings;
};

LocaleState &locale(Kernel &k) { return k.state<LocaleState>(); }

uint32_t settings_buffer(Kernel &k, LocaleState &s) {
    if (!s.buffer) s.buffer = k.memory().system_alloc(16);
    return s.buffer;
}

// XConfig::ReadSetting: the setting's bytes, size of them (0 where it has
// another size, as Xenia's ReadSetting<T>)
uint32_t read_setting(Kernel &k, uint16_t id, uint32_t size) {
    static const ExportFn get = find_export_fn("xboxkrnl", "ExGetXConfigSetting");
    LocaleState &s = locale(k);
    std::lock_guard<std::mutex> l(s.mutex);
    uint32_t buffer = settings_buffer(k, s);
    if (!get || !buffer) return 0;
    ppc::Context c{};
    k.attach(c);
    c.r[3] = CONFIG_USER, c.r[4] = id, c.r[5] = buffer, c.r[6] = 8, c.r[7] = buffer + 8;
    get(k, c);
    if (uint32_t(c.r[3]) != X_STATUS_SUCCESS || k.load16(buffer + 8) != size) return 0;
    return size == 1 ? k.host(buffer)[0] : k.load32(buffer);
}

// XConfig::WriteSetting of the one-byte country, through ExSetXConfigSetting
void write_country(Kernel &k, uint8_t country) {
    static const ExportFn set = find_export_fn("xboxkrnl", "ExSetXConfigSetting");
    LocaleState &s = locale(k);
    std::lock_guard<std::mutex> l(s.mutex);
    uint32_t buffer = settings_buffer(k, s);
    if (!set || !buffer) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            std::fprintf(stderr, "xam: XamSetCountry: the console's settings cannot be written here\n");
        return;
    }
    k.host(buffer)[0] = country;
    ppc::Context c{};
    k.attach(c);
    c.r[3] = CONFIG_USER, c.r[4] = CONFIG_COUNTRY, c.r[5] = buffer, c.r[6] = 1;
    set(k, c);
}

// xeXGetGameRegion (misc.cpp's XGetGameRegion)
uint32_t game_region(Kernel &k) {
    static const ExportFn get = find_export_fn("xam", "XGetGameRegion");
    if (!get) return 0xFFFF;
    ppc::Context c{};
    k.attach(c);
    get(k, c);
    return uint32_t(c.r[3]);
}

} // namespace

uint8_t config_country(Kernel &k) { return uint8_t(read_setting(k, CONFIG_COUNTRY, 1)); }
uint32_t config_language(Kernel &k) { return read_setting(k, CONFIG_LANGUAGE, 4); }

namespace {

// ---- the tables (xam_locale.cc) ----

// XLanguage
constexpr uint32_t LANGUAGE_ENGLISH = 1, LANGUAGE_JAPANESE = 2, LANGUAGE_KOREAN = 7, LANGUAGE_TCHINESE = 8;
constexpr uint32_t LANGUAGE_SWEDISH = 13, LANGUAGE_SCHINESE = 17, MAX_LANGUAGES = 18;

template <class T, size_t N> T at_or(const T (&table)[N], uint8_t id, T fallback) {
    return id < N ? table[id] : fallback;
}

uint8_t online_country_from_locale(uint8_t id) {
    static constexpr uint8_t table[] = {
        2,  6,  5,  8,  13, 16, 19, 20, 21,  23, 25, 32, 34,  24, 37,  39, 42,  46, 44, 50, 53, 56,
        71, 74, 76, 75, 82, 84, 91, 93, 109, 31, 90, 18, 101, 35, 103, 88, 236, 99, 4,  89, 45, 1,
    };
    // locale 0 (none) is online country 0 on the console's XAM (xfxam; every
    // other entry is the same); Xenia's table answers 2 for it
    return id ? at_or(table, id, uint8_t(0)) : uint8_t(0);
}

const char16_t *online_country_string(uint8_t id) {
    static constexpr const char16_t *table[] = {
        u"ZZ",   u"AE", u"AL",   u"AM",   u"AR",   u"AT", u"AU", u"AZ", u"BE",   u"BG", u"BH",   u"BN", u"BO",
        u"BR",   u"BY", u"BZ",   u"CA",   nullptr, u"CH", u"CL", u"CN", u"CO",   u"CR", u"CZ",   u"DE", u"DK",
        u"DO",   u"DZ", u"EC",   u"EE",   u"EG",   u"ES", u"FI", u"FO", u"FR",   u"GB", u"GE",   u"GR", u"GT",
        u"HK",   u"HN", u"HR",   u"HU",   u"ID",   u"IE", u"IL", u"IN", u"IQ",   u"IR", u"IS",   u"IT", u"JM",
        u"JO",   u"JP", u"KE",   u"KG",   u"KR",   u"KW", u"KZ", u"LB", u"LI",   u"LT", u"LU",   u"LV", u"LY",
        u"MA",   u"MC", u"MK",   u"MN",   u"MO",   u"MV", u"MX", u"MY", u"NI",   u"NL", u"NO",   u"NZ", u"OM",
        u"PA",   u"PE", u"PH",   u"PK",   u"PL",   u"PR", u"PT", u"PY", u"QA",   u"RO", u"RU",   u"SA", u"SE",
        u"SG",   u"SI", u"SK",   nullptr, u"SV",   u"SY", u"TH", u"TN", u"TR",   u"TT", u"TW",   u"UA", u"US",
        u"UY",   u"UZ", u"VE",   u"VN",   u"YE",   u"ZA", u"ZW", u"AF", nullptr, u"AD", u"AO",   u"AI", nullptr,
        u"AG",   u"AW", u"BS",   u"BD",   u"BB",   u"BJ", u"BM", u"BT", u"BA",   u"BW", u"BF",   u"BI", u"KH",
        u"CM",   u"CV", u"KY",   u"CF",   u"TD",   u"CX", u"CC", u"KM", u"CG",   u"CD", u"CK",   u"CI", u"CY",
        u"DJ",   u"DM", nullptr, u"GQ",   u"ER",   u"ET", u"FK", u"FJ", u"GF",   u"PF", u"GA",   u"GM", u"GH",
        u"GI",   u"GL", u"GD",   u"GP",   nullptr, u"GG", u"GN", u"GW", u"GY",   u"HT", u"JE",   u"KI", u"LA",
        u"LS",   u"LR", u"MG",   u"MW",   u"ML",   u"MT", u"MH", u"MQ", u"MR",   u"MU", u"YT",   u"FM", u"MD",
        u"ME",   u"MS", u"MZ",   u"MM",   u"NA",   u"NR", u"NP", u"AN", u"NC",   u"NE", u"NG",   u"NU", u"NF",
        nullptr, u"PW", u"PS",   u"PG",   u"PN",   u"RE", u"RW", u"WS", u"SM",   u"ST", u"SN",   u"RS", u"SC",
        u"SL",   u"SB", u"SO",   u"LK",   u"SH",   u"KN", u"LC", u"PM", u"VC",   u"SR", u"SZ",   u"TJ", u"TZ",
        u"TL",   u"TG", u"TK",   u"TO",   u"TM",   u"TC", u"TV", u"UG", u"VU",   u"VA", nullptr, u"VG", u"WF",
        u"EH",   u"ZM", u"ZZ",
    };
    return at_or<const char16_t *>(table, id, nullptr);
}

const char16_t *country_string(uint8_t id) {
    static constexpr const char16_t *table[] = {
        u"ZZ", u"AE", u"AL", u"AM",   u"AR", u"AT", u"AU", u"AZ", u"BE", u"BG", u"BH",   u"BN", u"BO", u"BR",
        u"BY", u"BZ", u"CA", nullptr, u"CH", u"CL", u"CN", u"CO", u"CR", u"CZ", u"DE",   u"DK", u"DO", u"DZ",
        u"EC", u"EE", u"EG", u"ES",   u"FI", u"FO", u"FR", u"GB", u"GE", u"GR", u"GT",   u"HK", u"HN", u"HR",
        u"HU", u"ID", u"IE", u"IL",   u"IN", u"IQ", u"IR", u"IS", u"IT", u"JM", u"JO",   u"JP", u"KE", u"KG",
        u"KR", u"KW", u"KZ", u"LB",   u"LI", u"LT", u"LU", u"LV", u"LY", u"MA", u"MC",   u"MK", u"MN", u"MO",
        u"MV", u"MX", u"MY", u"NI",   u"NL", u"NO", u"NZ", u"OM", u"PA", u"PE", u"PH",   u"PK", u"PL", u"PR",
        u"PT", u"PY", u"QA", u"RO",   u"RU", u"SA", u"SE", u"SG", u"SI", u"SK", nullptr, u"SV", u"SY", u"TH",
        u"TN", u"TR", u"TT", u"TW",   u"UA", u"US", u"UY", u"UZ", u"VE", u"VN", u"YE",   u"ZA", u"ZW", u"ZZ",
    };
    return at_or<const char16_t *>(table, id, nullptr);
}

const char16_t *language_string(uint8_t id) {
    static constexpr const char16_t *table[] = {
        u"zz", u"en",   u"ja", u"de", u"fr", u"es", u"it", u"ko", u"zh",
        u"pt", nullptr, u"pl", u"ru", u"sv", u"tr", u"nb", u"nl", u"zh",
    };
    return at_or<const char16_t *>(table, id, nullptr);
}

const char16_t *locale_string(uint8_t id) {
    static constexpr const char16_t *table[] = {
        u"ZZ", u"AU", u"AT", u"BE", u"BR", u"CA", u"CL", u"CN", u"CO", u"CZ", u"DK", u"FI", u"FR", u"DE", u"GR",
        u"HK", u"HU", u"IN", u"IE", u"IT", u"JP", u"KR", u"MX", u"NL", u"NZ", u"NO", u"PL", u"PT", u"SG", u"SK",
        u"ZA", u"ES", u"SE", u"CH", u"TW", u"GB", u"US", u"RU", u"ZZ", u"TR", u"AR", u"SA", u"IL", u"AE",
    };
    return at_or<const char16_t *>(table, id, nullptr);
}

uint8_t locale_from_online_country(uint8_t id) {
    static constexpr uint8_t table[] = {
        0, 43, 0,  0,  40, 2,  1, 0,  3,  0,  0,  0, 0,  4, 0, 0,  5, 0,  33, 6,  7,  8,  0,  9,  13, 10, 0,
        0, 0,  0,  0,  31, 11, 0, 12, 35, 0,  14, 0, 15, 0, 0, 16, 0, 18, 42, 17, 0,  0,  0,  19, 0,  0,  20,
        0, 0,  21, 0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 22, 0,  0,  23, 25, 24, 0,  0,  0,  0,
        0, 26, 0,  27, 0,  0,  0, 37, 41, 32, 28, 0, 29, 0, 0, 0,  0, 0,  39, 0,  34, 0,  36, 0,  0,  0,  0,
        0, 30, 0,  0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0, 0,  0,  0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0, 0,  0,  0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0, 0,  0,  0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0, 0,  0,  0,  0,  0,  0, 0,  0,  0,  0,  0, 0,  0, 0, 0,  0, 0,  0,  0,  38,
    };
    return at_or(table, id, uint8_t(0));
}

uint8_t language_from_online_language(uint8_t id) {
    static constexpr uint8_t table[] = {
        0, 1, 2, 3, 4, 5,  6, 7, 8, 9, 17, 11, 12, 1, 1, 15, 16, 13, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 14, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    };
    return at_or(table, id, uint8_t(0));
}

const char16_t *online_language_string(uint8_t id) {
    static constexpr const char16_t *table[] = {
        u"zz", u"en", u"ja", u"de", u"fr", u"es", u"it", u"ko", u"zh", u"pt", u"zh", u"pl", u"ru", u"da", u"fi", u"nb",
        u"nl", u"sv", u"cs", u"el", u"hu", u"sk", u"id", u"ms", u"ar", u"bg", u"et", u"hr", u"he", u"is", u"kk", u"lt",
        u"lv", u"ro", u"sl", u"th", u"tr", u"uk", u"vi", u"ps", u"sq", u"hy", u"bn", u"be", u"km", u"am", u"fo", u"ka",
        u"kl", u"sw", u"ky", u"lb", u"mk", u"mt", u"mn", u"ne", u"ur", u"rw", u"wo", u"si", u"tk",
    };
    return at_or<const char16_t *>(table, id, nullptr);
}

uint8_t country_from_online_country(uint8_t id) {
    // the identity up to 110 (17 and 94 have no country), 111 at 236
    if (id == 236) return 111;
    if (id > 110 || id == 17 || id == 94) return 0;
    return id;
}

// kOnlineCountryFeatures (xbox.h): {online country, features}
uint32_t online_country_features(uint8_t id) {
    static constexpr std::pair<uint8_t, uint32_t> table[] = {
        {4, 4},   {6, 12},  {5, 12},  {8, 12},  {13, 4},   {16, 12}, {19, 0},  {21, 0},  {23, 0},  {25, 12},
        {30, 0},  {32, 12}, {34, 12}, {24, 12}, {37, 0},   {39, 4},  {42, 0},  {46, 0},  {44, 12}, {45, 0},
        {50, 12}, {53, 8},  {56, 4},  {72, 0},  {71, 12},  {74, 12}, {76, 12}, {75, 12}, {82, 4},  {84, 4},
        {236, 0}, {88, 0},  {89, 0},  {91, 4},  {93, 0},   {109, 0}, {31, 12}, {90, 12}, {18, 12}, {101, 4},
        {97, 0},  {99, 4},  {35, 12}, {1, 0},   {103, 12}, {107, 0},
    };
    for (const auto &[country, features] : table)
        if (country == id) return features;
    return 0x80000000;
}

uint8_t locale_from_country(uint8_t id) {
    static constexpr uint8_t table[] = {
        0,  43, 0, 0,  40, 2,  1,  0,  3, 0,  0, 0,  0, 4, 0,  0,  5,  0,  33, 6,  7,  8, 0,  9, 13, 10, 0,  0,
        0,  0,  0, 31, 11, 0,  12, 35, 0, 14, 0, 15, 0, 0, 16, 0,  18, 42, 17, 0,  0,  0, 19, 0, 0,  20, 0,  0,
        21, 0,  0, 0,  0,  0,  0,  0,  0, 0,  0, 0,  0, 0, 0,  22, 0,  0,  23, 25, 24, 0, 0,  0, 0,  0,  26, 0,
        27, 0,  0, 0,  37, 41, 32, 28, 0, 29, 0, 0,  0, 0, 0,  39, 0,  34, 0,  36, 0,  0, 0,  0, 0,  30, 0,  38,
    };
    return at_or(table, id, uint8_t(0));
}

// xeXamGetLocaleEx: the country's locale, else by the game region
uint8_t locale_ex(Kernel &k, uint8_t max_country, uint8_t max_locale) {
    uint8_t country = config_country(k);
    if (country <= max_country) {
        uint8_t id = locale_from_country(country);
        if (id <= max_locale) return id;
    }
    uint32_t region = game_region(k);
    if (((region & 0xFF00) >> 8) == 1) return region == 0x101 ? 20 /* JP */ : 21 /* KR */;
    return ((region & 0xFF00) >> 8) == 2 ? 35 /* GB */ : 36 /* US */;
}

// a string to the guest, with its terminator, if it fits (the string
// exports' common tail)
uint32_t put_string(Kernel &k, uint32_t buffer, uint32_t length, const std::u16string &value) {
    if (value.size() + 1 > length) return hresult_from_win32(X_ERROR_INSUFFICIENT_BUFFER);
    put_u16(k, buffer, value, value.size() + 1);
    return X_E_SUCCESS;
}

uint32_t table_string(Kernel &k, const char16_t *s, uint32_t length, uint32_t buffer) {
    if (length >= 0x80000000u) return X_E_INVALIDARG;
    if (!s) return X_E_NOTFOUND;
    return put_string(k, buffer, length, s);
}

uint32_t pair_string(Kernel &k, const char16_t *a, const char16_t *b, uint32_t length, uint32_t buffer) {
    if (length >= 0x80000000u) return X_E_INVALIDARG;
    if (!a || !b) return X_E_NOTFOUND;
    return put_string(k, buffer, length, std::u16string(a) + u"-" + b);
}

// a profile's country and language, the console's where it has none
uint8_t country_of(Kernel &k, const User *u) {
    uint8_t country = u ? uint8_t((u->cached_flags & 0xFF00) >> 8) : 0;
    return country ? country : config_country(k);
}
uint32_t language_of(Kernel &k, const User *u) {
    uint32_t language = u ? (u->cached_flags & 0x3E000000) >> 25 : 0;
    return language ? language : config_language(k);
}

// XamState::GetUserProfileLive: the signed-in profile with that online XUID
// (an offline profile's is 0)
const User *user_live(Kernel &k, uint64_t online_xuid) {
    const User *u = user_at(k, 0);
    if (!u) return nullptr;
    uint64_t xuid = (u->reserved_flags & 0x20000000) ? u->xuid : 0;
    return xuid == online_xuid ? u : nullptr;
}

uint32_t live_legal_locale(Kernel &k, uint64_t xuid, uint32_t length, uint32_t buffer) {
    const User *u = user_live(k, xuid);
    return pair_string(k, online_language_string(uint8_t(language_of(k, u))), online_country_string(country_of(k, u)),
                       length, buffer);
}

// xeGetLanguage(true): the console's language, else by the region
uint32_t language(Kernel &k) {
    uint32_t desired = config_language(k);
    uint32_t region = game_region(k);
    if (desired < MAX_LANGUAGES) return desired;
    if ((region & 0xFF00) != 0x100) return LANGUAGE_ENGLISH;
    if (region == 0x101) return LANGUAGE_JAPANESE;
    return region == 0x102 ? LANGUAGE_SCHINESE : LANGUAGE_KOREAN;
}

// ---- dates and times ----

constexpr uint64_t FILETIME_DAY = 864'000'000'000ull; // 100 ns units
constexpr uint64_t MIN_VALID_FILETIME = 125911584000000000ull, MAX_VALID_FILETIME = 157469184000000000ull;

// XFormatDateString: MM/DD/YYYY
std::u16string date_string(uint64_t filetime) {
    using namespace std::chrono;
    year_month_day date{sys_days{year{1601} / January / 1} + days(int(filetime / FILETIME_DAY))};
    char text[32];
    std::snprintf(text, sizeof(text), "%02u/%02u/%d", unsigned(date.month()), unsigned(date.day()), int(date.year()));
    return std::u16string(text, text + std::strlen(text));
}

// XFormatTimeString: HH:MM
std::u16string time_string(uint64_t filetime) {
    uint64_t seconds = filetime % FILETIME_DAY / 10'000'000;
    char text[16];
    std::snprintf(text, sizeof(text), "%02u:%02u", unsigned(seconds / 3600), unsigned(seconds / 60 % 60));
    return std::u16string(text, text + std::strlen(text));
}

// (r3 the user index or format, r4 the filetime, r5 the buffer, r6 its size
// in bytes); the newer dashboards' form has no first argument, which an
// invalid time there says. The console's XAM has only the newer form, and a
// time it cannot format (or no room for it) is -1 with nothing written
// (xfxam); Xenia, and this before, wrote through whatever the arguments
// made of the buffer. A done one answers the characters written, as the
// console's snwprintf does; Xenia's is void.
template <class F> void format_time(Kernel &k, ppc::Context &c, F format) {
    uint64_t filetime = arg64(k, c, 1);
    uint32_t buffer = arg(k, c, 2), size = arg(k, c, 3);
    if (filetime < MIN_VALID_FILETIME || filetime > MAX_VALID_FILETIME) {
        filetime = c.r[3];
        buffer = uint32_t(c.r[4]), size = uint32_t(c.r[5]);
    }
    if (filetime < MIN_VALID_FILETIME || filetime > MAX_VALID_FILETIME || !buffer || size < 2)
        return result(c, 0xFFFFFFFF);
    std::u16string text = format(filetime);
    put_u16(k, buffer, text, size / 2);
    result(c, uint32_t(std::min<size_t>(text.size(), size / 2 - 1)));
}

// ---- the language's strings in guest memory (xam_state.cc) ----

void make_strings(Kernel &k, LocaleState &s) {
    if (s.strings_made) return;
    s.strings_made = true;
    static constexpr const char16_t *fallbacks[18] = {
        u"",      u"",       u"ja-JP", u"de-DE", u"fr-FR", u"es-ES", u"it-IT", u"ko-KR", u"zh-TW",
        u"pt-BR", u"zh-CHS", u"pl-PL", u"ru-RU", u"sv-SE", u"tr-TR", u"nb-NO", u"nl-NL", u"zh-CHS",
    };
    static constexpr const char16_t *patches[7] = {
        u"",
        u"file://media:/XenonSCLatin.xttp2",
        u"file://media:/XenonCLatin.xttp2",
        u"file://media:/XenonJKLatin.xttp2",
        u"file://media:/XenonSCLatin.xttp1",
        u"file://media:/XenonCLatin.xttp1",
        u"file://media:/XenonJKLatin.xttp1",
    };
    // each table's strings one after another, from its second
    auto place = [&](const char16_t *const *table, size_t count, uint32_t *out) {
        size_t chars = 0;
        for (size_t i = 1; i < count; i++) chars += std::char_traits<char16_t>::length(table[i]) + 1;
        uint32_t p = k.memory().system_alloc(uint32_t(2 * chars));
        if (!p) return;
        for (size_t i = 1; i < count; i++) {
            std::u16string_view v = table[i];
            out[i] = p;
            put_u16(k, p, v, v.size() + 1);
            p += uint32_t(2 * (v.size() + 1));
        }
    };
    place(fallbacks, std::size(fallbacks), s.fallbacks);
    place(patches, std::size(patches), s.typeface_patches);
}

// ---- the title's SPA (xdbf_io.cc, spa_info.cc) ----

std::u16string from_utf8(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t b = uint8_t(s[i]);
        uint32_t cp = b, extra = 0;
        if (b >= 0xF0)
            cp = b & 0x07, extra = 3;
        else if (b >= 0xE0)
            cp = b & 0x0F, extra = 2;
        else if (b >= 0xC0)
            cp = b & 0x1F, extra = 1;
        i++;
        for (uint32_t n = 0; n < extra && i < s.size(); n++, i++) cp = cp << 6 | (uint8_t(s[i]) & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

} // namespace

std::map<uint16_t, std::u16string> spa_strings(Kernel &k, uint32_t base, uint32_t size) {
    std::map<uint16_t, std::u16string> strings;
    // XdbfHeader (24): magic, version, entry count and used, free count and
    // used; the entries (18 each: section, id, offset, size), the free
    // table (8 each), then the data
    if (!base || size <= 24 || k.load32(base) != 0x58444246) return strings; // "XDBF"
    uint32_t entry_count = k.load32(base + 8), entry_used = k.load32(base + 12), free_count = k.load32(base + 16);
    uint32_t data = base + 24 + 18 * entry_count + 8 * free_count;
    auto find = [&](uint16_t section, uint64_t id, uint32_t &at, uint32_t &length) {
        for (uint32_t i = 0; i < entry_used && i < entry_count; i++) {
            uint32_t e = base + 24 + 18 * i;
            if (k.load16(e) != section || k.load64(e + 2) != id) continue;
            at = data + k.load32(e + 10), length = k.load32(e + 14);
            return true;
        }
        return false;
    };
    uint32_t at = 0, length = 0, language = LANGUAGE_ENGLISH;
    // XSTC (section 1): magic, version, size, the default language
    if (find(1, 0x58535443, at, length) && length >= 16) language = k.load32(at + 12);
    // XSTR (section 3, by language): magic, version, size, count (16 bits),
    // then each string's id, length and UTF-8 bytes
    if (!find(3, language, at, length) || length < 14 || k.load32(at) != 0x58535452) return strings;
    uint16_t count = k.load16(at + 12);
    uint32_t p = at + 14, end = at + length;
    for (uint16_t i = 0; i < count && p + 4 <= end; i++) {
        uint16_t id = k.load16(p), n = k.load16(p + 2);
        if (p + 4 + n > end) break;
        strings[id] = from_utf8(std::string_view(reinterpret_cast<const char *>(k.host(p + 4)), n));
        p += 4 + n;
    }
    return strings;
}

namespace {

// the executable's SPA: its resource named by the title ID
std::map<uint16_t, std::u16string> read_spa_strings(Kernel &k, ppc::Context &c) {
    uint32_t module = k.load32(k.variable("XexExecutableModuleHandle"));
    if (!module) return {};
    uint32_t name = export_scratch(c), data_ptr = name + 16, size_ptr = name + 20;
    std::snprintf(reinterpret_cast<char *>(k.host(name)), 16, "%08X", title_id(k));
    k.store32(data_ptr, 0), k.store32(size_ptr, 0);
    if (call_export(k, c, "xboxkrnl", "XexGetModuleSection", {module, name, data_ptr, size_ptr}) != X_STATUS_SUCCESS)
        return {};
    return spa_strings(k, k.load32(data_ptr), k.load32(size_ptr));
}

std::u16string localized_string(Kernel &k, ppc::Context &c, uint16_t id) {
    LocaleState &s = locale(k);
    std::lock_guard<std::mutex> l(s.mutex);
    if (!s.spa_strings) s.spa_strings = read_spa_strings(k, c);
    auto it = s.spa_strings->find(id);
    return it == s.spa_strings->end() ? std::u16string() : it->second;
}

// keXamBuildResourceLocator: a file in media: (no module), or a module's
// section; buffer_count in bytes
uint32_t resource_locator(Kernel &k, uint64_t module, std::u16string_view container, std::u16string_view resource,
                          uint32_t buffer, uint32_t buffer_count) {
    std::u16string path;
    if (!module) {
        path = u"file://media:/" + std::u16string(container) + u".xzp#" + std::u16string(resource);
    } else {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%X", uint32_t(module));
        path = u"section://" + std::u16string(hex, hex + std::strlen(hex)) + u"," + std::u16string(container) + u"#" +
               std::u16string(resource);
    }
    put_u16(k, buffer, path, buffer_count / 2);
    return 0;
}

// the xam module's handle (KernelState::GetModule("xam"))
uint32_t xam_module(Kernel &k, ppc::Context &c) {
    uint32_t name = export_scratch(c), hmodule_ptr = name + 8;
    std::memcpy(k.host(name), "xam", 4);
    k.store32(hmodule_ptr, 0);
    call_export(k, c, "xboxkrnl", "XexGetModuleHandle", {name, hmodule_ptr});
    return k.load32(hmodule_ptr);
}

} // namespace

// ---- locales ----

XF_EXPORT(xam, XamGetLocale) { result(c, locale_ex(k, 111, 43)); }

// (max_country_id, max_locale_id)
XF_EXPORT(xam, XamGetLocaleEx) { result(c, locale_ex(k, uint8_t(arg(k, c, 0)), uint8_t(arg(k, c, 1)))); }

XF_EXPORT(xam, XamGetOnlineCountryFromLocale) { result(c, online_country_from_locale(uint8_t(arg(k, c, 0)))); }
XF_EXPORT(xam, XamGetLocaleFromOnlineCountry) { result(c, locale_from_online_country(uint8_t(arg(k, c, 0)))); }
XF_EXPORT(xam, XamGetLanguageFromOnlineLanguage) { result(c, language_from_online_language(uint8_t(arg(k, c, 0)))); }
XF_EXPORT(xam, XamGetCountryFromOnlineCountry) { result(c, country_from_online_country(uint8_t(arg(k, c, 0)))); }
XF_EXPORT(xam, XamGetOnlineCountryFeatures) { result(c, online_country_features(uint8_t(arg(k, c, 0)))); }

// (id, buffer_length in characters, buffer)
XF_EXPORT(xam, XamGetOnlineCountryString) {
    result(c, table_string(k, online_country_string(uint8_t(arg(k, c, 0))), arg(k, c, 1), arg(k, c, 2)));
}
XF_EXPORT(xam, XamGetCountryString) {
    result(c, table_string(k, country_string(uint8_t(arg(k, c, 0))), arg(k, c, 1), arg(k, c, 2)));
}
XF_EXPORT(xam, XamGetLanguageString) {
    result(c, table_string(k, language_string(uint8_t(arg(k, c, 0))), arg(k, c, 1), arg(k, c, 2)));
}
XF_EXPORT(xam, XamGetLocaleString) {
    result(c, table_string(k, locale_string(uint8_t(arg(k, c, 0))), arg(k, c, 1), arg(k, c, 2)));
}
XF_EXPORT(xam, XamGetOnlineLanguageString) {
    result(c, table_string(k, online_language_string(uint8_t(arg(k, c, 0))), arg(k, c, 1), arg(k, c, 2)));
}

// (language_id, locale_id, buffer_length, buffer): "ll-CC"
XF_EXPORT(xam, XamGetLanguageLocaleString) {
    result(c, pair_string(k, language_string(uint8_t(arg(k, c, 0))), locale_string(uint8_t(arg(k, c, 1))), arg(k, c, 2),
                          arg(k, c, 3)));
}

// (language_id, country_id, buffer_length, buffer)
XF_EXPORT(xam, XamGetOnlineLanguageAndCountryString) {
    result(c, pair_string(k, online_language_string(uint8_t(arg(k, c, 0))),
                          online_country_string(uint8_t(arg(k, c, 1))), arg(k, c, 2), arg(k, c, 3)));
}

// (xuid, language_ptr, country_ptr): a byte each, the profile's or the
// console's
XF_EXPORT(xam, XamGetOnlineLanguageAndCountry) {
    const User *u = user_by_xuid(k, arg64(k, c, 0));
    uint32_t language_ptr = arg(k, c, 1), country_ptr = arg(k, c, 2);
    if (country_ptr) k.host(country_ptr)[0] = country_of(k, u);
    if (language_ptr) k.host(language_ptr)[0] = uint8_t(language_of(k, u));
}

// (online_xuid, buffer_length, buffer): the signed-in profile's language
// and country, as "ll-CC"
XF_EXPORT(xam, XamProfileGetLiveLegalLocale) {
    result(c, live_legal_locale(k, arg64(k, c, 0), arg(k, c, 1), arg(k, c, 2)));
}

// (buffer_length, buffer): the same in 8-bit characters, from a buffer of
// seven
XF_EXPORT(xam, XapipGetLocale) {
    uint32_t length = arg(k, c, 0), buffer = arg(k, c, 1);
    uint32_t wide = export_scratch(c);
    uint32_t r = live_legal_locale(k, 0, 7, wide);
    if (r == X_E_SUCCESS && length) {
        std::u16string s = guest_u16(k, wide, 7);
        size_t n = std::min<size_t>(s.size(), length - 1);
        for (size_t i = 0; i < n; i++) k.host(buffer + uint32_t(i))[0] = uint8_t(s[i]);
        k.host(buffer + uint32_t(n))[0] = 0;
    }
    result(c, r);
}

// (locale): the date's order (a table of the dashboard's switch). The
// console's XAM answers differently where Xenia's table, or its range, gives
// the default 3 (xfxam, every locale from 0 to 125 asked): 2 for it, except
// locale 13, which it answers 0, and locale 103, which stays 3. Entries that
// are not 3 are the same on both.
XF_EXPORT(xam, XamGetLocaleDateFormat) {
    static constexpr uint8_t table[] = {
        2, 1, 3, 1, 3, 3, 3, 3, 3, 3, 3, 2, 3, 2, 1, 4, 2, 3, 1, 2, 2, 3, 3, 3, 3, 3, 2, 1, 3, 2, 2, 3, 0, 3, 0, 3, 3,
        5, 3, 1, 3, 2, 3, 3, 3, 2, 3, 3, 5, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3, 0, 2, 1, 3, 3,
        3, 3, 3, 5, 3, 2, 3, 3, 3, 2, 3, 5, 0, 3, 1, 3, 3, 3, 3, 3, 3, 3, 4, 3, 3, 3, 3, 3, 3, 3, 5, 1, 1, 1, 1,
    };
    static_assert(std::size(table) > 0x68);
    uint32_t locale = arg(k, c, 0), biased = locale - 5;
    uint32_t format = biased > 0x68 ? 3 : table[biased];
    if (format == 3) format = locale == 103 ? 3 : locale == 13 ? 0 : 2;
    result(c, format);
}

// (user_index or format, filetime, buffer, size in bytes)
XF_EXPORT(xam, XamFormatDateString) { format_time(k, c, date_string); }
XF_EXPORT(xam, XamFormatTimeString) { format_time(k, c, time_string); }

// ---- languages and countries ----

XF_EXPORT(xam, XamGetLanguage) { result(c, language(k)); }

// the console's language before Swedish, else by the region
XF_EXPORT(xam, XTLGetLanguageV2) {
    uint32_t desired = config_language(k);
    uint32_t region = game_region(k);
    if (desired < LANGUAGE_SWEDISH) return result(c, desired);
    if ((region & 0xFF00) != 0x100) return result(c, LANGUAGE_ENGLISH);
    result(c, region == 0x101 ? LANGUAGE_JAPANESE : LANGUAGE_KOREAN);
}

// (language): its "ll-CC" fallback in guest memory
XF_EXPORT(xam, XamGetLanguageLocaleFallbackString) {
    uint32_t language = arg(k, c, 0);
    LocaleState &s = locale(k);
    std::lock_guard<std::mutex> l(s.mutex);
    make_strings(k, s);
    result(c, language < MAX_LANGUAGES ? s.fallbacks[language] : 0);
}

// (language, buffer_size, buffer): the font's path, whole (as Xenia's,
// which does not look at the size)
XF_EXPORT(xam, XamGetLanguageTypeface) {
    uint32_t language = arg(k, c, 0), buffer = arg(k, c, 2);
    std::u16string path = language == LANGUAGE_SCHINESE   ? u"file://media:/XenonSCLatin.xtt"
                          : language == LANGUAGE_TCHINESE ? u"file://media:/XenonCLatin.xtt"
                                                          : u"file://media:/XenonJKLatin.xtt";
    if (buffer) put_u16(k, buffer, path, path.size() + 1);
    result(c, X_STATUS_SUCCESS);
}

// (language): its font patch's path in guest memory (the xttp2 ones)
XF_EXPORT(xam, XamGetLanguageTypefacePatch) {
    uint32_t language = arg(k, c, 0);
    LocaleState &s = locale(k);
    std::lock_guard<std::mutex> l(s.mutex);
    make_strings(k, s);
    result(c, s.typeface_patches[language == LANGUAGE_SCHINESE ? 1 : language == LANGUAGE_TCHINESE ? 2 : 3]);
}

XF_EXPORT(xam, XamGetCountry) { result(c, config_country(k)); }

// (country): the console's country written
XF_EXPORT(xam, XamSetCountry) {
    write_country(k, uint8_t(arg(k, c, 0)));
    result(c, 0);
}

// ---- strings (xam_info.cc) ----

// (title_id, id (64 bits), user_index, buffer, size_ptr (characters),
// overlapped): the title's string by its SPA id
XF_EXPORT(xam, XamReadString) {
    uint64_t id = arg64(k, c, 1);
    uint32_t buffer = arg(k, c, 3), size_ptr = arg(k, c, 4), ov = arg(k, c, 5);
    if (!buffer || id == 0xFFFF) return result(c, X_E_INVALIDARG);
    if (!size_ptr) return result(c, hresult_from_win32(X_ERROR_INSUFFICIENT_BUFFER));
    std::u16string s = localized_string(k, c, uint16_t(id));
    result(c, run_or_defer(k, ov, [&](uint32_t &extended_error, uint32_t &length) {
               put_u16(k, buffer, s, k.load32(size_ptr));
               extended_error = 0, length = 0;
               return X_ERROR_SUCCESS;
           }));
}

// (index): no common strings (Xenia's stub)
XF_EXPORT(xam, XamLookupCommonStringByIndex) { result(c, 0); }

// XamGetOnlineSchema is in xam_info.cpp.

// (module (64 bits), container, resource, buffer, buffer_count in bytes)
XF_EXPORT(xam, XamBuildResourceLocator) {
    result(c, resource_locator(k, arg64(k, c, 0), guest_u16(k, arg(k, c, 1)), guest_u16(k, arg(k, c, 2)), arg(k, c, 3),
                               arg(k, c, 4)));
}

// (filename, buffer, buffer_count): xam's resources, its sections
XF_EXPORT(xam, XamBuildGamercardResourceLocator) {
    uint32_t filename = arg(k, c, 0), buffer = arg(k, c, 1), count = arg(k, c, 2);
    result(c, resource_locator(k, xam_module(k, c), u"gamercrd", guest_u16(k, filename), buffer, count));
}
XF_EXPORT(xam, XamBuildSharedSystemResourceLocator) {
    uint32_t filename = arg(k, c, 0), buffer = arg(k, c, 1), count = arg(k, c, 2);
    result(c, resource_locator(k, xam_module(k, c), u"shrdres", guest_u16(k, filename), buffer, count));
}
XF_EXPORT(xam, XamBuildLegacySystemResourceLocator) {
    uint32_t filename = arg(k, c, 0), buffer = arg(k, c, 1), count = arg(k, c, 2);
    result(c, resource_locator(k, xam_module(k, c), u"shrdres", guest_u16(k, filename), buffer, count));
}
// a local file, xam.xzp in media:
XF_EXPORT(xam, XamBuildXamResourceLocator) {
    result(c, resource_locator(k, 0, u"xam", guest_u16(k, arg(k, c, 0)), arg(k, c, 1), arg(k, c, 2)));
}

} // namespace xf::kernel
