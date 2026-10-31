// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The catalogue of Xenon 2 Amiga disk variants the importer knows (docs/re-notes/sources.md, retail-disk1.md,
// retail-protection.md): which discs it recognises by the SHA-1 of what they decode to, which of them it can
// import and how, what the import is expected to produce, and the retail protection-removal patch table behind the
// opt-in `--apply-protection-patch`. Identification data only; no game data is stored here (the patch table is
// 18 bytes of instruction encodings, kept as data so that it is reviewable in one place).
//
// Registry rows are table-driven: {id, name, how identified, recipe, expected outputs, canonical, protected,
// supported}. The recipes themselves live in importer.cpp (BS1 OFS executable, retail key track); this file only
// says which recipe applies to what.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

// ---------------------------------------------------------------------------------------------------------
// Whole-disc identification (the BS1 [t+3] release and the cracks that are recognised and refused).

struct KnownDisc {
    const char* sha1;
    const char* what;
    int quality;      // higher is preferred when several candidates are given
    const char* id;   // registry id of the variant ("" for disc 2 rows)
    const char* note; // shown when the disc is used ("" for none)
};

// Discs that decode to the canonical game image (the XENON-II executable in an OFS volume) or are used with it.
const KnownDisc* find_bs1_disc1(const std::string& sha1);
const KnownDisc* find_bs1_disc2(const std::string& sha1);

// Discs that are recognised and refused: a different build, a crack the importer cannot recover yet, a damaged copy.
struct RefusedDisc {
    const char* sha1;
    const char* id;
    const char* what;
    const char* why;
};
const RefusedDisc* find_refused_disc1(const std::string& sha1);
const RefusedDisc* find_refused_disc2(const std::string& sha1);

// Cracked copies of the retail disk 1: the game image stored plain (not under the retail XOR chain) at disk offset
// $70C00. Imported with the user's disc 2; not the canonical image (credit text, protection removal, trainers).
enum class CrackRecipe {
    Plain,         // the game image is the bytes at disk offset $70C00
    BlackMonksTail // plain, except the tail (the last 19 KB), which the crack left under the retail XOR chain (cracks.md 3.4)
};
struct CrackedRetail {
    std::string sha1, id, name;
    bool trainer = false; // a trainer build: gameplay may differ from the original
    std::string note;
    CrackRecipe recipe = CrackRecipe::Plain;
    std::string image_sha1; // the game image the recipe must produce (empty: not checked)
};

// The one-disk crunches (Vision Factory, VF [a], Black Monks [h][t +36]): ordinary AmigaDOS disks holding a crunched
// executable (the whole game as a memory snapshot) and six crunched data files whose names encode disc 2 sector ranges
// (docs/re-notes/cracks.md 3.3). The game image is cut out of the decrunched snapshot; disc 2 is rebuilt from the files.
enum class Cruncher { Onyx, Ice, Bs1 };
struct OneDiskCrunch {
    std::string sha1;     // of the whole disk
    std::string id, name;
    std::string exe_sha1; // the crunched executable on the disk
    Cruncher cruncher = Cruncher::Onyx;
    int hunk = 0;         // hunk of the executable that holds the crunched block
    size_t block = 0;     // offset of the block in that hunk's data
    uint32_t base = 0;    // memory address of snapshot byte 0
    bool trainer = false;
    std::string image_sha1; // the game image the recipe must produce
    std::string note;
};

// A game build that is not the canonical one: its own game image (the runtime starts it by the `build=` tag in
// manifest.txt, runtime/boot.h) and, for the compilation, its own disc 2. Identified by the SHA-1 of the game image.
struct GameBuild {
    std::string id;           // "power-pack-2234"
    std::string name;
    std::string build_tag;    // the manifest's build= value
    std::string game_sha1;    // of game_0400.bin as the importer writes it
    std::string disc2_sha1;   // of disc2.bin (the Power Pack's is rebuilt; the compilation's is its disk 2 unchanged)
    std::string disk1_image_sha1; // compilation: of the plain image at disk-1 offset $70C00 (empty for the Power Pack)
    std::string note;
};

// The CDTV release (docs/re-notes/cdtv.md, cdtv-code.md): identified by the SHA-1 of `X2`, the game's AmigaDOS hunk file on the
// disc. Imported by the pre-link recipe (cdtv_game.h): the hunks linked at fixed addresses become game_0400.bin, the files the
// game loads by name are stored beside it. Not the disk build: its own build tag (`cdtv`), its own assets folder.
struct CdtvDisc {
    std::string id, name;
    std::string x2_sha1;        // of the X2 file on the disc
    std::string game_sha1;      // of game_0400.bin as the importer writes it
    std::string languages;      // "EN,FR,DE,IT"
    size_t game_files = 0;      // files the game loads (everything but X2 and the OS files of the disc)
};
const CdtvDisc& builtin_cdtv_disc();

