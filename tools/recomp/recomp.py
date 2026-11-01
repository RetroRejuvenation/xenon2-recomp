#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Xenon 2 static recompiler generator (Phase 5a).

Decodes the resident game image and the six disc-2 level modules from the code map, builds per-function control
flow graphs and writes C++ into generated/recomp/ (gitignored):

  x2r_handlers.h         Musashi opcode handlers used by the game, specialised (see recompiler-design.md)
  x2r_<module>_<n>.cpp   translated functions, one C++ function per code-map function
  x2r_tables.cpp         dispatch entries (pc, opcode, cycles, function) per module
  sources.cmake          the list of .cpp files for CMake (-DX2_RECOMP=ON)
  manifest.json          functions, instruction-set usage, untranslated opcodes, capstone cross-check

usage: python3 -I tools/recomp/recomp.py [--build disk|cdtv|st] [--assets generated/assets] [--out generated/recomp]
                                          [--lane generated/lane-d2 | --no-lane] [--trace DIR]
                                          [--profile FILE ...] [--no-static]

--build cdtv translates the CDTV release instead (docs/re-notes/cdtv-recomp.md): the pre-linked X2 in game_0400.bin
(--assets generated/assets-cdtv) with its seven overlays (cd0/LEVEL1-5, INTRO, PICCODE, relocated the way X2's own
relocator does) as modules keyed by overlay name; output goes to generated/recomp-cdtv with x2rc_ names and the module set
x2r_modules_cdtv, so one recompiled build can carry both games. Seeds come from its own traces (tools/trace/run_corpus.py
--game cdtv) and from the program's entry, interrupt handler, engine API table and the overlays' entry stubs.

--build st translates the Atari ST release (docs/re-notes/atari-st-recomp.md): the engine (side 0, tracks 34-79, at $400), the
front end (RNC-unpacked to $40000) and the five level modules (RNC-unpacked to $4F000), all read from the import
(--assets generated/assets-st: disk.bin and manifest.txt); modules game, frontend, level1..level5; output generated/recomp-st
with x2rs_ names and the module set x2r_modules_st. Seeds come from its traces (tools/trace/run_corpus.py --game st; the trace
names a level module mod_<key>, matched here to the image with the same key) and from the entry, interrupt and timer handlers.

Seeds come from the Ghidra code map in --lane (a dev-only analysis product) and/or from runtime coverage
traces (--trace, made by tools/trace/run_corpus.py from the committed replays) and profiles. Without a code
map (no --lane directory, or --no-lane) only --trace/--profile seeds are used, with the level-module table and interrupt
vectors built in below; that is the end-user path in tools/build_all.sh.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import m68kdecode  # noqa: E402

GAME_BASE = 0x400
MODULE_BASE = 0x54E00
CHIP_END = 0x80000  # end of the disk builds' level-module slot (the 512 KB chip RAM top); not the machine's chip size, which the
                    # generated code never bakes in (memory accesses go through Musashi's callbacks into Machine)
