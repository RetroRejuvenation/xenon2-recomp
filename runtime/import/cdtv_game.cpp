// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/cdtv_game.h"

#include "cdtv/cdtv_disc.h"
#include "import/iso9660.h"
#include "import/sha1.h"
#include "import/variants.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace x2::import {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMaxGameBytes = 64ull << 20; // everything read from the disc for the game
constexpr uint32_t kMaxX2Bytes = 16u << 20;

bool all_digits(const std::string& s, size_t from, size_t count) {
    if (s.size() != from + count) return false;
    for (size_t i = from; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// The files the game opens by name (docs/re-notes/cdtv-c0.md, section on loader sites): INTRO, LEVEL1..5, PICCODE,
// PICDATA<digit or letter>, SOUNDS<nn>, SPEECH<nn>. Anything else on the disc is the CDTV's own software (CDTV.TM,
// CDTVPREFS, RMTM, the startup sequence) and not part of the game.
bool is_game_file(const std::string& name) {
    if (name == "INTRO" || name == "PICCODE") return true;
    if (name.size() == 6 && name.compare(0, 5, "LEVEL") == 0) return name[5] >= '1' && name[5] <= '5';
    if (name.size() == 8 && name.compare(0, 7, "PICDATA") == 0) return std::isalnum(static_cast<unsigned char>(name[7])) && !std::islower(static_cast<unsigned char>(name[7]));
    if (name.compare(0, 6, "SOUNDS") == 0) return all_digits(name, 6, 2);
    if (name.compare(0, 6, "SPEECH") == 0) return all_digits(name, 6, 2);
    return false;
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

bool read_small_file(const fs::path& p, uint64_t limit, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    uintmax_t n = fs::file_size(p, ec);
    if (ec) { why = "cannot read (" + ec.message() + ")"; return false; }
    if (n > limit) { why = "too large (" + std::to_string(n) + " bytes)"; return false; }
    std::ifstream f(p, std::ios::binary);
    out.resize(size_t(n));
    if (!f || (n && !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n)))) { why = "cannot read"; return false; }
    return true;
}

// Sector 16 of a bare data track (raw MODE1/2352 or cooked 2048): the volume id when it is an ISO 9660 volume.
bool quick_volume(const uint8_t* head, size_t n, std::string& volume) {
    static const uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    const uint8_t* pvd = nullptr;
    if (n >= size_t(17) * 2352 && std::memcmp(head + size_t(16) * 2352, kSync, 12) == 0 && head[size_t(16) * 2352 + 15] == 1)
        pvd = head + size_t(16) * 2352 + 16;
    else if (n >= size_t(17) * 2048)
        pvd = head + size_t(16) * 2048;
    if (!pvd || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    volume.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    volume.erase(volume.find_last_not_of(' ') + 1);
    return true;
}

bool bare_cdtv_track(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::string lower = path;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.size() >= 4 && (lower.compare(lower.size() - 4, 4, ".cue") == 0 || lower.compare(lower.size() - 4, 4, ".zip") == 0)) return false;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> head(size_t(17) * 2352);
    if (!f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()))) head.resize(size_t(f.gcount()));
    std::string volume;
    return quick_volume(head.data(), head.size(), volume) && volume == "XENONII";
}

std::string upper_languages(const std::vector<uint8_t>& code) {
    static const struct { const char* code; const char* word; } kWords[] = {
        {"EN", "ENGLISH"}, {"FR", "FRANCAIS"}, {"DE", "DEUTSCH"}, {"IT", "ITALIANO"}};
    std::string out;
    for (const auto& w : kWords) {
        const size_t n = std::strlen(w.word);
        auto it = std::search(code.begin(), code.end(), w.word, w.word + n);
        if (it != code.end()) out += (out.empty() ? "" : ",") + std::string(w.code);
    }
    return out;
}

Status from_cdtv(cdtv::Status s) {
    switch (s) {
    case cdtv::Status::Unsupported: return Status::Unsupported;
    default: return Status::BadInput;
    }
}

std::string hex_u32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "$%X", v);
    return b;
}

const char* flags_name(uint32_t f) { return f == 1 ? "chip" : f == 2 ? "fast" : "any"; }
const char* kind_name(Hunk::Kind k) { return k == Hunk::Kind::Code ? "code" : k == Hunk::Kind::Data ? "data" : "bss"; }

