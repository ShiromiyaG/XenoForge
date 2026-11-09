// A sampling profiler for boot (--profile N; docs/recompiler.md, "Where the
// time goes"): a thread that stops every other thread of the process about
// every millisecond, reads where it was, and lets it go on. At the end the
// addresses are named from the executable's symbols (the generated
// functions' public names are in the PDB even when their code is built
// without debug information) and the most sampled functions printed, with
// the share of the run each part of the program took (the generated code,
// the instructions' cold paths, the interpreter, the kernel, the GPU, the
// rest). Windows only for now (TODO: POSIX, through a timer signal and the
// interrupted context).

#pragma once

#include <cstddef>

namespace xf::sampler {

// start sampling (every `interval_us` microseconds); false where it is not
// supported. With `only_thread` (a host thread id), that thread alone, with
// its stack walked at each sample: the report then lists each function's
// inclusive share too (the samples it was on the stack for), what it and
// everything it calls cost.
bool start(unsigned interval_us = 1000, unsigned long only_thread = 0);
// stop and print the top `top` functions and the shares
void stop_and_report(size_t top);
// the CPU time the process has used, all threads, in seconds (-1: unknown)
double process_cpu_seconds();

} // namespace xf::sampler
