#!/usr/bin/env python3
"""Instruction-level comparison of the runtime's bus timing against the reference capture.

The reference capture generated/ref/runs/insn/insn.log holds every instruction of a few short
windows (`X2REF_INSNLOG`): `vsync line hpos cycle pc opcode blitbusy`. The runtime logs the same
with `--insn-log F-G:FILE` (without the blitter column, and numbered by game frame). The
reference's vsync is turned into a game frame with frames.log's last column, which accounts for
the load pauses that WAITLOAD/WAITSTEP insert.

The two streams are aligned by (pc, opcode) with a greedy resync, and three things are reported
per window:

  hpos   the beam position at aligned instructions, ours minus the reference (colour clocks);
  dur    the fraction of aligned instruction pairs whose duration equals the reference's exactly,
         split by whether the instruction sits on a line with bitplane DMA (58..257);
  vbl    the line and hpos at which the VBL handler's first instruction ($20C2) runs.

Usage:
  insn_window_cmp.py capture  OUTDIR [--exe build/x2recomp] [--timing accurate]
  insn_window_cmp.py report   OUTDIR [--ref generated/ref/runs/insn/insn.log] [--json FILE]
  insn_window_cmp.py both     OUTDIR [...]
"""
import argparse
import collections
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REF_DEFAULT = ROOT / "generated/ref/runs/insn/insn.log"
REPLAY = ROOT / "tests/replays/game_over.txt"
VBL_PC = 0x20C2
CCK_PER_LINE = 227
# The game frames the reference capture covers (vsyncs 6430-6437, 7100-7104, 8300-8306, 9000-9004).
WINDOWS = [(7, 14), (677, 681), (1171, 1177), (1871, 1875)]
# Lines with bitplane DMA: DIWSTRT/DIWSTOP give 58..257 (timing.md §6).
DISPLAY_LINES = (58, 258)


def win_name(w):
    return "%d-%d" % w


def capture(outdir, exe, timing, replay):
    outdir.mkdir(parents=True, exist_ok=True)
    for w in WINDOWS:
        out = outdir / ("ours.%s.log" % win_name(w))
        cmd = [
            str(exe), "--headless", "--no-reglog", "--report-every", "0",
            "--assets", str(ROOT / "generated/assets"), "--frames", str(w[1] + 2),
            "--input-script", str(replay), "--timing", timing,
            "--insn-log", "%d-%d:%s" % (w[0], w[1], out),
        ]
        r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0:
            sys.exit("capture failed for window %s:\n%s" % (win_name(w), r.stderr.decode()))
        print("  %-10s %7d instructions" % (win_name(w), sum(1 for _ in open(out))))


def game_frames(frameslog):
    """vsync -> game frame, from frames.log's last column (it is not vsync - 6423 after a load
    pause: WAITLOAD/WAITSTEP hold the reference's game time while the floppy runs)."""
    m = {}
    for line in open(frameslog):
        if line.startswith("#"):
            continue
        f = line.split()
        m[int(f[0])] = int(f[-1])
    return m


def load(path, window=None, vmap=None):
    """Rows of (pc, opcode, cycle, line, hpos, game frame). `vmap` translates the file's first
    column (a vsync) into a game frame; without it the column already is one."""
    out = []
    for line in open(path):
        f = line.split()
        v = int(f[0])
        if vmap is not None:
            v = vmap.get(v, -1)
        if window and not (window[0] <= v <= window[1]):
            continue
        out.append((int(f[4], 16), int(f[5], 16), int(f[3]), int(f[1]), int(f[2]), v))
    return out


def align(a, b, lookahead=200):
    """Greedy (pc, opcode) alignment: list of (index in a, index in b)."""
    i = j = 0
    pairs = []
    while i < len(a) and j < len(b):
        if a[i][0] == b[j][0] and a[i][1] == b[j][1]:
            pairs.append((i, j))
            i += 1
            j += 1
            continue
        found = False
        for k in range(1, lookahead):
            if (i + k < len(a) and a[i + k][0] == b[j][0] and a[i + k][1] == b[j][1]
                    and i + k + 1 < len(a) and j + 1 < len(b) and a[i + k + 1][0] == b[j + 1][0]):
                i += k
                found = True
                break
            if (j + k < len(b) and b[j + k][0] == a[i][0] and b[j + k][1] == a[i][1]
                    and j + k + 1 < len(b) and i + 1 < len(a) and b[j + k + 1][0] == a[i + 1][0]):
                j += k
                found = True
                break
        if not found:
            i += 1
            j += 1
    return pairs


def wrap(d):
    """A colour-clock difference folded into (-113, 113]."""
    d %= CCK_PER_LINE
    return d - CCK_PER_LINE if d > CCK_PER_LINE // 2 else d


