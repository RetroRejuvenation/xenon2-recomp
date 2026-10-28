#!/usr/bin/env python3
"""ADF (Amiga disk image) reader: OFS/FFS parser, lister, extractor.

usage: python3 -I tools/adf.py IMAGE.adf <command> [args]
  info                  boot block, root block, bitmap summary
  ls                    recursive listing with sizes, protection, checksum status
  extract OUTDIR        extract every file to OUTDIR (paths preserved)
  cat PATH              write one file to stdout
  boot OUTFILE          dump the 1024-byte boot block
  blocks                map of used blocks per type (header/data/ext/dir)
"""
import struct
import sys
import os

BSIZE = 512
NBLK = 1760
ROOT = 880


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def sbe32(b, o):
    return struct.unpack_from(">i", b, o)[0]


def block_sum(b):
    """Normal block checksum: sum of all longs (with checksum field) == 0."""
    s = 0
    for i in range(0, BSIZE, 4):
        s = (s + be32(b, i)) & 0xFFFFFFFF
    return s


def bootblock_checksum(b):
    s = 0
    for i in range(0, 1024, 4):
        v = be32(b, i)
        s += v
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s  # valid when 0xFFFFFFFF


def bstr(b, o):
    n = b[o]
    return b[o + 1:o + 1 + n].decode("latin-1")


def amiga_date(b, o):
    d, m, t = struct.unpack_from(">III", b, o)
    return d, m, t


def prot_str(p):
    # bits are active-low for rwed
    s = ""
    s += "h" if p & 0x80 else "-"
    s += "s" if p & 0x40 else "-"
    s += "p" if p & 0x20 else "-"
    s += "a" if p & 0x10 else "-"
    s += "-" if p & 0x08 else "r"
    s += "-" if p & 0x04 else "w"
    s += "-" if p & 0x02 else "e"
    s += "-" if p & 0x01 else "d"
    return s


class Adf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        if len(self.d) not in (901120, 1802240):
            raise ValueError("unexpected size %d" % len(self.d))
        self.nblk = len(self.d) // BSIZE
        self.root = self.nblk // 2
        self.dostype = self.d[0:4]
        self.flags = self.d[3] if self.d[:3] == b"DOS" else 0
        self.ffs = bool(self.flags & 1)
        self.intl = bool(self.flags & 2)
        self.dircache = bool(self.flags & 4)
        self.problems = []

    def blk(self, n):
        if not 0 <= n < self.nblk:
            raise IndexError("block %d out of range" % n)
        return self.d[n * BSIZE:(n + 1) * BSIZE]

    # -- hashing ---------------------------------------------------------
    @staticmethod
    def hash_name(name, htsize=72, intl=False):
        h = len(name)
        for ch in name.encode("latin-1"):
            c = ch
            if intl:
                if 0x61 <= c <= 0x7A or 0xE0 <= c <= 0xFE and c != 0xF7:
                    c -= 0x20
            elif 0x61 <= c <= 0x7A:
                c -= 0x20
            h = (h * 13 + c) & 0x7FF
        return h % htsize

    # -- root ------------------------------------------------------------
    def root_info(self):
        b = self.blk(self.root)
        info = dict(
            type=be32(b, 0), ht_size=be32(b, 12), checksum_ok=block_sum(b) == 0,
            bm_flag=sbe32(b, BSIZE - 200), bm_pages=[be32(b, BSIZE - 196 + 4 * i) for i in range(25)],
            name=bstr(b, BSIZE - 80), sec_type=sbe32(b, BSIZE - 4),
            days=amiga_date(b, BSIZE - 92), disk_alt=amiga_date(b, BSIZE - 40),
            create=amiga_date(b, BSIZE - 28),
        )
        return info

    def bitmap(self):
        """Return set of free blocks per bitmap pages (blocks 2.. only)."""
        ri = self.root_info()
        free = set()
        for pg in ri["bm_pages"]:
            if not pg:
                continue
            b = self.blk(pg)
            if block_sum(b) != 0:
                self.problems.append("bitmap block %d checksum bad" % pg)
            blkno = 2
            for i in range(1, 128):
                w = be32(b, 4 * i)
                for bit in range(32):
                    if blkno < self.nblk and (w >> bit) & 1:
                        free.add(blkno)
                    blkno += 1
        return free

    # -- directory walking ----------------------------------------------
    def read_dir(self, dirblk, path=""):
        """Yield entries: dict(name, path, kind('file'|'dir'|...), hdr, ...)."""
        b = self.blk(dirblk)
        htsize = 72
        out = []
        for i in range(htsize):
            h = be32(b, 24 + 4 * i)
            seen = set()
            while h:
                if h in seen or h >= self.nblk:
                    self.problems.append("bad hash chain at %d" % dirblk)
                    break
                seen.add(h)
                e = self.read_header(h, path)
                if e is None:
                    break
                out.append(e)
                h = e["hash_next"]
        return out

    def read_header(self, h, path):
        b = self.blk(h)
        typ = be32(b, 0)
        sec = sbe32(b, BSIZE - 4)
        if typ != 2:
            self.problems.append("block %d type %d not T_HEADER" % (h, typ))
            return None
        name = bstr(b, BSIZE - 80)
        e = dict(
            hdr=h, name=name, path=(path + "/" + name) if path else name,
            hash_next=be32(b, BSIZE - 16), parent=be32(b, BSIZE - 12),
            prot=be32(b, BSIZE - 192), size=be32(b, BSIZE - 188),
            comment=bstr(b, BSIZE - 184), date=amiga_date(b, BSIZE - 92),
            sec=sec, csum_ok=block_sum(b) == 0,
        )
        if sec == -3:
            e["kind"] = "file"
            e["first_data"] = be32(b, 16)
            e["high_seq"] = be32(b, 8)
            e["data_blocks"], e["ext_blocks"] = self.file_blocks(b, e)
        elif sec == 2:
            e["kind"] = "dir"
        else:
            e["kind"] = "other(%d)" % sec
        return e

    def file_blocks(self, hb, e):
        blocks = []
        exts = []
        cur = hb
        n = be32(hb, 8)
        # data block table is stored reversed from offset 24 .. 24+4*72
        while True:
            for i in range(min(n, 72)):
                blocks.append(be32(cur, BSIZE - 204 - 4 * i))
            ext = be32(cur, BSIZE - 8)
            if not ext:
                break
            if ext in exts or ext >= self.nblk:
                self.problems.append("bad extension chain in %s" % e["name"])
                break
            exts.append(ext)
            cur = self.blk(ext)
            if block_sum(cur) != 0:
                self.problems.append("ext block %d checksum bad" % ext)
            n = be32(cur, 8)
        return blocks, exts

    def walk(self, dirblk=None, path=""):
        if dirblk is None:
            dirblk = self.root
        for e in self.read_dir(dirblk, path):
            yield e
            if e["kind"] == "dir":
                yield from self.walk(e["hdr"], e["path"])

    def read_file(self, e):
        data = bytearray()
        for bn in e["data_blocks"]:
            b = self.blk(bn)
            if self.ffs:
                data += b
            else:
                if be32(b, 0) != 8:
                    self.problems.append("%s: data block %d bad type" % (e["path"], bn))
                sz = be32(b, 12)
                data += b[24:24 + sz]
        return bytes(data[:e["size"]])

    def find(self, path):
        for e in self.walk():
            if e["path"].lower() == path.lower().strip("/"):
                return e
        return None


