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
// debugging, the HAL and its system management controller (SMC), the clock,
// the current process type, the console's settings (XConfig), the
// microphone's requests, the keyboard's keys and the USB camera (the crypto
// is crypt.cpp). Translated from Xenia (BSD; see LICENSE-xenia):
// xboxkrnl_debug.cc, xboxkrnl_hal.cc with smc.h and smc.cc,
// xboxkrnl_threading.cc (the clock and the process type), xboxkrnl_misc.cc,
// xboxkrnl_hid.cc, xboxkrnl_usbcam.cc, xboxkrnl_xconfig.cc with xconfig.h and
// xconfig.cc (the settings' defaults, without the settings file); checked
// against them.
//
// Where Xenia stops the emulator (HalReturnToFirmware exits; KeBugCheckEx
// suspends the thread; DbgBreakPoint breaks into the debugger), these log;
// HalReturnToFirmware and KeBugCheckEx then end the process, as nothing
// after them is meant to run. The settings the title writes
// (ExSetXConfigSetting) last as long as the kernel: Xenia also writes them
// to its settings file.

#include "xam.h"
#include "xenoforge/kernel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <vector>

namespace xf::kernel {
namespace {

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
//
// DbgBreakPoint and RtlRaiseException raise through the exception dispatch
// (exceptions.cpp); KeBugCheck and KeBugCheckEx stop here.

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

// Xenia's default AV pack (its avpack setting): 8, HDMI
constexpr uint32_t AV_PACK = 8;

XF_EXPORT(xboxkrnl, HalGetCurrentAVPack) { result(c, AV_PACK); }

// The system management controller (Xenia's SystemManagementController):
// the DVD tray (open at start, as Xenia's), the remote's IR address, the
// tilt, the fans and the LEDs, as the commands left them.
constexpr uint8_t TRAY_OPEN = 0, TRAY_CLOSED = 2;
constexpr uint32_t XN_SYS_TRAY_STATE_CHANGED = 0x8000000D; // kXNotificationSystemTrayStateChanged

struct Smc {
    std::mutex mutex;
    uint8_t tray = TRAY_OPEN;
    uint8_t ir_address = 0x0F; // MEDIA_REMOTE_360
    uint8_t tilt = 0;          // vertical
    uint32_t cpu_fan = 100, gpu_fan = 100;
    uint8_t power_led[2] = {0x01, 0x00}; // on, not animated
    uint8_t leds[2] = {0x80, 0x01};      // green, top left
};

// SetTrayState: the state, and every listener told
void set_tray(Kernel &k, uint8_t state) {
    {
        Smc &smc = k.state<Smc>();
        std::lock_guard<std::mutex> l(smc.mutex);
        smc.tray = state;
    }
    broadcast(k, XN_SYS_TRAY_STATE_CHANGED, state);
}

// (message, response: X_SMC_DATA, a command byte and 15 of data): the
// queries answer in the response with the command and their fields, the
// rest of it left as it was; the settings answer nothing (but the IR
// address's, which answers the address unchanged, as Xenia's)
XF_EXPORT(xboxkrnl, HalSendSMCMessage) {
    uint32_t message = arg(k, c, 0), response = arg(k, c, 1);
    if (!message) return;
    uint8_t command = k.host(message)[0];
    const uint8_t *data = k.host(message) + 1;
    Smc &smc = k.state<Smc>();
    auto answer = [&](std::initializer_list<uint8_t> fields) {
        if (!response) return;
        k.host(response)[0] = command;
        size_t n = 1;
        for (uint8_t b : fields) k.host(response)[n++] = b;
    };
    // X_TEMPERATURE_DATA::SetTemp: the degrees in 8.8, low byte first
    auto temperature = [](float celsius) { return uint16_t(celsius * 256.0f); };
    uint16_t cpu = temperature(69.6f), gpu = temperature(69.9f);
    switch (command) {
    case 0x07: // QUERY_TEMP_SENSOR: CPU, GPU, EDRAM, motherboard
        answer({uint8_t(cpu), uint8_t(cpu >> 8), uint8_t(gpu), uint8_t(gpu >> 8), uint8_t(cpu), uint8_t(cpu >> 8),
                uint8_t(gpu), uint8_t(gpu >> 8)});
        break;
    case 0x0A: { // QUERY_TRAY
        std::lock_guard<std::mutex> l(smc.mutex);
        answer({smc.tray});
        break;
    }
    case 0x0F: // QUERY_AV_PACK: the AV pack's SMC value (HDMI's 0x1F)
        answer({AV_PACK == 8 ? uint8_t(0x1F) : uint8_t(0)});
        break;
    case 0x12: answer({65, 2, 1}); break; // QUERY_SMC_VERSION
    case 0x16:                            // QUERY_IR_ADDRESS
    case 0x95: {                          // SET_IR_ADDRESS
        std::lock_guard<std::mutex> l(smc.mutex);
        answer({smc.ir_address});
        break;
    }
    case 0x17: { // QUERY_TILT_SENSOR
        std::lock_guard<std::mutex> l(smc.mutex);
        answer({smc.tilt});
        break;
    }
    case 0x89:   // SET_FAN_SPEED_CPU
    case 0x94: { // SET_FAN_SPEED_GPU
        std::lock_guard<std::mutex> l(smc.mutex);
        (command == 0x89 ? smc.cpu_fan : smc.gpu_fan) = uint32_t(data[0] - 0x80);
        break;
    }
    case 0x8B: set_tray(k, uint8_t((data[0] & 0xF) % 5)); break; // SET_DVD_TRAY
    case 0x8C: {                                                 // SET_POWER_LED
        std::lock_guard<std::mutex> l(smc.mutex);
        smc.power_led[0] = data[0], smc.power_led[1] = data[1];
        break;
    }
    case 0x99: { // SET_LEDS
        std::lock_guard<std::mutex> l(smc.mutex);
        smc.leds[0] = data[0], smc.leds[1] = data[1];
        break;
    }
    default: {
        static std::mutex logged_mutex;
        static bool logged[256] = {};
        std::lock_guard<std::mutex> l(logged_mutex);
        if (!logged[command]) {
            logged[command] = true;
            std::fprintf(stderr, "kernel: HalSendSMCMessage: no SMC command %02X\n", command);
        }
        break;
    }
    }
}

// (open): the tray opened or closed, the listeners told
XF_EXPORT(xboxkrnl, HalOpenCloseODDTray) { set_tray(k, arg(k, c, 0) ? TRAY_OPEN : TRAY_CLOSED); }

// ---- the keyboard and the USB camera (xboxkrnl_hid.cc, xboxkrnl_usbcam.cc) ----

// (0, a 6-byte buffer, a 20-byte buffer): no keyboard,
// STATUS_DEVICE_NOT_CONNECTED, which games turn into ERROR_DEVICE_NOT_CONNECTED
XF_EXPORT(xboxkrnl, HidReadKeys) { result(c, 0xC000009D); }

// (buffer, size, handle out): the camera's buffers taken, success (games
// that see a failure skip their initialization and crash later, as Xenia
// notes), no handle
XF_EXPORT(xboxkrnl, XUsbcamCreate) { result(c, X_STATUS_SUCCESS); }
// no camera connected
XF_EXPORT(xboxkrnl, XUsbcamGetState) { result(c, 0); }

// ---- the clock (xboxkrnl_threading.cc, xboxkrnl_misc.cc) ----

XF_EXPORT(xboxkrnl, KeQueryInterruptTime) { c.r[3] = k.load64(k.variable("KeTimeStampBundle")); }

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

// The settings as the title sees them: the defaults, then what it wrote.
struct XConfig {
    std::mutex mutex;
    std::map<uint32_t, std::vector<uint8_t>> values = settings();
};

XF_EXPORT(xboxkrnl, ExGetXConfigSetting) {
    // (category, setting, buffer, buffer_size, required_size_ptr): an
    // unknown setting has size 0
    uint16_t category = uint16_t(arg(k, c, 0)), setting = uint16_t(arg(k, c, 1));
    uint32_t buffer = arg(k, c, 2), required_size_ptr = arg(k, c, 4);
    uint16_t buffer_size = uint16_t(arg(k, c, 3));
    XConfig &x = k.state<XConfig>();
    std::lock_guard<std::mutex> l(x.mutex);
    auto it = x.values.find(uint32_t(category) << 16 | setting);
    uint16_t size = it == x.values.end() ? 0 : uint16_t(it->second.size());
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

// (category, setting, buffer, buffer size): the setting's whole size taken
// from the buffer, whatever size is given (Xenia's XConfig::WriteSetting);
// an unknown setting is left alone; success either way
XF_EXPORT(xboxkrnl, ExSetXConfigSetting) {
    uint16_t category = uint16_t(arg(k, c, 0)), setting = uint16_t(arg(k, c, 1));
    uint32_t buffer = arg(k, c, 2);
    XConfig &x = k.state<XConfig>();
    std::lock_guard<std::mutex> l(x.mutex);
    auto it = x.values.find(uint32_t(category) << 16 | setting);
    if (it != x.values.end() && buffer) std::memcpy(it->second.data(), k.host(buffer), it->second.size());
    std::fprintf(stderr, "kernel: ExSetXConfigSetting: category 0x%04x, setting 0x%04x\n", category, setting);
    result(c, X_STATUS_SUCCESS);
}

// (category, setting, bits kept, bits set): a 32-bit setting's value
// masked, then or'ed (Xenia's: a setting of another size reads as 0 and gets
// its first bytes of the result)
XF_EXPORT(xboxkrnl, ExReadModifyWriteXConfigSettingUlong) {
    uint16_t category = uint16_t(arg(k, c, 0)), setting = uint16_t(arg(k, c, 1));
    uint32_t keep = arg(k, c, 2), set = arg(k, c, 3);
    XConfig &x = k.state<XConfig>();
    std::lock_guard<std::mutex> l(x.mutex);
    auto it = x.values.find(uint32_t(category) << 16 | setting);
    if (it != x.values.end()) {
        std::vector<uint8_t> &v = it->second;
        uint32_t value = v.size() == 4 ? uint32_t(v[0]) << 24 | v[1] << 16 | v[2] << 8 | v[3] : 0;
        std::vector<uint8_t> bytes = be32((value & keep) | set);
        std::copy_n(bytes.begin(), std::min<size_t>(v.size(), 4), v.begin());
    }
    std::fprintf(stderr,
                 "kernel: ExReadModifyWriteXConfigSettingUlong: category 0x%04x, setting 0x%04x, bits kept "
                 "0x%08X, bits set 0x%08X\n",
                 category, setting, keep, set);
    result(c, X_STATUS_SUCCESS);
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

[[noreturn]] void bug_check(Kernel &k, uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
    char message[96];
    std::snprintf(message, sizeof(message), "*** STOP: 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)", code, p1, p2, p3, p4);
    std::fprintf(stderr, "kernel: %s\n", message);
    if (!k.stopped.exchange(true)) k.stop_reason = std::string("KeBugCheck: ") + message;
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(4);
}

uint32_t smc_tray_state(Kernel &k) {
    Smc &smc = k.state<Smc>();
    std::lock_guard<std::mutex> l(smc.mutex);
    return smc.tray;
}

} // namespace xf::kernel
