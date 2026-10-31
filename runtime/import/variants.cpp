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
    // The demos: a different program from the game (docs/re-notes/single-disk-variants.md), recognised and reported.
    {"179ac1d81e65ca9ec84d7f57c77d1baaf817fb78", "demo-playable", "Xenon 2 Megablast playable demo disk",
     "the playable demo is a different, smaller program than the game: recognised, not imported"},
    {"927a8e26318073e72c15b982c09c077256951ec2", "demo-playable-byte-bandit", "Xenon 2 Megablast playable demo disk [v Byte Bandit]",
     "the playable demo is a different, smaller program than the game: recognised, not imported"},
    {"38a46f19d6204cf8edb5490137e4f30a4f0ab5b3", "lgcss-demo", "Rick Dangerous & Xenon II & 3D Pool & Tech compilation (L.G.C.S.S.)",
     "the Xenon II on this compilation disk is the demo (the files X2.BIN and X2DEMO.BIN), not the game: recognised, not imported"},
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
};
// IPF images (SPS preservation dumps) recognised by the sha1 of the file, only to name them in the message for a
// disc whose protected tracks cannot be imported (the Power Pack and the 1762 compilation are imported now).
const KnownDisc kIpfProtected[] = {
    {"72cf591760e014b53e946bf9603ae71b3d21d0d8", "retail disk 1, first pressing (SPS 0297)", 0, "", ""},
    {"11e95a503a80f32a7b3f0fda9ae532ae61552d86", "retail disk 1, second pressing (SPS 2874 [a])", 0, "", ""},
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
                         "181 bytes of credit text differ from the canonical image",
                         CrackRecipe::Plain, "1599c37d894f4e276bee64d6f3bf874cc80e1f01"});
    c.cracked.push_back({"2b98cbbac1a89901fcfe634ef805b66612d3b2de", "crack-qtx", "Quartex (QTX) crack of the retail disk 1", false,
                         "226 bytes differ from the canonical image (credit text and the protection sites)",
                         CrackRecipe::Plain, "86094773cb2b31c8b9886e242026ba7ba9bba721"});
    c.cracked.push_back({"a3574a216c51948ac64dd308c7449740c71a84b7", "crack-flashtro-16", "Flashtro +16 crack of the first retail pressing", true,
                         "936 bytes differ from the canonical image (credit text, the first pressing's code revision and a trainer)",
                         CrackRecipe::Plain, "70fcb3d8c7eaf937adeafa7db13dab9887d2b6ce"});
    // PNA [a]: the PNA crack plus a 4-byte trainer patch at $7B00 (infinite ships).
    c.cracked.push_back({"12b7bd53288d3764af062a71923c9ec8189e61f2", "crack-pna-a", "PNA [a] crack of the retail disk 1 (trainer)", true,
                         "181 bytes of credit text and a 4-byte trainer patch at $7B00 (infinite ships): gameplay differs from the game",
                         CrackRecipe::Plain, "2232da1c9a75e0d54b4025ee963d604ffd1d053b"});
    // Black Monks: the body is plain and the tail is still under the retail XOR chain (a per-disk repair, cracks.md 3.4); it
    // carries a trainer hook at $7913.
    c.cracked.push_back({"0df9822509837bce3999806e3ae2e218cea4ddd9", "crack-black-monks", "Black Monks crack of the retail disk 1 (trainer hook)", true,
                         "195 bytes differ from the canonical image (credit text, a trainer hook at $7913 and the protection sites); the image tail is "
                         "repaired from a duplicate block on the disk",
                         CrackRecipe::BlackMonksTail, "976f09d2f50c5d3bde2dbd7c68cca7d8964a3a5e"});
    // The one-disk crunches (docs/re-notes/cracks.md 3.3).
    c.onedisk.push_back({"8f50b895f344400787c34929bb29ef3e123b0a4e", "crack-vf", "Vision Factory (VF) one-disk crunch (ONYX)",
                         "87fa47ae859d6a7c285f6d511b83c5ce152be535", Cruncher::Onyx, 0, 0x20A, 0x80, false,
                         "2cae13f13a5ed9129d28a2eeb8c67e1c3558fb20",
                         "the game is a crunched memory snapshot (ONYX cruncher); the credit text, the disk loader at $921A and 10 bytes elsewhere differ "
                         "from the canonical image"});
    c.onedisk.push_back({"83e19d34e7202cde05e8151067d3669e22447182", "crack-vf-a", "Vision Factory (VF) [a] one-disk crunch (Ice-style)",
                         "7b27789cfdb14244a989ff97e5cde70f59ad8b27", Cruncher::Ice, 2, 0x196, 0x80, false,
                         "f1f50fa3eefd5bdd551ff3598c0dd905f834867a",
                         "as VF with an Ice-style cruncher; 13 more bytes differ"});
    c.onedisk.push_back({"1181e872781906466335349b2bfa6c72337b3096", "crack-black-monks-h", "Black Monks [h][t +36] one-disk trainer build",
                         "48a31ecb9c009cc0b9a04f13f4a7db2a990ad51f", Cruncher::Bs1, 0, 0xE8, 0x0, true,
                         "5ddb872b2a5c843d3ed0c0a5738b41c2e1711aa7",
                         "the BS1 ByteKiller executable with trainer hooks (876 bytes at $7290-$75FC and patches at $F78, $FD4, $206C, $20D6): gameplay differs"});
    // The two game builds (docs/re-notes/build-variants.md).
    c.builds.push_back({"power-pack-2234", "The Power Pack compilation (SPS 2234), one disk", "power-pack-2234",
                        "1d383dc38df7430332e46590ea370f87837c9e6d", "70f40c468ee8295c676bc4fa994b03d6fd1abd8e", "",
                        "the first retail pressing's code with its own start-up, loader and lower-precision samples; disc 2 is rebuilt from the six game "
                        "files (functionally, not byte for byte: 61 unused sectors differ)"});
    c.builds.push_back({"compilation-1762", "Bombuzal + Xenon 2 compilation (SPS 1762), two disks", "compilation-1762",
                        "a665f6592f5c62f13aedd80669245b4370d48635", "c294d6644f9d5b20056b46f004cd8ec891b01440",
                        "a665f6592f5c62f13aedd80669245b4370d48635",
                        "a different compile of the game (entry bra.w $6C3A, code shifted after $D000, its own module 6) with its own disk 2"});
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

