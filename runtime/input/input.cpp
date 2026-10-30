// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "input/input.h"

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace x2::input {

namespace {
const char* const kActionNames[kActionCount] = {
    "p2_up", "p2_down", "p2_left", "p2_right", "p2_fire",
    "p1_up", "p1_down", "p1_left", "p1_right", "p1_fire",
    "pause", "reset", "quit", "fullscreen",
    "volume_down", "volume_up", "scale_down", "scale_up",
    "faithful", "filter", "interp", "aspect",
    "asset_packs", "wide", "hd_panel", "music_source",
    "grab",
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

constexpr int16_t kStickThreshold = 16000;

unsigned ticks_ms() { return unsigned(SDL_GetTicks() & 0xFFFFFFFFu); } // --input-debug timestamps

// Key names for input.cfg must not depend on whether SDL's video driver has been initialised. SDL keeps one
// mutable name table and each backend renames a few entries in it at init (SDL_cocoakeyboard.m calls
// SDL_SetScancodeName(SDL_SCANCODE_LALT, "Left Option"), Windows uses "Left Windows" for LGUI, X11 and
// Wayland use "Menu" for Application), and SDL_GetScancodeFromName searches that same table. So a file the
// game writes with the window open says "Left Option", which --list-keys -- which never calls SDL_Init --
// cannot read back, and a file written on one platform cannot be read on another. We therefore write SDL's
// own platform-independent spelling and accept every platform's spelling when reading.
struct KeyAlias {
    const char* name;
    SDL_Scancode sc;
};
constexpr KeyAlias kCanonicalNames[] = {
    {"Left Alt", SDL_SCANCODE_LALT},
    {"Right Alt", SDL_SCANCODE_RALT},
    {"Left GUI", SDL_SCANCODE_LGUI},
    {"Right GUI", SDL_SCANCODE_RGUI},
    {"Application", SDL_SCANCODE_APPLICATION},
};
constexpr KeyAlias kNameAliases[] = {
    {"Left Option", SDL_SCANCODE_LALT},    {"Right Option", SDL_SCANCODE_RALT},
    {"Left Command", SDL_SCANCODE_LGUI},   {"Right Command", SDL_SCANCODE_RGUI},
    {"Left Windows", SDL_SCANCODE_LGUI},   {"Right Windows", SDL_SCANCODE_RGUI},
    {"Left Meta", SDL_SCANCODE_LGUI},      {"Right Meta", SDL_SCANCODE_RGUI},
    {"Menu", SDL_SCANCODE_APPLICATION},
};

// The name written to input.cfg: the same on every platform, whatever SDL is currently calling the key.
const char* stable_key_name(SDL_Scancode sc) {
    for (const KeyAlias& a : kCanonicalNames)
        if (a.sc == sc) return a.name;
    return SDL_GetScancodeName(sc);
}

// Any platform's spelling of a key, plus whatever SDL currently accepts.
SDL_Scancode key_from_name(const std::string& name) {
    for (const KeyAlias& a : kCanonicalNames)
        if (SDL_strcasecmp(name.c_str(), a.name) == 0) return a.sc;
    for (const KeyAlias& a : kNameAliases)
        if (SDL_strcasecmp(name.c_str(), a.name) == 0) return a.sc;
    return SDL_GetScancodeFromName(name.c_str());
}

// Offset from P2Up / P1Up for a gamepad button, or -1: the d-pad steers, any face button fires.
int gamepad_button_action(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 0;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 1;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 2;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 3;
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH: return 4;
    default: return -1;
    }
}
} // namespace

const char* action_name(int action) { return action >= 0 && action < kActionCount ? kActionNames[action] : "?"; }

namespace {
// The host actions, identical on every platform: the Amiga keyboard has no F11/F12 and none of these reach
// the game ($7954 handles P, ESC and Space only).
void host_defaults(Bindings& b) {
    b.keys[HostPause] = {SDL_SCANCODE_PAUSE, SDL_SCANCODE_F9};
    b.keys[HostReset] = {SDL_SCANCODE_F12};
    b.keys[HostQuit] = {SDL_SCANCODE_F10};
    b.keys[HostFullscreen] = {SDL_SCANCODE_F11};
    b.keys[HostVolumeDown] = {SDL_SCANCODE_F7};
    b.keys[HostVolumeUp] = {SDL_SCANCODE_F8};
    b.keys[HostScaleDown] = {SDL_SCANCODE_F5};
    b.keys[HostScaleUp] = {SDL_SCANCODE_F6};
    b.keys[HostFaithful] = {SDL_SCANCODE_F1};
    b.keys[HostFilter] = {SDL_SCANCODE_F2};
    b.keys[HostInterp] = {SDL_SCANCODE_F3};
    b.keys[HostAspect] = {SDL_SCANCODE_F4};
    // Keys the Amiga keyboard does not have (fn + Left / Right / Down / Up on a Mac laptop).
    b.keys[HostUpgrade] = {SDL_SCANCODE_HOME};
    b.keys[HostWide] = {SDL_SCANCODE_END};
    b.keys[HostPanel] = {SDL_SCANCODE_PAGEDOWN};
    b.keys[HostMusic] = {SDL_SCANCODE_PAGEUP};
    b.keys[HostGrab] = {SDL_SCANCODE_SCROLLLOCK};
}
} // namespace

Bindings default_bindings() {
    Bindings b;
    b.keys[P2Up] = {SDL_SCANCODE_UP};
    b.keys[P2Down] = {SDL_SCANCODE_DOWN};
    b.keys[P2Left] = {SDL_SCANCODE_LEFT};
    b.keys[P2Right] = {SDL_SCANCODE_RIGHT};
#ifdef __APPLE__
    // No Ctrl: Ctrl + the arrow keys are system-wide shortcuts on macOS (Ctrl+Left/Right switch Spaces,
    // Ctrl+Up opens Mission Control, Ctrl+Down App Expose), so holding fire and steering moved the window
    // and changed the focus instead of shooting. Option is free, and Z/X are the usual two-key arcade pair.
    b.keys[P2Fire] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT};
#else
    // Z and X first: Windows' "show location of pointer when I press the CTRL key" accessibility setting draws
    // a ring around the pointer on every Ctrl press, which is constant while firing, and Ctrl is the kind of
    // key a desktop is apt to claim. Ctrl stays bound, because that is what the Amiga used.
    b.keys[P2Fire] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL};
