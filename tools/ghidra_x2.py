#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run Ghidra headless over the Xenon 2 game image and the six level modules.

Usage: ghidra_x2.py [game|l1..l6|all]   (default all)

Writes generated/lane-d2/ghidra-proj (Ghidra project), cfg_*.txt (script config) and
ghidra_*.json (raw export from tools/ghidra/X2Export.java). Seeds are deliberately minimal
(reset, interrupt vectors, known dispatch tables) so that the export can report which of the
heuristic (capstone) entry points Ghidra reaches on its own; every other heuristic entry point
is passed as a 'cand' and classified by X2Export.java.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'generated' / 'lane-d2'
LANE_D = ROOT / 'generated' / 'lane-d'
GHIDRA = Path('/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless')
JDK = Path('/opt/homebrew/opt/openjdk@21')
LEVEL_BASE = 0x54E00

sys.path.insert(0, str(ROOT / 'tools'))
from build_entrypoints import NAMES, DISPATCH_TABLES  # noqa: E402

CUSTOM = {
    0x000: 'BLTDDAT', 0x002: 'DMACONR', 0x004: 'VPOSR', 0x006: 'VHPOSR', 0x008: 'DSKDATR', 0x00A: 'JOY0DAT',
    0x00C: 'JOY1DAT', 0x00E: 'CLXDAT', 0x010: 'ADKCONR', 0x012: 'POT0DAT', 0x014: 'POT1DAT', 0x016: 'POTGOR',
    0x018: 'SERDATR', 0x01A: 'DSKBYTR', 0x01C: 'INTENAR', 0x01E: 'INTREQR', 0x020: 'DSKPTH', 0x022: 'DSKPTL',
    0x024: 'DSKLEN', 0x026: 'DSKDAT', 0x028: 'REFPTR', 0x02A: 'VPOSW', 0x02C: 'VHPOSW', 0x02E: 'COPCON',
    0x030: 'SERDAT', 0x032: 'SERPER', 0x034: 'POTGO', 0x036: 'JOYTEST', 0x040: 'BLTCON0', 0x042: 'BLTCON1',
    0x044: 'BLTAFWM', 0x046: 'BLTALWM', 0x048: 'BLTCPTH', 0x04A: 'BLTCPTL', 0x04C: 'BLTBPTH', 0x04E: 'BLTBPTL',
    0x050: 'BLTAPTH', 0x052: 'BLTAPTL', 0x054: 'BLTDPTH', 0x056: 'BLTDPTL', 0x058: 'BLTSIZE', 0x060: 'BLTCMOD',
    0x062: 'BLTBMOD', 0x064: 'BLTAMOD', 0x066: 'BLTDMOD', 0x070: 'BLTCDAT', 0x072: 'BLTBDAT', 0x074: 'BLTADAT',
    0x07E: 'DSKSYNC', 0x080: 'COP1LCH', 0x082: 'COP1LCL', 0x084: 'COP2LCH', 0x086: 'COP2LCL', 0x088: 'COPJMP1',
    0x08A: 'COPJMP2', 0x08C: 'COPINS', 0x08E: 'DIWSTRT', 0x090: 'DIWSTOP', 0x092: 'DDFSTRT', 0x094: 'DDFSTOP',
    0x096: 'DMACON', 0x098: 'CLXCON', 0x09A: 'INTENA', 0x09C: 'INTREQ', 0x09E: 'ADKCON', 0x100: 'BPLCON0',
    0x102: 'BPLCON1', 0x104: 'BPLCON2', 0x108: 'BPL1MOD', 0x10A: 'BPL2MOD',
}
for i in range(4):
    for off, n in ((0, 'LCH'), (2, 'LCL'), (4, 'LEN'), (6, 'PER'), (8, 'VOL'), (0xA, 'DAT')):
        CUSTOM[0xA0 + i * 0x10 + off] = 'AUD%d%s' % (i, n)
for i in range(6):
    CUSTOM[0xE0 + i * 4] = 'BPL%dPTH' % (i + 1)
    CUSTOM[0xE2 + i * 4] = 'BPL%dPTL' % (i + 1)
    CUSTOM[0x110 + i * 2] = 'BPL%dDAT' % (i + 1)
for i in range(8):
    CUSTOM[0x120 + i * 4] = 'SPR%dPTH' % i
    CUSTOM[0x122 + i * 4] = 'SPR%dPTL' % i
    for off, n in ((0, 'POS'), (2, 'CTL'), (4, 'DATA'), (6, 'DATB')):
        CUSTOM[0x140 + i * 8 + off] = 'SPR%d%s' % (i, n)