std::map<std::string, std::string> parse_manifest(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

} // namespace

bool looks_like_cdtv_game_image(const std::string& path) { return cdtv::looks_like_disc_image(path) || bare_cdtv_track(path); }

CdtvGame import_cdtv_game(const std::string& path) {
    CdtvGame g;
    auto info = [&](const std::string& s) { g.messages.push_back({Message::Info, s}); };
    auto warn = [&](const std::string& s) { g.messages.push_back({Message::Warn, s}); };
    auto fail = [&](Status st, const std::string& s) {
        g.messages.push_back({Message::Error, s});
        g.status = st;
        return g;
    };

    // 1. The data track.
    std::vector<uint8_t> raw;
    std::string why;
    if (cdtv::looks_like_disc_image(path)) {
        cdtv::Status cs = cdtv::Status::BadInput;
        if (!cdtv::read_data_track(path, raw, g.source, why, cs)) return fail(from_cdtv(cs), path + ": " + why);
        g.has_cue = true;
        std::error_code ec;
        g.container = fs::is_directory(path, ec) ? "CDTV disc image (folder with .cue and .bin)"
                                                 : (path.size() >= 4 && (path.compare(path.size() - 4, 4, ".zip") == 0 || path.compare(path.size() - 4, 4, ".ZIP") == 0))
                                                       ? "CDTV disc image (ZIP)" : "CDTV disc image (.cue and .bin)";
    } else {
        std::error_code ec;
        if (!fs::exists(path, ec)) return fail(Status::BadInput, path + ": no such file or folder");
        fs::path abs = fs::absolute(path, ec);
        g.source = (ec ? fs::path(path) : abs).lexically_normal().string();
        if (!read_small_file(path, cdtv::kMaxDataTrackBytes, raw, why)) return fail(Status::BadInput, path + ": " + why);
    }
    IsoVolume vol;
    if (!vol.open(raw.data(), raw.size(), why)) return fail(Status::Unsupported, path + ": " + why + " (not a CD data track)");
    g.volume_id = vol.volume_id();
    if (g.volume_id != "XENONII")
        return fail(Status::Unsupported, path + ": the CD volume is '" + g.volume_id + "', not XENONII: not the Xenon 2 CDTV disc");
    if (!g.has_cue) g.container = vol.raw() ? "data track (raw MODE1/2352 .bin)" : "data track (.iso)";

    // 2. The files of the volume.
    std::vector<IsoEntry> entries;
    if (!vol.list(entries, why)) return fail(Status::BadInput, path + ": damaged ISO 9660 volume: " + why);
    g.iso_files = entries.size();
    const IsoEntry* x2 = nullptr;
    for (const IsoEntry& e : entries)
        if (e.path == "X2") x2 = &e;
    if (!x2) return fail(Status::Unsupported, path + ": the volume has no file X2 (the CDTV game): not the Xenon 2 CDTV disc");
    if (x2->size == 0 || x2->size > kMaxX2Bytes) return fail(Status::BadInput, path + ": X2 has an implausible size (" + std::to_string(x2->size) + " bytes)");

    // 3. X2: hunk file, linked at the fixed bases.
    std::vector<uint8_t> x2_bytes;
    if (!vol.read(*x2, x2_bytes, why)) return fail(Status::BadInput, path + ": " + why);
    g.x2_sha1 = sha1_hex(x2_bytes.data(), x2_bytes.size());
    HunkFile hf;
    if (!parse_hunk_file(x2_bytes.data(), x2_bytes.size(), hf, why)) return fail(Status::BadInput, "X2: " + why);
    if (hf.hunks.empty() || hf.hunks[0].kind != Hunk::Kind::Code) return fail(Status::BadInput, "X2: the first hunk is not code, so there is no entry point");
    for (const Hunk& h : hf.hunks) {
        g.hunk_kinds.push_back(uint8_t(h.kind));
        g.hunk_flags.push_back(h.mem_flags);
        if (h.mem_flags == 2) return fail(Status::Unsupported, "X2: a hunk wants fast RAM; the CDTV build runs in chip RAM only");
    }
    if (!link_hunk_file(hf, kCdtvBase, kCdtvChipLimit, g.link, why)) return fail(Status::BadInput, "X2: " + why);
    g.image = std::move(g.link.image);
    g.image_sha1 = sha1_hex(g.image.data(), g.image.size());
    g.languages = upper_languages(hf.hunks[0].data);

    // 4. The files the game loads.
    uint64_t total = 0;
    for (const IsoEntry& e : entries) {
        if (e.path != e.name || !is_game_file(e.name)) continue; // only the root, only the game's own names
        total += e.size;
        if (total > kMaxGameBytes) return fail(Status::BadInput, "the game's files together are larger than " + std::to_string(kMaxGameBytes >> 20) + " MB");
        CdtvFile f;
        f.name = e.name;
        if (!vol.read(e, f.data, why)) return fail(Status::BadInput, path + ": " + why);
        f.sha1 = sha1_hex(f.data.data(), f.data.size());
        g.files.push_back(std::move(f));
    }
    auto have = [&](const std::string& n) {
        for (const CdtvFile& f : g.files)
            if (f.name == n) return true;
        return false;
    };
    std::string missing;
    for (const char* n : {"INTRO", "PICCODE", "LEVEL1", "LEVEL2", "LEVEL3", "LEVEL4", "LEVEL5"})
        if (!have(n)) missing += (missing.empty() ? "" : ", ") + std::string(n);
    if (!missing.empty()) return fail(Status::Unsupported, "the disc lacks files the game loads (" + missing + "): not the Xenon 2 CDTV disc");

    // 5. Identify.
    const CdtvDisc& known = builtin_cdtv_disc();
    g.recognised = g.x2_sha1 == known.x2_sha1;
    g.variant_id = g.recognised ? known.id : "cdtv-unrecognised";
    g.variant_name = g.recognised ? known.name : "a CDTV disc image with an uncatalogued X2";
    info("disc image: " + g.source + " (" + g.container + ")");
    info("ISO 9660 volume " + g.volume_id + ", " + std::to_string(vol.volume_blocks()) + " blocks, " + std::to_string(g.iso_files) + " files");
    {
        std::string hunks;
        for (size_t i = 0; i < hf.hunks.size(); ++i)
            hunks += (i ? ", " : "") + std::string("hunk ") + std::to_string(i) + " " + kind_name(hf.hunks[i].kind) + " " +
                     std::to_string(hf.hunks[i].mem_bytes) + " bytes (" + flags_name(hf.hunks[i].mem_flags) + ") at " + hex_u32(g.link.hunk_base[i]) +
                     ", " + std::to_string(g.link.reloc_count[i]) + " fix-ups inside, " + std::to_string(g.link.target_count[i]) + " pointing into it";
        info("X2: AmigaDOS hunk file, " + std::to_string(x2_bytes.size()) + " bytes; " + hunks);
    }
    info("linked at " + hex_u32(g.link.base) + ": game_0400.bin is " + std::to_string(g.image.size()) + " bytes, entry " + hex_u32(g.link.base) + ", " +
         std::to_string(g.link.reloc_total) + " fix-ups applied");
    if (g.link.high_byte_values)
        info(std::to_string(g.link.high_byte_values) + " relocated value(s) carry a non-zero upper byte (a 68000 ignores it: 24-bit address bus); "
             "kept as the OS loader would keep it");
    info("languages in the program: " + (g.languages.empty() ? std::string("none found") : g.languages));
    info(std::to_string(g.files.size()) + " game files stored under cd0/ (" + std::to_string(total >> 10) + " KB)");
    if (g.recognised) {
        info("recognised: " + known.name + (g.image_sha1 == known.game_sha1 ? "" : " (the linked image differs from the expected one; the importer changed?)"));
        if (g.files.size() != known.game_files) warn("this disc has " + std::to_string(g.files.size()) + " game files, the catalogued disc has " + std::to_string(known.game_files));
    } else {
        warn("X2 differs from the catalogued CDTV disc (sha1 " + g.x2_sha1 + "); importing anyway");
    }
    info("the CDTV build is imported and verified but not playable yet: the runtime support is planned for 0.3.0");
    g.status = Status::Ok;
    return g;
}

