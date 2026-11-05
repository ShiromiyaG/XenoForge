/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The rest of the kernel's small exports (plan step A4; docs/runtime.md):
// debugging, the HAL, the clock, the current process type, the console's
// settings (XConfig), the crypto the games import, the microphone's
// requests. Translated from Xenia (BSD; see LICENSE-xenia):
// xboxkrnl_debug.cc, xboxkrnl_hal.cc, xboxkrnl_threading.cc (the clock and
// the process type), xboxkrnl_misc.cc, xboxkrnl_xconfig.cc with xconfig.h
// and xconfig.cc (the settings' defaults, without the settings file),
// xboxkrnl_crypt.cc; checked against them. SHA-1 is written here from FIPS
// 180-1 (Xenia uses TinySHA1).
//
// Where Xenia stops the emulator (HalReturnToFirmware exits; KeBugCheckEx
// suspends the thread; DbgBreakPoint breaks into the debugger), these log;
// HalReturnToFirmware and KeBugCheckEx then end the process, as nothing
// after them is meant to run.

#include "xenoforge/kernel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_PENDING = 0x00000103;
constexpr uint32_t X_STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t X_STATUS_INVALID_PARAMETER_3 = 0xC00000F1;

// Runs guest code from an export, as Xenia's Processor::Execute: the
// arguments in r3 and up, the stack padded (some games overwrite the
// caller's frame), the export's return address kept
uint64_t execute(Kernel &k, ppc::Context &c, uint32_t address, std::initializer_list<uint64_t> args) {
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    c.r[1] -= 64 + 112;
    uint64_t lr = c.lr;
    k.call(c, address);
    c.lr = lr;
    c.r[1] += 64 + 112;
    return c.r[3];
}

// ---- debugging (xboxkrnl_debug.cc) ----

XF_EXPORT(xboxkrnl, DbgBreakPoint) {
    // Xenia breaks into the host's debugger
    std::fprintf(stderr, "kernel: DbgBreakPoint from 0x%08X\n", uint32_t(c.lr));
}

XF_EXPORT(xboxkrnl, RtlRaiseException) {
    // (X_EXCEPTION_RECORD *): code, flags, record, address, parameter
    // count, parameters
    uint32_t record = arg(k, c, 0);
    uint32_t code = k.load32(record);
    if (code == 0x406D1388) {
        // SetThreadName: X_THREADNAME_INFO in the parameters (type 0x1000,
        // name, thread id, flags)
        uint32_t name = k.load32(record + 0x14 + 4), thread_id = k.load32(record + 0x14 + 8);
        std::string text;
        if (name)
            for (uint32_t p = name; k.host(p)[0] && text.size() < 256; p++) {
                char ch = char(k.host(p)[0]);
                text += (ch < 32 || ch > 126) ? '?' : ch;
            }
        std::fprintf(stderr, "kernel: SetThreadName(%d, %s)\n", int32_t(thread_id), text.c_str());
        return;
    }
    if (code == 0xE06D7363) {
        std::fprintf(stderr, "kernel: the guest throws a C++ exception\n");
        return;
    }
    // (no unwinding: RtlRaiseException returns)
    std::fprintf(stderr, "kernel: RtlRaiseException(0x%08X) from 0x%08X\n", code, uint32_t(c.lr));
}

[[noreturn]] void bug_check(Kernel &k, uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
    char message[96];
    std::snprintf(message, sizeof(message), "*** STOP: 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)", code, p1, p2, p3, p4);
    std::fprintf(stderr, "kernel: %s\n", message);
    if (!k.stopped.exchange(true)) k.stop_reason = std::string("KeBugCheck: ") + message;
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(4);
}

XF_EXPORT(xboxkrnl, KeBugCheckEx) {
    bug_check(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4));
}

XF_EXPORT(xboxkrnl, KeBugCheck) { bug_check(k, arg(k, c, 0), 0, 0, 0, 0); }

// ---- the HAL (xboxkrnl_hal.cc) ----

