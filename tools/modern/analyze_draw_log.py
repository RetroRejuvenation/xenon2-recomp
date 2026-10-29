#!/usr/bin/env python3
"""Measure what the game draws per logic step, from an `x2recomp --draw-log F-G:FILE` capture.

Usage: python3 -I tools/modern/analyze_draw_log.py LOG [LOG...]

Reports (docs/re-notes/modernisation-design.md):
  - blits per step by drawing routine class, CPU writes into the screen buffers by class;
  - how the background is drawn (far layer ring copy, foreground tile map) and its per-step displacement;
  - object identity: how many object blits carry an object record, how many records persist to the next step,
    and their per-step displacement;
  - objects that appear/disappear at the screen edges (widescreen pop-in), and object positions outside the
    320-pixel window.
"""
import sys
from collections import Counter, defaultdict

FAR = {0x1832, 0x185A, 0x1884}
TILES = range(0x18A0, 0x1EA2)
STARS = range(0x233A, 0x23EE)
HUD_RENDER = range(0x15CE, 0x17D6)


def parse(path):
    steps, cur = [], None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == 'S':
            cur = {'index': int(t[1]), 'first': int(t[2]), 'swap': int(t[3]), 'draw': int(t[5], 16),
                   'vars': [int(v, 16) for v in t[7:15]], 'B': [], 'W': [], 'O': []}
            steps.append(cur)
        elif t[0] == 'B':
            cur['B'].append({'pc': int(t[1], 16), 'obj': int(t[3], 16), 'con0': int(t[6], 16), 'w': int(t[10]),
                             'h': int(t[11]), 'apt': int(t[12], 16), 'bpt': int(t[13], 16), 'dpt': int(t[15], 16),
                             'buffer': int(t[22]), 'plane': int(t[23]), 'y': int(t[24]), 'x': int(t[25])})
        elif t[0] == 'W':
            cur['W'].append({'pc': int(t[1], 16), 'addr': int(t[2], 16), 'buffer': int(t[6])})
        elif t[0] == 'O':
            cur['O'].append({'addr': int(t[1], 16), 'list': int(t[2]), 'update': int(t[3], 16),
                             'draw': int(t[4], 16), 'hit': int(t[5], 16), 'gfx': int(t[6], 16),
                             'x': int(t[7]), 'y': int(t[8])})
    # Gameplay steps only (the far-layer copy at $1832 runs): menus, shop and loading screens are left out.
    return [s for s in steps if any(b['pc'] == 0x1832 for b in s['B'])]


def blit_class(b):
    if b['obj']:
        return 'objects (tile routine)' if b['pc'] in TILES else 'objects'
    if b['pc'] in FAR:
        return 'far layer'
    if b['pc'] in TILES:
        return 'tiles'
    return 'other %06X' % b['pc']


def write_class(w):
    if w['pc'] in STARS:
        return 'stars'
    if w['pc'] in HUD_RENDER:
        return 'panel'
    return 'other %06X' % w['pc']


