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
// Its hooks may also be a module of their own, reloaded while the game runs
// (xenoforge_add_port's HOOK_SOURCES and HOT_RELOAD; xenoforge/port_module.h):
// the same code, built with XF_PORT_MODULE, seeing this header alone.

#pragma once

#include "xenoforge/ppc_context.h"
#include "xenoforge/ppc_runtime.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
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
// The crash and stop reports show the call as the hook that made it.
constexpr uint32_t CALL_RETURN = 0xBCBCBCBC; // the return address it runs with: no code is there
void call(Context &c, uint32_t address);

// ---- typed calls into guest code (docs/sdk.md, "Calling the game") ----
//
// Temporaries on the guest's stack for the calls a hook makes: strings, a
// structure, an out value. Below the stack pointer as the hook found it,
// past a red zone (a leaf function's saves below its stack pointer, at a
// mid-function hook), then the linkage area a callee writes into (its saved
// LR at -8, the parameter words above its stack pointer), with the back chain
// at the bottom. While it lives, r1 is that bottom; it puts r1 back.
//
//   GuestStack stack(c);
//   const uint32_t menu = stack.string("TopMenu");
//   invoke(c, 0x824AE690, this_, menu);
class GuestStack {
  public:
    static constexpr uint32_t RED_ZONE = 0x120, LINKAGE = 0x60;
    explicit GuestStack(Context &c) : c_(c), saved_(c.r[1]), cursor_(uint32_t(c.r[1]) - RED_ZONE) { place(); }
    ~GuestStack() { c_.r[1] = saved_; }
    GuestStack(const GuestStack &) = delete;
    GuestStack &operator=(const GuestStack &) = delete;
    // `size` bytes, aligned to 16 (zeroed): their guest address
    uint32_t reserve(uint32_t size) {
        cursor_ = (cursor_ - size) & ~15u;
        std::memset(host(c_, cursor_), 0, size);
        place();
        return cursor_;
    }
    // bytes copied in as they are
    uint32_t bytes(const void *data, uint32_t size) {
        const uint32_t at = reserve(size);
        std::memcpy(host(c_, at), data, size);
        return at;
    }
    // a value as it is in memory (a structure of be<> fields, a be<T>)
    template <class T> uint32_t value(const T &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        return bytes(&v, uint32_t(sizeof(T)));
    }
    // an 8-bit string, NUL-terminated
    uint32_t string(std::string_view s) {
        const uint32_t at = reserve(uint32_t(s.size()) + 1);
        std::memcpy(host(c_, at), s.data(), s.size());
        return at;
    }
    // a UTF-16 string as the guest keeps it (big-endian), NUL-terminated
    uint32_t wstring(std::u16string_view s) {
        const uint32_t at = reserve(uint32_t(s.size() + 1) * 2);
        for (size_t k = 0; k < s.size(); k++) store<uint16_t>(c_, at + 2 * uint32_t(k), uint16_t(s[k]));
        return at;
    }

  private:
    // r1 below the temporaries and the linkage area, its back chain to the hook's frame
    void place() {
        const uint32_t sp = (cursor_ - LINKAGE) & ~15u;
        store<uint32_t>(c_, sp, uint32_t(saved_));
        c_.r[1] = sp;
    }
    Context &c_;
    uint64_t saved_;
    uint32_t cursor_;
};

namespace detail {
template <class T> constexpr bool float_arg = std::is_floating_point_v<T>;
template <class T> void put_arg(Context &c, int &gpr, int &fpr, T v) {
    if constexpr (float_arg<T>) {
        if (fpr <= 13) c.f[fpr++] = double(v);
    } else if constexpr (std::is_pointer_v<T>) {
        static_assert(!std::is_pointer_v<T>, "pass guest addresses (uint32_t), not host pointers");
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "an argument is an integer, an enum or a float");
        if (gpr <= 10) c.r[gpr++] = uint64_t(int64_t(v)) & (sizeof(T) == 8 ? ~0ull : 0xFFFFFFFFull);
    }
}
} // namespace detail

// The guest function at `address` called with C++ arguments: integers, enums
// and guest addresses (uint32_t) in r3-r10 in order, float and double in
// f1-f13 in order (the PowerPC ABI counts them apart), at most eight and
// thirteen. Its result as R: r3 (an integer, an enum, bool), f1 (float,
// double), nothing for void. Every register comes back as it was, r1
// included, so a hook calling the game's functions leaves the call it hooks
// as it found it; what the callee wrote into memory stays.
template <class R = void, class... A> R invoke(Context &c, uint32_t address, A... args) {
    static_assert(std::is_void_v<R> || std::is_integral_v<R> || std::is_enum_v<R> || std::is_floating_point_v<R>,
                  "a result is void, an integer, an enum or a float");
    uint64_t r[32];
    double f[32];
    std::remove_reference_t<decltype(c.v[0])> v[128];
    uint8_t cr[32];
    std::memcpy(r, c.r, sizeof(r)), std::memcpy(f, c.f, sizeof(f)), std::memcpy(v, c.v, sizeof(v));
    std::memcpy(cr, c.cr, sizeof(cr));
    const uint64_t ctr = c.ctr;
    const uint8_t ca = c.xer_ca, ov = c.xer_ov, so = c.xer_so, bc = c.xer_bc, sat = c.vscr_sat;
    const uint32_t fpscr = c.fpscr;
    int gpr = 3, fpr = 1;
    (detail::put_arg(c, gpr, fpr, args), ...);
    call(c, address);
    const uint64_t r3 = c.r[3];
    const double f1 = c.f[1];
    std::memcpy(c.r, r, sizeof(r)), std::memcpy(c.f, f, sizeof(f)), std::memcpy(c.v, v, sizeof(v));
    std::memcpy(c.cr, cr, sizeof(cr));
    c.ctr = ctr, c.xer_ca = ca, c.xer_ov = ov, c.xer_so = so, c.xer_bc = bc, c.vscr_sat = sat, c.fpscr = fpscr;
    if constexpr (std::is_floating_point_v<R>) return R(f1);
    else if constexpr (std::is_same_v<R, bool>) return (r3 & 0xFFFFFFFF) != 0;
    else if constexpr (!std::is_void_v<R>) return R(r3);
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

// Port state that outlives a reload of the port's hooks (a module loaded at
// run time, xenoforge_add_port's HOT_RELOAD; docs/sdk.md, "Reloading the
// hooks"): a block of `size` bytes kept by the runtime under `key`, zeroed
// the first time, the same block every time after (in this load and the
// next ones). `made` says it was just made. What a module keeps elsewhere
// (its statics, its heap, its threads) is gone with it. In an executable it
// is a block that lives as long as the process.
void *persistent(const char *key, size_t size, bool *made = nullptr);
// A T (trivially copyable, so a reloaded module may read what the last one
// wrote) kept as persistent() keeps it, value-initialized the first time.
// A T whose size changed between two loads is a new block (a warning).
template <class T> T &persistent(const char *key) {
    static_assert(std::is_trivially_copyable_v<T>, "port state that outlives a reload is plain data");
    bool made = false;
    void *p = persistent(key, sizeof(T), &made);
    if (made) new (p) T{};
    return *static_cast<T *>(p);
}

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
