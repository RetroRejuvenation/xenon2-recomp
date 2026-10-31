// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// stemu: a minimal Atari ST for running the Xenon 2 ST boot loaders in an analysis session (docs/re-notes/atari-st-analysis.md).
// Flat 16 MB memory, the 68000 core from vendor/musashi with the trace exception enabled (tools/acq/st_m68kconf.h), and
// just enough hardware for the loaders: the WD1772 floppy controller with its DMA (restore, seek, step, read sector,
// force interrupt; poll-based timing, so a sector can be reported slow or with a bad CRC, which is what the Copylock
// ST check looks for), the MFP general-purpose input, a Shifter video counter, the keyboard ACIA status bit, a level-4
// vertical-blank interrupt and the YM2149 (register writes are logged for tools/acq/ymrender.py). It runs the boot
// sector of a raw .st image the way TOS does and dumps memory ranges when it stops. It is NOT a faithful ST (no cycle
// timing, no video output, no sound output, no blitter). Where a loader's check is bypassed rather than satisfied, the
// bypass is a named --ret / --copy / --jump hook on the command line, so it is always visible.
//
// build (after `cmake --build build` has produced build/musashi_gen/m68kops.c):
//   cc -O1 -w -DMUSASHI_CNF='"st_m68kconf.h"' -Itools/acq -Ivendor/musashi -Ibuild/musashi_gen \
//      tools/acq/stemu.c vendor/musashi/m68kcpu.c build/musashi_gen/m68kops.c runtime/musashi_no_fpu.c -o stemu
// usage: stemu DISK.st [options]
//   --sides N --spt N          geometry (default 1 side, 10 sectors per track)
//   --boot-at ADDR             where the boot sector is loaded and run (default 0x10000)
//   --ret PC:D0                at PC pretend the subroutine ran: D0=value, return to the caller
//   --retseq PC:V1,V2,...      like --ret, returning the values in turn (cycling)
//   --jump PC:NEWPC            at PC continue at NEWPC
//   --copy PC:ADDR:FILE:NEWPC  at PC copy FILE to ADDR and continue at NEWPC
//   --set32 PC:ADDR:VALUE[:AFTER] at PC (once INSTRUCTION COUNT >= AFTER) write the 32-bit VALUE at ADDR and carry on (analysis probe)
//   --stop PC                  stop when PC is reached (may repeat)
//   --max N                    instruction cap (default 400000000)
//   --dump FILE:ADDR:LEN       write memory on exit (may repeat)
//   --fdc-log                  log every FDC command and DMA transfer on stderr
//   --ring                     print the last 64 PCs when the run ends
//   --io-summary               at exit, list every ST I/O register touched (reads, writes, first PCs), writes below $600 and TRAPs executed
//   --ym-log FILE              log every YM2149 register write as 'instruction-count register value' (for rendering the music)
//   --vbl N                    raise the level-4 (vertical blank) interrupt every N instructions
//   --pc-hist FILE             write the distinct PCs that were executed
//   --watch LO:HI              log every write that touches [LO, HI) on stderr as 'insn pc=.. wN [addr]=value'
//   --reg NAME=VALUE           initial register (d0-d7, a0-a6) when the boot sector starts, as TOS would leave it (may repeat)
//   --itrace FILE:LO:HI:N[:FROM]  log pc, the 8 bytes fetched at pc (plain under trace decryption), sr, d0/d1/a0/a1/a7 of up to N instructions with LO <= pc < HI (analysis aid)
//   --bad-sector TRK:SEC       report a CRC error (status 0x08) for this sector
//   --slow-sector TRK:SEC:PCT  make this sector's DMA transfer PCT percent slower (the Copylock timing check)
//   --st                      S0 mode (docs/re-notes/atari-st-s0.md): CPU-cycle timing (160,256 cycles per frame, VBL from cycles),
//                              an MFP 68901 (timers A-D in delay mode, IER/IPR/ISR/IMR/VR, priorities, GPI4 = keyboard ACIA), the keyboard
//                              ACIA with an IKBD (joystick event/interrogation, key codes), a cycle-based video counter with the base latched
//                              at VBL, multi-sector FDC reads. Without --st nothing below applies and the behaviour is the old one.
//   --frames N                 (--st) stop after N VBLs
//   --ikbd FILE                (--st) input script, lines 'FRAME joy PORT HEX' | 'FRAME key HEX' | 'FRAME brk HEX' | 'FRAME tap HEX N' |
//                              'FRAME jtap PORT HEX N' (joystick state bit7 fire, bits 3..0 right,left,down,up; PORT 0 or 1)
//   --shot FRAME:FILE          (--st) at VBL FRAME write the displayed screen: 'STSHOT' + base + 32000 screen bytes + 32 palette bytes (may repeat;
//                              tools/acq/stshot.py converts it to PNG)
//   --shot-every N:PREFIX      (--st) a shot every N VBLs as PREFIXnnnnn.stshot
//   --dump-frame F:FILE:ADDR:LEN (--st) write memory at VBL F (may repeat)
//   --frame-log FILE           (--st) per VBL: frame, pc, $404, $406, $40A, $410, $41C, busy cycles, FDC sectors, YM writes, timer A fires
//   --idle LO:HI               (--st) cycles with pc in [LO,HI) count as idle (frame pacing wait) in the frame log
//   --prof FILE                (--st) cycles per 64-byte pc bucket at exit
//   --wr-pcs LO:HI:FILE        write count and bytes per writing pc for RAM writes into [LO,HI) at exit
//   --excwatch                 log executions of any exception/TRAP vector target (bus/address error, illegal, TRAP...) and unmapped or odd accesses
//   --fdc-multi                multi-sector read (command bit 4) transfers up to the DMA sector count (implied by --st)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

#define MEMSIZE (1u << 24)
static uint8_t mem[MEMSIZE];
static uint8_t* disk;
static size_t disk_len;
static int sides = 1, spt = 10;

