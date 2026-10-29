// Paula unit tests on synthetic chip RAM (no game data): DMA start/loop/reload, period timing and clamping,
// volume, DMA gating, interrupt timing, stereo mapping, the filter model and resampler invariants. Most tests
// drive x2::Paula directly; the register-path tests go through a Machine.
#include "audio/paula.h"
#include "machine.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace x2;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

constexpr uint32_t kRamSize = 0x80000;

struct Rig {
    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize, 0);
    Paula p;
    std::vector<std::pair<uint64_t, uint16_t>> irqs; // (time, INTREQ bits)
    explicit Rig(AudioFilterModel f = AudioFilterModel::None, float sep = 1.0f) {
        p.attach(ram.data(), kRamSize - 1, [this](uint16_t bits) { irqs.push_back({p.now(), bits}); });
        PaulaConfig c;
        c.filter = f;
        c.stereo_separation = sep;
        c.output_gain = 1.0f; // levels below are in units of Paula full scale
        p.configure(c);
    }
    void bytes(uint32_t addr, std::initializer_list<int> v) {
        for (int b : v) ram[addr++] = uint8_t(b);
    }
    void fill(uint32_t addr, size_t n, int b) {
        for (size_t i = 0; i < n; ++i) ram[addr + i] = uint8_t(b);
    }
    void setup(int ch, uint32_t lc, uint16_t len, uint16_t per, uint16_t vol) {
        uint16_t base = uint16_t(0xA0 + 16 * ch);
        p.write_reg(base + 0, uint16_t(lc >> 16));
        p.write_reg(base + 2, uint16_t(lc));
        p.write_reg(base + 4, len);
        p.write_reg(base + 6, per);
        p.write_reg(base + 8, vol);
    }
    // Samples output by `ch` at each multiple of `per` colour clocks from now, n of them.
    std::vector<int> sequence(int ch, uint32_t per, int n) {
        std::vector<int> out;
        for (int i = 0; i < n; ++i) {
            out.push_back(p.channel_sample(ch));
            p.run_until(p.now() + per);
        }
        return out;
    }
    // Mean of the output over the last `frames` frames, per side.
    void mean_tail(size_t frames, double& l, double& r) {
        auto& o = p.output();
        size_t n = o.size() / 2;
        l = r = 0;
        for (size_t i = n - frames; i < n; ++i) { l += o[2 * i]; r += o[2 * i + 1]; }
        l /= double(frames);
        r /= double(frames);
    }
    double rms_tail(size_t frames, int side) {
        auto& o = p.output();
        size_t n = o.size() / 2;
        double s = 0;
        for (size_t i = n - frames; i < n; ++i) s += double(o[2 * i + size_t(side)]) * o[2 * i + size_t(side)];
        return std::sqrt(s / double(frames));
    }
};

void test_sample_loop_and_reload() {
    Rig r;
    r.bytes(0x1000, {1, 2, 3, 4, 5, 6, 7, 8});
    r.bytes(0x2000, {101, 102, 103, 104});
    r.setup(0, 0x1000, 4, 124, 64);
    r.p.set_dmacon(0x0201);
    CHECK(r.p.channel_active(0));
    // One-shot then loop: point the location registers at the loop buffer right after the start interrupt.
    r.setup(0, 0x2000, 2, 124, 64);
    auto seq = r.sequence(0, 124, 16);
    std::vector<int> want = {1, 2, 3, 4, 5, 6, 7, 8, 101, 102, 103, 104, 101, 102, 103, 104};
    CHECK(seq == want);
    // Interrupts: DMA start, then the reload when the last word of each buffer is fetched.
    CHECK(r.irqs.size() >= 3);
    CHECK(r.irqs[0].first == 0 && r.irqs[0].second == 0x0080);
    CHECK(r.p.stats()[0].dma_starts == 1);
}

