// xfshader: a Xenos shader's microcode disassembled as Xenia does it, or
// translated to SPIR-V. Reads Xenia's dumps (--dump_shaders:
// shader_<hash>.ucode.bin.vert|frag, words in host order) or a guest's
// big-endian words with --big-endian.

#include "xenoforge/shader_translator.h"
#include "xenoforge/ucode_parse.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xf;

int main(int argc, char **argv) {
    std::filesystem::path input;
    int type = -1; // 0 vertex, 1 pixel
    bool big_endian = false;
    std::filesystem::path spirv_path;
    std::string stage_name;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--spirv" && i + 1 < argc) spirv_path = std::filesystem::path(argv[++i]);
        else if (a == "--stage" && i + 1 < argc) stage_name = argv[++i];
        else if (a == "--vs") type = 0;
        else if (a == "--ps") type = 1;
        else if (a == "--big-endian") big_endian = true;
        else if (input.empty() && !a.starts_with("--")) input = std::filesystem::path(a);
        else input.clear(), i = argc;
    }
    if (input.empty()) {
        std::printf("usage: xfshader <microcode file> [--vs|--ps] [--big-endian] [--spirv <out.spv> [--stage S]]\n"
                    "  the type comes from the extension (.vert/.vs vertex, .frag/.ps pixel) unless given;\n"
                    "  with --spirv, translated instead of disassembled, to the shader's own stage or to\n"
                    "  --stage vertex|pixel|harness\n");
        return 2;
    }
    if (type < 0) {
        std::string ext = input.extension().string();
        if (ext == ".vert" || ext == ".vs") type = 0;
        else if (ext == ".frag" || ext == ".ps") type = 1;
        else {
            std::fprintf(stderr, "cannot tell vertex from pixel by the extension: give --vs or --ps\n");
            return 2;
        }
    }

    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<uint32_t> dwords(bytes.size() / 4);
    for (size_t i = 0; i < dwords.size(); i++) {
        const uint8_t *p = bytes.data() + 4 * i;
        dwords[i] = big_endian ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
                               : uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    auto shader = ucode::parse_shader(type == 0 ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel, dwords);
    if (!spirv_path.empty()) {
        shader::Stage stage = type == 0 ? shader::Stage::Vertex : shader::Stage::Pixel;
        if (stage_name == "harness") stage = shader::Stage::Harness;
        else if (stage_name == "vertex") stage = shader::Stage::Vertex;
        else if (stage_name == "pixel") stage = shader::Stage::Pixel;
        else if (!stage_name.empty()) {
            std::fprintf(stderr, "unknown stage %s\n", stage_name.c_str());
            return 2;
        }
        auto translation = shader::translate(shader, stage);
        if (!translation.ok()) {
            for (const std::string &u : translation.unsupported) std::fprintf(stderr, "unsupported: %s\n", u.c_str());
            return 3;
        }
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(translation.spirv.data()), std::streamsize(translation.spirv.size() * 4));
        return out ? 0 : 1;
    }
    std::string text = ucode::disassemble(shader);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
