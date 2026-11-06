// XAM's network offline (src/kernel/xam_net.cpp), driven through its
// exports as a game calls them, on guest threads: Winsock and XNet started,
// the title's address and the link; datagram sockets bound, given options,
// read from without data (non-blocking, with a receive timeout), sent to on
// the loopback and broadcast, peeked and truncated, selected, read by a
// thread blocked until another sends, woken by closesocket; stream sockets
// that listen and never connect; the WSA events; DNS and QoS lookups that
// fail at once; XNetLogonGetTitleID; XCustomRegisterDynamicActions; the
// disc's media verification, its callback a user APC; the overlapped
// Winsock calls (receives pending until a datagram, completed at once,
// truncated, cancelled, aborted by closesocket, ended through an event, a
// completion routine and WSAGetOverlappedResult waiting on another thread;
// gathered sends; WSAEventSelect); the Ex startups, the debug address,
// XNetInAddrToString, XAuth and XHttp offline.

#include "check.h"

#include "objects.h"
#include "xam.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <atomic>
#include <cstring>
#include <string>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;

constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;
constexpr uint32_t WSAEINTR = 10004, WSAEACCES = 10013, WSAEINVAL = 10022, WSAEWOULDBLOCK = 10035;
constexpr uint32_t WSAENOTSOCK = 10038, WSAEMSGSIZE = 10040, WSAEOPNOTSUPP = 10045, WSAEADDRINUSE = 10048;
constexpr uint32_t WSAENETUNREACH = 10051, WSAENOTCONN = 10057, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061;
constexpr uint32_t FIONBIO = 0x8004667E, FIONREAD = 0x4004667F, MSG_PEEK = 2;
constexpr uint32_t SOL_SOCKET = 0xFFFF, SO_REUSEADDR = 4, SO_BROADCAST = 0x20, SO_RCVBUF = 0x1002;
constexpr uint32_t SO_RCVTIMEO = 0x1006, SO_TYPE = 0x1008;
constexpr uint32_t LOOPBACK = 0x7F000001;

enum : uint32_t {
    F_MAIN = 0x82000000,
    F_RECEIVER = 0x82000010,
    F_CLOSED_WAITER = 0x82000020,
    F_SELECTER = 0x82000030,
    F_MEDIA_CALLBACK = 0x82000040,
    F_WSA_ROUTINE = 0x82000050,
    F_OVERLAPPED_WAITER = 0x82000060,
};

std::atomic<int> media_callbacks{0};
void media_callback(ppc::Context &c) {
    media_callbacks++;
    c.r[3] = 0;
}

// an export, as the code calls it: r3-r10, then the stack (a 32-bit value
// in the word at r1 + 0x54 + 8 * (n - 8))
uint32_t call(ppc::Context &c, const char *library, const char *name, std::initializer_list<uint64_t> args) {
    ExportFn fn = find_export_fn(library, name);
    if (!fn) {
        std::printf("no export %s!%s\n", library, name);
        failures++;
        return 0xFFFFFFFF;
    }
    int n = 0;
    for (uint64_t a : args) {
        if (n < 8)
            c.r[3 + n] = a;
        else
            K->store32(uint32_t(c.r[1]) + 0x54 + 8 * (n - 8), uint32_t(a));
        n++;
    }
    fn(*K, c);
    return uint32_t(c.r[3]);
}
uint32_t krnl(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call(c, "xboxkrnl", name, args);
}
uint32_t xam(ppc::Context &c, const char *name, std::initializer_list<uint64_t> args) {
    return call(c, "xam", name, args);
}
uint32_t last_error(ppc::Context &c) { return xam(c, "NetDll_WSAGetLastError", {}); }

// a WSA completion routine: what it was called with
std::atomic<int> routine_calls{0};
std::atomic<uint32_t> routine_error{0}, routine_bytes{0}, routine_overlapped{0};
void wsa_routine(ppc::Context &c) {
    routine_error = uint32_t(c.r[3]), routine_bytes = uint32_t(c.r[4]), routine_overlapped = uint32_t(c.r[5]);
    routine_calls++;
}

// per-thread guest scratch below the stack pointer
uint32_t local(ppc::Context &c, uint32_t offset) { return uint32_t(c.r[1]) - 0x1000 + offset; }

