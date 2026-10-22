#!/usr/bin/env python3
"""Fetches the extra Noto fonts for non-Latin scripts into assets/fonts/.

Noto Sans (Latin, Greek, Cyrillic) is already in assets/fonts/. This adds
regular-weight fonts for Arabic, Hebrew, Thai, Georgian and Ethiopic as
published, plus a subset of Noto Sans CJK SC holding the common
characters:

  - Chinese: GB 2312 (6,763 simplified) and Big5 level 1 (5,401 common
    traditional),
  - Japanese: JIS X 0208 kanji, all hiragana and katakana,
  - Korean: the 2,350 Hangul syllables of KS X 1001, and compatibility
    jamo,
  - CJK punctuation, full-width forms and Latin.

All are SIL Open Font License 1.1 (assets/fonts/OFL.txt). Downloads go to
build/fonts-src/ (git-ignored). Needs fontTools (pip install fonttools).

Usage: python3 tools/make_fonts.py
"""
import os
import sys
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build", "fonts-src")
OUT = os.path.join(ROOT, "assets", "fonts")

NOTO = "https://github.com/notofonts/notofonts.github.io/raw/main/fonts/%s/hinted/ttf/%s-Regular.ttf"
CJK_URL = ("https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/"
           "NotoSansCJKsc-Regular.otf")
SCRIPTS = ["NotoSansArabic", "NotoSansHebrew", "NotoSansThai", "NotoSansGeorgian",
           "NotoSansEthiopic"]


def fetch(url, name):
    path = os.path.join(SRC, name)
    if not os.path.exists(path):
        os.makedirs(SRC, exist_ok=True)
        print("downloading", name)
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return path


def decodable(codec, lead, trail):
    """Characters a double-byte encoding can represent."""
    out = set()
    for a in lead:
        for b in trail:
            try:
                ch = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def cjk_codepoints():
    cps = set(range(0x20, 0x7F))                    # ASCII
    cps |= set(range(0x3000, 0x3040))               # CJK symbols, punctuation
    cps |= set(range(0x3040, 0x3100))               # hiragana, katakana
    cps |= set(range(0x31F0, 0x3200))               # katakana extensions
    cps |= set(range(0x3130, 0x3190))               # Hangul compatibility jamo
    cps |= set(range(0xFF00, 0xFFF0))               # full-width forms
    cps |= decodable("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF))
    cps |= decodable("euc_jp", range(0xB0, 0xF5), range(0xA1, 0xFF))
    cps |= decodable("euc_kr", range(0xB0, 0xC9), range(0xA1, 0xFF))
    cps |= decodable("big5", range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
    return cps


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in SCRIPTS:
        src = fetch(NOTO % (name, name), name + "-Regular.ttf")
        dst = os.path.join(OUT, name + "-Regular.ttf")
        with open(src, "rb") as f, open(dst, "wb") as g:
            g.write(f.read())
        print("wrote", dst, os.path.getsize(dst), "bytes")

    src = fetch(CJK_URL, "NotoSansCJKsc-Regular.otf")
    font = TTFont(src)
    cmap = font.getBestCmap()
    wanted = cjk_codepoints()
    have = sorted(c for c in wanted if c in cmap)
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.glyph_names = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=have)
    sub.subset(font)
    dst = os.path.join(OUT, "NotoSansCJKsc-Subset.otf")
    font.save(dst)
    print("wrote %s: %d of %d wanted characters, %d bytes"
          % (dst, len(have), len(wanted), os.path.getsize(dst)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
