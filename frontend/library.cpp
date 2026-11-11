// The launcher's games and players (library.h). The title's name comes from
// the executable's XDBF resource as Xenia reads it: the resource lookup is
// UserModule::GetSection (src/xenia/kernel/user_module.cc), the XDBF's layout
// and its string tables kernel/xam/xdbf/xdbf_io.h and spa_info.cc (BSD; see
// LICENSE-xenia).

#include "library.h"

#include "xenoforge/bytes.h"
#include "xenoforge/disc.h"
#include "xenoforge/game_source.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace xf::frontend {

namespace {

constexpr uint32_t XDBF = 0x58444246, XSTC = 0x58535443, XSTR = 0x58535452;
constexpr uint16_t SECTION_METADATA = 1, SECTION_IMAGE = 2, SECTION_STRING_TABLE = 3;
constexpr uint64_t ID_TITLE = 0x8000; // kXdbfIdTitle: the title's name in each string table
constexpr uint64_t LANGUAGE_ENGLISH = 1;

uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// An XDBF's entries (XdbfFile): the header, the table of contents, the free
// list, then the entries' data, each at its offset from there.
struct Xdbf {
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t entries = 0;  // used ones
    size_t contents = 0;   // where the table of contents starts
    size_t data_start = 0; // where the entries' data starts

    bool open(const uint8_t *p, size_t n) {
        if (n < 24 || be32(p) != XDBF) return false;
        const uint32_t count = be32(p + 8), used = be32(p + 12), free_count = be32(p + 16);
        data = p, size = n, entries = std::min(used, count), contents = 24;
        data_start = 24 + size_t(count) * 18 + size_t(free_count) * 8;
        return data_start <= n;
    }
    // an entry's bytes, or an empty span
    std::pair<const uint8_t *, size_t> find(uint16_t section, uint64_t id) const {
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *e = data + contents + size_t(i) * 18;
            if (be16(e) != section || be64(e + 2) != id) continue;
            const size_t offset = data_start + be32(e + 10), length = be32(e + 14);
            if (offset > size || length > size - offset) return {nullptr, 0};
            return {data + offset, length};
        }
        return {nullptr, 0};
    }
    // a string of a language's table (an XSTR: magic, version, size, count,
    // then id, length and the UTF-8 bytes of each)
    std::string string(uint64_t language, uint16_t id) const {
        auto [p, n] = find(SECTION_STRING_TABLE, language);
        if (!p || n < 14 || be32(p) != XSTR) return {};
        const uint16_t count = be16(p + 12);
        size_t at = 14;
        for (uint16_t i = 0; i < count && at + 4 <= n; i++) {
            const uint16_t string_id = be16(p + at), length = be16(p + at + 2);
            at += 4;
            if (length > n - at) break;
            if (string_id == id) return std::string(reinterpret_cast<const char *>(p + at), length);
            at += length;
        }
        return {};
    }
    uint64_t default_language() const {
        auto [p, n] = find(SECTION_METADATA, XSTC);
        return p && n >= 16 && be32(p) == XSTC ? be32(p + 12) : LANGUAGE_ENGLISH;
    }
    // the title's image (GameInfoDatabase::GetIcon): a PNG, in the image section under the title's ID
    std::vector<uint8_t> icon() const {
        auto [p, n] = find(SECTION_IMAGE, ID_TITLE);
        if (!p || n < 8 || be32(p) != 0x89504E47) return {};
        return std::vector<uint8_t>(p, p + n);
    }
};

// the resource named so in the executable's RESOURCE_INFO, as bytes of the
// loaded image (GetSection gives its guest address and size)
std::pair<const uint8_t *, size_t> resource(const Xex &xex, const std::string &name) {
    for (const auto &[key, value] : xex.opt_headers) {
        if (key != xex_key::RESOURCE_INFO || value + 4 > xex.header.size()) continue;
        const uint8_t *info = xex.header.data() + value;
        const uint32_t bytes = std::min<size_t>(be32(info), xex.header.size() - value);
        const uint32_t count = bytes < 4 ? 0 : (bytes - 4) / 16;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *r = info + 4 + 16 * i;
            const char *res_name = reinterpret_cast<const char *>(r);
            if (name != std::string_view(res_name, strnlen(res_name, 8))) continue;
            const uint32_t address = be32(r + 8), size = be32(r + 12);
            if (address < xex.base_address) return {nullptr, 0};
            const size_t offset = address - xex.base_address;
            if (offset > xex.image.size() || size > xex.image.size() - offset) return {nullptr, 0};
            return {xex.image.data() + offset, size};
        }
    }
    return {nullptr, 0};
}

bool is_player_name(const std::filesystem::path &file) {
#ifdef _WIN32
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    if (extension != ".exe") return false;
#else
    if (file.has_extension()) return false;
#endif
    const std::string stem = file.stem().string();
    return stem == "player" || (stem.starts_with("player_") && stem.size() > 7);
}

