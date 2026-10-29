// Draw-command capture and interpolation tests on synthetic chip RAM (no game data). The CPU does not run: the
// test plays the game's part, programming the blitter and writing the buffer pointers the way the game's
// drawing routines do (docs/re-notes/modernisation-design.md), with REG_PPC set to the game's instruction
// addresses so the capture classifies the blits as it would in the game.
#include "custom_regs.h"
#include "machine.h"
#include "modern/draw_capture.h"
#include "modern/interp.h"

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
} // namespace

int main() {
    test_capture();
    test_interp();
    test_not_gameplay();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
