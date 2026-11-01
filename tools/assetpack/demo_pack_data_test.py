#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Data test of make_demo_pack.py on a real build (exit 77, "Skipped" in CTest, without the user's imported game).

    demo_pack_data_test.py --exe X2RECOMP --assets DIR --replay SCRIPT --out DIR
                           [--frames N] [--from F] [--to G] [--shots F]

Dumps the build over the replay (`--dump-assets` and `--dump-audio`), builds the demo pack (as a folder and as a stored ZIP),
and checks that
  * the runtime's own `--check-asset-pack ... --against` accepts it with no warning and covers every dumped key;
  * the pack is deterministic (two builds give the same bytes);
  * with the pack the presented pictures change (the replacements are in), identically from the folder and from the ZIP, and
    `--faithful` brings the original pictures back;
  * backgrounds and round stars are used when the dump lists a far layer;
  * `--modern-shots` writes the pictures, and `--audio-out` differs from the plain run, is not silent, and is the same
    from the folder and the ZIP.
Everything is written under --out (the dump holds the user's own graphics: never commit it). Standard library only.
"""
import argparse
import array
import hashlib
import os
import re
import shutil
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))


def fail(msg):
    print("FAILED: %s" % msg)
    sys.exit(1)


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        fail("%s exited with %d:\n%s\n%s" % (what, r.returncode, r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout + r.stderr


def tree_hash(root):
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            path = os.path.join(dirpath, fn)
            h.update(os.path.relpath(path, root).replace(os.sep, "/").encode())
            with open(path, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def file_hash(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def peak(path):
    with wave.open(path, "rb") as w:
        s = array.array("h")
        s.frombytes(w.readframes(w.getnframes()))
    return max((abs(v) for v in s), default=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--assets", required=True)
    ap.add_argument("--replay", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--from", dest="first", type=int, default=1150)
    ap.add_argument("--to", type=int, default=1450)
    ap.add_argument("--shots", type=int, default=1100)
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.assets, "game_0400.bin")) or not (
            os.path.isfile(os.path.join(a.assets, "disc2.bin")) or os.path.isdir(os.path.join(a.assets, "cd0"))):
        print("SKIPPED: no game data in %s" % a.assets)
        return 77
    out = os.path.abspath(a.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    base = [a.exe, "--headless", "--assets", a.assets, "--no-reglog", "--report-every", "0", "--input-script", a.replay,
            "--frames", str(a.frames)]
    py = [sys.executable, "-I", os.path.join(HERE, "make_demo_pack.py")]

    # 1. the dump, the pack, determinism
    dump = os.path.join(out, "dump")
    text = run(base + ["--dump-assets", dump, "--dump-audio", dump], "dump")
    m = re.search(r"dump-assets: (\d+) sprites, (\d+) tiles, (\d+) far lines, (\d+) flashes, (\d+) far layers", text)
    if not m:
        fail("no dump summary: %s" % text[-1500:])
    layers = int(m.group(5))
    demo, demo2, zpath = (os.path.join(out, n) for n in ("demo", "demo2", "demo.zip"))
    run(py + [dump, demo, "--scale", "2"], "make_demo_pack")
    run(py + [dump, demo2, "--scale", "2", "--no-check"], "make_demo_pack (second)")
    run(py + [dump, zpath, "--scale", "2", "--no-check"], "make_demo_pack (zip)")
    if tree_hash(demo) != tree_hash(demo2):
        fail("two builds of the demo pack differ")
    print("demo pack built from the dump (%d far layers), deterministic" % layers)

    # 2. the runtime's validator, as a folder and as a ZIP
    for pack in (demo, zpath):
        v = run([a.exe, "--check-asset-pack", pack, "--against", dump], "--check-asset-pack %s" % os.path.basename(pack))
        if "0 errors, 0 warnings" not in v:
            fail("the runtime reports problems with %s:\n%s" % (pack, v))
        cov = re.search(r"coverage of the dump: (.*)", v)
        if not cov or re.search(r"\b(\d+)/(\d+)", cov.group(1)) is None:
            fail("no coverage line: %s" % v)
        for done, total in re.findall(r"(\d+)/(\d+)", cov.group(1)):
            if int(done) < int(total):  # screens may count above: two dumped screens can share a first block
                fail("the demo pack does not cover the dump: %s" % cov.group(1))
    print("--check-asset-pack: valid, covers the dump (%s)" % cov.group(1))

    # 3. pictures
    def pictures(*extra):
        o = run(base + ["--modern", "interp,scale=2", "--modern-out", "%d-%d:-" % (a.first, a.to)] + list(extra), "modern-out")
        return re.findall(r"presenthash [0-9]+ [0-9]+ [0-9a-f]+", o), o

    plain, _ = pictures()
    if len(plain) < 200:
        fail("only %d presented pictures" % len(plain))
    withpack, o = pictures("--asset-pack", demo)
    zipped, _ = pictures("--asset-pack", zpath)
    faithful, _ = pictures("--asset-pack", demo, "--faithful")
    changed = sum(1 for x, y in zip(plain, withpack) if x != y)
    if changed < len(plain) // 2:
        fail("the demo pack changes only %d of %d pictures" % (changed, len(plain)))
    if zipped != withpack:
        fail("the ZIP gives other pictures than the folder")
    f_plain, _ = pictures("--faithful")
    if faithful != f_plain:
        fail("--faithful does not bring the original pictures back with the pack loaded")
    if layers:
        mm = re.search(r"modern packs: (\d+) steps with a background.*?(\d+) round stars", o)
        if not mm or int(mm.group(1)) < 50 or int(mm.group(2)) < 1:
            fail("backgrounds or round stars not used: %s" % re.findall(r"modern packs:.*", o))
    print("pictures: %d of %d change with the pack, the ZIP is identical, --faithful restores the original" % (changed, len(plain)))

    # 4. modern-shots
    shots = os.path.join(out, "shots")
    run(base + ["--modern", "scale=2,upgrade", "--asset-pack", demo, "--modern-shots", "%d:%s" % (a.shots, shots)], "modern-shots")
    names = sorted(os.listdir(shots))
    if "interp_t50.png" not in names or "real_a.png" not in names:
        fail("--modern-shots wrote %s" % names)
    print("--modern-shots wrote %d pictures" % len(names))

    # 5. audio
    wavs = {}
    for name, extra in (("plain", []), ("demo", ["--asset-pack", demo]), ("zip", ["--asset-pack", zpath])):
        path = os.path.join(out, name + ".wav")
        run(base + extra + ["--audio-out", path], "audio-out %s" % name)
        wavs[name] = path
    if file_hash(wavs["demo"]) == file_hash(wavs["plain"]):
        fail("the demo pack does not change the audio")
    if file_hash(wavs["demo"]) != file_hash(wavs["zip"]):
        fail("the ZIP gives other audio than the folder")
    if peak(wavs["demo"]) < 1000:
        fail("the demo pack's audio is silent (peak %d)" % peak(wavs["demo"]))
    print("audio: differs from the plain run, folder and ZIP identical, peak %d" % peak(wavs["demo"]))
    print("demo pack data test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
