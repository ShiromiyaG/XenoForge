// What a port includes (docs/recompiler.md, "Replacing and hooking"; plan
// E3): the guest's memory and registers through typed big-endian helpers,
// calls into guest code, and the hooks a port puts on guest functions:
//
//   - a replacement: native C++ in a function's place, with the original body
//     a call away;
//   - before and after: native code around a function, which runs as ever;
//   - mid-function: native code before the instruction at an address inside
//     a function, getting the whole context and saying where the code goes
//     on (on, a return, or another address).
//
// Hooks bind by guest address, registered at link time (XF_REPLACE,
// XF_BEFORE, XF_AFTER, XF_MID: static registrations in the port's sources,
// done before main) or at run time (replace(), before(), ...). Every way into
// a function the project file lists in `hooks` (or `mid_hooks` for an
// address) reaches the hooks: direct calls in the generated code go through
// its hook slot. An address the project file does not list is reached only
// through dispatch: indirect calls, calls from interpreted code, and the
// interpreter's own branches; direct calls in the generated code go past it,
// and the registration says so on stderr. The interpreter and the lockstep
// check honor the same hooks.
//
//   #include "xenoforge/port.h"
//   using namespace xf::port;
//
//   XF_REPLACE(0x82012340, clamp_speed) {   // a function the project file hooks
//       original(c);                        // the guest's own body
//       if (f(c, 1) > 30.0) set_f(c, 1, 30.0);
//   }
//   XF_BEFORE(0x82012400, count_frames) { frames++; }
//   XF_MID(0x82012468, skip_check) {
//       return r(c, 3) == 0 ? 0x82012480u : CONTINUE; // go on at a label the project file gave
//   }
//
// A port links the generated code statically (its sources in the executable,
// or an OBJECT library: a static library would drop a source holding only
// registrations). The guest's state is the Context: c.r[n], c.f[n], c.lr...

#pragma once

#include "xenoforge/ppc_context.h"
#include "xenoforge/ppc_runtime.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace xf::port {

using Context = ppc::Context;

// ---- the guest's memory ----

