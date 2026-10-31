// SPDX-License-Identifier: BSD-3-Clause AND GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Portions derived from MAME's src/lib/formats/ipf_dsk.cpp, license:BSD-3-Clause, copyright-holders:Olivier Galibert
// (the record layouts, the gap/data description decoding, the track splice and the timing rules below follow that
// file; the structure here is a port of tools/ipf.py, which is itself a port of it).
//
// Differences from the reference, all of them rejections of damaged input rather than behaviour: every read is
// bounds-checked (the reference relies on exceptions or wrapped indices), a track may have at most 2^20 cells, a
// file at most 64 MB, a data index at most 1000, a pattern shorter than one bit with a non-empty cell count is an
// error, and a track outside 84 cylinders x 2 sides is refused because the DiskImage model has no place for it.
#include "import/ipf.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace x2::import {

namespace {

constexpr uint8_t kCell0 = 0, kCell1 = 1, kCellWeak = 2;
constexpr size_t kMaxFileBytes = size_t(64) << 20;
constexpr uint64_t kMaxTrackCells = uint64_t(1) << 20;
constexpr uint32_t kMaxDataIndex = 1000;

// CRC-32 (the zlib/PNG polynomial), the record CRC of the format. `skip` zeroes four bytes at offset 8 (the CRC
// field of a record header) while summing.
uint32_t crc_step(uint8_t byte, const std::array<uint32_t, 256>& t, uint32_t crc) { return t[(crc ^ byte) & 0xFF] ^ (crc >> 8); }

const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            a[i] = c;
        }
        return a;
    }();
    return t;
}

