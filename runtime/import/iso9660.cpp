// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/iso9660.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {

constexpr uint32_t kBlock = 2048, kRawSector = 2352;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

uint32_t blocks_for(uint32_t bytes) { return uint32_t((uint64_t(bytes) + kBlock - 1) / kBlock); }

} // namespace

const uint8_t* IsoVolume::block(uint32_t lba) const {
    if (lba >= image_blocks_) return nullptr;
    return raw_ ? data_ + size_t(lba) * kRawSector + 16 : data_ + size_t(lba) * kBlock;
}

bool IsoVolume::extent_ok(uint32_t lba, uint32_t bytes) const {
    const uint64_t end = uint64_t(lba) + blocks_for(bytes);
    return end <= std::min(volume_blocks_, image_blocks_);
}

bool IsoVolume::open(const uint8_t* data, size_t size, std::string& why) {
    data_ = data;
    size_ = size;
    static const uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    const uint8_t* pvd = nullptr;
    if (data && size >= size_t(17) * kRawSector && std::memcmp(data + size_t(16) * kRawSector, kSync, 12) == 0 &&
        data[size_t(16) * kRawSector + 15] == 1) {
        raw_ = true;
        image_blocks_ = uint32_t(size / kRawSector);
        pvd = data + size_t(16) * kRawSector + 16;
    } else if (data && size >= size_t(17) * kBlock) {
        raw_ = false;
        image_blocks_ = uint32_t(size / kBlock);
        pvd = data + size_t(16) * kBlock;
    }
    if (!pvd || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0 || pvd[6] != 1) {
        why = "no ISO 9660 primary volume descriptor at sector 16";
        return false;
    }
    if (le16(pvd + 128) != kBlock) { why = "unsupported logical block size " + std::to_string(le16(pvd + 128)); return false; }
    volume_blocks_ = le32(pvd + 80);
    if (volume_blocks_ == 0 || volume_blocks_ < 17) { why = "implausible volume size in the descriptor"; return false; }
    if (volume_blocks_ > image_blocks_) {
        why = "the volume (" + std::to_string(volume_blocks_) + " blocks) is larger than the image (" + std::to_string(image_blocks_) + " blocks)";
        return false;
    }
    volume_id_.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    while (!volume_id_.empty() && (volume_id_.back() == ' ' || volume_id_.back() == '\0')) volume_id_.pop_back();
    const uint8_t* root = pvd + 156;
    if (root[0] < 34 || !(root[25] & 2)) { why = "the root directory record is damaged"; return false; }
    root_lba_ = le32(root + 2);
    root_bytes_ = le32(root + 10);
    if (root_bytes_ == 0 || root_bytes_ > kIsoMaxDirBytes || !extent_ok(root_lba_, root_bytes_)) {
        why = "the root directory lies outside the volume";
        return false;
    }
    return true;
}

bool IsoVolume::read_dir(uint32_t lba, uint32_t bytes, std::string prefix, int depth, std::vector<IsoEntry>& out,
                         std::vector<std::pair<uint32_t, uint32_t>>& dir_extents, size_t& entries, std::string& why) const {
    if (depth > kIsoMaxDepth) { why = "directories nested deeper than " + std::to_string(kIsoMaxDepth); return false; }
    for (const auto& d : dir_extents)
        if (d.first == lba) { why = "directory loop (extent at block " + std::to_string(lba) + " seen twice)"; return false; }
    if (bytes == 0 || bytes > kIsoMaxDirBytes || !extent_ok(lba, bytes)) { why = "a directory lies outside the volume"; return false; }
    dir_extents.emplace_back(lba, blocks_for(bytes));
    std::vector<uint8_t> dir(size_t(blocks_for(bytes)) * kBlock);
    for (uint32_t i = 0; i < blocks_for(bytes); ++i) std::memcpy(dir.data() + size_t(i) * kBlock, block(lba + i), kBlock);
    size_t pos = 0;
    while (pos < bytes) {
        const uint32_t len = dir[pos];
        if (len == 0) { pos = (pos / kBlock + 1) * kBlock; continue; }
        if (len < 34 || pos + len > bytes) { why = "a directory record runs past the end of its directory"; return false; }
        const uint8_t* rec = &dir[pos];
        const uint32_t name_len = rec[32];
        if (33 + name_len > len) { why = "a directory record's name runs past its end"; return false; }
        pos += len;
        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) continue; // "." and ".."
        if (++entries > kIsoMaxEntries) { why = "more than " + std::to_string(kIsoMaxEntries) + " directory entries"; return false; }
        std::string name(reinterpret_cast<const char*>(rec + 33), name_len);
        if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name == "..") {
            why = "a directory record has an unusable name";
            return false;
        }
        const uint32_t ext_lba = le32(rec + 2), ext_size = le32(rec + 10);
        if (rec[25] & 2) {
            if (!read_dir(ext_lba, ext_size, prefix + name + "/", depth + 1, out, dir_extents, entries, why)) return false;
        } else {
            if (ext_size && !extent_ok(ext_lba, ext_size)) {
                why = "file " + prefix + name + " lies outside the volume (block " + std::to_string(ext_lba) + ", " + std::to_string(ext_size) + " bytes)";
                return false;
            }
            IsoEntry e;
            e.path = prefix + name;
            e.name = name;
            e.lba = ext_lba;
            e.size = ext_size;
            out.push_back(std::move(e));
        }
    }
    return true;
}

bool IsoVolume::list(std::vector<IsoEntry>& out, std::string& why) const {
    out.clear();
    std::vector<std::pair<uint32_t, uint32_t>> dir_extents;
    size_t entries = 0;
    if (!read_dir(root_lba_, root_bytes_, "", 0, out, dir_extents, entries, why)) return false;
    for (const IsoEntry& e : out) {
        if (!e.size) continue;
        const uint64_t fe = uint64_t(e.lba) + blocks_for(e.size);
        for (const auto& d : dir_extents)
            if (e.lba < uint64_t(d.first) + d.second && d.first < fe) {
                why = "file " + e.path + " overlaps a directory extent";
                return false;
            }
    }
    return true;
}

bool IsoVolume::read(const IsoEntry& e, std::vector<uint8_t>& out, std::string& why) const {
    out.clear();
    if (e.size && !extent_ok(e.lba, e.size)) { why = e.path + ": extent lies outside the volume"; return false; }
    out.resize(e.size);
    for (uint32_t done = 0, b = 0; done < e.size; ++b) {
        const uint8_t* p = block(e.lba + b);
        if (!p) { why = e.path + ": extent lies outside the image"; return false; }
        const uint32_t n = std::min<uint32_t>(kBlock, e.size - done);
        std::memcpy(out.data() + done, p, n);
        done += n;
    }
    return true;
}

} // namespace x2::import