for i in range(32):
    CUSTOM[0x180 + i * 2] = 'COLOR%02d' % i
CIA = ['PRA', 'PRB', 'DDRA', 'DDRB', 'TALO', 'TAHI', 'TBLO', 'TBHI', 'TODLO', 'TODMID', 'TODHI', 'UNUSED',
       'SDR', 'ICR', 'CRA', 'CRB']


# Names established from the Ghidra pass (see docs/re-notes/ghidra-code-map.md); applied after
# analysis on top of build_entrypoints.NAMES. Value = (name, confidence H/M/L).
GHIDRA_NAMES = {
    0x0E96: ('apivec_e96_draw_bg_b', 'H'), 0x0FEE: ('obj_free', 'H'), 0x105C: ('obj_clip_xy_a', 'M'),
    0x107E: ('obj_clip_xy', 'M'), 0x10F6: ('blit_setup_masks_mods', 'M'), 0x128E: ('obj_draw_bob', 'M'),
    0x129A: ('blit_bob_cookie', 'H'), 0x1368: ('blit_bob_acd', 'M'), 0x1438: ('blit_bob_full', 'M'),
    0x15CE: ('print_string_blit', 'M'), 0x16BE: ('hud_draw_energy', 'L'), 0x176E: ('hud_draw_icons', 'L'),
    0x18B6: ('draw_background_a', 'M'), 0x1BD0: ('draw_background_b', 'M'), 0x1F50: ('rng_next', 'H'),
    0x1FF6: ('rng_mod', 'H'), 0x2418: ('obj_alloc', 'H'), 0x2C5A: ('anim_script_step', 'H'),
    0x3130: ('null_handler', 'H'), 0x3554: ('level_event_scan', 'M'), 0x366A: ('run_object_list_draw', 'H'),
    0x4634: ('powerup_dispatch', 'M'), 0x5924: ('obj_call_handler_a', 'H'), 0x6862: ('obj_unlink_free', 'M'),
    0x6FE8: ('palette_shadow_init', 'L'), 0x70F6: ('reset_trace_vector_then_init', 'H'),
    0x78A4: ('main_loop', 'H'), 0x7940: ('key_dispatch', 'H'), 0x79BA: ('key_pause', 'M'),
    0x7800: ('key_quit_to_menu', 'M'), 0x797A: ('key_space', 'M'), 0x79D6: ('main_loop_tail', 'M'),
    0x93C0: ('disk_step', 'M'), 0x19F9E: ('music_api_init', 'H'), 0x19FBA: ('music_api_stop', 'H'),
    0x19FC8: ('music_api_dma', 'M'), 0x19FE4: ('music_api_e4', 'M'), 0x19FF2: ('music_api_f2', 'M'),
    0x1B5A2: ('audio_dma_ctrl', 'M'),
}


def hw_lines():
    out = ['hw %x %s' % (0xDFF000 + o, n) for o, n in sorted(CUSTOM.items())]
    out += ['hw %x CIAA_%s' % (0xBFE001 + i * 0x100, n) for i, n in enumerate(CIA)]
    out += ['hw %x CIAB_%s' % (0xBFD000 + i * 0x100, n) for i, n in enumerate(CIA)]
    return out


def pointer_index():
    """exact BE32 values at even offsets in the game image and all level modules -> first location"""
    idx = {}
    imgs = [(LANE_D / 'game_0400.bin', 0x400)] + [(LANE_D / ('level%d_54e00.bin' % n), LEVEL_BASE) for n in range(1, 7)]
    for path, base in imgs:
        b = path.read_bytes()
        tag = 'game' if base == 0x400 else path.name[:6]
        for o in range(0, len(b) - 3, 2):
            v = int.from_bytes(b[o:o + 4], 'big')
            idx.setdefault(v, '%s@%x' % (tag, base + o))
    return idx


