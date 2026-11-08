// A game being prepared (prepare_job.h).

#include "prepare_job.h"

#include "xenoforge/shader_step.h"

#include <algorithm>
#include <cstdio>

namespace xf::frontend {

namespace {
constexpr size_t MAX_LOG = 4000;
using Clock = std::chrono::steady_clock;
double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
} // namespace

double PrepareJob::stage_weight(build::Stage stage) {
    switch (stage) {
    case build::Stage::Reading: return 0.03;
    case build::Stage::Discovering: return 0.07;
    case build::Stage::Emitting: return 0.15;
    case build::Stage::Compiling: return 0.68;
    case build::Stage::Linking: return 0.07;
    default: return 0;
    }
}

const char *PrepareJob::stage_name(build::Stage stage) {
    switch (stage) {
    case build::Stage::Reading: return "Reading the disc";
    case build::Stage::Discovering: return "Finding the game's functions";
    case build::Stage::Emitting: return "Writing the code";
    case build::Stage::Compiling: return "Compiling";
    case build::Stage::Linking: return "Linking the library";
    case build::Stage::Done: return "Done";
    case build::Stage::Failed: return "Failed";
    }
    return "";
}

PrepareJob::~PrepareJob() { join(); }

void PrepareJob::join() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

PrepareJob::Runner PrepareJob::build_runner(std::filesystem::path disc, std::filesystem::path cache_dir,
                                            unsigned jobs, bool force) {
    return [disc = std::move(disc), cache_dir = std::move(cache_dir), jobs, force](build::Progress &progress,
                                                                            std::string &error) {
        build::BuildOptions options;
        if (jobs) options.jobs = jobs;
        options.force = force;
        // the shaders runs met, made into the title's pipeline cache
        options.shader_step = &render::run_shader_step;
        return build::prepare_game(disc, cache_dir, progress, error, options);
    };
}

PrepareJob::Runner PrepareJob::demo_runner(double speed, bool fail) {
    return [speed, fail](build::Progress &progress, std::string &error) {
        using build::Stage;
        struct Step {
            Stage stage;
            uint64_t units;
            double seconds; // the stage's whole, at speed 1
            const char *what;
        };
        const Step steps[] = {{Stage::Reading, 1, 4, "default.xex"},
                              {Stage::Discovering, 1, 9, "functions"},
                              {Stage::Emitting, 291, 22, "ppc_recomp"},
                              {Stage::Compiling, 291, 110, "ppc_recomp"},
                              {Stage::Linking, 1, 12, "game.dll"}};
        for (const Step &step : steps) {
            for (uint64_t i = 0; i <= step.units; i++) {
                if (progress.cancel && progress.cancel->load()) {
                    error = "cancelled";
                    return false;
                }
                char detail[64];
                if (step.units > 1)
                    std::snprintf(detail, sizeof(detail), "%s.%03llu.cpp", step.what, (unsigned long long)i);
                else
                    std::snprintf(detail, sizeof(detail), "%s", step.what);
                progress.report(step.stage, i, step.units, detail);
                if (fail && step.stage == Stage::Compiling && i == step.units * 2 / 5) {
                    error = "error: use of undeclared identifier 'xf_dispatch_far' in ppc_recomp.114.cpp:2041";
                    progress.report(Stage::Failed, 0, 0, error);
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::duration<double>(step.seconds / double(step.units) / speed));
            }
        }
        progress.report(Stage::Done, 0, 0, "");
        return true;
    };
}

void PrepareJob::start(std::string title, Runner runner) {
    join();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State();
        log_.clear();
        log_dropped_ = 0;
        state_.started = state_.running = true;
        state_.title = std::move(title);
        log_.push_back("Preparing " + state_.title);
    }
    started_ = true;
    running_ = true;
    began_ = Clock::now();
    thread_ = std::thread([this, runner = std::move(runner)] { run(runner); });
}

void PrepareJob::run(Runner runner) {
    build::Progress progress;
    progress.cancel = &cancel_;
    build::Stage last = build::Stage::Done;
    bool announced = false;
    progress.report = [&](build::Stage stage, uint64_t done, uint64_t total, std::string_view detail) {
        std::lock_guard<std::mutex> lock(mutex_);
        State &s = state_;
        if (!announced || stage != last) {
            announced = true;
            last = stage;
            if (stage != build::Stage::Done && stage != build::Stage::Failed)
                log_.push_back(std::string(stage_name(stage)) + (total > 1 ? "..." : "..."));
        }
        s.stage = stage;
        s.done = done, s.total = total;
        s.detail = std::string(detail);
        if (stage == build::Stage::Failed) {
            s.error = std::string(detail);
        } else if (!detail.empty() && stage != build::Stage::Done) {
            log_.push_back("  " + std::string(detail));
        }
        if (log_.size() > MAX_LOG) {
            log_dropped_ += log_.size() - MAX_LOG;
            log_.erase(log_.begin(), log_.begin() + (log_.size() - MAX_LOG));
        }
        // the whole: the stages before this one, and this one's share
        double before = 0;
        for (build::Stage e : {build::Stage::Reading, build::Stage::Discovering, build::Stage::Emitting,
                               build::Stage::Compiling, build::Stage::Linking}) {
            if (e == stage) break;
            before += stage_weight(e);
        }
        if (stage == build::Stage::Done)
            s.fraction = 1;
        else if (stage != build::Stage::Failed)
            s.fraction = std::min(before + stage_weight(stage) * (total ? double(done) / double(total) : 0.0), 1.0);
    };

    std::string error;
    bool ok = false;
    try {
        ok = runner(progress, error);
    } catch (const std::exception &e) {
        error = e.what();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ended_ = Clock::now();
    State &s = state_;
    s.running = false;
    s.finished = true;
    s.ok = ok;
    s.cancelled = !ok && cancel_.load();
    if (!ok) {
        if (s.error.empty()) s.error = error.empty() ? "the build failed" : error;
        s.stage = build::Stage::Failed;
        log_.push_back(s.cancelled ? "Cancelled." : "Failed: " + s.error);
    } else {
        s.stage = build::Stage::Done;
        s.fraction = 1;
        log_.push_back("Done.");
    }
    running_ = false;
}

void PrepareJob::cancel() { cancel_ = true; }

PrepareJob::State PrepareJob::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    State s = state_;
    s.log_lines = log_dropped_ + log_.size();
    if (s.started) {
        s.elapsed = seconds((s.running ? Clock::now() : ended_) - began_);
        // the time left, from the pace of the whole so far (not before it
        // means something)
        if (s.running && s.fraction >= 0.04) s.remaining = s.elapsed * (1.0 - s.fraction) / s.fraction;
    }
    return s;
}

std::vector<std::string> PrepareJob::log_from(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t first = index > log_dropped_ ? index - log_dropped_ : 0;
    if (first >= log_.size()) return {};
    return std::vector<std::string>(log_.begin() + first, log_.end());
}

} // namespace xf::frontend
