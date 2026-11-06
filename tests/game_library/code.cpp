// A piece of generated code for game_library_test, written as xfrecomp emit
// writes it (ppc_generated.h, ppc_module.h): built into a game library by the
// recompile step (freestanding, through the runtime's table) and linked into
// the test as well (direct calls), so the two can be run side by side.

#include "xenoforge/ppc_generated.h"

namespace xfm_test {

XF_FN(xf_00001000);
XF_FN(xf_00001100);
XF_FN(xf_00001200);
XF_FN(xf_00002000);
XF_FN(xf_00002100);

// integer ops, a load and a store, then a call through dispatch to
// xf_00001100 (a copy the compiler makes a memcpy call of), then the FPU and
// VMX ops that reach the C library: fctiw (llrint), fadd. (the rounding mode
// and exception flags: fenv), vexptefp and vlogefp (exp2f, log2f)
XF_FN(xf_00001000) {
    I(addi, 0x38630001);    // addi r3, r3, 1
    I(addx, 0x7C632214);    // add r3, r3, r4
    I(lwz, 0x80A60000);     // lwz r5, 0(r6)
    I(stw, 0x90A60004);     // stw r5, 4(r6)
    c.lr = 0x0000100C;
    ::xf::ppc::dispatch(c, 0x00001100);
    I(fctiwx, 0xFC20101C);  // fctiw f1, f2
    I(faddx, 0xFC82182B);   // fadd. f4, f2, f3
    I(vexptefp, 0x1020118A); // vexptefp v1, v2
    I(vlogefp, 0x106019CA);  // vlogefp v3, v3
    return;
}

// copies r9 bytes from r8 to r7 (a size the compiler does not know: a call
// to memcpy, the table's in a game library)
XF_FN(xf_00001100) {
    ::xf::ppc::lib::memcpy(c.base + uint32_t(c.r[7]), c.base + uint32_t(c.r[8]), size_t(c.r[9]));
    return;
}

// an invalid instruction reached
XF_FN(xf_00001200) {
    ::xf::ppc::invalid(c, 0x00001200);
    return;
}

// A function with a resume site at 0x2010 (a setjmp's caller): it calls
// xf_00002100, which goes r5 frames down and returns to 0x2010 with r1 at
// this function's frame (a longjmp: an Unwind through every frame between),
// and counts the comebacks in r3.
XF_FN(xf_00002000) {
    static constexpr uint32_t xf_sites[] = {0x00002010u};
    ::xf::ppc::resumable(c, XF_RESUMABLE_BODY {
        switch (xf_at) {
        case 0x00002010: goto L_00002010;
        }
        c.r[1] -= 0x40;
        c.r[6] = c.r[1];
        c.lr = 0x00002010;
        xf_00002100(c);
        c.r[3] = 0xBAD; // never: the Unwind came back to 0x2010
    L_00002010:
        c.r[1] += 0x40;
        c.r[3] += 1;
        return;
    }, uint32_t(c.lr), uint32_t(c.r[1]), xf_sites, 1);
}

// r5 frames down (calls that are not tail calls), then LR 0x2010 and r1 back
// to xf_00002000's frame: a checked return that does not go to its caller
XF_FN(xf_00002100) {
    const uint32_t xf_lr = uint32_t(c.lr), xf_sp = uint32_t(c.r[1]);
    if (c.r[5] != 0) {
        c.r[5]--;
        c.r[1] -= 0x20;
        xf_00002100(c);
        c.r[1] += 0x20;
        c.r[10]++;
        XF_CHECKED_RETURN();
    }
    c.lr = 0x00002010;
    c.r[1] = c.r[6];
    XF_CHECKED_RETURN();
}

const ::xf::ppc::GeneratedFunction functions[] = {
    {0x00001000u, xf_00001000}, {0x00001100u, xf_00001100}, {0x00001200u, xf_00001200},
    {0x00002000u, xf_00002000}, {0x00002100u, xf_00002100},
};
const uint32_t resumes[] = {0x00002010u};
extern const ::xf::ppc::GeneratedModule module = {"test.xex", 0x00001000u, 0x00002000u, 0x0123456789ABCDEFull,
                                                  functions, 5, resumes, 1};

} // namespace xfm_test

extern "C" const ::xf::ppc::GeneratedModule *const xf_modules[] = {&xfm_test::module};
extern "C" const size_t xf_module_count = 1;