// ---------------------------------------------------------------- hooks
enum { H_RET, H_JUMP, H_COPY, H_RETSEQ, H_SET32 };
struct hook { unsigned pc; int kind; unsigned a, b; uint8_t* data; size_t len; unsigned seq[8]; int nseq, idx; unsigned long after; };
static struct hook hooks[32];
static int nhooks;
static unsigned stops[16];
static int nstops;
static unsigned long insns, maxi = 400000000UL;
static int stopped;
static int fdc_log;
static int st_mode, fdc_multi_on;   // tentative definitions: the --st block below needs them earlier
static unsigned frame_no;
static unsigned long fdc_sectors_frame, fdc_sectors_total, ym_writes_frame, ym_writes_total;
static uint8_t* pc_seen;
static int bad_trk = -1, bad_sec = -1;
static unsigned long vbl_period, vbl_next, vbl_count;
static FILE* ym_log;
static unsigned ring[256], ring_pos;
// --io-summary: what the program does to the ST hardware (docs/re-notes/atari-st-requirements.md). Off by default.
static int io_summary;
struct ioent { unsigned addr; unsigned long rd, wr; unsigned first_pc_rd, first_pc_wr; unsigned lastv; };
static struct ioent ioents[4096]; static int nio;
static unsigned cur_pc;
static void io_note(unsigned a, int wr, unsigned v) {
    if (!io_summary) return;
    for (int i = 0; i < nio; i++) if (ioents[i].addr == a) { if (wr) { if (!ioents[i].wr) ioents[i].first_pc_wr = cur_pc; ioents[i].wr++; ioents[i].lastv = v; } else { if (!ioents[i].rd) ioents[i].first_pc_rd = cur_pc; ioents[i].rd++; } return; }
    if (nio < 4096) { ioents[nio].addr = a; if (wr) { ioents[nio].wr = 1; ioents[nio].first_pc_wr = cur_pc; ioents[nio].lastv = v; } else { ioents[nio].rd = 1; ioents[nio].first_pc_rd = cur_pc; } nio++; }
}
struct lowent { unsigned addr, val, pc; unsigned long n; }; static struct lowent lows[2048]; static int nlow;
static void low_note(unsigned a, unsigned v) {
    if (!io_summary) return;
    for (int i = 0; i < nlow; i++) if (lows[i].addr == a) { lows[i].n++; lows[i].val = v; return; }
    if (nlow < 2048) { lows[nlow].addr = a; lows[nlow].val = v; lows[nlow].pc = cur_pc; lows[nlow].n = 1; nlow++; }
}
struct trapent { int n; unsigned fn; unsigned long c; unsigned pc; }; static struct trapent traps[512]; static int ntrap;
static void trap_note(int n, unsigned fn, unsigned pc) {
    for (int i = 0; i < ntrap; i++) if (traps[i].n == n && traps[i].fn == fn) { traps[i].c++; return; }
    if (ntrap < 512) { traps[ntrap].n = n; traps[ntrap].fn = fn; traps[ntrap].c = 1; traps[ntrap].pc = pc; ntrap++; }
}
static unsigned long irq4s;
static struct { int reg; unsigned val; } init_regs[16]; static int n_init_regs;
static int ring_on;
static unsigned watch_lo = 1, watch_hi; static FILE* watch_log;
static FILE* itrace; static unsigned itr_lo, itr_hi; static unsigned long itr_left, itr_from;
// --itrace logs each distinct (pc, fetched 8 bytes) at most ITR_REPEAT times, so delay loops do not fill the log.
#define ITR_REPEAT 3
#define ITR_SLOTS (1u << 22)
static struct { unsigned long long key; unsigned char n; } *itr_tab;
static int itr_seen_too_often(unsigned pc, const uint8_t* p) {
    unsigned long long k = ((unsigned long long)pc << 40) ^ ((unsigned long long)p[0] << 32) ^ ((unsigned long long)p[1] << 24) ^
                           ((unsigned long long)p[2] << 16) ^ ((unsigned long long)p[3] << 8) ^ p[4] ^ ((unsigned long long)p[5] << 48) ^
                           ((unsigned long long)p[6] << 56) ^ ((unsigned long long)p[7] << 4);
    k |= 1;
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 42) & (ITR_SLOTS - 1);
    for (unsigned i = 0; i < ITR_SLOTS; i++, h = (h + 1) & (ITR_SLOTS - 1)) {
        if (!itr_tab[h].key) { itr_tab[h].key = k; itr_tab[h].n = 1; return 0; }
        if (itr_tab[h].key == k) { if (itr_tab[h].n >= ITR_REPEAT) return 1; itr_tab[h].n++; return 0; }
    }
    return 1;
}

// ---------------------------------------------------------------- FDC / DMA
static unsigned dma_addr, dma_mode, dma_count;
static int fdc_track, fdc_sector, fdc_data, fdc_status, fdc_head;
static unsigned ym_sel, ym_porta = 0xFF;
// Poll clock: the loaders time the FDC by counting loop iterations that read the DMA address or the MFP input, so a
// read command completes after a number of those reads instead of after real time.
static int fdc_busy;            // a read command is in flight
static unsigned fdc_polls;      // reads of the DMA address / MFP GPIP since the command was issued
static unsigned fdc_xfer_len = 1000;  // polls the data transfer lasts (the protected sector can be made slower)
static unsigned fdc_lead = 40;  // polls before the first data arrives
static unsigned pend_addr, pend_sector, pend_head, pend_side;
static int pend_bad, pend_have, pend_multi; static unsigned pend_nsec = 1;
static int slow_trk = -1, slow_sec = -1; static unsigned slow_len = 1100;

static size_t sector_offset(int trk, int side, int sec) {  // sec 1..spt
    return (((size_t)trk * sides + side) * spt + (sec - 1)) * 512;
}

static void fdc_command(int cmd) {
    int type1 = (cmd & 0x80) == 0;
    if (fdc_log) fprintf(stderr, "[fdc] cmd %02x track=%d sector=%d data=%d head=%d dma=%06x count=%u\n", cmd, fdc_track, fdc_sector, fdc_data, fdc_head, dma_addr, dma_count);
    if (type1) {
        switch (cmd >> 4) {
        case 0: fdc_head = 0; fdc_track = 0; break;                    // restore
        case 1: fdc_head = fdc_data; fdc_track = fdc_data; break;      // seek
        case 2: case 3: break;                                          // step
        case 4: case 5: fdc_head++; if (cmd & 0x10) fdc_track++; break;
        case 6: case 7: if (fdc_head) fdc_head--; if (cmd & 0x10) fdc_track--; break;
        }
        fdc_status = fdc_head == 0 ? 0x04 : 0;
        return;
    }
    if ((cmd & 0xE0) == 0x80) {  // read sector (multiple if bit 4)
        int side = (ym_porta & 1) ? 0 : 1;  // port A bit 0 low selects side 1
        if (sides == 1) side = 0;
        int multi = cmd & 0x10;
        int domulti = multi && (st_mode || fdc_multi_on);
        if (multi && !domulti) { fprintf(stderr, "[fdc] multi-sector read not modelled in poll mode (cmd %02x)\n", cmd); }
        fdc_status = 0;
        if (fdc_track != fdc_head || fdc_sector < 1 || fdc_sector > spt) { fdc_status = 0x10; return; }  // record not found
        size_t o = sector_offset(fdc_head, side, fdc_sector);
        if (o + 512 > disk_len) { fdc_status = 0x10; return; }
        fdc_busy = 1; fdc_polls = 0;
        pend_addr = dma_addr; pend_sector = fdc_sector; pend_head = fdc_head; pend_side = side;
        pend_bad = (fdc_head == bad_trk && fdc_sector == bad_sec);
        fdc_xfer_len = (fdc_head == slow_trk && fdc_sector == slow_sec) ? slow_len : 1000;
        pend_multi = domulti; pend_nsec = 1;
        if (domulti) {   // as many sectors as the DMA chip will accept, up to the end of the track
            unsigned room = (unsigned)(spt - fdc_sector + 1);
            pend_nsec = dma_count ? (dma_count < room ? dma_count : room) : 1;
            fdc_xfer_len *= pend_nsec;
        }
        return;
    }
    if ((cmd & 0xF0) == 0xD0) { fdc_status = fdc_head == 0 ? 0x04 : 0; return; }  // force interrupt
    fprintf(stderr, "[fdc] unsupported command %02x\n", cmd);
    fdc_status = 0x80;
}

static void fdc_complete(void) {
    unsigned nsec = pend_multi ? pend_nsec : 1, sec = pend_sector, addr = pend_addr;
    for (unsigned k = 0; k < nsec; k++, sec++) {
        size_t o = sector_offset(pend_head, pend_side, sec);
        if (dma_count) {
            memcpy(mem + (addr & (MEMSIZE - 1)), disk + o, 512);
            if (fdc_log) fprintf(stderr, "[dma] frame %u trk %u side %u sec %u -> %06x\n", frame_no, pend_head, pend_side, sec, addr);
            addr += 512;
            dma_addr = addr;
            dma_count--;
            fdc_sectors_frame++; fdc_sectors_total++;
        }
    }
    fdc_status = pend_bad ? 0x08 : 0;
    fdc_sector = sec;
    fdc_busy = 0;
}

static void fdc_poll(void) {
    if (!fdc_busy) return;
    fdc_polls++;
    if (fdc_polls >= fdc_lead + fdc_xfer_len) fdc_complete();
}

static unsigned dma_visible(void) {  // the DMA address as the CPU sees it while a transfer is under way
    if (!fdc_busy) return dma_addr;
    if (fdc_polls < fdc_lead) return pend_addr;
    return pend_addr + 16;
}

