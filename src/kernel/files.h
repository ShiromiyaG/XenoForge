/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The files family (plan step A4; docs/runtime.md, "Files"): the virtual
// file system and the kernel's file objects. Translated from Xenia (BSD; see
// LICENSE-xenia): src/xenia/vfs (virtual_file_system.cc, entry.cc, the disc
// image, host path and null devices), src/xenia/base/filesystem_wildcard.cc,
// src/xenia/kernel/xfile.cc and xiocompletion.cc; checked against them.
//
// A guest path ("game:\media\a.bin", "\Device\Cdrom0\default.xex") is made
// canonical, its symbolic links resolved (the object manager's list, which
// ObCreateSymbolicLink fills, in objects.h), and handed to the device whose
// mount path starts it; the device's tree of entries (case-insensitive, as
// Xenia keeps it) names the file. The devices: the disc (\Device\Cdrom0), a
// directory on the host (a bare executable's directory, the cache
// partitions, mounted content), a content package (STFS or SVOD, in
// package.h), and the null device Xenia puts at
// \Device\Harddisk0 for the raw partitions.

#pragma once

#include "objects.h"
#include "xenoforge/disc.h"
#include "xenoforge/package.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xf::kernel {

// ---- statuses (Xenia's xbox.h) ----

constexpr uint32_t X_STATUS_BUFFER_OVERFLOW = 0x80000005;
constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
constexpr uint32_t X_STATUS_ACCESS_VIOLATION = 0xC0000005;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t X_STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t X_STATUS_OBJECT_NAME_INVALID = 0xC0000033;
constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;

namespace vfs {

// what NtCreateFile's disposition asks (FILE_SUPERSEDE...)
enum class Disposition : uint32_t { Supersede, Open, Create, OpenIf, Overwrite, OverwriteIf };
// what it did (the I/O status block's information)
enum class Action : uint32_t { Superseded, Opened, Created, Overwritten, Exists, DoesNotExist };

// access (xe::filesystem::FileAccess)
constexpr uint32_t GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, GENERIC_ALL = 0x10000000;
constexpr uint32_t READ_DATA = 0x1, WRITE_DATA = 0x2, APPEND_DATA = 0x4;

// attributes (X_FILE_ATTRIBUTES)
constexpr uint32_t ATTRIBUTE_READ_ONLY = 0x1, ATTRIBUTE_DIRECTORY = 0x10, ATTRIBUTE_NORMAL = 0x80;

// 1970-01-01 as a FILETIME: the disc's entries' times, as Xenia gives them
constexpr uint64_t UNIX_EPOCH_FILETIME = 116'444'736'000'000'000ull;

class Device;

// A file or directory of a device (Xenia's vfs::Entry). The tree is the
// file system's, under its lock; a file object holds its entry (and device)
// alive past a removal.
class Entry {
  public:
    Device *device = nullptr;
    Entry *parent = nullptr; // nullptr: the root, or removed
    std::string path;        // in the device: "media\a.bin" ("" the root)
    std::string name;
    uint32_t attributes = 0;
    uint64_t size = 0, allocation_size = 0;
    uint64_t create_time = 0, access_time = 0, write_time = 0; // FILETIMEs
    bool delete_on_close = false;
    bool removed = false;
    std::vector<std::shared_ptr<Entry>> children;

    // what its device keeps
    std::filesystem::path host_path; // a host directory's
    const DiscEntry *disc = nullptr; // the disc's
    const PackageNode *package = nullptr; // a content package's

    bool directory() const { return attributes & ATTRIBUTE_DIRECTORY; }
    // a child by name, any case
    Entry *child(std::string_view name) const;
    std::shared_ptr<Entry> shared_child(const Entry *child) const;
};

// An entry opened for reading or writing (Xenia's vfs::File).
class Stream {
  public:
    virtual ~Stream() = default;
    virtual uint32_t read(uint64_t offset, uint8_t *out, size_t length, size_t &done) = 0;
    virtual uint32_t write(uint64_t offset, const uint8_t *in, size_t length, size_t &done) = 0;
    virtual uint32_t set_length(uint64_t length);
};

// A device under a mount path (Xenia's vfs::Device), with what
// NtQueryVolumeInformationFile tells of it.
class Device {
  public:
    Device(std::string mount_path, std::string name, bool read_only);
    virtual ~Device();

