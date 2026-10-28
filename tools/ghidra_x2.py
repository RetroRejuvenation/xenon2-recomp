#!/usr/bin/env python3
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


def hw_lines():
    out = ['hw %x %s' % (0xDFF000 + o, n) for o, n in sorted(CUSTOM.items())]
    out += ['hw %x CIAA_%s' % (0xBFE001 + i * 0x100, n) for i, n in enumerate(CIA)]
    out += ['hw %x CIAB_%s' % (0xBFD000 + i * 0x100, n) for i, n in enumerate(CIA)]
    return out


def pointer_index():
    """low-24-bit BE32 values at even offsets in the game image and all level modules -> first location"""
    idx = {}
    imgs = [(LANE_D / 'game_0400.bin', 0x400)] + [(LANE_D / ('level%d_54e00.bin' % n), LEVEL_BASE) for n in range(1, 7)]
    for path, base in imgs:
        b = path.read_bytes()
        tag = 'game' if base == 0x400 else path.name[:6]
        for o in range(0, len(b) - 3, 2):
            v = int.from_bytes(b[o:o + 4], 'big') & 0xFFFFFF
            idx.setdefault(v, '%s@%x' % (tag, base + o))
    return idx


def cand(addr, kind, name, ptrs):
    return 'cand %x %s %s %s' % (addr, kind, name, ptrs.get(addr, '-'))


def game_cfg(ep, ptrs):
    lines = ['range 400 40000'] + hw_lines()
    roots = {0x400: 'entry_bra_reset', 0x7032: 'reset_entry', 0x20C2: 'irq3_vbl', 0x2098: 'irq3_vbl_diskload',
             0x0FAC: 'irq2_cia_keyboard', 0x95E4: 'irq1_dskblk'}
    lines += ['seed func %x %s' % (a, n) for a, n in roots.items()]
    for t in DISPATCH_TABLES:
        if not isinstance(t['targets'], list):
            continue
        call = t['kind'].startswith('jsr')
        for tg in t['targets']:
            lines.append('tableref %x %x %s' % (t['site'], tg, 'call' if call else 'jump'))
    # the IRQ3 jsr (a0) sites: $217E/$2194 -> sfx entry, $21A8 -> music tick (vars $2094/$2090)
    lines += ['tableref 2194 19fd6 call', 'tableref 21a8 19fac call']
    for f in ep['resident_functions']:
        if f['addr'] in roots or f['addr'] == 0x19F9E:
            continue
        lines.append(cand(f['addr'], f['kind'], f['name'], ptrs))
    lines += ['name %x %s' % (a, n) for a, n in NAMES.items() if a not in roots]
    lines += ['watch 7104 763a anti_trace_block', 'watch 54e00 80000 level_module_area']
    return lines


def level_cfg(ep, n, ptrs):
    lines = ['range %x 80000' % LEVEL_BASE, 'extra %s 400 game' % (LANE_D / 'game_0400.bin')] + hw_lines()
    for a, nm in ((0x54E0C, 'level_init'), (0x54E10, 'level_hdr_10'), (0x54E18, 'level_hdr_18'),
                  (0x54E20, 'level_hdr_20'), (0x54E24, 'level_frame'), (0x54E2C, 'level_hdr_2c')):
        lines.append('seed func %x %s' % (a, nm))
    lv = ep['levels'][str(n)]
    seen = {0x54E00, 0x54E0C, 0x54E10, 0x54E18, 0x54E20, 0x54E24, 0x54E2C}
    for f in lv['functions']:
        if f['addr'] not in seen:
            seen.add(f['addr'])
            lines.append(cand(f['addr'], 'call-branch', 'fn_%x' % f['addr'], ptrs))
    for x in lv['address_taken']:
        if x['addr'] not in seen:
            seen.add(x['addr'])
            lines.append(cand(x['addr'], x['how'].split('@')[0], 'fn_%x' % x['addr'], ptrs))
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
    if which in ('all', 'game'):
        run('game', LANE_D / 'game_0400.bin', 0x400, game_cfg(ep, ptrs))
    for n in range(1, 7):
        if which in ('all', 'l%d' % n):
            run('level%d' % n, LANE_D / ('level%d_54e00.bin' % n), LEVEL_BASE, level_cfg(ep, n, ptrs))


if __name__ == '__main__':
    main()
