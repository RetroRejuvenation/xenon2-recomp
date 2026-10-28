// Video, blitter line mode and frame pacing tests on synthetic chip RAM (no game data). Each test builds a
// Machine whose CPU idles in `bra.s *`, programs the display through a copper list, runs whole frames and
// checks pixels of the rendered framebuffer.
#include "gfx/image_io.h"
#include "gfx/pacer.h"
#include "gfx/video.h"
#include "machine.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
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

constexpr uint32_t kCopper = 0x2000, kPlanes = 0x10000, kSprites = 0x30000, kBitmap = 0x40000;
constexpr int kRowBytes = 40;
constexpr uint16_t color(int n) { return uint16_t(reg::COLOR00 + 2 * n); }

void poke(Machine& m, uint32_t addr, std::initializer_list<uint16_t> words) {
    for (uint16_t w : words) { m.write16(addr, w); addr += 2; }
}

struct Rig {
    Machine m;
    Video v;
    std::vector<uint16_t> cop; // copper list words, END appended by start()

    Rig() {
        poke(m, 0x1000, {0x60FE}); // bra.s *
        m.start_cpu(0x1000, 0x8000);
        v.attach(m);
    }
    void move(uint16_t reg, uint16_t value) { cop.insert(cop.end(), {reg, value}); }
    void move_ptr(uint16_t reg, uint32_t addr) {
        move(reg, uint16_t(addr >> 16));
        move(uint16_t(reg + 2), uint16_t(addr));
    }
    void wait(int vline, int hpos) { cop.insert(cop.end(), {uint16_t((vline << 8) | (hpos & 0xFE) | 1), 0xFFFE}); }
    // Standard PAL lores window 320x256 at ($81,$2C), `planes` bitplanes at kPlanes + p * $2800.
    void display(int planes, uint16_t extra_bplcon0 = 0) {
        m.write16(0xDFF08E, 0x2C81);
        m.write16(0xDFF090, 0x2CC1);
        m.write16(0xDFF092, 0x0038);
        m.write16(0xDFF094, 0x00D0);
        m.write16(0xDFF108, 0);
        m.write16(0xDFF10A, 0);
        m.write16(0xDFF100, uint16_t((planes << 12) | 0x0200 | extra_bplcon0));
        for (int p = 0; p < planes; ++p) move_ptr(uint16_t(reg::BPL1PTH + 4 * p), kPlanes + uint32_t(p) * 0x2800);
    }
    void start(uint16_t extra_dma = 0) {
        cop.insert(cop.end(), {0xFFFF, 0xFFFE});
        uint32_t a = kCopper;
        for (uint16_t w : cop) { m.write16(a, w); a += 2; }
        m.write16(0xDFF080, uint16_t(kCopper >> 16));
        m.write16(0xDFF082, uint16_t(kCopper));
        m.write16(0xDFF096, uint16_t(0x8000 | reg::DMAF_MASTER | reg::DMAF_COPPER | reg::DMAF_RASTER | extra_dma));
        m.run_frame();
        m.run_frame();
    }
    // Same 16-bit word in the given column of every row of plane p.
    void fill_column(int p, int word, uint16_t value) {
        for (int row = 0; row < 256; ++row)
            poke(m, kPlanes + uint32_t(p) * 0x2800 + uint32_t(row * kRowBytes + word * 2), {value});
    }
    // Framebuffer pixel as 12-bit Amiga colour (exact for lores, all guns are x17 multiples).
    uint16_t px(int x, int y) const {
        const uint8_t* p = &v.front()[(size_t(y) * Video::kFbWidth + size_t(x)) * 4];
        return uint16_t(((p[0] / 17) << 8) | ((p[1] / 17) << 4) | (p[2] / 17));
    }
    int y_of(int line) const { return line - Video::kFbTop; }
};

void test_palette_expansion() {
    CHECK(Video::rgba_of(0x000) == 0xFF000000u);
    CHECK(Video::rgba_of(0xFFF) == 0xFFFFFFFFu);
    CHECK(Video::rgba_of(0x18F) == (0x11u | (0x88u << 8) | (0xFFu << 16) | 0xFF000000u));
}

