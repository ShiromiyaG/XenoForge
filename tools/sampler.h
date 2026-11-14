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
#include <cstdint>
#include <functional>
#include <string>

namespace xf::sampler {

// start sampling (every `interval_us` microseconds); false where it is not
// supported. With `only_thread` (a host thread id), that thread alone, with
// its stack walked at each sample: the report then lists each function's
// inclusive share too (the samples it was on the stack for), what it and
// everything it calls cost.
bool start(unsigned interval_us = 1000, unsigned long only_thread = 0);
// The generated functions' host addresses and the guest addresses they were
// made from, for naming samples where no symbol does (a game library has no
// symbols): an address there is named after the nearest function at or
// below it, xf_<guest address>
void name_generated(const void *const *hosts, const unsigned *guests, size_t count);
// Names for addresses of code made at run time (the first tier's: its
// stencils, stencil.h's Module::describe), tried before the generated
// functions' table for an address with no symbol.
void name_with(std::function<bool(uint64_t address, std::string &name, uint64_t &displacement)> namer);
// With one thread sampled: only the samples taken under a function whose name has `function` in it
// count in the report (a tool's other work on the thread left out); null or empty for all of them.
void scope(const char *function);
// the calling thread's host id, for start's only_thread (0 where sampling is not supported)
unsigned long current_thread();
// stop and print the top `top` functions and the shares
void stop_and_report(size_t top);
// the CPU time the process has used, all threads, in seconds (-1: unknown)
double process_cpu_seconds();

} // namespace xf::sampler
