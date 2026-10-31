// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Debug outputs for headless runs: register write logs, chip RAM snapshots and a crude bitplane framebuffer;
// and the coverage-only chip RAM pokes of --poke and the input scripts.
#pragma once
#include <cstdint>
#include <string>

namespace x2 {

class Machine;

// Write m.frame_writes as text, one line per write: "frame line hpos src pc REG value".
bool write_reglog(const Machine& m, const std::string& path);
bool write_chip_ram(const Machine& m, const std::string& path);
// Decode the bitplanes latched at the start of the display window (m.display) into a PPM. Low-res planar
// playfield only (no hires/HAM/dual playfield/scroll/sprites): Phase 3b replaces this with the real renderer.
bool write_bitplane_ppm(const Machine& m, const std::string& path);

// COVERAGE ONLY: a chip RAM write applied at the start of frames from..to. size is 1, 2 or 4 bytes (big-endian).
// handler == 0: write value at addr. handler != 0 ("pokeobj"): addr is an offset, and value is written into every
// object on the game's hittable-object list whose hit handler (+$A) is handler, e.g. a boss's HP at +$32.
struct Poke {
    uint32_t from, to, addr, value;
    int size;
    uint32_t handler = 0;
};

// The hittable-object list as the collision loop $306E walks it: head pointer at $B4C, next at +$12, a type
// word (+0) of 0 ends it. Type-$B8 group heads let the collision loop skip their members (+$5A is the last
// member); following +$12 alone visits the members too. The walk stops after kObjWalkLimit nodes, so a
// corrupt or self-linked list cannot hang it.
constexpr int kObjWalkLimit = 1024;

// Apply p now; returns the number of writes (1 for a plain poke, the number of matching objects for pokeobj).
// objlist_head is where the build keeps the head pointer of the hittable-object list ($B4C in the disk builds, $B52 in the CDTV
// release: BuildInfo::var_objlist).
int apply_poke(Machine& m, const Poke& p, uint32_t objlist_head = 0xB4C);

} // namespace x2
