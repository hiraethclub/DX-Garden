#!/usr/bin/env python3
"""Draw DX Garden's LiveArea images (our own artwork, no third-party assets).

Writes 8-bit palette PNGs, which the Vita LiveArea expects:
  sce_sys/icon0.png                       128x128
  sce_sys/livearea/contents/bg.png        840x500
  sce_sys/livearea/contents/startup.png   280x158

Needs Pillow. Run from the repo root: python3 tools/make_livearea.py
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

BG = (14, 17, 22)
PANEL = (24, 29, 38)
LINE = (52, 62, 78)
AMBER = (242, 165, 65)
TEXT = (232, 234, 237)


def globe(draw, cx, cy, r, width):
    """Wireframe globe: outline, three meridians, three parallels, crosshair."""
    w = max(1, width)
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=PANEL, outline=LINE, width=w)
    for k in (0.35, 0.75):
        rx = r * k
        draw.ellipse([cx - rx, cy - r, cx + rx, cy + r], outline=LINE, width=w)
    draw.line([cx, cy - r, cx, cy + r], fill=LINE, width=w)
    for lat in (-50, 0, 50):
        y = cy + r * math.sin(math.radians(lat))
        half = r * math.cos(math.radians(lat))
        draw.line([cx - half, y, cx + half, y], fill=LINE, width=w)
    # A few "places" as amber dots.
    for (dx, dy, s) in ((-0.42, -0.30, 1.0), (0.18, -0.48, 0.8), (0.50, 0.10, 0.9),
                        (-0.10, 0.38, 0.7), (-0.60, 0.22, 0.6)):
        px, py, pr = cx + dx * r, cy + dy * r, max(2, r * 0.05 * s)
        draw.ellipse([px - pr, py - pr, px + pr, py + pr], fill=AMBER)
    # Crosshair in the centre.
    g, arm = r * 0.08, r * 0.22
    cw = max(2, w * 2)
    for (x0, y0, x1, y1) in ((cx - arm, cy, cx - g, cy), (cx + g, cy, cx + arm, cy),
                             (cx, cy - arm, cx, cy - g), (cx, cy + g, cx, cy + arm)):
        draw.line([x0, y0, x1, y1], fill=AMBER, width=cw)


def font(size):
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def save(img, path):
    """Save as an 8-bit palette PNG.

    The Vita refuses to install a VPK whose LiveArea PNGs use a smaller bit
    depth (VitaShell error 0x8010113D). Pillow picks 1/2/4-bit on its own when
    the palette is short, so pad the palette to 256 entries to force 8-bit.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    q = img.convert("RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    pal = q.getpalette()[: 256 * 3]
    q.putpalette(pal + [0] * (256 * 3 - len(pal)))
    q.save(path, bits=8)
    with open(path, "rb") as f:
        head = f.read(26)
    depth, ctype = head[24], head[25]
    if depth != 8 or ctype != 3:
        raise SystemExit(f"{path}: expected 8-bit palette PNG, got depth {depth} type {ctype}")
    print("wrote", path)


def main():
    icon = Image.new("RGB", (128, 128), BG)
    globe(ImageDraw.Draw(icon), 64, 64, 54, 2)
    save(icon, "sce_sys/icon0.png")

    bg = Image.new("RGB", (840, 500), BG)
    d = ImageDraw.Draw(bg)
    globe(d, 655, 250, 180, 3)
    d.text((60, 190), "DX Garden", font=font(64), fill=TEXT)
    d.text((64, 275), "Radio from your own station list", font=font(21), fill=AMBER)
    save(bg, "sce_sys/livearea/contents/bg.png")

    st = Image.new("RGB", (280, 158), BG)
    d = ImageDraw.Draw(st)
    globe(d, 60, 79, 50, 2)
    d.text((124, 62), "DX Garden", font=font(26), fill=TEXT)
    save(st, "sce_sys/livearea/contents/startup.png")


if __name__ == "__main__":
    main()