// The host address of a guest address (the 0xE0000000 view sits 4 KB further
// on in the host's mapping, as the instructions see it).
inline uint8_t *host(const Context &c, uint32_t address) {
    return c.base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

template <class T> constexpr T byteswap(T v) {
    static_assert(std::is_trivially_copyable_v<T> &&
                  (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
    if constexpr (sizeof(T) == 1)
        return v;
    else if constexpr (sizeof(T) == 2)
        return std::bit_cast<T>(__builtin_bswap16(std::bit_cast<uint16_t>(v)));
    else if constexpr (sizeof(T) == 4)
        return std::bit_cast<T>(__builtin_bswap32(std::bit_cast<uint32_t>(v)));
    else
        return std::bit_cast<T>(__builtin_bswap64(std::bit_cast<uint64_t>(v)));
}

// A value as the guest stores it (big-endian), for the fields of guest
// structures a port overlays on guest memory (ref<T>):
//
//   struct Actor { be<uint32_t> flags; be<float> x, y, z; ptr<Actor> next; };
//   Actor &a = ref<Actor>(c, uint32_t(c.r[3]));
//   a.x = a.x + 1.0f;
template <class T> struct be {
    T raw; // as stored: big-endian
    be() = default;
    be(T v) : raw(byteswap(v)) {}
    operator T() const { return byteswap(raw); }
    T get() const { return byteswap(raw); }
    be &operator=(T v) {
        raw = byteswap(v);
        return *this;
    }
};

// A guest pointer: a 32-bit guest address, stored big-endian.
template <class T> struct ptr {
    be<uint32_t> address;
    ptr() = default;
    ptr(uint32_t a) : address(a) {}
    explicit operator bool() const { return address.get() != 0; }
    uint32_t value() const { return address.get(); }
    T *get(const Context &c) const { return address.get() ? reinterpret_cast<T *>(host(c, address.get())) : nullptr; }
};

// The guest's memory at an address as a T (a structure of be<> fields).
template <class T> T &ref(const Context &c, uint32_t address) { return *reinterpret_cast<T *>(host(c, address)); }

// One value, big-endian: integers, float, double.
template <class T> T load(const Context &c, uint32_t address) {
    T v;
    std::memcpy(&v, host(c, address), sizeof(T));
    return byteswap(v);
}
template <class T> void store(const Context &c, uint32_t address, T value) {
    value = byteswap(value);
    std::memcpy(host(c, address), &value, sizeof(T));
}

// ---- the registers, as the calling convention uses them ----

// the n-th integer argument (r3 + n), 32 and 64 bits
inline uint32_t arg(const Context &c, int n) { return uint32_t(c.r[3 + n]); }
inline uint64_t arg64(const Context &c, int n) { return c.r[3 + n]; }
// the n-th floating-point argument (f1 + n)
inline double farg(const Context &c, int n) { return c.f[1 + n]; }
// r[n] and f[n] by number, and a result in r3 or f1
inline uint64_t r(const Context &c, int n) { return c.r[n]; }
inline double f(const Context &c, int n) { return c.f[n]; }
inline void set_r(Context &c, int n, uint64_t v) { c.r[n] = v; }
inline void set_f(Context &c, int n, double v) { c.f[n] = v; }
inline void result(Context &c, uint64_t v) { c.r[3] = v; }
inline void fresult(Context &c, double v) { c.f[1] = v; }

// A call into guest code from native code (a hook, a replacement): the
// function at `address` with the registers as they are (arguments in r3...),
// back when it returns; LR is kept. Generated code where there is some, the
// interpreter where not, and the hooks there too.
constexpr uint32_t CALL_RETURN = 0xBCBCBCBC; // the return address it runs with: no code is there
inline void call(Context &c, uint32_t address) {
    const uint64_t lr = c.lr;
    c.lr = CALL_RETURN;
    ppc::dispatch(c, address);
    c.lr = lr;
}

// ---- hooks ----

// A replaced function's own body: generated code, else the interpreter.
class Original {
  public:
    Original(uint32_t address, void (*fn)(Context &)) : address_(address), fn_(fn) {}
    void operator()(Context &c) const;
    uint32_t address() const { return address_; }

  private:
    uint32_t address_;
    void (*fn_)(Context &);
};

using Replacement = void (*)(Context &c, const Original &original);
using Callback = void (*)(Context &c);
// What a mid-function hook returns: CONTINUE (the instruction at its address
// runs, then the rest), RETURN (the function returns, as a blr: its frame is
// the hook's to undo), or the guest address to go on at (a label the project
// file gave, the next instruction, else any address, dispatched).
using MidHook = uint32_t (*)(Context &c);
constexpr uint32_t CONTINUE = 0, RETURN = 1;

// Registration at run time (also what the link-time macros call). A
// function has at most one replacement (a second replaces the first) and any
// number of before and after hooks, run in the order registered; mid hooks
// at one address run in order until one says other than CONTINUE. `name`
// names the function in the reports.
void replace(uint32_t address, Replacement fn, const char *name = nullptr);
void before(uint32_t address, Callback fn, const char *name = nullptr);
void after(uint32_t address, Callback fn, const char *name = nullptr);
void mid(uint32_t address, MidHook fn, const char *name = nullptr);
// every hook at an address dropped
void remove(uint32_t address);
// a name for a function, in the crash and stop reports and the lockstep
// check's (the project file's names are there already)
void name(uint32_t address, const std::string &name);
std::string name_of(uint32_t address); // empty: none

// A static registration (the macros below).
struct Registration {
    enum When { Before, After };
    Registration(uint32_t address, Replacement fn, const char *name) { replace(address, fn, name); }
    Registration(uint32_t address, When when, Callback fn, const char *name) {
        when == Before ? before(address, fn, name) : after(address, fn, name);
    }
    Registration(uint32_t address, MidHook fn, const char *name) { mid(address, fn, name); }
};

} // namespace xf::port

#define XF_PORT_CAT2(a, b) a##b
#define XF_PORT_CAT(a, b) XF_PORT_CAT2(a, b)

// void name(Context &c, const Original &original) { ... } in place of the function at `address`
#define XF_REPLACE(address, name)                                                                                      \
    static void name(::xf::port::Context &c, const ::xf::port::Original &original);                                    \
    static const ::xf::port::Registration XF_PORT_CAT(xf_registration_, name){(address), &name, #name};                \
    static void name([[maybe_unused]] ::xf::port::Context &c, [[maybe_unused]] const ::xf::port::Original &original)
// void name(Context &c) { ... } before the function at `address`
#define XF_BEFORE(address, name)                                                                                       \
    static void name(::xf::port::Context &c);                                                                          \
    static const ::xf::port::Registration XF_PORT_CAT(xf_registration_, name){                                         \
        (address), ::xf::port::Registration::Before, &name, #name};                                                    \
    static void name([[maybe_unused]] ::xf::port::Context &c)
// void name(Context &c) { ... } after it, its result in r3 or f1
#define XF_AFTER(address, name)                                                                                        \
    static void name(::xf::port::Context &c);                                                                          \
    static const ::xf::port::Registration XF_PORT_CAT(xf_registration_,                                                \
                                                      name){(address), ::xf::port::Registration::After, &name, #name}; \
    static void name([[maybe_unused]] ::xf::port::Context &c)
// uint32_t name(Context &c) { ...; return CONTINUE; } before the instruction at `address`
#define XF_MID(address, name)                                                                                          \
    static uint32_t name(::xf::port::Context &c);                                                                      \
    static const ::xf::port::Registration XF_PORT_CAT(xf_registration_, name){(address), &name, #name};                \
    static uint32_t name([[maybe_unused]] ::xf::port::Context &c)
