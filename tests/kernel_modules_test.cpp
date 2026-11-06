// The modules family (src/kernel/modules.cpp), the Rtl functions (rtl.cpp)
// and the small exports (misc.cpp), called as a game calls them on a kernel
// that loaded a made-up executable, against what Xenia's code gives. The
// second executables are DLLs the test builds itself (XEX2 files, neither
// encrypted nor compressed, as parse_xex and load_xex_image take any), in a
// temporary directory mounted as the game's: loaded, their imports bound
// (the kernel's, and one DLL's from the other), their entry points called,
// their exports found, unloaded and loaded again.

#include "kernel_harness.h"

#include "objects.h"
#include "xenoforge/xex.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

namespace xf::ppc {
[[noreturn]] void unwind(Context &c); // ppc_generated.h
}

using namespace xf;

namespace {

constexpr uint32_t BASE = 0x82000000;
// XEX_HEADER_CHECKSUM_TIMESTAMP, and the time stamps the test's images have
constexpr uint32_t CHECKSUM_TIMESTAMP = 0x00018002, EXE_STAMP = 0x4A5B6C7D, DLL_STAMP = 0x4B000001;

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    v[at] = uint8_t(x >> 24), v[at + 1] = uint8_t(x >> 16), v[at + 2] = uint8_t(x >> 8), v[at + 3] = uint8_t(x);
}
void put16(std::vector<uint8_t> &v, size_t at, uint16_t x) { v[at] = uint8_t(x >> 8), v[at + 1] = uint8_t(x); }
void put_le32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    v[at] = uint8_t(x), v[at + 1] = uint8_t(x >> 8), v[at + 2] = uint8_t(x >> 16), v[at + 3] = uint8_t(x >> 24);
}

// An executable with what the modules read: system flags, the entry point,
// the base address (a value in the header), the stack size, TLS, two
// resources, an export table in the image
Xex make_xex() {
    Xex xex;
    std::vector<uint8_t> &h = xex.header;
    h.assign(0x800, 0);
    std::memcpy(h.data(), "XEX2", 4);
    put32(h, 0x04, 1);     // module flags: a title
    put32(h, 0x08, 0x800); // header size
    put32(h, 0x10, 0x400); // security info
    const std::pair<uint32_t, uint32_t> headers[] = {
        {xex_key::SYSTEM_FLAGS, 0x00000440}, {xex_key::ENTRY_POINT, BASE + 0x1000},
        {xex_key::IMAGE_BASE_ADDRESS, BASE}, {xex_key::DEFAULT_STACK_SIZE, 0x00041234},
        {xex_key::TLS_INFO, 0x200},          {xex_key::RESOURCE_INFO, 0x300},
        {CHECKSUM_TIMESTAMP, 0x500},
    };
    put32(h, 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put32(h, 0x18 + 8 * i, headers[i].first);
        put32(h, 0x1C + 8 * i, headers[i].second);
        xex.opt_headers.push_back(headers[i]);
    }
    put32(h, 0x200, 64); // TLS: slots, raw data, data size, raw data size
    put32(h, 0x204, BASE + 0x3000);
    put32(h, 0x208, 0x100);
    put32(h, 0x20C, 0x80);
    put32(h, 0x300, 4 + 2 * 16); // resources
    std::memcpy(&h[0x304], "ABCD1234", 8);
    put32(h, 0x30C, BASE + 0x2000);
    put32(h, 0x310, 0x100);
    std::memcpy(&h[0x314], "XSTR", 4);
    put32(h, 0x31C, BASE + 0x2100);
    put32(h, 0x320, 0x20);
    put32(h, 0x500, 0x0BADF00D); // checksum, time stamp
    put32(h, 0x504, EXE_STAMP);
    put32(h, 0x400 + 4, 0x10000);           // image size
    put32(h, 0x400 + 0x160, BASE + 0x4000); // the export table
    xex.header_size = 0x800;
    xex.security_offset = 0x400;
    xex.module_flags = 1;
    xex.image_size = 0x10000;
    xex.base_address = xex.load_address = BASE;
    xex.entry_point = BASE + 0x1000;
    xex.image.assign(0x10000, 0);
    // the export table: image base 0x8200 << 16, two exports from ordinal 1
    put32(xex.image, 0x4020, 0x8200);
    put32(xex.image, 0x4024, 2);
    put32(xex.image, 0x4028, 1);
    put32(xex.image, 0x402C, 0x5000);
    put32(xex.image, 0x4030, 0x5100);
    return xex;
}

void modules(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // the executable's handle in the variable at Xenia's address; its loader
    // entry; its header in guest memory
    uint32_t handle_var = k.variable("XexExecutableModuleHandle");
    CHECK(handle_var == 0x802080D4);
    uint32_t exe = k.load32(handle_var);
    CHECK(exe != 0);
    uint32_t header = k.load32(exe + 0x58);
    CHECK(header != 0 && std::memcmp(k.host(header), "XEX2", 4) == 0 && k.load32(header + 0x400 + 4) == 0x10000);
    CHECK(k.load32(exe + 0x1C) == BASE && k.load32(exe + 0x38) == 0x10000 && k.load32(exe + 0x3C) == BASE + 0x1000);
    CHECK(k.load32(exe + 0x18) == 0 && k.load16(exe + 0x40) == 0);
    CHECK(k.load32(exe + 0x50) == EXE_STAMP && k.load32(exe + 0x48) == 0); // the time stamp, no checksum

    // RtlImageXexHeaderField: a value, the address of one, the address of data
    CHECK(h.krnl("RtlImageXexHeaderField", {header, xex_key::SYSTEM_FLAGS}) == 0x440);
    uint32_t base_field = h.krnl("RtlImageXexHeaderField", {header, xex_key::IMAGE_BASE_ADDRESS});
    CHECK(base_field == header + 0x18 + 8 * 2 + 4 && k.load32(base_field) == BASE);
    CHECK(h.krnl("RtlImageXexHeaderField", {header, xex_key::TLS_INFO}) == header + 0x200);
    CHECK(h.krnl("RtlImageXexHeaderField", {header, xex_key::EXECUTION_INFO}) == 0);
    CHECK(h.krnl("RtlImageXexHeaderField", {0, xex_key::SYSTEM_FLAGS}) == 0);

    // XexCheckExecutablePrivilege: a bit of the system flags
    CHECK(h.krnl("XexCheckExecutablePrivilege", {6}) == 1);
    CHECK(h.krnl("XexCheckExecutablePrivilege", {10}) == 1);
    CHECK(h.krnl("XexCheckExecutablePrivilege", {7}) == 0);

    // XexGetModuleHandle: by name, path, or none for the executable
    uint32_t out = h.alloc(4);
    CHECK(h.krnl("XexGetModuleHandle", {0, out}) == 0 && k.load32(out) == exe);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("default.xex"), out}) == 0 && k.load32(out) == exe);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("game:\\default.xex"), out}) == 0 && k.load32(out) == exe);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("xam.xex"), out}) == 0);
    uint32_t xam = k.load32(out);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("XBOXKRNL.EXE"), out}) == 0);
    uint32_t krnl = k.load32(out);
    CHECK(xam && krnl && xam != krnl && xam != exe && krnl != exe);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("xboxkrnl"), out}) == 0 && k.load32(out) == krnl);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("kernel32.dll"), out}) == 0x490 && k.load32(out) == 0);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("other.xex"), out}) == 0x490);

    // XexGetModuleSection: the executable's resources
    uint32_t data = h.alloc(4), size = h.alloc(4);
    CHECK(h.krnl("XexGetModuleSection", {exe, h.string("ABCD1234"), data, size}) == 0);
    CHECK(k.load32(data) == BASE + 0x2000 && k.load32(size) == 0x100);
    CHECK(h.krnl("XexGetModuleSection", {exe, h.string("XSTR"), data, size}) == 0);
    CHECK(k.load32(data) == BASE + 0x2100 && k.load32(size) == 0x20);
    CHECK(h.krnl("XexGetModuleSection", {exe, h.string("XST"), data, size}) == 0xC0000225);
    CHECK(h.krnl("XexGetModuleSection", {xam, h.string("XSTR"), data, size}) == kernel::X_STATUS_UNSUCCESSFUL);
    CHECK(h.krnl("XexGetModuleSection", {0x1234, h.string("XSTR"), data, size}) == 0xC0000008);

    // XexGetProcedureAddress: trampolines in the kernel modules' room
    // (xboxkrnl's at 0x80040000, xam's at 0x80050000), variables at their
    // address, the executable's export table, nothing for no implementation
    CHECK(h.krnl("XexGetProcedureAddress", {xam, 0x3CB /* XGetAVPack */, out}) == 0);
    uint32_t avpack = k.load32(out);
    CHECK(avpack == 0x80050000);
    CHECK(h.krnl("XexGetProcedureAddress", {xam, 0x3CB, out}) == 0 && k.load32(out) == avpack);
    k.call(*h.c, avpack);
    CHECK(uint32_t(h.c->r[3]) == 8);
    CHECK(h.krnl("XexGetProcedureAddress", {krnl, 0x0CC /* NtAllocateVirtualMemory */, out}) == 0);
    CHECK(k.load32(out) == 0x80040000);
    CHECK(h.krnl("XexGetProcedureAddress", {krnl, 0x158 /* XboxKrnlVersion */, out}) == 0);
    CHECK(k.load32(out) == 0x8004045C);
    CHECK(h.krnl("XexGetProcedureAddress", {krnl, 0x00D /* ExCreateThread */, out}) == 0xC0000263 ||
          kernel::Kernel::implemented("xboxkrnl", "ExCreateThread"));
    CHECK(h.krnl("XexGetProcedureAddress", {0, 1, out}) == 0 && k.load32(out) == BASE + 0x5000);
    CHECK(h.krnl("XexGetProcedureAddress", {exe, 2, out}) == 0 && k.load32(out) == BASE + 0x5100);
    CHECK(h.krnl("XexGetProcedureAddress", {exe, 3, out}) == 0xC0000263 && k.load32(out) == 0);
    CHECK(h.krnl("XexGetProcedureAddress", {0x1234, 1, out}) == 0xC0000008);

    // XexLoadImage of a loaded module: its handle, counted; XexUnloadImage
    CHECK(h.krnl("XexLoadImage", {h.string("xam.xex"), 8, 0, out}) == 0 && k.load32(out) == xam);
    CHECK(k.load16(xam + 0x40) == 1);
    CHECK(h.krnl("XexLoadImage", {h.string("default.xex"), 8, 0, out}) == 0 && k.load16(exe + 0x40) == 1);
    CHECK(h.krnl("XexUnloadImage", {exe}) == 0 && k.load16(exe + 0x40) == 0);
    CHECK(h.krnl("XexUnloadImage", {xam}) == 0 && k.load16(xam + 0x40) == 1); // kernel modules stay
    CHECK(h.krnl("XexLoadImage", {h.string("game:\\other.xex"), 8, 0, out}) == 0xC000000F && k.load32(out) == 0);
    CHECK(h.krnl("XexLoadImage", {h.string("xam.xex"), 8, 0, 0}) == 0x57);
    CHECK(h.krnl("XexUnloadImage", {0x1234}) == 0xC0000008);

    // ExRegisterTitleTerminateNotification: kept, no result
    uint32_t reg = h.alloc(16);
    k.store32(reg, 0x82001234);
    k.store32(reg + 4, 5);
    h.krnl("ExRegisterTitleTerminateNotification", {reg, 1});
    h.krnl("ExRegisterTitleTerminateNotification", {reg, 0});
}

