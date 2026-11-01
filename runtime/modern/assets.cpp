// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/assets.h"

#include "gfx/image_io.h"
#include "machine.h"
#include "pack/pack_paths.h"
#include "pack/pack_png.h"
#include "modern/interp.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace x2::modern {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;
void fnv(uint64_t& h, uint8_t b) {
    h ^= b;
    h *= kFnvPrime;
}

// Buffer position of a word the blit writes: line relative to the draw buffer (negative: gutter), plane and
// byte column. False outside the buffer and its gutter.
bool word_pos(const Step& s, uint32_t addr, int& line, int& plane, int& col) {
    const int64_t off = int64_t(addr & s.chip_mask) - (int64_t(s.draw_buf) - int64_t(kGutterLines) * kLineBytes);
    if (off < 0 || off >= int64_t(kGutterLines + kLines) * kLineBytes) return false;
    line = int(off / kLineBytes) - kGutterLines;
    plane = int(off % kLineBytes / kRowBytes);
    col = int(off % kRowBytes);
    return true;
}

// Calls f(row, word, address, {A, B, C}) for every word a blit writes, in the blitter's order.
template <class F>
void for_words(const Step& s, const BlitCmd& b, F&& f) {
    const bool desc = b.con1 & 2;
    const int inc = desc ? -2 : 2;
    int dmod = b.dmod & ~1;
    if (desc) dmod = -dmod;
    const uint16_t* wd = &s.words[b.data];
    uint32_t dpt = b.dpt;
    for (int y = 0; y < b.h; ++y, dpt += uint32_t(dmod))
        for (int x = 0; x < b.w; ++x, wd += 3, dpt += uint32_t(inc)) f(y, x, dpt, wd);
}

bool cookie_blit(const BlitCmd& b) {
    return b.data != ~0u && (b.con0 & 0x0F00) == 0x0F00 && (b.con0 & 0xFF) == 0xCA && b.cpt == b.dpt && !(b.con1 & 2);
}
// A hit flash: the bob's mask (A) written to the screen (C = D) with minterm $FA (the plane's bit set where the mask is) or $0A
// (cleared), once per plane: the bob in one flat colour.
bool flash_blit(const BlitCmd& b) {
    const uint8_t mt = uint8_t(b.con0);
    return b.obj && b.data != ~0u && (b.con0 & 0x0F00) == 0x0B00 && (mt == 0xFA || mt == 0x0A) && b.cpt == b.dpt && !(b.con1 & 2);
}
bool copy_blit(const BlitCmd& b, const CaptureLayout& L) {
    return b.data != ~0u && (b.con0 & 0x0F00) == 0x0900 && (b.con0 & 0xFF) == 0xF0 && !(b.con1 & 2) && !L.far_copy_pc(b.pc);
}

// Pixels a group of blits writes, merged: per (line, x) the planes written, whether it is drawn, and its colour.
struct Cells {
    int line0 = 0, lines = 0;
    std::vector<uint16_t> c; // bit 8-11 planes written, bit 4 drawn, bits 0-3 colour
    uint16_t& at(int line, int x) { return c[size_t(line - line0) * kWidth + size_t(x)]; }
};

