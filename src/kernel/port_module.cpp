// A port's hooks as a module loaded at run time (xenoforge/port_module.h;
// docs/sdk.md, "Reloading the hooks"): the copies loaded, the table each one
// gets, and the reload at a safe point.

#include "xenoforge/port_module.h"

#include "xenoforge/hooks.h"
#include "xenoforge/kernel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace xf::kernel {
namespace fs = std::filesystem;

namespace {

void *open_library(const fs::path &path, std::string &error) {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(path.wstring().c_str());
    if (!m) error = path.string() + ": cannot be loaded (error " + std::to_string(GetLastError()) + ")";
    return m;
#else
    void *m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        const char *why = dlerror();
        error = path.string() + ": cannot be loaded (" + (why ? why : "?") + ")";
    }
    return m;
#endif
}
void *symbol(void *library, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}
void close_library(void *library) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}
uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t(getpid());
#endif
}

// The runtime's side of the table: each call made as the module's owner.
struct Registering {
    hooks::Owner *before;
    explicit Registering(void *owner) : before(hooks::set_registering(static_cast<hooks::Owner *>(owner))) {}
    ~Registering() { hooks::set_registering(before); }
};
void api_replace(void *owner, uint32_t address, port::Replacement fn, const char *name) {
    Registering r(owner);
    port::replace(address, fn, name);
}
void api_before(void *owner, uint32_t address, port::Callback fn, const char *name) {
    Registering r(owner);
    port::before(address, fn, name);
}
void api_after(void *owner, uint32_t address, port::Callback fn, const char *name) {
    Registering r(owner);
    port::after(address, fn, name);
}
void api_mid(void *owner, uint32_t address, port::MidHook fn, const char *name) {
    Registering r(owner);
    port::mid(address, fn, name);
}
void api_remove(void *, uint32_t address) { port::remove(address); }
void api_name(uint32_t address, const char *name) { port::name(address, name ? name : ""); }
size_t api_name_of(uint32_t address, char *out, size_t size) {
    const std::string name = port::name_of(address);
    if (size) {
        const size_t n = std::min(name.size(), size - 1);
        std::memcpy(out, name.data(), n);
        out[n] = 0;
    }
    return name.size();
}
void api_original(ppc::Context &c, uint32_t address, void (*fn)(ppc::Context &)) { port::Original(address, fn)(c); }
void api_call(ppc::Context &c, uint32_t address) { port::call(c, address); }
void *api_persistent(const char *key, size_t size, bool *made) { return port::persistent(key, size, made); }
double api_option(const char *key) { return port::option(key); }

// One copy loaded: its library, its table, its owner in the registry.
struct Copy {
    fs::path file; // the copy's own file (removed once unloaded)
    void *library = nullptr;
    port::ModuleApi api{};
    hooks::Owner *owner = nullptr;
};

} // namespace

struct PortModule::State {
    fs::path path;
    fs::file_time_type stamp{};
    uintmax_t size = 0;
    // a change seen and when, waited on until it settles
    fs::file_time_type seen_stamp{};
    uintmax_t seen_size = 0;
    std::chrono::steady_clock::time_point seen_at{}, checked_at{};
    std::unique_ptr<Copy> current;
    std::vector<std::unique_ptr<Copy>> retired; // replaced, still run by some thread
    uint32_t loads = 0;

