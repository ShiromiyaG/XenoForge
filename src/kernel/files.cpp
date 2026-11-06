/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The file exports (files.h): Nt*File, the I/O completion ports, the Io*
// device calls, Fsc* and Stfs*, the symbolic link objects, and the mounts
// boot asks for. Translated from Xenia (BSD; see LICENSE-xenia):
// xboxkrnl_io.cc, xboxkrnl_io_info.cc, xfile.cc, xiocompletion.cc,
// xsymboliclink.cc, emulator.cc and xenia_main.cc (the mounts); checked
// against them.
//
// Requests complete at once, as Xenia completes them: the I/O status block,
// then the event, the APC routine (queued to the calling thread, delivered
// in its next alertable wait) and the completion ports. A file opened
// without FILE_SYNCHRONOUS_IO_* and with FILE_NO_INTERMEDIATE_BUFFERING
// answers STATUS_PENDING for a request that succeeded (Xenia's out_pending),
// its result already in the status block.
//
// Where it differs from Xenia, and why:
//   - a read or write at an explicit offset leaves the position after what
//     it moved (NT's rule; Xenia does not move it to the offset first when
//     the offset is 0, and never for a write), and a scatter or gather
//     request at an explicit offset 0 starts there (Xenia: at the position);
//   - after a write the entry's size is read again, so a query sees it;
//   - NtRemoveIoCompletion waits as NT does (no timeout: forever; Xenia
//     truncates the timeout to 32 bits) and on the scheduler's clock;
//   - a rename moves the entry in the tree too (Xenia renames the host file
//     and the entry's name, but leaves the entry under its old directory);
//   - NtWriteFileGather (absent from Xenia) is NtReadFileScatter's mirror;
//   - the Io* share-access and request calls, IoDismountVolume*, Stfs* and
//     IoInvalidDeviceRequest, which Xenia leaves to its unimplemented-export
//     answer (0), answer as that does: they serve the I/O manager's drivers
//     (the utility drive's FATX in the title's XAPI), whose dispatch
//     routines never run here because these exports answer the requests
//     themselves; IoInvalidDeviceRequest returns NT's status for it.

