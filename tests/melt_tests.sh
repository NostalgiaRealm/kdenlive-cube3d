#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# End-to-end checks: renders through the MLT that ships with Kdenlive, with the
# freshly built plugins, and inspects the pictures (tests/check_melt.py).
#
#   tests/melt_tests.sh            uses the Kdenlive Flatpak
#   MELT="melt" FFMPEG="ffmpeg" tests/melt_tests.sh     any other MLT 7
#
# Nothing is installed and no setting is changed: the plugin directory is passed
# to this one process through FREI0R_PATH. Everything is written to
# build/test-media and build/test-out/melt, which must be visible to melt
# (inside a Flatpak that means: somewhere under your home directory).
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
build="$here/build"
media="$build/test-media"
out="$build/test-out/melt"

# Kdenlive makes MLT read numbers with a decimal point whatever the desktop's
# language; plain melt would follow LC_NUMERIC and, with a decimal-comma locale,
# read "33.3333" as 33. So the tests pin the numeric locale the way Kdenlive does.
if [ -z "${MELT:-}" ]; then
    run_melt() {
        flatpak run --env=LC_NUMERIC=C "--env=FREI0R_PATH=/app/lib/frei0r-1:$build" \
            --command=melt org.kde.kdenlive "$@"
    }
    FFMPEG=${FFMPEG:-"flatpak run --command=ffmpeg org.kde.kdenlive"}
else
    export LC_NUMERIC=C
    export FREI0R_PATH="${FREI0R_PATH:-/usr/lib64/frei0r-1:/usr/lib/frei0r-1}:$build"
    # shellcheck disable=SC2086
    run_melt() { $MELT "$@"; }
    FFMPEG=${FFMPEG:-ffmpeg}
fi

[ -f "$build/cube3d.so" ] && [ -f "$build/cube3d_transition.so" ] || {
    echo "build the plugins first: make" >&2
    exit 2
}

# melt started through "flatpak run" must not inherit a current directory it cannot see
cd "$here"

if [ ! -f "$media/V.mp4" ] || [ ! -f "$media/square.png" ]; then
    echo "== creating test clips"
    python3 "$here/tests/make_test_media.py" "$media"
    for n in A B C D E; do
        $FFMPEG -hide_banner -loglevel error -y -framerate 25 -i "$media/frames/$n/%03d.png" \
            -c:v libx264 -preset ultrafast -crf 16 -pix_fmt yuv420p "$media/$n.mp4"
    done
    $FFMPEG -hide_banner -loglevel error -y -framerate 30 -i "$media/frames/V/%03d.png" \
        -c:v libx264 -preset ultrafast -crf 16 -pix_fmt yuv420p "$media/V.mp4"
    rm -rf "$media/frames"
fi

rm -rf "$out"
mkdir -p "$out"

# some arguments below are built as plain strings and split on spaces, so the
# clips are addressed through a link whose path has none
medialink="$HOME/.cube3d-test-media"
ln -sfn "$media" "$medialink"
trap 'rm -f "$medialink"' EXIT

# render <name> <profile> <consumer options...> -- <melt arguments...>
render() {
    name=$1
    profile=$2
    shift 2
    copts=""
    while [ "$1" != "--" ]; do
        copts="$copts $1"
        shift
    done
    shift
    mkdir -p "$out/$name"
    # shellcheck disable=SC2086
    if ! run_melt -profile "$profile" "$@" \
        -consumer avformat:"$out/$name/%03d.png" vcodec=png $copts >"$out/$name.log" 2>&1; then
        echo "FAIL: melt exited with an error for '$name' (see $out/$name.log)"
        echo "$name" >>"$out/crashed"
    fi
}

