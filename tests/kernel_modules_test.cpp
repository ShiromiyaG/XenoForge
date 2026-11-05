// The modules family (src/kernel/modules.cpp), the Rtl functions (rtl.cpp)
// and the small exports (misc.cpp), called as a game calls them on a kernel
// that loaded a made-up executable, against what Xenia's code gives.

#include "kernel_harness.h"

#include "xenoforge/xex.h"

#include <array>
#include <chrono>
#include <thread>

using namespace xf;

namespace {

constexpr uint32_t BASE = 0x82000000;

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    v[at] = uint8_t(x >> 24), v[at + 1] = uint8_t(x >> 16), v[at + 2] = uint8_t(x >> 8), v[at + 3] = uint8_t(x);
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
    CHECK(k.variable("ExLoadedImageName") == 0x80207E60 && h.read(0x80207E60) == "\\Device\\Cdrom0\\default.xex");
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

    // KeTimeStampBundle, kept by the timer
    uint32_t bundle = k.variable("KeTimeStampBundle");
    CHECK(bundle == 0x80240EE0);
    uint32_t tick = k.load32(bundle + 0x10);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(k.load32(bundle + 0x10) >= tick + 20);
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

void crypto(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    uint32_t out = h.alloc(32);
    // SHA-1("abc"), in three parts
    h.krnl("XeCryptSha", {h.string("a"), 1, h.string("bc"), 2, 0, 0, out, 20});
    static const uint8_t abc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(std::memcmp(k.host(out), abc, 20) == 0);
    // of nothing, cut to 8 bytes
    std::memset(k.host(out), 0, 32);
    h.krnl("XeCryptSha", {0, 0, 0, 0, 0, 0, out, 8});
    static const uint8_t empty[8] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d};
    CHECK(std::memcmp(k.host(out), empty, 8) == 0 && k.load32(out + 8) == 0);
    // a long message: SHA-1 of a million 'a's
    uint32_t million = h.alloc(1000000);
    std::memset(k.host(million), 'a', 1000000);
    h.krnl("XeCryptSha", {million, 1000000, 0, 0, 0, 0, out, 20});
    static const uint8_t a_million[20] = {0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda, 0xa4, 0xf6, 0x1e,
                                          0xeb, 0x2b, 0xdb, 0xad, 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f};
    CHECK(std::memcmp(k.host(out), a_million, 20) == 0);

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
    ConsoleId id{};
    id.RefurbBits = 3, id.ManufactureMonth = 9, id.ManufactureYear = 1;
    id.MacIndex3 = 0x40, id.MacIndex4 = 0x66, id.MacIndex5 = 0x7E, id.Crc = 0;
    CHECK(sizeof(ConsoleId) == 5 && std::memcmp(k.host(certificate + 2), id.Data, 5) == 0);
#endif
    CHECK(h.krnl("XeKeysConsolePrivateKeySign", {0, certificate}) == 0);
}

void process_type(KernelHarness &h) {
    kernel::Kernel &k = *h.k;
    // no KTHREAD in the PCR yet: the title
    CHECK(h.krnl("KeGetCurrentProcessType") == 1);
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

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_modules: no kernel\n"), 1;
    Xex xex = make_xex();
    std::string error;
    if (!h.k->load(xex, error)) return std::printf("kernel_modules: %s\n", error.c_str()), 1;
    modules(h);
    variables(h);
    strings(h);
    time(h);
    status_codes(h);
    printf_family(h);
    settings(h);
    crypto(h);
    process_type(h);
    debugging(h);
    return finish("kernel_modules");
}
