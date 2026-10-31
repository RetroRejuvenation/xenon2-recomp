// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_input.h"

#include "st/st_machine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace x2::st {

namespace {
bool parse_u32(const std::string& t, uint32_t& v, bool allow_plus = false) {
    if (t.empty()) return false;
    const char* b = t.c_str() + ((allow_plus && t[0] == '+') ? 1 : 0);
    char* end = nullptr;
    const unsigned long x = std::strtoul(b, &end, 0);
    if (end == b || *end || x > 0xFFFFFFFFul) return false;
    v = uint32_t(x);
    return true;
}

// U D L R F (any case); B (the Amiga's second button) and the like are not on an ST joystick.
bool parse_dirs(const std::string& s, uint8_t& bits) {
    bits = 0;
    for (char c : s) {
        switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'U': bits |= kJoyUp; break;
        case 'D': bits |= kJoyDown; break;
        case 'L': bits |= kJoyLeft; break;
        case 'R': bits |= kJoyRight; break;
        case 'F': bits |= kJoyFire; break;
        default: return false;
        }
    }
    return bits != 0;
}

bool iequals_prefix(const std::string& s, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i)
        if (i >= s.size() || std::toupper(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    return true;
}
} // namespace

bool parse_st_script(const std::string& text, StScript& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    uint32_t last = 0;
    int lineno = 0;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(lineno) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineno;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string t0, cmd;
        if (!(ss >> t0)) continue;
        if (t0 == "frames") {
            uint32_t n;
            std::string tn;
            if (!(ss >> tn) || !parse_u32(tn, n)) return fail("frames needs a number");
            out.frames = n;
            out.frames_set = true;
            continue;
        }
        uint32_t f;
        if (!parse_u32(t0, f, true)) return fail("expected a frame number, got '" + t0 + "'");
        if (t0[0] == '+') f += last;
        last = f;
        if (!(ss >> cmd)) return fail("missing command");
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);

        // Hatari harness lines: <vbl> JOYn_DIR 0|1, <vbl> KEYhh 0|1.
        if (iequals_prefix(cmd, "JOY0_") || iequals_prefix(cmd, "JOY1_")) {
            const int port = cmd[3] - '0';
            const std::string name = cmd.substr(5);
            uint8_t bits = 0;
            if (name == "UP" || name == "up") bits = kJoyUp;
            else if (name == "DOWN" || name == "down") bits = kJoyDown;
            else if (name == "LEFT" || name == "left") bits = kJoyLeft;
            else if (name == "RIGHT" || name == "right") bits = kJoyRight;
            else if (name == "FIRE" || name == "fire") bits = kJoyFire;
            else return fail("unknown joystick event '" + cmd + "'");
            uint32_t v;
            if (tok.size() != 1 || !parse_u32(tok[0], v) || v > 1) return fail(cmd + " needs 0 or 1");
            ScriptEvent e;
            e.kind = ScriptEvent::Joy;
            e.frame = f;
            e.hold = 0;
            e.bits = bits;
            e.port = port;
            e.edge = true;
            e.value = v != 0;
            out.events.push_back(e);
            continue;
        }
        if (iequals_prefix(cmd, "KEY") && cmd.size() > 3 && cmd != "key") {
            uint32_t sc, v;
            if (tok.size() != 1 || !parse_u32("0x" + cmd.substr(3), sc) || sc == 0 || sc > 0x7F || !parse_u32(tok[0], v) || v > 1)
                return fail("bad key event '" + cmd + "'");
            ScriptEvent e;
            e.kind = ScriptEvent::Key;
            e.frame = f;
            e.hold = 0;
            e.scancode = uint8_t(sc);
            e.edge = true;
            e.value = v != 0;
            out.events.push_back(e);
            continue;
        }

        uint32_t every = 0, until = f;
        for (size_t i = 0; i < tok.size(); ++i) {
            if (tok[i] != "every") continue;
            if (i + 4 != tok.size() || tok[i + 2] != "until" || !parse_u32(tok[i + 1], every) || !parse_u32(tok[i + 3], until) || !every ||
                until < f)
                return fail("bad 'every P until G'");
            tok.resize(i);
            break;
        }
        auto count = [&](size_t i, uint32_t def, uint32_t& v) {
            if (i >= tok.size()) { v = def; return true; }
            return parse_u32(tok[i], v) && v > 0;
        };
        ScriptEvent e;
        e.frame = f;
        if (cmd == "fire") {
            e.kind = ScriptEvent::Joy;
            e.bits = kJoyFire;
            if (!count(0, 3, e.hold)) return fail("fire needs a frame count");
        } else if (cmd == "joy") {
            e.kind = ScriptEvent::Joy;
            if (tok.empty() || !parse_dirs(tok[0], e.bits)) return fail("joy needs directions (letters U D L R F)");
            if (!count(1, 1, e.hold)) return fail("joy needs a frame count");
        } else if (cmd == "key") {
            e.kind = ScriptEvent::Key;
            uint32_t sc;
            if (tok.empty() || !parse_u32("0x" + tok[0], sc) || sc == 0 || sc > 0x7F) return fail("key needs an ST scancode in hex");
            e.scancode = uint8_t(sc);
            if (!count(1, 2, e.hold)) return fail("key needs a frame count");
        } else if (cmd == "poke" || cmd == "pokeobj") {
            return fail(cmd + " is not available for the ST target");
        } else {
            return fail("unknown command '" + cmd + "'");
        }
        out.events.push_back(e);
        if (every)
            for (uint32_t g = f + every; g <= until; g += every) {
                ScriptEvent r = e;
                r.frame = g;
                out.events.push_back(r);
            }
    }
    return true;
}

