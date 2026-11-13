// The renderer's recording thread (on by default, XF_RECORD_RING=0 off; docs/renderer.md,
// "The recording on a thread of its own").
//
// The renderer records through command buffer handles of its own (Fake) in
// place of the Vulkan ones. Every vkCmd* function the renderer calls is
// replaced, in renderer.cpp alone, by a function of the same signature here
// (the macros at the end): given a real command buffer it calls the driver at
// once; given a fake one it copies its arguments (and the arrays they point
// to) into a ring of bytes, which one thread reads and records into the real
// command buffers in order, submissions included. The thread that makes the
// commands then never calls the driver to record: only where it must see the
// GPU's work (a fence's wait, a presenter's submission after the frame's, a
// port recording into the command buffer itself) does it wait for the ring to
// be recorded, and those waits are counted.
//
// A command in the ring is a header (the function that records it, its size,
// which command buffer) and a trivially copyable lambda holding its arguments;
// arrays go before it in entries the thread skips. The thread spins a little
// when the ring is empty, then sleeps until more is handed over (publish).
#pragma once

#include "fast_clock.h"

#include <volk.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace xf::rec {

class Ring;

// A command buffer handle of the ring's: what it names is a ring and a target (one of the
// renderer's real command buffers). Taken from one table for the process, so a handle is told
// from a real one by its address alone.
struct Fake {
    Ring *ring = nullptr;
    uint32_t target = 0;
};
inline constexpr uint32_t FAKES = 256;
inline Fake g_fakes[FAKES];
inline std::mutex g_fakes_mutex;
inline bool g_fakes_taken[FAKES] = {};

inline Fake *fake_of(VkCommandBuffer c) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(c), base = reinterpret_cast<uintptr_t>(&g_fakes[0]);
    return p - base < sizeof(g_fakes) ? reinterpret_cast<Fake *>(c) : nullptr;
}

inline void spin_pause() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

class Ring {
  public:
    // records an entry: its payload, and the real command buffer of its target (none for NONE)
    using Run = void (*)(const void *payload, VkCommandBuffer real);
    struct alignas(16) Header {
        Run run;        // null: skipped (arrays, the ring's end)
        uint32_t bytes; // with the header, a multiple of 16
        uint32_t target;
    };
    static_assert(sizeof(Header) == 16);
    static constexpr uint32_t NONE = ~0u, QUIT = ~0u - 1, TARGETS = 16;

    // Why the producing thread waited for the recording thread (wait())
    enum Why : uint32_t { Fence, Present, Direct, Full, Final, WHYS };

