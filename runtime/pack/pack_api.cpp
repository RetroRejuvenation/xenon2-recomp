// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace x2::pack {

// ---- games ---------------------------------------------------------------------------------------------------------

const char* game_name(GameFamily g) {
    switch (g) {
    case GameFamily::Disk: return "disk";
    case GameFamily::Cdtv: return "cdtv";
    case GameFamily::St: return "st";
    }
    return "disk";
}

bool parse_game(const std::string& s, GameFamily& out) {
    if (s == "disk") out = GameFamily::Disk;
    else if (s == "cdtv") out = GameFamily::Cdtv;
    else if (s == "st") out = GameFamily::St;
    else return false;
    return true;
}

bool GameScope::applies(const PackContext& c) const {
    if (!((games >> unsigned(c.game)) & 1u)) return false;
    if (builds.empty()) return true;
    const std::string name = c.build_name();
    return std::find(builds.begin(), builds.end(), name) != builds.end();
}

GameScope GameScope::intersect(const GameScope& pack, const GameScope& entry) {
    GameScope r;
    r.games = uint8_t(pack.games & entry.games);
    if (pack.builds.empty()) r.builds = entry.builds;
    else if (entry.builds.empty()) r.builds = pack.builds;
    else {
        for (const std::string& b : pack.builds)
            if (std::find(entry.builds.begin(), entry.builds.end(), b) != entry.builds.end()) r.builds.push_back(b);
        if (r.builds.empty()) r.games = 0; // both name builds and none is common: applies nowhere
    }
    return r;
}

// ---- keys and hashing ----------------------------------------------------------------------------------------------

uint64_t fnv1a64(const uint8_t* data, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * kFnvPrime;
    return h;
}

std::string hash_hex(uint64_t h) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

namespace {

bool hex16(const std::string& s, size_t at) {
    if (s.size() < at + 16) return false;
    for (size_t i = at; i < at + 16; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Digits only, no leading zero, value in [lo, hi]; true on success.
bool decimal(const std::string& s, size_t from, size_t to, uint64_t lo, uint64_t hi, uint64_t& v) {
    if (to <= from || to - from > 10) return false;
    if (s[from] == '0' && to - from > 1) return false;
    v = 0;
    for (size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + uint64_t(s[i] - '0');
    }
    return v >= lo && v <= hi;
}

uint64_t fnv_u16le(uint64_t h, int v) {
    const uint8_t b[2] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF)};
    return fnv1a64(b, 2, h);
}

} // namespace

bool is_hash_hex(const std::string& s) { return s.size() == 16 && hex16(s, 0); }

uint64_t block_hash(int w, int h, const uint8_t* index_bytes) {
    uint64_t x = fnv_u16le(kFnvOffset, w);
    x = fnv_u16le(x, h);
    return fnv1a64(index_bytes, size_t(w) * size_t(h), x);
}

uint64_t block_hash_region(const uint8_t* frame, int stride, int x0, int y0, int w, int h) {
    uint64_t x = fnv_u16le(kFnvOffset, w);
    x = fnv_u16le(x, h);
    for (int y = 0; y < h; ++y) x = fnv1a64(frame + size_t(y0 + y) * size_t(stride) + size_t(x0), size_t(w), x);
    return x;
}

std::string sample_key(const uint8_t* sample, uint32_t bytes) {
    const uint8_t len[4] = {uint8_t(bytes & 0xFF), uint8_t((bytes >> 8) & 0xFF), uint8_t((bytes >> 16) & 0xFF),
                            uint8_t((bytes >> 24) & 0xFF)};
    const uint64_t h = fnv1a64(sample, bytes, fnv1a64(len, 4));
    return hash_hex(h) + "-" + std::to_string(bytes);
}

std::string panel_key(uint64_t block, int span) { return hash_hex(block) + "-" + std::to_string(span); }

bool valid_graphic_key(const std::string& k) {
    if (k.size() < 20 || !hex16(k, 0) || k[16] != '-') return false;
    const size_t x = k.find('x', 17);
    uint64_t w = 0, h = 0;
    return x != std::string::npos && decimal(k, 17, x, 1, 4096, w) && decimal(k, x + 1, k.size(), 1, 4096, h);
}

bool valid_flash_key(const std::string& k) { return k.size() > 2 && k[0] == 'f' && k[1] == ':' && valid_graphic_key(k.substr(2)); }

bool valid_sample_key(const std::string& k) {
    uint64_t n = 0;
    return k.size() >= 18 && hex16(k, 0) && k[16] == '-' && decimal(k, 17, k.size(), 1, 0xFFFFFFFFull, n);
}

