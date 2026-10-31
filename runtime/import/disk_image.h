// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// An in-memory floppy disk: the common form every container format (ADF, DMS, extended ADF, and later IPF, SCP,
// KryoFlux, HFE) is decoded to before the importer looks at it (docs: .omc/plans/xenon2-disk-variants-plan.md,
// section 3.1). A disk has up to 168 tracks (84 cylinders x 2 sides), numbered cylinder * 2 + side. A track is
// either Decoded (11 sectors of 512 bytes, each with a status) or Raw (an MFM bit stream, kept for protection
// tracks and for the flux decoders that come later).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kSectorsPerTrack = 11;
constexpr size_t kSectorBytes = 512;
constexpr size_t kTrackBytes = kSectorsPerTrack * kSectorBytes; // 5632
constexpr int kStandardTracks = 160;                            // 80 cylinders x 2 sides
constexpr int kMaxTracks = 168;                                 // 84 cylinders x 2 sides

enum class SectorStatus : uint8_t { Missing = 0, Ok, BadHeader, BadData };

struct Track {
    enum class Kind : uint8_t { Empty, Decoded, Raw };
    Kind kind = Kind::Empty;

    // Decoded: kTrackBytes of sector data in sector order, and one status per sector.
    std::vector<uint8_t> sectors;
    std::array<SectorStatus, kSectorsPerTrack> status{};

    // Raw: MFM bits, most significant bit first, `bit_length` of them (the rest of the last byte is padding).
    std::vector<uint8_t> mfm;
    uint32_t bit_length = 0;
    uint8_t revolutions = 1;
    // Raw tracks read from flux: the same track as other bit streams (the other revolutions, and the revolutions again through
    // a PLL that insists on valid MFM run lengths). A decoder for a custom track format tries `mfm` first, then these, and
    // takes only what passes its own checksums; nothing here is ever "repaired".
    struct Stream { std::vector<uint8_t> bits; uint32_t bit_length = 0; };
    std::vector<Stream> alt;

    bool all_sectors_ok() const;
};

class DiskImage {
public:
    DiskImage() : tracks_(kMaxTracks) {}

    // A plain 901120-byte ADF. False if `size` is not exactly that.
    static bool from_adf(const uint8_t* data, size_t size, DiskImage& out);

    Track& track(int index) { return tracks_[size_t(index)]; }
    const Track& track(int index) const { return tracks_[size_t(index)]; }

    // The 512 bytes of logical block `block` (0..1759), or nullptr if its track is not Decoded or the sector is
    // not intact.
    const uint8_t* sector(int block) const;

    // Numbers of tracks 0..159 that are not Decoded with every sector intact, in order.
    std::vector<int> nonstandard_tracks() const;
    // "track 3 (cylinder 1, side 1): raw MFM; track 7: missing; ..." for the first few of nonstandard_tracks().
    std::string describe_nonstandard() const;

    // The 901120 bytes of a plain ADF. False with the reason (the nonstandard tracks) otherwise. With
    // zero_fill_missing, tracks that are Empty are written as zero bytes instead of failing.
    bool to_adf(std::vector<uint8_t>& out, std::string& why, bool zero_fill_missing = false) const;

    // SHA-1 (hex) of one sector, or of a whole track's sector data; empty if not Decoded.
    std::string sector_sha1(int track, int sector) const;
    std::string track_sha1(int track) const;

private:
    std::vector<Track> tracks_;
};

// Decodes one raw MFM track holding standard Amiga sectors (sync 0x4489 0x4489, header and data checksums) into
// `out` as a Decoded track. Sectors may start at any bit offset and the stream wraps. Returns the number of
// sectors found intact; the others get BadHeader/BadData/Missing in out.status. `track_number` is the expected
// track (a header naming another track is a BadHeader).
// With wrap = false the stream is linear: a sector whose data would run past the end is not decoded (flux
// readers use this, because the start of a revolution is not continuous with its end, and a degenerate sector
// - constant data - can pass the XOR checksums with the wrong bits across such a seam).
int decode_amiga_mfm_track(const uint8_t* bits, uint32_t bit_length, int track_number, Track& out, bool wrap = true);

// Container formats the importer knows by their content (never by file name).
enum class Container { Unknown, Adf, AdfHd, Adz, Zip, Dms, ExtAdf, ExtAdf1, Ipf, Scp, KryoFlux, Hfe, Lha, FluxEngine };

Container detect_container(const uint8_t* data, size_t size);
const char* container_name(Container c);

} // namespace x2::import
