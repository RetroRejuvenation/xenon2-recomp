// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The retail Xenon 2 disk 1 (docs/re-notes/retail-disk1.md, retail-protection.md): the Rob Northen Copylock key
// track on cylinder 0 head 1 and the game image stored on the disc.
//
// The key track is verified, never replayed: the importer checks that the track holds the eleven Copylock sync words
// in order with index bytes 0 to 10, the "Rob Northen Comp" signature in sector 6 and, in sector 0, the 512 bytes of
// the 23-bit LFSR sequence the track's seed generates (the layout is documented in keirf's public-domain
// disk-utilities, libdisk/format/amiga/copylock.c; this is an independent re-implementation of the detection). The
// result is the receipt `copylock.txt` (the runtime reads it: runtime/retail_protection.h).
//
// The game image ($3FC00 bytes) sits at disk offset $70C00 as a 16-bit-word XOR chain: stored[k] = plain[k] ^
// plain[k-1] with plain[-1] = $002A. Undoing it is arithmetic and needs no key.
#pragma once
#include "import/disk_image.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kKeyTrack = 1;                   // cylinder 0, head 1
constexpr size_t kRetailImageOffset = 0x70C00; // in the 880 KB disc image
constexpr size_t kRetailImageSize = 0x3FC00;
constexpr uint32_t kRetailChainInit = 0x002A;
constexpr uint32_t kRetailStubAddress = 0x70F6; // the 14-byte Copylock entry stub inside the game image
constexpr size_t kRetailStubLength = 14;

struct CopylockReport {
    bool genuine = false;
    uint32_t seed = 0;     // the 23-bit LFSR seed (non-zero when genuine)
    int syncs_found = 0;   // of the eleven Copylock sync words, found with the right index byte
    bool signature = false;
    int lfsr_bytes = 0;    // sector 0 data bytes that match the LFSR sequence (512 when genuine)
    std::string why;       // when not genuine: what is missing
};

// Verifies `t` (the Raw MFM bits of cylinder 0 head 1) as a Copylock key track.
CopylockReport verify_copylock_track(const Track& t);

// The text of the receipt for a verified key track; byte for byte what runtime/retail_protection.cpp
// (format_copylock_datum) writes and parses.
std::string copylock_receipt(uint32_t seed);

std::vector<uint8_t> chain_decode(const uint8_t* stored, size_t n, uint32_t init = kRetailChainInit);

// The game image of a retail disc 1, decoded from the sectors at $70C00. False with the reason if those sectors are not
// all intact (`why` names the tracks).
bool decode_retail_image(const DiskImage& disc, std::vector<uint8_t>& image, std::string& why);
// The same from a plain 880 KB ADF.
bool decode_retail_image_adf(const uint8_t* adf, size_t size, std::vector<uint8_t>& image);

// True if `image` (as it lives at $400) has the retail entry (`bra.w $7032` at $400) and the Copylock entry stub at
// $70F6: the register-saving prologue (the stub's `lea x(pc)` displacement is not compared).
bool is_retail_image(const std::vector<uint8_t>& image);

} // namespace x2::import
