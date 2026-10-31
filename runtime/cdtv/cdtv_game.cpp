// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cdtv/cdtv_game.h"

#include "boot.h"
#include "cdtv/cdtv_music.h"
#include "machine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace x2::cdtv {

namespace fs = std::filesystem;

// The ten file loaders (cdtv-c0.md 7.2: hunk offsets, + $400 here): the `jsr -30(a6)` (Open), `jsr -42(a6)` (Read) and
// `jsr -36(a6)` (Close) of each. Registers at the call: Open d1 = name; Read d1 = handle, d2 = buffer, d3 = length.
const uint32_t kOpenSites[kFileSites] = {0x3393C, 0x3D2B0, 0x3D87A, 0x3E0A6, 0x50FDA, 0x50F74, 0x3C7EA, 0x411B4, 0x4FD34, 0x4FD84};
const uint32_t kReadSites[kFileSites] = {0x33954, 0x3D2C8, 0x3D892, 0x3E0BE, 0x50FF2, 0x50F8C, 0x3C802, 0x411CC, 0x4FD4C, 0x4FD9C};
const uint32_t kCloseSites[kFileSites] = {0x3395A, 0x3D2CE, 0x3D898, 0x3E0C4, 0x50FF8, 0x50F92, 0x3C808, 0x411D2, 0x4FD52, 0x4FDA2};

// ---------------------------------------------------------------------------------------------------------
// The files

std::string DiscFiles::key_of(const std::string& name) {
    std::string n = name;
    const size_t colon = n.find(':');
    if (colon != std::string::npos) n.erase(0, colon + 1);
    while (!n.empty() && n.front() == '/') n.erase(0, 1);
    const size_t semi = n.find(';');
    if (semi != std::string::npos) n.erase(semi);
    for (char& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
    return n;
}

bool DiscFiles::index(const std::string& dir, std::string& why) {
    paths_.clear();
    open_.clear();
    unknown_.clear();
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec)) {
        why = dir + ": the disc's files are missing (import your CDTV disc again: x2recomp --import PATH)";
        return false;
    }
    for (auto it = fs::directory_iterator(fs::path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        paths_[key_of(it->path().filename().string())] = it->path().string();
    }
    if (paths_.empty()) {
        why = dir + ": no files (import your CDTV disc again: x2recomp --import PATH)";
        return false;
    }
    return true;
}

uint32_t DiscFiles::open(const std::string& name) {
    const std::string key = key_of(name);
    auto p = paths_.find(key);
    if (p == paths_.end()) {
        if (std::find(unknown_.begin(), unknown_.end(), key) == unknown_.end()) unknown_.push_back(key);
        return 0;
    }
    Open o;
    {
        std::ifstream in(fs::path(p->second), std::ios::binary);
        if (!in) return 0;
        o.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const uint32_t h = next_++;
    open_.emplace(h, std::move(o));
    return h;
}

int32_t DiscFiles::read(uint32_t handle, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    auto it = open_.find(handle);
    if (it == open_.end()) return -1;
    Open& o = it->second;
    const size_t avail = o.data.size() - std::min(o.pos, o.data.size());
    const size_t take = std::min(n, avail);
    out.assign(o.data.begin() + std::ptrdiff_t(o.pos), o.data.begin() + std::ptrdiff_t(o.pos + take));
    o.pos += take;
    return int32_t(take);
}

bool DiscFiles::close(uint32_t handle) { return open_.erase(handle) != 0; }

// ---------------------------------------------------------------------------------------------------------
// The traps

namespace {
std::string c_string(Machine& m, uint32_t addr, size_t max = 80) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const uint8_t c = m.read8((addr + uint32_t(i)) & 0xFFFFFF);
        if (!c) break;
        s += char(c);
    }
    return s;
}

bool word_at(const std::vector<uint8_t>& image, uint32_t addr, uint16_t expect) {
    const size_t at = addr - kGameBase;
    return at + 2 <= image.size() && image[at] == uint8_t(expect >> 8) && image[at + 1] == uint8_t(expect);
}
} // namespace

