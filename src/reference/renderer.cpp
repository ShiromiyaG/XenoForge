/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The reference renderer as a sink (reference.h): its shaders, EDRAM, the
// resolves and the swaps; the draws are draw.cpp's. A resolve reads EDRAM's
// own bits as Xenia's resolve shaders do (resolve.xesli's XeResolveLoad*,
// through src/shaders/resolve.comp: the samples selected averaged in their
// order, 8_8_8_8_GAMMA through its linear codes) and writes guest memory
// through resolve.h's conversion, tiling and byte swaps; its clears write
// the rectangle's samples (edram_clear.comp). A swap's front buffer is the
// texture of fetch constant 0, through the display's gamma ramp, as the
// Vulkan renderer delivers it to match Xenia's trace dumper
// (renderer.cpp's deliver and display_gamma; Xenia's presenter). The trace
// is played as render::replay plays it (src/replay.cpp, from Xenia's
// trace_player.cc). BSD; see LICENSE-xenia.

#include "reference_impl.h"

#include "xenoforge/resolve.h"
#include "xenoforge/ucode.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xf::reference {

using xenos::reg::FETCH_CONSTANTS;

// ---- the pool ----

Pool::Pool(uint32_t threads) {
    for (uint32_t i = 1; i < threads; i++)
        workers_.emplace_back([this] {
            uint64_t seen = 0;
            for (;;) {
                {
                    std::unique_lock lock(mutex_);
                    start_.wait(lock, [&] { return stop_ || generation_ != seen; });
                    if (stop_) return;
                    seen = generation_;
                }
                work();
                std::lock_guard lock(mutex_);
                if (--pending_ == 0) done_.notify_all();
            }
        });
}

Pool::~Pool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    start_.notify_all();
    for (std::thread &t : workers_) t.join();
}

void Pool::work() {
    for (;;) {
        uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) return;
        (*job_)(i);
    }
}

void Pool::run(uint32_t count, const std::function<void(uint32_t)> &job) {
    if (!count) return;
    if (workers_.empty() || count == 1) {
        for (uint32_t i = 0; i < count; i++) job(i);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        job_ = &job;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        pending_ = uint32_t(workers_.size());
        generation_++;
    }
    start_.notify_all();
    work();
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return pending_ == 0; });
    job_ = nullptr;
}

// ---- EDRAM ----

uint32_t edram_address(uint32_t base, uint32_t pitch, uint32_t msaa, bool wide, bool depth, uint32_t x, uint32_t y,
                       uint32_t sample) {
    uint32_t u = x, v = y;
    if (msaa >= 2) {
        u = ((x >> 1) << 2) | (x & 1);
        v = ((y >> 1) << 2) | (y & 1);
    } else if (msaa == 1) {
        u = x & ~2u;
        v = ((y >> 1) << 2) | (y & 1) | (x & 2);
    }
    // 40x16-sample halves of 32bpp tiles (whole 64bpp tiles); a row of
    // tiles as xenos.h's GetSurfacePitchTiles gives it at 32 bits
    const uint32_t pitch_dwords = std::max(1u, (pitch * (msaa >= 2 ? 2 : 1) + 79) / 80) * TILE_DWORDS;
    uint32_t hx = u / 40, hy = v / 16, ix = u % 40, iy = v % 16;
    uint32_t row_in_tile = iy * 80, tile_row = pitch_dwords * hy;
    uint32_t first_half = tile_row + TILE_DWORDS * (hx >> 1) + row_in_tile + ix;
    bool second = hx & 1;
    uint32_t offset;
    if (depth)
        offset = first_half + (second ? 0u : 40u); // depth tiles have their halves swapped
    else if (wide)
        offset = (tile_row << 1) + TILE_DWORDS * hx + row_in_tile + (ix << 1);
    else
        offset = first_half + (second ? 40u : 0u);
    offset += (sample >> 1) * 160 + (sample & 1) * (wide ? 4 : 2);
    return uint32_t((uint64_t(base % EDRAM_TILES) * TILE_DWORDS + offset) % EDRAM_DWORDS);
}

// ---- the renderer ----

Renderer::Impl::Impl(Memory &m, gpu::CommandProcessor &p, const Options &o)
    : memory(m), guest(m.translate_physical(0)), processor(p), options(o), edram(EDRAM_DWORDS, 0), textures(guest),
      pool(o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency())) {}