void test_len_zero_is_65536_words() {
    Rig r;
    r.setup(1, 0x10000, 0, 124, 64);
    r.p.set_dmacon(0x0202);
    r.irqs.clear();
    // Reload happens when the last of 65536 words is fetched: 2 words prefetched at start, then one per
    // 2 * 124 colour clocks.
    r.p.run_until(uint64_t(65536 - 2) * 248 - 1);
    CHECK(r.irqs.empty());
    r.p.run_until(uint64_t(65536 - 2) * 248);
    CHECK(r.irqs.size() == 1 && r.irqs[0].second == 0x0100);
}

void test_period_and_clamp() {
    Rig r;
    r.bytes(0x1000, {10, 20, 30, 40});
    r.setup(0, 0x1000, 2, 300, 64);
    r.p.set_dmacon(0x0201);
    r.p.run_until(299);
    CHECK(r.p.channel_sample(0) == 10);
    r.p.run_until(300);
    CHECK(r.p.channel_sample(0) == 20);
    // Period changes take effect when the current period expires.
    r.p.write_reg(0xA6, 50); // below the DMA minimum: clamped to 124
    r.p.run_until(599);
    CHECK(r.p.channel_sample(0) == 20);
    r.p.run_until(600);
    CHECK(r.p.channel_sample(0) == 30);
    r.p.run_until(600 + 123);
    CHECK(r.p.channel_sample(0) == 30);
    r.p.run_until(600 + 124);
    CHECK(r.p.channel_sample(0) == 40);
    CHECK(r.p.stats()[0].period_clamps >= 1);
    CHECK(r.p.stats()[0].min_period == 124);
    // Rate: period P gives 3546895/P samples per second.
    Rig r2;
    r2.fill(0x1000, 2, 0);
    r2.setup(2, 0x1000, 1, 428, 64);
    r2.p.set_dmacon(0x0204);
    r2.p.run_until(Paula::kPalColorClock);
    // Each word = 2 samples; one reload interrupt per word (LEN 1) after the 2-word prefetch at start.
    size_t words = Paula::kPalColorClock / (2 * 428);
    CHECK(r2.irqs.size() >= words && r2.irqs.size() <= words + 3);
}

void test_dma_gating() {
    Rig r;
    r.fill(0x1000, 64, 50);
    r.setup(3, 0x1000, 32, 200, 64);
    r.p.set_dmacon(0x0008); // AUD3EN without the master bit
    CHECK(!r.p.channel_active(3));
    CHECK(r.irqs.empty());
    r.p.set_dmacon(0x0208);
    CHECK(r.p.channel_active(3));
    CHECK(r.irqs.size() == 1 && r.irqs[0].second == 0x0400);
    r.p.run_until(450);
    r.p.set_dmacon(0x0200); // AUD3EN off: finish the current sample, then stop
    CHECK(r.p.channel_active(3));
    r.p.run_until(599);
    CHECK(r.p.channel_active(3) && r.p.channel_sample(3) == 50);
    r.p.run_until(600);
    CHECK(!r.p.channel_active(3) && r.p.channel_sample(3) == 0);
    // Master off stops every channel too.
    r.p.set_dmacon(0x0208);
    CHECK(r.p.channel_active(3));
    r.p.set_dmacon(0x0008);
    r.p.run_until(r.p.now() + 200);
    CHECK(!r.p.channel_active(3));
    CHECK(r.p.stats()[3].dma_starts == 2);
}

void test_volume() {
    auto level = [](uint16_t vol) {
        Rig r;
        r.fill(0x1000, 256, 64); // DC sample +64
        r.setup(0, 0x1000, 128, 124, vol);
        r.p.set_output_enabled(true);
        r.p.set_dmacon(0x0201);
        r.p.run_until(Paula::kPalColorClock / 50);
        double l, rr;
        r.mean_tail(200, l, rr);
        return l;
    };
    // 64 * 64 / 16384 = 0.25 full scale = 8192 (FIR has unity DC gain).
    CHECK(std::fabs(level(64) - 8192) < 2);
    CHECK(std::fabs(level(32) - 4096) < 2);
    CHECK(std::fabs(level(0x40) - level(0x7F)) < 0.01); // bit 6 set = 64
    CHECK(std::fabs(level(0x0340) - 8192) < 2);           // upper bits ignored ($0340 as written by the game)
    CHECK(std::fabs(level(63) - 8192.0 * 63 / 64) < 2);
    CHECK(std::fabs(level(0)) < 0.5);
}

