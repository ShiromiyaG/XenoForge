// The lockstep check's kernel side (xenoforge/lockstep.h): the calls it
// takes (Kernel::run_generated), the imports that end its segments
// (Kernel::run_import) and the time base it replays.

#pragma once

#include "xenoforge/kernel.h"
#include "xenoforge/lockstep.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace xf::kernel {

class Lockstep {
  public:
    Lockstep(Kernel &k, const LockstepOptions &options);
    // whether a private view of the guest memory can be made
    bool usable(std::string &error);

    // whether this call to generated code is to be checked (none while this
    // thread is in one already)
    bool wanted(ppc::Context &c, uint32_t target);
    // the call checked: the interpreter on a private view, then fn for real
    void check(ppc::Context &c, uint32_t target, void (*fn)(ppc::Context &));
    // an import the code calls, on any thread: in a call being checked, the
    // end of a segment (compared) and the start of the next (after it)
    void before_import(ppc::Context &c, const Import &import);
    // resume: where the code goes on (0: LR); skip_mid: past a mid-function
    // hook there, which ran (the port's hooks end segments as imports do)
    void after_import(ppc::Context &c, uint32_t resume = 0, bool skip_mid = false);
    // mftb in a segment's generated code: what the interpreter read
    bool replay_time_base(ppc::Context &c, uint64_t &value);

    LockstepCounts counts() const;
    std::string report() const;

    struct Check; // a thread's call being checked (lockstep.cpp)

  private:
    void begin_segment(Check &check, ppc::Context &c, uint32_t pc);
    void compare(Check &check, ppc::Context &c, const Import *import);
    void give_up(const std::string &why);
    Check *current(ppc::Context &c);
    // The private views, a few shared by the threads: one is needed only
    // while a segment's interpreter runs, and each costs the host commit
    // charge (a copy-on-write view of 4.5 GB on Windows).
    std::unique_ptr<Memory::PrivateView> acquire_view(std::string &error);
    void release_view(std::unique_ptr<Memory::PrivateView> view); // clean again (null: one dropped)

    Kernel &k_;
    LockstepOptions options_;
    std::unordered_set<uint32_t> functions_;
    uint64_t generation_; // tells this check's threads' state from an earlier kernel's
    ppc::System system_;  // the interpreter's, in a segment
    mutable std::mutex mutex_;
    LockstepCounts counts_; // under mutex_
    std::mutex views_mutex_;
    std::condition_variable view_freed_;
    std::vector<std::unique_ptr<Memory::PrivateView>> free_views_; // under views_mutex_
    size_t views_ = 0;                                             // made and not dropped
};

} // namespace xf::kernel
