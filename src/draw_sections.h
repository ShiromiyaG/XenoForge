// The draw's time by section (XF_DRAW_SECTIONS=1): a time stamp at each of
// the draw's section boundaries, the cycles between two added to the section
// the second one closes, printed at the process's end. For measuring where
// a draw's time goes, a section against another within one run, which other
// processes' load skews less than a whole run's time. Off, a mark is one
// test of a constant.
#pragma once

#include "fast_clock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace xf {

class DrawSections {
  public:
    enum Section : uint32_t {
        Front,           // the draw's checks and its front part
        Indices,         // the index cache, the indices' conversion, tessellation's control points
        Constants,       // the constants' blocks brought up to date
        Targets,         // the targets' tiles taken over, the system constants
        Textures,        // the texture slots
        Pipeline,        // expanded primitives, the pipeline's key and lookup
        Vertex,          // the vertex data's request
        Fit,             // the draw data's room checked, the submission begun
        ConstantUploads, // the constants' upload
        SystemUploads,   // the system constants' upload
        Uploads,         // the indices' and attributes' words
        Pass,            // the render pass begun where it is another
        State,           // the binds and the dynamic state
        Record,          // the draw itself, and what follows it
        COUNT,
    };
    static bool on() {
        static const bool v = [] {
            const char *e = std::getenv("XF_DRAW_SECTIONS");
            return e && e[0] == '1';
        }();
        return v;
    }
    static DrawSections &get() {
        static DrawSections s;
        return s;
    }
    void begin() { last_ = FastClock::now(); }
    void mark(Section s) {
        const uint64_t now = FastClock::now();
        cycles_[s] += now - last_;
        counts_[s]++;
        last_ = now;
    }
    // the tallies so far dropped (the first play's, which makes pipelines)
    void reset() {
        for (uint32_t i = 0; i < COUNT; i++) cycles_[i] = counts_[i] = 0;
        for (uint32_t i = 0; i < TALLIES; i++) tallies_[i] = 0;
        draws_ = 0;
    }
    void draw() { draws_++; }
    // events counted beside the sections
    enum Tally : uint32_t {
        SlotHits,
        SlotMisses,
        UnwrittenScans,
        ConstantUploadCount,
        SystemUploadCount,
        MissTicks,
        MissEpoch,
        MissFetch,
        MissLoads,
        MissStale,
        MissWritten,
        TALLIES
    };
    void tally(Tally t, uint64_t n = 1) { tallies_[t] += n; }
    ~DrawSections() {
        if (!on() || !draws_) return;
        static const char *const names[COUNT] = {"front",    "indices", "constants", "targets", "textures",
                                                 "pipeline", "vertex",  "fit",       "constup", "systemup",
                                                 "words",    "pass",    "state",     "draw"};
        uint64_t total = 0;
        for (uint32_t i = 0; i < COUNT; i++) total += cycles_[i];
        std::fprintf(stderr, "draw sections: %llu draws, %.0f ticks a draw\n", (unsigned long long)draws_,
                     double(total) / double(draws_));
        for (uint32_t i = 0; i < COUNT; i++)
            std::fprintf(stderr, "  %-10s %7.0f ticks a draw %5.1f%% (%llu reached)\n", names[i],
                         double(cycles_[i]) / double(draws_), total ? 100.0 * double(cycles_[i]) / double(total) : 0.0,
                         (unsigned long long)counts_[i]);
        static const char *const tallies[TALLIES] = {"slot memo hits",   "slot memo misses", "unwritten scans",
                                                     "constant uploads", "system uploads",   "ticks in slot misses",
                                                     "miss: epoch",      "miss: fetch",      "miss: loads",
                                                     "miss: stale",      "miss: written"};
        for (uint32_t i = 0; i < TALLIES; i++)
            std::fprintf(stderr, "  %s: %.4f a draw (%llu)\n", tallies[i], double(tallies_[i]) / double(draws_),
                         (unsigned long long)tallies_[i]);
    }

  private:
    uint64_t last_ = 0, draws_ = 0;
    uint64_t cycles_[COUNT] = {}, counts_[COUNT] = {}, tallies_[TALLIES] = {};
};

#define XF_DRAW_TALLY(t)                                                                                               \
    do {                                                                                                               \
        if (::xf::DrawSections::on()) ::xf::DrawSections::get().tally(::xf::DrawSections::t);                          \
    } while (0)

// marks a section's end where sections are on
#define XF_DRAW_SECTION(s)                                                                                             \
    do {                                                                                                               \
        if (::xf::DrawSections::on()) ::xf::DrawSections::get().mark(::xf::DrawSections::s);                           \
    } while (0)

} // namespace xf
