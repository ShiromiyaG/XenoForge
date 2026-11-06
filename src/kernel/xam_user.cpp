/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's users and profiles (xam.h): one user signed in offline in slot 0,
// its account's flags, its profile settings and preferences, signing in
// and out, the accounts and profile packages (the input exports are in
// xam_input.cpp, the locale in xam_locale.cpp, the guide's UI in
// xam_ui.cpp). Translated from Xenia (BSD; see LICENSE-xenia):
// xam_user.cc, xam_profile.cc, xam_enum.cc (the profile enumerator), xam.h
// (X_XAMACCOUNTINFO and the creation info), profile_manager.cc (Login,
// Logout, the accounts, the profile's path), user_settings.h and .cc,
// user_data.cc, user_tracker.cc (GetUserSetting, the preferences, the
// subscription); checked against them.
//
// Where it differs from Xenia, and why:
//   - the user is Xenia's default profile (XUID 0xB13EBABEBABEBABE, or the
//     first profile in the content root) named "User", without its account
//     file or GPDs: settings a title writes are kept for the run, not in
//     the profile's GPD, and no titles played are recorded;
//   - the accounts XamProfileCreate makes are kept for the run (no Account
//     file is written) and cannot sign in: only the one user does, to slot
//     0;
//   - XamUserGetUserTenure completes an overlapped it is given (Xenia
//     leaves it alone, and a title waiting on it would wait for good);
//   - XamUserIsParentalControlled tests the account's parental control bit
//     (Xenia tests the language's bits; an offline account has neither).

#include "xam.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <random>

