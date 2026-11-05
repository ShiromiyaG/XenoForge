// Handles, dispatcher objects, waits and timers (objects.h). The handle
// table, the native objects found through the guest header and the Ob
// exports are translated from Xenia (src/xenia/kernel: util/object_table.cc,
// xobject.cc, xboxkrnl/xboxkrnl_ob.cc, xboxkrnl_threading.cc's LookupNamed
// Object; BSD, see LICENSE-xenia) and checked against them; the waits are
// the NT kernel's, written here over one lock instead of Xenia's host wait
// handles.

#include "objects.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace xf::kernel {
namespace {

constexpr uint32_t HANDLE_BASE = 0xF8000000; // Xenia's XObject::kHandleBase

std::string lowercase(std::string s) {
    for (char &ch : s) ch = char(std::tolower(uint8_t(ch)));
    return s;
}

void stash(Kernel &k, uint32_t address, uint32_t handle) {
    k.store32(address + header::WAIT_LIST, header::SIGNATURE);
    k.store32(address + header::WAIT_LIST + 4, handle);
}

} // namespace

// ---- guest helpers ----

void list_init(Kernel &k, uint32_t head) { k.store32(head, head), k.store32(head + 4, head); }
bool list_empty(Kernel &k, uint32_t head) { return k.load32(head) == head; }
void list_insert_tail(Kernel &k, uint32_t head, uint32_t entry) {
    uint32_t last = k.load32(head + 4);
    k.store32(entry, head), k.store32(entry + 4, last);
    k.store32(last, entry), k.store32(head + 4, entry);
}
void list_insert_head(Kernel &k, uint32_t head, uint32_t entry) {
    uint32_t first = k.load32(head);
    k.store32(entry, first), k.store32(entry + 4, head);
    k.store32(first + 4, entry), k.store32(head, entry);
}
void list_remove(Kernel &k, uint32_t entry) {
    uint32_t next = k.load32(entry), prev = k.load32(entry + 4);
    k.store32(prev, next), k.store32(next + 4, prev);
}

std::string ansi_string(Kernel &k, uint32_t address) {
    if (!address) return {};
    uint16_t length = k.load16(address);
    uint32_t buffer = k.load32(address + 4);
    if (!length || !buffer) return {};
    return std::string(reinterpret_cast<const char *>(k.host(buffer)), length);
}

std::string attributes_name(Kernel &k, uint32_t attributes) {
    if (!attributes) return {};
    return ansi_string(k, k.load32(attributes + object_attributes::NAME));
}

std::atomic_ref<uint32_t> guest_word(Kernel &k, uint32_t address) {
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(k.host(address)));
}

Objects &objects(Kernel &k) { return k.objects(); }

// ---- objects ----

Object::~Object() {
    if (allocated && guest) owner.k.memory().system_free(guest - object_header::SIZE);
}

int32_t Dispatcher::signal_state() const { return int32_t(owner.k.load32(guest + header::SIGNAL_STATE)); }
void Dispatcher::set_signal_state(int32_t state) { owner.k.store32(guest + header::SIGNAL_STATE, uint32_t(state)); }
GuestType Dispatcher::guest_type() const { return GuestType(owner.k.host(guest)[header::TYPE]); }

int32_t Semaphore::limit() const { return int32_t(owner.k.load32(guest + ksemaphore::LIMIT)); }

uint32_t Mutant::acquire(Thread &t) {
    set_signal_state(signal_state() - 1);
    if (owner_thread != &t) set_owner(&t);
    if (abandoned) {
        abandoned = false;
        owner.k.host(guest)[kmutant::ABANDONED] = 0;
        return X_STATUS_ABANDONED_WAIT_0;
    }
    return X_STATUS_SUCCESS;
}

void Mutant::set_owner(Thread *t) {
    if (owner_thread) std::erase(owner_thread->mutants, this);
    owner_thread = t;
    if (t) t->mutants.push_back(this);
    owner.k.store32(guest + kmutant::OWNER, t ? t->kthread() : 0);
}

