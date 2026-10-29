// Draw-command capture and interpolation tests on synthetic chip RAM (no game data). The CPU does not run: the
// test plays the game's part, programming the blitter and writing the buffer pointers the way the game's
// drawing routines do (docs/re-notes/modernisation-design.md), with REG_PPC set to the game's instruction
// addresses so the capture classifies the blits as it would in the game.
#include "custom_regs.h"
#include "machine.h"
#include "modern/draw_capture.h"
#include "modern/interp.h"
#include "modern/present.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" {
#include "m68k.h"
}

using namespace x2;
using namespace x2::modern;

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

constexpr uint32_t kBufA = 0x44000, kBufB = 0x4C700;
constexpr uint32_t kRingSrc = 0x60000;   // far layer: 2 planes, 80 bytes per line
constexpr uint32_t kBob = 0x62000;    // 16x8 bob: mask words, then 4 planes of 8 words
constexpr uint32_t kObj = 0x3F000, kSentinel = 0x3F100, kStack = 0x3000;

void set_ptr(Machine& m, uint16_t reg, uint32_t v) {
    m.custom_write(reg, uint16_t(v >> 16));
    m.custom_write(uint16_t(reg + 2), uint16_t(v));
}

struct BlitSpec {
    uint32_t pc;
    uint16_t con0, con1 = 0, fwm = 0xFFFF, lwm = 0xFFFF;
    uint32_t apt = 0, bpt = 0, cpt = 0, dpt = 0;
    int16_t amod = 0, bmod = 0, cmod = 0, dmod = 0;
    uint16_t adat = 0;
    int w, h;
};

void blit(Machine& m, const BlitSpec& b) {
    m68k_set_reg(M68K_REG_PPC, b.pc);
    m.custom_write(reg::BLTCON0, b.con0);
    m.custom_write(reg::BLTCON1, b.con1);
    m.custom_write(reg::BLTAFWM, b.fwm);
    m.custom_write(reg::BLTALWM, b.lwm);
    set_ptr(m, reg::BLTAPTH, b.apt);
    set_ptr(m, reg::BLTBPTH, b.bpt);
    set_ptr(m, reg::BLTCPTH, b.cpt);
    set_ptr(m, reg::BLTDPTH, b.dpt);
    m.custom_write(reg::BLTAMOD, uint16_t(b.amod));
    m.custom_write(reg::BLTBMOD, uint16_t(b.bmod));
    m.custom_write(reg::BLTCMOD, uint16_t(b.cmod));
    m.custom_write(reg::BLTDMOD, uint16_t(b.dmod));
    m.custom_write(reg::BLTADAT, b.adat);
    m.custom_write(reg::BLTSIZE, uint16_t((b.h << 6) | b.w));
}