void sleep_ms(ppc::Context &c, int64_t ms) {
    uint32_t p = local(c, 0xF00);
    K->store64(p, uint64_t(-ms * 10'000));
    krnl(c, "KeDelayExecutionThread", {1, 0, p});
}

uint32_t create_thread(ppc::Context &c, uint32_t start, uint32_t context) {
    uint32_t handle_ptr = local(c, 0xF10), id_ptr = local(c, 0xF14);
    CHECK(krnl(c, "ExCreateThread", {handle_ptr, 0, id_ptr, 0, start, context, 0}) == X_STATUS_SUCCESS);
    return K->load32(handle_ptr);
}

void join(ppc::Context &c, uint32_t handle) {
    CHECK(krnl(c, "NtWaitForSingleObjectEx", {handle, 0, 0, 0}) == X_STATUS_SUCCESS);
    CHECK(krnl(c, "NtClose", {handle}) == X_STATUS_SUCCESS);
}

// until the thread is blocked in a wait (the socket's)
void until_waiting(ppc::Context &c, uint32_t handle) {
    Objects &o = objects(*K);
    Ref<Thread> t = o.lookup<Thread>(handle);
    CHECK(t);
    for (int i = 0; i < 5000 && t; i++) {
        {
            std::lock_guard<std::mutex> l(o.lock);
            if (t->blocked == Thread::Blocked::Wait) return;
        }
        sleep_ms(c, 1);
    }
    CHECK(!"the thread never waited");
}

uint32_t udp(ppc::Context &c) { return xam(c, "NetDll_socket", {1, 2, 2, 17}); }

// XSOCKADDR_IN
uint32_t sockaddr(ppc::Context &c, uint32_t offset, uint32_t address, uint16_t port) {
    uint32_t p = local(c, offset);
    std::memset(K->host(p), 0, 16);
    K->store16(p, 2);
    K->store16(p + 2, port);
    K->store32(p + 4, address);
    return p;
}

uint32_t bind(ppc::Context &c, uint32_t s, uint16_t port) {
    return xam(c, "NetDll_bind", {1, s, sockaddr(c, 0x100, 0, port), 16});
}

uint32_t set_int_option(ppc::Context &c, uint32_t s, uint32_t level, uint32_t name, uint32_t value) {
    uint32_t p = local(c, 0x120);
    K->store32(p, value);
    return xam(c, "NetDll_setsockopt", {1, s, level, name, p, 4});
}

uint32_t send_to(ppc::Context &c, uint32_t s, uint32_t address, uint16_t port, const char *text) {
    uint32_t buffer = local(c, 0x140);
    uint32_t n = uint32_t(std::strlen(text));
    std::memcpy(K->host(buffer), text, n);
    return xam(c, "NetDll_sendto", {1, s, buffer, n, 0, sockaddr(c, 0x130, address, port), 16});
}

// recvfrom into a scratch buffer: the length, and the text and sender
struct Received {
    uint32_t n;
    std::string text;
    uint32_t address;
    uint16_t port;
};
Received receive(ppc::Context &c, uint32_t s, uint32_t length = 64, uint32_t flags = 0) {
    uint32_t buffer = local(c, 0x200), from = local(c, 0x1E0), from_length = local(c, 0x1F0);
    std::memset(K->host(buffer), 0, 64);
    std::memset(K->host(from), 0xEE, 16);
    K->store32(from_length, 16);
    Received r;
    r.n = xam(c, "NetDll_recvfrom", {1, s, buffer, length, flags, from, from_length});
    r.text = reinterpret_cast<const char *>(K->host(buffer));
    r.address = K->load32(from + 4);
    r.port = K->load16(from + 2);
    if (r.n != SOCKET_ERROR) CHECK(K->load16(from) == 2 && K->load32(from_length) == 16);
    return r;
}

uint16_t port_of(ppc::Context &c, uint32_t s) {
    uint32_t name = local(c, 0x160), length = local(c, 0x170);
    K->store32(length, 16);
    CHECK(xam(c, "NetDll_getsockname", {1, s, name, length}) == 0);
    CHECK(K->load16(name) == 2 && K->load32(length) == 16);
    return K->load16(name + 2);
}

// x_fd_set with one socket
uint32_t fd_set_of(ppc::Context &c, uint32_t offset, uint32_t s) {
    uint32_t p = local(c, offset);
    K->store32(p, 1);
    K->store32(p + 4, s);
    return p;
}

void startup(ppc::Context &c) {
    uint32_t data = local(c, 0x400);
    CHECK(xam(c, "NetDll_WSAStartup", {1, 0x0202, data}) == 0);
    CHECK(K->load16(data) == 0x0202 && K->load16(data + 2) == 0x0202);
    // XNetStartupParams: 32 KB receive buffers by default
    uint32_t params = local(c, 0x600);
    std::memset(K->host(params), 0, 13);
    K->host(params)[0] = 13;
    K->host(params)[4] = 32;
    CHECK(xam(c, "NetDll_XNetStartup", {1, params}) == 0);
    uint32_t got = local(c, 0x620), size = local(c, 0x630);
    K->store32(size, 4);
    CHECK(xam(c, "NetDll_XNetGetOpt", {1, 1, got, size}) == 10040 && K->load32(size) == 13);
    CHECK(xam(c, "NetDll_XNetGetOpt", {1, 1, got, size}) == 0 && K->host(got)[4] == 32);
    uint32_t xnaddr = local(c, 0x640);
    CHECK(xam(c, "NetDll_XNetGetTitleXnAddr", {1, xnaddr}) == 4 && K->load32(xnaddr) == LOOPBACK &&
          K->host(xnaddr + 10)[0] == 0xCC);
    CHECK(xam(c, "NetDll_XNetGetEthernetLinkStatus", {1}) == 0);
    uint32_t machine = local(c, 0x680);
    CHECK(xam(c, "XNetLogonGetMachineID", {machine}) == 0x80151802 && K->load64(machine) == 0);
    CHECK(xam(c, "NetDll_XNetXnAddrToInAddr", {1, xnaddr, 0, 0}) == 1);
    uint32_t ip = local(c, 0x6A0);
    std::memcpy(K->host(ip), "10.1.2.3", 9);
    CHECK(xam(c, "NetDll_inet_addr", {ip}) == 0x0A010203);
}

void datagrams(ppc::Context &c) {
    // a bound socket with nothing to read
    uint32_t a = udp(c);
    CHECK(a != SOCKET_ERROR && a != 0);
    CHECK(bind(c, a, 1000) == 0 && port_of(c, a) == 1000);
    CHECK(bind(c, a, 1001) == SOCKET_ERROR && last_error(c) == WSAEINVAL); // bound already
    uint32_t value = local(c, 0x180), length = local(c, 0x184);
    K->store32(length, 4);
    CHECK(xam(c, "NetDll_getsockopt", {1, a, SOL_SOCKET, SO_TYPE, value, length}) == 0 && K->load32(value) == 2);
    CHECK(xam(c, "NetDll_getsockopt", {1, a, SOL_SOCKET, SO_RCVBUF, value, length}) == 0 &&
          K->load32(value) == 32 * 1024 && K->load32(length) == 4);
    uint32_t on = local(c, 0x190);
    K->store32(on, 1);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, a, FIONBIO, on}) == 0);
    CHECK(receive(c, a).n == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, a, FIONREAD, on}) == 0 && K->load32(on) == 0);

    // a second, unbound one, sends to the first through the loopback, from
    // an ephemeral port
    uint32_t b = udp(c);
    CHECK(send_to(c, b, LOOPBACK, 1000, "hello") == 5);
    uint16_t b_port = port_of(c, b);
    CHECK(b_port >= 49152);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, a, FIONREAD, on}) == 0 && K->load32(on) == 5);
    uint32_t reads = fd_set_of(c, 0x1A0, a), timeval = local(c, 0x1B0);
    K->store32(timeval, 0), K->store32(timeval + 4, 0);
    CHECK(xam(c, "NetDll_select", {1, 0, reads, 0, 0, timeval}) == 1 && K->load32(reads) == 1 &&
          K->load32(reads + 4) == a);
    CHECK(xam(c, "NetDll___WSAFDIsSet", {a, reads}) == 1 && xam(c, "NetDll___WSAFDIsSet", {b, reads}) == 0);
    Received r = receive(c, a, 64, MSG_PEEK);
    CHECK(r.n == 5 && r.text == "hello" && r.address == LOOPBACK && r.port == b_port);
    r = receive(c, a);
    CHECK(r.n == 5 && r.text == "hello");
    CHECK(receive(c, a).n == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);
    // nothing readable: select empties the set
    CHECK(xam(c, "NetDll_select", {1, 0, reads, 0, 0, timeval}) == 0 && K->load32(reads) == 0);
    // a datagram socket is always writable
    uint32_t writes = fd_set_of(c, 0x1C0, b);
    CHECK(xam(c, "NetDll_select", {1, 0, 0, writes, 0, timeval}) == 1 && K->load32(writes) == 1);

    // anywhere else: sent, and lost
    CHECK(send_to(c, b, 0x0A000001, 1000, "away") == 4);
    CHECK(receive(c, a).n == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);

    // broadcast only with SO_BROADCAST
    CHECK(send_to(c, b, 0xFFFFFFFF, 1000, "all") == SOCKET_ERROR && last_error(c) == WSAEACCES);
    CHECK(set_int_option(c, b, SOL_SOCKET, SO_BROADCAST, 1) == 0);
    CHECK(send_to(c, b, 0xFFFFFFFF, 1000, "all") == 3);
    // too long for the buffer: the start, then the rest lost
    r = receive(c, a, 2);
    CHECK(r.n == SOCKET_ERROR && last_error(c) == WSAEMSGSIZE && r.text == "al");
    CHECK(receive(c, a).n == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);

    // the port is taken, unless reused
    uint32_t d = udp(c);
    CHECK(bind(c, d, 1000) == SOCKET_ERROR && last_error(c) == WSAEADDRINUSE);
    CHECK(set_int_option(c, d, SOL_SOCKET, SO_REUSEADDR, 1) == 0 && bind(c, d, 1000) == 0);
    // both bound there get a datagram sent there
    CHECK(send_to(c, b, LOOPBACK, 1000, "two") == 3);
    CHECK(receive(c, a).text == "two");
    K->store32(on, 1);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, d, FIONBIO, on}) == 0 && receive(c, d).text == "two");

    // connected: send and recv reach the peer
    CHECK(xam(c, "NetDll_connect", {1, b, sockaddr(c, 0x100, LOOPBACK, 1000), 16}) == 0);
    uint32_t buffer = local(c, 0x140);
    std::memcpy(K->host(buffer), "peer", 4);
    CHECK(xam(c, "NetDll_send", {1, b, buffer, 4, 0}) == 4 && receive(c, a).text == "peer");
    CHECK(receive(c, d).text == "peer");
    CHECK(xam(c, "NetDll_shutdown", {1, b, 2}) == 0);
    CHECK(xam(c, "NetDll_recv", {1, a, buffer, 4, 0}) == SOCKET_ERROR && last_error(c) == WSAENOTCONN);

    // a blocking read with a receive timeout
    CHECK(set_int_option(c, d, SOL_SOCKET, SO_RCVTIMEO, 20) == 0);
    K->store32(on, 0);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, d, FIONBIO, on}) == 0);
    CHECK(receive(c, d).n == SOCKET_ERROR && last_error(c) == WSAETIMEDOUT);
    // select's timeout
    K->store32(timeval, 0), K->store32(timeval + 4, 20000);
    reads = fd_set_of(c, 0x1A0, d);
    CHECK(xam(c, "NetDll_select", {1, 0, reads, 0, 0, timeval}) == 0);

    for (uint32_t s : {a, b, d}) CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);
    CHECK(xam(c, "NetDll_closesocket", {1, a}) == SOCKET_ERROR && last_error(c) == WSAENOTSOCK);
    CHECK(receive(c, a).n == SOCKET_ERROR && last_error(c) == WSAENOTSOCK);
    // the port is free again
    a = udp(c);
    CHECK(bind(c, a, 1000) == 0 && xam(c, "NetDll_closesocket", {1, a}) == 0);
}