def find_pc_tables(img, base, ranges):
    """Find long-pointer dispatch tables by opcode pattern (68000):
       movea.l d8(pc,Xn),An ; jmp|jsr (An)                     (table = pc-relative)
       lea abs.l,An ; movea.l d8(An,Xn),An ; jmp|jsr (An)       (table = abs + d8)
    Entries are read until one is not an even address in one of ranges (24-bit masked) or the
    table runs into the lowest target. Returns [{site, table, kind, targets}]."""
    w = lambda o: int.from_bytes(img[o:o + 2], 'big')
    out = []
    for o in range(0, len(img) - 12, 2):
        op = w(o)
        tbl = None
        if op & 0xF1FF == 0x207B and not w(o + 2) & 0x0100:          # movea.l d8(pc,Xn),An
            an = (op >> 9) & 7
            d8 = (w(o + 2) & 0xFF) - (0x100 if w(o + 2) & 0x80 else 0)
            tbl, nxt = base + o + 2 + d8, o + 4
        elif op & 0xF1FF == 0x41F9 and w(o + 6) & 0xF1F8 == 0x2070:    # lea abs.l,An ; movea.l d8(Am,Xn),An
            an = (op >> 9) & 7
            if w(o + 6) & 7 != an or (w(o + 6) >> 9) & 7 != an or w(o + 8) & 0x0100:
                continue
            d8 = (w(o + 8) & 0xFF) - (0x100 if w(o + 8) & 0x80 else 0)
            # d8 < 0 means 1-based indexing (e.g. -4(a0,d0.w*4)): entries start at the lea base
            tbl, nxt = int.from_bytes(img[o + 2:o + 6], 'big') + max(d8, 0), o + 10
        if tbl is None:
            continue
        if w(nxt) not in (0x4ED0 | an, 0x4E90 | an):
            nxt += 2                       # allow one 1-word insn in between (e.g. move.l a0,-(sp))
        j = w(nxt)
        if j not in (0x4ED0 | an, 0x4E90 | an):
            continue
        targets, p = [], tbl
        while len(targets) < 64 and base <= p < base + len(img) - 3:
            v = int.from_bytes(img[p - base:p - base + 4], 'big') & 0xFFFFFF
            if v & 1 or not any(a <= v < b for a, b in ranges) or any(tbl < t <= p for t in targets):
                break
            targets.append(v)
            p += 4
        if targets:
            out.append({'site': base + nxt, 'table': tbl, 'kind': 'jsr' if j & 0x40 == 0 else 'jmp',
                        'targets': targets})
    return out


def table_lines(img, base, ranges):
    lines = []
    for t in find_pc_tables(img, base, ranges):
        lines.append('# pctable site=%x table=%x kind=%s n=%d' % (t['site'], t['table'], t['kind'], len(t['targets'])))
        lines += ['tableref %x %x %s' % (t['site'], v, 'call' if t['kind'] == 'jsr' else 'jump') for v in t['targets']]
    return lines


def cand(addr, kind, name, ptrs):
    return 'cand %x %s %s %s' % (addr, kind, name, ptrs.get(addr, '-'))


def game_cfg(ep, ptrs):
    lines = ['range 400 40000'] + hw_lines()
    roots = {0x400: 'entry_bra_reset', 0x7032: 'reset_entry', 0x20C2: 'irq3_vbl', 0x2098: 'irq3_vbl_diskload',
             0x0FAC: 'irq2_cia_keyboard', 0x95E4: 'irq1_dskblk'}
    lines += ['seed func %x %s' % (a, n) for a, n in roots.items()]
    # $E0C-$F5B: 56 x 6-byte 'jmp abs.l' vector slots = the resident API used by the level modules
    img = (LANE_D / 'game_0400.bin').read_bytes()
    for a in range(0xE0C, 0xF5C, 6):
        assert img[a - 0x400:a - 0x3FE] == b'\x4e\xf9'
        lines.append('seed func %x apivec_%03x' % (a, a))
    for t in DISPATCH_TABLES:
        if not isinstance(t['targets'], list):
            continue
        call = t['kind'].startswith('jsr')
        for tg in t['targets']:
            lines.append('tableref %x %x %s' % (t['site'], tg, 'call' if call else 'jump'))
    # the IRQ3 jsr (a0) sites: $217E/$2194 -> sfx entry, $21A8 -> music tick (vars $2094/$2090)
    lines += ['tableref 2194 19fd6 call', 'tableref 21a8 19fac call']
    lines += table_lines((LANE_D / 'game_0400.bin').read_bytes(), 0x400, [(0x400, 0x40000)])
    # resident routines the level modules call/jump to (from the level exports, run first)
    level_calls = {}
    for n in range(1, 7):
        lj = OUT / ('ghidra_level%d.json' % n)
        if lj.exists():
            for e in json.loads(lj.read_text()).get('external_flow', []):
                a = int(e['to'], 16)
                if 0x400 <= a < 0x40000:
                    level_calls.setdefault(a, 'level%d@%s' % (n, e['from'][0].split(':')[0]))
    for f in ep['resident_functions']:
        if f['addr'] in roots or f['addr'] == 0x19F9E:
            continue
        if f['addr'] in level_calls:
            lines.append('cand %x level-call %s %s' % (f['addr'], f['name'], level_calls.pop(f['addr'])))
        else:
            lines.append(cand(f['addr'], f['kind'], f['name'], ptrs))
    for a, ev in sorted(level_calls.items()):
        lines.append('cand %x level-call fn_%06x %s' % (a, a, ev))
    names = {a: n for a, n in NAMES.items() if a not in roots}
    names.update({a: n for a, (n, _) in GHIDRA_NAMES.items()})
    lines += ['name %x %s' % (a, n) for a, n in sorted(names.items())]
    lines += ['watch 7104 763a anti_trace_block', 'watch 54e00 80000 level_module_area', 'deadrange 7104 763a']
    return lines


