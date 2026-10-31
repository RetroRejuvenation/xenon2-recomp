// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cdtv/cdtv_disc.h"

#include "cdtv/inflate_stream.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace x2::cdtv {

namespace {

namespace fs = std::filesystem;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && lower(s.substr(s.size() - tail.size())) == tail;
}
// The last path component of a name stored in a CUE or ZIP ('/' or '\\' separated).
std::string base_name(const std::string& s) {
    size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? s : s.substr(p + 1);
}
std::string hex32(uint32_t v) {
    char b[9];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

// ---------------------------------------------------------------------------------------------------------
// Containers: a ZIP read through its central directory (members streamed and inflated), or a folder.

struct Member {
    std::string name;
    uint16_t method = 0, flags = 0;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, local = 0;
};

class Container {
public:
    // A ZIP, a folder holding one .cue, or a .cue file.
    bool open(const std::string& path, std::string& why, Status& st) {
        std::error_code ec;
        st = Status::BadInput;
        if (fs::is_directory(path, ec)) {
            dir_ = path;
            std::vector<std::string> cues;
            for (const auto& e : fs::directory_iterator(path, ec))
                if (e.is_regular_file(ec) && ends_with(e.path().string(), ".cue")) cues.push_back(e.path().filename().string());
            if (cues.empty()) { why = path + ": no .cue file in this folder"; st = Status::Unsupported; return false; }
            if (cues.size() > 1) { why = path + ": more than one .cue file in this folder"; return false; }
            cue_name_ = cues[0];
            return read_small(cue_name_, cue_text, why);
        }
        if (!fs::is_regular_file(path, ec)) { why = path + ": not found"; return false; }
        if (ends_with(path, ".cue")) {
            dir_ = fs::path(path).parent_path().string();
            if (dir_.empty()) dir_ = ".";
            cue_name_ = fs::path(path).filename().string();
            return read_small(cue_name_, cue_text, why);
        }
        zip_ = path;
        if (!read_zip_directory(why, st)) return false;
        std::vector<const Member*> cues;
        for (const Member& m : members_)
            if (ends_with(m.name, ".cue")) cues.push_back(&m);
        if (cues.empty()) { why = path + ": no .cue file in this archive (not a CD image)"; st = Status::Unsupported; return false; }
        if (cues.size() > 1) { why = path + ": more than one .cue file in this archive"; return false; }
        cue_name_ = cues[0]->name;
        return read_small(cue_name_, cue_text, why);
    }

    bool is_zip() const { return !zip_.empty(); }
    const std::string& cue_name() const { return cue_name_; }
    std::string where() const {
        std::error_code ec;
        fs::path p = is_zip() ? fs::path(zip_) : fs::path(dir_) / cue_name_;
        fs::path a = fs::absolute(p, ec);
        return (ec ? p : a).lexically_normal().string();
    }

    // The file a CUE FILE line names: the ZIP member or the file next to the .cue with that base name.
    bool find(const std::string& cue_file, std::string& key, uint64_t& size) const {
        std::string want = lower(base_name(cue_file));
        if (is_zip()) {
            for (const Member& m : members_)
                if (lower(base_name(m.name)) == want) { key = m.name; size = m.usize; return true; }
            return false;
        }
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_, ec)) {
            if (lower(e.path().filename().string()) != want || !e.is_regular_file(ec)) continue;
            key = e.path().string();
            size = e.file_size(ec);
            return !ec;
        }
        return false;
    }

    // Streams a file to the sink. ZIP members are inflated and their CRC-32 checked if read to the end.
    // `stopped` is set when the sink ended the stream early.
    bool stream(const std::string& key, const InflateSink& sink, std::string& why, bool& stopped, uint64_t limit) {
        stopped = false;
        if (!is_zip()) {
            std::ifstream in(key, std::ios::binary);
            if (!in) { why = "cannot read " + key; return false; }
            std::vector<uint8_t> buf(1u << 16);
            uint64_t total = 0;
            while (in) {
                in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
                size_t n = size_t(in.gcount());
                if (!n) break;
                if ((total += n) > limit) { why = key + ": larger than any CD track"; return false; }
                if (!sink(buf.data(), n)) { stopped = true; return true; }
            }
            if (in.bad()) { why = "read error in " + key; return false; }
            return true;
        }
        const Member* m = nullptr;
        for (const Member& e : members_)
            if (e.name == key) m = &e;
        if (!m) { why = key + ": not in the archive"; return false; }
        if (m->flags & 1) { why = m->name + ": encrypted ZIP members are not supported"; return false; }
        if (m->method != 0 && m->method != 8) { why = m->name + ": ZIP compression method " + std::to_string(m->method) + " is not supported"; return false; }
        if (m->usize > limit) { why = m->name + ": larger than any CD track"; return false; }
        std::ifstream in(zip_, std::ios::binary);
        uint8_t lh[30];
        in.seekg(std::streamoff(m->local));
        if (!in.read(reinterpret_cast<char*>(lh), 30) || le32(lh) != 0x04034b50) { why = m->name + ": bad ZIP local header"; return false; }
        in.seekg(std::streamoff(m->local + 30 + le16(lh + 26) + le16(lh + 28)));
        uint64_t left = m->csize, out = 0;
        uint32_t crc = 0;
        bool too_big = false;
        auto source = [&](uint8_t* buf, size_t cap) -> size_t {
            size_t n = size_t(std::min<uint64_t>(cap, left));
            if (!n || !in.read(reinterpret_cast<char*>(buf), std::streamsize(n))) return 0;
            left -= n;
            return n;
        };
        auto counted = [&](const uint8_t* d, size_t n) {
            if ((out += n) > m->usize) { too_big = true; return false; }
            crc = crc32_update(crc, d, n);
            if (!sink(d, n)) { stopped = true; return false; }
            return true;
        };
        if (m->method == 0) {
            std::vector<uint8_t> buf(1u << 16);
            while (size_t n = source(buf.data(), buf.size()))
                if (!counted(buf.data(), n)) break;
        } else {
            InflateResult r = inflate_stream(source, counted);
            if (r != InflateResult::Done && r != InflateResult::Stopped) { why = m->name + ": " + inflate_result_name(r); return false; }
        }
        if (too_big) { why = m->name + ": more data than the ZIP directory says (corrupted archive)"; return false; }
        if (stopped) return true;
        if (out != m->usize) { why = m->name + ": truncated (" + std::to_string(out) + " of " + std::to_string(m->usize) + " bytes)"; return false; }
        if (crc != m->crc) { why = m->name + ": CRC-32 mismatch (corrupted archive)"; return false; }
        return true;
    }

    std::string cue_text;

