// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset packs v2: the contract between the pack loader and everything that consumes packs
// (docs/re-notes/asset-packs-v2-design.md, sections 3-7 and 10; the user guide is docs/ASSET-PACKS.md).
//
// This header is plain data and one read-only interface, so the later workstreams build against it without
// touching each other:
//   WS1b  implements PackLibrary (manifest v2, ZIP, layering, scoping, validator, discovery) and the three
//         functions at the end of this file;
//   WS2   reads graphics, screens, panel cells, backgrounds, stars and flashes from it (presentation only);
//   WS3b  reads sfx and speech clips from it (output mix only);
//   WS4   reads music slots from it (output mix only).
// A pack never changes machine state: the machine runs exactly as it would without one.
//
// With no pack loaded packs() is the null library below: every lookup answers "nothing", every count is zero, so a
// call site behaves exactly as it did before packs existed.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2::pack {

// ---- the game a pack is used with ---------------------------------------------------------------------------------

// `disk` covers every Amiga disk build (the canonical image, the retail pressings, the Power Pack, the 1762 compilation,
// the PDos build and the cracks: one engine, one capture layout); `cdtv` the CDTV disc; `st` the Atari ST.
enum class GameFamily : uint8_t { Disk = 0, Cdtv = 1, St = 2 };
const char* game_name(GameFamily g);                     // "disk", "cdtv", "st"
bool parse_game(const std::string& s, GameFamily& out);  // exactly those three names

// What a pack is being used for: the family and the importer's manifest `build=` tag ("" and "canonical" both name the
// canonical Amiga image; others: power-pack-2234, compilation-1762, pdos-v5, cdtv, atari-st-kixx).
struct PackContext {
    GameFamily game = GameFamily::Disk;
    std::string build;
    std::string build_name() const { return build.empty() ? "canonical" : build; }
};

// `games` and `builds` of a pack or of one entry (spec 3.3). The default applies everywhere.
struct GameScope {
    static constexpr uint8_t kAllGames = 7;                 // bit per GameFamily
    uint8_t games = kAllGames;
    std::vector<std::string> builds;                        // empty: every build
    bool applies(const PackContext& c) const;
    // The scope of an entry inside a pack: the intersection (an entry cannot widen its pack).
    static GameScope intersect(const GameScope& pack, const GameScope& entry);
};

// ---- keys and hashing (spec 3.4): every implementation, in C++ and in the Python tools, must agree -----------------

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;
uint64_t fnv1a64(const uint8_t* data, size_t n, uint64_t h = kFnvOffset);
std::string hash_hex(uint64_t h);                           // 16 lowercase hex digits
bool is_hash_hex(const std::string& s);                     // exactly 16 lowercase hex digits
// Block hash of a screen signature block or a panel run: w (u16 LE), h (u16 LE), then w*h colour-index bytes, row-major.
uint64_t block_hash(int w, int h, const uint8_t* index_bytes);
// The same for the region (x, y, w, h) of an indexed frame with `stride` bytes per row.
uint64_t block_hash_region(const uint8_t* frame, int stride, int x, int y, int w, int h);
// Sample key of an effect or speech clip: "<fnv1a64>-<bytes>" with the hash over `bytes` (u32 LE) then the sample bytes.
std::string sample_key(const uint8_t* sample, uint32_t bytes);
// Panel key of a run of `span` cells (1..8): "<block hash>-<span>".
std::string panel_key(uint64_t block, int span);
// Key syntax checks used by the loader and the validator.
bool valid_graphic_key(const std::string& k);               // "<16 hex>-<w>x<h>", w and h 1..4096
bool valid_flash_key(const std::string& k);                 // "f:<16 hex>-<w>x<h>"
bool valid_sample_key(const std::string& k);                // "<16 hex>-<bytes>"
bool valid_panel_key(const std::string& k);                 // "<16 hex>-<1..8>"

// ---- pictures ------------------------------------------------------------------------------------------------------

// An RGBA picture, premultiplied alpha, R in the low byte and A in the high byte: the layout of modern::Image
// (runtime/modern/assets.h), so WS1b can hand the PNG decoder's output straight over.
struct Picture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
};
struct Anchor {
    int x = 0, y = 0;  // the picture pixel placed on the original's top-left corner
};

