// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cdtv/cdtv_music.h"

#include "machine.h"

extern "C" {
#include "m68k.h"
}

#include <algorithm>

namespace x2::cdtv {

bool parse_music_source(const std::string& s, MusicSource& out) {
    if (s == "amiga") out = MusicSource::Amiga;
    else if (s == "cdtv") out = MusicSource::Cdtv;
    else if (s == "mix") out = MusicSource::Mix;
    else return false;
    return true;
}

const char* music_source_name(MusicSource s) {
    switch (s) {
    case MusicSource::Amiga: return "amiga";
    case MusicSource::Cdtv: return "cdtv";
    case MusicSource::Mix: return "mix";
    }
    return "?";
}

bool parse_pause_mode(const std::string& s, PauseMode& out) {
    if (s == "keep") out = PauseMode::Keep;
    else if (s == "duck") out = PauseMode::Duck;
    else if (s == "stop") out = PauseMode::Stop;
    else return false;
    return true;
}

const char* pause_mode_name(PauseMode m) {
    switch (m) {
    case PauseMode::Keep: return "keep";
    case PauseMode::Duck: return "duck";
    case PauseMode::Stop: return "stop";
    }
    return "?";
}

const char* track_title(int track) {
    static const char* const titles[12] = {
        "Megablast (Xenon II Mix)", "Megablast (Instrumental)", "Megablast", "Megablast (Hip Hop on Precinct 13)",
        "Megablast (Original Rap feat. Merlin)", "Megablast (Rap feat. Merlin)", "Desire Throughout the Entire World",
        "Doin the Do", "Theme from S-Express", "Nothing to Lose", "Boos Booming", "Oochy Koochy",
    };
    return track >= 2 && track <= 13 ? titles[track - 2] : "";
}

audio::CdtvRole cdtv_caller_role(uint32_t caller) {
    caller &= 0xFFFFFF;
    if (std::find(kCdtvTitleCallers.begin(), kCdtvTitleCallers.end(), caller) != kCdtvTitleCallers.end()) return audio::CdtvRole::Title;
    if (std::find(kCdtvLevelCallers.begin(), kCdtvLevelCallers.end(), caller) != kCdtvLevelCallers.end()) return audio::CdtvRole::Level;
    return audio::CdtvRole::Other;
}

bool CdtvMusic::packs_wanted() const {
    return pack::pack_settings().audio_active() && pack::packs().has_audio();
}

audio::MusicChoice CdtvMusic::choose(const Moment& mo) {
    packs_seen_ = packs_wanted();
    if (!packs_seen_) return {};
    const pack::PackLibrary& lib = pack::packs();
    if (!slots_ || slots_lib_ != &lib) {
        slots_ = std::make_unique<audio::MusicSlots>(lib);
        slots_lib_ = &lib;
    }
    audio::MusicChoice c = slots_->resolve(game_mode_ ? audio::cdtv_slot_names(mo.role, mo.level, mo.track) : audio::disk_slot_names(mo.slot));
    for (const pack::PackMessage& msg : slots_->take_report().messages) std::fprintf(stderr, "%s\n", msg.format().c_str());
    return c;
}

void CdtvMusic::queue_play(Machine& m, const Moment& mo, bool again, const char* what, uint32_t caller,
                           const audio::MusicChoice* decided) {
    last_ = mo;
    last_.valid = true;
    Cue c{m.paula.frames_out(), Cue::Play, mo.track, {}, again};
    if (!again) c.choice = decided ? *decided : choose(mo);
    if (!again) {
        // The tracker is muted from the cue itself (this runs inside the emulation, at the driver's entry), so none of the
        // original tune reaches the mix.
        const bool owns = c.choice.kind != audio::MusicChoice::Original;
        if (owns != pack_owns_) {
            pack_owns_ = owns;
            apply_gains();
        }
    }
    pending_.push_back(std::move(c));
    ++cues_;
    if (log_) {
        const char* kind = pending_.back().choice.kind == audio::MusicChoice::Track    ? " pack:"
                           : pending_.back().choice.kind == audio::MusicChoice::Silent ? " mute:" : "";
        std::fprintf(log_, "%u %d %s %d %06X%s%s\n", m.frame, m.line, what, mo.track, caller, kind,
                     pending_.back().choice.kind == audio::MusicChoice::Original ? "" : pending_.back().choice.slot.c_str());
    }
}

void CdtvMusic::install(Machine& m) {
    m_ = &m;
    auto cue = [this](Machine& mm, Cue::Kind kind, int track, const char* what, uint32_t caller) {
        pending_.push_back({mm.paula.frames_out(), kind, track, {}, false});
        ++cues_;
        if (log_)
            std::fprintf(log_, "%u %d %s %d %06X\n", mm.frame, mm.line, what, track, caller);
    };
    m.add_trap(kMusicInit, [this](Machine& mm) {
        const uint32_t caller = mm.read32(mm.areg(7)) & 0xFFFFFF;
        int slot = 0;
        if (caller != kTitleReturn) {
            const int level = mm.read16(kLevelWord);
            slot = level >= 1 && level <= 5 ? level : 1;
        }
        Moment mo;
        mo.track = map_.track[size_t(slot)];
        mo.slot = slot;
        queue_play(mm, mo, false, slot ? "level" : "title", caller);
    });
    m.add_trap(kMusicStop, [cue](Machine& mm) {
        const uint32_t caller = mm.read32(mm.areg(7)) & 0xFFFFFF;
        if (caller != kInitInternalStop) cue(mm, Cue::Stop, 0, "stop", caller);
    });
    m.add_trap(kPauseEnter, [cue](Machine& mm) { cue(mm, Cue::Pause, 0, "pause", 0); });
    m.add_trap(kPauseLeave, [cue](Machine& mm) { cue(mm, Cue::Resume, 0, "resume", 0); });
    // Writer of each AUDxLC write, by the address of the writing instruction (M68K_REG_PPC, which the
    // translated code keeps as the interpreter does): the effect code of the resident driver, its music code, or
    // whatever driver ($2090) currently points at (the shop/ending module's own driver counts as effects).
    m.paula.set_writer_query([this]() {
        const uint32_t pc = m68k_get_reg(nullptr, M68K_REG_PPC) & 0xFFFFFF;
        if (pc >= kEffectCode && pc < kDriverEnd && pc != kMusicRestore) return AudioSource::Effect;
        if (pc >= kMusicInit && pc < kDriverEnd) return AudioSource::Music;
        return m_->read32(kMusicPtr) == kMusicTick ? AudioSource::Music : AudioSource::Effect;
    });
    apply_gains();
}

void CdtvMusic::attach_game(Machine& m) {
    m_ = &m;
    game_mode_ = true;
    player_.set_self_loop(false); // the game's own poll (cdtv_game.cpp) decides when a finished track starts again
    apply_gains();
}

void CdtvMusic::cue_play(Machine& m, int track, uint32_t caller) {
    Moment mo;
    mo.track = track;
    mo.role = cdtv_caller_role(caller);
    mo.level = m.read16(kCdtvLevelWord);
    queue_play(m, mo, false, "play", caller);
}

void CdtvMusic::cue_stop(Machine& m, uint32_t caller) {
    pending_.push_back({m.paula.frames_out(), Cue::Stop, 0, {}, false});
    ++cues_;
    if (log_) std::fprintf(log_, "%u %d stop 0 %06X\n", m.frame, m.line, caller);
}

bool CdtvMusic::cue_poll(Machine& m, int track, uint32_t caller) {
    if (!pending_.empty() || (effective_source() == MusicSource::Amiga && !pack_voice_) || wanted_ != track || !player_.finished())
        return false;
    pending_.push_back({m.paula.frames_out(), Cue::Play, track, {}, true});
    ++cues_;
    ++restarts_;
    if (log_) std::fprintf(log_, "%u %d again %d %06X\n", m.frame, m.line, track, caller);
    return true;
}

void CdtvMusic::set_game_volume(int percent) {
    game_volume_ = std::clamp(percent, 0, 100);
    apply_gains();
}

void CdtvMusic::warn_no_disc() {
    if (warned_ || source_ == MusicSource::Amiga || disc_) return;
    warned_ = true;
    if (game_mode_) {
        std::fprintf(stderr, "x2recomp: no CDTV disc audio: the music will be silent (import the disc with --import PATH, or "
                             "--import-cdtv PATH for the audio only, or give --cdtv PATH)\n");
        return;
    }
    std::fprintf(stderr, "x2recomp: --music %s: no CDTV disc audio (import it with --import-cdtv PATH or give --cdtv PATH); "
                         "using the Amiga music\n", music_source_name(source_));
}

void CdtvMusic::apply_gains() {
    if (!m_) return;
    if (game_mode_) {
        // The CDTV release: the CD is the music and Paula carries only effects and speech, so Paula's gains and the
        // protect-music voices (a tracker-music feature) stay as they are.
        player_.set_volume(music_volume_ * game_volume_ / 100);
        return;
    }
    const MusicSource s = effective_source();
    // A pack slot that owns the music silences the tracker as the CD does, and has no tracker voice to protect either.
    const bool tracker_off = s == MusicSource::Cdtv || pack_owns_;
    m_->paula.set_source_gains(tracker_off ? 0 : music_volume_ * 256 / 100, sfx_volume_ * 256 / 100);
    player_.set_volume(music_volume_);
    // With the CD as the only music there is no tracker voice to protect, so protect is a complete no-op
    // there: its output headroom must not quieten the CD either.
    const bool protect = protect_ && !tracker_off;
    m_->paula.set_protect_music(protect, MusicShadow{kMusicShadow, kMusicShadowStride});
}

void CdtvMusic::set_source(MusicSource s) {
    const bool was_cd = effective_source() != MusicSource::Amiga;
    source_ = s;
    warn_no_disc();
    const bool is_cd = effective_source() != MusicSource::Amiga;
    if (is_cd && !was_cd && wanted_ && !pack_owns_) player_.play(disc_->track(wanted_), wanted_); // switched on mid-tune
    if (!is_cd && was_cd && !pack_voice_) player_.stop();
    apply_gains();
}

void CdtvMusic::set_volumes(int music_percent, int sfx_percent) {
    music_volume_ = std::clamp(music_percent, 0, 100);
    sfx_volume_ = std::clamp(sfx_percent, 0, 100);
    apply_gains();
}

void CdtvMusic::set_protect_music(bool on) {
    protect_ = on;
    apply_gains();
}

void CdtvMusic::apply(const Cue& c) {
    const bool cd = effective_source() != MusicSource::Amiga;
    using audio::MusicChoice;
    switch (c.kind) {
    case Cue::Play:
        wanted_ = c.track;
        if (c.again && pack_voice_) { // a finished pack track the game starts again (the Game loop rule)
            player_.restart();
        } else if (c.choice.kind == MusicChoice::Track) {
            const bool same = pack_voice_ && pack_slot_ == c.choice.slot;
            if (same && c.choice.retrigger == pack::Retrigger::Continue) break; // the slot keeps playing through the cue
            player_.play(c.choice.spec, c.track > 0 ? c.track : 1);
            pack_voice_ = true;
            pack_slot_ = c.choice.slot;
        } else if (c.choice.kind == MusicChoice::Silent) {
            if (cd || pack_voice_) player_.stop();
            pack_voice_ = false;
            pack_slot_ = c.choice.slot;
        } else {
            const bool was_pack = pack_voice_;
            pack_voice_ = false;
            pack_slot_.clear();
            if (cd) player_.play(disc_->track(c.track), c.track); // the CDTV game always restarts the track
            else if (was_pack) player_.stop();
        }
        break;
    case Cue::Stop:
        wanted_ = 0;
        if (cd || pack_voice_) player_.stop();
        pack_voice_ = false;
        pack_slot_.clear();
        break;
    case Cue::Pause:
    case Cue::Resume:
        paused_ = c.kind == Cue::Pause;
        player_.set_game_paused(paused_);
        break;
    }
}

void CdtvMusic::after_frame(Machine& m) {
    std::vector<int16_t>& out = m.paula.output();
    const uint64_t end = m.paula.frames_out();
    const uint64_t begin = std::max<uint64_t>(mixed_, end - std::min<uint64_t>(end, out.size() / 2));
    const uint64_t buf_first = end - out.size() / 2;
    const bool cd = effective_source() != MusicSource::Amiga;
    // The packs were switched on or off (F1, --faithful, the audio toggle) since the last cue: decide the current cue again.
    if (last_.valid && packs_wanted() != packs_seen_) {
        const Moment mo = last_;
        const bool owned = pack_owns_;
        const audio::MusicChoice ch = choose(mo); // also records the new state
        if (owned || ch.kind != audio::MusicChoice::Original) queue_play(m, mo, false, "refresh", 0, &ch);
    }
    uint64_t at = begin;
    auto render_to = [&](uint64_t to) {
        // The CD, or a pack track (and whatever of either is still fading out); idle otherwise, as it always was.
        if ((cd || !player_.idle()) && to > at) player_.render(&out[size_t(at - buf_first) * 2], size_t(to - at));
        at = std::max(at, to);
    };
    for (size_t i = 0; i < pending_.size(); ++i) {
        const Cue& c = pending_[i];
        render_to(std::clamp(c.at, begin, end));
        // The CDTV game stops the track before it plays one (cd_play): a Continue slot that is asked for again keeps playing.
        if (c.kind == Cue::Stop && pack_voice_ && i + 1 < pending_.size()) {
            const Cue& n = pending_[i + 1];
            if (n.kind == Cue::Play && n.at == c.at && n.choice.kind == audio::MusicChoice::Track && n.choice.slot == pack_slot_ &&
                n.choice.retrigger == pack::Retrigger::Continue)
                continue;
        }
        apply(c);
    }
    pending_.clear();
    render_to(end);
    mixed_ = end;
    player_.tick();
}

void CdtvMusic::reset() {
    pending_.clear();
    wanted_ = 0;
    last_ = Moment();
    pack_voice_ = false;
    pack_slot_.clear();
    if (pack_owns_) {
        pack_owns_ = false;
        apply_gains();
    }
    paused_ = false;
    player_.set_game_paused(false);
    player_.stop();
}

} // namespace x2::cdtv
