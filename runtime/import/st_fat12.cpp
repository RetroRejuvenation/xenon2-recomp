// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/st_fat12.h"

#include <algorithm>
#include <cstring>

namespace x2::import {
namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

struct Layout {
    FatInfo info;
    size_t fat_start = 0, root_start = 0, root_sectors = 0, data_start = 0; // in 512-byte sectors
};

bool layout_of(const uint8_t* img, size_t size, Layout& L, std::string& why) {
    if (size < 512 || size % 512 != 0) { why = "image is not a whole number of sectors"; return false; }
    FatInfo& i = L.info;
    i.bytes_per_sector = le16(img + 11);
    i.sectors_per_cluster = img[13];
    i.reserved = le16(img + 14);
    i.fats = img[16];
    i.root_entries = le16(img + 17);
    i.total_sectors = le16(img + 19);
    if (i.total_sectors == 0) i.total_sectors = le32(img + 32);
    i.media = img[21];
    i.fat_sectors = le16(img + 22);
    for (int k = 2; k < 8; ++k) i.oem += (img[k] >= 0x20 && img[k] < 0x7F) ? char(img[k]) : '.';
    if (i.bytes_per_sector != 512) { why = "bytes per sector " + std::to_string(i.bytes_per_sector) + " (only 512 is supported)"; return false; }
    if (i.sectors_per_cluster == 0 || i.sectors_per_cluster > 8 || (i.sectors_per_cluster & (i.sectors_per_cluster - 1))) {
        why = "sectors per cluster " + std::to_string(i.sectors_per_cluster);
        return false;
    }
    if (i.reserved < 1 || i.fats < 1 || i.fats > 2 || i.fat_sectors < 1 || i.root_entries < 1 || i.root_entries > 512) {
        why = "the BPB is not a FAT12 floppy BPB";
        return false;
    }
    const size_t image_sectors = size / 512;
    L.fat_start = i.reserved;
    L.root_start = L.fat_start + size_t(i.fats) * i.fat_sectors;
    L.root_sectors = (size_t(i.root_entries) * 32 + 511) / 512;
    L.data_start = L.root_start + L.root_sectors;
    if (L.data_start >= image_sectors) { why = "the BPB places the data area outside the image"; return false; }
    // A BPB may describe one side of a two-sided image (Kixx does); only the layout must fit the image.
    const size_t total = i.total_sectors && i.total_sectors <= image_sectors ? i.total_sectors : image_sectors;
    i.clusters = unsigned((total - L.data_start) / i.sectors_per_cluster);
    if (i.clusters < 1 || i.clusters > 4084) { why = "cluster count " + std::to_string(i.clusters) + " is not FAT12"; return false; }
    // A raw-track disk may keep a stale BPB (the Kixx boot sector does): a real FAT starts with the media descriptor and two $FF.
    const uint8_t* fat = img + L.fat_start * 512;
    if (i.media < 0xF0 || fat[0] != i.media || fat[1] != 0xFF || fat[2] != 0xFF) {
        why = "the first FAT sector does not start with the media descriptor, so the BPB is not backed by a file system";
        return false;
    }
    return true;
}

// FAT12 entry for cluster n; 0xFFFF on an out-of-range read.
unsigned fat_entry(const uint8_t* img, const Layout& L, unsigned n) {
    const size_t off = L.fat_start * 512 + size_t(n) * 3 / 2;
    if (off + 1 >= L.fat_start * 512 + size_t(L.info.fat_sectors) * 512) return 0xFFFF;
    const unsigned v = le16(img + off);
    return (n & 1) ? (v >> 4) : (v & 0x0FFF);
}

// The clusters of a chain, at most `limit`; false if the chain loops, leaves the data area or is cut off.
bool chain_of(const uint8_t* img, const Layout& L, unsigned first, size_t limit, std::vector<unsigned>& out, std::string& why) {
    out.clear();
    unsigned n = first;
    while (n >= 2 && n < 0xFF8) {
        if (n >= L.info.clusters + 2) { why = "cluster " + std::to_string(n) + " is outside the data area"; return false; }
        if (out.size() >= limit) { why = "cluster chain longer than the volume (a loop)"; return false; }
        out.push_back(n);
        n = fat_entry(img, L, n);
    }
    if (n < 0xFF8 && !out.empty()) { why = "cluster chain ends in " + std::to_string(n) + " (damaged or free)"; return false; }
    return true;
}

const uint8_t* cluster_bytes(const uint8_t* img, size_t size, const Layout& L, unsigned n, size_t& len) {
    const size_t sector = L.data_start + size_t(n - 2) * L.info.sectors_per_cluster;
    len = size_t(L.info.sectors_per_cluster) * 512;
    if ((sector + L.info.sectors_per_cluster) * 512 > size) return nullptr;
    return img + sector * 512;
}

std::string short_name(const uint8_t* e) {
    std::string name, ext;
    for (int k = 0; k < 8 && e[k] != ' '; ++k) name += char(e[k] >= 0x20 && e[k] < 0x7F ? e[k] : '?');
    for (int k = 8; k < 11 && e[k] != ' '; ++k) ext += char(e[k] >= 0x20 && e[k] < 0x7F ? e[k] : '?');
    return ext.empty() ? name : name + "." + ext;
}

struct Lister {
    const uint8_t* img;
    size_t size;
    const Layout& L;
    std::vector<FatEntry>& out;
    std::vector<std::string>& warnings;
    FatInfo& info;
    static constexpr size_t kMaxEntries = 1024;

