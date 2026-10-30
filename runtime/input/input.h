// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Host input for the windowed front end (SDL3): keyboard and gamepads mapped to the Amiga joystick ports and
// keyboard.
//
// - Joystick port 2 (JOY1DAT + CIA-A PRA bit 7) is the one the game reads ($20DE in the VBL handler). Its
//   two-player mode alternates players on that same joystick. Port 1 (JOY0DAT + PRA bit 6) is wired too.
// - Keys bound to a joystick or host action are consumed; every other key is sent to the game as an Amiga raw
//   keycode through the CIA-A serial port (the game's handler at $0FAC). The game's own keys: P = pause,
//   ESC = abort to menu, Space = $40 (handled at $797A). Space is therefore never bound.
// - Gamepads are hot-pluggable. The first one connected drives port 2, the second port 1: d-pad or left stick
//   for directions, any face button for fire.
// - Bindings are loaded from input.cfg in SDL_GetPrefPath("xenon2-recomp", "x2recomp"), written with the
//   defaults if missing. Format: one `action = Key Name, Key Name` per line (SDL scancode names), # comments.
// - macOS: the defaults bind no Ctrl key, because Ctrl + arrow keys are system-wide shortcuts there (Spaces,
//   Mission Control, App Expose); fire is Z, X or either Option key. The F-keys stay bound but the OS claims
//   most of them unless "Use F1, F2 etc. as standard function keys" is on, so every host action also has a
//   Cmd combination (host_command_for_gui()).
#pragma once
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace x2 {
class Machine;
}

