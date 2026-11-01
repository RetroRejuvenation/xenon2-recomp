// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "audio/audio_dump.h"

#include "pack/pack_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace jl = x2::pack;

namespace x2::audio {

namespace {
constexpr int kMinFileRate = 44100;  // see file_rate()

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (uint8_t(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", unsigned(uint8_t(c))); o += b; }
        else o += c;
    }
    return o + "\"";
}

void write_json(const jl::Json& j, std::string& o) {
    switch (j.t) {
    case jl::Json::Null: o += "null"; break;
    case jl::Json::Bool: o += j.b ? "true" : "false"; break;
    case jl::Json::Num: {
        char b[40];
        if (j.n == double(int64_t(j.n)) && j.n > -1e15 && j.n < 1e15) std::snprintf(b, sizeof b, "%lld", (long long)int64_t(j.n));
        else std::snprintf(b, sizeof b, "%.9g", j.n);
        o += b;
        break;
    }
    case jl::Json::Str: o += json_str(j.s); break;
    case jl::Json::Arr:
        o += '[';
        for (size_t i = 0; i < j.a.size(); ++i) { if (i) o += ", "; write_json(j.a[i], o); }
        o += ']';
        break;
    case jl::Json::Obj:
        o += '{';
        for (size_t i = 0; i < j.o.size(); ++i) {
            if (i) o += ", ";
            o += json_str(j.o[i].first) + ": ";
            write_json(j.o[i].second, o);
        }
        o += '}';
        break;
    }
}

// ---- finding a top-level member of a JSON object in its text (so the rest of a manifest is left byte for byte) ----

size_t skip_ws(const std::string& t, size_t p) {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
    return p;
}
size_t skip_string(const std::string& t, size_t p) {  // p at the opening quote; returns past the closing one
    for (++p; p < t.size() && t[p] != '"'; ++p)
        if (t[p] == '\\') ++p;
    return std::min(t.size(), p + 1);
}
size_t skip_value(const std::string& t, size_t p) {
    p = skip_ws(t, p);
    if (p >= t.size()) return p;
    if (t[p] == '"') return skip_string(t, p);
    if (t[p] == '{' || t[p] == '[') {
        int depth = 0;
        for (; p < t.size(); ++p) {
            if (t[p] == '"') { p = skip_string(t, p) - 1; continue; }
            if (t[p] == '{' || t[p] == '[') ++depth;
            else if ((t[p] == '}' || t[p] == ']') && --depth == 0) return p + 1;
        }
        return p;
    }
    while (p < t.size() && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != ' ' && t[p] != '\n' && t[p] != '\r' && t[p] != '\t') ++p;
    return p;
}
struct Span {
    size_t key_begin = 0, value_begin = 0, value_end = 0;
    bool found = false;
};
// The member `key` of the top-level object whose text is t (t is known to parse as an object).
Span find_member(const std::string& t, const std::string& key) {
    Span r;
    size_t p = skip_ws(t, 0);
    if (p >= t.size() || t[p] != '{') return r;
    ++p;
    for (;;) {
        p = skip_ws(t, p);
        if (p >= t.size() || t[p] != '"') return r;
        const size_t kb = p;
        const size_t ke = skip_string(t, p);
        const std::string k = t.substr(kb + 1, ke - kb - 2);
        p = skip_ws(t, ke);
        if (p >= t.size() || t[p] != ':') return r;
        const size_t vb = skip_ws(t, p + 1);
        const size_t ve = skip_value(t, vb);
        if (k == key) { r = {kb, vb, ve, true}; return r; }  // (keys with escapes never match: ours have none)
        p = skip_ws(t, ve);
        if (p < t.size() && t[p] == ',') ++p;
        else return r;
    }
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)); put16(v, uint16_t(x >> 16)); }
} // namespace

std::string AudioDumper::existing_audio_member(const std::string& dir) {
    std::string text;
    if (!read_text(dir + "/manifest.json", text)) return std::string();
    jl::Json root;
    std::string perr;
    if (!jl::parse_json(text, root, perr) || root.t != jl::Json::Obj) return std::string();
    const Span sp = find_member(text, "audio");
    if (!sp.found) return std::string();
    return "\"audio\": " + text.substr(sp.value_begin, sp.value_end - sp.value_begin);
}

