// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "persist.h"

#include "machine.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace x2 {

namespace {

constexpr char kMagic[4] = {'X', '2', 'H', 'S'};
constexpr size_t kHeaderSize = 4 + 4 + 8 + 4;
// Characters the name entry offers ($86FC) plus the space the table lines use.
constexpr const char* kTableChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.:0123456789 ";

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

uint64_t fnv1a64(const uint8_t* data, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

std::vector<uint8_t> encode_hiscores(const HiscoreTable& t, uint64_t image_hash) {
    std::vector<uint8_t> v(kMagic, kMagic + 4);
    put32(v, kHiscoreVersion);
    put32(v, uint32_t(image_hash >> 32));
    put32(v, uint32_t(image_hash));
    put32(v, kHiscoreSize);
    v.insert(v.end(), t.begin(), t.end());
    put32(v, crc32(v.data(), v.size()));
    return v;
}

bool decode_hiscores(const std::vector<uint8_t>& f, uint64_t image_hash, HiscoreTable& out, std::string& why) {
    if (f.size() < kHeaderSize + 4 || std::memcmp(f.data(), kMagic, 4) != 0) { why = "not a high-score file"; return false; }
    if (uint32_t v = get32(&f[4]); v != kHiscoreVersion) { why = "version " + std::to_string(v) + " is not supported"; return false; }
    uint64_t h = uint64_t(get32(&f[8])) << 32 | get32(&f[12]);
    if (h != image_hash) { why = "saved for a different game image"; return false; }
    if (get32(&f[16]) != kHiscoreSize || f.size() != kHeaderSize + kHiscoreSize + 4) { why = "wrong size"; return false; }
    if (crc32(f.data(), f.size() - 4) != get32(&f[f.size() - 4])) { why = "CRC mismatch"; return false; }
    std::memcpy(out.data(), &f[kHeaderSize], kHiscoreSize);
    for (uint32_t i = 40; i < kHiscoreSize; ++i)
        if (!out[i] || !std::strchr(kTableChars, out[i])) { why = "invalid character in the table"; return false; }
    return true;
}

bool load_hiscore_file(const std::string& path, uint64_t image_hash, HiscoreTable& out, std::string& why) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { why = "no saved table"; return false; }
    std::vector<uint8_t> data(kHeaderSize + kHiscoreSize + 8);
    size_t n = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    data.resize(n);
    return decode_hiscores(data, image_hash, out, why);
}

bool save_hiscore_file(const std::string& path, uint64_t image_hash, const HiscoreTable& t, std::string& why) {
    std::vector<uint8_t> data = encode_hiscores(t, image_hash);
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { why = std::string("cannot write ") + tmp + ": " + std::strerror(errno); return false; }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) {
        why = "cannot write " + path + (ec ? ": " + ec.message() : std::string());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

HiscoreTable read_hiscores(const Machine& m, uint32_t addr) {
    HiscoreTable t;
    std::memcpy(t.data(), &m.chip[addr], kHiscoreSize);
    return t;
}

void write_hiscores(Machine& m, const HiscoreTable& t, uint32_t addr) { std::memcpy(&m.chip[addr], t.data(), kHiscoreSize); }

namespace {
constexpr const char* kVideoFilterNames[int(VideoFilter::kCount)] = {"nearest", "sharp", "scanlines", "crt", "edge"};
}

const char* video_filter_name(VideoFilter f) {
    return f < VideoFilter::kCount ? kVideoFilterNames[int(f)] : "?";
}

bool parse_video_filter(const std::string& name, VideoFilter& out) {
    for (int i = 0; i < int(VideoFilter::kCount); ++i)
        if (name == kVideoFilterNames[i]) { out = VideoFilter(i); return true; }
    return false;
}

bool parse_settings(const std::string& text, Settings& s, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    bool ok = true;
    while (std::getline(in, line)) {
        ++lineno;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        if (trim(line).empty()) continue;
        auto eq = line.find('=');
        std::string key = trim(line.substr(0, eq)), val = eq == std::string::npos ? "" : trim(line.substr(eq + 1));
        auto bad = [&](const char* what) {
            error += "line " + std::to_string(lineno) + ": " + what + ": " + trim(line) + "\n";
            ok = false;
        };
        auto boolean = [&](bool& out) {
            if (val == "on" || val == "yes" || val == "true" || val == "1") out = true;
            else if (val == "off" || val == "no" || val == "false" || val == "0") out = false;
            else bad("expected on or off");
        };
        auto number = [&](double lo, double hi, double& out) {
            char* end = nullptr;
            double d = std::strtod(val.c_str(), &end);
            if (val.empty() || *end || d < lo || d > hi) { bad("value out of range"); return false; }
            out = d;
            return true;
        };
        double d = 0;
        if (eq == std::string::npos) bad("expected key = value");
        else if (key == "scale") { if (number(1, 8, d)) s.scale = int(d); }
        else if (key == "fullscreen") boolean(s.fullscreen);
        else if (key == "aspect") boolean(s.aspect);
        else if (key == "audio") boolean(s.audio);
        else if (key == "high_scores") boolean(s.high_scores);
        else if (key == "grab_keyboard") {
            if (val == "auto" || val == "on" || val == "off") s.grab_keyboard = val;
            else bad("expected auto, on or off");
        }
        else if (key == "cursor") {
            // The same names the command line takes (--cursor on/off/yes/no); the file keeps show and hide.
            if (val == "auto" || val == "show" || val == "hide") s.cursor = val;
            else if (val == "on" || val == "yes") s.cursor = "show";
            else if (val == "off" || val == "no") s.cursor = "hide";
            else bad("expected auto, show or hide (or on, off, yes, no)");
        }
        else if (key == "input_debug") boolean(s.input_debug);
        else if (key == "stereo_separation") { if (number(0, 1, d)) s.stereo_separation = float(d); }
        else if (key == "volume") { if (number(0, 100, d)) s.volume = int(d); }
        else if (key == "audio_latency") { if (number(10, 500, d)) s.audio_latency = int(d); }
        else if (key == "audio_filter") {
            if (val == "a500") s.filter = FilterSetting::A500;
            else if (val == "a1200") s.filter = FilterSetting::A1200;
            else if (val == "none") s.filter = FilterSetting::None;
            else bad("expected a500, a1200 or none");
        } else if (key == "modern") {
            if (val == "faithful") s.faithful = true;
            else if (val == "custom") s.faithful = false;
            else bad("expected faithful or custom");
        } else if (key == "interp") boolean(s.interp);
        else if (key == "interp_scale") { if (number(1, 8, d)) s.interp_scale = int(d); }
        else if (key == "smooth_far") boolean(s.smooth_far);
        else if (key == "filter") {
            if (!parse_video_filter(val, s.video_filter)) bad("expected nearest, sharp, scanlines, crt or edge");
        } else if (key == "integer") boolean(s.integer);
        else if (key == "curvature") boolean(s.curvature);
        else if (key == "upgrade") boolean(s.upgrade);
        else if (key == "asset_packs") s.asset_packs = val;
        else if (key == "audio_upgrade") boolean(s.audio_upgrade);
        else if (key == "packs_enabled") s.packs_enabled = val;
        else if (key == "hd_panel") boolean(s.hd_panel);
        else if (key == "wide") {
            // off, pixels per side 0..320, or W:H with 1..100 each (modern/present.h parse_wide).
            unsigned a = 0, b = 0;
            char rest = 0;
            if (val == "off" || (std::sscanf(val.c_str(), "%u%c", &a, &rest) == 1 && a <= 320 && val.find_first_not_of("0123456789") == std::string::npos) ||
                (std::sscanf(val.c_str(), "%u:%u%c", &a, &b, &rest) == 2 && a >= 1 && a <= 100 && b >= 1 && b <= 100 &&
                 val.find_first_not_of("0123456789:") == std::string::npos))
                s.wide = val;
            else
                bad("expected off, 16:10, 16:9, 21:9, 32:9 or pixels per side 0..320");
        }
        else if (key == "music") {
            if (val == "amiga" || val == "cdtv" || val == "mix") s.music = val;
            else bad("expected amiga, cdtv or mix");
        }
        else if (key == "music_volume") { if (number(0, 100, d)) s.music_volume = int(d); }
        else if (key == "sfx_volume") { if (number(0, 100, d)) s.sfx_volume = int(d); }
        else if (key == "speech_volume") { if (number(0, 100, d)) s.speech_volume = int(d); }
        else if (key == "protect_music") boolean(s.protect_music);
        else if (key == "cd_crossfade") { if (number(0, 500, d)) s.cd_crossfade = int(d); }
        else if (key == "cd_pause") {
            if (val == "keep" || val == "duck" || val == "stop") s.cd_pause = val;
            else bad("expected keep, duck or stop");
        }
        else if (key == "cdtv") s.cdtv = val;
        else if (key == "autofire") boolean(s.autofire);
        else if (key == "autofire_rate") { if (number(1, 25, d)) s.autofire_rate = int(d); }
        else if (key == "god_mode") boolean(s.god_mode);
        else if (key == "credits") { if (number(1, 9, d)) s.credits = int(d); }
        else if (key == "cheat_scores") boolean(s.cheat_scores);
        else if (key == "game") {
            if (val == "auto" || val == "disk" || val == "cdtv" || val == "st") s.game = val;
            else bad("expected auto, disk, cdtv or st");
        }
        else if (key == "launcher") {
            if (val == "auto" || val == "always" || val == "never") s.launcher = val;
            else bad("expected auto, always or never");
        }
        else if (key == "ui_text") {
            if (val == "normal" || val == "large") s.ui_text = val;
            else bad("expected normal or large");
        }
        else if (key == "ui_contrast") {
            if (val == "normal" || val == "high") s.ui_contrast = val;
            else bad("expected normal or high");
        }
        else if (key == "ui_motion") {
            if (val == "auto" || val == "full" || val == "reduced") s.ui_motion = val;
            else bad("expected auto, full or reduced");
        }
        else if (key == "last_game") {
            if (val.empty() || val == "disk" || val == "cdtv" || val == "st") s.last_game = val;
            else bad("expected disk, cdtv or st");
        }
        else if (key == "cdtv_tracks") {
            std::array<int, 6> t{};
            if (parse_cdtv_tracks(val, t)) s.cdtv_tracks = val;
            else bad("expected six CD track numbers 2..13: title,level1,...,level5");
        }
        else bad("unknown key");
    }
    return ok;
}

bool parse_cdtv_tracks(const std::string& s, std::array<int, 6>& out) {
    std::istringstream in(s);
    std::string item;
    size_t n = 0;
    while (std::getline(in, item, ',')) {
        item = trim(item);
        char* end = nullptr;
        long v = std::strtol(item.c_str(), &end, 10);
        if (item.empty() || *end || v < 2 || v > 13 || n >= out.size()) return false;
        out[n++] = int(v);
    }
    return n == out.size();
}

std::string format_settings(const Settings& s) {
    auto onoff = [](bool b) { return b ? "on" : "off"; };
    char sep[32];
    std::snprintf(sep, sizeof sep, "%g", double(s.stereo_separation));
    std::string r = "# x2recomp settings (window mode only). Command-line options override these values;\n"
                    "# the volume, scale and fullscreen hotkeys write their new values back here on exit.\n";
    r += "scale = " + std::to_string(s.scale) + "               # window scale 1..8\n";
    r += std::string("fullscreen = ") + onoff(s.fullscreen) + "\n";
    r += std::string("aspect = ") + onoff(s.aspect) + "             # on: 4:3 picture, off: square pixels\n";
    r += std::string("audio = ") + onoff(s.audio) + "               # off = --no-audio\n";
    r += std::string("audio_filter = ") + (s.filter == FilterSetting::A500 ? "a500" : s.filter == FilterSetting::A1200 ? "a1200" : "none") +
         "      # a500, a1200 or none\n";
    r += std::string("stereo_separation = ") + sep + "   # 0 (mono) .. 1 (hard left/right)\n";
    r += "volume = " + std::to_string(s.volume) + "            # master volume, percent\n";
    r += "audio_latency = " + std::to_string(s.audio_latency) + "      # ms 10..500: output buffer; raise it if the sound breaks up\n";
    r += std::string("high_scores = ") + onoff(s.high_scores) + "         # keep the high-score table in highscores.bin\n";
    r += "# Keyboard (x2recomp --list-keys prints the bindings; edit them in input.cfg).\n";
    r += "grab_keyboard = " + s.grab_keyboard + "    # auto (the platform default), on or off: send the system's\n"
         "                       # own keyboard shortcuts to the window while it has the focus\n";
    r += "cursor = " + s.cursor + "           # mouse cursor: auto (hidden while playing), show or hide\n";
    r += std::string("input_debug = ") + onoff(s.input_debug) + "         # log every key, focus change and frame to stdout\n";
    r += "# Picture (F1 opens the menu, whose first row is the Faithful switch; F2 cycles the filter, F3 interpolation, F4 the 4:3 aspect).\n";
    r += std::string("modern = ") + (s.faithful ? "faithful" : "custom") +
         "           # faithful: the original picture only (ignores the options below)\n";
    r += std::string("interp = ") + onoff(s.interp) + "              # interpolated frames at the display rate\n";
    r += "interp_scale = " + std::to_string(s.interp_scale) + "          # interpolation canvas 1..8 x 320x256\n";
    r += std::string("smooth_far = ") + onoff(s.smooth_far) + "           # far layer at constant speed\n";
    r += std::string("filter = ") + video_filter_name(s.video_filter) + "          # nearest, sharp, scanlines, crt or edge\n";
    r += std::string("integer = ") + onoff(s.integer) + "              # integer scale factors only\n";
    r += std::string("curvature = ") + onoff(s.curvature) + "           # crt filter: curved screen\n";
    r += "# Upgraded graphics (Home toggles the asset packs, End cycles the surround, Page Down the HD panel).\n";
    r += std::string("upgrade = ") + onoff(s.upgrade) + "             # replacement asset packs and palette overrides\n";
    r += "asset_packs = " + s.asset_packs + "    # pack directories or .zip files, separated by ';'\n";
    r += std::string("audio_upgrade = ") + onoff(s.audio_upgrade) + "       # packs' sound effects, speech and music (needs a pack with audio)\n";
    r += "packs_enabled = " + s.packs_enabled + "    # ids of the packs found in the data folder's packs/ folder, separated by ';'\n";
    r += "wide = " + s.wide + "               # widescreen surround: off, 16:10, 16:9, 21:9, 32:9 or pixels per side\n";
    r += std::string("hd_panel = ") + onoff(s.hd_panel) + "            # status panel with the edge-preserving upscale\n";
    r += "# CDTV CD music (your own CDTV disc: x2recomp --import-cdtv PATH; Page Up cycles amiga/cdtv/mix).\n";
    r += "music = " + s.music + "             # amiga (tracker music), cdtv (CD tracks) or mix (both)\n";
    r += "music_volume = " + std::to_string(s.music_volume) + "      # percent: CD tracks and tracker music\n";
    r += "sfx_volume = " + std::to_string(s.sfx_volume) + "        # percent: sound effects\n";
    r += "speech_volume = " + std::to_string(s.speech_volume) + "     # percent: speech (the CDTV release's voices)\n";
    r += std::string("protect_music = ") + onoff(s.protect_music) +
         "       # on: the tune keeps its voices through the sound effects (not the original)\n";
    r += "cd_crossfade = " + std::to_string(s.cd_crossfade) + "        # ms 0..500: seamless CD loops (0 = as the CDTV game)\n";
    r += "cd_pause = " + s.cd_pause + "           # game pause: keep, duck or stop the CD\n";
    r += "cdtv = " + s.cdtv + "     # imported CD audio directory or .cue in place (empty = <data dir>/cdtv)\n";
    r += "cdtv_tracks = " + s.cdtv_tracks + "  # CD track for title, level 1..5\n";
    r += "# Assists and cheats (all off by default; README 'Assists and cheats'). A session that uses a cheat\n"
         "# does not save high scores unless cheat_scores is on.\n";
    r += std::string("autofire = ") + onoff(s.autofire) + "            # pulse fire while it is held (during a level only)\n";
    r += "autofire_rate = " + std::to_string(s.autofire_rate) + "       # pulses per second 1..25 (17 = the game's own maximum)\n";
    r += std::string("god_mode = ") + onoff(s.god_mode) + "            # CHEAT: the ship's energy never runs out\n";
    r += "credits = " + std::to_string(s.credits) + "            # credits a new game starts with, 1..9 (the game's own value is 3)\n";
    r += std::string("cheat_scores = ") + onoff(s.cheat_scores) + "        # on: save high scores even after a cheat was used\n";
    r += "# Which imported release to start when both are imported (--game overrides).\n";
    r += "game = " + s.game + "              # auto (the disk release if imported, else CDTV, else Atari ST), disk, cdtv or st\n";
    r += "# The launcher and the in-game menu.\n";
    r += "launcher = " + s.launcher + "           # auto (shown when x2recomp is started without arguments), always or never\n";
    r += "ui_text = " + s.ui_text + "          # normal or large\n";
    r += "ui_contrast = " + s.ui_contrast + "      # normal or high\n";
    r += "ui_motion = " + s.ui_motion + "          # auto (follow the system), full or reduced\n";
    r += "last_game = " + s.last_game + "          # the release played last: disk, cdtv or st\n";
    return r;
}

namespace {

// "key = value   # comment" -> key and value (trimmed, comment removed). False for blank, comment-only and malformed lines.
bool split_setting_line(const std::string& line, std::string& key, std::string& value, std::string& comment, size_t* value_begin = nullptr) {
    std::string body = line;
    comment.clear();
    if (auto h = body.find('#'); h != std::string::npos) {
        comment = body.substr(h);
        body.resize(h);
    }
    const auto eq = body.find('=');
    if (eq == std::string::npos) return false;
    key = trim(body.substr(0, eq));
    value = trim(body.substr(eq + 1));
    if (value_begin) *value_begin = eq + 1;
    return !key.empty();
}

// The text format_settings writes for `key` in `s`, or "" with ok = false if it has no such line.
std::string canonical_value(const Settings& s, const std::string& key, bool& ok) {
    std::istringstream in(format_settings(s));
    std::string line, k, v, c;
    while (std::getline(in, line))
        if (split_setting_line(line, k, v, c) && k == key) { ok = true; return v; }
    ok = false;
    return std::string();
}

} // namespace

std::string merge_settings_text(const std::string& old_text, const Settings& s) {
    if (trim(old_text).empty()) return format_settings(s);
    // The lines format_settings writes for this `s`: value and full line per key, in its order.
    std::vector<std::string> keys;
    std::map<std::string, std::pair<std::string, std::string>> fresh; // key -> (value, generated line)
    {
        std::istringstream in(format_settings(s));
        std::string line, k, v, c;
        while (std::getline(in, line))
            if (split_setting_line(line, k, v, c)) {
                keys.push_back(k);
                fresh[k] = {v, line};
            }
    }
    std::vector<std::string> lines;
    {
        std::istringstream in(old_text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    // Only the last line of a key counts (parse_settings applies them in order).
    std::map<std::string, size_t> last;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string k, v, c;
        if (split_setting_line(lines[i], k, v, c) && fresh.count(k)) last[k] = i;
    }
    for (const auto& [k, idx] : last) {
        std::string key, value, comment;
        split_setting_line(lines[idx], key, value, comment);
        // What the file's own value means: apply the line to a default Settings and read the key back.
        Settings probe;
        std::string err;
        bool valid = parse_settings(key + " = " + value, probe, err);
        bool ok = false;
        const std::string old_canon = valid ? canonical_value(probe, key, ok) : std::string();
        if (valid && ok && old_canon == fresh[k].first) continue; // says the same thing: keep the line as it is
        std::string indent = lines[idx].substr(0, lines[idx].find_first_not_of(" \t"));
        std::string nl = indent + key + " = " + fresh[k].first;
        if (!comment.empty()) nl += "  " + comment;
        lines[idx] = nl;
    }
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    std::string added;
    for (const std::string& k : keys)
        if (!last.count(k)) added += fresh[k].second + "\n";
    if (!added.empty()) {
        if (!out.empty() && out.back() == '\n' && !lines.empty() && !lines.back().empty()) out += "\n";
        out += "# Settings this version added:\n";
        out += added;
    }
    return out;
}

bool load_settings_file(const std::string& path, Settings& s, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    error.clear();
    parse_settings(text, s, error);
    return true;
}

bool save_settings_file(const std::string& path, const Settings& s, std::string& why) {
    std::string old_text;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            old_text = ss.str();
        }
    }
    const std::string text = merge_settings_text(old_text, s);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { why = "cannot write " + tmp; return false; }
        out << text;
        out.flush();
        if (!out) { why = "cannot write " + tmp; std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        why = "cannot replace " + path + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace x2
