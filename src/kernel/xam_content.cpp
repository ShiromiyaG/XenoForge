/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's content: saves and other packages, as directories in Xenia's layout
// under the content root, so the two share them:
//
//   content/<XUID>/<TITLE ID>/<TYPE>/<file name>/...          the package
//   content/<XUID>/<TITLE ID>/Headers/<TYPE>/<file name>.header
//
// (16, 8 and 8 uppercase hex digits; XUID 0 for content of no profile, the
// marketplace's always). The header file is Xenia's XContentContainerHeader
// (the STFS header's CON magic, its metadata, then Xenia's 42-byte file
// name), rounded up to 4 KB. A package opened is a host directory device at
// \Device\Content\<N> under the title's root name ("save:").
//
// Translated from Xenia (BSD; see LICENSE-xenia): xam_content.cc,
// xam_content_device.cc, content_manager.cc, xcontent/xcontent.h,
// xcontent_package.cc and xcontent_package_directory.cc, xenumerator.cc's
// ContentEnumerator; checked against them. Where it differs, and why:
//   - no SPA is read from the executable, so a new package's header has its
//     display name in the English slot and no title name or thumbnails
//     (Xenia puts the name in the SPA's default language and takes the
//     title's name and icon from it);
//   - STFS package files (a console's own saves) are not mounted: only the
//     directory packages Xenia writes, and publisher content is looked up
//     under the title's own ID only;
//   - closing a package leaves the files still open in it usable (Xenia
//     releases a handle of each);
//   - the hard drive's size is the content root's volume's, the disc's
//     Xenia's dummy one.

#include "xam.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

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
constexpr size_t M_DISPLAY_NAME_EX = 0x50D6;

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
    if (size == AGGREGATE_SIZE) {
        d.xuid = k.load64(p + 0x134);
        d.title_id = k.load32(p + 0x13C);
    } else if (size == INTERNAL_SIZE) {
        d.xuid = k.load64(p + 0x138);
        d.title_id = k.load32(p + 0x140);
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
};

struct ContentState {
    std::mutex mutex;
    std::vector<std::unique_ptr<Package>> mounted;
    uint32_t next_device = 0;
};

ContentState &content(Kernel &k) { return k.state<ContentState>(); }

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
        wr32(m + M_TITLE_ID, rd32(a.data() + 0x13C));
        std::memcpy(m + M_DISPLAY_NAME, a.data() + 8, 0x100);
        std::memcpy(h.data() + FILE_NAME_AT, a.data() + 0x108, FILE_NAME_SIZE);
        license = rd32(a.data() + AGGREGATE_SIZE);
        return true;
    }
    if (size != META + 0x93D6 && size != HEADER_FILE_SIZE) return false;
    f.read(reinterpret_cast<char *>(h.data()), std::streamsize(std::min<uint64_t>(size, HEADER_SIZE)));
    return true;
}

bool write_header(const Package &p) {
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
ContentData metadata(const std::filesystem::path &host_path, const std::vector<uint8_t> &h, uint32_t license) {
    const uint8_t *m = h.data() + META;
    ContentData d;
    d.device_id = DEVICE_HDD;
    d.title_id = rd32(m + M_TITLE_ID);
    d.content_type = rd32(m + M_CONTENT_TYPE);
    d.content_size = rd64(m + M_CONTENT_SIZE);
    if (!d.content_size) d.content_size = directory_size(host_path);
    d.display_name = rd_u16(m + M_DISPLAY_NAME, 128);
    for (uint32_t i = 0; d.display_name.empty() && i < 12; i++) {
        if (i < 9)
            d.display_name = rd_u16(m + M_DISPLAY_NAME + 0x100 * i, 128);
        else if (rd32(m + M_VERSION) >= 2)
            d.display_name = rd_u16(m + M_DISPLAY_NAME_EX + 0x100 * (i - 9), 128);
    }
    d.file_name = vfs::guest_name(host_path.filename());
    if (d.content_type == TYPE_SAVED_GAME) {
        const char *name = reinterpret_cast<const char *>(h.data() + FILE_NAME_AT);
        std::string saved(name, strnlen(name, FILE_NAME_SIZE));
        if (!saved.empty()) d.file_name = saved;
    }
    d.license_mask = license;
    d.xuid = rd64(m + M_PROFILE_ID);
    return d;
}

bool open_package(const std::filesystem::path &path, Package &p) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return false; // STFS files are not read here
    p.host_path = path;
    p.header_path = header_path_of(path);
    uint32_t license = 0;
    if (!read_header(p.header_path, p.header, license)) p.header.assign(HEADER_SIZE, 0);
    p.data = metadata(path, p.header, license);
    return true;
}

bool is_open(ContentState &s, std::string_view root) {
    return std::any_of(s.mounted.begin(), s.mounted.end(),
                       [&](const auto &p) { return equal_case(p->root_name, root); });
}