    explicit Ring(size_t bytes = size_t(16) << 20) : size_(bytes), data_(new (std::align_val_t(64)) uint8_t[bytes]) {}
    ~Ring() {
        stop();
        std::lock_guard<std::mutex> lock(g_fakes_mutex);
        for (uint32_t i = 0; i < FAKES; i++)
            if (g_fakes[i].ring == this) g_fakes[i] = {}, g_fakes_taken[i] = false;
        ::operator delete[](data_, std::align_val_t(64));
    }
    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    // a handle for a real command buffer (target < TARGETS); null when the table is full
    VkCommandBuffer fake(uint32_t target, VkCommandBuffer real) {
        real_[target] = real;
        std::lock_guard<std::mutex> lock(g_fakes_mutex);
        for (uint32_t i = 0; i < FAKES; i++)
            if (!g_fakes_taken[i]) {
                g_fakes_taken[i] = true;
                g_fakes[i] = {this, target};
                return reinterpret_cast<VkCommandBuffer>(&g_fakes[i]);
            }
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer real(uint32_t target) const { return real_[target]; }

    // Direct: the ring recorded, then every command recorded at once by the caller, on the
    // real command buffers (a port recording into them itself)
    bool direct() const { return direct_; }
    void set_direct(bool on) {
        if (on) wait(Direct);
        direct_ = on;
    }

    // room for an entry of that many payload bytes; commit() puts it in
    void *reserve(uint32_t target, size_t payload, Run run) {
        const uint32_t total = uint32_t((sizeof(Header) + payload + 15) & ~size_t(15));
        size_t at = head_ % size_;
        if (at + total > size_) {
            // (never across the ring's end: the rest of it skipped)
            const uint32_t rest = uint32_t(size_ - at);
            room(rest);
            Header *h = reinterpret_cast<Header *>(data_ + at);
            h->run = nullptr, h->bytes = rest, h->target = NONE;
            head_ += rest;
            at = 0;
        }
        room(total);
        Header *h = reinterpret_cast<Header *>(data_ + at);
        h->run = run, h->bytes = total, h->target = target;
        pending_ = total;
        return h + 1;
    }
    void commit() {
        head_ += pending_;
        entries_++;
        // (handed over by the 256 KB at least when nothing else does)
        if (head_ - published_ >= (size_t(256) << 10)) publish();
    }
    template <class F> static void run_of(const void *payload, VkCommandBuffer real) {
        (*static_cast<const F *>(payload))(real);
    }
    // a command: f(real command buffer), recorded by the thread in order
    template <class F> void put(uint32_t target, const F &f) {
        static_assert(std::is_trivially_copyable_v<F> && alignof(F) <= 16);
        new (reserve(target, sizeof(F), &run_of<F>)) F(f);
        commit();
    }
    // an array's copy in the ring, for the next command (nothing for none)
    template <class T> T *copy(const T *from, uint32_t count) {
        if (!from || !count) return nullptr;
        void *to = reserve(NONE, sizeof(T) * count, nullptr);
        std::memcpy(to, from, sizeof(T) * count);
        commit();
        return static_cast<T *>(to);
    }

    // Handed over where that many bytes were put since the last time: the thread then records
    // in bursts and sleeps between them, rather than being woken at every draw (a system call
    // each, which cost the draws more than the driver's time they saved). The GPU sees nothing
    // before the submission anyway; the waits that need the ring recorded hand it over first.
    // XF_RECORD_RING_BATCH=<bytes> (16 KB by default; 0: at every draw).
    void publish_some() {
        if (head_ - published_ >= batch_) publish();
    }
    // the entries put so far handed to the thread (started at the first)
    void publish() {
        if (head_ == published_) return;
        if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
        published_ = head_;
        head_shared_.store(head_, std::memory_order_seq_cst);
        if (asleep_.load(std::memory_order_seq_cst)) head_shared_.notify_one();
        publishes_++;
    }
    // everything put recorded (counted by why, with the time it took)
    void wait(Why why) {
        publish();
        if (tail_seen_ == head_) return;
        const uint64_t t0 = FastClock::now();
        uint32_t spins = 0;
        while ((tail_seen_ = tail_shared_.load(std::memory_order_acquire)) != head_)
            if (++spins < 4096)
                spin_pause();
            else
                std::this_thread::yield();
        waits_[why]++;
        wait_ticks_[why] += FastClock::now() - t0;
    }
    // waits for a value the thread stores (a submission's serial), counted as wait()'s
    void wait_for(const std::atomic<uint64_t> &value, uint64_t at_least, Why why) {
        if (value.load(std::memory_order_acquire) >= at_least) return;
        publish();
        const uint64_t t0 = FastClock::now();
        uint32_t spins = 0;
        while (value.load(std::memory_order_acquire) < at_least)
            if (++spins < 4096)
                spin_pause();
            else
                std::this_thread::yield();
        waits_[why]++;
        wait_ticks_[why] += FastClock::now() - t0;
    }
    // the thread stopped, everything put recorded
    void stop() {
        if (!thread_.joinable()) return;
        reserve(QUIT, 0, nullptr);
        commit();
        publish();
        thread_.join();
    }

    // the counts, as one line
    void print(const char *what) const {
        static const char *const names[WHYS] = {"fences", "presenter", "port's frames", "ring full", "the end"};
        std::fprintf(stderr, "%s: %llu commands, %.1f MB, %llu hand-overs, %llu sleeps; waits:", what,
                     (unsigned long long)entries_, double(head_) / 1048576.0, (unsigned long long)publishes_,
                     (unsigned long long)sleeps_.load());
        for (uint32_t i = 0; i < WHYS; i++)
            if (waits_[i])
                std::fprintf(stderr, " %s %llu (%.3f ms)", names[i], (unsigned long long)waits_[i],
                             FastClock::seconds(wait_ticks_[i]) * 1000.0);
        std::fprintf(stderr, "\n");
    }
    uint64_t waits(Why why) const { return waits_[why]; }
    uint64_t wait_ticks(Why why) const { return wait_ticks_[why]; }

  private:
    // room for that many bytes at the head, the thread waited for where the ring is full
    void room(uint32_t bytes) {
        if (head_ + bytes - tail_seen_ <= size_) return;
        // (the thread's count read again first: only a ring still full is waited for)
        if (head_ + bytes - (tail_seen_ = tail_shared_.load(std::memory_order_acquire)) <= size_) return;
        publish();
        const uint64_t t0 = FastClock::now();
        while (head_ + bytes - (tail_seen_ = tail_shared_.load(std::memory_order_acquire)) > size_) spin_pause();
        waits_[Full]++;
        wait_ticks_[Full] += FastClock::now() - t0;
    }
    void loop() {
        size_t tail = 0;
        for (;;) {
            size_t head = head_shared_.load(std::memory_order_acquire);
            if (head == tail) {
                for (uint32_t spins = 0; spins < 2000 && head == tail; spins++) {
                    spin_pause();
                    head = head_shared_.load(std::memory_order_acquire);
                }
                if (head == tail) {
                    asleep_.store(true, std::memory_order_seq_cst);
                    sleeps_.fetch_add(1, std::memory_order_relaxed);
                    head_shared_.wait(tail, std::memory_order_seq_cst);
                    asleep_.store(false, std::memory_order_relaxed);
                    continue;
                }
            }
            while (tail != head) {
                const Header *h = reinterpret_cast<const Header *>(data_ + tail % size_);
                if (h->target == QUIT) {
                    tail_shared_.store(tail + h->bytes, std::memory_order_release);
                    return;
                }
                if (h->run) h->run(h + 1, h->target == NONE ? VK_NULL_HANDLE : real_[h->target]);
                tail += h->bytes;
            }
            tail_shared_.store(tail, std::memory_order_release);
        }
    }

    const size_t size_;
    uint8_t *const data_;
    VkCommandBuffer real_[TARGETS] = {};
    bool direct_ = false;
    const size_t batch_ = [] {
        const char *v = std::getenv("XF_RECORD_RING_BATCH");
        return v ? size_t(std::strtoull(v, nullptr, 10)) : size_t(16) << 10;
    }();
    // the producer's
    size_t head_ = 0, published_ = 0, tail_seen_ = 0;
    uint32_t pending_ = 0;
    uint64_t entries_ = 0, publishes_ = 0, waits_[WHYS] = {}, wait_ticks_[WHYS] = {};
    std::thread thread_;
    // shared, each on its own line
    alignas(64) std::atomic<size_t> head_shared_{0};
    alignas(64) std::atomic<size_t> tail_shared_{0};
    alignas(64) std::atomic<bool> asleep_{false};
    std::atomic<uint64_t> sleeps_{0};
};

// ---- the vkCmd* functions, each of the driver's at once on a real command buffer, into the
// ring on a fake one ----

// the command's real command buffer where it is recorded at once, else null (put in the ring)
#define XF_REC_ENTER(c, ...)                                                                                           \
    if (Fake *f_ = fake_of(c)) {                                                                                       \
        if (!f_->ring->direct()) {                                                                                     \
            Ring &ring_ = *f_->ring;                                                                                   \
            const uint32_t target_ = f_->target;                                                                       \
            __VA_ARGS__;                                                                                               \
            return;                                                                                                    \
        }                                                                                                              \
        c = f_->ring->real(f_->target);                                                                                \
    }
#define XF_REC_PUT(...) ring_.put(target_, [=](VkCommandBuffer r_) { __VA_ARGS__; })

inline VkResult begin_command_buffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *info) {
    if (Fake *f = fake_of(c); f && !f->ring->direct()) {
        VkCommandBufferBeginInfo copy = *info;
        copy.pNext = nullptr, copy.pInheritanceInfo = nullptr;
        f->ring->put(f->target, [=](VkCommandBuffer r) { ::vkBeginCommandBuffer(r, &copy); });
        return VK_SUCCESS;
    } else if (f) {
        c = f->ring->real(f->target);
    }
    return ::vkBeginCommandBuffer(c, info);
}
inline VkResult end_command_buffer(VkCommandBuffer c) {
    if (Fake *f = fake_of(c); f && !f->ring->direct()) {
        f->ring->put(f->target, [=](VkCommandBuffer r) { ::vkEndCommandBuffer(r); });
        return VK_SUCCESS;
    } else if (f) {
        c = f->ring->real(f->target);
    }
    return ::vkEndCommandBuffer(c);
}
inline void cmd_bind_pipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline p) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdBindPipeline(r_, bp, p)));
    ::vkCmdBindPipeline(c, bp, p);
}
inline void cmd_bind_descriptor_sets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                     uint32_t count, const VkDescriptorSet *sets, uint32_t offset_count,
                                     const uint32_t *offsets) {
    XF_REC_ENTER(c, const VkDescriptorSet *s = ring_.copy(sets, count);
                 const uint32_t *o = ring_.copy(offsets, offset_count);
                 XF_REC_PUT(::vkCmdBindDescriptorSets(r_, bp, layout, first, count, s, offset_count, o)));
    ::vkCmdBindDescriptorSets(c, bp, layout, first, count, sets, offset_count, offsets);
}
inline void cmd_bind_index_buffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize at, VkIndexType type) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdBindIndexBuffer(r_, b, at, type)));
    ::vkCmdBindIndexBuffer(c, b, at, type);
}
inline void cmd_bind_vertex_buffers(VkCommandBuffer c, uint32_t first, uint32_t count, const VkBuffer *buffers,
                                    const VkDeviceSize *offsets) {
    XF_REC_ENTER(c, const VkBuffer *b = ring_.copy(buffers, count); const VkDeviceSize *o = ring_.copy(offsets, count);
                 XF_REC_PUT(::vkCmdBindVertexBuffers(r_, first, count, b, o)));
    ::vkCmdBindVertexBuffers(c, first, count, buffers, offsets);
}
inline void cmd_set_viewport(VkCommandBuffer c, uint32_t first, uint32_t count, const VkViewport *v) {
    XF_REC_ENTER(c, const VkViewport *p = ring_.copy(v, count); XF_REC_PUT(::vkCmdSetViewport(r_, first, count, p)));
    ::vkCmdSetViewport(c, first, count, v);
}
inline void cmd_set_scissor(VkCommandBuffer c, uint32_t first, uint32_t count, const VkRect2D *s) {
    XF_REC_ENTER(c, const VkRect2D *p = ring_.copy(s, count); XF_REC_PUT(::vkCmdSetScissor(r_, first, count, p)));
    ::vkCmdSetScissor(c, first, count, s);
}
inline void cmd_set_blend_constants(VkCommandBuffer c, const float constants[4]) {
    struct Four {
        float v[4];
    } four;
    std::memcpy(four.v, constants, sizeof(four.v));
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetBlendConstants(r_, four.v)));
    ::vkCmdSetBlendConstants(c, constants);
}
inline void cmd_set_stencil_reference(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t v) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetStencilReference(r_, faces, v)));
    ::vkCmdSetStencilReference(c, faces, v);
}
inline void cmd_set_stencil_compare_mask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t v) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetStencilCompareMask(r_, faces, v)));
    ::vkCmdSetStencilCompareMask(c, faces, v);
}
inline void cmd_set_stencil_write_mask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t v) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetStencilWriteMask(r_, faces, v)));
    ::vkCmdSetStencilWriteMask(c, faces, v);
}
inline void cmd_set_primitive_topology(VkCommandBuffer c, VkPrimitiveTopology t) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetPrimitiveTopology(r_, t)));
    ::vkCmdSetPrimitiveTopology(c, t);
}
inline void cmd_set_primitive_restart_enable(VkCommandBuffer c, VkBool32 on) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetPrimitiveRestartEnable(r_, on)));
    ::vkCmdSetPrimitiveRestartEnable(c, on);
}
inline void cmd_set_cull_mode(VkCommandBuffer c, VkCullModeFlags m) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetCullMode(r_, m)));
    ::vkCmdSetCullMode(c, m);
}
inline void cmd_set_front_face(VkCommandBuffer c, VkFrontFace f) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetFrontFace(r_, f)));
    ::vkCmdSetFrontFace(c, f);
}
inline void cmd_set_depth_test_enable(VkCommandBuffer c, VkBool32 on) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetDepthTestEnable(r_, on)));
    ::vkCmdSetDepthTestEnable(c, on);
}
inline void cmd_set_depth_write_enable(VkCommandBuffer c, VkBool32 on) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetDepthWriteEnable(r_, on)));
    ::vkCmdSetDepthWriteEnable(c, on);
}
inline void cmd_set_depth_compare_op(VkCommandBuffer c, VkCompareOp op) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetDepthCompareOp(r_, op)));
    ::vkCmdSetDepthCompareOp(c, op);
}
inline void cmd_set_stencil_test_enable(VkCommandBuffer c, VkBool32 on) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetStencilTestEnable(r_, on)));
    ::vkCmdSetStencilTestEnable(c, on);
}
inline void cmd_set_depth_bias_enable(VkCommandBuffer c, VkBool32 on) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetDepthBiasEnable(r_, on)));
    ::vkCmdSetDepthBiasEnable(c, on);
}
inline void cmd_set_stencil_op(VkCommandBuffer c, VkStencilFaceFlags faces, VkStencilOp fail, VkStencilOp pass,
                               VkStencilOp depth_fail, VkCompareOp compare) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetStencilOp(r_, faces, fail, pass, depth_fail, compare)));
    ::vkCmdSetStencilOp(c, faces, fail, pass, depth_fail, compare);
}
inline void cmd_set_line_width(VkCommandBuffer c, float w) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetLineWidth(r_, w)));
    ::vkCmdSetLineWidth(c, w);
}
inline void cmd_set_depth_bias(VkCommandBuffer c, float constant, float clamp, float slope) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdSetDepthBias(r_, constant, clamp, slope)));
    ::vkCmdSetDepthBias(c, constant, clamp, slope);
}
inline void cmd_set_depth_bias2(VkCommandBuffer c, const VkDepthBiasInfoEXT *info) {
    // (the chain the renderer gives it: a representation at most)
    struct Copy {
        VkDepthBiasInfoEXT info;
        VkDepthBiasRepresentationInfoEXT representation;
        bool chained;
    } copy{*info, {VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT}, false};
    if (const auto *next = static_cast<const VkBaseInStructure *>(info->pNext);
        next && next->sType == VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT) {
        copy.representation = *reinterpret_cast<const VkDepthBiasRepresentationInfoEXT *>(next);
        copy.representation.pNext = nullptr;
        copy.chained = true;
    }
    copy.info.pNext = nullptr;
    XF_REC_ENTER(c, XF_REC_PUT(Copy k = copy; if (k.chained) k.info.pNext = &k.representation;
                               ::vkCmdSetDepthBias2EXT(r_, &k.info)));
    ::vkCmdSetDepthBias2EXT(c, info);
}
inline void cmd_begin_conditional_rendering(VkCommandBuffer c, const VkConditionalRenderingBeginInfoEXT *info) {
    VkConditionalRenderingBeginInfoEXT copy = *info;
    copy.pNext = nullptr;
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdBeginConditionalRenderingEXT(r_, &copy)));
    ::vkCmdBeginConditionalRenderingEXT(c, info);
}
inline void cmd_end_conditional_rendering(VkCommandBuffer c) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdEndConditionalRenderingEXT(r_)));
    ::vkCmdEndConditionalRenderingEXT(c);
}
inline void cmd_draw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first,
                     uint32_t first_instance) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdDraw(r_, vertices, instances, first, first_instance)));
    ::vkCmdDraw(c, vertices, instances, first, first_instance);
}
inline void cmd_draw_indexed(VkCommandBuffer c, uint32_t indices, uint32_t instances, uint32_t first,
                             int32_t vertex_offset, uint32_t first_instance) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdDrawIndexed(r_, indices, instances, first, vertex_offset, first_instance)));
    ::vkCmdDrawIndexed(c, indices, instances, first, vertex_offset, first_instance);
}
inline void cmd_dispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdDispatch(r_, x, y, z)));
    ::vkCmdDispatch(c, x, y, z);
}
inline void cmd_push_constants(VkCommandBuffer c, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                               uint32_t size, const void *values) {
    XF_REC_ENTER(c, const uint8_t *v = ring_.copy(static_cast<const uint8_t *>(values), size);
                 XF_REC_PUT(::vkCmdPushConstants(r_, layout, stages, offset, size, v)));
    ::vkCmdPushConstants(c, layout, stages, offset, size, values);
}
// A structure's chain copied into the ring: the structures the renderer chains to what it
// records (an image barrier's sample locations, for a multisampled depth image at custom
// sample positions); any other is a renderer the ring does not know, stopped here.
inline const void *copy_chain(Ring &ring, const void *next) {
    if (!next) return nullptr;
    const auto *base = static_cast<const VkBaseInStructure *>(next);
    if (base->sType == VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT) {
        const auto *from = static_cast<const VkSampleLocationsInfoEXT *>(next);
        VkSampleLocationsInfoEXT *to = ring.copy(from, 1);
        to->pNext = copy_chain(ring, from->pNext);
        to->pSampleLocations = ring.copy(from->pSampleLocations, from->sampleLocationsCount);
        return to;
    }
    std::fprintf(stderr, "record ring: a structure of type %d chained to a command\n", int(base->sType));
    std::abort();
}
inline void cmd_pipeline_barrier2(VkCommandBuffer c, const VkDependencyInfo *dep) {
    XF_REC_ENTER(
        c, VkDependencyInfo d = *dep; d.pNext = copy_chain(ring_, dep->pNext);
        VkMemoryBarrier2 *memory = ring_.copy(dep->pMemoryBarriers, dep->memoryBarrierCount);
        VkBufferMemoryBarrier2 *buffers = ring_.copy(dep->pBufferMemoryBarriers, dep->bufferMemoryBarrierCount);
        VkImageMemoryBarrier2 *images = ring_.copy(dep->pImageMemoryBarriers, dep->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < dep->memoryBarrierCount; i++) memory[i].pNext = copy_chain(ring_, memory[i].pNext);
        for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; i++) buffers[i].pNext =
            copy_chain(ring_, buffers[i].pNext);
        for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) images[i].pNext =
            copy_chain(ring_, images[i].pNext);
        d.pMemoryBarriers = memory, d.pBufferMemoryBarriers = buffers, d.pImageMemoryBarriers = images;
        XF_REC_PUT(::vkCmdPipelineBarrier2(r_, &d)));
    ::vkCmdPipelineBarrier2(c, dep);
}
inline void cmd_copy_buffer(VkCommandBuffer c, VkBuffer from, VkBuffer to, uint32_t count,
                            const VkBufferCopy *regions) {
    XF_REC_ENTER(c, const VkBufferCopy *r = ring_.copy(regions, count);
                 XF_REC_PUT(::vkCmdCopyBuffer(r_, from, to, count, r)));
    ::vkCmdCopyBuffer(c, from, to, count, regions);
}
inline void cmd_fill_buffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize at, VkDeviceSize size, uint32_t value) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdFillBuffer(r_, b, at, size, value)));
    ::vkCmdFillBuffer(c, b, at, size, value);
}
inline void cmd_begin_rendering(VkCommandBuffer c, const VkRenderingInfo *info) {
    XF_REC_ENTER(
        c, VkRenderingInfo i = *info; i.pNext = copy_chain(ring_, info->pNext);
        VkRenderingAttachmentInfo *colors = ring_.copy(info->pColorAttachments, info->colorAttachmentCount);
        for (uint32_t k = 0; k < info->colorAttachmentCount; k++) colors[k].pNext = copy_chain(ring_, colors[k].pNext);
        i.pColorAttachments = colors; const VkRenderingAttachmentInfo *depth = ring_.copy(info->pDepthAttachment, 1);
        const VkRenderingAttachmentInfo *stencil =
            info->pStencilAttachment == info->pDepthAttachment ? depth : ring_.copy(info->pStencilAttachment, 1);
        i.pDepthAttachment = depth, i.pStencilAttachment = stencil; XF_REC_PUT(::vkCmdBeginRendering(r_, &i)));
    ::vkCmdBeginRendering(c, info);
}
inline void cmd_end_rendering(VkCommandBuffer c) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdEndRendering(r_)));
    ::vkCmdEndRendering(c);
}
inline void cmd_copy_image_to_buffer(VkCommandBuffer c, VkImage image, VkImageLayout layout, VkBuffer b, uint32_t count,
                                     const VkBufferImageCopy *regions) {
    XF_REC_ENTER(c, const VkBufferImageCopy *r = ring_.copy(regions, count);
                 XF_REC_PUT(::vkCmdCopyImageToBuffer(r_, image, layout, b, count, r)));
    ::vkCmdCopyImageToBuffer(c, image, layout, b, count, regions);
}
inline void cmd_copy_buffer_to_image(VkCommandBuffer c, VkBuffer b, VkImage image, VkImageLayout layout, uint32_t count,
                                     const VkBufferImageCopy *regions) {
    XF_REC_ENTER(c, const VkBufferImageCopy *r = ring_.copy(regions, count);
                 XF_REC_PUT(::vkCmdCopyBufferToImage(r_, b, image, layout, count, r)));
    ::vkCmdCopyBufferToImage(c, b, image, layout, count, regions);
}
inline void cmd_copy_image(VkCommandBuffer c, VkImage from, VkImageLayout from_layout, VkImage to,
                           VkImageLayout to_layout, uint32_t count, const VkImageCopy *regions) {
    XF_REC_ENTER(c, const VkImageCopy *r = ring_.copy(regions, count);
                 XF_REC_PUT(::vkCmdCopyImage(r_, from, from_layout, to, to_layout, count, r)));
    ::vkCmdCopyImage(c, from, from_layout, to, to_layout, count, regions);
}
inline void cmd_clear_attachments(VkCommandBuffer c, uint32_t count, const VkClearAttachment *attachments,
                                  uint32_t rect_count, const VkClearRect *rects) {
    XF_REC_ENTER(c, const VkClearAttachment *a = ring_.copy(attachments, count);
                 const VkClearRect *r = ring_.copy(rects, rect_count);
                 XF_REC_PUT(::vkCmdClearAttachments(r_, count, a, rect_count, r)));
    ::vkCmdClearAttachments(c, count, attachments, rect_count, rects);
}
inline void cmd_clear_color_image(VkCommandBuffer c, VkImage image, VkImageLayout layout,
                                  const VkClearColorValue *color, uint32_t count,
                                  const VkImageSubresourceRange *ranges) {
    const VkClearColorValue value = *color;
    XF_REC_ENTER(c, const VkImageSubresourceRange *r = ring_.copy(ranges, count);
                 XF_REC_PUT(::vkCmdClearColorImage(r_, image, layout, &value, count, r)));
    ::vkCmdClearColorImage(c, image, layout, color, count, ranges);
}
inline void cmd_clear_depth_stencil_image(VkCommandBuffer c, VkImage image, VkImageLayout layout,
                                          const VkClearDepthStencilValue *value, uint32_t count,
                                          const VkImageSubresourceRange *ranges) {
    const VkClearDepthStencilValue v = *value;
    XF_REC_ENTER(c, const VkImageSubresourceRange *r = ring_.copy(ranges, count);
                 XF_REC_PUT(::vkCmdClearDepthStencilImage(r_, image, layout, &v, count, r)));
    ::vkCmdClearDepthStencilImage(c, image, layout, value, count, ranges);
}
inline void cmd_write_timestamp2(VkCommandBuffer c, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdWriteTimestamp2(r_, stage, pool, query)));
    ::vkCmdWriteTimestamp2(c, stage, pool, query);
}
inline void cmd_reset_query_pool(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdResetQueryPool(r_, pool, first, count)));
    ::vkCmdResetQueryPool(c, pool, first, count);
}
inline void cmd_begin_query(VkCommandBuffer c, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdBeginQuery(r_, pool, query, flags)));
    ::vkCmdBeginQuery(c, pool, query, flags);
}
inline void cmd_end_query(VkCommandBuffer c, VkQueryPool pool, uint32_t query) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdEndQuery(r_, pool, query)));
    ::vkCmdEndQuery(c, pool, query);
}
inline void cmd_copy_query_pool_results(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer b,
                                        VkDeviceSize at, VkDeviceSize stride, VkQueryResultFlags flags) {
    XF_REC_ENTER(c, XF_REC_PUT(::vkCmdCopyQueryPoolResults(r_, pool, first, count, b, at, stride, flags)));
    ::vkCmdCopyQueryPoolResults(c, pool, first, count, b, at, stride, flags);
}

} // namespace xf::rec