#endif
    b.keys[P1Up] = {SDL_SCANCODE_W};
    b.keys[P1Down] = {SDL_SCANCODE_S};
    b.keys[P1Left] = {SDL_SCANCODE_A};
    b.keys[P1Right] = {SDL_SCANCODE_D};
    b.keys[P1Fire] = {SDL_SCANCODE_LSHIFT};
    host_defaults(b);
    return b;
}

Bindings legacy_default_bindings() {
    Bindings b;
    b.keys[P2Up] = {SDL_SCANCODE_UP};
    b.keys[P2Down] = {SDL_SCANCODE_DOWN};
    b.keys[P2Left] = {SDL_SCANCODE_LEFT};
    b.keys[P2Right] = {SDL_SCANCODE_RIGHT};
    b.keys[P2Fire] = {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_Z};
    b.keys[P1Up] = {SDL_SCANCODE_W};
    b.keys[P1Down] = {SDL_SCANCODE_S};
    b.keys[P1Left] = {SDL_SCANCODE_A};
    b.keys[P1Right] = {SDL_SCANCODE_D};
    b.keys[P1Fire] = {SDL_SCANCODE_LSHIFT};
    host_defaults(b);
    b.keys[HostGrab].clear(); // version 2 added it; a version 1 file mentioning it was hand written
    return b;
}

