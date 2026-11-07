// xfcompat's rows and the parser of boot's output (tools/compat.h), on
// canned output: the summary line, a fault, a stall without a summary, and
// the files' resume.

#include "check.h"

#include "compat.h"

#include <filesystem>
#include <fstream>

using namespace xf;

int main() {
    // a boot that ran to the end of its time
    const char *ok = "storage: x\ngenerated code: 3 modules, 120 functions\n"
                     "xf-summary {\"title_id\":\"4D5307E6\",\"digest\":\"00ff00ff00ff00ff\",\"state\":\"ok\","
                     "\"detail\":\"\",\"modules\":3,\"functions\":120,\"interpreted_entries\":57,"
                     "\"interpreted_addresses\":4,\"swaps\":900,\"draws\":1234,"
                     "\"unimplemented\":[\"XamFoo\",\"XamBar\"],\"gpu\":[\"skipped: no \\\"shader\\\"\"]}\n";
    auto r = compat::parse_boot_output(ok, 0, false);
    CHECK(r.summary);
    CHECK(r.row.state == "ok");
    CHECK(r.row.title_id == "4D5307E6" && r.row.digest == "00ff00ff00ff00ff");
    CHECK(r.row.modules == 3 && r.row.functions == 120);
    CHECK(r.row.interpreted_entries == 57 && r.row.interpreted_addresses == 4);
    CHECK(r.row.swaps == 900 && r.row.draws == 1234);
    CHECK(r.row.first_unimplemented == "XamFoo" && r.row.unimplemented_all == "XamFoo; XamBar");
    CHECK(r.row.first_gpu == "skipped: no \"shader\"");

    // a stall: the summary says so and the threads' stack comes along
    const char *stalled = "stall: no swap for 10.0 s after swap 5 (20.0 s); threads:\n"
                          "  thread 1 (main), x: waiting, pc 0x82000000, lr 0x82000004\n"
                          "    waits for event 0x100\n"
                          "    stack: 0x82001000 0x82002000\n"
                          "  thread 2: running, pc 0, lr 0\n"
                          "    stack: 0x82003000\n"
                          "xf-summary {\"title_id\":\"1\",\"digest\":\"2\",\"state\":\"stall\",\"detail\":\"\","
                          "\"swaps\":5,\"unimplemented\":[],\"gpu\":[]}\n";
    r = compat::parse_boot_output(stalled, 0, false);
    CHECK(r.summary && r.row.state == "stall");
    CHECK(r.row.excerpt.find("stall: no swap") == 0);
    CHECK(r.row.excerpt.find("stack: 0x82001000 0x82002000 | stack: 0x82003000") != std::string::npos);
    CHECK(r.row.first_unimplemented.empty());

    // a crash that took the process: the fault line is the detail
    const char *crashed = "generated code: none\nfault: reading host 0x10 (guest 0x00000010), its guest thread last at "
                          "0x82004000, lr 0x82004004\nmodules:\n";
    r = compat::parse_boot_output(crashed, 3, false);
    CHECK(!r.summary && r.row.state == "crash" && r.row.exit_code == 3);
    CHECK(r.row.detail.find("reading host 0x10") == 0);
    CHECK(r.row.excerpt.find("lr 0x82004004") != std::string::npos);

    // killed at the hard timeout without a word, and a boot that ended silently
    r = compat::parse_boot_output("loaded\n", -1, true);
    CHECK(r.row.state == "stall" && r.row.detail.find("hard timeout") != std::string::npos);
    r = compat::parse_boot_output("hello\n", 0, false);
    CHECK(r.row.state == "no-summary");
    r = compat::parse_boot_output("hello\n", 5, false);
    CHECK(r.row.state == "crash");

    // a row through JSON and back, and its CSV line
    compat::Row row;
    row.disc = "D:\\discs\\a \"b\".iso", row.title = "A, B", row.title_id = "4D5307E6", row.digest = "abc";
    row.state = "crash", row.detail = "line\nbreak \\ \"q\"", row.build_seconds = 12.5, row.cached = true;
    row.library_bytes = 5u << 20, row.left_out = 7, row.swaps = 3, row.first_unimplemented = "XamFoo";
    row.excerpt = "a | b", row.exit_code = 3;
    compat::Row back;
    CHECK(compat::from_json(compat::to_json(row), back));
    CHECK(back.disc == row.disc && back.title == row.title && back.detail == row.detail);
    CHECK(back.build_seconds == 12.5 && back.cached && back.library_bytes == (5u << 20) && back.left_out == 7);
    CHECK(back.swaps == 3 && back.first_unimplemented == "XamFoo" && back.exit_code == 3);
    CHECK(compat::to_json(row).find('\n') == std::string::npos);
    const std::string csv = compat::to_csv(row);
    CHECK(csv.find("\"A, B\"") != std::string::npos && csv.find("\"D:\\discs\\a \"\"b\"\".iso\"") == 0);
    CHECK(csv.find('\n') == std::string::npos);
    CHECK(!compat::from_json("not json", back));

    // the files: rows appended, the discs seen again, a header once, redo clears
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "xfcompat_test";
    std::filesystem::remove_all(dir, ec);
    const compat::Files files = compat::files_for(dir / "run");
    CHECK(compat::done_discs(files).empty());
    CHECK(compat::append_row(files, row));
    row.disc = "other.iso";
    CHECK(compat::append_row(files, row));
    auto done = compat::done_discs(files);
    CHECK(done.size() == 2 && done.count("other.iso") && done.count("D:\\discs\\a \"b\".iso"));
    std::ifstream in(files.csv);
    std::string line;
    int lines = 0, headers = 0;
    while (std::getline(in, line)) lines++, headers += line.starts_with("disc,title");
    CHECK(lines == 3 && headers == 1);
    compat::clear(files);
    CHECK(compat::done_discs(files).empty());
    std::filesystem::remove_all(dir, ec);
    return finish("compat_test");
}
