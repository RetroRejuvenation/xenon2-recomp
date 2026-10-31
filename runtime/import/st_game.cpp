// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/st_game.h"

#include "import/rnc.h"
#include "import/sha1.h"
#include "import/st_fat12.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace x2::import {

namespace {

namespace fs = std::filesystem;

constexpr uintmax_t kMaxFile = 64u << 20;     // largest file read from disk
constexpr uintmax_t kMaxTotal = 256u << 20;   // bytes read from all given paths
constexpr size_t kMaxFolderFiles = 1024;
constexpr size_t kMaxRncStreams = 64;
constexpr size_t kMaxRncUnpacked = size_t(4) << 20;
constexpr int kMaxDepth = 3;

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

bool read_small_file(const fs::path& p, uint64_t limit, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    const uintmax_t n = fs::file_size(p, ec);
    if (ec) { why = "cannot read (" + ec.message() + ")"; return false; }
    if (n > limit) { why = "too large (" + std::to_string(n) + " bytes)"; return false; }
    std::ifstream f(p, std::ios::binary);
    out.resize(size_t(n));
    if (!f || (n && !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n)))) { why = "cannot read"; return false; }
    return true;
}

std::map<std::string, std::string> parse_manifest(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

std::string hex16(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04X", v & 0xFFFF);
    return b;
}

// One ST disk found in the inputs, before and after analysis.
struct Found {
    StDiskReport report;
    std::vector<uint8_t> logical;
    std::string source_sha1;
};

std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
    return s;
}

// RNC ProPack blocks on each side's logical stream (cylinder-major, one side).
void scan_rnc(const std::vector<uint8_t>& logical, const StDiskReport& r, std::vector<StRncStream>& out) {
    const size_t track_bytes = size_t(r.sectors_per_track) * kStSectorBytes;
    if (track_bytes == 0) return;
    for (int side = 0; side < r.sides; ++side) {
        std::vector<uint8_t> stream;
        stream.reserve(size_t(r.cylinders) * track_bytes);
        for (int c = 0; c < r.cylinders; ++c) {
            const size_t off = (size_t(c) * r.sides + size_t(side)) * track_bytes;
            stream.insert(stream.end(), logical.begin() + ptrdiff_t(off), logical.begin() + ptrdiff_t(off + track_bytes));
        }
        size_t i = 0;
        while (i + kRncHeaderBytes <= stream.size() && out.size() < kMaxRncStreams) {
            const void* hit = std::memchr(stream.data() + i, 'R', stream.size() - i - kRncHeaderBytes + 1);
            if (!hit) break;
            i = size_t(static_cast<const uint8_t*>(hit) - stream.data());
            RncHeader h;
            std::vector<uint8_t> unpacked;
            std::string why;
            if (rnc_header(stream.data(), stream.size(), i, h) && h.unpacked <= kMaxRncUnpacked &&
                rnc_unpack(stream.data(), stream.size(), i, unpacked, h, why, kMaxRncUnpacked)) {
                StRncStream s;
                s.side = side;
                s.cylinder = int(i / track_bytes);
                s.track_offset = i % track_bytes;
                s.side_offset = i;
                s.packed = h.packed, s.unpacked = h.unpacked, s.packed_crc = h.packed_crc, s.unpacked_crc = h.unpacked_crc;
                out.push_back(s);
                i += kRncHeaderBytes + h.packed;
            } else {
                ++i;
            }
        }
    }
}

