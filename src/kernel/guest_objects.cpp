// Objects the guest makes with ObCreateObject and puts in the object manager
// with ObInsertObject: enumerators, sessions and voices of the console's XAM,
// device objects, objects of types a title defines. Xenia has none of this
// (its ObInsertObject is not implemented, and its objects are host objects
// behind handles); what is written here is NT's object manager as the
// console's kernel keeps it: the body in guest memory after an
// X_OBJECT_HEADER (and, named, an X_OBJECT_HEADER_NAME_INFO), the pointer and
// handle counts in the header, the type's close and delete procedures called
// when a handle closes and the last reference goes, then its free procedure.
// The names are the table of objects.cpp (one namespace, case-insensitive);
// there are no directory objects, so a name's directories are not looked for
// (a name under a directory that does not exist is inserted all the same).

#include "files.h"
#include "objects.h"

#include <cstdio>
#include <cstring>

namespace xf::kernel {

GuestObject *Objects::guest_object(uint32_t body) {
    auto it = guest_objects_.find(body);
    return it == guest_objects_.end() ? nullptr : it->second.get();
}

GuestObject &Objects::add_guest_object(uint32_t body, uint32_t allocation, bool host_allocated, std::string path,
                                       bool permanent) {
    Ref<GuestObject> g(new GuestObject(*this));
    g->guest = body, g->allocation = allocation, g->host_allocated = host_allocated;
    g->path = std::move(path), g->permanent = permanent;
    GuestObject &ref = *g;
    guest_objects_[body] = std::move(g);
    return ref;
}

// The header and, for a named object, the name info, written over an
// allocation (ObCreateObject's, with the type's procedure, or the host's): the
// body's address.
uint32_t Objects::init_guest_object(uint32_t memory, uint32_t type, uint32_t body_size, const std::string &element,
                                    uint16_t flags) {
    if (element.empty()) {
        k.store32(memory + object_header::POINTER_COUNT, 1);
        k.store32(memory + object_header::HANDLE_COUNT, 0);
        k.store32(memory + object_header::OBJECT_TYPE, type);
        k.store16(memory + object_header::FLAGS, flags);
        return memory + object_header::SIZE;
    }
    // the name info, the header, the body (aligned to 4), the name
    uint32_t aligned = (body_size + 3) & ~3u, length = uint32_t(element.size());
    uint32_t header = memory + object_name_info::SIZE, body = header + object_header::SIZE, text = body + aligned;
    k.store32(memory + object_name_info::NEXT, 0);
    k.store32(memory + object_name_info::DIRECTORY, 0);
    k.store16(memory + object_name_info::NAME, uint16_t(length));
    k.store16(memory + object_name_info::NAME + 2, uint16_t(length));
    k.store32(memory + object_name_info::NAME + 4, text);
    std::memcpy(k.host(text), element.data(), length);
    k.store32(header + object_header::POINTER_COUNT, 1);
    k.store32(header + object_header::HANDLE_COUNT, 0);
    k.store32(header + object_header::OBJECT_TYPE, type);
    k.store16(header + object_header::FLAGS, uint16_t((flags & 0xFFFE) | object_flags::NAMED));
    return body;
}

uint32_t Objects::make_guest_object(uint32_t type, uint32_t body_size, const std::string &path, bool title_object) {
    std::string element;
    if (!path.empty()) {
        size_t last = path.find_last_of('\\');
        element = last == std::string::npos ? path : path.substr(last + 1);
    }
    uint32_t aligned = (body_size + 3) & ~3u;
    uint32_t size =
        object_header::SIZE + aligned + (element.empty() ? 0 : object_name_info::SIZE + uint32_t(element.size()));
    uint32_t memory = k.memory().system_alloc(size);
    if (!memory) return 0;
    std::memset(k.host(memory), 0, size);
    uint32_t body =
        init_guest_object(memory, type, body_size, element, title_object ? object_header::IS_TITLE_OBJECT : 0);
    std::lock_guard<std::mutex> l(lock);
    add_guest_object(body, memory, true, path, false);
    return body;
}

void Objects::reference_guest(GuestObject &g) {
    k.store32(g.header() + object_header::POINTER_COUNT, k.load32(g.header() + object_header::POINTER_COUNT) + 1);
}

bool Objects::dereference_guest(GuestObject &g) {
    uint32_t count = k.load32(g.header() + object_header::POINTER_COUNT);
    if (!count) {
        std::fprintf(stderr, "kernel: object 0x%08X dereferenced with no reference left\n", g.guest);
        return false;
    }
    k.store32(g.header() + object_header::POINTER_COUNT, count - 1);
    return count == 1;
}

uint32_t Objects::open_guest_handle(GuestObject &g) {
    uint32_t handle = add_handle(&g);
    k.store32(g.header() + object_header::HANDLE_COUNT, k.load32(g.header() + object_header::HANDLE_COUNT) + 1);
    reference_guest(g);
    return handle;
}

bool Objects::drop_guest_handle(GuestObject &g, uint32_t handle) {
    release_handle(handle);
    uint32_t count = k.load32(g.header() + object_header::HANDLE_COUNT);
    if (count) k.store32(g.header() + object_header::HANDLE_COUNT, count - 1);
    return dereference_guest(g);
}

Objects::Insertion Objects::insert_guest_object(GuestObject &g, const std::string &given_path, uint32_t attributes,
                                                uint32_t bias) {
    Insertion r;
    // a failure: the object is dereferenced, as NT's ObInsertObject does
    auto fail = [&](uint32_t status) {
        if (dereference_guest(g)) r.discard = Ref<GuestObject>::retain(&g);
        r.status = status;
        return r;
    };
    if (g.inserted) {
        std::fprintf(stderr, "kernel: ObInsertObject of 0x%08X again\n", g.guest);
        r.status = X_STATUS_INVALID_PARAMETER;
        return r;
    }
    bool named = (k.load16(g.header() + object_header::FLAGS) & object_flags::NAMED) != 0;
    std::string path = !given_path.empty() ? given_path : g.path;
    uint32_t type = k.load32(g.header() + object_header::OBJECT_TYPE);
    if (named) {
        if (path.empty() || path[0] != '\\') return fail(X_STATUS_OBJECT_NAME_INVALID);
        if (uint32_t existing = find_name(path)) {
            Object *e = object_in(existing);
            auto *eg = dynamic_cast<GuestObject *>(e);
            uint32_t etype = eg ? k.load32(eg->header() + object_header::OBJECT_TYPE) : object_type_for(*e);
            if (!(attributes & obj::OPEN_IF)) {
                release_handle(existing);
                return fail(X_STATUS_OBJECT_NAME_COLLISION);
            }
            if (etype != type) {
                release_handle(existing);
                return fail(X_STATUS_OBJECT_TYPE_MISMATCH);
            }
            // the name exists: a handle to that object (the pointer bias on it), the new one thrown away
            if (eg) {
                r.handle = open_guest_handle(*eg);
                for (uint32_t i = 0; i < bias; i++) reference_guest(*eg);
            } else {
                r.handle = add_handle(e);
            }
            release_handle(existing);
            fail(X_STATUS_OBJECT_NAME_EXISTS);
            return r;
        }
    }
    r.handle = add_handle(&g);
    if (named) set_name(g, path);
    g.permanent = g.permanent || (attributes & obj::PERMANENT) != 0;
    if (g.permanent) g.permanent_handle = add_handle(&g); // the permanent reference: it keeps the name
    // the handle's pointer and the bias, less the creation's; the permanent one
    k.store32(g.header() + object_header::HANDLE_COUNT, 1);
    k.store32(g.header() + object_header::POINTER_COUNT,
              k.load32(g.header() + object_header::POINTER_COUNT) + bias + (g.permanent ? 1 : 0));
    g.inserted = true;
    r.status = X_STATUS_SUCCESS;
    return r;
}

bool Objects::make_temporary(GuestObject &g) {
    if (!g.permanent) return false;
    g.permanent = false;
    if (uint32_t handle = std::exchange(g.permanent_handle, 0)) release_handle(handle);
    return dereference_guest(g);
}

void Objects::delete_guest_object(ppc::Context &c, Ref<GuestObject> g) {
    uint32_t type = k.load32(g->header() + object_header::OBJECT_TYPE);
    uint32_t delete_proc = type ? k.load32(type + object_type::DELETE) : 0;
    uint32_t free_proc = type ? k.load32(type + object_type::FREE) : 0;
    if (delete_proc) call(c, delete_proc, {g->guest});
    {
        std::lock_guard<std::mutex> l(lock);
        guest_objects_.erase(g->guest);
    }
    if (g->host_allocated)
        k.memory().system_free(g->allocation);
    else if (free_proc)
        call(c, free_proc, {g->allocation});
}

uint32_t Objects::close_handle(ppc::Context &c, uint32_t handle) {
    Ref<GuestObject> g;
    uint32_t handles_left = 0;
    {
        std::lock_guard<std::mutex> l(lock);
        Object *object = object_in(handle);
        if (!object) return X_STATUS_INVALID_HANDLE;
        g = Ref<GuestObject>::retain(dynamic_cast<GuestObject *>(object));
        if (!g) return release_handle(handle) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
        release_handle(handle);
        uint32_t count = k.load32(g->header() + object_header::HANDLE_COUNT);
        handles_left = count ? count - 1 : 0;
        k.store32(g->header() + object_header::HANDLE_COUNT, handles_left);
    }
    // the close procedure (the object, the handles left), then the handle's reference
    uint32_t type = k.load32(g->header() + object_header::OBJECT_TYPE);
    if (uint32_t close_proc = type ? k.load32(type + object_type::CLOSE) : 0)
        call(c, close_proc, {g->guest, handles_left});
    bool dead;
    {
        std::lock_guard<std::mutex> l(lock);
        dead = dereference_guest(*g);
    }
    if (dead) delete_guest_object(c, g);
    return X_STATUS_SUCCESS;
}

// (object, attributes, pointer bias, out handle): the object a ObCreateObject
// made put in the namespace (named by the last element ObCreateObject kept
// and the path it was made with, or the path of these attributes) and a handle
// opened to it, written to the out pointer. The header's counts: one handle,
// the pointer bias and the handle's pointer (the creation's is given up).
// OBJ_PERMANENT (0x10) in these attributes or the creation's keeps the name
// with no handle open until ObMakeTemporaryObject. A name that exists is
// STATUS_OBJECT_NAME_COLLISION, or with OBJ_OPENIF (0x80) a handle to that one
// and STATUS_OBJECT_NAME_EXISTS (the new object is deleted either way).
// The out pointer may be null: the handle is made all the same (NT's does).
XF_EXPORT(xboxkrnl, ObInsertObject) {
    uint32_t body = arg(k, c, 0), attributes = arg(k, c, 1), bias = arg(k, c, 2), handle_ptr = arg(k, c, 3);
    Objects &o = objects(k);
    if (handle_ptr) k.store32(handle_ptr, 0);
    Objects::Insertion r;
    {
        std::lock_guard<std::mutex> l(o.lock);
        GuestObject *g = o.guest_object(body);
        if (!g) {
            std::fprintf(stderr, "kernel: ObInsertObject of 0x%08X, which no ObCreateObject made\n", body);
            return result(c, X_STATUS_INVALID_PARAMETER);
        }
        uint32_t bits = attributes ? k.load32(attributes + object_attributes::ATTRIBUTES) : 0;
        std::string path = attributes_name(k, attributes);
        r = o.insert_guest_object(*g, path, bits, bias);
    }
    if (handle_ptr) k.store32(handle_ptr, r.handle);
    if (r.discard) o.delete_guest_object(c, std::move(r.discard));
    result(c, r.status);
}

// (object): a permanent object made temporary: its name goes with its last
// handle (at once if none is open) and the permanent reference is given up
XF_EXPORT(xboxkrnl, ObMakeTemporaryObject) {
    Objects &o = objects(k);
    Ref<GuestObject> dead;
    {
        std::lock_guard<std::mutex> l(o.lock);
        GuestObject *g = o.guest_object(arg(k, c, 0));
        if (g && o.make_temporary(*g)) dead = Ref<GuestObject>::retain(g);
    }
    if (dead) o.delete_guest_object(c, std::move(dead));
}

} // namespace xf::kernel