private:
    bool read_small(const std::string& name, std::string& text, std::string& why) {
        std::string key = is_zip() ? name : (fs::path(dir_) / name).string();
        bool stopped = false;
        text.clear();
        return stream(key, [&](const uint8_t* d, size_t n) { text.append(reinterpret_cast<const char*>(d), n); return text.size() < (1u << 20); },
                      why, stopped, 1u << 20) && !stopped;
    }

    bool read_zip_directory(std::string& why, Status& st) {
        std::ifstream in(zip_, std::ios::binary | std::ios::ate);
        if (!in) { why = "cannot read " + zip_; return false; }
        const uint64_t size = uint64_t(in.tellg());
        const uint64_t tail = std::min<uint64_t>(size, 65535 + 22);
        std::vector<uint8_t> t(tail);
        in.seekg(std::streamoff(size - tail));
        if (!in.read(reinterpret_cast<char*>(t.data()), std::streamsize(tail))) { why = "cannot read " + zip_; return false; }
        size_t eocd = SIZE_MAX;
        for (size_t i = tail >= 22 ? tail - 22 + 1 : 0; i-- > 0;)
            if (le32(&t[i]) == 0x06054b50) { eocd = i; break; }
        if (eocd == SIZE_MAX) { why = zip_ + ": not a ZIP archive"; st = Status::Unsupported; return false; }
        const uint8_t* e = &t[eocd];
        uint32_t cd_size = le32(e + 12), cd_off = le32(e + 16);
        uint16_t count = le16(e + 10);
        if (cd_off == 0xFFFFFFFFu || count == 0xFFFF) { why = zip_ + ": ZIP64 archives are not supported"; st = Status::Unsupported; return false; }
        if (uint64_t(cd_off) + cd_size > size || cd_size > (16u << 20)) { why = zip_ + ": corrupted ZIP directory"; return false; }
        std::vector<uint8_t> cd(cd_size);
        in.seekg(std::streamoff(cd_off));
        if (!in.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cd_size))) { why = zip_ + ": truncated ZIP directory"; return false; }
        for (size_t p = 0, i = 0; i < count; ++i) {
            if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) { why = zip_ + ": corrupted ZIP directory"; return false; }
            Member m;
            m.flags = le16(&cd[p + 8]);
            m.method = le16(&cd[p + 10]);
            m.crc = le32(&cd[p + 16]);
            m.csize = le32(&cd[p + 20]);
            m.usize = le32(&cd[p + 24]);
            m.local = le32(&cd[p + 42]);
            size_t nlen = le16(&cd[p + 28]), extra = le16(&cd[p + 30]), comment = le16(&cd[p + 32]);
            if (p + 46 + nlen > cd.size()) { why = zip_ + ": corrupted ZIP directory"; return false; }
            m.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
            if (m.csize == 0xFFFFFFFFu || m.usize == 0xFFFFFFFFu || m.local == 0xFFFFFFFFu) {
                why = zip_ + ": ZIP64 members are not supported";
                st = Status::Unsupported;
                return false;
            }
            if (m.local + m.csize > size) { why = zip_ + ": member " + m.name + " lies outside the archive (truncated?)"; return false; }
            if (!m.name.empty() && m.name.back() != '/') members_.push_back(m);
            p += 46 + nlen + extra + comment;
        }
        return true;
    }

    std::string zip_, dir_, cue_name_;
    std::vector<Member> members_;
};

