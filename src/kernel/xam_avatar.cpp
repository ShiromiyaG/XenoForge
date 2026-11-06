/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's avatars, Kinect and gamer tiles (plan step A4; docs/runtime.md,
// "XAM"), as Xenia answers with no avatar system, no Kinect and no profile
// images: the avatar exports complete at once with nothing, Kinect is not
// initialized (its title initialization kept, NuiState in xam.h), and no
// tile is found. Translated from Xenia (BSD; see LICENSE-xenia):
// xam_avatar.cc, xam_avatar.h, xam_nui.cc, nui.h and nui.cc, xam_user.cc
// (the tiles), user_settings.h (GamerPictureKey), title_id_utils.h; checked
// against them. Where it differs, and why:
//   - XamAvatarSetManifest keeps the manifest in memory for the session
//     (Xenia writes it to the dashboard's profile settings), so
//     XamAvatarGetManifestLocalUser finds it as Xenia's does;
//   - XamAvatarGetInstalledAssetPackageDescription writes the asset's title
//     ID and the file name from its fields as big-endian values, the whole
//     XCONTENT_DATA_AGGREGATE zeroed first (Xenia reads the X_ASSET_ID's
//     fields unswapped and leaves the rest uninitialized);
//   - XamWriteGamerTile succeeds without writing the pictures: they are the
//     SPA's images, which are not read here (Xenia writes them under the
//     dashboard's gamer pictures);
//   - XamReadTileToTexture never decodes a picture, as none is found;
//   - the device status and the tilt calls answer as the console's XAM does
//     with no sensor (xfxam): XamNuiGetDeviceStatus success with the status
//     bits 3, the tilt status and angle E_DEVICE_NOT_CONNECTED, the tilt
//     controller type none, whatever the retail flags say (Xenia: E_FAIL, and
//     the type 1 with the flags' Kinect bit).

#include "xam.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace xf::kernel {
namespace {

constexpr uint32_t AVATAR_EDITOR_ID = 0x584D07D1; // kAvatarEditorID
constexpr uint32_t AVATAR_METADATA_SIZE = 0x3E8;  // X_AVATAR_METADATA, kMaxUserDataSize
constexpr uint32_t CONTENT_TYPE_AVATAR_ITEM = 0x00009000;
constexpr uint32_t HR_NO_MORE_FILES = 0x80070012; // X_E_NO_MORE_FILES
constexpr uint32_t RETAIL_KINECT_INITIALIZED = 0x20000000;
constexpr uint32_t STATUS_NO_SUCH_FILE_NT = 0xC000000F | 0x10000000; // X_STATUS_NO_SUCH_FILE | X_FACILITY_NT_BIT

struct AvatarState {
    std::mutex mutex;
    std::vector<uint8_t> manifest; // the user's, once set
};

AvatarState &avatars(Kernel &k) { return k.state<AvatarState>(); }
NuiState &nui(Kernel &k) { return k.state<NuiState>(); }

// what an avatar request with nothing to do answers: an overlapped one
// completes at once
uint32_t done(Kernel &k, uint32_t ov) {
    if (!ov) return X_STATUS_SUCCESS;
    complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
}

// the run of an avatar request that reports an extended error: its result
// at once, or the extended error when it failed; deferred with an overlapped
uint32_t run_avatar(Kernel &k, uint32_t ov, const OverlappedRun &run) {
    if (ov) return complete_overlapped_deferred(k, ov, run);
    uint32_t extended_error = 0, length = 0;
    uint32_t r = run(extended_error, length);
    return r == X_ERROR_SUCCESS ? r : extended_error;
}

// NUIController::NuiInitialized: the retail flags say a Kinect was set up
bool nui_initialized(Kernel &k) {
    return config_setting32(k, 3, 0x0C, 0) & RETAIL_KINECT_INITIALIZED; // XCONFIG_USER_RETAIL_FLAGS
}

// XamNuiHudIsEnabled: the NUI data (always there in Xenia), its unknown flag
// (set), and the device ready
bool hud_enabled(Kernel &k) { return nui_initialized(k); }

} // namespace