HostCommand host_command_for_gui(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_1: return HostCommand::Faithful;   // as F1
    case SDL_SCANCODE_2: return HostCommand::Filter;     // F2
    case SDL_SCANCODE_3: return HostCommand::Interp;     // F3
    case SDL_SCANCODE_4: return HostCommand::Aspect;     // F4
    case SDL_SCANCODE_5: return HostCommand::ScaleDown;  // F5
    case SDL_SCANCODE_6: return HostCommand::ScaleUp;    // F6
    case SDL_SCANCODE_7: return HostCommand::VolumeDown; // F7
    case SDL_SCANCODE_8: return HostCommand::VolumeUp;   // F8
    case SDL_SCANCODE_9: return HostCommand::Pause;      // F9
    case SDL_SCANCODE_F: return HostCommand::Fullscreen; // F11 (the menu's own toggle is Ctrl+Cmd+F)
    case SDL_SCANCODE_R: return HostCommand::Reset;      // F12
    case SDL_SCANCODE_U: return HostCommand::Upgrade;    // Home
    case SDL_SCANCODE_E: return HostCommand::Wide;       // End
    case SDL_SCANCODE_D: return HostCommand::Panel;      // Page Down
    case SDL_SCANCODE_K: return HostCommand::Music;      // Page Up
    case SDL_SCANCODE_G: return HostCommand::Grab;       // Scroll Lock
    default: return HostCommand::None;
    }
}

bool parse_bindings(const std::string& text, Bindings& b, std::string& error) {
    std::istringstream in(text);
    std::string line;
    bool ok = true;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        size_t eq = line.find('=');
        std::string name = trim(line.substr(0, eq));
        int action = -1;
        for (int a = 0; a < kActionCount; ++a)
            if (name == kActionNames[a]) action = a;
        if (eq == std::string::npos || action < 0) {
            error += "line " + std::to_string(n) + ": unknown action '" + name + "'\n";
            ok = false;
            continue;
        }
        std::vector<SDL_Scancode> keys;
        std::istringstream list(line.substr(eq + 1));
        std::string key;
        while (std::getline(list, key, ',')) {
            key = trim(key);
            if (key.empty()) continue;
            SDL_Scancode sc = key_from_name(key);
            if (sc == SDL_SCANCODE_UNKNOWN) {
                error += "line " + std::to_string(n) + ": unknown key '" + key + "'\n";
                ok = false;
                continue;
            }
            keys.push_back(sc);
        }
        b.keys[size_t(action)] = keys;
    }
    return ok;
}

std::string format_bindings(const Bindings& b) {
    std::string s = "# x2recomp input bindings: action = key, key (SDL scancode names). Delete this file to\n"
                    "# restore the defaults. Unbound keys go to the game as Amiga keys (P pause, ESC menu).\n"
                    "# version: " + std::to_string(kBindingsVersion) +
                    " -- a file you have edited is never changed by the game; new defaults in a later\n"
                    "# version are only taken up by a file that still holds the old ones (kept as input.cfg.bak).\n";
    for (int a = 0; a < kActionCount; ++a) {
        s += kActionNames[a];
        s += " =";
        for (size_t i = 0; i < b.keys[size_t(a)].size(); ++i) {
            s += i ? ", " : " ";
            s += stable_key_name(b.keys[size_t(a)][i]);
        }
        s += "\n";
    }
    return s;
}

int bindings_version(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] != '#') continue;
        size_t at = s.find("version:");
        if (at == std::string::npos) continue;
        int v = std::atoi(trim(s.substr(at + 8)).c_str());
        if (v > 0) return v;
    }
    return 1;
}

