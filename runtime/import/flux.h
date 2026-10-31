// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Flux-level disk formats: the time between magnetic flux transitions, as a floppy drive's read head sees it.
// SCP (SuperCard Pro), KryoFlux stream files and HFE (HxC) are read into a FluxDisk (HFE already holds bit cells and
// bypasses the PLL); decode_flux_disk then recovers the raw MFM bit cells with a phase-locked loop and the
// standard Amiga sectors from those, voting over the revolutions the dump holds. Whatever is not standard Amiga
// sectors (copy-protection tracks) stays a Raw track in the DiskImage. Design and limits: docs/re-notes/flux.md.
#pragma once
#include "import/disk_image.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

// One revolution of one track.
struct FluxRev {
    // Nanoseconds between consecutive flux transitions. The first value is the time since the transition before
    // the revolution's start where the capture holds one (SCP after the first revolution), else the time from
    // the index pulse to the first transition.
    std::vector<uint32_t> ns;
    // Nanoseconds from the last transition to the next index pulse, when the capture stores the interval that
    // spans the pulse in two parts (KryoFlux): tail + the next revolution's first value is that interval. 0 if the
    // next revolution's first value is already the whole interval (SCP) or unknown.
    uint32_t tail_ns = 0;
};

struct FluxTrack {
    std::vector<FluxRev> revs;
};

struct FluxDisk {
    FluxDisk() : tracks(kMaxTracks) {}
    std::vector<FluxTrack> tracks; // index = cylinder * 2 + side
    std::string source;            // "SCP", "KryoFlux stream", ... for messages
};

struct PllParams {
    double nominal_ns = 2000.0; // one MFM cell of a double-density Amiga disk (4 us per data bit)
    double phase_gain = 0.1;    // how much of the timing error of each transition moves the cell boundary
    double freq_gain = 0.005;   // how much of it retunes the cell length
    double range = 0.12;        // the cell length may drift this far (fraction) from nominal
    double overlap_ns = 24e6;   // flux of the next revolution appended so that a sector across the index decodes
    // A transition that follows fewer than `run_min` or more than `run_max` cells is taken to follow that many. Valid MFM
    // has runs of 2 to 4 cells, so the retry pass clamps to that; the first pass (1 to 12) does not, because copy-protection
    // tracks legitimately break the rule.
    int run_min = 1;
    int run_max = 12;
};

// Flux intervals -> raw MFM bits, most significant bit first. A transition after n cells is n-1 zero bits and a one.
struct PllResult {
    std::vector<uint8_t> bits;
    uint32_t bit_length = 0;
    uint32_t own_bits = 0;    // bits produced by the first `own_count` intervals (the revolution proper)
    double mean_cell_ns = 0;  // mean cell length over the run (for diagnostics)
    uint32_t long_gaps = 0;   // transitions that followed an implausibly long silence
};
PllResult pll_decode(const std::vector<uint32_t>& intervals, size_t own_count, const PllParams& p = {});

// What decode_flux_track found, per track.
struct FluxTrackReport {
    int track = 0;
    int revolutions = 0;
    bool raw = false;                           // no revolution held a standard Amiga sector
    int late = 0;                               // sectors intact only in a later revolution
    bool disagree = false;                      // two revolutions gave different data for a sector that passed its checksum
    int retried = 0;                            // sectors intact only in the retry pass (a PLL limited to valid MFM runs)
    std::array<uint8_t, kSectorsPerTrack> ok_revs{};          // revolutions in which each sector was intact
    std::array<SectorStatus, kSectorsPerTrack> best{};        // best status seen over all revolutions
    double rev_ms = 0;                          // mean revolution time in milliseconds (0 if unknown)
    double mean_cell_ns = 0;
};

struct FluxReport {
    std::vector<FluxTrackReport> tracks;
    int recovered_late = 0; // sectors that were intact in some revolution but not the first
    int recovered_retry = 0; // sectors that only the retry pass (a clamped PLL) read intact
    int disagreements = 0;
    // "track 7 sector 3: data checksum error in all 3 revolutions; ..." for the first few unreadable sectors of
    // tracks 0..159 that are not raw.
    std::string problems(size_t max_items = 6) const;
    // One line: tracks decoded, revolutions, raw tracks, sectors rescued by later revolutions.
    std::string summary() const;
};

// Decodes one track: every revolution is run through the PLL with the start of the next revolution appended,
// sectors are decoded from each, and the first intact copy of every sector wins. With no intact sector at all the
// track becomes Raw (the first revolution's bit cells). `track_number` is cylinder * 2 + side.
void decode_flux_track(const FluxTrack& ft, int track_number, Track& out, FluxTrackReport& rep, const PllParams& p = {});

// All tracks that hold flux; the others stay Empty.
void decode_flux_disk(const FluxDisk& fd, DiskImage& out, FluxReport& rep, const PllParams& p = {});

// Adds the revolutions of `other` (another capture of the same disk) to every track of `fd`. More revolutions mean more
// chances for each sector to read intact; a sector is only ever accepted when its own checksums pass.
void merge_flux_disks(FluxDisk& fd, const FluxDisk& other);

// How many revolutions of a track the alternate streams of a raw track are built from, and how many alternate streams one
// track keeps (memory: about 12 KB each).
constexpr int kAltRevolutions = 8;
constexpr size_t kMaxAltStreams = 24;

// ---------------------------------------------------------------------------------------------------------
// Readers. `why` gets the reason on failure; `warnings` (optional) notes oddities that did not stop the read.

// SuperCard Pro: "SCP" header, track offset table, per-revolution 16-bit flux deltas (25 ns ticks by default).
bool read_scp(const std::vector<uint8_t>& data, FluxDisk& out, std::string& why, std::vector<std::string>* warnings = nullptr);

// FluxEngine / Greaseweazle-style ".flux" file (FluxEngine's native format): a protobuf stream of tracks, each a list of
// flux bytes (bit 7 = a transition, bit 6 = the index pulse, low 6 bits = 83.33 ns ticks, 0x3F = 63 more ticks).
bool read_fluxengine(const std::vector<uint8_t>& data, FluxDisk& out, std::string& why, std::vector<std::string>* warnings = nullptr);
// True if the bytes start like a FluxEngine file (field 1 = the "Flux" magic).
bool is_fluxengine(const uint8_t* d, size_t n);

// One KryoFlux stream file (trackNN.S.raw): flux cells and out-of-band blocks, sliced into revolutions at the
// index blocks.
bool read_kryoflux_stream(const std::vector<uint8_t>& data, FluxTrack& out, std::string& why,
                          std::vector<std::string>* warnings = nullptr);

// The cylinder and side a KryoFlux stream file name denotes ("track07.1.raw", any directory prefix); false if the
// name has no such form.
bool kryoflux_track_from_name(const std::string& name, int& cylinder, int& side);

// HxC floppy emulator image, v1 ("HXCPICFE") and v3 ("HXCHFEV3", best effort). It stores bit cells, not flux, so the
// tracks are decoded directly into `out`; HFE images of HD disks are refused.
bool read_hfe(const std::vector<uint8_t>& data, DiskImage& out, std::string& why, bool& high_density,
              std::vector<std::string>* warnings = nullptr);

} // namespace x2::import
