// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's keyboard path: the 6850 ACIA at $FFFC00/$FFFC02 and the HD6301 "IKBD" behind it, at the level the games use.
//
// The link is a 7,812.5 baud serial line (ten bits per byte: 10,267 CPU cycles). Bytes the IKBD sends (key codes,
// joystick packets, command replies) reach the ACIA one byte time apart; the ACIA holds one received byte (RDRF) until the
// CPU reads it, and the next byte waits for that. The ACIA's interrupt request (receive interrupt enabled and RDRF, or a
// transmit interrupt with TDRE) is the MFP's GPIP bit 4, active low; the machine wires it.
//
// IKBD commands understood: reset ($80 $01), the mouse mode commands (accepted, the mouse is not modelled), joystick
// event reporting ($14, the default after reset), interrogation ($15/$16), disable ($1A), monitoring ($17), time-of-day set
// and read ($1B/$1C), resume/pause ($11/$13) and the status inquiries; unknown commands are logged and swallowed with a
// parameter count of 0. Joystick events produce the real packets: $FE (joystick 0) or $FF (joystick 1) followed by a state
// byte (bit 0 up, 1 down, 2 left, 3 right, 7 fire). Keys: the make code, or code | $80 on release.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace x2::st {

constexpr uint64_t kIkbdByteCycles = 10267; // 10 bits at 7812.5 baud on the 8,021,247 Hz CPU clock

class Ikbd {
public:
    Ikbd() { reset(0); }
    void reset(uint64_t now);

    // $FFFC00 (control write / status read) and $FFFC02 (data).
    uint8_t status(uint64_t now);
    void write_control(uint8_t v, uint64_t now);
    uint8_t read_data(uint64_t now);
    void write_data(uint8_t v, uint64_t now);

    // Input from the host.
    void joystick(int port, uint8_t state, uint64_t now); // port 0 or 1; bit 0 up, 1 down, 2 left, 3 right, 7 fire
    void key(uint8_t scancode, bool released, uint64_t now);

    // Events: bytes arriving at the ACIA and the transmitter finishing.
    void run_to(uint64_t now);
    uint64_t next_event() const;
    // The interrupt request line (true = asserted: GPIP bit 4 low).
    bool irq() const;

    // Introspection.
    bool joystick_reporting() const { return joy_mode_ == 0x14; }
    uint64_t bytes_sent() const { return sent_; }
    uint64_t bytes_received_by_cpu() const { return read_by_cpu_; }
    const std::vector<uint8_t>& commands_seen() const { return commands_; }
    uint64_t overruns() const { return overruns_; }

private:
    void send(const uint8_t* b, size_t n, uint64_t now); // queue an atomic packet for the CPU
    void consume_command_byte(uint8_t v, uint64_t now);

    struct Pending {
        uint64_t arrive; // the cycle the byte has been clocked into the ACIA
        uint8_t byte;
    };
    std::deque<Pending> tx_to_cpu_; // bytes the IKBD has sent and the ACIA has not received yet, in order
    uint64_t wire_free_ = 0;        // the cycle the serial line is idle again
    uint8_t rx_data_ = 0;
    bool rdrf_ = false, overrun_ = false;
    uint8_t control_ = 0;
    uint64_t tx_busy_until_ = 0;    // the transmitter register is busy until then (TDRE low)
    // command parser
    int cmd_ = -1, need_ = 0;
    std::vector<uint8_t> params_;
    uint8_t joy_mode_ = 0x14; // 0x14 events, 0x15 interrogation, 0x1A disabled
    bool paused_ = false;
    uint8_t joy_[2] = {0, 0};
    std::vector<uint8_t> commands_;
    uint64_t sent_ = 0, read_by_cpu_ = 0, overruns_ = 0;
    uint64_t reset_reply_at_ = ~uint64_t(0);
    uint8_t clock_[6] = {0x92, 0x01, 0x01, 0x00, 0x00, 0x00}; // BCD: yy mm dd hh mm ss
};

} // namespace x2::st
