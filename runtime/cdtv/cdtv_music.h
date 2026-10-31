// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// CDTV music layer (Phase 8, off by default): plays the CDTV release's CD tracks where the disk version starts
// its one tracker tune, and silences the tracker music while keeping the sound effects.
//
// Cues (docs/re-notes/cdtv-music-map.md): PC traps on the resident driver's entry points, the same mechanism as
// the disk shim, so they behave identically under --interp-only and the translated executor. A trap only reads
// state; nothing in the machine changes.
//   - music_init $19F9E called from $8230 (return $8236): the title tune -> CD track for TITLES (default 2).
//   - music_init from the in-game starts $7896, $7C50, $7CDA, $7D8C: the level tune -> CD track for LEVEL n,
//     n = the level number word $E04 (1-5; defaults L1=3, L2=5, L3=2, L4=3, L5=5, as the CDTV table $3EE26).
//   - music_stop $19FBA (except its internal call from music_init, return $1A00C): stop the CD.
//   - the in-game pause loop: entered at $79BA, left at $79CA (pause_mode: keep, duck or hold the CD).
// Where the CDTV game plays its own CD-only screens (music select, info pictures: tracks 8-13 via $5047E)
// the disk game has nothing, so those tracks are never chosen.
//
// Music/effect separation: the resident driver plays both. Its effect code ($1B26A-$1B6CF: the command routine
// called through ($2094), and the effect records it starts, which the music tick then plays, e.g. the menu
// "select" sound) and its music code ($19F9E-$1B269, plus $1B5D2, which gives a channel back to the music when an
// effect ends) are told apart by the address of the instruction that writes AUDxLC. Writes from anywhere else
// count as music while ($2090) points at the resident tick $19FAC, and as effects while the shop/ending module's
// own driver is installed: the CDTV game stops the CD there and plays no other track, so the module's music and
// sounds stay audible. Paula records the writer of each channel's sample pointer, takes it over when the channel
// latches that pointer, and applies a gain per writer to its output mix only.
#pragma once
#include "cdtv/cd_player.h"
#include "cdtv/cdtv_disc.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x2 {
class Machine;
}

namespace x2::cdtv {

enum class MusicSource { Amiga, Cdtv, Mix }; // tracker music only (default), CD only, both
bool parse_music_source(const std::string& s, MusicSource& out);
const char* music_source_name(MusicSource s);
bool parse_pause_mode(const std::string& s, PauseMode& out);
const char* pause_mode_name(PauseMode m);

// CD track per music slot: 0 = title, 1-5 = levels (the CDTV game's defaults, table $3EE26).
struct MusicMap {
    std::array<int, 6> track = {2, 3, 5, 2, 3, 5};
};
// Titles of the CD tracks 2-13 (40-byte records at $3EC32 in the CDTV X2), for messages.
const char* track_title(int track);

// Disk-version addresses (resident driver and its callers; docs/re-notes/audio-notes.md).
constexpr uint32_t kMusicInit = 0x19F9E, kMusicStop = 0x19FBA, kMusicTick = 0x19FAC;
constexpr uint32_t kTitleReturn = 0x8236, kInitInternalStop = 0x1A00C;
constexpr uint32_t kLevelWord = 0xE04;
constexpr uint32_t kEffectCode = 0x1B26A, kDriverEnd = 0x1B6D0, kMusicRestore = 0x1B5D2, kMusicPtr = 0x2090;
constexpr uint32_t kPauseEnter = 0x79BA, kPauseLeave = 0x79CA;
// The driver's shadow channel records, `$1B762 + ch*$C`: the copy of AUDxLC/LEN/PER/VOL it keeps up to date on
// every tick whether or not a sound effect owns the channel (`$1A1EE`, `$1A1F2`, `$1A278`, `$1A27E`, `$1A28C`,
// `$1A2BE`, `$1A3A2`, `$1A3D4` are all unguarded; only the Paula writes after them test ownership at `$B(a4)`).
// Paula's mirror voices play from it for --protect-music (docs/re-notes/music-dropouts.md §8).
constexpr uint32_t kMusicShadow = 0x1B762, kMusicShadowStride = 0xC;

class CdtvMusic {
public:
    // Installs the traps (cue and writer tags). Call once, before running. Disk builds only: the traps sit in the
    // tracker driver, which the CDTV release does not have.
    void install(Machine& m);
    // The CDTV release's own use of the layer: the game plays CD tracks itself (cdtv/cdtv_game.h), so there are no traps
    // here and no tracker voices to tag or mute. Call instead of install(), once, before running.
    void attach_game(Machine& m);
    // The game's own CD commands, queued at the Paula output frame where it issued them.
    void cue_play(Machine& m, int track);
    void cue_stop(Machine& m);
    // The game's own volume command (full = 100, mute = 0), applied on top of the music volume.
    void set_game_volume(int percent);
    // The disc to play from (nullptr: none, the CD source falls back to the Amiga music with one warning).
    void set_disc(const Disc* disc) { disc_ = disc; }
    void set_source(MusicSource s);
    MusicSource source() const { return source_; }
    MusicSource effective_source() const { return disc_ ? source_ : MusicSource::Amiga; }
    void set_volumes(int music_percent, int sfx_percent);
    // Optional: keep the tune's voices audible through the sound effects that take their channels
    // (runtime/audio/paula.h, MusicShadow). Off by default; a deliberate deviation from the original.
    void set_protect_music(bool on);
    bool protect_music() const { return protect_; }
    void set_map(const MusicMap& map) { map_ = map; }
    CdPlayer& player() { return player_; }
    // Cue log: "frame line event slot track caller" per cue (tests and docs).
    void set_log(std::FILE* f) { log_ = f; }

    // After each emulated frame: renders the CD into the Paula frames that frame produced (still in
    // m.paula.output()), applying each cue at the output frame where it happened; then one loop-poll tick.
    void after_frame(Machine& m);
    // Host reset: the CD stops (the game starts its title tune again from boot).
    void reset();

    int wanted_track() const { return wanted_; } // the track the game's state asks for (0 = none)
    uint32_t cues() const { return cues_; }

private:
    struct Cue {
        uint64_t at;  // Paula output frame
        enum Kind { Play, Stop, Pause, Resume } kind;
        int track;
    };
    void apply(const Cue& c);
    void apply_gains();
    void warn_no_disc();

    Machine* m_ = nullptr;
    const Disc* disc_ = nullptr;
    MusicSource source_ = MusicSource::Amiga;
    MusicMap map_;
    CdPlayer player_;
    std::vector<Cue> pending_;
    std::FILE* log_ = nullptr;
    int wanted_ = 0, music_volume_ = 100, sfx_volume_ = 100;
    uint32_t cues_ = 0;
    uint64_t mixed_ = 0;
    bool warned_ = false, paused_ = false, protect_ = false, game_mode_ = false;
    int game_volume_ = 100;
};

} // namespace x2::cdtv