MODULES = ["game", "level1", "level2", "level3", "level4", "level5", "level6"]
# The CDTV release (docs/re-notes/cdtv-c0.md, cdtv-importer.md): X2 is linked at $400 by the importer (game_0400.bin); its
# overlays are read into one 180,000-byte area at hunk+$4A26 (= $4E26) and relocated by X2 itself (add hunk base + 6, i.e. $406,
# to every longword listed in the file's GEMDOS-style relocation table); the engine API table (56 longs) is at hunk $A12.
CDTV_BASE = 0x400
CDTV_OVERLAY_BASE = 0x4E26
CDTV_OVERLAY_SIZE = 180000
CDTV_RELOC_ADD = 0x406
CDTV_OVERLAYS = ["LEVEL1", "LEVEL2", "LEVEL3", "LEVEL4", "LEVEL5", "INTRO", "PICCODE"]
CDTV_MODULES = ["game"] + [n.lower() for n in CDTV_OVERLAYS]
CDTV_ENTRY = CDTV_BASE                      # the program starts at its first instruction
CDTV_LEVEL3_HANDLER = 0x34BB2 + CDTV_BASE   # the program's own level-3 (vertical blank) handler
CDTV_STAND_IN = 0x34CC4 + CDTV_BASE         # the 8-byte INTREQ-clearing routine the runtime points the vector at
CDTV_API_TABLE = 0xA12 + CDTV_BASE          # 56 slots
CDTV_API_SLOTS = 56
# The Atari ST release (docs/re-notes/atari-st-recomp.md, atari-st-disk-layout.md). The engine is read raw from side 0, cylinders
# 34-79, to $400; the front end (manifest rnc.0) and the five level modules (rnc.1-5) are RNC streams unpacked to $40000 and $4F000.
ST_ENGINE_BASE = 0x400
ST_ENGINE_FIRST_CYLINDER = 34
ST_FRONT_BASE = 0x40000
ST_LEVEL_BASE = 0x4F000
ST_MODULES = ["game", "frontend"] + ["level%d" % i for i in range(1, 6)]
ST_BUILD = "atari-st-kixx"
ST_ENTRY = 0x7CF8        # the engine's entry (and the default exception vector target)
ST_VBL = 0x2984          # level-4 vertical blank handler
ST_ACIA = 0x0F7C         # level-6 keyboard/MIDI ACIA handler
ST_TIMER_A = 0x40C94     # the front end's Timer A handler
ST_FRONT_ENTRY = ST_FRONT_BASE  # the front end starts at its first instruction (the depacker jumps there; a trace flags it only as a branch target)
ST_KEY_BYTES = 2048      # tracer module keys (runtime/trace/tracer.cpp st_phase): the front end from byte 0, a level module from byte 16
ST_LEVEL_KEY_SKIP = 16
# Handlers never translated: they only raise exceptions, and in this game they are data, not code.
EXCLUDED_HANDLERS = {"m68k_op_illegal", "m68k_op_1010", "m68k_op_1111"}
MAX_FUNC_INSNS = 6000
PART_INSNS = 2500  # instructions per generated .cpp file
# Without a code map: disc-2 level modules (start sector, count; the game's table at $91EA) and the game's
# interrupt vectors (docs/re-notes/disk-layout.md, code-map.md).
BUILTIN_EP = {
    "images": {"level_modules": {"sectors": [[11, 229], [250, 254], [514, 275], [799, 345], [1154, 312],
                                             [1476, 248]]}},
    "interrupt_vectors": {"0x64": 0x95E4, "0x68": 0x0FAC, "0x6C": 0x20C2, "0x6C_during_diskload": 0x2098,
                          "0x08-0x3FC_default": 0x7032},
}


# ------------------------------------------------------------------------------------------------------------
# Musashi: m68kmake + opcode dump + handler bodies

def build_musashi_tables(work):
    musashi = os.path.join(ROOT, "vendor", "musashi")
    os.makedirs(work, exist_ok=True)
    # A GCC-style host compiler (cc, gcc, clang; on Windows set CC=clang or gcc: MSVC's cl is not supported here).
    cc = os.environ.get("CC", "cc")
    exe = ".exe" if os.name == "nt" else ""
    mk = os.path.join(work, "m68kmake" + exe)
    subprocess.run([cc, "-O1", "-w", os.path.join(musashi, "m68kmake.c"), "-o", mk], check=True)
    subprocess.run([mk, work, os.path.join(musashi, "m68k_in.c")], check=True, stdout=subprocess.DEVNULL)
    dump = os.path.join(work, "opdump" + exe)
    subprocess.run([cc, "-O1", "-w", '-DMUSASHI_CNF="x2_m68kconf.h"', "-I" + musashi, "-I" + work,
                    "-I" + os.path.join(ROOT, "runtime"), os.path.join(HERE, "opdump.c"),
                    os.path.join(musashi, "m68kcpu.c"), os.path.join(musashi, "m68kdasm.c"),
                    os.path.join(musashi, "softfloat", "softfloat.c"), "-o", dump, "-lm"], check=True)  # -lm: Linux needs it
    out = subprocess.run([dump], check=True, capture_output=True, text=True).stdout
    open(os.path.join(work, "optable.txt"), "w").write(out)
    src = open(os.path.join(work, "m68kops.c")).read()
    names = re.findall(r"^\t\{(m68k_op_\w+)\s*,\s*0x[0-9a-f]+,\s*0x[0-9a-f]+,", src, re.M)
    table = [None] * 0x10000
    for line in out.splitlines():
        op, idx, cyc = (int(x) for x in line.split())
        table[op] = ("m68k_op_illegal" if idx < 0 else names[idx], cyc)
    bodies = dict(re.findall(r"^static void (m68k_op_\w+)\(void\)\n\{\n(.*?)\n\}\n", src, re.M | re.S))
    return table, bodies


# ------------------------------------------------------------------------------------------------------------
# Images

class Image:
    """A loaded image. `holes` are address ranges (lo, hi) inside it that hold something else at run time (the CDTV
    release's overlay area inside the resident image): never decoded, never seeded."""

    def __init__(self, name, base, data, holes=()):
        self.name, self.base, self.data, self.holes = name, base, data, tuple(holes)
        self.end = base + len(data)

    def in_hole(self, a):
        return any(lo <= a < hi for lo, hi in self.holes)

    def w16(self, a):
        o = a - self.base
        if o < 0 or o + 2 > len(self.data) or (self.holes and (self.in_hole(a) or self.in_hole(a + 1))):
            return None
        return (self.data[o] << 8) | self.data[o + 1]

    def contains(self, a):
        return self.base <= a < self.end and not (self.holes and self.in_hole(a))