namespace xf::kernel {
namespace {

bool signed_in(Kernel &k, uint32_t index) { return user_at(k, index & 0xFF) != nullptr && index < 0x100; }

// ---- profile settings (user_settings.h, user_data.h) ----

// a setting's id: id (16 bits), size (12), type (4)
enum class DataType : uint8_t {
    Context = 0,
    Int32 = 1,
    Int64 = 2,
    Double = 3,
    WString = 4,
    Float = 5,
    Binary = 6,
    DateTime = 7
};
constexpr uint32_t MAX_USER_DATA_SIZE = 0x3E8;

DataType type_of(uint32_t setting) { return DataType(setting >> 28); }
uint32_t max_size_of(uint32_t setting) { return std::min<uint32_t>((setting >> 16) & 0xFFF, MAX_USER_DATA_SIZE); }
bool extended(uint32_t setting) {
    return type_of(setting) == DataType::WString || type_of(setting) == DataType::Binary;
}

// UserSetting::is_setting_valid: an id below 0x58, or one of the known
// settings above it (the crux ones and the title's three)
bool valid_setting(uint32_t setting) {
    static constexpr uint32_t known[] = {
        0x100403EA, 0x100403EB, 0x100403EC, 0x100403ED, 0x100403EE, 0x100403EF, 0x100403F0, 0x100403F1, 0x100403F3,
        0x43E803FA, 0x406403FB, 0x406403FC, 0x406403FD, 0x406403FE, 0x406403E8, 0x410003F6, 0x603403F2, 0x60A803F5,
        0x601003F7, 0x601003F8, 0x601003F9, 0x63E83FFF, 0x63E83FFE, 0x63E83FFD, 0x700803F4,
    };
    return (setting & 0xFFFF) < 0x58 || std::find(std::begin(known), std::end(known), setting) != std::end(known);
}

// a setting's value: the union's 8 bytes (big-endian, as the guest has
// them) and, for strings and binaries, its data
struct Setting {
    uint8_t value[8] = {};
    std::vector<uint8_t> data;
    uint32_t source = 1; // DEFAULT; 2 when the title wrote it
};

void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// UserSetting::GetDefaultSetting: Xenia's three defaults, else zero of the
// setting's type
Setting default_setting(uint32_t setting) {
    Setting s;
    switch (setting) {
    case 0x10040003: put_be32(s.value, 3); break; // XPROFILE_OPTION_CONTROLLER_VIBRATION
    case 0x1004003A: put_be32(s.value, 6); break; // XPROFILE_GAMER_TIER: gold
    case 0x4064000F: {                            // XPROFILE_GAMERCARD_PICTURE_KEY
        const char *key = "FFFE07D10002000200010002";
        for (const char *p = key; *p; p++) s.data.push_back(0), s.data.push_back(uint8_t(*p));
        s.data.push_back(0), s.data.push_back(0);
        put_be32(s.value, uint32_t(s.data.size()));
        break;
    }
    default: break;
    }
    return s;
}

struct UserState {
    std::mutex mutex;
    std::map<std::pair<uint32_t, uint32_t>, Setting> written; // (title, setting)
};

UserState &users(Kernel &k) { return k.state<UserState>(); }

// the dashboard's settings in the profile (Xenia's dashboard GPD)
constexpr uint32_t DASHBOARD_ID = 0xFFFE07D1;
constexpr uint32_t XPROFILE_TENURE_LEVEL = 0x10040047, XPROFILE_TENURE_MILESTONE = 0x10040048;
constexpr uint32_t XPROFILE_TENURE_NEXT_MILESTONE_DATE = 0x70080049;
constexpr uint32_t XPROFILE_SUBSCRIPTION_TYPE_LENGTH_IN_MONTHS = 0x1004004B;
constexpr uint32_t XPROFILE_SUBSCRIPTION_PAYMENT_TYPE = 0x1004004C, XPROFILE_USER_PREFERENCES = 0x10040053;

uint32_t be32_at(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// UserTracker::GetSetting(user, kDashboardID, id) of a number: what was
// written, else its default; 32 or 64 bits by its type
uint64_t dashboard_value(Kernel &k, uint32_t id) {
    UserState &s = users(k);
    std::lock_guard<std::mutex> l(s.mutex);
    auto it = s.written.find({DASHBOARD_ID, id});
    Setting v = it != s.written.end() ? it->second : default_setting(id);
    if (type_of(id) == DataType::Int64 || type_of(id) == DataType::DateTime || type_of(id) == DataType::Double)
        return uint64_t(be32_at(v.value)) << 32 | be32_at(v.value + 4);
    return be32_at(v.value);
}

// GetUserPreferenceWrittenBit: the bit a multi-bit preference marks itself
// written with (0 for one bit), none for an unknown mask
std::optional<uint32_t> preference_written_bit(uint32_t mask) {
    switch (mask) {
    case 0x001:
    case 0x020:
    case 0x040:
    case 0x080:
    case 0x100:
    case 0x200: return 0;
    case 0x006: return 0x004;
    case 0x018: return 0x010;
    default: return std::nullopt;
    }
}

// IsUserSignedIn(user_index): the index's low byte
const User *signed_in_user(Kernel &k, uint32_t index) { return user_at(k, uint8_t(index)); }

// X_USER_PROFILE_SETTING (40): source, user_index or xuid (8), setting id,
// X_USER_DATA (type at 0x18, value at 0x20); strings and binaries after the
// settings, the setting's maximum size each (UserSetting::WriteToGuest)
void write_setting(Kernel &k, uint32_t at, uint32_t setting, const Setting &s, uint32_t &extra) {
    k.store32(at + 0x00, s.source);
    k.store32(at + 0x10, setting);
    k.host(at + 0x18)[0] = uint8_t(type_of(setting));
    std::memcpy(k.host(at + 0x20), s.value, 8);
    if (!extended(setting) || s.data.empty()) return;
    k.store32(at + 0x20, uint32_t(s.data.size()));
    k.store32(at + 0x24, extra);
    std::memcpy(k.host(extra), s.data.data(), s.data.size());
    extra += max_size_of(setting);
}

} // namespace

// ---- users (xam_user.cc) ----

XF_EXPORT(xam, XamUserGetSigninState) {
    uint32_t index = arg(k, c, 0);
    const User *u = index < 4 ? user_at(k, index) : nullptr;
    result(c, u ? u->signin_state : 0);
}

// (user_index, flags, X_USER_SIGNIN_INFO *)
XF_EXPORT(xam, XamUserGetSigninInfo) {
    uint32_t index = arg(k, c, 0), flags = arg(k, c, 1), info = arg(k, c, 2);
    if (!info) return result(c, X_E_INVALIDARG);
    std::memset(k.host(info), 0, 40);
    if (index >= 4) return result(c, X_E_INVALIDARG);
    const User *u = user_at(k, index);
    if (!u) return result(c, X_E_NO_SUCH_USER);
    size_t n = std::min<size_t>(u->name.size(), 15);
    std::memcpy(k.host(info + 0x18), u->name.data(), n);
    if (!flags || (flags & 2)) k.store64(info, u->xuid); // X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY
    k.store32(info + 0x0C, u->signin_state);
    result(c, X_E_SUCCESS);
}

// (user_index, type_mask, xuid_ptr): an offline profile is both kinds
XF_EXPORT(xam, XamUserGetXUID) {
    uint32_t index = arg(k, c, 0), mask = arg(k, c, 1), xuid_ptr = arg(k, c, 2);
    if (!xuid_ptr) return result(c, X_E_INVALIDARG);
    k.store64(xuid_ptr, 0);
    if (index >= 4) return result(c, X_E_INVALIDARG);
    const User *u = user_at(k, index);
    if (!u) return result(c, X_E_NO_SUCH_USER);
    if (!(mask & 3)) return result(c, X_E_NO_SUCH_USER); // the profile is local (1) and online (2)
    k.store64(xuid_ptr, u->xuid);
    result(c, X_E_SUCCESS);
}

// (user_index, buffer, length): the name and its terminator, as much as fits
XF_EXPORT(xam, XamUserGetName) {
    uint32_t index = arg(k, c, 0), buffer = arg(k, c, 1), length = arg(k, c, 2);
    const User *u = index < 4 ? user_at(k, index) : nullptr;
    if (!u) {
        if (buffer) k.host(buffer)[0] = 0;
        return result(c, index >= 4 ? X_ERROR_INVALID_PARAMETER : X_ERROR_NO_SUCH_USER);
    }
    uint32_t n = std::min<uint32_t>(length, uint32_t(u->name.size()) + 1);
    if (n) {
        std::memcpy(k.host(buffer), u->name.data(), n - 1);
        k.host(buffer)[n - 1] = 0;
    }
    result(c, X_ERROR_SUCCESS);
}

// (xuid, flags, index_ptr)
XF_EXPORT(xam, XamUserGetIndexFromXUID) {
    uint32_t index_ptr = arg(k, c, 2);
    if (!index_ptr) return result(c, X_E_INVALIDARG);
    if (!user_by_xuid(k, arg64(k, c, 0))) return result(c, X_E_NO_SUCH_USER);
    k.store32(index_ptr, 0);
    result(c, X_ERROR_SUCCESS);
}

namespace {
// XamUserCheckPrivilege: offline, every privilege is denied
uint32_t check_privilege(Kernel &k, uint32_t index, uint32_t out) {
    if (index == USER_INDEX_ANY) {
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t r = check_privilege(k, i, out);
            if (r != X_ERROR_NO_SUCH_USER) {
                if (out) k.store32(out, 0);
                return r;
            }
        }
        if (out) k.store32(out, 0);
        return X_ERROR_NO_SUCH_USER;
    }
    if (index >= 4) return X_ERROR_INVALID_PARAMETER;
    const User *u = user_at(k, index);
    if (!u) return X_ERROR_NO_SUCH_USER;
    if (out) k.store32(out, 0);
    return u->signin_state != 2 ? X_ERROR_NOT_LOGGED_ON : X_ERROR_SUCCESS;
}
} // namespace

// (user_index, mask, out_value)
XF_EXPORT(xam, XamUserCheckPrivilege) { result(c, check_privilege(k, arg(k, c, 0), arg(k, c, 2))); }

// (user_index, xuids, count, are_friends_ptr, overlapped): not online
XF_EXPORT(xam, XamUserAreUsersFriends) {
    uint32_t index = arg(k, c, 0), friends_ptr = arg(k, c, 3), ov = arg(k, c, 4);
    if (friends_ptr) k.store32(friends_ptr, 0);
    uint32_t r;
    if (index >= 4)
        r = X_ERROR_INVALID_PARAMETER;
    else if (const User *u = user_at(k, index))
        r = u->signin_state != 2 ? X_ERROR_NOT_LOGGED_ON : X_ERROR_SUCCESS;
    else
        r = X_ERROR_NO_SUCH_USER;
    if (ov) {
        complete_overlapped_immediate_ex(k, ov, r == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
                                         hresult_from_win32(r), 0);
        return result(c, X_ERROR_SUCCESS);
    }
    result(c, r);
}

namespace {
// XamUserReadProfileSettingsEx: X_USER_READ_PROFILE_SETTINGS (the count,
// the settings' address), the settings, their strings and binaries
uint32_t read_profile_settings(Kernel &k, uint32_t title, uint32_t index, uint32_t xuid_count, uint32_t xuids,
                               uint32_t count, uint32_t ids, uint32_t size_ptr, uint32_t buffer, uint32_t ov) {
    if (count < 1 || count > 32 || !size_ptr) return X_ERROR_INVALID_PARAMETER;
    uint32_t size = k.load32(size_ptr);
    if (size && !buffer) return X_ERROR_INVALID_PARAMETER;
    uint32_t header = 0, data = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = k.load32(ids + 4 * i);
        header += 40;
        if (extended(id)) data += (id >> 16) & 0xFFF;
    }
    if (xuids) header *= xuid_count, data *= xuid_count;
    uint32_t needed = header + 8 + data;
    if (!buffer || size < needed) {
        if (!size) k.store32(size_ptr, needed);
        return X_ERROR_INSUFFICIENT_BUFFER;
    }
    auto run = [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        extended_error = 0, length = 0;
        if (xuids && (!xuid_count || xuid_count > 16)) {
            extended_error = X_E_INVALIDARG;
            return X_ERROR_FUNCTION_FAILED;
        }
        const User *u = xuids ? user_by_xuid(k, k.load64(xuids)) : user_at(k, index);
        if (!u) {
            extended_error = X_E_NO_SUCH_USER;
            return X_ERROR_FUNCTION_FAILED;
        }
        uint32_t settings = buffer + 8, extra = settings + 40 * count, n = count, out = settings;
        std::memset(k.host(settings), 0, 40 * count);
        UserState &s = users(k);
        std::lock_guard<std::mutex> l(s.mutex);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t id = k.load32(ids + 4 * i);
            if (!valid_setting(id)) {
                if (id) std::fprintf(stderr, "xam: XamUserReadProfileSettings: setting 0x%08X is unknown\n", id);
                n--;
                continue;
            }
            auto w = s.written.find({title ? title : title_id(k), id});
            write_setting(k, out, id, w != s.written.end() ? w->second : default_setting(id), extra);
            if (xuids) {
                k.store64(out + 8, u->xuid);
            } else {
                k.store64(out + 8, ~0ull);
                k.store32(out + 8, index);
            }
            out += 40;
        }
        k.store32(buffer, n);
        k.store32(buffer + 4, settings);
        extended_error = 0;
        return X_ERROR_SUCCESS;
    };
    return run_or_defer(k, ov, run);
}
} // namespace

