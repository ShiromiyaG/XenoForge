// The voice calls: XAM's XamVoice* and the kernel's XVoiced* driver behind them,
// with no headset and no voice driver device. Xenia has stubs here (Create
// refuses, the rest answer 0); the console's XAM makes a voice object, and
// every answer below was read from the console's own XAM and kernel run under
// ours (xfxam, xfkrnl; the calls are in tests/kernel_absent_test.cpp).
//
// What the console does:
//   - XamVoiceCreate(user, max packets, out *) makes no device and asks no
//     driver. It keeps its voice objects in a table of eight 0x38-byte slots
//     in its own data, four for a title's threads and four for the system's
//     (the slot is the user index, plus four on a system thread's), and hands
//     out a pointer into it. The slot is cleared (its first word, the device's
//     tag, is kept: always 0 here, no device ever attaches), the packet
//     count stored, a buffer of 16 bytes a packet allocated from XAM's heap
//     (XamAlloc's), the packets linked into a free list, the time stamped,
//     and the pointer written. No argument is checked: a user index past three
//     takes the slot of the other side's, and past seven writes into XAM's
//     next data, which is not made here (it answers E_INVALIDARG, nothing
//     written). A buffer that does not allocate answers E_OUTOFMEMORY and writes
//     a null pointer. A null out pointer faults on the console; it is
//     left unwritten here.
//   - XamVoiceClose(voice) hands the voice to XVoicedClose, frees the packet
//     buffer, clears the buffer and count, and answers TRUE (1; Xenia's 0).
//   - XamVoiceHeadsetPresent, XamVoiceSubmitPacket and XamVoiceIsActiveProcess
//     pass their arguments to XVoicedHeadsetPresent, XVoicedSubmitPacket and
//     XVoicedIsActiveProcess.
//   - The driver reads a voice's first word as a device tag (high nibble: 1, 2,
//     4, 7, 8 are the headset and the microphone array's kinds). With no device
//     (tag 0, the only one a voice made here has) HeadsetPresent answers 0,
//     SubmitPacket 0xD000009D, Close 0, GetDirectionalData and GetBatteryStatus
//     0x10DF, and SendVPort leaves its argument as the answer. Activate stores
//     whether the system's process is the one using the voice; IsActiveProcess
//     is true when that agrees with the caller's process (a title's, when the
//     flag is clear: its start).
//
// Where it differs from the console: the kinds of device a tag could name are
// not modelled (none can attach); the statistics XAM logs when a voice closes
// (time open, to its event trace, not observable) are not kept apart from the
// two stamps in the slot.

#include "xam.h"

#include <cstring>