def report(outdir, refpath, jsonpath=None):
    vmap = game_frames(pathlib.Path(refpath).parent / "frames.log")
    result = {"windows": {}, "total": {}}
    tot = collections.Counter()
    tot_hpos = []
    for w in WINDOWS:
        name = win_name(w)
        ourpath = outdir / ("ours.%s.log" % name)
        if not ourpath.exists():
            sys.exit("missing %s (run the capture step first)" % ourpath)
        ref = load(refpath, w, vmap)
        ours = load(ourpath)
        pairs = align(ref, ours)
        pos = dict(pairs)
        hd = collections.Counter()
        dur = collections.Counter()  # (contended, 'n'|'exact')
        signed = []
        for i, j in pairs:
            d = wrap(ours[j][4] - ref[i][4])
            hd[d] += 1
            signed.append(d)
            # duration: both streams must have the next instruction aligned too
            if i + 1 not in pos or pos[i + 1] != j + 1:
                continue
            dr = ref[i + 1][2] - ref[i][2]
            do = ours[j + 1][2] - ours[j][2]
            if not (0 <= dr <= 300 and 0 <= do <= 300):
                continue  # an interrupt or a log gap landed between the two
            c = DISPLAY_LINES[0] <= ref[i][3] < DISPLAY_LINES[1]
            dur[(c, "n")] += 1
            if dr == do:
                dur[(c, "exact")] += 1
        # The VBL handler's entry position, paired by game frame (a frame can hold more than one
        # entry, and a handler delayed by a critical section starts on a later line).
        def entries(rows):
            m = collections.defaultdict(list)
            for r in rows:
                if r[0] == VBL_PC:
                    m[r[5]].append((r[3], r[4]))
            return m

        re_, oe = entries(ref), entries(ours)
        vbl = []
        for f in sorted(set(re_) & set(oe)):
            for a, b in zip(re_[f], oe[f]):
                vbl.append({"frame": f, "ref": list(a), "ours": list(b)})
        signed.sort()
        w_res = {
            "ref_insns": len(ref),
            "our_insns": len(ours),
            "aligned": len(pairs),
            "hpos_mean": sum(signed) / len(signed) if signed else 0.0,
            "hpos_median": signed[len(signed) // 2] if signed else 0,
            "hpos_hist": dict(sorted(hd.items())),
            "dur_n": dur[(False, "n")] + dur[(True, "n")],
            "dur_exact": dur[(False, "exact")] + dur[(True, "exact")],
            "dur_n_display": dur[(True, "n")],
            "dur_exact_display": dur[(True, "exact")],
            "vbl": vbl,
        }
        result["windows"][name] = w_res
        for k in ("ref_insns", "our_insns", "aligned", "dur_n", "dur_exact", "dur_n_display",
                  "dur_exact_display"):
            tot[k] += w_res[k]
        tot_hpos += signed
    tot_hpos.sort()
    result["total"] = dict(tot)
    result["total"]["hpos_mean"] = sum(tot_hpos) / len(tot_hpos) if tot_hpos else 0.0
    result["total"]["hpos_median"] = tot_hpos[len(tot_hpos) // 2] if tot_hpos else 0
    result["total"]["hpos_hist"] = dict(sorted(collections.Counter(tot_hpos).items()))

    def pct(a, b):
        return "%.2f%%" % (100.0 * a / b) if b else "-"

    print("window     ref insns  aligned   hpos mean  median   dur exact        on display lines")
    for w in WINDOWS:
        r = result["windows"][win_name(w)]
        print("%-10s %9d %8d %11.2f %7d   %5d/%-5d %-7s %5d/%-5d %s" % (
            win_name(w), r["ref_insns"], r["aligned"], r["hpos_mean"], r["hpos_median"],
            r["dur_exact"], r["dur_n"], pct(r["dur_exact"], r["dur_n"]),
            r["dur_exact_display"], r["dur_n_display"],
            pct(r["dur_exact_display"], r["dur_n_display"])))
    t = result["total"]
    print("%-10s %9d %8d %11.2f %7d   %5d/%-5d %-7s %5d/%-5d %s" % (
        "TOTAL", t["ref_insns"], t["aligned"], t["hpos_mean"], t["hpos_median"],
        t["dur_exact"], t["dur_n"], pct(t["dur_exact"], t["dur_n"]),
        t["dur_exact_display"], t["dur_n_display"],
        pct(t["dur_exact_display"], t["dur_n_display"])))
    print("\nhpos difference (ours - reference, colour clocks) over all aligned instructions:")
    for d, n in sorted(t["hpos_hist"].items()):
        if n * 200 >= t["aligned"]:
            print("  %+4d  %6d  %s" % (d, n, "#" * max(1, n * 60 // t["aligned"])))
    print("\nVBL handler first instruction ($%04X), line/hpos:" % VBL_PC)
    for w in WINDOWS:
        for e in result["windows"][win_name(w)]["vbl"]:
            print("  %-10s frame %5d  ref %3d/%-3d   ours %3d/%-3d   delta %+d" % (
                win_name(w), e["frame"], e["ref"][0], e["ref"][1], e["ours"][0], e["ours"][1],
                (e["ours"][0] - e["ref"][0]) * CCK_PER_LINE + e["ours"][1] - e["ref"][1]))
    if jsonpath:
        pathlib.Path(jsonpath).write_text(json.dumps(result, indent=1))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("capture", "report", "both"))
    ap.add_argument("outdir")
    ap.add_argument("--exe", default=str(ROOT / "build/x2recomp"))
    ap.add_argument("--timing", default="accurate")
    ap.add_argument("--replay", default=str(REPLAY))
    ap.add_argument("--ref", default=str(REF_DEFAULT))
    ap.add_argument("--json")
    a = ap.parse_args()
    outdir = pathlib.Path(a.outdir)
    if a.mode in ("capture", "both"):
        capture(outdir, pathlib.Path(a.exe), a.timing, pathlib.Path(a.replay))
    if a.mode in ("report", "both"):
        report(outdir, a.ref, a.json)


if __name__ == "__main__":
    main()
