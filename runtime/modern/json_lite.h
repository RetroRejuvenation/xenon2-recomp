// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A small strict JSON parser for pack manifests (screens and panel loader, tests). Same grammar as the one the v1 loader
// keeps in assets.cpp; kept separate so the two loaders do not depend on each other's internals.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x2::modern::jl {

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
inline const Json* Json::get(const char* k) const {
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

} // namespace x2::modern::jl
