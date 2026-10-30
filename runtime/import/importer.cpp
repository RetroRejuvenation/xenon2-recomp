// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/importer.h"

#include "import/disk_image.h"
#include "import/dms.h"
#include "import/ext_adf.h"
#include "import/ipf.h"
#include "import/flux.h"
#include "import/lha.h"
#include "import/whdload.h"
#include "import/sha1.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

extern "C" {
#include "puff.h"
}

namespace x2::import {

namespace {

namespace fs = std::filesystem;

constexpr size_t kAdfSize = 1760 * 512;
constexpr size_t kHdAdfSize = 2 * kAdfSize;
constexpr size_t kGameImageSize = 0x3FC00;
constexpr size_t kMaxUnpacked = 96u << 20; // largest archive member / gunzipped file accepted (a 5-revolution SCP is ~30 MB)
constexpr uintmax_t kMaxFile = 128u << 20; // largest file read from disk
constexpr int kMaxDepth = 3;               // archive nesting (zip in zip in zip)
constexpr size_t kMaxFolderFiles = 1024;   // files read from the given folders (a KryoFlux dump is ~168 files per disk)
constexpr uintmax_t kMaxTotal = 512u << 20; // bytes read from all given paths
constexpr uint32_t kRootBlock = 880;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// Disk images catalogued in docs/re-notes/sources.md (identification only; no game data is stored here).
struct Known {
    const char* sha1;
    const char* what;
    int quality; // higher is preferred when several candidates are given
};
const Known kDisc1[] = {
    {"45e66f9cd7cb68f1b759c9ff8dd0f4427ca942a6", "BS1 [t+3] trained release, disk 1", 3},
    {"268e8b529ae4e357d1d624c7fe4d565a2689f225", "BS1 [t+3] trained release, disk 1 [a] (alternate boot block)", 2},
    {"d6e1a7ba5cdd196f6aecffd4044b4c23b694092a", "BS1 [t+3] Deejay99 UAE-fix hack, disk 1", 1},
};
const Known kDisc2[] = {
    {"a748afc7a142335a40cc46fb7785dea820770241", "BS1 [t+3] trained release, disk 2", 3},
    {"13eb35605c6af1968a64096434d3ca93b91bb324", "BS1 Deejay99 hack, disk 2 (26 bytes differ from the BS1 disk 2)", 1},
};
const Known kOtherReleases[] = {
    {"2028f31af2fb760da405618a42f9e69e15882825", "PNA single-disk crack", 0},
    {"12b7bd53288d3764af062a71923c9ec8189e61f2", "PNA [a] single-disk crack", 0},
    {"2b98cbbac1a89901fcfe634ef805b66612d3b2de", "Quartex (QTX) single-disk crack", 0},
    {"0df9822509837bce3999806e3ae2e218cea4ddd9", "Black Monks single-disk crack", 0},
    {"8f50b895f344400787c34929bb29ef3e123b0a4e", "Vision Factory (VF) one-disk crunch", 0},
    {"83e19d34e7202cde05e8151067d3669e22447182", "Vision Factory (VF) [a] one-disk crunch", 0},
};
// IPF images (SPS preservation dumps) recognised by the sha1 of the file, only to name them in the message for a
// disk whose protected tracks cannot be imported yet.
const Known kIpfProtected[] = {
    {"72cf591760e014b53e946bf9603ae71b3d21d0d8", "retail disk 1, first pressing (SPS 0297)", 0},
    {"11e95a503a80f32a7b3f0fda9ae532ae61552d86", "retail disk 1, second pressing (SPS 2874 [a])", 0},
    {"0be7c3253e56173072abd80ebb41f5d5f2e52e7a", "Bombuzal + Xenon 2 compilation, disk 1 (SPS 1762)", 0},
    {"c06f8a4206b839663457192c7564f9306f8980e1", "The Power Pack compilation (SPS 2234)", 0},
};
const char* const kKnownExeSha1 = "778cf75e75d7e1b2036f4d68f8a557b69e99a817"; // XENON-II, same on all BS1 disks

template <size_t N>
const Known* lookup(const Known (&table)[N], const std::string& sha1) {
    for (const Known& k : table)
        if (sha1 == k.sha1) return &k;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Archives

} // namespace

bool inflate_raw(const uint8_t* src, size_t src_len, size_t out_len, std::vector<uint8_t>& out) {
    // puff counts in unsigned long, which is 32-bit on Windows. Every caller is far below that (kMaxFile,
    // kMaxUnpacked, a PNG in an asset pack), so refuse instead of truncating silently if that ever changes.
    if (src_len > 0xFFFFFFFFu || out_len > 0xFFFFFFFFu) return false;
    out.assign(out_len, 0);
    unsigned long dl = static_cast<unsigned long>(out_len), sl = static_cast<unsigned long>(src_len);
    // puff keeps the output pointer non-null even for an empty member.
    uint8_t dummy = 0;
    int rc = puff(out_len ? out.data() : &dummy, &dl, src, &sl);
    return rc == 0 && dl == out_len;
}

namespace {

bool gunzip(const std::vector<uint8_t>& d, std::vector<uint8_t>& out, std::string& why) {
    if (d.size() < 18 || d[2] != 8) { why = "not a deflate gzip stream"; return false; }
    uint8_t flg = d[3];
    size_t p = 10;
    if (flg & 4) { if (p + 2 > d.size()) { why = "truncated header"; return false; } p += 2 + le16(&d[p]); }
    for (int bit : {8, 16})
        if (flg & bit) {
            while (p < d.size() && d[p]) ++p;
            ++p;
        }
    if (flg & 2) p += 2;
    if (p + 8 > d.size()) { why = "truncated header"; return false; }
    uint32_t crc = le32(&d[d.size() - 8]), isize = le32(&d[d.size() - 4]);
    if (isize > kMaxUnpacked) { why = "uncompressed size " + std::to_string(isize) + " is too large"; return false; }
    if (!inflate_raw(&d[p], d.size() - p - 8, isize, out)) { why = "corrupted or truncated deflate data"; return false; }
    if (crc32(out.data(), out.size()) != crc) { why = "CRC mismatch"; return false; }
    return true;
}

} // namespace

bool unzip(const std::vector<uint8_t>& d, std::vector<ZipEntry>& entries, std::string& why, size_t max_total) {
    if (d.size() < 22) { why = "truncated zip"; return false; }
    size_t eocd = std::string::npos;
    size_t lo = d.size() > 22 + 65535 ? d.size() - 22 - 65535 : 0;
    for (size_t p = d.size() - 22 + 1; p-- > lo;)
        if (le32(&d[p]) == 0x06054b50) { eocd = p; break; }
    if (eocd == std::string::npos) { why = "no zip central directory (truncated or corrupted)"; return false; }
    uint32_t count = le16(&d[eocd + 10]), cd_off = le32(&d[eocd + 16]);
    if (cd_off == 0xFFFFFFFF || count == 0xFFFF) { why = "zip64 archives are not supported"; return false; }
    size_t p = cd_off, total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > d.size() || le32(&d[p]) != 0x02014b50) { why = "corrupted zip central directory"; return false; }
        uint16_t flags = le16(&d[p + 8]), method = le16(&d[p + 10]);
        uint32_t crc = le32(&d[p + 16]), csize = le32(&d[p + 20]), usize = le32(&d[p + 24]);
        uint16_t nlen = le16(&d[p + 28]), xlen = le16(&d[p + 30]), clen = le16(&d[p + 32]);
        uint32_t lho = le32(&d[p + 42]);
        if (p + 46 + nlen > d.size()) { why = "corrupted zip central directory"; return false; }
        ZipEntry e;
        e.name.assign(reinterpret_cast<const char*>(&d[p + 46]), nlen);
        p += 46 + size_t(nlen) + xlen + clen;
        if (e.name.empty() || e.name.back() == '/' || e.name.rfind("__MACOSX/", 0) == 0) continue;
        if (flags & 1) { e.problem = "encrypted zip entries are not supported"; e.unsupported = true; }
        else if (method != 0 && method != 8) {
            e.problem = "zip compression method " + std::to_string(method) + " is not supported (only stored/deflate)";
            e.unsupported = true;
        } else if (usize > kMaxUnpacked) e.problem = "too large (" + std::to_string(usize) + " bytes)";
        else if ((total += usize) > max_total) { why = "archive too large when unpacked"; return false; }
        else if (size_t(lho) + 30 > d.size() || le32(&d[lho]) != 0x04034b50) e.problem = "corrupted local header";
        else {
            size_t data = size_t(lho) + 30 + le16(&d[lho + 26]) + le16(&d[lho + 28]);
            if (data > d.size() || csize > d.size() - data) e.problem = "truncated data";
            else if (method == 0) {
                if (csize != usize) e.problem = "corrupted (stored size mismatch)";
                else e.data.assign(d.begin() + long(data), d.begin() + long(data + csize));
            } else if (!inflate_raw(&d[data], csize, usize, e.data)) e.problem = "corrupted deflate data";
            if (e.problem.empty() && crc32(e.data.data(), e.data.size()) != crc) e.problem = "CRC mismatch (corrupted)";
            if (!e.problem.empty()) e.data.clear();
        }
        entries.push_back(std::move(e));
    }
    return true;
}

