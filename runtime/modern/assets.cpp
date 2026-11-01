// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/assets.h"

#include "gfx/image_io.h"
#include "import/importer.h"
#include "import/sha1.h"
#include "machine.h"
#include "modern/interp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

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
bool copy_blit(const BlitCmd& b, const CaptureLayout& L) {
    return b.data != ~0u && (b.con0 & 0x0F00) == 0x0900 && (b.con0 & 0xFF) == 0xF0 && !(b.con1 & 2) && !L.far_copy_pc(b.pc);
}

// Pixels a group of blits writes, merged: per (line, x) the planes written, whether it is drawn, and its colour.
struct Cells {
    int line0 = 0, lines = 0;
    std::vector<uint16_t> c; // bit 8-11 planes written, bit 4 drawn, bits 0-3 colour
    uint16_t& at(int line, int x) { return c[size_t(line - line0) * kWidth + size_t(x)]; }
};

// Merges blits[first..last] into a Graphic; false if it is not a complete 4-plane picture.
bool merge(const Step& s, uint32_t first, uint32_t last, bool cookie, bool keep_pixels, Graphic& g) {
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
            const uint16_t mask = cookie ? wd[0] : 0xFFFF, bits = cookie ? wd[1] : wd[0];
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
    uint64_t h = kFnvBasis;
    for (int v : {g.w, g.h}) { fnv(h, uint8_t(v >> 8)); fnv(h, uint8_t(v)); }
    if (keep_pixels) g.pix.resize(size_t(g.w) * size_t(g.h));
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const uint16_t c = cells.c[size_t(y0 + y) * kWidth + size_t(x0 + x)];
            const uint8_t p = c & 0x10 ? uint8_t(c & 0x1F) : 0;
            fnv(h, p);
            if (keep_pixels) g.pix[size_t(y) * size_t(g.w) + size_t(x)] = p;
        }
    g.hash = h;
    return true;
}

// --- JSON (manifest.json): a small strict parser ---

// Object members are a struct, not std::pair<std::string, Json>: std::pair needs a complete Json here, which
// libstdc++ enforces (Clang with libstdc++ rejects the pair form); std::vector allows an incomplete element type.
struct JsonMember;
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<JsonMember> o;
    const Json* get(const char* k) const;
};
struct JsonMember {
    std::string first;
    Json second;
};
const Json* Json::get(const char* k) const {
    for (auto it = o.rbegin(); it != o.rend(); ++it)
        if (it->first == k) return &it->second;
    return nullptr;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out, std::string& why) {
        bool ok = value(out, 0);
        ws();
        if (ok && p_ != t_.size()) { err_ = "unexpected text after the document"; ok = false; }
        if (!ok) why = err_ + " at offset " + std::to_string(p_);
        return ok;
    }

private:
    void ws() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_; }
    bool fail(const char* w) { err_ = w; return false; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    bool string(std::string& out) {
        if (p_ >= t_.size() || t_[p_] != '"') return fail("expected a string");
        ++p_;
        while (p_ < t_.size() && t_[p_] != '"') {
            char c = t_[p_++];
            if (uint8_t(c) < 0x20) return fail("control character in a string");
            if (c != '\\') { out += c; continue; }
            if (p_ >= t_.size()) break;
            c = t_[p_++];
            switch (c) {
            case '"': case '\\': case '/': out += c; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (p_ + 4 > t_.size()) return fail("bad \\u escape");
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = t_[p_++];
                    v = v * 16 + unsigned(std::isdigit(uint8_t(h)) ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : 99);
                    if (v > 0xFFFF) return fail("bad \\u escape");
                }
                // UTF-8 (surrogate pairs are kept as two code points; names only need to round-trip).
                if (v < 0x80) out += char(v);
                else if (v < 0x800) { out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 63)); }
                else { out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 63)); out += char(0x80 | (v & 63)); }
                break;
            }
            default: return fail("bad escape in a string");
            }
        }
        if (p_ >= t_.size()) return fail("unterminated string");
        ++p_;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > 32) return fail("nested too deeply");
        ws();
        if (p_ >= t_.size()) return fail("unexpected end");
        const char c = t_[p_];
        if (c == '{') {
            v.t = Json::Obj;
            ++p_;
            ws();
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws();
                JsonMember m;
                if (!string(m.first)) return false;
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') return fail("expected ':'");
                ++p_;
                if (!value(m.second, depth + 1)) return false;
                v.o.push_back(std::move(m));
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.t = Json::Arr;
            ++p_;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = Json::Str; return string(v.s); }
        if (lit("true")) { v.t = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.t = Json::Bool; return true; }
        if (lit("null")) return true;
        if (c == '-' || std::isdigit(uint8_t(c))) {
            const char* begin = t_.c_str() + p_;
            char* end = nullptr;
            v.n = std::strtod(begin, &end);
            if (end == begin) return fail("bad number");
            p_ += size_t(end - begin);
            v.t = Json::Num;
            return true;
        }
        return fail("unexpected character");
    }
    const std::string& t_;
    size_t p_ = 0;
    std::string err_;
};

