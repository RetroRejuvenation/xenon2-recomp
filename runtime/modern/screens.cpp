// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/screens.h"

#include "modern/assets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace x2::modern {

namespace {

constexpr uint32_t kOpaque = 0xFF000000u;

// A pack picture (premultiplied RGBA) over an opaque canvas pixel.
inline uint32_t over(uint32_t s, uint32_t d) {
    const uint32_t a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    const uint32_t ia = 255 - a;
    uint32_t out = kOpaque;
    for (int sh = 0; sh < 24; sh += 8) {
        const uint32_t v = ((s >> sh) & 255) + (((d >> sh) & 255) * ia + 127) / 255;
        out |= (v > 255 ? 255u : v) << sh;
    }
    return out;
}

struct FadeQ {
    bool sub = false;
    uint32_t q[3] = {256, 256, 256};  // multiplier x256 (r, g, b)
    int d[3] = {0, 0, 0};             // amount taken off
};

inline uint32_t faded(uint32_t s, const FadeQ& f) {
    uint32_t out = s & kOpaque;
    for (int ch = 0; ch < 3; ++ch) {
        const int sh = 8 * ch;
        const int c = int((s >> sh) & 255);
        const int v = f.sub ? std::max(0, c - f.d[ch]) : int((uint32_t(c) * f.q[ch] + 128) >> 8);
        out |= uint32_t(std::min(v, 255)) << sh;
    }
    return out;
}

uint32_t q8(float v) { return uint32_t(std::clamp(int(std::lround(v * 256.0f)), 0, 256)); }

} // namespace

int count_signature_matches(const pack::ScreenEntry& e, const IndexedFrame& f, int need) {
    const pack::ScreenSignature& sig = e.signature;
    const int n = int(sig.blocks.size());
    const int bs = sig.block;
    if (n == 0 || bs <= 0) return 0;
    if (need < 0) need = sig.min;
    int matched = 0, misses = 0;
    for (const pack::SignatureBlock& b : sig.blocks) {
        bool ok = b.x >= 0 && b.y >= 0 && b.x + bs <= f.w && b.y + bs <= f.h &&
                  pack::block_hash_region(f.index, f.stride, b.x, b.y, bs, bs) == b.hash;
        if (ok) ++matched;
        else if (n - ++misses < need) break; // that many can no longer be reached
    }
    return matched;
}

ScreenMatch ScreenMatcher::match(const std::vector<const pack::ScreenEntry*>& screens, const IndexedFrame& f,
                                 const pack::ScreenEntry* prefer) {
    ScreenMatch best;
    if (!f.valid()) return best;
    if (prefer) {
        // A signature that asks for every block gets no leeway; one that already tolerates misses keeps the screen through two more.
        const int n = int(prefer->signature.blocks.size());
        const int keep = prefer->signature.min >= n ? n : std::max(1, prefer->signature.min - 2);
        const int m = count_signature_matches(*prefer, f, keep);
        if (m >= keep) return {prefer, m};
    }
    size_t best_n = 1;
    for (const pack::ScreenEntry* e : screens) {
        if (!e) continue;
        const int m = count_signature_matches(*e, f);
        if (m < e->signature.min || m <= 0) continue;
        const size_t n = e->signature.blocks.size();
        // share m/n against best.matched/best_n; equal shares: the later screen
        if (!best.entry || size_t(m) * best_n >= size_t(best.matched) * n) {
            best = {e, m};
            best_n = n;
        }
    }
    return best;
}