// ---------------------------------------------------------------- --st: cycle timing, MFP, keyboard ACIA / IKBD, video, logging
static int st_mode, excwatch, fdc_multi_on;
static unsigned long long cyc_base;
#define CPU_HZ 8021247ULL
#define MFP_HZ 2457600ULL
#define FRAME_CYC 160256ULL
static inline unsigned long long now_cyc(void) { return cyc_base + (unsigned)m68k_cycles_run(); }
static unsigned long long frame_start;
static unsigned frame_no, max_frames, latched_base;
static int vbl_pending, irq_dirty = 1;
static unsigned long long last_hook_cyc; static unsigned last_hook_pc;
static unsigned idle_lo, idle_hi; static unsigned long long idle_cyc, idle_cyc_frame_mark, frame_idle;
static unsigned long long* prof; static char* prof_file;
static FILE* frame_log; static unsigned long fdc_sectors_frame, ym_writes_frame, tima_fires_frame;
static unsigned long tima_fires, ym_writes_total, fdc_sectors_total;
static unsigned wrp_lo, wrp_hi; static char* wrp_file; static struct { unsigned pc; unsigned long n, bytes; } wrp[4096]; static int nwrp;
static unsigned unmapped_n, odd_n, exc_n; static unsigned ram_top = 0x80000;

// --- MFP 68901
struct tmr { int run; unsigned prescale, reload; unsigned long long next, start; };
static struct { uint8_t aer, ddr, ier[2], ipr[2], isr[2], imr[2], vr, tacr, tbcr, tcdcr, scr, ucr, rsr, tsr, udr; struct tmr t[4]; } mfp;
static const unsigned prescales[8] = {0, 4, 10, 16, 50, 64, 100, 200};
static unsigned long long tmr_period(const struct tmr* t, unsigned data) { return (unsigned long long)t->prescale * (data ? data : 256) * CPU_HZ / MFP_HZ; }
static void mfp_set_pending(int irq) {   // irq 0..15 (bit 15 = highest priority); pending only if enabled
    unsigned ier = (mfp.ier[0] << 8) | mfp.ier[1];
    if (ier & (1u << irq)) { unsigned ipr = ((mfp.ipr[0] << 8) | mfp.ipr[1]) | (1u << irq); mfp.ipr[0] = ipr >> 8; mfp.ipr[1] = ipr; irq_dirty = 1; }
}
static void tmr_control(int i, unsigned mode) {
    struct tmr* t = &mfp.t[i];
    unsigned ps = (mode & 8) ? 0 : prescales[mode & 7];   // event count / pulse width modes are not modelled
    if (mode & 8) fprintf(stderr, "[mfp] timer %c in event-count/pulse mode (%02x): not modelled\n", 'A' + i, mode);
    if (ps == 0) { t->run = 0; t->prescale = 0; return; }
    if (!t->run || t->prescale != ps) { t->prescale = ps; t->run = 1; t->start = now_cyc(); t->next = t->start + tmr_period(t, t->reload); }
}
static const int tmr_irq[4] = {13, 8, 5, 4};
static void mfp_timers(unsigned long long t) {
    for (int i = 0; i < 4; i++) {
        struct tmr* m = &mfp.t[i];
        while (m->run && t >= m->next) {
            mfp_set_pending(tmr_irq[i]);
            if (i == 0) { tima_fires++; tima_fires_frame++; }
            m->next += tmr_period(m, m->reload);
        }
    }
}
static int mfp_candidate(void) {   // highest-priority deliverable interrupt number, or -1
    unsigned ipr = (mfp.ipr[0] << 8) | mfp.ipr[1], imr = (mfp.imr[0] << 8) | mfp.imr[1], isr = (mfp.isr[0] << 8) | mfp.isr[1];
    unsigned c = ipr & imr;
    for (int i = 15; i >= 0; i--) {
        if (isr & (1u << i)) return -1;          // a higher or equal priority interrupt is in service
        if (c & (1u << i)) return i;
    }
    return -1;
}
static int mfp_ack(void) {
    int i = mfp_candidate();
    if (i < 0) return 24 + 15;   // spurious: the MFP supplies no vector; report the spurious-interrupt vector
    unsigned ipr = (mfp.ipr[0] << 8) | mfp.ipr[1]; ipr &= ~(1u << i); mfp.ipr[0] = ipr >> 8; mfp.ipr[1] = ipr;
    if (mfp.vr & 8) { unsigned isr = ((mfp.isr[0] << 8) | mfp.isr[1]) | (1u << i); mfp.isr[0] = isr >> 8; mfp.isr[1] = isr; }
    irq_dirty = 1;
    return (mfp.vr & 0xF0) | i;
}

// --- keyboard ACIA 6850 and the IKBD
static uint8_t acia_ctrl, acia_rdr; static int acia_rdrf, acia_ovrn, acia_line_prev = 1;
static uint8_t ikbd_q[4096]; static unsigned ikbd_qh, ikbd_qt; static unsigned long long ikbd_next;
static int joy_mode = 1;  // 1 event reporting (the IKBD default), 2 interrogation, 0 disabled
static uint8_t joy_state[2];
static int ik_cmd = -1, ik_need, ik_have; static uint8_t ik_par[8]; static unsigned long tx_bytes, rx_bytes;
static FILE* ikbd_trace;
static int acia_irq(void) { return (acia_ctrl & 0x80) && (acia_rdrf || acia_ovrn); }
static void ik_send(uint8_t b) { if (((ikbd_qt + 1) & 4095) != ikbd_qh) { ikbd_q[ikbd_qt] = b; ikbd_qt = (ikbd_qt + 1) & 4095; } }
static void ik_joy_change(int port, uint8_t st) {
    if (ikbd_trace) fprintf(ikbd_trace, "frame %u joy port %d %02x (mode %d, was %02x)\n", frame_no, port, st, joy_mode, joy_state[port]);
    if (joy_state[port] == st) return;
    joy_state[port] = st;
    if (joy_mode == 1) { ik_send(0xFE + port); ik_send(st); }
}
static const signed char ik_nparam[256] = {   // parameter bytes of the IKBD commands the game could send (others: 0)
    [0x07]=1,[0x09]=4,[0x0A]=2,[0x0B]=2,[0x0C]=2,[0x0E]=5,[0x17]=1,[0x19]=4,[0x1B]=6,[0x80]=1,[0x20]=3,[0x21]=1,[0x22]=3 };
static void ikbd_command(uint8_t b) {
    tx_bytes++;
    if (ikbd_trace) fprintf(ikbd_trace, "frame %u tx %02x\n", frame_no, b);
    if (ik_cmd >= 0) { ik_par[ik_have++] = b; if (ik_have >= ik_need) ik_cmd = -1; return; }
    ik_need = ik_nparam[b]; ik_have = 0;
    if (ik_need) { ik_cmd = b; }
    switch (b) {
    case 0x12: break;                         // disable mouse
    case 0x14: joy_mode = 1; break;           // joystick event reporting
    case 0x15: joy_mode = 2; break;           // joystick interrogation mode
    case 0x16: ik_send(0xFD); ik_send(joy_state[0]); ik_send(joy_state[1]); break;   // interrogate joysticks
    case 0x1A: joy_mode = 0; break;           // disable joysticks
    case 0x80: if (ik_need) ik_cmd = 0x80; break;   // reset (0x80 0x01)
    }
}
static void acia_line_update(void) {   // GPIP bit 4 is the open-collector ACIA IRQ line (low = asserted): a falling edge sets the MFP pending bit
    int line = acia_irq() ? 0 : 1;
    if (acia_line_prev && !line && !(mfp.aer & 0x10)) { mfp_set_pending(6); if (ikbd_trace) fprintf(ikbd_trace, "frame %u acia irq edge (ier %02x%02x imr %02x%02x ipr %02x%02x)\n", frame_no, mfp.ier[0], mfp.ier[1], mfp.imr[0], mfp.imr[1], mfp.ipr[0], mfp.ipr[1]); }
    acia_line_prev = line;
}
static void acia_tick(unsigned long long t) {
    if (!acia_rdrf && ikbd_qh != ikbd_qt && t >= ikbd_next) {
        acia_rdr = ikbd_q[ikbd_qh]; ikbd_qh = (ikbd_qh + 1) & 4095; acia_rdrf = 1; rx_bytes++;
        ikbd_next = t + 10267;   // 10 bits at 7812.5 baud
        if (ikbd_trace) fprintf(ikbd_trace, "frame %u rx %02x\n", frame_no, acia_rdr);
    }
    acia_line_update();
}
static unsigned mfp_gpip(void) {
    fdc_poll();   // the loaders time the controller by counting GPIP reads
    unsigned v = 0x80 | 0x40 | (acia_irq() ? 0 : 0x10) | (fdc_busy ? 0x20 : 0);
    return v;
}