    std::string mount_path; // "\Device\Cdrom0"
    std::string name;       // the file system's: "GDFX", "FATX", "STFS"
    bool read_only;
    uint32_t attributes = 0, component_name_max_length = 255;
    // the longest path a new file or directory may have, in the device (without
    // its mount path); 0: no limit. A name longer than component_name_max_length
    // cannot be created either.
    uint32_t max_path_length = 0;
    uint64_t total_units = 0, available_units = 0;
    uint32_t sectors_per_unit = 1, bytes_per_sector = 0x200;
    std::shared_ptr<Entry> root;

    // a path in the device ("media\a.bin"), any case
    virtual Entry *resolve(std::string_view path);
    // opens the entry for the access asked: X_STATUS
    virtual uint32_t open(Entry &entry, uint32_t access, std::unique_ptr<Stream> &out) = 0;
    // a new child (a host directory's): nullptr when it cannot be
    virtual std::shared_ptr<Entry> create(Entry &parent, std::string_view name, uint32_t attributes);
    // removes the child's data (a host directory's)
    virtual bool remove(Entry &child);
    // whether the entry is still there (a host directory's: the host decides)
    virtual bool exists(const Entry &) { return true; }
    // reads its size and times again
    virtual void update(Entry &) {}
    virtual bool set_attributes(Entry &, uint32_t) { return false; }
    // a time set by NtSetInformationFile (a host directory's: accepted and
    // kept nowhere, as Xenia's)
    virtual bool set_time(Entry &) { return false; }
};

// The game's disc (Xenia's DiscImageDevice), read in place.
std::shared_ptr<Device> make_disc_device(const std::string &mount_path, std::unique_ptr<Disc> disc);
// A content package (Xenia's StfsContainerDevice and SvodContainerDevice), STFS
// or SVOD by its header, read only. Null, with the reason in `error`, when the
// file is not one or is damaged.
std::shared_ptr<Device> make_package_device(const std::string &mount_path, const std::filesystem::path &host_path,
                                            std::string &error);
// A directory on the host (Xenia's HostPathDevice): created when writable
// and missing; package: named STFS (mounted content) instead of FATX.
std::shared_ptr<Device> make_host_device(const std::string &mount_path, const std::filesystem::path &host_path,
                                         bool read_only, bool package = false);
// Names that answer every read and write with nothing (Xenia's NullDevice).
std::shared_ptr<Device> make_null_device(const std::string &mount_path, const std::vector<std::string> &names);

// a guest name (UTF-8) as a host path's part, and back
std::filesystem::path host_name(std::string_view name);
std::string guest_name(const std::filesystem::path &path);

// Xenia's canonicalize_guest_path: separators either way, "." and ".."
// resolved (never above a device's "x:"), no empty parts
std::string canonical(std::string_view path);

// The pattern NtQueryDirectoryFile matches names with (Xenia's
// WildcardEngine): * and ?, any case.
class Wildcard {
  public:
    void set(std::string_view pattern);
    bool match(std::string_view name) const;

  private:
    struct Rule {
        std::string text; // lowercase
        bool from_start = false, to_end = false, exact_length = false;
    };
    std::vector<Rule> rules_;
};

class FileSystem {
  public:
    explicit FileSystem(Kernel &k) : k(k) {}

    Kernel &k;
    // the trees: every entry and the devices' list
    std::recursive_mutex mutex;

