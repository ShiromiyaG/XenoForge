// XAM's system families (src/kernel/xam_content.cpp, xam_info.cpp,
// xam_avatar.cpp, xam_input.cpp, xam_msg.cpp), driven through their exports
// as a game calls them, on a guest thread: content created internally,
// resolved, enumerated, its thumbnails set and read (mounted and not, the
// host's __thumbnail.png taken in), its metadata, attributes and localized
// strings; the devices' names, volume paths and enumerator; the disc (swap,
// tray, media); the dashboard's context and its notification; title IDs,
// features, settings, the Live hive, XAuth, network storage; the
// non-controller devices; avatars, Kinect (its title initialization through
// XMsg) and gamer tiles; and launching an image, which ends the run.

#include "check.h"

#include "files.h"
#include "xam.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
std::filesystem::path storage;
uint32_t scratch = 0, scratch_next = 0;

uint32_t alloc(uint32_t size) {
    uint32_t p = scratch + scratch_next;
    scratch_next += (size + 15) & ~15u;
    std::memset(K->host(p), 0, size);
    return p;
}

// an export as the code calls it: r3-r10, then the stack (a 32-bit value in
// the word at r1 + 0x54 + 8 * (n - 8)); the full r3 back
uint64_t call64(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s!%s\n", library, name);
        failures++;
        return ~0ull;
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
    return c.r[3];
}
uint32_t xam(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return uint32_t(call64(c, "xam", name, args));
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return uint32_t(call64(c, "xboxkrnl", name, args));
}

std::string host_read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string guest_string(uint32_t address) { return reinterpret_cast<const char *>(K->host(address)); }

uint32_t text(const char *s) {
    uint32_t p = alloc(uint32_t(std::strlen(s) + 1));
    std::memcpy(K->host(p), s, std::strlen(s));
    return p;
}

uint32_t wide(const char16_t *s) {
    std::u16string_view v(s);
    uint32_t p = alloc(uint32_t(2 * (v.size() + 1)));
    put_u16(*K, p, v, v.size() + 1);
    return p;
}

// a tiny disc: default.xex in its root
std::filesystem::path make_disc(const std::filesystem::path &dir) {
    constexpr size_t SECTOR = 2048;
    std::vector<uint8_t> image(36 * SECTOR, 0);
    auto le32 = [](uint8_t *p, uint32_t v) {
        for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
    };
    uint8_t *volume = image.data() + 32 * SECTOR;
    std::memcpy(volume, "MICROSOFT*XBOX*MEDIA", 20);
    le32(volume + 20, 33);
    le32(volume + 24, 2048);
    uint8_t *root = image.data() + 33 * SECTOR;
    std::memset(root, 0xFF, SECTOR);
    root[0] = root[1] = root[2] = root[3] = 0; // no children on either side
    le32(root + 4, 34);
    le32(root + 8, 8);
    root[12] = 0;
    root[13] = 11;
    std::memcpy(root + 14, "default.xex", 11);
    std::memcpy(image.data() + 34 * SECTOR, "XEX2abcd", 8);
    auto path = dir / "disc.iso";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(image.data()), std::streamsize(image.size()));
    return path;
}

// XCONTENT_DATA (0x134), and XCONTENT_DATA_INTERNAL (0x200) with an XUID
// and the running title
uint32_t content_data(uint32_t device, uint32_t type, const char16_t *display, const char *file) {
    uint32_t d = alloc(0x134);
    K->store32(d, device);
    K->store32(d + 4, type);
    put_u16(*K, d + 8, display, 128);
    std::memcpy(K->host(d + 0x108), file, std::strlen(file));
    return d;
}
uint32_t internal_data(uint32_t type, const char16_t *display, const char *file, uint64_t xuid) {
    uint32_t d = alloc(0x200);
    std::memcpy(K->host(d), K->host(content_data(1, type, display, file)), 0x134);
    K->store64(d + 0x138, xuid);
    K->store32(d + 0x140, 0xFFFFFFFF);
    return d;
}

uint32_t overlapped() { return alloc(0x1C); }

// ---- content ----