def load_images(assets, ep):
    game = open(os.path.join(assets, "game_0400.bin"), "rb").read()
    disc2 = open(os.path.join(assets, "disc2.bin"), "rb").read()
    imgs = {"game": Image("game", GAME_BASE, game)}
    for i, (s, n) in enumerate(ep["images"]["level_modules"]["sectors"]):
        data = disc2[s * 512:(s + n) * 512][: CHIP_END - MODULE_BASE]
        imgs["level%d" % (i + 1)] = Image("level%d" % (i + 1), MODULE_BASE, data)
    return imgs


def be32(b, o):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def relocate_overlay(raw):
    """The bytes of a CDTV overlay file (a 28-byte GEMDOS 0x601A header, the text, then the relocation table) as they lie
    in memory after X2 has read it into the overlay area and run its relocator ($3CEEE): every listed longword of the text
    gets CDTV_RELOC_ADD added. Returns (image bytes, relocation count). The table: a long with the offset of the first
    fix-up (from the start of the text), then one byte per next fix-up (the distance; 1 = advance 254 without a fix-up;
    0 = end). docs/re-notes/cdtv-c0.md section 1 (counts 682/900/976/1,043/1,110, INTRO 365, PICCODE 14)."""
    if len(raw) < 0x1C or raw[0] != 0x60 or raw[1] != 0x1A:
        raise ValueError("not a GEMDOS 0x601A overlay")
    tlen = be32(raw, 2)
    text0 = 0x1C
    rt = text0 + tlen
    if tlen > len(raw) - 0x1C or rt + 4 > len(raw):
        raise ValueError("overlay text or relocation table outside the file")
    out = bytearray(raw)
    # X2's relocator takes the first long as the offset of the first fix-up even when it is 0 (the overlays' first fix-up is at
    # offset 0; this is not the Atari "0 = no relocations" convention) and ends at the first zero byte.
    pos = be32(raw, rt)
    p = rt + 4
    n = 0
    while True:
        if pos + 4 > tlen:
            raise ValueError("relocation outside the text")
        a = text0 + pos
        v = (be32(out, a) + CDTV_RELOC_ADD) & 0xFFFFFFFF
        out[a:a + 4] = v.to_bytes(4, "big")
        n += 1
        while True:
            if p >= len(raw):
                raise ValueError("relocation table is not terminated")
            b = raw[p]
            p += 1
            if b == 1:
                pos += 254
                continue
            break
        if b == 0:
            break
        pos += b
    return bytes(out), n


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_manifest(path):
    kv = {}
    for line in open(path):
        line = line.rstrip("\n")
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


