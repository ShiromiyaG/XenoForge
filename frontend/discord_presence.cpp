// Discord Rich Presence (discord_presence.h). The protocol, as discord-rpc (MIT, Discord Inc.) speaks it: connect to
// discord-ipc-0..9, send a HANDSHAKE frame, read the READY reply, then SET_ACTIVITY frames; answer PING with PONG,
// reconnect on CLOSE; clear the activity and send CLOSE at the end.

#include "discord_presence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif !defined(__ANDROID__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define XF_DISCORD_UNIX 1
#endif

namespace xf::frontend::discord {

std::string json_string(const std::string &text, size_t max_bytes) {
    static const char *hex = "0123456789abcdef";
    std::string out = "\"";
    size_t i = 0, used = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        // the length of the character at i: 0 if it is not valid UTF-8
        size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        if (n > 1) {
            if (i + n > text.size()) n = 0;
            for (size_t k = 1; n && k < n; k++)
                if (((unsigned char)text[i + k] & 0xC0) != 0x80) n = 0;
            // overlong forms, surrogates and values past U+10FFFF
            if (n == 3 && c == 0xE0 && (unsigned char)text[i + 1] < 0xA0) n = 0;
            if (n == 3 && c == 0xED && (unsigned char)text[i + 1] >= 0xA0) n = 0;
            if (n == 4 && c == 0xF0 && (unsigned char)text[i + 1] < 0x90) n = 0;
            if (n == 4 && c == 0xF4 && (unsigned char)text[i + 1] >= 0x90) n = 0;
        }
        const size_t take = n ? n : 1;
        if (max_bytes && used + (n ? n : 3) > max_bytes) break;
        if (!n) {
            out += "\xEF\xBF\xBD";
            used += 3;
        } else if (n == 1 && (c == '"' || c == '\\')) {
            out += '\\';
            out += char(c);
            used += 1;
        } else if (n == 1 && c < 0x20) {
            switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default: out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; break;
            }
            used += 1;
        } else {
            out.append(text, i, n);
            used += n;
        }
        i += take;
    }
    return out + "\"";
}

std::string encode_frame(uint32_t opcode, const std::string &json) {
    const uint32_t length = uint32_t(json.size());
    std::string frame(8, '\0');
    for (int i = 0; i < 4; i++) {
        frame[size_t(i)] = char(opcode >> (8 * i));
        frame[size_t(4 + i)] = char(length >> (8 * i));
    }
    return frame + json;
}

std::string handshake_json(const std::string &client_id) {
    return "{\"v\":1,\"client_id\":" + json_string(client_id) + "}";
}

std::string activity_json(int pid, const std::string &title, const std::string &state, int64_t start_seconds,
                          const std::string &nonce, const std::string &image, const std::string &image_text,
                          const std::string &name) {
    // details and state are 2 to 128 characters for Discord; a shorter text is padded by a space
    auto field = [](const std::string &s) { return json_string(s.size() < 2 ? s + " " : s, 128); };
    // the activity's name in place of the application's ("Playing <name>"), where one is given
    const std::string named = name.empty() ? std::string() : "\"name\":" + field(name) + ",";
    // the image: an art asset's key, or a picture's address (Discord fetches it through its media proxy)
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":{" + named +
           "\"details\":" + field(title) + (state.empty() ? std::string() : ",\"state\":" + field(state)) +
           ",\"timestamps\":{\"start\":" + std::to_string(start_seconds) +
           "},\"assets\":{\"large_image\":" + json_string(image, 256) + ",\"large_text\":" + field(image_text) +
           "}}},\"nonce\":" + json_string(nonce) + "}";
}

std::string title_art_url(const std::string &title_id) {
    if (title_id.size() != 8) return "";
    std::string id;
    for (char c : title_id) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return "";
        id += char(std::tolower(static_cast<unsigned char>(c)));
    }
    // the Xbox 360 Marketplace's box art, by the title's ID in the last 8 digits of its product ID; served over
    // http only (its https certificate does not match), which Discord's media proxy takes
    return "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802" + id + "/1033/boxartlg.jpg";
}

