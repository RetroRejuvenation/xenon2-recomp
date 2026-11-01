// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/interp.h"

#include "machine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace x2::modern {

namespace {

int wrap192(int d) {
    d %= 192;
    if (d >= 96) d -= 192;
    if (d < -96) d += 192;
    return d;
}

float star_line(const Step& s, int j) {
    return float(s.stars[size_t(3 * j)] / kLineBytes) + float(s.stars[size_t(3 * j + 2)]) / 384.0f;
}

int signed_line(const Step& s, uint32_t addr) {
    int64_t off = int64_t(addr) - int64_t(s.draw_buf);
    return int(std::floor(double(off) / kLineBytes));
}

// The blitter's logic function on 16 pixels at once (minterm bit index = A << 2 | B << 1 | C).
uint16_t minterm(uint8_t mt, uint16_t a, uint16_t b, uint16_t c) {
    uint16_t r = 0;
    for (int k = 0; k < 8; ++k)
        if ((mt >> k) & 1) r |= uint16_t((k & 4 ? a : ~a) & (k & 2 ? b : ~b) & (k & 1 ? c : ~c));
    return r;
}

// Playfield (lines 0-191) entirely colour 0: the game cleared the screen.
bool blank(const Step& s) {
    if (s.image.empty()) return true;
    const uint8_t* p = &s.image[size_t(kGutterLines) * kLineBytes];
    return std::all_of(p, p + size_t(kPlayfieldLines) * kLineBytes, [](uint8_t v) { return v == 0; });
}

uint32_t rgba_of(uint16_t c) {
    return uint32_t(((c >> 8) & 15) * 17) | (uint32_t(((c >> 4) & 15) * 17) << 8) | (uint32_t((c & 15) * 17) << 16) |
           0xFF000000u;
}

// Premultiplied src over dst.
uint32_t over(uint32_t src, uint32_t dst) {
    const uint32_t k = 255 - (src >> 24);
    if (!k) return src;
    uint32_t r = 0;
    for (int c = 0; c < 32; c += 8) r |= std::min<uint32_t>(255, ((src >> c) & 255) + (((dst >> c) & 255) * k + 127) / 255) << c;
    return r;
}

// dst moved toward src by a/255 (both opaque).
uint32_t mix(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t r = 0xFF000000u;
    for (int c = 0; c < 24; c += 8) {
        const int d = int((dst >> c) & 255), t = int((src >> c) & 255);
        r |= uint32_t(d + ((t - d) * int(a) + (t >= d ? 127 : -127)) / 255) << c;
    }
    return r;
}

// Surround brightness at canvas distance i from the playfield edge (of ws): 256 = unchanged.
uint32_t darken(uint32_t c, int i, int ws) {
    const uint32_t g = uint32_t(176 - 96 * i / std::max(1, ws));
    return (((c & 255) * g) >> 8) | ((((c >> 8) & 255) * g) >> 8) << 8 | ((((c >> 16) & 255) * g) >> 8) << 16 | 0xFF000000u;
}

} // namespace

Layer layer_of(const BlitCmd& b, const CaptureLayout& L) {
    // Large objects are drawn with the tile routine from their draw handler: the object comes first.
    if (b.obj) return Layer::Object;
    if (L.far_pc(b.pc)) return Layer::Far;
    if (L.tile_pc(b.pc)) return Layer::Tiles;
    return Layer::Other;
}

bool pack_changes_gameplay(const pack::PackLibrary* lib) {
    if (!lib) return false;
    if (const pack::StarsEntry* st = lib->stars(); st && st->shape == pack::StarsEntry::Shape::Round) return true;
    for (int level = 1; level <= 5; ++level)
        if (lib->background(level)) return true;
    return false;
}

void Interpolator::note_once(const void* what, const std::string& text) {
    if (std::find(noted_.begin(), noted_.end(), what) != noted_.end()) return;
    noted_.push_back(what);
    std::fprintf(stderr, "asset pack: %s\n", text.c_str());
}

const pack::BackgroundEntry* Interpolator::background_for(const Step& s) {
    if (!opt.packs || s.layout->far_ring_lines <= 0 || s.level < 1 || s.level > 5) return nullptr;
    const pack::BackgroundEntry* b = opt.packs->background(s.level);
    if (!b || !b->pic || b->pic->empty()) return nullptr;
    const int lines = s.layout->far_ring_lines;
    if ((b->ring_lines != 0 && b->ring_lines != lines) || b->scale < 1 || b->pic->w != kWidth * b->scale || b->pic->h != lines * b->scale) {
        note_once(b, "backgrounds, level " + std::to_string(b->level) + ": the picture must be " + std::to_string(kWidth) + " x " +
                         std::to_string(lines) + " Amiga pixels (the far layer's ring is " + std::to_string(lines) + " lines) times its scale; ignored");
        return nullptr;
    }
    return b;
}

void Interpolator::learn_tie(const Step& s, const Graphic& g) {
    if (!opt.packs || !g.cookie || !g.shape || g.first >= s.blits.size()) return;
    if (ties_.size() > 4096) ties_.clear();
    if (shape_ties_.size() > 4096) shape_ties_.clear();
    const Tie t{g.hash, g.w, g.h};
    ties_[{s.blits[g.first].obj, g.shape}] = t;
    shape_ties_[g.shape] = t;
}

bool Interpolator::flash_replacement(const Step& s, const Graphic& g, Replacement& out) {
    if (!opt.packs || g.first >= s.blits.size()) return false;
    // An explicit picture for this mask shape ("f:" key) wins; then the graphic's own flash_png; then its picture whitened.
    if (const pack::GraphicEntry* e = opt.packs->flash(g.shape, g.w, g.h); e && e->pic && !e->pic->empty()) {
        out = Replacement{e->pic, e->scale, e->anchor.x, e->anchor.y};
        return true;
    }
    const Tie* t = nullptr;
    if (auto it = ties_.find({s.blits[g.first].obj, g.shape}); it != ties_.end()) t = &it->second;
    else if (auto sh = shape_ties_.find(g.shape); sh != shape_ties_.end()) t = &sh->second;
    if (!t) return false;
    if (const pack::GraphicEntry* e = opt.packs->graphic(t->hash, t->w, t->h); e && e->flash && !e->flash->empty()) {
        const int fs = std::max(1, e->flash_scale);
        out = Replacement{e->flash, fs, e->anchor.x * fs / std::max(1, e->scale), e->anchor.y * fs / std::max(1, e->scale)};
        return true;
    }
    const Replacement* r = opt.assets && !opt.assets->empty() ? opt.assets->graphic(t->hash, t->w, t->h) : nullptr;
    if (!r || !r->img || r->img->empty()) return false;
    auto it = derived_flash_.find(r->img);
    if (it == derived_flash_.end()) {
        Image w = *r->img;
        // 70 % toward white: premultiplied, white of alpha a is (a, a, a, a).
        for (uint32_t& px : w.px) {
            const uint32_t a = px >> 24;
            uint32_t v = a << 24;
            for (int c = 0; c < 24; c += 8) v |= ((((px >> c) & 255) * 3 + a * 7 + 5) / 10) << c;
            px = v;
        }
        it = derived_flash_.emplace(r->img, std::move(w)).first;
    }
    out = Replacement{&it->second, r->scale, r->ax, r->ay};
    return true;
}