def load_images_st(assets):
    """The ST translation's images from the import (manifest.txt, disk.bin): the engine, the front end and the level modules as
    they lie in RAM when unpacked. Also returns the tracer keys of the front end and of each level module."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "acq"))
    import rnc
    kv = read_manifest(os.path.join(assets, "manifest.txt"))
    if kv.get("build") != ST_BUILD:
        sys.exit("recomp: %s is not the %s import (build=%s): only the Kixx release is translated" % (assets, ST_BUILD, kv.get("build")))
    disk = open(os.path.join(assets, "disk.bin"), "rb").read()
    cyls, sides, spt, ssz = int(kv["cylinders"]), int(kv["sides"]), int(kv["sectors_per_track"]), int(kv["sector_bytes"])
    track = spt * ssz
    if len(disk) != cyls * sides * track:
        sys.exit("recomp: %s/disk.bin does not match the geometry in its manifest" % assets)

    def stream(side):
        return b"".join(disk[(c * sides + side) * track:(c * sides + side + 1) * track] for c in range(cyls))

    streams = [stream(0), stream(1)]
    imgs = {"game": Image("game", ST_ENGINE_BASE, streams[0][ST_ENGINE_FIRST_CYLINDER * track:])}
    keys = {}
    for n, mod, base in [(0, "frontend", ST_FRONT_BASE)] + [(i, "level%d" % i, ST_LEVEL_BASE) for i in range(1, 6)]:
        f = dict(x.split("=") for x in kv["rnc.%d" % n].split(","))
        ok, h, data = rnc.verify(streams[int(f["side"])], int(f["side_offset"]))
        if not ok or len(data) != int(f["unpacked"]):
            sys.exit("recomp: rnc.%d (%s) does not unpack in %s" % (n, mod, assets))
        imgs[mod] = Image(mod, base, data)
        skip = 0 if mod == "frontend" else ST_LEVEL_KEY_SKIP
        keys["%016x" % fnv1a64(data[skip:skip + ST_KEY_BYTES])] = mod
    return imgs, keys


def load_images_cdtv(assets):
    game = open(os.path.join(assets, "game_0400.bin"), "rb").read()
    hole = (CDTV_OVERLAY_BASE, CDTV_OVERLAY_BASE + CDTV_OVERLAY_SIZE)
    imgs = {"game": Image("game", CDTV_BASE, game, holes=[hole])}
    for name in CDTV_OVERLAYS:
        path = os.path.join(assets, "cd0", name)
        if not os.path.exists(path):
            continue
        data, _ = relocate_overlay(open(path, "rb").read())
        imgs[name.lower()] = Image(name.lower(), CDTV_OVERLAY_BASE, data[:CDTV_OVERLAY_SIZE])
    return imgs


# ------------------------------------------------------------------------------------------------------------
# Decoding and CFG

class Insn:
    __slots__ = ("pc", "op", "words", "handler", "cycles", "flow", "length")


def decode(img, pc, table, cache):
    if pc in cache:
        return cache[pc]
    ins = None
    op = img.w16(pc) if pc & 1 == 0 else None
    if op is not None:
        handler, cyc = table[op]
        ln = m68kdecode.length(op)
        if handler not in EXCLUDED_HANDLERS and cyc != 0 and ln is not None and pc + ln <= img.end:
            words = [img.w16(pc + 2 * i) for i in range(ln // 2)]
            if None in words:  # part of the instruction lies in a hole (or past the image)
                cache[pc] = None
                return None
            ins = Insn()
            ins.pc, ins.op, ins.words, ins.handler, ins.cycles, ins.length = pc, op, words, handler, cyc, ln
            ins.flow = m68kdecode.flow(op, pc, words)
    cache[pc] = ins
    return ins


def build_function(img, entry, table, cache):
    """All instructions reachable from `entry` without following calls. Returns (insns by pc, call targets,
    untranslated pcs reached)."""
    insns, calls, bad = {}, set(), set()
    work = [entry]
    while work and len(insns) < MAX_FUNC_INSNS:
        pc = work.pop()
        if pc in insns or pc in bad or not img.contains(pc):
            continue
        ins = decode(img, pc, table, cache)
        if ins is None:
            bad.add(pc)
            continue
        insns[pc] = ins
        f = ins.flow
        nxt = pc + ins.length
        if f.kind in ("seq", "branch", "call"):
            work.append(nxt)
        if f.kind in ("branch", "jump") and f.target is not None:
            work.append(f.target & 0xFFFFFF)
        if f.kind == "call" and f.target is not None:
            calls.add(f.target & 0xFFFFFF)
    return insns, calls, bad


# ------------------------------------------------------------------------------------------------------------
# Seeds

def cdtv_static_seeds(imgs, add):
    """Entry points known without a trace: the program's entry, its level-3 handler and the stand-in routine, every slot of the
    engine API table that points into the resident code, and the overlays' entry stubs (the `bra.w` words of the 60-byte
    header at the start of each overlay's text: main calls them at fixed addresses)."""
    game = imgs["game"]
    for pc, why in ((CDTV_ENTRY, "entry"), (CDTV_LEVEL3_HANDLER, "level-3 handler"), (CDTV_STAND_IN, "stand-in handler")):
        add("game", pc, "cdtv " + why)
    for i in range(CDTV_API_SLOTS):
        o = CDTV_API_TABLE + 4 * i - game.base
        if o < 0 or o + 4 > len(game.data):
            break
        t = be32(game.data, o) & 0xFFFFFF
        if game.contains(t):
            add("game", t, "cdtv api slot %d" % i)
        else:
            for mod in CDTV_MODULES[1:]:
                if mod in imgs and imgs[mod].contains(t):
                    add(mod, t, "cdtv api slot %d" % i)
    for mod in CDTV_MODULES[1:]:
        img = imgs.get(mod)
        if img is None:
            continue
        text = img.base + 0x1C
        for a in range(text, text + 60, 2):
            if img.w16(a) == 0x6000:  # BRA.W
                add(mod, a, "cdtv overlay stub")


def st_static_seeds(imgs, add):
    for pc, why in ((ST_ENTRY, "engine entry"), (ST_VBL, "vertical blank handler"), (ST_ACIA, "ACIA handler")):
        add("game", pc, "st " + why)
    add("frontend", ST_TIMER_A, "st Timer A handler")
    add("frontend", ST_FRONT_ENTRY, "st front end entry")


