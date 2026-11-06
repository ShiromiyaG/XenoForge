// The exports ported from Xenia's inventory in one go (docs/plan.md,
// "Appendix: Xenia's exports missing here"): the read-write locks and the
// settings' writes, the Ke and Nt event, timer, APC, DPC and thread calls,
// the symbolic link objects, the Ob lookups, the HAL and its SMC, the
// keyboard, the camera, Vd queries, the Rtl checksum, stack limits and image
// headers, the wide printf family, and the debug monitor (xbdm). Each called
// as a game calls it: on the test's own thread through the harness, and
// those that wait or suspend on guest threads whose code is host functions
// in the generated-code table (as tests/kernel_threads_test.cpp).

#include "kernel_harness.h"

#include "objects.h"
#include "xam.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

KernelHarness *H = nullptr;
Kernel *K = nullptr;
uint32_t scratch = 0; // guest memory the guest threads share, 0x10000 bytes

// ---- exports called from guest threads: through thunks, so each call is a
// safe point as the generated code's are ----

std::mutex thunks_mutex;
std::map<std::string, uint32_t> thunks;
uint32_t thunk_page = 0, thunk_next = 0;

uint32_t ex(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    uint32_t thunk;
    {
        std::lock_guard<std::mutex> l(thunks_mutex);
        std::string key = std::string(library) + "!" + name;
        auto it = thunks.find(key);
        if (it == thunks.end()) {
            thunk = thunk_page + 8 * thunk_next++;
            K->add_thunk(thunk, library, name);
            thunks[key] = thunk;
        } else {
            thunk = it->second;
        }
    }
    int n = 3;
    for (uint64_t a : args) c.r[n++] = a;
    K->call(c, thunk);
    return uint32_t(c.r[3]);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args = {}) {
    return ex(c, "xboxkrnl", name, args);
}

// a relative timeout in guest memory (a slot per caller)
uint32_t timeout_ms(uint32_t slot, int64_t ms) {
    uint32_t p = scratch + 0xF000 + 8 * slot;
    K->store64(p, uint64_t(-ms * 10'000));
    return p;
}

// polls (the host's sleep) until the condition holds, up to 5 s
bool wait_until(const std::function<bool()> &condition) {
    for (int i = 0; i < 5000; i++) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

Ref<Thread> thread_of(uint32_t handle) { return objects(*K).lookup<Thread>(handle); }

bool blocked(uint32_t handle, Thread::Blocked how, bool alertable = false) {
    Ref<Thread> t = thread_of(handle);
    std::lock_guard<std::mutex> l(objects(*K).lock);
    return t && t->blocked == how && (!alertable || t->alertable);
}

uint32_t create_thread(ppc::Context &c, uint32_t start, uint32_t context, uint32_t slot) {
    uint32_t handle_ptr = scratch + 0xE000 + 8 * slot, id_ptr = handle_ptr + 4;
    CHECK(krnl(c, "ExCreateThread", {handle_ptr, 0, id_ptr, 0, start, context, 0}) == X_STATUS_SUCCESS);
    return K->load32(handle_ptr);
}

void join(ppc::Context &c, uint32_t handle) {
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {handle, 0, 0, timeout_ms(30, 5000)}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtClose", {handle}) == X_STATUS_SUCCESS);
}

// ---- the harness's calls, on the test's own thread ----

uint32_t ansi(const char *s) {
    uint32_t a = H->alloc(8), n = uint32_t(std::strlen(s));
    K->store16(a, uint16_t(n));
    K->store16(a + 2, uint16_t(n + 1));
    K->store32(a + 4, H->string(s));
    return a;
}

uint32_t attributes(const char *name, uint32_t bits = 0) {
    uint32_t a = H->alloc(12);
    K->store32(a, 0);
    K->store32(a + 4, name ? ansi(name) : 0);
    K->store32(a + 8, bits);
    return a;
}

std::u16string read_wide(uint32_t address) {
    std::u16string s;
    for (uint16_t ch; (ch = K->load16(address)) != 0; address += 2) s.push_back(char16_t(ch));
    return s;
}

// ---- Rtl ----

void rtl(KernelHarness &h) {
    // CRC-32 (ISO-HDLC): the standard check value, continued from a seed
    uint32_t text = h.string("123456789");
    CHECK(h.krnl("RtlComputeCrc32", {0, text, 9}) == 0xCBF43926);
    uint32_t first = h.krnl("RtlComputeCrc32", {0, text, 4});
    CHECK(h.krnl("RtlComputeCrc32", {first, text + 4, 5}) == 0xCBF43926);
    CHECK(h.krnl("RtlComputeCrc32", {0x1234, text, 0}) == 0x1234);

    // a PE image: MZ, the NT headers at 0x80, one section (virtual 0x2000,
    // 0x1000 bytes at 0x800 in the file), the import directory in it, the
    // export directory in the headers
    uint32_t base = h.alloc(0x3000);
    std::memset(K->host(base), 0, 0x3000);
    auto le16 = [&](uint32_t a, uint16_t v) { K->host(a)[0] = uint8_t(v), K->host(a)[1] = uint8_t(v >> 8); };
    auto le32 = [&](uint32_t a, uint32_t v) { le16(a, uint16_t(v)), le16(a + 2, uint16_t(v >> 16)); };
    le16(base, 0x5A4D);
    le32(base + 0x3C, 0x80);
    uint32_t nt = base + 0x80, optional = nt + 0x18;
    le32(nt, 0x4550);
    le16(nt + 6, 1);       // sections
    le16(nt + 0x14, 0xE0); // optional header's size
    le16(optional, 0x10B);
    le32(optional + 0x3C, 0x400);                                        // SizeOfHeaders
    le32(optional + 0x5C, 16);                                           // NumberOfRvaAndSizes
    le32(optional + 0x60 + 0, 0x100), le32(optional + 0x60 + 4, 0x20);   // exports
    le32(optional + 0x60 + 8, 0x2010), le32(optional + 0x60 + 12, 0x40); // imports
    uint32_t section = optional + 0xE0;
    le32(section + 0xC, 0x2000), le32(section + 0x10, 0x1000), le32(section + 0x14, 0x800);
    CHECK(h.krnl("RtlImageNtHeader", {base}) == nt);
    CHECK(h.krnl("RtlImageNtHeader", {0}) == 0);
    CHECK(h.krnl("RtlImageNtHeader", {base + 0x10}) == 0);
    uint32_t size = h.alloc(4);
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 1, 1, size}) == base + 0x2010 && K->load32(size) == 0x40);
    // as a file: through the section, at its offset in the file
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 0, 1, size}) == base + 0x810);
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base | 1, 1, 1, size}) == base + 0x810);
    // in the headers: as mapped either way
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 0, 0, size}) == base + 0x100 && K->load32(size) == 0x20);
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 1, 2, size}) == 0);  // no such directory
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 1, 16, size}) == 0); // past the count
    le32(optional + 0x60 + 8, 0x3010);                                       // in no section
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 0, 1, size}) == 0);
    le16(optional, 0x20B);
    CHECK(h.krnl("RtlImageDirectoryEntryToData", {base, 1, 0, size}) == 0);

    // the stack's limits, from the PCR (its alternative stack when it says)
    uint32_t pcr = uint32_t(h.c->r[13]), low = h.alloc(4), high = h.alloc(4);
    K->store32(pcr + kpcr::STACK_BASE, 0x70100000), K->store32(pcr + kpcr::STACK_END, 0x700F0000);
    K->store32(pcr + kpcr::ALT_STACK_BASE, 0x70200000), K->store32(pcr + kpcr::ALT_STACK_END, 0x701F0000);
    K->store32(pcr + kpcr::USE_ALTERNATIVE_STACK, 0);
    h.krnl("RtlGetStackLimits", {low, high});
    CHECK(K->load32(low) == 0x700F0000 && K->load32(high) == 0x70100000);
    K->store32(pcr + kpcr::USE_ALTERNATIVE_STACK, 1);
    h.krnl("RtlGetStackLimits", {low, high});
    CHECK(K->load32(low) == 0x701F0000 && K->load32(high) == 0x70200000);
    K->store32(pcr + kpcr::USE_ALTERNATIVE_STACK, 0);

    // an assertion: logged, and the code goes on
    h.krnl("RtlRip", {h.string("a.cpp(1)"), 0});
}

