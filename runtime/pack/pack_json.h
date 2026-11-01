// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A small strict JSON reader for manifest.json (RFC 8259: no comments, no trailing commas, no NaN or hex numbers), with a
// depth limit and a node limit so that a hostile file cannot cost more memory than its size allows for.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace x2::pack {

// Object members are a struct, not std::pair<std::string, Json>: std::pair needs a complete Json here, which libstdc++
// enforces; std::vector allows an incomplete element type.
struct JsonMember;
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<JsonMember> o;
    // The last member with this key (JSON objects with duplicate keys: the last one wins, as in most readers), or null.
    const Json* get(const char* k) const;
    bool is_obj() const { return t == Obj; }
    bool is_arr() const { return t == Arr; }
    bool is_str() const { return t == Str; }
    bool is_num() const { return t == Num; }
    // True and the value for an integral number inside [lo, hi].
    bool as_int(long long lo, long long hi, int& out) const;
};
struct JsonMember {
    std::string first;
    Json second;
};

// Parses `text` (UTF-8, an optional BOM is skipped). On failure `why` says what and where ("... at offset N").
bool parse_json(const std::string& text, Json& out, std::string& why, size_t max_nodes = 1000000, int max_depth = 32);

} // namespace x2::pack
