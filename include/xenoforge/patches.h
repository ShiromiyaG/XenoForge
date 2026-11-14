// Game patches in the format of Xenia Canary's patcher (src/xenia/patcher/
// patch_db.* and patcher.*, BSD; see LICENSE-xenia): `<TITLE ID>*.patch.toml`
// files naming a title, the hashes of the modules they were made for and
// patches that write bytes at guest addresses (docs/runtime.md, "Game
// patches").
//
// XenoForge builds the game's code ahead of time, so a patch is applied by
// what it writes over: bytes in a code section of the module's image are code
// (applied to the image before the build and the load, and part of the
// library's cache key); the rest is data (written to guest memory once the
// module is loaded, as Xenia does).

#pragma once

#include "xenoforge/memory.h"
#include "xenoforge/xex.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xf::patches {

// bytes at an address, as the file's be8..array/string entries give them (the
// big-endian forms already swapped to the guest's order)
struct Entry {
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

struct Patch {
    std::string name, description, author;
    bool enabled = false; // the file's is_enabled: the default when the config says nothing
    std::vector<Entry> entries;
};

struct File {
    std::string path; // where it was read from (the file's name)
    uint32_t title_id = 0;
    std::string title_name;
    std::vector<uint64_t> hashes; // the modules it applies to (module_hash)
    std::vector<Patch> patches;
    std::vector<std::string> warnings; // patches left out, and why
};

// One file's text. false, and why, when it is not a patch file (not TOML, or
// without title_name, title_id or hash, as Xenia refuses it). A patch with a
// field that is wrong is left out whole (a warning); Xenia keeps the entries
// it read before the bad one.
bool parse(std::string_view text, File &out, std::string &error);

// whether a file name is one the patcher reads: eight hex digits, anything,
// ".patch.toml"
bool is_patch_file_name(std::string_view name);

// Every patch file of a folder (a missing folder has none), in the order of
// their names. What could not be read is in `problems`, a line each.
std::vector<File> load_directory(const std::filesystem::path &directory, std::vector<std::string> *problems = nullptr);

// the config folder's patches folder: %APPDATA%\xenoforge\patches, or
// $XDG_DATA_HOME/xenoforge/patches (~/.local/share/...) where the player's
// preference folder is
std::filesystem::path default_directory();

// What names a module to the patches: Xenia's hash (XXH3-64 of the image from
// the first code page to the end of the last), its title and where its code
// is.
struct ModuleInfo {
    uint32_t title_id = 0;
    uint64_t hash = 0;
    uint32_t base = 0, size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> code; // [begin, end) of each run of code pages
    bool valid() const { return size != 0; }
    bool is_code(uint32_t address) const;
};
// Of an executable as load_xex_image gives it (before any patch is applied;
// 0 hash for an image without code pages).
ModuleInfo module_info(const Xex &xex);

// what the user chose: the folder read, and by patch name on or off over the
// file's is_enabled
struct Config {
    std::filesystem::path directory; // empty: default_directory()
    std::map<std::string, bool> enabled;
    bool off = false; // no patches at all (Xenia's apply_patches = false)
};

// One enabled patch's piece: all of it code or all of it data
struct Item {
    std::string file, name;
    Entry entry;
    bool code = false;
};
struct Plan {
    std::vector<Item> items;
    // for each matching file the patches it holds and what applies; lines for the log
    std::vector<std::string> log;
    // empty without code patches; else a digest of their bytes in order: the
    // game library's cache key (game_build.h) carries it after the image's
    std::string code_key;
    bool any_code() const { return !code_key.empty(); }
};

// The enabled patches of the files that name this title and this module's
// hash, cut where they pass from code to data.
Plan make_plan(const ModuleInfo &module, const std::vector<File> &files, const std::map<std::string, bool> &enabled);
// load_directory and make_plan of a Config
Plan make_plan(const ModuleInfo &module, const Config &config);
// The executable file as an XEX2 whose image is `xex.image` (loaded from `file` and patched by
// apply_code), neither encrypted nor compressed: the file's headers with the file format info
// set to none, then the image. What discovery and emission read instead of the original, so
// nothing of the recompiler needs to know of patches. false when the headers do not allow it.
bool patched_file(std::span<const uint8_t> file, const Xex &xex, std::vector<uint8_t> &out);
// the cache's name of an image digest (16 hex digits) with the plan's code
// patches: the digest alone when there are none, else "<digest>+<code_key>"
std::string cache_key(const std::string &digest, const Plan &plan);

// The plan's code items written into the image (at `base`); what lies past
// the image is left out. The count of bytes changed.
size_t apply_code(const Plan &plan, std::vector<uint8_t> &image, uint32_t base);
// The data items written into guest memory (protection lifted for the write,
// then put back); an address in no heap is left out. Lines for the log.
std::vector<std::string> apply_data(const Plan &plan, Memory &memory);

} // namespace xf::patches
