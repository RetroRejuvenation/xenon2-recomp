// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// High-level replacement for the game's track loader (see docs/re-notes/disk-layout.md section 5).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace x2 {

class Machine;

class DiskShim {
public:
    static constexpr uint32_t kLoadSectors = 0x921A; // load_sectors(a0=dest, d0.w=sector, d1.w=count) -> d0=0
    static constexpr uint32_t kStagingBuffer = 0x44000; // loader's decoded-track buffer (11 sectors)
    static constexpr uint32_t kSectorSize = 512;
    static constexpr uint32_t kSectorsPerTrack = 11;

    // `loader_pc` is where the build keeps its disk loader ($921A in the canonical build, boot.h BuildInfo).
    // `sector_bias` is added to the sector number the game passes (the PDos game numbers its sectors one above the image's),
    // `sectors_per_track` is the track size of the build's disk (the staging buffer holds the last track touched).
    explicit DiskShim(std::vector<uint8_t> disc2, uint32_t loader_pc = kLoadSectors, int32_t sector_bias = 0,
                      uint32_t sectors_per_track = kSectorsPerTrack)
        : disc2_(std::move(disc2)), loader_pc_(loader_pc), bias_(sector_bias), per_track_(sectors_per_track) {}
    void install(Machine& m);

    // Perform one load_sectors call on the current CPU state and return to the caller.
    void load_sectors(Machine& m);

    uint32_t calls = 0;
    uint32_t sectors_read = 0;
    uint32_t bad_requests = 0;
    uint16_t last_sector = 0, last_count = 0;
    uint32_t last_dest = 0;
    // Optional observer of every successful load (dest, start sector, count, the copied disc bytes).
    std::function<void(uint32_t dest, uint16_t sector, uint16_t count, const uint8_t* data)> on_load;

private:
    std::vector<uint8_t> disc2_;
    uint32_t loader_pc_;
    int32_t bias_;
    uint32_t per_track_;
};

} // namespace x2