bool parse_hex64(const std::string& s, uint64_t& out) {
    if (s.size() != 16) return false;
    out = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        out = out << 4 | uint64_t(d);
    }
    return true;
}

// "#rgb" (an Amiga 12-bit colour) -> 0x0RGB; "#rrggbb" -> 0xAABBGGRR with alpha FF.
bool parse_colour(const std::string& s, bool amiga, uint32_t& out) {
    if (s.size() != (amiga ? 4u : 7u) || s[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = char(s[i] | 0x20);
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | uint32_t(d);
    }
    out = amiga ? v : 0xFF000000u | (v >> 16) | (v & 0xFF00) | ((v & 0xFF) << 16);
    return true;
}

bool int_in(const Json* j, int lo, int hi, int& out) {
    if (!j || j->t != Json::Num || j->n != std::floor(j->n) || j->n < lo || j->n > hi) return false;
    out = int(j->n);
    return true;
}

uint32_t premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    auto m = [a](uint8_t c) { return uint32_t((unsigned(c) * a + 127) / 255); };
    return m(r) | m(g) << 8 | m(b) << 16 | uint32_t(a) << 24;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint32_t amiga_rgba(uint16_t c) {
    return uint32_t(((c >> 8) & 15) * 17) | uint32_t(((c >> 4) & 15) * 17) << 8 | uint32_t((c & 15) * 17) << 16 | 0xFF000000u;
}

bool read_bytes(const fs::path& p, size_t limit, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) { why = "cannot read"; return false; }
    if (size > limit) { why = "too large (" + std::to_string(size) + " bytes, limit " + std::to_string(limit) + ")"; return false; }
    std::ifstream in(p, std::ios::binary);
    out.resize(size_t(size));
    if (!in || !in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) { why = "cannot read"; return false; }
    return true;
}

