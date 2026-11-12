#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspects the pictures rendered by tests/melt_tests.sh.

    check_melt.py <build/test-out/melt>

Prints one line per check and exits with the number of failures. Also writes
contact sheets (sheet_*.png) for a human to look at.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

OUT = Path(sys.argv[1])
failures = 0


def check(ok, text):
    global failures
    print(("ok    " if ok else "FAIL  ") + text)
    if not ok:
        failures += 1


def frames(name):
    return sorted((OUT / name).glob("*.png"))


def load(name, index, mode="RGB"):
    """index counts from 0; melt numbers its files from 1."""
    f = OUT / name / f"{index + 1:03d}.png"
    if not f.exists():
        return None
    return np.asarray(Image.open(f).convert(mode)).astype(np.int32)


def px(img, fx, fy):
    """Mean colour of a small patch around a relative position."""
    h, w = img.shape[:2]
    x, y = int(fx * (w - 1)), int(fy * (h - 1))
    return img[max(0, y - 3):y + 4, max(0, x - 3):x + 4].reshape(-1, img.shape[2]).mean(axis=0)


def near(colour, target, tol=40):
    return all(abs(float(c) - t) <= tol for c, t in zip(colour, target))


def which_clip(colour):
    """Test clips: A is red, B is green (sampled away from the letter)."""
    r, g, b = [float(c) for c in colour[:3]]
    if r > g + 40 and r > b + 20:
        return "A"
    if g > r + 40:
        return "B"
    if max(r, g, b) < 30:
        return "black"
    return "?"


