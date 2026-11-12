// The kernel's calls for what is not here: the camera on the USB bus, the
// remote media centre's receiver, the force-feedback motors of the input driver,
// the event-trace producers' session, the LZX decompression of the cabinet
// library. Xenia has none of them (its table names them, nothing answers).
// Each answers as the console's own kernel does (the calls are in
// tests/kernel_absent_test.cpp): with the device absent, the status it gives and
// what it writes. A request type the console
// refuses is refused here the same way; what it would do with the device is
// not done, there being none.
//
// Where it differs from the console: the event-trace registry is kept here
// (the console's is a table in its own memory, with a lock and a session
// list; no session is ever open, so the masks stay zero); LDICreateDecompression
// does not call a caller's allocator (the context lives in the caller's own
// buffer, or in the system heap when the caller gave an allocator).

#include "xam.h"
#include "xenoforge/kernel.h"
#include "xenoforge/lzx.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace xf::kernel {

namespace {

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
constexpr uint32_t STATUS_DEVICE_NOT_CONNECTED = 0xC000009D;
constexpr uint32_t STATUS_DEVICE_NOT_READY = 0xC00000A3;
constexpr uint32_t STATUS_LOG_FILE_FULL = 0xC0000188;
constexpr uint32_t STATUS_NO_MORE_ENTRIES = 0x8000001A;

} // namespace

// ---- the camera (PsCamDeviceRequest) ----

// (request *): the request's first word is its type, 0 to 0x69. With no camera
// the console answers by type: 0, 10, 29, 38 and 45 succeed (0 and 29 also
// write a status in the request's second word, 0 and 0xC000009D); 13 is not
// ready; 1-3, 5, 8, 14, 15, 18-21, 24, 25, 27, 30, 33-37, 40, 41, 43, 44, 46
// and 47 are not connected; every other type, and anything past 0x69, is an
// invalid device request.
XF_EXPORT(xboxkrnl, PsCamDeviceRequest) {
    uint32_t request = arg(k, c, 0);
    uint32_t type = k.load32(request);
    switch (type) {
    case 0:
        k.store32(request + 4, STATUS_SUCCESS);
        return result(c, STATUS_SUCCESS);
    case 29:
        k.store32(request + 4, STATUS_DEVICE_NOT_CONNECTED);
        return result(c, STATUS_SUCCESS);
    case 10:
    case 38:
    case 45:
        return result(c, STATUS_SUCCESS);
    case 13:
        return result(c, STATUS_DEVICE_NOT_READY);
    case 1: case 2: case 3: case 5: case 8: case 14: case 15: case 18: case 19: case 20: case 21: case 24: case 25:
    case 27: case 30: case 33: case 34: case 35: case 36: case 37: case 40: case 41: case 43: case 44: case 46:
    case 47:
        return result(c, STATUS_DEVICE_NOT_CONNECTED);
    default:
        return result(c, STATUS_INVALID_DEVICE_REQUEST);
    }
}

// ---- the receiver (RmcDeviceRequest) ----

// (request *): a u16 type and a u16 receiver index (0 to 3). An index past
// them is an invalid parameter; type 0 (a query of the receiver's presence)
// succeeds and zeroes the request's second word; any other type needs a
// receiver, and none is attached.
XF_EXPORT(xboxkrnl, RmcDeviceRequest) {
    uint32_t request = arg(k, c, 0);
    uint16_t type = k.load16(request), index = k.load16(request + 2);
    if (index >= 4) return result(c, STATUS_INVALID_PARAMETER);
    if (type == 0) {
        k.store32(request + 4, 0);
        return result(c, STATUS_SUCCESS);
    }
    result(c, STATUS_DEVICE_NOT_CONNECTED);
}

// ---- the input driver's force feedback (XInputdFF*) ----

namespace {

// The handle's top nibble must be 1 (the first class of device); its low
// nibble is the port. The console's driver has no force-feedback device on
// any port: a port of the four it knows answers as a device that takes no such
// request, one past them as not connected. The cancel of I/O answers the first
// for every port.
void force_feedback(ppc::Context &c, uint32_t handle, bool cancel) {
    if ((handle >> 28) != 1) return result(c, STATUS_UNSUCCESSFUL);
    uint32_t port = handle & 0xF;
    result(c, cancel || port < 4 ? STATUS_INVALID_DEVICE_REQUEST : STATUS_DEVICE_NOT_CONNECTED);
}

} // namespace

