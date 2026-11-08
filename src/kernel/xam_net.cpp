/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XAM's network, offline: Winsock and XNet started with no Ethernet link,
// the title's address the loopback, and sockets that only reach each other.
// Translated from Xenia (BSD; see LICENSE-xenia): xam_net.cc's exports and
// xsocket.cc/xsocket.h's socket object and option codes; checked against
// them.
//
// Where it differs from Xenia, and why:
//   - the sockets are the kernel's own objects, with no host socket behind
//     them (Xenia opens a host socket for each and passes every call to the
//     host's Winsock). What Xenia's answers come to on a console with no
//     network partner is kept: a datagram socket binds, takes its options,
//     and finds nothing to read (WSAEWOULDBLOCK when non-blocking; a
//     blocking read waits, up to SO_RCVTIMEO). Datagrams sent to the
//     loopback (127.x.x.x) or broadcast are delivered to this title's
//     sockets bound to that port, from 127.0.0.1 and the sender's port, as
//     a host stack delivers them to itself; datagrams to anywhere else are
//     counted as sent and dropped. A stream socket binds and listens, and
//     never gets a connection; connect fails at once (WSAECONNREFUSED to
//     the loopback, WSAENETUNREACH elsewhere) where a host socket would
//     have tried the host's network;
//   - what the guest reads back is big-endian as the guest lays it out
//     (Xenia copies the host's sockaddr and option values as the host
//     wrote them: getsockname's family and getsockopt's value come out
//     little-endian, and getsockopt never updates the length);
//   - select's timeout is the guest's (Xenia scales it to the host clock);
//   - WSAWaitForMultipleEvents returns WSA_WAIT_EVENT_0 + n, WSA_WAIT_TIMEOUT
//     or WSA_WAIT_IO_COMPLETION with its timeout in milliseconds, as
//     Winsock (Xenia passes the milliseconds to the NT wait as an absolute
//     time, so every timed wait ends at once, and returns 0 for any wake);
//   - the errors a call meets are Winsock's own for the case (a second
//     bind WSAEINVAL, a taken port WSAEADDRINUSE, a datagram longer than
//     the buffer WSAEMSGSIZE...), where Xenia gets them from the host;
//   - XNetLogonGetTitleID is not in Xenia (see it below);
//   - the overlapped Winsock calls (WSARecv, WSARecvFrom, WSASend, WSASendTo,
//     WSAGetOverlappedResult, WSACancelOverlappedIO, WSAEventSelect) follow
//     Winsock 2's contract on these sockets. Xenia has only WSARecvFrom, a
//     stub that returns -1 with no error set, and WSASendTo, which sends
//     without completing the overlapped or writing the bytes sent; the rest
//     are not in Xenia. Their signatures are Winsock's with the caller type
//     first, as every NetDll export that takes a socket (Xenia's WSARecvFrom
//     leaves out lpFromlen; Winsock's has it, before the overlapped). A send
//     completes at once. A receive takes a datagram queued, else it pends
//     (WSA_IO_PENDING, whether or not the socket blocks, as Winsock's
//     overlapped receives do) until a datagram comes, the request is
//     cancelled (WSACancelOverlappedIO) or the socket is closed (both
//     WSA_OPERATION_ABORTED). A request ends as an NT I/O request does: the
//     WSAOVERLAPPED's Internal the status (STATUS_PENDING meanwhile) and
//     InternalHigh the bytes, then its event set or, with a completion
//     routine, the routine queued as a user APC to the thread that asked:
//     routine(error, bytes, overlapped). The routine's fourth argument
//     (flags) is not passed: the kernel's user APCs carry three.
//     WSAEventSelect makes the socket non-blocking and sets the event for
//     FD_READ when a datagram is queued and for FD_WRITE at once on a
//     datagram socket (always writable here);
//   - XNetInAddrToString writes the address in dotted form (Xenia's stub
//     writes "666.666.666.666").
//
// A socket ends with closesocket (its handle released then); a thread
// blocked on it is woken with WSAEINTR, as Winsock does.

#include "xam.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <vector>

namespace xf::kernel {
namespace {

// Winsock's errors, as the console's Winsock numbers them (Xenia's X_WSAError
// has the first five)
constexpr uint32_t X_WSA_INVALID_PARAMETER = 0x57;
constexpr uint32_t X_WSAEINTR = 10004, X_WSAEACCES = 10013, X_WSAEFAULT = 10014, X_WSAEINVAL = 10022;
constexpr uint32_t X_WSAEWOULDBLOCK = 10035, X_WSAENOTSOCK = 10038, X_WSAEMSGSIZE = 10040;
constexpr uint32_t X_WSAEPROTONOSUPPORT = 10043, X_WSAESOCKTNOSUPPORT = 10044, X_WSAEOPNOTSUPP = 10045;
constexpr uint32_t X_WSAEAFNOSUPPORT = 10047, X_WSAEADDRINUSE = 10048, X_WSAENETUNREACH = 10051;
constexpr uint32_t X_WSAENOBUFS = 10055;
constexpr uint32_t X_WSAEISCONN = 10056, X_WSAENOTCONN = 10057, X_WSAETIMEDOUT = 10060;
constexpr uint32_t X_WSAECONNREFUSED = 10061;
constexpr uint32_t X_WSA_OPERATION_ABORTED = 995, X_WSA_IO_INCOMPLETE = 996, X_WSA_IO_PENDING = 997;
constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;

// WSAOVERLAPPED (0x14): Internal (the request's NT status), InternalHigh
// (the bytes moved), Offset, OffsetHigh, hEvent
namespace wsaov {
constexpr uint32_t INTERNAL = 0x0, INTERNAL_HIGH = 0x4, EVENT = 0x10;
}
constexpr uint32_t STATUS_BUFFER_OVERFLOW = 0x80000005, STATUS_CANCELLED = 0xC0000120;

// WSAEventSelect's network events
constexpr uint32_t FD_READ = 0x01, FD_WRITE = 0x02, FD_ALL_EVENTS = 0x3FF;

// XSocket's enums and the option codes xsocket.cc translates
constexpr uint32_t AF_INET = 2, SOCK_STREAM = 1, SOCK_DGRAM = 2;
constexpr uint32_t IPPROTO_TCP = 6, IPPROTO_UDP = 17, IPPROTO_VDP = 254;
constexpr uint32_t SOL_SOCKET = 0xFFFF;
constexpr uint32_t SO_ACCEPTCONN = 0x0002, SO_REUSEADDR = 0x0004, SO_BROADCAST = 0x0020, SO_SNDBUF = 0x1001;
constexpr uint32_t SO_RCVBUF = 0x1002, SO_RCVTIMEO = 0x1006, SO_ERROR = 0x1007, SO_TYPE = 0x1008;
// the console's own: the socket's traffic unencrypted (Xenia's SetOption)
constexpr uint32_t SO_INSECURE = 0x5801, SO_INSECURE_2 = 0x5802;
constexpr uint32_t FIONBIO = 0x8004667E, FIONREAD = 0x4004667F;
constexpr uint32_t MSG_PEEK = 0x2;
constexpr uint32_t SOCKADDR_IN_SIZE = 0x10;
constexpr uint32_t LOOPBACK = 0x7F000001, BROADCAST = 0xFFFFFFFF;
constexpr uint16_t EPHEMERAL_FIRST = 49152;
constexpr uint32_t MAX_DATAGRAM = 65507; // UDP over IPv4's largest payload

constexpr uint32_t XNET_GET_XNADDR_PENDING = 0x0, XNET_GET_XNADDR_NONE = 0x1, XNET_GET_XNADDR_STATIC = 0x4;
constexpr uint32_t XNADDR_SIZE = 0x24;
constexpr uint32_t XNET_STARTUP_PARAMS_SIZE = 0xD;
// XNetStartupParams' cfgSockDefaultRecvBufsizeInK and ...SendBufsizeInK
constexpr uint32_t PARAM_RECV_BUF_K = 4, PARAM_SEND_BUF_K = 5;
constexpr uint32_t DEFAULT_BUFFER_K = 16; // XNet's default for both

constexpr uint32_t WSA_WAIT_FAILED = 0xFFFFFFFF;

// a WSABUF: the length, the address
struct Buffer {
    uint32_t address, length;
};

// An overlapped receive pending on a socket: where its datagram goes, and
// how its end is told.
struct Request {
    uint32_t overlapped = 0, routine = 0;
    std::vector<Buffer> buffers;
    uint32_t from = 0, from_length_ptr = 0;
    Ref<Event> event; // hEvent's (none with a completion routine)
    Ref<Thread> thread;
};

// A socket (Xenia's XSocket), its state under the objects' lock.
class Socket : public Object {
  public:
    explicit Socket(Objects &owner) : Object(owner, Type::Other) {}
    struct Datagram {
        uint32_t address;
        uint16_t port;
        std::vector<uint8_t> data;
    };
    std::deque<Request> pending; // overlapped receives, in the order asked
    Ref<Event> select_event;     // WSAEventSelect's
    uint32_t select_mask = 0;
    // set each time a request ends, for WSAGetOverlappedResult's waits
    // (each resets it, under the lock, before it waits)
    Ref<Event> progress;
    uint32_t type = 0, protocol = 0;
    bool nonblocking = false, listening = false, connected = false, closed = false;
    bool bound = false;
    uint32_t bound_address = 0, peer_address = 0;
    uint16_t bound_port = 0, peer_port = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> options; // (level, name): the value setsockopt gave
    std::deque<Datagram> queue;
    size_t queued_bytes = 0;
    // signaled while a read would not block (data queued, or closed)
    Ref<Event> readable;

