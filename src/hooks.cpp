// The port's hooks (xenoforge/hooks.h, xenoforge/port.h): the registry, the
// slots bound to it, the runs, the hook frames the reports read, the counts,
// and the owners a module loaded at run time registers as.

#include "xenoforge/hooks.h"

#include "xenoforge/ppc_interp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace xf::hooks {
namespace {

// a bit per 4 KB page: 2^20 pages, 128 KB, zero until a hook is registered
uint8_t page_bits[1u << 17];

std::atomic<bool> counting{false};
// the owner the calling thread registers for (set_registering)
thread_local Owner *registering = nullptr;
// Threads between reading an entry and pinning its owners (Pin): an owner's
// code is unloaded only after this was seen at 0 once its hooks were dropped.
std::atomic<int64_t> resolving{0};

struct Registry {
    std::shared_mutex mutex;
    std::unordered_map<uint32_t, const Entry *> entries;
    std::vector<std::unique_ptr<Entry>> versions; // every Entry published: never dropped
    std::vector<const ppc::GeneratedModule *> modules;
    std::unordered_map<uint32_t, uint32_t> function_slots; // address, size (bound modules')
    std::unordered_map<uint32_t, std::string> names;       // the modules', then the port's
    std::set<uint32_t> warned;
    // the counters, by address, kind and hook name: kept across registrations
    std::map<std::tuple<uint32_t, int, std::string>, std::unique_ptr<Counter>> counters;
    std::vector<std::unique_ptr<Owner>> owners; // never dropped (entries point at them)

    uint32_t next_owner = 1;
    // persistent(): the blocks by key, and their sizes
    std::map<std::string, std::pair<std::unique_ptr<uint64_t[]>, size_t>> blocks;
};

Registry &registry() {
    static Registry r;
    return r;
}

void store_slot(ppc::HookSlot &slot, const Entry *e) {
    const void *hooks = nullptr;
    if (e && (slot.original ? e->function() : !e->mid.empty())) hooks = e;
    __atomic_store_n(&slot.hooks, hooks, __ATOMIC_SEQ_CST);
}

const ppc::GeneratedModule *module_of(Registry &r, uint32_t address) {
    for (const ppc::GeneratedModule *m : r.modules)
        if (address - m->base < m->size) return m;
    return nullptr;
}

// A hook a module's generated code has no slot for: said once, as only some
// ways in reach it (registry held)
void warn_unslotted(Registry &r, const Entry &e) {
    const ppc::GeneratedModule *m = module_of(r, e.address);
    if (!m || r.warned.count(e.address)) return;
    bool function_slot = false, mid_slot = false;
    for (size_t k = 0; k < m->hook_count; k++)
        if (m->hooks[k]->address == e.address) (m->hooks[k]->original ? function_slot : mid_slot) = true;
    const char *label = e.name.empty() ? "" : e.name.c_str();
    if (e.function() && !function_slot) {
        r.warned.insert(e.address);
        std::fprintf(stderr,
                     "port: 0x%08X %s is not among the project file's hooks for %s: direct calls in the generated "
                     "code go past its hooks (dispatch, indirect calls and the interpreter reach them)\n",
                     e.address, label, m->name);
    } else if (!e.mid.empty() && !mid_slot) {
        r.warned.insert(e.address);
        std::fprintf(stderr,
                     "port: 0x%08X %s is not among the project file's mid hooks for %s: only the interpreter runs "
                     "its mid-function hook\n",
                     e.address, label, m->name);
    }
}

Counter *counter(Registry &r, uint32_t address, Kind kind, const std::string &name) {
    auto &c = r.counters[{address, int(kind), name}];
    if (!c) c = std::make_unique<Counter>();
    return c.get();
}

// the owners with code in an entry (the executable's is never unloaded)
void collect_owners(Entry &e) {
    e.owners.clear();
    auto add = [&](Owner *o) {
        if (o && std::find(e.owners.begin(), e.owners.end(), o) == e.owners.end()) e.owners.push_back(o);
    };
    add(e.replace.owner);
    for (const auto &h : e.before) add(h.owner);
    for (const auto &h : e.after) add(h.owner);
    for (const auto &h : e.mid) add(h.owner);
}

// A new version of the hooks at an address, published and bound (registry held)
template <class F> void update_locked(Registry &r, uint32_t address, const char *name, F &&change) {
    auto it = r.entries.find(address);
    auto e = it == r.entries.end() ? std::make_unique<Entry>() : std::make_unique<Entry>(*it->second);
    e->address = address;
    if (name && *name && e->name.empty()) e->name = name;
    change(*e);
    collect_owners(*e);
    const Entry *published = e.get();
    r.versions.push_back(std::move(e));
    r.entries[address] = published;
    if (!published->name.empty()) r.names.emplace(address, published->name);
    __atomic_fetch_or(&page_bits[address >> 15], uint8_t(1u << ((address >> 12) & 7)), __ATOMIC_RELEASE);
    for (const ppc::GeneratedModule *m : r.modules)
        for (size_t k = 0; k < m->hook_count; k++)
            if (m->hooks[k]->address == address) store_slot(*m->hooks[k], published);
    warn_unslotted(r, *published);
}
template <class F> void update(uint32_t address, const char *name, F &&change) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    update_locked(r, address, name, change);
}