# The file parameters carry MLT property names that survive saving a project
# (see src/cube3d.c): file 1..6 of the effect, file 1..5 of the transition.
F1=resource F2=producer.resource F3=filename F4=av.file F5=av.filename F6=filter.resource
T3=luma T4=luma.resource T5=composite.luma
files="$F1=$medialink/B.mp4 $F2=$medialink/C.mp4 $F3=$medialink/D.mp4 $F4=$medialink/E.mp4 $F5=$medialink/V.mp4 $F6=$medialink/alpha.png"

echo "== 1 effect: transparency of the output (RGBA)"
render alpha atsc_720p_25 pix_fmt=rgba mlt_image_format=rgba real_time=-1 -- \
    "$media/A.mp4" out=4 -attach-cut frei0r.cube3d rot_y=30 rot_x=25

echo "== 2 effect: composited over a lower track"
render over atsc_720p_25 real_time=-1 -- \
    color:0x2040c0ff out=4 -track "$media/A.mp4" out=4 -attach-cut frei0r.cube3d rot_y=30 rot_x=25 \
    -transition qtblend a_track=0 b_track=1

echo "== 3 effect: six different pictures, one full turn, rendered with 1 and with 8 threads"
# shellcheck disable=SC2086
for rt in 1 8; do
    render turn_rt$rt atsc_720p_25 real_time=-$rt -- \
        "$media/A.mp4" out=59 -attach-cut frei0r.cube3d rot_y="0=0;59=360" rot_x="0=25;59=-25" scale=60 \
        face_front=1 face_right=3 face_back=4 face_left=5 face_top=6 face_bottom=7 $files
done

echo "== 4 effect: same scene at full and at quarter resolution (preview scaling)"
# shellcheck disable=SC2086
render res_full atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=2 -attach-cut frei0r.cube3d rot_y=35 rot_x=20 pos_x=12 pos_y=-8 pos_z=30 scale=70 \
    face_right=3 face_top=8 face_front=5 $files
render res_small atsc_720p_25 real_time=-1 width=640 height=360 -- \
    "$media/A.mp4" out=2 -attach-cut frei0r.cube3d rot_y=35 rot_x=20 pos_x=12 pos_y=-8 pos_z=30 scale=70 \
    face_right=3 face_top=8 face_front=5 $files

echo "== 4b effect: every face shows what was assigned to it"
# six plain colours as "files" (MLT accepts color: as a file name); the box is
# snapped to six poses, one per frame, each bringing another face to the front
colours="$F1=color:0xff0000ff $F2=color:0x00ff00ff $F3=color:0x0000ffff $F4=color:0xffff00ff $F5=color:0xff00ffff $F6=color:0x00ffffff"
poses_y="0|=0;1|=-90;2|=180;3|=90;4|=0;5|=0"
poses_x="0|=0;1|=0;2|=0;3|=0;4|=90;5|=-90"
# shellcheck disable=SC2086
render ident atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=5 -attach-cut frei0r.cube3d rot_y="$poses_y" rot_x="$poses_x" shading=0 \
    background=1 bg_color=0x204060ff \
    face_front=3 face_right=4 face_back=5 face_left=6 face_top=7 face_bottom=8 $colours

echo "== 5 effect: faces taken from the cells of a 3 x 2 grid"
# the grid itself is built the way the '3D Cube: grid cell' helper effect does it:
# the Transform filter (qtblend) with a rectangle given in percent, distortion on
render grid_source atsc_720p_25 real_time=-1 -- \
    color:black out=2 \
    -track "$media/A.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=0% 0% 33.3333% 50% 100%" \
    -track "$media/B.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=33.3333% 0% 33.3333% 50% 100%" \
    -track "$media/C.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=66.6667% 0% 33.3333% 50% 100%" \
    -track "$media/D.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=0% 50% 33.3333% 50% 100%" \
    -track "$media/E.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=33.3333% 50% 33.3333% 50% 100%" \
    -track "$media/V.mp4" out=2 -attach-cut qtblend distort=1 compositing=0 "rect=66.6667% 50% 33.3333% 50% 100%" \
    -transition qtblend a_track=0 b_track=1 -transition qtblend a_track=0 b_track=2 -transition qtblend a_track=0 b_track=3 -transition qtblend a_track=0 b_track=4 -transition qtblend a_track=0 b_track=5 -transition qtblend a_track=0 b_track=6
