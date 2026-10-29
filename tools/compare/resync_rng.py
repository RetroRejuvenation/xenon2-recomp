#!/usr/bin/env python3
"""Write a copy of a replay script that re-seeds the game's random generator after every disc load to the state
the reference had at that game frame.

    python3 -I tools/compare/resync_rng.py REPLAY.txt REF_RUN_DIR OUT.txt

Why: x2recomp serves disc sectors instantly, while the reference loads at real floppy speed (hundreds of frames,
held out of game time by the harness's WAITLOAD). The game keeps calling rng_next ($1F50) while it loads, so after
every load the generator is thousands of calls apart in the two runs, and every random outcome after it differs
whatever the CPU and DMA timing. Re-seeding the generator ($1FEE-$1FF4) at the first game frame after each
reference pause removes that one known difference, so a comparison of the rest of the replay measures the timing
model alone (docs/re-notes/reference-compare.md section 10). COMPARISON ONLY: the result is not a faithful run.

REF_RUN_DIR/frames.log must log w1FEE w1FF0 w1FF2 w1FF4 (X2REF_WATCH) with the game frame in the last column.
"""
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    replay, ref, out = sys.argv[1:]
    cols, first = None, {}
    for line in open(ref.rstrip("/") + "/frames.log"):
        p = line.split()
        if line.startswith("#"):
            cols = p[1:]
            continue
        row = dict(zip(cols, p))
        g = int(row["game_frame"])
        if g >= 0 and g not in first:
            first[g] = (int(row["frame"]), row)
    if not all(k in cols for k in ("w1FEE", "w1FF0", "w1FF2", "w1FF4")):
        sys.exit("frames.log does not log the generator state $1FEE-$1FF4")
    # A paused stretch: the game frame is held for many vsyncs (and then jumps), so the vsync distance to the
    # previous logged game frame exceeds the game-frame distance.
    keys = sorted(first)
    resume = [g for p, g in zip(keys, keys[1:]) if first[g][0] - first[p][0] > g - p]
    lines = open(replay).read().rstrip("\n").split("\n")
    lines.append("# resync_rng.py: generator state of the reference after each of its disc loads (comparison only)")
    for g in resume:
        r = first[g][1]
        lines.append(f"{g} poke 0x1FEE 0x{r['w1FEE']}{r['w1FF0']}.l")
        lines.append(f"{g} poke 0x1FF2 0x{r['w1FF2']}{r['w1FF4']}.l")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"{out}: re-seeded at game frames {resume}")


if __name__ == "__main__":
    main()
