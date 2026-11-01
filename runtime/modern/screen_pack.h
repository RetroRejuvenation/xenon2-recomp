// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A PackLibrary that holds screens and the status panel (pack/pack_api.h), for the screens workstream's end-to-end use and tests.
//
// It is built in memory (tests) or loaded from a pack directory's manifest.json (`screens` and `panel` sections only; every other
// section is left to the general loader, WS1b, which installs the full library and replaces this one). Pictures are PNGs inside
// the directory, decoded with the limits of modern/assets.h. Everything a pack author gets wrong is a message and the entry is
// skipped; nothing here throws.
#pragma once
#include "pack/pack_api.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2::modern {

class ScreenPackLibrary final : public pack::PackLibrary {
public:
    explicit ScreenPackLibrary(pack::PackContext ctx = {}) : ctx_(std::move(ctx)) {}

    // Loads the pack directory `dir`. Returns false when the pack cannot be used at all (no manifest, wrong format or version,
    // a scope that excludes this context); problems with single entries are appended to `report`.
    bool load(const std::string& dir, pack::PackReport& report);

    // Building (tests, and the loader itself). Pictures live as long as the library.
    const pack::Picture* add_picture(pack::Picture p);
    // Adds a screen; one with the same name replaces the earlier one in place (later packs override).
    const pack::ScreenEntry* add_screen(pack::ScreenEntry e);
    pack::PanelSet& panel_set() { return panel_; }

    // PackLibrary
    const pack::PackContext& context() const override { return ctx_; }
    const std::vector<pack::PackInfo>& packs() const override { return infos_; }
    bool has_graphics() const override { return !screens_.empty() || !panel_.empty(); }
    bool has_audio() const override { return false; }
    const pack::GraphicEntry* graphic(uint64_t, int, int) const override { return nullptr; }
    const pack::GraphicEntry* flash(uint64_t, int, int) const override { return nullptr; }
    const pack::FarLineRef* far_line(uint64_t) const override { return nullptr; }
    const pack::BackgroundEntry* background(int) const override { return nullptr; }
    const pack::StarsEntry* stars() const override { return nullptr; }
    const pack::PaletteOverride* palette(const uint16_t*) const override { return nullptr; }
    const pack::PanelSet* panel() const override { return panel_.empty() ? nullptr : &panel_; }
    const std::vector<const pack::ScreenEntry*>& screens() const override { return screens_; }
    const std::vector<const pack::ScreenEntry*>& screens_for_block(uint64_t first_block_hash) const override;
    bool has_clip_length(uint32_t) const override { return false; }
    const pack::ClipEntry* sfx(const std::string&) const override { return nullptr; }
    const pack::ClipEntry* speech(const std::string&) const override { return nullptr; }
    const pack::ClipEntry* speech_for_file(const std::string&) const override { return nullptr; }
    const pack::MusicDefaults& music_defaults() const override { return music_defaults_; }
    const pack::MusicSlot* music_slot(const std::string&) const override { return nullptr; }
    bool read_file(const pack::FileRef&, std::vector<uint8_t>&, std::string& why) const override {
        why = "not supported";
        return false;
    }
    std::unique_ptr<pack::ByteSource> open_file(const pack::FileRef&, std::string& why) const override {
        why = "not supported";
        return nullptr;
    }

private:
    pack::PackContext ctx_;
    std::vector<pack::PackInfo> infos_;
    std::deque<pack::Picture> pictures_;
    std::deque<pack::ScreenEntry> entries_;
    std::vector<const pack::ScreenEntry*> screens_;
    pack::PanelSet panel_;
    pack::MusicDefaults music_defaults_;
    mutable std::unordered_map<uint64_t, std::vector<const pack::ScreenEntry*>> by_block_;
    mutable bool by_block_valid_ = false;
};

} // namespace x2::modern