struct Rig {
    Machine m;
    DrawCapture cap;
    Rig() {
        m.set_timing(Timing::Fast);
        m.custom_write(reg::DMACON, 0x8240); // DMAEN | BLTEN
        for (int i = 0; i < 16; ++i) m.custom_write(uint16_t(reg::COLOR00 + 2 * i), uint16_t(i * 0x111)); // grey ramp
        // Far layer source: a 16-line stripe pattern in plane 0 (colour 1 on even 8-line bands).
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 40; ++x) m.chip[kRingSrc + uint32_t(y) * 80 + uint32_t(x)] = (y / 8) % 2 ? 0x00 : 0xFF;
        // Bob: full mask, all four planes set (colour 15).
        for (int i = 0; i < 8; ++i) m.write16(kBob + 2 * uint32_t(i), 0xFFFF);
        for (int p = 0; p < 4; ++p)
            for (int i = 0; i < 8; ++i) m.write16(kBob + 16 + uint32_t(p) * 16 + 2 * uint32_t(i), 0xFFFF);
        // Object list $B4C -> kObj -> sentinel; the stack as run_object_list_draw leaves it: {$3678, next}.
        m.write32(0xB4C, kObj);
        m.write16(kObj, 1);
        m.write32(kObj + 2, 0x5850);
        m.write32(kObj + 0x12, kSentinel);
        m.write16(kSentinel, 0);
        for (uint32_t h : {0xA88u, 0xAEAu, 0xBAEu, 0xC10u}) m.write32(h, kSentinel);
        m.write32(kStack, 0x3678);
        m.write32(kStack + 4, kSentinel);
        m.write32(DrawCapture::kDrawPtr, kBufA);
        m.write32(DrawCapture::kShowPtr, kBufB);
        cap.capture_data = true;
        cap.attach(m);
    }
    // One logic step as the game draws it: far layer copy + clear of planes 2-3, the object bob, then the swap.
    void step(int ring_row, int obj_x, int obj_y) {
        uint32_t buf = (uint32_t(m.chip[0x406]) << 24 | m.chip[0x407] << 16 | m.chip[0x408] << 8 | m.chip[0x409]);
        m.write16(0x436, uint16_t(ring_row));
        m.write16(kObj + 0x20, uint16_t(obj_x));
        m.write16(kObj + 0x24, uint16_t(obj_y));
        m.frame += 3; // one logic step per 3 VBLs
        m.set_areg(7, kStack + 0x100); // background pass: no object draw on the stack
        int first = 192 - ring_row;
        blit(m, {0x1832, 0x09F0, 0, 0xFFFF, 0xFFFF, kRingSrc + uint32_t(ring_row) * 80, 0, 0, buf, 0, 0, 0, 80, 0, 40, first});
        if (ring_row)
            blit(m, {0x185A, 0x09F0, 0, 0xFFFF, 0xFFFF, kRingSrc, 0, 0, buf + uint32_t(first) * 160, 0, 0, 0, 80, 0, 40, ring_row});
        blit(m, {0x1884, 0x01F0, 0, 0xFFFF, 0xFFFF, 0, 0, 0, buf + 80, 0, 0, 0, 80, 0, 40, 192});
        m.set_areg(7, kStack); // inside the object's draw handler
        for (int p = 0; p < 4; ++p) {
            uint32_t d = buf + uint32_t(obj_y) * 160 + uint32_t(p) * 40 + uint32_t(obj_x / 8);
            blit(m, {0x12CC, 0x0FCA, 0, 0xFFFF, 0xFFFF, kBob, kBob + 16 + uint32_t(p) * 16, d, d, 0, 0, 158, 158, 0, 1, 8});
        }
        // Panel line 195: one CPU word write (zero-moved by the interpolator).
        m.write16(buf + 195 * 160, uint16_t(0xF0F0 ^ obj_x));
        // The swap ($202E): $406 <-> $40A.
        uint32_t shown = buf == kBufA ? kBufB : kBufA;
        m68k_set_reg(M68K_REG_PPC, 0x2062);
        m.write32(DrawCapture::kDrawPtr, shown);
        m68k_set_reg(M68K_REG_PPC, 0x2068);
        m.write32(DrawCapture::kShowPtr, buf);
    }
};

uint32_t pixel(const std::vector<uint8_t>& rgba, int w, int x, int y) {
    const uint8_t* p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
}

void test_capture() {
    std::printf("capture: blit decoding, object identity, A/B/C data, CPU writes, step boundary\n");
    Rig r;
    r.step(10, 64, 50);
    CHECK(r.cap.steps().size() == 1);
    if (r.cap.steps().empty()) return;
    const Step& s = r.cap.steps()[0];
    CHECK(s.draw_buf == kBufA && s.other_buf == kBufB);
    CHECK(s.blits.size() == 2 + 1 + 4);
    const BlitCmd& far = s.blits[0];
    CHECK(far.pc == 0x1832 && far.buffer == 0 && far.y == 0 && far.plane == 0 && far.w == 40 && far.h == 182);
    CHECK(far.obj == 0);
    CHECK(s.blits[1].y == 182);
    const BlitCmd& bob = s.blits[5];
    CHECK(bob.pc == 0x12CC && bob.obj == kObj && bob.plane == 2 && bob.y == 50 && bob.x == 64);
    CHECK(bob.data != ~0u && s.words[bob.data] == 0xFFFF && s.words[bob.data + 1] == 0xFFFF);
    CHECK(s.writes.size() == 1 && s.writes[0].buffer == 0 && s.writes[0].value == (0xF0F0 ^ 64u) && s.writes[0].old == 0);
    CHECK(s.writes[0].seq > bob.seq);
    CHECK(s.objects.size() == 1 && s.objects[0].addr == kObj && s.objects[0].x == 64 && s.objects[0].y == 50);
    CHECK(s.vars[kRing] == 10);
    CHECK(s.image.size() == size_t(kGutterLines + kLines) * kLineBytes);
    // The captured picture is the buffer as drawn: line 50, x 64 is colour 15.
    std::vector<uint8_t> pic;
    Interpolator::picture(s, 1, pic);
    CHECK(Interpolator::gameplay(s));
    // Detaching stops the capture; the machine still blits.
    r.cap.detach(r.m);
    uint32_t before = r.m.blits;
    r.step(11, 64, 50);
    CHECK(r.cap.steps().size() == 1 && r.m.blits == before + 7);
}

