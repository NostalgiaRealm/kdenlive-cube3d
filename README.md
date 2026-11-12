# 3D Cube for Kdenlive

A 3D box with a clip on every face, for Kdenlive:

- **Effect "3D Cube"** – put it on a clip and the clip becomes a cube you can turn, tip, roll, move
  in three dimensions and resize, all with keyframes. Every face can show the clip itself, another
  video or image file, or one cell of a 3 × 2 grid. The area around the cube is transparent.
- **Transitions "3D Cube: turn left / turn right / tip up / tip down / flip card"** – same-track
  transitions from one clip to the next on the faces of a box.
- **Compositions "3D Cube" and "3D Cube (upper clip first)"** – the same between two tracks.

It is built as two [frei0r](https://frei0r.dyne.org/) plugins (`cube3d`, a filter, and
`cube3d_transition`, a two-input mixer) plus Kdenlive XML descriptions. Written against
Kdenlive 26.08.3 / MLT 7.43 (Flathub Flatpak).

## Build and install

Needs a C compiler, make and python3; no MLT or frei0r development packages.

```sh
make                          # build/cube3d.so, build/cube3d_transition.so, build/kdenlive/*.xml
./install.sh --flatpak-env    # Kdenlive from Flathub
./install.sh                  # Kdenlive from your distribution's packages
```

Everything goes into your home directory (see the top of `install.sh` for the exact places);
`./uninstall.sh [--flatpak-env]` removes it again. Restart Kdenlive afterwards.

The Flatpak needs `--flatpak-env` because it starts with `FREI0R_PATH=/app/lib/frei0r-1`, which
hides the per-user plugin directory. The switch adds `~/.frei0r-1/lib` to that variable with a
per-user Flatpak override; it changes nothing else about the sandbox. Undo it with
`./uninstall.sh --flatpak-env`, not with `flatpak override --unset-env`: that removes the variable
altogether and every frei0r effect Kdenlive ships disappears.

Build on an older system than the Flatpak's runtime if you can: a plugin that needs a newer C
library than the sandbox has is skipped by MLT silently. `install.sh` checks that Kdenlive's own
MLT lists the plugins and says so if it does not.

## Using it in Kdenlive

**As an effect.** Effects → search "3D Cube" (it is listed under *Misc*) → drag it onto a clip.
You get a cube half as tall as the frame with the clip on all six faces. Keyframe *Rotate Y* from
0 to 360 for one full turn, or set *Spin Y (per second)* for a constant spin without keyframes.
Tracks below show through around the cube.

**Other clips on the faces.**
- *Files:* pick a video or image under *File 1 … File 6*, then choose "File n" for a face.
- *Clips and zones from the project bin:* save the project, run `cube3d-faces` (it asks for the
  project file; or `cube3d-faces project.kdenlive`), then pick one of the `.c3dface` files it wrote
  into "<project name> cube faces" next to the project as *File n*. There is one per bin clip and
  one per zone; a zone plays just that stretch, looping. Run it again after adding clips or zones.
  (Kdenlive cannot drop bin entries onto an effect's file field: its bin drags carry internal ids,
  not files. Files dragged from a file manager do work.)
- *Timeline clips:* make a sequence, put up to six clips on separate tracks, give each the helper
  effect **3D Cube: grid cell** with a different cell. Use that sequence as a clip, add 3D Cube to
  it and set the faces to "Grid cell 1 … 6". This way the faces are ordinary timeline clips –
  trimmed, with their own effects, in sync.
- To move a clip to another face, change the face's drop-down.

**As a transition.** The *3D Cube: …* entries in the transitions list are applied like any other
same-track transition. Clip A is the outgoing clip, clip B the incoming one. The presets only
differ in their starting values: the move itself is two keyframes on *Rotate Y* (or *Rotate X*).
*Set the length first.* Kdenlive does not move the end keyframe when a transition is made longer
or shorter afterwards (the same is true of its own Push/Slide/Wipe transitions): go to the last
frame and set the angle again.

**As a composition.** *3D Cube* starts on the lower track's clip and turns to the clip the
composition sits on – for the start of the upper clip. *3D Cube (upper clip first)* is the
reverse, for the end of the upper clip.

## Parameters

Rotations are in degrees, everything else in percent. In Kdenlive these are the labels; the names
in brackets are the MLT property names (for `melt` and for reading project files).