// --- input script
struct ievent { unsigned frame; int kind; unsigned a, b, c; };   // kind 0 joy, 1 key make, 2 key break
static struct ievent* ievents; static int nievents, ievent_idx;
static int ievent_cmp(const void* a, const void* b) { const struct ievent* x = a; const struct ievent* y = b; return x->frame < y->frame ? -1 : x->frame > y->frame; }
static void load_script(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    char line[256]; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned fr, a, b, n; char w[16];
        if (line[0] == '#' || sscanf(line, "%u %15s", &fr, w) != 2) continue;
        if (nievents + 4 >= cap) { cap = cap ? cap * 2 : 256; ievents = realloc(ievents, cap * sizeof *ievents); }
        if (!strcmp(w, "joy") && sscanf(line, "%*u %*s %u %x", &a, &b) == 2) ievents[nievents++] = (struct ievent){fr, 0, a, b, 0};
        else if (!strcmp(w, "key") && sscanf(line, "%*u %*s %x", &a) == 1) ievents[nievents++] = (struct ievent){fr, 1, a, 0, 0};
        else if (!strcmp(w, "brk") && sscanf(line, "%*u %*s %x", &a) == 1) ievents[nievents++] = (struct ievent){fr, 2, a, 0, 0};
        else if (!strcmp(w, "tap") && sscanf(line, "%*u %*s %x %u", &a, &n) == 2) { ievents[nievents++] = (struct ievent){fr, 1, a, 0, 0}; ievents[nievents++] = (struct ievent){fr + n, 2, a, 0, 0}; }
        else if (!strcmp(w, "jtap") && sscanf(line, "%*u %*s %u %x %u", &a, &b, &n) == 3) { ievents[nievents++] = (struct ievent){fr, 0, a, b, 0}; ievents[nievents++] = (struct ievent){fr + n, 0, a, 0, 0}; }
    }
    fclose(f);
    qsort(ievents, nievents, sizeof *ievents, ievent_cmp);
}
static void apply_script(void) {
    while (ievent_idx < nievents && ievents[ievent_idx].frame <= frame_no) {
        struct ievent* e = &ievents[ievent_idx++];
        if (e->kind == 0) ik_joy_change(e->a & 1, (uint8_t)e->b);
        else if (e->kind == 1) ik_send((uint8_t)(e->a & 0x7F));
        else ik_send((uint8_t)((e->a & 0x7F) | 0x80));
    }
}

// --- shots and frame dumps
static struct { unsigned frame; char* file; } shots[64]; static int nshots; static unsigned shot_every; static char* shot_prefix;
static struct { unsigned frame; char* file; unsigned a, n; } fdumps[32]; static int nfdumps;
static void write_shot(const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    fwrite("STSHOT", 1, 6, f);
    uint8_t b[4] = {latched_base >> 24, latched_base >> 16, latched_base >> 8, latched_base}; fwrite(b, 1, 4, f);
    fwrite(mem + (latched_base & 0xFFFFFF), 1, 32000, f);
    fwrite(mem + 0xFF8240, 1, 32, f);
    fclose(f);
}
static void frame_hooks(void) {
    for (int i = 0; i < nshots; i++) if (shots[i].frame == frame_no) write_shot(shots[i].file);
    if (shot_every && frame_no % shot_every == 0) { char p[512]; snprintf(p, sizeof p, "%s%05u.stshot", shot_prefix, frame_no); write_shot(p); }
    for (int i = 0; i < nfdumps; i++) if (fdumps[i].frame == frame_no) { FILE* f = fopen(fdumps[i].file, "wb"); if (f) { fwrite(mem + (fdumps[i].a & 0xFFFFFF), 1, fdumps[i].n, f); fclose(f); } }
    if (frame_log) {
        unsigned long long idle = idle_cyc - idle_cyc_frame_mark; idle_cyc_frame_mark = idle_cyc;
        fprintf(frame_log, "%u pc=%06x v404=%08x v406=%08x v40a=%08x v410=%08x v41c=%08x idle=%llu fdc=%lu ym=%lu tma=%lu base=%06x\n", frame_no, cur_pc,
                (mem[0x404] << 24) | (mem[0x405] << 16) | (mem[0x406] << 8) | mem[0x407], (mem[0x406] << 24) | (mem[0x407] << 16) | (mem[0x408] << 8) | mem[0x409],
                (mem[0x40A] << 24) | (mem[0x40B] << 16) | (mem[0x40C] << 8) | mem[0x40D], (mem[0x410] << 24) | (mem[0x411] << 16) | (mem[0x412] << 8) | mem[0x413],
                (mem[0x41C] << 24) | (mem[0x41D] << 16) | (mem[0x41E] << 8) | mem[0x41F], idle, fdc_sectors_frame, ym_writes_frame, tima_fires_frame, latched_base);
        fdc_sectors_frame = ym_writes_frame = tima_fires_frame = 0;
    }
}
static void vbl_event(void) {
    frame_no++;
    latched_base = ((unsigned)mem[0xFF8201] << 16) | ((unsigned)mem[0xFF8203] << 8);
    vbl_pending = 1; irq_dirty = 1; vbl_count++; irq4s++;
    apply_script();
    frame_hooks();
    if (max_frames && frame_no >= max_frames) { stopped = 1; m68k_end_timeslice(); }
}
static unsigned st_video_counter(void) {   // lines 63..262 show the screen at 160 bytes per line; the base is latched at VBL
    unsigned long long t = now_cyc(); unsigned long long in = t - frame_start;
    unsigned line = (unsigned)(in / 512), x = (unsigned)(in % 512);
    unsigned off = 0;
    if (line >= 63) { unsigned l = line - 63; if (l >= 200) off = 32000; else { unsigned xb = x < 56 ? 0 : (x - 56 > 320 ? 160 : (x - 56) / 2); off = l * 160 + xb; } }
    return (latched_base + (off & ~1u)) & 0xFFFFFF;
}
static void st_sched(void) {   // called from the instruction hook: bring the timers, ACIA, VBL and the interrupt level up to date
    unsigned long long t = now_cyc();
    while (t >= frame_start + FRAME_CYC) { frame_start += FRAME_CYC; vbl_event(); }
    mfp_timers(t); acia_tick(t);
    if (irq_dirty) {
        irq_dirty = 0;
        int lvl = vbl_pending ? 4 : 0;
        if (mfp_candidate() >= 0) lvl = 6;
        m68k_set_irq(lvl);
    }
}

static unsigned video_counter(void) {  // a raster that sweeps the whole mid byte every 16K instructions, so any wait-for-line loop ends
    unsigned base = ((unsigned)mem[0xFF8201] << 16) | ((unsigned)mem[0xFF8203] << 8);
    return (base & 0xFF0000) | (unsigned)(((insns >> 6) & 0xFF) << 8) | (unsigned)(insns & 0xFE);
}

