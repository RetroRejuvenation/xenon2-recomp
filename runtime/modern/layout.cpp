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
};

} // namespace x2::modern