    uint32_t option(uint32_t level, uint32_t name, uint32_t otherwise = 0) const {
        auto it = options.find({level, name});
        return it == options.end() ? otherwise : it->second;
    }
    bool datagram() const { return type == SOCK_DGRAM; }
};

struct NetState {
    std::mutex mutex; // the startup parameters
    uint8_t startup_params[XNET_STARTUP_PARAMS_SIZE] = {};
    // the title's open sockets, each retained until closesocket, under the
    // objects' lock (never released at the kernel's end: the objects outlive
    // nothing here, as the handle table's own)
    std::vector<Socket *> sockets;
    uint16_t next_ephemeral = EPHEMERAL_FIRST;
    // the starts not yet cleaned up, by caller (XNCALLER_TITLE, XNCALLER_SYSAPP)
    uint32_t wsa_started[3] = {}, xnet_started[3] = {};
};

// The callers the console's XAM takes in the first argument of the startup
// calls: the title (1) and the system application (2); any other is
// WSAEINVAL (xfxam). Xenia reads no caller.
constexpr uint32_t XNCALLER_TITLE = 1, XNCALLER_SYSAPP = 2;
constexpr uint32_t WSAEINVAL = 0x2726, WSANOTINITIALISED = 0x276D;
bool valid_caller(uint32_t caller) { return caller == XNCALLER_TITLE || caller == XNCALLER_SYSAPP; }

uint32_t default_buffer(Kernel &k, uint32_t param) {
    NetState &s = k.state<NetState>();
    std::lock_guard<std::mutex> l(s.mutex);
    uint32_t kb = s.startup_params[param];
    return (kb ? kb : DEFAULT_BUFFER_K) * 1024;
}

// a socket call's failure: the error as the thread's last, SOCKET_ERROR
void fail(Kernel &k, ppc::Context &c, uint32_t error) {
    set_last_error(k, error);
    result(c, SOCKET_ERROR);
}

// a socket by handle, not closed (another handle to it may outlive closesocket)
Ref<Socket> socket_by_handle(Kernel &k, uint32_t handle) {
    Objects &o = objects(k);
    Ref<Socket> s = o.lookup<Socket>(handle);
    if (!s) return s;
    std::lock_guard<std::mutex> l(o.lock);
    return s->closed ? Ref<Socket>() : s;
}

// XSOCKADDR_IN (0x10): family, port, address big-endian, zeros
void store_sockaddr(Kernel &k, uint32_t address, uint32_t ip, uint16_t port) {
    k.store16(address + 0, uint16_t(AF_INET));
    k.store16(address + 2, port);
    k.store32(address + 4, ip);
    std::memset(k.host(address + 8), 0, 8);
}

// the readable event follows the socket's state (lock held)
void update_readable(Objects &o, Socket &s) {
    bool now = !s.queue.empty() || s.closed;
    if (now == (s.readable->signal_state() > 0)) return;
    s.readable->set_signal_state(now ? 1 : 0);
    if (now) o.signaled(*s.readable);
}

void set_event(Objects &o, Event &e) { // (lock held)
    e.set_signal_state(1);
    o.signaled(e);
}

// a datagram scattered over the buffers, as much as fits: the bytes copied
uint32_t scatter(Kernel &k, const std::vector<uint8_t> &data, const std::vector<Buffer> &buffers) {
    uint32_t copied = 0;
    for (const Buffer &b : buffers) {
        uint32_t n = std::min<uint32_t>(b.length, uint32_t(data.size()) - copied);
        if (n) std::memcpy(k.host(b.address), data.data() + copied, n);
        copied += n;
        if (copied == data.size()) break;
    }
    return copied;
}

uint32_t total_length(const std::vector<Buffer> &buffers) {
    uint64_t n = 0;
    for (const Buffer &b : buffers) n += b.length;
    return uint32_t(std::min<uint64_t>(n, 0xFFFFFFFF));
}

// a request ended (lock held): the status and the bytes in its
// WSAOVERLAPPED, then its event set or its routine queued to the thread
// that asked, routine(error, bytes, overlapped)
void end_request(Kernel &k, Objects &o, Socket &s, Request &r, uint32_t status, uint32_t error, uint32_t bytes) {
    k.store32(r.overlapped + wsaov::INTERNAL_HIGH, bytes);
    k.store32(r.overlapped + wsaov::INTERNAL, status);
    if (r.routine) {
        if (r.thread) o.queue_user_apc(*r.thread, r.routine, error, bytes, r.overlapped);
    } else if (r.event) {
        set_event(o, *r.event);
    }
    set_event(o, *s.progress);
}

// a datagram into a pending request (lock held)
void fill_request(Kernel &k, Objects &o, Socket &s, Request &r, const Socket::Datagram &d) {
    uint32_t copied = scatter(k, d.data, r.buffers);
    if (r.from) store_sockaddr(k, r.from, d.address, d.port);
    if (r.from_length_ptr) k.store32(r.from_length_ptr, SOCKADDR_IN_SIZE);
    bool whole = copied == d.data.size();
    end_request(k, o, s, r, whole ? X_STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW, whole ? 0 : X_WSAEMSGSIZE, copied);
}

// every request pending on the socket ends aborted (lock held), moved to
// ended: what they hold is released after the lock (a last release under
// it could take it again)
void abort_requests(Kernel &k, Objects &o, Socket &s, std::vector<Request> &ended) {
    for (Request &r : s.pending) {
        end_request(k, o, s, r, STATUS_CANCELLED, X_WSA_OPERATION_ABORTED, 0);
        ended.push_back(std::move(r));
    }
    s.pending.clear();
}

bool port_taken(NetState &n, const Socket &self, uint16_t port) {
    for (const Socket *s : n.sockets)
        if (s != &self && !s->closed && s->bound && s->bound_port == port && s->type == self.type) return true;
    return false;
}

// binds to the next free ephemeral port, as a host stack does for a socket
// that sends or connects unbound (lock held)
void bind_implicitly(NetState &n, Socket &s) {
    if (s.bound) return;
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t port = n.next_ephemeral;
        n.next_ephemeral = n.next_ephemeral == 0xFFFF ? EPHEMERAL_FIRST : uint16_t(n.next_ephemeral + 1);
        if (!port_taken(n, s, port)) {
            s.bound = true, s.bound_address = 0, s.bound_port = port;
            return;
        }
    }
}

// whether a datagram sent to address stays on this console
bool local_destination(uint32_t address) { return (address >> 24) == 127 || address == BROADCAST; }

// a datagram from s to address:port: delivered to this title's sockets bound
// there when the address is the loopback or broadcast, dropped otherwise
// (lock held); the bytes sent, else the error. The overlapped receives it
// ends go to ended, released after the lock.
uint32_t send_datagram(Kernel &k, Socket &s, uint32_t address, uint16_t port, const uint8_t *data, uint32_t length,
                       uint32_t &error, std::vector<Request> &ended) {
    if (address == BROADCAST && !s.option(SOL_SOCKET, SO_BROADCAST)) {
        error = X_WSAEACCES;
        return SOCKET_ERROR;
    }
    if (length > MAX_DATAGRAM) {
        error = X_WSAEMSGSIZE;
        return SOCKET_ERROR;
    }
    Objects &o = objects(k);
    NetState &n = k.state<NetState>();
    bind_implicitly(n, s);
    if (!local_destination(address)) return length;
    for (Socket *r : n.sockets) {
        Socket &to = *r;
        if (to.closed || !to.datagram() || !to.bound || to.bound_port != port) continue;
        if (to.bound_address && to.bound_address != address && address != BROADCAST) continue;
        if (to.connected && (to.peer_address != LOOPBACK || to.peer_port != s.bound_port)) continue;
        if (!to.pending.empty()) { // an overlapped receive waits for it
            fill_request(k, o, to, to.pending.front(),
                         {LOOPBACK, s.bound_port, std::vector<uint8_t>(data, data + length)});
            ended.push_back(std::move(to.pending.front()));
            to.pending.pop_front();
            continue;
        }
        uint32_t limit = to.option(SOL_SOCKET, SO_RCVBUF, default_buffer(k, PARAM_RECV_BUF_K));
        if (to.queued_bytes + length > limit) continue; // the receiver's buffer is full: lost, as UDP loses it
        to.queue.push_back({LOOPBACK, s.bound_port, std::vector<uint8_t>(data, data + length)});
        to.queued_bytes += length;
        update_readable(o, to);
        if (to.select_event && (to.select_mask & FD_READ)) set_event(o, *to.select_event);
    }
    return length;
}

// waits, the lock released meanwhile, for the socket to become readable or
// the deadline; false at the deadline
bool wait_readable(Objects &o, Socket &s, uint64_t deadline) {
    Thread *t = current_thread();
    if (!t) return false;
    Dispatcher *list[] = {s.readable.get()};
    return o.wait(*t, list, false, false, deadline) != X_STATUS_TIMEOUT;
}

uint64_t deadline_ms(Objects &o, uint32_t ms) { return ms ? o.now() + uint64_t(ms) * 10000 : Scheduler::NEVER; }

// recv, recvfrom, WSARecv and WSARecvFrom: the first datagram queued
// (MSG_PEEK leaves it) scattered over the buffers, waiting for one when the
// socket blocks; its length, else the error. An overlapped request (pend)
// that finds a datagram ends at once; one that finds none is left pending
// (WSA_IO_PENDING).
uint32_t receive(Kernel &k, Socket &s, const std::vector<Buffer> &buffers, uint32_t flags, uint32_t from,
                 uint32_t from_length_ptr, uint32_t &error, Request *pend = nullptr) {
    if (!s.datagram()) {
        error = s.listening ? X_WSAEINVAL : X_WSAENOTCONN;
        return SOCKET_ERROR;
    }
    if (from && from_length_ptr && k.load32(from_length_ptr) < SOCKADDR_IN_SIZE) {
        error = X_WSAEFAULT;
        return SOCKET_ERROR;
    }
    Objects &o = objects(k);
    uint64_t deadline = Scheduler::NEVER;
    bool deadline_set = false;
    std::unique_lock<std::mutex> l(o.lock);
    if (!s.bound) {
        error = X_WSAEINVAL; // Winsock: a datagram socket reads only once bound
        return SOCKET_ERROR;
    }
    for (;;) {
        if (s.closed) {
            error = X_WSAEINTR;
            return SOCKET_ERROR;
        }
        if (!s.queue.empty()) break;
        if (pend) {
            k.store32(pend->overlapped + wsaov::INTERNAL, X_STATUS_PENDING);
            k.store32(pend->overlapped + wsaov::INTERNAL_HIGH, 0);
            if (pend->event) pend->event->set_signal_state(0);
            pend->buffers = buffers, pend->from = from, pend->from_length_ptr = from_length_ptr;
            s.pending.push_back(std::move(*pend));
            error = X_WSA_IO_PENDING;
            return SOCKET_ERROR;
        }
        if (s.nonblocking) {
            error = X_WSAEWOULDBLOCK;
            return SOCKET_ERROR;
        }
        if (!deadline_set) deadline = deadline_ms(o, s.option(SOL_SOCKET, SO_RCVTIMEO)), deadline_set = true;
        l.unlock();
        bool woken = wait_readable(o, s, deadline);
        l.lock();
        if (!woken && s.queue.empty()) {
            error = X_WSAETIMEDOUT;
            return SOCKET_ERROR;
        }
    }
    Socket::Datagram &d = s.queue.front();
    uint32_t size = uint32_t(d.data.size()), copied = scatter(k, d.data, buffers);
    if (from) store_sockaddr(k, from, d.address, d.port);
    if (from_length_ptr) k.store32(from_length_ptr, SOCKADDR_IN_SIZE);
    if (!(flags & MSG_PEEK)) {
        s.queued_bytes -= size;
        s.queue.pop_front();
        update_readable(o, s);
    }
    if (pend)
        end_request(k, o, s, *pend, copied < size ? STATUS_BUFFER_OVERFLOW : X_STATUS_SUCCESS,
                    copied < size ? X_WSAEMSGSIZE : 0, copied);
    if (copied < size) { // the rest of the datagram is lost, as Winsock loses it
        error = X_WSAEMSGSIZE;
        return SOCKET_ERROR;
    }
    return size;
}

// ---- fd_set (x_fd_set: a count, 64 handles) ----

constexpr uint32_t FD_SETSIZE = 64;

struct SocketSet {
    uint32_t address = 0;
    std::vector<Ref<Socket>> sockets;
};

// the sockets a guest set names; false if one is not a socket
bool load_set(Kernel &k, uint32_t address, SocketSet &set) {
    set.address = address;
    if (!address) return true;
    uint32_t count = std::min(k.load32(address), FD_SETSIZE);
    for (uint32_t i = 0; i < count; i++) {
        Ref<Socket> s = socket_by_handle(k, k.load32(address + 4 + 4 * i));
        if (!s) return false;
        set.sockets.push_back(std::move(s));
    }
    return true;
}

// the sockets left in a set rewritten (Xenia's host_set::Store)
void store_set(Kernel &k, const SocketSet &set, const std::vector<bool> &ready) {
    if (!set.address) return;
    uint32_t count = 0;
    for (size_t i = 0; i < set.sockets.size(); i++)
        if (ready[i]) k.store32(set.address + 4 + 4 * count++, set.sockets[i]->handle());
    k.store32(set.address, count);
}

} // namespace

