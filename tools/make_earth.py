#!/usr/bin/env python3
"""Builds the globe's day imagery for the desktop app from NASA's Blue Marble.

Source: Blue Marble: Next Generation, July 2004, with topography and
bathymetry (NASA Earth Observatory, Reto Stöckli; public domain). The eight
500 m pieces (A1..D2, each 90 x 90 degrees, 21600 x 21600 px) are downloaded
once into the cache folder and checked against the SHA-256 sums below.

Output: earth-day.pack, a pyramid of 512 x 512 JPEG tiles in plate carree
(equirectangular) layout. Level L is 2^(L+1) x 2^L tiles; tile (0, 0) is the
north-west corner at 180 W, 90 N. Levels 0-5 go up to 32768 x 16384 px for
the whole Earth, about 1.2 km per pixel at the equator.

Pack format (little-endian): tile data, then an index of
(level u8, pad u8, x u16, y u16, pad u16, offset u64, size u32) per tile,
then a footer: index offset u64, tile count u32, b"DXGP", version u32.

Needs Pillow and about 1 GB of free memory.

  python3 tools/make_earth.py --cache third_party/earth --out assets-build
"""
import argparse
import hashlib
import io
import os
import struct
import sys
import urllib.request

from PIL import Image

Image.MAX_IMAGE_PIXELS = None

BASE = ("https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/"
        "bmng-topography-bathymetry/july/")
NAME = "world.topo.bathy.200407.3x21600x21600.%s.jpg"
# Columns A-D run west to east from 180 W; row 1 is the north, 2 the south.
PIECES = ["A1", "B1", "C1", "D1", "A2", "B2", "C2", "D2"]
SHA256 = {
    "A1": "cda310b5a180c7a7da95a605bc3a3a3fa082ecf870e0d9ea48994813216bcb70",
    "B1": "15d8ceb55676e6b3e7fdcd72217d1edd665b5e7f7396c606e7b98c427b28558e",
    "C1": "ee8490ab1eb35d620d8d1ad8e69b3234c0b050e4eddb80e7232a2d165e475aa0",
    "D1": "47480a4e98289d830adc689c32356771a83cd55a003cdbf32db7ade2a6609b56",
    "A2": "1f0b66979774a6a5704d6e597024e80b9e1fe71ab64ca871d195763ae625ad02",
    "B2": "37ce4c49ff093781eb1da10c14876cea317dfdf4d93af44d8e0c3b394bcd86d7",
    "C2": "3237e61c6ed8eaab15d7cdf797b6de3ee5c62314aa05381478b76dabcba2660b",
    "D2": "6883c2d0141b8165e0c336a3df7535bd494d56e75b0d1bce6d25719644237ea5",
}
TILE = 512
MAX_LEVEL = 5            # 32768 px around the equator
QUALITY = 88


def fetch(cache, piece):
    path = os.path.join(cache, NAME % piece)
    if not os.path.exists(path):
        print("downloading", NAME % piece, flush=True)
        tmp = path + ".part"
        urllib.request.urlretrieve(BASE + NAME % piece, tmp)
        os.replace(tmp, path)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    got = h.hexdigest()
    want = SHA256.get(piece)
    if want and got != want:
        sys.exit("SHA-256 mismatch for %s: %s" % (piece, got))
    if not want:
        print("  %s sha256 %s" % (piece, got))
    return path


class Pack:
    def __init__(self, path):
        self.f = open(path, "wb")
        self.index = []

    def add(self, level, x, y, img):
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=QUALITY, optimize=True, progressive=False)
        data = buf.getvalue()
        self.index.append((level, x, y, self.f.tell(), len(data)))
        self.f.write(data)

    def close(self):
        at = self.f.tell()
        for level, x, y, off, size in sorted(self.index):
            self.f.write(struct.pack("<BBHHHQI", level, 0, x, y, 0, off, size))
        self.f.write(struct.pack("<QI4sI", at, len(self.index), b"DXGP", 1))
        self.f.close()


def cut(pack, level, img, x0, y0):
    """Cuts img into tiles, the first being tile (x0, y0) of the level."""
    w, h = img.size
    for j in range(h // TILE):
        for i in range(w // TILE):
            box = (i * TILE, j * TILE, (i + 1) * TILE, (j + 1) * TILE)
            pack.add(level, x0 + i, y0 + j, img.crop(box))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", default="third_party/earth")
    ap.add_argument("--out", default="assets-build")
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)

    paths = {p: fetch(args.cache, p) for p in PIECES}
    pack = Pack(os.path.join(args.out, "earth-day.pack"))

    # Each piece is a quarter of the width and half the height. At the top
    # level a piece is 2^MAX_LEVEL tiles square.
    side_top = TILE << (MAX_LEVEL - 1)  # px per piece side at MAX_LEVEL: 8192 for 5
    per = side_top // TILE            # tiles per piece side at MAX_LEVEL
    # The whole Earth one level down, assembled from the pieces.
    canvas = Image.new("RGB", (4 * side_top // 2, 2 * side_top // 2))

    for n, piece in enumerate(PIECES):
        col, row = "ABCD".index(piece[0]), int(piece[1]) - 1
        print("piece %s (%d of 8)" % (piece, n + 1), flush=True)
        img = Image.open(paths[piece])
        # Decode at half size straight from the JPEG (10800 px): enough for
        # 8192 px and a fraction of the memory.
        img.draft("RGB", (21600 // 2, 21600 // 2))
        img = img.convert("RGB").resize((side_top, side_top), Image.LANCZOS)
        cut(pack, MAX_LEVEL, img, col * per, row * per)
        small = img.resize((side_top // 2, side_top // 2), Image.LANCZOS)
        canvas.paste(small, (col * side_top // 2, row * side_top // 2))
        del img, small

    level = MAX_LEVEL - 1
    while level >= 0:
        print("level", level, canvas.size, flush=True)
        cut(pack, level, canvas, 0, 0)
        if level:
            canvas = canvas.resize((canvas.size[0] // 2, canvas.size[1] // 2), Image.LANCZOS)
        level -= 1
    pack.close()
    print("wrote %s: %d tiles, %.1f MB" % (pack.f.name, len(pack.index),
                                          os.path.getsize(pack.f.name) / 1e6))


if __name__ == "__main__":
    main()
