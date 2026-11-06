/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The virtual file system (files.h). Translated from Xenia (BSD; see
// LICENSE-xenia): src/xenia/vfs/virtual_file_system.cc, entry.cc,
// devices/disc_image_device.cc (and _entry, _file), devices/host_path_device.cc
// (and _entry, _file), devices/null_device.cc (and _entry, _file),
// src/xenia/base/utf8.cc (the guest path helpers), filesystem_wildcard.cc,
// filesystem_win.cc (the host file's positioned reads and writes); checked
// against them. Where it differs, and why:
//   - a device is found by the longest mount path that starts the path at a
//     separator, any case (Xenia: the first registered whose mount path is a
//     prefix, case-sensitive), so the registration order of \CACHE and
//     \CACHE0, or of \Device\Harddisk0 and its Partition1, does not matter;
//   - a symbolic link is the longest one that starts the path (Xenia: any
//     that does, in its hash map's order);
//   - a host file opened for deletion on close is closed before it is
//     deleted (Xenia deletes it while its handle is open, which Windows
//     refuses), and host files are shared for deletion too;
//   - the disc is read through xf::Disc (an ISO read in place) instead of a
//     mapping of the whole image;
//   - a package device (devices/xcontent_container_*, xcontent_devices/*) is
//     read through xf::Package (package.h, where its own differences are),
//     and its entries come from the package's tree.

#include "files.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xf::kernel::vfs {
namespace {

bool equal_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

bool starts_with_case(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && equal_case(s.substr(0, prefix.size()), prefix);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &ch : out) ch = char(std::tolower(uint8_t(ch)));
    return out;
}

// xe::utf8::split_path: parts between either separator, empty ones dropped
std::vector<std::string_view> split(std::string_view path) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("\\/", start);
        if (end == std::string_view::npos) end = path.size();
        if (end > start) parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string join(std::string_view left, std::string_view right) {
    if (left.empty()) return std::string(right);
    if (right.empty()) return std::string(left);
    std::string out(left);
    if (out.back() != '\\') out += '\\';
    return out + std::string(right);
}

// xe::utf8::find_base_guest_path and find_name_from_guest_path on a
// canonical path (no trailing or repeated separators)
std::string_view base_of(std::string_view path) {
    size_t cut = path.find_last_of('\\');
    return cut == std::string_view::npos ? std::string_view{} : path.substr(0, cut);
}
std::string_view name_of(std::string_view path) {
    size_t cut = path.find_last_of('\\');
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

uint64_t round_up(uint64_t v, uint64_t to) { return to ? (v + to - 1) / to * to : v; }

// ---- the disc (Xenia's DiscImageDevice) ----

class DiscStream : public Stream {
  public:
    DiscStream(const Disc &disc, const Entry &entry) : disc_(disc), entry_(entry) {}
    uint32_t read(uint64_t offset, uint8_t *out, size_t length, size_t &done) override {
        done = 0;
        if (!entry_.disc || offset >= entry_.size) return X_STATUS_END_OF_FILE;
        size_t n = size_t(std::min<uint64_t>(length, entry_.size - offset));
        if (!disc_.read(*entry_.disc, offset, out, n)) {
            std::fprintf(stderr, "files: the disc cannot be read at %s+0x%llX\n", entry_.path.c_str(),
                         (unsigned long long)offset);
            return X_STATUS_END_OF_FILE;
        }
        done = n;
        return X_STATUS_SUCCESS;
    }
    uint32_t write(uint64_t, const uint8_t *, size_t, size_t &done) override {
        done = 0;
        return X_STATUS_ACCESS_DENIED;
    }
    uint32_t set_length(uint64_t) override { return X_STATUS_ACCESS_DENIED; }

  private:
    const Disc &disc_;
    const Entry &entry_;
};

class DiscDevice : public Device {
  public:
    DiscDevice(const std::string &mount_path, std::unique_ptr<Disc> disc)
        : Device(mount_path, "GDFX", true), disc_(std::move(disc)) {
        total_units = disc_->size() / sectors_per_unit / bytes_per_sector;
        available_units = 0;
        root = std::make_shared<Entry>();
        root->device = this;
        root->attributes = ATTRIBUTE_DIRECTORY;
        add_children(*root, disc_->root());
    }
    uint32_t open(Entry &entry, uint32_t, std::unique_ptr<Stream> &out) override {
        out = std::make_unique<DiscStream>(*disc_, entry);
        return X_STATUS_SUCCESS;
    }

  private:
    void add_children(Entry &parent, const DiscEntry &from) {
        for (const DiscEntry &d : from.children) {
            auto e = std::make_shared<Entry>();
            e->device = this;
            e->parent = &parent;
            e->disc = &d;
            e->name = d.name;
            e->path = join(parent.path, d.name);
            e->attributes = d.attributes | ATTRIBUTE_READ_ONLY;
            e->size = d.size;
            e->allocation_size = round_up(d.size, bytes_per_sector);
            e->create_time = e->access_time = e->write_time = UNIX_EPOCH_FILETIME;
            if (d.directory) add_children(*e, d);
            parent.children.push_back(std::move(e));
        }
    }
    std::unique_ptr<Disc> disc_;
};

// ---- a content package (Xenia's StfsContainerDevice, SvodContainerDevice) ----

class PackageStream : public Stream {
  public:
    PackageStream(const Package &package, const Entry &entry) : package_(package), entry_(entry) {}
    // XContentContainerFile::ReadSync
    uint32_t read(uint64_t offset, uint8_t *out, size_t length, size_t &done) override {
        done = 0;
        if (!entry_.package || offset >= entry_.size) return X_STATUS_END_OF_FILE;
        done = package_.read(*entry_.package, offset, out, length);
        return X_STATUS_SUCCESS;
    }
    // writes and resizing are denied, as Xenia's
    uint32_t write(uint64_t, const uint8_t *, size_t, size_t &done) override {
        done = 0;
        return X_STATUS_ACCESS_DENIED;
    }
    uint32_t set_length(uint64_t) override { return X_STATUS_ACCESS_DENIED; }

  private:
    const Package &package_;
    const Entry &entry_;
};

class PackageDevice : public Device {
  public:
    // the name Xenia gives each (an SVOD calls itself FATX); read_only is the
    // STFS descriptor's flag, so a package not marked read-only is opened for
    // writing and the writes are denied, as Xenia's
    PackageDevice(const std::string &mount_path, std::unique_ptr<Package> package)
        : Device(mount_path, package->is_stfs() ? "STFS" : "FATX", package->read_only()),
          package_(std::move(package)) {
        component_name_max_length = package_->is_stfs() ? 40 : 255;
        total_units = package_->total_units();
        available_units = package_->available_units();
        sectors_per_unit = 8;
        bytes_per_sector = 0x200;
        root = std::make_shared<Entry>();
        root->device = this;
        root->attributes = ATTRIBUTE_DIRECTORY;
        root->package = &package_->root();
        root->create_time = root->access_time = root->write_time = package_->root().write_time;
        add_children(*root, package_->root());
    }
    uint32_t open(Entry &entry, uint32_t, std::unique_ptr<Stream> &out) override {
        out = std::make_unique<PackageStream>(*package_, entry);
        return X_STATUS_SUCCESS;
    }
    const Package &package() const { return *package_; }

  private:
    void add_children(Entry &parent, const PackageNode &from) {
        for (const auto &n : from.children) {
            auto e = std::make_shared<Entry>();
            e->device = this;
            e->parent = &parent;
            e->package = n.get();
            e->name = n->name;
            e->path = join(parent.path, n->name);
            e->size = n->size;
            e->allocation_size = round_up(n->size, 0x1000);
            e->create_time = n->create_time;
            e->write_time = e->access_time = n->write_time;
            // a directory of an SVOD is read-only, of an STFS not (Xenia's)
            if (n->directory)
                e->attributes = package_->is_stfs() ? ATTRIBUTE_DIRECTORY : ATTRIBUTE_DIRECTORY | ATTRIBUTE_READ_ONLY;
            else
                e->attributes = ATTRIBUTE_NORMAL | ATTRIBUTE_READ_ONLY;
            if (n->directory) add_children(*e, *n);
            parent.children.push_back(std::move(e));
        }
    }
    std::unique_ptr<Package> package_;
};

// ---- a host directory (Xenia's HostPathDevice) ----

struct HostInfo {
    bool directory = false;
    uint64_t size = 0, create_time = 0, access_time = 0, write_time = 0;
};

#ifdef _WIN32
uint64_t filetime(const FILETIME &t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; }
#endif

bool host_info(const std::filesystem::path &path, HostInfo &info) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    info.directory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    info.size = uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    info.create_time = filetime(data.ftCreationTime);
    info.access_time = filetime(data.ftLastAccessTime);
    info.write_time = filetime(data.ftLastWriteTime);
    return true;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    info.directory = S_ISDIR(st.st_mode);
    info.size = uint64_t(st.st_size);
    auto ft = [](time_t s, long ns) { return UNIX_EPOCH_FILETIME + uint64_t(s) * 10'000'000ull + uint64_t(ns) / 100; };
    info.create_time = ft(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    info.access_time = ft(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    info.write_time = ft(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    return true;
#endif
}

// Positioned reads and writes on a host file (Xenia's FileHandle), so
// threads reading one file at once need no lock.
class HostStream : public Stream {
  public:
    static std::unique_ptr<HostStream> open(const std::filesystem::path &path, uint32_t access) {
#ifdef _WIN32
        DWORD rights = 0;
        if (access & GENERIC_READ) rights |= FILE_GENERIC_READ;
        if (access & GENERIC_WRITE) rights |= FILE_GENERIC_WRITE;
        if (access & GENERIC_ALL) rights |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
        if (access & READ_DATA) rights |= FILE_READ_DATA;
        if (access & WRITE_DATA) rights |= FILE_WRITE_DATA;
        if (access & APPEND_DATA) rights |= FILE_APPEND_DATA;
        HANDLE h = CreateFileW(path.c_str(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        return std::unique_ptr<HostStream>(new HostStream(h, access));
#else
        bool write = access & (GENERIC_WRITE | GENERIC_ALL | WRITE_DATA | APPEND_DATA);
        int fd = ::open(path.c_str(), write ? O_RDWR : O_RDONLY);
        if (fd < 0 && write) fd = ::open(path.c_str(), O_RDONLY); // a directory
        if (fd < 0) return nullptr;
        return std::unique_ptr<HostStream>(new HostStream(fd, access));
#endif
    }
    ~HostStream() override {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        ::close(fd_);
#endif
    }

    // HostPathFile::ReadSync: a failed read is the end of the file
    uint32_t read(uint64_t offset, uint8_t *out, size_t length, size_t &done) override {
        done = 0;
        if (!(access_ & (GENERIC_READ | READ_DATA))) return X_STATUS_ACCESS_DENIED;
#ifdef _WIN32
        OVERLAPPED o = {};
        o.Offset = DWORD(offset), o.OffsetHigh = DWORD(offset >> 32);
        DWORD n = 0;
        if (!ReadFile(handle_, out, DWORD(length), &n, &o)) return X_STATUS_END_OF_FILE;
        done = n;
#else
        ssize_t n = pread(fd_, out, length, off_t(offset));
        if (n < 0 || (n == 0 && length)) return X_STATUS_END_OF_FILE;
        done = size_t(n);
#endif
        return X_STATUS_SUCCESS;
    }
    uint32_t write(uint64_t offset, const uint8_t *in, size_t length, size_t &done) override {
        done = 0;
        if (!(access_ & (GENERIC_WRITE | WRITE_DATA | APPEND_DATA))) return X_STATUS_ACCESS_DENIED;
#ifdef _WIN32
        OVERLAPPED o = {};
        o.Offset = DWORD(offset), o.OffsetHigh = DWORD(offset >> 32);
        DWORD n = 0;
        if (!WriteFile(handle_, in, DWORD(length), &n, &o)) return X_STATUS_END_OF_FILE;
        done = n;
#else
        ssize_t n = pwrite(fd_, in, length, off_t(offset));
        if (n < 0) return X_STATUS_END_OF_FILE;
        done = size_t(n);
#endif
        return X_STATUS_SUCCESS;
    }
    uint32_t set_length(uint64_t length) override {
        if (!(access_ & (GENERIC_WRITE | WRITE_DATA))) return X_STATUS_ACCESS_DENIED;
#ifdef _WIN32
        LARGE_INTEGER p;
        p.QuadPart = LONGLONG(length);
        if (!SetFilePointerEx(handle_, p, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) return X_STATUS_END_OF_FILE;
#else
        if (ftruncate(fd_, off_t(length)) != 0) return X_STATUS_END_OF_FILE;
#endif
        return X_STATUS_SUCCESS;
    }

  private:
#ifdef _WIN32
    HostStream(HANDLE h, uint32_t access) : handle_(h), access_(access) {}
    HANDLE handle_;
#else
    HostStream(int fd, uint32_t access) : fd_(fd), access_(access) {}
    int fd_;
#endif
    uint32_t access_;
};

// A host path that may pass Windows' MAX_PATH: a FATX path of 240 characters under a storage
// root already deep takes more than 260. The extended-length prefix lifts that; elsewhere the
// path is as it is.
std::filesystem::path extended_path(const std::filesystem::path &p) {
#ifdef _WIN32
    std::wstring w = std::filesystem::absolute(p).lexically_normal().native();
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
#else
    return p;
#endif
}

class HostDevice : public Device {
  public:
    HostDevice(const std::string &mount_path, const std::filesystem::path &host_path, bool read_only, bool package)
        : Device(mount_path, package ? "STFS" : "FATX", read_only) {
        // FATX's limits (Free60's FATX page, "Limitations"): names of 42 characters, paths of 240;
        // Xenia's host device says 255 for the names and has no path limit
        if (!package) component_name_max_length = 42, max_path_length = 240;
        total_units = available_units = 128 * 1024;
        root = std::make_shared<Entry>();
        root->device = this;
        root->attributes = ATTRIBUTE_DIRECTORY;
        root->host_path = extended_path(host_path);
        std::vector<std::filesystem::path> ancestors;
        populate(*root, ancestors);
    }

    uint32_t open(Entry &entry, uint32_t access, std::unique_ptr<Stream> &out) override {
        if (read_only && (access & (WRITE_DATA | APPEND_DATA))) {
            std::fprintf(stderr, "files: %s is opened for writing on a read-only device\n", entry.path.c_str());
            return X_STATUS_ACCESS_DENIED;
        }
        out = HostStream::open(entry.host_path, access);
        return out ? X_STATUS_SUCCESS : X_STATUS_NO_SUCH_FILE;
    }

    std::shared_ptr<Entry> create(Entry &parent, std::string_view name, uint32_t attributes) override {
        std::filesystem::path full = parent.host_path / host_name(name);
        std::error_code ec;
        if (attributes & ATTRIBUTE_DIRECTORY) {
            if (!std::filesystem::create_directories(full, ec)) return nullptr;
        } else {
#ifdef _WIN32
            std::FILE *f = _wfopen(full.c_str(), L"wb");
#else
            std::FILE *f = std::fopen(full.c_str(), "wb");
#endif
            if (!f) return nullptr;
            std::fclose(f);
        }
        HostInfo info;
        if (!host_info(full, info)) return nullptr;
        return make_entry(parent, std::string(name), full, info);
    }

    bool remove(Entry &child) override {
        std::error_code ec;
        if (child.directory()) {
            auto n = std::filesystem::remove_all(child.host_path, ec);
            return !ec && n >= 1;
        }
        if (std::filesystem::is_directory(child.host_path, ec)) return false;
        if (std::filesystem::exists(child.host_path, ec)) {
            std::filesystem::remove(child.host_path, ec);
            if (ec) {
                std::fprintf(stderr, "files: cannot remove %s: %s\n", child.host_path.string().c_str(),
                             ec.message().c_str());
                return false;
            }
        }
        return true;
    }

    bool exists(const Entry &entry) override {
        std::error_code ec;
        return std::filesystem::exists(entry.host_path, ec);
    }

    void update(Entry &entry) override {
        HostInfo info;
        if (!host_info(entry.host_path, info) || info.directory) return;
        entry.size = info.size;
        entry.allocation_size = round_up(info.size, bytes_per_sector);
    }

    bool set_time(Entry &) override { return !read_only; }

    bool set_attributes(Entry &entry, uint32_t attributes) override {
        if (read_only) return false;
#ifdef _WIN32
        return SetFileAttributesW(entry.host_path.c_str(), attributes) != 0;
#else
        (void)entry, (void)attributes;
        return true;
#endif
    }

  private:
    std::shared_ptr<Entry> make_entry(Entry &parent, std::string name, const std::filesystem::path &full,
                                      const HostInfo &info) {
        auto e = std::make_shared<Entry>();
        e->device = this;
        e->parent = &parent;
        e->path = join(parent.path, name);
        e->name = std::move(name);
        e->host_path = full;
        e->create_time = info.create_time, e->access_time = info.access_time, e->write_time = info.write_time;
        if (info.directory) {
            e->attributes = ATTRIBUTE_DIRECTORY;
        } else {
            e->attributes = ATTRIBUTE_NORMAL | (read_only ? ATTRIBUTE_READ_ONLY : 0);
            e->size = info.size;
            e->allocation_size = round_up(info.size, bytes_per_sector);
        }
        return e;
    }

    // HostPathDevice::PopulateEntry: the tree, read once; a directory link
    // back into its own path is not followed again
    void populate(Entry &parent, std::vector<std::filesystem::path> &ancestors) {
        std::error_code ec;
        auto self = std::filesystem::canonical(parent.host_path, ec);
        if (!ec) {
            if (std::find(ancestors.begin(), ancestors.end(), self) != ancestors.end()) return;
            ancestors.push_back(self);
        }
        for (std::filesystem::directory_iterator it(parent.host_path, ec), end; !ec && it != end; it.increment(ec)) {
            HostInfo info;
            if (!host_info(it->path(), info)) continue;
            auto child = make_entry(parent, guest_name(it->path().filename()), it->path(), info);
            if (info.directory) populate(*child, ancestors);
            parent.children.push_back(std::move(child));
        }
        if (!self.empty() && !ancestors.empty() && ancestors.back() == self) ancestors.pop_back();
    }
};

// ---- the null device (Xenia's NullDevice) ----

class NullStream : public Stream {
  public:
    explicit NullStream(uint32_t access) : access_(access) {}
    uint32_t read(uint64_t, uint8_t *, size_t, size_t &done) override {
        done = 0;
        return (access_ & READ_DATA) ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
    }
    uint32_t write(uint64_t, const uint8_t *, size_t, size_t &done) override {
        done = 0;
        return (access_ & (WRITE_DATA | APPEND_DATA)) ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
    }
    uint32_t set_length(uint64_t) override {
        return (access_ & WRITE_DATA) ? X_STATUS_SUCCESS : X_STATUS_ACCESS_DENIED;
    }

  private:
    uint32_t access_;
};

class NullDevice : public Device {
  public:
    NullDevice(const std::string &mount_path, const std::vector<std::string> &names)
        : Device(mount_path, "NullDevice", false) {
        component_name_max_length = 40;
        total_units = available_units = 0x10;
        sectors_per_unit = 0x80;
        root = std::make_shared<Entry>();
        root->device = this;
        root->attributes = ATTRIBUTE_DIRECTORY;
        for (const std::string &name : names) {
            auto e = std::make_shared<Entry>();
            e->device = this;
            e->parent = root.get();
            e->name = e->path = name;
            e->attributes = ATTRIBUTE_NORMAL;
            root->children.push_back(std::move(e));
        }
    }
    // only its own names, no deeper (NullDevice::ResolvePath)
    Entry *resolve(std::string_view path) override { return path.empty() ? root.get() : root->child(path); }
    uint32_t open(Entry &, uint32_t access, std::unique_ptr<Stream> &out) override {
        out = std::make_unique<NullStream>(access);
        return X_STATUS_SUCCESS;
    }
};

// marks an entry and what is under it gone, so nothing reaches a parent
// that may no longer be there
void detach(Entry &e) {
    e.removed = true;
    e.parent = nullptr;
    for (auto &child : e.children) detach(*child);
}

} // namespace

// ---- entries and devices ----

Entry *Entry::child(std::string_view name) const {
    for (const auto &c : children)
        if (equal_case(c->name, name)) return c.get();
    return nullptr;
}

std::shared_ptr<Entry> Entry::shared_child(const Entry *child) const {
    for (const auto &c : children)
        if (c.get() == child) return c;
    return nullptr;
}

uint32_t Stream::set_length(uint64_t) { return X_STATUS_NOT_IMPLEMENTED; }

Device::Device(std::string mount_path, std::string name, bool read_only)
    : mount_path(std::move(mount_path)), name(std::move(name)), read_only(read_only) {}
Device::~Device() = default;

Entry *Device::resolve(std::string_view path) {
    Entry *e = root.get();
    for (std::string_view part : split(path)) {
        e = e->child(part);
        if (!e) return nullptr;
    }
    return e;
}

std::shared_ptr<Entry> Device::create(Entry &, std::string_view, uint32_t) { return nullptr; }
bool Device::remove(Entry &) { return false; }

std::shared_ptr<Device> make_disc_device(const std::string &mount_path, std::unique_ptr<Disc> disc) {
    return std::make_shared<DiscDevice>(mount_path, std::move(disc));
}

std::shared_ptr<Device> make_package_device(const std::string &mount_path, const std::filesystem::path &host_path,
                                            std::string &error) {
    std::unique_ptr<Package> package = Package::open(host_path, error);
    if (!package) return nullptr;
    for (const std::string &w : package->warnings())
        std::fprintf(stderr, "files: %s: %s\n", host_path.string().c_str(), w.c_str());
    return std::make_shared<PackageDevice>(mount_path, std::move(package));
}

std::shared_ptr<Device> make_host_device(const std::string &mount_path, const std::filesystem::path &host_path,
                                         bool read_only, bool package) {
    std::error_code ec;
    if (!std::filesystem::exists(host_path, ec)) {
        if (read_only) {
            std::fprintf(stderr, "files: %s does not exist\n", host_path.string().c_str());
            return nullptr;
        }
        std::filesystem::create_directories(host_path, ec);
        if (ec) {
            std::fprintf(stderr, "files: cannot create %s: %s\n", host_path.string().c_str(), ec.message().c_str());
            return nullptr;
        }
    }
    return std::make_shared<HostDevice>(mount_path, host_path, read_only, package);
}

std::shared_ptr<Device> make_null_device(const std::string &mount_path, const std::vector<std::string> &names) {
    return std::make_shared<NullDevice>(mount_path, names);
}

// ---- paths ----

std::filesystem::path host_name(std::string_view name) {
    return std::filesystem::path(std::u8string(name.begin(), name.end()));
}

std::string guest_name(const std::filesystem::path &path) {
    std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::string canonical(std::string_view path) {
    bool rooted = !path.empty() && path.front() == '\\';
    std::vector<std::string_view> parts;
    for (std::string_view part : split(path)) {
        if (part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && !parts.back().ends_with(':')) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out = rooted ? "\\" : "";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '\\';
        out += parts[i];
    }
    return out;
}

// WildcardEngine::PreparePattern: literal runs between * and ?; a run of ?
// takes as many characters; the first run is anchored at the start and the
// last at the end, unless a * frees them
void Wildcard::set(std::string_view pattern) {
    rules_.clear();
    bool first = true;
    size_t n = 0, last = 0;
    while ((n = pattern.find_first_of("*?", last)) != std::string_view::npos) {
        if (last != n) {
            Rule r;
            r.text = lower(pattern.substr(last, n - last));
            r.from_start = first;
            rules_.push_back(r);
        }
        if (pattern[n] == '?') {
            size_t end = pattern.find_first_not_of('?', n + 1);
            size_t count = end == std::string_view::npos ? pattern.size() - n : end - n;
            Rule r;
            r.text = std::string(count, '?');
            r.exact_length = true;
            rules_.push_back(r);
            last = n + count;
        } else {
            last = n + 1;
        }
        first = false;
    }
    if (last != pattern.size()) {
        Rule r;
        r.text = lower(pattern.substr(last));
        r.to_end = true;
        r.from_start = last == 0;
        rules_.push_back(r);
    }
}

bool Wildcard::match(std::string_view name) const {
    std::string s = lower(name);
    size_t offset = 0;
    for (const Rule &r : rules_) {
        if (r.text.empty()) continue;
        if (s.size() - offset < r.text.size()) return false;
        if (r.exact_length) {
            offset += r.text.size();
            continue;
        }
        size_t at = s.find(r.text, offset);
        if (at == std::string::npos) return false;
        if (r.from_start && at != offset) return false;
        if (r.to_end && at != s.size() - r.text.size()) return false;
        offset = at + r.text.size();
    }
    return true;
}

// ---- the file system ----

void FileSystem::add_device(std::shared_ptr<Device> device) {
    std::lock_guard<std::recursive_mutex> l(mutex);
    devices_.push_back(std::move(device));
}

bool FileSystem::remove_device(std::string_view mount_path) {
    std::lock_guard<std::recursive_mutex> l(mutex);
    return std::erase_if(devices_, [&](const auto &d) { return d->mount_path == mount_path; }) != 0;
}

bool FileSystem::add_link(const std::string &path, const std::string &target) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    for (const auto &link : o.symbolic_links)
        if (equal_case(link.first, path)) return false; // VirtualFileSystem::RegisterSymbolicLink refuses
    o.symbolic_links.push_back({path, target});
    return true;
}

bool FileSystem::remove_link(std::string_view path) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    return std::erase_if(o.symbolic_links, [&](const auto &link) { return equal_case(link.first, path); }) != 0;
}

// VirtualFileSystem::FindSymbolicLink: a link that starts the path
bool FileSystem::find_link(std::string_view path, std::string &target) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    size_t best = 0;
    for (const auto &[link, to] : o.symbolic_links) {
        if (starts_with_case(path, link) && link.size() > best) {
            best = link.size();
            target = to;
        }
    }
    return best != 0;
}

bool FileSystem::has_link(std::string_view path) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    return std::any_of(o.symbolic_links.begin(), o.symbolic_links.end(),
                       [&](const auto &link) { return equal_case(link.first, path); });
}

// VirtualFileSystem::ResolveSymbolicLink, links again until none applies
std::string FileSystem::resolve_links(std::string_view path) {
    std::string result = canonical(path);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    for (int depth = 0; depth < 32; depth++) {
        const std::pair<std::string, std::string> *best = nullptr;
        for (const auto &link : o.symbolic_links)
            if (!link.first.empty() && starts_with_case(result, link.first) &&
                (!best || link.first.size() > best->first.size()))
                best = &link;
        if (!best) break;
        result = canonical(best->second + "\\" + result.substr(best->first.size()));
    }
    return result;
}

Entry *FileSystem::resolve(std::string_view path, std::shared_ptr<Device> *device_out) {
    return find(resolve_links(path), device_out);
}

Entry *FileSystem::find(const std::string &resolved, std::shared_ptr<Device> *device_out) {
    std::lock_guard<std::recursive_mutex> l(mutex);
    Device *best = nullptr;
    std::shared_ptr<Device> best_shared;
    for (const auto &d : devices_) {
        std::string_view mount = d->mount_path;
        if (!starts_with_case(resolved, mount)) continue;
        if (resolved.size() != mount.size() && resolved[mount.size()] != '\\') continue;
        if (!best || mount.size() > best->mount_path.size()) best = d.get(), best_shared = d;
    }
    if (!best) {
        std::fprintf(stderr, "files: no device for %s\n", resolved.c_str());
        return nullptr;
    }
    if (device_out) *device_out = best_shared;
    std::string_view rest = std::string_view(resolved).substr(best->mount_path.size());
    while (!rest.empty() && rest.front() == '\\') rest.remove_prefix(1);
    return best->resolve(rest);
}

bool FileSystem::remove(Entry &entry) {
    std::lock_guard<std::recursive_mutex> l(mutex);
    Entry *parent = entry.parent;
    if (!parent || entry.device->read_only) return false;
    if (!entry.device->remove(entry)) return false;
    std::shared_ptr<Entry> keep = parent->shared_child(&entry);
    std::erase_if(parent->children, [&](const auto &c) { return c.get() == &entry; });
    detach(entry);
    return true;
}

// VirtualFileSystem::OpenFile. A new entry is made in its parent, which
// must exist (Xenia's CreatePath walks the path, but OpenFile has failed
// with OBJECT_PATH_NOT_FOUND by then when a directory on the way is missing).
uint32_t FileSystem::open(std::shared_ptr<Device> root_device, Entry *root, std::string_view raw_path,
                          Disposition disposition, uint32_t access, bool is_directory, bool non_directory, Opened &out,
                          Action &action) {
    if (access & GENERIC_READ) access |= READ_DATA;
    if (access & GENERIC_WRITE) access |= WRITE_DATA;
    if (access & GENERIC_ALL) access |= READ_DATA | WRITE_DATA;

    std::string path = canonical(raw_path);
    std::string base(base_of(path));
    std::string resolved_base = root ? std::string() : resolve_links(base);
    std::string resolved_path = root ? std::string() : resolve_links(path);

    std::lock_guard<std::recursive_mutex> l(mutex);
    std::shared_ptr<Device> device;
    Entry *parent = nullptr, *entry = nullptr;
    if (root) device = std::move(root_device);
    if (!base.empty()) {
        parent = root ? root->device->resolve(join(root->path, base)) : find(resolved_base, &device);
        if (!parent) {
            action = Action::DoesNotExist;
            return X_STATUS_OBJECT_PATH_NOT_FOUND;
        }
        entry = parent->child(name_of(path));
    } else {
        entry = root ? root->child(path) : find(resolved_path, &device);
        if (root) parent = root;
    }

    if (entry) {
        if (entry->directory() && non_directory) return X_STATUS_FILE_IS_A_DIRECTORY;
        // a host file removed behind the cache's back is gone
        if (parent && !entry->device->exists(*entry)) {
            if (Entry *p = entry->parent) {
                std::shared_ptr<Entry> keep = p->shared_child(entry);
                std::erase_if(p->children, [&](const auto &c) { return c.get() == entry; });
                detach(*entry);
            }
            entry = nullptr;
        }
    }

    switch (disposition) {
    case Disposition::Open:
    case Disposition::Overwrite:
        if (!entry) {
            action = Action::DoesNotExist;
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        break;
    case Disposition::Create:
        if (entry) {
            action = Action::Exists;
            return X_STATUS_OBJECT_NAME_COLLISION;
        }
        break;
    default: break;
    }

    // a new name must fit the device (FATX: 42 characters, 240 in a path); a file that exists
    // was made elsewhere (a host directory's), and is let be
    if (!entry && parent && !parent->device->read_only) {
        std::string_view name = base.empty() ? std::string_view(path) : name_of(path);
        const Device &d = *parent->device;
        size_t in_device = (parent->path.empty() ? 0 : parent->path.size() + 1) + name.size();
        if (name.size() > d.component_name_max_length || (d.max_path_length && in_device > d.max_path_length)) {
            action = Action::DoesNotExist;
            return X_STATUS_OBJECT_NAME_INVALID;
        }
    }

    bool wants_write = access & (WRITE_DATA | APPEND_DATA);
    if (wants_write && ((parent && parent->device->read_only) || (entry && entry->device->read_only))) {
        // as Xenia: games open read-only files for writing; they get reading
        std::fprintf(stderr, "files: %s is opened for writing on a read-only device; reading only\n",
                     std::string(raw_path).c_str());
        access = GENERIC_READ | READ_DATA;
    }

    if (!entry) {
        action = Action::Created;
    } else {
        switch (disposition) {
        case Disposition::Supersede:
            if (!remove(*entry)) return X_STATUS_ACCESS_DENIED;
            entry = nullptr;
            action = Action::Superseded;
            break;
        case Disposition::Open:
        case Disposition::OpenIf: action = Action::Opened; break;
        case Disposition::Overwrite:
        case Disposition::OverwriteIf:
            if (!remove(*entry)) return X_STATUS_ACCESS_DENIED;
            entry = nullptr;
            action = Action::Overwritten;
            break;
        default: return X_STATUS_ACCESS_DENIED;
        }
    }
    if (!entry) {
        std::string_view name = base.empty() ? std::string_view(path) : name_of(path);
        if (!parent || parent->device->read_only || parent->child(name)) return X_STATUS_ACCESS_DENIED;
        auto created = parent->device->create(*parent, name, is_directory ? ATTRIBUTE_DIRECTORY : ATTRIBUTE_NORMAL);
        if (!created) return X_STATUS_ACCESS_DENIED;
        entry = created.get();
        parent->children.push_back(std::move(created));
    }

    uint32_t status = entry->device->open(*entry, access, out.stream);
    if (status != X_STATUS_SUCCESS) {
        action = Action::DoesNotExist;
        return status;
    }
    out.entry = entry->parent ? entry->parent->shared_child(entry) : entry->device->root;
    out.device = device;
    out.access = access;
    return X_STATUS_SUCCESS;
}

} // namespace xf::kernel::vfs