bool CdtvGame::install(Machine& m, const GameAssets& assets, CdtvMusic* music, std::string& why) {
    music_ = music;
    if (!is_cdtv_image(assets.game_image.data(), assets.game_image.size())) {
        why = "not the CDTV release's X2 program as the importer links it";
        return false;
    }
    if (!files_.index(assets.cd0_dir, why)) return false;
    // Each load site must hold the dos call it replaces: `jsr d16(a6)` (4EAE) with the LVO of Open (-30), Read (-42), Close (-36).
    for (uint32_t i = 0; i < kFileSites; ++i) {
        if (!word_at(assets.game_image, kOpenSites[i], 0x4EAE) || !word_at(assets.game_image, kOpenSites[i] + 2, 0xFFE2) ||
            !word_at(assets.game_image, kReadSites[i], 0x4EAE) || !word_at(assets.game_image, kReadSites[i] + 2, 0xFFD6) ||
            !word_at(assets.game_image, kCloseSites[i], 0x4EAE) || !word_at(assets.game_image, kCloseSites[i] + 2, 0xFFDC)) {
            why = "file loader " + std::to_string(i) + " does not hold the dos Open/Read/Close calls the traps replace";
            return false;
        }
    }
    // The CD routines each start with `movea.l $4.l,a6` (2C79 0000 0004) or, for the poll, `tst.l $3A8F2` (4AB9).
    for (uint32_t pc : {kCdStop, kCdPlay, kCdVolumeFull, kCdVolumeMute})
        if (!word_at(assets.game_image, pc, 0x2C79)) { why = "a CD-audio routine does not start where the traps expect it"; return false; }
    if (!word_at(assets.game_image, kCdTick, 0x4AB9)) { why = "the CD poll does not start where the trap expects it"; return false; }

    m.add_trap(kOsBlockStart, [this](Machine& mm) {
        ++stats_.os_skips;
        mm.set_pc(kOsBlockResume);
    });
    m.add_trap(kPalCheck, [](Machine& mm) { mm.set_pc(kPalResume); });
    for (uint32_t i = 0; i < kFileSites; ++i) {
        m.add_trap(kOpenSites[i], [this](Machine& mm) { on_open(mm); });
        m.add_trap(kReadSites[i], [this](Machine& mm) { on_read(mm); });
        m.add_trap(kCloseSites[i], [this](Machine& mm) { on_close(mm); });
    }
    m.add_trap(kCdStop, [this](Machine& mm) { cd_stop(mm); mm.do_rts(); });
    m.add_trap(kCdPlay, [this](Machine& mm) { cd_play(mm, int(mm.dreg(0) & 0xFFFF)); mm.do_rts(); });
    // The poll asks cdtv.device once a second whether the track has played out and starts it again if so. The CD player does
    // exactly that in emulated time (CdPlayer::tick), so the routine itself has nothing left to do.
    m.add_trap(kCdTick, [this](Machine& mm) { ++stats_.cd_ticks; mm.do_rts(); });
    m.add_trap(kCdVolumeFull, [this](Machine& mm) {
        ++stats_.cd_volume;
        if (music_) music_->set_game_volume(100);
        mm.do_rts();
    });
    m.add_trap(kCdVolumeMute, [this](Machine& mm) {
        ++stats_.cd_volume;
        if (music_) music_->set_game_volume(0);
        mm.do_rts();
    });
    return true;
}

void CdtvGame::on_open(Machine& m) {
    const std::string name = c_string(m, m.dreg(1));
    const uint32_t h = files_.open(name);
    ++stats_.opens;
    if (!h) {
        ++stats_.failed_opens;
        if (log_) std::fprintf(log_, "%u %d open %s FAILED\n", m.frame, m.line, name.c_str());
        // The program retries Open until it succeeds: with nothing to open it waits here, which is what a CD that is
        // not in the drive looks like. The front end reports it (GameStats::failed_opens) rather than hiding it.
        if (stats_.failed_opens == 1) std::fprintf(stderr, "x2recomp: the CDTV game asked for '%s', which the import does not have\n", name.c_str());
    } else if (log_) {
        std::fprintf(log_, "%u %d open %s %u\n", m.frame, m.line, name.c_str(), h);
    }
    handle_of_last_open_ = h;
    m.set_dreg(0, h);
    m.set_pc(m.pc() + 4); // the `jsr -30(a6)` is not executed
}

void CdtvGame::on_read(Machine& m) {
    const uint32_t handle = m.dreg(1), dest = m.dreg(2) & 0xFFFFFF, len = m.dreg(3);
    std::vector<uint8_t> data;
    int32_t got = files_.read(handle, len, data);
    if (got > 0) {
        if (dest + uint32_t(got) > m.chip_size) {
            std::fprintf(stderr, "x2recomp: CDTV file read of %d bytes to $%06X leaves chip RAM; truncated\n", got, dest);
            got = int32_t(dest < m.chip_size ? m.chip_size - dest : 0);
            data.resize(size_t(got));
        }
        if (got > 0) m.load(dest, data.data(), size_t(got));
    }
    ++stats_.reads;
    if (got > 0) stats_.bytes += uint64_t(got);
    if (log_) std::fprintf(log_, "%u %d read %u -> $%06X %d/%u\n", m.frame, m.line, handle, dest, got, len);
    m.set_dreg(0, uint32_t(got));
    m.set_pc(m.pc() + 4);
}

void CdtvGame::on_close(Machine& m) {
    files_.close(m.dreg(1));
    ++stats_.closes;
    m.set_pc(m.pc() + 4);
}

void CdtvGame::cd_stop(Machine& m) {
    if (int32_t(m.read32(kCdTrackVar)) < 0) return; // nothing playing: the routine does nothing
    ++stats_.cd_stop;
    stats_.last_track = -1;
    if (music_) music_->cue_stop(m);
    m.write32(kCdTrackVar, 0xFFFFFFFFu);
}

void CdtvGame::cd_play(Machine& m, int track) {
    cd_stop(m); // the routine stops the current track first
    ++stats_.cd_play;
    stats_.last_track = track;
    m.write32(kCdTrackVar, uint32_t(track));
    if (music_) music_->cue_play(m, track);
}

} // namespace x2::cdtv
