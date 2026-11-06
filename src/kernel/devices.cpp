// Device objects: the X_DEVICE_OBJECTs of the I/O manager, in the object
// namespace as the console's drivers leave them (IoCreateDevice makes a named,
// permanent object of IoDeviceObjectType), and what is asked of them by name:
// ObReferenceObjectByName finds one, IoDismountVolumeByName and
// IoDismountVolume dismount it. The device object's layout and IoCreateDevice
// follow Xenia's (xboxkrnl_io.cc), which makes a device object in guest memory
// and no name for it; here the object is made as NT makes it (guest_objects.cpp:
// header, counts, name, permanent reference) so the namespace has it. The
// file systems stay the VFS's (files.cpp): a device object is what a driver
// names, and what XAM and a title look for, not where files are read.
//
// The devices that exist at start are a list of ours (the table in create_devices):
// the console's drivers make more (the memory units, USB storage, the flash),
// which nothing here asks for yet.

#include "files.h"
#include "objects.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace xf::kernel {

namespace {

// what the console's IoDismountVolume answers to what is not a device object
constexpr uint32_t X_STATUS_VOLUME_DISMOUNTED = 0xC000026E;

constexpr uint32_t DEVICE_OBJECT_SIZE = 0x50;
constexpr uint16_t IO_TYPE_DEVICE = 3;

// FILE_DEVICE_* of the ones made at start
constexpr uint8_t FILE_DEVICE_CD_ROM = 2, FILE_DEVICE_DISK = 7;

// Xenia's: the device types whose device has no mounted device of its own
// (the others point at themselves)
bool mounts_itself(uint32_t type) {
    static constexpr uint32_t unmounted[] = {7, 58, 62, 45, 2, 60, 61, 36, 64, 65, 66, 67, 68, 69, 70, 72, 73};
    return std::find(std::begin(unmounted), std::end(unmounted), type) == std::end(unmounted);
}

// The device object's fields (Xenia's IoCreateDevice): type, size, driver,
// mounted or self device, flags, extension, device type, stack size.
void fill_device(Kernel &k, uint32_t d, uint32_t driver, uint32_t extension_size, bool named, uint32_t type) {
    k.store16(d + 0x00, IO_TYPE_DEVICE);
    k.store16(d + 0x02, uint16_t(extension_size + DEVICE_OBJECT_SIZE)); // the size, unaligned
    k.store32(d + 0x0C, mounts_itself(type) ? d : 0);
    k.host(d)[0x1C] = uint8_t(type);
    k.host(d)[0x1E] = 1; // stack size
    k.store32(d + 0x14, 16 | (named ? 8 : 0));
    if (extension_size) k.store32(d + 0x18, d + DEVICE_OBJECT_SIZE);
    k.store32(d + 0x08, driver);
}

// A device object made and put in the namespace (permanent: it stays until
// IoDeleteDevice), as IoCreateDevice does: the object of the status' success,
// or 0 with the status. The device's name (empty: none) is its whole path.
uint32_t make_device(Kernel &k, ppc::Context *c, uint32_t driver, uint32_t extension_size, const std::string &name,
                     uint32_t type, uint32_t &status) {
    Objects &o = objects(k);
    uint32_t size = DEVICE_OBJECT_SIZE + ((extension_size + 7) & ~7u);
    uint32_t d = o.make_guest_object(k.variable("IoDeviceObjectType"), size, name, false);
    if (!d) return status = X_STATUS_NO_MEMORY, 0;
    fill_device(k, d, driver, extension_size, !name.empty(), type);
    Objects::Insertion r;
    {
        std::lock_guard<std::mutex> l(o.lock);
        GuestObject *g = o.guest_object(d);
        r = o.insert_guest_object(*g, name, obj::PERMANENT | obj::CASE_INSENSITIVE, 0);
        // no handle is kept (NT's IoCreateDevice closes it): the permanent reference holds the object
        if (r.status == X_STATUS_SUCCESS) o.drop_guest_handle(*g, r.handle);
    }
    status = r.status;
    if (r.discard && c) o.delete_guest_object(*c, std::move(r.discard));
    return r.status == X_STATUS_SUCCESS ? d : 0;
}

// The device object a name is (ObReferenceObjectByName with IoDeviceObjectType): the status, and the object with a
// reference taken on success.
uint32_t device_by_name(Kernel &k, const std::string &name, uint32_t &device) {
    device = 0;
    if (name.empty() || name[0] != '\\') return X_STATUS_OBJECT_NAME_INVALID;
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t handle = o.find_name(name);
    if (!handle) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    uint32_t status = X_STATUS_OBJECT_TYPE_MISMATCH;
    auto *g = dynamic_cast<GuestObject *>(o.object_in(handle));
    if (g && k.load32(g->header() + object_header::OBJECT_TYPE) == k.variable("IoDeviceObjectType")) {
        o.reference_guest(*g);
        device = g->guest;
        status = X_STATUS_SUCCESS;
    }
    o.release_handle(handle);
    return status;
}

// IoDismountVolume's answer for an object: the console's refuses what is not
// a device object with STATUS_VOLUME_DISMOUNTED. A device is dismounted by
// answering success: the file systems here are the VFS's and stay (what the
// console's dismount takes away is a file system's hold on the device, which
// the VFS keeps apart; the XAM launch dismounts the drives before it loads
// the next title).
uint32_t dismount(Kernel &k, uint32_t device) {
    if (!device) return X_STATUS_VOLUME_DISMOUNTED;
    Objects &o = objects(k);
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (GuestObject *g = o.guest_object(device))
            return k.load32(g->header() + object_header::OBJECT_TYPE) == k.variable("IoDeviceObjectType")
                       ? X_STATUS_SUCCESS
                       : X_STATUS_VOLUME_DISMOUNTED;
    }
    return k.load16(device) == IO_TYPE_DEVICE ? X_STATUS_SUCCESS : X_STATUS_VOLUME_DISMOUNTED;
}

} // namespace

