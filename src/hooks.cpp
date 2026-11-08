// The port's hooks (xenoforge/hooks.h, xenoforge/port.h): the registry, the
// slots bound to it, and the runs.

#include "xenoforge/hooks.h"

#include "xenoforge/ppc_interp.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>

namespace xf::hooks {
namespace {

// a bit per 4 KB page: 2^20 pages, 128 KB, zero until a hook is registered
uint8_t page_bits[1u << 17];

struct Registry {
    std::shared_mutex mutex;
    std::unordered_map<uint32_t, const Entry *> entries;
    std::vector<std::unique_ptr<Entry>> versions; // every Entry published: never dropped
    std::vector<const ppc::GeneratedModule *> modules;
    std::unordered_map<uint32_t, uint32_t> function_slots; // address, size (bound modules')
    std::unordered_map<uint32_t, std::string> names;       // the modules', then the port's
    std::set<uint32_t> warned;
};

Registry &registry() {
    static Registry r;
    return r;
}

void store_slot(ppc::HookSlot &slot, const Entry *e) {
    const void *hooks = nullptr;
    if (e && (slot.original ? e->function() : !e->mid.empty())) hooks = e;
    __atomic_store_n(&slot.hooks, hooks, __ATOMIC_RELEASE);
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

// A new version of the hooks at an address, published and bound
template <class F> void update(uint32_t address, const char *name, F &&change) {
    Registry &r = registry();
    std::unique_lock lock(r.mutex);
    auto it = r.entries.find(address);
    auto e = it == r.entries.end() ? std::make_unique<Entry>() : std::make_unique<Entry>(*it->second);
    e->address = address;
    if (name && *name && e->name.empty()) e->name = name;
    change(*e);
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

// the port's code around guest code, for the lockstep check (System's hook_boundary)
void boundary(ppc::Context &c, uint32_t address, bool after, uint32_t resume) {
    if (c.system && c.system->hook_boundary) c.system->hook_boundary(c, address, after, resume);
}

} // namespace

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
    boundary(c, e.address, false, 0);
    for (port::Callback b : e.before) b(c);
    const port::Original original(e.address, fn);
    if (e.replace) e.replace(c, original);
    else original(c);
    for (port::Callback a : e.after) a(c);
    boundary(c, e.address, true, uint32_t(c.lr) & ~3u);
}

uint32_t run_mid(ppc::Context &c, const Entry &e) {
    boundary(c, e.address, false, 0);
    uint32_t to = port::CONTINUE;
    for (port::MidHook h : e.mid)
        if ((to = h(c)) != port::CONTINUE) break;
    boundary(c, e.address, true, to == port::CONTINUE ? e.address : to == port::RETURN ? uint32_t(c.lr) & ~3u : to);
    return to;
}

} // namespace xf::hooks

namespace xf::port {

void Original::operator()(Context &c) const {
    if (fn_) return fn_(c);
    if (c.system && c.system->run_original) return c.system->run_original(c, address_);
    ppc::interpret(c, address_, uint32_t(c.lr) & ~3u, 100'000'000, uint32_t(c.r[1]));
}

void replace(uint32_t address, Replacement fn, const char *name) {
    hooks::update(address, name, [&](hooks::Entry &e) {
        if (e.replace && e.replace != fn)
            std::fprintf(stderr, "port: 0x%08X is replaced twice: the last replacement stands\n", address);
        e.replace = fn;
    });
}
void before(uint32_t address, Callback fn, const char *name) {
    hooks::update(address, name, [&](hooks::Entry &e) { e.before.push_back(fn); });
}
void after(uint32_t address, Callback fn, const char *name) {
    hooks::update(address, name, [&](hooks::Entry &e) { e.after.push_back(fn); });
}
void mid(uint32_t address, MidHook fn, const char *name) {
    hooks::update(address, name, [&](hooks::Entry &e) { e.mid.push_back(fn); });
}

void remove(uint32_t address) {
    hooks::update(address, nullptr, [](hooks::Entry &e) {
        e.replace = nullptr;
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

} // namespace xf::port

// what the generated code calls (ppc_runtime.h)
namespace xf::ppc {
void hooked(Context &c, HookSlot &slot) {
    const auto *e = static_cast<const hooks::Entry *>(__atomic_load_n(&slot.hooks, __ATOMIC_ACQUIRE));
    if (!e) return slot.original(c); // (removed since the slot was read)
    hooks::run(c, *e, slot.original);
}
uint32_t mid_hook(Context &c, HookSlot &slot) {
    const auto *e = static_cast<const hooks::Entry *>(__atomic_load_n(&slot.hooks, __ATOMIC_ACQUIRE));
    return e ? hooks::run_mid(c, *e) : 0;
}
} // namespace xf::ppc
