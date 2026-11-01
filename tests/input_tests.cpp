// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Input mapping tests (SDL3 key tables and binding config; no window or devices needed).
#include "input/input.h"
#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

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

// SDL keeps one mutable scancode-name table and each video backend renames entries in it at init (macOS turns
// "Left Alt" into "Left Option", Windows "Left GUI" into "Left Windows", X11 "Application" into "Menu"), and
// SDL_GetScancodeFromName searches that same table. input.cfg must not depend on it: the game writes the file
// with the window open, while --list-keys never calls SDL_Init.
void test_key_names_are_stable() {
    const std::string text = format_bindings(default_bindings());
    // The written spelling is SDL's platform-independent one, whatever SDL is currently calling the key.
    CHECK(text.find("Left Option") == std::string::npos);
    CHECK(text.find("Left Command") == std::string::npos);
    CHECK(text.find("Left Windows") == std::string::npos);
#ifdef __APPLE__
    CHECK(text.find("Left Alt") != std::string::npos); // p2_fire has both Option keys
#endif
    // Every platform's spelling reads back, in either initialisation state. "Left Option" is what the file on
    // the reporter's Mac actually says, written by a running game.
    struct { const char* name; SDL_Scancode sc; } cases[] = {
        {"Left Option", SDL_SCANCODE_LALT},   {"Right Option", SDL_SCANCODE_RALT},
        {"Left Alt", SDL_SCANCODE_LALT},      {"Right Alt", SDL_SCANCODE_RALT},
        {"Left Command", SDL_SCANCODE_LGUI},  {"Right Command", SDL_SCANCODE_RGUI},
        {"Left Windows", SDL_SCANCODE_LGUI},  {"Left Meta", SDL_SCANCODE_LGUI},
        {"Left GUI", SDL_SCANCODE_LGUI},      {"Application", SDL_SCANCODE_APPLICATION},
        {"Menu", SDL_SCANCODE_APPLICATION},   {"left option", SDL_SCANCODE_LALT}, // names are case-insensitive
    };
    for (const auto& c : cases) {
        Bindings b = default_bindings();
        std::string err;
        CHECK(parse_bindings(std::string("p2_fire = ") + c.name + "\n", b, err));
        CHECK(err.empty());
        CHECK(b.keys[P2Fire].size() == 1 && b.keys[P2Fire][0] == c.sc);
    }
    // A file holding the macOS spelling is still recognised as the version 2 defaults, not as customised.
    std::string mac_written = text;
    for (size_t at = mac_written.find("Left Alt"); at != std::string::npos; at = mac_written.find("Left Alt", at))
        mac_written.replace(at, 8, "Left Option");
    for (size_t at = mac_written.find("Right Alt"); at != std::string::npos; at = mac_written.find("Right Alt", at))
        mac_written.replace(at, 9, "Right Option");
    Bindings b = default_bindings();
    std::string err;
    CHECK(parse_bindings(mac_written, b, err));
    CHECK(err.empty());
    for (int a = 0; a < kActionCount; ++a) CHECK(b.keys[size_t(a)] == default_bindings().keys[size_t(a)]);
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
    const SDL_Scancode fire = default_bindings().keys[P2Fire].front(); // Left Ctrl, or Z on macOS
    key(SDL_SCANCODE_UP, true);
    key(fire, true);
    JoyState j = im.port(2);
    CHECK(j.up && j.fire && !j.down && !j.left && !j.right);
    key(SDL_SCANCODE_UP, false);
    CHECK(!im.port(2).up);
    CHECK(key(SDL_SCANCODE_F11, true) == HostCommand::Fullscreen);
    CHECK(key(SDL_SCANCODE_F12, true) == HostCommand::Reset);
    CHECK(key(SDL_SCANCODE_PAUSE, true) == HostCommand::Pause);
    CHECK(key(SDL_SCANCODE_F7, true) == HostCommand::VolumeDown);
    CHECK(key(SDL_SCANCODE_F1, true) == HostCommand::Menu); // F1 opens the in-game menu; the faithful action has no default key
    CHECK(key(SDL_SCANCODE_F2, true) == HostCommand::Filter);
    CHECK(key(SDL_SCANCODE_F3, true) == HostCommand::Interp);
    CHECK(key(SDL_SCANCODE_F4, true) == HostCommand::Aspect);
    CHECK(key(SDL_SCANCODE_F8, true) == HostCommand::VolumeUp);
    CHECK(key(SDL_SCANCODE_F5, true) == HostCommand::ScaleDown);
    CHECK(key(SDL_SCANCODE_F6, true) == HostCommand::ScaleUp);
    key(SDL_SCANCODE_W, true); // port 1 up
    CHECK(im.port(1).up);

    Machine m;
    im.apply(m); // flush the taps above, so only what is held now reaches the machine
    key(SDL_SCANCODE_LEFT, true);
    im.apply(m);
    CHECK(m.read16(0xDFF00C) == 0x0300); // JOY1DAT: left (bit 9) and left XOR up (bit 8)
    CHECK(m.read16(0xDFF00A) == 0x0100); // JOY0DAT: up
    CHECK((m.read8(0xBFE001) & 0xC0) == 0x40); // port 2 fire pressed (bit 7 low), port 1 fire released (bit 6 high)
}

