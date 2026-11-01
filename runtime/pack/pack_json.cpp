// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_json.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace x2::pack {

const Json* Json::get(const char* k) const {
    for (auto it = o.rbegin(); it != o.rend(); ++it)
        if (it->first == k) return &it->second;
    return nullptr;
}

bool Json::as_int(long long lo, long long hi, int& out) const {
    if (t != Num || n != std::floor(n) || n < double(lo) || n > double(hi)) return false;
    out = int(n);
    return true;
}

namespace {

class Parser {
public:
    Parser(const std::string& t, size_t max_nodes, int max_depth) : t_(t), max_nodes_(max_nodes), max_depth_(max_depth) {}
    bool parse(Json& out, std::string& why) {
        if (t_.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ = 3;
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
        const size_t n = std::strlen(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::string& out, unsigned v) {
        if (v < 0x80) out += char(v);
        else if (v < 0x800) { out += char(0xC0 | (v >> 6)); out += char(0x80 | (v & 63)); }
        else if (v < 0x10000) { out += char(0xE0 | (v >> 12)); out += char(0x80 | ((v >> 6) & 63)); out += char(0x80 | (v & 63)); }
        else { out += char(0xF0 | (v >> 18)); out += char(0x80 | ((v >> 12) & 63)); out += char(0x80 | ((v >> 6) & 63)); out += char(0x80 | (v & 63)); }
    }
    bool hex4(unsigned& v) {
        if (p_ + 4 > t_.size()) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = t_[p_++];
            const int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
            if (d < 0) return fail("bad \\u escape");
            v = v * 16 + unsigned(d);
        }
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
                unsigned v = 0;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') { // a surrogate pair
                    const size_t save = p_;
                    p_ += 2;
                    unsigned lo = 0;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    else p_ = save;
                }
                utf8(out, v);
                break;
            }
            default: return fail("bad escape in a string");
            }
        }
        if (p_ >= t_.size()) return fail("unterminated string");
        ++p_;
        return true;
    }
    bool number(Json& v) {
        const size_t b = p_;
        size_t q = p_;
        auto digit = [&](size_t i) { return i < t_.size() && t_[i] >= '0' && t_[i] <= '9'; };
        if (t_[q] == '-') ++q;
        if (!digit(q)) return fail("bad number");
        if (t_[q] == '0') ++q;
        else while (digit(q)) ++q;
        if (q < t_.size() && t_[q] == '.') {
            ++q;
            if (!digit(q)) return fail("bad number");
            while (digit(q)) ++q;
        }
        if (q < t_.size() && (t_[q] == 'e' || t_[q] == 'E')) {
            ++q;
            if (q < t_.size() && (t_[q] == '+' || t_[q] == '-')) ++q;
            if (!digit(q)) return fail("bad number");
            while (digit(q)) ++q;
        }
        const std::string text = t_.substr(b, q - b);
        v.n = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(v.n)) return fail("number out of range");
        v.t = Json::Num;
        p_ = q;
        return true;
    }
    bool value(Json& v, int depth) {
        if (depth > max_depth_) return fail("nested too deeply");
        if (++nodes_ > max_nodes_) return fail("too many values");
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
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character");
    }
    const std::string& t_;
    size_t p_ = 0, nodes_ = 0, max_nodes_;
    int max_depth_;
    std::string err_;
};

} // namespace

bool parse_json(const std::string& text, Json& out, std::string& why, size_t max_nodes, int max_depth) {
    out = Json();
    return Parser(text, max_nodes, max_depth).parse(out, why);
}

} // namespace x2::pack