namespace xf::kernel {

namespace {

constexpr uint32_t E_OUTOFMEMORY = 0x8007000E;
constexpr uint32_t STATUS_DEVICE_NOT_CONNECTED_FATAL = 0xD000009D;
constexpr uint32_t DRIVER_NO_DEVICE = 0x10DF;

constexpr uint32_t SLOT_SIZE = 0x38, SLOT_COUNT = 8;
// the slot's fields
constexpr uint32_t SLOT_TAG = 0x00, SLOT_PACKETS = 0x04, SLOT_BUFFER = 0x08, SLOT_FREE_HEAD = 0x0C,
                   SLOT_FREE_TAIL = 0x10, SLOT_FLAG = 0x14, SLOT_STATE = 0x18, SLOT_LIST = 0x1C, SLOT_OPENED = 0x28,
                   SLOT_COUNTED = 0x30;

struct VoiceState {
    uint32_t slots = 0;         // guest address of the table, made on first use
    bool system_active = false; // XVoicedActivate's flag
};

uint32_t process_type(Kernel &k, ppc::Context &c) {
    uint32_t pcr = uint32_t(c.r[13]);
    if (!k.load32(pcr + kpcr::DPC_ACTIVE)) {
        uint32_t thread = k.load32(pcr + kpcr::CURRENT_THREAD);
        return thread ? k.host(thread)[kthread::PROCESS_TYPE] : kprocess::TITLE; // (a bare context has none)
    }
    return k.host(pcr)[kpcr::PROCESS_TYPE_IN_DPC];
}

} // namespace

// ---- the kernel's driver (xboxkrnl), no device attached ----

// (voice *): the device's tag selects the device; none is attached
XF_EXPORT(xboxkrnl, XVoicedHeadsetPresent) { result(c, 0); }

// (voice *, flag, packet *)
XF_EXPORT(xboxkrnl, XVoicedSubmitPacket) { result(c, STATUS_DEVICE_NOT_CONNECTED_FATAL); }

// (voice *)
XF_EXPORT(xboxkrnl, XVoicedClose) { result(c, 0); }

// (flag): kept for XVoicedIsActiveProcess; r3 is left as it was
XF_EXPORT(xboxkrnl, XVoicedActivate) { k.state<VoiceState>().system_active = (arg(k, c, 0) & 0xFF) != 0; }

// (): the calling process is the one the voice is given to
XF_EXPORT(xboxkrnl, XVoicedIsActiveProcess) {
    bool system_process = process_type(k, c) != kprocess::TITLE;
    result(c, k.state<VoiceState>().system_active == system_process ? 1 : 0);
}

// (tag, packet): a request of the headset's or the array's own; r3 is left as it was
XF_EXPORT(xboxkrnl, XVoicedSendVPort) {}

// (tag, status *)
XF_EXPORT(xboxkrnl, XVoicedGetBatteryStatus) { result(c, DRIVER_NO_DEVICE); }

// (voice *, data *)
XF_EXPORT(xboxkrnl, XVoicedGetDirectionalData) { result(c, DRIVER_NO_DEVICE); }

// ---- XAM ----

// (user_index, max_attached_packets, voice **)
XF_EXPORT(xam, XamVoiceCreate) {
    uint32_t user = arg(k, c, 0), max_packets = arg(k, c, 1), out = arg(k, c, 2);
    uint64_t index = uint64_t(user) + (process_type(k, c) != kprocess::TITLE ? 4 : 0);
    if (index >= SLOT_COUNT) return result(c, X_E_INVALIDARG);

    VoiceState &s = k.state<VoiceState>();
    if (!s.slots) {
        s.slots = k.memory().system_alloc(SLOT_SIZE * SLOT_COUNT);
        if (!s.slots) return result(c, E_OUTOFMEMORY);
        std::memset(k.host(s.slots), 0, SLOT_SIZE * SLOT_COUNT);
    }
    uint32_t slot = s.slots + uint32_t(index) * SLOT_SIZE;
    uint32_t tag = k.load32(slot + SLOT_TAG);
    std::memset(k.host(slot), 0, SLOT_SIZE);
    k.host(slot)[SLOT_FLAG] = 1;
    k.store32(slot + SLOT_LIST, slot + SLOT_LIST);
    k.store32(slot + SLOT_LIST + 4, slot + SLOT_LIST);
    k.store32(slot + SLOT_PACKETS, max_packets);
    k.store32(slot + SLOT_TAG, tag);

    // 16 bytes a packet; a count past 0x0FFFFFFF asks for all the address space
    uint32_t size = max_packets <= 0x0FFFFFFF ? max_packets << 4 : 0xFFFFFFFF;
    // (the console's whole memory is 512 MB: a request past it cannot be had)
    uint32_t buffer = size >= 0x20000000 ? 0 : k.memory().system_alloc(size ? size : 16);
    k.store32(slot + SLOT_BUFFER, buffer);
    if (!buffer) {
        if (out) k.store32(out, 0);
        return result(c, E_OUTOFMEMORY);
    }
    std::memset(k.host(buffer), 0, size);

    // the packets from the last to the first, each appended to the free list
    uint32_t head = 0, tail = 0;
    for (uint32_t i = max_packets; i > 0; --i) {
        uint32_t packet = buffer + (i - 1) * 16;
        k.store32(packet + 8, slot);
        k.store32(packet + 12, 0);
        if (tail)
            k.store32(tail + 12, packet);
        else
            head = packet;
        tail = packet;
    }
    k.store32(slot + SLOT_FREE_HEAD, head);
    k.store32(slot + SLOT_FREE_TAIL, tail);
    k.store64(slot + SLOT_OPENED, k.system_time());
    k.store64(slot + SLOT_COUNTED, 0);
    if (out) k.store32(out, slot);
    result(c, X_ERROR_SUCCESS);
}

// (voice *): TRUE
XF_EXPORT(xam, XamVoiceClose) {
    uint32_t voice = arg(k, c, 0);
    if (!voice) return result(c, 1); // the console faults on a null voice
    k.store64(voice + SLOT_OPENED, 0);
    k.store64(voice + SLOT_COUNTED, 0);
    call_export(k, c, "xboxkrnl", "XVoicedClose", {voice});
    if (uint32_t buffer = k.load32(voice + SLOT_BUFFER)) {
        k.memory().system_free(buffer);
        k.store32(voice + SLOT_BUFFER, 0);
        k.store32(voice + SLOT_PACKETS, 0);
    }
    result(c, 1);
}

// (voice *)
XF_EXPORT(xam, XamVoiceHeadsetPresent) {
    result(c, call_export(k, c, "xboxkrnl", "XVoicedHeadsetPresent", {arg(k, c, 0)}));
}

// (voice *, flag, packet *)
XF_EXPORT(xam, XamVoiceSubmitPacket) {
    result(c, call_export(k, c, "xboxkrnl", "XVoicedSubmitPacket", {arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)}));
}

// ()
XF_EXPORT(xam, XamVoiceIsActiveProcess) { result(c, call_export(k, c, "xboxkrnl", "XVoicedIsActiveProcess", {})); }

} // namespace xf::kernel