def trace_module(build, name, m, imgs, st_keys, warned):
    """The image a trace module belongs to (None = not translated)."""
    if build != "st":
        return name
    if name in ("game", "frontend"):
        return name
    if name.startswith("mod_"):
        for k in m.get("content_keys", []):
            if k in st_keys:
                return st_keys[k]
        if name not in warned:
            warned.add(name)
            print("recomp: warning: trace module %s matches no level module (%d executed instructions skipped)" % (
                name, m["executed_insns"]))
    return None  # the stage-1 loader and the boot sector run once, interpreted


def collect_seeds(imgs, ep, functions, sites, profiles, use_static, trace_dir=None, build="disk", st_keys=None):
    seeds = collections.defaultdict(set)  # module -> entry pcs
    origin = {}

    def add(mod, pc, why):
        pc &= 0xFFFFFF
        if mod in imgs and imgs[mod].contains(pc):
            if pc not in seeds[mod]:
                origin[(mod, pc)] = why
            seeds[mod].add(pc)

    def add_any(pc, why):
        pc &= 0xFFFFFF
        if pc < MODULE_BASE:
            add("game", pc, why)
        else:
            for m in MODULES[1:]:
                add(m, pc, why)

    if use_static and build == "st":
        st_static_seeds(imgs, add)
    elif use_static and build == "cdtv":
        cdtv_static_seeds(imgs, add)
    elif use_static:
        for f in functions:
            add(f["module"], int(f["start"], 16), "code-map")
        for k, v in ep["interrupt_vectors"].items():
            if isinstance(v, int):
                add("game", v, "vector " + k)
        for s in sites.get("sites", []):
            for t in s.get("targets", []):
                add(s["module"], int(t, 16), "indirect " + s["site"])
        for k, lst in sites.get("handler_sets", {}).items():
            for t in lst:
                add_any(int(t, 16), "handler-set " + k)
    if trace_dir:
        # runtime/trace coverage runs (tools/trace): every executed call, indirect or interrupt target,
        # keyed by the overlay module the trace attributed it to.
        cov = json.load(open(os.path.join(trace_dir, "coverage.json")))
        warned = set()
        for name, m in cov["modules"].items():
            mod = trace_module(build, name, m, imgs, st_keys, warned)
            if mod is None:
                continue
            for pc, ln, count, flags, *rest in m["insns"]:
                if flags & 13:  # call, indirect or interrupt target (branch targets stay inside functions)
                    add(mod, pc, "trace")
        ind = os.path.join(trace_dir, "indirect_targets.json")
        if os.path.exists(ind):
            for site in json.load(open(ind)).get("sites", []):
                for t in site.get("targets", []):
                    tm = trace_module(build, t["module"], {"content_keys": cov["modules"].get(t["module"], {}).get("content_keys", []),
                                                           "executed_insns": 0}, imgs, st_keys, warned)
                    if tm is not None:
                        add(tm, int(t["pc"], 16) if isinstance(t["pc"], str) else t["pc"], "trace-indirect")
    for path in profiles:
        for line in open(path):
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            pc = int(parts[0], 16)
            code = bytes.fromhex(parts[2]) if len(parts) > 2 else b""
            for m, img in imgs.items():
                if not img.contains(pc):
                    continue
                o = pc - img.base
                if code and img.data[o:o + len(code)] != code:
                    continue
                add(m, pc, "profile")
    return seeds, origin


# ------------------------------------------------------------------------------------------------------------
# C++ emission

PREFIX = "x2r"  # "x2rc" for the CDTV release (set by main): function, file and handler-header names
HANDLERS_H = "x2r_handlers.h"


def cname(mod, pc):
    return "%s_%s_%06x" % (PREFIX, mod, pc)


