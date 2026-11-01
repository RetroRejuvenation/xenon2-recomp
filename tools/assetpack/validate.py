#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Validate an asset pack without the game or the runtime: `x2recomp --check-asset-pack` in pure Python.

    validate.py PACK [--game disk|cdtv|st] [--build TAG] [--against DUMPDIR] [--full] [--strict] [--json]

PACK is a pack folder or a .zip file. The rules are those of docs/re-notes/asset-packs-v2-design.md section 6 and of the runtime's
loader (runtime/pack/): the manifest against docs/schema/x2-asset-pack-v2.schema.json, unknown fields (warnings), a missing license
(a warning, an error with --strict), every file a manifest names (it must exist, a picture must decode, a WAV must read), picture
sizes against keys and scales, atlas rectangles, the ZIP member rules, duplicate keys, group keys, unreferenced files, and with
--against DUMPDIR the coverage of a dump, keys the dump never saw, and files byte-identical to the dump's (a dump holds the game's own
material and must not be redistributed).

Differences from the runtime's validator: no game or build scoping is applied (--game and --build only report how many entries apply),
FLAC and Ogg Vorbis files are checked by their signature only (WAV is read completely with --full: clipping and loop points), and the
memory estimate counts pictures only.

Output, one line per finding, then a summary (or --json, an array of the same findings and a final summary object):

    error: asset pack: <name>: <section>["<key>"]: <problem>

Exit codes: 0 valid (warnings allowed unless --strict), 1 warnings with --strict, 2 the pack is unusable or has invalid entries, 3 the
path cannot be read. Standard library only.
"""
import argparse
import hashlib
import io
import json
import math
import os
import re
import sys
import wave
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import minischema as MS  # noqa: E402
import pack_lib as P  # noqa: E402

SCHEMA_PATH = os.path.join(HERE, "..", "..", "docs", "schema", "x2-asset-pack-v2.schema.json")

MAX_ZIP_MEMBERS = 65536
MAX_ENTRIES = 65536
MAX_MEMBER = 512 << 20
RATIO = 1000
RATIO_FREE = 1 << 20
MAX_TOTAL_PICTURES = 512 << 20
RESERVED = {"CON", "PRN", "AUX", "NUL"} | {"COM%d" % i for i in range(1, 10)} | {"LPT%d" % i for i in range(1, 10)}

ERROR, WARNING, INFO = "error", "warning", "info"


class Report:
    def __init__(self, label):
        self.label = label
        self.messages = []

    def add(self, severity, section, key, text):
        self.messages.append({"severity": severity, "pack": self.label, "section": section, "key": key, "text": text})

    def count(self, severity):
        return sum(1 for m in self.messages if m["severity"] == severity)


def size_text(n):
    return "%d MB" % (n >> 20) if n >= 1 << 20 else "%d KB" % (n >> 10)


def format_message(m):
    r = "asset pack: "
    if m["pack"]:
        r += m["pack"] + ": "
    if m["section"]:
        r += m["section"]
        if m["key"]:
            r += '["%s"]' % m["key"]
        r += ": "
    return r + m["text"]


# ----------------------------------------------------------------------------------------------------------------
# Paths and sources

def path_problem(p):
    """Why a pack-internal path is unsafe ('' when fine): the runtime's rules (pack_paths.cpp)."""
    if not p:
        return "empty path"
    if len(p.encode("utf-8")) > 240:
        return "longer than 240 bytes"
    if p[0] == "/":
        return "absolute path"
    for c in p:
        if ord(c) < 0x20 or ord(c) == 0x7F:
            return "control character"
        if c == "\\":
            return "backslash"
        if c == ":":
            return "':' (drive letter)"
    if p[-1] == "/":
        return "ends with '/'"
    for comp in p.split("/"):
        if comp == "":
            return "empty component"
        if comp in (".", ".."):
            return "'%s' component" % comp
        if comp[-1] in " .":
            return "component ends with a space or a dot"
        if comp.split(".")[0].rstrip(" ").upper() in RESERVED:
            return "reserved Windows device name"
    return ""


class SourceError(Exception):
    def __init__(self, message, unreadable=False):
        Exception.__init__(self, message)
        self.unreadable = unreadable


class DirSource:
    is_zip = False

    def __init__(self, path):
        self.path = path
        self.root = os.path.realpath(path)

    def names(self):
        out = []
        for base, dirs, files in os.walk(self.root):
            dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(base, d))]
            for f in files:
                out.append(os.path.relpath(os.path.join(base, f), self.root).replace(os.sep, "/"))
        return sorted(out)

    def _resolve(self, rel):
        if path_problem(rel):
            raise SourceError("unsafe path '%s' (relative paths inside the pack only)" % rel)
        full = os.path.realpath(os.path.join(self.root, *rel.split("/")))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise SourceError("'%s' leaves the pack directory" % rel)
        if not os.path.isfile(full):
            raise SourceError("missing file '%s'" % rel)
        return full

    def exists(self, rel):
        try:
            self._resolve(rel)
            return True
        except SourceError:
            return False

    def size(self, rel):
        return os.path.getsize(self._resolve(rel))

    def read(self, rel, limit):
        full = self._resolve(rel)
        size = os.path.getsize(full)
        if size > limit:
            raise SourceError("'%s': too large (%d bytes, limit %d)" % (rel, size, limit))
        with open(full, "rb") as f:
            return f.read()

    def stored(self, rel):
        return True