namespace x2::input {

enum Action {
    P2Up, P2Down, P2Left, P2Right, P2Fire,
    P1Up, P1Down, P1Left, P1Right, P1Fire,
    HostPause, HostReset, HostQuit, HostFullscreen,
    HostVolumeDown, HostVolumeUp, HostScaleDown, HostScaleUp,
    HostFaithful, HostFilter, HostInterp, HostAspect,
    HostUpgrade, HostWide, HostPanel,
    HostMusic, // music source: amiga -> cdtv -> mix (CDTV CD audio)
    HostGrab,  // keyboard grab on/off
    kActionCount
};

// The joystick actions come first: P2Up..P1Fire.
constexpr int kJoystickActionCount = HostPause;

const char* action_name(int action);

struct Bindings {
    std::array<std::vector<SDL_Scancode>, kActionCount> keys;
};

// The defaults for this platform (macOS avoids the system-shortcut keys; see the file comment).
Bindings default_bindings();
// The defaults every release up to and including 0.1.0 wrote into input.cfg. Only used to recognise an
// untouched file so it can be upgraded (classify_bindings_file).
Bindings legacy_default_bindings();

// Parses config text into `b` (actions not mentioned keep their current keys). Returns false and sets `error`
// for an unknown action or key name; the valid lines are still applied.
bool parse_bindings(const std::string& text, Bindings& b, std::string& error);
std::string format_bindings(const Bindings& b);

// input.cfg format version. Files written before the macOS-safe defaults carry no version marker.
constexpr int kBindingsVersion = 2;
// Version marker a file declares, 1 when it has none.
int bindings_version(const std::string& text);

enum class CfgAction {
    Keep,   // the current version, or bindings the user changed: use the file as it is
    Upgrade // an old file holding nothing but old defaults: replace it with the new ones
};
CfgAction classify_bindings_file(const std::string& text);

struct LoadedBindings {
    Bindings bindings;
    std::string path;  // the file used, empty without a per-user data directory
    std::string error; // parse problems, one per line
    std::string note;  // one line about the file for the log, empty when there is nothing to say
};
// Loads <dir>input.cfg (dir: the per-user data directory, ending in a separator). Writes the defaults when
// there is no file, and upgrades an old file that holds only old defaults (keeping it as input.cfg.bak).
// A file the user has changed is never rewritten.
LoadedBindings load_bindings(const std::string& dir);

// The binding table as the --list-keys output: every action, its keys, and the built-in combinations.
std::string describe_bindings(const Bindings& b);

// Amiga raw keycode for a host key, or -1 if there is none.
int amiga_rawkey(SDL_Scancode sc);

enum class HostCommand {
    None, Pause, Reset, Quit, Fullscreen, VolumeDown, VolumeUp, ScaleDown, ScaleUp,
    Faithful, Filter, Interp, Aspect, // picture options (Faithful switch, next filter, interpolation, 4:3)
    Upgrade, Wide, Panel,             // asset packs on/off, next surround width, HD status panel on/off
    Music,                            // next music source (amiga, cdtv, mix)
    Grab                              // keyboard grab on/off
};

// Host command for Cmd+<key> (macOS) / Super+<key>, or None. Cmd+1..9 mirror F1..F9, Cmd+F the fullscreen
// toggle and Cmd+R the reset; Cmd+G toggles the keyboard grab. Cmd+Q, Cmd+H, Cmd+M, Cmd+W and Ctrl+Cmd+F
// never arrive here: SDL's application menu claims them (Cmd+Q arrives as SDL_EVENT_QUIT).
HostCommand host_command_for_gui(SDL_Scancode sc);

struct JoyState {
    bool up = false, down = false, left = false, right = false, fire = false;
};

// Should the keyboard be grabbed right now? Pure logic so it can be tested without a window.
//
// Grabbing asks the platform to route the system's own keyboard shortcuts to the window instead. It works on
// Windows and X11/Wayland; on macOS SDL's Cocoa backend only disables the global hotkeys when SDL itself was
// compiled with SDL_MAC_NO_SANDBOX, which neither Homebrew's nor our vendored build defines, so there it
// changes nothing (and always reports success). The default is therefore off on every platform and the safe
// default bindings are what keeps the system shortcuts out of the way; `on` is for people whose desktop does
// honour the grab.
class GrabPolicy {
public:
    enum class Mode { Auto, On, Off };
    static bool parse_mode(const std::string& name, Mode& out);
    static const char* mode_name(Mode m);

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    void set_focused(bool v) { focused_ = v; }
    void set_paused(bool v) { paused_ = v; }
    // Auto resolves to this; the caller passes the platform default (currently off everywhere).
    void set_auto_default(bool on) { auto_default_ = on; }
    bool want_grab() const;

private:
    Mode mode_ = Mode::Auto;
    bool focused_ = false, paused_ = false, auto_default_ = false;
};

class InputManager {
public:
    explicit InputManager(Bindings b) : bindings_(std::move(b)) {}
    ~InputManager();
    // Feed every SDL event. Returns a host command triggered by this event, if any.
    HostCommand handle_event(const SDL_Event& e);
    // Joystick state from keys and gamepads, and pending Amiga key events, applied to the machine. Call once
    // per emulated frame. A key or gamepad button tapped and released since the last call is still delivered
    // for this one frame, so a tap between two frames is never lost.
    void apply(Machine& m);
    JoyState port(int amiga_port) const; // 1 or 2; what is held now, without the tap latch
    size_t gamepads() const { return pads_.size(); }
    bool focused() const { return focused_; }
    // --input-debug: log every key, focus change and frame to stdout.
    void set_debug(bool on) { debug_ = on; }
    void log_grab(bool on, const char* why);

private:
    bool action_held(int action) const;
    int bound_action(SDL_Scancode sc) const; // the action this key is bound to, or -1
    // Focus change: drop the held keys and release the ones the game was told about. drop_taps also discards
    // a press that has not reached a frame yet.
    void forget_held(const char* why, bool drop_taps);
    Bindings bindings_;
    std::vector<SDL_Scancode> held_;
    std::array<bool, kJoystickActionCount> latched_{}; // tapped since the last apply()
    std::deque<std::pair<uint8_t, bool>> keys_;        // Amiga raw code, released
    std::vector<SDL_Gamepad*> pads_;
    uint16_t last_joy_ = 0; // --input-debug: both ports as bits, to log only the frames that change
    bool debug_ = false, focused_ = true;
};

} // namespace x2::input