// ---- the wide printf family ----

void wide_printf(KernelHarness &h) {
    uint32_t buffer = h.alloc(0x200);
    // in a wide format, %s is a wide string and %S a narrow one
    CHECK(h.krnl("swprintf", {buffer, h.wide(u"%d %s %S %c%%"), 42, h.wide(u"ab"), h.string("cd"), 'x'}) == 11);
    CHECK(read_wide(buffer) == u"42 ab cd x%");
    CHECK(h.krnl("swprintf", {buffer, h.wide(u"%hs|%ls|%5.2f|%-4x|"), h.string("n"), h.wide(u"w"),
                              std::bit_cast<uint64_t>(3.14159), 0xAB}) == 15);
    CHECK(read_wide(buffer) == u"n|w| 3.14|ab  |");
    CHECK(h.krnl("swprintf", {0, h.wide(u"x")}) == 0xFFFFFFFF);

    // _snwprintf: what fits, -1 and no terminator when it does not
    std::memset(K->host(buffer), 0xEE, 0x40);
    CHECK(h.krnl("_snwprintf", {buffer, 4, h.wide(u"hello")}) == 0xFFFFFFFF);
    CHECK(K->load16(buffer) == 'h' && K->load16(buffer + 6) == 'l' && K->load16(buffer + 8) == 0xEEEE);
    CHECK(h.krnl("_snwprintf", {buffer, 5, h.wide(u"%s"), h.wide(u"hello")}) == 5);
    CHECK(K->load16(buffer + 8) == 'o' && K->load16(buffer + 10) == 0xEEEE);
    CHECK(h.krnl("_snwprintf", {buffer, 6, h.wide(u"hi %u"), 7}) == 4 && read_wide(buffer) == u"hi 7");
    CHECK(h.krnl("_snwprintf", {buffer, 0, h.wide(u"x")}) == 0xFFFFFFFF);

    // the va_list forms: 8-byte slots
    uint32_t args = h.alloc(32);
    K->store64(args, 0xFFFFFFFF), K->store64(args + 8, h.wide(u"zz")), K->store64(args + 16, 0x123456789ull);
    CHECK(h.krnl("vswprintf", {buffer, h.wide(u"%d/%s/%I64x"), args}) == 15);
    CHECK(read_wide(buffer) == u"-1/zz/123456789");
    CHECK(h.krnl("_vscwprintf", {h.wide(u"%d/%s/%I64x"), args}) == 15);
    CHECK(h.krnl("_vscwprintf", {0, args}) == 0xFFFFFFFF);
    // _vsnwprintf: the whole count even when it does not fit (Xenia's)
    std::memset(K->host(buffer), 0xEE, 0x40);
    CHECK(h.krnl("_vsnwprintf", {buffer, 3, h.wide(u"%d/%s"), args}) == 5);
    CHECK(K->load16(buffer) == '-' && K->load16(buffer + 4) == '/' && K->load16(buffer + 6) == 0xEEEE);
    CHECK(h.krnl("_vsnwprintf", {buffer, 8, h.wide(u"%d/%s"), args}) == 5 && read_wide(buffer) == u"-1/zz");
}

// ---- the console's settings ----

void settings(KernelHarness &h) {
    uint32_t buffer = h.alloc(16), size = h.alloc(2);
    auto get32 = [&](uint16_t category, uint16_t id) {
        CHECK(h.krnl("ExGetXConfigSetting", {category, id, buffer, 4, size}) == 0);
        return K->load32(buffer);
    };
    // the video flags: 4:3 by default, widescreen once written
    CHECK(get32(3, 0x0A) == 0);
    K->store32(buffer, 0x00010000);
    CHECK(h.krnl("ExSetXConfigSetting", {3, 0x0A, buffer, 4}) == 0);
    K->store32(buffer, 0);
    CHECK(get32(3, 0x0A) == 0x00010000);
    // an unknown setting: nothing written, success
    CHECK(h.krnl("ExSetXConfigSetting", {3, 0x7F, buffer, 4}) == 0);
    // the retail flags: masked, then or'ed
    CHECK(get32(3, 0x0C) == 0x40);
    CHECK(h.krnl("ExReadModifyWriteXConfigSettingUlong", {3, 0x0C, 0xFFFFFFBF, 0x3}) == 0);
    CHECK(get32(3, 0x0C) == 0x3);
    CHECK(h.krnl("ExReadModifyWriteXConfigSettingUlong", {3, 0x0C, 0xFFFFFFFF, 0x100}) == 0);
    CHECK(get32(3, 0x0C) == 0x103);
    // a setting of one byte (the country, 103): read as 0, its first byte of
    // the big-endian result written (Xenia's)
    CHECK(h.krnl("ExReadModifyWriteXConfigSettingUlong", {3, 0x0E, 0xFFFFFFFF, 0x05}) == 0);
    CHECK(h.krnl("ExGetXConfigSetting", {3, 0x0E, buffer, 4, size}) == 0 && K->load16(size) == 1);
    CHECK(K->host(buffer)[0] == 0);
}