// Persistent workers for the bands of one render; the calling thread takes bands too.
struct Interpolator::Pool {
    explicit Pool(int workers) {
        for (int i = 0; i < workers; ++i) threads.emplace_back([this] { loop(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> l(mu);
            stop = true;
        }
        wake.notify_all();
        for (std::thread& t : threads) t.join();
    }
    void run(int n, const std::function<void(int)>& f) {
        {
            std::unique_lock<std::mutex> l(mu);
            idle.wait(l, [&] { return active == 0; });
            fn = &f;
            jobs = n;
            next = 0;
            done = 0;
            ++gen;
        }
        wake.notify_all();
        work();
        std::unique_lock<std::mutex> l(mu);
        idle.wait(l, [&] { return done == jobs && active == 0; });
    }
    void work() {
        for (int j; (j = next.fetch_add(1)) < jobs;) {
            (*fn)(j);
            if (done.fetch_add(1) + 1 == jobs) {
                std::lock_guard<std::mutex> l(mu);
                idle.notify_all();
            }
        }
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(mu);
                wake.wait(l, [&] { return stop || gen != seen; });
                if (stop) return;
                seen = gen;
                ++active;
            }
            work();
            std::lock_guard<std::mutex> l(mu);
            --active;
            idle.notify_all();
        }
    }
    std::vector<std::thread> threads;
    std::mutex mu;
    std::condition_variable wake, idle;
    const std::function<void(int)>* fn = nullptr;
    int jobs = 0, active = 0;
    std::atomic<int> next{0}, done{0};
    uint64_t gen = 0;
    bool stop = false;
};

Interpolator::Interpolator() = default;
Interpolator::~Interpolator() = default;

bool Interpolator::gameplay(const Step& s) {
    if (s.image.empty() || s.partial) return false;
    for (const BlitCmd& b : s.blits)
        if (b.pc == s.layout->far_copy_a) return true;
    return false;
}

PairMotion Interpolator::analyse(const Step* z, const Step& a, const Step& b) const {
    PairMotion mo;
    if (!gameplay(a) || !gameplay(b)) { mo.why = "not a gameplay step"; return mo; }
    if (b.index != a.index + 1) { mo.why = "steps not consecutive"; return mo; }
    uint32_t frames = b.swap_frame - a.swap_frame;
    if (frames < 2 || frames > 8) { mo.why = "step interval out of range"; return mo; }
    if (blank(a) || blank(b)) { mo.why = "screen clear"; return mo; }

    // Far layer: content moves down by the decrease of the ring row.
    mo.far_step = wrap192(int(a.vars[kRing]) - int(b.vars[kRing]));
    if (std::abs(mo.far_step) > 4) { mo.why = "far layer jump"; return mo; }
    mo.far_from = 0;
    mo.far_to = float(mo.far_step);
    if (opt.smooth_far_layer && z && gameplay(*z) && z->index + 1 == a.index) {
        int prev = wrap192(int(z->vars[kRing]) - int(a.vars[kRing]));
        if (std::abs(prev) <= 4) {
            // Positions relative to a: z at -prev, a at 0, b at far_step; show the two-step moving average.
            mo.far_from = -0.5f * float(prev);
            mo.far_to = 0.5f * float(mo.far_step);
        }
    }

    // Foreground tiles: the most common dy between tile blits with the same source, column and plane.
    std::map<std::array<uint32_t, 4>, std::vector<int>> prev;
    for (const BlitCmd& t : a.blits)
        if (a.layout->tile_pc(t.pc) && !t.obj) prev[{t.apt, t.bpt, uint32_t(t.x), uint32_t(t.plane)}].push_back(signed_line(a, t.dpt));
    std::map<int, int> votes;
    for (const BlitCmd& t : b.blits) {
        if (!b.layout->tile_pc(t.pc) || t.obj) continue;
        auto it = prev.find({t.apt, t.bpt, uint32_t(t.x), uint32_t(t.plane)});
        if (it == prev.end()) continue;
        int y = signed_line(b, t.dpt), best = 1 << 20;
        for (int py : it->second)
            if (std::abs(y - py) < std::abs(best)) best = y - py;
        ++votes[best];
    }
    if (!votes.empty())
        mo.fg_dy = std::max_element(votes.begin(), votes.end(), [](auto& p, auto& q) { return p.second < q.second; })->first;
    else
        mo.fg_dy = int16_t(a.vars[kScrollCD0] - b.vars[kScrollCD0]);
    if (std::abs(mo.fg_dy) > 8) { mo.why = "foreground scroll jump"; return mo; }

    // Objects: same record and update handler in both steps, with a plausible move. A record that jumps is a
    // cut for that object only (it is reused for a new object, or warped); when most objects jump at once the
    // whole scene was cut.
    std::unordered_map<uint32_t, const ObjectSnap*> pa;
    for (const ObjectSnap& o : a.objects) pa[o.addr] = &o;
    int matched = 0, jumped = 0;
    for (const ObjectSnap& o : b.objects) {
        auto it = pa.find(o.addr);
        if (it == pa.end() || it->second->update != o.update) continue;
        ++matched;
        int dx = o.x - it->second->x, dy = o.y - it->second->y;
        if (std::abs(dx) > opt.max_object_jump || std::abs(dy) > opt.max_object_jump) { ++jumped; continue; }
        mo.objects[o.addr] = {float(dx), float(dy)};
    }
    if (matched >= 4 && 2 * jumped > matched) { mo.why = "objects jump"; mo.objects.clear(); return mo; }

    // Stars: continuous line position, wrapping at line 192.
    for (int j = 0; j < kStars; ++j) {
        float d = star_line(b, j) - star_line(a, j);
        if (d > 96) d -= 192;
        if (d < -96) d += 192;
        mo.star_dy[size_t(j)] = d;
        mo.star_ok[size_t(j)] = std::fabs(d) < 8 && b.stars[size_t(3 * j + 1)] && a.stars[size_t(3 * j + 1)];
    }
    mo.ok = true;
    return mo;
}