// ---- avatars ----

// (coordinate_system, unknown, processor, function_ptrs, data, flags): only
// the avatar editor gets one (Xenia's allow_avatar_initialization, off)
XF_EXPORT(xam, XamAvatarInitialize) { result(c, title_id(k) == AVATAR_EDITOR_ID ? X_STATUS_SUCCESS : ~0u); }
XF_EXPORT(xam, XamAvatarShutdown) {}

// (user_index, X_AVATAR_METADATA *, overlapped): the manifest set for the
// user, else X_E_FAIL (the profile has none)
XF_EXPORT(xam, XamAvatarGetManifestLocalUser) {
    uint32_t index = arg(k, c, 0), metadata = arg(k, c, 1), ov = arg(k, c, 2);
    auto run = [&k, index, metadata](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        if (index >= 4 || !metadata) {
            extended_error = X_E_INVALIDARG;
            return X_ERROR_INVALID_PARAMETER;
        }
        if (!user_at(k, index)) {
            extended_error = X_E_NO_SUCH_USER;
            return X_ERROR_FUNCTION_FAILED;
        }
        AvatarState &s = avatars(k);
        std::lock_guard<std::mutex> l(s.mutex);
        if (s.manifest.empty()) {
            extended_error = X_E_FAIL;
            return X_ERROR_FUNCTION_FAILED;
        }
        std::memcpy(k.host(metadata), s.manifest.data(), s.manifest.size());
        return X_ERROR_SUCCESS;
    };
    result(c, run_avatar(k, ov, run));
}

// (user_index, X_AVATAR_METADATA *, overlapped)
XF_EXPORT(xam, XamAvatarSetManifest) {
    uint32_t index = arg(k, c, 0), metadata = arg(k, c, 1), ov = arg(k, c, 2);
    auto run = [&k, index, metadata](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        if (!user_at(k, index)) {
            extended_error = X_E_NO_SUCH_USER;
            return X_ERROR_FUNCTION_FAILED;
        }
        AvatarState &s = avatars(k);
        std::lock_guard<std::mutex> l(s.mutex);
        if (metadata)
            s.manifest.assign(k.host(metadata), k.host(metadata) + AVATAR_METADATA_SIZE);
        else
            s.manifest.clear();
        return X_STATUS_SUCCESS;
    };
    result(c, run_avatar(k, ov, run));
}

// (user_index, xuid_count, xuids, unknown, avatar_info, overlapped)
XF_EXPORT(xam, XamAvatarGetManifestsByXuid) { result(c, done(k, arg(k, c, 5))); }

// (component_mask, result_size_ptr, gpu_resource_size_ptr): nothing to hold
XF_EXPORT(xam, XamAvatarGetAssetsResultSize) {
    if (uint32_t p = arg(k, c, 1)) k.store32(p, 0);
    if (uint32_t p = arg(k, c, 2)) k.store32(p, 0);
    result(c, X_STATUS_SUCCESS);
}

// (metadata, component_mask, flags, result_buffer, gpu_resource_buffer,
// overlapped): an overlapped request fails (58410907 goes on with that)
XF_EXPORT(xam, XamAvatarGetAssets) {
    uint32_t ov = arg(k, c, 5);
    if (!ov) return result(c, X_STATUS_SUCCESS);
    complete_overlapped_immediate_ex(k, ov, X_ERROR_FUNCTION_FAILED, X_E_FAIL, 0);
    result(c, X_ERROR_IO_PENDING);
}

// (buffer_size, asset_data, custom_color_count, custom_colors, metadata)
XF_EXPORT(xam, XamAvatarSetCustomAsset) { result(c, X_STATUS_SUCCESS); }
// (body_type, count, metadata, overlapped)
XF_EXPORT(xam, XamAvatarGetMetadataRandom) { result(c, done(k, arg(k, c, 3))); }
// (count_ptr, overlapped): the count left as it was
XF_EXPORT(xam, XamAvatarGetMetadataSignedOutProfileCount) { result(c, done(k, arg(k, c, 1))); }
// (profile_index, metadata, overlapped)
XF_EXPORT(xam, XamAvatarGetMetadataSignedOutProfile) { result(c, done(k, arg(k, c, 2))); }
// (metadata): male
XF_EXPORT(xam, XamAvatarManifestGetBodyType) { result(c, 1); }

