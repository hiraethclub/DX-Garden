#!/usr/bin/env python3
"""DX Garden's LiveArea images: placeholders to draw over, and installing
your finished art into sce_sys/ for the VPK.

The Vita LiveArea needs these, as 8-bit palette PNGs (256 colours at most):
  icon0.png     128x128  sce_sys/icon0.png                      home-screen bubble
  bg.png        840x500  sce_sys/livearea/contents/bg.png       LiveArea background
  startup.png   280x158  sce_sys/livearea/contents/startup.png  the gate you tap to start

  python3 tools/make_livearea.py placeholders
      Writes labelled templates (with a coordinate grid and guides) to
      art/livearea/ as normal RGB PNGs to draw over, and installs them, so a
      build shows where each image lands on the device.
  python3 tools/make_livearea.py install [DIR]
      Checks the sizes of DIR/icon0.png, DIR/bg.png and DIR/startup.png
      (default art/livearea), converts each to an 8-bit palette PNG and
      copies it into sce_sys/. Draw in full colour; the conversion reduces
      to 256 colours with dithering, so flat colours and clean edges convert
      best.

Needs Pillow. Run from the repo root.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_DIR = os.path.join(ROOT, "art", "livearea")

# name -> (width, height, installed path, what it is)
IMAGES = {
    "icon0.png": (128, 128, "sce_sys/icon0.png",
                  "Home-screen bubble. Shown cropped to a circle."),
    "bg.png": (840, 500, "sce_sys/livearea/contents/bg.png",
               "LiveArea background. The gate and the title sit on top."),
    "startup.png": (280, 158, "sce_sys/livearea/contents/startup.png",
                    "The gate: the image you tap to start the app."),
}

INK = (20, 20, 20)


def font(size, bold=True):
    names = ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf") \
        if bold else ("DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def save_palette(img, path):
    """Save as an 8-bit palette PNG.

    The Vita refuses to install a VPK whose LiveArea PNGs use a smaller bit
    depth (VitaShell error 0x8010113D). Pillow picks 1/2/4-bit on its own when
    the palette is short, so pad the palette to 256 entries to force 8-bit.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    q = img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                    dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette()[: 256 * 3]
    q.putpalette(pal + [0] * (256 * 3 - len(pal)))
    q.save(path, bits=8)
    with open(path, "rb") as f:
        head = f.read(26)
    depth, ctype = head[24], head[25]
    if depth != 8 or ctype != 3:
        raise SystemExit(f"{path}: expected 8-bit palette PNG, got depth {depth} type {ctype}")
    print("installed", os.path.relpath(path, ROOT))


def grid(d, w, h, step, label_every, colour):
    """Thin lines every `step` px, labelled with their coordinate."""
    small = font(10, bold=False)
    line = tuple(c * 3 // 4 for c in colour)  # a darker shade of the fill
    for x in range(0, w, step):
        d.line([x, 0, x, h], fill=line, width=1)
        if x and x % label_every == 0:
            d.text((x + 2, 2), str(x), font=small, fill=INK)
    for y in range(0, h, step):
        d.line([0, y, w, y], fill=line, width=1)
        if y and y % label_every == 0:
            d.text((2, y + 2), str(y), font=small, fill=INK)


def centred(d, w, y, text, f):
    tw = d.textlength(text, font=f)
    d.text(((w - tw) / 2, y), text, font=f, fill=INK)


def placeholder(name):
    w, h, _, what = IMAGES[name]
    colour = {"icon0.png": (120, 200, 190), "bg.png": (150, 180, 230),
              "startup.png": (240, 190, 110)}[name]
    img = Image.new("RGB", (w, h), colour)
    d = ImageDraw.Draw(img)
    if name == "icon0.png":
        # The bubble crops to this circle: keep anything important inside.
        grid(d, w, h, 16, 1000, colour)
        d.ellipse([0, 0, w - 1, h - 1], outline=INK, width=2)
        d.ellipse([12, 12, w - 13, h - 13], outline=(200, 40, 40), width=1)
        centred(d, w, 42, "icon0", font(18))
        centred(d, w, 66, "128x128", font(14, bold=False))
    else:
        step = 25 if name == "startup.png" else 50
        grid(d, w, h, step, step * 2, colour)
        # A 5% margin, as a guide for keeping text clear of the edges.
        mx, my = round(w * 0.05), round(h * 0.05)
        d.rectangle([mx, my, w - 1 - mx, h - 1 - my], outline=(200, 40, 40), width=1)
        d.rectangle([0, 0, w - 1, h - 1], outline=INK, width=3)
        big = font(40 if name == "bg.png" else 20)
        body = font(16 if name == "bg.png" else 10, bold=False)
        y = h / 2 - (60 if name == "bg.png" else 30)
        centred(d, w, y, f"{name}  {w}x{h}", big)
        centred(d, w, y + (56 if name == "bg.png" else 28), what, body)
        centred(d, w, y + (80 if name == "bg.png" else 42),
                "8-bit palette PNG, 256 colours at most", body)
    return img


def cmd_placeholders():
    os.makedirs(ART_DIR, exist_ok=True)
    for name, (_, _, dest, _) in IMAGES.items():
        img = placeholder(name)
        path = os.path.join(ART_DIR, name)
        img.save(path)
        print("wrote", os.path.relpath(path, ROOT))
        save_palette(img, os.path.join(ROOT, dest))


def cmd_install(src):
    bad = []
    for name, (w, h, dest, _) in IMAGES.items():
        path = os.path.join(src, name)
        if not os.path.exists(path):
            bad.append(f"{name}: missing from {src}")
            continue
        img = Image.open(path)
        if img.size != (w, h):
            bad.append(f"{name}: is {img.size[0]}x{img.size[1]}, needs to be {w}x{h}")
            continue
        if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
            print(f"note: {name} has transparency; flattened onto black")
            flat = Image.new("RGB", img.size, (0, 0, 0))
            flat.paste(img.convert("RGBA"), mask=img.convert("RGBA").split()[3])
            img = flat
        save_palette(img, os.path.join(ROOT, dest))
    if bad:
        raise SystemExit("not installed:\n  " + "\n  ".join(bad))


def main(argv):
    if len(argv) >= 1 and argv[0] == "placeholders":
        cmd_placeholders()
    elif len(argv) >= 1 and argv[0] == "install":
        cmd_install(argv[1] if len(argv) > 1 else ART_DIR)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
