// The object manager's guest-made objects and the device objects (src/kernel/
// guest_objects.cpp, devices.cpp): ObCreateObject and ObInsertObject for an
// unnamed and a named object of a type the guest defines (its close and
// delete procedures host functions in the generated code's table), the
// header's pointer and handle counts through references, handles and
// closes, name collisions and OBJ_OPENIF, OBJ_PERMANENT and
// ObMakeTemporaryObject; the devices there are at start, IoCreateDevice and
// IoDeleteDevice, IoDismountVolume and IoDismountVolumeByName.

#include "kernel_harness.h"
#include "objects.h"

#include <vector>

using namespace xf;
using namespace xf::kernel;

namespace {

constexpr uint32_t F_CLOSE = 0x83000000, F_DELETE = 0x83000010;

// what the type's procedures were called with: (kind, object, extra)
struct ProcCall {
    uint32_t kind, object, extra;
    bool operator==(const ProcCall &) const = default;
};
std::vector<ProcCall> proc_calls;
Kernel *K = nullptr;

void close_proc(ppc::Context &c) { proc_calls.push_back({1, uint32_t(c.r[3]), uint32_t(c.r[4])}); }
void delete_proc(ppc::Context &c) { proc_calls.push_back({2, uint32_t(c.r[3]), 0}); }

const Generated generated[] = {{F_CLOSE, close_proc}, {F_DELETE, delete_proc}};

// X_ANSI_STRING and X_OBJECT_ATTRIBUTES for a name
uint32_t ansi(KernelHarness &h, const char *text) {
    uint32_t s = h.alloc(8);
    uint32_t t = h.string(text);
    K->store16(s, uint16_t(std::strlen(text)));
    K->store16(s + 2, uint16_t(std::strlen(text)));
    K->store32(s + 4, t);
    return s;
}
uint32_t attributes(KernelHarness &h, const char *name, uint32_t bits) {
    uint32_t a = h.alloc(12);
    K->store32(a, 0);
    K->store32(a + 4, name ? ansi(h, name) : 0);
    K->store32(a + 8, bits);
    return a;
}

uint32_t pointer_count(uint32_t body) { return K->load32(body - object_header::SIZE + object_header::POINTER_COUNT); }
uint32_t handle_count(uint32_t body) { return K->load32(body - object_header::SIZE + object_header::HANDLE_COUNT); }

void unnamed(KernelHarness &h, uint32_t type) {
    uint32_t out = h.alloc(4), handle = h.alloc(4), body;
    // ObCreateObject: a body after a header of count 1, no handle, the type's
    CHECK(h.krnl("ObCreateObject", {type, 0, 0x20, out}) == 0);
    body = K->load32(out);
    CHECK(body && pointer_count(body) == 1 && handle_count(body) == 0);
    CHECK(K->load32(body - object_header::SIZE + object_header::OBJECT_TYPE) == type);
    // ObInsertObject: a handle, one handle counted and the creation's reference now the handle's
    CHECK(h.krnl("ObInsertObject", {body, 0, 0, handle}) == 0);
    uint32_t hn = K->load32(handle);
    CHECK(hn >= 0xF8000000 && pointer_count(body) == 1 && handle_count(body) == 1);
    // it cannot be inserted twice, and what was never created is no object
    CHECK(h.krnl("ObInsertObject", {body, 0, 0, handle}) == X_STATUS_INVALID_PARAMETER);
    CHECK(h.krnl("ObInsertObject", {h.alloc(0x40), 0, 0, handle}) == X_STATUS_INVALID_PARAMETER);
    K->store32(handle, 0xAAAAAAAA);
    CHECK(h.krnl("ObInsertObject", {h.alloc(0x40), 0, 0, handle}) == X_STATUS_INVALID_PARAMETER &&
          K->load32(handle) == 0);

    // a reference by handle: the body, one more pointer; of another type, a mismatch
    uint32_t ref = h.alloc(4);
    CHECK(h.krnl("ObReferenceObjectByHandle", {hn, type, ref}) == 0 && K->load32(ref) == body);
    CHECK(pointer_count(body) == 2);
    CHECK(h.krnl("ObReferenceObjectByHandle", {hn, K->variable("ExEventObjectType"), ref}) ==
              X_STATUS_OBJECT_TYPE_MISMATCH &&
          pointer_count(body) == 2);
    CHECK(h.krnl("ObReferenceObjectByHandle", {hn, 0, ref}) == 0 && pointer_count(body) == 3);
    h.krnl("ObDereferenceObject", {body});
    h.krnl("ObDereferenceObject", {body});
    CHECK(pointer_count(body) == 1 && proc_calls.empty());
    // a second handle by pointer: counted, both close, the object goes at the last
    CHECK(h.krnl("ObOpenObjectByPointer", {body, handle}) == 0);
    uint32_t second = K->load32(handle);
    CHECK(second != hn && handle_count(body) == 2 && pointer_count(body) == 2);
    CHECK(h.krnl("NtClose", {second}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 1}}));
    CHECK(handle_count(body) == 1 && pointer_count(body) == 1);
    proc_calls.clear();
    CHECK(h.krnl("NtClose", {hn}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 0}, {2, body, 0}})); // closed, then deleted
    CHECK(h.krnl("ObReferenceObjectByHandle", {hn, type, ref}) == X_STATUS_INVALID_HANDLE);
    CHECK(h.krnl("NtClose", {hn}) == X_STATUS_INVALID_HANDLE);
    proc_calls.clear();

    // the pointer bias: the caller's reference outlives the handle
    CHECK(h.krnl("ObCreateObject", {type, 0, 0x10, out}) == 0);
    body = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {body, 0, 1, handle}) == 0 && pointer_count(body) == 2);
    CHECK(h.krnl("NtClose", {K->load32(handle)}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 0}}) && pointer_count(body) == 1);
    h.krnl("ObDereferenceObject", {body});
    CHECK(proc_calls.size() == 2 && proc_calls[1] == (ProcCall{2, body, 0}));
    proc_calls.clear();

    // an object made and never inserted goes with its creation reference
    CHECK(h.krnl("ObCreateObject", {type, 0, 0x10, out}) == 0);
    body = K->load32(out);
    h.krnl("ObDereferenceObject", {body});
    CHECK((proc_calls == std::vector<ProcCall>{{2, body, 0}}));
    proc_calls.clear();
}