// (unknown (64 bits), out): 0, and 1
XF_EXPORT(xam, XamAvatarGetInstrumentation) {
    if (uint32_t p = arg(k, c, 1)) k.store32(p, 0);
    result(c, 1);
}

// (unknown, unknown, unknown, unknown, overlapped)
XF_EXPORT(xam, XamAvatarGetAssetIcon) { result(c, done(k, arg(k, c, 4))); }
XF_EXPORT(xam, XamAvatarGetAssetBinary) { result(c, done(k, arg(k, c, 4))); }

// (X_ASSET_ID *, XCONTENT_DATA_AGGREGATE *): the package an awarded asset
// is installed as: an avatar item of the asset's title, named by the asset
XF_EXPORT(xam, XamAvatarGetInstalledAssetPackageDescription) {
    uint32_t asset = arg(k, c, 0), out = arg(k, c, 1);
    if (!asset || !out) return;
    uint64_t data = k.load64(asset);
    uint32_t data2 = k.load32(asset + 8), title = k.load32(asset + 0xC);
    std::memset(k.host(out), 0, 0x148);
    k.store32(out + 4, CONTENT_TYPE_AVATAR_ITEM);
    char name[48];
    std::snprintf(name, sizeof(name), "%016llX%08X%08X", (unsigned long long)data, data2, title);
    std::memcpy(k.host(out + 0x108), name, std::min<size_t>(std::strlen(name), 42));
    k.store64(out + 0x138, 0);
    k.store32(out + 0x140, title);
}

XF_EXPORT(xam, XamAvatarSetMocks) {}

// (asset_id_ptr, flags, output, overlapped): logged, nothing loaded
XF_EXPORT(xam, XamAvatarLoadAnimation) {
    uint32_t asset = arg(k, c, 0);
    std::fprintf(stderr, "xam: XamAvatarLoadAnimation 0x%016llX\n", (unsigned long long)(asset ? k.load64(asset) : 0));
    result(c, done(k, arg(k, c, 3)));
}

// (assets, flags, buffer_size, mip_map_buffer, overlapped)
XF_EXPORT(xam, XamAvatarGenerateMipMaps) { result(c, done(k, arg(k, c, 4))); }
// (unknown, unknown, unknown, unknown (16 bits), unknown, overlapped)
XF_EXPORT(xam, XamAvatarBeginEnumAssets) { result(c, done(k, arg(k, c, 5))); }
// (buffer, unknown, overlapped): none, so the caller stops asking
XF_EXPORT(xam, XamAvatarEnumAssets) { result(c, HR_NO_MORE_FILES); }
// (overlapped)
XF_EXPORT(xam, XamAvatarEndEnumAssets) { result(c, done(k, arg(k, c, 0))); }

// (unknown (64 bits), unknown, overlapped): the avatar editor's own request
// succeeds as it is (its overlapped untouched)
XF_EXPORT(xam, XamAvatarWearNow) {
    if (title_id(k) == AVATAR_EDITOR_ID) return result(c, X_ERROR_SUCCESS);
    uint32_t ov = arg(k, c, 2);
    if (!ov) return result(c, X_ERROR_SUCCESS);
    complete_overlapped_immediate(k, ov, X_ERROR_SUCCESS);
    result(c, X_ERROR_IO_PENDING);
}

// (string_out, string_size, unknown_ptr)
XF_EXPORT(xam, XamAvatarReinstallAwardedAsset) { result(c, X_ERROR_SUCCESS); }

// ---- Kinect ----