void variables(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // at Xenia's addresses, with Xenia's values
    uint32_t version = k.variable("XboxKrnlVersion");
    CHECK(version == 0x8004045C && k.load64(version) == 0x0002000007600000ull);
    uint32_t hardware = k.variable("XboxHardwareInfo");
    CHECK(hardware == 0x801D0030 && k.load32(hardware) == 0x20 && k.host(hardware + 4)[0] == 6);
    CHECK(k.variable("KeDebugMonitorData") == 0x80207A64 && k.load32(0x80207A64) == 0);
    CHECK(k.variable("KeCertMonitorData") == 0x80207A60 && k.load32(0x80207A60) == 0);
    CHECK(k.variable("ExLoadedCommandLine") == 0x80207C60 && h.read(0x80207C60) == "\"default.xex\"");
    // the executable's path: game:\default.xex resolved (the test's directory)
    CHECK(k.variable("ExLoadedImageName") == 0x80207E60 &&
          h.read(0x80207E60) == "\\Device\\Harddisk0\\Partition1\\default.xex");
    CHECK(k.variable("ExConsoleGameRegion") == 0x80205150 && k.load16(0x80205150) == 0xFFFF);
    CHECK(k.variable("VdGpuClockInMHz") == 0x801D0E94 && k.load32(0x801D0E94) == 500);
    uint32_t device = k.load32(k.variable("VdGlobalDevice"));
    CHECK(k.variable("VdGlobalDevice") == 0x801E6FC4 && device >= 0xE0000000);
    CHECK(k.variable("VdGlobalXamDevice") == 0x801E6FC8 && k.load32(0x801E6FC8) == 0);
    uint32_t hsio = k.variable("VdHSIOCalibrationLock");
    CHECK(hsio == 0x801D1210 && k.host(hsio)[0] == 1 && k.host(hsio)[1] == 40 && k.load32(hsio + 0x10) == 0xFFFFFFFF);

    // the object types, in the globals: pool tags and procedures
    uint32_t thread_type = k.variable("ExThreadObjectType"), event_type = k.variable("ExEventObjectType");
    CHECK(event_type == thread_type + 0x1C && k.variable("ObSymbolicLinkObjectType") == thread_type + 9 * 0x1C);
    CHECK(k.load32(thread_type + 0x18) == 0x65726854 && k.load32(event_type + 0x18) == 0x76657645);
    CHECK(k.load32(k.variable("IoFileObjectType") + 0x14) == 0x38);
    CHECK(k.load32(k.variable("IoDeviceObjectType") + 0x14) == thread_type + 0x118); // the default object
    CHECK(k.host(thread_type + 0x118)[0] == 1 && k.load32(thread_type + 0x118 + 4) == 1);
    h.c->r[3] = 0x40, h.c->r[4] = 0x54534554, h.c->r[5] = 0;
    k.call(*h.c, k.load32(event_type)); // allocate
    uint32_t object = uint32_t(h.c->r[3]);
    CHECK(object && (object & 0xFFF) == 8 && k.load32(object - 4) == 0x54534554);
    h.c->r[3] = object;
    k.call(*h.c, k.load32(event_type + 4)); // free
    h.c->r[3] = 0x40, h.c->r[4] = 0x65726854, h.c->r[5] = 0;
    k.call(*h.c, k.load32(thread_type)); // a thread: 8 more, 2 on
    uint32_t thread = uint32_t(h.c->r[3]);
    CHECK((thread & 0xFFF) == 10);

    // the processes: idle, title (from the header), system
    uint32_t title = thread_type + 0x188, system = thread_type + 0x1E8, idle = thread_type + 0x128;
    CHECK(k.host(idle)[0x2F] == 0 && k.load32(idle + 0x0C) == 0x7F);
    CHECK(k.host(system)[0x2F] == 2 && k.host(system)[0x18] == 2 && k.load16(system + 0x2C) == 32 * 4);
    CHECK(k.load32(system + 0x30) == 0xFFFFFFFF && k.load32(system + 0x34) == 0);
    CHECK(k.host(title)[0x2F] == 1 && k.host(title)[0x18] == 10 && k.host(title)[0x19] == 13);
    CHECK(k.load32(title + 0x04) == title + 4 && k.load32(title + 0x0C) == 60);
    CHECK(k.load32(title + 0x20) == BASE + 0x3000 && k.load32(title + 0x24) == 0x100 &&
          k.load32(title + 0x28) == 0x100);
    CHECK(k.load16(title + 0x2C) == 64 * 4 && k.load32(title + 0x30) == 0xFFFFFFFF &&
          k.load32(title + 0x34) == 0xFFFFFFFF);
    CHECK(k.load32(title + 0x38) == 0);
    CHECK(k.load32(title + 0x1C) == 0x42000); // the stack size, to pages

    // KeTimeStampBundle at Xenia's address, filled as the kernel starts (the
    // scheduler's clock moves it on once threads run: kernel_threads_test)
    uint32_t bundle = k.variable("KeTimeStampBundle");
    CHECK(bundle == 0x80240EE0);
    uint64_t system_time = k.load64(bundle + 8);
    CHECK(system_time <= k.system_time() && k.system_time() - system_time < 10'000'000);
}

void strings(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t s = h.alloc(8);
    uint32_t text = h.string("Hello");
    h.krnl("RtlInitAnsiString", {s, text});
    CHECK(k.load16(s) == 5 && k.load16(s + 2) == 6 && k.load32(s + 4) == text);
    h.krnl("RtlInitAnsiString", {s, 0});
    CHECK(k.load16(s) == 0 && k.load16(s + 2) == 0 && k.load32(s + 4) == 0);
    uint32_t wide = h.wide(u"Wé!");
    CHECK(h.krnl("RtlInitUnicodeString", {s, wide}) == s);
    CHECK(k.load16(s) == 6 && k.load16(s + 2) == 8 && k.load32(s + 4) == wide);

    // RtlUnicodeStringToAnsiString: UTF-8; allocated, or into a buffer
    uint32_t ansi = h.alloc(8);
    CHECK(h.krnl("RtlUnicodeStringToAnsiString", {ansi, s, 1}) == 0);
    CHECK(k.load16(ansi) == 4 && k.load16(ansi + 2) == 5 && h.read(k.load32(ansi + 4)) == "W\xC3\xA9!");
    h.krnl("RtlFreeAnsiString", {ansi});
    CHECK(k.load32(ansi + 4) == 0);
    uint32_t buffer = h.alloc(16);
    std::memset(k.host(buffer), 'x', 16);
    k.store16(ansi + 2, 3), k.store32(ansi + 4, buffer);
    CHECK(h.krnl("RtlUnicodeStringToAnsiString", {ansi, s, 0}) == 0x80000005);
    CHECK(h.read(buffer) == "W\xC3");

    // the conversions, byte for byte
    uint32_t written = h.alloc(4), out = h.alloc(32);
    CHECK(h.krnl("RtlMultiByteToUnicodeN", {out, 6, written, text, 5}) == 0);
    CHECK(k.load32(written) == 6 && k.load16(out) == 'H' && k.load16(out + 4) == 'l' && k.load16(out + 6) == 0);
    CHECK(h.krnl("RtlUnicodeToMultiByteN", {out + 16, 8, written, h.wide(u"a€b"), 6}) == 0);
    CHECK(k.load32(written) == 3 && std::memcmp(k.host(out + 16), "a?b", 3) == 0);

    // comparisons
    CHECK(int32_t(h.krnl("RtlCompareStringN", {h.string("abcD"), 0xFFFFFFFF, h.string("ABCd"), 0xFFFFFFFF, 1})) == 0);
    CHECK(int32_t(h.krnl("RtlCompareStringN", {h.string("abc"), 3, h.string("abd"), 3, 0})) == 'c' - 'd');
    CHECK(int32_t(h.krnl("RtlCompareStringN", {h.string("ab"), 2, h.string("abc"), 3, 0})) == -1);
    CHECK(h.krnl("RtlUpcaseUnicodeChar", {'q'}) == 'Q');
    CHECK(h.krnl("RtlUpperChar", {0xE9}) == 0xC9 && h.krnl("RtlUpperChar", {0xFF}) == 0x3F);
    CHECK(h.krnl("RtlLowerChar", {'A'}) == 'a');

    // memory
    uint32_t m = h.alloc(16);
    h.krnl("RtlFillMemoryUlong", {m, 12, 0x11223344});
    CHECK(k.load32(m) == 0x11223344 && k.load32(m + 8) == 0x11223344 && k.load32(m + 12) == 0);
    CHECK(h.krnl("RtlCompareMemoryUlong", {m, 16, 0x11223344}) == 12);
    CHECK(h.krnl("RtlCompareMemoryUlong", {m, 7, 0x11223344}) == 4);
    CHECK(h.krnl("RtlCompareMemory", {m, m + 8, 8}) == 4);
}