namespace {

// ---------------------------------------------------------------------------------------------------------
// AmigaDOS (OFS/FFS) reader: enough to list files and read their contents.

class Volume {
public:
    explicit Volume(const DiskImage& img) : img_(img), ffs_(img.sector(0) && (img.sector(0)[3] & 1)) {}

    bool valid() const {
        const uint8_t* r = block(kRootBlock);
        return r && be32(r) == 2 && be32(r + 508) == 1 && be32(r + 12) == 72 && checksum_ok(r);
    }
    std::string volume_name() const { return name_of(block(kRootBlock)); }

    // Calls f(name, header_block) for every file in the root directory and subdirectories (3 levels).
    template <class F>
    void for_each_file(F&& f) const { list(kRootBlock, 0, f); }

    bool read_file(uint32_t hdr, std::vector<uint8_t>& out, std::string& why) const {
        const uint8_t* h = block(hdr);
        uint32_t size = be32(h + 324);
        if (size > kAdfSize) { why = "file size field is corrupted"; return false; }
        out.clear();
        out.reserve(size);
        const uint8_t* t = h;
        for (int guard = 0; t && out.size() < size; ++guard) {
            uint32_t n = be32(t + 8);
            if (n > 72 || guard > 1760) { why = "corrupted block table"; return false; }
            for (uint32_t i = 0; i < n && out.size() < size; ++i) {
                const uint8_t* b = block(be32(t + 308 - 4 * i));
                if (!b) { why = "data block pointer out of range"; return false; }
                if (ffs_) {
                    out.insert(out.end(), b, b + std::min<size_t>(512, size - out.size()));
                } else {
                    uint32_t len = be32(b + 12);
                    if (be32(b) != 8 || be32(b + 4) != hdr || len > 488 || !checksum_ok(b)) {
                        why = "corrupted data block";
                        return false;
                    }
                    out.insert(out.end(), b + 24, b + 24 + std::min<size_t>(len, size - out.size()));
                }
            }
            if (out.size() >= size) break;
            uint32_t ext = be32(t + 504);
            t = ext ? block(ext) : nullptr;
            if (t && (be32(t) != 16 || !checksum_ok(t))) { why = "corrupted extension block"; return false; }
        }
        if (out.size() != size) { why = "file is shorter than its size field"; return false; }
        return true;
    }

private:
    const uint8_t* block(uint32_t n) const { return n >= 2 && n < 1760 ? img_.sector(int(n)) : nullptr; }
    static bool checksum_ok(const uint8_t* b) {
        uint32_t sum = 0;
        for (int i = 0; i < 128; ++i) sum += be32(b + 4 * i);
        return sum == 0;
    }
    static std::string name_of(const uint8_t* b) {
        size_t len = std::min<size_t>(b[432], 30);
        return std::string(reinterpret_cast<const char*>(b + 433), len);
    }
    template <class F>
    void list(uint32_t dir, int depth, F& f) const {
        const uint8_t* db = block(dir);
        int guard = 0;
        for (int i = 0; i < 72; ++i) {
            for (uint32_t k = be32(db + 24 + 4 * i); k && ++guard < 2000;) {
                const uint8_t* b = block(k);
                if (!b || be32(b) != 2 || !checksum_ok(b)) break;
                uint32_t sec = be32(b + 508);
                if (sec == 0xFFFFFFFD) f(name_of(b), k);
                else if (sec == 2 && depth < 3) list(k, depth + 1, f);
                k = be32(b + 496);
            }
        }
    }