void content(ppc::Context &c) {
    scratch_next = 0;
    auto title = storage / "content" / "B13EBABEBABEBABE" / "00000000";
    auto header_of = [&](const char *name) {
        return host_read(title / "Headers" / "00000001" / (std::string(name) + ".header"));
    };
    uint32_t data = content_data(1, 1, u"Slot 1", "slot1"), disposition = alloc(4), license = alloc(4);
    uint32_t root = text("save");
    CHECK(xam(c, "XamContentCreate", {0, root, data, 1, disposition, license, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(disposition) == 1);
    uint32_t empty = content_data(1, 1, u"", "");
    CHECK(xam(c, "XamContentCreate", {0, root, empty, 1, disposition, license, 0}) == X_ERROR_INVALID_NAME);

    // thumbnails: none, then set on the mounted package and read back
    uint32_t size = alloc(4), buffer = alloc(64);
    K->store32(size, 64);
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, 0}) == X_ERROR_FILE_NOT_FOUND &&
          K->load32(size) == 0);
    uint32_t png = text("PNGDATA");
    CHECK(xam(c, "XamContentSetThumbnail", {0, data, png, 7, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamContentSetThumbnail", {0, data, png, 0x3D01, 0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamContentSetThumbnail", {5, data, png, 7, 0}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, 0, size, 0}) == X_ERROR_SUCCESS && K->load32(size) == 7);
    K->store32(size, 3);
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, 0}) == X_ERROR_INSUFFICIENT_BUFFER);
    K->store32(size, 64);
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, 0}) == X_ERROR_SUCCESS &&
          std::memcmp(K->host(buffer), "PNGDATA", 7) == 0);
    uint32_t ov = overlapped();
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, ov}) == X_ERROR_IO_PENDING && K->load32(ov) == 0);
    CHECK(xam(c, "XamContentGetThumbnail", {9, data, buffer, size, 0}) == X_ERROR_NO_SUCH_USER);
    CHECK(xam(c, "XamContentClose", {root, 0}) == X_ERROR_SUCCESS);
    std::string header = header_of("slot1");
    CHECK(header.size() == 0xA000);
    if (header.size() == 0xA000) {
        const uint8_t *m = reinterpret_cast<const uint8_t *>(header.data()) + 0x344;
        CHECK(m[0x13CE + 3] == 7 && std::memcmp(m + 0x13D6, "PNGDATA", 7) == 0);
    }

    // not mounted: written to the header at once
    uint32_t pic2 = text("PIC2");
    CHECK(xam(c, "XamContentSetThumbnail", {0, data, pic2, 4, 0}) == X_ERROR_SUCCESS);
    header = header_of("slot1");
    CHECK(header.size() == 0xA000 && header[0x344 + 0x13CE + 3] == 4 && header.substr(0x344 + 0x13D6, 4) == "PIC2");
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, 0}) == X_ERROR_SUCCESS && K->load32(size) == 4 &&
          std::memcmp(K->host(buffer), "PIC2", 4) == 0);
    // a __thumbnail.png left in the package is taken into the header
    auto png_path = title / "00000001" / "slot1" / "__thumbnail.png";
    std::ofstream(png_path, std::ios::binary) << "HOSTPNG!";
    K->store32(size, 64);
    CHECK(xam(c, "XamContentGetThumbnail", {0, data, buffer, size, 0}) == X_ERROR_SUCCESS && K->load32(size) == 8 &&
          std::memcmp(K->host(buffer), "HOSTPNG!", 8) == 0);
    CHECK(!std::filesystem::exists(png_path));
    header = header_of("slot1");
    CHECK(header.size() == 0xA000 && header[0x344 + 0x13CE + 3] == 8 && header.substr(0x344 + 0x13D6, 8) == "HOSTPNG!");
    // the internal form is for no user, which has no profile
    uint32_t internal = internal_data(1, u"Slot 1", "slot1", DEFAULT_XUID);
    CHECK(xam(c, "XamContentSetThumbnailInternal", {internal, png, 7}) == X_ERROR_NO_SUCH_USER);
    uint32_t other = content_data(1, 1, u"None", "none");
    CHECK(xam(c, "XamContentGetThumbnail", {0, other, buffer, size, 0}) == X_ERROR_FILE_NOT_FOUND);
    CHECK(xam(c, "XamContentSetThumbnail", {0, other, png, 7, 0}) == X_ERROR_FILE_NOT_FOUND);

    // metadata and attributes, by the internal form (its XUID)
    uint32_t metadata = alloc(0x93D6);
    CHECK(xam(c, "XamContentGetMetaDataInternal", {internal, metadata, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(metadata) == 1 && K->load32(metadata + 4) == 2 && guest_u16(*K, metadata + 0xCD) == u"Slot 1" &&
          K->load32(metadata + 0x13CE) == 8 && K->load64(metadata + 0x2D) == DEFAULT_XUID);
    ov = overlapped();
    CHECK(xam(c, "XamContentGetMetaDataInternal", {internal, metadata, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == 0);
    uint32_t missing = internal_data(1, u"None", "none", DEFAULT_XUID);
    CHECK(xam(c, "XamContentGetMetaDataInternal", {missing, metadata, 0}) == X_ERROR_FUNCTION_FAILED);
    uint32_t attributes = alloc(0x24);
    CHECK(xam(c, "XamContentGetAttributesInternal", {internal, attributes, 0}) == X_ERROR_SUCCESS);
    uint64_t written = uint64_t(K->load32(attributes + 0x14)) << 32 | K->load32(attributes + 0x18);
    CHECK(K->load32(attributes) == 0 && written > vfs::UNIX_EPOCH_FILETIME && K->load32(attributes + 0x20) == 0 &&
          K->load32(attributes + 4) == K->load32(attributes + 0x14));
    CHECK(xam(c, "XamContentGetAttributesInternal", {missing, attributes, 0}) == X_ERROR_FUNCTION_FAILED);

    // localized strings: English's, truncated to the buffer; a description
    // in the version-2 slots only shows for version 2
    uint32_t name = alloc(64), description = alloc(64);
    put_u16(*K, metadata + 0x9CD, u"A save", 128);
    CHECK(xam(c, "XamContentGetLocalizedString", {metadata, name, 32, description, 32}) == X_ERROR_SUCCESS);
    CHECK(guest_u16(*K, name) == u"Slot 1" && guest_u16(*K, description) == u"A save");
    CHECK(xam(c, "XamContentGetLocalizedString", {metadata, name, 3, 0, 0}) == X_ERROR_SUCCESS &&
          guest_u16(*K, name) == u"Sl");
    CHECK(xam(c, "XamContentGetLocalizedString", {0, name, 3, 0, 0}) == X_ERROR_INVALID_PARAMETER);

    // resolved paths: the user's XUID for a save only; the disc's
    uint32_t path = alloc(260);
    CHECK(xam(c, "XamContentResolve", {0, data, path, 260, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(guest_string(path) ==
          "\\Device\\Harddisk0\\Partition1\\Content\\B13EBABEBABEBABE\\00000000\\00000001\\slot1");
    uint32_t dlc = content_data(2, 2, u"DLC", "dlc1");
    CHECK(xam(c, "XamContentResolve", {0, dlc, path, 260, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(guest_string(path) == "D:\\content\\0000000000000000\\00000000\\00000002\\dlc1");
    CHECK(xam(c, "XamContentResolve", {0, data, path, 8, 0, 0, 0}) == X_ERROR_SUCCESS &&
          guest_string(path) == "\\Device");
    CHECK(xam(c, "XamContentResolve", {0, data, path, 260, 0, root, 0}) == X_ERROR_INVALID_PARAMETER);
    uint32_t nowhere = content_data(3, 1, u"?", "x");
    CHECK(xam(c, "XamContentResolve", {0, nowhere, path, 260, 0, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    ov = overlapped();
    CHECK(xam(c, "XamContentResolveInternal", {internal, path, 260, 0, 0, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == 0);
    CHECK(guest_string(path) ==
          "\\Device\\Harddisk0\\Partition1\\Content\\0000000000000000\\00000000\\00000001\\slot1");

    // the internal create: the package the structure names, opened
    uint32_t root2 = text("save2:");
    CHECK(xam(c, "XamContentCreateInternal", {root2, internal, 3, disposition, license, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(disposition) == 2);
    CHECK(xam(c, "XamContentClose", {root2, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamContentCreateInternal", {root2, empty, 3, disposition, license, 0, 0, 0}) == X_ERROR_INVALID_NAME);
    CHECK(xam(c, "XamContentOpenFile", {0, root2, text("a"), 3, disposition, license, 0}) == X_ERROR_FILE_NOT_FOUND);

    // enumerated by XUID: the user's, then the common content (none)
    uint32_t handle = alloc(4), items = alloc(4 * 0x134), returned = alloc(4);
    CHECK(xam(c, "XamContentCreateEnumeratorInternal", {DEFAULT_XUID, 1, 1, 0, 0, 4, size, handle}) == X_ERROR_SUCCESS);
    CHECK(K->load32(size) == 4 * 0x134);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x134, returned, 0}) == X_ERROR_SUCCESS &&
          K->load32(returned) == 1 && guest_string(items + 0x108) == "slot1");
    krnl(c, "NtClose", {K->load32(handle)});
    CHECK(xam(c, "XamContentCreateEnumeratorInternal", {0, 1, 1, 0, 0, 4, size, handle}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x134, returned, 0}) == X_ERROR_NO_MORE_FILES);
    krnl(c, "NtClose", {K->load32(handle)});
    CHECK(xam(c, "XamContentCreateEnumeratorInternal", {DEFAULT_XUID, 7, 1, 0, 0, 4, size, handle}) == X_E_INVALIDARG);
    CHECK(K->load32(size) == 0);

    // deleted, internally
    CHECK(xam(c, "XamContentDeleteInternal", {internal, 0}) == X_ERROR_SUCCESS);
    CHECK(!std::filesystem::exists(title / "00000001" / "slot1"));
    CHECK(xam(c, "XamContentDeleteInternal", {internal, 0}) == X_ERROR_FILE_NOT_FOUND);
    xam(c, "XamContentRegisterChangeCallback", {0x82001000});
}

// ---- the devices and the disc ----

void devices(ppc::Context &c) {
    scratch_next = 0;
    uint32_t name = alloc(64);
    CHECK(xam(c, "XamContentGetDeviceName", {1, name, 16}) == X_ERROR_SUCCESS && guest_u16(*K, name) == u"Dummy HDD");
    CHECK(xam(c, "XamContentGetDeviceName", {2, name, 10}) == X_ERROR_SUCCESS && guest_u16(*K, name) == u"Dummy ODD");
    CHECK(xam(c, "XamContentGetDeviceName", {1, name, 9}) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(xam(c, "XamContentGetDeviceName", {3, name, 16}) == X_ERROR_DEVICE_NOT_CONNECTED);
    uint32_t path = alloc(16);
    CHECK(xam(c, "XamContentGetDeviceVolumePath", {1, path, 16, 0}) == X_ERROR_SUCCESS &&
          guest_string(path) == "hdd0\\");
    CHECK(xam(c, "XamContentGetDeviceVolumePath", {1, path, 3, 1}) == X_ERROR_SUCCESS && guest_string(path) == "hd");
    CHECK(xam(c, "XamContentGetDeviceVolumePath", {2, path, 16, 0}) == X_ERROR_FUNCTION_FAILED);
    uint32_t device = alloc(0x50);
    CHECK(xam(c, "XamContentGetLocalizedDeviceData", {2, device}) == X_ERROR_SUCCESS);
    CHECK(K->load32(device) == 2 && K->load32(device + 4) == 4 && K->load64(device + 8) == 7ull << 30 &&
          K->load64(device + 0x10) == 0 && guest_u16(*K, device + 0x18) == u"Optical Drive");
    CHECK(xam(c, "XamContentGetLocalizedDeviceData", {1, device}) == X_ERROR_SUCCESS &&
          guest_u16(*K, device + 0x18) == u"Hard Drive");
    CHECK(xam(c, "XamContentGetLocalizedDeviceData", {5, device}) == X_ERROR_DEVICE_NOT_CONNECTED);

    // the enumerator: both, or the hard drive alone without read-only ones
    uint32_t size = alloc(4), handle = alloc(4), items = alloc(4 * 0x50), returned = alloc(4);
    CHECK(xam(c, "XamContentCreateDeviceEnumerator", {0, 0, 4, size, handle}) == X_ERROR_SUCCESS &&
          K->load32(size) == 4 * 0x50);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x50, returned, 0}) == X_ERROR_SUCCESS &&
          K->load32(returned) == 2);
    CHECK(K->load32(items) == 1 && guest_u16(*K, items + 0x18) == u"Dummy HDD" && K->load32(items + 0x50) == 2 &&
          K->load32(items + 0x54) == 4);
    krnl(c, "NtClose", {K->load32(handle)});
    CHECK(xam(c, "XamContentCreateDeviceEnumerator", {0, 0x80000000, 4, size, handle}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x50, returned, 0}) == X_ERROR_SUCCESS &&
          K->load32(returned) == 1 && K->load32(items) == 1);
    krnl(c, "NtClose", {K->load32(handle)});

    // the disc: the one in the drive (no executable here: disc 0 of 0) is
    // set at once, its event signaled; the tray closed on it; a game disc
    uint32_t event = alloc(0x10);
    K->host(event)[0] = 1; // a synchronization event, not signaled
    CHECK(xam(c, "XamSwapDisc", {0, event, 0}) == X_ERROR_SUCCESS && K->load32(event + 4) == 1);
    CHECK(xam(c, "XamLoaderGetDvdTrayState", {}) == 2);
    uint32_t media = alloc(4), title = alloc(4), unknown = alloc(4);
    K->store32(title, 0x1234);
    K->store32(unknown, 0x1234);
    xam(c, "XamLoaderGetMediaInfoEx", {media, title, unknown});
    CHECK(K->load32(media) == 1 && K->load32(title) == 0 && K->load32(unknown) == 0);
    K->store32(media, 0);
    xam(c, "XamLoaderGetMediaInfo", {media, 0});
    CHECK(K->load32(media) == 1);
    // an image that is not there
    CHECK(xam(c, "XamContentLaunchImageFromFileInternal", {text("game:\\nothing.xex"), text("default.xex")}) ==
          0xC000000F);
    uint32_t dlc = content_data(2, 2, u"DLC", "dlc1");
    CHECK(xam(c, "XamContentLaunchImage", {0, dlc, text("default.xex")}) == 0xC000000F);
}

// ---- the system's queries ----

void system_queries(ppc::Context &c) {
    scratch_next = 0;
    CHECK(xam(c, "XamFeatureEnabled", {0}) == 1 && xam(c, "XamFeatureEnabled", {31}) == 1);
    CHECK(xam(c, "XamFeatureEnabled", {32}) == 0 && xam(c, "XamFeatureEnabled", {38}) == 0 &&
          xam(c, "XamFeatureEnabled", {1000}) == 0);
    CHECK(xam(c, "XamIsSystemTitleId", {0xFFFE07D1}) == 1 && xam(c, "XamIsSystemTitleId", {0}) == 1);
    CHECK(xam(c, "XamIsSystemTitleId", {0x4D5308BC}) == 0 && xam(c, "XamIsSystemTitleId", {0x5841089A}) == 0);
    CHECK(xam(c, "XamIsSystemTitleId", {0x58420001}) == 1); // an 'X' publisher, not Arcade
    CHECK(xam(c, "XamIsSystemExperienceTitleId", {0x58480001}) == 1 &&
          xam(c, "XamIsSystemExperienceTitleId", {0x584A0002}) == 1);
    CHECK(xam(c, "XamIsSystemExperienceTitleId", {0x584E07D1}) == 1 &&
          xam(c, "XamIsSystemExperienceTitleId", {0x4D5308BC}) == 0);
    CHECK(xam(c, "XamIsXbox1TitleId", {0x41430006}) == 1 && xam(c, "XamIsXbox1TitleId", {0x4D5308BC}) == 0);
    CHECK(xam(c, "XamIsXbox1TitleId", {0xFFFE07D1}) == 1 && xam(c, "XamIsXbox1TitleId", {0xFF000001}) == 0);
    CHECK(xam(c, "XamIsCurrentTitleDash", {}) == 0);
    CHECK(xam(c, "XamUpdateGetBaseSystemVersion", {}) == 1888 &&
          xam(c, "XamUpdateGetCurrentSystemVersion", {}) == 1888);
    CHECK(xam(c, "XamGetStagingMode", {}) == 0 && xam(c, "XamDoesOmniNeedConfiguration", {}) == 0 &&
          xam(c, "XamFirstRunExperienceShouldRun", {}) == 0 && xam(c, "XamIsChildAccountSignedIn", {}) == 0);
    CHECK(xam(c, "XamXStudioRequest", {6, alloc(4)}) == X_E_FAIL);
    CHECK(xam(c, "XamLogLocalizationEtx", {0x80300034, 0}) == 0 &&
          xam(c, "XamLogLocalizationEtx", {0x80300035, 0}) == 0);
    CHECK(xam(c, "XamLogLocalizationEtx", {0x80300036, 0}) == 0x80004001);
    xam(c, "XampWebInstrumentationSetProfileCounts", {1, 2, 3, 4});
    xam(c, "XamLoaderRegisterLaunchRequestCallback", {0x82001000});

    // the dashboard's context: a change is broadcast
    uint32_t listener = xam(c, "XamNotifyCreateListenerInternal", {1, 1, 0});
    CHECK(listener != 0);
    uint32_t id = alloc(4), param = alloc(4);
    while (xam(c, "XNotifyGetNext", {listener, 0, id, param})) {
    } // what a first listener gets at start
    CHECK(xam(c, "XamSetDashContext", {5}) == 0 && xam(c, "XamGetDashContext", {}) == 5);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == 0x8000000C);
    CHECK(xam(c, "XamSetDashContext", {5}) == 0 && xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 0);
    // XNotifyBroadcast reaches it too
    CHECK(xam(c, "XNotifyBroadcast", {XN_SYS_STORAGEDEVICESCHANGED, 7}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == XN_SYS_STORAGEDEVICESCHANGED &&
          K->load32(param) == 7);
    CHECK(xam(c, "XNotifyQueueUI", {0, 0, 1, wide(u"Hello"), 0}) == X_ERROR_SUCCESS);
    krnl(c, "NtClose", {listener});

    uint32_t app = alloc(0xC), back = alloc(0xC);
    for (uint32_t i = 0; i < 0xC; i++) K->host(app)[i] = uint8_t(i + 1);
    xam(c, "XamSetActiveDashAppInfo", {app});
    xam(c, "XamGetActiveDashAppInfo", {back});
    CHECK(std::memcmp(K->host(app), K->host(back), 0xC) == 0);
    xam(c, "XamSetActiveDashAppInfo", {0});
    xam(c, "XamGetActiveDashAppInfo", {back});
    CHECK(K->load32(back) == 0 && K->load32(back + 8) == 0);
    uint32_t count = alloc(4);
    K->store32(count, 2);
    CHECK(xam(c, "XamGetDashBackstackData", {alloc(0x314 * 2), count}) == X_ERROR_SUCCESS);

    // the settings XAM reads
    CHECK(xam(c, "XGetAudioFlags", {}) == 0x00010001 && xam(c, "XGetVideoCapabilities", {}) == 0);
    CHECK(xam(c, "XamIsIptvEnabled", {}) == X_E_FAIL);
    uint32_t service = alloc(4);
    CHECK(xam(c, "XamIptvGetServiceName", {service}) == X_ERROR_SUCCESS && K->load32(service) != 0 &&
          guest_u16(*K, K->load32(service)) == u"Xenia TV");
    uint32_t dvr = alloc(4), used = alloc(4);
    K->store32(dvr, 5), K->store32(used, 5);
    CHECK(xam(c, "XamGetDvrStorage", {dvr, used, 0}) == X_ERROR_SUCCESS && K->load32(dvr) == 0 && K->load32(used) == 0);
    CHECK(xam(c, "XamSetDvrStorage", {100, 0}) == X_ERROR_SUCCESS);

    // the online schema: the same empty one each time
    uint32_t schema = xam(c, "XamGetOnlineSchema", {});
    CHECK(schema && K->load32(schema) == schema + 8 && K->load32(schema + 4) == 44 &&
          K->host(schema + 8 + 15)[0] == 0x2C);
    CHECK(xam(c, "XamGetOnlineSchema", {}) == schema);

    // the Live hive: not logged on
    uint32_t feature = text("feature"), value = alloc(16);
    CHECK(xam(c, "XamQueryLiveHiveA", {feature, value, 16, 0}) == 0x80151802);
    CHECK(xam(c, "XamQueryLiveHiveW", {0, value, 16, 0}) == X_E_INVALIDARG);
    uint32_t ov = overlapped();
    CHECK(xam(c, "XamQueryLiveHiveA", {feature, value, 16, ov}) == 0x8000000A && K->load32(ov) == 0 &&
          K->load32(ov + xoverlapped::EXTENDED_ERROR) == 0x80151802);
    CHECK(xam(c, "XamGetLiveHiveValueA", {feature, value, 16, 0, 0}) == 0x80151802);
    CHECK(xam(c, "XamGetLiveHiveValueW", {feature, value, 0, 0, 0}) == X_E_INVALIDARG);
    ov = overlapped();
    CHECK(xam(c, "XamGetLiveHiveValueA", {feature, value, 16, 0xFFFFFFFF, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == 0x80151802);

    // a title's cached name: none known
    uint32_t name = alloc(64), length = alloc(4);
    CHECK(xam(c, "XamGetCachedTitleName", {0, name, length}) == X_ERROR_INVALID_PARAMETER && K->load32(length) == 1);
    CHECK(xam(c, "XamGetCachedTitleName", {0x4D5308BC, name, length}) == X_ERROR_SUCCESS && K->load32(length) == 1 &&
          K->load16(name) == 0);

    // XAuth and network storage, offline
    uint32_t settings = alloc(8);
    K->store32(settings, 8);
    CHECK(xam(c, "XampXAuthStartup", {settings}) == 0x80158406);
    K->store32(settings, 4);
    CHECK(xam(c, "XampXAuthStartup", {settings}) == 0x80158401);
    K->store32(count, 3);
    xam(c, "XampXAuthShutdown", {count});
    CHECK(K->load32(count) == 0 && xam(c, "XampXAuthGetTitleBuffer", {}) == 0);
    CHECK(xam(c, "XamNetworkStorageUserHasPrivilege", {0}) == 0 &&
          xam(c, "XamNetworkStorageUserHasPrivilege", {0xFF}) == 0);
    CHECK(xam(c, "XamNetworkStorageHasUserEnabledStorage", {0}) == 0);

    // devices that are never there
    CHECK(xam(c, "XamVoiceGetMicArrayStatus", {}) == 0);
    for (const char *e :
         {"EnumerateMediaObjects", "EnumerateMediaObjects_", "EnumerateMediaObjects_0", "EnumerateMediaObjects_1"})
        CHECK(xam(c, e, {}) == 0x80004001);
    CHECK(xam(c, "XamMediaVerificationClose", {}) == 0 && xam(c, "XamMediaVerificationInject", {0, 0}) == 0);
    CHECK(xam(c, "XAutomationpUnbindController", {0}) == 1 && xam(c, "XAutomationpUnbindController", {4}) == 0);
    uint32_t raw = alloc(0x40), raw_length = alloc(4), state = alloc(2);
    K->store32(raw_length, 0x20);
    CHECK(xam(c, "XamInputNonControllerGetRawEx", {4, raw, raw_length, state}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamInputNonControllerGetRawEx", {5, raw, raw_length, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamInputNonControllerGetRawEx", {6, raw, raw_length, state}) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xam(c, "XamInputNonControllerGetRaw", {state, raw_length, raw}) == X_ERROR_DEVICE_NOT_CONNECTED);
    K->store32(raw_length, 0x21);
    CHECK(xam(c, "XamInputNonControllerGetRaw", {state, raw_length, raw}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamInputNonControllerSetRawEx", {5, raw, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamInputNonControllerSetRawEx", {7, raw, 4}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamInputNonControllerSetRaw", {0x10, raw}) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xam(c, "XamInputNonControllerSetRaw", {0x21, raw}) == X_ERROR_INVALID_PARAMETER);
}

// ---- avatars, Kinect, tiles ----

void avatars_kinect_tiles(ppc::Context &c) {
    scratch_next = 0;
    CHECK(xam(c, "XamAvatarInitialize", {1, 0, 0, alloc(20), 0, 0}) == 0xFFFFFFFF);
    xam(c, "XamAvatarShutdown", {});
    uint32_t metadata = alloc(0x3E8), back = alloc(0x3E8);
    CHECK(xam(c, "XamAvatarGetManifestLocalUser", {0, back, 0}) == X_E_FAIL);
    CHECK(xam(c, "XamAvatarGetManifestLocalUser", {4, back, 0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamAvatarGetManifestLocalUser", {1, back, 0}) == X_E_NO_SUCH_USER);
    for (uint32_t i = 0; i < 0x3E8; i++) K->host(metadata)[i] = uint8_t(i * 7);
    CHECK(xam(c, "XamAvatarSetManifest", {0, metadata, 0}) == 0);
    CHECK(xam(c, "XamAvatarSetManifest", {2, metadata, 0}) == X_E_NO_SUCH_USER);
    uint32_t ov = overlapped();
    CHECK(xam(c, "XamAvatarGetManifestLocalUser", {0, back, ov}) == X_ERROR_IO_PENDING && K->load32(ov) == 0 &&
          std::memcmp(K->host(metadata), K->host(back), 0x3E8) == 0);
    ov = overlapped();
    CHECK(xam(c, "XamAvatarGetManifestLocalUser", {2, back, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == X_ERROR_FUNCTION_FAILED && K->load32(ov + xoverlapped::EXTENDED_ERROR) == X_E_NO_SUCH_USER);
    uint32_t a = alloc(4), b = alloc(4);
    K->store32(a, 9), K->store32(b, 9);
    CHECK(xam(c, "XamAvatarGetAssetsResultSize", {0xFF, a, b}) == 0 && K->load32(a) == 0 && K->load32(b) == 0);
    ov = overlapped();
    CHECK(xam(c, "XamAvatarGetAssets", {metadata, 0xFF, 0, a, b, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == X_ERROR_FUNCTION_FAILED && K->load32(ov + xoverlapped::EXTENDED_ERROR) == X_E_FAIL);
    CHECK(xam(c, "XamAvatarGetAssets", {metadata, 0xFF, 0, a, b, 0}) == 0);
    CHECK(xam(c, "XamAvatarManifestGetBodyType", {metadata}) == 1);
    K->store32(a, 9);
    CHECK(xam(c, "XamAvatarGetInstrumentation", {1, a}) == 1 && K->load32(a) == 0);
    for (const char *name : {"XamAvatarGetMetadataRandom", "XamAvatarGenerateMipMaps", "XamAvatarBeginEnumAssets"}) {
        ov = overlapped();
        K->store32(ov, 0x1234);
        uint32_t ov_at = std::strcmp(name, "XamAvatarGetMetadataRandom") == 0 ? 3
                         : std::strcmp(name, "XamAvatarGenerateMipMaps") == 0 ? 4
                                                                              : 5;
        std::vector<uint64_t> args(6, 0);
        args[ov_at] = ov;
        CHECK(xam(c, name, {args[0], args[1], args[2], args[3], args[4], args[5]}) == X_ERROR_IO_PENDING &&
              K->load32(ov) == 0);
    }
    CHECK(xam(c, "XamAvatarEnumAssets", {alloc(16), 0, 0}) == 0x80070012);
    CHECK(xam(c, "XamAvatarEndEnumAssets", {0}) == 0);
    ov = overlapped();
    CHECK(xam(c, "XamAvatarWearNow", {1, a, ov}) == X_ERROR_IO_PENDING && K->load32(ov) == 0);
    uint32_t asset_id = alloc(16), package = alloc(0x148);
    K->store64(asset_id, 0x0102030405060708ull);
    K->store32(asset_id + 8, 0x0A0B0C0D);
    K->store32(asset_id + 0xC, 0x4D5308BC);
    std::memset(K->host(package), 0xEE, 0x148);
    xam(c, "XamAvatarGetInstalledAssetPackageDescription", {asset_id, package});
    CHECK(K->load32(package) == 0 && K->load32(package + 4) == 0x9000 && K->load64(package + 0x138) == 0 &&
          K->load32(package + 0x140) == 0x4D5308BC);
    CHECK(guest_string(package + 0x108) == "01020304050607080A0B0C0D4D5308BC");
    CHECK(xam(c, "XamAvatarReinstallAwardedAsset", {0, 0, 0}) == 0 && xam(c, "XamAvatarSetCustomAsset", {}) == 0);

    // Kinect: none set up
    uint32_t status = alloc(0x10);
    std::memset(K->host(status), 0xEE, 0x10);
    CHECK(xam(c, "XamNuiGetDeviceStatus", {status}) == 0xC0050006 && K->load32(status) == 0 &&
          K->load32(status + 0xC) == 0);
    CHECK(xam(c, "XamNuiIsDeviceReady", {}) == 0 && xam(c, "XamNuiHudIsEnabled", {}) == 0 &&
          xam(c, "XamNuiIsChatMicEnabled", {}) == 0 && xam(c, "XamIsNuiUIActive", {}) == 0);
    CHECK(xam(c, "XamIsNuiAutomationEnabled", {0, 0}) == 0 && xam(c, "XamIsNatalPlaybackEnabled", {0, 0}) == 0);
    uint32_t index = alloc(4);
    K->store32(index, 9);
    CHECK(xam(c, "XamUserNuiGetUserIndexForSignin", {index}) == 0 && K->load32(index) == 0);
    CHECK(xam(c, "XamUserNuiGetUserIndexForSignin", {0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserNuiGetUserIndexForBind", {index}) == X_E_FAIL &&
          xam(c, "XamUserNuiGetUserIndexForBind", {0}) == X_E_INVALIDARG);
    CHECK(xam(c, "XamUserNuiGetUserIndex", {0, index}) == X_E_NO_SUCH_USER);
    CHECK(xam(c, "XamUserNuiEnableBiometric", {0, 1}) == X_E_INVALIDARG);
    CHECK(call64(c, "xam", "XamNuiSkeletonGetBestSkeletonIndex", {0}) == ~0ull);
    CHECK(xam(c, "XamNuiHudSetEngagedTrackingID", {7}) == 0 &&
          call64(c, "xam", "XamNuiHudGetEngagedTrackingID", {}) == 7);
    CHECK(xam(c, "XamNuiHudSetEngagedTrackingID", {0}) == 0 &&
          call64(c, "xam", "XamNuiHudGetEngagedTrackingID", {}) == 7);
    uint64_t session = call64(c, "xam", "XamNuiIdentityGetSessionId", {});
    CHECK(session == 0xDEADF00DDEADF00Dull && call64(c, "xam", "XamNuiIdentityGetSessionId", {}) == session);
    CHECK(xam(c, "XamNuiIdentityEnrollForSignIn", {0, 0, 0, 0}) == X_E_FAIL &&
          xam(c, "XamNuiIdentityAbort", {0}) == X_E_FAIL);
    uint32_t calibration = alloc(0x30);
    std::memset(K->host(calibration), 0xEE, 0x30);
    CHECK(xam(c, "XamNuiGetDepthCalibration", {calibration}) == 0xD000000F && K->load32(calibration + 0x2C) == 0);
    uint32_t tilt = alloc(0x50);
    K->store32(tilt, 0x50);
    K->store32(tilt + 4, 0x58745373);
    CHECK(xam(c, "XamNuiCameraTiltGetStatus", {tilt}) == X_E_FAIL &&
          xam(c, "XamNuiCameraTiltGetStatus", {0}) == X_E_FAIL);
    CHECK(xam(c, "XamNuiCameraElevationGetAngle", {a, b}) == X_E_FAIL);
    CHECK(xam(c, "XamNuiCameraGetTiltControllerType", {}) == 0);
    CHECK(xam(c, "XamNuiCameraSetFlags", {0, 0}) == X_E_DEVICE_NOT_CONNECTED);
    CHECK(xam(c, "XamNuiCameraTiltSetCallback", {0x82001000}) == 0);
    uint32_t engagement = alloc(0x1C);
    std::memset(K->host(engagement), 0xEE, 0x1C);
    xam(c, "XamNuiPlayerEngagementUpdate", {0, 0, engagement});
    CHECK(K->load32(engagement) == 0 && K->load32(engagement + 0x18) == 0);
    // XNuiTitleInitialize (XAM's message 0x2B003): XAM version 5 only; the
    // HUD's flags and versions as given; XNuiTitleShutdown clears them
    uint32_t init = alloc(0x18), xam_version = alloc(8), xtl_version = alloc(8);
    K->store64(init, 5);
    K->store64(init + 8, 0x0002000012345678ull);
    K->store32(init + 0x10, 0x40000200);
    CHECK(xam(c, "XMsgInProcessCall", {0xFE, 0x2B003, init, 0x18}) == 0);
    CHECK(xam(c, "XamNuiHudGetInitializeFlags", {}) == 0x40000200);
    xam(c, "XamNuiHudGetVersions", {xam_version, xtl_version});
    CHECK(K->load64(xam_version) == 5 && K->load64(xtl_version) == 0x0002000012345678ull);
    K->store32(init + 0x10, 0x80000000);
    CHECK(xam(c, "XMsgInProcessCall", {0xFE, 0x2B003, init, 0x18}) == X_E_FAIL);
    CHECK(xam(c, "XamNuiHudGetInitializeFlags", {}) == 0x80000000);
    CHECK(xam(c, "XMsgInProcessCall", {0xFE, 0x2B004, 0, 0}) == 0 && xam(c, "XamNuiHudGetInitializeFlags", {}) == 0);
    xam(c, "XamNuiHudGetVersions", {xam_version, 0});
    CHECK(K->load64(xam_version) == 0);
    K->store64(init, 4);
    CHECK(xam(c, "XMsgInProcessCall", {0xFE, 0x2B003, init, 0x18}) == X_E_FAIL);

    // gamer tiles: none
    uint32_t out = alloc(64), size = alloc(4);
    K->store32(size, 64);
    CHECK(xam(c, "XamReadTile", {1, 0, 0, 0, out, size, 0}) == X_ERROR_FILE_NOT_FOUND && K->load32(size) == 0);
    CHECK(xam(c, "XamReadTile", {1, 0, 0, 0, 0, size, 0}) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(xam(c, "XamReadTile", {1, 0, 0, 5, out, size, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamReadTile", {1, 0, DEFAULT_XUID, 5, out, size, 0}) == X_ERROR_FILE_NOT_FOUND);
    CHECK(xam(c, "XamReadTile", {1, 0, 0, 0, out, 0, 0}) == X_ERROR_INVALID_PARAMETER);
    ov = overlapped();
    CHECK(xam(c, "XamReadTile", {1, 0, 0, 0, out, size, ov}) == X_ERROR_IO_PENDING &&
          K->load32(ov) == X_ERROR_FILE_NOT_FOUND);
    CHECK(xam(c, "XamReadTileEx", {1, 0, 0, 0, 0, 0, out, size, 0}) == X_ERROR_FILE_NOT_FOUND);
    CHECK(xam(c, "XamReadTileEx", {1, 0, 0, 7, 0, 0, out, size, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamReadTileToTexture", {1, 0, 0, 0, out, 16, 4, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamReadTileToTexture", {1, 0, 0, 0, 0, 16, 4, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamReadTileToTexture", {1, 0, 0, 3, out, 16, 4, 0}) == X_ERROR_INVALID_PARAMETER);
    CHECK(xam(c, "XamWriteGamerTile", {4, 0, 1, 2, 0, 0}) == X_ERROR_FUNCTION_FAILED);
    CHECK(xam(c, "XamWriteGamerTile", {1, 0, 1, 2, 0, 0}) == X_E_INVALIDARG);
    ov = overlapped();
    CHECK(xam(c, "XamWriteGamerTile", {0, 0, 1, 2, 0, ov}) == X_ERROR_IO_PENDING && K->load32(ov) == 0);

    // a gamer tile key: 24 hex digits, the title's and the tiles' IDs
    uint32_t key = alloc(16), title = alloc(4), big = alloc(4), small = alloc(4);
    K->host(key)[0] = 4; // WSTRING
    K->store32(key + 8, 50);
    K->store32(key + 0xC, wide(u"FFFE07D10002000200010002"));
    CHECK(xam(c, "XamParseGamerTileKey", {key, title, big, small}) == X_ERROR_SUCCESS);
    CHECK(K->load32(title) == 0xFFFE07D1 && K->load32(big) == 0x00020002 && K->load32(small) == 0x00010002);
    K->store32(title, 0);
    K->store32(key + 0xC, wide(u"FFFE07D10002000200010002Z"));
    CHECK(xam(c, "XamParseGamerTileKey", {key, title, big, small}) == X_ERROR_INVALID_PARAMETER);
    K->store32(key + 0xC, wide(u"FFFE07D1000200020001000Z"));
    CHECK(xam(c, "XamParseGamerTileKey", {key, title, big, small}) == X_ERROR_SUCCESS && K->load32(title) == 0);
    K->store32(key + 8, 0x65);
    CHECK(xam(c, "XamParseGamerTileKey", {key, title, big, small}) == X_ERROR_INVALID_PARAMETER);
    K->host(key)[0] = 1; // INT32: not parsed
    CHECK(xam(c, "XamParseGamerTileKey", {key, title, big, small}) == X_ERROR_SUCCESS && K->load32(title) == 0);
    CHECK(xam(c, "XamParseGamerTileKey", {0, title, big, small}) == X_ERROR_INVALID_PARAMETER);
}

// ---- the guest functions ----

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_LAUNCH = 0x82000010,
    F_SWAP = 0x82000020,
};

void main_thread(ppc::Context &c) {
    content(c);
    devices(c);
    system_queries(c);
    avatars_kinect_tiles(c);
    c.r[3] = 0x600D;
}

// an image on the disc: the run ends with the launch
void launch_thread(ppc::Context &c) {
    scratch_next = 0x8000;
    xam(c, "XamContentLaunchImageFromFileInternal", {text("game:\\default.xex"), text("default.xex")});
    c.r[3] = 0xBAD; // never reached
}

// another disc: the run ends
void swap_thread(ppc::Context &c) {
    scratch_next = 0x9000;
    xam(c, "XamSwapDisc", {2, alloc(0x10), 0});
    c.r[3] = 0xBAD;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_LAUNCH, launch_thread},
    {F_SWAP, swap_thread},
};

uint32_t run_thread(Objects &o, uint32_t start) {
    Thread::Params p;
    p.stack_size = 256 * 1024;
    p.start_address = start;
    uint32_t status;
    Ref<Thread> t = o.create_thread(p, status);
    CHECK(t && status == X_STATUS_SUCCESS);
    if (!t) return 0xFFFFFFFF;
    CHECK(o.scheduler->start(*t));
    o.scheduler->join(*t);
    return t->exit_code;
}

} // namespace

int main() {
    std::error_code ec;
    // a directory of its own, so copies run in parallel
    auto dir = std::filesystem::temp_directory_path() /
               ("xenoforge_xam_system_test_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    storage = dir / "storage";
    auto iso = make_disc(dir);

    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        CHECK(mount_disc(k, iso, error));
        CHECK(mount_storage(k, storage, error));
        if (!error.empty()) std::printf("mount: %s\n", error.c_str());
        scratch = memory->system_alloc(0x10000);

        Objects &o = objects(k);
        CHECK(run_thread(o, F_MAIN) == 0x600D);
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
        // another disc and a launch each end their thread and the run (the
        // run restarted between them: threads start only while it goes)
        CHECK(run_thread(o, F_SWAP) == 0);
        CHECK(k.stopped && k.stop_reason.find("asks for disc 2 (XamSwapDisc)") != std::string::npos);
        k.stopped = false;
        k.stop_reason.clear();
        CHECK(run_thread(o, F_LAUNCH) == 0);
        CHECK(k.stopped && k.stop_reason.find("launches default.xex from game:\\default.xex") != std::string::npos);
    }
    std::filesystem::remove_all(dir, ec);
    return finish("kernel_xam_system");
}