// ---- Winsock ----

// (caller, version, X_WSADATA *): Winsock 2.2
XF_EXPORT(xam, NetDll_WSAStartup) {
    uint32_t caller = arg(k, c, 0), version = arg(k, c, 1) & 0xFFFF, data = arg(k, c, 2);
    if (!valid_caller(caller)) return result(c, WSAEINVAL);
    {
        NetState &s = k.state<NetState>();
        std::lock_guard<std::mutex> l(s.mutex);
        s.wsa_started[caller]++;
    }
    if (data) {
        std::memset(k.host(data), 0, 0x190);
        k.store16(data + 0, uint16_t(version));
        k.store16(data + 2, 0x0202);
    }
    result(c, 0);
}

// (caller, version, X_WSADATA *, version_requested): as WSAStartup
XF_EXPORT(xam, NetDll_WSAStartupEx) { find_export_fn("xam", "NetDll_WSAStartup")(k, c); }

// (caller): the console's XAM answers SOCKET_ERROR (-1, last error
// WSANOTINITIALISED) for a cleanup with no startup before it (xfxam); Xenia's
// answers 0 always.
XF_EXPORT(xam, NetDll_WSACleanup) {
    uint32_t caller = arg(k, c, 0);
    NetState &s = k.state<NetState>();
    std::lock_guard<std::mutex> l(s.mutex);
    if (!valid_caller(caller) || !s.wsa_started[caller]) {
        set_last_error(k, WSANOTINITIALISED);
        return result(c, 0xFFFFFFFF);
    }
    s.wsa_started[caller]--;
    result(c, 0);
}
XF_EXPORT(xam, NetDll_WSAGetLastError) { result(c, last_error(k)); }
XF_EXPORT(xam, NetDll_WSASetLastError) { set_last_error(k, arg(k, c, 0)); }

