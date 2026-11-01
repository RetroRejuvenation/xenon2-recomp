# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Shared helpers of the asset pack authoring tools (docs/re-notes/asset-packs-v2-design.md).

Standard library only, so every tool runs wherever Python 3 does:

  * a small PNG codec: `read_png` (greyscale, grey+alpha, RGB, RGBA, palette with tRNS; 1, 2, 4, 8 and 16 bits;
    not interlaced) and `write_png` (8-bit RGBA or RGB), both bounded against decode bombs;
  * `Image`, an 8-bit RGBA picture with crop, paste, edge bleed, integer scaling and box fill;
  * key syntax of the manifest (`<hash16>-<w>x<h>` graphics, `<hash16>-<n>` clips, `<hash16>-<span>` panel cells);
  * path rules identical to the schema's `relPath` and a zip-slip safe `safe_join`;
  * deterministic JSON reading and writing, with the manifest size limit.
"""
import json
import os
import re
import struct
import zlib

# Limits of docs/re-notes/asset-packs-v2-design.md section 3.5 (picture side / pixels, one PNG or manifest file).
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_SIDE = 4096
MAX_PIXELS = 8 * 1024 * 1024
MAX_SIDE_BACKGROUND = 16384
MAX_PIXELS_BACKGROUND = 32 * 1024 * 1024
MAX_SCALE = 8

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PackError(Exception):
    """A problem with an input file or value; the message says what and why."""


class PngError(PackError):
    pass


# ----------------------------------------------------------------------------------------------------------------
# Keys

GFX_KEY_RE = re.compile(r"^([0-9a-f]{16})-([1-9][0-9]*)x([1-9][0-9]*)$")
CLIP_KEY_RE = re.compile(r"^([0-9a-f]{16})-([1-9][0-9]*)$")
CELL_KEY_RE = re.compile(r"^([0-9a-f]{16})-([1-8])$")
FAR_LINE_RE = re.compile(r"^([0-9a-f]{16}|-|)$")


def parse_gfx_key(key):
    """'<hash16>-<w>x<h>' -> (hash16 string, w, h); PackError for anything else."""
    m = GFX_KEY_RE.match(key)
    if not m:
        raise PackError("not a graphics key (<16 hex digits>-<w>x<h>): %r" % (key,))
    return m.group(1), int(m.group(2)), int(m.group(3))


def format_gfx_key(hash16, w, h):
    """The inverse of `parse_gfx_key`; `hash16` is an int or a 16-digit lowercase hex string."""
    if isinstance(hash16, int):
        if not 0 <= hash16 < 1 << 64:
            raise PackError("hash out of range: %r" % (hash16,))
        hash16 = "%016x" % hash16
    if not re.match(r"^[0-9a-f]{16}$", hash16):
        raise PackError("hash must be 16 lowercase hex digits: %r" % (hash16,))
    if w < 1 or h < 1:
        raise PackError("size must be positive: %rx%r" % (w, h))
    return "%s-%dx%d" % (hash16, w, h)


def is_gfx_key(key):
    return bool(GFX_KEY_RE.match(key))


def parse_clip_key(key):
    """'<hash16>-<length>' (sound effect and speech keys) -> (hash16, length)."""
    m = CLIP_KEY_RE.match(key)
    if not m:
        raise PackError("not a clip key (<16 hex digits>-<length>): %r" % (key,))
    return m.group(1), int(m.group(2))


def parse_cell_key(key):
    """'<hash16>-<span>' (status panel cell keys, span 1-8 cells) -> (hash16, span)."""
    m = CELL_KEY_RE.match(key)
    if not m:
        raise PackError("not a panel cell key (<16 hex digits>-<1..8>): %r" % (key,))
    return m.group(1), int(m.group(2))


def fnv1a64(data, h=0xCBF29CE484222325):
    """FNV-1a, 64 bits (the hash family of the graphics keys; the tools never compute a key from a picture)."""
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ----------------------------------------------------------------------------------------------------------------
# Paths

_REL_BAD = re.compile(r"[\x00-\x1f]")


def is_safe_rel_path(p):
    """The schema's relPath rule: a relative '/'-separated path of at most 240 characters with no empty, '.' or '..'
    component, no backslash, no drive letter, no control characters."""
    if not isinstance(p, str) or not 1 <= len(p) <= 240:
        return False
    if p[0] in "/\\" or "\\" in p or _REL_BAD.search(p):
        return False
    if len(p) >= 2 and p[1] == ":" and p[0].isalpha():
        return False
    return all(part not in ("", ".", "..") for part in p.split("/"))


def safe_join(root, rel):
    """root/rel, refusing paths that are not `is_safe_rel_path` or that leave `root` (also through symlinks)."""
    if not is_safe_rel_path(rel):
        raise PackError("unsafe path in a pack: %r" % (rel,))
    base = os.path.realpath(root)
    full = os.path.realpath(os.path.join(base, *rel.split("/")))
    if full != base and not full.startswith(base + os.sep):
        raise PackError("path leaves the pack folder: %r" % (rel,))
    return full


# ----------------------------------------------------------------------------------------------------------------
# JSON

def read_json_file(path, max_bytes=MAX_FILE_BYTES):
    """Parse a UTF-8 JSON file; PackError if it is too large, not UTF-8 or not JSON."""
    try:
        size = os.path.getsize(path)
    except OSError as e:
        raise PackError("cannot read %s: %s" % (path, e))
    if size > max_bytes:
        raise PackError("%s is larger than %d bytes" % (path, max_bytes))
    try:
        with open(path, "rb") as f:
            return json.loads(f.read().decode("utf-8-sig"))
    except (ValueError, OSError) as e:
        raise PackError("%s is not valid JSON: %s" % (path, e))


def json_text(obj):
    """Deterministic JSON text: two-space indent, insertion order kept, LF line ends, trailing newline."""
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


def write_json_file(path, obj):
    with open(path, "wb") as f:
        f.write(json_text(obj).encode("utf-8"))


# ----------------------------------------------------------------------------------------------------------------
# Pictures

class Image:
    """An 8-bit RGBA picture: `w`, `h` and `data`, a bytearray of w*h*4 bytes (row-major, R G B A)."""

    __slots__ = ("w", "h", "data")

    def __init__(self, w, h, data=None):
        if w < 1 or h < 1:
            raise PackError("empty picture: %dx%d" % (w, h))
        self.w = w
        self.h = h
        self.data = bytearray(w * h * 4) if data is None else bytearray(data)
        if len(self.data) != w * h * 4:
            raise PackError("picture data is %d bytes, expected %d" % (len(self.data), w * h * 4))

    def get(self, x, y):
        i = (y * self.w + x) * 4
        return tuple(self.data[i:i + 4])

    def put(self, x, y, rgba):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.data[i:i + 4] = bytes(rgba)

    def fill_rect(self, x, y, w, h, rgba):
        px = bytes(rgba)
        x0, x1 = max(0, x), min(self.w, x + w)
        for yy in range(max(0, y), min(self.h, y + h)):
            if x1 > x0:
                self.data[(yy * self.w + x0) * 4:(yy * self.w + x1) * 4] = px * (x1 - x0)

    def crop(self, x, y, w, h):
        if w < 1 or h < 1 or x < 0 or y < 0 or x + w > self.w or y + h > self.h:
            raise PackError("crop %d,%d %dx%d lies outside the %dx%d picture" % (x, y, w, h, self.w, self.h))
        out = bytearray()
        for yy in range(y, y + h):
            out += self.data[(yy * self.w + x) * 4:(yy * self.w + x + w) * 4]
        return Image(w, h, out)

    def paste(self, src, x, y):
        """Copy `src` (replacing, not blending) with its top-left corner at x, y; parts outside are clipped."""
        for sy in range(src.h):
            dy = y + sy
            if not 0 <= dy < self.h:
                continue
            sx0 = max(0, -x)
            sx1 = min(src.w, self.w - x)
            if sx1 > sx0:
                self.data[(dy * self.w + x + sx0) * 4:(dy * self.w + x + sx1) * 4] = \
                    src.data[(sy * src.w + sx0) * 4:(sy * src.w + sx1) * 4]

    def bleed(self, x, y, w, h, n):
        """Extend the edge pixels of the rectangle x, y, w, h outwards by n pixels (texture padding); stays inside."""
        for k in range(1, n + 1):
            for yy in range(y, y + h):
                for dx, sx in ((x - k, x), (x + w - 1 + k, x + w - 1)):
                    if 0 <= dx < self.w:
                        i = (yy * self.w + sx) * 4
                        self.data[(yy * self.w + dx) * 4:(yy * self.w + dx) * 4 + 4] = self.data[i:i + 4]
        for k in range(1, n + 1):
            for xx in range(max(0, x - n), min(self.w, x + w + n)):
                for dy, sy in ((y - k, y), (y + h - 1 + k, y + h - 1)):
                    if 0 <= dy < self.h:
                        i = (sy * self.w + xx) * 4
                        self.data[(dy * self.w + xx) * 4:(dy * self.w + xx) * 4 + 4] = self.data[i:i + 4]

    def scaled(self, factor):
        """Nearest-neighbour enlargement by an integer factor."""
        if factor < 1 or factor > MAX_SCALE:
            raise PackError("scale factor must be 1-%d" % MAX_SCALE)
        if factor == 1:
            return Image(self.w, self.h, self.data)
        out = bytearray()
        for y in range(self.h):
            row = bytearray()
            for x in range(self.w):
                row += self.data[(y * self.w + x) * 4:(y * self.w + x) * 4 + 4] * factor
            out += bytes(row) * factor
        return Image(self.w * factor, self.h * factor, out)

    def is_opaque(self):
        return all(a == 255 for a in self.data[3::4])

    def opaque_brightness(self):
        """Sum of R+G+B over the pixels with alpha above zero (used to pick the brighter of two dumped copies)."""
        d = self.data
        return sum(d[i] + d[i + 1] + d[i + 2] for i in range(0, len(d), 4) if d[i + 3])

    def __eq__(self, other):
        return isinstance(other, Image) and (self.w, self.h, self.data) == (other.w, other.h, other.data)

    def __hash__(self):
        return hash((self.w, self.h, bytes(self.data)))


# ----------------------------------------------------------------------------------------------------------------
# PNG reader

_COLOR_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
_VALID_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}


def _unfilter(raw, w, h, bpp, stride):
    out = bytearray(h * stride)
    pos = 0
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ft == 0:
            pass
        elif ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        else:
            raise PngError("bad PNG filter type %d" % ft)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def decode_png(data, max_side=MAX_SIDE, max_pixels=MAX_PIXELS, max_bytes=MAX_FILE_BYTES):
    """Decode PNG bytes into an `Image` (RGBA, 8 bits). Chunk CRCs are checked; the picture size is checked against
    the limits before anything is allocated; interlaced files are refused."""
    if len(data) > max_bytes:
        raise PngError("PNG file larger than %d bytes" % max_bytes)
    if data[:8] != PNG_SIGNATURE:
        raise PngError("not a PNG file (bad signature)")
    pos = 8
    ihdr = None
    plte = None
    trns = None
    idat = bytearray()
    seen_end = False
    while pos < len(data):
        if pos + 8 > len(data):
            raise PngError("truncated PNG chunk header")
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        if length > 0x7FFFFFFF or pos + 12 + length > len(data):
            raise PngError("truncated or oversized PNG chunk %r" % ctype)
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise PngError("PNG chunk %r has a bad CRC" % ctype)
        pos += 12 + length
        if ctype == b"IHDR":
            if ihdr is not None or length != 13:
                raise PngError("bad IHDR")
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ihdr is None:
            raise PngError("PNG chunk before IHDR")
        elif ctype == b"PLTE":
            if length % 3 or not 3 <= length <= 768:
                raise PngError("bad PLTE")
            plte = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat += body
            if len(idat) > max_bytes:
                raise PngError("PNG image data larger than %d bytes" % max_bytes)
        elif ctype == b"IEND":
            seen_end = True
            break
    if ihdr is None or not seen_end:
        raise PngError("PNG has no IHDR or no IEND")
    w, h, depth, ctype_, comp, filt, interlace = ihdr
    if w < 1 or h < 1:
        raise PngError("empty PNG")
    if w > max_side or h > max_side or w * h > max_pixels:
        raise PngError("PNG is %dx%d: larger than the limit of %d pixels a side / %d pixels" % (w, h, max_side, max_pixels))
    if ctype_ not in _COLOR_CHANNELS or depth not in _VALID_DEPTHS[ctype_]:
        raise PngError("unsupported PNG colour type %d / bit depth %d" % (ctype_, depth))
    if comp != 0 or filt != 0:
        raise PngError("unsupported PNG compression or filter method")
    if interlace != 0:
        raise PngError("interlaced (Adam7) PNG files are not supported: save the picture without interlacing")
    if ctype_ == 3 and plte is None:
        raise PngError("palette PNG without PLTE")
    channels = _COLOR_CHANNELS[ctype_]
    bits_pp = channels * depth
    stride = (w * bits_pp + 7) // 8
    bpp = max(1, bits_pp // 8)
    expect = (stride + 1) * h
    d = zlib.decompressobj()
    try:
        raw = d.decompress(bytes(idat), expect + 1)
    except zlib.error as e:
        raise PngError("PNG image data is corrupt: %s" % e)
    if len(raw) != expect or d.unconsumed_tail or not d.eof:
        if len(raw) < expect:
            raise PngError("PNG image data is too short")
        raise PngError("PNG image data is longer than the picture")
    pix = _unfilter(raw, w, h, bpp, stride)
    out = bytearray(w * h * 4)
    # transparency key for grey / RGB
    tkey = None
    if trns is not None and ctype_ in (0, 2):
        tkey = struct.unpack(">%dH" % (len(trns) // 2), trns[:channels * 2 if ctype_ == 2 else 2])
    pal = None
    if ctype_ == 3:
        n = len(plte) // 3
        pal = [(plte[3 * i], plte[3 * i + 1], plte[3 * i + 2], (trns[i] if trns is not None and i < len(trns) else 255)) for i in range(n)]

    def sample(row_off, idx):
        if depth == 8:
            return pix[row_off + idx]
        if depth == 16:
            return (pix[row_off + 2 * idx] << 8) | pix[row_off + 2 * idx + 1]
        per = 8 // depth
        byte = pix[row_off + idx // per]
        shift = 8 - depth * (idx % per + 1)
        return (byte >> shift) & ((1 << depth) - 1)

    scale_to8 = {1: 255, 2: 85, 4: 17}.get(depth)
    o = 0
    for y in range(h):
        ro = y * stride
        for x in range(w):
            if ctype_ == 3:
                i = sample(ro, x)
                if i >= len(pal):
                    raise PngError("palette index %d out of range" % i)
                r, g, b, a = pal[i]
            elif ctype_ == 0:
                v = sample(ro, x)
                a = 255
                if tkey is not None and v == tkey[0]:
                    a = 0
                g = v * scale_to8 if scale_to8 else (v if depth == 8 else v >> 8)
                r = b = g
            elif ctype_ == 4:
                v = sample(ro, 2 * x)
                a = sample(ro, 2 * x + 1)
                if depth == 16:
                    v >>= 8
                    a >>= 8
                r = g = b = v
            else:
                cs = [sample(ro, channels * x + k) for k in range(channels)]
                a = 255
                if ctype_ == 2 and tkey is not None and tuple(cs) == tuple(tkey):
                    a = 0
                if ctype_ == 6:
                    a = cs[3] if depth == 8 else cs[3] >> 8
                if depth == 16:
                    cs = [c >> 8 for c in cs]
                r, g, b = cs[0], cs[1], cs[2]
            out[o:o + 4] = bytes((r, g, b, a))
            o += 4
    return Image(w, h, out)


def read_png(path, max_side=MAX_SIDE, max_pixels=MAX_PIXELS):
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_FILE_BYTES + 1)
    except OSError as e:
        raise PngError("cannot read %s: %s" % (path, e))
    try:
        return decode_png(data, max_side, max_pixels)
    except PngError as e:
        raise PngError("%s: %s" % (path, e))


# ----------------------------------------------------------------------------------------------------------------
# PNG writer

def _chunk(ctype, body):
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF)


def _filter_rows(rows, stride, bpp):
    """Per row, the filter among None, Sub and Up with the smallest sum of absolute values (deterministic)."""
    out = bytearray()
    prev = bytes(stride)
    for line in rows:
        cands = [(0, line)]
        sub = bytearray(stride)
        for i in range(stride):
            sub[i] = (line[i] - (line[i - bpp] if i >= bpp else 0)) & 255
        cands.append((1, bytes(sub)))
        up = bytes((line[i] - prev[i]) & 255 for i in range(stride))
        cands.append((2, up))
        best = min(cands, key=lambda c: (sum(v if v < 128 else 256 - v for v in c[1]), c[0]))
        out.append(best[0])
        out += best[1]
        prev = line
    return bytes(out)


def encode_png(img):
    """PNG bytes of an `Image`: 8-bit RGB when every pixel is opaque, otherwise RGBA. Deterministic."""
    opaque = img.is_opaque()
    if opaque:
        pix = bytearray()
        for i in range(0, len(img.data), 4):
            pix += img.data[i:i + 3]
        channels = 3
    else:
        pix = img.data
        channels = 4
    stride = img.w * channels
    rows = [bytes(pix[y * stride:(y + 1) * stride]) for y in range(img.h)]
    raw = _filter_rows(rows, stride, channels)
    ihdr = struct.pack(">IIBBBBB", img.w, img.h, 8, 2 if opaque else 6, 0, 0, 0)
    return PNG_SIGNATURE + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(raw, 9)) + _chunk(b"IEND", b"")


def write_png(path, img):
    data = encode_png(img)
    if len(data) > MAX_FILE_BYTES:
        raise PngError("%s would be larger than %d bytes" % (path, MAX_FILE_BYTES))
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