#include "files.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xf::kernel {
namespace {

// X_IO_STATUS_BLOCK: status, information
void write_iosb(Kernel &k, uint32_t iosb, uint32_t status, uint32_t information) {
    if (!iosb) return;
    k.store32(iosb, status);
    k.store32(iosb + 4, information);
}

// util::TranslateAnsiPath: the X_ANSI_STRING's text, trimmed
std::string guest_path(Kernel &k, uint32_t ansi) {
    std::string s = ansi_string(k, ansi);
    const char *space = " \t\r\n\f\v";
    size_t a = s.find_first_not_of(space);
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(space);
    return s.substr(a, b - a + 1);
}

// xfile.h's IsValidPath: printable ASCII without "+,<>|; * and ? only in a
// pattern, and a * only before a '.'
bool valid_path(std::string_view s, bool pattern) {
    bool asterisk = false;
    for (char ch : s) {
        uint8_t c = uint8_t(ch);
        if (c <= 31 || c >= 127) return false;
        if (asterisk) {
            if (c != '.') return false;
            asterisk = false;
        }
        switch (c) {
        case '"':
        case '+':
        case ',':
        case '<':
        case '>':
        case '|': return false;
        case '*':
            if (!pattern) return false;
            asterisk = true;
            break;
        case '?':
            if (!pattern) return false;
            break;
        default: break;
        }
    }
    return true;
}

// FNV-1a over the path: what Xenia's hash_combine gives a file's
// internal index (any stable number per file)
uint64_t path_hash(uint64_t seed, const std::string &path) {
    uint64_t h = 0xCBF29CE484222325ull ^ seed;
    for (char ch : path) h = (h ^ uint8_t(std::tolower(uint8_t(ch)))) * 0x100000001B3ull;
    return h;
}

// XF_LOG_FILES=1: each path the title opens or asks about, and the answer
bool log_files() {
    static const bool on = [] {
        const char *v = std::getenv("XF_LOG_FILES");
        return v && *v && *v != '0';
    }();
    return on;
}

struct FilesFamily {
    std::once_flag once;
    std::unique_ptr<vfs::FileSystem> fs;
};

Ref<File> file_by_handle(Objects &o, uint32_t handle) { return o.lookup<File>(handle); }

// the file's event and its ports, after a request (lock not held)
void complete(File &f, uint32_t apc_context, uint32_t status, uint32_t bytes) {
    Objects &o = f.owner;
    std::vector<std::pair<uint32_t, Ref<IoCompletion>>> ports;
    {
        std::lock_guard<std::mutex> l(f.mutex);
        ports = f.ports;
    }
    std::lock_guard<std::mutex> l(o.lock);
    for (auto &[key, port] : ports) port->post({key, apc_context, status, bytes});
    f.set_signal_state(1);
    o.signaled(f);
}

constexpr uint64_t CURRENT_POSITION = ~0ull;
constexpr uint64_t USE_FILE_POINTER = ~1ull; // FILE_USE_FILE_POINTER_POSITION

bool at_position(uint64_t offset) { return offset == CURRENT_POSITION || offset == USE_FILE_POINTER; }

// XFile::ReadInternal (f.mutex held)
uint32_t read_locked(Kernel &k, File &f, uint32_t buffer, uint32_t length, uint64_t offset, uint32_t &bytes,
                     bool *pending) {
    if (pending) *pending = false;
    if (at_position(offset)) offset = f.position;
    size_t done = 0;
    uint32_t status = X_STATUS_SUCCESS;
    if (length) {
        if (!buffer || UINT32_MAX - buffer < length) {
            status = X_STATUS_ACCESS_VIOLATION;
        } else {
            status = f.stream ? f.stream->read(offset, k.host(buffer), length, done) : X_STATUS_INVALID_DEVICE_REQUEST;
            if (pending && !f.synchronous) *pending = !f.buffering && status == X_STATUS_SUCCESS;
            if (status == X_STATUS_SUCCESS) f.position = offset + done;
        }
    }
    bytes = uint32_t(done);
    return status;
}

// XFile::Write (f.mutex held)
uint32_t write_locked(Kernel &k, File &f, uint32_t buffer, uint32_t length, uint64_t offset, uint32_t &bytes,
                      bool *pending) {
    if (pending) *pending = false;
    if (at_position(offset)) offset = f.position;
    bool extends = false;
    if (pending && !f.synchronous && !f.buffering && length) {
        std::lock_guard<std::recursive_mutex> l(file_system(k).mutex);
        f.device->update(*f.entry);
        extends = offset > f.entry->size || length > f.entry->size - offset;
    }
    size_t done = 0;
    uint32_t status = !buffer && length ? X_STATUS_ACCESS_VIOLATION
                      : f.stream        ? f.stream->write(offset, k.host(buffer), length, done)
                                        : X_STATUS_INVALID_DEVICE_REQUEST;
    if (pending && !f.synchronous) *pending = !f.buffering && length && !extends && status == X_STATUS_SUCCESS;
    if (status == X_STATUS_SUCCESS) {
        f.position = offset + done;
        std::lock_guard<std::recursive_mutex> l(file_system(k).mutex);
        f.device->update(*f.entry);
    }
    bytes = uint32_t(done);
    return status;
}

// The I/O's end for NtReadFile and its kin: the status block, the APC to the
// calling thread (an apc routine's low bit only keeps it from the ports),
// the event.
struct Request {
    uint32_t event = 0, apc_routine = 0, apc_context = 0, iosb = 0;
};

bool failed(uint32_t status) { return (status & 0xC0000000) != 0; } // XFAILED

// ran: the request was made (its file and event found); clear: a failed
// one's status block says 0 bytes (NtReadFile, NtWriteFile; the scatter
// read keeps what it moved)
uint32_t finish_request(Kernel &k, const Ref<Event> &event, const Request &r, uint32_t status, uint32_t bytes, bool ran,
                        bool queue_apc, bool pending, bool clear = true) {
    Objects &o = objects(k);
    if (ran) {
        write_iosb(k, r.iosb, status, bytes);
        if ((r.apc_routine & ~1u) && r.apc_context && queue_apc) {
            if (Thread *t = current_thread()) {
                std::lock_guard<std::mutex> l(o.lock);
                o.queue_user_apc(*t, r.apc_routine & ~1u, r.apc_context, r.iosb, 0);
            }
        }
    }
    if (failed(status) && (clear || !ran)) write_iosb(k, r.iosb, status, 0);
    if (event && ran) {
        std::lock_guard<std::mutex> l(o.lock);
        event->set_signal_state(1);
        o.signaled(*event);
    }
    return pending ? X_STATUS_PENDING : status;
}

// the event and the file a request names: X_STATUS_SUCCESS, or why not
uint32_t request_objects(Objects &o, uint32_t file_handle, uint32_t event_handle, Ref<File> &file, Ref<Event> &event) {
    uint32_t status = X_STATUS_SUCCESS;
    event = o.lookup<Event>(event_handle);
    if (event_handle && !event) status = X_STATUS_INVALID_HANDLE;
    file = file_by_handle(o, file_handle);
    if (!file) status = X_STATUS_INVALID_HANDLE;
    return status;
}

// NtReadFileScatter and NtWriteFileGather's segments: a page each
template <typename Fn>
uint32_t segmented(Kernel &k, uint32_t segments, uint32_t length, uint64_t offset, uint32_t &total, Fn &&one) {
    total = 0;
    if (!length) return one(0u, 0u, offset, total);
    uint32_t status = X_STATUS_SUCCESS, remain = length;
    while (remain) {
        uint32_t chunk = std::min<uint32_t>(remain, 4096);
        uint32_t buffer = k.load32(segments);
        segments += 4;
        uint32_t done = 0;
        status = one(buffer, chunk, at_position(offset) ? offset : offset + total, done);
        if (status != X_STATUS_SUCCESS) break;
        total += done;
        remain -= chunk;
    }
    return status;
}

} // namespace

vfs::FileSystem &file_system(Kernel &k) {
    FilesFamily &s = k.state<FilesFamily>();
    std::call_once(s.once, [&] { s.fs = std::make_unique<vfs::FileSystem>(k); });
    return *s.fs;
}

File::~File() {
    // FILE_DISPOSITION_INFORMATION's delete on close: the stream closed first
    stream.reset();
    if (entry && entry->delete_on_close && !entry->removed) file_system(owner.k).remove(*entry);
}

void IoCompletion::post(const Packet &packet) {
    packets.push_back(packet);
    set_signal_state(signal_state() + 1);
    owner.signaled(*this);
}

// ---- the mounts (Xenia's Emulator::MountPath, CompleteLaunch, xenia_main) ----

namespace {

// CompleteLaunch: the raw partitions answer with nothing
void mount_null_partitions(vfs::FileSystem &fs) {
    static constexpr const char *path = "\\Device\\Harddisk0";
    std::lock_guard<std::recursive_mutex> l(fs.mutex);
    fs.remove_device(path);
    fs.add_device(vfs::make_null_device(path, {"Partition0", "Cache0", "Cache1"}));
}

void link_game(vfs::FileSystem &fs, const std::string &mount_path) {
    fs.remove_link("GAME:");
    fs.remove_link("D:");
    fs.add_link("GAME:", mount_path);
    fs.add_link("D:", mount_path);
}

} // namespace

bool mount_disc(Kernel &k, const std::filesystem::path &iso, std::string &error) {
    auto disc = Disc::open(iso, error);
    if (!disc) return false;
    vfs::FileSystem &fs = file_system(k);
    fs.add_device(vfs::make_disc_device("\\Device\\Cdrom0", std::move(disc)));
    link_game(fs, "\\Device\\Cdrom0");
    mount_null_partitions(fs);
    fs.deployment = 0; // an optical disc
    return true;
}