int AudioDumper::file_rate(int ref_period, int* repeat) {
    // The file's rate is the colour clock over the period, rounded, times a whole number of repeats of every sample. Repeating holds each
    // 8-bit sample for its true duration (Paula's own zero-order hold), so the file sounds when resampled to the output rate like the original
    // does, instead of being smoothed; and the repeats shrink the rounding error of the rate, which over a looping sound that runs for many
    // seconds would drift the file against the original. At least 44.1 kHz (every sound is then a rate every decoder takes), at most 192 kHz.
    const int p = std::clamp(ref_period, 124, 65535);
    const double ideal = 3546895.0 / p;
    const int base = ideal >= kMinFileRate ? 1 : int(std::ceil(kMinFileRate / ideal));
    int best_rep = base, best_rate = 0;
    double best_err = 1e9;
    for (int rep = base; rep < base + 16 && ideal * rep <= 192000.0; ++rep) {
        const int rate = int(ideal * rep + 0.5);
        const double err = std::fabs(double(rate) - ideal * rep) / (ideal * rep);
        if (err < best_err) {
            best_err = err;
            best_rep = rep;
            best_rate = rate;
        }
        if (err < 1e-5) break;
    }
    if (repeat) *repeat = best_rep;
    return best_rate;
}

std::vector<uint8_t> AudioDumper::wav_bytes(const uint8_t* sample, uint32_t bytes, int ref_period) {
    int rep = 1;
    const int rate = file_rate(ref_period, &rep);
    const uint32_t data = bytes * uint32_t(rep) * 2u;
    std::vector<uint8_t> w;
    w.reserve(44 + data);
    const char* riff = "RIFF";
    w.insert(w.end(), riff, riff + 4);
    put32(w, 36 + data);
    const char* fmt = "WAVEfmt ";
    w.insert(w.end(), fmt, fmt + 8);
    put32(w, 16);
    put16(w, 1);  // PCM
    put16(w, 1);  // mono
    put32(w, uint32_t(rate));
    put32(w, uint32_t(rate) * 2);
    put16(w, 2);
    put16(w, 16);
    const char* d = "data";
    w.insert(w.end(), d, d + 4);
    put32(w, data);
    for (uint32_t i = 0; i < bytes; ++i) {
        const int16_t s = int16_t(int(int8_t(sample[i])) * 256);
        for (int r = 0; r < rep; ++r) put16(w, uint16_t(s));
    }
    return w;
}

bool AudioDumper::open(const std::string& dir, std::string& why) {
    dir_ = dir;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec || !fs::is_directory(dir_, ec)) {
        why = "cannot create " + dir_;
        return false;
    }
    return true;
}

size_t AudioDumper::count(bool speech) const {
    size_t n = 0;
    for (const auto& e : entries_)
        if (e.second.speech == speech) ++n;
    return n;
}

void AudioDumper::on_sample(const SampleStart& s) {
    const int ch = s.channel & 3;
    bool any = false;
    for (uint32_t i = 0; i < s.size && !any; ++i) any = s.bytes[i] != 0;
    if (!any || s.size < kMinBytes) {  // silence and the game's parking loops are not sounds
        pending_[ch].set = false;
        current_key_[ch].clear();
        return;
    }
    auto it = entries_.find(s.key);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxEntries || bytes_ + s.size > kMaxBytes) {
            ++skipped_;
            current_key_[ch].clear();
            return;
        }
        Entry e;
        e.key = s.key;
        e.data.assign(s.bytes, s.bytes + s.size);
        e.first_frame = s.frame;
        e.speech = s.speech;
        e.cd0_file = s.cd0_file;
        for (uint32_t i = 0; i < s.size; ++i) e.peak = std::max(e.peak, std::abs(int(int8_t(s.bytes[i]))));
        bytes_ += s.size;
        it = entries_.emplace(s.key, std::move(e)).first;
    }
    Entry& e = it->second;
    ++e.uses;
    e.channels |= 1u << ch;
    if (e.cd0_file.empty()) e.cd0_file = s.cd0_file;
    const uint16_t per = s.period ? s.period : uint16_t(65535);
    ++e.start_periods[per];
    e.pmin = std::min(e.pmin, per);
    e.pmax = std::max(e.pmax, per);
    pending_[ch].set = false;
    current_key_[ch] = s.key;
}