// a hook as registered: its name, owner and counter (registry held)
template <class H, class F> H make_hook(Registry &r, uint32_t address, Kind kind, F fn, const char *name) {
    H h;
    h.fn = fn;
    h.name = name ? name : "";
    h.owner = registering;
    h.counter = counter(r, address, kind, h.name);
    return h;
}

// the port's code around guest code, for the lockstep check (System's hook_boundary)
void boundary(ppc::Context &c, uint32_t address, bool after, uint32_t resume) {
    if (c.system && c.system->hook_boundary) c.system->hook_boundary(c, address, after, resume);
}

// ---- the hook frames ----

// One hook running: on the host stack of the run, linked from its context's
// head (Frames). Its fields change as the run goes from hook to hook.
struct Frame {
    Frame *outer = nullptr;
    const Entry *entry = nullptr;
    Kind kind = Kind::Before;
    size_t index = 0; // which of the entry's hooks of that kind
    uint32_t sp = 0, lr = 0;
    int guest = 0; // calls into guest code made from it that have not returned
};

// The heads, by context (a context is a guest thread's for its life; the
// list is empty whenever no hook runs on it, so a context made again at the
// same address finds it so). The calling thread's last lookup is cached.
struct Heads {
    std::mutex mutex;
    std::unordered_map<const ppc::Context *, std::unique_ptr<std::atomic<Frame *>>> by_context;
};
Heads &heads() {
    static Heads *h = new Heads; // never destroyed: a thread may run hooks after main returns
    return *h;
}
std::atomic<Frame *> &head(const ppc::Context &c) {
    thread_local const ppc::Context *cached_context = nullptr;
    thread_local std::atomic<Frame *> *cached = nullptr;
    if (cached_context == &c) return *cached;
    Heads &h = heads();
    std::lock_guard lock(h.mutex);
    auto &slot = h.by_context[&c];
    if (!slot) slot = std::make_unique<std::atomic<Frame *>>(nullptr);
    cached_context = &c, cached = slot.get();
    return *cached;
}

// A frame pushed for a run, popped as it ends (also by an exception: an
// Unwind or a thread's exit through a hook).
class Push {
  public:
    Push(ppc::Context &c, const Entry &e) : head_(head(c)) {
        frame_.entry = &e;
        frame_.sp = uint32_t(c.r[1]), frame_.lr = uint32_t(c.lr);
        frame_.outer = head_.load(std::memory_order_relaxed);
        head_.store(&frame_, std::memory_order_release);
    }
    ~Push() { head_.store(frame_.outer, std::memory_order_release); }
    void at(Kind kind, size_t index) { frame_.kind = kind, frame_.index = index; }

  private:
    std::atomic<Frame *> &head_;
    Frame frame_;
};

// The innermost frame on a context, marked as in guest code while `fn` runs.
template <class F> void in_guest(ppc::Context &c, F &&fn) {
    Frame *f = head(c).load(std::memory_order_relaxed);
    if (!f) return fn();
    struct Mark {
        Frame *f;
        ~Mark() { f->guest--; }
    } mark{f};
    f->guest++;
    fn();
}

