#!/usr/bin/env python3
"""Merge per-level analyze68k.py outputs into generated/lane-d/entrypoints.json.

Usage: build_entrypoints.py generated/lane-d   (expects ana_l1..6.json there)
"""
import json
import sys
from pathlib import Path

LEVEL_BASE = 0x54E00

# Hand-identified routines (see docs/re-notes/code-map.md)
NAMES = {
    0x400: 'entry_bra_reset', 0x7032: 'reset_entry', 0x763C: 'init_and_main_loop', 0x20C2: 'irq3_vbl',
    0x2098: 'irq3_vbl_diskload', 0x0FAC: 'irq2_cia_keyboard', 0x95E4: 'irq1_dskblk', 0x2026: 'wait_frame_swap_copper',
    0x202E: 'wait_frame_swap_copper_alt', 0x17D6: 'blit_objects_main', 0x233A: 'object_pass_a', 0x35D4: 'update_object_lists',
    0x3656: 'run_object_list', 0x921A: 'disk_read_sectors', 0x93F8: 'disk_read_track', 0x932E: 'disk_seek_track',
    0x951E: 'mfm_sync_find', 0x957E: 'mfm_decode', 0x92C8: 'disk_motor_on', 0x92EE: 'disk_motor_off', 0x9308: 'disk_home',
    0x8FF2: 'level_load_and_run', 0x917E: 'level_load_if_changed', 0x8F9A: 'disk_check_discid', 0x0E96: 'blit_wrapper_a',
    0x10F6: 'blit_wrapper_b', 0x1368: 'blit_wrapper_c', 0x1438: 'blit_wrapper_d', 0x635E: 'blit_copy_wrapper',
    0x6372: 'palette_fade', 0x8220: 'title_or_menu', 0x8CB6: 'options_menu', 0x19F9E: 'music_base_a3',
    0x19FAC: 'music_tick_entry', 0x19FD6: 'sfx_trigger_entry', 0x19FFC: 'music_init', 0x1A09A: 'music_stop', 0x1A154: 'music_tick_core',
    0x1B1B6: 'music_sample_table_setup', 0x1B26A: 'music_channel_update', 0x54E0C: 'level_init', 0x54E24: 'level_frame',
}

DISPATCH_TABLES = [
    {'site': 0x97E6, 'kind': 'jmp d(pc,d6.w) word offsets rel. to $97EA', 'table': 0x97EA, 'count': 6,
     'targets': [0x97F6, 0x988E, 0x98BE, 0x98DA, 0x987C, 0x9870], 'confidence': 'high'},
    {'site': 0x7952, 'kind': 'jmp (a0): 4-byte entries {key.b, target.24}, key = last raw keycode ($9FF)', 'table': 0x7954,
     'count': 4, 'targets': [0x79BA, 0x7800, 0x797A, 0x79D6], 'confidence': 'high'},
    {'site': 0x47D0, 'kind': 'jsr (a1): long entries, top byte = flag (68000 24-bit mask needed)', 'table': 0x47D6,
     'count': 19, 'targets': [0x4906, 0x48F6, 0x4894, 0x48B4, 0x5206, 0x4920, 0x48BC, 0x543A, 0x4F66, 0x538C,
                              0x5014, 0x50AC, 0x52E0, 0x4DC6, 0x4E74, 0x4D28, 0x4A94, 0x491A, 0x4926],
     'confidence': 'high'},
    {'site': 0x1A468, 'kind': 'jmp (a3,a2.w): a3=$19F9E, word offsets at $1A56A', 'table': 0x1A56A, 'count': 12,
     'targets': [0x1A46C, 0x1A492, 0x1A4A8, 0x1A4D4, 0x1A4D8, 0x1A512, 0x1A51A, 0x1A52E, 0x1A536, 0x1A53E,
                 0x1A550, 0x1A55C], 'confidence': 'high'},
    {'site': 0x217E, 'kind': 'jsr (a0) from irq3: handler ptrs in vars $2094 (sfx, = $19FD6) / $2090 (music tick, = $19FAC)',
     'targets': [0x19FD6, 0x19FAC], 'confidence': 'high'},
    {'site': 0x3662, 'kind': 'jsr (a1): per-object update handler = long at obj+2 (lists $A88,$AEA,$B4C,$BAE,$C10)',
     'targets': 'DATA-DRIVEN: object handler pointers; see address_taken (immediate/gap-sweep) + level modules',
     'confidence': 'medium'},
    {'site': 0x2C6C, 'kind': 'jmp (a2): animation/script stream interpreter (obj+$1C stream; long 0 then target)',
     'targets': 'DATA-DRIVEN: script streams in resident data and level modules', 'confidence': 'low'},
    {'site': 0x7952, 'kind': 'see above', 'targets': [], 'confidence': 'high'},
]