// ---- the kernel's state ----

Objects::Objects(Kernel &kernel) : k(kernel) {
    scheduler = make_host_scheduler(*this);
    // the guest's system time: the host's now, as a FILETIME (100 ns since 1601)
    auto unix_time = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    system_time_base = uint64_t(unix_time) + 116'444'736'000'000'000ull - now();

    // the object types and the processes are the guest globals'
    // (modules.cpp, set up before this)
    type_thread = k.variable("ExThreadObjectType");
    type_event = k.variable("ExEventObjectType");
    type_mutant = k.variable("ExMutantObjectType");
    type_semaphore = k.variable("ExSemaphoreObjectType");
    type_timer = k.variable("ExTimerObjectType");
    type_file = k.variable("IoFileObjectType");
    title_process = ::xf::kernel::title_process(k);
    system_process = ::xf::kernel::system_process(k);

    time_stamp_bundle = k.variable("KeTimeStampBundle");
    std::lock_guard<std::mutex> l(lock);
    on_clock();
}

Objects::~Objects() {
    scheduler.reset(); // the clock first: it reaches in here
    std::lock_guard<std::mutex> l(lock);
    for (Timer *t : armed_) t->release();
    armed_.clear();
}

uint32_t Objects::object_type_for(const Object &o) const {
    switch (o.type()) {
    case Object::Type::Event: return type_event;
    case Object::Type::Semaphore: return type_semaphore;
    case Object::Type::Mutant: return type_mutant;
    case Object::Type::Timer: return type_timer;
    case Object::Type::Thread: return type_thread;
    case Object::Type::File: return type_file; // as Xenia's: the other files family types have none
    default: return 0;
    }
}

// ---- handles (Xenia's ObjectTable) ----

uint32_t Objects::add_handle(Object *object) {
    size_t n = slots_.size();
    uint32_t slot = UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        uint32_t s = uint32_t((free_cursor_ + i) % n);
        if (!slots_[s].object) {
            slot = s;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        slot = uint32_t(n);
        slots_.resize(std::max<size_t>(16 * 1024, n * 2));
    }
    free_cursor_ = slot + 1;
    slots_[slot] = {object, 1};
    object->retain();
    uint32_t handle = HANDLE_BASE + (slot << 2);
    object->handles.push_back(handle);
    return handle;
}

uint32_t Objects::translate(uint32_t handle) {
    if (handle == 0xFFFFFFFF) return 0; // the current process: not an object here
    if (handle == 0xFFFFFFFE) {         // the current thread
        Thread *t = current_thread();
        return t ? t->handle() : 0;
    }
    return handle;
}

Object *Objects::object_in(uint32_t handle) {
    handle = translate(handle);
    if ((handle & HANDLE_BASE) != HANDLE_BASE) return nullptr;
    uint32_t slot = (handle - HANDLE_BASE) >> 2;
    return slot < slots_.size() ? slots_[slot].object : nullptr;
}

bool Objects::retain_handle(uint32_t handle) {
    handle = translate(handle);
    if (!object_in(handle)) return false;
    slots_[(handle - HANDLE_BASE) >> 2].refs++;
    return true;
}