CfgAction classify_bindings_file(const std::string& text) {
    if (bindings_version(text) >= kBindingsVersion) return CfgAction::Keep;
    // Mark every action as not-mentioned, then see which lines the file actually carries.
    Bindings seen;
    seen.keys.fill({SDL_SCANCODE_UNKNOWN});
    std::string err;
    if (!parse_bindings(text, seen, err)) return CfgAction::Keep; // not fully understood: leave it alone
    const Bindings old = legacy_default_bindings();
    for (int a = 0; a < kActionCount; ++a) {
        const auto& k = seen.keys[size_t(a)];
        if (k.size() == 1 && k[0] == SDL_SCANCODE_UNKNOWN) continue; // the file does not mention it
        if (k != old.keys[size_t(a)]) return CfgAction::Keep;        // the user changed it
    }
    return CfgAction::Upgrade;
}

namespace {
bool read_file(const std::string& path, std::string& text) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}
} // namespace

namespace {
// Appended to the note of any file that still fires on Ctrl, since that is the one binding that actively
// fights the host on macOS.
void warn_about_ctrl(LoadedBindings& r) {
#ifdef __APPLE__
    for (int a = 0; a < kJoystickActionCount; ++a)
        for (SDL_Scancode sc : r.bindings.keys[size_t(a)])
            if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL) {
                r.note += "\n  warning: this file binds Ctrl to " + std::string(action_name(a)) +
                          ", and Ctrl + an arrow key is a macOS system shortcut (Spaces, Mission Control):\n"
                          "  playing with it held will move windows and change the focus. Z, X and Option are free.";
                return;
            }
#else
    (void)r;
#endif
}
} // namespace

LoadedBindings preview_bindings(const std::string& dir) {
    LoadedBindings r;
    r.bindings = default_bindings();
    if (dir.empty()) {
        r.error = "no per-user data directory\n";
        return r;
    }
    r.path = dir + "input.cfg";
    std::string text;
    if (!read_file(r.path, text)) {
        r.state = CfgState::NoFile;
        r.note = "no file yet; the defaults below will be written on the next start";
        return r;
    }
    if (classify_bindings_file(text) == CfgAction::Upgrade) {
        // Keep the new defaults in `bindings`: they are what will apply, and what the caller should show.
        r.state = CfgState::WouldUpgrade;
        r.note = "this file holds the old defaults, so the game will upgrade it to version " +
                 std::to_string(kBindingsVersion) +
                 " on its next start\n  (the old file is kept as input.cfg.bak). The bindings below are the new ones.";
        return r;
    }
    parse_bindings(text, r.bindings, r.error);
    if (bindings_version(text) < kBindingsVersion) {
        r.state = CfgState::Customised;
        r.note = "your own bindings, kept as they are (delete the file for this platform's defaults)";
    } else {
        r.state = CfgState::Current;
    }
    warn_about_ctrl(r);
    return r;
}

LoadedBindings load_bindings(const std::string& dir) {
    LoadedBindings r = preview_bindings(dir);
    if (r.path.empty()) return r;
    if (r.state == CfgState::NoFile) {
        r.note = write_file(r.path, format_bindings(r.bindings)) ? "written with this platform's defaults"
                                                                 : "cannot be written; using the defaults";
        return r;
    }
    if (r.state == CfgState::WouldUpgrade) {
        std::string text;
        read_file(r.path, text);
        if (write_file(r.path + ".bak", text) && write_file(r.path, format_bindings(r.bindings))) {
            r.note = "upgraded to the version " + std::to_string(kBindingsVersion) +
                     " defaults (the old file is now input.cfg.bak)";
        } else { // could not replace it: go on with what it says, and say so
            r.bindings = default_bindings();
            parse_bindings(text, r.bindings, r.error);
            r.state = CfgState::Customised;
            r.note = "holds the old defaults but could not be rewritten; using what it says";
            warn_about_ctrl(r);
        }
    }
    return r;
}