FadeRatio fade_ratio_rows(const std::vector<uint32_t>& ref_palette, pack::FadeMode mode, const IndexedFrame& f, int y0, int y1,
                          const std::vector<pack::Rect>& skip_rects) {
    FadeRatio r;
    if (mode == pack::FadeMode::None || ref_palette.empty() || !f.valid()) return r;
    y0 = std::max(0, y0);
    y1 = std::min(f.h, y1);
    const int nref = int(ref_palette.size());
    std::vector<uint8_t> skip;
    if (!skip_rects.empty()) {
        skip.assign(size_t(f.w) * size_t(f.h), 0);
        for (const pack::Rect& d : skip_rects)
            for (int y = std::max(0, d.y); y < std::min(f.h, d.y + d.h); ++y)
                for (int x = std::max(0, d.x); x < std::min(f.w, d.x + d.w); ++x) skip[size_t(y) * size_t(f.w) + size_t(x)] = 1;
    }
    // What the frame uses: per line and index, the pixel count with the current and the reference colour.
    struct Obs {
        uint32_t n, cur, ref;
    };
    std::vector<Obs> obs;
    uint32_t count[32];
    for (int y = y0; y < y1; ++y) {
        std::fill(count, count + 32, 0u);
        const uint8_t* row = f.index + size_t(y) * size_t(f.stride);
        for (int x = 0; x < f.w; ++x) {
            if (!skip.empty() && skip[size_t(y) * size_t(f.w) + size_t(x)]) continue;
            ++count[row[x] & 31];
        }
        const uint32_t* pal = f.pal + size_t(y) * size_t(f.colours);
        for (int i = 0; i < 32 && i < nref && i < f.colours; ++i)
            if (count[i]) obs.push_back({count[i], pal[i], ref_palette[size_t(i)]});
    }
    double q[3], d[3];
    double err_mul = 0, err_sub = 0, err_id = 0, total = 0;
    double dev_mul = 0, dev_sub = 0, dev_id = 0;
    for (int ch = 0; ch < 3; ++ch) {
        const int sh = 16 - 8 * ch;
        double cur = 0, ref = 0, dn = 0, dw = 0;
        for (const Obs& o : obs) {
            const double c = double((o.cur >> sh) & 255), g = double((o.ref >> sh) & 255);
            cur += o.n * c;
            ref += o.n * g;
            if (c > 0 && g > c) { dn += o.n * (g - c); dw += o.n; }  // not clamped at zero: the amount taken off
        }
        q[ch] = std::clamp(ref > 0 ? cur / ref : 1.0, 0.0, 1.0);
        d[ch] = dw > 0 ? dn / dw : (cur > 0 ? 0.0 : 255.0);
        for (const Obs& o : obs) {
            const double c = double((o.cur >> sh) & 255), g = double((o.ref >> sh) & 255);
            err_mul += o.n * std::fabs(c - std::round(g * q[ch]));
            err_sub += o.n * std::fabs(c - std::max(0.0, g - d[ch]));
            err_id += o.n * std::fabs(c - g);
            total += o.n;
            dev_mul = std::max(dev_mul, std::fabs(c - std::round(g * q[ch])));
            dev_sub = std::max(dev_sub, std::fabs(c - std::max(0.0, g - d[ch])));
            dev_id = std::max(dev_id, std::fabs(c - g));
        }
    }
    if (q[0] >= 0.98 && q[1] >= 0.98 && q[2] >= 0.98) {   // within 2 % of the reference
        r.residual = total > 0 ? float(err_id / total) : 0.0f;
        r.max_dev = int(std::lround(dev_id));
        return r;
    }
    r.residual = total > 0 ? float(std::min(err_mul, err_sub) / total) : 0.0f;
    r.max_dev = int(std::lround(err_sub <= err_mul ? dev_sub : dev_mul));
    if (err_sub <= err_mul) {   // a tie goes to the game's own way of fading
        r.subtractive = true;
        r.sub_r = int(std::lround(d[0]));
        r.sub_g = int(std::lround(d[1]));
        r.sub_b = int(std::lround(d[2]));
    } else {
        r.r = float(q[0] >= 0.98 ? 1.0 : q[0]);
        r.g = float(q[1] >= 0.98 ? 1.0 : q[1]);
        r.b = float(q[2] >= 0.98 ? 1.0 : q[2]);
    }
    return r;
}

FadeRatio fade_ratio(const pack::ScreenEntry& e, const IndexedFrame& f) {
    return fade_ratio_rows(e.ref_palette, e.fade, f, 0, f.h, e.dynamic);
}

void PackScreens::set_library(const pack::PackLibrary* lib, const PanelLayout& panel) {
    lib_ = lib;
    layout_ = panel;
    screens_.clear();
    panel_ = nullptr;
    cache_.clear();
    cache_size_.clear();
    serial_ = ~0ull;
    current_ = nullptr;
    strip_valid_ = false;
    placements_.clear();
    name_.clear();
    if (!lib) return;
    screens_ = lib->screens();
    panel_ = lib->panel();
}

