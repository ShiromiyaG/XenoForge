/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's content: saves and other packages under the content root in Xenia's
// layout, so the two share them. A package is an STFS file, as the console
// keeps it (xf::write_stfs_package; ContentFormat::Directory keeps Xenia's
// directory packages instead, and a directory package already there stays
// one):
//
//   content/<XUID>/<TITLE ID>/<TYPE>/<file name>              the package file
//   content/<XUID>/<TITLE ID>/<TYPE>/<file name>/...          or a directory package
//   content/<XUID>/<TITLE ID>/Headers/<TYPE>/<file name>.header  (directory packages)
//   content-work/<XUID>/<TITLE ID>/<TYPE>/<file name>/...     an STFS package while a title has it open
//
// An STFS package a title opens for writing is a directory device in the
// content-work tree (extracted from the file, or new) for as long as it is
// open; closing it, or XamContentFlush, writes the package file again whole
// (to a temporary file and a rename, so the package is never half there),
// and the closing removes the working directory. A working directory found
// at an open is what a run that ended without closing left, and is used.
//
// (16, 8 and 8 uppercase hex digits; XUID 0 for content of no profile, the
// marketplace's always). The header file is Xenia's XContentContainerHeader
// (the STFS header's CON magic, its metadata, then Xenia's 42-byte file
// name), rounded up to 4 KB. A package opened is a host directory device at
// \Device\Content\<N> under the title's root name ("save:").
//
// Translated from Xenia (BSD; see LICENSE-xenia): xam_content.cc,
// xam_content_aggregate.cc, xam_content_device.cc, content_manager.cc,
// xcontent/xcontent.h, xcontent_package.cc and xcontent_package_directory.cc,
// xenumerator.cc's ContentEnumerator; checked against them. Where it
// differs, and why:
//   - no SPA is read from the executable, so a new package's header has its
//     display name in the English slot and no title name or thumbnails
//     (Xenia puts the name in the SPA's default language and takes the
//     title's name and icon from it);
//   - package files (STFS and SVOD: DLC, updates, XBLA titles) are mounted
//     through xf::Package as read-only devices, as Xenia mounts them, and
//     publisher content is looked up under the title's own ID only. New
//     content, and a console-format STFS package opened to be written (a
//     save), is an STFS package written back whole at its close, as above;
//     Xenia creates directory packages (its ContentPackageContainer has no
//     writing). Its CON signature is zero: the console's private key is not
//     ours to have, and Xenia's reader and ours take it;
//   - closing a package leaves the files still open in it usable (Xenia
//     releases a handle of each);
//   - the hard drive's size is the content root's volume's, the disc's
//     Xenia's dummy one;
//   - a thumbnail or metadata asked for is the mounted package's (its header
//     in memory) when it is mounted, and a thumbnail set on a package not
//     mounted is written to its header (Xenia reads a mounted package's
//     header from the host again, and loses a thumbnail set on one that is
//     not, never writing its header);
//   - XamContentGetAttributesInternal gives the package's last write time
//     as all three of its times (std::filesystem has only that one);
//   - XamContentLaunchImage* and XamSwapDisc to another disc end the run
//     with the reason (Xenia extracts the image and asks to be restarted, or
//     asks the user for the other disc); the disc's package for a title of
//     -1 is the running title's;
//   - XamLoaderGetDvdTrayState says closed when the title came on a disc
//     (Xenia's tray starts open, whatever the title came on).
//
// Also from xam_content.cc: XamSwapDisc and the loader's media queries; from
// xam_content_device.cc: the devices' names, data and enumerator.

#include "xam.h"

#include "xenoforge/package.h"
#include "xenoforge/sha1.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace xf::kernel {
namespace {

constexpr uint32_t CURRENTLY_RUNNING_TITLE = 0xFFFFFFFF;
constexpr uint32_t TYPE_SAVED_GAME = 1, TYPE_MARKETPLACE = 2, TYPE_PROFILE = 0x10000;
constexpr uint32_t DEVICE_HDD = 1, DEVICE_ODD = 2;
constexpr uint32_t DEPLOYMENT_OTHER = 3;

// XCONTENT_DATA (0x134), _AGGREGATE (0x148), _INTERNAL (0x200)
constexpr uint32_t CONTENT_DATA_SIZE = 0x134, AGGREGATE_SIZE = 0x148, INTERNAL_SIZE = 0x200;

// XContentContainerHeader: XContentHeader (0x344), XContentMetadata
// (0x93D6), the file name (42)
constexpr size_t HEADER_SIZE = 0x9744, HEADER_FILE_SIZE = 0xA000, META = 0x344;
constexpr uint32_t MAGIC_CON = 0x434F4E20, MAGIC_LIVE = 0x4C495645, MAGIC_PIRS = 0x50495253;
constexpr size_t FILE_NAME_AT = 0x971A, FILE_NAME_SIZE = 42;
// in the metadata
constexpr size_t M_CONTENT_TYPE = 0x0, M_VERSION = 0x4, M_CONTENT_SIZE = 0x8, M_EXECUTION = 0x10, M_TITLE_ID = 0x1C;
constexpr size_t M_PROFILE_ID = 0x2D, M_DATA_FILE_COUNT = 0x59, M_VOLUME_TYPE = 0x65, M_DISPLAY_NAME = 0xCD;
constexpr size_t M_DISPLAY_NAME_EX = 0x50D6, M_DESCRIPTION = 0x9CD, M_DESCRIPTION_EX = 0x90D6;
constexpr size_t M_THUMBNAIL_SIZE = 0x13CE, M_THUMBNAIL = 0x13D6, METADATA_SIZE = 0x93D6;
// the thumbnail's room: version 1's, and version 2's (three more languages'
// names take the rest)
constexpr uint32_t THUMBNAIL_V1 = 0x4000, THUMBNAIL_V2 = 0x3D00;

uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t rd64(const uint8_t *p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }
void wr32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void wr64(uint8_t *p, uint64_t v) { wr32(p, uint32_t(v >> 32)), wr32(p + 4, uint32_t(v)); }

std::u16string rd_u16(const uint8_t *p, size_t max) {
    std::u16string s;
    for (size_t i = 0; i < max; i++) {
        char16_t ch = char16_t(p[2 * i] << 8 | p[2 * i + 1]);
        if (!ch) break;
        s += ch;
    }
    return s;
}
void wr_u16(uint8_t *p, std::u16string_view s, size_t count) {
    std::memset(p, 0, count * 2);
    for (size_t i = 0; i < s.size() && i + 1 < count; i++) p[2 * i] = uint8_t(s[i] >> 8), p[2 * i + 1] = uint8_t(s[i]);
}

std::string trim(std::string s) {
    const char *space = " \t\r\n\f\v";
    size_t a = s.find_first_not_of(space);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(space) - a + 1);
}

bool equal_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

std::string hex(uint64_t v, int digits) {
    char s[24];
    std::snprintf(s, sizeof(s), "%0*llX", digits, (unsigned long long)v);
    return s;
}

std::u16string widen(std::string_view s) { return std::u16string(s.begin(), s.end()); }

// XCONTENT_DATA_INTERNAL's fields
struct ContentData {
    uint32_t device_id = 0, content_type = 0;
    std::u16string display_name;
    std::string file_name;
    uint64_t xuid = 0;
    uint32_t title_id = CURRENTLY_RUNNING_TITLE, license_mask = 0;
    uint64_t content_size = 0;
    // a package is found by these (XCONTENT_DATA_INTERNAL's operator==)
    bool same(const ContentData &o) const {
        return device_id == o.device_id && title_id == o.title_id && content_type == o.content_type &&
               file_name == o.file_name;
    }
};

// ReadContentData: by the structure's size
bool read_content_data(Kernel &k, uint32_t p, uint32_t size, ContentData &d) {
    if (size != CONTENT_DATA_SIZE && size != AGGREGATE_SIZE && size != INTERNAL_SIZE) return false;
    d.device_id = k.load32(p);
    d.content_type = k.load32(p + 4);
    d.display_name = guest_u16(k, p + 8, 128);
    const char *name = reinterpret_cast<const char *>(k.host(p + 0x108));
    d.file_name.assign(name, strnlen(name, FILE_NAME_SIZE));
    // both: the XUID 8-aligned after XCONTENT_DATA's 0x134 bytes, then the title
    if (size != CONTENT_DATA_SIZE) {
        d.xuid = k.load64(p + 0x138);
        d.title_id = k.load32(p + 0x140);
    }
    if (size == INTERNAL_SIZE) {
        d.license_mask = k.load32(p + 0x144);
        d.content_size = k.load64(p + 0x148);
    }
    return true;
}