// ---- graphics ------------------------------------------------------------------------------------------------------

// A sprite or tile replacement (v1 `graphics` entries, atlas frames and groups expanded at load time).
struct GraphicEntry {
    const Picture* pic = nullptr;
    int scale = 1;                 // picture pixels per Amiga pixel, 1..8
    Anchor anchor;
    const Picture* flash = nullptr;  // `flash_png`: the look while the graphic is hit; null = derive or show the original
    int flash_scale = 1;
};
// One row block of a far-layer strip (v1 `far`).
struct FarLineRef {
    const Picture* strip = nullptr;  // 320 * scale wide
    int scale = 1;
    int row = 0;                     // rows row*scale .. (row+1)*scale - 1
};
// Layer bits of PaletteOverride::layers: 1 << layer, with the modern::Layer order Other 0, Far 1, Tiles 2, Stars 3, Object 4.
struct PaletteOverride {
    uint16_t match[16] = {};  // the level's COLOR00-15 (12-bit)
    uint32_t rgba[16] = {};   // replacement colours, 0xAABBGGRR
    uint8_t layers = 0x1F;
};
// A whole-level far layer (`backgrounds`), wrapped vertically and bound to the far layer's ring row.
struct BackgroundEntry {
    int level = 0;                   // 1..5
    const Picture* pic = nullptr;    // 320*scale wide, ring_lines*scale tall
    int scale = 1;
    int ring_lines = 0;              // 0: the build's value (CaptureLayout::far_ring_lines)
};
struct StarsEntry {
    enum class Shape : uint8_t { Pixel, Round };
    Shape shape = Shape::Pixel;
    int size = 1;           // Amiga pixels, rounded up from size_px (at least 1)
    float softness = 0.5f;  // 0..1
    float size_px = 1.0f;   // the manifest's value, 0.25..8 (added by WS1b: `size` in the manifest is a number)
};

// ---- screens (spec 4.4) --------------------------------------------------------------------------------------------

struct SignatureBlock {
    int x = 0, y = 0;
    uint64_t hash = 0;
};
struct ScreenSignature {
    int block = 16;                       // block side in indexed-frame pixels
    int min = 0;                          // at least this many blocks must match
    std::vector<SignatureBlock> blocks;   // up to 64
};
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
enum class FadeMode : uint8_t { Scale, None };
struct ScreenEntry {
    std::string name;
    ScreenSignature signature;
    const Picture* pic = nullptr;         // 320*scale by 200*scale
    int scale = 1;
    Anchor anchor;
    std::vector<uint32_t> ref_palette;    // 0x00RRGGBB of the full-brightness palette the dump recorded (2..32)
    FadeMode fade = FadeMode::Scale;
    std::vector<Rect> dynamic;            // parts the game still draws itself (Amiga pixels, in the window)
    int bg_index = 0;                     // inside `dynamic`, original pixels of this index stay transparent
    const Picture* wide_pic = nullptr;    // used instead of black sides with widescreen on
    int wide_scale = 0;                   // 0: the pack's scale
};

// ---- status panel (spec 4.5) ---------------------------------------------------------------------------------------

struct PanelCell {
    const Picture* pic = nullptr;
    int scale = 1;
    Anchor anchor;
};
struct PanelSet {
    int scale = 1;
    int cell_w = 8;                       // pixels of one cell
    int max_span = 6;                     // longest run of cells matched as one key
    const Picture* background = nullptr;  // 320*scale by 8*scale, drawn first
    // The palette (0x00RRGGBB per index) the pictures were made with, and whether the game's fades dim them: like a screen's
    // `ref_palette` and `fade`; without a reference palette the panel stays at full brightness through a fade.
    std::vector<uint32_t> ref_palette;
    FadeMode fade = FadeMode::Scale;
    Anchor background_anchor;             // (added by WS1b) the background picture pixel placed on the strip's top-left corner
    std::unordered_map<std::string, PanelCell> cells;  // panel_key -> replacement
    bool empty() const { return cells.empty() && !background; }
    const PanelCell* find(const std::string& key) const {
        auto it = cells.find(key);
        return it == cells.end() ? nullptr : &it->second;
    }
};

