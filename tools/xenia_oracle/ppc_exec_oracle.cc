/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
// ppc_exec_oracle: the differential fuzzer's cases (include/xenoforge/
// fuzz_format.h) run by Xenia's JIT, its x64 backend, the way Xenia's own
// instruction test runner runs a test (src/xenia/cpu/ppc/testing/
// ppc_testing_main.cc, TestRunner::Setup and Run, from which this is adapted):
// one processor with the cases' code as a raw module at 0x80000000, a thread,
// the test heap at 0x10001000; per case the registers and the memory window
// set, MXCSR and the backend's mode reset, the case's function called with the
// sentinel as LR, and the state after written out. A case with an instruction
// Xenia has no emitter for is marked unsupported and not run (Xenia would
// compile it as a no-op). Built by build.bat, not part of XenoForge's build.
//
// usage: ppc_exec_oracle <cases.xfz> <out.xfs>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <xmmintrin.h>

#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

#include "xenoforge/fuzz_format.h"

DECLARE_bool(break_on_unimplemented_instructions);
// defined by Xenia's app, read by its executable loader (never run here)
namespace cvars {
bool allow_plugins = false;
}

namespace fz = xf::ppc::fuzz;
using namespace xe::cpu;

namespace {

// a call that faults inside the JIT's code counts as stopped, not as the end
bool call_guarded(Function *fn, ThreadState *thread, uint32_t lr) {
    __try {
        fn->Call(thread, lr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: ppc_exec_oracle <cases.xfz> <out.xfs>\n");
        return 2;
    }
    // XF_ORACLE_TRACE: each case's number before it runs (to find one that takes the oracle down)
    const bool trace = std::getenv("XF_ORACLE_TRACE") != nullptr;
    std::vector<fz::Case> cases;
    if (!fz::read_cases(argv[1], cases)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    cvars::break_on_unimplemented_instructions = false;

    // the code image, as RawModule loads it from a file
    std::filesystem::path bin = std::filesystem::path(argv[2]).replace_extension(".bin");
    {
        std::vector<uint8_t> image = fz::code_image(cases);
        FILE *f = std::fopen(bin.string().c_str(), "wb");
        if (!f) return std::fprintf(stderr, "cannot write %s\n", bin.string().c_str()), 1;
        std::fwrite(image.data(), 1, image.size(), f);
        std::fclose(f);
    }

    auto memory = std::make_unique<xe::Memory>();
    if (!memory->Initialize()) return std::fprintf(stderr, "Xenia's memory did not initialize\n"), 1;
    auto processor = std::make_unique<Processor>(memory.get(), nullptr);
    if (!processor->Setup(std::make_unique<backend::x64::X64Backend>()))
        return std::fprintf(stderr, "Xenia's processor did not set up\n"), 1;
    auto module = std::make_unique<RawModule>(processor.get());
    if (!module->LoadFile(fz::CODE_BASE, bin)) return std::fprintf(stderr, "cannot load the code\n"), 1;
    processor->AddModule(std::move(module));
    processor->backend()->CommitExecutableRange(fz::CODE_BASE, fz::CODE_BASE + 16 * 1024 * 1024);
    memory->LookupHeap(0)->AllocFixed(0x10001000, 0xEFFF, 0, xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                                      xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
    uint32_t stack_size = 64 * 1024, stack_address = fz::CODE_BASE - stack_size;
    auto thread = std::make_unique<ThreadState>(processor.get(), 0x100, stack_address, stack_address - 0x1000);
    auto *x64 = static_cast<backend::x64::X64Backend *>(processor->backend());

    std::vector<fz::Result> results(cases.size());
    size_t unsupported = 0, stopped = 0;
    for (size_t k = 0; k < cases.size(); k++) {
        const fz::Case &cs = cases[k];
        fz::Result &res = results[k];
        bool supported = true;
        for (uint32_t w : cs.code) {
            ppc::PPCOpcode op = ppc::LookupOpcode(w);
            if (op == ppc::PPCOpcode::kInvalid || !ppc::GetOpcodeInfo(op).emit) supported = false;
        }
        if (!supported) {
            res.status = fz::UNSUPPORTED, res.out = cs.in, unsupported++;
            continue;
        }
        ppc::PPCContext *c = thread->context();
        const fz::State &in = cs.in;
        for (int n = 0; n < 32; n++) c->r[n] = in.r[n], std::memcpy(&c->f[n], &in.f[n], 8);
        for (int n = 0; n < 128; n++) std::memcpy(c->v[n].u32, in.v[n], 16);
        // Xenia's set_cr is "not yet implemented": the fields' bytes (lt, gt, eq, so), as its cr() reads them
        for (int n = 0; n < 8; n++) {
            uint32_t field = in.cr >> (28 - 4 * n);
            uint8_t bytes[4] = {uint8_t(field >> 3 & 1), uint8_t(field >> 2 & 1), uint8_t(field >> 1 & 1),
                                uint8_t(field & 1)};
            std::memcpy(&c->cr0.value + n, bytes, 4);
        }
        c->fpscr.value = in.fpscr;
        c->xer_ca = in.xer_ca, c->xer_ov = in.xer_ov, c->xer_so = in.xer_so;
        c->vscr_sat = in.vscr_sat;
        c->vscr_vec = xe::vec128i(0, 0, 0, (in.vscr_nj ? 0x10000u : 0u) | in.vscr_sat);
        c->lr = fz::SENTINEL, c->ctr = in.ctr, c->msr = in.msr;
        c->reserved_val = 0;
        std::memcpy(memory->TranslateVirtual(fz::WINDOW), in.memory, fz::WINDOW_SIZE);
        // the runner's reset of the FPU state before each test
        _mm_setcsr(backend::x64::DEFAULT_FPU_MXCSR);
        x64->BackendContextForGuestContext(c)->flags &= ~(1U << backend::x64::kX64BackendMXCSRModeBit);

        if (trace) std::fprintf(stderr, "case %zu: %08X\n", k, cs.code.empty() ? 0 : cs.code[0]), std::fflush(stderr);
        Function *fn = processor->ResolveFunction(fz::case_address(k));
        bool ran = fn && call_guarded(fn, thread.get(), uint32_t(c->lr));
        fz::State &out = res.out;
        for (int n = 0; n < 32; n++) out.r[n] = c->r[n], std::memcpy(&out.f[n], &c->f[n], 8);
        for (int n = 0; n < 128; n++) std::memcpy(out.v[n], c->v[n].u32, 16);
        out.cr = uint32_t(c->cr());
        out.fpscr = c->fpscr.value;
        out.xer_ca = c->xer_ca, out.xer_ov = c->xer_ov, out.xer_so = c->xer_so;
        out.vscr_sat = c->vscr_sat;
        out.vscr_nj = (c->vscr_vec.u32[3] >> 16) & 1;
        out.lr = c->lr, out.ctr = c->ctr, out.msr = c->msr;
        std::memcpy(out.memory, memory->TranslateVirtual(fz::WINDOW), fz::WINDOW_SIZE);
        if (!ran) res.status = fz::STOPPED, stopped++;
    }
    if (!fz::write_results(argv[2], results)) return std::fprintf(stderr, "cannot write %s\n", argv[2]), 1;
    std::printf("%zu cases: %zu unsupported, %zu stopped\n", cases.size(), unsupported, stopped);
    return 0;
}