bool load_st_script(const std::string& path, StScript& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string why;
    if (!parse_st_script(ss.str(), out, why)) {
        error = path + ": " + why;
        return false;
    }
    return true;
}

ScriptPlayer::ScriptPlayer(StScript s) : script_(std::move(s)) {
    for (size_t i = 0; i < script_.events.size(); ++i)
        if (script_.events[i].edge) edge_order_.push_back(i);
    std::stable_sort(edge_order_.begin(), edge_order_.end(),
                     [&](size_t a, size_t b) { return script_.events[a].frame < script_.events[b].frame; });
}

void ScriptPlayer::apply(StMachine& m, uint32_t frame) {
    // Hatari-style edges that have come due.
    while (edge_next_ < edge_order_.size() && script_.events[edge_order_[edge_next_]].frame <= frame) {
        const ScriptEvent& e = script_.events[edge_order_[edge_next_++]];
        if (e.kind == ScriptEvent::Joy) {
            uint8_t& st = edge_state_[e.port & 1];
            st = e.value ? uint8_t(st | e.bits) : uint8_t(st & ~e.bits);
        } else {
            m.key_event(e.scancode, !e.value);
        }
    }
    // Windows: held joystick states, and keys pressed for a number of frames.
    uint8_t state[2] = {edge_state_[0], edge_state_[1]};
    for (const ScriptEvent& e : script_.events) {
        if (e.edge) continue;
        if (e.kind == ScriptEvent::Joy) {
            if (frame >= e.frame && frame < e.frame + e.hold) state[e.port & 1] |= e.bits;
        } else {
            if (frame == e.frame) m.key_event(e.scancode, false);
            if (frame == e.frame + e.hold) m.key_event(e.scancode, true);
        }
    }
    for (int p = 0; p < 2; ++p)
        if (!delivered_[p] ? state[p] != 0 : state[p] != held_[p]) {
            m.set_joystick(p, state[p]);
            held_[p] = state[p];
            delivered_[p] = true;
        }
}

uint8_t st_scancode_for_amiga_raw(int raw) {
    // Amiga raw key codes (the host layer's) to ST scancodes, for the keys a game can use.
    static const struct { uint8_t amiga, st; } kMap[] = {
        {0x45, 0x01}, {0x01, 0x02}, {0x02, 0x03}, {0x03, 0x04}, {0x04, 0x05}, {0x05, 0x06}, {0x06, 0x07}, {0x07, 0x08},
        {0x08, 0x09}, {0x09, 0x0A}, {0x0A, 0x0B}, {0x41, 0x0E}, {0x42, 0x0F}, {0x10, 0x10}, {0x11, 0x11}, {0x12, 0x12},
        {0x13, 0x13}, {0x14, 0x14}, {0x15, 0x15}, {0x16, 0x16}, {0x17, 0x17}, {0x18, 0x18}, {0x19, 0x19}, {0x44, 0x1C},
        {0x20, 0x1E}, {0x21, 0x1F}, {0x22, 0x20}, {0x23, 0x21}, {0x24, 0x22}, {0x25, 0x23}, {0x26, 0x24}, {0x27, 0x25},
        {0x28, 0x26}, {0x31, 0x2C}, {0x32, 0x2D}, {0x33, 0x2E}, {0x34, 0x2F}, {0x35, 0x30}, {0x36, 0x31}, {0x37, 0x32},
        {0x40, 0x39}, {0x50, 0x3B}, {0x51, 0x3C}, {0x52, 0x3D}, {0x53, 0x3E}, {0x54, 0x3F}, {0x55, 0x40}, {0x56, 0x41},
        {0x57, 0x42}, {0x58, 0x43}, {0x59, 0x44},
    };
    for (const auto& k : kMap)
        if (k.amiga == raw) return k.st;
    return 0;
}

} // namespace x2::st