void protection_findings(const StDisk& d, int spt, StDiskReport& r) {
    // Copylock ST: the key sector (cylinder 0 side 0 sector 6) begins "Rob Northen Comp" and is written with a CRC error.
    if (d.cylinders > 0 && !d.track(0, 0).sectors.empty()) {
        for (const StSector& s : d.track(0, 0).sectors)
            if (s.number == 6 && s.data.size() >= 24 && std::memcmp(s.data.data(), "Rob Northen Comp", 16) == 0)
                r.findings.push_back(std::string("Rob Northen Copylock ST key sector (cylinder 0 side 0 sector 6 begins \"Rob Northen Comp\"), ") +
                                     (s.crc_error() ? "written with a CRC error" : "no CRC error recorded (a plain image cannot keep it)"));
    }
    for (int c = 0; c < d.cylinders && r.findings.size() < 12; ++c)
        for (int s = 0; s < d.sides && r.findings.size() < 12; ++s) {
            const StTrack& t = d.track(c, s);
            if (t.fuzzy_bytes)
                r.findings.push_back("weak-bit (fuzzy) mask on cylinder " + std::to_string(c) + " side " + std::to_string(s) + " (" +
                                     std::to_string(t.fuzzy_bytes) + " bytes)");
            // A sector that reads noticeably slower than its neighbours (a long sector, as on the Copylock key track).
            std::vector<unsigned> times;
            for (const StSector& sec : t.sectors)
                if (sec.read_time) times.push_back(sec.read_time);
            if (times.size() >= 6) {
                std::vector<unsigned> sorted = times;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                for (const StSector& sec : t.sectors)
                    if (sec.read_time > median * 1.03) {
                        char b[160];
                        std::snprintf(b, sizeof b, "cylinder %d side %d sector %u reads %.1f%% longer than the other sectors of its track", c, s,
                                      unsigned(sec.number), (sec.read_time / median - 1.0) * 100.0);
                        r.findings.push_back(b);
                    }
            }
        }
    (void)spt;
}

// Analyses one decoded disk: the logical image, its identity, boot sector, protection findings, FAT12 listing and RNC blocks.
bool analyse(const std::string& name, const StDisk& d, StContainer container, const std::vector<StRelease>& catalogue, Found& f) {
    StDiskReport& r = f.report;
    r.name = name;
    r.container = container;
    r.cylinders = d.cylinders;
    r.sides = d.sides;
    r.sectors_per_track = d.sectors_per_track();
    if (r.sectors_per_track < 1) {
        r.problems.push_back("no 512-byte sectors on any track");
        return false;
    }
    d.logical_image(r.sectors_per_track, f.logical, r.problems);
    r.logical_sha1 = sha1_hex(f.logical.data(), f.logical.size());
    if (f.logical.size() >= 512) {
        r.boot_checksum = st_boot_checksum(f.logical.data());
        r.executable_boot = r.boot_checksum == 0x1234;
        r.oem = printable(f.logical.data() + 2, 6);
    }
    r.nonstandard = d.nonstandard_tracks(r.sectors_per_track, 8, &r.nonstandard_total);
    protection_findings(d, r.sectors_per_track, r);
    {
        FatInfo info;
        std::vector<FatEntry> entries;
        std::vector<std::string> warnings;
        std::string why;
        if (fat12_list(f.logical.data(), f.logical.size(), info, entries, warnings, why)) {
            r.fat_note = info.label.empty() ? "FAT12 volume without a label" : "FAT12 volume \"" + info.label + "\"";
            for (const FatEntry& e : entries) {
                if (r.files.size() >= 24) { r.files.push_back("..."); break; }
                r.files.push_back(e.path + (e.directory ? "/" : " (" + std::to_string(e.size) + " bytes)"));
            }
            for (size_t w = 0; w < warnings.size() && w < 3; ++w) r.fat_note += "; " + warnings[w];
            if (warnings.size() > 3) r.fat_note += "; ... (" + std::to_string(warnings.size() - 3) + " more warnings)";
        } else {
            r.fat_note = "no FAT12 volume (" + why + ")";
        }
    }
    scan_rnc(f.logical, r, r.rnc);
    r.release = nullptr;
    for (const StRelease& rel : catalogue)
        if (rel.disk_sha1 == r.logical_sha1) r.release = &rel;
    return true;
}

class Scanner {
public:
    std::vector<Found> disks;
    std::vector<Message> msgs;
    const std::vector<StRelease>* catalogue = &st_releases();
    bool detect_only = false; // looks_like_st_image: no parsing, just "is there one"
    bool saw_st = false;
    size_t budget = size_t(kMaxTotal);

    void error(const std::string& s) { msgs.push_back({Message::Error, s}); }
    void info(const std::string& s) { msgs.push_back({Message::Info, s}); }
    bool charge(const std::string& name, size_t n) {
        if (n > budget) {
            error(name + ": too much data when unpacked (more than " + std::to_string(kMaxTotal >> 20) + " MB in all)");
            return false;
        }
        budget -= n;
        return true;
    }