// ---- the HAL, the keyboard, the camera, the Vd queries, Fsc ----

void hal(KernelHarness &h) {
    CHECK(h.krnl("HalGetCurrentAVPack") == 8);
    uint32_t message = h.alloc(16), response = h.alloc(16);
    auto smc = [&](uint8_t command, std::initializer_list<uint8_t> data = {}) {
        std::memset(K->host(message), 0, 16);
        std::memset(K->host(response), 0xAA, 16);
        K->host(message)[0] = command;
        size_t n = 1;
        for (uint8_t b : data) K->host(message)[n++] = b;
        h.krnl("HalSendSMCMessage", {message, response});
        return std::vector<uint8_t>(K->host(response), K->host(response) + 16);
    };
    std::vector<uint8_t> r = smc(0x12); // the version
    CHECK(r[0] == 0x12 && r[1] == 65 && r[2] == 2 && r[3] == 1 && r[4] == 0xAA);
    r = smc(0x0A); // the tray: open at start
    CHECK(r[0] == 0x0A && r[1] == 0 && smc_tray_state(*K) == 0);
    r = smc(0x0F); // the AV pack: HDMI
    CHECK(r[0] == 0x0F && r[1] == 0x1F);
    r = smc(0x07); // the temperatures, 8.8 low byte first: 69.6 is 0x4599
    CHECK(r[0] == 0x07 && r[1] == 0x99 && r[2] == 0x45 && r[5] == 0x99 && r[6] == 0x45 && r[9] == 0xAA);
    r = smc(0x16);
    CHECK(r[0] == 0x16 && r[1] == 0x0F);
    r = smc(0x95, {0x01}); // the IR address set: answers the address unchanged
    CHECK(r[0] == 0x95 && r[1] == 0x0F);
    r = smc(0x17);
    CHECK(r[0] == 0x17 && r[1] == 0);
    r = smc(0x89, {0x90}); // a fan's speed: no answer
    CHECK(r[0] == 0xAA);
    r = smc(0x55); // unknown: nothing
    CHECK(r[0] == 0xAA);
    // the tray: closed, opened by command, closed again; queries see it
    h.krnl("HalOpenCloseODDTray", {0});
    CHECK(smc_tray_state(*K) == 2 && smc(0x0A)[1] == 2);
    smc(0x8B, {0x10}); // (0x10 & 0xF) % 5: open
    CHECK(smc_tray_state(*K) == 0);
    h.krnl("HalOpenCloseODDTray", {0});
    CHECK(smc_tray_state(*K) == 2);
    h.krnl("HalOpenCloseODDTray", {1});
    CHECK(smc_tray_state(*K) == 0);
    h.krnl("HalSendSMCMessage", {0, response}); // no message: nothing
    K->host(message)[0] = 0x12;
    h.krnl("HalSendSMCMessage", {message, 0}); // no response: nothing written

    CHECK(h.krnl("HidReadKeys", {0, h.alloc(8), h.alloc(20)}) == 0xC000009D);
    CHECK(h.krnl("XUsbcamCreate", {h.alloc(16), 0x4B000, h.alloc(4)}) == 0);
    CHECK(h.krnl("XUsbcamGetState") == 0);
    CHECK(h.krnl("FscGetCacheElementCount", {0}) == 0);
    CHECK(h.krnl("VdGetGraphicsAsicID") == 0x11);
}

// the real mode is the mode (the video driver's first call starts the GPU's
// threads: last)
void video_modes(KernelHarness &h) {
    uint32_t mode = h.alloc(0x30), real = h.alloc(0x30);
    std::memset(K->host(mode), 0xCC, 0x30), std::memset(K->host(real), 0x33, 0x30);
    h.krnl("VdQueryVideoMode", {mode});
    h.krnl("VdQueryRealVideoMode", {real});
    CHECK(std::memcmp(K->host(mode), K->host(real), 0x30) == 0 && K->load32(real) == 1280);
}

// ---- symbolic links and the Ob lookups ----