    const DiskImage& img_;
    bool ffs_;
};

// ---------------------------------------------------------------------------------------------------------
// XENON-II: one CODE hunk = decrunch stub + backwards ByteKiller-style stream (tools/unpack_xenon2.py).

bool is_xenon_exe(const std::vector<uint8_t>& exe, size_t& code, size_t& code_len) {
    if (exe.size() < 64 || be32(&exe[0]) != 0x3F3) return false;
    size_t p = 4;
    while (true) { // resident library names (normally none)
        if (p + 4 > exe.size()) return false;
        uint32_t n = be32(&exe[p]);
        p += 4;
        if (!n) break;
        if (n > 256) return false;
        p += 4 * size_t(n);
    }
    if (p + 12 > exe.size()) return false;
    uint32_t first = be32(&exe[p + 4]), last = be32(&exe[p + 8]);
    if (last < first || last - first > 64) return false;
    p += 12 + 4 * size_t(last - first + 1);
    if (p + 8 > exe.size() || (be32(&exe[p]) & 0x3FFFFFFF) != 0x3E9) return false;
    code_len = size_t(be32(&exe[p + 4]) & 0x3FFFFFFF) * 4;
    code = p + 8;
    if (code_len > exe.size() - code || code_len < 16) return false;
    // lea d16(pc),a0 ; adda.l #len,a0 : a0 = end of the packed stream.
    return be16(&exe[code]) == 0x41FA && be16(&exe[code + 4]) == 0xD1FC;
}

bool depack_xenon(const std::vector<uint8_t>& exe, std::vector<uint8_t>& game, std::string& why) {
    size_t code, code_len;
    if (!is_xenon_exe(exe, code, code_len)) { why = "not the XENON-II executable"; return false; }
    const uint8_t* c = &exe[code];
    size_t start = 2 + be16(c + 2), end = start + be32(c + 6);
    if (end > code_len || end < start + 8) { why = "packed stream exceeds the code hunk"; return false; }
    size_t pos = end;
    bool underrun = false;
    auto rl = [&]() -> uint32_t {
        if (pos < start + 4) { underrun = true; return 1; }
        pos -= 4;
        return be32(c + pos);
    };
    size_t size = rl();
    if (size < kGameImageSize || size > 0x80000) { why = "unpacked size " + std::to_string(size) + " is out of range"; return false; }
    std::vector<uint8_t> out(size);
    size_t a2 = size;
    uint32_t d0 = rl();
    auto bit = [&]() -> uint32_t {
        uint32_t cy = d0 & 1;
        d0 >>= 1;
        if (d0 == 0) {
            d0 = rl();
            cy = d0 & 1;
            d0 = (d0 >> 1) | 0x80000000u;
        }
        return cy;
    };
    auto bits = [&](int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    };
    while (!underrun) {
        uint32_t n = bits(3);
        if (n) {
            if (n == 7) {
                if (bit() == 0) n = bits(4) + 7;
                else {
                    n = bits(10);
                    if (n == 0) n = bits(0x12);
                }
            }
            if (n > a2) { why = "literal run past the start of the output"; return false; }
            for (uint32_t i = 0; i < n; ++i) out[--a2] = uint8_t(bits(8));
        }
        if (a2 == 0) break;
        uint32_t k = bits(2), ln, nb;
        if (k == 0) { ln = 2; nb = 8; }
        else if (k == 1) { ln = 3; nb = bit() ? 8 : 14; }
        else {
            if (k == 2) ln = 4;
            else {
                uint32_t j = bits(2);
                ln = j == 3 ? bits(8) : j == 2 ? bits(2) + 7 : j + 5;
            }
            nb = bit() == 0 ? 16 : bit() ? 8 : 12;
        }
        uint32_t off = bits(int(nb));
        if (ln > a2 || a2 - 1 + off >= size) { why = "match outside the output"; return false; }
        for (uint32_t i = 0; i < ln; ++i, --a2) out[a2 - 1] = out[a2 - 1 + off];
    }
    if (underrun) { why = "packed stream is truncated or corrupted"; return false; }
    if (std::memcmp(out.data(), "\x60\x00\x6C\x30", 4) != 0) { why = "depacked image does not start with bra.w $7032"; return false; }
    game.assign(out.begin(), out.begin() + long(kGameImageSize));
    return true;
}

// ---------------------------------------------------------------------------------------------------------

struct Candidate {
    std::string name, sha1, what, exe_sha1;
    int quality = 0;
    std::vector<uint8_t> data, game;
};

class Scanner {
public:
    std::vector<Candidate> disc1, disc2;
    struct RetailNote {
        std::string name;
        const RetailDisk1* disk;
    };
    std::vector<RetailNote> retail1; // retail (not BS1) disk 1 images found; see import_inputs
    std::vector<Message> msgs;
    bool unsupported = false, bad = false;

