// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// manifest.json of an asset pack (versions 1 and 2) as typed declarations: syntax, ranges and key forms are checked here, files are
// not touched (pack_stack.cpp loads them, the validator in pack_check.cpp inspects them). docs/schema/x2-asset-pack-v2.schema.json
// is the schema of the same document; the two agree on every field.
#pragma once
#include "pack/pack_api.h"
#include "pack/pack_json.h"
#include "pack/pack_limits.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace x2::pack {

constexpr int kMaxManifestVersion = 2;
constexpr int kFarWidth = 320;  // pixels of one far-layer line (modern::kWidth)

// Where a declaration came from, for messages ("graphics" + key, "far[2]" + "", ...), and which games and builds it is for.
struct DeclBase {
    std::string section, key;
    GameScope scope;
};

struct GraphicDecl : DeclBase {
    std::vector<std::string> keys;  // the key and its `keys` aliases; `f:`-prefixed ones are flash pictures
    std::string png, flash_png;
    int scale = 1;
    Anchor anchor;
};
struct FarDecl : DeclBase {
    std::string png;
    int scale = 1;
    std::vector<uint64_t> lines;    // per strip row; 0 with used[r] == false for a row left unused
    std::vector<bool> used;
};
struct PaletteDecl : DeclBase {
    PaletteOverride po;
};
struct BackgroundDecl : DeclBase {
    int level = 0;
    std::string png;
    int scale = 1;
    int ring_lines = 0;
};
struct AtlasFrame {
    std::string key;
    int rect[4] = {0, 0, 0, 0};
    Anchor anchor;
    bool has_flash = false;
    int flash_rect[4] = {0, 0, 0, 0};
};
struct AtlasDecl : DeclBase {
    std::string png;
    int scale = 1;
    std::vector<AtlasFrame> frames;
    bool has_grid = false;
    int cell_w = 0, cell_h = 0, columns = 0;  // columns 0: as many as fit the picture
    std::vector<std::string> grid_keys;        // "" for a skipped cell
};
struct StarsDecl : DeclBase {
    StarsEntry::Shape shape = StarsEntry::Shape::Pixel;
    double size = 1.0;
    double softness = 0.5;
};
struct ScreenDecl : DeclBase {
    ScreenSignature signature;
    std::string png, wide_png;
    int scale = 1, wide_scale = 0;
    Anchor anchor;
    std::vector<uint32_t> ref_palette;
    FadeMode fade = FadeMode::Scale;
    std::vector<Rect> dynamic;
    int bg_index = 0;
};
struct PanelCellDecl : DeclBase {
    std::vector<std::string> keys;
    std::string png;
    int scale = 1;
    Anchor anchor;
};
struct PanelDecl : DeclBase {
    int scale = 1, cell_w = 8, max_span = 6;
    std::string background_png;
    Anchor background_anchor;
    std::vector<PanelCellDecl> cells;
};
struct ClipDecl : DeclBase {
    bool speech = false;
    std::vector<std::string> keys;  // the key and its `keys` aliases
    std::string file;
    int ref_period = 0;
    float gain_db = 0.0f;
    PanMode pan = PanMode::Channel;
    float pan_pos = 0.0f;
    PitchMode pitch = PitchMode::Follow;
    OriginalMode original = OriginalMode::Mute;
    float original_mix_db = 0.0f;
    CutMode cut = CutMode::Channel;
    int fade_out_ms = 5;
    bool loop = false;
    std::string cd0_file;
};
struct MusicDefaultsDecl {
    std::optional<float> gain_db;
    std::optional<int> crossfade_ms, fade_in_ms, fade_out_ms;
    std::optional<LoopMode> loop_mode;
    std::optional<Retrigger> retrigger;
};
struct MusicSlotDecl : DeclBase {
    MusicSlot slot;           // slot.file / slot.intro hold only the path (pack index filled in by the loader)
    bool file_null = false;   // "file": null
    std::optional<float> gain_db;
};
struct Credit {
    std::string name, role, url;
};
struct Requirement {
    std::string id, version;  // version: "" or an operator and x.y[.z]
};

struct Manifest {
    int version = 0;
    std::string name, id, pack_version, description, author, url, license, preview;
    std::vector<Credit> credits;
    std::vector<std::string> provides;
    GameScope scope;
    std::string requires_x2recomp;
    std::vector<Requirement> requires_packs;
    int scale = 1;

    std::vector<GraphicDecl> graphics;
    std::vector<FarDecl> far;
    std::vector<PaletteDecl> palettes;
    std::vector<BackgroundDecl> backgrounds;
    std::vector<AtlasDecl> atlases;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::optional<StarsDecl> stars;
    std::vector<ScreenDecl> screens;
    std::optional<PanelDecl> panel;
    std::vector<ClipDecl> clips;  // sfx and speech, in manifest order
    MusicDefaultsDecl music_defaults;
    std::vector<MusicSlotDecl> slots;

    bool has_graphics_section = false, has_screens_section = false, has_audio_section = false;
    size_t entries = 0;           // declarations accepted (atlas frames and grid cells included)
    size_t skipped = 0;           // entries refused (each has an Error message)
};

struct ParseOptions {
    bool validate = false;        // also report unknown fields and a missing license (the loader stays quiet about them)
    PackLimits limits;
};

// `label` names the pack in messages. Returns false with `fatal` when the pack cannot be used at all (not an object, wrong format,
// unusable version, bad pack-level scope or scale); every other problem is a message and a skipped entry or field.
bool parse_manifest(const Json& root, const std::string& label, const ParseOptions& opt, Manifest& out, PackReport& report, std::string& fatal);

// "x.y[.z]" and "x.y.z[-pre][+meta]" comparisons for `requires` and `pack_version`.
struct Semver {
    int major = 0, minor = 0, patch = 0;
    bool pre = false;
};
bool parse_semver(const std::string& s, Semver& out);                 // x.y.z with an optional -pre / +build suffix
bool version_satisfies(const std::string& have, const std::string& constraint);  // ">=1.2", ">1", "=2.0.1", "1.0" (= ">=")
bool valid_constraint(const std::string& c);
bool valid_pack_id(const std::string& id);
std::string slug_of(const std::string& name);                         // a lowercase id from a display name ("" if nothing is left)

} // namespace x2::pack
