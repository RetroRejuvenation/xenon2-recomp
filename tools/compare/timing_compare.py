#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare the game's logic-step timing between x2recomp and the x2ref Amiberry reference.

    python3 -I tools/compare/timing_compare.py REF_RUN_DIR OURS_WATCH_LOG [--from G] [--to G] [--list N]

REF_RUN_DIR/frames.log is a harness run with X2REF_WATCH including 404,41C,1FEE,1FF0,1FF2,1FF4 and X2REF_IDLE=2034-2042
(docs/re-notes/reference-compare.md section 10); OURS_WATCH_LOG is `x2recomp --watch-log` of the same replay with the
same --watch list. Both hold, per frame, the chip words at the start of that frame, the lines the CPU spent in the
main loop's swap wait ($2034-$2041, "idle") and the lines the blitter was busy ("blt").

Frames are matched by game frame (the reference's last column; its disc loads are paused by WAITLOAD). Reported:
- RNG: game frames at which the generator state $1FEE-$1FF4 is identical, and the first that differs;
- logic steps: $404 is the VBL count ($41C) at the last buffer swap, so each change of $404 is one logic step; the
  number of steps since the start of each run, and the first game frame at which the counts differ;
- RNG calls: the generator is stepped from its power-on seed (the digits of pi, docs/re-notes/ghidra-code-map.md)
  to number every state, so each frame's state gives the number of calls made so far; the difference between the
  runs at each game frame (0 = the same position in the sequence);
- idle and blitter-busy lines per 500-frame window.
"""
import argparse
import sys


def rng_step(s0, s1):
    """rng_next ($1F50): two addx.w chains over the 64-bit state $1FEE (s0) / $1FF2 (s1)."""
    lo = (s0 & 0xFFFF) * 2
    x, lo = lo >> 16, lo & 0xFFFF
    r = lo + (s1 & 0xFFFF) + x
    lo0, x = r & 0xFFFF, r >> 16
    hi0 = s0 >> 16
    r = hi0 + (s1 >> 16) + x
    return ((r & 0xFFFF) << 16) | lo0, (hi0 << 16) | lo


def rng_index(limit=1_500_000):
    st, idx = (0x63141592, 0x35358979), {}
    for i in range(limit):
        idx.setdefault(st, i)
        st = rng_step(*st)
    return idx


def load(path):
    cols, rows = None, []
    for line in open(path):
        p = line.split()
        if line.startswith("#"):
            cols = p[1:]
            continue
        rows.append(dict(zip(cols, p)))
    return cols, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("ours")
    ap.add_argument("--from", dest="g0", type=int, default=0)
    ap.add_argument("--to", dest="g1", type=int, default=1 << 30)
    ap.add_argument("--list", type=int, default=0, help="print the first N differing frames")
    a = ap.parse_args()

    _, ref_rows = load(a.ref.rstrip("/") + "/frames.log")
    ref = {}
    for r in ref_rows:
        g = int(r["game_frame"])
        if g >= 0 and g not in ref:
            ref[g] = r
    _, our_rows = load(a.ours)
    ours = {int(r["frame"]): r for r in our_rows}
    frames = [g for g in sorted(ref) if g in ours and a.g0 <= g <= a.g1]
    if not frames:
        sys.exit("no common game frames")

    rng = ("w1FEE", "w1FF0", "w1FF2", "w1FF4")
    same_rng = [g for g in frames if all(ref[g][k] == ours[g][k] for k in rng)]
    first_rng = next((g for g in frames if g not in set(same_rng)), None)

    # Logic steps: count $404 changes from the first common frame on.
    def steps(table):
        out, n, last = {}, 0, None
        for g in frames:
            v = table[g]["w0404"]
            if last is not None and v != last:
                n += 1
            last = v
            out[g] = n
        return out

    rs, os_ = steps(ref), steps(ours)
    idx = rng_index()

    def calls(r):
        return idx.get((int(r["w1FEE"] + r["w1FF0"], 16), int(r["w1FF2"] + r["w1FF4"], 16)))

    dc = {g: calls(ours[g]) - calls(ref[g]) for g in frames if calls(ours[g]) is not None and calls(ref[g]) is not None}
    first_step = next((g for g in frames if rs[g] != os_[g]), None)
    idle_r = sum(int(ref[g].get("idle", 0)) for g in frames)
    idle_o = sum(int(ours[g].get("idle", 0)) for g in frames)
    blt_r = sum(int(ref[g].get("blt", 0)) for g in frames)
    blt_o = sum(int(ours[g].get("blt", 0)) for g in frames)
    print(f"game frames {frames[0]}-{frames[-1]} ({len(frames)} compared)")
    print(f"RNG state identical: {len(same_rng)}/{len(frames)}; first difference at game frame {first_rng}")
    zero = sum(1 for v in dc.values() if v == 0)
    print(f"RNG calls (ours - ref) known at {len(dc)} frames: equal at {zero}, |diff| <= 2 at "
          f"{sum(1 for v in dc.values() if abs(v) <= 2)}, max {max(dc.values(), default=0)}, "
          f"min {min(dc.values(), default=0)}, final {dc.get(max(dc, default=0))}")
    print(f"logic steps: ref {rs[frames[-1]]}, ours {os_[frames[-1]]}; step counts first differ at game frame "
          f"{first_step}")
    print(f"idle lines (swap wait): ref {idle_r}, ours {idle_o}; blitter-busy lines: ref {blt_r}, ours {blt_o}")
    # Per-window summary (every 500 game frames): step counts and idle lines.
    print("\n| game frames | steps ref | steps ours | idle ref | idle ours | blt ref | blt ours | RNG equal | "
          "RNG calls ours-ref (min/max) |")
    print("|---|---|---|---|---|---|---|---|---|")
    w0 = frames[0]
    while w0 <= frames[-1]:
        w = [g for g in frames if w0 <= g < w0 + 500]
        if w:
            print(f"| {w[0]}-{w[-1]} | {rs[w[-1]] - rs[w[0]]} | {os_[w[-1]] - os_[w[0]]} | "
                  f"{sum(int(ref[g].get('idle', 0)) for g in w)} | {sum(int(ours[g].get('idle', 0)) for g in w)} | "
                  f"{sum(int(ref[g].get('blt', 0)) for g in w)} | {sum(int(ours[g].get('blt', 0)) for g in w)} | "
                  f"{sum(1 for g in w if g in set(same_rng))}/{len(w)} | "
                  f"{min((dc[g] for g in w if g in dc), default='')}/{max((dc[g] for g in w if g in dc), default='')} |")
        w0 += 500
    if a.list:
        print("\nfirst differing frames (game frame: ref / ours $404 $41C rng idle blt):")
        n = 0
        for g in frames:
            r, o = ref[g], ours[g]
            if any(r[k] != o[k] for k in rng) or rs[g] != os_[g]:
                print(f"  {g} (calls {dc.get(g)}): {r['w0404']} {r['w041C']} {r['w1FEE']}{r['w1FF0']} {r.get('idle')} {r.get('blt')}  /  "
                      f"{o['w0404']} {o['w041C']} {o['w1FEE']}{o['w1FF0']} {o.get('idle')} {o.get('blt')}")
                n += 1
                if n >= a.list:
                    break


if __name__ == "__main__":
    main()