void test_stereo_mapping() {
    auto run = [](int ch, float sep, double& l, double& r) {
        Rig rig(AudioFilterModel::None, sep);
        rig.fill(0x1000, 256, 64);
        rig.setup(ch, 0x1000, 128, 124, 64);
        rig.p.set_output_enabled(true);
        rig.p.set_dmacon(uint16_t(0x0200 | (1 << ch)));
        rig.p.run_until(Paula::kPalColorClock / 50);
        rig.mean_tail(200, l, r);
    };
    double l, r;
    run(0, 1.0f, l, r);
    CHECK(l > 8000 && std::fabs(r) < 1);
    run(1, 1.0f, l, r);
    CHECK(r > 8000 && std::fabs(l) < 1);
    run(2, 1.0f, l, r);
    CHECK(r > 8000 && std::fabs(l) < 1);
    run(3, 1.0f, l, r);
    CHECK(l > 8000 && std::fabs(r) < 1);
    run(0, 0.0f, l, r); // mono
    CHECK(std::fabs(l - r) < 1 && std::fabs(l - 4096) < 2);
    run(0, 0.5f, l, r);
    CHECK(std::fabs(l - 6144) < 2 && std::fabs(r - 2048) < 2);
}

// Sine table of `n` samples, amplitude 127.
void sine(Rig& r, uint32_t addr, int n) {
    for (int i = 0; i < n; ++i) r.ram[addr + uint32_t(i)] = uint8_t(int8_t(std::lround(127 * std::sin(2 * 3.14159265358979 * i / n))));
}

double tone_rms(AudioFilterModel f, bool led, int table, uint16_t per) {
    Rig r(f);
    sine(r, 0x1000, table);
    r.setup(0, 0x1000, uint16_t(table / 2), per, 64);
    r.p.set_led_filter(led);
    r.p.set_output_enabled(true);
    r.p.set_dmacon(0x0201);
    r.p.run_until(Paula::kPalColorClock / 5); // 200 ms
    return r.rms_tail(4800, 0);
}

void test_filters() {
    // 320 Hz (32-sample sine, period 346) and 7.15 kHz (4-sample sine, period 124).
    double lo_none = tone_rms(AudioFilterModel::None, false, 32, 346);
    double hi_none = tone_rms(AudioFilterModel::None, false, 4, 124);
    double lo_a500 = tone_rms(AudioFilterModel::A500, false, 32, 346);
    double hi_a500 = tone_rms(AudioFilterModel::A500, false, 4, 124);
    double hi_a500_led = tone_rms(AudioFilterModel::A500, true, 4, 124);
    double hi_a1200 = tone_rms(AudioFilterModel::A1200, false, 4, 124);
    double hi_a1200_led = tone_rms(AudioFilterModel::A1200, true, 4, 124);
    std::printf("  filter rms: 320Hz none %.0f a500 %.0f | 7.15kHz none %.0f a500 %.0f a500+led %.0f a1200 %.0f "
                "a1200+led %.0f\n", lo_none, lo_a500, hi_none, hi_a500, hi_a500_led, hi_a1200, hi_a1200_led);
    // Amplitude 127 at volume 64 is 0.496 full scale: rms 0.496 * 32767 / sqrt(2) ~ 11490.
    CHECK(lo_none > 10500 && lo_none < 12000);
    CHECK(lo_a500 / lo_none > 0.95);                 // 320 Hz passes the 4.9 kHz RC
    CHECK(hi_a500 / hi_none < 0.70 && hi_a500 / hi_none > 0.45); // one pole at 4.9 kHz: ~-4.8 dB at 7.15 kHz
    CHECK(hi_a500_led / hi_a500 < 0.3);              // LED Butterworth at 3.3 kHz: ~-13 dB at 7.15 kHz
    CHECK(hi_a1200 / hi_none > 0.9);                 // A1200 fixed filter is far above the band
    CHECK(hi_a1200_led / hi_a1200 < 0.3);
    // DC is removed by the A500 output high-pass but kept with "none".
    Rig r(AudioFilterModel::A500);
    r.fill(0x1000, 256, 64);
    r.setup(0, 0x1000, 128, 124, 64);
    r.p.set_led_filter(false);
    r.p.set_output_enabled(true);
    r.p.set_dmacon(0x0201);
    r.p.run_until(Paula::kPalColorClock); // 1 s >> 1/(2 pi 5 Hz)
    double l, rr;
    r.mean_tail(200, l, rr);
    CHECK(std::fabs(l) < 50);
}