void test_interp() {
    std::printf("interpolation: endpoints exact, object at the midpoint, scale 2\n");
    for (int scale : {1, 2}) {
        Rig r;
        r.step(10, 64, 50);
        r.step(9, 96, 42); // object moves +32, -8; far layer moves down one line
        const Step& a = r.cap.steps()[0];
        const Step& b = r.cap.steps()[1];
        Interpolator ip;
        ip.opt.scale = scale;
        ip.opt.smooth_far_layer = false;
        PairMotion mo = ip.analyse(nullptr, a, b);
        CHECK(mo.ok);
        CHECK(mo.far_step == 1 && mo.objects.count(kObj) == 1);
        if (!mo.ok) continue;
        CHECK(mo.objects[kObj][0] == 32.0f && mo.objects[kObj][1] == -8.0f);
        std::vector<uint8_t> pa, pb, out;
        Interpolator::picture(a, scale, pa);
        Interpolator::picture(b, scale, pb);
        ip.render(a, b, mo, 0.0f, out);
        CHECK(out == pa);
        ip.render(a, b, mo, 1.0f, out);
        CHECK(out == pb);
        // t = 0.5: b's list moved back by half the motion: the bob spans x 80..95, lines 46..53.
        ip.render(a, b, mo, 0.5f, out);
        const int W = kWidth * scale;
        uint32_t c15 = pixel(pa, W, 64 * scale, 50 * scale); // colour 15 in a
        CHECK(pixel(out, W, 80 * scale, 46 * scale) == c15);
        CHECK(pixel(out, W, 95 * scale + scale - 1, 53 * scale + scale - 1) == c15);
        CHECK(pixel(out, W, 79 * scale, 46 * scale) != c15);
        CHECK(pixel(out, W, 96 * scale, 46 * scale) != c15);
        CHECK(pixel(out, W, 80 * scale, 54 * scale) != c15);
        // Panel lines are not moved and come from b.
        CHECK(std::equal(&out[size_t(192 * scale) * W * 4], &out[out.size() - 1], &pb[size_t(192 * scale) * W * 4]));
        // A cut (object jump beyond max_object_jump) is not interpolated.
        ip.opt.max_object_jump = 16;
        PairMotion cut = ip.analyse(nullptr, a, b);
        CHECK(cut.ok && cut.objects.count(kObj) == 0);
    }
}

void test_not_gameplay() {
    std::printf("interpolation: non-gameplay steps fall back\n");
    Rig r;
    r.step(10, 64, 50);
    r.step(9, 80, 50);
    Step a = r.cap.steps()[0], b = r.cap.steps()[1];
    a.blits.erase(a.blits.begin()); // no far-layer copy: not a gameplay step
    Interpolator ip;
    CHECK(!ip.analyse(nullptr, a, b).ok);
    b.index += 1; // not consecutive
    CHECK(!ip.analyse(nullptr, r.cap.steps()[0], b).ok);
}
// Bob's colour-15 span on a canvas line: [first, last] x in canvas pixels, or {-1, -1}.
std::array<int, 2> span(const std::vector<uint8_t>& rgba, int w, int y, uint32_t c) {
    int first = -1, last = -1;
    for (int x = 0; x < w; ++x)
        if (pixel(rgba, w, x, y) == c) { if (first < 0) first = x; last = x; }
    return {first, last};
}

