// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/variants.h"

#include <cstdio>

namespace x2::import {

namespace {

// Disk images catalogued in docs/re-notes/sources.md (identification only; no game data is stored here).
const KnownDisc kDisc1[] = {
    {"45e66f9cd7cb68f1b759c9ff8dd0f4427ca942a6", "BS1 [t+3] trained release, disk 1", 3, "bs1-t3", ""},
    {"268e8b529ae4e357d1d624c7fe4d565a2689f225", "BS1 [t+3] trained release, disk 1 [a] (alternate boot block)", 2, "bs1-t3-a", ""},
    {"d6e1a7ba5cdd196f6aecffd4044b4c23b694092a", "BS1 [t+3] Deejay99 UAE-fix hack, disk 1", 1, "bs1-dj99", ""},
    // The same XENON-II (sha1 778cf75e...) behind another trainer menu: the canonical game image.
    {"cb366028a1d0904601b16da745f2557ff08debfb", "BS1 [h Timecircle] trained release, disk 1", 2, "bs1-timecircle", ""},
    // A compilation's menu disk: the Xenon 2 executable is the file named `1`, found by its structure; it is the same
    // XENON-II, so the game image is the canonical one. Its disk 2 is `others4-d2` below.
    {"1b6f1fd89478a11831971026a716ef13c46bd52c", "Xenon 2 & 4 Others compilation, disk 1", 2, "others4",
     "the Xenon 2 executable is the file named \"1\" (the same program as XENON-II); the compilation's other games are ignored"},
    {"85a06ac4d45ca53ebd8e6932bc73678db65cacc8", "Xenon 2 & 4 Others compilation, disk 1 [a]", 2, "others4-a",
     "the Xenon 2 executable is the file named \"1\" (the same program as XENON-II); the compilation's other games are ignored"},
};
const KnownDisc kDisc2[] = {
    {"a748afc7a142335a40cc46fb7785dea820770241", "BS1 [t+3] trained release, disk 2", 3, "", ""},
    {"13eb35605c6af1968a64096434d3ca93b91bb324", "BS1 Deejay99 hack, disk 2 (26 bytes differ from the BS1 disk 2)", 1, "", ""},
    {"5e5d7865508da11ccc41d79edf0c7da1b51277d6", "Xenon 2 & 4 Others compilation, disk 2 (2 unused sectors differ from the BS1 disk 2)", 2, "",
     "all six level modules are identical to the canonical disc 2; only sectors 8 and 1752, which the game never reads, differ"},
};
// Recognised and refused. The reasons are measured (docs/re-notes/cracks.md, single-disk-variants.md).
const RefusedDisc kRefused1[] = {
    {"12b7bd53288d3764af062a71923c9ec8189e61f2", "crack-pna-a", "PNA [a] crack of the retail disk 1",
     "it is the PNA crack plus a trainer patch (infinite ships): gameplay differs from the game, so it is not imported; use the PNA "
     "disk or, better, an IPF or flux dump of the original"},
    {"0df9822509837bce3999806e3ae2e218cea4ddd9", "crack-black-monks", "Black Monks crack of the retail disk 1",
     "its game image needs a per-disk repair of the tail (the last 19 KB are left under the retail XOR chain), which the importer "
     "does not do yet"},
    {"1181e872781906466335349b2bfa6c72337b3096", "crack-black-monks-h", "Black Monks [h][t +36] one-disk trainer build",
     "a one-disk build (crunched game and data files) with trainer hooks: not supported"},
    {"8f50b895f344400787c34929bb29ef3e123b0a4e", "crack-vf", "Vision Factory (VF) one-disk crunch",
     "the game is a crunched memory snapshot (ONYX cruncher) with the data files crunched on the disk: the importer has no decruncher for it yet"},
    {"83e19d34e7202cde05e8151067d3669e22447182", "crack-vf-a", "Vision Factory (VF) [a] one-disk crunch",
     "the game is a crunched memory snapshot (Ice-style cruncher) with the data files crunched on the disk: the importer has no decruncher for it yet"},
    {"2405da96182c63fc6a7e48b7f19f2904f0a44ed9", "crack-predators-9", "Predators +9 crack of the retail disk 1",
     "its trainer replaced 224 bytes of the game's entry code, which cannot be restored without the original disc: use an IPF or flux "
     "dump of the original instead"},
    {"1b4855f98f7df8d5052e5578d387dbf486068f8f", "crack-flashtro-16-a", "Flashtro +16 [a] trainer build of the retail disk 1",
     "its game image is stored in a transformation that has not been analysed: use the plain Flashtro +16 disk, or an IPF or flux "
     "dump of the original"},
};
const RefusedDisc kRefused2[] = {
    {"31997fddbf4174ea175ba25375864a2a75a1c445", "others4-a-d2", "Xenon 2 & 4 Others compilation, disk 2 [a]",
     "a damaged copy: two ranges inside the level modules (tracks 61 and 79) hold a copier's filler instead of level data, so levels 3 "
     "and 4 break; use the other disk 2 or the standard disc 2"},
    {"c294d6644f9d5b20056b46f004cd8ec891b01440", "bombuzal-1762-d2", "Bombuzal + Xenon 2 compilation (SPS 1762), disk 2",
     "the data disc of a different build of the game (module 5's last sector and all of module 6 differ): not the canonical disc 2"},
};
// IPF images (SPS preservation dumps) recognised by the sha1 of the file, only to name them in the message for a
// disc whose protected tracks cannot be imported.
const KnownDisc kIpfProtected[] = {
    {"72cf591760e014b53e946bf9603ae71b3d21d0d8", "retail disk 1, first pressing (SPS 0297)", 0, "", ""},
    {"11e95a503a80f32a7b3f0fda9ae532ae61552d86", "retail disk 1, second pressing (SPS 2874 [a])", 0, "", ""},
    {"0be7c3253e56173072abd80ebb41f5d5f2e52e7a", "Bombuzal + Xenon 2 compilation, disk 1 (SPS 1762)", 0, "", ""},
    {"c06f8a4206b839663457192c7564f9306f8980e1", "The Power Pack compilation (SPS 2234)", 0, "", ""},
};
const char* const kKnownExeSha1 = "778cf75e75d7e1b2036f4d68f8a557b69e99a817";
const char* const kCanonicalGameSha1 = "1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4";

template <size_t N>
const KnownDisc* lookup(const KnownDisc (&table)[N], const std::string& sha1) {
    for (const KnownDisc& k : table)
        if (sha1 == k.sha1) return &k;
    return nullptr;
}
template <size_t N>
const RefusedDisc* lookup_refused(const RefusedDisc (&table)[N], const std::string& sha1) {
    for (const RefusedDisc& k : table)
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
    // Cracked copies of the retail disk 1 with the game image stored plain at $70C00 (docs/re-notes/cracks.md).
    c.cracked.push_back({"2028f31af2fb760da405618a42f9e69e15882825", "crack-pna", "PNA crack of the retail disk 1", false,
                         "181 bytes of credit text differ from the canonical image"});
    c.cracked.push_back({"2b98cbbac1a89901fcfe634ef805b66612d3b2de", "crack-qtx", "Quartex (QTX) crack of the retail disk 1", false,
                         "226 bytes differ from the canonical image (credit text and the protection sites)"});
    c.cracked.push_back({"a3574a216c51948ac64dd308c7449740c71a84b7", "crack-flashtro-16", "Flashtro +16 crack of the first retail pressing", true,
                         "936 bytes differ from the canonical image (credit text, the first pressing's code revision and a trainer)"});
    for (const RefusedDisc& r : kRefused2) c.refused_disc2.push_back(r);
    return c;
}

} // namespace

const KnownDisc* find_bs1_disc1(const std::string& sha1) { return lookup(kDisc1, sha1); }
const KnownDisc* find_bs1_disc2(const std::string& sha1) { return lookup(kDisc2, sha1); }
const RefusedDisc* find_refused_disc1(const std::string& sha1) { return lookup_refused(kRefused1, sha1); }
const RefusedDisc* find_refused_disc2(const std::string& sha1) { return lookup_refused(kRefused2, sha1); }
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

const CrackedRetail* Catalogue::find_cracked(const std::string& disc_sha1) const {
    for (const CrackedRetail& c : cracked)
        if (c.sha1 == disc_sha1) return &c;
    return nullptr;
}

const RefusedDisc* Catalogue::find_refused_disc2(const std::string& disc_sha1) const {
    for (const RefusedDisc& r : refused_disc2)
        if (disc_sha1 == r.sha1) return &r;
    return nullptr;
}

const std::vector<Disc2Module>& canonical_disc2_modules() {
    // The ranges are the six crunched data files of the one-disk builds (their names encode them, cracks.md section 3.3).
    static const std::vector<Disc2Module> m = {
        {11, 229, "5a6e6f6581320d9018c358ac6e97f2283103ca3c"}, {250, 254, "25b33cc882f7ab9f5eabcf0ae2992088af52eb3a"},
        {514, 275, "35b99a20910ecb0205ab75f6f365382d9c4988ee"}, {799, 345, "bc99a6c1c28c3d83c836528fbaa3211bea416f07"},
        {1154, 312, "59fb3a5bee4754a6651a47bcc7c23a92b928ddd9"}, {1476, 248, "0634530d1599b87554d9a5c5be8614847d0cd937"},
    };
    return m;
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
    auto bs1 = [&](const KnownDisc& d, const char* note) {
        VariantInfo i;
        i.id = d.id;
        i.name = d.what;
        i.identified_by = std::string("whole-disc SHA-1 ") + d.sha1 + ", Xenon 2 executable (XENON-II, found by structure) in an OFS volume";
        i.recipe = Recipe::Bs1OfsExecutable;
        i.game_sha1 = kCanonicalGameSha1;
        i.disc2_sha1 = disc2;
        i.canonical = true;
        i.supported = true;
        i.note = note;
        v.push_back(i);
    };
    bs1(kDisc1[0], "the canonical image; the replay goldens are recorded from it");
    bs1(kDisc1[1], "same game image as bs1-t3");
    bs1(kDisc1[2], "same game image as bs1-t3; its disk 2 has 26 changed bytes");
    bs1(kDisc1[3], "same XENON-II as bs1-t3: the canonical image");
    bs1(kDisc1[4], "same XENON-II as bs1-t3 (the file named \"1\"); disk 2: 2 unused sectors differ, level modules identical");
    bs1(kDisc1[5], "same XENON-II as bs1-t3 (the file named \"1\"); its disk 2 [a] is a damaged copy and is refused");
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
        i.note = "opt-in; works from any source that decodes disc 1 (plain ADF, DMS, IPF, flux, WHDLoad Disk.1); manifest patched=true";
        v.push_back(i);
    }
    for (const CrackedRetail& c : builtin_catalogue().cracked) {
        VariantInfo i;
        i.id = c.id;
        i.name = c.name;
        i.identified_by = "whole-disc SHA-1 " + c.sha1 + "; the game image is stored plain at disk offset $70C00";
        i.recipe = Recipe::CrackedRetail;
        i.disc2_sha1 = disc2;
        i.supported = true;
        i.note = c.note + (c.trainer ? "; a trainer build, gameplay may differ" : "") +
                 "; the standard disc 2 is still needed; not canonical, no replay goldens";
        v.push_back(i);
    }
    auto refused = [&](const RefusedDisc& d, const char* what_kind) {
        VariantInfo i;
        i.id = d.id;
        i.name = d.what;
        i.identified_by = std::string("whole-disc SHA-1 ") + d.sha1;
        i.note = std::string(what_kind) + ": " + d.why;
        v.push_back(i);
    };
    for (const RefusedDisc& d : kRefused1) refused(d, "recognised, refused");
    for (const RefusedDisc& d : kRefused2) refused(d, "disc 2 recognised, refused");
    auto recognised = [&](const char* id, const KnownDisc& d, const char* note) {
        VariantInfo i;
        i.id = id;
        i.name = d.what;
        i.identified_by = std::string("whole-disc SHA-1 ") + d.sha1;
        i.protected_ = false;
        i.note = note;
        v.push_back(i);
    };
    recognised("ipf-bombuzal-1762", kIpfProtected[2], "recognised by the IPF file's SHA-1, a different build: not importable yet");
    recognised("ipf-power-pack-2234", kIpfProtected[3], "recognised by the IPF file's SHA-1, not importable yet (custom track format)");
    return v;
}

} // namespace x2::import
