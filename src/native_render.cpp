// A port's own renderer in the console's place (xenoforge/native_render.h):
// the port's settings, the counters and the helpers that read the guest's
// data as the GPU does. The renderer takes the settings at each swap
// (renderer.cpp, "the port's frames").

#include "native_internal.h"

#include "xenoforge/xenos.h"

#include <bit>
#include <mutex>

namespace xf::native {

namespace {
std::mutex settings_mutex;
detail::Settings current;
std::atomic<uint64_t> settings_version{0};
bool enabled_value = true;
std::atomic<bool> active_value{false};
detail::AtomicCounters counter_values;

template <class F> void change(F &&f) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    f(current);
    settings_version.fetch_add(1, std::memory_order_release);
}
} // namespace

namespace detail {
uint64_t version() { return settings_version.load(std::memory_order_acquire); }
void settings(Settings &out) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    out = current;
}
AtomicCounters &atomic_counters() { return counter_values; }
void set_active(bool on) { active_value.store(on, std::memory_order_relaxed); }
} // namespace detail

void set_output_renderer(OutputRenderer renderer) {
    change([&](detail::Settings &s) { s.renderer = std::move(renderer); });
}
bool has_output_renderer() {
    std::lock_guard<std::mutex> lock(settings_mutex);
    return bool(current.renderer);
}
void set_output_size(uint32_t width, uint32_t height) {
    change([&](detail::Settings &s) { s.width = width, s.height = height; });
}
void set_pass_filter(PassFilter filter) {
    change([&](detail::Settings &s) { s.filter = std::move(filter); });
}
void set_draw_observer(DrawObserver observer) {
    change([&](detail::Settings &s) { s.observer = std::move(observer); });
}
void set_occlusion_samples(uint32_t samples) {
    change([&](detail::Settings &s) { s.occlusion_samples = samples; });
}
bool &enabled_flag() { return enabled_value; }
namespace {
std::atomic<bool> shadow_value{false};
}
void set_shadow(bool on) { shadow_value.store(on, std::memory_order_relaxed); }
bool shadow() { return shadow_value.load(std::memory_order_relaxed); }
bool active() { return active_value.load(std::memory_order_relaxed); }

Counters counters() {
    const detail::AtomicCounters &a = counter_values;
    Counters c;
    c.frames_native = a.frames_native.load(std::memory_order_relaxed);
    c.frames_yielded = a.frames_yielded.load(std::memory_order_relaxed);
    c.frames_held = a.frames_held.load(std::memory_order_relaxed);
    c.draws_skipped = a.draws_skipped.load(std::memory_order_relaxed);
    c.resolves_skipped = a.resolves_skipped.load(std::memory_order_relaxed);
    c.draws_kept = a.draws_kept.load(std::memory_order_relaxed);
    c.resolves_kept = a.resolves_kept.load(std::memory_order_relaxed);
    c.memexport_kept = a.memexport_kept.load(std::memory_order_relaxed);
    c.occlusion_faked = a.occlusion_faked.load(std::memory_order_relaxed);
    c.frames_shadowed = a.frames_shadowed.load(std::memory_order_relaxed);
    return c;
}

// Xenia's xe_gpu_vertex_fetch_t: type in bits 0-1 of the first word (3: a
// vertex buffer), the address in words above it; the endian swap in bits 0-1
// of the second, the size in words in bits 2-25.
VertexFetch vertex_fetch(const uint32_t *registers, uint32_t index) {
    VertexFetch f;
    if (index >= 96) return f;
    const uint32_t *w = registers + xenos::reg::FETCH_CONSTANTS + index * 2;
    f.valid = (w[0] & 3) == 3;
    f.address = w[0] & 0xFFFFFFFCu;
    f.endian = w[1] & 3;
    f.bytes = ((w[1] >> 2) & 0xFFFFFF) * 4;
    return f;
}

const uint32_t *texture_fetch(const uint32_t *registers, uint32_t index) {
    return registers + xenos::reg::FETCH_CONSTANTS + (index & 31) * 6;
}

void float_constant(const uint32_t *registers, uint32_t index, float out[4]) {
    const uint32_t *w = registers + xenos::reg::ALU_CONSTANTS + (index & 511) * 4;
    for (int i = 0; i < 4; i++) out[i] = std::bit_cast<float>(w[i]);
}
bool bool_constant(const uint32_t *registers, uint32_t index) {
    index &= 255;
    return (registers[xenos::reg::BOOL_CONSTANTS + index / 32] >> (index % 32)) & 1;
}
uint32_t loop_constant(const uint32_t *registers, uint32_t index) {
    return registers[xenos::reg::LOOP_CONSTANTS + (index & 31)];
}

void read_words(const uint8_t *physical, uint32_t address, uint32_t endian, uint32_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v;
        std::memcpy(&v, physical + ((address + i * 4) & 0x1FFFFFFFu), 4);
        out[i] = swap_word(v, endian);
    }
}
void read_floats(const uint8_t *physical, uint32_t address, uint32_t endian, float *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v;
        read_words(physical, uint32_t(address + i * 4), endian, &v, 1);
        out[i] = std::bit_cast<float>(v);
    }
}
void read_indices(const uint8_t *physical, uint32_t address, uint32_t endian, bool index32, uint32_t *out,
                  size_t count) {
    if (index32) return read_words(physical, address, endian, out, count);
    // 16-bit indices: swapped only by the 8-in-16 swap, as the renderer
    // reads them (renderer.cpp's indexed draws)
    for (size_t i = 0; i < count; i++) {
        uint16_t v;
        std::memcpy(&v, physical + ((address + i * 2) & 0x1FFFFFFFu), 2);
        out[i] = (endian & 3) == 1 ? __builtin_bswap16(v) : v;
    }
}

} // namespace xf::native
