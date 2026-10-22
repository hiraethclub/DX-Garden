#!/usr/bin/env python3
"""The globe artwork for DX Garden's LiveArea, drawn by
`python3 tools/make_livearea.py render` (which saves and installs it).

The globe is rendered from the app's own map, assets/map/world.png (land
and sea) and assets/map/lines.bin (coastlines and borders), both drawn by
tools/make_map.py from public-domain Natural Earth data. The gate's name
uses the shipped Noto Sans Bold. Colours follow vita/gfx.h and the globe
view. The place dots are a handful of well-known cities, for decoration
only; no station data is used.

Images are drawn at 3x and scaled down, so edges stay smooth after the
palette reduction. First written in a cloud session (commit b2bc9e7).

Needs Pillow and numpy.
"""
import math
import os
import random
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_PNG = os.path.join(ROOT, "assets", "map", "world.png")
MAP_LINES = os.path.join(ROOT, "assets", "map", "lines.bin")
FONT_BOLD = os.path.join(ROOT, "assets", "fonts", "NotoSans-Bold.ttf")
FONT_REGULAR = os.path.join(ROOT, "assets", "fonts", "NotoSans-Regular.ttf")

SS = 3  # supersampling factor

# vita/gfx.h
BG = (14, 17, 22)
TEXT = (232, 234, 237)
DIM = (140, 149, 163)
ACCENT = (242, 165, 65)
INFO = (90, 176, 240)
# Globe, a little brighter than the in-app texture so it reads at icon size.
SEA = (16, 33, 52)
LAND = (52, 70, 89)
COAST = (120, 142, 166)
BORDER = (82, 100, 122)
GRID = (60, 84, 110)
ATMO = (70, 140, 205)
SKY_LOW = (17, 30, 46)

# Well-known cities (lat, lon), only to scatter a few dots on the globe.
CITIES = [
    (51.5, -0.1), (48.9, 2.4), (52.5, 13.4), (41.9, 12.5), (40.4, -3.7),
    (59.3, 18.1), (55.8, 37.6), (50.4, 30.5), (52.2, 21.0), (41.0, 29.0),
    (37.98, 23.7), (30.0, 31.2), (36.8, 3.1), (33.6, -7.6), (6.5, 3.4),
    (-1.3, 36.8), (9.0, 38.7), (14.7, -17.4), (-26.2, 28.0), (-33.9, 18.4),
    (24.7, 46.7), (25.3, 55.3), (35.7, 51.4), (33.9, 35.5), (31.8, 35.2),
    (19.1, 72.9), (28.6, 77.2), (13.1, 80.3), (23.8, 90.4), (64.1, -21.9),
    (53.3, -6.3), (55.7, 12.6), (60.2, 24.9), (47.5, 19.0), (44.4, 26.1),
    (38.7, -9.1), (45.5, 9.2), (48.2, 16.4), (50.1, 14.4), (42.7, 23.3),
    (40.7, -74.0), (34.1, -118.2), (19.4, -99.1), (-23.6, -46.6),
    (-34.6, -58.4), (35.7, 139.7), (39.9, 116.4), (1.35, 103.8),
    (-33.9, 151.2), (37.6, 127.0), (43.7, -79.4), (4.7, -74.1),
    (12.0, 8.5), (15.6, 32.5), (-4.3, 15.3), (5.6, -0.2), (36.2, 37.2),
    (41.3, 69.3), (43.2, 76.9), (34.5, 69.2), (24.9, 67.0),
]


# --------------------------------------------------------------- map data

def load_land():
    """Land mask from world.png: land is the lighter of its two fills."""
    img = Image.open(MAP_PNG).convert("L")
    return np.asarray(img) > 20


