"""Checks a --audio-out WAV of the scripted title -> level 1 run (stdlib only; used by game_audio.cmake).

usage: python3 -I check_wav.py FILE.wav FIRST_FRAME:LAST_FRAME[:min_rms] ...
Each range (in 50 Hz emulated frames) must have an RMS of at least min_rms (default 300, int16 units).
The whole file must be 48 kHz stereo 16-bit, contain no samples at int16 full scale and no DC offset.
"""
import array
import math
import sys
import wave

SAMPLES_PER_FRAME = 48000 * 71051 / 3546895  # 313 lines x 227 colour clocks per frame (accurate timing)


def main():
    w = wave.open(sys.argv[1])
    if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (48000, 2, 2):
        print("FAIL: format", w.getparams())
        return 1
    data = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        data.byteswap()
    ok = True
    full = sum(1 for s in data if s >= 32767 or s <= -32768)
    mean_l = sum(data[0::2]) / max(1, len(data) // 2)
    mean_r = sum(data[1::2]) / max(1, len(data) // 2)
    print(f"{len(data) // 2} frames ({len(data) / 2 / 48000:.2f} s), full-scale samples {full}, mean L {mean_l:.1f} R {mean_r:.1f}")
    if full:
        print("FAIL: clipping")
        ok = False
    if abs(mean_l) > 200 or abs(mean_r) > 200:
        print("FAIL: DC offset")
        ok = False
    for spec in sys.argv[2:]:
        parts = spec.split(":")
        a, b = int(parts[0]), int(parts[1])
        min_rms = float(parts[2]) if len(parts) > 2 else 300.0
        seg = data[int(a * SAMPLES_PER_FRAME) * 2:int(b * SAMPLES_PER_FRAME) * 2]
        if not seg:
            print(f"FAIL: frames {a}-{b} beyond end of file")
            ok = False
            continue
        rl = math.sqrt(sum(s * s for s in seg[0::2]) / (len(seg) // 2))
        rr = math.sqrt(sum(s * s for s in seg[1::2]) / (len(seg) // 2))
        good = rl >= min_rms and rr >= min_rms
        print(f"frames {a}-{b}: rms L {rl:.0f} R {rr:.0f} (min {min_rms:.0f}) {'ok' if good else 'FAIL'}")
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
