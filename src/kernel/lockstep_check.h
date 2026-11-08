// The lockstep check's kernel side (xenoforge/lockstep.h): the calls it
// takes (Kernel::run_generated), the imports that end its segments
// (Kernel::run_import) and the time base it replays.

#pragma once

#include "xenoforge/kernel.h"
#include "xenoforge/lockstep.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

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
    void after_import(ppc::Context &c);
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

    Kernel &k_;
    LockstepOptions options_;
    std::unordered_set<uint32_t> functions_;
    uint64_t generation_; // tells this check's threads' state from an earlier kernel's
    ppc::System system_;  // the interpreter's, in a segment
    mutable std::mutex mutex_;
    LockstepCounts counts_; // under mutex_
};

} // namespace xf::kernel
