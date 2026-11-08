// Input's seam (plan step A6; docs/input.md): what answers XAM's input
// exports. The kernel's input system (src/kernel/xam_input.cpp, Xenia's
// InputSystem) asks its drivers in order, as Xenia's does: the first one that
// answers for a user is that user's controller. With no driver every user is
// disconnected (Xenia's nop driver). The player adds drivers for SDL's
// gamepads and the keyboard; boot adds an idle pad (a controller for user 0
// with nothing pressed) unless told not to.
//
// The structures are Xenia's (hid/input.h) in host order; the kernel writes
// them big-endian into the guest's.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace xf::kernel {
class Kernel;
}

namespace xf::input {

constexpr uint32_t USERS = 4;

// the results the exports pass on (Win32 errors, Xenia's xbox.h)
constexpr uint32_t SUCCESS = 0x0, BAD_ARGUMENTS = 0xA0, DEVICE_NOT_CONNECTED = 0x48F, EMPTY = 0x10D2;
constexpr uint32_t FUNCTION_FAILED = 0x65B;

// X_INPUT_GAMEPAD's buttons
enum Button : uint16_t {
    DPAD_UP = 0x0001,
    DPAD_DOWN = 0x0002,
    DPAD_LEFT = 0x0004,
    DPAD_RIGHT = 0x0008,
    START = 0x0010,
    BACK = 0x0020,
    LEFT_THUMB = 0x0040,
    RIGHT_THUMB = 0x0080,
    LEFT_SHOULDER = 0x0100,
    RIGHT_SHOULDER = 0x0200,
    GUIDE = 0x0400,
    A = 0x1000,
    B = 0x2000,
    X = 0x4000,
    Y = 0x8000,
};

// X_INPUT_FLAG: which kind of device a call asks for
constexpr uint32_t FLAG_GAMEPAD = 1, FLAG_KEYBOARD = 2, FLAG_ANY_USER = 1u << 30;

// X_INPUT_KEYSTROKE's flags
constexpr uint16_t KEYSTROKE_KEYDOWN = 1, KEYSTROKE_KEYUP = 2, KEYSTROKE_REPEAT = 4;

// the pad's virtual keys (Xenia's ui::VirtualKey kXInputPad*, the XDK's VK_PAD_*)
namespace vk {
constexpr uint16_t A = 0x5800, B = 0x5801, X = 0x5802, Y = 0x5803, RSHOULDER = 0x5804, LSHOULDER = 0x5805;
constexpr uint16_t LTRIGGER = 0x5806, RTRIGGER = 0x5807;
constexpr uint16_t DPAD_UP = 0x5810, DPAD_DOWN = 0x5811, DPAD_LEFT = 0x5812, DPAD_RIGHT = 0x5813;
constexpr uint16_t START = 0x5814, BACK = 0x5815, LTHUMB_PRESS = 0x5816, RTHUMB_PRESS = 0x5817;
constexpr uint16_t LTHUMB_UP = 0x5820, LTHUMB_DOWN = 0x5821, LTHUMB_RIGHT = 0x5822, LTHUMB_LEFT = 0x5823;
constexpr uint16_t LTHUMB_UPLEFT = 0x5824, LTHUMB_UPRIGHT = 0x5825, LTHUMB_DOWNRIGHT = 0x5826, LTHUMB_DOWNLEFT = 0x5827;
constexpr uint16_t RTHUMB_UP = 0x5830, RTHUMB_DOWN = 0x5831, RTHUMB_RIGHT = 0x5832, RTHUMB_LEFT = 0x5833;
constexpr uint16_t RTHUMB_UPLEFT = 0x5834, RTHUMB_UPRIGHT = 0x5835, RTHUMB_DOWNRIGHT = 0x5836, RTHUMB_DOWNLEFT = 0x5837;
constexpr uint16_t GUIDE = 0x5838;
} // namespace vk

struct Gamepad {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
    bool operator==(const Gamepad &) const = default;
};

struct State {
    uint32_t packet_number = 0;
    Gamepad gamepad;
};

struct Vibration {
    uint16_t left_motor_speed = 0, right_motor_speed = 0;
};

// X_INPUT_CAPABILITIES: type 1 a gamepad, sub_type 1 a standard one; the
// gamepad's fields the controls it has (all bits, full ranges)
struct Capabilities {
    uint8_t type = 0, sub_type = 0;
    uint16_t flags = 0;
    Gamepad gamepad;
    Vibration vibration;
};

struct Keystroke {
    uint16_t virtual_key = 0, unicode = 0, flags = 0;
    uint8_t user_index = 0, hid_code = 0;
};

// Xenia's InputType: what a driver answers for (the calls' flags filter on it)
enum class Type { None = 0, Controller = 1, Keyboard = 2 };

// Xenia's InputDriver. The calls come on guest threads, one at a time (the
// input system's lock); now_ms is the scheduler's clock (milliseconds since
// the kernel started), the one the keystrokes' repeats and scripted presses
// run on.
class Driver {
  public:
    virtual ~Driver() = default;
    virtual Type type() const { return Type::Controller; }
    virtual uint32_t capabilities(uint32_t user, uint32_t flags, Capabilities &out) = 0;
    virtual uint32_t state(uint32_t user, uint64_t now_ms, State &out) = 0;
    virtual uint32_t vibrate(uint32_t user, const Vibration &vibration) = 0;
    virtual uint32_t keystroke(uint32_t user, uint32_t flags, uint64_t now_ms, Keystroke &out) = 0;
};

// The capabilities Xenia's SDL driver reports for a standard gamepad: every
// button but the guide's unknown bit, full triggers and sticks, both motors.
Capabilities gamepad_capabilities();

// Xenia's SDL driver's keystrokes for one user (GetKeystroke): the buttons,
// the triggers past 0x1F and the sticks past 0x4E00 (eight directions each)
// made key downs and ups, the ups first, in the driver's order; a key held
// repeats after 400 ms every 100 ms. EMPTY when nothing changed.
class PadKeystrokes {
  public:
    uint32_t next(const Gamepad &pad, uint32_t user, uint64_t now_ms, Keystroke &out);
    void reset() { *this = {}; }

