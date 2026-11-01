// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The layered packs of one run: PackStack loads packs one after the other (a later pack overrides an earlier one per key) and is the
// PackLibrary the presenter and the audio stage read. docs/re-notes/asset-packs-loader.md.
#pragma once
#include "pack/pack_api.h"
#include "pack/pack_limits.h"
#include "pack/pack_manifest.h"
#include "pack/pack_source.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2::pack {

struct StackOptions {
    PackLimits limits;
    // Validator behaviour: report unknown fields and a missing license, apply no game/build scoping, keep each manifest and its
    // pictures reachable (PackStack::record()), and decode audio only when `decode_audio` is set (otherwise probe the headers).
    bool validate = false;
    bool decode_audio = true;
    // The runtime's own version for `requires.x2recomp` ("" = X2_VERSION, or no check when that is unknown).
    std::string runtime_version;
};

class PackStack final : public PackLibrary {
public:
    PackStack(PackContext ctx, StackOptions opt = {});
    ~PackStack() override;
    PackStack(const PackStack&) = delete;
    PackStack& operator=(const PackStack&) = delete;

    // Adds the pack at `path` (a directory or a .zip). Returns false when the pack is not used (the reason is the report's last
    // Error message and PackInfo::problem); problems with single entries are Error messages and the entry is skipped.
    bool add(const std::string& path, PackReport& report, bool discovered = false);

    // What the validator and the tools look at.
    struct Record {
        std::shared_ptr<PackSource> source;
        std::unique_ptr<Manifest> manifest;                 // with StackOptions::validate
        std::map<std::string, const Picture*> pictures;     // decoded pictures by path, with StackOptions::validate
        std::map<std::string, AudioInfo> audio;             // probed audio files by path, with StackOptions::validate
        std::map<std::string, std::string> picture_errors;  // paths that failed to load: why
    };
    const std::vector<std::unique_ptr<Record>>& records() const { return records_; }
    const StackOptions& options() const { return opt_; }
    // Limits for the packs added from now on.
    void set_limits(const PackLimits& l) { opt_.limits = l; }
    // Every effective graphic (sprites and tiles) and far line, for adapters that mirror the lookup tables.
    template <class F>
    void for_each_graphic(F&& f) const {
        for (const auto& kv : graphics_) f(kv.first.hash, kv.first.w, kv.first.h, kv.second);
    }
    template <class F>
    void for_each_far(F&& f) const {
        for (const auto& kv : far_) f(kv.first, kv.second);
    }

    // Counts (v1 AssetLibrary accessors) and the memory in use.
    size_t graphic_count() const { return graphics_.size(); }
    size_t flash_count() const { return flashes_.size(); }
    size_t far_count() const { return far_.size(); }
    size_t palette_count() const { return palettes_.size(); }
    size_t screen_count() const { return screens_.size(); }
    size_t panel_cell_count() const { return panel_.cells.size(); }
    size_t sfx_count() const { return sfx_.size(); }
    size_t speech_count() const { return speech_.size(); }
    size_t music_slot_count() const { return slots_.size(); }
    size_t picture_bytes() const { return picture_bytes_; }
    size_t audio_bytes() const { return audio_bytes_; }

    // PackLibrary
    const PackContext& context() const override { return ctx_; }
    const std::vector<PackInfo>& packs() const override { return infos_; }
    bool has_graphics() const override;
    bool has_audio() const override { return !sfx_.empty() || !speech_.empty() || !slots_.empty(); }
    const GraphicEntry* graphic(uint64_t hash, int w, int h) const override;
    const GraphicEntry* flash(uint64_t mask_hash, int w, int h) const override;
    const FarLineRef* far_line(uint64_t line_hash) const override;
    const BackgroundEntry* background(int level) const override;
    const StarsEntry* stars() const override { return has_stars_ ? &stars_ : nullptr; }
    const PaletteOverride* palette(const uint16_t* colors16) const override;
    const PanelSet* panel() const override { return has_panel_ ? &panel_ : nullptr; }
    const std::vector<const ScreenEntry*>& screens() const override { return screen_list_; }
    const std::vector<const ScreenEntry*>& screens_for_block(uint64_t first_block_hash) const override;
    bool has_clip_length(uint32_t sample_bytes) const override;
    const ClipEntry* sfx(const std::string& sample_key) const override;
    const ClipEntry* speech(const std::string& sample_key) const override;
    const ClipEntry* speech_for_file(const std::string& cd0_file) const override;
    const MusicDefaults& music_defaults() const override { return defaults_; }
    const MusicSlot* music_slot(const std::string& slot) const override;
    bool read_file(const FileRef& f, std::vector<uint8_t>& out, std::string& why) const override;
    std::unique_ptr<ByteSource> open_file(const FileRef& f, std::string& why) const override;

private:
    friend class Loader;
    struct GKey {
        uint64_t hash;
        int w, h;
        bool operator==(const GKey& o) const { return hash == o.hash && w == o.w && h == o.h; }
    };
    struct GKeyHash {
        size_t operator()(const GKey& k) const { return size_t((k.hash ^ (uint64_t(uint32_t(k.w)) << 40) ^ (uint64_t(uint32_t(k.h)) << 20)) * 0x9E3779B97F4A7C15ull); }
    };
    void rebuild_indices();

    PackContext ctx_;
    StackOptions opt_;
    std::vector<std::unique_ptr<Record>> records_;
    std::vector<PackInfo> infos_;                       // parallel to records_ (what packs() returns)
    std::vector<std::unique_ptr<Picture>> pictures_;
    size_t picture_bytes_ = 0, audio_bytes_ = 0;

    std::unordered_map<GKey, GraphicEntry, GKeyHash> graphics_, flashes_;
    std::unordered_map<uint64_t, FarLineRef> far_;
    BackgroundEntry backgrounds_[6];
    bool has_background_[6] = {};
    StarsEntry stars_;
    bool has_stars_ = false;
    std::vector<PaletteOverride> palettes_;
    PanelSet panel_;
    bool has_panel_ = false;
    std::vector<std::unique_ptr<ScreenEntry>> screens_;                   // by name, load order (an override replaces in place)
    std::vector<std::string> screen_names_;
    std::vector<const ScreenEntry*> screen_list_;
    std::unordered_map<uint64_t, std::vector<const ScreenEntry*>> screens_by_block_;
    std::unordered_map<std::string, ClipEntry> sfx_, speech_;
    std::unordered_map<std::string, std::string> speech_by_file_;         // cd0_file -> speech key
    std::unordered_map<uint32_t, int> clip_length_counts_;
    MusicDefaults defaults_;
    std::unordered_map<std::string, MusicSlot> slots_;
    std::unordered_map<std::string, std::string> origins_;                // "section/key" -> pack label that set it (override reports)
    std::vector<const ScreenEntry*> no_screens_;
};

// The load order of a run (AssetPackSettings: packs_enabled ids found in packs_dir, then pack_paths) as one stack; the same as
// load_packs (pack_api.h) but with the concrete type, for adapters. nullptr when no pack was asked for.
std::shared_ptr<PackStack> load_pack_stack(const AssetPackSettings& s, const PackContext& c, PackReport& report);

// "<16 hex>-<w>x<h>" into its parts (the key must pass valid_graphic_key).
bool parse_graphic_key(const std::string& key, uint64_t& hash, int& w, int& h);

} // namespace x2::pack
