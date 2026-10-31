#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Fail if git tracks game data, disk/CD images, audio, ROMs, picture dumps, build output or generated code.

    python3 -I tools/check_no_game_data.py [--repo DIR]    check `git ls-files` (exit 1 on a hit, 77 = no git)
    python3 -I tools/check_no_game_data.py --self-test     unit tests of the rules

The rules work on paths only (plus a size limit for files that are present), so the check needs no game data.
Run by CTest (check_no_game_data) and by the CI hygiene job. Windows: `py -3 -I tools\\check_no_game_data.py`.
"""
import argparse
import os
import subprocess
import sys
import unittest

# Disk and CD images, archives of them, ROMs, raw/decoded audio and picture dumps.
DENY_EXTENSIONS = {
    "adf", "adz", "dms", "ipf", "hdf", "zip", "7z", "rar", "lha", "lzx",  # Amiga disks and archives
    "dsk", "fdi", "scp", "st", "msa",  # other floppy image formats
    "iso", "bin", "cue", "img", "nrg", "ccd", "chd", "mds", "mdf",  # CD images
    "rom", "kick",  # Kickstart / CDTV ROMs
    "wav", "pcm", "flac", "ogg", "mp3", "aiff", "aif", "raw", "m4a", "aac", "opus", "wma",  # audio
    "mod", "xm", "s3m", "it", "med", "ahx", "sid",  # tracker / chip music
    "png", "ppm", "pgm", "bmp", "gif", "tga", "iff", "ilbm", "lbm",  # screenshots, frame and asset dumps
    "jpg", "jpeg", "webp", "tif", "tiff", "psd", "xcf",  # other picture formats (box art, manual scans)
    "exe", "dll", "dylib", "so", "a", "lib", "o", "obj", "dmg", "app", "appimage", "pkg",  # build output
    "tar", "gz", "tgz", "bz2", "xz", "zst",  # release archives
    "mov", "mp4", "m4v", "mkv", "avi", "webm", "mpg", "mpeg",  # screen recordings (they carry the game's graphics and audio)
}
# Directories that only ever hold generated or user-derived files.
DENY_DIRS = ("generated/", "build/", "dist/", ".omc/")
DENY_DIR_PREFIXES = ("build-",)  # build-recomp/, build-x86_64/, ...
# Names of the importer's outputs whatever their extension.
DENY_NAMES = {"game_0400.bin", "disc2.bin", "highscores.bin"}
# Larger tracked files are suspicious (the biggest source file is well under this).
MAX_BYTES = 2 * 1024 * 1024
# Paths that are allowed despite the rules above (keep empty unless reviewed).
ALLOW = set()


def violation(path, size=None):
    """Why `path` (repository-relative, '/' separators) must not be tracked, or None."""
    if path in ALLOW:
        return None
    p = path.replace("\\", "/")
    first = p.split("/", 1)[0] + "/"
    if any(p.startswith(d) for d in DENY_DIRS) or (
        "/" in p and any(first.startswith(x) for x in DENY_DIR_PREFIXES)
    ):
        return "generated or build directory"
    base = p.rsplit("/", 1)[-1]
    if base.lower() in DENY_NAMES:
        return "imported game data"
    if "." in base:
        ext = base.rsplit(".", 1)[-1].lower()
        if ext in DENY_EXTENSIONS:
            return "denied file type ." + ext
    if base.startswith(("x2r_", "x2rc_")) and base.endswith(".cpp"):  # tools/recomp/recomp.py output (disk builds, CDTV release)
        return "translated game code"
    if size is not None and size > MAX_BYTES:
        return "file larger than %d bytes" % MAX_BYTES
    return None


def tracked_files(repo):
    out = subprocess.run(["git", "-C", repo, "ls-files", "-z"], check=True, capture_output=True).stdout
    return [f for f in out.decode("utf-8", "surrogateescape").split("\0") if f]


def check(repo):
    try:
        files = tracked_files(repo)
    except (OSError, subprocess.CalledProcessError) as e:
        print("check_no_game_data: not a git checkout or no git (%s): skipped" % e)
        return 77
    bad = []
    for f in files:
        full = os.path.join(repo, f)
        size = os.path.getsize(full) if os.path.isfile(full) else None
        why = violation(f, size)
        if why:
            bad.append((f, why))
    for f, why in bad:
        print("tracked file not allowed: %s (%s)" % (f, why))
    if bad:
        print("check_no_game_data: %d file(s) must be removed from git (git rm --cached FILE)" % len(bad))
        return 1
    print("check_no_game_data: %d tracked files ok" % len(files))
    return 0


class Rules(unittest.TestCase):
    def test_allowed_sources(self):
        for p in ("runtime/main.cpp", "tools/recomp/recomp.py", "tests/replays/level1_play.txt",
                  "tests/replays/level1_play.hashes", "tests/cdtv/golden/boss1.cues", "vendor/puff/LICENSE",
                  "docs/BUILDING.md", "CMakeLists.txt", "packaging/licenses/SDL3-LICENSE.txt",
                  "runtime/recomp/x2r_runtime.h", "tools/build_all.sh", ".github/workflows/ci.yml"):
            self.assertIsNone(violation(p, 1000), p)

    def test_disk_and_cd_images(self):
        for p in ("Xenon2.zip", "x/Disk 1.ADF", "d.adz", "cd/track02.bin", "cd/x.cue", "x.iso", "kick.rom",
                  "x.dsk", "x.fdi", "x.scp"):
            self.assertIsNotNone(violation(p), p)

    def test_audio_and_pictures(self):
        for p in ("out/game_audio.wav", "cdtv/track03.pcm", "shots/frame.png", "dump/000001.ppm",
                  "cd/track02.m4a", "music/title.mod", "art/box.jpg", "art/sprite.webp", "scans/manual.tiff"):
            self.assertIsNotNone(violation(p), p)

    def test_screen_recordings(self):
        for p in ("x2recomp-audio-capture.mov", "capture/run.mp4", "a.mkv", "b.webm", "c.avi"):
            self.assertIsNotNone(violation(p), p)

    def test_release_archives(self):
        for p in ("dist2/x2recomp-0.1.0-linux-arm64.tar.gz", "x.tar", "x.xz", "x.zst"):
            self.assertIsNotNone(violation(p), p)

    def test_generated_and_build_dirs(self):
        for p in ("generated/assets/manifest.txt", "generated/recomp/x2r_game_0.cpp", "build/x2recomp",
                  "build-recomp/CMakeCache.txt", "dist/x2recomp.zip", "somewhere/x2r_level1_0.cpp", "somewhere/x2rc_level1_0.cpp",
                  "generated/recomp-cdtv/x2rc_tables.cpp"):
            self.assertIsNotNone(violation(p), p)
        self.assertIsNone(violation("build-notes.md"))  # a file, not a build-* directory

    def test_imported_names_and_size(self):
        self.assertIsNotNone(violation("assets/game_0400.bin"))
        self.assertIsNotNone(violation("assets/highscores.bin"))
        self.assertIsNotNone(violation("data/blob.txt", MAX_BYTES + 1))
        self.assertIsNone(violation("data/blob.txt", MAX_BYTES))

    def test_windows_separators(self):
        self.assertIsNotNone(violation("generated\\assets\\disc2.bin"))

    def test_check_skips_without_git(self):
        self.assertEqual(check(os.path.join(os.sep, "nonexistent-x2-dir")), 77)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--self-test", action="store_true", help="run the unit tests of the rules")
    a = ap.parse_args()
    if a.self_test:
        r = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Rules))
        return 0 if r.wasSuccessful() else 1
    return check(os.path.abspath(a.repo))


if __name__ == "__main__":
    sys.exit(main())