bool Objects::release_handle(uint32_t handle) {
    handle = translate(handle);
    Object *object = object_in(handle);
    if (!object) return false;
    Slot &slot = slots_[(handle - HANDLE_BASE) >> 2];
    if (--slot.refs > 0) return true;
    slot = {};
    std::erase(object->handles, handle);
    // the guest header named this handle: the next, or none (an empty wait
    // list again, so the next use makes a new object; Xenia's
    // UpdateNativeHandle)
    if (object->guest && object->dispatcher() && k.load32(object->guest + header::WAIT_LIST) == header::SIGNATURE &&
        k.load32(object->guest + header::WAIT_LIST + 4) == handle) {
        if (!object->handles.empty())
            stash(k, object->guest, object->handles.front());
        else
            list_init(k, object->guest + header::WAIT_LIST);
    }
    if (!object->name.empty()) {
        std::string key = lowercase(object->name);
        size_t named = std::erase_if(names_, [&](const auto &n) { return n.first == key && n.second == handle; });
        if (named && !object->handles.empty()) names_.push_back({key, object->handles.front()});
    }
    // a timer no handle names any more stops (NT deletes, so cancels, it)
    if (object->type() == Object::Type::Timer && object->handles.empty()) disarm(static_cast<Timer &>(*object));
    object->release();
    return true;
}

uint32_t Objects::find_name(const std::string &name) {
    std::string key = lowercase(name);
    for (const auto &[n, handle] : names_)
        if (n == key && retain_handle(handle)) return handle;
    return 0;
}

void Objects::set_name(Object &object, const std::string &name) {
    if (name.empty()) return;
    std::string key = lowercase(name);
    for (const auto &n : names_)
        if (n.first == key) return; // a collision: the object stays unnamed, as Xenia's
    object.name = name;
    names_.push_back({key, object.handle()});
}

// the object whose body is at a guest address: its stash, else a search
Object *Objects::by_guest(uint32_t address) {
    if (!address) return nullptr;
    if (k.load32(address + header::WAIT_LIST) == header::SIGNATURE) {
        Object *o = object_in(k.load32(address + header::WAIT_LIST + 4));
        if (o && o->guest == address) return o;
    }
    for (const Slot &s : slots_)
        if (s.object && s.object->guest == address) return s.object;
    return nullptr;
}

// ---- native objects (Xenia's CreateNative, GetNativeObject) ----

bool Objects::make_native(Dispatcher &d, GuestType type, uint32_t body_size) {
    uint32_t memory = k.memory().system_alloc(object_header::SIZE + body_size);
    if (!memory) return false;
    std::lock_guard<std::mutex> l(lock);
    d.guest = memory + object_header::SIZE, d.allocated = true;
    uint32_t handle = add_handle(&d);
    k.store32(memory + object_header::POINTER_COUNT, 1);
    k.store32(memory + object_header::HANDLE_COUNT, 1);
    k.store32(memory + object_header::OBJECT_TYPE, object_type_for(d));
    k.store16(memory + object_header::FLAGS, object_header::IS_TITLE_OBJECT);
    k.host(d.guest)[header::TYPE] = uint8_t(type);
    stash(k, d.guest, handle);
    return true;
}

Ref<Dispatcher> Objects::native(uint32_t address) {
    std::lock_guard<std::mutex> l(lock);
    return native_locked(address);
}

Ref<Dispatcher> Objects::native_locked(uint32_t address) {
    if (!address) return {};
    if (k.load32(address + header::WAIT_LIST) == header::SIGNATURE) {
        Object *o = object_in(k.load32(address + header::WAIT_LIST + 4));
        if (o && o->guest == address && o->dispatcher()) return Ref<Dispatcher>::retain(static_cast<Dispatcher *>(o));
    }
    // first use: what the header says it is (Xenia makes events, mutants and
    // semaphores; timers here too)
    Dispatcher *d = nullptr;
    switch (GuestType(k.host(address)[header::TYPE])) {
    case GuestType::EventNotification:
    case GuestType::EventSynchronization: d = new Event(*this); break;
    case GuestType::Semaphore: d = new Semaphore(*this); break;
    case GuestType::Mutant: d = new Mutant(*this); break;
    case GuestType::TimerNotification:
    case GuestType::TimerSynchronization: d = new Timer(*this); break;
    default:
        std::fprintf(stderr, "kernel: no object of type %u at 0x%08X\n", k.host(address)[header::TYPE], address);
        return {};
    }
    Ref<Dispatcher> ref(d);
    d->guest = address;
    stash(k, address, add_handle(d));
    return ref;
}