| Parameter | Meaning |
|---|---|
| Rotate Y (`rot_y`) | Turn. Positive turns the front to the right, so the *left* face comes round. |
| Rotate X (`rot_x`) | Tip. Positive tips the top towards you, so the *top* face comes round. |
| Rotate Z (`rot_z`) | Roll, clockwise. |
| Position X / Y (`pos_x`, `pos_y`) | Percent of the frame width / height; right and down are positive. |
| Distance (`pos_z`) | Percent of the frame height; positive is away from you. |
| Size (`scale`) | 100 = the unrotated front face exactly matches the frame (cube: a square as tall as the frame). |
| Depth (`depth`) | Depth of the box; 0 = a flat card with a front and a back. |
| Perspective (`fov`) | Field of view, 1–160°. Small = flat, large = dramatic. |
| Face opacity, Shading (`face_opacity`, `shading`) | Shading darkens faces as they turn away. |
| Soften turns (`easing`) | 0 off · 1 ease at every quarter turn · 2 at every half turn. Applied to the three rotations, so a plain two-keyframe turn starts and stops gently. |
| Spin X/Y/Z (`spin_x`, …) | Degrees per second, added to the rotation. |
| Shape (`shape`) | 0 cube · 1 frame-shaped, as deep as wide (for turning) · 2 frame-shaped, as deep as tall (for tipping). |
| Picture on face (`fit`) | 0 fill (crop) · 1 fit (whole picture) · 2 stretch. |
| Rotation order (`rot_order`) | 0 turntable (turn, then tip) · 1 tumble (tip, then turn). |
| Keep inside frame (`keep_in_frame`) | 100 shrinks the box just enough to stay completely visible. |
| Show inside of faces (`show_inside`) | Draw far faces from the inside where you can see through. |
| Quality (`quality`) | 0 draft · 1 good · 2 best. |
| Background (`background`, `bg_color`) | 0 transparent · 1 colour · 2 clip A · 3 clip B. |
| Faces (`face_front`, `face_right`, `face_back`, `face_left`, `face_top`, `face_bottom`) | 0 empty · 1 clip A · 2 clip B · 3–8 file 1–6 · 9–14 grid cell 1–6 of clip A · 15 `face_color`. |
| Files, effect | `resource`, `producer.resource`, `filename`, `av.file`, `av.filename`, `filter.resource` |
| Files, transition (five) | `resource`, `producer.resource`, `luma`, `luma.resource`, `composite.luma` |

In Kdenlive the rotations, position, distance and size are keyframable everywhere; depth,
perspective, opacity and shading only in the effect. (Through `melt` every number is.)

The file parameters have borrowed names because MLT, when it saves a project, shortens every path
below the project folder to a relative one and restores only a fixed list of property names when
it loads the project again. With any other name a face file next to your project would be lost the
first time the project is saved, reopened or rendered.

With `melt`:

```sh
LC_NUMERIC=C melt clip.mp4 -attach-cut frei0r.cube3d rot_y="0=0;99=360" rot_x=25 scale=62.5 \
    face_right=3 resource=other.mp4
```

(`LC_NUMERIC=C` because melt otherwise reads numbers the way your desktop language writes them;
Kdenlive always uses a decimal point.)

## Good to know

- Faces from files are video only (no sound). In the effect they run on the clip's own time: the
  file starts when the clip's *source* starts, so on a clip whose beginning was trimmed away it is
  already part-way in – and so is a *Spin*. In a transition they start with the transition. Files
  loop. Use the grid-cell route when you need trimming, effects or exact sync.
- Project files (`.kdenlive`, `.mlt`) are not accepted as face files; render them first or use the
  grid cells.
- File names containing `=` or `;` do not work: MLT reads such values as keyframes.
- Pixels are assumed to be square (true for every HD/UHD profile).
- Parameter values are plain degrees/percent rather than frei0r's customary 0–1 range. MLT passes
  them through untouched; other frei0r hosts may not.
- Each face file is one more open decoder. The plugin enlarges MLT's decoder caches accordingly
  (MLT sizes them for the number of tracks and frees decoders that are in use when they overflow).
  Set `CUBE3D_DEBUG=1` in the environment to see it do so.

## Tests

```sh
make check               # renderer: pixel-exact faces, seams, alpha, filtering, robustness
tests/melt_tests.sh      # through Kdenlive's own melt: effect, file and grid faces, stills, timing,
                         # saved projects, transitions, 1 vs 8 render threads, preview scaling
tests/kdenlive_probe.sh  # drives a headless Kdenlive: add clip, add effect, autosave, render
```

None of them installs anything or changes a setting; the probe runs Kdenlive with a throw-away
home directory and a private D-Bus. What they cannot show is the parameter panel itself, and
adding a transition from the Kdenlive UI (Kdenlive has no scripting call for that).

## Source

| | |
|---|---|
| `src/c3d_render.[ch]` | the renderer: perspective-correct, mip-mapped, 32-sample antialiasing, multithreaded; no dependencies |
| `src/c3d_media.[ch]` | pictures from files through the hosting MLT (looked up at run time, not linked) |
| `src/cube3d.c` | the frei0r plugin, compiled once as filter and once as mixer |
| `tools/gen_kdenlive_xml.py` | writes the Kdenlive XML |
| `include/frei0r.h` | the frei0r API header, from frei0r 3.7.0 |

GPL-2.0-or-later (see `COPYING`).