    void info(const std::string& s) { msgs.push_back({Message::Info, s}); }
    void warn(const std::string& s) { msgs.push_back({Message::Warn, s}); }
    void error(const std::string& s, bool is_unsupported) {
        msgs.push_back({Message::Error, s});
        (is_unsupported ? unsupported : bad) = true;
    }

    // `nested`: inside an archive or folder, where unrelated files (readme, nfo) are expected and only noted.
    void scan(const std::string& name, const std::vector<uint8_t>& d, int depth, bool nested) {
        auto magic = [&](const char* m, size_t at = 0) {
            size_t n = std::strlen(m);
            return d.size() >= at + n && std::memcmp(d.data() + at, m, n) == 0;
        };
        if (d.size() >= 2 && d[0] == 0x1F && d[1] == 0x8B) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            std::vector<uint8_t> out;
            std::string why;
            if (!gunzip(d, out, why)) { error(name + ": corrupted gzip/ADZ file (" + why + ")", false); return; }
            scan(name + " (gunzipped)", out, depth + 1, nested);
        } else if (magic("PK\x03\x04") || magic("PK\x05\x06")) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            std::vector<ZipEntry> entries;
            std::string why;
            if (!unzip(d, entries, why)) { error(name + ": " + why, why.find("not supported") != std::string::npos); return; }
            if (entries.empty()) { error(name + ": empty zip archive", false); return; }
            for (ZipEntry& e : entries) {
                std::string en = name + " > " + e.name;
                if (!e.problem.empty()) error(en + ": " + e.problem, e.unsupported);
                else scan(en, e.data, depth + 1, true);
            }
        } else if (magic("DMS!")) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            DiskImage img;
            DmsResult dr = read_dms(d, img);
            if (dr.status != DmsStatus::Ok) {
                error(name + ": DMS archive: " + dr.why, dr.status == DmsStatus::Unsupported);
                return;
            }
            std::vector<uint8_t> adf;
            std::string why;
            if (!img.to_adf(adf, why, dr.info.no_zero)) {
                error(name + ": DMS archive does not hold a complete 880 KB disk (" + why + ")", false);
                return;
            }
            scan(name + " (DMS unpacked)", adf, depth + 1, nested);
        } else if (magic("CAPS")) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            DiskImage img;
            IpfDisk disk;
            std::string why;
            if (!read_ipf(d, img, disk, why)) { error(name + ": IPF image: " + why, false); return; }
            std::vector<uint8_t> adf;
            if (!img.to_adf(adf, why)) {
                std::string sha1 = sha1_hex(d.data(), d.size());
                if (const Known* k = lookup(kIpfProtected, sha1))
                    error(name + ": IPF of the " + std::string(k->what) + ": protected retail disk: recognised, retail import "
                          "not available yet (" + why + ")",
                          true);
                else
                    error(name + ": IPF image with copy-protected or non-standard tracks: recognised, import of protected "
                          "disks not available yet (" + why + ")",
                          true);
                return;
            }
            scan(name + " (IPF decoded)", adf, depth + 1, nested);
        } else if (magic("UAE--ADF") || magic("UAE-1ADF")) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            DiskImage img;
            std::string why;
            bool hd = false;
            if (!read_ext_adf(d, img, why, hd)) { error(name + ": extended ADF: " + why, false); return; }
            if (hd) { error(name + ": extended ADF of a 1.76 MB HD disk: Xenon 2 discs are 880 KB DD ADFs", true); return; }
            std::vector<uint8_t> adf;
            if (!img.to_adf(adf, why)) {
                bool raw = false;
                for (int t : img.nonstandard_tracks()) raw = raw || img.track(t).kind == Track::Kind::Raw;
                error(name + ": extended ADF that is not a plain disk image (" + why + ")" +
                          (raw ? "; copy-protected raw tracks are only kept by flux or IPF images, and protected disks cannot be imported yet" : ""),
                      raw);
                return;
            }
            scan(name + " (extended ADF)", adf, depth + 1, nested);
        } else if (Container c = detect_container(d.data(), d.size()); c == Container::Scp || c == Container::Hfe) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            scan_flux_image(name, d, c, depth, nested);
        } else if (c == Container::KryoFlux) {
            collect_kryoflux(name, d);
        } else if (c == Container::Lha) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply", false); return; }
            LhaArchive arc;
            std::string why;
            if (!read_lha(d, arc, why, kMaxUnpacked)) { error(name + ": " + why, false); return; }
            if (!arc.note.empty()) warn(name + ": " + arc.note);
            for (LhaEntry& e : arc.entries) {
                if (e.is_dir) continue;
                std::string en = name + " > " + e.name;
                if (!e.problem.empty()) error(en + ": " + e.problem, e.unsupported);
                else scan(en, e.data, depth + 1, true);
            }
        } else if (const RetailDisk1* retail = identify_retail_disk1(d.data(), d.size())) {
            retail1.push_back({name, retail});
            info(name + ": Xenon 2 disk 1 of the " + retail->what + " (floppy version " + retail->version + ", sha1 " +
                 sha1_hex(d.data(), d.size()) + ")");
        } else if (WhdSlave slave; parse_whdload_slave(d.data(), d.size(), slave)) {
            note_whdload_slave(name, slave);
        } else if (d.size() == kAdfSize) {
            classify_adf(name, d);
        } else if (d.size() == kHdAdfSize) {
            error(name + ": 1.76 MB HD disk image: Xenon 2 discs are 880 KB DD ADFs", true);
        } else if (magic("DOS") || magic("X2B ", 2)) {
            error(name + ": " + std::string(d.size() < kAdfSize ? "truncated" : "oversized") + " disk image (" +
                      std::to_string(d.size()) + " bytes, an ADF is exactly 901120)",
                  false);
        } else if (nested) {
            info(name + ": ignored (not a disk image)");
        } else {
            error(name + ": not a disk image or archive (" + std::to_string(d.size()) +
                      " bytes; expected an ADF, ADZ or ZIP)",
                  false);
        }
    }

    // The KryoFlux stream files collected by scan() form disks (one per directory); call once after every input has
    // been scanned.
    void finish_kryoflux() {
        for (auto& kv : kf_sets_) {
            KfSet& set = kv.second;
            std::string label = (kv.first.empty() ? std::string("KryoFlux stream files") : kv.first) + " (KryoFlux streams)";
            FluxDisk fd;
            fd.source = "KryoFlux stream";
            int have = 0;
            for (int t = 0; t < kMaxTracks; ++t) {
                if (set.tracks[size_t(t)].revs.empty()) continue;
                fd.tracks[size_t(t)] = std::move(set.tracks[size_t(t)]);
                ++have;
            }
            for (const std::string& w : set.warnings) warn(label + ": " + w);
            if (have == 0) { error(label + ": no readable track files", false); continue; }
            DiskImage img;
            FluxReport rep;
            decode_flux_disk(fd, img, rep);
            finish_flux_disk(label, img, rep, "KryoFlux stream set", 0, true);
        }
        kf_sets_.clear();
    }