// ---- XNet ----

// (caller, XNetStartupParams *): kept for XNetGetOpt
XF_EXPORT(xam, NetDll_XNetStartup) {
    uint32_t caller = arg(k, c, 0);
    if (!valid_caller(caller)) return result(c, WSAEINVAL);
    {
        NetState &s = k.state<NetState>();
        std::lock_guard<std::mutex> l(s.mutex);
        s.xnet_started[caller]++;
    }
    if (uint32_t params = arg(k, c, 1)) {
        NetState &s = k.state<NetState>();
        std::lock_guard<std::mutex> l(s.mutex);
        std::memcpy(s.startup_params, k.host(params), XNET_STARTUP_PARAMS_SIZE);
    }
    result(c, 0);
}

// (caller, XNetStartupParams *, version_requested): as XNetStartup
XF_EXPORT(xam, NetDll_XNetStartupEx) { find_export_fn("xam", "NetDll_XNetStartup")(k, c); }

// (caller): the title's cleanup with no startup before it is
// WSANOTINITIALISED on the console's XAM (xfxam; the system application's, and
// any other caller's, 0); Xenia's answers 0 always.
XF_EXPORT(xam, NetDll_XNetCleanup) {
    uint32_t caller = arg(k, c, 0);
    NetState &s = k.state<NetState>();
    std::lock_guard<std::mutex> l(s.mutex);
    if (caller == XNCALLER_TITLE && !s.xnet_started[caller]) return result(c, WSANOTINITIALISED);
    if (valid_caller(caller) && s.xnet_started[caller]) s.xnet_started[caller]--;
    result(c, 0);
}

// (machine_id_ptr): not logged on (X_ERROR_LOGON_NOT_LOGGED_ON)
XF_EXPORT(xam, XNetLogonGetMachineID) {
    if (uint32_t p = arg(k, c, 0)) k.store64(p, 0);
    result(c, 0x80151802);
}

// (index): the running title's ID. Xenia has no implementation (its
// undefined import leaves r3, the argument, as it is); the title's own ID
// is what the name asks for, and the one caller seen (a UE3 game) only
// puts it in a Live request that fails earlier (no user signed in to Live).
XF_EXPORT(xam, XNetLogonGetTitleID) { result(c, title_id(k)); }

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
        // (a fixed seed in the deterministic mode, as XeCryptRandom's)
        static std::atomic<uint64_t> calls{0};
        std::mt19937_64 gen(k.deterministic() ? 0x584E657452616E64ull + calls++ : std::random_device{}());
        for (uint32_t i = 0; i < length; i++) k.host(buffer + i)[0] = uint8_t(gen());
    }
    result(c, X_ERROR_SUCCESS);
}

// (caller, XNADDR *): before the title's XNetStartup, the console's XAM
// (no adapter, no startup: xfxam) answers 0, XNET_GET_XNADDR_PENDING, with
// the structure zeroed but for a 1 in the byte at +0x20, taking only the
// network lock. After it, here: the loopback; an Ethernet address of 0xCC
// (RakNet makes its random numbers from it, and zero ones fail), where Xenia
// answers it at once (and the console's real adapter would have its own).
XF_EXPORT(xam, NetDll_XNetGetTitleXnAddr) {
    uint32_t addr = arg(k, c, 1);
    uint8_t *p = k.host(addr);
    {
        NetState &s = k.state<NetState>();
        std::lock_guard<std::mutex> l(s.mutex);
        if (!s.xnet_started[XNCALLER_TITLE]) {
            std::memset(p, 0, XNADDR_SIZE);
            p[0x20] = 1;
            return result(c, XNET_GET_XNADDR_PENDING);
        }
    }
    p[0] = 127, p[1] = 0, p[2] = 0, p[3] = 1; // ina
    std::memset(p + 4, 0, 6);                 // inaOnline, wPortOnline
    std::memset(p + 10, 0xCC, 6);             // abEnet
    std::memset(p + 16, 0, 20);               // abOnline
    result(c, XNET_GET_XNADDR_STATIC);
}

// (caller, XNADDR *): no debugger, the address zero (XNET_GET_XNADDR_NONE;
// the callers give up gracefully)
XF_EXPORT(xam, NetDll_XNetGetDebugXnAddr) {
    if (uint32_t addr = arg(k, c, 1)) std::memset(k.host(addr), 0, XNADDR_SIZE);
    result(c, XNET_GET_XNADDR_NONE);
}

