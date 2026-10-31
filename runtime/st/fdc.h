// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's floppy path: a WD1772 controller behind the DMA chip ($FF8604 data/command, $FF8606 mode and status,
// $FF8609/0B/0D DMA address), reading a logical disk image (st_disk.h) at command level. Type I commands (restore,
// seek, step) take the step rate's time per track, a sector read takes one sector time (a fifth of a revolution at 300
// rpm with a perfectly skewed disk), a multi-sector read runs on one sector time apart until the sector number runs off
// the track (record not found) or a force-interrupt ends it. Data lands in RAM when a sector completes; the DMA address
// the CPU reads advances in 16-byte steps while a transfer runs, as the real one does. INTRQ is raised on completion and
// cleared by a status read or a new command; the machine routes it to MFP GPIP bit 5 (active low).
//
// Not modelled: spin-up and head-load delays, the index-pulse interrupt conditions of force-interrupt, write commands (the
// disk is write-protected), read address / read track (record not found), a missing drive select (the drive is always
// ready). Docs: docs/re-notes/atari-st-machine.md.
#pragma once
#include "st/st_disk.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace x2::st {

class FdcDma {
public:
    struct ReadRecord {
        uint64_t cycle;
        uint8_t track, side, sector;
        uint32_t addr;
        bool stored; // the sector data was copied to RAM (a DMA count was set)
    };

    void set_disk(const StDisk* d) { disk_ = d; }
    // RAM the DMA writes into (addresses beyond `size` are dropped).
    void set_ram(uint8_t* ram, uint32_t size) { ram_ = ram; ram_size_ = size; }
    // Called when INTRQ changes: (asserted, CPU cycle of the change).
    void set_irq_callback(std::function<void(bool, uint64_t)> cb) { irq_cb_ = std::move(cb); }
    // The drive side from YM port A bit 0: low selects side 1.
    void set_side(int side) { side_ = side; }
    // Time divisor for commands: 1 = a real drive, N = N times faster (development and tests).
    void set_speed(unsigned s) { speed_ = s ? s : 1; }
    void log_reads(bool on) { log_ = on; }
    const std::vector<ReadRecord>& read_log() const { return read_log_; }
    uint64_t commands_issued() const { return commands_; }

    // CPU access: `off` is the address - $FF8600.
    uint16_t read16(unsigned off, uint64_t now);
    void write16(unsigned off, uint16_t v, uint64_t now);
    uint8_t read8(unsigned off, uint64_t now);
    void write8(unsigned off, uint8_t v, uint64_t now);

    // Events: complete commands up to `now`; the cycle of the next completion (~0 = none).
    void run_to(uint64_t now);
    uint64_t next_event() const { return busy_ ? done_ : ~uint64_t(0); }

    bool intrq() const { return intrq_; }
    bool busy() const { return busy_; }
    uint32_t dma_address() const { return dma_addr_; }
    unsigned dma_count() const { return dma_count_; }
    unsigned track_register() const { return track_; }
    unsigned head_track() const { return unsigned(head_); }

private:
    enum class Op { None, TypeI, Read, ReadMulti };
    void command(uint8_t cmd, uint64_t now);
    void finish(uint64_t at);
    void set_intrq(bool on, uint64_t at);
    void begin_sector(uint64_t at);
    uint8_t status_value(uint64_t now);
    uint64_t cycles_ms(uint64_t ms) const;

    const StDisk* disk_ = nullptr;
    uint8_t* ram_ = nullptr;
    uint32_t ram_size_ = 0;
    std::function<void(bool, uint64_t)> irq_cb_;
    int side_ = 0;
    unsigned speed_ = 1;
    bool log_ = false;
    std::vector<ReadRecord> read_log_;
    uint64_t commands_ = 0;

    // WD1772 registers
    uint8_t cmd_ = 0, track_ = 0, sector_ = 1, data_ = 0;
    int head_ = 0;             // the physical head position
    int step_dir_ = 1;         // the direction the last step went (+1 in)
    bool type1_status_ = true; // the status register shows Type I bits (after a Type I command, force-interrupt or reset)
    uint8_t status_ = 0x04;    // the stored status bits (RNF, lost data, ...) of the last command
    bool intrq_ = false;
    bool busy_ = false;
    Op op_ = Op::None;
    uint64_t done_ = 0;        // the cycle the pending step completes at
    uint64_t xfer_start_ = 0;  // the sector transfer window the visible DMA address follows
    uint32_t xfer_addr_ = 0;
    bool xfer_active_ = false;
    uint64_t motor_until_ = 0;
    // DMA chip
    uint16_t dma_mode_ = 0;
    uint32_t dma_addr_ = 0;
    unsigned dma_count_ = 0;
};

} // namespace x2::st