XF_EXPORT(xboxkrnl, HalReturnToFirmware) {
    static constexpr const char *messages[] = {
        "hard poweroff",
        "hard reset (video error)",
        "hard reset (used for dumpwritedump/frozen processor)",
        "hard reset",
        "power off (hard)",
        "power off (nice)",
        "Shut off (Lost Settings)",
        "Shut off (Frozen Console)",
        "Shut off",
    };
    uint32_t routine = arg(k, c, 0);
    if (routine < std::size(messages))
        std::fprintf(stderr, "kernel: the game asks for a %s through HalReturnToFirmware\n", messages[routine]);
    else
        std::fprintf(stderr, "kernel: the game asks for a reboot (routine %u) through HalReturnToFirmware\n", routine);
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(0);
}

// ---- the clock (xboxkrnl_threading.cc, xboxkrnl_misc.cc) ----

XF_EXPORT(xboxkrnl, KeQuerySystemTime) {
    // also the timestamp bundle's system time, kept consistent with it
    uint32_t time_ptr = arg(k, c, 0);
    if (!time_ptr) return;
    uint64_t time = k.system_time();
    k.store64(k.variable("KeTimeStampBundle") + 8, time);
    k.store64(time_ptr, time);
}

XF_EXPORT(xboxkrnl, KeQueryPerformanceFrequency) {
    // the time base's: 50 MHz
    result(c, 50'000'000);
}

XF_EXPORT(xboxkrnl, KeQueryInterruptTime) { c.r[3] = k.load64(k.variable("KeTimeStampBundle")); }

// ---- the current process (xboxkrnl_threading.cc, xboxkrnl_misc.cc) ----
// The PCR (r13): the PRCB at 0x100 with the current thread at 0x100 and DPC
// activity at 0x150, the process type in a DPC at 0xC; the thread (KTHREAD):
// the FPU exceptions at 0x71, the process type at 0x73.

constexpr uint32_t PCR_PROCESS_TYPE_IN_DPC = 0x0C;
constexpr uint32_t PCR_CURRENT_THREAD = 0x100;
constexpr uint32_t PCR_DPC_ACTIVE = 0x150;
constexpr uint32_t KTHREAD_FPU_EXCEPTIONS = 0x71;
constexpr uint32_t KTHREAD_PROCESS_TYPE = 0x73;

XF_EXPORT(xboxkrnl, KeGetCurrentProcessType) {
    uint32_t pcr = uint32_t(c.r[13]);
    if (k.load32(pcr + PCR_DPC_ACTIVE)) return result(c, k.host(pcr + PCR_PROCESS_TYPE_IN_DPC)[0]);
    uint32_t thread = k.load32(pcr + PCR_CURRENT_THREAD);
    // a thread with no KTHREAD yet (until the threads family gives it one)
    // runs in the title
    if (!thread) return result(c, 1);
    result(c, k.host(thread + KTHREAD_PROCESS_TYPE)[0]);
}

XF_EXPORT(xboxkrnl, KeSetCurrentProcessType) {
    uint32_t pcr = uint32_t(c.r[13]);
    if (k.load32(pcr + PCR_DPC_ACTIVE)) k.host(pcr + PCR_PROCESS_TYPE_IN_DPC)[0] = uint8_t(arg(k, c, 0));
}

XF_EXPORT(xboxkrnl, KeEnableFpuExceptions) {
    // (enabled): kept in the thread, nothing more (Xenia's stub)
    uint32_t thread = k.load32(uint32_t(c.r[13]) + PCR_CURRENT_THREAD);
    if (thread) k.host(thread + KTHREAD_FPU_EXCEPTIONS)[0] = arg(k, c, 0) != 0;
}

// ---- the console's settings (xboxkrnl_xconfig.cc, xconfig.h) ----
// Each setting by category and id, at its size, with the value Xenia's
// XConfig::SetDefaults gives it (zero where it gives none), big-endian.

std::vector<uint8_t> be32(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }
std::vector<uint8_t> zeros(size_t n) { return std::vector<uint8_t>(n, 0); }