bool write_cdtv_assets(const std::string& dir, const CdtvGame& g, std::string& error) {
    if (g.status != Status::Ok || g.image.empty()) { error = "nothing to write: the import did not succeed"; return false; }
    fs::path d(dir);
    std::error_code ec;
    fs::create_directories(d, ec);
    if (ec) { error = "cannot create " + d.string() + ": " + ec.message(); return false; }
    fs::remove(d / "manifest.txt", ec); // the set is incomplete until the new manifest is in place
    // Files of an earlier import that do not belong to this one.
    fs::remove(d / "disc2.bin", ec);
    fs::remove(d / "copylock.txt", ec);
    fs::remove_all(d / "cd0", ec);
    fs::create_directories(d / "cd0", ec);
    if (ec) { error = "cannot create " + (d / "cd0").string() + ": " + ec.message(); return false; }
    if (!write_atomic(d / "game_0400.bin", g.image.data(), g.image.size(), error)) return false;
    for (const CdtvFile& f : g.files)
        if (!write_atomic(d / "cd0" / f.name, f.data.data(), f.data.size(), error)) return false;
    std::ostringstream m;
    m << "# x2recomp imported assets (written by x2recomp --import; delete this folder to import again)\n"
      << "importer_version=" << kImporterVersion << "\n"
      << "source=" << g.source << "\n"
      << "build=" << kCdtvBuildTag << "\n"
      << "chip_kb=" << kCdtvChipKb << "\n"
      << "variant=" << g.variant_id << "\n"
      << "variant_name=" << g.variant_name << "\n"
      << "recognised=" << (g.recognised ? "true" : "false") << "\n"
      << "container=" << g.container << "\n"
      << "canonical=false\npatched=false\ncopylock=absent\n"
      << "languages=" << g.languages << "\n"
      << "iso_volume=" << g.volume_id << "\n"
      << "iso_files=" << g.iso_files << "\n"
      << "x2_sha1=" << g.x2_sha1 << "\n"
      << "game_0400.bin_sha1=" << g.image_sha1 << "\n"
      << "image_base=" << hex_u32(g.link.base) << "\n"
      << "image_bytes=" << g.image.size() << "\n"
      << "entry=" << hex_u32(g.link.base) << "\n"
      << "hunk_count=" << g.link.hunk_base.size() << "\n";
    for (size_t i = 0; i < g.link.hunk_base.size(); ++i)
        m << "hunk." << i << "=" << (g.hunk_kinds[i] == uint8_t(Hunk::Kind::Code) ? "code" : g.hunk_kinds[i] == uint8_t(Hunk::Kind::Data) ? "data" : "bss") << ","
          << g.link.hunk_bytes[i] << "," << flags_name(g.hunk_flags[i]) << ",base=" << hex_u32(g.link.hunk_base[i]) << ",fixups_in=" << g.link.reloc_count[i] << ",fixups_to=" << g.link.target_count[i] << "\n";
    m << "reloc_total=" << g.link.reloc_total << "\n"
      << "file_count=" << g.files.size() << "\n";
    for (size_t i = 0; i < g.files.size(); ++i) m << "file." << i << "=" << g.files[i].name << "," << g.files[i].data.size() << "," << g.files[i].sha1 << "\n";
    const std::string text = m.str();
    return write_atomic(d / "manifest.txt", reinterpret_cast<const uint8_t*>(text.data()), text.size(), error);
}