def main():
    d = Path(sys.argv[1])
    levels = {}
    for n in range(1, 7):
        levels[n] = json.load((d / ('ana_l%d.json' % n)).open())
    res_sets = []
    for n, j in levels.items():
        res_sets.append({f['start'] for f in j['functions'] if f['start'] < LEVEL_BASE})
    resident_funcs = sorted(set.union(*res_sets))
    common = set.intersection(*res_sets)
    taken = {}
    for j in levels.values():
        for x in j['address_taken']:
            if x['addr'] < LEVEL_BASE:
                taken.setdefault(x['addr'], x['how'])
    roots = set(NAMES)

    def kind(a):
        if a in NAMES:
            return 'named-root', 'high'
        how = taken.get(a)
        if how is None:
            return 'call-target/branch', 'high'
        if how.startswith('gap-sweep'):
            return 'gap-sweep', 'low'
        return how.split('@')[0] + '-ptr', 'medium'

    out = {
        'about': 'Xenon 2 Megablast (Amiga) function entry points for the recompiler. Addresses are runtime '
                 'addresses with chip RAM mapped 1:1 (68000: mask pointers to 24 bits).',
        'images': {
            'resident': {'file': 'game_0400.bin', 'base': 0x400, 'size': 0x3FC00,
                         'derivation': 'unpack_xenon2.py XENON-II -> image@$30000; bytes [0:$3FC00] copied to $400 by crack stub'},
            'level_modules': {'base': LEVEL_BASE, 'files': 'level{1..6}_54e00.bin (disc 2 raw sectors)',
                              'sectors': [[11, 229], [250, 254], [514, 275], [799, 345], [1154, 312], [1476, 248]]},
        },
        'interrupt_vectors': {'0x64': 0x95E4, '0x68': 0x0FAC, '0x6C': 0x20C2, '0x6C_during_diskload': 0x2098,
                              '0x08-0x3FC_default': 0x7032, '0x24': 'bogus 0x7670CB6B (anti-trace)'},
        'resident_functions': [
            {'addr': a, 'name': NAMES.get(a, 'fn_%06x' % a), 'kind': kind(a)[0], 'confidence': kind(a)[1],
             'in_all_levels_analysis': a in common}
            for a in resident_funcs],
        'level_module_entry_points': {
            'header_bra': {f'0x{o:x}': 'bra.w init/handlers' for o in (0x54E0C, 0x54E10, 0x54E18, 0x54E20, 0x54E24, 0x54E2C)},
            'notes': '$54E00 is entered by JSR from game (header longs execute as harmless ORI.B), falls into bra at $54E0C',
        },
        'levels': {
            str(n): {
                'functions': [{'addr': f['start'], 'kind': 'call/branch/bra-root'}
                              for f in j['functions'] if f['start'] >= LEVEL_BASE],
                'dynamic_sites': [x for x in j['dynamic_sites'] if x['addr'] >= LEVEL_BASE],
                'address_taken': [x for x in j['address_taken'] if x['addr'] >= LEVEL_BASE],
            } for n, j in levels.items()},
        'dispatch_tables': DISPATCH_TABLES,
        'dynamic_sites_resident': [x for x in levels[1]['dynamic_sites'] if x['addr'] < LEVEL_BASE],
        'self_modifying_code': {
            'confirmed': [
                {'where': 'crack stub $70000 (not part of game)', 'writes': 'byte $60 -> $6384 (bra over energy decrement), '
                 'NOPs x3 -> $7B1C (lives decrement) when trainer flags $70232/$70236 set'},
            ],
            'candidates': [x for x in levels[1]['code_writes']],
            'notes': 'candidates are mostly variables/data decoded as code by heuristics; none confirmed in resident game',
        },
        'hardware': {
            'dff080_copper_lists': [0x1038, 0x1014],
            'bitplanes': 'screen buffers $4C700/$44000 via vars $406/$40A, 4 planes interleaved, modulo $78',
        },
    }
    (d / 'entrypoints.json').write_text(json.dumps(out, indent=1))
    print('resident funcs', len(resident_funcs), 'dispatch tables', len(DISPATCH_TABLES))


if __name__ == '__main__':
    main()
