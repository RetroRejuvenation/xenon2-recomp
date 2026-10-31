# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Static model of the first two layers of Rob Northen's Copylock ST as used on the Xenon 2 Image Works disk 1
# (docs/re-notes/atari-st-image-works.md). Everything here was derived by running the loader in tools/acq/stemu.c and
# checking the result against the emulated memory; no disk bytes are stored in this file.
#
#   key        six rounds of  d0 += long; d0 = rol.l(d0, 3)  over the first 24 bytes of track 0 sector 6, the
#              sector that is written with a bad CRC and holds "Rob Northen Comp" (the check also needs that
#              sector to read about 2% slower than a normal one: the physical track is long, which only a flux or
#              Pasti (.stx) image of the original disk keeps).
#   layer 1    the loader's stage 1 (tracks 1-32, 163,840 bytes, loaded at $40000) re-runs itself once with its
#              body replaced: a trace-vector handler that is placed at (key ^ C1) decrypts [$406f0, $67cfe) with a
#              cumulative XOR seeded with (key & $FF) ^ $B8 and stores the result at $40000.
#   layer 2    the second body decrypts later code in place with the same key, and a second cumulative XOR (byte i+1
#              ^= new byte i) runs over [$40a7c, $ffffc).
#   trace      inside the layers, an instruction is stored encrypted: its first longword is XORed with
#              swap(~(the raw longword just before it)); only the first 4 bytes of an instruction are encrypted.
#
# The third layer (the real loader) was NOT decoded: after layer 2 the loader restarts at $40000 with the trace bit
# set and needs a trace vector that the emulated TOS state does not provide.
import hashlib
import struct
import sys

SIGNATURE = b"Rob Northen Comp"
# Two 32-bit immediates from the loader (key ^ first = the layer 1 handler address, key ^ second = the code the second
# body patches and runs) are NOT stored in the repository: with a known address each implies the disk's key. They are read
# from a local JSON file named by $X2_ST_COPYLOCK_CONSTS ({"layer1_handler_const": "0x...", "layer2_target_const": "0x..."}).
import json
import os


def _local_consts():
    path = os.environ.get("X2_ST_COPYLOCK_CONSTS")
    if not path:
        return None, None
    with open(path) as f:
        c = json.load(f)
    return int(str(c["layer1_handler_const"]), 0), int(str(c["layer2_target_const"]), 0)


LAYER1_HANDLER_CONST, LAYER2_TARGET_CONST = _local_consts()
LAYER1_INIT_XOR = 0xB8
STAGE1_BASE = 0x40000
STAGE1_TRACKS = range(1, 33)
LAYER1_SRC = 0x406F0
LAYER1_END = 0x67CFE


def rol32(v, n):
    v &= 0xFFFFFFFF
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def has_signature(sector):
    return len(sector) >= 24 and sector[:16] == SIGNATURE


def key_from_sector(sector):
    """The 32-bit key: six rounds of add.l / rol.l #3 over the first 24 bytes of the key sector."""
    if len(sector) < 24:
        raise ValueError("the key sector is shorter than 24 bytes")
    d0 = 0
    for i in range(6):
        d0 = rol32((d0 + struct.unpack(">I", sector[4 * i:4 * i + 4])[0]) & 0xFFFFFFFF, 3)
    return d0


def handler_address(key, const):
    return (key ^ const) & 0xFFFFFFFF


def prefix_xor(src, seed):
    """Cumulative XOR: out[i] = seed ^ src[0] ^ ... ^ src[i] (the layer 1 loop: eor.b d1,d0 ; move.b d0,(a0)+)."""
    out = bytearray(len(src))
    acc = seed & 0xFF
    for i, b in enumerate(src):
        acc ^= b
        out[i] = acc
    return bytes(out)


def chain_xor_in_place(buf, start=0):
    """Layer 2 loop (move.b (a0)+,d1 ; eor.b d1,(a0)): byte i+1 is XORed with the new byte i, from `start`."""
    b = bytearray(buf)
    for i in range(start, len(b) - 1):
        b[i + 1] ^= b[i]
    return bytes(b)


def layer1_decrypt(stage1, key):
    """[$406f0, $67cfe) of the stage 1 block decrypted; the loader stores it at $40000."""
    lo, hi = LAYER1_SRC - STAGE1_BASE, LAYER1_END - STAGE1_BASE
    if len(stage1) < hi:
        raise ValueError("stage 1 is shorter than $%X bytes" % hi)
    return prefix_xor(stage1[lo:hi], (key & 0xFF) ^ LAYER1_INIT_XOR)


def trace_plain_first_long(raw_long, raw_prev_long):
    """The first longword of an instruction as the trace handler decrypts it."""
    k = (~raw_prev_long) & 0xFFFFFFFF
    k = ((k << 16) | (k >> 16)) & 0xFFFFFFFF
    return raw_long ^ k


def sha1(b):
    return hashlib.sha1(b).hexdigest()


def report(stx_path):
    """Print the verifiable facts about a Pasti image of an Image Works disk 1 (no bytes leave the process)."""
    import stx
    d = open(stx_path, "rb").read()
    info = stx.parse_stx(d)
    t0 = info["tracks"].get((0, 0))
    sec = None
    if t0:
        for s in t0["secs"]:
            if s["id"][2] == 6:
                sec = s
    if sec is None:
        print("track 0 sector 6 is not in the image: no Copylock key track")
        return 1
    data = stx.sector_data(d, t0, sec)
    print("key sector: signature %s, fdc status %02x (bad CRC bit %s), sha1 %s" % (
        "present" if has_signature(data) else "ABSENT", sec["fdc"], "set" if sec["fdc"] & 0x08 else "clear", sha1(data)[:12]))
    if not has_signature(data):
        return 1
    key = key_from_sector(data)
    print("key %08x" % key)
    if LAYER1_HANDLER_CONST is None:
        print("loader constants not provided ($X2_ST_COPYLOCK_CONSTS): the layer check is skipped")
    else:
        print("layer 1 handler = key ^ const = %08x (expected %05x)" % (handler_address(key, LAYER1_HANDLER_CONST), LAYER1_END))
        print("layer 2 target  = key ^ const = %08x" % handler_address(key, LAYER2_TARGET_CONST))
    raw = stx.raw_image(d)
    stage1 = raw[5120:5120 + 5120 * len(STAGE1_TRACKS)]
    print("stage 1 (tracks 1-32): %d bytes, sha1 %s" % (len(stage1), sha1(stage1)[:12]))
    body = layer1_decrypt(stage1, key)
    print("layer 1 body: %d bytes, sha1 %s" % (len(body), sha1(body)[:12]))
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: stcopylock.py DISK1.stx")
        sys.exit(2)
    sys.exit(report(sys.argv[1]))
