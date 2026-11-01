// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_stack.h"

#include "pack/pack_png.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef X2_VERSION
#define X2_VERSION "unknown"
#endif

namespace x2::pack {

bool parse_graphic_key(const std::string& key, uint64_t& hash, int& w, int& h) {
    if (!valid_graphic_key(key)) return false;
    hash = std::strtoull(key.substr(0, 16).c_str(), nullptr, 16);
    const size_t x = key.find('x', 17);
    w = std::atoi(key.substr(17, x - 17).c_str());
    h = std::atoi(key.c_str() + x + 1);
    return true;
}

namespace {

std::string basename_of(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t at = p.find_last_of("/\\");
    std::string b = at == std::string::npos ? p : p.substr(at + 1);
    if (b.size() > 4) {
        std::string tail = b.substr(b.size() - 4);
        for (char& c : tail) c = char(std::tolower(uint8_t(c)));
        if (tail == ".zip") b.resize(b.size() - 4);
    }
    return b.empty() ? "pack" : b;
}

} // namespace

// ---- the loader of one pack ---------------------------------------------------------------------------------------------

class Loader {
public:
    Loader(PackStack& s, const std::string& path, PackReport& rep, bool discovered) : s_(s), path_(path), rep_(rep), discovered_(discovered) {}
    bool run();

private:
    struct PicResult {
        const Picture* pic = nullptr;
        std::string err;
    };

    void say(Severity sev, const std::string& section, const std::string& key, const std::string& text) { rep_.add(sev, label_, section, key, text); }
    void fail_entry(const DeclBase& d, const std::string& text) {
        say(Severity::Error, d.section, d.key, text);
        ++info_.skipped_entries;
    }
    bool applies(const GameScope& entry) const {
        if (s_.opt_.validate) return true;
        return GameScope::intersect(m_.scope, entry).applies(s_.ctx_);
    }
    bool applies2(const GameScope& a, const GameScope& b) const {
        if (s_.opt_.validate) return true;
        return GameScope::intersect(GameScope::intersect(m_.scope, a), b).applies(s_.ctx_);
    }
    void overridden(const std::string& section, const std::string& key) {
        const std::string k = section + "/" + key;
        auto it = s_.origins_.find(k);
        if (it != s_.origins_.end() && it->second != label_) {
            if (++overrides_ <= 20) say(Severity::Info, section, key, "overrides the entry of '" + it->second + "'");
        }
        s_.origins_[k] = label_;
    }

    const Picture* picture(const std::string& rel, bool background, std::string& err);
    const Picture* crop(const Picture& src, const int rect[4], std::string& err);
    bool add_picture(std::unique_ptr<Picture> p, std::string& err, const std::string& what);
    std::shared_ptr<const DecodedClip> clip_of(const std::string& file, AudioInfo& info, bool for_music, std::string& err);
    bool probe_music(const std::string& file, std::string& err);

    void graphics();
    void far();
    void palettes();
    void backgrounds();
    void atlases();
    void stars();
    void screens();
    void panel();
    void clips();
    void music();
    bool check_requirements(std::string& why);