void time(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t t = h.alloc(8), f = h.alloc(16);
    auto fields = [&](uint64_t filetime) {
        k.store64(t, filetime);
        h.krnl("RtlTimeToTimeFields", {t, f});
        std::array<uint16_t, 8> v;
        for (int i = 0; i < 8; i++) v[i] = k.load16(f + 2 * i);
        return v;
    };
    // 1601-01-01, a Monday
    CHECK((fields(0) == std::array<uint16_t, 8>{1601, 1, 1, 0, 0, 0, 0, 1}));
    // 1970-01-01, a Thursday
    CHECK((fields(116444736000000000ull) == std::array<uint16_t, 8>{1970, 1, 1, 0, 0, 0, 0, 4}));
    // 2000-01-01, a Saturday; then 2000-02-29 12:34:56.789, a Tuesday
    CHECK((fields(125911584000000000ull) == std::array<uint16_t, 8>{2000, 1, 1, 0, 0, 0, 0, 6}));
    uint64_t leap = 125911584000000000ull + (59ull * 86400 + 12 * 3600 + 34 * 60 + 56) * 10'000'000 + 7'890'000;
    CHECK((fields(leap) == std::array<uint16_t, 8>{2000, 2, 29, 12, 34, 56, 789, 2}));
    // and back
    k.store64(t, 0);
    CHECK(h.krnl("RtlTimeFieldsToTime", {f, t}) == 1 && k.load64(t) == leap);
    k.store16(f + 4, 30); // February 30th
    CHECK(h.krnl("RtlTimeFieldsToTime", {f, t}) == 0);
    k.store16(f + 0, 1600), k.store16(f + 4, 1);
    CHECK(h.krnl("RtlTimeFieldsToTime", {f, t}) == 0);
    k.store16(f + 0, 1601), k.store16(f + 2, 1), k.store16(f + 4, 1);
    for (int i = 3; i < 7; i++) k.store16(f + 2 * i, 0);
    CHECK(h.krnl("RtlTimeFieldsToTime", {f, t}) == 1 && k.load64(t) == 0);

    // the clock
    CHECK(h.krnl("KeQueryPerformanceFrequency") == 50'000'000);
    h.krnl("KeQuerySystemTime", {t});
    uint64_t now = k.load64(t);
    CHECK(now > 133'000'000'000'000'000ull && k.system_time() - now < 10'000'000);
    // (the timer may have moved the bundle on since)
    uint64_t bundle_time = k.load64(k.variable("KeTimeStampBundle") + 8);
    CHECK(bundle_time >= now && bundle_time - now < 10'000'000);
    uint64_t interrupt = h.call("xboxkrnl", "KeQueryInterruptTime");
    CHECK(interrupt != 0 && interrupt <= k.interrupt_time() && k.interrupt_time() - interrupt < 10'000'000);
}

void status_codes(KernelHarness &h) {
    auto dos = [&](uint32_t status) { return h.krnl("RtlNtStatusToDosError", {status}); };
    CHECK(dos(0) == 0);
    CHECK(dos(0x00000103) == 0x3E5);      // pending: ERROR_IO_PENDING
    CHECK(dos(0xC0000022) == 5);          // access denied
    CHECK(dos(0xD0000022) == 5);          // the high bits do not matter
    CHECK(dos(0xC0000034) == 2);          // object name not found
    CHECK(dos(0xC000000D) == 0x57);       // invalid parameter
    CHECK(dos(0xC0000017) == 8);          // no memory
    CHECK(dos(0x80000006) == 0x12);       // no more files
    CHECK(dos(0xC000001D) == 0xC000001D); // kept as the table has it
    CHECK(dos(0x80070005) == 5);          // a Win32 HRESULT
    CHECK(dos(0xC0010004) == 4);          // 0xC001xxxx: the low half
    CHECK(dos(0xE0000001) == 0xE0000001); // a customer code
    CHECK(dos(0x40000000) == 317);        // none
    CHECK(dos(0xC0000263) == 0x7F);       // driver entry point not found
}

void printf_family(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t buffer = h.alloc(256);
    double pi = 3.14159;
    uint64_t pi_bits;
    std::memcpy(&pi_bits, &pi, 8);
    // sprintf: two fixed arguments, then nine in r5-r10 and on the stack
    uint32_t format = h.string("%d|%5s|%-4x|%04X|%c|%%|%p|%lld|%.2f!");
    int32_t n = int32_t(h.krnl("sprintf", {buffer, format, uint64_t(int64_t(-42)), h.string("ab"), 0xAB, 0x1F, 'z',
                                           0x1234, 0x123456789ull, pi_bits}));
    std::string expected = "-42|   ab|ab  |001F|z|%|00001234|4886718345|3.14!";
    CHECK(h.read(buffer) == expected && n == int32_t(expected.size()));
    // strings: (null), precision, wide
    n = int32_t(h.krnl("sprintf", {buffer, h.string("[%s][%.3s][%S][%ls][%#x][%#o][%+d][% d]"), 0, h.string("abcdef"),
                                   h.wide(u"wide"), h.wide(u"ls"), 0x1F, 8, 5, 7}));
    expected = "[(null)][abc][wide][ls][0x1f][010][+5][ 7]";
    CHECK(h.read(buffer) == expected && n == int32_t(expected.size()));
    // _snprintf: what fits, -1 when it does not
    std::memset(k.host(buffer), 'x', 16);
    CHECK(int32_t(h.krnl("_snprintf", {buffer, 4, h.string("hello")})) == -1);
    CHECK(std::memcmp(k.host(buffer), "hellx", 5) == 0);
    CHECK(int32_t(h.krnl("_snprintf", {buffer, 16, h.string("%u-%i"), 7, uint64_t(int64_t(-3))})) == 4);
    CHECK(h.read(buffer) == "7--3");
    CHECK(int32_t(h.krnl("_snprintf", {buffer, 0, h.string("x")})) == -1);
    // _vsnprintf: the arguments in an array of 8-byte slots
    uint32_t args = h.alloc(24);
    k.store64(args, 12), k.store64(args + 8, h.string("go")), k.store64(args + 16, 0xBEEF);
    CHECK(int32_t(h.krnl("_vsnprintf", {buffer, 64, h.string("%03d %s %X"), args})) == 11);
    CHECK(h.read(buffer) == "012 go BEEF");
    CHECK(int32_t(h.krnl("vsprintf", {buffer, h.string("%c%c"), args})) == 2);
    // DbgPrint: logged
    CHECK(h.krnl("DbgPrint", {h.string("test %d\n"), 1}) == 0);
    CHECK(h.krnl("DbgPrint", {0}) == kernel::X_STATUS_INVALID_PARAMETER);
}

void settings(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t buffer = h.alloc(16), required = h.alloc(2);
    // the language: English
    CHECK(h.krnl("ExGetXConfigSetting", {3, 9, buffer, 4, required}) == 0);
    CHECK(k.load32(buffer) == 1 && k.load16(required) == 4);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 9, buffer, 2, required}) == 0xC0000023 && k.load16(required) == 0);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 9, 0, 0, required}) == 0 && k.load16(required) == 4);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 9, 0, 4, required}) == 0xC00000F1);
    // the AV region, the country, the screen size
    CHECK(h.krnl("ExGetXConfigSetting", {2, 2, buffer, 16, required}) == 0 && k.load32(buffer) == 0x00400100);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 0x0E, buffer, 16, required}) == 0);
    CHECK(k.host(buffer)[0] == 103 && k.load16(required) == 1);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 0x13, buffer, 4, required}) == 0 && k.load32(buffer) == 0x050002D0);
    // unknown: size 0
    CHECK(h.krnl("ExGetXConfigSetting", {3, 0x99, buffer, 4, required}) == 0 && k.load16(required) == 0);
    CHECK(h.krnl("ExGetXConfigSetting", {0x0B, 1, buffer, 4, 0}) == 0);
    // xam's queries of them
    CHECK(h.call32("xam", "XGetAVPack") == 8);
    CHECK(h.call32("xam", "XGetGameRegion") == 0xFF);
    CHECK(h.call32("xam", "XGetLanguage") == 1);
}