void Renderer::Impl::load_shader(uint32_t type, std::span<const uint32_t> code) {
    if (type > 1) return;
    uint64_t hash = 0xcbf29ce484222325ull ^ type; // FNV-1a
    for (uint32_t d : code)
        for (int i = 0; i < 4; i++) hash = (hash ^ ((d >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    auto &s = shaders[hash];
    if (!s) {
        s = std::make_unique<Shader>();
        s->parsed = ucode::parse_shader(type == 0 ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel, code);
        s->exports = ucode::analyze(code).memexport;
        for (const ucode::ControlFlow &cf : s->parsed.control_flow) {
            const auto *e = std::get_if<ucode::Exec>(&cf.instruction);
            if (!e) continue;
            for (const ucode::ExecSlot &slot : e->slots) {
                const auto *a = std::get_if<ucode::AluInstruction>(&slot.instruction);
                if (!a) continue;
                for (const ucode::Result *res : {&a->vector_and_constant_result, &a->scalar_result}) {
                    uint32_t mask = res->used_write_mask();
                    if (!mask) continue;
                    switch (res->target) {
                    case ucode::StorageTarget::Color:
                        if (res->index < 4) s->colors |= 1u << res->index;
                        break;
                    case ucode::StorageTarget::Depth: s->depth = true; break;
                    case ucode::StorageTarget::PointSizeEdgeFlagKillVertex:
                        if (mask & 1) s->point_size = true;
                        if (mask & 4) s->vertex_kill = true;
                        break;
                    case ucode::StorageTarget::ExportData: s->exports = true; break;
                    default: break;
                    }
                }
            }
        }
    }
    (type == 0 ? vertex_shader : pixel_shader) = s.get();
}

void Renderer::Impl::export_words(uint32_t address, const uint32_t *words, const uint32_t *masks, uint32_t count) {
    std::lock_guard lock(memory_mutex);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = (address + 4 * i) & GuestMemory::PHYSICAL_MASK & ~3u;
        uint8_t *p = guest.data() + at;
        uint32_t old;
        std::memcpy(&old, p, 4);
        uint32_t value = (old & ~masks[i]) | (words[i] & masks[i]);
        std::memcpy(p, &value, 4);
    }
    report.exports++;
}

// A resolve: the rectangle's samples from EDRAM to guest memory, then the
// clears over the same rectangle.
void Renderer::Impl::resolve(const uint32_t *regs) {
    report.resolves++;
    resolve::Info info;
    std::string why;
    if (!resolve::get_info({regs, xenos::REGISTER_COUNT}, guest, info, why)) return skip("resolve: " + why);
    if (!info.copy && !why.empty()) approximate("resolve not copied: " + why);
    uint32_t pitch = regs[xenos::reg::RB_SURFACE_INFO] & 0x3FFF;
    uint32_t surface_pitch = info.surface_pitch ? info.surface_pitch : pitch;
    // the source's rows within EDRAM from its base (the Vulkan renderer's edram_view)
    auto view_size = [&](bool depth, uint32_t base, uint32_t format, uint32_t msaa, uint32_t &width, uint32_t &height) {
        bool wide = !depth && (format == 5 || format == 7 || format == 15);
        uint32_t samples_x = msaa >= 2 ? 2 : 1, samples_y = msaa >= 1 ? 2 : 1;
        uint32_t pitch_tiles = std::max(1u, (surface_pitch * samples_x + 79) / 80) << (wide ? 1 : 0);
        uint32_t rows = std::max(1u, (EDRAM_TILES - std::min(base, EDRAM_TILES - 1)) / pitch_tiles);
        width = std::clamp(surface_pitch, 1u, 8192u);
        height = std::clamp(rows * (16 / samples_y), 1u, 8192u);
    };
    uint32_t sw, sh;
    view_size(info.depth, info.edram_base, info.source_format, info.msaa, sw, sh);
    uint32_t x = std::min(info.x, sw), y = std::min(info.y, sh);
    uint32_t w = std::min(info.width, sw - x), h = std::min(info.height, sh - y);
    if (options.verbose)
        std::fprintf(stderr,
                     "  ref resolve: %s base %u (%u,%u %ux%u) -> %08X format %u from %u (msaa %u, samples %u)\n",
                     info.depth ? "depth" : "color", info.edram_base, x, y, w, h, info.dest_base, info.dest_format,
                     info.source_format, info.msaa, info.sample_select);
    if (info.copy && w && h) {
        resolve::Info clipped = info;
        clipped.x = x, clipped.y = y, clipped.width = w, clipped.height = h;
        const uint32_t format = info.source_format;
        const bool wide = !info.depth && (format == 5 || format == 7 || format == 15);
        const bool gamma = !info.depth && format == 1;
        const uint32_t first = resolve::first_sample(info.sample_select),
                       count = resolve::sample_count(info.sample_select);
        const float factor = std::ldexp(1.0f, info.dest_exp_bias);
        // resolve.comp's XePWLGammaToLinearCode and XeResolveAveragePWLGamma
        auto linear_code = [](uint32_t c) {
            uint32_t scale = c < 64 ? 1 : c < 96 ? 2 : c < 192 ? 4 : 8;
            uint32_t offset = c < 64 ? 0 : c < 96 ? 64 : c < 192 ? 256 : 1024;
            uint32_t l = c * scale - offset;
            return l + ((l * scale) >> 10);
        };
        auto average_gamma = [&](float sum, float weight) {
            float averaged = sum * weight;
            uint32_t m = uint32_t(averaged);
            uint32_t c = m < 64 ? m : m < 128 ? (m + 64) >> 1 : m < 512 ? (m + 256) >> 2 : (m + 1024) >> 3;
            c = std::min(c, 255u);
            uint32_t n = linear_code(c);
            return n == 1023 ? 1.0f : float((n << 14) | (n << 4) | (n >> 6)) * (1.0f / 16777216.0f);
        };
        std::vector<resolve::LoadedPixel> pixels(size_t(w) * h);
        for (uint32_t j = 0; j < h; j++)
            for (uint32_t i = 0; i < w; i++) {
                resolve::LoadedPixel &px = pixels[size_t(j) * w + i];
                float sum[4] = {};
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t at = edram_address(info.edram_base, surface_pitch, info.msaa, wide, info.depth, x + i,
                                                y + j, first + k);
                    uint32_t words[2] = {edram[at], wide ? edram[(at + 1) % EDRAM_DWORDS] : 0u};
                    if (k == 0) px.words[0] = words[0], px.words[1] = words[1];
                    if (info.depth) continue;
                    float c[4];
                    // XeResolveUnpack*: the formats' components, two-component
                    // ones with blue and alpha 0, one-component ones the rest 0
                    if (format == 4 || format == 6 || format == 14 || format == 15) {
                        float u[4];
                        pieces::unpack_color(words, format, u);
                        c[0] = u[0], c[1] = format == 14 ? 0.0f : u[1], c[2] = 0.0f, c[3] = 0.0f;
                    } else {
                        pieces::unpack_color(words, gamma ? 0u : format, c);
                    }
                    if (gamma)
                        for (int q = 0; q < 3; q++) c[q] = float(linear_code(uint32_t(c[q] * 255.0f + 0.5f)));
                    for (int q = 0; q < 4; q++) sum[q] = k == 0 ? c[q] : sum[q] + c[q];
                }
                float weight = count == 4 ? 0.25f : count == 2 ? 0.5f : 1.0f;
                if (gamma) {
                    for (int q = 0; q < 3; q++) px.loaded[q] = average_gamma(sum[q], weight) * factor;
                    px.loaded[3] = sum[3] * (factor * weight);
                } else {
                    float bias = factor * weight;
                    for (int q = 0; q < 4; q++) px.loaded[q] = sum[q] * bias;
                }
            }
        if (!resolve::write_loaded(clipped, pixels, guest, why)) {
            skip("resolve: " + why);
        } else {
            // what was written: the textures over it are read again
            std::vector<std::pair<uint32_t, uint32_t>> written, partial;
            if (resolve::destination_ranges(clipped, written, partial))
                for (auto [s, e] : written) textures.invalidate(s, e - s);
            else
                textures.clear();
        }
    }
    // the clears, after the copy, over the same rectangle (RB_DEPTH_CLEAR,
    // or RB_COLOR_CLEAR with RB_COLOR_CLEAR_LO first at 64 bits per sample)
    if ((info.clear_color || info.clear_depth) && w && h) {
        uint32_t msaa = (regs[xenos::reg::RB_SURFACE_INFO] >> 16) & 3, samples = 1u << msaa;
        auto clear = [&](bool depth, uint32_t target_info, uint32_t lo, uint32_t hi) {
            uint32_t base = target_info & 0xFFF, format = depth ? (target_info >> 16) & 1 : (target_info >> 16) & 0xF;
            bool wide = !depth && (format == 5 || format == 7 || format == 15);
            uint32_t tw, th;
            view_size(depth, base, format, msaa, tw, th);
            uint32_t cw = std::min(w, tw - std::min(x, tw)), ch = std::min(h, th - std::min(y, th));
            for (uint32_t j = 0; j < ch; j++)
                for (uint32_t i = 0; i < cw; i++)
                    for (uint32_t s = 0; s < samples; s++) {
                        uint32_t at = edram_address(base, surface_pitch, msaa, wide, depth, x + i, y + j, s);
                        edram[at] = wide ? lo : hi;
                        if (wide) edram[(at + 1) % EDRAM_DWORDS] = hi;
                    }
        };
        if (info.clear_depth)
            clear(true, regs[xenos::reg::RB_DEPTH_INFO], regs[r::RB_DEPTH_CLEAR], regs[r::RB_DEPTH_CLEAR]);
        if (info.clear_color)
            clear(false, regs[r::COLOR_INFO[info.color_target & 3]], regs[r::RB_COLOR_CLEAR_LO],
                  regs[r::RB_COLOR_CLEAR]);
    }
}

namespace {

// renderer.cpp's display_gamma: DC_LUTA_CONTROL's 256-entry table, or the
// piecewise linear ramp stepped by the blue field's increment
uint32_t display_gamma(uint32_t value, uint32_t bits, uint32_t channel, uint32_t luta_control,
                       const std::array<uint32_t, 256> &table, const std::array<uint32_t, 128 * 3> &pwl) {
    uint32_t increment = luta_control & 0xF;
    if (!increment) return (table[(value >> (bits - 8)) & 0xFF] >> (channel == 0 ? 20 : channel == 1 ? 10 : 0)) & 0x3FF;
    increment = std::min(increment, 9u);
    uint32_t entry = pwl[((value >> increment) & 0x7F) * 3 + channel];
    float out =
        (float(entry & 0xFFFF) + float((value & ((1u << increment) - 1)) * (entry >> 16)) / float(1u << increment)) *
        (1.0f / (64.0f * 1023.0f));
    return uint32_t(std::clamp(out, 0.0f, 1.0f) * 1023.0f + 0.5f);
}

} // namespace

// A swap: the front buffer (fetch constant 0's texture), as the display
// shows it: the fetch constant's swizzle, opaque, through its gamma ramp.
void Renderer::Impl::swap(const uint32_t *regs) {
    uint64_t number = report.swaps++;
    const uint32_t *fetch = &regs[FETCH_CONSTANTS];
    texture::Info info = texture::parse_fetch_constant(fetch);
    if ((fetch[0] & 3) != 2 || info.dimension != 1 || !info.base_address) {
        approximate("a swap without a 2D texture in fetch constant 0");
        return;
    }
    texture::Plan plan;
    texture::Image image;
    std::string why;
    if (!texture::plan(info, plan, why) || !texture::decode(info, guest, image, why)) {
        approximate("swap texture not decoded: " + why);
        return;
    }
    std::vector<uint8_t> rgba = texture::to_rgba8(image, 0);
    size_t pixels = size_t(image.width) * image.height;
    if (rgba.size() < pixels * 4) {
        approximate("swap texture not decoded: no base level");
        return;
    }
    const uint32_t swizzle = texture::compose_swizzle(info.swizzle, plan.host_swizzle);
    Image frame;
    frame.width = image.width, frame.height = image.height;
    frame.rgba.resize(pixels * 4);
    for (size_t i = 0; i < pixels; i++)
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t guest_c = (swizzle >> (3 * c)) & 7;
            uint8_t value = guest_c == 5 ? 255 : guest_c >= 4 ? 0 : rgba[i * 4 + guest_c];
            frame.rgba[i * 4 + c] = c == 3 ? 255 : value;
        }
    const uint32_t luta = regs[r::DC_LUTA_CONTROL];
    const bool ten_bits = info.format == 7 || info.format == 54;
    const auto &table = processor.gamma_ramp_table();
    const auto &pwl = processor.gamma_ramp_pwl();
    if (ten_bits && image.format != texture::HostFormat::RGB10A2) {
        approximate("the display's gamma ramp (front buffer not 10 bits a component)");
    } else {
        for (size_t i = 0; i < pixels; i++) {
            uint32_t values[3] = {};
            if (ten_bits) {
                uint32_t word;
                std::memcpy(&word, image.levels[0].data() + 4 * i, 4);
                const uint32_t texel[4] = {word & 0x3FF, (word >> 10) & 0x3FF, (word >> 20) & 0x3FF,
                                           (word >> 30) * 341};
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t g = (swizzle >> (3 * c)) & 7;
                    values[c] = g == 5 ? 1023 : g >= 4 ? 0 : texel[g];
                }
            } else {
                for (uint32_t c = 0; c < 3; c++) values[c] = frame.rgba[i * 4 + c];
            }
            for (uint32_t c = 0; c < 3; c++) {
                uint32_t ten = display_gamma(values[c], ten_bits ? 10 : 8, c, luta, table, pwl);
                frame.rgba[i * 4 + c] =
                    ten_bits ? uint8_t(float(ten) * (255.0f / 1023.0f) + 0.5f) : uint8_t((ten * 255 + 511) / 1023);
            }
        }
    }
    report.frames++;
    if (on_frame && *on_frame) (*on_frame)(number, frame);
}