// a blocking reader on port 2000: what it got
void receiver(ppc::Context &c) {
    uint32_t s = udp(c);
    CHECK(bind(c, s, 2000) == 0);
    Received r = receive(c, s);
    CHECK(r.n == 4 && r.text == "wake" && r.address == LOOPBACK);
    CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);
    c.r[3] = 0;
}

// blocked on a socket (handed in r3) until it is closed
void closed_waiter(ppc::Context &c) {
    uint32_t s = uint32_t(c.r[3]);
    Received r = receive(c, s);
    CHECK(r.n == SOCKET_ERROR && last_error(c) == WSAEINTR);
    c.r[3] = 0;
}

// select with no timeout on a socket (in r3) until a datagram comes
void selecter(ppc::Context &c) {
    uint32_t s = uint32_t(c.r[3]);
    uint32_t reads = fd_set_of(c, 0x1A0, s);
    CHECK(xam(c, "NetDll_select", {1, 0, reads, 0, 0, 0}) == 1 && K->load32(reads) == 1);
    CHECK(receive(c, s).text == "sel");
    c.r[3] = 0;
}

void blocking(ppc::Context &c) {
    // a thread blocked in recvfrom, woken by a datagram
    uint32_t t = create_thread(c, F_RECEIVER, 0);
    uint32_t sender = udp(c);
    // until its socket is bound and it waits on it
    until_waiting(c, t);
    CHECK(send_to(c, sender, LOOPBACK, 2000, "wake") == 4);
    join(c, t);

    // one blocked in select
    uint32_t s = udp(c);
    CHECK(bind(c, s, 2001) == 0);
    t = create_thread(c, F_SELECTER, s);
    until_waiting(c, t);
    CHECK(send_to(c, sender, LOOPBACK, 2001, "sel") == 3);
    join(c, t);
    CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);

    // one woken by closesocket
    s = udp(c);
    CHECK(bind(c, s, 2002) == 0);
    t = create_thread(c, F_CLOSED_WAITER, s);
    until_waiting(c, t);
    CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);
    join(c, t);
    CHECK(xam(c, "NetDll_closesocket", {1, sender}) == 0);
}