def load_lines():
    """Coastline and border segments from lines.bin, as (N, 2, 2) arrays of
    (lat, lon) degree pairs, one array per layer."""
    with open(MAP_LINES, "rb") as f:
        data = f.read()
    magic, _ver, tx, ty, layers = struct.unpack_from("<4sIIII", data, 0)
    if magic != b"DXGL":
        raise SystemExit(MAP_LINES + ": not a DXGL file")
    off = 20
    index = struct.unpack_from("<%dI" % (layers * tx * ty * 2), data, off)
    off += layers * tx * ty * 8
    pts = np.frombuffer(data, dtype="<i2", offset=off).reshape(-1, 2).astype(np.float64)
    lon = pts[:, 0] * 180.0 / 32767.0
    lat = pts[:, 1] * 90.0 / 32767.0
    out = []
    per = tx * ty
    for layer in range(layers):
        idx = []
        for t in range(per):
            first, count = index[(layer * per + t) * 2], index[(layer * per + t) * 2 + 1]
            idx.append(np.arange(first, first + count))
        idx = np.concatenate(idx) if idx else np.zeros(0, int)
        seg = np.stack([lat[idx], lon[idx]], axis=1).reshape(-1, 2, 2)
        out.append(seg)
    return out


# ------------------------------------------------------------------ globe

class View:
    """Orthographic view of the globe: screen centre, radius in pixels,
    and the latitude/longitude facing the viewer."""

    def __init__(self, cx, cy, r, lat0, lon0):
        self.cx, self.cy, self.r = cx, cy, r
        self.p0, self.l0 = math.radians(lat0), math.radians(lon0)

    def project(self, lat, lon):
        """Degrees to screen x, y and depth z (z > 0 faces the viewer)."""
        p, l = np.radians(lat), np.radians(lon) - self.l0
        cp = np.cos(p)
        x = cp * np.sin(l)
        y = np.sin(p) * math.cos(self.p0) - cp * np.cos(l) * math.sin(self.p0)
        z = np.sin(p) * math.sin(self.p0) + cp * np.cos(l) * math.cos(self.p0)
        return self.cx + x * self.r, self.cy - y * self.r, z


def blend(canvas, mask, colour):
    """canvas = mix(canvas, colour, mask), mask in 0..1, shape (H, W)."""
    m = mask[..., None]
    canvas *= 1.0 - m
    canvas += m * np.asarray(colour, np.float32)


def add(canvas, mask, colour):
    canvas += mask[..., None] * np.asarray(colour, np.float32)


def draw_globe(canvas, view, land, lines, coast_w, border_w, grid_w, glow, coast_a=0.9, light=1.0):
    """Shaded globe with coastlines, borders and a graticule, plus an
    atmosphere glow of `glow` radii around it."""
    h, w, _ = canvas.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x = (xs + 0.5 - view.cx) / view.r
    y = (view.cy - ys - 0.5) / view.r
    d2 = x * x + y * y
    inside = d2 < 1.0
    z = np.sqrt(np.clip(1.0 - d2, 0.0, 1.0))

    # Glow outside the disc.
    d = np.sqrt(d2)
    out = np.clip(1.0 - (d - 1.0) / glow, 0.0, 1.0) * (~inside)
    add(canvas, 0.55 * out ** 2.2, ATMO)

    # Inverse projection for the pixels on the disc.
    cp0, sp0 = math.cos(view.p0), math.sin(view.p0)
    lat = np.degrees(np.arcsin(np.clip(y * cp0 + z * sp0, -1, 1)))
    lon = np.degrees(view.l0 + np.arctan2(x, z * cp0 - y * sp0))
    lon = (lon + 180.0) % 360.0 - 180.0
    mh, mw = land.shape
    u = np.clip(((lon + 180.0) / 360.0 * mw).astype(int), 0, mw - 1)
    v = np.clip(((90.0 - lat) / 180.0 * mh).astype(int), 0, mh - 1)
    is_land = land[v, u]

    base = np.where(is_land[..., None], np.float32(LAND), np.float32(SEA)).astype(np.float32)
    # Light from the upper left, towards the viewer.
    L = np.array([-0.45, 0.55, 0.70])
    L /= np.linalg.norm(L)
    diffuse = np.clip(x * L[0] + y * L[1] + z * L[2], 0.0, 1.0)
    shade = (0.42 + 0.78 * diffuse) * light
    disc = base * shade[..., None]
    # Atmosphere towards the rim.
    rim = (1.0 - z) ** 2.5
    disc = disc * (1.0 - 0.55 * rim[..., None]) + np.float32(ATMO) * (0.55 * rim[..., None])

    # Antialiased disc edge (one pixel wide).
    edge = np.clip((1.0 - d) * view.r, 0.0, 1.0)
    blend(canvas, edge * inside, (0, 0, 0))
    canvas += disc * (edge * inside)[..., None]

    # Lines: graticule, borders, coastlines.
    grid = []
    for la in range(-60, 61, 30):
        lo = np.arange(-180, 181, 2.0)
        pts = np.stack([np.full_like(lo, la), lo], 1)
        grid.append(np.stack([pts[:-1], pts[1:]], 1))
    for lo in range(-180, 180, 30):
        la = np.arange(-90, 91, 2.0)
        pts = np.stack([la, np.full_like(la, lo)], 1)
        grid.append(np.stack([pts[:-1], pts[1:]], 1))
    layers = [(np.concatenate(grid), grid_w, GRID, 0.55)]
    if len(lines) > 1 and border_w:
        layers.append((lines[1], border_w, BORDER, 0.8))
    layers.append((lines[0], coast_w, COAST, coast_a))
    for seg, width, colour, alpha in layers:
        mask = line_mask((h, w), view, seg, width)
        blend(canvas, mask * alpha * edge * inside, colour)