render grid_cube atsc_720p_25 real_time=-1 -- \
    "$out/grid_source/001.png" out=2 -attach-cut frei0r.cube3d rot_y=-35 rot_x=25 scale=60 \
    face_front=9 face_right=10 face_back=11 face_left=12 face_top=13 face_bottom=14
# and numerically, with a grid of plain colours
render grid_colours atsc_720p_25 real_time=-1 -- \
    color:black out=2 \
    -track color:0xff0000ff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=0% 0% 33.3333% 50% 100%" \
    -track color:0x00ff00ff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=33.3333% 0% 33.3333% 50% 100%" \
    -track color:0x0000ffff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=66.6667% 0% 33.3333% 50% 100%" \
    -track color:0xffff00ff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=0% 50% 33.3333% 50% 100%" \
    -track color:0xff00ffff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=33.3333% 50% 33.3333% 50% 100%" \
    -track color:0x00ffffff out=2 -attach-cut qtblend distort=1 compositing=0 "rect=66.6667% 50% 33.3333% 50% 100%" \
    -transition qtblend a_track=0 b_track=1 -transition qtblend a_track=0 b_track=2 -transition qtblend a_track=0 b_track=3 -transition qtblend a_track=0 b_track=4 -transition qtblend a_track=0 b_track=5 -transition qtblend a_track=0 b_track=6
render grid_ident atsc_720p_25 real_time=-1 -- \
    "$out/grid_colours/001.png" out=5 -attach-cut frei0r.cube3d rot_y="$poses_y" rot_x="$poses_x" shading=0 \
    background=1 bg_color=0x204060ff \
    face_front=9 face_right=10 face_back=11 face_left=12 face_top=13 face_bottom=14

echo "== 6 transition between two tracks (composition): default move"
render trans atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=24 rot_y="0i=0;24=-90"

echo "== 7 transition with the tracks given in reverse order, as Kdenlive does for some same-track mixes"
render trans_rev atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=1 b_track=0 in=0 out=24 rot_y="0i=0;24=-90"

echo "== 8 transition presets: tip up, tip down, turn right, flip card (rendered with 4 threads)"
render tip_up atsc_720p_25 real_time=-4 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=24 shape=2 rot_x="0i=0;24=-90"
render tip_down atsc_720p_25 real_time=-4 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=24 shape=2 rot_x="0i=0;24=90"
render turn_right atsc_720p_25 real_time=-4 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=24 rot_y="0i=0;24=90"
render flip atsc_720p_25 real_time=-4 -- \
    "$media/A.mp4" out=24 -track "$media/B.mp4" out=24 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=24 depth=0 fov=30 rot_y="0i=0;24=-180"

echo "== 9 a face file that does not exist, and one that is not a media file"
render missing atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=2 -attach-cut frei0r.cube3d rot_y=-35 rot_x=25 \
    face_right=3 face_top=4 $F1=/nonexistent/clip.mp4 $F2="$here/Makefile"

echo "== 10 constant spin without keyframes, against the same angle set by hand"
render spin atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=24 -attach-cut frei0r.cube3d spin_y=90 rot_x=20
# frame 20 of a 25 fps clip is 0.8 s: 72 degrees
render spin_ref atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" in=20 out=20 -attach-cut frei0r.cube3d rot_y=72 rot_x=20

echo "== 11 stills keep their proportions (MLT's fallback format is 4:3)"
render still_wide atsc_720p_25 real_time=-1 -- \
    color:black out=1 -attach-cut frei0r.cube3d shape=1 scale=100 face_front=3 $F1="$media/wide.png"