// ---- audio (spec 5) ------------------------------------------------------------------------------------------------

// A decoded clip: interleaved signed 16-bit, 1 or 2 channels.
struct DecodedClip {
    std::vector<int16_t> pcm;
    int channels = 1;
    int rate = 0;
    uint64_t frames() const { return channels > 0 ? pcm.size() / size_t(channels) : 0; }
};
enum class PanMode : uint8_t { Channel, Position };      // mono files: side of the Paula channel, or a constant position
enum class PitchMode : uint8_t { Follow, Fixed };         // follow the game's period changes, or play at the file's own rate
enum class OriginalMode : uint8_t { Mute, Mix };          // the original channel in the mix: removed, or kept at original_mix_db
enum class CutMode : uint8_t { Channel, Never };          // end with the channel's next DMA start/stop, or ring out
// An effect or speech replacement (`audio.sfx`, `audio.speech`).
struct ClipEntry {
    std::string key;                       // sample key
    std::string file;                      // path inside the pack (diagnostics)
    std::shared_ptr<const DecodedClip> clip;
    int ref_period = 0;                    // Paula period at which the file sounds like the original; 0 = take it from the dump default
    float gain_db = 0.0f;
    PanMode pan = PanMode::Channel;
    float pan_pos = 0.0f;                  // -1 (left) .. 1 (right) with PanMode::Position
    PitchMode pitch = PitchMode::Follow;
    OriginalMode original = OriginalMode::Mute;
    float original_mix_db = 0.0f;          // with OriginalMode::Mix ("mix" = 0, "mix:-12" = -12)
    CutMode cut = CutMode::Channel;
    int fade_out_ms = 5;
    bool loop = false;
    std::string cd0_file;                  // speech only: the disc file the clip came from ("SPEECH63"), language independent
};

// How a pack file is read: whole, or as a seekable stream (music is streamed from ZIP members stored uncompressed).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const = 0;
    // Reads up to n bytes at `offset`; returns the count (0 at the end or on error).
    virtual size_t read(uint64_t offset, void* dst, size_t n) = 0;
};

enum class LoopKind : uint8_t { Default, Off, Whole, Points };  // Default: the manifest says nothing (file metadata decides)
struct MusicLoop {
    LoopKind kind = LoopKind::Default;
    double start_s = 0.0;  // Points: seconds in the file after `intro`
    double end_s = -1.0;   // Points: negative = the end of the file
};
enum class LoopMode : uint8_t { Pack, Game };           // seamless by the pack's points, or the original's restart rule
enum class Retrigger : uint8_t { Restart, Continue };
// `audio.music.defaults`: applies to every slot that does not say otherwise.
struct MusicDefaults {
    int crossfade_ms = 0;  // 0..5000
    int fade_in_ms = 0;
    int fade_out_ms = 0;
    LoopMode loop_mode = LoopMode::Pack;
    Retrigger retrigger = Retrigger::Restart;
};
// Where a pack file lives, for streaming: the pack's index in PackLibrary::packs() and the path inside it.
struct FileRef {
    int pack = -1;
    std::string path;
    bool valid() const { return pack >= 0 && !path.empty(); }
};
// One music slot: title, level1..level5, level (fallback), cd2..cd13 (CDTV). `mute` with no file silences the slot.
struct MusicSlot {
    std::string slot;
    FileRef file, intro;
    bool mute = false;
    MusicLoop loop;
    float gain_db = 0.0f;
    std::optional<int> crossfade_ms, fade_in_ms, fade_out_ms;  // unset: MusicDefaults
    std::optional<LoopMode> loop_mode;
    std::optional<Retrigger> retrigger;
};

// ---- audio decoding hook (added by WS1b) ----------------------------------------------------------------------------