// an enumerator's XCONTENT_DATA (the first 0x134 bytes of the internal one)
void write_content_data(uint8_t *p, const ContentData &d) {
    std::memset(p, 0, CONTENT_DATA_SIZE);
    wr32(p, d.device_id);
    wr32(p + 4, d.content_type);
    wr_u16(p + 8, d.display_name, 128);
    std::memcpy(p + 0x108, d.file_name.data(), std::min(d.file_name.size(), FILE_NAME_SIZE));
}

struct Package {
    std::string root_name; // without its ':'
    std::string device_path;
    std::filesystem::path host_path, header_path;
    std::vector<uint8_t> header; // HEADER_SIZE
    ContentData data;
    // a package file (STFS or SVOD: DLC, an update, an XBLA title), whose own
    // first bytes are the header, not a directory with a header file beside it
    bool file = false;
    // an STFS package written as a file at its close: host_path is its working directory, this the file
    std::filesystem::path stfs_path;
};

struct ContentState {
    ~ContentState();
    std::mutex mutex;
    bool stfs = true; // new content is an STFS package (set_content_format)
    std::vector<std::unique_ptr<Package>> mounted;
    uint32_t next_device = 0;
    uint32_t change_callback = 0; // XamContentRegisterChangeCallback's, kept (Xenia never calls it)
};

bool export_stfs(const Package &p);

ContentState &content(Kernel &k) { return k.state<ContentState>(); }

// A package still open as the kernel ends is written as at its close.
ContentState::~ContentState() {
    for (const auto &p : mounted) {
        if (p->stfs_path.empty() || !export_stfs(*p)) continue;
        std::error_code ec;
        std::filesystem::remove_all(p->host_path, ec);
    }
}

uint32_t running(Kernel &k, uint32_t title) { return title == CURRENTLY_RUNNING_TITLE || !title ? title_id(k) : title; }

// ContentManager::ResolvePackageRoot and ResolvePackagePath
std::filesystem::path package_root(Kernel &k, uint64_t xuid, uint32_t title, uint32_t type) {
    return content_root(k) / hex(xuid, 16) / hex(running(k, title), 8) / hex(type, 8);
}

std::filesystem::path package_path(Kernel &k, uint64_t xuid, const ContentData &d) {
    uint64_t used = d.xuid != ~0ull && d.xuid ? d.xuid : xuid;
    if (d.content_type == TYPE_MARKETPLACE) used = 0;
    return package_root(k, used, d.title_id, d.content_type) / vfs::host_name(trim(d.file_name));
}

// ComputeHeaderPath: <TITLE>/Headers/<TYPE>/<name>.header beside the type's directory
std::filesystem::path header_path_of(const std::filesystem::path &package) {
    std::filesystem::path name = package.filename();
    name += ".header";
    return package.parent_path().parent_path() / "Headers" / package.parent_path().filename() / name;
}

// ContentPackage::InitializePackageHeader
std::vector<uint8_t> new_header(Kernel &k, const ContentData &d) {
    std::vector<uint8_t> h(HEADER_SIZE, 0);
    wr32(h.data(), MAGIC_CON);
    wr32(h.data() + 0x340, uint32_t(META + 0x93D6)); // header size
    uint8_t *m = h.data() + META;
    wr32(m + M_CONTENT_TYPE, d.content_type);
    wr32(m + M_VERSION, 2);
    if (const Xex *x = k.xex(); x && x->has_execution_info) {
        const XexExecutionInfo &e = x->execution;
        uint8_t *p = m + M_EXECUTION;
        wr32(p + 0x0, e.media_id), wr32(p + 0x4, e.version), wr32(p + 0x8, e.base_version);
        p[0x10] = e.platform, p[0x11] = e.executable_table, p[0x12] = e.disc_number, p[0x13] = e.disc_count;
        wr32(p + 0x14, e.savegame_id);
    }
    wr32(m + M_TITLE_ID, d.title_id);
    wr64(m + M_PROFILE_ID, d.xuid);
    wr32(m + M_DATA_FILE_COUNT, 1);
    wr32(m + M_VOLUME_TYPE, 0); // STFS
    wr_u16(m + M_DISPLAY_NAME, d.display_name, 128);
    if (d.content_type == TYPE_SAVED_GAME)
        std::memcpy(h.data() + FILE_NAME_AT, d.file_name.data(), std::min(d.file_name.size(), FILE_NAME_SIZE));
    return h;
}

// ContentPackageDirectory::ReadContentHeaderFile: Xenia's full header, or
// its older one (XCONTENT_DATA_AGGREGATE, then the license mask)
bool read_header(const std::filesystem::path &path, std::vector<uint8_t> &h, uint32_t &license) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size < AGGREGATE_SIZE) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    h.assign(HEADER_SIZE, 0);
    license = 0;
    if (size < META + 0x93D6) {
        std::vector<uint8_t> a(AGGREGATE_SIZE + 4, 0);
        f.read(reinterpret_cast<char *>(a.data()), std::streamsize(std::min<uint64_t>(size, a.size())));
        wr32(h.data(), MAGIC_CON);
        wr32(h.data() + 0x340, uint32_t(META + 0x93D6));
        uint8_t *m = h.data() + META;
        wr32(m + M_CONTENT_TYPE, rd32(a.data() + 4));
        wr32(m + M_DATA_FILE_COUNT, 1);
        wr32(m + M_VERSION, 2);
        wr32(m + M_TITLE_ID, rd32(a.data() + 0x140));
        std::memcpy(m + M_DISPLAY_NAME, a.data() + 8, 0x100);
        std::memcpy(h.data() + FILE_NAME_AT, a.data() + 0x108, FILE_NAME_SIZE);
        license = rd32(a.data() + AGGREGATE_SIZE);
        return true;
    }
    if (size != META + 0x93D6 && size != HEADER_FILE_SIZE) return false;
    f.read(reinterpret_cast<char *>(h.data()), std::streamsize(std::min<uint64_t>(size, HEADER_SIZE)));
    return true;
}

