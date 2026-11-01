// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset-pack music (docs/re-notes/asset-packs-v2-design.md section 5.3, docs/re-notes/asset-packs-music.md): turns the music
// slots of the loaded pack library into streams the music player (cdtv/cd_player.h) plays.
//
//   MusicSlots slots(pack::packs());
//   MusicChoice c = slots.resolve(disk_slot_names(3));     // exact slot, then the level fallback
//   if (c.kind == MusicChoice::Track) player.play(c.spec, 1);
//
// A slot is opened and checked the first time it is resolved (or by prepare(), at load): the file is opened through the
// library, its header read against the decode limits, its length known, its loop points validated, and the first buffer
// decoded and kept, so that a cue starts without decoding at the cue. A slot that cannot be used (missing or damaged file,
// a format the decoder refuses, an intro that does not match) is reported once and skipped: the original music plays. Nothing
// here ever touches machine state.
#pragma once
#include "audio/decode.h"
#include "audio/stream_source.h"
#include "pack/pack_api.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x2::audio {

// What a cue resolved to.
struct MusicChoice {
    enum Kind : uint8_t {
        Original, // no pack slot: the game's own music (tracker, or the CD track) plays
        Silent,   // a slot with `mute`: the original is silenced and nothing plays
        Track,    // play `spec`
    } kind = Original;
    std::string slot;                                  // the manifest key that matched ("level3", "cd5", ...)
    std::shared_ptr<const StreamSpec> spec;            // Track only
    pack::Retrigger retrigger = pack::Retrigger::Restart;
};

// Slot names to try for a cue, best first (exact, role alias, `level`).
//   disk: `slot` is 0 for the title, 1-5 for a level.
std::vector<std::string> disk_slot_names(int slot);
//   CDTV: the game asked for CD track `track` (2-13); `role` says from where (title screen, a level, anything else);
//   `level` (1-5) is the game's level word at the cue.
enum class CdtvRole : uint8_t { Other, Title, Level };
std::vector<std::string> cdtv_slot_names(CdtvRole role, int level, int track);

class MusicSlots {
public:
    // `lib` must outlive this object and everything resolve() returned (the specs open files through it).
    explicit MusicSlots(const pack::PackLibrary& lib, DecodeLimits limits = {});

    // Resolves the first name the library has a slot for, in order. A slot that fails to open is skipped with a warning (once).
    MusicChoice resolve(const std::vector<std::string>& names);
    // Opens and checks a slot ahead of its first cue. Returns false when it has a slot but cannot be used.
    bool prepare(const std::string& name);

    // Warnings and errors found so far, formatted as "asset pack: <pack>: music["slot"]: ...". Drained by the caller.
    pack::PackReport take_report();

    // Frames decoded ahead per stream (the first buffer).
    static constexpr size_t kHeadFrames = 16384;

private:
    struct Entry {
        bool usable = false, mute = false;
        std::shared_ptr<const StreamSpec> spec;
        pack::Retrigger retrigger = pack::Retrigger::Restart;
        std::string note; // how the loop was decided, for the log
    };
    const Entry& entry(const std::string& name, const pack::MusicSlot& slot);
    bool build(const pack::MusicSlot& slot, Entry& e);
    void warn(const pack::MusicSlot& slot, const std::string& text);
    std::string pack_name(const pack::FileRef& f) const;

    const pack::PackLibrary& lib_;
    DecodeLimits limits_;
    std::map<std::string, Entry> entries_;
    pack::PackReport report_;
};

} // namespace x2::audio