const OneDiskCrunch* Catalogue::find_onedisk(const std::string& disc_sha1) const {
    for (const OneDiskCrunch& o : onedisk)
        if (o.sha1 == disc_sha1) return &o;
    return nullptr;
}

const GameBuild* Catalogue::find_build_by_disk1_image(const std::string& image_sha1) const {
    for (const GameBuild& b : builds)
        if (!b.disk1_image_sha1.empty() && b.disk1_image_sha1 == image_sha1) return &b;
    return nullptr;
}

const GameBuild* Catalogue::find_build_by_disc2(const std::string& disc2_sha1) const {
    for (const GameBuild& b : builds)
        if (!b.disk1_image_sha1.empty() && b.disc2_sha1 == disc2_sha1) return &b;
    return nullptr;
}

const GameBuild* Catalogue::find_build_by_game(const std::string& game_sha1) const {
    for (const GameBuild& b : builds)
        if (b.game_sha1 == game_sha1) return &b;
    return nullptr;
}

const GameBuild* Catalogue::find_build(const std::string& tag) const {
    for (const GameBuild& b : builds)
        if (b.build_tag == tag) return &b;
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

const CdtvDisc& builtin_cdtv_disc() {
    static const CdtvDisc d{"cdtv-xenon2-1991", "Xenon 2 Megablast, CDTV release (EN/FR/DE/IT)",
                            "f2d180efb1816e703a227f2ae50008569066dffd", "fa8419ed78d817ab43f480fcd75b4728b4355b2d", "EN,FR,DE,IT", 95};
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
        i.identified_by = "whole-disc SHA-1 " + c.sha1 + "; the game image is stored " +
                          (c.recipe == CrackRecipe::BlackMonksTail ? "at disk offset $70C00 with its tail repaired" : "plain at disk offset $70C00");
        i.recipe = Recipe::CrackedRetail;
        i.game_sha1 = c.image_sha1;
        i.disc2_sha1 = disc2;
        i.supported = true;
        i.trainer = c.trainer;
        i.note = c.note + (c.trainer ? "; a trainer build: gameplay differs" : "") +
                 "; the standard disc 2 is still needed; not canonical, own replay goldens (tests/replays/cracks)";
        v.push_back(i);
    }
    for (const OneDiskCrunch& o : builtin_catalogue().onedisk) {
        VariantInfo i;
        i.id = o.id;
        i.name = o.name;
        i.identified_by = "whole-disc SHA-1 " + o.sha1 + "; crunched executable " + o.exe_sha1 + " (the whole game as a memory snapshot) and six crunched data files";
        i.recipe = Recipe::OneDiskCrunch;
        i.game_sha1 = o.image_sha1;
        i.disc2_sha1 = "6c10b7bd28f4d18f8411d35f30697a4b0ea71a6e";
        i.supported = true;
        i.trainer = o.trainer;
        i.note = o.note + (o.trainer ? "; a trainer build: gameplay differs" : "") +
                 "; disc 2 is rebuilt from the data files (the standard disc 2 is used instead when given); not canonical, own replay goldens (tests/replays/cracks)";
        v.push_back(i);
    }
    for (const GameBuild& b : builtin_catalogue().builds) {
        VariantInfo i;
        i.id = b.id;
        i.name = b.name;
        i.recipe = b.disk1_image_sha1.empty() ? Recipe::GameBuildPowerPack : Recipe::GameBuildCompilation;
        i.game_sha1 = b.game_sha1;
        i.disc2_sha1 = b.disc2_sha1;
        i.build = b.build_tag;
        i.supported = true;
        i.protected_ = b.disk1_image_sha1.empty();
        i.identified_by = b.disk1_image_sha1.empty()
                              ? "from an IPF or flux image (or the WHDLoad Disk.1): the 157 custom data tracks decode with valid checksums, the main image "
                                "and the six game files decrunch intact"
                              : "disk 1: the plain game image at $70C00 has SHA-1 " + b.disk1_image_sha1 + "; disk 2 SHA-1 " + b.disc2_sha1;
        i.note = b.note + "; the manifest carries build=" + b.build_tag + ", so the runtime starts this build; not canonical, own replay goldens (tests/replays/" +
                 b.build_tag + ")";
        v.push_back(i);
    }
    {
        const CdtvDisc& c = builtin_cdtv_disc();
        VariantInfo i;
        i.id = c.id;
        i.name = c.name;
        i.identified_by = "a CD data track (MODE1/2352 .bin, .iso, or a .cue with its .bin files, also from a ZIP or folder) whose ISO 9660 volume is "
                          "XENONII and holds the hunk file X2 with SHA-1 " + c.x2_sha1;
        i.recipe = Recipe::CdtvDisc;
        i.game_sha1 = c.game_sha1;
        i.build = "cdtv";
        i.supported = true;
        i.note = "pre-linked at import: X2 linked at $400 (hunk 1 behind it), the " + std::to_string(c.game_files) +
                 " files the game loads stored beside it (cd0/), manifest build=cdtv, chip_kb=1024; the CD audio is imported too when the "
                 "input has the audio tracks. The runtime plays it (x2recomp --game cdtv): the interpreter only, not canonical, no disc 2";
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
    return v;
}

} // namespace x2::import