// (X_NUI_DEVICE_STATUS *: camera, tilt, microphone, status bits): the console's
// XAM, with no sensor (the kernel's UsbdGetNatalHardwareVersion answering 0),
// answers success whatever the retail flags say, the three statuses zero and
// the status bits 3 (xfxam; Xenia answers 0xC0050006 with the bits 0 when
// Kinect is not initialized, and success with the bits 1 when it is)
XF_EXPORT(xam, XamNuiGetDeviceStatus) {
    if (uint32_t status = arg(k, c, 0)) {
        std::memset(k.host(status), 0, 0x10);
        k.store32(status + 0xC, 3);
    }
    result(c, X_ERROR_SUCCESS);
}

XF_EXPORT(xam, XamNuiIsDeviceReady) { result(c, nui_initialized(k) ? 1 : 0); }
// the console's XAM answers 1, with no sensor and no call made (xfxam); Xenia 0
XF_EXPORT(xam, XamNuiIsChatMicEnabled) { result(c, 1); }
// (unknown, unknown): no automation, no playback, yet success (as XAM answers)
XF_EXPORT(xam, XamIsNuiAutomationEnabled) { result(c, X_ERROR_SUCCESS); }
XF_EXPORT(xam, XamIsNatalPlaybackEnabled) { result(c, X_ERROR_SUCCESS); }
// the guide's Kinect UI: never up (answered at once)
XF_EXPORT(xam, XamIsNuiUIActive) { result(c, 0); }

// (unknown, index_ptr)
XF_EXPORT(xam, XamUserNuiGetUserIndex) { result(c, X_E_NO_SUCH_USER); }

// (user_index_ptr): the first signed-in user
XF_EXPORT(xam, XamUserNuiGetUserIndexForSignin) {
    uint32_t out = arg(k, c, 0);
    if (!out) return result(c, X_E_INVALIDARG);
    for (uint32_t i = 0; i < 4; i++) {
        if (user_at(k, i)) {
            k.store32(out, i);
            return result(c, X_E_SUCCESS);
        }
    }
    result(c, X_E_FAIL);
}

// (user_index_ptr)
XF_EXPORT(xam, XamUserNuiGetUserIndexForBind) { result(c, arg(k, c, 0) ? X_E_FAIL : X_E_INVALIDARG); }
// (user_index, enable)
XF_EXPORT(xam, XamUserNuiEnableBiometric) { result(c, X_E_INVALIDARG); }

// (X_NUI_DEPTH_CALIBRATION *): cleared; no calibration
XF_EXPORT(xam, XamNuiGetDepthCalibration) {
    if (uint32_t p = arg(k, c, 0)) std::memset(k.host(p), 0, 0x30);
    result(c, STATUS_NO_SUCH_FILE_NT);
}

// (unknown): no skeleton, -1 in 64 bits
XF_EXPORT(xam, XamNuiSkeletonGetBestSkeletonIndex) { c.r[3] = ~0ull; }

// (callback): kept
XF_EXPORT(xam, XamNuiCameraTiltSetCallback) {
    NuiState &n = nui(k);
    std::lock_guard<std::mutex> l(n.mutex);
    n.tilt_callback = arg(k, c, 0);
    result(c, X_ERROR_SUCCESS);
}

// The tilt exports follow the console's XAM, which asks the kernel for the
// sensor's hardware version (UsbdGetNatalHardwareVersion) first and, with no
// sensor, answers E_DEVICE_NOT_CONNECTED (HRESULT of ERROR_DEVICE_NOT_CONNECTED)
// whatever the request, a zeroed one too (xfxam). Xenia answers E_FAIL, and
// takes Kinect's controller type from the retail flags.

// (X_NUI_TILT_STATUS *: its size, 0x50 or more, and 'XtSs', ...): no sensor
XF_EXPORT(xam, XamNuiCameraTiltGetStatus) { result(c, X_E_DEVICE_NOT_CONNECTED); }

// (angle_ptr, moving_ptr): the status is asked first, and fails the same
XF_EXPORT(xam, XamNuiCameraElevationGetAngle) { result(c, X_E_DEVICE_NOT_CONNECTED); }

