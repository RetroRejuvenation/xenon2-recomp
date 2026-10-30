// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Input mapping tests (SDL3 key tables and binding config; no window or devices needed).
#include "input/input.h"
#include "machine.h"

#include <cstdio>

using namespace x2;
using namespace x2::input;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

void test_rawkeys() {
    CHECK(amiga_rawkey(SDL_SCANCODE_Q) == 0x10);
    CHECK(amiga_rawkey(SDL_SCANCODE_P) == 0x19); // the game's pause key
    CHECK(amiga_rawkey(SDL_SCANCODE_A) == 0x20);
    CHECK(amiga_rawkey(SDL_SCANCODE_L) == 0x28);
    CHECK(amiga_rawkey(SDL_SCANCODE_Z) == 0x31);
    CHECK(amiga_rawkey(SDL_SCANCODE_M) == 0x37);
    CHECK(amiga_rawkey(SDL_SCANCODE_1) == 0x01);
    CHECK(amiga_rawkey(SDL_SCANCODE_0) == 0x0A);
    CHECK(amiga_rawkey(SDL_SCANCODE_SPACE) == 0x40);
    CHECK(amiga_rawkey(SDL_SCANCODE_ESCAPE) == 0x45); // the game's abort-to-menu key
    CHECK(amiga_rawkey(SDL_SCANCODE_F1) == 0x50);
    CHECK(amiga_rawkey(SDL_SCANCODE_F10) == 0x59);
    CHECK(amiga_rawkey(SDL_SCANCODE_UP) == 0x4C);
    CHECK(amiga_rawkey(SDL_SCANCODE_F11) == -1);
}

void test_bindings_roundtrip() {
    Bindings d = default_bindings();
    Bindings b;
    std::string err;
    CHECK(parse_bindings(format_bindings(d), b, err));
    CHECK(err.empty());
    for (int a = 0; a < kActionCount; ++a) CHECK(b.keys[size_t(a)] == d.keys[size_t(a)]);

    Bindings c = default_bindings();
    err.clear();
    CHECK(!parse_bindings("p2_fire = Space, Return  # comment\nbogus = A\np1_up = NoSuchKey\n", c, err));
    CHECK(!err.empty());
}

void test_parse_rebind() {
    Bindings c = default_bindings();
    std::string err;
    CHECK(parse_bindings("p2_fire = Space, Return\n", c, err));
    CHECK(c.keys[P2Fire].size() == 2 && c.keys[P2Fire][0] == SDL_SCANCODE_SPACE && c.keys[P2Fire][1] == SDL_SCANCODE_RETURN);
    CHECK(c.keys[P2Up] == default_bindings().keys[P2Up]);
}

void test_events_to_machine() {
    InputManager im(default_bindings());
    auto key = [&](SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.down = down;
        return im.handle_event(e);
    };
    key(SDL_SCANCODE_UP, true);
    key(SDL_SCANCODE_LCTRL, true);
    JoyState j = im.port(2);
    CHECK(j.up && j.fire && !j.down && !j.left && !j.right);
    key(SDL_SCANCODE_UP, false);
    CHECK(!im.port(2).up);
    CHECK(key(SDL_SCANCODE_F11, true) == HostCommand::Fullscreen);
    CHECK(key(SDL_SCANCODE_F12, true) == HostCommand::Reset);
    CHECK(key(SDL_SCANCODE_PAUSE, true) == HostCommand::Pause);
    CHECK(key(SDL_SCANCODE_F7, true) == HostCommand::VolumeDown);
    CHECK(key(SDL_SCANCODE_F1, true) == HostCommand::Faithful);
    CHECK(key(SDL_SCANCODE_F2, true) == HostCommand::Filter);
    CHECK(key(SDL_SCANCODE_F3, true) == HostCommand::Interp);
    CHECK(key(SDL_SCANCODE_F4, true) == HostCommand::Aspect);
    CHECK(key(SDL_SCANCODE_F8, true) == HostCommand::VolumeUp);
    CHECK(key(SDL_SCANCODE_F5, true) == HostCommand::ScaleDown);
    CHECK(key(SDL_SCANCODE_F6, true) == HostCommand::ScaleUp);
    key(SDL_SCANCODE_W, true); // port 1 up
    CHECK(im.port(1).up);

    Machine m;
    key(SDL_SCANCODE_LEFT, true);
    im.apply(m);
    CHECK(m.read16(0xDFF00C) == 0x0300); // JOY1DAT: left (bit 9) and left XOR up (bit 8)
    CHECK(m.read16(0xDFF00A) == 0x0100); // JOY0DAT: up
    CHECK((m.read8(0xBFE001) & 0xC0) == 0x40); // port 2 fire pressed (bit 7 low), port 1 fire released (bit 6 high)
}

} // namespace

int main() {
    test_rawkeys();
    test_bindings_roundtrip();
    test_parse_rebind();
    test_events_to_machine();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