void links_and_names(KernelHarness &h) {
    uint32_t out = h.alloc(4);
    CHECK(h.krnl("ObCreateSymbolicLink", {ansi("\\??\\inv:"), ansi("\\Device\\Inventory")}) == 0);
    CHECK(h.krnl("NtOpenSymbolicLinkObject", {out, attributes("\\??\\inv:")}) == 0);
    uint32_t link = K->load32(out);
    uint32_t target = h.alloc(8), text = h.alloc(64);
    K->store16(target, 0), K->store16(target + 2, 64), K->store32(target + 4, text);
    CHECK(h.krnl("NtQuerySymbolicLinkObject", {link, target}) == 0);
    CHECK(K->load16(target) == 17 && std::string(reinterpret_cast<char *>(K->host(text)), 17) == "\\Device\\Inventory");
    K->store16(target + 2, 4); // the buffer's size: what fits, unterminated
    CHECK(h.krnl("NtQuerySymbolicLinkObject", {link, target}) == 0 && K->load16(target) == 4);
    // a path under the link finds it too (FindSymbolicLink)
    CHECK(h.krnl("NtOpenSymbolicLinkObject", {out, attributes("inv:\\sub")}) == 0);
    CHECK(h.krnl("NtClose", {K->load32(out)}) == 0);
    CHECK(h.krnl("NtOpenSymbolicLinkObject", {out, attributes("\\??\\nolink:")}) == X_STATUS_NO_SUCH_FILE);
    CHECK(h.krnl("NtOpenSymbolicLinkObject", {out, attributes("bad|name:")}) == X_STATUS_OBJECT_NAME_INVALID);
    CHECK(h.krnl("NtOpenSymbolicLinkObject", {out, 0}) == X_STATUS_INVALID_PARAMETER);
    CHECK(h.krnl("NtClose", {link}) == 0);
    CHECK(h.krnl("NtQuerySymbolicLinkObject", {link, target}) == X_STATUS_NO_SUCH_FILE);

    // a named event: opened by name (another reference on its handle), and
    // referenced by name as by handle
    uint32_t handle_ptr = h.alloc(4);
    CHECK(h.krnl("NtCreateEvent", {handle_ptr, attributes("InventoryEvent"), 0, 0}) == 0);
    uint32_t event = K->load32(handle_ptr);
    CHECK(h.krnl("ObOpenObjectByName", {attributes("inventoryevent"), 0, 0, out}) == 0 && K->load32(out) == event);
    CHECK(h.krnl("ObOpenObjectByName", {attributes("nothing"), 0, 0, out}) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(h.krnl("ObOpenObjectByName", {0, 0, 0, out}) == X_STATUS_INVALID_PARAMETER);
    uint32_t body = h.alloc(4);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi("InventoryEvent"), 0, K->variable("ExEventObjectType"), 0, body}) ==
          0);
    CHECK(K->load32(body) != 0 && K->host(K->load32(body))[0] == 0);
    h.krnl("ObDereferenceObject", {K->load32(body)});
    CHECK(h.krnl("ObReferenceObjectByName", {ansi("InventoryEvent"), 0, K->variable("ExMutantObjectType"), 0, body}) ==
          X_STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi("Missing"), 0, 0, 0, body}) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    h.krnl("ObReferenceObjectByName", {ansi("InventoryEvent"), 0, 0, 0, body});
    h.krnl("ObDereferenceObject", {K->load32(body)});
    // the references balance: the creation's and the open's handles close,
    // then the handle is gone
    CHECK(h.krnl("NtClose", {event}) == 0);
    CHECK(h.krnl("NtClose", {event}) == 0);
    CHECK(h.krnl("NtClose", {event}) == X_STATUS_INVALID_HANDLE);

    // ObCreateObject: from the type's pool, after its header (no thread:
    // the system's pool, no title flag; the attributes' 0x1000 the title's)
    uint32_t type = K->variable("ExEventObjectType");
    CHECK(h.krnl("ObCreateObject", {type, 0, 0x10, out}) == 0);
    uint32_t object = K->load32(out), header = object - 0x10;
    CHECK(object && K->load32(header) == 1 && K->load32(header + 4) == 0 && K->load32(header + 8) == type);
    CHECK(K->load16(header + 0xC) == 0);
    CHECK(h.krnl("ObCreateObject", {type, attributes(nullptr, 0x1000), 0x10, out}) == 0);
    CHECK(K->load16(K->load32(out) - 0x10 + 0xC) == 0x10);
    // named: the name info before the header, the name's last element after
    // the body
    CHECK(h.krnl("ObCreateObject", {type, attributes("\\Base\\Thing", 0x1000), 0x6, out}) == 0);
    object = K->load32(out), header = object - 0x10;
    uint32_t info = header - 0x10;
    CHECK(K->load16(header + 0xC) == 0x11 && K->load16(info + 8) == 5 && K->load32(info + 0xC) == object + 8);
    CHECK(std::string(reinterpret_cast<char *>(K->host(object + 8)), 5) == "Thing");
    CHECK(h.krnl("ObCreateObject", {type, attributes("\\Base\\\\Thing"), 4, out}) == 0xC0000033 && !K->load32(out));
    CHECK(h.krnl("ObCreateObject", {type, attributes("\\Base\\"), 4, out}) == 0xC0000033);
}

// ---- SLists, IRQL ----

void small_ones(KernelHarness &h) {
    uint32_t list = h.alloc(8), e1 = h.alloc(8), e2 = h.alloc(8);
    K->store64(list, 0);
    CHECK(h.krnl("InterlockedPushEntrySList", {list, e1}) == 0);
    CHECK(h.krnl("InterlockedPushEntrySList", {list, e2}) == e1);
    CHECK(K->load32(list) == e2 && K->load16(list + 4) == 2 && K->load16(list + 6) == 2 && K->load32(e2) == e1);
    CHECK(h.krnl("InterlockedPopEntrySList", {list}) == e2);
    CHECK(h.krnl("InterlockedPopEntrySList", {list}) == e1);
    CHECK(h.krnl("InterlockedPopEntrySList", {list}) == 0);

    uint32_t pcr = uint32_t(h.c->r[13]);
    K->host(pcr)[kpcr::CURRENT_IRQL] = 0;
    CHECK(h.krnl("KfRaiseIrql", {2}) == 0 && K->host(pcr)[kpcr::CURRENT_IRQL] == 2);
    CHECK(h.krnl("KfRaiseIrql", {31}) == 2);
    h.krnl("KfLowerIrql", {0});
    CHECK(K->host(pcr)[kpcr::CURRENT_IRQL] == 0);

    // a DPC runs as it is inserted: none is left to remove
    uint32_t dpc = h.alloc(0x1C);
    h.krnl("KeInitializeDpc", {dpc, 0, 0});
    CHECK(h.krnl("KeInsertQueueDpc", {dpc, 1, 2}) == 1);
    CHECK(h.krnl("KeRemoveQueueDpc", {dpc}) == 0);
}

// ---- the debug monitor, on the test's own thread ----