bool mount_directory(Kernel &k, const std::filesystem::path &dir, std::string &error) {
    // read-only, as Xenia's default (allow_game_relative_writes off)
    auto device = vfs::make_host_device("\\Device\\Harddisk0\\Partition1", dir, true);
    if (!device) return error = "cannot mount " + dir.string(), false;
    vfs::FileSystem &fs = file_system(k);
    fs.add_device(device);
    link_game(fs, "\\Device\\Harddisk0\\Partition1");
    mount_null_partitions(fs);
    fs.deployment = 1; // installed to the hard drive
    return true;
}

bool mount_cache(Kernel &k, const std::filesystem::path &root, std::string &error) {
    vfs::FileSystem &fs = file_system(k);
    for (const char *name : {"cache0", "cache1", "cache"}) {
        std::string upper = name;
        for (char &ch : upper) ch = char(std::toupper(uint8_t(ch)));
        auto device = vfs::make_host_device("\\" + upper, root / name, false);
        if (!device) return error = "cannot mount " + (root / name).string(), false;
        fs.add_device(device);
        fs.add_link(std::string(name) + ":", "\\" + upper);
    }
    return true;
}

// ---- files ----

namespace {

struct CreateOptions {
    static constexpr uint32_t DIRECTORY_FILE = 0x1, NO_INTERMEDIATE_BUFFERING = 0x8;
    static constexpr uint32_t SYNCHRONOUS_IO_ALERT = 0x10, SYNCHRONOUS_IO_NONALERT = 0x20;
    static constexpr uint32_t NON_DIRECTORY_FILE = 0x40;
};

constexpr uint32_t OB_DOS_DEVICES = 0xFFFFFFFD;

uint32_t create_file(Kernel &k, uint32_t handle_out, uint32_t access, uint32_t attributes, uint32_t iosb,
                     uint32_t disposition, uint32_t options) {
    if (!attributes) return X_STATUS_INVALID_PARAMETER; // some games pass none
    std::string path = guest_path(k, k.load32(attributes + object_attributes::NAME));
    if (!valid_path(path, false)) return X_STATUS_OBJECT_NAME_INVALID;

    Objects &o = objects(k);
    vfs::FileSystem &fs = file_system(k);
    Ref<File> root;
    uint32_t root_handle = k.load32(attributes);
    if (root_handle != OB_DOS_DEVICES && root_handle != 0) {
        root = file_by_handle(o, root_handle);
        if (!root) return X_STATUS_INVALID_HANDLE;
    }

    vfs::FileSystem::Opened opened;
    vfs::Action action = vfs::Action::DoesNotExist;
    uint32_t status = fs.open(root ? root->device : nullptr, root ? root->entry.get() : nullptr, path,
                              vfs::Disposition(disposition), access, (options & CreateOptions::DIRECTORY_FILE) != 0,
                              (options & CreateOptions::NON_DIRECTORY_FILE) != 0, opened, action);
    uint32_t handle = 0xFFFFFFFF; // X_INVALID_HANDLE_VALUE
    if (status == X_STATUS_SUCCESS) {
        Ref<File> f = o.create<File>(GuestType::EventSynchronization, header::SIZE);
        if (!f) {
            status = X_STATUS_NO_MEMORY;
        } else {
            f->device = std::move(opened.device);
            f->entry = std::move(opened.entry);
            f->stream = std::move(opened.stream);
            f->access = opened.access;
            f->synchronous = options & (CreateOptions::SYNCHRONOUS_IO_ALERT | CreateOptions::SYNCHRONOUS_IO_NONALERT);
            f->buffering = !(options & CreateOptions::NO_INTERMEDIATE_BUFFERING);
            handle = f->handle();
        }
    }
    write_iosb(k, iosb, status, uint32_t(action));
    if (handle_out) k.store32(handle_out, handle);
    if (log_files())
        std::fprintf(stderr,
                     "files: open %s (access 0x%08X, disposition %u, options 0x%X): 0x%08X, action %u, handle 0x%08X\n",
                     path.c_str(), access, disposition, options, status, uint32_t(action), handle);
    return status;
}

} // namespace

// (handle_out, desired_access, object_attributes, io_status_block,
// allocation_size, file_attributes, share_access, creation_disposition,
// create_options)
XF_EXPORT(xboxkrnl, NtCreateFile) {
    result(c, create_file(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 7), arg(k, c, 8)));
}

// (handle_out, desired_access, object_attributes, io_status_block,
// share_access, open_options)
XF_EXPORT(xboxkrnl, NtOpenFile) {
    result(c, create_file(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), uint32_t(vfs::Disposition::Open),
                          arg(k, c, 5)));
}

// (file, event, apc_routine, apc_context, io_status_block, buffer, length,
// byte_offset_ptr)
XF_EXPORT(xboxkrnl, NtReadFile) {
    Request r{arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)};
    uint32_t buffer = arg(k, c, 5), length = arg(k, c, 6), offset_ptr = arg(k, c, 7);
    Objects &o = objects(k);
    Ref<File> f;
    Ref<Event> event;
    uint32_t status = request_objects(o, arg(k, c, 0), r.event, f, event);
    uint32_t bytes = 0;
    bool pending = false, ran = status == X_STATUS_SUCCESS;
    if (ran) {
        uint64_t offset = offset_ptr ? k.load64(offset_ptr) : CURRENT_POSITION;
        {
            std::lock_guard<std::mutex> l(f->mutex);
            status = read_locked(k, *f, buffer, length, offset, bytes, &pending);
        }
        complete(*f, r.apc_context, status, bytes);
    }
    result(c, finish_request(k, event, r, status, bytes, ran, status == X_STATUS_SUCCESS, pending));
}

