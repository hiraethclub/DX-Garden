#!/usr/bin/env python3
"""Draws the globe texture, assets/map/world.png, and the coastline and
border lines, assets/map/lines.bin, from Natural Earth data.

Natural Earth (naturalearthdata.com) is public domain. This script downloads
three 1:50m GeoJSON layers once into build/naturalearth/ (git-ignored) and
draws land, lakes, coastlines and country borders in DX Garden's own
colours, as an equirectangular image (longitude -180..180 left to right,
latitude 90..-90 top to bottom).

This is map geography only. No station data is fetched or used.

The texture gives land and sea. The lines are drawn on top by the app as
real geometry, so coastlines stay sharp at any zoom.

lines.bin format (little-endian):
  "DXGL", u32 version (1), u32 tiles_x, u32 tiles_y, u32 layers
  for each layer, for each tile (row from the north, column from -180):
      u32 first_point, u32 point_count      (points come in pairs: GL_LINES)
  then all points: i16 lon, i16 lat, scaled so 32767 = 180 / 90 degrees.
Layer 0 is coastlines and lake shores, layer 1 is country borders. A
segment belongs to the tile holding its midpoint.

Usage: python3 tools/make_map.py [--width 4096]
Needs Pillow.
"""
import argparse
import json
import os
import struct
import sys
import urllib.request

from PIL import Image, ImageDraw, ImageFilter

NE_TAG = "v5.1.2"
NE_BASE = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
           + NE_TAG + "/geojson/")
LAYERS = {
    "land": "ne_50m_land.geojson",
    "lakes": "ne_50m_lakes.geojson",
    "borders": "ne_50m_admin_0_boundary_lines_land.geojson",
    "coast": "ne_50m_coastline.geojson",
}
TILE_DEG = 10
TILES_X = 360 // TILE_DEG
TILES_Y = 180 // TILE_DEG

# Our own dark palette: blue-black sea, slate land, lighter coast and borders.
SEA = (9, 17, 27)
LAND = (31, 41, 52)
COAST = (58, 72, 88)
BORDER = (50, 62, 76)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "naturalearth")
OUT = os.path.join(ROOT, "assets", "map", "world.png")
OUT_LINES = os.path.join(ROOT, "assets", "map", "lines.bin")


def fetch(name):
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        print("downloading", name)
        with urllib.request.urlopen(NE_BASE + name, timeout=60) as r:
            data = r.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def rings(geom):
    """Yields (exterior, [holes]) for Polygon / MultiPolygon."""
    if geom["type"] == "Polygon":
        yield geom["coordinates"][0], geom["coordinates"][1:]
    elif geom["type"] == "MultiPolygon":
        for poly in geom["coordinates"]:
            yield poly[0], poly[1:]


def lines(geom):
    if geom["type"] == "LineString":
        yield geom["coordinates"]
    elif geom["type"] == "MultiLineString":
        yield from geom["coordinates"]