void AudioDumper::flush_pending(int ch) {
    Pending& p = pending_[ch];
    if (!p.set) return;
    p.set = false;
    if (current_key_[ch].empty()) return;
    auto it = entries_.find(current_key_[ch]);
    if (it == entries_.end()) return;
    it->second.pmin = std::min(it->second.pmin, p.period);
    it->second.pmax = std::max(it->second.pmax, p.period);
}

void AudioDumper::on_period(int channel, uint16_t period, uint64_t frame) {
    const int ch = channel & 3;
    flush_pending(ch);
    if (current_key_[ch].empty()) return;
    pending_[ch] = {true, period ? period : uint16_t(65535), frame};
}

void AudioDumper::on_loop(int channel) {
    const int ch = channel & 3;
    if (current_key_[ch].empty()) return;
    auto it = entries_.find(current_key_[ch]);
    if (it != entries_.end()) it->second.loops = true;
}

void AudioDumper::on_channel_end(int channel, uint64_t frame) {
    const int ch = channel & 3;
    if (pending_[ch].set && frame > pending_[ch].frame + uint64_t(kParkMs) * uint64_t(rate_) / 1000) flush_pending(ch);
    pending_[ch].set = false;
    current_key_[ch].clear();
}

void AudioDumper::note_cue(uint64_t frame, const std::string& slot, const std::string& state, uint32_t caller) {
    if (cues_.size() < 100000) cues_.push_back({frame, slot, state, caller});
}

int AudioDumper::ref_period_of(const Entry& e) const {
    // The period most starts used; ties go to the lower period (the longer sound).
    uint16_t best = 0;
    uint32_t n = 0;
    for (const auto& kv : e.start_periods)
        if (kv.second > n) { best = kv.first; n = kv.second; }
    return std::clamp(int(best), 124, 65535);
}