// Merges blits[first..last] into a Graphic; false if it is not a complete 4-plane picture. `flash_planes` (a hit flash: bit p set
// when plane p's blit sets the mask's pixels, clear when it clears them) gives the colour instead of the B data.
bool merge(const Step& s, uint32_t first, uint32_t last, bool cookie, bool keep_pixels, Graphic& g, int flash_planes = -1) {
    int lo = 1 << 20, hi = -(1 << 20);
    for (uint32_t i = first; i <= last; ++i)
        for_words(s, s.blits[i], [&](int, int, uint32_t a, const uint16_t*) {
            int line, plane, col;
            if (word_pos(s, a, line, plane, col)) { lo = std::min(lo, line); hi = std::max(hi, line); }
        });
    if (lo > hi) return false;
    Cells cells;
    cells.line0 = lo;
    cells.lines = hi - lo + 1;
    cells.c.assign(size_t(cells.lines) * kWidth, 0);
    for (uint32_t i = first; i <= last; ++i)
        for_words(s, s.blits[i], [&](int, int, uint32_t a, const uint16_t* wd) {
            int line, plane, col;
            if (!word_pos(s, a, line, plane, col) || (col & 1)) return;
            const uint16_t mask = cookie ? wd[0] : 0xFFFF,
                           bits = flash_planes >= 0 ? uint16_t((flash_planes >> plane) & 1 ? 0xFFFF : 0) : cookie ? wd[1] : wd[0];
            for (int k = 0; k < 16; ++k) {
                const uint16_t m = uint16_t(0x8000 >> k);
                if (!(mask & m) || col * 8 + k >= kWidth) continue;
                uint16_t& c = cells.at(line, col * 8 + k);
                c = uint16_t((c & ~(1u << plane)) | 0x10 | (0x100u << plane) | ((bits & m) ? 1u << plane : 0u));
            }
        });
    int x0 = kWidth, x1 = -1, y0 = 1 << 20, y1 = -(1 << 20);
    for (int l = 0; l < cells.lines; ++l)
        for (int x = 0; x < kWidth; ++x) {
            const uint16_t c = cells.c[size_t(l) * kWidth + size_t(x)];
            if (!(c & 0x10)) continue;
            if ((c & 0xF00) != 0xF00) return false; // not every plane drawn: no picture of its own
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, l);
            y1 = std::max(y1, l);
        }
    if (x1 < 0) return false;
    {
        const BlitCmd& b = s.blits[first];
        int line, plane, col;
        if (!word_pos(s, b.dpt, line, plane, col)) return false;
        g.ox = col * 8 + (cookie ? b.ash() : 0);
        g.oline = line;
        g.rows = cells.lines;
        uint64_t src = kFnvBasis;
        for (uint32_t v : {uint32_t(cookie), b.apt, cookie ? b.bpt : 0u})
            for (int k = 0; k < 4; ++k) fnv(src, uint8_t(v >> (8 * k)));
        g.src = src;
    }
    g.first = first;
    g.last = last;
    g.cookie = cookie;
    g.x = x0;
    g.y = cells.line0 + y0;
    g.w = x1 - x0 + 1;
    g.h = y1 - y0 + 1;
    uint64_t h = kFnvBasis, shape = kFnvBasis;
    fnv(shape, 'M');
    for (int v : {g.w, g.h}) { fnv(h, uint8_t(v >> 8)); fnv(h, uint8_t(v)); fnv(shape, uint8_t(v >> 8)); fnv(shape, uint8_t(v)); }
    if (keep_pixels) g.pix.resize(size_t(g.w) * size_t(g.h));
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const uint16_t c = cells.c[size_t(y0 + y) * kWidth + size_t(x0 + x)];
            const uint8_t p = c & 0x10 ? uint8_t(c & 0x1F) : 0;
            fnv(h, p);
            fnv(shape, p ? 1 : 0);
            if (keep_pixels) g.pix[size_t(y) * size_t(g.w) + size_t(x)] = p;
        }
    g.hash = h;
    g.shape = shape;
    return true;
}

uint32_t amiga_rgba(uint16_t c) {
    return uint32_t(((c >> 8) & 15) * 17) | uint32_t(((c >> 4) & 15) * 17) << 8 | uint32_t((c & 15) * 17) << 16 | 0xFF000000u;
}

} // namespace

std::string hash_hex(uint64_t h) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

std::string graphic_key(uint64_t hash, int w, int h) { return hash_hex(hash) + "-" + std::to_string(w) + "x" + std::to_string(h); }