void process_type(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // the thread's (every guest thread has its KTHREAD in the PCR)
    uint32_t thread = h.alloc(0x100);
    k.store32(h.pcr + 0x100, thread);
    k.host(thread + 0x73)[0] = 2;
    CHECK(h.krnl("KeGetCurrentProcessType") == 2);
    h.krnl("KeEnableFpuExceptions", {1});
    CHECK(k.host(thread + 0x71)[0] == 1);
    // in a DPC: the PCR's
    k.store32(h.pcr + 0x150, 1);
    h.krnl("KeSetCurrentProcessType", {0});
    CHECK(h.krnl("KeGetCurrentProcessType") == 0);
    k.store32(h.pcr + 0x150, 0);
    k.store32(h.pcr + 0x100, 0);
}

void debugging(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // SetThreadName and others: logged, returning
    uint32_t record = h.alloc(0x50);
    k.store32(record, 0x406D1388);
    k.store32(record + 0x14, 0x1000);
    k.store32(record + 0x18, h.string("Main"));
    k.store32(record + 0x1C, 0xFFFFFFFF);
    h.krnl("RtlRaiseException", {record});
    k.store32(record, 0x80000003);
    h.krnl("RtlRaiseException", {record});
    h.krnl("DbgBreakPoint");
    // no microphone: not connected, the completion called at once
    uint32_t device = h.alloc(0x40);
    CHECK(h.krnl("MicDeviceRequest", {0}) == kernel::X_STATUS_INVALID_PARAMETER);
    k.store16(device, 0x0000), k.store16(device + 2, 0), k.store32(device + 4, 5);
    CHECK(h.krnl("MicDeviceRequest", {device}) == 0 && k.load32(device + 4) == 0);
    k.store16(device, 0x0009);
    CHECK(h.krnl("MicDeviceRequest", {device}) == 0x103 && k.load32(device + 0x18) == 0x100 &&
          k.load16(device + 0x1C) == 1);
    // the completion: guest code (stw r4, 0x20(r3); blr) run from the
    // export, which still returns to its caller with its own result
    uint32_t completion = h.alloc(8);
    k.store32(completion, 0x90830020);
    k.store32(completion + 4, 0x4E800020);
    k.store32(device + 0x10, completion);
    uint64_t sp = h.c->r[1];
    CHECK(h.krnl("MicDeviceRequest", {device}) == 0x103);
    CHECK(k.load32(device + 0x20) == kernel::X_STATUS_UNSUCCESSFUL && h.c->r[1] == sp);
    k.store32(device + 0x10, 0);
    k.store16(device, 0x000C);
    CHECK(h.krnl("MicDeviceRequest", {device}) == 0xC0000010);
    k.store16(device, 0x0001), k.store16(device + 2, 5);
    CHECK(h.krnl("MicDeviceRequest", {device}) == kernel::X_STATUS_INVALID_PARAMETER);
}

// ---- second executables: two DLLs ----
//
// lib.dll at LIB imports KeQueryPerformanceFrequency; its export 1 returns
// that plus 7, its export 2 is a word of data. app.dll at APP imports from
// lib.dll (export 1's thunk and descriptor, export 2, and ordinals 7 and 9
// lib.dll has not) and from the kernel (KeQueryPerformanceFrequency,
// XboxHardwareInfo); its export 1 calls lib.dll's through the import thunk
// and adds 0x100. Each DllMain appends (hinstance, reason, reserved, its
// id) to a log in guest memory.

constexpr uint32_t LIB = 0x88000000, APP = 0x89000000;
constexpr uint32_t FUNCTION = 0x1000, ENTRY = 0x1800, IMPORTS = 0x3000, DATA = 0x4000, IMAGE_SIZE = 0x10000;
constexpr uint32_t LIB_ID = 0x11, APP_ID = 0x22;
constexpr uint16_t KE_QUERY_PERFORMANCE_FREQUENCY = 0x83, XBOX_HARDWARE_INFO = 0x156;

// the instructions the DLLs' code uses
constexpr uint32_t MFLR_R0 = 0x7C0802A6, MTLR_R0 = 0x7C0803A6, MTCTR_R11 = 0x7D6903A6, BCTR = 0x4E800420;
constexpr uint32_t BLR = 0x4E800020;
uint32_t li(uint32_t d, int16_t v) { return 14u << 26 | d << 21 | uint16_t(v); }
uint32_t addi(uint32_t d, uint32_t a, int16_t v) { return 14u << 26 | d << 21 | a << 16 | uint16_t(v); }
uint32_t lis(uint32_t d, uint16_t v) { return 15u << 26 | d << 21 | v; }
uint32_t ori(uint32_t a, uint32_t s, uint16_t v) { return 24u << 26 | s << 21 | a << 16 | v; }
uint32_t lwz(uint32_t d, int16_t offset, uint32_t a) { return 32u << 26 | d << 21 | a << 16 | uint16_t(offset); }
uint32_t stw(uint32_t s, int16_t offset, uint32_t a) { return 36u << 26 | s << 21 | a << 16 | uint16_t(offset); }
uint32_t stwu(uint32_t s, int16_t offset, uint32_t a) { return 37u << 26 | s << 21 | a << 16 | uint16_t(offset); }
uint32_t slwi(uint32_t a, uint32_t s, uint32_t n) { return 21u << 26 | s << 21 | a << 16 | n << 11 | (31 - n) << 1; }
uint32_t add(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 266u << 1; }
uint32_t bl(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC) | 1; }

void put_code(std::vector<uint8_t> &image, uint32_t at, std::initializer_list<uint32_t> code) {
    for (uint32_t word : code) put32(image, at, word), at += 4;
}

// An import thunk as the linker leaves it: the record (type 1, the ordinal),
// a word the loader overwrites, mtctr r11, bctr
void put_thunk(std::vector<uint8_t> &image, uint32_t at, uint16_t ordinal) {
    put_code(image, at, {0x01000000u | ordinal, 0, MTCTR_R11, BCTR});
}

struct ImportLibrary {
    std::string name;
    std::vector<uint32_t> records;
};

// The image: MZ; the XEX export table at 0x100 (two exports from ordinal 1:
// the function, the data); a PE export directory at 0x200 (little-endian,
// its tables relative to it) naming them LibFunction and LibData; the
// DllMain; the data word.
std::vector<uint8_t> make_image(uint32_t base, uint32_t id, uint32_t log) {
    std::vector<uint8_t> image(IMAGE_SIZE, 0);
    image[0] = 'M', image[1] = 'Z';
    put32(image, 0x120, base >> 16);
    put32(image, 0x124, 2);
    put32(image, 0x128, 1);
    put32(image, 0x12C, FUNCTION);
    put32(image, 0x130, DATA);
    put_le32(image, 0x214, 2);    // functions
    put_le32(image, 0x218, 2);    // names
    put_le32(image, 0x21C, 0x40); // the functions' addresses
    put_le32(image, 0x220, 0x48); // the names
    put_le32(image, 0x224, 0x50); // the names' ordinals
    put_le32(image, 0x240, FUNCTION);
    put_le32(image, 0x244, DATA);
    put_le32(image, 0x248, 0x60);
    put_le32(image, 0x24C, 0x70);
    image[0x250] = 0, image[0x252] = 1;
    std::memcpy(&image[0x260], "LibFunction", 12);
    std::memcpy(&image[0x270], "LibData", 8);
    // DllMain(hinstance, reason, reserved): the log's count at log, its
    // entries of 16 bytes from log + 4
    put_code(image, ENTRY,
             {lis(11, uint16_t(log >> 16)), ori(11, 11, uint16_t(log)), lwz(12, 0, 11), slwi(10, 12, 4),
              add(10, 10, 11), stw(3, 4, 10), stw(4, 8, 10), stw(5, 12, 10), li(9, int16_t(id)), stw(9, 16, 10),
              addi(12, 12, 1), stw(12, 0, 11), li(3, 1), BLR});
    put32(image, DATA, 0xCAFEF00D);
    return image;
}

// A DLL as the XDK links one, neither encrypted nor compressed: a header of
// 0x1000 bytes (module flags title and DLL; entry point, base, file format,
// import libraries, exports by name), its security info at 0x400 (image
// size, load address, export table; no page descriptors), then the image.
std::vector<uint8_t> make_dll(uint32_t base, const std::vector<uint8_t> &image,
                              const std::vector<ImportLibrary> &imports) {
    std::vector<uint8_t> f(0x1000, 0);
    std::memcpy(f.data(), "XEX2", 4);
    put32(f, 0x04, 0x9);
    put32(f, 0x08, 0x1000);
    put32(f, 0x10, 0x400);
    const std::pair<uint32_t, uint32_t> headers[] = {
        {xex_key::ENTRY_POINT, base + ENTRY},      {xex_key::IMAGE_BASE_ADDRESS, base},
        {xex_key::FILE_FORMAT_INFO, 0x600},        {xex_key::IMPORT_LIBRARIES, 0x700},
        {0x00E10402 /* exports by name */, 0x610}, {CHECKSUM_TIMESTAMP, 0x680},
    };
    put32(f, 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put32(f, 0x18 + 8 * i, headers[i].first);
        put32(f, 0x1C + 8 * i, headers[i].second);
    }
    put32(f, 0x400 + 0x004, uint32_t(image.size()));
    put32(f, 0x400 + 0x110, base);
    put32(f, 0x400 + 0x160, base + 0x100);
    put32(f, 0x600, 8); // no encryption, no compression
    put32(f, 0x610, 0x200);
    put32(f, 0x614, 0x80);
    put32(f, 0x680, 0x12345678);
    put32(f, 0x684, DLL_STAMP);
    // the import libraries: the names (each padded to 4), then each one's
    // records
    std::vector<uint8_t> names;
    for (const ImportLibrary &library : imports) {
        names.insert(names.end(), library.name.begin(), library.name.end());
        names.push_back(0);
        while (names.size() % 4) names.push_back(0);
    }
    std::memcpy(&f[0x70C], names.data(), names.size());
    size_t at = 0x70C + names.size();
    for (size_t i = 0; i < imports.size(); i++) {
        uint32_t size = 0x28 + 4 * uint32_t(imports[i].records.size());
        put32(f, at, size);
        put16(f, at + 0x24, uint16_t(i));
        put16(f, at + 0x26, uint16_t(imports[i].records.size()));
        for (size_t r = 0; r < imports[i].records.size(); r++) put32(f, at + 0x28 + 4 * r, imports[i].records[r]);
        at += size;
    }
    put32(f, 0x700, uint32_t(at - 0x700));
    put32(f, 0x704, uint32_t(names.size()));
    put32(f, 0x708, uint32_t(imports.size()));
    f.insert(f.end(), image.begin(), image.end());
    return f;
}