void streams(ppc::Context &c) {
    uint32_t s = xam(c, "NetDll_socket", {1, 2, 1, 6});
    CHECK(s != SOCKET_ERROR);
    CHECK(xam(c, "NetDll_listen", {1, s, 5}) == SOCKET_ERROR && last_error(c) == WSAEINVAL); // unbound
    CHECK(bind(c, s, 3000) == 0 && xam(c, "NetDll_listen", {1, s, 5}) == 0);
    uint32_t on = local(c, 0x190), addr = local(c, 0x1A0), length = local(c, 0x1B0);
    K->store32(on, 1);
    K->store32(length, 16);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, s, FIONBIO, on}) == 0);
    CHECK(xam(c, "NetDll_accept", {1, s, addr, length}) == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);
    uint32_t t = xam(c, "NetDll_socket", {1, 2, 1, 0});
    CHECK(xam(c, "NetDll_connect", {1, t, sockaddr(c, 0x100, LOOPBACK, 3000), 16}) == SOCKET_ERROR &&
          last_error(c) == WSAECONNREFUSED);
    CHECK(xam(c, "NetDll_connect", {1, t, sockaddr(c, 0x100, 0x0A000001, 80), 16}) == SOCKET_ERROR &&
          last_error(c) == WSAENETUNREACH);
    uint32_t buffer = local(c, 0x140);
    CHECK(xam(c, "NetDll_send", {1, t, buffer, 4, 0}) == SOCKET_ERROR && last_error(c) == WSAENOTCONN);
    CHECK(xam(c, "NetDll_recv", {1, t, buffer, 4, 0}) == SOCKET_ERROR && last_error(c) == WSAENOTCONN);
    CHECK(xam(c, "NetDll_shutdown", {1, t, 2}) == SOCKET_ERROR && last_error(c) == WSAENOTCONN);
    uint32_t u = udp(c);
    CHECK(bind(c, u, 3001) == 0 && xam(c, "NetDll_listen", {1, u, 5}) == SOCKET_ERROR &&
          last_error(c) == WSAEOPNOTSUPP);
    for (uint32_t x : {s, t, u}) CHECK(xam(c, "NetDll_closesocket", {1, x}) == 0);
    // not a socket at all
    CHECK(xam(c, "NetDll_socket", {1, 23, 2, 0}) == SOCKET_ERROR && last_error(c) == 10047);
    CHECK(xam(c, "NetDll_socket", {1, 2, 2, 6}) == SOCKET_ERROR && last_error(c) == 10043);
}

