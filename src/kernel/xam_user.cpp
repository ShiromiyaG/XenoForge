/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's users and UI (xam.h): one user signed in offline in slot 0, its
// profile settings, and the guide's UI answered as Xenia answers it
// headless (the input exports are in xam_input.cpp). Translated from Xenia
// (BSD; see LICENSE-xenia): xam_user.cc, user_settings.h and .cc, user_data.cc,
// user_tracker.cc (GetUserSetting), xam_ui.cc (the headless paths);
// checked against them.
//
// Where it differs from Xenia, and why:
//   - the user is Xenia's default profile (XUID 0xB13EBABEBABEBABE, or the
//     first profile in the content root) named "User", without its account
//     file or GPDs: settings a title writes are kept for the run, not in
//     the profile's GPD;
//   - the UI's notifications (XN_SYS_UI on, then off) go out together
//     around the answer (Xenia turns it off 200 ms later).

#include "xam.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

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

// (title_id, user_index, xuid_count, xuids, setting_count, setting_ids,
// buffer_size_ptr, buffer, overlapped): X_USER_READ_PROFILE_SETTINGS (the
// count, the settings' address), the settings, their strings and binaries
XF_EXPORT(xam, XamUserReadProfileSettings) {
    uint32_t title = arg(k, c, 0), index = arg(k, c, 1), xuid_count = arg(k, c, 2), xuids = arg(k, c, 3);
    uint32_t count = arg(k, c, 4), ids = arg(k, c, 5), size_ptr = arg(k, c, 6), buffer = arg(k, c, 7);
    uint32_t ov = arg(k, c, 8);
    if (count < 1 || count > 32 || !size_ptr) return result(c, X_ERROR_INVALID_PARAMETER);
    uint32_t size = k.load32(size_ptr);
    if (size && !buffer) return result(c, X_ERROR_INVALID_PARAMETER);
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
        return result(c, X_ERROR_INSUFFICIENT_BUFFER);
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
    result(c, run_or_defer(k, ov, run));
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

// (input: xam_input.cpp)

// ---- the guide's UI (xam_ui.cc, headless) ----

namespace {

// xeXamDispatchHeadless: the UI shows, the answer is made, the UI goes
uint32_t headless(Kernel &k, uint32_t ov, const std::function<uint32_t()> &answer) {
    broadcast(k, XN_SYS_UI, 1);
    uint32_t r;
    if (ov) {
        r = complete_overlapped_deferred(k, ov, [&](uint32_t &extended_error, uint32_t &length) {
            uint32_t a = answer();
            extended_error = a, length = 0;
            return a;
        });
    } else {
        r = answer();
    }
    broadcast(k, XN_SYS_UI, 0);
    return r;
}

// xeXamDispatchHeadlessAsync: the answer is made after the export returns
uint32_t headless_async(Kernel &k, const std::function<void()> &answer) {
    broadcast(k, XN_SYS_UI, 1);
    answer();
    broadcast(k, XN_SYS_UI, 0);
    return X_ERROR_SUCCESS;
}

// a message box: the focused button
uint32_t message_box(Kernel &k, uint32_t active_button, uint32_t result_ptr, uint32_t ov) {
    return headless(k, ov, [&] {
        if (result_ptr) k.store32(result_ptr, active_button); // MESSAGEBOX_RESULT
        return X_ERROR_SUCCESS;
    });
}

} // namespace

XF_EXPORT(xam, XamIsUIActive) { result(c, 0); }

// (user_index, title, text, button_count, buttons, active_button, flags,
// result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUI) { result(c, message_box(k, arg(k, c, 5), arg(k, c, 7), arg(k, c, 8))); }

// (user_index, title, text, button_count, buttons, active_button, flags,
// unused, result, overlapped)
XF_EXPORT(xam, XamShowMessageBoxUIEx) { result(c, message_box(k, arg(k, c, 5), arg(k, c, 8), arg(k, c, 9))); }

// (user_index, flags, default_text, title, description, buffer, length,
// overlapped): the default text is the answer
XF_EXPORT(xam, XamShowKeyboardUI) {
    uint32_t default_text = arg(k, c, 2), buffer = arg(k, c, 5), length = arg(k, c, 6), ov = arg(k, c, 7);
    if (!buffer) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, headless(k, ov, [&] {
               if (!default_text)
                   std::memset(k.host(buffer), 0, size_t(length) * 2);
               else
                   put_u16(k, buffer, guest_u16(k, default_text), length);
               return X_ERROR_SUCCESS;
           }));
}

// (user_index, content_type, content_flags, total_requested, device_id_ptr,
// overlapped): the hard drive
XF_EXPORT(xam, XamShowDeviceSelectorUI) {
    uint32_t index = arg(k, c, 0), flags = arg(k, c, 2), device_ptr = arg(k, c, 4), ov = arg(k, c, 5);
    if (!ov) return result(c, X_ERROR_INVALID_PARAMETER);
    if ((index >= 4 && index != USER_INDEX_ANY) || (flags & 0x83F00008) || !device_ptr) {
        k.store32(ov + xoverlapped::EXTENDED_ERROR, X_ERROR_INVALID_PARAMETER);
        return result(c, X_ERROR_INVALID_PARAMETER);
    }
    result(c, headless(k, ov, [&] {
               k.store32(device_ptr, 1); // the HDD (xam_content.cpp's devices)
               return X_ERROR_SUCCESS;
           }));
}

// (users_needed, flags): the user is signed in already; the games wait for
// the notification
XF_EXPORT(xam, XamShowSigninUI) {
    uint32_t needed = arg(k, c, 0);
    if (needed != 1 && needed != 2 && needed != 4) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, headless_async(k, [&] { broadcast(k, XN_SYS_SIGNINCHANGED, 1); }));
}

// (user_index): the title could not read its disc; said here, as Xenia's
// dialog says it
XF_EXPORT(xam, XamShowDirtyDiscErrorUI) {
    std::fprintf(stderr, "xam: the title reports a disc read error (XamShowDirtyDiscErrorUI, from 0x%08X)\n",
                 uint32_t(c.lr));
    result(c, X_ERROR_SUCCESS);
}

// (user_index, ui_type, offer_id, offer_type, category, ...): nothing to buy
XF_EXPORT(xam, XamShowMarketplaceUI) {
    if (arg(k, c, 0) >= 4) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!signed_in(k, arg(k, c, 0))) return result(c, X_ERROR_NO_SUCH_USER);
    result(c, headless_async(k, [] {}));
}

// (user_index, entry_point, offers, offer_count, hresult_ptr, overlapped)
XF_EXPORT(xam, XamShowMarketplaceDownloadItemsUI) {
    uint32_t hresult_ptr = arg(k, c, 4), ov = arg(k, c, 5);
    if (arg(k, c, 0) >= 4 || !arg(k, c, 2) || arg(k, c, 3) > 6) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!signed_in(k, arg(k, c, 0))) {
        if (!ov) return result(c, X_ERROR_NO_SUCH_USER);
        complete_overlapped_immediate(k, ov, X_ERROR_NO_SUCH_USER);
        return result(c, X_ERROR_IO_PENDING);
    }
    result(c, headless(k, ov, [&] {
               if (hresult_ptr) k.store32(hresult_ptr, X_E_SUCCESS);
               return X_ERROR_SUCCESS;
           }));
}

// (user_index, title_id)
XF_EXPORT(xam, XamShowAchievementsUI) {
    if (!user_at(k, arg(k, c, 0))) return result(c, X_ERROR_NO_SUCH_USER);
    result(c, headless_async(k, [] {}));
}

} // namespace xf::kernel