void test_resampler_invariants() {
    // 1 s of emulated time gives exactly the output rate in frames (+-1 for the FIR phase).
    Rig a;
    sine(a, 0x1000, 32);
    a.setup(0, 0x1000, 16, 200, 64);
    a.setup(1, 0x1000, 16, 333, 40);
    a.p.set_output_enabled(true);
    a.p.set_dmacon(0x0203);
    a.p.run_until(Paula::kPalColorClock);
    CHECK(a.p.frames_out() >= 47999 && a.p.frames_out() <= 48001);
    CHECK(a.p.clipped_samples() == 0);
    // Splitting run_until() arbitrarily does not change a single output sample.
    Rig b;
    sine(b, 0x1000, 32);
    b.setup(0, 0x1000, 16, 200, 64);
    b.setup(1, 0x1000, 16, 333, 40);
    b.p.set_output_enabled(true);
    b.p.set_dmacon(0x0203);
    uint64_t t = 0, step = 1;
    while (t < Paula::kPalColorClock) {
        t = std::min<uint64_t>(t + step, Paula::kPalColorClock);
        b.p.run_until(t);
        step = step * 7 % 997 + 1;
    }
    CHECK(a.p.output() == b.p.output());
}

void test_machine_registers() {
    Machine m;
    // AUD2: 8-word buffer at $3000, period 200, enabled through the custom registers.
    for (int i = 0; i < 16; ++i) m.chip[0x3000 + size_t(i)] = uint8_t(i * 8);
    m.write32(0xDFF0C0, 0x00003000);
    m.write16(0xDFF0C4, 8);
    m.write16(0xDFF0C6, 200);
    m.write16(0xDFF0C8, 64);
    m.write16(0xDFF096, 0x8204);
    CHECK(m.paula.channel_active(2));
    CHECK(m.read16(0xDFF01E) & 0x0200); // INTREQ AUD2 at DMA start
    m.write16(0xDFF09C, 0x0200);
    CHECK(!(m.read16(0xDFF01E) & 0x0200));
    // The game never enables audio interrupts; with INTENA AUD2 off the bit is still set on reload.
    m.run_frame();
    CHECK(m.read16(0xDFF01E) & 0x0200);
    CHECK(m.paula.stats()[2].buffer_reloads > 10);
    // The machine keeps Paula exactly in step with the beam.
    CHECK(m.paula.now() == uint64_t(m.lines_per_frame()) * kColorClocksPerLine);
    // CIA-A PRA bit 1 high = power LED off = filter off (the game does bset #1,$bfe001 in music_init).
    CHECK(m.paula.led_filter());
    m.write8(0xBFE001, 0x02);
    CHECK(!m.paula.led_filter());
    m.write8(0xBFE001, 0x00);
    CHECK(m.paula.led_filter());
    // DMACON master off stops the channel within a period.
    m.write16(0xDFF096, 0x0200);
    m.run_frame();
    CHECK(!m.paula.channel_active(2));
}

} // namespace

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"sample_loop_and_reload", test_sample_loop_and_reload},
        {"len_zero_is_65536_words", test_len_zero_is_65536_words},
        {"period_and_clamp", test_period_and_clamp},
        {"dma_gating", test_dma_gating},
        {"volume", test_volume},
        {"stereo_mapping", test_stereo_mapping},
        {"filters", test_filters},
        {"resampler_invariants", test_resampler_invariants},
        {"machine_registers", test_machine_registers},
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