void test_linear() {
    std::printf("interpolation: linear motion at t = k/8, scales 1-4; threads give the same pixels\n");
    for (int scale : {1, 2, 3, 4}) {
        Rig r;
        r.step(10, 64, 50);
        r.step(9, 96, 42); // +32, -8 per step
        const Step& a = r.cap.steps()[0];
        const Step& b = r.cap.steps()[1];
        Interpolator ip;
        ip.opt.scale = scale;
        ip.opt.smooth_far_layer = false;
        PairMotion mo = ip.analyse(nullptr, a, b);
        CHECK(mo.ok);
        if (!mo.ok) continue;
        std::vector<uint8_t> pa, out;
        Interpolator::picture(a, scale, pa);
        const int W = kWidth * scale;
        const uint32_t c15 = pixel(pa, W, 64 * scale, 50 * scale);
        for (int k = 1; k < 8; ++k) {
            const float t = float(k) / 8;
            ip.render(a, b, mo, t, out);
            // Expected left edge and top line in canvas pixels: from a (t < 0.5) or back from b, rounded.
            const int x = t < 0.5f ? 64 * scale + int(std::lround(32 * t * scale)) : 96 * scale + int(std::lround(-32 * (1 - t) * scale));
            const int y = t < 0.5f ? 50 * scale + int(std::lround(-8 * t * scale)) : 42 * scale + int(std::lround(8 * (1 - t) * scale));
            auto sp = span(out, W, y, c15);
            CHECK(sp[0] == x && sp[1] == x + 16 * scale - 1);
            CHECK(span(out, W, y - 1, c15)[0] < 0 || y - 1 < 0);
            CHECK(span(out, W, y + 8 * scale - 1, c15)[0] == x);
            // Exactly linear in t at the scale's resolution: |x(t) - x(0) - 32 t scale| <= 1/2 canvas pixel.
            CHECK(std::fabs(float(x - 64 * scale) - 32.0f * t * float(scale)) <= 0.5f);
        }
        // Bands on worker threads: the same pixels at every t.
        Interpolator mt;
        mt.opt = ip.opt;
        mt.opt.threads = 4;
        std::vector<uint8_t> o1, o4;
        for (float t : {0.0f, 0.3f, 0.5f, 0.9f, 1.0f}) {
            ip.render(a, b, mo, t, o1);
            mt.render(a, b, mo, t, o4);
            CHECK(o1 == o4);
        }
    }
}

void test_palette_blend() {
    std::printf("interpolation: palette fades blend per channel, exact at the ends\n");
    Rig r;
    r.m.custom_write(uint16_t(reg::COLOR00 + 2 * 15), 0x0F00);
    r.step(10, 64, 50);
    r.m.custom_write(uint16_t(reg::COLOR00 + 2 * 15), 0x00F0);
    r.step(9, 80, 50);
    const Step& a = r.cap.steps()[0];
    const Step& b = r.cap.steps()[1];
    Interpolator ip;
    ip.opt.smooth_far_layer = false;
    PairMotion mo = ip.analyse(nullptr, a, b);
    CHECK(mo.ok);
    std::vector<uint8_t> out;
    ip.render(a, b, mo, 0.0f, out);
    CHECK(pixel(out, kWidth, 64, 50) == 0x0000FFu);
    ip.render(a, b, mo, 1.0f, out);
    CHECK(pixel(out, kWidth, 80, 50) == 0x00FF00u);
    ip.render(a, b, mo, 0.25f, out); // bob at x 68
    CHECK(pixel(out, kWidth, 68, 50) == (0x0000BFu | 0x004000u));
    ip.render(a, b, mo, 0.5f, out); // b's list at x 72
    CHECK(pixel(out, kWidth, 72, 50) == (0x000080u | 0x008000u));
}

void test_cuts() {
    std::printf("interpolation: scene cuts (cleared screen, objects jumping together)\n");
    Rig r;
    r.step(10, 64, 50);
    r.step(9, 80, 50);
    Interpolator ip;
    Step a = r.cap.steps()[0], b = r.cap.steps()[1];
    CHECK(ip.analyse(nullptr, a, b).ok);
    Step cleared = b;
    std::fill(cleared.image.begin(), cleared.image.end(), uint8_t(0));
    PairMotion mo = ip.analyse(nullptr, a, cleared);
    CHECK(!mo.ok && std::string(mo.why) == "screen clear");
    // Four more objects in both steps: three jump 100 px (a warp), so most of the five matched jump.
    Step ja = a, jb = b;
    for (uint32_t i = 0; i < 4; ++i) {
        ObjectSnap o;
        o.addr = 0x3E000 + 0x40 * i;
        o.update = 0x5850;
        o.x = int16_t(10 * i);
        ja.objects.push_back(o);
        o.x = int16_t(i < 3 ? 10 * i + 100 : 10 * i + 2);
        jb.objects.push_back(o);
    }
    mo = ip.analyse(nullptr, ja, jb);
    CHECK(!mo.ok && std::string(mo.why) == "objects jump");
    jb.objects[jb.objects.size() - 2].x = int16_t(20 + 2); // two of five jump: per-object cuts only
    mo = ip.analyse(nullptr, ja, jb);
    CHECK(mo.ok && mo.objects.count(0x3E000) == 0 && mo.objects.count(0x3E080) == 1 && mo.objects.count(kObj) == 1 &&
          mo.objects.count(0x3E040) == 0);
}