// ---------------------------------------------------------------------------------------------------------
// Disc analysis shared by the import and in-place use.

struct Plan {
    std::vector<CueTrack> cue;
    struct Audio {
        CueTrack t;
        std::string key;
        uint64_t size = 0;
    };
    std::vector<Audio> audio;
    std::string volume;
};

bool plan_disc(Container& c, Plan& plan, std::string& why, Status& st) {
    st = Status::BadInput;
    if (!parse_cue(c.cue_text, plan.cue, why)) { why = c.cue_name() + ": " + why; return false; }
    // Data track first.
    const CueTrack* data = nullptr;
    for (const CueTrack& t : plan.cue)
        if (!t.audio) { data = &t; break; }
    if (!data || data->number != 1 || data->mode != "MODE1/2352") {
        why = "track 1 is not MODE1/2352 data: not the Xenon 2 CDTV disc";
        st = Status::Unsupported;
        return false;
    }
    std::string key;
    uint64_t size = 0;
    if (!c.find(data->file, key, size)) { why = "data track file missing: " + data->file; return false; }
    std::vector<uint8_t> head;
    bool stopped = false;
    const size_t want = size_t(kSectorBytes) * 17;
    if (!c.stream(key, [&](const uint8_t* d, size_t n) { head.insert(head.end(), d, d + n); return head.size() < want; },
                  why, stopped, kMaxTrackBytes))
        return false;
    if (!identify_data_track(head.data(), head.size(), plan.volume, why)) { st = Status::Unsupported; return false; }
    // Twelve audio tracks 2..13.
    uint64_t total = 0;
    for (const CueTrack& t : plan.cue) {
        if (!t.audio) continue;
        Plan::Audio a{t, "", 0};
        if (!c.find(t.file, a.key, a.size)) { why = "track " + std::to_string(t.number) + " file missing: " + t.file; return false; }
        if (a.size > kMaxTrackBytes) { why = "track " + std::to_string(t.number) + ": larger than any CD track"; return false; }
        if ((total += a.size) > kMaxDiscBytes) { why = "the audio tracks are larger than a CD"; return false; }
        plan.audio.push_back(a);
    }
    bool numbers_ok = plan.audio.size() == size_t(kLastAudioTrack - kFirstAudioTrack + 1);
    for (size_t i = 0; numbers_ok && i < plan.audio.size(); ++i) numbers_ok = plan.audio[i].t.number == kFirstAudioTrack + int(i);
    if (!numbers_ok) {
        why = "expected audio tracks 2-13, the CUE lists " + std::to_string(plan.audio.size()) + ": not the Xenon 2 CDTV disc";
        st = Status::Unsupported;
        return false;
    }
    return true;
}

std::string pcm_name(int track) {
    char b[32];
    std::snprintf(b, sizeof b, "track%02d.pcm", track);
    return b;
}

