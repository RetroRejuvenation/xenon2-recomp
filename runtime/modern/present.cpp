// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/present.h"

#include "gfx/video.h"
#include "modern/indexed.h"

#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace x2::modern {

bool parse_wide(const std::string& t, WideSpec& out) {
    WideSpec w;
    if (t == "off" || t == "0") { out = w; return true; }
    char* end = nullptr;
    const long a = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || a < 0) return false;
    if (!*end) {
        if (a > kMaxWide) return false;
        w.px = int(a);
    } else if (*end == ':') {
        const char* q = end + 1;
        const long b = std::strtol(q, &end, 10);
        if (end == q || *end || a < 1 || b < 1 || a > 100 || b > 100) return false;
        w.num = int(a);
        w.den = int(b);
    } else {
        return false;
    }
    out = w;
    return true;
}

std::string wide_name(const WideSpec& w) {
    if (w.num) return std::to_string(w.num) + ":" + std::to_string(w.den);
    return w.px ? std::to_string(w.px) : "off";
}

int wide_pixels(const WideSpec& w, bool aspect) {
    if (w.px) return std::clamp(w.px, 0, kMaxWide);
    if (!w.num) return 0;
    // Width of the whole picture in Amiga pixels at the frame's height: square pixels, or 4:3 (pixels 16:15 wide).
    double total = double(kFrameHeight) * w.num / w.den;
    if (aspect) total *= 15.0 / 16.0;
    return std::clamp(int(std::lround((total - kFrameWidth) / 2)), 0, kMaxWide);
}

WideSpec next_wide(const WideSpec& w) {
    static const int kCycle[][2] = {{0, 0}, {16, 10}, {16, 9}, {21, 9}, {32, 9}};
    size_t i = 0;
    while (i < 5 && !(w.px == 0 && w.num == kCycle[i][0] && w.den == kCycle[i][1])) ++i;
    WideSpec n;
    n.num = kCycle[(i + 1) % 5][0];
    n.den = kCycle[(i + 1) % 5][1];
    return n;
}

PresentOptions PresentOptions::effective() const {
    if (!faithful) return *this;
    PresentOptions f;
    f.faithful = true;
    f.interp = false;
    f.filter = VideoFilter::Nearest;
    f.aspect = false;
    f.integer = true;
    f.curvature = false;
    f.scale = scale;
    f.smooth = smooth;
    return f; // upgrade, wide and hd_panel off
}

std::string PresentOptions::describe() const {
    if (faithful) return "Faithful";
    std::string s = interp ? "interpolated " + std::to_string(filter == VideoFilter::Edge ? 1 : scale) + "x" : "50 Hz";
    s += std::string(", ") + video_filter_name(filter);
    if (filter == VideoFilter::Crt && curvature) s += " curved";
    s += aspect ? ", 4:3" : ", square pixels";
    if (integer) s += ", integer";
    if (interp && !smooth) s += ", nosmooth";
    if (upgrade) s += ", asset packs";
    if (wide.on()) s += ", wide " + wide_name(wide);
    if (hd_panel) s += ", HD panel";
    return s;
}

void scale2x(const uint32_t* src, int w, int h, uint32_t* dst) {
    const size_t dw = size_t(w) * 2;
    for (int y = 0; y < h; ++y) {
        const uint32_t* r = src + size_t(y) * size_t(w);
        const uint32_t* up = y > 0 ? r - w : r;
        const uint32_t* dn = y + 1 < h ? r + w : r;
        uint32_t* o0 = dst + size_t(2 * y) * dw;
        uint32_t* o1 = o0 + dw;
        for (int x = 0; x < w; ++x) {
            const uint32_t B = up[x], D = r[x > 0 ? x - 1 : x], E = r[x], F = r[x + 1 < w ? x + 1 : x], H = dn[x];
            if (B != H && D != F) {
                o0[2 * x] = D == B ? D : E;
                o0[2 * x + 1] = B == F ? F : E;
                o1[2 * x] = D == H ? D : E;
                o1[2 * x + 1] = H == F ? F : E;
            } else {
                o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = E;
            }
        }
    }
}