// (file, event, apc_routine, apc_context, io_status_block, segments,
// length, byte_offset_ptr)
XF_EXPORT(xboxkrnl, NtReadFileScatter) {
    Request r{arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)};
    uint32_t segments = arg(k, c, 5), length = arg(k, c, 6), offset_ptr = arg(k, c, 7);
    Objects &o = objects(k);
    Ref<File> f;
    Ref<Event> event;
    uint32_t status = request_objects(o, arg(k, c, 0), r.event, f, event);
    uint32_t bytes = 0;
    bool pending = false, ran = status == X_STATUS_SUCCESS;
    if (ran) {
        uint64_t offset = offset_ptr ? k.load64(offset_ptr) : CURRENT_POSITION;
        {
            std::lock_guard<std::mutex> l(f->mutex);
            status = segmented(k, segments, length, offset, bytes,
                               [&](uint32_t buffer, uint32_t n, uint64_t at, uint32_t &done) {
                                   return read_locked(k, *f, buffer, n, at, done, &pending);
                               });
        }
        complete(*f, r.apc_context, status, bytes);
    }
    result(c, finish_request(k, event, r, status, bytes, ran, true, pending, false));
}

// (file, event, apc_routine, apc_context, io_status_block, buffer, length,
// byte_offset_ptr)
XF_EXPORT(xboxkrnl, NtWriteFile) {
    Request r{arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)};
    uint32_t buffer = arg(k, c, 5), length = arg(k, c, 6), offset_ptr = arg(k, c, 7);
    Objects &o = objects(k);
    Ref<File> f;
    Ref<Event> event;
    uint32_t status = request_objects(o, arg(k, c, 0), r.event, f, event);
    uint32_t bytes = 0;
    bool pending = false, ran = status == X_STATUS_SUCCESS;
    if (ran) {
        uint64_t offset = offset_ptr ? k.load64(offset_ptr) : CURRENT_POSITION;
        {
            std::lock_guard<std::mutex> l(f->mutex);
            status = write_locked(k, *f, buffer, length, offset, bytes, &pending);
        }
        complete(*f, r.apc_context, status, bytes);
    }
    result(c, finish_request(k, event, r, status, bytes, ran, true, pending));
}

// (file, event, apc_routine, apc_context, io_status_block, segments,
// length, byte_offset_ptr)
XF_EXPORT(xboxkrnl, NtWriteFileGather) {
    Request r{arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)};
    uint32_t segments = arg(k, c, 5), length = arg(k, c, 6), offset_ptr = arg(k, c, 7);
    Objects &o = objects(k);
    Ref<File> f;
    Ref<Event> event;
    uint32_t status = request_objects(o, arg(k, c, 0), r.event, f, event);
    uint32_t bytes = 0;
    bool pending = false, ran = status == X_STATUS_SUCCESS;
    if (ran) {
        uint64_t offset = offset_ptr ? k.load64(offset_ptr) : CURRENT_POSITION;
        {
            std::lock_guard<std::mutex> l(f->mutex);
            status = segmented(k, segments, length, offset, bytes,
                               [&](uint32_t buffer, uint32_t n, uint64_t at, uint32_t &done) {
                                   return write_locked(k, *f, buffer, n, at, done, &pending);
                               });
        }
        complete(*f, r.apc_context, status, bytes);
    }
    result(c, finish_request(k, event, r, status, bytes, ran, true, pending, false));
}

// ---- information (xboxkrnl_io_info.cc) ----

