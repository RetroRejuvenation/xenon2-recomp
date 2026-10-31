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
    int quality; // higher is preferred when several candidates are given
};

const KnownDisc* find_bs1_disc1(const std::string& sha1);
const KnownDisc* find_bs1_disc2(const std::string& sha1);
const KnownDisc* find_other_release(const std::string& sha1);  // single-disk cracks: recognised, refused
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
    const RetailPressing* find_retail(const std::string& image_sha1) const;
};
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

enum class Recipe { Bs1OfsExecutable, RetailKeyTrack, RetailPatched, Recognised };

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
    std::string note;
};
std::vector<VariantInfo> all_variants();

} // namespace x2::import
