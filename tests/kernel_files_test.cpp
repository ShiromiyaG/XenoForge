// The files family and XAM (src/kernel/files.h, xam.h), driven through
// their exports as a game calls them, on a guest thread: a tiny XDVDFS disc
// made here mounted as the game (game:, d:, \Device\Cdrom0), the storage
// root in a temporary directory (cache:, content); files created, read,
// written, queried, listed, renamed and deleted; symbolic links;
// asynchronous requests completed through an event, an APC and an I/O
// completion port; content created, enumerated, closed, opened and deleted
// on the host in Xenia's layout; the notifications, the user and its
// profile settings, input, the loader's launch data.

#include "check.h"

#include "files.h"
#include "xam.h"
#include "xenoforge/input.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
std::filesystem::path storage;
uint32_t scratch = 0, scratch_next = 0;

// guest memory for one call's structures (reset by each scenario)
uint32_t alloc(uint32_t size) {
    uint32_t p = scratch + scratch_next;
    scratch_next += (size + 15) & ~15u;
    std::memset(K->host(p), 0, size);
    return p;
}

// an export, as the code calls it: r3-r10, then the stack (a 32-bit value
// in the word at r1 + 0x54 + 8 * (n - 8))
uint32_t call(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s!%s\n", library, name);
        failures++;
        return 0xFFFFFFFF;
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
    return uint32_t(c.r[3]);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call(c, "xboxkrnl", name, args);
}
uint32_t xam(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call(c, "xam", name, args);
}

// X_ANSI_STRING and X_OBJECT_ATTRIBUTES for a path
uint32_t ansi(const std::string &s) {
    uint32_t text = alloc(uint32_t(s.size() + 1));
    std::memcpy(K->host(text), s.data(), s.size());
    uint32_t str = alloc(8);
    K->store16(str, uint16_t(s.size()));
    K->store16(str + 2, uint16_t(s.size() + 1));
    K->store32(str + 4, text);
    return str;
}
uint32_t attributes(const std::string &path, uint32_t root = 0) {
    uint32_t a = alloc(12);
    K->store32(a, root);
    K->store32(a + 4, ansi(path));
    K->store32(a + 8, 0x40);
    return a;
}

constexpr uint32_t GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, SYNCHRONIZE = 0x00100000;
constexpr uint32_t OPEN = 1, CREATE = 2, OPEN_IF = 3;
constexpr uint32_t DIRECTORY_FILE = 0x1, NO_BUFFERING = 0x8, SYNCHRONOUS = 0x20, NON_DIRECTORY = 0x40;

// NtCreateFile: the handle (0xFFFFFFFF when none), the status and action
struct Opened {
    uint32_t handle, status, action;
};
Opened create(ppc::Context &c, const std::string &path, uint32_t access, uint32_t disposition, uint32_t options,
              uint32_t root = 0) {
    uint32_t handle = alloc(4), iosb = alloc(8);
    uint32_t status =
        krnl(c, "NtCreateFile", {handle, access, attributes(path, root), iosb, 0, 0, 0, disposition, options});
    return {K->load32(handle), status, K->load32(iosb + 4)};
}

std::string read_file(ppc::Context &c, uint32_t handle, uint32_t length, int64_t offset, uint32_t *status = nullptr) {
    uint32_t buffer = alloc(length + 1), iosb = alloc(8), off = 0;
    if (offset >= 0) {
        off = alloc(8);
        K->store64(off, uint64_t(offset));
    }
    uint32_t s = krnl(c, "NtReadFile", {handle, 0, 0, 0, iosb, buffer, length, off});
    if (status) *status = s;
    if (s != X_STATUS_SUCCESS) return {};
    CHECK(K->load32(iosb) == X_STATUS_SUCCESS);
    return std::string(reinterpret_cast<const char *>(K->host(buffer)), K->load32(iosb + 4));
}

uint32_t write_file(ppc::Context &c, uint32_t handle, const std::string &data, int64_t offset) {
    uint32_t buffer = alloc(uint32_t(data.size())), iosb = alloc(8), off = 0;
    std::memcpy(K->host(buffer), data.data(), data.size());
    if (offset >= 0) {
        off = alloc(8);
        K->store64(off, uint64_t(offset));
    }
    uint32_t s = krnl(c, "NtWriteFile", {handle, 0, 0, 0, iosb, buffer, uint32_t(data.size()), off});
    if (s == X_STATUS_SUCCESS) CHECK(K->load32(iosb + 4) == data.size());
    return s;
}

uint64_t size_of(ppc::Context &c, uint32_t handle) {
    uint32_t info = alloc(56), iosb = alloc(8);
    CHECK(krnl(c, "NtQueryInformationFile", {handle, iosb, info, 56, 34}) == X_STATUS_SUCCESS); // NetworkOpen
    return K->load64(info + 0x28);
}

void close(ppc::Context &c, uint32_t handle) { CHECK(krnl(c, "NtClose", {handle}) == X_STATUS_SUCCESS); }

std::string host_read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---- the disc ----

constexpr size_t SECTOR = 2048;

void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

void disc_entry(uint8_t *table, uint16_t slot, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
                uint8_t attributes, const char *name) {
    uint8_t *p = table + slot * 4;
    p[0] = uint8_t(left), p[1] = uint8_t(left >> 8), p[2] = uint8_t(right), p[3] = uint8_t(right >> 8);
    put_le32(p + 4, sector);
    put_le32(p + 8, size);
    p[12] = attributes;
    p[13] = uint8_t(std::strlen(name));
    std::memcpy(p + 14, name, std::strlen(name));
}

