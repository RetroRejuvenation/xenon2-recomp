// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
//
// Making untrusted text safe to print. Names that come from files the user opens (archive member names, disk volume
// names, asset-pack manifest strings, WHDLoad slave strings) are shown on the console, in the log file and in the
// launcher. Raw control bytes there can move the cursor, set a terminal title (ESC sequences), forge a log line (CR/LF)
// or make a file name look like something else (bidirectional overrides). sanitize_text() keeps valid UTF-8 text as it
// is and replaces everything else with a visible escape.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace x2 {

namespace text_safety_detail {
inline void put_escape(std::string& out, const char* prefix, unsigned value, int digits) {
    static const char hex[] = "0123456789abcdef";
    out += prefix;
    for (int i = digits - 1; i >= 0; --i) out += hex[(value >> (4 * i)) & 15u];
}
} // namespace text_safety_detail

// Returns `s` with control characters and malformed UTF-8 replaced by escapes: bytes below 0x20 and 0x7F as \xNN,
// C1 controls (U+0080..U+009F), line/paragraph separators and bidirectional override/isolate marks as \uNNNN, and
// bytes that are not part of a valid UTF-8 sequence as \xNN. If max_bytes is non-zero the result is cut to at most that
// many bytes (on a character boundary) and "..." is appended.
inline std::string sanitize_text(std::string_view s, std::size_t max_bytes = 0) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (max_bytes != 0 && out.size() >= max_bytes) {
            out += "...";
            break;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) {
            text_safety_detail::put_escape(out, "\\x", c, 2);
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        // Multi-byte UTF-8: validate the sequence (no overlongs, no surrogates, nothing above U+10FFFF).
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
        bool ok = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) ok = false;
            else cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (ok) {
            if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;
        }
        if (!ok) {
            text_safety_detail::put_escape(out, "\\x", c, 2);
            ++i;
            continue;
        }
        const bool c1 = cp >= 0x80 && cp <= 0x9F;
        const bool separator = cp == 0x2028 || cp == 0x2029;
        const bool bidi = (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F || cp == 0x061C;
        if (c1 || separator || bidi) {
            text_safety_detail::put_escape(out, "\\u", cp, 4);
        } else {
            out.append(s.substr(i, len));
        }
        i += len;
    }
    return out;
}

// True when sanitize_text(s) would change `s`.
inline bool has_unsafe_text(std::string_view s) { return sanitize_text(s) != s; }

} // namespace x2