render still_wide_ref atsc_720p_25 real_time=-1 -- "$media/wide.png" out=1
render still_square atsc_720p_25 real_time=-1 -- \
    color:black out=1 -attach-cut frei0r.cube3d shape=0 scale=100 shading=0 background=1 bg_color=0x000000ff \
    face_front=3 $F1="$media/square.png"

echo "== 12 a video on a face plays at its own rate from its first frame, and loops"
render facetime atsc_720p_25 real_time=-1 -- \
    color:black out=104 -attach-cut frei0r.cube3d shape=1 scale=100 face_front=3 $F1="$media/B.mp4"
render facetime_ref atsc_720p_25 real_time=-1 -- "$media/B.mp4" out=10

echo "== 13 file faces survive what Kdenlive does when it saves or renders a project"
# the XML writer, given the project folder as root, stores paths below it as relative ones
mkdir -p "$out/saved"
# shellcheck disable=SC2086
run_melt -profile atsc_720p_25 "$media/A.mp4" out=5 \
    -attach-cut frei0r.cube3d rot_y="$poses_y" rot_x="$poses_x" shading=0 \
    face_front=3 face_right=4 face_back=5 face_left=6 face_top=7 face_bottom=8 $files \
    -consumer xml:"$out/saved/effect.mlt" root="$medialink" >"$out/saved/effect.log" 2>&1
# shellcheck disable=SC2086
run_melt -profile atsc_720p_25 "$media/A.mp4" out=4 -track "$media/A.mp4" out=4 \
    -transition frei0r.cube3d_transition a_track=0 b_track=1 in=0 out=4 shading=0 easing=0 \
    rot_y="0|=0;1|=-90;2|=180;3|=90;4|=0" rot_x="0|=0;4|=90" shape=0 scale=60 keep_in_frame=0 \
    face_front=3 face_right=4 face_back=5 face_left=6 face_top=7 \
    $F1="$media/B.mp4" $F2="$media/C.mp4" $T3="$media/D.mp4" $T4="$media/E.mp4" $T5="$media/V.mp4" \
    -consumer xml:"$out/saved/transition.mlt" root="$media" >"$out/saved/transition.log" 2>&1
render saved_effect atsc_720p_25 real_time=-1 -- "$out/saved/effect.mlt"
render saved_transition atsc_720p_25 real_time=-1 -- "$out/saved/transition.mlt"

echo "== 14 a project file as a face is refused (it used to crash MLT)"
render projectface atsc_720p_25 real_time=-1 -- \
    "$media/A.mp4" out=9 -attach-cut frei0r.cube3d rot_y=-35 rot_x=25 face_right=3 face_top=4 \
    $F1="$out/saved/effect.mlt" $F2="$out/saved/transition.mlt"

echo "== 15 a zone of a project-bin clip on a face (tools/cube3d-faces)"
mkdir -p "$out/zone"
cat >"$out/zone/proj.kdenlive" <<XML
<?xml version="1.0"?><mlt LC_NUMERIC="C" root="$media"><profile frame_rate_num="25" frame_rate_den="1"/>
<chain id="c1"><property name="resource">B.mp4</property><property name="mlt_service">avformat-novalidate</property><property name="kdenlive:id">2</property><property name="kdenlive:clipname">Clip B</property><property name="kdenlive:clipzones">[{"in":50,"out":59,"name":"Zone 1"}]</property></chain></mlt>
XML
python3 "$here/tools/cube3d-faces" "$out/zone/proj.kdenlive" "$out/zone/faces" >/dev/null
render zoneface atsc_720p_25 real_time=-1 -- \
    color:black out=23 -attach-cut frei0r.cube3d shape=1 scale=100 face_front=3 "$F1=$out/zone/faces/Clip B - Zone 1.c3dface"
render zoneface_ref atsc_720p_25 real_time=-1 -- "$media/B.mp4" out=59

echo "== checking pictures"
python3 "$here/tests/check_melt.py" "$out"
