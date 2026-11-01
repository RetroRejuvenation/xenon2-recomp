// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/screen_dump.h"

#include "gfx/image_io.h"
#include "pack/pack_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>

namespace x2::modern {

namespace {

namespace fs = std::filesystem;

struct DRect {
    int x, y, w, h;
};

uint32_t to_rgba(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF); }

std::string hex6(uint32_t rgb) {
    char b[16];
    std::snprintf(b, sizeof b, "\"#%06x\"", rgb & 0xFFFFFF);
    return b;
}

// The changed grid blocks as at most kMaxDynamic rectangles: horizontal runs per grid row, equal runs of consecutive rows
// joined, then, while there are too many, the two rectangles whose union costs the least extra area merged.
std::vector<DRect> dynamic_rects(const std::vector<uint8_t>& changed, int gw, int gh, int bs, bool bottom, int w, int h) {
    std::vector<DRect> r;
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw;) {
            if (!changed[size_t(y * gw + x)]) { ++x; continue; }
            int e = x;
            while (e < gw && changed[size_t(y * gw + e)]) ++e;
            // join with the rectangle ending at the previous row if it spans the same columns
            bool joined = false;
            for (DRect& q : r)
                if (q.x == x * bs && q.w == (e - x) * bs && q.y + q.h == y * bs) { q.h += bs; joined = true; break; }
            if (!joined) r.push_back({x * bs, y * bs, (e - x) * bs, bs});
            x = e;
        }
    if (bottom && h > gh * bs) r.push_back({0, gh * bs, w, h - gh * bs});
    while (int(r.size()) > ScreenDumper::kMaxDynamic) {
        size_t bi = 0, bj = 1;
        long best = std::numeric_limits<long>::max();
        for (size_t i = 0; i < r.size(); ++i)
            for (size_t j = i + 1; j < r.size(); ++j) {
                const int x0 = std::min(r[i].x, r[j].x), y0 = std::min(r[i].y, r[j].y);
                const int x1 = std::max(r[i].x + r[i].w, r[j].x + r[j].w), y1 = std::max(r[i].y + r[i].h, r[j].y + r[j].h);
                const long extra = long(x1 - x0) * (y1 - y0) - long(r[i].w) * r[i].h - long(r[j].w) * r[j].h;
                if (extra < best) { best = extra; bi = i; bj = j; }
            }
        const int x0 = std::min(r[bi].x, r[bj].x), y0 = std::min(r[bi].y, r[bj].y);
        const int x1 = std::max(r[bi].x + r[bi].w, r[bj].x + r[bj].w), y1 = std::max(r[bi].y + r[bi].h, r[bj].y + r[bj].h);
        r[bi] = {x0, y0, x1 - x0, y1 - y0};
        r.erase(r.begin() + long(bj));
    }
    return r;
}

double frame_brightness(const IndexedFrame& f) {
    double sum = 0;
    uint32_t count[32];
    for (int y = 0; y < f.h; ++y) {
        std::fill(count, count + 32, 0u);
        const uint8_t* row = f.index + size_t(y) * size_t(f.stride);
        for (int x = 0; x < f.w; ++x) ++count[row[x] & 31];
        const uint32_t* pal = f.pal + size_t(y) * size_t(f.colours);
        for (int i = 0; i < 32 && i < f.colours; ++i)
            if (count[i]) sum += double(count[i]) * double((pal[i] & 255) + ((pal[i] >> 8) & 255) + ((pal[i] >> 16) & 255));
    }
    return sum;
}

} // namespace

void ScreenDumper::grid_of(const IndexedFrame& f, std::vector<uint64_t>& hashes, std::vector<uint8_t>& flat) const {
    hashes.resize(size_t(gw_) * size_t(gh_));
    flat.resize(hashes.size());
    for (int by = 0; by < gh_; ++by)
        for (int bx = 0; bx < gw_; ++bx) {
            const int x = bx * kBlock, y = by * kBlock;
            hashes[size_t(by * gw_ + bx)] = pack::block_hash_region(f.index, f.stride, x, y, kBlock, kBlock);
            const uint8_t v = f.at(x, y);
            bool fl = true;
            for (int yy = 0; yy < kBlock && fl; ++yy) {
                const uint8_t* row = f.index + size_t(y + yy) * size_t(f.stride) + size_t(x);
                for (int xx = 0; xx < kBlock; ++xx)
                    if (row[xx] != v) { fl = false; break; }
            }
            flat[size_t(by * gw_ + bx)] = fl ? 1 : 0;
        }
}

