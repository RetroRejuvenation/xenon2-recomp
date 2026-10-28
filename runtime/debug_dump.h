// Debug outputs for headless runs: register write logs, chip RAM snapshots and a crude bitplane framebuffer.
#pragma once
#include <string>

namespace x2 {

class Machine;

// Write m.frame_writes as text, one line per write: "frame line hpos src pc REG value".
bool write_reglog(const Machine& m, const std::string& path);
bool write_chip_ram(const Machine& m, const std::string& path);
// Decode the bitplanes latched at the start of the display window (m.display) into a PPM. Low-res planar
// playfield only (no hires/HAM/dual playfield/scroll/sprites): Phase 3b replaces this with the real renderer.
bool write_bitplane_ppm(const Machine& m, const std::string& path);

} // namespace x2
