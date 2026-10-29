#include "modern/interp.h"

#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace x2::modern {

namespace {

// Drawing routines of the resident game (docs/re-notes/modernisation-design.md section 2).
bool far_pc(uint32_t pc) { return pc == 0x1832 || pc == 0x185A || pc == 0x1884; } // blit_objects_main $17D6
bool tile_pc(uint32_t pc) { return pc >= 0x18A0 && pc < 0x1EA2; }                 // draw_background_a/b
bool star_pc(uint32_t pc) { return pc >= 0x233A && pc < 0x23EE; }                 // object_pass_a $233A

// The planes a star write may set: the four star groups of $233A use colours 4, 5, 6 and 7.
uint8_t star_planes(uint32_t pc) {
    if (pc < 0x2370) return 0x4;
    if (pc < 0x2398) return 0x5;
    if (pc < 0x23C2) return 0x6;
    return 0x7;
}

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

} // namespace

bool Interpolator::gameplay(const Step& s) {
    if (s.image.empty()) return false;
    for (const BlitCmd& b : s.blits)
        if (b.pc == 0x1832) return true;
    return false;
}

PairMotion Interpolator::analyse(const Step* z, const Step& a, const Step& b) const {
    PairMotion mo;
    if (!gameplay(a) || !gameplay(b)) { mo.why = "not a gameplay step"; return mo; }
    if (b.index != a.index + 1) { mo.why = "steps not consecutive"; return mo; }
    uint32_t frames = b.swap_frame - a.swap_frame;
    if (frames < 2 || frames > 8) { mo.why = "step interval out of range"; return mo; }

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
        if (tile_pc(t.pc) && !t.obj) prev[{t.apt, t.bpt, uint32_t(t.x), uint32_t(t.plane)}].push_back(signed_line(a, t.dpt));
    std::map<int, int> votes;
    for (const BlitCmd& t : b.blits) {
        if (!tile_pc(t.pc) || t.obj) continue;
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

    // Objects: same record and update handler in both steps, with a plausible move.
    std::unordered_map<uint32_t, const ObjectSnap*> pa;
    for (const ObjectSnap& o : a.objects) pa[o.addr] = &o;
    for (const ObjectSnap& o : b.objects) {
        auto it = pa.find(o.addr);
        if (it == pa.end() || it->second->update != o.update) continue;
        int dx = o.x - it->second->x, dy = o.y - it->second->y;
        if (std::abs(dx) > opt.max_object_jump || std::abs(dy) > opt.max_object_jump) continue;
        mo.objects[o.addr] = {float(dx), float(dy)};
    }

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

void Interpolator::replay_blit(const Step& s, const BlitCmd& b, int ox, int oy, bool clip) {
    if (b.data == ~0u || !(b.con0 & 0x100)) return; // line / fill mode (not used in gameplay), or no D
    const int S = opt.scale;
    const bool desc = b.con1 & 2;
    const int inc = desc ? -2 : 2;
    int dmod = b.dmod & ~1;
    if (desc) dmod = -dmod;
    const bool cookie = (b.con0 & 0x200) && b.cpt == b.dpt && b.cmod == b.dmod;
    const uint8_t minterm = uint8_t(b.con0);
    const int64_t base = int64_t(s.draw_buf) - int64_t(kGutterLines) * kLineBytes;
    const int64_t bytes = int64_t(kGutterLines + kLines) * kLineBytes;
    const int ylimit = clip ? (kGutterLines + kPlayfieldLines) * S : h_;
    const uint16_t* wd = &s.words[b.data];
    uint32_t dpt = b.dpt;
    for (int y = 0; y < b.h; ++y) {
        for (int x = 0; x < b.w; ++x, wd += 3, dpt += uint32_t(inc)) {
            int64_t off = int64_t(dpt & kChipMask) - base;
            if (off < 0 || off >= bytes) continue;
            const int cy = int(off / kLineBytes), plane = int(off % kLineBytes / kRowBytes);
            const int px0 = int(off % kRowBytes) * 8;
            const uint8_t pbit = uint8_t(1 << plane);
            for (int i = 0; i < 16; ++i) {
                const int ab = (wd[0] >> (15 - i)) & 1, bb = (wd[1] >> (15 - i)) & 1, cb = (wd[2] >> (15 - i)) & 1;
                const int X0 = (px0 + i) * S + ox, Y0 = cy * S + oy;
                for (int sy = 0; sy < S; ++sy) {
                    const int Y = Y0 + sy;
                    if (Y < 0 || Y >= ylimit) continue;
                    uint8_t* row = &canvas_[size_t(Y) * size_t(w_)];
                    for (int sx = 0; sx < S; ++sx) {
                        const int X = X0 + sx;
                        if (X < 0 || X >= w_) continue;
                        const int c = cookie ? (row[X] & pbit) != 0 : cb;
                        if ((minterm >> ((ab << 2) | (bb << 1) | c)) & 1) row[X] |= pbit;
                        else row[X] &= uint8_t(~pbit);
                    }
                }
            }
        }
        dpt += uint32_t(dmod);
    }
}

void Interpolator::replay_write(const Step& s, const CpuWrite& w, int ox, int oy, bool clip, bool star) {
    const int S = opt.scale;
    const int64_t off = int64_t(w.addr) - (int64_t(s.draw_buf) - int64_t(kGutterLines) * kLineBytes);
    if (off < 0 || off >= int64_t(kGutterLines + kLines) * kLineBytes) return;
    const int bits = w.size == 1 ? 8 : 16;
    const uint32_t set = w.value & ~w.old, clr = w.old & ~w.value;
    if (!set && !clr) return;
    const int cy = int(off / kLineBytes), plane = int(off % kLineBytes / kRowBytes);
    const int px0 = int(off % kRowBytes) * 8;
    const uint8_t pbit = uint8_t(1 << plane), allowed = star ? star_planes(w.pc) : 0xF;
    const int ylimit = clip ? (kGutterLines + kPlayfieldLines) * S : h_;
    for (int i = 0; i < bits; ++i) {
        const uint32_t m = 1u << (bits - 1 - i);
        if (!((set | clr) & m)) continue;
        for (int sy = 0; sy < S; ++sy) {
            const int Y = cy * S + oy + sy;
            if (Y < 0 || Y >= ylimit) continue;
            for (int sx = 0; sx < S; ++sx) {
                const int X = (px0 + i) * S + ox + sx;
                if (X < 0 || X >= w_) continue;
                uint8_t& p = canvas_[size_t(Y) * size_t(w_) + size_t(X)];
                if (set & m) {
                    // A star is only drawn on empty background ($233A tests all four planes first).
                    if (star && (p & ~allowed)) continue;
                    p |= pbit;
                } else {
                    p &= uint8_t(~pbit);
                }
            }
        }
    }
}

void Interpolator::replay(const Step& s, const PairMotion& mo, float f, bool from_a) {
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
            const BlitCmd& b = s.blits[bi++];
            // Large objects are drawn with the tile routine from their draw handler: the object comes first.
            if (b.obj) {
                auto it = mo.objects.find(b.obj);
                if (it != mo.objects.end()) { dx = f * it->second[0]; dy = f * it->second[1]; }
            } else if (far_pc(b.pc)) dy = far;
            else if (tile_pc(b.pc)) dy = f * float(mo.fg_dy);
            int ox = px(dx), oy = px(dy);
            replay_blit(s, b, ox, oy, ox || oy);
        } else {
            const CpuWrite& w = s.writes[wi++];
            bool star = star_pc(w.pc);
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
            int oy = px(dy);
            replay_write(s, w, 0, oy, oy != 0, star);
        }
    }
}

