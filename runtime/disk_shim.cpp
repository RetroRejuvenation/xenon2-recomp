#include "disk_shim.h"

#include "machine.h"

#include <cstdio>

namespace x2 {

void DiskShim::install(Machine& m) {
    m.add_trap(kLoadSectors, [this](Machine& mm) { load_sectors(mm); });
}

// Replaces $921A..$92BE. The original reads whole tracks into the staging buffer at $44000 and copies the
// requested sectors from there, so after a load $44000 holds the last track touched; that side effect is
// reproduced. The raw MFM buffer at $45600 is not (it only ever holds undecoded flux data).
// The disc-id check at $8F9A needs no hook: it calls load_sectors for sector 0 into $49600 and compares
// the real "X2B " id from the disc image.
void DiskShim::load_sectors(Machine& m) {
    uint32_t dest = m.areg(0) & 0xFFFFFF;
    uint16_t sector = uint16_t(m.dreg(0));
    uint16_t count = uint16_t(m.dreg(1));
    ++calls;
    last_sector = sector;
    last_count = count;
    last_dest = dest;
    const uint32_t total = uint32_t(disc2_.size() / kSectorSize);
    uint32_t end = uint32_t(sector) + count;
    if (count == 0 || end > total) {
        ++bad_requests;
        std::fprintf(stderr, "disk shim: bad request sector=%u count=%u (disc has %u sectors)\n", sector, count, total);
        m.set_dreg(0, (m.dreg(0) & 0xFFFF0000u) | 0xFFFF);
        m.set_sr(uint16_t((m.sr() & ~0x1F) | 0x08)); // moveq #-1,d0: N set
        m.do_rts();
        return;
    }
    m.load(dest, disc2_.data() + size_t(sector) * kSectorSize, size_t(count) * kSectorSize);
    uint32_t last_track = (end - 1) / kSectorsPerTrack;
    m.load(kStagingBuffer, disc2_.data() + size_t(last_track) * kSectorsPerTrack * kSectorSize,
           kSectorsPerTrack * kSectorSize);
    sectors_read += count;
    if (on_load) on_load(dest, sector, count, disc2_.data() + size_t(sector) * kSectorSize);
    m.set_areg(0, dest + uint32_t(count) * kSectorSize); // the copy loop leaves a0 past the data
    m.set_dreg(0, 0);                                    // moveq #0,d0
    m.set_sr(uint16_t((m.sr() & ~0x1F) | 0x04));         // Z set, N/V/C clear
    m.do_rts();
}

} // namespace x2