// (caller, in_addr, string, size): the address in dotted form, truncated
// to the size with its terminator (see the top)
XF_EXPORT(xam, NetDll_XNetInAddrToString) {
    uint32_t ip = arg(k, c, 1), out = arg(k, c, 2), size = arg(k, c, 3);
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    if (out && size) {
        uint32_t n = std::min<uint32_t>(uint32_t(std::strlen(text)), size - 1);
        std::memcpy(k.host(out), text, n);
        k.host(out)[n] = 0;
    }
    result(c, 0);
}

// (caller, XNADDR *, machine_id_ptr): not signed in to Live
XF_EXPORT(xam, NetDll_XNetXnAddrToMachineId) { result(c, 1); }
// (caller, XNADDR *, XNKID *, IN_ADDR *) and its reverse: no secure
// association to make offline
XF_EXPORT(xam, NetDll_XNetXnAddrToInAddr) { result(c, 1); }
XF_EXPORT(xam, NetDll_XNetInAddrToXnAddr) { result(c, 1); }

// no cable
XF_EXPORT(xam, NetDll_XNetGetEthernetLinkStatus) { result(c, 0); }

// (caller, host, event, XNDNS **): an XNDNS (0x28) from the system heap
// with a failed status, the event set at once
XF_EXPORT(xam, NetDll_XNetDnsLookup) {
    uint32_t event = arg(k, c, 2), dns_ptr = arg(k, c, 3);
    if (dns_ptr) {
        uint32_t dns = k.memory().system_alloc(0x28);
        std::memset(k.host(dns), 0, 0x28);
        k.store32(dns + 0, 1); // status: non-zero, an error
        k.store32(dns_ptr, dns);
    }
    if (event) {
        Objects &o = objects(k);
        if (Ref<Event> e = o.lookup<Event>(event)) {
            std::lock_guard<std::mutex> l(o.lock);
            e->set_signal_state(1);
            o.signaled(*e);
        }
    }
    result(c, 0);
}

// (caller, XNDNS *)
XF_EXPORT(xam, NetDll_XNetDnsRelease) {
    uint32_t dns = arg(k, c, 1);
    if (!dns) return result(c, X_STATUS_INVALID_PARAMETER);
    k.memory().system_free(dns);
    result(c, 0);
}

// (caller, flags, event, XNQOS **): an XNQOS (0x20) with nothing in it
// (some games read it whatever the result), the event set at once
XF_EXPORT(xam, NetDll_XNetQosServiceLookup) {
    uint32_t event = arg(k, c, 2), qos_ptr = arg(k, c, 3);
    if (qos_ptr) {
        uint32_t qos = k.memory().system_alloc(0x20);
        std::memset(k.host(qos), 0, 0x20);
        k.store32(qos_ptr, qos);
    }
    if (event) {
        Objects &o = objects(k);
        if (Ref<Event> e = o.lookup<Event>(event)) {
            std::lock_guard<std::mutex> l(o.lock);
            e->set_signal_state(1);
            o.signaled(*e);
        }
    }
    result(c, 0);
}

// (caller, XNQOS *)
XF_EXPORT(xam, NetDll_XNetQosRelease) {
    uint32_t qos = arg(k, c, 1);
    if (!qos) return result(c, X_STATUS_INVALID_PARAMETER);
    k.memory().system_free(qos);
    result(c, 0);
}

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

// ---- XHttp: offline, as Xenia's stubs (XAuth's are in xam_info.cpp) ----

// (caller, reserved, reserved_ptr): TRUE, the last error cleared
XF_EXPORT(xam, NetDll_XHttpStartup) {
    set_last_error(k, X_ERROR_SUCCESS);
    result(c, 1);
}

// (caller, user_agent, access_type, proxy_name, proxy_bypass, flags): no
// session, ERROR_NOT_SUPPORTED
XF_EXPORT(xam, NetDll_XHttpOpen) {
    set_last_error(k, X_ERROR_NOT_SUPPORTED);
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

// ---- WSA events: the kernel's manual-reset events ----

XF_EXPORT(xam, NetDll_WSACreateEvent) {
    Ref<Event> e = objects(k).create<Event>(GuestType::EventNotification, header::SIZE);
    result(c, e ? e->handle() : 0);
}

// (event): TRUE, else FALSE with the error as the last
XF_EXPORT(xam, NetDll_WSACloseEvent) {
    Objects &o = objects(k);
    uint32_t handle = arg(k, c, 0);
    if (Ref<Event> e = o.lookup<Event>(handle)) {
        std::lock_guard<std::mutex> l(o.lock);
        if (o.release_handle(handle)) return result(c, 1);
    }
    set_last_error(k, X_ERROR_INVALID_HANDLE);
    result(c, 0);
}

XF_EXPORT(xam, NetDll_WSASetEvent) {
    Objects &o = objects(k);
    Ref<Event> e = o.lookup<Event>(arg(k, c, 0));
    if (!e) return set_last_error(k, X_ERROR_INVALID_HANDLE), result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    e->set_signal_state(1);
    o.signaled(*e);
    result(c, 1);
}

XF_EXPORT(xam, NetDll_WSAResetEvent) {
    Objects &o = objects(k);
    Ref<Event> e = o.lookup<Event>(arg(k, c, 0));
    if (!e) return set_last_error(k, X_ERROR_INVALID_HANDLE), result(c, 0);
    std::lock_guard<std::mutex> l(o.lock);
    e->set_signal_state(0);
    result(c, 1);
}

// (count, events, wait_all, timeout_ms, alertable)
XF_EXPORT(xam, NetDll_WSAWaitForMultipleEvents) {
    uint32_t count = arg(k, c, 0), events = arg(k, c, 1), wait_all = arg(k, c, 2), timeout = arg(k, c, 3);
    uint32_t alertable = arg(k, c, 4);
    Objects &o = objects(k);
    Thread *t = current_thread();
    if (!t || !count || count > 64) return set_last_error(k, X_WSA_INVALID_PARAMETER), result(c, WSA_WAIT_FAILED);
    std::vector<Ref<Event>> refs;
    std::vector<Dispatcher *> list;
    for (uint32_t i = 0; i < count; i++) {
        refs.push_back(o.lookup<Event>(k.load32(events + 4 * i)));
        if (!refs.back()) return set_last_error(k, X_ERROR_INVALID_HANDLE), result(c, WSA_WAIT_FAILED);
        list.push_back(refs.back().get());
    }
    uint64_t deadline = timeout == 0xFFFFFFFF ? Scheduler::NEVER : o.now() + uint64_t(timeout) * 10000;
    uint32_t status = o.wait(*t, list, wait_all != 0, alertable != 0, deadline);
    if (status == X_STATUS_USER_APC) o.deliver_apcs(*t, 1);
    result(c, status); // WSA_WAIT_EVENT_0 + n, WSA_WAIT_TIMEOUT, WSA_WAIT_IO_COMPLETION as NT's
}

// ---- sockets ----

// (caller, af, type, protocol): a handle, else INVALID_SOCKET
XF_EXPORT(xam, NetDll_socket) {
    uint32_t af = arg(k, c, 1), type = arg(k, c, 2), protocol = arg(k, c, 3);
    if (af != AF_INET) return fail(k, c, X_WSAEAFNOSUPPORT);
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return fail(k, c, X_WSAESOCKTNOSUPPORT);
    bool fits = protocol == 0 ||
                (type == SOCK_STREAM ? protocol == IPPROTO_TCP : protocol == IPPROTO_UDP || protocol == IPPROTO_VDP);
    if (!fits) return fail(k, c, X_WSAEPROTONOSUPPORT);
    Objects &o = objects(k);
    Ref<Event> readable = o.create<Event>(GuestType::EventNotification, header::SIZE);
    Ref<Event> progress = o.create<Event>(GuestType::EventNotification, header::SIZE);
    if (!readable || !progress) return fail(k, c, X_WSAENOBUFS);
    Ref<Socket> s(new Socket(o));
    s->type = type, s->protocol = protocol, s->readable = readable, s->progress = progress;
    std::lock_guard<std::mutex> l(o.lock);
    o.release_handle(readable->handle()); // the socket's own, waited on through it
    o.release_handle(progress->handle());
    uint32_t handle = o.add_handle(s.get());
    s->retain();
    k.state<NetState>().sockets.push_back(s.get());
    result(c, handle);
}

// (caller, socket): its handle released; a thread blocked on it woken
XF_EXPORT(xam, NetDll_closesocket) {
    uint32_t handle = arg(k, c, 1);
    Ref<Socket> s = socket_by_handle(k, handle);
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::vector<Request> ended;
    Ref<Event> ended_event;
    std::lock_guard<std::mutex> l(o.lock);
    s->closed = true;
    s->queue.clear(), s->queued_bytes = 0;
    abort_requests(k, o, *s, ended);
    ended_event = std::move(s->select_event);
    update_readable(o, *s);
    std::erase(k.state<NetState>().sockets, s.get());
    s->release(); // the registry's (the Ref here keeps it until the export returns)
    o.release_handle(handle);
    result(c, 0);
}

// (caller, socket, how)
XF_EXPORT(xam, NetDll_shutdown) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (!s->connected) return fail(k, c, X_WSAENOTCONN);
    result(c, 0);
}

