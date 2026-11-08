// The shader and pipeline storage (shader_storage.h): records appended and
// read back, each once; a tail cut short left out and written over; a file
// of another version started again.

#include "xenoforge/shader_storage.h"

#include "check.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace xf;
namespace fs = std::filesystem;

int main() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "xf_shader_storage_test";
    fs::remove_all(dir, ec);
    const fs::path file = ShaderStorage::path(dir, 0x475807D7);
    CHECK(file.filename() == "475807D7.xfshaders");
    CHECK(ShaderStorage::pipeline_cache_path(dir, 0x475807D7).filename() == "475807D7.vkpipelines");

    const std::vector<uint32_t> vs = {1, 2, 3, 4}, ps = {5, 6, 7};
    const uint64_t vh = ShaderStorage::hash(0, vs), ph = ShaderStorage::hash(1, ps);
    {
        ShaderStorage s;
        CHECK(s.open(file));
        CHECK(s.add_shader(0, vh, vs));
        CHECK(!s.add_shader(0, vh, vs)); // once
        CHECK(s.add_shader(1, ph, ps));
        CHECK(s.add_pipeline(std::vector<uint32_t>{1, 9, 9}));
        CHECK(!s.add_pipeline(std::vector<uint32_t>{1, 9, 9}));
        ShaderStorage::Built b;
        b.vendor = 0x10DE, b.device = 0x2D05, b.driver = 7;
        b.uuid[3] = 0xAB;
        b.libraries = {11, 12};
        b.pipelines = {0x123456789ull};
        s.add_built(b);
    }
    {
        ShaderStorage s;
        CHECK(s.open(file, false));
        auto shaders = s.shaders();
        CHECK(shaders.size() == 2);
        CHECK(shaders.size() == 2 && shaders[0].hash == vh && shaders[0].code == vs && shaders[1].type == 1);
        CHECK(s.has_shader(ph));
        CHECK(s.pipelines().size() == 1);
        auto built = s.built();
        CHECK(built.size() == 1);
        CHECK(built.size() == 1 && built[0].vendor == 0x10DE && built[0].uuid[3] == 0xAB &&
              built[0].libraries == std::vector<uint64_t>({11, 12}) &&
              built[0].pipelines == std::vector<uint64_t>({0x123456789ull}));
    }
    // a record cut short: left out, the file made whole again and appended to
    {
        const auto size = fs::file_size(file, ec);
        std::ofstream(file, std::ios::binary | std::ios::app).write("\x01\x00\x00\x00\x09\x00\x00\x00\x01", 9);
        ShaderStorage s;
        CHECK(s.open(file));
        CHECK(fs::file_size(file, ec) == size);
        CHECK(s.add_pipeline(std::vector<uint32_t>{1, 8}));
        ShaderStorage again;
        CHECK(again.open(file, false));
        CHECK(again.pipelines().size() == 2 && again.shaders().size() == 2);
    }
    // another version: started again
    {
        std::ofstream(file, std::ios::binary).write("XFSS\x07\x00\x00\x00", 8);
        ShaderStorage s;
        CHECK(s.open(file));
        CHECK(s.shaders().empty() && s.pipelines().empty());
        CHECK(fs::file_size(file, ec) == 8);
    }
    fs::remove_all(dir, ec);
    return finish("shader_storage");
}