void xbdm(KernelHarness &h) {
    constexpr uint32_t OK = 0x02DA0000, FAIL = 0x82DA0000, INVALIDARG = 0x80070057;
    auto dm = [&](const char *name, std::initializer_list<uint64_t> args = {}) { return h.call32("xbdm", name, args); };
    uint32_t pool = dm("DmAllocatePool", {64});
    CHECK(pool != 0 && dm("DmFreePool", {pool}) == OK);

    uint32_t name = h.alloc(64), size = h.alloc(4);
    K->store32(size, 5);
    CHECK(dm("DmGetXboxName", {name, size}) == 0x82DA0105);
    K->store32(size, 64);
    CHECK(dm("DmGetXboxName", {name, size}) == OK && h.read(name) == "Xbox360Name");
    CHECK(dm("DmGetXboxName", {0, size}) == INVALIDARG);
    CHECK(dm("DmGetConsoleType", {size}) == OK && K->load32(size) == 0xFFFFFFFF);
    CHECK(dm("DmIsDebuggerPresent") == 0);
    CHECK(dm("DmGetConsoleDebugMemoryStatus", {size}) == OK && K->load32(size) == 0);

    uint32_t info = h.alloc(32);
    std::memset(K->host(info), 0x77, 32);
    CHECK(dm("DmGetSystemInfo", {info}) == OK);
    CHECK(K->load16(info + 4) == 2 && K->load16(info + 6) == 0 && K->load16(info + 8) == 1888);
    CHECK(K->load16(info + 0xC) == 2 && K->load16(info + 0x10) == 1888 && K->load16(info + 0x12) == 0);
    CHECK(K->load32(info) == 0x77777777); // the size, the caller's

    uint32_t big = h.alloc(300);
    std::memset(K->host(big), 0x55, 300);
    CHECK(dm("DmGetXbeInfo", {0, big}) == OK && K->host(big)[0] == 0 && K->host(big)[275] == 0);
    CHECK(K->host(big)[276] == 0x55);
    CHECK(dm("DmFindPdbSignature", {0x82000000, big}) == 0x57);
    CHECK(dm("DmFindPdbSignature", {0, big}) == INVALIDARG);
    std::memset(K->host(big), 0x55, 300);
    CHECK(dm("DmWalkLoadedModules", {size, big}) == 0x82DA0104 && K->host(big)[291] == 0);
    CHECK(dm("DmCloseLoadedModules", {size}) == OK);

    // memory: into read-write pages only
    uint32_t from = h.string("copied!"), to = h.alloc(16), written = h.alloc(4);
    CHECK(dm("DmGetMemory", {from, 8, to, written}) == OK && h.read(to) == "copied!" && K->load32(written) == 8);
    CHECK(dm("DmSetMemory", {to, 3, h.string("XYZ"), written}) == OK && h.read(to) == "XYZied!");
    CHECK(dm("DmSetMemory", {to, 0, from, written}) == INVALIDARG);
    uint32_t read_only = 0;
    Heap *heap = K->memory().heap_by_type(false, 4096);
    CHECK(heap->alloc(0x1000, 0x1000, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ, false, &read_only));
    CHECK(dm("DmSetMemory", {read_only, 4, from, written}) == FAIL && K->load32(written) == 0);
    heap->release(read_only);

    // the title's memory statistics, from MmQueryStatistics
    uint32_t stats = h.alloc(48);
    K->store32(stats, 40);
    CHECK(dm("DmQueryTitleMemoryStatistics", {stats}) == INVALIDARG);
    K->store32(stats, 48);
    CHECK(dm("DmQueryTitleMemoryStatistics", {stats}) == OK);
    uint32_t sum = 0;
    for (uint32_t at = 8; at < 0x2C; at += 4) sum += K->load32(stats + at);
    CHECK(K->load32(stats + 4) == sum && K->load32(stats + 8) != 0 && K->load32(stats + 0x14) == 0);

    uint32_t trace = h.alloc(16);
    std::memset(K->host(trace), 0x11, 16);
    CHECK(dm("DmCaptureStackBackTrace", {4, trace}) == OK && K->load32(trace + 12) == 0);
    CHECK(dm("DmCaptureStackBackTrace", {257, trace}) == INVALIDARG);

    CHECK(dm("DmSendNotificationString", {h.string("hi")}) == OK && dm("DmSendNotificationString", {0}) == INVALIDARG);
    CHECK(dm("DmRegisterCommandProcessor", {h.string("cmd"), 0x82000000}) == OK);
    CHECK(dm("DmRegisterCommandProcessorEx", {0, 0x82000000, 0}) == INVALIDARG);
    K->store32(size, 0x1234);
    CHECK(dm("DmOpenNotificationSession", {0, size}) == OK && K->load32(size) == 0);
    CHECK(dm("DmNotify", {size, 0, 0}) == OK && dm("DmNotify", {0, 0, 0}) == INVALIDARG);
    CHECK(dm("DmStartProfiling", {h.string("log"), 0x1000}) == OK && dm("DmStopProfiling") == OK);
    CHECK(dm("DmSetProfilingOptions", {0}) == OK && dm("DmSetDumpMode", {1}) == OK);
    CHECK(dm("DmIsFastCAPEnabled") == FAIL);
    dm("__CAP_Start_Profiling", {0, 0}), dm("__CAP_Enter_Function"), dm("__CAP_Exit_Function"),
        dm("__CAP_End_Profiling");

    // the performance counters' names: none until installed
    CHECK(dm("DmPMCGetCounterName", {1}) == 0);
    CHECK(dm("DmPMCInstallAndStart", {0}) == OK && dm("DmPMCInstallAndStart", {0}) == OK);
    CHECK(h.read(dm("DmPMCGetCounterName", {0})) == "load-hit-stores (S)");
    CHECK(h.read(dm("DmPMCGetCounterName", {1})) == "instructions committed");
    CHECK(h.read(dm("DmPMCGetCounterName", {99})) == "Bad counter number - must be 0-15.");

    // the development kit's drive: none without a storage root, then under it
    CHECK(dm("DmMapDevkitDrive") == FAIL);
    std::random_device random;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("xf_inventory_test_" + std::to_string(random()) + "_" +
                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    CHECK(mount_storage(*K, root, error));
    CHECK(dm("DmMapDevkitDrive") == 0 && dm("DmMapDevkitDrive") == 0);
    CHECK(std::filesystem::is_directory(root / "devkit"));
    CHECK(file_system(*K).resolve("e:\\") != nullptr && file_system(*K).has_link("DEVKIT:"));
}

// ---- guest threads ----

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_WAIT_EVENT = 0x82000010,
    F_WRITER = 0x82000020,
    F_READER = 0x82000030,
    F_STRESS = 0x82000040,
    F_ALERTABLE = 0x82000050,
    F_APC = 0x82000060,
    F_YIELDER = 0x82000070,
};

std::atomic<uint32_t> flag{0}, apc_calls{0}, stop{0}, inside_writers{0}, inside_readers{0}, bad{0};
std::mutex order_mutex;
std::vector<uint32_t> order;
uint32_t event_addr = 0, rw = 0, counter = 0;
uint32_t apc_args[3];

void note(uint32_t what) {
    std::lock_guard<std::mutex> l(order_mutex);
    order.push_back(what);
}