std::vector<uint8_t> make_lib(uint32_t log) {
    std::vector<uint8_t> image = make_image(LIB, LIB_ID, log);
    // export 1: KeQueryPerformanceFrequency() + 7
    put_code(image, FUNCTION,
             {MFLR_R0, stw(0, -8, 1), stwu(1, -0x60, 1), bl(LIB + FUNCTION + 12, LIB + IMPORTS + 0x10), addi(3, 3, 7),
              addi(1, 1, 0x60), lwz(0, -8, 1), MTLR_R0, BLR});
    put32(image, IMPORTS, KE_QUERY_PERFORMANCE_FREQUENCY);
    put_thunk(image, IMPORTS + 0x10, KE_QUERY_PERFORMANCE_FREQUENCY);
    return make_dll(LIB, image, {{"xboxkrnl.exe", {LIB + IMPORTS, LIB + IMPORTS + 0x10}}});
}

std::vector<uint8_t> make_app(uint32_t log) {
    std::vector<uint8_t> image = make_image(APP, APP_ID, log);
    // export 1: lib.dll's export 1 + 0x100, through the import thunk
    put_code(image, FUNCTION,
             {MFLR_R0, stw(0, -8, 1), stwu(1, -0x60, 1), bl(APP + FUNCTION + 12, APP + IMPORTS + 0x10),
              addi(3, 3, 0x100), addi(1, 1, 0x60), lwz(0, -8, 1), MTLR_R0, BLR});
    put32(image, IMPORTS + 0x00, 1);
    put_thunk(image, IMPORTS + 0x10, 1);
    put32(image, IMPORTS + 0x20, 2);
    put32(image, IMPORTS + 0x24, 7);
    put32(image, IMPORTS + 0x28, 9);
    put_thunk(image, IMPORTS + 0x30, 9);
    put32(image, IMPORTS + 0x40, KE_QUERY_PERFORMANCE_FREQUENCY);
    put_thunk(image, IMPORTS + 0x50, KE_QUERY_PERFORMANCE_FREQUENCY);
    put32(image, IMPORTS + 0x60, XBOX_HARDWARE_INFO);
    return make_dll(APP, image,
                    {{"lib.dll",
                      {APP + IMPORTS, APP + IMPORTS + 0x10, APP + IMPORTS + 0x20, APP + IMPORTS + 0x24,
                       APP + IMPORTS + 0x28, APP + IMPORTS + 0x30}},
                     {"xboxkrnl.exe", {APP + IMPORTS + 0x40, APP + IMPORTS + 0x50, APP + IMPORTS + 0x60}}});
}

// The generated code per module: lib.dll's table, made from its image, has
// its export 1 as host code that does what the guest code does (the kernel's
// import through the thunk, plus 7) and counts its calls; app.dll's table
// names the same base and size and another digest (another version of the
// module), so app.dll's code stays interpreted and its "generated" export
// never runs.
std::atomic<int> lib_generated_calls{0};
std::atomic<bool> app_generated_ran{false};
kernel::Kernel *modules_kernel = nullptr;
void lib_function(ppc::Context &c) {
    lib_generated_calls++;
    modules_kernel->dispatch(c, LIB + IMPORTS + 0x10);
    c.r[3] = uint32_t(c.r[3]) + 7;
}
void app_function(ppc::Context &c) {
    app_generated_ran = true;
    c.r[3] = 0;
}
const ppc::GeneratedFunction lib_functions[] = {{LIB + FUNCTION, lib_function}};
const ppc::GeneratedFunction app_functions[] = {{APP + FUNCTION, app_function}};
ppc::GeneratedModule lib_module{"lib.dll", LIB, 0, 0, lib_functions, 1};
ppc::GeneratedModule app_module{"app.dll", APP, 0, 0, app_functions, 1};
const ppc::GeneratedModule *const generated_modules[] = {&lib_module, &app_module};

// a module's table for a DLL file, its digest as the kernel computes it
void made_from(ppc::GeneratedModule &m, const std::vector<uint8_t> &file) {
    Xex x;
    std::string error;
    CHECK(parse_xex(file, x, error) && load_xex_image(file, x, error));
    m.size = uint32_t(x.image.size());
    m.digest = image_digest(x.image);
}

struct EntryCall {
    uint32_t hinstance, reason, reserved, id;
    bool operator==(const EntryCall &) const = default;
};

// the DllMain calls logged since `from`
std::vector<EntryCall> entry_calls(kernel::Kernel &k, uint32_t log, uint32_t from = 0) {
    std::vector<EntryCall> calls;
    for (uint32_t i = from; i < k.load32(log); i++) {
        uint32_t e = log + 16 * i;
        calls.push_back({k.load32(e + 4), k.load32(e + 8), k.load32(e + 12), k.load32(e + 16)});
    }
    return calls;
}

uint32_t ansi(KernelHarness &h, const char *s) {
    uint32_t a = h.alloc(8), n = uint32_t(std::strlen(s));
    h.k->store16(a, uint16_t(n));
    h.k->store16(a + 2, uint16_t(n + 1));
    h.k->store32(a + 4, h.string(s));
    return a;
}