void events_and_lookups(ppc::Context &c) {
    uint32_t e = xam(c, "NetDll_WSACreateEvent", {});
    CHECK(e != 0);
    uint32_t list = local(c, 0x300);
    K->store32(list, e);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 10, 0}) == 0x102);
    CHECK(xam(c, "NetDll_WSASetEvent", {e}) == 1);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 0xFFFFFFFF, 0}) == 0);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 1, 0, 0}) == 0); // manual reset: still set
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 0, 0}) == 0x102);

    // DNS and QoS: answered at once, failed / empty, the event set
    uint32_t dns_ptr = local(c, 0x310), host = local(c, 0x320);
    std::memcpy(K->host(host), "example.com", 12);
    CHECK(xam(c, "NetDll_XNetDnsLookup", {1, host, e, dns_ptr}) == 0);
    uint32_t dns = K->load32(dns_ptr);
    CHECK(dns != 0 && K->load32(dns) == 1 && K->load32(dns + 4) == 0);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 0, 0}) == 0);
    CHECK(xam(c, "NetDll_XNetDnsRelease", {1, dns}) == 0);
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);
    uint32_t qos_ptr = local(c, 0x340);
    CHECK(xam(c, "NetDll_XNetQosServiceLookup", {1, 0, e, qos_ptr}) == 0);
    uint32_t qos = K->load32(qos_ptr);
    CHECK(qos != 0 && K->load32(qos) == 0 && K->load32(qos + 4) == 0);
    CHECK(xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 0, 0}) == 0);
    CHECK(xam(c, "NetDll_XNetQosRelease", {1, qos}) == 0);

    CHECK(xam(c, "NetDll_WSACloseEvent", {e}) == 1);
    CHECK(xam(c, "NetDll_WSACloseEvent", {e}) == 0 && last_error(c) == X_ERROR_INVALID_HANDLE);

    // the guide's custom actions: accepted, r3 left as it was
    CHECK(xam(c, "XCustomRegisterDynamicActions", {0x1234}) == 0x1234);
    CHECK(xam(c, "XNetLogonGetTitleID", {9}) == xam(c, "XamGetCurrentTitleId", {}));
}

void media_verification(ppc::Context &c) {
    CHECK(xam(c, "XamMediaVerificationCreate", {0}) == 0);
    uint32_t info = local(c, 0x380);
    std::memset(K->host(info), 0xEE, 0x18);
    K->store32(info, 0x10);
    CHECK(xam(c, "XamMediaVerificationFailedBlocks", {info}) == X_ERROR_NOT_ENOUGH_MEMORY);
    K->store32(info, 0x18);
    CHECK(xam(c, "XamMediaVerificationFailedBlocks", {info}) == 0);
    CHECK(K->load32(info + 4) == 0 && K->load32(info + 8) == 0 && K->load32(info + 0xC) == 0 &&
          K->load32(info + 0x10) == 0 && K->load32(info + 0x14) == 1);
    // the callback runs at the thread's next alertable wait
    CHECK(xam(c, "XamMediaVerificationVerify", {0, 0, F_MEDIA_CALLBACK | 1}) == 0);
    CHECK(media_callbacks == 0);
    uint32_t p = local(c, 0xF00);
    K->store64(p, uint64_t(-1000 * 10'000));
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, p}) == X_STATUS_USER_APC);
    CHECK(media_callbacks == 1);
}

constexpr uint32_t WSA_OPERATION_ABORTED = 995, WSA_IO_INCOMPLETE = 996, WSA_IO_PENDING = 997, WSAEFAULT = 10014;
constexpr uint32_t STATUS_PENDING = 0x103, STATUS_BUFFER_OVERFLOW = 0x80000005, STATUS_CANCELLED = 0xC0000120;
constexpr uint32_t FD_READ = 1, FD_WRITE = 2;

// a WSABUF array: (address, length) each
uint32_t wsabufs(ppc::Context &c, uint32_t offset, std::initializer_list<std::pair<uint32_t, uint32_t>> buffers) {
    uint32_t p = local(c, offset), at = p;
    for (auto [address, length] : buffers) {
        K->store32(at, length);
        K->store32(at + 4, address);
        at += 8;
    }
    return p;
}

// a WSAOVERLAPPED with an event (0 for none)
uint32_t wsa_overlapped(ppc::Context &c, uint32_t offset, uint32_t event) {
    uint32_t p = local(c, offset);
    std::memset(K->host(p), 0xEE, 0x14);
    K->store32(p + 0x10, event);
    return p;
}

