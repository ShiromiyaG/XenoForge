// XAM's calls that need something a console running a game has not been given
// here: a camera on a tilt motor, the biometric sensor, a cache partition,
// the Live file upload service, the guide's custom actions, a microphone array.
// Xenia has none of them; each answers as the console's own XAM does with the
// thing absent: its status, what it writes, what it leaves alone. The answers
// were checked against the console's XAM (xfxam; the calls are in
// tests/kernel_absent_test.cpp), the arguments' meaning read from its code: a pointer the console would fault on is not read here, a plain
// invalid-parameter answer being the gentler of the two.

#include "xam.h"

namespace xf::kernel {

namespace {

constexpr uint32_t E_ACCESSDENIED = 0x80070005, E_HANDLE = 0x80070006;
constexpr uint32_t E_DEVICE_NOT_CONNECTED = 0x8007048F, E_NO_SUCH_USER = 0x80070525;
constexpr uint32_t E_XLFS_NOT_INITIALIZED = 0x80150005;
constexpr uint32_t ERROR_NO_DATA = 0xE8;

} // namespace

// (mask, wake): remembers the users the array leaves idle; no array to tell
XF_EXPORT(xam, XamVoiceSetMicArrayIdleUsers) { result(c, X_ERROR_SUCCESS); }

// ---- the guide's custom actions (the console keeps one set per title) ----

// (user_index *, xuid *): no gamercard is selected: the xuid is zeroed, the
// answer FALSE. The user index is only written for a selected one.
XF_EXPORT(xam, XCustomGetCurrentGamercard) {
    uint32_t user = arg(k, c, 0), xuid = arg(k, c, 1);
    if (user && xuid) k.store64(xuid, 0);
    result(c, 0);
}

// (user_index *, action *, xuid *, text, text_length *): nothing was pressed,
// so the outputs are zeroed (the length only with a text to hold it) and the
// answer is ERROR_NO_DATA. A length past what is held (nothing) is the
// caller's buffer being big enough.
XF_EXPORT(xam, XCustomGetLastActionPressEx) {
    uint32_t user = arg(k, c, 0), action = arg(k, c, 1), xuid = arg(k, c, 2), text = arg(k, c, 3),
             length = arg(k, c, 4);
    if (user) k.store32(user, 0);
    if (action) k.store32(action, 0);
    if (xuid) k.store64(xuid, 0);
    if (length && text) k.store16(length, 0);
    result(c, ERROR_NO_DATA);
}

// (user_index, xuid, actions, count): more than three actions is invalid; the
// user "none", a zero xuid, no array or no actions clear what was set (and
// succeed); a set is refused with ERROR_ACCESS_DENIED while the title has no
// custom-actions session, which nothing here opens
XF_EXPORT(xam, XCustomSetDynamicActions) {
    uint32_t user = arg(k, c, 0), actions = arg(k, c, 2), count = arg(k, c, 3) & 0xFFFF;
    uint64_t xuid = arg64(k, c, 1);
    if (count > 3) return result(c, X_ERROR_INVALID_PARAMETER);
    if (user == USER_INDEX_NONE || !xuid || !actions || !count) return result(c, X_ERROR_SUCCESS);
    result(c, X_ERROR_ACCESS_DENIED);
}

// (): clears the set; the console's function returns nothing
XF_EXPORT(xam, XCustomUnregisterDynamicActions) { result(c, 0); }

// ---- the cache partitions: none is mounted ----

// (XCACHE_FILE *, ...): a null structure or a cache the console has not is an
// invalid parameter; the title's cache 2 is a partition it has, which holds no
// such file (ERROR_FILE_NOT_FOUND), the structure's byte at +2 marked 0x40
XF_EXPORT(xam, XamCacheOpenFile) {
    uint32_t file = arg(k, c, 0);
    if (!file) return result(c, X_ERROR_INVALID_PARAMETER);
    if (k.load32(file + 4) != 2) return result(c, X_ERROR_INVALID_PARAMETER);
    k.host(file)[2] = 0x40;
    result(c, X_ERROR_FILE_NOT_FOUND);
}

// (XCACHE_FILE *, ...): -1, nothing was open
XF_EXPORT(xam, XamCacheCloseFile) { result(c, 0xFFFFFFFF); }

// (cache_id): FALSE for -1, TRUE for a cache id (there is nothing in it to clear)
XF_EXPORT(xam, XamCacheReset) { result(c, arg(k, c, 0) == 0xFFFFFFFF ? 0 : 1); }

// ---- content, biometrics and the age group ----

// (...): the console refuses what it is given when no package can be installed
XF_EXPORT(xam, XamContentInstall) { result(c, X_ERROR_INVALID_PARAMETER); }

// (...): no sensor; the console's check of the arguments comes first and refuses them
XF_EXPORT(xam, XamReadBiometricData) { result(c, X_ERROR_INVALID_PARAMETER); }
XF_EXPORT(xam, XamWriteBiometricData) { result(c, X_ERROR_INVALID_PARAMETER); }

// (user_index, DWORD *): a null pointer is invalid; with no account's data the
// console's answer is ERROR_FUNCTION_FAILED (whatever the user, signed in or not)
XF_EXPORT(xam, XamUserGetAgeGroup) {
    if (!arg(k, c, 1)) return result(c, X_ERROR_INVALID_PARAMETER);
    result(c, X_ERROR_FUNCTION_FAILED);
}

// (user_index, DWORD *): the Kinect enrollment of a user of the four; none is
// enrolled (-2 written), a user past them or a null pointer is invalid
XF_EXPORT(xam, XamUserNuiGetEnrollmentIndex) {
    uint32_t user = arg(k, c, 0), out = arg(k, c, 1);
    if (!out || user >= 4) return result(c, X_E_INVALIDARG);
    k.store32(out, 0xFFFFFFFE);
    result(c, E_NO_SUCH_USER);
}

// ---- the Kinect's tilt motor: no camera ----

XF_EXPORT(xam, XamNuiCameraElevationSetAngle) { result(c, E_DEVICE_NOT_CONNECTED); }
XF_EXPORT(xam, XamNuiCameraElevationStopMovement) { result(c, E_DEVICE_NOT_CONNECTED); }
XF_EXPORT(xam, XamNuiCameraRememberFloor) { result(c, E_DEVICE_NOT_CONNECTED); }
// (status): hands back what it is given
XF_EXPORT(xam, XamNuiCameraTiltReportStatus) { result(c, arg(k, c, 0)); }

// ---- the Live file upload queue: the service is not there ----

XF_EXPORT(xam, XamXlfsInitializeUploadQueue) { result(c, E_ACCESSDENIED); }
XF_EXPORT(xam, XamXlfsUninitializeUploadQueue) { result(c, E_XLFS_NOT_INITIALIZED); }
XF_EXPORT(xam, XamXlfsMountUploadQueueInstance) { result(c, E_HANDLE); }
XF_EXPORT(xam, XamXlfsUnmountUploadQueueInstance) { result(c, E_HANDLE); }

// (mode): the background downloads' mode; nothing to schedule
XF_EXPORT(xam, XamBackgroundDownloadSetMode) { result(c, X_ERROR_SUCCESS); }

} // namespace xf::kernel
