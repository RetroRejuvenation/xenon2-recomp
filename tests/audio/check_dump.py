#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Checks a --dump-audio directory without trusting the runtime: every audio.sfx / audio.speech entry of manifest.json has its WAV
file, the file is mono 16-bit at round(colour clock / ref_period) Hz (or a whole multiple of it (every sample repeated, 44.1-192 kHz)), and the samples are
the 8-bit sample times 256 (repeated by that factor) whose key (FNV-1a 64 over the u32 LE byte count then the bytes, '-', byte count)
is the entry's key. Prints the keys, one 'sfx|speech KEY [cd0_file]' line each, with --list. Usage: check_dump.py DIR [--list]"""
import json
import math
import struct
import sys
import wave

CCK = 3546895


def fnv1a64(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    root = sys.argv[1]
    listing = "--list" in sys.argv[2:]
    m = json.load(open(root + "/manifest.json"))
    assert m["format"] == "x2-asset-pack" and m["version"] == 2, "not a version 2 pack"
    audio = m.get("audio", {})
    problems = []
    lines = []
    for section in ("sfx", "speech"):
        for key, e in sorted(audio.get(section, {}).items()):
            lines.append("%s %s%s" % (section, key, " " + e["cd0_file"] if "cd0_file" in e else ""))
            try:
                w = wave.open(root + "/" + e["file"], "rb")
            except Exception as ex:  # noqa: BLE001
                problems.append("%s: %s" % (key, ex))
                continue
            ref = e["ref_period"]
            ideal = CCK / ref
            base = 1 if ideal >= 44100 else math.ceil(44100 / ideal)
            rep, rate, best = base, 0, 1e9
            for r in range(base, base + 16):
                if ideal * r > 192000:
                    break
                cand = int(ideal * r + 0.5)
                err = abs(cand - ideal * r) / (ideal * r)
                if err < best:
                    best, rep, rate = err, r, cand
                if err < 1e-5:
                    break
            frames = w.readframes(w.getnframes())
            if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (1, 2, rate):
                problems.append("%s: format %s, want mono 16-bit %d Hz" % (key, w.getparams()[:3], rate))
                continue
            samples = struct.unpack("<%dh" % (len(frames) // 2), frames)
            raw = bytearray()
            for i in range(0, len(samples), rep):
                s = samples[i]
                if s % 256 or any(samples[i + j] != s for j in range(rep)):
                    problems.append("%s: not an 8-bit sample times 256 (repeat %d) at %d" % (key, rep, i))
                    break
                raw.append(s // 256 & 0xFF)
            else:
                want_hash, want_bytes = key.split("-")
                if len(raw) != int(want_bytes) or e.get("bytes") != len(raw):
                    problems.append("%s: %d bytes, key says %s" % (key, len(raw), want_bytes))
                elif "%016x" % fnv1a64(bytes(raw), fnv1a64(struct.pack("<I", len(raw)))) != want_hash:
                    problems.append("%s: the samples do not hash to the key" % key)
    if listing:
        print("\n".join(lines))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("%d entries ok" % len(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