    // the file copied, loaded and its version checked: not registered yet
    std::unique_ptr<Copy> open(std::string &error) {
        std::error_code ec;
        const fs::file_time_type at = fs::last_write_time(path, ec);
        const uintmax_t bytes = fs::file_size(path, ec);
        if (ec) return error = path.string() + ": " + ec.message(), nullptr;
        const fs::path dir = fs::temp_directory_path(ec) / "xenoforge-port";
        fs::create_directories(dir, ec);
        auto copy = std::make_unique<Copy>();
        copy->file = dir / (path.stem().string() + "-" + std::to_string(process_id()) + "-" + std::to_string(loads) +
                            path.extension().string());
        fs::copy_file(path, copy->file, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return error = "cannot copy " + path.string() + " to " + copy->file.string() + ": " + ec.message(), nullptr;
        if (!(copy->library = open_library(copy->file, error))) {
            fs::remove(copy->file, ec);
            return nullptr;
        }
        auto version = reinterpret_cast<XfPortModuleVersion>(symbol(copy->library, XF_PORT_MODULE_VERSION_NAME));
        auto entry = reinterpret_cast<XfPortModuleEntry>(symbol(copy->library, XF_PORT_MODULE_ENTRY_NAME));
        if (!version || !entry || version() != port::MODULE_API_VERSION) {
            error = path.string() + (!version || !entry ? ": not a port module (no " XF_PORT_MODULE_ENTRY_NAME ")"
                                                        : ": built for another port module interface");
            close(*copy);
            return nullptr;
        }
        stamp = at, size = bytes;
        seen_stamp = at, seen_size = bytes;
        loads++;
        copy->owner = hooks::make_owner(path.filename().string() + "#" + std::to_string(loads));
        copy->api = {port::MODULE_API_VERSION,
                     uint32_t(sizeof(port::ModuleApi)),
                     copy->owner,
                     api_replace,
                     api_before,
                     api_after,
                     api_mid,
                     api_remove,
                     api_name,
                     api_name_of,
                     api_original,
                     api_call,
                     api_persistent,
                     api_option};
        return copy;
    }
    // its hooks registered (its static registrations, then what its entry does)
    bool start(Copy &copy, std::string &error) {
        auto entry = reinterpret_cast<XfPortModuleEntry>(symbol(copy.library, XF_PORT_MODULE_ENTRY_NAME));
        Registering r(copy.owner);
        if (!entry(&copy.api)) return error = path.string() + ": its entry refused the runtime's table", false;
        return true;
    }
    static void close(Copy &copy) {
        if (copy.library) close_library(copy.library);
        copy.library = nullptr;
        std::error_code ec;
        fs::remove(copy.file, ec);
    }
    size_t collect(int wait_ms) {
        std::erase_if(retired, [&](std::unique_ptr<Copy> &c) {
            if (!hooks::drained(c->owner, wait_ms)) return false;
            close(*c);
            return true;
        });
        return retired.size();
    }
};

PortModule::PortModule() : s_(std::make_unique<State>()) {}
// (left loaded: the process ends with guest threads that may run its hooks)
PortModule::~PortModule() = default;

bool PortModule::load(const fs::path &path, std::string &error) {
    s_->path = path;
    std::unique_ptr<Copy> copy = s_->open(error);
    if (!copy) return false;
    if (!s_->start(*copy, error)) {
        hooks::remove_owner(copy->owner);
        State::close(*copy);
        return false;
    }
    s_->current = std::move(copy);
    return true;
}

bool PortModule::changed(uint32_t settle_ms) {
    // (the file looked at four times a second at most: the player asks every frame)
    const auto asked = std::chrono::steady_clock::now();
    if (asked - s_->checked_at < std::chrono::milliseconds(250)) return false;
    s_->checked_at = asked;
    std::error_code ec;
    const fs::file_time_type at = fs::last_write_time(s_->path, ec);
    const uintmax_t bytes = fs::file_size(s_->path, ec);
    if (ec || (at == s_->stamp && bytes == s_->size)) return false;
    const auto now = std::chrono::steady_clock::now();
    if (at != s_->seen_stamp || bytes != s_->seen_size) {
        s_->seen_stamp = at, s_->seen_size = bytes, s_->seen_at = now;
        return false;
    }
    return now - s_->seen_at >= std::chrono::milliseconds(settle_ms);
}

bool PortModule::reload(Kernel &k, std::string &error, std::string *report, uint32_t pause_ms) {
    // the new copy loaded before anything stops: a bad build changes nothing
    std::unique_ptr<Copy> copy = s_->open(error);
    if (!copy) return false;
    const auto started = std::chrono::steady_clock::now();
    const bool parked = k.pause(pause_ms);
    const uint32_t unparked = k.pause_unparked();
    if (s_->current) hooks::remove_owner(s_->current->owner);
    const bool ok = s_->start(*copy, error);
    k.resume();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!ok) {
        // (the old module's hooks are gone; its copy stays loaded, retired)
        hooks::remove_owner(copy->owner);
        State::close(*copy);
        if (s_->current) s_->retired.push_back(std::move(s_->current));
        return false;
    }
    if (s_->current) s_->retired.push_back(std::move(s_->current));
    s_->current = std::move(copy);
    const size_t still = s_->collect(100);
    if (report) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "port: %s reloaded (load %u), the guest paused %.1f ms%s; %zu old cop%s still in use\n",
                      s_->path.filename().string().c_str(), s_->loads, ms,
                      parked ? "" : (", " + std::to_string(unparked) + " thread(s) not parked in time").c_str(), still,
                      still == 1 ? "y" : "ies");
        *report = line;
    }
    return true;
}

size_t PortModule::collect() { return s_->collect(0); }
uint32_t PortModule::reloads() const { return s_->loads ? s_->loads - 1 : 0; }
const fs::path &PortModule::path() const { return s_->path; }

} // namespace xf::kernel