void find_graphics(const Step& s, std::vector<Graphic>& out, bool keep_pixels) {
    out.clear();
    for (uint32_t i = 0; i < s.blits.size();) {
        const BlitCmd& b = s.blits[i];
        if (s.layout->far_copy_pc(b.pc) && b.data != ~0u && b.w == 40 && (b.con0 & 0xFFF) == 0x9F0) {
            // One far-layer line per row: planes 0 and 1 (20 words each).
            const uint16_t* wd = &s.words[b.data];
            uint32_t dpt = b.dpt;
            for (uint32_t r = 0; r < b.h; ++r, wd += 3 * 40, dpt += 80 + uint32_t(b.dmod & ~1)) {
                int line, plane, col;
                if (!word_pos(s, dpt, line, plane, col) || plane != 0 || col != 0) continue;
                Graphic g;
                g.kind = GfxKind::Far;
                g.first = g.last = i;
                g.row = r;
                g.y = line;
                g.w = kWidth;
                g.h = 1;
                uint64_t h = kFnvBasis;
                fnv(h, 'F');
                for (int k = 0; k < 40; ++k) { fnv(h, uint8_t(wd[3 * k] >> 8)); fnv(h, uint8_t(wd[3 * k])); }
                g.hash = h;
                if (keep_pixels) {
                    g.pix.resize(kWidth);
                    for (int x = 0; x < kWidth; ++x) {
                        const uint16_t m = uint16_t(0x8000 >> (x & 15));
                        g.pix[size_t(x)] = uint8_t(0x10 | ((wd[3 * (x >> 4)] & m) ? 1 : 0) | ((wd[3 * (20 + (x >> 4))] & m) ? 2 : 0));
                    }
                }
                out.push_back(std::move(g));
            }
            ++i;
            continue;
        }
        if (flash_blit(b)) {
            // A hit flash: four plane blits of one object, the same mask (A) and size, planes 40 bytes apart.
            uint32_t n = 1;
            int planes = (uint8_t(b.con0) == 0xFA) ? 1 : 0;
            while (n < 4 && i + n < s.blits.size()) {
                const BlitCmd& c = s.blits[i + n];
                if (!flash_blit(c) || c.obj != b.obj || c.apt != b.apt || c.w != b.w || c.h != b.h || (c.con0 & 0xFF00) != (b.con0 & 0xFF00) ||
                    c.fwm != b.fwm || c.lwm != b.lwm || c.dpt != b.dpt + 40 * n)
                    break;
                if (uint8_t(c.con0) == 0xFA) planes |= 1 << n;
                ++n;
            }
            if (n == 4) {
                Graphic g;
                g.kind = GfxKind::Flash;
                if (merge(s, i, i + 3, true, keep_pixels, g, planes)) out.push_back(std::move(g));
            }
            i += n;
            continue;
        }
        if (cookie_blit(b)) {
            // A bob or masked tile: one blit per plane, the same mask (A) and size, planes 40 bytes apart.
            uint32_t n = 1;
            while (n < 4 && i + n < s.blits.size()) {
                const BlitCmd& c = s.blits[i + n];
                if (!cookie_blit(c) || c.obj != b.obj || c.apt != b.apt || c.w != b.w || c.h != b.h || c.con0 != b.con0 ||
                    c.fwm != b.fwm || c.lwm != b.lwm || c.dpt != b.dpt + 40 * n)
                    break;
                ++n;
            }
            Graphic g;
            g.kind = b.obj ? GfxKind::Sprite : GfxKind::Tile;
            if (n == 4 && merge(s, i, i + 3, true, keep_pixels, g)) out.push_back(std::move(g));
            i += n;
            continue;
        }
        if (copy_blit(b, *s.layout)) {
            Graphic g;
            g.kind = b.obj ? GfxKind::Sprite : GfxKind::Tile;
            if (merge(s, i, i, false, keep_pixels, g)) out.push_back(std::move(g));
        }
        ++i;
    }
}

const SourceTracker::Full* SourceTracker::fuller(const Graphic& g, const uint16_t* palette) {
    uint64_t key = g.src;
    for (int i = 0; i < 16; ++i) fnv(key, uint8_t(palette[i] >> 4)), fnv(key, uint8_t(palette[i]));
    const Full mine{g.hash, g.w, g.h, g.x - g.ox, g.y - g.oline, g.rows};
    auto [it, fresh] = m_.emplace(key, mine);
    if (fresh) return nullptr;
    if (g.rows < it->second.rows && g.w <= it->second.w && g.hash != it->second.hash) {
        found_ = it->second;
        return &found_;
    }
    if (g.rows >= it->second.rows) it->second = mine;
    return nullptr;
}