void test_copper_palette() {
    Rig r;
    r.display(0);
    r.move(reg::COLOR00, 0xF00);
    r.wait(0x80, 0x06);
    r.move(reg::COLOR00, 0x00F);
    r.wait(0xA0, 0x80); // mid-line
    r.move(reg::COLOR00, 0x0F0);
    r.start();
    CHECK(r.v.frames_completed() >= 1);
    CHECK(r.px(10, r.y_of(0x7F)) == 0xF00);
    CHECK(r.px(10, r.y_of(0x80)) == 0x00F);
    CHECK(r.px(300, r.y_of(0x80)) == 0x00F);
    CHECK(r.px(0, r.y_of(0xA0)) == 0x00F);   // x=$81 is colour clock $40, before the WAIT
    CHECK(r.px(300, r.y_of(0xA0)) == 0x0F0); // colour clock $D6, after it
    CHECK(r.px(0, r.y_of(0xA1)) == 0x0F0);
    CHECK(r.px(0, 0) == 0xF00); // line $2C, after the copper restarts at the top of the frame
}

void test_bitplanes_and_scroll() {
    for (int scroll : {0, 3}) {
        Rig r;
        r.display(1);
        r.move(reg::COLOR00, 0x000);
        r.move(color(1), 0xFFF);
        r.move(reg::BPLCON1, uint16_t(scroll));
        r.fill_column(0, 0, 0x8000);
        poke(r.m, kPlanes + 10 * kRowBytes, {0x4000}); // row 10: second pixel instead
        poke(r.m, kPlanes + 20 * kRowBytes + 38, {0x0001}); // row 20: last pixel of the line
        r.start();
        CHECK(r.px(0 + scroll, 0) == 0xFFF);
        CHECK(r.px(1 + scroll, 0) == 0x000);
        CHECK(r.px(0 + scroll, 255) == 0xFFF);
        CHECK(r.px(1 + scroll, 10) == 0xFFF);
        CHECK(r.px(0 + scroll, 10) == 0x000);
        CHECK(r.px(319, 20) == (scroll ? 0x000 : 0xFFF)); // scrolled past the window edge
        if (scroll) CHECK(r.px(scroll - 1, 0) == 0x000);
    }
}

void test_window_border() {
    Rig r;
    r.display(1);
    r.m.write16(0xDFF08E, 0x3A91); // window from line $3A, x $91
    r.move(reg::COLOR00, 0x123);
    r.move(color(1), 0xFFF);
    for (int w = 0; w < 20; ++w) r.fill_column(0, w, 0xFFFF);
    r.start();
    CHECK(r.px(100, r.y_of(0x39)) == 0x123);
    CHECK(r.px(100, r.y_of(0x3A)) == 0xFFF);
    CHECK(r.px(15, r.y_of(0x50)) == 0x123); // x = $90
    CHECK(r.px(16, r.y_of(0x50)) == 0xFFF); // x = $91
}

void test_dual_playfield() {
    for (bool pf2pri : {false, true}) {
        Rig r;
        r.display(2, 0x0400);
        r.move(color(1), 0x00F);
        r.move(color(9), 0xF00);
        r.move(color(10), 0x0F0);
        r.move(reg::BPLCON2, pf2pri ? 0x0040 : 0x0000);
        r.fill_column(0, 0, 0x8000); // PF1 pixel 0
        r.fill_column(1, 0, 0xC000); // PF2 pixels 0 and 1
        r.start();
        CHECK(r.px(0, 50) == (pf2pri ? 0xF00 : 0x00F));
        CHECK(r.px(1, 50) == 0xF00); // PF1 transparent there
        CHECK(r.px(2, 50) == 0x000);
    }
}

void test_ham_and_ehb() {
    {
        Rig r;
        r.display(6, 0x0800);
        r.move(reg::COLOR00, 0x000);
        r.move(color(1), 0x00F);
        const uint16_t planes[6] = {0xC000, 0x4000, 0x4000, 0x6000, 0x2000, 0x6000};
        for (int p = 0; p < 6; ++p) r.fill_column(p, 0, planes[p]);
        r.start();
        CHECK(r.px(0, 40) == 0x00F); // set colour 1
        CHECK(r.px(1, 40) == 0xF0F); // modify red = $F
        CHECK(r.px(2, 40) == 0xF8F); // modify green = 8
        CHECK(r.px(3, 40) == 0x000); // control 00, data 0: set COLOR00
    }
    {
        Rig r;
        r.display(6); // 6 planes without HAM/DPF = extra half-brite
        r.move(color(3), 0xEEE);
        r.fill_column(0, 0, 0x8000);
        r.fill_column(1, 0, 0x8000);
        r.fill_column(5, 0, 0x8000); // index 35 = half of COLOR03
        r.start();
        CHECK(r.px(0, 40) == 0x777);
    }
}