class ZipSource:
    is_zip = True

    def __init__(self, path):
        self.path = path
        self.notes = []
        try:
            self.zf = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError, EOFError) as e:
            raise SourceError("not a usable ZIP file: %s" % e, unreadable=True)
        infos = self.zf.infolist()
        if len(infos) > MAX_ZIP_MEMBERS:
            raise SourceError("not a usable ZIP file: zip archive with more than %d members" % MAX_ZIP_MEMBERS, unreadable=True)
        members = []
        backslashes = False
        for i in infos:
            name = i.filename
            if "\\" in name:
                backslashes = True
                name = name.replace("\\", "/")
            if not name or name.endswith("/") or name.startswith("__MACOSX/"):
                continue
            members.append((name, i))
        if backslashes:
            self.notes.append("ZIP member names with backslashes were read as '/' (an archive made by an old Windows tool)")
        root_manifest = any(n == "manifest.json" for n, _ in members)
        nested = [n.split("/", 1)[0] + "/" for n, _ in members if "/" in n and n.split("/", 1)[1] == "manifest.json"]
        prefix = ""
        if not root_manifest:
            if len(nested) != 1:
                raise SourceError("no manifest.json in the ZIP file")
            prefix = nested[0]
        self.members = {}
        folded = {}
        for name, info in members:
            if not name.startswith(prefix):
                continue
            rel = name[len(prefix):]
            bad = path_problem(rel)
            if bad:
                self.notes.append("ZIP member '%s' is ignored: %s" % (name, bad))
                continue
            key = rel.lower()
            if key in folded:
                raise SourceError("two members collide on a case-insensitive file system ('%s' and '%s')" % (folded[key], rel))
            folded[key] = rel
            self.members[rel] = info

    def names(self):
        return sorted(self.members)

    def exists(self, rel):
        return not path_problem(rel) and rel in self.members

    def size(self, rel):
        if rel not in self.members:
            raise SourceError("missing file '%s'" % rel)
        return self.members[rel].file_size

    def stored(self, rel):
        return rel in self.members and self.members[rel].compress_type == zipfile.ZIP_STORED

    def read(self, rel, limit):
        if path_problem(rel):
            raise SourceError("unsafe path '%s' (relative paths inside the pack only)" % rel)
        info = self.members.get(rel)
        if info is None:
            raise SourceError("missing file '%s'" % rel)
        if info.flag_bits & 1:
            raise SourceError("'%s': encrypted zip entries are not supported" % rel)
        if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise SourceError("'%s': zip compression method %d is not supported (only stored/deflate)" % (rel, info.compress_type))
        if info.file_size > MAX_MEMBER:
            raise SourceError("'%s': too large (%d bytes)" % (rel, info.file_size))
        if info.file_size > limit:
            raise SourceError("'%s': too large (%d bytes, limit %d)" % (rel, info.file_size, limit))
        if info.compress_type == zipfile.ZIP_DEFLATED and info.file_size > RATIO_FREE and info.file_size // max(info.compress_size, 1) > RATIO:
            raise SourceError("'%s': refused, compression ratio above %d:1" % (rel, RATIO))
        try:
            data = self.zf.read(info)
        except (zipfile.BadZipFile, OSError, EOFError, RuntimeError, zlib_error()) as e:
            raise SourceError("'%s': %s" % (rel, e))
        return data


def zlib_error():
    import zlib
    return zlib.error


def open_source(path):
    if os.path.isdir(path):
        return DirSource(path)
    if os.path.isfile(path):
        return ZipSource(path)
    raise SourceError("no such asset pack (a directory or a .zip file)", unreadable=True)


# ----------------------------------------------------------------------------------------------------------------
# The check

def _reject_constant(name):
    raise ValueError("%s is not valid JSON" % name)


def _no_dups(pairs):
    seen = set()
    out = {}
    dups = []
    for k, v in pairs:
        if k in seen:
            dups.append(k)
        seen.add(k)
        out[k] = v
    if dups:
        out["\0dups"] = dups
    return out


def schema_known_fields(inst, sch, root, path, out):
    """Appends (path, unknown field) for every object member a `properties` schema does not list, walking `$ref`s, items,
    properties, additionalProperties and the branches of oneOf/anyOf/allOf that the instance is valid against."""
    if not isinstance(sch, dict):
        return
    if "$ref" in sch:
        schema_known_fields(inst, MS._resolve(root, sch["$ref"]), root, path, out)
    for sub in sch.get("allOf", []):
        schema_known_fields(inst, sub, root, path, out)
    for sub in sch.get("oneOf", []) + sch.get("anyOf", []):
        if not MS._check(inst, sub, path, root):
            schema_known_fields(inst, sub, root, path, out)
    if isinstance(inst, dict):
        props = sch.get("properties")
        for k, v in inst.items():
            if k == "\0dups":
                continue
            if props is not None and k not in props and "additionalProperties" not in sch:
                out.append((path, k))
            if props is not None and k in props:
                schema_known_fields(v, props[k], root, path + "." + k, out)
            elif "additionalProperties" in sch:
                schema_known_fields(v, sch["additionalProperties"], root, path + "." + k, out)
    elif isinstance(inst, list) and "items" in sch:
        for i, v in enumerate(inst):
            schema_known_fields(v, sch["items"], root, "%s[%d]" % (path, i), out)