// (title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
// buffer_size_ptr, buffer, overlapped)
XF_EXPORT(xam, XamUserReadProfileSettings) {
    result(c, read_profile_settings(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4),
                                    arg(k, c, 5), arg(k, c, 6), arg(k, c, 7), arg(k, c, 8)));
}

// (title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
// buffer_size_ptr, other_buffer_size_ptr, buffer, overlapped): the size
// through either pointer
XF_EXPORT(xam, XamUserReadProfileSettingsEx) {
    uint32_t size_ptr = arg(k, c, 6) ? arg(k, c, 6) : arg(k, c, 7);
    result(c, read_profile_settings(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4),
                                    arg(k, c, 5), size_ptr, arg(k, c, 8), arg(k, c, 9)));
}

// (title_id, user_index, setting_count, settings, overlapped): kept for the
// run; a ProfileSettingChanged notification for the user
XF_EXPORT(xam, XamUserWriteProfileSettings) {
    uint32_t title = arg(k, c, 0), index = arg(k, c, 1), count = arg(k, c, 2), settings = arg(k, c, 3);
    uint32_t ov = arg(k, c, 4);
    if (!count || !settings) return result(c, X_ERROR_INVALID_PARAMETER);
    auto run = [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        length = 0;
        if (!user_at(k, index)) {
            extended_error = hresult_from_win32(X_ERROR_NO_SUCH_USER);
            return X_ERROR_NO_SUCH_USER;
        }
        {
            UserState &s = users(k);
            std::lock_guard<std::mutex> l(s.mutex);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t at = settings + 40 * i, id = k.load32(at + 0x10);
                Setting v;
                v.source = 2;
                std::memcpy(v.value, k.host(at + 0x20), 8);
                if (extended(id)) {
                    uint32_t n = std::min(k.load32(at + 0x20), MAX_USER_DATA_SIZE), p = k.load32(at + 0x24);
                    if (n && p) v.data.assign(k.host(p), k.host(p) + n);
                    put_be32(v.value, uint32_t(v.data.size()));
                    put_be32(v.value + 4, 0);
                }
                s.written[{title ? title : title_id(k), id}] = std::move(v);
            }
        }
        broadcast(k, XN_SYS_PROFILESETTINGCHANGED, (1u << index) & 0xF);
        extended_error = 0;
        return X_ERROR_SUCCESS;
    };
    result(c, run_or_defer(k, ov, run));
}