void user_modules(KernelHarness &h, uint32_t log, const std::vector<uint8_t> &lib_file) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(4);
    constexpr uint32_t ATTACH = 1, DETACH = 0, THREAD_ATTACH = 2, THREAD_DETACH = 3;

    // XexLoadImage of app.dll: lib.dll, which it imports from, loaded with
    // it; both DllMains on this thread, lib.dll's first, each with its
    // HMODULE; app.dll's load counted
    CHECK(h.krnl("XexLoadImage", {h.string("game:\\app.dll"), 9, 0, out}) == 0);
    uint32_t app = k.load32(out);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("lib.dll"), out}) == 0);
    uint32_t lib = k.load32(out);
    CHECK(app && lib && app != lib);
    CHECK((entry_calls(k, log) == std::vector<EntryCall>{{lib, ATTACH, 0, LIB_ID}, {app, ATTACH, 0, APP_ID}}));
    CHECK(k.load16(app + 0x40) == 1 && k.load16(lib + 0x40) == 0);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("game:\\LIB.DLL"), out}) == 0 && k.load32(out) == lib);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("app"), out}) == 0 && k.load32(out) == app);

    // the loader entry and the header in guest memory
    uint32_t header = k.load32(app + 0x58);
    CHECK(header && std::memcmp(k.host(header), "XEX2", 4) == 0 && k.load32(header + 8) == 0x1000);
    CHECK(k.load32(app + 0x1C) == APP && k.load32(app + 0x38) == IMAGE_SIZE && k.load32(app + 0x3C) == APP + ENTRY);
    CHECK(k.load32(app + 0x18) == 0 && k.load32(app + 0x50) == DLL_STAMP && k.load32(lib + 0x50) == DLL_STAMP);
    CHECK(h.krnl("RtlImageXexHeaderField", {header, xex_key::ENTRY_POINT}) == APP + ENTRY);
    // the image at its base, its pages taken
    CHECK(k.host(APP)[0] == 'M' && k.load32(APP + DATA) == 0xCAFEF00D);
    RegionInfo region;
    CHECK(k.memory().heap_for(APP)->query(APP, region) && region.state != 0 && region.allocation_base == APP);

    // the imports: lib.dll's function descriptor and data get their
    // addresses, its function's thunk jumps there (lis r11 / ori r11 before
    // the thunk's own mtctr r11; bctr), what lib.dll lacks gets 0xF00DF00D or
    // a thunk to nothing; the kernel's as the executable's
    CHECK(k.load32(APP + IMPORTS) == LIB + FUNCTION);
    CHECK(k.load32(APP + IMPORTS + 0x10) == lis(11, LIB >> 16) &&
          k.load32(APP + IMPORTS + 0x14) == ori(11, 11, FUNCTION));
    CHECK(k.load32(APP + IMPORTS + 0x18) == MTCTR_R11 && k.load32(APP + IMPORTS + 0x1C) == BCTR);
    CHECK(k.load32(APP + IMPORTS + 0x20) == LIB + DATA);
    CHECK(k.load32(APP + IMPORTS + 0x24) == 0xF00DF00D && k.load32(APP + IMPORTS + 0x28) == 0xF00DF00D);
    CHECK(k.load32(APP + IMPORTS + 0x30) == 0x44000042 && k.load32(APP + IMPORTS + 0x34) == BLR);
    CHECK(k.load32(APP + IMPORTS + 0x40) == 0xDEADC0DE);
    CHECK(k.load32(APP + IMPORTS + 0x50) == 0x44000042 && k.load32(APP + IMPORTS + 0x58) == 0x60000000);
    CHECK(k.load32(APP + IMPORTS + 0x60) == k.variable("XboxHardwareInfo"));
    CHECK(k.load32(LIB + IMPORTS + 0x10) == 0x44000042);

    // the code runs: app.dll's export (interpreted: its table was made from
    // another image) through its thunk into lib.dll's (its generated code:
    // dispatch takes the thunk to the export), which calls the kernel through
    // its own
    CHECK(h.krnl("XexGetProcedureAddress", {app, 1, out}) == 0 && k.load32(out) == APP + FUNCTION);
    k.call(*h.c, APP + FUNCTION);
    CHECK(uint32_t(h.c->r[3]) == 50'000'000 + 7 + 0x100);
    CHECK(lib_generated_calls == 1 && !app_generated_ran);
    CHECK(uint32_t(h.c->r[11]) == LIB + FUNCTION && uint32_t(h.c->ctr) == LIB + FUNCTION); // as the thunk leaves them
    std::string code_report = kernel::modules_report(k);
    CHECK(code_report.find("\\lib.dll: 0x88000000-0x88010000, entry 0x88001800, a DLL, attached, loaded 0 time(s); "
                           "generated code, 1 functions") != std::string::npos);
    CHECK(code_report.find("\\app.dll: 0x89000000-0x89010000, entry 0x89001800, a DLL, attached, loaded 1 time(s); "
                           "interpreted: its image is not the one app.dll's generated code was made from") !=
          std::string::npos);
    CHECK(kernel::module_at(k, LIB + FUNCTION) == "lib.dll" && kernel::module_at(k, 0x8F000000).empty());
    k.call(*h.c, APP + IMPORTS + 0x30); // lib.dll's ordinal 9: no such export
    CHECK(uint32_t(h.c->r[3]) == 0 && k.unimplemented().count("lib!lib_009") == 1);

    // XexGetProcedureAddress: the export table by ordinal, the PE export
    // directory by name
    CHECK(h.krnl("XexGetProcedureAddress", {lib, 1, out}) == 0 && k.load32(out) == LIB + FUNCTION);
    CHECK(h.krnl("XexGetProcedureAddress", {lib, 2, out}) == 0 && k.load32(out) == LIB + DATA);
    CHECK(h.krnl("XexGetProcedureAddress", {lib, 3, out}) == 0xC0000263 && k.load32(out) == 0);
    CHECK(h.krnl("XexGetProcedureAddress", {lib, h.string("LibData"), out}) == 0 && k.load32(out) == LIB + DATA);
    CHECK(h.krnl("XexGetProcedureAddress", {lib, h.string("LibFunction"), out}) == 0);
    CHECK(k.load32(out) == LIB + FUNCTION);
    CHECK(h.krnl("XexGetProcedureAddress", {lib, h.string("LibNothing"), out}) == 0xC0000263);

    // loaded already: found by a bare name or another case, counted, no
    // DllMain again
    CHECK(h.krnl("XexLoadImage", {h.string("lib.dll"), 9, 0, out}) == 0 && k.load32(out) == lib);
    CHECK(h.krnl("XexLoadImage", {h.string("GAME:\\App.dll"), 9, 0, out}) == 0 && k.load32(out) == app);
    CHECK(k.load16(lib + 0x40) == 1 && k.load16(app + 0x40) == 2 && k.load32(log) == 2);

    // a guest thread: each DLL's DllMain on it as it starts and as it ends
    // (in the modules' order: app.dll was listed before lib.dll loaded)
    k.store32(BASE + 0x6000, li(3, 0x55));
    k.store32(BASE + 0x6004, BLR);
    uint32_t handle_ptr = h.alloc(4), id_ptr = h.alloc(4);
    CHECK(h.krnl("ExCreateThread", {handle_ptr, 0, id_ptr, 0, BASE + 0x6000, 0, 0}) == 0);
    uint32_t thread = k.load32(handle_ptr);
    {
        kernel::Objects &o = k.objects();
        kernel::Ref<kernel::Thread> t = o.lookup<kernel::Thread>(thread);
        CHECK(t);
        std::unique_lock<std::mutex> l(o.lock);
        CHECK(t && o.thread_exited.wait_for(l, std::chrono::seconds(30), [&] { return t->finished; }));
        CHECK(t && t->exit_code == 0x55);
    }
    CHECK(h.krnl("NtClose", {thread}) == 0);
    CHECK((entry_calls(k, log, 2) == std::vector<EntryCall>{{app, THREAD_ATTACH, 0, APP_ID},
                                                            {lib, THREAD_ATTACH, 0, LIB_ID},
                                                            {app, THREAD_DETACH, 0, APP_ID},
                                                            {lib, THREAD_DETACH, 0, LIB_ID}}));

    // XexUnloadImage: counted down; at none, DllMain(DLL_PROCESS_DETACH) on
    // this thread, then gone: its pages released
    CHECK(h.krnl("XexUnloadImage", {app}) == 0 && k.load16(app + 0x40) == 1 && k.load32(log) == 6);
    CHECK(h.krnl("XexUnloadImage", {app}) == 0);
    CHECK((entry_calls(k, log, 6) == std::vector<EntryCall>{{app, DETACH, 0, APP_ID}}));
    CHECK(h.krnl("XexGetModuleHandle", {h.string("app.dll"), out}) == 0x490);
    CHECK(h.krnl("XexUnloadImage", {app}) == 0xC0000008);
    CHECK(k.memory().heap_for(APP)->query(APP, region) && region.state == 0);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("lib.dll"), out}) == 0 && k.load32(out) == lib);
    CHECK(h.krnl("XexUnloadImage", {lib}) == 0);
    CHECK((entry_calls(k, log, 7) == std::vector<EntryCall>{{lib, DETACH, 0, LIB_ID}}));
    CHECK(h.krnl("XexGetModuleHandle", {h.string("lib.dll"), out}) == 0x490);
    CHECK(k.memory().heap_for(LIB)->query(LIB, region) && region.state == 0);
    // its generated code gone with it
    CHECK(!k.try_call(*h.c, LIB + FUNCTION));

    // and loaded again where they were, lib.dll's generated code with it
    CHECK(h.krnl("XexLoadImage", {h.string("app.dll"), 9, 0, out}) == 0);
    app = k.load32(out);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("lib.dll"), out}) == 0);
    lib = k.load32(out);
    CHECK((entry_calls(k, log, 8) == std::vector<EntryCall>{{lib, ATTACH, 0, LIB_ID}, {app, ATTACH, 0, APP_ID}}));
    k.call(*h.c, APP + FUNCTION);
    CHECK(uint32_t(h.c->r[3]) == 50'000'000 + 7 + 0x100);
    CHECK(lib_generated_calls == 2 && !app_generated_ran);
    std::string report = kernel::modules_report(k);
    CHECK(report.find("\\app.dll: 0x89000000-0x89010000, entry 0x89001800, a DLL, attached, loaded 1 time(s)") !=
          std::string::npos);
    CHECK(report.find("\\lib.dll: unloaded") != std::string::npos);

    // what cannot be loaded: no file, no executable
    k.store32(out, 0x1234);
    CHECK(h.krnl("XexLoadImage", {h.string("game:\\missing.dll"), 9, 0, out}) == 0xC000000F && k.load32(out) == 0);
    CHECK(h.krnl("XexLoadImage", {h.string("junk.dll"), 9, 0, out}) == 0xC000000F && k.load32(out) == 0);

    // XexLoadImageHeaders: the headers as the file has them, into a buffer
    // they fit in
    uint32_t buffer = h.alloc(0x2000);
    CHECK(h.krnl("XexLoadImageHeaders", {ansi(h, "game:\\lib.dll"), buffer, 0x2000}) == 0);
    CHECK(std::memcmp(k.host(buffer), lib_file.data(), 0x1000) == 0);
    CHECK(h.krnl("XexLoadImageHeaders", {ansi(h, " game:\\lib.dll "), buffer, 0x1000}) == 0);
    CHECK(h.krnl("XexLoadImageHeaders", {ansi(h, "game:\\lib.dll"), buffer, 0x800}) == 0xC0000023);
    CHECK(h.krnl("XexLoadImageHeaders", {ansi(h, "game:\\lib.dll"), buffer, 0x7FF}) == 0xC0000023);
    CHECK(h.krnl("XexLoadImageHeaders", {ansi(h, "game:\\junk.dll"), buffer, 0x2000}) == 0xC000007B);
    CHECK(int32_t(h.krnl("XexLoadImageHeaders", {ansi(h, "game:\\missing.dll"), buffer, 0x2000})) < 0);
}

// ---- the loader lock: gate.dll, its DllMain host code ----
//
// gate.dll at GATE has its DllMain in the generated-code table: it logs each
// call (HMODULE, reason, the thread) on the host side, and in some phases
// stays in the call (until a flag or a timeout) or calls ExitThread. Its
// guest code is the other DLLs' and never runs.

constexpr uint32_t GATE = 0x8A000000;
constexpr uint32_t F_LOADER = BASE + 0x7000; // a thread that loads gate.dll (generated)
constexpr uint32_t PROCESS_DETACH = 0, PROCESS_ATTACH = 1, THREAD_ATTACH = 2, THREAD_DETACH = 3;

kernel::Kernel *K = nullptr;

struct GateCall {
    uint32_t hmodule, reason;
    int64_t thread;   // the guest thread's id, -1 the test's own (host) thread
    bool saw = false; // the other thread's load or unload ended during this call
    bool operator==(const GateCall &) const = default;
};

