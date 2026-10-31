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

void CdtvMusic::install(Machine& m) {
    m_ = &m;
    auto cue = [this](Machine& mm, Cue::Kind kind, int track, const char* what, uint32_t caller) {
        pending_.push_back({mm.paula.frames_out(), kind, track});
        ++cues_;
        if (log_)
            std::fprintf(log_, "%u %d %s %d %06X\n", mm.frame, mm.line, what, track, caller);
    };
    m.add_trap(kMusicInit, [this, cue](Machine& mm) {
        const uint32_t caller = mm.read32(mm.areg(7)) & 0xFFFFFF;
        int slot = 0;
        if (caller != kTitleReturn) {
            const int level = mm.read16(kLevelWord);
            slot = level >= 1 && level <= 5 ? level : 1;
        }
        cue(mm, Cue::Play, map_.track[size_t(slot)], slot ? "level" : "title", caller);
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
    pending_.push_back({m.paula.frames_out(), Cue::Play, track});
    ++cues_;
    if (log_) std::fprintf(log_, "%u %d play %d %06X\n", m.frame, m.line, track, caller);
}

void CdtvMusic::cue_stop(Machine& m, uint32_t caller) {
    pending_.push_back({m.paula.frames_out(), Cue::Stop, 0});
    ++cues_;
    if (log_) std::fprintf(log_, "%u %d stop 0 %06X\n", m.frame, m.line, caller);
}

bool CdtvMusic::cue_poll(Machine& m, int track, uint32_t caller) {
    if (!pending_.empty() || effective_source() == MusicSource::Amiga || wanted_ != track || !player_.finished()) return false;
    pending_.push_back({m.paula.frames_out(), Cue::Play, track});
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
    m_->paula.set_source_gains(s == MusicSource::Cdtv ? 0 : music_volume_ * 256 / 100, sfx_volume_ * 256 / 100);
    player_.set_volume(music_volume_);
    // With the CD as the only music there is no tracker voice to protect, so protect is a complete no-op
    // there: its output headroom must not quieten the CD either.
    const bool protect = protect_ && s != MusicSource::Cdtv;
    m_->paula.set_protect_music(protect, MusicShadow{kMusicShadow, kMusicShadowStride});
}

void CdtvMusic::set_source(MusicSource s) {
    const bool was_cd = effective_source() != MusicSource::Amiga;
    source_ = s;
    warn_no_disc();
    const bool is_cd = effective_source() != MusicSource::Amiga;
    if (is_cd && !was_cd && wanted_) player_.play(disc_->track(wanted_), wanted_); // switched on mid-tune
    if (!is_cd && was_cd) player_.stop();
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
    switch (c.kind) {
    case Cue::Play:
        wanted_ = c.track;
        if (cd) player_.play(disc_->track(c.track), c.track); // the CDTV game always restarts the track
        break;
    case Cue::Stop:
        wanted_ = 0;
        if (cd) player_.stop();
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
    uint64_t at = begin;
    auto render_to = [&](uint64_t to) {
        if (cd && to > at) player_.render(&out[size_t(at - buf_first) * 2], size_t(to - at));
        at = std::max(at, to);
    };
    for (const Cue& c : pending_) {
        render_to(std::clamp(c.at, begin, end));
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
    paused_ = false;
    player_.set_game_paused(false);
    player_.stop();
}

} // namespace x2::cdtv
