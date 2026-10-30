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
    CHECK(binds(P2Fire, SDL_SCANCODE_LCTRL) && binds(P2Fire, SDL_SCANCODE_Z));
#endif
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
    CHECK(host_command_for_gui(SDL_SCANCODE_1) == HostCommand::Faithful);
    CHECK(host_command_for_gui(SDL_SCANCODE_9) == HostCommand::Pause);
    CHECK(host_command_for_gui(SDL_SCANCODE_G) == HostCommand::Grab);
    CHECK(host_command_for_gui(SDL_SCANCODE_W) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_M) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_H) == HostCommand::None);
    CHECK(host_command_for_gui(SDL_SCANCODE_J) == HostCommand::None);
#ifdef __APPLE__
    CHECK(cmd(SDL_SCANCODE_F, SDL_KMOD_LGUI) == HostCommand::Fullscreen);
    CHECK(cmd(SDL_SCANCODE_1, SDL_KMOD_LGUI) == HostCommand::Faithful);
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
    // The same bindings in a file that carries the version marker are the user's business, not ours.
    CHECK(classify_bindings_file("# version: 2\n" + std::string(kUserFile)) == CfgAction::Keep);
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
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    test_rawkeys();
    test_bindings_roundtrip();
    test_parse_rebind();
    test_events_to_machine();
    test_platform_defaults();
    test_gui_hotkeys();
    test_tap_not_lost();
    test_focus_clears_held();
    test_grab_policy();
    test_cfg_migration();
    test_load_bindings();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
