// A port's view of the shader translator (xenoforge/native_shaders.h):
// - translate_shader's hash is the renderer's (the shader storage's), per stage;
// - a ShaderCache translates a shader once: a second cache on the same
//   directory reads it (the same SPIR-V, or the same why), and nothing is
//   kept in a folder of another translator version;
// - bake_shaders translates what a shader storage holds into a cache's
//   directory, and a second bake has nothing left to do;
// - with XF_SHADERS (a directory of shader dumps, shader_<hash>.ucode.bin.vert
//   or .frag, as shader_compare_test reads), every one of them through the
//   cache: translated, the cache's SPIR-V a SPIR-V module.

#include "check.h"

#include "xenoforge/native_shaders.h"
#include "xenoforge/shader_storage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace xf;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char *name) {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec) / name;
    fs::remove_all(p, ec);
    return p;
}

bool same(const native::TranslatedShader &a, const native::TranslatedShader &b) {
    return a.hash == b.hash && a.spirv == b.spirv && a.ok() == b.ok() && (a.ok() || !b.why.empty());
}

// the cache over one shader, twice: translated by the first cache, read by the second
void cache_round_trip(const std::vector<uint32_t> &code, native::ShaderStage stage, const fs::path &dir,
                      bool must_translate) {
    native::ShaderCache first(dir);
    const native::TranslatedShader &t = first.get(code.data(), code.size(), stage);
    CHECK(t.hash == native::shader_hash(code.data(), code.size(), stage));
    CHECK(t.hash == ShaderStorage::hash(stage == native::ShaderStage::Vertex ? 0 : 1, code));
    CHECK(&first.get(code.data(), code.size(), stage) == &t); // met once
    CHECK(first.stats().translated == 1 && first.stats().read == 0);
    if (must_translate) {
        CHECK(t.ok());
        CHECK(t.spirv.size() > 5 && t.spirv[0] == 0x07230203u); // SPIR-V's magic
    }
    native::ShaderCache second(dir);
    const native::TranslatedShader &r = second.get(code.data(), code.size(), stage);
    CHECK(second.stats().read == 1 && second.stats().translated == 0);
    CHECK(same(t, r));
    // the constants it reads: the same from the translation, the parse alone and the cache's read
    uint64_t used[4];
    CHECK(native::used_float_constants(code.data(), code.size(), stage, used));
    for (int k = 0; k < 4; k++) CHECK(used[k] == t.float_constants[k] && used[k] == r.float_constants[k]);
    native::ShaderUses uses;
    CHECK(native::shader_uses(code.data(), code.size(), stage, uses));
    for (int k = 0; k < 4; k++) CHECK(uses.float_constants[k] == used[k]);
    // a stride only for a vertex fetch constant it names
    for (int f = 0; f < 96; f++)
        if (uses.vertex_strides[f]) CHECK(uses.vertex_fetches[f / 32] & (1u << (f % 32)));
    // a vertex shader's rectangle list form: kept apart from its plain one
    if (stage == native::ShaderStage::Vertex) {
        const native::TranslatedShader &r =
            first.get(code.data(), code.size(), stage, native::VertexForm::RectangleList);
        CHECK(&r != &t && r.hash == t.hash);
        CHECK(&first.get(code.data(), code.size(), stage, native::VertexForm::RectangleList) == &r);
    }
    // kept under the translator's version only
    CHECK(fs::is_directory(dir / native::translator_version()));
}

} // namespace

int main() {
    // a made-up microcode (what it translates to, or why not, does not matter here: the cache keeps either)
    const std::vector<uint32_t> made_up = {0x00000000u, 0x00001000u, 0x00000000u, 0, 0, 0};
    {
        const fs::path dir = scratch("xf_native_shaders_cache");
        cache_round_trip(made_up, native::ShaderStage::Pixel, dir, false);
        cache_round_trip(made_up, native::ShaderStage::Vertex, scratch("xf_native_shaders_cache_vs"), false);
        // the stage is part of the key
        CHECK(native::shader_hash(made_up.data(), made_up.size(), native::ShaderStage::Vertex) !=
              native::shader_hash(made_up.data(), made_up.size(), native::ShaderStage::Pixel));
    }
    // the bake from a shader storage
    {
        const fs::path root = scratch("xf_native_shaders_bake");
        fs::create_directories(root);
        const fs::path storage = root / "58460001.xfshaders", dir = root / "cache";
        {
            ShaderStorage s;
            CHECK(s.open(storage, true));
            CHECK(s.add_shader(1, ShaderStorage::hash(1, made_up), made_up));
        }
        std::string why;
        CHECK(native::bake_shaders(storage, dir, why) == 1);
        CHECK(native::bake_shaders(storage, dir, why) == 0); // baked already
        native::ShaderCache cache(dir);
        cache.get(made_up.data(), made_up.size(), native::ShaderStage::Pixel);
        CHECK(cache.stats().read == 1 && cache.stats().translated == 0);
        CHECK(native::bake_shaders(root / "none.xfshaders", dir, why) == -1 && !why.empty());
    }
    // a corpus of real shaders, when given
    if (const char *corpus = std::getenv("XF_SHADERS"); corpus && *corpus) {
        const fs::path dir = scratch("xf_native_shaders_corpus");
        size_t n = 0, with_constants = 0;
        for (const auto &e : fs::directory_iterator(corpus)) {
            const std::string name = e.path().filename().string();
            const bool vert = name.ends_with(".ucode.bin.vert"), frag = name.ends_with(".ucode.bin.frag");
            if (!vert && !frag) continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<uint32_t> code(bytes.size() / 4);
            std::memcpy(code.data(), bytes.data(), code.size() * 4);
            const native::ShaderStage stage = vert ? native::ShaderStage::Vertex : native::ShaderStage::Pixel;
            cache_round_trip(code, stage, dir, true); // (the dumps' names are Xenia's hashes, not ours)
            uint64_t used[4];
            native::used_float_constants(code.data(), code.size(), stage, used);
            if (used[0] | used[1] | used[2] | used[3]) with_constants++;
            n++;
        }
        std::printf("native_shaders: %zu shaders of %s through the cache, %zu reading float constants\n", n, corpus,
                    with_constants);
        CHECK(with_constants > 0);
    }
    return finish("native_shaders");
}