static unsigned io_read8_real(unsigned a);
static unsigned io_read8(unsigned a) { unsigned v = io_read8_real(a); io_note(a, 0, v); return v; }
static unsigned mfp_read(unsigned a) {
    unsigned r = (a - 0xFFFA01) >> 1;
    switch (r) {
    case 0: return mfp_gpip();
    case 1: return mfp.aer; case 2: return mfp.ddr;
    case 3: return mfp.ier[0]; case 4: return mfp.ier[1];
    case 5: return mfp.ipr[0]; case 6: return mfp.ipr[1];
    case 7: return mfp.isr[0]; case 8: return mfp.isr[1];
    case 9: return mfp.imr[0]; case 10: return mfp.imr[1];
    case 11: return mfp.vr; case 12: return mfp.tacr; case 13: return mfp.tbcr; case 14: return mfp.tcdcr;
    case 15: case 16: case 17: case 18: {   // timer data registers: the current count
        struct tmr* t = &mfp.t[r - 15];
        if (!t->run) return t->reload & 0xFF;
        unsigned long long now = now_cyc(), rem = t->next > now ? t->next - now : 0;
        return (unsigned)((rem * MFP_HZ / CPU_HZ) / t->prescale + 1) & 0xFF;
    }
    case 19: return mfp.scr; case 20: return mfp.ucr; case 21: return mfp.rsr; case 22: return mfp.tsr; case 23: return mfp.udr;
    }
    return 0;
}
static void mfp_write(unsigned a, unsigned v) {
    unsigned r = (a - 0xFFFA01) >> 1;
    v &= 0xFF;
    switch (r) {
    case 0: break;                                              // GPIP: inputs only here
    case 1: mfp.aer = v; break; case 2: mfp.ddr = v; break;
    case 3: mfp.ier[0] = v; mfp.ipr[0] &= v; irq_dirty = 1; break;
    case 4: mfp.ier[1] = v; mfp.ipr[1] &= v; irq_dirty = 1; break;
    case 5: mfp.ipr[0] &= v; irq_dirty = 1; break;               // writing 0 clears a pending bit
    case 6: mfp.ipr[1] &= v; irq_dirty = 1; break;
    case 7: mfp.isr[0] &= v; irq_dirty = 1; break;               // software end of interrupt
    case 8: mfp.isr[1] &= v; irq_dirty = 1; break;
    case 9: mfp.imr[0] = v; irq_dirty = 1; break; case 10: mfp.imr[1] = v; irq_dirty = 1; break;
    case 11: mfp.vr = v; break;
    case 12: mfp.tacr = v; tmr_control(0, v & 15); break;
    case 13: mfp.tbcr = v; tmr_control(1, v & 15); break;
    case 14: mfp.tcdcr = v; tmr_control(2, (v >> 4) & 7); tmr_control(3, v & 7); break;
    case 15: case 16: case 17: case 18: mfp.t[r - 15].reload = v; break;
    case 19: mfp.scr = v; break; case 20: mfp.ucr = v; break; case 21: mfp.rsr = v; break; case 22: mfp.tsr = v; break; case 23: mfp.udr = v; break;
    }
}
static unsigned st_io_read8(unsigned a, int* handled) {
    *handled = 1;
    if (a >= 0xFFFA01 && a <= 0xFFFA2F && (a & 1)) return mfp_read(a);
    if (a == 0xFFFC00) { return (acia_rdrf ? 1 : 0) | 2 | (acia_ovrn ? 0x20 : 0) | (acia_irq() ? 0x80 : 0); }
    if (a == 0xFFFC02) { unsigned v = acia_rdr; if (ikbd_trace) fprintf(ikbd_trace, "frame %u read data %02x (rdrf %d) pc=%06x\n", frame_no, v, acia_rdrf, cur_pc); acia_rdrf = 0; acia_ovrn = 0; irq_dirty = 1; acia_line_update(); return v; }
    if (a == 0xFFFC04) return 2;       // MIDI ACIA: nothing connected
    if (a == 0xFFFC06) return 0;
    if (a == 0xFF8205) return (st_video_counter() >> 16) & 0xFF;
    if (a == 0xFF8207) return (st_video_counter() >> 8) & 0xFF;
    if (a == 0xFF8209) return st_video_counter() & 0xFF;
    *handled = 0; return 0;
}
static int st_io_write8(unsigned a, unsigned v) {
    if (a >= 0xFFFA01 && a <= 0xFFFA2F && (a & 1)) { mfp_write(a, v); return 1; }
    if (a == 0xFFFC00) { if ((v & 3) == 3) { acia_rdrf = acia_ovrn = 0; acia_ctrl = v & ~3; } else acia_ctrl = v; irq_dirty = 1; acia_line_update(); return 1; }
    if (a == 0xFFFC02) { ikbd_command((uint8_t)v); return 1; }
    if (a == 0xFFFC04 || a == 0xFFFC06) return 1;
    return 0;
}
static unsigned io_read8_real(unsigned a) {
    if (st_mode) { int h; unsigned v = st_io_read8(a, &h); if (h) return v; }
    switch (a) {
    case 0xFF8205: return (video_counter() >> 16) & 0xFF;
    case 0xFF8207: return (video_counter() >> 8) & 0xFF;
    case 0xFF8209: return video_counter() & 0xFF;
    case 0xFF8260: return 0;                 // low resolution
    case 0xFFFA01: fdc_poll(); return fdc_busy ? 0x20 : 0;  // MFP GPIP bit 5 low = FDC interrupt request (command finished)
    case 0xFF8609: return (dma_visible() >> 16) & 0xFF;
    case 0xFF860B: fdc_poll(); return (dma_visible() >> 8) & 0xFF;
    case 0xFF860D: fdc_poll(); return dma_visible() & 0xFF;
    case 0xFF8800: return ym_sel == 14 ? ym_porta : 0;
    case 0xFFFC00: case 0xFFFC04: return 0x02;   // keyboard / MIDI ACIA status: transmit register empty, nothing received
    case 0xFFFC02: case 0xFFFC06: return 0;
    }
    if (a == 0xFF8604 || a == 0xFF8605) {
        unsigned v = 0;
        if (fdc_busy && !(dma_mode & 0x10) && ((dma_mode >> 1) & 3) == 0) fdc_poll();
        if (dma_mode & 0x10) v = dma_count;
        else switch ((dma_mode >> 1) & 3) {
            case 0: v = fdc_status; break;
            case 1: v = fdc_track; break;
            case 2: v = fdc_sector; break;
            case 3: v = fdc_data; break;
        }
        return a == 0xFF8604 ? 0 : v;
    }
    if (a == 0xFF8606 || a == 0xFF8607) return a == 0xFF8607 ? 0x01 : 0;  // DMA status: OK, no error
    return mem[a];
}

static void io_write8_real(unsigned a, unsigned v);
static void io_write8(unsigned a, unsigned v) { io_note(a, 1, v); io_write8_real(a, v); }
static void io_write8_real(unsigned a, unsigned v) {
    v &= 0xFF;
    if (st_mode && st_io_write8(a, v)) return;
    if (a == 0xFF8609) { dma_addr = (dma_addr & 0x00FFFF) | (v << 16); return; }
    if (a == 0xFF860B) { dma_addr = (dma_addr & 0xFF00FF) | (v << 8); return; }
    if (a == 0xFF860D) { dma_addr = (dma_addr & 0xFFFF00) | v; return; }
    if (a == 0xFF8800) { ym_sel = v; return; }
    if (a == 0xFF8802) {
        if (ym_log && ym_sel < 14) fprintf(ym_log, "%llu %u %u\n", st_mode ? now_cyc() : (unsigned long long)insns, ym_sel, v);
        if (ym_sel < 14) { ym_writes_frame++; ym_writes_total++; }
        if (ym_sel == 14) ym_porta = v;
        return;
    }
    mem[a] = (uint8_t)v;
}

static void io_write16(unsigned a, unsigned v) {
    if (a == 0xFF8606) { dma_mode = v; return; }
    if (a == 0xFF8604) {
        if (dma_mode & 0x10) { dma_count = v & 0xFF; return; }
        switch ((dma_mode >> 1) & 3) {
        case 0: fdc_command(v & 0xFF); break;
        case 1: fdc_track = v & 0xFF; break;
        case 2: fdc_sector = v & 0xFF; break;
        case 3: fdc_data = v & 0xFF; break;
        }
        return;
    }
    io_write8(a, v >> 8);
    io_write8(a + 1, v & 0xFF);
}

