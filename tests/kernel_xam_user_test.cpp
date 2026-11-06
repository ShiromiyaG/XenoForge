// XAM's users, profiles, locale and Win32 functions (src/kernel/xam_user.cpp,
// xam_locale.cpp, xam_win32.cpp), driven through their exports as a game
// calls them, on guest threads: the locale tables and strings, the
// console's country and language, dates and times, the language's font
// paths, the resource locators, an SPA's string table; the user's gamertag,
// account flags, tenure and subscription, restrictions, preferences,
// signing out and in with its notifications, the titles played, Kinect's
// answers; profiles found, created, enumerated, opened and closed; threads
// made (suspended too), resumed, waited for and ended, sleeps and waits
// woken by APCs, the last error, the clock, RtlRandom, lstrlenW, XamAllocEx.

#include "check.h"

#include "files.h"
#include "objects.h"
#include "xam.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_WORKER = 0x82000010,
    F_EXITER = 0x82000020,
    F_APC = 0x82000030,
};

constexpr uint64_t OTHER_XUID = 0xE030000000001234ull;
constexpr uint32_t X_STATUS_INVALID_HANDLE_ = 0xC0000008;
constexpr uint32_t WAIT_TIMEOUT = 0x102, WAIT_IO_COMPLETION = 0xC0, WAIT_FAILED = 0xFFFFFFFF;

// an export, as the code calls it: r3-r10, then the stack's words at
// r1 + 0x54 + 8 * (n - 8)
uint32_t call(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s!%s\n", library, name);
        failures++;
        return 0xFFFFFFFF;
    }
    int n = 0;
    for (uint64_t a : args) {
        if (n < 8)
            c.r[3 + n] = a;
        else
            K->store32(uint32_t(c.r[1]) + 0x54 + 8 * (n - 8), uint32_t(a));
        n++;
    }
    fn(*K, c);
    return uint32_t(c.r[3]);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args = {}) {
    return call(c, "xboxkrnl", name, args);
}
uint32_t xam(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args = {}) {
    return call(c, "xam", name, args);
}

// per-thread guest scratch below the stack pointer (clear of the exports'
// own, just below it)
uint32_t local(ppc::Context &c, uint32_t offset) { return uint32_t(c.r[1]) - 0x1000 + offset; }

uint32_t wide(ppc::Context &c, uint32_t offset, std::u16string_view s) {
    uint32_t p = local(c, offset);
    put_u16(*K, p, s, s.size() + 1);
    return p;
}
std::u16string read_wide(uint32_t p) { return guest_u16(*K, p); }
std::string read_text(uint32_t p) { return reinterpret_cast<const char *>(K->host(p)); }

std::atomic<uint32_t> worker_param{0}, worker_r4{0xFFFFFFFF}, worker_id{0}, worker_runs{0}, apcs{0};

void worker(ppc::Context &c) {
    worker_param = uint32_t(c.r[3]);
    worker_r4 = uint32_t(c.r[4]);
    worker_id = xam(c, "GetCurrentThreadId");
    worker_runs++;
    c.r[3] = 0;
}

void exiter(ppc::Context &c) {
    xam(c, "ExitThread", {0x42});
    c.r[3] = 0xBAD;
}

void apc(ppc::Context &c) {
    apcs++;
    c.r[3] = 0;
}

void queue_apc_to_self() {
    Objects &o = objects(*K);
    std::lock_guard<std::mutex> l(o.lock);
    o.queue_user_apc(*current_thread(), F_APC, 0, 0, 0);
}

uint32_t listener_next(ppc::Context &c, uint32_t listener, uint32_t &param) {
    uint32_t id = local(c, 0xF00), p = local(c, 0xF04);
    if (!xam(c, "XNotifyGetNext", {listener, 0, id, p})) return 0;
    param = K->load32(p);
    return K->load32(id);
}

// ---- locale ----

