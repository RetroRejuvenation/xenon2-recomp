// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "settings_schema.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace x2::ui {

const char* game_name(GameKind g) {
  switch (g) {
    case GameKind::Disk: return "Amiga disk";
    case GameKind::Cdtv: return "Amiga CDTV";
    case GameKind::St: return "Atari ST";
  }
  return "?";
}
const char* game_short_name(GameKind g) {
  switch (g) {
    case GameKind::Disk: return "disk";
    case GameKind::Cdtv: return "CDTV";
    case GameKind::St: return "ST";
  }
  return "?";
}
const char* game_flag_value(GameKind g) {
  switch (g) {
    case GameKind::Disk: return "disk";
    case GameKind::Cdtv: return "cdtv";
    case GameKind::St: return "st";
  }
  return "?";
}

namespace {

// ---- the fields of Settings a row reads and writes ----
struct Field {
  enum Kind { B, I, F, S, AudioFilter, VideoFilter } kind;
  bool Settings::*b = nullptr;
  int Settings::*i = nullptr;
  float Settings::*f = nullptr;
  std::string Settings::*s = nullptr;
  x2::FilterSetting Settings::*af = nullptr;
  x2::VideoFilter Settings::*vf = nullptr;
};
Field fb(bool Settings::*p) { Field x{Field::B}; x.b = p; return x; }
Field fi(int Settings::*p) { Field x{Field::I}; x.i = p; return x; }
Field ff(float Settings::*p) { Field x{Field::F}; x.f = p; return x; }
Field fs(std::string Settings::*p) { Field x{Field::S}; x.s = p; return x; }

const std::map<std::string, Field>& fields() {
  static const std::map<std::string, Field> m = {
      {"scale", fi(&Settings::scale)},
      {"fullscreen", fb(&Settings::fullscreen)},
      {"aspect", fb(&Settings::aspect)},
      {"audio", fb(&Settings::audio)},
      {"audio_filter", [] { Field x{Field::AudioFilter}; x.af = &Settings::filter; return x; }()},
      {"stereo_separation", ff(&Settings::stereo_separation)},
      {"volume", fi(&Settings::volume)},
      {"audio_latency", fi(&Settings::audio_latency)},
      {"high_scores", fb(&Settings::high_scores)},
      {"grab_keyboard", fs(&Settings::grab_keyboard)},
      {"cursor", fs(&Settings::cursor)},
      {"input_debug", fb(&Settings::input_debug)},
      {"modern", fb(&Settings::faithful)},
      {"interp", fb(&Settings::interp)},
      {"interp_scale", fi(&Settings::interp_scale)},
      {"smooth_far", fb(&Settings::smooth_far)},
      {"filter", [] { Field x{Field::VideoFilter}; x.vf = &Settings::video_filter; return x; }()},
      {"integer", fb(&Settings::integer)},
      {"curvature", fb(&Settings::curvature)},
      {"upgrade", fb(&Settings::upgrade)},
      {"asset_packs", fs(&Settings::asset_packs)},
      {"audio_upgrade", fb(&Settings::audio_upgrade)},
      {"packs_enabled", fs(&Settings::packs_enabled)},
      {"hd_panel", fb(&Settings::hd_panel)},
      {"wide", fs(&Settings::wide)},
      {"music", fs(&Settings::music)},
      {"music_volume", fi(&Settings::music_volume)},
      {"sfx_volume", fi(&Settings::sfx_volume)},
      {"speech_volume", fi(&Settings::speech_volume)},
      {"protect_music", fb(&Settings::protect_music)},
      {"cd_crossfade", fi(&Settings::cd_crossfade)},
      {"cd_pause", fs(&Settings::cd_pause)},
      {"cdtv", fs(&Settings::cdtv)},
      {"cdtv_tracks", fs(&Settings::cdtv_tracks)},
      {"autofire", fb(&Settings::autofire)},
      {"autofire_rate", fi(&Settings::autofire_rate)},
      {"god_mode", fb(&Settings::god_mode)},
      {"credits", fi(&Settings::credits)},
      {"cheat_scores", fb(&Settings::cheat_scores)},
      {"game", fs(&Settings::game)},
      {"launcher", fs(&Settings::launcher)},
      {"ui_text", fs(&Settings::ui_text)},
      {"ui_contrast", fs(&Settings::ui_contrast)},
      {"ui_motion", fs(&Settings::ui_motion)},
      {"last_game", fs(&Settings::last_game)},
  };
  return m;
}

const Field& field_of(const SettingRow& r) { return fields().at(r.key); }

// ---- the choice lists ----
constexpr Choice kGameChoices[] = {{"auto", "Automatic"}, {"disk", "Amiga disk"}, {"cdtv", "Amiga CDTV"}, {"st", "Atari ST"}};
constexpr Choice kLauncherChoices[] = {{"auto", "Automatic"}, {"always", "Always"}, {"never", "Never"}};
constexpr Choice kFilterChoices[] = {{"nearest", "nearest"}, {"sharp", "sharp"}, {"scanlines", "scanlines"}, {"crt", "crt"}, {"edge", "edge"}};
constexpr Choice kWideChoices[] = {{"off", "off"}, {"16:10", "16:10"}, {"16:9", "16:9"}, {"21:9", "21:9"}, {"32:9", "32:9"}};
constexpr Choice kAudioFilterChoices[] = {{"a500", "A500"}, {"a1200", "A1200"}, {"none", "none"}};
constexpr Choice kMusicChoices[] = {{"amiga", "Tracker"}, {"cdtv", "CD tracks"}, {"mix", "Both"}};
constexpr Choice kCdPauseChoices[] = {{"keep", "Keep"}, {"duck", "Quieter"}, {"stop", "Stop"}};
constexpr Choice kGrabChoices[] = {{"auto", "auto"}, {"on", "on"}, {"off", "off"}};
constexpr Choice kCursorChoices[] = {{"auto", "auto"}, {"show", "show"}, {"hide", "hide"}};
constexpr Choice kTextChoices[] = {{"normal", "Normal"}, {"large", "Large"}};
constexpr Choice kContrastChoices[] = {{"normal", "Normal"}, {"high", "High"}};
constexpr Choice kMotionChoices[] = {{"auto", "System"}, {"full", "Full"}, {"reduced", "Reduced"}};
const char* const kGrabFlags[] = {"", "--grab-keyboard", "--no-grab-keyboard"};

struct RowBuilder {
  SettingRow r{};
  RowBuilder(const char* key, SettingType t, const char* label, const char* help) {
    r.key = key;
    r.type = t;
    r.label = label;
    r.help = help;
  }
  RowBuilder& games(unsigned g) { r.games = g; return *this; }
  RowBuilder& restart() { r.effect = Effect::Restart; return *this; }
  RowBuilder& pages(unsigned p) { r.pages = p; return *this; }
  RowBuilder& group(const char* g) { r.group = g; return *this; }
  RowBuilder& expert() { r.expert = true; return *this; }
  RowBuilder& cheat() { r.cheat = true; return *this; }
  RowBuilder& picture() { r.picture = true; return *this; }
  RowBuilder& range(int lo, int hi, int step, const char* unit = "") { r.lo = lo; r.hi = hi; r.step = step; r.unit = unit; return *this; }
  RowBuilder& frange(float lo, float hi, float step) { r.flo = lo; r.fhi = hi; r.fstep = step; return *this; }
  template <size_t N>
  RowBuilder& choices(const Choice (&a)[N]) { r.choices = a; r.nchoices = static_cast<int>(N); return *this; }
  RowBuilder& cli(CliKind k, const char* flag = "", const char* item = "", const char* item_off = "") {
    r.cli.kind = k; r.cli.flag = flag; r.cli.item = item; r.cli.item_off = item_off;
    return *this;
  }
  RowBuilder& map_flags(const char* const* f) { r.cli.kind = CliKind::Map; r.cli.map_flags = f; return *this; }
  RowBuilder& action(const char* a) { r.action = a; return *this; }
  RowBuilder& note(const char* n) { r.note = n; return *this; }
  operator SettingRow() const { return r; }
};

constexpr unsigned kAmiga = kDisk | kCdtv;

std::vector<SettingRow> build_schema() {
  using T = SettingType;
  std::vector<SettingRow> v;
  auto add = [&](RowBuilder b) { v.push_back(b.r); };

  // ---- Play ----
  add(RowBuilder("game", T::Choice, "Start this release", "Which imported release starts. Automatic takes the disk release, then CDTV, then Atari ST.")
          .choices(kGameChoices).restart().pages(kPagePlay).group("Start").cli(CliKind::Value, "game"));
  add(RowBuilder("launcher", T::Choice, "Show this launcher", "Automatic shows it when x2recomp is started without arguments. Never starts the game straight away.")
          .choices(kLauncherChoices).restart().pages(kPagePlay).group("Start")
          .note("the launcher setting is kept in settings.cfg"));
  add(RowBuilder("high_scores", T::Bool, "Keep high scores", "Saves the high-score table between runs.")
          .pages(kPagePlay).group("Scores").cli(CliKind::NoSwitch, "no-persist"));
  add(RowBuilder("autofire", T::Bool, "Autofire", "Pulses the fire line while fire is held, during a level only. An assist, not a cheat.")
          .pages(kPagePlay | kPageControls).group("Assists").cli(CliKind::Autofire, "autofire").action("autofire"));
  add(RowBuilder("autofire_rate", T::Int, "Autofire rate", "Pulses per second. 17 is the game's own highest rate.")
          .range(1, 25, 1, "/s").pages(kPagePlay | kPageControls).group("Assists").cli(CliKind::Autofire, "autofire"));
  add(RowBuilder("god_mode", T::Bool, "God mode", "CHEAT: the ship's energy never runs out. No high scores are saved while it is on.")
          .cheat().pages(kPagePlay).group("Cheats").cli(CliKind::Switch, "god-mode").action("god_mode"));
  add(RowBuilder("credits", T::Int, "Starting credits", "Credits a new game starts with. The game's own value is 3; more is a cheat.")
          .range(1, 9, 1).pages(kPagePlay).group("Cheats").cli(CliKind::Value, "credits"));
  add(RowBuilder("cheat_scores", T::Bool, "Save scores anyway", "Saves high scores even in a session that used a cheat.")
          .pages(kPagePlay).group("Cheats").cli(CliKind::Switch, "cheat-scores"));

  // ---- Display ----
  add(RowBuilder("modern", T::Bool, "Faithful switch", "Original picture only. Overrides every other picture option.")
          .pages(kPageDisplay).group("Mode").cli(CliKind::Switch, "faithful").action("faithful"));
  add(RowBuilder("scale", T::Int, "Window size", "Multiples of the original picture.")
          .range(1, 8, 1, "x").pages(kPageDisplay).group("Window").cli(CliKind::Value, "scale").action("scale_up"));
  add(RowBuilder("fullscreen", T::Bool, "Fullscreen", "Use the whole display.")
          .pages(kPageDisplay).group("Window").cli(CliKind::Switch, "fullscreen").action("fullscreen"));
  add(RowBuilder("filter", T::Choice, "Filter", "How the picture is scaled: nearest, sharp, scanlines, crt or edge.")
          .choices(kFilterChoices).picture().pages(kPageDisplay).group("Picture").cli(CliKind::Value, "filter").action("filter"));
  add(RowBuilder("curvature", T::Bool, "CRT curvature", "Curves the picture like a tube.")
          .picture().pages(kPageDisplay).group("Picture").cli(CliKind::Switch, "curvature"));
  add(RowBuilder("aspect", T::Bool, "4:3 picture", "PAL pixels are 16:15 wide, so 4:3 is the original shape. Off gives square pixels.")
          .picture().pages(kPageDisplay).group("Picture").cli(CliKind::Pair, "aspect").action("aspect"));
  add(RowBuilder("integer", T::Bool, "Whole-pixel scale", "Only whole-number scale factors, so every pixel is the same size.")
          .picture().pages(kPageDisplay).group("Picture").cli(CliKind::NoSwitch, "no-integer"));
  add(RowBuilder("interp", T::Bool, "Smooth frames", "Interpolated frames at the display rate.")
          .games(kAmiga).picture().pages(kPageDisplay).group("Smoothing").cli(CliKind::Modern, "", "interp").action("interp"));
  add(RowBuilder("interp_scale", T::Int, "Smooth size", "Canvas scale for smooth frames and upgraded graphics.")
          .range(1, 8, 1, "x").picture().pages(kPageDisplay).group("Smoothing").cli(CliKind::ModernVal, "", "scale="));
  add(RowBuilder("smooth_far", T::Bool, "Smooth far layer", "Glides the far background between frames.")
          .games(kAmiga).picture().pages(kPageDisplay).group("Smoothing").cli(CliKind::Modern, "", "", "nosmooth"));
  add(RowBuilder("wide", T::Choice, "Widescreen", "Extra picture beside the original, and a wider window.")
          .games(kAmiga).choices(kWideChoices).picture().pages(kPageDisplay).group("Smoothing")
          .cli(CliKind::ModernVal, "", "wide=").action("wide"));
  add(RowBuilder("hd_panel", T::Bool, "HD status panel", "A sharper status panel.")
          .games(kAmiga).picture().pages(kPageDisplay).group("Smoothing").cli(CliKind::Modern, "", "hdpanel").action("hd_panel"));
  add(RowBuilder("upgrade", T::Bool, "Asset packs", "Uses replacement graphics and sound from the enabled packs.")
          .picture().pages(kPageDisplay | kPagePacks).group("Graphics").cli(CliKind::Modern, "", "upgrade").action("asset_packs"));
  add(RowBuilder("ui_text", T::Choice, "Text size", "The launcher and menu text.")
          .choices(kTextChoices).pages(kPageDisplay).group("This menu").note("the menu settings are kept in settings.cfg"));
  add(RowBuilder("ui_contrast", T::Choice, "Contrast", "High contrast is white on black with a yellow focus.")
          .choices(kContrastChoices).pages(kPageDisplay).group("This menu").note("the menu settings are kept in settings.cfg"));
  add(RowBuilder("ui_motion", T::Choice, "Motion", "Reduced motion removes slides, the starfield and moving stripes. System follows your device.")
          .choices(kMotionChoices).pages(kPageDisplay).group("This menu").note("the menu settings are kept in settings.cfg"));

  // ---- Audio ----
  add(RowBuilder("audio", T::Bool, "Sound", "Turns all sound on or off.")
          .pages(kPageAudio).group("Volume").cli(CliKind::NoSwitch, "no-audio"));
  add(RowBuilder("volume", T::Int, "Master volume", "Overall volume.")
          .range(0, 100, 5, "%").pages(kPageAudio).group("Volume").cli(CliKind::Value, "volume").action("volume_up"));
  add(RowBuilder("sfx_volume", T::Int, "Sound effects", "Volume of the effects.")
          .games(kAmiga).range(0, 100, 5, "%").pages(kPageAudio).group("Volume").cli(CliKind::Value, "sfx-volume"));
  add(RowBuilder("music_volume", T::Int, "Music", "Volume of the music, tracker or CD.")
          .games(kAmiga).range(0, 100, 5, "%").pages(kPageAudio).group("Volume").cli(CliKind::Value, "music-volume"));
  add(RowBuilder("speech_volume", T::Int, "Speech", "Volume of the speech. Only the CDTV release has speech.")
          .games(kCdtv).range(0, 100, 5, "%").pages(kPageAudio).group("Volume").cli(CliKind::Value, "speech-volume"));
  add(RowBuilder("audio_filter", T::Choice, "Output filter", "The Amiga model's sound filter: A500, A1200 or none.")
          .games(kAmiga).choices(kAudioFilterChoices).pages(kPageAudio).group("Output").cli(CliKind::Value, "audio-filter"));
  add(RowBuilder("stereo_separation", T::Float, "Stereo separation", "0 is mono, 100 is hard left and right as on the real machine.")
          .games(kAmiga).frange(0.0f, 1.0f, 0.05f).pages(kPageAudio).group("Output").cli(CliKind::Value, "stereo-sep"));
  add(RowBuilder("audio_latency", T::Int, "Output buffer", "How much sound is queued. Raise this if the sound crackles.")
          .range(10, 500, 10, " ms").pages(kPageAudio).group("Output").cli(CliKind::Value, "audio-latency"));
  add(RowBuilder("audio_upgrade", T::Bool, "Asset-pack audio", "Uses replacement sounds and music from enabled packs that have them.")
          .games(kAmiga).pages(kPageAudio).group("Output").cli(CliKind::Modern, "", "", "noaudio"));
  add(RowBuilder("protect_music", T::Bool, "Protect the tune", "Not what the original does: heavy firing silences a voice of the tune there. On keeps it through the effects.")
          .games(kDisk).pages(kPageAudio).group("Output").cli(CliKind::Switch, "protect-music").action("protect_music"));
  add(RowBuilder("music", T::Choice, "Music source", "Tracker music, the CD tracks of your own CDTV disc, or both.")
          .games(kAmiga).choices(kMusicChoices).pages(kPageAudio).group("CD music").cli(CliKind::Value, "music").action("music_source"));
  add(RowBuilder("cd_crossfade", T::Int, "CD crossfade", "Seamless CD loops and track changes. 0 is as the CDTV game does it.")
          .games(kAmiga).range(0, 500, 50, " ms").pages(kPageAudio).group("CD music").cli(CliKind::Value, "cd-crossfade"));
  add(RowBuilder("cd_pause", T::Choice, "CD during pause", "What the CD does while the game is paused.")
          .games(kAmiga).choices(kCdPauseChoices).pages(kPageAudio).group("CD music").cli(CliKind::Value, "cd-pause"));
  add(RowBuilder("cdtv_tracks", T::Text, "CD track map", "The CD track for the title and levels 1 to 5 of the disk release, for example 2,3,5,2,3,5.")
          .games(kDisk).expert().pages(kPageAudio).group("CD music").cli(CliKind::Value, "cdtv-tracks"));
  add(RowBuilder("cdtv", T::Text, "CD audio location", "Your imported CD audio folder, or a .cue file used in place. Empty means the data folder.")
          .games(kAmiga).expert().pages(kPageAudio).group("CD music").cli(CliKind::Value, "cdtv"));

  // ---- Controls and advanced ----
  add(RowBuilder("grab_keyboard", T::Choice, "Keyboard grab", "Sends the system's own shortcuts to the window while it has the focus.")
          .games(kAmiga).choices(kGrabChoices).pages(kPageControls).group("Keyboard").map_flags(kGrabFlags).action("grab"));
  add(RowBuilder("cursor", T::Choice, "Mouse cursor", "Auto hides it while playing and shows it while paused.")
          .games(kAmiga).choices(kCursorChoices).pages(kPageControls).group("Keyboard").cli(CliKind::Value, "cursor"));
  add(RowBuilder("input_debug", T::Bool, "Input log", "Writes every key, focus change and frame to the console.")
          .pages(kPageAdvanced).group("Diagnostics").cli(CliKind::Switch, "input-debug"));

  // ---- Packs and file-only ----
  add(RowBuilder("asset_packs", T::Text, "Asset pack paths", "Pack folders or .zip files, loaded after the enabled packs; later ones override earlier ones.")
          .pages(kPagePacks).group("Packs").cli(CliKind::Repeat, "asset-pack"));
  add(RowBuilder("packs_enabled", T::Text, "Enabled packs", "The ids of the packs in the data folder's packs folder, in load order.")
          .pages(kPagePacks).group("Packs").note("packs_enabled is in settings.cfg only"));
  add(RowBuilder("last_game", T::Text, "Last played", "Remembered so Play starts it next time.")
          .note("last_game is kept by the launcher in settings.cfg"));
  return v;
}

std::map<std::string, std::string> settings_map(const Settings& s) {
  std::map<std::string, std::string> m;
  std::istringstream in(format_settings(s));
  std::string line;
  while (std::getline(in, line)) {
    std::string body = line;
    if (auto h = body.find('#'); h != std::string::npos) body.resize(h);
    const auto eq = body.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string t) {
      const size_t a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
      return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    m[trim(body.substr(0, eq))] = trim(body.substr(eq + 1));
  }
  return m;
}

std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t;&|<>()$`\\\"'*?[]#~%!{}") == std::string::npos) return a;
  std::string q = "\"";
  for (char c : a) {
    if (c == '"' || c == '\\') q += '\\';
    q += c;
  }
  return q + "\"";
}

std::vector<std::string> split_list(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string p; std::getline(ss, p, sep);)
    if (p.find_first_not_of(" \t") != std::string::npos) out.push_back(p);
  return out;
}

}  // namespace

const std::vector<SettingRow>& schema() {
  static const std::vector<SettingRow> v = build_schema();
  return v;
}

const SettingRow* find_row(std::string_view key) {
  for (const SettingRow& r : schema())
    if (key == r.key) return &r;
  return nullptr;
}

std::vector<const SettingRow*> rows_for_page(PageBit page) {
  std::vector<const SettingRow*> out;
  for (const SettingRow& r : schema())
    if (r.pages & page) out.push_back(&r);
  return out;
}

// ---- values ----
std::string value_text(const SettingRow& r, const Settings& s) {
  const auto m = settings_map(s);
  const auto it = m.find(r.key);
  return it == m.end() ? std::string() : it->second;
}

bool set_value_text(const SettingRow& r, Settings& s, const std::string& text, std::string& err) {
  Settings probe = s;
  err.clear();
  if (!parse_settings(std::string(r.key) + " = " + text, probe, err)) return false;
  s = probe;
  return true;
}

bool get_bool(const SettingRow& r, const Settings& s) {
  const Field& f = field_of(r);
  return f.kind == Field::B ? s.*f.b : false;
}
void set_bool(const SettingRow& r, Settings& s, bool v) {
  const Field& f = field_of(r);
  if (f.kind == Field::B) s.*f.b = v;
}
int get_int(const SettingRow& r, const Settings& s) {
  const Field& f = field_of(r);
  return f.kind == Field::I ? s.*f.i : 0;
}
void set_int(const SettingRow& r, Settings& s, int v) {
  const Field& f = field_of(r);
  if (f.kind == Field::I) s.*f.i = std::clamp(v, r.lo, r.hi);
}
float get_float(const SettingRow& r, const Settings& s) {
  const Field& f = field_of(r);
  return f.kind == Field::F ? s.*f.f : 0.0f;
}
void set_float(const SettingRow& r, Settings& s, float v) {
  const Field& f = field_of(r);
  if (f.kind == Field::F) s.*f.f = std::clamp(v, r.flo, r.fhi);
}
int get_choice(const SettingRow& r, const Settings& s) {
  const Field& f = field_of(r);
  if (f.kind == Field::AudioFilter) return static_cast<int>(s.*f.af);
  if (f.kind == Field::VideoFilter) return static_cast<int>(s.*f.vf);
  if (f.kind != Field::S) return -1;
  for (int i = 0; i < r.nchoices; ++i)
    if (s.*f.s == r.choices[i].value) return i;
  return -1;
}
void set_choice(const SettingRow& r, Settings& s, int index) {
  if (index < 0 || index >= r.nchoices) return;
  const Field& f = field_of(r);
  if (f.kind == Field::AudioFilter) s.*f.af = static_cast<x2::FilterSetting>(index);
  else if (f.kind == Field::VideoFilter) s.*f.vf = static_cast<x2::VideoFilter>(index);
  else if (f.kind == Field::S) s.*f.s = r.choices[index].value;
}

// ---- what applies ----
bool is_cheat_active(const SettingRow& r, const Settings& s) {
  if (!r.cheat && std::strcmp(r.key, "credits") != 0) return false;
  if (std::strcmp(r.key, "credits") == 0) return s.credits > 3;
  return get_bool(r, s);
}

std::string disabled_reason(const SettingRow& r, const Settings& s, GameKind g) {
  const std::string key = r.key;
  if (!applies_to(r, g)) {
    if (key == "speech_volume") return "Only the CDTV release has speech.";
    if (key == "protect_music") return "Only the disk release plays tracker music, and this keeps its tune.";
    return std::string("The ") + game_name(g) + " release does not use this.";
  }
  if (r.picture && s.faithful) return "The Faithful switch is on. It overrides this.";
  if (key == "curvature" && s.video_filter != VideoFilter::Crt) return "Only with the crt filter.";
  if (key == "interp_scale" && !(s.interp && g != GameKind::St) && !s.upgrade) return g == GameKind::St ? "Turn on asset packs first." : "Turn on smooth frames or asset packs first.";
  if (key == "smooth_far" && !s.interp) return "Turn on smooth frames first.";
  if (key == "autofire_rate" && !s.autofire) return "Turn on autofire first.";
  if ((r.pages & kPageAudio) && key != "audio" && !s.audio) return "Sound is off.";
  return std::string();
}

// ---- the command line ----
std::string CommandLine::text(const char* program) const {
  std::string t = program;
  for (const std::string& a : args) t += " " + quote_arg(a);
  return t;
}

CommandLine command_line(const Settings& s, GameKind game, bool with_game) {
  static const std::map<std::string, std::string> defaults = settings_map(Settings{});
  const std::map<std::string, std::string> now = settings_map(s);
  auto differs = [&](const char* key) { return now.at(key) != defaults.at(key); };
  CommandLine c;
  std::vector<std::string> modern;
  bool packs_written = false;
  const bool faithful = s.faithful;
  if (with_game) {
    c.args.push_back("--game");
    c.args.push_back(game_flag_value(game));
  } else if (differs("game")) {
    c.args.push_back("--game");
    c.args.push_back(s.game);
  }
  for (const SettingRow& r : schema()) {
    if (std::strcmp(r.key, "game") == 0) continue;
    const bool diff = differs(r.key);
    if (r.cli.kind == CliKind::Autofire) {
      if (std::strcmp(r.key, "autofire") == 0) {
        if (!applies_to(r, game)) continue;
        if (s.autofire) c.args.push_back(s.autofire_rate != Settings{}.autofire_rate ? "--autofire=" + std::to_string(s.autofire_rate) : "--autofire");
      } else if (diff && !s.autofire && applies_to(r, game)) {
        c.notes.push_back("autofire_rate has an effect only with autofire on");
      }
      continue;
    }
    if (!diff) continue;
    if (!applies_to(r, game)) continue;
    if (faithful && r.picture) continue;
    const std::string& val = now.at(r.key);
    switch (r.cli.kind) {
      case CliKind::None:
        c.notes.push_back(r.note[0] ? r.note : std::string(r.key) + " is kept in settings.cfg");
        break;
      case CliKind::Value:
        c.args.push_back(std::string("--") + r.cli.flag);
        c.args.push_back(val);
        break;
      case CliKind::Switch:
      case CliKind::NoSwitch:
        c.args.push_back(std::string("--") + r.cli.flag);
        break;
      case CliKind::Pair:
        c.args.push_back(std::string(get_bool(r, s) ? "--" : "--no-") + r.cli.flag);
        break;
      case CliKind::Modern:
        modern.push_back(get_bool(r, s) ? r.cli.item : r.cli.item_off);
        break;
      case CliKind::ModernVal:
        modern.push_back(std::string(r.cli.item) + val);
        break;
      case CliKind::Repeat:
        for (const std::string& p : split_list(val, ';')) {
          c.args.push_back(std::string("--") + r.cli.flag);
          c.args.push_back(p);
          packs_written = true;
        }
        break;
      case CliKind::Map: {
        const int i = get_choice(r, s);
        if (i >= 0 && r.cli.map_flags[i][0]) c.args.push_back(r.cli.map_flags[i]);
        break;
      }
      case CliKind::Autofire:
        break;
    }
  }
  if (packs_written) {
    // --asset-pack also turns the asset packs on, so the separate item would only repeat it.
    modern.erase(std::remove(modern.begin(), modern.end(), std::string("upgrade")), modern.end());
    if (!s.upgrade && !faithful) c.notes.push_back("--asset-pack also turns the asset packs on");
  }
  if (!modern.empty()) {
    std::string list;
    for (size_t i = 0; i < modern.size(); ++i) list += (i ? "," : "") + modern[i];
    c.args.push_back("--modern");
    c.args.push_back(list);
  }
  return c;
}

}  // namespace x2::ui