def _pointer_to_section(path):
    """'$.graphics.<key>.png' -> ('graphics', '<key>'); '$.far[0].png' -> ('far[0]', '')."""
    parts = re.findall(r"\.([^.\[\]]+)|\[(\d+)\]", path)
    names = [a or "[%s]" % b for a, b in parts]
    if not names:
        return "", ""
    first = names[0]
    if len(names) > 1 and names[1].startswith("["):
        return first + names[1], ""
    if first in ("graphics", "screens") and len(names) > 1:
        return first, names[1]
    if first == "panel" and len(names) > 2 and names[1] == "cells":
        return "panel", names[2]
    if first == "audio" and len(names) > 2 and names[1] in ("sfx", "speech"):
        return "audio." + names[1], names[2]
    if first == "audio" and len(names) > 3 and names[1] == "music" and names[2] == "slots":
        return "audio.music.slots", names[3]
    return first, ""


class Checker:
    def __init__(self, path, game=None, build=None, against=None, full=False, strict=False):
        self.path, self.game, self.build, self.against, self.full, self.strict = path, game, build, against, full, strict
        self.rep = Report(os.path.basename(path.rstrip("/\\")) or path)
        self.pictures = {}      # rel -> Image or None
        self.picture_errors = set()
        self.picture_bytes = 0
        self.audio = {}         # rel -> dict(rate, frames, channels, peak)
        self.entries = 0
        self.skipped = 0
        self.src = None
        self.m = None

    # ---- loading helpers
    def add(self, sev, section, key, text):
        self.rep.add(sev, section, key, text)

    def fail(self, section, key, text):
        self.skipped += 1
        self.add(ERROR, section, key, text)

    def picture(self, rel, background=False):
        if rel in self.pictures:
            return self.pictures[rel]
        img = None
        try:
            data = self.src.read(rel, P.MAX_FILE_BYTES)
            side = P.MAX_SIDE_BACKGROUND if background else P.MAX_SIDE
            pixels = P.MAX_PIXELS_BACKGROUND if background else P.MAX_PIXELS
            img = P.decode_png(data, side, pixels)
            self.picture_bytes += img.w * img.h * 4
        except SourceError as e:
            self._picture_failed(rel, str(e))
        except P.PackError as e:
            self._picture_failed(rel, "'%s': %s" % (rel, e))
        self.pictures[rel] = img
        return img

    def _picture_failed(self, rel, why):
        self.picture_errors.add(rel)
        self.last_error = why

    def load_picture(self, section, key, rel, background=False):
        """The picture, or None after an Error message (the entry is skipped)."""
        self.last_error = ""
        img = self.picture(rel, background)
        if img is None:
            if not self.last_error:
                try:
                    self.src.read(rel, P.MAX_FILE_BYTES)
                except SourceError as e:
                    self.last_error = str(e)
                self.last_error = self.last_error or "'%s': cannot be read" % rel
            self.fail(section, key, self.last_error)
        return img

    def expect(self, section, key, rel, img, w, h, what):
        if img is not None and (img.w, img.h) != (w, h):
            self.add(WARNING, section, key, "'%s' is %dx%d, %s is expected to be %dx%d (a picture that overhangs through its anchor is fine, otherwise check the scale)"
                     % (rel, img.w, img.h, what, w, h))

    def probe_audio(self, rel, music):
        """Existence and a header check; WAV is read (and, with --full, its peak measured)."""
        if rel in self.audio:
            return self.audio[rel]
        info = None
        try:
            data = self.src.read(rel, 512 << 20)
        except SourceError as e:
            self.audio_error = str(e)
            self.audio[rel] = None
            return None
        self.audio_error = ""
        if data[:4] == b"RIFF" and data[8:12] == b"WAVE":
            try:
                with wave.open(io.BytesIO(data), "rb") as w:
                    ch, rate, frames, width = w.getnchannels(), w.getframerate(), w.getnframes(), w.getsampwidth()
                    if ch not in (1, 2):
                        self.audio_error = "'%s': %d channels (1 or 2 are supported)" % (rel, ch)
                    elif not 8000 <= rate <= 192000:
                        self.audio_error = "'%s': sample rate %d is outside 8000-192000" % (rel, rate)
                    else:
                        limit = 7200 if music else 120
                        if frames / rate > limit:
                            self.audio_error = "'%s': longer than %s" % (rel, "2 h" if music else "120 s")
                        else:
                            info = {"rate": rate, "frames": frames, "channels": ch, "peak": None}
                            if self.full and width in (1, 2):
                                raw = w.readframes(frames)
                                if width == 2:
                                    import array
                                    a = array.array("h")
                                    a.frombytes(raw[: len(raw) // 2 * 2])
                                    if sys.byteorder == "big":
                                        a.byteswap()
                                    info["peak"] = max((abs(v) for v in a), default=0) / 32768.0
                                else:
                                    info["peak"] = max((abs(v - 128) for v in raw), default=0) / 128.0
            except (wave.Error, EOFError) as e:
                self.audio_error = "'%s': %s" % (rel, e)
        elif data[:4] == b"fLaC" or data[:4] == b"OggS":
            info = {"rate": None, "frames": None, "channels": None, "peak": None}
        else:
            self.audio_error = "'%s': not a WAV, FLAC or Ogg Vorbis file" % rel
        self.audio[rel] = info
        return info

    # ---- the whole check
    def run(self):
        try:
            self.src = open_source(self.path)
        except SourceError as e:
            self.add(ERROR, "", "", str(e))
            return 3 if e.unreadable else 2
        for n in getattr(self.src, "notes", []):
            self.add(WARNING, "", "", n)
        try:
            raw = self.src.read("manifest.json", P.MAX_FILE_BYTES)
        except SourceError as e:
            self.add(ERROR, "", "", str(e))
            return 2
        try:
            m = json.loads(raw.decode("utf-8-sig"), parse_constant=_reject_constant, object_pairs_hook=_no_dups)
        except (ValueError, UnicodeDecodeError, RecursionError) as e:
            self.add(ERROR, "", "", "manifest.json: %s" % e)
            return 2
        if not isinstance(m, dict):
            self.add(ERROR, "", "", "manifest.json: the document must be a JSON object")
            return 2
        if m.get("format") != "x2-asset-pack":
            self.add(ERROR, "", "", 'manifest.json: "format" must be "x2-asset-pack"')
            return 2
        v = m.get("version")
        if isinstance(v, bool) or not isinstance(v, (int, float)) or v != int(v) or v < 1:
            self.add(ERROR, "", "", 'manifest.json: unsupported "version" (this runtime reads versions 1 to 2)')
            return 2
        if v > 2:
            self.add(ERROR, "", "", 'manifest.json: "version" %d needs a newer x2recomp (this one reads versions 1 to 2)' % v)
            return 2
        self.m = m
        if isinstance(m.get("name"), str) and 0 < len(m["name"]) <= 80:
            self.rep.label = m["name"]
        schema = P.read_json_file(SCHEMA_PATH)
        self.schema_check(m, schema)
        try:
            self.semantic(m)
        except (KeyError, TypeError, ValueError, AttributeError, IndexError):
            self.add(ERROR, "", "", "manifest.json: the structure is too damaged to check the entries further")
        if self.against:
            self.run_against(m)
        errors = self.rep.count(ERROR)
        return 2 if errors else 0

    def schema_check(self, m, schema):
        for e in MS.validate(m, schema):
            path, _, msg = e.partition(": ")
            section, key = _pointer_to_section(path)
            self.add(ERROR, section, key, "%s (%s)" % (msg, path))
        unknown = []
        schema_known_fields(m, schema, schema, "$", unknown)
        seen = {}
        for path, field in unknown:
            section, key = _pointer_to_section(path)
            e = seen.setdefault(("unknown", section, field), [0, key])
            e[0] += 1
        for dup in m.get("\0dups", []):
            seen.setdefault(("duplicate", "", dup), [0, ""])[0] += 1
        for (what, section, field), (count, key) in seen.items():
            text = 'unknown field "%s" (ignored' % field if what == "unknown" else 'duplicate field "%s" (the last one is used' % field
            if count > 1:
                text += "; also in %d other entr%s of this section" % (count - 1, "y" if count == 2 else "ies")
            self.add(WARNING, section, key, text + ")")
        if not m.get("license"):
            self.add(WARNING, "license", "", "missing: say under which terms the pack may be used and shared (an error with --strict)")

    def semantic(self, m):
        scale = m.get("scale", 1) if isinstance(m.get("scale", 1), int) else 1
        version = m.get("version", 2)
        v2 = version >= 2
        graphics_keys = {}

        def dup(key, where):
            if key in graphics_keys:
                self.add(WARNING, "graphics", key, "is defined twice (%s and %s); the later one wins" % (graphics_keys[key], where))
            else:
                graphics_keys[key] = where

        for key, e in (m.get("graphics") or {}).items():
            if not isinstance(e, dict) or not isinstance(e.get("png"), str):
                continue
            self.entries += 1
            k = key[2:] if key.startswith("f:") else key
            mm = P.GFX_KEY_RE.match(k)
            if not mm:
                continue
            w, h = int(mm.group(2)), int(mm.group(3))
            sc = e.get("scale", scale)
            if not key.startswith("f:"):
                dup(key, "graphics")
            img = self.load_picture("graphics", key, e["png"])
            if img is not None:
                self.expect("graphics", key, e["png"], img, w * sc, h * sc, "a graphic of this key at this scale")
            if isinstance(e.get("flash_png"), str):
                fi = self.load_picture("graphics", key, e["flash_png"])
                if fi is not None:
                    self.expect("graphics", key, e["flash_png"], fi, w * sc, h * sc, "the flash picture of this key at this scale")
            for alias in e.get("keys", []) if isinstance(e.get("keys"), list) else []:
                if isinstance(alias, str) and not alias.startswith("f:") and not key.startswith("f:"):
                    dup(alias, "graphics")
        for i, e in enumerate(m.get("far") or []):
            if not isinstance(e, dict) or not isinstance(e.get("png"), str) or not isinstance(e.get("lines"), list):
                continue
            self.entries += 1
            sc = e.get("scale", scale)
            sec = "far[%d]" % i
            img = self.load_picture(sec, "", e["png"])
            if img is None:
                continue
            if img.w != 320 * sc or img.h < len(e["lines"]) * sc:
                self.fail(sec, "", "the strip must be %d pixels wide and %d rows per line (%d lines)" % (320 * sc, sc, len(e["lines"])))
            elif img.h % sc:
                self.add(WARNING, sec, "", "'%s' is %d pixels tall, not a multiple of its scale %d" % (e["png"], img.h, sc))
        for i, e in enumerate(m.get("palettes") or []):
            self.entries += 1
        if v2:
            self.semantic_v2(m, scale, dup)
        self.references(m)
        est = self.picture_bytes
        self.add(INFO, "memory", "", "decoded pictures %s of %s" % (size_text(est), size_text(MAX_TOTAL_PICTURES)))
        if est > MAX_TOTAL_PICTURES // 10 * 8:
            self.add(WARNING, "memory", "", "the pictures use more than 80% of the memory limit for all packs")
        if self.game or self.build:
            self.scoping(m)

    def semantic_v2(self, m, scale, dup):
        for i, e in enumerate(m.get("backgrounds") or []):
            if not isinstance(e, dict) or not isinstance(e.get("png"), str) or not isinstance(e.get("level"), int):
                continue
            self.entries += 1
            sc = e.get("scale", scale)
            sec, key = "backgrounds[%d]" % i, "level%d" % e["level"]
            img = self.load_picture(sec, key, e["png"], True)
            if img is None:
                continue
            if img.w != 320 * sc:
                self.fail(sec, key, "the picture must be %d pixels wide (320 x scale), it is %d" % (320 * sc, img.w))
            elif img.h % sc or ("ring_lines" in e and img.h != e["ring_lines"] * sc):
                self.fail(sec, key, ("the picture must be %d pixels tall (ring_lines x scale), it is %d" % (e["ring_lines"] * sc, img.h))
                          if "ring_lines" in e else "the picture height %d is not a multiple of its scale" % img.h)
        for i, e in enumerate(m.get("atlases") or []):
            if not isinstance(e, dict) or not isinstance(e.get("png"), str):
                continue
            sc = e.get("scale", scale)
            sec = "atlases[%d]" % i
            sheet = self.load_picture(sec, "", e["png"])
            if sheet is None:
                continue

            def check(key, rect, anchor_ok=True):
                mm = P.GFX_KEY_RE.match(key) if isinstance(key, str) else None
                if not mm:
                    return
                self.entries += 1
                dup(key, sec)
                kw, kh = int(mm.group(2)), int(mm.group(3))
                x, y, w, h = rect
                if x + w > sheet.w or y + h > sheet.h:
                    self.fail(sec, key, "rect [%d, %d, %d, %d] lies outside the %dx%d atlas" % (x, y, w, h, sheet.w, sheet.h))
                elif (w, h) != (kw * sc, kh * sc):
                    self.add(WARNING, sec, key, "the frame is %dx%d, a graphic of this key at scale %d is expected to be %dx%d" % (w, h, sc, kw * sc, kh * sc))

            for f in e.get("frames") or []:
                if isinstance(f, dict) and isinstance(f.get("rect"), list) and len(f["rect"]) == 4:
                    check(f.get("key"), f["rect"])
                    if isinstance(f.get("flash_rect"), list) and len(f["flash_rect"]) == 4:
                        fx, fy, fw, fh = f["flash_rect"]
                        if fx + fw > sheet.w or fy + fh > sheet.h:
                            self.fail(sec, f.get("key", ""), "\"flash_rect\": rect lies outside the atlas")
            g = e.get("grid")
            if isinstance(g, dict) and isinstance(g.get("cell"), list) and len(g["cell"]) == 2 and isinstance(g.get("keys"), list):
                cw, ch = g["cell"]
                cols = g.get("columns") or max(1, sheet.w // cw)
                for n, k in enumerate(g["keys"]):
                    if k is not None:
                        check(k, [(n % cols) * cw, (n // cols) * ch, cw, ch])
        if isinstance(m.get("groups"), dict):
            for name, keys in m["groups"].items():
                for k in keys if isinstance(keys, list) else []:
                    if k not in {key for key in (m.get("graphics") or {})} and not any(
                            isinstance(a, dict) and (k in [f.get("key") for f in a.get("frames", []) if isinstance(f, dict)] or k in (a.get("grid", {}) or {}).get("keys", []))
                            for a in m.get("atlases") or []):
                        self.add(WARNING, "groups", name, "lists the key '%s', which no graphics entry or atlas defines" % k)
        for name, e in (m.get("screens") or {}).items():
            if not isinstance(e, dict) or not isinstance(e.get("png"), str):
                continue
            self.entries += 1
            sc = e.get("scale", scale)
            img = self.load_picture("screens", name, e["png"])
            if img is not None:
                self.expect("screens", name, e["png"], img, 320 * sc, 200 * sc, "a screen picture")
            if isinstance(e.get("wide_png"), str):
                self.load_picture("screens", name, e["wide_png"])
            for r in e.get("dynamic") or []:
                if isinstance(r, dict) and (r.get("x", 0) + r.get("w", 0) > 320 or r.get("y", 0) + r.get("h", 0) > 200):
                    self.add(WARNING, "screens", name, 'a "dynamic" rectangle reaches outside the 320x200 window')
                    break
            sig = e.get("signature") or {}
            if isinstance(sig.get("blocks"), list):
                if isinstance(sig.get("min"), int) and sig["min"] > len(sig["blocks"]):
                    self.fail("screens", name, 'signature "min" is larger than the number of blocks: the screen could never match')
                if len(sig["blocks"]) < 6:
                    self.add(WARNING, "screens", name, "the signature has fewer than 6 blocks: it may match other screens")
            if not e.get("ref_palette") and e.get("fade", "scale") == "scale":
                self.add(INFO, "screens", name, 'has no "ref_palette": the picture stays at full brightness while the game fades')
        pn = m.get("panel")
        if isinstance(pn, dict):
            sc = pn.get("scale", scale)
            cw = pn.get("cell_w", 8)
            bg = pn.get("background")
            if isinstance(bg, dict) and isinstance(bg.get("png"), str):
                self.entries += 1
                img = self.load_picture("panel", "", bg["png"])
                if img is not None:
                    self.expect("panel", "", bg["png"], img, 320 * sc, 8 * sc, "the panel background")
            for key, e in (pn.get("cells") or {}).items():
                if not isinstance(e, dict) or not isinstance(e.get("png"), str):
                    continue
                mm = P.CELL_KEY_RE.match(key)
                if not mm:
                    continue
                self.entries += 1
                csc = e.get("scale", sc)
                img = self.load_picture("panel", key, e["png"])
                if img is not None:
                    self.expect("panel", key, e["png"], img, cw * int(mm.group(2)) * csc, 8 * csc, "a panel run of this many cells")
        au = m.get("audio")
        if isinstance(au, dict):
            self.audio_section(au)

    def audio_section(self, au):
        for kind in ("sfx", "speech"):
            for key, e in (au.get(kind) or {}).items():
                if not isinstance(e, dict) or not isinstance(e.get("file"), str):
                    continue
                self.entries += 1
                info = self.probe_audio(e["file"], False)
                if info is None:
                    self.fail("audio." + kind, key, self.audio_error)
                    continue
                if info["peak"] is not None and info["peak"] > 1.0:
                    self.add(WARNING, "audio." + kind, key, "'%s' clips: its peak is %.1f dBFS above full scale" % (e["file"], 20 * math.log10(info["peak"])))
        for slot, e in ((au.get("music") or {}).get("slots") or {}).items():
            if not isinstance(e, dict):
                continue
            infos = {}
            for field in ("file", "intro"):
                rel = e.get(field)
                if not isinstance(rel, str):
                    continue
                self.entries += 1 if field == "file" else 0
                info = self.probe_audio(rel, True)
                if info is None:
                    self.fail("audio.music.slots", slot, '"%s": %s' % (field, self.audio_error))
                    continue
                infos[field] = info
                if self.src.is_zip and not self.src.stored(rel):
                    self.add(WARNING, "audio.music.slots", slot, "'%s': a ZIP member stored without compression streams from the archive; this one is deflated and is inflated into memory" % rel)
                if info["peak"] is not None and info["peak"] > 1.0:
                    self.add(WARNING, "audio.music.slots", slot, "'%s' clips: its peak is %.1f dBFS above full scale" % (rel, 20 * math.log10(info["peak"])))
            lp = e.get("loop")
            info = infos.get("file")
            if isinstance(lp, dict) and info and info["frames"] and info["rate"]:
                dur = info["frames"] / info["rate"]
                if lp.get("start_s", 0) >= dur:
                    self.add(ERROR, "audio.music.slots", slot, "the loop starts at %g s, after the end of the file (%g s)" % (lp["start_s"], dur))
                elif lp.get("end_s", 0) > dur + 2.0 / info["rate"]:
                    self.add(ERROR, "audio.music.slots", slot, "the loop ends at %g s, after the end of the file (%g s)" % (lp["end_s"], dur))

    def references(self, m):
        used = set()

        def use(p):
            if isinstance(p, str):
                used.add(p)

        for e in (m.get("graphics") or {}).values():
            if isinstance(e, dict):
                use(e.get("png")); use(e.get("flash_png"))
        for e in m.get("far") or []:
            if isinstance(e, dict):
                use(e.get("png"))
        for e in (m.get("backgrounds") or []) + (m.get("atlases") or []):
            if isinstance(e, dict):
                use(e.get("png"))
        for e in (m.get("screens") or {}).values():
            if isinstance(e, dict):
                use(e.get("png")); use(e.get("wide_png"))
        pn = m.get("panel")
        if isinstance(pn, dict):
            if isinstance(pn.get("background"), dict):
                use(pn["background"].get("png"))
            for e in (pn.get("cells") or {}).values():
                if isinstance(e, dict):
                    use(e.get("png"))
        au = m.get("audio") if isinstance(m.get("audio"), dict) else {}
        for kind in ("sfx", "speech"):
            for e in (au.get(kind) or {}).values():
                if isinstance(e, dict):
                    use(e.get("file"))
        for e in ((au.get("music") or {}).get("slots") or {}).values():
            if isinstance(e, dict):
                use(e.get("file")); use(e.get("intro"))
        use(m.get("preview"))
        if isinstance(m.get("preview"), str):
            try:
                if self.src.size(m["preview"]) > 1 << 20:
                    self.add(WARNING, "preview", "", "'%s' is larger than 1 MB" % m["preview"])
            except SourceError:
                self.add(ERROR, "preview", "", "missing file '%s'" % m["preview"])
        extra = []
        for n in self.src.names():
            low = n.lower()
            base = low.rsplit("/", 1)[-1]
            if n in used or n == "manifest.json":
                continue
            if base.startswith(("readme", "licen", "notice", "credits", "changelog", ".")) or low.endswith((".md", ".txt")):
                continue
            extra.append(n)
        if extra:
            self.add(INFO, "files", "", "%d file%s not used by manifest.json (%s%s)" % (len(extra), " is" if len(extra) == 1 else "s are", ", ".join(extra[:3]), ", ..." if len(extra) > 3 else ""))
        rq = m.get("requires")
        if isinstance(rq, dict):
            for q in rq.get("packs") or []:
                if isinstance(q, dict) and isinstance(q.get("id"), str):
                    self.add(INFO, "requires", q["id"], "needs the pack '%s'%s to be loaded before this one" % (q["id"], " " + q["version"] if q.get("version") else ""))
            if rq.get("x2recomp"):
                self.add(INFO, "requires", "x2recomp", "needs x2recomp %s" % rq["x2recomp"])

    def scoping(self, m):
        game = self.game or "disk"
        build = self.build or "canonical"

        def applies(scope):
            g = scope.get("games", ["disk", "cdtv", "st"]) if isinstance(scope, dict) else ["disk", "cdtv", "st"]
            b = scope.get("builds") if isinstance(scope, dict) else None
            return game in g and (not b or build in b)

        def both(a, b):
            ga = a.get("games", ["disk", "cdtv", "st"])
            gb = b.get("games", ["disk", "cdtv", "st"])
            if game not in ga or game not in gb:
                return False
            ba, bb = a.get("builds"), b.get("builds")
            return (not ba or build in ba) and (not bb or build in bb)

        if not applies(m):
            self.add(WARNING, "games", "", "the pack's games/builds do not include %s build %s: nothing of it would be used there" % (game, build))
            return
        total = inside = 0
        for name in ("graphics", "screens"):
            for e in (m.get(name) or {}).values():
                total += 1
                inside += both(m, e) if isinstance(e, dict) else 0
        for name in ("far", "palettes", "backgrounds", "atlases"):
            for e in m.get(name) or []:
                total += 1
                inside += both(m, e) if isinstance(e, dict) else 0
        au = m.get("audio") if isinstance(m.get("audio"), dict) else {}
        for kind in ("sfx", "speech"):
            for e in (au.get(kind) or {}).values():
                total += 1
                inside += both(m, e) if isinstance(e, dict) else 0
        for e in ((au.get("music") or {}).get("slots") or {}).values():
            total += 1
            inside += both(m, e) if isinstance(e, dict) else 0
        self.add(INFO, "scope", "", "%d of %d entries apply to %s, build %s" % (inside, total, game, build))

    # ---- --against DUMPDIR
    def run_against(self, m):
        dm_path = os.path.join(self.against, "manifest.json")
        try:
            dm = P.read_json_file(dm_path)
        except (P.PackError, OSError) as e:
            self.add(ERROR, "against", "", "cannot read %s: %s" % (dm_path, e))
            return

        def keys_of(man):
            r = {"graphics": {}, "far": set(), "panel": {}, "sfx": {}, "speech": {}, "screens": {}}
            for k, e in (man.get("graphics") or {}).items():
                if isinstance(e, dict) and not k.startswith("f:"):
                    r["graphics"][k] = e.get("png")
                    for a in e.get("keys", []) if isinstance(e.get("keys"), list) else []:
                        r["graphics"][a] = e.get("png")
            for a in man.get("atlases") or []:
                for f in (a.get("frames") or []) if isinstance(a, dict) else []:
                    r["graphics"].setdefault(f.get("key"), None)
                for k in ((a.get("grid") or {}).get("keys") or []) if isinstance(a, dict) else []:
                    if k:
                        r["graphics"].setdefault(k, None)
            for e in man.get("far") or []:
                for ln in (e.get("lines") or []) if isinstance(e, dict) else []:
                    if isinstance(ln, str) and re.match(r"^[0-9a-f]{16}$", ln):
                        r["far"].add(ln)
            for k, e in ((man.get("panel") or {}).get("cells") or {}).items():
                if isinstance(e, dict):
                    r["panel"][k] = e.get("png")
                    for a in e.get("keys", []) if isinstance(e.get("keys"), list) else []:
                        r["panel"][a] = e.get("png")
            au = man.get("audio") if isinstance(man.get("audio"), dict) else {}
            for kind in ("sfx", "speech"):
                for k, e in (au.get(kind) or {}).items():
                    if isinstance(e, dict):
                        r[kind][k] = e.get("file")
                        for a in e.get("keys", []) if isinstance(e.get("keys"), list) else []:
                            r[kind][a] = e.get("file")
            for name, e in (man.get("screens") or {}).items():
                blocks = ((e.get("signature") or {}).get("blocks") or []) if isinstance(e, dict) else []
                if blocks:
                    b = blocks[0]
                    r["screens"]["%s,%s,%s" % (b.get("x"), b.get("y"), b.get("hash"))] = e.get("png")
                    e["__sig"] = "%s,%s,%s" % (b.get("x"), b.get("y"), b.get("hash"))
            return r

        d, p = keys_of(dm), keys_of(m)
        hits = {}
        typos = 0
        label_of = {"graphics": "graphics", "panel": "panel", "sfx": "audio.sfx", "speech": "audio.speech"}
        for kind in ("graphics", "panel", "sfx", "speech"):
            hits[kind] = 0
            for k in p[kind]:
                if k in d[kind]:
                    hits[kind] += 1
                elif d[kind]:
                    typos += 1
                    if typos <= 50:
                        self.add(WARNING, label_of[kind], k, "this key is not in the dump: a typo, or a graphic or sound of another build")
        hits["far"] = 0
        for k in p["far"]:
            if k in d["far"]:
                hits["far"] += 1
            elif d["far"]:
                typos += 1
                if typos <= 50:
                    self.add(WARNING, "far", k, "this key is not in the dump: a typo, or a graphic or sound of another build")
        hits["screens"] = 0
        for name, e in (m.get("screens") or {}).items():
            blocks = ((e.get("signature") or {}).get("blocks") or []) if isinstance(e, dict) else []
            sig = "%s,%s,%s" % (blocks[0].get("x"), blocks[0].get("y"), blocks[0].get("hash")) if blocks else None
            if sig in d["screens"]:
                hits["screens"] += 1
            elif d["screens"]:
                self.add(WARNING, "screens", name, "its signature does not match any screen in the dump (the dump may not have reached it, or it is from another build)")
        if typos > 50:
            self.add(WARNING, "", "", "%d more keys are not in the dump" % (typos - 50))

        def pct(a, b):
            return "%d%%" % (a * 100 // b) if b else "-"

        self.add(INFO, "against", "", "coverage of the dump: graphics %d/%d (%s), far lines %d/%d (%s), screens %d/%d, panel cells %d/%d, sound effects %d/%d, speech %d/%d" % (
            hits["graphics"], len(d["graphics"]), pct(hits["graphics"], len(d["graphics"])), hits["far"], len(d["far"]), pct(hits["far"], len(d["far"])),
            hits["screens"], len(d["screens"]), hits["panel"], len(d["panel"]), hits["sfx"], len(d["sfx"]), hits["speech"], len(d["speech"])))

        done = set()

        def compare(section, key, pack_file, dump_file):
            if not isinstance(pack_file, str) or not isinstance(dump_file, str) or not P.is_safe_rel_path(dump_file) or (pack_file, dump_file) in done:
                return
            done.add((pack_file, dump_file))
            try:
                with open(P.safe_join(self.against, dump_file), "rb") as f:
                    a = hashlib.sha1(f.read()).hexdigest()
                b = hashlib.sha1(self.src.read(pack_file, 512 << 20)).hexdigest()
            except (P.PackError, OSError, SourceError):
                return
            if a == b:
                self.add(WARNING, section, key, "'%s' is byte-identical to the dump's original: a dump holds the game's own material and must not be redistributed" % pack_file)

        for k, f in p["graphics"].items():
            if k in d["graphics"]:
                compare("graphics", k, f, d["graphics"][k])
        for k, f in p["panel"].items():
            if k in d["panel"]:
                compare("panel", k, f, d["panel"][k])
        for kind in ("sfx", "speech"):
            for k, f in p[kind].items():
                if k in d[kind]:
                    compare(label_of[kind], k, f, d[kind][k])
        for name, e in (m.get("screens") or {}).items():
            blocks = ((e.get("signature") or {}).get("blocks") or []) if isinstance(e, dict) else []
            sig = "%s,%s,%s" % (blocks[0].get("x"), blocks[0].get("y"), blocks[0].get("hash")) if blocks else None
            if sig in d["screens"]:
                compare("screens", name, e.get("png"), d["screens"][sig])


def run_check(path, game=None, build=None, against=None, full=False, strict=False):
    """-> (Report, exit code, entries used, entries skipped)"""
    c = Checker(path, game, build, against, full, strict)
    code = c.run()
    if strict:
        for m in c.rep.messages:
            if m["severity"] == WARNING and m["section"] == "license":
                m["severity"] = ERROR
        if code == 0 and c.rep.count(ERROR):
            code = 2
    if code == 0 and strict and c.rep.count(WARNING):
        code = 1
    return c.rep, code, max(0, c.entries - c.skipped), c.skipped


def format_report(rep, code, entries, skipped, as_json):
    errors, warnings = rep.count(ERROR), rep.count(WARNING)
    if as_json:
        out = [dict(m, message=format_message(m)) for m in rep.messages]
        out.append({"severity": "summary", "errors": errors, "warnings": warnings, "entries": entries, "skipped": skipped, "exit_code": code})
        return json.dumps(out, indent=1) + "\n"
    lines = ["%s: %s" % (m["severity"], format_message(m)) for m in rep.messages]
    verdict = {0: "valid", 1: "valid with warnings (--strict)", 2: "not valid", 3: "cannot be read"}[code]
    lines.append("check: %s: %d error%s, %d warning%s, %d entries used, %d skipped: %s" % (
        rep.label or "pack", errors, "" if errors == 1 else "s", warnings, "" if warnings == 1 else "s", entries, skipped, verdict))
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack", help="pack folder or .zip file")
    ap.add_argument("--game", choices=("disk", "cdtv", "st"))
    ap.add_argument("--build", help="manifest build tag, e.g. power-pack-2234 (the canonical image is 'canonical')")
    ap.add_argument("--against", metavar="DUMPDIR", help="a --dump-assets / --dump-audio folder: coverage, typos, byte-identical copies")
    ap.add_argument("--full", action="store_true", help="decode WAV files completely (clipping)")
    ap.add_argument("--strict", action="store_true", help="warnings make the exit code 1; a missing license is an error")
    ap.add_argument("--json", action="store_true", help="the findings as a JSON array")
    a = ap.parse_args(argv)
    rep, code, entries, skipped = run_check(a.pack, a.game, a.build, a.against, a.full, a.strict)
    text = format_report(rep, code, entries, skipped, a.json)
    (sys.stderr if code == 3 and not a.json else sys.stdout).write(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