void scale3x(const uint32_t* src, int w, int h, uint32_t* dst) {
    const size_t dw = size_t(w) * 3;
    for (int y = 0; y < h; ++y) {
        const uint32_t* r = src + size_t(y) * size_t(w);
        const uint32_t* up = y > 0 ? r - w : r;
        const uint32_t* dn = y + 1 < h ? r + w : r;
        uint32_t* o0 = dst + size_t(3 * y) * dw;
        uint32_t* o1 = o0 + dw;
        uint32_t* o2 = o1 + dw;
        for (int x = 0; x < w; ++x) {
            const int xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x;
            const uint32_t A = up[xl], B = up[x], C = up[xr], D = r[xl], E = r[x], F = r[xr], G = dn[xl], H = dn[x],
                           I = dn[xr];
            uint32_t* p0 = o0 + 3 * x;
            uint32_t* p1 = o1 + 3 * x;
            uint32_t* p2 = o2 + 3 * x;
            if (B != H && D != F) {
                p0[0] = D == B ? D : E;
                p0[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                p0[2] = B == F ? F : E;
                p1[0] = (D == B && E != G) || (D == H && E != A) ? D : E;
                p1[1] = E;
                p1[2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                p2[0] = D == H ? D : E;
                p2[1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                p2[2] = H == F ? F : E;
            } else {
                p0[0] = p0[1] = p0[2] = p1[0] = p1[1] = p1[2] = p2[0] = p2[1] = p2[2] = E;
            }
        }
    }
}

void Presenter::configure(Machine& m, const PresentOptions& o, int lines_per_frame, bool keep_capture) {
    eff_ = o.effective();
    keep_capture_ = keep_capture;
    packs_ = eff_.upgrade && ((assets_ && !assets_->empty()) || pack_changes_gameplay(&pack::packs()));
    screens_.set_library(&pack::packs(), layout_->panel);
    sp_ = eff_.upgrade && screens_.active();
    wide_px_ = wide_pixels(eff_.wide, eff_.aspect);
    scale_ = eff_.filter == VideoFilter::Edge ? 1 : std::clamp(eff_.scale, 1, 8);
    // The capture is needed for interpolation, replacement graphics and the surround.
    const bool need = eff_.interp || keep_capture || packs_ || wide_px_ > 0;
    if (need && !ip_) {
        ip_ = std::make_unique<InterpPresenter>();
        ip_->capture.layout = layout_;
        ip_->attach(m, lines_per_frame);
    } else if (!need && ip_) {
        ip_->detach(m);
        ip_.reset();
    }
    if (ip_) {
        // The edge filter upscales Amiga pixels, so it interpolates at 1x (whole-pixel motion).
        ip_->interp.opt.scale = scale_;
        ip_->interp.opt.smooth_far_layer = eff_.smooth;
        ip_->interp.opt.threads = 0;
        ip_->interp.opt.wide = wide_px_;
        ip_->interp.opt.assets = packs_ ? assets_.get() : nullptr;
        ip_->interp.opt.packs = packs_ ? &pack::packs() : nullptr;
        ip_->interpolate = eff_.interp;
    }
}

void Presenter::reset() {
    if (ip_) ip_->capture.reset();
}

Canvas Presenter::faithful(const std::vector<uint8_t>& front) {
    if (eff_.filter == VideoFilter::Edge)
        return edge(reinterpret_cast<const uint32_t*>(front.data()), kFrameWidth, kFrameHeight, 1);
    return {front.data(), kFrameWidth, kFrameHeight, 1};
}

bool Presenter::held_matches(const Step& s, const std::vector<uint8_t>& front) {
    const uint32_t* f = reinterpret_cast<const uint32_t*>(front.data());
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = size_t(kFrameTop) * kFrameWidth, n = i + size_t(kPlayfieldLines) * kFrameWidth; i < n; ++i) h = (h ^ f[i]) * 0x100000001b3ull;
    if (held_index_ == s.index && held_hash_ == h) return held_ok_;
    held_index_ = s.index;
    held_hash_ = h;
    std::vector<uint8_t> pic;
    Interpolator::picture(s, 1, pic);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(pic.data());
    held_ok_ = true;
    for (size_t i = 0, n = size_t(kPlayfieldLines) * kFrameWidth; i < n && held_ok_; ++i)
        held_ok_ = (p[i] | 0xFF000000u) == (f[size_t(kFrameTop) * kFrameWidth + i] | 0xFF000000u);
    return held_ok_;
}

Canvas Presenter::at(double tau, const std::vector<uint8_t>& front) {
    const bool replay = ip_ && (eff_.interp || packs_ || wide_px_ > 0);
    // Screens and the panel from packs work on the indexed frame of the very frame being shown.
    IndexedFrame idx;
    if (sp_ && video_ && front.data() == video_->front().data()) idx = amiga_indexed(*video_);
    const bool sp = idx.valid();
    if (!replay && !eff_.hd_panel && !sp) return faithful(front);
    const int S = scale_, WS = wide_px_ * S, Wc = kFrameWidth * S, W = Wc + 2 * WS, H = kFrameHeight * S;
    full_.resize(size_t(W) * size_t(H) * 4);
    uint32_t* out = reinterpret_cast<uint32_t*>(full_.data());
    uint32_t* centre = out + WS;
    bool have = replay && ip_->frame(tau, centre + size_t(kFrameTop * S) * size_t(W), size_t(W));
    // A game pause stops the steps, and the frame on display stays the held step's picture: keep showing that picture composed
    // (replaced graphics, surround) instead of the original frame. A load or a menu changes the frame, and then it is shown as is.
    if (!have && replay && (packs_ || wide_px_ > 0) && ip_->held() && held_matches(*ip_->held(), front))
        have = ip_->frame_held(centre + size_t(kFrameTop * S) * size_t(W), size_t(W));
    if (!have && !WS && !eff_.hd_panel && !sp) return faithful(front);
    // The faithful frame outside the display window, where there is no step picture, and in the whole centre
    // unless interpolation or replacement graphics change it (the surround alone keeps the frame exact).
    // Without interpolation the status panel is the frame's too: the CPU updates it in place after the swap.
    const bool exact = !eff_.interp && !packs_;
    const uint32_t* f = reinterpret_cast<const uint32_t*>(front.data());
    for (int y = 0; y < H; ++y) {
        const bool window = y >= kFrameTop * S && y < (kFrameTop + kLines) * S;
        const bool panel = y >= (kFrameTop + kPlayfieldLines) * S && window;
        if (window && have && !exact && !(panel && !eff_.interp)) continue;
        const uint32_t* src = f + size_t(y / S) * kFrameWidth;
        uint32_t* dst = centre + size_t(y) * size_t(W);
        for (int x = 0; x < Wc; ++x) dst[x] = src[x / S];
        if (WS && !(window && have)) {
            std::fill(dst - WS, dst, 0xFF000000u);
            std::fill(dst + Wc, dst + Wc + WS, 0xFF000000u);
        }
    }
    if (eff_.hd_panel) {
        // The panel as presented: the step picture's (interpolation shows the panel with the playfield, one
        // step behind the frame), else the frame's.
        hd_panel(have && !exact ? nullptr : f, centre, size_t(W), S);
    }
    if (sp) {
        // While a step picture is shown with interpolation its panel is the step's, not the frame's: the panel's cells are found in that.
        IndexedFrame frame = idx;
        const Step* st = have && eff_.interp && screens_.has_panel() ? ip_->interp.shown_step() : nullptr;
        if (st && !st->image.empty()) {
            const PanelLayout& pl = layout_->panel;
            strip_idx_.assign(size_t(kFrameWidth) * kLines, 0);
            strip_pal_.assign(size_t(kLines) * 16, 0);
            Interpolator::indices(*st, pl.first_line, pl.lines, strip_idx_.data() + size_t(pl.first_line) * kFrameWidth);
            for (int y = pl.first_line; y < pl.first_line + pl.lines; ++y)
                std::copy(ip_->interp.shown_palette(), ip_->interp.shown_palette() + 16, strip_pal_.begin() + long(y) * 16);
            frame.colours = 16;
            frame.index = strip_idx_.data();
            frame.stride = kFrameWidth;
            frame.pal = strip_pal_.data();
        }
        screens_.apply(frame, video_->frames_completed(), in_game_, !have, out, size_t(W), W, H, S, WS, kFrameTop * S, wide_px_);
    }
    if (eff_.filter == VideoFilter::Edge) return edge(out, W, H, S);
    return {full_.data(), W, H, S};
}

// The status panel (window lines 192-199) of the faithful frame, upscaled with Scale2x/Scale3x (twice for 4, 6
// and 8) instead of pixel replication: sharp edges, smoothed diagonals in the digits and icons. Other scales keep
// the replicated panel.
void Presenter::hd_panel(const uint32_t* front, uint32_t* centre, size_t stride, int S) {
    // front null: take the panel from the canvas (its pixels are replicated S x S, the panel does not move).
    if (eff_.filter == VideoFilter::Edge) return; // the whole picture is edge-scaled already
    int steps[3], n = 0;
    switch (S) {
    case 2: steps[n++] = 2; break;
    case 3: steps[n++] = 3; break;
    case 4: steps[n++] = 2; steps[n++] = 2; break;
    case 6: steps[n++] = 3; steps[n++] = 2; break;
    case 8: steps[n++] = 2; steps[n++] = 2; steps[n++] = 2; break;
    default: return;
    }
    const int rows = kLines - kPlayfieldLines, top = kFrameTop + kPlayfieldLines;
    if (front) {
        panel_a_.assign(front + size_t(top) * kFrameWidth, front + size_t(kFrameTop + kLines) * kFrameWidth);
    } else {
        panel_a_.resize(size_t(rows) * kFrameWidth);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < kFrameWidth; ++x) panel_a_[size_t(y) * kFrameWidth + size_t(x)] = centre[size_t((top + y) * S) * stride + size_t(x * S)];
    }
    int w = kFrameWidth, h = rows;
    for (int i = 0; i < n; ++i) {
        panel_b_.resize(size_t(w) * size_t(h) * size_t(steps[i] * steps[i]));
        if (steps[i] == 2) scale2x(panel_a_.data(), w, h, panel_b_.data());
        else scale3x(panel_a_.data(), w, h, panel_b_.data());
        w *= steps[i];
        h *= steps[i];
        panel_a_.swap(panel_b_);
    }
    for (int y = 0; y < h; ++y)
        std::memcpy(centre + size_t((kFrameTop + kPlayfieldLines) * S + y) * stride, &panel_a_[size_t(y) * size_t(w)], size_t(w) * 4);
}

Canvas Presenter::edge(const uint32_t* src, int w, int h, int scale) {
    auto t0 = std::chrono::steady_clock::now();
    const int k = std::clamp(edge_factor, 2, 4);
    edge_a_.resize(size_t(w) * size_t(h) * size_t(k == 3 ? 9 : 4));
    const uint32_t* out = edge_a_.data();
    if (k == 3) {
        scale3x(src, w, h, edge_a_.data());
    } else {
        scale2x(src, w, h, edge_a_.data());
        if (k == 4) {
            edge_b_.resize(size_t(w) * size_t(h) * 16);
            scale2x(edge_a_.data(), 2 * w, 2 * h, edge_b_.data());
            out = edge_b_.data();
        }
    }
    edge_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++edged;
    return {reinterpret_cast<const uint8_t*>(out), w * k, h * k, scale * k};
}

void PresentClock::frame_done(uint64_t now_ns, double frames) {
    newest_ = frames;
    extra_ = std::max(0.0, extra_ - 0.0001);
    const double target = frames - lag - extra_;
    if (!valid_) {
        valid_ = true;
        t0_ = now_ns;
        tau0_ = last_ = target;
        rate_ = 1;
        integ_ = 0;
        ++snaps_;
        return;
    }
    const double pred = tau0_ + (double(now_ns) - double(t0_)) / frame_ns_ * rate_;
    const double e = target - pred;
    if (std::fabs(e) > 4) { // far off (stall, catch-up burst): start again here
        t0_ = now_ns;
        tau0_ = last_ = target;
        rate_ = 1;
        integ_ = 0;
        ++snaps_;
        return;
    }
    // Continue from the predicted position and steer the speed towards the target: a proportional term for
    // the frame-arrival jitter and a slow integral one for the drift between the audio and host clocks.
    t0_ = now_ns;
    tau0_ = pred;
    integ_ = std::clamp(integ_ + 0.002 * e, -0.02, 0.02);
    rate_ = std::clamp(1.0 + 0.05 * e + integ_, 0.9, 1.1);
}

double PresentClock::tau(uint64_t display_ns) {
    if (!valid_) return newest_;
    double v = tau0_ + (double(display_ns) - double(t0_)) / frame_ns_ * rate_;
    if (v > newest_) extra_ = std::min(kMaxExtra, extra_ + 0.1); // caught up: stay further behind
    v = std::clamp(v, last_, std::max(last_, newest_));
    last_ = v;
    return v;
}

uint64_t VsyncTimeline::next(uint64_t now_ns) {
    const double now = double(now_ns);
    if (!init_ || origin_ + double(slot_) * period_ > now + 3 * period_) { // first present, or ran ahead
        if (init_) ++resyncs_;
        init_ = true;
        origin_ = now;
        slot_ = 0;
        return now_ns;
    }
    int64_t s = int64_t(std::floor((now - origin_) / period_)) + 1;
    slot_ = std::max(s, slot_ + 1);
    return uint64_t(origin_ + double(slot_) * period_);
}

void VsyncTimeline::set_mode_period(double period_ns) {
    if (period_ns != mode_) {
        mode_ = period_ns;
        measured_ = false;
        intervals_.clear();
    }
    if (!measured_) period_ = mode_;
}

void VsyncTimeline::presented(uint64_t now_ns) {
    if (!init_) return;
    if (last_) {
        intervals_.push_back(double(now_ns - last_));
        if (intervals_.size() == 32) {
            // Missed refreshes give multiples of the period, so the median is the period while most presents
            // make their refresh.
            std::nth_element(intervals_.begin(), intervals_.begin() + 16, intervals_.end());
            double med = intervals_[16], sum = 0;
            int n = 0;
            for (double d : intervals_)
                if (std::fabs(d - med) < 0.25 * med) { sum += d; ++n; }
            med = sum / n; // the mean of the intervals near the median
            if (med > 0.4 * mode_ && med < 4 * mode_ && std::fabs(med - period_) > 0.005 * period_) {
                period_ = med;
                measured_ = true;
            }
            intervals_.clear();
        }
    }
    last_ = now_ns;
    const double err = double(now_ns) - (origin_ + double(slot_) * period_);
    if (std::fabs(err) > 1.5 * period_) { // missed refreshes or a stall: re-anchor on this present
        origin_ = double(now_ns) - double(slot_) * period_;
        ++resyncs_;
    } else {
        origin_ += 0.05 * err;
    }
}

} // namespace x2::modern