def level_cfg(ep, n, ptrs):
    lines = ['range %x 80000' % LEVEL_BASE, 'extra %s 400 game' % (LANE_D / 'game_0400.bin')] + hw_lines()
    img = (LANE_D / ('level%d_54e00.bin' % n)).read_bytes()
    if img[0xC:0xE] == b'\x60\x00':
        # header: bra.w slots; the game calls $54E00 (falls through ori.b longs), $54E0C..$54E2C
        # game callers: $0C <- $3066/$30D0/$312A (object hook), $10 <- $3574 / $18 <- $35B2 (scroll events
        # from lists $54E38/$54E14), $20 <- $6F6A (level start), $24 <- $7912 (per frame), $2C <- $6EBE
        for off, nm in ((0xC, 'level_hook_0c'), (0x10, 'level_event_a'), (0x18, 'level_event_b'),
                        (0x20, 'level_start'), (0x24, 'level_frame'), (0x2C, 'level_hook_2c')):
            a = LEVEL_BASE + off
            tgt = a + 2 + int.from_bytes(img[off + 2:off + 4], 'big', signed=True)
            lines.append('seed code %x slot_%x' % (a, a))
            lines.append('seed func %x %s' % (tgt, nm))
    else:
        # not a header-based level module (disc-2 slot 6): plain code entered by jsr $54E00
        lines.append('seed func %x module_entry' % LEVEL_BASE)
    lines += table_lines(img, LEVEL_BASE, [(0x400, 0x1C400), (LEVEL_BASE, LEVEL_BASE + len(img))])
    lv = ep['levels'][str(n)]
    seen = {0x54E00, 0x54E0C, 0x54E10, 0x54E18, 0x54E20, 0x54E24, 0x54E2C}
    for x in lv['address_taken']:
        if x['addr'] not in seen:
            seen.add(x['addr'])
            lines.append(cand(x['addr'], x['how'].split('@')[0], 'fn_%x' % x['addr'], ptrs))
    for f in lv['functions']:
        if f['addr'] not in seen:
            seen.add(f['addr'])
            lines.append(cand(f['addr'], 'call-branch', 'fn_%x' % f['addr'], ptrs))
    lines += ['watch 54e00 54e0c level_header_longs']
    return lines


def run(name, binfile, base, cfg_lines):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'ghidra-proj').mkdir(exist_ok=True)
    cfg = OUT / ('cfg_%s.txt' % name)
    cfg.write_text('\n'.join(cfg_lines) + '\n')
    out = OUT / ('ghidra_%s.json' % name)
    env = dict(os.environ, JAVA_HOME=str(JDK), PATH='%s/bin:%s' % (JDK, os.environ['PATH']))
    cmd = [str(GHIDRA), str(OUT / 'ghidra-proj'), 'xenon2', '-import', str(binfile), '-overwrite',
           '-loader', 'BinaryLoader', '-loader-baseAddr', hex(base), '-processor', '68000:BE:32:default',
           '-cspec', 'default', '-scriptPath', str(ROOT / 'tools' / 'ghidra'),
           '-preScript', 'X2Prep.java', str(cfg), '-postScript', 'X2Export.java', str(cfg), str(out),
           str(OUT / ('ghidra_%s.lst' % name))]
    log = OUT / ('headless_%s.log' % name)
    with log.open('w') as lf:
        lf.write(' '.join(cmd) + '\n')
        lf.flush()
        rc = subprocess.call(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    print(name, 'rc', rc, 'log', log, 'out', out if out.exists() else 'MISSING')
    return rc


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else 'all'
    ep = json.loads((LANE_D / 'entrypoints.json').read_text())
    ptrs = pointer_index()
    for n in range(1, 7):
        if which in ('all', 'l%d' % n):
            run('level%d' % n, LANE_D / ('level%d_54e00.bin' % n), LEVEL_BASE, level_cfg(ep, n, ptrs))
    if which in ('all', 'game'):
        run('game', LANE_D / 'game_0400.bin', 0x400, game_cfg(ep, ptrs))


if __name__ == '__main__':
    main()