// the controller type, from the sensor's hardware version: none
XF_EXPORT(xam, XamNuiCameraGetTiltControllerType) { result(c, 0); }

// (unknown (64 bits), unknown): type none: not connected
XF_EXPORT(xam, XamNuiCameraSetFlags) { result(c, X_E_DEVICE_NOT_CONNECTED); }

// (tracking_id): kept (the NUI data is always there)
XF_EXPORT(xam, XamNuiHudSetEngagedTrackingID) {
    if (uint32_t id = arg(k, c, 0)) {
        NuiState &n = nui(k);
        std::lock_guard<std::mutex> l(n.mutex);
        n.engaged_tracking_id = id;
    }
    result(c, X_STATUS_SUCCESS);
}

// 64 bits
XF_EXPORT(xam, XamNuiHudGetEngagedTrackingID) {
    NuiState &n = nui(k);
    std::lock_guard<std::mutex> l(n.mutex);
    c.r[3] = n.engaged_tracking_id;
}

XF_EXPORT(xam, XamNuiHudIsEnabled) { result(c, hud_enabled(k) ? 1 : 0); }

XF_EXPORT(xam, XamNuiHudGetInitializeFlags) {
    NuiState &n = nui(k);
    std::lock_guard<std::mutex> l(n.mutex);
    result(c, n.init_flags);
}

// (xam_version_ptr, xtl_version_ptr): 64-bit each, as XNuiTitleInitialize gave them
XF_EXPORT(xam, XamNuiHudGetVersions) {
    NuiState &n = nui(k);
    std::lock_guard<std::mutex> l(n.mutex);
    if (uint32_t p = arg(k, c, 0)) k.store64(p, n.versions[0]);
    if (uint32_t p = arg(k, c, 1)) k.store64(p, n.versions[1]);
}

// 64 bits: made once (Xenia's fixed one; XAM draws it from XeCryptRandom)
XF_EXPORT(xam, XamNuiIdentityGetSessionId) {
    NuiState &n = nui(k);
    std::lock_guard<std::mutex> l(n.mutex);
    if (!n.session_id) n.session_id = 0xDEADF00DDEADF00Dull;
    c.r[3] = n.session_id;
}

// (unknown, unknown (64 bits), unknown (64 bits), unknown)
XF_EXPORT(xam, XamNuiIdentityEnrollForSignIn) { result(c, X_E_FAIL); }
// (unknown)
XF_EXPORT(xam, XamNuiIdentityAbort) { result(c, X_E_FAIL); }

// (unknown, unknown, X_NUI_PLAYER_ENGAGEMENT_UPDATE *): cleared
XF_EXPORT(xam, XamNuiPlayerEngagementUpdate) {
    if (uint32_t p = arg(k, c, 2)) std::memset(k.host(p), 0, 0x1C);
}

// ---- gamer tiles (xam_user.cc): no picture is ever found ----

namespace {

// XamReadTile: the user by index, else the one whose XUID the item ID is;
// the tile is looked up (none here)
uint32_t read_tile(Kernel &k, uint32_t user_index, uint64_t item_id, uint32_t output, uint32_t size_ptr, uint32_t ov) {
    if (!user_at(k, user_index) && !user_by_xuid(k, item_id)) return X_ERROR_INVALID_PARAMETER;
    if (!size_ptr) return X_ERROR_INVALID_PARAMETER;
    auto run = [&k, output, size_ptr](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        uint32_t r = X_ERROR_FILE_NOT_FOUND; // an empty tile
        k.store32(size_ptr, 0);
        if (!output) r = X_ERROR_INSUFFICIENT_BUFFER;
        extended_error = hresult_from_win32(r);
        length = 0;
        return r;
    };
    return run_or_defer(k, ov, run);
}

} // namespace

// (tile_type, title_id, item_id (64 bits), user_index, output, size_ptr,
// overlapped)
XF_EXPORT(xam, XamReadTile) {
    result(c, read_tile(k, arg(k, c, 3), arg64(k, c, 2), arg(k, c, 4), arg(k, c, 5), arg(k, c, 6)));
}