// (caller, socket, level, name, value, length): kept, read back by getsockopt
XF_EXPORT(xam, NetDll_setsockopt) {
    uint32_t level = arg(k, c, 2), name = arg(k, c, 3), value = arg(k, c, 4), length = arg(k, c, 5);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (level == SOL_SOCKET && (name == SO_INSECURE || name == SO_INSECURE_2)) return result(c, 0);
    if (!value || length == 0) return fail(k, c, X_WSAEFAULT);
    // an int, or a BOOL the console's Winsock takes as a byte as well
    uint32_t v = length >= 4 ? k.load32(value) : k.host(value)[0];
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    s->options[{level, name}] = v;
    result(c, 0);
}

// (caller, socket, level, name, value, length_ptr): an int, big-endian
XF_EXPORT(xam, NetDll_getsockopt) {
    uint32_t level = arg(k, c, 2), name = arg(k, c, 3), value = arg(k, c, 4), length_ptr = arg(k, c, 5);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (!value || !length_ptr || k.load32(length_ptr) < 4) return fail(k, c, X_WSAEFAULT);
    Objects &o = objects(k);
    uint32_t v;
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (level == SOL_SOCKET && name == SO_TYPE)
            v = s->type;
        else if (level == SOL_SOCKET && name == SO_ACCEPTCONN)
            v = s->listening;
        else if (level == SOL_SOCKET && name == SO_ERROR)
            v = 0;
        else if (level == SOL_SOCKET && name == SO_RCVBUF)
            v = s->option(level, name, default_buffer(k, PARAM_RECV_BUF_K));
        else if (level == SOL_SOCKET && name == SO_SNDBUF)
            v = s->option(level, name, default_buffer(k, PARAM_SEND_BUF_K));
        else
            v = s->option(level, name);
    }
    k.store32(value, v);
    k.store32(length_ptr, 4);
    result(c, 0);
}

// (caller, socket, command, arg): FIONBIO and FIONREAD
XF_EXPORT(xam, NetDll_ioctlsocket) {
    uint32_t command = arg(k, c, 2), argp = arg(k, c, 3);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (!argp) return fail(k, c, X_WSAEFAULT);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (command == FIONBIO)
        s->nonblocking = k.load32(argp) != 0;
    else if (command == FIONREAD)
        k.store32(argp, uint32_t(s->queued_bytes));
    else
        return fail(k, c, X_WSAEINVAL);
    result(c, 0);
}

// (caller, socket, XSOCKADDR_IN *, length): port 0 an ephemeral one
XF_EXPORT(xam, NetDll_bind) {
    uint32_t name = arg(k, c, 2), length = arg(k, c, 3);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (!name || length < SOCKADDR_IN_SIZE) return fail(k, c, X_WSAEFAULT);
    if (k.load16(name) != AF_INET) return fail(k, c, X_WSAEAFNOSUPPORT);
    uint16_t port = k.load16(name + 2);
    uint32_t address = k.load32(name + 4);
    Objects &o = objects(k);
    NetState &n = k.state<NetState>();
    std::lock_guard<std::mutex> l(o.lock);
    if (s->bound) return fail(k, c, X_WSAEINVAL);
    if (port) {
        if (port_taken(n, *s, port) && !s->option(SOL_SOCKET, SO_REUSEADDR)) return fail(k, c, X_WSAEADDRINUSE);
        s->bound = true, s->bound_port = port;
    } else {
        bind_implicitly(n, *s);
    }
    s->bound_address = address;
    result(c, 0);
}

// (caller, socket, XSOCKADDR *, length): a datagram socket's default peer;
// a stream socket finds nobody (see the top)
XF_EXPORT(xam, NetDll_connect) {
    uint32_t name = arg(k, c, 2), length = arg(k, c, 3);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (!name || length < SOCKADDR_IN_SIZE) return fail(k, c, X_WSAEFAULT);
    if (k.load16(name) != AF_INET) return fail(k, c, X_WSAEAFNOSUPPORT);
    uint16_t port = k.load16(name + 2);
    uint32_t address = k.load32(name + 4);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (s->listening) return fail(k, c, X_WSAEINVAL);
    if (!s->datagram()) {
        if (s->connected) return fail(k, c, X_WSAEISCONN);
        return fail(k, c, local_destination(address) ? X_WSAECONNREFUSED : X_WSAENETUNREACH);
    }
    bind_implicitly(k.state<NetState>(), *s);
    s->connected = address != 0;
    s->peer_address = address, s->peer_port = port;
    result(c, 0);
}

// (caller, socket, backlog)
XF_EXPORT(xam, NetDll_listen) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (s->datagram()) return fail(k, c, X_WSAEOPNOTSUPP);
    if (!s->bound) return fail(k, c, X_WSAEINVAL);
    s->listening = true;
    result(c, 0);
}

// (caller, socket, XSOCKADDR *, length_ptr): no connection ever comes; a
// blocking accept waits until the socket is closed
XF_EXPORT(xam, NetDll_accept) {
    uint32_t addr = arg(k, c, 2);
    if (!addr) return fail(k, c, X_WSAEFAULT); // as Xenia's
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    {
        std::lock_guard<std::mutex> l(o.lock);
        if (!s->listening) return fail(k, c, X_WSAEINVAL);
        if (s->nonblocking) return fail(k, c, X_WSAEWOULDBLOCK);
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> l(o.lock);
            if (s->closed) break;
        }
        if (!current_thread()) return fail(k, c, X_WSAEWOULDBLOCK);
        wait_readable(o, *s, Scheduler::NEVER);
    }
    fail(k, c, X_WSAEINTR);
}