namespace {

enum InfoClass : uint32_t {
    FileBasicInformation = 4,
    FileInternalInformation = 6,
    FileNameInformation = 9,
    FileRenameInformation = 10,
    FileLinkInformation = 11,
    FileDispositionInformation = 13,
    FilePositionInformation = 14,
    FileModeInformation = 16,
    FileAlignmentInformation = 17,
    FileAllocationInformation = 19,
    FileEndOfFileInformation = 20,
    FileMountPartitionInformation = 23,
    FileMountPartitionsInformation = 24,
    FileSectorInformation = 26,
    FileXctdCompressionInformation = 27,
    FileCompletionInformation = 30,
    FileIoPriorityInformation = 32,
    FileNetworkOpenInformation = 34,
};

uint32_t query_minimum(uint32_t c) {
    switch (c) {
    case FileInternalInformation:
    case FilePositionInformation: return 8;
    case FileXctdCompressionInformation: return 4;
    case FileNetworkOpenInformation: return 56;
    case FileModeInformation:
    case FileAlignmentInformation:
    case FileSectorInformation:
    case FileIoPriorityInformation: return 4;
    case FileNameInformation:
    case FileAllocationInformation: return 8;
    case FileBasicInformation: return 40;
    default: return 0;
    }
}

uint32_t set_minimum(uint32_t c) {
    switch (c) {
    case FileRenameInformation: return 16;
    case FileDispositionInformation: return 1;
    case FilePositionInformation: return 8;
    case FileCompletionInformation: return 8;
    case FileModeInformation:
    case FileIoPriorityInformation: return 4;
    case FileAllocationInformation:
    case FileEndOfFileInformation:
    case FileMountPartitionInformation: return 8;
    case FileLinkInformation: return 16;
    case FileBasicInformation: return 40;
    case FileMountPartitionsInformation: return 152;
    default: return 0;
    }
}

// X_FILE_NETWORK_OPEN_INFORMATION (56 bytes, its pad left alone)
void write_network_open(Kernel &k, uint32_t at, const vfs::Entry &e) {
    k.store64(at + 0x00, e.create_time);
    k.store64(at + 0x08, e.access_time);
    k.store64(at + 0x10, e.write_time);
    k.store64(at + 0x18, e.write_time);
    k.store64(at + 0x20, e.allocation_size);
    k.store64(at + 0x28, e.size);
    k.store32(at + 0x30, e.attributes);
}

// which information classes reached the default case, logged once each
void unhandled_class(const char *what, uint32_t c) {
    static std::mutex m;
    static std::vector<std::pair<const char *, uint32_t>> seen;
    std::lock_guard<std::mutex> l(m);
    for (auto &s : seen)
        if (s.first == what && s.second == c) return;
    seen.push_back({what, c});
    std::fprintf(stderr, "files: %s: information class %u is not answered\n", what, c);
}

// a rename: the entry moved to the new name's directory, on its device
uint32_t rename_entry(Kernel &k, File &f, const std::string &target, bool replace) {
    vfs::FileSystem &fs = file_system(k);
    std::string canonical = vfs::canonical(target);
    size_t cut = canonical.find_last_of('\\');
    if (cut == std::string::npos || cut + 1 == canonical.size()) return X_STATUS_INVALID_PARAMETER;
    std::string base = canonical.substr(0, cut), name = canonical.substr(cut + 1);
    std::string resolved_base = fs.resolve_links(base);
    std::shared_ptr<vfs::Device> device;
    std::lock_guard<std::recursive_mutex> l(fs.mutex);
    vfs::Entry *dir = fs.find(resolved_base, &device);
    vfs::Entry &e = *f.entry;
    if (!dir || !dir->directory() || dir->device != e.device || !e.parent) return X_STATUS_OBJECT_PATH_NOT_FOUND;
    if (e.device->read_only) return X_STATUS_ACCESS_DENIED;
    if (vfs::Entry *existing = dir->child(name); existing && existing != &e) {
        if (!replace) return X_STATUS_OBJECT_NAME_COLLISION;
        if (!fs.remove(*existing)) return X_STATUS_ACCESS_DENIED;
    }
    std::filesystem::path to = dir->host_path / vfs::host_name(name);
    std::error_code ec;
    std::filesystem::rename(e.host_path, to, ec);
    if (ec) {
        std::fprintf(stderr, "files: cannot rename %s to %s: %s\n", e.host_path.string().c_str(), to.string().c_str(),
                     ec.message().c_str());
        return X_STATUS_ACCESS_DENIED;
    }
    std::shared_ptr<vfs::Entry> keep = e.parent->shared_child(&e);
    std::erase_if(e.parent->children, [&](const auto &child) { return child.get() == &e; });
    e.parent = dir;
    e.name = name;
    e.path = dir->path.empty() ? name : dir->path + "\\" + name;
    e.host_path = to;
    dir->children.push_back(keep);
    // what is under a directory moves with it
    std::function<void(vfs::Entry &)> repath = [&](vfs::Entry &p) {
        for (auto &child : p.children) {
            child->path = p.path + "\\" + child->name;
            child->host_path = p.host_path / vfs::host_name(child->name);
            repath(*child);
        }
    };
    repath(e);
    return X_STATUS_SUCCESS;
}

} // namespace

// (file, io_status_block, info, length, class)
XF_EXPORT(xboxkrnl, NtQueryInformationFile) {
    uint32_t iosb = arg(k, c, 1), info = arg(k, c, 2), length = arg(k, c, 3), info_class = arg(k, c, 4);
    uint32_t minimum = query_minimum(info_class);
    if (!minimum) return result(c, X_STATUS_INVALID_INFO_CLASS);
    if (length < minimum) return result(c, X_STATUS_INFO_LENGTH_MISMATCH);
    Ref<File> f = file_by_handle(objects(k), arg(k, c, 0));
    if (!f) return result(c, X_STATUS_INVALID_HANDLE);
    std::memset(k.host(info), 0, length);
    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (info_class) {
    case FileInternalInformation:
        k.store64(info, path_hash(0, f->entry->path));
        out = 8;
        break;
    case FilePositionInformation: {
        std::lock_guard<std::mutex> l(f->mutex);
        k.store64(info, f->position);
        out = 8;
        break;
    }
    case FileAlignmentInformation: // the XMountUtilityDrive task asks
        k.store32(info, 0);
        out = 4;
        break;
    case FileSectorInformation: { // a number naming the file
        uint64_t h = path_hash(82589933, f->entry->path);
        k.store32(info, uint32_t(h ^ (h >> 32)));
        out = 4;
        break;
    }
    case FileXctdCompressionInformation: status = X_STATUS_INVALID_PARAMETER; break;
    case FileNetworkOpenInformation: {
        std::lock_guard<std::recursive_mutex> l(file_system(k).mutex);
        f->device->update(*f->entry);
        write_network_open(k, info, *f->entry);
        out = 56;
        break;
    }
    default:
        unhandled_class("NtQueryInformationFile", info_class);
        status = X_STATUS_INVALID_PARAMETER;
        break;
    }
    write_iosb(k, iosb, status, out);
    result(c, status);
}