uint32_t ipf_crc32(const uint8_t* p, size_t n, bool zero_crc_field = false) {
    const auto& t = crc_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = crc_step(zero_crc_field && i >= 8 && i < 12 ? uint8_t(0) : p[i], t, crc);
    return ~crc;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A big-endian long at `o` of `d`, 0 outside it (every call with a file-controlled offset is range-checked by the
// caller; this keeps the fixed header offsets safe even if a check is ever missed).
uint32_t u32(const std::vector<uint8_t>& d, uint64_t o) { return o + 4 <= d.size() ? be32(&d[size_t(o)]) : 0; }

// One IMGE/DATA pair.
struct RawTrack {
    bool info_set = false, has_data = false;
    uint32_t cylinder = 0, head = 0, type = 0;
    uint32_t size_bytes = 0, index_bytes = 0, index_cells = 0, datasize_cells = 0, gapsize_cells = 0;
    uint64_t size_cells = 0;
    uint32_t block_count = 0, weak_bits = 0;
    std::vector<uint8_t> data;
};

// Rebuilds one track as cells. `d` is the DATA payload: block_count 32-byte block descriptors, then the data and
// gap descriptions they point to.
class Generator {
public:
    Generator(const std::vector<uint8_t>& d, uint64_t size_cells) : cells_(size_cells, kCell0), d_(d) {}

    std::vector<uint8_t> cells_;
    bool context_ = false;

    // --- writers -----------------------------------------------------------------------------------------

    // `cells` unencoded cells taken from the bits of `data` (the caller guarantees ceil(cells / 8) bytes).
    bool write_raw(uint64_t tpos, const uint8_t* data, uint64_t cells) {
        if (tpos + cells > cells_.size()) return false;
        for (uint64_t i = 0; i < cells; ++i) cells_[tpos + i] = (data[i >> 3] & (0x80 >> (i & 7))) ? kCell1 : kCell0;
        if (cells) context_ = cells_[tpos + cells - 1] == kCell1;
        return true;
    }

    // `cells` cells of MFM-encoded repetition of a `patlen_bits`-bit pattern (data bits, one clock cell inserted before
    // each), starting `start_offset` cells into the pattern (taken modulo 2^32, as the reference does).
    bool write_mfm(uint64_t tpos, const uint8_t* data, uint64_t data_bytes, uint32_t start_offset, uint64_t patlen_bits,
                   uint64_t cells) {
        if (cells == 0) return true;
        uint64_t patlen = patlen_bits * 2;
        if (patlen == 0 || patlen > 0xFFFFFFFFull || ((patlen - 1) >> 4) >= data_bytes) return false;
        if (tpos + cells > cells_.size()) return false;
        for (uint64_t i = 0; i < cells; ++i) {
            uint32_t pos = uint32_t(uint32_t(i) + start_offset) % uint32_t(patlen);
            bool bit = (data[pos >> 4] & (0x80 >> ((pos >> 1) & 7))) != 0;
            if (pos & 1) {
                cells_[tpos + i] = bit ? kCell1 : kCell0;
                context_ = bit;
            } else {
                cells_[tpos + i] = (context_ || bit) ? kCell0 : kCell1;
            }
        }
        return true;
    }

    bool write_weak(uint64_t tpos, uint64_t cells) {
        if (tpos + cells > cells_.size()) return false;
        std::fill_n(cells_.begin() + long(tpos), cells, kCellWeak);
        return true;
    }

    // --- block data (sync marks, MFM data, raw cells, weak bytes) -------------------------------------------

    bool block_data(uint64_t pos, uint64_t tpos, uint64_t tlimit, bool dmb, bool raw) {
        const uint64_t dlimit = d_.size();
        for (;;) {
            if (pos >= dlimit) return false;
            uint8_t val = d_[size_t(pos++)];
            unsigned n = val >> 5;
            if (n > dlimit - pos) return false;
            uint64_t param = 0;
            for (unsigned i = 0; i < n; ++i) param = param << 8 | d_[size_t(pos++)];
            uint64_t tleft = tlimit - tpos;
            uint64_t bitcount = dmb ? param : param * 8;
            uint64_t bytecount = (bitcount + 7) / 8;
            switch (val & 0x1F) {
            case 0:
                return tleft == 0;
            case 1: // sync mark, unencoded cells
                if (raw || bitcount > tleft || bytecount > dlimit - pos) return false;
                if (!write_raw(tpos, (d_.data() + pos), bitcount)) return false;
                tpos += bitcount;
                pos += bytecount;
                break;
            case 2:
            case 3: // MFM-encoded data / gap bytes
                if (raw || 2 * bitcount > tleft || bytecount > dlimit - pos) return false;
                if (!write_mfm(tpos, (d_.data() + pos), bytecount, 0, bitcount, 2 * bitcount)) return false;
                tpos += 2 * bitcount;
                pos += bytecount;
                break;
            case 4: // raw cell bits, sized in bytes whatever the size mode
                if (!raw) return false;
                bitcount = param * 8;
                bytecount = param;
                if (bitcount > tleft || bytecount > dlimit - pos) return false;
                if (!write_raw(tpos, (d_.data() + pos), bitcount)) return false;
                tpos += bitcount;
                pos += bytecount;
                break;
            case 5: // weak bytes
                if (raw || 2 * bitcount > tleft) return false;
                if (!write_weak(tpos, 2 * bitcount)) return false;
                tpos += 2 * bitcount;
                context_ = false;
                break;
            default:
                return false;
            }
        }
    }

    // --- gaps ----------------------------------------------------------------------------------------------

    struct Reserved {
        bool ok = false;
        int64_t size = 0;
        uint64_t pos = 0;
    };

    // The cells a gap description reserves for its fixed parts (sync-like elements), and where it ends.
    Reserved gap_reserved(uint64_t pos) const {
        const uint64_t dlimit = d_.size();
        Reserved r;
        int64_t res = 0;
        for (;;) {
            if (pos >= dlimit) return r;
            uint8_t val = d_[size_t(pos++)];
            unsigned n = val >> 5;
            if (n > dlimit - pos) return r;
            uint64_t param = 0;
            for (unsigned i = 0; i < n; ++i) param = param << 8 | d_[size_t(pos++)];
            switch (val & 0x1F) {
            case 0:
                r.ok = true;
                r.size = res;
                r.pos = pos;
                return r;
            case 1:
                res += int64_t(param) * 2;
                if (res > (int64_t(1) << 40)) return r;
                break;
            case 2:
                pos += (param + 7) / 8;
                break;
            default:
                return r;
            }
        }
    }

    // Fills `size` cells at `tpos` from the gap description at `pos`; `pos` is left at its end.
    bool gap_description(uint64_t& pos, uint64_t tpos, int64_t size, bool pre) {
        Reserved r = gap_reserved(pos);
        if (!r.ok || size < 0 || r.size > size) return false;
        const uint64_t dlimit = d_.size();
        uint8_t pattern[16] = {0};
        uint64_t pattern_size = 0, p = 0, block_size = 0;
        for (;;) {
            if (pos >= dlimit) return false;
            uint8_t val = d_[size_t(pos++)];
            unsigned n = val >> 5;
            if (n > dlimit - pos) return false;
            uint64_t param = 0;
            for (unsigned i = 0; i < n; ++i) param = param << 8 | d_[size_t(pos++)];
            switch (val & 0x1F) {
            case 0:
                return uint64_t(size) == p;
            case 1:
                if (block_size) return false;
                block_size = param * 2;
                pattern_size = 0;
                break;
            case 2: {
                pattern_size = param;
                if (pattern_size > 16 * 8) return false;
                uint64_t nb = (pattern_size + 7) / 8;
                if (nb > dlimit - pos) return false;
                std::memcpy(pattern, (d_.data() + pos), size_t(nb));
                pos += nb;
                if (pre) {
                    if (pos < dlimit && !(d_[size_t(pos)] & 0x1F)) {
                        if (p > uint64_t(size)) return false;
                        block_size = uint64_t(size) - p;
                    } else if (!block_size) {
                        block_size = pattern_size;
                    }
                    if (p + block_size > uint64_t(size)) return false;
                    if (!write_mfm(tpos + p, pattern, sizeof pattern, 0, pattern_size, block_size)) return false;
                    p += block_size;
                } else {
                    if (p == 0 && block_size && r.size != size) {
                        int64_t bs = size - (r.size - int64_t(block_size));
                        if (bs < 0) return false;
                        block_size = uint64_t(bs);
                    }
                    if (!block_size) block_size = uint64_t(size - r.size);
                    if (p + block_size > uint64_t(size)) return false;
                    if (!write_mfm(tpos + p, pattern, sizeof pattern, uint32_t(uint64_t(0) - block_size), pattern_size, block_size))
                        return false;
                    p += block_size;
                }
                block_size = 0;
                break;
            }
            default:
                return false;
            }
        }
    }

    // Gap type 0: a repeated one-byte pattern split around the splice (or index) position `spos`.
    bool gap0(uint64_t tpos, int64_t gap_cells, uint32_t pattern, int64_t ipos, int64_t& spos) {
        spos = (ipos >= 16 && ipos + 16 <= gap_cells) ? ipos : gap_cells >> 1;
        uint8_t pat = uint8_t(pattern & 0xFF);
        if (!write_mfm(tpos, &pat, 1, 0, 8, uint64_t(spos))) return false;
        uint64_t t = tpos + uint64_t(spos);
        int64_t delta = 0;
        if (gap_cells & 1) {
            if (t >= cells_.size()) return false;
            cells_[t++] = kCell0;
            delta = 1;
        }
        return write_mfm(t, &pat, 1, uint32_t(spos + delta - gap_cells), 8, uint64_t(gap_cells - spos - delta));
    }

    bool gap1(uint64_t tpos, int64_t gap_cells, int64_t ipos, uint64_t desc, int64_t& spos) {
        spos = (ipos >= 16 && ipos < gap_cells - 16) ? ipos : 0;
        return gap_description(desc, tpos, gap_cells, true);
    }

    bool gap2(uint64_t tpos, int64_t gap_cells, int64_t ipos, uint64_t desc, int64_t& spos) {
        spos = (ipos >= 16 && ipos < gap_cells - 16) ? ipos : gap_cells;
        return gap_description(desc, tpos, gap_cells, false);
    }

    bool gap3(uint64_t tpos, int64_t gap_cells, int64_t ipos, uint64_t desc, int64_t& spos) {
        spos = 0;
        Reserved a = gap_reserved(desc);
        if (!a.ok) return false;
        Reserved b = gap_reserved(a.pos);
        if (!b.ok) return false;
        int64_t presize = a.size, postsize = b.size;
        int64_t delta = gap_cells & 1;
        int64_t usable = gap_cells - delta;
        if (presize + postsize > usable) {
            int64_t rem = presize + postsize - usable;
            int64_t rem0 = rem >> 1;
            int64_t rem1 = rem - rem0;
            if (presize < rem0) {
                rem1 += rem0 - presize;
                rem0 = presize;
            }
            if (postsize < rem1) {
                rem0 += rem1 - postsize;
                rem1 = postsize;
            }
            presize -= rem0;
            postsize -= rem1;
        }
        if (ipos >= 16 && ipos < gap_cells - 16) {
            spos = ipos;
            if (spos < presize) spos = presize;
            if (spos > usable - postsize) spos = usable - postsize;
        } else {
            spos = presize + (usable - presize - postsize) / 2;
        }
        if (spos < 0 || spos > usable) return false;
        uint64_t pos = desc;
        if (!gap_description(pos, tpos, spos, true)) return false;
        if (delta) cells_[size_t(tpos + uint64_t(spos))] = kCell0;
        return gap_description(pos, tpos + uint64_t(spos) + uint64_t(delta), gap_cells - spos - delta, false);
    }

    // --- one block ------------------------------------------------------------------------------------------

    struct Block {
        uint64_t pos = 0, data_pos = 0, gap_pos = 0;
        int64_t splice = 0;
    };

    bool block(uint32_t idx, int64_t ipos, uint64_t pos, uint32_t encoder_type, Block& out) {
        const uint64_t th = uint64_t(32) * idx;
        uint64_t data_cells = u32(d_, th);
        uint64_t gap_cells = u32(d_, th + 4);
        if (gap_cells < 8) gap_cells = 0;
        uint32_t encoder = u32(d_, th + 16);
        if (encoder != 1 && encoder != 2) return false;
        bool raw = encoder == 2;
        uint32_t flags = encoder_type == 1 ? 0 : u32(d_, th + 20);
        uint32_t gap_type = flags & 3;
        bool dmb = (flags & 4) != 0;
        uint64_t dpos = pos;
        uint64_t gpos = dpos + data_cells;
        uint64_t end = gpos + gap_cells;
        if (end > cells_.size()) return false;
        if (!block_data(u32(d_, th + 28), dpos, gpos, dmb, raw)) return false;
        // the gap-description offset and the type-0 pattern share the descriptor
        int64_t gip = ipos > int64_t(gpos) ? ipos - int64_t(gpos) : 0;
        uint32_t gdesc = u32(d_, th + 8);
        uint32_t pattern = u32(d_, th + 24);
        int64_t spos = 0;
        bool ok = false;
        switch (gap_type) {
        case 0: ok = gap0(gpos, int64_t(gap_cells), pattern, gip, spos); break;
        case 1: ok = gap1(gpos, int64_t(gap_cells), gip, gdesc, spos); break;
        case 2: ok = gap2(gpos, int64_t(gap_cells), gip, gdesc, spos); break;
        default: ok = gap3(gpos, int64_t(gap_cells), gip, gdesc, spos); break;
        }
        if (!ok) return false;
        out.pos = end;
        out.data_pos = dpos;
        out.gap_pos = gpos;
        out.splice = spos + int64_t(gpos);
        return true;
    }

private:
    const std::vector<uint8_t>& d_;
};

uint64_t block_real_size(const RawTrack& t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t.block_count; ++i) {
        uint64_t dc = u32(t.data, uint64_t(32) * i);
        uint64_t gc = u32(t.data, uint64_t(32) * i + 4);
        if (gc < 8) gc = 0;
        size += dc + gc;
    }
    return size;
}

void timing_set(std::vector<uint16_t>& times, uint64_t start, uint64_t end, uint16_t value) {
    end = std::min<uint64_t>(end, times.size());
    for (uint64_t i = start; i < end; ++i) times[size_t(i)] = value;
}

bool generate_timings(const RawTrack& t, uint64_t size_cells, std::vector<uint16_t>& times,
                      const std::vector<uint64_t>& data_pos, const std::vector<uint64_t>& gap_pos) {
    std::fill(times.begin(), times.end(), uint16_t(2000));
    const uint32_t bc = t.block_count;
    switch (t.type) {
    case 2:
        break;
    case 3:
        if (bc >= 5) {
            timing_set(times, gap_pos[3], data_pos[4], 1890);
            timing_set(times, data_pos[4], gap_pos[4], 1890);
        }
        if (bc >= 6) {
            timing_set(times, gap_pos[4], data_pos[5], 1990);
            timing_set(times, data_pos[5], gap_pos[5], 1990);
        }
        if (bc >= 7) {
            timing_set(times, gap_pos[5], data_pos[6], 2090);
            timing_set(times, data_pos[6], gap_pos[6], 2090);
        }
        break;
    case 4:
        if (bc < 1) return false;
        timing_set(times, gap_pos[bc - 1], size_cells, 1890);
        timing_set(times, data_pos[0], gap_pos[0], 1890);
        if (bc >= 2) {
            timing_set(times, gap_pos[0], data_pos[1], 1990);
            timing_set(times, data_pos[1], gap_pos[1], 1990);
        }
        if (bc >= 3) {
            timing_set(times, gap_pos[1], data_pos[2], 2090);
            timing_set(times, data_pos[2], gap_pos[2], 2090);
        }
        break;
    case 5:
        if (bc >= 6) timing_set(times, data_pos[5], gap_pos[5], 2100);
        break;
    case 6:
        if (bc >= 2) timing_set(times, data_pos[1], gap_pos[1], 2200);
        if (bc >= 3) timing_set(times, data_pos[2], gap_pos[2], 1800);
        break;
    case 7:
        if (bc >= 2) timing_set(times, data_pos[1], gap_pos[1], 2100);
        break;
    case 8: {
        static const struct { uint32_t block; uint16_t ns; } kSegments[] = {{1, 2200}, {2, 2100}, {4, 1900}, {5, 1800}, {6, 1700}};
        for (const auto& s : kSegments)
            if (bc >= s.block + 1) timing_set(times, data_pos[s.block], gap_pos[s.block], s.ns);
        break;
    }
    case 9: {
        uint32_t mask = u32(t.data, 24);
        for (uint32_t i = 1; i < bc; ++i)
            timing_set(times, data_pos[i], gap_pos[i], (i - 1 < 32 && (mask >> (i - 1)) & 1) ? 1900 : 2100);
        break;
    }
    default:
        return false;
    }
    return true;
}

// Rebuilds the cells and timings of one track. `cells` stays empty for a track without cells.
bool generate_track(RawTrack& t, uint32_t encoder_type, std::vector<uint8_t>& cells, std::vector<uint16_t>& times) {
    cells.clear();
    times.clear();
    if (!t.size_cells) return true;
    if (t.size_cells > kMaxTrackCells) return false;
    if (t.type == 1) { // noise / unformatted
        cells.assign(size_t(t.size_cells), kCellWeak);
        times.assign(size_t(t.size_cells), uint16_t(2000));
        return true;
    }
    if (t.data.size() / 32 < t.block_count) return false;
    uint64_t real = block_real_size(t);
    if (real > kMaxTrackCells) return false;
    t.size_cells = real;
    if (t.index_cells >= t.size_cells) return false;
    Generator g(t.data, t.size_cells);
    std::vector<uint64_t> data_pos(size_t(t.block_count) + 1, 0), gap_pos(t.block_count, 0);
    std::vector<int64_t> splice_pos(t.block_count, 0);
    uint64_t pos = 0;
    for (uint32_t i = 0; i < t.block_count; ++i) {
        int64_t ipos = i == t.block_count - 1 ? int64_t(t.size_cells - t.index_cells) : int64_t(0xFFFFFFFFu);
        Generator::Block b;
        if (!g.block(i, ipos, pos, encoder_type, b)) return false;
        pos = b.pos;
        data_pos[i] = b.data_pos;
        gap_pos[i] = b.gap_pos;
        splice_pos[i] = b.splice;
    }
    if (pos != t.size_cells) return false;
    data_pos[t.block_count] = pos;
    // mark_track_splice: the write splice flips three cells
    int64_t sp = splice_pos[t.block_count - 1];
    for (int i = 0; i < 3; ++i) {
        uint64_t k = uint64_t(sp + i) % t.size_cells;
        if (g.cells_[size_t(k)] == kCell0) g.cells_[size_t(k)] = kCell1;
        else if (g.cells_[size_t(k)] == kCell1) g.cells_[size_t(k)] = kCell0;
    }
    times.assign(size_t(t.size_cells), uint16_t(2000));
    if (!generate_timings(t, t.size_cells, times, data_pos, gap_pos)) return false;
    if (t.index_cells) {
        size_t off = size_t(t.size_cells - t.index_cells);
        std::rotate(g.cells_.begin(), g.cells_.begin() + long(off), g.cells_.end());
        std::rotate(times.begin(), times.begin() + long(off), times.end());
    }
    cells = std::move(g.cells_);
    return true;
}

} // namespace