// (caller, nfds, readfds, writefds, exceptfds, timeval *): the sockets
// ready in each set (a datagram socket is always writable; nothing is ever
// exceptional), waiting up to the timeout for one to become readable
XF_EXPORT(xam, NetDll_select) {
    uint32_t timeout = arg(k, c, 5);
    SocketSet sets[3];
    for (int i = 0; i < 3; i++)
        if (!load_set(k, arg(k, c, 2 + i), sets[i])) return fail(k, c, X_WSAENOTSOCK);
    if (sets[0].sockets.empty() && sets[1].sockets.empty() && sets[2].sockets.empty())
        return fail(k, c, X_WSAEINVAL); // nothing to wait for: Winsock's answer
    Objects &o = objects(k);
    uint64_t deadline = Scheduler::NEVER;
    if (timeout) {
        // timeval: tv_sec, tv_usec
        uint64_t us = uint64_t(int32_t(k.load32(timeout))) * 1000000 + uint64_t(int32_t(k.load32(timeout + 4)));
        deadline = o.now() + us * 10;
    }
    std::vector<bool> ready[3];
    for (;;) {
        uint32_t total = 0;
        std::vector<Dispatcher *> waits;
        {
            std::lock_guard<std::mutex> l(o.lock);
            for (int i = 0; i < 3; i++) {
                ready[i].assign(sets[i].sockets.size(), false);
                for (size_t n = 0; n < sets[i].sockets.size(); n++) {
                    Socket &s = *sets[i].sockets[n];
                    bool r = i == 0 ? !s.queue.empty() || s.closed : i == 1 ? s.datagram() || s.connected : false;
                    ready[i][n] = r;
                    total += r;
                    waits.push_back(s.readable.get());
                }
            }
        }
        Thread *t = current_thread();
        if (total || (timeout && o.now() >= deadline) || !t) {
            for (int i = 0; i < 3; i++) store_set(k, sets[i], ready[i]);
            return result(c, total);
        }
        o.wait(*t, waits, false, false, deadline);
    }
}

// (socket, x_fd_set *): whether the set names the socket
XF_EXPORT(xam, NetDll___WSAFDIsSet) {
    uint32_t handle = arg(k, c, 0), set = arg(k, c, 1);
    uint32_t count = std::min(k.load32(set), FD_SETSIZE);
    for (uint32_t i = 0; i < count; i++)
        if (k.load32(set + 4 + 4 * i) == handle) return result(c, 1);
    result(c, 0);
}

// (caller, socket, buffer, length, flags)
XF_EXPORT(xam, NetDll_recv) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    bool connected;
    {
        std::lock_guard<std::mutex> l(objects(k).lock);
        connected = s->connected;
    }
    if (s->datagram() && !connected) return fail(k, c, X_WSAENOTCONN);
    uint32_t error = 0;
    uint32_t n = receive(k, *s, {{arg(k, c, 2), arg(k, c, 3)}}, arg(k, c, 4), 0, 0, error);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    result(c, n);
}

// (caller, socket, buffer, length, flags, XSOCKADDR_IN *from, from_length_ptr)
XF_EXPORT(xam, NetDll_recvfrom) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    uint32_t error = 0;
    uint32_t n = receive(k, *s, {{arg(k, c, 2), arg(k, c, 3)}}, arg(k, c, 4), arg(k, c, 5), arg(k, c, 6), error);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    result(c, n);
}

// (caller, socket, buffer, length, flags): to a datagram socket's peer
XF_EXPORT(xam, NetDll_send) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::vector<Request> ended;
    std::lock_guard<std::mutex> l(o.lock);
    if (!s->connected) return fail(k, c, X_WSAENOTCONN);
    uint32_t error = 0, buffer = arg(k, c, 2), length = arg(k, c, 3);
    uint32_t n = send_datagram(k, *s, s->peer_address, s->peer_port, k.host(buffer), length, error, ended);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    result(c, n);
}

// (caller, socket, buffer, length, flags, XSOCKADDR_IN *to, to_length)
XF_EXPORT(xam, NetDll_sendto) {
    uint32_t buffer = arg(k, c, 2), length = arg(k, c, 3), to = arg(k, c, 5), to_length = arg(k, c, 6);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::vector<Request> ended;
    std::lock_guard<std::mutex> l(o.lock);
    if (!s->datagram()) return fail(k, c, X_WSAENOTCONN);
    uint32_t address = s->peer_address;
    uint16_t port = s->peer_port;
    if (to) {
        if (to_length < SOCKADDR_IN_SIZE) return fail(k, c, X_WSAEFAULT);
        address = k.load32(to + 4), port = k.load16(to + 2);
    } else if (!s->connected) {
        return fail(k, c, X_WSAENOTCONN);
    }
    uint32_t error = 0;
    uint32_t n = send_datagram(k, *s, address, port, k.host(buffer), length, error, ended);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    result(c, n);
}

// (caller, socket, XSOCKADDR_IN *, length_ptr): where it is bound
XF_EXPORT(xam, NetDll_getsockname) {
    uint32_t name = arg(k, c, 2), length_ptr = arg(k, c, 3);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    if (!name || !length_ptr || k.load32(length_ptr) < SOCKADDR_IN_SIZE) return fail(k, c, X_WSAEFAULT);
    Objects &o = objects(k);
    std::lock_guard<std::mutex> l(o.lock);
    if (!s->bound) return fail(k, c, X_WSAEINVAL);
    store_sockaddr(k, name, s->bound_address, s->bound_port);
    k.store32(length_ptr, SOCKADDR_IN_SIZE);
    result(c, 0);
}

// ---- overlapped Winsock (see the top) ----