int PackScreens::pack_scale() const {
    int s = 1;
    for (const pack::ScreenEntry* e : screens_) {
        s = std::max(s, e->scale);
        if (e->wide_pic) s = std::max(s, e->wide_scale > 0 ? e->wide_scale : e->scale);
    }
    if (panel_) {
        s = std::max(s, panel_->scale);
        for (const auto& [k, c] : panel_->cells) s = std::max(s, c.scale);
    }
    return std::clamp(s, 1, 8);
}

PackScreens::View PackScreens::scaled(const pack::Picture* p, int pic_scale, int S) {
    View v;
    if (!p || p->empty() || pic_scale < 1 || S < 1) return v;
    if (pic_scale == S) return {p->px.data(), p->w, p->h};
    const auto key = std::make_tuple(p, pic_scale, S);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        Image src, dst;
        src.w = p->w;
        src.h = p->h;
        src.px = p->px;
        resample(src, S, pic_scale, dst);
        cache_size_[key] = {dst.w, dst.h};
        it = cache_.emplace(key, std::move(dst.px)).first;
    }
    const auto sz = cache_size_[key];
    return {it->second.data(), sz.first, sz.second};
}

void PackScreens::draw_picture(uint32_t* canvas, size_t stride, int cw, int ch, int x0, int y0, const View& v, const FadeRatio& fade) {
    if (!v.px) return;
    const bool fade_on = !fade.unity();
    FadeQ fq;
    fq.sub = fade.subtractive;
    fq.q[0] = q8(fade.r);
    fq.q[1] = q8(fade.g);
    fq.q[2] = q8(fade.b);
    fq.d[0] = fade.sub_r;
    fq.d[1] = fade.sub_g;
    fq.d[2] = fade.sub_b;
    const int xa = std::max(0, -x0), xb = std::min(v.w, cw - x0);
    for (int y = std::max(0, -y0); y < v.h && y0 + y < ch; ++y) {
        const uint32_t* s = v.px + size_t(y) * size_t(v.w);
        uint32_t* d = canvas + size_t(y0 + y) * stride;
        if (!fade_on && xa < xb) {
            // A row without transparency replaces the canvas (the usual picture): copy it.
            uint32_t all = ~0u;
            for (int x = xa; x < xb; ++x) all &= s[x];
            if ((all >> 24) == 255) {
                std::memcpy(d + x0 + xa, s + xa, size_t(xb - xa) * 4);
                continue;
            }
        }
        for (int x = xa; x < xb; ++x) d[x0 + x] = over(fade_on ? faded(s[x], fq) : s[x], d[x0 + x]);
    }
}

void PackScreens::draw_screen(const IndexedFrame& f, const pack::ScreenEntry& e, const FadeRatio& fade, uint32_t* canvas,
                              size_t stride, int cw, int ch, int S, int ox, int oy, int side) {
    if (side > 0 && e.wide_pic) {
        const int ws = e.wide_scale > 0 ? e.wide_scale : e.scale;
        const View w = scaled(e.wide_pic, ws, S);
        if (w.px) draw_picture(canvas, stride, cw, ch, ox + 160 * S - w.w / 2, oy, w, fade);
    }
    const View v = scaled(e.pic, e.scale, S);
    draw_picture(canvas, stride, cw, ch, ox - e.anchor.x * S / std::max(1, e.scale), oy - e.anchor.y * S / std::max(1, e.scale), v, fade);
    // `dynamic`: what the game still draws itself, in front of the artwork
    for (const pack::Rect& d : e.dynamic) {
        for (int py = std::max(0, d.y); py < std::min(f.h, d.y + d.h); ++py)
            for (int px = std::max(0, d.x); px < std::min(f.w, d.x + d.w); ++px) {
                if (f.at(px, py) == e.bg_index) continue;
                const uint32_t c = f.rgba[size_t(py) * size_t(f.rgba_stride) + size_t(px)] | kOpaque;
                for (int sy = 0; sy < S; ++sy) {
                    const int y = oy + py * S + sy;
                    if (y < 0 || y >= ch) continue;
                    for (int sx = 0; sx < S; ++sx) {
                        const int x = ox + px * S + sx;
                        if (x >= 0 && x < cw) canvas[size_t(y) * stride + size_t(x)] = c;
                    }
                }
            }
    }
}