// Sprite DMA list at kSprites + 0x100*n: POS/CTL, `lines` x DATA/DATB, then 0/0.
void sprite_list(Rig& r, int n, int vstart, int hpos, int lines, uint16_t data, uint16_t datb, bool attach = false) {
    uint32_t a = kSprites + uint32_t(n) * 0x100;
    int vstop = vstart + lines;
    uint16_t pos = uint16_t(((vstart & 0xFF) << 8) | ((hpos >> 1) & 0xFF));
    uint16_t ctl = uint16_t(((vstop & 0xFF) << 8) | (attach ? 0x80 : 0) | ((vstart >> 6) & 4) | ((vstop >> 7) & 2) | (hpos & 1));
    poke(r.m, a, {pos, ctl});
    a += 4;
    for (int i = 0; i < lines; ++i, a += 4) poke(r.m, a, {data, datb});
    poke(r.m, a, {0, 0});
    r.move_ptr(uint16_t(reg::SPR0PTH + 4 * n), kSprites + uint32_t(n) * 0x100);
}

void test_sprites() {
    { // single sprite, colour and position
        Rig r;
        r.display(1);
        r.move(color(17), 0x0F0);
        r.move(color(18), 0xF00);
        r.move(color(19), 0xFF0);
        sprite_list(r, 0, 0x60, 0x90, 4, 0x8001, 0x0001);
        r.start(reg::DMAF_SPRITE);
        int y = r.y_of(0x60);
        CHECK(r.px(15, y) == 0x000);
        CHECK(r.px(16, y) == 0x0F0); // H=$90 shows at x=$91
        CHECK(r.px(17, y) == 0x000);
        CHECK(r.px(31, y) == 0xFF0); // DATA and DATB bit 0: colour 3
        CHECK(r.px(16, y + 3) == 0x0F0);
        CHECK(r.px(16, y + 4) == 0x000); // VSTOP line
        CHECK(r.px(16, y - 1) == 0x000);
    }
    { // attached pair 0/1: 4-bit colour
        Rig r;
        r.display(1);
        r.move(color(29), 0x0FF);
        sprite_list(r, 0, 0x70, 0xA0, 2, 0x8000, 0x0000);              // low bits = 1
        sprite_list(r, 1, 0x70, 0xA0, 2, 0x8000, 0x8000, true);        // high bits = 3
        r.start(reg::DMAF_SPRITE);
        CHECK(r.px(0xA1 - 0x81, r.y_of(0x70)) == 0x0FF); // 1 | 3<<2 = 13 -> COLOR29
    }
    // Priority against the playfield (BPLCON2) and between sprites.
    for (uint16_t bplcon2 : {0x0000, 0x0009, 0x0012}) {
        Rig r;
        r.display(1);
        r.move(color(1), 0x00F);
        r.move(color(17), 0x0F0); // sprite 0
        r.move(color(21), 0xF00); // sprite 2
        r.move(reg::BPLCON2, bplcon2);
        for (int w = 0; w < 20; ++w) r.fill_column(0, w, 0xFFFF);
        sprite_list(r, 0, 0x80, 0x90, 2, 0xFFFF, 0);
        sprite_list(r, 2, 0x80, 0x98, 2, 0xFFFF, 0);
        r.start(reg::DMAF_SPRITE);
        int y = r.y_of(0x80);
        // Sprite 0 covers x=$91..$A0, sprite 2 x=$99..$A8; sprite 0 beats sprite 2 where they overlap.
        uint16_t s0 = r.px(0x91 - 0x81, y), overlap = r.px(0x99 - 0x81, y), s2 = r.px(0xA1 - 0x81, y);
        if (bplcon2 == 0x0000) { CHECK(s0 == 0x00F); CHECK(overlap == 0x00F); CHECK(s2 == 0x00F); }
        if (bplcon2 == 0x0009) { CHECK(s0 == 0x0F0); CHECK(overlap == 0x0F0); CHECK(s2 == 0x00F); }
        if (bplcon2 == 0x0012) { CHECK(s0 == 0x0F0); CHECK(overlap == 0x0F0); CHECK(s2 == 0xF00); }
    }
}

void test_manual_sprite() {
    Rig r;
    r.display(1);
    r.move(color(17), 0xABC);
    r.move(reg::SPR0POS, 0x00C0); // H = $180
    r.move(reg::SPR0POS + 6, 0x0000);
    r.move(reg::SPR0POS + 4, 0x8000); // DATA write arms the sprite on every line until CTL is written
    r.start();
    CHECK(r.px(0x181 - 0x81, 10) == 0xABC);
    CHECK(r.px(0x181 - 0x81, 200) == 0xABC);
}

