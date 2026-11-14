// The renderer's side of xenoforge/native_render.h: the port's settings as
// the renderer takes them at a swap, and the counters it keeps.

#pragma once

#include "xenoforge/native_render.h"

#include <atomic>
#include <cstdint>

namespace xf::native::detail {

// What the port set, as one swap takes it.
struct Settings {
    OutputRenderer renderer;
    PassFilter filter;
    DrawObserver observer;
    uint32_t width = 0, height = 0;
    uint32_t occlusion_samples = 1000;
};

// Changes with every set_* call: the renderer copies the settings again only
// when it moved (one relaxed load a swap otherwise).
uint64_t version();
void settings(Settings &out);

struct AtomicCounters {
    std::atomic<uint64_t> frames_native{0}, frames_yielded{0}, frames_held{0};
    std::atomic<uint64_t> draws_skipped{0}, resolves_skipped{0}, draws_kept{0}, resolves_kept{0};
    std::atomic<uint64_t> memexport_kept{0}, occlusion_faked{0};
};
AtomicCounters &atomic_counters();
inline void bump(std::atomic<uint64_t> &n) { n.fetch_add(1, std::memory_order_relaxed); }
void set_active(bool on);

} // namespace xf::native::detail
