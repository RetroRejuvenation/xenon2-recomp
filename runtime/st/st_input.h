// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Scripted and host input for the ST machine.
//
// The script is the project's frame-indexed input script (tests/replays/README.md) reduced to what the ST has: `FRAME fire [N]`,
// `FRAME joy DIRS [N]` (letters U D L R F: the game's joystick, IKBD port 1) and `FRAME key HEX [N]` (an ST scancode, pressed
// for N frames, default 2), each optionally "every P until G", and `frames N`. It also reads the Hatari harness's
// lines, `VBL JOY1_FIRE 1` / `VBL JOY0_LEFT 0` / `VBL KEY39 1` (tools/compare/hatari-harness/inputs), so one script drives
// both the reference and this runtime. The game reads joystick port 1 (docs/re-notes/atari-st-s0.md); port 0 is the mouse port.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2::st {

class StMachine;

// IKBD joystick state bits (the packet's state byte): up, down, left, right, fire.
constexpr uint8_t kJoyUp = 0x01, kJoyDown = 0x02, kJoyLeft = 0x04, kJoyRight = 0x08, kJoyFire = 0x80;

struct ScriptEvent {
    enum Kind { Joy, Key } kind = Joy;
    uint32_t frame = 0;
    uint32_t hold = 1;   // frames the state or key is held (Joy and Key windows)
    uint8_t bits = 0;    // Joy: IKBD joystick bits
    uint8_t scancode = 0; // Key
    int port = 1;        // Joy: IKBD joystick port 0 or 1
    // Hatari-style edges: set (or clear) `bits` on `port` from `frame` on, until the matching clear. hold == 0 marks an edge.
    bool edge = false, value = false;
};

struct StScript {
    std::vector<ScriptEvent> events;
    uint32_t frames = 0;
    bool frames_set = false;
};

// Parse a script (the text). False with `error` ("line N: ...") on a bad line.
bool parse_st_script(const std::string& text, StScript& out, std::string& error);
bool load_st_script(const std::string& path, StScript& out, std::string& error);

// Applies a script to a machine frame by frame (call before each run_frame).
class ScriptPlayer {
public:
    explicit ScriptPlayer(StScript s);
    void apply(StMachine& m, uint32_t frame);
    uint32_t frames() const { return script_.frames; }
    bool frames_set() const { return script_.frames_set; }

private:
    StScript script_;
    uint8_t held_[2] = {0, 0}; // the state last delivered, per port
    bool delivered_[2] = {false, false};
    uint8_t edge_state_[2] = {0, 0};
    size_t edge_next_ = 0;
    std::vector<size_t> edge_order_;
};

// The ST scancode for an Amiga raw key code (what the host input layer produces), or 0 when the ST has no equivalent.
uint8_t st_scancode_for_amiga_raw(int raw);

// Host joystick bits (up, down, left, right, fire) -> the IKBD state byte.
constexpr uint8_t joy_bits(bool up, bool down, bool left, bool right, bool fire) {
    return uint8_t((up ? kJoyUp : 0) | (down ? kJoyDown : 0) | (left ? kJoyLeft : 0) | (right ? kJoyRight : 0) | (fire ? kJoyFire : 0));
}

} // namespace x2::st