template <class H> void count(const H &h) {
    if (counting.load(std::memory_order_relaxed)) [[unlikely]]
        h.counter->calls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

const char *kind_name(Kind kind) {
    switch (kind) {
    case Kind::Before: return "before";
    case Kind::After: return "after";
    case Kind::Replace: return "replace";
    default: return "mid";
    }
}

const uint8_t *pages() { return page_bits; }

const Entry *find(uint32_t address) {
    Registry &r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.entries.find(address);
    return it == r.entries.end() ? nullptr : it->second;
}

bool slotted(uint32_t address) {
    Registry &r = registry();
    std::shared_lock lock(r.mutex);
    return r.function_slots.count(address) != 0;
}

uint32_t function_size(uint32_t address) {
    Registry &r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.function_slots.find(address);
    return it == r.function_slots.end() ? 0 : it->second;
}

Pin::Pin() { resolving.fetch_add(1, std::memory_order_seq_cst); }
Pin::~Pin() {
    if (resolving_) resolving.fetch_sub(1, std::memory_order_seq_cst);
    if (entry_)
        for (Owner *o : entry_->owners) o->active.fetch_sub(1, std::memory_order_release);
}
const Entry *Pin::take(const Entry *e) {
    if (e && !entry_) {
        for (Owner *o : e->owners) o->active.fetch_add(1, std::memory_order_seq_cst);
        entry_ = e;
    }
    resolving.fetch_sub(1, std::memory_order_seq_cst);
    resolving_ = false;
    return e;
}

void bind(const ppc::GeneratedModule &m) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    for (const ppc::GeneratedModule *known : r.modules)
        if (known == &m) return;
    r.modules.push_back(&m);
    for (size_t k = 0; k < m.name_count; k++) r.names.emplace(m.names[k].address, m.names[k].name);
    for (size_t k = 0; k < m.hook_count; k++) {
        ppc::HookSlot &slot = *m.hooks[k];
        if (slot.original) r.function_slots[slot.address] = slot.size;
        auto it = r.entries.find(slot.address);
        store_slot(slot, it == r.entries.end() ? nullptr : it->second);
    }
    for (const auto &[address, e] : r.entries) warn_unslotted(r, *e);
}

void unbind(const ppc::GeneratedModule &m) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    std::erase(r.modules, &m);
    for (size_t k = 0; k < m.hook_count; k++)
        if (m.hooks[k]->original) r.function_slots.erase(m.hooks[k]->address);
}

void run(ppc::Context &c, const Entry &e, void (*fn)(ppc::Context &)) {
    Push frame(c, e);
    boundary(c, e.address, false, 0);
    for (size_t i = 0; i < e.before.size(); i++) {
        frame.at(Kind::Before, i);
        count(e.before[i]);
        e.before[i].fn(c);
    }
    const port::Original original(e.address, fn);
    if (e.replace.fn) {
        frame.at(Kind::Replace, 0);
        count(e.replace);
        e.replace.fn(c, original);
    } else {
        original(c);
    }
    for (size_t i = 0; i < e.after.size(); i++) {
        frame.at(Kind::After, i);
        count(e.after[i]);
        e.after[i].fn(c);
    }
    boundary(c, e.address, true, uint32_t(c.lr) & ~3u);
}

uint32_t run_mid(ppc::Context &c, const Entry &e) {
    Push frame(c, e);
    boundary(c, e.address, false, 0);
    uint32_t to = port::CONTINUE;
    for (size_t i = 0; i < e.mid.size(); i++) {
        frame.at(Kind::Mid, i);
        count(e.mid[i]);
        if ((to = e.mid[i].fn(c)) != port::CONTINUE) break;
    }
    boundary(c, e.address, true, to == port::CONTINUE ? e.address : to == port::RETURN ? uint32_t(c.lr) & ~3u : to);
    return to;
}