def emit_function(mod, entry, insns, out):
    fn = cname(mod, entry)
    pcs = sorted(insns)
    out.append("int %s(uint32_t pc24) {" % fn)
    out.append("    int k = 0;")
    out.append("    const uint32_t ph = REG_PC & 0xFF000000u;")
    out.append("    switch (pc24) {")
    for pc in pcs:
        out.append("    case 0x%x: goto E_%x;" % (pc, pc))
    out.append("    default: return 0;")
    out.append("    }")
    out.append("R:")
    out.append("    if ((REG_PC & 0xFF000000u) != ph) return k;")
    out.append("    switch (REG_PC & 0xFFFFFFu) {")
    for pc in pcs:
        out.append("    case 0x%x: goto L_%x;" % (pc, pc))
    out.append("    default: return k;")
    out.append("    }")
    for pc in pcs:
        ins = insns[pc]
        ext = ins.words[1:]
        verify = " && ".join("X2R_CODE16(0x%x) == 0x%04x" % (pc + 2 * i, w) for i, w in enumerate(ins.words))
        h = "h_" + ins.handler[len("m68k_op_"):]
        out.append("L_%x: X2R_BOUNDARY(0x%x);" % (pc, pc))
        if ext:
            out.append("E_%x: { static const uint16_t x[] = {%s};" % (pc, ", ".join("0x%04x" % w for w in ext)))
            arr, n = "x", len(ext)
        else:
            out.append("E_%x: {" % pc)
            arr, n = "nullptr", 0
        out.append("    if (!(%s)) X2R_UNVERIFIED();" % verify)
        out.append("    X2R_BEGIN(0x%x, 0x%04x); %s(0x%04x, %s, %d); X2R_END(%d);" % (pc, ins.op, h, ins.op, arr, n,
                                                                                    ins.cycles))
        # Statically expected successors; anything else goes through the switch at R (or back to the executor).
        succ = []
        f = ins.flow
        nxt = pc + ins.length
        if f.target is not None and f.kind in ("branch", "jump", "call") and (f.target & 0xFFFFFF) in insns:
            t = f.target & 0xFFFFFF
            if (ins.op & 0xFF80) == 0x4E80 and (ins.op & 0x3E) == 0x38:
                succ.append(("0x%xu" % f.target, t))  # JMP/JSR abs.w/abs.l: the PC takes the operand as is
            else:
                succ.append(("ph | 0x%xu" % t, t))
        if f.kind in ("seq", "branch") and nxt in insns:
            succ.append(("ph | 0x%xu" % nxt, nxt))
        for expr, t in succ:
            out.append("    if (REG_PC == (%s)) goto L_%x;" % (expr, t))
        out.append("    goto R; }")
    out.append("}")
    out.append("")
    return fn


def prelude():
    return ("// Generated by tools/recomp/recomp.py - do not edit. See docs/re-notes/recompiler-design.md.\n"
            '#include "recomp/x2r_runtime.h"\n#include "%s"\n' % HANDLERS_H)


def emit_handlers(used, bodies, path):
    lines = ["// Generated by tools/recomp/recomp.py from Musashi's m68kops.c - do not edit.",
             "// Each handler is Musashi's body verbatim; REG_IR and the instruction-stream reads are specialised",
             "// by the macros in runtime/recomp/x2r_runtime.h.",
             "#pragma once", ""]
    for name in sorted(used):
        body = bodies[name]
        lines.append("static X2R_INLINE void h_%s(const uint ir_, const uint16_t* const e_, const int n_) {" %
                     name[len("m68k_op_"):])
        lines.append("\tint ei_ = 0; (void)ei_; (void)e_; (void)n_; (void)ir_;")
        lines.append(body)
        lines.append("}")
        lines.append("")
    open(path, "w").write("\n".join(lines))


# ------------------------------------------------------------------------------------------------------------

def capstone_check(imgs, funcs):
    try:
        import dis68k
        d = dis68k.Dis()
    except Exception as e:  # pragma: no cover - capstone missing
        return {"available": False, "error": str(e)}
    mism, n = [], 0
    for (mod, entry), insns in funcs.items():
        img = imgs[mod]
        for pc, ins in insns.items():
            o = pc - img.base
            size, mn, ops = d.one(img.data[o:o + 12], pc)
            n += 1
            if mn != "dc.w" and size != ins.length:
                mism.append({"module": mod, "pc": "0x%x" % pc, "decoder": ins.length, "capstone": size,
                             "text": "%s %s" % (mn, ops), "handler": ins.handler})
    return {"available": True, "checked": n, "mismatches": mism[:200], "mismatch_count": len(mism)}


