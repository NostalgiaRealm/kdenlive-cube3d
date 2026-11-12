#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Creates the labelled clips used by tests/melt_tests.sh.

    make_test_media.py <dir>

Writes PNG frame sequences to <dir>/frames/<name>/NNN.png (the shell script
encodes them with the ffmpeg that ships with Kdenlive) and <dir>/alpha.png.

Every clip has its own colour, a big letter that is not mirror-symmetric, a red
dot in the top-left corner, a frame counter top-right and a bar that moves
along the bottom, so that orientation, identity and timing can all be read off
a rendered frame.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = ImageFont.load_default()
COLOURS = {
    "A": (230, 70, 70),
    "B": (70, 200, 90),
    "C": (80, 110, 240),
    "D": (230, 200, 60),
    "E": (200, 90, 220),
    "V": (70, 210, 220),
}


def big_text(txt, scale):
    w, h = FONT.getbbox(txt)[2:]
    im = Image.new("L", (w + 2, h + 2), 0)
    ImageDraw.Draw(im).text((1, 1), txt, fill=255, font=FONT)
    return im.resize((im.width * scale, im.height * scale), Image.NEAREST)


def clip(out, name, size, frames):
    W, H = size
    col = COLOURS[name]
    base = Image.new("RGB", size)
    px = base.load()
    for y in range(H):
        for x in range(W):
            fx, fy = x / (W - 1), y / (H - 1)
            px[x, y] = (int(col[0] * (0.5 + 0.5 * fx)), int(col[1] * (0.5 + 0.5 * fy)),
                        int(col[2] * (0.5 + 0.5 * (1 - fx))))
    d = ImageDraw.Draw(base)
    d.rectangle([0, 0, W - 1, H - 1], outline=(255, 255, 255), width=max(4, H // 90))
    d.ellipse([H // 20, H // 20, H // 20 + H // 12, H // 20 + H // 12], fill=(255, 0, 0))
    letter = big_text(name, H // 22)
    base.paste((255, 255, 255), ((W - letter.width) // 2, (H - letter.height) // 2), letter)
    target = out / "frames" / name
    target.mkdir(parents=True, exist_ok=True)
    for n in range(frames):
        im = base.copy()
        d = ImageDraw.Draw(im)
        num = big_text(f"{n:03d}", H // 90)
        im.paste((255, 255, 0), (W - num.width - H // 20, H // 20), num)
        x = int((W - 40) * n / (frames - 1))
        d.rectangle([x, H - H // 12, x + 40, H - H // 24], fill=(255, 255, 255))
        im.save(target / f"{n:03d}.png", compress_level=1)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name in "ABCDE":
        clip(out, name, (1280, 720), 100)
    clip(out, "V", (720, 1280), 120)  # portrait, and encoded at another frame rate
    # a still with transparency: opaque block on a fully transparent field whose
    # hidden colour is red, so careless filtering would show a red fringe
    im = Image.new("RGBA", (800, 600), (255, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([200, 150, 600, 450], fill=(60, 220, 60, 255))
    t = big_text("PNG", 12)
    im.paste((255, 255, 255, 255), ((800 - t.width) // 2, (600 - t.height) // 2), t)
    im.save(out / "alpha.png")
    # stills whose proportions differ from 4:3, the shape MLT falls back to:
    # a 16:9 one and a square one, each with a 20 px magenta frame at its edge
    for name, size in (("wide.png", (1600, 900)), ("square.png", (600, 600))):
        im = Image.new("RGB", size, (40, 160, 200))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, size[0] - 1, size[1] - 1], outline=(255, 0, 255), width=20)
        d.ellipse([60, 60, 160, 160], fill=(255, 255, 0))  # a circle must stay a circle
        im.save(out / name)


if __name__ == "__main__":
    main()