bool crc_file(const std::string& path, uint64_t bytes, uint32_t& crc, std::string& why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { why = "cannot read " + path; return false; }
    std::vector<uint8_t> buf(1u << 16);
    crc = 0;
    while (bytes) {
        size_t n = size_t(std::min<uint64_t>(bytes, buf.size()));
        if (!in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n))) { why = path + ": shorter than the manifest says"; return false; }
        crc = crc32_update(crc, buf.data(), n);
        bytes -= n;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------

bool parse_cue(const std::string& text, std::vector<CueTrack>& tracks, std::string& why) {
    tracks.clear();
    std::istringstream in(text);
    std::string line, file;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        word = lower(word);
        auto bad = [&](const std::string& what) {
            why = "line " + std::to_string(lineno) + ": " + what;
            return false;
        };
        if (word == "file") {
            size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) file = line.substr(q1 + 1, q2 - q1 - 1);
            else if (!(ls >> file)) return bad("FILE without a name");
            if (file.empty()) return bad("FILE without a name");
        } else if (word == "track") {
            CueTrack t;
            if (!(ls >> t.number >> t.mode) || t.number < 1 || t.number > 99) return bad("bad TRACK line");
            if (file.empty()) return bad("TRACK before any FILE");
            if (!tracks.empty() && t.number <= tracks.back().number) return bad("track numbers out of order");
            for (char& ch : t.mode) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            t.audio = t.mode == "AUDIO";
            t.file = file;
            tracks.push_back(t);
        } else if (word == "index") {
            int n = -1, mm = 0, ss = 0, ff = 0;
            std::string msf;
            if (tracks.empty() || !(ls >> n >> msf) || std::sscanf(msf.c_str(), "%d:%d:%d", &mm, &ss, &ff) != 3 || n < 0 ||
                n > 99 || mm < 0 || ss < 0 || ss > 59 || ff < 0 || ff > 74)
                return bad("bad INDEX line");
            int sectors = (mm * 60 + ss) * 75 + ff;
            if (n == 0) tracks.back().index0 = sectors;
            if (n == 1) tracks.back().index1 = sectors;
        }
    }
    if (tracks.empty()) { why = "no tracks"; return false; }
    for (const CueTrack& t : tracks)
        if (t.index1 < 0) { why = "track " + std::to_string(t.number) + " has no INDEX 01"; return false; }
    return true;
}

uint32_t find_programme_start(const uint8_t* data, size_t bytes, int index1) {
    const uint32_t first = uint32_t(std::max(1, index1));
    const uint32_t sectors = uint32_t(std::min<size_t>(bytes / kSectorBytes, kStartSearchSectors));
    for (uint32_t s = first; s < sectors; ++s) {
        const uint8_t* p = data + size_t(s) * kSectorBytes;
        for (uint32_t i = 0; i < kSectorBytes; ++i)
            if (p[i]) return s;
    }
    return uint32_t(std::max(0, index1));
}

bool identify_data_track(const uint8_t* data, size_t bytes, std::string& volume, std::string& why) {
    volume.clear();
    if (bytes < size_t(kSectorBytes) * 17) { why = "the data track is too short"; return false; }
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    const uint8_t* s = data + size_t(kSectorBytes) * 16;
    if (std::memcmp(s, sync, 12) != 0 || s[15] != 1) { why = "track 1 is not raw MODE1/2352 data"; return false; }
    const uint8_t* pvd = s + 16;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) { why = "track 1 holds no ISO 9660 volume"; return false; }
    volume.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    volume.erase(volume.find_last_not_of(' ') + 1);
    if (volume != "XENONII") { why = "the CD volume is '" + volume + "', not XENONII: not the Xenon 2 CDTV disc"; return false; }
    return true;
}

const TrackSource* Disc::track(int number) const {
    for (const TrackSource& t : tracks)
        if (t.number == number) return &t;
    return nullptr;
}

uint64_t Disc::total_frames() const {
    uint64_t n = 0;
    for (const TrackSource& t : tracks) n += t.frames;
    return n;
}

