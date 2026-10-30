// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// WHDLoad installs of Xenon 2: the slave file that marks one, and the retail disk 1 images (WHDLoad `Disk.1`, or any
// 880 KB image of a retail disk 1) told apart by the CRC-16/ARC of their 1 KB boot block, which is how the install's own
// imager selects the floppy version. The imager and slave sources are catalogued in docs/re-notes/sources.md and
// described in docs/re-notes/whdload.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace x2::import {

struct WhdSlave {
    uint16_t version = 0; // WHDLoad slave structure version
    uint16_t flags = 0;
    std::string name, copyright, info; // ws_name, ws_copy, ws_info
};

// True if `data` is a WHDLoad slave (an AmigaOS executable whose first code is `moveq #-1,d0; rts` followed by the
// "WHDLOADS" marker); fills in the slave's strings.
bool parse_whdload_slave(const uint8_t* data, size_t size, WhdSlave& out);

struct RetailDisk1 {
    uint16_t boot_crc; // CRC-16/ARC of the first 1024 bytes
    const char* version; // "v3"
    const char* what;
    bool two_disk;     // part of the retail two-disk release: disk 2 is the canonical disc 2
};

// Identifies a Xenon 2 retail disk 1 image (880 KB ADF, or a WHDLoad `Disk.1` of the one-disk Power Pack layout)
// by its boot block; null for anything else.
const RetailDisk1* identify_retail_disk1(const uint8_t* data, size_t size);

} // namespace x2::import