void Interpolator::replay_blit(const Step& s, const BlitCmd& b, int ox, int oy, bool clip, uint8_t layer, bool keep, int y0,
                               int y1) {
    if (b.data == ~0u || !(b.con0 & 0x100)) return; // line / fill mode (not used in gameplay), or no D
    const int S = opt.scale;
    const bool desc = b.con1 & 2;
    const int inc = desc ? -2 : 2;
    int dmod = b.dmod & ~1;
    if (desc) dmod = -dmod;
    // A cookie-cut reads C from the destination it writes, so it is evaluated against the canvas.
    const bool cookie = (b.con0 & 0x200) && b.cpt == b.dpt && b.cmod == b.dmod;
    const uint8_t mt = uint8_t(b.con0);
    const int64_t base = int64_t(s.draw_buf) - int64_t(kGutterLines) * kLineBytes;
    const int64_t bytes = int64_t(kGutterLines + kLines) * kLineBytes;
    const int ylimit = std::min(y1, clip ? (kGutterLines + kPlayfieldLines) * S : h_);
    const uint16_t* wd = &s.words[b.data];
    uint32_t dpt = b.dpt;
    for (int y = 0; y < b.h; ++y, dpt += uint32_t(dmod)) {
        for (int x = 0; x < b.w; ++x, wd += 3, dpt += uint32_t(inc)) {
            const int64_t off = int64_t(dpt & s.chip_mask) - base;
            if (off < 0 || off >= bytes) continue;
            const int Y0 = int(off / kLineBytes) * S + oy;
            const int ya = std::max(Y0, y0), yb = std::min(Y0 + S, ylimit);
            if (ya >= yb) continue;
            // Per pixel: D = r0 where the destination bit is 0, r1 where it is 1.
            const uint16_t r0 = minterm(mt, wd[0], wd[1], cookie ? 0 : wd[2]);
            const uint16_t r1 = cookie ? minterm(mt, wd[0], wd[1], 0xFFFF) : r0;
            const uint16_t set = r0 & r1, clr = uint16_t(~r0 & ~r1), tog = uint16_t(r0 & ~r1);
            if (!(set | clr | tog)) continue;
            const uint8_t pbit = uint8_t(1 << (off % kLineBytes / kRowBytes));
            const int px0 = int(off % kRowBytes) * 8;
            for (int i = 0; i < 16; ++i) {
                const uint16_t m = uint16_t(0x8000 >> i);
                if (!((set | clr | tog) & m)) continue;
                const int X0 = (px0 + i) * S + ox, xa = std::max(X0, 0), xb = std::min(X0 + S, w_);
                if (xa >= xb) continue;
                for (int Y = ya; Y < yb; ++Y) {
                    uint8_t* row = &canvas_[size_t(Y) * size_t(w_)];
                    if (set & m) for (int X = xa; X < xb; ++X) row[X] = uint8_t(((row[X] | pbit) & 15) | layer);
                    else if (clr & m) for (int X = xa; X < xb; ++X) row[X] = uint8_t((row[X] & 15 & ~pbit) | layer);
                    else for (int X = xa; X < xb; ++X) row[X] = uint8_t(((row[X] ^ pbit) & 15) | layer);
                    // A drawn pixel covers any replacement picture there (unless the blit is the replaced graphic).
                    if (over_on_ && !keep) std::fill(&over_[size_t(Y) * size_t(w_) + size_t(xa)], &over_[size_t(Y) * size_t(w_) + size_t(xb)], 0u);
                }
            }
        }
    }
}

void Interpolator::replay_write(const Step& s, const CpuWrite& w, int ox, int oy, bool clip, bool star, uint8_t layer, int y0, int y1) {
    const int S = opt.scale;
    const int64_t off = int64_t(w.addr) - (int64_t(s.draw_buf) - int64_t(kGutterLines) * kLineBytes);
    if (off < 0 || off >= int64_t(kGutterLines + kLines) * kLineBytes) return;
    const int bits = w.size == 1 ? 8 : 16;
    const uint32_t set = w.value & ~w.old, clr = w.old & ~w.value;
    if (!set && !clr) return;
    const int Y0 = int(off / kLineBytes) * S + oy;
    const int ya = std::max(Y0, y0), yb = std::min({Y0 + S, y1, clip ? (kGutterLines + kPlayfieldLines) * S : h_});
    if (ya >= yb) return;
    const int px0 = int(off % kRowBytes) * 8;
    const uint8_t pbit = uint8_t(1 << (off % kLineBytes / kRowBytes)), allowed = star ? s.layout->star_planes(w.pc) : 0xF;
    for (int i = 0; i < bits; ++i) {
        const uint32_t m = 1u << (bits - 1 - i);
        if (!((set | clr) & m)) continue;
        const int X0 = (px0 + i) * S + ox, xa = std::max(X0, 0), xb = std::min(X0 + S, w_);
        // Round stars: remember the block and what was under it (the disc replaces the block after the replay).
        size_t prev = SIZE_MAX;
        if (star && round_stars_ && (set & m)) {
            for (const StarBlock& b : star_blocks_)
                if (b.x == X0 && b.y == Y0) { prev = b.prev; break; }
            if (prev == SIZE_MAX) {
                prev = star_prev_.size();
                star_prev_.resize(prev + size_t(S) * size_t(S), 0xFF);
                star_blocks_.push_back({X0, Y0, prev});
            }
        }
        for (int Y = ya; Y < yb; ++Y) {
            uint8_t* row = &canvas_[size_t(Y) * size_t(w_)];
            for (int X = xa; X < xb; ++X) {
                uint8_t& p = row[X];
                if (set & m) {
                    // A star is only drawn on empty background ($233A tests all four planes first). Where the layout says the
                    // captured writes are exact (CaptureLayout::star_writes_exact), the game already did that test for a write
                    // at its own position, so the rule only matters once interpolation has moved the star.
                    if (star && (p & 15 & ~allowed) && !(s.layout->star_writes_exact && !(ox | oy))) continue;
                    if (prev != SIZE_MAX && (p >> 4) != int(Layer::Stars)) star_prev_[prev + size_t(Y - Y0) * size_t(S) + size_t(X - X0)] = p;
                    p = uint8_t(((p | pbit) & 15) | layer);
                } else {
                    p = uint8_t((p & 15 & ~pbit) | layer);
                }
                if (over_on_) over_[size_t(Y) * size_t(w_) + size_t(X)] = 0;
            }
        }
    }
}