bool manifest_is_cdtv(const std::string& text) {
    const auto kv = parse_manifest(text);
    auto it = kv.find("build");
    return it != kv.end() && it->second == kCdtvBuildTag;
}

bool verify_cdtv_assets(const std::string& dir, const std::string& manifest_text, std::string& reason) {
    auto kv = parse_manifest(manifest_text);
    fs::path d(dir);
    std::vector<uint8_t> bytes;
    std::string why;
    if (!read_small_file(d / "game_0400.bin", kMaxGameBytes, bytes, why)) { reason = (d / "game_0400.bin").string() + ": " + why; return false; }
    if (sha1_hex(bytes.data(), bytes.size()) != kv["game_0400.bin_sha1"]) { reason = (d / "game_0400.bin").string() + " does not match the import manifest"; return false; }
    size_t count = 0;
    try { count = size_t(std::stoul(kv["file_count"])); } catch (...) { reason = "the import manifest has no valid file_count"; return false; }
    if (count == 0 || count > 1024) { reason = "the import manifest lists an implausible number of files"; return false; }
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const std::string row = kv["file." + std::to_string(i)];
        const auto c1 = row.find(','), c2 = row.find(',', c1 == std::string::npos ? 0 : c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) { reason = "the import manifest's file table is damaged (row " + std::to_string(i) + ")"; return false; }
        const std::string name = row.substr(0, c1), sha = row.substr(c2 + 1);
        uint64_t size = 0;
        try { size = std::stoull(row.substr(c1 + 1, c2 - c1 - 1)); } catch (...) { reason = "the import manifest's file table is damaged (row " + std::to_string(i) + ")"; return false; }
        if (!is_game_file(name)) { reason = "the import manifest names a file that is not one of the game's (" + name + ")"; return false; }
        const fs::path p = d / "cd0" / name;
        if (!read_small_file(p, kMaxGameBytes, bytes, why)) { reason = p.string() + ": " + why; return false; }
        total += bytes.size();
        if (total > kMaxGameBytes) { reason = "the stored game files are larger than expected"; return false; }
        if (bytes.size() != size || sha1_hex(bytes.data(), bytes.size()) != sha) { reason = p.string() + " does not match the import manifest"; return false; }
    }
    return true;
}