#define IS_IO(a) ((a) >= 0xFF8000)

// ---------------------------------------------------------------- memory interface
static void access_check(unsigned a, int n, int wr);
unsigned int m68k_read_memory_8(unsigned a) { a &= 0xFFFFFF; access_check(a, 1, 0); return IS_IO(a) ? io_read8(a) : mem[a]; }
unsigned int m68k_read_memory_16(unsigned a) {
    a &= 0xFFFFFF;
    access_check(a, 2, 0);
    if (IS_IO(a)) return (io_read8(a) << 8) | io_read8(a + 1);
    return (mem[a] << 8) | mem[a + 1];
}
unsigned int m68k_read_memory_32(unsigned a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_16(unsigned a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned a) { return m68k_read_memory_32(a); }
static void watch_write(unsigned a, unsigned v, int n) {
    if (watch_log && a < watch_hi && a + n > watch_lo) fprintf(watch_log, "%lu pc=%06x w%d [%06x]=%0*x\n", insns, cur_pc, n, a, n * 2, n == 4 ? v : v & ((1u << (8 * n)) - 1));
}
static void wrp_note(unsigned a, int n) {
    if (!wrp_file || a < wrp_lo || a >= wrp_hi) return;
    for (int i = 0; i < nwrp; i++) if (wrp[i].pc == cur_pc) { wrp[i].n++; wrp[i].bytes += n; return; }
    if (nwrp < 4096) { wrp[nwrp].pc = cur_pc; wrp[nwrp].n = 1; wrp[nwrp].bytes = n; nwrp++; }
}
static void access_check(unsigned a, int n, int wr) {
    if (!excwatch) return;
    if (a < 0xFF8000 && a >= ram_top && a < 0xFC0000) { unmapped_n++; if (unmapped_n <= 20) fprintf(stderr, "[unmapped] frame %u pc=%06x %s%d at %06x\n", frame_no, cur_pc, wr ? "write" : "read", n * 8, a); }
    if (n > 1 && (a & 1)) { odd_n++; if (odd_n <= 20) fprintf(stderr, "[odd] frame %u pc=%06x %s%d at %06x\n", frame_no, cur_pc, wr ? "write" : "read", n * 8, a); }
}
#define ST_UNMAPPED_W(a) (st_mode && (a) >= ram_top && (a) < 0xFF8000 && !((a) >= 0xFC0000))
void m68k_write_memory_8(unsigned a, unsigned v) { a &= 0xFFFFFF; access_check(a, 1, 1); if (ST_UNMAPPED_W(a)) return; wrp_note(a, 1); watch_write(a, v, 1); if (IS_IO(a)) io_write8(a, v); else { if (a < 0x600) low_note(a, v & 0xff); mem[a] = (uint8_t)v; } }
void m68k_write_memory_16(unsigned a, unsigned v) {
    a &= 0xFFFFFF;
    access_check(a, 2, 1); if (ST_UNMAPPED_W(a)) return; wrp_note(a, 2);
    watch_write(a, v, 2);
    if (IS_IO(a)) { if (a == 0xFF8604 || a == 0xFF8606) io_note(a, 1, v); io_write16(a, v); return; }
    if (a < 0x600) low_note(a, v);
    mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v;
}
void m68k_write_memory_32(unsigned a, unsigned v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v & 0xFFFF); }

static int irq_log;
int st_int_ack(int level) {
    if (irq_log && st_mode) fprintf(stderr, "[irq] frame %u insn %lu level %d (ipr %02x%02x ier %02x%02x imr %02x%02x vr %02x) pc=%06x\n", frame_no, insns, level, mfp.ipr[0], mfp.ipr[1], mfp.ier[0], mfp.ier[1], mfp.imr[0], mfp.imr[1], mfp.vr, cur_pc);
    if (st_mode && level == 6) { int v = mfp_ack(); m68k_set_irq(0); irq_dirty = 1; return v; }
    if (st_mode && level == 4) { vbl_pending = 0; irq_dirty = 1; }
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

static unsigned exc_logged;
void st_instruction_hook(unsigned pc) {
    insns++;
    if (st_mode) {
        unsigned long long t = now_cyc();
        unsigned long long d = t - last_hook_cyc; last_hook_cyc = t;
        if (prof) prof[(last_hook_pc & 0xFFFFFF) >> 6] += d;
        if (idle_hi && last_hook_pc >= idle_lo && last_hook_pc < idle_hi) idle_cyc += d;
        last_hook_pc = pc;
        st_sched();
    }
    if (excwatch) {   // an exception, TRAP or MFP vector target being executed: bus/address error, illegal, line A/F, TRAPs...
        for (unsigned v = 2; v < 64; v++) {
            if (v == 28 || v == 30) continue;   // VBL (level 4) and HBL are normal
            unsigned tgt = (mem[v * 4] << 24) | (mem[v * 4 + 1] << 16) | (mem[v * 4 + 2] << 8) | mem[v * 4 + 3];
            if (tgt && tgt == pc && exc_logged < 40) { exc_logged++; fprintf(stderr, "[exc] frame %u insn %lu: vector %u target %06x executed (previous pc %06x)\n", frame_no, insns, v, pc, cur_pc); }
            if (tgt && tgt == pc) exc_n++;
        }
    }
    cur_pc = pc;
    if (io_summary) {
        unsigned op = (mem[pc & 0xFFFFFF] << 8) | mem[(pc + 1) & 0xFFFFFF];
        if ((op & 0xFFF0) == 0x4E40) trap_note(op & 15, m68k_read_memory_16(m68k_get_reg(NULL, M68K_REG_SP)), pc);
    }
    if (insns >= maxi) { m68k_end_timeslice(); }   // --max is exact: stop before executing this instruction's successor
    if (!st_mode && vbl_period && insns >= vbl_next) { vbl_next = insns + vbl_period; vbl_count++; irq4s++; m68k_set_irq(4); }
    if (ring_on) ring[ring_pos++ & 255] = pc;
    if (itrace && insns >= itr_from && itr_left && pc >= itr_lo && pc < itr_hi && !itr_seen_too_often(pc, mem + pc)) {
        itr_left--;
        fprintf(itrace, "%lu %06x %02x%02x%02x%02x%02x%02x%02x%02x sr=%04x d0=%08x d1=%08x d2=%08x a0=%08x a1=%08x a2=%08x a3=%08x a7=%08x\n", insns, pc,
                mem[pc], mem[pc + 1], mem[pc + 2], mem[pc + 3], mem[pc + 4], mem[pc + 5], mem[pc + 6], mem[pc + 7],
                m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A7));
    }
    if (pc_seen) pc_seen[(pc & 0xFFFFFF) >> 1] = 1;
    for (int i = 0; i < nstops; i++)
        if (pc == stops[i]) { stopped = 1; m68k_end_timeslice(); return; }
    for (int i = 0; i < nhooks; i++) {
        if (hooks[i].pc != pc) continue;
        switch (hooks[i].kind) {
        case H_RET: {
            unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
            unsigned ret = m68k_read_memory_32(sp);
            m68k_set_reg(M68K_REG_D0, hooks[i].a);
            m68k_set_reg(M68K_REG_SP, sp + 4);
            m68k_set_reg(M68K_REG_PC, ret);
            break;
        }
        case H_RETSEQ: {
            unsigned sp = m68k_get_reg(NULL, M68K_REG_SP);
            unsigned ret = m68k_read_memory_32(sp);
            m68k_set_reg(M68K_REG_D0, hooks[i].seq[hooks[i].idx % hooks[i].nseq]);
            hooks[i].idx++;
            m68k_set_reg(M68K_REG_SP, sp + 4);
            m68k_set_reg(M68K_REG_PC, ret);
            break;
        }
        case H_SET32:
            if (insns >= hooks[i].after) {
                unsigned a = hooks[i].a & 0xFFFFFF, v = hooks[i].b;
                mem[a] = v >> 24; mem[a + 1] = v >> 16; mem[a + 2] = v >> 8; mem[a + 3] = v;
            }
            continue;   // do not stop other hooks; the instruction runs normally
        case H_JUMP: m68k_set_reg(M68K_REG_PC, hooks[i].a); break;
        case H_COPY:
            memcpy(mem + hooks[i].a, hooks[i].data, hooks[i].len);
            m68k_set_reg(M68K_REG_PC, hooks[i].b);
            break;
        }
        return;
    }
}

