// A game's own shaders for a port's renderer (xenoforge/native_shaders.h):
// the emulated path's parser and translator behind the port's interface.

#include "xenoforge/native_shaders.h"

#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/shader_translator.h"
#include "xenoforge/ucode_parse.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <unordered_map>

namespace xf::native {

// the port's view of the constants is the translator's (shader_harness.h)
static_assert(sizeof(ShaderConstants) == sizeof(harness::Constants));
static_assert(offsetof(ShaderConstants, bool_constants) == offsetof(harness::Constants, bool_constants));
static_assert(offsetof(ShaderConstants, loop_constants) == offsetof(harness::Constants, loop_constants));
static_assert(offsetof(ShaderConstants, fetch_constants) == offsetof(harness::Constants, fetch_constants));

const char *translator_version() { return XF_TRANSLATOR_VERSION; }

uint64_t shader_hash(const uint32_t *microcode, size_t words, ShaderStage stage) {
    // the renderer's (and the storage's): FNV-1a over the words' bytes, seeded with the type
    return ShaderStorage::hash(stage == ShaderStage::Vertex ? 0u : 1u, std::span<const uint32_t>(microcode, words));
}

namespace {
void mark_constants(const ucode::ParsedShader &parsed, uint64_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    auto mark = [&](const ucode::Operand &o) {
        if (o.source != ucode::StorageSource::ConstantFloat) return;
        if (o.addressing != ucode::StorageAddressing::Absolute)
            out[0] = out[1] = out[2] = out[3] = ~0ull; // [n+a0], [n+aL]: any of them
        else if (o.index < 256)
            out[o.index / 64] |= 1ull << (o.index % 64);
    };
    for (const ucode::ControlFlow &cf : parsed.control_flow) {
        const auto *exec = std::get_if<ucode::Exec>(&cf.instruction);
        if (!exec) continue;
        for (const ucode::ExecSlot &slot : exec->slots) {
            const auto *alu = std::get_if<ucode::AluInstruction>(&slot.instruction);
            if (!alu) continue;
            for (uint32_t k = 0; k < alu->vector_operand_count && k < 3; k++) mark(alu->vector_operands[k]);
            for (uint32_t k = 0; k < alu->scalar_operand_count && k < 2; k++) mark(alu->scalar_operands[k]);
        }
    }
}
} // namespace

bool used_float_constants(const uint32_t *microcode, size_t words, ShaderStage stage, uint64_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!microcode || !words) return false;
    const ucode::ParsedShader parsed = ucode::parse_shader(
        stage == ShaderStage::Vertex ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel,
        std::span<const uint32_t>(microcode, words));
    mark_constants(parsed, out);
    return true;
}

TranslatedShader translate_shader(const uint32_t *microcode, size_t words, ShaderStage stage) {
    TranslatedShader out;
    out.hash = shader_hash(microcode, words, stage);
    if (!microcode || !words) {
        out.why = "no microcode";
        return out;
    }
    const ucode::ParsedShader parsed =
        ucode::parse_shader(stage == ShaderStage::Vertex ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel,
                            std::span<const uint32_t>(microcode, words));
    mark_constants(parsed, out.float_constants);
    shader::Translation t =
        shader::translate(parsed, stage == ShaderStage::Vertex ? shader::Stage::Vertex : shader::Stage::Pixel);
    out.approximate = std::move(t.approximate);
    if (!t.unsupported.empty()) {
        for (size_t i = 0; i < t.unsupported.size(); i++) out.why += (i ? "; " : "") + t.unsupported[i];
        return out;
    }
    if (t.spirv.empty()) {
        out.why = "the translator gave no SPIR-V";
        return out;
    }
    out.spirv = std::move(t.spirv);
    return out;
}