void ScreenDumper::add_gameplay_sample(const std::vector<uint64_t>& hashes) {
    if (gameplay_seen_++ % 16 == 0 && gameplay_grids_.size() < kMaxGameplayFrames) gameplay_grids_.push_back(hashes);
}

void ScreenDumper::frame(const IndexedFrame& f, uint32_t frame_no, bool in_game, bool gameplay) {
    if (!f.valid()) return;
    if (gw_ == 0) {
        w_ = f.w;
        h_ = f.h;
        colours_ = f.colours;
        gw_ = f.w / kBlock;
        gh_ = f.h / kBlock;
    }
    if (f.w != w_ || f.h != h_) return;
    grid_of(f, cur_hash_, cur_flat_);
    if (gameplay) {
        close_life();
        have_prev_ = false;
        add_gameplay_sample(cur_hash_);
    } else {
        const uint64_t bottom = h_ > gh_ * kBlock ? pack::block_hash_region(f.index, f.stride, 0, gh_ * kBlock, w_, h_ - gh_ * kBlock) : 0;
        bool cut = !life_.open;
        if (!cut && have_prev_) {
            int nonflat = 0, diff = 0;
            for (size_t b = 0; b < cur_hash_.size(); ++b)
                if (!cur_flat_[b] || !prev_flat_[b]) {
                    ++nonflat;
                    diff += cur_hash_[b] != prev_hash_[b];
                }
            cut = nonflat > 0 && diff * 100 > 40 * nonflat;
        }
        if (cut) {
            close_life();
            life_ = Life{};
            life_.open = true;
            life_.first_frame = frame_no;
            life_.first_hash = cur_hash_;
            life_.flat = cur_flat_;
            life_.changed.assign(cur_hash_.size(), 0);
            life_.bottom_first = bottom;
        } else {
            for (size_t b = 0; b < cur_hash_.size(); ++b)
                if (cur_hash_[b] != life_.first_hash[b]) life_.changed[b] = 1;
            if (bottom != life_.bottom_first) life_.bottom_changed = true;
        }
        ++life_.frames;
        const double br = frame_brightness(f);
        if (br > life_.brightness) {
            life_.brightness = br;
            life_.best_frame = frame_no;
            life_.index.resize(size_t(w_) * size_t(h_));
            for (int y = 0; y < h_; ++y) std::copy(f.index + size_t(y) * size_t(f.stride), f.index + size_t(y) * size_t(f.stride) + w_, life_.index.begin() + long(y * w_));
            life_.pal.assign(f.pal, f.pal + size_t(h_) * size_t(colours_));
        }
        prev_hash_ = cur_hash_;
        prev_flat_ = cur_flat_;
        have_prev_ = true;
    }
    if (in_game) collect_panel(f, frame_no);
}