def cmd_info(a):
    bb = a.d[:1024]
    print("dostype      :", bb[:4], "flags=%d (%s%s%s)" % (
        a.flags, "FFS" if a.ffs else "OFS", "+INTL" if a.intl else "", "+DC" if a.dircache else ""))
    print("boot checksum:", "OK" if bootblock_checksum(bb) == 0xFFFFFFFF else "BAD (stored=%08x)" % be32(bb, 4),
          "rootblock ptr=%d" % be32(bb, 8))
    ri = a.root_info()
    print("root         : block %d type=%d sec=%d ht_size=%d csum_ok=%s name=%r bm_flag=%d bm_pages=%s" % (
        a.root, ri["type"], ri["sec_type"], ri["ht_size"], ri["checksum_ok"], ri["name"], ri["bm_flag"],
        [p for p in ri["bm_pages"] if p]))
    free = a.bitmap()
    print("free blocks  :", len(free), "of", a.nblk - 2)


def cmd_ls(a):
    n = 0
    tot = 0
    for e in a.walk():
        if e["kind"] == "dir":
            print("%-48s <dir>  hdr=%d prot=%s%s" % (e["path"] + "/", e["hdr"], prot_str(e["prot"]),
                                                      "" if e["csum_ok"] else " CSUM-BAD"))
        else:
            n += 1
            tot += e["size"]
            print("%-48s %8d hdr=%-4d prot=%s blocks=%d ext=%d%s" % (
                e["path"], e["size"], e["hdr"], prot_str(e["prot"]), len(e["data_blocks"]), len(e["ext_blocks"]),
                "" if e["csum_ok"] else " CSUM-BAD"))
    print("%d files, %d bytes" % (n, tot))
    for p in a.problems:
        print("PROBLEM:", p)


def cmd_extract(a, outdir):
    for e in a.walk():
        dest = os.path.join(outdir, e["path"])
        if e["kind"] == "dir":
            os.makedirs(dest, exist_ok=True)
        elif e["kind"] == "file":
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(a.read_file(e))
            print("extracted", e["path"], e["size"])
    for p in a.problems:
        print("PROBLEM:", p)


def cmd_blocks(a):
    owner = {0: "boot", 1: "boot", a.root: "root"}
    ri = a.root_info()
    for p in ri["bm_pages"]:
        if p:
            owner[p] = "bitmap"
    for e in a.walk():
        owner[e["hdr"]] = e["kind"] + "-hdr"
        if e["kind"] == "file":
            for b in e["data_blocks"]:
                owner[b] = "data:" + e["path"]
            for b in e["ext_blocks"]:
                owner[b] = "ext:" + e["path"]
    free = a.bitmap()
    unowned_used = [b for b in range(a.nblk) if b not in owner and b not in free]
    print("owned blocks:", len(owner), "free:", len(free), "used-but-unowned:", len(unowned_used))
    if unowned_used:
        # compress to ranges
        rs = []
        s = p = unowned_used[0]
        for b in unowned_used[1:]:
            if b != p + 1:
                rs.append((s, p))
                s = b
            p = b
        rs.append((s, p))
        print("unowned used ranges (block, track=blk//22, cyl=blk//22//2):")
        for s, p in rs:
            print("  %d-%d  (trk %d.%d - %d.%d, %d blocks)" % (s, p, s // 22, s % 22, p // 22, p % 22, p - s + 1))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    a = Adf(argv[1])
    c = argv[2]
    if c == "info":
        cmd_info(a)
    elif c == "ls":
        cmd_ls(a)
    elif c == "extract":
        cmd_extract(a, argv[3])
    elif c == "cat":
        e = a.find(argv[3])
        sys.stdout.buffer.write(a.read_file(e))
    elif c == "boot":
        open(argv[3], "wb").write(a.d[:1024])
    elif c == "blocks":
        cmd_blocks(a)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