private:
    struct KfSet {
        KfSet() : tracks(kMaxTracks) {}
        std::vector<FluxTrack> tracks;
        std::vector<std::string> warnings;
    };
    std::map<std::string, KfSet> kf_sets_;

    void note_whdload_slave(const std::string& name, const WhdSlave& slave) {
        // ws_info holds several lines (credits, "Version 1.9 Floppy (11.05.2005)"); show the version line.
        std::string version;
        for (size_t pos = 0; pos < slave.info.size();) {
            size_t end = slave.info.find('\n', pos);
            if (end == std::string::npos) end = slave.info.size();
            if (slave.info.compare(pos, 7, "Version") == 0) { version = slave.info.substr(pos, end - pos); break; }
            pos = end + 1;
        }
        std::string what = "WHDLoad slave \"" + slave.name + "\"" + (version.empty() ? "" : " (" + version + ")");
        auto has = [&](const char* word) { return slave.name.find(word) != std::string::npos || slave.info.find(word) != std::string::npos; };
        if (has("Demo"))
            error(name + ": " + what + ": the Xenon 2 demo is a different, smaller program than the game: recognised, not imported", true);
        else if (has("CDTV"))
            error(name + ": " + what + ": the CDTV release has a different game executable than the disk release: recognised, not imported", true);
        else
            info(name + ": " + what + ": a WHDLoad install of the floppy release, its Disk.1 and Disk.2 images are read like any other disk images");
    }

    void collect_kryoflux(const std::string& name, const std::vector<uint8_t>& d) {
        int cyl = 0, side = 0;
        if (!kryoflux_track_from_name(name, cyl, side)) {
            error(name + ": KryoFlux stream file whose name is not of the form trackNN.S.raw, so its track number is unknown", false);
            return;
        }
        std::string dir;
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) dir = name.substr(0, slash);
        KfSet& set = kf_sets_[dir];
        FluxTrack ft;
        std::string why;
        std::vector<std::string> warns;
        if (!read_kryoflux_stream(d, ft, why, &warns)) { error(name + ": " + why, false); return; }
        for (const std::string& w : warns) set.warnings.push_back("track " + std::to_string(cyl * 2 + side) + ": " + w);
        FluxTrack& slot = set.tracks[size_t(cyl * 2 + side)];
        if (!slot.revs.empty()) {
            warn(name + ": a second stream file for track " + std::to_string(cyl * 2 + side) + " in the same folder; the later one replaces the earlier");
            slot = FluxTrack();
        }
        slot = std::move(ft);
    }

    // SCP and HFE: one file is one disk.
    void scan_flux_image(const std::string& name, const std::vector<uint8_t>& d, Container c, int depth, bool nested) {
        DiskImage img;
        FluxReport rep;
        std::string why;
        std::vector<std::string> warns;
        const char* label = container_name(c);
        if (c == Container::Scp) {
            FluxDisk fd;
            if (!read_scp(d, fd, why, &warns)) { error(name + ": " + label + " image: " + why, false); return; }
            decode_flux_disk(fd, img, rep);
        } else {
            bool hd = false;
            if (!read_hfe(d, img, why, hd, &warns)) { error(name + ": " + label + " image: " + why, hd); return; }
        }
        for (const std::string& w : warns) warn(name + ": " + w);
        finish_flux_disk(name, img, rep, label, depth, nested);
    }

    void finish_flux_disk(const std::string& name, DiskImage& img, const FluxReport& rep, const char* label, int depth, bool nested) {
        std::vector<uint8_t> adf;
        std::string why;
        if (!rep.tracks.empty()) info(name + ": " + label + " flux image: " + rep.summary());
        if (!img.to_adf(adf, why)) {
            bool raw = false;
            for (int t : img.nonstandard_tracks()) raw = raw || img.track(t).kind == Track::Kind::Raw;
            std::string detail = why;
            std::string pr = rep.problems();
            if (!pr.empty()) detail += (detail.empty() ? "" : "; ") + pr;
            error(name + ": " + label + " that is not a plain disk image (" + detail + ")" +
                      (raw ? "; copy-protected tracks need the retail-disk import, which is not supported yet" : ""),
                  raw);
            return;
        }
        scan(name + " (" + label + " decoded)", adf, depth + 1, nested);
    }

    void classify_adf(const std::string& name, const std::vector<uint8_t>& d) {
        std::string sha1 = sha1_hex(d.data(), d.size());
        if (const Known* k = lookup(kOtherReleases, sha1)) {
            error(name + ": recognised " + std::string(k->what) +
                      ": a different build of the game that this runtime does not support "
                      "(only the two-disc BS1 [t+3] release is supported)",
                  true);
            return;
        }
        if (std::memcmp(d.data() + 2, "X2B ", 4) == 0) {
            Candidate c{name, sha1, "", "", 0, d, {}};
            if (const Known* k = lookup(kDisc2, sha1)) { c.what = k->what; c.quality = k->quality; }
            else c.what = "unrecognised Xenon 2 disc 2 (\"X2B \" id)";
            info(name + ": " + c.what + " (sha1 " + sha1 + ")");
            disc2.push_back(std::move(c));
            return;
        }
        if (std::memcmp(d.data(), "DOS", 3) != 0) {
            error(name + ": 880 KB image that is neither an AmigaDOS disc nor the Xenon 2 game disc (no \"DOS\" boot "
                         "block, no \"X2B \" id)",
                  false);
            return;
        }
        DiskImage img;
        DiskImage::from_adf(d.data(), d.size(), img);
        Volume vol(img);
        if (!vol.valid()) {
            error(name + ": AmigaDOS boot block but no readable file system (custom trackloader disc?): not Xenon 2 "
                         "BS1 disc 1. Single-disk cracks are not supported.",
                  false);
            return;
        }
        std::string exe_name, exe_problem, lookalike;
        std::vector<uint8_t> game, exe;
        vol.for_each_file([&](const std::string& fname, uint32_t hdr) {
            if (!exe_name.empty()) return;
            std::string upper = fname;
            for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            std::vector<uint8_t> data;
            std::string why;
            if (!vol.read_file(hdr, data, why)) {
                if (upper.find("XENON") != std::string::npos) exe_problem = fname + ": " + why;
                return;
            }
            size_t code, code_len;
            if (!is_xenon_exe(data, code, code_len)) {
                if (upper.find("XENON") != std::string::npos) lookalike = fname;
                return;
            }
            exe_name = fname;
            if (!depack_xenon(data, game, why)) exe_problem = fname + ": " + why;
            exe = std::move(data);
        });
        std::string volname = vol.volume_name();
        if (!exe_problem.empty()) {
            error(name + ": corrupted disc 1 (" + exe_problem + ")", false);
            return;
        }
        if (exe_name.empty()) {
            if (!lookalike.empty())
                error(name + ": volume \"" + volname + "\" has a Xenon 2 executable (" + lookalike +
                          ") packed differently from the BS1 release: a single-disk build this runtime does not support",
                      true);
            else
                error(name + ": AmigaDOS disc \"" + volname + "\" without the XENON-II executable: not Xenon 2 disc 1", false);
            return;
        }
        Candidate c{name, sha1, "", sha1_hex(exe.data(), exe.size()), 0, d, std::move(game)};
        if (const Known* k = lookup(kDisc1, sha1)) { c.what = k->what; c.quality = k->quality; }
        else c.what = "unrecognised AmigaDOS disc \"" + volname + "\" with a Xenon 2 executable (" + exe_name + ")";
        info(name + ": " + c.what + " (sha1 " + sha1 + ")");
        disc1.push_back(std::move(c));
    }
};

