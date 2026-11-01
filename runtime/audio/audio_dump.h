// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// `--dump-audio DIR` (docs/re-notes/asset-packs-v2-design.md section 5.5, docs/re-notes/asset-packs-sfx.md): records every distinct
// effect and speech sample the game starts during a run and writes them as an author's starting pack.
//
//   DIR/sfx/<key>.wav      the ORIGINAL sample, mono 16-bit, at round(colour clock / ref_period) Hz (or a whole multiple of it when
//                          that is under 8 kHz, by sample repetition, so the file is one the decoders accept)
//   DIR/speech/<key>.wav   the same for CDTV speech
//   DIR/music_cues.txt     the music cue log, when the music layer reports cues (note_cue)
//   DIR/manifest.json      a valid version 2 pack: audio.sfx / audio.speech entries pointing at those files, plus the dump-only fields
//                          bytes, uses, first_frame, channels, periods, peak, source, cd0_file
//
// The dump merges into what is already there: an existing manifest keeps all its other sections, an entry that is already listed is
// left exactly as it is, and a WAV file that exists is never overwritten, so an author can edit the files and the entries, run
// another playthrough to catch more sounds, and lose nothing. The files are the game's own audio: user data, never to distribute.
#pragma once
#include "audio/pack_audio.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace x2::audio {

class AudioDumper {
public:
    static constexpr uint32_t kMinBytes = 16;  // shorter buffers are the channel parking loops (CDTV: 4 bytes), not sounds
    static constexpr size_t kMaxEntries = 4096;
    static constexpr size_t kMaxBytes = size_t(256) << 20;

    // Creates DIR (and sfx/, speech/ when something is written). False with `why` set when DIR cannot be made.
    bool open(const std::string& dir, std::string& why);

    // PackAudio calls these.
    void on_sample(const SampleStart& s);
    // The game changed AUDxPER while the channel plays the last sample started. A change within kParkMs of the channel's end is the
    // parking write the effect code makes before DMA off (period 124), not a pitch of the sound, and is left out of `periods`.
    void on_period(int channel, uint16_t period, uint64_t frame);
    // The channel's buffer ran out and started over on the same buffer: the game loops this sound (the entry gets "loop": true).
    void on_loop(int channel);
    void on_channel_end(int channel, uint64_t frame);  // the channel stopped or started something else
    static constexpr int kParkMs = 3;
    void set_rate(int hz) { rate_ = hz; }  // the Paula output rate the frames of the events are counted at (default 48000)

    // The music layer's cues, for slot discovery: written to music_cues.txt by finish().
    void note_cue(uint64_t frame, const std::string& slot, const std::string& state, uint32_t caller);

    // Writes the WAV files and merges the audio section into DIR/manifest.json. Call after any other dumper has written its manifest
    // (--dump-assets rewrites the file). False with `why` set on a write failure or a manifest that is not a JSON object.
    bool finish(std::string& why);

    size_t sfx_count() const { return count(false); }
    size_t speech_count() const { return count(true); }
    size_t skipped() const { return skipped_; }  // distinct samples dropped by the caps
    const std::vector<std::string>& warnings() const { return warnings_; }

    // The `"audio": {...}` member of DIR/manifest.json as text ("" when there is none or the file is not a JSON object), for a dumper
    // that rewrites the manifest (--dump-assets) to carry the section over.
    static std::string existing_audio_member(const std::string& dir);

    // Pure helpers (tests): the dump file's rate and its sample-repetition factor for a ref_period, and the WAV bytes of a sample.
    static int file_rate(int ref_period, int* repeat = nullptr);
    static std::vector<uint8_t> wav_bytes(const uint8_t* sample, uint32_t bytes, int ref_period);

private:
    struct Entry {
        std::string key;
        std::vector<uint8_t> data;
        uint64_t uses = 0, first_frame = 0;
        uint32_t channels = 0;
        uint16_t pmin = 0xFFFF, pmax = 0;
        std::map<uint16_t, uint32_t> start_periods;
        int peak = 0;
        bool speech = false, loops = false;
        std::string cd0_file;
    };
    size_t count(bool speech) const;
    int ref_period_of(const Entry& e) const;

    std::string dir_;
    std::map<std::string, Entry> entries_;
    std::string current_key_[4];  // the sample each channel plays, while it is one the dump has
    struct Pending {
        bool set = false;
        uint16_t period = 0;
        uint64_t frame = 0;
    } pending_[4];                // the last period change of each channel, not yet counted
    void flush_pending(int channel);
    size_t bytes_ = 0, skipped_ = 0;
    int rate_ = 48000;
    std::vector<std::string> warnings_;
    struct Cue {
        uint64_t frame;
        std::string slot, state;
        uint32_t caller;
    };
    std::vector<Cue> cues_;
};

} // namespace x2::audio