void test_presenter() {
    std::printf("presenter: Faithful options, faithful picture is the frame itself, cut/hold fallbacks\n");
    PresentOptions o;
    o.interp = true;
    o.filter = VideoFilter::Crt;
    o.aspect = true;
    o.integer = false;
    o.curvature = true;
    o.faithful = true;
    PresentOptions e = o.effective();
    CHECK(e.faithful && !e.interp && e.filter == VideoFilter::Nearest && !e.aspect && e.integer && !e.curvature);
    o.faithful = false;
    CHECK(o.effective().interp && o.effective().filter == VideoFilter::Crt);

    Rig r;
    Presenter p;
    p.configure(r.m, e, 313);
    CHECK(!p.interp() && !p.interpolating());
    std::vector<uint8_t> front(size_t(kFrameWidth) * kFrameHeight * 4);
    for (size_t i = 0; i < front.size(); ++i) front[i] = uint8_t(i * 7);
    Canvas c = p.at(100.0, front);
    CHECK(c.rgba == front.data() && c.w == kFrameWidth && c.h == kFrameHeight && c.scale == 1);

    // Interpolation on: before any step the faithful frame, then the pair, then a hold falls back.
    PresentOptions io;
    io.interp = true;
    io.scale = 2;
    p.configure(r.m, io, 313);
    CHECK(p.interp() != nullptr);
    c = p.at(1.0, front);
    CHECK(c.rgba == front.data());
    r.step(10, 64, 50); // swapped in during frame 3 (Rig::step advances 3 frames first)
    r.step(9, 96, 42);  // frame 6
    c = p.at(7.5, front);
    CHECK(c.scale == 2 && c.w == 2 * kFrameWidth && c.h == 2 * kFrameHeight);
    // t = 0.5: the bob halfway at x 80, line 46 of the window (frame line 46 + kFrameTop).
    if (c.scale == 2) {
        std::vector<uint8_t> v(c.rgba, c.rgba + size_t(c.w) * size_t(c.h) * 4);
        CHECK(pixel(v, c.w, 80 * 2, (46 + kFrameTop) * 2) == 0xFFFFFFu);
        CHECK(pixel(v, c.w, 0, 0) == pixel(front, kFrameWidth, 0, 0)); // above the window: the faithful frame
    }
    c = p.at(6.0 + 20.0, front); // held 20 frames (> hold_frames): the faithful frame
    CHECK(c.rgba == front.data());
    p.reset();
    c = p.at(30.0, front);
    CHECK(c.rgba == front.data());
}

void test_edge_scalers() {
    std::printf("edge upscalers: Scale2x / Scale3x\n");
    const uint32_t X = 0xFFFFFFFFu, o = 0xFF000000u;
    // A one-pixel diagonal: the corners next to it are filled in, flat areas and the line's own pixels stay.
    const uint32_t diag[9] = {X, o, o, o, X, o, o, o, X};
    uint32_t d2[36], d3[81];
    scale2x(diag, 3, 3, d2);
    CHECK(d2[2 * 6 + 2] == X && d2[3 * 6 + 3] == X);        // the middle pixel's block stays X
    CHECK(d2[1 * 6 + 2] == X && d2[0 * 6 + 3] == o);        // (1,0) lower-left corner joins the line
    CHECK(d2[2 * 6 + 1] == X && d2[3 * 6 + 0] == o);        // (0,1) upper-right corner joins the line
    scale3x(diag, 3, 3, d3);
    CHECK(d3[4 * 9 + 4] == X && d3[2 * 9 + 3] == X && d3[0 * 9 + 5] == o);
    const uint32_t flat[4] = {o, o, o, o};
    scale3x(flat, 2, 2, d3);
    CHECK(std::all_of(d3, d3 + 36, [&](uint32_t v) { return v == o; }));
    const uint32_t dot[9] = {o, o, o, o, X, o, o, o, o}; // an isolated pixel stays a square
    scale2x(dot, 3, 3, d2);
    CHECK(d2[2 * 6 + 2] == X && d2[3 * 6 + 3] == X && d2[2 * 6 + 3] == X && d2[1 * 6 + 2] == o);
}

