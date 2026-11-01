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
// - F1 (and Start on a gamepad) opens the in-game menu (runtime/ui/overlay.h), which has the Faithful switch and every
//   other option. The gamepad buttons for fire, the second button and the menu are bindings too (pad_* lines).
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
    HostAutofire, HostGodMode, HostAddCredit, // assists and cheats (off by default)
    HostProtectMusic,                         // keep the tune audible through the effects (not faithful)
    HostButton2,                              // the port-2 second fire button (POTGOR bit 14): read by the CDTV release only
    HostMenu,                                 // the in-game menu (F1); also the gamepad's menu button
    kActionCount
};

// The joystick actions come first: P2Up..P1Fire.
constexpr int kJoystickActionCount = HostPause;

const char* action_name(int action);

// The gamepad buttons that do something besides steering (the d-pad and the left stick always steer and are not bindings).
// Stored in input.cfg as pad_fire, pad_button2 and pad_menu, with the names pad_button_name() writes.
struct PadBindings {
    std::vector<SDL_GamepadButton> fire = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                           SDL_GAMEPAD_BUTTON_NORTH};
    std::vector<SDL_GamepadButton> button2 = {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    std::vector<SDL_GamepadButton> menu = {SDL_GAMEPAD_BUTTON_START};
    bool operator==(const PadBindings& o) const { return fire == o.fire && button2 == o.button2 && menu == o.menu; }
};

struct Bindings {
    std::array<std::vector<SDL_Scancode>, kActionCount> keys;
    PadBindings pad;
    bool operator==(const Bindings& o) const { return keys == o.keys && pad == o.pad; }
};

// Names for the Controls page and input.cfg. The pad names do not depend on the pad's labels: `south` is A on an Xbox pad and
// Cross on a PlayStation one.
const char* pad_button_name(SDL_GamepadButton b);
bool pad_button_from_name(const std::string& name, SDL_GamepadButton& out);
// The buttons a pad can be bound to, in the order the Controls page offers them.
const std::vector<SDL_GamepadButton>& bindable_pad_buttons();
// A key as people write it ("Left Alt", "F1"), the same on every platform.
std::string key_label(SDL_Scancode sc);
// The action another action's key is already bound to, or -1. Joystick actions of the two ports count as different actions.
int find_binding_conflict(const Bindings& b, int action, SDL_Scancode sc);
// Keys the game itself reads (P pauses, Esc aborts to the menu, Space): binding one takes it away from the game.
bool is_game_key(SDL_Scancode sc);

// The defaults for this platform (macOS avoids the system-shortcut keys; see the file comment).
Bindings default_bindings();
// Every set of defaults this program has ever written for THIS platform, newest first. A file holding
// nothing but one of them carries no choice of the user's, so classify_bindings_file() treats it as ours and
// upgrades it; anything else is theirs and is left alone. Listing every set, rather than only the one the
// file's version marker names, matters twice over: the defaults have always been platform-specific, so two
// versions can coincide on one platform and differ on another, and the Windows and Linux fire order changed
// once within version 2 without a format bump. A file written on a different platform matches no set here
// and is therefore kept, never silently rewritten.
std::vector<Bindings> historical_default_sets();
// The bindings every release up to and including 0.1.0 wrote (format version 1, the same on every platform).
Bindings legacy_default_bindings();

// Parses config text into `b` (actions not mentioned keep their current keys). Returns false and sets `error`
// for an unknown action or key name; the valid lines are still applied.
bool parse_bindings(const std::string& text, Bindings& b, std::string& error);
std::string format_bindings(const Bindings& b);

// input.cfg format version. Files written before the macOS-safe defaults carry no version marker.
constexpr int kBindingsVersion = 6;
// Version marker a file declares, 1 when it has none.
int bindings_version(const std::string& text);