XF_EXPORT(xboxkrnl, XInputdFFGetDeviceInfo) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFSetEffect) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFUpdateEffect) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFEffectOperation) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFDeviceControl) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFSetDeviceGain) { force_feedback(c, arg(k, c, 0), false); }
XF_EXPORT(xboxkrnl, XInputdFFCancelIo) { force_feedback(c, arg(k, c, 0), true); }
XF_EXPORT(xboxkrnl, XInputdFFSetRumble) { force_feedback(c, arg(k, c, 0), false); }

// ---- event trace producers (Etx*) ----

namespace {

// The console keeps up to 128 producers in a table of its own, each slot
// holding the producer's id and block. The block is the caller's: a size, the
// flags, the events' table and count, the id, and the slot's index in the
// top ten bits of the word at +0x18.
constexpr uint32_t ETX_SLOTS = 0x80, ETX_BLOCK_MIN = 0x40;
constexpr uint32_t ETX_FLAGS_KEEP_EVENTS = 0x20000, ETX_EVENT_SIZE = 0x2C;

struct EtxProducers {
    std::mutex mutex;
    uint32_t id[ETX_SLOTS] = {};
    uint32_t count = 0, next = 0;
};

} // namespace

// (id *, events, event_count, flags, size, block): the block is zeroed first,
// its size then checked (0x40 at least, or invalid parameter); it takes the
// size, the id, the flags' high half, and (unless the flags ask to keep the
// caller's) the events' table with its first eight bytes of each entry's
// state at +0x1C zeroed; the producer is given a free slot of the 128 (none
// left: STATUS_NO_MORE_ENTRIES, the block zeroed again). No session is open,
// so no event is enabled. The flag the console sets from the processor's mode
// is not set.
XF_EXPORT(xboxkrnl, EtxProducerRegister) {
    uint32_t id = arg(k, c, 0), events = arg(k, c, 1), count = arg(k, c, 2), flags = arg(k, c, 3),
             size = arg(k, c, 4), block = arg(k, c, 5);
    std::memset(k.host(block), 0, size);
    if (size < ETX_BLOCK_MIN) return result(c, STATUS_INVALID_PARAMETER);
    k.store32(block + 0, size);
    k.store32(block + 0x1C, id);
    k.store32(block + 4, flags & 0xFFFF0000);
    if (!(flags & ETX_FLAGS_KEEP_EVENTS)) {
        for (uint32_t i = 0; i < count; i++) std::memset(k.host(events + 0x1C + ETX_EVENT_SIZE * i), 0, 8);
        k.store32(block + 0x10, events);
        k.store32(block + 0x14, count);
    }
    EtxProducers &p = k.state<EtxProducers>();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.count >= ETX_SLOTS) {
        std::memset(k.host(block), 0, size);
        return result(c, STATUS_NO_MORE_ENTRIES);
    }
    uint32_t slot = p.next;
    while (p.id[slot]) slot = (slot + 1) % ETX_SLOTS;
    p.id[slot] = id;
    p.count++;
    p.next = (slot + 1) % ETX_SLOTS;
    k.store32(block + 0x18, k.load32(block + 0x18) | slot << 22);
    result(c, STATUS_SUCCESS);
}

// (block): the slot is given back and the block zeroed; a block under 0x40
// bytes is an invalid parameter
XF_EXPORT(xboxkrnl, EtxProducerUnregister) {
    uint32_t block = arg(k, c, 0), size = k.load32(block);
    if (size < ETX_BLOCK_MIN) return result(c, STATUS_INVALID_PARAMETER);
    uint32_t slot = k.load32(block + 0x18) >> 22;
    {
        EtxProducers &p = k.state<EtxProducers>();
        std::lock_guard<std::mutex> lock(p.mutex);
        if (slot < ETX_SLOTS && p.id[slot] && p.id[slot] == k.load32(block + 0x1C)) {
            p.id[slot] = 0;
            p.count--;
        }
    }
    std::memset(k.host(block), 0, size);
    result(c, STATUS_SUCCESS);
}

// (block, event_index, ..., a variable list): an event is written to a buffer
// a session holds, and none does: the event's drop count (at +0x20 of its
// entry) is bumped and the answer is STATUS_LOG_FILE_FULL. A block under 0x40
// bytes is an invalid parameter.
XF_EXPORT(xboxkrnl, EtxProducerLog) {
    uint32_t block = arg(k, c, 0), index = arg(k, c, 1);
    if (k.load32(block) < ETX_BLOCK_MIN) return result(c, STATUS_INVALID_PARAMETER);
    if (uint32_t events = k.load32(block + 0x10)) {
        uint32_t drops = events + index * ETX_EVENT_SIZE + 0x20;
        k.store32(drops, k.load32(drops) + 1);
    }
    result(c, STATUS_LOG_FILE_FULL);
}

