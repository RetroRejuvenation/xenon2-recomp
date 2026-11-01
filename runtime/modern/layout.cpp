// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/layout.h"

namespace x2::modern {

const CaptureLayout kDiskLayout{
    "disk",
    0x406, 0x40A,
    {0xA88, 0xAEA, 0xB4C, 0xBAE, 0xC10},
    0x3678,
    {0x436, 0xCD0, 0xCCE, 0xCD6, 0xCD8, 0xCDC, 0xCEA, 0xCE8},
    0x42FDC,
    0x1832, 0x185A, 0x1884,
    0x18A0, 0x1EA2,
    0x233A, 0x23EE,
    {0x2370, 0x2398, 0x23C2},
    false,
    0xE04,         // level word
    kUnknown, 0,   // far ring height and scroll sign: to be measured (WS2)
    {192, 8, 8, 6},
};

// Mapped from the disk build by instruction shape (tools/modern/map_cdtv.py), every value voted by 3 to 38 aligned
// instruction pairs; the draw/show pointers sit at $408/$40C (disk $406/$40A), the object lists and the variable block
// at the disk addresses + 6, and the star table in hunk 1 at $85380 (disk $42FDC).
const CaptureLayout kCdtvLayout{
    "cdtv",
    0x408, 0x40C,
    {0xA8E, 0xAF0, 0xB52, 0xBB4, 0xC16},
    0x3674E,
    {0x43C, 0xCD6, 0xCD4, 0xCDC, 0xCDE, 0xCE2, 0xCF0, 0xCEE},
    0x85380,
    0x34712, 0x3473C, 0x34766,
    0x34784, 0x34DC6,
    0x35260, 0x35318,
    {0x3529A, 0x352C2, 0x352EC},
    true,
    0xE0A,         // level word (the disk address + 6)
    kUnknown, 0,   // far ring height and scroll sign: to be measured (WS2)
    {192, 8, 8, 6},
};

const CaptureLayout kStLayout{
    "st",
    kUnknownAddr, kUnknownAddr,
    {kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr},
    kUnknownAddr,
    {kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr},
    kUnknownAddr,
    kUnknownAddr, kUnknownAddr, kUnknownAddr,
    kUnknownAddr, kUnknownAddr,
    kUnknownAddr, kUnknownAddr,
    {kUnknownAddr, kUnknownAddr, kUnknownAddr},
    false,
    0xE04,         // the ST keeps the level word where the disk builds do (docs/re-notes/atari-st-s0.md)
    kUnknown, 0,
    {},            // the panel lines of the ST picture: to be measured (WS2)
};

const ScreenGeometry kAmigaScreen{"amiga", 0, 0x3A - 0x2C, 320, 200, 32};
const ScreenGeometry kStScreen{"st", 0, 0, 320, 200, 16};

const AudioLayout kDiskAudio{
    "disk",
    0x1B26A, 0x1B6D0,
    kUnknownAddr, kUnknownAddr,
    {0, 1, 1, 0},   // AUD0 and AUD3 left, AUD1 and AUD2 right
    0xE04,
    0x19F9E, 0x19FBA,
    0x8230,
    {0x7896, 0x7C50, 0x7CDA, 0x7D8C},
    kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr,
    kUnknownAddr,
    0x1B762,
};

// The CDTV program has no tracker: every sample start is an effect or speech, so there is no effect range; the speech buffer
// starts at $B645C (its end is to be measured: WS3b).
const AudioLayout kCdtvAudio{
    "cdtv",
    kUnknownAddr, kUnknownAddr,
    0xB645C, kUnknownAddr,
    {0, 1, 1, 0},
    0xE0A,
    kUnknownAddr, kUnknownAddr,
    kUnknownAddr,
    {kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr},
    0x3A924, 0x3A8F6, 0x3A95C, 0x3A9BA, 0x3A9E6,
    0x3A8F2,
    kUnknownAddr,
};

// The ST has no Paula: its YM effects are register patterns and its music entries are unmapped (spec 7, phase 2).
const AudioLayout kStAudio{
    "st",
    kUnknownAddr, kUnknownAddr,
    kUnknownAddr, kUnknownAddr,
    {0xFF, 0xFF, 0xFF, 0xFF},
    0xE04,
    kUnknownAddr, kUnknownAddr,
    kUnknownAddr,
    {kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr},
    kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr, kUnknownAddr,
    kUnknownAddr,
    kUnknownAddr,
};

const CaptureLayout& capture_layout_for(pack::GameFamily g) {
    switch (g) {
    case pack::GameFamily::Cdtv: return kCdtvLayout;
    case pack::GameFamily::St: return kStLayout;
    case pack::GameFamily::Disk: break;
    }
    return kDiskLayout;
}

const ScreenGeometry& screen_geometry_for(pack::GameFamily g) { return g == pack::GameFamily::St ? kStScreen : kAmigaScreen; }

const AudioLayout& audio_layout_for(pack::GameFamily g) {
    switch (g) {
    case pack::GameFamily::Cdtv: return kCdtvAudio;
    case pack::GameFamily::St: return kStAudio;
    case pack::GameFamily::Disk: break;
    }
    return kDiskAudio;
}

} // namespace x2::modern
