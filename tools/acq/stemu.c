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
//   --stop PC                  stop when PC is reached (may repeat)
//   --max N                    instruction cap (default 400000000)
//   --dump FILE:ADDR:LEN       write memory on exit (may repeat)
//   --fdc-log                  log every FDC command and DMA transfer on stderr
//   --ring                     print the last 64 PCs when the run ends
//   --ym-log FILE              log every YM2149 register write as 'instruction-count register value' (for rendering the music)
//   --vbl N                    raise the level-4 (vertical blank) interrupt every N instructions
//   --pc-hist FILE             write the distinct PCs that were executed
//   --bad-sector TRK:SEC       report a CRC error (status 0x08) for this sector
//   --slow-sector TRK:SEC:PCT  make this sector's DMA transfer PCT percent slower (the Copylock timing check)
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
enum { H_RET, H_JUMP, H_COPY, H_RETSEQ };
struct hook { unsigned pc; int kind; unsigned a, b; uint8_t* data; size_t len; unsigned seq[8]; int nseq, idx; };
static struct hook hooks[32];
static int nhooks;
static unsigned stops[16];
static int nstops;
static unsigned long insns, maxi = 400000000UL;
static int stopped;
static int fdc_log;
static uint8_t* pc_seen;
static int bad_trk = -1, bad_sec = -1;
static unsigned long vbl_period, vbl_next, vbl_count;
static FILE* ym_log;
static unsigned ring[256], ring_pos;
static int ring_on;

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
static int pend_bad, pend_have;
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
        if (multi) { fprintf(stderr, "[fdc] multi-sector read not modelled in poll mode (cmd %02x)\n", cmd); }
        fdc_status = 0;
        if (fdc_track != fdc_head || fdc_sector < 1 || fdc_sector > spt) { fdc_status = 0x10; return; }  // record not found
        size_t o = sector_offset(fdc_head, side, fdc_sector);
        if (o + 512 > disk_len) { fdc_status = 0x10; return; }
        fdc_busy = 1; fdc_polls = 0;
        pend_addr = dma_addr; pend_sector = fdc_sector; pend_head = fdc_head; pend_side = side;
        pend_bad = (fdc_head == bad_trk && fdc_sector == bad_sec);
        fdc_xfer_len = (fdc_head == slow_trk && fdc_sector == slow_sec) ? slow_len : 1000;
        return;
    }
    if ((cmd & 0xF0) == 0xD0) { fdc_status = fdc_head == 0 ? 0x04 : 0; return; }  // force interrupt
    fprintf(stderr, "[fdc] unsupported command %02x\n", cmd);
    fdc_status = 0x80;
}

static void fdc_complete(void) {
    size_t o = sector_offset(pend_head, pend_side, pend_sector);
    if (dma_count) {
        memcpy(mem + (pend_addr & (MEMSIZE - 1)), disk + o, 512);
        if (fdc_log) fprintf(stderr, "[dma] trk %u side %u sec %u -> %06x\n", pend_head, pend_side, pend_sector, pend_addr);
        dma_addr = pend_addr + 512;
        dma_count--;
    }
    fdc_status = pend_bad ? 0x08 : 0;
    fdc_sector = pend_sector + 1;
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

static unsigned video_counter(void) {  // a raster that sweeps the whole mid byte every 16K instructions, so any wait-for-line loop ends
    unsigned base = ((unsigned)mem[0xFF8201] << 16) | ((unsigned)mem[0xFF8203] << 8);
    return (base & 0xFF0000) | (unsigned)(((insns >> 6) & 0xFF) << 8) | (unsigned)(insns & 0xFE);
}

static unsigned io_read8(unsigned a) {
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

static void io_write8(unsigned a, unsigned v) {
    v &= 0xFF;
    if (a == 0xFF8609) { dma_addr = (dma_addr & 0x00FFFF) | (v << 16); return; }
    if (a == 0xFF860B) { dma_addr = (dma_addr & 0xFF00FF) | (v << 8); return; }
    if (a == 0xFF860D) { dma_addr = (dma_addr & 0xFFFF00) | v; return; }
    if (a == 0xFF8800) { ym_sel = v; return; }
    if (a == 0xFF8802) {
        if (ym_log && ym_sel < 14) fprintf(ym_log, "%lu %u %u\n", insns, ym_sel, v);
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
unsigned int m68k_read_memory_8(unsigned a) { a &= 0xFFFFFF; return IS_IO(a) ? io_read8(a) : mem[a]; }
unsigned int m68k_read_memory_16(unsigned a) {
    a &= 0xFFFFFF;
    if (IS_IO(a)) return (io_read8(a) << 8) | io_read8(a + 1);
    return (mem[a] << 8) | mem[a + 1];
}
unsigned int m68k_read_memory_32(unsigned a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_16(unsigned a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned a, unsigned v) { a &= 0xFFFFFF; if (IS_IO(a)) io_write8(a, v); else mem[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned a, unsigned v) {
    a &= 0xFFFFFF;
    if (IS_IO(a)) { io_write16(a, v); return; }
    mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v;
}
void m68k_write_memory_32(unsigned a, unsigned v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v & 0xFFFF); }

int st_int_ack(int level) {
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

void st_instruction_hook(unsigned pc) {
    insns++;
    if (vbl_period && insns >= vbl_next) { vbl_next = insns + vbl_period; vbl_count++; m68k_set_irq(4); }
    if (ring_on) ring[ring_pos++ & 255] = pc;
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
        else if (!strcmp(a, "--ring")) ring_on = 1;
        else if (!strcmp(a, "--ym-log") && i + 1 < argc) { ym_log = fopen(argv[++i], "w"); if (!ym_log) return 2; }
        else if (!strcmp(a, "--vbl") && i + 1 < argc) { vbl_period = strtoul(argv[++i], 0, 0); vbl_next = vbl_period; }
        else if (!strcmp(a, "--slow-sector") && i + 1 < argc) { int p; sscanf(argv[++i], "%d:%d:%d", &slow_trk, &slow_sec, &p); slow_len = 1000 + 1000 * p / 100; }
        else if (!strcmp(a, "--stop") && i + 1 < argc) stops[nstops++] = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(a, "--pc-hist") && i + 1 < argc) pc_hist = argv[++i];
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
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_PC, boot_at);
    m68k_set_reg(M68K_REG_SR, 0x2300);
    m68k_set_reg(M68K_REG_A7, boot_at);
    while (!stopped && insns < maxi) m68k_execute(100000);
    fprintf(stderr, "stopped=%d insns=%lu pc=%06x sr=%04x sp=%06x\n", stopped, insns, m68k_get_reg(NULL, M68K_REG_PC),
            m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_SP));
    if (ring_on) { fprintf(stderr, "last PCs:"); for (int i = 0; i < 64; i++) fprintf(stderr, " %06x", ring[(ring_pos - 64 + i) & 255]); fprintf(stderr, "\n"); }
    for (int i = 0; i < ndumps; i++) {
        FILE* f = fopen(dumps[i].file, "wb");
        if (!f) { perror(dumps[i].file); return 2; }
        fwrite(mem + dumps[i].a, 1, dumps[i].n, f);
        fclose(f);
    }
    if (ym_log) fclose(ym_log);
    if (pc_hist) {
        FILE* f = fopen(pc_hist, "w");
        for (unsigned i = 0; i < MEMSIZE / 2; i++) if (pc_seen[i]) fprintf(f, "%06x\n", i * 2);
        fclose(f);
    }
    return stopped ? 0 : 3;
}