// ---- blitter line mode ----

void draw_line(Machine& m, int x1, int y1, int x2, int y2, uint8_t minterm = 0xCA, bool sing = false) {
    int dx = x2 - x1, dy = y2 - y1, adx = std::abs(dx), ady = std::abs(dy);
    bool sud = adx >= ady;
    int l = sud ? adx : ady, s = sud ? ady : adx;
    bool aul = sud ? dx < 0 : dy < 0, sul = sud ? dy < 0 : dx < 0;
    int err = 4 * s - 2 * l;
    m.write16(0xDFF096, 0x8000 | reg::DMAF_MASTER | reg::DMAF_BLITTER);
    m.write16(0xDFF040, uint16_t(((x1 & 15) << 12) | 0x0B00 | minterm));
    m.write16(0xDFF042, uint16_t((sud << 4) | (sul << 3) | (aul << 2) | (sing << 1) | 1 | (err < 0 ? 0x40 : 0)));
    m.write16(0xDFF050, 0);
    m.write16(0xDFF052, uint16_t(err));
    m.write16(0xDFF064, uint16_t(4 * (s - l)));
    m.write16(0xDFF062, uint16_t(4 * s));
    m.write16(0xDFF060, kRowBytes);
    m.write16(0xDFF066, kRowBytes);
    m.write16(0xDFF074, 0x8000);
    m.write16(0xDFF072, 0xFFFF);
    m.write32(0xDFF044, 0xFFFFFFFF);
    uint32_t start = kBitmap + uint32_t(y1 * kRowBytes + (x1 >> 4) * 2);
    m.write32(0xDFF048, start);
    m.write32(0xDFF054, start);
    m.write16(0xDFF058, uint16_t(((l + 1) << 6) | 2));
}

bool pixel(Machine& m, int x, int y) { return (m.read8(kBitmap + uint32_t(y * kRowBytes + (x >> 3))) >> (7 - (x & 7))) & 1; }

int count_pixels(Machine& m) {
    int n = 0;
    for (uint32_t a = kBitmap; a < kBitmap + 200 * kRowBytes; ++a)
        for (int b = 0; b < 8; ++b) n += (m.read8(a) >> b) & 1;
    return n;
}

// Every column (x-major) or row (y-major) has exactly one pixel, within half a pixel of the ideal line.
void check_line(Machine& m, int x1, int y1, int x2, int y2) {
    bool xmajor = std::abs(x2 - x1) >= std::abs(y2 - y1);
    int a0 = xmajor ? x1 : y1, a1 = xmajor ? x2 : y2, step = a1 >= a0 ? 1 : -1;
    bool all_ok = true;
    for (int a = a0;; a += step) {
        double t = a1 == a0 ? 0 : double(a - a0) / (a1 - a0);
        double ideal = xmajor ? y1 + t * (y2 - y1) : x1 + t * (x2 - x1);
        int hits = 0, where = -1;
        for (int b = 0; b < 200; ++b)
            if (xmajor ? (b < 200 && pixel(m, a, b)) : (b < 320 && pixel(m, b, a))) { ++hits; where = b; }
        if (hits != 1 || std::fabs(where - ideal) > 0.5 + 1e-9) all_ok = false;
        if (a == a1) break;
    }
    CHECK(all_ok);
    CHECK(pixel(m, x1, y1));
    CHECK(pixel(m, x2, y2));
}

