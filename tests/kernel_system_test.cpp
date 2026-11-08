// The system track (src/kernel/gpd.cpp, xam_user.cpp, misc.cpp, xam.cpp): GPD
// files read and written; a profile's settings and achievements kept in its
// package across runs (two kernels on one storage root); the console's
// settings the player chooses and the ones a title writes, kept between runs;
// the title ending through ExTerminateTitleProcess and XamLoaderLaunchTitle
// with the terminate notifications run and the exit recorded.

#include "check.h"

#include "gpd.h"
#include "objects.h"
#include "xam.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
std::function<void(ppc::Context &)> scenario;
std::atomic<uint32_t> notified{0}, notified_order{0};

enum : uint32_t { F_MAIN = 0x82000000, F_NOTIFY_LOW = 0x82000010, F_NOTIFY_HIGH = 0x82000020 };

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

void main_thread(ppc::Context &c) {
    scenario(c);
    c.r[3] = 0x600D;
}
// the notifications record the order they ran in (the highest priority first)
void notify_low(ppc::Context &c) { notified |= 1u << (2 * notified_order++), c.r[3] = 0; }
void notify_high(ppc::Context &c) { notified |= 2u << (2 * notified_order++), c.r[3] = 0; }

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_NOTIFY_LOW, notify_low},
    {F_NOTIFY_HIGH, notify_high},
};

// a kernel on this storage root, its scenario run on a guest thread; the
// kernel is handed to `after` before it goes away
void run(const std::filesystem::path &storage, std::function<void(ppc::Context &)> body,
         std::function<void(Kernel &)> after = nullptr, const SystemSettings *settings = nullptr) {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), (void)failures++;
    Kernel k(*memory);
    K = &k;
    scenario = std::move(body);
    k.set_generated(generated, std::size(generated));
    CHECK(mount_storage(k, storage, error));
    if (settings) apply_system_settings(k, *settings);
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
        CHECK(t->exit_code == 0x600D || k.stopped); // a title that ends the run does not return
    }
    if (after) after(k);
}

// ---- the XDBF/GPD files ----

void be16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(uint8_t(x >> 8)), v.push_back(uint8_t(x)); }
void be32(std::vector<uint8_t> &v, uint32_t x) { be16(v, uint16_t(x >> 16)), be16(v, uint16_t(x)); }