std::string describe_bindings(const Bindings& b) {
    std::string s = "action        keys\n";
    for (int a = 0; a < kActionCount; ++a) {
        std::string name = kActionNames[a];
        s += name + std::string(name.size() < 13 ? 13 - name.size() : 1, ' ') + " ";
        if (b.keys[size_t(a)].empty()) s += "(unbound)";
        for (size_t i = 0; i < b.keys[size_t(a)].size(); ++i) {
            if (i) s += ", ";
            const char* sdl_name = SDL_GetScancodeName(b.keys[size_t(a)][i]);
            s += sdl_name && *sdl_name ? sdl_name : stable_key_name(b.keys[size_t(a)][i]);
        }
        s += "\n";
    }
    s += "\nAlways available: Alt+Return fullscreen, Cmd+Q / Ctrl+Q quit.\n";
#ifdef __APPLE__
    s += "'Left Alt' and 'Right Alt' above are the Option keys on a Mac keyboard.\n";
    s += "Also, for the F-keys the OS claims unless 'Use F1, F2 etc. as standard function keys' is on:\n"
         "  Cmd+1..Cmd+9  as F1..F9 (faithful, filter, interp, aspect, scale -/+, volume -/+, pause)\n"
         "  Cmd+F fullscreen  Cmd+R reset  Cmd+U asset packs  Cmd+E surround  Cmd+D HD panel\n"
         "  Cmd+K music source  Cmd+G keyboard grab\n"
         "The OS keeps Cmd+Q, Cmd+H, Cmd+M, Cmd+W and Ctrl+Cmd+F for the application menu.\n";
#endif
    s += "Keys bound to nothing reach the game as Amiga keys: P pauses, ESC aborts to the menu, Space fires.\n";
    return s;
}

int amiga_rawkey(SDL_Scancode sc) {
    // Letters by keyboard row: QWERTYUIOP = $10-$19, ASDFGHJKL = $20-$28, ZXCVBNM = $31-$37.
    static const char* const rows[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const int row_base[3] = {0x10, 0x20, 0x31};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char c = char('A' + (sc - SDL_SCANCODE_A));
        for (int r = 0; r < 3; ++r)
            for (int i = 0; rows[r][i]; ++i)
                if (rows[r][i] == c) return row_base[r] + i;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x01 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x50 + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_0: return 0x0A;
    case SDL_SCANCODE_GRAVE: return 0x00;
    case SDL_SCANCODE_MINUS: return 0x0B;
    case SDL_SCANCODE_EQUALS: return 0x0C;
    case SDL_SCANCODE_BACKSLASH: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON: return 0x29;
    case SDL_SCANCODE_APOSTROPHE: return 0x2A;
    case SDL_SCANCODE_NONUSHASH: return 0x2B;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x30;
    case SDL_SCANCODE_COMMA: return 0x38;
    case SDL_SCANCODE_PERIOD: return 0x39;
    case SDL_SCANCODE_SLASH: return 0x3A;
    case SDL_SCANCODE_SPACE: return 0x40;
    case SDL_SCANCODE_BACKSPACE: return 0x41;
    case SDL_SCANCODE_TAB: return 0x42;
    case SDL_SCANCODE_KP_ENTER: return 0x43;
    case SDL_SCANCODE_RETURN: return 0x44;
    case SDL_SCANCODE_ESCAPE: return 0x45;
    case SDL_SCANCODE_DELETE: return 0x46;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_UP: return 0x4C;
    case SDL_SCANCODE_DOWN: return 0x4D;
    case SDL_SCANCODE_RIGHT: return 0x4E;
    case SDL_SCANCODE_LEFT: return 0x4F;
    case SDL_SCANCODE_KP_0: return 0x0F;
    case SDL_SCANCODE_KP_1: return 0x1D;
    case SDL_SCANCODE_KP_2: return 0x1E;
    case SDL_SCANCODE_KP_3: return 0x1F;
    case SDL_SCANCODE_KP_4: return 0x2D;
    case SDL_SCANCODE_KP_5: return 0x2E;
    case SDL_SCANCODE_KP_6: return 0x2F;
    case SDL_SCANCODE_KP_PERIOD: return 0x3C;
    case SDL_SCANCODE_KP_7: return 0x3D;
    case SDL_SCANCODE_KP_8: return 0x3E;
    case SDL_SCANCODE_KP_9: return 0x3F;
    case SDL_SCANCODE_KP_LEFTPAREN: return 0x5A;
    case SDL_SCANCODE_KP_RIGHTPAREN: return 0x5B;
    case SDL_SCANCODE_KP_DIVIDE: return 0x5C;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x5D;
    case SDL_SCANCODE_KP_PLUS: return 0x5E;
    case SDL_SCANCODE_HELP: case SDL_SCANCODE_INSERT: return 0x5F;
    case SDL_SCANCODE_LSHIFT: return 0x60;
    case SDL_SCANCODE_RSHIFT: return 0x61;
    case SDL_SCANCODE_CAPSLOCK: return 0x62;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x63;
    case SDL_SCANCODE_LALT: return 0x64;
    case SDL_SCANCODE_RALT: return 0x65;
    case SDL_SCANCODE_LGUI: return 0x66;
    case SDL_SCANCODE_RGUI: return 0x67;
    default: return -1;
    }
}

