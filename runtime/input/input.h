// Host input for the windowed front end (SDL3): keyboard and gamepads mapped to the Amiga joystick ports and
// keyboard.
//
// - Joystick port 2 (JOY1DAT + CIA-A PRA bit 7) is the one the game reads ($20DE in the VBL handler). Its
//   two-player mode alternates players on that same joystick. Port 1 (JOY0DAT + PRA bit 6) is wired too.
// - Keys bound to a joystick or host action are consumed; every other key is sent to the game as an Amiga raw
//   keycode through the CIA-A serial port (the game's handler at $0FAC). The game's own keys: P = pause,
//   ESC = abort to menu, Space = $40 (handled at $797A).
// - Gamepads are hot-pluggable. The first one connected drives port 2, the second port 1: d-pad or left stick
//   for directions, any face button for fire.
// - Bindings are loaded from input.cfg in SDL_GetPrefPath("xenon2-recomp", "x2recomp"), written with the
//   defaults if missing. Format: one `action = Key Name, Key Name` per line (SDL scancode names), # comments.
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
    kActionCount
};

const char* action_name(int action);

struct Bindings {
    std::array<std::vector<SDL_Scancode>, kActionCount> keys;
};

Bindings default_bindings();
// Parses config text into `b` (actions not mentioned keep their current keys). Returns false and sets `error`
// for an unknown action or key name; the valid lines are still applied.
bool parse_bindings(const std::string& text, Bindings& b, std::string& error);
std::string format_bindings(const Bindings& b);
// Loads <pref path>/input.cfg, creating it with the defaults if absent. `path_out` receives the file used.
Bindings load_bindings(std::string& path_out, std::string& error);

// Amiga raw keycode for a host key, or -1 if there is none.
int amiga_rawkey(SDL_Scancode sc);

enum class HostCommand { None, Pause, Reset, Quit, Fullscreen };

struct JoyState {
    bool up = false, down = false, left = false, right = false, fire = false;
};

class InputManager {
public:
    explicit InputManager(Bindings b) : bindings_(std::move(b)) {}
    ~InputManager();
    // Feed every SDL event. Returns a host command triggered by this event, if any.
    HostCommand handle_event(const SDL_Event& e);
    // Joystick state from keys and gamepads, and pending Amiga key events, applied to the machine. Call once
    // per emulated frame.
    void apply(Machine& m);
    JoyState port(int amiga_port) const; // 1 or 2
    size_t gamepads() const { return pads_.size(); }

private:
    bool action_held(int action) const;
    Bindings bindings_;
    std::vector<SDL_Scancode> held_;
    std::deque<std::pair<uint8_t, bool>> keys_; // Amiga raw code, released
    std::vector<SDL_Gamepad*> pads_;
};

} // namespace x2::input