// (title_id, user_index, xuid, flags, offset, count, buffer_size_ptr,
// handle_ptr): the title's achievements (none known here)
XF_EXPORT(xam, XamUserCreateAchievementEnumerator) {
    uint32_t index = arg(k, c, 1), flags = arg(k, c, 3), count = arg(k, c, 5), size_ptr = arg(k, c, 6);
    uint32_t handle_ptr = arg(k, c, 7);
    if (!count || !size_ptr || !handle_ptr || index >= 4) return result(c, X_ERROR_INVALID_PARAMETER);
    uint32_t entry = 36 + ((flags & 7) ? 464 : 0); // X_ACHIEVEMENT_DETAILS and its strings
    k.store32(size_ptr, entry * count);
    if (!user_at(k, index)) return result(c, X_ERROR_INVALID_PARAMETER);
    Ref<Enumerator> e = make_enumerator(k, index, 0xFB, 0xB000A, 0xB000B, 0, count, 36, 0);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    k.store32(handle_ptr, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// (title_id, user_index, count, flags, size, stats, buffer_size_ptr,
// handle_ptr): no statistics offline
XF_EXPORT(xam, XamUserCreateStatsEnumerator) {
    uint32_t index = arg(k, c, 1), count = arg(k, c, 2), flags = arg(k, c, 3), size = arg(k, c, 4);
    uint32_t stats = arg(k, c, 5), size_ptr = arg(k, c, 6), handle_ptr = arg(k, c, 7);
    if (!count || !size_ptr || !handle_ptr || !stats || index >= 4 || !flags || flags > 0x64 || !size)
        return result(c, X_ERROR_INVALID_PARAMETER);
    k.store32(size_ptr, 0);
    Ref<Enumerator> e = make_enumerator(k, index, 0xFB, 0xB0023, 0xB0024, 0, 0, 0, 0);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    k.store32(handle_ptr, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// (user_index, device_class, out): the user's own index, or none
XF_EXPORT(xam, XamUserGetDeviceContext) {
    uint32_t index = arg(k, c, 0), out = arg(k, c, 2);
    if (out) k.store32(out, 0);
    if (signed_in(k, index) || (index & 0xFF) == USER_INDEX_ANY) {
        if (out) k.store32(out, index);
        return result(c, X_E_SUCCESS);
    }
    result(c, X_E_DEVICE_NOT_CONNECTED);
}

// (user_index, buffer, buffer_len): the gamertag, 16 wide characters at
// most with the terminator
XF_EXPORT(xam, XamUserGetGamerTag) {
    uint32_t index = arg(k, c, 0), buffer = arg(k, c, 1), length = arg(k, c, 2);
    if (!buffer || length < 16 || index >= 4) return result(c, X_E_INVALIDARG);
    const User *u = signed_in_user(k, index);
    if (!u) return result(c, X_E_NO_SUCH_USER);
    put_u16(k, buffer, std::u16string(u->name.begin(), u->name.end()), std::min<uint32_t>(length, 16));
    result(c, X_E_SUCCESS);
}

// ---- the account's flags (X_XAMACCOUNTINFO) ----

namespace {
constexpr uint32_t ACCOUNT_LIVE_ENABLED = 0x20000000, ACCOUNT_PARENTAL_CONTROL = 0x01000000;
uint32_t tier_of(const User &u) { return (u.cached_flags & 0xF00000) >> 20; }
uint32_t user_flags_of(const User &u) { return u.cached_flags | u.reserved_flags; }
} // namespace

XF_EXPORT(xam, XamUserIsOnlineEnabled) {
    uint32_t index = arg(k, c, 0);
    const User *u = index < 4 ? signed_in_user(k, index) : nullptr;
    result(c, u && (u->reserved_flags & ACCOUNT_LIVE_ENABLED) ? 1 : 0);
}

// the subscription tier: none (0), silver (3), gold (6), family gold (9)
XF_EXPORT(xam, XamUserGetMembershipTier) {
    uint32_t index = arg(k, c, 0);
    const User *u = index < 4 ? signed_in_user(k, index) : nullptr;
    result(c, u ? tier_of(*u) : 0);
}

XF_EXPORT(xam, XamUserGetMembershipTierFromXUID) {
    const User *u = user_by_xuid(k, arg64(k, c, 0));
    result(c, u ? tier_of(*u) : 0);
}

XF_EXPORT(xam, XamUserGetCachedUserFlags) {
    const User *u = signed_in_user(k, arg(k, c, 0));
    result(c, u ? u->cached_flags : 0);
}

XF_EXPORT(xam, XamUserGetUserFlags) {
    const User *u = signed_in_user(k, arg(k, c, 0));
    result(c, u ? user_flags_of(*u) : 0);
}

XF_EXPORT(xam, XamUserGetUserFlagsFromXUID) {
    const User *u = user_by_xuid(k, arg64(k, c, 0));
    result(c, u ? user_flags_of(*u) : 0);
}

// a profile without a language or country of its own (an offline one) has
// the console's
XF_EXPORT(xam, XamUserGetOnlineLanguageFromXUID) {
    const User *u = user_by_xuid(k, arg64(k, c, 0));
    uint32_t language = u ? (u->cached_flags & 0x3E000000) >> 25 : 0;
    result(c, language ? language : config_language(k));
}

XF_EXPORT(xam, XamUserGetOnlineCountryFromXUID) {
    const User *u = user_by_xuid(k, arg64(k, c, 0));
    uint32_t country = u ? (u->cached_flags & 0xFF00) >> 8 : 0;
    result(c, country ? country : config_country(k));
}

// (user_index): the account's parental control flag (Xenia tests the
// language's bits there; an offline account has neither)
XF_EXPORT(xam, XamUserIsParentalControlled) {
    const User *u = user_at(k, arg(k, c, 0));
    result(c, u && (u->cached_flags & ACCOUNT_PARENTAL_CONTROL) ? 1 : 0);
}

// no guests sign in here
XF_EXPORT(xam, XamUserIsGuest) { result(c, 0); }

// (user_index, subscription_length_ptr, subscription_payment_ptr): the
// dashboard's settings
XF_EXPORT(xam, XamUserGetSubscriptionType) {
    uint32_t index = arg(k, c, 0), length_ptr = arg(k, c, 1), payment_ptr = arg(k, c, 2);
    if (index >= 4 || !length_ptr || !payment_ptr) return result(c, X_E_INVALIDARG);
    if (!user_at(k, index)) return result(c, X_E_NO_SUCH_USER);
    k.store32(length_ptr, uint32_t(dashboard_value(k, XPROFILE_SUBSCRIPTION_TYPE_LENGTH_IN_MONTHS)));
    k.store32(payment_ptr, uint32_t(dashboard_value(k, XPROFILE_SUBSCRIPTION_PAYMENT_TYPE)));
    result(c, X_ERROR_SUCCESS);
}

// (user_index, tenure_level_ptr, milestone_ptr, milestone_date_ptr,
// overlapped): the dashboard's settings
XF_EXPORT(xam, XamUserGetUserTenure) {
    uint32_t index = arg(k, c, 0), level_ptr = arg(k, c, 1), milestone_ptr = arg(k, c, 2);
    uint32_t date_ptr = arg(k, c, 3), ov = arg(k, c, 4);
    if (!signed_in_user(k, index)) return result(c, X_E_NO_SUCH_USER);
    if (level_ptr) k.store32(level_ptr, uint32_t(dashboard_value(k, XPROFILE_TENURE_LEVEL)));
    if (milestone_ptr) k.store32(milestone_ptr, uint32_t(dashboard_value(k, XPROFILE_TENURE_MILESTONE)));
    if (date_ptr) k.store64(date_ptr, dashboard_value(k, XPROFILE_TENURE_NEXT_MILESTONE_DATE));
    // Xenia leaves an overlapped alone; one completed here, as XAM's own
    // asynchronous calls complete theirs
    result(c, run_or_defer(k, ov, [](uint32_t &extended_error, uint32_t &length) {
               extended_error = 0, length = 0;
               return X_ERROR_SUCCESS;
           }));
}

// ---- restrictions ----

// (user_index, pc_check, result_ptr): allowed
XF_EXPORT(xam, XamUserIsUnsafeProgrammingAllowed) {
    uint32_t index = arg(k, c, 0), result_ptr = arg(k, c, 2);
    if (!result_ptr || (index != USER_INDEX_ANY && index >= 4)) return result(c, X_ERROR_INVALID_PARAMETER);
    k.store32(result_ptr, 1);
    result(c, X_ERROR_SUCCESS);
}

// (user_index, out_flags): none
XF_EXPORT(xam, XamUserContentRestrictionGetFlags) {
    uint32_t index = arg(k, c, 0), flags_ptr = arg(k, c, 1);
    if (index >= 4 && index != USER_INDEX_ANY) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!signed_in_user(k, index)) return result(c, X_ERROR_NO_SUCH_USER);
    if (flags_ptr) k.store32(flags_ptr, 0);
    result(c, X_ERROR_SUCCESS);
}

// (user_index, content_type, out_rating, out_unknown): 0x3F, which games
// take for "any"; content type 0 has no rating
XF_EXPORT(xam, XamUserContentRestrictionGetRating) {
    uint32_t index = arg(k, c, 0), type = arg(k, c, 1), rating_ptr = arg(k, c, 2), other_ptr = arg(k, c, 3);
    if (!type) return result(c, X_ERROR_NOT_FOUND);
    if (!signed_in_user(k, index)) return result(c, X_ERROR_NO_SUCH_USER);
    if (rating_ptr) k.store32(rating_ptr, 0x3F);
    if (other_ptr) k.store32(other_ptr, 0);
    result(c, X_ERROR_SUCCESS);
}

// (user_index, unknown, content_type, unknown, unknown, out_access,
// overlapped): allowed; content type 0 at once even with an overlapped,
// which stays untouched
XF_EXPORT(xam, XamUserContentRestrictionCheckAccess) {
    uint32_t type = arg(k, c, 2), access_ptr = arg(k, c, 5), ov = arg(k, c, 6);
    if (access_ptr) k.store32(access_ptr, 1);
    if (ov && type) {
        complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, X_ERROR_SUCCESS);
}

// ---- preferences (UserTracker::ReadUserPreference, WriteUserPreference):
// bit fields of the dashboard's XPROFILE_USER_PREFERENCES ----

// (user_index, mask, value_ptr, overlapped)
XF_EXPORT(xam, XamUserReadUserPreference) {
    uint32_t index = arg(k, c, 0), mask = arg(k, c, 1), value_ptr = arg(k, c, 2), ov = arg(k, c, 3);
    if (index >= 4 || !value_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, run_or_defer(k, ov, [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
               length = 0;
               if (!user_at(k, index)) {
                   extended_error = X_E_NO_SUCH_USER;
                   return X_ERROR_FUNCTION_FAILED;
               }
               std::optional<uint32_t> written = preference_written_bit(mask);
               if (!written) {
                   extended_error = X_E_INVALIDARG;
                   return X_ERROR_FUNCTION_FAILED;
               }
               uint32_t value = uint32_t(dashboard_value(k, XPROFILE_USER_PREFERENCES)) & mask;
               extended_error = X_E_SUCCESS;
               if (*written) {
                   if ((value & *written) != *written)
                       extended_error = 1; // S_FALSE: never written, the bits as they are
                   else
                       value &= ~*written;
               }
               k.store32(value_ptr, value);
               return X_ERROR_SUCCESS;
           }));
}

// (user_index, mask, value, overlapped): the bits and the field's written
// bit; ProfileSettingChanged for the user
XF_EXPORT(xam, XamUserWriteUserPreference) {
    uint32_t index = arg(k, c, 0), mask = arg(k, c, 1), value = arg(k, c, 2), ov = arg(k, c, 3);
    if (index >= 4) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, run_or_defer(k, ov, [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
               length = 0;
               if (!user_at(k, index)) {
                   extended_error = X_E_NO_SUCH_USER;
                   return X_ERROR_FUNCTION_FAILED;
               }
               std::optional<uint32_t> written = preference_written_bit(mask);
               if (!written || (mask | value) != mask || (value & *written)) {
                   extended_error = X_E_INVALIDARG;
                   return X_ERROR_FUNCTION_FAILED;
               }
               uint32_t preferences = uint32_t(dashboard_value(k, XPROFILE_USER_PREFERENCES));
               Setting v;
               v.source = 2;
               put_be32(v.value, (preferences & ~mask) | value | *written);
               {
                   UserState &s = users(k);
                   std::lock_guard<std::mutex> l(s.mutex);
                   s.written[{DASHBOARD_ID, XPROFILE_USER_PREFERENCES}] = v;
               }
               broadcast(k, XN_SYS_PROFILESETTINGCHANGED, 1u << index);
               extended_error = X_E_SUCCESS;
               return X_ERROR_SUCCESS;
           }));
}

// ---- signing in and out (ProfileManager::Login, Logout) ----

namespace {
constexpr uint32_t LOGON_FORCE_LIVE_LOG_OFF = 0x2, LOGON_ADD_USER = 0x4, LOGON_REMOVE_USER = 0x8;

void logout(Kernel &k) {
    if (set_signed_in(k, false)) broadcast(k, XN_SYS_SIGNINCHANGED, 0);
}

// a known account to its slot (the one user to slot 0); signed in already,
// it is signed out first
void login(Kernel &k, uint64_t xuid) {
    if (xuid != user_profile(k).xuid) return;
    logout(k);
    set_signed_in(k, true);
    broadcast(k, XN_SYS_SIGNINCHANGED, 1);
}
} // namespace

// (xuids (4), flags, overlapped)
XF_EXPORT(xam, XamUserLogon) {
    uint32_t xuids_ptr = arg(k, c, 0), flags = arg(k, c, 1), ov = arg(k, c, 2);
    if (!xuids_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    uint64_t xuids[4];
    for (uint32_t i = 0; i < 4; i++) xuids[i] = k.load64(xuids_ptr + 8 * i);
    result(c, run_or_defer(k, ov, [&](uint32_t &extended_error, uint32_t &length) -> uint32_t {
               if (flags & LOGON_ADD_USER)
                   for (uint64_t xuid : xuids)
                       if (xuid) login(k, xuid);
               if (flags & LOGON_REMOVE_USER)
                   for (uint64_t xuid : xuids)
                       if (xuid && user_by_xuid(k, xuid)) logout(k);
               if (flags & LOGON_FORCE_LIVE_LOG_OFF) logout(k);
               extended_error = 0, length = 0;
               return X_ERROR_SUCCESS;
           }));
}

// (X_PROFILEENUMRESULT *, flags, overlapped): accepted (Xenia's stub)
XF_EXPORT(xam, XamUserLogonEx) {
    result(c, run_or_defer(k, arg(k, c, 2), [](uint32_t &extended_error, uint32_t &length) {
               extended_error = 0, length = 0;
               return X_ERROR_SUCCESS;
           }));
}

// ---- the titles played (XTitleEnumerator) ----

// (title_id, user_index, xuid, starting_index, game_count, buffer_size_ptr,
// handle_ptr): XTITLE_PLAYED (0xA8) items; the titles the dashboard's
// records list, none here (there are no records)
XF_EXPORT(xam, XamUserCreateTitlesPlayedEnumerator) {
    uint32_t index = arg(k, c, 1), count = arg(k, c, 4), size_ptr = arg(k, c, 5), handle_ptr = arg(k, c, 6);
    if (index >= 4 || !count || !size_ptr || !handle_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!user_at(k, index)) return result(c, X_ERROR_FUNCTION_FAILED);
    k.store32(size_ptr, count * 0xA8);
    Ref<Enumerator> e = make_enumerator(k, index, 0xFB, 0xB0050, 0xB000B, 0, count, 0xA8, 0x28);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    k.store32(handle_ptr, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// Kinect's XamUserNui* (xam_nui.cc) are in xam_avatar.cpp, with the rest of Kinect.

// ---- profiles (xam_profile.cc, xam_enum.cc; ProfileManager) ----

namespace {

constexpr uint32_t ACCOUNT_SIZE = 0x17C;           // X_XAMACCOUNTINFO
constexpr uint32_t CREATION_INFO_SIZE = 0xAC0;     // X_PROFILE_CREATION_INFO
constexpr uint32_t PROFILE_CONTENT_TYPE = 0x10000; // XContentType::kProfile

struct ProfileMount {
    std::string name, device_path;
};

// The accounts (ProfileManager's): the user's and those XamProfileCreate
// made, kept for the run (no Account file is written); the profiles
// mounted by XamProfileOpen.
struct ProfileState {
    std::mutex mutex;
    std::map<uint64_t, std::vector<uint8_t>> created;
    std::vector<ProfileMount> mounts;
    uint32_t next_device = 0;
};

ProfileState &profiles(Kernel &k) { return k.state<ProfileState>(); }

// the user's X_XAMACCOUNTINFO: flags, the gamertag (16 wide characters),
// the online XUID (none offline), the cached flags
std::vector<uint8_t> user_account(const User &u) {
    std::vector<uint8_t> a(ACCOUNT_SIZE, 0);
    put_be32(a.data(), u.reserved_flags);
    for (size_t i = 0; i < u.name.size() && i < 15; i++) a[8 + 2 * i + 1] = uint8_t(u.name[i]);
    put_be32(a.data() + 0x30, u.cached_flags);
    return a;
}

std::map<uint64_t, std::vector<uint8_t>> accounts(Kernel &k) {
    ProfileState &s = profiles(k);
    std::lock_guard<std::mutex> l(s.mutex);
    std::map<uint64_t, std::vector<uint8_t>> all = s.created;
    all[user_profile(k).xuid] = user_account(user_profile(k));
    return all;
}

std::string hex16(uint64_t xuid) {
    char text[20];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(xuid));
    return text;
}

// ProfileManager::GetProfilePath: content/<XUID>/FFFE07D1/00010000/<XUID>
std::filesystem::path profile_path(Kernel &k, uint64_t xuid) {
    return content_root(k) / hex16(xuid) / "FFFE07D1" / "00010000" / hex16(xuid);
}

bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

// GenerateXuid: an offline XUID, E03 and 31 random bits
uint64_t generate_xuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    return (uint64_t(0xE03) << 52) + (gen() % (1u << 31));
}

} // namespace

// (xuid, account_ptr, device_id_ptr): the account and its device (the
// hard drive)
XF_EXPORT(xam, XamProfileFindAccount) {
    uint64_t xuid = arg64(k, c, 0);
    uint32_t account_ptr = arg(k, c, 1), device_ptr = arg(k, c, 2);
    if (!account_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    std::memset(k.host(account_ptr), 0, ACCOUNT_SIZE);
    auto all = accounts(k);
    auto it = all.find(xuid);
    if (it == all.end()) return result(c, X_ERROR_NO_SUCH_USER);
    std::memcpy(k.host(account_ptr), it->second.data(), ACCOUNT_SIZE);
    if (device_ptr) k.store32(device_ptr, 1);
    result(c, X_ERROR_SUCCESS);
}

// (device_id, xuid, account_ptr)
XF_EXPORT(xam, XamProfileLoadAccountInfo) {
    uint64_t xuid = arg64(k, c, 1);
    uint32_t account_ptr = arg(k, c, 2);
    if (!account_ptr || !xuid) return result(c, X_ERROR_INVALID_PARAMETER);
    auto all = accounts(k);
    auto it = all.find(xuid);
    if (it == all.end()) return result(c, X_ERROR_INVALID_PARAMETER);
    std::memcpy(k.host(account_ptr), it->second.data(), ACCOUNT_SIZE);
    result(c, X_ERROR_SUCCESS);
}

// (xuid, mount_path, flags, XCONTENT_DATA_AGGREGATE *): the profile's
// package mounted (as User_<XUID> with no path) and described
XF_EXPORT(xam, XamProfileOpen) {
    uint64_t xuid = arg64(k, c, 0);
    uint32_t mount_ptr = arg(k, c, 1), data_ptr = arg(k, c, 3);
    std::string name = mount_ptr ? std::string(reinterpret_cast<const char *>(k.host(mount_ptr))) : "";
    if (name.empty()) name = "User_" + hex16(xuid);
    std::filesystem::path path = profile_path(k, xuid);
    std::error_code ec;
    if (content_root(k).empty() || !std::filesystem::is_directory(path, ec))
        return result(c, X_ERROR_INVALID_PARAMETER);
    {
        ProfileState &s = profiles(k);
        std::lock_guard<std::mutex> l(s.mutex);
        ProfileMount m{name, "\\Device\\Profile\\" + std::to_string(++s.next_device)};
        auto device = vfs::make_host_device(m.device_path, path, false, true);
        vfs::FileSystem &fs = file_system(k);
        if (!device || !fs.add_link(name + ":", m.device_path)) return result(c, X_ERROR_INVALID_PARAMETER);
        fs.add_device(device);
        s.mounts.push_back(m);
    }
    if (data_ptr) {
        // XCONTENT_DATA: device, type, display name, file name (42); then
        // the XUID and the title (the dashboard's)
        k.store32(data_ptr + 0x0, 1);
        k.store32(data_ptr + 0x4, PROFILE_CONTENT_TYPE);
        std::string file = hex16(xuid);
        std::memset(k.host(data_ptr + 0x108), 0, 42);
        std::memcpy(k.host(data_ptr + 0x108), file.data(), file.size());
        k.store32(data_ptr + 0x140, DASHBOARD_ID);
    }
    result(c, X_ERROR_SUCCESS);
}

// (mount_name): what XamProfileOpen mounted
XF_EXPORT(xam, XamProfileClose) {
    uint32_t name_ptr = arg(k, c, 0);
    std::string name = name_ptr ? reinterpret_cast<const char *>(k.host(name_ptr)) : "";
    ProfileState &s = profiles(k);
    std::lock_guard<std::mutex> l(s.mutex);
    auto it =
        std::find_if(s.mounts.begin(), s.mounts.end(), [&](const ProfileMount &m) { return same_name(m.name, name); });
    if (it == s.mounts.end()) return result(c, X_ERROR_FUNCTION_FAILED);
    vfs::FileSystem &fs = file_system(k);
    fs.remove_link(it->name + ":");
    fs.remove_device(it->device_path);
    s.mounts.erase(it);
    result(c, X_ERROR_SUCCESS);
}

// (flags, device_id_ptr, xuid, account, payment_info, user_token,
// owner_token, profile_info_ptr): X_PROFILE_CREATION_INFO in the system
// heap, and the profile's directory and account (Xenia does it at once
// too; the console schedules a task)
XF_EXPORT(xam, XamProfileCreate) {
    uint32_t flags = arg(k, c, 0), device_ptr = arg(k, c, 1);
    uint64_t xuid = arg64(k, c, 2);
    uint32_t account = arg(k, c, 3), payment = arg(k, c, 4), user_token = arg(k, c, 5);
    uint32_t owner_token = arg(k, c, 6), info_ptr = arg(k, c, 7);
    uint32_t info = k.memory().system_alloc(CREATION_INFO_SIZE);
    if (!info) return result(c, X_ERROR_NOT_ENOUGH_MEMORY);
    std::memset(k.host(info), 0, CREATION_INFO_SIZE);
    if (info_ptr) k.store32(info_ptr, info);
    // flags, device, account (0x8), payment (0x184), offline XUID (0xA78),
    // the tokens (0xA80, 0xA9C)
    k.store32(info, flags & 0x7FFFFFFF);
    if (device_ptr) {
        k.store32(device_ptr, 1);
        k.store32(info + 4, 1);
    }
    if (!xuid) xuid = generate_xuid();
    k.store64(info + 0xA78, xuid);
    if (account) std::memcpy(k.host(info + 0x8), k.host(account), ACCOUNT_SIZE);
    if (payment) std::memcpy(k.host(info + 0x184), k.host(payment), 0x8F0);
    if (user_token) std::memcpy(k.host(info + 0xA80), k.host(user_token), 0x1C);
    if (owner_token) std::memcpy(k.host(info + 0xA9C), k.host(owner_token), 0x1C);
    // ProfileManager::CreateProfile: a new directory, else it fails
    std::error_code ec;
    if (content_root(k).empty() || !std::filesystem::create_directories(profile_path(k, xuid), ec))
        return result(c, X_ERROR_INVALID_PARAMETER);
    ProfileState &s = profiles(k);
    std::lock_guard<std::mutex> l(s.mutex);
    s.created[xuid].assign(k.host(info + 0x8), k.host(info + 0x8) + ACCOUNT_SIZE);
    result(c, X_ERROR_SUCCESS);
}

// (profile_info, offline_xuid_ptr): the XUID, and the creation info freed
XF_EXPORT(xam, XamProfileGetCreationStatus) {
    uint32_t info = arg(k, c, 0), xuid_ptr = arg(k, c, 1);
    if (!info) return result(c, X_ERROR_SUCCESS);
    if (xuid_ptr) k.store64(xuid_ptr, k.load64(info + 0xA78));
    k.memory().system_free(info);
    result(c, X_ERROR_SUCCESS);
}

// (device_id, handle_ptr): the accounts, X_PROFILEENUMRESULT (0x188: the
// XUID, the account, the device) one a call
XF_EXPORT(xam, XamProfileCreateEnumerator) {
    uint32_t handle_ptr = arg(k, c, 1);
    if (!handle_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    Ref<Enumerator> e = make_enumerator(k, USER_INDEX_ANY, 0xFE, 0x23001, 0x23003, 0, 1, 0x188, 0x28);
    if (!e) return result(c, X_STATUS_NO_MEMORY);
    for (const auto &[xuid, account] : accounts(k)) {
        uint8_t *item = e->append();
        for (int i = 0; i < 8; i++) item[i] = uint8_t(xuid >> (56 - 8 * i));
        std::memcpy(item + 8, account.data(), ACCOUNT_SIZE);
        put_be32(item + 8 + ACCOUNT_SIZE, 1);
    }
    k.store32(handle_ptr, e->handle());
    result(c, X_ERROR_SUCCESS);
}

// (handle, flags, buffer, overlapped): xeXamEnumerate with no buffer size
// (the profile enumerator writes regardless)
XF_EXPORT(xam, XamProfileEnumerate) {
    uint32_t handle = arg(k, c, 0), buffer = arg(k, c, 2), ov = arg(k, c, 3);
    Ref<Enumerator> e = objects(k).lookup<Enumerator>(handle);
    if (!e) return result(c, X_ERROR_INVALID_HANDLE);
    auto run = [&](uint32_t &extended_error, uint32_t &length) {
        uint32_t count = 0;
        uint32_t r = buffer ? e->write_items(k, buffer, UINT32_MAX, count) : X_ERROR_INVALID_PARAMETER;
        extended_error = hresult_from_win32(r);
        length = count;
        if (r && ov) r = X_ERROR_FUNCTION_FAILED;
        return r;
    };
    result(c, run_or_defer(k, ov, run));
}

// (input: xam_input.cpp; the guide's UI: xam_ui.cpp)

} // namespace xf::kernel
