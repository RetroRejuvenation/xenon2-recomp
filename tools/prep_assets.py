#!/usr/bin/env python3
"""Prepare the runtime's input files from the user's own Xenon 2 disk images.

Writes <out>/game_0400.bin (the depacked game image as it lives at $400 at run time, $3FC00 bytes) and
<out>/disc2.bin (the flat 1760-sector game disc). The runtime (x2recomp --assets <out>) loads only these.

Usage:
  prep_assets.py --zip Xenon2.zip [--out generated/assets]
  prep_assets.py --disc1 disc1.adf --disc2 disc2.adf [--out generated/assets]

Disc 1 must be the AmigaDOS disc holding the packed XENON-II executable; disc 2 the raw "X2B " disc.
Nothing is patched: the crack/trainer stub that normally copies the image to $400 is skipped entirely.
"""
import argparse
import hashlib
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adf  # noqa: E402
import unpack_xenon2  # noqa: E402

ADF_SIZE = 901120
IMAGE_SIZE = 0x3FC00


def adfs_from_zip(path):
    with zipfile.ZipFile(path) as z:
        names = sorted(n for n in z.namelist() if n.lower().endswith('.adf'))
        disks = [z.read(n) for n in names]
    d1 = d2 = None
    for d in disks:
        if len(d) != ADF_SIZE:
            continue
        if d[2:6] == b'X2B ':
            d2 = d
        elif d[:3] == b'DOS':
            d1 = d
    return d1, d2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip')
    ap.add_argument('--disc1')
    ap.add_argument('--disc2')
    ap.add_argument('--out', default=os.path.join('generated', 'assets'))
    a = ap.parse_args()

    if a.zip:
        d1, d2 = adfs_from_zip(a.zip)
    else:
        if not (a.disc1 and a.disc2):
            ap.error('give --zip or both --disc1 and --disc2')
        d1, d2 = open(a.disc1, 'rb').read(), open(a.disc2, 'rb').read()
    if not d1 or len(d1) != ADF_SIZE or d1[:3] != b'DOS':
        sys.exit('disc 1: not an 880 KB AmigaDOS ADF')
    if not d2 or len(d2) != ADF_SIZE or d2[2:6] != b'X2B ':
        sys.exit('disc 2: not an 880 KB ADF with the "X2B " disc id')

    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(a.out, '.disc1.adf')
    with open(tmp, 'wb') as f:
        f.write(d1)
    try:
        vol = adf.Adf(tmp)
        entry = vol.find('XENON-II')
        if entry is None:
            sys.exit('disc 1: no XENON-II file')
        exe = vol.read_file(entry)
    finally:
        os.remove(tmp)

    image = unpack_xenon2.unpack(unpack_xenon2.code_hunk(exe))
    if len(image) < IMAGE_SIZE:
        sys.exit('depacked image too small (%d bytes)' % len(image))
    game = image[:IMAGE_SIZE]
    if game[:4] != bytes.fromhex('60006c30'):
        sys.exit('depacked image does not start with bra.w $7032')

    for name, data in (('game_0400.bin', game), ('disc2.bin', d2)):
        path = os.path.join(a.out, name)
        with open(path, 'wb') as f:
            f.write(data)
        print('%-40s %7d bytes sha1 %s' % (path, len(data), hashlib.sha1(data).hexdigest()))


if __name__ == '__main__':
    main()