enum class CfgAction {
    Keep,   // the current version, or bindings the user changed: use the file as it is
    Upgrade // an old file holding nothing but old defaults: replace it with the new ones
};
CfgAction classify_bindings_file(const std::string& text);

enum class CfgState {
    NoFile,       // nothing on disk yet
    Current,      // written by this version of the format
    WouldUpgrade, // holds an older version's defaults, so the defaults replace it
    Customised    // the user's own bindings, left alone
};

struct LoadedBindings {
    Bindings bindings; // what applies (for WouldUpgrade: the new defaults, not what the file says)
    std::string path;  // the file, empty without a per-user data directory
    std::string error; // parse problems, one per line
    std::string note;  // a line about the file for the log, empty when there is nothing to say
    CfgState state = CfgState::NoFile;
};

// What <dir>input.cfg means for this run, WITHOUT reading or writing anything else: the bindings that will
// apply and a note phrased as what the next start will do. Used by --list-keys, which must not change a file
// the user is in the middle of testing with.
LoadedBindings preview_bindings(const std::string& dir);

// preview_bindings(), then the writes it describes: the defaults when there is no file, and an upgrade of a
// file that holds only an older version's defaults (keeping it as input.cfg.bak). A file the user has
// changed is never rewritten.
LoadedBindings load_bindings(const std::string& dir);

// Writes <dir>input.cfg as format_bindings() says (a temporary file, then a rename, so an interrupted write leaves the old one). This
// is the Controls page's save: the user changed the bindings on purpose, so the file becomes theirs (version 6, never upgraded).
bool save_bindings_file(const std::string& dir, const Bindings& b, std::string& why);

// The binding table as the --list-keys output: every action, its keys, and the built-in combinations.
std::string describe_bindings(const Bindings& b);

// Amiga raw keycode for a host key, or -1 if there is none.
int amiga_rawkey(SDL_Scancode sc);

enum class HostCommand {
    None, Pause, Reset, Quit, Fullscreen, VolumeDown, VolumeUp, ScaleDown, ScaleUp,
    Menu,                             // open or close the in-game menu (F1, Start on a pad)
    Faithful, Filter, Interp, Aspect, // picture options (Faithful switch, next filter, interpolation, 4:3)
    Upgrade, Wide, Panel,             // asset packs on/off, next surround width, HD status panel on/off
    Music,                            // next music source (amiga, cdtv, mix)
    Grab,                             // keyboard grab on/off
    Autofire, GodMode, AddCredit,     // assists and cheats
    ProtectMusic                      // keep the tune audible through the effects
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

// Should the mouse cursor be visible right now? Pure logic so it can be tested without a window.
//
// The game has no mouse input at all (the "mouse port" is joystick port 1 here), so an arrow sitting over the
// picture is only ever in the way. SDL_HideCursor() just stops drawing it: the pointer keeps moving and stays
// the window manager's, unlike SDL_SetWindowRelativeMouseMode(), which would also confine and capture it and
// is deliberately not used. The first-run import screen runs before this policy exists, so the cursor is
// always visible there.
class CursorPolicy {
public:
    enum class Mode {
        Auto, // hidden while the window has the focus and the game is running
        Show, // always visible
        Hide  // hidden whenever the window has the focus, frozen or not
    };
    static bool parse_mode(const std::string& name, Mode& out);
    static const char* mode_name(Mode m);

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    void set_focused(bool v) { focused_ = v; }
    void set_paused(bool v) { paused_ = v; }
    bool want_visible() const;

private:
    Mode mode_ = Mode::Auto;
    bool focused_ = false, paused_ = false;
};

// Should the fire line be asserted this frame? Pure logic so it can be tested without a window.
//
// Measured on the real game (docs/re-notes/ghidra-code-map.md, and the experiment in the commit message):
// the VBL handler at $20DE latches a fire PRESS as an edge into $A01, and the player's weapon handler reads
// the held bit $A00 bit 7 but only fires again once its own cooldown expires. Holding fire gives about 5
// shots per 400 frames; tapping gives about 60, and a pulse every 2-3 frames reaches that ceiling. So
// autofire pulses the line while fire is physically held.
//
// Two safeguards keep it out of the menus, which read the same edge latch to make selections: it only
// engages while a game is in progress (the caller passes that; the runtime reads the game's ship count), and
// only after fire has been held longer than a tap, so the first press is always delivered unchanged.
class AutofirePolicy {
public:
    static constexpr int kHoldFrames = 15;  // about 0.3 s at 50 Hz before pulsing starts
    static constexpr int kDefaultRate = 17; // pulses per second: every third frame, the game's own maximum

