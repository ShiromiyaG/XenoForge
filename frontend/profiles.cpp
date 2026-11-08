#include "profiles.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <regex>

namespace xf::frontend {

namespace fs = std::filesystem;

namespace {

bool is_xuid(const std::string &s) {
    static const std::regex pattern("[0-9A-F]{16}");
    return std::regex_match(s, pattern) && s != "0000000000000000";
}

std::string package_dir_name(const std::string &xuid) { return xuid; }

fs::path profile_package(const fs::path &storage, const std::string &xuid) {
    return storage / "content" / xuid / "FFFE07D1" / "00010000" / package_dir_name(xuid);
}

} // namespace

bool valid_gamertag(const std::string &name, std::string &why) {
    if (name.empty()) return why = "a gamertag needs at least one character", false;
    if (name.size() > 15) return why = "a gamertag has 15 characters at most", false;
    if (!std::isalpha((unsigned char)name[0])) return why = "a gamertag starts with a letter", false;
    if (name.back() == ' ') return why = "a gamertag does not end with a space", false;
    for (size_t i = 0; i < name.size(); i++) {
        const unsigned char c = name[i];
        if (c >= 0x80 || !(std::isalnum(c) || c == ' '))
            return why = "a gamertag has letters, digits and spaces only", false;
        if (c == ' ' && i && name[i - 1] == ' ') return why = "a gamertag has no two spaces in a row", false;
    }
    return true;
}

std::vector<ProfileEntry> list_profiles(const fs::path &storage, const Settings &settings) {
    std::vector<ProfileEntry> out;
    std::error_code ec;
    for (fs::directory_iterator it(storage / "content", ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (!it->is_directory(ec) || !is_xuid(name)) continue;
        if (!fs::exists(profile_package(storage, name), ec)) continue;
        ProfileEntry e;
        e.xuid = name;
        if (auto n = settings.profile.names.find(name); n != settings.profile.names.end()) e.name = n->second;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const ProfileEntry &a, const ProfileEntry &b) { return a.xuid < b.xuid; });
    if (out.empty() && is_xuid(settings.profile.xuid)) {
        ProfileEntry e;
        e.xuid = settings.profile.xuid;
        if (auto n = settings.profile.names.find(e.xuid); n != settings.profile.names.end()) e.name = n->second;
        out.push_back(std::move(e));
    }
    // the selected: the settings' XUID when it is there, else the first (as the kernel's storage choice)
    bool found = false;
    for (ProfileEntry &e : out)
        if (e.xuid == settings.profile.xuid) e.selected = found = true;
    if (!found && !out.empty()) out.front().selected = true;
    return out;
}

bool create_profile(const fs::path &storage, Settings &settings, const std::string &name, std::string &xuid_out,
                    std::string &error) {
    if (!valid_gamertag(name, error)) return false;
    for (const ProfileEntry &e : list_profiles(storage, settings))
        if (e.name.size() == name.size() &&
            std::equal(name.begin(), name.end(), e.name.begin(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
            return error = "a profile has that gamertag already", false;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::string xuid;
    std::error_code ec;
    for (int attempt = 0; attempt < 16; attempt++) {
        char text[20];
        std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)((uint64_t(0xE03) << 52) + (gen() % (1u << 31))));
        if (!fs::exists(storage / "content" / text, ec)) {
            xuid = text;
            break;
        }
    }
    if (xuid.empty()) return error = "no free XUID", false;
    fs::create_directories(profile_package(storage, xuid), ec);
    if (ec) return error = "cannot create the profile's directory: " + ec.message(), false;
    const bool first = list_profiles(storage, settings).size() <= 1 && settings.profile.xuid.empty();
    settings.profile.names[xuid] = name;
    if (first) settings.profile.xuid = xuid, settings.profile.gamertag = name;
    xuid_out = xuid;
    return true;
}

bool rename_profile(Settings &settings, const std::string &xuid, const std::string &name, std::string &error) {
    if (!is_xuid(xuid)) return error = "no such profile", false;
    if (!valid_gamertag(name, error)) return false;
    settings.profile.names[xuid] = name;
    if (xuid == settings.profile.xuid) settings.profile.gamertag = name;
    return true;
}

bool select_profile(Settings &settings, const std::string &xuid, std::string &error) {
    if (!is_xuid(xuid)) return error = "no such profile", false;
    settings.profile.xuid = xuid;
    auto n = settings.profile.names.find(xuid);
    settings.profile.gamertag = n != settings.profile.names.end() ? n->second : std::string();
    return true;
}

bool delete_profile(const fs::path &storage, Settings &settings, const std::string &xuid, std::string &error) {
    if (!is_xuid(xuid)) return error = "no such profile", false;
    std::vector<ProfileEntry> all = list_profiles(storage, settings);
    if (all.size() <= 1) return error = "the only profile cannot be deleted", false;
    std::error_code ec;
    fs::remove_all(storage / "content" / xuid, ec);
    if (ec) return error = "cannot remove the profile's directory: " + ec.message(), false;
    settings.profile.names.erase(xuid);
    if (xuid == settings.profile.xuid) {
        settings.profile.xuid.clear(), settings.profile.gamertag.clear();
        for (const ProfileEntry &e : all)
            if (e.xuid != xuid) {
                select_profile(settings, e.xuid, error);
                break;
            }
    }
    return true;
}

fs::path storage_root(const Settings &settings, const fs::path &config_file) {
    if (!settings.saves.root.empty()) return fs::path(std::u8string(settings.saves.root.begin(), settings.saves.root.end()));
    return config_file.parent_path() / "storage";
}

uint64_t selected_xuid(const Settings &settings) {
    return is_xuid(settings.profile.xuid) ? std::stoull(settings.profile.xuid, nullptr, 16) : 0;
}

std::string selected_gamertag(const fs::path &storage, const Settings &settings) {
    for (const ProfileEntry &e : list_profiles(storage, settings))
        if (e.selected && !e.name.empty()) return e.name;
    return settings.profile.gamertag;
}

} // namespace xf::frontend
