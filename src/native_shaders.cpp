// A game's own shaders for a port's renderer (xenoforge/native_shaders.h):
// the emulated path's parser and translator behind the port's interface.

#include "xenoforge/native_shaders.h"

#include "xenoforge/shader_harness.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/shader_translator.h"
#include "xenoforge/ucode.h"
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

bool shader_uses(const uint32_t *microcode, size_t words, ShaderStage stage, ShaderUses &out) {
    out = {};
    if (!microcode || !words) return false;
    const ucode::ParsedShader parsed =
        ucode::parse_shader(stage == ShaderStage::Vertex ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel,
                            std::span<const uint32_t>(microcode, words));
    mark_constants(parsed, out.float_constants);
    for (const ucode::ControlFlow &cf : parsed.control_flow) {
        const auto *exec = std::get_if<ucode::Exec>(&cf.instruction);
        if (!exec) continue;
        for (const ucode::ExecSlot &slot : exec->slots) {
            if (const auto *t = std::get_if<ucode::TextureFetchInstruction>(&slot.instruction)) {
                if (t->operand_count <= 1) continue;
                const uint32_t s = t->operands[1].index & 31;
                out.texture_slots |= 1u << s;
                // (the renderer's Impl::analyze: the first fetch's filters, per slot)
                if (t->opcode == ucode::FetchOpcode::TextureFetch && !((out.filter_slots >> s) & 1)) {
                    out.filter_slots |= 1u << s;
                    out.slot_filters[s][0] = uint8_t(t->mag_filter), out.slot_filters[s][1] = uint8_t(t->min_filter);
                    out.slot_filters[s][2] = uint8_t(t->mip_filter), out.slot_filters[s][3] = uint8_t(t->aniso_filter);
                }
            } else if (const auto *v = std::get_if<ucode::VertexFetchInstruction>(&slot.instruction)) {
                const uint32_t f = v->operands[1].index;
                if (f >= 96) continue;
                out.vertex_fetches[f / 32] |= 1u << (f % 32);
                if (!v->is_mini_fetch && v->stride > out.vertex_strides[f]) out.vertex_strides[f] = uint8_t(v->stride);
            } else if (const auto *a = std::get_if<ucode::AluInstruction>(&slot.instruction)) {
                for (const ucode::Result *r : {&a->vector_and_constant_result, &a->scalar_result}) {
                    if (!r->used_write_mask()) continue;
                    if (r->target == ucode::StorageTarget::Color) out.colors_written |= uint8_t(1u << (r->index & 3));
                    if (r->target == ucode::StorageTarget::Depth) out.writes_depth = true;
                    if (r->target == ucode::StorageTarget::ExportData && r->index < 5) out.exports = true;
                }
            }
        }
    }
    out.kills = ucode::analyze(std::span<const uint32_t>(microcode, words)).kills;
    return true;
}

bool used_float_constants(const uint32_t *microcode, size_t words, ShaderStage stage, uint64_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!microcode || !words) return false;
    const ucode::ParsedShader parsed =
        ucode::parse_shader(stage == ShaderStage::Vertex ? ucode::ShaderType::Vertex : ucode::ShaderType::Pixel,
                            std::span<const uint32_t>(microcode, words));
    mark_constants(parsed, out);
    return true;
}

namespace {
shader::VertexOptions vertex_options(const ShaderVariant &v) {
    shader::VertexOptions o;
    o.host = v.form == VertexForm::RectangleList ? shader::HostVertex::RectangleListAsTriangleStrip
             : v.form == VertexForm::PointList   ? shader::HostVertex::PointListAsTriangleStrip
                                                 : shader::HostVertex::Vertex;
    o.clip_planes = v.clip_planes, o.clip_planes_cull = v.clip_planes_cull, o.point_ucp_mode = v.point_ucp_mode;
    return o;
}
shader::PixelOptions pixel_options(const ShaderVariant &v) {
    shader::PixelOptions o;
    o.depth_float24 = v.depth_float24, o.centroid = v.centroid, o.point_coordinates = v.point_coordinates;
    o.early_depth_stencil = v.early_depth_stencil, o.blend_premultiply = v.blend_premultiply;
    return o;
}
} // namespace

