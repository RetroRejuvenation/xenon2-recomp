# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Approximate YM2149 (Atari ST sound chip) renderer. Input: a log of register writes, one per line,
# "TIME REGISTER VALUE" (what tools/acq/stemu.c --ym-log writes; TIME is an instruction count, converted to seconds
# with --ips). Output: a mono 16-bit WAV. Three square-wave tone channels, the 17-bit noise generator, the mixer
# (register 7), amplitude registers 8-10 with the envelope bit, and the 16 envelope shapes, clocked at 2 MHz as on
# the ST. It does NOT reproduce timer-driven sample playback (the "digi-drum" trick of writing volume registers at
# a high rate from an MFP timer) beyond what the register writes themselves give, and the volume curve is the usual
# logarithmic approximation. A listening aid, not a reference.
import struct
import sys
import wave

import numpy as np

CLOCK = 2_000_000
VOL = np.array([0.0, 0.0137, 0.0205, 0.0291, 0.0423, 0.0618, 0.0847, 0.1369, 0.1691, 0.2647, 0.3527, 0.4499,
                0.5704, 0.6873, 0.8482, 1.0])


def _noise_table():
    lfsr, out = 1, np.empty(131071, dtype=np.uint8)
    for i in range(131071):
        out[i] = lfsr & 1
        bit = (lfsr ^ (lfsr >> 3)) & 1
        lfsr = (lfsr >> 1) | (bit << 16)
    return out


_NOISE = _noise_table()


def envelope_level(shape, pos):
    """Envelope level (0..15) at integer step position pos (0.. from the restart) for the shape in register 13."""
    pos = np.asarray(pos)
    up = np.minimum(pos, 15)
    down = 15 - up
    first = pos < 16
    if shape & 8:
        hold = bool(shape & 1)
        alt = bool(shape & 2)
        attack = bool(shape & 4)
        base_first = up if attack else down
        if hold:
            end_level = (15 if attack else 0)
            if alt:
                end_level = 15 - end_level
            return np.where(first, base_first, end_level)
        if alt:
            cyc = (pos // 16) % 2
            ramp = pos % 16
            rising = np.where(cyc == 0, attack, not attack)
            return np.where(rising, ramp, 15 - ramp)
        ramp = pos % 16
        return ramp if attack else 15 - ramp
    # shapes 0-7: one ramp then silence (0-3 fall, 4-7 rise then drop to zero)
    if shape & 4:
        return np.where(first, up, 0)
    return np.where(first, down, 0)


def render(events, ips, rate=44100, tail=0.5):
    """events: list of (time_in_insns, register, value) sorted by time. Returns int16 samples."""
    if not events:
        return np.zeros(0, dtype=np.int16)
    t0 = events[0][0]
    total = (events[-1][0] - t0) / ips + tail
    n = int(total * rate)
    out = np.zeros(n)
    reg = [0] * 16
    reg[7] = 0xFF
    phase = [0.0, 0.0, 0.0]          # tone phase in clock cycles
    noise_pos = 0.0
    env_pos = 0.0
    cps = CLOCK / rate               # clock cycles per output sample
    times = [int((e[0] - t0) / ips * rate) for e in events] + [n]
    for i, (_t, r, v) in enumerate(events):
        if r < 14:
            reg[r] = v & 0xFF
        s, e = times[i], times[i + 1]
        if r == 13:
            env_pos = 0.0
        if e > n:
            e = n
        if e <= s:
            continue
        m = e - s
        idx = np.arange(m)
        mix = reg[7]
        sig = np.zeros(m)
        nperiod = max(1, reg[6] & 0x1F)
        nstep = cps / (16 * nperiod)
        noise = _NOISE[((noise_pos + idx * nstep).astype(np.int64)) % 131071]
        epe = max(1, reg[11] | (reg[12] << 8))
        estep = cps / (16 * epe)
        envs = envelope_level(reg[13] & 15, (env_pos + idx * estep).astype(np.int64))
        for ch in range(3):
            period = max(1, reg[2 * ch] | ((reg[2 * ch + 1] & 0x0F) << 8))
            half = 8 * period
            tone = (((phase[ch] + idx * cps) // half).astype(np.int64) % 2).astype(np.float64)
            tone_on = not (mix >> ch) & 1
            noise_on = not (mix >> (ch + 3)) & 1
            gate = np.ones(m)
            if tone_on:
                gate = gate * tone
            if noise_on:
                gate = gate * noise
            if not tone_on and not noise_on:
                gate = np.ones(m)
            amp = reg[8 + ch]
            level = VOL[envs] if amp & 0x10 else np.full(m, VOL[amp & 0x0F])
            sig += gate * level
            phase[ch] = (phase[ch] + m * cps) % (2 * half)
        noise_pos = (noise_pos + m * nstep) % 131071
        env_pos += m * estep
        out[s:e] = sig
    out = out / 3.0
    out -= out.mean() if len(out) else 0
    return np.clip(out * 24000, -32768, 32767).astype(np.int16)


def read_log(path):
    ev = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 3 and all(x.isdigit() for x in p):
                ev.append((int(p[0]), int(p[1]), int(p[2])))
    ev.sort(key=lambda x: x[0])
    return ev


def write_wav(path, samples, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.astype("<i2").tobytes())


def main(argv):
    if len(argv) < 3:
        print("usage: ymrender.py LOG OUT.wav [INSTRUCTIONS_PER_SECOND (default 4500000: 90000 per 50 Hz frame)] [START_S [END_S]]")
        return 2
    ev = read_log(argv[1])
    ips = float(argv[3]) if len(argv) > 3 else 4_500_000.0
    if len(argv) > 4:
        lo = ev[0][0] + float(argv[4]) * ips
        hi = ev[0][0] + (float(argv[5]) * ips if len(argv) > 5 else float("inf"))
        # keep the register state: replay everything before lo as writes at lo
        pre = {}
        keep = []
        for t, r, v in ev:
            if t < lo:
                pre[r] = v
            elif t <= hi:
                keep.append((t, r, v))
        ev = [(lo, r, v) for r, v in sorted(pre.items())] + keep
    write_wav(argv[2], render(ev, ips))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