bool PackScreens::draw_panel(const IndexedFrame& f, uint32_t* canvas, size_t stride, int cw, int ch, int S, int ox, int oy) {
    const int first = layout_.first_line, lines = layout_.lines;
    const int cell_w = panel_->cell_w > 0 ? panel_->cell_w : layout_.cell_w;
    const int max_span = std::clamp(panel_->max_span > 0 ? panel_->max_span : layout_.max_span, 1, 8);
    if (first < 0 || lines <= 0 || first + lines > f.h || cell_w <= 0) return false;
    // The strip is re-evaluated only when its bytes change.
    uint64_t h = pack::kFnvOffset;
    for (int y = 0; y < lines; ++y) h = pack::fnv1a64(f.index + size_t(first + y) * size_t(f.stride), size_t(f.w), h);
    bool blank = true;
    for (int y = 0; y < lines && blank; ++y)
        for (int x = 0; x < f.w; ++x)
            if (f.at(x, first + y)) { blank = false; break; }
    if (blank) return false; // nothing is shown there (loading, GET READY): nothing to restyle
    if (!strip_valid_ || h != strip_hash_) {
        strip_valid_ = true;
        strip_hash_ = h;
        placements_.clear();
        const int cols = f.w / cell_w;
        for (int c = 0; c < cols;) {
            int placed = 0;
            for (int span = std::min(max_span, cols - c); span >= 1 && !placed; --span) {
                const uint64_t bh = pack::block_hash_region(f.index, f.stride, c * cell_w, first, span * cell_w, lines);
                if (const pack::PanelCell* cell = panel_->find(pack::panel_key(bh, span))) {
                    placements_.push_back({c, span, cell});
                    placed = span;
                }
            }
            c += placed ? placed : 1;
        }
    }
    const int sx0 = ox, sy0 = oy + first * S, sw = f.w * S, sh = lines * S;
    const FadeRatio fade = fade_ratio_rows(panel_->ref_palette, panel_->fade, f, first, first + lines, {});
    panel_fade_ = fade;
    const bool bg = panel_->background && !panel_->background->empty();
    if (bg) {
        saved_.resize(size_t(sw) * size_t(sh));
        for (int y = 0; y < sh; ++y)
            if (sy0 + y >= 0 && sy0 + y < ch)
                std::memcpy(&saved_[size_t(y) * size_t(sw)], canvas + size_t(sy0 + y) * stride + size_t(sx0), size_t(sw) * 4);
        draw_picture(canvas, stride, cw, ch, sx0, sy0, scaled(panel_->background, panel_->scale, S), fade);
        // cells with no replacement keep the original, whatever the background covers
        std::vector<uint8_t> covered(size_t(f.w / cell_w), 0);
        for (const Placement& p : placements_)
            for (int k = 0; k < p.span; ++k) covered[size_t(p.col + k)] = 1;
        for (int c = 0; c < f.w / cell_w; ++c) {
            if (covered[size_t(c)]) continue;
            for (int y = 0; y < sh; ++y)
                if (sy0 + y >= 0 && sy0 + y < ch)
                    std::memcpy(canvas + size_t(sy0 + y) * stride + size_t(sx0 + c * cell_w * S), &saved_[size_t(y) * size_t(sw) + size_t(c * cell_w * S)],
                                size_t(cell_w * S) * 4);
        }
    }
    for (const Placement& p : placements_) {
        const int cs = std::max(1, p.cell->scale);
        draw_picture(canvas, stride, cw, ch, sx0 + p.col * cell_w * S - p.cell->anchor.x * S / cs, sy0 - p.cell->anchor.y * S / cs,
                     scaled(p.cell->pic, cs, S), fade);
    }
    return true;
}