// A console-format package file at rest takes a header changed in memory (a thumbnail set on it) in place: its
// bytes up to the header's size, the content ID (the SHA-1 of the metadata) made again. Other package files
// (LIVE, PIRS, SVOD) are read-only.
bool patch_package_header(const Package &p) {
    if (rd32(p.header.data()) != MAGIC_CON) return true;
    size_t size = rd32(p.header.data() + 0x340);
    if (!size) size = META + METADATA_SIZE;
    if (size < META + 0x100 || size > p.header.size()) return true;
    std::fstream f(p.host_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> disk(size);
    f.read(reinterpret_cast<char *>(disk.data()), std::streamsize(size));
    if (size_t(f.gcount()) != size) return true;
    // skip the content ID: a package of the console's own was hashed as it was
    if (std::equal(disk.begin(), disk.begin() + 0x32C, p.header.begin()) &&
        std::equal(disk.begin() + 0x340, disk.end(), p.header.begin() + 0x340))
        return true;
    std::vector<uint8_t> out(p.header.begin(), p.header.begin() + std::ptrdiff_t(size));
    sha1({out.data() + META, size - META}, out.data() + 0x32C);
    f.clear();
    f.seekp(0);
    f.write(reinterpret_cast<const char *>(out.data()), std::streamsize(size));
    return bool(f);
}

bool write_header(const Package &p) {
    if (!p.stfs_path.empty()) return true; // written with the package
    if (p.file) return patch_package_header(p);
    std::error_code ec;
    std::filesystem::create_directories(p.header_path.parent_path(), ec);
    std::ofstream f(p.header_path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::vector<uint8_t> out(HEADER_FILE_SIZE, 0);
    std::memcpy(out.data(), p.header.data(), HEADER_SIZE);
    f.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

bool valid_header(const std::vector<uint8_t> &h) {
    uint32_t magic = rd32(h.data());
    return (magic == MAGIC_CON || magic == MAGIC_LIVE || magic == MAGIC_PIRS) && rd32(h.data() + META);
}

uint64_t directory_size(const std::filesystem::path &dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

// ContentPackage::GetContentMetadata
ContentData metadata(const std::filesystem::path &host_path, const std::vector<uint8_t> &h, uint32_t license,
                     bool file = false) {
    const uint8_t *m = h.data() + META;
    ContentData d;
    d.device_id = DEVICE_HDD;
    d.title_id = rd32(m + M_TITLE_ID);
    d.content_type = rd32(m + M_CONTENT_TYPE);
    d.content_size = rd64(m + M_CONTENT_SIZE);
    if (!d.content_size) {
        std::error_code ec;
        d.content_size = file ? std::filesystem::file_size(host_path, ec) : directory_size(host_path);
    }
    d.display_name = rd_u16(m + M_DISPLAY_NAME, 128);
    for (uint32_t i = 0; d.display_name.empty() && i < 12; i++) {
        if (i < 9)
            d.display_name = rd_u16(m + M_DISPLAY_NAME + 0x100 * i, 128);
        else if (rd32(m + M_VERSION) >= 2)
            d.display_name = rd_u16(m + M_DISPLAY_NAME_EX + 0x100 * (i - 9), 128);
    }
    d.file_name = vfs::guest_name(host_path.filename());
    // (Xenia's own file name field is past a package file's metadata, where a
    // real package has other data)
    if (d.content_type == TYPE_SAVED_GAME && !file) {
        const char *name = reinterpret_cast<const char *>(h.data() + FILE_NAME_AT);
        std::string saved(name, strnlen(name, FILE_NAME_SIZE));
        if (!saved.empty()) d.file_name = saved;
    }
    d.license_mask = license;
    d.xuid = rd64(m + M_PROFILE_ID);
    return d;
}

// ContentPackageContainer::LoadPackage and ComputePackageLicenseMask: the
// package file's first bytes are the header; the licenses' bits are or-ed
bool open_package_file(const std::filesystem::path &path, Package &p) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    p.header.assign(HEADER_SIZE, 0);
    f.read(reinterpret_cast<char *>(p.header.data()), std::streamsize(p.header.size()));
    if (size_t(f.gcount()) < META + METADATA_SIZE || !valid_header(p.header)) return false;
    uint32_t license = 0;
    for (size_t i = 0; i < 16; i++) license |= rd32(p.header.data() + 4 + 0x228 + 0x10 * i + 8);
    p.host_path = path;
    p.file = true;
    p.data = metadata(path, p.header, license, true);
    return true;
}

bool open_package(const std::filesystem::path &path, Package &p) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) return open_package_file(path, p);
    if (!std::filesystem::is_directory(path, ec)) return false;
    p.host_path = path;
    p.header_path = header_path_of(path);
    uint32_t license = 0;
    if (!read_header(p.header_path, p.header, license)) p.header.assign(HEADER_SIZE, 0);
    p.data = metadata(path, p.header, license);
    return true;
}

bool stfs_mode(Kernel &k) { return content(k).stfs && !content_root(k).empty(); }

// where an STFS package's working directory is while a title has it open
std::filesystem::path work_path(Kernel &k, const std::filesystem::path &package) {
    std::filesystem::path root = content_root(k);
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(package, root, ec);
    if (ec || rel.empty() || *rel.begin() == "..") rel = package.filename();
    return root.parent_path() / "content-work" / rel;
}

// the package file written from its working directory (the header as it is in memory)
bool export_stfs(const Package &p) {
    if (p.stfs_path.empty()) return true;
    std::string error;
    if (xf::write_stfs_package(p.stfs_path, p.header, p.host_path, error)) return true;
    std::fprintf(stderr, "xam: cannot write the package %s: %s (its files stay in %s)\n",
                 p.stfs_path.string().c_str(), error.c_str(), p.host_path.string().c_str());
    return false;
}

// A package file a title opens to write (a console-format STFS, not a read-only LIVE, PIRS or SVOD one) becomes
// a working directory: extracted from the file unless one is left from a run that did not close it.
void make_writable(Kernel &k, Package &p) {
    if (!p.file || !stfs_mode(k) || rd32(p.header.data()) != MAGIC_CON) return;
    std::string error;
    std::unique_ptr<xf::Package> package = xf::Package::open(p.host_path, error);
    if (!package || !package->is_stfs() || package->read_only()) return;
    std::filesystem::path work = work_path(k, p.host_path);
    std::error_code ec;
    if (!std::filesystem::is_directory(work, ec)) {
        std::filesystem::path partial = work;
        partial += ".new";
        std::filesystem::remove_all(partial, ec);
        std::filesystem::create_directories(work.parent_path(), ec);
        if (!xf::extract_package(*package, partial, error)) {
            std::fprintf(stderr, "xam: cannot open %s for writing: %s\n", p.host_path.string().c_str(), error.c_str());
            std::filesystem::remove_all(partial, ec);
            return; // read-only, as a package file is
        }
        std::filesystem::rename(partial, work, ec);
        if (ec) return;
    }
    p.stfs_path = p.host_path;
    p.host_path = work;
    p.file = false;
}

bool is_open(ContentState &s, std::string_view root) {
    return std::any_of(s.mounted.begin(), s.mounted.end(),
                       [&](const auto &p) { return equal_case(p->root_name, root); });
}

// ContentPackage::MountPackageAndRegister (the content lock held)
bool mount(Kernel &k, ContentState &s, std::unique_ptr<Package> p, const std::string &root) {
    p->device_path = "\\Device\\Content\\" + std::to_string(++s.next_device);
    std::shared_ptr<vfs::Device> device;
    if (p->file) {
        std::string error;
        device = vfs::make_package_device(p->device_path, p->host_path, error);
        if (!device) std::fprintf(stderr, "xam: %s: %s\n", p->host_path.string().c_str(), error.c_str());
    } else {
        device = vfs::make_host_device(p->device_path, p->host_path, false, true);
    }
    if (!device) return false;
    vfs::FileSystem &fs = file_system(k);
    if (!fs.add_link(root + ":", p->device_path)) {
        std::fprintf(stderr, "xam: the content root %s: is in use\n", root.c_str());
        return false;
    }
    fs.add_device(device);
    p->root_name = root;
    s.mounted.push_back(std::move(p));
    return true;
}

// the package's device and link gone, its header written (Xenia's
// ContentPackageDirectory and ContentPackage destructors)
void unmount(Kernel &k, Package &p) {
    vfs::FileSystem &fs = file_system(k);
    fs.remove_link(p.root_name + ":");
    write_header(p);
    fs.remove_device(p.device_path);
    if (!p.stfs_path.empty() && export_stfs(p)) {
        std::error_code ec;
        std::filesystem::remove_all(p.host_path, ec);
        p.stfs_path.clear();
    }
}

bool content_exists(Kernel &k, uint64_t xuid, const ContentData &d) {
    std::error_code ec;
    return std::filesystem::exists(package_path(k, xuid, d), ec);
}

// ContentManager::DeleteContent
uint32_t delete_content(Kernel &k, ContentState &s, uint64_t xuid, const ContentData &d) {
    ContentData wanted = d;
    wanted.title_id = running(k, d.title_id);
    for (const auto &p : s.mounted)
        if (p->data.same(wanted)) return X_ERROR_ACCESS_DENIED;
    std::error_code ec;
    std::filesystem::path path = package_path(k, xuid, d);
    if (!content_root(k).empty()) std::filesystem::remove_all(work_path(k, path), ec); // an STFS package's, if left
    return std::filesystem::remove_all(path, ec) > 0 ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
}

// ContentManager::ListContent (the hard drive)
std::vector<ContentData> list_content(Kernel &k, uint64_t xuid, uint32_t title, uint32_t type) {
    std::vector<ContentData> out;
    if (type == TYPE_PROFILE) return out; // profiles are Xenia's own
    std::error_code ec;
    for (std::filesystem::directory_iterator it(package_root(k, xuid, title, type), ec), end; !ec && it != end;
         it.increment(ec)) {
        Package p;
        if (open_package(it->path(), p) && valid_header(p.header)) out.push_back(p.data);
    }
    return out;
}

// ContentManager::ListContentODD: what the disc carries in its content
// directory
std::vector<ContentData> list_disc_content(Kernel &k, uint32_t title, uint32_t type) {
    std::vector<ContentData> out;
    vfs::FileSystem &fs = file_system(k);
    std::string resolved =
        fs.resolve_links("GAME:\\content\\" + hex(0, 16) + "\\" + hex(title, 8) + "\\" + hex(type, 8));
    std::lock_guard<std::recursive_mutex> l(fs.mutex);
    vfs::Entry *dir = fs.find(resolved);
    if (!dir) return out;
    for (const auto &child : dir->children) {
        ContentData d;
        d.device_id = DEVICE_ODD;
        d.content_type = type;
        d.display_name = widen(child->name);
        d.file_name = child->name;
        d.title_id = title;
        out.push_back(d);
    }
    return out;
}

// the dummy devices (xam_content_device.cc): the hard drive holds the
// content root; the disc
struct DeviceInfo {
    uint32_t id, type;
    uint64_t total, free;
    const char16_t *name;
};

bool device_info(Kernel &k, uint32_t id, DeviceInfo &out) {
    constexpr uint64_t GB = 1ull << 30;
    if (id == DEVICE_HDD) {
        out = {DEVICE_HDD, 1, 20 * GB, 10 * GB, u"Dummy HDD"};
        std::error_code ec;
        auto space = std::filesystem::space(content_root(k), ec);
        if (!ec) out.total = space.capacity, out.free = space.free;
        return true;
    }
    if (id == DEVICE_ODD) {
        out = {DEVICE_ODD, 4, 7 * GB, 0, u"Dummy ODD"};
        return true;
    }
    return false;
}

std::string guest_string(Kernel &k, uint32_t p) { return p ? reinterpret_cast<const char *>(k.host(p)) : ""; }

// xeXamContentCreate
uint32_t content_create(Kernel &k, uint32_t user_index, uint32_t root_ptr, uint32_t data_ptr, uint32_t data_size,
                        uint32_t flags, uint32_t disposition_ptr, uint32_t license_ptr, uint32_t ov) {
    uint64_t xuid = 0;
    if (user_index != USER_INDEX_NONE && user_index != USER_INDEX_ANY) {
        const User *u = user_at(k, user_index);
        if (!u) return X_ERROR_NO_SUCH_USER;
        xuid = u->xuid;
    }
    std::string root = guest_string(k, root_ptr);
    if (root.empty()) return X_ERROR_INVALID_NAME;
    ContentData d;
    if (!read_content_data(k, data_ptr, data_size, d)) return X_ERROR_INVALID_PARAMETER;
    if (root.ends_with(':')) root.pop_back();

    ContentState &s = content(k);
    {
        std::lock_guard<std::mutex> l(s.mutex);
        // the same package already under that name is fine
        for (const auto &p : s.mounted) {
            if (!equal_case(p->root_name, root)) continue;
            ContentData wanted = d;
            wanted.title_id = title_id(k);
            return p->data.same(wanted) ? X_ERROR_SUCCESS : X_ERROR_INVALID_PARAMETER;
        }
        // a link of that name that is no package
        if (file_system(k).has_link(root + ":")) return X_ERROR_INVALID_PARAMETER;
    }
    if (d.content_type == TYPE_MARKETPLACE) xuid = 0;
    if (ov && disposition_ptr) k.store32(disposition_ptr, 0);

    auto run = [&, root, d, xuid](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        enum { Unknown, Create, Open } disposition = Unknown;
        uint32_t r = X_ERROR_INVALID_PARAMETER;
        std::lock_guard<std::mutex> l(s.mutex);
        bool exists = content_exists(k, xuid, d);
        switch (flags & 0xF) {
        case 1: // create new
            if (exists)
                r = X_ERROR_ALREADY_EXISTS;
            else
                disposition = Create;
            break;
        case 2: // create always
            if (exists) delete_content(k, s, xuid, d);
            disposition = Create;
            break;
        case 3: // open existing
            if (!exists)
                r = X_ERROR_PATH_NOT_FOUND;
            else
                disposition = Open;
            break;
        case 4: // open always
            disposition = exists ? Open : Create;
            break;
        case 5: // truncate existing
            if (!exists) {
                r = X_ERROR_PATH_NOT_FOUND;
            } else {
                delete_content(k, s, xuid, d);
                disposition = Create;
            }
            break;
        default: std::fprintf(stderr, "xam: XamContentCreate: flags 0x%X\n", flags); break;
        }
        uint32_t license = 0;
        if (disposition != Unknown && is_open(s, root)) {
            r = X_ERROR_ALREADY_EXISTS;
        } else if (disposition == Create) {
            // ContentManager::CreateContent
            ContentData c = d;
            c.title_id = running(k, c.title_id);
            if (!c.xuid) c.xuid = xuid;
            c.content_size = CONTENT_DATA_SIZE; // as Xenia (the structure's size)
            auto p = std::make_unique<Package>();
            std::filesystem::path path = package_path(k, xuid, c);
            const bool as_stfs = stfs_mode(k);
            if (as_stfs) {
                p->stfs_path = path;
                p->host_path = work_path(k, path);
            } else {
                p->host_path = path;
                p->header_path = header_path_of(path);
            }
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                r = X_ERROR_ALREADY_EXISTS;
            } else {
                std::filesystem::create_directories(path.parent_path(), ec);
                p->header = new_header(k, c);
                p->data = c;
                p->data.device_id = DEVICE_HDD;
                bool ok = true;
                if (as_stfs) {
                    // a new package is on the host at once (it lists, it exists), and its working directory empty
                    std::filesystem::remove_all(p->host_path, ec);
                    std::filesystem::create_directories(p->host_path, ec);
                    ok = !ec && export_stfs(*p);
                } else {
                    ok = write_header(*p);
                }
                ok = ok && mount(k, s, std::move(p), root);
                r = ok ? X_ERROR_SUCCESS : X_ERROR_ACCESS_DENIED;
            }
        } else if (disposition == Open) {
            // ContentManager::OpenContent
            auto p = std::make_unique<Package>();
            if (!open_package(package_path(k, xuid, d), *p)) {
                r = X_ERROR_FILE_NOT_FOUND;
            } else {
                license = p->data.license_mask;
                make_writable(k, *p);
                r = mount(k, s, std::move(p), root) ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
            }
        }
        if (license_ptr && !(r & 0x80000000)) k.store32(license_ptr, license);
        extended_error = hresult_from_win32(r);
        length = uint32_t(disposition);
        if (disposition_ptr) k.store32(disposition_ptr, uint32_t(disposition));
        if (r && ov) r = X_ERROR_FUNCTION_FAILED;
        return r;
    };
    return run_or_defer(k, ov, run);
}

} // namespace