// The defaults must not collide with the keys the game reads, nor with the host's own shortcuts.
void test_platform_defaults() {
    Bindings d = default_bindings();
    auto binds = [&](int action, SDL_Scancode sc) {
        const auto& k = d.keys[size_t(action)];
        return std::find(k.begin(), k.end(), sc) != k.end();
    };
    for (int a = 0; a < kActionCount; ++a) {
        CHECK(!binds(a, SDL_SCANCODE_SPACE));  // the game's own fire key ($40, handled at $797A)
        CHECK(!binds(a, SDL_SCANCODE_P));      // the game's pause
        CHECK(!binds(a, SDL_SCANCODE_ESCAPE)); // the game's abort to menu
    }
    // No key drives two actions.
    for (int a = 0; a < kActionCount; ++a)
        for (SDL_Scancode sc : d.keys[size_t(a)])
            for (int b = a + 1; b < kActionCount; ++b) CHECK(!binds(b, sc));
#ifdef __APPLE__
    // Ctrl + an arrow key is a system-wide shortcut on macOS (Spaces, Mission Control, App Expose).
    for (int a = 0; a < kActionCount; ++a) {
        CHECK(!binds(a, SDL_SCANCODE_LCTRL));
        CHECK(!binds(a, SDL_SCANCODE_RCTRL));
    }
    CHECK(binds(P2Fire, SDL_SCANCODE_Z) && binds(P2Fire, SDL_SCANCODE_LALT) && binds(P2Fire, SDL_SCANCODE_RALT));
#else
    // Ctrl stays bound off macOS, but Z and X come first: Windows draws a ring around the pointer on every
    // Ctrl press if the accessibility setting for it is on.
    CHECK(binds(P2Fire, SDL_SCANCODE_LCTRL) && binds(P2Fire, SDL_SCANCODE_RCTRL));
    CHECK(d.keys[P2Fire].at(0) == SDL_SCANCODE_Z && d.keys[P2Fire].at(1) == SDL_SCANCODE_X);
#endif
    // The historical table the migration compares against must keep the bindings 0.1.0 actually wrote.
    const std::vector<SDL_Scancode> v1_fire = {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_Z};
    CHECK(legacy_default_bindings().keys[P2Fire] == v1_fire);
}

void test_gui_hotkeys() {
    InputManager im(default_bindings());
    auto cmd = [&](SDL_Scancode sc, SDL_Keymod mod) {
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = sc;
        e.key.down = true;
        e.key.mod = mod;
        return im.handle_event(e);
    };
    CHECK(cmd(SDL_SCANCODE_Q, SDL_KMOD_LGUI) == HostCommand::Quit);
    CHECK(cmd(SDL_SCANCODE_RETURN, SDL_KMOD_LALT) == HostCommand::Fullscreen);
    // The table itself. The keys SDL's application menu claims must stay out of it: Cmd+W closes the window,
    // Cmd+M minimises, Cmd+H hides, and the menu's own fullscreen toggle is Ctrl+Cmd+F.
    CHECK(host_command_for_gui(SDL_SCANCODE_F) == HostCommand::Fullscreen);
    CHECK(host_command_for_gui(SDL_SCANCODE_R) == HostCommand::Reset);
    CHECK(host_command_for_gui(SDL_SCANCODE_1) == HostCommand::Menu);
    CHECK(host_command_for_gui(SDL_SCANCODE_9) == HostCommand::Pause);
    CHECK(host_command_for_gui(SDL_SCANCODE_G) == HostCommand::Grab);
    CHECK(host_command_for_gui(SDL_SCANCODE_W) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_M) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_H) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_J) == HostCommand::None);
#ifdef __APPLE__
    CHECK(cmd(SDL_SCANCODE_F, SDL_KMOD_LGUI) == HostCommand::Fullscreen);
    CHECK(cmd(SDL_SCANCODE_1, SDL_KMOD_LGUI) == HostCommand::Menu);
    CHECK(cmd(SDL_SCANCODE_G, SDL_KMOD_LGUI) == HostCommand::Grab);
    CHECK(cmd(SDL_SCANCODE_F, SDL_Keymod(SDL_KMOD_LGUI | SDL_KMOD_LCTRL)) == HostCommand::None);
    CHECK(cmd(SDL_SCANCODE_J, SDL_KMOD_LGUI) == HostCommand::None);
#else
    // Elsewhere that key is Super and belongs to the desktop environment.
    CHECK(cmd(SDL_SCANCODE_F, SDL_KMOD_LGUI) == HostCommand::None);
    CHECK(cmd(SDL_SCANCODE_1, SDL_KMOD_LGUI) == HostCommand::None);
#endif
}

// A tap whose press and release both land between two emulated frames must still be seen for one frame.
void test_tap_not_lost() {
    InputManager im(default_bindings());
    const SDL_Scancode fire = default_bindings().keys[P2Fire].front();
    auto key = [&](SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.down = down;
        im.handle_event(e);
    };
    auto fire_low = [](Machine& m) { return (m.read8(0xBFE001) & 0x80) == 0; };

    Machine m;
    im.apply(m);
    CHECK(!fire_low(m));
    key(fire, true);
    key(fire, false);          // released before the frame ran
    CHECK(!im.port(2).fire);   // port() still reports only what is held
    im.apply(m);
    CHECK(fire_low(m));        // ... but the frame sees the tap
    im.apply(m);
    CHECK(!fire_low(m));       // and only that one frame, so the game sees a press and a release

    key(fire, true);           // held across frames: every frame sees it
    im.apply(m);
    CHECK(fire_low(m));
    im.apply(m);
    CHECK(fire_low(m));
    key(fire, false);
    im.apply(m);
    CHECK(!fire_low(m));
}

void test_focus_clears_held() {
    InputManager im(default_bindings());
    auto key = [&](SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.down = down;
        im.handle_event(e);
    };
    auto window_event = [&](SDL_EventType type) {
        SDL_Event e{};
        e.type = type;
        im.handle_event(e);
    };
    CHECK(im.focused());
    key(SDL_SCANCODE_UP, true);
    key(SDL_SCANCODE_B, true); // not bound: the game was told B is down ($35)
    CHECK(im.port(2).up);

    Machine m;
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    CHECK(!im.focused());
    CHECK(!im.port(2).up); // the key-up went to whoever has the focus now
    im.apply(m);
    CHECK(!im.port(2).up);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    CHECK(im.focused());
    CHECK(!im.port(2).up);
    im.apply(m);
    CHECK((m.read16(0xDFF00C) & 0x0300) == 0); // JOY1DAT: nothing held

    // A tap that arrives with the focus is still owed a frame, so regaining the focus must not discard it.
    const SDL_Scancode fire = default_bindings().keys[P2Fire].front();
    key(fire, true);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    key(fire, false);
    im.apply(m);
    CHECK((m.read8(0xBFE001) & 0x80) == 0);
    im.apply(m);
    CHECK((m.read8(0xBFE001) & 0x80) != 0);
}