std::string clear_json(int pid, const std::string &nonce) {
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + "},\"nonce\":" + json_string(nonce) + "}";
}

namespace {

using Ms = std::chrono::milliseconds;

// One connection to Discord's local IPC.
class Transport {
public:
    ~Transport() { close(); }
    bool open(); // tries discord-ipc-0..9
    void close();
    bool is_open() const;
    bool write(const std::string &data);
    // up to n bytes within the time: the count, 0 for none yet, -1 when the connection is gone
    long read(char *into, size_t n, int timeout_ms);

private:
#if defined(_WIN32)
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#elif defined(XF_DISCORD_UNIX)
    int fd_ = -1;
#endif
};

#if defined(_WIN32)
bool Transport::open() {
    for (int i = 0; i < 10; i++) {
        const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipe_ = h;
            return true;
        }
    }
    return false;
}
void Transport::close() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
}
bool Transport::is_open() const { return pipe_ != INVALID_HANDLE_VALUE; }
bool Transport::write(const std::string &data) {
    DWORD done = 0;
    return WriteFile(pipe_, data.data(), DWORD(data.size()), &done, nullptr) && done == data.size();
}
long Transport::read(char *into, size_t n, int timeout_ms) {
    auto until = std::chrono::steady_clock::now() + Ms(timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(pipe_, into, DWORD(std::min<size_t>(n, avail)), &got, nullptr)) return -1;
            return long(got);
        }
        if (std::chrono::steady_clock::now() >= until) return 0;
        std::this_thread::sleep_for(Ms(10));
    }
}
#elif defined(XF_DISCORD_UNIX)
bool Transport::open() {
    // the directories Discord's clients look in, in discord-rpc's order
    const char *dirs[] = {std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"), std::getenv("TMP"), std::getenv("TEMP"),
                          "/tmp"};
    for (const char *dir : dirs) {
        if (!dir || !*dir) continue;
        for (int i = 0; i < 10; i++) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::string path = std::string(dir) + "/discord-ipc-" + std::to_string(i);
            if (path.size() >= sizeof(addr.sun_path)) continue;
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
                fd_ = fd;
                return true;
            }
            ::close(fd);
        }
    }
    return false;
}
void Transport::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}
bool Transport::is_open() const { return fd_ >= 0; }
bool Transport::write(const std::string &data) {
    size_t at = 0;
    while (at < data.size()) {
#if defined(MSG_NOSIGNAL)
        ssize_t n = ::send(fd_, data.data() + at, data.size() - at, MSG_NOSIGNAL);
#else
        ssize_t n = ::send(fd_, data.data() + at, data.size() - at, 0);
#endif
        if (n <= 0) return false;
        at += size_t(n);
    }
    return true;
}
long Transport::read(char *into, size_t n, int timeout_ms) {
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    ssize_t got = ::recv(fd_, into, n, 0);
    return got <= 0 ? -1 : long(got);
}
#else // Android: Discord has no local IPC there
bool Transport::open() { return false; }
void Transport::close() {}
bool Transport::is_open() const { return false; }
bool Transport::write(const std::string &) { return false; }
long Transport::read(char *, size_t, int) { return -1; }
#endif

int process_id() {
#if defined(_WIN32)
    return int(GetCurrentProcessId());
#elif defined(XF_DISCORD_UNIX)
    return int(getpid());
#else
    return 0;
#endif
}

} // namespace

struct Presence::Impl {
    std::string client_id, title, state, image, image_text, name;
    int64_t started = 0;
    // what was last said in the log (one line a change, not one a retry): 0 nothing, 1 set, 2 not running,
    // 3 no answer
    int said = 0;
    void say(int what, const char *text) {
        if (said == what) return;
        said = what;
        std::printf("discord: %s\n", text);
        std::fflush(stdout);
    }
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::atomic<bool> ready{false};
    Transport link;
    int nonce = 0;

