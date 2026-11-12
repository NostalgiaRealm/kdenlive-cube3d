#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert the raw RGBA dumps written by render_test into PNGs.

    topng.py <dir> [--checker]

Files are named <name>_<w>x<h>.rgba. With --checker the picture is shown over
a grey checkerboard so transparent areas are recognisable.
"""
import re
import sys
from pathlib import Path

from PIL import Image


def checkerboard(w, h, cell=16):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (150, 150, 150) if ((x // cell) + (y // cell)) % 2 else (105, 105, 105)
    return img


def main():
    d = Path(sys.argv[1])
    checker = "--checker" in sys.argv
    for f in sorted(d.glob("*.rgba")):
        m = re.search(r"_(\d+)x(\d+)\.rgba$", f.name)
        if not m:
            continue
        w, h = int(m.group(1)), int(m.group(2))
        img = Image.frombytes("RGBA", (w, h), f.read_bytes())
        if checker:
            bg = checkerboard(w, h)
            bg.paste(img, (0, 0), img)
            img = bg
        img.save(f.with_suffix(".png"))
        print(f.with_suffix(".png"))


if __name__ == "__main__":
    main()
