// Paula audio: four DMA sound channels, the A500/A1200 output filters and resampling to the host rate.
//
// Time base: Paula is clocked in PAL colour clocks (3,546,895 Hz). The machine advances it with
// run_until(t), t being an absolute colour-clock count, at the end of every scanline and immediately before
// any write that changes audio state (AUDx registers, DMACON, ADKCON, CIA-A PRA). So a register write in the
// middle of a line takes effect at the colour clock it happened at, and the output is a pure function of
// emulated time (never of wall-clock time).
//
// Channel model (DMA mode, HRM chapter 5):
//   - DMA start (DMACON master + AUDxEN rising edge): pointer/length are latched from AUDxLC/AUDxLEN, the
//     audio interrupt is raised (INTREQ AUD0-3), the first word is fetched.
//   - Each byte (signed 8-bit sample, high byte first) is held for AUDxPER colour clocks. The period is
//     re-read from AUDxPER when the previous one expires; DMA cannot fetch faster than one word per 248
//     colour clocks, so periods below 124 are clamped to 124. Period 0 means 65536.
//   - Paula double-buffers: the next word is fetched as soon as the current one moves to the output. When the
//     last word of the buffer is fetched the length counter reloads from AUDxLC/AUDxLEN (the values in the
//     registers at that moment, which is how one-shot-then-loop samples work) and the interrupt is raised
//     again. AUDxLEN 0 means 65536 words.
//   - DMA stop: the channel finishes the sample being output, then goes idle and outputs 0.
//   - AUDxVOL: bit 6 set means 64, else bits 0-5. Takes effect immediately.
//   - Not modelled (the game never uses them, see docs/re-notes/audio-notes.md): ADKCON attach modulation
//     (ATVOL/ATPER; the game only ever clears ADKCON bits 0-7), manual AUDxDAT playback with DMA off, DMA
//     slot start-up latency (a few colour clocks).
//
// Output: channels 0 and 3 go left, 1 and 2 right; `stereo_separation` 1.0 = hard left/right, 0.0 = mono.
// Resampling: the piecewise-constant Paula output is integrated exactly (area average) into an intermediate
// stream at 4x the output rate, the analogue filter model runs at that rate, and a 127-tap Kaiser-windowed
// sinc FIR (cut-off 20 kHz, ~80 dB stop band) decimates by 4 to the output rate. The area average is a
// first-order anti-alias filter that also keeps every Paula step at its exact sub-sample position; the FIR
// removes what folds back from 24-96 kHz. Fixed-point time bookkeeping makes the result bit-exact for a given
// build regardless of how run_until() calls are split.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace x2 {

enum class AudioFilterModel {
    A500,  // fixed 6 dB/oct RC low-pass ~4.9 kHz always on, plus the LED filter, plus ~5 Hz DC high-pass
    A1200, // fixed low-pass ~32 kHz (effectively flat), plus the LED filter, plus ~5 Hz DC high-pass
    None,  // raw Paula output (only the resampler's anti-alias filtering)
};

// Who wrote a channel's sample pointer (AUDxLC): the music driver or the sound-effect routine. The machine does
// not know; the CDTV music layer (runtime/cdtv/cdtv_music.h) installs a query that Paula asks at each AUDxLC
// write. Only the output mix uses it, never the channel state.
enum class AudioSource : uint8_t { Music, Effect };

struct PaulaConfig {
    int output_rate = 48000;
    float stereo_separation = 1.0f;
    AudioFilterModel filter = AudioFilterModel::A500;
    // Paula full scale on one side (two channels at +-128 x volume 64) maps to this fraction of int16 full
    // scale. The game drives the right side to full scale; the band-limiting FIR and the DC high-pass
    // overshoot by ~6% there, so 0.75 (-2.5 dB) leaves headroom and the output never clips in practice.
    float output_gain = 0.75f;
};

class Paula {
public:
    static constexpr uint32_t kPalColorClock = 3546895; // Hz
    static constexpr uint16_t kMinDmaPeriod = 124;

    struct ChannelStats {
        uint32_t dma_starts = 0;        // DMA enable edges that started the channel
        uint32_t buffer_reloads = 0;    // end-of-buffer reloads (loops)
        uint32_t irqs = 0;              // audio interrupts raised (INTREQ bit set, whether enabled or not)
        uint32_t period_clamps = 0;     // periods below 124 that were clamped
        uint64_t active_cck = 0;        // colour clocks with DMA running and volume > 0
        uint32_t min_period = 0xFFFF, max_period = 0;
        std::vector<std::pair<uint32_t, uint32_t>> starts; // distinct (LC, LEN) pairs latched at DMA start
    };

    Paula();

