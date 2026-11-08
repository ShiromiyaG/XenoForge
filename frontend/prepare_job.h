// A game being prepared (docs/runtime.md, "The launcher"): xf::build::
// prepare_game on a worker thread, and what the launcher's screen needs of it
// at each frame: the stage, how far, the files done of the total, the time
// gone and left, the log, and how it ended. No UI code, no SDL: the view
// only reads state() and calls cancel(); the tests run it with runners of
// their own.

#pragma once

#include "xenoforge/game_build.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xf::frontend {

class PrepareJob {
  public:
    // What runs on the thread: the build, reporting through the progress it
    // is given; false and why when it fails or is cancelled.
    using Runner = std::function<bool(build::Progress &progress, std::string &error)>;

    ~PrepareJob(); // cancelled, and waited for

    // The real build of a disc into the cache, with `jobs` compilers at once
    // (0: the build's default).
    // `force` builds although the cache has the library (a rebuild).
    static Runner build_runner(std::filesystem::path disc, std::filesystem::path cache_dir, unsigned jobs = 0,
                               bool force = false);
    // A build that is not one, for --demo-progress: a few minutes of stages
    // at `speed` times real time, ending in a failure when `fail`.
    static Runner demo_runner(double speed, bool fail);

    // Starts a run (the one before, if any, ended and joined first).
    void start(std::string title, Runner runner);
    // Asks the build to stop at its next unit; state().cancelled once it has.
    void cancel();
    bool active() const { return running_.load(); }
    bool started() const { return started_; }

    struct State {
        bool started = false, running = false, finished = false, ok = false, cancelled = false;
        build::Stage stage = build::Stage::Reading;
        uint64_t done = 0, total = 0; // in the stage: files, modules, steps
        std::string detail;           // the unit under way
        std::string error;            // why it failed
        double fraction = 0;          // the whole build, 0 to 1 (stages weighed as they cost)
        double elapsed = 0;           // seconds, stopped when it ends
        double remaining = -1;        // seconds estimated from the pace so far, -1 while unknown
        size_t log_lines = 0; // lines so far (log_from fetches them)
        std::string title;
    };
    State state() const;
    // the log's lines from the index-th on (a screen asks for what it has not got)
    std::vector<std::string> log_from(size_t index) const;

    // How much of the whole each stage is (a build's time goes mostly to
    // compiling the generated code), and the name a screen shows for it.
    static double stage_weight(build::Stage stage);
    static const char *stage_name(build::Stage stage);

  private:
    void run(Runner runner);
    void join();

    mutable std::mutex mutex_;
    State state_;
    std::vector<std::string> log_;
    size_t log_dropped_ = 0;
    std::atomic<bool> cancel_{false}, running_{false};
    bool started_ = false;
    std::thread thread_;
    std::chrono::steady_clock::time_point began_, ended_;
};

} // namespace xf::frontend