std::vector<FrameInfo> frames(const ppc::Context &c) {
    std::vector<FrameInfo> out;
    int n = 0;
    for (const Frame *f = head(c).load(std::memory_order_acquire); f && n < 64; f = f->outer, n++) {
        FrameInfo i;
        const Entry &e = *f->entry;
        i.kind = f->kind, i.address = e.address, i.sp = f->sp, i.lr = f->lr, i.in_guest = f->guest > 0;
        i.function = port::name_of(e.address);
        auto fill = [&](const auto &hooks) {
            if (f->index >= hooks.size()) return;
            const auto &h = hooks[f->index];
            i.hook = h.name, i.fn = reinterpret_cast<const void *>(h.fn);
            i.owner = h.owner ? h.owner->name : std::string();
        };
        switch (f->kind) {
        case Kind::Before: fill(e.before); break;
        case Kind::After: fill(e.after); break;
        case Kind::Replace:
            i.hook = e.replace.name, i.fn = reinterpret_cast<const void *>(e.replace.fn);
            i.owner = e.replace.owner ? e.replace.owner->name : std::string();
            break;
        case Kind::Mid: fill(e.mid); break;
        }
        out.push_back(std::move(i));
    }
    return out;
}

std::string describe(const FrameInfo &f) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", f.address);
    std::string out = std::string(kind_name(f.kind)) + " hook";
    if (!f.hook.empty()) out += " " + f.hook;
    out += f.kind == Kind::Mid ? " at " : " on ";
    // (the function's name, unless it is only the hook's: a registration names
    // the function when nothing else does)
    if (!f.function.empty() && f.function != f.hook) out += f.function + " ";
    out += hex;
    if (!f.owner.empty()) out += " (" + f.owner + ")";
    return out;
}

void set_stats(bool on) { counting.store(on, std::memory_order_relaxed); }
bool stats_on() { return counting.load(std::memory_order_relaxed); }

std::vector<Stat> stats() {
    Registry &r = registry();
    std::shared_lock lock(r.mutex);
    std::vector<Stat> out;
    for (const auto &[key, c] : r.counters) {
        Stat s;
        s.address = std::get<0>(key), s.kind = Kind(std::get<1>(key)), s.hook = std::get<2>(key);
        auto it = r.names.find(s.address);
        if (it != r.names.end()) s.function = it->second;
        s.calls = c->calls.load(std::memory_order_relaxed);
        out.push_back(std::move(s));
    }
    return out;
}

std::string stats_report() {
    std::string out = "hook calls:\n";
    std::vector<Stat> all = stats();
    if (all.empty()) return out + "  (no hooks registered)\n";
    for (const Stat &s : all) {
        char line[256];
        std::snprintf(line, sizeof(line), "  0x%08X %-24s %-7s %-24s %llu\n", s.address,
                      s.function.empty() ? "-" : s.function.c_str(), kind_name(s.kind),
                      s.hook.empty() ? "-" : s.hook.c_str(), (unsigned long long)s.calls);
        out += line;
    }
    return out;
}

Owner *make_owner(const std::string &name) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    auto o = std::make_unique<Owner>();
    o->id = r.next_owner++, o->name = name;
    r.owners.push_back(std::move(o));
    return r.owners.back().get();
}

Owner *set_registering(Owner *owner) { return std::exchange(registering, owner); }

void remove_owner(Owner *owner) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    std::vector<uint32_t> addresses;
    for (const auto &[address, e] : r.entries)
        if (std::find(e->owners.begin(), e->owners.end(), owner) != e->owners.end()) addresses.push_back(address);
    for (uint32_t address : addresses)
        update_locked(r, address, nullptr, [&](Entry &e) {
            if (e.replace.owner == owner) e.replace = {};
            auto mine = [&](const auto &h) { return h.owner == owner; };
            std::erase_if(e.before, mine), std::erase_if(e.after, mine), std::erase_if(e.mid, mine);
        });
}

