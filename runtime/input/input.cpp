#include "input/input.h"

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace x2::input {

namespace {
const char* const kActionNames[kActionCount] = {
    "p2_up", "p2_down", "p2_left", "p2_right", "p2_fire",
    "p1_up", "p1_down", "p1_left", "p1_right", "p1_fire",
    "pause", "reset", "quit", "fullscreen",
    "volume_down", "volume_up", "scale_down", "scale_up",
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

constexpr int16_t kStickThreshold = 16000;
} // namespace

const char* action_name(int action) { return action >= 0 && action < kActionCount ? kActionNames[action] : "?"; }

Bindings default_bindings() {
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
    b.keys[HostPause] = {SDL_SCANCODE_PAUSE, SDL_SCANCODE_F9};
    b.keys[HostReset] = {SDL_SCANCODE_F12};
    b.keys[HostQuit] = {SDL_SCANCODE_F10};
    b.keys[HostFullscreen] = {SDL_SCANCODE_F11};
    b.keys[HostVolumeDown] = {SDL_SCANCODE_F7};
    b.keys[HostVolumeUp] = {SDL_SCANCODE_F8};
    b.keys[HostScaleDown] = {SDL_SCANCODE_F5};
    b.keys[HostScaleUp] = {SDL_SCANCODE_F6};
    return b;
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
            SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str());
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
                    "# restore the defaults. Unbound keys go to the game as Amiga keys (P pause, ESC menu).\n";
    for (int a = 0; a < kActionCount; ++a) {
        s += kActionNames[a];
        s += " =";
        for (size_t i = 0; i < b.keys[size_t(a)].size(); ++i) {
            s += i ? ", " : " ";
            s += SDL_GetScancodeName(b.keys[size_t(a)][i]);
        }
        s += "\n";
    }
    return s;
}

Bindings load_bindings(const std::string& dir, std::string& path_out, std::string& error) {
    Bindings b = default_bindings();
    if (dir.empty()) {
        error = "no per-user data directory\n";
        return b;
    }
    path_out = dir + "input.cfg";
    if (FILE* f = std::fopen(path_out.c_str(), "rb")) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        parse_bindings(text, b, error);
    } else if (FILE* w = std::fopen(path_out.c_str(), "wb")) {
        std::string text = format_bindings(b);
        std::fwrite(text.data(), 1, text.size(), w);
        std::fclose(w);
    }
    return b;
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

InputManager::~InputManager() {
    for (SDL_Gamepad* p : pads_) SDL_CloseGamepad(p);
}

bool InputManager::action_held(int action) const {
    for (SDL_Scancode sc : bindings_.keys[size_t(action)])
        if (std::find(held_.begin(), held_.end(), sc) != held_.end()) return true;
    return false;
}

HostCommand InputManager::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT: return HostCommand::Quit;
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
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        SDL_Scancode sc = e.key.scancode;
        if (down && e.key.repeat) return HostCommand::None;
        if (down && sc == SDL_SCANCODE_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) return HostCommand::Quit;
        if (down && sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) return HostCommand::Fullscreen;
        auto it = std::find(held_.begin(), held_.end(), sc);
        if (down && it == held_.end()) held_.push_back(sc);
        if (!down && it != held_.end()) held_.erase(it);
        for (int a = 0; a < kActionCount; ++a) {
            const auto& keys = bindings_.keys[size_t(a)];
            if (std::find(keys.begin(), keys.end(), sc) == keys.end()) continue;
            if (!down) return HostCommand::None;
            switch (a) {
            case HostPause: return HostCommand::Pause;
            case HostReset: return HostCommand::Reset;
            case HostQuit: return HostCommand::Quit;
            case HostFullscreen: return HostCommand::Fullscreen;
            case HostVolumeDown: return HostCommand::VolumeDown;
            case HostVolumeUp: return HostCommand::VolumeUp;
            case HostScaleDown: return HostCommand::ScaleDown;
            case HostScaleUp: return HostCommand::ScaleUp;
            default: return HostCommand::None; // joystick binding: read from held_ in port()
            }
        }
        int code = amiga_rawkey(sc);
        if (code >= 0) keys_.push_back({uint8_t(code), !down});
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
    m.set_joystick(p2.up, p2.down, p2.left, p2.right, p2.fire);
    m.set_joystick_port1(p1.up, p1.down, p1.left, p1.right, p1.fire);
    while (!keys_.empty()) {
        m.key_event(keys_.front().first, keys_.front().second);
        keys_.pop_front();
    }
}

} // namespace x2::input