bool safe_pack_path(const std::string& p) { return pack::safe_pack_path(p); }

bool decode_png(const std::vector<uint8_t>& f, Image& out, std::string& why, int max_side, size_t max_pixels) {
    return pack::decode_png(f.data(), f.size(), out, why, max_side, max_pixels);
}

void resample(const Image& src, int num, int den, Image& out) {
    out.w = std::max(1, int((int64_t(src.w) * num + den / 2) / den));
    out.h = std::max(1, int((int64_t(src.h) * num + den / 2) / den));
    out.px.assign(size_t(out.w) * size_t(out.h), 0);
    if (num % den == 0) { // integer up-scale: replicate
        const int k = num / den;
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
                out.px[size_t(y) * size_t(out.w) + size_t(x)] = src.px[size_t(std::min(y / k, src.h - 1)) * size_t(src.w) + size_t(std::min(x / k, src.w - 1))];
        return;
    }
    // Separable box filter: each output pixel averages the source area it covers (den / num source pixels).
    const double r = double(den) / double(num);
    auto weights = [&](int n_out, int n_src, std::vector<std::vector<std::pair<int, double>>>& wts) {
        wts.resize(size_t(n_out));
        for (int o = 0; o < n_out; ++o) {
            const double a = o * r, b = std::min(double(n_src), (o + 1) * r);
            double sum = 0;
            for (int i = int(std::floor(a)); i < int(std::ceil(b)) && i < n_src; ++i) {
                const double cover = std::min(b, double(i + 1)) - std::max(a, double(i));
                if (cover > 0) { wts[size_t(o)].push_back({i, cover}); sum += cover; }
            }
            for (auto& p : wts[size_t(o)]) p.second /= sum > 0 ? sum : 1;
        }
    };
    std::vector<std::vector<std::pair<int, double>>> wx, wy;
    weights(out.w, src.w, wx);
    weights(out.h, src.h, wy);
    std::vector<double> tmp(size_t(out.w) * size_t(src.h) * 4, 0.0);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < out.w; ++x)
            for (auto [i, wt] : wx[size_t(x)]) {
                const uint32_t p = src.px[size_t(y) * size_t(src.w) + size_t(i)];
                for (int c = 0; c < 4; ++c) tmp[(size_t(y) * size_t(out.w) + size_t(x)) * 4 + size_t(c)] += wt * double((p >> (8 * c)) & 255);
            }
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (auto [i, wt] : wy[size_t(y)])
                for (int c = 0; c < 4; ++c) acc[c] += wt * tmp[(size_t(i) * size_t(out.w) + size_t(x)) * 4 + size_t(c)];
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c) v |= uint32_t(std::clamp(int(std::lround(acc[c])), 0, 255)) << (8 * c);
            out.px[size_t(y) * size_t(out.w) + size_t(x)] = v;
        }
}

// ---- AssetLibrary: the version 1 interface over the version 2 loader (runtime/pack) ------------------------------------

AssetLibrary::AssetLibrary() = default;
AssetLibrary::~AssetLibrary() = default;

uint64_t AssetLibrary::gkey_hash(const GKey& k) { return (k.hash ^ (uint64_t(uint32_t(k.w)) << 40) ^ (uint64_t(uint32_t(k.h)) << 20)) * 0x9E3779B97F4A7C15ull; }

void AssetLibrary::set_context(const pack::PackContext& ctx) {
    if (!stack_) context_ = ctx;
}

void AssetLibrary::rebuild_mirror() {
    graphics_.clear();
    far_.clear();
    if (!stack_) return;
    stack_->for_each_graphic([&](uint64_t hash, int w, int h, const pack::GraphicEntry& e) {
        graphics_[GKey{hash, w, h}] = Replacement{e.pic, e.scale, e.anchor.x, e.anchor.y};
    });
    stack_->for_each_far([&](uint64_t hash, const pack::FarLineRef& r) { far_[hash] = FarRow{r.strip, r.scale, r.row}; });
}