void set_content_format(Kernel &k, ContentFormat format) { content(k).stfs = format == ContentFormat::Stfs; }

// (user_index, root_name, XCONTENT_DATA *, flags, disposition_ptr,
// license_mask_ptr, cache_size, content_size, overlapped)
XF_EXPORT(xam, XamContentCreateEx) {
    uint32_t data = arg(k, c, 2);
    if (!k.host(data + 0x108)[0]) return result(c, X_ERROR_INVALID_NAME);
    result(c, content_create(k, arg(k, c, 0), arg(k, c, 1), data, CONTENT_DATA_SIZE, arg(k, c, 3), arg(k, c, 4),
                             arg(k, c, 5), arg(k, c, 8)));
}

// (user_index, root_name, XCONTENT_DATA *, flags, disposition_ptr,
// license_mask_ptr, overlapped): XamContentCreateEx with no cache
XF_EXPORT(xam, XamContentCreate) {
    uint32_t data = arg(k, c, 2);
    if (!k.host(data + 0x108)[0]) return result(c, X_ERROR_INVALID_NAME);
    result(c, content_create(k, arg(k, c, 0), arg(k, c, 1), data, CONTENT_DATA_SIZE, arg(k, c, 3), arg(k, c, 4),
                             arg(k, c, 5), arg(k, c, 6)));
}

// (root_name, XCONTENT_DATA_INTERNAL *, flags, disposition_ptr,
// license_mask_ptr, cache_size, content_size (64 bits), overlapped): for no
// user, the XUID and title the structure names
XF_EXPORT(xam, XamContentCreateInternal) {
    uint32_t data = arg(k, c, 1);
    if (!k.host(data + 0x108)[0]) return result(c, X_ERROR_INVALID_NAME);
    result(c, content_create(k, USER_INDEX_NONE, arg(k, c, 0), data, INTERNAL_SIZE, arg(k, c, 2), arg(k, c, 3),
                             arg(k, c, 4), arg(k, c, 7)));
}