bool within(const fs::path& p, const fs::path& root) {
    auto r = root.begin(), q = p.begin();
    for (; r != root.end(); ++r, ++q)
        if (q == p.end() || *q != *r) return false;
    return true;
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

bool safe_pack_path(const std::string& p) {
    if (p.empty() || p.size() > 255 || p[0] == '/') return false;
    if (p.find('\\') != std::string::npos || p.find(':') != std::string::npos || p.find('\0') != std::string::npos) return false;
    std::stringstream ss(p);
    for (std::string part; std::getline(ss, part, '/');)
        if (part.empty() || part == "." || part == "..") return false;
    return p.back() != '/';
}

bool decode_png(const std::vector<uint8_t>& f, Image& out, std::string& why, int max_side, size_t max_pixels) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (f.size() < 8 || std::memcmp(f.data(), kSig, 8) != 0) { why = "not a PNG file"; return false; }
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    bool ihdr = false, iend = false;
    for (size_t p = 8; p + 12 <= f.size() && !iend;) {
        const uint32_t len = be32(&f[p]);
        if (len > f.size() - p - 12) { why = "truncated chunk"; return false; }
        const uint8_t* type = &f[p + 4];
        const uint8_t* d = &f[p + 8];
        if (x2::import::crc32(type, len + 4) != be32(d + len)) { why = "chunk CRC mismatch (corrupted file)"; return false; }
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len != 13) { why = "bad IHDR"; return false; }
            w = be32(d);
            h = be32(d + 4);
            depth = d[8];
            ctype = d[9];
            interlace = d[12];
            if (d[10] || d[11]) { why = "unknown compression or filter method"; return false; }
            // Size limits before anything is inflated.
            if (!w || !h || w > uint32_t(max_side) || h > uint32_t(max_side) || size_t(w) * h > max_pixels) {
                why = "picture too large or empty (" + std::to_string(w) + "x" + std::to_string(h) + ", limit " +
                      std::to_string(max_side) + " per side, " + std::to_string(max_pixels) + " pixels)";
                return false;
            }
            ihdr = true;
        } else if (!ihdr) {
            why = "IHDR is not the first chunk";
            return false;
        } else if (!std::memcmp(type, "PLTE", 4)) plte.assign(d, d + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(d, d + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
        else if (!std::memcmp(type, "IEND", 4)) iend = true;
        p += 12 + len;
    }
    if (!ihdr || idat.empty()) { why = "no image data"; return false; }
    if (interlace) { why = "interlaced PNGs are not supported"; return false; }
    int channels = 0;
    switch (ctype) {
    case 0: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) channels = 0; break;
    case 2: channels = 3; if (depth != 8 && depth != 16) channels = 0; break;
    case 3: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) channels = 0; break;
    case 4: channels = 2; if (depth != 8 && depth != 16) channels = 0; break;
    case 6: channels = 4; if (depth != 8 && depth != 16) channels = 0; break;
    default: break;
    }
    if (!channels) { why = "unsupported colour type / bit depth"; return false; }
    if (ctype == 3 && (plte.empty() || plte.size() % 3)) { why = "palette missing"; return false; }
    const size_t bits = size_t(channels) * size_t(depth), row = (size_t(w) * bits + 7) / 8, bpp = std::max<size_t>(1, bits / 8);
    // zlib stream: CM 8, no preset dictionary.
    if (idat.size() < 6 || (idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 || (idat[1] & 0x20)) {
        why = "bad zlib stream";
        return false;
    }
    std::vector<uint8_t> raw;
    if (!x2::import::inflate_raw(&idat[2], idat.size() - 6, size_t(h) * (row + 1), raw)) { why = "corrupted image data"; return false; }
    // Undo the row filters in place.
    std::vector<uint8_t> prev(row, 0);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* r = &raw[size_t(y) * (row + 1)];
        const uint8_t ft = r[0];
        uint8_t* c = r + 1;
        for (size_t i = 0; i < row; ++i) {
            const int a = i >= bpp ? c[i - bpp] : 0, b = prev[i], d = i >= bpp ? prev[i - bpp] : 0;
            int v = c[i];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - d, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - d);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : d;
                break;
            }
            default: why = "bad row filter"; return false;
            }
            c[i] = uint8_t(v);
        }
        std::memcpy(prev.data(), c, row);
    }
    out.w = int(w);
    out.h = int(h);
    out.px.assign(size_t(w) * h, 0);
    auto sample = [&](const uint8_t* r, size_t idx) -> unsigned { // channel sample idx of a row, native depth
        if (depth == 8) return r[idx];
        if (depth == 16) return unsigned(r[2 * idx]) << 8 | r[2 * idx + 1];
        const size_t bit = idx * size_t(depth);
        return (r[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    };
    auto to8 = [&](unsigned v) -> uint8_t {
        if (depth == 16) return uint8_t(v >> 8);
        if (depth == 8 || ctype == 3) return uint8_t(v);
        return uint8_t(v * 255 / ((1u << depth) - 1));
    };
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* r = &raw[size_t(y) * (row + 1) + 1];
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t R, G, B, A = 255;
            if (ctype == 0) {
                const unsigned v = sample(r, x);
                R = G = B = to8(v);
                if (trns.size() >= 2 && v == (unsigned(trns[0]) << 8 | trns[1])) A = 0;
            } else if (ctype == 2) {
                const unsigned r16 = sample(r, 3 * x), g16 = sample(r, 3 * x + 1), b16 = sample(r, 3 * x + 2);
                R = to8(r16);
                G = to8(g16);
                B = to8(b16);
                if (trns.size() >= 6 && r16 == (unsigned(trns[0]) << 8 | trns[1]) && g16 == (unsigned(trns[2]) << 8 | trns[3]) &&
                    b16 == (unsigned(trns[4]) << 8 | trns[5]))
                    A = 0;
            } else if (ctype == 3) {
                const unsigned i = sample(r, x);
                if (i * 3 + 2 >= plte.size()) { why = "palette index out of range"; return false; }
                R = plte[i * 3];
                G = plte[i * 3 + 1];
                B = plte[i * 3 + 2];
                if (i < trns.size()) A = trns[i];
            } else if (ctype == 4) {
                R = G = B = to8(sample(r, 2 * x));
                A = to8(sample(r, 2 * x + 1));
            } else {
                R = to8(sample(r, 4 * x));
                G = to8(sample(r, 4 * x + 1));
                B = to8(sample(r, 4 * x + 2));
                A = to8(sample(r, 4 * x + 3));
            }
            out.px[size_t(y) * w + x] = premultiply(R, G, B, A);
        }
    }
    return true;
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

