#include "modern/present.h"

#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace x2::modern {

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
    return f;
}

std::string PresentOptions::describe() const {
    if (faithful) return "Faithful";
    std::string s = interp ? "interpolated " + std::to_string(filter == VideoFilter::Edge ? 1 : scale) + "x" : "50 Hz";
    s += std::string(", ") + video_filter_name(filter);
    if (filter == VideoFilter::Crt && curvature) s += " curved";
    s += aspect ? ", 4:3" : ", square pixels";
    if (integer) s += ", integer";
    if (interp && !smooth) s += ", nosmooth";
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
    const bool need = eff_.interp || keep_capture;
    if (need && !ip_) {
        ip_ = std::make_unique<InterpPresenter>();
        ip_->attach(m, lines_per_frame);
    } else if (!need && ip_) {
        ip_->detach(m);
        ip_.reset();
    }
    if (ip_) {
        // The edge filter upscales Amiga pixels, so it interpolates at 1x (whole-pixel motion).
        ip_->interp.opt.scale = eff_.filter == VideoFilter::Edge ? 1 : std::clamp(eff_.scale, 1, 8);
        ip_->interp.opt.smooth_far_layer = eff_.smooth;
        ip_->interp.opt.threads = 0;
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

Canvas Presenter::at(double tau, const std::vector<uint8_t>& front) {
    if (!eff_.interp || !ip_) return faithful(front);
    const int S = ip_->interp.opt.scale, W = kFrameWidth * S, H = kFrameHeight * S;
    full_.resize(size_t(W) * size_t(H) * 4);
    uint32_t* out = reinterpret_cast<uint32_t*>(full_.data());
    if (!ip_->frame(tau, out + size_t(kFrameTop * S) * size_t(W), size_t(W))) return faithful(front);
    // Lines outside the display window come from the faithful frame.
    const uint32_t* f = reinterpret_cast<const uint32_t*>(front.data());
    for (int y = 0; y < H; ++y) {
        if (y >= kFrameTop * S && y < (kFrameTop + kLines) * S) continue;
        const uint32_t* src = f + size_t(y / S) * kFrameWidth;
        uint32_t* dst = out + size_t(y) * size_t(W);
        for (int x = 0; x < W; ++x) dst[x] = src[x / S];
    }
    if (eff_.filter == VideoFilter::Edge) return edge(out, W, H, S);
    return {full_.data(), W, H, S};
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
