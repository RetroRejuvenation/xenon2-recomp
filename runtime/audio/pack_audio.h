// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Sound-effect and speech replacement by asset packs (docs/re-notes/asset-packs-v2-design.md section 5.1-5.2,
// docs/re-notes/asset-packs-sfx.md): the output-mix stage.
//
// PackAudio listens to Paula's events (audio/paula.h SoundEvent), decides at each DMA start whether a pack replaces the sample the
// channel is about to play, and renders the replacement as a **pack voice** into the Paula output frames the machine produced, in
// the stage that renders the CD player (CdtvMusic::after_frame): call after_frame() once per emulated frame, after it and before
// any consumer of Paula's output. Nothing here changes machine state: the channel keeps playing its sample (DMA, interrupts and
// statistics are untouched) and only its share of the mix is gated (Paula::set_channel_mix) while the voice stands in for it.
//
// Time and determinism. Every event carries the Paula output frame it happened at; voices start, change pitch and volume, and end
// at those frames (shifted by kLatencyFrames so they line up with the original, which Paula's output filter delays by the same
// amount). Voice positions are integers (Q32.32 frames of the clip prepared at the output rate), pitch is the integer ratio
// ref_period / period, gains are integers (Q16) made by deterministic arithmetic (no libm), so the output is a pure function of the
// emulated run: the same on every platform and in Debug and Release builds. With no pack (or the Faithful switch) the stage
// never touches the mix, and without a dump it does nothing at all.
#pragma once
#include "audio/paula.h"
#include "pack/pack_api.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2::modern {
struct AudioLayout;
}

namespace x2::audio {

class AudioDumper;

// The loader's sound-file hook (pack/pack_api.h AudioCodec) over the decoders (audio/decode.h): register it before the packs are loaded
// (`pack::set_audio_codec(&pack_codec())`), or audio entries are not registered and a check cannot validate them.
const pack::AudioCodec& pack_codec();

// A sample as the channel is about to play it, for the dump and for diagnostics.
struct SampleStart {
    const uint8_t* bytes = nullptr;  // the bytes the channel reads, wrapped inside chip RAM
    uint32_t size = 0;
    std::string key;                 // sample key (pack_api.h)
    bool speech = false;             // CDTV: the buffer lies in the speech region
    std::string cd0_file;            // CDTV speech: the disc file that was loaded there ("SPEECH63"), else ""
    int channel = 0;
    uint16_t period = 0, volume = 0;
    uint64_t frame = 0;              // the machine's frame number (for first_frame in a dump)
};

struct PackAudioStats {
    uint64_t starts = 0;       // DMA starts the stage looked at
    uint64_t candidates = 0;   // of them effect or speech starts (the ones a pack can replace)
    uint64_t replaced = 0;     // voices started
    uint64_t stolen = 0;       // voices ended early by the polyphony limit
    uint64_t hashed = 0;       // samples hashed (a replacement of that byte length exists, or a dump is running)
};

class PackAudio {
public:
    static constexpr int kMaxVoices = 16;
    // The FIR in Paula's output stage delays the original by 63 samples at 4x the output rate: 16 output frames.
    static constexpr int kLatencyFrames = 16;
    static constexpr int kPeriodDeferMs = 3;  // a write of the minimum period reaches a voice this much later (see on_event)

    PackAudio();
    ~PackAudio();
    PackAudio(const PackAudio&) = delete;
    PackAudio& operator=(const PackAudio&) = delete;

    // Installs the event sink on `paula`. `chip` and `chip_mask` are the machine's chip RAM (the samples are read from it, never
    // written). The packs come from pack::packs() and the switches from pack::pack_settings(), looked up as they are needed.
    void attach(Paula& paula, const uint8_t* chip, uint32_t chip_mask, pack::GameFamily game, const modern::AudioLayout& layout);
    // `--dump-audio`: every effect and speech start is also passed to the dumper (nullptr: stop).
    void set_dumper(AudioDumper* d);
    // The machine's frame number, for the dump (optional).
    void set_frame_source(std::function<uint64_t()> f) { frame_source_ = std::move(f); }
    // The effect volume in percent (--sfx-volume), applied to pack voices as they play. The speech volume is read from
    // pack::pack_settings().speech_volume at every frame.
    void set_sfx_volume(int percent);
    // CDTV: the file service loaded `size` bytes of disc file `name` ("CD0:SPEECH63" or "SPEECH63") at `dest`.
    void on_file_loaded(uint32_t dest, const std::string& name, uint32_t size);

    // After each emulated frame, before the consumers of paula.output() (and after the CD layer has rendered into it).
    void after_frame(Paula& paula);
    // A host reset: every voice ends at once and the gates open.
    void reset(Paula& paula);

    // False for a disk build whose sound driver is not mapped (modern/layout.h): its effects are neither replaced nor dumped.
    bool effects_mapped() const { return effects_mapped_; }
    const PackAudioStats& stats() const { return stats_; }
    int active_voices() const;

private:
    struct Prepared;
    struct Voice;
    struct Queued;

    void on_event(const SoundEvent& e);
    void push(Queued&& q);
    void on_start(const SoundEvent& e);
    bool classify(const SoundEvent& e, bool& speech) const;
    std::string cd0_name_for(uint32_t addr) const;
    void queue_start(const SoundEvent& e, const pack::ClipEntry* clip, bool speech);
    void apply(const Queued& q);
    void render(int16_t* out, uint64_t first, uint64_t from, uint64_t to);
    std::shared_ptr<const Prepared> prepare(const pack::ClipEntry& e);
    void release_all_gates();
    void fade_voice(Voice& v);
    bool audio_on();

    Paula* paula_ = nullptr;
    const uint8_t* chip_ = nullptr;
    uint32_t chip_mask_ = 0;
    pack::GameFamily game_ = pack::GameFamily::Disk;
    uint32_t speech_lo_ = 0, speech_hi_ = 0;
    bool effects_mapped_ = true;     // a disk build whose effect code is not mapped has no effect starts to find
    AudioDumper* dumper_ = nullptr;
    std::function<uint64_t()> frame_source_;
    int rate_ = 48000;
    double side_same_ = 1.0, side_cross_ = 0.0;
    double k_ = 0.375;
    int32_t cat_q16_[2] = {65536, 65536};
    int ramp_step_ = 682;
    std::array<uint8_t, 4> side_{0, 1, 1, 0};
    std::array<uint64_t, 4> owner_{};   // serial of the voice that follows each channel (0: none)
    std::array<uint32_t, 4> cur_lc_{};   // the buffer each channel was started with, for the dump (len 0: not a recorded sample)
    std::array<uint32_t, 4> cur_len_{};
    std::array<bool, 4> owned_{};   // a pack voice follows this channel (queued, in event order)

    struct LoadedFile {
        uint32_t dest = 0, size = 0;
        std::string name;
    };
    std::vector<LoadedFile> loaded_;

    std::vector<Voice> voices_;
    std::vector<Queued> queue_;
    std::unordered_map<const void*, std::shared_ptr<const Prepared>> prepared_;
    uint64_t mixed_ = 0;
    uint64_t serial_ = 0;
    bool was_on_ = true;
    PackAudioStats stats_;
    std::vector<int32_t> acc_;
    std::vector<uint8_t> scratch_;
};

} // namespace x2::audio