def mnemonic_of(handler):
    n = handler[len("m68k_op_"):]
    return n.split("_")[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", choices=["disk", "cdtv", "st"], default="disk",
                    help="which game to translate: the disk builds (default), the CDTV release or the Atari ST release")
    ap.add_argument("--assets", default=None, help="imported assets (default generated/assets, or generated/assets-cdtv)")
    ap.add_argument("--lane", default=os.path.join(ROOT, "generated", "lane-d2"))
    ap.add_argument("--no-lane", action="store_true",
                    help="ignore the code map even if present: seed from --trace/--profile only (tools/build_all.sh)")
    ap.add_argument("--out", default=None, help="output directory (default generated/recomp, or generated/recomp-cdtv)")
    ap.add_argument("--profile", action="append", default=[],
                    help="extra seeds: 'PC COUNT [HEXBYTES]' lines written by x2recomp --recomp-profile")
    ap.add_argument("--trace", metavar="DIR",
                    help="extra seeds from runtime/trace coverage output (coverage.json, indirect_targets.json)")
    ap.add_argument("--no-static", action="store_true", help="use only --profile seeds (for coverage studies)")
    ap.add_argument("--no-capstone", action="store_true")
    args = ap.parse_args()

    global PREFIX, HANDLERS_H
    cdtv = args.build == "cdtv"
    st = args.build == "st"
    nodisk = cdtv or st  # no code map, no disc-2 level table: static seeds and traces only
    if cdtv:
        PREFIX, HANDLERS_H = "x2rc", "x2rc_handlers.h"
    elif st:
        PREFIX, HANDLERS_H = "x2rs", "x2rs_handlers.h"
    modules = CDTV_MODULES if cdtv else ST_MODULES if st else MODULES
    symbol = "x2r_modules_cdtv" if cdtv else "x2r_modules_st" if st else "x2r_modules"
    cmake_var, cmake_dir = (("X2_RECOMP_CDTV_SOURCES", "X2_RECOMP_CDTV_DIR") if cdtv else
                            ("X2_RECOMP_ST_SOURCES", "X2_RECOMP_ST_DIR") if st else ("X2_RECOMP_SOURCES", "X2_RECOMP_DIR"))
    suffix = "-cdtv" if cdtv else "-st" if st else ""
    if args.assets is None:
        args.assets = os.path.join(ROOT, "generated", "assets" + suffix)
    if args.out is None:
        args.out = os.path.join(ROOT, "generated", "recomp" + suffix)
    st_keys = {}

    if nodisk:
        # No code map exists for the CDTV release: static seeds (entry points, API table, overlay stubs) and traces only.
        ep, functions, sites = {"images": {}, "interrupt_vectors": {}}, [], {}
        if not (args.trace or args.profile) and args.no_static:
            sys.exit("recomp: --build %s --no-static needs --trace DIR or --profile FILE" % args.build)
        if not (args.trace or args.profile):
            print("recomp: --build %s without --trace/--profile: seeding from the static entry points only "
                  "(tools/trace/run_corpus.py --game %s records the traces)" % (args.build, args.build))
        if st:
            imgs, st_keys = load_images_st(args.assets)
        else:
            imgs = load_images_cdtv(args.assets)
    else:
        if not args.no_lane and os.path.exists(os.path.join(args.lane, "entrypoints_v2.json")):
            ep = json.load(open(os.path.join(args.lane, "entrypoints_v2.json")))
            functions = json.load(open(os.path.join(args.lane, "functions.json")))
            sites = json.load(open(os.path.join(args.lane, "indirect_sites.json")))
        else:
            if not (args.trace or args.profile):
                sys.exit("recomp: no code map in %s: give --trace DIR (tools/trace/run_corpus.py) or --profile FILE"
                         % args.lane)
            print("recomp: %s, seeding from traces/profiles and the built-in vectors only"
                  % ("--no-lane" if args.no_lane else "no code map in " + args.lane))
            ep, functions, sites = BUILTIN_EP, [], {}
        imgs = load_images(args.assets, ep)
    modules = [m for m in modules if m in imgs]
    table, bodies = build_musashi_tables(os.path.join(args.out, "musashi"))

    seeds, origin = collect_seeds(imgs, ep, functions, sites, args.profile, not args.no_static, args.trace, args.build, st_keys)
    funcs = {}
    bad_all = collections.defaultdict(set)
    for mod in modules:
        if mod not in imgs:
            continue
        img = imgs[mod]
        cache = {}
        work = sorted(seeds.get(mod, ()))
        done = set()
        while work:
            entry = work.pop()
            if entry in done:
                continue
            done.add(entry)
            insns, calls, bad = build_function(img, entry, table, cache)
            bad_all[mod] |= bad
            if insns:
                funcs[(mod, entry)] = insns
            for c in calls:
                if img.contains(c) and c not in done:
                    origin.setdefault((mod, c), "call")
                    work.append(c)

    # Dispatch: each pc maps to one function per module (prefer the function that starts there, else the smallest).
    owner = {}
    for (mod, entry), insns in funcs.items():
        for pc in insns:
            key = (mod, pc)
            cur = owner.get(key)
            if cur is None or (entry == pc and cur[1] != pc) or (
                    cur[1] != pc and entry != pc and len(insns) < len(funcs[cur])):
                owner[key] = (mod, entry)
    used = set()
    for insns in funcs.values():
        for ins in insns.values():
            used.add(ins.handler)
    missing = [h for h in used if h not in bodies]
    if missing:
        sys.exit("handler bodies not found: %s" % missing)

    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith(PREFIX + "_") and f.endswith(".cpp"):
            os.remove(os.path.join(args.out, f))
    emit_handlers(used, bodies, os.path.join(args.out, HANDLERS_H))

    sources, decls, tables = [], [], {}
    for mod in modules:
        mf = sorted((e, i) for (m, e), i in funcs.items() if m == mod)
        part, out, count = 0, [], 0

        def flush():
            nonlocal part, out, count
            if not out:
                return
            name = "%s_%s_%d.cpp" % (PREFIX, mod, part)
            open(os.path.join(args.out, name), "w").write(prelude() + "\n" + "\n".join(out))
            sources.append(name)
            part, out, count = part + 1, [], 0

        for entry, insns in mf:
            fn = emit_function(mod, entry, insns, out)
            decls.append(fn)
            count += len(insns)
            if count >= PART_INSNS:
                flush()
        flush()
        rows = []
        for (m, pc), (_, entry) in sorted(owner.items()):
            if m != mod:
                continue
            ins = funcs[(mod, entry)][pc]
            rows.append("    {0x%x, 0x%04x, %d, %d, %s}," % (pc, ins.op, ins.cycles, ins.length, cname(mod, entry)))
        tables[mod] = rows

    t = [prelude().split("\n")[0], '#include "recomp/x2r_runtime.h"', ""]
    func_counts = {}
    for d in decls:
        t.append("int %s(uint32_t);" % d)
    t.append("")
    for mod in modules:
        t.append("static const x2r_entry entries_%s[] = {" % mod)
        t.extend(tables[mod])
        t.append("    {0, 0, 0, 0, nullptr}};")
        t.append("static const x2r_func funcs_%s[] = {" % mod)
        fl = sorted(e for (m, e) in funcs if m == mod)
        t.extend("    {0x%x, %s}," % (e, cname(mod, e)) for e in fl)
        t.append("    {0, nullptr}};")
        func_counts[mod] = len(fl)
    t.append("")
    t.append("extern const x2r_module %s[] = {" % symbol)
    for mod in modules:
        if mod not in imgs:
            continue
        img = imgs[mod]
        sec = (0, 0) if (mod == "game" or nodisk) else tuple(ep["images"]["level_modules"]["sectors"][MODULES.index(mod) - 1])
        t.append('    {"%s", 0x%x, 0x%x, %d, %d, entries_%s, %d, funcs_%s, %d},' % (
            mod, img.base, img.end, sec[0], sec[1], mod, len(tables[mod]), mod, func_counts[mod]))
    t.append("    {nullptr, 0, 0, 0, 0, nullptr, 0, nullptr, 0}};")
    open(os.path.join(args.out, PREFIX + "_tables.cpp"), "w").write("\n".join(t) + "\n")
    sources.append(PREFIX + "_tables.cpp")
    with open(os.path.join(args.out, "sources.cmake"), "w") as f:
        f.write("# Generated by tools/recomp/recomp.py\nset(%s\n" % cmake_var)
        for s in sources:
            f.write("  ${%s}/%s\n" % (cmake_dir, s))
        f.write(")\n")

    # Manifest / report data.
    usage = collections.Counter()
    hcount = collections.Counter()
    for (mod, pc), (_, entry) in owner.items():
        h = funcs[(mod, entry)][pc].handler
        hcount[h] += 1
        usage[mnemonic_of(h)] += 1
    untrans = collections.Counter()
    for mod, pcs in bad_all.items():
        for pc in pcs:
            op = imgs[mod].w16(pc) if pc % 2 == 0 else None
            reason = "odd address" if op is None else (
                "%s (%s)" % (table[op][0], "no 68000 cycles" if table[op][1] == 0 else "excluded")
                if table[op][0] in EXCLUDED_HANDLERS or table[op][1] == 0 else "decoder: no length")
            untrans[reason] += 1
    manifest = {
        "functions": [{"module": m, "entry": "0x%x" % e, "insns": len(i), "origin": origin.get((m, e), "?")}
                      for (m, e), i in sorted(funcs.items())],
        "instructions": {m: sum(1 for (mm, _) in owner if mm == m) for m in modules},
        "mnemonics": dict(sorted(usage.items())),
        "handlers": dict(sorted(hcount.items())),
        "untranslated_reached": {k: v for k, v in untrans.most_common()},
        "untranslated_sites": {m: ["0x%x" % p for p in sorted(v)] for m, v in bad_all.items()},
        "capstone": {} if args.no_capstone else capstone_check(imgs, funcs),
    }
    json.dump(manifest, open(os.path.join(args.out, "manifest.json"), "w"), indent=1)
    total = sum(manifest["instructions"].values())
    print("recomp: %d functions, %d translated instructions (%s), %d handlers, %d files" % (
        len(funcs), total, ", ".join("%s %d" % kv for kv in manifest["instructions"].items()), len(used),
        len(sources)))
    if not args.no_capstone:
        c = manifest["capstone"]
        print("recomp: capstone length cross-check: %s" % (
            "%d checked, %d mismatches" % (c["checked"], c["mismatch_count"]) if c.get("available") else c))
    print("recomp: untranslated decode stops: %s" % dict(untrans.most_common(8)))


if __name__ == "__main__":
    main()
