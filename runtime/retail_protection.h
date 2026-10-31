// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The in-game Copylock check of the RETAIL Xenon 2 images (docs/re-notes/retail-protection.md).
//
// A game image decoded from an original retail disk 1 still contains Rob Northen Copylock's entry stub at $70F6.
// Run for real, that routine decrypts itself one instruction at a time under the 68000 trace exception, drives
// the floppy drive (motor, index pulses, a raw DMA read of the key track, per-sector timing) and finally leaves
// through the privilege-violation vector to $763A. The runtime has no floppy hardware and Musashi is built
// without trace exceptions, so it cannot run that. Instead, this module answers the check the way the original
// drive would have: the importer verifies the key track of the user's own disc (the Copylock signature, the
// sync order and the LFSR data, tools/copylock.py) and records that in `copylock.txt` next to the game image;
// when that receipt is present the runtime continues at $763A, exactly where the real routine ends up. The image
// bytes are never changed, and no key value appears in the program.
//
// Without the receipt a retail image is refused, because running it would crash. An image without the stub (the
// canonical, already unprotected image) is not touched at all.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace x2 {

class Machine;
struct GameAssets;

constexpr uint32_t kRetailCopylockEntry = 0x70F6; // the 14-byte stub the real routine starts with
constexpr uint32_t kRetailCopylockExit = 0x763A;  // its exit: the privilege-violation vector it installs
constexpr uint32_t kCopylockStubLength = 14;
constexpr const char* kCopylockDatumFile = "copylock.txt";

// What the importer saw on the user's disc. Only `present` and a plausible seed matter to the runtime; the
// seed is reported so a bug report can tell which key track was verified.
struct CopylockDatum {
    bool present = false;
    uint32_t seed = 0;      // the 23-bit LFSR seed of the key track (non-zero)
    std::string image_sha1; // lower-case hex SHA-1 of the game image the receipt was written for (version 2; empty in version 1)
};

// Text form, one `name value` pair per line:
//   x2recomp-copylock 2
//   signature rob-northen-comp
//   syncs ok
//   seed 0x36FA43
//   image-sha1 <40 hex digits>
// The image line binds the receipt to the game image it was written for, so a receipt copied or written by hand next to
// another image arms nothing. Version 1 (no image line) is still parsed but is refused by install_retail_protection.
// Returns false with a reason for anything else (wrong version, a missing line, a zero or oversized seed, a bad hash).
bool parse_copylock_datum(const std::string& text, CopylockDatum& out, std::string& why);
std::string format_copylock_datum(uint32_t seed, const std::string& image_sha1);

// Reads <dir>/copylock.txt. A missing file is not an error (out.present stays false, why empty); an unreadable
// or invalid one is (false, why set).
bool load_copylock_datum(const std::string& dir, CopylockDatum& out, std::string& why);

// True if the image (as it lives at $400, game_0400.bin) starts the in-game Copylock routine at $70F6: the
// register-saving prologue, `move.l a6,-(a7); lea x(pc),a6; movem.l d0-d7/a0-a7,(a6); lea $40(a6),a6`.
bool is_retail_copylock_image(const uint8_t* image, size_t size);

enum class RetailProtection {
    NotRetail,  // the image has no Copylock stub: nothing installed
    Installed,  // retail image with a key-track receipt: the check at $70F6 is answered
    MissingKey, // retail image without a receipt: do not run it
    UnboundKey, // a version 1 receipt, which names no image: re-import the disc
    WrongImage, // the receipt was written for a different game image: re-import the disc
};

// Installs the PC trap for a retail image. `image_sha1` is the SHA-1 (lower-case hex) of assets.game_image, computed by the
// caller (the importer library owns the hash); the receipt must name exactly that image. Call once after boot_game(); the
// trap survives resets.
RetailProtection install_retail_protection(Machine& m, const GameAssets& assets, const CopylockDatum& key,
                                           const std::string& image_sha1);

} // namespace x2