// default.xex and media\ (a.bin: 0..255 twice, b.txt, c.txt)
std::filesystem::path make_disc(const std::filesystem::path &dir) {
    std::vector<uint8_t> image(40 * SECTOR, 0);
    uint8_t *volume = image.data() + 32 * SECTOR;
    std::memcpy(volume, "MICROSOFT*XBOX*MEDIA", 20);
    put_le32(volume + 20, 33);
    put_le32(volume + 24, 2048);
    uint8_t *root = image.data() + 33 * SECTOR;
    std::memset(root, 0xFF, SECTOR);
    disc_entry(root, 0, 5, 0, 34, 2048, 0x10, "media");
    disc_entry(root, 5, 0, 0, 38, 8, 0x00, "default.xex");
    uint8_t *media = image.data() + 34 * SECTOR;
    std::memset(media, 0xFF, SECTOR);
    disc_entry(media, 0, 6, 12, 36, 5, 0x00, "b.txt");
    disc_entry(media, 6, 0, 0, 35, 512, 0x00, "a.bin");
    disc_entry(media, 12, 0, 0, 37, 4, 0x00, "c.txt");
    for (int i = 0; i < 512; i++) image[35 * SECTOR + i] = uint8_t(i);
    std::memcpy(image.data() + 36 * SECTOR, "hello", 5);
    std::memcpy(image.data() + 37 * SECTOR, "four", 4);
    std::memcpy(image.data() + 38 * SECTOR, "XEX2abcd", 8);
    auto path = dir / "disc.iso";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(image.data()), std::streamsize(image.size()));
    return path;
}

// ---- the guest functions ----

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_IO_APC = 0x82000010,
    F_COMPLETION = 0x82000020,
};

std::atomic<uint32_t> apc_calls{0};
uint32_t apc_args[3];

// an I/O APC: (context, io_status_block, 0)
void io_apc(ppc::Context &c) {
    apc_args[0] = uint32_t(c.r[3]), apc_args[1] = uint32_t(c.r[4]), apc_args[2] = uint32_t(c.r[5]);
    apc_calls++;
}

// an overlapped request's completion routine: (result, length, overlapped)
void completion(ppc::Context &c) {
    apc_args[0] = uint32_t(c.r[3]), apc_args[1] = uint32_t(c.r[4]), apc_args[2] = uint32_t(c.r[5]);
    apc_calls++;
}

// ---- the scenarios ----

