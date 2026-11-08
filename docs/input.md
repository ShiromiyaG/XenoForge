# Input (2026-11-05)

How a controller reaches the game: XAM's input exports, answered as Xenia
answers them (`kernel/xam/xam_input.cc`, `hid/input_system.cc`, the SDL
and keyboard drivers in `hid/`), from drivers the player or boot installs.

## The kernel's side

`xenoforge/input.h` is the seam; `src/kernel/xam_input.cpp` the exports
and the input system behind them (Xenia's InputSystem):

- **Drivers in order.** `kernel::add_input_driver` appends one; a call
  asks them in that order and the first that answers for the user is that
  user's controller. A driver says what it is (a controller, a keyboard
  in Xenia's passthrough sense, or nothing), and the calls' flags filter
  on it: `XINPUT_FLAG_GAMEPAD` (1) asks for controllers, `KEYBOARD` (2) for
  keyboards, no flags means the gamepad for a state and both for the
  capabilities. With no driver every user is disconnected (Xenia's nop
  driver).
- **The exports**: XamInputGetState (and its packet numbers), SetState
  (vibration), GetCapabilities and GetCapabilitiesEx, GetKeystroke and
  GetKeystrokeEx, XamEnableInactivityProcessing and XamResetInactivity.
  "Any user" (0xFF) is user 0, as in Xenia. A user going from disconnected
  to connected or back sends `XN_SYS_INPUTDEVICESCHANGED`.
- **Deadzones** (`kernel::set_input_deadzones`, the player's
  `input.left_deadzone`/`right_deadzone`): Xenia's shape (each axis's
  zone scaled by the stick's angle), over the stick's full range; off by
  default, as Xenia's.
- **Keystrokes** from a pad are Xenia's SDL driver's (`input::PadKeystrokes`):
  the buttons, the triggers past 0x1F and the sticks past 0x4E00 in eight
  directions become `VK_PAD_*` key downs and ups (ups first, so a stick
  going from up-left to left lets up-left go before left goes down),
  repeated after 400 ms every 100 ms, on the scheduler's clock.
- `input_report` (boot's and the player's report) counts the states
  delivered by user, the calls answered "not connected", keystrokes,
  vibrations and connection changes.

The games: both call XamInputGetCapabilities, GetState and SetState for
users 0-3 every frame; one (game B) also polls XamInputGetKeystrokeEx with
`ANY_USER | KEYBOARD` (0x40000002), which only a keyboard in Xenia's
passthrough mode answers, so it gets "not connected" here as in Xenia
with its default drivers.

## boot

Boot installs an **idle pad**: a controller for user 0, nothing pressed
(`input::make_idle_pad`). Xenia's default drivers answer nothing without a
pad plugged in, and game B then pauses on "Controller Lost Connection";
with the idle pad it plays its intro. `--no-controller` leaves every user
disconnected, as Xenia without a pad.

`--press <buttons>@<seconds>[:<hold>]` (repeatable) holds buttons on user
0's controller from a time on the scheduler's clock (0.3 s unless a hold
is given): `start@40`, `a+lt@12.5s:1`; or from the GPU's Nth swap,
`start@#1100`, which follows what the game shows whatever the run's speed
(though a movie's frames run on the clock, so the swap a screen comes at
still varies). Buttons: `a b x y start back guide lb rb ls rs up down left
right lt rt`. The packet number moves with the press, as a real pad's
does. The player takes the same option, and prints when each screenshot's
swap came on the scheduler's clock, to choose the times from.

## Recordings

`kernel::record_input(k, path)` writes what the input exports answer, and
`kernel::replay_input(k, path)` answers from such a file in place of the
drivers (boot's `--record-input`/`--replay-input`, `XF_INPUT_RECORD` and
`XF_INPUT_REPLAY` for boot and the player). The file is text, a line each
time a user's answer changes, at the time it was asked on the scheduler's
clock (100 ns):

    s <time> <user> <result> <packet> <buttons> <lt> <rt> <lx> <ly> <rx> <ry>   XamInputGetState
    k <time> <user> <result> <virtual key> <unicode> <flags> <hid code>          a keystroke
    c <time> <user> <result>                                                     the capabilities

Results, packets, buttons and keys in hex. Every keystroke delivered is a
line; a state, the capabilities and a keystroke call's other answers
(empty, not connected) only when they change. A replay answers a state or
the capabilities with the last line at or before the time asked (the first
before it), deadzones and packet numbers as recorded, and the keystrokes in
order once their time has come. In the deterministic mode (runtime.md) the
polls come at the same times, so a replay repeats the recorded run exactly;
recorded while playing, it is close (the times are the host's).
Vibration is not replayed.

## The player

`frontend/sdl_input.cpp`, two drivers in Xenia's order:

- **SDL3's gamepads** (Xenia's SDL driver): a pad goes to the slot of its
  player index if free, else the first free one (and its index is set to
  it); its axes and buttons as Xenia maps them (the Y axes inverted, the
  triggers' 0-32767 to 0-255, the paddles as Y, B, X, A, the misc button
  and the touchpad as the guide); the packet number moves when the state
  does; the capabilities say wireless and missing navigation buttons as
  SDL reports them, the subtype from SDL's joystick type; vibration through
  SDL's rumble (`input.vibration = false` stops the motors).
- **The keyboard as a controller** (Xenia's keyboard driver in mode 1)
  for `input.keyboard_user` (0), on by default (`input.keyboard`; Xenia's
  default is off). Asked after the gamepads, so a pad in the same slot
  answers first. Its bindings are Xenia's (`keyboard_binding_table.inc`),
  in config.toml's `[keyboard]` as SDL key names, space-separated, `^` for
  a key with Shift (or Caps Lock) and `_` without:

  | control | keys | control | keys |
  | --- | --- | --- | --- |
  | a | `;` | dpad_up/down/left/right | `^W` `^S` `^A` `^D` |
  | b | `'` | left_thumb_up/down/left/right | `_W` `_S` `_A` `_D` |
  | x | `L` | right_thumb_up/down/left/right | `Up` `Down` `Left` `Right` |
  | y | `P` | left_thumb, right_thumb (clicks) | `F`, `K` |
  | start | `X` | left_trigger | `Q I` |
  | back | `Z` | right_trigger | `E O` |
  | guide | `Backspace` | left_shoulder, right_shoulder | `1`, `3` |

  A stick's opposite keys add up, as in Xenia; the packet number moves
  with every call, as Xenia's keyboard driver counts them.

Differences from Xenia's keyboard driver: the state is read from SDL's
key state each turn of the window's loop (Xenia follows key events and can
leave a binding held when Shift changes under a held key); its keystrokes
are the pad's (downs, ups, repeats), where Xenia's sends only a down for
each key event; the overlay open (F1) takes the keyboard from the game.
Guitars and their whammy options are left out.