def stats(xs):
    if not xs:
        return 'n/a'
    xs = sorted(xs)
    return 'min %d / median %d / mean %.1f / max %d' % (xs[0], xs[len(xs) // 2], sum(xs) / len(xs), xs[-1])


def analyze(path):
    steps = parse(path)
    print('== %s: %d steps (swaps in frames %d-%d)' % (path, len(steps), steps[0]['swap'], steps[-1]['swap']))
    print('frames per step:', dict(Counter(b['swap'] - a['swap'] for a, b in zip(steps, steps[1:]))))
    print('blits per step:', stats([len(s['B']) for s in steps]))
    cls = Counter()
    for s in steps:
        for b in s['B']:
            cls[blit_class(b)] += 1
    for k, v in cls.most_common(12):
        print('   %-18s %6.1f per step' % (k, v / len(steps)))
    print('blit destinations: draw buffer %d, displayed buffer %d, elsewhere (gutter above the buffer) %d' % (
        sum(b['buffer'] == 0 for s in steps for b in s['B']), sum(b['buffer'] == 1 for s in steps for b in s['B']),
        sum(b['buffer'] == -1 for s in steps for b in s['B'])))
    print('CPU writes into screen buffers per step:', stats([len(s['W']) for s in steps]))
    wc = Counter(write_class(w) for s in steps for w in s['W'])
    for k, v in wc.most_common(6):
        print('   %-18s %6.1f per step' % (k, v / len(steps)))
    print('objects alive per step:', stats([len(s['O']) for s in steps]))

    # Far layer: ring row = (apt - ring base) / 80 of the first copy.
    ring = [s['vars'][0] for s in steps]
    print('far layer ring row ($436) deltas:', dict(Counter((a - b) % 192 for a, b in zip(ring, ring[1:]))))
    # Foreground tiles: match (source, x, plane) between consecutive steps, histogram of dy.
    dys = Counter()
    for a, b in zip(steps, steps[1:]):
        prev = defaultdict(list)
        for t in a['B']:
            if t['pc'] in TILES and t['buffer'] >= 0 and not t['obj']:
                prev[(t['apt'], t['bpt'], t['x'], t['plane'])].append(t['y'])
        step_dy = Counter()
        for t in b['B']:
            if t['pc'] in TILES and t['buffer'] >= 0 and not t['obj']:
                ys = prev.get((t['apt'], t['bpt'], t['x'], t['plane']))
                if ys:
                    step_dy[min((t['y'] - y for y in ys), key=abs)] += 1
        if step_dy:
            dys[step_dy.most_common(1)[0][0]] += 1
    print('foreground tile layer: per-step dy (mode of matched tiles):', dict(dys),
          ' $CD0 deltas:', dict(Counter((a['vars'][1] - b['vars'][1]) & 0xFFFF for a, b in zip(steps, steps[1:]))))

    # Edge columns: share of 16-line tile rows whose leftmost / rightmost 16-pixel column has a foreground tile
    # (the side walls), i.e. how often an extended view would continue a wall rather than open space.
    cover = {0: [], 304: []}
    for s in steps:
        rows = defaultdict(set)
        for t in s['B']:
            if t['pc'] in TILES and not t['obj'] and t['buffer'] == 0 and t['plane'] == 0:
                rows[t['x'] // 16 * 16].add(t['y'] // 16)
        for col in cover:
            cover[col].append(len(rows[col]) / 12.0)
    for col, v in cover.items():
        full = sum(1 for c in v if c >= 0.99) / len(v)
        empty = sum(1 for c in v if c == 0) / len(v)
        print('tile column x=%d: mean coverage %.0f%% of tile rows; fully covered in %.0f%% of steps, empty in %.0f%%' % (
            col, 100 * sum(v) / len(v), 100 * full, 100 * empty))

    # Object identity.
    ob = [b for s in steps for b in s['B'] if b['obj']]
    print('object blits with an identified record: %d of %d non-background blits' % (
        len(ob), sum(1 for s in steps for b in s['B'] if blit_class(b) not in ('far layer', 'tiles'))))
    print('background tile blits with an object record: %d' % sum(1 for b in ob if b['pc'] in TILES))
    persist = moved = total = 0
    dx, dy = Counter(), Counter()
    big = 0
    for a, b in zip(steps, steps[1:]):
        if b['index'] != a['index'] + 1:
            continue
        pa = {o['addr']: o for o in a['O']}
        for o in b['O']:
            total += 1
            p = pa.get(o['addr'])
            if p and p['update'] == o['update']:
                persist += 1
                ddx, ddy = o['x'] - p['x'], o['y'] - p['y']
                if abs(ddx) > 48 or abs(ddy) > 48:
                    big += 1
                    continue
                if ddx or ddy:
                    moved += 1
                dx[ddx] += 1
                dy[ddy] += 1
    print('object records alive in consecutive steps (same record, same update handler): %d of %d; moving %d;'
          ' jumps > 48 px %d' % (persist, total, moved, big))
    print('   dx histogram:', dict(sorted(dx.items())))
    print('   dy histogram:', dict(sorted(dy.items())))

    # Pop-in / pop-out at the edges: objects whose first (last) drawn step has a blit touching x < 16 or > 304.
    first_seen, last_seen, drawn = {}, {}, defaultdict(dict)
    for i, s in enumerate(steps):
        for b in s['B']:
            if b['obj']:
                key = b['obj']
                x0, x1 = b['x'], b['x'] + 16 * b['w'] - 16
                y0 = b['y']
                cur = drawn[i].get(key)
                drawn[i][key] = (min(x0, cur[0]) if cur else x0, max(x1, cur[1]) if cur else x1,
                                 min(y0, cur[2]) if cur else y0)
    spawn_edge, despawn_edge, spawns, despawns = Counter(), Counter(), 0, 0
    for i in range(1, len(steps) - 1):
        if steps[i]['index'] != steps[i - 1]['index'] + 1 or steps[i + 1]['index'] != steps[i]['index'] + 1:
            continue
        for key, (x0, x1, y0) in drawn[i].items():
            if key not in drawn[i - 1]:
                spawns += 1
                spawn_edge[('left' if x0 < 16 else 'right' if x1 > 288 else 'top' if y0 <= 2 else 'inside')] += 1
            if key not in drawn[i + 1]:
                despawns += 1
                despawn_edge[('left' if x0 < 16 else 'right' if x1 > 288 else 'top' if y0 <= 2 else
                              'bottom' if y0 > 170 else 'inside')] += 1
    print('objects starting to be drawn: %d (%s); stopping: %d (%s)' % (spawns, dict(spawn_edge), despawns,
                                                                       dict(despawn_edge)))
    xs = [o['x'] for s in steps for o in s['O'] if o['update'] != 0]
    outside = sum(1 for x in xs if x < 0 or x >= 320)
    print('object x (+$20) over all steps: %s; outside 0..319: %d of %d' % (stats(xs), outside, len(xs)))
    near = Counter()
    for s in steps:
        for o in s['O']:
            for n in (16, 32, 64):
                if -n <= o['x'] < 0 or 320 <= o['x'] < 320 + n:
                    near[n] += 1
    print('object positions within N px outside the window (N: count):', dict(near))
    print()


if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