int import_cdtv_game_cli(const std::string& path, const std::string& assets_dir) {
    CdtvGame g = import_cdtv_game(path);
    for (const Message& m : g.messages) std::printf("%s\n", format_message(m).c_str());
    if (g.status != Status::Ok) {
        std::printf("import failed (%s): nothing written\n", status_name(g.status));
        return int(g.status);
    }
    std::string err;
    if (!write_cdtv_assets(assets_dir, g, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("imported (%s): %s/game_0400.bin, cd0/ (%zu files), manifest.txt\n", g.variant_name.c_str(), assets_dir.c_str(), g.files.size());
    return 0;
}

int import_cdtv_cli(const std::string& path, const std::string& assets_dir, const std::string& audio_dir) {
    CdtvGame g = import_cdtv_game(path);
    for (const Message& m : g.messages) std::printf("%s\n", format_message(m).c_str());
    if (g.status != Status::Ok) {
        std::printf("import failed (%s): nothing written\n", status_name(g.status));
        return int(g.status);
    }
    std::string err;
    if (!write_cdtv_assets(assets_dir, g, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("imported (%s): %s/game_0400.bin, cd0/ (%zu files), manifest.txt\n", g.variant_name.c_str(), assets_dir.c_str(), g.files.size());
    if (!g.has_cue) {
        std::printf("no CD-audio tracks in this input (a bare data track): the music is not imported; import the .cue (or its ZIP) for it, "
                    "or later with --import-cdtv\n");
        return 0;
    }
    std::printf("CD audio:\n");
    const int rc = cdtv::import_cli(path, audio_dir);
    if (rc == int(cdtv::Status::Unsupported)) {
        std::printf("the input has no complete set of CD-audio tracks: only the game assets were imported\n");
        return 0;
    }
    if (rc != 0) std::printf("the game assets were imported, but the CD audio was not (exit code %d)\n", rc);
    return rc;
}

bool diagnose_cdtv(const std::string& path, std::vector<std::string>& lines) {
    lines.clear();
    if (!looks_like_cdtv_game_image(path)) return false;
    lines.push_back(path + ":");
    CdtvGame g = import_cdtv_game(path);
    for (const Message& m : g.messages) lines.push_back("  " + format_message(m));
    if (g.status == Status::Ok) {
        lines.push_back("  game files: " + std::string([&] {
            std::string s;
            for (const CdtvFile& f : g.files) s += (s.empty() ? "" : " ") + f.name;
            return s;
        }()));
        lines.push_back("  would import as " + g.variant_name + " (build=cdtv, chip_kb=1024), game_0400.bin sha1 " + g.image_sha1);
    } else {
        lines.push_back("  not importable (" + std::string(status_name(g.status)) + ")");
    }
    return true;
}

} // namespace x2::import
