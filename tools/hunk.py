#!/usr/bin/env python3
"""AmigaDOS hunk (load file) parser.

usage: python3 -I tools/hunk.py FILE [--dump-dir DIR]
Prints hunk table (memory flags, sizes), per-hunk contents (code/data/bss),
relocation counts, symbol/debug hunks, and checks overlay/trailing data.
With --dump-dir writes each code/data hunk as DIR/hunkN.bin and its reloc offsets as hunkN.relocs.txt.
"""
import struct
import sys
import os

NAMES = {
    0x3E7: "HUNK_UNIT", 0x3E8: "HUNK_NAME", 0x3E9: "HUNK_CODE", 0x3EA: "HUNK_DATA",
    0x3EB: "HUNK_BSS", 0x3EC: "HUNK_RELOC32", 0x3ED: "HUNK_RELOC16", 0x3EE: "HUNK_RELOC8",
    0x3EF: "HUNK_EXT", 0x3F0: "HUNK_SYMBOL", 0x3F1: "HUNK_DEBUG", 0x3F2: "HUNK_END",
    0x3F3: "HUNK_HEADER", 0x3F5: "HUNK_OVERLAY", 0x3F6: "HUNK_BREAK",
    0x3F7: "HUNK_DREL32", 0x3F8: "HUNK_DREL16", 0x3F9: "HUNK_DREL8", 0x3FA: "HUNK_LIB",
    0x3FB: "HUNK_INDEX", 0x3FC: "HUNK_RELOC32SHORT", 0x3FD: "HUNK_RELRELOC32",
    0x3FE: "HUNK_ABSRELOC16",
}
MEM = {0: "ANY", 1: "CHIP", 2: "FAST", 3: "EXT"}


class Hunk:
    def __init__(self, idx, size_words, memf):
        self.idx = idx
        self.size_words = size_words
        self.memf = memf
        self.kind = None
        self.data = b""
        self.alloc_words = size_words
        self.relocs = []  # (target_hunk, [offsets])
        self.symbols = []
        self.debug = []
        self.name = None


def parse(buf):
    pos = 0

    def u32():
        nonlocal pos
        v = struct.unpack_from(">I", buf, pos)[0]
        pos += 4
        return v

    if u32() != 0x3F3:
        raise ValueError("not a hunk executable")
    names = []
    while True:
        n = u32()
        if n == 0:
            break
        names.append(buf[pos:pos + 4 * n])
        pos += 4 * n
    table = u32()
    first = u32()
    last = u32()
    hunks = []
    for i in range(last - first + 1):
        s = u32()
        mf = s >> 30
        extra = None
        if mf == 3:
            extra = u32()
        h = Hunk(first + i, s & 0x3FFFFFFF, mf)
        h.memflags_extra = extra
        hunks.append(h)
    info = dict(table=table, first=first, last=last, hunks=hunks)
    cur = 0
    events = []
    while pos < len(buf) and cur < len(hunks):
        t = u32() & 0x3FFFFFFF
        h = hunks[cur]
        if t in (0x3E9, 0x3EA):
            n = u32() & 0x3FFFFFFF
            h.kind = "CODE" if t == 0x3E9 else "DATA"
            h.data = buf[pos:pos + 4 * n]
            h.file_off = pos
            pos += 4 * n
        elif t == 0x3EB:
            n = u32()
            h.kind = "BSS"
            h.bss_words = n
        elif t in (0x3EC, 0x3FC, 0x3F7):
            while True:
                if t == 0x3FC:
                    n = struct.unpack_from(">H", buf, pos)[0]
                    pos += 2
                else:
                    n = u32()
                if n == 0:
                    break
                if t == 0x3FC:
                    tgt = struct.unpack_from(">H", buf, pos)[0]
                    pos += 2
                    offs = list(struct.unpack_from(">%dH" % n, buf, pos))
                    pos += 2 * n
                else:
                    tgt = u32()
                    offs = list(struct.unpack_from(">%dI" % n, buf, pos))
                    pos += 4 * n
                h.relocs.append((tgt, offs))
            if t == 0x3FC:
                pos = (pos + 3) & ~3
        elif t == 0x3F0:
            while True:
                n = u32() & 0xFFFFFF
                if n == 0:
                    break
                nm = buf[pos:pos + 4 * n].rstrip(b"\0").decode("latin-1")
                pos += 4 * n
                h.symbols.append((nm, u32()))
        elif t == 0x3F1:
            n = u32()
            h.debug.append((pos, 4 * n))
            pos += 4 * n
        elif t == 0x3E8:
            n = u32()
            h.name = buf[pos:pos + 4 * n]
            pos += 4 * n
        elif t == 0x3F2:
            cur += 1
        else:
            events.append("unknown hunk id 0x%x at %d" % (t, pos - 4))
            break
    info["end_pos"] = pos
    info["trailing"] = len(buf) - pos
    info["events"] = events
    return info


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    buf = open(argv[1], "rb").read()
    info = parse(buf)
    print("file size %d, hunks %d..%d, parsed to %d, trailing %d bytes" % (
        len(buf), info["first"], info["last"], info["end_pos"], info["trailing"]))
    for e in info["events"]:
        print("EVENT:", e)
    dump = None
    if "--dump-dir" in argv:
        dump = argv[argv.index("--dump-dir") + 1]
        os.makedirs(dump, exist_ok=True)
    for h in info["hunks"]:
        nrel = sum(len(o) for _, o in h.relocs)
        print("hunk %d: %-4s alloc=%d bytes mem=%s file_data=%d bytes relocs=%d (targets %s) syms=%d debug=%d" % (
            h.idx, h.kind, h.size_words * 4, MEM[h.memf], len(h.data), nrel,
            sorted(set(t for t, _ in h.relocs)), len(h.symbols), len(h.debug)))
        for nm, off in h.symbols[:20]:
            print("   sym %-24s %08x" % (nm, off))
        if dump is not None and h.data:
            open(os.path.join(dump, "hunk%d.bin" % h.idx), "wb").write(h.data)
            with open(os.path.join(dump, "hunk%d.relocs.txt" % h.idx), "w") as f:
                for t, offs in h.relocs:
                    for o in offs:
                        f.write("%d %08x\n" % (t, o))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