bool GrabPolicy::parse_mode(const std::string& name, Mode& out) {
    if (name == "auto") out = Mode::Auto;
    else if (name == "on" || name == "yes" || name == "true" || name == "1") out = Mode::On;
    else if (name == "off" || name == "no" || name == "false" || name == "0") out = Mode::Off;
    else return false;
    return true;
}

const char* GrabPolicy::mode_name(Mode m) {
    return m == Mode::Auto ? "auto" : m == Mode::On ? "on" : "off";
}

bool GrabPolicy::want_grab() const {
    if (mode_ == Mode::Off || (mode_ == Mode::Auto && !auto_default_)) return false;
    return focused_ && !paused_;
}

bool CursorPolicy::parse_mode(const std::string& name, Mode& out) {
    if (name == "auto") out = Mode::Auto;
    else if (name == "show" || name == "on" || name == "yes") out = Mode::Show;
    else if (name == "hide" || name == "off" || name == "no") out = Mode::Hide;
    else return false;
    return true;
}

const char* CursorPolicy::mode_name(Mode m) {
    return m == Mode::Auto ? "auto" : m == Mode::Show ? "show" : "hide";
}

bool CursorPolicy::want_visible() const {
    if (mode_ == Mode::Show) return true;
    if (!focused_) return true; // the pointer belongs to whatever the user went to
    return mode_ == Mode::Auto && paused_;
}

InputManager::~InputManager() {
    for (SDL_Gamepad* p : pads_) SDL_CloseGamepad(p);
}

bool InputManager::action_held(int action) const {
    for (SDL_Scancode sc : bindings_.keys[size_t(action)])
        if (std::find(held_.begin(), held_.end(), sc) != held_.end()) return true;
    return false;
}

int InputManager::bound_action(SDL_Scancode sc) const {
    for (int a = 0; a < kActionCount; ++a) {
        const auto& keys = bindings_.keys[size_t(a)];
        if (std::find(keys.begin(), keys.end(), sc) != keys.end()) return a;
    }
    return -1;
}

void InputManager::forget_held(const char* why, bool drop_taps) {
    // The key-ups went to whoever has the focus now, so nothing is held any more. Release the keys the game
    // was told about, or it keeps seeing them down.
    int released = 0;
    for (SDL_Scancode sc : held_) {
        if (bound_action(sc) >= 0) continue; // never sent as an Amiga key
        int code = amiga_rawkey(sc);
        if (code >= 0) { keys_.push_back({uint8_t(code), true}); ++released; }
    }
    if (debug_ && (!held_.empty() || released))
        std::printf("[input] %u %s: dropped %zu held, released %d amiga\n", ticks_ms(), why, held_.size(),
                    released);
    held_.clear();
    if (drop_taps) latched_.fill(false);
}