// (user_index, root_name, path, flags, disposition_ptr, license_mask_ptr,
// overlapped): Xenia's stub, no such file
// A null or empty root name is ERROR_INVALID_NAME on the console's XAM for
// this, Close and Flush (xfxam; it checks the name before anything else);
// Xenia has no check.
XF_EXPORT(xam, XamContentOpenFile) {
    result(c, guest_string(k, arg(k, c, 1)).empty() ? X_ERROR_INVALID_NAME : X_ERROR_FILE_NOT_FOUND);
}

// (root_name, overlapped)
XF_EXPORT(xam, XamContentClose) {
    std::string root = guest_string(k, arg(k, c, 0));
    if (root.empty()) return result(c, X_ERROR_INVALID_NAME);
    uint32_t ov = arg(k, c, 1);
    if (root.ends_with(':')) root.pop_back();
    ContentState &s = content(k);
    uint32_t r = X_ERROR_FILE_NOT_FOUND;
    {
        std::lock_guard<std::mutex> l(s.mutex);
        auto it = std::find_if(s.mounted.begin(), s.mounted.end(),
                               [&](const auto &p) { return equal_case(p->root_name, root); });
        if (it != s.mounted.end()) {
            unmount(k, **it);
            s.mounted.erase(it);
            r = X_ERROR_SUCCESS;
        }
    }
    if (ov) {
        complete_overlapped_immediate_ex(k, ov, r, hresult_from_win32(r), 0);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, r);
}

// (root_name, overlapped): the header written now, and an STFS package's file
XF_EXPORT(xam, XamContentFlush) {
    std::string root = guest_string(k, arg(k, c, 0));
    if (root.empty()) return result(c, X_ERROR_INVALID_NAME);
    uint32_t ov = arg(k, c, 1);
    if (root.ends_with(':')) root.pop_back();
    ContentState &s = content(k);
    {
        std::lock_guard<std::mutex> l(s.mutex);
        auto it = std::find_if(s.mounted.begin(), s.mounted.end(),
                               [&](const auto &p) { return equal_case(p->root_name, root); });
        if (it == s.mounted.end()) return result(c, X_STATUS_INVALID_PARAMETER);
        write_header(**it);
        export_stfs(**it); // the package file as it is now; the working directory stays
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, X_ERROR_SUCCESS);
}

namespace {
// xeXamContentDelete: the content data read by its size, then the user
uint32_t content_delete(Kernel &k, uint32_t index, uint32_t data, uint32_t data_size, uint32_t ov) {
    ContentData d;
    if (!read_content_data(k, data, data_size, d)) return X_ERROR_INVALID_PARAMETER;
    uint64_t xuid = 0;
    if (index != USER_INDEX_NONE) {
        const User *u = user_at(k, index);
        if (!u) return X_ERROR_NO_SUCH_USER;
        xuid = u->xuid;
    }
    ContentState &s = content(k);
    uint32_t r;
    {
        std::lock_guard<std::mutex> l(s.mutex);
        r = delete_content(k, s, xuid, d);
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, r);
        return X_ERROR_IO_PENDING;
    }
    return r;
}
} // namespace

// (user_index, XCONTENT_DATA *, overlapped)
XF_EXPORT(xam, XamContentDelete) {
    result(c, content_delete(k, arg(k, c, 0), arg(k, c, 1), CONTENT_DATA_SIZE, arg(k, c, 2)));
}

// (XCONTENT_DATA_INTERNAL *, overlapped): XAM's internal form, for no user
XF_EXPORT(xam, XamContentDeleteInternal) {
    result(c, content_delete(k, USER_INDEX_NONE, arg(k, c, 0), INTERNAL_SIZE, arg(k, c, 1)));
}

namespace {
// XamContentCreateEnumeratorInternal: the XUID's and the common content on
// the hard drive (of the title, 0 the running one), and the disc's
uint32_t create_content_enumerator(Kernel &k, uint64_t xuid, uint32_t device_id, uint32_t type, uint32_t title,
                                   uint32_t flags, uint32_t items, uint32_t size_ptr, uint32_t handle_out) {
    DeviceInfo info{};
    bool known = device_id && device_info(k, device_id, info);
    if ((device_id && !known) || !handle_out) {
        if (size_ptr) k.store32(size_ptr, 0);
        return X_E_INVALIDARG;
    }
    if (size_ptr) k.store32(size_ptr, CONTENT_DATA_SIZE * items);
    Ref<Enumerator> e = make_enumerator(k, USER_INDEX_ANY, 0xFE, 0x20005, 0x20007, 0, items, CONTENT_DATA_SIZE, 0x98);
    if (!e) return X_STATUS_NO_MEMORY;
    if (!title) title = title_id(k);
    std::vector<ContentData> found;
    if (!known || info.id == DEVICE_HDD) {
        std::vector<uint64_t> xuids;
        if (xuid) xuids.push_back(xuid);
        if (!xuid || !(flags & 0x1000)) xuids.push_back(0); // kExcludeCommon
        for (uint64_t x : xuids) {
            auto list = list_content(k, x, title, type);
            found.insert(found.end(), list.begin(), list.end());
        }
        found.erase(std::unique(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.same(b); }),
                    found.end());
    }
    if (!known || info.id == DEVICE_ODD) {
        auto list = list_disc_content(k, title, type);
        found.insert(found.end(), list.begin(), list.end());
    }
    for (const ContentData &d : found) write_content_data(e->append(), d);
    k.store32(handle_out, e->handle());
    return X_ERROR_SUCCESS;
}
} // namespace

// (xuid (64 bits), device_id, content_type, title_id, flags,
// items_per_enumerate, buffer_size_ptr, handle_out)
XF_EXPORT(xam, XamContentCreateEnumeratorInternal) {
    result(c, create_content_enumerator(k, arg64(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4),
                                        arg(k, c, 5), arg(k, c, 6), arg(k, c, 7)));
}

// (user_index, device_id, content_type, flags, items_per_enumerate,
// buffer_size_ptr, handle_out): the user's (a user slot) and the common
// content, of the running title
XF_EXPORT(xam, XamContentCreateEnumerator) {
    uint32_t index = arg(k, c, 0);
    uint64_t xuid = 0;
    if (index < 4) {
        const User *u = user_at(k, index);
        if (!u) return result(c, X_ERROR_NO_SUCH_USER);
        xuid = u->xuid;
    }
    result(c, create_content_enumerator(k, xuid, arg(k, c, 1), arg(k, c, 2), 0, arg(k, c, 3), arg(k, c, 4),
                                        arg(k, c, 5), arg(k, c, 6)));
}

