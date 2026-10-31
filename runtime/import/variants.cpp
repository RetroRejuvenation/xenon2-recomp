// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/variants.h"

#include <cstdio>

namespace x2::import {

namespace {

// Disk images catalogued in docs/re-notes/sources.md (identification only; no game data is stored here).
const KnownDisc kDisc1[] = {
    {"45e66f9cd7cb68f1b759c9ff8dd0f4427ca942a6", "BS1 [t+3] trained release, disk 1", 3},
    {"268e8b529ae4e357d1d624c7fe4d565a2689f225", "BS1 [t+3] trained release, disk 1 [a] (alternate boot block)", 2},
    {"d6e1a7ba5cdd196f6aecffd4044b4c23b694092a", "BS1 [t+3] Deejay99 UAE-fix hack, disk 1", 1},
};
const KnownDisc kDisc2[] = {
    {"a748afc7a142335a40cc46fb7785dea820770241", "BS1 [t+3] trained release, disk 2", 3},
    {"13eb35605c6af1968a64096434d3ca93b91bb324", "BS1 Deejay99 hack, disk 2 (26 bytes differ from the BS1 disk 2)", 1},
};
const KnownDisc kOtherReleases[] = {
    {"2028f31af2fb760da405618a42f9e69e15882825", "PNA single-disk crack", 0},
    {"12b7bd53288d3764af062a71923c9ec8189e61f2", "PNA [a] single-disk crack", 0},
    {"2b98cbbac1a89901fcfe634ef805b66612d3b2de", "Quartex (QTX) single-disk crack", 0},
    {"0df9822509837bce3999806e3ae2e218cea4ddd9", "Black Monks single-disk crack", 0},
    {"8f50b895f344400787c34929bb29ef3e123b0a4e", "Vision Factory (VF) one-disk crunch", 0},
    {"83e19d34e7202cde05e8151067d3669e22447182", "Vision Factory (VF) [a] one-disk crunch", 0},
};
// IPF images (SPS preservation dumps) recognised by the sha1 of the file, only to name them in the message for a
// disc whose protected tracks cannot be imported.
const KnownDisc kIpfProtected[] = {
    {"72cf591760e014b53e946bf9603ae71b3d21d0d8", "retail disk 1, first pressing (SPS 0297)", 0},
    {"11e95a503a80f32a7b3f0fda9ae532ae61552d86", "retail disk 1, second pressing (SPS 2874 [a])", 0},
    {"0be7c3253e56173072abd80ebb41f5d5f2e52e7a", "Bombuzal + Xenon 2 compilation, disk 1 (SPS 1762)", 0},
    {"c06f8a4206b839663457192c7564f9306f8980e1", "The Power Pack compilation (SPS 2234)", 0},
};
const char* const kKnownExeSha1 = "778cf75e75d7e1b2036f4d68f8a557b69e99a817";
const char* const kCanonicalGameSha1 = "1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4";

template <size_t N>
const KnownDisc* lookup(const KnownDisc (&table)[N], const std::string& sha1) {
    for (const KnownDisc& k : table)
        if (sha1 == k.sha1) return &k;
    return nullptr;
}

std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> out;
    for (; s[0] && s[1]; s += 2) {
        unsigned v = 0;
        std::sscanf(s, "%2x", &v);
        out.push_back(uint8_t(v));
    }
    return out;
}

Catalogue make_catalogue() {
    Catalogue c;
    // sha1 of the decoded game image (docs/re-notes/retail-disk1.md).
    c.retail.push_back({"retail-2874", "retail original, second pressing (SPS 2874 [a])", "fc84cc6f1ed47e3da0d5dc913632c705140f1e0a"});
    c.retail.push_back({"retail-0297", "retail original, first pressing (SPS 0297)", "8d02d25be543d21d3545b321ec774b41afc9d7e3"});
    // The protection-removal bytes (docs/re-notes/retail-disk1.md, "The game image"): the same for both pressings.
    // $70F6: the retail Copylock entry stub (`move.l a6,-(a7); lea x(pc),a6; movem.l d0-d7/a0-a7,(a6); lea $40(a6),a6`)
    //        becomes a short stub that loads a constant, stores it at vector $24 and branches to the routine's exit $763A.
    // $76AE: the check constant (`eori.w #$5A88,d1`) becomes `move.w #$95E4,d1`, the address of the game's own handler.
    c.patch.push_back({0x70F6, hex("2f0e4dfaff8648d6ffff4dee0040"), hex("203c7670cb6b21c0002460000538"),
                       "the Copylock entry stub, replaced by a stub that continues at the routine's exit"});
    c.patch.push_back({0x76AE, hex("0a415a88"), hex("323c95e4"), "the check constant"});
    return c;
}

} // namespace

const KnownDisc* find_bs1_disc1(const std::string& sha1) { return lookup(kDisc1, sha1); }
const KnownDisc* find_bs1_disc2(const std::string& sha1) { return lookup(kDisc2, sha1); }
const KnownDisc* find_other_release(const std::string& sha1) { return lookup(kOtherReleases, sha1); }
const KnownDisc* find_protected_ipf(const std::string& sha1) { return lookup(kIpfProtected, sha1); }
const char* bs1_disc1_reference_sha1() { return kDisc1[0].sha1; }
const char* bs1_disc2_reference_sha1() { return kDisc2[0].sha1; }
const char* known_exe_sha1() { return kKnownExeSha1; }
const char* canonical_game_image_sha1() { return kCanonicalGameSha1; }

const RetailPressing* Catalogue::find_retail(const std::string& image_sha1) const {
    for (const RetailPressing& p : retail)
        if (p.image_sha1 == image_sha1) return &p;
    return nullptr;
}

