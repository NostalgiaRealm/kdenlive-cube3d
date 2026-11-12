#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes the Kdenlive descriptions (XML) for the cube3d frei0r plugins.

    gen_kdenlive_xml.py <output dir>

creates <output dir>/effects/*.xml and <output dir>/transitions/*.xml.

The files are generated because the effect, the transition presets and the
compositions share almost all of their parameters; only defaults and a few
labels differ. Parameter names and value ranges must match src/cube3d.c.

Things Kdenlive does that shaped these files:
  * It shows the parameters that cannot be keyframed first, in file order, and
    the keyframable ones in a block underneath. The files use that order too.
  * All keyframable parameters of one effect share their keyframe positions.
    A transition's move is two keyframes, so in the transitions every
    keyframable parameter starts with those two; what does not need to move
    is not keyframable there.
  * It knows only some of MLT's keyframe easings (none of the in-out ones), so
    moves are plain linear keyframes and the plugin does the easing itself.
  * A number row gets a slider that is far too coarse for wide ranges unless
    the row is "compact" (number field only).
"""
import sys
from pathlib import Path
from xml.sax.saxutils import escape

AUTHOR = "NostalgiaRealm"
N_CELLS = 6
# MLT property names for the file parameters, see the comment in src/cube3d.c
EFFECT_FILES = ["resource", "producer.resource", "filename", "av.file", "av.filename", "filter.resource"]
TRANSITION_FILES = ["resource", "producer.resource", "luma", "luma.resource", "composite.luma"]


def param(ptype, name, label, comment=None, body="", **attrs):
    a = "".join(f' {k}="{escape(str(v), {chr(34): "&quot;"})}"' for k, v in attrs.items())
    out = [f'    <parameter type="{ptype}" name="{name}"{a}>', f"        <name>{escape(label)}</name>"]
    if comment:
        out.append(f"        <comment><![CDATA[{comment}]]></comment>")
    if body:
        out.append(body)
    out.append("    </parameter>")
    return "\n".join(out)


def plist(name, label, default, options, comment=None):
    """options: list of (value, text); text must not contain commas."""
    for _, text in options:
        assert "," not in text, text
    assert str(default) in [str(v) for v, _ in options], (name, default)
    body = "        <paramlistdisplay>" + ",".join(escape(t) for _, t in options) + "</paramlistdisplay>"
    return param("list", name, label, comment, body, default=default,
                 paramlist=";".join(str(v) for v, _ in options))


def number(name, label, comment, value, lo, hi, suffix, animated, two_keyframes, first=False, wide=False):
    """A number row. animated: keyframable. two_keyframes: start and end of a transition."""
    attrs = dict(min=lo, max=hi, decimals=1)
    if suffix:
        attrs["suffix"] = suffix
    if wide:
        attrs["compact"] = 1  # the slider would move in steps of 1% of the range
    if not animated:
        return param("constant", name, label, comment, default=value, **attrs)
    start, end = value if isinstance(value, tuple) else (value, value)
    default = f"0={start};%out={end}" if two_keyframes else start
    if not first:
        attrs["notintimeline"] = 1
    else:
        attrs.update(visualmin=-360, visualmax=360)
    return param("animated", name, label, comment, default=default, **attrs)


def parameters(kind, d):
    """kind: 'effect' or 'transition'. d: defaults that differ between the variants."""
    two = kind == "transition"
    files = TRANSITION_FILES if two else EFFECT_FILES
    a, b = ("clip A", "clip B") if two else ("this clip", None)
    p = []

    # ---- rows that are not keyframed, in the order Kdenlive shows them ------
    p.append(plist("shape", "Shape", d["shape"],
                   [(0, "Cube (square faces)"),
                    (1, "Frame-shaped - for turning left/right"),
                    (2, "Frame-shaped - for tipping up/down")],
                   "A cube has six square faces. The frame-shaped boxes have a front the shape of your video "
                   "and are as deep as needed for the next face to fill the frame after a quarter turn."))
    p.append(plist("fit", "Picture on face", 0,
                   [(0, "Fill (crop)"), (1, "Fit (whole picture)"), (2, "Stretch")],
                   "What to do when a picture and a face have different proportions."))

    src = [(0, "Empty")]
    src += [(1, "Clip A (from)"), (2, "Clip B (to)")] if two else [(1, "This clip")]
    src += [(3 + i, f"File {i + 1}") for i in range(len(files))]
    src += [(9 + i, f"Grid cell {i + 1} of {a}") for i in range(N_CELLS)]
    src += [(15, "Face colour")]
    hint = (f"What this face shows. Files are chosen further down. Grid cells read {a} as 3 x 2 pictures: "
            "cells 1-3 are the top row, 4-6 the bottom row.")
    for key, label in (("front", "Front face"), ("right", "Right face"), ("back", "Back face"),
                       ("left", "Left face"), ("top", "Top face"), ("bottom", "Bottom face")):
        p.append(plist(f"face_{key}", label, d[f"face_{key}"], src, hint))
    p.append(param("fixedcolor", "face_color", "Face colour", "For faces set to 'Face colour'.",
                   default="0x404040ff"))
    when = ("It plays from its beginning when the transition starts" if two else
            "It runs on the clip's own time: it starts at the beginning of the clip's source, "
            "so on a clip whose start was trimmed it is already part-way in")
    for i, name in enumerate(files):
        p.append(param("url", name, f"File {i + 1}",
                       f"A video or image file (not a project file), or a .c3dface link to a project-bin clip or zone "
                       f"made with the cube3d-faces tool. {when}, and it loops. No sound."))

    bg = [(0, "Transparent"), (1, "Background colour")]
    bg += [(2, "Clip A (from)"), (3, "Clip B (to)")] if two else [(2, "This clip")]
    p.append(plist("background", "Background", d["background"], bg, "What is shown around the box."))
    p.append(param("fixedcolor", "bg_color", "Background colour", default="0x000000ff"))
    p.append(number("keep_in_frame", "Keep inside frame",
                    "At 100 the box is shrunk just enough, moment by moment, to stay completely visible.",
                    d["keep"], 0, 100, "%", False, False))
    p.append(plist("easing", "Soften turns", d["easing"],
                   [(0, "Off"), (1, "At every quarter turn"), (2, "At every half turn")],
                   "Makes the keyframed rotation start and stop gently at every 90 (or 180) degrees, "
                   "so a plain two-keyframe quarter turn does not look mechanical. Leave it off for a steady spin."))
    p.append(plist("rot_order", "Rotation order", 0,
                   [(0, "Turntable: turn then tip"), (1, "Tumble: tip then turn")],
                   "Turntable: Rotate Y spins the box about its own upright axis and Rotate X then tilts the whole "
                   "thing. Tumble: the other way round."))
    if two:
        # not keyframable in a transition: see the note at the top of this file
        p.append(number("depth", "Depth", "Depth of the box. 0 makes a flat card with a front and a back.",
                        d["depth"], 0, 400, "%", False, False, wide=True))
        p.append(number("fov", "Perspective",
                        "Field of view in degrees. Small values look flat, large values exaggerate depth.",
                        d["fov"], 1, 160, "", False, False))
        p.append(number("face_opacity", "Face opacity", None, 100, 0, 100, "%", False, False))
        p.append(number("shading", "Shading", "How much faces darken as they turn away from you.",
                        35, 0, 100, "%", False, False))
    spin_from = "the start of the transition" if two else "the start of the clip's source (a trimmed clip has already turned)"
    for axis in "yxz":
        p.append(number(f"spin_{axis}", f"Spin {axis.upper()} (per second)",
                        "Keeps the box turning at this many degrees per second without any keyframes. "
                        f"Added to Rotate {axis.upper()}; counted from {spin_from}.",
                        0, -3600, 3600, "°/s", False, False, wide=True))
    p.append(param("bool", "show_inside", "Show inside of faces",
                   "Draw the far faces from the inside where you can see through an empty, "
                   "partly filled or see-through face.", default=1))
    p.append(plist("quality", "Quality", 1, [(0, "Draft (fastest)"), (1, "Good"), (2, "Best")]))

    # ---- keyframable rows -----------------------------------------------------
    p.append(number("rot_y", "Rotate Y (turn)",
                    "Degrees. Positive turns the front to the right, so the left face comes round; "
                    "negative brings the right face round. 360 is one full turn.",
                    d["rot_y"], -3600, 3600, "°", True, two, first=True, wide=True))
    p.append(number("rot_x", "Rotate X (tip)",
                    "Degrees. Positive tips the top towards you, so the top face comes round; "
                    "negative brings the bottom face round.",
                    d["rot_x"], -3600, 3600, "°", True, two, wide=True))
    p.append(number("rot_z", "Rotate Z (roll)", "Degrees, clockwise.", 0, -3600, 3600, "°", True, two, wide=True))
    p.append(number("pos_x", "Position X", "Percent of the frame width; positive is right.",
                    0, -500, 500, "%", True, two, wide=True))
    p.append(number("pos_y", "Position Y", "Percent of the frame height; positive is down.",
                    0, -500, 500, "%", True, two, wide=True))
    p.append(number("pos_z", "Distance (Z)",
                    "Percent of the frame height; positive moves the box away from you, negative towards you.",
                    0, -500, 2000, "%", True, two, wide=True))
    p.append(number("scale", "Size",
                    "At 100 the front face, when not rotated, exactly matches the frame "
                    "(for the cube shape: a square as tall as the frame).",
                    d["scale"], 1, 1000, "%", True, two, wide=True))
    if not two:
        p.append(number("depth", "Depth", "Depth of the box. 0 makes a flat card with a front and a back.",
                        d["depth"], 0, 400, "%", True, False, wide=True))
        p.append(number("fov", "Perspective",
                        "Field of view in degrees. Small values look flat, large values exaggerate depth.",
                        d["fov"], 1, 160, "", True, False))
        p.append(number("face_opacity", "Face opacity", None, 100, 0, 100, "%", True, False))
        p.append(number("shading", "Shading", "How much faces darken as they turn away from you.",
                        35, 0, 100, "%", True, False))
    return "\n".join(p)


EFFECT_DEFAULTS = dict(rot_y=0, rot_x=0, scale=50, depth=100, fov=40, shape=0, keep=0, background=0, easing=0,
                       face_front=1, face_right=1, face_back=1, face_left=1, face_top=1, face_bottom=1)
TRANS_DEFAULTS = dict(rot_y=0, rot_x=0, scale=100, depth=100, fov=15, shape=1, keep=100, background=1, easing=1,
                      face_front=1, face_right=2, face_back=2, face_left=2, face_top=2, face_bottom=2)

EFFECT_DESC = ("Puts clips on the six faces of a 3D box that you can spin, tip, roll, move and resize with keyframes. "
               "Each face shows this clip, a file of your choice, or one cell of this clip read as a 3 x 2 grid. "
               "The area around the box is transparent unless you pick a background.")
TRANS_DESC = ("Turns from one clip to the next on the faces of a 3D box. The move is two keyframes on the rotation, "
              "which you can change. Set the length first: Kdenlive does not move the end keyframe when a transition "
              "is made longer or shorter afterwards - go to its last frame and set the angle again.")


def effect_xml():
    return f"""<?xml version="1.0"?>
<!DOCTYPE kpartgui>
<effect xmlns="https://www.kdenlive.org" LC_NUMERIC="C" tag="frei0r.cube3d" id="frei0r.cube3d">
    <name>3D Cube</name>
    <description>{escape(EFFECT_DESC)}</description>
    <author>{escape(AUTHOR)}</author>
{parameters("effect", EFFECT_DEFAULTS)}
</effect>
"""


def transition_xml(asset_id, name, desc, overrides, as_transition):
    d = dict(TRANS_DEFAULTS, **overrides)
    kind = ' type="videotransition"' if as_transition else ""
    return f"""<?xml version="1.0"?>
<!DOCTYPE kpartgui>
<transition xmlns="https://www.kdenlive.org" LC_NUMERIC="C" tag="frei0r.cube3d_transition" id="{asset_id}"{kind}>
    <name>{escape(name)}</name>
    <description>{escape(desc)}</description>
    <author>{escape(AUTHOR)}</author>
{parameters("transition", d)}
</transition>
"""


def gridcell_xml():
    cells = []
    for i in range(N_CELLS):
        col, row = i % 3, i // 3
        where = ("top", "bottom")[row] + " " + ("left", "middle", "right")[col]
        cells.append((f"{col * 100 / 3:.4f}% {row * 50}% {100 / 3:.4f}% 50% 100%", f"Cell {i + 1} ({where})"))
    body = "        <paramlistdisplay>" + ",".join(t for _, t in cells) + "</paramlistdisplay>"
    cell = param("list", "rect", "Grid cell",
                 "Squeezes the clip into this cell of a 3 x 2 grid. Put up to six clips on separate tracks of a "
                 "sequence, give each a different cell, then use that sequence as a clip and let the 3D Cube "
                 "effect read its faces from the grid cells.",
                 body, default=cells[0][0], paramlist=";".join(v for v, _ in cells))
    return f"""<?xml version="1.0"?>
<!DOCTYPE kpartgui>
<effect xmlns="https://www.kdenlive.org" LC_NUMERIC="C" tag="qtblend" id="cube3d_gridcell">
    <name>3D Cube: grid cell</name>
    <description>Helper for the 3D Cube effect: places a clip in one cell of a 3 x 2 grid so that several timeline clips can be fed to the cube through one sequence.</description>
    <author>{escape(AUTHOR)}</author>
{cell}
    <parameter type="fixed" name="distort" default="1"/>
    <parameter type="fixed" name="compositing" default="0"/>
    <parameter type="fixed" name="rotation" default="0"/>
</effect>
"""


B_FIRST = dict(face_front=2, face_right=1, face_back=1, face_left=1, face_top=1, face_bottom=1)
COMP_DESC = ("Composition between two tracks: " + TRANS_DESC[0].lower() + TRANS_DESC[1:]
             + " Clip A is the lower track, clip B the track this composition sits on.")
TRANSITIONS = [
    # id, name, description, overrides, listed as same-track transition (True) or as composition (False)
    ("frei0r.cube3d_transition", "3D Cube: turn left", TRANS_DESC, dict(rot_y=(0, -90)), True),
    ("cube3d_turn_right", "3D Cube: turn right", TRANS_DESC, dict(rot_y=(0, 90)), True),
    ("cube3d_tip_up", "3D Cube: tip up", TRANS_DESC, dict(rot_x=(0, -90), shape=2), True),
    ("cube3d_tip_down", "3D Cube: tip down", TRANS_DESC, dict(rot_x=(0, 90), shape=2), True),
    ("cube3d_flip", "3D Cube: flip card", TRANS_DESC, dict(rot_y=(0, -180), depth=0, fov=30, easing=2), True),
    ("cube3d_composition", "3D Cube", COMP_DESC + " Starts on the lower clip - for a composition at the start "
     "of the upper clip.", dict(rot_y=(0, -90)), False),
    ("cube3d_composition_out", "3D Cube (upper clip first)", COMP_DESC + " Starts on the upper clip and turns "
     "to the lower one - for a composition at the end of the upper clip.", dict(rot_y=(0, -90), **B_FIRST), False),
]


def main():
    out = Path(sys.argv[1])
    (out / "effects").mkdir(parents=True, exist_ok=True)
    (out / "transitions").mkdir(parents=True, exist_ok=True)
    (out / "effects" / "frei0r_cube3d.xml").write_text(effect_xml(), encoding="utf-8")
    (out / "effects" / "cube3d_gridcell.xml").write_text(gridcell_xml(), encoding="utf-8")
    for asset_id, name, desc, overrides, as_transition in TRANSITIONS:
        fname = asset_id.replace(".", "_") + ".xml"
        (out / "transitions" / fname).write_text(
            transition_xml(asset_id, name, desc, overrides, as_transition), encoding="utf-8")
    print("wrote", ", ".join(sorted(str(p.relative_to(out)) for p in out.rglob("*.xml"))))


if __name__ == "__main__":
    main()
