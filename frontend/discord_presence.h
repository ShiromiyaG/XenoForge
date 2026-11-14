// Discord Rich Presence (docs/runtime.md, "Discord Rich Presence"): "Playing <title>" with the time since the game
// started, shown on the player's Discord profile. It speaks Discord's local IPC protocol directly (a named pipe on
// Windows, a Unix socket elsewhere; after the MIT-licensed discord-rpc library's design), so there is no SDK to
// build. The work is on a thread of its own and never blocks the game: with Discord not running it retries quietly.
// Nothing but the title and the start time is sent (no title ID, no account, no path).
//
// The encoders are plain functions, apart from the transport, so a test can check the frames and the JSON.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace xf::frontend::discord {

// IPC frame opcodes
enum Opcode : uint32_t { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

// the string as a JSON string literal, quotes included: quotes, backslashes and control characters escaped, UTF-8
// kept as it is (bytes that are not valid UTF-8 become U+FFFD), cut at `max_bytes` on a character boundary when
// it is non-zero
std::string json_string(const std::string &text, size_t max_bytes = 0);

// a frame: the opcode and the JSON's length as little-endian 32-bit words, then the JSON
std::string encode_frame(uint32_t opcode, const std::string &json);

// {"v":1,"client_id":"..."}
std::string handshake_json(const std::string &client_id);

// SET_ACTIVITY with the title as `details`, `state`, the start time (seconds since the Unix epoch) and the large
// image (an art asset's key, or a picture's http(s) address) with its hover text; `nonce` pairs the reply with it
std::string activity_json(int pid, const std::string &title, const std::string &state, int64_t start_seconds,
                          const std::string &nonce, const std::string &image = "xenoforge",
                          const std::string &image_text = "XenoForge");

// The game's art on Xbox's own catalog of title images, by its title ID ("4D5300AB" style, 8 hex digits), for the
// large image while a game runs; "" for an ID that is not one
std::string title_art_url(const std::string &title_id);

// SET_ACTIVITY without an activity: clears the presence
std::string clear_json(int pid, const std::string &nonce);

// The presence of the running game. start() launches the thread (nothing when `client_id` is empty, or on Android,
// which has no local Discord IPC); stop() clears the activity, says goodbye and joins it.
class Presence {
public:
    Presence();
    ~Presence();
    // false: nothing started. `state` is the second line; `image` the large image (a key or an address)
    bool start(const std::string &client_id, const std::string &title, const std::string &state = "XenoForge",
               const std::string &image = "xenoforge", const std::string &image_text = "XenoForge");
    void stop();
    // for a check: whether the thread has its activity sent to a running Discord
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xf::frontend::discord
