// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The settings schema: one row for every key of settings.cfg (runtime/persist.h) saying what it is called in the launcher, how it is
// edited, which releases honour it, when a change takes effect, which page shows it and how the command line says the same thing. The
// pages, the "equivalent command line" footer and the tests all read this table, so a setting is added in one place; the integrity test
// fails when a key of settings.cfg has no row. docs/re-notes/gui-options-schema.md has the design.
//
// The schema knows Settings and nothing of the window: it builds without SDL and without the widget kit.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "persist.h"

namespace x2::ui {

enum class GameKind { Disk, Cdtv, St };
enum GameBit : unsigned { kDisk = 1, kCdtv = 2, kSt = 4, kAllGames = 7 };
inline unsigned game_bit(GameKind g) { return 1u << static_cast<unsigned>(g); }
const char* game_name(GameKind g);        // "Amiga disk", "Amiga CDTV", "Atari ST"
const char* game_short_name(GameKind g);  // "disk", "CDTV", "ST"
const char* game_flag_value(GameKind g);  // the --game value: "disk", "cdtv", "st"

enum class SettingType { Bool, Int, Float, Choice, Text };
// Now: a running game takes the change at once (the same code path as the hotkey). Restart: from the next start.
enum class Effect { Now, Restart };
enum PageBit : unsigned { kPagePlay = 1, kPageDisplay = 2, kPageAudio = 4, kPageControls = 8, kPagePacks = 16, kPageAdvanced = 32 };

struct Choice {
  const char* value;  // as settings.cfg spells it
  const char* label;  // as the page shows it
};

// How the command line says a non-default value. The renderer composes the command from the difference to the defaults.
enum class CliKind {
  None,       // settings.cfg only (or another workstream's flag: see `note`)
  Value,      // --flag VALUE
  Switch,     // --flag turns it on (off is the default, so there is nothing to say)
  NoSwitch,   // --no-flag turns it off (on is the default)
  Pair,       // --flag on, --no-flag off (the default decides which one is ever written)
  Modern,     // an item of --modern, written when the value is not the default: `item` (on) or `item_off` (off)
  ModernVal,  // an item of --modern with a value: item + value ("scale=" + "4")
  Repeat,     // --flag PATH for each element of a ';' list
  Map,        // a choice whose values have flags of their own: choices[i].value -> map_flags[i] ("" = nothing to say)
  Autofire,   // --autofire[=RATE]: the autofire row writes the rate too
};
struct CliForm {
  CliKind kind = CliKind::None;
  const char* flag = "";      // the flag without its leading dashes ("filter")
  const char* item = "";      // Modern/ModernVal: the --modern item; Modern: written when the value is on
  const char* item_off = "";  // Modern: written when the value is off
  const char* const* map_flags = nullptr;  // Map
};

struct SettingRow {
  const char* key;  // settings.cfg key
  SettingType type;
  const char* label;
  const char* help;  // one line
  unsigned games = kAllGames;
  Effect effect = Effect::Now;
  unsigned pages = 0;  // PageBit mask; 0 = in settings.cfg only
  const char* group = "";
  bool expert = false;   // shown only when the page's "Expert" disclosure is open
  bool cheat = false;    // a cheat when it is on (credits: above 3, see is_cheat_active)
  bool picture = false;  // a picture option: the Faithful switch overrides it
  int lo = 0, hi = 0, step = 1;     // Int
  float flo = 0, fhi = 1, fstep = 0.05f;  // Float
  const char* unit = "";            // Int/Float: "%", "x", " ms"
  const Choice* choices = nullptr;  // Choice
  int nchoices = 0;
  CliForm cli;
  const char* action = "";  // the input.cfg action that does the same, for the hotkey hint ("filter")
  const char* note = "";    // about the command line when it cannot say everything
};

const std::vector<SettingRow>& schema();
const SettingRow* find_row(std::string_view key);
// Rows of a page in schema order.
std::vector<const SettingRow*> rows_for_page(PageBit page);

// ---- values (a Settings in, text or typed values out) ----
// What settings.cfg says for the row ("on", "crt", "16:9"): the canonical spelling.
std::string value_text(const SettingRow& r, const Settings& s);
// parse_settings on a single `key = text` line: false and a reason in `err` when the file would refuse it. `s` is untouched then.
bool set_value_text(const SettingRow& r, Settings& s, const std::string& text, std::string& err);
bool get_bool(const SettingRow& r, const Settings& s);
void set_bool(const SettingRow& r, Settings& s, bool v);
int get_int(const SettingRow& r, const Settings& s);
void set_int(const SettingRow& r, Settings& s, int v);   // clamped to lo..hi
float get_float(const SettingRow& r, const Settings& s);
void set_float(const SettingRow& r, Settings& s, float v);
// Index into choices, or -1 when the value is none of them (a surround given as pixels: wide = 40).
int get_choice(const SettingRow& r, const Settings& s);
void set_choice(const SettingRow& r, Settings& s, int index);

// ---- what applies ----
inline bool applies_to(const SettingRow& r, GameKind g) { return (r.games & game_bit(g)) != 0; }
// "" when the row can be changed; otherwise why not (it is for other releases, another setting has to be on first, the Faithful switch).
std::string disabled_reason(const SettingRow& r, const Settings& s, GameKind g);
// A cheat while active: god mode, and starting credits above the game's own 3.
bool is_cheat_active(const SettingRow& r, const Settings& s);

// ---- the command line ----
struct CommandLine {
  std::vector<std::string> args;   // without the program name
  std::vector<std::string> notes;  // what the command line cannot say ("packs_enabled is in settings.cfg only")
  std::string text(const char* program = "x2recomp") const;
};
// The command that starts `game` with `s` on a clean settings file: every row that differs from Settings{} and that `game` honours.
// `with_game` always writes --game (the launcher shows what Play does); otherwise it is written only for a non-default `game` setting.
CommandLine command_line(const Settings& s, GameKind game, bool with_game);

}  // namespace x2::ui