// The loader reads sound files through this hook so that it does not depend on the decoders (runtime/audio/decode.*, WS3a): the
// audio stage registers the codec once at start-up. With no codec registered, packs still load; `audio.sfx` and `audio.speech`
// entries are then not registered (nothing could play them) and music slots are only checked for existence. A pack ByteSource is
// offset based; an adapter to the decoders' stream is a few lines.
struct AudioInfo {
    std::string format;           // "wav", "flac", "vorbis"
    int channels = 0, rate = 0;
    int64_t frames = -1;          // -1: the file does not say
    bool has_loop = false;        // from the file (WAV smpl chunk, Vorbis/FLAC comments)
    uint64_t loop_start = 0, loop_end = 0;  // frames, end exclusive
    float peak = 0.0f;            // after decode(): the largest absolute sample, 1.0 = full scale
};
struct AudioLimits {
    uint64_t max_file_bytes = uint64_t(512) << 20;
    uint64_t max_frames = uint64_t(7200) * 192000;
    size_t max_decoded_bytes = 32u << 20;   // decode(): the int16 output
    int min_rate = 8000, max_rate = 192000, max_channels = 2;
};
struct AudioCodec {
    // Reads the header only. False with `why` for an unknown format, a limit exceeded or a damaged header.
    bool (*probe)(ByteSource& src, const AudioLimits& limits, AudioInfo& info, std::string& why) = nullptr;
    // Decodes the whole file to interleaved int16 at the file's own rate (the host resamples).
    bool (*decode)(ByteSource& src, const AudioLimits& limits, DecodedClip& out, AudioInfo& info, std::string& why) = nullptr;
};
void set_audio_codec(const AudioCodec* codec);  // nullptr: none
const AudioCodec* audio_codec();

// ---- messages and pack information ---------------------------------------------------------------------------------

enum class Severity : uint8_t { Info, Warning, Error };
// One finding, for the loader and the validator: "asset pack: <pack>: <section>["<key>"]: <problem>".
struct PackMessage {
    Severity severity = Severity::Info;
    std::string pack, section, key, text;
    std::string format() const;
};
struct PackReport {
    std::vector<PackMessage> messages;
    void add(Severity s, std::string pack, std::string section, std::string key, std::string text);
    size_t count(Severity s) const;
};

// What a loaded or discovered pack says about itself (manifest metadata plus the loader's verdict).
struct PackInfo {
    std::string id, name, pack_version, author, license, description, url;
    std::string credits;                 // (added by the launcher's packs page) the manifest's credits as "Name (role); Name"
    std::string path;                    // directory or .zip
    int format_version = 0;              // manifest `version`
    GameScope scope;
    bool provides_graphics = false, provides_screens = false, provides_audio = false;
    bool discovered = false;             // found in <data dir>/packs/
    bool enabled = false;
    bool usable = false;                 // false: not used (the reason is in the report)
    std::string problem;                 // (added by WS1b) why it is not usable, or a one-line note, "" when fine
    size_t entries = 0, skipped_entries = 0;
    uint64_t decoded_bytes = 0;
};

// ---- the library ---------------------------------------------------------------------------------------------------

// A read-only snapshot of the layered packs for one PackContext (later packs already override earlier ones per key,
// entries outside the context's scope are not in it). Shared by the presenter and the audio stage; thread-compatible
// for reading after construction.
class PackLibrary {
public:
    virtual ~PackLibrary() = default;

    virtual const PackContext& context() const = 0;
    virtual const std::vector<PackInfo>& packs() const = 0;
    virtual bool has_graphics() const = 0;  // any graphics, screens, panel, background, stars or palette entry
    virtual bool has_audio() const = 0;     // any sfx, speech or music entry
    bool empty() const { return !has_graphics() && !has_audio(); }

    // graphics (the presenter; hash and size as drawn, spec 3.4)
    virtual const GraphicEntry* graphic(uint64_t hash, int w, int h) const = 0;
    virtual const GraphicEntry* flash(uint64_t mask_hash, int w, int h) const = 0;   // explicit `f:` keys
    virtual const FarLineRef* far_line(uint64_t line_hash) const = 0;
    virtual const BackgroundEntry* background(int level) const = 0;
    virtual const StarsEntry* stars() const = 0;
    // The override for a step palette (COLOR00-15), the latest loaded pack first.
    virtual const PaletteOverride* palette(const uint16_t* colors16) const = 0;
    virtual const PanelSet* panel() const = 0;
    // Screens: all of them in load order, and the ones whose first signature block has this hash (a short list).
    virtual const std::vector<const ScreenEntry*>& screens() const = 0;
    virtual const std::vector<const ScreenEntry*>& screens_for_block(uint64_t first_block_hash) const = 0;