bool read_file(const fs::path& p, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    uintmax_t n = fs::file_size(p, ec);
    if (ec) { why = "cannot read (" + ec.message() + ")"; return false; }
    if (n > kMaxFile) { why = "too large to be a disk image or archive (" + std::to_string(n) + " bytes)"; return false; }
    std::ifstream f(p, std::ios::binary);
    out.resize(size_t(n));
    if (!f || !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n))) { why = "cannot read"; return false; }
    return true;
}

bool write_atomic(const fs::path& path, const uint8_t* data, size_t size, std::string& error) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data), std::streamsize(size)) || !f.flush()) {
            error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot rename " + tmp.string() + " to " + path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

std::string format_message(const Message& m) {
    return std::string(m.level == Message::Error ? "error: " : m.level == Message::Warn ? "warning: " : "") + m.text;
}

int import_cli(const std::vector<std::string>& paths, const std::string& assets_dir) {
    Result r = import_paths(paths);
    for (const Message& m : r.messages) std::printf("%s\n", format_message(m).c_str());
    if (r.status != Status::Ok) {
        std::printf("import failed (%s): nothing written\n", status_name(r.status));
        return int(r.status);
    }
    std::string err;
    if (!write_assets(assets_dir, r, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("imported: %s/game_0400.bin, disc2.bin, manifest.txt\n", assets_dir.c_str());
    return 0;
}

bool ensure_imported(const std::string& assets_dir, std::string& reason) {
    std::vector<std::string> sources;
    if (verify_assets(assets_dir, reason, &sources)) return true;
    if (sources.empty()) return false;
    for (const std::string& s : sources) {
        std::error_code ec;
        if (!fs::exists(s, ec)) { reason += "; the recorded source " + s + " is gone, import again"; return false; }
    }
    std::printf("x2recomp: %s: re-importing from the recorded sources\n", reason.c_str());
    if (import_cli(sources, assets_dir) != 0) { reason = "re-import from the recorded sources failed"; return false; }
    return verify_assets(assets_dir, reason);
}

const char* status_name(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::BadInput: return "bad input";
    case Status::Unsupported: return "unsupported";
    }
    return "?";
}

Result import_inputs(const std::vector<Input>& inputs) {
    Scanner sc;
    Result r;
    for (const Input& in : inputs) {
        r.sources.push_back(in.name);
        sc.scan(in.name, in.data, 0, in.in_folder);
    }
    sc.finish_kryoflux();
    auto best = [](std::vector<Candidate>& v) -> Candidate* {
        Candidate* b = nullptr;
        for (Candidate& c : v)
            if (!b || c.quality > b->quality) b = &c;
        return b;
    };
    Candidate* d1 = best(sc.disc1);
    Candidate* d2 = best(sc.disc2);
    // A retail disk 1 (Copylock-protected, game image stored differently) is recognised but cannot be imported yet.
    if (!d1)
        for (const Scanner::RetailNote& rn : sc.retail1) {
            sc.msgs.push_back({Message::Error, rn.name + ": " + (rn.disk->two_disk
                ? std::string("retail disk 1 recognised (floppy version ") + rn.disk->version + "): importing it is not available yet, "
                  "the retail game image carries Copylock checks that the runtime does not handle yet"
                : std::string("one-disk release recognised (floppy version ") + rn.disk->version + "): not supported yet")});
            sc.unsupported = true;
        }
    if (!d1)
        sc.msgs.push_back({Message::Error, "disc 1 not found: the AmigaDOS boot disc (volume \"BAMIGA SECTOR ONE\") "
                                           "holding the packed XENON-II executable"});
    if (!d2) sc.msgs.push_back({Message::Error, "disc 2 not found: the game data disc (\"X2B \" disc id, no file system)"});
    if (!d1 || !d2) {
        r.status = sc.unsupported ? Status::Unsupported : Status::BadInput;
        r.messages = std::move(sc.msgs);
        return r;
    }
    for (auto* v : {&sc.disc1, &sc.disc2})
        for (Candidate& c : *v)
            if (&c != d1 && &c != d2) sc.msgs.push_back({Message::Info, c.name + ": not used (a preferred image of the same disc was given)"});
    for (const Scanner::RetailNote& rn : sc.retail1)
        sc.msgs.push_back({Message::Info, rn.name + ": retail disk 1 (" + rn.disk->version + ") not used: a supported disc 1 was given"});
    sc.msgs.push_back({Message::Info, "disc 1: " + d1->name});
    sc.msgs.push_back({Message::Info, "disc 2: " + d2->name});
    if (d1->quality == 0)
        sc.msgs.push_back({Message::Warn, "disc 1 is not a recognised image; importing anyway. If the game misbehaves, "
                                          "use the BS1 [t+3] disk 1 (sha1 " + std::string(kDisc1[0].sha1) + ")"});
    if (d1->exe_sha1 != kKnownExeSha1)
        sc.msgs.push_back({Message::Warn, "the XENON-II executable differs from the known release (sha1 " + d1->exe_sha1 +
                                          "); importing anyway"});
    if (d2->quality < 3)
        sc.msgs.push_back({Message::Warn, "disc 2 differs from the clean BS1 [t+3] disk 2 (sha1 " + std::string(kDisc2[0].sha1) +
                                          "); importing anyway, levels may misbehave"});
    if (d1->quality > 0)
        sc.msgs.push_back({Message::Info, "trained release: the BS1 trainer loader and its patches are skipped by design, "
                                          "the game runs unmodified (no trainer options)"});
    r.status = Status::Ok;
    r.messages = std::move(sc.msgs);
    r.game_image = std::move(d1->game);
    r.disc2 = std::move(d2->data);
    r.disc1_sha1 = d1->sha1;
    r.disc2_sha1 = d2->sha1;
    r.exe_sha1 = d1->exe_sha1;
    r.disc1_what = d1->what;
    r.disc2_what = d2->what;
    return r;
}

Result import_paths(const std::vector<std::string>& paths) {
    std::vector<Input> inputs;
    std::vector<Message> pre;
    bool bad = false;
    uintmax_t total = 0;
    auto add = [&](const fs::path& p, bool in_folder) {
        Input in{p.string(), {}, in_folder};
        std::string why;
        std::error_code ec;
        uintmax_t size = fs::file_size(p, ec);
        if (!ec && total + size > kMaxTotal) {
            pre.push_back({Message::Error, p.string() + ": skipped, too much data given (more than 512 MB in all)"});
            bad = true;
        } else if (read_file(p, in.data, why)) {
            total += in.data.size();
            inputs.push_back(std::move(in));
        } else {
            pre.push_back({Message::Error, p.string() + ": " + why});
            bad = true;
        }
    };
    for (const std::string& s : paths) {
        fs::path p(s);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> files;
            for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it.depth() >= 1) it.disable_recursion_pending();
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            if (files.empty()) { pre.push_back({Message::Error, s + ": empty folder"}); bad = true; }
            if (files.size() > kMaxFolderFiles) {
                pre.push_back({Message::Error, s + ": more than " + std::to_string(kMaxFolderFiles) +
                                                   " files; give the disk images or a smaller folder"});
                bad = true;
                files.clear();
            }
            for (const fs::path& f : files) add(f, true);
        } else if (fs::exists(p, ec)) {
            add(p, false);
        } else {
            pre.push_back({Message::Error, s + ": no such file or folder"});
            bad = true;
        }
    }
    Result r = import_inputs(inputs);
    r.messages.insert(r.messages.begin(), pre.begin(), pre.end());
    r.sources.clear();
    for (const std::string& s : paths) {
        std::error_code ec;
        fs::path abs = fs::absolute(s, ec);
        r.sources.push_back(ec ? s : abs.string());
    }
    if (bad && r.status == Status::Ok)
        r.messages.push_back({Message::Warn, "some inputs could not be read, but both discs were found"});
    return r;
}