void AssetLibrary::adopt(std::shared_ptr<const pack::PackStack> stack) {
    stack_ = std::move(stack);
    owned_.reset();
    rebuild_mirror();
}

bool AssetLibrary::load(const std::string& path, std::vector<std::string>& msgs) {
    if (!owned_) {
        pack::StackOptions so;
        so.limits = limits;
        owned_ = std::make_shared<pack::PackStack>(context_, so);
        stack_ = owned_;
    } else {
        owned_->set_limits(limits);
    }
    pack::PackReport report;
    const bool ok = owned_->add(path, report);
    for (const pack::PackMessage& m : report.messages) {
        std::string body;
        if (!m.section.empty()) body = m.section + (m.key.empty() ? "" : "[\"" + m.key + "\"]") + ": ";
        msgs.push_back(path + ": " + body + m.text);
    }
    if (ok) packs_.push_back(path);
    rebuild_mirror();
    return ok;
}

const Replacement* AssetLibrary::graphic(uint64_t hash, int w, int h) const {
    auto it = graphics_.find(GKey{hash, w, h});
    return it == graphics_.end() ? nullptr : &it->second;
}

const FarRow* AssetLibrary::far_line(uint64_t hash) const {
    auto it = far_.find(hash);
    return it == far_.end() ? nullptr : &it->second;
}

const PaletteOverride* AssetLibrary::palette(const uint16_t* colors) const { return stack_ ? stack_->palette(colors) : nullptr; }

bool AssetLibrary::empty() const { return !stack_ || !stack_->has_graphics(); }
size_t AssetLibrary::palettes() const { return stack_ ? stack_->palette_count() : 0; }
size_t AssetLibrary::bytes() const { return stack_ ? stack_->picture_bytes() : 0; }

// --- AssetDumper ---

bool AssetDumper::open(const std::string& dir, std::string& why) {
    std::error_code ec;
    for (const char* sub : {"sprites", "tiles", "far", "flashes", "far_full"}) fs::create_directories(fs::path(dir) / sub, ec);
    if (ec) { why = "cannot create " + dir + ": " + ec.message(); return false; }
    dir_ = dir;
    open_ = true;
    return true;
}

uint32_t AssetDumper::palette_index(const uint16_t* colors) {
    std::array<uint16_t, 16> p;
    int bright = 0;
    for (int i = 0; i < 16; ++i) {
        p[size_t(i)] = colors[i] & 0xFFF;
        bright += (p[size_t(i)] >> 8) + ((p[size_t(i)] >> 4) & 15) + (p[size_t(i)] & 15);
    }
    for (size_t i = 0; i < palettes_.size(); ++i)
        if (palettes_[i] == p) return uint32_t(i);
    palettes_.push_back(p);
    brightness_.push_back(bright);
    return uint32_t(palettes_.size() - 1);
}

