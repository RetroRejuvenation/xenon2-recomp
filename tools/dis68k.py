#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""68000 disassembler via libcapstone (ctypes).

usage: python3 -I tools/dis68k.py FILE BASE_ADDR [START_OFF [COUNT_BYTES]]
Linear sweep; undecodable words are emitted as 'dc.w'.

Set X2_CAPSTONE to a specific library to override the search below. recomp.py's length cross-check is
optional: it records "available": false with the load error when capstone is not there.
"""
import ctypes
import ctypes.util
import os
import sys

# A pinned Homebrew path used to be the only candidate here, so the cross-check silently did nothing on
# Linux and Windows, and on macOS as soon as Homebrew moved past that version.
def _candidates():
    if os.environ.get("X2_CAPSTONE"):
        yield os.environ["X2_CAPSTONE"]
    found = ctypes.util.find_library("capstone")
    if found:
        yield found
    yield from ("libcapstone.dylib", "libcapstone.so.5", "libcapstone.so", "capstone.dll")
    yield "/opt/homebrew/lib/libcapstone.dylib"


def _load():
    tried = []
    for name in _candidates():
        try:
            return ctypes.CDLL(name)
        except OSError as e:
            tried.append("%s: %s" % (name, e))
    raise OSError("libcapstone not found; set X2_CAPSTONE. Tried:\n  " + "\n  ".join(tried))


class Insn(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint), ("address", ctypes.c_uint64), ("size", ctypes.c_uint16),
                ("bytes", ctypes.c_ubyte * 24), ("mnemonic", ctypes.c_char * 32),
                ("op_str", ctypes.c_char * 160), ("detail", ctypes.c_void_p)]


class Dis:
    def __init__(self):
        self.cs = _load()
        self.cs.cs_disasm.restype = ctypes.c_size_t
        self.cs.cs_disasm.argtypes = [ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64,
                                      ctypes.c_size_t, ctypes.POINTER(ctypes.POINTER(Insn))]
        self.cs.cs_free.argtypes = [ctypes.POINTER(Insn), ctypes.c_size_t]
        h = ctypes.c_size_t()
        if self.cs.cs_open(8, 1 << 1, ctypes.byref(h)):
            raise RuntimeError("cs_open failed")
        self.h = h.value

    def one(self, data, addr):
        out = ctypes.POINTER(Insn)()
        n = self.cs.cs_disasm(self.h, data, len(data), addr, 1, ctypes.byref(out))
        if n == 0:
            return 2, "dc.w", "$%04x" % int.from_bytes(data[:2], "big")
        i = out[0]
        r = (i.size, i.mnemonic.decode(), i.op_str.decode())
        self.cs.cs_free(out, n)
        return r

    def sweep(self, data, base, start=0, count=None):
        end = len(data) if count is None else min(len(data), start + count)
        o = start
        while o < end:
            sz, m, ops = self.one(bytes(data[o:o + 10]), base + o)
            yield base + o, bytes(data[o:o + sz]), m, ops
            o += sz


def main(argv):
    data = open(argv[1], "rb").read()
    base = int(argv[2], 0)
    start = int(argv[3], 0) - base if len(argv) > 3 and int(argv[3], 0) >= base else (int(argv[3], 0) if len(argv) > 3 else 0)
    cnt = int(argv[4], 0) if len(argv) > 4 else None
    for a, b, m, ops in Dis().sweep(data, base, start, cnt):
        print("%08x  %-14s %s %s" % (a, b.hex(), m, ops))


if __name__ == "__main__":
    main(sys.argv)