void disc_files(ppc::Context &c) {
    scratch_next = 0;
    // through game:, any case, and the device's own path
    Opened f = create(c, "game:\\media\\a.bin", GENERIC_READ | SYNCHRONIZE, OPEN, SYNCHRONOUS | NON_DIRECTORY);
    CHECK(f.status == X_STATUS_SUCCESS && f.action == 1 && f.handle != 0xFFFFFFFF);
    std::string data = read_file(c, f.handle, 16, 4);
    CHECK(data.size() == 16 && uint8_t(data[0]) == 4 && uint8_t(data[15]) == 19);
    // the position follows: the next read without an offset goes on
    data = read_file(c, f.handle, 4, -1);
    CHECK(data.size() == 4 && uint8_t(data[0]) == 20);
    uint32_t pos = alloc(8), iosb = alloc(8);
    CHECK(krnl(c, "NtQueryInformationFile", {f.handle, iosb, pos, 8, 14}) == X_STATUS_SUCCESS);
    CHECK(K->load64(pos) == 24 && K->load32(iosb + 4) == 8);
    K->store64(pos, 510);
    CHECK(krnl(c, "NtSetInformationFile", {f.handle, iosb, pos, 8, 14}) == X_STATUS_SUCCESS);
    data = read_file(c, f.handle, 16, -1);
    CHECK(data.size() == 2 && uint8_t(data[0]) == 254 && uint8_t(data[1]) == 255);
    uint32_t status;
    read_file(c, f.handle, 16, 512, &status);
    CHECK(status == X_STATUS_END_OF_FILE);
    CHECK(size_of(c, f.handle) == 512);
    // what the volume says of itself
    uint32_t info = alloc(64);
    CHECK(krnl(c, "NtQueryVolumeInformationFile", {f.handle, iosb, info, 64, 5}) == X_STATUS_SUCCESS);
    CHECK(K->load32(info + 8) == 4 && std::memcmp(K->host(info + 12), "GDFX", 4) == 0 && K->load32(iosb + 4) == 16);
    CHECK(krnl(c, "NtQueryVolumeInformationFile", {f.handle, iosb, info, 64, 3}) == X_STATUS_SUCCESS);
    CHECK(K->load32(info + 20) == 0x200 && K->load64(info + 8) == 0);
    CHECK(krnl(c, "NtQueryVolumeInformationFile", {f.handle, iosb, info, 64, 2}) == X_STATUS_INVALID_INFO_CLASS);
    close(c, f.handle);
    CHECK(krnl(c, "NtClose", {f.handle}) == X_STATUS_INVALID_HANDLE);

    Opened g = create(c, "D:\\MEDIA\\B.TXT", GENERIC_READ, OPEN, 0);
    CHECK(g.status == X_STATUS_SUCCESS && read_file(c, g.handle, 64, 0) == "hello");
    close(c, g.handle);
    g = create(c, "\\Device\\Cdrom0\\media\\..\\media\\.\\c.txt", GENERIC_READ, OPEN, 0);
    CHECK(g.status == X_STATUS_SUCCESS && read_file(c, g.handle, 64, 0) == "four");
    close(c, g.handle);

    // not there, a missing directory, a bad name, a directory as a file
    CHECK(create(c, "game:\\media\\none.bin", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(create(c, "game:\\none\\a.bin", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(create(c, "game:\\a+b", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_NAME_INVALID);
    CHECK(create(c, "game:\\media", GENERIC_READ, OPEN, NON_DIRECTORY).status == X_STATUS_FILE_IS_A_DIRECTORY);
    Opened none = create(c, "nodevice:\\x", GENERIC_READ, OPEN, 0);
    CHECK(none.status == X_STATUS_OBJECT_PATH_NOT_FOUND && none.handle == 0xFFFFFFFF);
    // the disc is read-only: a write gets reading only; nothing is created
    Opened w = create(c, "game:\\media\\b.txt", GENERIC_WRITE, OPEN, 0);
    CHECK(w.status == X_STATUS_SUCCESS && write_file(c, w.handle, "x", 0) == X_STATUS_ACCESS_DENIED);
    close(c, w.handle);
    CHECK(create(c, "game:\\media\\new.bin", GENERIC_WRITE, CREATE, 0).status == X_STATUS_ACCESS_DENIED);

    // NtQueryFullAttributesFile: by path, no handle
    uint32_t attrs = alloc(56);
    CHECK(krnl(c, "NtQueryFullAttributesFile", {attributes("game:\\media\\a.bin"), attrs}) == X_STATUS_SUCCESS);
    CHECK(K->load64(attrs + 0x28) == 512 && K->load32(attrs + 0x30) == 0x01 &&
          K->load64(attrs) == vfs::UNIX_EPOCH_FILETIME);
    CHECK(krnl(c, "NtQueryFullAttributesFile", {attributes("game:\\media"), attrs}) == X_STATUS_SUCCESS);
    CHECK((K->load32(attrs + 0x30) & 0x10) != 0);
    CHECK(krnl(c, "NtQueryFullAttributesFile", {attributes("game:\\gone"), attrs}) == X_STATUS_NO_SUCH_FILE);

    // the game's own symbolic link (ObCreateSymbolicLink) reaches the files
    CHECK(krnl(c, "ObCreateSymbolicLink", {ansi("\\??\\data:"), ansi("\\Device\\Cdrom0\\media")}) == 0);
    g = create(c, "data:\\b.txt", GENERIC_READ, OPEN, 0);
    CHECK(g.status == X_STATUS_SUCCESS && read_file(c, g.handle, 64, 0) == "hello");
    close(c, g.handle);
    CHECK(krnl(c, "ObDeleteSymbolicLink", {ansi("\\??\\data:")}) == 0);
    CHECK(create(c, "data:\\b.txt", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_PATH_NOT_FOUND);

    // a directory: one entry a call, in the disc's order; a pattern; the
    // relative open through it as root
    Opened d = create(c, "game:\\media", GENERIC_READ, OPEN, DIRECTORY_FILE);
    CHECK(d.status == X_STATUS_SUCCESS);
    uint32_t list = alloc(0x100);
    std::vector<std::string> names;
    for (int i = 0; i < 5; i++) {
        uint32_t s = krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x100, 0, i == 0});
        if (s != X_STATUS_SUCCESS) {
            CHECK(s == X_STATUS_NO_MORE_FILES && i == 3);
            break;
        }
        CHECK(K->load32(iosb + 4) == 0x100);
        names.emplace_back(reinterpret_cast<const char *>(K->host(list + 0x40)), K->load32(list + 0x3C));
        if (names.back() == "a.bin") CHECK(K->load64(list + 0x28) == 512 && K->load64(list + 0x30) == 512);
    }
    CHECK((names == std::vector<std::string>{"a.bin", "b.txt", "c.txt"}));
    CHECK(krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x100, ansi("*.txt"), 0}) == 0);
    CHECK(std::string(reinterpret_cast<const char *>(K->host(list + 0x40)), 5) == "b.txt");
    CHECK(krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x100, 0, 0}) == 0);
    CHECK(std::string(reinterpret_cast<const char *>(K->host(list + 0x40)), 5) == "c.txt");
    CHECK(krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x100, 0, 0}) == X_STATUS_NO_MORE_FILES);
    CHECK(krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x100, ansi("z*"), 0}) ==
          X_STATUS_NO_SUCH_FILE);
    CHECK(krnl(c, "NtQueryDirectoryFile", {d.handle, 0, 0, 0, iosb, list, 0x40, 0, 1}) ==
          X_STATUS_INFO_LENGTH_MISMATCH);
    g = create(c, "c.txt", GENERIC_READ, OPEN, 0, d.handle);
    CHECK(g.status == X_STATUS_SUCCESS && read_file(c, g.handle, 64, 0) == "four");
    close(c, g.handle);
    close(c, d.handle);

    // the raw partitions answer with nothing
    Opened raw = create(c, "\\Device\\Harddisk0\\Partition0", GENERIC_READ | GENERIC_WRITE, OPEN, 0);
    CHECK(raw.status == X_STATUS_SUCCESS);
    CHECK(read_file(c, raw.handle, 16, 0).empty());
    uint32_t geometry = alloc(8);
    CHECK(krnl(c, "NtDeviceIoControlFile", {raw.handle, 0, 0, 0, iosb, 0x70000, 0, 0, geometry, 8}) == 0);
    CHECK(K->load32(geometry) == 0xFF000 / 0x200 && K->load32(geometry + 4) == 0x200);
    close(c, raw.handle);
}