    void scan(const std::string& name, const std::vector<uint8_t>& d, int depth, bool nested) {
        if (d.size() >= 4 && std::memcmp(d.data(), "PK\x03\x04", 4) == 0) {
            if (depth >= kMaxDepth) { error(name + ": archives nested too deeply"); return; }
            std::vector<ZipEntry> entries;
            std::string why;
            if (!unzip(d, entries, why, budget)) { error(name + ": " + why); return; }
            size_t got = 0;
            for (const ZipEntry& e : entries) got += e.data.size();
            if (!charge(name, got)) return;
            for (ZipEntry& e : entries) {
                const std::string en = name + " > " + e.name;
                if (!e.problem.empty()) error(en + ": " + e.problem);
                else scan(en, e.data, depth + 1, true);
            }
            return;
        }
        const StContainer c = detect_st_container(d.data(), d.size());
        if (c == StContainer::None) {
            if (nested) info(name + ": not an Atari ST disk image, skipped");
            else error(name + ": not an Atari ST disk image (.st, .msa or Pasti .stx)");
            return;
        }
        saw_st = true;
        if (detect_only) return;
        StDisk disk;
        std::string why;
        bool ok = false;
        switch (c) {
        case StContainer::St: ok = read_st_image(d.data(), d.size(), disk, why); break;
        case StContainer::Msa: ok = read_msa(d.data(), d.size(), disk, why); break;
        case StContainer::Stx: ok = read_stx(d.data(), d.size(), disk, why); break;
        case StContainer::None: break;
        }
        if (!ok) { error(name + ": " + std::string(st_container_name(c)) + " image: " + why); return; }
        if (!charge(name, size_t(disk.cylinders) * size_t(disk.sides) * 11 * kStSectorBytes)) return;
        Found f;
        f.source_sha1 = sha1_hex(d.data(), d.size());
        if (!analyse(name, disk, c, *catalogue, f)) {
            error(name + ": " + st_container_name(c) + " image with " + f.report.problems.front());
            return;
        }
        disks.push_back(std::move(f));
    }
};

// Reads the given paths into memory: files, ZIPs (as files), folders (one level).
bool collect_inputs(const std::vector<std::string>& paths, std::vector<StInput>& inputs, std::vector<Message>& pre) {
    bool ok = true;
    uintmax_t total = 0;
    auto add = [&](const fs::path& p, bool in_folder) {
        StInput in;
        in.name = p.string();
        in.in_folder = in_folder;
        std::string why;
        std::error_code ec;
        const uintmax_t n = fs::file_size(p, ec);
        if (!ec && total + n > kMaxTotal) {
            pre.push_back({Message::Error, p.string() + ": skipped, too much data given (more than " + std::to_string(kMaxTotal >> 20) + " MB in all)"});
            ok = false;
        } else if (read_small_file(p, kMaxFile, in.data, why)) {
            total += in.data.size();
            inputs.push_back(std::move(in));
        } else {
            pre.push_back({Message::Error, p.string() + ": " + why});
            ok = false;
        }
    };
    for (const std::string& s : paths) {
        const fs::path p(s);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> files;
            for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
                 it.increment(ec)) {
                std::error_code fec;
                if (it->is_regular_file(fec)) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            if (files.empty()) { pre.push_back({Message::Error, s + ": empty folder"}); ok = false; }
            if (files.size() > kMaxFolderFiles) {
                pre.push_back({Message::Error, s + ": more than " + std::to_string(kMaxFolderFiles) + " files; give the disk images or a smaller folder"});
                ok = false;
                files.clear();
            }
            for (const fs::path& f : files) add(f, true);
        } else if (fs::exists(p, ec)) {
            add(p, false);
        } else {
            pre.push_back({Message::Error, s + ": no such file or folder"});
            ok = false;
        }
    }
    return ok;
}

std::string absolute_of(const std::string& s) {
    std::error_code ec;
    const fs::path a = fs::absolute(s, ec);
    return ec ? s : a.string();
}

} // namespace

bool looks_like_st_image(const std::vector<std::string>& paths) {
    std::vector<StInput> inputs;
    std::vector<Message> pre;
    collect_inputs(paths, inputs, pre);
    Scanner sc;
    sc.detect_only = true;
    for (const StInput& in : inputs) {
        sc.scan(in.name, in.data, 0, in.in_folder);
        if (sc.saw_st) return true;
    }
    return false;
}