const std::map<uint32_t, std::vector<uint8_t>> &settings() {
    static const std::map<uint32_t, std::vector<uint8_t>> table = [] {
        std::map<uint32_t, std::vector<uint8_t>> t;
        auto set = [&](uint16_t category, uint16_t setting, std::vector<uint8_t> value) {
            t[uint32_t(category) << 16 | setting] = std::move(value);
        };
        // static (0)
        set(0x00, 0x01, zeros(5)); // first power-on date
        // secured (2)
        set(0x02, 0x01, zeros(6));         // MAC address
        set(0x02, 0x02, be32(0x00400100)); // AV region: NTSC-M
        set(0x02, 0x03, zeros(2));         // game region
        set(0x02, 0x04, zeros(4));         // DVD region
        set(0x02, 0x05, zeros(4));         // reset key
        set(0x02, 0x06, zeros(4));         // system flags
        set(0x02, 0x07, zeros(2));         // power mode
        set(0x02, 0x08, zeros(4));         // online network id
        set(0x02, 0x09, zeros(2));         // power VCS control
        // user (3); the time zone is Xenia's default, London's
        set(0x03, 0x01, be32(0));                  // time zone bias
        set(0x03, 0x02, {'G', 'M', 'T', 0});       // standard name
        set(0x03, 0x03, {'B', 'S', 'T', 0});       // daylight name
        set(0x03, 0x04, {0x0A, 0x05, 0x00, 0x02}); // standard date
        set(0x03, 0x05, {0x03, 0x05, 0x00, 0x01}); // daylight date
        set(0x03, 0x06, be32(0));                  // standard bias
        set(0x03, 0x07, be32(uint32_t(-60)));      // daylight bias
        set(0x03, 0x08, zeros(8));                 // default profile
        set(0x03, 0x09, be32(1));                  // language: English
        set(0x03, 0x0A, be32(0));                  // video flags: 4:3
        set(0x03, 0x0B, be32(0x00010001));         // audio flags: Dolby Digital, Pro Logic
        set(0x03, 0x0C, be32(0x00000040));         // retail flags: dashboard initialized
        set(0x03, 0x0D, zeros(4));                 // devkit flags
        set(0x03, 0x0E, {103});                    // country: United States
        set(0x03, 0x0F, {0x03});                   // parental control: Live, membership creation
        set(0x03, 0x10, zeros(256));               // SMB config
        set(0x03, 0x11, zeros(8));                 // Live PUID
        set(0x03, 0x12, zeros(16));                // Live credentials
        set(0x03, 0x13, be32(1280u << 16 | 720));  // HDMI/composite screen size
        set(0x03, 0x14, be32(1280u << 16 | 720));  // component screen size
        set(0x03, 0x15, be32(720u << 16 | 576));   // VGA screen size
        set(0x03, 0x16, be32(0xFF));               // parental control: games, none restricted
        for (uint16_t id = 0x17; id <= 0x1A; id++) set(0x03, id, zeros(4));
        set(0x03, 0x1B, zeros(1));         // parental control hint
        set(0x03, 0x1C, zeros(32));        // hint answer
        set(0x03, 0x1D, zeros(32));        // override
        set(0x03, 0x1E, zeros(4));         // music playback mode
        set(0x03, 0x1F, be32(0x3F333333)); // music volume: 0.7f
        for (uint16_t id = 0x20; id <= 0x29; id++) set(0x03, id, zeros(4));
        set(0x03, 0x2A, zeros(1)); // video player display mode
        set(0x03, 0x2B, zeros(4)); // alternate video timing id
        set(0x03, 0x2C, zeros(4)); // video driver options
        set(0x03, 0x2D, zeros(4)); // music UI flags
        set(0x03, 0x2E, zeros(1)); // video, music, photo media source types
        set(0x03, 0x2F, zeros(1));
        set(0x03, 0x30, zeros(1));
        // console (7)
        set(0x07, 0x01, zeros(2));   // screen saver
        set(0x07, 0x02, zeros(2));   // auto shut off
        set(0x07, 0x03, zeros(256)); // wireless settings
        set(0x07, 0x04, zeros(4));   // camera settings
        set(0x07, 0x05, zeros(20));  // play timer data
        set(0x07, 0x06, zeros(2));   // media: disable auto launch
        set(0x07, 0x07, zeros(2));   // keyboard layout
        // IPTV (9): the service provider's name, 60 wide characters
        std::vector<uint8_t> provider = zeros(120);
        const char *name = "Xenia TV";
        for (size_t i = 0; name[i]; i++) provider[2 * i + 1] = uint8_t(name[i]);
        set(0x09, 0x01, provider);
        // system (10)
        set(0x0A, 0x01, zeros(8)); // alarm time
        set(0x0A, 0x02, zeros(4)); // previous flash version
        return t;
    }();
    return table;
}