void Interpolator::build(const Step& s, const PairMotion& mo, float f, bool from_a) {
    const int S = opt.scale;
    // Motion of the far layer at this t, relative to this step's own position.
    const float t = from_a ? f : 1.0f + f;
    const float far = mo.far_from + (mo.far_to - mo.far_from) * t - (from_a ? 0.0f : float(mo.far_step));
    auto px = [&](float d) { return int(std::lround(d * float(S))); };

    // Star index by the plane-0 word offset it draws at.
    std::unordered_map<uint32_t, int> star_at;
    for (int j = 0; j < kStars; ++j)
        if (s.stars[size_t(3 * j + 1)]) star_at[uint32_t(s.stars[size_t(3 * j)])] = j;

    size_t bi = 0, wi = 0;
    while (bi < s.blits.size() || wi < s.writes.size()) {
        bool blit = wi >= s.writes.size() || (bi < s.blits.size() && s.blits[bi].seq < s.writes[wi].seq);
        float dx = 0, dy = 0;
        if (blit) {
            const BlitCmd& b = s.blits[bi];
            switch (layer_of(b, *s.layout)) {
            case Layer::Object:
                if (auto it = mo.objects.find(b.obj); it != mo.objects.end()) { dx = f * it->second[0]; dy = f * it->second[1]; }
                break;
            case Layer::Far: dy = far; break;
            case Layer::Tiles: dy = f * float(mo.fg_dy); break;
            default: break;
            }
            cmds_.push_back({true, false, uint8_t(int(layer_of(b, *s.layout)) << 4), px(dx), px(dy), uint32_t(bi++)});
        } else {
            const CpuWrite& w = s.writes[wi];
            bool star = s.layout->star_pc(w.pc);
            if (star && w.buffer == 0) {
                uint32_t off = w.addr - s.draw_buf;
                for (uint32_t back : {0u, 0x28u, 0x50u}) {
                    auto it = star_at.find(off - back);
                    if (it != star_at.end() && (s.stars[size_t(3 * it->second + 1)] & (w.value & ~w.old))) {
                        if (mo.star_ok[size_t(it->second)]) dy = f * mo.star_dy[size_t(it->second)];
                        break;
                    }
                }
            }
            cmds_.push_back({false, star, uint8_t(int(star ? Layer::Stars : Layer::Other) << 4), 0, px(dy), uint32_t(wi++)});
        }
    }
}

void Interpolator::base(const Step& s) {
    const int S = opt.scale;
    const uint64_t key = (uint64_t(s.index) << 32) ^ (uint64_t(s.swap_frame) << 8) ^ uint64_t(S) ^ (uint64_t(s.draw_buf) << 40);
    if (key == base_key_ && base_.size() == size_t(w_) * size_t(h_)) return;
    base_key_ = key;
    base_.resize(size_t(w_) * size_t(h_));
    std::vector<uint8_t> line(kWidth);
    for (int ay = 0; ay < kGutterLines + kLines; ++ay) {
        const uint8_t* src = &s.image[size_t(ay) * kLineBytes];
        for (int ax = 0; ax < kWidth; ++ax) {
            uint8_t v = 0;
            for (int p = 0; p < kPlanes; ++p) v |= uint8_t(((src[p * kRowBytes + ax / 8] >> (7 - ax % 8)) & 1) << p);
            line[size_t(ax)] = v;
        }
        uint8_t* dst = &base_[size_t(ay * S) * size_t(w_)];
        for (int ax = 0; ax < kWidth; ++ax) std::memset(dst + ax * S, line[size_t(ax)], size_t(S));
        for (int sy = 1; sy < S; ++sy) std::memcpy(dst + size_t(sy) * size_t(w_), dst, size_t(w_));
    }
}

void Interpolator::band(const Step& s, int y0, int y1, const uint32_t* pal, uint32_t* out, size_t stride) {
    // Start from the step's finished picture: lines the moved playfield does not cover keep it.
    std::memcpy(&canvas_[size_t(y0) * size_t(w_)], &base_[size_t(y0) * size_t(w_)], size_t(y1 - y0) * size_t(w_));
    if (over_on_) std::fill(&over_[size_t(y0) * size_t(w_)], &over_[size_t(y1) * size_t(w_)], 0u);
    const Plan* pl = plan_;
    size_t pi = 0;
    bool snapped = opt.wide <= 0;
    for (const Cmd& c : cmds_) {
        const bool moved = c.ox || c.oy;
        if (!snapped && !(c.blit && (c.layer >> 4 == int(Layer::Far) || c.layer >> 4 == int(Layer::Tiles)))) {
            snapshot(y0, y1); // the background before the first star or object
            snapped = true;
        }
        if (c.blit) {
            // Replacements composited before this blit (replaced cookie-cut graphics), the blit, those after it.
            if (pl)
                for (; pi < pl->paints.size() && pl->paints[pi].at <= c.index && pl->paints[pi].before; ++pi)
                    if (pl->paints[pi].at == c.index) paint(pl->paints[pi], c.ox, c.oy, moved, y0, y1, pal);
            replay_blit(s, s.blits[c.index], c.ox, c.oy, moved, c.layer, pl && pl->keep[c.index], y0, y1);
            if (pl)
                for (; pi < pl->paints.size() && pl->paints[pi].at <= c.index; ++pi)
                    if (pl->paints[pi].at == c.index) paint(pl->paints[pi], c.ox, c.oy, moved, y0, y1, pal);
        } else {
            replay_write(s, s.writes[c.index], c.ox, c.oy, moved, c.star, c.layer, y0, y1);
        }
    }
    if (!snapped) snapshot(y0, y1);
    if (round_stars_) star_discs(pal, y0, y1);
    const int top = kGutterLines * opt.scale;
    for (int Y = y0; Y < y1; ++Y) {
        const uint8_t* src = &canvas_[size_t(Y) * size_t(w_)];
        uint32_t* dst = out + size_t(Y - top) * stride;
        if (!over_on_) {
            for (int X = 0; X < w_; ++X) dst[X] = pal[src[X]];
            continue;
        }
        const uint32_t* ov = &over_[size_t(Y) * size_t(w_)];
        for (int X = 0; X < w_; ++X) dst[X] = ov[X] ? over(ov[X], pal[src[X]]) : pal[src[X]];
    }
    if (opt.wide > 0) sides(pal, out, stride, y0, y1);
}