void Interpolator::to_rgba(const uint16_t* palette, std::vector<uint8_t>& rgba) const {
    const int S = opt.scale;
    uint32_t pal[16];
    for (int i = 0; i < 16; ++i) {
        uint16_t c = palette[i];
        pal[i] = (uint32_t(((c >> 8) & 15) * 17)) | (uint32_t(((c >> 4) & 15) * 17) << 8) |
                 (uint32_t((c & 15) * 17) << 16) | 0xFF000000u;
    }
    rgba.resize(size_t(w_) * size_t(kLines * S) * 4);
    const uint8_t* src = &canvas_[size_t(kGutterLines * S) * size_t(w_)];
    for (size_t i = 0; i < size_t(w_) * size_t(kLines * S); ++i) {
        uint32_t c = pal[src[i] & 15];
        rgba[4 * i] = uint8_t(c);
        rgba[4 * i + 1] = uint8_t(c >> 8);
        rgba[4 * i + 2] = uint8_t(c >> 16);
        rgba[4 * i + 3] = 0xFF;
    }
}

void Interpolator::render(const Step& a, const Step& b, const PairMotion& mo, float t, std::vector<uint8_t>& rgba) {
    const int S = opt.scale;
    const bool from_a = t < 0.5f || !mo.ok;
    const Step& s = from_a ? a : b;
    w_ = kWidth * S;
    h_ = (kGutterLines + kLines) * S;
    canvas_.assign(size_t(w_) * size_t(h_), 0);
    // Start from the step's finished picture: lines the moved playfield does not cover keep it.
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const int ay = y / S, ax = x / S;
            const uint8_t* line = &s.image[size_t(ay) * kLineBytes];
            uint8_t v = 0;
            for (int p = 0; p < kPlanes; ++p)
                v |= uint8_t(((line[p * kRowBytes + ax / 8] >> (7 - ax % 8)) & 1) << p);
            canvas_[size_t(y) * size_t(w_) + size_t(x)] = v;
        }
    if (mo.ok) replay(s, mo, from_a ? t : t - 1.0f, from_a);
    to_rgba(s.palette, rgba);
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