// ---- waits ----

bool Objects::try_satisfy(Thread &t, std::span<Dispatcher *const> objects, bool all, uint32_t &status) {
    if (all) {
        for (Dispatcher *d : objects)
            if (!d->signaled_for(t)) return false;
        status = X_STATUS_SUCCESS;
        for (Dispatcher *d : objects)
            if (d->acquire(t) == X_STATUS_ABANDONED_WAIT_0) status = X_STATUS_ABANDONED_WAIT_0;
        return true;
    }
    for (uint32_t i = 0; i < objects.size(); i++) {
        if (objects[i]->signaled_for(t)) {
            status = objects[i]->acquire(t) + i;
            return true;
        }
    }
    return false;
}

void Objects::remove_waits(Thread &t) {
    for (Dispatcher *d : t.waiting_on) std::erase_if(d->waiters, [&](const Waiter &w) { return w.thread == &t; });
    t.waiting_on.clear();
}

void Objects::unwait(Thread &t, uint32_t status) {
    remove_waits(t);
    t.wait_status = status;
    scheduler->wake(t);
}

void Objects::signaled(Dispatcher &d) {
    // the oldest waits first, each satisfied while the object stays signaled
    // for it (NT's KiWaitTest)
    for (size_t i = 0; i < d.waiters.size();) {
        Waiter w = d.waiters[i];
        Thread &t = *w.thread;
        if (!d.signaled_for(t)) {
            i++;
            continue;
        }
        uint32_t status;
        if (t.wait_all) {
            if (!try_satisfy(t, t.waiting_on, true, status)) {
                i++;
                continue;
            }
        } else {
            status = d.acquire(t) + w.index;
        }
        unwait(t, status); // removes w from d.waiters
        if (d.signal_state() <= 0 && d.type() != Object::Type::Mutant) break;
    }
}

uint32_t Objects::wait(Thread &self, std::span<Dispatcher *const> objects, bool all, bool alertable,
                       uint64_t deadline) {
    std::unique_lock<std::mutex> l(lock);
    for (;;) {
        if ((self.pending.load() & Thread::PENDING_KERNEL_APC) &&
            !k.load32(self.kthread() + kthread::APC_DISABLE_COUNT)) {
            l.unlock();
            deliver_apcs(self, 0);
            l.lock();
            continue;
        }
        if (alertable && user_apc_pending(self)) return X_STATUS_USER_APC;
        uint32_t status;
        if (try_satisfy(self, objects, all, status)) return status;
        if (deadline != Scheduler::NEVER && now() >= deadline) return X_STATUS_TIMEOUT;

        self.waiting_on.assign(objects.begin(), objects.end());
        self.wait_all = all, self.alertable = alertable;
        self.wait_status = X_STATUS_PENDING;
        for (uint32_t i = 0; i < objects.size(); i++) objects[i]->waiters.push_back({&self, i});
        self.blocked = Thread::Blocked::Wait;
        k.host(self.kthread())[kthread::STATE] = kthread::WAITING;
        scheduler->block(self, l, deadline);
        self.blocked = Thread::Blocked::No;
        k.host(self.kthread())[kthread::STATE] = kthread::RUNNING;
        status = self.wait_status;
        if (status == X_STATUS_PENDING) { // the deadline (or a spurious wake): look again
            remove_waits(self);
            continue;
        }
        if (status == WAKE_FOR_KERNEL_APC) continue;
        return status;
    }
}

uint64_t Objects::deadline(uint32_t timeout_ptr) {
    if (!timeout_ptr) return Scheduler::NEVER;
    int64_t timeout = int64_t(k.load64(timeout_ptr));
    uint64_t t = now();
    if (timeout < 0) {
        uint64_t relative = timeout == INT64_MIN ? uint64_t(INT64_MAX) : uint64_t(-timeout);
        return relative > Scheduler::NEVER - 1 - t ? Scheduler::NEVER - 1 : t + relative;
    }
    if (timeout == 0) return t;
    // an absolute system time
    return uint64_t(timeout) <= system_time_base ? 0 : uint64_t(timeout) - system_time_base;
}