// a player run with --identify: one line per module it carries,
// "module <digest> <path>"
bool identify(PlayerBinary &player) {
    const std::string path = player.path.string();
    const char *args[] = {path.c_str(), "--identify", nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true); // no console window
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) return false;
    size_t size = 0;
    char *out = static_cast<char *>(SDL_ReadProcess(process, &size, nullptr));
    SDL_DestroyProcess(process);
    if (!out) return false;
    std::istringstream lines(std::string(out, size));
    SDL_free(out);
    std::string line;
    bool answered = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "player") answered = true; // the generic player's answer: no modules
        if (!line.starts_with("module ")) continue;
        answered = true;
        const size_t space = line.find(' ', 7);
        if (space == std::string::npos) continue;
        player.modules.emplace_back(std::strtoull(line.substr(7, space - 7).c_str(), nullptr, 16),
                                    line.substr(space + 1));
    }
    return answered;
}

} // namespace

std::string GameEntry::title_id_text() const {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", title_id);
    return text;
}

std::string title_name(const Xex &xex) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", xex.execution.title_id);
    auto [p, n] = resource(xex, name);
    Xdbf xdbf;
    if (!p || !xdbf.open(p, n)) return {};
    std::string title = xdbf.string(LANGUAGE_ENGLISH, ID_TITLE);
    return title.empty() ? xdbf.string(xdbf.default_language(), ID_TITLE) : title;
}

std::vector<uint8_t> title_icon(const Xex &xex) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", xex.execution.title_id);
    auto [p, n] = resource(xex, name);
    Xdbf xdbf;
    if (!p || !xdbf.open(p, n)) return {};
    return xdbf.icon();
}

GameEntry read_game(const std::filesystem::path &path) {
    GameEntry game;
    game.path = path;
    std::error_code ec;
    game.size = std::filesystem::file_size(path, ec);
    std::vector<uint8_t> file;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    if (extension == ".xex") {
        std::ifstream f(path, std::ios::binary);
        if (!f) return game.error = "cannot be read", game;
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } else {
        auto source = GameSource::open(path, game.error);
        if (!source) return game;
        const GameSource::File *entry = source->find("default.xex");
        if (!entry) return game.error = source->package() ? "no game in the package" : "no default.xex on the disc", game;
        file = source->read_all(*entry);
    }
    Xex xex;
    if (!parse_xex(file, xex, game.error) || !load_xex_image(file, xex, game.error)) return game;
    game.title_id = xex.execution.title_id;
    game.version = xex.execution.version;
    game.media_id = xex.execution.media_id;
    game.digest = image_digest(xex.image);
    game.title = title_name(xex);
    game.icon = title_icon(xex);
    if (game.title.empty()) game.title = path.stem().string();
    return game;
}

std::vector<std::filesystem::path> find_games(const std::filesystem::path &folder) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, options, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        // the folder and its subfolders, no deeper, but for a content
        // package's own folders (<title id>\<content type>, eight hex digits each)
        if (it.depth() >= 1 && it->is_directory(ec)) {
            const std::string dir = it->path().filename().string();
            const bool content = dir.size() == 8 && std::all_of(dir.begin(), dir.end(), [](char ch) {
                return std::isxdigit(uint8_t(ch)) != 0;
            });
            if (!content || it.depth() >= 3) it.disable_recursion_pending();
        }
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.ends_with(".iso") || name == "default.xex")
            found.push_back(it->path());
        // a package holding a game (an arcade game, a game on demand): no
        // extension, and a default.xex inside (an update or an add-on has none)
        else if (!it->path().has_extension() && GameSource::is_package(it->path())) {
            std::string why;
            if (auto source = GameSource::open(it->path(), why); source && source->find("default.xex"))
                found.push_back(it->path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<PlayerBinary> find_players(const std::filesystem::path &directory) {
    std::vector<PlayerBinary> players;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec) || !is_player_name(entry.path())) continue;
        PlayerBinary player;
        player.path = entry.path();
        player.generic = entry.path().stem() == "player";
        if (identify(player)) players.push_back(std::move(player));
    }
    std::sort(players.begin(), players.end(), [](auto &a, auto &b) { return a.path < b.path; });
    return players;
}

const PlayerBinary *player_for(const std::vector<PlayerBinary> &players, const GameEntry &game, bool cached_library) {
    const PlayerBinary *generic = nullptr, *carrying = nullptr;
    for (const PlayerBinary &p : players) {
        if (p.generic) generic = &p;
        for (const auto &module : p.modules)
            if (game.digest && module.first == game.digest) carrying = &p;
    }
    // a cached library is what the emulator runs; a player with the game's
    // code is the way out for a disc with none
    if (cached_library && generic) return generic;
    return carrying ? carrying : generic;
}

bool plan_launch(const std::vector<PlayerBinary> &players, const GameEntry &game, bool cached_library,
                 const std::vector<std::string> &extra_args, LaunchPlan &plan) {
    const PlayerBinary *player = player_for(players, game, cached_library);
    if (!player) return false;
    plan = LaunchPlan{};
    plan.program = player->path;
    plan.generic = player->generic;
    plan.from_library = cached_library && player->generic;
    plan.args = extra_args;
    return true;
}

} // namespace xf::frontend