def fill_rings(buf, W, H, ring_list, value):
    """Even-odd scanline fill of one polygon (exterior plus holes) into a
    W*H bytearray, sampling at pixel centres. Pillow's own polygon fill
    leaves stray horizontal lines across the sea on large polygons such as
    Eurasia, so this replaces it."""
    rows = {}
    for ring in ring_list:
        n = len(ring)
        for i in range(n):
            x0, y0 = ring[i]
            x1, y1 = ring[(i + 1) % n]
            if y0 == y1:
                continue
            if y0 > y1:
                x0, y0, x1, y1 = x1, y1, x0, y0
            # Rows whose centre (y + 0.5) lies in [y0, y1).
            ystart = max(0, int(-(-(y0 - 0.5) // 1)))
            yend = min(H - 1, int(-(-(y1 - 0.5) // 1)) - 1)
            slope = (x1 - x0) / (y1 - y0)
            for y in range(ystart, yend + 1):
                rows.setdefault(y, []).append(x0 + (y + 0.5 - y0) * slope)
    for y, xs in rows.items():
        xs.sort()
        base = y * W
        for i in range(0, len(xs) - 1, 2):
            a = max(0, int(-(-(xs[i] - 0.5) // 1)))
            b = min(W, int(-(-(xs[i + 1] - 0.5) // 1)))
            if b > a:
                buf[base + a:base + b] = bytes([value]) * (b - a)


def q(v, full):
    return max(-32767, min(32767, int(round(v / full * 32767))))


def write_lines(layers, path):
    """layers: list of lists of polylines ([(lon, lat), ...])."""
    tables, points = [], []
    for polylines in layers:
        tiles = [[] for _ in range(TILES_X * TILES_Y)]
        for pl in polylines:
            for a, b in zip(pl, pl[1:]):
                if abs(a[0] - b[0]) > 180:
                    continue  # never draw a segment the long way round
                mx = (a[0] + b[0]) / 2
                my = (a[1] + b[1]) / 2
                tx = min(TILES_X - 1, max(0, int((mx + 180) // TILE_DEG)))
                ty = min(TILES_Y - 1, max(0, int((90 - my) // TILE_DEG)))
                tiles[ty * TILES_X + tx].append((a, b))
        table = []
        for segs in tiles:
            table.append((len(points), len(segs) * 2))
            for a, b in segs:
                points.append((q(a[0], 180), q(a[1], 90)))
                points.append((q(b[0], 180), q(b[1], 90)))
        tables.append(table)
    with open(path, "wb") as f:
        f.write(b"DXGL" + struct.pack("<4I", 1, TILES_X, TILES_Y, len(layers)))
        for table in tables:
            for first, count in table:
                f.write(struct.pack("<2I", first, count))
        for lon, lat in points:
            f.write(struct.pack("<2h", lon, lat))
    print("wrote", path, len(points) // 2, "segments", os.path.getsize(path), "bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=4096)
    args = ap.parse_args()
    w = args.width
    h = w // 2
    ss = 2  # supersampling
    W, H = w * ss, h * ss

    def xy(pt):
        lon, lat = pt[0], pt[1]
        return ((lon + 180.0) / 360.0 * W, (90.0 - lat) / 180.0 * H)

    land = fetch(LAYERS["land"])
    lakes = fetch(LAYERS["lakes"])
    borders = fetch(LAYERS["borders"])

    # Land mask, with lakes cut out.
    buf = bytearray(W * H)
    for feat in land["features"]:
        for ext, holes in rings(feat["geometry"]):
            fill_rings(buf, W, H, [[xy(p) for p in r] for r in [ext] + holes], 255)
    for feat in lakes["features"]:
        for ext, holes in rings(feat["geometry"]):
            fill_rings(buf, W, H, [[xy(p) for p in r] for r in [ext] + holes], 0)
    mask = Image.frombytes("L", (W, H), bytes(buf))

    # Coastline: the edge of the mask.
    edges = mask.filter(ImageFilter.FIND_EDGES).point(lambda v: 255 if v > 0 else 0)

    # Country borders.
    bmask = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(bmask)
    for feat in borders["features"]:
        for ln in lines(feat["geometry"]):
            bd.line([xy(p) for p in ln], fill=255, width=ss)

    img = Image.new("RGB", (W, H), SEA)
    img.paste(Image.new("RGB", (W, H), LAND), (0, 0), mask)
    img.paste(Image.new("RGB", (W, H), BORDER), (0, 0), bmask)
    img.paste(Image.new("RGB", (W, H), COAST), (0, 0), edges)
    img = img.resize((w, h), Image.LANCZOS)

    coast = []
    for feat in fetch(LAYERS["coast"])["features"]:
        coast.extend(lines(feat["geometry"]))
    for feat in lakes["features"]:
        for ext, holes in rings(feat["geometry"]):
            coast.append(ext)
            coast.extend(holes)
    border = []
    for feat in borders["features"]:
        border.extend(lines(feat["geometry"]))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    write_lines([coast, border], OUT_LINES)
    # Few distinct colours, so a palette PNG keeps the file small.
    img.quantize(colors=64, method=Image.MEDIANCUT, dither=Image.NONE).save(
        OUT, optimize=True)
    print("wrote", OUT, "%dx%d" % (w, h), os.path.getsize(OUT), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