StGame import_st_inputs(const std::vector<StInput>& inputs, const std::vector<StRelease>* releases) {
    StGame g;
    Scanner sc;
    if (releases) sc.catalogue = releases;
    for (const StInput& in : inputs) {
        g.sources.push_back(in.name);
        sc.scan(in.name, in.data, 0, in.in_folder);
    }
    g.messages = std::move(sc.msgs);
    if (sc.disks.empty()) {
        bool has_error = false;
        for (const Message& m : g.messages) has_error = has_error || m.level == Message::Error;
        if (!has_error) g.messages.push_back({Message::Error, "no Atari ST disk image found (.st, .msa or Pasti .stx, also in a ZIP or folder)"});
        g.status = Status::BadInput;
        return g;
    }
    Found* chosen = nullptr;
    for (Found& f : sc.disks)
        if (f.report.release && f.report.release->importable) { chosen = &f; break; }
    for (const Found& f : sc.disks) {
        const StDiskReport& r = f.report;
        std::string line = r.name + ": " + st_container_name(r.container) + ", " + std::to_string(r.cylinders) + " cylinders x " +
                           std::to_string(r.sides) + (r.sides == 1 ? " side x " : " sides x ") + std::to_string(r.sectors_per_track) + " sectors";
        line += r.release ? ", recognised as " + r.release->name : ", not a catalogued Xenon 2 ST disk (logical image sha1 " + r.logical_sha1 + ")";
        g.messages.push_back({Message::Info, line});
    }
    if (!chosen) {
        // Nothing importable: say why for the disks that are recognised, or that none is.
        bool said = false;
        for (const Found& f : sc.disks) {
            const StDiskReport& r = f.report;
            if (r.release) {
                g.messages.push_back({Message::Error, r.name + ": " + r.release->name + ": " + r.release->note});
                said = true;
            }
        }
        if (!said) {
            const StDiskReport& r = sc.disks.front().report;
            g.messages.push_back({Message::Error, r.name + ": this is not a Xenon 2 Megablast ST disk the importer knows (logical image sha1 " + r.logical_sha1 +
                                                      "); it is not a general ST disk tool. Importable: the Kixx budget re-release"});
        }
        g.messages.push_back({Message::Info, "run x2recomp --list-variants for the ST releases, and --diagnose-image on the disk for what is on it"});
        g.status = Status::Unsupported;
        g.disk = sc.disks.front().report;
        return g;
    }
    for (Found& f : sc.disks) {
        if (&f == chosen) continue;
        if (f.report.release == chosen->report.release)
            g.messages.push_back({Message::Info, f.report.name + ": the same disk as " + chosen->report.name + ", not used"});
        else
            g.messages.push_back({Message::Info, f.report.name + ": not used (not importable)"});
        g.others.push_back(f.report);
    }
    g.disk = chosen->report;
    g.image = std::move(chosen->logical);
    g.image_sha1 = chosen->report.logical_sha1;
    g.source_sha1 = chosen->source_sha1;
    g.messages.push_back({Message::Info, "unprotected release: the loader reads raw tracks and the game never calls TOS, so the disk is imported as it is"});
    if (!g.disk.executable_boot)
        g.messages.push_back({Message::Warn, "the boot sector is not executable (checksum " + hex16(g.disk.boot_checksum) + "): the disk will not boot"});
    g.status = Status::Ok;
    return g;
}

StGame import_st_paths(const std::vector<std::string>& paths) {
    std::vector<StInput> inputs;
    std::vector<Message> pre;
    const bool read_ok = collect_inputs(paths, inputs, pre);
    StGame g = import_st_inputs(inputs);
    g.messages.insert(g.messages.begin(), pre.begin(), pre.end());
    g.sources.clear();
    for (const std::string& s : paths) g.sources.push_back(absolute_of(s));
    if (!read_ok && g.status == Status::Ok) g.messages.push_back({Message::Warn, "some inputs could not be read, but the disk was found"});
    return g;
}

