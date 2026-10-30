// SPDX-License-Identifier: BSD-3-Clause AND GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// IPF (SPS/CAPS) disk image reader. Portions derived from MAME's src/lib/formats/ipf_dsk.cpp (license:BSD-3-Clause,
// copyright-holders:Olivier Galibert); see ipf.cpp and docs/PROVENANCE.md. It does not use or link the SPS capsimg
// library (whose licence forbids redistribution).
//
// An IPF file is a chain of CAPS/INFO/IMGE/DATA records. Each track is described as blocks of encoded data and gaps
// (the way the original disk was mastered); read_ipf rebuilds every track as a stream of MFM cells and hands it to
// the shared disk layer: a track that holds the 11 standard Amiga sectors becomes a Decoded track, anything else
// (copy-protection tracks, unformatted tracks) stays a Raw track in the DiskImage, so DiskImage::to_adf reports it.
// Every record CRC and every payload CRC is checked; all offsets and sizes are bounds-checked and limited.
#pragma once
#include "import/disk_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

struct IpfInfo {
    uint32_t type = 0, encoder_type = 0, encoder_revision = 0, release = 0, revision = 0, origin = 0;
    uint32_t min_cylinder = 0, max_cylinder = 0, min_head = 0, max_head = 0;
    uint32_t credit_day = 0, credit_time = 0;
    uint32_t platform[4] = {0, 0, 0, 0};
    uint32_t extra[5] = {0, 0, 0, 0, 0};
};

// What the file said about one track and what was rebuilt from it (the same quantities tools/ipf.py prints).
struct IpfTrackInfo {
    uint32_t cylinder = 0, head = 0;
    uint32_t type = 0;        // IMGE track type: 1 noise, 2 standard, 3..9 timing-variant protection types
    uint32_t cells = 0;       // MFM cells in the rebuilt track (0: the track is empty)
    uint32_t index_cells = 0; // cells between the index pulse and the end of the track
    uint32_t blocks = 0;
    uint32_t weak_cells = 0;           // cells that read differently every revolution (stored as 0 bits)
    std::vector<uint16_t> timing_ns;   // the distinct nominal cell times, ascending (2000 for a normal track)
    bool standard = false;             // 11 intact Amiga sectors: the track is Decoded in the DiskImage
    int number() const { return int(cylinder * 2 + head); }
};

struct IpfDisk {
    IpfInfo info;
    std::vector<IpfTrackInfo> tracks; // in file order of (cylinder, head); empty tracks included
};

// Parses `data` into `out` (replacing it) and `disk`. False with `why` for a damaged, truncated or unsupported file
// (CRC error, bad record sizes, offsets outside the data, a track that cannot be rebuilt, a track outside 84
// cylinders x 2 sides, or a file or track larger than the limits).
bool read_ipf(const std::vector<uint8_t>& data, DiskImage& out, IpfDisk& disk, std::string& why);

} // namespace x2::import