const Replacement* AssetLibrary::graphic(uint64_t hash, int w, int h) const {
    auto it = graphics_.find(graphic_key(hash, w, h));
    return it == graphics_.end() ? nullptr : &it->second;
}

const FarRow* AssetLibrary::far_line(uint64_t hash) const {
    auto it = far_.find(hash);
    return it == far_.end() ? nullptr : &it->second;
}

const PaletteOverride* AssetLibrary::palette(const uint16_t* colors) const {
    for (auto it = palettes_.rbegin(); it != palettes_.rend(); ++it) {
        bool same = true;
        for (int i = 0; i < 16 && same; ++i) same = it->match[i] == (colors[i] & 0xFFF);
        if (same) return &*it;
    }
    return nullptr;
}

bool AssetLibrary::load(const std::string& path, std::vector<std::string>& msgs) {
    auto note = [&](const std::string& what) { msgs.push_back(path + ": " + what); };
    auto fail = [&](const std::string& what) { note(what); return false; };
    std::error_code ec;
    std::function<bool(const std::string&, std::vector<uint8_t>&, std::string&)> read;
    fs::path root;
    std::vector<x2::import::ZipEntry> zip;
    std::string prefix;
    if (fs::is_directory(path, ec)) {
        root = fs::canonical(path, ec);
        if (ec) return fail("cannot open the directory");
        read = [&](const std::string& rel, std::vector<uint8_t>& out, std::string& why) {
            if (!safe_pack_path(rel)) { why = "unsafe path '" + rel + "' (relative paths inside the pack only)"; return false; }
            std::error_code e;
            const fs::path p = fs::weakly_canonical(root / fs::path(rel), e);
            if (e || !within(p, root)) { why = "'" + rel + "' leaves the pack directory"; return false; }
            if (!fs::is_regular_file(p, e)) { why = "missing file '" + rel + "'"; return false; }
            if (!read_bytes(p, limits.max_file, out, why)) { why = "'" + rel + "': " + why; return false; }
            return true;
        };
    } else if (fs::is_regular_file(path, ec)) {
        std::vector<uint8_t> data;
        std::string why;
        if (!read_bytes(path, limits.max_zip, data, why)) return fail(why);
        if (!x2::import::unzip(data, zip, why, limits.max_zip)) return fail("not a usable ZIP file: " + why);
        // manifest.json at the root, or in a single top-level folder.
        bool root_manifest = false;
        std::vector<std::string> nested;
        for (const auto& e : zip) {
            if (e.name == "manifest.json") root_manifest = true;
            auto slash = e.name.find('/');
            if (slash != std::string::npos && e.name.compare(slash, std::string::npos, "/manifest.json") == 0) nested.push_back(e.name.substr(0, slash + 1));
        }
        if (!root_manifest) {
            if (nested.size() != 1) return fail("no manifest.json in the ZIP file");
            prefix = nested[0];
        }
        read = [&](const std::string& rel, std::vector<uint8_t>& out, std::string& why) {
            if (!safe_pack_path(rel)) { why = "unsafe path '" + rel + "' (relative paths inside the pack only)"; return false; }
            for (const auto& e : zip) {
                if (e.name != prefix + rel) continue;
                if (!e.problem.empty()) { why = "'" + rel + "': " + e.problem; return false; }
                if (e.data.size() > limits.max_file) { why = "'" + rel + "' is too large"; return false; }
                out = e.data;
                return true;
            }
            why = "missing file '" + rel + "'";
            return false;
        };
    } else {
        return fail("no such asset pack (a directory or a .zip file)");
    }

    std::vector<uint8_t> mbytes;
    std::string why;
    if (!read("manifest.json", mbytes, why)) return fail(why);
    Json m;
    if (!JsonParser(std::string(mbytes.begin(), mbytes.end())).parse(m, why)) return fail("manifest.json: " + why);
    const Json* fmt = m.get("format");
    if (m.t != Json::Obj || !fmt || fmt->t != Json::Str || fmt->s != "x2-asset-pack") return fail("manifest.json: \"format\" must be \"x2-asset-pack\"");
    int version = 0, pack_scale = 1;
    if (!int_in(m.get("version"), 1, 2, version)) return fail("manifest.json: unsupported \"version\" (this runtime reads versions 1 and 2)");
    if (m.get("scale") && !int_in(m.get("scale"), 1, 8, pack_scale)) return fail("manifest.json: \"scale\" must be 1-8");

    // Pictures, decoded once per file. A failed one is remembered so its error is reported once per entry.
    std::map<std::string, std::pair<const Image*, std::string>> pictures;
    auto picture = [&](const std::string& rel, std::string& err) -> const Image* {
        auto it = pictures.find(rel);
        if (it == pictures.end()) {
            std::vector<uint8_t> bytes;
            std::string e;
            auto img = std::make_unique<Image>();
            const Image* got = nullptr;
            if (!read(rel, bytes, e)) {
            } else if (!decode_png(bytes, *img, e, limits.max_side, limits.max_pixels)) {
                e = "'" + rel + "': " + e;
            } else if (bytes_ + img->px.size() * 4 > limits.max_total) {
                e = "'" + rel + "': over the memory limit for all packs (" + std::to_string(limits.max_total >> 20) + " MB)";
            } else {
                bytes_ += img->px.size() * 4;
                images_.push_back(std::move(img));
                got = images_.back().get();
            }
            it = pictures.emplace(rel, std::make_pair(got, e)).first;
        }
        err = it->second.second;
        return it->second.first;
    };
    auto png_of = [&](const Json& e, std::string& err) -> const Image* {
        const Json* p = e.get("png");
        if (!p || p->t != Json::Str) { err = "\"png\" missing"; return nullptr; }
        return picture(p->s, err);
    };

    size_t entries = 0;
    auto room = [&]() {
        if (++entries <= limits.max_entries) return true;
        if (entries == limits.max_entries + 1) note("more than " + std::to_string(limits.max_entries) + " entries: the rest are ignored");
        return false;
    };
    size_t n_graphics = 0, n_far = 0, n_pal = 0;
    if (const Json* g = m.get("graphics")) {
        if (g->t != Json::Obj) return fail("manifest.json: \"graphics\" must be an object");
        for (const auto& [key, e] : g->o) {
            if (!room()) break;
            const std::string where = "graphics[\"" + key + "\"]: ";
            uint64_t hash = 0;
            int w = 0, h = 0;
            const auto dash = key.find('-'), x = key.find('x', dash == std::string::npos ? 0 : dash);
            if (dash != 16 || x == std::string::npos || !parse_hex64(key.substr(0, 16), hash) ||
                std::sscanf(key.c_str() + 17, "%dx%d", &w, &h) != 2 || w < 1 || h < 1 || w > 1024 || h > 1024 ||
                graphic_key(hash, w, h) != key) {
                note(where + "the key must be \"<16 hex digits>-<width>x<height>\" as written by --dump-assets");
                continue;
            }
            if (e.t != Json::Obj) { note(where + "must be an object"); continue; }
            Replacement r;
            r.scale = pack_scale;
            if (e.get("scale") && !int_in(e.get("scale"), 1, 8, r.scale)) { note(where + "\"scale\" must be 1-8"); continue; }
            if (const Json* a = e.get("anchor")) {
                if (a->t != Json::Arr || a->a.size() != 2 || !int_in(&a->a[0], -4096, 4096, r.ax) || !int_in(&a->a[1], -4096, 4096, r.ay)) {
                    note(where + "\"anchor\" must be [x, y] in picture pixels");
                    continue;
                }
            }
            std::string err;
            r.img = png_of(e, err);
            if (!r.img) { note(where + err); continue; }
            graphics_[key] = r;
            ++n_graphics;
        }
    }
    if (const Json* f = m.get("far")) {
        if (f->t != Json::Arr) return fail("manifest.json: \"far\" must be an array");
        for (size_t i = 0; i < f->a.size(); ++i) {
            const Json& e = f->a[i];
            const std::string where = "far[" + std::to_string(i) + "]: ";
            int scale = pack_scale;
            const Json* lines = e.t == Json::Obj ? e.get("lines") : nullptr;
            if (!lines || lines->t != Json::Arr) { note(where + "needs \"png\" and \"lines\""); continue; }
            if (e.get("scale") && !int_in(e.get("scale"), 1, 8, scale)) { note(where + "\"scale\" must be 1-8"); continue; }
            std::string err;
            const Image* img = png_of(e, err);
            if (!img) { note(where + err); continue; }
            if (img->w != kWidth * scale || img->h < int(lines->a.size()) * scale) {
                note(where + "the strip must be " + std::to_string(kWidth * scale) + " pixels wide and " + std::to_string(scale) +
                     " rows per line (" + std::to_string(lines->a.size()) + " lines)");
                continue;
            }
            for (size_t r = 0; r < lines->a.size(); ++r) {
                uint64_t hash = 0;
                const Json& l = lines->a[r];
                if (l.t != Json::Str || l.s.empty() || l.s == "-") continue; // a row left unused
                if (!parse_hex64(l.s, hash)) { note(where + "line " + std::to_string(r) + ": not a 16-digit hex key"); continue; }
                if (!room()) break;
                far_[hash] = FarRow{img, scale, int(r)};
                ++n_far;
            }
        }
    }
    if (const Json* p = m.get("palettes")) {
        if (p->t != Json::Arr) return fail("manifest.json: \"palettes\" must be an array");
        static const char* const kLayerNames[] = {"other", "far", "tiles", "stars", "objects"};
        for (size_t i = 0; i < p->a.size(); ++i) {
            const Json& e = p->a[i];
            const std::string where = "palettes[" + std::to_string(i) + "]: ";
            const Json* match = e.t == Json::Obj ? e.get("match") : nullptr;
            const Json* colors = e.t == Json::Obj ? e.get("colors") : nullptr;
            PaletteOverride po;
            bool ok = match && colors && match->t == Json::Arr && colors->t == Json::Arr && match->a.size() == 16 && colors->a.size() == 16;
            for (int k = 0; ok && k < 16; ++k) {
                uint32_t v = 0, c = 0;
                ok = match->a[size_t(k)].t == Json::Str && parse_colour(match->a[size_t(k)].s, true, v) &&
                     colors->a[size_t(k)].t == Json::Str && parse_colour(colors->a[size_t(k)].s, false, c);
                po.match[k] = uint16_t(v);
                po.rgba[k] = c;
            }
            if (!ok) { note(where + "needs \"match\" (16 \"#rgb\" Amiga colours) and \"colors\" (16 \"#rrggbb\")"); continue; }
            if (const Json* l = e.get("layers")) {
                po.layers = 0;
                for (const Json& n : l->t == Json::Arr ? l->a : std::vector<Json>{}) {
                    int bit = -1;
                    for (int k = 0; k < 5; ++k)
                        if (n.t == Json::Str && n.s == kLayerNames[k]) bit = k;
                    if (bit < 0) { ok = false; break; }
                    po.layers |= uint8_t(1 << bit);
                }
                if (!ok || l->t != Json::Arr) { note(where + "\"layers\": a list of other, far, tiles, stars, objects"); continue; }
            }
            if (!room()) break;
            palettes_.push_back(po);
            ++n_pal;
        }
    }
    packs_.push_back(path);
    note("loaded: " + std::to_string(n_graphics) + " graphics, " + std::to_string(n_far) + " far lines, " + std::to_string(n_pal) +
         " palettes (all packs: " + std::to_string(bytes_ >> 10) + " KB of pictures)");
    return true;
}