// ---- the cabinet library's LZX decompression (LDI*) ----

namespace {

constexpr uint32_t LDI_MAGIC = 0x4349444C; // 'CIDL', at the start of a context
constexpr uint32_t LDI_OK = 0, LDI_NOT_ENOUGH_MEMORY = 1, LDI_BAD_PARAMETERS = 2, LDI_BUFFER_OVERFLOW = 3,
                   LDI_FAILED = 4;
constexpr uint32_t LDI_BLOCK_EXTRA = 0x1800; // what a source buffer holds beyond a block

struct LdiContexts {
    std::mutex mutex;
    std::map<uint32_t, std::unique_ptr<LzxStream>> streams;
};

} // namespace

// (block_max *, config *, alloc, free, buffer, source_min *, context *): the
// context is made in the caller's buffer, or in the system heap when the
// caller gave an allocator (which is not called). The configuration's first
// word is the window in bytes, 0x8000 to 0x2000000 and a power of two (0 is
// taken and decodes nothing); its second is kept at +8. The context starts
// with 'CIDL' and the block's maximum; the source buffer's minimum is the
// block plus 0x1800. A window the decoder cannot have is "not enough memory".
XF_EXPORT(xboxkrnl, LDICreateDecompression) {
    uint32_t block_max = arg(k, c, 0), config = arg(k, c, 1), alloc = arg(k, c, 2), user = arg(k, c, 4),
             source_min = arg(k, c, 5), out = arg(k, c, 6);
    uint32_t block = k.load32(block_max);
    k.store32(source_min, block + LDI_BLOCK_EXTRA);
    if (!out) return result(c, LDI_OK);
    k.store32(out, 0);
    uint32_t window = k.load32(config);
    bool valid = window == 0 || (window >= 0x8000 && window <= 0x2000000 && !(window & (window - 1)));
    uint32_t context = alloc ? k.memory().system_alloc(0x20) : user;
    if (!context) return result(c, LDI_NOT_ENOUGH_MEMORY);
    k.store32(context + 0, LDI_MAGIC);
    k.store32(context + 4, block);
    k.store32(context + 8, k.load32(config + 4));
    auto stream = std::make_unique<LzxStream>();
    std::string error;
    if (!valid || (window && !stream->start(window, error))) {
        if (alloc) k.memory().system_free(context);
        return result(c, LDI_NOT_ENOUGH_MEMORY);
    }
    {
        LdiContexts &l = k.state<LdiContexts>();
        std::lock_guard<std::mutex> lock(l.mutex);
        l.streams[context] = std::move(stream);
    }
    k.store32(out, context);
    result(c, LDI_OK);
}

// (context, source, source_size, destination, size *): decodes one frame. The
// size asked for may not pass the context's block maximum (3); a frame the
// decoder refuses is 4; the size is written back as what was decoded.
XF_EXPORT(xboxkrnl, LDIDecompress) {
    uint32_t context = arg(k, c, 0), source = arg(k, c, 1), source_size = arg(k, c, 2), destination = arg(k, c, 3),
             size_ptr = arg(k, c, 4);
    if (k.load32(context) != LDI_MAGIC) return result(c, LDI_BAD_PARAMETERS);
    uint32_t size = k.load32(size_ptr);
    if (size > k.load32(context + 4)) return result(c, LDI_BUFFER_OVERFLOW);
    LdiContexts &l = k.state<LdiContexts>();
    std::lock_guard<std::mutex> lock(l.mutex);
    auto it = l.streams.find(context);
    std::string error;
    if (it == l.streams.end() ||
        !it->second->frame({k.host(source), source_size}, {k.host(destination), size}, error))
        return result(c, LDI_FAILED);
    result(c, LDI_OK);
}

// (context): the context's magic is cleared and its decoder released
XF_EXPORT(xboxkrnl, LDIDestroyDecompression) {
    uint32_t context = arg(k, c, 0);
    if (k.load32(context) != LDI_MAGIC) return result(c, LDI_BAD_PARAMETERS);
    k.store32(context, 0);
    LdiContexts &l = k.state<LdiContexts>();
    std::lock_guard<std::mutex> lock(l.mutex);
    l.streams.erase(context);
    result(c, LDI_OK);
}

} // namespace xf::kernel