def sheet(name, picks, cols, out):
    tw, th = 384, 216
    rows = (len(picks) + cols - 1) // cols
    sh = Image.new("RGB", (tw * cols, (th + 16) * rows), (30, 30, 30))
    dr = ImageDraw.Draw(sh)
    for i, n in enumerate(picks):
        f = OUT / name / f"{n + 1:03d}.png"
        if not f.exists():
            continue
        im = Image.open(f)
        if im.mode == "RGBA":
            bg = Image.new("RGB", im.size, (110, 110, 110))
            bgd = ImageDraw.Draw(bg)
            for yy in range(0, im.size[1], 32):
                for xx in range((yy // 32 % 2) * 32, im.size[0], 64):
                    bgd.rectangle([xx, yy, xx + 31, yy + 31], fill=(150, 150, 150))
            bg.paste(im, (0, 0), im)
            im = bg
        im = im.convert("RGB").resize((tw, th), Image.LANCZOS)
        x, y = (i % cols) * tw, (i // cols) * (th + 16)
        sh.paste(im, (x, y + 16))
        dr.text((x + 4, y + 2), f"{name} #{n}", fill=(255, 255, 255))
    sh.save(OUT / out)


# -- nothing crashed -----------------------------------------------------------
crashed = (OUT / "crashed").read_text().split() if (OUT / "crashed").exists() else []
check(not crashed, "melt finished every render" + (f" (failed: {', '.join(crashed)})" if crashed else ""))

# -- 1 transparency ------------------------------------------------------------
img = load("alpha", 0, "RGBA")
if img is None:
    check(False, "alpha: no picture")
else:
    a = img[:, :, 3]
    check(a[4, 4] == 0 and a[-5, -5] == 0, f"alpha: outside the box is transparent (corner alpha {a[4, 4]})")
    check(a[a.shape[0] // 2, a.shape[1] // 2] == 255, "alpha: the box itself is opaque")
    partial = np.count_nonzero((a > 0) & (a < 255))
    check(0 < partial < a.size * 0.01, f"alpha: edges are antialiased ({partial} partly covered pixels)")
    sheet("alpha", [0], 1, "sheet_alpha.png")

# -- 2 over a lower track ------------------------------------------------------
img = load("over", 0)
if img is None:
    check(False, "over: no picture")
else:
    check(near(px(img, 0.03, 0.05), (0x20, 0x40, 0xC0), 12), f"over: lower track shows around the box {px(img, 0.03, 0.05).round()}")
    check(which_clip(px(img, 0.42, 0.62)) == "A", "over: the box shows the clip")

# -- 3 one turn, single vs multi threaded ----------------------------------------
f1, f8 = frames("turn_rt1"), frames("turn_rt8")
check(len(f1) == 60 and len(f8) == 60, f"turn: 60 frames each ({len(f1)}, {len(f8)})")
worst = 0
for i in range(min(len(f1), len(f8))):
    a, b = load("turn_rt1", i), load("turn_rt8", i)
    worst = max(worst, int(np.abs(a - b).max()))
check(worst == 0, f"turn: 1 thread and 8 threads give identical frames (largest difference {worst})")
sheet("turn_rt8", [0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55], 4, "sheet_turn.png")
# ... and the faces show the real clips that were assigned to them. Sampled on
# the face that is in front, left of the letter, where a clip's colour is its
# base colour times (0.65, 0.75, 0.85).
CLIPS = {"A": (230, 70, 70), "B": (70, 200, 90), "C": (80, 110, 240), "D": (230, 200, 60), "E": (200, 90, 220)}
for frame, clip, face in ((0, "A", "front"), (15, "D", "left"), (30, "C", "back"), (44, "B", "right")):
    img = load("turn_rt1", frame)
    if img is None:
        check(False, f"turn: frame {frame} missing")
        continue
    want = tuple(round(c * f) for c, f in zip(CLIPS[clip], (0.65, 0.75, 0.85)))
    got = px(img, 0.38, 0.5)
    check(near(got, want, 38), f"turn: frame {frame} shows clip {clip} on the {face} face "
                               f"(expected about {want}, got {tuple(int(v) for v in got)})")

# -- 4 resolution independence ---------------------------------------------------
full, small = load("res_full", 1), load("res_small", 1)
if full is None or small is None:
    check(False, "resolution: pictures missing")
else:
    check(small.shape[1] == 640 and small.shape[0] == 360, f"resolution: small render is 640x360 ({small.shape[1]}x{small.shape[0]})")
    down = np.asarray(Image.fromarray(full.astype(np.uint8)).resize((small.shape[1], small.shape[0]), Image.BOX)).astype(np.int32)
    diff = np.abs(down - small)
    check(diff.mean() < 2.5, f"resolution: quarter-size render matches the full-size one (mean difference {diff.mean():.2f} of 255)")
    # the silhouette must be in the same place: compare where the picture is not black
    m1, m2 = down.sum(axis=2) > 24, small.sum(axis=2) > 24
    moved = np.count_nonzero(m1 != m2) / m1.size
    check(moved < 0.006, f"resolution: box outline in the same place ({moved * 100:.2f}% of pixels differ)")

# -- 4b / 5 every face shows its picture -----------------------------------------
EXPECT = [("front", (255, 0, 0)), ("right", (0, 255, 0)), ("back", (0, 0, 255)),
          ("left", (255, 255, 0)), ("top", (255, 0, 255)), ("bottom", (0, 255, 255))]
for test, what in (("ident", "file"), ("grid_ident", "grid cell")):
    for i, (face, colour) in enumerate(EXPECT):
        img = load(test, i)
        if img is None:
            check(False, f"{test}: frame {i} missing")
            continue
        c = px(img, 0.5, 0.5)
        check(near(c, colour, 30), f"{test}: {face} face shows its {what} (expected {colour}, got {tuple(int(v) for v in c)})")
        check(near(px(img, 0.03, 0.05), (0x20, 0x40, 0x60), 10), f"{test}: background colour around the box, frame {i}")
sheet("grid_source", [0], 1, "sheet_grid_source.png")
sheet("grid_cube", [0], 1, "sheet_grid_cube.png")

# -- 6 / 7 / 8 transitions --------------------------------------------------------
P_TL, P_L, P_R, P_T, P_B = (0.25, 0.25), (0.2, 0.3), (0.8, 0.3), (0.3, 0.22), (0.3, 0.78)


def trans(name, first, last, mid=None):
    n = len(frames(name))
    check(n == 25, f"{name}: 25 frames ({n})")
    a, z = load(name, 0), load(name, n - 1)
    if a is None or z is None:
        return
    check(which_clip(px(a, *P_TL)) == first, f"{name}: starts on clip {first} (got {which_clip(px(a, *P_TL))})")
    check(which_clip(px(z, *P_TL)) == last, f"{name}: ends on clip {last} (got {which_clip(px(z, *P_TL))})")
    if mid:
        m = load(name, n // 2)
        for pos, want in mid:
            got = which_clip(px(m, *pos))
            check(got == want, f"{name}: half way, {want} at {pos} (got {got})")
    sheet(name, [0, 3, 6, 9, 12, 15, 18, 21, 24], 9, f"sheet_{name}.png")


trans("trans", "A", "B", [(P_L, "A"), (P_R, "B")])
trans("trans_rev", "B", "A", [(P_L, "B"), (P_R, "A")])
trans("turn_right", "A", "B", [(P_L, "B"), (P_R, "A")])
trans("tip_up", "A", "B", [(P_T, "A"), (P_B, "B")])
trans("tip_down", "A", "B", [(P_T, "B"), (P_B, "A")])
trans("flip", "A", "B", [((0.1, 0.5), "black"), ((0.9, 0.5), "black")])

# the very first and last frame of a transition must be the untouched clips
for name in ("trans", "tip_up"):
    a = load(name, 0)
    if a is not None:
        edge = np.concatenate([a[0].reshape(-1, 3), a[-1].reshape(-1, 3), a[:, 0].reshape(-1, 3), a[:, -1].reshape(-1, 3)])
        check(edge.min() > 180, f"{name}: first frame fills the whole frame (clip's white border reaches every edge)")

# -- 9 missing files ---------------------------------------------------------------
img = load("missing", 0)
if img is None:
    check(False, "missing: no picture")
else:
    grey = (np.abs(img[:, :, 0] - img[:, :, 1]) < 6) & (np.abs(img[:, :, 1] - img[:, :, 2]) < 6) & (img[:, :, 0] > 25) & (img[:, :, 0] < 90)
    check(np.count_nonzero(grey) > 3000, f"missing: unreadable files show the placeholder ({np.count_nonzero(grey)} grey pixels)")
    sheet("missing", [0], 1, "sheet_missing.png")

# -- 10 constant spin ----------------------------------------------------------------
a, b, ref = load("spin", 0), load("spin", 20), load("spin_ref", 0)
if a is None or b is None or ref is None:
    check(False, "spin: pictures missing")
else:
    check(np.abs(a - b).mean() > 3, f"spin: the box turns without keyframes (mean change {np.abs(a - b).mean():.1f})")
    d = np.abs(b - ref).mean()
    check(d < 1.0, f"spin: 0.8 s at 90 degrees per second equals Rotate Y = 72 (mean difference {d:.2f})")
    sheet("spin", [0, 5, 10, 15, 20, 24], 6, "sheet_spin.png")

# -- 11 stills ----------------------------------------------------------------------
img, ref = load("still_wide", 0), load("still_wide_ref", 0)
if img is None or ref is None:
    check(False, "stills: pictures missing")
else:
    d = np.abs(img - ref).mean()
    check(d < 3.0, f"stills: a 16:9 picture fills a frame-shaped face exactly as MLT shows it on its own (mean difference {d:.2f})")
img = load("still_square", 0)
if img is None:
    check(False, "stills: square picture missing")
else:
    h, w = img.shape[:2]
    lit = img.sum(axis=2) > 60
    ys, xs = np.nonzero(lit)
    ok = len(xs) > 0 and abs((xs.max() - xs.min() + 1) - h) <= 2 and abs((ys.max() - ys.min() + 1) - h) <= 2
    check(ok, "stills: a square picture covers the whole square face"
          + (f" ({xs.max() - xs.min() + 1}x{ys.max() - ys.min() + 1} of {h}x{h})" if len(xs) else " (nothing drawn)"))
    # its magenta frame must reach all four edges of the face: no bars, no cropping
    x0 = (w - h) // 2
    edges = [px(img, (x0 + 6) / w, 0.5), px(img, (x0 + h - 7) / w, 0.5), px(img, 0.5, 6 / h), px(img, 0.5, (h - 7) / h)]
    check(all(near(e, (255, 0, 255), 45) for e in edges),
          f"stills: its frame reaches every edge of the face ({[tuple(int(v) for v in e) for e in edges]})")
    sheet("still_square", [0], 1, "sheet_still_square.png")

# -- 12 face clip timing --------------------------------------------------------------
def same(a, b):
    return float(np.abs(a - b).mean())


f0, f10, f104 = load("facetime", 0), load("facetime", 10), load("facetime", 104)
r0, r4, r10 = load("facetime_ref", 0), load("facetime_ref", 4), load("facetime_ref", 10)
if any(v is None for v in (f0, f10, f104, r0, r4, r10)):
    check(False, "face clip timing: pictures missing")
else:
    check(same(f0, r0) < 2.5 and same(f0, r0) < same(f0, r4), f"face clip: starts on its first frame ({same(f0, r0):.2f} vs {same(f0, r4):.2f} for frame 4)")
    check(same(f10, r10) < 2.5 and same(f10, r10) < same(f10, r4), f"face clip: frame 10 after 10 frames ({same(f10, r10):.2f})")
    check(same(f104, r4) < 2.5 and same(f104, r4) < same(f104, r0), f"face clip: a 100-frame clip is back on frame 4 at frame 104 ({same(f104, r4):.2f} vs {same(f104, r0):.2f} for frame 0)")

# -- 13 saved projects ------------------------------------------------------------------
for name in ("effect", "transition"):
    text = (OUT / "saved" / f"{name}.mlt").read_text(encoding="utf-8") if (OUT / "saved" / f"{name}.mlt").exists() else ""
    check(">B.mp4<" in text and "test-media/B.mp4" not in text.split("<filter")[-1].split("<transition")[-1],
          f"saved {name}: the writer did store the face files as relative paths (so this test means something)")
    log = (OUT / f"saved_{name}.log").read_text(errors="replace") if (OUT / f"saved_{name}.log").exists() else "missing"
    check("cannot open face file" not in log and log != "missing", f"saved {name}: every face file opens after reloading")
# front, right, back, left, top, bottom = files 1-6 = clips B, C, D, E, V and the still
SAVED = [("B", (70, 200, 90)), ("C", (80, 110, 240)), ("D", (230, 200, 60)), ("E", (200, 90, 220))]
for name, count in (("saved_effect", 4), ("saved_transition", 4)):
    for i, (clip, base) in enumerate(SAVED[:count]):
        img = load(name, i)
        if img is None:
            check(False, f"{name}: frame {i} missing")
            continue
        want = tuple(round(c * f) for c, f in zip(base, (0.65, 0.75, 0.85)))
        fx = 0.5 - 0.356 * (0.5 if name == "saved_effect" else 0.6) * img.shape[0] / img.shape[1]
        got = px(img, fx, 0.5)
        check(near(got, want, 38), f"{name}: face {i + 1} shows clip {clip} (expected about {want}, got {tuple(int(v) for v in got)})")

# -- 14 project files as faces ------------------------------------------------------------
n = len(frames("projectface"))
check(n == 10, f"project file as a face: melt survives and renders ({n} of 10 frames)")
log = (OUT / "projectface.log").read_text(errors="replace") if (OUT / "projectface.log").exists() else ""
check("cannot be shown on a face" in log, "project file as a face: refused with a message")

# -- 15 bin zone on a face: frames 50..59 of the clip, looping ----------------------------
for k, src in ((0, 50), (9, 59), (10, 50), (23, 53)):
    a, b, c = load("zoneface", k), load("zoneface_ref", src), load("zoneface_ref", src - 3)
    if a is None or b is None:
        check(False, f"zone face: frame {k} missing")
    else:
        check(same(a, b) < 2.5 and same(a, b) < same(a, c), f"zone face: frame {k} shows clip frame {src} ({same(a, b):.2f})")

print(f"{'FAILED' if failures else 'all end-to-end checks passed'} ({failures} failed)")
sys.exit(failures)