void test_grab_policy() {
    GrabPolicy::Mode m = GrabPolicy::Mode::Auto;
    CHECK(GrabPolicy::parse_mode("auto", m) && m == GrabPolicy::Mode::Auto);
    CHECK(GrabPolicy::parse_mode("on", m) && m == GrabPolicy::Mode::On);
    CHECK(GrabPolicy::parse_mode("off", m) && m == GrabPolicy::Mode::Off);
    CHECK(!GrabPolicy::parse_mode("sometimes", m));

    GrabPolicy p;
    p.set_mode(GrabPolicy::Mode::On);
    CHECK(!p.want_grab()); // not focused yet
    p.set_focused(true);
    CHECK(p.want_grab());
    p.set_paused(true);
    CHECK(!p.want_grab()); // a paused game must not hold the system's shortcuts
    p.set_paused(false);
    p.set_focused(false);
    CHECK(!p.want_grab());
    p.set_focused(true);
    p.set_mode(GrabPolicy::Mode::Off);
    CHECK(!p.want_grab());
    p.set_mode(GrabPolicy::Mode::Auto);
    CHECK(!p.want_grab()); // auto is off on every platform today
    p.set_auto_default(true);
    CHECK(p.want_grab());
}

void test_cursor_policy() {
    CursorPolicy::Mode m = CursorPolicy::Mode::Show;
    CHECK(CursorPolicy::parse_mode("auto", m) && m == CursorPolicy::Mode::Auto);
    CHECK(CursorPolicy::parse_mode("show", m) && m == CursorPolicy::Mode::Show);
    CHECK(CursorPolicy::parse_mode("hide", m) && m == CursorPolicy::Mode::Hide);
    CHECK(CursorPolicy::parse_mode("off", m) && m == CursorPolicy::Mode::Hide);
    CHECK(!CursorPolicy::parse_mode("sometimes", m));
    CHECK(std::string(CursorPolicy::mode_name(CursorPolicy::Mode::Auto)) == "auto");

    CursorPolicy p;
    CHECK(p.mode() == CursorPolicy::Mode::Auto);
    CHECK(p.want_visible()); // nothing is hidden before the window has the focus

    p.set_focused(true);
    CHECK(!p.want_visible()); // playing: the arrow is only in the way
    p.set_paused(true);
    CHECK(p.want_visible()); // frozen: the user wants the pointer back
    p.set_focused(false);
    CHECK(p.want_visible());
    p.set_paused(false);
    CHECK(p.want_visible()); // the pointer belongs to whatever they went to

    p.set_mode(CursorPolicy::Mode::Show);
    p.set_focused(true);
    CHECK(p.want_visible());
    p.set_paused(true);
    CHECK(p.want_visible());

    p.set_mode(CursorPolicy::Mode::Hide);
    p.set_paused(true);
    CHECK(!p.want_visible()); // hide means hide, frozen or not
    p.set_paused(false);
    CHECK(!p.want_visible());
    p.set_focused(false);
    CHECK(p.want_visible()); // ... but never outside our own window
}

// Holding fire gives about 5 shots per 400 frames on the real game; tapping gives about 60, and a pulse
// every 2-3 frames reaches that ceiling (measured with --audio-cues; see the commit message). Autofire
// pulses while fire is held, but must leave taps and the menus exactly alone.
void test_autofire_policy() {
    AutofirePolicy a;
    CHECK(!a.enabled());
    CHECK(a.rate() == AutofirePolicy::kDefaultRate);
    CHECK(a.period_frames() == 3); // 17 pulses a second at 50 Hz, the game's own maximum
    a.set_rate(25);
    CHECK(a.period_frames() == 2);
    a.set_rate(1);
    CHECK(a.period_frames() == 50);
    a.set_rate(1000); // clamped
    CHECK(a.rate() == 25);
    a.set_rate(0);
    CHECK(a.rate() == 1);

    // Off: whatever comes in goes out, held for as long as it is held.
    a.set_rate(AutofirePolicy::kDefaultRate);
    a.set_in_game(true);
    int on = 0;
    for (int i = 0; i < 60; ++i) on += a.fire(true);
    CHECK(on == 60);
    CHECK(!a.engaged());
    CHECK(!a.fire(false));

    // On, but not in a game: the menus read the same press as a selection, so nothing is pulsed.
    a.set_enabled(true);
    a.set_in_game(false);
    on = 0;
    for (int i = 0; i < 60; ++i) on += a.fire(true);
    CHECK(on == 60);
    CHECK(!a.engaged());
    a.fire(false);

    // On and in a game: a tap is passed through untouched...
    a.set_in_game(true);
    for (int i = 0; i < AutofirePolicy::kHoldFrames; ++i) {
        CHECK(a.fire(true));
        CHECK(!a.engaged());
    }
    a.fire(false);
    // ... and a hold starts pulsing once it is clearly a hold, one frame on in every period.
    on = 0;
    const int frames = AutofirePolicy::kHoldFrames + 300;
    for (int i = 0; i < frames; ++i) on += a.fire(true);
    CHECK(a.engaged());
    CHECK(on == AutofirePolicy::kHoldFrames + 300 / a.period_frames());
    // The pulse really is an edge: the frame after an asserted one is released.
    a.fire(false);
    for (int i = 0; i < AutofirePolicy::kHoldFrames; ++i) a.fire(true);
    CHECK(a.fire(true));  // first pulse
    CHECK(!a.fire(true)); // and off again, so the game sees a new press next time
    CHECK(!a.fire(true));
    CHECK(a.fire(true));

    // Releasing resets everything, so the next press is a fresh pass-through.
    CHECK(!a.fire(false));
    CHECK(!a.engaged());
    CHECK(a.fire(true));
    CHECK(!a.engaged());
}