    // audio (the output stage)
    virtual bool has_clip_length(uint32_t sample_bytes) const = 0;  // a replacement exists for this length: hash only then
    virtual const ClipEntry* sfx(const std::string& sample_key) const = 0;
    virtual const ClipEntry* speech(const std::string& sample_key) const = 0;
    virtual const ClipEntry* speech_for_file(const std::string& cd0_file) const = 0;
    virtual const MusicDefaults& music_defaults() const = 0;
    // The slot named exactly `slot`; the fallback order (exact, alias, `level`) is the music stage's (WS4).
    virtual const MusicSlot* music_slot(const std::string& slot) const = 0;

    // pack files (music streaming, lazy ZIP members)
    virtual bool read_file(const FileRef& f, std::vector<uint8_t>& out, std::string& why) const = 0;
    virtual std::unique_ptr<ByteSource> open_file(const FileRef& f, std::string& why) const = 0;
};

// The empty library: nothing is replaced. Always valid; not copyable state.
const PackLibrary& null_library();

// ---- settings and the one global accessor -------------------------------------------------------------------------

// Everything the run decides about packs, filled by the front end from settings.cfg and the command line.
struct AssetPackSettings {
    std::vector<std::string> pack_paths;    // settings `asset_packs`, then each --asset-pack, in load order
    std::vector<std::string> packs_enabled; // settings `packs_enabled`: ids of discovered packs, in load order
    std::string packs_dir;                  // <data dir>/packs ("" = no per-user data directory)
    bool upgrade = false;                   // graphics and screens (`upgrade`, --modern upgrade, implied by --asset-pack)
    bool audio_upgrade = true;              // pack audio (`audio_upgrade`, --modern audio|noaudio); only used when a pack has audio
    bool faithful = false;                  // the Faithful switch (F1, --faithful): every pack effect off
    int speech_volume = 100;                // percent
    bool graphics_active() const { return upgrade && !faithful; }
    bool audio_active() const { return audio_upgrade && !faithful; }
};
// Splits a ';'-separated list (settings values), trimming blanks and dropping empty items.
std::vector<std::string> split_list(const std::string& s);
// Joins `dir` and a child with exactly one separator ("" when dir is empty).
std::string join_path(const std::string& dir, const std::string& child);
// <data dir>/packs: the folder scanned for discovered packs.
std::string default_packs_dir(const std::string& data_dir);
// One item of `--modern SPEC` that concerns packs: "audio" or "noaudio". Returns true when it was one of them.
bool apply_modern_item(const std::string& item, AssetPackSettings& s);

const AssetPackSettings& pack_settings();
void set_pack_settings(const AssetPackSettings& s);
// The installed library, or the null library. WS1b installs the real one once, before the machine runs.
const PackLibrary& packs();
void set_packs(std::shared_ptr<const PackLibrary> lib);  // nullptr: back to the null library

// ---- entry points (WS1b: pack_load.cpp, pack_check.cpp; --dump-audio stays a stub until WS3b) -------------------------

struct CheckOptions {
    std::string path;                       // --check-asset-pack PATH
    std::optional<GameFamily> game;         // --game
    std::string build;                      // --build TAG
    std::string against;                    // --against DUMPDIR
    bool full = false, strict = false, json = false;
};
// Load-time entry: layers packs_enabled (ids found in packs_dir), then pack_paths, for the context, into one library. Returns nullptr
// (the null library) when nothing was asked for; problems of single packs and entries are in the report, a refused pack is skipped.
std::shared_ptr<const PackLibrary> load_packs(const AssetPackSettings& s, const PackContext& c, PackReport& report);
// `--check-asset-pack`: prints the findings and returns the exit code of spec section 6 (0 valid, 1 warnings with --strict,
// 2 unusable or invalid entries, 3 unreadable).
int check_asset_pack_cli(const CheckOptions& o);
// `--list-asset-packs [--json]`: lists the discovered packs in AssetPackSettings::packs_dir and returns 0.
int list_asset_packs_cli(const AssetPackSettings& s, bool json);

} // namespace x2::pack