namespace {

constexpr uint32_t MAX_WSABUFS = 1024;

// a WSABUF array (length, address each); false if there is none
bool load_buffers(Kernel &k, uint32_t address, uint32_t count, std::vector<Buffer> &out) {
    if (!address || !count || count > MAX_WSABUFS) return false;
    for (uint32_t i = 0; i < count; i++) out.push_back({k.load32(address + 8 * i + 4), k.load32(address + 8 * i)});
    return true;
}

// an overlapped request's parts that are looked up before the lock is
// taken: hEvent's event (ignored with a completion routine, as Winsock
// ignores it) and the thread that asks; false if hEvent is no event
bool prepare(Kernel &k, uint32_t ov, uint32_t routine, Request &r) {
    r.overlapped = ov, r.routine = routine;
    if (Thread *t = current_thread()) r.thread = Ref<Thread>::retain(t);
    uint32_t handle = routine ? 0 : k.load32(ov + wsaov::EVENT);
    if (!handle) return true;
    r.event = objects(k).lookup<Event>(handle);
    return bool(r.event);
}

// WSARecv and WSARecvFrom: 0 and the bytes when a datagram was there,
// else SOCKET_ERROR (WSA_IO_PENDING for an overlapped request left pending)
void wsa_receive(Kernel &k, ppc::Context &c, uint32_t handle, uint32_t buffers_at, uint32_t count, uint32_t bytes_ptr,
                 uint32_t flags_ptr, uint32_t from, uint32_t from_length_ptr, uint32_t ov, uint32_t routine,
                 bool connected_only) {
    Ref<Socket> s = socket_by_handle(k, handle);
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    std::vector<Buffer> buffers;
    if (!load_buffers(k, buffers_at, count, buffers)) return fail(k, c, X_WSAEFAULT);
    if (connected_only && s->datagram()) {
        std::lock_guard<std::mutex> l(objects(k).lock);
        if (!s->connected) return fail(k, c, X_WSAENOTCONN);
    }
    uint32_t flags = flags_ptr ? k.load32(flags_ptr) : 0;
    Request r;
    if (ov && !prepare(k, ov, routine, r)) return fail(k, c, X_ERROR_INVALID_HANDLE);
    uint32_t error = 0;
    uint32_t n = receive(k, *s, buffers, flags, from, from_length_ptr, error, ov ? &r : nullptr);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    if (bytes_ptr) k.store32(bytes_ptr, n);
    if (flags_ptr) k.store32(flags_ptr, 0);
    result(c, 0);
}

// WSASend and WSASendTo: the buffers gathered into one datagram, sent at
// once (to, else the connected peer); an overlapped request ends with it
void wsa_send(Kernel &k, ppc::Context &c, uint32_t handle, uint32_t buffers_at, uint32_t count, uint32_t bytes_ptr,
              uint32_t to, uint32_t to_length, uint32_t ov, uint32_t routine) {
    Ref<Socket> s = socket_by_handle(k, handle);
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    std::vector<Buffer> buffers;
    if (!load_buffers(k, buffers_at, count, buffers)) return fail(k, c, X_WSAEFAULT);
    std::vector<uint8_t> data;
    data.reserve(total_length(buffers));
    for (const Buffer &b : buffers) data.insert(data.end(), k.host(b.address), k.host(b.address) + b.length);
    Request r;
    if (ov && !prepare(k, ov, routine, r)) return fail(k, c, X_ERROR_INVALID_HANDLE);
    Objects &o = objects(k);
    std::vector<Request> ended;
    std::lock_guard<std::mutex> l(o.lock);
    if (!s->datagram()) return fail(k, c, X_WSAENOTCONN);
    uint32_t address = s->peer_address;
    uint16_t port = s->peer_port;
    if (to) {
        if (to_length < SOCKADDR_IN_SIZE) return fail(k, c, X_WSAEFAULT);
        address = k.load32(to + 4), port = k.load16(to + 2);
    } else if (!s->connected) {
        return fail(k, c, X_WSAENOTCONN);
    }
    uint32_t error = 0;
    uint32_t n = send_datagram(k, *s, address, port, data.data(), uint32_t(data.size()), error, ended);
    if (n == SOCKET_ERROR) return fail(k, c, error);
    if (ov) end_request(k, o, *s, r, X_STATUS_SUCCESS, 0, n);
    if (bytes_ptr) k.store32(bytes_ptr, n);
    result(c, 0);
}

} // namespace

// (caller, socket, buffers, count, bytes_ptr, flags_ptr, overlapped, routine)
XF_EXPORT(xam, NetDll_WSARecv) {
    wsa_receive(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5), 0, 0, arg(k, c, 6),
                arg(k, c, 7), true);
}

// (caller, socket, buffers, count, bytes_ptr, flags_ptr, from, from_length_ptr,
// overlapped, routine)
XF_EXPORT(xam, NetDll_WSARecvFrom) {
    wsa_receive(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 5), arg(k, c, 6), arg(k, c, 7),
                arg(k, c, 8), arg(k, c, 9), false);
}

// (caller, socket, buffers, count, bytes_ptr, flags, overlapped, routine)
XF_EXPORT(xam, NetDll_WSASend) {
    wsa_send(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), 0, 0, arg(k, c, 6), arg(k, c, 7));
}

// (caller, socket, buffers, count, bytes_ptr, flags, to, to_length,
// overlapped, routine)
XF_EXPORT(xam, NetDll_WSASendTo) {
    wsa_send(k, c, arg(k, c, 1), arg(k, c, 2), arg(k, c, 3), arg(k, c, 4), arg(k, c, 6), arg(k, c, 7), arg(k, c, 8),
             arg(k, c, 9));
}

// (caller, socket, overlapped, bytes_ptr, wait, flags_ptr): TRUE and the
// bytes when the request succeeded; FALSE with WSA_IO_INCOMPLETE while it
// pends (waiting for its end first with wait), else with its error
XF_EXPORT(xam, NetDll_WSAGetOverlappedResult) {
    uint32_t ov = arg(k, c, 2), bytes_ptr = arg(k, c, 3), wait = arg(k, c, 4), flags_ptr = arg(k, c, 5);
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return set_last_error(k, X_WSAENOTSOCK), result(c, 0);
    if (!ov) return set_last_error(k, X_WSAEFAULT), result(c, 0);
    Objects &o = objects(k);
    std::unique_lock<std::mutex> l(o.lock);
    for (;;) {
        bool pending =
            std::any_of(s->pending.begin(), s->pending.end(), [&](const Request &r) { return r.overlapped == ov; });
        if (!pending && k.load32(ov + wsaov::INTERNAL) != X_STATUS_PENDING) break;
        Thread *t = current_thread();
        if (!wait || !pending || !t) {
            l.unlock();
            set_last_error(k, X_WSA_IO_INCOMPLETE);
            return result(c, 0);
        }
        // until a request on the socket ends; the reset is seen by no one
        // already woken, as the wake happens under the lock
        Ref<Event> progress = s->progress;
        progress->set_signal_state(0);
        l.unlock();
        Dispatcher *list[] = {progress.get()};
        o.wait(*t, list, false, false, Scheduler::NEVER);
        l.lock();
    }
    uint32_t status = k.load32(ov + wsaov::INTERNAL), bytes = k.load32(ov + wsaov::INTERNAL_HIGH);
    l.unlock();
    if (bytes_ptr) k.store32(bytes_ptr, bytes);
    if (flags_ptr) k.store32(flags_ptr, 0);
    if (status == X_STATUS_SUCCESS) return result(c, 1);
    set_last_error(k, status == STATUS_CANCELLED         ? X_WSA_OPERATION_ABORTED
                      : status == STATUS_BUFFER_OVERFLOW ? X_WSAEMSGSIZE
                                                         : X_WSAEINVAL);
    result(c, 0);
}

// (caller, socket): every overlapped request pending on it ends aborted
XF_EXPORT(xam, NetDll_WSACancelOverlappedIO) {
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    std::vector<Request> ended;
    std::lock_guard<std::mutex> l(o.lock);
    abort_requests(k, o, *s, ended);
    result(c, 0);
}

// (caller, socket, event, network_events): the socket non-blocking, the
// event set for the events asked as they happen (FD_READ, FD_WRITE; see the
// top); no events and no event undo it
XF_EXPORT(xam, NetDll_WSAEventSelect) {
    uint32_t handle = arg(k, c, 2), mask = arg(k, c, 3) & FD_ALL_EVENTS;
    Ref<Socket> s = socket_by_handle(k, arg(k, c, 1));
    if (!s) return fail(k, c, X_WSAENOTSOCK);
    Objects &o = objects(k);
    Ref<Event> e = handle ? o.lookup<Event>(handle) : Ref<Event>();
    if (mask && !e) return fail(k, c, X_WSAEINVAL);
    Ref<Event> previous;
    std::lock_guard<std::mutex> l(o.lock);
    previous = std::move(s->select_event);
    s->nonblocking = true;
    s->select_mask = mask;
    if (mask) s->select_event = e;
    if (mask) {
        bool readable = (mask & FD_READ) && !s->queue.empty();
        bool writable = (mask & FD_WRITE) && (s->datagram() || s->connected);
        if (readable || writable) set_event(o, *e);
    }
    result(c, 0);
}

} // namespace xf::kernel