// The window loop on a simulated host: the audio device pulls 512-sample chunks at 48 kHz and a frame is run
// (2 ms) after a refresh when its queue is under 40 ms; the display refreshes at hz with a little jitter in
// when the vsync wait returns. Every refresh presents tau(display time); logic steps every 3 frames.
void pacing(double hz) {
    const double frame_ns = 313.0 * 227 * 1e9 / 3546895.0, period = 1e9 / hz;
    const double samples_per_frame = 48000.0 * frame_ns / 1e9, chunk = 512, target = 48000 * 0.040;
    PresentClock clock(frame_ns);
    clock.lag = PresentClock::kMargin + period / frame_ns; // as the window loop sets it
    VsyncTimeline grid(period);
    double queue = 2 * target, now = 0, consumed_at = 0;
    uint32_t frames = 0, lcg = 12345;
    std::vector<double> taus;
    auto jitter = [&](double amp) { lcg = lcg * 1664525u + 1013904223u; return (double(lcg >> 8) / 16777216.0 - 0.5) * 2 * amp; };
    for (int k = 0; k < int(hz * 20); ++k) {
        const uint64_t at = grid.next(uint64_t(now));
        taus.push_back(clock.tau(at));
        now = std::max(now, double(at)) + jitter(150'000) + 150'000; // the vsync wait returns just after it
        grid.presented(uint64_t(now));
        while (consumed_at + chunk / 48000.0 * 1e9 <= now) { consumed_at += chunk / 48000.0 * 1e9; queue -= chunk; }
        if (queue < target) {
            now += 2'000'000;
            ++frames;
            queue += samples_per_frame;
            clock.frame_done(uint64_t(now), double(frames));
        }
    }
    // After a 5 s warm-up (the clock adapts its lag to the audio chunks): every refresh advances emulated time by the refresh period, give or take the clock's
    // corrections (no repeated and no skipped pictures), and each one shows a different step pair or t.
    const double expect = period / frame_ns;
    double lo = 1e9, hi = 0;
    int same = 0;
    auto shown = [](double tau) { // (step pair, t) with steps swapped in at 3j + 258/313
        double sw = 258.0 / 313.0;
        double j = std::floor((tau - sw) / 3);
        return std::array<double, 2>{j, (tau - sw - 3 * j) / 3};
    };
    for (size_t i = size_t(hz * 5); i < taus.size(); ++i) {
        const double d = taus[i] - taus[i - 1];
        lo = std::min(lo, d / expect);
        hi = std::max(hi, d / expect);
        same += shown(taus[i]) == shown(taus[i - 1]);
    }
    std::printf("  %5.1f Hz: %u frames run, tau step %.3f..%.3f x the refresh period, %d repeated pictures, "
                "%u clock snaps, %u grid resyncs\n", hz, frames, lo, hi, same, clock.snaps(), grid.resyncs());
    CHECK(lo > 0.85 && hi < 1.15);
    CHECK(same == 0);
    CHECK(clock.snaps() == 1 && grid.resyncs() <= 1);
    CHECK(std::fabs(double(frames) - 20 * 1e9 / frame_ns) < 5); // the audio clock sets the emulation speed
}

void test_pacing() {
    std::printf("presentation pacing: emulated time per refresh at 50-144 Hz\n");
    for (double hz : {50.0, 60.0, 75.0, 90.0, 120.0, 144.0}) pacing(hz);
    // Monotonic and never past the newest frame.
    PresentClock c(20e6);
    c.frame_done(0, 10);
    CHECK(c.tau(0) == 9.25);
    CHECK(c.tau(5'000'000) == 9.5);
    CHECK(c.tau(4'000'000) == 9.5);   // not backwards
    CHECK(c.extra_lag() == 0);
    CHECK(c.tau(100'000'000) == 10.0); // held at the newest frame, and the clock now stays further behind
    CHECK(c.extra_lag() > 0);
    c.reset();
    c.frame_done(200'000'000, 50);    // after a reset: start again at the new position
    CHECK(c.tau(200'000'000) < 49.25 && c.tau(200'000'000) > 49.0 && c.snaps() == 2);
}

} // namespace

int main() {
    test_capture();
    test_interp();
    test_not_gameplay();
    test_linear();
    test_palette_blend();
    test_cuts();
    test_presenter();
    test_edge_scalers();
    test_pacing();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