void AssetDumper::step(const Step& s) {
    if (!open_ || !Interpolator::gameplay(s)) return;
    std::vector<Graphic> gs;
    find_graphics(s, gs, true);
    const uint32_t pal = palette_index(s.palette);
    auto brighter = [&](uint32_t& have) { if (brightness_[pal] > brightness_[have]) have = pal; };
    FarFull* ff = nullptr;
    const int ring_lines = s.layout->far_ring_lines;
    if (ring_lines > 0 && s.level >= 1 && s.level <= 5) {
        ff = &far_full_[s.level];
        if (ff->pix.empty()) {
            ff->pix.assign(size_t(ring_lines) * kWidth, 0);
            ff->seen.assign(size_t(ring_lines), 0);
            ff->lines = ring_lines;
            ff->palette = pal;
        }
    }
    bool ff_rows = false;
    for (Graphic& g : gs) {
        if (g.kind == GfxKind::Flash) {
            char key[48];
            std::snprintf(key, sizeof key, "f:%s-%dx%d", hash_hex(g.shape).c_str(), g.w, g.h);
            auto it = entries_.find(key);
            if (it != entries_.end()) {
                ++it->second.uses;
                brighter(it->second.palette);
                continue;
            }
            if (entries_.size() >= kMaxGraphics) continue;
            ++n_flashes_;
            char png[64];
            std::snprintf(png, sizeof png, "flashes/%s-%dx%d.png", hash_hex(g.shape).c_str(), g.w, g.h);
            entries_.emplace(key, Entry{GfxKind::Flash, g.w, g.h, 1, s.swap_frame, pal, png, std::move(g.pix)});
            continue;
        }
        if (g.kind == GfxKind::Far) {
            if (ff) {
                // Ring line (ring + y) mod ring_lines is the line the layer draws at screen line y.
                const int r = ((int(s.vars[kRing]) + g.y) % ring_lines + ring_lines) % ring_lines;
                if (g.pix.size() == size_t(kWidth)) {
                    std::copy(g.pix.begin(), g.pix.end(), ff->pix.begin() + ptrdiff_t(r) * kWidth);
                    ff->seen[size_t(r)] = 1;
                    ff_rows = true;
                }
            }
            auto it = far_seen_.find(g.hash);
            if (it != far_seen_.end()) { brighter(far_[it->second].palette); continue; }
            if (far_.size() >= kMaxGraphics) continue;
            far_seen_[g.hash] = uint32_t(far_.size());
            far_.push_back({hash_hex(g.hash), pal, std::move(g.pix)});
            continue;
        }
        if (const SourceTracker::Full* full = sources_.fuller(g, s.palette)) {
            // A bottom-clipped draw of a graphic kept already: replacing that one replaces this too.
            auto it = entries_.find(graphic_key(full->hash, full->w, full->h));
            if (it != entries_.end()) { ++it->second.uses; continue; }
        }
        const std::string key = graphic_key(g.hash, g.w, g.h);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            ++it->second.uses;
            brighter(it->second.palette);
            continue;
        }
        if (entries_.size() >= kMaxGraphics) continue;
        (g.kind == GfxKind::Sprite ? n_sprites_ : n_tiles_)++;
        entries_.emplace(key, Entry{g.kind, g.w, g.h, 1, s.swap_frame, pal,
                                    std::string(g.kind == GfxKind::Sprite ? "sprites/" : "tiles/") + key + ".png", std::move(g.pix)});
    }
    if (ff && ff_rows) brighter(ff->palette);
}

size_t AssetDumper::backgrounds() const {
    size_t n = 0;
    for (const auto& kv : far_full_)
        for (uint8_t v : kv.second.seen)
            if (v) { ++n; break; }
    return n;
}