struct Gate {
    std::mutex mutex;
    std::vector<GateCall> calls;
    // 1: a thread's DLL_THREAD_DETACH stays until the unload returns (or
    // 300 ms); 2: DLL_PROCESS_DETACH stays until the loader thread's load
    // returns (or 300 ms); 3: DLL_THREAD_ATTACH and DETACH call ExitThread
    std::atomic<int> phase{0};
    std::atomic<bool> in_thread_detach{false}, unload_returned{false};
    std::atomic<bool> loader_running{false}, in_process_detach{false}, load_returned{false};
    uint32_t name = 0, out = 0; // the loader thread's XexLoadImage arguments
    std::atomic<uint32_t> load_status{0}, loaded{0};

    std::vector<GateCall> since(size_t from) {
        std::lock_guard l(mutex);
        return std::vector<GateCall>(calls.begin() + std::min(from, calls.size()), calls.end());
    }
    size_t count() {
        std::lock_guard l(mutex);
        return calls.size();
    }
} gate;

// polls a flag until it is set or the time is up; whether it was set
bool wait_flag(const std::atomic<bool> &flag, std::chrono::milliseconds limit) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (!flag && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag;
}

int64_t thread_id() {
    kernel::Thread *t = kernel::current_thread();
    return t ? int64_t(t->id) : -1;
}

void gate_main(ppc::Context &c) {
    GateCall call{uint32_t(c.r[3]), uint32_t(c.r[4]), thread_id()};
    int phase = gate.phase;
    if (phase == 1 && call.reason == THREAD_DETACH) {
        gate.in_thread_detach = true;
        call.saw = wait_flag(gate.unload_returned, std::chrono::milliseconds(300));
    } else if (phase == 2 && call.reason == PROCESS_DETACH) {
        gate.in_process_detach = true;
        call.saw = wait_flag(gate.load_returned, std::chrono::milliseconds(300));
    }
    {
        std::lock_guard l(gate.mutex);
        gate.calls.push_back(call);
    }
    if (phase == 3 && (call.reason == THREAD_ATTACH || call.reason == THREAD_DETACH))
        throw kernel::ThreadExit{call.reason == THREAD_ATTACH ? 0x77u : 0x78u}; // what ExTerminateThread does
    c.r[3] = 1;
}

// a guest thread that loads gate.dll once its DLL_PROCESS_DETACH runs on
// another thread
void loader_thread(ppc::Context &c) {
    gate.loader_running = true;
    wait_flag(gate.in_process_detach, std::chrono::seconds(30));
    c.r[3] = gate.name, c.r[4] = 9, c.r[5] = 0, c.r[6] = gate.out;
    kernel::find_export_fn("xboxkrnl", "XexLoadImage")(*K, c);
    gate.load_status = uint32_t(c.r[3]);
    gate.loaded = K->load32(gate.out);
    gate.load_returned = true;
    c.r[3] = 0;
}

// ---- a longjmp across generated code ----
//
// Guest code at J, interpreted: SETJMP keeps r1 and LR in a buffer and
// returns 0; LONGJMP takes them back (r1 loaded from the buffer: discovery
// leaves such a function to the interpreter) and returns r4 to where SETJMP
// was called. S sets a jump with SETJMP, then calls X, a host function in the
// generated-code table that pushes a frame and calls LONGJMP as generated
// code does (LR set, dispatch); the longjmp must come back into S, never
// into X. S2 longjmps to a buffer whose r1 lies above the whole call (a
// fiber's stack above the thread's): the kernel goes on interpreting there.

void put_code_at(kernel::Kernel &k, uint32_t at, std::initializer_list<uint32_t> code) {
    for (uint32_t word : code) k.store32(at, word), at += 4;
}

constexpr uint32_t J = BASE + 0x8000;
constexpr uint32_t SETJMP = J, LONGJMP = J + 0x20, S = J + 0x40, X = J + 0x100, S2 = J + 0x140, G = J + 0x180;
constexpr uint32_t X2 = J + 0x1C0, G2 = J + 0x200;
std::atomic<bool> x_continued{false};
uint32_t jump3 = 0; // G's buffer
uint32_t mr(uint32_t a, uint32_t s) { return 31u << 26 | s << 21 | a << 16 | s << 11 | 444u << 1; }
uint32_t cmpwi0(uint32_t a, int16_t v) { return 11u << 26 | a << 16 | uint16_t(v); }
uint32_t bne(uint32_t from, uint32_t to) { return 16u << 26 | 4u << 21 | 2u << 16 | ((to - from) & 0xFFFC); }

void x_function(ppc::Context &c) {
    c.r[1] -= 0x20; // its frame
    c.lr = X + 4;
    K->dispatch(c, LONGJMP);
    x_continued = true; // never: the longjmp went to S's frame
    c.r[1] += 0x20;
}
// X2, called by G while the kernel already goes on interpreting above the
// call: a second switch above its frame, thrown up to the kernel again
void x2_function(ppc::Context &c) {
    c.r[1] -= 0x20;
    c.r[3] = jump3, c.r[4] = 0;
    c.lr = X2 + 4;
    K->dispatch(c, LONGJMP);
    x_continued = true;
    c.r[1] += 0x20;
}

void longjmp_across_generated(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t buf = h.alloc(0x10), buf2 = h.alloc(0x10);
    put_code_at(k, SETJMP, {stw(1, 0, 3), MFLR_R0, stw(0, 4, 3), li(3, 0), BLR});
    put_code_at(k, LONGJMP, {lwz(1, 0, 3), lwz(0, 4, 3), MTLR_R0, mr(3, 4), BLR});
    put_code_at(k, S,
                {MFLR_R0, stw(0, -8, 1), stwu(1, -0x40, 1), lis(3, uint16_t(buf >> 16)), ori(3, 3, uint16_t(buf)),
                 bl(S + 0x14, SETJMP), cmpwi0(3, 0), bne(S + 0x1C, S + 0x38), lis(3, uint16_t(buf >> 16)),
                 ori(3, 3, uint16_t(buf)), li(4, 7), bl(S + 0x2C, X), li(3, 1), 0x48000008 /* b S + 0x3C */,
                 addi(3, 3, 0x100), addi(1, 1, 0x40), lwz(0, -8, 1), MTLR_R0, BLR});
    uint64_t sp = h.c->r[1];
    k.call(*h.c, S);
    CHECK(uint32_t(h.c->r[3]) == 0x107 && !x_continued && h.c->r[1] == sp);
    CHECK(k.interpreted_targets().count(S + 0x18) == 1); // where the interpreter went on

    // S2: r31 = buf2, its return address in buf2 + 8, longjmp to G with r1
    // above every frame of the call; G calls X2, whose longjmp goes above
    // again, to G2, which returns from the call
    jump3 = h.alloc(0x10);
    put_code_at(k, S2,
                {lis(31, uint16_t(buf2 >> 16)), ori(31, 31, uint16_t(buf2)), MFLR_R0, stw(0, 8, 31), mr(3, 31), li(4, 0),
                 bl(S2 + 0x18, LONGJMP), li(3, 1), BLR});
    put_code_at(k, G, {bl(G, X2), li(3, 1), BLR});
    put_code_at(k, G2, {lwz(0, 8, 31), MTLR_R0, li(3, 9), BLR});
    k.store32(buf2, uint32_t(sp) + 0x1000);
    k.store32(buf2 + 4, G);
    k.store32(jump3, uint32_t(sp) + 0x2000);
    k.store32(jump3 + 4, G2);
    k.call(*h.c, S2);
    CHECK(uint32_t(h.c->r[3]) == 9 && uint32_t(h.c->r[1]) == uint32_t(sp) + 0x2000 && !k.stopped && !x_continued);
    h.c->r[1] = sp;
}

// ---- a longjmp back into generated code ----
//
// XR is generated code shaped as the emitter writes a function with a resume
// site (docs/recompiler.md, "Stack switches"): it calls SETJMP (the guest's,
// interpreted), then XLJ through dispatch (an indirect call: its stack top is
// the r1 SETJMP kept). XLJ is a generated longjmp: r1 and LR back from the
// buffer, its return checked, so it throws an Unwind to XR_RET. The dispatch
// lets it through (XR_RET is a resume site), and XR goes on there in its own
// frame, nothing interpreted.

constexpr uint32_t XR = J + 0x240, XR_RET = XR + 0x10, XLJ = J + 0x280;
uint32_t xr_buffer = 0;
std::atomic<bool> xr_fell{false};
uint64_t xr_value = 0;

void xr_function(ppc::Context &c) {
    const uint32_t xf_sp = uint32_t(c.r[1]);
    uint32_t xf_at = 0;
    for (;;) {
        try {
            if (xf_at == XR_RET) goto resume;
            c.r[1] -= 0x40; // its frame
            c.r[3] = xr_buffer;
            c.lr = XR_RET;
            K->dispatch(c, SETJMP);
        resume:
            if (uint32_t(c.r[3]) == 0) {
                c.r[3] = xr_buffer, c.r[4] = 5;
                c.lr = XR + 0x20;
                K->dispatch(c, XLJ);
                xr_fell = true; // never: the longjmp came back to XR_RET
            }
            xr_value = c.r[3];
            c.r[1] += 0x40;
            return;
        } catch (const ppc::Unwind &u) {
            if (uint32_t(c.r[1]) >= xf_sp) throw;
            if (u.target != XR_RET) throw;
            xf_at = u.target;
        }
    }
}