// Every vkCmd* the renderer calls goes through the functions above (renderer.cpp includes this
// header after volk's, and nothing after it declares these names).
#define vkBeginCommandBuffer ::xf::rec::begin_command_buffer
#define vkEndCommandBuffer ::xf::rec::end_command_buffer
#define vkCmdBindPipeline ::xf::rec::cmd_bind_pipeline
#define vkCmdBindDescriptorSets ::xf::rec::cmd_bind_descriptor_sets
#define vkCmdBindIndexBuffer ::xf::rec::cmd_bind_index_buffer
#define vkCmdBindVertexBuffers ::xf::rec::cmd_bind_vertex_buffers
#define vkCmdSetViewport ::xf::rec::cmd_set_viewport
#define vkCmdSetScissor ::xf::rec::cmd_set_scissor
#define vkCmdSetBlendConstants ::xf::rec::cmd_set_blend_constants
#define vkCmdSetStencilReference ::xf::rec::cmd_set_stencil_reference
#define vkCmdSetStencilCompareMask ::xf::rec::cmd_set_stencil_compare_mask
#define vkCmdSetStencilWriteMask ::xf::rec::cmd_set_stencil_write_mask
#define vkCmdSetPrimitiveTopology ::xf::rec::cmd_set_primitive_topology
#define vkCmdSetPrimitiveRestartEnable ::xf::rec::cmd_set_primitive_restart_enable
#define vkCmdSetCullMode ::xf::rec::cmd_set_cull_mode
#define vkCmdSetFrontFace ::xf::rec::cmd_set_front_face
#define vkCmdSetDepthTestEnable ::xf::rec::cmd_set_depth_test_enable
#define vkCmdSetDepthWriteEnable ::xf::rec::cmd_set_depth_write_enable
#define vkCmdSetDepthCompareOp ::xf::rec::cmd_set_depth_compare_op
#define vkCmdSetStencilTestEnable ::xf::rec::cmd_set_stencil_test_enable
#define vkCmdSetDepthBiasEnable ::xf::rec::cmd_set_depth_bias_enable
#define vkCmdSetStencilOp ::xf::rec::cmd_set_stencil_op
#define vkCmdSetLineWidth ::xf::rec::cmd_set_line_width
#define vkCmdSetDepthBias ::xf::rec::cmd_set_depth_bias
#define vkCmdSetDepthBias2EXT ::xf::rec::cmd_set_depth_bias2
#define vkCmdBeginConditionalRenderingEXT ::xf::rec::cmd_begin_conditional_rendering
#define vkCmdEndConditionalRenderingEXT ::xf::rec::cmd_end_conditional_rendering
#define vkCmdDraw ::xf::rec::cmd_draw
#define vkCmdDrawIndexed ::xf::rec::cmd_draw_indexed
#define vkCmdDispatch ::xf::rec::cmd_dispatch
#define vkCmdPushConstants ::xf::rec::cmd_push_constants
#define vkCmdPipelineBarrier2 ::xf::rec::cmd_pipeline_barrier2
#define vkCmdCopyBuffer ::xf::rec::cmd_copy_buffer
#define vkCmdFillBuffer ::xf::rec::cmd_fill_buffer
#define vkCmdBeginRendering ::xf::rec::cmd_begin_rendering
#define vkCmdEndRendering ::xf::rec::cmd_end_rendering
#define vkCmdCopyImageToBuffer ::xf::rec::cmd_copy_image_to_buffer
#define vkCmdCopyBufferToImage ::xf::rec::cmd_copy_buffer_to_image
#define vkCmdCopyImage ::xf::rec::cmd_copy_image
#define vkCmdClearAttachments ::xf::rec::cmd_clear_attachments
#define vkCmdClearColorImage ::xf::rec::cmd_clear_color_image
#define vkCmdClearDepthStencilImage ::xf::rec::cmd_clear_depth_stencil_image
#define vkCmdWriteTimestamp2 ::xf::rec::cmd_write_timestamp2
#define vkCmdResetQueryPool ::xf::rec::cmd_reset_query_pool
#define vkCmdBeginQuery ::xf::rec::cmd_begin_query
#define vkCmdEndQuery ::xf::rec::cmd_end_query
#define vkCmdCopyQueryPoolResults ::xf::rec::cmd_copy_query_pool_results
