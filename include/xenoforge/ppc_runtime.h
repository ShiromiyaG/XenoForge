// What the generated code needs from outside itself (docs/runtime.md, "The
// game library"): the runtime's calls (dispatch, invalid, unwind,
// switch_stack, resumable) and the few pieces of the C library the
// instructions' semantics use. One description, reached two ways:
//
// - linked statically (boot_<game>, player_<game>, a port, the tests):
//   direct calls to the runtime's functions and to the C library;
// - in a game library (XF_GAME_LIBRARY: a DLL or .so the recompile step
//   builds on the player's machine, game_build.h): freestanding, with no
//   standard header and no C library, the same calls going through the table
//   the runtime passes in when it loads the library (Runtime below,
//   game_library.h). So the toolchain that builds it is clang and lld alone.
//
// The rest of the standard library the instructions use is compiler builtins
// either way (lib:: below), so both compile to the same code. The guest's
// registers, its memory's base and the system's callbacks (sc, traps, mftb,
// MMIO) come in the Context each call is given (ppc_context.h).

#pragma once

#if defined(__clang__) || defined(__GNUC__)
#define XF_INLINE inline __attribute__((always_inline))
#else
#define XF_INLINE __forceinline
#endif

#ifdef XF_GAME_LIBRARY
// the fixed-width types from the compiler's own definitions, no header
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ uintptr_t;
#define INT32_MAX __INT32_MAX__
#define INT32_MIN (-__INT32_MAX__ - 1)
#define INT64_MAX __INT64_MAX__
#define INT64_MIN (-__INT64_MAX__ - 1)
#define UINT32_MAX __UINT32_MAX__
#define UINT64_MAX __UINT64_MAX__
#else
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#endif

