#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Drives the real Kdenlive (Flatpak), without showing a window and without
# touching your Kdenlive settings or your session's D-Bus:
#
#   1. starts Kdenlive on Qt's "offscreen" platform, with HOME and the XDG
#      directories pointing into build/probe and a private D-Bus daemon;
#   2. through Kdenlive's own scripting calls, puts a clip on the timeline and
#      applies the 3D Cube effect to it;
#   3. lets Kdenlive autosave, keeps that project file as build/probe/project.kdenlive
#      and renders it with the melt that ships with Kdenlive.
#
# It proves that Kdenlive reads the XML descriptions, lists the effect and the
# transitions, creates the effect with the parameters the XML declares, and
# writes a project that renders. It cannot show what the parameter panel looks
# like, and it cannot add a transition or composition: Kdenlive offers no
# scripting call for that (its transition actions are placeholders).
#
# Needs: the plugins built (make), the test clips (tests/melt_tests.sh makes
# them), and in the calling shell: flatpak, dbus-daemon, gdbus.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
build="$here/build"
media="$build/test-media"
pr="$build/probe"
APP=org.kde.kdenlive

[ -f "$media/A.mp4" ] || {
    echo "test clips missing: run tests/melt_tests.sh first" >&2
    exit 2
}

rm -rf "$pr"
mkdir -p "$pr/config" "$pr/cache" "$pr/state" "$pr/data/kdenlive" "$pr/home" "$pr/out"
cp -r "$build/kdenlive/effects" "$build/kdenlive/transitions" "$pr/data/kdenlive/"
cat >"$pr/config/kdenlive-flatpakrc" <<EOF
[misc]
autosave_time=2
crashrecovery=true
checkfirstprojectclip=false
showWelcome=false
EOF

# a socket path may be at most ~100 characters, so it cannot live in build/;
# it must be somewhere both this shell and the Flatpak sandbox can see
sock="$HOME/.cube3d-probe-$$.bus"
bus="unix:path=$sock"
dbus-daemon --session --address="$bus" --nofork --nopidfile >"$pr/bus.log" 2>&1 &
bus_pid=$!
kd_pid=""
cleanup() {
    [ -n "$kd_pid" ] && kill "$kd_pid" 2>/dev/null || true
    kill "$bus_pid" 2>/dev/null || true
    rm -f "$sock"
}
trap cleanup EXIT INT TERM

cat >"$pr/run.sh" <<EOF
#!/bin/sh
export HOME="$pr/home"
export XDG_CONFIG_HOME="$pr/config" XDG_DATA_HOME="$pr/data" XDG_CACHE_HOME="$pr/cache"
export XDG_STATE_HOME="$pr/state"
export FREI0R_PATH="/app/lib/frei0r-1:$build"
export DBUS_SESSION_BUS_ADDRESS="$bus"
export QT_QPA_PLATFORM=offscreen
export QT_LOGGING_RULES="*.debug=true;qt.*=false;kf.*=false"
cd "$pr"
exec timeout -s KILL 150 kdenlive --no-welcome >"$pr/kdenlive.log" 2>&1
EOF
chmod +x "$pr/run.sh"

cd "$here"
flatpak run --command=sh "$APP" "$pr/run.sh" &
kd_pid=$!

dcall() { DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session --timeout 30 "$@"; }
name=""
i=0
while [ $i -lt 90 ] && [ -z "$name" ]; do
    sleep 1
    i=$((i + 1))
    name=$(dcall --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.ListNames 2>/dev/null | tr "'" '\n' | grep -m1 '^org\.kde\.kdenlive' || true)
done
[ -n "$name" ] || {
    echo "FAIL  Kdenlive did not appear on the private bus (see $pr/kdenlive.log)"
    exit 1
}
echo "Kdenlive is up as $name after ${i}s"
sleep 8 # let the main window finish building

win() { dcall --dest "$name" --object-path /kdenlive/MainWindow_1 --method "$@"; }
# every effect and transition Kdenlive knows is also an action: keep the list
win org.kde.KMainWindow.actions 2>/dev/null | tr "'" '\n' | grep -v '^[,( )\[\]]*$' | sort >"$pr/actions.txt" || true
step() {
    echo "-> $*"
    win "$@" >>"$pr/dbus.log" 2>&1 || echo "   (call failed, see $pr/dbus.log)"
}

# gdbus reads arguments as GVariant text: strings go in single quotes
step org.kde.kdenlive.MainWindow.addProjectClip "'$media/A.mp4'" "'-1'"
sleep 8
# Inserting needs the clip to be loaded in the clip monitor, which happens
# some time after it was selected - and the call selects and inserts in one go.
# So ask twice: whichever call finds the clip ready puts it on the timeline.
step org.kde.kdenlive.MainWindow.addTimelineClip "'$media/A.mp4'"
sleep 3
step org.kde.kdenlive.MainWindow.addTimelineClip "'$media/A.mp4'"
sleep 3
step org.kde.KMainWindow.activateAction "'seek_start'"
sleep 1
step org.kde.KMainWindow.activateAction "'select_timeline_clip'"
sleep 1
step org.kde.kdenlive.MainWindow.addEffect "'frei0r.cube3d'"
sleep 2
sleep 7 # autosave runs 2 s after the last change

stale=$(find "$pr/data" "$pr/home" -path '*stalefiles*' -type f ! -name '*.lock' 2>/dev/null | head -1)
if [ -n "$stale" ] && [ -s "$stale" ]; then
    cp "$stale" "$pr/project.kdenlive"
    echo "autosaved project kept as $pr/project.kdenlive ($(wc -c <"$pr/project.kdenlive") bytes)"
else
    echo "FAIL  no autosave file was written"
fi

win org.kde.kdenlive.MainWindow.exitApp >/dev/null 2>&1 || true
sleep 2
kill "$kd_pid" 2>/dev/null || true
kd_pid=""

[ -s "$pr/project.kdenlive" ] || exit 1

# Render what Kdenlive saved, with Kdenlive's melt. A project file names the
# project bin as the thing to play; Kdenlive's own renderer points that at the
# timeline sequence instead, and so does this.
python3 - "$pr/project.kdenlive" "$pr/render.mlt" <<'PY'
import sys, xml.etree.ElementTree as ET
tree = ET.parse(sys.argv[1])
root = tree.getroot()
sequence = [t for t in root.findall("tractor")
            if any(p.get("name") == "kdenlive:uuid" for p in t.findall("property"))]
root.set("producer", sequence[0].get("id"))
tree.write(sys.argv[2], encoding="utf-8", xml_declaration=True)
PY
# No LC_NUMERIC here on purpose: the file itself says which decimal separator it uses.
flatpak run --env=FREI0R_PATH="/app/lib/frei0r-1:$build" --command=melt "$APP" \
    "$pr/render.mlt" out=4 -consumer avformat:"$pr/out/%03d.png" vcodec=png real_time=-1 \
    >"$pr/melt.log" 2>&1 || echo "FAIL  melt could not render the project (see $pr/melt.log)"

python3 "$here/tests/check_kdenlive_project.py" "$pr"
