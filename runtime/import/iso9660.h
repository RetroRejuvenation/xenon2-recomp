// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A bounds-checked, read-only ISO 9660 (Level 1, no extensions) reader over an in-memory data track: raw MODE1/2352
// sectors (a Redump .bin) or cooked 2048-byte blocks (an .iso). Used by the CDTV importer to find and read the files
// of the user's own Xenon 2 CDTV disc. Every directory record, extent and size is checked against the volume and the
// image, directory loops are refused, and the amount listed is limited, so a damaged or hostile image cannot make it
// read outside the buffer or run without end.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr size_t kIsoMaxEntries = 4096;   // files and directories in the whole volume
constexpr int kIsoMaxDepth = 8;           // directory nesting
constexpr uint32_t kIsoMaxDirBytes = 1u << 20;

struct IsoEntry {
    std::string path;  // "DEVS/SYSTEM-CONFIGURATION" (version suffix removed)
    std::string name;  // last component
    uint32_t lba = 0;  // first logical block of the extent
    uint32_t size = 0; // bytes
};

class IsoVolume {
public:
    // `data` must stay alive and unchanged while the volume is used. False with the reason when sector 16 does not hold
    // a primary volume descriptor in either layout or the descriptor is inconsistent.
    bool open(const uint8_t* data, size_t size, std::string& why);

    const std::string& volume_id() const { return volume_id_; }
    uint32_t volume_blocks() const { return volume_blocks_; }
    uint32_t image_blocks() const { return image_blocks_; }
    bool raw() const { return raw_; }

    // Every file (not directory) of the volume, in directory order. False with the reason for a damaged volume.
    bool list(std::vector<IsoEntry>& out, std::string& why) const;
    // Copies a file's bytes. False with the reason when the extent lies outside the volume or the image.
    bool read(const IsoEntry& e, std::vector<uint8_t>& out, std::string& why) const;

private:
    const uint8_t* block(uint32_t lba) const; // 2048 user bytes of block `lba`, or nullptr outside the image
    bool read_dir(uint32_t lba, uint32_t bytes, std::string prefix, int depth, std::vector<IsoEntry>& out,
                  std::vector<std::pair<uint32_t, uint32_t>>& dir_extents, size_t& entries, std::string& why) const;
    bool extent_ok(uint32_t lba, uint32_t bytes) const;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool raw_ = false;
    uint32_t volume_blocks_ = 0, image_blocks_ = 0, root_lba_ = 0, root_bytes_ = 0;
    std::string volume_id_;
};

} // namespace x2::import
