/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's network as the games see it at start, offline: Winsock started,
// XNet started with no Ethernet link, the title's address the loopback.
// Translated from Xenia (BSD; see LICENSE-xenia): xam_net.cc's answers for
// these exports; checked against it. The sockets themselves (Xenia's host
// sockets) are not here: no game has reached them yet, and they stay
// counted as unimplemented until one does.

#include "xam.h"

#include <cstring>
#include <random>

namespace xf::kernel {
namespace {

constexpr uint32_t X_WSAEINVAL = 0x2726, X_WSAEMSGSIZE = 0x2738;
constexpr uint32_t XNET_GET_XNADDR_STATIC = 0x4;
constexpr uint32_t XNET_STARTUP_PARAMS_SIZE = 0xD;

struct NetState {
    std::mutex mutex;
    uint8_t startup_params[XNET_STARTUP_PARAMS_SIZE] = {};
};

} // namespace

// (caller, version, X_WSADATA *): Winsock 2.2
XF_EXPORT(xam, NetDll_WSAStartup) {
    uint32_t version = arg(k, c, 1) & 0xFFFF, data = arg(k, c, 2);
    if (data) {
        std::memset(k.host(data), 0, 0x190);
        k.store16(data + 0, uint16_t(version));
        k.store16(data + 2, 0x0202);
    }
    result(c, 0);
}

XF_EXPORT(xam, NetDll_WSACleanup) { result(c, 0); }
XF_EXPORT(xam, NetDll_WSAGetLastError) { result(c, last_error(k)); }
XF_EXPORT(xam, NetDll_WSASetLastError) { set_last_error(k, arg(k, c, 0)); }

// (caller, XNetStartupParams *): kept for XNetGetOpt
XF_EXPORT(xam, NetDll_XNetStartup) {
    if (uint32_t params = arg(k, c, 1)) {
        NetState &s = k.state<NetState>();
        std::lock_guard<std::mutex> l(s.mutex);
        std::memcpy(s.startup_params, k.host(params), XNET_STARTUP_PARAMS_SIZE);
    }
    result(c, 0);
}

XF_EXPORT(xam, NetDll_XNetCleanup) { result(c, 0); }

// (one, option, buffer, size_ptr): option 1, the startup parameters
XF_EXPORT(xam, NetDll_XNetGetOpt) {
    uint32_t option = arg(k, c, 1), buffer = arg(k, c, 2), size_ptr = arg(k, c, 3);
    if (option != 1) return result(c, X_WSAEINVAL);
    if (k.load32(size_ptr) < XNET_STARTUP_PARAMS_SIZE) {
        k.store32(size_ptr, XNET_STARTUP_PARAMS_SIZE);
        return result(c, X_WSAEMSGSIZE);
    }
    NetState &s = k.state<NetState>();
    std::lock_guard<std::mutex> l(s.mutex);
    std::memcpy(k.host(buffer), s.startup_params, XNET_STARTUP_PARAMS_SIZE);
    result(c, 0);
}

// (caller, buffer, length): random bytes
XF_EXPORT(xam, NetDll_XNetRandom) {
    uint32_t buffer = arg(k, c, 1), length = arg(k, c, 2);
    if (buffer && length) {
        std::random_device device;
        std::mt19937_64 gen(device());
        for (uint32_t i = 0; i < length; i++) k.host(buffer + i)[0] = uint8_t(gen());
    }
    result(c, X_ERROR_SUCCESS);
}

// (caller, XNADDR *): the loopback; an Ethernet address of 0xCC (RakNet
// makes its random numbers from it, and zero ones fail)
XF_EXPORT(xam, NetDll_XNetGetTitleXnAddr) {
    uint32_t addr = arg(k, c, 1);
    uint8_t *p = k.host(addr);
    p[0] = 127, p[1] = 0, p[2] = 0, p[3] = 1; // ina
    std::memset(p + 4, 0, 6);                 // inaOnline, wPortOnline
    std::memset(p + 10, 0xCC, 6);             // abEnet
    std::memset(p + 16, 0, 20);               // abOnline
    result(c, XNET_GET_XNADDR_STATIC);
}

// no cable
XF_EXPORT(xam, NetDll_XNetGetEthernetLinkStatus) { result(c, 0); }

XF_EXPORT(xam, NetDll_XNetQosListen) { result(c, X_ERROR_FUNCTION_FAILED); }
XF_EXPORT(xam, NetDll_XNetSetSystemLinkPort) { result(c, 1); }
XF_EXPORT(xam, NetDll_XNetRegisterKey) { result(c, 0); }
XF_EXPORT(xam, NetDll_XNetUnregisterKey) { result(c, 0); }

// (caller, key_id, exchange_key): 0xBE bytes
XF_EXPORT(xam, NetDll_XNetCreateKey) {
    std::memset(k.host(arg(k, c, 1)), 0xBE, 8);
    std::memset(k.host(arg(k, c, 2)), 0xBE, 16);
    result(c, 0);
}

// (address): the old inet_addr, an empty string 0 (as the console)
XF_EXPORT(xam, NetDll_inet_addr) {
    uint32_t p = arg(k, c, 0);
    if (!p) return result(c, 0xFFFFFFFF);
    std::string s(reinterpret_cast<const char *>(k.host(p)));
    if (s.empty()) return result(c, 0);
    uint32_t parts[4], n = 0;
    size_t at = 0;
    while (n < 4) {
        size_t end = s.find('.', at);
        std::string part = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos)
            return result(c, 0xFFFFFFFF);
        parts[n++] = uint32_t(std::stoul(part));
        if (parts[n - 1] > 255) return result(c, 0xFFFFFFFF);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (n != 4) return result(c, 0xFFFFFFFF);
    // network order in guest memory, so big-endian as the guest reads it
    result(c, parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
}

} // namespace xf::kernel