    void add_device(std::shared_ptr<Device> device);
    bool remove_device(std::string_view mount_path);
    // whether a mount's path is the path or starts it (at a backslash)
    bool has_device(std::string_view resolved);
    // A symbolic link of the system's (game:, cache:, a content root), in
    // the object manager's list where ObCreateSymbolicLink puts the game's.
    bool add_link(const std::string &path, const std::string &target);
    bool remove_link(std::string_view path);
    bool find_link(std::string_view path, std::string &target);
    bool has_link(std::string_view path); // that very name, any case

    // how the title came (Xenia's XDeploymentType): 0 a disc, 1 installed
    // (a bare executable), 3 other (nothing mounted)
    uint32_t deployment = 3;

    // A guest path, canonical, its symbolic links resolved.
    std::string resolve_links(std::string_view path);
    // The entry a guest path names: nullptr when none. Not with the lock
    // held: the links are the object manager's, under its lock, which is
    // never taken while this one is held (a file object closed under it may
    // take this one to delete its entry).
    Entry *resolve(std::string_view path, std::shared_ptr<Device> *device = nullptr);
    // The entry a path with its links resolved names (takes the lock).
    Entry *find(const std::string &resolved, std::shared_ptr<Device> *device = nullptr);

    struct Opened {
        std::shared_ptr<Device> device;
        std::shared_ptr<Entry> entry;
        std::unique_ptr<Stream> stream;
        uint32_t access = 0;
    };
    // Xenia's VirtualFileSystem::OpenFile: path relative to root when given
    // (an NtCreateFile root directory, on root_device), else a guest path.
    uint32_t open(std::shared_ptr<Device> root_device, Entry *root, std::string_view path, Disposition disposition,
                  uint32_t access, bool directory, bool non_directory, Opened &out, Action &action);
    // removes an entry and its data
    bool remove(Entry &entry);

  private:
    std::vector<std::shared_ptr<Device>> devices_;
};

} // namespace vfs

class IoCompletion;

// A file opened (Xenia's XFile): its entry, its stream, the position, the
// directory search; a dispatcher (an auto-reset event in guest memory,
// signaled after each request, as Xenia's async event) so a wait on it
// returns once its request is done.
class File : public Dispatcher {
  public:
    explicit File(Objects &owner) : Dispatcher(owner, Type::File) {}
    ~File() override;
    uint32_t acquire(Thread &) override { // auto-reset
        set_signal_state(0);
        return X_STATUS_SUCCESS;
    }

    std::shared_ptr<vfs::Device> device;
    std::shared_ptr<vfs::Entry> entry;
    std::unique_ptr<vfs::Stream> stream;
    uint32_t access = 0;
    bool synchronous = false; // FILE_SYNCHRONOUS_IO_*: requests never pend
    bool buffering = true;    // !FILE_NO_INTERMEDIATE_BUFFERING

    std::mutex mutex; // what follows
    uint64_t position = 0;
    vfs::Wildcard find;
    size_t find_index = 0;
    std::vector<std::pair<uint32_t, Ref<IoCompletion>>> ports; // completion key, port
};

// An I/O completion port (Xenia's XIOCompletion): its packets, under the
// objects' lock; its signal state is how many wait.
class IoCompletion : public Dispatcher {
  public:
    explicit IoCompletion(Objects &owner) : Dispatcher(owner, Type::IoCompletion) {}
    struct Packet {
        uint32_t key = 0, apc_context = 0, status = 0, bytes = 0;
    };
    std::deque<Packet> packets;
    uint32_t acquire(Thread &) override {
        set_signal_state(signal_state() - 1);
        return X_STATUS_SUCCESS;
    }
    // queues a packet and wakes a waiter (lock held)
    void post(const Packet &packet);
};

// The files family's state: the file system.
vfs::FileSystem &file_system(Kernel &k);
// The cache partitions under the storage root (xenia_main.cc's mount_cache):
// \CACHE0, \CACHE1 and \CACHE at cache0, cache1 and cache, with their
// links cache0:, cache1: and cache:.
bool mount_cache(Kernel &k, const std::filesystem::path &root, std::string &error);

} // namespace xf::kernel