const Image* Interpolator::scaled(const Image* img, int k) {
    if (k == opt.scale) return img;
    auto key = std::make_pair(img, opt.scale);
    auto it = scaled_.find(key);
    if (it == scaled_.end()) {
        it = scaled_.emplace(key, Image{}).first;
        resample(*img, opt.scale, k, it->second);
    }
    return &it->second;
}

const Interpolator::Plan* Interpolator::plan(const Step& s) {
    const int S = opt.scale;
    if (plans_for_ != opt.assets || plans_packs_ != opt.packs) { // another library: forget everything derived from the old one
        for (Plan& p : plans_) p.key = ~0ull;
        scaled_.clear();
        derived_flash_.clear();
        ties_.clear();
        shape_ties_.clear();
        sources_.clear();
        plans_for_ = opt.assets;
        plans_packs_ = opt.packs;
    }
    const AssetLibrary* lib = opt.assets && !opt.assets->empty() ? opt.assets : nullptr;
    const uint64_t key = (uint64_t(s.index) << 32) ^ (uint64_t(s.swap_frame) << 8) ^ uint64_t(S) ^ (uint64_t(s.draw_buf) << 40);
    for (Plan& p : plans_)
        if (p.key == key) return &p;
    Plan& p = plans_[plan_next_++ & 3];
    p.key = key;
    p.keep.assign(s.blits.size(), 0);
    p.paints.clear();
    p.mask.clear();
    std::vector<Graphic> gs;
    find_graphics(s, gs, true);
    int last_far = -1;
    for (size_t i = 0; i < s.blits.size(); ++i)
        if (s.layout->far_pc(s.blits[i].pc)) last_far = int(i);
    const pack::BackgroundEntry* bg = background_for(s);
    if (bg) ++stats.background_plans;
    for (Graphic& g : gs) {
        if (g.kind == GfxKind::Far) {
            // Composited after the last far-layer command (the clear of planes 2-3 at $1884).
            if (last_far < 0) continue;
            if (bg) {
                // The level's whole far layer: screen line y shows picture row (ring + y) mod ring_lines (a level's `backgrounds`
                // picture replaces its `far` lines). The lines just above the playfield are painted too, for the sub-line moves
                // of the layer's interpolation (the gutter is not shown while the layer is unmoved).
                const int lines = s.layout->far_ring_lines;
                const Image* im = scaled(bg->pic, bg->scale);
                for (int k = g.y == 0 ? 4 : 0; k >= 0; --k) {
                    const int y = g.y - k, row = ((int(s.vars[kRing]) + y) % lines + lines) % lines;
                    p.paints.push_back({uint32_t(last_far), false, im, 0, (y + kGutterLines) * S, row * S, S});
                }
                continue;
            }
            const FarRow* r = lib ? lib->far_line(g.hash) : nullptr;
            if (r) p.paints.push_back({uint32_t(last_far), false, scaled(r->img, r->scale), 0, (g.y + kGutterLines) * S, r->row * S, S});
            continue;
        }
        Replacement flash;
        const Replacement* r = nullptr;
        if (g.kind == GfxKind::Flash) {
            if (!flash_replacement(s, g, flash)) continue;
            ++stats.flashes_replaced;
            r = &flash;
        } else {
            if (g.kind == GfxKind::Sprite) learn_tie(s, g);
            r = lib ? lib->graphic(g.hash, g.w, g.h) : nullptr;
        }
        const SourceTracker::Full* full = g.kind == GfxKind::Flash || !lib ? nullptr : sources_.fuller(g, s.palette);
        int gx = g.x, gy = g.y, cy0 = -(1 << 30), cy1 = 1 << 30;
        if (!r && full && (r = lib->graphic(full->hash, full->w, full->h))) {
            // A bottom-clipped draw: the full graphic's replacement, on the rows drawn.
            gx = g.ox + full->dx;
            gy = g.oline + full->dy;
            cy0 = (g.oline + kGutterLines) * S;
            cy1 = (g.oline + g.rows + kGutterLines) * S;
        }
        if (!r) continue;
        const Image* im = scaled(r->img, r->scale);
        const int x = gx * S - int(std::lround(double(r->ax) * S / r->scale));
        const int y = (gy + kGutterLines) * S - int(std::lround(double(r->ay) * S / r->scale));
        // An opaque copy is composited over its own replay; a cookie-cut graphic over what is underneath it,
        // before its blits (which then replay the original into the indices without removing the picture).
        Paint pt{g.cookie ? g.first : g.last, g.cookie, im, x, y, 0, im->h, cy0, cy1};
        if (g.cookie) {
            for (uint32_t i = g.first; i <= g.last; ++i) p.keep[i] = 1;
            pt.mask = int(p.mask.size());
            pt.gx = g.x;
            pt.gy = g.y + kGutterLines;
            pt.gw = g.w;
            pt.gh = g.h;
            p.mask.push_back(std::move(g.pix));
        }
        p.paints.push_back(pt);
    }
    std::stable_sort(p.paints.begin(), p.paints.end(),
                     [](const Paint& a, const Paint& b) { return a.at != b.at ? a.at < b.at : a.before > b.before; });
    return &p;
}