void InputManager::log_grab(bool on, const char* why) {
    if (debug_) std::printf("[input] %u grab %s (%s)\n", ticks_ms(), on ? "on" : "off", why);
}

void InputManager::log_cursor(bool visible, const char* why) {
    if (debug_) std::printf("[input] %u cursor %s (%s)\n", ticks_ms(), visible ? "shown" : "hidden", why);
}

HostCommand InputManager::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT: return HostCommand::Quit;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        // Drop whatever looked held, but keep a tap that has not reached a frame yet: the events of a press
        // that arrives with the focus are still owed a frame.
        focused_ = true;
        forget_held("focus gained", false);
        return HostCommand::None;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused_ = false;
        forget_held("focus lost", true);
        return HostCommand::None;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        // Buttons are sampled once per emulated frame; latch the press so a tap between two frames counts.
        int base = -1;
        for (size_t i = 0; i < pads_.size() && base < 0; ++i)
            if (SDL_GetGamepadID(pads_[i]) == e.gbutton.which) base = i == 0 ? P2Up : P1Up;
        int off = gamepad_button_action(SDL_GamepadButton(e.gbutton.button));
        if (base >= 0 && off >= 0) {
            latched_[size_t(base + off)] = true;
            if (debug_)
                std::printf("[input] %u pad %d button %s -> %s\n", ticks_ms(), base == P2Up ? 2 : 1,
                            SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button)),
                            action_name(base + off));
        }
        return HostCommand::None;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) {
            pads_.push_back(p);
            std::printf("gamepad connected: %s (drives port %d)\n", SDL_GetGamepadName(p), pads_.size() == 1 ? 2 : 1);
        }
        return HostCommand::None;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (size_t i = 0; i < pads_.size(); ++i) {
            if (SDL_GetGamepadID(pads_[i]) == e.gdevice.which) {
                SDL_CloseGamepad(pads_[i]);
                pads_.erase(pads_.begin() + long(i));
                std::printf("gamepad disconnected\n");
                break;
            }
        }
        return HostCommand::None;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        const SDL_Scancode sc = e.key.scancode;
        auto log = [&](const char* what) {
            if (debug_)
                std::printf("[input] %u %-4s %s sc=%d key=%08X mod=%04X rep=%d %s\n", ticks_ms(),
                            down ? "down" : "up", SDL_GetScancodeName(sc), int(sc), unsigned(e.key.key),
                            unsigned(e.key.mod), e.key.repeat ? 1 : 0, what);
        };
        if (down && e.key.repeat) { log("repeat, ignored"); return HostCommand::None; }
        if (down && sc == SDL_SCANCODE_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
            log("-> quit");
            return HostCommand::Quit;
        }
        if (down && sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
            log("-> fullscreen");
            return HostCommand::Fullscreen;
        }
#ifdef __APPLE__
        // Cmd combinations for the host actions whose F-key the OS claims first. Only on macOS: elsewhere the
        // same key is Super, which the desktop environment uses for its own shortcuts.
        if (down && (e.key.mod & SDL_KMOD_GUI) && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            if (HostCommand c = host_command_for_gui(sc); c != HostCommand::None) {
                log("-> cmd hotkey");
                return c;
            }
        }