// (xuid, device_id, content_type, title_id, handle_out), Xenia's
// xam_content_aggregate.cc: the hard drive's content of the title (0: the
// running one) and of the alternate titles its executable names, one item
// a call, XCONTENT_DATA_AGGREGATE (0x148; XCONTENT_DATA, the XUID at 0x138,
// the title at 0x140) or, for content type 0 (all), XCONTENT_DATA_INTERNAL
XF_EXPORT(xam, XamContentAggregateCreateEnumerator) {
    uint64_t xuid = arg64(k, c, 0);
    uint32_t device_id = arg(k, c, 1), type = arg(k, c, 2), title = arg(k, c, 3), handle_out = arg(k, c, 4);
    DeviceInfo info{};
    bool known = device_id && device_info(k, device_id, info);
    if ((device_id && !known) || !handle_out) return result(c, X_E_INVALIDARG);
    uint32_t item_size = type == 0 ? INTERNAL_SIZE : AGGREGATE_SIZE;
    // X_KENUMERATOR_CONTENT_AGGREGATE after it: Xenia's object signature and
    // the handle
    Ref<Enumerator> e = make_enumerator(k, USER_INDEX_ANY, 0xFE, 0x2000E, 0x20010, 0, 1, item_size, 8);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    k.store32(e->guest + 0x18, header::SIGNATURE);
    k.store32(e->guest + 0x1C, e->handle());
    if (!known || info.id == DEVICE_HDD) {
        std::vector<uint32_t> titles{title ? title : title_id(k)};
        if (const Xex *xex = k.xex()) {
            for (const auto &[key, offset] : xex->opt_headers) {
                if (key != xex_key::ALTERNATE_TITLE_IDS || offset + 4 > xex->header.size()) continue;
                const uint8_t *p = xex->header.data() + offset;
                uint32_t size = std::min<uint32_t>(rd32(p), uint32_t(xex->header.size() - offset));
                for (uint32_t at = 4; at + 4 <= size; at += 4) titles.push_back(rd32(p + at));
            }
        }
        for (uint32_t t : titles) {
            for (const ContentData &d : list_content(k, xuid == ~0ull ? 0 : xuid, t, type)) {
                uint8_t *p = e->append();
                std::memset(p, 0, item_size);
                write_content_data(p, d);
                wr64(p + 0x138, d.xuid);
                wr32(p + 0x140, d.title_id);
                if (item_size == INTERNAL_SIZE) {
                    wr32(p + 0x144, d.license_mask);
                    wr64(p + 0x148, d.content_size);
                }
            }
        }
    }
    k.store32(handle_out, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// (user_index, XCONTENT_DATA *, is_creator_ptr, creator_xuid_ptr,
// overlapped): the user made every save
XF_EXPORT(xam, XamContentGetCreator) {
    uint32_t index = arg(k, c, 0), is_creator_ptr = arg(k, c, 2), creator_ptr = arg(k, c, 3), ov = arg(k, c, 4);
    if (!is_creator_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    const User *u = user_at(k, index);
    if (!u) return result(c, X_ERROR_NO_SUCH_USER);
    ContentData d;
    read_content_data(k, arg(k, c, 1), CONTENT_DATA_SIZE, d);
    uint64_t xuid = u->xuid;
    auto run = [&, d, xuid](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        uint32_t r = X_ERROR_SUCCESS;
        if (content_exists(k, xuid, d)) {
            bool save = d.content_type == TYPE_SAVED_GAME;
            k.store32(is_creator_ptr, save ? 1 : 0);
            if (creator_ptr) k.store64(creator_ptr, save ? xuid : 0);
        } else {
            r = X_ERROR_PATH_NOT_FOUND;
        }
        extended_error = hresult_from_win32(r);
        length = 0;
        if (r && ov) r = X_ERROR_FUNCTION_FAILED;
        return r;
    };
    result(c, run_or_defer(k, ov, run));
}

// (mask_ptr, overlapped): no licenses (Xenia's license_mask 0), for a
// title that was mounted
XF_EXPORT(xam, XamContentGetLicenseMask) {
    uint32_t mask_ptr = arg(k, c, 0), ov = arg(k, c, 1);
    if (!mask_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    auto run = [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        uint32_t r = X_ERROR_FUNCTION_FAILED;
        if (file_system(k).deployment != DEPLOYMENT_OTHER) {
            k.store32(mask_ptr, 0);
            r = X_ERROR_SUCCESS;
        }
        extended_error = hresult_from_win32(r);
        length = 0;
        return r;
    };
    result(c, run_or_defer(k, ov, run));
}

// (device_id, overlapped)
XF_EXPORT(xam, XamContentGetDeviceState) {
    uint32_t device_id = arg(k, c, 0), ov = arg(k, c, 1);
    DeviceInfo info;
    if (!device_info(k, device_id, info)) {
        if (!ov) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
        complete_overlapped_immediate_ex(k, ov, X_ERROR_FUNCTION_FAILED,
                                         hresult_from_win32(X_ERROR_DEVICE_NOT_CONNECTED), 0);
        return result(c, X_ERROR_IO_PENDING);
    }
    if (!ov) return result(c, X_ERROR_SUCCESS);
    complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
    result(c, X_ERROR_IO_PENDING);
}

namespace {
constexpr uint32_t DEVICE_DATA_SIZE = 0x50; // X_CONTENT_DEVICE_DATA

// X_CONTENT_DEVICE_DATA: id, type, total and free bytes, the name (28
// characters with its terminator)
void fill_device_data(uint8_t *p, const DeviceInfo &info, std::u16string_view name) {
    std::memset(p, 0, DEVICE_DATA_SIZE);
    wr32(p + 0x00, info.id);
    wr32(p + 0x04, info.type);
    wr64(p + 0x08, info.total);
    wr64(p + 0x10, info.free);
    wr_u16(p + 0x18, name, 28);
}

// XDeviceTypeMap: the name a device type is shown with
std::u16string_view localized_device_name(uint32_t type) {
    switch (type) {
    case 1: return u"Hard Drive";
    case 2: return u"Memory Unit";
    case 3: return u"System";
    case 4: return u"Optical Drive";
    case 8: return u"Transfer Cable";
    case 15: return u"Sapphire";
    case 16: return u"USB Storage";
    default: return u"Storage Device";
    }
}

// copy_truncating: as much as fits with its terminator
void put_string(Kernel &k, uint32_t address, std::string_view s, uint32_t count) {
    if (!count || !address) return;
    size_t n = std::min<size_t>(s.size(), count - 1);
    std::memcpy(k.host(address), s.data(), n);
    k.host(address)[n] = 0;
}
} // namespace

// (device_id, X_CONTENT_DEVICE_DATA *)
XF_EXPORT(xam, XamContentGetDeviceData) {
    uint32_t device_id = arg(k, c, 0), out = arg(k, c, 1);
    DeviceInfo info;
    if (!device_info(k, device_id, info)) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    fill_device_data(k.host(out), info, info.name);
    result(c, X_ERROR_SUCCESS);
}

// (device_id, X_CONTENT_DEVICE_DATA *): the name by the device's type
XF_EXPORT(xam, XamContentGetLocalizedDeviceData) {
    uint32_t device_id = arg(k, c, 0), out = arg(k, c, 1);
    DeviceInfo info;
    if (!device_info(k, device_id, info)) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    fill_device_data(k.host(out), info, localized_device_name(info.type));
    result(c, X_ERROR_SUCCESS);
}

// (device_id, name_buffer, name_capacity): the device's name, if it fits
XF_EXPORT(xam, XamContentGetDeviceName) {
    uint32_t device_id = arg(k, c, 0), buffer = arg(k, c, 1), capacity = arg(k, c, 2);
    DeviceInfo info;
    if (!device_info(k, device_id, info)) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    std::u16string_view name = info.name;
    if (capacity < name.size() + 1) return result(c, X_ERROR_INSUFFICIENT_BUFFER);
    put_u16(k, buffer, name, capacity);
    result(c, X_ERROR_SUCCESS);
}

// (device_id, path, path_size, append_backslash): the hard drive's volume,
// "hdd0\" (the backslash always there, as Xenia's)
XF_EXPORT(xam, XamContentGetDeviceVolumePath) {
    if (arg(k, c, 0) != DEVICE_HDD) return result(c, X_ERROR_FUNCTION_FAILED);
    put_string(k, arg(k, c, 1), "hdd0\\", arg(k, c, 2));
    result(c, X_ERROR_SUCCESS);
}

// (content_type, content_flags, max_count, buffer_size_ptr, handle_out): the
// devices, the disc left out with kExcludeReadOnlyDevices
XF_EXPORT(xam, XamContentCreateDeviceEnumerator) {
    uint32_t flags = arg(k, c, 1), max_count = arg(k, c, 2), size_ptr = arg(k, c, 3), handle_out = arg(k, c, 4);
    if (size_ptr) k.store32(size_ptr, DEVICE_DATA_SIZE * max_count);
    Ref<Enumerator> e = make_enumerator(k, USER_INDEX_NONE, 0xFE, 0x2000A, 0x20009, 0, max_count, DEVICE_DATA_SIZE, 0);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    for (uint32_t id : {DEVICE_HDD, DEVICE_ODD}) {
        DeviceInfo info;
        device_info(k, id, info);
        if (info.type == 4 && (flags & 0x80000000)) continue;
        fill_device_data(e->append(), info, info.name);
    }
    if (handle_out) k.store32(handle_out, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// ---- resolving a package's path (xeXamContentResolve) ----

namespace {
// \Device\Harddisk0\Partition1\Content\ or D:\content\, then
// <XUID>\<TITLE>\<TYPE>\<file name>: the user's XUID for a save, else 0; the
// running title. A root name is not supported (as Xenia's).
uint32_t content_resolve(Kernel &k, uint32_t index, uint32_t data, uint32_t data_size, uint32_t path,
                         uint32_t path_size, uint32_t root_name, uint32_t ov) {
    auto run = [&k, index, data, data_size, path, path_size, root_name](uint32_t &, uint32_t &) -> uint32_t {
        ContentData d;
        if (!read_content_data(k, data, data_size, d)) return X_ERROR_INVALID_PARAMETER;
        uint64_t xuid = 0;
        if (index < 4) {
            const User *u = user_at(k, index);
            if (u && d.content_type == TYPE_SAVED_GAME) xuid = u->xuid;
        }
        if (root_name) return X_ERROR_INVALID_PARAMETER;
        std::string root;
        if (d.device_id == DEVICE_HDD)
            root = "\\Device\\Harddisk0\\Partition1\\Content\\";
        else if (d.device_id == DEVICE_ODD)
            root = "D:\\content\\";
        else
            return X_ERROR_INVALID_PARAMETER;
        std::string full =
            root + hex(xuid, 16) + "\\" + hex(title_id(k), 8) + "\\" + hex(d.content_type, 8) + "\\" + d.file_name;
        put_string(k, path, full, path_size);
        return X_ERROR_SUCCESS;
    };
    return run_or_defer(k, ov, run);
}
} // namespace

// (user_index, XCONTENT_DATA *, path, path_size, create_directory,
// root_name, overlapped)
XF_EXPORT(xam, XamContentResolve) {
    result(c, content_resolve(k, arg(k, c, 0), arg(k, c, 1), CONTENT_DATA_SIZE, arg(k, c, 2), arg(k, c, 3),
                              arg(k, c, 5), arg(k, c, 6)));
}

// (XCONTENT_DATA_INTERNAL *, path, path_size, create_directory, root_name,
// overlapped): for no user
XF_EXPORT(xam, XamContentResolveInternal) {
    result(c, content_resolve(k, USER_INDEX_NONE, arg(k, c, 0), INTERNAL_SIZE, arg(k, c, 1), arg(k, c, 2), arg(k, c, 4),
                              arg(k, c, 5)));
}

// ---- thumbnails, metadata, attributes ----

namespace {

uint32_t max_thumbnail(const std::vector<uint8_t> &h) {
    return rd32(h.data() + META + M_VERSION) == 1 ? THUMBNAIL_V1 : THUMBNAIL_V2;
}

// ContentPackageDirectory::GetThumbnail: a __thumbnail.png left in the
// package is taken into its header (and removed), else the header's own;
// changed when the header took one
uint32_t package_thumbnail(Package &p, std::vector<uint8_t> &out, bool &changed) {
    uint8_t *m = p.header.data() + META;
    std::filesystem::path png = p.host_path / "__thumbnail.png";
    std::ifstream f(png, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        f.close();
        uint32_t size = std::min<uint32_t>(uint32_t(out.size()), max_thumbnail(p.header));
        std::memcpy(m + M_THUMBNAIL, out.data(), size);
        wr32(m + M_THUMBNAIL_SIZE, size);
        std::error_code ec;
        std::filesystem::remove(png, ec);
        changed = true;
        return X_ERROR_SUCCESS;
    }
    uint32_t size = std::min(rd32(m + M_THUMBNAIL_SIZE), max_thumbnail(p.header));
    if (!size) return X_ERROR_FILE_NOT_FOUND;
    out.assign(m + M_THUMBNAIL, m + M_THUMBNAIL + size);
    return X_ERROR_SUCCESS;
}

// ContentPackageDirectory::SetThumbnail
uint32_t set_package_thumbnail(Package &p, const uint8_t *data, uint32_t size) {
    if (size > max_thumbnail(p.header)) return X_ERROR_INSUFFICIENT_BUFFER;
    uint8_t *m = p.header.data() + META;
    if (size) std::memcpy(m + M_THUMBNAIL, data, size);
    wr32(m + M_THUMBNAIL_SIZE, size);
    return X_ERROR_SUCCESS;
}

// The package content data names, the content lock held: the one mounted
// (its header written at its close), else the one on the host opened into
// `opened` (its header written by the caller). nullptr: none.
Package *find_package(Kernel &k, ContentState &s, uint64_t xuid, const ContentData &d, Package &opened) {
    ContentData wanted = d;
    wanted.title_id = running(k, d.title_id);
    for (const auto &p : s.mounted)
        if (p->data.same(wanted)) return p.get();
    if (!content_exists(k, xuid, d) || !open_package(package_path(k, xuid, d), opened)) return nullptr;
    return &opened;
}

} // namespace

// (user_index, XCONTENT_DATA *, buffer, buffer_size_ptr, overlapped): the
// thumbnail's size, and the thumbnail when there is a buffer for it
XF_EXPORT(xam, XamContentGetThumbnail) {
    uint32_t index = arg(k, c, 0), data = arg(k, c, 1), buffer = arg(k, c, 2), size_ptr = arg(k, c, 3);
    uint32_t ov = arg(k, c, 4);
    const User *u = user_at(k, index);
    if (!u) return result(c, X_ERROR_NO_SUCH_USER);
    if (!size_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    uint32_t buffer_size = k.load32(size_ptr);
    ContentData d;
    read_content_data(k, data, CONTENT_DATA_SIZE, d);
    std::vector<uint8_t> thumbnail;
    uint32_t r;
    {
        ContentState &s = content(k);
        std::lock_guard<std::mutex> l(s.mutex);
        Package opened;
        Package *p = find_package(k, s, u->xuid, d, opened);
        bool changed = false;
        r = p ? package_thumbnail(*p, thumbnail, changed) : X_ERROR_FILE_NOT_FOUND;
        if (changed && p == &opened) write_header(opened);
    }
    k.store32(size_ptr, uint32_t(thumbnail.size()));
    if (r == X_ERROR_SUCCESS && buffer) {
        if (buffer_size < thumbnail.size())
            r = X_ERROR_INSUFFICIENT_BUFFER;
        else
            std::memcpy(k.host(buffer), thumbnail.data(), thumbnail.size());
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, r);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, r);
}

namespace {
// xeXamContentSetThumbnail: the PNG kept in the package's header
uint32_t content_set_thumbnail(Kernel &k, uint32_t index, uint32_t data, uint32_t data_size, uint32_t buffer,
                               uint32_t buffer_size, uint32_t ov) {
    const User *u = user_at(k, index);
    if (!u) return X_ERROR_NO_SUCH_USER;
    if (buffer_size > THUMBNAIL_V2) return X_E_INVALIDARG;
    ContentData d;
    if (!read_content_data(k, data, data_size, d)) return X_ERROR_INVALID_PARAMETER;
    if (data_size != INTERNAL_SIZE) d.title_id = title_id(k);
    uint32_t r;
    {
        ContentState &s = content(k);
        std::lock_guard<std::mutex> l(s.mutex);
        Package opened;
        Package *p = find_package(k, s, u->xuid, d, opened);
        r = p ? set_package_thumbnail(*p, buffer_size ? k.host(buffer) : nullptr, buffer_size) : X_ERROR_FILE_NOT_FOUND;
        if (r == X_ERROR_SUCCESS && p == &opened) write_header(opened);
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, r);
        return X_ERROR_IO_PENDING;
    }
    return r;
}
} // namespace

// (user_index, XCONTENT_DATA *, buffer, buffer_size, overlapped)
XF_EXPORT(xam, XamContentSetThumbnail) {
    result(c, content_set_thumbnail(k, arg(k, c, 0), arg(k, c, 1), CONTENT_DATA_SIZE, arg(k, c, 2), arg(k, c, 3),
                                    arg(k, c, 4)));
}

// (XCONTENT_DATA_INTERNAL *, buffer, buffer_size): for no user, which
// Xenia's lookup finds no profile for: X_ERROR_NO_SUCH_USER
XF_EXPORT(xam, XamContentSetThumbnailInternal) {
    result(c, content_set_thumbnail(k, USER_INDEX_NONE, arg(k, c, 0), INTERNAL_SIZE, arg(k, c, 1), arg(k, c, 2), 0));
}

// (XCONTENT_DATA_INTERNAL *, XContentMetadata *, overlapped): the package's
// metadata (0x93D6 bytes)
XF_EXPORT(xam, XamContentGetMetaDataInternal) {
    uint32_t data = arg(k, c, 0), out = arg(k, c, 1), ov = arg(k, c, 2);
    ContentData d;
    read_content_data(k, data, INTERNAL_SIZE, d);
    {
        ContentState &s = content(k);
        std::lock_guard<std::mutex> l(s.mutex);
        Package opened;
        Package *p = find_package(k, s, 0, d, opened);
        if (!p) return result(c, X_ERROR_FUNCTION_FAILED);
        if (out) std::memcpy(k.host(out), p->header.data() + META, METADATA_SIZE);
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, 0);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, X_ERROR_SUCCESS);
}

// (XCONTENT_DATA_INTERNAL *, X_FILE_ATTRIBUTE_DATA *, overlapped): the
// package's directory's times (each its last write here), no attributes,
// size 0
XF_EXPORT(xam, XamContentGetAttributesInternal) {
    uint32_t data = arg(k, c, 0), out = arg(k, c, 1), ov = arg(k, c, 2);
    ContentData d;
    read_content_data(k, data, INTERNAL_SIZE, d);
    std::filesystem::path path = package_path(k, 0, d);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return result(c, X_ERROR_FUNCTION_FAILED);
    auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return result(c, X_ERROR_FUNCTION_FAILED);
    uint64_t size = std::filesystem::is_directory(path, ec) ? 0 : std::filesystem::file_size(path, ec);
    auto since_epoch = std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch();
    uint64_t time =
        vfs::UNIX_EPOCH_FILETIME +
        uint64_t(
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(since_epoch).count());
    if (out) {
        uint8_t *p = k.host(out);
        std::memset(p, 0, 0x24);
        for (uint32_t at : {0x4u, 0xCu, 0x14u}) wr32(p + at, uint32_t(time >> 32)), wr32(p + at + 4, uint32_t(time));
        wr32(p + 0x1C, uint32_t(size >> 32));
        wr32(p + 0x20, uint32_t(size));
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, 0);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, X_ERROR_SUCCESS);
}

// (XContentMetadata *, display_name, display_name_size, description,
// description_size): in the console's language (XConfig's), from the
// metadata's twelve languages (the last three from version 2)
XF_EXPORT(xam, XamContentGetLocalizedString) {
    uint32_t metadata = arg(k, c, 0), name = arg(k, c, 1), name_size = arg(k, c, 2);
    uint32_t description = arg(k, c, 3), description_size = arg(k, c, 4);
    if (!metadata) return result(c, X_ERROR_INVALID_PARAMETER);
    uint32_t language = config_setting32(k, 3, 0x09, 1); // XCONFIG_USER_LANGUAGE
    uint32_t version = k.load32(metadata + M_VERSION);
    // the slot: language - 1 (an invalid language: Xenia's 1 for the name),
    // English's past the twelfth
    auto text = [&](size_t base, size_t base_ex, uint32_t slot) -> std::u16string {
        if (slot >= 12) slot = 0;
        if (slot < 9) return guest_u16(k, metadata + uint32_t(base + 0x100 * slot), 128);
        if (version >= 2) return guest_u16(k, metadata + uint32_t(base_ex + 0x100 * (slot - 9)), 128);
        return u"";
    };
    if (name) put_u16(k, name, text(M_DISPLAY_NAME, M_DISPLAY_NAME_EX, language ? language - 1 : 1), name_size);
    if (description) put_u16(k, description, text(M_DESCRIPTION, M_DESCRIPTION_EX, language - 1), description_size);
    result(c, X_ERROR_SUCCESS);
}

// (callback): kept, as Xenia keeps it (no content changes to report)
XF_EXPORT(xam, XamContentRegisterChangeCallback) {
    ContentState &s = content(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.change_callback = arg(k, c, 0);
}

// ---- launching an image (xeXamContentLaunchImage) ----

namespace {
// The image (a path, or the disc's package the content data names) must
// exist; then the run ends with the launch, as XamLoaderLaunchTitle's does
// (Xenia extracts the image and asks to be restarted).
uint32_t launch_image(Kernel &k, uint32_t image_location, uint32_t data, uint32_t data_size, uint32_t xex_path) {
    std::string image;
    if (!image_location) {
        ContentData d;
        if (!data || !read_content_data(k, data, data_size, d)) return X_ERROR_INVALID_PARAMETER;
        image = "GAME:\\Content\\0000000000000000\\" + hex(running(k, d.title_id), 8) + "\\" + hex(d.content_type, 8) +
                "\\" + d.file_name;
    } else {
        image = guest_string(k, image_location);
    }
    if (!file_system(k).resolve(image)) return 0xC000000F; // X_STATUS_NO_SUCH_FILE
    exit_title(k, true, image + "\\" + guest_string(k, xex_path), 0, 0,
               "the title launches " + guest_string(k, xex_path) + " from " + image + " (XamContentLaunchImage)");
}
} // namespace

// (user_index, XCONTENT_DATA *, xex_path)
XF_EXPORT(xam, XamContentLaunchImage) { result(c, launch_image(k, 0, arg(k, c, 1), CONTENT_DATA_SIZE, arg(k, c, 2))); }
// (XCONTENT_DATA_INTERNAL *, xex_path)
XF_EXPORT(xam, XamContentLaunchImageInternal) {
    result(c, launch_image(k, 0, arg(k, c, 0), INTERNAL_SIZE, arg(k, c, 1)));
}
// (XCONTENT_DATA_INTERNAL *, xex_path, flags)
XF_EXPORT(xam, XamContentLaunchImageInternalEx) {
    result(c, launch_image(k, 0, arg(k, c, 0), INTERNAL_SIZE, arg(k, c, 1)));
}
// (image_location, xex_name)
XF_EXPORT(xam, XamContentLaunchImageFromFileInternal) { result(c, launch_image(k, arg(k, c, 0), 0, 0, arg(k, c, 1))); }

// ---- the disc ----

// (disc_number, X_KEVENT *completion, X_SWAPDISC_ERROR_MESSAGE *): the disc
// in the drive is set at once; another ends the run, as there is no other
// disc to swap to (Xenia asks the user for one and mounts it at
// \Device\LauncherData)
XF_EXPORT(xam, XamSwapDisc) {
    uint32_t disc = arg(k, c, 0), event = arg(k, c, 1), message = arg(k, c, 2);
    XexExecutionInfo info = k.xex() ? k.xex()->execution : XexExecutionInfo{};
    if (info.disc_number > info.disc_count) return result(c, X_ERROR_INVALID_PARAMETER);
    if (info.disc_number == disc) {
        Objects &o = objects(k);
        if (Ref<Dispatcher> d = event ? o.native(event) : Ref<Dispatcher>()) {
            if (Event *e = dynamic_cast<Event *>(d.get())) {
                std::lock_guard<std::mutex> l(o.lock);
                e->set_signal_state(1);
                o.signaled(*e);
            }
        }
        return result(c, X_ERROR_SUCCESS);
    }
    std::u16string text = message ? guest_u16(k, k.load32(message + 4), 0x400) : u"";
    std::string why = "the title asks for disc " + std::to_string(disc) + " (XamSwapDisc";
    if (!text.empty()) {
        why += ": \"";
        for (char16_t ch : text) why += ch < 0x80 ? char(ch) : '?';
        why += "\"";
    }
    end_title(k, why + "); there is no other disc to swap to");
}

// X_DVD_TRAY_STATE: closed with the game's disc in, else open (Xenia's
// state as it starts, whatever the title came on)
XF_EXPORT(xam, XamLoaderGetDvdTrayState) { result(c, file_system(k).deployment == 0 ? 2 : 0); }

// (media_type_ptr, title_id_ptr, unknown_ptr): an Xbox 360 game disc, no
// title
XF_EXPORT(xam, XamLoaderGetMediaInfoEx) {
    if (uint32_t p = arg(k, c, 0)) k.store32(p, 1); // XBOX_360_GAME_DISC
    if (uint32_t p = arg(k, c, 1)) k.store32(p, 0);
    if (uint32_t p = arg(k, c, 2)) k.store32(p, 0);
    result(c, X_ERROR_SUCCESS); // the console's XAM answers 0 (xfxam); r3 was left as the argument
}

// (media_type_ptr, title_id_ptr)
XF_EXPORT(xam, XamLoaderGetMediaInfo) {
    if (uint32_t p = arg(k, c, 0)) k.store32(p, 1);
    if (uint32_t p = arg(k, c, 1)) k.store32(p, 0);
    result(c, X_ERROR_SUCCESS);
}

} // namespace xf::kernel