XF_EXPORT(xboxkrnl, ExGetXConfigSetting) {
    // (category, setting, buffer, buffer_size, required_size_ptr): an
    // unknown setting has size 0
    uint16_t category = uint16_t(arg(k, c, 0)), setting = uint16_t(arg(k, c, 1));
    uint32_t buffer = arg(k, c, 2), required_size_ptr = arg(k, c, 4);
    uint16_t buffer_size = uint16_t(arg(k, c, 3));
    auto it = settings().find(uint32_t(category) << 16 | setting);
    uint16_t size = it == settings().end() ? 0 : uint16_t(it->second.size());
    if (!size) std::fprintf(stderr, "kernel: ExGetXConfigSetting: no setting %u in category %u\n", setting, category);
    uint32_t status = X_STATUS_SUCCESS;
    uint16_t required_size = 0;
    if (!buffer && buffer_size) {
        status = X_STATUS_INVALID_PARAMETER_3;
    } else if (buffer && buffer_size < size) {
        status = X_STATUS_BUFFER_TOO_SMALL;
    } else {
        if (buffer && size) std::memcpy(k.host(buffer), it->second.data(), size);
        required_size = size;
    }
    if (required_size_ptr) k.store16(required_size_ptr, required_size);
    result(c, status);
}

// ---- xam's queries of the settings (xam_info.cc, xam_locale.cc) ----

XF_EXPORT(xam, XGetAVPack) {
    // Xenia's default AV pack, 8 (HDMI); games take one outside 3/4/6/8 for
    // PAL
    result(c, 8);
}

// xeXGetGameRegion: by the user's country
uint32_t game_region() {
    static constexpr uint16_t table[] = {
        0xFFFF, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x02FE, 0x0201, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x03FF, 0x03FF, 0x03FF,
        0x02FE, 0x03FF, 0x00FF, 0xFFFF, 0x02FE, 0x03FF, 0x0102, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x02FE, 0x03FF, 0x03FF,
        0x03FF, 0x02FE, 0x03FF, 0x02FE, 0x02FE, 0x02FE, 0x02FE, 0x02FE, 0x02FE, 0x02FE, 0x03FF, 0x03FF, 0x03FF, 0x02FE,
        0x02FE, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x03FF, 0x0101, 0x03FF, 0x03FF,
        0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x02FE, 0x02FE, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x0102,
        0x03FF, 0x00FF, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x0201, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x02FE, 0x03FF,
        0x02FE, 0x03FF, 0x03FF, 0x02FE, 0x02FE, 0x03FF, 0x02FE, 0x03FF, 0x02FE, 0x02FE, 0xFFFF, 0x03FF, 0x03FF, 0x03FF,
        0x03FF, 0x02FE, 0x03FF, 0x03FF, 0x02FE, 0x00FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF};
    uint8_t country = settings().at(0x0003000E)[0];
    return country < std::size(table) ? table[country] : 0xFFFF;
}

XF_EXPORT(xam, XGetGameRegion) { result(c, game_region()); }

XF_EXPORT(xam, XGetLanguage) {
    // the user's language among the base ten, else by the region
    const std::vector<uint8_t> &v = settings().at(0x00030009);
    uint32_t language = uint32_t(v[0]) << 24 | v[1] << 16 | v[2] << 8 | v[3];
    uint32_t region = game_region();
    if (language < 10) return result(c, language);       // kMaxBaseLanguages
    if ((region & 0xFF00) != 0x100) return result(c, 1); // English
    result(c, region == 0x101 ? 2 /* Japanese */ : region == 0x102 ? 1 /* English */ : 7 /* Korean */);
}

// ---- crypto (xboxkrnl_crypt.cc) ----