bool event_set(ppc::Context &c, uint32_t event) {
    uint32_t list = local(c, 0x300);
    K->store32(list, event);
    return xam(c, "NetDll_WSAWaitForMultipleEvents", {1, list, 0, 0, 0}) == 0;
}

// a text in a scratch buffer, as one WSABUF
uint32_t text_buffer(ppc::Context &c, uint32_t offset, uint32_t bufs_offset, const char *text) {
    uint32_t p = local(c, offset);
    std::memcpy(K->host(p), text, std::strlen(text));
    return wsabufs(c, bufs_offset, {{p, uint32_t(std::strlen(text))}});
}

// WSAGetOverlappedResult with wait on a socket and an overlapped the main
// thread left pending (both in these), until a datagram ends it
uint32_t waiter_socket = 0, waiter_overlapped = 0;
void overlapped_waiter(ppc::Context &c) {
    uint32_t bytes = local(c, 0x10), flags = local(c, 0x14);
    CHECK(xam(c, "NetDll_WSAGetOverlappedResult", {1, waiter_socket, waiter_overlapped, bytes, 1, flags}) == 1);
    CHECK(K->load32(bytes) == 4 && K->load32(flags) == 0);
    c.r[3] = 0;
}

void overlapped_io(ppc::Context &c) {
    uint32_t a = udp(c), b = udp(c);
    CHECK(bind(c, a, 4000) == 0);
    uint32_t e = xam(c, "NetDll_WSACreateEvent", {});
    uint32_t first = local(c, 0x720), second = local(c, 0x740);
    std::memset(K->host(first), 0, 16), std::memset(K->host(second), 0, 16);
    uint32_t bufs = wsabufs(c, 0x700, {{first, 3}, {second, 8}});
    uint32_t bytes = local(c, 0x760), flags = local(c, 0x764), from = local(c, 0x770), from_length = local(c, 0x780);
    uint32_t ov = wsa_overlapped(c, 0x900, e);
    K->store32(flags, 0), K->store32(from_length, 16), K->store32(bytes, 0xEEEEEEEE);

    // nothing to read: pending, whether or not the socket blocks
    CHECK(xam(c, "NetDll_WSARecvFrom", {1, a, bufs, 2, bytes, flags, from, from_length, ov, 0}) == SOCKET_ERROR &&
          last_error(c) == WSA_IO_PENDING);
    CHECK(K->load32(ov) == STATUS_PENDING && K->load32(bytes) == 0xEEEEEEEE && !event_set(c, e));
    CHECK(xam(c, "NetDll_WSAGetOverlappedResult", {1, a, ov, bytes, 0, flags}) == 0 &&
          last_error(c) == WSA_IO_INCOMPLETE);
    // a send gathered from two buffers ends it: scattered over the two
    uint32_t hello = local(c, 0x7C0), there = local(c, 0x7D0);
    std::memcpy(K->host(hello), "hel", 3), std::memcpy(K->host(there), "lo there", 8);
    uint32_t sbufs = wsabufs(c, 0x7A0, {{hello, 3}, {there, 8}}), sent = local(c, 0x7E0);
    CHECK(xam(c, "NetDll_WSASendTo", {1, b, sbufs, 2, sent, 0, sockaddr(c, 0x130, LOOPBACK, 4000), 16, 0, 0}) == 0 &&
          K->load32(sent) == 11);
    CHECK(event_set(c, e) && K->load32(ov) == 0 && K->load32(ov + 4) == 11);
    CHECK(std::string(reinterpret_cast<const char *>(K->host(first))) == "hel" &&
          std::string(reinterpret_cast<const char *>(K->host(second))) == "lo there");
    uint16_t b_port = port_of(c, b);
    CHECK(K->load32(from + 4) == LOOPBACK && K->load16(from + 2) == b_port && K->load32(from_length) == 16);
    CHECK(xam(c, "NetDll_WSAGetOverlappedResult", {1, a, ov, bytes, 0, flags}) == 1 && K->load32(bytes) == 11);
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);

    // WSARecv reads a connected socket only
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 2, bytes, flags, 0, 0}) == SOCKET_ERROR &&
          last_error(c) == WSAENOTCONN);
    CHECK(xam(c, "NetDll_connect", {1, a, sockaddr(c, 0x100, LOOPBACK, b_port), 16}) == 0);
    // a datagram already there: completed at once, the event set too
    CHECK(xam(c, "NetDll_WSASendTo",
              {1, b, text_buffer(c, 0x7C0, 0x7A0, "now"), 1, sent, 0, sockaddr(c, 0x130, LOOPBACK, 4000), 16, 0, 0}) ==
          0);
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 2, bytes, flags, ov, 0}) == 0 && K->load32(bytes) == 3);
    CHECK(K->load32(ov) == 0 && K->load32(ov + 4) == 3 && event_set(c, e));
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);
    // and with no overlapped, as recv: non-blocking, nothing there
    uint32_t on = local(c, 0x190);
    K->store32(on, 1);
    CHECK(xam(c, "NetDll_ioctlsocket", {1, a, FIONBIO, on}) == 0);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 2, bytes, flags, 0, 0}) == SOCKET_ERROR &&
          last_error(c) == WSAEWOULDBLOCK);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 0, bytes, flags, 0, 0}) == SOCKET_ERROR && last_error(c) == WSAEFAULT);

    // too long for the buffers: the start, STATUS_BUFFER_OVERFLOW, WSAEMSGSIZE
    uint32_t small = wsabufs(c, 0x7F0, {{first, 2}});
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, small, 1, bytes, flags, ov, 0}) == SOCKET_ERROR &&
          last_error(c) == WSA_IO_PENDING);
    CHECK(xam(c, "NetDll_WSASendTo",
              {1, b, text_buffer(c, 0x7C0, 0x7A0, "long"), 1, sent, 0, sockaddr(c, 0x130, LOOPBACK, 4000), 16, 0, 0}) ==
          0);
    CHECK(K->load32(ov) == STATUS_BUFFER_OVERFLOW && event_set(c, e));
    CHECK(xam(c, "NetDll_WSAGetOverlappedResult", {1, a, ov, bytes, 0, flags}) == 0 && last_error(c) == WSAEMSGSIZE &&
          K->load32(bytes) == 2);
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);

    // cancelled
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 2, bytes, flags, ov, 0}) == SOCKET_ERROR);
    CHECK(xam(c, "NetDll_WSACancelOverlappedIO", {1, a}) == 0);
    CHECK(K->load32(ov) == STATUS_CANCELLED && event_set(c, e));
    CHECK(xam(c, "NetDll_WSAGetOverlappedResult", {1, a, ov, bytes, 1, flags}) == 0 &&
          last_error(c) == WSA_OPERATION_ABORTED);
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);

    // a completion routine instead of the event: queued to this thread, run
    // at its next alertable wait, the event left alone
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSARecv", {1, a, bufs, 2, bytes, flags, ov, F_WSA_ROUTINE}) == SOCKET_ERROR &&
          last_error(c) == WSA_IO_PENDING);
    CHECK(xam(c, "NetDll_WSASendTo",
              {1, b, text_buffer(c, 0x7C0, 0x7A0, "apc"), 1, sent, 0, sockaddr(c, 0x130, LOOPBACK, 4000), 16, 0, 0}) ==
          0);
    CHECK(K->load32(ov) == 0 && !event_set(c, e) && routine_calls == 0);
    uint32_t p = local(c, 0xF00);
    K->store64(p, uint64_t(-1000 * 10'000));
    CHECK(krnl(c, "KeDelayExecutionThread", {1, 1, p}) == X_STATUS_USER_APC);
    CHECK(routine_calls == 1 && routine_error == 0 && routine_bytes == 3 && routine_overlapped == ov);

    // WSASend to the connected peer, overlapped: completed at once
    CHECK(xam(c, "NetDll_connect", {1, b, sockaddr(c, 0x100, LOOPBACK, 4000), 16}) == 0);
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSASend", {1, b, text_buffer(c, 0x7C0, 0x7A0, "peer"), 1, sent, 0, ov, 0}) == 0 &&
          K->load32(sent) == 4 && K->load32(ov) == 0 && K->load32(ov + 4) == 4 && event_set(c, e));
    CHECK(xam(c, "NetDll_recv", {1, a, first, 16, 0}) == 4);
    uint32_t unconnected = udp(c);
    CHECK(xam(c, "NetDll_WSASend", {1, unconnected, text_buffer(c, 0x7C0, 0x7A0, "x"), 1, sent, 0, 0, 0}) ==
              SOCKET_ERROR &&
          last_error(c) == WSAENOTCONN);

    // another thread waits in WSAGetOverlappedResult until a datagram comes
    uint32_t w = udp(c);
    CHECK(bind(c, w, 4001) == 0);
    waiter_socket = w, waiter_overlapped = wsa_overlapped(c, 0x7F8, 0);
    CHECK(xam(c, "NetDll_WSARecv", {1, 0x1234, bufs, 2, bytes, flags, 0, 0}) == SOCKET_ERROR &&
          last_error(c) == WSAENOTSOCK);
    CHECK(xam(c, "NetDll_WSARecvFrom", {1, w, bufs, 2, bytes, flags, 0, 0, waiter_overlapped, 0}) == SOCKET_ERROR &&
          last_error(c) == WSA_IO_PENDING);
    uint32_t t = create_thread(c, F_OVERLAPPED_WAITER, 0);
    until_waiting(c, t);
    CHECK(xam(c, "NetDll_WSASendTo",
              {1, b, text_buffer(c, 0x7C0, 0x7A0, "wake"), 1, sent, 0, sockaddr(c, 0x130, LOOPBACK, 4001), 16, 0, 0}) ==
          0);
    join(c, t);

    // closesocket aborts what pends on it
    ov = wsa_overlapped(c, 0x900, e);
    CHECK(xam(c, "NetDll_WSARecvFrom", {1, w, bufs, 2, bytes, flags, 0, 0, ov, 0}) == SOCKET_ERROR);
    CHECK(xam(c, "NetDll_closesocket", {1, w}) == 0);
    CHECK(K->load32(ov) == STATUS_CANCELLED && event_set(c, e));

    for (uint32_t s : {a, b, unconnected}) CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);
    CHECK(xam(c, "NetDll_WSACloseEvent", {e}) == 1);
}