void ScreenDumper::close_life() {
    if (!life_.open) return;
    life_.open = false;
    if (life_.frames < unsigned(kMinFrames)) {
        ++transitions_;
        return;
    }
    std::vector<int> stable;
    int changed_blocks = 0;
    for (size_t b = 0; b < life_.changed.size(); ++b) {
        changed_blocks += life_.changed[b];
        if (!life_.changed[b] && !life_.flat[b]) stable.push_back(int(b));
    }
    if (int(stable.size()) < kMinStable) {
        ++not_matchable_;
        return;
    }
    if (changed_blocks * 100 > int(life_.changed.size()) * 85) {
        ++not_matchable_;
        char b[160];
        std::snprintf(b, sizeof b, "frames %u-%u: mostly changing (%d of %zu blocks), not a screen", life_.first_frame,
                      life_.first_frame + life_.frames - 1, changed_blocks, life_.changed.size());
        warnings_.push_back(b);
        return;
    }
    // Up to kMaxSignature stable blocks, each the farthest from those chosen already (the first is the lowest-numbered one).
    std::vector<int> pick;
    pick.push_back(stable[0]);
    std::vector<long> dist(stable.size(), std::numeric_limits<long>::max());
    while (int(pick.size()) < kMaxSignature && pick.size() < stable.size()) {
        const int px = pick.back() % gw_, py = pick.back() / gw_;
        long far = -1;
        size_t far_i = 0;
        for (size_t i = 0; i < stable.size(); ++i) {
            const long dx = stable[i] % gw_ - px, dy = stable[i] / gw_ - py;
            dist[i] = std::min(dist[i], dx * dx + dy * dy);
            if (dist[i] > far) { far = dist[i]; far_i = i; }
        }
        if (far <= 0) break;
        pick.push_back(stable[far_i]);
    }
    std::sort(pick.begin(), pick.end());
    Shot s;
    s.block = kBlock;
    s.min = int(pick.size());   // every block: the blocks that ever changed are not in the signature, so nothing needs to be tolerated
    uint64_t sig = pack::kFnvOffset;
    const uint8_t head[2] = {uint8_t(s.block), uint8_t(s.min)};
    sig = pack::fnv1a64(head, 2, sig);
    for (int b : pick) {
        const int x = (b % gw_) * kBlock, y = (b / gw_) * kBlock;
        s.pos.push_back({x, y});
        s.hash.push_back(life_.first_hash[size_t(b)]);
        const uint8_t xy[4] = {uint8_t(x & 255), uint8_t(x >> 8), uint8_t(y & 255), uint8_t(y >> 8)};
        sig = pack::fnv1a64(xy, 4, sig);
        uint8_t hb[8];
        for (int k = 0; k < 8; ++k) hb[k] = uint8_t(life_.first_hash[size_t(b)] >> (8 * k));
        sig = pack::fnv1a64(hb, 8, sig);
    }
    s.name = "screen_" + pack::hash_hex(sig).substr(0, 8);
    s.first_frame = life_.first_frame;
    s.frames = life_.frames;
    s.uses = 1;
    s.best_frame = life_.best_frame;
    s.brightness = life_.brightness;
    s.changed = life_.changed;
    s.bottom_changed = life_.bottom_changed;
    s.index = life_.index;
    s.pal = life_.pal;
    auto it = screens_.find(s.name);
    if (it == screens_.end()) {
        if (screens_.size() < kMaxScreens) screens_.emplace(s.name, std::move(s));
        return;
    }
    Shot& old = it->second;
    ++old.uses;
    old.frames += s.frames;
    for (size_t b = 0; b < old.changed.size(); ++b) old.changed[b] |= s.changed[b];
    old.bottom_changed = old.bottom_changed || s.bottom_changed;
    if (s.brightness > old.brightness) {
        old.brightness = s.brightness;
        old.best_frame = s.best_frame;
        old.index = std::move(s.index);
        old.pal = std::move(s.pal);
    }
}

void ScreenDumper::collect_panel(const IndexedFrame& f, uint32_t frame_no) {
    const int first = panel_.first_line, lines = panel_.lines, cw = panel_.cell_w;
    if (!panel_.known() || lines <= 0 || cw <= 0 || first < 0 || first + lines > f.h) return;
    bool blank = true;
    for (int y = 0; y < lines && blank; ++y)
        for (int x = 0; x < f.w; ++x)
            if (f.at(x, first + y)) { blank = false; break; }
    if (blank) return;
    {
        const uint32_t* pal = f.pal + size_t(first) * size_t(f.colours);
        std::vector<uint32_t> key(pal, pal + f.colours);
        auto [it, fresh] = strip_pals_.try_emplace(std::move(key));
        ++it->second.frames;
        if (fresh) it->second.lines.assign(f.pal + size_t(first) * size_t(f.colours), f.pal + size_t(first + lines) * size_t(f.colours));
    }
    for (int c = 0; c < f.w / cw; ++c) {
        const uint64_t h = pack::block_hash_region(f.index, f.stride, c * cw, first, cw, lines);
        auto it = cells_.find(h);
        if (it != cells_.end()) {
            ++it->second.uses;
            continue;
        }
        if (cells_.size() >= size_t(kMaxCells)) continue;
        Cell cell;
        cell.index.resize(size_t(cw) * size_t(lines));
        for (int y = 0; y < lines; ++y)
            for (int x = 0; x < cw; ++x) cell.index[size_t(y * cw + x)] = f.at(c * cw + x, first + y);
        cell.uses = 1;
        cell.first_frame = frame_no;
        cells_.emplace(h, std::move(cell));
    }
}