// (file, io_status_block, info, length, class)
XF_EXPORT(xboxkrnl, NtSetInformationFile) {
    uint32_t iosb = arg(k, c, 1), info = arg(k, c, 2), length = arg(k, c, 3), info_class = arg(k, c, 4);
    uint32_t minimum = set_minimum(info_class);
    if (!minimum) return result(c, X_STATUS_INVALID_INFO_CLASS);
    if (length < minimum) return result(c, X_STATUS_INFO_LENGTH_MISMATCH);
    Ref<File> f = file_by_handle(objects(k), arg(k, c, 0));
    if (!f) return result(c, X_STATUS_INVALID_HANDLE);
    vfs::FileSystem &fs = file_system(k);
    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (info_class) {
    case FileBasicInformation: { // times (kept nowhere), attributes
        std::lock_guard<std::recursive_mutex> l(fs.mutex);
        bool ok = true;
        for (uint32_t at : {0u, 8u, 16u})
            if (k.load64(info + at)) ok &= f->device->set_time(*f->entry);
        ok &= f->device->set_attributes(*f->entry, uint32_t(k.load64(info + 0x20)));
        if (!ok) status = X_STATUS_UNSUCCESSFUL;
        out = 40;
        break;
    }
    case FileRenameInformation: { // replace_existing, root directory, the name
        std::string target = guest_path(k, info + 8);
        if (!valid_path(target, false)) return result(c, X_STATUS_OBJECT_NAME_INVALID);
        status = rename_entry(k, *f, target, k.load32(info) != 0);
        if (status == X_STATUS_INVALID_PARAMETER) return result(c, status);
        out = 16;
        break;
    }
    case FileDispositionInformation:
        f->entry->delete_on_close = k.host(info)[0] != 0;
        out = 0;
        break;
    case FilePositionInformation: {
        std::lock_guard<std::mutex> l(f->mutex);
        f->position = k.load64(info);
        out = 8;
        break;
    }
    case FileAllocationInformation: out = 8; break; // ignored, as Xenia
    case FileEndOfFileInformation: {
        status = f->stream ? f->stream->set_length(k.load64(info)) : X_STATUS_INVALID_DEVICE_REQUEST;
        out = 8;
        std::lock_guard<std::recursive_mutex> l(fs.mutex);
        f->device->update(*f->entry);
        break;
    }
    case FileCompletionInformation: { // the port's handle, the key
        Ref<IoCompletion> port = objects(k).lookup<IoCompletion>(k.load32(info));
        out = 8;
        if (!port) {
            status = X_STATUS_INVALID_HANDLE;
        } else {
            std::lock_guard<std::mutex> l(f->mutex);
            f->ports.push_back({k.load32(info + 4), port});
        }
        break;
    }
    default: unhandled_class("NtSetInformationFile", info_class); break;
    }
    write_iosb(k, iosb, status, out);
    result(c, status);
}

// (file, io_status_block, info, length, class)
XF_EXPORT(xboxkrnl, NtQueryVolumeInformationFile) {
    uint32_t iosb = arg(k, c, 1), info = arg(k, c, 2), length = arg(k, c, 3), info_class = arg(k, c, 4);
    enum : uint32_t { Volume = 1, Size = 3, Device = 4, Attribute = 5 };
    uint32_t minimum = info_class == Volume      ? 0x18
                       : info_class == Size      ? 0x18
                       : info_class == Device    ? 8
                       : info_class == Attribute ? 0x10
                                                 : 0;
    if (!minimum) {
        unhandled_class("NtQueryVolumeInformationFile", info_class);
        return result(c, X_STATUS_INVALID_INFO_CLASS);
    }
    if (length < minimum) return result(c, X_STATUS_INFO_LENGTH_MISMATCH);
    Ref<File> f = file_by_handle(objects(k), arg(k, c, 0));
    if (!f) return result(c, X_STATUS_INVALID_HANDLE);
    std::memset(k.host(info), 0, length);
    vfs::Device &d = *f->device;
    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (info_class) {
    case Volume: // no creation time, serial number or label
        out = 0x11;
        break;
    case Size:
        k.store64(info + 0, d.total_units);
        k.store64(info + 8, d.available_units);
        k.store32(info + 16, d.sectors_per_unit);
        k.store32(info + 20, d.bytes_per_sector);
        out = 0x18;
        break;
    case Device:
        k.store32(info, 0x22); // FILE_DEVICE_UNKNOWN
        k.store32(info + 4, 0);
        out = 8;
        break;
    case Attribute:
        k.store32(info + 0, d.attributes);
        k.store32(info + 4, d.component_name_max_length);
        k.store32(info + 8, uint32_t(d.name.size()));
        if (length >= 12 + d.name.size()) {
            std::memcpy(k.host(info + 12), d.name.data(), d.name.size());
            out = 12 + uint32_t(d.name.size());
        } else {
            status = X_STATUS_BUFFER_OVERFLOW;
            out = 12;
        }
        break;
    }
    write_iosb(k, iosb, status, out);
    result(c, status);
}

// (object_attributes, X_FILE_NETWORK_OPEN_INFORMATION *)
XF_EXPORT(xboxkrnl, NtQueryFullAttributesFile) {
    uint32_t attributes = arg(k, c, 0), info = arg(k, c, 1);
    std::string path = guest_path(k, k.load32(attributes + object_attributes::NAME));
    if (!valid_path(path, false)) return result(c, X_STATUS_OBJECT_NAME_INVALID);
    vfs::FileSystem &fs = file_system(k);
    std::string resolved = fs.resolve_links(path);
    std::lock_guard<std::recursive_mutex> l(fs.mutex);
    vfs::Entry *e = fs.find(resolved);
    if (log_files()) std::fprintf(stderr, "files: attributes of %s: %s\n", path.c_str(), e ? "found" : "none");
    if (!e) return result(c, X_STATUS_NO_SUCH_FILE);
    write_network_open(k, info, *e);
    result(c, X_STATUS_SUCCESS);
}