// (tile_type, game_id, item_id (64 bits), offset, unknown, unknown, output,
// size_ptr, overlapped): XamReadTile with the offset as the user index, as
// Xenia passes it
XF_EXPORT(xam, XamReadTileEx) {
    result(c, read_tile(k, arg(k, c, 3), arg64(k, c, 2), arg(k, c, 6), arg(k, c, 7), arg(k, c, 8)));
}

// (tile_type, title_id, tile_id (64 bits), user_index, buffer, stride,
// height, overlapped): no tile, so nothing written and success (the
// overlapped untouched, as Xenia's)
XF_EXPORT(xam, XamReadTileToTexture) {
    if (!arg(k, c, 4)) return result(c, X_ERROR_INVALID_PARAMETER);
    if (!user_at(k, arg(k, c, 3))) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, X_ERROR_SUCCESS);
}

// (user_index, title_id, big_tile_id, small_tile_id, for_enumerate,
// overlapped): XUserAwardGamerPicture
XF_EXPORT(xam, XamWriteGamerTile) {
    uint32_t index = arg(k, c, 0), title = arg(k, c, 1), big = arg(k, c, 2), small = arg(k, c, 3);
    uint32_t ov = arg(k, c, 5);
    if (index >= 4) return result(c, X_ERROR_FUNCTION_FAILED);
    auto run = [&k, index, title, big, small](uint32_t &extended_error, uint32_t &length) -> uint32_t {
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        if (!user_at(k, index)) {
            extended_error = X_E_INVALIDARG;
            return X_ERROR_FUNCTION_FAILED;
        }
        std::fprintf(stderr, "xam: gamer picture %08x%08x%08x awarded (no picture to write)\n",
                     title ? title : title_id(k), big, small);
        return X_ERROR_SUCCESS;
    };
    result(c, run_avatar(k, ov, run));
}

// (X_USER_DATA *key, title_id_ptr, big_tile_id_ptr, small_tile_id_ptr): a
// wide string of 24 hex digits, the title's and the two tiles' IDs
XF_EXPORT(xam, XamParseGamerTileKey) {
    uint32_t key = arg(k, c, 0), title_ptr = arg(k, c, 1), big_ptr = arg(k, c, 2), small_ptr = arg(k, c, 3);
    if (!key) return result(c, X_ERROR_INVALID_PARAMETER);
    if (k.host(key)[0] != 4) return result(c, X_ERROR_SUCCESS); // not a WSTRING: not parsed
    uint32_t size = k.load32(key + 8), text = k.load32(key + 0xC);
    if (size > 0x64 || !text) return result(c, X_ERROR_INVALID_PARAMETER);
    // Xenia measures the key in UTF-8: 24 bytes
    std::u16string wide = guest_u16(k, text);
    size_t utf8_size = 0;
    for (size_t i = 0; i < wide.size(); i++) {
        char16_t ch = wide[i];
        bool pair = ch >= 0xD800 && ch < 0xDC00 && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 && wide[i + 1] < 0xE000;
        utf8_size += ch < 0x80 ? 1 : ch < 0x800 ? 2 : pair ? 4 : 3;
        if (pair) i++;
    }
    if (utf8_size != 24) return result(c, X_ERROR_INVALID_PARAMETER);
    std::string digits;
    for (char16_t ch : wide) {
        if (ch >= 0x80 || !std::isxdigit(int(ch))) return result(c, X_ERROR_SUCCESS); // not hex: not parsed
        digits += char(ch);
    }
    auto field = [&](size_t at) { return uint32_t(std::stoul(digits.substr(at, 8), nullptr, 16)); };
    if (title_ptr) k.store32(title_ptr, field(0));
    if (big_ptr) k.store32(big_ptr, field(8));
    if (small_ptr) k.store32(small_ptr, field(16));
    result(c, X_ERROR_SUCCESS);
}

} // namespace xf::kernel