void Interpolator::paint(const Paint& p, int ox, int oy, bool clip, int y0, int y1, const uint32_t* pal) {
    const int S = opt.scale, X0 = p.x + ox, Y0 = p.y + oy;
    const int ylimit = std::min(y1, clip ? (kGutterLines + kPlayfieldLines) * S : h_);
    if (p.mask < 0) { // over the replayed original
        const int ya = std::max({Y0, y0, p.cy0 + oy}), yb = std::min({Y0 + p.sh, ylimit, p.cy1 + oy});
        const int xa = std::max(X0, 0), xb = std::min(X0 + p.img->w, w_);
        for (int Y = ya; Y < yb; ++Y) {
            const uint32_t* src = &p.img->px[size_t(p.sy + Y - Y0) * size_t(p.img->w)];
            uint32_t* d = &over_[size_t(Y) * size_t(w_)];
            for (int X = xa; X < xb; ++X)
                if (const uint32_t c = src[X - X0]) d[X] = d[X] ? over(c, d[X]) : c;
        }
        return;
    }
    // Over what is underneath, made opaque, on the picture and on the original's footprint (where the original
    // will be replayed into the indices: the picture's transparent parts must show the background there).
    const int GX = p.gx * S + ox, GY = p.gy * S + oy;
    const std::vector<uint8_t>& mask = plan_->mask[size_t(p.mask)];
    const int ya = std::max({std::min(Y0, GY), y0, p.cy0 + oy}), yb = std::min({std::max(Y0 + p.sh, GY + p.gh * S), ylimit, p.cy1 + oy});
    const int xa = std::max(std::min(X0, GX), 0), xb = std::min(std::max(X0 + p.img->w, GX + p.gw * S), w_);
    for (int Y = ya; Y < yb; ++Y) {
        const bool in_img = Y >= Y0 && Y < Y0 + p.sh, in_g = Y >= GY && Y < GY + p.gh * S;
        const uint32_t* src = in_img ? &p.img->px[size_t(p.sy + Y - Y0) * size_t(p.img->w)] : nullptr;
        const uint8_t* m = in_g ? &mask[size_t((Y - GY) / S) * size_t(p.gw)] : nullptr;
        uint32_t* d = &over_[size_t(Y) * size_t(w_)];
        const uint8_t* idx = &canvas_[size_t(Y) * size_t(w_)];
        for (int X = xa; X < xb; ++X) {
            const uint32_t c = src && X >= X0 && X < X0 + p.img->w ? src[X - X0] : 0;
            const bool foot = m && X >= GX && X < GX + p.gw * S && m[(X - GX) / S];
            if (!c && !foot) continue;
            const uint32_t under = d[X] ? over(d[X], pal[idx[X]]) : pal[idx[X]];
            d[X] = c ? over(c, under) : under;
        }
    }
}

void Interpolator::star_discs(const uint32_t* pal, int y0, int y1) {
    const int S = opt.scale, M = star_disc_.margin, T = star_disc_.side;
    // The stars the replay left visible (an object may have been drawn over a block since), with their colour.
    std::vector<std::pair<const StarBlock*, uint8_t>> live;
    for (const StarBlock& b : star_blocks_) {
        const int cx = b.x + S / 2, cy = b.y + S / 2;
        if (cx < 0 || cx >= w_ || cy < y0 || cy >= y1) continue;
        const uint8_t v = canvas_[size_t(cy) * size_t(w_) + size_t(cx)];
        if ((v >> 4) != int(Layer::Stars) || !(v & 15)) continue;
        live.push_back({&b, uint8_t(v & 15)});
    }
    // The square pixels of the original go; what was under them comes back.
    for (const auto& [b, colour] : live)
        for (int dy = 0; dy < S; ++dy) {
            const int Y = b->y + dy;
            if (Y < y0 || Y >= y1) continue;
            for (int dx = 0; dx < S; ++dx) {
                const int X = b->x + dx;
                if (X < 0 || X >= w_) continue;
                uint8_t& c = canvas_[size_t(Y) * size_t(w_) + size_t(X)];
                if ((c >> 4) == int(Layer::Stars)) {
                    const uint8_t pv = star_prev_[b->prev + size_t(dy) * size_t(S) + size_t(dx)];
                    c = pv == 0xFF ? uint8_t(0) : pv;
                }
            }
        }
    stats.round_stars += live.size();
    for (const auto& [b, colour] : live) {
        const uint32_t col = pal[int(Layer::Stars) << 4 | colour];
        for (int ty = 0; ty < T; ++ty) {
            const int Y = b->y + ty - M;
            if (Y < y0 || Y >= y1) continue;
            for (int tx = 0; tx < T; ++tx) {
                const int X = b->x + tx - M;
                const uint32_t a = star_disc_.alpha[size_t(ty) * size_t(T) + size_t(tx)];
                if (!a || X < 0 || X >= w_) continue;
                // Stars are behind tiles and objects.
                const int layer = canvas_[size_t(Y) * size_t(w_) + size_t(X)] >> 4;
                if (layer == int(Layer::Object) || layer == int(Layer::Tiles)) continue;
                uint32_t c = col;
                if (a != 255) {
                    c = a << 24;
                    for (int k = 0; k < 24; k += 8) c |= ((((col >> k) & 255) * a + 127) / 255) << k;
                }
                uint32_t& d = over_[size_t(Y) * size_t(w_) + size_t(X)];
                d = d ? over(c, d) : c;
            }
        }
    }
}

void Interpolator::snapshot(int y0, int y1) {
    const size_t SW = size_t(16 * opt.scale);
    for (int Y = y0; Y < y1; ++Y) {
        const size_t row = size_t(Y) * size_t(w_);
        for (int side = 0; side < 2; ++side) {
            const size_t from = row + (side ? size_t(w_) - SW : 0), to = (size_t(Y) * 2 + size_t(side)) * SW;
            std::memcpy(&snap_idx_[to], &canvas_[from], SW);
            if (over_on_) std::memcpy(&snap_ov_[to], &over_[from], SW * 4);
            else std::fill(&snap_ov_[to], &snap_ov_[to + SW], 0u);
        }
    }
}