bool AudioDumper::finish(std::string& why) {
    for (int c = 0; c < 4; ++c) flush_pending(c);
    const std::string manifest_path = dir_ + "/manifest.json";
    std::string text;
    const bool have = read_text(manifest_path, text);
    jl::Json root;
    if (have) {
        std::string perr;
        if (!jl::parse_json(text, root, perr) || root.t != jl::Json::Obj) {
            why = manifest_path + (perr.empty() ? " is not a JSON object" : ": " + perr);
            return false;
        }
    }

    // What the manifest already lists: those entries stay exactly as they are.
    std::set<std::string> listed_sfx, listed_speech;
    std::string old_audio_text;
    jl::Json old_audio;
    if (have) {
        const Span sp = find_member(text, "audio");
        if (sp.found) {
            old_audio_text = text.substr(sp.value_begin, sp.value_end - sp.value_begin);
            std::string perr;
            if (jl::parse_json(old_audio_text, old_audio, perr) && old_audio.t == jl::Json::Obj) {
                if (const jl::Json* s = old_audio.get("sfx"); s && s->t == jl::Json::Obj)
                    for (const auto& m : s->o) listed_sfx.insert(m.first);
                if (const jl::Json* s = old_audio.get("speech"); s && s->t == jl::Json::Obj)
                    for (const auto& m : s->o) listed_speech.insert(m.first);
            } else {
                old_audio = jl::Json{};
            }
        }
    }

    std::error_code ec;
    std::string new_sfx, new_speech;
    size_t written = 0;
    for (const auto& kv : entries_) {
        const Entry& e = kv.second;
        const std::set<std::string>& listed = e.speech ? listed_speech : listed_sfx;
        const std::string sub = e.speech ? "speech" : "sfx";
        const int ref = ref_period_of(e);
        const std::string rel = sub + "/" + e.key + ".wav";
        const std::string path = dir_ + "/" + rel;
        if (!fs::exists(path, ec)) {
            fs::create_directories(dir_ + "/" + sub, ec);
            const std::vector<uint8_t> w = wav_bytes(e.data.data(), uint32_t(e.data.size()), ref);
            std::ofstream out(path, std::ios::binary);
            if (!out || !out.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()))) {
                why = "cannot write " + path;
                return false;
            }
            ++written;
        }
        if (listed.count(e.key)) continue;
        char peak[32];
        std::snprintf(peak, sizeof peak, "%.3f", double(e.peak) / 128.0);
        std::string& dst = e.speech ? new_speech : new_sfx;
        if (!dst.empty()) dst += ",\n";
        dst += "      " + json_str(e.key) + ": {\"file\": " + json_str(rel) + ", \"ref_period\": " + std::to_string(ref) +
               ", \"bytes\": " + std::to_string(e.data.size()) + ", \"uses\": " + std::to_string(e.uses) +
               ", \"first_frame\": " + std::to_string(e.first_frame) + ", \"channels\": " + std::to_string(e.channels) +
               ", \"periods\": [" + std::to_string(e.pmin) + ", " + std::to_string(e.pmax) + "], \"peak\": " + peak +
               ", \"source\": " + (e.speech ? "\"speech\"" : "\"effect\"");
        if (!e.cd0_file.empty()) dst += ", \"cd0_file\": " + json_str(e.cd0_file);
        if (e.loops) dst += ", \"loop\": true";
        dst += "}";
    }

    // The audio section: the old members first (music and anything else the author added stay), then the new entries.
    auto section = [&](const char* name, const std::string& fresh) {
        std::string body;
        if (const jl::Json* s = old_audio.get(name); s && s->t == jl::Json::Obj)
            for (const auto& m : s->o) {
                if (!body.empty()) body += ",\n";
                body += "      " + json_str(m.first) + ": ";
                write_json(m.second, body);
            }
        if (!fresh.empty()) {
            if (!body.empty()) body += ",\n";
            body += fresh;
        }
        return body;
    };
    std::string audio = "{\n";
    bool first = true;
    auto add_member = [&](const std::string& name, const std::string& body) {
        if (body.empty()) return;
        if (!first) audio += ",\n";
        first = false;
        audio += "    " + json_str(name) + ": {\n" + body + "\n    }";
    };
    add_member("sfx", section("sfx", new_sfx));
    add_member("speech", section("speech", new_speech));
    for (const auto& m : old_audio.o) {
        if (m.first == "sfx" || m.first == "speech") continue;
        std::string v;
        write_json(m.second, v);
        if (!first) audio += ",\n";
        first = false;
        audio += "    " + json_str(m.first) + ": " + v;
    }
    audio += "\n  }";
    if (first) audio = "{}";

    std::string out;
    if (!have) {
        out = "{\n  \"format\": \"x2-asset-pack\",\n  \"version\": 2,\n  \"name\": \"dump\",\n  \"audio\": " + audio + "\n}\n";
    } else {
        out = text;
        // The member is spliced in last-first so the earlier offsets stay valid: the audio member, then the version.
        const Span au = find_member(out, "audio");
        if (au.found) {
            out.replace(au.value_begin, au.value_end - au.value_begin, audio);
        } else {
            size_t close = out.rfind('}');
            if (close == std::string::npos) { why = manifest_path + " has no closing brace"; return false; }
            size_t before = close;
            while (before > 0 && (out[before - 1] == ' ' || out[before - 1] == '\n' || out[before - 1] == '\r' || out[before - 1] == '\t')) --before;
            const bool empty = before > 0 && out[before - 1] == '{';
            out.replace(before, close - before, std::string(empty ? "" : ",") + "\n  \"audio\": " + audio + "\n");
        }
        const Span ver = find_member(out, "version");
        const jl::Json* v = root.get("version");
        if (ver.found && v && v->t == jl::Json::Num && v->n < 2) out.replace(ver.value_begin, ver.value_end - ver.value_begin, "2");
        else if (!ver.found) {
            const size_t open = out.find('{');
            out.insert(open + 1, "\n  \"version\": 2,");
        }
    }
    {
        std::ofstream mf(manifest_path, std::ios::binary);
        if (!mf || !mf.write(out.data(), std::streamsize(out.size()))) {
            why = "cannot write " + manifest_path;
            return false;
        }
    }
    if (!cues_.empty()) {
        std::ofstream cf(dir_ + "/music_cues.txt", std::ios::binary);
        if (!cf) { why = "cannot write " + dir_ + "/music_cues.txt"; return false; }
        cf << "# frame slot state caller\n";
        for (const Cue& c : cues_) {
            char caller[16];
            std::snprintf(caller, sizeof caller, "%06X", c.caller);
            cf << c.frame << ' ' << c.slot << ' ' << c.state << ' ' << caller << '\n';
        }
    }
    (void)written;
    return true;
}

} // namespace x2::audio
