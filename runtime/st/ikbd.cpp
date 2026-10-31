// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/ikbd.h"

#include <algorithm>

namespace x2::st {

namespace {
constexpr uint64_t kNever = ~uint64_t(0);
constexpr uint64_t kResetDelay = 8021247 / 5; // the IKBD's self test takes a moment before $F0 comes back

// Parameter bytes that follow a command byte.
int params_for(int cmd) {
    switch (cmd) {
    case 0x07: return 1;
    case 0x09: return 4;
    case 0x0A: case 0x0B: case 0x0C: return 2;
    case 0x0E: return 5;
    case 0x17: return 1;
    case 0x19: return 6;
    case 0x1B: return 6;
    case 0x20: return 3;
    case 0x21: return 2;
    case 0x22: return 2;
    case 0x80: return 1;
    default: return 0;
    }
}
} // namespace

void Ikbd::reset(uint64_t now) {
    tx_to_cpu_.clear();
    wire_free_ = now;
    rx_data_ = 0;
    rdrf_ = overrun_ = false;
    control_ = 0;
    tx_busy_until_ = 0;
    cmd_ = -1;
    need_ = 0;
    params_.clear();
    joy_mode_ = 0x14;
    paused_ = false;
    joy_[0] = joy_[1] = 0;
    commands_.clear();
    sent_ = read_by_cpu_ = overruns_ = 0;
    reset_reply_at_ = kNever;
}

void Ikbd::send(const uint8_t* b, size_t n, uint64_t now) {
    for (size_t i = 0; i < n; ++i) {
        const uint64_t arrive = std::max(wire_free_, now) + kIkbdByteCycles; // each byte takes a byte time on the wire
        wire_free_ = arrive;
        tx_to_cpu_.push_back({arrive, b[i]});
    }
    sent_ += n;
}

void Ikbd::run_to(uint64_t now) {
    if (reset_reply_at_ <= now) {
        const uint64_t at = reset_reply_at_;
        reset_reply_at_ = kNever;
        const uint8_t r = 0xF0;
        send(&r, 1, at);
    }
    // A byte reaches the receive register when it has been clocked in and the register is free.
    while (!tx_to_cpu_.empty() && !rdrf_ && tx_to_cpu_.front().arrive <= now) {
        rx_data_ = tx_to_cpu_.front().byte;
        tx_to_cpu_.pop_front();
        rdrf_ = true;
    }
}

uint64_t Ikbd::next_event() const {
    uint64_t best = reset_reply_at_;
    if (!tx_to_cpu_.empty() && !rdrf_) best = std::min(best, tx_to_cpu_.front().arrive);
    return best;
}

bool Ikbd::irq() const {
    const bool rx_int = (control_ & 0x80) && (rdrf_ || overrun_);
    return rx_int; // transmit interrupts: the transmitter is never busy for long; not asserted (see status())
}

uint8_t Ikbd::status(uint64_t now) {
    run_to(now);
    uint8_t s = 0;
    if (rdrf_) s |= 0x01;
    if (now >= tx_busy_until_) s |= 0x02; // TDRE
    if (overrun_) s |= 0x20;
    if (irq()) s |= 0x80;
    return s;
}

void Ikbd::write_control(uint8_t v, uint64_t now) {
    run_to(now);
    if ((v & 3) == 3) { // master reset of the ACIA: clears the receive state
        rdrf_ = overrun_ = false;
        control_ = v; // the written configuration is kept
        return;
    }
    control_ = v;
}

uint8_t Ikbd::read_data(uint64_t now) {
    run_to(now);
    const uint8_t v = rx_data_;
    if (rdrf_) {
        ++read_by_cpu_;
        rdrf_ = false;
        overrun_ = false;
        run_to(now); // the next byte, if it has already been clocked in, arrives at once
    }
    return v;
}

void Ikbd::write_data(uint8_t v, uint64_t now) {
    run_to(now);
    tx_busy_until_ = now + kIkbdByteCycles;
    consume_command_byte(v, now);
}

void Ikbd::consume_command_byte(uint8_t v, uint64_t now) {
    if (cmd_ < 0) {
        cmd_ = v;
        need_ = params_for(v);
        params_.clear();
        if (need_ > 0) return;
    } else {
        params_.push_back(v);
        if (int(params_.size()) < need_) return;
    }
    const int cmd = cmd_;
    cmd_ = -1;
    commands_.push_back(uint8_t(cmd));
    switch (cmd) {
    case 0x80: // reset: $80 $01
        if (!params_.empty() && params_[0] == 0x01) {
            joy_mode_ = 0x14;
            paused_ = false;
            joy_[0] = joy_[1] = 0;
            reset_reply_at_ = now + kResetDelay;
        }
        break;
    case 0x11: paused_ = false; break;
    case 0x13: paused_ = true; break;
    case 0x14: case 0x15: case 0x1A: joy_mode_ = uint8_t(cmd); break;
    case 0x16: { // joystick interrogation
        const uint8_t pkt[3] = {0xFD, joy_[0], joy_[1]};
        send(pkt, 3, now);
        break;
    }
    case 0x1B:
        for (size_t i = 0; i < 6 && i < params_.size(); ++i) clock_[i] = params_[i];
        break;
    case 0x1C: {
        const uint8_t pkt[7] = {0xFC, clock_[0], clock_[1], clock_[2], clock_[3], clock_[4], clock_[5]};
        send(pkt, 7, now);
        break;
    }
    default: break; // the mouse commands, status inquiries ($87-$9A) and the rest: accepted, no effect modelled
    }
}

void Ikbd::joystick(int port, uint8_t state, uint64_t now) {
    run_to(now);
    port &= 1;
    state &= 0x8F;
    if (joy_[port] == state) return;
    joy_[port] = state;
    if (joy_mode_ != 0x14 || paused_) return;
    const uint8_t pkt[2] = {uint8_t(0xFE + port), state};
    send(pkt, 2, now);
}

void Ikbd::key(uint8_t scancode, bool released, uint64_t now) {
    run_to(now);
    if (paused_) return;
    const uint8_t code = uint8_t((scancode & 0x7F) | (released ? 0x80 : 0));
    send(&code, 1, now);
}

} // namespace x2::st