// ContentPackage::MountPackageAndRegister (the content lock held)
bool mount(Kernel &k, ContentState &s, std::unique_ptr<Package> p, const std::string &root) {
    p->device_path = "\\Device\\Content\\" + std::to_string(++s.next_device);
    auto device = vfs::make_host_device(p->device_path, p->host_path, false, true);
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
    return std::filesystem::remove_all(package_path(k, xuid, d), ec) > 0 ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
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
            p->host_path = package_path(k, xuid, c);
            p->header_path = header_path_of(p->host_path);
            std::error_code ec;
            if (std::filesystem::exists(p->host_path, ec)) {
                r = X_ERROR_ALREADY_EXISTS;
            } else {
                std::filesystem::create_directories(p->host_path.parent_path(), ec);
                p->header = new_header(k, c);
                p->data = c;
                p->data.device_id = DEVICE_HDD;
                bool ok = write_header(*p) && mount(k, s, std::move(p), root);
                r = ok ? X_ERROR_SUCCESS : X_ERROR_ACCESS_DENIED;
            }
        } else if (disposition == Open) {
            // ContentManager::OpenContent
            auto p = std::make_unique<Package>();
            if (!open_package(package_path(k, xuid, d), *p)) {
                r = X_ERROR_FILE_NOT_FOUND;
            } else {
                license = p->data.license_mask;
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

// (user_index, root_name, XCONTENT_DATA *, flags, disposition_ptr,
// license_mask_ptr, cache_size, content_size, overlapped)
XF_EXPORT(xam, XamContentCreateEx) {
    uint32_t data = arg(k, c, 2);
    if (!k.host(data + 0x108)[0]) return result(c, X_ERROR_INVALID_NAME);
    result(c, content_create(k, arg(k, c, 0), arg(k, c, 1), data, CONTENT_DATA_SIZE, arg(k, c, 3), arg(k, c, 4),
                             arg(k, c, 5), arg(k, c, 8)));
}

// (root_name, overlapped)
XF_EXPORT(xam, XamContentClose) {
    std::string root = guest_string(k, arg(k, c, 0));
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

// (root_name, overlapped): the header written now
XF_EXPORT(xam, XamContentFlush) {
    std::string root = guest_string(k, arg(k, c, 0));
    uint32_t ov = arg(k, c, 1);
    if (root.ends_with(':')) root.pop_back();
    ContentState &s = content(k);
    {
        std::lock_guard<std::mutex> l(s.mutex);
        auto it = std::find_if(s.mounted.begin(), s.mounted.end(),
                               [&](const auto &p) { return equal_case(p->root_name, root); });
        if (it == s.mounted.end()) return result(c, X_STATUS_INVALID_PARAMETER);
        write_header(**it);
    }
    if (ov) {
        complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, X_ERROR_SUCCESS);
}

// (user_index, XCONTENT_DATA *, overlapped)
XF_EXPORT(xam, XamContentDelete) {
    uint32_t index = arg(k, c, 0), ov = arg(k, c, 2);
    ContentData d;
    read_content_data(k, arg(k, c, 1), CONTENT_DATA_SIZE, d);
    uint64_t xuid = 0;
    if (index != USER_INDEX_NONE) {
        const User *u = user_at(k, index);
        if (!u) return result(c, X_ERROR_NO_SUCH_USER);
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
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, r);
}

// (user_index, device_id, content_type, flags, items_per_enumerate,
// buffer_size_ptr, handle_out): the user's and the common content on the
// hard drive, and the disc's
XF_EXPORT(xam, XamContentCreateEnumerator) {
    uint32_t index = arg(k, c, 0), device_id = arg(k, c, 1), type = arg(k, c, 2), flags = arg(k, c, 3);
    uint32_t items = arg(k, c, 4), size_ptr = arg(k, c, 5), handle_out = arg(k, c, 6);
    uint64_t xuid = 0;
    if (index < 4) {
        const User *u = user_at(k, index);
        if (!u) return result(c, X_ERROR_NO_SUCH_USER);
        xuid = u->xuid;
    }
    DeviceInfo info{};
    bool known = device_id && device_info(k, device_id, info);
    if ((device_id && !known) || !handle_out) {
        if (size_ptr) k.store32(size_ptr, 0);
        return result(c, X_E_INVALIDARG);
    }
    if (size_ptr) k.store32(size_ptr, CONTENT_DATA_SIZE * items);
    Ref<Enumerator> e = make_enumerator(k, USER_INDEX_ANY, 0xFE, 0x20005, 0x20007, 0, items, CONTENT_DATA_SIZE, 0x98);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    uint32_t title = title_id(k);
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

// (device_id, X_CONTENT_DEVICE_DATA *): id, type, total and free bytes, name
XF_EXPORT(xam, XamContentGetDeviceData) {
    uint32_t device_id = arg(k, c, 0), out = arg(k, c, 1);
    DeviceInfo info;
    if (!device_info(k, device_id, info)) return result(c, X_ERROR_DEVICE_NOT_CONNECTED);
    std::memset(k.host(out), 0, 0x50);
    k.store32(out + 0x00, info.id);
    k.store32(out + 0x04, info.type);
    k.store64(out + 0x08, info.total);
    k.store64(out + 0x10, info.free);
    put_u16(k, out + 0x18, info.name, 28);
    result(c, X_ERROR_SUCCESS);
}

} // namespace xf::kernel