void event_select(ppc::Context &c) {
    uint32_t u = udp(c), sender = udp(c);
    CHECK(bind(c, u, 4002) == 0);
    uint32_t e = xam(c, "NetDll_WSACreateEvent", {});
    // a datagram socket is writable at once
    CHECK(xam(c, "NetDll_WSAEventSelect", {1, u, e, FD_READ | FD_WRITE}) == 0 && event_set(c, e));
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);
    CHECK(xam(c, "NetDll_WSAEventSelect", {1, u, e, FD_READ}) == 0 && !event_set(c, e));
    CHECK(send_to(c, sender, LOOPBACK, 4002, "read") == 4 && event_set(c, e));
    // and non-blocking now
    CHECK(receive(c, u).text == "read");
    CHECK(receive(c, u).n == SOCKET_ERROR && last_error(c) == WSAEWOULDBLOCK);
    // undone with no events
    CHECK(xam(c, "NetDll_WSAResetEvent", {e}) == 1);
    CHECK(xam(c, "NetDll_WSAEventSelect", {1, u, 0, 0}) == 0);
    CHECK(send_to(c, sender, LOOPBACK, 4002, "more") == 4 && !event_set(c, e));
    CHECK(xam(c, "NetDll_WSAEventSelect", {1, u, 0x1234, FD_READ}) == SOCKET_ERROR && last_error(c) == WSAEINVAL);
    CHECK(xam(c, "NetDll_WSAEventSelect", {1, 0x1234, e, FD_READ}) == SOCKET_ERROR && last_error(c) == WSAENOTSOCK);
    for (uint32_t s : {u, sender}) CHECK(xam(c, "NetDll_closesocket", {1, s}) == 0);
    CHECK(xam(c, "NetDll_WSACloseEvent", {e}) == 1);
}