const KnownDisc* find_protected_ipf(const std::string& sha1);  // IPF files of discs that cannot be imported
const char* bs1_disc1_reference_sha1();                         // the clean BS1 [t+3] disk 1
const char* bs1_disc2_reference_sha1();                         // the clean BS1 [t+3] disk 2 = the canonical disc 2
const char* known_exe_sha1();                                   // XENON-II, the same on all BS1 disks
const char* canonical_game_image_sha1();                        // the depacked BS1 image (game_0400.bin)

// ---------------------------------------------------------------------------------------------------------
// Retail pressings: identified by the SHA-1 of the game image decoded from disc 1.

struct RetailPressing {
    std::string id;         // "retail-2874"
    std::string name;       // "retail original, second pressing (SPS 2874 [a])"
    std::string image_sha1; // of the decoded $3FC00-byte game image
};

// One protection-removal range: `original` is what the retail image must hold there (checked before anything is
// changed), `patched` what it holds afterwards.
struct PatchRange {
    uint32_t address = 0; // game address (the image lives at $400)
    std::vector<uint8_t> original, patched;
    std::string purpose;
};

struct Catalogue {
    std::vector<RetailPressing> retail;
    std::vector<PatchRange> patch;
    std::vector<CrackedRetail> cracked;
    std::vector<OneDiskCrunch> onedisk;
    std::vector<GameBuild> builds;
    // Disc 2 images refused by the SHA-1 of the whole disc (damaged copies, other builds).
    std::vector<RefusedDisc> refused_disc2;
    const RetailPressing* find_retail(const std::string& image_sha1) const;
    const CrackedRetail* find_cracked(const std::string& disc_sha1) const;
    const OneDiskCrunch* find_onedisk(const std::string& disc_sha1) const;
    // The build whose plain disk-1 image (at $70C00) has this SHA-1 / whose disc 2 has this SHA-1 (the compilation).
    const GameBuild* find_build_by_disk1_image(const std::string& image_sha1) const;
    const GameBuild* find_build_by_disc2(const std::string& disc2_sha1) const;
    // The build with this game image SHA-1 (the Power Pack, recognised after decoding), or by its tag.
    const GameBuild* find_build_by_game(const std::string& game_sha1) const;
    const GameBuild* find_build(const std::string& tag) const;
    const RefusedDisc* find_refused_disc2(const std::string& disc_sha1) const;
};

// The six level modules of the canonical disc 2 (sector ranges and the SHA-1 of each range), used to tell a disc 2 that
// differs only in unused sectors from one whose level data differ. The module numbers are 1 to 6.
struct Disc2Module {
    int first_sector, sectors;
    const char* sha1;
};
const std::vector<Disc2Module>& canonical_disc2_modules();
const Catalogue& builtin_catalogue();

// Applies `ranges` to `image` (as it lives at $400). False with the reason, and `image` untouched, if the image is
// too short or any range does not hold its expected original bytes. `applied` (optional) gets "$70F6+14"-style
// descriptions of the ranges changed.
bool apply_patch_ranges(std::vector<uint8_t>& image, const std::vector<PatchRange>& ranges, std::string& why,
                        std::vector<std::string>* applied = nullptr);

// The text printed before the protection patch is used (one line per entry).
const std::vector<std::string>& protection_patch_disclaimer();

// ---------------------------------------------------------------------------------------------------------
// The registry shown by `x2recomp --list-variants`.

enum class Recipe { Bs1OfsExecutable, RetailKeyTrack, RetailPatched, CrackedRetail, OneDiskCrunch, GameBuildPowerPack, GameBuildCompilation, CdtvDisc, Recognised };

struct VariantInfo {
    std::string id;
    std::string name;
    std::string identified_by;   // what the importer checks
    Recipe recipe = Recipe::Recognised;
    std::string game_sha1;       // expected game_0400.bin (empty: not an import target)
    std::string disc2_sha1;      // expected disc2.bin
    bool canonical = false;      // the image the replay goldens were recorded with
    bool protected_ = false;     // the original disc is copy-protected
    bool supported = false;      // can be imported
    bool trainer = false;        // a trainer build: gameplay differs from the game
    std::string build;           // the manifest's build= tag (empty: canonical-shaped image, no tag)
    std::string note;
};
std::vector<VariantInfo> all_variants();

} // namespace x2::import