#endif
        auto it = std::find(held_.begin(), held_.end(), sc);
        if (down && it == held_.end()) held_.push_back(sc);
        if (!down && it != held_.end()) held_.erase(it);
        const int a = bound_action(sc);
        if (a >= 0) {
            if (a < kJoystickActionCount) {
                if (down) latched_[size_t(a)] = true; // seen for at least one frame, even if released first
                log(action_name(a));
                return HostCommand::None;
            }
            if (!down) { log(action_name(a)); return HostCommand::None; }
            log(action_name(a));
            switch (a) {
            case HostPause: return HostCommand::Pause;
            case HostReset: return HostCommand::Reset;
            case HostQuit: return HostCommand::Quit;
            case HostFullscreen: return HostCommand::Fullscreen;
            case HostVolumeDown: return HostCommand::VolumeDown;
            case HostVolumeUp: return HostCommand::VolumeUp;
            case HostScaleDown: return HostCommand::ScaleDown;
            case HostScaleUp: return HostCommand::ScaleUp;
            case HostFaithful: return HostCommand::Faithful;
            case HostFilter: return HostCommand::Filter;
            case HostInterp: return HostCommand::Interp;
            case HostAspect: return HostCommand::Aspect;
            case HostUpgrade: return HostCommand::Upgrade;
            case HostWide: return HostCommand::Wide;
            case HostPanel: return HostCommand::Panel;
            case HostMusic: return HostCommand::Music;
            case HostGrab: return HostCommand::Grab;
            default: return HostCommand::None;
            }
        }
        int code = amiga_rawkey(sc);
        if (code >= 0) {
            keys_.push_back({uint8_t(code), !down});
            if (debug_) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "amiga $%02X", code);
                log(buf);
            }
        } else log("no mapping");
        return HostCommand::None;
    }
    default: return HostCommand::None;
    }
}

JoyState InputManager::port(int amiga_port) const {
    int base = amiga_port == 2 ? P2Up : P1Up;
    JoyState j{action_held(base), action_held(base + 1), action_held(base + 2), action_held(base + 3),
               action_held(base + 4)};
    size_t pad = amiga_port == 2 ? 0 : 1;
    if (pad < pads_.size()) {
        SDL_Gamepad* p = pads_[pad];
        int16_t ax = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        j.up |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -kStickThreshold;
        j.down |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > kStickThreshold;
        j.left |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -kStickThreshold;
        j.right |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > kStickThreshold;
        j.fire |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST) ||
                  SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH);
    }
    return j;
}

void InputManager::apply(Machine& m) {
    JoyState p2 = port(2), p1 = port(1);
    // A tap that began and ended between two frames is only in the latch; hold it for this one frame.
    auto latch = [&](JoyState& j, int base) {
        bool* f[5] = {&j.up, &j.down, &j.left, &j.right, &j.fire};
        for (int i = 0; i < 5; ++i) *f[i] |= latched_[size_t(base + i)];
    };
    latch(p2, P2Up);
    latch(p1, P1Up);
    auto bits = [](const JoyState& j) {
        return uint8_t(j.up | j.down << 1 | j.left << 2 | j.right << 3 | j.fire << 4);
    };
    const uint16_t now = uint16_t(bits(p2) | bits(p1) << 8);
    if (debug_ && (now != last_joy_ || !keys_.empty())) {
        auto text = [](uint8_t b) {
            std::string s = "-----";
            const char* on = "UDLRF";
            for (int i = 0; i < 5; ++i)
                if (b & (1 << i)) s[size_t(i)] = on[i];
            return s;
        };
        std::string tapped;
        for (int a = 0; a < kJoystickActionCount; ++a)
            if (latched_[size_t(a)] && !action_held(a)) tapped += std::string(tapped.empty() ? "" : ",") + action_name(a);
        std::printf("[input] %u frame %u p2=%s p1=%s keys=%zu%s%s\n", ticks_ms(), m.frame, text(bits(p2)).c_str(),
                    text(bits(p1)).c_str(), keys_.size(), tapped.empty() ? "" : " tap=", tapped.c_str());
    }
    last_joy_ = now;
    latched_.fill(false);
    m.set_joystick(p2.up, p2.down, p2.left, p2.right, p2.fire);
    m.set_joystick_port1(p1.up, p1.down, p1.left, p1.right, p1.fire);
    while (!keys_.empty()) {
        m.key_event(keys_.front().first, keys_.front().second);
        keys_.pop_front();
    }
}

} // namespace x2::input