void Interpolator::place_side_stars(const Step& s, const PairMotion& mo, float f) {
    const int S = opt.scale;
    // Each star's colour, from the writes that plotted it this step (star_planes of the plotting routine).
    std::unordered_map<uint32_t, uint8_t> colour_at;
    for (const CpuWrite& w : s.writes) {
        if (!s.layout->star_pc(w.pc) || w.buffer != 0) continue;
        const uint32_t off = w.addr - s.draw_buf;
        colour_at[off - off % kLineBytes / kRowBytes * kRowBytes] = s.layout->star_planes(w.pc);
    }
    // As many stars per side as the playfield has per 320 pixels, each a copy of one playfield star: same line
    // and speed, x spread over the side by a fixed permutation.
    const int n = std::clamp((kStars * opt.wide + kWidth / 2) / kWidth, 1, kStars);
    for (int side = 0; side < 2; ++side) {
        side_stars_[side].clear();
        for (int k = 0; k < n; ++k) {
            const int j = (k * 29 + side * 17) % kStars;
            const uint16_t mask = s.stars[size_t(3 * j + 1)], off = s.stars[size_t(3 * j)];
            if (!mask) continue;
            int bit = 15;
            while (bit > 0 && !(mask & (1u << bit))) --bit;
            const int x = int(off % kRowBytes) * 8 + (15 - bit), line = int(off / kLineBytes);
            const float dy = mo.ok && mo.star_ok[size_t(j)] ? f * mo.star_dy[size_t(j)] : 0.0f;
            const auto c = colour_at.find(off);
            const int xs = (x * 7 + k * 53 + side * 31) % opt.wide;
            side_stars_[side].push_back({xs * S, (line + kGutterLines) * S + int(std::lround(dy * float(S))),
                                         uint8_t(c != colour_at.end() ? c->second : 4 + j * 4 / kStars)});
        }
    }
}

void Interpolator::sides(const uint32_t* pal, uint32_t* out, size_t stride, int y0, int y1) {
    const int S = opt.scale, SW = 16 * S, WS = opt.wide * S, top = kGutterLines * S, bottom = (kGutterLines + kPlayfieldLines) * S;
    for (int Y = y0; Y < y1; ++Y) {
        uint32_t* dst = out + size_t(Y - top) * stride;
        if (Y >= bottom) { // the panel: black sides, the panel stays centred
            std::fill(dst - WS, dst, 0xFF000000u);
            std::fill(dst + w_, dst + w_ + WS, 0xFF000000u);
            continue;
        }
        for (int side = 0; side < 2; ++side) {
            const uint8_t* si = &snap_idx_[(size_t(Y) * 2 + size_t(side)) * size_t(SW)];
            const uint32_t* so = &snap_ov_[(size_t(Y) * 2 + size_t(side)) * size_t(SW)];
            // Column of the edge strip for distance i from the edge: mirrored back and forth.
            auto col = [&](int i) {
                const int p = i % (2 * SW), c = p < SW ? p : 2 * SW - 1 - p;
                return side ? SW - 1 - c : c;
            };
            auto at = [&](int i) -> uint32_t& { return side ? dst[w_ + i] : dst[-1 - i]; };
            for (int i = 0; i < WS; ++i) {
                const int c = col(i);
                at(i) = darken(so[c] ? over(so[c], pal[si[c]]) : pal[si[c]], i, WS);
            }
            for (const SideStar& st : side_stars_[side]) {
                if (round_stars_) {
                    const int M = star_disc_.margin, T = star_disc_.side;
                    if (Y < st.y - M || Y >= st.y + S + M) continue;
                    const uint8_t* arow = &star_disc_.alpha[size_t(Y - (st.y - M)) * size_t(T)];
                    for (int tx = 0; tx < T; ++tx) {
                        const int i = st.x - M + tx;
                        if (i < 0 || i >= WS || !arow[tx]) continue;
                        const int c = col(i);
                        if (!(si[c] & 15) && !so[c]) at(i) = mix(at(i), darken(pal[int(Layer::Stars) << 4 | st.colour], i, WS), arow[tx]);
                    }
                    continue;
                }
                if (Y < st.y || Y >= st.y + S) continue;
                for (int i = st.x; i < st.x + S && i < WS; ++i) {
                    const int c = col(i);
                    if (!(si[c] & 15) && !so[c]) at(i) = darken(pal[int(Layer::Stars) << 4 | st.colour], i, WS);
                }
            }
        }
    }
}