// (file, event, apc_routine, apc_context, io_status_block, info, length,
// file_name, restart_scan): one entry a call (XFile::QueryDirectory)
XF_EXPORT(xboxkrnl, NtQueryDirectoryFile) {
    uint32_t iosb = arg(k, c, 4), info = arg(k, c, 5), length = arg(k, c, 6), name_ptr = arg(k, c, 7);
    bool restart = arg(k, c, 8) != 0;
    if (length < 72) return result(c, X_STATUS_INFO_LENGTH_MISMATCH);
    Ref<File> f = file_by_handle(objects(k), arg(k, c, 0));
    std::string name = guest_path(k, name_ptr);
    if (!valid_path(name, true)) return result(c, X_STATUS_INVALID_PARAMETER);
    uint32_t status = X_STATUS_NO_SUCH_FILE, out = 0;
    if (f) {
        vfs::FileSystem &fs = file_system(k);
        std::lock_guard<std::mutex> fl(f->mutex);
        std::lock_guard<std::recursive_mutex> l(fs.mutex);
        auto next = [&]() -> vfs::Entry * {
            auto &children = f->entry->children;
            while (f->find_index < children.size()) {
                vfs::Entry *e = children[f->find_index++].get();
                if (f->find.match(e->name)) return e;
            }
            return nullptr;
        };
        vfs::Entry *e = nullptr;
        if (!name.empty()) {
            // only this directory's names; the search starts over
            f->find.set(name);
            f->find_index = 0;
            e = next();
            status = e ? X_STATUS_SUCCESS : X_STATUS_NO_SUCH_FILE;
        } else {
            if (restart) f->find_index = 0;
            e = next();
            status = e ? X_STATUS_SUCCESS : X_STATUS_NO_MORE_FILES;
        }
        if (e && 0x40 + e->name.size() > length) status = X_STATUS_NO_SUCH_FILE;
        if (status == X_STATUS_SUCCESS) {
            // X_FILE_DIRECTORY_INFORMATION
            k.store32(info + 0x00, 0); // next entry offset
            k.store32(info + 0x04, uint32_t(f->find_index));
            k.store64(info + 0x08, e->create_time);
            k.store64(info + 0x10, e->access_time);
            k.store64(info + 0x18, e->write_time);
            k.store64(info + 0x20, e->write_time);
            k.store64(info + 0x28, e->size);
            k.store64(info + 0x30, e->allocation_size);
            k.store32(info + 0x38, e->attributes);
            k.store32(info + 0x3C, uint32_t(e->name.size()));
            std::memcpy(k.host(info + 0x40), e->name.data(), e->name.size());
            out = length;
        }
    }
    write_iosb(k, iosb, status, out);
    result(c, status);
}

// (file, io_status_block): nothing is buffered
XF_EXPORT(xboxkrnl, NtFlushBuffersFile) {
    write_iosb(k, arg(k, c, 1), X_STATUS_SUCCESS, 0);
    result(c, X_STATUS_SUCCESS);
}

// (file): requests complete at once, so none waits to be cancelled
XF_EXPORT(xboxkrnl, NtCancelIoFile) {
    result(c, file_by_handle(objects(k), arg(k, c, 0)) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE);
}

// (file, event, apc_routine, apc_context, io_status_block, code, input,
// input_length, output, output_length): the cache partition's geometry for
// XMountUtilityDrive, whichever file asks (as Xenia)
XF_EXPORT(xboxkrnl, NtDeviceIoControlFile) {
    uint32_t code = arg(k, c, 5), output = arg(k, c, 8), output_length = arg(k, c, 9);
    constexpr uint32_t CACHE_SIZE = 0xFF000;
    constexpr uint32_t GET_DRIVE_GEOMETRY = 0x70000, GET_PARTITION_INFO = 0x74004;
    if (code == GET_DRIVE_GEOMETRY) {
        if (output_length < 8) return result(c, X_STATUS_BUFFER_TOO_SMALL);
        k.store32(output, CACHE_SIZE / 0x200); // sectors
        k.store32(output + 4, 0x200);          // their size
    } else if (code == GET_PARTITION_INFO) {
        if (output_length < 16) return result(c, X_STATUS_BUFFER_TOO_SMALL);
        k.store64(output, 0);
        k.store64(output + 8, CACHE_SIZE);
    } else {
        std::fprintf(stderr, "files: NtDeviceIoControlFile(0x%X) is not answered\n", code);
        return result(c, X_STATUS_INVALID_PARAMETER);
    }
    result(c, X_STATUS_SUCCESS);
}

// ---- I/O completion ports (xiocompletion.cc) ----

// (handle_out, desired_access, object_attributes, concurrent_threads)
XF_EXPORT(xboxkrnl, NtCreateIoCompletion) {
    uint32_t handle_out = arg(k, c, 0);
    Ref<IoCompletion> port = objects(k).create<IoCompletion>(GuestType::Queue, header::SIZE);
    if (!port) return result(c, X_STATUS_NO_MEMORY);
    if (handle_out) k.store32(handle_out, port->handle());
    result(c, X_STATUS_SUCCESS);
}

// (port, key_context, apc_context, completion_status, bytes)
XF_EXPORT(xboxkrnl, NtSetIoCompletion) {
    Objects &o = objects(k);
    Ref<IoCompletion> port = o.lookup<IoCompletion>(arg(k, c, 0));
    if (!port) return result(c, X_STATUS_INVALID_HANDLE);
    std::lock_guard<std::mutex> l(o.lock);
    port->post({arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4)});
    result(c, X_STATUS_SUCCESS);
}

// (port, key_context_ptr, apc_context_ptr, io_status_block, timeout_ptr):
// the oldest packet, waiting for one until the timeout
XF_EXPORT(xboxkrnl, NtRemoveIoCompletion) {
    uint32_t key_ptr = arg(k, c, 1), apc_ptr = arg(k, c, 2), iosb = arg(k, c, 3), timeout_ptr = arg(k, c, 4);
    Objects &o = objects(k);
    Ref<IoCompletion> port = o.lookup<IoCompletion>(arg(k, c, 0));
    if (!port) return result(c, X_STATUS_INVALID_HANDLE);
    Thread *t = current_thread();
    if (t) {
        Dispatcher *list[] = {port.get()};
        if (o.wait(*t, list, false, false, o.deadline(timeout_ptr)) != X_STATUS_SUCCESS)
            return result(c, X_STATUS_TIMEOUT);
    } else { // off a guest thread: what is there now
        std::lock_guard<std::mutex> l(o.lock);
        if (port->packets.empty()) return result(c, X_STATUS_TIMEOUT);
        port->set_signal_state(port->signal_state() - 1);
    }
    IoCompletion::Packet p;
    {
        std::lock_guard<std::mutex> l(o.lock);
        p = port->packets.front();
        port->packets.pop_front();
    }
    if (key_ptr) k.store32(key_ptr, p.key);
    if (apc_ptr) k.store32(apc_ptr, p.apc_context);
    write_iosb(k, iosb, p.status, p.bytes);
    result(c, X_STATUS_SUCCESS);
}