void test_line_mode() {
    const int lines[][4] = {{2, 3, 50, 20},  {50, 20, 2, 3},   {10, 10, 14, 60}, {14, 60, 10, 10},
                            {100, 5, 30, 40}, {30, 40, 100, 5}, {7, 7, 7, 30},    {20, 9, 90, 9}, {40, 40, 40, 40}};
    for (auto& l : lines) {
        Machine m;
        uint32_t blits = m.blits;
        draw_line(m, l[0], l[1], l[2], l[3]);
        CHECK(m.blits == blits + 1 && m.line_blits == 1);
        check_line(m, l[0], l[1], l[2], l[3]);
        int major = std::max(std::abs(l[2] - l[0]), std::abs(l[3] - l[1]));
        CHECK(count_pixels(m) == major + 1);
        CHECK(m.intreq & reg::INTF_BLIT);
    }
    { // XOR twice clears; BZERO reflects the second pass
        Machine m;
        draw_line(m, 3, 4, 60, 33, 0x4A);
        CHECK(count_pixels(m) == 58);
        draw_line(m, 3, 4, 60, 33, 0x4A);
        CHECK(count_pixels(m) == 0);
    }
    { // SING: one pixel per row (for blitter fill outlines)
        Machine m;
        draw_line(m, 2, 3, 50, 20, 0xCA, true);
        CHECK(count_pixels(m) == 18);
        for (int y = 3; y <= 20; ++y) {
            int n = 0;
            for (int x = 0; x < 64; ++x) n += pixel(m, x, y);
            CHECK(n == 1);
        }
    }
    { // texture: BLTBDAT = $F0F0 with BSH = 15 draws it MSB first along a horizontal line
        Machine m;
        draw_line(m, 0, 0, 0, 0); // set up the constant registers (single dot at 0,0)
        m.write8(kBitmap, 0);
        m.write16(0xDFF042, uint16_t(0xF000 | (1 << 4) | 1 | 0x40));
        m.write16(0xDFF052, uint16_t(-2 * 31));
        m.write16(0xDFF064, uint16_t(-4 * 31));
        m.write16(0xDFF062, 0);
        m.write16(0xDFF072, 0xF0F0);
        m.write16(0xDFF040, 0x0BCA);
        m.write32(0xDFF048, kBitmap);
        m.write32(0xDFF054, kBitmap);
        m.write16(0xDFF058, uint16_t((32 << 6) | 2));
        CHECK(m.read16(kBitmap) == 0xF0F0 && m.read16(kBitmap + 2) == 0xF0F0);
    }
}

// ---- pacing ----

void test_pacer() {
    { // 60 Hz host for 10 minutes: exactly 30000 frames, at most 1 per refresh except the 6:5 pattern
        FramePacer p;
        p.reset(0);
        uint64_t total = 0;
        uint32_t max_burst = 0;
        for (uint64_t i = 1; i <= 60ull * 600; ++i) {
            uint64_t now = i * 1'000'000'000ull / 60;
            uint32_t n = p.frames_due(now);
            total += n;
            max_burst = std::max(max_burst, n);
        }
        CHECK(total == 30000);
        CHECK(max_burst == 1);
        CHECK(p.rebases() == 0);
    }
    { // jittery 144 Hz host
        FramePacer p;
        p.reset(5);
        uint64_t total = 0, now = 5;
        for (int i = 0; i < 144 * 600; ++i) {
            now += 1'000'000'000ull / 144 + uint64_t((i * 7919) % 2'000'000) - 1'000'000;
            total += p.frames_due(now);
        }
        CHECK(total == (now - 5) / 20'000'000);
        CHECK(p.rebases() == 0);
    }
    { // a 1 s stall rebases instead of running 50 frames at once
        FramePacer p;
        p.reset(0);
        CHECK(p.frames_due(20'000'000) == 1);
        CHECK(p.frames_due(1'020'000'000) == 1);
        CHECK(p.rebases() == 1);
        CHECK(p.frames_due(1'030'000'000) == 0);
        CHECK(p.frames_due(1'040'000'000) == 1);
    }
}

void test_image_writers() {
    std::vector<uint8_t> rgba(4 * 3 * 2);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(i * 10);
    auto dir = std::filesystem::temp_directory_path();
    std::string png = (dir / "x2_video_test.png").string(), ppm = (dir / "x2_video_test.ppm").string();
    CHECK(write_image(png, rgba, 3, 2));
    CHECK(write_image(ppm, rgba, 3, 2));
    FILE* f = std::fopen(png.c_str(), "rb");
    unsigned char sig[8] = {};
    if (f) { CHECK(std::fread(sig, 1, 8, f) == 8); std::fclose(f); }
    CHECK(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
    CHECK(std::filesystem::file_size(ppm) == std::string("P6\n3 2\n255\n").size() + 18);
    std::filesystem::remove(png);
    std::filesystem::remove(ppm);
}

} // namespace

int main() {
    struct T { const char* name; void (*fn)(); } tests[] = {
        {"palette_expansion", test_palette_expansion},
        {"copper_palette", test_copper_palette},
        {"bitplanes_and_scroll", test_bitplanes_and_scroll},
        {"window_border", test_window_border},
        {"dual_playfield", test_dual_playfield},
        {"ham_and_ehb", test_ham_and_ehb},
        {"sprites", test_sprites},
        {"manual_sprite", test_manual_sprite},
        {"line_mode", test_line_mode},
        {"pacer", test_pacer},
        {"image_writers", test_image_writers},
    };
    for (auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-22s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