bool read_data_track(const std::string& path, std::vector<uint8_t>& raw, std::string& where, std::string& why, Status& st) {
    raw.clear();
    Container c;
    if (!c.open(path, why, st)) return false;
    where = c.where();
    std::vector<CueTrack> cue;
    if (!parse_cue(c.cue_text, cue, why)) { why = c.cue_name() + ": " + why; st = Status::BadInput; return false; }
    const CueTrack* data = nullptr;
    for (const CueTrack& t : cue)
        if (!t.audio) { data = &t; break; }
    if (!data || data->number != 1 || data->mode != "MODE1/2352") {
        why = "track 1 is not MODE1/2352 data: not the Xenon 2 CDTV disc";
        st = Status::Unsupported;
        return false;
    }
    std::string key;
    uint64_t size = 0;
    if (!c.find(data->file, key, size)) { why = "data track file missing: " + data->file; st = Status::BadInput; return false; }
    if (size > kMaxDataTrackBytes || size % kSectorBytes != 0 || size < uint64_t(kSectorBytes) * 17) {
        why = "data track " + data->file + " has an implausible size (" + std::to_string(size) + " bytes)";
        st = Status::BadInput;
        return false;
    }
    bool stopped = false;
    raw.reserve(size_t(size));
    if (!c.stream(key, [&](const uint8_t* d, size_t n) { raw.insert(raw.end(), d, d + n); return raw.size() <= kMaxDataTrackBytes; },
                  why, stopped, kMaxDataTrackBytes) || stopped) {
        if (why.empty()) why = "data track " + data->file + " is too large";
        st = Status::BadInput;
        raw.clear();
        return false;
    }
    std::string volume;
    if (!identify_data_track(raw.data(), raw.size(), volume, why)) { st = Status::Unsupported; raw.clear(); return false; }
    st = Status::Ok;
    return true;
}

bool looks_like_disc_image(const std::string& path) {
    std::error_code ec;
    if (ends_with(path, ".cue")) return fs::is_regular_file(path, ec);
    if (fs::is_directory(path, ec)) {
        for (const auto& e : fs::directory_iterator(path, ec))
            if (ends_with(e.path().string(), ".cue")) return true;
        return false;
    }
    if (!ends_with(path, ".zip")) return false;
    Container c;
    std::string why;
    Status st;
    return c.open(path, why, st);
}