// ---- devices and the rest of Io*, Fsc*, Stfs* ----

// (driver_object, extension_size, device_name, device_type,
// extra_attributes, device_object_ptr): an X_DEVICE_OBJECT (0x50) and its
// extension, for XMountUtilityDrive's task (Xenia's IoCreateDevice)
XF_EXPORT(xboxkrnl, IoCreateDevice) {
    uint32_t driver = arg(k, c, 0), extension_size = arg(k, c, 1), name = arg(k, c, 2), type = arg(k, c, 3);
    uint32_t out_ptr = arg(k, c, 5);
    uint32_t size = 0x50 + ((extension_size + 7) & ~7u);
    uint32_t d = k.memory().system_alloc(size);
    if (!d) return result(c, X_STATUS_NO_MEMORY);
    std::memset(k.host(d), 0, size);
    k.store16(d + 0x00, 3);
    k.store16(d + 0x02, uint16_t(extension_size + 0x50)); // the size, unaligned
    static constexpr uint32_t unmounted[] = {7, 58, 62, 45, 2, 60, 61, 36, 64, 65, 66, 67, 68, 69, 70, 72, 73};
    bool self = std::find(std::begin(unmounted), std::end(unmounted), type) == std::end(unmounted);
    k.store32(d + 0x0C, self ? d : 0); // mounted or self device
    k.host(d)[0x1C] = uint8_t(type);
    k.host(d)[0x1E] = 1; // stack size
    k.store32(d + 0x14, 16 | (name ? 8 : 0));
    if (extension_size) k.store32(d + 0x18, d + 0x50);
    k.store32(d + 0x08, driver);
    if (out_ptr) k.store32(out_ptr, d);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, IoDeleteDevice) {
    if (uint32_t d = arg(k, c, 0)) k.memory().system_free(d);
}

// the I/O manager's driver calls (see the top of the file)
XF_EXPORT(xboxkrnl, IoCheckShareAccess) { result(c, X_STATUS_SUCCESS); }
XF_EXPORT(xboxkrnl, IoSetShareAccess) {}
XF_EXPORT(xboxkrnl, IoRemoveShareAccess) {}
XF_EXPORT(xboxkrnl, IoCompleteRequest) {}
XF_EXPORT(xboxkrnl, IoInvalidDeviceRequest) { result(c, X_STATUS_INVALID_DEVICE_REQUEST); }
XF_EXPORT(xboxkrnl, IoDismountVolume) { result(c, X_STATUS_SUCCESS); }
XF_EXPORT(xboxkrnl, IoDismountVolumeByFileHandle) { result(c, X_STATUS_SUCCESS); }

// the file system cache's size: nothing is cached here
XF_EXPORT(xboxkrnl, FscSetCacheElementCount) { result(c, X_STATUS_SUCCESS); }
XF_EXPORT(xboxkrnl, FscGetCacheElementCount) { result(c, 0); }

// ---- symbolic link objects (Xenia's XSymbolicLink) ----

namespace {
// a link opened by name: its name and target, no dispatcher header
class SymbolicLink : public Object {
  public:
    explicit SymbolicLink(Objects &owner) : Object(owner, Type::Other) {}
    std::string path, target;
};
} // namespace

// (handle out, attributes): a handle to the link that starts the name (the
// object manager's \??\ prefix dropped), as VirtualFileSystem::FindSymbolicLink
// finds it. The prefix is dropped before the name is checked: Xenia checks
// it first, and its check refuses the '?', so a name with the prefix never
// opens there.
XF_EXPORT(xboxkrnl, NtOpenSymbolicLinkObject) {
    uint32_t handle_out = arg(k, c, 0), attributes = arg(k, c, 1);
    if (!attributes) return result(c, X_STATUS_INVALID_PARAMETER);
    std::string path = guest_path(k, k.load32(attributes + object_attributes::NAME));
    if (path.starts_with("\\??\\")) path = path.substr(4);
    if (!valid_path(path, false)) return result(c, X_STATUS_OBJECT_NAME_INVALID);
    std::string target;
    if (!file_system(k).find_link(path, target)) return result(c, X_STATUS_NO_SUCH_FILE);
    Objects &o = objects(k);
    Ref<SymbolicLink> link(new SymbolicLink(o));
    link->path = path, link->target = target;
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t handle = o.add_handle(link.get());
    if (handle_out) k.store32(handle_out, handle);
    result(c, X_STATUS_SUCCESS);
}

// (handle, target: X_ANSI_STRING): the target, as much as its buffer holds,
// unterminated
XF_EXPORT(xboxkrnl, NtQuerySymbolicLinkObject) {
    uint32_t target = arg(k, c, 1);
    Objects &o = objects(k);
    Ref<Object> object = o.lookup<Object>(arg(k, c, 0));
    auto *link = dynamic_cast<SymbolicLink *>(object.get());
    if (!link) return result(c, X_STATUS_NO_SUCH_FILE);
    uint32_t length = std::min<uint32_t>(k.load16(target + 2), uint32_t(link->target.size()));
    if (length) std::memcpy(k.host(k.load32(target + 4)), link->target.data(), length);
    k.store16(target, uint16_t(length));
    result(c, X_STATUS_SUCCESS);
}

// An STFS package mounted by the title's own code: no device is made (the
// content family mounts packages as directories, as Xenia does)
XF_EXPORT(xboxkrnl, StfsCreateDevice) {
    static std::once_flag once;
    std::call_once(once, [&] {
        std::fprintf(stderr, "files: StfsCreateDevice (r3 0x%08X, from 0x%08X) makes no device\n", uint32_t(c.r[3]),
                     uint32_t(c.lr));
    });
    result(c, X_STATUS_SUCCESS);
}
XF_EXPORT(xboxkrnl, StfsControlDevice) { result(c, X_STATUS_SUCCESS); }

} // namespace xf::kernel
