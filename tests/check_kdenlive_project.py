#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the results of tests/kdenlive_probe.sh.

    check_kdenlive_project.py <build/probe>

Looks at the action list Kdenlive published, at the project file it autosaved
and at the frames melt rendered from that project.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

PR = Path(sys.argv[1])
failures = 0


def check(ok, text):
    global failures
    print(("ok    " if ok else "FAIL  ") + text)
    if not ok:
        failures += 1


# -- what Kdenlive lists -------------------------------------------------------
actions = set((PR / "actions.txt").read_text(encoding="utf-8").split("\n")) if (PR / "actions.txt").exists() else set()
for a in ("effect_frei0r.cube3d", "effect_cube3d_gridcell", "transition_3D Cube", "transition_3D Cube: turn left",
          "transition_3D Cube: turn right", "transition_3D Cube: tip up", "transition_3D Cube: tip down",
          "transition_3D Cube: flip card", "transition_3D Cube (upper clip first)"):
    check(a in actions, f"Kdenlive lists '{a}'")

# -- what Kdenlive wrote ---------------------------------------------------------
root = ET.parse(PR / "project.kdenlive").getroot()
check(root.get("LC_NUMERIC") == "C", f"project uses the C numeric locale ({root.get('LC_NUMERIC')})")
cubes = []
for f in root.iter("filter"):
    props = {p.get("name"): (p.text or "") for p in f.findall("property")}
    if props.get("mlt_service") == "frei0r.cube3d":
        cubes.append(props)
check(len(cubes) >= 1, f"project contains the cube filter ({len(cubes)} instance(s))")
# ... and it sits on a clip that is on the timeline, not only in the project bin
on_timeline = set()
for pl in root.findall("playlist"):
    if pl.get("id") != "main_bin":
        on_timeline.update(e.get("producer") for e in pl.findall("entry"))
with_cube = {el.get("id") for el in root if el.tag in ("producer", "chain")
             and any(any(p.get("name") == "mlt_service" and p.text == "frei0r.cube3d" for p in f.findall("property"))
                     for f in el.findall("filter"))}
with_cube |= {e.get("producer") for pl in root.findall("playlist") if pl.get("id") != "main_bin"
              for e in pl.findall("entry")
              if any(any(p.get("name") == "mlt_service" and p.text == "frei0r.cube3d" for p in f.findall("property"))
                     for f in e.findall("filter"))}
check(bool(on_timeline & with_cube), f"the effect is on a timeline clip (timeline clips: {sorted(on_timeline)}, with cube: {sorted(with_cube)})")
if cubes:
    # the instance on the timeline clip (Kdenlive adds bookkeeping properties there)
    p = max(cubes, key=len)

    def value(name):
        v = p.get(name)
        return None if v is None else v.split("=")[-1]

    expected = {
        "rot_x": "0", "rot_y": "0", "rot_z": "0", "pos_x": "0", "pos_y": "0", "pos_z": "0",
        "scale": "50", "depth": "100", "fov": "40", "face_opacity": "100", "shading": "35",
        "spin_x": "0", "spin_y": "0", "spin_z": "0", "shape": "0", "fit": "0", "rot_order": "0",
        "easing": "0", "keep_in_frame": "0", "show_inside": "1", "quality": "1", "background": "0",
        "bg_color": "0x000000ff", "face_front": "1", "face_right": "1", "face_back": "1",
        "face_left": "1", "face_top": "1", "face_bottom": "1", "face_color": "0x404040ff",
        "resource": "", "producer.resource": "", "filename": "", "av.file": "", "av.filename": "",
        "filter.resource": "",
    }
    wrong = {k: (value(k), v) for k, v in expected.items() if value(k) != v}
    check(not wrong, f"Kdenlive set all {len(expected)} parameters to the declared defaults"
          + (f" - differing (got, expected): {wrong}" if wrong else ""))
    check(p.get("kdenlive_id") == "frei0r.cube3d", "filter is tagged with the effect id")

# -- what melt made of it -----------------------------------------------------------
frames = sorted((PR / "out").glob("*.png"))
check(len(frames) == 5, f"melt rendered the project ({len(frames)} of 5 frames)")
if frames:
    img = np.asarray(Image.open(frames[2]).convert("RGB")).astype(np.int32)
    h, w = img.shape[:2]
    lit = img.sum(axis=2) > 40
    ys, xs = np.nonzero(lit)
    if len(xs) == 0:
        check(False, "rendered frame is black")
    else:
        x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
        # defaults: a cube half as tall as the frame, centred, seen head-on
        side = h / 2
        ok = (abs((x1 - x0 + 1) - side) <= 3 and abs((y1 - y0 + 1) - side) <= 3
              and abs((x0 + x1) / 2 - (w - 1) / 2) <= 2 and abs((y0 + y1) / 2 - (h - 1) / 2) <= 2)
        check(ok, f"the cube is where the defaults put it: {x1 - x0 + 1}x{y1 - y0 + 1} px at "
                  f"({x0},{y0}) in a {w}x{h} frame (expected a centred {side:.0f} px square)")
        c = img[h // 2 - h // 8, w // 2 - h // 8]
        check(c[0] > c[1] + 40, f"its front face shows the clip (colour {tuple(int(v) for v in c)})")
        check(not lit[5, 5] and not lit[h - 6, w - 6], "nothing is drawn around the cube")
    Image.open(frames[2]).convert("RGB").resize((640, 360)).save(PR / "kdenlive_render.png")

print(f"{'FAILED' if failures else 'Kdenlive probe passed'} ({failures} failed)")
sys.exit(failures)
