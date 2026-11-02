// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
//
// Untrusted names never reach a terminal or a log as raw control bytes (runtime/text_safety.h). Synthetic data only:
// the helper itself, an importer run whose input is named with escape sequences, and an asset-pack manifest whose strings
// carry them.
//
//   x2_text_safety_tests
#include "import/importer.h"
#include "pack/pack_api.h"
#include "pack/pack_json.h"
#include "pack/pack_manifest.h"
#include "text_safety.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace x2;

namespace {
int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

bool clean(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

void test_helper() {
    CHECK(sanitize_text("plain name.adf") == "plain name.adf");
    CHECK(sanitize_text("") == "");
    CHECK(sanitize_text("a\x1b[31mb") == "a\\x1b[31mb");
    CHECK(sanitize_text("l1\nl2\r\n") == "l1\\x0al2\\x0d\\x0a");
    CHECK(sanitize_text("tab\there") == "tab\\x09here");
    CHECK(sanitize_text(std::string("nul\0byte", 8)) == "nul\\x00byte");
    CHECK(sanitize_text("del\x7f") == "del\\x7f");
    // Valid UTF-8 stays as it is: accents, CJK, an emoji.
    CHECK(sanitize_text("caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(sanitize_text("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    CHECK(sanitize_text("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");
    // C1 controls (U+0085 is NEL, U+009B is the 8-bit CSI), separators and bidi overrides are escaped.
    CHECK(sanitize_text("a\xC2\x85z") == "a\\u0085z");
    CHECK(sanitize_text("a\xC2\x9B[31mz") == "a\\u009b[31mz");
    CHECK(sanitize_text("a\xE2\x80\xA8z") == "a\\u2028z");
    CHECK(sanitize_text("a\xE2\x80\xAEz") == "a\\u202ez");
    // Malformed UTF-8: lone continuation, invalid lead, overlong, truncated, surrogate, above U+10FFFF.
    CHECK(sanitize_text("\x80") == "\\x80");
    CHECK(sanitize_text("\xFF") == "\\xff");
    CHECK(sanitize_text("\xC0\x80") == "\\xc0\\x80");
    CHECK(sanitize_text("\xE2\x82") == "\\xe2\\x82");
    CHECK(sanitize_text("\xED\xA0\x80") == "\\xed\\xa0\\x80");
    CHECK(sanitize_text("\xF4\x90\x80\x80") == "\\xf4\\x90\\x80\\x80");
    // Every byte value: the output never holds a raw control byte and never grows without bound.
    std::string all;
    for (int b = 0; b < 256; ++b) all += static_cast<char>(b);
    const std::string s = sanitize_text(all);
    CHECK(clean(s));
    CHECK(s.size() < all.size() * 4 + 4);
    CHECK(sanitize_text(s) == s); // idempotent
    CHECK(has_unsafe_text("a\x1b") && !has_unsafe_text("fine"));
    // The length cap cuts on a character boundary and says so.
    const std::string cut = sanitize_text("abcdef\xC3\xA9ghij", 8);
    CHECK(cut == "abcdef\xC3\xA9..." || cut == "abcdef\xC3\xA9g...");
}

void test_importer_messages() {
    import::Input in;
    in.name = "evil\x1b]0;owned\x07\nerror: forged line.adf";
    in.data.assign(1000, 0x41); // not a disk image: the importer reports on the name
    import::Result r = import::import_inputs({in});
    CHECK(!r.messages.empty());
    bool mentioned = false;
    for (const import::Message& m : r.messages) {
        CHECK(clean(m.text));
        CHECK(clean(import::format_message(m)));
        if (m.text.find("\\x1b") != std::string::npos) mentioned = true;
    }
    CHECK(clean(r.variant_name));
    CHECK(mentioned); // the name is still shown, escaped
}

void test_pack_manifest() {
    const std::string text =
        "{\"format\":\"x2-asset-pack\",\"version\":2,\"name\":\"x\\u001b]0;owned\\u0007\\nasset pack: forged: ok\","
        "\"author\":\"a\\u001b[2Jb\",\"license\":\"MIT\\u0085\",\"description\":\"line one\\nline two\\u001b[1m\"}";
    pack::Json root;
    std::string why;
    CHECK(pack::parse_json(text, root, why));
    pack::Manifest m;
    pack::PackReport report;
    std::string fatal;
    pack::ParseOptions opt;
    CHECK(pack::parse_manifest(root, "label", opt, m, report, fatal));
    CHECK(clean(m.name) && clean(m.author) && clean(m.license) && clean(m.description));
    CHECK(m.name.find("\\x1b") != std::string::npos); // visible, not removed
    CHECK(m.description.find("line one line two") == 0); // a multi-line description is folded to one line
    // Messages built from a pack name or a manifest key never carry control bytes either.
    pack::PackMessage pm{pack::Severity::Error, std::string("p\x1b[2J"), "graphics", "k\n2", "bad\r\nx"};
    CHECK(clean(pm.format()));
    CHECK(pm.format().find("\\x1b") != std::string::npos);
}
} // namespace

int main() {
    test_helper();
    test_importer_messages();
    test_pack_manifest();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
