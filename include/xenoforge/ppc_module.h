// The generated code's tables (docs/recompiler.md, "Modules"): xfrecomp emit
// writes one per executable or DLL, and the game library carries every
// module's. The kernel gives a module's table the module's address range when
// an image it was made from is loaded, and drops it when the image is
// unloaded; dispatch looks addresses up there. Included by the generated code
// (through ppc_generated.h) and by the kernel; nothing else.

#pragma once

#include "xenoforge/ppc_context.h"

#include <cstddef>
#include <cstdint>

namespace xf::ppc {

// A generated function and the guest address it was made from.
struct GeneratedFunction {
    uint32_t address;
    void (*fn)(Context &);
};

// One module's generated code: the image it was made from (its file name, for
// the reports; where it is placed; the digest of its bytes as the loader
// unpacks them, before any import is bound, xf::image_digest) and its
// functions in address order.
struct GeneratedModule {
    const char *name; // "default.xex", "waves\\L360.dll": the path on the disc
    uint32_t base, size;
    uint64_t digest;
    const GeneratedFunction *functions;
    size_t count;
};

} // namespace xf::ppc

// What a game library defines (xfrecomp emit's modules.cpp): every module's
// table, C linkage so its users declare it without the C++ name carrying the
// types.
//
//   extern "C" const xf::ppc::GeneratedModule *const xf_modules[];
//   extern "C" const size_t xf_module_count;
