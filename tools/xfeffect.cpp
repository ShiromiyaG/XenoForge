// xfeffect: the shaders inside files (XNA's compiled effects, a game's
// effect files or archives), written as Xenia's --dump_shaders writes them:
// <stem>.<n>.ucode.bin.vert|frag, the microcode in host order, so xfshader,
// compare_disasm.ps1 and shader_compare_test (XF_SHADERS) read them alike.

#include "xenoforge/shader_container.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xf;

int main(int argc, char **argv) {
    std::filesystem::path output = ".";
    std::vector<std::filesystem::path> inputs;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else inputs.emplace_back(a);
    }
    if (inputs.empty()) {
        std::printf("usage: xfeffect <file>... [-o <folder>]\n"
                    "  writes each shader container's microcode as <stem>.<n>.ucode.bin.vert|frag\n");
        return 2;
    }
    std::filesystem::create_directories(output);
    int total = 0;
    for (const auto &input : inputs) {
        std::ifstream in(input, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
            return 1;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto shaders = find_shader_containers(data);
        int n = 0;
        for (const ShaderContainer &s : shaders) {
            std::string name = input.stem().string() + "." + std::to_string(n++) + ".ucode.bin." + (s.vertex ? "vert" : "frag");
            std::ofstream out(output / name, std::ios::binary);
            out.write(reinterpret_cast<const char *>(s.microcode.data()), std::streamsize(s.microcode.size() * 4));
            std::printf("%s: %s shader at 0x%zX, %zu words\n", name.c_str(), s.vertex ? "vertex" : "pixel", s.offset,
                        s.microcode.size());
        }
        total += n;
    }
    std::printf("%d shaders\n", total);
    return 0;
}