// The devices there are at start.
void create_devices(Kernel &k) {
    struct Device {
        const char *path;
        uint8_t type;
    };
    static constexpr Device devices[] = {
        {"\\Device\\Cdrom0", FILE_DEVICE_CD_ROM},
        {"\\Device\\HdDvdRom", FILE_DEVICE_CD_ROM},
        {"\\Device\\Harddisk0\\Partition0", FILE_DEVICE_DISK},
        {"\\Device\\Harddisk0\\Partition1", FILE_DEVICE_DISK},
        {"\\Device\\Harddisk0\\Cache0", FILE_DEVICE_DISK},
        {"\\Device\\Harddisk0\\Cache1", FILE_DEVICE_DISK},
    };
    for (const Device &d : devices) {
        uint32_t status;
        make_device(k, nullptr, 0, 0, d.path, d.type, status);
    }
}

// (driver object, extension size, device name: PANSI_STRING, device type,
// extra attributes, out device): an X_DEVICE_OBJECT (0x50) and its extension,
// named by the device name (permanent, in the namespace); a name that exists is
// STATUS_OBJECT_NAME_COLLISION, as NT's. The device is the creator's to
// initialize (XMountUtilityDrive's task).
XF_EXPORT(xboxkrnl, IoCreateDevice) {
    uint32_t driver = arg(k, c, 0), extension_size = arg(k, c, 1), name = arg(k, c, 2), type = arg(k, c, 3);
    uint32_t out_ptr = arg(k, c, 5);
    uint32_t status;
    uint32_t d = make_device(k, &c, driver, extension_size, name ? ansi_string(k, name) : std::string(), type, status);
    if (out_ptr) k.store32(out_ptr, d);
    result(c, status);
}

// (device): NT's: the device made temporary, which gives up its permanent
// reference and deletes it unless something still refers to it
XF_EXPORT(xboxkrnl, IoDeleteDevice) {
    Objects &o = objects(k);
    Ref<GuestObject> dead;
    {
        std::lock_guard<std::mutex> l(o.lock);
        GuestObject *g = o.guest_object(arg(k, c, 0));
        if (g && o.make_temporary(*g)) dead = Ref<GuestObject>::retain(g);
    }
    if (dead) o.delete_guest_object(c, std::move(dead));
}

// (device object)
XF_EXPORT(xboxkrnl, IoDismountVolume) { result(c, dismount(k, arg(k, c, 0))); }

// (device name: PSTRING): the device by name, as ObReferenceObjectByName finds
// it with IoDeviceObjectType (a name that is not rooted, not there or not a
// device answers as that does), dismounted, and given back
XF_EXPORT(xboxkrnl, IoDismountVolumeByName) {
    uint32_t name = arg(k, c, 0), device;
    uint32_t status = device_by_name(k, name ? ansi_string(k, name) : std::string(), device);
    if (status != X_STATUS_SUCCESS) return result(c, status);
    status = dismount(k, device);
    Objects &o = objects(k);
    Ref<GuestObject> dead;
    {
        std::lock_guard<std::mutex> l(o.lock);
        GuestObject *g = o.guest_object(device);
        if (g && o.dereference_guest(*g)) dead = Ref<GuestObject>::retain(g);
    }
    if (dead) o.delete_guest_object(c, std::move(dead));
    result(c, status);
}

} // namespace xf::kernel
