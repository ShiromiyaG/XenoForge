// The XAM oracle (tools/xfxam.cpp; docs/runtime.md, "XAM's oracle") on the
// console's own XAM, where XF_XAM_XEX names its file (the system update's
// $flash_xam.xex from the player's disc, 2.0.16202; never in the
// repository): its imports bind, its entry runs to its end on our kernel,
// and xfxam answers a call on both sides, compares them, and keeps a fault
// to the side it happened on. Without XF_XAM_XEX it is skipped.
//
//   xam_oracle_test <xfxam>

#include "check.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

std::string g_xfxam, g_xex;
std::filesystem::path g_work;

std::string quote(const std::string &s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char ch : s) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return out + "'";
#endif
}

// xfxam's output for the arguments after the file, and its exit code
std::string xfxam(const std::string &args, int &code) {
    std::string out = (g_work / "out.txt").string();
    std::string command = quote(g_xfxam) + " " + quote(g_xex) + " " + args + " --work " + quote(g_work.string()) +
                          " > " + quote(out) + " 2>&1";
#ifdef _WIN32
    code = std::system(("\"" + command + "\"").c_str()); // cmd /c strips a line's outer quotes
#else
    int r = std::system(command.c_str());
    code = WIFEXITED(r) ? WEXITSTATUS(r) : -1;
#endif
    std::ifstream f(out, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool has(const std::string &text, const char *part) {
    bool found = text.find(part) != std::string::npos;
    if (!found) std::printf("  no \"%s\" in:\n%s\n", part, text.c_str());
    return found;
}

} // namespace

int main(int argc, char **argv) {
    const char *xex = std::getenv("XF_XAM_XEX");
    if (!xex || !*xex) {
        std::printf("XF_XAM_XEX not set: no XAM to compare with\n");
        return 77; // skipped
    }
    if (argc < 2) {
        std::printf("usage: xam_oracle_test <xfxam>\n");
        return 1;
    }
    g_xfxam = argv[1], g_xex = xex;
    // (a directory of its own: copies of the test may run at the same time)
    g_work = std::filesystem::temp_directory_path() /
             ("xam_oracle_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(g_work, ec);
    int code = 0;

    // every function it imports from the kernel names an export of ours or an
    // unimplemented one, counted
    std::string imports = xfxam("imports", code);
    CHECK(code == 0);
    CHECK(has(imports, "imports: 429 functions"));

    // its entry (DllMain, DLL_PROCESS_ATTACH, on a system thread) returns TRUE
    std::string init = xfxam("init", code);
    CHECK(code == 0);
    CHECK(has(init, "XAM's start: returned 0x00000001"));

    // a call both answer alike: the language (English, the console's default)
    std::string language = xfxam("call XamGetLanguage", code);
    CHECK(code == 0);
    CHECK(has(language, "verdict: matches"));

    // a title running (the oracle's synthetic one, built by xfxam: no game's
    // data): the kernel holds it on both sides, the hard drive's Partition1 is
    // a directory of the host, and the console's XAM has the heap it makes for
    // a title, so a call that allocates on a title's thread works there; with
    // no title (--no-title) it has none, and answers out of memory
    std::string started = xfxam("init", code);
    CHECK(has(started, "the kernel's title: started"));
    CHECK(has(started, "prepared: 0x816E5118 -> 0x00000000"));
    std::string heap = xfxam("call XamAlloc 0 0x100 buf:16", code);
    CHECK(has(heap, "real: returned, r3 0x00000000"));
    CHECK(has(heap, "ours: returned, r3 0x00000000"));
    std::string no_heap = xfxam("call XamAlloc 0 0x100 buf:16 --no-title", code);
    CHECK(has(no_heap, "real: returned, r3 0x8007000E"));
    CHECK(has(no_heap, "verdict: needs state"));
    std::string title_id = xfxam("call XamGetCurrentTitleId", code);
    CHECK(has(title_id, "ours: returned, r3 0x58450001")); // the synthetic title's ID, in the kernel

    // the console's own answer, whatever ours says: the system version XAM
    // reports
    std::string version = xfxam("call XamGetSystemVersion", code);
    CHECK(has(version, "real: returned, r3 0x20076000"));
    CHECK(has(version, "verdict: matches")); // ours takes it since the second survey

    // what the survey found and the HLE took from the console's XAM: these
    // answer alike now (each was a difference)
    for (const char *call :
         {"XamUpdateGetCurrentSystemVersion", "XamIsXbox1TitleId 0", "XamGetLocaleDateFormat 7",
          "XamNotifyCreateListener 0 0", "NetDll_WSAStartup 0 0x0202 buf:512",
          "XamShowMessageBoxUI 0 wstr:T wstr:X 1 buf:16 0 0 buf:16 0", "XamInputGetCapabilities 0 0 buf:32",
          "XamVoiceGetMicArrayStatus",
          // with a title and no user (the third survey)
          "XamGetCurrentTitleId", "XamUserGetSigninInfo 0 0 buf:48", "XamUserIsUnsafeProgrammingAllowed 0 0 buf:16",
          "XamUserContentRestrictionGetFlags 0 buf:16", "XamUserNuiEnableBiometric 0 0"}) {
        std::string answer = xfxam(std::string("call ") + call, code);
        CHECK(has(answer, "verdict: matches"));
    }

    // memory written on both sides, compared: 0x100 random bytes in a buffer
    // differ (each side's own random numbers), and the answer says where
    std::string random = xfxam("call NetDll_XNetRandom 0 buf:0x100 0x100", code);
    CHECK(has(random, "real: returned, r3 0x00000000"));
    CHECK(has(random, "arg 1 +0x")); // (where the difference starts is the numbers' chance)

    // a fault ends only its side's process: a buffer at a page nothing maps
    // (neither XAM checks the pointer), and the comparison still made
    std::string fault = xfxam("call NetDll_XNetRandom 0 0x10 0x100", code);
    CHECK(has(fault, "real: fault writing"));
    CHECK(has(fault, "verdict: faults"));

    std::filesystem::remove_all(g_work, ec);
    return finish("xam_oracle");
}
