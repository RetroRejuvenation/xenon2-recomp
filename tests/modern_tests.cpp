// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Draw-command capture and interpolation tests on synthetic chip RAM (no game data). The CPU does not run: the
// test plays the game's part, programming the blitter and writing the buffer pointers the way the game's
// drawing routines do (docs/re-notes/modernisation-design.md), with REG_PPC set to the game's instruction
// addresses so the capture classifies the blits as it would in the game.
#include "custom_regs.h"
#include "gfx/image_io.h"
#include "import/sha1.h"
#include "machine.h"
#include "modern/assets.h"
#include "modern/draw_capture.h"
#include "modern/interp.h"
#include "modern/present.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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
constexpr uint32_t kBob2 = 0x64000;   // the same bob with a spill word per row (2 words: data, 0) for shifted draws
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
        for (int i = 0; i < 5 * 8; ++i) m.write16(kBob2 + 4 * uint32_t(i), 0xFFFF); // mask, then planes 0-3
        // Object list $B4C -> kObj -> sentinel; the stack as run_object_list_draw leaves it: {$3678, next}.
        m.write32(0xB4C, kObj);
        m.write16(kObj, 1);
        m.write32(kObj + 2, 0x5850);
        m.write32(kObj + 0x12, kSentinel);
        m.write16(kSentinel, 0);
        for (uint32_t h : {0xA88u, 0xAEAu, 0xBAEu, 0xC10u}) m.write32(h, kSentinel);
        m.write32(kStack, 0x3678);
        m.write32(kStack + 4, kSentinel);
        m.write32(kDiskLayout.draw_ptr, kBufA);
        m.write32(kDiskLayout.show_ptr, kBufB);
        cap.capture_data = true;
        cap.attach(m);
    }
    // One logic step as the game draws it: far layer copy + clear of planes 2-3, the object bob, then the swap.
    // An x that is not a multiple of 16 draws the bob shifted, two words wide, like blit_bob_cookie.
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
            if (obj_x % 16) {
                const int sh = obj_x % 16;
                uint32_t d = buf + uint32_t(obj_y) * 160 + uint32_t(p) * 40 + uint32_t(obj_x / 16 * 2);
                blit(m, {0x12CC, uint16_t(sh << 12 | 0x0FCA), uint16_t(sh << 12), 0xFFFF, 0x0000, kBob2,
                         kBob2 + 4 * 8 * uint32_t(p + 1), d, d, 0, 0, 156, 156, 0, 2, 8});
                continue;
            }
            uint32_t d = buf + uint32_t(obj_y) * 160 + uint32_t(p) * 40 + uint32_t(obj_x / 8);
            blit(m, {0x12CC, 0x0FCA, 0, 0xFFFF, 0xFFFF, kBob, kBob + 16 + uint32_t(p) * 16, d, d, 0, 0, 158, 158, 0, 1, 8});
        }
        // Panel line 195: one CPU word write (zero-moved by the interpolator).
        m.write16(buf + 195 * 160, uint16_t(0xF0F0 ^ obj_x));
        // The swap ($202E): $406 <-> $40A.
        uint32_t shown = buf == kBufA ? kBufB : kBufA;
        m68k_set_reg(M68K_REG_PPC, 0x2062);
        m.write32(kDiskLayout.draw_ptr, shown);
        m68k_set_reg(M68K_REG_PPC, 0x2068);
        m.write32(kDiskLayout.show_ptr, buf);
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


// --- Phase 9d: graphic keys, asset packs, the dumper ---

namespace fs = std::filesystem;
std::string g_tmp = "modern_test";

void put_file(const std::string& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
}

// A w x h RGBA PNG filled by f(x, y) -> 0xAABBGGRR.
template <class F>
void put_png(const std::string& path, int w, int h, F f) {
    fs::create_directories(fs::path(path).parent_path());
    std::vector<uint8_t> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) px.push_back(uint8_t(f(x, y) >> (8 * k)));
    write_png(path, px, w, h, true);
}