std::vector<StDiskReport> diagnose_st_inputs(const std::vector<StInput>& inputs, std::vector<Message>& messages,
                                             const std::vector<StRelease>* releases) {
    Scanner sc;
    if (releases) sc.catalogue = releases;
    for (const StInput& in : inputs) sc.scan(in.name, in.data, 0, in.in_folder);
    messages = std::move(sc.msgs);
    std::vector<StDiskReport> out;
    for (Found& f : sc.disks) out.push_back(std::move(f.report));
    return out;
}

std::vector<std::string> format_st_report(const StDiskReport& r) {
    std::vector<std::string> l;
    l.push_back(r.name + ":");
    l.push_back("  container: " + std::string(st_container_name(r.container)));
    l.push_back("  geometry: " + std::to_string(r.cylinders) + " cylinders x " + std::to_string(r.sides) + (r.sides == 1 ? " side x " : " sides x ") +
                std::to_string(r.sectors_per_track) + " sectors of 512 bytes (" +
                std::to_string(size_t(r.cylinders) * size_t(r.sides) * size_t(r.sectors_per_track) * kStSectorBytes) + " bytes)");
    l.push_back("  boot sector: checksum " + hex16(r.boot_checksum) + (r.executable_boot ? " (executable)" : " (not executable)") + ", OEM \"" + r.oem + "\"");
    if (r.nonstandard_total == 0) {
        l.push_back("  tracks: all " + std::to_string(size_t(r.cylinders) * size_t(r.sides)) + " are clean 512-byte sector tracks");
    } else {
        l.push_back("  tracks: " + std::to_string(r.nonstandard_total) + " of " + std::to_string(size_t(r.cylinders) * size_t(r.sides)) +
                    " are not clean 512-byte sector tracks:");
        for (const std::string& s : r.nonstandard) l.push_back("    " + s);
        if (r.nonstandard_total > r.nonstandard.size()) l.push_back("    ...");
    }
    if (r.findings.empty()) {
        l.push_back("  copy protection: none found (no key sector, no weak bits, no slow sector)");
    } else {
        l.push_back("  copy protection findings:");
        for (const std::string& s : r.findings) l.push_back("    " + s);
    }
    if (!r.problems.empty()) {
        l.push_back("  sectors the logical image cannot hold intact:");
        for (const std::string& s : r.problems) l.push_back("    " + s);
    }
    l.push_back("  " + r.fat_note);
    for (const std::string& f : r.files) l.push_back("    " + f);
    if (r.rnc.empty()) {
        l.push_back("  RNC ProPack blocks: none that verify");
    } else {
        l.push_back("  RNC ProPack blocks (both CRC16s verify): " + std::to_string(r.rnc.size()));
        size_t shown = 0;
        for (const StRncStream& s : r.rnc) {
            if (++shown > 16) { l.push_back("    ..."); break; }
            l.push_back("    side " + std::to_string(s.side) + " cylinder " + std::to_string(s.cylinder) + " +" + std::to_string(s.track_offset) + ": " +
                        std::to_string(s.packed) + " -> " + std::to_string(s.unpacked) + " bytes");
        }
    }
    l.push_back("  logical image sha1: " + r.logical_sha1);
    if (r.release) {
        l.push_back("  recognised as: " + r.release->name + (r.release->importable ? " (importable, build=" + r.release->build_tag + ")"
                                                                                  : " (not importable: " + r.release->note + ")"));
    } else {
        l.push_back("  recognised as: not a catalogued Xenon 2 ST disk");
    }
    return l;
}

bool diagnose_st(const std::string& path, std::vector<std::string>& lines) {
    lines.clear();
    if (!looks_like_st_image({path})) return false;
    std::vector<StInput> inputs;
    std::vector<Message> pre;
    collect_inputs({path}, inputs, pre);
    std::vector<Message> msgs;
    const std::vector<StDiskReport> reports = diagnose_st_inputs(inputs, msgs);
    for (const Message& m : pre) lines.push_back(format_message(m));
    for (const Message& m : msgs)
        if (m.level != Message::Info) lines.push_back(format_message(m));
    for (const StDiskReport& r : reports)
        for (const std::string& s : format_st_report(r)) lines.push_back(s);
    return true;
}