bool valid_panel_key(const std::string& k) {
    uint64_t n = 0;
    return k.size() >= 18 && hex16(k, 0) && k[16] == '-' && decimal(k, 17, k.size(), 1, 8, n);
}

// ---- messages ------------------------------------------------------------------------------------------------------

std::string PackMessage::format() const {
    std::string r = "asset pack: ";
    if (!pack.empty()) r += pack + ": ";
    if (!section.empty()) {
        r += section;
        if (!key.empty()) r += "[\"" + key + "\"]";
        r += ": ";
    }
    return r + text;
}

void PackReport::add(Severity s, std::string pack, std::string section, std::string key, std::string text) {
    messages.push_back({s, std::move(pack), std::move(section), std::move(key), std::move(text)});
}

size_t PackReport::count(Severity s) const {
    return size_t(std::count_if(messages.begin(), messages.end(), [s](const PackMessage& m) { return m.severity == s; }));
}

// ---- the null library ----------------------------------------------------------------------------------------------

namespace {

class NullLibrary final : public PackLibrary {
public:
    const PackContext& context() const override { return ctx_; }
    const std::vector<PackInfo>& packs() const override { return packs_; }
    bool has_graphics() const override { return false; }
    bool has_audio() const override { return false; }
    const GraphicEntry* graphic(uint64_t, int, int) const override { return nullptr; }
    const GraphicEntry* flash(uint64_t, int, int) const override { return nullptr; }
    const FarLineRef* far_line(uint64_t) const override { return nullptr; }
    const BackgroundEntry* background(int) const override { return nullptr; }
    const StarsEntry* stars() const override { return nullptr; }
    const PaletteOverride* palette(const uint16_t*) const override { return nullptr; }
    const PanelSet* panel() const override { return nullptr; }
    const std::vector<const ScreenEntry*>& screens() const override { return screens_; }
    const std::vector<const ScreenEntry*>& screens_for_block(uint64_t) const override { return screens_; }
    bool has_clip_length(uint32_t) const override { return false; }
    const ClipEntry* sfx(const std::string&) const override { return nullptr; }
    const ClipEntry* speech(const std::string&) const override { return nullptr; }
    const ClipEntry* speech_for_file(const std::string&) const override { return nullptr; }
    const MusicDefaults& music_defaults() const override { return defaults_; }
    const MusicSlot* music_slot(const std::string&) const override { return nullptr; }
    bool read_file(const FileRef&, std::vector<uint8_t>&, std::string& why) const override {
        why = "no asset pack is loaded";
        return false;
    }
    std::unique_ptr<ByteSource> open_file(const FileRef&, std::string& why) const override {
        why = "no asset pack is loaded";
        return nullptr;
    }

private:
    PackContext ctx_;
    std::vector<PackInfo> packs_;
    std::vector<const ScreenEntry*> screens_;
    MusicDefaults defaults_;
};

AssetPackSettings g_settings;
std::shared_ptr<const PackLibrary> g_library;

} // namespace

const PackLibrary& null_library() {
    static const NullLibrary lib;
    return lib;
}

// ---- settings helpers ----------------------------------------------------------------------------------------------

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t end = s.find(';', at);
        if (end == std::string::npos) end = s.size();
        const size_t b = s.find_first_not_of(" \t", at);
        if (b != std::string::npos && b < end) {
            const size_t e = s.find_last_not_of(" \t", end - 1);
            out.push_back(s.substr(b, e - b + 1));
        }
        at = end + 1;
    }
    return out;
}

std::string join_path(const std::string& dir, const std::string& child) {
    if (dir.empty()) return "";
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + child : dir + "/" + child;
}

std::string default_packs_dir(const std::string& data_dir) { return join_path(data_dir, "packs"); }

bool apply_modern_item(const std::string& item, AssetPackSettings& s) {
    if (item == "audio") s.audio_upgrade = true;
    else if (item == "noaudio") s.audio_upgrade = false;
    else return false;
    return true;
}

const AssetPackSettings& pack_settings() { return g_settings; }
void set_pack_settings(const AssetPackSettings& s) { g_settings = s; }

const PackLibrary& packs() { return g_library ? *g_library : null_library(); }
void set_packs(std::shared_ptr<const PackLibrary> lib) { g_library = std::move(lib); }

// ---- audio codec hook ---------------------------------------------------------------------------------------------

namespace {
const AudioCodec* g_codec = nullptr;
}
void set_audio_codec(const AudioCodec* codec) { g_codec = codec; }
const AudioCodec* audio_codec() { return g_codec; }

// ---- entry points ----------------------------------------------------------------------------------------------------

// load_packs: pack_load.cpp; check_asset_pack_cli: pack_check.cpp; list_asset_packs_cli: pack_load.cpp.

} // namespace x2::pack
