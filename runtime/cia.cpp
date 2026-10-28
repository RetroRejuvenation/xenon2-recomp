#include "cia.h"

namespace x2 {

void Cia::reset() {
    *this = Cia{};
}

uint8_t Cia::read(int reg) {
    switch (reg & 15) {
    case PRA: return (pra_ & ddra_) | (ext_pra & ~ddra_);
    case PRB: return (prb_ & ddrb_) | (ext_prb & ~ddrb_);
    case DDRA: return ddra_;
    case DDRB: return ddrb_;
    case TALO: return ta_ & 0xFF;
    case TAHI: return ta_ >> 8;
    case TBLO: return tb_ & 0xFF;
    case TBHI: return tb_ >> 8;
    case TODLO: {
        uint32_t v = tod_latched_ ? tod_latch_ : tod_;
        tod_latched_ = false;
        return v & 0xFF;
    }
    case TODMID: return ((tod_latched_ ? tod_latch_ : tod_) >> 8) & 0xFF;
    case TODHI:
        // Reading the high byte latches the counter until the low byte is read.
        tod_latch_ = tod_;
        tod_latched_ = true;
        return (tod_ >> 16) & 0xFF;
    case SDR: return sdr_;
    case ICR: {
        uint8_t v = icr_data_ | (irq() ? 0x80 : 0);
        icr_data_ = 0;
        return v;
    }
    case CRA: return cra_;
    case CRB: return crb_;
    default: return 0xFF;
    }
}

void Cia::write(int reg, uint8_t v) {
    switch (reg & 15) {
    case PRA: pra_ = v; break;
    case PRB: prb_ = v; break;
    case DDRA: ddra_ = v; break;
    case DDRB: ddrb_ = v; break;
    case TALO: ta_latch_ = (ta_latch_ & 0xFF00) | v; break;
    case TAHI:
        ta_latch_ = (ta_latch_ & 0x00FF) | (v << 8);
        // Writing the high byte loads the counter when the timer is stopped; one-shot mode also starts it.
        if (!(cra_ & 0x01)) ta_ = ta_latch_;
        if (cra_ & 0x08) { ta_ = ta_latch_; cra_ |= 0x01; }
        break;
    case TBLO: tb_latch_ = (tb_latch_ & 0xFF00) | v; break;
    case TBHI:
        tb_latch_ = (tb_latch_ & 0x00FF) | (v << 8);
        if (!(crb_ & 0x01)) tb_ = tb_latch_;
        if (crb_ & 0x08) { tb_ = tb_latch_; crb_ |= 0x01; }
        break;
    case TODLO:
        if (crb_ & 0x80) tod_alarm_ = (tod_alarm_ & 0xFFFF00) | v;
        else { tod_ = (tod_ & 0xFFFF00) | v; tod_stopped_ = false; }
        break;
    case TODMID:
        if (crb_ & 0x80) tod_alarm_ = (tod_alarm_ & 0xFF00FF) | (v << 8);
        else tod_ = (tod_ & 0xFF00FF) | (v << 8);
        break;
    case TODHI:
        if (crb_ & 0x80) tod_alarm_ = (tod_alarm_ & 0x00FFFF) | (v << 16);
        else { tod_ = (tod_ & 0x00FFFF) | (v << 16); tod_stopped_ = true; }
        break;
    case SDR: sdr_ = v; break;
    case ICR:
        if (v & 0x80) icr_mask_ |= v & 0x1F;
        else icr_mask_ &= ~v & 0x1F;
        break;
    case CRA:
        if (v & 0x10) { ta_ = ta_latch_; v &= ~0x10; } // force load strobe
        cra_ = v;
        break;
    case CRB:
        if (v & 0x10) { tb_ = tb_latch_; v &= ~0x10; }
        crb_ = v;
        break;
    default: break;
    }
}

// Count `ticks` down on a running timer and return how many times it underflowed.
uint32_t Cia::timer_underflows(uint16_t& counter, uint16_t latch, uint8_t& cr, uint32_t ticks) {
    uint32_t n = 0;
    while (ticks && (cr & 0x01)) {
        uint32_t to_zero = uint32_t(counter) + 1; // underflow happens on the tick after reaching 0
        if (ticks < to_zero) { counter = uint16_t(counter - ticks); break; }
        ticks -= to_zero;
        counter = latch;
        ++n;
        if (cr & 0x08) cr &= ~0x01; // one-shot: stop
        if (latch == 0xFFFF && ticks > 0x10000) { n += ticks / 0x10000; ticks %= 0x10000; }
    }
    return n;
}

void Cia::tick(uint32_t n) {
    // Timer A counts E clocks when INMODE (CRA bit 5) = 0.
    if ((cra_ & 0x01) && !(cra_ & 0x20)) {
        uint32_t u = timer_underflows(ta_, ta_latch_, cra_, n);
        if (u) {
            icr_data_ |= ICR_TA;
            // Timer B in INMODE 10 counts timer A underflows.
            if ((crb_ & 0x61) == 0x41) {
                if (timer_underflows(tb_, tb_latch_, crb_, u)) icr_data_ |= ICR_TB;
            }
        }
    }
    if ((crb_ & 0x01) && !(crb_ & 0x60)) {
        if (timer_underflows(tb_, tb_latch_, crb_, n)) icr_data_ |= ICR_TB;
    }
}

void Cia::tod_pulse() {
    if (tod_stopped_) return;
    tod_ = (tod_ + 1) & 0xFFFFFF;
    if (tod_ == tod_alarm_) icr_data_ |= ICR_ALRM;
}

void Cia::serial_in(uint8_t byte) {
    sdr_ = byte;
    icr_data_ |= ICR_SP;
}

} // namespace x2