def line_mask(shape, view, seg, width):
    h, w = shape
    sx, sy, sz = view.project(seg[..., 0], seg[..., 1])
    vis = (sz[:, 0] > 0) & (sz[:, 1] > 0)
    x0, x1 = sx[:, 0], sx[:, 1]
    y0, y1 = sy[:, 0], sy[:, 1]
    vis &= (np.maximum(x0, x1) >= -width) & (np.minimum(x0, x1) <= w + width)
    vis &= (np.maximum(y0, y1) >= -width) & (np.minimum(y0, y1) <= h + width)
    img = Image.new("L", (w, h), 0)
    dr = ImageDraw.Draw(img)
    # Fade lines out towards the rim, where they bunch up.
    fade = np.clip(np.minimum(sz[:, 0], sz[:, 1]) * 4.0, 0.0, 1.0)
    for i in np.nonzero(vis)[0]:
        dr.line([(x0[i], y0[i]), (x1[i], y1[i])], fill=int(255 * fade[i]), width=width)
    return np.asarray(img, np.float32) / 255.0


def draw_places(canvas, view, size, focus=None, rings=0, ring_step=0.0, ring_w=1, sep=0.0):
    """City dots in the app's style (amber with a dark rim), and around
    `focus` (lat, lon) a set of fading amber rings. Like the app, a dot
    closer than `sep` pixels to one already drawn is skipped."""
    h, w, _ = canvas.shape
    pts = np.array(CITIES)
    sx, sy, sz = view.project(pts[:, 0], pts[:, 1])
    glow = Image.new("L", (w, h), 0)
    rim = Image.new("L", (w, h), 0)
    dot = Image.new("L", (w, h), 0)
    dg, drm, dd = ImageDraw.Draw(glow), ImageDraw.Draw(rim), ImageDraw.Draw(dot)
    drawn = []
    if focus is not None:
        fx, fy, _ = view.project(np.array(focus[0]), np.array(focus[1]))
        drawn.append((float(fx), float(fy)))
    for x, y, z in zip(sx, sy, sz):
        if z < 0.15 or any((x - a) ** 2 + (y - b) ** 2 < sep * sep for a, b in drawn):
            continue
        drawn.append((x, y))
        s = size * (0.55 + 0.45 * z)  # smaller towards the rim
        dg.ellipse([x - 3 * s, y - 3 * s, x + 3 * s, y + 3 * s], fill=int(110 * z))
        drm.ellipse([x - s * 1.45, y - s * 1.45, x + s * 1.45, y + s * 1.45], fill=190)
        dd.ellipse([x - s, y - s, x + s, y + s], fill=255)
    ring = Image.new("L", (w, h), 0)
    if focus is not None:
        fx, fy, fz = view.project(np.array(focus[0]), np.array(focus[1]))
        fx, fy = float(fx), float(fy)
        s = size * 1.25
        drm.ellipse([fx - s * 1.45, fy - s * 1.45, fx + s * 1.45, fy + s * 1.45], fill=190)
        dd.ellipse([fx - s, fy - s, fx + s, fy + s], fill=255)
        dg.ellipse([fx - 4 * s, fy - 4 * s, fx + 4 * s, fy + 4 * s], fill=150)
        dr = ImageDraw.Draw(ring)
        for k in range(rings):
            rr = s * 2.6 + k * ring_step
            a = int(230 * (1.0 - k / rings) ** 1.4)
            dr.ellipse([fx - rr, fy - rr, fx + rr, fy + rr], outline=a, width=ring_w)
    glow = glow.filter(ImageFilter.GaussianBlur(size * 2.5))
    add(canvas, np.asarray(glow, np.float32) / 255.0 * 0.6, ACCENT)
    blend(canvas, np.asarray(ring, np.float32) / 255.0 * 0.85, ACCENT)
    blend(canvas, np.asarray(rim, np.float32) / 255.0 * 0.75, BG)
    blend(canvas, np.asarray(dot, np.float32) / 255.0, ACCENT)