namespace {

std::filesystem::path file_of(const std::filesystem::path &base, uint64_t hash, ShaderStage stage, const char *ext) {
    const std::filesystem::path dir = base / translator_version();
    char name[48];
    std::snprintf(name, sizeof(name), "%016llx.%s.%s", (unsigned long long)hash,
                  stage == ShaderStage::Vertex ? "vs" : "ps", ext);
    return dir / name;
}

// the SPIR-V (or the why) written beside the others: a temporary file renamed, so a reader never sees half
void write_entry(const std::filesystem::path &dir, const TranslatedShader &t, ShaderStage stage) {
    std::error_code ec;
    const std::filesystem::path final = file_of(dir, t.hash, stage, t.ok() ? "spv" : "txt");
    std::filesystem::create_directories(final.parent_path(), ec);
    const std::filesystem::path temp = final.string() + ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        if (t.ok())
            f.write(reinterpret_cast<const char *>(t.spirv.data()), std::streamsize(t.spirv.size() * 4));
        else
            f << t.why;
        if (!f) return;
    }
    std::filesystem::rename(temp, final, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

// what the directory holds for the shader: true when it has it (translated or not)
bool read_entry(const std::filesystem::path &dir, uint64_t hash, ShaderStage stage, TranslatedShader &out) {
    out.hash = hash;
    if (std::ifstream f(file_of(dir, hash, stage, "spv"), std::ios::binary); f) {
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() % 4) return false;
        out.spirv.resize(bytes.size() / 4);
        std::memcpy(out.spirv.data(), bytes.data(), bytes.size());
        return true;
    }
    if (std::ifstream f(file_of(dir, hash, stage, "txt")); f) {
        out.why.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (out.why.empty()) out.why = "did not translate";
        return true;
    }
    return false;
}

} // namespace

struct ShaderCache::State {
    std::filesystem::path directory;
    mutable std::mutex mutex;
    // by hash and stage; node-based, so references stay valid
    std::unordered_map<uint64_t, TranslatedShader> shaders[2];
    Stats stats;
};

ShaderCache::ShaderCache(std::filesystem::path directory) : state_(std::make_unique<State>()) {
    state_->directory = std::move(directory);
}
ShaderCache::~ShaderCache() = default;

const TranslatedShader &ShaderCache::get(const uint32_t *microcode, size_t words, ShaderStage stage) {
    const uint64_t hash = shader_hash(microcode, words, stage);
    auto &map = state_->shaders[stage == ShaderStage::Vertex ? 0 : 1];
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (auto it = map.find(hash); it != map.end()) return it->second;
    }
    // read or translated outside the lock (a translation takes a while); a
    // second thread on the same shader does the same work, and the first one
    // in keeps its result
    TranslatedShader t;
    bool read = !state_->directory.empty() && read_entry(state_->directory, hash, stage, t);
    if (read) used_float_constants(microcode, words, stage, t.float_constants); // (not kept on disk)
    if (!read) {
        t = translate_shader(microcode, words, stage);
        if (!state_->directory.empty()) write_entry(state_->directory, t, stage);
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto [it, made] = map.emplace(hash, std::move(t));
    if (made) {
        if (read)
            state_->stats.read++;
        else
            state_->stats.translated++;
        if (!it->second.ok()) state_->stats.failed++;
    }
    return it->second;
}

ShaderCache::Stats ShaderCache::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

long bake_shaders(const std::filesystem::path &storage, const std::filesystem::path &directory, std::string &why) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(storage, ec)) return why = "no " + storage.string(), -1;
    ShaderStorage s;
    if (!s.open(storage, false)) return why = "cannot read " + storage.string(), -1;
    long written = 0;
    for (const ShaderStorage::Shader &shader : s.shaders()) {
        const ShaderStage stage = shader.type == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
        TranslatedShader t;
        if (read_entry(directory, shader.hash, stage, t)) continue; // baked already
        t = translate_shader(shader.code.data(), shader.code.size(), stage);
        write_entry(directory, t, stage);
        written++;
    }
    return written;
}

} // namespace xf::native