void xlj_function(ppc::Context &c) {
    const uint32_t xf_lr = uint32_t(c.lr), xf_sp = uint32_t(c.r[1]);
    uint32_t buffer = uint32_t(c.r[3]);
    c.r[1] = K->load32(buffer);
    c.lr = K->load32(buffer + 4);
    c.r[3] = c.r[4];
    if (uint32_t(c.r[1]) != xf_sp || (uint32_t(c.lr) & ~3u) != xf_lr) ppc::unwind(c);
}

void longjmp_into_generated(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    xr_buffer = h.alloc(0x10);
    uint64_t sp = h.c->r[1], entries = k.interpreted_entries(), thrown = k.unwinds_thrown;
    uint64_t by_dispatch = k.unwinds_by_dispatch;
    k.call(*h.c, XR);
    CHECK(xr_value == 5 && !xr_fell && h.c->r[1] == sp && !k.stopped);
    // SETJMP's entry alone was interpreted; the Unwind went to XR's frame
    CHECK(k.interpreted_entries() == entries + 1 && k.interpreted_targets().count(SETJMP));
    CHECK(k.unwinds_thrown == thrown + 1 && k.unwinds_by_dispatch == by_dispatch);
}

const kernel::Generated generated[] = {
    {F_LOADER, loader_thread},
    {X, x_function},
    {X2, x2_function},
    {XR, xr_function},
    {XLJ, xlj_function},
    {GATE + ENTRY, gate_main},
};
const uint32_t resumes[] = {XR_RET};

std::vector<uint8_t> make_gate(uint32_t log) {
    std::vector<uint8_t> image = make_image(GATE, 0x33, log);
    put32(image, IMPORTS, KE_QUERY_PERFORMANCE_FREQUENCY);
    put_thunk(image, IMPORTS + 0x10, KE_QUERY_PERFORMANCE_FREQUENCY);
    return make_dll(GATE, image, {{"xboxkrnl.exe", {GATE + IMPORTS, GATE + IMPORTS + 0x10}}});
}

// a guest thread from start to end; its id
uint32_t run_thread(KernelHarness &h, uint32_t start, uint32_t expected_exit, std::function<void()> meanwhile = {}) {
    kernel::Kernel &k = *h.k;
    uint32_t handle_ptr = h.alloc(4), id_ptr = h.alloc(4);
    CHECK(h.krnl("ExCreateThread", {handle_ptr, 0, id_ptr, 0, start, 0, 0}) == 0);
    uint32_t thread = k.load32(handle_ptr);
    if (meanwhile) meanwhile();
    kernel::Objects &o = k.objects();
    kernel::Ref<kernel::Thread> t = o.lookup<kernel::Thread>(thread);
    CHECK(t);
    {
        std::unique_lock<std::mutex> l(o.lock);
        CHECK(t && o.thread_exited.wait_for(l, std::chrono::seconds(30), [&] { return t->finished; }));
        CHECK(t && t->exit_code == expected_exit);
    }
    CHECK(h.krnl("NtClose", {thread}) == 0);
    return k.load32(id_ptr);
}

// gate.dll loaded before the others: first in the list after the executable
uint32_t load_gate(KernelHarness &h) {
    uint32_t out = h.alloc(4);
    size_t from = gate.count();
    CHECK(h.krnl("XexLoadImage", {h.string("gate.dll"), 9, 0, out}) == 0);
    uint32_t hmodule = h.k->load32(out);
    CHECK((gate.since(from) == std::vector<GateCall>{{hmodule, PROCESS_ATTACH, -1}}));
    return hmodule;
}

// ExitThread in DllMain: in DLL_THREAD_ATTACH, the thread ends there (the
// DLLs after gate.dll get no THREAD_ATTACH, its start routine does not run);
// in DLL_THREAD_DETACH, gate.dll's own call ends and the others still get
// theirs. The loader lock is free after.
void exit_in_dll_main(KernelHarness &h, uint32_t log, uint32_t gate_module) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(4);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("app.dll"), out}) == 0);
    uint32_t app = k.load32(out);
    CHECK(h.krnl("XexGetModuleHandle", {h.string("lib.dll"), out}) == 0);
    uint32_t lib = k.load32(out);
    uint32_t logged = k.load32(log);
    size_t from = gate.count();
    gate.phase = 3;
    uint32_t id = run_thread(h, BASE + 0x6000, 0x77);
    gate.phase = 0;
    CHECK((gate.since(from) ==
           std::vector<GateCall>{{gate_module, THREAD_ATTACH, id}, {gate_module, THREAD_DETACH, id}}));
    CHECK((entry_calls(k, log, logged) ==
           std::vector<EntryCall>{{app, THREAD_DETACH, 0, APP_ID}, {lib, THREAD_DETACH, 0, LIB_ID}}));
    std::string report = kernel::modules_report(k);
    CHECK(report.find("the loader lock") == std::string::npos);
    if (report.find("the loader lock") != std::string::npos) return; // the rest would wait for it forever
    CHECK(h.krnl("XexGetModuleHandle", {h.string("gate.dll"), out}) == 0 && k.load32(out) == gate_module);
}

void loader_lock(KernelHarness &h, uint32_t gate_module) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(4);

    // A thread ends while gate.dll is unloaded: its DLL_THREAD_DETACH (still
    // running when XexUnloadImage is called) ends before the unload's
    // DLL_PROCESS_DETACH starts, and nothing reaches the module after.
    size_t from = gate.count();
    gate.phase = 1;
    uint32_t id = run_thread(h, BASE + 0x6000, 0x55, [&] {
        CHECK(wait_flag(gate.in_thread_detach, std::chrono::seconds(30)));
        CHECK(h.krnl("XexUnloadImage", {gate_module}) == 0);
        gate.unload_returned = true;
    });
    gate.phase = 0;
    CHECK((gate.since(from) == std::vector<GateCall>{{gate_module, THREAD_ATTACH, id},
                                                     {gate_module, THREAD_DETACH, id, false},
                                                     {gate_module, PROCESS_DETACH, -1}}));
    CHECK(h.krnl("XexGetModuleHandle", {h.string("gate.dll"), out}) == 0x490);
    RegionInfo region;
    CHECK(k.memory().heap_for(GATE)->query(GATE, region) && region.state == 0);

    // A thread loads gate.dll while it is being unloaded: the load waits for
    // the unload, then loads a fresh copy and attaches it on its thread.
    uint32_t again = load_gate(h);
    from = gate.count();
    gate.name = h.string("gate.dll"), gate.out = h.alloc(4);
    id = run_thread(h, F_LOADER, 0, [&] {
        CHECK(wait_flag(gate.loader_running, std::chrono::seconds(30)));
        gate.phase = 2;
        CHECK(h.krnl("XexUnloadImage", {again}) == 0);
    });
    gate.phase = 0;
    uint32_t fresh = gate.loaded;
    CHECK(gate.load_status == 0 && fresh != 0);
    CHECK((gate.since(from) == std::vector<GateCall>{{again, THREAD_ATTACH, id},
                                                     {again, PROCESS_DETACH, -1, false},
                                                     {fresh, PROCESS_ATTACH, id},
                                                     {fresh, THREAD_DETACH, id}}));
    CHECK(h.krnl("XexGetModuleHandle", {h.string("gate.dll"), out}) == 0 && k.load32(out) == fresh);
    CHECK(k.load16(fresh + 0x40) == 1 && k.load32(GATE + DATA) == 0xCAFEF00D);

    // and gone
    from = gate.count();
    CHECK(h.krnl("XexUnloadImage", {fresh}) == 0);
    CHECK((gate.since(from) == std::vector<GateCall>{{fresh, PROCESS_DETACH, -1}}));
    CHECK(h.krnl("XexGetModuleHandle", {h.string("gate.dll"), out}) == 0x490);
}

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_modules: no kernel\n"), 1;
    // the game's directory: the DLLs, and a file that is no executable (its
    // own name, so copies of the test run side by side)
    std::random_device random;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("xf_modules_test_" + std::to_string(random()) + "_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    uint32_t log = h.alloc(0x400);
    std::vector<uint8_t> lib_file = make_lib(log), app_file = make_app(log);
    auto write = [&](const char *name, const std::vector<uint8_t> &data) {
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), data.size());
    };
    write("lib.dll", lib_file);
    write("app.dll", app_file);
    made_from(lib_module, lib_file);
    made_from(app_module, app_file);
    app_module.digest ^= 1; // another image
    write("junk.dll", std::vector<uint8_t>(0x1000, 'j'));
    write("gate.dll", make_gate(log));
    std::string error;
    if (!kernel::mount_directory(*h.k, dir, error)) return std::printf("kernel_modules: %s\n", error.c_str()), 1;

    K = h.k.get();
    modules_kernel = h.k.get();
    h.k->set_generated(generated, std::size(generated), resumes, std::size(resumes));
    h.k->set_generated_modules(generated_modules, std::size(generated_modules));
    Xex xex = make_xex();
    if (!h.k->load(xex, error)) return std::printf("kernel_modules: %s\n", error.c_str()), 1;
    modules(h);
    longjmp_across_generated(h);
    longjmp_into_generated(h);
    uint32_t gate_module = load_gate(h);
    user_modules(h, log, lib_file);
    exit_in_dll_main(h, log, gate_module);
    loader_lock(h, gate_module);
    variables(h);
    strings(h);
    time(h);
    status_codes(h);
    printf_family(h);
    settings(h);
    process_type(h);
    debugging(h);
    h.k.reset(); // its files closed first
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return finish("kernel_modules");
}