bool write_assets(const std::string& assets_dir, const Result& r, std::string& error) {
    if (r.status != Status::Ok || r.game_image.size() != kGameImageSize || r.disc2.size() != kAdfSize) {
        error = "nothing to write: the import did not succeed";
        return false;
    }
    fs::path dir(assets_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = "cannot create " + dir.string() + ": " + ec.message(); return false; }
    fs::remove(dir / "manifest.txt", ec); // the set is incomplete until the new manifest is in place
    if (!write_atomic(dir / "game_0400.bin", r.game_image.data(), r.game_image.size(), error)) return false;
    if (!write_atomic(dir / "disc2.bin", r.disc2.data(), r.disc2.size(), error)) return false;
    std::ostringstream m;
    m << "# x2recomp imported assets (written by x2recomp --import; delete this folder to import again)\n"
      << "importer_version=" << kImporterVersion << "\n";
    for (const std::string& s : r.sources) m << "source=" << s << "\n";
    m << "disc1=" << r.disc1_what << "\n"
      << "disc1_sha1=" << r.disc1_sha1 << "\n"
      << "exe_sha1=" << r.exe_sha1 << "\n"
      << "disc2=" << r.disc2_what << "\n"
      << "disc2_sha1=" << r.disc2_sha1 << "\n"
      << "game_0400.bin_sha1=" << sha1_hex(r.game_image.data(), r.game_image.size()) << "\n"
      << "disc2.bin_sha1=" << sha1_hex(r.disc2.data(), r.disc2.size()) << "\n";
    std::string text = m.str();
    return write_atomic(dir / "manifest.txt", reinterpret_cast<const uint8_t*>(text.data()), text.size(), error);
}

