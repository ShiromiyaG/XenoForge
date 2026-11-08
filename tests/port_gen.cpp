// Emits port_test's guest program (port_program.h) at build time with its
// project file, as xfrecomp emit does a module: <out>/m/functions_0.cpp and
// <out>/m/table.cpp, the table as xfm_m::module.
//
//   port_gen <out dir>

#include "port_program.h"

#include <cstdio>

using namespace xf;

int main(int argc, char **argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: port_gen <out dir>\n"), 2;
    recomp::Project project;
    std::string error;
    if (!recomp::parse_project(port_program::PROJECT, ".", project, error))
        return std::fprintf(stderr, "port_gen: %s\n", error.c_str()), 1;
    recomp::Image image = port_program::image();
    recomp::EmitOptions options;
    options.discover.scan_pointers = false;
    options.project = &project;
    recomp::EmittedModule m;
    if (!recomp::emit_image(image, "port_test.xex", image_digest(image.bytes), argv[1], "m", options, m, error))
        return std::fprintf(stderr, "port_gen: %s\n", error.c_str()), 1;
    for (const std::string &w : m.warnings) std::fprintf(stderr, "port_gen: %s\n", w.c_str());
    std::printf("port_gen: %zu functions, %zu hook slots, %zu mid-function, %zu names\n", m.functions, m.hooks,
                m.mid_hooks, m.names);
    return m.files == 1 ? 0 : 1;
}