def draw_crosshair(canvas, cx, cy, gap, arm, width, colour=TEXT, alpha=0.82):
    h, w, _ = canvas.shape
    img = Image.new("L", (w, h), 0)
    dr = ImageDraw.Draw(img)
    for (x0, y0, x1, y1) in ((cx - arm, cy, cx - gap, cy), (cx + gap, cy, cx + arm, cy),
                             (cx, cy - arm, cx, cy - gap), (cx, cy + gap, cx, cy + arm)):
        dr.line([x0, y0, x1, y1], fill=255, width=width)
    blend(canvas, np.asarray(img, np.float32) / 255.0 * alpha, colour)


# ---------------------------------------------------------- backgrounds

def new_canvas(w, h, colour=BG):
    c = np.empty((h * SS, w * SS, 3), np.float32)
    c[:] = colour
    return c


def vertical_gradient(canvas, top, bottom, power=1.0):
    h = canvas.shape[0]
    t = (np.linspace(0.0, 1.0, h, dtype=np.float32) ** power)[:, None, None]
    canvas[:] = np.float32(top) * (1 - t) + np.float32(bottom) * t


def radial_vignette(canvas, cx, cy, r, colour, strength):
    h, w, _ = canvas.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2) / r
    add(canvas, np.clip(1.0 - d, 0.0, 1.0) ** 2 * strength, colour)


def stars(canvas, count, seed, max_y, avoid=None):
    rnd = random.Random(seed)
    h, w, _ = canvas.shape
    img = Image.new("L", (w, h), 0)
    dr = ImageDraw.Draw(img)
    for _ in range(count):
        x, y = rnd.uniform(0, w), rnd.uniform(0, max_y)
        if avoid and avoid(x, y):
            continue
        s = rnd.choice((0.5, 0.6, 0.8, 1.1)) * SS
        dr.ellipse([x - s, y - s, x + s, y + s], fill=rnd.randint(60, 170))
    blend(canvas, np.asarray(img, np.float32) / 255.0, (200, 215, 235))


def to_image(canvas, w, h):
    img = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGB")
    return img.resize((w, h), Image.Resampling.LANCZOS)


def font(path, size):
    return ImageFont.truetype(path, size)


# ---------------------------------------------------------------- images

# The place under the crosshair and rings in all three images: Swansea,
# where DX Garden was made.
FOCUS = (51.62, -3.94)