ImportReport import_disc(const std::string& path, const std::string& out_dir, bool analyse_only) {
    ImportReport rep;
    auto error = [&](const std::string& msg, Status st) {
        rep.messages.push_back("error: " + msg);
        rep.status = st;
        return rep;
    };
    Container c;
    std::string why;
    Status st = Status::BadInput;
    if (!c.open(path, why, st)) return error(why, st);
    Plan plan;
    if (!plan_disc(c, plan, why, st)) return error(why, st);
    rep.messages.push_back("CDTV disc: volume " + plan.volume + " (" + c.where() + "), " + std::to_string(plan.audio.size()) +
                           " audio tracks");
    uint64_t total = 0;
    for (const Plan::Audio& a : plan.audio) total += a.size;
    std::error_code ec;
    if (!analyse_only) {
        fs::create_directories(out_dir, ec);
        if (ec) return error("cannot create " + out_dir + ": " + ec.message(), Status::WriteFailed);
        fs::space_info sp = fs::space(out_dir, ec);
        if (!ec && sp.available < total + (16u << 20))
            return error("not enough free space in " + out_dir + ": " + std::to_string(total >> 20) + " MB needed, " +
                             std::to_string(sp.available >> 20) + " MB available",
                         Status::WriteFailed);
        rep.messages.push_back("warning: the CD audio takes about " + std::to_string(total >> 20) + " MB in " + out_dir);
        fs::remove(fs::path(out_dir) / "manifest.txt", ec); // the old set is invalid from here on
    }
    std::ostringstream manifest;
    manifest << "x2recomp-cdtv " << kManifestVersion << "\nvolume " << plan.volume << "\nsource " << c.where()
             << "\nformat s16le stereo " << kSampleRate << "\n";
    for (const Plan::Audio& a : plan.audio) {
        const int n = a.t.number;
        const fs::path out = fs::path(out_dir) / pcm_name(n);
        fs::path tmp = out;
        tmp += ".tmp";
        std::ofstream f;
        if (!analyse_only) {
            f.open(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return error("cannot write " + tmp.string(), Status::WriteFailed);
        }
        // Hold the first kStartSearchSectors sectors to find the programme start, then stream the rest.
        std::vector<uint8_t> head;
        const size_t head_bytes = size_t(kSectorBytes) * kStartSearchSectors;
        uint32_t start = 0;
        bool started = false, write_ok = true;
        uint64_t written = 0;
        uint32_t crc = 0, head_crc = 0;
        auto emit = [&](const uint8_t* d, size_t len) {
            for (size_t i = 0; i < len;) { // CRC of the first kHeadCheckBytes on its own
                size_t k = written < kHeadCheckBytes ? std::min<size_t>(len - i, size_t(kHeadCheckBytes - written)) : len - i;
                if (written < kHeadCheckBytes) head_crc = crc32_update(head_crc, d + i, k);
                crc = crc32_update(crc, d + i, k);
                written += k;
                i += k;
            }
            if (f.is_open() && !f.write(reinterpret_cast<const char*>(d), std::streamsize(len))) write_ok = false;
            return write_ok;
        };
        auto begin = [&]() {
            started = true;
            start = find_programme_start(head.data(), head.size(), a.t.index1);
            if (size_t(start) * kSectorBytes < head.size())
                return emit(head.data() + size_t(start) * kSectorBytes, head.size() - size_t(start) * kSectorBytes);
            return true;
        };
        auto sink = [&](const uint8_t* d, size_t len) {
            if (started) return emit(d, len);
            size_t k = std::min(len, head_bytes - head.size());
            head.insert(head.end(), d, d + k);
            if (head.size() < head_bytes) return true;
            return begin() && (k == len || emit(d + k, len - k));
        };
        bool stopped = false;
        if (!c.stream(a.key, sink, why, stopped, kMaxTrackBytes)) {
            if (f.is_open()) { f.close(); fs::remove(tmp, ec); }
            return error("track " + std::to_string(n) + ": " + why, Status::BadInput);
        }
        if (!started && !begin()) write_ok = false;
        if (f.is_open()) {
            f.close();
            if (!write_ok || !f) { fs::remove(tmp, ec); return error("cannot write " + tmp.string(), Status::WriteFailed); }
        }
        if (stopped || !write_ok) return error("cannot write " + tmp.string(), Status::WriteFailed);
        if (a.size % kSectorBytes) rep.messages.push_back("warning: track " + std::to_string(n) + ": file size is not a whole number of sectors");
        const uint64_t frames = written / 4;
        if (frames < uint64_t(kSampleRate)) {
            if (!analyse_only) fs::remove(tmp, ec);
            return error("track " + std::to_string(n) + ": less than one second of audio (short or damaged track)", Status::BadInput);
        }
        if (!analyse_only) {
            fs::rename(tmp, out, ec);
            if (ec) { fs::remove(tmp, ec); return error("cannot rename " + tmp.string() + ": " + ec.message(), Status::WriteFailed); }
        }
        TrackSource ts;
        ts.number = n;
        ts.path = analyse_only ? a.key : out.string();
        ts.offset = analyse_only ? uint64_t(start) * kSectorBytes : 0;
        ts.frames = frames;
        ts.start_sector = start;
        ts.crc = crc;
        ts.head_crc = head_crc;
        rep.disc.tracks.push_back(ts);
        char line[160];
        std::snprintf(line, sizeof line, "track %d: programme from sector %u (CUE INDEX 01 at %d), %llu frames (%d:%05.2f)", n,
                      start, a.t.index1, (unsigned long long)frames, int(frames / kSampleRate / 60),
                      double(frames % (uint64_t(kSampleRate) * 60)) / kSampleRate);
        rep.messages.push_back(line);
        manifest << "track " << n << " file " << pcm_name(n) << " start " << start << " frames " << frames << " crc "
                 << hex32(crc) << " head " << hex32(head_crc) << "\n";
    }
    rep.disc.where = analyse_only ? c.where() : out_dir;
    rep.disc.imported = !analyse_only;
    if (!analyse_only) {
        const fs::path mp = fs::path(out_dir) / "manifest.txt";
        fs::path tmp = mp;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!(f << manifest.str()) || !f.flush()) return error("cannot write " + tmp.string(), Status::WriteFailed);
        }
        fs::rename(tmp, mp, ec);
        if (ec) return error("cannot rename " + tmp.string() + ": " + ec.message(), Status::WriteFailed);
        rep.messages.push_back("imported " + std::to_string(rep.disc.tracks.size()) + " CD audio tracks into " + out_dir);
    }
    rep.status = Status::Ok;
    return rep;
}

