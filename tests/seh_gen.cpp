// Recompiles kernel_exceptions_test's guest program (seh_program.h) at build
// time, as xfrecomp emits a module: discovery from its .pdata and imports,
// then every function but the host-coded C++ handler, with the table of
// functions and resume sites the test hands the kernel.
//
//   seh_gen <out.cpp>

#include "seh_program.h"
#include "xenoforge/recomp.h"

#include <cstdio>
#include <fstream>
#include <set>

using namespace xf;

int main(int argc, char **argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: seh_gen <out.cpp>\n"), 2;
    seh::Program p = seh::build_program();
    recomp::Image image;
    image.base = seh::BASE;
    image.bytes = p.image;
    image.sections.push_back({".text", seh::BASE, seh::DATA - seh::BASE, true, false});
    image.sections.push_back({".data", seh::DATA, seh::SIZE - (seh::DATA - seh::BASE), false, true});
    for (const seh::Function &f : p.functions) image.pdata.push_back({f.begin, f.end - f.begin, f.prolog, f.exception});
    for (const seh::Import &i : seh::IMPORTS) image.imports.push_back({"xboxkrnl.exe", i.ordinal, i.thunk});
    recomp::Discovery d = recomp::discover(image, {.scan_pointers = false});

    const uint32_t host = p.at("seh_cxx_handler");
    std::string declarations, bodies, table;
    std::set<uint32_t> resumes;
    size_t count = 0;
    for (const auto &[a, f] : d.functions) {
        if (a == host || f.source == recomp::Source::Import) continue;
        std::string name = recomp::function_name("xf_", a);
        declarations += "XF_FN(" + name + ");\n";
        bodies += recomp::emit_function(image, d, f, "xf_") + "\n";
        table += "    {0x" + recomp::function_name("", a) + "u, xfm_seh::" + name + "},\n";
        resumes.insert(f.resumes.begin(), f.resumes.end());
        count++;
    }
    std::string resume_list;
    for (uint32_t a : resumes) resume_list += "0x" + recomp::function_name("", a) + "u, ";
    if (resumes.empty()) resume_list = "0";

    std::ofstream out(argv[1]);
    out << "// Written by seh_gen: kernel_exceptions_test's guest program, recompiled.\n"
           "#include \"xenoforge/ppc_generated.h\"\n#include <cstddef>\n\nnamespace xfm_seh {\n\n"
        << declarations << "\n"
        << bodies << "} // namespace xfm_seh\n\n"
        << "extern const xf::ppc::GeneratedFunction xf_seh_functions[] = {\n"
        << table << "};\nextern const size_t xf_seh_function_count = " << count << ";\n"
        << "extern const uint32_t xf_seh_resumes[] = {" << resume_list << "};\n"
        << "extern const size_t xf_seh_resume_count = " << resumes.size() << ";\n";
    std::printf("seh_gen: %zu functions, %zu resume sites\n", count, resumes.size());
    return out ? 0 : 1;
}
