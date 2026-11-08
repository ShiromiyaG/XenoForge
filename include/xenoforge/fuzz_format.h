// The differential fuzzer's files (docs/recompiler.md, "The differential
// fuzzer"): cases of random instruction sequences with their registers and
// memory before, and the state each runner (the interpreter, the generated
// code, Xenia's JIT through tools/xenia_oracle/ppc_exec_oracle.cc) left after.
//
// A case's code sits at CODE_BASE + index * CASE_STRIDE and ends with blr; it
// returns to LR = SENTINEL. Its memory is the window WINDOW..WINDOW + WINDOW_SIZE
// (inside Xenia's test heap at 0x10001000), and r1 points at POINTER in it: every
// load and store the generator makes is r1-relative.
//
// Cases file: "XFFUZZC1", a 32-bit count, then per case a 32-bit word count, the
// words, and the State before. States file: "XFFUZZS1", a 32-bit count, then per
// case a 32-bit status (Status) and the State after. Little-endian, as the
// structures lie in memory. Header-only and plain C++, so the oracle (MSVC,
// against Xenia) uses it too.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xf::ppc::fuzz {

constexpr uint32_t CODE_BASE = 0x80000000;
constexpr uint32_t CASE_STRIDE = 0x40; // 15 instructions and the blr
constexpr uint32_t MAX_WORDS = CASE_STRIDE / 4 - 1;
constexpr uint32_t SENTINEL = 0xBCBCBCBC;
constexpr uint32_t WINDOW = 0x10007C00, WINDOW_SIZE = 0x800, POINTER = 0x10008000;
constexpr uint64_t MSR = 0x9030;

struct State {
    uint64_t r[32];
    uint64_t f[32]; // the bits
    uint32_t v[128][4];  // words in guest order
    uint32_t cr;
    uint32_t fpscr;
    uint8_t xer_ca, xer_ov, xer_so, vscr_sat;
    uint32_t vscr_nj; // 1: non-Java mode (denormals flushed)
    uint64_t lr, ctr, msr;
    uint8_t memory[WINDOW_SIZE];
};

enum Status : uint32_t {
    RAN = 0,
    UNSUPPORTED = 1, // the runner has no semantics for an instruction in it
    STOPPED = 2,     // did not return, or faulted
};

struct Case {
    std::vector<uint32_t> code; // without the blr
    State in;
};

struct Result {
    uint32_t status = RAN;
    State out;
};

inline uint32_t case_address(size_t index) { return CODE_BASE + uint32_t(index) * CASE_STRIDE; }
constexpr uint32_t BLR = 0x4E800020;

// the code image: each case's words and a blr, big-endian, at its address
inline std::vector<uint8_t> code_image(const std::vector<Case> &cases) {
    std::vector<uint8_t> image(cases.size() * CASE_STRIDE, 0);
    for (size_t k = 0; k < cases.size(); k++) {
        uint8_t *p = image.data() + k * CASE_STRIDE;
        auto put = [&](uint32_t w) {
            p[0] = uint8_t(w >> 24), p[1] = uint8_t(w >> 16), p[2] = uint8_t(w >> 8), p[3] = uint8_t(w), p += 4;
        };
        for (uint32_t w : cases[k].code) put(w);
        put(BLR);
    }
    return image;
}

inline bool write_cases(const char *path, const std::vector<Case> &cases) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t n = uint32_t(cases.size());
    bool ok = std::fwrite("XFFUZZC1", 1, 8, f) == 8 && std::fwrite(&n, 4, 1, f) == 1;
    for (const Case &c : cases) {
        uint32_t words = uint32_t(c.code.size());
        ok = ok && std::fwrite(&words, 4, 1, f) == 1 && std::fwrite(c.code.data(), 4, words, f) == words &&
             std::fwrite(&c.in, sizeof(State), 1, f) == 1;
    }
    return std::fclose(f) == 0 && ok;
}

inline bool read_cases(const char *path, std::vector<Case> &cases) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "XFFUZZC1", 8) == 0 && std::fread(&n, 4, 1, f) == 1;
    cases.clear();
    for (uint32_t k = 0; ok && k < n; k++) {
        Case c;
        uint32_t words = 0;
        ok = std::fread(&words, 4, 1, f) == 1 && words <= MAX_WORDS;
        if (!ok) break;
        c.code.resize(words);
        ok = std::fread(c.code.data(), 4, words, f) == words && std::fread(&c.in, sizeof(State), 1, f) == 1;
        if (ok) cases.push_back(std::move(c));
    }
    std::fclose(f);
    return ok;
}

inline bool write_results(const char *path, const std::vector<Result> &results) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t n = uint32_t(results.size());
    bool ok = std::fwrite("XFFUZZS1", 1, 8, f) == 8 && std::fwrite(&n, 4, 1, f) == 1;
    for (const Result &r : results)
        ok = ok && std::fwrite(&r.status, 4, 1, f) == 1 && std::fwrite(&r.out, sizeof(State), 1, f) == 1;
    return std::fclose(f) == 0 && ok;
}

inline bool read_results(const char *path, std::vector<Result> &results) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "XFFUZZS1", 8) == 0 && std::fread(&n, 4, 1, f) == 1;
    results.assign(ok ? n : 0, Result{});
    for (uint32_t k = 0; ok && k < n; k++)
        ok = std::fread(&results[k].status, 4, 1, f) == 1 && std::fread(&results[k].out, sizeof(State), 1, f) == 1;
    std::fclose(f);
    return ok;
}

} // namespace xf::ppc::fuzz