bool ScreenDumper::finish(const std::string& dir, std::string& members, std::string& why) {
    close_life();
    members.clear();
    std::error_code ec;
    // Block hashes of each screen's own frame, to see which signatures also match something else.
    std::map<std::string, std::vector<uint64_t>> grids;
    for (auto& [name, s] : screens_) {
        std::vector<uint64_t> g(size_t(gw_) * size_t(gh_));
        for (int by = 0; by < gh_; ++by)
            for (int bx = 0; bx < gw_; ++bx)
                g[size_t(by * gw_ + bx)] = pack::block_hash_region(s.index.data(), w_, bx * kBlock, by * kBlock, kBlock, kBlock);
        grids[name] = std::move(g);
    }
    auto matches = [&](const Shot& a, const std::vector<uint64_t>& grid) {
        int m = 0;
        for (size_t i = 0; i < a.pos.size(); ++i)
            m += grid[size_t(a.pos[i][1] / kBlock * gw_ + a.pos[i][0] / kBlock)] == a.hash[i];
        return m;
    };
    // A screen collides with another when the other's whole signature matches its picture: the matcher takes the better share, and
    // an equal one goes to the later screen of the manifest.
    for (const auto& [name, a] : screens_) {
        for (const auto& kv : grids) {
            const std::string& other = kv.first;
            if (other == name) continue;
            const Shot& b = screens_.at(other);
            if (matches(b, grids.at(name)) == int(b.pos.size()))
                warnings_.push_back(other + " matches the whole picture of " + name + " (it will take it over when it comes later in the manifest)");
        }
        for (size_t i = 0; i < gameplay_grids_.size(); ++i)
            if (matches(a, gameplay_grids_[i]) >= a.min) {
                warnings_.push_back(name + " matches a sampled gameplay frame: it would replace play, drop it from the pack");
                break;
            }
    }
    if (screens_.empty() && cells_.empty()) return true;
    std::string j;
    if (!screens_.empty()) {
        fs::create_directories(fs::path(dir) / "screens", ec);
        if (ec) { why = "cannot create " + dir + "/screens"; return false; }
        j += "\"screens\": {";
        bool first = true;
        for (const auto& [name, s] : screens_) {
            const std::vector<DRect> dyn = dynamic_rects(s.changed, gw_, gh_, kBlock, s.bottom_changed, w_, h_);
            // The picture: the brightest frame, with the parts the game keeps drawing replaced by the background colour (index 0).
            std::vector<uint8_t> mask(size_t(w_) * size_t(h_), 0);
            for (const DRect& r : dyn)
                for (int y = r.y; y < std::min(h_, r.y + r.h); ++y)
                    for (int x = r.x; x < std::min(w_, r.x + r.w); ++x) mask[size_t(y * w_ + x)] = 1;
            std::vector<uint8_t> rgba(size_t(w_) * size_t(h_) * 4);
            std::vector<double> sum(size_t(colours_) * 3, 0.0);
            std::vector<double> cnt(size_t(colours_), 0.0);
            for (int y = 0; y < h_; ++y) {
                const uint32_t* pal = &s.pal[size_t(y) * size_t(colours_)];
                for (int x = 0; x < w_; ++x) {
                    const bool dynp = mask[size_t(y * w_ + x)];
                    const int idx = dynp ? 0 : s.index[size_t(y * w_ + x)] % colours_;
                    const uint32_t c = to_rgba(pal[idx]);
                    for (int k = 0; k < 4; ++k) rgba[(size_t(y) * size_t(w_) + size_t(x)) * 4 + size_t(k)] = uint8_t(c >> (8 * k));
                    if (!dynp) {
                        cnt[size_t(idx)] += 1;
                        for (int ch = 0; ch < 3; ++ch) sum[size_t(idx) * 3 + size_t(ch)] += double((pal[idx] >> (16 - 8 * ch)) & 255);
                    }
                }
            }
            const std::string rel = "screens/" + name + ".png";
            if (!x2::write_png(dir + "/" + rel, rgba, w_, h_, false)) { why = "cannot write " + dir + "/" + rel; return false; }
            j += std::string(first ? "" : ",") + "\n    \"" + name + "\": {\"signature\": {\"block\": " + std::to_string(s.block) +
                 ", \"min\": " + std::to_string(s.min) + ", \"blocks\": [";
            for (size_t i = 0; i < s.pos.size(); ++i)
                j += std::string(i ? ", " : "") + "{\"x\": " + std::to_string(s.pos[i][0]) + ", \"y\": " + std::to_string(s.pos[i][1]) +
                     ", \"hash\": \"" + pack::hash_hex(s.hash[i]) + "\"}";
            j += "]}, \"png\": \"" + rel + "\", \"ref_palette\": [";
            for (int i = 0; i < colours_; ++i) {
                uint32_t v;
                if (cnt[size_t(i)] > 0) {
                    v = 0;
                    for (int ch = 0; ch < 3; ++ch)
                        v |= uint32_t(std::clamp(int(sum[size_t(i) * 3 + size_t(ch)] / cnt[size_t(i)] + 0.5), 0, 255)) << (16 - 8 * ch);
                } else {
                    v = s.pal[size_t(i)];   // unused index: the first line's colour
                }
                j += std::string(i ? ", " : "") + hex6(v);
            }
            j += "], \"fade\": \"scale\", \"bg_index\": 0";
            if (!dyn.empty()) {
                j += ", \"dynamic\": [";
                for (size_t i = 0; i < dyn.size(); ++i)
                    j += std::string(i ? ", " : "") + "{\"x\": " + std::to_string(dyn[i].x) + ", \"y\": " + std::to_string(dyn[i].y) +
                         ", \"w\": " + std::to_string(dyn[i].w) + ", \"h\": " + std::to_string(dyn[i].h) + "}";
                j += "]";
            }
            j += ", \"first_frame\": " + std::to_string(s.first_frame) + ", \"frames\": " + std::to_string(s.frames) +
                 ", \"uses\": " + std::to_string(s.uses) + "}";
            first = false;
        }
        j += "\n  }";
    }
    if (!cells_.empty()) {
        fs::create_directories(fs::path(dir) / "panel", ec);
        if (ec) { why = "cannot create " + dir + "/panel"; return false; }
        // The palette the strip had in most frames paints the pictures and is the reference of the fade.
        const StripPalette* best = nullptr;
        const std::vector<uint32_t>* best_key = nullptr;
        for (const auto& [key, sp] : strip_pals_)
            if (!best || sp.frames > best->frames) { best = &sp; best_key = &key; }
        const int colours = int(best_key->size());
        j += std::string(j.empty() ? "" : ",\n  ") + "\"panel\": {\"scale\": 1, \"cell_w\": " + std::to_string(panel_.cell_w) +
             ", \"max_span\": " + std::to_string(panel_.max_span) + ", \"ref_palette\": [";
        for (int i = 0; i < colours; ++i) j += std::string(i ? ", " : "") + hex6((*best_key)[size_t(i)]);
        j += "], \"fade\": \"scale\", \"cells\": {";
        bool first = true;
        for (const auto& [h, c] : cells_) {
            const std::string key = pack::panel_key(h, 1);
            const std::string rel = "panel/" + key + ".png";
            std::vector<uint8_t> rgba(size_t(panel_.cell_w) * size_t(panel_.lines) * 4);
            for (int y = 0; y < panel_.lines; ++y)
                for (int x = 0; x < panel_.cell_w; ++x) {
                    const uint32_t px = to_rgba(best->lines[size_t(y) * size_t(colours) + c.index[size_t(y * panel_.cell_w + x)] % colours]);
                    for (int k = 0; k < 4; ++k) rgba[(size_t(y) * size_t(panel_.cell_w) + size_t(x)) * 4 + size_t(k)] = uint8_t(px >> (8 * k));
                }
            if (!x2::write_png(dir + "/" + rel, rgba, panel_.cell_w, panel_.lines, false)) { why = "cannot write " + dir + "/" + rel; return false; }
            j += std::string(first ? "" : ",") + "\n    \"" + key + "\": {\"png\": \"" + rel + "\", \"uses\": " + std::to_string(c.uses) + "}";
            first = false;
        }
        j += "\n  }}";
    }
    members = j;
    return true;
}

} // namespace x2::modern