void gpd_files() {
    Gpd g;
    g.entries[{gpd_section::SETTING, 7}] = {1, 2, 3};
    g.entries[{gpd_section::ACHIEVEMENT, 2}] = {9};
    g.entries[{gpd_section::IMAGE, 0x8000}] = std::vector<uint8_t>(100, 0x5A);
    std::vector<uint8_t> bytes = g.serialize();
    CHECK(bytes.size() == 24 + 18 * 512 + 8 * 512 + 3 + 1 + 100);
    CHECK(std::memcmp(bytes.data(), "XDBF", 4) == 0);
    Gpd back;
    CHECK(back.parse(bytes.data(), bytes.size()));
    CHECK(back.entries == g.entries);
    CHECK(!back.parse(bytes.data(), 10));
    bytes[0] = 'Y';
    Gpd bad;
    CHECK(!bad.parse(bytes.data(), bytes.size()));

    // an entry table past 512 entries grows by 512
    Gpd many;
    for (uint64_t i = 0; i < 600; i++) many.entries[{gpd_section::STRING, i}] = {uint8_t(i)};
    Gpd many_back;
    CHECK(many_back.parse(many.serialize().data(), many.serialize().size()));
    CHECK(many_back.entries.size() == 600);

    // an achievement entry
    Achievement a;
    a.id = 5, a.image_id = 0x10005, a.gamerscore = 25, a.flags = 3, a.label = u"Win", a.description = u"Win a race",
    a.unachieved = u"A secret";
    Achievement b;
    CHECK(b.decode(a.encode()));
    CHECK(b.id == 5 && b.gamerscore == 25 && b.label == u"Win" && b.unachieved == u"A secret" && !b.unlocked());
    b.flags |= ACHIEVEMENT_ACHIEVED, b.unlock_time = 0x01DB000000000001ull;
    Achievement c;
    CHECK(c.decode(b.encode()) && c.unlocked() && c.unlock_time == b.unlock_time);

    // a profile setting's entry
    uint8_t value[8] = {0, 0, 0, 3};
    uint32_t id;
    uint8_t type, got[8];
    std::vector<uint8_t> data;
    CHECK(decode_setting(encode_setting(0x10040003, 1, value, {7, 8}), id, type, got, data));
    CHECK(id == 0x10040003 && type == 1 && std::memcmp(got, value, 8) == 0 && data == std::vector<uint8_t>({7, 8}));

    // a SPA's achievements: XACH with strings from XSTR, the second language the default
    Gpd spa;
    std::vector<uint8_t> xach;
    be32(xach, 0x58414348), be32(xach, 1), be32(xach, 2 + 72), be16(xach, 2);
    for (uint16_t i = 1; i <= 2; i++) {
        be16(xach, i), be16(xach, 10 * i), be16(xach, 10 * i + 1), be16(xach, 10 * i + 2);
        be32(xach, 0x10000 + i), be16(xach, 10 * i), be16(xach, 0), be32(xach, 3);
        for (int j = 0; j < 4; j++) be32(xach, 0);
    }
    spa.entries[{SPA_METADATA, SPA_ID_XACH}] = xach;
    std::vector<uint8_t> xstc;
    be32(xstc, 0x58535443), be32(xstc, 1), be32(xstc, 4), be32(xstc, 1); // default language: English
    spa.entries[{SPA_METADATA, SPA_ID_XSTC}] = xstc;
    std::vector<uint8_t> xstr;
    be32(xstr, 0x58535452), be32(xstr, 1), be32(xstr, 0), be16(xstr, 6);
    for (uint16_t sid : {10, 11, 12, 20, 21, 22}) {
        std::string text = "s" + std::to_string(sid);
        be16(xstr, sid), be16(xstr, uint16_t(text.size()));
        xstr.insert(xstr.end(), text.begin(), text.end());
    }
    spa.entries[{SPA_STRINGS, 1}] = xstr;
    std::vector<Achievement> list = spa_achievements(spa, 5); // Spanish has no table: English follows
    CHECK(list.size() == 2);
    if (list.size() == 2) {
        CHECK(list[0].id == 1 && list[0].label == u"s10" && list[0].description == u"s11" &&
              list[0].unachieved == u"s12" && list[0].gamerscore == 10 && list[0].image_id == 0x10001);
        CHECK(list[1].id == 2 && list[1].label == u"s20" && list[1].gamerscore == 20);
    }
}

// ---- a profile kept in its package ----

std::filesystem::path profile_dir(const std::filesystem::path &storage) {
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)DEFAULT_XUID);
    return storage / "content" / xuid / "FFFE07D1" / "00010000" / xuid;
}

constexpr uint32_t TITLE = 0x11223344;

void write_setting(ppc::Context &c, uint32_t id, uint32_t value) {
    uint32_t at = K->memory().system_alloc(40);
    std::memset(K->host(at), 0, 40);
    K->store32(at + 0x10, id);
    K->store32(at + 0x20, value);
    CHECK(xam(c, "XamUserWriteProfileSettings", {TITLE, 0, 1, at, 0}) == X_ERROR_SUCCESS);
}

// the value of a setting the title wrote, 0xFFFFFFFF with none
uint32_t read_setting(ppc::Context &c, uint32_t id, uint32_t &source) {
    uint32_t ids = K->memory().system_alloc(4), size_ptr = K->memory().system_alloc(4),
             buffer = K->memory().system_alloc(256);
    K->store32(ids, id), K->store32(size_ptr, 256);
    uint32_t r = xam(c, "XamUserReadProfileSettings", {TITLE, 0, 0, 0, 1, ids, size_ptr, buffer, 0});
    if (r != X_ERROR_SUCCESS) return 0xFFFFFFFF;
    source = K->load32(buffer + 8);
    return K->load32(buffer + 8 + 0x20);
}