    bool read_exact(char *into, size_t n, int timeout_ms) {
        size_t at = 0;
        while (at < n) {
            long got = link.read(into + at, n - at, timeout_ms);
            if (got <= 0) return false;
            at += size_t(got);
        }
        return true;
    }
    // the rest of a frame whose first header byte is in head[0]; false when it breaks
    bool read_frame_after_first(unsigned char *head, uint32_t &opcode, std::string &body) {
        if (!read_exact((char *)head + 1, 7, 2000)) return false;
        opcode = head[0] | head[1] << 8 | head[2] << 16 | uint32_t(head[3]) << 24;
        uint32_t length = head[4] | head[5] << 8 | head[6] << 16 | uint32_t(head[7]) << 24;
        if (length > (1u << 20)) return false;
        body.assign(length, '\0');
        return length == 0 || read_exact(body.data(), length, 2000);
    }
    std::string next_nonce() { return std::to_string(++nonce); }
    bool stop_requested() {
        std::lock_guard lock(mutex);
        return stopping;
    }
    void drop() {
        link.close();
        ready = false;
    }
    // sleeps up to `seconds`, less when asked to stop
    void pause(int seconds = 15) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping; });
    }

    // connect, handshake and set the activity; false when Discord is not there or does not answer. A Discord that
    // is there but slow (it answers late while it takes in another client's activity: titles opened and closed in a
    // row, the launcher stepping aside) is tried again soon, 2, 4 then 8 s later, before the 15 s of one not running.
    int unanswered = 0;
    int connect() {
        if (!link.open()) {
            say(2, "Discord is not running (tried again every 15 s)");
            return 15;
        }
        uint32_t op = 0;
        std::string body;
        unsigned char head[8];
        if (!link.write(encode_frame(OP_HANDSHAKE, handshake_json(client_id))) || link.read((char *)head, 1, 8000) <= 0 ||
            !read_frame_after_first(head, op, body) || op != OP_FRAME ||
            !link.write(encode_frame(OP_FRAME, activity_json(process_id(), title, state, started, next_nonce(), image,
                                                             image_text, name)))) {
            link.close();
            say(3, "Discord did not answer the handshake (tried again shortly)");
            const int wait = unanswered < 3 ? 2 << unanswered : 15;
            unanswered++;
            return wait;
        }
        say(1, "the activity is set");
        unanswered = 0;
        ready = true;
        return 0;
    }

    void run() {
        while (!stop_requested()) {
            if (!link.is_open()) {
                if (const int wait = connect()) {
                    pause(wait); // not running (or not answering): try again later, quietly
                    continue;
                }
            }
            // connected: answer what Discord sends (its replies, PING, CLOSE) while waiting
            unsigned char head[8];
            long got = link.read((char *)head, 1, 500);
            if (got < 0) {
                drop();
                continue;
            }
            if (got == 0) continue;
            uint32_t op = 0;
            std::string body;
            if (!read_frame_after_first(head, op, body)) {
                drop();
                continue;
            }
            if (op == OP_PING) {
                if (!link.write(encode_frame(OP_PONG, body))) drop();
            } else if (op == OP_CLOSE) {
                drop();
                pause();
            }
        }
        if (link.is_open()) {
            // the activity cleared, then the goodbye
            link.write(encode_frame(OP_FRAME, clear_json(process_id(), next_nonce())));
            link.write(encode_frame(OP_CLOSE, "{}"));
            link.close();
        }
        ready = false;
    }
};

Presence::Presence() = default;
Presence::~Presence() { stop(); }

bool Presence::start(const std::string &client_id, const std::string &title, const std::string &state,
                     const std::string &image, const std::string &image_text, const std::string &name) {
#if defined(__ANDROID__)
    (void)client_id, (void)title, (void)state, (void)image, (void)image_text, (void)name;
    return false;
#else
    if (client_id.empty() || impl_) return false;
    impl_ = std::make_unique<Impl>();
    impl_->client_id = client_id;
    impl_->title = title;
    impl_->state = state;
    impl_->image = image;
    impl_->image_text = image_text;
    impl_->name = name;
    impl_->started =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
    return true;
#endif
}

void Presence::stop() {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_.reset();
}

bool Presence::connected() const { return impl_ && impl_->ready; }

} // namespace xf::frontend::discord