void named(KernelHarness &h, uint32_t type) {
    uint32_t out = h.alloc(4), handle = h.alloc(4), ref = h.alloc(4);
    const char *path = "\\Test\\Thing";
    // a named object's header keeps the last element, flagged named
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, path, 0), 0x20, out}) == 0);
    uint32_t body = K->load32(out);
    uint32_t header = body - object_header::SIZE;
    CHECK(K->load16(header + object_header::FLAGS) & object_flags::NAMED);
    uint32_t info = header - object_name_info::SIZE;
    CHECK(K->load16(info + object_name_info::NAME) == 5 &&
          h.read(K->load32(info + object_name_info::NAME + 4)).substr(0, 5) == "Thing");
    // nothing is found before it is inserted
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, path), 0, type, 0, ref}) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(h.krnl("ObInsertObject", {body, 0, 0, handle}) == 0);
    uint32_t first = K->load32(handle);
    // by name, by its full path, case-insensitive: the same body, a reference each
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\test\\THING"), 0, type, 0, ref}) == 0 &&
          K->load32(ref) == body);
    CHECK(pointer_count(body) == 2);
    h.krnl("ObDereferenceObject", {body});
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, path), 0, K->variable("ExEventObjectType"), 0, ref}) ==
          X_STATUS_OBJECT_TYPE_MISMATCH);
    // ObOpenObjectByName: a handle of its own
    CHECK(h.krnl("ObOpenObjectByName", {attributes(h, path, 0), type, 0, handle}) == 0);
    uint32_t opened = K->load32(handle);
    CHECK(opened != first && handle_count(body) == 2 && pointer_count(body) == 2);
    CHECK(h.krnl("ObOpenObjectByName", {attributes(h, path, 0), K->variable("ExEventObjectType"), 0, handle}) ==
          X_STATUS_OBJECT_TYPE_MISMATCH);

    // a second object by the name: a collision, and it is deleted; with OBJ_OPENIF, the first one's handle
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, path, 0), 0x20, out}) == 0);
    uint32_t other = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {other, attributes(h, path, 0), 0, handle}) == 0xC0000035);
    CHECK(K->load32(handle) == 0 && (proc_calls == std::vector<ProcCall>{{2, other, 0}}));
    proc_calls.clear();
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, path, obj::OPEN_IF), 0x20, out}) == 0);
    other = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {other, attributes(h, path, obj::OPEN_IF), 0, handle}) ==
          X_STATUS_OBJECT_NAME_EXISTS);
    uint32_t third = K->load32(handle);
    CHECK(third && third != first && third != opened && handle_count(body) == 3);
    CHECK((proc_calls == std::vector<ProcCall>{{2, other, 0}}));
    proc_calls.clear();
    // OPENIF with another type is a mismatch
    CHECK(h.krnl("ObCreateObject",
                 {K->variable("ExSemaphoreObjectType"), attributes(h, path, obj::OPEN_IF), 0x20, out}) == 0);
    other = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {other, attributes(h, path, obj::OPEN_IF), 0, handle}) ==
          X_STATUS_OBJECT_TYPE_MISMATCH);
    // a name that is not rooted
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, "Rootless", 0), 0x20, out}) == 0);
    other = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {other, 0, 0, handle}) == 0xC0000033 && proc_calls.size() == 1);
    proc_calls.clear();
    // an empty element, or the one the backslashes leave empty
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, "\\Test\\", 0), 0x20, out}) == 0xC0000033);
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, "\\Test\\\\Thing", 0), 0x20, out}) == 0xC0000033);

    // the name goes with the last handle
    CHECK(h.krnl("NtClose", {first}) == 0 && h.krnl("NtClose", {opened}) == 0);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, path), 0, type, 0, ref}) == 0);
    h.krnl("ObDereferenceObject", {body});
    proc_calls.clear();
    CHECK(h.krnl("NtClose", {third}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 0}, {2, body, 0}}));
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, path), 0, type, 0, ref}) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    proc_calls.clear();

    // a permanent object keeps its name with no handle open, until it is made temporary
    CHECK(h.krnl("ObCreateObject", {type, attributes(h, "\\Test\\Forever", obj::PERMANENT), 0x20, out}) == 0);
    body = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {body, 0, 0, handle}) == 0 && handle_count(body) == 1);
    CHECK(pointer_count(body) == 2); // the handle's and the permanent one
    CHECK(h.krnl("NtClose", {K->load32(handle)}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 0}}) && pointer_count(body) == 1);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Test\\Forever"), 0, type, 0, ref}) == 0 &&
          K->load32(ref) == body);
    CHECK(pointer_count(body) == 2);
    proc_calls.clear();
    h.krnl("ObMakeTemporaryObject", {body}); // our own reference holds it
    CHECK(proc_calls.empty() && pointer_count(body) == 1);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Test\\Forever"), 0, type, 0, ref}) ==
          X_STATUS_OBJECT_NAME_NOT_FOUND);
    h.krnl("ObDereferenceObject", {body});
    CHECK((proc_calls == std::vector<ProcCall>{{2, body, 0}}));
    proc_calls.clear();

    // NtDuplicateObject counts a handle and a pointer
    CHECK(h.krnl("ObCreateObject", {type, 0, 0x10, out}) == 0);
    body = K->load32(out);
    CHECK(h.krnl("ObInsertObject", {body, 0, 0, handle}) == 0);
    uint32_t original = K->load32(handle);
    CHECK(h.krnl("NtDuplicateObject", {original, handle, 0}) == 0 && handle_count(body) == 2 &&
          pointer_count(body) == 2);
    CHECK(h.krnl("NtClose", {original}) == 0 && h.krnl("NtClose", {K->load32(handle)}) == 0);
    CHECK((proc_calls == std::vector<ProcCall>{{1, body, 1}, {1, body, 0}, {2, body, 0}}));
    proc_calls.clear();
}