// ---- timers ----

void Objects::arm(Timer &t) {
    if (!t.armed) {
        t.armed = true;
        t.retain();
        armed_.push_back(&t);
    }
    scheduler->timers_changed();
}

void Objects::disarm(Timer &t) {
    if (!t.armed) return;
    t.armed = false;
    std::erase(armed_, &t);
    t.release();
}

uint64_t Objects::next_due() {
    uint64_t due = Scheduler::NEVER;
    for (const Timer *t : armed_) due = std::min(due, t->due);
    return due;
}

void Objects::on_clock() {
    uint64_t t = now();
    // KeTimeStampBundle (Xenia's UpdateKeTimestampBundle): games read the
    // tick count there instead of calling the kernel
    k.store64(time_stamp_bundle + time_stamp_bundle::INTERRUPT_TIME, t);
    k.store64(time_stamp_bundle + time_stamp_bundle::SYSTEM_TIME, system_time_base + t);
    k.store32(time_stamp_bundle + time_stamp_bundle::TICK_COUNT, uint32_t(t / 10'000));

    std::vector<Timer *> due;
    for (Timer *timer : armed_)
        if (timer->due <= t) due.push_back(timer);
    for (Timer *timer : due) {
        Ref<Timer> keep = Ref<Timer>::retain(timer);
        timer->set_signal_state(1);
        signaled(*timer);
        if (timer->routine && timer->apc_thread && !timer->apc_thread->terminated) {
            // the routine as a user APC on the thread that set the timer
            // (routine_arg, the time's low and high words; Xenia's XTimer)
            uint64_t time = system_time_base + t;
            queue_user_apc(*timer->apc_thread, timer->routine, timer->routine_arg, uint32_t(time),
                           uint32_t(time >> 32));
        }
        if (timer->period) {
            timer->due += timer->period;
            if (timer->due <= t) timer->due = t + timer->period;
        } else {
            disarm(*timer);
        }
    }
}

// ---- Ob exports (Xenia's xboxkrnl_ob.cc) ----

XF_EXPORT(xboxkrnl, ObReferenceObjectByHandle) {
    uint32_t handle = arg(k, c, 0), type_ptr = arg(k, c, 1), out_ptr = arg(k, c, 2);
    Objects &o = objects(k);
    if (out_ptr) k.store32(out_ptr, 0);
    std::lock_guard<std::mutex> l(o.lock);
    Object *object = o.object_in(handle);
    if (!object) return result(c, X_STATUS_INVALID_HANDLE);
    uint32_t native = object->guest;
    if (type_ptr) {
        uint32_t actual = o.object_type_for(*object);
        if (!actual)
            native = 0xDEADF00D; // as Xenia: a type it cannot name
        else if (actual != type_ptr)
            return result(c, X_STATUS_OBJECT_TYPE_MISMATCH);
    }
    // the caller holds the reference until ObDereferenceObject
    o.retain_handle(handle);
    if (out_ptr) k.store32(out_ptr, native);
    result(c, X_STATUS_SUCCESS);
}

// (object, out handle): a handle to an object by its body (Xenia's: the
// object's own handle, one more reference)
XF_EXPORT(xboxkrnl, ObOpenObjectByPointer) {
    uint32_t address = arg(k, c, 0), out_ptr = arg(k, c, 1);
    Objects &o = objects(k);
    if (out_ptr) k.store32(out_ptr, 0);
    Ref<Dispatcher> made;
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (Object *object = o.by_guest(address)) {
            o.retain_handle(object->handle());
            if (out_ptr) k.store32(out_ptr, object->handle());
            return result(c, X_STATUS_SUCCESS);
        }
    }
    // a header the game initialized itself: its host object made now
    made = o.native(address);
    if (!made) return result(c, X_STATUS_UNSUCCESSFUL);
    std::lock_guard<std::mutex> l(o.lock);
    o.retain_handle(made->handle());
    if (out_ptr) k.store32(out_ptr, made->handle());
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, ObReferenceObject) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (Object *object = o.by_guest(arg(k, c, 0))) o.retain_handle(object->handle());
}

