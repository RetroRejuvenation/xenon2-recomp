#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Recursive-descent 68000 analyser for the unpacked Xenon 2 game image (base $400).

Needs capstone (pip install capstone). Produces a JSON database of function starts,
branch/jump-table targets, dynamic dispatch sites, hardware-register usage per routine
and writes into code (self-modifying candidates).

Usage: analyze68k.py out.json --seg file@HEXBASE [--seg ...] [--root HEX ...]
"""
import argparse
import json
import re
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

NOWRITE = ('cmp', 'btst', 'tst', 'lea', 'pea', 'jmp', 'jsr', 'chk', 'bra', 'bsr', 'db', 'movem')
ABS = re.compile(r'^\$([0-9a-f]+)\.([wl])$')
IMM = re.compile(r'^#\$([0-9a-f]+)$')
DISP_AN = re.compile(r'^(-?)\$([0-9a-f]+)\(a([0-7])\)$')
REG_AN = re.compile(r'^\(a([0-7])\)\+?$|^-\(a([0-7])\)$')


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--seg', action='append', required=True, help='file@HEXBASE (repeatable)')
    ap.add_argument('--root', action='append', default=[])
    ap.add_argument('--codezone', action='append', default=[], help='LO-HI hex range to gap-sweep for unreached code')
    a = ap.parse_args()
    mem = bytearray(0x100000)
    valid = bytearray(0x100000)
    segs = []
    for sp in a.seg:
        fn_, b_ = sp.rsplit('@', 1)
        data = open(fn_, 'rb').read()
        b_ = int(b_, 16)
        mem[b_:b_ + len(data)] = data
        valid[b_:b_ + len(data)] = b'\x01' * len(data)
        segs.append((b_, b_ + len(data)))
    base = segs[0][0]
    end = max(e for _, e in segs)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.detail = True

    roots = {int(r, 0) for r in a.root} | {base}
    bases = {b for b, _ in segs}
    funcs = set(roots)          # call targets / entry points
    labels = set()              # intra-function branch targets
    insns = {}                  # addr -> (size, mnem, ops)
    func_of = {}                # addr -> owning function (first function to reach it)
    dyn_sites = []              # indirect jmp/jsr
    tables = []                 # pc-relative jump-table style sites
    abs_reads = defaultdict(set)
    abs_writes = defaultdict(set)   # addr -> {insn addr}
    hw = defaultdict(lambda: defaultdict(set))   # func -> reg -> {insn}
    vec_writes = []
    queue = [(r, r) for r in sorted(roots)]
    seen_start = set()

    def in_img(x):
        return 0 <= x < len(valid) and valid[x]

    def record_mem(op, ia, fn, aregs, is_write, mnem):
        addr = None
        m = ABS.match(op)
        if m:
            addr = int(m.group(1), 16)
            if m.group(2) == 'w' and addr >= 0x8000:
                addr |= 0xFFFF0000
                addr &= 0xFFFFFFFF
        else:
            m = DISP_AN.match(op)
            if m and aregs.get(int(m.group(3))) is not None:
                d = int(m.group(2), 16) * (-1 if m.group(1) else 1)
                addr = aregs[int(m.group(3))] + d
            else:
                m = REG_AN.match(op)
                if m:
                    r = int(m.group(1) or m.group(2))
                    if aregs.get(r) is not None:
                        addr = aregs[r]
        if addr is None:
            return
        addr &= 0xFFFFFFFF
        if 0xDFF000 <= addr < 0xE00000:
            hw[fn][addr].add(ia)
        elif 0xBFD000 <= addr < 0xBFF000 or 0xBFE001 <= addr <= 0xBFEF01:
            hw[fn][addr].add(ia)
        if addr < 0x400 and is_write:
            vec_writes.append((ia, addr, mnem))
        if is_write:
            abs_writes[addr].add(ia)
        else:
            abs_reads[addr].add(ia)

    def drain():
        while queue:
            start, fn = queue.pop()
            if start in seen_start or not in_img(start) or start & 1:
                continue
            seen_start.add(start)
            pc = start
            aregs = {}
            while in_img(pc) and in_img(pc + 1):
                if pc in insns and pc != start:
                    break
                code = bytes(mem[pc:pc + 10])
                ins = next(md.disasm(code, pc, 1), None)
                if ins is None or ins.mnemonic in ('dc.w', '.byte'):
                    break
                ops = split_ops(ins.op_str)
                insns[pc] = (ins.size, ins.mnemonic, ins.op_str)
                for om in re.finditer(r'(?:#|^|, )\$([0-9a-f]{3,6})(?:\.l)?(?=,|$)', ins.op_str):
                    if re.match(r'^(move|movea|lea|pea|cmpi|cmpa)', ins.mnemonic) and ins.mnemonic.endswith(('.l', 'a', '.l')) or ins.mnemonic.startswith(('lea', 'pea')):
                        tv = int(om.group(1), 16)
                        if 0x400 <= tv < 0x80000 and not tv & 1 and (ins.op_str.find('#$' + om.group(1)) >= 0 or ins.mnemonic.startswith(('lea', 'pea')) and '.l' in ins.op_str):
                            imm_addrs[tv].add(pc)
                func_of.setdefault(pc, fn)
                mn = ins.mnemonic
                base_mn = re.sub(r'\.[bwl]$', '', mn)
                # operand memory tracking
                nops = len(ops)
                for idx, op in enumerate(ops):
                    wr = (idx == nops - 1) and not base_mn.startswith(NOWRITE) and nops >= 1 \
                        and not (base_mn.startswith('move') and False)
                    if nops == 1 and base_mn in ('clr', 'neg', 'negx', 'not', 'tas', 'nbcd') or \
                            (nops == 1 and re.match(r'^s(t|f|[a-z]{2})$', base_mn)):
                        wr = True
                    elif nops == 1:
                        wr = False
                    record_mem(op, pc, fn, aregs, wr, base_mn)
                # a-reg constant tracking
                if base_mn in ('lea', 'movea', 'move') and nops == 2 and re.match(r'^a[0-7]$', ops[1]):
                    r = int(ops[1][1])
                    m = ABS.match(ops[0]) if base_mn == 'lea' else IMM.match(ops[0])
                    aregs[r] = int(m.group(1), 16) if m else None
                    if m and base_mn == 'lea' and ABS.match(ops[0]).group(2) == 'w':
                        pass
                elif nops == 2 and re.match(r'^a[0-7]$', ops[1]) and base_mn in (
                        'adda', 'suba', 'addq', 'subq', 'move'):
                    aregs[int(ops[1][1])] = None
                elif nops == 1 and re.match(r'^\(a[0-7]\)\+$', ops[0]):
                    pass
                groups = [ins.group_name(g) for g in ins.groups]
                target = None
                if 'jump' in groups or 'branch_relative' in groups:
                    last = ops[-1]
                    m = ABS.match(last)
                    mp = re.match(r'^\$([0-9a-f]+)$', last)
                    if m:
                        target = int(m.group(1), 16)
                    elif mp:
                        target = int(mp.group(1), 16)
                if base_mn in ('bsr', 'jsr'):
                    if target is not None:
                        if in_img(target):
                            funcs.add(target)
                            queue.append((target, target))
                    else:
                        dyn_sites.append((pc, mn, ins.op_str))
                    pc += ins.size
                    continue
                if base_mn in ('bra', 'jmp'):
                    if target is not None:
                        if in_img(target):
                            labels.add(target)
                            queue.append((target, fn))
                    else:
                        if '(pc' in ins.op_str:
                            tables.append((pc, mn, ins.op_str))
                        dyn_sites.append((pc, mn, ins.op_str))
                    break
                if base_mn.startswith('b') and 'branch_relative' in groups or base_mn.startswith('db'):
                    if target is not None and in_img(target):
                        labels.add(target)
                        queue.append((target, fn))
                    pc += ins.size
                    continue
                if base_mn in ('rts', 'rte', 'rtr', 'illegal', 'stop', 'reset'):
                    break
                pc += ins.size


    imm_addrs = defaultdict(set)    # candidate code address -> {insn addr}
    addr_taken = {}                 # validated candidates -> how found

    def plausible(ad):
        if ad & 1 or not in_img(ad) or ad < 0x400:
            return False
        pc2, n = ad, 0
        while n < 80 and in_img(pc2) and in_img(pc2 + 1):
            ins2 = next(md.disasm(bytes(mem[pc2:pc2 + 10]), pc2, 1), None)
            if ins2 is None or ins2.mnemonic in ('dc.w', '.byte', 'illegal', 'reset', 'stop', 'trap'):
                return False
            b2 = re.sub(r'\.[bwl]$', '', ins2.mnemonic)
            if b2 in ('rts', 'rte', 'rtr', 'bra', 'jmp'):
                return n >= 1 or b2 in ('rts', 'rte')
            pc2 += ins2.size
            n += 1
        return False

    swept = [False]
    primary = set()
    drain()
    for _round in range(8):
        new_roots = []
        cands = set()
        for t, srcs in imm_addrs.items():
            cands.add((t, 'immediate'))
        # long-word tables in uncovered memory: runs of >=3 plausible code pointers
        covered_now = set()
        for ad_, (sz_, _, _) in insns.items():
            covered_now.update(range(ad_, ad_ + sz_))
        for b_, e_ in segs:
            p_ = b_
            while p_ + 4 <= e_:
                if p_ in covered_now:
                    p_ += 2
                    continue
                run = []
                q_ = p_
                while q_ + 4 <= e_ and q_ not in covered_now:
                    v_ = int.from_bytes(mem[q_:q_ + 4], 'big')
                    if v_ & 1 or not in_img(v_) or v_ < 0x400 or v_ >= 0x80000:
                        break
                    run.append(v_)
                    q_ += 4
                if len(run) >= 3 and all(plausible(v_) for v_ in run):
                    for v_ in run:
                        cands.add((v_, 'table@%x' % p_))
                    p_ = q_
                else:
                    p_ += 2
        for t, how in cands:
            if t not in addr_taken and t not in funcs and t not in abs_writes and t not in abs_reads \
                    and plausible(t):
                addr_taken[t] = how
                new_roots.append(t)
        # gap sweep: unreached bytes inside declared code zones that decode cleanly to a terminator
        if not new_roots and not swept[0]:
            swept[0] = True
            primary.update(insns)
            cov2 = set()
            for ad_, (sz_, _, _) in insns.items():
                cov2.update(range(ad_, ad_ + sz_))
            for z in a.codezone:
                lo_, hi_ = (int(x, 16) for x in z.split('-'))
                p2 = lo_ & ~1
                while p2 < hi_:
                    if p2 in cov2 or p2 in funcs:
                        p2 += 2
                        continue
                    w_ = int.from_bytes(mem[p2:p2 + 2], 'big')
                    # skip zero padding and table-ish words
                    if w_ != 0 and p2 not in abs_writes and p2 not in abs_reads and plausible(p2) and p2 not in addr_taken:
                        addr_taken[p2] = 'gap-sweep'
                        new_roots.append(p2)
                        funcs.add(p2)
                        queue.append((p2, p2))
                        drain()
                        cov2 = set()
                        for ad_, (sz_, _, _) in insns.items():
                            cov2.update(range(ad_, ad_ + sz_))
                    p2 += 2
        if not new_roots:
            break
        for t in new_roots:
            funcs.add(t)
            queue.append((t, t))
        drain()


    # function sizes (by sorted starts)
    fl = sorted(funcs)
    result = {
        'segments': segs,
        'functions': [],
        'labels': sorted(labels),
        'address_taken': [{'addr': x, 'how': h} for x, h in sorted(addr_taken.items())],
        'dynamic_sites': [{'addr': x, 'mnemonic': m, 'ops': o} for x, m, o in sorted(dyn_sites)],
        'pc_table_sites': [{'addr': x, 'mnemonic': m, 'ops': o} for x, m, o in sorted(tables)],
        'vector_writes': [{'addr': x, 'vec': v, 'mnemonic': m} for x, v, m in sorted(vec_writes)],
    }
    owned = defaultdict(list)
    for ad, f in func_of.items():
        owned[f].append(ad)
    for f in fl:
        addrs = sorted(owned.get(f, [f]))
        result['functions'].append({
            'start': f, 'insn_count': len(addrs), 'last': addrs[-1],
            'hw': {('%06x' % r): len(s) for r, s in sorted(hw.get(f, {}).items())},
        })
    covered = set()
    for ad, (sz, _, _) in insns.items():
        covered.update(range(ad, ad + sz))
    smc = []
    cov_primary = set()
    for ad, (sz, _, _) in insns.items():
        if ad in primary or not swept[0]:
            cov_primary.update(range(ad, ad + sz))
    for addr, srcs in sorted(abs_writes.items()):
        if addr in covered:
            smc.append({'target': addr, 'writers': sorted(srcs),
                        'confidence': 'high' if addr in cov_primary else 'low(swept-code only)'})
    result['code_writes'] = smc
    result['coverage_bytes'] = len(covered)
    result['insn_total'] = len(insns)
    json.dump(result, open(a.out, 'w'), indent=1)
    # also dump insn listing for browsing
    with open(a.out.replace('.json', '.lst'), 'w') as f:
        for ad in sorted(insns):
            sz, mn, op = insns[ad]
            tag = ''
            if ad in funcs:
                tag = '\n; ---- function %06x ----\n' % ad
            elif ad in labels:
                tag = ';'
            f.write('%s%06x  %s %s\n' % (tag, ad, mn, op))
    print('functions', len(funcs), 'insns', len(insns), 'covered', len(covered), 'of', sum(e - b for b, e in segs),
          'dynamic', len(dyn_sites), 'code-writes', len(smc))


if __name__ == '__main__':
    main()