void wait_event(ppc::Context &c) {
    CHECK(krnl(c, "KeWaitForSingleObject", {event_addr, 0, 0, 0, timeout_ms(uint32_t(c.r[3]), 5000)}) ==
          X_STATUS_SUCCESS);
    flag++;
}

// (id): an exclusive hold, noted
void writer(ppc::Context &c) {
    uint32_t id = uint32_t(c.r[3]);
    krnl(c, "ExAcquireReadWriteLockExclusive", {rw});
    note(id);
    krnl(c, "ExReleaseReadWriteLock", {rw});
}

void reader(ppc::Context &c) {
    uint32_t id = uint32_t(c.r[3]);
    krnl(c, "ExAcquireReadWriteLockShared", {rw});
    note(id);
    krnl(c, "ExReleaseReadWriteLock", {rw});
}

// (id): writers count up a guest word read and written apart; readers check
// no writer is inside
void stress(ppc::Context &c) {
    uint32_t id = uint32_t(c.r[3]);
    for (int i = 0; i < 300; i++) {
        bool exclusive = (i + id) % 3 == 0;
        if (exclusive) {
            krnl(c, "ExAcquireReadWriteLockExclusive", {rw});
            if (inside_writers.fetch_add(1) != 0 || inside_readers.load() != 0) bad++;
            uint32_t v = K->load32(counter);
            if (i % 16 == 0) krnl(c, "NtYieldExecution");
            K->store32(counter, v + 1);
            inside_writers--;
        } else {
            krnl(c, "ExAcquireReadWriteLockShared", {rw});
            inside_readers++;
            if (inside_writers.load() != 0) bad++;
            if (i % 16 == 0) krnl(c, "NtYieldExecution");
            inside_readers--;
        }
        krnl(c, "ExReleaseReadWriteLock", {rw});
    }
}

void alertable(ppc::Context &c) {
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, timeout_ms(uint32_t(c.r[3]), 5000)}) == X_STATUS_USER_APC);
    flag = 1;
}

// an APC's normal routine: (context, arg1, arg2)
void apc(ppc::Context &c) {
    apc_args[0] = uint32_t(c.r[3]), apc_args[1] = uint32_t(c.r[4]), apc_args[2] = uint32_t(c.r[5]);
    apc_calls++;
}

