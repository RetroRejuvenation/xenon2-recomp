#include "modern/interp.h"

#include "machine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

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

} // namespace

Layer layer_of(const BlitCmd& b) {
    // Large objects are drawn with the tile routine from their draw handler: the object comes first.
    if (b.obj) return Layer::Object;
    if (far_pc(b.pc)) return Layer::Far;
    if (tile_pc(b.pc)) return Layer::Tiles;
    return Layer::Other;
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
        if (b.pc == 0x1832) return true;
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

void Interpolator::replay_blit(const Step& s, const BlitCmd& b, int ox, int oy, bool clip, int y0, int y1) {
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
            const int64_t off = int64_t(dpt & kChipMask) - base;
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
                    if (set & m) for (int X = xa; X < xb; ++X) row[X] |= pbit;
                    else if (clr & m) for (int X = xa; X < xb; ++X) row[X] &= uint8_t(~pbit);
                    else for (int X = xa; X < xb; ++X) row[X] ^= pbit;
                }
            }
        }
    }
}

void Interpolator::replay_write(const Step& s, const CpuWrite& w, int ox, int oy, bool clip, bool star, int y0, int y1) {
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
    const uint8_t pbit = uint8_t(1 << (off % kLineBytes / kRowBytes)), allowed = star ? star_planes(w.pc) : 0xF;
    for (int i = 0; i < bits; ++i) {
        const uint32_t m = 1u << (bits - 1 - i);
        if (!((set | clr) & m)) continue;
        const int X0 = (px0 + i) * S + ox, xa = std::max(X0, 0), xb = std::min(X0 + S, w_);
        for (int Y = ya; Y < yb; ++Y) {
            uint8_t* row = &canvas_[size_t(Y) * size_t(w_)];
            for (int X = xa; X < xb; ++X) {
                uint8_t& p = row[X];
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
            switch (layer_of(b)) {
            case Layer::Object:
                if (auto it = mo.objects.find(b.obj); it != mo.objects.end()) { dx = f * it->second[0]; dy = f * it->second[1]; }
                break;
            case Layer::Far: dy = far; break;
            case Layer::Tiles: dy = f * float(mo.fg_dy); break;
            default: break;
            }
            cmds_.push_back({true, false, px(dx), px(dy), uint32_t(bi++)});
        } else {
            const CpuWrite& w = s.writes[wi];
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
            cmds_.push_back({false, star, 0, px(dy), uint32_t(wi++)});
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
    for (const Cmd& c : cmds_) {
        const bool moved = c.ox || c.oy;
        if (c.blit) replay_blit(s, s.blits[c.index], c.ox, c.oy, moved, y0, y1);
        else replay_write(s, s.writes[c.index], c.ox, c.oy, moved, c.star, y0, y1);
    }
    const int top = kGutterLines * opt.scale;
    for (int Y = y0; Y < y1; ++Y) {
        const uint8_t* src = &canvas_[size_t(Y) * size_t(w_)];
        uint32_t* dst = out + size_t(Y - top) * stride;
        for (int X = 0; X < w_; ++X) dst[X] = pal[src[X] & 15];
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
    if (mo.ok) build(s, mo, from_a ? t : t - 1.0f, from_a);

    // Palette blended per channel from a's to b's: exact at t = 0 and 1.
    uint32_t pal[16];
    for (int i = 0; i < 16; ++i) {
        if (!mo.ok) { pal[i] = rgba_of(s.palette[i]); continue; }
        const uint32_t ca = rgba_of(a.palette[i]), cb = rgba_of(b.palette[i]);
        pal[i] = 0xFF000000u;
        for (int k = 0; k < 24; k += 8) {
            const float va = float((ca >> k) & 255), vb = float((cb >> k) & 255);
            pal[i] |= uint32_t(std::lround(va + (vb - va) * t)) << k;
        }
    }

    const int top = kGutterLines * S, rows = h_ - top;
    int threads = opt.threads > 0 ? opt.threads : int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    if (threads > 1 && (!pool_ || int(pool_->threads.size()) != threads - 1)) pool_ = std::make_unique<Pool>(threads - 1);
    const int bands = threads > 1 ? std::min(rows, 2 * threads) : 1;
    auto job = [&](int k) { band(s, top + rows * k / bands, top + rows * (k + 1) / bands, pal, out, stride); };
    if (bands == 1) job(0);
    else pool_->run(bands, job);
}

void Interpolator::render(const Step& a, const Step& b, const PairMotion& mo, float t, std::vector<uint8_t>& rgba) {
    const size_t w = size_t(kWidth * opt.scale);
    rgba.resize(w * size_t(kLines * opt.scale) * 4);
    render(a, b, mo, t, reinterpret_cast<uint32_t*>(rgba.data()), w);
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

bool InterpPresenter::frame(double tau, uint32_t* out, size_t stride) {
    ++presented;
    const auto& st = capture.steps();
    size_t n = st.size();
    while (n > 0 && step_time(st[n - 1]) > tau) --n; // newest step already swapped in at tau
    if (n < 1) return false;
    const Step& b = st[n - 1];
    const double span = n >= 2 ? step_time(b) - step_time(st[n - 2]) : 0.0;
    if (tau - step_time(b) > std::max(hold_frames, span)) return false; // no new picture for a while
    const bool consecutive = n >= 2 && st[n - 2].index + 1 == b.index;
    if (consecutive && motion_for_ != b.index) {
        motion_ = interp.analyse(n >= 3 ? &st[n - 3] : nullptr, st[n - 2], b);
        motion_for_ = b.index;
    }
    auto t0 = std::chrono::steady_clock::now();
    if (consecutive && motion_.ok) {
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
    const size_t w = size_t(kWidth * interp.opt.scale);
    rgba.resize(w * size_t(kLines * interp.opt.scale) * 4);
    return frame(tau, reinterpret_cast<uint32_t*>(rgba.data()), w);
}

} // namespace x2::modern