static uint8_t* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(n ? n : 1);
    if (fread(p, 1, n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: stemu DISK.st [options] (see the file header)\n"); return 2; }
    disk = slurp(argv[1], &disk_len);
    unsigned boot_at = 0x10000;
    struct { char* file; unsigned a, n; } dumps[16];
    int ndumps = 0;
    char* pc_hist = NULL;
    for (int i = 2; i < argc; i++) {
        char* a = argv[i];
        if (!strcmp(a, "--sides") && i + 1 < argc) sides = atoi(argv[++i]);
        else if (!strcmp(a, "--spt") && i + 1 < argc) spt = atoi(argv[++i]);
        else if (!strcmp(a, "--boot-at") && i + 1 < argc) boot_at = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(a, "--max") && i + 1 < argc) maxi = strtoul(argv[++i], 0, 0);
        else if (!strcmp(a, "--fdc-log")) fdc_log = 1;
        else if (!strcmp(a, "--io-summary")) io_summary = 1;
        else if (!strcmp(a, "--st")) st_mode = 1;
        else if (!strcmp(a, "--ram-kb") && i + 1 < argc) ram_top = (unsigned)strtoul(argv[++i], 0, 0) * 1024;
        else if (!strcmp(a, "--fdc-multi")) fdc_multi_on = 1;
        else if (!strcmp(a, "--excwatch")) excwatch = 1;
        else if (!strcmp(a, "--irq-log")) irq_log = 1;
        else if (!strcmp(a, "--frames") && i + 1 < argc) max_frames = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(a, "--ikbd") && i + 1 < argc) load_script(argv[++i]);
        else if (!strcmp(a, "--ikbd-trace") && i + 1 < argc) { ikbd_trace = fopen(argv[++i], "w"); if (!ikbd_trace) return 2; }
        else if (!strcmp(a, "--frame-log") && i + 1 < argc) { frame_log = fopen(argv[++i], "w"); if (!frame_log) return 2; }
        else if (!strcmp(a, "--idle") && i + 1 < argc) { char* sp = argv[++i]; idle_lo = (unsigned)strtoul(sp, &sp, 0); idle_hi = (unsigned)strtoul(sp + 1, 0, 0); }
        else if (!strcmp(a, "--prof") && i + 1 < argc) { prof_file = argv[++i]; prof = calloc(MEMSIZE / 64, sizeof *prof); }
        else if (!strcmp(a, "--wr-pcs") && i + 1 < argc) { char* sp = argv[++i]; wrp_lo = (unsigned)strtoul(sp, &sp, 0); wrp_hi = (unsigned)strtoul(sp + 1, &sp, 0); wrp_file = strdup(sp + 1); }
        else if (!strcmp(a, "--shot") && i + 1 < argc && nshots < 64) { char* sp = argv[++i]; shots[nshots].frame = (unsigned)strtoul(sp, &sp, 0); shots[nshots].file = strdup(sp + 1); nshots++; }
        else if (!strcmp(a, "--shot-every") && i + 1 < argc) { char* sp = argv[++i]; shot_every = (unsigned)strtoul(sp, &sp, 0); shot_prefix = strdup(sp + 1); }
        else if (!strcmp(a, "--dump-frame") && i + 1 < argc && nfdumps < 32) {
            char* sp = argv[++i]; fdumps[nfdumps].frame = (unsigned)strtoul(sp, &sp, 0); char* c2 = strchr(sp + 1, ':'); if (!c2) return 2; *c2 = 0;
            fdumps[nfdumps].file = strdup(sp + 1); fdumps[nfdumps].a = (unsigned)strtoul(c2 + 1, &c2, 0); fdumps[nfdumps].n = (unsigned)strtoul(c2 + 1, 0, 0); nfdumps++;
        }
        else if (!strcmp(a, "--ring")) ring_on = 1;
        else if (!strcmp(a, "--ym-log") && i + 1 < argc) { ym_log = fopen(argv[++i], "w"); if (!ym_log) return 2; }
        else if (!strcmp(a, "--vbl") && i + 1 < argc) { vbl_period = strtoul(argv[++i], 0, 0); vbl_next = vbl_period; }
        else if (!strcmp(a, "--slow-sector") && i + 1 < argc) { int p; sscanf(argv[++i], "%d:%d:%d", &slow_trk, &slow_sec, &p); slow_len = 1000 + 1000 * p / 100; }
        else if (!strcmp(a, "--stop") && i + 1 < argc) stops[nstops++] = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(a, "--pc-hist") && i + 1 < argc) pc_hist = argv[++i];
        else if (!strcmp(a, "--set32") && i + 1 < argc) {
            unsigned pc, ad, v; unsigned long af = 0; char* sp = argv[++i];
            pc = (unsigned)strtoul(sp, &sp, 0); ad = (unsigned)strtoul(sp + 1, &sp, 0); v = (unsigned)strtoul(sp + 1, &sp, 0);
            if (*sp == ':') af = strtoul(sp + 1, 0, 0);
            hooks[nhooks++] = (struct hook){.pc = pc, .kind = H_SET32, .a = ad, .b = v, .after = af};
        }
        else if (!strcmp(a, "--watch") && i + 1 < argc) {
            char* sp = argv[++i]; watch_lo = (unsigned)strtoul(sp, &sp, 0); watch_hi = (unsigned)strtoul(sp + 1, 0, 0);
            if (!watch_log) { watch_log = stderr; }
        }
        else if (!strcmp(a, "--reg") && i + 1 < argc) {
            char* r = argv[++i]; char* eq = strchr(r, '='); if (!eq || n_init_regs >= 16) return 2; *eq++ = 0;
            int idx = (r[0] == 'd' ? 0 : r[0] == 'a' ? 8 : -1); if (idx < 0 || r[1] < '0' || r[1] > '7') return 2;
            init_regs[n_init_regs].reg = idx + (r[1] - '0'); init_regs[n_init_regs++].val = (unsigned)strtoul(eq, 0, 0);
        }
        else if (!strcmp(a, "--itrace") && i + 1 < argc) {
            char buf[512]; strncpy(buf, argv[++i], sizeof buf - 1); buf[sizeof buf - 1] = 0;
            char* f = buf; char* c1 = strchr(f, ':'); if (!c1) return 2; *c1++ = 0;
            itr_lo = (unsigned)strtoul(c1, &c1, 0); itr_hi = (unsigned)strtoul(c1 + 1, &c1, 0); itr_left = strtoul(c1 + 1, &c1, 0); if (*c1 == ':') itr_from = strtoul(c1 + 1, 0, 0);
            itrace = fopen(f, "w"); if (!itrace) return 2;
            itr_tab = calloc(ITR_SLOTS, sizeof *itr_tab);
        }
        else if (!strcmp(a, "--bad-sector") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &bad_trk, &bad_sec);
        else if (!strcmp(a, "--ret") && i + 1 < argc) {
            unsigned pc, v;
            if (sscanf(argv[++i], "%i:%i", &pc, &v) != 2) return 2;
            hooks[nhooks++] = (struct hook){.pc = pc, .kind = H_RET, .a = v};
        } else if (!strcmp(a, "--retseq") && i + 1 < argc) {
            char* sp = argv[++i];
            struct hook h = {0};
            h.kind = H_RETSEQ;
            h.pc = (unsigned)strtoul(sp, &sp, 0);
            while (*sp == ':' || *sp == ',') { sp++; h.seq[h.nseq++] = (unsigned)strtoul(sp, &sp, 0); if (h.nseq == 8) break; }
            if (!h.nseq) return 2;
            hooks[nhooks++] = h;
        } else if (!strcmp(a, "--jump") && i + 1 < argc) {
            unsigned pc, v;
            if (sscanf(argv[++i], "%i:%i", &pc, &v) != 2) return 2;
            hooks[nhooks++] = (struct hook){.pc = pc, .kind = H_JUMP, .a = v};
        } else if (!strcmp(a, "--copy") && i + 1 < argc) {
            char buf[512];
            unsigned pc, dst, nxt;
            strncpy(buf, argv[++i], sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            char* p1 = strchr(buf, ':'); if (!p1) return 2; *p1++ = 0;
            char* p2 = strchr(p1, ':'); if (!p2) return 2; *p2++ = 0;
            char* p3 = strrchr(p2, ':'); if (!p3) return 2; *p3++ = 0;
            pc = (unsigned)strtoul(buf, 0, 0); dst = (unsigned)strtoul(p1, 0, 0); nxt = (unsigned)strtoul(p3, 0, 0);
            size_t n; uint8_t* d = slurp(p2, &n);
            hooks[nhooks++] = (struct hook){.pc = pc, .kind = H_COPY, .a = dst, .b = nxt, .data = d, .len = n};
        } else if (!strcmp(a, "--dump") && i + 1 < argc) {
            char* s = argv[++i];
            char* p1 = strrchr(s, ':'); if (!p1) return 2; *p1++ = 0;
            char* p2 = strrchr(s, ':'); if (!p2) return 2; *p2++ = 0;
            dumps[ndumps].file = s; dumps[ndumps].a = (unsigned)strtoul(p2, 0, 0); dumps[ndumps].n = (unsigned)strtoul(p1, 0, 0);
            ndumps++;
        } else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (pc_hist) pc_seen = calloc(MEMSIZE / 2, 1);
    if (disk_len < 512) { fprintf(stderr, "disk too small\n"); return 2; }

    // What TOS leaves behind that the loaders read: boot device, the ROM reset vector, a resting stack.
    memcpy(mem + boot_at, disk, 512);
    mem[0x446] = 0; mem[0x447] = 0;                       // _bootdev = A:
    mem[4] = 0x00; mem[5] = 0xFC; mem[6] = 0x00; mem[7] = 0x20;  // ROM reset vector (TOS 1.0 style)
    mem[1] = 0x01; mem[2] = 0x00; mem[3] = 0x00;          // initial SSP $10000 (overwritten by the boot code's own stack)
    mem[5] = 0xFC;
    // TOS leaves a vertical-blank (level 4) handler in vector $70 until the program installs its own: an rte stub in ROM.
    mem[0xFC0200] = 0x4E; mem[0xFC0201] = 0x73;
    mem[0x70] = 0x00; mem[0x71] = 0xFC; mem[0x72] = 0x02; mem[0x73] = 0x00;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_PC, boot_at);
    m68k_set_reg(M68K_REG_SR, 0x2300);
    m68k_set_reg(M68K_REG_A7, boot_at);
    for (int k = 0; k < n_init_regs; k++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + init_regs[k].reg), init_regs[k].val);
    m68k_set_reg(M68K_REG_A7, boot_at);
    if (st_mode) {
        frame_start = 0;
        // What TOS leaves in the Shifter: the screen at the top of RAM (phystop - $8000), 50 Hz sync, low resolution.
        unsigned vb = ram_top - 0x8000;
        mem[0xFF8201] = vb >> 16; mem[0xFF8203] = vb >> 8; mem[0xFF820A] = 2; mem[0xFF8260] = 0;
        latched_base = vb;
        mfp.vr = 0x48;   // TOS: vector base $40, software end of interrupt
        acia_ctrl = 0x96;   // TOS: keyboard ACIA 8N1, divide by 64, receive interrupt enabled
    }
    while (!stopped && insns < maxi) { int c = m68k_execute(100000); if (c > 0) cyc_base += (unsigned)c; }
    if (getenv("STEMU_REGS")) { fprintf(stderr, "regs:"); for (int r = 0; r < 16; r++) fprintf(stderr, " %c%d=%08x", r < 8 ? 'd' : 'a', r & 7, m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + r))); fprintf(stderr, "\n"); }
    fprintf(stderr, "stopped=%d insns=%lu pc=%06x sr=%04x sp=%06x\n", stopped, insns, m68k_get_reg(NULL, M68K_REG_PC),
            m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_SP));
    if (ring_on) { fprintf(stderr, "last PCs:"); for (int i = 0; i < 64; i++) fprintf(stderr, " %06x", ring[(ring_pos - 64 + i) & 255]); fprintf(stderr, "\n"); }
    for (int i = 0; i < ndumps; i++) {
        FILE* f = fopen(dumps[i].file, "wb");
        if (!f) { perror(dumps[i].file); return 2; }
        fwrite(mem + dumps[i].a, 1, dumps[i].n, f);
        fclose(f);
    }
    if (io_summary) {
        fprintf(stderr, "== IO summary (addr rd wr first-pc-read first-pc-write last-written)\n");
        for (int i = 0; i < nio; i++) fprintf(stderr, "IO %06x rd=%lu wr=%lu rpc=%06x wpc=%06x last=%02x\n", ioents[i].addr, ioents[i].rd, ioents[i].wr, ioents[i].first_pc_rd, ioents[i].first_pc_wr, ioents[i].lastv & 0xffff);
        fprintf(stderr, "== writes below $600 (vectors, system variables; addr last-value first-pc count)\n");
        for (int i = 0; i < nlow; i++) fprintf(stderr, "LOW %03x val=%x pc=%06x n=%lu\n", lows[i].addr, lows[i].val, lows[i].pc, lows[i].n);
        fprintf(stderr, "== TRAP instructions executed (trap, function word on the stack, count, first pc)\n");
        for (int i = 0; i < ntrap; i++) fprintf(stderr, "TRAP #%d fn=%u n=%lu pc=%06x\n", traps[i].n, traps[i].fn, traps[i].c, traps[i].pc);
        fprintf(stderr, "level-4 interrupts raised: %lu\n", irq4s);
    }
    if (st_mode) {
        fprintf(stderr, "== --st: frames=%u cycles=%llu timerA-fires=%lu ikbd tx=%lu rx=%lu fdc-sectors=%lu ym-writes=%lu\n", frame_no, cyc_base, tima_fires, tx_bytes, rx_bytes, fdc_sectors_total, ym_writes_total);
        if (idle_hi) fprintf(stderr, "== idle cycles (pc in [%06x,%06x)): %llu of %llu (%.1f%%)\n", idle_lo, idle_hi, idle_cyc, cyc_base, cyc_base ? 100.0 * idle_cyc / cyc_base : 0.0);
    }
    if (excwatch) fprintf(stderr, "== excwatch: exception-target executions=%u unmapped accesses=%u odd word/long accesses=%u\n", exc_n, unmapped_n, odd_n);
    if (prof && prof_file) {
        FILE* f = fopen(prof_file, "w");
        if (f) { for (unsigned i = 0; i < MEMSIZE / 64; i++) if (prof[i]) fprintf(f, "%06x %llu\n", i * 64, prof[i]); fclose(f); }
    }
    if (wrp_file) {
        FILE* f = fopen(wrp_file, "w");
        if (f) { for (int i = 0; i < nwrp; i++) fprintf(f, "%06x %lu %lu\n", wrp[i].pc, wrp[i].n, wrp[i].bytes); fclose(f); }
    }
    if (frame_log) fclose(frame_log);
    if (ikbd_trace) fclose(ikbd_trace);
    if (ym_log) fclose(ym_log);
    if (itrace) fclose(itrace);
    if (pc_hist) {
        FILE* f = fopen(pc_hist, "w");
        for (unsigned i = 0; i < MEMSIZE / 2; i++) if (pc_seen[i]) fprintf(f, "%06x\n", i * 2);
        fclose(f);
    }
    return stopped ? 0 : 3;
}