void locale_tables(ppc::Context &c) {
    // the console's: the United States, English
    CHECK(xam(c, "XamGetCountry") == 103);
    CHECK(xam(c, "XamGetLocale") == 36);
    CHECK(xam(c, "XamGetLocaleEx", {111, 43}) == 36);
    CHECK(xam(c, "XamGetLocaleEx", {100, 43}) == 36); // past the countries: by the region, not Japan or Korea
    CHECK(xam(c, "XamGetLanguage") == 1);
    CHECK(xam(c, "XTLGetLanguageV2") == 1);

    CHECK(xam(c, "XamGetOnlineCountryFromLocale", {36}) == 103);
    CHECK(xam(c, "XamGetOnlineCountryFromLocale", {38}) == 236);
    CHECK(xam(c, "XamGetOnlineCountryFromLocale", {44}) == 0);
    CHECK(xam(c, "XamGetLocaleFromOnlineCountry", {103}) == 36);
    CHECK(xam(c, "XamGetLocaleFromOnlineCountry", {236}) == 38);
    CHECK(xam(c, "XamGetLocaleFromOnlineCountry", {237}) == 0);
    CHECK(xam(c, "XamGetCountryFromOnlineCountry", {50}) == 50);
    CHECK(xam(c, "XamGetCountryFromOnlineCountry", {17}) == 0);
    CHECK(xam(c, "XamGetCountryFromOnlineCountry", {94}) == 0);
    CHECK(xam(c, "XamGetCountryFromOnlineCountry", {236}) == 111);
    CHECK(xam(c, "XamGetCountryFromOnlineCountry", {120}) == 0);
    CHECK(xam(c, "XamGetLanguageFromOnlineLanguage", {10}) == 17);
    CHECK(xam(c, "XamGetLanguageFromOnlineLanguage", {36}) == 14);
    CHECK(xam(c, "XamGetLanguageFromOnlineLanguage", {40}) == 1);
    CHECK(xam(c, "XamGetLanguageFromOnlineLanguage", {61}) == 0);
    CHECK(xam(c, "XamGetOnlineCountryFeatures", {103}) == 12);
    CHECK(xam(c, "XamGetOnlineCountryFeatures", {53}) == 8);
    CHECK(xam(c, "XamGetOnlineCountryFeatures", {236}) == 0);
    CHECK(xam(c, "XamGetOnlineCountryFeatures", {2}) == 0x80000000);

    // strings: the terminator must fit
    uint32_t buffer = local(c, 0x100);
    CHECK(xam(c, "XamGetCountryString", {103, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"US");
    CHECK(xam(c, "XamGetCountryString", {103, 2, buffer}) == hresult_from_win32(X_ERROR_INSUFFICIENT_BUFFER));
    CHECK(xam(c, "XamGetCountryString", {17, 3, buffer}) == X_E_NOTFOUND);
    CHECK(xam(c, "XamGetCountryString", {112, 3, buffer}) == X_E_NOTFOUND);
    CHECK(xam(c, "XamGetCountryString", {103, 0x80000000, buffer}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamGetLanguageString", {3, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"de");
    CHECK(xam(c, "XamGetLanguageString", {10, 3, buffer}) == X_E_NOTFOUND);
    CHECK(xam(c, "XamGetLocaleString", {36, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"US");
    CHECK(xam(c, "XamGetOnlineCountryString", {236, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"ZZ");
    CHECK(xam(c, "XamGetOnlineCountryString", {113, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"AD");
    CHECK(xam(c, "XamGetOnlineLanguageString", {13, 3, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"da");
    CHECK(xam(c, "XamGetOnlineLanguageString", {61, 3, buffer}) == X_E_NOTFOUND);
    CHECK(xam(c, "XamGetLanguageLocaleString", {1, 36, 6, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"en-US");
    CHECK(xam(c, "XamGetLanguageLocaleString", {1, 36, 5, buffer}) == hresult_from_win32(X_ERROR_INSUFFICIENT_BUFFER));
    CHECK(xam(c, "XamGetLanguageLocaleString", {1, 44, 6, buffer}) == X_E_NOTFOUND);
    CHECK(xam(c, "XamGetOnlineLanguageAndCountryString", {3, 24, 6, buffer}) == X_E_SUCCESS &&
          read_wide(buffer) == u"de-DE");

    // the user's, an offline profile's: the console's
    uint32_t language = local(c, 0x180), country = local(c, 0x181);
    K->host(language)[0] = K->host(country)[0] = 0xEE;
    xam(c, "XamGetOnlineLanguageAndCountry", {DEFAULT_XUID, language, country});
    CHECK(K->host(language)[0] == 1 && K->host(country)[0] == 103);
    CHECK(xam(c, "XamProfileGetLiveLegalLocale", {0, 7, buffer}) == X_E_SUCCESS && read_wide(buffer) == u"en-US");
    CHECK(xam(c, "XapipGetLocale", {16, buffer}) == X_E_SUCCESS && read_text(buffer) == "en-US");
    CHECK(xam(c, "XapipGetLocale", {3, buffer}) == X_E_SUCCESS && read_text(buffer) == "en");

    // the date's order by locale
    CHECK(xam(c, "XamGetLocaleDateFormat", {5}) == 2);
    CHECK(xam(c, "XamGetLocaleDateFormat", {6}) == 1);
    CHECK(xam(c, "XamGetLocaleDateFormat", {0x6D}) == 5);
    CHECK(xam(c, "XamGetLocaleDateFormat", {0x6E}) == 3);
    CHECK(xam(c, "XamGetLocaleDateFormat", {4}) == 3);

    // the country written, if the console's settings can be
    if (find_export_fn("xboxkrnl", "ExSetXConfigSetting")) {
        CHECK(xam(c, "XamSetCountry", {50}) == 0);
        CHECK(xam(c, "XamGetCountry") == 50);
        CHECK(xam(c, "XamGetLocale") == 19); // Italy
        CHECK(xam(c, "XamSetCountry", {103}) == 0);
        CHECK(xam(c, "XamGetCountry") == 103);
    } else {
        CHECK(xam(c, "XamSetCountry", {50}) == 0);
        std::printf("kernel_xam_user: no ExSetXConfigSetting, XamSetCountry not checked\n");
    }
}

void dates_and_fonts(ppc::Context &c) {
    // 2020-03-04 05:06:07 UTC
    constexpr uint64_t filetime = (1583298367ull + 11644473600ull) * 10'000'000ull;
    uint32_t buffer = local(c, 0x100);
    xam(c, "XamFormatDateString", {0, filetime, buffer, 64});
    CHECK(read_wide(buffer) == u"03/04/2020");
    xam(c, "XamFormatTimeString", {0, filetime, buffer, 64});
    CHECK(read_wide(buffer) == u"05:06");
    // the newer dashboards' form, with no first argument
    xam(c, "XamFormatDateString", {filetime, buffer, 64, 0});
    CHECK(read_wide(buffer) == u"03/04/2020");
    // truncated to the bytes given
    xam(c, "XamFormatTimeString", {0, filetime, buffer, 6});
    CHECK(read_wide(buffer) == u"05");

    uint32_t de = xam(c, "XamGetLanguageLocaleFallbackString", {3});
    CHECK(de && read_wide(de) == u"de-DE");
    CHECK(xam(c, "XamGetLanguageLocaleFallbackString", {3}) == de);
    CHECK(read_wide(xam(c, "XamGetLanguageLocaleFallbackString", {17})) == u"zh-CHS");
    uint32_t english = xam(c, "XamGetLanguageLocaleFallbackString", {1});
    CHECK(english && read_wide(english).empty());
    CHECK(xam(c, "XamGetLanguageLocaleFallbackString", {0}) == 0);
    CHECK(xam(c, "XamGetLanguageTypeface", {17, 0, buffer}) == 0 &&
          read_wide(buffer) == u"file://media:/XenonSCLatin.xtt");
    CHECK(xam(c, "XamGetLanguageTypeface", {1, 0, buffer}) == 0 &&
          read_wide(buffer) == u"file://media:/XenonJKLatin.xtt");
    CHECK(read_wide(xam(c, "XamGetLanguageTypefacePatch", {8})) == u"file://media:/XenonCLatin.xttp2");
    CHECK(read_wide(xam(c, "XamGetLanguageTypefacePatch", {2})) == u"file://media:/XenonJKLatin.xttp2");
}

void locators_and_strings(ppc::Context &c) {
    uint32_t buffer = local(c, 0x200);
    uint32_t container = wide(c, 0x300, u"cont"), resource = wide(c, 0x340, u"res");
    CHECK(xam(c, "XamBuildResourceLocator", {0, container, resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"file://media:/cont.xzp#res");
    CHECK(xam(c, "XamBuildResourceLocator", {0x12345678, container, resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"section://12345678,cont#res");
    CHECK(xam(c, "XamBuildResourceLocator", {0, container, resource, buffer, 10}) == 0);
    CHECK(read_wide(buffer) == u"file");
    CHECK(xam(c, "XamBuildXamResourceLocator", {resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"file://media:/xam.xzp#res");
    // xam's own sections, by its module's handle
    uint32_t name = local(c, 0x380), hmodule = local(c, 0x390);
    std::memcpy(K->host(name), "xam", 4);
    krnl(c, "XexGetModuleHandle", {name, hmodule});
    CHECK(K->load32(hmodule) != 0);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%X", K->load32(hmodule));
    std::u16string module_hex(hex, hex + std::strlen(hex));
    CHECK(xam(c, "XamBuildGamercardResourceLocator", {resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"section://" + module_hex + u",gamercrd#res");
    CHECK(xam(c, "XamBuildSharedSystemResourceLocator", {resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"section://" + module_hex + u",shrdres#res");
    CHECK(xam(c, "XamBuildLegacySystemResourceLocator", {resource, buffer, 200}) == 0);
    CHECK(read_wide(buffer) == u"section://" + module_hex + u",shrdres#res");

    // the title's strings: no executable here, so none
    uint32_t size = local(c, 0x3A0);
    K->store32(size, 16);
    K->store16(buffer, 'x');
    CHECK(xam(c, "XamReadString", {0, 5, 0, buffer, size, 0}) == 0 && K->load16(buffer) == 0);
    CHECK(xam(c, "XamReadString", {0, 0xFFFF, 0, buffer, size, 0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamReadString", {0, 5, 0, 0, size, 0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamReadString", {0, 5, 0, buffer, 0, 0}) == hresult_from_win32(X_ERROR_INSUFFICIENT_BUFFER));
    CHECK(xam(c, "XamLookupCommonStringByIndex", {3}) == 0);
    uint32_t schema = xam(c, "XamGetOnlineSchema");
    CHECK(schema && K->load32(schema) == schema + 8 && K->load32(schema + 4) == 44 &&
          K->load32(schema + 8 + 12) == 0x2C && K->host(schema + 8 + 43)[0] == 0x18);
    CHECK(xam(c, "XamGetOnlineSchema") == schema);

    // an SPA: XSTC says French, whose XSTR table has two strings (one
    // UTF-8 with an accent); English's is not read
    uint32_t spa = K->memory().system_alloc(0x200);
    std::memset(K->host(spa), 0, 0x200);
    uint32_t entries = spa + 24, data = entries + 18 * 4 + 8 * 1;
    K->store32(spa, 0x58444246);
    K->store32(spa + 4, 0x10000);
    K->store32(spa + 8, 4), K->store32(spa + 12, 3), K->store32(spa + 16, 1), K->store32(spa + 20, 1);
    auto entry = [&](uint32_t i, uint16_t section, uint64_t id, uint32_t offset, uint32_t length) {
        uint32_t e = entries + 18 * i;
        K->store16(e, section);
        K->store64(e + 2, id);
        K->store32(e + 10, offset);
        K->store32(e + 14, length);
    };
    // XSTC at 0: French (4)
    K->store32(data, 0x58535443), K->store32(data + 4, 1), K->store32(data + 8, 4), K->store32(data + 12, 4);
    entry(0, 1, 0x58535443, 0, 16);
    // English at 16: one string
    uint32_t en = data + 16;
    K->store32(en, 0x58535452), K->store32(en + 4, 1), K->store32(en + 8, 0), K->store16(en + 12, 1);
    K->store16(en + 14, 1), K->store16(en + 16, 2), std::memcpy(K->host(en + 18), "EN", 2);
    entry(1, 3, 1, 16, 20);
    // French at 48: "Ete" with two accents (UTF-8) and "ok"
    uint32_t fr = data + 48;
    K->store32(fr, 0x58535452), K->store32(fr + 4, 1), K->store32(fr + 8, 0), K->store16(fr + 12, 2);
    K->store16(fr + 14, 1), K->store16(fr + 16, 5), std::memcpy(K->host(fr + 18), "\xC3\x89t\xC3\xA9", 5);
    K->store16(fr + 23, 7), K->store16(fr + 25, 2), std::memcpy(K->host(fr + 27), "ok", 2);
    entry(2, 3, 4, 48, 31);
    auto strings = spa_strings(*K, spa, 0x200);
    CHECK(strings.size() == 2 && strings[1] == u"\u00C9t\u00E9" && strings[7] == u"ok");
    // no XSTC: English
    K->store16(entries, 9);
    strings = spa_strings(*K, spa, 0x200);
    CHECK(strings.size() == 1 && strings[1] == u"EN");
    K->store32(spa, 0);
    CHECK(spa_strings(*K, spa, 0x200).empty());
}

// ---- users ----

void user_flags(ppc::Context &c) {
    uint32_t buffer = local(c, 0x100);
    CHECK(xam(c, "XamUserGetGamerTag", {0, buffer, 16}) == X_E_SUCCESS && read_wide(buffer) == u"User");
    CHECK(xam(c, "XamUserGetGamerTag", {0, buffer, 15}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserGetGamerTag", {0, 0, 16}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserGetGamerTag", {4, buffer, 16}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserGetGamerTag", {1, buffer, 16}) == X_E_NO_SUCH_USER);

    // an offline account: no tier, flags, Live or parental control
    CHECK(xam(c, "XamUserGetMembershipTier", {0}) == 0);
    CHECK(xam(c, "XamUserGetMembershipTierFromXUID", {DEFAULT_XUID}) == 0);
    CHECK(xam(c, "XamUserIsOnlineEnabled", {0}) == 0);
    CHECK(xam(c, "XamUserGetCachedUserFlags", {0}) == 0);
    CHECK(xam(c, "XamUserGetUserFlags", {0}) == 0);
    CHECK(xam(c, "XamUserGetUserFlagsFromXUID", {DEFAULT_XUID}) == 0);
    CHECK(xam(c, "XamUserIsParentalControlled", {0}) == 0);
    CHECK(xam(c, "XamUserIsGuest", {0}) == 0);
    CHECK(xam(c, "XamUserIsGuest", {7}) == 0);
    // the console's language and country, for the user and for nobody
    CHECK(xam(c, "XamUserGetOnlineLanguageFromXUID", {DEFAULT_XUID}) == 1);
    CHECK(xam(c, "XamUserGetOnlineCountryFromXUID", {DEFAULT_XUID}) == 103);
    CHECK(xam(c, "XamUserGetOnlineCountryFromXUID", {OTHER_XUID}) == 103);

    uint32_t a = local(c, 0x200), b = local(c, 0x204), d = local(c, 0x208);
    K->store32(a, 0xEEEE), K->store32(b, 0xEEEE);
    CHECK(xam(c, "XamUserGetSubscriptionType", {0, a, b}) == X_ERROR_SUCCESS && K->load32(a) == 0 && K->load32(b) == 0);
    CHECK(xam(c, "XamUserGetSubscriptionType", {4, a, b}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserGetSubscriptionType", {0, 0, b}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserGetSubscriptionType", {1, a, b}) == X_E_NO_SUCH_USER);

    // tenure: the dashboard's settings, as written
    K->store32(a, 0xEEEE), K->store32(b, 0xEEEE), K->store64(d, 0xEEEE);
    CHECK(xam(c, "XamUserGetUserTenure", {0, a, b, d, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(a) == 0 && K->load32(b) == 0 && K->load64(d) == 0);
    CHECK(xam(c, "XamUserGetUserTenure", {1, a, b, d, 0}) == X_E_NO_SUCH_USER);
    uint32_t settings = local(c, 0x300);
    std::memset(K->host(settings), 0, 80);
    K->store32(settings + 0x10, 0x10040047); // XPROFILE_TENURE_LEVEL
    K->store32(settings + 0x20, 7);
    K->store32(settings + 40 + 0x10, 0x70080049); // XPROFILE_TENURE_NEXT_MILESTONE_DATE
    K->store64(settings + 40 + 0x20, 0x0123456789ABCDEFull);
    CHECK(xam(c, "XamUserWriteProfileSettings", {0xFFFE07D1, 0, 2, settings, 0}) == X_ERROR_SUCCESS);
    uint32_t ov = local(c, 0x380);
    std::memset(K->host(ov), 0, 0x1C);
    CHECK(xam(c, "XamUserGetUserTenure", {0, a, b, d, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(a) == 7 && K->load32(b) == 0 && K->load64(d) == 0x0123456789ABCDEFull);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_SUCCESS);

    // restrictions: none
    CHECK(xam(c, "XamUserIsUnsafeProgrammingAllowed", {0, 0, a}) == X_ERROR_SUCCESS && K->load32(a) == 1);
    CHECK(xam(c, "XamUserIsUnsafeProgrammingAllowed", {0xFF, 0, a}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserIsUnsafeProgrammingAllowed", {5, 0, a}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserIsUnsafeProgrammingAllowed", {0, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    K->store32(a, 0xEEEE);
    CHECK(xam(c, "XamUserContentRestrictionGetFlags", {0, a}) == X_ERROR_SUCCESS && K->load32(a) == 0);
    CHECK(xam(c, "XamUserContentRestrictionGetFlags", {0xFF, a}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserContentRestrictionGetFlags", {1, a}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam(c, "XamUserContentRestrictionGetFlags", {5, a}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserContentRestrictionGetRating", {0, 0, a, b}) == X_ERROR_NOT_FOUND);
    CHECK(xam(c, "XamUserContentRestrictionGetRating", {0, 1, a, b}) == X_ERROR_SUCCESS && K->load32(a) == 0x3F &&
          K->load32(b) == 0);
    CHECK(xam(c, "XamUserContentRestrictionGetRating", {2, 1, a, b}) == X_ERROR_NO_SUCH_USER);
    K->store32(ov + xoverlapped::RESULT, 0xEEEE);
    CHECK(xam(c, "XamUserContentRestrictionCheckAccess", {0, 0, 1, 0, 0, a, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(a) == 1 && K->load32(ov + xoverlapped::RESULT) == X_ERROR_SUCCESS);
    K->store32(ov + xoverlapped::RESULT, 0xEEEE);
    CHECK(xam(c, "XamUserContentRestrictionCheckAccess", {0, 0, 0, 0, 0, a, ov}) == X_ERROR_SUCCESS);
    CHECK(K->load32(ov + xoverlapped::RESULT) == 0xEEEE);

    // the settings read through the second size pointer, the buffer and
    // overlapped on the stack
    uint32_t ids = local(c, 0x400), size = local(c, 0x404), out = local(c, 0x440);
    K->store32(ids, 0x1004003A); // XPROFILE_GAMER_TIER
    K->store32(size, 0);
    CHECK(xam(c, "XamUserReadProfileSettingsEx", {0, 0, 0, 0, 1, ids, 0, size, 0, 0}) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(K->load32(size) == 48);
    CHECK(xam(c, "XamUserReadProfileSettingsEx", {0, 0, 0, 0, 1, ids, 0, size, out, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(out) == 1 && K->load32(out + 4) == out + 8 && K->load32(out + 8 + 0x10) == 0x1004003A &&
          K->load32(out + 8 + 0x20) == 6);
    CHECK(xam(c, "XamUserReadProfileSettingsEx", {0, 0, 0, 0, 1, ids, 0, 0, out, 0}) == X_ERROR_INVALID_PARAMETER);
}

void preferences(ppc::Context &c, uint32_t listener) {
    uint32_t value = local(c, 0x100), ov = local(c, 0x120), param = 0;
    // (the tenure written before)
    CHECK(listener_next(c, listener, param) == XN_SYS_PROFILESETTINGCHANGED && param == 1);
    CHECK(listener_next(c, listener, param) == 0);
    // never written: the two-bit field without its written bit
    K->store32(value, 0xEEEE);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x006, value, 0}) == X_ERROR_SUCCESS && K->load32(value) == 0);
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x006, 0x002, 0}) == X_ERROR_SUCCESS);
    CHECK(listener_next(c, listener, param) == XN_SYS_PROFILESETTINGCHANGED && param == 1);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x006, value, 0}) == X_ERROR_SUCCESS && K->load32(value) == 0x002);
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x001, 0x001, 0}) == X_ERROR_SUCCESS);
    CHECK(listener_next(c, listener, param) == XN_SYS_PROFILESETTINGCHANGED);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x001, value, 0}) == X_ERROR_SUCCESS && K->load32(value) == 1);
    // the other field kept
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x006, value, 0}) == X_ERROR_SUCCESS && K->load32(value) == 0x002);
    // a written bit, bits outside the mask, an unknown mask
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x006, 0x004, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x001, 0x002, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x003, 0x001, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x003, value, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamUserReadUserPreference", {1, 0x001, value, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamUserReadUserPreference", {4, 0x001, value, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x001, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserWriteUserPreference", {4, 0x001, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(listener_next(c, listener, param) == 0);
    // through an overlapped: the result and extended error there
    std::memset(K->host(ov), 0, 0x1C);
    CHECK(xam(c, "XamUserWriteUserPreference", {0, 0x003, 0x001, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_FUNCTION_FAILED &&
          K->load32(ov + xoverlapped::EXTENDED_ERROR) == X_E_INVALIDARG);
    CHECK(xam(c, "XGetOverlappedExtendedError", {ov}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserReadUserPreference", {0, 0x018, value, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_SUCCESS && K->load32(ov + xoverlapped::EXTENDED_ERROR) == 1);
}

void logon(ppc::Context &c, uint32_t listener) {
    uint32_t xuids = local(c, 0x100), ov = local(c, 0x140), name = local(c, 0x160), param = 0;
    std::memset(K->host(xuids), 0, 32);
    K->store64(xuids, DEFAULT_XUID);
    // out: nobody at 0, a notification with no slots
    CHECK(xam(c, "XamUserLogon", {xuids, 0x8, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserGetSigninState", {0}) == 0);
    CHECK(xam(c, "XamUserGetName", {0, name, 16}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam(c, "XamUserGetGamerTag", {0, name, 16}) == X_E_NO_SUCH_USER);
    CHECK(xam(c, "XamUserNuiGetUserIndexForSignin", {name}) == X_E_FAIL);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 0);
    // the account is still there
    uint32_t account = local(c, 0x200);
    CHECK(xam(c, "XamProfileFindAccount", {DEFAULT_XUID, account, 0}) == X_ERROR_SUCCESS);
    // in again
    CHECK(xam(c, "XamUserLogon", {xuids, 0x4, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserGetSigninState", {0}) == 1);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 1);
    // in while in: out, then in
    std::memset(K->host(ov), 0, 0x1C);
    CHECK(xam(c, "XamUserLogon", {xuids, 0x4, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_SUCCESS);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 0);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 1);
    // an unknown account adds nobody
    K->store64(xuids, OTHER_XUID + 1);
    CHECK(xam(c, "XamUserLogon", {xuids, 0x4, 0}) == X_ERROR_SUCCESS);
    CHECK(listener_next(c, listener, param) == 0);
    // everyone out, then the user in
    CHECK(xam(c, "XamUserLogon", {xuids, 0x2, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserGetSigninState", {0}) == 0);
    K->store64(xuids, DEFAULT_XUID);
    CHECK(xam(c, "XamUserLogon", {xuids, 0x4, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamUserGetSigninState", {0}) == 1);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 0);
    CHECK(listener_next(c, listener, param) == XN_SYS_SIGNINCHANGED && param == 1);
    CHECK(xam(c, "XamUserLogon", {0, 0x4, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserLogonEx", {0, 0, 0}) == X_ERROR_SUCCESS);

    // Kinect
    K->store32(name, 0xEEEE);
    CHECK(xam(c, "XamUserNuiGetUserIndexForSignin", {name}) == X_E_SUCCESS && K->load32(name) == 0);
    CHECK(xam(c, "XamUserNuiGetUserIndexForSignin", {0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserNuiGetUserIndexForBind", {name}) == X_E_FAIL);
    CHECK(xam(c, "XamUserNuiGetUserIndexForBind", {0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserNuiGetUserIndex", {0, name}) == X_E_NO_SUCH_USER);
    CHECK(xam(c, "XamUserNuiEnableBiometric", {0, 1}) == X_E_INVALIDARG);

    // the titles played: none recorded
    uint32_t size = local(c, 0x300), handle = local(c, 0x304), returned = local(c, 0x308);
    CHECK(xam(c, "XamUserCreateTitlesPlayedEnumerator", {0, 0, 0, 0, 5, size, handle}) == X_ERROR_SUCCESS);
    CHECK(K->load32(size) == 5 * 0xA8);
    uint32_t list = K->memory().system_alloc(5 * 0xA8);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, list, 5 * 0xA8, returned, 0}) == X_ERROR_NO_MORE_FILES);
    CHECK(krnl(c, "NtClose", {K->load32(handle)}) == X_STATUS_SUCCESS);
    CHECK(xam(c, "XamUserCreateTitlesPlayedEnumerator", {0, 4, 0, 0, 5, size, handle}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamUserCreateTitlesPlayedEnumerator", {0, 1, 0, 0, 5, size, handle}) == X_ERROR_FUNCTION_FAILED);
}

void profiles(ppc::Context &c) {
    uint32_t account = local(c, 0x100), device = local(c, 0x280);
    // the user's account: its gamertag
    K->store32(device, 0);
    CHECK(xam(c, "XamProfileFindAccount", {DEFAULT_XUID, account, device}) == X_ERROR_SUCCESS);
    CHECK(read_wide(account + 8) == u"User" && K->load32(device) == 1 && K->load32(account) == 0);
    std::memset(K->host(account), 0xEE, 0x17C);
    CHECK(xam(c, "XamProfileFindAccount", {OTHER_XUID, account, device}) == X_ERROR_NO_SUCH_USER);
    CHECK(K->load32(account) == 0 && K->load32(account + 0x178) == 0);
    CHECK(xam(c, "XamProfileFindAccount", {DEFAULT_XUID, 0, device}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamProfileLoadAccountInfo", {1, DEFAULT_XUID, account}) == X_ERROR_SUCCESS);
    CHECK(read_wide(account + 8) == u"User");
    CHECK(xam(c, "XamProfileLoadAccountInfo", {1, 0, account}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamProfileLoadAccountInfo", {1, OTHER_XUID, account}) == X_ERROR_INVALID_PARAMETER);
    // the user has no profile package here to open
    CHECK(xam(c, "XamProfileOpen", {DEFAULT_XUID, 0, 0, 0}) == X_ERROR_INVALID_PARAMETER);

    // a new profile: its creation info, directory and account
    std::memset(K->host(account), 0, 0x17C);
    put_u16(*K, account + 8, u"Made", 16);
    uint32_t info_ptr = local(c, 0x284), xuid = local(c, 0x288);
    CHECK(xam(c, "XamProfileCreate", {0x80000001, device, OTHER_XUID, account, 0, 0, 0, info_ptr}) == X_ERROR_SUCCESS);
    uint32_t info = K->load32(info_ptr);
    CHECK(info && K->load32(info) == 1 && K->load32(info + 4) == 1 && K->load64(info + 0xA78) == OTHER_XUID);
    CHECK(read_wide(info + 8 + 8) == u"Made");
    CHECK(std::filesystem::is_directory(content_root(*K) / "E030000000001234" / "FFFE07D1" / "00010000" /
                                        "E030000000001234"));
    CHECK(xam(c, "XamProfileGetCreationStatus", {info, xuid}) == X_ERROR_SUCCESS && K->load64(xuid) == OTHER_XUID);
    CHECK(xam(c, "XamProfileGetCreationStatus", {0, xuid}) == X_ERROR_SUCCESS);
    // again: the directory is there
    CHECK(xam(c, "XamProfileCreate", {0, 0, OTHER_XUID, account, 0, 0, 0, info_ptr}) == X_ERROR_INVALID_PARAMETER);
    K->memory().system_free(K->load32(info_ptr));
    CHECK(xam(c, "XamProfileFindAccount", {OTHER_XUID, account, device}) == X_ERROR_SUCCESS &&
          read_wide(account + 8) == u"Made");

    // both enumerated, by XUID, one a call
    uint32_t handle = local(c, 0x290), item = K->memory().system_alloc(0x188);
    CHECK(xam(c, "XamProfileCreateEnumerator", {1, handle}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamProfileEnumerate", {K->load32(handle), 0, item, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load64(item) == DEFAULT_XUID && read_wide(item + 8 + 8) == u"User" && K->load32(item + 0x184) == 1);
    uint32_t ov = local(c, 0x2A0);
    std::memset(K->host(ov), 0, 0x1C);
    CHECK(xam(c, "XamProfileEnumerate", {K->load32(handle), 0, item, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_SUCCESS && K->load32(ov + xoverlapped::LENGTH) == 1);
    CHECK(K->load64(item) == OTHER_XUID && read_wide(item + 8 + 8) == u"Made");
    CHECK(xam(c, "XamProfileEnumerate", {K->load32(handle), 0, item, 0}) == X_ERROR_NO_MORE_FILES);
    CHECK(xam(c, "XamProfileEnumerate", {K->load32(handle), 0, item, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov + xoverlapped::RESULT) == X_ERROR_FUNCTION_FAILED);
    CHECK(krnl(c, "NtClose", {K->load32(handle)}) == X_STATUS_SUCCESS);
    CHECK(xam(c, "XamProfileEnumerate", {K->load32(handle), 0, item, 0}) == X_ERROR_INVALID_HANDLE);
    CHECK(xam(c, "XamProfileCreateEnumerator", {1, 0}) == X_ERROR_INVALID_PARAMETER);

    // its package opened under a name, described, closed
    uint32_t mount = local(c, 0x2C0), data = K->memory().system_alloc(0x148);
    std::memcpy(K->host(mount), "prof", 5);
    std::memset(K->host(data), 0xEE, 0x148);
    CHECK(xam(c, "XamProfileOpen", {OTHER_XUID, mount, 0, data}) == X_ERROR_SUCCESS);
    CHECK(file_system(*K).has_link("prof:"));
    CHECK(K->load32(data) == 1 && K->load32(data + 4) == 0x10000 && read_text(data + 0x108) == "E030000000001234" &&
          K->load32(data + 0x140) == 0xFFFE07D1);
    CHECK(xam(c, "XamProfileOpen", {OTHER_XUID, mount, 0, 0}) == X_ERROR_INVALID_PARAMETER); // the name is taken
    std::memcpy(K->host(mount), "PROF", 5);
    CHECK(xam(c, "XamProfileClose", {mount}) == X_ERROR_SUCCESS);
    CHECK(!file_system(*K).has_link("prof:"));
    CHECK(xam(c, "XamProfileClose", {mount}) == X_ERROR_FUNCTION_FAILED);
    // with no name: User_<XUID>
    CHECK(xam(c, "XamProfileOpen", {OTHER_XUID, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(file_system(*K).has_link("User_E030000000001234:"));
    std::memcpy(K->host(mount), "User_E030000000001234", 22);
    CHECK(xam(c, "XamProfileClose", {mount}) == X_ERROR_SUCCESS);
}

// ---- the Win32 functions ----

void errors_and_clock(ppc::Context &c) {
    CHECK(xam(c, "GetCurrentThreadId") == current_thread()->id);
    CHECK(xam(c, "RtlSetLastNTError", {X_STATUS_INVALID_HANDLE_}) == X_ERROR_INVALID_HANDLE);
    CHECK(xam(c, "GetLastError") == X_ERROR_INVALID_HANDLE && xam(c, "RtlGetLastError") == X_ERROR_INVALID_HANDLE);
    CHECK(xam(c, "RtlSetLastNTError", {0}) == 0 && xam(c, "GetLastError") == 0);
    CHECK(xam(c, "XGetOverlappedExtendedError", {0}) == 0);
    uint32_t ov = local(c, 0x100);
    K->store32(ov + xoverlapped::RESULT, X_ERROR_IO_PENDING);
    K->store32(ov + xoverlapped::EXTENDED_ERROR, 0x1234);
    CHECK(xam(c, "XGetOverlappedExtendedError", {ov}) == X_ERROR_IO_INCOMPLETE);
    K->store32(ov + xoverlapped::RESULT, 0);
    CHECK(xam(c, "XGetOverlappedExtendedError", {ov}) == 0x1234);

    // handles
    CHECK(xam(c, "CloseHandle", {0x1234}) == 0 && xam(c, "GetLastError") == X_ERROR_INVALID_HANDLE);
    uint32_t handle_ptr = local(c, 0x120);
    CHECK(krnl(c, "NtCreateEvent", {handle_ptr, 0, 0, 1}) == X_STATUS_SUCCESS);
    CHECK(xam(c, "CloseHandle", {K->load32(handle_ptr)}) == 1);

    // modules: none by that name is 0, the last error untouched (Xenia's
    // XexGetModuleHandle answers a Win32 error, not a status)
    uint32_t name = local(c, 0x140);
    std::memcpy(K->host(name), "xam", 4);
    CHECK(xam(c, "GetModuleHandleA", {name}) != 0);
    std::memcpy(K->host(name), "nothing.xex", 12);
    xam(c, "RtlSetLastNTError", {0});
    CHECK(xam(c, "GetModuleHandleA", {name}) == 0 && xam(c, "GetLastError") == 0);

    // the clock
    uint32_t t0 = xam(c, "GetTickCount");
    xam(c, "Sleep", {20});
    CHECK(xam(c, "GetTickCount") - t0 >= 20);
    uint32_t time = local(c, 0x160);
    xam(c, "GetSystemTimeAsFileTime", {time});
    CHECK(K->load64(time) > 125911584000000000ull);
    CHECK(K->load64(objects(*K).time_stamp_bundle + time_stamp_bundle::SYSTEM_TIME) == K->load64(time));
    xam(c, "GetSystemTimeAsFileTime", {0});
    K->store64(time, 0);
    CHECK(xam(c, "QueryPerformanceFrequency", {time}) == 1 && K->load64(time) == 50'000'000);

    // RtlRandom: the entry the second step picks, replaced by the first
    uint32_t seed = local(c, 0x180);
    K->store32(seed, 1);
    CHECK(xam(c, "RtlRandom", {seed}) == 2052111302 && K->load32(seed) == 0x5A1);
    K->store32(seed, 1);
    CHECK(xam(c, "RtlRandom", {seed}) == 0x7FFFFFB1 && K->load32(seed) == 0x5A1);

    CHECK(xam(c, "lstrlenW", {wide(c, 0x1A0, u"hello")}) == 5);
    CHECK(xam(c, "lstrlenW", {wide(c, 0x1A0, u"")}) == 0);
    CHECK(xam(c, "lstrlenW", {0}) == 0);

    // XamAllocEx: the system heap, or physical memory aligned and zeroed
    uint32_t out = local(c, 0x1C0);
    CHECK(xam(c, "XamAllocEx", {0, 0, 100, out}) == 0 && K->load32(out) != 0);
    xam(c, "XamFree", {K->load32(out)});
    CHECK(xam(c, "XamAllocEx", {0x60000000, 0x40000000, 0x3000, out}) == 0);
    uint32_t p = K->load32(out);
    CHECK(p >= 0xA0000000 && p % 0x1000 == 0);
    bool zero = true;
    for (uint32_t i = 0; i < 0x3000; i += 4) zero = zero && K->load32(p + i) == 0;
    CHECK(zero);
    // (aligned in physical memory)
    CHECK(xam(c, "XamAllocEx", {0x0E000000, 0x40000000, 0x1000, out}) == 0 && K->load32(out) &&
          K->memory().physical_address(K->load32(out)) % 0x4000 == 0);
}

void sleeps_and_waits(ppc::Context &c) {
    CHECK(xam(c, "RtlSleep", {0, 0}) == X_STATUS_SUCCESS);
    CHECK(xam(c, "SleepEx", {1, 1}) == X_STATUS_SUCCESS);
    // an APC ends an alertable sleep
    apcs = 0;
    queue_apc_to_self();
    CHECK(xam(c, "SleepEx", {10'000, 1}) == X_STATUS_USER_APC && apcs == 1);
    queue_apc_to_self();
    CHECK(xam(c, "RtlSleep", {0xFFFFFFFF, 1}) == X_STATUS_USER_APC && apcs == 2);

    uint32_t timeout = local(c, 0x100);
    CHECK(uint32_t(xam(c, "XapiFormatTimeOut", {timeout, 5})) == timeout && int64_t(K->load64(timeout)) == -50'000);
    CHECK(xam(c, "XapiFormatTimeOut", {timeout, 0xFFFFFFFF}) == 0);

    uint32_t handle_ptr = local(c, 0x120);
    CHECK(krnl(c, "NtCreateEvent", {handle_ptr, 0, 0, 1}) == X_STATUS_SUCCESS);
    uint32_t set = K->load32(handle_ptr);
    CHECK(krnl(c, "NtCreateEvent", {handle_ptr, 0, 0, 0}) == X_STATUS_SUCCESS);
    uint32_t unset = K->load32(handle_ptr);
    CHECK(xam(c, "WaitForSingleObject", {set, 0xFFFFFFFF}) == 0);
    CHECK(xam(c, "WaitForSingleObjectEx", {set, 0, 0}) == 0);
    CHECK(xam(c, "WaitForSingleObject", {unset, 10}) == WAIT_TIMEOUT);
    CHECK(xam(c, "WaitForSingleObject", {unset, 0}) == WAIT_TIMEOUT);
    queue_apc_to_self();
    CHECK(xam(c, "WaitForSingleObjectEx", {unset, 10'000, 1}) == WAIT_IO_COMPLETION && apcs == 3);
    xam(c, "RtlSetLastNTError", {0});
    CHECK(xam(c, "WaitForSingleObject", {0x1234, 0}) == WAIT_FAILED &&
          xam(c, "GetLastError") == X_ERROR_INVALID_HANDLE);
    CHECK(xam(c, "CloseHandle", {set}) == 1 && xam(c, "CloseHandle", {unset}) == 1);
}

void threads(ppc::Context &c) {
    uint32_t id_ptr = local(c, 0x100);
    // the parameter to the start address, the id written
    worker_runs = 0;
    uint32_t h = xam(c, "CreateThread", {0, 0, F_WORKER, 0x77, 0, id_ptr});
    CHECK(h != 0);
    CHECK(xam(c, "WaitForSingleObject", {h, 0xFFFFFFFF}) == 0);
    CHECK(worker_runs == 1 && worker_param == 0x77 && worker_r4 == 0 && worker_id == K->load32(id_ptr));
    CHECK(xam(c, "CloseHandle", {h}) == 1);

    // suspended until resumed
    h = xam(c, "CreateThread", {0, 0x8000, F_WORKER, 0x78, 4, 0});
    CHECK(h != 0);
    CHECK(xam(c, "WaitForSingleObject", {h, 30}) == WAIT_TIMEOUT && worker_runs == 1);
    CHECK(xam(c, "ResumeThread", {h}) == 1);
    CHECK(xam(c, "WaitForSingleObject", {h, 0xFFFFFFFF}) == 0 && worker_runs == 2 && worker_param == 0x78);
    CHECK(xam(c, "ResumeThread", {h}) == 0);
    CHECK(xam(c, "CloseHandle", {h}) == 1);
    CHECK(xam(c, "ResumeThread", {h}) == 0xFFFFFFFF && xam(c, "GetLastError") == X_ERROR_INVALID_HANDLE);

    // on a processor
    h = xam(c, "XapipCreateThread", {0, 0, F_WORKER, 0x79, 0, 2, id_ptr});
    CHECK(h != 0);
    CHECK(xam(c, "WaitForSingleObject", {h, 0xFFFFFFFF}) == 0 && worker_runs == 3 && worker_param == 0x79);
    CHECK(xam(c, "CloseHandle", {h}) == 1);

    // ended by ExitThread, its code kept
    h = xam(c, "CreateThread", {0, 0, F_EXITER, 0, 0, 0});
    CHECK(h != 0);
    CHECK(xam(c, "WaitForSingleObject", {h, 0xFFFFFFFF}) == 0);
    if (Ref<Thread> t = objects(*K).lookup<Thread>(h)) CHECK(t->exit_code == 0x42);
    CHECK(xam(c, "CloseHandle", {h}) == 1);
}

void main_thread(ppc::Context &c) {
    c.r[1] -= 0x2000; // a frame: the stack arguments above, the scratch below
    uint32_t listener = xam(c, "XamNotifyCreateListener", {1, 1});
    uint32_t param = 0;
    while (listener_next(c, listener, param)) {
    }
    locale_tables(c);
    dates_and_fonts(c);
    locators_and_strings(c);
    user_flags(c);
    preferences(c, listener);
    logon(c, listener);
    profiles(c);
    errors_and_clock(c);
    sleeps_and_waits(c);
    threads(c);
    c.r[1] += 0x2000;
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_WORKER, worker},
    {F_EXITER, exiter},
    {F_APC, apc},
};

} // namespace

int main() {
    std::error_code ec;
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("xenoforge_xam_user_test_" + std::to_string(rd()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        CHECK(mount_storage(k, dir / "storage", error));
        Objects &o = objects(k);
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
    }
    std::filesystem::remove_all(dir, ec);
    return finish("kernel_xam_user");
}