Renderer::Renderer(Memory &memory, gpu::CommandProcessor &processor, const Options &options)
    : impl_(std::make_unique<Impl>(memory, processor, options)) {
    impl_->on_frame = &on_frame;
}
Renderer::~Renderer() = default;

void Renderer::shader(uint32_t type, uint32_t, std::span<const uint32_t> code) { impl_->load_shader(type, code); }
void Renderer::draw(const gpu::RegisterFile &registers, const gpu::Draw &draw) { impl_->draw(registers.data(), draw); }
void Renderer::resolve(const gpu::RegisterFile &registers, const gpu::Draw &) { impl_->resolve(registers.data()); }
void Renderer::swap(const gpu::RegisterFile &registers, const gpu::Swap &) { impl_->swap(registers.data()); }

void Renderer::restore_edram(std::span<const uint8_t> bytes) {
    std::memcpy(impl_->edram.data(), bytes.data(), std::min<size_t>(bytes.size(), impl_->edram.size() * 4));
}
void Renderer::memory_written(uint32_t address, uint32_t bytes) { impl_->textures.invalidate(address, bytes); }

const Report &Renderer::report() const {
    impl_->counts.take(impl_->report.approximate);
    return impl_->report;
}
std::span<const uint32_t> Renderer::edram() const { return impl_->edram; }

