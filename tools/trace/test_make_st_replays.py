#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Checks on the Atari ST replay corpus that need no game data and no Hatari (ctest st_replay_scripts):

  * the committed scripts are exactly what tools/trace/make_st_replays.py writes (a script edit without a re-record, or a generator
    edit without regenerating, fails here);
  * every generated script has a hash manifest of the right shape (a frame hash and an audio hash every 500 frames and at the last
    frame) and every manifest has a script;
  * the Hatari conversion: joystick edges, held pokes, object pokes, shifts and the fire chain that runs through a shift.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import make_st_replays as g  # noqa: E402

CORPUS = os.path.join(ROOT, "tests", "replays", "st")
HAND_WRITTEN = {"ending_l5", "hold_fire", "level2"}  # the S5 hook tests' scripts, not generated


class Corpus(unittest.TestCase):
    def test_scripts_match_generator(self):
        for name, build in g.REPLAYS.items():
            want = build().text()
            with open(os.path.join(CORPUS, name + ".txt"), newline="") as f:
                self.assertEqual(f.read(), want, f"{name}.txt differs from the generator: run make_st_replays.py")

    def test_manifests(self):
        names = set(g.REPLAYS)
        for f in os.listdir(CORPUS):
            if f.endswith(".hashes"):
                self.assertIn(f[:-7], names, f"{f} has no generated script")
        for name in names:
            text = open(os.path.join(CORPUS, name + ".txt")).read()
            n = g.total_frames(text)
            frames = list(range(500, n, 500)) + [n - 1]
            lines = open(os.path.join(CORPUS, name + ".hashes")).read().split("\n")
            lines = [x for x in lines if x]
            want = []
            for fr in frames:
                want += [r"framehash %d [0-9a-f]{16}" % fr, r"audiohash %d [0-9a-f]{16}" % fr]
            self.assertEqual(len(lines), len(want), name)
            for got, rx in zip(lines, want):
                self.assertRegex(got, "^" + rx + "$", name)

    def test_hand_written_scripts_untouched(self):
        for name in HAND_WRITTEN:
            self.assertTrue(os.path.exists(os.path.join(CORPUS, name + ".txt")))
            self.assertNotIn(name, g.REPLAYS)

    def test_script_syntax(self):
        ok = re.compile(r"^(\d+ (joy [UDLRF]+ \d+|fire \d+|key [0-9a-f]{2} \d+|poke \$[0-9A-F]+ \$[0-9A-F]+\.[bwl]( \d+)?|"
                        r"pokeobj \$[0-9A-F]+ \$[0-9A-F]+ \$[0-9A-F]+\.[bwl]( \d+)?)( every \d+ until \d+)?|frames \d+|#.*)$")
        for name in g.REPLAYS:
            frames = None
            for line in open(os.path.join(CORPUS, name + ".txt")):
                line = line.rstrip("\n")
                self.assertRegex(line, ok, f"{name}: {line}")
                if line.startswith("frames "):
                    frames = int(line.split()[1])
            self.assertIsNotNone(frames, name)
            for line in open(os.path.join(CORPUS, name + ".txt")):
                m = re.match(r"(\d+) ", line)
                if m:
                    self.assertLess(int(m.group(1)), frames, f"{name}: event after the last frame: {line}")


class HatariConversion(unittest.TestCase):
    def test_edges(self):
        out = g.to_hatari("100 joy LF 5\n200 fire 3\n300 key 19 6\nframes 400\n", 10).split("\n")
        self.assertIn("110 JOY1_LEFT 1", out)
        self.assertIn("115 JOY1_LEFT 0", out)
        self.assertIn("110 JOY1_FIRE 1", out)
        self.assertIn("210 JOY1_FIRE 1", out)
        self.assertIn("213 JOY1_FIRE 0", out)
        self.assertIn("310 KEY19 1", out)
        self.assertIn("316 KEY19 0", out)

    def test_pokes(self):
        out = g.to_hatari("50 poke $CC6 $27.w 3\n60 pokeobj $50396 $32 $1.w 2\n70 poke $C9A $30D40.l\nframes 100\n", 0).split("\n")
        self.assertEqual([x for x in out if " POKE " in x], ["50 POKE CC6 27 2", "51 POKE CC6 27 2", "52 POKE CC6 27 2", "70 POKE C9A 30D40 4"])
        self.assertEqual([x for x in out if "POKEOBJ" in x], ["60 POKEOBJ 50396 32 1 2", "61 POKEOBJ 50396 32 1 2"])

    def test_shifts(self):
        out = g.to_hatari("100 fire 2\n300 fire 2\nframes 400\n", 436, [(200, 800)]).split("\n")
        self.assertIn("536 JOY1_FIRE 1", out)
        self.assertIn("1536 JOY1_FIRE 1", out)

    def test_fire_chain_runs_through_a_shift(self):
        text = "100 fire 2 every 50 until 200\nframes 400\n"
        plain = g.to_hatari(text, 0).split("\n")
        longer = g.to_hatari(text, 0, [(230, 500)]).split("\n")
        n_plain = sum(1 for x in plain if x.endswith("FIRE 1"))
        n_long = sum(1 for x in longer if x.endswith("FIRE 1"))
        self.assertEqual(n_plain, 3)
        self.assertGreaterEqual(n_long, 3 + 9)

    def test_corpus_converts(self):
        for name in g.REPLAYS:
            with open(os.path.join(CORPUS, name + ".txt")) as f:
                text = g.to_hatari(f.read(), 436, [(2200, 800)])
            for line in [x for x in text.splitlines() if x]:
                self.assertRegex(line, r"^\d+ (JOY1_(UP|DOWN|LEFT|RIGHT|FIRE) [01]|KEY[0-9A-F]{2} [01]|POKE [0-9A-F]+ [0-9A-F]+ [124]|"
                                       r"POKEOBJ [0-9A-F]+ [0-9A-F]+ [0-9A-F]+ [124])$", f"{name}: {line}")


if __name__ == "__main__":
    unittest.main()
