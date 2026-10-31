// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/fdc.h"

#include "st/mfp.h" // kCpuHz

#include <algorithm>
#include <cstring>

namespace x2::st {

namespace {
constexpr uint64_t kSectorMs = 20;      // one sector time: 200 ms a revolution / 10 sectors
constexpr uint64_t kRnfMs = 1000;       // five revolutions before "record not found"
constexpr uint64_t kRevMs = 200;
constexpr uint64_t kMotorMs = 1800;     // the motor keeps running nine revolutions after a command
constexpr unsigned kStepMs[4] = {6, 12, 2, 3};

enum : uint8_t { Busy = 0x01, Index = 0x02, Track0 = 0x04, CrcErr = 0x08, RecNotFound = 0x10, HeadLoaded = 0x20, LostData = 0x04, MotorOn = 0x80 };
} // namespace

uint64_t FdcDma::cycles_ms(uint64_t ms) const { return std::max<uint64_t>(1, ms * kCpuHz / 1000 / speed_); }

void FdcDma::set_intrq(bool on, uint64_t at) {
    if (intrq_ == on) return;
    intrq_ = on;
    if (irq_cb_) irq_cb_(on, at);
}

uint8_t FdcDma::status_value(uint64_t now) {
    uint8_t s = status_;
    if (type1_status_) {
        s = uint8_t(s & ~(Track0 | Index | MotorOn));
        if (head_ == 0) s |= Track0;
        if ((now % cycles_ms(kRevMs)) < cycles_ms(4)) s |= Index; // the index pulse: a few ms every revolution
        if (now < motor_until_ || busy_) s |= MotorOn;
    } else if (!(now < motor_until_ || busy_)) {
        s |= MotorOn; // Types II/III: bit 7 is "not ready" (the motor is not up)
    }
    if (busy_) s |= Busy;
    return s;
}

void FdcDma::command(uint8_t cmd, uint64_t now) {
    ++commands_;
    cmd_ = cmd;
    if ((cmd & 0xF0) == 0xD0) { // force interrupt: ends the command in progress
        busy_ = false;
        op_ = Op::None;
        xfer_active_ = false;
        type1_status_ = true;
        status_ = uint8_t(status_ & HeadLoaded);
        if (cmd & 0x08) set_intrq(true, now); // immediate interrupt; D0 terminates without one
        else set_intrq(false, now);
        return;
    }
    if (busy_) return; // the WD1772 ignores a command while busy (but for force interrupt)
    set_intrq(false, now);
    motor_until_ = now + cycles_ms(kMotorMs);
    if ((cmd & 0x80) == 0) { // Type I
        type1_status_ = true;
        status_ = (cmd & 0x08) ? HeadLoaded : 0;
        const uint64_t step = kStepMs[cmd & 3];
        uint64_t steps = 0;
        switch (cmd >> 4) {
        case 0x0: steps = unsigned(head_); head_ = 0; track_ = 0; break;                    // restore
        case 0x1: {                                                                         // seek to the data register
            steps = track_ > data_ ? track_ - data_ : data_ - track_;
            head_ = std::clamp(head_ + int(data_) - int(track_), 0, disk_ ? disk_->tracks - 1 : 79);
            track_ = data_;
            break;
        }
        case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: {                       // step, step in, step out
            if ((cmd >> 4) >= 0x6) step_dir_ = -1;
            else if ((cmd >> 4) >= 0x4) step_dir_ = 1; // 0x2/0x3 repeat the last direction
            head_ = std::clamp(head_ + step_dir_, 0, disk_ ? disk_->tracks - 1 : 79);
            if (cmd & 0x10) track_ = uint8_t(std::clamp(int(track_) + step_dir_, 0, 255));
            steps = 1;
            break;
        }
        }
        uint64_t t = std::max<uint64_t>(1, steps) * step;
        if (cmd & 0x04) t += 15; // verify: the head settling time
        done_ = now + cycles_ms(t);
        busy_ = true;
        op_ = Op::TypeI;
        return;
    }
    if ((cmd & 0xE0) == 0x80) { // Type II read sector, 0x90 = multiple
        type1_status_ = false;
        status_ = 0;
        op_ = (cmd & 0x10) ? Op::ReadMulti : Op::Read;
        busy_ = true;
        begin_sector(now);
        return;
    }
    // write sector / write track (write protected), read address / read track (not modelled): fail promptly.
    type1_status_ = false;
    status_ = (cmd & 0xE0) == 0xA0 || (cmd & 0xF0) == 0xF0 ? 0x40 /*write protect*/ : RecNotFound;
    done_ = now + cycles_ms(1);
    busy_ = true;
    op_ = Op::None;
}

void FdcDma::begin_sector(uint64_t at) {
    const bool ok = disk_ && head_ == track_ && sector_ >= 1 && sector_ <= disk_->sectors_per_track &&
                    disk_->sector(head_, std::min(side_, disk_->sides - 1), sector_) != nullptr;
    if (!ok) { // the ID field never matches: five revolutions, then "record not found"
        status_ = RecNotFound;
        done_ = at + cycles_ms(kRnfMs);
        op_ = Op::None;
        xfer_active_ = false;
        return;
    }
    xfer_start_ = at;
    xfer_addr_ = dma_addr_;
    xfer_active_ = true;
    done_ = at + cycles_ms(kSectorMs);
}

void FdcDma::finish(uint64_t at) {
    if (op_ == Op::TypeI || op_ == Op::None) {
        busy_ = false;
        xfer_active_ = false;
        set_intrq(true, at);
        return;
    }
    // A sector has been read.
    const int side = std::min(side_, disk_->sides - 1);
    const uint8_t* src = disk_->sector(head_, side, sector_);
    const bool store = dma_count_ > 0 && ram_ && (dma_addr_ & 0xFFFFFF) + 512 <= ram_size_;
    if (store) {
        std::memcpy(ram_ + (dma_addr_ & 0xFFFFFF), src, 512);
    }
    if (log_) read_log_.push_back({at, uint8_t(head_), uint8_t(side), uint8_t(sector_), dma_addr_, store});
    if (dma_count_ > 0) {
        dma_addr_ += 512;
        --dma_count_;
    } else {
        status_ |= LostData; // the controller had a byte and nobody to take it
    }
    xfer_active_ = false;
    if (op_ == Op::Read) {
        ++sector_;
        busy_ = false;
        set_intrq(true, at);
        return;
    }
    ++sector_; // multi-sector: carry on with the next record
    begin_sector(at);
    if (op_ == Op::None) { /* record not found ends it at done_ */ }
}

void FdcDma::run_to(uint64_t now) {
    while (busy_ && done_ <= now) {
        const uint64_t at = done_;
        if (op_ == Op::None && (status_ == RecNotFound || status_ == 0x40) && !xfer_active_) {
            busy_ = false;
            set_intrq(true, at);
            continue;
        }
        finish(at);
    }
}

uint16_t FdcDma::read16(unsigned off, uint64_t now) {
    run_to(now);
    switch (off) {
    case 0x04: {
        if (dma_mode_ & 0x08) return 0; // the hard-disk controller side: nothing attached
        if (dma_mode_ & 0x10) return uint16_t(dma_count_);
        switch ((dma_mode_ >> 1) & 3) {
        case 0: {
            const uint8_t s = status_value(now);
            set_intrq(false, now); // a status read clears INTRQ
            return s;
        }
        case 1: return track_;
        case 2: return sector_;
        default: return data_;
        }
    }
    case 0x06: return uint16_t(0x01 | (dma_count_ ? 0x02 : 0)); // DMA status: no error, sector count non-zero
    }
    return 0xFFFF;
}

void FdcDma::write16(unsigned off, uint16_t v, uint64_t now) {
    run_to(now);
    switch (off) {
    case 0x04:
        if (dma_mode_ & 0x08) return;
        if (dma_mode_ & 0x10) { dma_count_ = v & 0xFF; return; }
        switch ((dma_mode_ >> 1) & 3) {
        case 0: command(uint8_t(v), now); break;
        case 1: if (!busy_) track_ = uint8_t(v); break;
        case 2: if (!busy_) sector_ = uint8_t(v); break;
        default: data_ = uint8_t(v); break;
        }
        break;
    case 0x06: dma_mode_ = v; break;
    }
}

uint8_t FdcDma::read8(unsigned off, uint64_t now) {
    run_to(now);
    switch (off) {
    case 0x09: case 0x0B: case 0x0D: {
        uint32_t a = dma_addr_;
        if (busy_ && xfer_active_) { // the address a transfer in flight has reached, in 16-byte steps
            const uint64_t len = done_ - xfer_start_;
            const uint64_t el = now > xfer_start_ ? now - xfer_start_ : 0;
            const uint32_t progress = len ? uint32_t(std::min<uint64_t>(512, el * 512 / len)) & ~15u : 0;
            a = xfer_addr_ + progress;
        }
        return off == 0x09 ? uint8_t(a >> 16) : off == 0x0B ? uint8_t(a >> 8) : uint8_t(a & 0xFE);
    }
    case 0x04: case 0x06: return 0;                                  // the high byte of the word registers
    case 0x05: return uint8_t(read16(0x04, now));
    case 0x07: return uint8_t(read16(0x06, now));
    }
    return 0xFF;
}

void FdcDma::write8(unsigned off, uint8_t v, uint64_t now) {
    run_to(now);
    switch (off) {
    case 0x09: dma_addr_ = (dma_addr_ & 0x00FFFF) | (uint32_t(v) << 16); break;
    case 0x0B: dma_addr_ = (dma_addr_ & 0xFF00FF) | (uint32_t(v) << 8); break;
    case 0x0D: dma_addr_ = (dma_addr_ & 0xFFFF00) | (v & 0xFE); break;
    case 0x05: write16(0x04, v, now); break;
    case 0x07: write16(0x06, v, now); break;
    }
}

} // namespace x2::st