bool read_ipf(const std::vector<uint8_t>& data, DiskImage& out, IpfDisk& disk, std::string& why) {
    out = DiskImage();
    disk = IpfDisk();
    if (data.size() > kMaxFileBytes) { why = "file larger than 64 MB"; return false; }
    std::map<uint32_t, RawTrack> infos;
    IpfInfo info;
    bool have_info = false;
    size_t pos = 0;
    while (pos != data.size()) {
        if (data.size() - pos < 12) { why = "truncated record header"; return false; }
        const uint8_t* rec = &data[pos];
        uint32_t tsize = be32(rec + 4);
        if (tsize < 12 || data.size() - pos < tsize) { why = "bad record size for " + std::string(reinterpret_cast<const char*>(rec), 4); return false; }
        if (ipf_crc32(rec, tsize, true) != be32(rec + 8)) {
            why = "CRC error in the " + std::string(reinterpret_cast<const char*>(rec), 4) + " record at offset " + std::to_string(pos);
            return false;
        }
        const uint8_t* tag = rec;
        pos += tsize;
        auto is = [&](const char* s) { return std::memcmp(tag, s, 4) == 0; };
        if (is("CAPS")) {
            if (tsize != 12) { why = "bad CAPS record"; return false; }
        } else if (is("INFO")) {
            if (tsize != 96) { why = "bad INFO record"; return false; }
            info.type = be32(rec + 12);
            info.encoder_type = be32(rec + 16);
            info.encoder_revision = be32(rec + 20);
            info.release = be32(rec + 24);
            info.revision = be32(rec + 28);
            info.origin = be32(rec + 32);
            info.min_cylinder = be32(rec + 36);
            info.max_cylinder = be32(rec + 40);
            info.min_head = be32(rec + 44);
            info.max_head = be32(rec + 48);
            info.credit_day = be32(rec + 52);
            info.credit_time = be32(rec + 56);
            for (int i = 0; i < 4; ++i) info.platform[i] = be32(rec + 60 + 4 * i);
            for (int i = 0; i < 5; ++i) info.extra[i] = be32(rec + 76 + 4 * i);
            if (info.type != 1 || (info.encoder_type != 1 && info.encoder_type != 2) || info.encoder_revision != 1) {
                why = "unsupported INFO record (media type or encoder)";
                return false;
            }
            have_info = true;
        } else if (is("IMGE")) {
            if (tsize != 80) { why = "bad IMGE record"; return false; }
            if (!have_info) { why = "IMGE record before the INFO record"; return false; }
            uint32_t idx = be32(rec + 64);
            if (idx > kMaxDataIndex) { why = "track index out of range"; return false; }
            RawTrack& t = infos[idx];
            t.info_set = true;
            t.cylinder = be32(rec + 12);
            t.head = be32(rec + 16);
            if (t.cylinder < info.min_cylinder || t.cylinder > info.max_cylinder || t.head < info.min_head ||
                t.head > info.max_head) {
                why = "track outside the image geometry";
                return false;
            }
            if (t.head > 1 || t.cylinder > 83) { why = "track outside 84 cylinders x 2 sides"; return false; }
            t.type = be32(rec + 20);
            if (be32(rec + 24) != 1 || be32(rec + 56) != 0) { why = "unsupported signal type or encoder process"; return false; }
            t.size_bytes = be32(rec + 28);
            t.index_bytes = be32(rec + 32);
            t.index_cells = be32(rec + 36);
            t.datasize_cells = be32(rec + 40);
            t.gapsize_cells = be32(rec + 44);
            t.size_cells = be32(rec + 48);
            t.block_count = be32(rec + 52);
            t.weak_bits = be32(rec + 60);
        } else if (is("DATA")) {
            if (tsize != 28) { why = "bad DATA record"; return false; }
            uint32_t idx = be32(rec + 24);
            if (idx > kMaxDataIndex) { why = "track index out of range"; return false; }
            RawTrack& t = infos[idx];
            uint32_t n = be32(rec + 12);
            if (n > data.size() - pos) { why = "DATA payload truncated"; return false; }
            if (ipf_crc32(data.data() + pos, n) != be32(rec + 20)) {
                why = "DATA payload CRC error (track index " + std::to_string(idx) + ")";
                return false;
            }
            t.data.assign(data.begin() + long(pos), data.begin() + long(pos + n));
            t.has_data = true;
            pos += n;
        }
        // other records (TRCK, CTEX, CTEI, ...) carry nothing needed for decoding
    }
    if (!have_info) { why = "no INFO record"; return false; }
    disk.info = info;

    std::vector<uint8_t> cells;
    std::vector<uint16_t> times;
    for (auto& [idx, t] : infos) {
        if (t.info_set != t.has_data) { why = "IMGE/DATA mismatch for track index " + std::to_string(idx); return false; }
        if (!t.info_set) continue;
        if (!generate_track(t, info.encoder_type, cells, times)) {
            why = "could not rebuild cylinder " + std::to_string(t.cylinder) + " head " + std::to_string(t.head);
            return false;
        }
        IpfTrackInfo ti;
        ti.cylinder = t.cylinder;
        ti.head = t.head;
        ti.type = t.type;
        ti.cells = uint32_t(cells.size());
        ti.index_cells = t.index_cells;
        ti.blocks = t.block_count;
        for (uint8_t c : cells) ti.weak_cells += c == kCellWeak;
        std::vector<uint16_t> uniq(times);
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        ti.timing_ns = std::move(uniq);
        if (!cells.empty()) {
            std::vector<uint8_t> bits((cells.size() + 7) / 8, 0);
            for (size_t i = 0; i < cells.size(); ++i)
                if (cells[i] == kCell1) bits[i >> 3] = uint8_t(bits[i >> 3] | (0x80 >> (i & 7)));
            Track& tr = out.track(ti.number());
            Track decoded;
            int ok = cells.size() >= 4000 ? decode_amiga_mfm_track(bits.data(), uint32_t(cells.size()), ti.number(), decoded) : 0;
            if (ok == kSectorsPerTrack && decoded.all_sectors_ok()) {
                tr = std::move(decoded);
                ti.standard = true;
            } else {
                tr = Track();
                tr.kind = Track::Kind::Raw;
                tr.mfm = std::move(bits);
                tr.bit_length = uint32_t(cells.size());
            }
        }
        disk.tracks.push_back(std::move(ti));
    }
    std::sort(disk.tracks.begin(), disk.tracks.end(), [](const IpfTrackInfo& a, const IpfTrackInfo& b) {
        return a.number() < b.number();
    });
    return true;
}

} // namespace x2::import