void cache_files(ppc::Context &c) {
    scratch_next = 0;
    // a directory missing on the way; made; a file made in it
    CHECK(create(c, "cache:\\dir\\x.dat", GENERIC_WRITE, CREATE, 0).status == X_STATUS_OBJECT_PATH_NOT_FOUND);
    Opened d = create(c, "cache:\\dir", GENERIC_READ, OPEN_IF, DIRECTORY_FILE);
    CHECK(d.status == X_STATUS_SUCCESS && d.action == 2 && std::filesystem::is_directory(storage / "cache" / "dir"));
    close(c, d.handle);
    Opened f = create(c, "cache:\\dir\\x.dat", GENERIC_READ | GENERIC_WRITE, CREATE, SYNCHRONOUS);
    CHECK(f.status == X_STATUS_SUCCESS && f.action == 2);
    CHECK(create(c, "cache:\\dir\\x.dat", GENERIC_READ, CREATE, 0).status == X_STATUS_OBJECT_NAME_COLLISION);
    CHECK(write_file(c, f.handle, "0123456789", -1) == X_STATUS_SUCCESS);
    CHECK(write_file(c, f.handle, "abc", 3) == X_STATUS_SUCCESS);
    CHECK(write_file(c, f.handle, "!", -1) == X_STATUS_SUCCESS); // after "abc"
    CHECK(size_of(c, f.handle) == 10);
    CHECK(read_file(c, f.handle, 64, 0) == "012abc!789");
    CHECK(host_read(storage / "cache" / "dir" / "x.dat") == "012abc!789");
    // the end of the file moved; its attributes by path see it
    uint32_t eof = alloc(8), iosb = alloc(8), attrs = alloc(56);
    K->store64(eof, 4);
    CHECK(krnl(c, "NtSetInformationFile", {f.handle, iosb, eof, 8, 20}) == X_STATUS_SUCCESS);
    CHECK(size_of(c, f.handle) == 4);
    CHECK(krnl(c, "NtQueryFullAttributesFile", {attributes("cache:\\dir\\x.dat"), attrs}) == 0);
    CHECK(K->load64(attrs + 0x28) == 4 && K->load32(attrs + 0x30) == 0x80);
    CHECK(krnl(c, "NtFlushBuffersFile", {f.handle, iosb}) == 0);
    // renamed in its directory, then deleted on close
    uint32_t rename = alloc(16);
    K->store32(rename, 0);
    std::string to = "cache:\\dir\\y.dat";
    uint32_t to_str = ansi(to);
    std::memcpy(K->host(rename + 8), K->host(to_str), 8);
    CHECK(krnl(c, "NtSetInformationFile", {f.handle, iosb, rename, 16, 10}) == X_STATUS_SUCCESS);
    CHECK(!std::filesystem::exists(storage / "cache" / "dir" / "x.dat"));
    CHECK(host_read(storage / "cache" / "dir" / "y.dat") == "012a");
    CHECK(create(c, "cache:\\dir\\x.dat", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
    uint32_t del = alloc(4);
    K->host(del)[0] = 1;
    CHECK(krnl(c, "NtSetInformationFile", {f.handle, iosb, del, 1, 13}) == X_STATUS_SUCCESS);
    CHECK(std::filesystem::exists(storage / "cache" / "dir" / "y.dat"));
    close(c, f.handle);
    CHECK(!std::filesystem::exists(storage / "cache" / "dir" / "y.dat"));
    CHECK(create(c, "cache:\\dir\\y.dat", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_NAME_NOT_FOUND);
    // overwrite: an existing file emptied
    Opened o = create(c, "cache0:\\z.bin", GENERIC_WRITE, OPEN_IF, 0);
    CHECK(o.status == X_STATUS_SUCCESS && o.action == 2 && write_file(c, o.handle, "zzzz", 0) == 0);
    close(c, o.handle);
    o = create(c, "cache0:\\z.bin", GENERIC_WRITE, 5 /* overwrite if */, 0);
    CHECK(o.status == X_STATUS_SUCCESS && o.action == 3 && size_of(c, o.handle) == 0);
    close(c, o.handle);
    CHECK(std::filesystem::exists(storage / "cache0" / "z.bin"));
    // a title linking cache0: to its own device keeps the system's (Xenia's)
    CHECK(krnl(c, "ObCreateSymbolicLink", {ansi("\\??\\cache0:"), ansi("\\Device\\cache0")}) == 0);
    o = create(c, "cache0:\\z.bin", GENERIC_READ, OPEN, 0);
    CHECK(o.status == X_STATUS_SUCCESS);
    close(c, o.handle);
}

void async_files(ppc::Context &c) {
    scratch_next = 0;
    // without FILE_SYNCHRONOUS_IO_* and with no buffering, a request that
    // succeeds answers STATUS_PENDING, done; the event, the file itself
    Opened f = create(c, "game:\\media\\a.bin", GENERIC_READ, OPEN, NO_BUFFERING);
    CHECK(f.status == X_STATUS_SUCCESS);
    uint32_t event_ptr = alloc(4), iosb = alloc(8), buffer = alloc(64), off = alloc(8), zero = alloc(8);
    CHECK(krnl(c, "NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
    uint32_t event = K->load32(event_ptr);
    K->store64(off, 100);
    K->store64(zero, 0);
    CHECK(krnl(c, "NtReadFile", {f.handle, event, 0, 0, iosb, buffer, 8, off}) == X_STATUS_PENDING);
    CHECK(K->load32(iosb) == X_STATUS_SUCCESS && K->load32(iosb + 4) == 8 && K->host(buffer)[0] == 100);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {event, 0, 0, zero}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {f.handle, 0, 0, zero}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {f.handle, 0, 0, zero}) == X_STATUS_TIMEOUT); // auto-reset

    // the APC routine: queued to this thread, run in an alertable wait
    apc_calls = 0;
    CHECK(krnl(c, "NtReadFile", {f.handle, 0, F_IO_APC, 0x1234, iosb, buffer, 8, off}) == X_STATUS_PENDING);
    CHECK(apc_calls == 0);
    uint32_t timeout = alloc(8);
    K->store64(timeout, uint64_t(-int64_t(2'000'000)));
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, timeout}) == X_STATUS_USER_APC);
    CHECK(apc_calls == 1 && apc_args[0] == 0x1234 && apc_args[1] == iosb && apc_args[2] == 0);

    // an I/O completion port: the file's key and the request's context
    uint32_t port_ptr = alloc(4);
    CHECK(krnl(c, "NtCreateIoCompletion", {port_ptr, 0, 0, 0}) == 0);
    uint32_t port = K->load32(port_ptr);
    uint32_t completion_info = alloc(8);
    K->store32(completion_info, port);
    K->store32(completion_info + 4, 77);
    CHECK(krnl(c, "NtSetInformationFile", {f.handle, iosb, completion_info, 8, 30}) == 0);
    CHECK(krnl(c, "NtReadFile", {f.handle, 0, 0, 0x55, iosb, buffer, 16, off}) == X_STATUS_PENDING);
    uint32_t key = alloc(4), context = alloc(4), packet = alloc(8);
    CHECK(krnl(c, "NtRemoveIoCompletion", {port, key, context, packet, timeout}) == X_STATUS_SUCCESS);
    CHECK(K->load32(key) == 77 && K->load32(context) == 0x55 && K->load32(packet) == 0 && K->load32(packet + 4) == 16);
    CHECK(krnl(c, "NtSetIoCompletion", {port, 1, 2, 3, 4}) == 0);
    CHECK(krnl(c, "NtRemoveIoCompletion", {port, key, context, packet, timeout}) == X_STATUS_SUCCESS);
    CHECK(K->load32(key) == 1 && K->load32(context) == 2 && K->load32(packet) == 3 && K->load32(packet + 4) == 4);
    K->store64(timeout, uint64_t(-int64_t(100'000)));
    CHECK(krnl(c, "NtRemoveIoCompletion", {port, key, context, packet, timeout}) == X_STATUS_TIMEOUT);
    CHECK(krnl(c, "NtCancelIoFile", {f.handle}) == 0);
    close(c, f.handle);
    close(c, port);
    close(c, event);

    // a scatter read: a page a segment, from an explicit offset 0
    Opened g = create(c, "game:\\media\\a.bin", GENERIC_READ, OPEN, SYNCHRONOUS);
    uint32_t pages = K->memory().system_alloc(0x2000, 0x1000), segments = alloc(8);
    K->store32(segments, pages);
    K->store32(segments + 4, pages + 0x1000);
    CHECK(krnl(c, "NtReadFileScatter", {g.handle, 0, 0, 0, iosb, segments, 0x1000, zero}) == 0);
    CHECK(K->load32(iosb + 4) == 512 && K->host(pages)[511] == 255);
    close(c, g.handle);
}

// XCONTENT_DATA: the hard drive, a type, a display name, a file name
uint32_t content_data(uint32_t type, const char16_t *display, const char *file) {
    uint32_t d = alloc(0x134);
    K->store32(d, 1);
    K->store32(d + 4, type);
    put_u16(*K, d + 8, display, 128);
    std::memcpy(K->host(d + 0x108), file, std::strlen(file));
    return d;
}

void content(ppc::Context &c) {
    scratch_next = 0;
    uint32_t data = content_data(1, u"Slot 1", "slot1"), disposition = alloc(4), license = alloc(4);
    CHECK(xam(c, "XamContentCreateEx", {0, 0, data, 1, disposition, license, 0, 0, 0}) == X_ERROR_INVALID_NAME);
    uint32_t root = alloc(8);
    std::memcpy(K->host(root), "save", 5);
    CHECK(xam(c, "XamContentCreateEx", {0, root, data, 1, disposition, license, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(disposition) == 1); // created
    Opened f = create(c, "save:\\data.bin", GENERIC_WRITE, CREATE, 0);
    CHECK(f.status == X_STATUS_SUCCESS && write_file(c, f.handle, "saved!", 0) == 0);
    close(c, f.handle);
    CHECK(xam(c, "XamContentCreateEx", {0, root, data, 3, disposition, license, 0, 0, 0}) ==
          X_ERROR_SUCCESS); // the same
    CHECK(xam(c, "XamContentClose", {root, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamContentClose", {root, 0}) == X_ERROR_FILE_NOT_FOUND);
    CHECK(create(c, "save:\\data.bin", GENERIC_READ, OPEN, 0).status == X_STATUS_OBJECT_PATH_NOT_FOUND);

    // on the host, in Xenia's layout: the package and its header
    auto title = storage / "content" / "B13EBABEBABEBABE" / "00000000";
    CHECK(host_read(title / "00000001" / "slot1" / "data.bin") == "saved!");
    std::string header = host_read(title / "Headers" / "00000001" / "slot1.header");
    CHECK(header.size() == 0xA000 && header.substr(0, 4) == "CON ");
    if (header.size() == 0xA000) {
        const uint8_t *h = reinterpret_cast<const uint8_t *>(header.data());
        CHECK(h[0x344 + 3] == 1 && h[0x344 + 0xCD + 1] == 'S' && h[0x344 + 0xCD + 11] == '1');
        CHECK(std::memcmp(h + 0x971A, "slot1", 6) == 0);
        CHECK(h[0x344 + 0x2D] == 0xB1 && h[0x344 + 0x2D + 7] == 0xBE); // the profile
    }

    // again: exists; opened; read back
    CHECK(xam(c, "XamContentCreateEx", {0, root, data, 1, disposition, license, 0, 0, 0}) == X_ERROR_ALREADY_EXISTS);
    CHECK(xam(c, "XamContentCreateEx", {0, root, data, 3, disposition, license, 0, 0, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(disposition) == 2); // opened
    f = create(c, "SAVE:\\data.bin", GENERIC_READ, OPEN, 0);
    CHECK(f.status == X_STATUS_SUCCESS && read_file(c, f.handle, 64, 0) == "saved!");
    close(c, f.handle);
    CHECK(xam(c, "XamContentClose", {root, 0}) == X_ERROR_SUCCESS);

    // enumerated: the user's saves of this title
    uint32_t size = alloc(4), handle = alloc(4);
    CHECK(xam(c, "XamContentCreateEnumerator", {0, 1, 1, 0, 4, size, handle}) == X_ERROR_SUCCESS);
    CHECK(K->load32(size) == 4 * 0x134);
    uint32_t items = alloc(4 * 0x134), returned = alloc(4);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x134, returned, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(returned) == 1);
    CHECK(K->load32(items) == 1 && K->load32(items + 4) == 1 && guest_u16(*K, items + 8) == u"Slot 1" &&
          std::string(reinterpret_cast<const char *>(K->host(items + 0x108))) == "slot1");
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x134, returned, 0}) == X_ERROR_NO_MORE_FILES);
    uint32_t private_ptr = alloc(4);
    CHECK(xam(c, "XamGetPrivateEnumStructureFromHandle", {K->load32(handle), private_ptr}) == 0);
    CHECK(K->load32(K->load32(private_ptr)) == 0xFE && K->load32(K->load32(private_ptr) + 0x10) == 4);
    krnl(c, "ObDereferenceObject", {K->load32(private_ptr)});
    close(c, K->load32(handle));

    // the creator: the user
    uint32_t is_creator = alloc(4), creator = alloc(8);
    CHECK(xam(c, "XamContentGetCreator", {0, data, is_creator, creator, 0}) == X_ERROR_SUCCESS);
    CHECK(K->load32(is_creator) == 1 && K->load64(creator) == DEFAULT_XUID);

    // overlapped: done at once, the event set, the result in it
    uint32_t event_ptr = alloc(4), ov = alloc(0x1C), zero = alloc(8);
    CHECK(krnl(c, "NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
    K->store32(ov + xoverlapped::EVENT, K->load32(event_ptr));
    CHECK(xam(c, "XamContentCreateEx", {0, root, data, 3, disposition, license, 0, 0, ov}) == X_ERROR_IO_PENDING);
    CHECK(K->load32(ov) == X_ERROR_SUCCESS && K->load32(ov + xoverlapped::LENGTH) == 2);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {K->load32(event_ptr), 0, 0, zero}) == X_STATUS_SUCCESS);
    uint32_t length = alloc(4);
    CHECK(xam(c, "XamGetOverlappedResult", {ov, length, 1}) == X_ERROR_SUCCESS && K->load32(length) == 2);
    // a completion routine instead of the event: an APC to this thread
    uint32_t ov2 = alloc(0x1C);
    K->store32(ov2 + xoverlapped::COMPLETION_ROUTINE, F_COMPLETION);
    apc_calls = 0;
    CHECK(xam(c, "XamContentClose", {root, ov2}) == X_ERROR_IO_PENDING);
    uint32_t timeout = alloc(8);
    K->store64(timeout, uint64_t(-int64_t(2'000'000)));
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, timeout}) == X_STATUS_USER_APC);
    CHECK(apc_calls == 1 && apc_args[0] == 0 && apc_args[1] == 0 && apc_args[2] == ov2);
    close(c, K->load32(event_ptr));

    // deleted: gone from the host and the list
    CHECK(xam(c, "XamContentDelete", {0, data, 0}) == X_ERROR_SUCCESS);
    CHECK(!std::filesystem::exists(title / "00000001" / "slot1"));
    CHECK(xam(c, "XamContentDelete", {0, data, 0}) == X_ERROR_FILE_NOT_FOUND);
    CHECK(xam(c, "XamContentCreateEnumerator", {0, 1, 1, 0, 4, size, handle}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamEnumerate", {K->load32(handle), 0, items, 4 * 0x134, returned, 0}) == X_ERROR_NO_MORE_FILES);
    close(c, K->load32(handle));
    // the devices
    uint32_t device = alloc(0x50);
    CHECK(xam(c, "XamContentGetDeviceData", {1, device}) == X_ERROR_SUCCESS);
    CHECK(K->load32(device) == 1 && K->load32(device + 4) == 1 && K->load64(device + 8) != 0);
    CHECK(xam(c, "XamContentGetDeviceData", {3, device}) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xam(c, "XamContentGetDeviceState", {1, 0}) == X_ERROR_SUCCESS);
    uint32_t mask = alloc(4);
    CHECK(xam(c, "XamContentGetLicenseMask", {mask, 0}) == X_ERROR_SUCCESS && K->load32(mask) == 0);
}

// The input exports with boot's idle pad, START pressed by its overlay when
// asked: states and their packet numbers, the capabilities, keystrokes
// (Xenia's SDL driver's, sticks included), vibration.
std::atomic<bool> start_pressed{false};
void input_exports(ppc::Context &c, uint32_t state) {
    using namespace xf::input;
    add_input_driver(*K, make_idle_pad([](uint32_t, uint64_t) {
        Gamepad g;
        if (start_pressed) g.buttons = START;
        return g;
    }));
    CHECK(xam(c, "XamInputGetState", {0, 0, state}) == X_ERROR_SUCCESS && K->load32(state) == 1 &&
          K->load16(state + 4) == 0);
    CHECK(xam(c, "XamInputGetState", {0, 0, state}) == X_ERROR_SUCCESS && K->load32(state) == 1);
    CHECK(xam(c, "XamInputGetState", {1, 0, state}) == X_ERROR_DEVICE_NOT_CONNECTED && K->load32(state) == 0);
    CHECK(xam(c, "XamInputGetState", {0, 2, state}) == X_ERROR_DEVICE_NOT_CONNECTED); // the keyboard asked for
    start_pressed = true;
    CHECK(xam(c, "XamInputGetState", {0, 1, state}) == X_ERROR_SUCCESS && K->load32(state) == 2 &&
          K->load16(state + 4) == START);
    uint32_t caps = alloc(32);
    CHECK(xam(c, "XamInputGetCapabilities", {0, 1, caps}) == X_ERROR_SUCCESS && K->host(caps)[0] == 1 &&
          K->host(caps)[1] == 1 && K->load16(caps + 4) == 0xF7FF && K->load16(caps + 18) == 0xFFFF);
    CHECK(xam(c, "XamInputGetCapabilitiesEx", {1, 0xFF, 0, caps}) == X_ERROR_SUCCESS && K->host(caps)[0] == 1);
    CHECK(xam(c, "XamInputGetCapabilities", {2, 1, caps}) == X_ERROR_DEVICE_NOT_CONNECTED);
    uint32_t user = alloc(4), key = alloc(8);
    K->store32(user, 0xFF);
    CHECK(xam(c, "XamInputGetKeystrokeEx", {user, 1, key}) == X_ERROR_SUCCESS && K->load16(key) == vk::START &&
          K->load16(key + 4) == KEYSTROKE_KEYDOWN && K->load32(user) == 0);
    CHECK(xam(c, "XamInputGetKeystrokeEx", {user, 1, key}) == X_ERROR_EMPTY);
    start_pressed = false;
    CHECK(xam(c, "XamInputGetKeystroke", {0, 1, key}) == X_ERROR_SUCCESS && K->load16(key) == vk::START &&
          K->load16(key + 4) == KEYSTROKE_KEYUP);
    CHECK(xam(c, "XamInputGetKeystroke", {1, 1, key}) == X_ERROR_DEVICE_NOT_CONNECTED);
    uint32_t vibration = alloc(4);
    CHECK(xam(c, "XamInputSetState", {0, 0, vibration}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XamInputSetState", {3, 0, vibration}) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(input_states_delivered(*K) == 3);

    // a stick: up-left, then left alone (up-left lets go before left goes down)
    PadKeystrokes keys;
    Keystroke s;
    Gamepad g;
    g.thumb_lx = -32768, g.thumb_ly = 32767;
    CHECK(keys.next(g, 0, 0, s) == SUCCESS && s.virtual_key == vk::LTHUMB_UPLEFT && s.flags == KEYSTROKE_KEYDOWN);
    g.thumb_ly = 0;
    CHECK(keys.next(g, 0, 10, s) == SUCCESS && s.virtual_key == vk::LTHUMB_UPLEFT && s.flags == KEYSTROKE_KEYUP);
    CHECK(keys.next(g, 0, 20, s) == SUCCESS && s.virtual_key == vk::LTHUMB_LEFT && s.flags == KEYSTROKE_KEYDOWN);
    CHECK(keys.next(g, 0, 30, s) == EMPTY);
    // held: repeated after 400 ms, then every 100 ms
    CHECK(keys.next(g, 0, 420, s) == EMPTY);
    CHECK(keys.next(g, 0, 421, s) == SUCCESS && s.virtual_key == vk::LTHUMB_LEFT &&
          s.flags == (KEYSTROKE_KEYDOWN | KEYSTROKE_REPEAT));
    CHECK(keys.next(g, 0, 500, s) == EMPTY);
    CHECK(keys.next(g, 0, 523, s) == SUCCESS && (s.flags & KEYSTROKE_REPEAT));
    g.left_trigger = 0x20;
    CHECK(keys.next(g, 0, 524, s) == SUCCESS && s.virtual_key == vk::LTRIGGER && s.flags == KEYSTROKE_KEYDOWN);
}

void notifications_and_users(ppc::Context &c) {
    scratch_next = 0;
    // the first system listener hears the UI and the sign-in at start
    uint32_t listener = xam(c, "XamNotifyCreateListener", {1 | 2, 1});
    uint32_t second = xam(c, "XamNotifyCreateListener", {1, 1});
    CHECK(listener && second && listener != second);
    uint32_t id = alloc(4), param = alloc(4), zero = alloc(8);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {listener, 0, 0, zero}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {second, 0, 0, zero}) == X_STATUS_TIMEOUT);
    CHECK(xam(c, "XNotifyGetNext", {listener, XN_SYS_SIGNINCHANGED, id, param}) == 1);
    CHECK(K->load32(id) == XN_SYS_SIGNINCHANGED && K->load32(param) == 1);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == XN_SYS_UI);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == XN_LIVE_CONNECTIONCHANGED);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == XN_LIVE_LINK_STATE_CHANGED);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 0 && K->load32(id) == 0);
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {listener, 0, 0, zero}) == X_STATUS_TIMEOUT);

    // the user: signed in offline in slot 0, Xenia's default profile
    CHECK(xam(c, "XamUserGetSigninState", {0}) == 1 && xam(c, "XamUserGetSigninState", {1}) == 0);
    uint32_t xuid = alloc(8), name = alloc(16), info = alloc(40);
    CHECK(xam(c, "XamUserGetXUID", {0, 1, xuid}) == 0 && K->load64(xuid) == DEFAULT_XUID);
    CHECK(xam(c, "XamUserGetXUID", {1, 1, xuid}) == X_E_NO_SUCH_USER);
    CHECK(xam(c, "XamUserGetName", {0, name, 16}) == 0 &&
          std::string(reinterpret_cast<char *>(K->host(name))) == "User");
    CHECK(xam(c, "XamUserGetSigninInfo", {0, 0, info}) == 0 && K->load64(info) == DEFAULT_XUID &&
          K->load32(info + 0xC) == 1 && std::string(reinterpret_cast<char *>(K->host(info + 0x18))) == "User");
    uint32_t privilege = alloc(4);
    CHECK(xam(c, "XamUserCheckPrivilege", {0, 0xFE, privilege}) == X_ERROR_NOT_LOGGED_ON && K->load32(privilege) == 0);

    // profile settings: the defaults, then what the title writes
    uint32_t ids = alloc(16), size = alloc(4);
    K->store32(ids + 0, 0x10040003);  // controller vibration
    K->store32(ids + 4, 0x4064000F);  // gamercard picture key
    K->store32(ids + 8, 0x63E83FFF);  // title specific 1
    K->store32(ids + 12, 0x10040002); // y axis inversion
    CHECK(xam(c, "XamUserReadProfileSettings", {0, 0, 0, 0, 4, ids, size, 0, 0}) == X_ERROR_INSUFFICIENT_BUFFER);
    uint32_t needed = K->load32(size);
    CHECK(needed == 8 + 4 * 40 + 0x64 + 0x3E8);
    uint32_t buffer = alloc(needed);
    CHECK(xam(c, "XamUserReadProfileSettings", {0, 0, 0, 0, 4, ids, size, buffer, 0}) == X_ERROR_SUCCESS);
    uint32_t settings = K->load32(buffer + 4);
    CHECK(K->load32(buffer) == 4 && settings == buffer + 8);
    CHECK(K->load32(settings + 0x10) == 0x10040003 && K->host(settings + 0x18)[0] == 1 &&
          K->load32(settings + 0x20) == 3 && K->load32(settings) == 1 && K->load32(settings + 8) == 0);
    uint32_t key = settings + 40;
    CHECK(K->load32(key + 0x20) == 50 && guest_u16(*K, K->load32(key + 0x24)) == u"FFFE07D10002000200010002");
    CHECK(K->load32(settings + 80 + 0x20) == 0); // nothing written yet
    uint32_t write = alloc(40), blob = alloc(4);
    std::memcpy(K->host(blob), "opts", 4);
    K->store32(write + 0x10, 0x63E83FFF);
    K->host(write + 0x18)[0] = 6;
    K->store32(write + 0x20, 4);
    K->store32(write + 0x24, blob);
    CHECK(xam(c, "XamUserWriteProfileSettings", {0, 0, 1, write, 0}) == X_ERROR_SUCCESS);
    CHECK(xam(c, "XNotifyGetNext", {listener, 0, id, param}) == 1 && K->load32(id) == XN_SYS_PROFILESETTINGCHANGED &&
          K->load32(param) == 1);
    CHECK(xam(c, "XNotifyGetNext", {second, XN_SYS_PROFILESETTINGCHANGED, id, param}) == 1);
    CHECK(xam(c, "XamUserReadProfileSettings", {0, 0, 0, 0, 4, ids, size, buffer, 0}) == X_ERROR_SUCCESS);
    uint32_t specific = settings + 80;
    CHECK(K->load32(specific) == 2 && K->load32(specific + 0x20) == 4 &&
          std::memcmp(K->host(K->load32(specific + 0x24)), "opts", 4) == 0);
    close(c, listener);
    close(c, second);

    // no controller yet
    uint32_t state = alloc(16);
    CHECK(xam(c, "XamInputGetState", {0, 0, state}) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xam(c, "XamInputGetCapabilities", {0, 1, state}) == X_ERROR_DEVICE_NOT_CONNECTED);
    input_exports(c, state);

    // the loader's launch data
    uint32_t launch = alloc(16);
    CHECK(xam(c, "XamLoaderGetLaunchDataSize", {size}) == X_ERROR_NOT_FOUND && K->load32(size) == 0);
    std::memcpy(K->host(launch), "go!", 4);
    CHECK(xam(c, "XamLoaderSetLaunchData", {launch, 4}) == 0);
    CHECK(xam(c, "XamLoaderGetLaunchDataSize", {size}) == 0 && K->load32(size) == 4);
    std::memset(K->host(launch), 0, 4);
    CHECK(xam(c, "XamLoaderGetLaunchData", {launch, 16}) == 0 &&
          std::string(reinterpret_cast<char *>(K->host(launch))) == "go!");

    // the guide's UI, headless: the focused button, the hard drive
    uint32_t box = alloc(4), ov = alloc(0x1C), device = alloc(4);
    CHECK(xam(c, "XamShowMessageBoxUI", {0, 0, 0, 0, 0, 1, 0, box, 0}) == X_ERROR_SUCCESS && K->load32(box) == 1);
    CHECK(xam(c, "XamShowDeviceSelectorUI", {0, 1, 0, 0, device, ov}) == X_ERROR_IO_PENDING && K->load32(device) == 1 &&
          K->load32(ov) == 0);
}

void main_thread(ppc::Context &c) {
    disc_files(c);
    cache_files(c);
    async_files(c);
    content(c);
    notifications_and_users(c);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_IO_APC, io_apc},
    {F_COMPLETION, completion},
};

} // namespace

int main() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path() / "xenoforge_files_test";
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
        CHECK(std::filesystem::is_directory(storage / "content") && std::filesystem::is_directory(storage / "cache"));
        scratch = memory->system_alloc(0x10000);

        Objects &o = objects(k);
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
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
    }
    std::filesystem::remove_all(dir, ec);
    return finish("kernel_files");
}