bool drained(Owner *owner, int wait_ms) {
    // every thread that read an entry before the owner's hooks were dropped
    // has pinned it (resolving seen at 0), then none runs its code
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        if (resolving.load(std::memory_order_seq_cst) == 0 && owner->active.load(std::memory_order_seq_cst) == 0)
            return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace xf::hooks

namespace xf::port {

void Original::operator()(Context &c) const {
    hooks::in_guest(c, [&] {
        if (fn_) return fn_(c);
        if (c.system && c.system->run_original) return c.system->run_original(c, address_);
        ppc::interpret(c, address_, uint32_t(c.lr) & ~3u, 100'000'000, uint32_t(c.r[1]));
    });
}

void call(Context &c, uint32_t address) {
    const uint64_t lr = c.lr;
    c.lr = CALL_RETURN;
    hooks::in_guest(c, [&] { ppc::dispatch(c, address); });
    c.lr = lr;
}

void replace(uint32_t address, Replacement fn, const char *name) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    auto h = hooks::make_hook<hooks::Hook<Replacement>>(r, address, hooks::Kind::Replace, fn, name);
    hooks::update_locked(r, address, name, [&](hooks::Entry &e) {
        if (e.replace.fn && e.replace.fn != fn)
            std::fprintf(stderr, "port: 0x%08X is replaced twice: the last replacement stands\n", address);
        e.replace = h;
    });
}
void before(uint32_t address, Callback fn, const char *name) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    auto h = hooks::make_hook<hooks::Hook<Callback>>(r, address, hooks::Kind::Before, fn, name);
    hooks::update_locked(r, address, name, [&](hooks::Entry &e) { e.before.push_back(h); });
}
void after(uint32_t address, Callback fn, const char *name) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    auto h = hooks::make_hook<hooks::Hook<Callback>>(r, address, hooks::Kind::After, fn, name);
    hooks::update_locked(r, address, name, [&](hooks::Entry &e) { e.after.push_back(h); });
}
void mid(uint32_t address, MidHook fn, const char *name) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    auto h = hooks::make_hook<hooks::Hook<MidHook>>(r, address, hooks::Kind::Mid, fn, name);
    hooks::update_locked(r, address, name, [&](hooks::Entry &e) { e.mid.push_back(h); });
}

void remove(uint32_t address) {
    hooks::update(address, nullptr, [](hooks::Entry &e) {
        e.replace = {};
        e.before.clear(), e.after.clear(), e.mid.clear();
    });
}

void name(uint32_t address, const std::string &name) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    r.names[address] = name;
}

std::string name_of(uint32_t address) {
    hooks::Registry &r = hooks::registry();
    std::shared_lock lock(r.mutex);
    auto it = r.names.find(address);
    return it == r.names.end() ? std::string() : it->second;
}

namespace {
std::function<double(const char *)> &option_reader() {
    static std::function<double(const char *)> reader;
    return reader;
}
} // namespace

void set_option_reader(std::function<double(const char *)> reader) { option_reader() = std::move(reader); }
double option(const char *key) {
    const auto &reader = option_reader();
    return reader ? reader(key) : std::nan("");
}

void *persistent(const char *key, size_t size, bool *made) {
    hooks::Registry &r = hooks::registry();
    std::unique_lock lock(r.mutex);
    auto &block = r.blocks[key];
    bool fresh = !block.first;
    if (!fresh && block.second != size) {
        std::fprintf(stderr, "port: the state \"%s\" changed size (%zu bytes, then %zu): made anew\n", key,
                     block.second, size);
        fresh = true; // (the old block is kept: a thread may still read it)
        static std::vector<std::unique_ptr<uint64_t[]>> old;
        old.push_back(std::move(block.first));
    }
    if (fresh) block.first = std::make_unique<uint64_t[]>((size + 7) / 8 + 1), block.second = size;
    if (made) *made = fresh;
    return block.first.get();
}

} // namespace xf::port

// what the generated code calls (ppc_runtime.h)
namespace xf::ppc {
void hooked(Context &c, HookSlot &slot) {
    hooks::Pin pin;
    const auto *e = pin.take(static_cast<const hooks::Entry *>(__atomic_load_n(&slot.hooks, __ATOMIC_SEQ_CST)));
    if (!e) return slot.original(c); // (removed since the slot was read)
    hooks::run(c, *e, slot.original);
}
uint32_t mid_hook(Context &c, HookSlot &slot) {
    hooks::Pin pin;
    const auto *e = pin.take(static_cast<const hooks::Entry *>(__atomic_load_n(&slot.hooks, __ATOMIC_SEQ_CST)));
    return e ? hooks::run_mid(c, *e) : 0;
}
} // namespace xf::ppc