// SHA-1 (FIPS 180-1)
class Sha1 {
  public:
    void update(const uint8_t *data, size_t size) {
        length_ += uint64_t(size) * 8;
        while (size--) {
            block_[used_++] = *data++;
            if (used_ == 64) compress(), used_ = 0;
        }
    }
    void finish(uint8_t digest[20]) {
        uint64_t length = length_;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (used_ != 56) update(&pad, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t b = uint8_t(length >> (8 * i));
            update(&b, 1);
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) digest[4 * i + j] = uint8_t(h_[i] >> (24 - 8 * j));
    }

  private:
    static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
    void compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(block_[4 * i]) << 24 | uint32_t(block_[4 * i + 1]) << 16 |
                   uint32_t(block_[4 * i + 2]) << 8 | block_[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
    }
    uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block_[64] = {};
    size_t used_ = 0;
    uint64_t length_ = 0;
};

XF_EXPORT(xboxkrnl, XeCryptSha) {
    // (input1, size1, input2, size2, input3, size3, output, output_size):
    // the SHA-1 of the three, as much of it as fits
    Sha1 sha;
    for (int i = 0; i < 3; i++) {
        uint32_t input = arg(k, c, 2 * i), size = arg(k, c, 2 * i + 1);
        if (input && size) sha.update(k.host(input), size);
    }
    uint8_t digest[20];
    sha.finish(digest);
    uint32_t output = arg(k, c, 6), output_size = arg(k, c, 7);
    std::memcpy(k.host(output), digest, std::min<size_t>(sizeof(digest), output_size));
}

XF_EXPORT(xboxkrnl, XeKeysConsolePrivateKeySign) {
    // (hash, XE_CONSOLE_CERTIFICATE *output): no signature, Xenia's made-up
    // retail certificate
    uint32_t hash = arg(k, c, 0), output = arg(k, c, 1);
    if (!hash || !output) return result(c, 0);
    std::memset(k.host(output), 0, 0x1A8);
    // the console id (XE_CONSOLE_ID's bit fields as MSVC lays them out:
    // refurb 3 and month 9 in the first byte, then year 1, MAC index bytes
    // 0x40, 0x66, 0x7E and CRC 0 in a little-endian word)
    static constexpr uint8_t console_id[5] = {0x93, 0x01, 0x64, 0xE6, 0x07};
    std::memcpy(k.host(output + 0x02), console_id, sizeof(console_id));
    k.store32(output + 0x18, 2); // console type: retail
    static constexpr uint8_t manufacture_date[8] = {2, 0, 0, 5, 1, 1, 2, 2};
    std::memcpy(k.host(output + 0x1C), manufacture_date, sizeof(manufacture_date));
    result(c, 1);
}

// ---- the microphone (xboxkrnl_misc.cc) ----

XF_EXPORT(xboxkrnl, MicDeviceRequest) {
    // X_MIC_DEVICE: request type (16 bits), user index (16), state, then
    // for overlapped requests an unknown, an event, a completion routine;
    // the capabilities at 0x18. No microphone is connected.
    uint32_t device = arg(k, c, 0);
    if (!device) return result(c, X_STATUS_INVALID_PARAMETER);
    uint16_t request = k.load16(device), user = k.load16(device + 2);
    std::fprintf(stderr, "kernel: MicDeviceRequest State: %08X Action: %04X USER: %08X\n", k.load32(device + 4),
                 request, user);
    if (user > 4) return result(c, X_STATUS_INVALID_PARAMETER); // XUserMaxUserCount
    if (request > 0x000B) return result(c, X_STATUS_INVALID_DEVICE_REQUEST);
    k.store32(device + 4, 0);                   // not connected
    if (request == 0x0000) return result(c, 0); // MicGetStatus
    if (request == 0x0009) {
        // MicGetCapabilities: features, format tag, microphone color
        k.store32(device + 0x18, 0x100);
        k.store16(device + 0x1C, 1);
        k.host(device + 0x18 + 0x10)[0] = 0;
    }
    // completed at once, as having failed (a DPC's job on the console)
    if (uint32_t completion = k.load32(device + 0x10)) execute(k, c, completion, {device, X_STATUS_UNSUCCESSFUL});
    result(c, X_STATUS_PENDING);
}

} // namespace
} // namespace xf::kernel