int import_cli(const std::string& path, const std::string& out_dir) {
    ImportReport r = import_disc(path, out_dir);
    for (const std::string& m : r.messages) std::fprintf(m.rfind("error", 0) == 0 ? stderr : stdout, "%s\n", m.c_str());
    std::fflush(stdout);
    return int(r.status);
}

bool open_imported(const std::string& dir, Disc& disc, std::string& why, bool full_check) {
    disc = Disc{};
    const fs::path mp = fs::path(dir) / "manifest.txt";
    std::ifstream in(mp);
    if (!in) { why = "no CD audio imported in " + dir; return false; }
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "x2recomp-cdtv") { why = mp.string() + ": not a CD audio manifest"; return false; }
    if (version != kManifestVersion) { why = mp.string() + ": made by another importer version; import the disc again"; return false; }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string w, file, k1, k2, k3, k4, k5;
        if (!(ls >> w) || w != "track") continue;
        TrackSource t;
        std::string crc, head;
        if (!(ls >> t.number >> k1 >> file >> k2 >> t.start_sector >> k3 >> t.frames >> k4 >> crc >> k5 >> head) || k1 != "file" ||
            k2 != "start" || k3 != "frames" || k4 != "crc" || k5 != "head" || file.find_first_of("/\\") != std::string::npos ||
            file == ".." || t.frames == 0) {
            why = mp.string() + ": bad line: " + line;
            return false;
        }
        t.crc = uint32_t(std::strtoul(crc.c_str(), nullptr, 16));
        t.head_crc = uint32_t(std::strtoul(head.c_str(), nullptr, 16));
        t.path = (fs::path(dir) / file).string();
        std::error_code ec;
        const uint64_t size = fs::file_size(t.path, ec);
        if (ec || size / 4 != t.frames) { why = t.path + ": missing or not the size the manifest records; import the disc again"; return false; }
        uint32_t c = 0;
        if (!crc_file(t.path, std::min<uint64_t>(size, kHeadCheckBytes), c, why)) return false;
        if (c != t.head_crc) { why = t.path + ": contents changed since the import; import the disc again"; return false; }
        if (full_check) {
            if (!crc_file(t.path, size, c, why)) return false;
            if (c != t.crc) { why = t.path + ": CRC-32 mismatch; import the disc again"; return false; }
        }
        disc.tracks.push_back(t);
    }
    if (disc.tracks.empty()) { why = mp.string() + ": no tracks"; return false; }
    disc.where = dir;
    disc.imported = true;
    return true;
}

bool open_in_place(const std::string& path, Disc& disc, std::string& why) {
    disc = Disc{};
    std::error_code ec;
    if (ends_with(path, ".zip")) {
        why = path + ": a ZIP cannot be played in place; import it with --import-cdtv (or point --cdtv at the extracted .cue)";
        return false;
    }
    Container c;
    Status st;
    if (!c.open(path, why, st)) return false;
    Plan plan;
    if (!plan_disc(c, plan, why, st)) return false;
    for (const Plan::Audio& a : plan.audio) {
        std::vector<uint8_t> head;
        bool stopped = false;
        const size_t want = size_t(kSectorBytes) * kStartSearchSectors;
        if (!c.stream(a.key, [&](const uint8_t* d, size_t n) { head.insert(head.end(), d, d + n); return head.size() < want; },
                      why, stopped, kMaxTrackBytes))
            return false;
        TrackSource t;
        t.number = a.t.number;
        t.path = a.key;
        t.start_sector = find_programme_start(head.data(), head.size(), a.t.index1);
        t.offset = uint64_t(t.start_sector) * kSectorBytes;
        t.frames = a.size > t.offset ? (a.size - t.offset) / 4 : 0;
        if (t.frames < uint64_t(kSampleRate)) { why = "track " + std::to_string(t.number) + ": less than one second of audio"; return false; }
        disc.tracks.push_back(t);
    }
    disc.where = c.where();
    return true;
}

bool open_disc(const std::string& path, Disc& disc, std::string& why) {
    std::error_code ec;
    if (fs::is_directory(path, ec) && fs::exists(fs::path(path) / "manifest.txt", ec)) return open_imported(path, disc, why);
    return open_in_place(path, disc, why);
}

} // namespace x2::cdtv