namespace xf::ppc {

struct Context;
struct HookSlot; // ppc_module.h

// The body of a function with resume sites (docs/recompiler.md, "Stack
// switches"), entered at `at` (0: its entry), with its entry's LR and r1.
using ResumableBody = void (*)(Context &c, uint32_t at, uint32_t lr, uint32_t sp);

// The runtime's table, as a game library gets it when it is loaded
// (game_library.h): the calls below, and the C library's pieces lib:: needs.
// The fenv calls number the rounding modes 0 to 3 (nearest, toward zero, up,
// down) and the exception flags 1 to 16 (inexact, underflow, overflow,
// divide by zero, invalid), not as the host's <cfenv> does: a game library
// uses them only on a host whose control register lib::FpEnv does not know.
// fma and fmaf are the C library's (exact, in the current rounding mode),
// for a library built for a CPU level without FMA.
struct Runtime {
    uint32_t version; // the interface version (game_library.h)
    uint32_t size;    // sizeof(Runtime)
    void (*dispatch)(Context &c, uint32_t target);
    void (*invalid)(Context &c, uint32_t pc);
    void (*unwind)(Context &c); // never returns
    void (*switch_stack)(Context &c, uint32_t thunk, uint32_t resume);
    void (*resumable)(Context &c, ResumableBody body, uint32_t lr, uint32_t sp, const uint32_t *sites, size_t count);
    void *(*memcpy)(void *to, const void *from, size_t n);
    void *(*memmove)(void *to, const void *from, size_t n);
    void *(*memset)(void *to, int value, size_t n);
    long long (*llrint)(double v);
    float (*exp2f)(float v);
    float (*log2f)(float v);
    double (*fmod)(double a, double b);
    int (*fesetround)(int mode);
    int (*feclearexcept)(int flags);
    int (*fetestexcept)(int flags);
    void (*hooked)(Context &c, HookSlot &slot);
    uint32_t (*mid_hook)(Context &c, HookSlot &slot);
    double (*fma)(double a, double b, double c);
    float (*fmaf)(float a, float b, float c);
};

#ifdef XF_GAME_LIBRARY
// the library's copy of the table, filled by its entry (game_library.h)
extern Runtime runtime;
#endif

// ---- the runtime's calls ----

#ifdef XF_GAME_LIBRARY
XF_INLINE void dispatch(Context &c, uint32_t target) { runtime.dispatch(c, target); }
XF_INLINE void invalid(Context &c, uint32_t pc) { runtime.invalid(c, pc); }
[[noreturn]] XF_INLINE void unwind(Context &c) {
    runtime.unwind(c);
    __builtin_unreachable();
}
XF_INLINE void switch_stack(Context &c, uint32_t thunk, uint32_t resume) { runtime.switch_stack(c, thunk, resume); }
XF_INLINE void resumable(Context &c, ResumableBody body, uint32_t lr, uint32_t sp, const uint32_t *sites,
                         size_t count) {
    runtime.resumable(c, body, lr, sp, sites, count);
}
XF_INLINE void hooked(Context &c, HookSlot &slot) { runtime.hooked(c, slot); }
XF_INLINE uint32_t mid_hook(Context &c, HookSlot &slot) { return runtime.mid_hook(c, slot); }
#else
// An indirect call or jump, or a call to code with no generated function:
// the function at `target`, else the interpreter from there until it
// returns. LR is already set as the code set it. Defined by whoever links
// the generated code (the kernel, a test), as the next three.
void dispatch(Context &c, uint32_t target);
// An invalid instruction reached.
void invalid(Context &c, uint32_t pc);
// Throws an Unwind (ppc_module.h) to where LR points (a function that
// switches the stack returning elsewhere than to its caller).
[[noreturn]] void unwind(Context &c);
// A branch to KeSetCurrentStackPointers' thunk (a fiber switch), with where
// this frame's code goes on when its stack is switched back to: the return
// address after a bl, the function's own return address for a tail branch.
// The kernel runs each guest stack on a host stack of its own.
void switch_stack(Context &c, uint32_t thunk, uint32_t resume);
// A function with resume sites: runs its body from its entry, and again from
// a resume site (sites: sorted) when an Unwind to one of them comes back with
// r1 inside its frame (below sp); an Unwind for a frame further up goes on.
// The generated code catches nothing itself: Unwinds are thrown and caught
// by the runtime alone, so a game library carries no C++ runtime. Defined in
// the core (ppc_runtime.cpp).
void resumable(Context &c, ResumableBody body, uint32_t lr, uint32_t sp, const uint32_t *sites, size_t count);
// A hooked function called while its slot holds the port's hooks (ppc_module.h,
// xenoforge/hooks.h): before, the replacement or the original body, after.
// Defined in the core (hooks.cpp).
void hooked(Context &c, HookSlot &slot);
// The port's mid-function hooks at a slot's address: 0 to run on, 1 to
// return, else the address to go on at. Defined in the core (hooks.cpp).
uint32_t mid_hook(Context &c, HookSlot &slot);
#endif

// ---- the standard library's pieces, freestanding ----

namespace lib {

template <class To, class From> XF_INLINE constexpr To bit_cast(const From &from) {
    return __builtin_bit_cast(To, from);
}
template <class T> XF_INLINE constexpr T rotl(T x, int s) {
    if constexpr (sizeof(T) == 8)
        return __builtin_rotateleft64(x, uint64_t(s));
    else if constexpr (sizeof(T) == 4)
        return __builtin_rotateleft32(x, uint32_t(s));
    else if constexpr (sizeof(T) == 2)
        return __builtin_rotateleft16(x, uint16_t(s));
    else
        return __builtin_rotateleft8(x, uint8_t(s));
}
template <class T> XF_INLINE constexpr int countl_zero(T x) { return __builtin_clzg(x, int(sizeof(T) * 8)); }
template <class T> XF_INLINE constexpr int countr_zero(T x) { return __builtin_ctzg(x, int(sizeof(T) * 8)); }
template <class T> XF_INLINE constexpr int popcount(T x) { return __builtin_popcountg(x); }

XF_INLINE void *memcpy(void *to, const void *from, size_t n) { return __builtin_memcpy(to, from, n); }
XF_INLINE void *memset(void *to, int value, size_t n) { return __builtin_memset(to, value, n); }
XF_INLINE void atomic_thread_fence_seq_cst() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
constexpr bool little_endian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;

// integer types' limits (the instructions use no other)
template <class T> struct numeric_limits {
    static constexpr bool is_signed = T(-1) < T(0);
    static constexpr T max() { return is_signed ? T(~(T(1) << (sizeof(T) * 8 - 1))) : T(~T(0)); }
    static constexpr T min() { return is_signed ? T(T(1) << (sizeof(T) * 8 - 1)) : T(0); }
};
template <bool B, class T, class F> struct conditional {
    using type = T;
};
template <class T, class F> struct conditional<false, T, F> {
    using type = F;
};
template <bool B, class T, class F> using conditional_t = typename conditional<B, T, F>::type;
template <class T> struct make_unsigned;
template <> struct make_unsigned<signed char> {
    using type = unsigned char;
};
template <> struct make_unsigned<unsigned char> {
    using type = unsigned char;
};
template <> struct make_unsigned<char> {
    using type = unsigned char;
};
template <> struct make_unsigned<short> {
    using type = unsigned short;
};
template <> struct make_unsigned<unsigned short> {
    using type = unsigned short;
};
template <> struct make_unsigned<int> {
    using type = unsigned int;
};
template <> struct make_unsigned<unsigned int> {
    using type = unsigned int;
};
template <> struct make_unsigned<long> {
    using type = unsigned long;
};
template <> struct make_unsigned<unsigned long> {
    using type = unsigned long;
};
template <> struct make_unsigned<long long> {
    using type = unsigned long long;
};
template <> struct make_unsigned<unsigned long long> {
    using type = unsigned long long;
};
template <class T> using make_unsigned_t = typename make_unsigned<T>::type;

// Whether the build's CPU level fuses a multiply-add in one instruction
// (x86-64-v3, ARM64; game_build_flags.h). Without it, lib::fma is the C
// library's (exact, slow): a call through the runtime's table in a game
// library, which has no C library.
#if defined(__FMA__) || defined(__aarch64__) || defined(_M_ARM64)
#define XF_HARDWARE_FMA 1
#else
#define XF_HARDWARE_FMA 0
#endif

// what compiles to an instruction (SSE4.1 for trunc: x86-64-v2 and above)
XF_INLINE double fabs(double v) { return __builtin_fabs(v); }
XF_INLINE float fabs(float v) { return __builtin_fabsf(v); }
template <class T> XF_INLINE bool isinf(T v) { return __builtin_isinf(v); }
template <class T> XF_INLINE bool signbit(T v) { return __builtin_signbit(v); }
XF_INLINE double copysign(double a, double b) { return __builtin_copysign(a, b); }
XF_INLINE double sqrt(double v) { return __builtin_sqrt(v); }
XF_INLINE float sqrt(float v) { return __builtin_sqrtf(v); }
#if XF_HARDWARE_FMA || !defined(XF_GAME_LIBRARY)
XF_INLINE double fma(double a, double b, double c) { return __builtin_fma(a, b, c); }
XF_INLINE float fma(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
#else
XF_INLINE double fma(double a, double b, double c) { return runtime.fma(a, b, c); }
XF_INLINE float fma(float a, float b, float c) { return runtime.fmaf(a, b, c); }
#endif
XF_INLINE double trunc(double v) { return __builtin_trunc(v); }

// what the C library does: the host's own, through the runtime's table in a
// game library
#ifdef XF_GAME_LIBRARY
XF_INLINE long long llrint(double v) { return runtime.llrint(v); }
XF_INLINE float exp2(float v) { return runtime.exp2f(v); }
XF_INLINE float log2(float v) { return runtime.log2f(v); }
XF_INLINE double fmod(double a, double b) { return runtime.fmod(a, b); }
#else
XF_INLINE long long llrint(double v) { return std::llrint(v); }
XF_INLINE float exp2(float v) { return std::exp2(v); }
XF_INLINE float log2(float v) { return std::log2(v); }
XF_INLINE double fmod(double a, double b) { return std::fmod(a, b); }
#endif

// The host's floating point environment as the slow path of the floating
// point instructions uses it (ppc_ops/fpu.h): a rounding mode (ROUND_*) set
// around an operation and the exception flags (EXCEPT_*) it raised. Where
// the host's control register is known it is read and written inline, and
// ROUND_* and EXCEPT_* are its own bits, so nothing is translated:
// - x86-64: MXCSR (rounding control in bits 13-14; flags in bits 0-5:
//   invalid, denormal operand, divide by zero, overflow, underflow,
//   precision);
// - ARM64: FPCR's RMode (bits 22-23) and FPSR's cumulative flags (bits 0-4:
//   invalid, divide by zero, overflow, underflow, inexact; bit 7, input
//   denormal).
// Elsewhere it is the C library's <cfenv> (through the runtime's table in a
// game library, numbered as Runtime says). The control register is read
// once, written only when the value changes, and nothing but the rounding
// mode and the flags is touched (DAZ/FTZ and the exception masks stay).
//
// The code that computes between these calls is compiled with
// `#pragma STDC FENV_ACCESS ON` (strict floating point): the register's
// reads and writes are operations with side effects that the compiler does
// not move strict operations across, as it does not move them across the C
// library's calls.
#if defined(__clang__) && (defined(__x86_64__) || defined(__aarch64__))
#define XF_FP_CONTROL_INLINE 1
#else
#define XF_FP_CONTROL_INLINE 0
#endif

#if XF_FP_CONTROL_INLINE && defined(__x86_64__)
enum : uint32_t { ROUND_NEAREST = 0u << 13, ROUND_DOWN = 1u << 13, ROUND_UP = 2u << 13, ROUND_ZERO = 3u << 13 };
enum : uint32_t {
    EXCEPT_INVALID = 1u << 0,
    EXCEPT_DIVBYZERO = 1u << 2,
    EXCEPT_OVERFLOW = 1u << 3,
    EXCEPT_UNDERFLOW = 1u << 4,
    EXCEPT_INEXACT = 1u << 5,
    EXCEPT_ALL = EXCEPT_INVALID | EXCEPT_DIVBYZERO | EXCEPT_OVERFLOW | EXCEPT_UNDERFLOW | EXCEPT_INEXACT,
};
struct FpEnv {
    // the rounding control; the flags, the denormal operand's among them
    static constexpr uint32_t ROUND_MASK = 3u << 13, FLAG_MASK = 0x3Fu;
    uint32_t entry, now; // MXCSR at the start, and as last read or written
    XF_INLINE FpEnv() : entry(__builtin_ia32_stmxcsr()), now(entry) {}
    XF_INLINE void write(uint32_t v) {
        if (v != now) __builtin_ia32_ldmxcsr(v), now = v;
    }
    // The rounding mode set and the flags cleared. Called right after the
    // constructor or flags(), when `now` is what the register holds (an
    // operation in between could have raised a flag `now` lacks).
    XF_INLINE void start(uint32_t mode) { write((now & ~(ROUND_MASK | FLAG_MASK)) | mode); }
    // the rounding mode set; the flags after it are unknown
    XF_INLINE void round(uint32_t mode) { write((now & ~ROUND_MASK) | mode); }
    // the flags raised since start()
    XF_INLINE uint32_t flags() {
        now = __builtin_ia32_stmxcsr();
        return now & EXCEPT_ALL;
    }
    // the rounding mode of the start put back (the flags left as they are)
    XF_INLINE void end() { round(entry & ROUND_MASK); }
};
#elif XF_FP_CONTROL_INLINE && defined(__aarch64__)
enum : uint32_t { ROUND_NEAREST = 0u << 22, ROUND_UP = 1u << 22, ROUND_DOWN = 2u << 22, ROUND_ZERO = 3u << 22 };
enum : uint32_t {
    EXCEPT_INVALID = 1u << 0,
    EXCEPT_DIVBYZERO = 1u << 1,
    EXCEPT_OVERFLOW = 1u << 2,
    EXCEPT_UNDERFLOW = 1u << 3,
    EXCEPT_INEXACT = 1u << 4,
    EXCEPT_ALL = 0x1Fu,
};
struct FpEnv {
    // FPCR's rounding mode; FPSR's flags, the input denormal's among them
    static constexpr uint64_t ROUND_MASK = 3u << 22, FLAG_MASK = 0x9Fu;
    uint64_t entry, now; // FPCR at the start, and as last written
    XF_INLINE FpEnv() : entry(__builtin_arm_rsr64("fpcr")), now(entry) {}
    XF_INLINE void start(uint32_t mode) {
        round(mode);
        __builtin_arm_wsr64("fpsr", __builtin_arm_rsr64("fpsr") & ~FLAG_MASK);
    }
    XF_INLINE void round(uint32_t mode) {
        const uint64_t v = (now & ~ROUND_MASK) | mode;
        if (v != now) __builtin_arm_wsr64("fpcr", v), now = v;
    }
    XF_INLINE uint32_t flags() { return uint32_t(__builtin_arm_rsr64("fpsr")) & EXCEPT_ALL; }
    XF_INLINE void end() { round(uint32_t(entry & ROUND_MASK)); }
};
#else
#ifdef XF_GAME_LIBRARY
enum : int { ROUND_NEAREST, ROUND_ZERO, ROUND_UP, ROUND_DOWN };
enum : int {
    EXCEPT_INEXACT = 1,
    EXCEPT_UNDERFLOW = 2,
    EXCEPT_OVERFLOW = 4,
    EXCEPT_DIVBYZERO = 8,
    EXCEPT_INVALID = 16,
    EXCEPT_ALL = 31,
};
XF_INLINE int fesetround(int mode) { return runtime.fesetround(mode); }
XF_INLINE int feclearexcept(int flags) { return runtime.feclearexcept(flags); }
XF_INLINE int fetestexcept(int flags) { return runtime.fetestexcept(flags); }
#else
enum : int { ROUND_NEAREST = FE_TONEAREST, ROUND_ZERO = FE_TOWARDZERO, ROUND_UP = FE_UPWARD, ROUND_DOWN = FE_DOWNWARD };
enum : int {
    EXCEPT_INEXACT = FE_INEXACT,
    EXCEPT_UNDERFLOW = FE_UNDERFLOW,
    EXCEPT_OVERFLOW = FE_OVERFLOW,
    EXCEPT_DIVBYZERO = FE_DIVBYZERO,
    EXCEPT_INVALID = FE_INVALID,
    EXCEPT_ALL = FE_ALL_EXCEPT,
};
XF_INLINE int fesetround(int mode) { return std::fesetround(mode); }
XF_INLINE int feclearexcept(int flags) { return std::feclearexcept(flags); }
XF_INLINE int fetestexcept(int flags) { return std::fetestexcept(flags); }
#endif
// the same through the C library, every call made (it keeps no state)
struct FpEnv {
    XF_INLINE void start(int mode) { fesetround(mode), feclearexcept(EXCEPT_ALL); }
    XF_INLINE void round(int mode) { fesetround(mode); }
    XF_INLINE int flags() { return fetestexcept(EXCEPT_ALL); }
    XF_INLINE void end() { fesetround(ROUND_NEAREST); }
};
#endif

} // namespace lib

} // namespace xf::ppc
