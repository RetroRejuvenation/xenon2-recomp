#include "persist.h"

#include "machine.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

HiscoreTable read_hiscores(const Machine& m) {
    HiscoreTable t;
    std::memcpy(t.data(), &m.chip[kHiscoreAddr], kHiscoreSize);
    return t;
}

void write_hiscores(Machine& m, const HiscoreTable& t) { std::memcpy(&m.chip[kHiscoreAddr], t.data(), kHiscoreSize); }

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
        else if (key == "stereo_separation") { if (number(0, 1, d)) s.stereo_separation = float(d); }
        else if (key == "volume") { if (number(0, 100, d)) s.volume = int(d); }
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
        else bad("unknown key");
    }
    return ok;
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
    r += std::string("high_scores = ") + onoff(s.high_scores) + "         # keep the high-score table in highscores.bin\n";
    r += "# Picture (F1 toggles Faithful, F2 cycles the filter, F3 interpolation, F4 the 4:3 aspect).\n";
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
    r += "wide = " + s.wide + "               # widescreen surround: off, 16:10, 16:9, 21:9, 32:9 or pixels per side\n";
    r += std::string("hd_panel = ") + onoff(s.hd_panel) + "            # status panel with the edge-preserving upscale\n";
    return r;
}

} // namespace x2