DrawResult PackScreens::apply(const IndexedFrame& f, uint64_t frame_serial, bool in_game, bool allow_screens, uint32_t* canvas,
                              size_t stride, int cw, int ch, int S, int ox, int oy, int side) {
    DrawResult r;
    name_.clear();
    last_panel_ = false;
    if (!active() || !f.valid() || S < 1 || !canvas) return r;
    if (frame_serial != serial_) {
        serial_ = frame_serial;
        if (allow_screens && has_screens()) {
            ++matches_run;
            const pack::ScreenEntry* prev = current_;
            current_ = ScreenMatcher::match(screens_, f, prev).entry;
            if (current_) fade_ = fade_ratio(*current_, f);
        } else {
            current_ = nullptr;
        }
    }
    if (allow_screens && current_) {
        draw_screen(f, *current_, fade_, canvas, stride, cw, ch, S, ox, oy, side);
        name_ = current_->name;
        r.screen = true;
        ++frames_with_screen;
    } else if (in_game && has_panel()) {
        if (draw_panel(f, canvas, stride, cw, ch, S, ox, oy)) {
            r.panel = true;
            last_panel_ = true;
            ++frames_with_panel;
        }
    }
    return r;
}

bool PackScreens::render_window(const IndexedFrame& f, uint64_t frame_serial, bool in_game, int S, std::vector<uint32_t>& out) {
    if (!active() || !f.valid() || S < 1) return false;
    const int cw = f.w * S, ch = f.h * S;
    out.resize(size_t(cw) * size_t(ch));
    for (int y = 0; y < ch; ++y) {
        const uint32_t* src = f.rgba + size_t(y / S) * size_t(f.rgba_stride);
        uint32_t* dst = out.data() + size_t(y) * size_t(cw);
        for (int x = 0; x < cw; ++x) dst[x] = src[x / S];
    }
    const DrawResult r = apply(f, frame_serial, in_game, true, out.data(), size_t(cw), cw, ch, S, 0, 0, 0);
    return r.screen || r.panel;
}

void ScreensCheck::add(const PackScreens& ps, const IndexedFrame& f, const uint32_t* canvas, size_t stride, int S, int ox, int oy,
                       uint32_t frame_no) {
    const bool screen = !ps.screen_name().empty(), panel = ps.last_drew_panel();
    if (!screen && !panel) return;
    const FadeRatio& fade = screen ? ps.last_fade() : ps.last_panel_fade();
    const bool fading = !fade.unity();
    ++frames;
    with_screen += screen;
    faded += fading;
    // a screen is compared over the whole window, the panel over its strip only (above it is the playfield)
    const PanelLayout& pl = ps.panel_layout();
    const int y0 = screen ? 0 : (pl.known() ? pl.first_line : f.h - 8), y1 = screen ? f.h : (pl.known() ? pl.first_line + pl.lines : f.h);
    int err = 0;
    for (int y = y0 * S; y < y1 * S; ++y)
        for (int x = 0; x < f.w * S; ++x) {
            const uint32_t a = canvas[size_t(oy + y) * stride + size_t(ox + x)];
            const uint32_t b = f.rgba[size_t(y / S) * size_t(f.rgba_stride) + size_t(x / S)];
            for (int sh = 0; sh < 24; sh += 8) err = std::max(err, std::abs(int((a >> sh) & 255) - int((b >> sh) & 255)));
        }
    if (err == 0) {
        ++exact;
        faded_exact += fading;
    } else if (!fade.explained()) {
        ++palette;   // the palette differs from the pack's in a way no fade explains (palette animation, another state of the game)
    } else if (fading) {
        fade_err = std::max(fade_err, err);
    } else {
        ++bad;
        last_bad = frame_no;
        max_err = std::max(max_err, err);
    }
}

std::string ScreensCheck::summary(const PackScreens& ps) const {
    char b[640];
    std::snprintf(b, sizeof b,
                  "screens check: %u frames with a screen or the panel, %u identical to the frame; screens drawn in %llu frames (%u fading, %u of those "
                  "identical, largest channel error %d), panel drawn in %llu; %u differ with a palette that is not a fade of the pack's (shop, bonus), "
                  "%u differ otherwise (max channel error %d, last at frame %u)",
                  frames, exact, (unsigned long long)ps.frames_with_screen, faded, faded_exact, fade_err, (unsigned long long)ps.frames_with_panel,
                  palette, bad, max_err, last_bad);
    return b;
}

} // namespace x2::modern