bool verify_assets(const std::string& assets_dir, std::string& reason, std::vector<std::string>* sources) {
    fs::path dir(assets_dir);
    std::ifstream mf(dir / "manifest.txt");
    if (!mf) { reason = "no import manifest in " + dir.string(); return false; }
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(mf, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "source" && sources) sources->push_back(v);
        kv[k] = v;
    }
    if (kv["importer_version"] != std::to_string(kImporterVersion)) {
        reason = "imported by importer version " + kv["importer_version"] + ", this build is version " +
                 std::to_string(kImporterVersion);
        return false;
    }
    for (const char* f : {"game_0400.bin", "disc2.bin"}) {
        std::vector<uint8_t> d;
        std::string why;
        if (!read_file(dir / f, d, why)) { reason = (dir / f).string() + ": " + why; return false; }
        if (sha1_hex(d.data(), d.size()) != kv[std::string(f) + "_sha1"]) {
            reason = (dir / f).string() + " does not match the import manifest";
            return false;
        }
    }
    return true;
}

std::string fallback_pref_path() {
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\xenon2-recomp\\x2recomp\\";
    return "";
#else
    const char* app = "xenon2-recomp/x2recomp/";
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    return home && *home ? std::string(home) + "/Library/Application Support/" + app : "";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/" + app;
    return home && *home ? std::string(home) + "/.local/share/" + app : "";
#endif
#endif
}

} // namespace x2::import