void test_window_mode_suffix() {
    WindowModes m;
    CHECK(window_mode_suffix(m).empty()); // nothing on: nothing in the title
    m.protect_music = true;
    CHECK(window_mode_suffix(m) == " [protect music]");
    m.autofire = true;
    CHECK(window_mode_suffix(m) == " [autofire] [protect music]");
    m.keyboard_grabbed = true;
    CHECK(window_mode_suffix(m) == " [keyboard grabbed] [autofire] [protect music]");
    // A cheat is named, and god mode is specific enough on its own.
    WindowModes c;
    c.cheat_used = true;
    CHECK(window_mode_suffix(c) == " [CHEAT used]");
    c.god_mode = true;
    CHECK(window_mode_suffix(c) == " [CHEAT: god mode]");
    CHECK(window_mode_suffix(c).find("[CHEAT used]") == std::string::npos);
}

void test_protect_music_action() {
    Bindings d = default_bindings();
    // Bound on every platform, and not to a key anything else already uses.
    CHECK(d.keys[HostProtectMusic].size() == 1 && d.keys[HostProtectMusic][0] == SDL_SCANCODE_DELETE);
    CHECK(d.keys[HostGodMode][0] == SDL_SCANCODE_BACKSPACE); // Delete is deliberately not Backspace
    CHECK(amiga_rawkey(SDL_SCANCODE_DELETE) == 0x46);        // what it used to reach the game as
    // The key and the Cmd combination both reach the command.
    InputManager im(d);
    auto key = [&](SDL_Scancode sc, SDL_Keymod mod) {
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = sc;
        e.key.down = true;
        e.key.mod = mod;
        return im.handle_event(e);
    };
    CHECK(key(SDL_SCANCODE_DELETE, SDL_KMOD_NONE) == HostCommand::ProtectMusic);
    CHECK(host_command_for_gui(SDL_SCANCODE_Y) == HostCommand::ProtectMusic);
#ifdef __APPLE__
    CHECK(key(SDL_SCANCODE_Y, SDL_KMOD_LGUI) == HostCommand::ProtectMusic);
#else
    CHECK(key(SDL_SCANCODE_Y, SDL_KMOD_LGUI) == HostCommand::None); // Super belongs to the desktop
#endif
    // Releasing it must not re-trigger.
    SDL_Event up{};
    up.type = SDL_EVENT_KEY_UP;
    up.key.scancode = SDL_SCANCODE_DELETE;
    CHECK(im.handle_event(up) == HostCommand::None);
}

// The CDTV release's second button: bound to C by default, held (not latched: the game samples POTGOR only every 20 frames) and
// delivered to the machine as POTGOR bit 14, active low. Nothing else changes in POTGOR.
void test_button2_action() {
    Bindings d = default_bindings();
    CHECK(d.keys[HostButton2].size() == 1 && d.keys[HostButton2][0] == SDL_SCANCODE_C);
    CHECK(std::string(action_name(HostButton2)) == "button2");
    InputManager im(d);
    auto key = [&](SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.down = down;
        return im.handle_event(e);
    };
    Machine m;
    CHECK(m.read16(0xDFF016) == 0xFF00);
    CHECK(!im.button2());
    CHECK(key(SDL_SCANCODE_C, true) == HostCommand::None); // a joystick-like action: no host command
    CHECK(im.button2());
    im.apply(m);
    CHECK(m.read16(0xDFF016) == 0xBF00);
    CHECK(key(SDL_SCANCODE_C, false) == HostCommand::None);
    im.apply(m);
    CHECK(m.read16(0xDFF016) == 0xFF00);
    // The same via the scripted/headless path.
    m.set_button2(true);
    CHECK(m.read16(0xDFF016) == 0xBF00);
    m.set_button2(false);
    CHECK(m.read16(0xDFF016) == 0xFF00);
}

