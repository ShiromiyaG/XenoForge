// A title update: a content package (type 0x000B0000, an "installer") whose
// files replace or patch the game's. An executable's patch is the file of its
// own path with a "p" added (default.xex's is default.xexp), applied to the
// executable by apply_xex_patch (xex.h) before it is recompiled or run, as
// Xenia's KernelState::ApplyTitleUpdate does at each module's load.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xf {

class Package;

class TitleUpdate {
public:
    static constexpr uint32_t CONTENT_TYPE = 0x000B0000;

    // A package of that content type. On failure: null and the reason.
    static std::unique_ptr<TitleUpdate> open(const std::filesystem::path &package, std::string &error);
    // The title's update among the content folders under the roots
    // (<root>/<title id>/000B0000/<file>, eight hex digits, as the console's
    // and Xenia's content is laid out): the one of the highest version, and
    // an empty path where there is none. Missing roots are passed over.
    static std::filesystem::path find(uint32_t title_id, std::span<const std::filesystem::path> roots);
    ~TitleUpdate();

    const std::filesystem::path &path() const { return path_; }
    uint32_t title_id() const;
    uint32_t version() const; // the update's (the execution info's, as an executable's)
    // The patch of the executable at this path of the game's device
    // ("default.xex", either slash, any case): the bytes of <path>p, empty
    // where the update has none.
    std::vector<uint8_t> patch(std::string_view module_path) const;

private:
    TitleUpdate();
    std::filesystem::path path_;
    std::unique_ptr<Package> package_;
};

} // namespace xf