namespace {

// What the processor asks of the system during a replay: nothing happens
// between two polls, so a wait that does not match never will.
class TraceHost final : public gpu::CommandProcessor::Host {
  public:
    void interrupt(uint32_t) override {}
    bool wait(uint32_t) override { return false; }
};

class Player final : public TraceVisitor {
  public:
    Player(Memory &memory, gpu::CommandProcessor &processor, Renderer &renderer)
        : memory_(memory), processor_(processor), renderer_(renderer) {}
    void packet(uint32_t address, std::span<const uint8_t> words) override {
        uint32_t count = uint32_t(words.size() / 4);
        if (!count) return;
        write(address, words);
        processor_.execute_packets(address, count);
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override {
        write(address, bytes);
        renderer_.memory_written(address & 0x1FFFFFFF, uint32_t(bytes.size()));
    }
    void registers(uint32_t first, std::span<const uint32_t> values) override {
        processor_.restore_registers(first, values);
    }
    void edram_snapshot(std::span<const uint8_t> bytes) override { renderer_.restore_edram(bytes); }
    void gamma_ramp(std::span<const uint32_t> table, std::span<const uint32_t> pwl) override {
        processor_.restore_gamma_ramp(table, pwl);
    }

  private:
    void write(uint32_t address, std::span<const uint8_t> bytes) {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t at = (address + uint32_t(i)) & 0x1FFFFFFF;
            size_t n = std::min<size_t>(0x20000000 - at, bytes.size() - i);
            std::memcpy(memory_.translate_physical(at), bytes.data() + i, n);
            i += n;
        }
    }
    Memory &memory_;
    gpu::CommandProcessor &processor_;
    Renderer &renderer_;
};

} // namespace

bool replay(const GpuTrace &trace, const Options &options, Image &frame, Report &report, std::string &error) {
    std::unique_ptr<Memory> memory = Memory::create(error);
    if (!memory) return false;
    TraceHost host;
    gpu::CommandProcessor processor(*memory, host);
    Renderer renderer(*memory, processor, options);
    renderer.on_frame = [&](uint64_t, const Image &image) { frame = image; };
    processor.set_sink(&renderer);
    Player player(*memory, processor, renderer);
    bool walked = walk_gpu_trace(trace, player, error);
    processor.set_sink(nullptr);
    report = renderer.report();
    // XF_REF_DUMP_MEMORY=<hex address>:<hex size>:<file>: that physical memory
    // after the trace, resolves included (to set beside Xenia's, docs/renderer.md)
    if (const char *dump = std::getenv("XF_REF_DUMP_MEMORY")) {
        unsigned address = 0, size = 0;
        char path[512] = {};
        if (std::sscanf(dump, "%x:%x:%511[^\n]", &address, &size, path) == 3)
            if (FILE *f = std::fopen(path, "wb")) {
                std::fwrite(memory->translate_physical(address), 1, size, f);
                std::fclose(f);
            }
    }
    return walked;
}

} // namespace xf::reference
