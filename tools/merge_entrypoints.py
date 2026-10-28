#!/usr/bin/env python3
"""Merge the heuristic (capstone) entry points with the Ghidra pass.

Usage: merge_entrypoints.py        (run tools/ghidra_x2.py all first)

Inputs : generated/lane-d/entrypoints.json, generated/lane-d2/ghidra_{game,level1..6}.json
Outputs: generated/lane-d2/functions.json, indirect_sites.json, hw_access.json, entrypoints_v2.json
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
D2 = ROOT / 'generated' / 'lane-d2'
sys.path.insert(0, str(ROOT / 'tools'))
from build_entrypoints import NAMES  # noqa: E402
from ghidra_x2 import GHIDRA_NAMES, LEVEL_BASE  # noqa: E402

MODULES = ['game'] + ['level%d' % n for n in range(1, 7)]

# Resolution of every indirect control transfer (H = read in the Ghidra listing).
INDIRECT_NOTES = {
    0x217E: ('known-vars', 'jsr (a0), a0 = long at $2094 (sfx entry): $19FD6, or $647A6 while disc-2 slot 6 (shop) is loaded', 'H'),
    0x2194: ('known-vars', 'jsr (a0), a0 = long at $2094: $19FD6 / $647A6 (shop module)', 'H'),
    0x21A8: ('known-vars', 'jsr (a0), a0 = long at $2090 (music tick): $19FAC / $6477C (shop module)', 'H'),
    0x2C6C: ('data-driven', 'anim_script_step: obj+$1C stream, long 0 then long jump target (a0 = obj)', 'H'),
    0x3662: ('data-driven', 'run_object_list: obj+2 update handler (move.l #imm,(2,An) sets; see handler_sets)', 'H'),
    0x3676: ('data-driven', 'run_object_list_draw: obj+6 draw handler (blit_bob_* / null_handler / API vectors)', 'H'),
    0x47D0: ('known-table', 'jsr (a1): 19 longs at $47D6, top byte = flags, mask 24 bits', 'H'),
    0x5924: ('data-driven', 'move.l ($A,a1),-(sp); rts = jump to obj+$A handler (push/rts, not flagged by Ghidra)', 'H'),
    0x7952: ('known-table', 'key_dispatch: 4-byte {key.b,target.24} entries at $7954 (P,ESC,SPACE,default)', 'H'),
    0x97E6: ('known-table', 'jmp d(pc,d6.w): 6 word offsets at $97EA', 'H'),
    0x1A468: ('known-table', 'jmp (a3,a2.w): 12 word offsets at $1A56A, a3 = $19F9E (Ghidra also adds two bogus targets $19F9E/$19FDE)', 'H'),
}
BOGUS_TARGETS = {0x1A468: {0x19F9E, 0x19FDE}}
EXTRA_SITES = [{'site': 0x5924, 'insn': 'move.l (0xa,A1),-(SP) ; rts', 'module': 'game', 'function': 'obj_call_handler_a',
                'flow': 'PUSH_RTS', 'ghidra_targets': []}]


def hx(v):
    return '0x%x' % v


def load():
    return {m: json.loads((D2 / ('ghidra_%s.json' % m)).read_text()) for m in MODULES}


def confidence(f, cand_kind):
    if f['pure']:
        return 'high', 'ghidra-auto'
    if f['source'] in ('USER_DEFINED', 'IMPORTED') and cand_kind is None:
        return 'high', 'ghidra-seed'
    if cand_kind in (None,):
        return ('medium', 'ghidra-orphan-entry') if f['orphan_entry'] else ('high', 'ghidra-table/flow')
    if cand_kind == 'gap-sweep':
        return 'low', 'heuristic-gap-sweep+ghidra-valid'
    return 'medium', 'heuristic-%s+ghidra-valid' % cand_kind


def main():
    ex = load()
    heur = json.loads((ROOT / 'generated' / 'lane-d' / 'entrypoints.json').read_text())
    names = dict(NAMES)
    names.update({a: n for a, (n, _) in GHIDRA_NAMES.items()})
    name_conf = {a: c for a, (_, c) in GHIDRA_NAMES.items()}

    functions, hw, sites = [], [], []
    for m, d in ex.items():
        cands = {int(c['addr'], 16): c for c in d['candidates']}
        for f in d['functions']:
            a = int(f['start'], 16)
            c = cands.get(a)
            ck = c['heuristic_kind'] if c and not c['verdict'].startswith('code:reached') else None
            conf, src = confidence(f, ck)
            functions.append({'module': m, 'start': f['start'], 'end': f['end'], 'name': f['name'],
                              'name_confidence': name_conf.get(a, 'M' if a in NAMES else None),
                              'confidence': conf, 'source': src, 'insns': f['insns'],
                              'reached_by_plain_analysis': f['pure'], 'ref_kinds': f['ref_kinds'],
                              'refs_to': f['refs_to']})
        for h in d['hw_access']:
            hw.append(dict(h, module=m))
        for s in d['indirect_sites']:
            a = int(s['site'], 16)
            kind, note, conf = INDIRECT_NOTES.get(a, (None, None, None))
            tg = [t for t in s['ghidra_targets'] if int(t, 16) not in BOGUS_TARGETS.get(a, set())]
            tg = list(dict.fromkeys(tg))
            if kind is None:
                if s['insn'].startswith('jmp (A2)') and tg:
                    kind, note, conf = ('known-table', 'level dispatcher: movea.l tbl(pc,d0.w*4),a2; jmp (a2); '
                                        'table found by opcode scan, entry 0 = resident API vector', 'H')
                elif 'jsr (0x46,A2)' in s['insn']:
                    kind, note, conf = ('data-driven', 'jsr $46(a2): a2 = object/data record; Ghidra target %s is a '
                                        'constant-propagation artefact' % s['ghidra_targets'], 'M')
                    tg = []
                elif s['insn'].startswith('jsr (A0)') and m == 'level6':
                    kind, note, conf = ('known-table', 'lea $5573A,a0; movea.l -4(a0,d0.w*4),a0; jsr (a0): '
                                        'shop menu action table (index from data)', 'M')
                else:
                    kind, note, conf = ('data-driven', 'jmp (a1) to obj+2 handler (tail call into object update)', 'H')
            sites.append({'module': m, 'site': s['site'], 'insn': s['insn'], 'function': s['function'],
                          'flow': s['flow'], 'resolution': kind, 'note': note, 'confidence': conf,
                          'targets': tg, 'target_count': len(tg)})
    for e in EXTRA_SITES:
        kind, note, conf = INDIRECT_NOTES[e['site']]
        sites.append({'module': e['module'], 'site': hx(e['site']), 'insn': e['insn'], 'function': e['function'],
                      'flow': e['flow'], 'resolution': kind, 'note': note, 'confidence': conf, 'targets': [],
                      'target_count': 0})

    # data-driven handler target sets (immediates stored to obj+2/+6/+A/+$1C in all listings)
    import re
    rx = re.compile(r'move\.l #0x([0-9a-f]+),\(0x(2|6|a|1c),A[0-7]\)')
    handler_sets = {}
    for m in MODULES:
        for line in (D2 / ('ghidra_%s.lst' % m)).read_text().splitlines():
            r = rx.search(line)
            if r:
                handler_sets.setdefault('obj+0x' + r.group(2), set()).add(int(r.group(1), 16))
    handler_sets = {k: [hx(v) for v in sorted(s)] for k, s in handler_sets.items()}

    # reconciliation of heuristic resident entry points
    g = ex['game']
    gc = {int(c['addr'], 16): c for c in g['candidates']}
    recon = []
    resident_all = g['functions']
    for f in heur['resident_functions']:
        a = f['addr']
        c = gc.get(a)
        if c is None:
            status = 'seed' if a in (0x400, 0x7032, 0x20C2, 0x2098, 0x0FAC, 0x95E4) else 'agree:ghidra-function'
            final = 'function-entry' if any(int(f2['start'], 16) == a for f2 in resident_all) else 'absent'
        else:
            final = c['final_state']
            v = c['verdict']
            if v.startswith('code:reached'):
                status = 'agree:reached-by-ghidra'
            elif v.startswith('code'):
                status = 'agree:code-validated' if final in ('function-entry', 'instruction-start') else 'disagree:mid-instruction'
            elif v.startswith('dead'):
                status = 'disagree:dead-anti-trace-block'
            elif v.startswith('unknown'):
                status = 'unknown:code-shaped-unreferenced'
            else:
                status = 'disagree:data'
        recon.append({'addr': hx(a), 'heuristic_name': f['name'], 'heuristic_kind': f['kind'],
                      'heuristic_confidence': f['confidence'], 'status': status, 'ghidra_final_state': final,
                      'verdict': c['verdict'] if c else 'seed', 'pointer_evidence': c['pointer_evidence'] if c else '-'})

    level_recon = {}
    for n in range(1, 7):
        d = ex['level%d' % n]
        level_recon[str(n)] = [{'addr': c['addr'], 'heuristic_kind': c['heuristic_kind'], 'verdict': c['verdict'],
                                'final_state': c['final_state']} for c in d['candidates']]

    resident = [f for f in functions if f['module'] == 'game']
    out = {
        'about': 'Xenon 2 entry points v2: Ghidra 12.1.4 headless (68000:BE:32:default) over the resident image and '
                 'the 6 disc-2 modules, reconciled with lane-d heuristic entrypoints.json. Addresses are runtime '
                 '(mask pointers to 24 bits). See docs/re-notes/ghidra-code-map.md.',
        'images': heur['images'],
        'interrupt_vectors': heur['interrupt_vectors'],
        'resident_api_vectors': {'range': ['0xe0c', '0xf5c'], 'count': 56, 'stride': 6,
                                 'note': 'jmp abs.l slots called by the level modules; seeded as functions'},
        'resident_functions': [{k: f[k] for k in ('start', 'end', 'name', 'name_confidence', 'confidence', 'source',
                                                  'insns', 'reached_by_plain_analysis')} for f in resident],
        'level_modules': {
            m: {'kind': 'shop+ending module (no bra header; entered at $54E00)' if m == 'level6' else 'level module',
                'functions': [{k: f[k] for k in ('start', 'end', 'name', 'confidence', 'source', 'insns')}
                              for f in functions if f['module'] == m],
                'calls_into_resident': ex[m]['external_flow']}
            for m in MODULES[1:]},
        'indirect_sites': sites,
        'handler_sets': handler_sets,
        'heuristic_reconciliation': {'resident': recon, 'levels': level_recon},
        'self_modifying_code': {
            'resident_game': 'none found: Ghidra WRITE refs into instructions = 0; all lane-d candidates are data '
                             'words between functions (e.g. RNG seeds $1FEE/$1FF2 written by $9692)',
            'level6_false_positives': ex['level6']['code_writes'],
            'crack_stub_patches': heur['self_modifying_code']['confirmed'],
        },
        'anti_trace_block': [w for w in g['watch'] if w['tag'] == 'anti_trace_block'][0],
        'level_header_refs_from_game': [w for w in g['watch'] if w['tag'] == 'level_module_area'][0]['external_refs'],
    }
    (D2 / 'functions.json').write_text(json.dumps(functions, indent=1))
    (D2 / 'indirect_sites.json').write_text(json.dumps({'sites': sites, 'handler_sets': handler_sets}, indent=1))
    (D2 / 'hw_access.json').write_text(json.dumps(hw, indent=1))
    (D2 / 'entrypoints_v2.json').write_text(json.dumps(out, indent=1))
    from collections import Counter
    print('functions', len(functions), Counter(f['module'] for f in functions))
    print('indirect sites', len(sites), Counter(s['resolution'] for s in sites))
    print('hw routines', len(hw))
    print('resident reconciliation', Counter(r['status'] for r in recon))
    print('gap-sweep', Counter(r['status'] for r in recon if r['heuristic_kind'] == 'gap-sweep'))


if __name__ == '__main__':
    main()