// --- AssetDumper ---

bool AssetDumper::open(const std::string& dir, std::string& why) {
    std::error_code ec;
    for (const char* sub : {"sprites", "tiles", "far"}) fs::create_directories(fs::path(dir) / sub, ec);
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
    for (Graphic& g : gs) {
        if (g.kind == GfxKind::Far) {
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
    std::string j = std::string("{\n  \"format\": \"x2-asset-pack\",\n  \"version\": ") + (extra_.empty() ? "1" : "2") + ",\n  \"name\": \"dump\",\n"
                    "  \"note\": \"Original graphics of your own copy of the game, written by x2recomp --dump-assets. "
                    "User data: do not distribute.\",\n  \"scale\": 1,\n  \"graphics\": {";
    bool first = true;
    for (const auto& [key, e] : entries_) {
        char line[512];
        std::snprintf(line, sizeof line,
                      "%s\n    \"%s\": {\"png\": \"%s\", \"kind\": \"%s\", \"w\": %d, \"h\": %d, \"uses\": %u, \"first_frame\": %u, "
                      "\"palette\": %u}",
                      first ? "" : ",", key.c_str(), e.png.c_str(), e.kind == GfxKind::Sprite ? "sprite" : "tile", e.w, e.h,
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
    if (!extra_.empty()) j += ",\n  " + extra_;
    j += "\n}\n";
    std::ofstream out(dir_ + "/manifest.json", std::ios::binary);
    if (!out || !out.write(j.data(), std::streamsize(j.size()))) { why = "cannot write " + dir_ + "/manifest.json"; return false; }
    if (errors_) { why = std::to_string(errors_) + " PNG files could not be written"; return false; }
    return true;
}

} // namespace x2::modern