    void directory(const uint8_t* data, size_t len, const std::string& prefix, int depth, bool root) {
        for (size_t o = 0; o + 32 <= len && out.size() < kMaxEntries; o += 32) {
            const uint8_t* e = data + o;
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5) continue;
            const uint8_t attr = e[11];
            if (attr == 0x0F) continue; // long-name piece
            if (attr & 0x08) { // volume label
                if (root) {
                    info.label.clear();
                    for (int k = 0; k < 11; ++k) info.label += char(e[k] >= 0x20 && e[k] < 0x7F ? e[k] : '?');
                    while (!info.label.empty() && info.label.back() == ' ') info.label.pop_back();
                }
                continue;
            }
            // "." and ".." (TOS writes odd extension bytes after them)
            if (e[0] == '.' && (e[1] == ' ' || e[1] == 0 || (e[1] == '.' && (e[2] == ' ' || e[2] == 0)))) continue;
            const std::string name = short_name(e);
            FatEntry f;
            f.path = prefix + name;
            f.size = le32(e + 28);
            f.first_cluster = le16(e + 26);
            f.directory = (attr & 0x10) != 0;
            out.push_back(f);
            if (f.directory) {
                if (depth >= 4) { warnings.push_back(f.path + ": directories nested too deeply, not listed"); continue; }
                std::vector<unsigned> chain;
                std::string why;
                if (!chain_of(img, L, f.first_cluster, size_t(L.info.clusters) + 1, chain, why)) {
                    warnings.push_back(f.path + ": " + why);
                    continue;
                }
                std::vector<uint8_t> bytes;
                for (unsigned c : chain) {
                    size_t clen = 0;
                    const uint8_t* cp = cluster_bytes(img, size, L, c, clen);
                    if (!cp) { warnings.push_back(f.path + ": cluster outside the image"); break; }
                    bytes.insert(bytes.end(), cp, cp + clen);
                    if (bytes.size() > (size_t(1) << 20)) break;
                }
                directory(bytes.data(), bytes.size(), f.path + "/", depth + 1, false);
            }
        }
    }
};

} // namespace

bool fat12_info(const uint8_t* image, size_t size, FatInfo& info, std::string& why) {
    Layout L;
    if (!layout_of(image, size, L, why)) return false;
    info = L.info;
    return true;
}

bool fat12_list(const uint8_t* image, size_t size, FatInfo& info, std::vector<FatEntry>& out, std::vector<std::string>& warnings,
                std::string& why) {
    Layout L;
    if (!layout_of(image, size, L, why)) return false;
    info = L.info;
    out.clear();
    if ((L.root_start + L.root_sectors) * 512 > size) { why = "the root directory runs past the image"; return false; }
    Lister lister{image, size, L, out, warnings, info};
    lister.directory(image + L.root_start * 512, L.root_sectors * 512, "", 0, true);
    return true;
}

bool fat12_read(const uint8_t* image, size_t size, const FatEntry& entry, std::vector<uint8_t>& out, std::string& why, size_t max_bytes) {
    Layout L;
    if (!layout_of(image, size, L, why)) return false;
    out.clear();
    if (entry.directory) { why = "a directory"; return false; }
    if (entry.size > max_bytes) { why = "file too large"; return false; }
    if (entry.size == 0) return true;
    std::vector<unsigned> chain;
    if (!chain_of(image, L, entry.first_cluster, size_t(L.info.clusters) + 1, chain, why)) return false;
    out.reserve(entry.size);
    for (unsigned c : chain) {
        size_t clen = 0;
        const uint8_t* cp = cluster_bytes(image, size, L, c, clen);
        if (!cp) { why = "cluster outside the image"; return false; }
        const size_t take = std::min(clen, size_t(entry.size) - out.size());
        out.insert(out.end(), cp, cp + take);
        if (out.size() >= entry.size) break;
    }
    if (out.size() != entry.size) { why = "chain shorter than the file size"; out.clear(); return false; }
    return true;
}

} // namespace x2::import