    PackStack& s_;
    std::string path_;
    PackReport& rep_;
    bool discovered_;
    std::string label_;
    PackInfo info_;
    Manifest m_;
    PackStack::Record* rec_ = nullptr;
    size_t index_ = 0;
    size_t overrides_ = 0, n_graphics_ = 0, n_far_ = 0, n_palettes_ = 0, n_bg_ = 0, n_screens_ = 0, n_cells_ = 0, n_clips_ = 0, n_slots_ = 0;
    bool no_codec_said_ = false;
    std::map<std::string, PicResult> pic_cache_;
    std::map<std::string, std::pair<std::shared_ptr<const DecodedClip>, std::string>> clip_cache_;
    bool any_graphics_ = false;
    uint64_t decoded_ = 0;
};

bool Loader::add_picture(std::unique_ptr<Picture> p, std::string& err, const std::string& what) {
    const size_t bytes = p->px.size() * 4;
    if (s_.picture_bytes_ + bytes > s_.opt_.limits.max_total) {
        err = what + "over the memory limit for all packs (" + std::to_string(s_.opt_.limits.max_total >> 20) + " MB)";
        return false;
    }
    s_.picture_bytes_ += bytes;
    decoded_ += bytes;
    s_.pictures_.push_back(std::move(p));
    return true;
}

const Picture* Loader::picture(const std::string& rel, bool background, std::string& err) {
    const std::string cache_key = (background ? "b:" : "p:") + rel;
    auto it = pic_cache_.find(cache_key);
    if (it == pic_cache_.end()) {
        PicResult r;
        std::vector<uint8_t> bytes;
        auto pic = std::make_unique<Picture>();
        std::string e;
        const PackLimits& lim = s_.opt_.limits;
        if (!rec_->source->read(rel, lim.max_file, bytes, e)) {
        } else if (!decode_png(bytes.data(), bytes.size(), *pic, e, background ? lim.max_background_side : lim.max_side,
                               background ? lim.max_background_pixels : lim.max_pixels)) {
            e = "'" + rel + "': " + e;
        } else {
            const Picture* raw = pic.get();
            if (add_picture(std::move(pic), e, "'" + rel + "': ")) r.pic = raw;
        }
        r.err = e;
        it = pic_cache_.emplace(cache_key, std::move(r)).first;
        if (s_.opt_.validate) {
            if (it->second.pic) rec_->pictures[rel] = it->second.pic;
            else rec_->picture_errors[rel] = it->second.err;
        }
    }
    err = it->second.err;
    return it->second.pic;
}

const Picture* Loader::crop(const Picture& src, const int rect[4], std::string& err) {
    const long long x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    if (x + w > src.w || y + h > src.h) {
        err = "rect [" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(w) + ", " + std::to_string(h) + "] lies outside the " +
              std::to_string(src.w) + "x" + std::to_string(src.h) + " atlas";
        return nullptr;
    }
    auto p = std::make_unique<Picture>();
    p->w = int(w);
    p->h = int(h);
    p->px.resize(size_t(w) * size_t(h));
    for (long long r = 0; r < h; ++r) std::copy_n(&src.px[size_t(y + r) * size_t(src.w) + size_t(x)], size_t(w), &p->px[size_t(r) * size_t(w)]);
    const Picture* raw = p.get();
    if (!add_picture(std::move(p), err, "atlas frame ")) return nullptr;
    return raw;
}

// ---- audio ---------------------------------------------------------------------------------------------------------------

static AudioLimits audio_limits(const PackLimits& l, bool music) {
    AudioLimits a;
    a.max_file_bytes = l.max_audio_file;
    a.max_frames = uint64_t((music ? l.max_music_seconds : l.max_clip_seconds) * l.max_rate);
    a.max_decoded_bytes = l.max_clip_bytes;
    a.min_rate = l.min_rate;
    a.max_rate = l.max_rate;
    a.max_channels = l.max_channels;
    return a;
}

std::shared_ptr<const DecodedClip> Loader::clip_of(const std::string& file, AudioInfo& info, bool, std::string& err) {
    auto it = clip_cache_.find(file);
    if (it != clip_cache_.end()) { err = it->second.second; return it->second.first; }
    std::shared_ptr<const DecodedClip> out;
    std::string e;
    const AudioCodec* codec = audio_codec();
    const PackLimits& lim = s_.opt_.limits;
    auto src = rec_->source->open(file, lim.max_audio_file, e);
    if (src && codec && codec->probe) {
        AudioInfo probed;
        if (!codec->probe(*src, audio_limits(lim, false), probed, e)) {
            e = "'" + file + "': " + e;
        } else if (probed.frames >= 0 && probed.rate > 0 && double(probed.frames) / probed.rate > lim.max_clip_seconds) {
            e = "'" + file + "': longer than " + std::to_string(int(lim.max_clip_seconds)) + " s (sound effects and speech are short clips; use a music slot for long audio)";
        } else {
            info = probed;
            if (s_.opt_.decode_audio && codec->decode) {
                auto clip = std::make_shared<DecodedClip>();
                AudioInfo full = probed;
                if (!codec->decode(*src, audio_limits(lim, false), *clip, full, e)) e = "'" + file + "': " + e;
                else if (clip->pcm.size() * 2 > lim.max_clip_bytes) e = "'" + file + "': decodes to more than " + std::to_string(lim.max_clip_bytes) + " bytes";
                else if (s_.audio_bytes_ + clip->pcm.size() * 2 > lim.max_audio_total) e = "'" + file + "': over the memory limit for decoded sound effects and speech (" + std::to_string(lim.max_audio_total >> 20) + " MB)";
                else {
                    s_.audio_bytes_ += clip->pcm.size() * 2;
                    decoded_ += clip->pcm.size() * 2;
                    info = full;
                    out = clip;
                }
            }
            if (e.empty() && !out && !s_.opt_.decode_audio) out = std::make_shared<DecodedClip>();  // probe only: the clip's frames are not needed
        }
    } else if (!src) {
        // e says why
    } else {
        e = "no audio decoder in this build";
    }
    if (s_.opt_.validate && !e.size() && info.channels) rec_->audio[file] = info;
    clip_cache_.emplace(file, std::make_pair(out, e));
    err = e;
    return out;
}

bool Loader::probe_music(const std::string& file, std::string& err) {
    const PackLimits& lim = s_.opt_.limits;
    if (!rec_->source->exists(file)) {
        std::string why;
        std::vector<uint8_t> none;
        rec_->source->read(file, 0, none, why);
        err = why.empty() ? "missing file '" + file + "'" : why;
        return false;
    }
    const AudioCodec* codec = audio_codec();
    if (!codec || !codec->probe) return true;
    std::string e;
    auto src = rec_->source->open(file, lim.max_audio_file, e);
    if (!src) { err = e; return false; }
    AudioInfo info;
    if (!codec->probe(*src, audio_limits(lim, true), info, e)) { err = "'" + file + "': " + e; return false; }
    if (info.frames >= 0 && info.rate > 0 && double(info.frames) / info.rate > lim.max_music_seconds) {
        err = "'" + file + "': longer than " + std::to_string(int(lim.max_music_seconds / 3600)) + " h";
        return false;
    }
    if (s_.opt_.validate) rec_->audio[file] = info;
    return true;
}

// ---- sections ------------------------------------------------------------------------------------------------------------

void Loader::graphics() {
    for (const GraphicDecl& d : m_.graphics) {
        if (!applies(d.scope)) continue;
        std::string err;
        const Picture* p = picture(d.png, false, err);
        if (!p) { fail_entry(d, err); continue; }
        const Picture* fp = nullptr;
        if (!d.flash_png.empty() && !(fp = picture(d.flash_png, false, err))) { fail_entry(d, "\"flash_png\": " + err); continue; }
        for (const std::string& k : d.keys) {
            const bool flash = k.compare(0, 2, "f:") == 0;
            uint64_t hash = 0;
            int w = 0, h = 0;
            parse_graphic_key(flash ? k.substr(2) : k, hash, w, h);
            GraphicEntry e;
            e.pic = p;
            e.scale = d.scale;
            e.anchor = d.anchor;
            if (!flash) {
                e.flash = fp;
                e.flash_scale = d.scale;
                overridden("graphics", k);
                s_.graphics_[{hash, w, h}] = e;
            } else {
                overridden("graphics", k);
                s_.flashes_[{hash, w, h}] = e;
            }
        }
        ++n_graphics_;
        ++info_.entries;
        any_graphics_ = true;
    }
}

void Loader::far() {
    for (const FarDecl& d : m_.far) {
        if (!applies(d.scope)) continue;
        std::string err;
        const Picture* p = picture(d.png, false, err);
        if (!p) { fail_entry(d, err); continue; }
        if (p->w != kFarWidth * d.scale || p->h < int(d.lines.size()) * d.scale) {
            fail_entry(d, "the strip must be " + std::to_string(kFarWidth * d.scale) + " pixels wide and " + std::to_string(d.scale) + " rows per line (" +
                              std::to_string(d.lines.size()) + " lines)");
            continue;
        }
        for (size_t r = 0; r < d.lines.size(); ++r) {
            if (!d.used[r]) continue;
            s_.far_[d.lines[r]] = FarLineRef{p, d.scale, int(r)};
            ++n_far_;
        }
        ++info_.entries;
        any_graphics_ = true;
    }
}

void Loader::palettes() {
    for (const PaletteDecl& d : m_.palettes) {
        if (!applies(d.scope)) continue;
        s_.palettes_.push_back(d.po);
        ++n_palettes_;
        ++info_.entries;
        any_graphics_ = true;
    }
}

void Loader::backgrounds() {
    for (const BackgroundDecl& d : m_.backgrounds) {
        if (!applies(d.scope)) continue;
        std::string err;
        const Picture* p = picture(d.png, true, err);
        if (!p) { fail_entry(d, err); continue; }
        if (p->w != kFarWidth * d.scale) { fail_entry(d, "the picture must be " + std::to_string(kFarWidth * d.scale) + " pixels wide (320 x scale), it is " + std::to_string(p->w)); continue; }
        if (p->h % d.scale != 0 || (d.ring_lines && p->h != d.ring_lines * d.scale)) {
            fail_entry(d, d.ring_lines ? "the picture must be " + std::to_string(d.ring_lines * d.scale) + " pixels tall (ring_lines x scale), it is " + std::to_string(p->h)
                                       : "the picture height " + std::to_string(p->h) + " is not a multiple of its scale");
            continue;
        }
        overridden("backgrounds", d.key);
        s_.backgrounds_[d.level] = BackgroundEntry{d.level, p, d.scale, d.ring_lines};
        s_.has_background_[d.level] = true;
        ++n_bg_;
        ++info_.entries;
        any_graphics_ = true;
    }
}

void Loader::atlases() {
    for (const AtlasDecl& d : m_.atlases) {
        if (!applies(d.scope)) continue;
        std::string err;
        const Picture* atlas = picture(d.png, false, err);
        if (!atlas) { fail_entry(d, err); continue; }
        auto add = [&](const std::string& key, const int rect[4], Anchor anchor, const int* flash_rect) {
            DeclBase where = d;
            where.key = key;
            std::string e;
            const Picture* p = crop(*atlas, rect, e);
            if (!p) { fail_entry(where, e); return; }
            const Picture* fp = nullptr;
            if (flash_rect && !(fp = crop(*atlas, flash_rect, e))) { fail_entry(where, "\"flash_rect\": " + e); return; }
            uint64_t hash = 0;
            int w = 0, h = 0;
            parse_graphic_key(key, hash, w, h);
            GraphicEntry ge;
            ge.pic = p;
            ge.scale = d.scale;
            ge.anchor = anchor;
            ge.flash = fp;
            ge.flash_scale = d.scale;
            overridden("graphics", key);
            s_.graphics_[{hash, w, h}] = ge;
            ++n_graphics_;
            ++info_.entries;
            any_graphics_ = true;
        };
        for (const AtlasFrame& f : d.frames) add(f.key, f.rect, f.anchor, f.has_flash ? f.flash_rect : nullptr);
        if (d.has_grid) {
            const int columns = d.columns ? d.columns : std::max(1, atlas->w / d.cell_w);
            for (size_t i = 0; i < d.grid_keys.size(); ++i) {
                if (d.grid_keys[i].empty()) continue;
                const long long cx = (long long)(i % size_t(columns)) * d.cell_w, cy = (long long)(i / size_t(columns)) * d.cell_h;
                if (cx > (1 << 20) || cy > (1 << 20)) { DeclBase w2 = d; w2.key = d.grid_keys[i]; fail_entry(w2, "grid cell lies outside the atlas"); continue; }
                const int rect[4] = {int(cx), int(cy), d.cell_w, d.cell_h};
                add(d.grid_keys[i], rect, Anchor{}, nullptr);
            }
        }
    }
}

void Loader::stars() {
    if (!m_.stars || !applies(m_.stars->scope)) return;
    StarsEntry e;
    e.shape = m_.stars->shape;
    e.size_px = float(m_.stars->size);
    e.size = std::max(1, int(std::lround(m_.stars->size)));
    e.softness = float(m_.stars->softness);
    s_.stars_ = e;
    s_.has_stars_ = true;
    ++info_.entries;
    any_graphics_ = true;
}

void Loader::screens() {
    for (const ScreenDecl& d : m_.screens) {
        if (!applies(d.scope)) continue;
        std::string err;
        const Picture* p = picture(d.png, false, err);
        if (!p) { fail_entry(d, err); continue; }
        const Picture* wide = nullptr;
        if (!d.wide_png.empty() && !(wide = picture(d.wide_png, false, err))) { fail_entry(d, "\"wide_png\": " + err); continue; }
        auto e = std::make_unique<ScreenEntry>();
        e->name = d.key;
        e->signature = d.signature;
        e->pic = p;
        e->scale = d.scale;
        e->anchor = d.anchor;
        e->ref_palette = d.ref_palette;
        e->fade = d.fade;
        e->dynamic = d.dynamic;
        e->bg_index = d.bg_index;
        e->wide_pic = wide;
        e->wide_scale = wide ? (d.wide_scale ? d.wide_scale : m_.scale) : 0;
        overridden("screens", d.key);
        auto at = std::find(s_.screen_names_.begin(), s_.screen_names_.end(), d.key);
        if (at != s_.screen_names_.end()) s_.screens_[size_t(at - s_.screen_names_.begin())] = std::move(e);
        else {
            s_.screen_names_.push_back(d.key);
            s_.screens_.push_back(std::move(e));
        }
        ++n_screens_;
        ++info_.entries;
        any_graphics_ = true;
        info_.provides_screens = true;
    }
}

void Loader::panel() {
    if (!m_.panel || !applies(m_.panel->scope)) return;
    const PanelDecl& d = *m_.panel;
    PanelSet& ps = s_.panel_;
    ps.scale = d.scale;
    ps.cell_w = d.cell_w;
    ps.max_span = d.max_span;
    if (!d.ref_palette.empty()) ps.ref_palette = d.ref_palette;
    if (d.has_fade) ps.fade = d.fade;
    if (!d.background_png.empty()) {
        std::string err;
        if (const Picture* b = picture(d.background_png, false, err)) {
            ps.background = b;
            ps.background_anchor = d.background_anchor;
            ++info_.entries;
        } else {
            say(Severity::Error, "panel", "", "\"background\": " + err);
            ++info_.skipped_entries;
        }
    }
    for (const PanelCellDecl& c : d.cells) {
        if (!applies2(d.scope, c.scope)) continue;
        std::string err;
        const Picture* p = picture(c.png, false, err);
        if (!p) { fail_entry(c, err); continue; }
        for (const std::string& k : c.keys) {
            overridden("panel", k);
            ps.cells[k] = PanelCell{p, c.scale, c.anchor};
        }
        ++n_cells_;
        ++info_.entries;
    }
    s_.has_panel_ = !ps.empty();
    any_graphics_ = any_graphics_ || s_.has_panel_;
}

void Loader::clips() {
    for (const ClipDecl& d : m_.clips) {
        if (!applies(d.scope)) continue;
        if (!audio_codec()) {
            if (!no_codec_said_) {
                say(Severity::Info, d.section, "", "this build has no audio decoder yet: sound effect and speech replacements are not used");
                no_codec_said_ = true;
            }
            continue;
        }
        std::string err;
        AudioInfo info;
        auto clip = clip_of(d.file, info, false, err);
        if (!clip) { fail_entry(d, err); continue; }
        ClipEntry e;
        e.file = d.file;
        e.clip = clip;
        e.ref_period = d.ref_period;
        e.gain_db = d.gain_db;
        e.pan = d.pan;
        e.pan_pos = d.pan_pos;
        e.pitch = d.pitch;
        e.original = d.original;
        e.original_mix_db = d.original_mix_db;
        e.cut = d.cut;
        e.fade_out_ms = d.fade_out_ms;
        e.loop = d.loop;
        e.cd0_file = d.speech ? d.cd0_file : std::string();
        auto& table = d.speech ? s_.speech_ : s_.sfx_;
        for (const std::string& k : d.keys) {
            e.key = k;
            overridden(d.section, k);
            table[k] = e;
        }
        if (d.speech && !d.cd0_file.empty()) s_.speech_by_file_[d.cd0_file] = d.key;
        ++n_clips_;
        ++info_.entries;
        info_.provides_audio = true;
    }
}

void Loader::music() {
    if (m_.has_audio_section) {
        const MusicDefaultsDecl& md = m_.music_defaults;
        if (applies(GameScope())) {
            if (md.crossfade_ms) s_.defaults_.crossfade_ms = *md.crossfade_ms;
            if (md.fade_in_ms) s_.defaults_.fade_in_ms = *md.fade_in_ms;
            if (md.fade_out_ms) s_.defaults_.fade_out_ms = *md.fade_out_ms;
            if (md.loop_mode) s_.defaults_.loop_mode = *md.loop_mode;
            if (md.retrigger) s_.defaults_.retrigger = *md.retrigger;
        }
    }
    for (const MusicSlotDecl& d : m_.slots) {
        if (!applies(d.scope)) continue;
        MusicSlot slot = d.slot;
        std::string err;
        if (!slot.file.path.empty() && !probe_music(slot.file.path, err)) { fail_entry(d, "\"file\": " + err); continue; }
        if (!slot.intro.path.empty() && !probe_music(slot.intro.path, err)) { fail_entry(d, "\"intro\": " + err); continue; }
        if (slot.file.path.empty() && !slot.mute) { fail_entry(d, "needs a \"file\" or \"mute\": true"); continue; }
        if (!slot.file.path.empty()) slot.file.pack = int(index_);
        if (!slot.intro.path.empty()) slot.intro.pack = int(index_);
        slot.gain_db = d.gain_db ? *d.gain_db : m_.music_defaults.gain_db ? *m_.music_defaults.gain_db : 0.0f;
        overridden("audio.music.slots", d.key);
        s_.slots_[d.key] = std::move(slot);
        ++n_slots_;
        ++info_.entries;
        info_.provides_audio = true;
    }
}

bool Loader::check_requirements(std::string& why) {
    if (!m_.requires_x2recomp.empty()) {
        const std::string have = s_.opt_.runtime_version.empty() ? std::string(X2_VERSION) : s_.opt_.runtime_version;
        Semver v;
        if (parse_semver(have, v) || parse_semver(have + ".0", v)) {
            std::string h = have;
            if (std::count(h.begin(), h.end(), '.') < 2) h += ".0";
            if (!version_satisfies(h, m_.requires_x2recomp)) {
                why = "needs x2recomp " + m_.requires_x2recomp + " (this is " + have + ")";
                return false;
            }
        }
    }
    for (const Requirement& q : m_.requires_packs) {
        const PackInfo* found = nullptr;
        for (size_t i = 0; i < s_.infos_.size(); ++i)
            if (s_.infos_[i].usable && s_.infos_[i].id == q.id) found = &s_.infos_[i];
        if (!found) {
            why = "requires the pack '" + q.id + "'" + (q.version.empty() ? "" : " " + q.version) + ", which must be loaded before it (list it earlier)";
            return false;
        }
        if (!q.version.empty() && !version_satisfies(found->pack_version, q.version)) {
            why = "requires the pack '" + q.id + "' " + q.version + ", but " + (found->pack_version.empty() ? "the one loaded has no pack_version" : "version " + found->pack_version + " is loaded");
            return false;
        }
    }
    return true;
}

bool Loader::run() {
    auto rec = std::make_unique<PackStack::Record>();
    rec_ = rec.get();
    index_ = s_.records_.size();
    info_.path = path_;
    info_.discovered = discovered_;
    info_.enabled = true;
    label_ = basename_of(path_);
    auto finish = [&](bool usable, const std::string& problem) {
        info_.usable = usable;
        info_.problem = problem;
        info_.decoded_bytes = decoded_;
        if (info_.id.empty()) info_.id = slug_of(label_);
        s_.records_.push_back(std::move(rec));
        s_.infos_.push_back(info_);
        if (usable) s_.rebuild_indices();
        return usable;
    };
    auto refuse = [&](const std::string& why) {
        say(Severity::Error, "", "", why);
        return finish(false, why);
    };

    std::string why;
    std::shared_ptr<PackSource> src = open_pack_source(path_, s_.opt_.limits, why);
    if (!src) return refuse(why);
    rec_->source = src;
    for (const std::string& n : src->notes()) say(Severity::Warning, "", "", n);

    std::vector<uint8_t> mbytes;
    if (!src->read("manifest.json", s_.opt_.limits.max_file, mbytes, why)) return refuse(why);
    Json root;
    if (!parse_json(std::string(mbytes.begin(), mbytes.end()), root, why, s_.opt_.limits.max_json_nodes)) return refuse("manifest.json: " + why);
    if (root.is_obj()) {
        if (const Json* n = root.get("name"); n && n->is_str() && !n->s.empty() && n->s.size() <= 80) label_ = n->s;
    }
    ParseOptions po;
    po.validate = s_.opt_.validate;
    po.limits = s_.opt_.limits;
    std::string fatal;
    if (!parse_manifest(root, label_, po, m_, rep_, fatal)) return refuse(fatal);

    info_.name = m_.name.empty() ? label_ : m_.name;
    info_.id = !m_.id.empty() ? m_.id : slug_of(m_.name).empty() ? slug_of(basename_of(path_)) : slug_of(m_.name);
    info_.pack_version = m_.pack_version;
    info_.author = m_.author;
    info_.license = m_.license;
    info_.description = m_.description;
    info_.url = m_.url;
    info_.format_version = m_.version;
    info_.scope = m_.scope;
    info_.skipped_entries = m_.skipped;
    if (s_.opt_.validate) rec_->manifest = std::make_unique<Manifest>(m_);

    if (!s_.opt_.validate && !check_requirements(why)) return refuse(why);
    if (!s_.opt_.validate && !m_.scope.applies(s_.ctx_)) {
        say(Severity::Info, "", "", std::string("none of its entries apply to this game (") + game_name(s_.ctx_.game) + ", build " + s_.ctx_.build_name() + "); nothing was loaded");
        return finish(true, "");
    }
    for (size_t i = 0; i < s_.infos_.size(); ++i)
        if (s_.infos_[i].usable && s_.infos_[i].id == info_.id) {
            say(Severity::Warning, "", "", "has the same id '" + info_.id + "' as the pack loaded before it; the later pack's entries win");
            break;
        }

    graphics();
    far();
    palettes();
    backgrounds();
    atlases();
    stars();
    screens();
    panel();
    clips();
    music();

    info_.provides_graphics = any_graphics_;
    info_.provides_audio = info_.provides_audio || false;
    if (overrides_ > 20) say(Severity::Info, "", "", "and " + std::to_string(overrides_ - 20) + " more entries override those of earlier packs");

    std::string summary = "loaded: " + std::to_string(n_graphics_) + " graphics, " + std::to_string(n_far_) + " far lines, " + std::to_string(n_palettes_) + " palettes";
    auto more = [&](size_t n, const char* what) {
        if (n) summary += ", " + std::to_string(n) + " " + what;
    };
    more(n_bg_, "backgrounds");
    more(n_screens_, "screens");
    more(n_cells_, "panel cells");
    more(n_clips_, "sound clips");
    more(n_slots_, "music slots");
    summary += " (all packs: " + std::to_string(s_.picture_bytes_ >> 10) + " KB of pictures";
    if (s_.audio_bytes_) summary += ", " + std::to_string(s_.audio_bytes_ >> 10) + " KB of sound";
    summary += ")";
    say(Severity::Info, "", "", summary);
    return finish(true, "");
}

// ---- PackStack -----------------------------------------------------------------------------------------------------------

PackStack::PackStack(PackContext ctx, StackOptions opt) : ctx_(std::move(ctx)), opt_(std::move(opt)) {}
PackStack::~PackStack() = default;

bool PackStack::add(const std::string& path, PackReport& report, bool discovered) { return Loader(*this, path, report, discovered).run(); }

void PackStack::rebuild_indices() {
    screen_list_.clear();
    screens_by_block_.clear();
    for (const auto& s : screens_) {
        screen_list_.push_back(s.get());
        if (!s->signature.blocks.empty()) screens_by_block_[s->signature.blocks[0].hash].push_back(s.get());
    }
    clip_length_counts_.clear();
    for (const auto* table : {&sfx_, &speech_})
        for (const auto& kv : *table) {
            const size_t dash = kv.first.find('-');
            if (dash == std::string::npos) continue;
            clip_length_counts_[uint32_t(std::strtoul(kv.first.c_str() + dash + 1, nullptr, 10))]++;
        }
}

bool PackStack::has_graphics() const {
    return !graphics_.empty() || !flashes_.empty() || !far_.empty() || has_stars_ || !palettes_.empty() || has_panel_ || !screens_.empty() ||
           std::any_of(std::begin(has_background_), std::end(has_background_), [](bool b) { return b; });
}

const GraphicEntry* PackStack::graphic(uint64_t hash, int w, int h) const {
    auto it = graphics_.find({hash, w, h});
    return it == graphics_.end() ? nullptr : &it->second;
}
const GraphicEntry* PackStack::flash(uint64_t hash, int w, int h) const {
    auto it = flashes_.find({hash, w, h});
    return it == flashes_.end() ? nullptr : &it->second;
}
const FarLineRef* PackStack::far_line(uint64_t hash) const {
    auto it = far_.find(hash);
    return it == far_.end() ? nullptr : &it->second;
}
const BackgroundEntry* PackStack::background(int level) const { return level >= 1 && level <= 5 && has_background_[level] ? &backgrounds_[level] : nullptr; }

const PaletteOverride* PackStack::palette(const uint16_t* colors) const {
    for (auto it = palettes_.rbegin(); it != palettes_.rend(); ++it) {
        bool same = true;
        for (int i = 0; i < 16 && same; ++i) same = it->match[i] == (colors[i] & 0xFFF);
        if (same) return &*it;
    }
    return nullptr;
}

const std::vector<const ScreenEntry*>& PackStack::screens_for_block(uint64_t h) const {
    auto it = screens_by_block_.find(h);
    return it == screens_by_block_.end() ? no_screens_ : it->second;
}
bool PackStack::has_clip_length(uint32_t n) const { return clip_length_counts_.count(n) != 0; }
const ClipEntry* PackStack::sfx(const std::string& k) const {
    auto it = sfx_.find(k);
    return it == sfx_.end() ? nullptr : &it->second;
}
const ClipEntry* PackStack::speech(const std::string& k) const {
    auto it = speech_.find(k);
    return it == speech_.end() ? nullptr : &it->second;
}
const ClipEntry* PackStack::speech_for_file(const std::string& f) const {
    auto it = speech_by_file_.find(f);
    return it == speech_by_file_.end() ? nullptr : speech(it->second);
}
const MusicSlot* PackStack::music_slot(const std::string& s) const {
    auto it = slots_.find(s);
    return it == slots_.end() ? nullptr : &it->second;
}

bool PackStack::read_file(const FileRef& f, std::vector<uint8_t>& out, std::string& why) const {
    if (!f.valid() || size_t(f.pack) >= records_.size() || !records_[size_t(f.pack)]->source) { why = "no such pack file"; return false; }
    return records_[size_t(f.pack)]->source->read(f.path, opt_.limits.max_audio_file, out, why);
}
std::unique_ptr<ByteSource> PackStack::open_file(const FileRef& f, std::string& why) const {
    if (!f.valid() || size_t(f.pack) >= records_.size() || !records_[size_t(f.pack)]->source) { why = "no such pack file"; return nullptr; }
    return records_[size_t(f.pack)]->source->open(f.path, opt_.limits.max_audio_file, why);
}

} // namespace x2::pack