// A stored (uncompressed) ZIP archive of {name, bytes}.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd;
    auto u16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    for (const auto& [name, data] : files) {
        const uint32_t crc = x2::import::crc32(reinterpret_cast<const uint8_t*>(data.data()), data.size()), off = uint32_t(out.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, uint32_t(data.size())); u32(out, uint32_t(data.size())); u16(out, uint16_t(name.size())); u16(out, 0);
        out += name + data;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
        u32(cd, crc); u32(cd, uint32_t(data.size())); u32(cd, uint32_t(data.size())); u16(cd, uint16_t(name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd += name;
    }
    const uint32_t cd_off = uint32_t(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, uint16_t(files.size())); u16(out, uint16_t(files.size()));
    u32(out, uint32_t(cd.size())); u32(out, cd_off); u16(out, 0);
    return out;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

const Graphic* sprite_of(const std::vector<Graphic>& gs) {
    for (const Graphic& g : gs)
        if (g.kind == GfxKind::Sprite) return &g;
    return nullptr;
}

void test_graphics() {
    std::printf("asset keys: merged bob, far lines, shift-independent key\n");
    Rig r;
    r.step(10, 64, 50);
    r.step(9, 69, 50); // the same bob drawn 5 pixels further right: shifted, two words wide
    std::vector<Graphic> ga, gb;
    find_graphics(r.cap.steps()[0], ga, true);
    find_graphics(r.cap.steps()[1], gb, true);
    const Graphic* a = sprite_of(ga);
    const Graphic* b = sprite_of(gb);
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->cookie && a->w == 16 && a->h == 8 && a->x == 64 && a->y == 50 && a->last - a->first == 3);
    CHECK(b->x == 69 && b->w == 16 && b->h == 8);
    CHECK(a->hash == b->hash); // the key does not depend on the blitter shift
    CHECK(a->pix.size() == 128 && a->pix[0] == (0x10 | 15));
    size_t far = 0;
    for (const Graphic& g : ga) far += g.kind == GfxKind::Far;
    CHECK(far == 192);
    CHECK(graphic_key(0x0123456789abcdefull, 16, 8) == "0123456789abcdef-16x8");
}

void test_png_paths_resample() {
    std::printf("asset packs: PNG decoding, safe paths, resampling\n");
    const std::string dir = g_tmp + "/png";
    put_png(dir + "/a.png", 3, 2, [](int x, int y) { return x == 2 && y == 1 ? 0x00000000u : 0xFF000000u | uint32_t(x * 40 + y); });
    std::ifstream in(dir + "/a.png", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    Image img;
    std::string why;
    CHECK(decode_png(bytes, img, why) && img.w == 3 && img.h == 2);
    CHECK(img.px.size() == 6 && img.px[1] == 0xFF000028u && img.px[5] == 0);
    bytes[bytes.size() - 20] ^= 1; // corrupt a byte of the image data chunk
    CHECK(!decode_png(bytes, img, why));
    CHECK(safe_pack_path("a/b.png") && safe_pack_path("x.png"));
    for (const char* bad : {"../a.png", "/etc/passwd", "a/../../b.png", "a\\b.png", "c:x.png", "", "a//b.png", "./a.png", "a/"})
        CHECK(!safe_pack_path(bad));
    Image src, out;
    src.w = 4;
    src.h = 2;
    src.px = {0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u, 0xFFFF0000u};
    resample(src, 1, 2, out); // half size: area averages
    CHECK(out.w == 2 && out.h == 1 && out.px[0] == 0xFF0000FFu && out.px[1] == 0xFFFF0000u);
    resample(src, 3, 1, out); // integer up-scale: replication
    CHECK(out.w == 12 && out.h == 6 && out.px[5] == 0xFF0000FFu && out.px[6] == 0xFFFF0000u);
    src.w = 2;
    src.h = 1;
    src.px = {0xFF0000FFu, 0x00000000u};
    resample(src, 1, 2, out); // half covered: half transparent, premultiplied
    CHECK(out.w == 1 && out.px[0] == 0x80000080u);
}

// Renders step pair (0, 1) of a rig at t with a library (or none).
std::vector<uint8_t> render_with(Rig& r, const AssetLibrary* lib, int scale, float t, int wide = 0) {
    Interpolator ip;
    ip.opt.scale = scale;
    ip.opt.smooth_far_layer = false;
    ip.opt.assets = lib;
    ip.opt.wide = wide;
    PairMotion mo = ip.analyse(nullptr, r.cap.steps()[0], r.cap.steps()[1]);
    std::vector<uint8_t> out;
    ip.render(r.cap.steps()[0], r.cap.steps()[1], mo, t, out);
    return out;
}

void test_packs() {
    std::printf("asset packs: replacement, scale, anchor, transparency, palette override, collisions, errors, zip\n");
    Rig r;
    r.step(10, 64, 50);
    r.step(9, 96, 42);
    std::vector<Graphic> gs;
    find_graphics(r.cap.steps()[0], gs, false);
    const Graphic* bob = sprite_of(gs);
    CHECK(bob != nullptr);
    if (!bob) return;
    const std::string key = graphic_key(bob->hash, 16, 8), other = graphic_key(bob->hash, 16, 9);
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u;
    const std::string dir = g_tmp + "/pack1";
    fs::remove_all(dir);
    // 2x picture: red with one transparent Amiga pixel (2x2 picture pixels) at (3, 2).
    put_png(dir + "/gfx/bob.png", 32, 16, [&](int x, int y) { return x / 2 == 3 && y / 2 == 2 ? 0u : red; });
    // 4x picture: left half red, right half blue.
    put_png(dir + "/gfx/bob4.png", 64, 32, [&](int x, int) { return x < 32 ? red : blue; });
    put_file(dir + "/manifest.json",
             "{\"format\": \"x2-asset-pack\", \"version\": 1, \"scale\": 2, \"graphics\": {\n"
             "  \"" + key + "\": {\"png\": \"gfx/bob.png\"},\n"
             "  \"" + other + "\": {\"png\": \"gfx/bob4.png\", \"scale\": 4},\n"
             "  \"0000000000000001-16x8\": {\"png\": \"gfx/missing.png\"},\n"
             "  \"0000000000000002-16x8\": {\"png\": \"../outside.png\"},\n"
             "  \"0000000000000003-16x8\": {\"png\": \"/etc/passwd\"},\n"
             "  \"not-a-key\": {\"png\": \"gfx/bob.png\"}},\n"
             " \"palettes\": [{\"match\": [\"#000\", \"#111\", \"#222\", \"#333\", \"#444\", \"#555\", \"#666\", \"#777\", "
             "\"#888\", \"#999\", \"#aaa\", \"#bbb\", \"#ccc\", \"#ddd\", \"#eee\", \"#fff\"],\n"
             "   \"colors\": [\"#000000\", \"#00ff00\", \"#222222\", \"#333333\", \"#444444\", \"#555555\", \"#666666\", "
             "\"#777777\", \"#888888\", \"#999999\", \"#aaaaaa\", \"#bbbbbb\", \"#cccccc\", \"#dddddd\", \"#eeeeee\", \"#ffffff\"],\n"
             "   \"layers\": [\"far\"]}]}\n");
    AssetLibrary lib;
    std::vector<std::string> msgs;
    CHECK(lib.load(dir, msgs));
    auto said = [&](const char* what) {
        for (const std::string& m : msgs)
            if (m.find(what) != std::string::npos) return true;
        return false;
    };
    CHECK(lib.graphics() == 2 && lib.palettes() == 1); // the same hash at two sizes: two entries
    CHECK(said("missing file 'gfx/missing.png'") && said("unsafe path '../outside.png'") && said("unsafe path '/etc/passwd'") &&
          said("not-a-key"));

    // Scale 2, t = 0: the bob (64, 50) is red, its transparent pixel shows the far layer, the far layer's colour 1
    // is green (layer palette override) and the panel keeps the original palette.
    std::vector<uint8_t> none = render_with(r, nullptr, 2, 0.0f), got = render_with(r, &lib, 2, 0.0f);
    const int W = 2 * kWidth;
    CHECK(pixel(got, W, 2 * 64, 2 * 50) == 0x0000FFu && pixel(got, W, 2 * 79 + 1, 2 * 57 + 1) == 0x0000FFu);
    CHECK(pixel(got, W, 2 * 67, 2 * 52) == pixel(got, W, 2 * 60, 2 * 52)); // transparent: the far layer at line 52
    const uint32_t far_none = pixel(none, W, 2 * 10, 2 * 8), far_got = pixel(got, W, 2 * 10, 2 * 8);
    CHECK(far_none == 0x111111u && far_got == 0x00FF00u);                     // line 8: far colour 1
    CHECK(pixel(got, W, 2 * 10, 2 * 195) == pixel(none, W, 2 * 10, 2 * 195)); // panel (layer other): unchanged
    // t = 0.5: the replacement moves with its object (b's list moved back: x 80, line 46).
    got = render_with(r, &lib, 2, 0.5f);
    CHECK(pixel(got, W, 2 * 80, 2 * 46) == 0x0000FFu && pixel(got, W, 2 * 79, 2 * 46) != 0x0000FFu);
    // Canvas scale 1 from the 2x picture: area-averaged, the transparent pixel blends with the far layer.
    got = render_with(r, &lib, 1, 0.0f);
    CHECK(pixel(got, kWidth, 64, 50) == 0x0000FFu);

    // Anchor: the picture pixel (2, 1) on the original's corner moves the picture 1 Amiga pixel up-left.
    const std::string dir2 = g_tmp + "/pack2";
    fs::remove_all(dir2);
    put_png(dir2 + "/b.png", 32, 16, [&](int, int) { return blue; });
    put_file(dir2 + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 1, \"graphics\": {\"" + key +
                                          "\": {\"png\": \"b.png\", \"scale\": 2, \"anchor\": [2, 2]}}}");
    AssetLibrary lib2;
    msgs.clear();
    CHECK(lib2.load(dir2, msgs));
    got = render_with(r, &lib2, 2, 0.0f);
    CHECK(pixel(got, W, 2 * 63, 2 * 49) == 0xFF0000u && pixel(got, W, 2 * 78, 2 * 56) == 0xFF0000u);
    CHECK(pixel(got, W, 2 * 79, 2 * 57) != 0xFF0000u); // the original's last pixel is not covered any more
    // Later packs override earlier ones (pack2 after pack1: blue).
    msgs.clear();
    CHECK(lib.load(dir2, msgs));
    got = render_with(r, &lib, 2, 0.0f);
    CHECK(pixel(got, W, 2 * 70, 2 * 54) == 0xFF0000u);

    // The same pack as a ZIP, inside a top-level folder.
    std::vector<std::pair<std::string, std::string>> files = {{"mypack/manifest.json", slurp(dir2 + "/manifest.json")},
                                                              {"mypack/b.png", slurp(dir2 + "/b.png")}};
    put_file(g_tmp + "/pack2.zip", make_zip(files));
    AssetLibrary libz;
    msgs.clear();
    CHECK(libz.load(g_tmp + "/pack2.zip", msgs) && libz.graphics() == 1);
    CHECK(render_with(r, &libz, 2, 0.0f) == render_with(r, &lib2, 2, 0.0f));
    files[1].first = "mypack/../b.png";
    put_file(g_tmp + "/evil.zip", make_zip(files));
    AssetLibrary libe;
    msgs.clear();
    CHECK(libe.load(g_tmp + "/evil.zip", msgs) && libe.graphics() == 0 && said("missing file 'b.png'"));

    // Refused: a picture larger than the limits (checked from its header), an unknown format, broken JSON.
    std::string huge = "\x89PNG\r\n\x1a\n";
    std::string ihdr = std::string("IHDR") + std::string("\x00\x01\x86\xa0\x00\x00\x00\x0a\x08\x06\x00\x00\x00", 13);
    const uint32_t crc = x2::import::crc32(reinterpret_cast<const uint8_t*>(ihdr.data()), ihdr.size());
    huge += std::string("\x00\x00\x00\x0d", 4) + ihdr + char(crc >> 24) + char(crc >> 16) + char(crc >> 8) + char(crc);
    const std::string dir3 = g_tmp + "/pack3";
    fs::remove_all(dir3);
    put_file(dir3 + "/huge.png", huge);
    put_file(dir3 + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 1, \"graphics\": {\"" + key + "\": {\"png\": \"huge.png\"}}}");
    AssetLibrary lib3;
    msgs.clear();
    CHECK(lib3.load(dir3, msgs) && lib3.graphics() == 0 && said("too large or empty (100000x10"));
    put_file(dir3 + "/manifest.json", "{\"format\": \"something-else\", \"version\": 1}");
    msgs.clear();
    CHECK(!lib3.load(dir3, msgs) && said("\"format\" must be"));
    put_file(dir3 + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 1,");
    msgs.clear();
    CHECK(!lib3.load(dir3, msgs) && said("manifest.json:"));
    msgs.clear();
    CHECK(!lib3.load(g_tmp + "/no-such-pack", msgs) && said("no such asset pack"));
    // A pack over the memory limit for all pictures.
    AssetLibrary small;
    small.limits.max_total = 1000;
    msgs.clear();
    CHECK(small.load(dir, msgs) && small.graphics() == 0 && said("memory limit"));

    // An empty library ("upgrade" without packs) renders exactly as no library.
    AssetLibrary empty;
    for (float t : {0.0f, 0.4f, 0.6f, 1.0f}) CHECK(render_with(r, &empty, 3, t) == render_with(r, nullptr, 3, t));
}

void test_dumper() {
    std::printf("asset dump: every graphic once, loadable as a pack that reproduces the original\n");
    Rig r;
    r.step(10, 64, 50);
    r.step(9, 69, 42); // shifted: the same key as the first
    const std::string dir = g_tmp + "/dump";
    fs::remove_all(dir);
    AssetDumper d;
    std::string why;
    CHECK(d.open(dir, why));
    for (const Step& s : r.cap.steps()) d.step(s);
    CHECK(d.finish(why));
    // The far ring's stripes are 2 distinct lines (the bob data inside the ring area adds one more).
    CHECK(d.sprites() == 1 && d.tiles() == 0 && d.far_lines() == 3 && d.palettes() == 1);
    const std::string manifest = slurp(dir + "/manifest.json");
    CHECK(manifest.find("\"uses\": 2") != std::string::npos && fs::exists(dir + "/far/strip_000.png"));
    AssetLibrary lib;
    std::vector<std::string> msgs;
    CHECK(lib.load(dir, msgs) && lib.graphics() == 1 && lib.far_lines() == 3);
    for (int scale : {1, 2, 3})
        for (float t : {0.0f, 0.5f, 1.0f}) CHECK(render_with(r, &lib, scale, t) == render_with(r, nullptr, scale, t));
}

// --- Phase 9e: surround ---

void test_wide() {
    std::printf("surround: widths, centre exact, scenery-only sides, objects clipped at 320\n");
    WideSpec w;
    CHECK(parse_wide("16:9", w) && wide_pixels(w, false) == 68 && wide_pixels(w, true) == 53);
    CHECK(parse_wide("16:10", w) && wide_pixels(w, false) == 45);
    CHECK(parse_wide("21:9", w) && wide_pixels(w, false) == 139);
    CHECK(parse_wide("32:9", w) && wide_pixels(w, false) == 295);
    CHECK(parse_wide("4:3", w) && wide_pixels(w, false) == 11);
    CHECK(parse_wide("40", w) && wide_pixels(w, true) == 40 && wide_name(w) == "40");
    CHECK(parse_wide("off", w) && !w.on());
    for (const char* bad : {"400", "x", "16:", ":9", "16:9x", "-3"}) CHECK(!parse_wide(bad, w));
    WideSpec c;
    std::string cycle;
    for (int i = 0; i < 5; ++i) { c = next_wide(c); cycle += wide_name(c) + " "; }
    CHECK(cycle == "16:10 16:9 21:9 32:9 off ");

    Rig r;
    r.step(10, 64, 50);
    r.step(9, 300, 42); // the bob ends at x 315, by the right edge
    for (int scale : {1, 2, 3}) {
        const int wd = 24, W = (kWidth + 2 * wd) * scale;
        std::vector<uint8_t> pa, pb;
        Interpolator::picture(r.cap.steps()[0], scale, pa);
        Interpolator::picture(r.cap.steps()[1], scale, pb);
        for (float t : {0.0f, 0.5f, 1.0f}) {
            std::vector<uint8_t> out = render_with(r, nullptr, scale, t, wd), plain = render_with(r, nullptr, scale, t);
            CHECK(out.size() == size_t(W) * size_t(kLines * scale) * 4);
            bool centre = true, clean = true;
            for (int y = 0; y < kLines * scale; ++y) {
                centre = centre && std::equal(&out[(size_t(y) * size_t(W) + size_t(wd * scale)) * 4],
                                              &out[(size_t(y) * size_t(W) + size_t((wd + kWidth) * scale)) * 4],
                                              &plain[size_t(y) * size_t(kWidth * scale) * 4]);
                for (int x = 0; x < wd * scale; ++x)
                    for (int X : {x, W - 1 - x}) clean = clean && pixel(out, W, X, y) != 0xFFFFFFu; // no bob (colour 15)
            }
            CHECK(centre);
            CHECK(clean);
            if (t == 0.0f) CHECK(plain == pa);
            if (t == 1.0f) CHECK(plain == pb);
            // Playfield lines: the far layer's stripes (colour 1 on line 8) continue into the sides, darkened
            // with the distance from the edge; panel lines are black.
            const uint32_t seam = pixel(out, W, wd * scale - 1, 8 * scale), outer = pixel(out, W, 0, 8 * scale);
            CHECK(seam == 0x0B0B0Bu && outer == 0x050505u && pixel(out, W, W - 1, 8 * scale) == outer);
            CHECK(pixel(out, W, 0, 195 * scale) == 0 && pixel(out, W, W - 1, 195 * scale) == 0);
        }
    }
}

void test_presenter_wide_panel() {
    std::printf("presenter: surround without interpolation keeps the frame exactly; HD panel\n");
    Rig r;
    std::vector<uint8_t> front(size_t(kFrameWidth) * kFrameHeight * 4);
    for (size_t i = 0; i < front.size(); ++i) front[i] = uint8_t(i % 4 == 3 ? 255 : (i * 13) >> 3);
    Presenter p;
    PresentOptions o;
    parse_wide("16:9", o.wide);
    o.scale = 1;
    p.configure(r.m, o, 313);
    CHECK(p.wide() == 68 && p.interp() != nullptr);
    r.step(10, 64, 50);
    r.step(9, 96, 42);
    Canvas c = p.at(7.5, front);
    const int W = kFrameWidth + 2 * 68;
    CHECK(c.w == W && c.h == kFrameHeight && c.scale == 1);
    bool centre = true;
    for (int y = 0; y < kFrameHeight; ++y)
        centre = centre && std::equal(c.rgba + (size_t(y) * W + 68) * 4, c.rgba + (size_t(y) * W + 68 + kFrameWidth) * 4,
                                      &front[size_t(y) * kFrameWidth * 4]);
    CHECK(centre);
    std::vector<uint8_t> v(c.rgba, c.rgba + size_t(c.w) * size_t(c.h) * 4);
    CHECK(pixel(v, W, 0, kFrameTop + 8) != 0 && pixel(v, W, 0, kFrameTop + 195) == 0 && pixel(v, W, 0, 2) == 0);
    // Faithful turns it all off.
    o.faithful = true;
    p.configure(r.m, o, 313);
    CHECK(p.at(7.5, front).rgba == front.data() && p.wide() == 0);

    // HD panel at scale 2: the panel lines are the Scale2x upscale of the frame's, the rest is replicated.
    PresentOptions h;
    h.hd_panel = true;
    h.scale = 2;
    Presenter q;
    q.configure(r.m, h, 313);
    c = q.at(100.0, front);
    CHECK(c.w == 2 * kFrameWidth && c.h == 2 * kFrameHeight && c.scale == 2);
    std::vector<uint32_t> panel(size_t(kFrameWidth) * 8 * 4);
    scale2x(reinterpret_cast<const uint32_t*>(front.data()) + size_t(kFrameTop + 192) * kFrameWidth, kFrameWidth, 8, panel.data());
    const uint32_t* px = reinterpret_cast<const uint32_t*>(c.rgba);
    CHECK(std::equal(panel.begin(), panel.end(), px + size_t(2 * (kFrameTop + 192)) * size_t(c.w)));
    CHECK(px[size_t(2 * 20) * size_t(c.w) + 2 * 30 + 1] == reinterpret_cast<const uint32_t*>(front.data())[20 * kFrameWidth + 30]);
}

} // namespace

// The per-build capture layouts (modern/layout.h): the disk layout holds the values that used to be constants, the CDTV layout is
// self-consistent and shifted the way the engine was (variables and list heads by +6, the screen pointers by +2, the drawing routines
// elsewhere), and a capture made with it reads those addresses and no others.
void test_layouts() {
    std::printf("layouts: disk values unchanged, CDTV layout consistent, a capture follows its layout\n");
    const CaptureLayout& D = kDiskLayout;
    const CaptureLayout& C = kCdtvLayout;
    CHECK(D.draw_ptr == 0x406 && D.show_ptr == 0x40A && D.star_table == 0x42FDC && D.object_draw_return == 0x3678);
    CHECK(D.list_heads[0] == 0xA88 && D.list_heads[4] == 0xC10);
    CHECK(D.far_copy_a == 0x1832 && D.far_copy_b == 0x185A && D.far_clear == 0x1884 && D.tile_lo == 0x18A0 && D.tile_hi == 0x1EA2);
    CHECK(D.star_lo == 0x233A && D.star_hi == 0x23EE && !D.star_writes_exact);
    CHECK(C.star_writes_exact);
    // The CDTV engine variables sit at the disk addresses + 6 (the ring row $436 -> $43C, the scroll variables, the list heads) and
    // the two screen pointers at +2; the object list heads keep the disk's spacing.
    for (int i = 0; i < kVarCount; ++i) CHECK(C.var_addr[size_t(i)] == D.var_addr[size_t(i)] + 6);
    for (size_t i = 0; i < 5; ++i) CHECK(C.list_heads[i] == D.list_heads[i] + 6);
    CHECK(C.draw_ptr == D.draw_ptr + 2 && C.show_ptr == C.draw_ptr + 4);
    for (const CaptureLayout* L : {&D, &C}) {
        // The routines are ordered and disjoint, so a PC is in at most one class.
        CHECK(L->far_copy_a < L->far_copy_b && L->far_copy_b < L->far_clear && L->far_clear < L->tile_lo && L->tile_lo < L->tile_hi);
        CHECK(L->tile_hi <= L->star_lo && L->star_lo < L->star_split[0] && L->star_split[0] < L->star_split[1] &&
              L->star_split[1] < L->star_split[2] && L->star_split[2] < L->star_hi);
        CHECK(L->far_pc(L->far_copy_a) && L->far_pc(L->far_clear) && L->far_copy_pc(L->far_copy_b) && !L->far_copy_pc(L->far_clear));
        CHECK(L->tile_pc(L->tile_lo) && L->tile_pc(L->tile_hi - 2) && !L->tile_pc(L->tile_hi) && !L->tile_pc(L->tile_lo - 2));
        CHECK(L->star_pc(L->star_lo) && !L->star_pc(L->star_hi));
        CHECK(L->star_planes(L->star_lo) == 4 && L->star_planes(L->star_split[0]) == 5 && L->star_planes(L->star_split[1]) == 6 &&
              L->star_planes(L->star_split[2]) == 7);
        CHECK(!L->far_pc(L->tile_lo) && !L->tile_pc(L->far_clear) && !L->star_pc(L->tile_lo));
    }
    // The other build's drawing PCs are not this build's.
    CHECK(!D.far_pc(C.far_copy_a) && !C.far_pc(D.far_copy_a) && !D.tile_pc(C.tile_lo) && !C.star_pc(D.star_lo));
    BlitCmd far;
    far.pc = C.far_copy_a;
    CHECK(layer_of(far, C) == Layer::Far && layer_of(far, D) == Layer::Other);
    BlitCmd tile;
    tile.pc = C.tile_lo + 4;
    CHECK(layer_of(tile, C) == Layer::Tiles && layer_of(tile, D) == Layer::Other);
    BlitCmd obj = tile;
    obj.obj = 0x1000;
    CHECK(layer_of(obj, C) == Layer::Object && layer_of(obj, D) == Layer::Object);

    // A capture with the CDTV layout on a 1 MB machine: pointers at $408/$40C, the object list at $B52 found through the
    // return address $3674E on the stack, the variables from the CDTV addresses, the step closing on the show pointer's low word.
    Machine m(1024 * 1024);
    m.set_timing(Timing::Fast);
    m.custom_write(reg::DMACON, 0x8240);
    DrawCapture cap;
    cap.layout = &C;
    constexpr uint32_t buf_a = 0x84000, buf_b = 0x8C700, obj_rec = 0x3F000, sentinel = 0x3F100, stack = 0x3000;
    m.write32(C.draw_ptr, buf_a);
    m.write32(C.show_ptr, buf_b);
    for (uint32_t h : C.list_heads) m.write32(h, sentinel);
    m.write32(C.list_heads[2], obj_rec);
    m.write16(obj_rec, 1);
    m.write32(obj_rec + 0x12, sentinel);
    m.write16(obj_rec + 0x20, 33);
    m.write16(obj_rec + 0x24, 44);
    m.write16(sentinel, 0);
    m.write32(stack, C.object_draw_return);
    m.write32(stack + 4, sentinel);
    m.write16(C.var_addr[kRing], 7);
    m.write16(0x436, 99); // the disk build's ring row address: must not be read
    cap.attach(m);
    m.set_areg(7, stack);
    m.frame += 3;
    blit(m, {C.far_copy_a, 0x09F0, 0, 0xFFFF, 0xFFFF, 0x60000, 0, 0, buf_a, 0, 0, 0, 80, 0, 40, 4});
    blit(m, {C.tile_lo + 2, 0x0FCA, 0, 0xFFFF, 0xFFFF, 0x62000, 0x62010, buf_a + 160 * 20, buf_a + 160 * 20, 0, 0, 158, 158, 0, 1, 8});
    m68k_set_reg(M68K_REG_PPC, 0x34F36); // the swap's `move.l d0,$40C`
    m.write32(C.draw_ptr, buf_b);
    m.write32(C.show_ptr, buf_a);
    CHECK(cap.steps().size() == 1);
    if (!cap.steps().empty()) {
        const Step& st = cap.steps()[0];
        CHECK(st.layout == &C && st.draw_buf == buf_a && st.other_buf == buf_b && st.vars[kRing] == 7);
        CHECK(st.blits.size() == 2 && st.blits[0].buffer == 0 && st.blits[1].buffer == 0 && st.blits[1].y == 20);
        CHECK(st.blits[1].obj == obj_rec && st.blits[0].obj == obj_rec); // both ran with the object's draw handler on the stack
        CHECK(st.objects.size() == 1 && st.objects[0].addr == obj_rec && st.objects[0].list == 2 && st.objects[0].x == 33 &&
              st.objects[0].y == 44);
        CHECK(layer_of(st.blits[0], *st.layout) == Layer::Object);
    }
    cap.detach(m);
}

int main(int argc, char** argv) {
    if (argc > 1) g_tmp = argv[1];
    fs::create_directories(g_tmp);
    test_layouts();
    test_capture();
    test_interp();
    test_not_gameplay();
    test_linear();
    test_palette_blend();
    test_cuts();
    test_presenter();
    test_edge_scalers();
    test_pacing();
    test_graphics();
    test_png_paths_resample();
    test_packs();
    test_dumper();
    test_wide();
    test_presenter_wide_panel();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