    void set_enabled(bool v) { enabled_ = v; }
    bool enabled() const { return enabled_; }
    void set_rate(int pulses_per_second);
    int rate() const { return rate_; }
    int period_frames() const; // frames between pulses
    // True while a game is running; outside one, autofire never engages.
    void set_in_game(bool v) { in_game_ = v; }
    // Once per emulated frame, with the physically held state. Returns what the machine should see.
    bool fire(bool held);
    // True when the last fire() call was pulsing rather than passing the input through.
    bool engaged() const { return engaged_; }

private:
    bool enabled_ = false, in_game_ = false, engaged_ = false;
    int rate_ = kDefaultRate;
    int held_frames_ = 0;
};

// The modes the window title advertises, so a screen recording shows which were on. Composed in one place
// and pure, so it can be tested; the caller passes what the user ASKED for rather than any transient state
// (protect music fades over 20 ms, and the title must not flicker during it).
struct WindowModes {
    bool keyboard_grabbed = false;
    bool autofire = false;
    bool protect_music = false;
    bool god_mode = false;  // a cheat in its own right
    bool cheat_used = false; // some other cheat was used this session
};
// " [keyboard grabbed] [autofire] ..." -- empty when every mode is off. god_mode implies a cheat was used, so
// only the more specific label is shown.
std::string window_mode_suffix(const WindowModes& m);

// What the host input amounts to for one emulated frame, independent of the machine it is delivered to.
struct FrameInput {
    JoyState p2, p1;           // Amiga port 2 (the primary joystick) and port 1, with taps and autofire applied
    bool button2 = false;
    std::vector<std::pair<uint8_t, bool>> keys; // Amiga raw key codes (code, released), in order
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
    // The same, without a machine: consumes the taps and queued keys and returns them (apply() is collect() plus the delivery).
    // `frame` is only used by --input-debug.
    FrameInput collect(uint32_t frame);
    JoyState port(int amiga_port) const; // 1 or 2; what is held now, without the tap latch
    // The second fire button of port 2: the `button2` key, or a shoulder button of the first gamepad.
    bool button2() const;
    size_t gamepads() const { return pads_.size(); }
    // The bindings in force. set_bindings() replaces them (the Controls page of the in-game menu) and drops whatever looked held.
    const Bindings& bindings() const { return bindings_; }
    void set_bindings(Bindings b);
    // Forget every held key and pending tap: the in-game menu took the keyboard for a while, and the key-ups went to it.
    void release_all(const char* why);
    bool focused() const { return focused_; }
    // --input-debug: log every key, focus change and frame to stdout.
    void set_debug(bool on) { debug_ = on; }
    void log_grab(bool on, const char* why);
    void log_cursor(bool visible, const char* why);
    // Autofire for both joystick ports. The caller keeps them in step with the settings and tells them
    // whether a game is running; apply() consults them.
    AutofirePolicy& autofire(int amiga_port) { return autofire_[amiga_port == 2 ? 0 : 1]; }

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
    std::array<AutofirePolicy, 2> autofire_; // [0] = port 2, [1] = port 1
    uint16_t last_joy_ = 0; // --input-debug: both ports as bits, to log only the frames that change
    bool debug_ = false, focused_ = true;
};

} // namespace x2::input