void Interpolator::render(const Step& a, const Step& b, const PairMotion& mo, float t, uint32_t* out, size_t stride) {
    const int S = opt.scale;
    const bool from_a = t < 0.5f || !mo.ok;
    const Step& s = from_a ? a : b;
    w_ = kWidth * S;
    h_ = (kGutterLines + kLines) * S;
    canvas_.resize(size_t(w_) * size_t(h_));
    base(s);
    cmds_.clear();
    const pack::StarsEntry* stars = opt.packs ? opt.packs->stars() : nullptr;
    round_stars_ = stars && stars->shape == pack::StarsEntry::Shape::Round;
    plan_ = (opt.assets && !opt.assets->empty()) || pack_changes_gameplay(opt.packs) ? plan(s) : nullptr;
    over_on_ = (plan_ && !plan_->paints.empty()) || round_stars_;
    star_blocks_.clear();
    star_prev_.clear();
    if (round_stars_) {
        const int size = std::clamp(stars->size, 1, 8);
        const float softness = std::clamp(stars->softness, 0.0f, 1.0f);
        if (star_disc_.scale != S || star_disc_.size != size || star_disc_.softness != softness) {
            // alpha = 1 inside, 0 outside, a linear edge as wide as `softness` of the radius (at least one pixel) centred on it
            StarDisc& d = star_disc_;
            d.scale = S;
            d.size = size;
            d.softness = softness;
            const double R = size * S / 2.0, edge = std::max(softness * R, 1.0), reach = std::ceil(R + edge / 2);
            d.margin = std::max(0, int(std::ceil(reach - S / 2.0)));
            d.side = S + 2 * d.margin;
            d.alpha.assign(size_t(d.side) * size_t(d.side), 0);
            for (int ty = 0; ty < d.side; ++ty)
                for (int tx = 0; tx < d.side; ++tx) {
                    const double dist = std::hypot(tx - d.margin + 0.5 - S / 2.0, ty - d.margin + 0.5 - S / 2.0);
                    d.alpha[size_t(ty) * size_t(d.side) + size_t(tx)] = uint8_t(std::lround(255.0 * std::clamp((R - dist) / edge + 0.5, 0.0, 1.0)));
                }
        }
    }
    if (over_on_) over_.resize(size_t(w_) * size_t(h_));
    const float f = from_a ? t : t - 1.0f;
    // Without motion the commands are replayed in place when replacements or the surround need them.
    if (mo.ok) build(s, mo, f, from_a);
    else if (plan_ || opt.wide > 0) build(s, PairMotion{}, 0.0f, true);
    if (opt.wide > 0) {
        snap_idx_.resize(size_t(h_) * 2 * size_t(16 * S));
        snap_ov_.resize(snap_idx_.size());
        place_side_stars(s, mo, mo.ok ? f : 0.0f);
    }

    // Palette blended per channel from a's to b's: exact at t = 0 and 1. One palette per layer (index = the
    // canvas byte, Layer << 4 | colour), where an asset pack overrides the level's colours.
    uint32_t pal[16 * 5];
    const AssetLibrary* lib = opt.assets;
    const PaletteOverride* oa = lib ? lib->palette(a.palette) : nullptr;
    const PaletteOverride* ob = lib ? lib->palette(b.palette) : nullptr;
    const PaletteOverride* os = from_a ? oa : ob;
    for (int l = 0; l < 5; ++l) {
        auto colour = [l](const Step& st, const PaletteOverride* o, int i) {
            return o && (o->layers >> l & 1) ? o->rgba[i] | 0xFF000000u : rgba_of(st.palette[i]);
        };
        for (int i = 0; i < 16; ++i) {
            uint32_t& p = pal[l * 16 + i];
            if (!mo.ok) { p = colour(s, os, i); continue; }
            const uint32_t ca = colour(a, oa, i), cb = colour(b, ob, i);
            p = 0xFF000000u;
            for (int k = 0; k < 24; k += 8) {
                const float va = float((ca >> k) & 255), vb = float((cb >> k) & 255);
                p |= uint32_t(std::lround(va + (vb - va) * t)) << k;
            }
        }
    }

    shown_ = &s;
    for (int i = 0; i < 16; ++i) shown_pal_[i] = (pal[i] & 0xFF) << 16 | (pal[i] & 0xFF00) | (pal[i] >> 16 & 0xFF);
    const int top = kGutterLines * S, rows = h_ - top;
    int threads = opt.threads > 0 ? opt.threads : int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    if (threads > 1 && (!pool_ || int(pool_->threads.size()) != threads - 1)) pool_ = std::make_unique<Pool>(threads - 1);
    const int bands = threads > 1 && !round_stars_ ? std::min(rows, 2 * threads) : 1;
    auto job = [&](int k) { band(s, top + rows * k / bands, top + rows * (k + 1) / bands, pal, out, stride); };
    if (bands == 1) job(0);
    else pool_->run(bands, job);
}

void Interpolator::render(const Step& a, const Step& b, const PairMotion& mo, float t, std::vector<uint8_t>& rgba) {
    const size_t w = size_t((kWidth + 2 * std::max(0, opt.wide)) * opt.scale);
    rgba.resize(w * size_t(kLines * opt.scale) * 4);
    render(a, b, mo, t, reinterpret_cast<uint32_t*>(rgba.data()) + std::max(0, opt.wide) * opt.scale, w);
}

void Interpolator::indices(const Step& s, int first, int lines, uint8_t* out) {
    for (int y = 0; y < lines; ++y) {
        const uint8_t* src = &s.image[size_t(kGutterLines + first + y) * kLineBytes];
        for (int ax = 0; ax < kWidth; ++ax) {
            uint8_t v = 0;
            for (int p = 0; p < kPlanes; ++p) v |= uint8_t(((src[p * kRowBytes + ax / 8] >> (7 - ax % 8)) & 1) << p);
            out[size_t(y) * kWidth + size_t(ax)] = v;
        }
    }
}

void Interpolator::picture(const Step& s, int scale, std::vector<uint8_t>& rgba) {
    Interpolator ip;
    ip.opt.scale = scale;
    PairMotion none;
    ip.render(s, s, none, 0.0f, rgba);
}

void InterpPresenter::attach(Machine& m, int lines_per_frame) {
    lines_ = lines_per_frame;
    capture.capture_data = true;
    capture.keep = true;
    capture.max_kept = 3;
    capture.attach(m);
}

bool InterpPresenter::frame_held(uint32_t* out, size_t stride) {
    if (!held_) return false;
    interp.render(*held_, *held_, PairMotion{}, 0.0f, out, stride);
    return true;
}

bool InterpPresenter::frame(double tau, uint32_t* out, size_t stride) {
    ++presented;
    held_ = nullptr;
    // Without interpolation: the picture on display in the frame that ends at tau, as the faithful frame shows
    // it (a step swapped in at line 258 of frame F is displayed from frame F + 1).
    if (!interpolate) tau -= 1.0;
    const auto& st = capture.steps();
    size_t n = st.size();
    while (n > 0 && step_time(st[n - 1]) > tau) --n; // newest step already swapped in at tau
    if (n < 1) return false;
    const Step& b = st[n - 1];
    const double span = n >= 2 ? step_time(b) - step_time(st[n - 2]) : 0.0;
    if (tau - step_time(b) > std::max(hold_frames, span)) { // no new picture for a while
        if (Interpolator::gameplay(b)) held_ = &b;
        return false;
    }
    const bool consecutive = n >= 2 && st[n - 2].index + 1 == b.index;
    if (consecutive && interpolate && motion_for_ != b.index) {
        motion_ = interp.analyse(n >= 3 ? &st[n - 3] : nullptr, st[n - 2], b);
        motion_for_ = b.index;
    }
    auto t0 = std::chrono::steady_clock::now();
    if (consecutive && motion_.ok && interpolate) {
        double t = std::clamp((tau - step_time(b)) / span, 0.0, 1.0);
        interp.render(st[n - 2], b, motion_, float(t), out, stride);
        ++interpolated;
    } else {
        // A cut or the first step after one: the newest step's own picture, if it is a gameplay picture.
        if (!Interpolator::gameplay(b)) return false;
        interp.render(b, b, PairMotion{}, 0.0f, out, stride);
    }
    render_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

bool InterpPresenter::frame(double tau, std::vector<uint8_t>& rgba) {
    const int ws = std::max(0, interp.opt.wide) * interp.opt.scale;
    const size_t w = size_t(kWidth * interp.opt.scale + 2 * ws);
    rgba.resize(w * size_t(kLines * interp.opt.scale) * 4);
    return frame(tau, reinterpret_cast<uint32_t*>(rgba.data()) + ws, w);
}

} // namespace x2::modern