def render():
    """{"icon0.png": image, "bg.png": image, "startup.png": image}, RGB."""
    land, lines = load_land(), load_lines()
    return {"icon0.png": make_icon(land, lines), "bg.png": make_bg(land, lines),
            "startup.png": make_startup(land, lines)}


def make_icon(land, lines):
    # The home screen shows the icon in a circle; keep the globe and its
    # glow inside it.
    c = new_canvas(128, 128)
    radial_vignette(c, 64 * SS, 64 * SS, 66 * SS, (20, 34, 52), 1.0)
    v = View(64 * SS, 64 * SS, 50 * SS, FOCUS[0] - 8, FOCUS[1] - 6)
    draw_globe(c, v, land, lines, coast_w=SS, border_w=0, grid_w=1, glow=0.2, coast_a=0.5)
    draw_places(c, v, 1.5 * SS, focus=FOCUS, rings=3, ring_step=4.5 * SS, ring_w=SS, sep=11 * SS)
    fx, fy, _ = v.project(np.array(FOCUS[0]), np.array(FOCUS[1]))
    draw_crosshair(c, float(fx), float(fy), 9 * SS, 18 * SS, 2 * SS)
    return to_image(c, 128, 128)


def make_bg(land, lines):
    W, H = 840, 500
    c = new_canvas(W, H)
    vertical_gradient(c, BG, SKY_LOW, power=1.6)
    # A big globe rising from the bottom, seen from orbit, with the focus
    # place (Swansea) on the horizon left of the gate, which covers the
    # middle of the LiveArea. Swansea is on Europe's western edge, so with
    # it on the left the horizon to its right is Europe, not the Atlantic
    # (00.27 had it on the right). The view is solved so Swansea lands at
    # about (176, 364).
    v = View(370 * SS, 1120 * SS, 790 * SS, -26.5, 19.25)

    def under_globe(x, y):
        return (x - v.cx) ** 2 + (y - v.cy) ** 2 < (v.r * 1.08) ** 2
    stars(c, 170, seed=7, max_y=H * SS, avoid=under_globe)
    draw_globe(c, v, land, lines, coast_w=SS, border_w=SS, grid_w=SS, glow=0.11,
               coast_a=0.75, light=0.82)
    draw_places(c, v, 3.2 * SS, focus=FOCUS, rings=6, ring_step=15 * SS,
                ring_w=2 * SS, sep=14 * SS)
    # No title: the gate sits over the upper middle and covered it (00.25
    # on hardware), and the gate already carries the name.
    return to_image(c, W, H)


def make_startup(land, lines):
    W, H = 280, 158
    c = new_canvas(W, H)
    vertical_gradient(c, (21, 27, 36), BG)
    radial_vignette(c, 66 * SS, 79 * SS, 110 * SS, (22, 40, 62), 1.0)
    v = View(66 * SS, 79 * SS, 56 * SS, FOCUS[0] - 8, FOCUS[1] - 6)
    draw_globe(c, v, land, lines, coast_w=SS, border_w=0, grid_w=1, glow=0.16, coast_a=0.55)
    draw_places(c, v, 1.6 * SS, focus=FOCUS, rings=3, ring_step=5 * SS, ring_w=SS, sep=11 * SS)
    fx, fy, _ = v.project(np.array(FOCUS[0]), np.array(FOCUS[1]))
    draw_crosshair(c, float(fx), float(fy), 10 * SS, 21 * SS, 2 * SS)
    img = to_image(c, W, H)

    d = ImageDraw.Draw(img)
    # Kept high: the Vita draws its Start button over the bottom of the gate.
    d.text((136, 22), "DX", font=font(FONT_BOLD, 30), fill=TEXT)
    d.text((136, 52), "Garden", font=font(FONT_BOLD, 30), fill=TEXT)
    d.rectangle([138, 94, 138 + 26, 96], fill=ACCENT)
    # A thin frame, as the gate sits on the LiveArea background.
    d.rectangle([0, 0, W - 1, H - 1], outline=(52, 62, 78))
    return img
