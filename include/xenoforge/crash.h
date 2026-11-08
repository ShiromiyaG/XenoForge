// Crash and stop reports (docs/runtime.md, "Crash and stop reports"): on a
// host fault in guest code, or when the kernel stops the run, a report with
// where the guest was (its address and function), its stack (the r1 back
// chain and the saved LRs, each as a function), the thread, and the kernel
// calls it made last. Written to a file; the tools print it. src/kernel/crash.cpp.

#pragma once

#include "xenoforge/kernel.h"

#include <filesystem>
#include <string>

namespace xf::kernel {

struct CrashReport {
    std::string what;  // "reading host 0x10 (guest 0x00000010)", the stop reason
    std::string text;  // the whole report
    std::string stack; // one line: "pc <- frame <- frame ...", each as module+offset (function)
    uint32_t pc = 0, lr = 0; // the faulting thread's last known (the interpreter's exact; generated code's, a call's)
    std::filesystem::path file; // where write_crash_report put it
};

// Notes an export call in the calling thread's ring of its last ones (16);
// run_import's, with the LR it was called from.
void note_kernel_call(const Import &import, uint32_t lr);

// The report for the calling thread's guest context (none on a host thread
// that is not a guest's: then without the thread, stack and calls), and for
// a fault, the host code address it came at (to name the generated function
// that faulted: its context's pc is a call's, not the access's). Takes no
// lock that can be held by the code that faulted: it runs in the handler.
CrashReport make_crash_report(Kernel &k, const std::string &what, const void *host_pc = nullptr);

// The text into <directory>/crash-<date>-<time>.txt (the directory made);
// the path, or empty if it cannot be written. Sets report.file.
bool write_crash_report(CrashReport &report, const std::filesystem::path &directory);

// Called in the fault handler, after the report is written; it prints and
// ends the process (without it the handler prints the report to stderr and
// ends with exit code 3).
using CrashHook = void (*)(Kernel &k, CrashReport &report, void *user);

// The handler of access faults, illegal instructions and integer faults in
// the process: a vectored exception handler on Windows (where the
// ExceptionRecord says the access), signals under POSIX. Install it before
// the memory's write watches are used (memory.cpp's handler chains to the
// one before it for the faults it does not take). `directory` is where the
// report is written (empty: not written).
void install_crash_handler(Kernel &k, const std::filesystem::path &directory, CrashHook hook = nullptr,
                           void *user = nullptr);

} // namespace xf::kernel