TranslatedShader translate_shader(const uint32_t *microcode, size_t words, ShaderStage stage,
                                  const ShaderVariant &variant) {
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
    shader::Translation t = stage == ShaderStage::Vertex
                                ? shader::translate(parsed, shader::Stage::Vertex, vertex_options(variant))
                                : shader::translate(parsed, shader::Stage::Pixel, {}, pixel_options(variant));
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

TranslatedShader translate_shader(const uint32_t *microcode, size_t words, ShaderStage stage, VertexForm form) {
    ShaderVariant v;
    if (stage == ShaderStage::Vertex) v.form = form;
    return translate_shader(microcode, words, stage, v);
}

namespace {

// <hash>.vs / .ps for the plain forms (the bake's), <hash>.rects.vs for a rectangle list's, and
// <hash>.vs.<key> / .ps.<key> for the other variants
std::filesystem::path file_of(const std::filesystem::path &base, uint64_t hash, ShaderStage stage, const char *ext,
                              uint32_t key = 0) {
    const std::filesystem::path dir = base / translator_version();
    const bool vertex = stage == ShaderStage::Vertex;
    char name[64];
    if (key == 0 || (vertex && key == uint32_t(VertexForm::RectangleList)))
        std::snprintf(name, sizeof(name), "%016llx.%s.%s", (unsigned long long)hash,
                      !vertex ? "ps" : key ? "rects.vs" : "vs", ext);
    else
        std::snprintf(name, sizeof(name), "%016llx.%s.%08x.%s", (unsigned long long)hash, vertex ? "vs" : "ps",
                      unsigned(key), ext);
    return dir / name;
}

// the SPIR-V (or the why) written beside the others: a temporary file renamed, so a reader never sees half
void write_entry(const std::filesystem::path &dir, const TranslatedShader &t, ShaderStage stage, uint32_t key = 0) {
    std::error_code ec;
    const std::filesystem::path final = file_of(dir, t.hash, stage, t.ok() ? "spv" : "txt", key);
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
bool read_entry(const std::filesystem::path &dir, uint64_t hash, ShaderStage stage, TranslatedShader &out,
                uint32_t key = 0) {
    out.hash = hash;
    if (std::ifstream f(file_of(dir, hash, stage, "spv", key), std::ios::binary); f) {
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() % 4) return false;
        out.spirv.resize(bytes.size() / 4);
        std::memcpy(out.spirv.data(), bytes.data(), bytes.size());
        return true;
    }
    if (std::ifstream f(file_of(dir, hash, stage, "txt", key)); f) {
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
    // by hash, stage and variant key; node-based, so references stay valid
    struct Key {
        uint64_t hash;
        uint32_t stage, variant;
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            return size_t(k.hash ^ (uint64_t(k.variant) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.stage) << 63));
        }
    };
    std::unordered_map<Key, TranslatedShader, KeyHash> shaders;
    Stats stats;
};

ShaderCache::ShaderCache(std::filesystem::path directory) : state_(std::make_unique<State>()) {
    state_->directory = std::move(directory);
}
ShaderCache::~ShaderCache() = default;

const TranslatedShader &ShaderCache::get(const uint32_t *microcode, size_t words, ShaderStage stage, VertexForm form) {
    ShaderVariant v;
    if (stage == ShaderStage::Vertex) v.form = form;
    return get(microcode, words, stage, v);
}

const TranslatedShader &ShaderCache::get(const uint32_t *microcode, size_t words, ShaderStage stage,
                                         const ShaderVariant &variant) {
    const uint64_t hash = shader_hash(microcode, words, stage);
    const uint32_t key = variant.key(stage == ShaderStage::Vertex);
    const State::Key k{hash, uint32_t(stage), key};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (auto it = state_->shaders.find(k); it != state_->shaders.end()) return it->second;
    }
    // read or translated outside the lock (a translation takes a while); a
    // second thread on the same shader does the same work, and the first one
    // in keeps its result
    TranslatedShader t;
    bool read = !state_->directory.empty() && read_entry(state_->directory, hash, stage, t, key);
    if (read) used_float_constants(microcode, words, stage, t.float_constants); // (not kept on disk)
    if (!read) {
        t = translate_shader(microcode, words, stage, variant);
        if (!state_->directory.empty()) write_entry(state_->directory, t, stage, key);
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto [it, made] = state_->shaders.emplace(k, std::move(t));
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
