// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// HxC floppy emulator images (HFE). The format stores bit cells (here: raw MFM), not flux timing, so no PLL is needed:
// the track bits go straight to the Amiga MFM sector decoder.
//
//   0x00  "HXCPICFE" (v1) or "HXCHFEV3"      0x0A  track count (cylinders)       0x0B  sides
//   0x0C  track encoding (0 ISO MFM, 1 Amiga MFM, 2 ISO FM, ...)                 0x0D  bit rate in kbit/s, 16-bit LE
//   0x12  interface mode, 0x14 track list offset in 512-byte blocks (16-bit LE)
//
// The track list is one 4-byte entry per cylinder (offset in 512-byte blocks, length in bytes, both 16-bit LE).
// A track's data is stored in 512-byte blocks, the first 256 bytes of each for side 0 and the next 256 for side 1;
// bytes hold the cells least significant bit first. v3 adds opcode bytes 0xF0..0xF4 inside the data (NOP, set
// index, set bit rate, skip bits, random): handled here on a best-effort basis (not tested against real v3 files).
#include "import/flux.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

uint8_t reverse_bits(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}
} // namespace

bool read_hfe(const std::vector<uint8_t>& d, DiskImage& out, std::string& why, bool& high_density, std::vector<std::string>* warnings) {
    out = DiskImage();
    high_density = false;
    bool v3 = d.size() >= 8 && std::memcmp(d.data(), "HXCHFEV3", 8) == 0;
    if (d.size() < 512 || (!v3 && std::memcmp(d.data(), "HXCPICFE", 8) != 0)) {
        why = "not an HFE image (too short or no HXCPICFE header)";
        return false;
    }
    int tracks = d[9], sides = d[10], encoding = d[11], bitrate = rd16(&d[12]);
    size_t list_off = size_t(rd16(&d[18])) * 512;
    if (sides < 1 || sides > 2) { why = "HFE header names " + std::to_string(sides) + " sides"; return false; }
    if (tracks < 1 || tracks > 84) { why = "HFE header names " + std::to_string(tracks) + " cylinders"; return false; }
    if (bitrate >= 400) { high_density = true; why = "HFE image of a high-density disk"; return false; }
    if (encoding != 0 && encoding != 1) {
        why = "HFE image with track encoding " + std::to_string(encoding) + " (only MFM, ISO or Amiga, is read)";
        return false;
    }
    if (list_off == 0 || list_off + size_t(tracks) * 4 > d.size()) { why = "HFE track list lies outside the file"; return false; }
    if (warnings && v3) warnings->push_back("HFE v3 support is untested against real files");
    for (int cyl = 0; cyl < tracks; ++cyl) {
        size_t off = size_t(rd16(&d[list_off + 4 * size_t(cyl)])) * 512, len = rd16(&d[list_off + 4 * size_t(cyl) + 2]);
        if (len == 0) continue;
        if (off == 0 || off > d.size()) { why = "HFE cylinder " + std::to_string(cyl) + ": data offset is outside the file"; return false; }
        if (off + len / 2 > d.size()) { why = "HFE cylinder " + std::to_string(cyl) + ": data runs past the end of the file"; return false; }
        for (int side = 0; side < sides; ++side) {
            size_t side_bytes = len / 2;
            std::vector<uint8_t> raw;
            raw.reserve(side_bytes);
            for (size_t j = 0; j < side_bytes; ++j) {
                size_t at = off + (j / 256) * 512 + size_t(side) * 256 + (j % 256);
                if (at >= d.size()) break;
                raw.push_back(d[at]);
            }
            // bytes -> MFM bits, most significant first
            std::vector<uint8_t> bits;
            uint32_t nbits = 0;
            if (!v3) {
                bits.reserve(raw.size());
                for (uint8_t b : raw) bits.push_back(reverse_bits(b));
                nbits = uint32_t(bits.size() * 8);
            } else {
                uint32_t skip = 0;
                auto put_bit = [&](int bit) {
                    if ((nbits & 7) == 0) bits.push_back(0);
                    if (bit) bits.back() = uint8_t(bits.back() | (0x80 >> (nbits & 7)));
                    ++nbits;
                };
                for (size_t j = 0; j < raw.size(); ++j) {
                    uint8_t b = raw[j];
                    if ((b & 0xF8) == 0xF0 && b <= 0xF4) {
                        if ((b == 0xF2 || b == 0xF3) && j + 1 < raw.size()) {
                            if (b == 0xF3) skip = raw[j + 1] & 7;
                            ++j;
                        } else if (b == 0xF4 && j + 1 < raw.size()) {
                            ++j; // random (weak) cells: take them as zeros
                            for (int k = 0; k < 8; ++k) put_bit(0);
                        }
                        continue;
                    }
                    uint8_t r = reverse_bits(b);
                    for (int k = 0; k < 8; ++k) {
                        if (skip) { --skip; continue; }
                        put_bit((r >> (7 - k)) & 1);
                    }
                }
            }
            int track_number = cyl * 2 + side;
            Track& tr = out.track(track_number);
            decode_amiga_mfm_track(bits.data(), nbits, track_number, tr);
            bool any = false;
            for (SectorStatus s : tr.status) any = any || s != SectorStatus::Missing;
            if (!any) {
                tr = Track();
                tr.kind = Track::Kind::Raw;
                tr.mfm = std::move(bits);
                tr.bit_length = nbits;
            }
        }
    }
    return true;
}

} // namespace x2::import
