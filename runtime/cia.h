// MOS 8520 CIA model (Amiga CIA-A / CIA-B).
// Clocked by the E clock (CPU clock / 10 = 709379 Hz on PAL). Timers A/B, TOD, serial port (SDR) and the
// interrupt control register are modelled; parallel ports are plain latches combined with an "external
// input" value supplied by the machine (fire buttons, disk status lines).
#pragma once
#include <cstdint>

namespace x2 {

class Cia {
public:
    enum Reg { PRA, PRB, DDRA, DDRB, TALO, TAHI, TBLO, TBHI, TODLO, TODMID, TODHI, UNUSED, SDR, ICR, CRA, CRB };
    enum Icr : uint8_t { ICR_TA = 0x01, ICR_TB = 0x02, ICR_ALRM = 0x04, ICR_SP = 0x08, ICR_FLG = 0x10 };

    void reset();
    uint8_t read(int reg);
    void write(int reg, uint8_t v);

    // Advance the timers by n E-clock ticks.
    void tick(uint32_t n);
    // One pulse on the TOD input (CIA-A: 50 Hz vsync, CIA-B: hsync).
    void tod_pulse();
    // Serial input byte arrives (keyboard -> CIA-A SDR). Sets ICR SP.
    void serial_in(uint8_t byte);

    // True while the CIA drives its /IRQ line (an enabled ICR source is pending).
    bool irq() const { return (icr_data_ & icr_mask_ & 0x1F) != 0; }

    // Bits read back from the port pins for lines configured as inputs.
    uint8_t ext_pra = 0xFF;
    uint8_t ext_prb = 0xFF;

    uint8_t cra() const { return cra_; }
    uint8_t crb() const { return crb_; }
    uint16_t timer_a() const { return ta_; }
    uint16_t timer_b() const { return tb_; }
    uint8_t pra_out() const { return pra_; }
    uint8_t prb_out() const { return prb_; }
    uint8_t ddra() const { return ddra_; }
    uint8_t ddrb() const { return ddrb_; }

private:
    uint32_t timer_underflows(uint16_t& counter, uint16_t latch, uint8_t& cr, uint32_t ticks);

    uint8_t pra_ = 0, prb_ = 0, ddra_ = 0, ddrb_ = 0;
    uint16_t ta_ = 0xFFFF, tb_ = 0xFFFF, ta_latch_ = 0xFFFF, tb_latch_ = 0xFFFF;
    uint8_t cra_ = 0, crb_ = 0;
    uint8_t sdr_ = 0;
    uint8_t icr_data_ = 0, icr_mask_ = 0;
    uint32_t tod_ = 0, tod_alarm_ = 0, tod_latch_ = 0;
    bool tod_latched_ = false, tod_stopped_ = false;
};

} // namespace x2