void devices(KernelHarness &h) {
    uint32_t out = h.alloc(4);
    uint32_t device_type = K->variable("IoDeviceObjectType");
    // the devices there are at start, found by name as objects of IoDeviceObjectType
    for (const char *name : {"\\Device\\HdDvdRom", "\\Device\\Cdrom0", "\\Device\\Harddisk0\\Partition1",
                             "\\device\\harddisk0\\partition0"}) {
        K->store32(out, 0);
        CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, name), 0, device_type, 0, out}) == 0);
        uint32_t d = K->load32(out);
        CHECK(d && K->load16(d) == 3 && K->load16(d + 2) == 0x50);
        CHECK(pointer_count(d) == 2); // the permanent reference, ours
        h.krnl("ObDereferenceObject", {d});
        CHECK(pointer_count(d) == 1 && handle_count(d) == 0);
    }
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\HdDvdRom"), 0, K->variable("ExEventObjectType"), 0,
                                             out}) == X_STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\NoSuch"), 0, device_type, 0, out}) ==
          X_STATUS_OBJECT_NAME_NOT_FOUND);

    // IoDismountVolumeByName: the device by name; no such name, not rooted, not a device
    CHECK(h.krnl("IoDismountVolumeByName", {ansi(h, "\\Device\\HdDvdRom")}) == 0);
    CHECK(h.krnl("IoDismountVolumeByName", {ansi(h, "\\Device\\HdDvdRom")}) == 0); // and again: nothing is kept
    CHECK(h.krnl("IoDismountVolumeByName", {ansi(h, "\\Device\\NoSuch")}) == X_STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(h.krnl("IoDismountVolumeByName", {ansi(h, "Device")}) == 0xC0000033);
    CHECK(h.krnl("IoDismountVolumeByName", {0}) == 0xC0000033);
    // IoDismountVolume: a device object, else STATUS_VOLUME_DISMOUNTED
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\Cdrom0"), 0, device_type, 0, out}) == 0);
    uint32_t cdrom = K->load32(out);
    CHECK(h.krnl("IoDismountVolume", {cdrom}) == 0);
    h.krnl("ObDereferenceObject", {cdrom});
    uint32_t not_a_device = h.alloc(0x60);
    CHECK(h.krnl("IoDismountVolume", {not_a_device}) == 0xC000026E);
    CHECK(h.krnl("IoDismountVolume", {0}) == 0xC000026E);

    // IoCreateDevice: an object named, permanent, with its extension; a name that exists is a collision
    uint32_t dev = h.alloc(4);
    CHECK(h.krnl("IoCreateDevice", {0x1234, 0x10, ansi(h, "\\Device\\Test0"), 0x22, 0, dev}) == 0);
    uint32_t d = K->load32(dev);
    CHECK(d && K->load16(d) == 3 && K->load16(d + 2) == 0x60 && K->load32(d + 8) == 0x1234);
    CHECK(K->host(d)[0x1C] == 0x22 && K->load32(d + 0x0C) == d && K->load32(d + 0x18) == d + 0x50);
    CHECK(K->load32(d + 0x14) == 24); // named: 16 | 8
    CHECK(pointer_count(d) == 1 && handle_count(d) == 0);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\Test0"), 0, device_type, 0, out}) == 0 &&
          K->load32(out) == d);
    h.krnl("ObDereferenceObject", {d});
    K->store32(dev, 0xAAAAAAAA);
    CHECK(h.krnl("IoCreateDevice", {0, 0, ansi(h, "\\Device\\Test0"), 0x22, 0, dev}) == 0xC0000035 &&
          K->load32(dev) == 0);
    // an unnamed one is no name's, flags 16
    CHECK(h.krnl("IoCreateDevice", {0, 0, 0, 7, 0, dev}) == 0);
    uint32_t unnamed_device = K->load32(dev);
    CHECK(K->load32(unnamed_device + 0x14) == 16 && K->load32(unnamed_device + 0x0C) == 0); // disks are mounted apart
    // IoDeleteDevice: the name goes, and what refers to it keeps the object
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\Test0"), 0, device_type, 0, out}) == 0);
    h.krnl("IoDeleteDevice", {d});
    CHECK(pointer_count(d) == 1);
    CHECK(h.krnl("ObReferenceObjectByName", {ansi(h, "\\Device\\Test0"), 0, device_type, 0, out}) ==
          X_STATUS_OBJECT_NAME_NOT_FOUND);
    h.krnl("ObDereferenceObject", {d});                                                     // gone now
    CHECK(h.krnl("IoCreateDevice", {0, 0, ansi(h, "\\Device\\Test0"), 0x22, 0, dev}) == 0); // the name is free again
    h.krnl("IoDeleteDevice", {K->load32(dev)});
    h.krnl("IoDeleteDevice", {unnamed_device});
    h.krnl("IoDeleteDevice", {0});
}

} // namespace

int main() {
    KernelHarness h;
    if (!h.init()) return std::printf("kernel_ob: no kernel\n"), 1;
    K = h.k.get();
    K->set_generated(generated, std::size(generated));
    // a type the guest defines: the pool's allocate and free, its own close and delete
    uint32_t type = h.alloc(object_type::SIZE);
    uint32_t pool = K->variable("ExEventObjectType");
    K->store32(type + object_type::ALLOCATE, K->load32(pool + object_type::ALLOCATE));
    K->store32(type + object_type::FREE, K->load32(pool + object_type::FREE));
    K->store32(type + object_type::CLOSE, F_CLOSE);
    K->store32(type + object_type::DELETE, F_DELETE);
    K->store32(type + object_type::POOL_TAG, 0x74736554); // 'Test'
    unnamed(h, type);
    named(h, type);
    devices(h);
    return finish("kernel_ob");
}