bool write_st_assets(const std::string& dir, const StGame& g, std::string& error) {
    if (g.status != Status::Ok || g.image.empty() || !g.disk.release || !g.disk.release->importable) {
        error = "nothing to write: the import did not succeed";
        return false;
    }
    const fs::path d(dir);
    std::error_code ec;
    fs::create_directories(d, ec);
    if (ec) { error = "cannot create " + d.string() + ": " + ec.message(); return false; }
    fs::remove(d / "manifest.txt", ec); // the set is incomplete until the new manifest is in place
    // Files of an earlier import of another build in this folder do not belong to this one.
    fs::remove(d / "game_0400.bin", ec);
    fs::remove(d / "disc2.bin", ec);
    fs::remove(d / "copylock.txt", ec);
    fs::remove_all(d / "cd0", ec);
    if (!write_atomic(d / "disk.bin", g.image.data(), g.image.size(), error)) return false;
    const StDiskReport& r = g.disk;
    std::ostringstream m;
    m << "# x2recomp imported assets (written by x2recomp --import; delete this folder to import again)\n"
      << "importer_version=" << kImporterVersion << "\n";
    for (const std::string& s : g.sources) m << "source=" << s << "\n";
    m << "build=" << r.release->build_tag << "\n"
      << "game=" << kStGameTag << "\n"
      << "variant=" << r.release->id << "\n"
      << "variant_name=" << r.release->name << "\n"
      << "container=" << st_container_name(r.container) << "\n"
      << "canonical=false\npatched=false\ncopylock=absent\n"
      << "cylinders=" << r.cylinders << "\n"
      << "sides=" << r.sides << "\n"
      << "sectors_per_track=" << r.sectors_per_track << "\n"
      << "sector_bytes=" << kStSectorBytes << "\n"
      << "layout=cylinder-major, sides interleaved (the .st layout)\n"
      << "boot_checksum=" << hex16(r.boot_checksum) << "\n"
      << "bootable=" << (r.executable_boot ? "true" : "false") << "\n"
      << "source_sha1=" << g.source_sha1 << "\n"
      << "disk.bin_size=" << g.image.size() << "\n"
      << "disk.bin_sha1=" << g.image_sha1 << "\n"
      << "rnc_streams=" << r.rnc.size() << "\n";
    for (size_t i = 0; i < r.rnc.size(); ++i) {
        const StRncStream& s = r.rnc[i];
        m << "rnc." << i << "=side=" << s.side << ",cylinder=" << s.cylinder << ",offset=" << s.track_offset << ",side_offset=" << s.side_offset
          << ",packed=" << s.packed << ",unpacked=" << s.unpacked << ",crc_packed=" << hex16(s.packed_crc) << ",crc_unpacked=" << hex16(s.unpacked_crc) << "\n";
    }
    const std::string text = m.str();
    return write_atomic(d / "manifest.txt", reinterpret_cast<const uint8_t*>(text.data()), text.size(), error);
}

bool manifest_is_st(const std::string& manifest_text) {
    const auto kv = parse_manifest(manifest_text);
    const auto it = kv.find("build");
    return it != kv.end() && it->second.rfind(kStBuildPrefix, 0) == 0;
}

bool verify_st_assets(const std::string& dir, const std::string& manifest_text, std::string& reason) {
    const auto kv = parse_manifest(manifest_text);
    const fs::path p = fs::path(dir) / "disk.bin";
    std::vector<uint8_t> bytes;
    std::string why;
    if (!read_small_file(p, kMaxFile, bytes, why)) { reason = p.string() + ": " + why; return false; }
    const auto size = kv.find("disk.bin_size");
    const auto sha = kv.find("disk.bin_sha1");
    if (size == kv.end() || sha == kv.end() || std::to_string(bytes.size()) != size->second || sha1_hex(bytes.data(), bytes.size()) != sha->second) {
        reason = p.string() + " does not match the import manifest";
        return false;
    }
    return true;
}

int import_st_cli(const std::vector<std::string>& paths, const std::string& assets_dir) {
    StGame g = import_st_paths(paths);
    for (const Message& m : g.messages) std::printf("%s\n", format_message(m).c_str());
    if (g.status != Status::Ok) {
        std::printf("import failed (%s): nothing written\n", status_name(g.status));
        return int(g.status);
    }
    std::string err;
    if (!write_st_assets(assets_dir, g, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("imported (%s): %s/disk.bin, manifest.txt\n", g.disk.release->name.c_str(), assets_dir.c_str());
    return 0;
}

} // namespace x2::import