void profile_settings(const std::filesystem::path &storage) {
    constexpr uint32_t VIBRATION = 0x10040003; // XPROFILE_OPTION_CONTROLLER_VIBRATION, default 3
    // the first run: the default, then a write
    run(storage, [](ppc::Context &c) {
        uint32_t source = 0;
        CHECK(read_setting(c, VIBRATION, source) == 3 && source == 1);
        write_setting(c, VIBRATION, 0x55);
        CHECK(read_setting(c, VIBRATION, source) == 0x55 && source == 2);
    });
    CHECK(std::filesystem::exists(profile_dir(storage) / "11223344.gpd"));
    // the second run, a new kernel on the same storage: read from the package
    run(storage, [](ppc::Context &c) {
        uint32_t source = 0;
        CHECK(read_setting(c, VIBRATION, source) == 0x55 && source == 2);
    });
    // the file is a GPD with the setting in it
    std::ifstream in(profile_dir(storage) / "11223344.gpd", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Gpd g;
    CHECK(g.parse(bytes.data(), bytes.size()));
    CHECK(g.find(gpd_section::SETTING, VIBRATION) != nullptr);
}

void achievements(const std::filesystem::path &storage) {
    // the profile's GPD for the title has two achievements (a SPA's would add them)
    Gpd g;
    for (uint32_t id : {1u, 2u}) {
        Achievement a;
        a.id = id, a.image_id = 0x10000 + id, a.gamerscore = 10 * id, a.flags = 7;
        a.label = u"Label " + std::u16string(1, char16_t(u'0' + id));
        a.description = u"Done", a.unachieved = u"Not yet";
        g.entries[{gpd_section::ACHIEVEMENT, id}] = a.encode();
    }
    std::filesystem::create_directories(profile_dir(storage));
    {
        std::vector<uint8_t> bytes = g.serialize();
        std::ofstream out(profile_dir(storage) / "00000000.gpd", std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    }
    run(storage, [](ppc::Context &c) {
        CHECK(earn_achievement(*K, 0, 2));
        CHECK(earn_achievement(*K, 0, 2));   // again: still true, nothing changes
        CHECK(!earn_achievement(*K, 0, 99)); // not in the title
        CHECK(!earn_achievement(*K, 3, 1));  // nobody signed in at index 3
        // XGIUserWriteAchievements through the app dispatcher's message is the same call
        // the enumerator, strings behind the records
        uint32_t size_ptr = K->memory().system_alloc(4), handle_ptr = K->memory().system_alloc(4);
        CHECK(xam(c, "XamUserCreateAchievementEnumerator", {0, 0, 0, 7, 0, 4, size_ptr, handle_ptr}) ==
              X_ERROR_SUCCESS);
        CHECK(K->load32(size_ptr) == (36 + 464) * 4);
        uint32_t buffer = K->memory().system_alloc(K->load32(size_ptr)), count_ptr = K->memory().system_alloc(4);
        CHECK(xam(c, "XamEnumerate", {K->load32(handle_ptr), 0, buffer, K->load32(size_ptr), count_ptr, 0}) ==
              X_ERROR_SUCCESS);
        CHECK(K->load32(count_ptr) == 2);
        CHECK(K->load32(buffer + 0) == 1 && K->load32(buffer + 20) == 10 && K->load64(buffer + 24) == 0);
        CHECK(K->load32(buffer + 36 + 0) == 2 && K->load32(buffer + 36 + 20) == 20);
        CHECK(K->load64(buffer + 36 + 24) != 0); // unlocked: its time
        CHECK((K->load32(buffer + 36 + 32) & ACHIEVEMENT_ACHIEVED) != 0 &&
              (K->load32(buffer + 32) & ACHIEVEMENT_ACHIEVED) == 0);
        CHECK(guest_u16(*K, K->load32(buffer + 4)) == u"Label 1");
        CHECK(guest_u16(*K, K->load32(buffer + 8)) == u"Done");
        CHECK(guest_u16(*K, K->load32(buffer + 12)) == u"Not yet");
        CHECK(guest_u16(*K, K->load32(buffer + 36 + 4)) == u"Label 2");
        // nothing left
        CHECK(xam(c, "XamEnumerate", {K->load32(handle_ptr), 0, buffer, K->load32(size_ptr), count_ptr, 0}) ==
              X_ERROR_NO_MORE_FILES);
        // flags 0: no strings; an offset skips items
        CHECK(xam(c, "XamUserCreateAchievementEnumerator", {0, 0, 0, 0, 1, 4, size_ptr, handle_ptr}) ==
              X_ERROR_SUCCESS);
        CHECK(xam(c, "XamEnumerate", {K->load32(handle_ptr), 0, buffer, K->load32(size_ptr), count_ptr, 0}) ==
              X_ERROR_SUCCESS);
        CHECK(K->load32(count_ptr) == 1 && K->load32(buffer) == 2 && K->load32(buffer + 4) == 0);
    });
    // a later run finds it unlocked, in the file
    std::ifstream in(profile_dir(storage) / "00000000.gpd", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Gpd back;
    CHECK(back.parse(bytes.data(), bytes.size()));
    Achievement one, two;
    CHECK(one.decode(*back.find(gpd_section::ACHIEVEMENT, 1)) && !one.unlocked());
    CHECK(two.decode(*back.find(gpd_section::ACHIEVEMENT, 2)) && two.unlocked() && two.unlock_time != 0);
}

// ---- the console's settings ----

void console_settings(const std::filesystem::path &storage) {
    SystemSettings s;
    s.file = storage / "xconfig.bin";
    s.language = 5;
    s.country = 73;
    s.width = 640, s.height = 480;
    s.widescreen = 0;
    s.refresh = 50;
    run(
        storage,
        [](ppc::Context &c) {
            CHECK(xam(c, "XamGetLanguage") == 5);
            CHECK(xam(c, "XamGetCountry") == 73);
            uint32_t mode = K->memory().system_alloc(0x30);
            xam(c, "XGetVideoMode", {mode});
            CHECK(K->load32(mode) == 640 && K->load32(mode + 4) == 480);
            CHECK(K->load32(mode + 0x0C) == 0);          // 4:3, no widescreen flag
            CHECK(K->load32(mode + 0x14) == 0x42480000); // 50.0f
            // a title writes a setting (its video flags: widescreen)
            uint32_t buffer = K->memory().system_alloc(4);
            K->store32(buffer, 0x00010000);
            CHECK(krnl(c, "ExSetXConfigSetting", {3, 0x0A, buffer, 4}) == X_STATUS_SUCCESS);
            xam(c, "XGetVideoMode", {mode});
            CHECK(K->load32(mode + 0x0C) == 1);
        },
        nullptr, &s);
    // the next run, with nothing chosen: the title's setting is still there; the defaults elsewhere
    SystemSettings none;
    none.file = storage / "xconfig.bin";
    run(
        storage,
        [](ppc::Context &c) {
            uint32_t mode = K->memory().system_alloc(0x30);
            xam(c, "XGetVideoMode", {mode});
            CHECK(K->load32(mode + 0x0C) == 1);
            CHECK(K->load32(mode) == 1280 && K->load32(mode + 4) == 720);
            CHECK(xam(c, "XamGetLanguage") == 1);
        },
        nullptr, &none);
    // 60 Hz and 1080 from the player's choice; a file that is not ours is ignored
    {
        std::ofstream out(storage / "garbage.bin", std::ios::binary);
        out << "not a settings file";
    }
    SystemSettings hd;
    hd.file = storage / "garbage.bin";
    hd.width = 1920, hd.height = 1080, hd.refresh = 60;
    run(
        storage,
        [](ppc::Context &c) {
            uint32_t mode = K->memory().system_alloc(0x30);
            xam(c, "XGetVideoMode", {mode});
            CHECK(K->load32(mode) == 1920 && K->load32(mode + 4) == 1080);
            CHECK(K->load32(mode + 0x14) == 0x42700000); // 60.0f
        },
        nullptr, &hd);
}

// ---- the title ends ----

void title_ends(const std::filesystem::path &storage) {
    // ExTerminateTitleProcess: the notifications run, the highest priority first, then the run ends
    notified = 0, notified_order = 0;
    run(
        storage,
        [](ppc::Context &c) {
            uint32_t low = K->memory().system_alloc(8), high = K->memory().system_alloc(8);
            K->store32(low, F_NOTIFY_LOW), K->store32(low + 4, 1);
            K->store32(high, F_NOTIFY_HIGH), K->store32(high + 4, 9);
            krnl(c, "ExRegisterTitleTerminateNotification", {low, 1});
            krnl(c, "ExRegisterTitleTerminateNotification", {high, 1});
            krnl(c, "ExTerminateTitleProcess", {7});
            CHECK(!"ExTerminateTitleProcess returned");
        },
        [](Kernel &k) {
            CHECK(k.stopped);
            CHECK(k.title_exit.exit_code == 7 && !k.title_exit.launch);
            CHECK(notified_order == 2);
            CHECK(notified == (2u | (1u << 2))); // the high one first (bit 1), then the low (bit 2)
        });
    // XamLoaderLaunchTitle: the next title's path, flags and launch data are kept for the host
    notified = 0, notified_order = 0;
    run(
        storage,
        [](ppc::Context &c) {
            uint32_t data = K->memory().system_alloc(8), name = K->memory().system_alloc(32);
            std::memcpy(K->host(data), "\x01\x02\x03\x04", 4);
            std::strcpy(reinterpret_cast<char *>(K->host(name)), "game:\\Next.xex");
            xam(c, "XamLoaderSetLaunchData", {data, 4});
            xam(c, "XamLoaderLaunchTitle", {name, 0x10});
            CHECK(!"XamLoaderLaunchTitle returned");
        },
        [](Kernel &k) {
            CHECK(k.stopped);
            CHECK(k.title_exit.launch && k.title_exit.path == "game:\\Next.xex" && k.title_exit.flags == 0x10);
            CHECK(k.title_exit.launch_data == std::vector<uint8_t>({1, 2, 3, 4}));
        });
    // a launch with no name is the dashboard; XamLoaderTerminateTitle ends with no launch
    run(
        storage,
        [](ppc::Context &c) {
            xam(c, "XamLoaderTerminateTitle");
            CHECK(!"XamLoaderTerminateTitle returned");
        },
        [](Kernel &k) { CHECK(k.stopped && !k.title_exit.launch); });
    // the launch data the host hands the next title is what it reads
    run(storage, [](ppc::Context &c) {
        uint32_t size_ptr = K->memory().system_alloc(4), buffer = K->memory().system_alloc(8);
        set_launch_data(*K, {9, 8, 7});
        CHECK(xam(c, "XamLoaderGetLaunchDataSize", {size_ptr}) == X_ERROR_SUCCESS && K->load32(size_ptr) == 3);
        CHECK(xam(c, "XamLoaderGetLaunchData", {buffer, 8}) == X_ERROR_SUCCESS);
        CHECK(K->host(buffer)[0] == 9 && K->host(buffer)[2] == 7);
    });
}

// ---- the xboxkrnl exports XAM imports (NT's own) ----

void nt_exports(const std::filesystem::path &storage) {
    run(storage, [](ppc::Context &c) {
        // RtlAppendStringToString: the source after the destination's text; too long is refused whole
        uint32_t dst = K->memory().system_alloc(8), src = K->memory().system_alloc(8);
        uint32_t dst_buf = K->memory().system_alloc(8), src_buf = K->memory().system_alloc(8);
        std::memcpy(K->host(dst_buf), "ab", 2), std::memcpy(K->host(src_buf), "cdef", 4);
        K->store16(dst, 2), K->store16(dst + 2, 8), K->store32(dst + 4, dst_buf);
        K->store16(src, 4), K->store16(src + 2, 4), K->store32(src + 4, src_buf);
        CHECK(krnl(c, "RtlAppendStringToString", {dst, src}) == X_STATUS_SUCCESS);
        CHECK(K->load16(dst) == 6 && std::memcmp(K->host(dst_buf), "abcdef", 6) == 0);
        CHECK(krnl(c, "RtlAppendStringToString", {dst, src}) == 0xC0000023);
        CHECK(K->load16(dst) == 6);
        CHECK(krnl(c, "RtlAppendStringToString", {dst, 0}) == X_STATUS_SUCCESS && K->load16(dst) == 6);
    });
}

} // namespace

int main() {
    std::error_code ec;
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("xenoforge_system_test_" + std::to_string(rd()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    gpd_files();
    profile_settings(dir / "a");
    achievements(dir / "b");
    console_settings(dir / "c");
    title_ends(dir / "d");
    nt_exports(dir / "e");
    std::filesystem::remove_all(dir, ec);
    return finish("kernel_system");
}