  private:
    uint64_t buttons_ = 0;
    enum class Repeat { Idle, Waiting, Repeating } repeat_ = Repeat::Idle;
    uint8_t repeat_index_ = 0;
    uint64_t repeat_time_ = 0;
};

// A packet number that moves when the state does (Xenia's SDL driver: from
// 1, the first read counting as a change, as XInput starts).
class Packets {
  public:
    uint32_t next(const Gamepad &pad) {
        if (first_ || !(pad == last_)) packet_++, last_ = pad, first_ = false;
        return packet_;
    }

  private:
    Gamepad last_;
    uint32_t packet_ = 0;
    bool first_ = true;
};

// What is pressed on a user's controller at a time besides the device's own
// input (scripted presses, boot's and the player's --press): ORed into its
// buttons, the triggers and sticks taken where they move further.
using Overlay = std::function<Gamepad(uint32_t user, uint64_t now_ms)>;
Gamepad combine(const Gamepad &a, const Gamepad &b);

// A controller for user 0 with nothing pressed but what `overlay` gives, as
// a pad plugged in and left alone: boot's default, so a game that waits for
// a controller sees one.
std::unique_ptr<Driver> make_idle_pad(Overlay overlay = nullptr);

} // namespace xf::input

namespace xf::kernel {

// Set before the game runs: a driver, asked after those added before it
// (Xenia's driver order); the sticks' deadzones, 0 to 1 of their range
// (Xenia's left/right_stick_deadzone_percentage; 0, none, by default).
void add_input_driver(Kernel &k, std::unique_ptr<input::Driver> driver);
void set_input_deadzones(Kernel &k, double left, double right);
// what the input exports answered: states delivered and keystrokes, by
// user, for boot and the player's exit report
std::string input_report(Kernel &k);
uint64_t input_states_delivered(Kernel &k);
// Recordings (docs/input.md, "Recordings"), set before the game runs: every
// answer the input exports give written to path as it changes, at its time on
// the scheduler's clock; or a recording's answers given in place of the
// drivers'. In the deterministic mode a replay repeats the run it was
// recorded in exactly. False, and why, when the file cannot be used.
bool record_input(Kernel &k, const std::filesystem::path &path, std::string &error);
bool replay_input(Kernel &k, const std::filesystem::path &path, std::string &error);

} // namespace xf::kernel
