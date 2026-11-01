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
//
// Asset-pack music (audio/music_slots.h, docs/re-notes/asset-packs-music.md) uses the same cues. At a cue the slot names for
// that moment are tried against the loaded packs (disk: title, levelN, level; CDTV: cdN by the track the game asked for,
// then title / levelN / level by who asked); a slot that matches takes the music: the CD track is not started, on the disk
// build the tracker's music is muted in Paula's mix from that very instruction (the trap) and the pack track starts at the
// cue's output frame in the same player. Nothing but the output mix changes. A slot with `mute` only silences.
#pragma once
#include "audio/music_slots.h"
#include "cdtv/cd_player.h"
#include "cdtv/cdtv_disc.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
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
// The CDTV release: its level word, and the return addresses (24 bit) of the game's calls of the CD-play routine, by who calls
// (docs/re-notes/asset-packs-music.md): the title screen, and the level start-ups. Any other caller is "other" (CD-only screens).
constexpr uint32_t kCdtvLevelWord = 0xE0A;
constexpr std::array<uint32_t, 3> kCdtvTitleCallers = {0x03C822, 0x03B95C, 0x03AD6A};
constexpr std::array<uint32_t, 3> kCdtvLevelCallers = {0x03ADB6, 0x03B45A, 0x03B2D6};
audio::CdtvRole cdtv_caller_role(uint32_t caller);
constexpr uint32_t kEffectCode = 0x1B26A, kDriverEnd = 0x1B6D0, kMusicRestore = 0x1B5D2, kMusicPtr = 0x2090;
constexpr uint32_t kPauseEnter = 0x79BA, kPauseLeave = 0x79CA;
// The driver's shadow channel records, `$1B762 + ch*$C`: the copy of AUDxLC/LEN/PER/VOL it keeps up to date on
// every tick whether or not a sound effect owns the channel (`$1A1EE`, `$1A1F2`, `$1A278`, `$1A27E`, `$1A28C`,
// `$1A2BE`, `$1A3A2`, `$1A3D4` are all unguarded; only the Paula writes after them test ownership at `$B(a4)`).
// Paula's mirror voices play from it for --protect-music (docs/re-notes/music-dropouts.md §8).
constexpr uint32_t kMusicShadow = 0x1B762, kMusicShadowStride = 0xC;

// Where the resident driver's effect and music code lie in a disk build (see the notes above): the disk builds share the canonical
// addresses (the constants above), the 1762 compilation has its own (modern/layout.h kDisk1762Audio).
struct EffectTags {
    uint32_t effect_lo = kEffectCode, effect_hi = kDriverEnd, music_restore = kMusicRestore, music_lo = kMusicInit;
    uint32_t music_tick = kMusicTick, music_ptr = kMusicPtr;
};

class CdtvMusic {
public:
    // Replaces the addresses the writer tags use (before install()).
    void set_effect_tags(const EffectTags& t) { tags_ = t; }
    // Installs the traps (cue and writer tags). Call once, before running. Disk builds only: the traps sit in the
    // tracker driver, which the CDTV release does not have.
    void install(Machine& m);
    // The CDTV release's own use of the layer: the game plays CD tracks itself (cdtv/cdtv_game.h), so there are no traps
    // here and no tracker voices to tag or mute. Call instead of install(), once, before running.
    void attach_game(Machine& m);
    // The game's own CD commands, queued at the Paula output frame where it issued them.
    // `caller` (the game's return address) is only logged.
    void cue_play(Machine& m, int track, uint32_t caller = 0);
    void cue_stop(Machine& m, uint32_t caller = 0);
    // The game's once-a-second CheckIO of the request for `track`: true when the track has played out, in which case the
    // game plays it again and the restart is queued like a cue (a no-op while a cue is still waiting to be applied, for a
    // track other than the playing one, with the Amiga source, or while the track is still playing).
    bool cue_poll(Machine& m, int track, uint32_t caller = 0);
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
    // True while an asset pack owns the music (a slot matched the last cue): the tracker is muted in the mix / the CD track is not played.
    bool pack_owns_music() const { return pack_owns_; }
    // The slot of the pack track now playing ("" when none).
    const std::string& pack_slot() const { return pack_slot_; }

    // After each emulated frame: renders the CD into the Paula frames that frame produced (still in
    // m.paula.output()), applying each cue at the output frame where it happened; then one loop-poll tick.
    void after_frame(Machine& m);
    // Host reset: the CD stops (the game starts its title tune again from boot).
    void reset();

    int wanted_track() const { return wanted_; } // the track the game's state asks for (0 = none)
    uint32_t cues() const { return cues_; }
    uint32_t restarts() const { return restarts_; } // finished tracks the game started again (cue_poll)

private:
    struct Cue {
        uint64_t at;  // Paula output frame
        enum Kind { Play, Stop, Pause, Resume } kind;
        int track;
        audio::MusicChoice choice; // Play: what the packs decided at the cue (Original: no pack slot)
        bool again = false;        // Play: the game's own restart of a finished track
    };
    // The cue's moment, kept to decide again when the packs are switched on or off (F1) mid-tune.
    struct Moment {
        bool valid = false;
        int track = 0, slot = 0, level = 0; // disk: slot 0 title, 1-5 level; CDTV: the level word
        audio::CdtvRole role = audio::CdtvRole::Other;
    };
    void apply(const Cue& c);
    void apply_gains();
    void warn_no_disc();
    bool packs_wanted() const;
    audio::MusicChoice choose(const Moment& mo);
    // Resolves a play cue against the packs, mutes the tracker at once if a slot matches, and queues the cue.
    void queue_play(Machine& m, const Moment& mo, bool again, const char* what, uint32_t caller,
                    const audio::MusicChoice* decided = nullptr);

    EffectTags tags_;
    Machine* m_ = nullptr;
    const Disc* disc_ = nullptr;
    MusicSource source_ = MusicSource::Amiga;
    MusicMap map_;
    CdPlayer player_;
    std::vector<Cue> pending_;
    std::FILE* log_ = nullptr;
    int wanted_ = 0, music_volume_ = 100, sfx_volume_ = 100;
    uint32_t cues_ = 0, restarts_ = 0;
    uint64_t mixed_ = 0;
    bool warned_ = false, paused_ = false, protect_ = false, game_mode_ = false;
    int game_volume_ = 100;
    // Asset-pack music
    std::unique_ptr<audio::MusicSlots> slots_;
    const pack::PackLibrary* slots_lib_ = nullptr;
    Moment last_;
    std::string pack_slot_;
    bool pack_owns_ = false;   // a slot matched the last play cue (track or mute)
    bool pack_voice_ = false;  // the player's current voice is a pack track
    bool packs_seen_ = false;  // packs_wanted() when the last cue was decided
};

} // namespace x2::cdtv
