// What xfcompat (the batch compatibility run, plan D5) keeps apart from the
// process handling, so the tests can run it: the row written for each disc,
// the files it goes to (JSON lines, which the run resumes from, and a CSV
// to read), and the parser of boot's output (its "xf-summary" line, else the
// fault and stall reports it printed).
#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace xf::compat {

// How a disc ended: ok (running at the end of the time, swapping), stall
// (no swap for a while), stopped (the kernel stopped: a bug check, an
// unimplemented thing it cannot go on without), ended (the main thread
// returned), crash (a fault, or the process died), build-failed, no-summary
// (boot ended without its summary line).
struct Row {
    std::string disc; // the path as given: what the run resumes by
    std::string title, title_id, digest;
    std::string state, detail; // how it ended, and why (stop reason, fault, build error)
    double build_seconds = 0;  // 0 with cached true
    bool cached = false;       // the library was in the cache already
    uint64_t library_bytes = 0;
    int64_t left_out = -1; // functions left out at build (-1: not reported)
    uint64_t modules = 0, functions = 0;
    uint64_t interpreted_entries = 0, interpreted_addresses = 0;
    uint64_t swaps = 0, draws = 0;
    std::string first_unimplemented;
    std::string first_gpu; // the first GPU gap the renderer counted ("skipped: reason", "approx: what")
    std::string unimplemented_all, gpu_all; // "; " joined
    std::string excerpt;                    // the guest stack excerpt of a stall or crash
    double boot_seconds = 0;
    int exit_code = 0;
};

// the line, and a row back from it
std::string to_json(const Row &row);
bool from_json(std::string_view line, Row &row);
// the CSV's header and a row's line
std::string csv_header();
std::string to_csv(const Row &row);

// What boot printed, as far as the run needs it.
struct BootResult {
    bool summary = false; // the xf-summary line was there
    Row row;              // title_id, digest, state, detail, counts, unimplemented, gpu, excerpt
};
// Reads boot's output (stdout and stderr together) and its exit code. A
// missing summary (a crash that took the process, a timeout) is reported
// from the fault line and stack lines if any, else as "crash" (or
// "no-summary" with exit code 0).
BootResult parse_boot_output(std::string_view output, int exit_code, bool timed_out);

// The files of a run: <base>.jsonl and <base>.csv.
struct Files {
    std::filesystem::path jsonl, csv;
};
Files files_for(const std::filesystem::path &base);
// the discs with a row already in the file (by the path in each row)
std::set<std::string> done_discs(const Files &files);
// one row appended to both (the CSV's header written when it is new); false when not writable
bool append_row(const Files &files, const Row &row);
// the rows deleted (--redo)
void clear(const Files &files);

} // namespace xf::compat