void extras(ppc::Context &c) {
    uint32_t data = local(c, 0x400);
    CHECK(xam(c, "NetDll_WSAStartupEx", {1, 0x0202, data, 0x20501400}) == 0 && K->load16(data + 2) == 0x0202);
    uint32_t params = local(c, 0x600);
    std::memset(K->host(params), 0, 13);
    K->host(params)[0] = 13;
    CHECK(xam(c, "NetDll_XNetStartupEx", {1, params, 0x20501400}) == 0);
    uint32_t xnaddr = local(c, 0x640);
    std::memset(K->host(xnaddr), 0xEE, 0x24);
    CHECK(xam(c, "NetDll_XNetGetDebugXnAddr", {1, xnaddr}) == 1 && K->load32(xnaddr) == 0 &&
          K->load32(xnaddr + 0x20) == 0);
    uint32_t text = local(c, 0x680);
    CHECK(xam(c, "NetDll_XNetInAddrToString", {1, 0xC0A80102, text, 16}) == 0 &&
          std::string(reinterpret_cast<const char *>(K->host(text))) == "192.168.1.2");
    CHECK(xam(c, "NetDll_XNetInAddrToString", {1, 0xC0A80102, text, 4}) == 0 &&
          std::string(reinterpret_cast<const char *>(K->host(text))) == "192");
    uint32_t settings = local(c, 0x6A0);
    K->store32(settings, 8);
    CHECK(xam(c, "XampXAuthStartup", {settings}) == 0x80158406);
    K->store32(settings, 12);
    CHECK(xam(c, "XampXAuthStartup", {settings}) == 0x80158401);
    uint32_t count = local(c, 0x6B0);
    K->store32(count, 5);
    xam(c, "XampXAuthShutdown", {count});
    CHECK(K->load32(count) == 0 && xam(c, "XampXAuthGetTitleBuffer", {}) == 0);
    CHECK(xam(c, "NetDll_XHttpStartup", {1, 0, 0}) == 1 && last_error(c) == 0);
    CHECK(xam(c, "NetDll_XHttpOpen", {1, 0, 0, 0, 0, 0}) == 0 && last_error(c) == X_ERROR_NOT_SUPPORTED);
}

void main_thread(ppc::Context &c) {
    startup(c);
    datagrams(c);
    blocking(c);
    streams(c);
    events_and_lookups(c);
    media_verification(c);
    overlapped_io(c);
    event_select(c);
    extras(c);
    c.r[3] = 0x600D;
}

const Generated generated[] = {
    {F_MAIN, main_thread},
    {F_RECEIVER, receiver},
    {F_CLOSED_WAITER, closed_waiter},
    {F_SELECTER, selecter},
    {F_MEDIA_CALLBACK, media_callback},
    {F_WSA_ROUTINE, wsa_routine},
    {F_OVERLAPPED_WAITER, overlapped_waiter},
};

} // namespace

int main() {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        Objects &o = objects(k);
        Thread::Params p;
        p.stack_size = 256 * 1024;
        p.start_address = F_MAIN;
        uint32_t status;
        Ref<Thread> t = o.create_thread(p, status);
        CHECK(t && status == X_STATUS_SUCCESS);
        if (t) {
            CHECK(o.scheduler->start(*t));
            o.scheduler->join(*t);
            CHECK(t->exit_code == 0x600D);
        }
        CHECK(!k.stopped);
        if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
    }
    return finish("kernel_net");
}
