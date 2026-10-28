#include "paula.h"

#include <algorithm>
#include <cmath>

namespace x2 {

namespace {
constexpr int kOversample = 4;
constexpr int kFirTaps = 127;
constexpr double kFirCutoffHz = 20000.0;
constexpr double kKaiserBeta = 8.0;
constexpr double kPi = 3.14159265358979323846;

// Analogue filter corner frequencies.
constexpr double kA500FixedHz = 4900.0;  // A500 RC low-pass (6 dB/oct)
constexpr double kA1200FixedHz = 32000.0; // A1200 low-pass, approximate
constexpr double kLedHz = 3275.0;         // LED filter: 2-pole Butterworth (12 dB/oct)
constexpr double kDcHz = 5.0;             // output coupling capacitor high-pass

// Modified Bessel function of the first kind, order 0 (power series; libc++ has no std::cyl_bessel_i).
double bessel_i0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 50; ++k) {
        term *= q / (double(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}
} // namespace

Paula::Paula() { configure(PaulaConfig{}); }

void Paula::attach(const uint8_t* chip, uint32_t chip_mask, std::function<void(uint16_t)> raise_irq) {
    chip_ = chip;
    chip_mask_ = chip_mask;
    raise_irq_ = std::move(raise_irq);
}

void Paula::configure(const PaulaConfig& cfg) {
    cfg_ = cfg;
    cfg_.output_rate = std::clamp(cfg_.output_rate, 8000, 192000);
    cfg_.stereo_separation = std::clamp(cfg_.stereo_separation, 0.0f, 1.0f);
    cfg_.output_gain = std::clamp(cfg_.output_gain, 0.0f, 4.0f);
    internal_rate_ = uint32_t(cfg_.output_rate) * kOversample;
    unit_left_ = kPalColorClock;
    acc_l_ = acc_r_ = 0;
    const double fs = internal_rate_;

    use_rc_ = cfg_.filter != AudioFilterModel::None;
    use_hp_ = cfg_.filter != AudioFilterModel::None;
    double fixed_hz = cfg_.filter == AudioFilterModel::A500 ? kA500FixedHz : kA1200FixedHz;
    rc_a_ = 1.0 - std::exp(-2.0 * kPi * fixed_hz / fs);
    rc_l_ = rc_r_ = 0;

    // LED filter: RBJ biquad low-pass, Q = 1/sqrt(2) (Butterworth), bilinear transform.
    double w0 = 2.0 * kPi * kLedHz / fs, alpha = std::sin(w0) / (2.0 * std::sqrt(0.5)), c = std::cos(w0);
    double a0 = 1.0 + alpha;
    bq_b0_ = (1.0 - c) / 2.0 / a0;
    bq_b1_ = (1.0 - c) / a0;
    bq_b2_ = bq_b0_;
    bq_a1_ = -2.0 * c / a0;
    bq_a2_ = (1.0 - alpha) / a0;
    bq_l_.fill(0);
    bq_r_.fill(0);

    double rc = 1.0 / (2.0 * kPi * kDcHz);
    hp_a_ = rc / (rc + 1.0 / fs);
    hp_xl_ = hp_yl_ = hp_xr_ = hp_yr_ = 0;

    // Decimation FIR: Kaiser-windowed sinc, unity DC gain.
    fir_.assign(kFirTaps, 0.0);
    double fc = kFirCutoffHz / fs, sum = 0, mid = (kFirTaps - 1) / 2.0;
    for (int i = 0; i < kFirTaps; ++i) {
        double n = i - mid;
        double sinc = n == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * n) / (kPi * n);
        double r = n / mid;
        double w = bessel_i0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel_i0(kKaiserBeta);
        fir_[size_t(i)] = sinc * w;
        sum += fir_[size_t(i)];
    }
    for (double& t : fir_) t /= sum;
    hist_l_.assign(kFirTaps, 0.0);
    hist_r_.assign(kFirTaps, 0.0);
    hist_pos_ = 0;
    phase_ = 0;
}

void Paula::irq(int ch) {
    ++stats_[size_t(ch)].irqs;
    if (raise_irq_) raise_irq_(uint16_t(0x0080 << ch));
}

uint32_t Paula::period(int ch) {
    uint32_t p = ch_[size_t(ch)].per;
    if (p == 0) p = 65536;
    ChannelStats& s = stats_[size_t(ch)];
    if (p < kMinDmaPeriod) {
        ++s.period_clamps;
        p = kMinDmaPeriod;
    }
    s.min_period = std::min(s.min_period, p);
    s.max_period = std::max(s.max_period, p);
    return p;
}

void Paula::fetch(int ch) {
    Channel& c = ch_[size_t(ch)];
    uint32_t a = c.ptr & chip_mask_ & ~1u;
    c.dat = chip_ ? uint16_t((chip_[a] << 8) | chip_[(a + 1) & chip_mask_]) : 0;
    c.ptr += 2;
    if (--c.count == 0) {
        c.ptr = c.lc;
        c.count = c.len ? c.len : 65536;
        ++stats_[size_t(ch)].buffer_reloads;
        irq(ch);
    }
}

void Paula::start_channel(int ch) {
    Channel& c = ch_[size_t(ch)];
    ChannelStats& s = stats_[size_t(ch)];
    ++s.dma_starts;
    std::pair<uint32_t, uint32_t> key{c.lc, c.len};
    if (s.starts.size() < 4096 && std::find(s.starts.begin(), s.starts.end(), key) == s.starts.end())
        s.starts.push_back(key);
    c.active = true;
    c.stopping = false;
    c.ptr = c.lc;
    c.count = c.len ? c.len : 65536;
    irq(ch);
    fetch(ch);
    c.word = c.dat;
    fetch(ch);
    c.byte = 0;
    c.sample = int8_t(c.word >> 8);
    c.per_left = period(ch);
}

void Paula::next_sample(int ch) {
    Channel& c = ch_[size_t(ch)];
    if (c.stopping) {
        c.active = c.stopping = false;
        c.sample = 0;
        return;
    }
    if (c.byte == 0) {
        c.byte = 1;
        c.sample = int8_t(c.word & 0xFF);
    } else {
        c.word = c.dat;
        fetch(ch);
        c.byte = 0;
        c.sample = int8_t(c.word >> 8);
    }
    c.per_left = period(ch);
}

void Paula::write_reg(uint16_t off, uint16_t v) {
    if (off < 0xA0 || off >= 0xE0) return;
    Channel& c = ch_[size_t((off - 0xA0) >> 4)];
    switch (off & 0xE) {
    case 0x0: c.lc = (c.lc & 0xFFFF) | (uint32_t(v & 0x1F) << 16); break; // AUDxLCH
    case 0x2: c.lc = (c.lc & 0xFFFF0000u) | (v & 0xFFFE); break;        // AUDxLCL
    case 0x4: c.len = v; break;                                           // AUDxLEN
    case 0x6: c.per = v; break;                                           // AUDxPER
    case 0x8: c.vol = (v & 0x40) ? 64 : (v & 0x3F); break;                // AUDxVOL
    default: break;                                                       // AUDxDAT: manual mode, unused
    }
}

void Paula::set_dmacon(uint16_t dmacon) {
    uint16_t en = (dmacon & 0x0200) ? (dmacon & 0x000F) : 0;
    for (int i = 0; i < 4; ++i) {
        Channel& c = ch_[size_t(i)];
        bool on = en & (1 << i);
        if (on && (!c.active || c.stopping)) {
            if (c.active) c.stopping = false; // re-enabled before the current sample ended: keep playing
            else start_channel(i);
        } else if (!on && c.active) {
            c.stopping = true;
        }
    }
    dma_ = en;
}

void Paula::set_adkcon(uint16_t adkcon) {
    if (adkcon & 0x00FF) ++adk_mod_writes_; // attach modulation requested; not modelled
}

void Paula::set_led_filter(bool on) { led_ = on; }

void Paula::run_until(uint64_t t) {
    while (now_ < t) {
        uint64_t step = t - now_;
        for (const Channel& c : ch_)
            if (c.active) step = std::min<uint64_t>(step, c.per_left);
        emit(step);
        now_ += step;
        for (int i = 0; i < 4; ++i) {
            Channel& c = ch_[size_t(i)];
            if (!c.active) continue;
            if (c.vol) stats_[size_t(i)].active_cck += step;
            c.per_left -= uint32_t(step);
            if (c.per_left == 0) next_sample(i);
        }
    }
}

void Paula::emit(uint64_t cck) {
    if (!output_enabled_ || cck == 0) return;
    int lvl[4];
    for (int i = 0; i < 4; ++i) lvl[i] = ch_[size_t(i)].sample * int(ch_[size_t(i)].vol);
    // Integer accumulation keeps the result independent of how emulated time is split into run_until() calls.
    int64_t left = lvl[0] + lvl[3], right = lvl[1] + lvl[2];
    uint64_t units = cck * internal_rate_;
    while (units) {
        uint64_t take = std::min(units, unit_left_);
        acc_l_ += left * int64_t(take);
        acc_r_ += right * int64_t(take);
        units -= take;
        unit_left_ -= take;
        if (unit_left_ == 0) {
            constexpr double kScale = 1.0 / (16384.0 * kPalColorClock); // 2 channels x 128 x 64 = full scale
            double l = double(acc_l_) * kScale, r = double(acc_r_) * kScale;
            double sep = cfg_.stereo_separation, same = (1.0 + sep) * 0.5, cross = (1.0 - sep) * 0.5;
            push_internal(l * same + r * cross, r * same + l * cross);
            acc_l_ = acc_r_ = 0;
            unit_left_ = kPalColorClock;
        }
    }
}

void Paula::push_internal(double l, double r) {
    if (use_rc_) {
        rc_l_ += rc_a_ * (l - rc_l_);
        rc_r_ += rc_a_ * (r - rc_r_);
        l = rc_l_;
        r = rc_r_;
    }
    if (led_ && cfg_.filter != AudioFilterModel::None) {
        auto biquad = [&](std::array<double, 4>& s, double x) {
            double y = bq_b0_ * x + bq_b1_ * s[0] + bq_b2_ * s[1] - bq_a1_ * s[2] - bq_a2_ * s[3];
            s = {x, s[0], y, s[2]};
            return y;
        };
        l = biquad(bq_l_, l);
        r = biquad(bq_r_, r);
    }
    if (use_hp_) {
        double yl = hp_a_ * (hp_yl_ + l - hp_xl_), yr = hp_a_ * (hp_yr_ + r - hp_xr_);
        hp_xl_ = l;
        hp_yl_ = yl;
        hp_xr_ = r;
        hp_yr_ = yr;
        l = yl;
        r = yr;
    }
    hist_l_[hist_pos_] = l;
    hist_r_[hist_pos_] = r;
    hist_pos_ = (hist_pos_ + 1) % hist_l_.size();
    if (++phase_ < kOversample) return;
    phase_ = 0;
    // Convolve: hist_pos_ is the oldest sample.
    double sl = 0, sr = 0, gain = 32767.0 * double(cfg_.output_gain);
    size_t n = fir_.size(), p = hist_pos_;
    for (size_t i = 0; i < n; ++i) {
        sl += fir_[i] * hist_l_[p];
        sr += fir_[i] * hist_r_[p];
        if (++p == n) p = 0;
    }
    auto to_i16 = [&](double v) {
        peak_ = std::max(peak_, std::fabs(v * cfg_.output_gain));
        double s = std::round(v * gain);
        if (s > 32767.0 || s < -32768.0) {
            ++clipped_;
            s = std::clamp(s, -32768.0, 32767.0);
        }
        return int16_t(s);
    };
    out_.push_back(to_i16(sl));
    out_.push_back(to_i16(sr));
    ++frames_out_;
}

} // namespace x2