XF_EXPORT(xboxkrnl, ObDereferenceObject) {
    uint32_t native = arg(k, c, 0);
    if (!native || native == 0xDEADF00D) return;
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (Object *object = o.by_guest(native))
        o.release_handle(object->handle());
    else
        std::fprintf(stderr, "kernel: ObDereferenceObject of an unknown object 0x%08X\n", native);
}

// Whether an object belongs to the title (its header's flag): the objects
// the kernel made carry it; any other is in the title's memory.
XF_EXPORT(xboxkrnl, ObIsTitleObject) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    Object *object = o.by_guest(arg(k, c, 0));
    if (object && object->allocated)
        return result(c, (k.load16(object->guest - object_header::SIZE + object_header::FLAGS) &
                          object_header::IS_TITLE_OBJECT) != 0);
    result(c, 1);
}

XF_EXPORT(xboxkrnl, NtClose) {
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    result(c, o.release_handle(arg(k, c, 0)) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE);
}

// Another handle to the same object; with DUPLICATE_CLOSE_SOURCE (1) the
// source's own (Xenia keeps the handle's value). -2 is the current thread.
XF_EXPORT(xboxkrnl, NtDuplicateObject) {
    uint32_t handle = arg(k, c, 0), new_ptr = arg(k, c, 1), options = arg(k, c, 2);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    uint32_t source = o.translate(handle), duplicate = 0xFFFFFFFF;
    Object *object = o.object_in(source);
    if (object) duplicate = (options & 1) ? source : o.add_handle(object);
    if (new_ptr) k.store32(new_ptr, duplicate);
    result(c, object ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE);
}

// ---- symbolic links (ObCreateSymbolicLink), kept for the file system ----

namespace {

// Xenia's canonicalize_guest_path, trimmed and without the object
// manager's \??\ and \System??\ prefixes
std::string link_path(std::string path) {
    size_t a = path.find_first_not_of(" \t\r\n"), b = path.find_last_not_of(" \t\r\n");
    path = a == std::string::npos ? std::string() : path.substr(a, b - a + 1);
    for (const char *prefix : {"\\??\\", "\\System??\\"})
        if (path.starts_with(prefix)) path = path.substr(std::string_view(prefix).size());
    bool rooted = path.starts_with('\\');
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("\\/", start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty() && !parts.back().ends_with(':')) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out = rooted ? "\\" : "";
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "\\" : "") + parts[i];
    return out;
}

} // namespace

XF_EXPORT(xboxkrnl, ObCreateSymbolicLink) {
    std::string path = link_path(ansi_string(k, arg(k, c, 0)));
    std::string target = link_path(ansi_string(k, arg(k, c, 1)));
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    // a link that already starts the path stays, and the call succeeds (as
    // Xenia's FindSymbolicLink check: the system's cache0: stays the host's
    // cache partition when the title links it to its own device)
    for (const auto &link : o.symbolic_links)
        if (lowercase(path).starts_with(lowercase(link.first))) return result(c, X_STATUS_SUCCESS);
    o.symbolic_links.push_back({path, target});
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, ObDeleteSymbolicLink) {
    std::string path = lowercase(link_path(ansi_string(k, arg(k, c, 0))));
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    size_t n = std::erase_if(o.symbolic_links, [&](const auto &link) { return lowercase(link.first) == path; });
    result(c, n ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL);
}

} // namespace xf::kernel