bool InterpPresenter::frame(double tau, std::vector<uint8_t>& rgba) {
    ++presented;
    const auto& st = capture.steps();
    size_t n = st.size();
    while (n > 0 && step_time(st[n - 1]) > tau) --n; // newest step already swapped in at tau
    if (n < 2) return false;
    const Step& a = st[n - 2];
    const Step& b = st[n - 1];
    if (motion_for_ != b.index) {
        motion_ = interp.analyse(n >= 3 ? &st[n - 3] : nullptr, a, b);
        motion_for_ = b.index;
    }
    if (!motion_.ok) return false;
    double span = step_time(b) - step_time(a);
    double t = std::clamp((tau - step_time(b)) / span, 0.0, 1.0);
    auto t0 = std::chrono::steady_clock::now();
    interp.render(a, b, motion_, float(t), rgba);
    render_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++interpolated;
    return true;
}

void InterpPresenter::compose(const std::vector<uint8_t>& faithful, int fw, int fh, int top,
                              const std::vector<uint8_t>& play, std::vector<uint8_t>& out) const {
    const int S = interp.opt.scale, W = fw * S, H = fh * S;
    out.resize(size_t(W) * size_t(H) * 4);
    for (int y = 0; y < H; ++y) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(&out[size_t(y) * size_t(W) * 4]);
        int py = y - top * S;
        if (py >= 0 && py < kLines * S && fw == kWidth) {
            std::copy_n(reinterpret_cast<const uint32_t*>(&play[size_t(py) * size_t(W) * 4]), W, dst);
            continue;
        }
        const uint32_t* src = reinterpret_cast<const uint32_t*>(&faithful[size_t(y / S) * size_t(fw) * 4]);
        for (int x = 0; x < W; ++x) dst[x] = src[x / S];
    }
}

} // namespace x2::modern