// kernel calls (safe points) until told to stop
void yielder(ppc::Context &c) {
    while (!stop) {
        krnl(c, "NtYieldExecution");
        flag++;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void events_and_timers(ppc::Context &c) {
    Objects &o = objects(*K);
    // KeInitializeEvent: a notification event, its host object made; again
    // on the same header: the same object kept
    event_addr = scratch + 0x100;
    std::memset(K->host(event_addr), 0xFF, 0x10);
    krnl(c, "KeInitializeEvent", {event_addr, 0, 0});
    CHECK(K->host(event_addr)[0] == 0 && K->load32(event_addr + 4) == 0 && K->host(event_addr)[3] == 0);
    uint32_t out = scratch + 0x200;
    CHECK(krnl(c, "ObOpenObjectByPointer", {event_addr, out}) == X_STATUS_SUCCESS);
    uint32_t handle = K->load32(out);
    krnl(c, "KeInitializeEvent", {event_addr, 0, 1});
    CHECK(krnl(c, "ObOpenObjectByPointer", {event_addr, out}) == X_STATUS_SUCCESS && K->load32(out) == handle);
    CHECK(krnl(c, "NtClose", {handle}) == X_STATUS_SUCCESS);
    // NtQueryEvent: its type and state
    uint32_t query = scratch + 0x210;
    CHECK(krnl(c, "NtQueryEvent", {handle, query}) == X_STATUS_SUCCESS);
    CHECK(K->load32(query) == 0 && K->load32(query + 4) == 1);
    // KePulseEvent: the state before; the waiters go, the event ends reset
    CHECK(krnl(c, "KePulseEvent", {event_addr, 0, 0}) == 1 && K->load32(event_addr + 4) == 0);
    uint32_t a = create_thread(c, F_WAIT_EVENT, 1, 1), b = create_thread(c, F_WAIT_EVENT, 2, 2);
    CHECK(wait_until([&] { return blocked(a, Thread::Blocked::Wait) && blocked(b, Thread::Blocked::Wait); }));
    CHECK(krnl(c, "KePulseEvent", {event_addr, 1, 0}) == 0);
    CHECK(K->load32(event_addr + 4) == 0);
    join(c, a), join(c, b);
    CHECK(flag == 2);
    // a synchronization event (type 1) queried
    krnl(c, "KeInitializeEvent", {event_addr, 1, 0});
    CHECK(krnl(c, "NtQueryEvent", {handle, query}) == X_STATUS_SUCCESS && K->load32(query) == 1);
    CHECK(krnl(c, "NtClose", {handle}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtQueryEvent", {0xF8FF0000, query}) == X_STATUS_INVALID_HANDLE);

    // KeInitializeTimerEx: the header, a wait that times out
    uint32_t timer = scratch + 0x300;
    std::memset(K->host(timer), 0xFF, 0x28);
    krnl(c, "KeInitializeTimerEx", {timer, 1, 0x101});
    CHECK(K->host(timer)[0] == uint8_t(GuestType::TimerSynchronization) && K->host(timer)[1] == 0xFF);
    CHECK(K->host(timer)[2] == 1 && K->host(timer)[3] == 0 && K->load32(timer + 4) == 0);
    CHECK(K->load32(timer + 8) == timer + 8 && K->load64(timer + 0x10) == 0 && K->load32(timer + 0x24) == 0);
    CHECK(K->load32(timer + 0x20) == 0xFFFFFFFF); // the DPC carries through
    CHECK(krnl(c, "KeWaitForSingleObject", {timer, 0, 0, 0, timeout_ms(0, 5)}) == X_STATUS_TIMEOUT);
    krnl(c, "KeInitializeTimerEx", {timer, 0, 0});
    CHECK(K->host(timer)[0] == uint8_t(GuestType::TimerNotification));
    CHECK(krnl(c, "ObOpenObjectByPointer", {timer, out}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtQueryEvent", {K->load32(out), query}) == X_STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(krnl(c, "NtClose", {K->load32(out)}) == X_STATUS_SUCCESS);
    (void)o;
}

void read_write_locks(ppc::Context &c) {
    rw = scratch + 0x400;
    std::memset(K->host(rw), 0xFF, 0x38);
    krnl(c, "ExInitializeReadWriteLock", {rw});
    CHECK(K->load32(rw) == 0xFFFFFFFF && K->load32(rw + 4) == 0 && K->load32(rw + 8) == 0 && K->load32(rw + 12) == 0);
    CHECK(K->host(rw + 0x10)[0] == 1 && K->host(rw + 0x20)[0] == 5 && K->load32(rw + 0x30) == 0x7FFFFFFF);
    CHECK(K->load32(rw + 0x34) == 0);
    // tries: one writer alone; readers together, no writer with them
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockExclusive", {rw}) == 1);
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockExclusive", {rw}) == 0);
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockShared", {rw}) == 0);
    krnl(c, "ExReleaseReadWriteLock", {rw});
    CHECK(K->load32(rw) == 0xFFFFFFFF);
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockShared", {rw}) == 1);
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockShared", {rw}) == 1);
    CHECK(K->load32(rw) == 1 && K->load32(rw + 12) == 2);
    CHECK(krnl(c, "ExTryToAcquireReadWriteLockExclusive", {rw}) == 0);
    krnl(c, "ExAcquireReadWriteLockShared", {rw}); // a third reader enters at once
    CHECK(K->load32(rw + 12) == 3);
    for (int i = 0; i < 3; i++) krnl(c, "ExReleaseReadWriteLock", {rw});
    CHECK(K->load32(rw) == 0xFFFFFFFF && K->load32(rw + 12) == 0);
    // the IRQL is back where it was
    CHECK(K->host(uint32_t(c.r[13]))[kpcr::CURRENT_IRQL] == 0);

    // a writer behind a writer is let in when the first leaves (where
    // Xenia's release never wakes it)
    krnl(c, "ExAcquireReadWriteLockExclusive", {rw});
    order.clear();
    uint32_t w = create_thread(c, F_WRITER, 10, 3);
    CHECK(wait_until([&] { return blocked(w, Thread::Blocked::Wait); }));
    CHECK(K->load32(rw + 4) == 1);
    note(1);
    krnl(c, "ExReleaseReadWriteLock", {rw});
    join(c, w);
    CHECK((order == std::vector<uint32_t>{1, 10}));
    CHECK(K->load32(rw) == 0xFFFFFFFF && K->load32(rw + 4) == 0);

    // a writer and a reader queued behind a writer: the readers first, then
    // the writer when the last reader leaves
    order.clear();
    krnl(c, "ExAcquireReadWriteLockExclusive", {rw});
    w = create_thread(c, F_WRITER, 20, 3);
    CHECK(wait_until([&] { return blocked(w, Thread::Blocked::Wait); }));
    uint32_t r1 = create_thread(c, F_READER, 21, 4);
    CHECK(wait_until([&] { return blocked(r1, Thread::Blocked::Wait); }));
    uint32_t r2 = create_thread(c, F_READER, 22, 5);
    CHECK(wait_until([&] { return blocked(r2, Thread::Blocked::Wait); }));
    CHECK(K->load32(rw + 4) == 1 && K->load32(rw + 8) == 2);
    krnl(c, "ExReleaseReadWriteLock", {rw});
    join(c, w), join(c, r1), join(c, r2);
    CHECK(order.size() == 3 && order[2] == 20);
    CHECK(K->load32(rw) == 0xFFFFFFFF && K->load32(rw + 4) == 0 && K->load32(rw + 8) == 0);

    // four threads, readers and writers mixed
    counter = scratch + 0x500;
    K->store32(counter, 0);
    uint32_t threads[4];
    for (uint32_t i = 0; i < 4; i++) threads[i] = create_thread(c, F_STRESS, i, 6 + i);
    for (uint32_t t : threads) join(c, t);
    CHECK(bad == 0);
    CHECK(K->load32(counter) == 400); // 100 exclusive holds a thread
    CHECK(K->load32(rw) == 0xFFFFFFFF && K->load32(rw + 4) == 0 && K->load32(rw + 8) == 0);
}

void apcs(ppc::Context &c) {
    // NtQueueApcThread: a user APC to an alertable sleeper, which wakes for it
    flag = 0;
    uint32_t t = create_thread(c, F_ALERTABLE, 11, 11);
    CHECK(wait_until([&] { return blocked(t, Thread::Blocked::Wait, true); }));
    CHECK(krnl(c, "NtQueueApcThread", {t, F_APC, 0x1234, 5, 6}) == X_STATUS_SUCCESS);
    join(c, t);
    CHECK(flag == 1 && apc_calls == 1 && apc_args[0] == 0x1234 && apc_args[1] == 5 && apc_args[2] == 6);
    CHECK(krnl(c, "NtQueueApcThread", {0xF8FF0000, F_APC, 0, 0, 0}) == X_STATUS_INVALID_HANDLE);

    // to itself: delivered in its next alertable wait
    CHECK(krnl(c, "NtQueueApcThread", {0xFFFFFFFE, F_APC, 0x99, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(apc_calls == 1);
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, timeout_ms(0, 0)}) == X_STATUS_USER_APC);
    CHECK(apc_calls == 2 && apc_args[0] == 0x99);

    // KeRemoveQueueApc: an APC queued and taken off before it runs
    Thread *self = current_thread();
    uint32_t a = scratch + 0x600;
    std::memset(K->host(a), 0, 0x28);
    krnl(c, "KeInitializeApc", {a, self->kthread(), 0, 0, F_APC, 1, 0x77});
    CHECK(krnl(c, "KeInsertQueueApc", {a, 1, 2, 0}) == 1);
    CHECK(K->host(a)[kapc::ENQUEUED] == 1);
    CHECK(krnl(c, "KeRemoveQueueApc", {a}) == 1 && K->host(a)[kapc::ENQUEUED] == 0);
    CHECK(krnl(c, "KeRemoveQueueApc", {a}) == 0);
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, timeout_ms(0, 1)}) == X_STATUS_SUCCESS);
    CHECK(apc_calls == 2);
    K->store32(a + kapc::THREAD, 0);
    CHECK(krnl(c, "KeRemoveQueueApc", {a}) == 0);
}

void thread_calls(ppc::Context &c) {
    Thread *self = current_thread();
    uint32_t kthread = self->kthread();
    // KeSetPriorityThread: the previous priority back, the new one kept
    uint8_t before = K->host(kthread)[kthread::PRIORITY];
    CHECK(krnl(c, "KeSetPriorityThread", {kthread, 20}) == before);
    CHECK(krnl(c, "KeSetPriorityThread", {kthread, uint32_t(-3)}) == 20 && K->host(kthread)[kthread::PRIORITY] == 0);
    krnl(c, "KeSetPriorityThread", {kthread, before});
    CHECK(krnl(c, "KeSetPriorityThread", {0, 5}) == 0);

    // ObLookupThreadByThreadId: its KTHREAD, referenced
    uint32_t out = scratch + 0x700;
    CHECK(krnl(c, "ObLookupThreadByThreadId", {self->id, out}) == X_STATUS_SUCCESS && K->load32(out) == kthread);
    krnl(c, "ObDereferenceObject", {kthread});
    CHECK(krnl(c, "ObLookupThreadByThreadId", {0xFFFF, out}) == X_STATUS_INVALID_PARAMETER && K->load32(out) == 0);

    // the stack's limits on a guest thread: its own stack
    krnl(c, "RtlGetStackLimits", {out, out + 4});
    CHECK(K->load32(out) == self->stack_limit && K->load32(out + 4) == self->stack_base);

    // KeSuspendThread: the previous count; the thread stops at its next
    // kernel call, and goes on when resumed
    stop = 0, flag = 0;
    uint32_t y = create_thread(c, F_YIELDER, 0, 12);
    Ref<Thread> yt = thread_of(y);
    CHECK(wait_until([&] { return flag.load() > 2; }));
    CHECK(krnl(c, "KeSuspendThread", {yt->kthread()}) == 0);
    CHECK(krnl(c, "KeSuspendThread", {yt->kthread()}) == 1);
    CHECK(K->host(yt->kthread())[kthread::SUSPEND_COUNT] == 2);
    CHECK(wait_until([&] { return blocked(y, Thread::Blocked::Suspended); }));
    uint32_t frozen = flag;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(flag == frozen);
    CHECK(krnl(c, "KeResumeThread", {yt->kthread()}) == 2);
    CHECK(blocked(y, Thread::Blocked::Suspended));
    CHECK(krnl(c, "KeResumeThread", {yt->kthread()}) == 1);
    CHECK(wait_until([&] { return flag.load() > frozen; }));

    // the debug monitor's thread calls
    uint32_t ids = scratch + 0x800, count = scratch + 0x8F0;
    K->store32(count, 1);
    CHECK(ex(c, "xbdm", "DmGetThreadList", {ids, count}) == 0x82DA0105);
    K->store32(count, 32);
    CHECK(ex(c, "xbdm", "DmGetThreadList", {ids, count}) == 0x02DA0000 && K->load32(count) == 2);
    CHECK((K->load32(ids) == self->id && K->load32(ids + 4) == yt->id) ||
          (K->load32(ids) == yt->id && K->load32(ids + 4) == self->id));
    uint32_t info = scratch + 0x900;
    CHECK(ex(c, "xbdm", "DmGetThreadInfoEx", {self->id, info}) == 0x02DA0000);
    CHECK(K->load32(info) == 52 && K->load32(info + 4) == 0);
    CHECK(K->load32(K->load32(info + 0x0C)) == self->pcr && K->load32(K->load32(info + 0x10)) == F_MAIN);
    CHECK(K->load32(K->load32(info + 0x14)) == self->stack_base);
    CHECK(K->load32(K->load32(info + 0x18)) == self->stack_limit);
    CHECK(K->load64(info + 0x1C) == K->load64(kthread + kthread::CREATE_TIME));
    char expected[32];
    std::snprintf(expected, sizeof(expected), "XThread%04X (%08X)", self->id, self->self_handle);
    CHECK(std::string(reinterpret_cast<char *>(K->host(K->load32(info + 0x28)))) == expected);
    CHECK(K->load32(info + 0x2C) == std::strlen(expected) + 1);
    CHECK(ex(c, "xbdm", "DmGetThreadInfoEx", {0xFFFF, info}) == 0x82DA0000);
    CHECK(ex(c, "xbdm", "DmSuspendThread", {yt->id}) == 0x02DA0000);
    CHECK(wait_until([&] { return blocked(y, Thread::Blocked::Suspended); }));
    CHECK(ex(c, "xbdm", "DmGetThreadInfoEx", {yt->id, info}) == 0x02DA0000 && K->load32(info + 4) == 1);
    CHECK(ex(c, "xbdm", "DmResumeThread", {yt->id}) == 0x02DA0000);
    CHECK(ex(c, "xbdm", "DmSuspendThread", {0xFFFF}) == 0x82DA0000);
    stop = 1;
    join(c, y);
}

void main_thread(ppc::Context &c) {
    events_and_timers(c);
    read_write_locks(c);
    apcs(c);
    thread_calls(c);
    c.r[3] = 0x600D;
}

const ppc::GeneratedFunction generated[] = {
    {F_MAIN, main_thread}, {F_WAIT_EVENT, wait_event}, {F_WRITER, writer}, {F_READER, reader},
    {F_STRESS, stress},    {F_ALERTABLE, alertable},   {F_APC, apc},       {F_YIELDER, yielder},
};

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_inventory: no kernel\n"), 1;
    H = &h;
    K = h.k.get();
    std::memset(K->host(h.pcr), 0, 0x2D8);
    scratch = K->memory().system_alloc(0x10000);
    thunk_page = K->memory().system_alloc(0x1000);
    K->set_generated(generated, std::size(generated));

    rtl(h);
    wide_printf(h);
    settings(h);
    hal(h);
    links_and_names(h);
    small_ones(h);
    xbdm(h);

    // the guest threads' part, the host waiting for it
    Objects &o = objects(*K);
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
    CHECK(!K->stopped);
    if (K->stopped) std::printf("stopped: %s\n", K->stop_reason.c_str());
    t = {};
    {
        std::lock_guard<std::mutex> l(o.lock);
        CHECK(o.threads.empty());
    }
    video_modes(h);
    std::filesystem::path storage = content_root(*K).parent_path();
    h.k.reset();
    std::error_code ec;
    if (!storage.empty()) std::filesystem::remove_all(storage, ec);
    return finish("kernel_inventory");
}
