// The calls for what is not attached or not running (src/kernel/kernel_absent.cpp,
// xam_absent.cpp, and the XNet and input ones that go with them): each answers
// as the console's own kernel and XAM do with the thing absent (the camera, the
// receiver, force feedback, the Kinect's motor, the biometric sensor, the cache
// partitions, the Live upload queue, the guide's custom actions, a network that
// never connects), and the LZX decompression of the cabinet library works on
// frames. The answers are the console's, read from it; the arguments of each
// are what a game passes.

#include "kernel_harness.h"

#include "files.h"

#include <cstring>

using namespace xf;

namespace {

KernelHarness h;

constexpr uint32_t DEVICE_NOT_CONNECTED = 0xC000009D, INVALID_DEVICE_REQUEST = 0xC0000010,
                   INVALID_PARAMETER = 0xC000000D, DEVICE_NOT_READY = 0xC00000A3, UNSUCCESSFUL = 0xC0000001;

// a request of the camera's: type in the first word, the rest 0xAA
uint32_t cam(uint32_t type, uint32_t *second = nullptr) {
    uint32_t request = h.alloc(64);
    std::memset(h.k->host(request), 0xAA, 64);
    h.k->store32(request, type);
    uint32_t r = h.krnl("PsCamDeviceRequest", {request});
    if (second) *second = h.k->load32(request + 4);
    return r;
}

void camera() {
    uint32_t second = 0;
    CHECK(cam(0, &second) == 0 && second == 0);
    CHECK(cam(29, &second) == 0 && second == DEVICE_NOT_CONNECTED);
    for (uint32_t t : {10u, 38u, 45u}) CHECK(cam(t) == 0);
    CHECK(cam(13) == DEVICE_NOT_READY);
    for (uint32_t t : {1u,  2u,  3u,  5u,  8u,  14u, 15u, 18u, 19u, 20u, 21u, 24u, 25u,
                       27u, 30u, 33u, 34u, 35u, 36u, 37u, 40u, 41u, 43u, 44u, 46u, 47u})
        CHECK(cam(t) == DEVICE_NOT_CONNECTED);
    for (uint32_t t :
         {4u, 6u, 7u, 9u, 11u, 12u, 16u, 17u, 22u, 23u, 26u, 28u, 31u, 32u, 39u, 42u, 48u, 105u, 106u, 1000u})
        CHECK(cam(t) == INVALID_DEVICE_REQUEST);
}

void receiver() {
    uint32_t request = h.alloc(32);
    auto make = [&](uint16_t type, uint16_t index) {
        std::memset(h.k->host(request), 0xAA, 32);
        h.k->store16(request, type);
        h.k->store16(request + 2, index);
        return h.krnl("RmcDeviceRequest", {request});
    };
    CHECK(make(0, 0) == 0 && h.k->load32(request + 4) == 0);
    CHECK(make(0, 3) == 0);
    CHECK(make(1, 1) == DEVICE_NOT_CONNECTED);
    CHECK(make(24, 0) == DEVICE_NOT_CONNECTED);
    CHECK(make(0, 4) == INVALID_PARAMETER);
    CHECK(make(5, 0x20) == INVALID_PARAMETER);
}

void force_feedback() {
    const char *names[] = {"XInputdFFGetDeviceInfo",   "XInputdFFSetEffect",     "XInputdFFUpdateEffect",
                           "XInputdFFEffectOperation", "XInputdFFDeviceControl", "XInputdFFSetDeviceGain",
                           "XInputdFFSetRumble"};
    uint32_t buffer = h.alloc(64);
    for (const char *name : names) {
        for (uint32_t port = 0; port < 4; port++)
            CHECK(h.krnl(name, {0x10000000 | port, buffer, 0, 0}) == INVALID_DEVICE_REQUEST);
        CHECK(h.krnl(name, {0x10000004, buffer, 0, 0}) == DEVICE_NOT_CONNECTED);
        CHECK(h.krnl(name, {0x1000000F, buffer, 0, 0}) == DEVICE_NOT_CONNECTED);
        CHECK(h.krnl(name, {0x20000000, buffer, 0, 0}) == UNSUCCESSFUL);
        CHECK(h.krnl(name, {0, buffer, 0, 0}) == UNSUCCESSFUL);
    }
    // the cancel of I/O does not tell the ports apart
    for (uint32_t port : {0u, 3u, 4u, 15u})
        CHECK(h.krnl("XInputdFFCancelIo", {0x10000000 | port, 0, 0, 0}) == INVALID_DEVICE_REQUEST);
    CHECK(h.krnl("XInputdFFCancelIo", {0x30000000, 0, 0, 0}) == UNSUCCESSFUL);
}

void event_trace() {
    constexpr uint32_t BLOCK = 0x100;
    uint32_t id = h.alloc(16), events = h.alloc(0x2C * 3);
    std::memset(h.k->host(events), 0xEE, 0x2C * 3);
    uint32_t block = h.alloc(BLOCK);
    std::memset(h.k->host(block), 0x77, BLOCK);
    // a block under 0x40 bytes is refused, and zeroed as far as it goes
    CHECK(h.krnl("EtxProducerRegister", {id, events, 3, 0, 0x30, block}) == INVALID_PARAMETER);
    CHECK(h.k->load32(block) == 0 && h.k->load32(block + 0x2C) == 0 && h.k->load32(block + 0x30) == 0x77777777);
    CHECK(h.krnl("EtxProducerRegister", {id, events, 3, 0x00010005, 0x80, block}) == 0);
    CHECK(h.k->load32(block + 0) == 0x80 && h.k->load32(block + 0x1C) == id && h.k->load32(block + 4) == 0x00010000);
    CHECK(h.k->load32(block + 0x10) == events && h.k->load32(block + 0x14) == 3);
    CHECK(h.k->load32(block + 0x18) >> 22 == 0);
    // each event's state words (+0x1C, eight bytes) are cleared, nothing else
    CHECK(h.k->load32(events + 0x1C) == 0 && h.k->load32(events + 0x20) == 0 && h.k->load32(events + 0x44 + 4) == 0);
    CHECK(h.k->load32(events + 0x24) == 0xEEEEEEEE && h.k->load32(events) == 0xEEEEEEEE);
    // no session holds a buffer: the event is dropped and counted
    h.k->store32(events + 0x2C + 0x20, 5);
    CHECK(h.krnl("EtxProducerLog", {block, 1, 0, 0, 0, 0}) == 0xC0000188);
    CHECK(h.k->load32(events + 0x2C + 0x20) == 6);
    // a second producer gets the next slot
    uint32_t id2 = h.alloc(16), block2 = h.alloc(BLOCK);
    CHECK(h.krnl("EtxProducerRegister", {id2, 0, 0, 0x20000, 0x40, block2}) == 0);
    CHECK(h.k->load32(block2 + 0x18) >> 22 == 1 && h.k->load32(block2 + 0x10) == 0);
    // unregistering gives the slot back and zeroes the block
    CHECK(h.krnl("EtxProducerUnregister", {block}) == 0);
    CHECK(h.k->load32(block) == 0 && h.k->load32(block + 0x1C) == 0);
    CHECK(h.krnl("EtxProducerUnregister", {block}) == INVALID_PARAMETER);
    CHECK(h.krnl("EtxProducerLog", {block, 0, 0, 0, 0, 0}) == INVALID_PARAMETER);
    uint32_t block3 = h.alloc(BLOCK);
    CHECK(h.krnl("EtxProducerRegister", {id, events, 3, 0, 0x80, block3}) == 0 &&
          h.k->load32(block3 + 0x18) >> 22 == 2); // the search goes on from the last slot given
    // the table holds 128 producers
    uint32_t blocks[130];
    uint32_t ok = 0, full = 0;
    for (uint32_t &b : blocks) {
        b = h.alloc(0x40);
        uint32_t r = h.krnl("EtxProducerRegister", {id, 0, 0, 0, 0x40, b});
        (r == 0 ? ok : full) += r == 0 || r == 0x8000001A;
    }
    CHECK(ok == 126 && full == 4); // two were taken above
    CHECK(h.k->load32(blocks[129]) == 0);
}

// LZX's specification example, "abc" in an uncompressed block
const uint8_t ABC[20] = {0x00, 0x30, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
                         0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x61, 0x62, 0x63, 0x00};

void ldi() {
    uint32_t block_max = h.alloc(4), config = h.alloc(8), source_min = h.alloc(4), context_ptr = h.alloc(4);
    uint32_t user = h.alloc(0x3000);
    h.k->store32(block_max, 0x8000);
    h.k->store32(config, 0x10000);
    h.k->store32(config + 4, 0x1234);
    CHECK(h.krnl("LDICreateDecompression", {block_max, config, 0, 0, user, source_min, context_ptr}) == 0);
    CHECK(h.k->load32(source_min) == 0x8000 + 0x1800);
    uint32_t context = h.k->load32(context_ptr);
    CHECK(context == user && h.k->load32(context) == 0x4349444C && h.k->load32(context + 4) == 0x8000 &&
          h.k->load32(context + 8) == 0x1234);
    uint32_t source = h.alloc(32), destination = h.alloc(0x8000), size = h.alloc(4);
    std::memcpy(h.k->host(source), ABC, 20);
    h.k->store32(size, 3);
    CHECK(h.krnl("LDIDecompress", {context, source, 20, destination, size}) == 0);
    CHECK(h.k->load32(size) == 3 && std::memcmp(h.k->host(destination), "abc", 3) == 0);
    // a bigger frame than the block, a context that is not one, garbage
    h.k->store32(size, 0x8001);
    CHECK(h.krnl("LDIDecompress", {context, source, 20, destination, size}) == 3);
    h.k->store32(size, 3);
    CHECK(h.krnl("LDIDecompress", {user + 0x100, source, 20, destination, size}) == 2);
    std::memset(h.k->host(source), 0xFF, 20);
    CHECK(h.krnl("LDIDecompress", {context, source, 20, destination, size}) == 4);
    CHECK(h.krnl("LDIDestroyDecompression", {context}) == 0);
    CHECK(h.k->load32(context) == 0);
    CHECK(h.krnl("LDIDestroyDecompression", {context}) == 2);

    // a window that is not one: "not enough memory", the context cleared
    h.k->store32(config, 21);
    h.k->store32(context_ptr, 0xAAAAAAAA);
    CHECK(h.krnl("LDICreateDecompression", {block_max, config, 0, 0, user, source_min, context_ptr}) == 1);
    CHECK(h.k->load32(context_ptr) == 0);
    h.k->store32(config, 0x200000);
    CHECK(h.krnl("LDICreateDecompression", {block_max, config, 0, 0, user, source_min, context_ptr}) == 0);
    // no place for the context asked for: the size is still given
    h.k->store32(source_min, 0);
    CHECK(h.krnl("LDICreateDecompression", {block_max, config, 0, 0, user, source_min, 0}) == 0);
    CHECK(h.k->load32(source_min) == 0x9800);
}

void xam_absent() {
    uint32_t buffer = h.alloc(256), out = h.alloc(16);
    auto x = [&](const char *name, std::initializer_list<uint64_t> a) { return h.call32("xam", name, a); };
    // the Kinect's motor and the biometric sensor
    CHECK(x("XamNuiCameraElevationSetAngle", {10}) == 0x8007048F);
    CHECK(x("XamNuiCameraElevationStopMovement", {}) == 0x8007048F);
    CHECK(x("XamNuiCameraRememberFloor", {}) == 0x8007048F);
    for (uint32_t status : {0u, 1u, 2u, 3u}) CHECK(x("XamNuiCameraTiltReportStatus", {status}) == status);
    CHECK(x("XamReadBiometricData", {1, buffer, 4}) == 0x57 && x("XamWriteBiometricData", {1, buffer, 4}) == 0x57);
    CHECK(x("XamContentInstall", {buffer, buffer, buffer, 0}) == 0x57);
    // users
    std::memset(h.k->host(out), 0xAA, 16);
    CHECK(x("XamUserGetAgeGroup", {0, out}) == 0x65B && h.k->load32(out) == 0xAAAAAAAA);
    CHECK(x("XamUserGetAgeGroup", {0, 0}) == 0x57);
    CHECK(x("XamUserNuiGetEnrollmentIndex", {1, out}) == 0x80070525 && h.k->load32(out) == 0xFFFFFFFE);
    h.k->store32(out, 0xAAAAAAAA);
    CHECK(x("XamUserNuiGetEnrollmentIndex", {7, out}) == 0x80070057 && h.k->load32(out) == 0xAAAAAAAA);
    CHECK(x("XamUserNuiGetEnrollmentIndex", {0, 0}) == 0x80070057);
    // the cache partitions
    uint32_t file = h.alloc(16);
    std::memset(h.k->host(file), 0, 16);
    CHECK(x("XamCacheOpenFile", {0}) == 0x57);
    h.k->store32(file + 4, 6);
    CHECK(x("XamCacheOpenFile", {file}) == 0x57);
    h.k->store32(file + 4, 2);
    CHECK(x("XamCacheOpenFile", {file}) == 2 && h.k->host(file)[2] == 0x40);
    CHECK(x("XamCacheCloseFile", {file}) == 0xFFFFFFFF);
    CHECK(x("XamCacheReset", {0xFFFFFFFF}) == 0 && x("XamCacheReset", {5}) == 1);
    // the Live upload queue
    CHECK(x("XamXlfsInitializeUploadQueue", {buffer, buffer}) == 0x80070005);
    CHECK(x("XamXlfsUninitializeUploadQueue", {buffer}) == 0x80150005);
    CHECK(x("XamXlfsMountUploadQueueInstance", {buffer, buffer}) == 0x80070006);
    CHECK(x("XamXlfsUnmountUploadQueueInstance", {buffer}) == 0x80070006);
    CHECK(x("XamBackgroundDownloadSetMode", {2}) == 0 && x("XamVoiceSetMicArrayIdleUsers", {0xF, 0}) == 0);
    // the guide's custom actions: nothing pressed, no session to set actions in
    uint32_t user = h.alloc(4), action = h.alloc(4), xuid = h.alloc(8), text = h.alloc(32), length = h.alloc(2);
    std::memset(h.k->host(xuid), 0xAA, 8);
    h.k->store32(user, 0xAAAAAAAA), h.k->store32(action, 0xAAAAAAAA), h.k->store16(length, 0x20);
    CHECK(x("XCustomGetLastActionPressEx", {user, action, xuid, text, length}) == 0xE8);
    CHECK(h.k->load32(user) == 0 && h.k->load32(action) == 0 && h.k->load64(xuid) == 0 && h.k->load16(length) == 0);
    std::memset(h.k->host(xuid), 0xAA, 8);
    CHECK(x("XCustomGetCurrentGamercard", {user, xuid}) == 0 && h.k->load64(xuid) == 0);
    CHECK(x("XCustomGetCurrentGamercard", {0, xuid}) == 0);
    CHECK(x("XCustomSetDynamicActions", {0, 1, buffer, 1}) == 5 &&
          x("XCustomSetDynamicActions", {0, 1, buffer, 3}) == 5);
    CHECK(x("XCustomSetDynamicActions", {0, 1, buffer, 4}) == 0x57);
    CHECK(x("XCustomSetDynamicActions", {0xFE, 1, buffer, 1}) == 0 &&
          x("XCustomSetDynamicActions", {0, 0, buffer, 1}) == 0);
    CHECK(x("XCustomSetDynamicActions", {0, 1, 0, 1}) == 0 && x("XCustomSetDynamicActions", {0, 1, buffer, 0}) == 0);
    x("XCustomUnregisterDynamicActions", {});
    // a pad's vibration setting and raw state
    CHECK(x("XamInputRawState", {0, buffer}) == 0x48F);
    uint32_t level = h.alloc(4);
    h.k->store32(level, 0xAAAAAAAA);
    uint32_t r = x("XamInputGetUserVibrationLevel", {0, level});
    CHECK((r == 0 && h.k->load32(level) == 3) || (r == 0x48F && h.k->load32(level) == 0xAAAAAAAA));
    CHECK(x("XamInputGetUserVibrationLevel", {4, level}) == 0x48F &&
          x("XamInputGetUserVibrationLevel", {0xFF, level}) == 0x48F);
}

void xnet_absent() {
    auto x = [&](const char *name, std::initializer_list<uint64_t> a) { return h.call32("xam", name, a); };
    // a network that never connects: every secure address is unknown
    CHECK(x("NetDll_XNetConnect", {1, 0x0A000001}) == 10022);
    CHECK(x("NetDll_XNetServerToInAddr", {1, 0x0A000001, 5, 0}) == 10022);
    CHECK(x("NetDll_XNetUnregisterInAddr", {1, 0x0A000001}) == 10022);
    CHECK(x("NetDll_XNetQosGetListenStats", {1, 0, 0}) == 10022);
    CHECK(x("NetDll_XNetGetConnectStatus", {1, 0x0A000001}) == 3);
    // the QoS lookup: nothing asked is nothing answered; secure addresses it
    // has no arrays for are invalid; addresses are all probed, at once, and
    // none contacted
    uint32_t qos_ptr = h.alloc(4), ina = h.alloc(8), service = h.alloc(8), event = 0;
    std::memset(h.k->host(qos_ptr), 0xAA, 4);
    CHECK(x("NetDll_XNetQosLookup", {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, qos_ptr}) == 0 &&
          h.k->load32(qos_ptr) == 0xAAAAAAAA);
    CHECK(x("NetDll_XNetQosLookup", {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, qos_ptr}) == 10022);
    h.k->store32(ina, 0x0A000001), h.k->store32(ina + 4, 0x0A000002);
    CHECK(x("NetDll_XNetQosLookup", {1, 0, 0, 0, 0, 2, ina, service, 5, 0, 0, event, qos_ptr}) == 0);
    uint32_t qos = h.k->load32(qos_ptr);
    CHECK(qos && h.k->load32(qos) == 2 && h.k->load32(qos + 4) == 0 && h.k->host(qos + 8)[0] == 1 &&
          h.k->host(qos + 8 + 0x18)[0] == 1);
    CHECK(x("NetDll_XNetQosRelease", {1, qos}) == 0);
    // sockets: one the stack never made, one made and not connected
    CHECK(x("NetDll_WSAStartup", {1, 0x0202, h.alloc(0x190)}) == 0);
    uint32_t name = h.alloc(16), length = h.alloc(4);
    h.k->store32(length, 16);
    CHECK(x("NetDll_getpeername", {1, 0x1234, name, length}) == 0xFFFFFFFF);
    uint32_t socket = x("NetDll_socket", {1, 2, 2, 17});
    CHECK(socket != 0xFFFFFFFF);
    CHECK(x("NetDll_getpeername", {1, socket, name, length}) ==
          0xFFFFFFFF); // (the errors are the net test's, on guest threads)
    CHECK(x("NetDll_closesocket", {1, socket}) == 0);
}

// Small answers of the console's kernel that Xenia's differ from: a formatted
// length with no buffer, the cache count's limit, the STFS device's checks
void console_answers() {
    uint32_t format = h.string("%d");
    CHECK(h.krnl("_snprintf", {0, 8, format, 7}) == 1);
    CHECK(h.krnl("_snprintf", {0, 0, h.string("abcdefgh")}) == 8);
    uint32_t buffer = h.alloc(16);
    CHECK(h.krnl("_snprintf", {buffer, 0, format, 7}) == 0xFFFFFFFF);
    CHECK(h.krnl("_snprintf", {buffer, 8, format, 7}) == 1 && h.read(buffer) == "7");
    CHECK(h.krnl("FscSetCacheElementCount", {0, 0x401}) == INVALID_PARAMETER);
    CHECK(h.krnl("FscSetCacheElementCount", {0, 0x400}) == 0);
    uint32_t parameters = h.alloc(0x58);
    std::memset(h.k->host(parameters), 0, 0x58);
    CHECK(h.krnl("StfsCreateDevice", {parameters, 0x58, 0}) == INVALID_PARAMETER);
    h.k->host(parameters)[0x50] = 5;
    CHECK(h.krnl("StfsCreateDevice", {parameters, 0x40, 0}) == INVALID_PARAMETER);
    CHECK(h.krnl("StfsCreateDevice", {0, 0x58, 0}) == INVALID_PARAMETER);
    CHECK(h.krnl("StfsCreateDevice", {parameters, 0x58, 0}) == 0);
}

// The voice objects the console's XAM makes (no headset, no driver device) and
// the voice driver's answers; the four calls its launch reaches.
void voice_and_launch() {
    uint32_t out = h.alloc(16);
    auto x = [&](const char *name, std::initializer_list<uint64_t> a) { return h.call32("xam", name, a); };
    // the voice: a slot of the table, its packets in a free list
    h.k->store32(out, 0xAAAAAAAA);
    CHECK(x("XamVoiceCreate", {0, 15, out}) == 0);
    uint32_t voice = h.k->load32(out), buffer = h.k->load32(voice + 8);
    CHECK(voice != 0 && buffer != 0 && h.k->load32(voice + 4) == 15 && h.k->host(voice)[0x14] == 1);
    CHECK(h.k->load32(voice + 0xC) == buffer + 14 * 16 && h.k->load32(voice + 0x10) == buffer);
    CHECK(h.k->load32(buffer + 8) == voice && h.k->load32(buffer + 14 * 16 + 12) == buffer + 13 * 16);
    CHECK(h.k->load32(buffer + 12) == 0 && h.k->load32(voice + 0x1C) == voice + 0x1C);
    CHECK(h.k->load64(voice + 0x28) != 0 && h.k->load64(voice + 0x30) == 0);
    // the slot is the user's (four to a side); a second make takes the same one
    CHECK(x("XamVoiceCreate", {0, 15, out}) == 0 && h.k->load32(out) == voice);
    CHECK(x("XamVoiceCreate", {1, 4, out}) == 0 && h.k->load32(out) == voice + 0x38);
    CHECK(x("XamVoiceCreate", {5, 4, out}) == 0 && h.k->load32(out) == voice + 5 * 0x38);
    CHECK(x("XamVoiceCreate", {8, 4, out}) == 0x80070057);
    // a buffer that cannot be had: nothing, and a null pointer written
    h.k->store32(out, 0xAAAAAAAA);
    CHECK(x("XamVoiceCreate", {2, 0x10000000, out}) == 0x8007000E && h.k->load32(out) == 0);
    CHECK(x("XamVoiceCreate", {2, 0, out}) == 0 && h.k->load32(out) == voice + 2 * 0x38 &&
          h.k->load32(voice + 2 * 0x38 + 0xC) == 0);
    // with no device: no headset, nothing to submit to; the title's process is the active one
    CHECK(x("XamVoiceHeadsetPresent", {voice}) == 0);
    CHECK(x("XamVoiceSubmitPacket", {voice, 0, out}) == 0xD000009D);
    CHECK(x("XamVoiceIsActiveProcess", {}) == 1);
    h.krnl("XVoicedActivate", {1});
    CHECK(x("XamVoiceIsActiveProcess", {}) == 0 && h.krnl("XVoicedIsActiveProcess") == 0);
    h.krnl("XVoicedActivate", {0});
    CHECK(h.krnl("XVoicedIsActiveProcess") == 1);
    CHECK(h.krnl("XVoicedGetDirectionalData", {voice, out}) == 0x10DF &&
          h.krnl("XVoicedGetBatteryStatus", {0, out}) == 0x10DF);
    CHECK(h.krnl("XVoicedSendVPort", {0x1234, 0}) == 0x1234 && h.krnl("XVoicedClose", {voice}) == 0);
    // closing frees the buffer and answers TRUE
    CHECK(x("XamVoiceClose", {voice}) == 1);
    CHECK(h.k->load32(voice + 8) == 0 && h.k->load32(voice + 4) == 0 && h.k->load64(voice + 0x28) == 0);
    CHECK(x("XamVoiceClose", {voice}) == 1);

    // XNetLogonGetTitleID: the logon service's own ID, as in kernel_net_test

    // KeSetPriorityClassThread: the class change moves the bases
    uint32_t process = h.alloc(0x60), thread = h.alloc(0x400);
    std::memset(h.k->host(process), 0, 0x60);
    std::memset(h.k->host(thread), 0, 0x400);
    h.k->store32(process + 0xC, 0x77);
    h.k->store32(thread + 0x84, process);
    uint8_t *g = h.k->host(thread);
    g[0xC9] = 5, g[0xB9] = 9, g[0x70] = 9, g[0xC8] = 5, g[0xCA] = 11;
    h.krnl("KeSetPriorityClassThread", {thread, 1});
    CHECK(g[0xC9] == 13 && g[0xB9] == 17 && g[0x70] == 17 && g[0xC8] == 13 && g[0xCA] == 19);
    CHECK(h.k->load32(thread + 0xB4) == 0x77);
    h.krnl("KeSetPriorityClassThread", {thread, 1}); // the same class: nothing
    CHECK(g[0xB9] == 17);
    h.krnl("KeSetPriorityClassThread", {thread, 0});
    CHECK(g[0xC9] == 5 && g[0xB9] == 9 && g[0x70] == 9);
    g[0x70] = 0x12; // a real-time thread keeps its priority
    h.krnl("KeSetPriorityClassThread", {thread, 1});
    CHECK(g[0xB9] == 17 && g[0x70] == 0x12);

    CHECK(h.krnl("XexShimLock") == 0);

    // IoSynchronousDeviceIoControlRequest: a device with no driver
    uint32_t device = h.alloc(0x50), returned = h.alloc(4);
    std::memset(h.k->host(device), 0, 0x50);
    h.k->store16(device, 3);
    h.k->store32(returned, 0x55);
    CHECK(h.krnl("IoSynchronousDeviceIoControlRequest", {0x4D014, device, 0, 0, 0, 0, returned}) ==
          INVALID_DEVICE_REQUEST);
    CHECK(h.k->load32(returned) == 0);
    CHECK(h.krnl("IoSynchronousDeviceIoControlRequest", {0x4D014, 0, 0, 0, 0, 0, returned}) == INVALID_PARAMETER);

    // ObTranslateSymbolicLink: links followed, devices found, the rest appended
    xf::kernel::file_system(*h.k).add_link("\\TestLink", "\\Device\\Cdrom0");
    uint32_t attributes = h.alloc(12), name = h.alloc(8), text = h.alloc(64), result = h.alloc(8), buf = h.alloc(64);
    h.k->store32(name + 4, text);
    h.k->store32(attributes + 4, name);
    h.k->store16(result + 2, 64);
    h.k->store32(result + 4, buf);
    auto translate = [&](const char *path, uint32_t root = 0) {
        std::memcpy(h.k->host(text), path, std::strlen(path) + 1);
        h.k->store16(name, uint16_t(std::strlen(path)));
        h.k->store16(name + 2, 64);
        h.k->store32(attributes, root);
        h.k->store16(result, 0xFFFF);
        return h.krnl("ObTranslateSymbolicLink", {attributes, result});
    };
    auto translated = [&] { return std::string(reinterpret_cast<const char *>(h.k->host(buf)), h.k->load16(result)); };
    CHECK(translate("\\TestLink\\x\\y") == 0 && translated() == "\\Device\\Cdrom0\\x\\y");
    CHECK(translate("\\Device\\Harddisk0\\Partition1\\a") == 0 && translated() == "\\Device\\Harddisk0\\Partition1\\a");
    CHECK(translate("\\Nowhere\\x") == 0xC0000034 && h.k->load16(result) == 0);
    CHECK(translate("relative") == INVALID_PARAMETER && translate("") == INVALID_PARAMETER);
    CHECK(translate("\\TestLink", 5) == INVALID_PARAMETER);
    h.k->store16(result + 2, 4);
    CHECK(translate("\\TestLink\\x") == 0xC0000023);
}

} // namespace

int main() {
    if (!h.init()) return 1;
    voice_and_launch();
    console_answers();
    camera();
    receiver();
    force_feedback();
    event_trace();
    ldi();
    xam_absent();
    xnet_absent();
    return finish("kernel_absent");
}