    // chip: chip RAM (byte array) with `chip_mask` = size - 1. raise_irq receives INTREQ bits to set.
    void attach(const uint8_t* chip, uint32_t chip_mask, std::function<void(uint16_t)> raise_irq);
    void configure(const PaulaConfig& cfg);
    const PaulaConfig& config() const { return cfg_; }
    // Output generation is off by default (headless runs without --audio-out skip the resampler). Channel
    // state, DMA and interrupts run either way.
    void set_output_enabled(bool on) { output_enabled_ = on; }
    bool output_enabled() const { return output_enabled_; }

    // Output gain per source in 1/256 (0..256) of the channels whose current buffer was latched from a pointer
    // that source wrote. With both at 256 (the default) the mix is exactly the one without gains.
    void set_writer_query(std::function<AudioSource()> q) { writer_query_ = std::move(q); }
    void set_source_gains(int music, int effect);
    AudioSource channel_source(int ch) const { return ch_[size_t(ch)].play_src; }

    // Advance to absolute colour-clock time t. Calls with t <= now() do nothing.
    void run_until(uint64_t t);
    uint64_t now() const { return now_; }

    // Register interface. The caller runs run_until(current time) first.
    void write_reg(uint16_t off, uint16_t v); // $0A0-$0DF
    void set_dmacon(uint16_t dmacon);         // full DMACON value (master bit 9, AUDxEN bits 0-3)
    void set_adkcon(uint16_t adkcon);
    void set_led_filter(bool on);             // CIA-A PRA bit 1 low = LED on = filter on
    bool led_filter() const { return led_; }

    // Interleaved stereo int16 frames produced so far; the consumer erases what it has used.
    std::vector<int16_t>& output() { return out_; }
    uint64_t frames_out() const { return frames_out_; }
    uint64_t clipped_samples() const { return clipped_; }
    double peak_level() const { return peak_; } // largest |output| before int16 conversion (1.0 = int16 full scale)

    const std::array<ChannelStats, 4>& stats() const { return stats_; }
    bool channel_active(int ch) const { return ch_[size_t(ch)].active; }
    int8_t channel_sample(int ch) const { return ch_[size_t(ch)].sample; }
    uint32_t adkcon_modulation_writes() const { return adk_mod_writes_; }
    // Audio DMA word fetches of channel `ch` so far (each takes one of the channel's DMA slots).
    uint32_t dma_fetches(int ch) const { return fetches_[size_t(ch)]; }

private:
    struct Channel {
        uint32_t lc = 0;
        uint16_t len = 0, per = 0, vol = 0;
        bool active = false, stopping = false;
        uint32_t ptr = 0, count = 0;
        uint16_t dat = 0, word = 0;
        int byte = 0;
        int8_t sample = 0;
        uint32_t per_left = 0;
        AudioSource lc_src = AudioSource::Music, play_src = AudioSource::Music; // LC writer; writer of the latched LC
    };

    void start_channel(int ch);
    void fetch(int ch);
    void next_sample(int ch);
    uint32_t period(int ch);
    void emit(uint64_t cck);
    void push_internal(double l, double r);
    void irq(int ch);
    AudioSource writer() const { return writer_query_ ? writer_query_() : AudioSource::Music; }

    PaulaConfig cfg_;
    const uint8_t* chip_ = nullptr;
    uint32_t chip_mask_ = 0;
    std::function<void(uint16_t)> raise_irq_;
    std::array<Channel, 4> ch_{};
    std::array<ChannelStats, 4> stats_{};
    std::array<uint32_t, 4> fetches_{};
    uint16_t dma_ = 0;
    bool led_ = false;
    bool output_enabled_ = false;
    uint64_t now_ = 0;
    uint32_t adk_mod_writes_ = 0;
    std::function<AudioSource()> writer_query_; // none: everything counts as music
    int gain_music_ = 256, gain_effect_ = 256;

    // Resampler: time in units of 1/(colour clock * internal rate); one colour clock = internal_rate_ units,
    // one internal sample = kPalColorClock units.
    uint32_t internal_rate_ = 0;
    uint64_t unit_left_ = 0; // units still needed to complete the current internal sample
    int64_t acc_l_ = 0, acc_r_ = 0;  // sum of (channel level x units) over the current internal sample

    // Filters (state per side).
    double rc_a_ = 0, rc_l_ = 0, rc_r_ = 0;           // fixed 1-pole low-pass
    double bq_b0_ = 0, bq_b1_ = 0, bq_b2_ = 0, bq_a1_ = 0, bq_a2_ = 0;
    std::array<double, 4> bq_l_{}, bq_r_{};           // LED biquad (x1, x2, y1, y2)
    double hp_a_ = 0, hp_xl_ = 0, hp_yl_ = 0, hp_xr_ = 0, hp_yr_ = 0;
    bool use_rc_ = true, use_hp_ = true;

    // Decimator.
    std::vector<double> fir_;
    std::vector<double> hist_l_, hist_r_; // circular, length = fir_.size()
    size_t hist_pos_ = 0;
    int phase_ = 0;

    std::vector<int16_t> out_;
    uint64_t frames_out_ = 0;
    uint64_t clipped_ = 0;
    double peak_ = 0;
};

} // namespace x2