bool AssetDumper::finish(std::string& why) {
    if (!open_) return true;
    auto rgba = [&](const std::vector<uint8_t>& pix, uint32_t pal, std::vector<uint8_t>& out) {
        for (uint8_t p : pix) {
            const uint32_t c = p & 0x10 ? amiga_rgba(palettes_[pal][size_t(p & 15)]) : 0u;
            for (int k = 0; k < 4; ++k) out.push_back(uint8_t(c >> (8 * k)));
        }
    };
    for (const auto& [key, e] : entries_) {
        std::vector<uint8_t> img;
        rgba(e.pix, e.palette, img);
        if (!x2::write_png(dir_ + "/" + e.png, img, e.w, e.h, true)) ++errors_;
    }
    // Far lines in strips of up to 192 (one ring's worth), in order of first appearance.
    std::vector<std::pair<std::string, std::vector<std::string>>> far_strips_;
    for (size_t i = 0; i < far_.size(); i += 192) {
        char name[32];
        std::snprintf(name, sizeof name, "far/strip_%03zu.png", i / 192);
        std::vector<uint8_t> img;
        std::vector<std::string> lines;
        for (size_t k = i; k < far_.size() && k < i + 192; ++k) {
            rgba(far_[k].pix, far_[k].palette, img);
            lines.push_back(far_[k].key);
        }
        if (!x2::write_png(dir_ + "/" + name, img, kWidth, int(lines.size()), true)) ++errors_;
        far_strips_.push_back({name, lines});
    }
    // Each level's far layer, the ring unrolled (ring line r at picture row r); lines never seen are transparent.
    std::string backgrounds;
    for (const auto& [level, ff] : far_full_) {
        size_t seen = 0;
        for (uint8_t v : ff.seen) seen += v;
        if (!seen) continue;
        std::vector<uint8_t> img;
        for (int r = 0; r < ff.lines; ++r) {
            std::vector<uint8_t> row(ff.pix.begin() + ptrdiff_t(r) * kWidth, ff.pix.begin() + ptrdiff_t(r + 1) * kWidth);
            if (!ff.seen[size_t(r)]) std::fill(row.begin(), row.end(), uint8_t(0));
            rgba(row, ff.palette, img);
        }
        char name[48];
        std::snprintf(name, sizeof name, "far_full/level%d.png", level);
        if (!x2::write_png(dir_ + "/" + name, img, kWidth, ff.lines, true)) ++errors_;
        char entry[160];
        std::snprintf(entry, sizeof entry, "%s\n    {\"level\": %d, \"png\": \"%s\", \"scale\": 1, \"ring_lines\": %d}",
                      backgrounds.empty() ? "" : ",", level, name, ff.lines);
        backgrounds += entry;
    }
    std::string j = std::string("{\n  \"format\": \"x2-asset-pack\",\n  \"version\": ") + (extra_.empty() ? "1" : "2") + ",\n  \"name\": \"dump\",\n"
                    "  \"note\": \"Original graphics of your own copy of the game, written by x2recomp --dump-assets. "
                    "User data: do not distribute.\",\n  \"scale\": 1,\n  \"graphics\": {";
    bool first = true;
    for (const auto& [key, e] : entries_) {
        if (e.kind == GfxKind::Flash && extra_.empty()) continue; // flash keys are version 2
        char line[512];
        std::snprintf(line, sizeof line,
                      "%s\n    \"%s\": {\"png\": \"%s\", \"kind\": \"%s\", \"w\": %d, \"h\": %d, \"uses\": %u, \"first_frame\": %u, "
                      "\"palette\": %u}",
                      first ? "" : ",", key.c_str(), e.png.c_str(), e.kind == GfxKind::Sprite ? "sprite" : e.kind == GfxKind::Flash ? "flash" : "tile", e.w, e.h,
                      e.uses, e.first_frame, e.palette);
        j += line;
        first = false;
    }
    j += "\n  },\n  \"far\": [";
    for (size_t i = 0; i < far_strips_.size(); ++i) {
        j += std::string(i ? "," : "") + "\n    {\"png\": \"" + far_strips_[i].first + "\", \"lines\": [";
        for (size_t k = 0; k < far_strips_[i].second.size(); ++k) j += std::string(k ? ", " : "") + "\"" + far_strips_[i].second[k] + "\"";
        j += "]}";
    }
    j += "\n  ],\n  \"seen_palettes\": [";
    for (size_t i = 0; i < palettes_.size(); ++i) {
        j += std::string(i ? "," : "") + "\n    [";
        for (int k = 0; k < 16; ++k) {
            char c[8];
            std::snprintf(c, sizeof c, "\"#%03x\"", palettes_[i][size_t(k)]);
            j += std::string(k ? ", " : "") + c;
        }
        j += "]";
    }
    j += "\n  ],\n  \"palettes\": []";
    if (!extra_.empty() && !backgrounds.empty()) j += ",\n  \"backgrounds\": [" + backgrounds + "\n  ]";
    if (!extra_.empty()) j += ",\n  " + extra_;
    j += "\n}\n";
    std::ofstream out(dir_ + "/manifest.json", std::ios::binary);
    if (!out || !out.write(j.data(), std::streamsize(j.size()))) { why = "cannot write " + dir_ + "/manifest.json"; return false; }
    if (errors_) { why = std::to_string(errors_) + " PNG files could not be written"; return false; }
    return true;
}

} // namespace x2::modern