// F1 opens the in-game menu; the Faithful switch became a row of that menu and keeps its action, now without a default key.
void test_menu_action() {
    const Bindings d = default_bindings();
    CHECK(d.keys[HostMenu] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    CHECK(d.keys[HostFaithful].empty());
    CHECK(std::string(action_name(HostMenu)) == "menu");
    InputManager im(d);
    auto key = [&](InputManager& m, SDL_Scancode sc, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = sc;
        e.key.down = down;
        return m.handle_event(e);
    };
    CHECK(key(im, SDL_SCANCODE_F1, true) == HostCommand::Menu);
    CHECK(key(im, SDL_SCANCODE_F1, false) == HostCommand::None);
    // Unbound, the faithful action does nothing; bound by the user, it is still the Faithful switch.
    Bindings own = d;
    own.keys[HostFaithful] = {SDL_SCANCODE_F8};
    own.keys[HostVolumeUp] = {SDL_SCANCODE_F7};
    own.keys[HostVolumeDown].clear();
    InputManager im2(own);
    CHECK(key(im2, SDL_SCANCODE_F8, true) == HostCommand::Faithful);
    // set_bindings() replaces the table and forgets what looked held.
    InputManager im3(d);
    key(im3, SDL_SCANCODE_UP, true);
    CHECK(im3.port(2).up);
    im3.set_bindings(own);
    CHECK(!im3.port(2).up);
    CHECK(im3.bindings() == own);
    key(im3, SDL_SCANCODE_UP, true);
    CHECK(im3.port(2).up);
    im3.release_all("test");
    CHECK(!im3.port(2).up);
    // The gamepad's menu button opens it too, whichever pad sends it (and Start is not a fire button).
    SDL_Event pad{};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.which = 4242;
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_START;
    pad.gbutton.down = true;
    CHECK(im.handle_event(pad) == HostCommand::Menu);
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(im.handle_event(pad) == HostCommand::None);
    Bindings other_menu = d;
    other_menu.pad.menu = {SDL_GAMEPAD_BUTTON_BACK};
    InputManager im4(other_menu);
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_START;
    CHECK(im4.handle_event(pad) == HostCommand::None);
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_BACK;
    CHECK(im4.handle_event(pad) == HostCommand::Menu);
}

// The gamepad buttons are bindings too: pad_fire, pad_button2 and pad_menu in input.cfg.
void test_pad_bindings() {
    const Bindings d = default_bindings();
    CHECK(d.pad.fire.size() == 4 && d.pad.button2.size() == 2 && d.pad.menu.size() == 1);
    // The names are the pad's own labels' neutral spelling, case-insensitive, and every bindable button has one that reads back.
    for (SDL_GamepadButton b : bindable_pad_buttons()) {
        SDL_GamepadButton back = SDL_GAMEPAD_BUTTON_INVALID;
        CHECK(std::string(pad_button_name(b)) != "?");
        CHECK(pad_button_from_name(pad_button_name(b), back) && back == b);
    }
    SDL_GamepadButton b;
    CHECK(pad_button_from_name("SOUTH", b) && b == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(!pad_button_from_name("dpad_up", b)); // the d-pad steers; it is not a binding
    CHECK(!pad_button_from_name("", b));
    // Round trip through the file.
    Bindings own = d;
    own.pad.fire = {SDL_GAMEPAD_BUTTON_SOUTH};
    own.pad.button2 = {SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    own.pad.menu = {SDL_GAMEPAD_BUTTON_BACK};
    Bindings read = d;
    std::string err;
    CHECK(parse_bindings(format_bindings(own), read, err));
    CHECK(err.empty());
    CHECK(read == own);
    CHECK(parse_bindings(format_bindings(d), read, err) && read == d);
    // An empty list is a choice too (no button fires), and a bad name is reported while the rest still applies.
    Bindings none = d;
    CHECK(parse_bindings("pad_button2 =\npad_fire = north\n", none, err));
    CHECK(none.pad.button2.empty() && none.pad.fire == std::vector<SDL_GamepadButton>{SDL_GAMEPAD_BUTTON_NORTH});
    Bindings bad = d;
    err.clear();
    CHECK(!parse_bindings("pad_fire = south, banana\npad_menu = start\n", bad, err));
    CHECK(err.find("banana") != std::string::npos);
    CHECK(bad.pad.fire == std::vector<SDL_GamepadButton>{SDL_GAMEPAD_BUTTON_SOUTH});
    // The pad lines never decide whether a file is "ours": a version 5 file has none.
    CHECK(format_bindings(d).find("pad_fire = south, east, west, north") != std::string::npos);
}

void test_binding_helpers() {
    Bindings b = default_bindings();
    CHECK(find_binding_conflict(b, HostFaithful, SDL_SCANCODE_F2) == HostFilter);
    CHECK(find_binding_conflict(b, HostFilter, SDL_SCANCODE_F2) == -1); // its own key is no conflict
    CHECK(find_binding_conflict(b, HostFaithful, SDL_SCANCODE_F12) == HostReset);
    CHECK(find_binding_conflict(b, HostFaithful, SDL_SCANCODE_Q) == -1);
    CHECK(find_binding_conflict(b, P1Fire, b.keys[P2Fire].front()) == P2Fire); // the two joysticks are different actions
    CHECK(is_game_key(SDL_SCANCODE_P) && is_game_key(SDL_SCANCODE_ESCAPE) && is_game_key(SDL_SCANCODE_SPACE));
    CHECK(!is_game_key(SDL_SCANCODE_F1) && !is_game_key(SDL_SCANCODE_C));
    CHECK(key_label(SDL_SCANCODE_F1) == "F1");
    CHECK(!key_label(SDL_SCANCODE_LALT).empty());

    // save_bindings_file(): the file reads back, it is the current version (so it is never upgraded over), and a failed write is reported.
    const std::string dir = (std::filesystem::temp_directory_path() / "x2_input_save_test").string() + "/";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Bindings own = default_bindings();
    own.keys[HostFaithful] = {SDL_SCANCODE_F8};
    own.keys[HostVolumeUp] = {SDL_SCANCODE_F7};
    own.keys[HostVolumeDown].clear();
    own.pad.fire = {SDL_GAMEPAD_BUTTON_SOUTH};
    std::string why;
    CHECK(save_bindings_file(dir, own, why));
    CHECK(why.empty());
    CHECK(!std::filesystem::exists(dir + "input.cfg.tmp"));
    LoadedBindings loaded = load_bindings(dir);
    CHECK(loaded.error.empty());
    CHECK(loaded.state == CfgState::Current);
    CHECK(loaded.bindings == own);
    CHECK(!save_bindings_file("", own, why) && !why.empty());
    why.clear();
    CHECK(!save_bindings_file(dir + "missing/", own, why) && !why.empty());
    std::filesystem::remove_all(dir, ec);
}

// A file written before version 6 has no `menu` line: F1 is the menu unless the file itself uses F1 (it was the Faithful switch).
void test_menu_key_migration() {
    const std::string dir = (std::filesystem::temp_directory_path() / "x2_input_menu_test").string() + "/";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    auto write = [&](const std::string& text) {
        FILE* f = std::fopen((dir + "input.cfg").c_str(), "wb");
        CHECK(f != nullptr);
        if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    };
    // The version 5 defaults as 0.2.0 wrote them: ours, so upgraded (F1 becomes the menu; the old file is kept).
    std::string v5 = "# version: 5\n";
    {
        Bindings old = historical_default_sets()[1];
        std::string line;
        for (std::istringstream in(format_bindings(old)); std::getline(in, line);) {
            if (line.empty() || line[0] == '#' || line.compare(0, 4, "pad_") == 0) continue;
            if (line.find('=') + 1 == line.size()) continue;
            v5 += line + "\n";
        }
    }
    write(v5);
    LoadedBindings up = preview_bindings(dir);
    CHECK(up.state == CfgState::WouldUpgrade);
    CHECK(up.bindings.keys[HostMenu] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    CHECK(up.bindings.keys[HostFaithful].empty());
    // The same file with one edit is the user's: it keeps its own F1 as the Faithful switch, and the menu has no key (with a note that says why).
    write(v5 + "p1_fire = Return\n");
    LoadedBindings kept = preview_bindings(dir);
    CHECK(kept.state == CfgState::Customised);
    CHECK(kept.bindings.keys[HostFaithful] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    CHECK(kept.bindings.keys[HostMenu].empty());
    CHECK(kept.note.find("F1") != std::string::npos && kept.note.find("faithful") != std::string::npos);
    CHECK(find_binding_conflict(kept.bindings, HostMenu, SDL_SCANCODE_F1) == HostFaithful);
    // A customised file that left F1 free gives it to the menu.
    std::string moved;
    for (std::istringstream in(v5 + "p1_fire = Return\n"); !in.eof();) {
        std::string line;
        std::getline(in, line);
        moved += (line == "faithful = F1" ? std::string("faithful = F8") : line) + "\n";
    }
    for (size_t at = moved.find("volume_up = F8"); at != std::string::npos; at = moved.find("volume_up = F8", at)) moved.replace(at, 14, "volume_up = F7");
    for (size_t at = moved.find("volume_down = F7"); at != std::string::npos; at = moved.find("volume_down = F7", at)) moved.replace(at, 16, "volume_down = Q");
    write(moved);
    LoadedBindings free_f1 = preview_bindings(dir);
    CHECK(free_f1.state == CfgState::Customised);
    CHECK(free_f1.bindings.keys[HostMenu] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    CHECK(free_f1.bindings.keys[HostFaithful] == std::vector<SDL_Scancode>{SDL_SCANCODE_F8});
    // A file that names `menu` itself is taken as it is (even when that is a conflict: the Controls page is where those are resolved).
    write(v5 + "p1_fire = Return\nmenu = F2\n");
    LoadedBindings own = preview_bindings(dir);
    CHECK(own.bindings.keys[HostMenu] == std::vector<SDL_Scancode>{SDL_SCANCODE_F2});
    std::filesystem::remove_all(dir, ec);
}

// Every set of defaults we have written must still be recognised as ours, so each format bump reaches the
// files we wrote at the previous one. Built from historical_default_sets() rather than from a literal, so it
// cannot bake in one platform's bindings -- the mistake that broke CI at version 3.
void test_version_bump_reaches_our_files() {
    CHECK(kBindingsVersion == 6);
    std::string line;
    // The file as that version really wrote it: no comments, so it declares no version, and no line for an
    // action it did not have yet (format_bindings would emit those as "action =" with no keys).
    auto body = [&](const Bindings& b) {
        std::string text;
        for (std::istringstream in(format_bindings(b)); std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            if (line.find('=') + 1 == line.size()) continue; // an action with no keys: that version lacked it
            text += line + "\n";
        }
        return text;
    };
    const std::vector<Bindings> sets = historical_default_sets();
    CHECK(sets.size() >= 6); // current, v5, v4, v3, v2 (plus the Windows/Linux Ctrl-first variant), v1
    for (const Bindings& b : sets) CHECK(classify_bindings_file(body(b)) == CfgAction::Upgrade);

    // The version 5 set is the one 0.2.0 shipped: it differs from the current table by F1 (the Faithful switch there, the menu now), and a
    // file holding it must be upgraded rather than taken for the user's own.
    const Bindings& v5 = sets[1];
    CHECK(v5.keys[HostFaithful] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    CHECK(v5.keys[HostMenu].empty());
    CHECK(default_bindings().keys[HostFaithful].empty());
    CHECK(default_bindings().keys[HostMenu] == std::vector<SDL_Scancode>{SDL_SCANCODE_F1});
    for (int a = 0; a < kActionCount; ++a)
        if (a != HostFaithful && a != HostMenu) CHECK(v5.keys[size_t(a)] == default_bindings().keys[size_t(a)]);
    CHECK(classify_bindings_file("# version: 5\n" + body(v5)) == CfgAction::Upgrade);
    CHECK(classify_bindings_file("# version: 5\n" + body(v5) + "p1_fire = Return\n") == CfgAction::Keep);
    // The version 4 set: before the second button as well.
    const Bindings& v4 = sets[2];
    CHECK(v4.keys[HostButton2].empty());
    CHECK(!default_bindings().keys[HostButton2].empty());
    for (int a = 0; a < kActionCount; ++a)
        if (a != HostButton2 && a != HostFaithful && a != HostMenu) CHECK(v4.keys[size_t(a)] == default_bindings().keys[size_t(a)]);
    CHECK(classify_bindings_file("# version: 4\n" + body(v4)) == CfgAction::Upgrade);
    CHECK(classify_bindings_file("# version: 4\n" + body(v4) + "p1_fire = Return\n") == CfgAction::Keep);
    // The version 3 set is the one the assists shipped in: it lacks the protect-music toggle and the second button.
    const Bindings& v3 = sets[3];
    CHECK(v3.keys[HostProtectMusic].empty());
    CHECK(v3.keys[HostButton2].empty());
    CHECK(!default_bindings().keys[HostProtectMusic].empty());
    for (int a = 0; a < kActionCount; ++a)
        if (a != HostProtectMusic && a != HostButton2 && a != HostFaithful && a != HostMenu)
            CHECK(v3.keys[size_t(a)] == default_bindings().keys[size_t(a)]);
    CHECK(classify_bindings_file("# version: 3\n" + body(v3)) == CfgAction::Upgrade);
    // ... and a version 3 file the user edited stays theirs, with the new action defaulting in.
    CHECK(classify_bindings_file("# version: 3\n" + body(v3) + "p1_fire = Return\n") == CfgAction::Keep);
    Bindings loaded = default_bindings();
    std::string err;
    CHECK(parse_bindings(body(v3), loaded, err));
    CHECK(err.empty());
    CHECK(loaded.keys[HostProtectMusic] == default_bindings().keys[HostProtectMusic]);
    CHECK(loaded.keys[HostButton2] == default_bindings().keys[HostButton2]);
}

// input.cfg the user already has, written by the version that fixed the macOS bindings. It must be
// recognised as ours and upgraded, or they would never get the assist and cheat bindings. This is the exact
// text from the reporter's Mac, "Left Option" spelling and all.
const char* const kVersion2File =
    "# x2recomp input bindings: action = key, key (SDL scancode names). Delete this file to\n"
    "# restore the defaults. Unbound keys go to the game as Amiga keys (P pause, ESC menu).\n"
    "# version: 2 -- a file you have edited is never changed by the game; new defaults in a later\n"
    "# version are only taken up by a file that still holds the old ones (kept as input.cfg.bak).\n"
    "p2_up = Up\n"
    "p2_down = Down\n"
    "p2_left = Left\n"
    "p2_right = Right\n"
#ifdef __APPLE__
    "p2_fire = Z, X, Left Option, Right Option\n"
#else
    "p2_fire = Left Ctrl, Right Ctrl, Z\n"
#endif
    "p1_up = W\n"
    "p1_down = S\n"
    "p1_left = A\n"
    "p1_right = D\n"
    "p1_fire = Left Shift\n"
    "pause = Pause, F9\n"
    "reset = F12\n"
    "quit = F10\n"
    "fullscreen = F11\n"
    "volume_down = F7\n"
    "volume_up = F8\n"
    "scale_down = F5\n"
    "scale_up = F6\n"
    "faithful = F1\n"
    "filter = F2\n"
    "interp = F3\n"
    "aspect = F4\n"
    "asset_packs = Home\n"
    "wide = End\n"
    "hd_panel = PageDown\n"
    "music_source = PageUp\n"
    "grab = ScrollLock\n";

void test_version2_migration() {
    CHECK(bindings_version(kVersion2File) == 2);
    CHECK(kBindingsVersion > 2); // version 2 is history; this test is about reaching the files it wrote
    // Ours, so it is upgraded and picks up autofire, god_mode and add_credit.
    CHECK(classify_bindings_file(kVersion2File) == CfgAction::Upgrade);
    CHECK(!default_bindings().keys[HostAutofire].empty());
    CHECK(!default_bindings().keys[HostGodMode].empty());
    CHECK(!default_bindings().keys[HostAddCredit].empty());
    // A version 2 file the user edited is still theirs.
    CHECK(classify_bindings_file(std::string(kVersion2File) + "p1_fire = Return\n") == CfgAction::Keep);
    // A version 2 file that already names a version 3 action was hand written, so it is left alone.
    CHECK(classify_bindings_file(std::string(kVersion2File) + "autofire = Q\n") == CfgAction::Keep);
    // A current file is never touched.
    CHECK(classify_bindings_file(format_bindings(default_bindings())) == CfgAction::Keep);
    // A file missing the new actions still works: the defaults apply to whatever it does not mention.
    Bindings b = default_bindings();
    std::string err;
    CHECK(parse_bindings(kVersion2File, b, err));
    CHECK(err.empty());
    CHECK(b.keys[HostAutofire] == default_bindings().keys[HostAutofire]);
}

// The file every release up to 0.1.0 wrote (the one on the reporter's Mac) must be recognised and upgraded;
// a file the user has edited must be left alone.
const char* const kUserFile =
    "# x2recomp input bindings: action = key, key (SDL scancode names). Delete this file to\n"
    "# restore the defaults. Unbound keys go to the game as Amiga keys (P pause, ESC menu).\n"
    "p2_up = Up\n"
    "p2_down = Down\n"
    "p2_left = Left\n"
    "p2_right = Right\n"
    "p2_fire = Left Ctrl, Right Ctrl, Z\n"
    "p1_up = W\n"
    "p1_down = S\n"
    "p1_left = A\n"
    "p1_right = D\n"
    "p1_fire = Left Shift\n"
    "pause = Pause, F9\n"
    "reset = F12\n"
    "quit = F10\n"
    "fullscreen = F11\n";

void test_cfg_migration() {
    std::string line;
    CHECK(bindings_version(kUserFile) == 1);
    CHECK(bindings_version(format_bindings(default_bindings())) == kBindingsVersion);
    CHECK(classify_bindings_file(kUserFile) == CfgAction::Upgrade);
    CHECK(classify_bindings_file(format_bindings(default_bindings())) == CfgAction::Keep);
    // The version marker does not change the answer: what decides is whether the bindings are ones we wrote.
    // (This file carries the version 1 bindings, which on Windows and Linux are also a version 2 set, so
    // asserting one platform's outcome here is what broke the build on the others.)
    CHECK(classify_bindings_file("# version: 2\n" + std::string(kUserFile)) == CfgAction::Upgrade);
    // A file declaring a format version from the future is left alone whatever it holds.
    CHECK(classify_bindings_file("# version: 99\n" + std::string(kUserFile)) == CfgAction::Keep);
    // A full version 1 file: every action the 0.1.0 release knew, with no version marker.
    std::string legacy;
    for (std::istringstream in(format_bindings(legacy_default_bindings())); std::getline(in, line);)
        if (line.empty() || line[0] != '#') legacy += line + "\n";
    CHECK(bindings_version(legacy) == 1);
    CHECK(classify_bindings_file(legacy) == CfgAction::Upgrade);

    std::string custom = std::string(kUserFile) + "p2_fire = Space\n";
    CHECK(classify_bindings_file(custom) == CfgAction::Keep);
    std::string one_changed = "p2_up = Up\np1_fire = Return\n";
    CHECK(classify_bindings_file(one_changed) == CfgAction::Keep);
    CHECK(classify_bindings_file("p2_up = Up\nnonsense = Q\n") == CfgAction::Keep); // not understood: never touched
    CHECK(classify_bindings_file("") == CfgAction::Upgrade);                        // nothing to lose

    // Every set we have ever written is recognised as ours, on whatever platform this is.
    for (const Bindings& b : historical_default_sets()) {
        std::string text;
        for (std::istringstream in(format_bindings(b)); std::getline(in, line);)
            if (line.empty() || line[0] != '#') text += line + "\n";
        CHECK(classify_bindings_file(text) == CfgAction::Upgrade);
    }
    // A file holding another platform's defaults is kept, never silently rewritten: its fire keys are not
    // ones this platform ever wrote. (Both spellings of the Option keys, since the file could come from
    // either a running game or --list-keys.)
    for (const char* other :
#ifdef __APPLE__
         {"p2_fire = Left Ctrl, Right Ctrl, Z\n", "p2_fire = Left Ctrl, Right Ctrl, Z\n"}
#else
         {"p2_fire = Z, X, Left Alt, Right Alt\n", "p2_fire = Z, X, Left Option, Right Option\n"}
#endif
    ) {
        std::string text;
        for (std::istringstream in(format_bindings(default_bindings())); std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            text += line.compare(0, 9, "p2_fire =") == 0 ? std::string(other) : line + "\n";
        }
        CHECK(classify_bindings_file(text) == CfgAction::Keep);
    }
}

void test_load_bindings() {
    const std::string dir = (std::filesystem::temp_directory_path() / "x2_input_cfg_test").string() + "/";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    CHECK(!ec);
    auto write = [&](const std::string& name, const std::string& text) {
        FILE* f = std::fopen((dir + name).c_str(), "wb");
        CHECK(f != nullptr);
        if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    };
    auto read = [&](const std::string& name) {
        std::string text;
        if (FILE* f = std::fopen((dir + name).c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    };
    std::remove((dir + "input.cfg").c_str());
    std::remove((dir + "input.cfg.bak").c_str());

    LoadedBindings fresh = load_bindings(dir); // no file: written with the defaults
    CHECK(fresh.error.empty() && !fresh.note.empty());
    CHECK(fresh.bindings.keys[P2Fire] == default_bindings().keys[P2Fire]);
    CHECK(bindings_version(read("input.cfg")) == kBindingsVersion);

    LoadedBindings again = load_bindings(dir); // the current version: used as it is, no note
    CHECK(again.error.empty() && again.note.empty());
    CHECK(again.bindings.keys[P2Fire] == default_bindings().keys[P2Fire]);

    write("input.cfg", kUserFile); // the reporter's file: upgraded, the old one kept
    LoadedBindings up = load_bindings(dir);
    CHECK(up.bindings.keys[P2Fire] == default_bindings().keys[P2Fire]);
    CHECK(up.note.find("upgraded") != std::string::npos);
    CHECK(read("input.cfg.bak") == kUserFile);
    CHECK(bindings_version(read("input.cfg")) == kBindingsVersion);

    const std::string custom = std::string(kUserFile) + "p1_fire = Return\n";
    write("input.cfg", custom);
    LoadedBindings kept = load_bindings(dir);
    CHECK(kept.bindings.keys[P1Fire].size() == 1 && kept.bindings.keys[P1Fire][0] == SDL_SCANCODE_RETURN);
    CHECK(kept.bindings.keys[P2Fire][0] == SDL_SCANCODE_LCTRL); // the file's own choice, Ctrl and all
    CHECK(!kept.note.empty());
    CHECK(read("input.cfg") == custom); // never rewritten behind the user's back
#ifdef __APPLE__
    CHECK(kept.note.find("Ctrl") != std::string::npos); // a kept file that still fires on Ctrl says so
#endif

    CHECK(load_bindings("").path.empty());

    // --list-keys must say what the NEXT start will do and show the bindings that will apply, without
    // touching anything: the reporter was testing against a file that was still on the old defaults.
    write("input.cfg", kUserFile);
    std::remove((dir + "input.cfg.bak").c_str());
    LoadedBindings pre = preview_bindings(dir);
    CHECK(pre.state == CfgState::WouldUpgrade);
    CHECK(pre.note.find("upgrade") != std::string::npos);
    CHECK(pre.note.find(std::to_string(kBindingsVersion)) != std::string::npos);
    CHECK(pre.bindings.keys[P2Fire] == default_bindings().keys[P2Fire]); // the NEW bindings, not the file's
    CHECK(read("input.cfg") == kUserFile);                               // read-only
    CHECK(read("input.cfg.bak").empty());                                // no backup written either

    write("input.cfg", custom); // a file of their own: shown as it is, still read-only
    LoadedBindings pre_custom = preview_bindings(dir);
    CHECK(pre_custom.state == CfgState::Customised);
    CHECK(pre_custom.bindings.keys[P1Fire].size() == 1 && pre_custom.bindings.keys[P1Fire][0] == SDL_SCANCODE_RETURN);
    CHECK(read("input.cfg") == custom);

    write("input.cfg", format_bindings(default_bindings())); // current version: nothing to report
    LoadedBindings pre_current = preview_bindings(dir);
    CHECK(pre_current.state == CfgState::Current);
    CHECK(pre_current.note.empty());

    std::remove((dir + "input.cfg").c_str());
    LoadedBindings pre_none = preview_bindings(dir);
    CHECK(pre_none.state == CfgState::NoFile);
    CHECK(!pre_none.note.empty());
    CHECK(pre_none.bindings.keys[P2Fire] == default_bindings().keys[P2Fire]);
    CHECK(read("input.cfg").empty()); // preview never creates it

    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    test_rawkeys();
    test_bindings_roundtrip();
    test_key_names_are_stable();
    test_parse_rebind();
    test_events_to_machine();
    test_platform_defaults();
    test_gui_hotkeys();
    test_tap_not_lost();
    test_focus_clears_held();
    test_grab_policy();
    test_cursor_policy();
    test_autofire_policy();
    test_window_mode_suffix();
    test_protect_music_action();
    test_button2_action();
    test_menu_action();
    test_pad_bindings();
    test_binding_helpers();
    test_menu_key_migration();
    test_version_bump_reaches_our_files();
    test_version2_migration();
    test_cfg_migration();
    test_load_bindings();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