const Catalogue& builtin_catalogue() {
    static const Catalogue c = make_catalogue();
    return c;
}

bool apply_patch_ranges(std::vector<uint8_t>& image, const std::vector<PatchRange>& ranges, std::string& why,
                        std::vector<std::string>* applied) {
    constexpr uint32_t kBase = 0x400;
    for (const PatchRange& r : ranges) {
        char at[32];
        std::snprintf(at, sizeof at, "$%X", r.address);
        if (r.original.size() != r.patched.size() || r.original.empty()) { why = std::string("internal: bad patch range at ") + at; return false; }
        if (r.address < kBase || size_t(r.address - kBase) + r.original.size() > image.size()) {
            why = std::string("the image is too short for the patch range at ") + at;
            return false;
        }
        for (size_t i = 0; i < r.original.size(); ++i)
            if (image[size_t(r.address - kBase) + i] != r.original[i]) {
                why = std::string("the image does not hold the expected retail bytes at ") + at + "; refusing to patch it";
                return false;
            }
    }
    for (const PatchRange& r : ranges) {
        for (size_t i = 0; i < r.patched.size(); ++i) image[size_t(r.address - kBase) + i] = r.patched[i];
        if (applied) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "$%X+%zu", r.address, r.patched.size());
            applied->push_back(buf);
        }
    }
    return true;
}

const std::vector<std::string>& protection_patch_disclaimer() {
    static const std::vector<std::string> d = {
        "protection patch (--apply-protection-patch): please read this before going on.",
        "  What it does: it changes 18 bytes of the game image decoded from YOUR retail disc 1 (the Copylock call at "
        "$70F6 and a check constant at $76AE), so that the key-track check is removed from the image instead of being "
        "answered by the runtime.",
        "  It is applied locally, in memory, to your own copy, and the result is written only to your data folder. "
        "Nothing patched is distributed by this project.",
        "  Use it only with a copy of the game that you own. You are responsible for complying with the law where you "
        "live.",
        "  It is only applied to a disc whose decoded game image is a catalogued retail pressing (checked by SHA-1), and "
        "the import records it (patched=true).",
        "  The default method needs no patch and is preferred: import an IPF or flux image (KryoFlux, SCP, HFE) of the "
        "original disc, which keeps the key track.",
    };
    return d;
}

std::vector<VariantInfo> all_variants() {
    std::vector<VariantInfo> v;
    const char* disc2 = kDisc2[0].sha1;
    auto bs1 = [&](const char* id, const KnownDisc& d, const char* note) {
        VariantInfo i;
        i.id = id;
        i.name = d.what;
        i.identified_by = std::string("whole-disc SHA-1 ") + d.sha1 + ", XENON-II executable in an OFS volume";
        i.recipe = Recipe::Bs1OfsExecutable;
        i.game_sha1 = kCanonicalGameSha1;
        i.disc2_sha1 = disc2;
        i.canonical = true;
        i.supported = true;
        i.note = note;
        v.push_back(i);
    };
    bs1("bs1-t3", kDisc1[0], "the canonical image; the replay goldens are recorded from it");
    bs1("bs1-t3-a", kDisc1[1], "same game image as bs1-t3");
    bs1("bs1-dj99", kDisc1[2], "same game image as bs1-t3; its disk 2 has 26 changed bytes");
    for (const RetailPressing& p : builtin_catalogue().retail) {
        VariantInfo i;
        i.id = p.id;
        i.name = p.name;
        i.identified_by = "key track on cylinder 0 head 1 (Copylock), game image decoded from disc 1, SHA-1 " + p.image_sha1;
        i.recipe = Recipe::RetailKeyTrack;
        i.game_sha1 = p.image_sha1;
        i.disc2_sha1 = disc2;
        i.protected_ = true;
        i.supported = true;
        i.note = "needs an IPF or flux image of disc 1 (the key track); the runtime answers the in-game check "
                 "(retail-protection.md). Own replay goldens, not canonical.";
        v.push_back(i);
    }
    {
        VariantInfo i;
        i.id = "retail-patched";
        i.name = "a catalogued retail pressing imported with the protection patch";
        i.identified_by = "decoded game image SHA-1 of a retail pressing; --apply-protection-patch --i-confirm-i-own-the-original";
        i.recipe = Recipe::RetailPatched;
        i.disc2_sha1 = disc2;
        i.protected_ = true;
        i.supported = true;
        i.note = "opt-in; works from any source that decodes disc 1 (plain ADF, DMS, IPF, flux); manifest patched=true";
        v.push_back(i);
    }
    auto recognised = [&](const char* id, const KnownDisc& d, const char* note) {
        VariantInfo i;
        i.id = id;
        i.name = d.what;
        i.identified_by = std::string("whole-disc SHA-1 ") + d.sha1;
        i.protected_ = false;
        i.note = note;
        v.push_back(i);
    };
    recognised("crack-pna", kOtherReleases[0], "recognised and refused: custom trackloader, game re-laid on one disk");
    recognised("crack-pna-a", kOtherReleases[1], "recognised and refused");
    recognised("crack-qtx", kOtherReleases[2], "recognised and refused");
    recognised("crack-black-monks", kOtherReleases[3], "recognised and refused");
    recognised("crack-vf", kOtherReleases[4], "recognised and refused");
    recognised("crack-vf-a", kOtherReleases[5], "recognised and refused");
    recognised("ipf-bombuzal-1762", kIpfProtected[2], "recognised by the IPF file's SHA-1, not importable yet");
    recognised("ipf-power-pack-2234", kIpfProtected[3], "recognised by the IPF file's SHA-1, not importable yet (custom track format)");
    return v;
}

} // namespace x2::import
